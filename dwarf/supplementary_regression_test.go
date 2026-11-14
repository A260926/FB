// Copyright 2025 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package dwarf_test

// Regression tests for DWARF 5 supplementary object support.
//
// The tests use the deterministic samples in task_support/samples as well
// as synthetic DWARF assembled in this file. The synthetic fixtures cover
// what the samples cannot: a supplementary object with more than one
// compilation unit, a 64-bit DWARF unit, DW_FORM_ref_sup8, and a type
// layout rich enough for an offline memory-forensics interpretation.
//
// The purpose of the interpretation helper below is to turn a type plus a
// caller-supplied byte sample into a record that can serve as the source
// for a forensic report: for each fixed-size, non-bitfield member it
// records the member name, the half-open byte range, the raw bytes, the
// value, the object that owns the type, and the type identity. It never
// follows pointer values into memory.

import (
	. "debug/dwarf"
	"debug/elf"
	"encoding/binary"
	"errors"
	"fmt"
	"path/filepath"
	"strconv"
	"testing"

	"example.local/supplementary/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// Synthetic DWARF5 assembler.

const (
	fAddr    = 0x01
	fString  = 0x08
	fData1   = 0x0b
	fStrp    = 0x0e
	fRef4    = 0x13
	fRefSup4 = 0x1c
	fStrpSup = 0x1d
	fRefSup8 = 0x24
)

const (
	aName          = 0x03
	aByteSize      = 0x0b
	aBitSize       = 0x0d
	aDataMemberLoc = 0x38
	aEncoding      = 0x3e
	aType          = 0x49
)

var fixtureIdentity = []byte{0xde, 0xad, 0xbe, 0xef}

// tb is the subset of *testing.T used by the fixture helpers, so they can
// also be used from a testable example. A failed construction panics, which
// is acceptable for fixtures whose correctness is asserted elsewhere.
type tb interface {
	Helper()
	Fatalf(format string, args ...interface{})
}

// sampleIdentity is the opaque identity recorded in the shipped
// task_support/samples objects (checksum_len 4, bytes 01 23 45 67).
var sampleIdentity = []byte{0x01, 0x23, 0x45, 0x67}

type afieldSpec struct{ attr, form byte }

func u16le(v uint16) []byte { b := make([]byte, 2); binary.LittleEndian.PutUint16(b, v); return b }
func u32le(v uint32) []byte { b := make([]byte, 4); binary.LittleEndian.PutUint32(b, v); return b }
func u64le(v uint64) []byte { b := make([]byte, 8); binary.LittleEndian.PutUint64(b, v); return b }
func cstr(s string) []byte  { return append([]byte(s), 0) }

func uleb(v uint64) []byte {
	var out []byte
	for {
		c := byte(v & 0x7f)
		v >>= 7
		if v != 0 {
			out = append(out, c|0x80)
		} else {
			return append(out, c)
		}
	}
}

func abbrevEntry(code, tag byte, children bool, fields ...afieldSpec) []byte {
	out := append(uleb(uint64(code)), uleb(uint64(tag))...)
	if children {
		out = append(out, 1)
	} else {
		out = append(out, 0)
	}
	for _, f := range fields {
		out = append(out, f.attr, f.form)
	}
	return append(out, 0, 0)
}

// unit5HeaderLen is the number of bytes before the first DIE of a DWARF5
// unit, including the initial length field.
func unit5HeaderLen(dwarf64 bool) int {
	if dwarf64 {
		return 12 + 2 + 1 + 1 + 8
	}
	return 4 + 2 + 1 + 1 + 4
}

// unit5 assembles a DWARF5 compile unit with the given address size and
// abbreviation offset around body. body holds the DIEs and their
// terminating zero.
func unit5(dwarf64 bool, asize byte, abbrevOff uint64, body []byte) []byte {
	payload := u16le(5)
	payload = append(payload, 1, asize) // DW_UT_compile
	if dwarf64 {
		payload = append(payload, u64le(abbrevOff)...)
	} else {
		payload = append(payload, u32le(uint32(abbrevOff))...)
	}
	payload = append(payload, body...)
	if dwarf64 {
		out := append(u32le(0xffffffff), u64le(uint64(len(payload)))...)
		return append(out, payload...)
	}
	return append(u32le(uint32(len(payload))), payload...)
}

func putU32At(b []byte, at int, v uint32) { binary.LittleEndian.PutUint32(b[at:], v) }

// suppSection assembles a .debug_sup header. When withLength is true the
// DWARF initial length field is included; the shipped fixtures omit it,
// so both layouts are exercised.
func suppSection(withLength bool, version uint16, isSup bool, filename string, checksum []byte) []byte {
	var payload []byte
	payload = append(payload, u16le(version)...)
	if isSup {
		payload = append(payload, 1)
	} else {
		payload = append(payload, 0)
	}
	payload = append(payload, cstr(filename)...)
	payload = append(payload, byte(len(checksum)))
	payload = append(payload, checksum...)
	if !withLength {
		return payload
	}
	return append(u32le(uint32(len(payload))), payload...)
}

func newData(t tb, abbrev, info, str []byte) *Data {
	t.Helper()
	d, err := New(abbrev, nil, nil, info, nil, nil, nil, str)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	return d
}

func addSup(t tb, d *Data, sec []byte) {
	t.Helper()
	if err := d.AddSection(".debug_sup", sec); err != nil {
		t.Fatalf("AddSection(.debug_sup): %v", err)
	}
}

// newEmptyObject returns a minimal but valid Data, optionally with a
// .debug_sup section, for binding checks that do not read types.
func newEmptyObject(t tb, supSec []byte) *Data {
	t.Helper()
	abbrev := append(abbrevEntry(1, 0x11, true, afieldSpec{aName, fString}), 0)
	body := append([]byte{1}, cstr("empty")...)
	body = append(body, 0)
	d := newData(t, abbrev, unit5(false, 8, 0, body), nil)
	if supSec != nil {
		addSup(t, d, supSec)
	}
	return d
}

// mainSpec describes a synthetic main object whose only type is a
// typedef that refers into the supplementary object.
type mainSpec struct {
	dwarf64  bool
	asize    byte
	nameForm byte   // fString or fStrpSup
	name     string // used with fString
	nameOff  uint64 // used with fStrpSup: offset in the supplementary .debug_str
	refForm  byte   // fRefSup4 or fRefSup8
	ref      uint64 // supplementary .debug_info offset
	supSec   []byte
}

func buildMain(t tb, spec mainSpec) (*Data, Offset) {
	t.Helper()
	var abbrev []byte
	abbrev = append(abbrev, abbrevEntry(1, 0x11, true, afieldSpec{aName, fString})...)
	abbrev = append(abbrev, abbrevEntry(2, 0x16, false,
		afieldSpec{aName, spec.nameForm}, afieldSpec{aType, spec.refForm})...)
	abbrev = append(abbrev, 0)

	var body []byte
	body = append(body, 1)
	body = append(body, cstr("m")...)
	typedefRel := len(body)
	body = append(body, 2)
	if spec.nameForm == fString {
		body = append(body, cstr(spec.name)...)
	} else if spec.dwarf64 {
		body = append(body, u64le(spec.nameOff)...)
	} else {
		body = append(body, u32le(uint32(spec.nameOff))...)
	}
	if spec.refForm == fRefSup8 {
		body = append(body, u64le(spec.ref)...)
	} else {
		body = append(body, u32le(uint32(spec.ref))...)
	}
	body = append(body, 0)

	d := newData(t, abbrev, unit5(spec.dwarf64, spec.asize, 0, body), nil)
	if spec.supSec != nil {
		addSup(t, d, spec.supSec)
	}
	return d, Offset(unit5HeaderLen(spec.dwarf64) + typedefRel)
}

// supFixture is a supplementary object with two compilation units. The
// first holds a decoy base type; the second holds a struct with a mix of
// member types. The second CU uses address size 8, so a pointer's size
// reveals which object supplied the address width.
type supFixture struct {
	data      *Data
	str       []byte
	structOff Offset
	decoyOff  Offset
}

func buildSupFixture(t tb) *supFixture {
	t.Helper()

	// .debug_str of the supplementary object.
	var str []byte
	strOff := map[string]uint32{}
	addStr := func(s string) uint32 {
		o := uint32(len(str))
		str = append(str, cstr(s)...)
		strOff[s] = o
		return o
	}
	addStr("decoy")
	addStr("Header")
	addStr("magic")
	addStr("version")
	addStr("flags")
	addStr("ratio")
	addStr("next")
	addStr("uint32")
	addStr("uint8")
	addStr("float")

	// One abbreviation table shared by both units.
	var abbrev []byte
	abbrev = append(abbrev, abbrevEntry(1, 0x11, true, afieldSpec{aName, fStrp})...)
	abbrev = append(abbrev, abbrevEntry(2, 0x24, false,
		afieldSpec{aName, fStrp}, afieldSpec{aEncoding, fData1}, afieldSpec{aByteSize, fData1})...)
	abbrev = append(abbrev, abbrevEntry(3, 0x13, true,
		afieldSpec{aName, fStrp}, afieldSpec{aByteSize, fData1})...)
	abbrev = append(abbrev, abbrevEntry(4, 0x0d, false,
		afieldSpec{aName, fStrp}, afieldSpec{aType, fRef4}, afieldSpec{aDataMemberLoc, fData1})...)
	abbrev = append(abbrev, abbrevEntry(5, 0x0f, false, afieldSpec{aType, fRef4})...)
	abbrev = append(abbrev, abbrevEntry(6, 0x0d, false,
		afieldSpec{aName, fStrp}, afieldSpec{aType, fRef4}, afieldSpec{aDataMemberLoc, fData1}, afieldSpec{aBitSize, fData1})...)
	abbrev = append(abbrev, 0)

	// CU 1: compile unit plus a decoy base type. Address size 4.
	cu1 := []byte{1}
	cu1 = append(cu1, u32le(strOff["s"])...)
	decoyRel := len(cu1)
	cu1 = append(cu1, 2)
	cu1 = append(cu1, u32le(strOff["decoy"])...)
	cu1 = append(cu1, 0x05, 4) // signed, 4 bytes
	cu1 = append(cu1, 0)
	unit1 := unit5(false, 4, 0, cu1)

	// CU 2: the Header struct. Address size 8.
	cu2 := []byte{1}
	cu2 = append(cu2, u32le(strOff["s"])...)

	headerRel := len(cu2)
	cu2 = append(cu2, 3)
	cu2 = append(cu2, u32le(strOff["Header"])...)
	cu2 = append(cu2, 24)

	member := func(name string, loc byte, abbrevCode byte) int {
		rel := len(cu2)
		cu2 = append(cu2, abbrevCode)
		cu2 = append(cu2, u32le(strOff[name])...)
		cu2 = append(cu2, 0, 0, 0, 0) // AttrType patched below
		cu2 = append(cu2, loc)
		return rel
	}
	magicRel := member("magic", 0, 4)
	versionRel := member("version", 4, 4)
	flagsRel := len(cu2)
	cu2 = append(cu2, 6)
	cu2 = append(cu2, u32le(strOff["flags"])...)
	cu2 = append(cu2, 0, 0, 0, 0) // AttrType patched below
	cu2 = append(cu2, 5, 3)       // byte offset 5, bit size 3
	ratioRel := member("ratio", 8, 4)
	nextRel := member("next", 16, 4)
	cu2 = append(cu2, 0) // end of struct children

	ptrRel := len(cu2)
	cu2 = append(cu2, 5)
	ptrPatch := len(cu2)
	cu2 = append(cu2, 0, 0, 0, 0)

	u32Rel := len(cu2)
	cu2 = append(cu2, 2)
	cu2 = append(cu2, u32le(strOff["uint32"])...)
	cu2 = append(cu2, 0x07, 4)
	u8Rel := len(cu2)
	cu2 = append(cu2, 2)
	cu2 = append(cu2, u32le(strOff["uint8"])...)
	cu2 = append(cu2, 0x07, 1)
	floatRel := len(cu2)
	cu2 = append(cu2, 2)
	cu2 = append(cu2, u32le(strOff["float"])...)
	cu2 = append(cu2, 0x04, 4)
	cu2 = append(cu2, 0) // end of CU children

	hdr := unit5HeaderLen(false)
	// DW_FORM_ref4 values are relative to the start of the unit header.
	putRef := func(at, targetRel int) { putU32At(cu2, at, uint32(hdr+targetRel)) }
	putRef(magicRel+1+4, u32Rel)
	putRef(versionRel+1+4, u8Rel)
	putRef(flagsRel+1+4, u8Rel)
	putRef(ratioRel+1+4, floatRel)
	putRef(nextRel+1+4, ptrRel)
	putRef(ptrPatch, headerRel)

	unit2 := unit5(false, 8, 0, cu2)
	info := append(unit1, unit2...)

	d := newData(t, abbrev, info, str)
	addSup(t, d, suppSection(false, 5, true, "", fixtureIdentity))

	return &supFixture{
		data:      d,
		str:       str,
		structOff: Offset(len(unit1) + hdr + headerRel),
		decoyOff:  Offset(hdr + decoyRel),
	}
}

// mainSupSection returns a main object's .debug_sup section.
func mainSupSection() []byte { return suppSection(false, 5, false, "supplement.elf", fixtureIdentity) }

// sampleSupSection returns a main object's .debug_sup section carrying
// the identity found in the shipped samples.
func sampleSupSection() []byte {
	return suppSection(false, 5, false, "supplement.elf", sampleIdentity)
}

// loadSample opens one of the shipped ELF samples as a Data.
func loadSample(t tb, name string) *Data {
	t.Helper()
	f, err := elf.Open(filepath.Join("..", "task_support", "samples", name))
	if err != nil {
		t.Fatalf("open sample %s: %v", name, err)
	}
	defer f.Close()
	d, err := f.DWARF()
	if err != nil {
		t.Fatalf("read DWARF from %s: %v", name, err)
	}
	return d
}

func entryAt(t tb, d *Data, off Offset) (*Entry, error) {
	t.Helper()
	r := d.Reader()
	r.Seek(off)
	return r.Next()
}

func isDecodeErr(err error) bool {
	var de DecodeError
	return errors.As(err, &de)
}

// ---------------------------------------------------------------------------
// Offline memory-forensics interpretation.

// interpretedMember is the source record for one member: which object
// supplied the type, the type identity, the member name, its byte range
// within the sample, the raw bytes, and either a decoded value or an
// explicit reason the member is not interpretable.
type interpretedMember struct {
	Object    string // identity of the object that owns the type
	TypeID    string // object plus offset plus type name
	Member    string
	Start     int64
	End       int64
	Raw       []byte
	Value     string
	Note      string
	Interpret bool
}

func unwrapTypedef(t Type) Type {
	for {
		td, ok := t.(*TypedefType)
		if !ok {
			return t
		}
		t = td.Type
	}
}

func decodeUint(raw []byte, order binary.ByteOrder) (uint64, bool) {
	switch len(raw) {
	case 1:
		return uint64(raw[0]), true
	case 2:
		return uint64(order.Uint16(raw)), true
	case 4:
		return uint64(order.Uint32(raw)), true
	case 8:
		return order.Uint64(raw), true
	}
	return 0, false
}

// interpret decodes sample as the struct type t owned by object obj at
// type offset off. Only struct/union members with a fixed byte size are
// interpreted; bit fields, unsupported member types, and members outside
// the sample are reported explicitly instead of guessed. Pointer members
// report the stored address and are never dereferenced.
func interpret(obj string, off Offset, t Type, sample []byte, order binary.ByteOrder) ([]interpretedMember, error) {
	if t == nil {
		return nil, fmt.Errorf("%s at offset %d has no type; not interpretable", obj, off)
	}
	root := t
	st, ok := unwrapTypedef(t).(*StructType)
	if !ok || st.Incomplete {
		return nil, fmt.Errorf("%s at offset %d is not a complete struct type; not interpretable", root.String(), off)
	}
	typeID := fmt.Sprintf("%s@%d %s", obj, off, root.String())

	var out []interpretedMember
	for _, f := range st.Field {
		m := interpretedMember{
			Object: obj,
			TypeID: typeID,
			Member: f.Name,
			Start:  f.ByteOffset,
		}
		size := f.Type.Size()
		if size <= 0 {
			m.Note = "unsupported type: unknown or invalid size"
			out = append(out, m)
			continue
		}
		m.End = f.ByteOffset + size
		if f.ByteOffset < 0 || m.End > int64(len(sample)) {
			m.Note = "not interpretable: out of sample bounds"
			out = append(out, m)
			continue
		}
		m.Raw = append([]byte(nil), sample[f.ByteOffset:m.End]...)

		if f.BitSize != 0 {
			m.Note = "not interpretable: bit field"
			out = append(out, m)
			continue
		}

		v, ok := decodeUint(m.Raw, order)
		switch ft := unwrapTypedef(f.Type).(type) {
		case *PtrType:
			// A pointer reports its stored address. Its value is never
			// followed; doing so would require memory that is not part
			// of the caller-provided sample.
			if ok {
				m.Value = fmt.Sprintf("0x%0*x", size*2, v)
			}
			m.Note = "pointer address (not dereferenced)"
			m.Interpret = ok
		case *IntType:
			if ok {
				m.Value = strconv.FormatInt(signExtend(v, len(m.Raw)), 10)
			}
			m.Interpret = ok
		case *UintType, *BoolType:
			if ok {
				m.Value = strconv.FormatUint(v, 10)
			}
			m.Interpret = ok
		case *CharType:
			if ok {
				m.Value = strconv.FormatInt(int64(int8(v)), 10)
			}
			m.Interpret = ok
		case *UcharType:
			if ok {
				m.Value = strconv.FormatUint(uint64(uint8(v)), 10)
			}
			m.Interpret = ok
		case *FloatType:
			m.Note = "not interpretable: floating-point member"
		case *StructType:
			m.Note = "nested aggregate: range and raw bytes only"
		default:
			_ = ft
			m.Note = fmt.Sprintf("not interpretable: unsupported type %T", unwrapTypedef(f.Type))
		}
		if !m.Interpret && m.Note == "" {
			// decodeUint failed for a member size other than 1, 2, 4,
			// or 8. Never expose a partially decoded value as success.
			m.Value = ""
			m.Note = fmt.Sprintf("not interpretable: unsupported member size %d", size)
		}
		out = append(out, m)
	}
	return out, nil
}

func signExtend(v uint64, size int) int64 {
	switch size {
	case 1:
		return int64(int8(v))
	case 2:
		return int64(int16(v))
	case 4:
		return int64(int32(v))
	}
	return int64(v)
}

func findMember(ms []interpretedMember, name string) *interpretedMember {
	for i := range ms {
		if ms[i].Member == name {
			return &ms[i]
		}
	}
	return nil
}

// ---------------------------------------------------------------------------
// Test groups.

func TestSupplementaryStrings(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// The shipped main object names its typedef with DW_FORM_strp_sup,
		// pointing at the supplementary .debug_str.
		main := loadSample(t, "main.elf")
		sup := loadSample(t, "supplement.elf")
		c.Check(main.SetSupplementary(sup) == nil, "binding matching identity should succeed")

		e, err := entryAt(t, main, 24)
		c.Check(err == nil && e != nil, "typedef entry should decode: %v", err)
		if e != nil {
			nameField := e.AttrField(AttrName)
			c.Check(nameField != nil && nameField.Class == ClassString, "strp_sup attribute should have ClassString")
			name, _ := e.Val(AttrName).(string)
			c.Check(name == "Node", "strp_sup should resolve in the supplementary .debug_str, got %q", name)

			typeField := e.AttrField(AttrType)
			c.Check(typeField != nil && typeField.Class == ClassReference, "ref_sup4 attribute should have ClassReference")
			so, ok := e.Val(AttrType).(SupplementaryOffset)
			c.Check(ok, "ref_sup4 value type should be SupplementaryOffset, got %T", e.Val(AttrType))
			c.Check(uint64(so) == 15, "ref_sup4 value should be 15, got %d", uint64(so))
		}

		// Rejections must not disturb the association, and a later valid
		// bind must still work.
		m := loadSample(t, "main.elf")
		s := loadSample(t, "supplement.elf")
		badSec := suppSection(false, 5, false, "supplement.elf", []byte{0xde, 0xad, 0xbe, 0x00})
		bad := newEmptyObject(t, badSec)
		c.Check(m.SetSupplementary(nil) != nil, "nil supplementary object should be rejected")
		c.Check(m.SetSupplementary(m) != nil, "self binding should be rejected")
		c.Check(m.SetSupplementary(bad) != nil, "identity mismatch should be rejected")
		c.Check(m.SetSupplementary(s) == nil, "binding after rejected attempts should succeed")
		if e, err := entryAt(t, m, 24); e != nil && err == nil {
			c.Check(e.Val(AttrName) == "Node", "association should survive rejections, got %v", e.Val(AttrName))
		} else {
			c.Check(false, "typedef should decode after binding: %v", err)
		}
		c.Check(m.SetSupplementary(s) != nil, "duplicate binding should be rejected")
		if e, err := entryAt(t, m, 24); e != nil && err == nil {
			c.Check(e.Val(AttrName) == "Node", "association should survive duplicate rejection")
		} else {
			c.Check(false, "typedef should still decode after duplicate rejection: %v", err)
		}
		c.Check(m.AddSection(".debug_sup", mainSupSection()) != nil, "main .debug_sup must be frozen after binding")
		c.Check(s.AddSection(".debug_sup", suppSection(false, 5, true, "", fixtureIdentity)) != nil, "supplementary .debug_sup must be frozen after binding")
		if e, err := entryAt(t, m, 24); e != nil && err == nil {
			c.Check(e.Val(AttrName) == "Node", "frozen section should keep resolving")
		}

		// Missing identity section, then supplying it, then binding.
		noID := newEmptyObject(t, nil)
		withID := loadSample(t, "supplement.elf")
		c.Check(noID.SetSupplementary(withID) != nil, "missing .debug_sup should be rejected")
		c.Check(noID.AddSection(".debug_sup", sampleSupSection()) == nil, "adding .debug_sup should succeed")
		c.Check(noID.SetSupplementary(withID) == nil, "binding after adding identity should succeed")

		// The supplementary side must also carry an identity section.
		mainWithID := newEmptyObject(t, sampleSupSection())
		c.Check(mainWithID.SetSupplementary(newEmptyObject(t, nil)) != nil,
			"supplementary object without .debug_sup should be rejected")
		c.Check(mainWithID.SetSupplementary(withID) == nil,
			"rejected bind should leave the object free to bind later")

		// A Data already bound as a supplementary object cannot itself
		// become a main object.
		c.Check(withID.SetSupplementary(newEmptyObject(t, sampleSupSection())) != nil,
			"an object already serving as supplementary should not bind another")

		// Structural identity checks.
		checks := []struct {
			name string
			sec  []byte
			sup  []byte
		}{
			{"version", suppSection(false, 4, false, "x", fixtureIdentity), suppSection(false, 5, true, "", fixtureIdentity)},
			{"main flag", suppSection(false, 5, true, "x", fixtureIdentity), suppSection(false, 5, true, "", fixtureIdentity)},
			{"sup flag", suppSection(false, 5, false, "x", fixtureIdentity), suppSection(false, 5, false, "", fixtureIdentity)},
			{"main filename", suppSection(false, 5, false, "", fixtureIdentity), suppSection(false, 5, true, "", fixtureIdentity)},
			{"sup filename", suppSection(false, 5, false, "x", fixtureIdentity), suppSection(false, 5, true, "y", fixtureIdentity)},
			{"checksum", suppSection(false, 5, false, "x", fixtureIdentity), suppSection(false, 5, true, "", []byte{0xde, 0xad, 0xbe, 0x00})},
			{"checksum length", suppSection(false, 5, false, "x", fixtureIdentity), suppSection(false, 5, true, "", []byte{0xde, 0xad, 0xbe, 0xef, 0x00})},
			{"empty checksum", suppSection(false, 5, false, "x", nil), suppSection(false, 5, true, "", nil)},
		}
		for _, tc := range checks {
			a := newEmptyObject(t, tc.sec)
			b := newEmptyObject(t, tc.sup)
			err := a.SetSupplementary(b)
			if tc.name == "empty checksum" {
				c.Check(err == nil, "equal empty checksums should match")
			} else {
				c.Check(err != nil, "%s mismatch should be rejected", tc.name)
			}
		}

		// A unit_length-prefixed .debug_sup is the standard layout and
		// should also be accepted.
		withLen := newEmptyObject(t, suppSection(true, 5, false, "x", fixtureIdentity))
		supLen := newEmptyObject(t, suppSection(true, 5, true, "", fixtureIdentity))
		c.Check(withLen.SetSupplementary(supLen) == nil, "unit_length-prefixed .debug_sup should bind")

		// 32-bit and 64-bit DWARF units encode strp_sup offsets with
		// different widths. Both should resolve against the target object.
		supFx := buildSupFixture(t)
		main32, off32 := buildMain(t, mainSpec{
			asize: 4, nameForm: fStrpSup, nameOff: 0,
			refForm: fRefSup4, ref: uint64(supFx.structOff), supSec: mainSupSection(),
		})
		c.Check(main32.SetSupplementary(supFx.data) == nil, "32-bit main should bind")
		e32, err32 := entryAt(t, main32, off32)
		c.Check(err32 == nil && e32 != nil, "32-bit strp_sup entry should decode: %v", err32)
		if e32 != nil {
			c.Check(e32.Val(AttrName) == "decoy", "32-bit strp_sup should resolve from supplementary object, got %v", e32.Val(AttrName))
		}

		main64, off64 := buildMain(t, mainSpec{
			dwarf64: true, asize: 8, nameForm: fStrpSup, nameOff: 0,
			refForm: fRefSup4, ref: uint64(supFx.structOff), supSec: mainSupSection(),
		})
		c.Check(main64.SetSupplementary(supFx.data) == nil, "64-bit main should bind")
		e64, err64 := entryAt(t, main64, off64)
		c.Check(err64 == nil && e64 != nil, "64-bit strp_sup entry should decode: %v", err64)
		if e64 != nil {
			c.Check(e64.Val(AttrName) == "decoy", "64-bit strp_sup should resolve, got %v", e64.Val(AttrName))
		}

		// A 64-bit string offset beyond 4GiB must not be truncated to a
		// small valid offset.
		mainHigh, offHigh := buildMain(t, mainSpec{
			dwarf64: true, asize: 8, nameForm: fStrpSup, nameOff: 1 << 32,
			refForm: fRefSup4, ref: uint64(supFx.structOff), supSec: mainSupSection(),
		})
		c.Check(mainHigh.SetSupplementary(supFx.data) == nil, "high-offset main should bind")
		eHigh, errHigh := entryAt(t, mainHigh, offHigh)
		c.Check(errHigh != nil, "out-of-range 64-bit strp_sup should fail, got %v", eHigh)
		c.Check(isDecodeErr(errHigh), "out-of-range strp_sup should be a DecodeError, got %T", errHigh)

		// An out-of-range 32-bit string offset is a locatable decode error.
		mainOOR, offOOR := buildMain(t, mainSpec{
			asize: 8, nameForm: fStrpSup, nameOff: uint64(len(supFx.str) + 10),
			refForm: fRefSup4, ref: uint64(supFx.structOff), supSec: mainSupSection(),
		})
		c.Check(mainOOR.SetSupplementary(supFx.data) == nil, "out-of-range main should bind")
		eOOR, errOOR := entryAt(t, mainOOR, offOOR)
		c.Check(errOOR != nil, "out-of-range strp_sup should fail, got %v", eOOR)
		c.Check(isDecodeErr(errOOR), "out-of-range strp_sup should be a DecodeError, got %T", errOOR)

		// A string without its terminating zero must not be returned.
		mainNoNul, offNoNul := buildMain(t, mainSpec{
			asize: 8, nameForm: fStrpSup, nameOff: 1,
			refForm: fRefSup4, ref: uint64(supFx.structOff), supSec: mainSupSection(),
		})
		// Use a supplementary object whose .debug_str lacks a NUL.
		supNoNul := newData(t, buildSupAbbrevForTest(), mainBodyForTest(), []byte("abc"))
		addSup(t, supNoNul, suppSection(false, 5, true, "", fixtureIdentity))
		c.Check(mainNoNul.SetSupplementary(supNoNul) == nil, "missing-NUL main should bind")
		eNoNul, errNoNul := entryAt(t, mainNoNul, offNoNul)
		c.Check(errNoNul != nil, "non-terminated string should fail, got %v", eNoNul)
		c.Check(isDecodeErr(errNoNul), "non-terminated string should be a DecodeError, got %T", errNoNul)

		// Without a binding, a strp_sup attribute must fail, not return
		// an integer or an empty success.
		unbound, offUnbound := buildMain(t, mainSpec{
			asize: 8, nameForm: fStrpSup, nameOff: 0,
			refForm: fRefSup4, ref: uint64(supFx.structOff), supSec: mainSupSection(),
		})
		eUnbound, errUnbound := entryAt(t, unbound, offUnbound)
		c.Check(errUnbound != nil, "unbound strp_sup should fail, got %v", eUnbound)
		c.Check(isDecodeErr(errUnbound), "unbound strp_sup should be a DecodeError, got %T", errUnbound)
	})
}

