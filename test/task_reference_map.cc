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

// Task tests for the read-only binary reference map.  Every object file below
// is assembled byte by byte in this file, and the checked offsets are computed
// while assembling rather than discovered from the map under test.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "task_support/check.h"
#include "wabt/binary-reader.h"
#include "wabt/binary-reference-map.h"
#include "wabt/feature.h"

namespace {

using wabt::BinaryReferenceMap;
using wabt::ByteSpan;
using wabt::Errors;
using wabt::ReadBinaryOptions;
using wabt::ReferenceRecord;
using wabt::ReferenceSource;
using wabt::ReferenceSymbol;
using wabt::ReferenceTarget;
using wabt::RelocType;
using wabt::Result;
using wabt::SymbolType;

class Builder {
 public:
  void U8(uint8_t v) { data_.push_back(v); }

  void U32(uint32_t v) {
    do {
      uint8_t byte = v & 0x7f;
      v >>= 7;
      if (v) {
        byte |= 0x80;
      }
      data_.push_back(byte);
    } while (v);
  }

  void S32(int32_t v) {
    bool more = true;
    while (more) {
      uint8_t byte = v & 0x7f;
      v >>= 7;
      bool sign_bit = byte & 0x40;
      if ((v == 0 && !sign_bit) || (v == -1 && sign_bit)) {
        more = false;
      } else {
        byte |= 0x80;
      }
      data_.push_back(byte);
    }
  }

  // A deliberately padded, always-five-bytes unsigned LEB128.
  void U32Fixed5(uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      data_.push_back((v & 0x7f) | 0x80);
      v >>= 7;
    }
    data_.push_back(v & 0x0f);
  }

  // A deliberately padded, always-five-bytes signed LEB128.
  void S32Fixed5(int32_t v) {
    for (int i = 0; i < 4; ++i) {
      data_.push_back((v & 0x7f) | 0x80);
      v >>= 7;
    }
    data_.push_back(v & 0x7f);
  }

  void Fixed4(uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      data_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
  }

  void Str(const char* s) {
    size_t len = strlen(s);
    U32(static_cast<uint32_t>(len));
    data_.insert(data_.end(), s, s + len);
  }

  void Bytes(const std::vector<uint8_t>& bytes) {
    data_.insert(data_.end(), bytes.begin(), bytes.end());
  }

  size_t size() const { return data_.size(); }
  const uint8_t* data() const { return data_.data(); }
  std::vector<uint8_t> take() { return std::move(data_); }

 private:
  std::vector<uint8_t> data_;
};

// Symbol table indices used by the fixture.
constexpr wabt::Index kSymImpFunc = 0;
constexpr wabt::Index kSymFunc = 1;
constexpr wabt::Index kSymImpGlobal = 2;
constexpr wabt::Index kSymGlobal = 3;
constexpr wabt::Index kSymImpTable = 4;
constexpr wabt::Index kSymTable = 5;
constexpr wabt::Index kSymDataA = 6;
constexpr wabt::Index kSymDataB = 7;
constexpr wabt::Index kSymSectionCode = 8;
constexpr wabt::Index kSymSectionData = 9;
constexpr wabt::Index kSymSectionX = 10;

// Physical section indices of the fixture (custom sections included).
constexpr wabt::Index kSecType = 0;
constexpr wabt::Index kSecImport = 1;
constexpr wabt::Index kSecFunction = 2;
constexpr wabt::Index kSecTable = 3;
constexpr wabt::Index kSecMemory = 4;
constexpr wabt::Index kSecGlobal = 5;
constexpr wabt::Index kSecCode = 6;
constexpr wabt::Index kSecData = 7;
constexpr wabt::Index kSecX1 = 8;
constexpr wabt::Index kSecX2 = 9;
constexpr wabt::Index kSecRelocCode1 = 10;
constexpr wabt::Index kSecRelocCode2 = 11;
constexpr wabt::Index kSecRelocData = 12;
constexpr wabt::Index kSecRelocX = 13;
constexpr wabt::Index kSecLinking = 14;

// Hand-computed code-section payload offsets (see BuildCodePayload).
constexpr size_t kCodePayloadSize = 40;
constexpr size_t kOffBodySize = 1;
constexpr size_t kOffLocals = 2;
constexpr size_t kOffGlobalGetOpcode = 3;
constexpr size_t kOffGlobalIdx = 4;
constexpr size_t kOffI32ConstOpcode = 9;
constexpr size_t kOffSleb = 10;
constexpr size_t kOffCallOpcode = 15;
constexpr size_t kOffCallIdx = 16;
constexpr size_t kOffBlockOpcode = 21;
constexpr size_t kOffBlockType = 22;
constexpr size_t kOffCallIndirectOpcode = 28;
constexpr size_t kOffCallIndirectType = 29;
constexpr size_t kOffCallIndirectTable = 34;

// Hand-computed data-section payload offsets.
constexpr size_t kOffDataField0 = 6;
constexpr size_t kOffDataField1 = 10;
// Hand-computed payload offsets inside the second "x" custom section.
constexpr size_t kOffXField0 = 2;
constexpr size_t kOffXField1 = 6;

std::vector<uint8_t> BuildCodePayload() {
  Builder b;
  b.U32(1);  // one function body
  b.U32(kCodePayloadSize - 2);  // body size (count byte + size byte excluded)
  b.U32(0);                     // no local declarations
  // global.get 0 (padded 5-byte LEB)
  b.U8(0x23);
  b.U32Fixed5(0);
  // i32.const 0 (padded 5-byte signed LEB)
  b.U8(0x41);
  b.S32Fixed5(0);
  // call 1 (padded 5-byte LEB)
  b.U8(0x10);
  b.U32Fixed5(1);
  // block (type 1) with a padded signed block type index, then end
  b.U8(0x02);
  b.S32Fixed5(1);
  b.U8(0x0b);
  // call_indirect type=1 table=0, both immediates padded
  b.U8(0x11);
  b.U32Fixed5(1);
  b.U32Fixed5(0);
  b.U8(0x0b);  // function end
  return b.take();
}

std::vector<uint8_t> BuildDataPayload() {
  Builder b;
  b.U32(1);       // one segment
  b.U32(0);       // flags: active, memory 0
  b.U8(0x41);     // i32.const
  b.U32(0);
  b.U8(0x0b);     // end
  b.U32(8);       // eight content bytes, two four-byte fields
  b.Fixed4(0);
  b.Fixed4(0);
  return b.take();
}

std::vector<uint8_t> BuildXPayload() {
  Builder b;
  b.Str("x");
  b.Fixed4(0);
  b.Fixed4(0);
  return b.take();
}

void AppendRelocEntry(Builder& b,
                      uint32_t type,
                      uint32_t offset,
                      uint32_t index,
                      bool has_addend,
                      int32_t addend = 0) {
  b.U32(type);
  b.U32(offset);
  b.U32(index);
  if (has_addend) {
    b.S32(addend);
  }
}

