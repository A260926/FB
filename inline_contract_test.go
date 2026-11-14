/*Copyright (C) 2024 Mandiant, Inc. All Rights Reserved.*/

package main

// Inline recovery contract tests.
//
// Two independent harnesses are used:
//
//   - A hand-built 64-bit little-endian ELF image carrying compact, fully
//     specified pclntab/moduledata/gofunc metadata. Expected chains are taken
//     from the construction below (hardcoded PCs, names and lines), never from
//     the methods under test.
//   - The stripped samples produced from task_support/fixture/main.go, driven
//     through the real analyzer command line for ELF, PE and Mach-O.
//
// The pclntab is self-contained: every sub-table offset in its header is
// relative to the header start. Blob layout (VA = SegmentVA + blob offset):
//
//	0x000 pclntab header    0x048 funcnametab   0x0c0 filetab
//	0x100 cutab             0x120 pcdata tables 0x200 functab/gofunc
//	0x218 _func record      0x280 inlinedCall tree
//	0x300 textsect entry    0x320 moduledata    0x600 text bytes
//
// The physical function main.physical starts at SegmentVA (entryOff 0) and
// spans 0x100 bytes. Its inline tree holds four records:
//
//	r0 main.leaf   parentPc 0x18 startLine 100   (sibling call A)
//	r1 main.leaf   parentPc 0x28 startLine 100   (sibling call B, same name)
//	r2 main.middle parentPc 0x38 startLine 50
//	r3 main.inner  parentPc 0x34 startLine 70    (nested inside r2's body)
//
// Every parentPc is positioned where PCDATA_InlTreeIndex already selects the
// parent layer: r3's parent sits in r2's region, the other parents sit in a
// physical (-1) region.
//
// PCDATA_InlTreeIndex intervals (value active for [prev boundary, boundary)):
//
//	[E,0x10) -1 | [0x10,0x18) r0 | [0x18,0x20) -1 | [0x20,0x28) r1 |
//	[0x28,0x30) -1 | [0x30,0x38) r2 | [0x38,0x40) -1 |
//	[0x40,0x48) r3 | [0x48,...) -1
//
// pcln line boundaries: 0x10->201 0x18->202 0x20->210 0x28->211 0x30->220
//
//	0x34->221 0x38->222 0x40->301 0x48->302 0x50->303 0x70->401
//
// pcfile boundaries: 0x30->file0 0x50->file1 0x70->file0.

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/mandiant/GoReSym/debug/gosym"
	"github.com/mandiant/GoReSym/objfile"
	"github.com/mandiant/GoReSym/task_support/taskcheck"
)

const (
	segVA    uint64 = 0x12340000
	blobSize uint64 = 0x900

	pclnHeaderOff  uint64 = 0x000
	funcNameTabOff uint64 = 0x048
	fileTabOff     uint64 = 0x0c0
	cuTabOff       uint64 = 0x100
	pcTabOff       uint64 = 0x120
	funcTabOff     uint64 = 0x200
	funcRecordOff  uint64 = 0x218
	inlTreeOff     uint64 = 0x280
	textSectOff    uint64 = 0x300
	moduleDataOff  uint64 = 0x320
	textBytesOff   uint64 = 0x600
)

// syntheticOptions toggles the edge-case images.
type syntheticOptions struct {
	funcdataAbsent bool // FUNCDATA_InlTree slot holds 0xffffffff
	funcdataZero   bool // FUNCDATA_InlTree slot holds a valid zero offset
	pcdataAbsent   bool // PCDATA_InlTreeIndex slot offset is zero
	goFuncUnmapped bool // gofunc VA points outside the loadable segment
	badNameOffset  bool // an inlined record nameOff is out of funcnametab
	parentEscapes  bool // an inlined record parentPc leaves the physical function
	cyclicParent   bool // parent re-query selects the same record again
}

func va(blobOff uint64) uint64 { return segVA + blobOff }

// zigzag encodes a signed pctab delta.
func zigzag(d int32) uint32 {
	if d >= 0 {
		return uint32(d) << 1
	}
	return uint32(^d)<<1 | 1
}

func appendVarint(b []byte, v uint32) []byte {
	for v >= 0x80 {
		b = append(b, byte(v)|0x80)
		v >>= 7
	}
	return append(b, byte(v))
}