// buildSupAbbrevForTest and mainBodyForTest provide a valid, minimal
// .debug_info for auxiliary Data values used only to host a .debug_str.
func buildSupAbbrevForTest() []byte {
	return append(abbrevEntry(1, 0x11, true, afieldSpec{aName, fString}), 0)
}

func mainBodyForTest() []byte {
	body := append([]byte{1}, cstr("x")...)
	return unit5(false, 8, 0, append(body, 0))
}

func TestSupplementaryTypes(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// The shipped samples: the main typedef resolves to the
		// supplementary recursive Node struct.
		main := loadSample(t, "main.elf")
		sup := loadSample(t, "supplement.elf")
		c.Check(main.SetSupplementary(sup) == nil, "binding shipped samples should succeed")

		td, err := main.Type(24)
		c.Check(err == nil, "main typedef should resolve: %v", err)
		typedef, ok := td.(*TypedefType)
		c.Check(ok, "type at main offset 24 should be a typedef, got %T", td)
		if ok {
			c.Check(typedef.Name == "Node", "typedef name should come from supplementary strings, got %q", typedef.Name)
			c.Check(typedef.Size() == 8, "typedef size should be 8, got %d", typedef.Size())
			st, ok := typedef.Type.(*StructType)
			c.Check(ok, "typedef should refer to a struct, got %T", typedef.Type)
			if ok {
				c.Check(st.StructName == "Node" && st.Kind == "struct", "struct should be struct Node, got %s %s", st.Kind, st.StructName)
				c.Check(len(st.Field) == 1, "Node should have one member, got %d", len(st.Field))
				if len(st.Field) == 1 {
					f := st.Field[0]
					c.Check(f.Name == "next", "member name should be next, got %q", f.Name)
					c.Check(f.ByteOffset == 0, "member offset should be 0, got %d", f.ByteOffset)
					ptr, ok := f.Type.(*PtrType)
					c.Check(ok, "member should be a pointer, got %T", f.Type)
					if ok {
						c.Check(ptr.Type == st, "recursive pointer should refer to the same struct identity")
					}
				}
				// The standalone supplementary Data and the main Data
				// share the type identity from the supplementary cache.
				standalone, err := sup.Type(15)
				c.Check(err == nil && standalone == Type(st), "supplementary cache should be reused across lookups")
			}
		}

		// Offsets are scoped to their object: main offset 15 is the
		// local base type, not the supplementary Node.
		local, err := main.Type(15)
		c.Check(err == nil, "main local type should still resolve: %v", err)
		if err == nil {
			c.Check(local.String() == "local", "main offset 15 should be local, got %s", local.String())
			c.Check(local.Size() == 4, "main local type should be 4 bytes, got %d", local.Size())
			_, isStruct := local.(*StructType)
			c.Check(!isStruct, "main offset 15 must not be decoded as the supplementary Node")
		}

		// Synthetic fixture: the target type lives in the second CU of
		// the supplementary object; its members, names, address width,
		// and reference base all come from that object.
		fx := buildSupFixture(t)
		mainFx, typedefOff := buildMain(t, mainSpec{
			asize: 4, nameForm: fString, name: "Packet",
			refForm: fRefSup4, ref: uint64(fx.structOff), supSec: mainSupSection(),
		})
		c.Check(mainFx.SetSupplementary(fx.data) == nil, "synthetic main should bind")

		decoy, err := fx.data.Type(fx.decoyOff)
		c.Check(err == nil && decoy.String() == "decoy" && decoy.Size() == 4,
			"first supplementary CU type should be readable, got %v %v", decoy, err)

		packetType, err := mainFx.Type(typedefOff)
		c.Check(err == nil, "typedef in main should resolve into the supplementary second CU: %v", err)
		packet, ok := packetType.(*TypedefType)
		c.Check(ok, "synthetic typedef expected, got %T", packetType)
		var header *StructType
		if ok {
			c.Check(packet.Name == "Packet", "synthetic typedef name should be Packet, got %q", packet.Name)
			header, ok = packet.Type.(*StructType)
			c.Check(ok, "Packet should refer to a struct, got %T", packet.Type)
		}
		if header != nil {
			c.Check(header.StructName == "Header", "struct name from target .debug_str should be Header, got %q", header.StructName)
			c.Check(header.Size() == 24, "Header should be 24 bytes, got %d", header.Size())
			c.Check(len(header.Field) == 5, "Header should have 5 members, got %d", len(header.Field))
			if len(header.Field) == 5 {
				type memberCheck struct {
					name string
					off  int64
					size int64
					kind string
				}
				want := []memberCheck{
					{"magic", 0, 4, "*dwarf.UintType"},
					{"version", 4, 1, "*dwarf.UintType"},
					{"flags", 5, 1, "*dwarf.UintType"},
					{"ratio", 8, 4, "*dwarf.FloatType"},
					{"next", 16, 8, "*dwarf.PtrType"},
				}
				for i, w := range want {
					f := header.Field[i]
					c.Check(f.Name == w.name, "member %d name should be %q, got %q", i, w.name, f.Name)
					c.Check(f.ByteOffset == w.off, "member %q offset should be %d, got %d", w.name, w.off, f.ByteOffset)
					c.Check(f.Type.Size() == w.size, "member %q size should be %d, got %d", w.name, w.size, f.Type.Size())
					c.Check(fmt.Sprintf("%T", unwrapTypedef(f.Type)) == w.kind,
						"member %q kind should be %s, got %T", w.name, w.kind, unwrapTypedef(f.Type))
				}
				// The pointer's 8-byte size comes from the target CU's
				// address size even though the main CU uses 4.
				c.Check(header.Field[4].Type.Size() == 8, "pointer size should use the target CU address width")
			}
		}

		// Forensic interpretation of a caller-supplied sample.
		sample := make([]byte, 24)
		binary.LittleEndian.PutUint32(sample[0:], 0x12345678)
		sample[4] = 2 // version
		sample[5] = 0x5a
		binary.LittleEndian.PutUint32(sample[8:], 0x3fc00000) // float 1.5
		binary.LittleEndian.PutUint64(sample[16:], 0x1122334455667788)
		records, err := interpret("supplementary", fx.structOff, packetType, sample, binary.LittleEndian)
		c.Check(err == nil, "Header sample should be interpretable: %v", err)
		c.Check(len(records) == 5, "interpretation should report all five members, got %d", len(records))
		if magic := findMember(records, "magic"); magic != nil {
			c.Check(magic.Interpret && magic.Value == "305419896", "magic value should be 305419896, got %q", magic.Value)
			c.Check(magic.Start == 0 && magic.End == 4, "magic range should be [0,4), got [%d,%d)", magic.Start, magic.End)
			c.Check(len(magic.Raw) == 4 && magic.Raw[0] == 0x78 && magic.Raw[3] == 0x12, "magic raw bytes should be the sample bytes")
			c.Check(magic.Object == "supplementary", "magic should record its owning object")
			c.Check(magic.TypeID != "", "magic should record a type identity")
		}
		if version := findMember(records, "version"); version != nil {
			c.Check(version.Interpret && version.Value == "2", "version value should be 2, got %q", version.Value)
			c.Check(version.Start == 4 && version.End == 5, "version range should be [4,5), got [%d,%d)", version.Start, version.End)
		}
		if flags := findMember(records, "flags"); flags != nil {
			c.Check(!flags.Interpret && flags.Note == "not interpretable: bit field", "bit field should be reported, got %q", flags.Note)
		}
		if ratio := findMember(records, "ratio"); ratio != nil {
			c.Check(!ratio.Interpret && ratio.Start == 8 && ratio.End == 12, "float member should be reported as unsupported at [8,12)")
			c.Check(ratio.Raw != nil, "unsupported member should still carry raw bytes")
		}
		if next := findMember(records, "next"); next != nil {
			c.Check(next.Interpret && next.Value == "0x1122334455667788",
				"pointer should show the stored address, got %q", next.Value)
			c.Check(next.Start == 16 && next.End == 24, "pointer range should be [16,24), got [%d,%d)", next.Start, next.End)
			c.Check(next.Note == "pointer address (not dereferenced)", "pointer must not be followed")
		}

		// A short sample must report out-of-bounds members rather than
		// reading past the provided bytes.
		short := make([]byte, 12)
		shortRecords, err := interpret("supplementary", fx.structOff, packetType, short, binary.LittleEndian)
		c.Check(err == nil, "short sample should still be inspected")
		if next := findMember(shortRecords, "next"); next != nil {
			c.Check(!next.Interpret && next.Note == "not interpretable: out of sample bounds",
				"out-of-bounds member should be reported, got %q", next.Note)
			c.Check(next.Raw == nil, "out-of-bounds member must not carry bytes")
		}
		if magic := findMember(shortRecords, "magic"); magic != nil {
			c.Check(magic.Interpret, "in-bounds members should still be interpreted")
		}

		// 64-bit DWARF format: ref_sup4 is still 4 bytes and resolves.
		main64, off64 := buildMain(t, mainSpec{
			dwarf64: true, asize: 8, nameForm: fString, name: "Packet64",
			refForm: fRefSup4, ref: uint64(fx.structOff), supSec: mainSupSection(),
		})
		c.Check(main64.SetSupplementary(fx.data) == nil, "64-bit main should bind")
		t64, err64 := main64.Type(off64)
		c.Check(err64 == nil, "64-bit main type should resolve: %v", err64)
		if td, ok := t64.(*TypedefType); ok {
			c.Check(td.Name == "Packet64" && unwrapTypedef(td.Type).(*StructType).StructName == "Header",
				"64-bit typedef should resolve to Header")
		} else {
			c.Check(false, "64-bit type should be a typedef, got %T", t64)
		}

		// ref_sup8 is 8 bytes wide. A value with bits above 32 must be
		// preserved by the decoder and must not silently resolve to a
		// small, valid offset.
		highRef := uint64(1)<<32 + uint64(fx.structOff)
		mainRef8, offRef8 := buildMain(t, mainSpec{
			asize: 8, nameForm: fString, name: "Packet8",
			refForm: fRefSup8, ref: highRef, supSec: mainSupSection(),
		})
		c.Check(mainRef8.SetSupplementary(fx.data) == nil, "ref_sup8 main should bind")
		e8, err8 := entryAt(t, mainRef8, offRef8)
		c.Check(err8 == nil && e8 != nil, "ref_sup8 entry should decode: %v", err8)
		if e8 != nil {
			got, ok := e8.Val(AttrType).(SupplementaryOffset)
			c.Check(ok, "ref_sup8 value should be SupplementaryOffset, got %T", e8.Val(AttrType))
			c.Check(uint64(got) == highRef, "ref_sup8 must keep all 64 bits, got 0x%x want 0x%x", uint64(got), highRef)
			c.Check(e8.AttrField(AttrType).Class == ClassReference, "ref_sup8 class should remain ClassReference")
		}
		_, errHighType := mainRef8.Type(offRef8)
		c.Check(errHighType != nil, "ref_sup8 above the Offset range must not resolve to a truncated offset")
		if td, ok := mainRef8.Type(offRef8); ok == nil && td != nil {
			c.Check(td.String() != "Packet8", "high ref_sup8 must not be truncated into the valid low offset")
		}

		// Missing association and out-of-range references.
		unbound, offUnbound := buildMain(t, mainSpec{
			asize: 8, nameForm: fString, name: "Unbound",
			refForm: fRefSup4, ref: uint64(fx.structOff), supSec: mainSupSection(),
		})
		tUnbound, errUnbound := unbound.Type(offUnbound)
		c.Check(errUnbound != nil, "type with no bound supplementary object should fail, got %v", tUnbound)
		c.Check(isDecodeErr(errUnbound), "missing association should be a DecodeError, got %T", errUnbound)

		boundOOR, offOOR := buildMain(t, mainSpec{
			asize: 8, nameForm: fString, name: "OOR",
			refForm: fRefSup4, ref: 0x7000, supSec: mainSupSection(),
		})
		c.Check(boundOOR.SetSupplementary(fx.data) == nil, "out-of-range main should bind")
		tOOR, errOOR := boundOOR.Type(offOOR)
		c.Check(errOOR != nil, "out-of-range supplementary reference should fail, got %v", tOOR)
		c.Check(isDecodeErr(errOOR), "out-of-range reference should be a DecodeError, got %T", errOOR)
	})
}