std::vector<uint8_t> BuildRelocCode1Payload() {
  Builder b;
  b.U32(kSecCode);
  b.U32(3);
  AppendRelocEntry(b, /*FuncIndexLEB=*/0, kOffCallIdx, kSymFunc, false);
  AppendRelocEntry(b, /*GlobalIndexLEB=*/7, kOffGlobalIdx, kSymImpGlobal,
                   false);
  AppendRelocEntry(b, /*TypeIndexLEB=*/6, kOffCallIndirectType, 1, false);
  return b.take();
}

std::vector<uint8_t> BuildRelocCode2Payload() {
  Builder b;
  b.U32(kSecCode);
  b.U32(3);
  // Deliberately out of order relative to the field offsets, and split across
  // a second table that targets the same code section.
  AppendRelocEntry(b, /*TableNumberLEB=*/20, kOffCallIndirectTable,
                   kSymImpTable, false);
  AppendRelocEntry(b, /*MemoryAddressSLEB=*/4, kOffSleb, kSymDataB, true, 7);
  AppendRelocEntry(b, /*TypeIndexLEB=*/6, kOffBlockType, 1, false);
  return b.take();
}

std::vector<uint8_t> BuildRelocDataPayload() {
  Builder b;
  b.U32(kSecData);
  b.U32(2);
  AppendRelocEntry(b, /*MemoryAddressI32=*/5, kOffDataField0, kSymDataA, true,
                   0);
  AppendRelocEntry(b, /*MemoryAddressI32=*/5, kOffDataField1,
                   kSymSectionData, true, 0);
  return b.take();
}

std::vector<uint8_t> BuildRelocXPayload() {
  Builder b;
  b.U32(kSecX2);
  b.U32(2);
  AppendRelocEntry(b, /*FunctionOffsetI32=*/8, kOffXField0, kSymFunc, true,
                   -4);
  AppendRelocEntry(b, /*SectionOffsetI32=*/9, kOffXField1, kSymSectionX, true,
                   0);
  return b.take();
}

void AppendFunctionSymbol(Builder& b,
                          uint32_t index,
                          uint32_t flags,
                          const char* name,
                          uint32_t func_index) {
  b.U32(0);  // SymbolType::Function
  b.U32(flags);
  b.U32(func_index);
  // Undefined symbols without the explicit-name flag omit the name; their
  // identity is recovered from the import section.
  if ((flags & WABT_SYMBOL_FLAG_UNDEFINED) == 0 ||
      (flags & WABT_SYMBOL_FLAG_EXPLICIT_NAME) != 0) {
    b.Str(name);
  }
}

void AppendGlobalSymbol(Builder& b,
                        uint32_t flags,
                        const char* name,
                        uint32_t global_index) {
  b.U32(2);  // SymbolType::Global
  b.U32(flags);
  b.U32(global_index);
  if ((flags & WABT_SYMBOL_FLAG_UNDEFINED) == 0 ||
      (flags & WABT_SYMBOL_FLAG_EXPLICIT_NAME) != 0) {
    b.Str(name);
  }
}

void AppendTableSymbol(Builder& b,
                       uint32_t flags,
                       const char* name,
                       uint32_t table_index) {
  b.U32(5);  // SymbolType::Table
  b.U32(flags);
  b.U32(table_index);
  if ((flags & WABT_SYMBOL_FLAG_UNDEFINED) == 0 ||
      (flags & WABT_SYMBOL_FLAG_EXPLICIT_NAME) != 0) {
    b.Str(name);
  }
}

void AppendDataSymbol(Builder& b,
                      uint32_t flags,
                      const char* name,
                      uint32_t segment,
                      uint32_t offset,
                      uint32_t size) {
  b.U32(1);  // SymbolType::Data
  b.U32(flags);
  b.Str(name);
  b.U32(segment);
  b.U32(offset);
  b.U32(size);
}

void AppendSectionSymbol(Builder& b, uint32_t flags, uint32_t section_index) {
  b.U32(3);  // SymbolType::Section
  b.U32(flags);
  b.U32(section_index);
}

std::vector<uint8_t> BuildLinkingPayload() {
  Builder symbols;
  AppendFunctionSymbol(symbols, kSymImpFunc,
                       /*undefined=*/0x10, "impf", 0);
  AppendFunctionSymbol(symbols, kSymFunc, /*defined=*/0, "f", 1);
  AppendGlobalSymbol(symbols, 0x10, "impg", 0);
  AppendGlobalSymbol(symbols, 0, "g", 1);
  AppendTableSymbol(symbols, 0x10, "impt", 0);
  AppendTableSymbol(symbols, 0, "t", 1);
  AppendDataSymbol(symbols, 0, "a", 0, 0, 4);
  AppendDataSymbol(symbols, 0, "b", 0, 0, 8);
  AppendSectionSymbol(symbols, 0, kSecCode);
  AppendSectionSymbol(symbols, 0, kSecData);
  AppendSectionSymbol(symbols, 0, kSecX2);

  Builder count;
  count.U32(11);  // count field, part of the subsection body

  Builder b;
  b.U32(2);  // linking metadata version 2
  b.U32(8);  // SymbolTable subsection
  b.U32(static_cast<uint32_t>(count.size() + symbols.size()));
  b.Bytes(count.take());
  b.Bytes(symbols.take());
  return b.take();
}

std::vector<uint8_t> BuildTypePayload() {
  Builder b;
  b.U32(2);
  b.U8(0x60);
  b.U32(1);
  b.U8(0x7f);  // (i32) -> nil
  b.U32(0);
  b.U8(0x60);
  b.U32(0);
  b.U32(0);  // () -> nil
  return b.take();
}

std::vector<uint8_t> BuildImportPayload() {
  Builder b;
  b.U32(3);
  // Interleaved kinds: function, then global, then table.  Same-name symbols
  // ("g" appears as an imported field and a defined global) must not merge.
  b.Str("e");
  b.Str("f");
  b.U8(0);  // ExternalKind::Func
  b.U32(1);
  b.Str("e");
  b.Str("g");
  b.U8(3);  // ExternalKind::Global
  b.U8(0x7f);
  b.U8(0);
  b.Str("e");
  b.Str("t");
  b.U8(1);  // ExternalKind::Table
  b.U8(0x70);
  b.U8(0);
  b.U32(1);
  return b.take();
}

std::vector<uint8_t> BuildFunctionPayload() {
  Builder b;
  b.U32(1);
  b.U32(1);
  return b.take();
}

std::vector<uint8_t> BuildTablePayload() {
  Builder b;
  b.U32(1);
  b.U8(0x70);
  b.U8(0);
  b.U32(1);
  return b.take();
}

std::vector<uint8_t> BuildMemoryPayload() {
  Builder b;
  b.U32(1);
  b.U8(0);
  b.U32(1);
  return b.take();
}

std::vector<uint8_t> BuildGlobalPayload() {
  Builder b;
  b.U32(1);
  b.U8(0x7f);
  b.U8(0);
  b.U8(0x41);
  b.U32(0);
  b.U8(0x0b);
  return b.take();
}