// pcTable emits a pctab value table. Each step is (boundary PC delta, value),
// starting from val=-1 at the function entry. Consecutive equal values are
// merged because a zero value-delta is the table terminator.
func pcTable(deltas []struct {
	pcDelta uint32
	value   int32
}) []byte {
	type step struct {
		pcDelta uint32
		value   int32
	}
	var merged []step
	for _, d := range deltas {
		if n := len(merged); n > 0 && merged[n-1].value == d.value {
			merged[n-1].pcDelta += d.pcDelta
			continue
		}
		merged = append(merged, step{d.pcDelta, d.value})
	}
	var b []byte
	val := int32(-1)
	for _, d := range merged {
		b = appendVarint(b, zigzag(d.value-val))
		b = appendVarint(b, d.pcDelta)
		val = d.value
	}
	b = append(b, 0) // terminator: uvdelta == 0 on a non-first step
	return b
}

// buildSyntheticELF writes the compact metadata image to dir and returns its path.
func buildSyntheticELF(t *testing.T, dir string, opt syntheticOptions) string {
	t.Helper()
	blob := make([]byte, blobSize)
	le := binary.LittleEndian

	putU32 := func(off uint64, v uint32) { le.PutUint32(blob[off:], v) }
	putU64 := func(off uint64, v uint64) { le.PutUint64(blob[off:], v) }

	// ---- funcnametab (NUL separated) ----
	names := []string{"main.physical", "main.leaf", "main.middle", "main.inner"}
	nameOff := make(map[string]uint32, len(names))
	var cursor uint32
	for _, n := range names {
		nameOff[n] = cursor
		copy(blob[funcNameTabOff+uint64(cursor):], n)
		cursor += uint32(len(n)) + 1
	}

	// ---- filetab + cutab (one CU, two files) ----
	file0, file1 := "a/physical.go", "b/inline.go"
	copy(blob[fileTabOff:], file0)
	file1Off := uint32(len(file0) + 1)
	copy(blob[fileTabOff+uint64(file1Off):], file1)
	putU32(cuTabOff, 0)
	putU32(cuTabOff+4, file1Off)

	// ---- pctab region: pcfile, pcln, inline index, in that order ----
	pcfileTab := pcTable([]struct {
		pcDelta uint32
		value   int32
	}{{0x30, 0}, {0x20, 1}, {0xb0, 0}})
	pclnTab := pcTable([]struct {
		pcDelta uint32
		value   int32
	}{
		{0x10, 201}, {0x08, 202}, {0x08, 210}, {0x08, 211},
		{0x08, 220}, {0x04, 221}, {0x04, 222},
		{0x08, 301}, {0x08, 302}, {0x08, 303}, {0x90, 401},
	})
	inlIndexTab := pcTable([]struct {
		pcDelta uint32
		value   int32
	}{
		{0x10, -1}, {0x08, 0}, {0x08, -1}, {0x08, 1},
		{0x08, -1}, {0x08, 2}, {0x08, -1}, {0x08, 3},
		{0xb8, -1},
	})
	pcfileOff := uint32(2) // offset 0 is the pctab "absent" marker; pad two bytes
	pclnOff := pcfileOff + uint32(len(pcfileTab))
	inlIndexOff := pclnOff + uint32(len(pclnTab))
	copy(blob[pcTabOff+uint64(pcfileOff):], pcfileTab)
	copy(blob[pcTabOff+uint64(pclnOff):], pclnTab)
	copy(blob[pcTabOff+uint64(inlIndexOff):], inlIndexTab)

	// ---- functab: one function, entryOff 0, funcOff 24; closing pair ----
	putU32(funcTabOff+0, 0)
	putU32(funcTabOff+4, 24)
	putU32(funcTabOff+8, 0x100)
	putU32(funcTabOff+12, 0)

	// ---- _func record (44 byte prefix + 3 PCDATA + 4 FUNCDATA slots) ----
	fr := funcRecordOff
	putU32(fr+0, 0) // entryOff
	putU32(fr+4, nameOff["main.physical"])
	putU32(fr+8, 0)  // args
	putU32(fr+12, 0) // deferreturn
	putU32(fr+16, 0) // pcsp (absent)
	putU32(fr+20, pcfileOff)
	putU32(fr+24, pclnOff)
	putU32(fr+28, 3) // npcdata
	putU32(fr+32, 0) // cuOffset
	putU32(fr+36, 7) // startLine
	blob[fr+40] = 0  // funcID
	blob[fr+41] = 0  // flag
	blob[fr+42] = 0  // pad
	blob[fr+43] = 4  // nfuncdata
	pcBase := fr + 44
	putU32(pcBase+0, 0) // PCDATA 0 absent
	putU32(pcBase+4, 0) // PCDATA 1 absent
	if opt.pcdataAbsent {
		putU32(pcBase+8, 0)
	} else {
		putU32(pcBase+8, inlIndexOff)
	}
	fdBase := pcBase + 12
	putU32(fdBase+0, 0xffffffff) // FUNCDATA 0 absent
	putU32(fdBase+4, 0xffffffff) // FUNCDATA 1 absent
	putU32(fdBase+8, 0xffffffff) // FUNCDATA 2 absent
	switch {
	case opt.funcdataAbsent:
		putU32(fdBase+12, 0xffffffff)
	case opt.funcdataZero:
		putU32(fdBase+12, 0) // valid zero offset: tree sits at gofunc itself
	default:
		putU32(fdBase+12, uint32(inlTreeOff-funcTabOff))
	}

	// ---- inlinedCall tree (four 16 byte records) ----
	leafName := nameOff["main.leaf"]
	middleName := nameOff["main.middle"]
	innerName := nameOff["main.inner"]
	records := []struct {
		nameOff             uint32
		parentPc, startLine int32
	}{
		{leafName, 0x18, 100},
		{leafName, 0x28, 100},
		{middleName, 0x38, 50},
		{innerName, 0x34, 70},
	}
	for i, r := range records {
		base := inlTreeOff + uint64(i)*16
		blob[base+0], blob[base+1], blob[base+2], blob[base+3] = 0, 0, 0, 0
		putU32(base+4, r.nameOff)
		putU32(base+8, uint32(r.parentPc))
		putU32(base+12, uint32(r.startLine))
	}
	if opt.badNameOffset {
		putU32(inlTreeOff+3*16+4, 0x7fffffff) // r3, reached by the nested query
	}
	if opt.parentEscapes {
		putU32(inlTreeOff+3*16+8, 0x0fff) // r3 parentPc beyond the 0x100 byte function
	}
	if opt.cyclicParent {
		// r3 parentPc points inside r3's own body, so re-querying the index at
		// the parent position selects record 3 again.
		putU32(inlTreeOff+3*16+8, 0x44)
	}

	// ---- textsect map entry (single text section) ----
	// textAddr() ignores the map when there is only one text section, so the
	// base address stays zero; a real VA here would also collide with the
	// moduledata scan needle before the moduledata itself.
	putU64(textSectOff+0, 0)      // vaddr
	putU64(textSectOff+8, 0x1000) // end
	putU64(textSectOff+16, 0)     // baseaddr

	// ---- moduledata, 1.22/1.26-style offsets used by the generic parser ----
	goFuncVA := va(funcTabOff)
	switch {
	case opt.goFuncUnmapped:
		goFuncVA = 0xffff0000
	case opt.funcdataZero:
		// A zero FUNCDATA offset addresses gofunc itself, so anchor gofunc at
		// the tree rather than the functab.
		goFuncVA = va(inlTreeOff)
	}
	putU64(moduleDataOff+0, va(pclnHeaderOff)) // pcHeader pointer (moduledata scan anchor)
	putU64(moduleDataOff+128, va(funcTabOff))  // ftab.Data
	putU64(moduleDataOff+136, 1)               // ftab.Len
	putU64(moduleDataOff+144, 1)               // ftab.Cap
	putU64(moduleDataOff+160, segVA)           // minpc
	putU64(moduleDataOff+176, segVA)           // text
	putU64(moduleDataOff+296, va(fileTabOff))  // types
	putU64(moduleDataOff+304, va(cuTabOff))    // etypes
	putU64(moduleDataOff+320, goFuncVA)        // gofunc
	putU64(moduleDataOff+336, va(textSectOff)) // textsectmap.Data
	putU64(moduleDataOff+344, 1)               // textsectmap.Len
	putU64(moduleDataOff+360, va(fileTabOff))  // typelinks.Data
	putU64(moduleDataOff+368, 0)               // typelinks.Len
	putU64(moduleDataOff+384, va(fileTabOff))  // itablinks.Data
	putU64(moduleDataOff+392, 0)               // itablinks.Len

	// ---- pclntab header (magic 0xfffffff1, quantum 1, ptrsize 8) ----
	header := []byte{0xf1, 0xff, 0xff, 0xff, 0x00, 0x00, 0x01, 0x08}
	words := []uint64{
		1,              // nfunc
		2,              // nfiletab
		0,              // unused
		funcNameTabOff, // funcnametab (header-relative)
		cuTabOff,       // cutab
		fileTabOff,     // filetab
		pcTabOff,       // pctab
		funcTabOff,     // funcdata/functab
	}
	for _, w := range words {
		header = le.AppendUint64(header, w)
	}
	copy(blob[pclnHeaderOff:], header)

	// Embed a build version string for diagnostic purposes.
	copy(blob[textBytesOff:], "go1.26 ")

	return writeELF64(t, dir, blob)
}

