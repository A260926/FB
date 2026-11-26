// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package dwarf_test

import (
	"encoding/binary"
	"errors"
	"io"
	"testing"

	"github.com/blacktop/go-dwarf"
	"github.com/blacktop/go-dwarf/task_support/fixture"
	"github.com/blacktop/go-dwarf/task_support/taskcheck"
)

// Raw DWARF constants mirroring the package's unexported form/tag/attr
// values, so the test drives only the public API.
const (
	tagCompileUnit = 0x11
	tagPointerType = 0x0F
	tagStructType  = 0x13
	tagTypedef     = 0x16
	tagBaseType    = 0x24
	tagMember      = 0x0D
	tagConstant    = 0x27

	attrName       = 0x03
	attrConstValue = 0x1C
	attrByteSize   = 0x0B
	attrEncoding   = 0x3E
	attrType       = 0x49
	attrMemberLoc  = 0x38

	formBlock   = 0x09
	formData1   = 0x0B
	formData4   = 0x06
	formStrp    = 0x0E
	formRefAddr = 0x10
	formRef1    = 0x11
	formRef2    = 0x12
	formRef4    = 0x13

	encSigned     = 0x05
	dwarf64Marker = 0xFFFFFFFF
	header32      = 11 // length(4)+version(2)+abbrev off(4)+addr size(1)
	header64      = 23 // marker(4)+length(8)+version(2)+abbrev off(8)+addr size(1)
)

// Logical positions used by the sparse layout. The multi-gigabyte
// regions between them are never allocated.
const (
	sparseUnitB   = int64(0x1_0000_0000) // first unit past the 4 GiB line
	sparseHighStr = int64(0x1_0000_8000) // a string living past 4 GiB

	// Small, fully materialized positions for the compact comparison.
	compactUnitB   = int64(0x400)
	compactHighStr = int64(0x200)
)

// Abbreviation codes.
const (
	abCU = 1 + iota
	abBase
	abPtr
	abTypedefAbs
	abStruct
	abMember
	abRefProbe
	abBlock
	abBaseBadStr
)

type eb struct {
	order binary.ByteOrder
	buf   []byte
}

func (b *eb) u8(v int)     { b.buf = append(b.buf, byte(v)) }
func (b *eb) u16(v uint16) { b.buf = append(b.buf, 0, 0); b.order.PutUint16(b.buf[len(b.buf)-2:], v) }
func (b *eb) u32(v uint32) {
	b.buf = append(b.buf, 0, 0, 0, 0)
	b.order.PutUint32(b.buf[len(b.buf)-4:], v)
}
func (b *eb) u64(v uint64) {
	b.buf = append(b.buf, make([]byte, 8)...)
	b.order.PutUint64(b.buf[len(b.buf)-8:], v)
}
func (b *eb) uleb(v uint64) {
	for v >= 0x80 {
		b.buf = append(b.buf, byte(v)|0x80)
		v >>= 7
	}
	b.buf = append(b.buf, byte(v))
}

// wide writes a DW_FORM_strp / DW_FORM_ref_addr value at the width
// dictated by the unit's DWARF 32/64 format.
func (b *eb) wide(v uint64, is64 bool) {
	if is64 {
		b.u64(v)
	} else {
		b.u32(uint32(v))
	}
}

func abbrevSection() []byte {
	var a eb
	decl := func(code, tag int, children bool, attrs ...[2]int) {
		a.uleb(uint64(code))
		a.uleb(uint64(tag))
		if children {
			a.u8(1)
		} else {
			a.u8(0)
		}
		for _, at := range attrs {
			a.uleb(uint64(at[0]))
			a.uleb(uint64(at[1]))
		}
		a.u8(0)
		a.u8(0)
	}
	decl(abCU, tagCompileUnit, true, [2]int{attrName, formStrp})
	decl(abBase, tagBaseType, false,
		[2]int{attrName, formStrp}, [2]int{attrEncoding, formData1}, [2]int{attrByteSize, formData1})
	decl(abBaseBadStr, tagBaseType, false,
		[2]int{attrName, formStrp}, [2]int{attrEncoding, formData1}, [2]int{attrByteSize, formData1})
	decl(abPtr, tagPointerType, false, [2]int{attrType, formRef4})
	decl(abTypedefAbs, tagTypedef, false, [2]int{attrName, formStrp}, [2]int{attrType, formRefAddr})
	decl(abStruct, tagStructType, true, [2]int{attrName, formStrp}, [2]int{attrByteSize, formData4})
	decl(abMember, tagMember, false,
		[2]int{attrName, formStrp}, [2]int{attrType, formRef4}, [2]int{attrMemberLoc, formData1})
	// One DIE carrying each unit-relative reference width.
	decl(abRefProbe, tagConstant, false,
		[2]int{attrConstValue, formRef1},
		[2]int{attrConstValue, formRef2},
		[2]int{attrConstValue, formRef4})
	decl(abBlock, tagConstant, false, [2]int{attrConstValue, formBlock})
	a.u8(0)
	return a.buf
}

