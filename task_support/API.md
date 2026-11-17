The existing class and four vector-returning methods remain the production entry points.
The input is an ordinary, uncompressed ELF32 or ELF64 file in either byte order, with
ordinary section counts and indices. No extended section numbering, SHNDX attachments,
packed Android relocations, dynamic-loader resolution or relocation application is requested.

Expose file metadata as const methods returning the values recorded in e_ident/e_machine:

```cpp
uint8_t elf_class() const;       // ELFCLASS32 / ELFCLASS64
uint8_t data_encoding() const;   // ELFDATA2LSB / ELFDATA2MSB
uint16_t machine() const;        // EM_386 / EM_X86_64 / EM_PPC / EM_PPC64
```

Keep existing field names. All file offsets, virtual/physical addresses, raw relocation
info, byte sizes, entry sizes and alignments are uint64_t, including section_size,
symbol_size and relocation_symbol_value. Indices and counts fit uint32_t here;
symbol_index keeps its existing textual representation of st_shndx.

Add these fields to the public records:

```cpp
// section_t
uint32_t section_type_id, section_link, section_info;
uint64_t section_flags;
// symbol_t
uint32_t symbol_table_index;
// relocation_t
uint32_t relocation_type_id;
uint32_t relocation_symbol_table_index, relocation_symbol_index;
uint32_t relocation_target_section_index;
bool relocation_has_addend;
int64_t relocation_addend;
```

The section fields retain raw sh_type/sh_link/sh_info/sh_flags. A symbol table's
index is its physical section index; symbol_num remains the index within that table,
including the null symbol. symbol_section retains its existing meaning (the containing
symbol-table section's name). For a relocation, the symbol-table and target-section
indices are sh_link/sh_info; its symbol index and type id come from its own r_info.
The symbol name/value refer to that table's entry, whose string is read through the
symbol table's sh_link. Return records in physical table order and original record order.

REL has relocation_has_addend=false and relocation_addend=0. RELA has true and its
sign-extended explicit addend. No target bytes are interpreted as implicit addends.
The existing relocation_plt_address remains a display field using the old .plt entry
calculation; this change does not infer per-architecture PLT instruction layouts.

For type names support the canonical names for ids 1,2,5,6,7 on i386/x86-64, and
1 (R_PPC_ADDR32) and 26 (R_PPC_REL32) on PPC. PPC64 supports 1 (R_PPC64_ADDR32),
26 (R_PPC64_REL32), 38 (R_PPC64_ADDR64) and 44 (R_PPC64_REL64).
On x86-64 id 1 is R_X86_64_64.
Unknown ids still return OTHERS while retaining relocation_type_id. Existing string
labels for section, segment, symbol kind/binding/visibility stay compatible.
Unsupported file magic/class/encoding/machine should throw std::runtime_error instead
of terminating the caller. Support ordinary valid inputs; no fuzzing or exploit work.

Format reference: https://gabi.xinuos.com/elf/contents.html
The vendored elf.h is only the pinned musl format declarations; see vendor/SOURCE.json
and vendor/MUSL-COPYRIGHT. It supplies no parser or new behavior.
