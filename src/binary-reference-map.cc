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

#include "wabt/binary-reference-map.h"

#include <algorithm>
#include <cstdarg>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "wabt/binary-reader-nop.h"
#include "wabt/config.h"
#include "wabt/leb128.h"
#include "wabt/string-format.h"

namespace wabt {

namespace {

std::string Vprintf(const char* format, va_list args) {
  va_list args_copy;
  va_copy(args_copy, args);
  size_t len = wabt_vsnprintf(nullptr, 0, format, args_copy) + 1;
  va_end(args_copy);
  std::vector<char> buffer(len);
  wabt_vsnprintf(buffer.data(), len, format, args);
  return std::string(buffer.data(), len - 1);
}

// One raw relocation entry, remembered until EndModule where all sections and
// the symbol table are available for cross-validation and resolution.
struct RawReloc {
  Index reloc_section_index;
  Index reloc_entry_index;
  Index target_section_index;
  RelocType type;
  Offset section_offset;
  Index index;
  int32_t addend;
};

// Exact byte span of one instruction immediate, observed while the regular
// binary reader consumes that instruction.
struct FieldEvent {
  Offset instruction_begin;
  Offset field_begin;
  Offset field_end;
  Opcode opcode;
  unsigned field_index;
};

struct FuncBodyInfo {
  Index function_index;
  Offset body_begin;  // First byte after the body size field.
  Offset body_end;
  Offset size_field_begin;
  Offset size_field_end;
  Offset locals_end;  // End of the local declaration area.
  std::vector<FieldEvent> fields;
};

struct DataSegmentInfo {
  Index segment_index;
  Offset content_begin;
  Offset content_end;
};

struct CustomInfo {
  Index section_index;
  Offset payload_begin;
  Offset content_begin;  // After the section name.
  Offset content_end;
};

struct ImportIdentity {
  std::string module;
  std::string field;
};

struct Interval {
  Offset begin;
  Offset end;
  Index reloc_section;
  Index reloc_entry;
};

bool IsCodeReloc(RelocType type) {
  return type == RelocType::FuncIndexLEB ||
         type == RelocType::TypeIndexLEB ||
         type == RelocType::GlobalIndexLEB ||
         type == RelocType::TableNumberLEB ||
         type == RelocType::MemoryAddressSLEB;
}

bool IsSupportedReloc(RelocType type) {
  return IsCodeReloc(type) || type == RelocType::MemoryAddressI32 ||
         type == RelocType::FunctionOffsetI32 ||
         type == RelocType::SectionOffsetI32;
}

class ReferenceMapReader : public BinaryReaderNop {
 public:
  ReferenceMapReader(ByteSpan data, Errors* errors)
      : data_(data), errors_(errors) {}

  bool OnError(const Error& error) override {
    if (error.error_level == ErrorLevel::Error) {
      failed_ = true;
      errors_->push_back(error);
    }
    // Handled: the default printer must not duplicate the diagnostic.
    return true;
  }

  Result BeginSection(Index section_index,
                      BinarySection section_type,
                      Offset size) override {
    ReferenceSection section;
    section.section_index = section_index;
    section.section_type = section_type;
    section.payload_begin = state->offset;
    section.payload_end = state->offset + size;
    sections_.push_back(std::move(section));
    current_payload_begin_ = state->offset;
    current_payload_end_ = state->offset + size;
    return Result::Ok;
  }

  Result BeginCustomSection(Index section_index,
                            Offset size,
                            std::string_view section_name) override {
    sections_.back().name = std::string(section_name);
    // The payload starts at the name length; content begins right after the
    // name bytes.
    custom_content_begin_ = state->offset;
    return Result::Ok;
  }

  Result EndCustomSection() override {
    CustomInfo info;
    info.section_index = sections_.back().section_index;
    info.payload_begin = current_payload_begin_;
    info.content_begin = custom_content_begin_;
    info.content_end = current_payload_end_;
    custom_sections_.push_back(std::move(info));
    return Result::Ok;
  }

  Result OnTypeCount(Index count) override {
    type_count_ = count;
    return Result::Ok;
  }

  Result OnImportFunc(Index import_index,
                      std::string_view module_name,
                      std::string_view field_name,
                      Index func_index,
                      Index sig_index) override {
    func_imports_.resize(std::max<size_t>(func_imports_.size(),
                                          size_t(func_index) + 1));
    func_imports_[func_index] = {std::string(module_name),
                                 std::string(field_name)};
    return Result::Ok;
  }

  Result OnImportGlobal(Index import_index,
                        std::string_view module_name,
                        std::string_view field_name,
                        Index global_index,
                        Type type,
                        bool mutable_) override {
    global_imports_.resize(std::max<size_t>(global_imports_.size(),
                                            size_t(global_index) + 1));
    global_imports_[global_index] = {std::string(module_name),
                                     std::string(field_name)};
    return Result::Ok;
  }

  Result OnImportTable(Index import_index,
                       std::string_view module_name,
                       std::string_view field_name,
                       Index table_index,
                       Type elem_type,
                       const Limits* elem_limits) override {
    table_imports_.resize(std::max<size_t>(table_imports_.size(),
                                           size_t(table_index) + 1));
    table_imports_[table_index] = {std::string(module_name),
                                   std::string(field_name)};
    return Result::Ok;
  }

  Result OnFunction(Index index, Index sig_index) override {
    num_defined_functions_++;
    return Result::Ok;
  }

  Result BeginTable(Index index,
                    Type elem_type,
                    const Limits* elem_limits,
                    TableInitExprStatus init_provided) override {
    if (index >= table_imports_.size()) {
      num_defined_tables_++;
    }
    return Result::Ok;
  }

