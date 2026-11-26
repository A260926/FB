ELF Reader
==========

About
-----

This library is for reading ELF files using the Go programming language. Go's
standard library already includes ELF-related functions, but these do not
include some useful functionality for displaying or accessing some aspects of
ELF files out-of-the-box.

This library supports both big and little-endian 32- and 64-bit ELF files. You
can specifically read 32-bit files by calling `ParseELF32File(...)`, or 64-bit
files by `ParseELF64File(...)`, or either type of file using
`ParseELFFile(...)`. If you use the generic `ParseELFFile(...)` function, then
you can either use the returned `ELFFile` interface directly, or use type
assertions to retrieve a 32-bit `*ELF32File` or a 64-bit `*ELF64File`.

Usage
-----

The following example shows how this library can be used to output section
names to standard output. For a more complete example of how to read
information from an ELF file, see the command-line tool at
`elf_view/elf_view.go`.

```go
import (
	"fmt"
	"github.com/yalue/elf_reader"
	"os"
)

func main() {
	// Print the section names in /bin/bash. This code will work on both 32-bit
	// and 64-bit systems.
	raw, e := os.ReadFile("/bin/bash")
	if e != nil {
		fmt.Printf("Failed reading /bin/bash: %s\n", e)
		return
	}
	elf, e := elf_reader.ParseELFFile(raw)
	if e != nil {
		fmt.Printf("Failed parsing ELF file: %s\n", e)
		return
	}
	count := elf.GetSectionCount()
	for i := uint16(0); i < count; i++ {
		if i == 0 {
			fmt.Printf("Section 0: NULL section (no name)\n")
			continue
		}
		name, e := elf.GetSectionName(uint16(i))
		if e != nil {
			fmt.Printf("Failed getting section %d name: %s\n", i, e)
			continue
		}
		fmt.Printf("Section %d name: %s\n", i, name)
	}
}
```

Reading Dynamic Imports Without Section Headers
-----------------------------------------------

Some 64-bit ELF images—such as stripped shared libraries—contain program
headers but no section table. For these, `ReadDynamicImports(f *ELF64File)`
catalogs the image's imported (undefined) dynamic symbols using only the
`PT_DYNAMIC` segment and the file-backed portions of `PT_LOAD` segments.
Both little- and big-endian 64-bit images are supported; 32-bit images are
not. The image is only inspected: it is never loaded or executed, and the
`ELF64File`'s `Raw`, `Sections`, and `Segments` are left unmodified.

Each returned record holds the symbol's original table index, name,
binding, and type, plus its raw `DT_VERSYM` value (hidden bit included),
the 15-bit version index with the hidden bit removed, and the required
version and dependency file names when the image carries version
requirements. The reader:

- Resolves the virtual addresses in dynamic tags (e.g. `DT_SYMTAB`,
  `DT_STRTAB`) through `PT_LOAD` mappings rather than treating them as
  file offsets. Reads may span adjacent mappings, but addresses in
  zero-filled segment tails or unmapped holes are rejected, as are
  overlapping mappings that disagree about an address's file source.
- Determines the dynamic symbol count from `DT_HASH` (`nchain`) or
  `DT_GNU_HASH` (buckets and chain terminators, including the `symoffset`
  prefix). If both tables are present their counts must agree; if neither
  is present the image is reported as unsupported.
- Associates imports with symbol versions via `DT_VERSYM` and the
  `DT_VERNEED`/`DT_VERNEEDNUM` chains, validating record counts, relative
  links, and terminating zero links along the way.
