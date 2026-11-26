// Copyright 2026 the LinuxBoot Authors. All rights reserved
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package uefi

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"sort"
	"strconv"
	"strings"

	"github.com/linuxboot/fiano/pkg/compression"
)

// This file implements a read-only layout report for PI firmware images.
//
// ParseFirmwareLayout parses an image that starts with a single FFS2/FFS3
// firmware volume and returns the parse tree together with a FirmwareLayout
// index. For every node the index records a stable ancestor path, the node
// type, the source space the node lives in, the span of the complete record
// (header plus payload) and the span of the payload alone. All header
// lengths, the FV extended header location and GUID-defined DataOffset
// values are taken from the raw bytes of the image, never derived from
// re-serialized structures. Padding between siblings and the free tail of a
// volume belong to the container and are never attributed to a child node.
//
// Coordinate spaces ("source spaces")
//
// Every span is expressed in exactly one source space:
//
//   - The flash space (ID FlashSpace) covers the input image. Addresses are
//     absolute: they include the flashBase passed to ParseFirmwareLayout, so
//     a node at image offset X has span [flashBase+X, flashBase+X+len).
//   - Each processing-required GUID-defined section that this package can
//     decode (LZMA, via the pure-Go decoder in pkg/compression; no external
//     tools are ever invoked) produces one decoded space. Decoded spaces are
//     zero-based: the first byte of the decoded stream is offset 0. Nodes in
//     a decoded space have no flash addresses; only the encapsulating
//     section itself keeps a physical span. LayoutSpaceInfo records which
//     node produced the space and the span of the compressed data in the
//     parent space.
//
// GUID-defined sections that do not require processing keep their content in
// the parent space starting at DataOffset. GUID-defined sections with an
// unsupported processing GUID are kept as opaque nodes; their internals are
// not guessed.
//
// Erase polarity is interpreted per firmware volume from its own header
// attributes, so nested volumes with opposite polarity parse correctly. The
// parser never touches the package-level Attributes, ReadOnly,
// DisableDecompression or SuppressErasePolarityError options, and one parse
// never affects the next.

// fvSignature is the little-endian value of the "_FVH" signature.
const fvSignature = 0x4856465F

// guidDefinedHeaderLen is the binary size of SectionGUIDDefinedHeader.
const guidDefinedHeaderLen = 20

// LayoutSpaceID identifies a source space in a FirmwareLayout.
type LayoutSpaceID uint32

// FlashSpace is the ID of the flash source space, whose addresses include
// the flash base passed to ParseFirmwareLayout.
const FlashSpace LayoutSpaceID = 0

// LayoutSpaceKind describes what produced a source space.
type LayoutSpaceKind int

// Source space kinds.
const (
	// LayoutSpaceFlash is the physical flash image, addressed absolutely
	// (flash base included).
	LayoutSpaceFlash LayoutSpaceKind = iota
	// LayoutSpaceDecoded is a zero-based space produced by decoding a
	// processing-required GUID-defined section.
	LayoutSpaceDecoded
)

// String returns a human-readable name of the space kind.
func (k LayoutSpaceKind) String() string {
	switch k {
	case LayoutSpaceFlash:
		return "flash"
	case LayoutSpaceDecoded:
		return "decoded"
	}
	return "unknown"
}

// LayoutSpan is a half-open byte range [Start, End) in a source space.
type LayoutSpan struct {
	Start uint64
	End   uint64
}

// Len returns the number of bytes covered by the span.
func (s LayoutSpan) Len() uint64 {
	return s.End - s.Start
}

// Contains reports whether off lies inside the span.
func (s LayoutSpan) Contains(off uint64) bool {
	return off >= s.Start && off < s.End
}

// Intersects reports whether the two spans share at least one byte.
func (s LayoutSpan) Intersects(o LayoutSpan) bool {
	return s.Start < o.End && o.Start < s.End
}

// LayoutSpaceInfo describes one source space of a FirmwareLayout.
type LayoutSpaceInfo struct {
	ID   LayoutSpaceID
	Kind LayoutSpaceKind
	// Base is the coordinate of the first byte of the space's stream: the
	// flash base for the flash space, 0 for decoded spaces.
	Base uint64
	// Size is the number of bytes in the space's stream.
	Size uint64

	// The fields below are only set for decoded spaces.

	// ProducerPath is the path of the GUID-defined section whose decoded
	// payload created this space.
	ProducerPath string
	// Parent is the space the compressed data was read from.
	Parent LayoutSpaceID
	// Compressed is the span of the compressed data in the parent space.
	Compressed LayoutSpan
}

