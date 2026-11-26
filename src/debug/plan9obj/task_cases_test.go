// Copyright 2024 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package plan9obj

import (
	"bytes"
	"encoding/binary"
	"errors"
	"strings"
	"testing"
)

// This file builds small synthetic Plan 9 a.out images in memory and
// exercises (*File).SourceMap without any external tools or source files.

// execBuilder assembles a minimal Plan 9 a.out image.
type execBuilder struct {
	magic   uint32
	ptrsz   int
	text    []byte
	pcsz    []byte
	syms    bytes.Buffer
	entry64 uint64
}

func newExecBuilder(magic uint32, textSize int) *execBuilder {
	b := &execBuilder{magic: magic, ptrsz: 4, text: make([]byte, textSize)}
	if magic&Magic64 != 0 {
		b.ptrsz = 8
	}
	return b
}

func (b *execBuilder) putValue(buf *bytes.Buffer, v uint64) {
	if b.ptrsz == 8 {
		binary.Write(buf, binary.BigEndian, v)
	} else {
		binary.Write(buf, binary.BigEndian, uint32(v))
	}
}

// f adds a file-name symbol: value is the component number.
func (b *execBuilder) f(num uint64, name string) {
	b.putValue(&b.syms, num)
	b.syms.WriteByte('f')
	b.syms.WriteString(name)
	b.syms.WriteByte(0)
}

// z adds a history push (codes given) or pop (no codes).
func (b *execBuilder) z(line uint64, codes ...uint16) {
	b.putValue(&b.syms, line)
	b.syms.WriteByte('z')
	b.syms.WriteByte(0) // history records carry an empty NUL name
	for _, c := range codes {
		binary.Write(&b.syms, binary.BigEndian, c)
	}
	b.syms.Write([]byte{0, 0})
}

// Z adds a #line history directive.
func (b *execBuilder) Z(line uint64) {
	b.putValue(&b.syms, line)
	b.syms.WriteByte('Z')
	b.syms.WriteByte(0)
	b.syms.Write([]byte{0, 0})
}

// txt adds a text symbol of the given type ('T', 't', 'L' or 'l').
func (b *execBuilder) txt(typ byte, off uint64, name string) {
	b.putValue(&b.syms, off)
	b.syms.WriteByte(typ)
	b.syms.WriteString(name)
	b.syms.WriteByte(0)
}

