// Command arm64x-view is a read-only example of inspecting the native and the
// x64-compatible view of an ARM64X PE image.
//
// It never loads the image, never executes it and never writes anything back:
// the compatible view is synthesized in a private in-memory copy, while the
// input bytes (and any other File sharing them) stay untouched.
//
// Usage:
//
//	go run ./examples/arm64x-view [path-to-arm64x.dll]
//
// Without an argument it parses the synthetic fixture image shipped under
// task_support/fixture.
package main

import (
	"bytes"
	"fmt"
	"os"

	pe "github.com/saferwall/pe"
	"github.com/saferwall/pe/task_support/fixture"
)

func main() {
	// Load the image bytes read-only. NewBytes never takes ownership in a way
	// that would mutate the slice: the compatible view is built on a private
	// copy instead.
	var input []byte
	if len(os.Args) > 1 {
		var err error
		input, err = os.ReadFile(os.Args[1])
		if err != nil {
			fmt.Fprintf(os.Stderr, "read %s: %v\n", os.Args[1], err)
			os.Exit(1)
		}
	} else {
		input = fixture.Image()
	}
	snapshot := append([]byte(nil), input...)

	// First pass: the default, native ARM64 view.
	native, err := pe.NewBytes(input, &pe.Options{})
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	if err := native.Parse(); err != nil {
		fmt.Fprintln(os.Stderr, "native parse:", err)
		os.Exit(1)
	}
	printView("native", native)
	printFixups(native)

	// A second File over the same bytes selects the x64-compatible view.
	alternate, err := pe.NewBytes(input, &pe.Options{ARM64XAlternate: true})
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	if err := alternate.Parse(); err != nil {
		fmt.Fprintln(os.Stderr, "alternate parse:", err)
		os.Exit(1)
	}
	printView("compatible", alternate)

	// Fast mode still reports the corrected header; it only skips directories.
	fast, err := pe.NewBytes(input, &pe.Options{Fast: true, ARM64XAlternate: true})
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	if err := fast.Parse(); err != nil {
		fmt.Fprintln(os.Stderr, "fast parse:", err)
		os.Exit(1)
	}
	fmt.Printf("fast-compatible machine=0x%x export_parsed=%t\n",
		uint16(fast.NtHeader.FileHeader.Machine), fast.HasExport)

	// The original input must be byte-identical to the starting snapshot even
	// though two compatible-view parses ran over it.
	if !bytes.Equal(input, snapshot) {
		fmt.Fprintln(os.Stderr, "input bytes were modified")
		os.Exit(1)
	}
	fmt.Println("input unchanged=true")
}

func printView(label string, f *pe.File) {
	oh := f.NtHeader.OptionalHeader.(pe.ImageOptionalHeader64)
	exportDir := oh.DataDirectory[pe.ImageDirectoryEntryExport]
	fmt.Printf("%s: machine=0x%x entry=0x%x export_dir_rva=0x%x exports=%d\n",
		label, uint16(f.NtHeader.FileHeader.Machine),
		oh.AddressOfEntryPoint, exportDir.VirtualAddress, len(f.Export.Functions))
	for _, fn := range f.Export.Functions {
		fmt.Printf("  export %s rva=0x%x ordinal=%d\n", fn.Name, fn.FunctionRVA, fn.Ordinal)
	}
}

func printFixups(f *pe.File) {
	if f.LoadConfig.DVRT == nil {
		fmt.Println("dvrt: none")
		return
	}
	dvrt := f.LoadConfig.DVRT
	fmt.Printf("dvrt: version=%d size=%d symbol6_fixups=%d\n",
		dvrt.Version, dvrt.Size, len(dvrt.ARM64XFixups))
	for _, rec := range dvrt.ARM64XFixups {
		switch rec.Type {
		case pe.ARM64XFixupDelta:
			fmt.Printf("  src=0x%x target_rva=0x%x type=DELTA width=%d delta=%d\n",
				rec.SourceOffset, rec.TargetRVA, rec.Width, rec.Delta)
		case pe.ARM64XFixupZerofill:
			fmt.Printf("  src=0x%x target_rva=0x%x type=ZEROFILL width=%d\n",
				rec.SourceOffset, rec.TargetRVA, rec.Width)
		default:
			fmt.Printf("  src=0x%x target_rva=0x%x type=VALUE width=%d value=0x%x\n",
				rec.SourceOffset, rec.TargetRVA, rec.Width, rec.Value)
		}
	}
}
