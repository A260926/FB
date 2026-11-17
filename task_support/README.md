Read-only FatELF workbench

`bash task_support/run.sh baseline` builds the original libelf production code,
generates inert ELF/FatELF/ar bytes in `.task-build/samples`, and calls the real
ELF/GElf section, symbol, string, archive and raw-file APIs. No sample is executed.
`bash task_support/run.sh target` additionally compiles and runs
`tests/fatelf_contract.c`; register `records`, `views`, `lifetime` cases through
`task_support/check.h`, with TASK_CHECK at the actual assertions. The test target
is intentionally absent from the starting source. The fixture helpers are basic
well-formed samples, not a FatELF reader or a regression implementation.

Format reference: https://github.com/icculus/fatelf/blob/main/docs/fatelf-specification.txt
FatELF v1 header has 8 bytes; each record is 24 bytes. Its integer fields are
always little endian. Contained ELF headers retain their own class and byte order.
The two provided records are ELF64LE x86_64 and ELF32BE PowerPC, at 4096 and 8192.
Both define the symbol `same` in section 1, with values 1 and 2 respectively.
`normal.a` contains the two ordinary ELF files; `nested.a` contains the FatELF
container followed by the ordinary ELF32 file.

The new APIs requested in the user prompt are additions to the library. Existing
ELF_K_NONE/AR/COFF/ELF numeric values must remain stable. A recognized FatELF with
invalid container metadata should fail the opening call with NULL and elf_errmsg.
Inner ELF header validation is done when opening that record, allowing another
valid record to remain usable after an inner failure. Metadata inspection does
not change selection. Opening past the last record returns NULL. A record is
identified by its index within its own container; selecting it repeatedly may
reuse a live descriptor using the existing activation-count convention.

`build.sh` compiles every `lib/*.c` (including newly added production files) into
a static library, then links both the baseline and target to it. Only the system
C compiler, ar, bash and POSIX libc are used. It requires an LP64 host; a compile
check validates the byte and integer widths. No compiler/package caches are used.
The two config headers in `config/` are the output of this source commit's
configure probes on LP64 macOS, with 64-bit ELF and symbol versioning enabled,
NLS and shared libraries disabled. The source's own elf_repl.h supplies ELF types.
They are supplied because this old Autoconf's host detection and generated sed
script fail on modern Darwin; no library implementation is changed by this
build route. Compilation remains strict C11 and does not suppress diagnostics.
The source headers and functions are retained under COPYING.LIB (LGPL-2.0-or-later).
