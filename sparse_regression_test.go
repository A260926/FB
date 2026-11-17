// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package dwarf_test

// Sparse, >4 GiB DWARF regression tests. All DWARF is synthesized in
// memory: offsets beyond 4 GiB are purely logical, so neither a 4 GiB
// allocation nor a temporary file is ever needed.

import (
	"encoding/binary"
	"errors"
	"io"
	"testing"

	"github.com/blacktop/go-dwarf"
	"github.com/blacktop/go-dwarf/task_support/fixture"
	"github.com/blacktop/go-dwarf/task_support/taskcheck"
)

// Logical gap that pushes the second unit and several strings past the
// 4 GiB boundary.
const sparseGap = int64(1) << 32

// Abbreviation codes used by the synthesized units.
const (
	abCompileUnit = 1
	abBaseType    = 2
	abTypedef     = 3
	abPointer     = 4
	abBlockVar    = 5
	abPointerU    = 6
)

// DW_FORM values (the package does not export its form constants).
const (
	formBlock    uint64 = 0x09
	formData1    uint64 = 0x0B
	formStrp     uint64 = 0x0E
	formRefAddr  uint64 = 0x10
	formRef4     uint64 = 0x13
	formRefUdata uint64 = 0x15
)

// uleb128 encodes v as an unsigned LEB128 value.
func uleb128(v uint64) []byte {
	var out []byte
	for {
		c := byte(v & 0x7f)
		v >>= 7
		if v != 0 {
			out = append(out, c|0x80)
		} else {
			out = append(out, c)
			return out
		}
	}
}

// synthAbbrev builds the .debug_abbrev bytes shared by every unit.
func synthAbbrev() []byte {
	var b []byte
	put := func(code uint64, tag dwarf.Tag, children bool, fields ...[2]any) {
		b = append(b, uleb128(code)...)
		b = append(b, uleb128(uint64(tag))...)
		if children {
			b = append(b, 1)
		} else {
			b = append(b, 0)
		}
		for _, f := range fields {
			b = append(b, uleb128(uint64(f[0].(dwarf.Attr)))...)
			b = append(b, uleb128(f[1].(uint64))...)
		}
		b = append(b, 0, 0)
	}
	put(abCompileUnit, dwarf.TagCompileUnit, true,
		[2]any{dwarf.AttrName, formStrp})
	put(abBaseType, dwarf.TagBaseType, false,
		[2]any{dwarf.AttrName, formStrp},
		[2]any{dwarf.AttrEncoding, formData1},
		[2]any{dwarf.AttrByteSize, formData1})
	put(abTypedef, dwarf.TagTypedef, false,
		[2]any{dwarf.AttrName, formStrp},
		[2]any{dwarf.AttrType, formRefAddr})
	put(abPointer, dwarf.TagPointerType, false,
		[2]any{dwarf.AttrType, formRef4})
	put(abBlockVar, dwarf.TagVariable, false,
		[2]any{dwarf.AttrLocation, formBlock})
	put(abPointerU, dwarf.TagPointerType, false,
		[2]any{dwarf.AttrType, formRefUdata})
	b = append(b, 0)
	return b
}

// cuBuilder assembles one version-4 compilation unit, DWARF32 or
// DWARF64. Body-relative offsets start at zero; they map to absolute
// section offsets through the length-field plus the unit's placement.
type cuBuilder struct {
	order  binary.ByteOrder
	is64   bool
	body   []byte // bytes after the unit-length field
	labels map[string]uint32
}

func newCUBuilder(order binary.ByteOrder, is64 bool) *cuBuilder {
	c := &cuBuilder{order: order, is64: is64, labels: map[string]uint32{}}
	if is64 {
		// version(2) + abbrev offset(8) + address size(1).
		hdr := make([]byte, 11)
		order.PutUint16(hdr[0:], 4)
		// abbrev offset 0; address size 8.
		hdr[10] = 8
		c.body = append(c.body, hdr...)
	} else {
		// version(2) + abbrev offset(4) + address size(1).
		hdr := make([]byte, 7)
		order.PutUint16(hdr[0:], 4)
		hdr[6] = 8
		c.body = append(c.body, hdr...)
	}
	return c
}

