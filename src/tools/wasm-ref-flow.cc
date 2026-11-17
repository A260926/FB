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

// Read-only command line consumer of the function-reference flow analysis.
// It never instantiates or executes the module: it reads and validates a
// binary, runs BuildReferenceFlow, and prints one deterministic JSON object
// per context followed by a roots object.

#include <cstdio>
#include <string>
#include <vector>

#include "wabt/binary-reader-ir.h"
#include "wabt/binary-reader.h"
#include "wabt/error-formatter.h"
#include "wabt/ir.h"
#include "wabt/reference-flow.h"
#include "wabt/validator.h"

namespace {

using namespace wabt;

std::string Bool(bool value) { return value ? "true" : "false"; }

std::string IndexList(const std::vector<Index>& values) {
  std::string out = "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) out += ",";
    out += std::to_string(values[i]);
  }
  out += "]";
  return out;
}

std::string SizeList(const std::vector<size_t>& values) {
  std::string out = "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) out += ",";
    out += std::to_string(values[i]);
  }
  out += "]";
  return out;
}

std::string RefValueJson(const RefValue& value) {
  return "{\"functions\":" + IndexList(value.functions) +
         ",\"may_be_null\":" + Bool(value.may_be_null) +
         ",\"external\":" + Bool(value.external) + "}";
}

std::string RefValueListJson(const std::vector<RefValue>& values) {
  std::string out = "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) out += ",";
    out += RefValueJson(values[i]);
  }
  out += "]";
  return out;
}

std::string CallJson(const FlowCall& call) {
  return "{\"expr_ordinal\":" + std::to_string(call.expr_ordinal) +
         ",\"indirect\":" + Bool(call.indirect) +
         ",\"targets\":" + IndexList(call.targets) +
         ",\"callee_contexts\":" + SizeList(call.callee_contexts) +
         ",\"external\":" + Bool(call.external) +
         ",\"may_trap_null\":" + Bool(call.may_trap_null) + "}";
}

std::string CallListJson(const std::vector<FlowCall>& calls) {
  std::string out = "[";
  for (size_t i = 0; i < calls.size(); ++i) {
    if (i) out += ",";
    out += CallJson(calls[i]);
  }
  out += "]";
  return out;
}

void PrintUsage() {
  std::fprintf(stderr,
               "usage: wasm-ref-flow FILE --root N [--root N ...]\n");
}

}  // namespace

int main(int argc, char** argv) {
  InitStdio();

  std::string filename;
  std::vector<Index> roots;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--root") {
      if (i + 1 >= argc) {
        PrintUsage();
        return 1;
      }
      char* end = nullptr;
      unsigned long long value = std::strtoull(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0') {
        std::fprintf(stderr, "invalid --root index: %s\n", argv[i]);
        return 1;
      }
      roots.push_back(static_cast<Index>(value));
    } else if (!arg.empty() && arg[0] == '-') {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      PrintUsage();
      return 1;
    } else if (filename.empty()) {
      filename = arg;
    } else {
      std::fprintf(stderr, "unexpected argument: %s\n", arg.c_str());
      PrintUsage();
      return 1;
    }
  }

  if (filename.empty() || roots.empty()) {
    PrintUsage();
    return 1;
  }

  Features features;
  features.enable_function_references();

  std::vector<uint8_t> file_data;
  Errors errors;
  Result result = ReadFile(filename.c_str(), &file_data);
  if (Failed(result)) {
    std::fprintf(stderr, "error: unable to read %s\n", filename.c_str());
    return 1;
  }

  Module module;
  ReadBinaryOptions read_options(features, nullptr, false, true, true);
  result = ReadBinaryIr(filename.c_str(), file_data, read_options, &errors,
                        &module);
  if (Succeeded(result)) {
    result = ValidateModule(&module, &errors, ValidateOptions(features));
  }
  if (Failed(result)) {
    FormatErrorsToFile(errors, Location::Type::Binary);
    return 1;
  }

  ReferenceFlow flow;
  if (Failed(BuildReferenceFlow(module, roots, &flow, &errors))) {
    FormatErrorsToFile(errors, Location::Type::Binary);
    return 1;
  }

  for (size_t i = 0; i < flow.contexts.size(); ++i) {
    const FlowContext& context = flow.contexts[i];
    std::printf(
        "{\"context\":%zu,\"function\":%u,\"arguments\":%s,\"may_return\":%s,"
        "\"results\":%s,\"calls\":%s}\n",
        i, context.function, RefValueListJson(context.arguments).c_str(),
        Bool(context.may_return).c_str(),
        RefValueListJson(context.results).c_str(),
        CallListJson(context.calls).c_str());
  }
  std::printf("{\"roots\":%s}\n", SizeList(flow.roots).c_str());
  return 0;
}
