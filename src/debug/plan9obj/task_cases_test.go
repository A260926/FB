// Copyright 2014 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package plan9obj

import (
	"bytes"
	"encoding/binary"
	"strings"
	"testing"
)

// TaskCases registers the source-map exercise groups driven by the task
// harness. Every check goes through TaskCheck so the harness can observe
// that each group actually asserts something.
var TaskCases = []TaskCase{
	{Name: "compressed", Run: testCompressed},
	{Name: "history", Run: testHistory},
	{Name: "identity", Run: testIdentity},
	{Name: "malformed", Run: testMalformed},
}

// execBuilder assures a minimal Plan 9 a.out image from whole sections.
type execBuilder struct {
	magic uint32
	ptrsz int
	hdr   uint32
	text  []byte
	data  []byte
	syms  []byte
	spsz  []byte
	pcsz  []byte
}

func newExecBuilder(magic uint32) *execBuilder {
	if magic&Magic64 != 0 {
		return &execBuilder{magic: magic, ptrsz: 8, hdr: 40}
	}
	return &execBuilder{magic: magic, ptrsz: 4, hdr: 32}
}

func (b *execBuilder) putValue(buf *bytes.Buffer, v uint64) {
	if b.ptrsz == 8 {
		var tmp [8]byte
		binary.BigEndian.PutUint64(tmp[:], v)
		buf.Write(tmp[:])
	} else {
		var tmp [4]byte
		binary.BigEndian.PutUint32(tmp[:], uint32(v))
		buf.Write(tmp[:])
	}
}

func (b *execBuilder) fSym(num uint16, name string) {
	var buf bytes.Buffer
	b.putValue(&buf, uint64(num))
	buf.WriteByte('f')
	buf.WriteString(name)
	buf.WriteByte(0)
	b.syms = append(b.syms, buf.Bytes()...)
}

func (b *execBuilder) textSym(value uint64, typ byte, name string) {
	var buf bytes.Buffer
	b.putValue(&buf, value)
	buf.WriteByte(typ)
	buf.WriteString(name)
	buf.WriteByte(0)
	b.syms = append(b.syms, buf.Bytes()...)
}

// histSym writes a z (pairs != nil semantics) or Z record. An empty pairs
// slice is the empty-name record that leaves the current file.
func (b *execBuilder) histSym(typ byte, value uint64, pairs []uint16) {
	var buf bytes.Buffer
	b.putValue(&buf, value)
	buf.WriteByte(typ)
	buf.WriteByte(0) // history names begin with a NUL
	for _, p := range pairs {
		var tmp [2]byte
		binary.BigEndian.PutUint16(tmp[:], p)
		buf.Write(tmp[:])
	}
	buf.WriteByte(0)
	buf.WriteByte(0) // double-NUL terminator
	b.syms = append(b.syms, buf.Bytes()...)
}

func (b *execBuilder) bytes() []byte {
	var out bytes.Buffer
	words := []uint32{
		b.magic,
		uint32(len(b.text)),
		uint32(len(b.data)),
		0, // bss
		uint32(len(b.syms)),
		0, // entry (32-bit field)
		uint32(len(b.spsz)),
		uint32(len(b.pcsz)),
	}
	for _, w := range words {
		var tmp [4]byte
		binary.BigEndian.PutUint32(tmp[:], w)
		out.Write(tmp[:])
	}
	if b.ptrsz == 8 {
		out.Write(make([]byte, 8)) // 64-bit entry point
	}
	out.Write(b.text)
	out.Write(b.data)
	out.Write(b.syms)
	out.Write(b.spsz)
	out.Write(b.pcsz)
	return out.Bytes()
}

func (b *execBuilder) file(t *testing.T) *File {
	t.Helper()
	f, err := NewFile(bytes.NewReader(b.bytes()))
	if err != nil {
		t.Fatalf("NewFile: %v", err)
	}
	return f
}

