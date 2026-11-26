# elf-parser
Lightweight elf binary parser with no external dependencies - Sections, Symbols, Relocations, Segments

Files are read only as static bytes: nothing is executed and no relocation is
applied. The same API reads ELF32 and ELF64 objects in either byte order,
decoding every field from the file's own `EI_CLASS` / `EI_DATA` rather than
from the host layout.

# Required libraries
No dependencies. The ELF format declarations used at build time are the pinned
musl `elf.h` under `task_support/vendor`; no platform ELF development package is
required.

# File metadata

```cpp
uint8_t  elf_class();      // ELFCLASS32 / ELFCLASS64
uint8_t  data_encoding();  // ELFDATA2LSB / ELFDATA2MSB
uint16_t machine();        // EM_386 / EM_PPC / EM_X86_64 / EM_PPC64
```

Supported targets: 32-bit little-endian i386, 32-bit big-endian PowerPC,
64-bit little-endian x86-64 and 64-bit big-endian PowerPC64. An unsupported
magic, class, encoding or machine makes the constructor throw
`std::runtime_error` instead of terminating the caller.

# Usage

## Sections (readelf -S executable)
parse sections header table from elf binary and return vector of section_t below

```cpp
typedef struct {
    uint32_t section_index;
    uint64_t section_offset, section_addr;
    std::string section_name;
    std::string section_type;
    uint64_t section_size, section_ent_size, section_addr_align;
    // Raw sh_type / sh_link / sh_info / sh_flags.
    uint32_t section_type_id, section_link, section_info;
    uint64_t section_flags;
} section_t;
```

All offsets, addresses, sizes, entry sizes and alignments are `uint64_t`, so
they survive 64-bit values regardless of host word size.

```cpp
#include <elf_parser.hpp>
elf_parser::Elf_parser elf_parser(executable_path);
std::vector<elf_parser::section_t> secs = elf_parser.get_sections();
```
see [example](examples/sections.cc)

## Segments (readelf -l executable)
parse program header table from elf binary and return vector of segment_t below

```cpp
typedef struct {
    std::string segment_type, segment_flags;
    uint64_t segment_offset, segment_virtaddr, segment_physaddr;
    uint64_t segment_filesize, segment_memsize;
    uint64_t segment_align;
} segment_t;
```

`p_flags` is read from the class-specific slot: directly after `p_type` on
ELF64, but after `p_memsz` on ELF32.

```cpp
#include <elf_parser.hpp>
elf_parser::Elf_parser elf_parser(executable_path);
std::vector<elf_parser::segment_t> segs = elf_parser.get_segments();
```
see [example](examples/segments.cc)

## Symbols (readelf -s executable)
parse symbol table from elf binary and return vector of symbol_t below

```cpp
typedef struct {
    std::string symbol_index;
    uint64_t symbol_value;
    uint32_t symbol_num;
    uint64_t symbol_size;
    std::string symbol_type, symbol_bind, symbol_visibility, symbol_name, symbol_section;
    // Physical section index of the symbol table holding this record.
    uint32_t symbol_table_index;
} symbol_t;
```

Each symbol table is linked to its own string table through `sh_link` (section
names are never matched by literal names such as `.strtab`). Records keep their
original on-disk order, including the mandatory null symbol at index zero, and
the tables are returned in physical section order. The ELF32 and ELF64 layouts
place `st_info`/`st_other` differently and both are handled.

```cpp
#include <elf_parser.hpp>
elf_parser::Elf_parser elf_parser(executable_path);
std::vector<elf_parser::symbol_t> syms = elf_parser.get_symbols();
```
see [example](examples/symbols.cc)

## Relocations (readelf -r executable)
parse relocations and return vector of relocation_t below

```cpp
typedef struct {
    uint64_t relocation_offset, relocation_info, relocation_symbol_value;
    std::string   relocation_type, relocation_symbol_name, relocation_section_name;
    uint64_t relocation_plt_address;
    uint32_t relocation_type_id;
    uint32_t relocation_symbol_table_index, relocation_symbol_index;
    uint32_t relocation_target_section_index;
    bool relocation_has_addend;
    int64_t relocation_addend;
} relocation_t;
```

The `r_info` split into symbol index and type id follows the class
(high 24 / low 8 bits on ELF32; high 32 / low 32 on ELF64), and type names are
interpreted per `e_machine`. A relocation section reaches its owning symbol
table via its own `sh_link` and uses the original record number there;
`relocation_target_section_index` is its `sh_info`.

`RELA` entries set `relocation_has_addend = true` and expose the explicit,
sign-extended `relocation_addend` (32-bit addends are sign extended for
ELF32). `REL` entries set `relocation_has_addend = false` and
`relocation_addend = 0`; no implicit addend is inferred from target
instruction bytes. `relocation_plt_address` remains a display field using the
legacy `.plt` entry calculation.

```cpp
#include <elf_parser.hpp>
elf_parser::Elf_parser elf_parser(executable_path);
std::vector<elf_parser::relocation_t> relocs = elf_parser.get_relocations();
```
see [example](examples/relocations.cc)

# Scope
Ordinary, uncompressed sections and ordinary section numbers only. Extended
section numbering (`SHN_XINDEX`), `SHNDX` attachments, packed/Android
relocations, dynamic-loader resolution, compressed sections and relocation
application are out of scope.

# Supported Architecture
i386 (ELF32, little endian), PowerPC (ELF32, big endian),
x86-64 (ELF64, little endian), PowerPC64 (ELF64, big endian)

# Projects using elf-parser
[finixbit / ltrace - Library call tracer](http://github.com/finixbit/ltrace)

[finixbit / ftrace - Function call tracer](http://github.com/finixbit/ftrace)