// lenField is the size of the unit-length field that precedes body.
func (c *cuBuilder) lenField() int64 {
	if c.is64 {
		return 12
	}
	return 4
}

// rel is the current body-relative offset.
func (c *cuBuilder) rel() uint32 { return uint32(len(c.body)) }

// abs maps a body-relative offset to an absolute section offset.
func (c *cuBuilder) abs(unitBase int64, rel uint32) uint64 {
	return uint64(unitBase + c.lenField() + int64(rel))
}

func (c *cuBuilder) put16(v uint16) {
	b := make([]byte, 2)
	c.order.PutUint16(b, v)
	c.body = append(c.body, b...)
}

func (c *cuBuilder) put32(v uint32) {
	b := make([]byte, 4)
	c.order.PutUint32(b, v)
	c.body = append(c.body, b...)
}

func (c *cuBuilder) put64(v uint64) {
	b := make([]byte, 8)
	c.order.PutUint64(b, v)
	c.body = append(c.body, b...)
}

// putStrp writes a DW_FORM_strp offset whose width follows the unit's
// DWARF32/64 format.
func (c *cuBuilder) putStrp(off uint64) {
	if c.is64 {
		c.put64(off)
	} else {
		c.put32(uint32(off))
	}
}

// putRefAddr writes an absolute DW_FORM_ref_addr offset.
func (c *cuBuilder) putRefAddr(off uint64) {
	if c.is64 {
		c.put64(off)
	} else {
		c.put32(uint32(off))
	}
}

// beginCU emits the root compile_unit DIE opening its child list.
func (c *cuBuilder) beginCU(nameStrOff uint64) {
	c.body = append(c.body, uleb128(abCompileUnit)...)
	c.putStrp(nameStrOff)
}

// baseType emits a base-type DIE and records its body-relative offset.
func (c *cuBuilder) baseType(label string, nameStrOff uint64, enc, size byte) {
	c.labels[label] = c.rel()
	c.body = append(c.body, uleb128(abBaseType)...)
	c.putStrp(nameStrOff)
	c.body = append(c.body, enc, size)
}

// typedef emits a typedef whose target is an absolute info offset.
func (c *cuBuilder) typedef(label string, nameStrOff, absTarget uint64) {
	c.labels[label] = c.rel()
	c.body = append(c.body, uleb128(abTypedef)...)
	c.putStrp(nameStrOff)
	c.putRefAddr(absTarget)
}

// selfPointer emits a pointer type whose ref4 points at the DIE itself.
// The reference is always exactly four bytes and is relative to the
// unit header, even in DWARF64.
func (c *cuBuilder) selfPointer(label string) {
	dieRel := uint32(c.lenField()) + c.rel() // offset of the abbrev code byte
	c.labels[label] = c.rel()
	c.body = append(c.body, uleb128(abPointer)...)
	c.put32(dieRel)
}

// selfPointerUdata is like selfPointer but uses DW_FORM_ref_udata,
// which is a unit-relative LEB128 reference: widening Offset must not
// change the bytes it consumes.
func (c *cuBuilder) selfPointerUdata(label string) {
	dieRel := uint64(c.lenField()) + uint64(c.rel())
	c.labels[label] = c.rel()
	c.body = append(c.body, uleb128(abPointerU)...)
	c.body = append(c.body, uleb128(dieRel)...)
}

// blockVar emits a variable carrying a large DW_FORM_block value and
// returns the body-relative span of the block payload.
func (c *cuBuilder) blockVar(size int) (payloadStart, payloadEnd uint32) {
	c.body = append(c.body, uleb128(abBlockVar)...)
	c.body = append(c.body, uleb128(uint64(size))...)
	payloadStart = c.rel()
	c.body = append(c.body, make([]byte, size)...)
	payloadEnd = c.rel()
	return payloadStart, payloadEnd
}