// synthesized holds one logically identical DWARF fixture, placed
// either sparsely beyond 4 GiB (all dwarf64) or compactly in ordinary
// slices (all dwarf32).
type synthesized struct {
	order  binary.ByteOrder
	is64   bool
	hdr    int
	abbrev []byte

	infoSpan []fixture.Span
	strSpan  []fixture.Span
	infoSize int64
	strSize  int64

	blockGapLo, blockGapHi int64

	baseA   dwarf.Offset // base_type "int" in unit A (offset 0)
	baseB   dwarf.Offset // base_type "int" in unit B (same low 32 bits in sparse)
	ptrB    dwarf.Offset
	structB dwarf.Offset
	typedef dwarf.Offset // cross-unit ref_addr typedef
	badStr  dwarf.Offset // DIE whose strp points past .debug_str
	cuC     dwarf.Offset

	highStrName string
	badStrOff   dwarf.Offset
}

type placement struct {
	unitB, highStr int64
}

// build constructs the fixture for one byte order, placement and width.
func build(order binary.ByteOrder, p placement, blockEnd int64, is64 bool) *synthesized {
	hdr := header32
	if is64 {
		hdr = header64
	}
	s := &synthesized{
		order:       order,
		is64:        is64,
		hdr:         hdr,
		abbrev:      abbrevSection(),
		highStrName: "high_string",
	}

	// ---- .debug_str ----
	strb := &eb{order: order}
	strOff := map[string]uint64{}
	putStr := func(name string) uint64 {
		if off, ok := strOff[name]; ok {
			return off
		}
		off := uint64(len(strb.buf))
		strOff[name] = off
		strb.buf = append(strb.buf, []byte(name)...)
		strb.u8(0)
		return off
	}
	for _, n := range []string{"cu0", "int", "cu1", "node", "link", "alias", "cu2"} {
		putStr(n)
	}
	s.strSpan = append(s.strSpan, fixture.Span{Offset: 0, Data: strb.buf})
	highBytes := append([]byte(s.highStrName), 0)
	s.strSpan = append(s.strSpan, fixture.Span{Offset: p.highStr, Data: highBytes})
	s.strSize = p.highStr + int64(len(highBytes)) + 16
	s.badStrOff = dwarf.Offset(s.strSize + 5)

	// ---- Unit A (starts at 0; spans to blockEnd) ----
	a := &eb{order: order}
	a.uleb(abCU)
	a.wide(putStr("cu0"), is64)
	cuBodyLen := len(a.buf) // body offset where the CU DIE ends / base DIE begins
	a.uleb(abBase)
	a.wide(putStr("int"), is64)
	a.u8(encSigned)
	a.u8(4)
	s.baseA = dwarf.Offset(hdr + cuBodyLen)

	// Block DIE. Its declared payload pads the unit exactly to blockEnd;
	// the payload and terminator live in the (possibly huge) gap and are
	// never materialized.
	a.uleb(abBlock)
	lenSlot := len(a.buf)
	a.buf = append(a.buf, 0, 0, 0, 0, 0) // fixed 5-byte ULEB128 slot
	payloadOff := len(a.buf)
	dieRegion := uint64(blockEnd - int64(hdr))     // total DIE-stream length of unit A
	blockLen := dieRegion - uint64(payloadOff) - 1 // one byte for the null terminator
	patchULEB5(a.buf, lenSlot, blockLen)
	bodyA := a.buf[:payloadOff] // payload bytes are not backed
	unitA := packUnit(order, bodyA, dieRegion, is64)
	s.infoSpan = append(s.infoSpan, fixture.Span{Offset: 0, Data: unitA})
	// The protected region sits in the middle of the unbacked block,
	// far from the header windows anchored at either unit boundary.
	mid := blockEnd / 2
	s.blockGapLo = mid - 4096
	s.blockGapHi = mid + 4096

	// ---- Unit B (same width, identical DIE prefix as unit A) ----
	b0 := &eb{order: order}
	b0.uleb(abCU)
	b0.wide(putStr("cu1"), is64)
	b0.uleb(abBase) // same in-unit position as unit A's base_type
	b0.wide(putStr("int"), is64)
	b0.u8(encSigned)
	b0.u8(4)
	s.baseB = dwarf.Offset(p.unitB) + dwarf.Offset(hdr+cuBodyLen)

	ptrLocal := len(b0.buf)
	b0.uleb(abPtr)
	ptrPatch := len(b0.buf)
	b0.u32(0)

	structLocal := len(b0.buf)
	b0.uleb(abStruct)
	b0.wide(putStr("node"), is64)
	b0.u32(16)
	b0.uleb(abMember)
	b0.wide(putStr("link"), is64)
	// ref4 is relative to the unit header base, not the DIE stream.
	b0.u32(uint32(hdr + ptrLocal)) // member -> pointer
	b0.u8(0)
	b0.u8(0)                                                       // end struct children
	b0.order.PutUint32(b0.buf[ptrPatch:], uint32(hdr+structLocal)) // pointer -> struct
	s.ptrB = dwarf.Offset(p.unitB) + dwarf.Offset(hdr+ptrLocal)
	s.structB = dwarf.Offset(p.unitB) + dwarf.Offset(hdr+structLocal)

	// Reference-width probe: ref1/ref2/ref4 stay 1/2/4 bytes and stay
	// relative to the unit header, all resolving to baseB.
	rel := uint32(hdr + cuBodyLen)
	b0.uleb(abRefProbe)
	b0.u8(int(rel & 0xff))
	b0.u16(uint16(rel))
	b0.u32(rel)

	typedefLocal := len(b0.buf)
	b0.uleb(abTypedefAbs)
	b0.wide(putStr("alias"), is64)
	b0.wide(uint64(s.baseA), is64) // absolute DW_FORM_ref_addr
	s.typedef = dwarf.Offset(p.unitB) + dwarf.Offset(hdr+typedefLocal)
	b0.u8(0) // end unit B children
	unitB := packUnit(order, b0.buf, uint64(len(b0.buf)), is64)
	s.infoSpan = append(s.infoSpan, fixture.Span{Offset: p.unitB, Data: unitB})
	// Unit C is contiguous with unit B: DWARF units are packed, so a
	// zero-filled gap between units would read as an end-of-section
	// zero length. It still lies past 4 GiB in the sparse layout.
	unitCPos := p.unitB + int64(len(unitB))

	// ---- Unit C (same width; high-offset strings + a bad strp) ----
	c := &eb{order: order}
	c.uleb(abCU)
	c.wide(uint64(p.highStr), is64)
	c.uleb(abTypedefAbs)
	c.wide(uint64(p.highStr), is64)
	c.wide(uint64(s.baseA), is64)
	s.badStr = dwarf.Offset(unitCPos) + dwarf.Offset(hdr+len(c.buf)) // start of the bad base_type DIE
	c.uleb(abBaseBadStr)
	c.wide(uint64(s.badStrOff), is64) // points past the .debug_str end
	c.u8(encSigned)
	c.u8(4)
	c.u8(0) // end unit C children
	s.cuC = dwarf.Offset(unitCPos)
	unitC := packUnit(order, c.buf, uint64(len(c.buf)), is64)
	s.infoSpan = append(s.infoSpan, fixture.Span{Offset: unitCPos, Data: unitC})
	s.infoSize = unitCPos + int64(len(unitC)) + 16

	return s
}