void AppendSection(Builder& module, uint8_t id, const std::vector<uint8_t>& payload) {
  module.U8(id);
  module.U32(static_cast<uint32_t>(payload.size()));
  module.Bytes(payload);
}

// Wraps custom section contents with the section name, since the name is part
// of the section payload.
std::vector<uint8_t> NamedCustom(const char* name,
                                 const std::vector<uint8_t>& content) {
  Builder b;
  b.Str(name);
  b.Bytes(content);
  return b.take();
}

// Builds the full fixture.  Every relocation section (and the second "x")
// appears before the linking section, while each targeted section still comes
// first.
std::vector<uint8_t> BuildFixture() {
  Builder module;
  module.U8(0x00);
  module.U8(0x61);
  module.U8(0x73);
  module.U8(0x6d);  // magic
  module.U8(0x01);
  module.U8(0x00);
  module.U8(0x00);
  module.U8(0x00);  // version
  AppendSection(module, 1, BuildTypePayload());
  AppendSection(module, 2, BuildImportPayload());
  AppendSection(module, 3, BuildFunctionPayload());
  AppendSection(module, 4, BuildTablePayload());
  AppendSection(module, 5, BuildMemoryPayload());
  AppendSection(module, 6, BuildGlobalPayload());
  AppendSection(module, 10, BuildCodePayload());
  AppendSection(module, 11, BuildDataPayload());
  AppendSection(module, 0, BuildXPayload());  // first custom named "x"
  AppendSection(module, 0, BuildXPayload());  // second custom named "x"
  AppendSection(module, 0,
                NamedCustom("reloc.Code", BuildRelocCode1Payload()));
  AppendSection(module, 0,
                NamedCustom("reloc.Code", BuildRelocCode2Payload()));
  AppendSection(module, 0,
                NamedCustom("reloc.Data", BuildRelocDataPayload()));
  AppendSection(module, 0, NamedCustom("reloc.x", BuildRelocXPayload()));
  AppendSection(module, 0,
                NamedCustom("linking", BuildLinkingPayload()));
  return module.take();
}

// Minimal module header used by the negative tests.
void AppendModuleHeader(Builder& module) {
  module.U8(0x00);
  module.U8(0x61);
  module.U8(0x73);
  module.U8(0x6d);
  module.U8(0x01);
  module.U8(0x00);
  module.U8(0x00);
  module.U8(0x00);
}

ReadBinaryOptions MakeOptions() {
  ReadBinaryOptions options;
  options.features.EnableAll();
  options.stop_on_first_error = true;
  options.fail_on_custom_section_error = true;
  return options;
}

bool ReadMap(const std::vector<uint8_t>& bytes,
             Errors* errors,
             BinaryReferenceMap* map) {
  return wabt::Succeeded(ReadBinaryReferenceMap(
      ByteSpan(bytes.data(), bytes.size()), MakeOptions(), errors, map));
}

const ReferenceRecord* FindRecord(const BinaryReferenceMap& map,
                                  wabt::Index reloc_section,
                                  wabt::Index entry) {
  for (const ReferenceRecord& record : map.records) {
    if (record.reloc_section_index == reloc_section &&
        record.reloc_entry_index == entry) {
      return &record;
    }
  }
  return nullptr;
}

const ReferenceSymbol* FindSymbol(const BinaryReferenceMap& map,
                                  wabt::Index symbol_index) {
  if (symbol_index >= map.symbols.size()) {
    return nullptr;
  }
  return &map.symbols[symbol_index];
}

// Converts an offset relative to a section payload into a file offset.
size_t FileOffset(const BinaryReferenceMap& map,
                  wabt::Index section_index,
                  size_t section_offset) {
  const auto* section = map.GetSection(section_index);
  return section->payload_begin + section_offset;
}

// ---- sites group -----------------------------------------------------------

int TestRecordOrderAndIdentity() {
  auto bytes = BuildFixture();
  Errors errors;
  BinaryReferenceMap map;
  TASK_CHECK(ReadMap(bytes, &errors, &map));
  TASK_CHECK(errors.empty());
  // Six code + two data + two custom records.
  TASK_CHECK(map.records.size() == 10);
  // Original table and entry order is preserved verbatim.
  TASK_CHECK(map.records[0].reloc_section_index == kSecRelocCode1);
  TASK_CHECK(map.records[0].reloc_entry_index == 0);
  TASK_CHECK(map.records[2].reloc_section_index == kSecRelocCode1);
  TASK_CHECK(map.records[3].reloc_section_index == kSecRelocCode2);
  TASK_CHECK(map.records[5].reloc_entry_index == 2);
  TASK_CHECK(map.records[8].reloc_section_index == kSecRelocX);
  return 0;
}

int TestPaddedCodeFields() {
  auto bytes = BuildFixture();
  Errors errors;
  BinaryReferenceMap map;
  TASK_CHECK(ReadMap(bytes, &errors, &map));

  const ReferenceRecord* global =
      FindRecord(map, kSecRelocCode1, 1);  // GlobalIndexLEB
  TASK_CHECK(global != nullptr);
  TASK_CHECK(global->type == RelocType::GlobalIndexLEB);
  TASK_CHECK(global->target_section_index == kSecCode);
  TASK_CHECK(global->section_offset == kOffGlobalIdx);
  TASK_CHECK(global->file_end - global->file_begin == 5);
  TASK_CHECK(global->encoding.size() == 5);
  TASK_CHECK(global->encoding[0] == 0x80);
  TASK_CHECK(global->encoding[4] == 0x00);
  TASK_CHECK(global->source.kind ==
             ReferenceSource::Kind::FunctionInstruction);
  TASK_CHECK(global->source.function_index == 1);
  TASK_CHECK(global->source.instruction_offset ==
             FileOffset(map, kSecCode, kOffGlobalGetOpcode));

  const ReferenceRecord* call = FindRecord(map, kSecRelocCode1, 0);
  TASK_CHECK(call != nullptr);
  TASK_CHECK(call->type == RelocType::FuncIndexLEB);
  TASK_CHECK(call->section_offset == kOffCallIdx);
  TASK_CHECK(call->file_end - call->file_begin == 5);
  TASK_CHECK(call->source.instruction_offset ==
             FileOffset(map, kSecCode, kOffCallOpcode));

  const ReferenceRecord* sleb = FindRecord(map, kSecRelocCode2, 1);
  TASK_CHECK(sleb != nullptr);
  TASK_CHECK(sleb->type == RelocType::MemoryAddressSLEB);
  TASK_CHECK(sleb->addend == 7);
  TASK_CHECK(sleb->section_offset == kOffSleb);
  TASK_CHECK(sleb->file_end - sleb->file_begin == 5);
  TASK_CHECK(sleb->source.instruction_offset ==
             FileOffset(map, kSecCode, kOffI32ConstOpcode));
  return 0;
}

