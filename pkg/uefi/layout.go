// Copyright 2026 the LinuxBoot Authors. All rights reserved
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package uefi

import (
	"encoding/binary"
	"fmt"
	"sort"
	"strings"

	"github.com/linuxboot/fiano/pkg/compression"
	"github.com/linuxboot/fiano/pkg/guid"
)

// This file implements a read-only firmware layout analyzer.
//
// The legacy parsers (NewFirmwareVolume, NewFile, NewSection) keep each node's
// bytes in a local buffer and only record the top level FirmwareVolume's
// flash offset. Adding child offsets to the flash base is wrong as soon as a
// GUID defined section decompresses its payload: the decoded bytes live in a
// different address space than the flash image. ParseFirmwareLayout tracks
// every node against an explicit SourceSpace instead, creates a new source
// space for each decoded payload, and builds an independent LayoutIndex that
// answers range queries across those spaces.
//
// The analysis never mutates the input image, the legacy parser objects or the
// package global options (Attributes, ReadOnly, DisableDecompression), and it
// never invokes external programs: LZMA is decoded with the pure Go decoder
// vendored in this repository.

// Source kinds.
const (
	// LayoutSourceFlash is the address space of the image passed to
	// ParseFirmwareLayout. Its coordinates are flashBase + offsets into image.
	LayoutSourceFlash = "flash"
	// LayoutSourceDecoded is an address space produced by decoding a
	// processing-required GUID defined section. Its coordinates always start
	// at zero and are local to the producing encapsulation node.
	LayoutSourceDecoded = "decoded"
)

// Layout node types.
const (
	LayoutNodeFirmwareVolume   = "FirmwareVolume"
	LayoutNodeFile             = "File"
	LayoutNodeSection          = "Section"
	LayoutNodePadding          = "Padding"
	LayoutNodeFreeSpace        = "FreeSpace"
	LayoutNodeOpaqueGUIDed     = "OpaqueGUIDDefined"
	LayoutNodeDecodedSpaceRoot = "DecodedSpace"
)

// LayoutSource identifies the address space a layout node lives in.
//
// Flash space is a singleton: ID 0, Base flashBase, Size len(image).
// Decoded spaces are numbered in the order their producing GUID defined
// sections are encountered (depth first); their Base is always zero and their
// Size is the decoded payload length. Equal local offsets in two decoded
// spaces never alias because the space IDs differ.
type LayoutSource struct {
	// ID is the stable identifier of the space: 0 for flash, 1..N for
	// decoded spaces in discovery order.
	ID uint64

	// Kind is LayoutSourceFlash or LayoutSourceDecoded.
	Kind string

	// Base is the coordinate of byte zero of the space. It is flashBase for
	// the flash space and zero for every decoded space.
	Base uint64

	// Size is the number of bytes backing the space.
	Size uint64

	// ProducerPath is empty for the flash space; for a decoded space it is
	// the stable ancestor path of the GUID defined section whose decode
	// produced the space.
	ProducerPath string

	// CompressedRange records, for a decoded space, where the compressed
	// payload lives in the parent source space (parent-local coordinates).
	// It is zero-valued for flash.
	CompressedRange LayoutRange
}

// LayoutRange is a half-open interval [Start, End) inside one source space.
// Node ranges use space-local coordinates: image offsets for the flash space
// (add the space Base to get the physical address), zero-based offsets for
// decoded spaces.
type LayoutRange struct {
	Start uint64
	End   uint64
}

// LayoutNode describes one element of the parsed firmware layout.
//
// Every range is expressed in local coordinates of Source. Record spans the
// whole binary record including its header; Payload spans the record's payload
// only. Padding and free space nodes use Record for their extent and leave
// Payload as the zero value.
type LayoutNode struct {
	// Path is the stable ancestor path of the node, built from node type
	// and zero-based sibling position, for example
	// "/FirmwareVolume[0]/File[0]/Section[1]".
	Path string

	// Kind is one of the LayoutNode* constants.
	Kind string

	// TypeName carries the PI type name (FV file system GUID name, file type
	// name or section type name) when the node has a typed header.
	TypeName string

	// GUID carries the identifying GUID of a file, GUID defined section or
	// firmware volume name, when present.
	GUID string

	// Source is the space the node's ranges are interpreted in.
	Source LayoutSource

	// Record is the complete binary span of the node in Source coordinates.
	Record LayoutRange

	// Payload is the payload span of the node in Source coordinates.
	Payload LayoutRange

	// Children are the direct descendants of the node, in parse order.
	Children []*LayoutNode

	// ErasePolarity is the erase polarity (0x00 or 0xFF) used by the firmware
	// volume that owns the node. It is zero for non volume nodes.
	ErasePolarity uint8

	// DecodedSpaceID is non zero for the GUID defined section that produced
	// the decoded space with this ID; zero otherwise.
	DecodedSpaceID uint64
}

