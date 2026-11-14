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

// Example: build the read-only binary reference map for an unlinked object
// file and print every record plus the incoming/outgoing indexes.  Build with:
//
//   c++ -std=c++20 -Iinclude -I<build>/include \
//       docs/reference-map-example.cc -L<build> -lwabt -o refmap-example

#include <cstdio>
#include <vector>

#include "wabt/binary-reference-map.h"
#include "wabt/common.h"
#include "wabt/error-formatter.h"
#include "wabt/feature.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s object.wasm\n", argv[0]);
    return 2;
  }

  std::vector<uint8_t> bytes;
  if (wabt::Failed(wabt::ReadFile(argv[1], &bytes))) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }

  wabt::Features features;
  features.EnableAll();
  wabt::ReadBinaryOptions options(features, /*log_stream=*/nullptr,
                                  /*read_debug_names=*/true,
                                  /*stop_on_first_error=*/true,
                                  /*fail_on_custom_section_error=*/true);

  wabt::Errors errors;
  wabt::BinaryReferenceMap map;
  wabt::Result result = wabt::ReadBinaryReferenceMap(
      wabt::ByteSpan(bytes.data(), bytes.size()), options, &errors, &map);
  if (wabt::Failed(result)) {
    // The input is untouched and no partial report is published.
    wabt::FormatErrorsToFile(errors, wabt::Location::Type::Binary);
    return 1;
  }

  // The input buffer may now be released; the map owns all its data.
  bytes.clear();
  bytes.shrink_to_fit();

  for (const wabt::ReferenceRecord& record : map.records) {
    std::printf(
        "reloc.section[%u] entry[%u] %s section+%zu file %zu..%zu addend=%d\n",
        record.reloc_section_index, record.reloc_entry_index,
        wabt::GetRelocTypeName(record.type), record.section_offset,
        record.file_begin, record.file_end, record.addend);
    for (const wabt::ReferenceTarget& target : record.targets) {
      if (target.kind == wabt::ReferenceTarget::Kind::Type) {
        std::printf("  -> type[%u]\n", target.index);
      } else {
        std::printf("  -> symbol[%u]", target.symbol_index);
        if (target.imported) {
          std::printf(" import <%s.%s>", target.import_module.c_str(),
                      target.import_field.c_str());
        }
        std::printf("\n");
      }
    }
  }

  // Reverse lookup: who references each symbol, and what each definition
  // references itself.
  for (wabt::Index symbol = 0; symbol < map.symbols.size(); ++symbol) {
    const auto& incoming = map.IncomingReferences(symbol);
    const auto& outgoing = map.OutgoingReferences(symbol);
    if (incoming.empty() && outgoing.empty()) {
      continue;
    }
    std::printf("symbol[%u] %s: incoming=%zu outgoing=%zu\n", symbol,
                map.symbols[symbol].name.c_str(), incoming.size(),
                outgoing.size());
  }
  return 0;
}