  Result BeginGlobal(Index index, Type type, bool mutable_) override {
    if (index >= global_imports_.size()) {
      num_defined_globals_++;
    }
    return Result::Ok;
  }

  Result BeginFunctionBody(Index index, Offset size) override {
    in_function_body_ = true;
    current_body_ = FuncBodyInfo();
    current_body_->function_index = index;
    current_body_->body_begin = state->offset;
    current_body_->locals_end = state->offset;
    // OnFunctionBodySizeRange fires just before BeginFunctionBody.
    current_body_->size_field_begin = pending_size_begin_;
    current_body_->size_field_end = pending_size_end_;
    return Result::Ok;
  }

  Result OnFunctionBodySizeRange(Index function_index,
                                 Offset begin,
                                 Offset end) override {
    pending_size_begin_ = begin;
    pending_size_end_ = end;
    return Result::Ok;
  }

  Result OnLocalDeclCount(Index count) override {
    if (in_function_body_) {
      current_body_->locals_end = state->offset;
    }
    return Result::Ok;
  }

  Result OnLocalDecl(Index decl_index, Index count, Type type) override {
    if (in_function_body_) {
      current_body_->locals_end = state->offset;
    }
    return Result::Ok;
  }

  Result EndLocalDecls() override {
    if (in_function_body_) {
      current_body_->locals_end = state->offset;
    }
    return Result::Ok;
  }

  Result OnOpcode(Opcode opcode) override {
    if (in_function_body_) {
      current_instruction_begin_ = state->offset - opcode.GetLength();
    }
    return Result::Ok;
  }

  Result OnOpcodeImmediateRange(Opcode opcode,
                                unsigned field_index,
                                Offset begin,
                                Offset end) override {
    if (in_function_body_ && current_body_) {
      current_body_->fields.push_back(
          {current_instruction_begin_, begin, end, opcode, field_index});
    }
    return Result::Ok;
  }

  Result EndFunctionBody(Index index) override {
    if (current_body_) {
      current_body_->body_end = state->offset;
      bodies_.push_back(std::move(*current_body_));
      current_body_.reset();
    }
    in_function_body_ = false;
    pending_size_begin_ = kInvalidOffset;
    pending_size_end_ = kInvalidOffset;
    return Result::Ok;
  }

  Result OnDataSegmentData(Index index, ByteSpan data) override {
    DataSegmentInfo info;
    info.segment_index = index;
    info.content_end = state->offset;
    info.content_begin = state->offset - data.size();
    data_segments_.push_back(std::move(info));
    return Result::Ok;
  }

  Result OnSymbolCount(Index count) override {
    symbols_.resize(count);
    symbol_seen_.assign(count, false);
    return Result::Ok;
  }

  Result OnDataSymbol(Index index,
                      uint32_t flags,
                      std::string_view name,
                      Index segment,
                      uint32_t offset,
                      uint32_t size) override {
    if (index >= symbols_.size()) {
      return Fail("symbol table entry %" PRIindex
                  " is outside the declared symbol count",
                  index);
    }
    ReferenceSymbol& sym = symbols_[index];
    sym.symbol_index = index;
    sym.symbol_type = SymbolType::Data;
    sym.flags = flags;
    sym.name = std::string(name);
    sym.defined = (flags & WABT_SYMBOL_FLAG_UNDEFINED) == 0;
    if (sym.defined) {
      sym.data_segment = segment;
      sym.data_offset = offset;
      sym.data_size = size;
    }
    symbol_seen_[index] = true;
    return Result::Ok;
  }

  Result OnFunctionSymbol(Index index,
                          uint32_t flags,
                          std::string_view name,
                          Index function_index) override {
    return StoreIndexSymbol(index, SymbolType::Function, flags, name,
                            function_index);
  }

  Result OnGlobalSymbol(Index index,
                        uint32_t flags,
                        std::string_view name,
                        Index global_index) override {
    return StoreIndexSymbol(index, SymbolType::Global, flags, name,
                            global_index);
  }

  Result OnTableSymbol(Index index,
                       uint32_t flags,
                       std::string_view name,
                       Index table_index) override {
    return StoreIndexSymbol(index, SymbolType::Table, flags, name,
                            table_index);
  }

  Result OnSectionSymbol(Index index,
                         uint32_t flags,
                         Index section_index) override {
    if (index >= symbols_.size()) {
      return Fail("symbol table entry %" PRIindex
                  " is outside the declared symbol count",
                  index);
    }
    ReferenceSymbol& sym = symbols_[index];
    sym.symbol_index = index;
    sym.symbol_type = SymbolType::Section;
    sym.flags = flags;
    sym.defined = true;
    sym.item_index = section_index;
    symbol_seen_[index] = true;
    return Result::Ok;
  }

  Result OnRelocCount(Index count, Index section_index) override {
    Index reloc_section = sections_.back().section_index;
    if (section_index >= sections_.size()) {
      return Fail(
          "relocation table in section %" PRIindex
          " targets section %" PRIindex " which does not exist",
          reloc_section, section_index);
    }
    if (section_index >= reloc_section) {
      return Fail(
          "relocation table in section %" PRIindex
          " targets section %" PRIindex
          " which does not precede the relocation section",
          reloc_section, section_index);
    }
    current_reloc_target_ = section_index;
    current_reloc_entries_ = 0;
    return Result::Ok;
  }