// writeELF64 wraps blob in a minimal little-endian ELF64 executable with one
// LOAD segment and one allocated section.
func writeELF64(t *testing.T, dir string, blob []byte) string {
	t.Helper()
	const (
		ehdrSize = 64
		phdrSize = 56
		shdrSize = 64
	)
	shdrOff := uint64(ehdrSize + phdrSize)
	dataOff := uint64(0x100)

	var f []byte
	put := func(off uint64, b []byte) {
		if uint64(len(f)) < off+uint64(len(b)) {
			grown := make([]byte, off+uint64(len(b)))
			copy(grown, f)
			f = grown
		}
		copy(f[off:], b)
	}
	putU64 := func(off uint64, v uint64) {
		b := make([]byte, 8)
		binary.LittleEndian.PutUint64(b, v)
		put(off, b)
	}
	putU32 := func(off uint64, v uint32) {
		b := make([]byte, 4)
		binary.LittleEndian.PutUint32(b, v)
		put(off, b)
	}
	putU16 := func(off uint64, v uint16) {
		b := make([]byte, 2)
		binary.LittleEndian.PutUint16(b, v)
		put(off, b)
	}

	// ELF header
	put(0, []byte{0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0})
	putU16(16, 2)        // ET_EXEC
	putU16(18, 62)       // EM_X86_64
	putU32(20, 1)        // EV_CURRENT
	putU64(24, segVA)    // e_entry
	putU64(32, ehdrSize) // e_phoff
	putU64(40, shdrOff)  // e_shoff
	putU32(48, 0)        // e_flags
	putU16(52, ehdrSize) // e_ehsize
	putU16(54, phdrSize) // e_phentsize
	putU16(56, 1)        // e_phnum
	putU16(58, shdrSize) // e_shentsize
	putU16(60, 2)        // e_shnum
	putU16(62, 0)        // e_shstrndx = SHN_UNDEF

	// Program header: one PT_LOAD covering the blob
	ph := uint64(ehdrSize)
	putU32(ph+0, 1) // PT_LOAD
	putU32(ph+4, 5) // PF_R | PF_X
	putU64(ph+8, dataOff)
	putU64(ph+16, segVA)
	putU64(ph+24, segVA)
	putU64(ph+32, uint64(len(blob)))
	putU64(ph+40, uint64(len(blob)))
	putU64(ph+48, 0x1000)

	// Section 0 is the null header. Section 1 covers the whole segment.
	sh := shdrOff + shdrSize
	putU32(sh+0, 0) // sh_name
	putU32(sh+4, 1) // sh_type SHT_PROGBITS
	putU64(sh+8, 2) // sh_flags SHF_ALLOC
	putU64(sh+16, segVA)
	putU64(sh+24, dataOff)
	putU64(sh+32, uint64(len(blob)))
	putU32(sh+40, 0) // sh_link
	putU32(sh+44, 0) // sh_info
	putU64(sh+48, 1) // sh_addralign
	putU64(sh+56, 0) // sh_entsize

	put(dataOff, blob)

	path := filepath.Join(dir, "synthetic")
	if err := os.WriteFile(path, f, 0o755); err != nil {
		t.Fatalf("write synthetic elf: %v", err)
	}
	return path
}