// patchULEB5 writes v into a fixed 5-byte ULEB128 slot. The first four
// bytes carry the continuation bit, so a decoder consumes exactly five.
func patchULEB5(buf []byte, off int, v uint64) {
	for i := 0; i < 5; i++ {
		b := byte(v & 0x7f)
		v >>= 7
		if i < 4 {
			b |= 0x80
		}
		buf[off+i] = b
	}
}

func packUnit(order binary.ByteOrder, body []byte, bodyLen uint64, is64 bool) []byte {
	// The unit-length field counts every byte following it: the fixed
	// header tail (version, abbrev offset, address size) plus the DIE
	// stream. bodyLen may exceed len(body) when the tail is an
	// unbacked, sparse block.
	fixedTail := uint64(7) // version(2)+abbrev(4)+addr(1) in dwarf32
	if is64 {
		fixedTail = 11 // version(2)+abbrev(8)+addr(1) in dwarf64
	}
	length := fixedTail + bodyLen
	out := &eb{order: order}
	if is64 {
		out.u32(dwarf64Marker)
		out.u64(length)
		out.u16(4)
		out.u64(0) // abbrev offset
	} else {
		out.u32(uint32(length))
		out.u16(4)
		out.u32(0) // abbrev offset
	}
	out.u8(8) // address size
	out.buf = append(out.buf, body...)
	return out.buf
}