  Result OnReloc(RelocType type,
                 Offset offset,
                 Index index,
                 uint32_t addend) override {
    Index reloc_section = sections_.back().section_index;
    Index entry = current_reloc_entries_++;
    if (!IsSupportedReloc(type)) {
      return Fail(
          "relocation table in section %" PRIindex " entry %" PRIindex
          ": unsupported relocation type %s",
          reloc_section, entry, GetRelocTypeName(type));
    }
    RawReloc raw;
    raw.reloc_section_index = reloc_section;
    raw.reloc_entry_index = entry;
    raw.target_section_index = current_reloc_target_;
    raw.type = type;
    raw.section_offset = offset;
    raw.index = index;
    raw.addend = static_cast<int32_t>(addend);
    relocs_.push_back(std::move(raw));
    return Result::Ok;
  }

  Result EndModule() override {
    if (failed_) {
      return Result::Error;
    }
    return BuildMap();
  }

  bool failed() const { return failed_; }

  BinaryReferenceMap TakeMap() { return std::move(map_); }

 private:
  Result Fail(const char* format, ...)
      WABT_PRINTF_FORMAT(2, 3) {
    va_list args;
    va_start(args, format);
    std::string message = Vprintf(format, args);
    va_end(args);
    errors_->emplace_back(ErrorLevel::Error, Location(state ? state->offset : 0),
                          std::string_view(), message);
    failed_ = true;
    return Result::Error;
  }

  Result FailReloc(const RawReloc& reloc, const char* format, ...)
      WABT_PRINTF_FORMAT(3, 4) {
    std::string prefix = StringPrintf(
        "relocation table in section %" PRIindex " entry %" PRIindex ": ",
        reloc.reloc_section_index, reloc.reloc_entry_index);
    va_list args;
    va_start(args, format);
    std::string detail = Vprintf(format, args);
    va_end(args);
    errors_->emplace_back(ErrorLevel::Error,
                          Location(reloc.section_offset),
                          std::string_view(), prefix + detail);
    failed_ = true;
    return Result::Error;
  }

  Result StoreIndexSymbol(Index index,
                          SymbolType kind,
                          uint32_t flags,
                          std::string_view name,
                          Index item_index) {
    if (index >= symbols_.size()) {
      return Fail("symbol table entry %" PRIindex
                  " is outside the declared symbol count",
                  index);
    }
    ReferenceSymbol& sym = symbols_[index];
    sym.symbol_index = index;
    sym.symbol_type = kind;
    sym.flags = flags;
    sym.name = std::string(name);
    sym.defined = (flags & WABT_SYMBOL_FLAG_UNDEFINED) == 0;
    sym.item_index = item_index;
    symbol_seen_[index] = true;
    return Result::Ok;
  }

  const ReferenceSection* SectionByIndex(Index index) const {
    for (const ReferenceSection& section : map_.sections) {
      if (section.section_index == index) {
        return &section;
      }
    }
    return nullptr;
  }

  const ImportIdentity* LookupImport(SymbolType kind,
                                     Index item_index) const {
    const std::vector<ImportIdentity>* imports = nullptr;
    switch (kind) {
      case SymbolType::Function:
        imports = &func_imports_;
        break;
      case SymbolType::Global:
        imports = &global_imports_;
        break;
      case SymbolType::Table:
        imports = &table_imports_;
        break;
      default:
        return nullptr;
    }
    if (item_index >= imports->size()) {
      return nullptr;
    }
    return &(*imports)[item_index];
  }

  // Verify the defined/undefined index carried by a function/global/table
  // symbol points at the right kind of index space entry.
  Result ValidateIndexSymbol(const RawReloc& reloc,
                             const ReferenceSymbol& sym) {
    size_t import_count = 0;
    size_t defined_count = 0;
    const char* kind_name = GetSymbolTypeName(sym.symbol_type);
    switch (sym.symbol_type) {
      case SymbolType::Function:
        import_count = func_imports_.size();
        defined_count = num_defined_functions_;
        break;
      case SymbolType::Global:
        import_count = global_imports_.size();
        defined_count = num_defined_globals_;
        break;
      case SymbolType::Table:
        import_count = table_imports_.size();
        defined_count = num_defined_tables_;
        break;
      default:
        return Result::Ok;
    }
    size_t total = import_count + defined_count;
    if (sym.item_index >= total) {
      return FailReloc(reloc,
                       "symbol %" PRIindex " (%s) has out of range %s index %"
                       PRIindex " (space size %zu)",
                       sym.symbol_index, sym.name.c_str(), kind_name,
                       sym.item_index, total);
    }
    if (!sym.defined) {
      if (sym.item_index >= import_count) {
        return FailReloc(reloc,
                         "undefined symbol %" PRIindex
                         " (%s) points at defined %s index %" PRIindex,
                         sym.symbol_index, sym.name.c_str(), kind_name,
                         sym.item_index);
      }
      if (!LookupImport(sym.symbol_type, sym.item_index)) {
        return FailReloc(reloc,
                         "undefined symbol %" PRIindex
                         " (%s) has no matching import at %s index %" PRIindex,
                         sym.symbol_index, sym.name.c_str(), kind_name,
                         sym.item_index);
      }
    } else if (sym.item_index < import_count) {
      return FailReloc(reloc,
                       "defined symbol %" PRIindex
                       " (%s) points at imported %s index %" PRIindex,
                       sym.symbol_index, sym.name.c_str(), kind_name,
                       sym.item_index);
    }
    return Result::Ok;
  }

  Result CheckOverlap(const RawReloc& reloc,
                      Index target_section,
                      Offset begin,
                      Offset end) {
    std::vector<Interval>& intervals = section_intervals_[target_section];
    for (const Interval& existing : intervals) {
      if (begin < existing.end && existing.begin < end) {
        return FailReloc(
            reloc,
            "field [%#" PRIzx ",%#" PRIzx
            ") duplicates or overlaps the field already claimed by "
            "relocation table section %" PRIindex " entry %" PRIindex,
            begin, end, existing.reloc_section, existing.reloc_entry);
      }
    }
    intervals.push_back({begin, end, reloc.reloc_section_index,
                         reloc.reloc_entry_index});
    return Result::Ok;
  }

