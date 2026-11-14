package main

import (
	"bytes"
	"fmt"
	"github.com/Binject/debug/macho"
	"github.com/Binject/debug/task_support/fixture"
	"io"
)

func must(ok bool, msg string) {
	if !ok {
		panic(msg)
	}
}
func main() {
	ordinary, err := macho.NewFile(bytes.NewReader(fixture.Standalone()))
	must(err == nil, fmt.Sprint(err))
	sec := ordinary.Section("__text")
	data, err := sec.Data()
	must(err == nil && len(data) == 16, "normal text")
	opened, err := io.ReadAll(sec.Open())
	must(err == nil && bytes.Equal(opened, data), "normal Open")
	must(len(sec.Relocs) == 1 && len(ordinary.Symtab.Syms) == 4, "normal symbols/relocations")
	dw, err := ordinary.DWARF()
	must(err == nil, fmt.Sprint(err))
	die, err := dw.Reader().Next()
	must(err == nil && die != nil, "normal DWARF")
	fmt.Printf("ordinary text=%d relocations=%d symbols=%d CU=%v\n", len(data), len(sec.Relocs), len(ordinary.Symtab.Syms), die.Val(3))
	raw := fixture.Fileset()
	before := append([]byte(nil), raw...)
	root, err := macho.NewFile(bytes.NewReader(raw))
	must(err == nil, fmt.Sprint(err))
	opaque := 0
	for _, l := range root.Loads {
		if _, ok := l.(macho.LoadBytes); ok {
			opaque++
		}
	}
	_, sliceErr := macho.NewFile(bytes.NewReader(raw[0x1000:0x1600]))
	fmt.Printf("fileset commands=%d opaque_entries=%d naive_child_slice_error=%v\n", len(root.Loads), opaque, sliceErr)
	must(opaque == 2 && sliceErr != nil, "upstream fileset capability changed")
	must(fixture.Unchanged(raw, before), "input changed")
	fmt.Println("baseline: existing readers work; fileset dual-origin support absent")
}
