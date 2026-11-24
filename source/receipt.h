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
// An explicit, per-round journal for comparison coverage. In round mode a
// single comparison trace is attributed to the exact triple
// (source version, module build, corpus round); module base names, load
// addresses and process IDs are never used to merge identities.
//
// The textual receipts produced by CommitRound() and merged by
// tools/receipt_merge.cc are parsed and validated by the helpers declared
// below, so that the on-disk format has a single authoritative
// implementation.

#ifndef CMPCOV_RECEIPT_H_
#define CMPCOV_RECEIPT_H_

#include <cstdint>
#include <set>
#include <string>

// A single comparison trace in receipt form. The offset is a module-relative
// instruction offset with no trace tags folded into it; arg1/arg2 are the two
// comparison labels kept separately, exactly as passed to TrySaveTrace().
struct ReceiptTrace {
  std::string build;  // 64 lowercase hex characters.
  uint64_t offset;    // Unprefixed lowercase hexadecimal in receipts.
  uint64_t arg1;      // Unprefixed, no leading zeros in receipts.
  uint64_t arg2;

  bool operator<(const ReceiptTrace& other) const {
    if (build != other.build) return build < other.build;
    if (offset != other.offset) return offset < other.offset;
    if (arg1 != other.arg1) return arg1 < other.arg1;
    return arg2 < other.arg2;
  }
  bool operator==(const ReceiptTrace& other) const {
    return build == other.build && offset == other.offset &&
           arg1 == other.arg1 && arg2 == other.arg2;
  }
};

// A fully parsed receipt (either a per-round CMPCOV-ROUND file or a
// CMPCOV-MERGED union). Module and trace records are stored already sorted.
struct Receipt {
  std::string magic;                 // "CMPCOV-ROUND" or "CMPCOV-MERGED".
  int version = 0;                   // Always 1 for the current format.
  std::string source;                // 40 lowercase hex characters.
  uint64_t round = 0;                // Round receipts only; 0 for merges.
  bool has_round = false;
  std::string input;                 // 64 lowercase hex characters.
  bool has_input = false;            // Round receipts only.
  std::set<std::string> modules;     // Sorted, deduplicated build IDs.
  std::set<ReceiptTrace> traces;     // Sorted, deduplicated.
};

// Validates a 40-character lowercase hex source identifier.
bool IsValidSourceId(const std::string& source);

// Validates a 64-character lowercase hex build/input identifier.
bool IsValidBuildId(const std::string& build);

// Renders an unsigned value as an unprefixed lowercase hexadecimal string.
std::string HexUint(uint64_t value);

// Renders an unsigned value as a decimal string without leading zeros.
std::string DecUint(uint64_t value);

// Parses an unprefixed lowercase hexadecimal string. Empty strings, embedded
// NULs and any non-[0-9a-f] character make the parse fail.
bool ParseHexUint(const std::string& text, uint64_t* value);

// Parses an unprefixed decimal string with no leading zeros (except for the
// single digit "0").
bool ParseDecUint(const std::string& text, uint64_t* value);

// Serializes a receipt to its canonical textual representation. Every line is
// terminated with '\n'; there is no trailing whitespace, no absolute paths,
// base addresses or process IDs.
std::string SerializeReceipt(const Receipt& receipt);

// Strictly parses a complete receipt. Bad fields, unknown versions,
// undeclared modules, duplicate or out-of-order records and truncated files
// are all rejected. The magic line determines which variant is expected.
bool ParseReceipt(const std::string& contents, Receipt* receipt);

// Atomically publishes a receipt at path. The full contents are first written
// to a temporary file in the same directory and then renamed into place. If
// the destination already exists with identical contents, the call succeeds
// idempotently; different existing contents are rejected. On any failure the
// previously existing file is left untouched.
bool PublishReceipt(const std::string& path, const std::string& contents);

// C ABI wrappers around the round journal owned by the global Traces
// collector in cmpcov.cc. Null pointers or malformed identifiers fail without
// changing any state. All four return a success indicator.
extern "C" {
bool CmpCovBeginRound(const char* source, uint64_t round, const char* input);
bool CmpCovBindModule(const void* pc, const char* build);
bool CmpCovCommitRound(const char* path);
bool CmpCovAbortRound();
}

#endif  // CMPCOV_RECEIPT_H_