// LayoutReport is the result of ParseFirmwareLayout: the parsed node tree plus
// a description of every source space backing it.
type LayoutReport struct {
	// Root is the outermost firmware volume.
	Root *LayoutNode

	// Sources lists every source space, flash first, in ID order.
	Sources []LayoutSource

	// FlashBase is the base address used for flash coordinates.
	FlashBase uint64
}

// LayoutIndex is an independent, self-contained index over a layout report.
// It copies every node (and source descriptor) it stores, so mutating the
// image byte slice (or the report tree) after building the index does not
// affect queries.
type LayoutIndex struct {
	nodes   []LayoutNode
	sources []LayoutSource
	childOf map[string]string
}

// layoutParser carries the mutable state of one analysis. It never touches the
// package globals.
type layoutParser struct {
	sources []LayoutSource
	index   *LayoutIndex
}

// recordRange validates that a record of length fits the passed slice and
// builds its source-local range starting at localBase, proving the address
// addition cannot overflow. The slice may start partway through the source
// space, so containment is checked against its length rather than localBase.
func recordRange(localBase, length, bufLen uint64, what string) (LayoutRange, error) {
	if length > bufLen {
		return LayoutRange{}, fmt.Errorf(
			"%s: declared record length %#x exceeds available %#x bytes", what, length, bufLen)
	}
	if localBase > ^uint64(0)-length {
		return LayoutRange{}, fmt.Errorf(
			"%s: record end overflows: base %#x length %#x", what, localBase, length)
	}
	return LayoutRange{Start: localBase, End: localBase + length}, nil
}

// sliceChecked returns buf[start:start+length] after validating the interval.
// It never borrows bytes from a sibling record to make up a short length.
func sliceChecked(buf []byte, start, length uint64, what string) ([]byte, error) {
	if length > uint64(len(buf)) || start > uint64(len(buf))-length {
		return nil, fmt.Errorf(
			"%s: declared range exceeds available data: offset %#x length %#x available %#x",
			what, start, length, len(buf))
	}
	return buf[start : start+length], nil
}

// readUint16/32/64 helpers with explicit bounds checks and locatable errors.
func readUint16At(buf []byte, off uint64, what string) (uint16, error) {
	if off > uint64(len(buf))-2 {
		return 0, fmt.Errorf("%s: header too short for uint16 at offset %#x (have %#x bytes)",
			what, off, len(buf))
	}
	return binary.LittleEndian.Uint16(buf[off:]), nil
}

func readUint32At(buf []byte, off uint64, what string) (uint32, error) {
	if off > uint64(len(buf))-4 {
		return 0, fmt.Errorf("%s: header too short for uint32 at offset %#x (have %#x bytes)",
			what, off, len(buf))
	}
	return binary.LittleEndian.Uint32(buf[off:]), nil
}

func readUint64At(buf []byte, off uint64, what string) (uint64, error) {
	if off > uint64(len(buf))-8 {
		return 0, fmt.Errorf("%s: header too short for uint64 at offset %#x (have %#x bytes)",
			what, off, len(buf))
	}
	return binary.LittleEndian.Uint64(buf[off:]), nil
}

func readGUIDAt(buf []byte, off uint64, what string) (guid.GUID, error) {
	var g guid.GUID
	if off > uint64(len(buf))-guid.Size {
		return g, fmt.Errorf("%s: header too short for GUID at offset %#x (have %#x bytes)",
			what, off, len(buf))
	}
	copy(g[:], buf[off:off+guid.Size])
	return g, nil
}

// appendNode registers the node with the report-wide index and links it into
// its parent's children.
func (p *layoutParser) appendNode(parent *LayoutNode, node *LayoutNode) {
	p.index.nodes = append(p.index.nodes, *node)
	if parent != nil {
		p.index.childOf[node.Path] = parent.Path
	} else {
		p.index.childOf[node.Path] = ""
	}
	if parent != nil {
		parent.Children = append(parent.Children, node)
	}
}

// childPath builds the stable path of the nth child of kind.
func childPath(parent *LayoutNode, kind string, n int) string {
	if parent == nil {
		return fmt.Sprintf("/%s[%d]", kind, n)
	}
	return fmt.Sprintf("%s/%s[%d]", parent.Path, kind, n)
}

