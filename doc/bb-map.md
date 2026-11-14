# LLVM basic block address map example

`examples/bb_map/bb_map.cpp` builds a read-only index of every
`SHT_LLVM_BB_ADDR_MAP` (`0x6fff4c0a`) section in an ELF file and answers one
address query. The input is never modified, relocated or executed. Only
ELF64 little-endian `EM_X86_64` files of type `ET_REL`, `ET_EXEC` and
`ET_DYN` are accepted, with version 2 (features 0/8) and version 3
(features 0/8/32/40) map records.

## Usage

```
bb_map <elf-path> <code-section-index> <decimal-offset>
```

The offset is relative to the start of the code section named by the
section index, not a virtual address.

Every nonempty basic block whose half-open range contains the offset is
printed as one TSV row. Blocks are listed in physical
map/function/range/block order, so overlapping blocks print in a stable
order. Zero-size blocks and gaps between blocks never match. If no block
covers the address, the program prints `UNCOVERED`.

## TSV columns

Columns are tab separated and appear in this order. Numeric fields are
decimal unless noted.

| # | Column | Meaning |
|---|--------|---------|
| 1 | `map_section` | Index of the `SHT_LLVM_BB_ADDR_MAP` section holding the record |
| 2 | `function_offset` | Byte offset of the entry record (version byte) inside the map section; together with column 1 this is the function identity |
| 3 | `range_ordinal` | Ordinal of the range inside the entry record, starting at 0. Range 0 identifies the function entry; later ranges are hot/cold split ranges of the same function |
| 4 | `block_ordinal` | Ordinal of the block inside the function, across all ranges, in physical record order |
| 5 | `block_id` | Raw block ID from the record. IDs are unique per function but need not increase with the address |
| 6 | `code_section` | Index of the code section the block was resolved to |
| 7 | `begin` | Section-relative half-open begin offset of the block |
| 8 | `end` | Section-relative half-open end offset; an end equal to the section size is valid |
| 9 | `metadata` | Raw metadata ULEB value, preserved unchanged |
| 10 | `callsites` | Comma-separated section-relative callsite offsets, ordered as encoded; empty when the block has none. These are metadata positions, not return PCs |
| 11 | `map_begin` | Byte offset of the block record (ID ULEB) inside the map section |
| 12 | `map_end` | Byte offset one past the block record (after metadata) |
| 13 | `base_file_offset` | Physical file offset of the owning range's 8-byte base field, derived from `sh_offset` (never a virtual address) |
| 14 | `base_offset` | Section-relative offset of the range base |
| 15 | `raw_base` | Raw 8-byte base field value as stored; for `ET_REL` this is normally zero and is not the resolved address |
| 16 | `rela` | `RELA-section:entry-index` of the relocation resolving the base field, or `-` for linked objects |
| 17 | `symbol` | `symbol-table:symbol-index` named by that relocation's `sh_link` and `r_info`, or `-` |
| 18 | `addend` | Signed RELA addend used in the resolution, or `-` |

### Address resolution

* For `ET_REL`, each base field must have exactly one `R_X86_64_64`
  relocation in an `SHT_RELA` whose `sh_info` is the map section. The
  referenced defined `STT_SECTION`/`STT_FUNC` symbol supplies the code
  section, and the section-relative base is `st_value` plus the signed
  addend. The raw base bytes are ignored.
* For `ET_EXEC`/`ET_DYN`, the base virtual address is mapped to the unique
  executable `SHT_PROGBITS` section containing the complete range, and the
  section offset is the address minus `sh_addr`. Ambiguous or absent
  mappings are errors.

Any unsupported construct or malformed record aborts the whole build with a
diagnostic carrying the section index and byte offset; no partial index is
published.

## Library API

The index is built with `ELFIO::bb_map_index::load(const elfio&,
bb_map_error&)`. It owns all copied records, so the source `elfio` object
may be destroyed afterwards. The index exposes:

* `get_functions_num()` / `get_function(index)` — entry records with their
  ranges (`bb_map_range`, including base field and relocation identity) and
  blocks;
* `get_blocks_num()` / `get_block(index)` — blocks in physical order;
* `find(code_section, offset)` — all overlapping nonempty blocks;
* `find_function(map_section, entry_offset)` — lookup by record identity.