// LayoutNodeKind classifies layout nodes.
type LayoutNodeKind int

// Layout node kinds.
const (
	LayoutNodeFirmwareVolume LayoutNodeKind = iota
	LayoutNodeFile
	LayoutNodeSection
)

// String returns a human-readable name of the node kind.
func (k LayoutNodeKind) String() string {
	switch k {
	case LayoutNodeFirmwareVolume:
		return "FirmwareVolume"
	case LayoutNodeFile:
		return "File"
	case LayoutNodeSection:
		return "Section"
	}
	return "unknown"
}

// LayoutNode describes one record of the firmware layout: where the complete
// record lives and where its payload lives, in one source space.
type LayoutNode struct {
	// Path is the stable ancestor path of the node, e.g.
	// "FV[0]/File[2]/Section[0]/FV[0]/File[1]". Indices are zero-based and
	// count siblings of the same kind within their parent.
	Path string
	// Kind classifies the node.
	Kind LayoutNodeKind
	// TypeName is the type of the record: the FV type name, the FFS file
	// type or the section type.
	TypeName string
	// GUID is the identifying GUID of the record if it has one: the file
	// system GUID of a volume, the file GUID, or the encapsulation GUID of
	// a GUID-defined section. It is empty otherwise.
	GUID string
	// Space is the source space Full and Payload refer to.
	Space LayoutSpaceID
	// Full is the half-open span of the complete record, header included.
	Full LayoutSpan
	// Payload is the half-open span of the record's payload. For
	// GUID-defined sections it starts at the DataOffset read from the
	// image, so extra authenticated header bytes stay in the header region.
	Payload LayoutSpan
}

// String returns a single-line, stable description of the node.
func (n LayoutNode) String() string {
	return fmt.Sprintf("%s %s space=%d full=[%#x,%#x) payload=[%#x,%#x)",
		n.Path, n.Kind, n.Space, n.Full.Start, n.Full.End, n.Payload.Start, n.Payload.End)
}

// LayoutError pinpoints where parsing a firmware layout failed.
type LayoutError struct {
	// Path is the ancestor path of the node that failed to parse.
	Path string
	// Space is the source space being parsed.
	Space LayoutSpaceID
	// Offset is the stream offset within Space at which the problem was
	// detected.
	Offset uint64
	// Msg describes the problem.
	Msg string
}

// Error implements the error interface.
func (e *LayoutError) Error() string {
	return fmt.Sprintf("firmware layout: %s: %s (space %d, stream offset %#x)",
		e.Path, e.Msg, e.Space, e.Offset)
}

// FirmwareLayout is a source-space-aware index over the nodes of a parsed
// firmware image. It is independent of the parse tree and of the input
// image: it holds only its own data, so callers may modify or discard the
// image after parsing and keep querying.
type FirmwareLayout struct {
	flashBase uint64
	spaces    []LayoutSpaceInfo
	// nodes are stored in pre-order of the parse tree, which coincides with
	// path order.
	nodes []LayoutNode
}

// FlashBase returns the flash base the layout was built with.
func (l *FirmwareLayout) FlashBase() uint64 {
	return l.flashBase
}

// Spaces returns all source spaces known to the layout, in creation order.
// The flash space is always first and has ID FlashSpace.
func (l *FirmwareLayout) Spaces() []LayoutSpaceInfo {
	out := make([]LayoutSpaceInfo, len(l.spaces))
	copy(out, l.spaces)
	return out
}

// Space returns the descriptor of one source space.
func (l *FirmwareLayout) Space(id LayoutSpaceID) (LayoutSpaceInfo, error) {
	if int(id) >= len(l.spaces) {
		return LayoutSpaceInfo{}, fmt.Errorf("firmware layout: unknown space %d", id)
	}
	return l.spaces[id], nil
}

// Nodes returns all layout nodes in stable path order.
func (l *FirmwareLayout) Nodes() []LayoutNode {
	out := make([]LayoutNode, len(l.nodes))
	copy(out, l.nodes)
	return out
}