int TestSignedBlockType() {
  auto bytes = BuildFixture();
  Errors errors;
  BinaryReferenceMap map;
  TASK_CHECK(ReadMap(bytes, &errors, &map));

  const ReferenceRecord* block = FindRecord(map, kSecRelocCode2, 2);
  TASK_CHECK(block != nullptr);
  TASK_CHECK(block->type == RelocType::TypeIndexLEB);
  TASK_CHECK(block->section_offset == kOffBlockType);
  TASK_CHECK(block->file_end - block->file_begin == 5);
  TASK_CHECK(block->source.instruction_offset ==
             FileOffset(map, kSecCode, kOffBlockOpcode));
  TASK_CHECK(block->targets.size() == 1);
  TASK_CHECK(block->targets[0].kind == ReferenceTarget::Kind::Type);
  TASK_CHECK(block->targets[0].index == 1);
  TASK_CHECK(block->targets[0].symbol_index == wabt::kInvalidIndex);
  return 0;
}

int TestCallIndirectTwoFields() {
  auto bytes = BuildFixture();
  Errors errors;
  BinaryReferenceMap map;
  TASK_CHECK(ReadMap(bytes, &errors, &map));

  const ReferenceRecord* type = FindRecord(map, kSecRelocCode1, 2);
  const ReferenceRecord* table = FindRecord(map, kSecRelocCode2, 0);
  TASK_CHECK(type != nullptr && table != nullptr);
  TASK_CHECK(type->type == RelocType::TypeIndexLEB);
  TASK_CHECK(table->type == RelocType::TableNumberLEB);
  // Both fields belong to the same instruction; the following block/end
  // instruction cannot claim either one.
  TASK_CHECK(type->source.instruction_offset ==
             FileOffset(map, kSecCode, kOffCallIndirectOpcode));
  TASK_CHECK(table->source.instruction_offset ==
             FileOffset(map, kSecCode, kOffCallIndirectOpcode));
  TASK_CHECK(type->section_offset == kOffCallIndirectType);
  TASK_CHECK(table->section_offset == kOffCallIndirectTable);
  TASK_CHECK(type->file_begin < table->file_begin);
  TASK_CHECK(type->file_end == table->file_begin);
  TASK_CHECK(type->targets[0].kind == ReferenceTarget::Kind::Type);
  TASK_CHECK(table->targets.size() == 1);
  TASK_CHECK(table->targets[0].kind == ReferenceTarget::Kind::Table);
  return 0;
}

int TestDataAndCustomFixedFields() {
  auto bytes = BuildFixture();
  Errors errors;
  BinaryReferenceMap map;
  TASK_CHECK(ReadMap(bytes, &errors, &map));

  const ReferenceRecord* data0 = FindRecord(map, kSecRelocData, 0);
  const ReferenceRecord* data1 = FindRecord(map, kSecRelocData, 1);
  TASK_CHECK(data0 != nullptr && data1 != nullptr);
  TASK_CHECK(data0->type == RelocType::MemoryAddressI32);
  TASK_CHECK(data0->target_section_index == kSecData);
  TASK_CHECK(data0->source.kind == ReferenceSource::Kind::DataSegment);
  TASK_CHECK(data0->source.data_segment == 0);
  TASK_CHECK(data0->source.data_segment_offset == 0);
  TASK_CHECK(data0->file_end - data0->file_begin == 4);
  TASK_CHECK(data1->source.data_segment_offset == 4);

  const ReferenceRecord* func_off = FindRecord(map, kSecRelocX, 0);
  const ReferenceRecord* sec_off = FindRecord(map, kSecRelocX, 1);
  TASK_CHECK(func_off != nullptr && sec_off != nullptr);
  TASK_CHECK(func_off->type == RelocType::FunctionOffsetI32);
  TASK_CHECK(func_off->addend == -4);
  TASK_CHECK(sec_off->type == RelocType::SectionOffsetI32);
  TASK_CHECK(func_off->target_section_index == kSecX2);
  TASK_CHECK(sec_off->target_section_index == kSecX2);
  TASK_CHECK(func_off->source.kind ==
             ReferenceSource::Kind::CustomContent);
  TASK_CHECK(func_off->source.custom_section_index == kSecX2);
  TASK_CHECK(func_off->source.custom_content_offset == 0);
  TASK_CHECK(sec_off->source.custom_content_offset == 4);
  return 0;
}

int TestDuplicateCustomNames() {
  auto bytes = BuildFixture();
  Errors errors;
  BinaryReferenceMap map;
  TASK_CHECK(ReadMap(bytes, &errors, &map));

  size_t named_x = 0;
  for (const auto& section : map.sections) {
    if (section.section_type == wabt::BinarySection::Custom &&
        section.name == "x") {
      ++named_x;
    }
  }
  TASK_CHECK(named_x == 2);
  const auto* first = map.GetSection(kSecX1);
  const auto* second = map.GetSection(kSecX2);
  TASK_CHECK(first != nullptr && second != nullptr);
  TASK_CHECK(first->name == "x" && second->name == "x");
  TASK_CHECK(first->payload_begin != second->payload_begin);
  // Payload origin includes the name length and name.
  TASK_CHECK(second->payload_end - second->payload_begin == 10);
  const ReferenceRecord* sec_off = FindRecord(map, kSecRelocX, 1);
  TASK_CHECK(sec_off != nullptr);
  TASK_CHECK(sec_off->section_offset == kOffXField1);
  return 0;
}