func (b *execBuilder) bytes() []byte {
	var out bytes.Buffer
	// 32-bit program header.
	binary.Write(&out, binary.BigEndian, b.magic)
	binary.Write(&out, binary.BigEndian, uint32(len(b.text)))  // text
	binary.Write(&out, binary.BigEndian, uint32(0))            // data
	binary.Write(&out, binary.BigEndian, uint32(0))            // bss
	binary.Write(&out, binary.BigEndian, uint32(b.syms.Len())) // syms
	binary.Write(&out, binary.BigEndian, uint32(0))            // entry (32-bit field)
	binary.Write(&out, binary.BigEndian, uint32(0))            // spsz
	binary.Write(&out, binary.BigEndian, uint32(len(b.pcsz)))  // pcsz
	if b.ptrsz == 8 {
		binary.Write(&out, binary.BigEndian, b.entry64)
	}
	out.Write(b.text)
	out.Write(b.syms.Bytes())
	// spsz is empty
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

// gateReader fails every ReadAt once closed, proving the SourceMap
// owns its data.
type gateReader struct {
	data   []byte
	closed bool
}

func (g *gateReader) ReadAt(p []byte, off int64) (int, error) {
	if g.closed {
		return 0, errors.New("reader is closed")
	}
	if off < 0 || off >= int64(len(g.data)) {
		return 0, errors.New("bad offset")
	}
	n := copy(p, g.data[off:])
	if n < len(p) {
		return n, errors.New("short read")
	}
	return n, nil
}

// TaskCases registers the required source-map test groups.
var TaskCases = []TaskCase{
	{Name: "compressed", Run: testCompressed},
	{Name: "history", Run: testHistory},
	{Name: "identity", Run: testIdentity},
	{Name: "malformed", Run: testMalformed},
}

// testCompressed covers counter jumps, both forms of negative line
// increment, first-reach lookup, half-open ranges and the hard end of
// the table.
func testCompressed(t *testing.T) {
	const textStart = 0x1000
	b := newExecBuilder(Magic386, 16)
	b.pcsz = []byte{
		10,                        // line 10, pc 0x1000
		130,                       // jump: advance two quanta at line 10
		1,                         // line 11, pc 0x1003
		65,                        // line 10 (negative short increment)
		0, 0xff, 0xff, 0xff, 0xfb, // line 5 (int32 delta -5)
		3,   // line 8
		132, // jump: advance four quanta at line 8
		2,   // line 10, pc 0x100b
	}
	m, err := b.file(t).SourceMap(textStart, 1)
	if err != nil {
		t.Fatalf("SourceMap: %v", err)
	}

	type want struct {
		pc   uint64
		line int64
		ok   bool
	}
	wants := []want{
		{textStart - 1, 0, false}, // before the text range
		{textStart, 10, true},     // first record
		{textStart + 1, 10, true}, // covered by the jump
		{textStart + 2, 10, true}, // jump landing point
		{textStart + 3, 11, true},
		{textStart + 4, 10, true}, // 65: subtract one line
		{textStart + 5, 5, true},  // int32 negative delta
		{textStart + 6, 8, true},
		{textStart + 7, 8, true}, // inside the second jump
		{textStart + 8, 8, true},
		{textStart + 9, 8, true},
		{textStart + 10, 8, true},  // jump landing point
		{textStart + 11, 10, true}, // last table point
		{textStart + 12, 0, false}, // table ended: no infinite extension
		{textStart + 15, 0, false},
		{textStart + 16, 0, false}, // half-open upper bound
	}
	for _, w := range wants {
		loc, ok := m.Lookup(w.pc)
		TaskCheck(t, ok == w.ok, "lookup ok flag mismatch")
		if w.ok {
			TaskCheck(t, loc.Line == w.line, "lookup line mismatch")
		}
	}

	// quantum validation
	for _, q := range []uint8{0, 3, 8, 255} {
		_, err := b.file(t).SourceMap(textStart, q)
		TaskCheck(t, err != nil, "invalid quantum must be rejected")
	}
	for _, q := range []uint8{1, 2, 4} {
		_, err := b.file(t).SourceMap(textStart, q)
		TaskCheck(t, err == nil, "valid quantum must be accepted")
	}

	// quantum 4: every record and every jump step moves in 4-byte units.
	q4 := newExecBuilder(Magic386, 24)
	q4.pcsz = []byte{
		5,   // line 5, pc 0x2000
		131, // jump: two extra quanta (8 bytes) at line 5, landing 0x200c
		2,   // line 7, pc 0x2010
	}
	m4, err := q4.file(t).SourceMap(0x2000, 4)
	TaskCheck(t, err == nil, "quantum-4 map construction")
	q4wants := []struct {
		pc   uint64
		line int64
		ok   bool
	}{
		{0x2000, 5, true},
		{0x2004, 5, true}, // inside the jump
		{0x200b, 5, true},
		{0x200c, 5, true}, // jump landing
		{0x200f, 7, true}, // next point at 0x2010
		{0x2010, 7, true},
		{0x2014, 0, false}, // table ended before this address
		{0x2018, 0, false}, // half-open upper bound
	}
	for _, w := range q4wants {
		loc, ok := m4.Lookup(w.pc)
		TaskCheck(t, ok == w.ok, "quantum-4 lookup ok flag")
		if w.ok {
			TaskCheck(t, loc.Line == w.line, "quantum-4 lookup line")
		}
	}
}

// testHistory covers two-level includes, #line directives, popping back
// to parent files with line correction, and two compilation units that
// contain a same-named file.
func testHistory(t *testing.T) {
	const textStart = 0x2000
	b := newExecBuilder(Magic386, 24)
	// One quantum per record, absolute lines 1..24.
	for i := 0; i < 24; i++ {
		b.pcsz = append(b.pcsz, 1)
	}

	// File component numbers.
	b.f(1, "dir")
	b.f(2, "a.c")
	b.f(3, "b.h")
	b.f(4, "c.h")

	// A text symbol without any history, emitted first.
	b.txt('T', 0x14, "nohist")

	// Compilation unit 1: a.c includes b.h, which includes c.h,
	// and b.h carries a #line directive.
	b.z(1, 1, 2) // enter dir/a.c at absolute line 1
	b.z(3, 1, 3) // enter dir/b.h at line 3
	b.Z(10)      // #line 10 inside b.h
	b.z(5, 1, 4) // enter dir/c.h at line 5
	b.z(7)       // leave c.h at line 7
	b.z(9)       // leave b.h at line 9
	b.z(12)      // leave a.c at line 12
	b.txt('T', 0, "cu1func")

	// Compilation unit 2: another dir/a.c, with its own short history.
	b.z(1, 1, 2)
	b.z(2, 1, 4)
	b.z(4)
	b.txt('T', 0xa, "cu2func")

	m, err := b.file(t).SourceMap(textStart, 1)
	if err != nil {
		t.Fatalf("SourceMap: %v", err)
	}

	type want struct {
		pc       uint64
		symbol   string
		line     int64
		file     string
		fileLine int64
	}
	wants := []want{
		// cu1func: nested includes and #line.
		{textStart + 0, "cu1func", 1, "dir/a.c", 1},
		{textStart + 1, "cu1func", 2, "dir/a.c", 2},
		{textStart + 2, "cu1func", 3, "dir/b.h", 10}, // #line 10
		{textStart + 3, "cu1func", 4, "dir/b.h", 11},
		{textStart + 4, "cu1func", 5, "dir/c.h", 1}, // second include level
		{textStart + 5, "cu1func", 6, "dir/c.h", 2},
		{textStart + 6, "cu1func", 7, "dir/b.h", 12}, // back in b.h, span removed
		{textStart + 7, "cu1func", 8, "dir/b.h", 13},
		{textStart + 8, "cu1func", 9, "dir/a.c", 3}, // back in a.c
		{textStart + 9, "cu1func", 10, "dir/a.c", 4},
		// cu2func: same file name, independent history segment.
		{textStart + 10, "cu2func", 11, "dir/a.c", 9},
		{textStart + 11, "cu2func", 12, "dir/a.c", 10},
		{textStart + 12, "cu2func", 13, "dir/a.c", 11},
		{textStart + 13, "cu2func", 14, "dir/a.c", 12},
		// nohist: symbol and absolute line known, file unknown.
		{textStart + 20, "nohist", 21, "", 0},
		{textStart + 23, "nohist", 24, "", 0},
	}
	for _, w := range wants {
		loc, ok := m.Lookup(w.pc)
		TaskCheck(t, ok, "history lookup must succeed")
		TaskCheck(t, loc.Symbol == w.symbol, "symbol mismatch")
		TaskCheck(t, loc.Line == w.line, "absolute line mismatch")
		TaskCheck(t, loc.File == w.file, "file mismatch")
		TaskCheck(t, loc.FileLine == w.fileLine, "in-file line mismatch")
	}

	// The two same-named files must not be merged.
	loc1, _ := m.Lookup(textStart + 1)
	loc2, _ := m.Lookup(textStart + 10)
	TaskCheck(t, loc1.File == loc2.File && loc1.File == "dir/a.c", "same-named files")
	TaskCheck(t, loc1.Symbol != loc2.Symbol, "compilation units must stay separate")
	TaskCheck(t, loc1.FileLine != loc2.FileLine, "histories must not be merged")
}

// testIdentity covers same-address aliases, symbol ordering, empty pcs,
// reader independence, Entry vs textStart, and 64-bit addresses.
func testIdentity(t *testing.T) {
	const textStart = 0x3000

	// Aliases at one address: the first table entry wins lookups,
	// while Symbols keeps the original order.
	b := newExecBuilder(Magic386, 8)
	for i := 0; i < 8; i++ {
		b.pcsz = append(b.pcsz, 1)
	}
	b.txt('T', 0, "first")
	b.txt('t', 0, "second")
	b.txt('L', 2, "lfunc")
	f := b.file(t)
	m, err := f.SourceMap(textStart, 1)
	TaskCheck(t, err == nil, "SourceMap construction")
	loc, ok := m.Lookup(textStart)
	TaskCheck(t, ok && loc.Symbol == "first", "same-address alias keeps first symbol")
	loc, ok = m.Lookup(textStart + 1)
	TaskCheck(t, ok && loc.Symbol == "first", "alias range keeps first symbol")
	loc, ok = m.Lookup(textStart + 2)
	TaskCheck(t, ok && loc.Symbol == "lfunc", "later text symbol takes over")

	syms, err := f.Symbols()
	TaskCheck(t, err == nil, "Symbols must still work")
	iFirst, iSecond, iL := -1, -1, -1
	for i, s := range syms {
		switch s.Name {
		case "first":
			iFirst = i
		case "second":
			iSecond = i
		case "lfunc":
			iL = i
		}
	}
	TaskCheck(t, iFirst >= 0 && iFirst < iSecond && iSecond < iL, "Symbols order must be preserved")

	// Empty pcs: an empty map, never a panic, and every lookup fails.
	empty := newExecBuilder(Magic386, 4)
	em, err := empty.file(t).SourceMap(textStart, 1)
	TaskCheck(t, err == nil && em != nil, "empty pcs builds an empty map")
	_, ok = em.Lookup(textStart)
	TaskCheck(t, !ok, "empty map has no locations")

	// The map owns its bytes: closing or replacing the reader must
	// not change query results.
	gr := &gateReader{data: b.bytes()}
	gf, err := NewFile(gr)
	TaskCheck(t, err == nil, "NewFile on gate reader")
	gm, err := gf.SourceMap(textStart, 1)
	TaskCheck(t, err == nil, "SourceMap on gate reader")
	gr.closed = true
	loc, ok = gm.Lookup(textStart + 3)
	TaskCheck(t, ok && loc.Line == 4 && loc.Symbol == "lfunc", "map survives reader close")
	TaskCheck(t, gf.Close() == nil, "Close is harmless")

	// Entry must never be used as the text start.
	be := newExecBuilder(Magic386, 4)
	be.pcsz = []byte{1, 1, 1, 1}
	fe := be.file(t)
	fe.Entry = 0xdeadbeef
	me, err := fe.SourceMap(textStart, 1)
	TaskCheck(t, err == nil, "explicit textStart accepted")
	loc, ok = me.Lookup(textStart + 1)
	TaskCheck(t, ok && loc.Line == 2, "lookup keyed on textStart, not Entry")
	_, ok = me.Lookup(fe.Entry)
	TaskCheck(t, !ok, "Entry address is not implicitly mapped")

	// 64-bit object with a high text start address.
	const hiStart = uint64(0xFFFFFFFFFFFF0000)
	b64 := newExecBuilder(MagicAMD64, 8)
	b64.entry64 = hiStart
	b64.pcsz = []byte{1, 1, 1, 1}
	b64.txt('T', 0, "hi64")
	m64, err := b64.file(t).SourceMap(hiStart, 1)
	TaskCheck(t, err == nil, "64-bit SourceMap construction")
	loc, ok = m64.Lookup(hiStart + 2)
	TaskCheck(t, ok && loc.Line == 3 && loc.Symbol == "hi64", "64-bit address lookup")
	_, ok = m64.Lookup(hiStart + 8)
	TaskCheck(t, !ok, "64-bit half-open upper bound")
	_, ok = m64.Lookup(hiStart - 1)
	TaskCheck(t, !ok, "64-bit lower bound")
}

// testMalformed checks that every corrupt input is rejected with a
// section name and an in-section offset, and never yields a half map.
func testMalformed(t *testing.T) {
	check := func(t *testing.T, b *execBuilder, textStart uint64, quantum uint8, section string) {
		t.Helper()
		m, err := b.file(t).SourceMap(textStart, quantum)
		TaskCheck(t, err != nil, "malformed input must fail")
		TaskCheck(t, m == nil, "no partial map on error")
		TaskCheck(t, strings.Contains(err.Error(), section), "error names the section")
		TaskCheck(t, strings.Contains(err.Error(), "offset"), "error carries an offset")
	}

	// Truncated 32-bit line delta at the start of pcs.
	trunc := newExecBuilder(Magic386, 4)
	trunc.pcsz = []byte{0, 0, 0}
	check(t, trunc, 0x1000, 1, "pcsz")

	// Undefined file number in a z record.
	badFile := newExecBuilder(Magic386, 4)
	badFile.z(1, 99)
	check(t, badFile, 0x1000, 1, "syms")

	// Orphan Z directive with an empty history stack.
	orphanZ := newExecBuilder(Magic386, 4)
	orphanZ.Z(3)
	check(t, orphanZ, 0x1000, 1, "syms")

	// History stack underflow: pop with nothing pushed.
	underflow := newExecBuilder(Magic386, 4)
	underflow.z(5)
	check(t, underflow, 0x1000, 1, "syms")

	// Truncated symbol value in a 64-bit symbol table.
	truncSym := newExecBuilder(MagicAMD64, 4)
	truncSym.syms.Write([]byte{0, 0, 0, 0, 0, 0})
	check(t, truncSym, 0x1000, 1, "syms")

	// Dangling component pair in a history record.
	dangling := newExecBuilder(Magic386, 4)
	dangling.putValue(&dangling.syms, 1)
	dangling.syms.WriteByte('z')
	dangling.syms.Write([]byte{0, 0}) // name NUL, then one orphan pair byte
	check(t, dangling, 0x1000, 1, "syms")

	// Program counter arithmetic overflow near the top of the
	// address space: byte 255 advances 126 extra quanta.
	over := newExecBuilder(Magic386, 1)
	over.pcsz = []byte{255}
	check(t, over, 0xFFFFFFFFFFFFFFFE, 1, "pcsz")
}