// openSynthetic parses the compact image through the real object file path and
// returns the final table/module candidates plus an open File.
func openSynthetic(t *testing.T, path string) (*objfile.File, *gosym.Table, *objfile.ModuleData) {
	t.Helper()
	f, err := objfile.Open(path)
	if err != nil {
		t.Fatalf("objfile.Open: %v", err)
	}
	ch, err := f.PCLineTable("1.26", 0, segVA)
	if err != nil {
		t.Fatalf("PCLineTable: %v", err)
	}
	var tab *gosym.Table
	for cand := range ch {
		tab = cand.ParsedPclntab
	}
	if tab == nil {
		t.Fatalf("no pclntab candidate parsed from synthetic image")
	}
	_, module, err := f.ModuleDataTable(va(pclnHeaderOff), "1.26", "1.20", true, true)
	if err != nil {
		t.Fatalf("ModuleDataTable: %v", err)
	}
	return f, tab, module
}

func TestInlineChain(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dir := t.TempDir()
		path := buildSyntheticELF(t, dir, syntheticOptions{})
		f, tab, module := openSynthetic(t, path)
		defer f.Close()

		entry := segVA

		// Nested chain: innermost first, physical function last.
		nested, err := f.InlineFrames(tab, module, entry+0x44)
		c.Check(err == nil, "nested chain query failed: %v", err)
		c.Check(len(nested) == 3, "nested chain depth: got %d want 3", len(nested))
		if len(nested) == 3 {
			c.Check(nested[0].Name == "main.inner" && nested[0].Inlined, "frame 0 must be inlined inner, got %+v", nested[0])
			c.Check(nested[1].Name == "main.middle" && nested[1].Inlined, "frame 1 must be inlined middle, got %+v", nested[1])
			c.Check(nested[2].Name == "main.physical" && !nested[2].Inlined, "frame 2 must be physical, got %+v", nested[2])
			c.Check(nested[0].PC == entry+0x34, "inner call PC: got %#x want %#x", nested[0].PC, entry+0x34)
			c.Check(nested[1].PC == entry+0x38, "middle call PC: got %#x want %#x", nested[1].PC, entry+0x38)
			c.Check(nested[2].PC == entry+0x44, "physical frame keeps query PC, got %#x", nested[2].PC)
		}

		// Sibling calls share the inlined function name but keep distinct sites.
		sibA, err := f.InlineFrames(tab, module, entry+0x14)
		c.Check(err == nil, "sibling A query failed: %v", err)
		sibB, err := f.InlineFrames(tab, module, entry+0x24)
		c.Check(err == nil, "sibling B query failed: %v", err)
		c.Check(len(sibA) == 2 && len(sibB) == 2, "sibling chain depths: %d %d", len(sibA), len(sibB))
		if len(sibA) == 2 && len(sibB) == 2 {
			c.Check(sibA[0].Name == "main.leaf" && sibB[0].Name == "main.leaf", "both siblings must be main.leaf")
			c.Check(sibA[0].PC == entry+0x18 && sibB[0].PC == entry+0x28, "sibling call sites must differ: %#x %#x", sibA[0].PC, sibB[0].PC)
			c.Check(sibA[0].StartLine == 100 && sibB[0].StartLine == 100, "leaf declaration line: got %d %d", sibA[0].StartLine, sibB[0].StartLine)
			c.Check(sibA[1].Name == "main.physical" && sibB[1].Name == "main.physical", "sibling physical frame")
		}

		// No inline information at a physical-only PC: exactly one frame.
		plain, err := f.InlineFrames(tab, module, entry+0x60)
		c.Check(err == nil, "physical-only query failed: %v", err)
		c.Check(len(plain) == 1, "physical-only frame count: got %d want 1", len(plain))
		if len(plain) == 1 {
			c.Check(plain[0].Name == "main.physical" && !plain[0].Inlined, "physical-only frame contents: %+v", plain[0])
		}

		// PC outside every function is an error, not a partial chain.
		outside, err := f.InlineFrames(tab, module, entry+0x5000)
		c.Check(err != nil && outside == nil, "out-of-range PC must error, got frames=%v err=%v", outside, err)

		// FUNCDATA absent (0xffffffff) while PCDATA names a record: hard error.
		absentPath := buildSyntheticELF(t, dir, syntheticOptions{funcdataAbsent: true})
		af, atab, amod := openSynthetic(t, absentPath)
		_, err = af.InlineFrames(atab, amod, entry+0x44)
		c.Check(err != nil, "absent FUNCDATA must error")
		af.Close()

		// Valid zero FUNCDATA offset: the tree begins exactly at gofunc.
		zeroPath := buildSyntheticELF(t, dir, syntheticOptions{funcdataZero: true})
		zf, ztab, zmod := openSynthetic(t, zeroPath)
		zeroFrames, err := zf.InlineFrames(ztab, zmod, entry+0x14)
		c.Check(err == nil, "zero FUNCDATA offset query failed: %v", err)
		if err == nil {
			c.Check(len(zeroFrames) == 2 && zeroFrames[0].Name == "main.leaf", "zero offset must resolve the tree at gofunc: %+v", zeroFrames)
		}
		zf.Close()

		// No PCDATA table: physical frame only, no error.
		noPCPath := buildSyntheticELF(t, dir, syntheticOptions{pcdataAbsent: true})
		nf, ntab, nmod := openSynthetic(t, noPCPath)
		noPCFrames, err := nf.InlineFrames(ntab, nmod, entry+0x44)
		c.Check(err == nil, "missing PCDATA must not error: %v", err)
		c.Check(len(noPCFrames) == 1 && !noPCFrames[0].Inlined, "missing PCDATA yields physical frame only")
		nf.Close()

		// Malformed chain conditions must surface errors.
		for _, opt := range []syntheticOptions{
			{goFuncUnmapped: true},
			{badNameOffset: true},
			{parentEscapes: true},
			{cyclicParent: true},
		} {
			badPath := buildSyntheticELF(t, dir, opt)
			bf, btab, bmod := openSynthetic(t, badPath)
			bad, badErr := bf.InlineFrames(btab, bmod, entry+0x44)
			c.Check(badErr != nil && bad == nil, "corrupt chain %+v must error, got %v", opt, badErr)
			bf.Close()
		}
	})
}

