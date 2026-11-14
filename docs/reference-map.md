# Binary Reference Map

The binary reference map is a read-only index of the references encoded in an
**unlinked** WebAssembly object file (a module that carries `reloc.*` and
`linking` custom sections).  It answers two questions directly from the
bytes, without linking, rewriting, instantiating or executing the module:

1. Given a linking symbol, which bytes of the file reference it?
2. Given one instruction or data field, does it reference a type, a function,
   a global, a table, a data symbol or a section symbol?

The public entry point is
`wabt::ReadBinaryReferenceMap()` declared in
`wabt/binary-reference-map.h`.  It takes a read-only `ByteSpan` and
`ReadBinaryOptions`, and on success returns a self-contained
`BinaryReferenceMap` plus any diagnostics in an `Errors` collection.  Every
string and byte range in the report is owned by the report, so the input
buffer may be released as soon as the call returns.

## How field byte ranges are found

The map does not search for numeric values, guess instruction ends, or run a
second instruction decoder.  The regular binary reader is extended with two
optional range callbacks that fire while the original bytes are being
consumed:

* `OnOpcodeImmediateRange` records the exact file span of each instruction
  immediate, including deliberately padded five-byte LEB128 encodings and the
  signed LEB128 used by block type indices.
* `OnFunctionBodySizeRange` records the span of each function body size field
  so relocations landing there can be rejected.

The two immediates of `call_indirect` (and `return_call_indirect`) are
recorded as separate fields of the *same* instruction.  When one instruction
produces two reference records, both stay attached to that instruction; the
next instruction can never take over one of the fields.

## Supported relocation types and locations

Support is intentionally limited to the following combinations.  Any other
relocation type produces an explicit `unsupported relocation type` error.

| Relocation type       | Allowed location                         | Field width |
|-----------------------|------------------------------------------|-------------|
| `FuncIndexLEB`        | Code: `call`, `return_call`, `ref.func`  | LEB128      |
| `TypeIndexLEB`        | Code: block/loop/if type, `call_indirect` type | LEB128 |
| `GlobalIndexLEB`      | Code: `global.get`, `global.set`         | LEB128      |
| `TableNumberLEB`      | Code: `call_indirect` table, `table.*`   | LEB128      |
| `MemoryAddressSLEB`   | Code: `i32.const`                        | signed LEB128 |
| `MemoryAddressI32`    | Data segment contents                    | 4 bytes     |
| `FunctionOffsetI32`   | Ordinary custom section contents         | 4 bytes     |
| `SectionOffsetI32`    | Ordinary custom section contents         | 4 bytes     |

Offsets are relative to the start of the target section **payload**.  For
custom sections the payload includes the name length and the name bytes, so
the content begins after the name.  Physical section indices count every
section, custom sections included; two custom sections with the same name are
kept as separate sections and are never merged.

Fixed-width fields must stay inside the contents that own them: a
four-byte field overrunning a data segment or a custom section is an error.
Relocations that land on an opcode, in the middle of an immediate, on a local
variable declaration or on a function body size field are reported as
location errors.  Other instructions are consumed normally by the reader and
need no reference type.

## Symbol interpretation

`TypeIndexLEB` addresses the type index space directly and never consults the
symbol table.  Every other relocation names a linking symbol, whose kind is
checked against the relocation:

* function, global and table symbols resolve to the corresponding index
  spaces, preserving symbol index, flags, name and definition location;
* undefined function/global/table symbols recover their `module`/`field`
  identity from the import section, even when imports of different kinds are
  interleaved or names appear late;
* section symbols point at a physical section;
* defined data symbols keep their segment, offset and size, and an undefined
  data symbol stays an external symbol instead of gaining a fake zero
  definition.

When several defined data symbol ranges fully cover one data field, every
covering symbol is retained as a target alias; the report never picks one by
name or first match.

## Indexes and ordering

The report keeps the original relocation tables and their entry order,
including out-of-order entries and several tables targeting the same section.
After the whole file has been read it builds:

* `IncomingReferences(symbol)` — records referencing the symbol;
* `OutgoingReferences(symbol)` — records physically contained in the symbol's
  own definition (a function body, a covering data range or a section).

The indexes store positions into `records`, so each record is held exactly
once.  A `reloc.*` section is allowed to appear before the `linking` section,
but the section it targets must already have appeared.  The reader never
emits a linked artifact.

Any malformed index, out-of-range definition, kind/field mismatch or
duplicated/overlapping field fails the whole read with a diagnostic that names
the relocation table section and entry; no partial report is published.  A
failed read does not affect later reads, and a previously produced map stays
valid after its input is released.

## Command line

`wasm-objdump --reference-map file.wasm` prints the index.  The flag works on
its own and together with `-d`, `-x` and `-j`; `-j SECTION` only filters the
source sections that are displayed, never the symbol associations.  On a
library failure the tool exits non-zero, prints diagnostics and suppresses the
report.  Without the flag, all historical output is unchanged.
