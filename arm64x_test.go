// Copyright 2018 Saferwall. All rights reserved.
// Use of this source code is governed by Apache v2 license
// license that can be found in the LICENSE file.

package pe_test

import (
	"bytes"
	"encoding/binary"
	"errors"
	"reflect"
	"strings"
	"testing"

	pe "github.com/saferwall/pe"
	"github.com/saferwall/pe/task_support/fixture"
	"github.com/saferwall/pe/task_support/taskcheck"
)

// Fixture landmarks (see task_support/fixture/fixture.go).
const (
	fixtureSize           = 0xe00
	dvrtTableOffset       = 0xa40
	loadConfigOffset      = 0x600
	nativeExportDirRVA    = 0x2200
	alternateExportDirRVA = 0x2300
	sectionDataRaw        = 0x600
)

func put16(b []byte, offset int, value uint16) {
	binary.LittleEndian.PutUint16(b[offset:], value)
}

func put32(b []byte, offset int, value uint32) {
	binary.LittleEndian.PutUint32(b[offset:], value)
}

func put64(b []byte, offset int, value uint64) {
	binary.LittleEndian.PutUint64(b[offset:], value)
}

func cloneImage(b []byte) []byte {
	return append([]byte(nil), b...)
}

func fixtureBytes() []byte {
	return cloneImage(fixture.Image())
}

// encodedFixup is a declarative symbol-6 fixup used by the test image builder.
type encodedFixup struct {
	page    uint32
	offset  uint16 // page-relative byte offset
	typ     uint8  // 0 ZEROFILL, 1 VALUE, 2 DELTA
	arg     uint8  // 2-bit argument field
	payload []byte // VALUE bytes or the 2-byte DELTA magnitude
}

// buildARM64XImage returns a fixture-based image whose DVRT holds one symbol-6
// record containing the given fixups, grouped into page blocks in order.
func buildARM64XImage(fixups []encodedFixup) []byte {
	b := fixtureBytes()
	for i := dvrtTableOffset; i < fixtureSize; i++ {
		b[i] = 0
	}

	type pageBlock struct {
		page uint32
		body []byte
	}
	var (
		blocks     []pageBlock
		pageOrder  []uint32
		blockIndex = map[uint32]int{}
	)
	for _, fx := range fixups {
		idx, ok := blockIndex[fx.page]
		if !ok {
			idx = len(blocks)
			blockIndex[fx.page] = idx
			pageOrder = append(pageOrder, fx.page)
			blocks = append(blocks, pageBlock{page: fx.page})
		}
		word := fx.offset&0x0fff | uint16(fx.typ)<<12 | uint16(fx.arg)<<14
		var wordBytes [2]byte
		binary.LittleEndian.PutUint16(wordBytes[:], word)
		blocks[idx].body = append(blocks[idx].body, wordBytes[:]...)
		blocks[idx].body = append(blocks[idx].body, fx.payload...)
	}

	var payload bytes.Buffer
	for _, page := range pageOrder {
		block := blocks[blockIndex[page]]
		blockSize := uint32(8 + len(block.body))
		if blockSize%4 != 0 {
			block.body = append(block.body, 0, 0)
			blockSize += 2
		}
		var header [8]byte
		binary.LittleEndian.PutUint32(header[0:], page)
		binary.LittleEndian.PutUint32(header[4:], blockSize)
		payload.Write(header[:])
		payload.Write(block.body)
	}

	recordSize := uint32(payload.Len()) // BaseRelocSize of the symbol-6 record
	put32(b, dvrtTableOffset, 1)        // DVRT version 1
	put32(b, dvrtTableOffset+4, 12+recordSize)
	put64(b, dvrtTableOffset+8, 6) // symbol 6
	put32(b, dvrtTableOffset+16, recordSize)
	copy(b[dvrtTableOffset+20:], payload.Bytes())
	return b
}