// endCU appends the null terminator and returns the complete unit.
func (c *cuBuilder) endCU() []byte {
	c.body = append(c.body, 0)
	return c.wrap(uint64(len(c.body)))
}

// endCULength is like endCU but declares an explicit DWARF unit length.
// A length larger than the encoded body logically pads the unit with
// unread space, letting one unit span a multi-GiB gap so that the next
// unit header sits exactly where the declared length says it does.
func (c *cuBuilder) endCULength(declaredBodyLen uint64) []byte {
	c.body = append(c.body, 0)
	return c.wrap(declaredBodyLen)
}

func (c *cuBuilder) wrap(bodyLen uint64) []byte {
	if c.is64 {
		out := make([]byte, 12)
		out[0], out[1], out[2], out[3] = 0xff, 0xff, 0xff, 0xff
		c.order.PutUint64(out[4:], bodyLen)
		return append(out, c.body...)
	}
	out := make([]byte, 4)
	c.order.PutUint32(out, uint32(bodyLen))
	return append(out, c.body...)
}

// strTable is a sparse .debug_str: named NUL-terminated strings placed
// at explicit logical offsets inside a possibly huge section.
type strTable struct {
	size  int64
	spans []fixture.Span
}

func newStrTable(size int64) *strTable { return &strTable{size: size} }

func (s *strTable) add(off int64, text string) {
	s.spans = append(s.spans, fixture.Span{Offset: off, Data: append([]byte(text), 0)})
}

func (s *strTable) sparse() *fixture.Sparse {
	return &fixture.Sparse{Size: s.size, Spans: s.spans}
}

// guardReaderAt records every requested byte range and delegates to
// inner. It lets tests prove that a region was never touched.
type guardReaderAt struct {
	inner io.ReaderAt
	touch []spanRange
}

type spanRange struct{ off, n int64 }

func (g *guardReaderAt) ReadAt(p []byte, off int64) (int, error) {
	g.touch = append(g.touch, spanRange{off, int64(len(p))})
	return g.inner.ReadAt(p, off)
}

func (g *guardReaderAt) intersects(start, end int64) bool {
	for _, r := range g.touch {
		if r.off < end && r.off+r.n > start {
			return true
		}
	}
	return false
}

// errReaderAt fails ReadAt calls landing at a specific offset with a
// sentinel, and delegates everything else.
type errReaderAt struct {
	inner    io.ReaderAt
	failOff  int64
	sentinel error
}

func (e *errReaderAt) ReadAt(p []byte, off int64) (int, error) {
	if off == e.failOff {
		return 0, e.sentinel
	}
	return e.inner.ReadAt(p, off)
}

// shortReaderAt fails every read at or past failOff with an
// unexpected EOF, simulating a section whose bytes run out early.
type shortReaderAt struct {
	inner   io.ReaderAt
	failOff int64
}

func (s *shortReaderAt) ReadAt(p []byte, off int64) (int, error) {
	if off >= s.failOff {
		return 0, io.ErrUnexpectedEOF
	}
	if end := off + int64(len(p)); end > s.failOff {
		p = p[:s.failOff-off]
	}
	return s.inner.ReadAt(p, off)
}

// sparseLayout holds the pieces of a two-unit sparse .debug_info.
type sparseLayout struct {
	order      binary.ByteOrder
	infoSparse *fixture.Sparse
	infoGuard  *guardReaderAt
	strSparse  *fixture.Sparse
	infoSize   int64
	cu1Base    int64
	base0Abs   uint64
	base1Abs   uint64
	blockMid   int64 // offset deep inside the constant block
	blockEnd   int64
	nameStrOff uint64
	intStrOff  uint64
	wideStrOff int64
}

