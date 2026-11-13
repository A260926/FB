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

Reading dynamic imports
-----------------------

64-bit ELF files of either endianness can also be examined with
`ReadDynamicImports(f *ELF64File)`, which recovers the imported (undefined)
dynamic symbols along with their version requirements using only the program
headers. This works even for images that have no section table: it reads the
`PT_DYNAMIC` segment up to `DT_NULL`, resolves the virtual addresses carried
by tags such as `DT_SYMTAB`, `DT_STRTAB`, `DT_HASH`, `DT_GNU_HASH`,
`DT_VERSYM`, and `DT_VERNEED` through the file-backed portions of `PT_LOAD`
segments, and never executes or loads the image.

The number of dynamic symbols comes from `DT_HASH`'s `nchain` or from walking
the `DT_GNU_HASH` buckets and chains; when both are present they must agree,
and images with neither are unsupported. Each returned `DynamicImport` keeps
the original symbol table index, name, binding, type, raw version value,
version index with the hidden bit removed, required version name, and
dependency file name. Only undefined symbols with non-empty names are
included, and duplicate names are preserved. Images without `DT_VERSYM`
report a raw version and version index of 1 with empty version and dependency
names. Malformed images (unterminated strings, broken version chains,
conflicting address mappings, and so on) produce errors rather than guesses.
