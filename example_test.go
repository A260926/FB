// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package dwarf_test

import (
	"encoding/binary"
	"fmt"

	"github.com/blacktop/go-dwarf"
	"github.com/blacktop/go-dwarf/task_support/fixture"
)

// ExampleNewReaderAt shows how to inspect DWARF whose section offsets
// may exceed 4 GiB without holding the sections in memory. The source
// here is an in-memory sparse section, but any io.ReaderAt (for example
// one backed by a file) works the same way.
func ExampleNewReaderAt() {
	order := binary.LittleEndian

	// One DWARF64 compile unit: a CU root with a strp name and a
	// base type it contains.
	cu := newCUBuilder(order, true)
	cu.beginCU(0) // name string lives at offset 0 of .debug_str
	cu.baseType("base", 9, 7, 4)
	unit := cu.endCU()

	info := &fixture.Sparse{
		Size:  int64(len(unit)),
		Spans: []fixture.Span{{Offset: 0, Data: unit}},
	}
	str := &fixture.Sparse{
		Size: 16,
		Spans: []fixture.Span{
			{Offset: 0, Data: []byte("example\x00")},
			{Offset: 9, Data: []byte("int\x00")},
		},
	}

	d, err := dwarf.NewReaderAt(
		synthAbbrev(),
		dwarf.Section{Reader: info, Size: info.Size},
		dwarf.Section{Reader: str, Size: str.Size},
	)
	if err != nil {
		panic(err)
	}

	r := d.Reader()
	root, err := r.Next()
	if err != nil {
		panic(err)
	}
	fmt.Println("root:", root.Tag, root.Val(dwarf.AttrName))

	base, err := r.Next()
	if err != nil {
		panic(err)
	}
	typ, err := d.Type(base.Offset)
	if err != nil {
		panic(err)
	}
	fmt.Println("type:", typ.String(), typ.Size())
	// Output:
	// root: CompileUnit example
	// type: int 4
}