func TestInlineCoordinates(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dir := t.TempDir()
		path := buildSyntheticELF(t, dir, syntheticOptions{})
		f, tab, module := openSynthetic(t, path)
		defer f.Close()
		entry := segVA

		nested, err := f.InlineFrames(tab, module, entry+0x44)
		c.Check(err == nil, "nested query: %v", err)
		if len(nested) == 3 {
			// Positions come from the physical pcfile/pcln at each parentPc.
			c.Check(nested[0].File == "b/inline.go" && nested[0].Line == 222, "inner call site coordinates: %s:%d", nested[0].File, nested[0].Line)
			c.Check(nested[0].StartLine == 70, "inner declaration line: %d", nested[0].StartLine)
			c.Check(nested[1].File == "b/inline.go" && nested[1].Line == 301, "middle call site coordinates: %s:%d", nested[1].File, nested[1].Line)
			c.Check(nested[1].StartLine == 50, "middle declaration line: %d", nested[1].StartLine)
			// The physical frame uses the queried instruction's own position
			// in the physical pcfile/pcln tables.
			c.Check(nested[2].File == "b/inline.go" && nested[2].Line == 302, "physical coordinates: %s:%d", nested[2].File, nested[2].Line)
			c.Check(nested[2].StartLine == 7, "physical declaration line: %d", nested[2].StartLine)
		}

		// Irregular line table strides land each sibling on its own call line.
		sibA, _ := f.InlineFrames(tab, module, entry+0x14)
		sibB, _ := f.InlineFrames(tab, module, entry+0x24)
		if len(sibA) == 2 && len(sibB) == 2 {
			c.Check(sibA[0].Line == 210 && sibA[0].File == "a/physical.go", "sibling A call line: %d", sibA[0].Line)
			c.Check(sibB[0].Line == 220 && sibB[0].File == "a/physical.go", "sibling B call line: %d", sibB[0].Line)
			c.Check(sibA[1].Line == 202 && sibB[1].Line == 211, "physical lines follow the same pcln table: %d %d", sibA[1].Line, sibB[1].Line)
		}

		// Every frame carries the instruction PC for its own layer. A PC in
		// the second CU file exercises the cutab/filetab mapping.
		plain, err := f.InlineFrames(tab, module, entry+0x48)
		c.Check(err == nil && len(plain) == 1, "plain query: %v", err)
		if len(plain) == 1 {
			c.Check(plain[0].PC == entry+0x48 && plain[0].Line == 303 && plain[0].File == "b/inline.go",
				"plain frame coordinates: %#x %s:%d", plain[0].PC, plain[0].File, plain[0].Line)
		}
	})
}