func buildSparse(order binary.ByteOrder, blockSize int) sparseLayout {
	const (
		nameStrOff = uint64(0)
		intStrOff  = uint64(9)
	)
	wideStrOff := sparseGap + 0x20000
	strs := newStrTable(wideStrOff + 32)
	strs.add(0, "cu-name")
	strs.add(int64(intStrOff), "int")
	strs.add(wideStrOff, "wide-int")

	// Unit 0 at offset 0. It is DWARF64 with a declared length that
	// logically spans the whole gap, so unit 1's header begins exactly
	// where unit 0's declared length ends: the layout is valid DWARF
	// even though the gap bytes are never stored or read.
	cu1Base := sparseGap + 0x10000
	c0 := newCUBuilder(order, true)
	c0.beginCU(nameStrOff)
	payStart, payEnd := c0.blockVar(blockSize)
	c0.baseType("base", intStrOff, 7, 4) // encUnsigned, size 4
	declared0 := uint64(cu1Base) - 12    // body length reaching unit 1
	unit0 := c0.endCULength(declared0)

	c1 := newCUBuilder(order, true)
	c1.beginCU(nameStrOff)
	c1.baseType("base", intStrOff, 7, 4)
	unit1 := c1.endCU()

	infoSize := cu1Base + int64(len(unit1))
	info := &fixture.Sparse{
		Size: infoSize,
		Spans: []fixture.Span{
			{Offset: 0, Data: unit0},
			{Offset: cu1Base, Data: unit1},
		},
	}
	return sparseLayout{
		order:      order,
		infoSparse: info,
		infoGuard:  &guardReaderAt{inner: info},
		strSparse:  strs.sparse(),
		infoSize:   infoSize,
		cu1Base:    cu1Base,
		base0Abs:   c0.abs(0, c0.labels["base"]),
		base1Abs:   c1.abs(cu1Base, c1.labels["base"]),
		blockMid:   int64(c0.abs(0, (payStart+payEnd)/2)),
		blockEnd:   int64(c0.abs(0, payEnd)),
		nameStrOff: nameStrOff,
		intStrOff:  intStrOff,
		wideStrOff: wideStrOff,
	}
}

func orderName(o binary.ByteOrder) string {
	if o == binary.BigEndian {
		return "big-endian"
	}
	return "little-endian"
}

