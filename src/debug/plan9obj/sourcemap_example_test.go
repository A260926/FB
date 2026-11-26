// Copyright 2024 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package plan9obj

import (
	"bytes"
	"encoding/binary"
	"fmt"
)

// ExampleFile_SourceMap builds a tiny Plan 9 a.out image in memory and
// resolves a code address back to a source location. No external
// addr2line tool and no source files are involved.
func ExampleFile_SourceMap() {
	const (
		magic     = Magic386
		textStart = uint64(0x1000)
		textSize  = uint32(4)
	)

	// A pc/line stream of one-byte records: each record adds one to
	// the line and one quantum to the program counter.
	pcsz := []byte{7, 1, 1, 1}

	var img bytes.Buffer
	for _, v := range []uint32{
		magic,
		textSize, // text
		0,        // data
		0,        // bss
		0,        // syms
		0,        // entry
		0,        // spsz
		uint32(len(pcsz)),
	} {
		binary.Write(&img, binary.BigEndian, v)
	}
	img.Write(make([]byte, textSize)) // text section
	img.Write(pcsz)                   // pcsz section

	f, err := NewFile(bytes.NewReader(img.Bytes()))
	if err != nil {
		panic(err)
	}
	m, err := f.SourceMap(textStart, 1)
	if err != nil {
		panic(err)
	}
	loc, ok := m.Lookup(textStart + 2)
	fmt.Println(ok, loc.Line)
	// Output: true 9
}
