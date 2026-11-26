// Copyright 2026 the LinuxBoot Authors. All rights reserved
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package uefi

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"reflect"
	"strings"
	"testing"

	"github.com/linuxboot/fiano/pkg/compression"
	"github.com/linuxboot/fiano/pkg/guid"
)

// Test GUIDs used to synthesize firmware images.
var (
	layoutGUIDFile1  = guid.MustParse("11111111-2222-3333-4444-555555555555")
	layoutGUIDFile2  = guid.MustParse("22222222-3333-4444-5555-666666666666")
	layoutGUIDFile3  = guid.MustParse("33333333-4444-5555-6666-777777777777")
	layoutGUIDTrans  = guid.MustParse("44444444-5555-4666-8777-888888888888")
	layoutGUIDFVName = guid.MustParse("55555555-6666-4777-8888-999999999999")
)

// leBytes serializes v little-endian. Test helper.
func leBytes(v interface{}) []byte {
	var b bytes.Buffer
	if err := binary.Write(&b, binary.LittleEndian, v); err != nil {
		panic(err)
	}
	return b.Bytes()
}

// rawSectionRecord builds a section record with a normal 4-byte header.
func rawSectionRecord(stype SectionType, payload []byte) []byte {
	size := uint64(SectionMinLength) + uint64(len(payload))
	rec := leBytes(Write3Size(size))
	rec = append(rec, byte(stype))
	return append(rec, payload...)
}

// extSectionRecord builds a section record with an extended 8-byte header.
func extSectionRecord(stype SectionType, payload []byte) []byte {
	size := uint64(SectionExtMinLength) + uint64(len(payload))
	rec := leBytes(Write3Size(0xFFFFFF))
	rec = append(rec, byte(stype))
	rec = append(rec, leBytes(uint32(size))...)
	return append(rec, payload...)
}

// fileRecord builds an FFS file record (header + payload). large selects the
// extended 32-byte file header.
func fileRecord(g guid.GUID, ftype FVFileType, payload []byte, large bool) []byte {
	if large {
		size := uint64(FileHeaderExtMinLength) + uint64(len(payload))
		fh := FileHeaderExtended{
			FileHeader: FileHeader{
				GUID:       g,
				Type:       ftype,
				Attributes: fileAttr(0x01), // large file
				Size:       Write3Size(0xFFFFFF),
				State:      0xF8,
			},
			ExtendedSize: size,
		}
		return append(leBytes(fh), payload...)
	}
	size := uint64(FileHeaderMinLength) + uint64(len(payload))
	fh := FileHeader{
		GUID:  g,
		Type:  ftype,
		Size:  Write3Size(size),
		State: 0xF8,
	}
	return append(leBytes(fh), payload...)
}

// guidDefinedRecord builds a GUID-defined section record. extraHeader holds
// additional header bytes placed between the fixed GUID-defined header and
// the payload; DataOffset accounts for them.
func guidDefinedRecord(g guid.GUID, attrs uint16, extraHeader, payload []byte) []byte {
	dataOffset := uint16(SectionMinLength + guidDefinedHeaderLen + len(extraHeader))
	size := uint64(dataOffset) + uint64(len(payload))
	rec := leBytes(Write3Size(size))
	rec = append(rec, byte(SectionTypeGUIDDefined))
	rec = append(rec, leBytes(SectionGUIDDefinedHeader{
		GUID:       g,
		DataOffset: dataOffset,
		Attributes: attrs,
	})...)
	rec = append(rec, extraHeader...)
	return append(rec, payload...)
}

// fvSpec describes a synthesized firmware volume.
type fvSpec struct {
	fsGUID    guid.GUID
	attrs     uint32 // Attributes field; bit 0x800 selects 0xFF erase polarity
	extHeader bool   // include an extended FV header
	files     [][]byte
	freeTail  int    // trailing free space filled with the erase polarity
	totalLen  uint64 // total FV length; 0 means exactly fit the content
}

// fvRecord synthesizes a firmware volume image.
func fvRecord(spec fvSpec) []byte {
	polarity := byte(0)
	if spec.attrs&0x800 != 0 {
		polarity = 0xFF
	}
	headerLen := uint64(FirmwareVolumeFixedHeaderSize + 8 + 8) // fixed header + one block + terminator
	extOff, extSize := uint64(0), uint64(0)
	if spec.extHeader {
		extOff = headerLen
		extSize = FirmwareVolumeExtHeaderMinSize
		headerLen = extOff + extSize
	}
	dataOffset := Align8(headerLen)

	body := []byte{}
	for _, f := range spec.files {
		// 8-byte alignment padding between files, filled with the polarity.
		for (dataOffset+uint64(len(body)))%8 != 0 {
			body = append(body, polarity)
		}
		body = append(body, f...)
	}
	for i := 0; i < spec.freeTail; i++ {
		body = append(body, polarity)
	}
	total := dataOffset + uint64(len(body))
	if spec.totalLen != 0 {
		total = spec.totalLen
	}
	if total < dataOffset+uint64(len(body)) {
		panic("fvSpec totalLen too small for content")
	}

	hdr := FirmwareVolumeFixedHeader{
		FileSystemGUID: spec.fsGUID,
		Length:         total,
		Signature:      fvSignature,
		Attributes:     spec.attrs,
		HeaderLen:      uint16(headerLen),
		Revision:       2,
	}
	if spec.extHeader {
		hdr.ExtHeaderOffset = uint16(extOff)
	}
	rec := leBytes(hdr)
	rec = append(rec, leBytes(Block{Count: 1, Size: uint32(total)})...)
	rec = append(rec, leBytes(Block{})...)
	if spec.extHeader {
		rec = append(rec, leBytes(FirmwareVolumeExtHeader{
			FVName:        *layoutGUIDFVName,
			ExtHeaderSize: uint32(extSize),
		})...)
	}
	for uint64(len(rec)) < dataOffset {
		rec = append(rec, polarity)
	}
	rec = append(rec, body...)
	for uint64(len(rec)) < total {
		rec = append(rec, polarity)
	}
	return rec
}

