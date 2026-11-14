package macho

// Runnable, self-contained example for the offline compact unwind index.
// It uses the inert task_support/fixture image, builds the index through the
// normal NewFile/Section/VM path, then shows that queries keep working after
// the File is closed because the index owns every byte it decoded.

import (
	"bytes"
	"errors"
	"fmt"

	"github.com/blacktop/go-macho/task_support/fixture"
)

func ExampleFile_BuildCompactUnwindIndex() {
	raw, err := fixture.Normal()
	if err != nil {
		fmt.Println("fixture:", err)
		return
	}

	f, err := NewFile(bytes.NewReader(raw))
	if err != nil {
		fmt.Println("NewFile:", err)
		return
	}

	// One offline parse: no stack reads, no personality/LSDA execution.
	idx, err := f.BuildCompactUnwindIndex()
	if err != nil {
		fmt.Println("BuildCompactUnwindIndex:", err)
		return
	}

	// Lookup takes a preferred VM address verbatim: no slide, no PC-1.
	entry, err := idx.Lookup(fixture.Base + 0x400)
	if err != nil {
		fmt.Println("Lookup:", err)
		return
	}
	fmt.Printf("range: %#x-%#x\n", entry.RangeStart, entry.RangeEnd)
	fmt.Printf("encoding source: %s\n", entry.EncodingSource.Kind)
	fmt.Printf("mode: %s\n", entry.Rule.Mode)
	fmt.Printf("cfa: %s%+d\n", entry.Rule.CFA.Base, entry.Rule.CFA.Offset)
	fmt.Printf("rip: cfa%+d\n", entry.Rule.RIP.OffsetFromCFA)
	fmt.Printf("saved registers: %d\n", len(entry.Rule.SavedRegisters))
	fmt.Printf("entry unwind_info offset: %#x\n", entry.Entry.SectionOffset)

	// Addresses past the last range are reported, not guessed.
	if _, err := idx.Lookup(fixture.Base + 0x440); errors.Is(err, ErrCompactUnwindNotFound) {
		fmt.Println("gap: not found")
	}

	// The index owns its bytes: close the File, then keep querying.
	if err := f.Close(); err != nil {
		fmt.Println("Close:", err)
		return
	}
	again, err := idx.Lookup(fixture.Base + 0x43f)
	if err != nil {
		fmt.Println("Lookup after close:", err)
		return
	}
	fmt.Printf("after close: %#x\n", again.RangeStart)

	// Output:
	// range: 0x100000400-0x100000440
	// encoding source: inline
	// mode: STACK_IMMD
	// cfa: RSP+32
	// rip: cfa-8
	// saved registers: 0
	// entry unwind_info offset: 0x1008
	// gap: not found
	// after close: 0x100000400
}