// materialize turns the spans into one contiguous, fully-backed
// buffer (used only with small, compact placements).
func materialize(spans []fixture.Span, size int64) []byte {
	buf := make([]byte, size)
	for _, sp := range spans {
		copy(buf[sp.Offset:], sp.Data)
	}
	return buf
}

func (s *synthesized) sparseSections() (dwarf.Section, dwarf.Section) {
	info := &fixture.Sparse{Size: s.infoSize, Spans: s.infoSpan}
	str := &fixture.Sparse{Size: s.strSize, Spans: s.strSpan}
	return dwarf.Section{Reader: info, Size: s.infoSize},
		dwarf.Section{Reader: str, Size: s.strSize}
}

// recordedReader records read ranges and can fail or short-read reads
// that start inside a chosen interval.
type recordedReader struct {
	inner   io.ReaderAt
	ranges  *[]readRange
	armed   bool
	short   bool
	failLo  int64
	failHi  int64
	failErr error
}

type readRange struct{ off, n int64 }

func (r *recordedReader) ReadAt(p []byte, off int64) (int, error) {
	*r.ranges = append(*r.ranges, readRange{off, int64(len(p))})
	if r.armed && off >= r.failLo && off < r.failHi {
		if r.short {
			n := len(p) / 2
			if n == 0 {
				n = 1
			}
			return r.inner.ReadAt(p[:n], off)
		}
		return 0, r.failErr
	}
	return r.inner.ReadAt(p, off)
}

func touchesAny(ranges []readRange, lo, hi int64) bool {
	for _, rg := range ranges {
		if rg.off < hi && rg.off+rg.n > lo {
			return true
		}
	}
	return false
}

func tagOf(e *dwarf.Entry) dwarf.Tag {
	if e == nil {
		return 0
	}
	return e.Tag
}

func offOf(e *dwarf.Entry) dwarf.Offset {
	if e == nil {
		return 0
	}
	return e.Offset
}

func typeName(t dwarf.Type) string {
	if t == nil {
		return "<nil>"
	}
	return t.String()
}

