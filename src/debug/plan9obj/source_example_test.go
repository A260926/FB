// Copyright 2014 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package plan9obj

import (
	"bytes"
	"encoding/binary"
	"fmt"
)

// ExampleFile_SourceMap builds a minimal Plan 9 a.out image in memory and
// uses a SourceMap to translate a few program counters into line numbers.
// It uses only the standard library and never reads a source file.
func ExampleFile_SourceMap() {
	// Compressed pc/line records, starting one quantum before the text
	// section with line 0:
	//	5    -> advance one quantum, line 5
	//	130  -> jump one extra quantum, line unchanged (b-129 == 1)
	//	2    -> advance one quantum, line 7
	pcsz := []byte{5, 130, 2}

	var img bytes.Buffer
	for _, word := range []uint32{
		Magic386, // magic
		8,        // text size
		0,        // data size
		0,        // bss size
		0,        // symbol table size
		0,        // entry point
		0,        // spsz size
		uint32(len(pcsz)),
	} {
		var tmp [4]byte
		binary.BigEndian.PutUint32(tmp[:], word)
		img.Write(tmp[:])
	}
	img.Write(make([]byte, 8)) // text section contents
	img.Write(pcsz)

	f, err := NewFile(bytes.NewReader(img.Bytes()))
	if err != nil {
		panic(err)
	}

	// The text section is loaded at 0x1000 with a one-byte instruction
	// quantum. The start is given explicitly rather than taken from Entry.
	m, err := f.SourceMap(0x1000, 1)
	if err != nil {
		panic(err)
	}

	for _, pc := range []uint64{0x1000, 0x1001, 0x1002, 0x1003, 0x1004} {
		loc, ok := m.Lookup(pc)
		if ok {
			fmt.Println(loc.Line, ok)
		} else {
			fmt.Println(0, ok)
		}
	}
	// Output:
	// 5 true
	// 5 true
	// 5 true
	// 7 true
	// 0 false
}