// Query returns every node of the given source space whose complete record
// intersects the half-open interval [start, end). Ancestors and descendants
// are kept in the result; a query that only hits container padding or free
// space returns just the container. Use flash-base-inclusive addresses for
// the flash space and zero-based local offsets for decoded spaces. Results
// are ordered stably by path.
func (l *FirmwareLayout) Query(space LayoutSpaceID, start, end uint64) ([]LayoutNode, error) {
	if start > end {
		return nil, fmt.Errorf("firmware layout: invalid query interval [%#x, %#x)", start, end)
	}
	if int(space) >= len(l.spaces) {
		return nil, fmt.Errorf("firmware layout: unknown space %d", space)
	}
	q := LayoutSpan{Start: start, End: end}
	var out []LayoutNode
	for _, n := range l.nodes {
		if n.Space != space || !n.Full.Intersects(q) {
			continue
		}
		out = append(out, n)
	}
	sort.SliceStable(out, func(i, j int) bool {
		return compareLayoutPaths(out[i].Path, out[j].Path) < 0
	})
	return out, nil
}

// compareLayoutPaths orders paths segment by segment, comparing sibling
// indices numerically, so that ancestors always sort before descendants.
func compareLayoutPaths(a, b string) int {
	as, bs := strings.Split(a, "/"), strings.Split(b, "/")
	for i := 0; i < len(as) && i < len(bs); i++ {
		if c := comparePathSegment(as[i], bs[i]); c != 0 {
			return c
		}
	}
	switch {
	case len(as) < len(bs):
		return -1
	case len(as) > len(bs):
		return 1
	}
	return 0
}

// comparePathSegment compares one path segment, e.g. "File[10]" vs "File[2]".
func comparePathSegment(a, b string) int {
	an, ai := splitPathSegment(a)
	bn, bi := splitPathSegment(b)
	if an != bn {
		if an < bn {
			return -1
		}
		return 1
	}
	switch {
	case ai < bi:
		return -1
	case ai > bi:
		return 1
	}
	return 0
}

// splitPathSegment splits "File[12]" into ("File", 12).
func splitPathSegment(s string) (string, int) {
	i := strings.IndexByte(s, '[')
	if i < 0 || !strings.HasSuffix(s, "]") {
		return s, 0
	}
	n, err := strconv.Atoi(s[i+1 : len(s)-1])
	if err != nil {
		return s, 0
	}
	return s[:i], n
}

// layoutStream is one source space being parsed: its ID, the coordinate of
// the first byte of data, and the data itself.
type layoutStream struct {
	id   LayoutSpaceID
	base uint64
	data []byte
}

// layoutParser accumulates layout nodes and source spaces while walking one
// firmware image. All state is per-parse; nothing is shared between parses.
type layoutParser struct {
	flashBase uint64
	spaces    []LayoutSpaceInfo
	nodes     []LayoutNode
}

// ParseFirmwareLayout parses image, which must start with a complete
// FFS2/FFS3 firmware volume, and returns the parse tree together with an
// independent layout index. flashBase is the physical address of image[0]
// and is only used to address the flash source space.
//
// The parse is strictly read-only: image, the global package options and any
// previously returned parse trees are left untouched, and the returned
// FirmwareLayout keeps working even if image is modified afterwards.
func ParseFirmwareLayout(image []byte, flashBase uint64) (*FirmwareVolume, *FirmwareLayout, error) {
	if len(image) == 0 {
		return nil, nil, &LayoutError{Path: "FV[0]", Space: FlashSpace, Msg: "empty image"}
	}
	if flashBase > ^uint64(0)-uint64(len(image)) {
		return nil, nil, &LayoutError{
			Path:  "FV[0]",
			Space: FlashSpace,
			Msg: fmt.Sprintf("flash base %#x plus image size %#x overflows the 64-bit address space",
				flashBase, len(image)),
		}
	}
	p := &layoutParser{flashBase: flashBase}
	p.spaces = append(p.spaces, LayoutSpaceInfo{
		ID:   FlashSpace,
		Kind: LayoutSpaceFlash,
		Base: flashBase,
		Size: uint64(len(image)),
	})
	sp := &layoutStream{id: FlashSpace, base: flashBase, data: image}
	root, err := p.parseFirmwareVolume(sp, 0, uint64(len(image)), "FV[0]")
	if err != nil {
		return nil, nil, err
	}
	return root, &FirmwareLayout{
		flashBase: flashBase,
		spaces:    p.spaces,
		nodes:     p.nodes,
	}, nil
}