// lzmaEncode compresses data with the pure-Go LZMA encoder.
func lzmaEncode(t *testing.T, data []byte) []byte {
	t.Helper()
	enc, err := (&compression.LZMA{}).Encode(data)
	if err != nil {
		t.Fatalf("LZMA encode: %v", err)
	}
	return enc
}

// nodeByPath indexes the layout nodes by their path, failing on duplicates.
func nodeByPath(t *testing.T, l *FirmwareLayout) map[string]LayoutNode {
	t.Helper()
	m := map[string]LayoutNode{}
	for _, n := range l.Nodes() {
		if _, dup := m[n.Path]; dup {
			t.Fatalf("duplicate layout path %q", n.Path)
		}
		m[n.Path] = n
	}
	return m
}

// requireNode fetches one node or fails the test.
func requireNode(t *testing.T, m map[string]LayoutNode, path string) LayoutNode {
	t.Helper()
	n, ok := m[path]
	if !ok {
		t.Fatalf("layout node %q not found", path)
	}
	return n
}

// checkSpan verifies the full and payload spans of a node.
func checkSpan(t *testing.T, n LayoutNode, full, payload LayoutSpan) {
	t.Helper()
	if n.Full != full || n.Payload != payload {
		t.Errorf("%s: got full=%+v payload=%+v, want full=%+v payload=%+v",
			n.Path, n.Full, n.Payload, full, payload)
	}
}

// queryPaths runs a layout query and returns the resulting node paths.
func queryPaths(t *testing.T, l *FirmwareLayout, space LayoutSpaceID, start, end uint64) []string {
	t.Helper()
	nodes, err := l.Query(space, start, end)
	if err != nil {
		t.Fatalf("Query(space=%d, %#x, %#x): %v", space, start, end, err)
	}
	paths := []string{}
	for _, n := range nodes {
		paths = append(paths, n.Path)
	}
	return paths
}