// ParseFirmwareLayout parses a complete FFS2 or FFS3 firmware volume image and
// returns both a node tree and an independent layout index.
//
// flashBase is the physical address of image[0]; it only applies to the flash
// source space. Payloads decoded from processing-required GUID defined sections
// become their own zero-based source spaces and are never assigned flash
// addresses. The input slice is treated as read-only and is not retained by
// the returned report or index.
func ParseFirmwareLayout(image []byte, flashBase uint64) (*LayoutReport, *LayoutIndex, error) {
	p := &layoutParser{
		sources: []LayoutSource{{
			ID:   0,
			Kind: LayoutSourceFlash,
			Base: flashBase,
			Size: uint64(len(image)),
		}},
		index: &LayoutIndex{childOf: map[string]string{}},
	}

	root, err := p.parseVolume(image, 0, 0, nil)
	if err != nil {
		return nil, nil, fmt.Errorf("firmware layout parse failed: %w", err)
	}
	p.index.sources = make([]LayoutSource, len(p.sources))
	copy(p.index.sources, p.sources)
	return &LayoutReport{
		Root:      root,
		Sources:   p.sources,
		FlashBase: flashBase,
	}, p.index, nil
}

// parseVolume parses one firmware volume. buf is the backing space identified
// by sourceID, with the volume record starting at offset zero within buf.
// localBase is the offset of buf[0] within the source space.
func (p *layoutParser) parseVolume(buf []byte, localBase, sourceID uint64, parent *LayoutNode) (*LayoutNode, error) {
	const (
		fvFixedHeaderSize = FirmwareVolumeFixedHeaderSize // 56
		fvBlockSize       = 8
		fvExtHeaderMin    = FirmwareVolumeExtHeaderMinSize // 20
	)
	what := "firmware volume"
	if uint64(len(buf)) < FirmwareVolumeMinSize {
		return nil, fmt.Errorf("%s: buffer too small: expected at least %#x bytes, got %#x",
			what, FirmwareVolumeMinSize, len(buf))
	}

	// Fixed header fields, read straight from the original bytes.
	fsGUID, err := readGUIDAt(buf, 16, what)
	if err != nil {
		return nil, err
	}
	length, err := readUint64At(buf, 32, what)
	if err != nil {
		return nil, err
	}
	signature, err := readUint32At(buf, 40, what)
	if err != nil {
		return nil, err
	}
	if signature != 0x4856465f { // "_FVH"
		return nil, fmt.Errorf("%s: bad signature %#08x at offset %#x", what, signature, localBase+40)
	}
	attributes, err := readUint32At(buf, 44, what)
	if err != nil {
		return nil, err
	}
	headerLen16, err := readUint16At(buf, 48, what)
	if err != nil {
		return nil, err
	}
	headerLen := uint64(headerLen16)
	extOffset16, err := readUint16At(buf, 52, what)
	if err != nil {
		return nil, err
	}
	extOffset := uint64(extOffset16)

	polarity := byte(0)
	if attributes&0x800 != 0 {
		polarity = 0xFF
	}

	// The declared volume length must fit the backing space; never walk past
	// it using sibling bytes.
	record, err := recordRange(localBase, length, uint64(len(buf)), what)
	if err != nil {
		return nil, err
	}
	volBytes, err := sliceChecked(buf, 0, length, what)
	if err != nil {
		return nil, err
	}

	// Walk the block map to locate its terminating zero entry and validate it
	// against the actual header length.
	blockEnd := uint64(fvFixedHeaderSize)
	for {
		entry, err := sliceChecked(volBytes, blockEnd, fvBlockSize, what+" block map")
		if err != nil {
			return nil, err
		}
		count := binary.LittleEndian.Uint32(entry)
		size := binary.LittleEndian.Uint32(entry[4:])
		blockEnd += fvBlockSize
		if count == 0 && size == 0 {
			break
		}
		if headerLen != 0 && blockEnd >= headerLen {
			return nil, fmt.Errorf("%s: block map runs past header length %#x", what, headerLen)
		}
	}
	if headerLen < blockEnd {
		return nil, fmt.Errorf("%s: header length %#x smaller than block map end %#x",
			what, headerLen, blockEnd)
	}

	// Extended header is optional. The data offset is derived from the raw
	// ExtHeaderOffset/ExtHeaderSize fields, never from a re-serialized header.
	dataOffset := headerLen
	volName := guid.GUID{}
	hasExt := false
	if extOffset != 0 {
		if extOffset >= length || length-extOffset < fvExtHeaderMin {
			return nil, fmt.Errorf("%s: extended header offset %#x invalid for length %#x",
				what, extOffset, length)
		}
		volName, err = readGUIDAt(volBytes, extOffset, what+" extended header")
		if err != nil {
			return nil, err
		}
		extSize32, err := readUint32At(volBytes, extOffset+16, what+" extended header")
		if err != nil {
			return nil, err
		}
		extSize := uint64(extSize32)
		if extSize < fvExtHeaderMin || extOffset+extSize > length {
			return nil, fmt.Errorf("%s: extended header size %#x invalid at offset %#x",
				what, extSize, extOffset)
		}
		dataOffset = Align8(extOffset + extSize)
		hasExt = true
	}
	if dataOffset > length {
		return nil, fmt.Errorf("%s: data offset %#x exceeds volume length %#x", what, dataOffset, length)
	}

	node := &LayoutNode{
		Kind:          LayoutNodeFirmwareVolume,
		TypeName:      FVGUIDs[fsGUID],
		GUID:          volumeGUIDString(fsGUID, volName, hasExt),
		Source:        p.sources[sourceID],
		Record:        record,
		Payload:       LayoutRange{Start: localBase + dataOffset, End: record.End},
		ErasePolarity: polarity,
	}
	node.Path = childPath(parent, LayoutNodeFirmwareVolume, p.volumeChildIndex(parent))
	p.appendNode(parent, node)

	if !supportedFVs[fsGUID] {
		// Unsupported file system: keep the volume record as an opaque node.
		return node, nil
	}

	// Parse files between dataOffset and the end of the declared volume.
	// Alignment padding between containers belongs to the volume, never to the
	// next child.
	cursor := dataOffset
	fileIndex := 0
	for cursor < length {
		aligned := Align8(cursor)
		if aligned > length {
			// A sub-header alignment gap that cannot fit another file stays
			// with the volume and is not attributed to the next child.
			p.addPaddingNode(node, LayoutNodePadding, sourceID, localBase, cursor, length, polarity)
			break
		}
		if aligned > cursor {
			p.addPaddingNode(node, LayoutNodePadding, sourceID, localBase, cursor, aligned, polarity)
			cursor = aligned
		}
		if length-cursor < FileHeaderMinLength {
			// Remaining tail is smaller than any file header: it belongs to
			// the volume. Erased tails are reported as free space, anything
			// else as preserved padding.
			tail, err := sliceChecked(volBytes, cursor, length-cursor, what+" free space")
			if err != nil {
				return nil, err
			}
			kind := LayoutNodePadding
			if isErasedBytes(tail, polarity) {
				kind = LayoutNodeFreeSpace
			}
			p.addPaddingNode(node, kind, sourceID, localBase, cursor, length, polarity)
			break
		}
		headerProbe, err := sliceChecked(volBytes, cursor, FileHeaderMinLength, what+" free space probe")
		if err != nil {
			return nil, err
		}
		if isErasedBytes(headerProbe, polarity) {
			// An erased file header marks the start of free space for both
			// erase polarities (0xFF shows the FFFFFF size marker, 0x00 shows
			// an all-zero header).
			kind := LayoutNodePadding
			tail, err := sliceChecked(volBytes, cursor, length-cursor, what+" free space")
			if err != nil {
				return nil, err
			}
			if isErasedBytes(tail, polarity) {
				kind = LayoutNodeFreeSpace
			}
			p.addPaddingNode(node, kind, sourceID, localBase, cursor, length, polarity)
			break
		}

		consumed, free, err := p.parseFile(volBytes[cursor:], localBase+cursor, sourceID, node, fileIndex, polarity)
		if err != nil {
			return nil, fmt.Errorf("%s: file at offset %#x: %w", what, cursor, err)
		}
		if free {
			// Start of free space. The whole remainder of the volume belongs
			// to the volume as free space; do not invent phantom files.
			p.addPaddingNode(node, LayoutNodeFreeSpace, sourceID, localBase, cursor, length, polarity)
			break
		}
		if consumed == 0 {
			return nil, fmt.Errorf("%s: zero-length file at offset %#x cannot progress", what, cursor)
		}
		cursor += consumed
		fileIndex++
	}
	return node, nil
}

