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

#include "receipt.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common.h"

namespace {

constexpr char kRoundMagic[] = "CMPCOV-ROUND 1";
constexpr char kMergedMagic[] = "CMPCOV-MERGED 1";

bool IsLowerHex(const std::string& text, size_t expected_length) {
  if (text.size() != expected_length) {
    return false;
  }
  for (char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// Splits a single line into space-separated fields. A trailing field is not
// allowed, so "trace x y z " (trailing space) is rejected.
bool SplitFields(const std::string& line,
                 std::vector<std::string>* fields) {
  fields->clear();
  size_t pos = 0;
  while (pos < line.size()) {
    size_t next = line.find(' ', pos);
    std::string field = line.substr(
        pos, next == std::string::npos ? std::string::npos : next - pos);
    if (field.empty()) {
      return false;
    }
    fields->push_back(field);
    if (next == std::string::npos) {
      break;
    }
    pos = next + 1;
  }
  return !fields->empty();
}

}  // namespace

bool IsValidSourceId(const std::string& source) {
  return IsLowerHex(source, 40);
}

bool IsValidBuildId(const std::string& build) {
  return IsLowerHex(build, 64);
}

std::string HexUint(uint64_t value) {
  char buffer[17];
  snprintf(buffer, sizeof(buffer), "%" PRIx64, value);
  return std::string(buffer);
}

std::string DecUint(uint64_t value) {
  char buffer[21];
  snprintf(buffer, sizeof(buffer), "%" PRIu64, value);
  return std::string(buffer);
}

bool ParseHexUint(const std::string& text, uint64_t* value) {
  if (text.empty() || text.size() > 16) {
    return false;
  }
  uint64_t result = 0;
  for (char c : text) {
    int digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else {
      return false;
    }
    result = result * 16 + static_cast<uint64_t>(digit);
  }
  *value = result;
  return true;
}

bool ParseDecUint(const std::string& text, uint64_t* value) {
  if (text.empty() || text.size() > 20) {
    return false;
  }
  if (text.size() > 1 && text[0] == '0') {
    return false;
  }
  uint64_t result = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    uint64_t digit = static_cast<uint64_t>(c - '0');
    if (result > (UINT64_MAX - digit) / 10) {
      return false;
    }
    result = result * 10 + digit;
  }
  *value = result;
  return true;
}

std::string SerializeReceipt(const Receipt& receipt) {
  std::string out;
  if (receipt.magic == "CMPCOV-ROUND") {
    out += kRoundMagic;
  } else {
    out += kMergedMagic;
  }
  out.push_back('\n');

  out += "source ";
  out += receipt.source;
  out.push_back('\n');

  if (receipt.magic == "CMPCOV-ROUND") {
    out += "round ";
    out += DecUint(receipt.round);
    out.push_back('\n');
  }

  if (receipt.has_input) {
    out += "input ";
    out += receipt.input;
    out.push_back('\n');
  }

  for (const std::string& build : receipt.modules) {
    out += "module ";
    out += build;
    out.push_back('\n');
  }

  for (const ReceiptTrace& trace : receipt.traces) {
    out += "trace ";
    out += trace.build;
    out.push_back(' ');
    out += HexUint(trace.offset);
    out.push_back(' ');
    out += DecUint(trace.arg1);
    out.push_back(' ');
    out += DecUint(trace.arg2);
    out.push_back('\n');
  }

  return out;
}

bool ParseReceipt(const std::string& contents, Receipt* receipt) {
  *receipt = Receipt();

  // The file must end with a newline; truncation is otherwise ambiguous with
  // a complete last record.
  if (contents.empty() || contents.back() != '\n') {
    return false;
  }

  std::vector<std::string> lines;
  {
    std::string current;
    for (char c : contents) {
      if (c == '\n') {
        lines.push_back(current);
        current.clear();
      } else {
        current.push_back(c);
      }
    }
  }

  size_t line_index = 0;
  std::vector<std::string> fields;

  // First line identifies the variant and its version.
  if (!SplitFields(lines[line_index], &fields) || fields.size() != 2) {
    return false;
  }
  if (fields[0] == "CMPCOV-ROUND") {
    receipt->magic = "CMPCOV-ROUND";
  } else if (fields[0] == "CMPCOV-MERGED") {
    receipt->magic = "CMPCOV-MERGED";
  } else {
    return false;
  }
  if (fields[1] != "1") {
    return false;
  }
  receipt->version = 1;
  ++line_index;

  // source HEX
  if (line_index >= lines.size() ||
      !SplitFields(lines[line_index], &fields) || fields.size() != 2 ||
      fields[0] != "source" || !IsValidSourceId(fields[1])) {
    return false;
  }
  receipt->source = fields[1];
  ++line_index;

  // round DEC (round receipts only)
  if (receipt->magic == "CMPCOV-ROUND") {
    if (line_index >= lines.size() ||
        !SplitFields(lines[line_index], &fields) || fields.size() != 2 ||
        fields[0] != "round" || !ParseDecUint(fields[1], &receipt->round) ||
        receipt->round == 0) {
      return false;
    }
    receipt->has_round = true;
    ++line_index;
  }

  // input HEX (round receipts only; merged receipts carry no input line).
  if (receipt->magic == "CMPCOV-ROUND") {
    if (line_index >= lines.size() ||
        !SplitFields(lines[line_index], &fields) || fields.size() != 2 ||
        fields[0] != "input" || !IsValidBuildId(fields[1])) {
      return false;
    }
    receipt->input = fields[1];
    receipt->has_input = true;
    ++line_index;
  } else if (line_index < lines.size()) {
    if (!SplitFields(lines[line_index], &fields) ||
        fields[0] == "input") {
      return false;
    }
  }

  // module HEX records, strictly sorted and deduplicated in file order.
  std::string previous_module;
  while (line_index < lines.size()) {
    if (!SplitFields(lines[line_index], &fields) || fields.size() != 2 ||
        fields[0] != "module") {
      break;
    }
    if (!IsValidBuildId(fields[1])) {
      return false;
    }
    if (!previous_module.empty() && fields[1] <= previous_module) {
      // Out-of-order or duplicate record.
      return false;
    }
    previous_module = fields[1];
    receipt->modules.insert(fields[1]);
    ++line_index;
  }
  // A round with no bindings carries no module lines; that is structurally
  // valid, though any trace would still require a declared module.

  // trace BUILD OFFSET ARG1 ARG2 records, strictly sorted and deduplicated.
  bool have_previous_trace = false;
  ReceiptTrace previous_trace;
  while (line_index < lines.size()) {
    if (!SplitFields(lines[line_index], &fields) || fields.size() != 5 ||
        fields[0] != "trace") {
      return false;
    }
    ReceiptTrace trace;
    trace.build = fields[1];
    if (!IsValidBuildId(trace.build) ||
        !receipt->modules.count(trace.build) ||
        !ParseHexUint(fields[2], &trace.offset) ||
        !ParseDecUint(fields[3], &trace.arg1) ||
        !ParseDecUint(fields[4], &trace.arg2)) {
      return false;
    }
    if (have_previous_trace && !(previous_trace < trace)) {
      // Out-of-order or duplicate record.
      return false;
    }
    previous_trace = trace;
    have_previous_trace = true;
    receipt->traces.insert(trace);
    ++line_index;
  }

  return line_index == lines.size();
}

bool PublishReceipt(const std::string& path, const std::string& contents) {
  // An identical destination is an idempotent success; a different one must
  // never be overwritten.
  {
    std::ifstream existing(path.c_str(), std::ios::binary);
    if (existing.good() || existing.eof()) {
      std::ostringstream buffer;
      buffer << existing.rdbuf();
      if (existing.bad()) {
        return false;
      }
      // An identical destination is an idempotent success; a different one
      // must never be overwritten.
      return buffer.str() == contents;
    }
#ifdef _WIN32
    if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      // The file exists but cannot be read; do not attempt to replace it.
      return false;
    }
#else
    if (access(path.c_str(), F_OK) == 0) {
      // The file exists but cannot be read; do not attempt to replace it.
      return false;
    }
#endif
  }

  // Publish through a temporary file in the same directory so that the final
  // rename is atomic. The counter keeps rapid commits inside one process from
  // reusing a still-present temporary name.
  static uint64_t temp_counter = 0;
  std::string temp_path = path + ".tmp." +
                          DecUint(static_cast<uint64_t>(GetPid())) + "." +
                          DecUint(temp_counter++);
  {
    std::ofstream temp(temp_path.c_str(),
                       std::ios::binary | std::ios::trunc);
    if (!temp.good()) {
      remove(temp_path.c_str());
      return false;
    }
    temp.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    temp.flush();
    if (!temp.good()) {
      temp.close();
      remove(temp_path.c_str());
      return false;
    }
    temp.close();
    if (!temp) {
      remove(temp_path.c_str());
      return false;
    }
  }

  if (rename(temp_path.c_str(), path.c_str()) != 0) {
    remove(temp_path.c_str());
    return false;
  }
  return true;
}