// buildLegacyImage packs symbol 3/4/5 DVRT records (no symbol 6) plus a
// regular base-relocation directory into the fixture layout.
func buildLegacyImage() []byte {
	b := fixtureBytes()
	for i := dvrtTableOffset; i < fixtureSize; i++ {
		b[i] = 0
	}

	// Regular base relocation directory: data directory index 5, one block at
	// RVA 0x3100 (file offset 0xb00) with a HIGHLOW entry plus padding.
	put32(b, 0x108+5*8, 0x3100)
	put32(b, 0x10c+5*8, 16)
	put32(b, 0xb00, 0x1000) // page RVA
	put32(b, 0xb04, 16)     // block size
	put16(b, 0xb08, 0x3000) // IMAGE_REL_BASED_HIGHLOW, offset 0
	put16(b, 0xb0a, 0)      // padding

	block := func(page uint32, words ...uint16) []byte {
		size := 8 + 2*len(words)
		out := make([]byte, size)
		binary.LittleEndian.PutUint32(out[0:], page)
		binary.LittleEndian.PutUint32(out[4:], uint32(size))
		for i, w := range words {
			binary.LittleEndian.PutUint16(out[8+i*2:], w)
		}
		return out
	}

	// Three records, each with its 12-byte header followed by one page block.
	records := []struct {
		symbol uint64
		block  []byte
	}{
		// Symbol 3: one 4-byte entry (IAT index 2, CALL, page offset 0x10).
		{3, block(0x1000, 0x5010, 0x0000)},
		// Symbol 4: CFG-check branch at page offset 0x20 plus zero padding.
		{4, block(0x1000, 0x4020, 0)},
		// Symbol 5: switchable branch, register 5, page offset 0x30.
		{5, block(0x1000, 0x5030, 0)},
	}

	cur := dvrtTableOffset + 8
	for _, rec := range records {
		put64(b, cur, rec.symbol)
		put32(b, cur+8, uint32(len(rec.block)))
		copy(b[cur+12:], rec.block)
		cur += 12 + len(rec.block)
	}

	put32(b, dvrtTableOffset, 1) // DVRT v1
	put32(b, dvrtTableOffset+4, uint32(cur-(dvrtTableOffset+8)))
	return b
}

// buildPE32Image builds a minimal valid PE32 image (magic 0x10b) with one
// section and no data directories.
func buildPE32Image() []byte {
	b := make([]byte, 0x400)
	put16(b, 0, 0x5a4d)
	put32(b, 0x3c, 0x80)
	put32(b, 0x80, 0x4550)
	put16(b, 0x84, uint16(pe.ImageFileMachineI386))
	put16(b, 0x86, 1)
	put16(b, 0x94, 0xe0)
	put16(b, 0x96, 0x2022)
	put16(b, 0x98, uint16(pe.ImageNtOptionalHeader32Magic))
	put32(b, 0xb4, 0x400000) // ImageBase, 64 KiB aligned
	put32(b, 0xb8, 0x1000)   // SectionAlignment
	put32(b, 0xbc, 0x200)    // FileAlignment
	put32(b, 0xd0, 0x3000)   // SizeOfImage
	put32(b, 0xd4, 0x200)    // SizeOfHeaders
	put16(b, 0xdc, 3)        // Subsystem: console
	put32(b, 0xf4, 16)       // NumberOfRvaAndSizes
	copy(b[0x178:], ".text")
	put32(b, 0x180, 0x200) // VirtualSize
	put32(b, 0x184, 0x1000)
	put32(b, 0x188, 0x200) // SizeOfRawData
	put32(b, 0x18c, 0x200) // PointerToRawData
	put32(b, 0x19c, 0x40000040)
	return b
}

func parseBytes(t *testing.T, b []byte, opts *pe.Options) *pe.File {
	t.Helper()
	f, err := pe.NewBytes(b, opts)
	if err != nil {
		t.Fatalf("NewBytes failed: %v", err)
	}
	return f
}