// errf builds a located parse error.
func (p *layoutParser) errf(path string, sp *layoutStream, off uint64, format string, args ...interface{}) error {
	return &LayoutError{
		Path:   path,
		Space:  sp.id,
		Offset: off,
		Msg:    fmt.Sprintf(format, args...),
	}
}

// addNode records one layout node.
func (p *layoutParser) addNode(n LayoutNode) {
	p.nodes = append(p.nodes, n)
}

// addDecodedSpace registers a new decoded source space produced by the
// GUID-defined section at producerPath, whose compressed payload occupies
// [compStart, compEnd) in the parent stream.
func (p *layoutParser) addDecodedSpace(producerPath string, parent *layoutStream, compStart, compEnd uint64, decoded []byte) *layoutStream {
	id := LayoutSpaceID(len(p.spaces))
	p.spaces = append(p.spaces, LayoutSpaceInfo{
		ID:           id,
		Kind:         LayoutSpaceDecoded,
		Base:         0,
		Size:         uint64(len(decoded)),
		ProducerPath: producerPath,
		Parent:       parent.id,
		Compressed:   LayoutSpan{Start: parent.base + compStart, End: parent.base + compEnd},
	})
	return &layoutStream{id: id, base: 0, data: decoded}
}

// parseFirmwareVolume parses the firmware volume at stream offset off,
// bounded by limit (the end of the containing record), and records the
// layout of the volume and everything inside it.
func (p *layoutParser) parseFirmwareVolume(sp *layoutStream, off, limit uint64, path string) (*FirmwareVolume, error) {
	data := sp.data
	if off > limit || limit > uint64(len(data)) {
		return nil, p.errf(path, sp, off, "firmware volume offset %#x with limit %#x outside stream of %#x bytes",
			off, limit, len(data))
	}
	avail := limit - off
	if avail < FirmwareVolumeMinSize {
		return nil, p.errf(path, sp, off, "firmware volume header truncated: need at least %#x bytes, have %#x",
			uint64(FirmwareVolumeMinSize), avail)
	}
	var fixedHdr FirmwareVolumeFixedHeader
	if err := binary.Read(bytes.NewReader(data[off:]), binary.LittleEndian, &fixedHdr); err != nil {
		return nil, p.errf(path, sp, off, "unable to read firmware volume header: %v", err)
	}
	if fixedHdr.Signature != fvSignature {
		return nil, p.errf(path, sp, off, "invalid firmware volume signature %#08x, want %#08x (_FVH)",
			fixedHdr.Signature, fvSignature)
	}
	length := fixedHdr.Length
	if length < FirmwareVolumeMinSize {
		return nil, p.errf(path, sp, off, "invalid firmware volume length %#x", length)
	}
	if length > avail {
		return nil, p.errf(path, sp, off, "firmware volume length %#x extends beyond containing region of %#x bytes",
			length, avail)
	}
	headerLen := uint64(fixedHdr.HeaderLen)
	if headerLen < FirmwareVolumeMinSize || headerLen > length {
		return nil, p.errf(path, sp, off, "invalid firmware volume header length %#x (volume length %#x)",
			headerLen, length)
	}

	// Read the block map; the terminating zero entry must lie within the
	// declared header.
	var blocks []Block
	for boff := off + FirmwareVolumeFixedHeaderSize; ; boff += 8 {
		if boff+8 > off+headerLen {
			return nil, p.errf(path, sp, boff, "firmware volume block map is not terminated within the header")
		}
		var blk Block
		if err := binary.Read(bytes.NewReader(data[boff:]), binary.LittleEndian, &blk); err != nil {
			return nil, p.errf(path, sp, boff, "unable to read block map entry: %v", err)
		}
		if blk.Count == 0 && blk.Size == 0 {
			break
		}
		blocks = append(blocks, blk)
	}

	// The data offset comes from the raw header fields: HeaderLen, and the
	// extended header location if one is present.
	var extHdr FirmwareVolumeExtHeader
	hasExt := false
	dataOffset := headerLen
	if fixedHdr.ExtHeaderOffset != 0 {
		extOff := uint64(fixedHdr.ExtHeaderOffset)
		if extOff > length || length-extOff < FirmwareVolumeExtHeaderMinSize {
			return nil, p.errf(path, sp, off+extOff,
				"firmware volume extended header offset %#x outside volume of %#x bytes", extOff, length)
		}
		if err := binary.Read(bytes.NewReader(data[off+extOff:]), binary.LittleEndian, &extHdr); err != nil {
			return nil, p.errf(path, sp, off+extOff, "unable to read firmware volume extended header: %v", err)
		}
		extSize := uint64(extHdr.ExtHeaderSize)
		if extSize < FirmwareVolumeExtHeaderMinSize || extOff+extSize > length {
			return nil, p.errf(path, sp, off+extOff, "invalid firmware volume extended header size %#x", extSize)
		}
		hasExt = true
		dataOffset = extOff + extSize
	}
	// Make sure DataOffset is 8 byte aligned at least.
	dataOffset = Align8(dataOffset)
	if dataOffset > length {
		return nil, p.errf(path, sp, off, "firmware volume data offset %#x beyond volume length %#x",
			dataOffset, length)
	}

	fv := &FirmwareVolume{
		FirmwareVolumeFixedHeader: fixedHdr,
		Blocks:                    blocks,
		DataOffset:                dataOffset,
		FVType:                    FVGUIDs[fixedHdr.FileSystemGUID],
		FVOffset:                  off,
	}
	if hasExt {
		fv.FirmwareVolumeExtHeader = extHdr
	}
	fvBuf := make([]byte, length)
	copy(fvBuf, data[off:off+length])
	fv.buf = fvBuf

	typeName := fv.FVType
	if typeName == "" {
		typeName = "FIRMWARE_VOLUME"
	}
	p.addNode(LayoutNode{
		Path:     path,
		Kind:     LayoutNodeFirmwareVolume,
		TypeName: typeName,
		GUID:     fixedHdr.FileSystemGUID.String(),
		Space:    sp.id,
		Full:     LayoutSpan{Start: sp.base + off, End: sp.base + off + length},
		Payload:  LayoutSpan{Start: sp.base + off + dataOffset, End: sp.base + off + length},
	})

	if !supportedFVs[fixedHdr.FileSystemGUID] {
		// Unsupported volume types stay opaque, as in NewFirmwareVolume.
		return fv, nil
	}

	// Every volume interprets free space and file state with its own erase
	// polarity, taken from its own header attributes.
	polarity := fv.GetErasePolarity()
	fvEnd := off + length
	for i, rel := 0, dataOffset; rel < length; i++ {
		foff := off + rel
		remaining := fvEnd - foff
		childPath := path + "/File[" + strconv.Itoa(i) + "]"
		peek := data[foff : foff+min(remaining, uint64(FileHeaderMinLength))]
		if IsErased(peek, polarity) {
			// Free space (including a short tail) belongs to the volume.
			fv.FreeSpace = remaining
			break
		}
		if remaining < FileHeaderMinLength {
			return nil, p.errf(childPath, sp, foff,
				"file header truncated: %#x bytes left before end of firmware volume", remaining)
		}
		file, err := p.parseFile(sp, foff, fvEnd, polarity, childPath)
		if err != nil {
			return nil, err
		}
		fv.Files = append(fv.Files, file)
		// Files are 8-byte aligned relative to the start of the volume.
		rel = Align8(rel + file.Header.ExtendedSize)
	}
	return fv, nil
}

