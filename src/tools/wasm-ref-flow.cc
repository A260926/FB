/*
 * Copyright 2024 WebAssembly Community Group participants
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

// wasm-ref-flow: read-only function-reference flow consumer.
//
// Usage:
//   wasm-ref-flow FILE --root N [--root N ...]
//
// Reads a WebAssembly binary, validates it, runs BuildReferenceFlow and
// prints deterministic JSON: one context object per line (in report order)
// followed by a final {"roots":[...]} line.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "wabt/binary-reader-ir.h"
#include "wabt/binary-reader.h"
#include "wabt/common.h"
#include "wabt/error-formatter.h"
#include "wabt/ir.h"
#include "wabt/reference-flow.h"
#include "wabt/validator.h"

using namespace wabt;

namespace {

std::string g_infile;
std::vector<Index> g_roots;

const char* kDescription =
    "  Read a WebAssembly binary and print function-reference flow summaries.\n"
    "\n"
    "examples:\n"
    "  $ wasm-ref-flow module.wasm --root 4 --root 5\n";

void Usage(FILE* out) {
  std::fprintf(out, "wasm-ref-flow FILE --root N [--root N ...]\n%s\n",
               kDescription);
}

bool ParseRoot(const std::string& value, Index* out) {
  if (value.empty()) {
    return false;
  }
  char* end = nullptr;
  unsigned long long n = std::strtoull(value.c_str(), &end, 10);
  if (end != value.c_str() + value.size() || n > kInvalidIndex) {
    return false;
  }
  *out = static_cast<Index>(n);
  return true;
}

int ParseArgs(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      Usage(stdout);
      return 1;
    }
    if (arg == "--root") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: --root requires an index\n");
        return 2;
      }
      Index root;
      if (!ParseRoot(argv[++i], &root)) {
        std::fprintf(stderr, "error: invalid --root index '%s'\n", argv[i]);
        return 2;
      }
      g_roots.push_back(root);
    } else if (arg.rfind("--root=", 0) == 0) {
      Index root;
      if (!ParseRoot(arg.substr(7), &root)) {
        std::fprintf(stderr, "error: invalid --root index '%s'\n",
                     arg.substr(7).c_str());
        return 2;
      }
      g_roots.push_back(root);
    } else if (!arg.empty() && arg[0] == '-') {
      std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
      return 2;
    } else if (!g_infile.empty()) {
      std::fprintf(stderr, "error: unexpected argument '%s'\n", arg.c_str());
      return 2;
    } else {
      g_infile = arg;
    }
  }
  if (g_infile.empty()) {
    Usage(stderr);
    return 2;
  }
  return 0;
}

void PrintIndexArray(const std::vector<Index>& values) {
  std::putchar('[');
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) {
      std::putchar(',');
    }
    std::printf("%u", static_cast<unsigned>(values[i]));
  }
  std::putchar(']');
}

void PrintContextIndexArray(const std::vector<size_t>& values) {
  std::putchar('[');
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) {
      std::putchar(',');
    }
    std::printf("%zu", values[i]);
  }
  std::putchar(']');
}

void PrintRefValue(const RefValue& v) {
  std::fputs("{\"functions\":", stdout);
  PrintIndexArray(v.functions);
  std::fprintf(stdout, ",\"may_be_null\":%s,\"external\":%s}",
               v.may_be_null ? "true" : "false",
               v.external ? "true" : "false");
}

void PrintRefValueArray(const std::vector<RefValue>& values) {
  std::putchar('[');
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) {
      std::putchar(',');
    }
    PrintRefValue(values[i]);
  }
  std::putchar(']');
}

void PrintCall(const FlowCall& c) {
  std::fputs("{\"expr_ordinal\":", stdout);
  std::printf("%zu", c.expr_ordinal);
  std::fprintf(stdout, ",\"indirect\":%s", c.indirect ? "true" : "false");
  std::fputs(",\"targets\":", stdout);
  PrintIndexArray(c.targets);
  std::fputs(",\"callee_contexts\":", stdout);
  PrintContextIndexArray(c.callee_contexts);
  std::fprintf(stdout, ",\"external\":%s,\"may_trap_null\":%s}",
               c.external ? "true" : "false",
               c.may_trap_null ? "true" : "false");
}

void PrintContextLine(size_t index, const FlowContext& ctx) {
  std::fputs("{\"context\":", stdout);
  std::printf("%zu", index);
  std::fputs(",\"function\":", stdout);
  std::printf("%u", static_cast<unsigned>(ctx.function));
  std::fputs(",\"arguments\":", stdout);
  PrintRefValueArray(ctx.arguments);
  std::fprintf(stdout, ",\"may_return\":%s",
               ctx.may_return ? "true" : "false");
  std::fputs(",\"results\":", stdout);
  PrintRefValueArray(ctx.results);
  std::fputs(",\"calls\":[", stdout);
  for (size_t i = 0; i < ctx.calls.size(); ++i) {
    if (i) {
      std::putchar(',');
    }
    PrintCall(ctx.calls[i]);
  }
  std::fputs("]}\n", stdout);
}

}  // namespace

int ProgramMain(int argc, char** argv) {
  InitStdio();

  int parse_status = ParseArgs(argc, argv);
  if (parse_status != 0) {
    return parse_status == 1 ? 0 : parse_status;
  }

  std::vector<uint8_t> file_data;
  Result result = ReadFile(g_infile.c_str(), &file_data);
  if (Failed(result)) {
    std::fprintf(stderr, "wasm-ref-flow: unable to read '%s'\n",
                 g_infile.c_str());
    return 1;
  }

  Features features;
  features.enable_function_references();

  Errors errors;
  Module module;
  ReadBinaryOptions options(features, nullptr, false, true, true);
  result = ReadBinaryIr(g_infile.c_str(), file_data.data(), file_data.size(),
                        options, &errors, &module);
  if (Succeeded(result)) {
    result = ValidateModule(&module, &errors, ValidateOptions(features));
  }

  ReferenceFlow flow;
  if (Succeeded(result)) {
    result = BuildReferenceFlow(module, g_roots, &flow, &errors);
  }

  if (Failed(result)) {
    FormatErrorsToFile(errors, Location::Type::Binary);
    std::fprintf(stderr, "wasm-ref-flow: analysis failed for '%s'\n",
                 g_infile.c_str());
    return 1;
  }

  for (size_t i = 0; i < flow.contexts.size(); ++i) {
    PrintContextLine(i, flow.contexts[i]);
  }
  std::fputs("{\"roots\":", stdout);
  PrintContextIndexArray(flow.roots);
  std::fputs("}\n", stdout);
  return 0;
}

int main(int argc, char** argv) {
  WABT_TRY
  return ProgramMain(argc, argv);
  WABT_CATCH_BAD_ALLOC_AND_EXIT
}