// TestARM64XRecords covers the mixed variable-length symbol-6 records of the
// fixture: header fixups, section fixups, VALUE/ZEROFILL/DELTA semantics and
// visibility of the records from a native-mode parse.
func TestARM64XRecords(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		b := fixtureBytes()
		f := parseBytes(t, b, &pe.Options{})
		err := f.Parse()
		c.Check(err == nil, "native parse failed: %v", err)

		c.Check(f.LoadConfig.DVRT != nil, "native parse must expose the DVRT")
		if f.LoadConfig.DVRT == nil {
			return
		}
		dvrt := f.LoadConfig.DVRT
		c.Check(dvrt.Version == 1, "DVRT version = %d, want 1", dvrt.Version)
		c.Check(dvrt.Size == 56, "DVRT size = %d, want 56", dvrt.Size)
		c.Check(len(dvrt.Entries) == 1, "DVRT entries = %d, want 1", len(dvrt.Entries))

		reloc, ok := dvrt.Entries[0].ImageDynamicRelocation.(pe.ImageDynamicRelocation64)
		c.Check(ok, "symbol-6 record must use the PE32+ header")
		c.Check(reloc.Symbol == 6, "record symbol = %d, want 6", reloc.Symbol)
		c.Check(reloc.BaseRelocSize == 44, "base reloc size = %d, want 44", reloc.BaseRelocSize)
		c.Check(len(dvrt.Entries[0].RelocBlocks) == 2,
			"page blocks = %d, want 2", len(dvrt.Entries[0].RelocBlocks))
		c.Check(dvrt.Entries[0].RelocBlocks[0].ImgBaseReloc.VirtualAddress == 0,
			"first block page RVA mismatch")
		c.Check(dvrt.Entries[0].RelocBlocks[1].ImgBaseReloc.VirtualAddress == 0x2000,
			"second block page RVA mismatch")

		fixups := dvrt.ARM64XFixups
		c.Check(len(fixups) == 5, "fixup count = %d, want 5", len(fixups))
		if len(fixups) != 5 {
			return
		}

		// Records stay in strict file order.
		c.Check(fixups[0].SourceOffset == 0xa5c && fixups[1].SourceOffset == 0xa60 &&
			fixups[2].SourceOffset == 0xa66 && fixups[3].SourceOffset == 0xa74 &&
			fixups[4].SourceOffset == 0xa7e, "fixup source offsets out of order: %+v", fixups)

		// Header-target VALUE fixups (machine and entry point).
		c.Check(fixups[0].TargetRVA == 0x84 && fixups[0].Type == pe.ARM64XFixupValue &&
			fixups[0].Width == 2 && fixups[0].Value == 0x8664,
			"machine fixup mismatch: %+v", fixups[0])
		c.Check(fixups[1].TargetRVA == 0xa8 && fixups[1].Type == pe.ARM64XFixupValue &&
			fixups[1].Width == 4 && fixups[1].Value == 0x1020,
			"entry point fixup mismatch: %+v", fixups[1])

		// DELTA: magnitude 0x40, scale 4, addition -> +256 on a 32-bit target.
		c.Check(fixups[2].TargetRVA == 0x108 && fixups[2].Type == pe.ARM64XFixupDelta &&
			fixups[2].Width == 4 && fixups[2].Delta == 256,
			"delta fixup mismatch: %+v", fixups[2])

		// Section-target VALUE8 and ZEROFILL8 inside .data.
		c.Check(fixups[3].TargetRVA == 0x21f0 && fixups[3].Type == pe.ARM64XFixupValue &&
			fixups[3].Width == 8 && fixups[3].Value == 0x0102030405060708,
			"section VALUE fixup mismatch: %+v", fixups[3])
		c.Check(fixups[4].TargetRVA == 0x21f8 && fixups[4].Type == pe.ARM64XFixupZerofill &&
			fixups[4].Width == 8,
			"section ZEROFILL fixup mismatch: %+v", fixups[4])

		// Legacy symbol 3/4/5 records and ordinary base relocations still parse.
		legacy := buildLegacyImage()
		lf := parseBytes(t, legacy, &pe.Options{})
		c.Check(lf.Parse() == nil, "legacy DVRT parse failed")
		c.Check(lf.LoadConfig.DVRT != nil && len(lf.LoadConfig.DVRT.Entries) == 3,
			"legacy DVRT entries missing")
		if lf.LoadConfig.DVRT != nil && len(lf.LoadConfig.DVRT.Entries) == 3 {
			symbols := make([]uint64, 3)
			for i, entry := range lf.LoadConfig.DVRT.Entries {
				reloc, ok := entry.ImageDynamicRelocation.(pe.ImageDynamicRelocation64)
				c.Check(ok, "legacy record %d must use PE32+ header", i)
				symbols[i] = reloc.Symbol
				c.Check(len(entry.RelocBlocks) == 1, "legacy record %d block count", i)
			}
			c.Check(symbols[0] == 3 && symbols[1] == 4 && symbols[2] == 5,
				"legacy symbols = %v, want [3 4 5]", symbols)

			imp, ok := lf.LoadConfig.DVRT.Entries[0].RelocBlocks[0].TypeOffsets[0].(pe.ImageImportControlTransferDynamicRelocation)
			c.Check(ok && imp.IATIndex == 2 && imp.IndirectCall == 1 &&
				imp.PageRelativeOffset == 0x10, "symbol-3 entry mismatch: %+v", imp)
			ind, ok := lf.LoadConfig.DVRT.Entries[1].RelocBlocks[0].TypeOffsets[0].(pe.ImageIndirectControlTransferDynamicRelocation)
			c.Check(ok && ind.CfgCheck == 1 && ind.PageRelativeOffset == 0x20,
				"symbol-4 entry mismatch: %+v", ind)
			sw, ok := lf.LoadConfig.DVRT.Entries[2].RelocBlocks[0].TypeOffsets[0].(pe.ImageSwitchableBranchDynamicRelocation)
			c.Check(ok && sw.RegisterNumber == 5 && sw.PageRelativeOffset == 0x30,
				"symbol-5 entry mismatch: %+v", sw)
		}
		c.Check(len(lf.Relocations) == 1,
			"ordinary base relocations must keep parsing")
		if len(lf.Relocations) == 1 {
			// Padding words are kept as Absolute entries; the first word must
			// remain the HIGHLOW fixup.
			c.Check(lf.Relocations[0].Entries[0].Type == pe.ImageRelBasedHighLow,
				"base reloc type = %d, want HIGHLOW", lf.Relocations[0].Entries[0].Type)
			c.Check(lf.Relocations[0].Entries[0].Offset == 0,
				"base reloc offset = %d, want 0", lf.Relocations[0].Entries[0].Offset)
			c.Check(lf.Relocations[0].Data.VirtualAddress == 0x1000,
				"base reloc page RVA mismatch")
		}

		// DELTA chains use the previously corrected value and wrap modulo 2^32.
		chain := buildARM64XImage([]encodedFixup{
			// Replace the native 0x1000 at header RVA 0xa8 with 0x5000.
			{page: 0, offset: 0xa8, typ: 1, arg: 2, payload: []byte{0x00, 0x50, 0x00, 0x00}},
			// Add 0x100 (magnitude 0x40 * scale 4), reading the corrected 0x5000.
			{page: 0, offset: 0xa8, typ: 2, arg: 0, payload: []byte{0x40, 0x00}},
			// Subtract 0x80 (bit14 set, magnitude 0x20 * scale 4).
			{page: 0, offset: 0xa8, typ: 2, arg: 1, payload: []byte{0x20, 0x00}},
		})
		chf := parseBytes(t, chain, &pe.Options{ARM64XAlternate: true})
		c.Check(chf.Parse() == nil, "delta chain parse failed")
		got, err := chf.GetData(0xa8, 4)
		c.Check(err == nil, "GetData delta target failed: %v", err)
		c.Check(bytes.Equal(got, []byte{0x80, 0x50, 0x00, 0x00}),
			"delta chain result = % x, want 0x5080", got)

		// 32-bit wraparound: native 0xfffffff0 in .data (file 0x610) plus
		// 0x40 wraps to 0x30.
		wrap := buildARM64XImage([]encodedFixup{
			{page: 0x2000, offset: 0x10, typ: 2, arg: 0, payload: []byte{0x10, 0x00}},
		})
		put32(wrap, 0x610, 0xfffffff0)
		wf := parseBytes(t, wrap, &pe.Options{ARM64XAlternate: true})
		c.Check(wf.Parse() == nil, "wrap parse failed")
		got, err = wf.GetData(0x2010, 4)
		c.Check(err == nil, "GetData wrap target failed: %v", err)
		c.Check(bytes.Equal(got, []byte{0x30, 0x00, 0x00, 0x00}),
			"wrapped delta result = % x, want 0x30", got)
	})
}