func TestSparseUnits(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		for _, order := range []binary.ByteOrder{binary.LittleEndian, binary.BigEndian} {
			layout := buildSparse(order, 1<<18)
			d, err := dwarf.NewReaderAt(synthAbbrev(),
				dwarf.Section{Reader: layout.infoGuard, Size: layout.infoSize},
				dwarf.Section{Reader: layout.strSparse, Size: layout.strSparse.Size})
			c.Check(err == nil, "%s NewReaderAt: %v", orderName(order), err)

			// Construction reads headers only; the far half of the
			// constant block and the bulk of the string section
			// stay untouched.
			c.Check(!layout.infoGuard.intersects(layout.blockMid, layout.blockEnd),
				"construction read the constant block")

			r := d.Reader()
			cu, err := r.Next()
			c.Check(err == nil, "first Next: %v", err)
			c.Check(cu != nil && cu.Tag == dwarf.TagCompileUnit, "first entry is compile unit")
			c.Check(cu.Val(dwarf.AttrName) == "cu-name", "CU name resolved from sparse str")

			// Skipping the CU children jumps to the next unit
			// without decoding the huge block.
			r.SkipChildren()
			cu1, err := r.Next()
			c.Check(err == nil, "Next after SkipChildren: %v", err)
			c.Check(cu1 != nil && cu1.Tag == dwarf.TagCompileUnit, "reached second compile unit")
			c.Check(cu1.Offset >= dwarf.Offset(sparseGap), "second unit is beyond 4 GiB, got %#x", cu1.Offset)
			c.Check(!layout.infoGuard.intersects(layout.blockMid, layout.blockEnd),
				"SkipChildren read the constant block")

			base1, err := r.Next()
			c.Check(err == nil, "base type Next: %v", err)
			c.Check(base1 != nil && base1.Tag == dwarf.TagBaseType, "base type decoded")
			c.Check(base1.Offset == dwarf.Offset(layout.base1Abs),
				"base type keeps full 64-bit offset %#x", base1.Offset)
			c.Check(base1.Val(dwarf.AttrName) == "int", "sparse string value %q", base1.Val(dwarf.AttrName))

			// Random Seek straight past 4 GiB.
			r2 := d.Reader()
			r2.Seek(dwarf.Offset(layout.base1Abs))
			got, gerr := r2.Next()
			c.Check(gerr == nil, "Seek+Next past 4 GiB: %v", gerr)
			c.Check(got != nil && got.Offset == dwarf.Offset(layout.base1Abs), "Seek landed on the right DIE")

			// On-demand Type resolution past 4 GiB.
			typ, terr := d.Type(dwarf.Offset(layout.base1Abs))
			c.Check(terr == nil, "Type past 4 GiB: %v", terr)
			c.Check(typ != nil && typ.String() == "int" && typ.Size() == 4,
				"type is int/4, got %v", typ)

			// The low-offset DIE in unit 0 is reachable too.
			lowType, lerr := d.Type(dwarf.Offset(layout.base0Abs))
			c.Check(lerr == nil, "Type in first unit: %v", lerr)
			c.Check(lowType != nil && lowType.String() == "int", "first-unit type resolves")

			// The >4 GiB string section is read in small windows,
			// never as a whole.
			c.Check(layout.strSparse.Bytes < 1<<20,
				"string reads stayed on-demand (%d bytes fetched)", layout.strSparse.Bytes)

			// Same logical content through the classic slice API
			// must yield equivalent DIE shape and type.
			sliceBuilder := newCUBuilder(order, false)
			sliceBuilder.beginCU(layout.nameStrOff)
			sliceBuilder.baseType("base", layout.intStrOff, 7, 4)
			contig := sliceBuilder.endCU()
			// Match the sparse string layout exactly: "cu-name"
			// at 0, a padding zero at 8, "int" at offset 9.
			cd, nerr := dwarf.New(synthAbbrev(), nil, nil, contig, nil, nil, nil,
				[]byte("cu-name\x00\x00int\x00"))
			c.Check(nerr == nil, "slice New: %v", nerr)
			ce, nerr2 := cd.Reader().Next()
			c.Check(nerr2 == nil, "slice Next: %v", nerr2)
			c.Check(ce.Tag == cu1.Tag && ce.Children == cu1.Children,
				"slice and sparse DIE shape match")
			baseSliceOff := dwarf.Offset(sliceBuilder.abs(0, sliceBuilder.labels["base"]))
			ct, nerr3 := cd.Type(baseSliceOff)
			c.Check(nerr3 == nil, "slice Type: %v", nerr3)
			c.Check(ct.String() == typ.String() && ct.Size() == typ.Size(),
				"slice and sparse type match")
		}
	})
}

func TestWideReferences(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		for _, order := range []binary.ByteOrder{binary.LittleEndian, binary.BigEndian} {
			checkWideReferences(c, order)
		}
	})
}