// bigDelta encodes a byte-0 record carrying a signed 32-bit line delta.
func bigDelta(delta int32) []byte {
	var buf bytes.Buffer
	buf.WriteByte(0)
	var tmp [4]byte
	binary.BigEndian.PutUint32(tmp[:], uint32(delta))
	buf.Write(tmp[:])
	return buf.Bytes()
}

type wantLoc struct {
	pc       uint64
	ok       bool
	symbol   string
	line     int64
	file     string
	fileLine int64
}

func checkLocations(t *testing.T, m *SourceMap, wants []wantLoc) {
	t.Helper()
	for _, w := range wants {
		loc, ok := m.Lookup(w.pc)
		TaskCheck(t, ok == w.ok, "Lookup presence mismatch for pc")
		if !w.ok {
			continue
		}
		TaskCheck(t, loc.TextSymbol == w.symbol, "text symbol mismatch for pc")
		TaskCheck(t, loc.Line == w.line, "absolute line mismatch for pc")
		TaskCheck(t, loc.File == w.file, "source file mismatch for pc")
		TaskCheck(t, loc.FileLine == w.fileLine, "file line mismatch for pc")
	}
}

// testCompressed exercises the compressed pc/line stream: fixed positive
// and negative steps, 32-bit signed deltas, PC-only jumps and half-open
// range handling, across all supported quanta.
func testCompressed(t *testing.T) {
	const textStart = uint64(0x1000)
	// Records, starting from pc=textStart-1 quantum, line 0:
	//	+10 line; jump 3 extra quanta; big delta -7; small -2; jump 0; big +100000
	pcsz := []byte{10, 132}
	pcsz = append(pcsz, bigDelta(-7)...)
	pcsz = append(pcsz, 66, 129)
	pcsz = append(pcsz, bigDelta(100000)...)

	b := newExecBuilder(Magic386)
	b.text = make([]byte, 64)
	b.pcsz = pcsz
	f := b.file(t)

	for _, q := range []uint8{3, 0, 8} {
		_, err := f.SourceMap(textStart, q)
		TaskCheck(t, err != nil, "unsupported quantum must be rejected")
	}

	m, err := f.SourceMap(textStart, 1)
	TaskCheck(t, err == nil, "SourceMap with quantum 1 failed")
	TaskCheck(t, m != nil, "SourceMap returned nil map")

	// Independent address-to-location expectations for quantum 1.
	checkLocations(t, m, []wantLoc{
		{pc: 0xFFF, ok: false}, // before textStart
		{pc: 0x1000, ok: true, line: 10},
		{pc: 0x1001, ok: true, line: 10}, // jumped-over byte keeps prior line
		{pc: 0x1003, ok: true, line: 10}, // last byte covered by the jump
		{pc: 0x1004, ok: true, line: 10}, // jump landing address
		{pc: 0x1005, ok: true, line: 3},  // after negative 32-bit delta
		{pc: 0x1006, ok: true, line: 1},  // after small negative step
		{pc: 0x1007, ok: true, line: 1},  // zero-extra jump still advances
		{pc: 0x1008, ok: true, line: 100001},
		{pc: 0x1009, ok: false}, // table ended: last line must not extend
		{pc: 0x100F, ok: false}, // inside text but never reached by the table
	})

	// quantum 2 and 4 scale every PC move but never the line numbers.
	// Record endpoints in byte offsets from textStart (quantum 2):
	// +0 (line 10), +8 (line 10), +10 (line 3), +12 (line 1), +14
	// (line 1), +16 (line 100001). Each entry spans a 2-byte quantum.
	m2, err := f.SourceMap(textStart, 2)
	TaskCheck(t, err == nil, "SourceMap with quantum 2 failed")
	checkLocations(t, m2, []wantLoc{
		{pc: 0x1000, ok: true, line: 10},
		{pc: 0x1001, ok: true, line: 10}, // within the first 2-byte quantum
		{pc: 0x1008, ok: true, line: 10}, // jump landing: +(3+1)*2 bytes
		{pc: 0x1009, ok: true, line: 3},
		{pc: 0x100A, ok: true, line: 3},
		{pc: 0x100B, ok: true, line: 1},
		{pc: 0x100F, ok: true, line: 100001},
		{pc: 0x1010, ok: true, line: 100001}, // last byte of the final quantum
		{pc: 0x1011, ok: false},              // table ended: last line must not extend
	})

	// Record endpoints (quantum 4): +0, +16, +20, +24, +28, +32.
	m4, err := f.SourceMap(textStart, 4)
	TaskCheck(t, err == nil, "SourceMap with quantum 4 failed")
	checkLocations(t, m4, []wantLoc{
		{pc: 0x1000, ok: true, line: 10},
		{pc: 0x1003, ok: true, line: 10}, // within the first 4-byte quantum
		{pc: 0x1010, ok: true, line: 10}, // jump landing: +(3+1)*4 bytes
		{pc: 0x1011, ok: true, line: 3},
		{pc: 0x1014, ok: true, line: 3},
		{pc: 0x1015, ok: true, line: 1},
		{pc: 0x101D, ok: true, line: 100001},
		{pc: 0x1020, ok: true, line: 100001}, // last byte of the final quantum
		{pc: 0x1021, ok: false},              // table ended
	})
}