// TestARM64XViews covers the header/export linkage between the native and the
// compatible view, including Fast mode and the no-symbol-6 fallback.
func TestARM64XViews(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		b := fixtureBytes()

		native := parseBytes(t, b, &pe.Options{})
		c.Check(native.Parse() == nil, "native parse failed")
		alt := parseBytes(t, b, &pe.Options{ARM64XAlternate: true})
		c.Check(alt.Parse() == nil, "alternate parse failed")

		c.Check(uint16(native.NtHeader.FileHeader.Machine) == 0xaa64,
			"native machine = %x", native.NtHeader.FileHeader.Machine)
		c.Check(uint16(alt.NtHeader.FileHeader.Machine) == 0x8664,
			"alternate machine = %x, want x64", alt.NtHeader.FileHeader.Machine)

		nativeOH := native.NtHeader.OptionalHeader.(pe.ImageOptionalHeader64)
		altOH := alt.NtHeader.OptionalHeader.(pe.ImageOptionalHeader64)
		c.Check(nativeOH.AddressOfEntryPoint == 0x1000, "native entry point changed")
		c.Check(altOH.AddressOfEntryPoint == 0x1020,
			"alternate entry point = %x, want 0x1020", altOH.AddressOfEntryPoint)
		c.Check(nativeOH.DataDirectory[pe.ImageDirectoryEntryExport].VirtualAddress ==
			nativeExportDirRVA, "native export directory RVA changed")
		c.Check(altOH.DataDirectory[pe.ImageDirectoryEntryExport].VirtualAddress ==
			alternateExportDirRVA, "alternate export directory RVA = %x",
			altOH.DataDirectory[pe.ImageDirectoryEntryExport].VirtualAddress)

		// The corrected export directory must drive the normal export parser.
		c.Check(len(native.Export.Functions) == 1 &&
			native.Export.Functions[0].Name == "NativeSymbol",
			"native export mismatch: %+v", native.Export.Functions)
		c.Check(len(alt.Export.Functions) == 1 &&
			alt.Export.Functions[0].Name == "AlternateSymbol" &&
			alt.Export.Functions[0].FunctionRVA == 0x1020,
			"alternate export mismatch: %+v", alt.Export.Functions)

		// GetData and the RVA helpers read the corrected bytes uniformly.
		machineBytes, err := alt.GetData(0x84, 2)
		c.Check(err == nil && bytes.Equal(machineBytes, []byte{0x64, 0x86}),
			"alternate machine bytes = % x, err %v", machineBytes, err)
		entryBytes, err := alt.GetData(0xa8, 4)
		c.Check(err == nil && bytes.Equal(entryBytes, []byte{0x20, 0x10, 0x00, 0x00}),
			"alternate entry bytes = % x", entryBytes)
		c.Check(alt.GetOffsetFromRva(0x21f0) == 0x7f0,
			"section RVA mapping must follow the native layout")
		sectionBytes, err := alt.GetData(0x21f8, 8)
		c.Check(err == nil && bytes.Equal(sectionBytes, make([]byte, 8)),
			"ZEROFILL target = % x", sectionBytes)

		// Fast mode still builds the compatible header, just skips directories.
		fast := parseBytes(t, b, &pe.Options{Fast: true, ARM64XAlternate: true})
		c.Check(fast.Parse() == nil, "fast alternate parse failed")
		c.Check(uint16(fast.NtHeader.FileHeader.Machine) == 0x8664,
			"fast mode must still expose the compatible machine")
		c.Check(!fast.HasExport, "fast mode must not parse the export directory")
		c.Check(len(fast.Sections) == 3, "fast mode must keep the native section layout")

		// An image without symbol-6 records returns the native view unchanged.
		legacy := buildLegacyImage()
		fallback := parseBytes(t, legacy, &pe.Options{ARM64XAlternate: true})
		c.Check(fallback.Parse() == nil, "fallback parse failed")
		c.Check(uint16(fallback.NtHeader.FileHeader.Machine) == 0xaa64,
			"no-symbol-6 image must keep the native machine")
		c.Check(len(fallback.Export.Functions) == 1 &&
			fallback.Export.Functions[0].Name == "NativeSymbol",
			"no-symbol-6 image must keep the native export")
		fallbackBytes, err := fallback.GetData(0x84, 2)
		c.Check(err == nil && bytes.Equal(fallbackBytes, []byte{0x64, 0xaa}),
			"no-symbol-6 view must expose native bytes: % x", fallbackBytes)
	})
}