// volumeChildIndex returns the position the next FirmwareVolume child would
// take under parent. Firmware volumes only nest inside sections.
func (p *layoutParser) volumeChildIndex(parent *LayoutNode) int {
	if parent == nil {
		return 0
	}
	n := 0
	for _, c := range parent.Children {
		if c.Kind == LayoutNodeFirmwareVolume {
			n++
		}
	}
	return n
}

// addPaddingNode registers a padding or free-space node.
func (p *layoutParser) addPaddingNode(parent *LayoutNode, kind string, sourceID, localBase, start, end uint64, polarity byte) {
	if end <= start {
		return
	}
	node := &LayoutNode{
		Kind:          kind,
		Source:        p.sources[sourceID],
		Record:        LayoutRange{Start: localBase + start, End: localBase + end},
		ErasePolarity: polarity,
	}
	node.Path = childPath(parent, kind, p.childCount(parent, kind))
	p.appendNode(parent, node)
}

func (p *layoutParser) childCount(parent *LayoutNode, kind string) int {
	if parent == nil {
		return 0
	}
	n := 0
	for _, c := range parent.Children {
		if c.Kind == kind {
			n++
		}
	}
	return n
}

// parseFile parses one FFS file at buf[0] inside source sourceID. It returns
// the number of source bytes the record occupies and free=true when the bytes
// mark volume free space.
func (p *layoutParser) parseFile(buf []byte, localBase, sourceID uint64, parent *LayoutNode, index int, polarity byte) (consumed uint64, free bool, err error) {
	what := "firmware file"
	if uint64(len(buf)) < FileHeaderMinLength {
		return 0, false, fmt.Errorf("%s: buffer smaller than minimum header %#x", what, FileHeaderMinLength)
	}
	fileGUID, err := readGUIDAt(buf, 0, what)
	if err != nil {
		return 0, false, err
	}
	fileType := FVFileType(buf[18])
	size3 := [3]byte{buf[20], buf[21], buf[22]}
	isLarge := size3 == [3]byte{0xFF, 0xFF, 0xFF}

	var headerLen, totalSize uint64
	if isLarge {
		if uint64(len(buf)) < FileHeaderExtMinLength {
			return 0, false, fmt.Errorf("%s: large file declared but buffer smaller than extended header %#x",
				what, FileHeaderExtMinLength)
		}
		extSize, err := readUint64At(buf, 24, what)
		if err != nil {
			return 0, false, err
		}
		if extSize == 0xFFFFFFFFFFFFFFFF {
			// FFS free-space marker, not a pad file.
			return 0, true, nil
		}
		headerLen = FileHeaderExtMinLength
		totalSize = extSize
	} else {
		headerLen = FileHeaderMinLength
		totalSize = Read3Size(size3)
	}
	if totalSize < headerLen {
		return 0, false, fmt.Errorf("%s: declared size %#x smaller than header length %#x",
			what, totalSize, headerLen)
	}
	record, err := recordRange(localBase, totalSize, uint64(len(buf)), what)
	if err != nil {
		return 0, false, err
	}
	fileBytes, err := sliceChecked(buf, 0, totalSize, what)
	if err != nil {
		return 0, false, err
	}

	node := &LayoutNode{
		Kind:     LayoutNodeFile,
		TypeName: fileType.String(),
		GUID:     fileGUID.String(),
		Source:   p.sources[sourceID],
		Record:   record,
		Payload: LayoutRange{
			Start: localBase + headerLen,
			End:   record.End,
		},
		ErasePolarity: polarity,
	}
	node.Path = childPath(parent, LayoutNodeFile, index)
	p.appendNode(parent, node)

	if !SupportedFiles[fileType] {
		// Raw and other opaque file types keep their payload as bytes only.
		return totalSize, false, nil
	}

	// Sections start right after the file header.
	secIndex := 0
	cursor := headerLen
	for cursor < totalSize {
		aligned := Align4(cursor)
		if aligned > totalSize {
			break
		}
		if aligned > cursor {
			p.addPaddingNode(node, LayoutNodePadding, sourceID, localBase, cursor, aligned, polarity)
			cursor = aligned
		}
		if totalSize-cursor < SectionMinLength {
			break
		}
		secSize, err := p.parseSection(fileBytes[cursor:], localBase+cursor, sourceID, node, secIndex, polarity)
		if err != nil {
			return 0, false, fmt.Errorf("section at offset %#x: %w", cursor, err)
		}
		if secSize == 0 {
			// Erased section header: the rest of the file is padding.
			p.addPaddingNode(node, LayoutNodePadding, sourceID, localBase, cursor, totalSize, polarity)
			cursor = totalSize
			break
		}
		cursor += secSize
		secIndex++
	}
	if cursor < totalSize {
		// Unconsumed tail stays inside the file rather than being lent to the
		// next sibling; record it explicitly so the range remains queryable.
		if _, perr := sliceChecked(fileBytes, cursor, totalSize-cursor, what+" tail"); perr != nil {
			return 0, false, perr
		}
		p.addPaddingNode(node, LayoutNodePadding, sourceID, localBase, cursor, totalSize, polarity)
	}
	return totalSize, false, nil
}