int TestLocationErrors() {
  // Relocation landing on an opcode byte.
  {
    Builder module;
    AppendModuleHeader(module);
    AppendSection(module, 1, BuildTypePayload());
    AppendSection(module, 2, BuildImportPayload());
    AppendSection(module, 3, BuildFunctionPayload());
    AppendSection(module, 10, BuildCodePayload());
    Builder reloc;
    reloc.U32(kSecCode);
    reloc.U32(1);
    AppendRelocEntry(reloc, 0, kOffGlobalGetOpcode, kSymFunc, false);
    AppendSection(module, 0, NamedCustom("reloc.Code", reloc.take()));
    AppendSection(module, 0,
                  NamedCustom("linking", BuildLinkingPayload()));
    Errors errors;
    BinaryReferenceMap map;
    TASK_CHECK(!ReadMap(module.take(), &errors, &map));
    TASK_CHECK(!errors.empty());
    TASK_CHECK(map.records.empty());
  }

  // Relocation on the function body size field.
  {
    Builder module;
    AppendModuleHeader(module);
    AppendSection(module, 1, BuildTypePayload());
    AppendSection(module, 3, BuildFunctionPayload());
    AppendSection(module, 10, BuildCodePayload());
    Builder reloc;
    reloc.U32(kSecCode);
    reloc.U32(1);
    AppendRelocEntry(reloc, 0, kOffBodySize, kSymFunc, false);
    AppendSection(module, 0, NamedCustom("reloc.Code", reloc.take()));
    AppendSection(module, 0,
                  NamedCustom("linking", BuildLinkingPayload()));
    Errors errors;
    BinaryReferenceMap map;
    TASK_CHECK(!ReadMap(module.take(), &errors, &map));
    TASK_CHECK(!errors.empty());
  }

  // Relocation on a local declaration field.
  {
    Builder module;
    AppendModuleHeader(module);
    AppendSection(module, 1, BuildTypePayload());
    AppendSection(module, 3, BuildFunctionPayload());
    AppendSection(module, 10, BuildCodePayload());
    Builder reloc;
    reloc.U32(kSecCode);
    reloc.U32(1);
    AppendRelocEntry(reloc, 7, kOffLocals, kSymImpGlobal, false);
    AppendSection(module, 0, NamedCustom("reloc.Code", reloc.take()));
    AppendSection(module, 0,
                  NamedCustom("linking", BuildLinkingPayload()));
    Errors errors;
    BinaryReferenceMap map;
    TASK_CHECK(!ReadMap(module.take(), &errors, &map));
    TASK_CHECK(!errors.empty());
  }

  // Relocation in the middle of a multi-byte immediate.
  {
    Builder module;
    AppendModuleHeader(module);
    AppendSection(module, 1, BuildTypePayload());
    AppendSection(module, 2, BuildImportPayload());
    AppendSection(module, 3, BuildFunctionPayload());
    AppendSection(module, 10, BuildCodePayload());
    Builder reloc;
    reloc.U32(kSecCode);
    reloc.U32(1);
    AppendRelocEntry(reloc, 7, kOffGlobalIdx + 1, kSymImpGlobal, false);
    AppendSection(module, 0, NamedCustom("reloc.Code", reloc.take()));
    AppendSection(module, 0,
                  NamedCustom("linking", BuildLinkingPayload()));
    Errors errors;
    BinaryReferenceMap map;
    TASK_CHECK(!ReadMap(module.take(), &errors, &map));
    TASK_CHECK(!errors.empty());
  }

  // A fixed-width field overrunning its data segment.
  {
    Builder data;
    data.U32(1);
    data.U32(0);
    data.U8(0x41);
    data.U32(0);
    data.U8(0x0b);
    data.U32(4);  // only four content bytes
    data.Fixed4(0);
    // Content starts at payload offset 6; a four-byte field at offset 6 ends
    // exactly at the content end, so place the reloc one byte later.
    Builder module;
    AppendModuleHeader(module);
    AppendSection(module, 5, BuildMemoryPayload());
    AppendSection(module, 11, data.take());
    Builder reloc;
    reloc.U32(kSecData);
    reloc.U32(1);
    AppendRelocEntry(reloc, 5, kOffDataField0 + 1, kSymDataA, true, 0);
    AppendSection(module, 0, NamedCustom("reloc.Data", reloc.take()));
    AppendSection(module, 0,
                  NamedCustom("linking", BuildLinkingPayload()));
    Errors errors;
    BinaryReferenceMap map;
    TASK_CHECK(!ReadMap(module.take(), &errors, &map));
    TASK_CHECK(!errors.empty());
  }
  return 0;
}

int TestUnsupportedAndBadIndexes() {
  // Unsupported relocation type (TableIndexSLEB = 1).
  {
    Builder module;
    AppendModuleHeader(module);
    AppendSection(module, 1, BuildTypePayload());
    AppendSection(module, 3, BuildFunctionPayload());
    AppendSection(module, 10, BuildCodePayload());
    Builder reloc;
    reloc.U32(kSecCode);
    reloc.U32(1);
    AppendRelocEntry(reloc, 1, kOffCallIdx, kSymFunc, false);
    AppendSection(module, 0, NamedCustom("reloc.Code", reloc.take()));
    AppendSection(module, 0,
                  NamedCustom("linking", BuildLinkingPayload()));
    Errors errors;
    BinaryReferenceMap map;
    TASK_CHECK(!ReadMap(module.take(), &errors, &map));
    TASK_CHECK(!errors.empty());
    TASK_CHECK(map.records.empty());
  }

  // Symbol index out of range.
  {
    Builder module;
    AppendModuleHeader(module);
    AppendSection(module, 1, BuildTypePayload());
    AppendSection(module, 3, BuildFunctionPayload());
    AppendSection(module, 10, BuildCodePayload());
    Builder reloc;
    reloc.U32(kSecCode);
    reloc.U32(1);
    AppendRelocEntry(reloc, 0, kOffCallIdx, 999, false);
    AppendSection(module, 0, NamedCustom("reloc.Code", reloc.take()));
    AppendSection(module, 0,
                  NamedCustom("linking", BuildLinkingPayload()));
    Errors errors;
    BinaryReferenceMap map;
    TASK_CHECK(!ReadMap(module.take(), &errors, &map));
    TASK_CHECK(!errors.empty());
  }

  // Type/field mismatch: a global reloc names a function symbol.
  {
    Builder module;
    AppendModuleHeader(module);
    AppendSection(module, 1, BuildTypePayload());
    AppendSection(module, 2, BuildImportPayload());
    AppendSection(module, 3, BuildFunctionPayload());
    AppendSection(module, 10, BuildCodePayload());
    Builder reloc;
    reloc.U32(kSecCode);
    reloc.U32(1);
    AppendRelocEntry(reloc, 7, kOffGlobalIdx, kSymFunc, false);
    AppendSection(module, 0, NamedCustom("reloc.Code", reloc.take()));
    AppendSection(module, 0,
                  NamedCustom("linking", BuildLinkingPayload()));
    Errors errors;
    BinaryReferenceMap map;
    TASK_CHECK(!ReadMap(module.take(), &errors, &map));
    TASK_CHECK(!errors.empty());
  }
  return 0;
}

int TestReuseAfterFailureAndInputRelease() {
  auto good = BuildFixture();

  // A broken file must not poison the reader for later inputs.
  {
    Builder module;
    AppendModuleHeader(module);
    AppendSection(module, 10, BuildCodePayload());
    Builder reloc;
    reloc.U32(kSecCode);
    reloc.U32(1);
    AppendRelocEntry(reloc, 0, kOffCallIdx, 999, false);
    AppendSection(module, 0, NamedCustom("reloc.Code", reloc.take()));
    Errors errors;
    BinaryReferenceMap failed_map;
    TASK_CHECK(!ReadMap(module.take(), &errors, &failed_map));
    TASK_CHECK(!errors.empty());
  }

  Errors errors;
  BinaryReferenceMap map;
  TASK_CHECK(ReadMap(good, &errors, &map));
  TASK_CHECK(errors.empty());

  // Release the input buffer; the map owns everything it exposes.
  std::vector<uint8_t> dropped = BuildFixture();
  {
    Errors local_errors;
    BinaryReferenceMap local_map;
    TASK_CHECK(ReadMap(dropped, &local_errors, &local_map));
    dropped.clear();
    dropped.shrink_to_fit();
    TASK_CHECK(!local_map.records.empty());
    const ReferenceRecord& record = local_map.records[0];
    TASK_CHECK(!record.encoding.empty());
    TASK_CHECK(!local_map.symbols[kSymFunc].name.empty());
    TASK_CHECK(local_map.symbols[kSymFunc].name == "f");
  }
  return 0;
}