// testHistory covers two-level include/return line accounting and a #line
// (z followed by Z) redirecting subsequent positions.
func testHistory(t *testing.T) {
	const textStart = uint64(0x2000)
	b := newExecBuilder(Magic386)
	b.text = make([]byte, 16)

	// File-number components.
	b.fSym(1, "/")
	b.fSym(2, "src")
	b.fSym(3, "main.c")
	b.fSym(4, "hdr.h")

	// One compile unit, one text symbol, then the history.
	b.histSym('z', 1, []uint16{1, 2, 3}) // enter /src/main.c at absolute line 1
	b.histSym('Z', 50, nil)              // #line 50: main.c now starts at line 50
	b.textSym(0, 'T', "fn")
	b.histSym('z', 10, []uint16{1, 2, 4}) // enter /src/hdr.h at absolute line 10
	b.histSym('z', 14, nil)               // leave header at absolute line 14

	// Absolute lines reached: 2 (main.c after #line), 11 (inside the
	// header), 15 (back in main.c). The include consumes absolute lines
	// 10..13, so main.c resumes on the line after where it paused.
	var pcsz []byte
	pcsz = append(pcsz, bigDelta(2)...) // line 2
	pcsz = append(pcsz, bigDelta(9)...) // line 11
	pcsz = append(pcsz, bigDelta(4)...) // line 15
	b.pcsz = pcsz

	f := b.file(t)
	m, err := f.SourceMap(textStart, 1)
	TaskCheck(t, err == nil, "history SourceMap failed")

	const mainFile = "/src/main.c"
	const hdrFile = "/src/hdr.h"
	checkLocations(t, m, []wantLoc{
		{pc: textStart + 0, ok: true, symbol: "fn", line: 2, file: mainFile, fileLine: 51},
		{pc: textStart + 1, ok: true, symbol: "fn", line: 11, file: hdrFile, fileLine: 2},
		{pc: textStart + 2, ok: true, symbol: "fn", line: 15, file: mainFile, fileLine: 61},
	})
}