func TestSupplementaryIsolation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Two main objects sharing one supplementary object. The main
		// objects have different typedef names at the same offset; the
		// target recursive struct must be reused.
		sup := loadSample(t, "supplement.elf")
		mainA, offA := buildMain(t, mainSpec{
			asize: 8, nameForm: fString, name: "NodeA",
			refForm: fRefSup4, ref: 15, supSec: sampleSupSection(),
		})
		mainB, offB := buildMain(t, mainSpec{
			asize: 8, nameForm: fString, name: "NodeB",
			refForm: fRefSup4, ref: 15, supSec: sampleSupSection(),
		})
		c.Check(offA == offB, "the two main objects should place their typedef at the same offset")
		c.Check(mainA.SetSupplementary(sup) == nil, "first main should bind the shared supplementary object")
		c.Check(mainB.SetSupplementary(sup) == nil, "second main should bind the shared supplementary object")

		aType, errA := mainA.Type(offA)
		bType, errB := mainB.Type(offB)
		c.Check(errA == nil && errB == nil, "both mains should resolve types: %v %v", errA, errB)
		tdA, okA := aType.(*TypedefType)
		tdB, okB := bType.(*TypedefType)
		c.Check(okA && okB, "both results should be typedefs")
		if okA && okB {
			c.Check(tdA.Name == "NodeA" && tdB.Name == "NodeB",
				"each main should keep its own typedef identity, got %q and %q", tdA.Name, tdB.Name)
			c.Check(aType != bType, "typedefs from different main objects must not share an identity")
			structA, okA := tdA.Type.(*StructType)
			structB, okB := tdB.Type.(*StructType)
			c.Check(okA && okB, "both typedefs should refer to a struct")
			c.Check(structA == structB, "the shared supplementary recursive struct should be reused")
			if structA != nil {
				ptr, ok := structA.Field[0].Type.(*PtrType)
				c.Check(ok && ptr.Type == structA, "shared recursive node should point to itself")
				standalone, err := sup.Type(15)
				c.Check(err == nil && standalone == Type(structA), "standalone supplementary lookup should share the recursive identity")
			}
		}

		// Alternating queries between the two main objects and the
		// supplementary object keep returning the same recursive node.
		for i := 0; i < 4; i++ {
			ta, err := mainA.Type(offA)
			c.Check(err == nil, "alternating mainA query %d: %v", i, err)
			sb, err := sup.Type(15)
			c.Check(err == nil, "alternating supplementary query %d: %v", i, err)
			tb, err := mainB.Type(offB)
			c.Check(err == nil, "alternating mainB query %d: %v", i, err)
			sa, _ := ta.(*TypedefType)
			bs, _ := tb.(*TypedefType)
			if sa != nil && bs != nil {
				c.Check(sa.Type == sb && bs.Type == sb, "recursive node identity should be stable across alternating queries")
			}
		}

		// Local types remain readable in both main objects.
		mainLocal, err := mainA.Type(15)
		c.Check(err == nil, "main local offset 15 should remain readable: %v", err)
		if err == nil {
			c.Check(mainLocal.String() == "NodeA", "main offset 15 is the local typedef NodeA here, got %s", mainLocal.String())
			c.Check(mainLocal.Size() == 8, "local typedef size should be 8, got %d", mainLocal.Size())
		}

		// Same numeric offset, different object: main offset 15 and
		// supplementary offset 15 must have distinct identities.
		supType, err := sup.Type(15)
		c.Check(err == nil, "supplementary offset 15 should resolve: %v", err)
		if err == nil {
			c.Check(Type(supType) != mainLocal, "equal offsets in different objects must not share a type identity")
		}

		// Same bytes must not be interpreted with the wrong object's
		// type. The supplementary Node has a pointer member at [0,8);
		// the shipped main base type at offset 15 is a 4-byte scalar.
		mainSample := loadSample(t, "main.elf")
		c.Check(mainSample.SetSupplementary(loadSample(t, "supplement.elf")) == nil, "sample main should bind")
		mainScalar, err := mainSample.Type(15)
		c.Check(err == nil && mainScalar.Size() == 4, "main offset 15 should be a 4-byte scalar")
		stride := []byte{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}
		_, errScalar := interpret("main", 15, mainScalar, stride, binary.LittleEndian)
		c.Check(errScalar != nil, "the same bytes must not be interpretable with the wrong main-object type: %v", errScalar)
		nodeType, err := mainSample.Type(24) // typedef into supplementary Node
		c.Check(err == nil, "sample typedef into Node should resolve: %v", err)
		nodeRecords, err := interpret("supplementary", 15, nodeType, stride, binary.LittleEndian)
		c.Check(err == nil, "Node should be interpretable as a struct: %v", err)
		if next := findMember(nodeRecords, "next"); next != nil {
			c.Check(next.Interpret && next.Value == "0x8877665544332211",
				"Node.next should show the stored address 0x8877665544332211, got %q", next.Value)
			c.Check(next.Start == 0 && next.End == 8, "Node.next should occupy [0,8), got [%d,%d)", next.Start, next.End)
			c.Check(len(next.Raw) == 8 && next.Raw[0] == 0x11 && next.Raw[7] == 0x88, "Node.next raw bytes should match the sample")
			c.Check(next.Object == "supplementary", "Node.next should be attributed to the supplementary object")
			c.Check(next.TypeID != "" && next.TypeID != fmt.Sprintf("main@%d %s", 15, mainScalar.String()),
				"type identity must distinguish the two objects at the same offset")
		} else {
			c.Check(false, "Node interpretation should include the next member")
		}
		if mainScalar != nil {
			c.Check(fmt.Sprintf("%T", mainScalar) == "*dwarf.IntType", "main offset 15 should stay a local int type, got %T", mainScalar)
		}

		// Two main objects with the same offset but different layouts
		// both resolve through one shared supplementary object.
		fx := buildSupFixture(t)
		packetMain, packetOff := buildMain(t, mainSpec{
			asize: 4, nameForm: fString, name: "Packet",
			refForm: fRefSup4, ref: uint64(fx.structOff), supSec: mainSupSection(),
		})
		otherMain, otherOff := buildMain(t, mainSpec{
			asize: 8, nameForm: fString, name: "Other",
			refForm: fRefSup4, ref: uint64(fx.structOff), supSec: mainSupSection(),
		})
		c.Check(packetOff == otherOff, "fixture mains should share a typedef offset")
		c.Check(packetMain.SetSupplementary(fx.data) == nil, "packet main should bind")
		c.Check(otherMain.SetSupplementary(fx.data) == nil, "other main should bind")
		pt, errP := packetMain.Type(packetOff)
		ot, errO := otherMain.Type(otherOff)
		c.Check(errP == nil && errO == nil, "both fixture mains should resolve: %v %v", errP, errO)
		if ptd, ok := pt.(*TypedefType); ok {
			if otd, ok := ot.(*TypedefType); ok {
				c.Check(ptd.Name == "Packet" && otd.Name == "Other",
					"each main should use its own typedef name, got %q and %q", ptd.Name, otd.Name)
				c.Check(pt != ot, "same-offset typedefs in different mains must not share identity")
				c.Check(ptd.Type == otd.Type, "both mains should reuse the supplementary Header identity")
			}
		}

		// A rejected identity mismatch must leave the existing
		// association intact.
		mismatch := newEmptyObject(t, suppSection(false, 5, true, "", []byte{1, 2, 3, 4}))
		c.Check(packetMain.SetSupplementary(mismatch) != nil, "mismatched identity should be rejected")
		pt2, errP2 := packetMain.Type(packetOff)
		c.Check(errP2 == nil && pt2 == pt, "the original association should be unchanged after a rejected bind")
		if ptd, ok := pt2.(*TypedefType); ok {
			c.Check(ptd.Type == pt.(*TypedefType).Type, "the shared shape should remain reachable after rejection")
		}
	})
}