func checkWideReferences(c *taskcheck.C, order binary.ByteOrder) {
	// DWARF64 units so DW_FORM_strp and DW_FORM_ref_addr are eight
	// bytes wide and can legitimately address data beyond 4 GiB, while
	// DW_FORM_ref4 stays four bytes and unit-relative.
	strs := newStrTable(sparseGap + 0x40000)
	strs.add(0, "cu-name")
	strs.add(9, "int")
	strs.add(13, "myint")
	wideStrOff := sparseGap + 0x20000
	strs.add(wideStrOff, "wide-int")
	strSparse := strs.sparse()

	// Unit 0 at base 0 (DWARF64), with a declared length
	// spanning the whole gap so unit 1 follows it legally.
	cu1Base := sparseGap
	c0 := newCUBuilder(order, true)
	c0.beginCU(0)
	c0.baseType("base", 9, 7, 4)
	unit0 := c0.endCULength(uint64(cu1Base) - 12)
	lowOff := dwarf.Offset(c0.abs(0, c0.labels["base"]))

	// Unit 1 sits at the 4 GiB boundary so that adding the
	// same body delta reproduces unit 0's low 32 bits
	// while setting a different high bit.
	c1 := newCUBuilder(order, true)
	c1.beginCU(0)
	c1.baseType("base", uint64(wideStrOff), 7, 4)
	c1.typedef("td", 13, uint64(lowOff))
	unit1 := c1.endCU()
	highOff := dwarf.Offset(c1.abs(cu1Base, c1.labels["base"]))
	tdOff := dwarf.Offset(c1.abs(cu1Base, c1.labels["td"]))

	c.Check(uint32(highOff) == uint32(lowOff),
		"low 32 bits collide: high=%#x low=%#x", highOff, lowOff)
	c.Check(highOff != lowOff, "full offsets stay distinct")
	c.Check(highOff >= dwarf.Offset(sparseGap), "high DIE beyond 4 GiB")

	infoSize := cu1Base + int64(len(unit1))
	info := &fixture.Sparse{
		Size: infoSize,
		Spans: []fixture.Span{
			{Offset: 0, Data: unit0},
			{Offset: cu1Base, Data: unit1},
		},
	}
	d, err := dwarf.NewReaderAt(synthAbbrev(),
		dwarf.Section{Reader: info, Size: infoSize},
		dwarf.Section{Reader: strSparse, Size: strSparse.Size})
	c.Check(err == nil, "NewReaderAt: %v", err)

	lowType, err := d.Type(lowOff)
	c.Check(err == nil, "low Type: %v", err)
	highType, err := d.Type(highOff)
	c.Check(err == nil, "high Type: %v", err)
	c.Check(lowType.String() == "int" && highType.String() == "wide-int",
		"same-low-32 DIEs resolve to distinct types: %q vs %q", lowType, highType)

	// Distinct cache identities survive repeat lookups.
	againLow, err := d.Type(lowOff)
	c.Check(err == nil, "low re-Type: %v", err)
	againHigh, err := d.Type(highOff)
	c.Check(err == nil, "high re-Type: %v", err)
	c.Check(againLow == lowType && againHigh == highType && lowType != highType,
		"type cache distinguishes colliding low-32-bit offsets")

	// Cross-unit typedef follows the 8-byte ref_addr back
	// across the 4 GiB gap.
	td, err := d.Type(tdOff)
	c.Check(err == nil, "cross-unit typedef Type: %v", err)
	tt, ok := td.(*dwarf.TypedefType)
	c.Check(ok, "typedef decoded as *TypedefType, got %T", td)
	if ok {
		c.Check(tt.Type.String() == "int", "typedef follows ref_addr across 4 GiB")
		// A shared target resolves to the identical cached object.
		c.Check(tt.Type == lowType, "cross-unit typedef shares the target type object")
	}

	// Recursive pointer: a DW_FORM_ref4 (still 4 bytes in
	// DWARF64) pointing at the DIE itself.
	c2 := newCUBuilder(order, true)
	c2.beginCU(0)
	c2.selfPointer("self")
	unit2 := c2.endCU()
	info2 := &fixture.Sparse{
		Size:  int64(len(unit2)),
		Spans: []fixture.Span{{Offset: 0, Data: unit2}},
	}
	d2, err := dwarf.NewReaderAt(synthAbbrev(),
		dwarf.Section{Reader: info2, Size: int64(len(unit2))},
		dwarf.Section{Reader: strSparse, Size: strSparse.Size})
	c.Check(err == nil, "self-pointer NewReaderAt: %v", err)
	selfOff := dwarf.Offset(c2.abs(0, c2.labels["self"]))
	pt, err := d2.Type(selfOff)
	c.Check(err == nil, "self-pointer Type: %v", err)
	ptr, ok := pt.(*dwarf.PtrType)
	c.Check(ok, "pointer decoded as *PtrType, got %T", pt)
	if ok {
		c.Check(ptr.Type == pt, "recursive pointer resolves to itself")
		c.Check(ptr.Size() == 8, "pointer uses the unit address size")
	}

	// The same recursion through DW_FORM_ref_udata: a LEB128,
	// unit-relative reference that keeps its narrow encoding.
	c3 := newCUBuilder(order, true)
	c3.beginCU(0)
	c3.selfPointerUdata("self")
	unit3 := c3.endCU()
	info3 := &fixture.Sparse{
		Size:  int64(len(unit3)),
		Spans: []fixture.Span{{Offset: 0, Data: unit3}},
	}
	d3, err := dwarf.NewReaderAt(synthAbbrev(),
		dwarf.Section{Reader: info3, Size: int64(len(unit3))},
		dwarf.Section{Reader: strSparse, Size: strSparse.Size})
	c.Check(err == nil, "self-pointer-udata NewReaderAt: %v", err)
	selfOff3 := dwarf.Offset(c3.abs(0, c3.labels["self"]))
	pt3, err := d3.Type(selfOff3)
	c.Check(err == nil, "self-pointer-udata Type: %v", err)
	ptr3, ok := pt3.(*dwarf.PtrType)
	c.Check(ok, "udata pointer decoded as *PtrType, got %T", pt3)
	if ok {
		c.Check(ptr3.Type == pt3, "ref_udata recursion resolves to itself")
	}
}