// cliJSON mirrors the analyzer output fields exercised by the CLI tests.
type cliJSON struct {
	Version string
	TabMeta struct {
		VA      uint64
		Version string
	}
	ModuleMeta    map[string]json.RawMessage
	UserFunctions []struct {
		Start    uint64
		End      uint64
		FullName string
	}
	PCFrames []struct {
		PC     uint64
		Frames []gosym.InlineFrame
		Error  string
	}
}

func buildAnalyzer(t *testing.T, dir string) string {
	t.Helper()
	binaryPath := filepath.Join(dir, "goresym")
	cmd := exec.Command("go", "build", "-o", binaryPath, ".")
	cmd.Env = append(os.Environ(), "GOTOOLCHAIN=local", "CGO_ENABLED=0", "GOFLAGS=-mod=vendor -buildvcs=false")
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("build analyzer: %v\n%s", err, out)
	}
	return binaryPath
}

func runAnalyzer(t *testing.T, analyzer, sample string, extra []string) cliJSON {
	t.Helper()
	parsed, err := tryAnalyzer(analyzer, sample, extra)
	if err != nil {
		t.Fatalf("analyzer on %s failed: %v", sample, err)
	}
	return parsed
}

func tryAnalyzer(analyzer, sample string, extra []string) (cliJSON, error) {
	// The stdlib flag package stops at the first positional argument, so the
	// sample path must follow all flags.
	args := append(append([]string{}, extra...), sample)
	cmd := exec.Command(analyzer, args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return cliJSON{}, fmt.Errorf("%w: %s", err, strings.TrimSpace(string(out)))
	}
	var parsed cliJSON
	if err := json.Unmarshal(out, &parsed); err != nil {
		return cliJSON{}, fmt.Errorf("output is not JSON: %w: %s", err, strings.TrimSpace(string(out)))
	}
	return parsed, nil
}