// testIdentity checks that separate compile units keep separate histories
// even for same-named files, that same-address text aliases keep the first
// symbol without reordering Symbols(), that 64-bit addresses work, and that
// positions before any history still report symbol and absolute line.
func testIdentity(t *testing.T) {
	const textStart = uint64(0x4000)
	b := newExecBuilder(Magic386)
	b.text = make([]byte, 32)

	// A text symbol appearing before any history: file must stay unknown.
	b.textSym(0, 'T', "early")

	b.fSym(1, "/")
	b.fSym(2, "a")
	b.fSym(3, "main.c")
	b.fSym(4, "b")
	b.fSym(5, "main.c") // same base name, different path

	// Compile unit 1: /a/main.c, text symbol "a".
	b.histSym('z', 1, []uint16{1, 2, 3})
	b.textSym(8, 'T', "a")
	b.histSym('z', 3, nil) // close the unit's file

	// Compile unit 2: /b/main.c, text symbol "b" plus an alias at "a"'s
	// address to check first-wins handling.
	b.histSym('z', 1, []uint16{1, 4, 5})
	b.textSym(8, 'T', "alias-of-a")
	b.textSym(16, 'T', "b")
	b.histSym('z', 3, nil)

	// Absolute line 2 in both units; each must resolve to its own path.
	// The big delta lands at pc+1, then a jump of 16 extra quanta carries
	// the unchanged line far enough to cover both text symbols (+8, +16).
	var pcsz []byte
	pcsz = append(pcsz, bigDelta(2)...) // pc +1, line 2
	pcsz = append(pcsz, 145)            // +(16+1) quanta, line unchanged
	b.pcsz = pcsz

	f := b.file(t)
	m, err := f.SourceMap(textStart, 1)
	TaskCheck(t, err == nil, "identity SourceMap failed")

	// Before any history: text symbol and absolute line, unknown file.
	loc, ok := m.Lookup(textStart)
	TaskCheck(t, ok, "pre-history lookup must succeed")
	TaskCheck(t, loc.TextSymbol == "early", "pre-history text symbol wrong")
	TaskCheck(t, loc.Line == 2, "pre-history absolute line wrong")
	TaskCheck(t, loc.File == "" && loc.FileLine == 0, "pre-history file must be unknown")

	// Same absolute line, two compile units, two distinct files.
	locA, ok := m.Lookup(textStart + 8)
	TaskCheck(t, ok, "unit-1 lookup failed")
	TaskCheck(t, locA.TextSymbol == "a", "first same-address alias must win")
	TaskCheck(t, locA.Line == 2 && locA.File == "/a/main.c" && locA.FileLine == 2,
		"unit-1 must use its own history")

	locB, ok := m.Lookup(textStart + 16)
	TaskCheck(t, ok, "unit-2 lookup failed")
	TaskCheck(t, locB.TextSymbol == "b", "unit-2 text symbol wrong")
	TaskCheck(t, locB.Line == 2 && locB.File == "/b/main.c" && locB.FileLine == 2,
		"unit-2 must use its own history")

	// Symbols() must preserve original table order, alias included.
	syms, err := f.Symbols()
	TaskCheck(t, err == nil, "Symbols failed")
	var names []string
	for _, s := range syms {
		if s.Type == 'T' {
			names = append(names, s.Name)
		}
	}
	TaskCheck(t, strings.Join(names, ",") == "early,a,alias-of-a,b",
		"Symbols() order must be preserved")

	// 64-bit image with a high text start exercises 8-byte symbol values
	// and large addresses, including independence from Entry.
	const hiStart = uint64(0x1_0000_0000)
	b64 := newExecBuilder(MagicAMD64)
	b64.text = make([]byte, 8)
	b64.fSym(1, "/")
	b64.fSym(2, "x.c")
	b64.histSym('z', 1, []uint16{1, 2})
	b64.textSym(0, 'T', "hi")
	b64.pcsz = bigDelta(5)
	f64 := b64.file(t)
	TaskCheck(t, f64.PtrSize == 8, "64-bit pointer size wrong")
	m64, err := f64.SourceMap(hiStart, 1)
	TaskCheck(t, err == nil, "64-bit SourceMap failed")
	hi, ok := m64.Lookup(hiStart)
	TaskCheck(t, ok, "64-bit lookup failed")
	TaskCheck(t, hi.TextSymbol == "hi" && hi.Line == 5 && hi.File == "/x.c" && hi.FileLine == 5,
		"64-bit location wrong")
	TaskCheck(t, hiStart != f64.Entry, "test relies on textStart differing from Entry")
}

