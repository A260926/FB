// Copyright 2025 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package dwarf_test

// This example shows the intended offline use of supplementary objects:
// the caller supplies the main object, the supplementary object, a type
// offset, and a caller-synthesized byte sample, and gets back a
// member-by-member record that can serve as the source of a forensic
// interpretation. The type is reached through a typedef in the main
// object that refers into the supplementary object.
//
// Only fixed-size, non-bitfield members are interpreted. Bit fields and
// floating-point members are reported as not interpretable, pointers show
// their stored address without being followed, and members that fall
// outside the sample are reported explicitly.

import (
	"encoding/binary"
	"fmt"
)

// exampleT adapts the fixture helpers, which take a testing.TB-like
// interface, to use from an example.
type exampleT struct{}

func (exampleT) Helper()                                   {}
func (exampleT) Fatalf(format string, args ...interface{}) { panic(fmt.Sprintf(format, args...)) }

func ExampleData_SetSupplementary() {
	t := exampleT{}
	sup := buildSupFixture(t)
	main, typedefOff := buildMain(t, mainSpec{
		asize: 4, nameForm: fString, name: "Packet",
		refForm: fRefSup4, ref: uint64(sup.structOff), supSec: mainSupSection(),
	})
	if err := main.SetSupplementary(sup.data); err != nil {
		fmt.Println("bind:", err)
		return
	}

	// The main object's typedef resolves through DW_FORM_ref_sup4 into
	// the supplementary object's second compilation unit.
	typ, err := main.Type(typedefOff)
	if err != nil {
		fmt.Println("type:", err)
		return
	}

	// The caller-provided bytes: a 24-byte Header value.
	sample := make([]byte, 24)
	binary.LittleEndian.PutUint32(sample[0:], 0x12345678)
	sample[4] = 2 // version
	sample[5] = 0x5a
	binary.LittleEndian.PutUint32(sample[8:], 0x3fc00000) // 1.5
	binary.LittleEndian.PutUint64(sample[16:], 0x1122334455667788)

	records, err := interpret("supplementary", sup.structOff, typ, sample, binary.LittleEndian)
	if err != nil {
		fmt.Println("interpret:", err)
		return
	}
	for _, m := range records {
		fmt.Printf("%s %s [%d,%d) raw=% x value=%q note=%q\n",
			m.TypeID, m.Member, m.Start, m.End, m.Raw, m.Value, m.Note)
	}

	// A truncated sample makes the tail members explicitly uninterpretable
	// instead of reading past the supplied bytes.
	short := sample[:12]
	records, err = interpret("supplementary", sup.structOff, typ, short, binary.LittleEndian)
	if err != nil {
		fmt.Println("interpret:", err)
		return
	}
	for _, m := range records {
		fmt.Printf("%s %s [%d,%d) value=%q note=%q\n",
			m.TypeID, m.Member, m.Start, m.End, m.Value, m.Note)
	}

	// Output:
	// supplementary@42 Packet magic [0,4) raw=78 56 34 12 value="305419896" note=""
	// supplementary@42 Packet version [4,5) raw=02 value="2" note=""
	// supplementary@42 Packet flags [5,6) raw=5a value="" note="not interpretable: bit field"
	// supplementary@42 Packet ratio [8,12) raw=00 00 c0 3f value="" note="not interpretable: floating-point member"
	// supplementary@42 Packet next [16,24) raw=88 77 66 55 44 33 22 11 value="0x1122334455667788" note="pointer address (not dereferenced)"
	// supplementary@42 Packet magic [0,4) value="305419896" note=""
	// supplementary@42 Packet version [4,5) value="2" note=""
	// supplementary@42 Packet flags [5,6) value="" note="not interpretable: bit field"
	// supplementary@42 Packet ratio [8,12) value="" note="not interpretable: floating-point member"
	// supplementary@42 Packet next [16,24) value="" note="not interpretable: out of sample bounds"
}
