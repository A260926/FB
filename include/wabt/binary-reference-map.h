/*
 * Copyright 2026 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef WABT_BINARY_REFERENCE_MAP_H_
#define WABT_BINARY_REFERENCE_MAP_H_

#include <cstdint>
#include <string>
#include <vector>

#include "wabt/binary-reader.h"
#include "wabt/common.h"
#include "wabt/error.h"

namespace wabt {

// Read-only index of the references encoded in an unlinked WebAssembly
// object file.  The map describes where each relocation's bytes physically
// live, what kind of field they are, and which type or linking symbol they
// refer to.  It never links, rewrites, instantiates or executes the module:
// every byte range is observed while the regular binary reader traverses the
// file, and all owned data is copied out so the input buffer can be released
// as soon as ReadBinaryReferenceMap returns.
//
// The supported relocation types are:
//
//   FuncIndexLEB       call / return_call / ref.func immediates (Code)
//   TypeIndexLEB       block type and call_indirect type immediates (Code)
//   GlobalIndexLEB     global.get / global.set immediates (Code)
//   TableNumberLEB     call_indirect table and table.* immediates (Code)
//   MemoryAddressSLEB  i32.const immediates (Code)
//   MemoryAddressI32   four-byte fields inside data segment contents (Data)
//   FunctionOffsetI32  four-byte fields inside ordinary custom contents
//   SectionOffsetI32   four-byte fields inside ordinary custom contents
//
// Every other relocation type is reported as an explicit unsupported error.

// Where the bytes of a reference field live.
struct ReferenceSource {
  enum class Kind {
    FunctionInstruction,
    DataSegment,
    CustomContent,
  };

  Kind kind = Kind::FunctionInstruction;

  // FunctionInstruction: the function's index in the function index space and
  // the file offset of the first opcode byte of the owning instruction.
  Index function_index = kInvalidIndex;
  Offset instruction_offset = kInvalidOffset;

  // DataSegment: index of the data segment and the offset of the field within
  // that segment's contents.
  Index data_segment = kInvalidIndex;
  Offset data_segment_offset = kInvalidOffset;

  // CustomContent: physical section index of the custom section and the
  // offset of the field within its contents (i.e. after the section name, but
  // still relative to the section payload start).
  Index custom_section_index = kInvalidIndex;
  Offset custom_content_offset = kInvalidOffset;
};

// What a reference field points at.  TypeIndexLEB fields name a type directly
// and carry no symbol.  All other fields name one or more linking symbols.
struct ReferenceTarget {
  enum class Kind {
    Type,
    Function,
    Global,
    Table,
    Data,
    Section,
  };

  Kind kind = Kind::Type;

  // Type: index in the type space.  Function/Global/Table: index in the
  // corresponding index space.  Section: physical section index.  Unused for
  // Data targets, which are identified through their symbol identity.
  Index index = kInvalidIndex;

  // Linking symbol table index.  kInvalidIndex for direct type references.
  Index symbol_index = kInvalidIndex;

  // True when the target symbol is undefined and its identity was recovered
  // from the import space.
  bool imported = false;
  std::string import_module;
  std::string import_field;

  // Defined data symbol location.  defined is false for an undefined external
  // data symbol, so a zero segment/offset is never mistaken for a definition.
  bool defined = false;
  Index data_segment = kInvalidIndex;
  uint32_t data_offset = 0;
  uint32_t data_size = 0;
};

// A single relocation entry, resolved against the actual binary layout.
struct ReferenceRecord {
  // Physical section index of the reloc.* custom section holding the entry,
  // and the ordinal of the entry within that section.
  Index reloc_section_index = kInvalidIndex;
  Index reloc_entry_index = kInvalidIndex;

  // Physical section index of the section the relocation applies to.
  Index target_section_index = kInvalidIndex;

  // Offset of the field relative to the target section payload origin.  For
  // custom sections the payload includes the name length and name bytes.
  Offset section_offset = kInvalidOffset;

  // Inclusive start and exclusive end file offsets of the field bytes.
  Offset file_begin = kInvalidOffset;
  Offset file_end = kInvalidOffset;

  // Original field encoding, copied out of the input buffer.
  std::vector<uint8_t> encoding;

  // Relocation type (always one of the supported subset) and its signed
  // addend (zero when the entry carries no addend).
  RelocType type = RelocType::FuncIndexLEB;
  int32_t addend = 0;

  ReferenceSource source;

  // Resolved target identities.  Usually a single entry; a data field can be
  // fully covered by several alias data symbols, in which case every matching
  // symbol is retained, in symbol table order.
  std::vector<ReferenceTarget> targets;
};

// A symbol table entry, copied into the map so it outlives the input.
struct ReferenceSymbol {
  Index symbol_index = kInvalidIndex;
  SymbolType symbol_type = SymbolType::Function;
  uint32_t flags = 0;
  std::string name;
  bool defined = false;

  // Function/Global/Table: index in the kind's index space.  Section: physical
  // section index.
  Index item_index = kInvalidIndex;

  // Defined data symbol location.
  Index data_segment = kInvalidIndex;
  uint32_t data_offset = 0;
  uint32_t data_size = 0;

  // Import identity for an undefined function/global/table symbol.
  std::string import_module;
  std::string import_field;
};

// Physical layout of one section, custom sections included.  Sections with
// duplicate names are kept as separate entries.
struct ReferenceSection {
  Index section_index = kInvalidIndex;
  BinarySection section_type = BinarySection::Custom;
  std::string name;  // Custom section name, empty for known sections.
  Offset payload_begin = kInvalidOffset;
  Offset payload_end = kInvalidOffset;
};

struct BinaryReferenceMap {
  // All records, in the order in which their relocation entries were read.
  std::vector<ReferenceRecord> records;

  // Symbol table entries, indexed by their original symbol table index.
  std::vector<ReferenceSymbol> symbols;

  // Every physical section, in file order (custom sections included).
  std::vector<ReferenceSection> sections;

  // Symbol index -> records that reference the symbol (incoming) and records
  // whose source is the symbol's own definition (outgoing).  The record itself
  // is stored once in |records|; the indexes only hold positions.
  std::vector<std::vector<size_t>> incoming_references;
  std::vector<std::vector<size_t>> outgoing_references;

  const std::vector<size_t>& IncomingReferences(Index symbol_index) const;
  const std::vector<size_t>& OutgoingReferences(Index symbol_index) const;

  // Look up a physical section by index; returns null when out of range.
  const ReferenceSection* GetSection(Index section_index) const;
};

// Build a reference map for |data|.  On success fills |out_map| and returns
// Result::Ok.  On any validation failure returns Result::Error, appends
// diagnostics to |out_errors| (including the offending relocation table and
// entry position where applicable) and leaves |out_map| untouched, so no
// partial report is published.
Result ReadBinaryReferenceMap(ByteSpan data,
                              const ReadBinaryOptions& options,
                              Errors* out_errors,
                              BinaryReferenceMap* out_map);

}  // namespace wabt

#endif /* WABT_BINARY_REFERENCE_MAP_H_ */
