The dwarf/ directory is an unmodified export of Go's src/debug/dwarf.
LICENSE and PATENTS are retained. Source provenance is recorded with the draft.

run.sh uses a Go overlay to compile this exported code as debug/dwarf, so the
standard library ELF/Mach-O/PE readers and the original tests call this code,
not the installed decoder. Original external tests keep their original imports.
New .go files in dwarf/ are included in the overlay; files found only in the
installed package are suppressed. The test binary runs from dwarf/ so testdata
comes from the export. GOROOT is read only. Go >=1.26 is required.
Each run creates empty build/module caches and disables package/toolchain downloads.

make_samples.go deterministically writes two legal ELF64 relocatable objects.
They contain no code or loadable segments. main.elf contains a local base type
at .debug_info offset 15 and a typedef using supplementary forms. supplement.elf
contains a recursive Node at that same offset. .debug_sup contains role, filename,
and a four-byte opaque identity. This is fixture material, not a resolver.
Regenerate with: go run task_support/make_samples.go

baseline reads the local type and independently reads the supplementary Node.
New supplementary_regression_test.go is required for target mode. Use
example.local/supplementary/task_support/taskcheck for executed assertions.
No target implementation or regression assertions are provided.
