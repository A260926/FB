// Copyright 2026 the LinuxBoot Authors. All rights reserved
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package uefi

import (
	"bytes"
	"fmt"
	"testing"

	"github.com/linuxboot/fiano/pkg/compression"
	"github.com/linuxboot/fiano/pkg/guid"
)

// Coordinate conventions used by these tests:
//
//   - The flash source space always has ID 0. Its node ranges are offsets into
//     the input image; physical addresses are flashBase plus the range.
//   - A processing-required GUID defined section that decodes successfully
//     creates a decoded source space with ID >= 1, Base 0 and offsets that
//     restart at zero. Equal offsets in two decoded spaces are distinct
//     addresses because the space ID differs.
//   - Every range is half-open [Start, End).

var (
	layoutTestGUID1  = guid.MustParse("11111111-2222-3333-4444-555555555555")
	layoutTestGUID2  = guid.MustParse("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
	layoutTestGUID3  = guid.MustParse("12345678-1234-1234-1234-123456789abc")
	layoutTransGUID  = guid.MustParse("fedcba98-7654-3210-fedc-ba9876543210")
	layoutOpaqueGUID = guid.MustParse("abcdef01-2345-6789-abcd-ef0123456789")
)

func put16(b []byte, off uint64, v uint16) {
	b[off] = byte(v)
	b[off+1] = byte(v >> 8)
}

func put32(b []byte, off uint64, v uint32) {
	b[off] = byte(v)
	b[off+1] = byte(v >> 8)
	b[off+2] = byte(v >> 16)
	b[off+3] = byte(v >> 24)
}

func put64(b []byte, off uint64, v uint64) {
	put32(b, off, uint32(v))
	put32(b, off+4, uint32(v>>32))
}

func putGUID(b []byte, off uint64, g guid.GUID) {
	copy(b[off:off+guid.Size], g[:])
}

// buildLayoutFV synthesizes a firmware volume of size bytes. When ext is true
// an extended header is placed at offset 72 and file data starts at 96.
func buildLayoutFV(size int, fsGUID *guid.GUID, attributes uint32, ext bool, body []byte, bodyOffset uint64) []byte {
	polarity := byte(0xFF)
	if attributes&0x800 == 0 {
		polarity = 0x00
	}
	b := bytes.Repeat([]byte{polarity}, size)
	putGUID(b, 16, *fsGUID)
	put64(b, 32, uint64(size))
	put32(b, 40, 0x4856465f) // "_FVH"
	put32(b, 44, attributes)
	put16(b, 48, 72) // HeaderLen covers the fixed header plus the block map
	put16(b, 50, 0)  // checksum, not validated by the layout parser
	put16(b, 52, 0)  // ExtHeaderOffset is zero unless an extended header follows
	b[55] = 2        // revision
	// Block map: one entry plus the terminating zero entry.
	put32(b, 56, 1)
	put32(b, 60, uint32(size))
	put32(b, 64, 0)
	put32(b, 68, 0)
	if ext {
		put16(b, 52, 72) // ExtHeaderOffset
		putGUID(b, 72, *layoutTestGUID3)
		put32(b, 88, 20) // ExtHeaderSize -> data offset aligns to 96
	}
	if body != nil {
		copy(b[bodyOffset:], body)
	}
	return b
}

// buildLayoutFile synthesizes one FFS file record. Unused body bytes are
// filled with the 0xFF erase polarity, like padding inside a real FFS file.
func buildLayoutFile(g guid.GUID, ftype FVFileType, large bool, totalSize uint64, body []byte) []byte {
	headerLen := uint64(FileHeaderMinLength)
	if large {
		headerLen = FileHeaderExtMinLength
	}
	b := bytes.Repeat([]byte{0xFF}, int(totalSize))
	putGUID(b, 0, g)
	b[18] = byte(ftype)
	b[19] = 0 // attributes
	b[23] = 0xF8
	if large {
		b[20], b[21], b[22] = 0xFF, 0xFF, 0xFF
		put64(b, 24, totalSize)
	} else {
		b[20], b[21], b[22] = byte(totalSize), byte(totalSize>>8), byte(totalSize>>16)
	}
	copy(b[headerLen:], body)
	return b
}

// rawSection builds a common EFI_SECTION_RAW record.
func rawSection(payload []byte) []byte {
	size := uint32(SectionMinLength + len(payload))
	b := make([]byte, size)
	b[0], b[1], b[2] = byte(size), byte(size>>8), byte(size>>16)
	b[3] = byte(SectionTypeRaw)
	copy(b[4:], payload)
	return b
}

// extRawSection forces the extended (8 byte) section header even for a small
// record, so tests prove the header length is taken from the raw bytes.
func extRawSection(payload []byte) []byte {
	total := uint32(SectionExtMinLength + len(payload))
	b := make([]byte, total)
	b[0], b[1], b[2] = 0xFF, 0xFF, 0xFF
	b[3] = byte(SectionTypeRaw)
	put32(b, 4, total)
	copy(b[8:], payload)
	return b
}

// guidDefinedSection builds an EFI_SECTION_GUID_DEFINED record. extraHeader is
// inserted between the 20 byte type specific header and the data, enlarging
// DataOffset accordingly.
func guidDefinedSection(g guid.GUID, processing bool, extraHeader, data []byte) []byte {
	const common = SectionMinLength
	gdLen := uint64(20)
	dataOffset := common + gdLen + uint64(len(extraHeader))
	total := dataOffset + uint64(len(data))
	b := make([]byte, total)
	put32(b, 0, uint32(total))
	b[3] = byte(SectionTypeGUIDDefined)
	putGUID(b, common, g)
	put16(b, common+16, uint16(dataOffset))
	var attrs uint16
	if processing {
		attrs = uint16(GUIDEDSectionProcessingRequired)
	}
	put16(b, common+18, attrs)
	copy(b[common+gdLen:], extraHeader)
	copy(b[dataOffset:], data)
	return b
}

// layoutBody returns an erase-polarity initialized scratch area for an FV body.
func layoutBody(size int, polarity byte) []byte {
	return bytes.Repeat([]byte{polarity}, size)
}

// fvImageSection wraps a nested volume in a firmware volume image section.
func fvImageSection(nestedFV []byte) []byte {
	total := uint32(SectionMinLength + len(nestedFV))
	b := make([]byte, total)
	put32(b, 0, total)
	b[3] = byte(SectionTypeFirmwareVolumeImage)
	copy(b[4:], nestedFV)
	return b
}

func lzmaMustEncode(t *testing.T, plain []byte) []byte {
	t.Helper()
	encoded, err := (&compression.LZMA{}).Encode(plain)
	if err != nil {
		t.Fatalf("LZMA encode failed: %v", err)
	}
	return encoded
}

func walkLayout(n *LayoutNode, fn func(*LayoutNode)) {
	fn(n)
	for _, c := range n.Children {
		walkLayout(c, fn)
	}
}

func findLayoutNode(n *LayoutNode, pred func(*LayoutNode) bool) *LayoutNode {
	var found *LayoutNode
	walkLayout(n, func(x *LayoutNode) {
		if found == nil && pred(x) {
			found = x
		}
	})
	return found
}

func nodePaths(nodes []LayoutNode) map[string]bool {
	out := map[string]bool{}
	for _, n := range nodes {
		out[n.Path] = true
	}
	return out
}

// TestFirmwareLayout exercises the read-only layout analyzer. The subtests
// combine normal and extended headers, non-zero flash bases, nested volumes of
// opposite erase polarity, transparent GUID encapsulation, LZMA decoded
// spaces and opaque/error handling.
func TestFirmwareLayout(t *testing.T) {
	const flashBase = uint64(0x7F000000)

	// The analyzer must never touch the package globals.
	savedAttrs := Attributes
	savedReadOnly := ReadOnly
	savedDisable := DisableDecompression
	t.Cleanup(func() {
		Attributes = savedAttrs
		ReadOnly = savedReadOnly
		DisableDecompression = savedDisable
	})

	t.Run("basic_records_padding_and_flash_base", func(t *testing.T) {
		// File A: 0x24 bytes with an 8 byte raw section and a 4 byte tail.
		fileABody := rawSection([]byte("DATA"))
		fileA := buildLayoutFile(*layoutTestGUID1, FVFileTypeDriver, false, 0x24, fileABody)
		// File B: opaque raw file, 0x20 bytes, lands after a 4 byte FV gap.
		fileB := buildLayoutFile(*layoutTestGUID2, FVFileTypeRaw, false, 0x20, []byte("RAW-PAYLOAD!!"))
		body := layoutBody(0x200-0x48, 0xFF)
		copy(body[0:0x24], fileA) // 0x48..0x6c; 0x6c..0x70 is alignment padding
		copy(body[0x28:0x48], fileB)
		image := buildLayoutFV(0x200, FFS2, 0x800, false, body, 0x48)

		report, index, err := ParseFirmwareLayout(image, flashBase)
		if err != nil {
			t.Fatalf("ParseFirmwareLayout: %v", err)
		}
		if report.FlashBase != flashBase || len(report.Sources) != 1 {
			t.Fatalf("unexpected report: base %#x sources %d", report.FlashBase, len(report.Sources))
		}
		root := report.Root
		if root.Record != (LayoutRange{0, 0x200}) || root.Payload != (LayoutRange{0x48, 0x200}) {
			t.Fatalf("bad root spans: record %+v payload %+v", root.Record, root.Payload)
		}
		if root.ErasePolarity != 0xFF || root.Source.ID != 0 || root.Source.Base != flashBase {
			t.Fatalf("bad root source/polarity: %+v polarity %#x", root.Source, root.ErasePolarity)
		}

		fileA24 := findLayoutNode(root, func(n *LayoutNode) bool {
			return n.Kind == LayoutNodeFile && n.GUID == layoutTestGUID1.String()
		})
		if fileA24 == nil {
			t.Fatal("file A missing")
		}
		if fileA24.Record != (LayoutRange{0x48, 0x6c}) || fileA24.Payload != (LayoutRange{0x60, 0x6c}) {
			t.Fatalf("bad file A spans: record %+v payload %+v", fileA24.Record, fileA24.Payload)
		}
		sec := findLayoutNode(fileA24, func(n *LayoutNode) bool { return n.Kind == LayoutNodeSection })
		if sec == nil || sec.Record != (LayoutRange{0x60, 0x68}) || sec.Payload != (LayoutRange{0x64, 0x68}) {
			t.Fatalf("bad section spans: %+v", sec)
		}
		tail := findLayoutNode(fileA24, func(n *LayoutNode) bool { return n.Kind == LayoutNodePadding })
		if tail == nil || tail.Record != (LayoutRange{0x68, 0x6c}) {
			t.Fatalf("bad in-file padding: %+v", tail)
		}

		gap := findLayoutNode(root, func(n *LayoutNode) bool {
			return n.Kind == LayoutNodePadding && n.Record == (LayoutRange{0x6c, 0x70})
		})
		if gap == nil {
			t.Fatal("inter-container alignment padding missing")
		}
		fileBNode := findLayoutNode(root, func(n *LayoutNode) bool { return n.GUID == layoutTestGUID2.String() })
		if fileBNode == nil || fileBNode.Record != (LayoutRange{0x70, 0x90}) {
			t.Fatalf("bad file B span: %+v", fileBNode)
		}
		free := findLayoutNode(root, func(n *LayoutNode) bool { return n.Kind == LayoutNodeFreeSpace })
		if free == nil || free.Record != (LayoutRange{0x90, 0x200}) {
			t.Fatalf("bad free space: %+v", free)
		}

		// Flash query uses physical coordinates and keeps ancestors and the
		// hit subtree, but not unrelated siblings.
		hits, err := index.Query(0, flashBase+0x64, flashBase+0x68)
		if err != nil {
			t.Fatal(err)
		}
		paths := nodePaths(hits)
		for _, want := range []string{root.Path, fileA24.Path, sec.Path} {
			if !paths[want] {
				t.Fatalf("query missing %q; got %v", want, paths)
			}
		}
		if paths[tail.Path] {
			t.Fatal("non-intersecting sibling padding must not be returned")
		}
		if paths[fileBNode.Path] {
			t.Fatal("query over section payload must not return the sibling file")
		}

		// A query covering the whole file record retains all its descendants,
		// including the in-file padding.
		fileHits, err := index.Query(0, flashBase+0x48, flashBase+0x6c)
		if err != nil {
			t.Fatal(err)
		}
		if !nodePaths(fileHits)[tail.Path] {
			t.Fatal("query over the full file record must retain descendant padding")
		}

		// A query landing only on the inter-container gap returns the gap and
		// its containers, never fabricated children.
		gapHits, err := index.Query(0, flashBase+0x6d, flashBase+0x6e)
		if err != nil {
			t.Fatal(err)
		}
		gapPaths := nodePaths(gapHits)
		if len(gapPaths) != 2 || !gapPaths[root.Path] || !gapPaths[gap.Path] {
			t.Fatalf("padding-only query returned %v, want only root and gap", gapPaths)
		}
	})

	t.Run("extended_file_and_section_headers", func(t *testing.T) {
		// Extended section header (8 bytes) carrying 0x18 bytes of payload,
		// wrapped in a large file with a 32 byte header: total 0x40.
		section := extRawSection(bytes.Repeat([]byte{0x77}, 0x18))
		if len(section) != 0x20 {
			t.Fatalf("test setup: extended section is %#x bytes", len(section))
		}
		file := buildLayoutFile(*layoutTestGUID1, FVFileTypeDriver, true, 0x40, section)
		body := layoutBody(0x300-0x60, 0xFF)
		copy(body, file)
		image := buildLayoutFV(0x300, FFS3, 0x800, true, body, 0x60)

		report, _, err := ParseFirmwareLayout(image, flashBase)
		if err != nil {
			t.Fatalf("ParseFirmwareLayout: %v", err)
		}
		root := report.Root
		if root.Payload.Start != 0x60 {
			t.Fatalf("extended FV data offset not derived from raw header: %#x", root.Payload.Start)
		}
		large := findLayoutNode(root, func(n *LayoutNode) bool { return n.Kind == LayoutNodeFile })
		if large == nil || large.Payload.Start != 0x60+0x20 {
			t.Fatalf("large file header length wrong: %+v", large)
		}
		extSec := findLayoutNode(large, func(n *LayoutNode) bool { return n.Kind == LayoutNodeSection })
		if extSec == nil {
			t.Fatal("extended section missing")
		}
		if extSec.Record != (LayoutRange{0x80, 0xa0}) || extSec.Payload != (LayoutRange{0x88, 0xa0}) {
			t.Fatalf("extended section spans wrong (header must be 8 bytes): %+v", extSec)
		}
	})

	t.Run("nested_volume_opposite_polarity", func(t *testing.T) {
		// Inner volume is erased to zero (polarity 0x00) and holds no files, so
		// its whole data region must be detected as zero-polarity free space.
		innerFV := buildLayoutFV(0x180, FFS2, 0x0000, false, nil, 0)
		imageSection := fvImageSection(innerFV)
		outerFile := buildLayoutFile(*layoutTestGUID2, FVFileTypeVolumeImage, false,
			uint64(FileHeaderMinLength+len(imageSection)), imageSection)
		body := layoutBody(0x400-0x48, 0xFF)
		copy(body, outerFile)
		image := buildLayoutFV(0x400, FFS3, 0x800, false, body, 0x48)

		report, _, err := ParseFirmwareLayout(image, flashBase)
		if err != nil {
			t.Fatalf("ParseFirmwareLayout: %v", err)
		}
		root := report.Root
		if root.ErasePolarity != 0xFF {
			t.Fatalf("outer polarity: got %#x want 0xFF", root.ErasePolarity)
		}
		inner := findLayoutNode(root, func(n *LayoutNode) bool {
			return n.Kind == LayoutNodeFirmwareVolume && n != root
		})
		if inner == nil {
			t.Fatal("nested volume missing")
		}
		if inner.ErasePolarity != 0x00 {
			t.Fatalf("nested volume must keep its own polarity, got %#x", inner.ErasePolarity)
		}
		innerFree := findLayoutNode(inner, func(n *LayoutNode) bool { return n.Kind == LayoutNodeFreeSpace })
		if innerFree == nil {
			t.Fatal("zero-polarity free space not detected inside nested volume")
		}
		if innerFree.Source.ID != 0 || innerFree.Record.Start < inner.Record.Start {
			t.Fatalf("nested free space has wrong coordinates: %+v", innerFree)
		}
		outerFree := findLayoutNode(root, func(n *LayoutNode) bool {
			return n.Kind == LayoutNodeFreeSpace && n.ErasePolarity == 0xFF
		})
		if outerFree == nil {
			t.Fatalf("outer free space wrong: %+v", outerFree)
		}
		if Attributes != savedAttrs || ReadOnly != savedReadOnly || DisableDecompression != savedDisable {
			t.Fatal("layout parsing modified package global options")
		}
	})

	t.Run("transparent_guid_extra_header_bytes", func(t *testing.T) {
		extra := bytes.Repeat([]byte{0x5a}, 8)
		child := rawSection([]byte("INSIDE!!"))
		// DataOffset is pushed past eight additional type-specific header
		// bytes; processing is not required so children stay in flash space.
		gd := guidDefinedSection(*layoutTransGUID, false, extra,
			child)
		file := buildLayoutFile(*layoutTestGUID1, FVFileTypeFreeForm, false,
			uint64(FileHeaderMinLength+len(gd)), gd)
		body := layoutBody(0x200-0x48, 0xFF)
		copy(body, file)
		image := buildLayoutFV(0x200, FFS2, 0x800, false, body, 0x48)

		report, _, err := ParseFirmwareLayout(image, flashBase)
		if err != nil {
			t.Fatalf("ParseFirmwareLayout: %v", err)
		}
		gdNode := findLayoutNode(report.Root, func(n *LayoutNode) bool {
			return n.Kind == LayoutNodeSection && n.GUID == layoutTransGUID.String()
		})
		if gdNode == nil {
			t.Fatal("transparent GUID section missing")
		}
		// Common (4) + type specific (20) + extra (8) = 32.
		if gdNode.Payload.Start-gdNode.Record.Start != 32 {
			t.Fatalf("DataOffset not honored: payload starts %#x into record",
				gdNode.Payload.Start-gdNode.Record.Start)
		}
		if gdNode.Source.Kind != LayoutSourceFlash {
			t.Fatal("transparent section must not create a decoded space")
		}
		childNode := findLayoutNode(gdNode, func(n *LayoutNode) bool { return n.Kind == LayoutNodeSection })
		if childNode == nil || childNode.Source.ID != 0 {
			t.Fatalf("encapsulated child must share flash space: %+v", childNode)
		}
		// The eight extra header bytes belong to the record, not the payload.
		if gdNode.Payload.Start <= gdNode.Record.Start+24 {
			t.Fatal("extra header bytes were misclassified as payload")
		}
	})

	t.Run("lzma_decoded_space_with_nested_volume", func(t *testing.T) {
		innerFV := buildLayoutFV(0x100, FFS2, 0x800, false, nil, 0)
		plain := append(rawSection([]byte("DECODED1")), fvImageSection(innerFV)...)
		encoded := lzmaMustEncode(t, plain)
		gd := guidDefinedSection(compression.LZMAGUID, true, nil, encoded)
		file := buildLayoutFile(*layoutTestGUID1, FVFileTypeDriver, false,
			uint64(FileHeaderMinLength+len(gd)), gd)
		body := layoutBody(0x300-0x48, 0xFF)
		copy(body, file)
		image := buildLayoutFV(0x300, FFS2, 0x800, false, body, 0x48)

		report, index, err := ParseFirmwareLayout(image, flashBase)
		if err != nil {
			t.Fatalf("ParseFirmwareLayout: %v", err)
		}
		if len(report.Sources) != 2 {
			t.Fatalf("want flash + one decoded space, got %d", len(report.Sources))
		}
		decoded := report.Sources[1]
		if decoded.Kind != LayoutSourceDecoded || decoded.Base != 0 || decoded.Size != uint64(len(plain)) {
			t.Fatalf("bad decoded source descriptor: %+v", decoded)
		}
		gdNode := findLayoutNode(report.Root, func(n *LayoutNode) bool {
			return n.Kind == LayoutNodeSection && n.GUID == compression.LZMAGUID.String()
		})
		if gdNode == nil {
			t.Fatal("LZMA section missing")
		}
		if gdNode.Source.ID != 0 || gdNode.DecodedSpaceID != 1 {
			t.Fatalf("outer LZMA container must keep its flash span: %+v", gdNode)
		}
		if decoded.ProducerPath != gdNode.Path {
			t.Fatalf("producer path %q != section path %q", decoded.ProducerPath, gdNode.Path)
		}
		if decoded.CompressedRange != gdNode.Payload {
			t.Fatalf("compressed range %+v != section payload %+v", decoded.CompressedRange, gdNode.Payload)
		}
		spaceRoot := findLayoutNode(gdNode, func(n *LayoutNode) bool {
			return n.Kind == LayoutNodeDecodedSpaceRoot
		})
		if spaceRoot == nil || spaceRoot.Source.ID != 1 || spaceRoot.Record != (LayoutRange{0, uint64(len(plain))}) {
			t.Fatalf("bad decoded space root: %+v", spaceRoot)
		}
		decodedFV := findLayoutNode(spaceRoot, func(n *LayoutNode) bool { return n.Kind == LayoutNodeFirmwareVolume })
		if decodedFV == nil || decodedFV.Source.Kind != LayoutSourceDecoded {
			t.Fatalf("nested volume inside LZMA must live in decoded space: %+v", decodedFV)
		}
		if decodedFV.Record.Start >= flashBase {
			t.Fatal("decoded node must never be assigned a flash physical address")
		}

		// Local coordinates of the decoded space: first raw section starts at 0.
		localHits, err := index.Query(1, 0, 12)
		if err != nil {
			t.Fatal(err)
		}
		localPaths := nodePaths(localHits)
		if !localPaths[spaceRoot.Path] {
			t.Fatalf("decoded-space query missing its root: %v", localPaths)
		}
		// A flash query over the compressed bytes pulls in the decoded
		// descendants as well.
		flashHits, err := index.Query(0, flashBase+gdNode.Payload.Start, flashBase+gdNode.Payload.End)
		if err != nil {
			t.Fatal(err)
		}
		if !nodePaths(flashHits)[decodedFV.Path] {
			t.Fatal("flash query over the LZMA record must retain decoded descendants")
		}
		if _, err := index.Query(99, 0, 1); err == nil {
			t.Fatal("unknown source space must error")
		}
		if _, err := index.Query(1, 4, 2); err == nil {
			t.Fatal("inverted interval must error")
		}
	})

	t.Run("two_decoded_spaces_share_local_offsets", func(t *testing.T) {
		plain := rawSection([]byte("SAMELZMA!"))
		encoded := lzmaMustEncode(t, plain)
		section1 := guidDefinedSection(compression.LZMAGUID, true, nil, encoded)
		section2 := guidDefinedSection(compression.LZMAX86GUID, true, nil, encoded)
		file1 := buildLayoutFile(*layoutTestGUID1, FVFileTypeDriver, false,
			uint64(FileHeaderMinLength+len(section1)), section1)
		file2 := buildLayoutFile(*layoutTestGUID2, FVFileTypeDriver, false,
			uint64(FileHeaderMinLength+len(section2)), section2)
		body := layoutBody(0x400-0x48, 0xFF)
		copy(body[0:], file1)
		off2 := Align8(uint64(len(file1)))
		copy(body[off2:], file2)
		image := buildLayoutFV(0x400, FFS2, 0x800, false, body, 0x48)

		report, index, err := ParseFirmwareLayout(image, flashBase)
		if err != nil {
			t.Fatalf("ParseFirmwareLayout: %v", err)
		}
		if len(report.Sources) != 3 {
			t.Fatalf("want flash + two decoded spaces, got %d", len(report.Sources))
		}
		hits1, err := index.Query(1, 0, 8)
		if err != nil {
			t.Fatal(err)
		}
		hits2, err := index.Query(2, 0, 8)
		if err != nil {
			t.Fatal(err)
		}
		p1, p2 := nodePaths(hits1), nodePaths(hits2)
		// Shared flash ancestors (the outer volume) legitimately appear in
		// both queries; nodes inside either decoded space must not alias.
		for path := range p1 {
			if !p2[path] {
				continue
			}
			if n := index.Find(path); n != nil && n.Source.ID != 0 {
				t.Fatalf("decoded-space node %q appears in both spaces; offsets must not alias", path)
			}
		}
		// Both spaces contain a section at the same local offset zero.
		if idx := findLayoutNode(report.Root, func(n *LayoutNode) bool {
			return n.Source.ID == 1 && n.Kind == LayoutNodeSection
		}); idx == nil || idx.Record.Start != 0 {
			t.Fatalf("space 1 section wrong: %+v", idx)
		}
		if idx := findLayoutNode(report.Root, func(n *LayoutNode) bool {
			return n.Source.ID == 2 && n.Kind == LayoutNodeSection
		}); idx == nil || idx.Record.Start != 0 {
			t.Fatalf("space 2 section wrong: %+v", idx)
		}
	})

	t.Run("opaque_processing_guid", func(t *testing.T) {
		gd := guidDefinedSection(*layoutOpaqueGUID, true, nil, []byte("mystery-bytes"))
		file := buildLayoutFile(*layoutTestGUID1, FVFileTypeFreeForm, false,
			uint64(FileHeaderMinLength+len(gd)), gd)
		body := layoutBody(0x200-0x48, 0xFF)
		copy(body, file)
		image := buildLayoutFV(0x200, FFS2, 0x800, false, body, 0x48)

		report, index, err := ParseFirmwareLayout(image, flashBase)
		if err != nil {
			t.Fatalf("ParseFirmwareLayout: %v", err)
		}
		opaque := findLayoutNode(report.Root, func(n *LayoutNode) bool {
			return n.Kind == LayoutNodeOpaqueGUIDed
		})
		if opaque == nil || opaque.GUID != layoutOpaqueGUID.String() {
			t.Fatalf("unknown processing GUID must stay opaque: %+v", opaque)
		}
		if len(opaque.Children) != 0 {
			t.Fatal("opaque node must not expose guessed children")
		}
		if len(report.Sources) != 1 {
			t.Fatal("opaque processing GUID must not create a decoded space")
		}
		if len(index.Sources()) != 1 {
			t.Fatal("index must be self-contained but saw extra sources")
		}
	})

	t.Run("truncation_and_bad_headers_return_located_errors", func(t *testing.T) {
		goodFile := buildLayoutFile(*layoutTestGUID1, FVFileTypeDriver, false, 0x20,
			rawSection([]byte("DATA")))
		goodBody := layoutBody(0x200-0x48, 0xFF)
		copy(goodBody, goodFile)
		goodImage := buildLayoutFV(0x200, FFS2, 0x800, false, goodBody, 0x48)

		if _, _, err := ParseFirmwareLayout(goodImage[:FirmwareVolumeMinSize-1], flashBase); err == nil {
			t.Fatal("truncated FV must error")
		}

		// A file that declares more bytes than the volume can provide.
		oversize := buildLayoutFile(*layoutTestGUID1, FVFileTypeDriver, false, 0x1000,
			rawSection([]byte("DATA")))
		badBody := layoutBody(0x200-0x48, 0xFF)
		copy(badBody, oversize)
		badImage := buildLayoutFV(0x200, FFS2, 0x800, false, badBody, 0x48)
		if _, _, err := ParseFirmwareLayout(badImage, flashBase); err == nil {
			t.Fatal("file extending past the volume must error instead of borrowing sibling bytes")
		}

		// GUID DataOffset outside its own record.
		gd := guidDefinedSection(*layoutTransGUID, false, nil, []byte("x"))
		put16(gd, 4+16, uint16(len(gd)+16)) // corrupt DataOffset
		corruptFile := buildLayoutFile(*layoutTestGUID1, FVFileTypeFreeForm, false,
			uint64(FileHeaderMinLength+len(gd)), gd)
		corruptBody := layoutBody(0x200-0x48, 0xFF)
		copy(corruptBody, corruptFile)
		corruptImage := buildLayoutFV(0x200, FFS2, 0x800, false, corruptBody, 0x48)
		if _, _, err := ParseFirmwareLayout(corruptImage, flashBase); err == nil {
			t.Fatal("out-of-range GUID DataOffset must error")
		}

		// Processing-required LZMA with undecodable bytes.
		badLZMA := guidDefinedSection(compression.LZMAGUID, true, nil, []byte{0, 1, 2, 3, 4, 5, 6, 7})
		lzmaFile := buildLayoutFile(*layoutTestGUID1, FVFileTypeDriver, false,
			uint64(FileHeaderMinLength+len(badLZMA)), badLZMA)
		lzmaBody := layoutBody(0x200-0x48, 0xFF)
		copy(lzmaBody, lzmaFile)
		lzmaImage := buildLayoutFV(0x200, FFS2, 0x800, false, lzmaBody, 0x48)
		if _, _, err := ParseFirmwareLayout(lzmaImage, flashBase); err == nil {
			t.Fatal("undecodable LZMA payload must return a located error")
		}

		// The input image is never modified, even on failure.
		if !bytes.Equal(badImage, buildLayoutFV(0x200, FFS2, 0x800, false, badBody, 0x48)) {
			t.Fatal("failed parse mutated the input image")
		}
		if !bytes.Equal(goodImage, buildLayoutFV(0x200, FFS2, 0x800, false, goodBody, 0x48)) {
			t.Fatal("parse mutated a valid input image")
		}
	})

	t.Run("index_survives_image_mutation_and_legacy_api_unchanged", func(t *testing.T) {
		file := buildLayoutFile(*layoutTestGUID1, FVFileTypeDriver, false, 0x20,
			rawSection([]byte("DATA")))
		body := layoutBody(0x200-0x48, 0xFF)
		copy(body, file)
		image := buildLayoutFV(0x200, FFS2, 0x800, false, body, 0x48)
		original := append([]byte(nil), image...)

		report, index, err := ParseFirmwareLayout(image, flashBase)
		if err != nil {
			t.Fatalf("ParseFirmwareLayout: %v", err)
		}
		before := index.Nodes()
		if len(before) == 0 {
			t.Fatal("index holds no nodes")
		}

		// The caller mutates (and even shrinks) the image after parsing.
		for i := range image {
			image[i] = 0x00
		}
		image = image[:FirmwareVolumeMinSize]
		after := index.Nodes()
		if len(after) != len(before) {
			t.Fatalf("node count changed after image mutation: %d -> %d", len(before), len(after))
		}
		hits, err := index.Query(0, flashBase+0x48, flashBase+0x68)
		if err != nil {
			t.Fatal(err)
		}
		if !nodePaths(hits)[report.Root.Path] {
			t.Fatal("indexed data must survive mutation of the caller's image")
		}

		// The legacy entry points keep their signatures and behavior. They
		// intentionally use the package-global polarity, so reset it to the
		// poisoned initial value first (other tests may have set it) and
		// snapshot/restore around the calls.
		Attributes.ErasePolarity = poisonedPolarity
		legacyRO := ReadOnly
		legacyDisable := DisableDecompression
		fv, err := NewFirmwareVolume(original, 0x100, true)
		if err != nil {
			t.Fatalf("legacy NewFirmwareVolume: %v", err)
		}
		if fv.FVOffset != 0x100 || len(fv.Files) != 1 {
			t.Fatalf("legacy FV behavior changed: offset %#x files %d", fv.FVOffset, len(fv.Files))
		}
		legacyFile, err := NewFile(original[0x48:])
		if err != nil || legacyFile == nil {
			t.Fatalf("legacy NewFile failed: %v", err)
		}
		legacySec, err := NewSection(original[0x60:], 0)
		if err != nil || legacySec == nil {
			t.Fatalf("legacy NewSection failed: %v", err)
		}
		if string(legacySec.Buf()[4:8]) != "DATA" {
			t.Fatal("legacy section payload mismatch")
		}
		Attributes = savedAttrs
		ReadOnly = legacyRO
		DisableDecompression = legacyDisable
	})
}

// ExampleParseFirmwareLayout shows the coordinate split between the flash
// space (physical addresses = flashBase + local offset) and a zero-based
// decoded space produced by an LZMA section.
func ExampleParseFirmwareLayout() {
	const base = uint64(0x100000)
	inner := buildLayoutFV(0x100, FFS2, 0x800, false, nil, 0)
	plain := fvImageSection(inner)
	encoded, err := (&compression.LZMA{}).Encode(plain)
	if err != nil {
		panic(err)
	}
	gd := guidDefinedSection(compression.LZMAGUID, true, nil, encoded)
	file := buildLayoutFile(*layoutTestGUID1, FVFileTypeDriver, false,
		uint64(FileHeaderMinLength+len(gd)), gd)
	body := layoutBody(0x300-0x48, 0xFF)
	copy(body, file)
	image := buildLayoutFV(0x300, FFS2, 0x800, false, body, 0x48)

	report, index, err := ParseFirmwareLayout(image, base)
	if err != nil {
		panic(err)
	}
	fmtNode := findLayoutNode(report.Root, func(n *LayoutNode) bool {
		return n.GUID == compression.LZMAGUID.String()
	})
	fmt.Printf("root=%s record=[%#x,%#x) sources=%d\n",
		report.Root.Kind, report.Root.Record.Start, report.Root.Record.End, len(report.Sources))
	fmt.Printf("lzma flash record=[%#x,%#x) decoded_space=%d\n",
		base+fmtNode.Record.Start, base+fmtNode.Record.End, fmtNode.DecodedSpaceID)
	decodedFV := findLayoutNode(report.Root, func(n *LayoutNode) bool {
		return n.Kind == LayoutNodeFirmwareVolume && n.Source.ID == fmtNode.DecodedSpaceID
	})
	fmt.Printf("decoded volume local record=[%#x,%#x) base=%#x\n",
		decodedFV.Record.Start, decodedFV.Record.End, decodedFV.Source.Base)
	hits, err := index.Query(fmtNode.DecodedSpaceID, 0, 0x10)
	if err != nil {
		panic(err)
	}
	fmt.Printf("decoded query hits=%d\n", len(hits))
	// Output:
	// root=FirmwareVolume record=[0x0,0x300) sources=2
	// lzma flash record=[0x100060,0x1000ba) decoded_space=1
	// decoded volume local record=[0x4,0x104) base=0x0
	// decoded query hits=7
}