  bool OpcodeMatchesField(RelocType type,
                          Opcode opcode,
                          unsigned field_index) const {
    switch (type) {
      case RelocType::FuncIndexLEB:
        return (opcode == Opcode::Call || opcode == Opcode::ReturnCall ||
                opcode == Opcode::RefFunc) &&
               field_index == 0;
      case RelocType::TypeIndexLEB:
        return (opcode == Opcode::Block || opcode == Opcode::Loop ||
                opcode == Opcode::If || opcode == Opcode::CallIndirect ||
                opcode == Opcode::ReturnCallIndirect) &&
               field_index == 0;
      case RelocType::GlobalIndexLEB:
        return (opcode == Opcode::GlobalGet ||
                opcode == Opcode::GlobalSet) &&
               field_index == 0;
      case RelocType::TableNumberLEB:
        if (field_index == 1 &&
            (opcode == Opcode::CallIndirect ||
             opcode == Opcode::ReturnCallIndirect)) {
          return true;
        }
        return field_index == 0 &&
               (opcode == Opcode::TableGet || opcode == Opcode::TableSet ||
                opcode == Opcode::TableGrow || opcode == Opcode::TableSize ||
                opcode == Opcode::TableFill);
      case RelocType::MemoryAddressSLEB:
        return opcode == Opcode::I32Const && field_index == 0;
      default:
        return false;
    }
  }

  // Locate and validate a code-section field.  The exact byte span comes from
  // the events recorded while the reader parsed the instruction, never from
  // scanning for a numeric value.
  Result ResolveCodeField(const RawReloc& reloc,
                          const ReferenceSection& section,
                          Offset* out_begin,
                          Offset* out_end,
                          FuncBodyInfo** out_body,
                          const FieldEvent** out_event) {
    Offset file_begin = section.payload_begin + reloc.section_offset;
    FuncBodyInfo* body = nullptr;
    for (FuncBodyInfo& candidate : bodies_) {
      if (file_begin >= candidate.size_field_begin &&
          file_begin < candidate.body_end) {
        body = &candidate;
        break;
      }
    }
    if (!body) {
      return FailReloc(reloc,
                       "offset %#" PRIzx
                       " does not land inside a function body",
                       reloc.section_offset);
    }
    if (file_begin >= body->size_field_begin &&
        file_begin < body->size_field_end) {
      return FailReloc(reloc,
                       "offset %#" PRIzx
                       " lands on the function body size field",
                       reloc.section_offset);
    }
    if (file_begin < body->locals_end) {
      return FailReloc(reloc,
                       "offset %#" PRIzx
                       " lands on a local variable declaration",
                       reloc.section_offset);
    }
    const FieldEvent* event = nullptr;
    for (const FieldEvent& candidate : body->fields) {
      if (candidate.field_begin == file_begin) {
        event = &candidate;
        break;
      }
    }
    if (!event) {
      for (const FieldEvent& candidate : body->fields) {
        if (file_begin == candidate.instruction_begin) {
          return FailReloc(reloc,
                           "offset %#" PRIzx " lands on an opcode byte",
                           reloc.section_offset);
        }
        if (file_begin > candidate.field_begin &&
            file_begin < candidate.field_end) {
          return FailReloc(reloc,
                           "offset %#" PRIzx
                           " lands in the middle of an instruction immediate",
                           reloc.section_offset);
        }
      }
      return FailReloc(reloc,
                       "offset %#" PRIzx
                       " does not land on an instruction immediate",
                       reloc.section_offset);
    }
    if (!OpcodeMatchesField(reloc.type, event->opcode, event->field_index)) {
      return FailReloc(reloc,
                       "field at offset %#" PRIzx
                       " belongs to %s but the relocation type is %s",
                       reloc.section_offset, event->opcode.GetName(),
                       GetRelocTypeName(reloc.type));
    }
    // Re-read the LEB through the project's own decoder to confirm the
    // observed span is a complete encoding inside the body.  Five-byte padded
    // encodings (including signed block type indices) are accepted.
    const uint8_t* ptr = data_.data() + event->field_begin;
    const uint8_t* end = data_.data() + body->body_end;
    uint32_t decoded = 0;
    size_t length = 0;
    bool signed_field = reloc.type == RelocType::MemoryAddressSLEB ||
                        (reloc.type == RelocType::TypeIndexLEB &&
                         (event->opcode == Opcode::Block ||
                          event->opcode == Opcode::Loop ||
                          event->opcode == Opcode::If));
    if (signed_field) {
      length = ReadS32Leb128(ptr, end, &decoded);
    } else {
      length = ReadU32Leb128(ptr, end, &decoded);
    }
    if (length == 0 || event->field_begin + length != event->field_end) {
      return FailReloc(reloc,
                       "field at offset %#" PRIzx
                       " is not a complete LEB128 immediate",
                       reloc.section_offset);
    }
    // A block type relocation can only name a positive type section index.
    // Negative encodings are the inline value types (void, i32, ...).
    if (reloc.type == RelocType::TypeIndexLEB &&
        (event->opcode == Opcode::Block ||
         event->opcode == Opcode::Loop || event->opcode == Opcode::If) &&
        static_cast<int32_t>(decoded) < 0) {
      return FailReloc(reloc,
                       "block type at offset %#" PRIzx
                       " is an inline value type, not a type section index",
                       reloc.section_offset);
    }
    *out_begin = event->field_begin;
    *out_end = event->field_end;
    *out_body = body;
    *out_event = event;
    return Result::Ok;
  }