func toolchainInScope(t *testing.T) (bool, string) {
	out, err := exec.Command("go", "env", "GOVERSION").CombinedOutput()
	if err != nil {
		return false, ""
	}
	ver := strings.TrimSpace(strings.TrimPrefix(string(out), "go"))
	parts := strings.Split(ver, ".")
	if len(parts) < 2 {
		return false, ver
	}
	minor, err := strconv.Atoi(parts[1])
	if err != nil {
		return false, ver
	}
	return minor >= 20 && minor <= 26, ver
}

func TestInlineCLI(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dir := t.TempDir()
		analyzer := buildAnalyzer(t, dir)

		// ---- Always-available end-to-end harness: synthetic stripped ELF ----
		synth := buildSyntheticELF(t, dir, syntheticOptions{})
		entry := segVA
		goodPC := fmt.Sprintf("0x%x", entry+0x44)
		dupPC := fmt.Sprintf("0x%x", entry+0x14)
		badPC := "0x1"
		parsed := runAnalyzer(t, analyzer, synth, []string{
			"-pc", goodPC,
			"-pc", dupPC,
			"-pc", dupPC,
			"-pc", badPC,
			"-nofuncs",
		})
		c.Check(len(parsed.PCFrames) == 4, "PCFrames count and order: got %d", len(parsed.PCFrames))
		if len(parsed.PCFrames) == 4 {
			c.Check(parsed.PCFrames[0].PC == entry+0x44 && parsed.PCFrames[0].Error == "", "first query succeeds")
			c.Check(len(parsed.PCFrames[0].Frames) == 3, "nested chain through CLI: %d", len(parsed.PCFrames[0].Frames))
			c.Check(parsed.PCFrames[1].PC == entry+0x14 && parsed.PCFrames[2].PC == entry+0x14, "duplicate -pc retained in order")
			c.Check(len(parsed.PCFrames[1].Frames) == 2 && len(parsed.PCFrames[2].Frames) == 2, "duplicate chains identical")
			c.Check(parsed.PCFrames[3].PC == 1 && parsed.PCFrames[3].Error != "" && len(parsed.PCFrames[3].Frames) == 0,
				"failed query isolates error: %+v", parsed.PCFrames[3])
			c.Check(parsed.PCFrames[0].Error == "" && parsed.PCFrames[1].Error == "" && parsed.PCFrames[2].Error == "",
				"one bad address does not affect the rest of the batch")
		}
		c.Check(len(parsed.UserFunctions) == 0, "-nofuncs suppresses the function listing but not PC queries")
		// GoFunc is internal (json:"-") and must never appear in ModuleMeta JSON.
		_, hasGoFunc := parsed.ModuleMeta["GoFunc"]
		c.Check(!hasGoFunc, "ModuleMeta JSON must not expose GoFunc")

		// ---- Stripped fixtures from task_support/fixture across 3 formats ----
		inScope, goVer := toolchainInScope(t)
		if !inScope {
			t.Logf("local toolchain %s is outside the 1.20-1.26 support scope; three-format fixture assertions run on an in-scope toolchain", goVer)
		}
		type target struct {
			goos, goarch, ext string
		}
		targets := []target{
			{"linux", "amd64", ".elf"},
			{"windows", "amd64", ".exe"},
			{"darwin", "arm64", ".macho"},
		}
		for _, tgt := range targets {
			sample := filepath.Join(dir, "fixture"+tgt.ext)
			build := exec.Command("go", "build", "-trimpath", "-ldflags=-s -w", "-o", sample, "./task_support/fixture")
			build.Env = append(os.Environ(), "GOTOOLCHAIN=local", "CGO_ENABLED=0",
				"GOOS="+tgt.goos, "GOARCH="+tgt.goarch, "GOFLAGS=-mod=vendor -buildvcs=false")
			if out, berr := build.CombinedOutput(); berr != nil {
				t.Logf("fixture build %s/%s failed: %v\n%s", tgt.goos, tgt.goarch, berr, out)
				continue
			}

			base, baseErr := tryAnalyzer(analyzer, sample, nil)
			if baseErr != nil {
				if !inScope {
					t.Logf("%s fixture built by out-of-scope toolchain is not parseable (%v); skipping format assertions", tgt.goos, baseErr)
					continue
				}
				c.Check(false, "%s analyzer failed: %v", tgt.goos, baseErr)
				continue
			}
			c.Check(len(base.UserFunctions) > 0, "%s fixture lists physical functions", tgt.goos)
			var rootStart, rootEnd uint64
			foundRoot := false
			for _, fn := range base.UserFunctions {
				if fn.FullName == "main.root" {
					rootStart, rootEnd, foundRoot = fn.Start, fn.End, true
				}
				c.Check(fn.FullName != "main.leaf" && fn.FullName != "main.middle",
					"%s fixture must be stripped of inlined callees: %s", tgt.goos, fn.FullName)
			}
			c.Check(foundRoot, "%s fixture contains physical main.root", tgt.goos)
			if !foundRoot {
				continue
			}

			// Dense sampling across root at fixed construction strides (not
			// values produced by the method under test).
			var pcs []string
			step := (rootEnd - rootStart) / 48
			if step == 0 {
				step = 1
			}
			for i := uint64(0); i < 48; i++ {
				pcs = append(pcs, "0x"+strconv.FormatUint(rootStart+i*step, 16))
			}
			outside := "0xdead000000000000"
			args := []string{"-pc", pcs[0]}
			for _, p := range pcs[1:] {
				args = append(args, "-pc", p)
			}
			args = append(args, "-pc", pcs[0], "-pc", outside) // repeated entry + error isolation
			res, resErr := tryAnalyzer(analyzer, sample, args)
			if resErr != nil {
				c.Check(false, "%s analyzer -pc run failed: %v", tgt.goos, resErr)
				continue
			}

			c.Check(len(res.PCFrames) == len(pcs)+2, "%s PCFrames count: %d", tgt.goos, len(res.PCFrames))
			sawDepthThree, sawSiblingMiddle := false, false
			middleCallPCs := map[uint64]bool{}
			for i, r := range res.PCFrames {
				if i == len(res.PCFrames)-1 {
					c.Check(r.Error != "" && len(r.Frames) == 0, "%s outside address errors cleanly", tgt.goos)
					continue
				}
				c.Check(r.Error == "" && len(r.Frames) >= 1, "%s query %d failed: %s", tgt.goos, i, r.Error)
				if len(r.Frames) == 0 {
					continue
				}
				last := r.Frames[len(r.Frames)-1]
				c.Check(last.Name == "main.root" && !last.Inlined, "%s chain ends at physical main.root", tgt.goos)
				depth := 0
				for _, fr := range r.Frames {
					if fr.Inlined {
						depth++
						c.Check(strings.HasSuffix(fr.File, "/task_support/fixture/main.go") && fr.Line > 0 && fr.StartLine > 0,
							"%s inlined frame coordinates: %s:%d", tgt.goos, fr.File, fr.Line)
					}
					if fr.Name == "main.middle" {
						middleCallPCs[fr.PC] = true
					}
				}
				if depth >= 2 {
					sawDepthThree = true
				}
			}
			sawSiblingMiddle = len(middleCallPCs) >= 2
			c.Check(sawDepthThree, "%s fixture has a leaf<-middle<-root chain", tgt.goos)
			c.Check(sawSiblingMiddle, "%s fixture distinguishes the two middle call sites", tgt.goos)
			c.Check(res.PCFrames[0].PC == res.PCFrames[len(res.PCFrames)-2].PC, "%s duplicate query retained", tgt.goos)
		}

		// Without -pc the output stays compatible: no PCFrames key is emitted.
		plain := runAnalyzer(t, analyzer, synth, nil)
		c.Check(plain.PCFrames == nil, "omitting -pc keeps the legacy JSON shape")
	})
}