// TestARM64XReparse covers repeated Parse calls and input sharing between a
// native and a compatible File over the same byte slice.
func TestARM64XReparse(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		b := fixtureBytes()
		snapshot := cloneImage(b)

		native := parseBytes(t, b, &pe.Options{})
		c.Check(native.Parse() == nil, "native parse failed")
		alt := parseBytes(t, b, &pe.Options{ARM64XAlternate: true})
		c.Check(alt.Parse() == nil, "first alternate parse failed")

		// Parse the alternate File a second time: DELTA must not accumulate
		// again and every corrected field must match the first run.
		c.Check(alt.Parse() == nil, "second alternate parse failed")
		c.Check(uint16(alt.NtHeader.FileHeader.Machine) == 0x8664,
			"machine changed after reparse: %x", alt.NtHeader.FileHeader.Machine)
		c.Check(alt.NtHeader.OptionalHeader.(pe.ImageOptionalHeader64).
			AddressOfEntryPoint == 0x1020, "entry point changed after reparse")
		c.Check(len(alt.Export.Functions) == 1 &&
			alt.Export.Functions[0].Name == "AlternateSymbol",
			"export changed after reparse: %+v", alt.Export.Functions)
		deltaBytes, err := alt.GetData(0x108, 4)
		c.Check(err == nil, "delta target read failed: %v", err)
		c.Check(bytes.Equal(deltaBytes, []byte{0x00, 0x23, 0x00, 0x00}),
			"DELTA accumulated twice: % x, want 0x2300", deltaBytes)

		// A third Parse still yields the same result.
		c.Check(alt.Parse() == nil, "third alternate parse failed")
		deltaBytes, _ = alt.GetData(0x108, 4)
		c.Check(bytes.Equal(deltaBytes, []byte{0x00, 0x23, 0x00, 0x00}),
			"DELTA accumulated on the third parse: % x", deltaBytes)

		// The native File sharing the input slice never sees corrected bytes.
		c.Check(uint16(native.NtHeader.FileHeader.Machine) == 0xaa64,
			"native machine leaked from alternate File: %x",
			native.NtHeader.FileHeader.Machine)
		c.Check(len(native.Export.Functions) == 1 &&
			native.Export.Functions[0].Name == "NativeSymbol",
			"native export leaked from alternate File")
		nativeMachine, err := native.GetData(0x84, 2)
		c.Check(err == nil && bytes.Equal(nativeMachine, []byte{0x64, 0xaa}),
			"native header bytes leaked: % x", nativeMachine)
		nativeSection, err := native.GetData(0x21f0, 8)
		c.Check(err == nil && bytes.Equal(nativeSection,
			[]byte{0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}),
			"native section bytes leaked: % x", nativeSection)

		// A freshly built native File over the same input agrees.
		other := parseBytes(t, b, &pe.Options{})
		c.Check(other.Parse() == nil, "second native parse failed")
		c.Check(uint16(other.NtHeader.FileHeader.Machine) == 0xaa64,
			"new native File saw corrected bytes")
		c.Check(other.GetOffsetFromRva(0x84) == 0x84,
			"native RVA mapping changed")

		// Neither the shared slice nor its backing array changed.
		c.Check(bytes.Equal(b, snapshot), "input slice was modified")
		c.Check(binary.LittleEndian.Uint16(b[0x84:]) == 0xaa64,
			"input machine bytes were modified")
	})
}