// TestSparseUnits: jumping across the 4 GiB boundary, skipping a unit
// with a huge unbacked constant block, sparse high-offset strings, and
// both byte orders.
func TestSparseUnits(t *testing.T) {
	for _, order := range []binary.ByteOrder{binary.LittleEndian, binary.BigEndian} {
		s := build(order, placement{sparseUnitB, sparseHighStr}, sparseUnitB, true)
		taskcheck.Run(t, func(c *taskcheck.C) {
			infoSec, strSec := s.sparseSections()
			var ranges []readRange
			tracked := &recordedReader{inner: infoSec.Reader.(io.ReaderAt), ranges: &ranges}
			d, err := dwarf.NewReaderAt(s.abbrev,
				dwarf.Section{Reader: tracked, Size: s.infoSize}, strSec)
			c.Check(err == nil, "NewReaderAt: %v", err)

			// Construction reads only small header windows and never
			// touches the deep constant-block gap.
			c.Check(!touchesAny(ranges, s.blockGapLo, s.blockGapHi),
				"construction read inside the constant block gap")
			var total int64
			for _, rg := range ranges {
				total += rg.n
			}
			c.Check(total < 1<<16, "construction read %d bytes for a >4GiB section", total)

			r := d.Reader()
			cu, err := r.Next()
			c.Check(err == nil && tagOf(cu) == dwarf.TagCompileUnit, "unit A CU: %v %v", tagOf(cu), err)

			// Skipping unit A's children must not read its block gap.
			ranges = nil
			r.SkipChildren()
			c.Check(!touchesAny(ranges, s.blockGapLo, s.blockGapHi),
				"SkipChildren read the constant block gap")

			// Sequential iteration lands in unit B beyond 4 GiB.
			cuB, err := r.Next()
			c.Check(err == nil && tagOf(cuB) == dwarf.TagCompileUnit, "unit B CU: %v %v", tagOf(cuB), err)
			c.Check(cuB != nil && cuB.Offset >= dwarf.Offset(sparseUnitB),
				"unit B offset %#x not past 4GiB", offOf(cuB))
			r.SkipChildren()

			// Unit C is contiguous with B and also lies past 4 GiB;
			// its name string lives at a high .debug_str offset.
			cuC, err := r.Next()
			c.Check(err == nil && tagOf(cuC) == dwarf.TagCompileUnit, "unit C CU: %v %v", tagOf(cuC), err)
			c.Check(cuC != nil && cuC.Offset > cuB.Offset && cuC.Offset >= dwarf.Offset(sparseUnitB),
				"unit C offset %#x", offOf(cuC))
			name, _ := cuC.Val(dwarf.AttrName).(string)
			c.Check(name == s.highStrName, "high sparse string %q want %q", name, s.highStrName)
			c.Check(r.ByteOrder() == order, "byte order = %v", r.ByteOrder())

			// Random Seek straight to unit B, independent of scanning.
			r2 := d.Reader()
			r2.Seek(s.baseB)
			base, err := r2.Next()
			c.Check(err == nil && tagOf(base) == dwarf.TagBaseType, "seek baseB: %v %v", tagOf(base), err)
			c.Check(base != nil && base.Offset == s.baseB, "seek offset %#x want %#x", offOf(base), s.baseB)
		})
	}
}