// parseFile parses the FFS file at stream offset off, bounded by fvEnd (the
// end of the containing firmware volume).
func (p *layoutParser) parseFile(sp *layoutStream, off, fvEnd uint64, polarity byte, path string) (*File, error) {
	data := sp.data
	var fh FileHeader
	if err := binary.Read(bytes.NewReader(data[off:]), binary.LittleEndian, &fh); err != nil {
		return nil, p.errf(path, sp, off, "unable to read file header: %v", err)
	}
	headerLen := uint64(FileHeaderMinLength)
	var extSize uint64
	if fh.Size == [3]uint8{0xFF, 0xFF, 0xFF} {
		// Extended header: the real size is in the 8-byte ExtendedSize
		// field of the raw header.
		if fvEnd-off < FileHeaderExtMinLength {
			return nil, p.errf(path, sp, off, "extended file header truncated: need %#x bytes, have %#x",
				uint64(FileHeaderExtMinLength), fvEnd-off)
		}
		extSize = binary.LittleEndian.Uint64(data[off+FileHeaderMinLength:])
		if extSize == 0xFFFFFFFFFFFFFFFF {
			return nil, p.errf(path, sp, off, "file header has an erased extended size but a non-erased header")
		}
		headerLen = FileHeaderExtMinLength
	} else {
		extSize = Read3Size(fh.Size)
	}
	if extSize < headerLen {
		return nil, p.errf(path, sp, off, "invalid file size %#x smaller than header length %#x",
			extSize, headerLen)
	}
	if extSize > fvEnd-off {
		return nil, p.errf(path, sp, off,
			"file size %#x extends beyond containing firmware volume (%#x bytes left)", extSize, fvEnd-off)
	}

	f := &File{
		Type:       fh.Type.String(),
		DataOffset: headerLen,
	}
	f.Header.FileHeader = fh
	f.Header.ExtendedSize = extSize
	fbuf := make([]byte, extSize)
	copy(fbuf, data[off:off+extSize])
	f.buf = fbuf

	p.addNode(LayoutNode{
		Path:     path,
		Kind:     LayoutNodeFile,
		TypeName: f.Type,
		GUID:     fh.GUID.String(),
		Space:    sp.id,
		Full:     LayoutSpan{Start: sp.base + off, End: sp.base + off + extSize},
		Payload:  LayoutSpan{Start: sp.base + off + headerLen, End: sp.base + off + extSize},
	})

	if !SupportedFiles[fh.Type] {
		// File types we do not parse stay opaque, as in NewFile.
		return f, nil
	}
	secs, err := p.parseSections(sp, off+headerLen, off+extSize, polarity, path)
	if err != nil {
		return nil, err
	}
	f.Sections = secs
	return f, nil
}