// ---- symbols group ---------------------------------------------------------

int TestSymbolIdentities() {
  auto bytes = BuildFixture();
  Errors errors;
  BinaryReferenceMap map;
  TASK_CHECK(ReadMap(bytes, &errors, &map));
  TASK_CHECK(map.symbols.size() == 11);

  const ReferenceSymbol* imp_func = FindSymbol(map, kSymImpFunc);
  const ReferenceSymbol* func = FindSymbol(map, kSymFunc);
  const ReferenceSymbol* imp_global = FindSymbol(map, kSymImpGlobal);
  const ReferenceSymbol* imp_table = FindSymbol(map, kSymImpTable);
  TASK_CHECK(imp_func && func && imp_global && imp_table);
  TASK_CHECK(imp_func->symbol_type == SymbolType::Function);
  TASK_CHECK(imp_func->defined == false);
  TASK_CHECK(imp_func->item_index == 0);
  TASK_CHECK(imp_func->import_module == "e");
  TASK_CHECK(imp_func->import_field == "f");
  TASK_CHECK(func->defined && func->item_index == 1);
  TASK_CHECK(imp_global->symbol_type == SymbolType::Global);
  TASK_CHECK(imp_global->import_field == "g");
  TASK_CHECK(imp_table->symbol_type == SymbolType::Table);
  TASK_CHECK(imp_table->import_field == "t");

  // Undefined data keeps an external identity instead of a zero definition.
  const ReferenceRecord* call = FindRecord(map, kSecRelocCode1, 0);
  TASK_CHECK(call != nullptr);
  TASK_CHECK(call->targets.size() == 1);
  TASK_CHECK(call->targets[0].kind == ReferenceTarget::Kind::Function);
  TASK_CHECK(call->targets[0].index == 1);

  const ReferenceRecord* global = FindRecord(map, kSecRelocCode1, 1);
  TASK_CHECK(global != nullptr);
  TASK_CHECK(global->targets[0].imported);
  TASK_CHECK(global->targets[0].import_module == "e");
  TASK_CHECK(global->targets[0].import_field == "g");

  const ReferenceSymbol* section_sym = FindSymbol(map, kSymSectionX);
  TASK_CHECK(section_sym != nullptr);
  TASK_CHECK(section_sym->symbol_type == SymbolType::Section);
  TASK_CHECK(section_sym->item_index == kSecX2);
  return 0;
}

int TestDataAliases() {
  auto bytes = BuildFixture();
  Errors errors;
  BinaryReferenceMap map;
  TASK_CHECK(ReadMap(bytes, &errors, &map));

  const ReferenceRecord* field0 = FindRecord(map, kSecRelocData, 0);
  TASK_CHECK(field0 != nullptr);
  // Both "a" (offset 0 size 4) and "b" (offset 0 size 8) fully cover the
  // first field: neither name nor first-match may pick just one.
  bool saw_a = false;
  bool saw_b = false;
  for (const ReferenceTarget& target : field0->targets) {
    TASK_CHECK(target.kind == ReferenceTarget::Kind::Data);
    TASK_CHECK(target.defined);
    TASK_CHECK(target.data_segment == 0);
    if (target.symbol_index == kSymDataA) {
      saw_a = true;
      TASK_CHECK(target.data_size == 4);
    } else if (target.symbol_index == kSymDataB) {
      saw_b = true;
      TASK_CHECK(target.data_size == 8);
    }
  }
  TASK_CHECK(saw_a && saw_b);
  TASK_CHECK(field0->targets.size() == 2);

  const ReferenceRecord* field1 = FindRecord(map, kSecRelocData, 1);
  TASK_CHECK(field1 != nullptr);
  bool saw_section = false;
  saw_b = false;
  for (const ReferenceTarget& target : field1->targets) {
    if (target.kind == ReferenceTarget::Kind::Section) {
      saw_section = true;
      TASK_CHECK(target.index == kSecData);
    } else if (target.symbol_index == kSymDataB) {
      saw_b = true;
    }
  }
  TASK_CHECK(saw_section && saw_b);
  return 0;
}

int TestIncomingOutgoingIndexes() {
  auto bytes = BuildFixture();
  Errors errors;
  BinaryReferenceMap map;
  TASK_CHECK(ReadMap(bytes, &errors, &map));

  // The defined function is targeted by the call record and the
  // FunctionOffsetI32 record.
  const auto& func_in = map.IncomingReferences(kSymFunc);
  TASK_CHECK(func_in.size() == 2);

  // The broad data alias is targeted by the SLEB and both data fields.
  const auto& data_b_in = map.IncomingReferences(kSymDataB);
  TASK_CHECK(data_b_in.size() == 3);

  // The function definition symbol contains every code-section field.
  const auto& func_out = map.OutgoingReferences(kSymFunc);
  TASK_CHECK(func_out.size() == 6);

  // The code section symbol covers the same six fields.
  const auto& code_section_out = map.OutgoingReferences(kSymSectionCode);
  TASK_CHECK(code_section_out.size() == 6);
  TASK_CHECK(func_out == code_section_out);

  // The custom section symbol owns the two custom records.
  const auto& x_out = map.OutgoingReferences(kSymSectionX);
  TASK_CHECK(x_out.size() == 2);

  // Indexed records are the same objects as records[], never copies.
  for (size_t record_index : func_out) {
    TASK_CHECK(record_index < map.records.size());
    TASK_CHECK(map.records[record_index].target_section_index == kSecCode);
  }
  // Out-of-range symbol queries return an empty list.
  TASK_CHECK(map.IncomingReferences(9999).empty());
  TASK_CHECK(map.OutgoingReferences(9999).empty());
  return 0;
}

int TestTypeTargetBypassesSymbolTable() {
  auto bytes = BuildFixture();
  Errors errors;
  BinaryReferenceMap map;
  TASK_CHECK(ReadMap(bytes, &errors, &map));

  const ReferenceRecord* block = FindRecord(map, kSecRelocCode2, 2);
  const ReferenceRecord* ci_type = FindRecord(map, kSecRelocCode1, 2);
  TASK_CHECK(block != nullptr && ci_type != nullptr);
  for (const ReferenceRecord* record : {block, ci_type}) {
    TASK_CHECK(record->targets.size() == 1);
    TASK_CHECK(record->targets[0].kind == ReferenceTarget::Kind::Type);
    TASK_CHECK(record->targets[0].symbol_index == wabt::kInvalidIndex);
  }
  // No incoming reference for a type index: types have no symbol.
  TASK_CHECK(map.IncomingReferences(kSymFunc).size() == 2);
  return 0;
}

