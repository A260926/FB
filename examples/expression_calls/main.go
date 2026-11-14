// Command expression_calls reads an ELF file and prints the result of
// evaluating the location expression of every DW_TAG_dwarf_procedure DIE,
// following DW_OP_call2, DW_OP_call4 and DW_OP_call_ref to the shared
// sub-expressions they reference.
package main

import (
	"debug/dwarf"
	"debug/elf"
	"fmt"
	"os"
	"sort"

	"github.com/go-delve/delve/pkg/dwarf/godwarf"
	"github.com/go-delve/delve/pkg/dwarf/op"
)

type procedure struct {
	off  dwarf.Offset
	name string
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintf(os.Stderr, "usage: %s <elf-file>\n", os.Args[0])
		os.Exit(2)
	}
	if err := run(os.Args[1]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func run(path string) error {
	f, err := elf.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()

	dw, err := f.DWARF()
	if err != nil {
		return err
	}
	debugInfo, err := godwarf.GetDebugSectionElf(f, "info")
	if err != nil {
		return err
	}
	ev, err := godwarf.NewExpressionEvaluator(dw, debugInfo)
	if err != nil {
		return err
	}

	procs, err := locatedProcedures(dw)
	if err != nil {
		return err
	}

	for _, p := range procs {
		val, pieces, err := ev.Evaluate(p.off, op.DwarfRegisters{}, 8, nil)
		switch {
		case err != nil:
			fmt.Printf("%s (%#x): error: %v\n", p.name, uint64(p.off), err)
		case pieces != nil:
			fmt.Printf("%s (%#x): pieces=%v\n", p.name, uint64(p.off), pieces)
		default:
			fmt.Printf("%s (%#x): %d\n", p.name, uint64(p.off), val)
		}
	}
	return nil
}

// locatedProcedures returns every DW_TAG_dwarf_procedure DIE with a location
// expression, sorted by offset.
func locatedProcedures(dw *dwarf.Data) ([]procedure, error) {
	var procs []procedure
	rdr := dw.Reader()
	for {
		e, err := rdr.Next()
		if err != nil {
			return nil, err
		}
		if e == nil {
			break
		}
		if e.Tag != dwarf.TagDwarfProcedure {
			continue
		}
		if _, ok := e.Val(dwarf.AttrLocation).([]byte); !ok {
			continue
		}
		name, _ := e.Val(dwarf.AttrName).(string)
		procs = append(procs, procedure{off: e.Offset, name: name})
	}
	sort.Slice(procs, func(i, j int) bool { return procs[i].off < procs[j].off })
	return procs, nil
}
