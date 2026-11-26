// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package dwarf_test

import (
	"bytes"
	"encoding/binary"
	"fmt"

	"github.com/blacktop/go-dwarf"
	"github.com/blacktop/go-dwarf/task_support/fixture"
)

// ExampleNewReaderAt shows how to open DWARF whose .debug_info and
// .debug_str sections are presented as ReaderAt-backed Sections rather
// than complete byte slices. The same Reader, Seek, SkipChildren and
// Type calls work on both forms; offsets keep their original section
// values even above 4 GiB.
func ExampleNewReaderAt() {
	abbrev, info, str, typeOffset := fixture.Normal(binary.LittleEndian)

	data, err := dwarf.NewReaderAt(
		abbrev,
		dwarf.Section{Reader: bytes.NewReader(info), Size: int64(len(info))},
		dwarf.Section{Reader: bytes.NewReader(str), Size: int64(len(str))},
	)
	if err != nil {
		fmt.Println("error:", err)
		return
	}

	r := data.Reader()
	if _, err := r.Next(); err != nil { // compilation unit
		fmt.Println("error:", err)
		return
	}
	t, err := data.Type(dwarf.Offset(typeOffset))
	if err != nil {
		fmt.Println("error:", err)
		return
	}
	fmt.Printf("%s %d\n", t.String(), t.Size())
	// Output: int 4
}