int TestOverlappingFieldsRejected() {
  // Two relocation entries claiming the same bytes must be rejected, even
  // when they live in separate tables targeting the same section.
  Builder module;
  AppendModuleHeader(module);
  AppendSection(module, 1, BuildTypePayload());
  AppendSection(module, 3, BuildFunctionPayload());
  AppendSection(module, 10, BuildCodePayload());
  Builder reloc;
  reloc.U32(kSecCode);
  reloc.U32(2);
  AppendRelocEntry(reloc, 0, kOffCallIdx, kSymFunc, false);
  AppendRelocEntry(reloc, 0, kOffCallIdx, kSymFunc, false);
  AppendSection(module, 0, NamedCustom("reloc.Code", reloc.take()));
  AppendSection(module, 0,
                NamedCustom("linking", BuildLinkingPayload()));
  Errors errors;
  BinaryReferenceMap map;
  TASK_CHECK(!ReadMap(module.take(), &errors, &map));
  TASK_CHECK(!errors.empty());
  TASK_CHECK(map.records.empty());
  return 0;
}

int TestUndefinedDataStaysExternal() {
  // An undefined data symbol keeps an external identity: its zero segment and
  // offset fields must not be mistaken for a definition.
  Builder data;
  data.U32(1);
  data.U32(0);
  data.U8(0x41);
  data.U32(0);
  data.U8(0x0b);
  data.U32(4);
  data.Fixed4(0);

  Builder symbols;
  symbols.U32(1);  // SymbolType::Data
  symbols.U32(/*undefined=*/0x10);
  symbols.Str("ext");  // undefined data carries no segment fields

  Builder linking;
  linking.U32(2);
  linking.U32(8);
  // Subsection size = symbol count field plus the single symbol entry.
  linking.U32(static_cast<uint32_t>(symbols.size() + 1));
  linking.U32(1);
  linking.Bytes(symbols.take());

  Builder reloc;
  reloc.U32(/*Data section physical index=*/1);
  reloc.U32(1);
  AppendRelocEntry(reloc, 5, kOffDataField0, 0, true, 0);

  Builder module;
  AppendModuleHeader(module);
  AppendSection(module, 5, BuildMemoryPayload());
  AppendSection(module, 11, data.take());
  AppendSection(module, 0, NamedCustom("reloc.Data", reloc.take()));
  AppendSection(module, 0, NamedCustom("linking", linking.take()));

  Errors errors;
  BinaryReferenceMap map;
  TASK_CHECK(ReadMap(module.take(), &errors, &map));
  TASK_CHECK(errors.empty());
  TASK_CHECK(map.records.size() == 1);
  const ReferenceRecord& record = map.records[0];
  TASK_CHECK(record.targets.size() == 1);
  const ReferenceTarget& target = record.targets[0];
  TASK_CHECK(target.kind == ReferenceTarget::Kind::Data);
  TASK_CHECK(target.defined == false);
  TASK_CHECK(target.symbol_index == 0);
  TASK_CHECK(map.symbols[0].name == "ext");
  TASK_CHECK(map.symbols[0].defined == false);
  TASK_CHECK(map.symbols[0].data_segment == wabt::kInvalidIndex);
  return 0;
}

int TestUnreferencedBadSymbolRejected() {
  // A function symbol pointing past the function space must fail even though
  // no relocation references it.
  Builder symbols;
  symbols.U32(1);  // SymbolType::Function
  symbols.U32(0);  // defined
  symbols.U32(99); // out of range function index
  symbols.Str("ghost");

  Builder linking;
  linking.U32(2);
  linking.U32(8);
  linking.U32(static_cast<uint32_t>(symbols.size() + 1));
  linking.U32(1);
  linking.Bytes(symbols.take());

  Builder module;
  AppendModuleHeader(module);
  AppendSection(module, 1, BuildTypePayload());
  AppendSection(module, 3, BuildFunctionPayload());
  AppendSection(module, 0,
                NamedCustom("linking", linking.take()));

  Errors errors;
  BinaryReferenceMap map;
  TASK_CHECK(!ReadMap(module.take(), &errors, &map));
  TASK_CHECK(!errors.empty());
  TASK_CHECK(map.records.empty());

  // A section symbol naming a nonexistent physical section must likewise
  // fail eagerly.
  Builder symbols2;
  symbols2.U32(3);  // SymbolType::Section
  symbols2.U32(0);
  symbols2.U32(99);

  Builder linking2;
  linking2.U32(2);
  linking2.U32(8);
  linking2.U32(static_cast<uint32_t>(symbols2.size() + 1));
  linking2.U32(1);
  linking2.Bytes(symbols2.take());

  Builder module2;
  AppendModuleHeader(module2);
  AppendSection(module2, 0,
                NamedCustom("linking", linking2.take()));
  Errors errors2;
  BinaryReferenceMap map2;
  TASK_CHECK(!ReadMap(module2.take(), &errors2, &map2));
  TASK_CHECK(!errors2.empty());
  return 0;
}

int TestRelocBeforeLinkingAccepted() {
  // The fixture itself places every reloc.* section before the linking
  // section; simply confirm it parses and the symbols still resolve.
  auto bytes = BuildFixture();
  Errors errors;
  BinaryReferenceMap map;
  TASK_CHECK(ReadMap(bytes, &errors, &map));
  TASK_CHECK(map.GetSection(kSecLinking) != nullptr);
  TASK_CHECK(map.GetSection(kSecRelocCode1) != nullptr);
  TASK_CHECK(map.GetSection(kSecRelocCode1)->section_index <
             kSecLinking);
  const ReferenceRecord* table = FindRecord(map, kSecRelocCode2, 0);
  TASK_CHECK(table != nullptr);
  TASK_CHECK(table->targets[0].kind == ReferenceTarget::Kind::Table);
  TASK_CHECK(table->targets[0].imported);
  return 0;
}

// ---- consumer group --------------------------------------------------------

// popen statuses are passed to waitpid; provide the portable decode.
int WExitStatus(int status) { return (status >> 8) & 0xff; }

std::filesystem::path TestDir() {
  const char* tmp = getenv("TMPDIR");
  std::filesystem::path dir =
      tmp ? std::filesystem::path(tmp) / "wabt_rmap_tests"
          : std::filesystem::path(".task-build") / "rmap-tests";
  std::filesystem::create_directories(dir);
  return dir;
}

int RunObjdump(const std::string& args,
               const std::filesystem::path& wasm,
               std::string* output,
               int* exit_code) {
  std::string command = ".task-build/wabt/wasm-objdump " + args + " " +
                        wasm.string() + " 2>/dev/null";
  FILE* pipe = popen(command.c_str(), "r");
  TASK_CHECK(pipe != nullptr);
  output->clear();
  char buffer[256];
  size_t read_bytes;
  while ((read_bytes = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
    output->append(buffer, read_bytes);
  }
  int status = pclose(pipe);
  *exit_code = WExitStatus(status);
  return 0;
}

int TestConsumerNewMode() {
  auto dir = TestDir();
  auto wasm = dir / "fixture.wasm";
  auto bytes = BuildFixture();
  std::ofstream(wasm, std::ios::binary).write(
      reinterpret_cast<const char*>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()));

  std::string output;
  int exit_code = 0;
  TASK_CHECK(RunObjdump("--reference-map", wasm, &output, &exit_code) == 0);
  TASK_CHECK(exit_code == 0);
  TASK_CHECK(output.find("Reference Map:") != std::string::npos);
  TASK_CHECK(output.find("R_WASM_TYPE_INDEX_LEB") != std::string::npos);
  TASK_CHECK(output.find("R_WASM_TABLE_NUMBER_LEB") != std::string::npos);
  TASK_CHECK(output.find("R_WASM_MEMORY_ADDR_I32") != std::string::npos);
  TASK_CHECK(output.find("R_WASM_FUNCTION_OFFSET_I32") !=
             std::string::npos);
  TASK_CHECK(output.find("func[1] instruction@") != std::string::npos);
  TASK_CHECK(output.find("data[0]+0x0") != std::string::npos);
  TASK_CHECK(output.find("custom[9]+0x0") != std::string::npos);
  TASK_CHECK(output.find("import<e.g>") != std::string::npos);
  return 0;
}