// TestWideReferences: full-width offsets, distinct cache identity for
// same-low-32-bit DIEs, recursive pointer types, cross-unit references
// and exact unit-relative reference widths. Runs on both the sparse
// >4 GiB dwarf64 layout and an ordinary compact dwarf32 slice layout.
func TestWideReferences(t *testing.T) {
	cases := []struct {
		name      string
		sparse    bool
		placement placement
		blockEnd  int64
		is64      bool
	}{
		{"sparse-dwarf64", true, placement{sparseUnitB, sparseHighStr}, sparseUnitB, true},
		{"compact-dwarf32", false, placement{compactUnitB, compactHighStr}, compactUnitB, false},
	}
	for _, order := range []binary.ByteOrder{binary.LittleEndian, binary.BigEndian} {
		for _, tc := range cases {
			s := build(order, tc.placement, tc.blockEnd, tc.is64)
			taskcheck.Run(t, func(c *taskcheck.C) {
				var d *dwarf.Data
				var err error
				if tc.sparse {
					infoSec, strSec := s.sparseSections()
					d, err = dwarf.NewReaderAt(s.abbrev, infoSec, strSec)
				} else {
					info := materialize(s.infoSpan, s.infoSize)
					str := materialize(s.strSpan, s.strSize)
					d, err = dwarf.New(s.abbrev, nil, nil, info, nil, nil, nil, str)
				}
				c.Check(err == nil, "open %s: %v", tc.name, err)

				if tc.sparse {
					c.Check(uint32(s.baseA) == uint32(s.baseB) && s.baseA != s.baseB,
						"fixture: low32(%#x) must equal low32(%#x) while full offsets differ",
						s.baseA, s.baseB)
				}

				// Same low 32 bits (sparse) must remain distinct cache keys.
				tA, err := d.Type(s.baseA)
				c.Check(err == nil && tA != nil && tA.String() == "int" && tA.Size() == 4,
					"Type(baseA) %s: %v", typeName(tA), err)
				tB, err := d.Type(s.baseB)
				c.Check(err == nil && tB != nil && tB.String() == "int" && tB.Size() == 4,
					"Type(baseB) %s: %v", typeName(tB), err)
				c.Check(any(tA) != any(tB), "same-low32 DIEs collapsed into one cached type")
				tA2, _ := d.Type(s.baseA)
				tB2, _ := d.Type(s.baseB)
				c.Check(any(tA2) == any(tA) && any(tB2) == any(tB), "type cache unstable")

				// Recursive struct node { node *link; }.
				st, err := d.Type(s.structB)
				c.Check(err == nil, "Type(struct): %v", err)
				stStruct, ok := st.(*dwarf.StructType)
				c.Check(ok, "struct decoded as %T", st)
				if ok {
					c.Check(stStruct.StructName == "node" && len(stStruct.Field) == 1,
						"struct %q fields=%d", stStruct.StructName, len(stStruct.Field))
					ptr, isPtr := stStruct.Field[0].Type.(*dwarf.PtrType)
					c.Check(isPtr, "member type is %T", stStruct.Field[0].Type)
					if isPtr {
						target, isStruct := ptr.Type.(*dwarf.StructType)
						c.Check(isStruct && target.StructName == "node",
							"recursive pointer target %q", ptr.Type.String())
					}
				}
				pt, err := d.Type(s.ptrB)
				c.Check(err == nil, "Type(pointer): %v", err)
				if pp, ok := pt.(*dwarf.PtrType); ok {
					target, isStruct := pp.Type.(*dwarf.StructType)
					c.Check(isStruct && target.StructName == "node",
						"ptr target %q", pp.Type.String())
				} else {
					c.Check(ok, "pointer DIE decoded as %T", pt)
				}

				// Cross-unit typedef through DW_FORM_ref_addr.
				td, err := d.Type(s.typedef)
				c.Check(err == nil, "Type(typedef): %v", err)
				if tdt, ok := td.(*dwarf.TypedefType); ok {
					c.Check(tdt.Name == "alias" && tdt.Size() == 4 && tdt.Type.String() == "int",
						"typedef %q size %d underlying %q", tdt.Name, tdt.Size(), tdt.Type.String())
				} else {
					c.Check(ok, "typedef decoded as %T", td)
				}

				// ref1/ref2/ref4 keep their 1/2/4 byte widths and stay
				// relative to the unit header, all resolving to baseB.
				// Seek to a valid DIE offset (the unit header itself is
				// not a legal Seek target).
				r := d.Reader()
				r.Seek(s.baseB)
				var probe *dwarf.Entry
				for {
					e, eerr := r.Next()
					if eerr != nil || e == nil {
						break
					}
					if e.Tag == dwarf.TagConstant {
						probe = e
						break
					}
				}
				c.Check(probe != nil && len(probe.Field) == 3, "probe DIE: %+v", probe)
				if probe != nil {
					for _, f := range probe.Field {
						got, ok := f.Val.(dwarf.Offset)
						c.Check(ok && f.Class == dwarf.ClassReference && got == s.baseB,
							"probe field class=%v val=%#x want %#x", f.Class, uint64(got), s.baseB)
					}
				}
			})
		}
	}
}