// parseSection parses one section at buf[0] and returns its full record size
// (common header plus type specific header plus payload) in source bytes.
func (p *layoutParser) parseSection(buf []byte, localBase, sourceID uint64, parent *LayoutNode, index int, polarity byte) (uint64, error) {
	what := "section"
	if uint64(len(buf)) < SectionMinLength {
		return 0, fmt.Errorf("%s: buffer smaller than minimum header %#x", what, SectionMinLength)
	}
	// An erased common header (0xFFFFFF for 0xFF polarity or 0x000000 for
	// 0x00 polarity) marks the end of the section stream rather than a real
	// section; the caller records the surrounding tail as padding.
	if isErasedBytes(buf[:SectionMinLength], polarity) {
		return 0, nil
	}
	size3 := [3]byte{buf[0], buf[1], buf[2]}
	secType := SectionType(buf[3])

	var commonHeaderLen, totalSize uint64
	if size3 == [3]byte{0xFF, 0xFF, 0xFF} {
		if uint64(len(buf)) < SectionExtMinLength {
			return 0, fmt.Errorf("%s: extended section declared but buffer smaller than %#x bytes",
				what, SectionExtMinLength)
		}
		extSize, err := readUint32At(buf, 4, what)
		if err != nil {
			return 0, err
		}
		if extSize == 0xFFFFFFFF {
			return 0, fmt.Errorf("%s: section size and extended size are both erased", what)
		}
		commonHeaderLen = SectionExtMinLength
		totalSize = uint64(extSize)
	} else {
		commonHeaderLen = SectionMinLength
		totalSize = Read3Size(size3)
	}
	if totalSize < commonHeaderLen {
		return 0, fmt.Errorf("%s: declared size %#x smaller than common header %#x",
			what, totalSize, commonHeaderLen)
	}
	record, err := recordRange(localBase, totalSize, uint64(len(buf)), what)
	if err != nil {
		return 0, err
	}
	secBytes, err := sliceChecked(buf, 0, totalSize, what)
	if err != nil {
		return 0, err
	}

	node := &LayoutNode{
		Kind:     LayoutNodeSection,
		TypeName: secType.String(),
		Source:   p.sources[sourceID],
		Record:   record,
	}
	node.Path = childPath(parent, LayoutNodeSection, index)
	p.appendNode(parent, node)

	switch secType {
	case SectionTypeGUIDDefined:
		return p.parseGUIDDefined(secBytes, localBase, sourceID, totalSize, commonHeaderLen, node, polarity)
	case SectionTypeFirmwareVolumeImage:
		if totalSize <= commonHeaderLen {
			return 0, fmt.Errorf("%s: firmware volume image section has no payload", what)
		}
		payloadStart := commonHeaderLen
		nested, err := sliceChecked(secBytes, payloadStart, totalSize-payloadStart, what+" nested volume")
		if err != nil {
			return 0, err
		}
		node.Payload = LayoutRange{Start: localBase + payloadStart, End: record.End}
		if _, err := p.parseVolume(nested, localBase+payloadStart, sourceID, node); err != nil {
			return 0, err
		}
		return totalSize, nil
	default:
		// All other section types are leaf records; their payload starts right
		// after the common header.
		node.Payload = LayoutRange{Start: localBase + commonHeaderLen, End: record.End}
		return totalSize, nil
	}
}