  Result ResolveFixedField(const RawReloc& reloc,
                           const ReferenceSection& section,
                           ReferenceSource::Kind expected_kind,
                           Offset* out_begin,
                           Offset* out_end,
                           ReferenceSource* out_source) {
    Offset file_begin = section.payload_begin + reloc.section_offset;
    constexpr Offset kFieldWidth = 4;
    if (expected_kind == ReferenceSource::Kind::DataSegment) {
      const DataSegmentInfo* segment_info = nullptr;
      for (const DataSegmentInfo& candidate : data_segments_) {
        if (file_begin >= candidate.content_begin &&
            file_begin < candidate.content_end) {
          segment_info = &candidate;
          break;
        }
      }
      if (!segment_info) {
        return FailReloc(reloc,
                         "offset %#" PRIzx
                         " is not inside data segment contents",
                         reloc.section_offset);
      }
      Offset file_end = file_begin + kFieldWidth;
      if (file_end > segment_info->content_end) {
        return FailReloc(reloc,
                         "four-byte field at offset %#" PRIzx
                         " runs past the end of data segment %" PRIindex,
                         reloc.section_offset, segment_info->segment_index);
      }
      out_source->kind = ReferenceSource::Kind::DataSegment;
      out_source->data_segment = segment_info->segment_index;
      out_source->data_segment_offset =
          file_begin - segment_info->content_begin;
      *out_begin = file_begin;
      *out_end = file_end;
      return Result::Ok;
    }

    const CustomInfo* custom = nullptr;
    for (const CustomInfo& candidate : custom_sections_) {
      if (candidate.section_index == section.section_index) {
        custom = &candidate;
        break;
      }
    }
    if (!custom) {
      return FailReloc(reloc, "target section %" PRIindex " is not custom",
                       section.section_index);
    }
    if (file_begin < custom->content_begin) {
      return FailReloc(reloc,
                       "offset %#" PRIzx
                       " lands on the custom section name",
                       reloc.section_offset);
    }
    Offset file_end = file_begin + kFieldWidth;
    if (file_end > custom->content_end) {
      return FailReloc(reloc,
                       "four-byte field at offset %#" PRIzx
                       " runs past the end of the custom section contents",
                       reloc.section_offset);
    }
    out_source->kind = ReferenceSource::Kind::CustomContent;
    out_source->custom_section_index = custom->section_index;
    out_source->custom_content_offset = file_begin - custom->content_begin;
    *out_begin = file_begin;
    *out_end = file_end;
    return Result::Ok;
  }

  Result CheckSymbolKind(const RawReloc& reloc,
                         const ReferenceSymbol& sym) {
    auto expect = [&](SymbolType expected) -> Result {
      if (sym.symbol_type != expected) {
        return FailReloc(reloc,
                         "relocation %s requires a %s symbol but symbol %"
                         PRIindex " is %s",
                         GetRelocTypeName(reloc.type),
                         GetSymbolTypeName(expected), sym.symbol_index,
                         GetSymbolTypeName(sym.symbol_type));
      }
      return Result::Ok;
    };
    switch (reloc.type) {
      case RelocType::FuncIndexLEB:
        return expect(SymbolType::Function);
      case RelocType::GlobalIndexLEB:
        return expect(SymbolType::Global);
      case RelocType::TableNumberLEB:
        return expect(SymbolType::Table);
      case RelocType::MemoryAddressSLEB:
      case RelocType::MemoryAddressI32:
        if (sym.symbol_type != SymbolType::Data &&
            sym.symbol_type != SymbolType::Section) {
          return FailReloc(reloc,
                           "relocation %s requires a data or section symbol "
                           "but symbol %" PRIindex " is %s",
                           GetRelocTypeName(reloc.type), sym.symbol_index,
                           GetSymbolTypeName(sym.symbol_type));
        }
        return Result::Ok;
      case RelocType::FunctionOffsetI32:
        if (sym.symbol_type != SymbolType::Function &&
            sym.symbol_type != SymbolType::Section) {
          return FailReloc(reloc,
                           "relocation %s requires a function or section "
                           "symbol but symbol %" PRIindex " is %s",
                           GetRelocTypeName(reloc.type), sym.symbol_index,
                           GetSymbolTypeName(sym.symbol_type));
        }
        return Result::Ok;
      case RelocType::SectionOffsetI32:
        return expect(SymbolType::Section);
      default:
        return Result::Ok;
    }
  }

  ReferenceTarget MakeTarget(const ReferenceSymbol& sym) {
    ReferenceTarget target;
    target.symbol_index = sym.symbol_index;
    switch (sym.symbol_type) {
      case SymbolType::Function:
        target.kind = ReferenceTarget::Kind::Function;
        target.index = sym.item_index;
        break;
      case SymbolType::Global:
        target.kind = ReferenceTarget::Kind::Global;
        target.index = sym.item_index;
        break;
      case SymbolType::Table:
        target.kind = ReferenceTarget::Kind::Table;
        target.index = sym.item_index;
        break;
      case SymbolType::Section:
        target.kind = ReferenceTarget::Kind::Section;
        target.index = sym.item_index;
        break;
      case SymbolType::Data:
        target.kind = ReferenceTarget::Kind::Data;
        if (sym.defined) {
          target.defined = true;
          target.data_segment = sym.data_segment;
          target.data_offset = sym.data_offset;
          target.data_size = sym.data_size;
        }
        break;
      case SymbolType::Tag:
        // No supported relocation targets tags.
        break;
    }
    if (!sym.defined &&
        (sym.symbol_type == SymbolType::Function ||
         sym.symbol_type == SymbolType::Global ||
         sym.symbol_type == SymbolType::Table)) {
      if (const ImportIdentity* import =
              LookupImport(sym.symbol_type, sym.item_index)) {
        target.imported = true;
        target.import_module = import->module;
        target.import_field = import->field;
      }
    }
    return target;
  }