// TestSparseIsolation: a failed or short read names the section and
// real offset and is recognizable with errors.Is, while an independent
// Reader and already-completed types keep working.
func TestSparseIsolation(t *testing.T) {
	sentinel := errors.New("synthetic read failure")
	for _, order := range []binary.ByteOrder{binary.LittleEndian, binary.BigEndian} {
		s := build(order, placement{sparseUnitB, sparseHighStr}, sparseUnitB, true)
		taskcheck.Run(t, func(c *taskcheck.C) {
			infoSrc := &fixture.Sparse{Size: s.infoSize, Spans: s.infoSpan}
			strSrc := &fixture.Sparse{Size: s.strSize, Spans: s.strSpan}

			// The failure interval covers only the base_type DIE body
			// inside unit B; the unit B header and CU DIE sit before it,
			// so independent readers can still open unit B.
			var ranges []readRange
			fail := &recordedReader{
				inner:   infoSrc,
				ranges:  &ranges,
				failLo:  int64(s.baseB),
				failHi:  int64(s.baseB) + 0x40,
				failErr: sentinel,
			}
			d, err := dwarf.NewReaderAt(s.abbrev,
				dwarf.Section{Reader: fail, Size: s.infoSize},
				dwarf.Section{Reader: strSrc, Size: s.strSize})
			c.Check(err == nil, "NewReaderAt: %v", err)

			// A type completed before arming the failure stays valid.
			good, err := d.Type(s.baseA)
			c.Check(err == nil && good != nil && good.Size() == 4, "pre-failure type: %v", err)

			// Underlying I/O error path.
			fail.armed = true
			r1 := d.Reader()
			r1.Seek(s.baseB)
			_, errFail := r1.Next()
			c.Check(errFail != nil, "expected read failure")
			var de dwarf.DecodeError
			c.Check(errors.As(errFail, &de), "not a DecodeError: %T", errFail)
			c.Check(errors.Is(errFail, sentinel), "cause not recognizable: %v", errFail)
			c.Check(de.Name == "info" && de.Offset == s.baseB,
				"error section=%q offset=%#x want %#x", de.Name, de.Offset, s.baseB)

			// An independent Reader still reads unit A and unit B's CU.
			r2 := d.Reader()
			cu, err := r2.Next()
			c.Check(err == nil && tagOf(cu) == dwarf.TagCompileUnit, "independent unit A CU: %v %v", tagOf(cu), err)
			r2.SkipChildren()
			cuB, err := r2.Next()
			c.Check(err == nil && tagOf(cuB) == dwarf.TagCompileUnit, "independent unit B CU: %v %v", tagOf(cuB), err)
			c.Check(good.String() == "int" && good.Size() == 4, "completed type corrupted after failure")

			// Short-read path reports ErrUnexpectedEOF.
			short := &recordedReader{
				inner:  &fixture.Sparse{Size: s.infoSize, Spans: s.infoSpan},
				ranges: &[]readRange{},
				short:  true, armed: true,
				failLo: int64(s.baseB),
				failHi: int64(s.baseB) + 0x40,
			}
			dShort, err := dwarf.NewReaderAt(s.abbrev,
				dwarf.Section{Reader: short, Size: s.infoSize},
				dwarf.Section{Reader: &fixture.Sparse{Size: s.strSize, Spans: s.strSpan}, Size: s.strSize})
			if err == nil {
				rr := dShort.Reader()
				rr.Seek(s.baseB)
				_, err = rr.Next()
			}
			c.Check(err != nil && errors.Is(err, io.ErrUnexpectedEOF), "short read cause: %v", err)
			var sde dwarf.DecodeError
			c.Check(errors.As(err, &sde) && sde.Name == "info", "short read section: %v", err)

			// Clean source: reading the bad-strp DIE names "str" and
			// keeps the real, full-width offset.
			dOK, err := dwarf.NewReaderAt(s.abbrev,
				dwarf.Section{Reader: &fixture.Sparse{Size: s.infoSize, Spans: s.infoSpan}, Size: s.infoSize},
				dwarf.Section{Reader: &fixture.Sparse{Size: s.strSize, Spans: s.strSpan}, Size: s.strSize})
			c.Check(err == nil, "reopen: %v", err)
			rOK := dOK.Reader()
			rOK.Seek(s.badStr) // valid DIE offset; its name strp is out of range
			_, strErr := rOK.Next()
			c.Check(strErr != nil, "expected .debug_str error")
			var bde dwarf.DecodeError
			c.Check(errors.As(strErr, &bde) && bde.Name == "str" && bde.Offset == s.badStrOff,
				"str error section=%q offset=%#x want %#x: %v",
				errName(strErr), errOffset(strErr), s.badStrOff, strErr)
			c.Check(errors.Is(strErr, io.ErrUnexpectedEOF), "str cause: %v", strErr)

			// Out-of-range Seek is reported rather than panicking.
			r4 := dOK.Reader()
			r4.Seek(dwarf.Offset(s.infoSize + 100))
			_, seekErr := r4.Next()
			c.Check(seekErr != nil, "expected out-of-range Seek error")
		})
	}
}

func errName(err error) string {
	var de dwarf.DecodeError
	if errors.As(err, &de) {
		return de.Name
	}
	return ""
}

func errOffset(err error) dwarf.Offset {
	var de dwarf.DecodeError
	if errors.As(err, &de) {
		return de.Offset
	}
	return 0
}