// parseGUIDDefined handles a GUID defined encapsulation section. Transparent
// sections keep their payload in the parent space; processing-required LZMA
// sections decode into a brand new source space.
func (p *layoutParser) parseGUIDDefined(secBytes []byte, localBase, sourceID, totalSize, commonHeaderLen uint64, node *LayoutNode, polarity byte) (uint64, error) {
	what := "guid defined section"
	gdHeaderLen := uint64(20) // GUID (16) + DataOffset (2) + Attributes (2)
	if totalSize < commonHeaderLen+gdHeaderLen {
		return 0, fmt.Errorf("%s: record too small for GUID defined type header", what)
	}
	gd, err := readGUIDAt(secBytes, commonHeaderLen, what)
	if err != nil {
		return 0, err
	}
	dataOffset16, err := readUint16At(secBytes, commonHeaderLen+16, what)
	if err != nil {
		return 0, err
	}
	attributes16, err := readUint16At(secBytes, commonHeaderLen+18, what)
	if err != nil {
		return 0, err
	}
	dataOffset := uint64(dataOffset16)

	// The DataOffset comes straight from the image and must point inside the
	// declared record, after both headers. Any extra header bytes (a larger
	// than default DataOffset) stay part of the record instead of being
	// treated as payload.
	if dataOffset < commonHeaderLen+gdHeaderLen || dataOffset > totalSize {
		return 0, fmt.Errorf("%s: GUID DataOffset %#x outside record of size %#x",
			what, dataOffset, totalSize)
	}
	node.GUID = gd.String()
	node.Payload = LayoutRange{Start: localBase + dataOffset, End: localBase + totalSize}

	processingRequired := attributes16&uint16(GUIDEDSectionProcessingRequired) != 0
	if !processingRequired {
		// Transparent encapsulation: children share the parent source space
		// and start at the raw DataOffset.
		return totalSize, p.parseSectionStream(secBytes, localBase, sourceID, totalSize, dataOffset, node, polarity)
	}

	compressor := compression.PureGoCompressor(&gd)
	if compressor == nil {
		// Unknown processing GUID: keep the section opaque and do not guess at
		// an internal structure.
		node.Kind = LayoutNodeOpaqueGUIDed
		node.TypeName = "EFI_SECTION_GUID_DEFINED"
		return totalSize, nil
	}

	compressed, err := sliceChecked(secBytes, dataOffset, totalSize-dataOffset, what+" compressed payload")
	if err != nil {
		return 0, err
	}
	decoded, err := compressor.Decode(compressed)
	if err != nil {
		return 0, fmt.Errorf("%s: %s decode failed: %w", what, compressor.Name(), err)
	}

	// Register a brand new zero-based source space. Its data is owned by the
	// report and survives mutations of the caller's image.
	newID := uint64(len(p.sources))
	space := LayoutSource{
		ID:           newID,
		Kind:         LayoutSourceDecoded,
		Base:         0,
		Size:         uint64(len(decoded)),
		ProducerPath: node.Path,
		CompressedRange: LayoutRange{
			Start: localBase + dataOffset,
			End:   localBase + totalSize,
		},
	}
	p.sources = append(p.sources, space)
	node.DecodedSpaceID = newID

	rootNode := &LayoutNode{
		Kind:   LayoutNodeDecodedSpaceRoot,
		Source: space,
		Record: LayoutRange{Start: 0, End: uint64(len(decoded))},
	}
	rootNode.Path = node.Path + "/" + LayoutNodeDecodedSpaceRoot + "[0]"
	p.appendNode(node, rootNode)

	// The decoded stream is itself a sequence of sections (which may contain
	// nested firmware volumes or more GUID defined sections).
	if err := p.parseSectionStream(decoded, 0, newID, uint64(len(decoded)), 0, rootNode, polarity); err != nil {
		return 0, fmt.Errorf("%s: decoded payload: %w", what, err)
	}
	return totalSize, nil
}

