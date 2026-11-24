/////////////////////////////////////////////////////////////////////////
//
// Author: Mateusz Jurczyk (mjurczyk@google.com)
//
// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Description
// ===========
//
// Merges per-round comparison coverage receipts into one union receipt:
//
//   receipt_merge OUTPUT RECEIPT...
//
// Every input receipt must share the same source identifier and the exact
// same complete set of module build identifiers. Multiple receipts for the
// same (source, round, input) triple must be byte-identical. The output is a
// CMPCOV-MERGED receipt containing the union of all traces. Malformed input
// never touches an existing OUTPUT file; the order of input arguments and
// duplicate files do not affect the result.

#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "source/receipt.h"

namespace {

bool ReadFile(const std::string& path, std::string* contents) {
  std::ifstream file(path.c_str(), std::ios::binary);
  if (!file.good()) {
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (file.bad()) {
    return false;
  }
  *contents = buffer.str();
  return true;
}

struct RoundKey {
  std::string source;
  uint64_t round;
  std::string input;

  bool operator<(const RoundKey& other) const {
    if (source != other.source) return source < other.source;
    if (round != other.round) return round < other.round;
    return input < other.input;
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s OUTPUT RECEIPT...\n",
                 argc > 0 ? argv[0] : "receipt_merge");
    return 2;
  }

  const std::string output_path = argv[1];

  std::string common_source;
  std::set<std::string> common_modules;
  bool have_reference = false;
  std::map<RoundKey, std::string> seen_rounds;
  std::set<ReceiptTrace> union_traces;

  for (int i = 2; i < argc; ++i) {
    const std::string receipt_path = argv[i];

    std::string contents;
    if (!ReadFile(receipt_path, &contents)) {
      std::fprintf(stderr, "receipt_merge: unable to read %s\n",
                   receipt_path.c_str());
      return 1;
    }

    Receipt receipt;
    if (!ParseReceipt(contents, &receipt) ||
        receipt.magic != "CMPCOV-ROUND") {
      std::fprintf(stderr, "receipt_merge: malformed receipt %s\n",
                   receipt_path.c_str());
      return 1;
    }

    if (!have_reference) {
      common_source = receipt.source;
      common_modules = receipt.modules;
      have_reference = true;
    } else {
      if (receipt.source != common_source ||
          receipt.modules != common_modules) {
        std::fprintf(stderr,
                     "receipt_merge: source or module set mismatch in %s\n",
                     receipt_path.c_str());
        return 1;
      }
    }

    const RoundKey key{receipt.source, receipt.round, receipt.input};
    const auto inserted = seen_rounds.emplace(key, contents);
    if (!inserted.second && inserted.first->second != contents) {
      std::fprintf(stderr,
                   "receipt_merge: conflicting receipts for the same "
                   "(source, round, input) in %s\n",
                   receipt_path.c_str());
      return 1;
    }

    union_traces.insert(receipt.traces.begin(), receipt.traces.end());
  }

  Receipt merged;
  merged.magic = "CMPCOV-MERGED";
  merged.version = 1;
  merged.source = common_source;
  merged.modules = common_modules;
  merged.traces = union_traces;

  const std::string serialized = SerializeReceipt(merged);
  if (!PublishReceipt(output_path, serialized)) {
    std::fprintf(stderr,
                 "receipt_merge: publication of %s failed; existing file "
                 "left untouched\n",
                 output_path.c_str());
    return 1;
  }

  std::printf("receipt_merge: wrote %zu module(s), %zu trace(s) to %s\n",
              merged.modules.size(), merged.traces.size(),
              output_path.c_str());
  return 0;
}