  // Every defined, non-ABS data symbol whose range fully covers the field is
  // an alias source and must be retained, not just the first or named one.
  std::vector<Index> CoveringDataSymbols(const DataSegmentInfo& segment_info,
                                         Offset field_begin,
                                         Offset field_end) {
    std::vector<Index> result;
    Offset field_offset = field_begin - segment_info.content_begin;
    Offset field_length = field_end - field_begin;
    for (const ReferenceSymbol& sym : map_.symbols) {
      if (sym.symbol_type != SymbolType::Data || !sym.defined ||
          (sym.flags & WABT_SYMBOL_FLAG_ABS) != 0) {
        continue;
      }
      if (sym.data_segment != segment_info.segment_index) {
        continue;
      }
      uint64_t sym_end = uint64_t(sym.data_offset) + sym.data_size;
      if (sym.data_offset <= field_offset &&
          sym_end >= field_offset + field_length) {
        result.push_back(sym.symbol_index);
      }
    }
    return result;
  }

  Result ResolveTargets(const RawReloc& reloc,
                        const ReferenceSource& source,
                        Offset field_begin,
                        Offset field_end,
                        ReferenceRecord* record) {
    if (reloc.type == RelocType::TypeIndexLEB) {
      // Type indices address the type space directly; the symbol table is not
      // consulted.
      if (reloc.index >= type_count_) {
        return FailReloc(reloc,
                         "type index %" PRIindex
                         " is out of range (type count %" PRIindex ")",
                         reloc.index, type_count_);
      }
      ReferenceTarget target;
      target.kind = ReferenceTarget::Kind::Type;
      target.index = reloc.index;
      record->targets.push_back(std::move(target));
      return Result::Ok;
    }

    if (reloc.index >= map_.symbols.size()) {
      return FailReloc(reloc,
                       "symbol index %" PRIindex
                       " is out of range (symbol count %zu)",
                       reloc.index, map_.symbols.size());
    }
    const ReferenceSymbol& named = map_.symbols[reloc.index];
    CHECK_RESULT(CheckSymbolKind(reloc, named));
    if (named.symbol_type == SymbolType::Section) {
      if (named.item_index >= map_.sections.size()) {
        return FailReloc(reloc,
                         "section symbol %" PRIindex
                         " points at missing physical section %" PRIindex,
                         named.symbol_index, named.item_index);
      }
    } else if (named.symbol_type != SymbolType::Data) {
      CHECK_RESULT(ValidateIndexSymbol(reloc, named));
    } else if (named.defined &&
               (named.flags & WABT_SYMBOL_FLAG_ABS) == 0) {
      bool segment_exists = false;
      for (const DataSegmentInfo& info : data_segments_) {
        if (info.segment_index == named.data_segment) {
          segment_exists = true;
          uint64_t sym_end = uint64_t(named.data_offset) + named.data_size;
          if (sym_end > info.content_end - info.content_begin) {
            return FailReloc(reloc,
                             "data symbol %" PRIindex
                             " (" PRIstringview
                             ") extends past its data segment",
                             named.symbol_index,
                             WABT_PRINTF_STRING_VIEW_ARG(named.name));
          }
          break;
        }
      }
      if (!segment_exists) {
        return FailReloc(reloc,
                         "data symbol %" PRIindex
                         " (" PRIstringview
                         ") references missing data segment %" PRIindex,
                         named.symbol_index,
                         WABT_PRINTF_STRING_VIEW_ARG(named.name),
                         named.data_segment);
      }
    }

    std::vector<Index> symbol_indices;
    symbol_indices.push_back(named.symbol_index);
    if (source.kind == ReferenceSource::Kind::DataSegment) {
      const DataSegmentInfo* segment_info = nullptr;
      for (const DataSegmentInfo& candidate : data_segments_) {
        if (candidate.segment_index == source.data_segment) {
          segment_info = &candidate;
          break;
        }
      }
      if (segment_info) {
        for (Index alias :
             CoveringDataSymbols(*segment_info, field_begin, field_end)) {
          if (std::find(symbol_indices.begin(), symbol_indices.end(), alias) ==
              symbol_indices.end()) {
            symbol_indices.push_back(alias);
          }
        }
      }
    }
    for (Index symbol_index : symbol_indices) {
      record->targets.push_back(MakeTarget(map_.symbols[symbol_index]));
    }
    return Result::Ok;
  }