// parseSectionStream walks a sequence of sections with 4-byte alignment
// between records. buf backs source sourceID; localBase is the source-local
// offset of buf[0].
func (p *layoutParser) parseSectionStream(buf []byte, localBase, sourceID, limit, start uint64, container *LayoutNode, polarity byte) error {
	cursor := start
	secIndex := 0
	for cursor < limit {
		aligned := Align4(cursor)
		if aligned > limit {
			// A tail that cannot fit another section stays with the stream.
			p.addPaddingNode(container, LayoutNodePadding, sourceID, localBase, cursor, limit, polarity)
			break
		}
		if aligned > cursor {
			p.addPaddingNode(container, LayoutNodePadding, sourceID, localBase, cursor, aligned, polarity)
			cursor = aligned
		}
		if limit-cursor < SectionMinLength {
			p.addPaddingNode(container, LayoutNodePadding, sourceID, localBase, cursor, limit, polarity)
			break
		}
		secSize, err := p.parseSection(buf[cursor:], localBase+cursor, sourceID, container, secIndex, polarity)
		if err != nil {
			return err
		}
		if secSize == 0 {
			// Erased section header: the rest of the stream is padding.
			p.addPaddingNode(container, LayoutNodePadding, sourceID, localBase, cursor, limit, polarity)
			cursor = limit
			break
		}
		cursor += secSize
		secIndex++
	}
	return nil
}

// isErasedBytes reports whether every byte equals the volume erase polarity.
func isErasedBytes(buf []byte, polarity byte) bool {
	for _, b := range buf {
		if b != polarity {
			return false
		}
	}
	return true
}

// volumeGUIDString renders the identifier used for a volume node: the FV name
// from the extended header when present, otherwise the file system GUID.
func volumeGUIDString(fsGUID, name guid.GUID, hasExt bool) string {
	if hasExt {
		return name.String()
	}
	return fsGUID.String()
}