// TestARM64XErrors covers truncated payloads, illegal records, bad block
// lengths, unmappable targets and unsupported images/tables.
func TestARM64XErrors(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		expectError := func(name string, b []byte, wantFragment string) {
			f := parseBytes(t, b, &pe.Options{ARM64XAlternate: true})
			err := f.Parse()
			c.Check(err != nil, "%s: expected error, got nil", name)
			if err != nil {
				c.Check(strings.Contains(err.Error(), wantFragment),
					"%s: error %q does not contain %q", name, err, wantFragment)
			}
			// A failed parse must leave no corrected bytes behind: a fresh
			// native File over the same input reads the original machine.
			native := parseBytes(t, b, &pe.Options{})
			c.Check(native.Parse() == nil, "%s: native re-parse failed", name)
			c.Check(uint16(native.NtHeader.FileHeader.Machine) == 0xaa64,
				"%s: failed parse left corrected bytes behind", name)
		}

		// PE32 images are explicitly rejected.
		pe32 := buildPE32Image()
		f32 := parseBytes(t, pe32, &pe.Options{ARM64XAlternate: true})
		err32 := f32.Parse()
		c.Check(errors.Is(err32, pe.ErrARM64XUnsupported),
			"PE32 parse error = %v, want ErrARM64XUnsupported", err32)

		// DVRT versions other than 1 are rejected, citing the table offset.
		badVersion := fixtureBytes()
		put32(badVersion, dvrtTableOffset, 2)
		expectError("dvrt v2", badVersion, "0xa40")

		// A section number that does not exist cannot locate the table.
		missingSection := fixtureBytes()
		field := reflect.TypeOf(pe.ImageLoadConfigDirectory64{}).Field(35) // ...Section
		put16(missingSection, loadConfigOffset+int(field.Offset), 99)
		expectError("missing table section", missingSection, "out of range")

		// Truncated image: the declared payload runs past end of file.
		expectError("truncated payload", fixtureBytes()[:0xa60], "truncated")

		// Illegal fixup type 3, with its source offset reported.
		badType := fixtureBytes()
		put16(badType, 0xa5c, 0x7084)
		expectError("illegal fixup type", badType, "0xa5c")

		// Page block smaller than its 8-byte header.
		badBlock := fixtureBytes()
		put32(badBlock, 0xa58, 6)
		expectError("bad block size", badBlock, "0xa58")

		// Header-adjacent target with no raw-backed mapping.
		unmapped := fixtureBytes()
		put16(unmapped, 0xa5c, 0x5fff) // page offset 0xfff, VALUE width 2
		expectError("unmapped target", unmapped, "0xa5c")

		// Section target straddling the end of .data's raw range.
		straddle := fixtureBytes()
		put16(straddle, 0xa74, 0xd3fc) // RVA 0x23fc, VALUE width 8
		expectError("straddling target", straddle, "raw file mapping")

		// VALUE with argument 0 has no legal width.
		badWidth := fixtureBytes()
		put16(badWidth, 0xa5c, 0x1084) // VALUE, argument 0
		expectError("bad width argument", badWidth, "0xa5c")

		// VALUE payload cut short inside its page block.
		shortValue := buildARM64XImage([]encodedFixup{
			{page: 0, offset: 0x90, typ: 1, arg: 3, payload: []byte{0x01, 0x02}},
		})
		expectError("truncated VALUE payload", shortValue, "truncated VALUE")

		// Every failure leaves the original input untouched.
		clean := fixtureBytes()
		snapshot := cloneImage(clean)
		badType2 := cloneImage(clean)
		put16(badType2, 0xa5c, 0x7084)
		f := parseBytes(t, badType2, &pe.Options{ARM64XAlternate: true})
		c.Check(f.Parse() != nil, "corrupt image must fail")
		c.Check(badType2[0xa5c] == 0x84 && badType2[0xa5d] == 0x70,
			"corrupt parse modified its input")
		c.Check(bytes.Equal(clean, snapshot), "sibling input was modified")
	})
}