  // Validate every symbol table entry, including ones no relocation names.
  Result ValidateAllSymbols() {
    size_t func_total = func_imports_.size() + num_defined_functions_;
    size_t global_total = global_imports_.size() + num_defined_globals_;
    size_t table_total = table_imports_.size() + num_defined_tables_;
    for (const ReferenceSymbol& sym : map_.symbols) {
      auto entry_fail = [&](const std::string& detail) {
        errors_->emplace_back(
            ErrorLevel::Error, Location(0), std::string_view(),
            StringPrintf("symbol table entry %" PRIindex ": %s",
                         sym.symbol_index, detail.c_str()));
        failed_ = true;
        return Result::Error;
      };
      switch (sym.symbol_type) {
        case SymbolType::Section:
          if (sym.item_index >= map_.sections.size()) {
            return entry_fail(StringPrintf(
                "section index %" PRIindex " is out of range",
                sym.item_index));
          }
          break;
        case SymbolType::Function:
        case SymbolType::Global:
        case SymbolType::Table: {
          size_t import_count =
              sym.symbol_type == SymbolType::Function ? func_imports_.size()
              : sym.symbol_type == SymbolType::Global
                  ? global_imports_.size()
                  : table_imports_.size();
          size_t total =
              sym.symbol_type == SymbolType::Function ? func_total
              : sym.symbol_type == SymbolType::Global ? global_total
                                                       : table_total;
          if (sym.item_index >= total) {
            return entry_fail(StringPrintf(
                "%s index %" PRIindex " is out of range (size %zu)",
                GetSymbolTypeName(sym.symbol_type), sym.item_index, total));
          }
          if (sym.defined == (sym.item_index < import_count)) {
            return entry_fail(StringPrintf(
                "defined/undefined state does not match the import space at "
                "%s index %" PRIindex,
                GetSymbolTypeName(sym.symbol_type), sym.item_index));
          }
          if (!sym.defined &&
              !LookupImport(sym.symbol_type, sym.item_index)) {
            return entry_fail(StringPrintf(
                "no import provides %s index %" PRIindex,
                GetSymbolTypeName(sym.symbol_type), sym.item_index));
          }
          break;
        }
        case SymbolType::Data:
          if (sym.defined &&
              (sym.flags & WABT_SYMBOL_FLAG_ABS) == 0) {
            const DataSegmentInfo* info = nullptr;
            for (const DataSegmentInfo& candidate : data_segments_) {
              if (candidate.segment_index == sym.data_segment) {
                info = &candidate;
                break;
              }
            }
            if (!info) {
              return entry_fail(StringPrintf(
                  "data segment %" PRIindex " does not exist",
                  sym.data_segment));
            }
            if (uint64_t(sym.data_offset) + sym.data_size >
                info->content_end - info->content_begin) {
              return entry_fail(
                  "data range extends past its data segment");
            }
          }
          break;
        case SymbolType::Tag:
          // Tags are not referenced by any supported relocation; nothing to
          // validate against an index space here.
          break;
      }
    }
    return Result::Ok;
  }

  Result BuildMap() {
    map_.sections = sections_;
    map_.symbols = std::move(symbols_);
    map_.incoming_references.resize(map_.symbols.size());
    map_.outgoing_references.resize(map_.symbols.size());

    for (bool seen : symbol_seen_) {
      if (!seen) {
        return Fail("gaps in the linking symbol table: not every declared "
                    "symbol was defined");
      }
    }

    // Recover import identities for undefined function/global/table symbols
    // from the matching index space.
    for (ReferenceSymbol& sym : map_.symbols) {
      if (sym.defined ||
          (sym.symbol_type != SymbolType::Function &&
           sym.symbol_type != SymbolType::Global &&
           sym.symbol_type != SymbolType::Table)) {
        continue;
      }
      if (const ImportIdentity* import =
              LookupImport(sym.symbol_type, sym.item_index)) {
        sym.import_module = import->module;
        sym.import_field = import->field;
      }
    }

    CHECK_RESULT(ValidateAllSymbols());

    for (const RawReloc& reloc : relocs_) {
      const ReferenceSection* section =
          SectionByIndex(reloc.target_section_index);
      if (!section) {
        return FailReloc(reloc,
                         "target section %" PRIindex " does not exist",
                         reloc.target_section_index);
      }

      // Relocation type / target section matrix.
      ReferenceSource::Kind expected_source;
      if (IsCodeReloc(reloc.type)) {
        if (section->section_type != BinarySection::Code) {
          return FailReloc(reloc,
                           "%s is only valid in the code section",
                           GetRelocTypeName(reloc.type));
        }
        expected_source = ReferenceSource::Kind::FunctionInstruction;
      } else if (reloc.type == RelocType::MemoryAddressI32) {
        if (section->section_type != BinarySection::Data) {
          return FailReloc(reloc,
                           "%s is only valid in data segment contents",
                           GetRelocTypeName(reloc.type));
        }
        expected_source = ReferenceSource::Kind::DataSegment;
      } else {
        if (section->section_type != BinarySection::Custom) {
          return FailReloc(reloc,
                           "%s is only valid in custom section contents",
                           GetRelocTypeName(reloc.type));
        }
        // Only ordinary custom contents are in scope; metadata sections
        // (reloc.*, linking, dylink, name and target_features) carry fields
        // with their own structure and are not relocatable content.
        const std::string& name = section->name;
        bool metadata_custom =
            name.starts_with(WABT_BINARY_SECTION_RELOC) ||
            name == WABT_BINARY_SECTION_LINKING ||
            name == WABT_BINARY_SECTION_DYLINK ||
            name == WABT_BINARY_SECTION_DYLINK0 ||
            name == WABT_BINARY_SECTION_TARGET_FEATURES ||
            name == WABT_BINARY_SECTION_NAME;
        if (metadata_custom) {
          return FailReloc(reloc,
                           "%s is only valid in ordinary custom section "
                           "contents, not in metadata section \"%s\"",
                           GetRelocTypeName(reloc.type), name.c_str());
        }
        expected_source = ReferenceSource::Kind::CustomContent;
      }

      ReferenceRecord record;
      record.reloc_section_index = reloc.reloc_section_index;
      record.reloc_entry_index = reloc.reloc_entry_index;
      record.target_section_index = reloc.target_section_index;
      record.section_offset = reloc.section_offset;
      record.type = reloc.type;
      record.addend = reloc.addend;

      Offset field_begin = 0;
      Offset field_end = 0;
      if (expected_source == ReferenceSource::Kind::FunctionInstruction) {
        FuncBodyInfo* body = nullptr;
        const FieldEvent* event = nullptr;
        CHECK_RESULT(ResolveCodeField(reloc, *section, &field_begin,
                                      &field_end, &body, &event));
        record.source.kind = ReferenceSource::Kind::FunctionInstruction;
        record.source.function_index = body->function_index;
        record.source.instruction_offset = event->instruction_begin;
      } else {
        CHECK_RESULT(ResolveFixedField(reloc, *section, expected_source,
                                       &field_begin, &field_end,
                                       &record.source));
      }

      if (field_begin < section->payload_begin ||
          field_end > section->payload_end) {
        return FailReloc(reloc, "field escapes the target section payload");
      }
      CHECK_RESULT(CheckOverlap(reloc, reloc.target_section_index,
                                field_begin, field_end));

      record.file_begin = field_begin;
      record.file_end = field_end;
      record.encoding.assign(data_.begin() + field_begin,
                             data_.begin() + field_end);
      CHECK_RESULT(
          ResolveTargets(reloc, record.source, field_begin, field_end,
                         &record));

      map_.records.push_back(std::move(record));
    }

    BuildIndexes();
    return failed_ ? Result::Error : Result::Ok;
  }