func TestSparseIsolation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		order := binary.LittleEndian
		layout := buildSparse(order, 256)

		good, err := dwarf.NewReaderAt(synthAbbrev(),
			dwarf.Section{Reader: layout.infoGuard, Size: layout.infoSize},
			dwarf.Section{Reader: layout.strSparse, Size: layout.strSparse.Size})
		c.Check(err == nil, "good NewReaderAt: %v", err)

		// Complete a type before any failure happens.
		completed, err := good.Type(dwarf.Offset(layout.base1Abs))
		c.Check(err == nil, "complete type up front: %v", err)
		c.Check(completed.String() == "int", "completed type is int")

		// A second, independent reader parked on the far unit.
		parked := good.Reader()
		parked.Seek(dwarf.Offset(layout.base1Abs))

		// Transport failure injected at the far DIE offset.
		sentinel := errors.New("injected sparse transport failure")
		badData := dwarf.Section{
			Reader: &errReaderAt{inner: layout.infoSparse, failOff: int64(layout.base1Abs), sentinel: sentinel},
			Size:   layout.infoSize,
		}
		bad, err := dwarf.NewReaderAt(synthAbbrev(), badData,
			dwarf.Section{Reader: layout.strSparse, Size: layout.strSparse.Size})
		c.Check(err == nil, "bad NewReaderAt still builds from headers: %v", err)
		br := bad.Reader()
		br.Seek(dwarf.Offset(layout.base1Abs))
		_, ferr := br.Next()
		c.Check(ferr != nil, "failing read reports an error")
		var de dwarf.DecodeError
		c.Check(errors.As(ferr, &de), "error is a DecodeError, got %T", ferr)
		c.Check(errors.Is(ferr, sentinel), "underlying cause is reachable via errors.Is")
		c.Check(de.Name == "info", "error names the info section, got %q", de.Name)
		c.Check(de.Offset == dwarf.Offset(layout.base1Abs), "error carries the real offset %#x", de.Offset)

		// The parked reader's cursor is unaffected and still reads.
		pe, perr := parked.Next()
		c.Check(perr == nil, "parked reader still works after another Data failed: %v", perr)
		c.Check(pe != nil && pe.Offset == dwarf.Offset(layout.base1Abs), "parked cursor unchanged")

		// A failing reader on the same good Data must not disturb a
		// sibling reader or the completed type.
		failing := good.Reader()
		failing.Seek(dwarf.Offset(layout.infoSize + 10))
		_, oobErr := failing.Next()
		c.Check(oobErr != nil, "out-of-bounds Seek is rejected on Next")
		fresh := good.Reader()
		fe, ferr2 := fresh.Next()
		c.Check(ferr2 == nil, "sibling reader reads after out-of-bounds failure: %v", ferr2)
		c.Check(fe != nil && fe.Tag == dwarf.TagCompileUnit, "sibling reader at first CU")
		c.Check(completed.String() == "int" && completed.Size() == 4, "completed type survives failures")

		// Short read: EOF-class cause, real section name and offset.
		shortData := dwarf.Section{
			Reader: &shortReaderAt{inner: layout.infoSparse, failOff: int64(layout.base1Abs)},
			Size:   layout.infoSize,
		}
		sd, err := dwarf.NewReaderAt(synthAbbrev(), shortData,
			dwarf.Section{Reader: layout.strSparse, Size: layout.strSparse.Size})
		c.Check(err == nil, "short NewReaderAt builds: %v", err)
		sr := sd.Reader()
		sr.Seek(dwarf.Offset(layout.base1Abs))
		_, serr := sr.Next()
		c.Check(serr != nil, "short read reports an error")
		c.Check(errors.Is(serr, io.ErrUnexpectedEOF) || errors.Is(serr, io.EOF),
			"short read wraps an EOF-class cause")
		var sde dwarf.DecodeError
		c.Check(errors.As(serr, &sde) && sde.Name == "info" && sde.Offset == dwarf.Offset(layout.base1Abs),
			"short-read error names section and offset")

		// Declared section size bounds: claim a larger .debug_str
		// than the backing reader actually serves, then force a read
		// past the real bytes. The failure must name the str section,
		// carry the real offset, and expose an EOF-class cause.
		strClaimed := dwarf.Section{
			Reader: layout.strSparse,
			Size:   layout.strSparse.Size + 1<<20,
		}
		// Unit 0 spans the gap; unit 1 (past 4 GiB) holds the base
		// type whose strp points beyond the backing string bytes.
		spanner := newCUBuilder(order, true)
		spanner.beginCU(0)
		spanner.baseType("base", 9, 7, 4)
		spanUnit := spanner.endCULength(uint64(layout.cu1Base) - 12)

		far1 := newCUBuilder(order, true)
		far1.beginCU(0)
		far1.baseType("far", uint64(layout.strSparse.Size)+64, 7, 4)
		farUnit := far1.endCU()
		farInfo := &fixture.Sparse{
			Size: layout.cu1Base + int64(len(farUnit)),
			Spans: []fixture.Span{
				{Offset: 0, Data: spanUnit},
				{Offset: layout.cu1Base, Data: farUnit},
			},
		}
		farData, ferr3 := dwarf.NewReaderAt(synthAbbrev(),
			dwarf.Section{Reader: farInfo, Size: farInfo.Size}, strClaimed)
		c.Check(ferr3 == nil, "far-str NewReaderAt: %v", ferr3)
		farOff := dwarf.Offset(far1.abs(layout.cu1Base, far1.labels["far"]))
		_, strErr := farData.Type(farOff)
		c.Check(strErr != nil, "read past backing str data reports an error")
		var strDE dwarf.DecodeError
		c.Check(errors.As(strErr, &strDE), "str error is a DecodeError, got %T", strErr)
		c.Check(strDE.Name == "str", "str error names the str section, got %q", strDE.Name)
		c.Check(strDE.Offset >= dwarf.Offset(layout.strSparse.Size), "str error carries the real offset %#x", strDE.Offset)
		c.Check(errors.Is(strErr, io.EOF) || errors.Is(strErr, io.ErrUnexpectedEOF),
			"str error wraps an EOF-class cause")
	})
}