// testMalformed verifies that corrupt tables are rejected with section and
// offset detail, that no half-built map is returned, and that empty input
// yields an empty map instead of panicking.
func testMalformed(t *testing.T) {
	// Empty pcsz: empty, queryable-but-always-miss map, no panic.
	empty := newExecBuilder(Magic386)
	empty.text = make([]byte, 8)
	fEmpty := empty.file(t)
	mEmpty, err := fEmpty.SourceMap(0x1000, 1)
	TaskCheck(t, err == nil, "empty pcsz must not error")
	TaskCheck(t, mEmpty != nil, "empty pcsz must still return a map")
	_, ok := mEmpty.Lookup(0x1000)
	TaskCheck(t, !ok, "empty map must have no locations")

	cases := []struct {
		name    string
		section string
		build   func() *execBuilder
	}{
		{
			name:    "truncated big line delta",
			section: "pcsz",
			build: func() *execBuilder {
				b := newExecBuilder(Magic386)
				b.text = make([]byte, 8)
				b.pcsz = []byte{1, 0, 0x10, 0x20} // 0x00 then only three delta bytes
				return b
			},
		},
		{
			name:    "undefined file number",
			section: "syms",
			build: func() *execBuilder {
				b := newExecBuilder(Magic386)
				b.text = make([]byte, 8)
				b.fSym(1, "/")
				b.histSym('z', 1, []uint16{99}) // number 99 never defined
				b.pcsz = bigDelta(1)
				return b
			},
		},
		{
			name:    "orphan Z record",
			section: "syms",
			build: func() *execBuilder {
				b := newExecBuilder(Magic386)
				b.text = make([]byte, 8)
				b.fSym(1, "/")
				b.histSym('z', 1, []uint16{1})
				b.histSym('z', 2, nil) // pop the only frame...
				b.histSym('Z', 5, nil) // ...so Z has no preceding entry
				b.pcsz = bigDelta(1)
				return b
			},
		},
		{
			name:    "history stack underflow",
			section: "syms",
			build: func() *execBuilder {
				b := newExecBuilder(Magic386)
				b.text = make([]byte, 8)
				b.fSym(1, "/")
				b.histSym('z', 1, []uint16{1})
				b.histSym('z', 2, nil) // balanced pop
				b.histSym('z', 3, nil) // underflow
				b.pcsz = bigDelta(1)
				return b
			},
		},
		{
			name:    "truncated history pair list",
			section: "syms",
			build: func() *execBuilder {
				b := newExecBuilder(Magic386)
				b.text = make([]byte, 8)
				b.fSym(1, "/")
				// Value, type, leading NUL, then a single dangling byte.
				b.syms = append(b.syms, 0, 0, 0, 1, 'z', 0, 0x07)
				b.pcsz = bigDelta(1)
				return b
			},
		},
		{
			name:    "history value overflow",
			section: "syms",
			build: func() *execBuilder {
				b := newExecBuilder(MagicAMD64)
				b.text = make([]byte, 8)
				b.fSym(1, "/")
				b.histSym('z', 1, []uint16{1}) // open a frame first
				b.histSym('Z', 1<<63, nil)     // exceeds int64
				b.pcsz = bigDelta(1)
				return b
			},
		},
		{
			name:    "pc overflow on jump",
			section: "pcsz",
			build: func() *execBuilder {
				b := newExecBuilder(Magic386)
				b.text = make([]byte, 4)
				b.pcsz = []byte{255} // jump 126 extra quanta past uint64
				return b
			},
		},
	}

	for _, tc := range cases {
		f := tc.build().file(t)
		var start uint64
		if tc.name == "pc overflow on jump" {
			// Close enough to the top that 127 quanta overflow, while the
			// text range itself still fits in uint64.
			start = ^uint64(0) - 7
		} else {
			start = 0x1000
		}
		m, err := f.SourceMap(start, 1)
		TaskCheck(t, err != nil, tc.name+": expected error")
		TaskCheck(t, m == nil, tc.name+": must not return a partial map")
		TaskCheck(t, strings.Contains(err.Error(), tc.section), tc.name+": error must name section")
		TaskCheck(t, strings.Contains(err.Error(), "offset"), tc.name+": error must carry offset")
	}
}