int TestConsumerSectionFilter() {
  auto dir = TestDir();
  auto wasm = dir / "fixture.wasm";

  std::string code_output;
  int code_exit = 0;
  TASK_CHECK(RunObjdump("--reference-map -j Code", wasm, &code_output,
                        &code_exit) == 0);
  TASK_CHECK(code_exit == 0);
  TASK_CHECK(code_output.find("R_WASM_GLOBAL_INDEX_LEB") !=
             std::string::npos);
  TASK_CHECK(code_output.find("R_WASM_MEMORY_ADDR_I32") ==
             std::string::npos);
  TASK_CHECK(code_output.find("R_WASM_SECTION_OFFSET_I32") ==
             std::string::npos);

  std::string data_output;
  int data_exit = 0;
  TASK_CHECK(RunObjdump("--reference-map -j Data", wasm, &data_output,
                        &data_exit) == 0);
  TASK_CHECK(data_exit == 0);
  TASK_CHECK(data_output.find("R_WASM_MEMORY_ADDR_I32") !=
             std::string::npos);
  TASK_CHECK(data_output.find("R_WASM_GLOBAL_INDEX_LEB") ==
             std::string::npos);

  // Filtering only hides source sections; the full map still parses.
  std::string custom_output;
  int custom_exit = 0;
  TASK_CHECK(RunObjdump("--reference-map -j x", wasm, &custom_output,
                        &custom_exit) == 0);
  TASK_CHECK(custom_exit == 0);
  TASK_CHECK(custom_output.find("R_WASM_FUNCTION_OFFSET_I32") !=
             std::string::npos);

  // An unknown section name keeps the historical -j failure behavior.
  std::string missing_output;
  int missing_exit = 0;
  TASK_CHECK(RunObjdump("--reference-map -j NoSuch", wasm,
                        &missing_output, &missing_exit) == 0);
  TASK_CHECK(missing_exit != 0);
  TASK_CHECK(missing_output.find("Reference Map:") ==
             std::string::npos);
  return 0;
}

int TestConsumerOldModeUnchanged() {
  auto dir = TestDir();
  auto wasm = dir / "fixture.wasm";

  std::string output;
  int exit_code = 0;
  TASK_CHECK(RunObjdump("-d -x", wasm, &output, &exit_code) == 0);
  TASK_CHECK(exit_code == 0);
  TASK_CHECK(output.find("Reference Map:") == std::string::npos);
  TASK_CHECK(output.find("Section Details:") != std::string::npos);
  TASK_CHECK(output.find("Code Disassembly:") != std::string::npos);

  std::string reloc_output;
  int reloc_exit = 0;
  TASK_CHECK(RunObjdump("-d -r", wasm, &reloc_output, &reloc_exit) == 0);
  TASK_CHECK(reloc_exit == 0);
  TASK_CHECK(reloc_output.find("R_WASM_FUNCTION_INDEX_LEB") !=
             std::string::npos);
  TASK_CHECK(reloc_output.find("Reference Map:") == std::string::npos);

  // The new mode combines with the old ones.
  std::string combined;
  int combined_exit = 0;
  TASK_CHECK(RunObjdump("-d --reference-map", wasm, &combined,
                        &combined_exit) == 0);
  TASK_CHECK(combined_exit == 0);
  TASK_CHECK(combined.find("Code Disassembly:") != std::string::npos);
  TASK_CHECK(combined.find("Reference Map:") != std::string::npos);
  return 0;
}

int TestConsumerFailureDiagnostic() {
  auto dir = TestDir();
  auto bad = dir / "bad.wasm";
  Builder module;
  AppendModuleHeader(module);
  AppendSection(module, 10, BuildCodePayload());
  Builder reloc;
  reloc.U32(kSecCode);
  reloc.U32(1);
  AppendRelocEntry(reloc, 1, kOffCallIdx, kSymFunc, false);
  AppendSection(module, 0, NamedCustom("reloc.Code", reloc.take()));
  auto bytes = module.take();
  std::ofstream(bad, std::ios::binary).write(
      reinterpret_cast<const char*>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()));

  std::string output;
  int exit_code = 0;
  TASK_CHECK(RunObjdump("--reference-map", bad, &output, &exit_code) == 0);
  TASK_CHECK(exit_code != 0);
  TASK_CHECK(output.find("Reference Map:") == std::string::npos);
  return 0;
}

}  // namespace

const struct task_case task_cases[] = {
    {"sites", "record-order-and-identity", TestRecordOrderAndIdentity},
    {"sites", "padded-code-fields", TestPaddedCodeFields},
    {"sites", "signed-block-type", TestSignedBlockType},
    {"sites", "call-indirect-two-fields", TestCallIndirectTwoFields},
    {"sites", "data-and-custom-fixed-fields",
     TestDataAndCustomFixedFields},
    {"sites", "duplicate-custom-names", TestDuplicateCustomNames},
    {"sites", "location-errors", TestLocationErrors},
    {"sites", "unsupported-and-bad-indexes",
     TestUnsupportedAndBadIndexes},
    {"sites", "reuse-after-failure-and-input-release",
     TestReuseAfterFailureAndInputRelease},
    {"symbols", "symbol-identities", TestSymbolIdentities},
    {"symbols", "data-aliases", TestDataAliases},
    {"symbols", "incoming-outgoing-indexes",
     TestIncomingOutgoingIndexes},
    {"symbols", "type-target-bypasses-symbol-table",
     TestTypeTargetBypassesSymbolTable},
    {"symbols", "overlapping-fields-rejected",
     TestOverlappingFieldsRejected},
    {"symbols", "undefined-data-stays-external",
     TestUndefinedDataStaysExternal},
    {"symbols", "unreferenced-bad-symbol-rejected",
     TestUnreferencedBadSymbolRejected},
    {"symbols", "reloc-before-linking-accepted",
     TestRelocBeforeLinkingAccepted},
    {"consumer", "new-mode", TestConsumerNewMode},
    {"consumer", "section-filter", TestConsumerSectionFilter},
    {"consumer", "old-mode-unchanged", TestConsumerOldModeUnchanged},
    {"consumer", "failure-diagnostic", TestConsumerFailureDiagnostic},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