func TestFirmwareLayout(t *testing.T) {
	t.Run("normal_and_extended_headers", func(t *testing.T) {
		// File 0: normal 24-byte header; section 0 with a normal 4-byte
		// header, section 1 with an extended 8-byte header.
		sec0 := rawSectionRecord(SectionTypeRaw, []byte("DATA"))                  // 8 bytes
		sec1 := extSectionRecord(SectionTypePE32, bytes.Repeat([]byte{0x5A}, 12)) // 20 bytes
		file0 := fileRecord(*layoutGUIDFile1, FVFileTypeDriver, append(sec0, sec1...), false)
		// File 1: extended 32-byte header, one normal section.
		sec2 := rawSectionRecord(SectionTypeRaw, bytes.Repeat([]byte{0x11}, 8)) // 12 bytes
		file1 := fileRecord(*layoutGUIDFile2, FVFileTypeDriver, sec2, true)
		image := fvRecord(fvSpec{
			fsGUID: *FFS2, attrs: 0x800,
			files: [][]byte{file0, file1}, freeTail: 24,
		})
		// Expected geometry (flash base 0):
		//   FV header [0,72), file0 [72,124), pad [124,128),
		//   file1 [128,172), alignment pad [172,176), free tail [176,196).
		fv, layout, err := ParseFirmwareLayout(image, 0)
		if err != nil {
			t.Fatalf("ParseFirmwareLayout: %v", err)
		}
		if len(fv.Files) != 2 {
			t.Fatalf("got %d files, want 2", len(fv.Files))
		}
		if got := len(fv.Files[0].Sections); got != 2 {
			t.Fatalf("file 0 has %d sections, want 2", got)
		}
		if got := len(fv.Files[1].Sections); got != 1 {
			t.Fatalf("file 1 has %d sections, want 1", got)
		}
		if got := fv.Files[0].HeaderLen(); got != FileHeaderMinLength {
			t.Errorf("file 0 header length %d, want %d", got, FileHeaderMinLength)
		}
		if got := fv.Files[1].HeaderLen(); got != FileHeaderExtMinLength {
			t.Errorf("file 1 header length %d, want %d", got, FileHeaderExtMinLength)
		}
		if fv.FreeSpace != 20 {
			t.Errorf("free space = %#x, want 0x14", fv.FreeSpace)
		}

		nodes := nodeByPath(t, layout)
		root := requireNode(t, nodes, "FV[0]")
		checkSpan(t, root, LayoutSpan{0, 196}, LayoutSpan{72, 196})
		if root.Kind != LayoutNodeFirmwareVolume || root.TypeName != "FFS2" {
			t.Errorf("root kind/type = %v/%q", root.Kind, root.TypeName)
		}
		f0 := requireNode(t, nodes, "FV[0]/File[0]")
		checkSpan(t, f0, LayoutSpan{72, 124}, LayoutSpan{96, 124})
		if f0.GUID != layoutGUIDFile1.String() {
			t.Errorf("file 0 GUID = %q, want %q", f0.GUID, layoutGUIDFile1)
		}
		checkSpan(t, requireNode(t, nodes, "FV[0]/File[0]/Section[0]"),
			LayoutSpan{96, 104}, LayoutSpan{100, 104})
		// Extended section header: payload starts 8 bytes in.
		s1 := requireNode(t, nodes, "FV[0]/File[0]/Section[1]")
		checkSpan(t, s1, LayoutSpan{104, 124}, LayoutSpan{112, 124})
		// Extended file header: payload starts 32 bytes in.
		f1 := requireNode(t, nodes, "FV[0]/File[1]")
		checkSpan(t, f1, LayoutSpan{128, 172}, LayoutSpan{160, 172})
		checkSpan(t, requireNode(t, nodes, "FV[0]/File[1]/Section[0]"),
			LayoutSpan{160, 172}, LayoutSpan{164, 172})

		// A query that only hits the padding between files returns just the
		// container; no child is fabricated for the padding.
		if got := queryPaths(t, layout, FlashSpace, 124, 128); !reflect.DeepEqual(got, []string{"FV[0]"}) {
			t.Errorf("padding query = %v, want [FV[0]]", got)
		}
		// Same for the free tail.
		if got := queryPaths(t, layout, FlashSpace, 176, 196); !reflect.DeepEqual(got, []string{"FV[0]"}) {
			t.Errorf("free tail query = %v, want [FV[0]]", got)
		}
		// A query over a section keeps its ancestors.
		want := []string{"FV[0]", "FV[0]/File[0]", "FV[0]/File[0]/Section[0]"}
		if got := queryPaths(t, layout, FlashSpace, 100, 104); !reflect.DeepEqual(got, want) {
			t.Errorf("section query = %v, want %v", got, want)
		}
		// The whole volume returns every node in path order.
		all := []string{
			"FV[0]",
			"FV[0]/File[0]",
			"FV[0]/File[0]/Section[0]",
			"FV[0]/File[0]/Section[1]",
			"FV[0]/File[1]",
			"FV[0]/File[1]/Section[0]",
		}
		if got := queryPaths(t, layout, FlashSpace, 0, 196); !reflect.DeepEqual(got, all) {
			t.Errorf("full query = %v, want %v", got, all)
		}
	})

	t.Run("nonzero_flash_base", func(t *testing.T) {
		const base = 0xFF800000
		sec := rawSectionRecord(SectionTypeRaw, []byte("DATA"))
		file := fileRecord(*layoutGUIDFile1, FVFileTypeDriver, sec, false)
		image := fvRecord(fvSpec{
			fsGUID: *FFS2, attrs: 0x800,
			files: [][]byte{file}, freeTail: 24,
		})
		// FV [0,128), file [72,104), section [96,104), payload [100,104).
		fv, layout, err := ParseFirmwareLayout(image, base)
		if err != nil {
			t.Fatalf("ParseFirmwareLayout: %v", err)
		}
		if layout.FlashBase() != base {
			t.Errorf("FlashBase = %#x, want %#x", layout.FlashBase(), base)
		}
		spaces := layout.Spaces()
		if len(spaces) != 1 || spaces[0].ID != FlashSpace || spaces[0].Kind != LayoutSpaceFlash || spaces[0].Base != base {
			t.Fatalf("spaces = %+v", spaces)
		}
		nodes := nodeByPath(t, layout)
		checkSpan(t, requireNode(t, nodes, "FV[0]"),
			LayoutSpan{base, base + 128}, LayoutSpan{base + 72, base + 128})
		checkSpan(t, requireNode(t, nodes, "FV[0]/File[0]"),
			LayoutSpan{base + 72, base + 104}, LayoutSpan{base + 96, base + 104})
		checkSpan(t, requireNode(t, nodes, "FV[0]/File[0]/Section[0]"),
			LayoutSpan{base + 96, base + 104}, LayoutSpan{base + 100, base + 104})

		// Flash queries use the base-inclusive addresses.
		want := []string{"FV[0]", "FV[0]/File[0]"}
		if got := queryPaths(t, layout, FlashSpace, base+72, base+73); !reflect.DeepEqual(got, want) {
			t.Errorf("file header query = %v, want %v", got, want)
		}
		want = []string{"FV[0]", "FV[0]/File[0]", "FV[0]/File[0]/Section[0]"}
		if got := queryPaths(t, layout, FlashSpace, base+100, base+104); !reflect.DeepEqual(got, want) {
			t.Errorf("payload query = %v, want %v", got, want)
		}

		// The index holds its own data: corrupting the image must not
		// change query results, and the tree buffers are private copies.
		for i := range image {
			image[i] = 0xAA
		}
		if got := queryPaths(t, layout, FlashSpace, base+100, base+104); !reflect.DeepEqual(got, want) {
			t.Errorf("query after image corruption = %v, want %v", got, want)
		}
		if fv.Files[0].Buf()[0] != 0x11 {
			t.Errorf("tree buffer aliases the input image")
		}
	})

	t.Run("nested_opposite_polarity_fv", func(t *testing.T) {
		// The parser must not disturb the package-level options, no matter
		// which polarities the volumes use.
		polarityBefore := Attributes.ErasePolarity
		readOnlyBefore := ReadOnly
		decompBefore := DisableDecompression
		suppressBefore := SuppressErasePolarityError

		// Inner FV: FFS3, erase polarity 0x00 (attribute bit 0x800 clear).
		innerSec := rawSectionRecord(SectionTypeRaw, []byte("INNER")) // 9 bytes
		innerFile := fileRecord(*layoutGUIDFile3, FVFileTypeDriver, innerSec, false)
		innerFV := fvRecord(fvSpec{fsGUID: *FFS3, attrs: 0x000, files: [][]byte{innerFile}, freeTail: 16})
		// Inner geometry: header [0,72), file [72,105), zeroed free tail,
		// total 121.
		fvImageSec := rawSectionRecord(SectionTypeFirmwareVolumeImage, innerFV)
		outerFile := fileRecord(*layoutGUIDFile1, FVFileTypeVolumeImage, fvImageSec, false)
		// Outer FV: FFS2, erase polarity 0xFF.
		image := fvRecord(fvSpec{fsGUID: *FFS2, attrs: 0x800, files: [][]byte{outerFile}, freeTail: 8})
		// Outer geometry: file [72,221), free tail, total 229.

		fv, layout, err := ParseFirmwareLayout(image, 0)
		if err != nil {
			t.Fatalf("ParseFirmwareLayout: %v", err)
		}
		if len(fv.Files) != 1 || len(fv.Files[0].Sections) != 1 {
			t.Fatalf("unexpected outer tree shape")
		}
		encap := fv.Files[0].Sections[0].Encapsulated
		if len(encap) != 1 {
			t.Fatalf("FV image section has %d encapsulated entries, want 1", len(encap))
		}
		inner, ok := encap[0].Value.(*FirmwareVolume)
		if !ok {
			t.Fatalf("encapsulated value is %T, want *FirmwareVolume", encap[0].Value)
		}
		if len(inner.Files) != 1 {
			t.Fatalf("inner FV has %d files, want 1", len(inner.Files))
		}
		// The inner free tail is made of 0x00 bytes and must be detected
		// with the inner volume's own polarity.
		if inner.FreeSpace != 9 {
			t.Errorf("inner free space = %#x, want 0x9", inner.FreeSpace)
		}
		if fv.FreeSpace != 5 {
			t.Errorf("outer free space = %#x, want 0x5", fv.FreeSpace)
		}

		// The nested volume and its content live in the flash space: they
		// do have physical addresses.
		nodes := nodeByPath(t, layout)
		innerPath := "FV[0]/File[0]/Section[0]/FV[0]"
		innerNode := requireNode(t, nodes, innerPath)
		checkSpan(t, innerNode, LayoutSpan{100, 221}, LayoutSpan{172, 221})
		if innerNode.Space != FlashSpace || innerNode.TypeName != "FFS3" {
			t.Errorf("inner FV space/type = %d/%q", innerNode.Space, innerNode.TypeName)
		}
		checkSpan(t, requireNode(t, nodes, innerPath+"/File[0]"),
			LayoutSpan{172, 205}, LayoutSpan{196, 205})
		checkSpan(t, requireNode(t, nodes, innerPath+"/File[0]/Section[0]"),
			LayoutSpan{196, 205}, LayoutSpan{200, 205})

		// Global options are untouched, and parses are independent.
		if Attributes.ErasePolarity != polarityBefore {
			t.Errorf("Attributes.ErasePolarity changed: %#x -> %#x", polarityBefore, Attributes.ErasePolarity)
		}
		if ReadOnly != readOnlyBefore || DisableDecompression != decompBefore ||
			SuppressErasePolarityError != suppressBefore {
			t.Errorf("global options changed: ReadOnly=%v DisableDecompression=%v SuppressErasePolarityError=%v",
				ReadOnly, DisableDecompression, SuppressErasePolarityError)
		}
		if _, _, err := ParseFirmwareLayout(image, 0); err != nil {
			t.Errorf("second parse of the same image: %v", err)
		}
		// A 0x00-polarity root volume followed by a 0xFF-polarity one must
		// not trip the global polarity conflict check either.
		zeroImage := fvRecord(fvSpec{fsGUID: *FFS2, attrs: 0x000, files: [][]byte{innerFile}, freeTail: 8})
		if _, _, err := ParseFirmwareLayout(zeroImage, 0); err != nil {
			t.Errorf("0x00-polarity root volume: %v", err)
		}
		if _, _, err := ParseFirmwareLayout(image, 0); err != nil {
			t.Errorf("0xFF-polarity root volume after 0x00 one: %v", err)
		}
	})

	t.Run("transparent_guid_extra_header", func(t *testing.T) {
		// A GUID-defined section without the processing-required bit keeps
		// its content in the parent space. The 5 extra authenticated header
		// bytes belong to the section header, not to the payload, and make
		// DataOffset land on a non-4-aligned offset.
		extra := bytes.Repeat([]byte{0xA5}, 5)
		child1 := rawSectionRecord(SectionTypeRaw, []byte("PAY1")) // 8 bytes
		child2 := rawSectionRecord(SectionTypeRaw, []byte("PAY2")) // 8 bytes
		guidSec := guidDefinedRecord(*layoutGUIDTrans, 0, extra, append(child1, child2...))
		file := fileRecord(*layoutGUIDFile1, FVFileTypeDriver, guidSec, false)
		image := fvRecord(fvSpec{fsGUID: *FFS2, attrs: 0x800, files: [][]byte{file}, freeTail: 8})
		// File [72,141), GUID section [96,141), DataOffset 29 -> payload
		// [125,141), child sections [125,133) and [133,141) (aligned
		// relative to the payload start, not the stream).

		fv, layout, err := ParseFirmwareLayout(image, 0)
		if err != nil {
			t.Fatalf("ParseFirmwareLayout: %v", err)
		}
		sec := fv.Files[0].Sections[0]
		if sec.TypeSpecific == nil {
			t.Fatalf("GUID-defined section has no type-specific header")
		}
		gd, ok := sec.TypeSpecific.Header.(*SectionGUIDDefined)
		if !ok {
			t.Fatalf("type-specific header is %T", sec.TypeSpecific.Header)
		}
		if gd.GUID != *layoutGUIDTrans {
			t.Errorf("encapsulation GUID = %v, want %v", gd.GUID, layoutGUIDTrans)
		}
		if gd.DataOffset != 29 {
			t.Errorf("DataOffset = %d, want 29", gd.DataOffset)
		}
		if len(sec.Encapsulated) != 2 {
			t.Fatalf("transparent GUID section has %d children, want 2", len(sec.Encapsulated))
		}

		nodes := nodeByPath(t, layout)
		guidNode := requireNode(t, nodes, "FV[0]/File[0]/Section[0]")
		checkSpan(t, guidNode, LayoutSpan{96, 141}, LayoutSpan{125, 141})
		if guidNode.GUID != layoutGUIDTrans.String() {
			t.Errorf("node GUID = %q, want %q", guidNode.GUID, layoutGUIDTrans)
		}
		childNode1 := requireNode(t, nodes, "FV[0]/File[0]/Section[0]/Section[0]")
		checkSpan(t, childNode1, LayoutSpan{125, 133}, LayoutSpan{129, 133})
		childNode2 := requireNode(t, nodes, "FV[0]/File[0]/Section[0]/Section[1]")
		checkSpan(t, childNode2, LayoutSpan{133, 141}, LayoutSpan{137, 141})
		if childNode1.Space != FlashSpace || childNode2.Space != FlashSpace {
			t.Errorf("transparent GUID children in spaces %d/%d, want flash space",
				childNode1.Space, childNode2.Space)
		}
		// No decoded space was created.
		if got := len(layout.Spaces()); got != 1 {
			t.Errorf("got %d spaces, want 1", got)
		}
		// The extra header bytes belong to the encapsulating section only.
		want := []string{"FV[0]", "FV[0]/File[0]", "FV[0]/File[0]/Section[0]"}
		if got := queryPaths(t, layout, FlashSpace, 120, 125); !reflect.DeepEqual(got, want) {
			t.Errorf("extra header query = %v, want %v", got, want)
		}
	})

	t.Run("lzma_nested_structure", func(t *testing.T) {
		// The decoded LZMA stream contains a raw section and an FV image
		// section holding a complete nested volume.
		deepSec := rawSectionRecord(SectionTypeRaw, []byte("DEEP")) // 8 bytes
		deepFile := fileRecord(*layoutGUIDFile2, FVFileTypeDriver, deepSec, false)
		innerFV := fvRecord(fvSpec{fsGUID: *FFS2, attrs: 0x800, files: [][]byte{deepFile}, freeTail: 8})
		// Inner FV: total 112.
		fvImageSec := rawSectionRecord(SectionTypeFirmwareVolumeImage, innerFV) // 116 bytes
		decoded := append(rawSectionRecord(SectionTypeRaw, []byte("LZMA-DATA-00")), fvImageSec...)
		// Decoded stream: raw section [0,16), FV image section [16,132).
		lzma := lzmaEncode(t, decoded)
		guidSec := guidDefinedRecord(compression.LZMAGUID, uint16(GUIDEDSectionProcessingRequired), nil, lzma)
		file := fileRecord(*layoutGUIDFile1, FVFileTypeDriver, guidSec, false)
		image := fvRecord(fvSpec{fsGUID: *FFS2, attrs: 0x800, files: [][]byte{file}, freeTail: 8})

		fv, layout, err := ParseFirmwareLayout(image, 0)
		if err != nil {
			t.Fatalf("ParseFirmwareLayout: %v", err)
		}
		secSize := uint64(24 + len(lzma)) // DataOffset 24 + compressed payload

		// The decoded stream became its own zero-based source space.
		spaces := layout.Spaces()
		if len(spaces) != 2 {
			t.Fatalf("got %d spaces, want 2", len(spaces))
		}
		sp1 := spaces[1]
		if sp1.Kind != LayoutSpaceDecoded || sp1.Base != 0 || sp1.Size != uint64(len(decoded)) {
			t.Errorf("decoded space = %+v", sp1)
		}
		if sp1.ProducerPath != "FV[0]/File[0]/Section[0]" {
			t.Errorf("producer = %q", sp1.ProducerPath)
		}
		if sp1.Parent != FlashSpace {
			t.Errorf("parent space = %d, want flash space", sp1.Parent)
		}
		wantCompressed := LayoutSpan{120, 120 + uint64(len(lzma))}
		if sp1.Compressed != wantCompressed {
			t.Errorf("compressed span = %+v, want %+v", sp1.Compressed, wantCompressed)
		}

		nodes := nodeByPath(t, layout)
		// The encapsulating section itself keeps its physical span.
		guidPath := "FV[0]/File[0]/Section[0]"
		guidNode := requireNode(t, nodes, guidPath)
		checkSpan(t, guidNode, LayoutSpan{96, 96 + secSize}, LayoutSpan{120, 96 + secSize})
		if guidNode.Space != FlashSpace {
			t.Errorf("GUID section in space %d, want flash space", guidNode.Space)
		}
		// The decoded descendants only have local offsets in space 1.
		checkSpan(t, requireNode(t, nodes, guidPath+"/Section[0]"),
			LayoutSpan{0, 16}, LayoutSpan{4, 16})
		checkSpan(t, requireNode(t, nodes, guidPath+"/Section[1]"),
			LayoutSpan{16, 132}, LayoutSpan{20, 132})
		innerPath := guidPath + "/Section[1]/FV[0]"
		checkSpan(t, requireNode(t, nodes, innerPath),
			LayoutSpan{20, 132}, LayoutSpan{92, 132})
		checkSpan(t, requireNode(t, nodes, innerPath+"/File[0]"),
			LayoutSpan{92, 124}, LayoutSpan{116, 124})
		deepNode := requireNode(t, nodes, innerPath+"/File[0]/Section[0]")
		checkSpan(t, deepNode, LayoutSpan{116, 124}, LayoutSpan{120, 124})
		for path, n := range nodes {
			if strings.HasPrefix(path, guidPath+"/") && n.Space != sp1.ID {
				t.Errorf("decoded descendant %s in space %d, want %d", path, n.Space, sp1.ID)
			}
		}

		// The tree carries the nested structure as well.
		gd := fv.Files[0].Sections[0].TypeSpecific.Header.(*SectionGUIDDefined)
		if gd.Compression != "LZMA" {
			t.Errorf("compression = %q, want LZMA", gd.Compression)
		}
		if got := len(fv.Files[0].Sections[0].Encapsulated); got != 2 {
			t.Fatalf("decoded section count = %d, want 2", got)
		}
		fvImage, ok := fv.Files[0].Sections[0].Encapsulated[1].Value.(*Section)
		if !ok {
			t.Fatalf("second decoded child is %T, want *Section", fv.Files[0].Sections[0].Encapsulated[1].Value)
		}
		inner, ok := fvImage.Encapsulated[0].Value.(*FirmwareVolume)
		if !ok || len(inner.Files) != 1 {
			t.Fatalf("nested FV in decoded space malformed: ok=%v files=%d", ok, len(inner.Files))
		}

		// Decoded-space queries use local coordinates and stay in space 1.
		if got := queryPaths(t, layout, sp1.ID, 0, 4); !reflect.DeepEqual(got, []string{guidPath + "/Section[0]"}) {
			t.Errorf("decoded query = %v", got)
		}
		// Flash queries over the compressed bytes see the container but
		// never the decoded descendants.
		want := []string{"FV[0]", "FV[0]/File[0]", guidPath}
		if got := queryPaths(t, layout, FlashSpace, 120, 121); !reflect.DeepEqual(got, want) {
			t.Errorf("compressed query = %v, want %v", got, want)
		}
		if got := queryPaths(t, layout, FlashSpace, 0, uint64(len(image))); len(got) != 3 {
			t.Errorf("full flash query returned %d nodes, want 3: %v", len(got), got)
		}
	})

	t.Run("identical_decoded_offsets", func(t *testing.T) {
		// Two LZMA encapsulations decode to streams whose first section
		// sits at the same local offset with the same size. The two spaces
		// must stay separate.
		dec1 := rawSectionRecord(SectionTypeRaw, []byte("PAYLOAD-1")) // 13 bytes
		dec2 := rawSectionRecord(SectionTypeRaw, []byte("PAYLOAD-2")) // 13 bytes
		lz1 := lzmaEncode(t, dec1)
		lz2 := lzmaEncode(t, dec2)
		sec1 := guidDefinedRecord(compression.LZMAGUID, uint16(GUIDEDSectionProcessingRequired), nil, lz1)
		sec2 := guidDefinedRecord(compression.LZMAGUID, uint16(GUIDEDSectionProcessingRequired), nil, lz2)
		file1 := fileRecord(*layoutGUIDFile1, FVFileTypeDriver, sec1, false)
		file2 := fileRecord(*layoutGUIDFile2, FVFileTypeDriver, sec2, false)
		image := fvRecord(fvSpec{fsGUID: *FFS2, attrs: 0x800, files: [][]byte{file1, file2}, freeTail: 8})

		_, layout, err := ParseFirmwareLayout(image, 0)
		if err != nil {
			t.Fatalf("ParseFirmwareLayout: %v", err)
		}
		spaces := layout.Spaces()
		if len(spaces) != 3 {
			t.Fatalf("got %d spaces, want 3", len(spaces))
		}
		if spaces[1].ProducerPath != "FV[0]/File[0]/Section[0]" ||
			spaces[2].ProducerPath != "FV[0]/File[1]/Section[0]" {
			t.Errorf("producers = %q / %q", spaces[1].ProducerPath, spaces[2].ProducerPath)
		}
		if spaces[1].Compressed == spaces[2].Compressed {
			t.Errorf("compressed spans of distinct encapsulations coincide: %+v", spaces[1].Compressed)
		}

		path1 := "FV[0]/File[0]/Section[0]/Section[0]"
		path2 := "FV[0]/File[1]/Section[0]/Section[0]"
		if got := queryPaths(t, layout, spaces[1].ID, 0, 13); !reflect.DeepEqual(got, []string{path1}) {
			t.Errorf("space 1 query = %v, want [%s]", got, path1)
		}
		if got := queryPaths(t, layout, spaces[2].ID, 0, 13); !reflect.DeepEqual(got, []string{path2}) {
			t.Errorf("space 2 query = %v, want [%s]", got, path2)
		}
		// Identical local offsets in different spaces refer to different
		// nodes and must not be merged.
		nodes := nodeByPath(t, layout)
		n1 := requireNode(t, nodes, path1)
		n2 := requireNode(t, nodes, path2)
		if n1.Full != n2.Full {
			t.Errorf("local spans differ: %+v vs %+v", n1.Full, n2.Full)
		}
		if n1.Full != (LayoutSpan{0, 13}) {
			t.Errorf("local span = %+v, want [0,13)", n1.Full)
		}
		if n1.Space == n2.Space {
			t.Errorf("identical decoded offsets share space %d", n1.Space)
		}
	})

	t.Run("recursive_decoded_spaces", func(t *testing.T) {
		// A decompressed stream may itself contain another compressed
		// encapsulation; each decode creates its own zero-based space.
		inner := rawSectionRecord(SectionTypeRaw, []byte("DEEP")) // 8 bytes
		innerLZ := lzmaEncode(t, inner)
		middle := guidDefinedRecord(compression.LZMAGUID, uint16(GUIDEDSectionProcessingRequired), nil, innerLZ)
		middleLZ := lzmaEncode(t, middle)
		outer := guidDefinedRecord(compression.LZMAGUID, uint16(GUIDEDSectionProcessingRequired), nil, middleLZ)
		file := fileRecord(*layoutGUIDFile1, FVFileTypeDriver, outer, false)
		image := fvRecord(fvSpec{fsGUID: *FFS2, attrs: 0x800, files: [][]byte{file}, freeTail: 8})

		_, layout, err := ParseFirmwareLayout(image, 0)
		if err != nil {
			t.Fatalf("ParseFirmwareLayout: %v", err)
		}
		spaces := layout.Spaces()
		if len(spaces) != 3 {
			t.Fatalf("got %d spaces, want 3", len(spaces))
		}
		// Space 1 holds the middle encapsulation, space 2 its payload.
		if spaces[1].Parent != FlashSpace || spaces[2].Parent != spaces[1].ID {
			t.Errorf("space parents = %d, %d", spaces[1].Parent, spaces[2].Parent)
		}
		if spaces[2].ProducerPath != "FV[0]/File[0]/Section[0]/Section[0]" {
			t.Errorf("space 2 producer = %q", spaces[2].ProducerPath)
		}
		// The compressed span of the inner encapsulation is expressed in
		// the local coordinates of space 1, not in flash addresses.
		middleDataOff := uint64(SectionMinLength + guidDefinedHeaderLen)
		wantCompressed := LayoutSpan{middleDataOff, middleDataOff + uint64(len(innerLZ))}
		if spaces[2].Compressed != wantCompressed {
			t.Errorf("space 2 compressed span = %+v, want %+v", spaces[2].Compressed, wantCompressed)
		}
		// The innermost section sits at local offset 0 of space 2.
		deepPath := "FV[0]/File[0]/Section[0]/Section[0]/Section[0]"
		if got := queryPaths(t, layout, spaces[2].ID, 0, 8); !reflect.DeepEqual(got, []string{deepPath}) {
			t.Errorf("space 2 query = %v, want [%s]", got, deepPath)
		}
		nodes := nodeByPath(t, layout)
		checkSpan(t, requireNode(t, nodes, deepPath), LayoutSpan{0, 8}, LayoutSpan{4, 8})
	})

	t.Run("locatable_errors", func(t *testing.T) {
		sec := rawSectionRecord(SectionTypeRaw, []byte("DATA"))
		file := fileRecord(*layoutGUIDFile1, FVFileTypeDriver, sec, false)
		good := fvRecord(fvSpec{fsGUID: *FFS2, attrs: 0x800, files: [][]byte{file}, freeTail: 24})
		// good: FV [0,128), file [72,104), section [96,104).

		// A declared FV length beyond the end of the image.
		_, _, err := ParseFirmwareLayout(good[:100], 0)
		var le *LayoutError
		if err == nil || !errors.As(err, &le) || le.Path != "FV[0]" {
			t.Errorf("truncated image: err=%v", err)
		}

		// A file whose declared size runs past the end of the volume.
		badFile := append([]byte(nil), good...)
		badFile[72+20], badFile[72+21], badFile[72+22] = 0xF0, 0xFF, 0xFF // Size = 0xFFFFF0
		if _, _, err := ParseFirmwareLayout(badFile, 0); err == nil ||
			!strings.Contains(err.Error(), "FV[0]/File[0]") {
			t.Errorf("oversized file: err=%v", err)
		}

		// A section whose declared size runs past the end of its file.
		badSec := append([]byte(nil), good...)
		badSec[96], badSec[97], badSec[98] = 0xF0, 0xFF, 0xFF // Size = 0xFFFFF0
		if _, _, err := ParseFirmwareLayout(badSec, 0); err == nil ||
			!strings.Contains(err.Error(), "FV[0]/File[0]/Section[0]") {
			t.Errorf("oversized section: err=%v", err)
		}

		// A flash base that overflows when the image size is added.
		if _, _, err := ParseFirmwareLayout(good, ^uint64(0)-10); err == nil ||
			!strings.Contains(err.Error(), "overflow") {
			t.Errorf("address overflow: err=%v", err)
		}

		// A non-erased tail too short for a file header.
		badTail := fvRecord(fvSpec{fsGUID: *FFS2, attrs: 0x800, files: [][]byte{file}, freeTail: 8})
		badTail[104] = 0x00 // 8 bytes left, not erased
		if _, _, err := ParseFirmwareLayout(badTail, 0); err == nil ||
			!strings.Contains(err.Error(), "FV[0]/File[1]") || !strings.Contains(err.Error(), "truncated") {
			t.Errorf("truncated file header: err=%v", err)
		}

		// A GUID-defined DataOffset beyond the end of its section.
		badGD := guidDefinedRecord(*layoutGUIDTrans, 0, nil, []byte("PAY1"))
		badGD[20], badGD[21] = 0xFF, 0xFF // DataOffset = 0xFFFF
		badGDFile := fileRecord(*layoutGUIDFile1, FVFileTypeDriver, badGD, false)
		badGDImage := fvRecord(fvSpec{fsGUID: *FFS2, attrs: 0x800, files: [][]byte{badGDFile}, freeTail: 8})
		if _, _, err := ParseFirmwareLayout(badGDImage, 0); err == nil ||
			!strings.Contains(err.Error(), "DataOffset") {
			t.Errorf("bad DataOffset: err=%v", err)
		}

		// The failures above must not affect a subsequent good parse.
		if _, _, err := ParseFirmwareLayout(good, 0); err != nil {
			t.Errorf("parse after errors: %v", err)
		}
	})
}

// ExampleParseFirmwareLayout parses a small synthesized volume and prints
// its layout report. Spans are half-open and expressed in the node's source
// space; the flash space includes the flash base passed to
// ParseFirmwareLayout.
func ExampleParseFirmwareLayout() {
	payload := rawSectionRecord(SectionTypeRaw, []byte("DATA"))
	file := fileRecord(*layoutGUIDFile1, FVFileTypeDriver, payload, false)
	image := fvRecord(fvSpec{
		fsGUID: *FFS2, attrs: 0x800,
		files: [][]byte{file}, freeTail: 24,
	})
	fv, layout, err := ParseFirmwareLayout(image, 0x1000)
	if err != nil {
		panic(err)
	}
	for _, n := range layout.Nodes() {
		fmt.Println(n)
	}
	fmt.Printf("free tail: %#x\n", fv.FreeSpace)
	// Output:
	// FV[0] FirmwareVolume space=0 full=[0x1000,0x1080) payload=[0x1048,0x1080)
	// FV[0]/File[0] File space=0 full=[0x1048,0x1068) payload=[0x1060,0x1068)
	// FV[0]/File[0]/Section[0] Section space=0 full=[0x1060,0x1068) payload=[0x1064,0x1068)
	// free tail: 0x18
}