// parseSections parses the sequence of sections in [start, end) and records
// their layout under parentPath. Section alignment is relative to start,
// matching the buffer-relative alignment of the encapsulated stream.
func (p *layoutParser) parseSections(sp *layoutStream, start, end uint64, polarity byte, parentPath string) ([]*Section, error) {
	var secs []*Section
	for i, rel := 0, uint64(0); start+rel < end; i++ {
		off := start + rel
		childPath := parentPath + "/Section[" + strconv.Itoa(i) + "]"
		if rem := end - off; rem < SectionMinLength {
			if IsErased(sp.data[off:end], polarity) {
				// Trailing alignment padding belongs to the container.
				break
			}
			return nil, p.errf(childPath, sp, off,
				"section header truncated: %#x bytes left in container", rem)
		}
		s, err := p.parseSection(sp, off, end, i, polarity, childPath)
		if err != nil {
			return nil, err
		}
		secs = append(secs, s)
		// Sections are 4-byte aligned relative to the start of the
		// containing payload.
		rel = Align4(rel + uint64(s.Header.ExtendedSize))
	}
	return secs, nil
}

// parseSection parses the section at stream offset off, bounded by limit
// (the end of the containing payload).
func (p *layoutParser) parseSection(sp *layoutStream, off, limit uint64, order int, polarity byte, path string) (*Section, error) {
	data := sp.data
	rem := limit - off // >= SectionMinLength, guaranteed by the caller
	var sh SectionHeader
	if err := binary.Read(bytes.NewReader(data[off:]), binary.LittleEndian, &sh); err != nil {
		return nil, p.errf(path, sp, off, "unable to read section header: %v", err)
	}
	headerLen := uint64(SectionMinLength)
	var extSize uint64
	if sh.Size == [3]uint8{0xFF, 0xFF, 0xFF} {
		// Extended header: the real size is in the 4-byte ExtendedSize
		// field of the raw header.
		if rem < SectionExtMinLength {
			return nil, p.errf(path, sp, off, "extended section header truncated: need %#x bytes, have %#x",
				uint64(SectionExtMinLength), rem)
		}
		es := binary.LittleEndian.Uint32(data[off+SectionMinLength:])
		if es == 0xFFFFFFFF {
			return nil, p.errf(path, sp, off, "section size and extended size are all FFs")
		}
		extSize = uint64(es)
		headerLen = SectionExtMinLength
	} else {
		extSize = Read3Size(sh.Size)
	}
	if extSize < headerLen {
		return nil, p.errf(path, sp, off, "invalid section size %#x smaller than header length %#x",
			extSize, headerLen)
	}
	if extSize > rem {
		return nil, p.errf(path, sp, off,
			"section size %#x extends beyond containing payload (%#x bytes left)", extSize, rem)
	}

	s := &Section{FileOrder: order}
	s.Header.SectionHeader = sh
	s.Header.ExtendedSize = uint32(extSize)
	s.Type = sh.Type.String()
	sbuf := make([]byte, extSize)
	copy(sbuf, data[off:off+extSize])
	s.buf = sbuf

	payloadOff := off + headerLen
	var gdh SectionGUIDDefinedHeader
	var gd *SectionGUIDDefined
	guidStr := ""
	if sh.Type == SectionTypeGUIDDefined {
		if extSize < headerLen+guidDefinedHeaderLen {
			return nil, p.errf(path, sp, off,
				"GUID-defined section header truncated: section size %#x, need at least %#x",
				extSize, headerLen+guidDefinedHeaderLen)
		}
		if err := binary.Read(bytes.NewReader(data[off+headerLen:]), binary.LittleEndian, &gdh); err != nil {
			return nil, p.errf(path, sp, off+headerLen, "unable to read GUID-defined section header: %v", err)
		}
		gd = &SectionGUIDDefined{SectionGUIDDefinedHeader: gdh}
		s.TypeSpecific = &TypeSpecificHeader{Type: SectionTypeGUIDDefined, Header: gd}
		guidStr = gdh.GUID.String()
		// The payload starts at the DataOffset read from the raw bytes;
		// any extra authenticated header bytes stay in the header region.
		dataOff := uint64(gdh.DataOffset)
		if dataOff < headerLen+guidDefinedHeaderLen {
			return nil, p.errf(path, sp, off,
				"GUID-defined DataOffset %#x points into the section header (minimum %#x)",
				dataOff, headerLen+guidDefinedHeaderLen)
		}
		if dataOff > extSize {
			return nil, p.errf(path, sp, off,
				"GUID-defined DataOffset %#x beyond section size %#x", dataOff, extSize)
		}
		payloadOff = off + dataOff
	}

	p.addNode(LayoutNode{
		Path:     path,
		Kind:     LayoutNodeSection,
		TypeName: s.Type,
		GUID:     guidStr,
		Space:    sp.id,
		Full:     LayoutSpan{Start: sp.base + off, End: sp.base + off + extSize},
		Payload:  LayoutSpan{Start: sp.base + payloadOff, End: sp.base + off + extSize},
	})

	switch sh.Type {
	case SectionTypeGUIDDefined:
		payloadEnd := off + extSize
		if gdh.Attributes&uint16(GUIDEDSectionProcessingRequired) == 0 {
			// No processing required: the content stays in the parent
			// source space starting at DataOffset.
			children, err := p.parseSections(sp, payloadOff, payloadEnd, polarity, path)
			if err != nil {
				return nil, err
			}
			for _, c := range children {
				s.Encapsulated = append(s.Encapsulated, MakeTyped(c))
			}
			return s, nil
		}
		if gdh.GUID == compression.LZMAGUID {
			// Decode with the pure-Go LZMA decoder from this repository;
			// no external programs are invoked. The decoded stream is a
			// new source space with local, zero-based offsets.
			decoder := &compression.LZMA{}
			decoded, err := decoder.Decode(data[payloadOff:payloadEnd])
			if err != nil {
				return nil, p.errf(path, sp, payloadOff, "unable to decode LZMA GUID-defined section: %v", err)
			}
			gd.Compression = decoder.Name()
			childSp := p.addDecodedSpace(path, sp, payloadOff, payloadEnd, decoded)
			children, err := p.parseSections(childSp, 0, uint64(len(decoded)), polarity, path)
			if err != nil {
				return nil, err
			}
			for _, c := range children {
				s.Encapsulated = append(s.Encapsulated, MakeTyped(c))
			}
			return s, nil
		}
		// Unsupported processing GUID: keep the node opaque instead of
		// guessing its internal structure.
		gd.Compression = "UNKNOWN"
		return s, nil

	case SectionTypeFirmwareVolumeImage:
		// The nested volume lives in the same source space as the
		// section payload and is bounded by it.
		fv, err := p.parseFirmwareVolume(sp, payloadOff, off+extSize, path+"/FV[0]")
		if err != nil {
			return nil, err
		}
		s.Encapsulated = []*TypedFirmware{MakeTyped(fv)}
		return s, nil
	}
	return s, nil
}
