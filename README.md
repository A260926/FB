# libbacktrace
A C library that may be linked into a C/C++ program to produce symbolic backtraces

Initially written by Ian Lance Taylor <iant@golang.org>.

This is version 1.0.
It is likely that this will always be version 1.0.

The libbacktrace library may be linked into a program or library and
used to produce symbolic backtraces.
Sample uses would be to print a detailed backtrace when an error
occurs or to gather detailed profiling information.

In general the functions provided by this library are async-signal-safe,
meaning that they may be safely called from a signal handler.
That said, on systems that use `dl_iterate_phdr`, such as GNU/Linux,
the first call to a libbacktrace function will call `dl_iterate_phdr`,
which is not in general async-signal-safe.  Therefore, programs
that call libbacktrace from a signal handler should ensure that they
make an initial call from outside of a signal handler.
Similar considerations apply when arranging to call libbacktrace
from within malloc; `dl_iterate_phdr` can also call malloc,
so make an initial call to a libbacktrace function outside of
malloc before trying to call libbacktrace functions within malloc.

The libbacktrace library is provided under a BSD license.
See the source files for the exact license text.

The public functions are declared and documented in the header file
backtrace.h, which should be #include'd by a user of the library.

Building libbacktrace will generate a file backtrace-supported.h,
which a user of the library may use to determine whether backtraces
will work.
See the source file backtrace-supported.h.in for the macros that it
defines.

As of July 2024, libbacktrace supports ELF, PE/COFF, Mach-O, and
XCOFF executables with DWARF debugging information.
In other words, it supports GNU/Linux, *BSD, macOS, Windows, and AIX.
The library is written to make it straightforward to add support for
other object file and debugging formats.

The library relies on the C++ unwind API defined at
https://itanium-cxx-abi.github.io/cxx-abi/abi-eh.html
This API is provided by GCC and clang.

# Offline split DWARF attachment

The functions described here support offline symbolization of synthetic
debug sections without running the image they describe and without
maintaining a second symbolizer. They are internal interfaces declared
in `internal.h`; function and inline-chain information continues to be
delivered through the ordinary `backtrace_pcinfo` full callback.

A main object added with `backtrace_dwarf_add` may contain a DWARF 5
`DW_UT_skeleton` compilation unit. The skeleton owns the PC ranges, the
real line number program and `.debug_addr`, but its function DIEs live in
a separate `DW_UT_split_compile` unit (a `.dwo` bundle). Two entry
points bind such a bundle to the unique skeleton whose 64-bit `dwo_id`
matches:

* `backtrace_dwarf_attach_split(state, main_entry, split_sections,
  is_bigendian, error_callback, data)` binds sections that the caller has
  already unpacked. `split_sections` is indexed by the existing
  `DEBUG_*` numbers, with the `.dwo` suffix removed
  (`.debug_info.dwo` is supplied as `DEBUG_INFO`, and so on).
* `backtrace_elf_attach_split(state, main_entry, path, error_callback,
  data)` opens an ELF64 DWO file read-only through the production
  `elf.c` section reader, extracts its uncompressed `.debug_*.dwo`
  sections and performs the same binding. It does not parse the file a
  second way and never appends the file as an ordinary module to the
  fileline query chain.

Both return 1 on success and 0 after reporting an error when the
`dwo_id` matches no skeleton, matches more than one skeleton
ambiguously, or the skeleton is already bound. A failed attachment never
changes units that were already readable. Only non-threaded
`backtrace_state` objects are supported.

After a successful attachment, querying a PC covered by the skeleton
through `backtrace_pcinfo` immediately reports the function tree and
inline chain from the split unit. The innermost executed row (file and
line) always comes from the main object's line program; inline call
sites use the split unit's own DWARF 5 file table. Split `addrx` forms
are resolved through the skeleton's `addr_base` in the main object's
`.debug_addr`; split `strx` forms use the split unit's own
`.debug_str_offsets` contribution, defaulting to just past the
DWARF 32/64 section header when `DW_AT_str_offsets_base` is omitted.
Numeric offsets and string indices are never shared between the two
objects: equal local offsets or index zero in a DWO never resolve into
the main object. rnglists, type units and supplementary references are
not supported.

## Section lifecycle

* For `backtrace_dwarf_attach_split`, the buffers referenced by
  `split_sections` must stay valid for the lifetime of `state`. The
  attachment copies every byte it needs into library-owned storage, so
  once the call returns the caller may release, reuse or delete the
  input buffers; later PC queries keep working from the copies.
* For `backtrace_elf_attach_split`, the file descriptor and all section
  views are released before the function returns, and the section bytes
  are likewise copied. The DWO file may be closed or deleted afterwards
  without affecting later queries.
* The main object's sections passed to `backtrace_dwarf_add`, including
  its `.debug_addr`, must remain valid for the lifetime of `state`,
  because split `addrx` resolution borrows that contribution.

## Offline crash-evidence workflow

A typical offline flow builds a `backtrace_state`, adds each synthetic
main object with `backtrace_dwarf_add`, attaches its explicitly
identified skeleton/DWO pair through one of the entry points above, and
then feeds a fixed local list of PC samples to `backtrace_pcinfo`. Each
callback records module identity, PC, function, the inline call chain,
and file/line. Samples are fixed inputs, never collected from a running
process. A sample whose split object has not been attached still
produces the skeleton's file/line row with the function reported as
missing, so evidence from one build can never be combined with a
different build's function names into a seemingly complete chain.