  void BuildIndexes() {
    for (size_t record_index = 0; record_index < map_.records.size();
         ++record_index) {
      const ReferenceRecord& record = map_.records[record_index];
      for (const ReferenceTarget& target : record.targets) {
        if (target.symbol_index != kInvalidIndex) {
          map_.incoming_references[target.symbol_index].push_back(
              record_index);
        }
      }

      // Outgoing references are indexed by the definition that physically
      // contains the source field.
      for (const ReferenceSymbol& sym : map_.symbols) {
        if (!sym.defined) {
          continue;
        }
        bool contains = false;
        if (sym.symbol_type == SymbolType::Function &&
            record.source.kind ==
                ReferenceSource::Kind::FunctionInstruction &&
            sym.item_index == record.source.function_index) {
          contains = true;
        } else if (sym.symbol_type == SymbolType::Section &&
                   sym.item_index == record.target_section_index) {
          contains = true;
        } else if (sym.symbol_type == SymbolType::Data &&
                   record.source.kind ==
                       ReferenceSource::Kind::DataSegment &&
                   (sym.flags & WABT_SYMBOL_FLAG_ABS) == 0 &&
                   sym.data_segment == record.source.data_segment) {
          uint64_t field_offset = record.source.data_segment_offset;
          uint64_t field_end = field_offset +
                               (record.file_end - record.file_begin);
          contains = sym.data_offset <= field_offset &&
                     uint64_t(sym.data_offset) + sym.data_size >= field_end;
        }
        if (contains) {
          auto& outgoing = map_.outgoing_references[sym.symbol_index];
          if (std::find(outgoing.begin(), outgoing.end(), record_index) ==
              outgoing.end()) {
            outgoing.push_back(record_index);
          }
        }
      }
    }
  }

  ByteSpan data_;
  Errors* errors_;
  bool failed_ = false;

  std::vector<ReferenceSection> sections_;
  Offset current_payload_begin_ = 0;
  Offset current_payload_end_ = 0;
  Offset custom_content_begin_ = 0;

  Index type_count_ = 0;
  std::vector<ImportIdentity> func_imports_;
  std::vector<ImportIdentity> global_imports_;
  std::vector<ImportIdentity> table_imports_;
  size_t num_defined_functions_ = 0;
  size_t num_defined_globals_ = 0;
  size_t num_defined_tables_ = 0;

  bool in_function_body_ = false;
  std::optional<FuncBodyInfo> current_body_;
  Offset pending_size_begin_ = kInvalidOffset;
  Offset pending_size_end_ = kInvalidOffset;
  Offset current_instruction_begin_ = 0;
  std::vector<FuncBodyInfo> bodies_;

  std::vector<DataSegmentInfo> data_segments_;
  std::vector<CustomInfo> custom_sections_;

  std::vector<ReferenceSymbol> symbols_;
  std::vector<bool> symbol_seen_;

  Index current_reloc_target_ = kInvalidIndex;
  Index current_reloc_entries_ = 0;
  std::vector<RawReloc> relocs_;

  std::map<Index, std::vector<Interval>> section_intervals_;

  BinaryReferenceMap map_;
};

}  // namespace

const std::vector<size_t>& BinaryReferenceMap::IncomingReferences(
    Index symbol_index) const {
  static const std::vector<size_t> empty;
  if (symbol_index >= incoming_references.size()) {
    return empty;
  }
  return incoming_references[symbol_index];
}

const std::vector<size_t>& BinaryReferenceMap::OutgoingReferences(
    Index symbol_index) const {
  static const std::vector<size_t> empty;
  if (symbol_index >= outgoing_references.size()) {
    return empty;
  }
  return outgoing_references[symbol_index];
}

const ReferenceSection* BinaryReferenceMap::GetSection(
    Index section_index) const {
  for (const ReferenceSection& section : sections) {
    if (section.section_index == section_index) {
      return &section;
    }
  }
  return nullptr;
}

Result ReadBinaryReferenceMap(ByteSpan data,
                              const ReadBinaryOptions& options,
                              Errors* out_errors,
                              BinaryReferenceMap* out_map) {
  assert(out_errors);
  assert(out_map);

  ReferenceMapReader reader(data, out_errors);
  ReadBinaryOptions strict_options = options;
  // A reference map needs a complete, strict parse of every section and
  // function body; relaxed/partial reads cannot establish exact field spans.
  strict_options.stop_on_first_error = true;
  strict_options.fail_on_custom_section_error = true;
  strict_options.skip_function_bodies = false;

  Result result = ReadBinary(data, &reader, strict_options);
  if (Failed(result) || reader.failed()) {
    return Result::Error;
  }
  *out_map = reader.TakeMap();
  return Result::Ok;
}

}  // namespace wabt