// Query returns every stored node whose record intersects the half-open
// interval [start, end) in source space sourceID, together with their
// ancestors and descendants, sorted by stable path.
//
// For the flash space pass flash-based addresses (the flashBase given to
// ParseFirmwareLayout is subtracted internally); for a decoded space pass the
// local coordinates of that space. Intervals from different decoded spaces
// never mix because their IDs are part of the query. A query that only hits
// padding or free space returns just the padding node and its owning container
// chain, never fabricated children.
func (idx *LayoutIndex) Query(sourceID, start, end uint64) ([]LayoutNode, error) {
	if end < start {
		return nil, fmt.Errorf("layout query: inverted interval [%#x, %#x)", start, end)
	}

	// Translate the caller's space coordinates into the local coordinates used
	// by the stored ranges. Decoded spaces have Base zero; flash space uses
	// flashBase.
	if sourceID >= uint64(len(idx.sources)) {
		return nil, fmt.Errorf("layout query: unknown source space ID %d", sourceID)
	}
	space := idx.sources[sourceID]
	if space.ID != sourceID {
		return nil, fmt.Errorf("layout query: source space index %d out of order", sourceID)
	}
	base := space.Base
	if start < base {
		return nil, fmt.Errorf("layout query: start %#x below source space base %#x", start, base)
	}
	localStart, localEnd := start-base, end-base

	// Pass 1: collect every node whose record intersects the interval (the
	// hit set) together with all of its ancestors.
	hits := map[string]bool{}
	selected := map[string]bool{}
	for i := range idx.nodes {
		n := &idx.nodes[i]
		if n.Source.ID != sourceID {
			continue
		}
		if n.Record.End <= localStart || n.Record.Start >= localEnd {
			continue
		}
		hits[n.Path] = true
		path := n.Path
		for path != "" {
			selected[path] = true
			path = idx.childOf[path]
		}
	}

	// Only the deepest hits expand downward. Without this restriction the
	// outermost container (which intersects every sub-range) would pull in
	// unrelated siblings, and a padding-only query would fabricate children.
	deepest := map[string]bool{}
	for path := range hits {
		isDeepest := true
		for other := range hits {
			if other == path {
				continue
			}
			if idx.isAncestor(path, other) {
				isDeepest = false
				break
			}
		}
		if isDeepest {
			deepest[path] = true
		}
	}

	// Pass 2: pull in descendants of the deepest hits (including descendants
	// living in a decoded child space).
	for i := range idx.nodes {
		path := idx.nodes[i].Path
		for path != "" {
			if deepest[path] {
				selected[idx.nodes[i].Path] = true
				break
			}
			path = idx.childOf[path]
		}
	}

	out := make([]LayoutNode, 0, len(selected))
	for i := range idx.nodes {
		if selected[idx.nodes[i].Path] {
			out = append(out, idx.nodes[i])
		}
	}
	sort.SliceStable(out, func(i, j int) bool {
		return pathLess(out[i].Path, out[j].Path)
	})
	return out, nil
}

// isAncestor reports whether ancestor is a strict ancestor of descendant in
// the indexed tree.
func (idx *LayoutIndex) isAncestor(ancestor, descendant string) bool {
	path := idx.childOf[descendant]
	for path != "" {
		if path == ancestor {
			return true
		}
		path = idx.childOf[path]
	}
	return false
}

// pathLess orders stable paths as a human would read them: path segments in
// lexicographic order with their bracketed index compared numerically.
func pathLess(a, b string) bool {
	sa := strings.Split(a, "/")
	sb := strings.Split(b, "/")
	for i := 0; i < len(sa) && i < len(sb); i++ {
		if sa[i] == sb[i] {
			continue
		}
		na, ia := splitPathSegment(sa[i])
		nb, ib := splitPathSegment(sb[i])
		if na != nb {
			return na < nb
		}
		return ia < ib
	}
	return len(sa) < len(sb)
}

// splitPathSegment splits "Kind[n]" into ("Kind", n).
func splitPathSegment(seg string) (string, int) {
	open := strings.IndexByte(seg, '[')
	if open < 0 {
		return seg, 0
	}
	var idx int
	if _, err := fmt.Sscanf(seg[open+1:len(seg)-1], "%d", &idx); err != nil {
		return seg[:open], 0
	}
	return seg[:open], idx
}

// Sources returns a copy of the source spaces held by the index, flash first.
func (idx *LayoutIndex) Sources() []LayoutSource {
	out := make([]LayoutSource, len(idx.sources))
	copy(out, idx.sources)
	return out
}

// Nodes returns a copy of every indexed node in stable path order. It lets
// callers inspect the whole layout without retaining the parse tree.
func (idx *LayoutIndex) Nodes() []LayoutNode {
	out := make([]LayoutNode, len(idx.nodes))
	copy(out, idx.nodes)
	sort.SliceStable(out, func(i, j int) bool {
		return pathLess(out[i].Path, out[j].Path)
	})
	return out
}

// Find returns a copy of the indexed node with the given stable path, or nil.
func (idx *LayoutIndex) Find(path string) *LayoutNode {
	for i := range idx.nodes {
		if idx.nodes[i].Path == path {
			n := idx.nodes[i]
			return &n
		}
	}
	return nil
}
