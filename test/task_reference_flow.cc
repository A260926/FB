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

// Task tests for the read-only function-reference flow analysis. There is no
// main(): the cases are registered through task_support/check.h and driven by
// task_support/driver.c. Every module is synthesized from WAT, parsed,
// validated, written to binary, read back with ReadBinaryIr and validated
// again before being analyzed; nothing is instantiated or executed.

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "task_support/check.h"
#include "task_support/fixture.h"
#include "wabt/binary-writer.h"
#include "wabt/error-formatter.h"
#include "wabt/reference-flow.h"
#include "wabt/stream.h"

namespace {

using namespace wabt;

// ---------------------------------------------------------------------------
// Small construction / lookup helpers
// ---------------------------------------------------------------------------

struct Loaded {
  std::unique_ptr<Module> module;
  std::vector<uint8_t> bytes;
};

bool LoadModule(const std::string& wat, Loaded* loaded, Errors* errors) {
  loaded->module = std::make_unique<Module>();
  return Succeeded(fixture::RoundTrip(wat, loaded->module.get(), errors,
                                      &loaded->bytes));
}

bool BuildFlow(const Module& module, std::vector<Index> roots,
               ReferenceFlow* out, Errors* errors) {
  return Succeeded(BuildReferenceFlow(module, roots, out, errors));
}

const FlowContext* FindContext(const ReferenceFlow& flow, Index function) {
  for (const FlowContext& context : flow.contexts) {
    if (context.function == function) return &context;
  }
  return nullptr;
}

const FlowContext* FindContextWithArg(const ReferenceFlow& flow,
                                      Index function,
                                      std::initializer_list<Index> arg_funcs) {
  std::vector<Index> wanted(arg_funcs);
  for (const FlowContext& context : flow.contexts) {
    if (context.function != function || context.arguments.size() != 1) {
      continue;
    }
    if (context.arguments[0].functions == wanted) return &context;
  }
  return nullptr;
}

const FlowCall* FindCall(const FlowContext& context, size_t ordinal) {
  for (const FlowCall& call : context.calls) {
    if (call.expr_ordinal == ordinal) return &call;
  }
  return nullptr;
}

bool SameFunctions(const RefValue& value,
                   std::initializer_list<Index> functions) {
  return value.functions == std::vector<Index>(functions);
}

bool SameFunctions(const std::vector<Index>& actual,
                   std::initializer_list<Index> functions) {
  return actual == std::vector<Index>(functions);
}

bool IsBottomValue(const RefValue& value) {
  return value.functions.empty() && !value.may_be_null && !value.external;
}

// ---------------------------------------------------------------------------
// Minimal deterministic JSON parser for the consumer output
// ---------------------------------------------------------------------------

struct Json {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool boolean = false;
  double number = 0;
  std::string text;
  std::vector<Json> elements;
  std::vector<std::pair<std::string, Json>> members;

  const Json* Find(const std::string& key) const {
    for (const auto& member : members) {
      if (member.first == key) return &member.second;
    }
    return nullptr;
  }
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& input) : input_(input) {}

  bool Parse(Json* out) {
    Skip();
    if (!Value(out)) return false;
    Skip();
    return pos_ == input_.size();
  }

 private:
  void Skip() {
    while (pos_ < input_.size() &&
           (input_[pos_] == ' ' || input_[pos_] == '\t' ||
            input_[pos_] == '\r' || input_[pos_] == '\n')) {
      ++pos_;
    }
  }

  bool Value(Json* out) {
    Skip();
    if (pos_ >= input_.size()) return false;
    char c = input_[pos_];
    if (c == '{') return Object(out);
    if (c == '[') return Array(out);
    if (c == '"') return String(out);
    if (input_.compare(pos_, 4, "true") == 0) {
      out->type = Json::Bool;
      out->boolean = true;
      pos_ += 4;
      return true;
    }
    if (input_.compare(pos_, 5, "false") == 0) {
      out->type = Json::Bool;
      out->boolean = false;
      pos_ += 5;
      return true;
    }
    if (input_.compare(pos_, 4, "null") == 0) {
      out->type = Json::Null;
      pos_ += 4;
      return true;
    }
    return Number(out);
  }

  bool Number(Json* out) {
    size_t start = pos_;
    while (pos_ < input_.size() &&
           (std::isdigit(static_cast<unsigned char>(input_[pos_])) ||
            input_[pos_] == '-' || input_[pos_] == '+')) {
      ++pos_;
    }
    if (start == pos_) return false;
    out->type = Json::Number;
    out->number = std::strtod(input_.c_str() + start, nullptr);
    return true;
  }

  bool String(Json* out) {
    ++pos_;  // opening quote
    std::string value;
    while (pos_ < input_.size() && input_[pos_] != '"') {
      if (input_[pos_] == '\\' && pos_ + 1 < input_.size()) {
        value.push_back(input_[pos_ + 1]);
        pos_ += 2;
      } else {
        value.push_back(input_[pos_++]);
      }
    }
    if (pos_ >= input_.size()) return false;
    ++pos_;  // closing quote
    out->type = Json::String;
    out->text = std::move(value);
    return true;
  }

  bool Array(Json* out) {
    out->type = Json::Array;
    ++pos_;
    Skip();
    if (pos_ < input_.size() && input_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (pos_ < input_.size()) {
      Json element;
      if (!Value(&element)) return false;
      out->elements.push_back(std::move(element));
      Skip();
      if (pos_ >= input_.size()) return false;
      if (input_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (input_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return false;
    }
    return false;
  }

  bool Object(Json* out) {
    out->type = Json::Object;
    ++pos_;
    Skip();
    if (pos_ < input_.size() && input_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (pos_ < input_.size()) {
      Skip();
      Json key;
      if (!String(&key)) return false;
      Skip();
      if (pos_ >= input_.size() || input_[pos_] != ':') return false;
      ++pos_;
      Json value;
      if (!Value(&value)) return false;
      out->members.emplace_back(key.text, std::move(value));
      Skip();
      if (pos_ >= input_.size()) return false;
      if (input_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (input_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return false;
    }
    return false;
  }

  const std::string& input_;
  size_t pos_ = 0;
};

bool JsonEqual(const Json& a, const Json& b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case Json::Null:
      return true;
    case Json::Bool:
      return a.boolean == b.boolean;
    case Json::Number:
      return a.number == b.number;
    case Json::String:
      return a.text == b.text;
    case Json::Array:
      if (a.elements.size() != b.elements.size()) return false;
      for (size_t i = 0; i < a.elements.size(); ++i) {
        if (!JsonEqual(a.elements[i], b.elements[i])) return false;
      }
      return true;
    case Json::Object:
      if (a.members.size() != b.members.size()) return false;
      for (const auto& member : a.members) {
        const Json* other = b.Find(member.first);
        if (!other || !JsonEqual(member.second, *other)) return false;
      }
      return true;
  }
  return false;
}

const char* ConsumerPath() {
  static const char* candidates[] = {
      ".task-build/wabt/wasm-ref-flow",
      "./wasm-ref-flow",
  };
  for (const char* path : candidates) {
    if (access(path, X_OK) == 0) return path;
  }
  return nullptr;
}

int RunConsumer(const std::string& wasm_path,
                const std::vector<Index>& roots,
                std::vector<Json>* lines,
                bool capture_stderr = false) {
  const char* binary = ConsumerPath();
  if (!binary) return -1;
  std::string command = std::string(binary) + " " + wasm_path;
  for (Index root : roots) {
    command += " --root " + std::to_string(root);
  }
  if (capture_stderr) command += " 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) return -1;
  char buffer[512];
  std::string output;
  while (std::fgets(buffer, sizeof(buffer), pipe)) output += buffer;
  int status = pclose(pipe);
  if (status == -1) return -1;
  lines->clear();
  size_t start = 0;
  while (start < output.size()) {
    size_t end = output.find('\n', start);
    std::string line = output.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (!line.empty()) {
      Json json;
      JsonParser parser(line);
      if (!parser.Parse(&json)) return -2;
      lines->push_back(std::move(json));
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
#ifdef _WIN32
  return status;
#else
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;
#endif
}

std::string WriteTempWasm(const std::vector<uint8_t>& bytes,
                          const char* name) {
  std::string path = std::string(".task-build/") + name;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return path;
}

// Deep comparison of one emitted context JSON line against the library
// context at the same sorted index.
bool JsonMatchesContext(const Json& json, const FlowContext& context) {
  if (json.type != Json::Object) return false;
  const Json* function = json.Find("function");
  if (!function || function->number != context.function) return false;
  const Json* may_return = json.Find("may_return");
  if (!may_return || may_return->boolean != context.may_return) return false;

  const Json* arguments = json.Find("arguments");
  if (!arguments || arguments->elements.size() != context.arguments.size()) {
    return false;
  }
  const Json* results = json.Find("results");
  if (!results || results->elements.size() != context.results.size()) {
    return false;
  }
  for (size_t i = 0; i < context.arguments.size(); ++i) {
    const Json& value = arguments->elements[i];
    const RefValue& expected = context.arguments[i];
    const Json* fns = value.Find("functions");
    if (!fns || fns->elements.size() != expected.functions.size()) return false;
    for (size_t j = 0; j < expected.functions.size(); ++j) {
      if (static_cast<Index>(fns->elements[j].number) !=
          expected.functions[j]) {
        return false;
      }
    }
    if (value.Find("may_be_null")->boolean != expected.may_be_null) {
      return false;
    }
    if (value.Find("external")->boolean != expected.external) return false;
  }
  for (size_t i = 0; i < context.results.size(); ++i) {
    const Json& value = results->elements[i];
    const RefValue& expected = context.results[i];
    const Json* fns = value.Find("functions");
    if (!fns || fns->elements.size() != expected.functions.size()) return false;
    for (size_t j = 0; j < expected.functions.size(); ++j) {
      if (static_cast<Index>(fns->elements[j].number) !=
          expected.functions[j]) {
        return false;
      }
    }
    if (value.Find("may_be_null")->boolean != expected.may_be_null) {
      return false;
    }
    if (value.Find("external")->boolean != expected.external) return false;
  }

  const Json* calls = json.Find("calls");
  if (!calls || calls->elements.size() != context.calls.size()) return false;
  for (size_t i = 0; i < context.calls.size(); ++i) {
    const Json& call = calls->elements[i];
    const FlowCall& expected = context.calls[i];
    if (static_cast<size_t>(call.Find("expr_ordinal")->number) !=
        expected.expr_ordinal) {
      return false;
    }
    if (call.Find("indirect")->boolean != expected.indirect) return false;
    if (call.Find("external")->boolean != expected.external) return false;
    if (call.Find("may_trap_null")->boolean != expected.may_trap_null) {
      return false;
    }
    const Json& targets = *call.Find("targets");
    const Json& callee_contexts = *call.Find("callee_contexts");
    if (targets.elements.size() != expected.targets.size() ||
        callee_contexts.elements.size() !=
            expected.callee_contexts.size()) {
      return false;
    }
    for (size_t j = 0; j < expected.targets.size(); ++j) {
      if (static_cast<Index>(targets.elements[j].number) !=
          expected.targets[j]) {
        return false;
      }
    }
    for (size_t j = 0; j < expected.callee_contexts.size(); ++j) {
      if (static_cast<size_t>(callee_contexts.elements[j].number) !=
          expected.callee_contexts[j]) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

// ===========================================================================
// contexts group: helper separation, ref return then call_ref, import
// prefix, top values, unsupported-node rejection.
// ===========================================================================

static int ContextsSeparation(void) {
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (type $rt (func (param (ref null $cb)) (result (ref null $cb))))
      (import "env" "imp" (func $imp (type $rt)))
      (func $a)
      (func $b)
      (elem declare func $a $b)
      (func $relay (type $rt) local.get 0)
      (func $left ref.func $a call $relay call_ref $cb)
      (func $right ref.func $b call $relay call_ref $cb)
      (export "left" (func $left))))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  // Indices: imp=0, a=1, b=2, relay=3, left=4, right=5.
  ReferenceFlow flow;
  TASK_CHECK(BuildFlow(*loaded.module, {4, 5}, &flow, &errors));

  const FlowContext* left = FindContext(flow, 4);
  const FlowContext* right = FindContext(flow, 5);
  TASK_CHECK(left && right);
  const FlowCall* left_call = FindCall(*left, 2);
  const FlowCall* right_call = FindCall(*right, 2);
  TASK_CHECK(left_call && right_call);
  TASK_CHECK(left_call->indirect && right_call->indirect);
  // The same helper fed different refs must not merge candidate targets.
  TASK_CHECK(SameFunctions(left_call->targets, {1}));
  TASK_CHECK(SameFunctions(right_call->targets, {2}));
  TASK_CHECK(!left_call->may_trap_null && !right_call->may_trap_null);

  const FlowContext* relay_a = FindContextWithArg(flow, 3, {1});
  const FlowContext* relay_b = FindContextWithArg(flow, 3, {2});
  TASK_CHECK(relay_a && relay_b);
  TASK_CHECK(relay_a != relay_b);
  TASK_CHECK(relay_a->may_return && relay_b->may_return);
  TASK_CHECK(SameFunctions(relay_a->results[0], {1}));
  TASK_CHECK(SameFunctions(relay_b->results[0], {2}));

  // Roots are sorted and deduplicated and reference the emitted contexts.
  TASK_CHECK(flow.roots.size() == 2);
  TASK_CHECK(flow.contexts[flow.roots[0]].function == 4);
  TASK_CHECK(flow.contexts[flow.roots[1]].function == 5);
  return 0;
}

static int ContextsTopAndImportPrefix(void) {
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (type $rt (func (param (ref null $cb)) (result (ref null $cb))))
      (import "env" "imp" (func $imp (type $rt)))
      (func $a)
      (func $b)
      (elem declare func $a $b)
      (func $toproot (param (ref null $cb)) local.get 0 call_ref $cb)))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  // Indices: imp=0 (different signature), a=1, b=2, toproot=3.
  ReferenceFlow flow;
  TASK_CHECK(BuildFlow(*loaded.module, {3}, &flow, &errors));

  const FlowContext* root = FindContext(flow, 3);
  TASK_CHECK(root && root->arguments.size() == 1);
  const RefValue& top = root->arguments[0];
  // Top contains every structurally compatible module function, the null
  // component for the nullable parameter and an external possibility.
  TASK_CHECK(SameFunctions(top, {1, 2}));
  TASK_CHECK(top.may_be_null && top.external);

  const FlowCall* call = FindCall(*root, 1);
  TASK_CHECK(call && call->indirect);
  TASK_CHECK(SameFunctions(call->targets, {1, 2}));
  TASK_CHECK(call->external && call->may_trap_null);
  TASK_CHECK(call->callee_contexts.size() == 2);
  // The import keeps the function-0 index prefix and is not reported as a
  // candidate because its signature does not match $cb.
  TASK_CHECK(loaded.module->num_func_imports == 1);
  for (Index target : call->targets) TASK_CHECK(target != 0);
  return 0;
}

static int ContextsImportAsRootAndRefFunc(void) {
  static const char* kWat = R"(
    (module
      (type $rt (func (param funcref) (result funcref)))
      (import "env" "imp" (func $imp (type $rt)))
      (func $a)
      (elem declare func $imp $a)
      (func $pick (result funcref) ref.func $imp)
      (func $root ref.func $a call $imp drop)))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  // Indices: imp=0, a=1, pick=2, root=3.
  TASK_CHECK(loaded.module->num_func_imports == 1);

  // The import itself can be a root: it is assumed to return normally and its
  // reference argument/result take top, including its own index.
  ReferenceFlow import_flow;
  TASK_CHECK(BuildFlow(*loaded.module, {0}, &import_flow, &errors));
  const FlowContext* import_ctx = FindContext(import_flow, 0);
  TASK_CHECK(import_ctx && import_ctx->may_return);
  TASK_CHECK(import_ctx->arguments.size() == 1);
  TASK_CHECK(import_ctx->arguments[0].may_be_null &&
             import_ctx->arguments[0].external);
  TASK_CHECK(import_ctx->results[0].may_be_null &&
             import_ctx->results[0].external);

  // ref.func naming an import reports the import's real index rather than the
  // external possibility.
  ReferenceFlow pick_flow;
  TASK_CHECK(BuildFlow(*loaded.module, {2}, &pick_flow, &errors));
  const FlowContext* pick = FindContext(pick_flow, 2);
  TASK_CHECK(pick && pick->results.size() == 1);
  TASK_CHECK(SameFunctions(pick->results[0], {0}));
  TASK_CHECK(!pick->results[0].external && !pick->results[0].may_be_null);

  // A direct call to an import creates a context for the import and consumes
  // its top result.
  ReferenceFlow root_flow;
  TASK_CHECK(BuildFlow(*loaded.module, {3}, &root_flow, &errors));
  const FlowContext* root = FindContext(root_flow, 3);
  TASK_CHECK(root && root->may_return);
  const FlowCall* call = FindCall(*root, 1);
  TASK_CHECK(call && !call->indirect);
  TASK_CHECK(SameFunctions(call->targets, {0}));
  TASK_CHECK(call->callee_contexts.size() == 1);
  const FlowContext& callee =
      root_flow.contexts[call->callee_contexts[0]];
  TASK_CHECK(callee.function == 0 && callee.may_return);
  return 0;
}

static int ContextsUnsupportedRejected(void) {
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (func $loopy (loop (nop)))))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  ReferenceFlow flow;
  TASK_CHECK(!BuildFlow(*loaded.module, {0}, &flow, &errors));
  TASK_CHECK(!errors.empty());
  // The diagnostic names the full function index and the expr ordinal.
  TASK_CHECK(errors.front().message.find("function 0") != std::string::npos);
  TASK_CHECK(errors.front().message.find("expression 0") !=
             std::string::npos);

  // A failed analysis publishes nothing and leaves the output untouched.
  ReferenceFlow sentinel;
  sentinel.roots.push_back(77);
  ReferenceFlow copy = sentinel;
  TASK_CHECK(Failed(BuildReferenceFlow(*loaded.module, {99}, &copy, &errors)));
  TASK_CHECK(copy.roots.size() == 1 && copy.roots[0] == 77);
  return 0;
}

// ===========================================================================
// locals group: strong update, if snapshot/normal-arm merge, select,
// multi-result positions.
// ===========================================================================

static int LocalsStrongUpdate(void) {
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (func $a)
      (func $b)
      (elem declare func $a $b)
      (func $f (local $p (ref null $cb))
        ref.func $a local.set $p
        ref.func $b local.set $p
        local.get $p call_ref $cb)))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  ReferenceFlow flow;
  TASK_CHECK(BuildFlow(*loaded.module, {2}, &flow, &errors));
  const FlowContext* f = FindContext(flow, 2);
  TASK_CHECK(f);
  const FlowCall* call = FindCall(*f, 5);
  TASK_CHECK(call);
  // local.set overwrites: the earlier ref.func $a must not survive.
  // No imports: a=0, b=1, f=2, so the surviving reference is b (index 1).
  TASK_CHECK(SameFunctions(call->targets, {1}));
  TASK_CHECK(!call->may_trap_null);
  return 0;
}

static int LocalsIfMerge(void) {
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (func $a)
      (func $b)
      (elem declare func $a $b)
      (func $g (local $p (ref null $cb))
        i32.const 0
        if
          ref.func $a local.set $p
        else
          ref.func $b local.set $p
        end
        local.get $p call_ref $cb)
      (func $h (local $p (ref null $cb))
        i32.const 0
        if
          ref.func $a local.set $p return
        else
          ref.func $b local.set $p
        end
        local.get $p call_ref $cb)))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  // Indices: a=0, b=1, g=2, h=3.
  ReferenceFlow flow;
  TASK_CHECK(BuildFlow(*loaded.module, {2, 3}, &flow, &errors));

  const FlowContext* g = FindContext(flow, 2);
  TASK_CHECK(g);
  // Ordinals in g: i32.const=0, if=1, true{ref.func=2,local.set=3},
  // false{ref.func=4,local.set=5}, local.get=6, call_ref=7.
  const FlowCall* g_call = FindCall(*g, 7);
  TASK_CHECK(g_call);
  // Both arms complete normally, so the post-if reference joins A and B.
  TASK_CHECK(SameFunctions(g_call->targets, {0, 1}));

  const FlowContext* h = FindContext(flow, 3);
  TASK_CHECK(h);
  // h's true arm also contains a return (ordinal 4), shifting call_ref to 8.
  const FlowCall* h_call = FindCall(*h, 8);
  TASK_CHECK(h_call);
  // The true arm returns and contributes no tail state; only B survives.
  TASK_CHECK(SameFunctions(h_call->targets, {1}));
  return 0;
}

static int LocalsSelect(void) {
  // The numeric type index is used in the typed select: this WABT build does
  // not resolve a *named* type used as a select result type, though it
  // resolves the same name in function and block signatures.
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (func $a)
      (func $b)
      (elem declare func $a $b)
      (func $f
        ref.func $a ref.func $b i32.const 0
        select (result (ref null 0))
        call_ref $cb)))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  ReferenceFlow flow;
  TASK_CHECK(BuildFlow(*loaded.module, {2}, &flow, &errors));
  const FlowContext* f = FindContext(flow, 2);
  TASK_CHECK(f);
  const FlowCall* call = FindCall(*f, 4);
  TASK_CHECK(call);
  // The numeric condition is not evaluated: both candidates are retained.
  // a=0, b=1.
  TASK_CHECK(SameFunctions(call->targets, {0, 1}));
  return 0;
}

static int LocalsRefAsNonNull(void) {
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (func $a)
      (elem declare func $a)
      (func $f
        ref.func $a ref.null $cb i32.const 0
        select (result (ref null 0))
        ref.as_non_null call_ref $cb)))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  // Indices: a=0, f=1.
  ReferenceFlow flow;
  TASK_CHECK(BuildFlow(*loaded.module, {1}, &flow, &errors));
  const FlowContext* f = FindContext(flow, 1);
  TASK_CHECK(f && f->may_return);
  // Ordinals: ref.func=0, ref.null=1, i32.const=2, select=3,
  // ref.as_non_null=4, call_ref=5.
  const FlowCall* call = FindCall(*f, 5);
  TASK_CHECK(call);
  // as_non_null removes the null possibility: only the concrete function
  // remains, and the call can no longer trap on a null reference.
  TASK_CHECK(SameFunctions(call->targets, {0}));
  TASK_CHECK(!call->may_trap_null && !call->external);
  return 0;
}

static int LocalsMultiResult(void) {
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (type $pair (func (result i32 (ref null $cb) (ref null $cb))))
      (func $a)
      (func $b)
      (elem declare func $a $b)
      (func $mk (type $pair) i32.const 7 ref.func $a ref.func $b)
      (func $use (result i32)
        call $mk
        call_ref $cb
        drop)))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  // Indices: a=0, b=1, mk=2, use=3.
  ReferenceFlow flow;
  TASK_CHECK(BuildFlow(*loaded.module, {3}, &flow, &errors));

  const FlowContext* mk = FindContext(flow, 2);
  TASK_CHECK(mk && mk->results.size() == 3);
  TASK_CHECK(IsBottomValue(mk->results[0]));  // scalar i32 position
  TASK_CHECK(SameFunctions(mk->results[1], {0}));
  TASK_CHECK(SameFunctions(mk->results[2], {1}));

  const FlowContext* use = FindContext(flow, 3);
  TASK_CHECK(use && use->may_return);
  // call $mk is ordinal 0; the following call_ref is ordinal 1 and consumes
  // the top (third) reference result, which is b (index 1).
  const FlowCall* call = FindCall(*use, 1);
  TASK_CHECK(call && call->indirect);
  TASK_CHECK(SameFunctions(call->targets, {1}));
  return 0;
}

// ===========================================================================
// recursion group: mutual recursion with an exit, pure recursion, null-only
// call_ref, tail calls, independent repeated analyses.
// ===========================================================================

static int RecursionMutualWithExit(void) {
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (type $rt (func (param (ref null $cb)) (result (ref null $cb))))
      (func $a)
      (elem declare func $a)
      (func $bridge (type $rt)
        i32.const 0
        if (result (ref null $cb))
          local.get 0
        else
          local.get 0 call $peer
        end)
      (func $peer (type $rt) local.get 0 call $bridge)
      (func $root ref.func $a call $peer call_ref $cb)))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  // Indices: a=0, bridge=1, peer=2, root=3.
  ReferenceFlow flow;
  TASK_CHECK(BuildFlow(*loaded.module, {3}, &flow, &errors));

  const FlowContext* bridge = FindContextWithArg(flow, 1, {0});
  const FlowContext* peer = FindContextWithArg(flow, 2, {0});
  TASK_CHECK(bridge && peer);
  // The mutually recursive pair has a real returning arm, so both summaries
  // eventually carry the reference back to the earliest caller.
  TASK_CHECK(bridge->may_return && peer->may_return);
  TASK_CHECK(SameFunctions(bridge->results[0], {0}));
  TASK_CHECK(SameFunctions(peer->results[0], {0}));

  const FlowContext* root = FindContext(flow, 3);
  TASK_CHECK(root);
  const FlowCall* call = FindCall(*root, 2);
  TASK_CHECK(call);
  TASK_CHECK(SameFunctions(call->targets, {0}));
  return 0;
}

static int RecursionPureAndNullOnly(void) {
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (func $b)
      (elem declare func $b)
      (func $spin (result (ref null $cb)) call $spin)
      (func $spin_root call $spin call_ref $cb ref.func $b call_ref $cb)
      (func $null_root ref.null $cb call_ref $cb ref.func $b call_ref $cb)))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  // Indices: b=0, spin=1, spin_root=2, null_root=3.
  ReferenceFlow flow;
  TASK_CHECK(BuildFlow(*loaded.module, {2, 3}, &flow, &errors));

  const FlowContext* spin = FindContext(flow, 1);
  TASK_CHECK(spin);
  // A pure recursion loop never returns normally and does not widen to top.
  TASK_CHECK(!spin->may_return);
  TASK_CHECK(IsBottomValue(spin->results[0]));

  const FlowContext* spin_root = FindContext(flow, 2);
  TASK_CHECK(spin_root && !spin_root->may_return);
  // The non-returning call stops the scan: the later call_ref is not
  // published, and the function has no normal continuation.
  const FlowCall* spin_call = FindCall(*spin_root, 0);
  TASK_CHECK(spin_call && !spin_call->indirect);
  TASK_CHECK(FindCall(*spin_root, 1) == nullptr);
  TASK_CHECK(FindCall(*spin_root, 2) == nullptr);

  const FlowContext* null_root = FindContext(flow, 3);
  TASK_CHECK(null_root && !null_root->may_return);
  // A null-only call_ref keeps one record with the trap flag, but no target.
  const FlowCall* null_call = FindCall(*null_root, 1);
  TASK_CHECK(null_call && null_call->indirect);
  TASK_CHECK(null_call->targets.empty() && null_call->may_trap_null);
  TASK_CHECK(FindCall(*null_root, 2) == nullptr);
  return 0;
}

static int RecursionTailCall(void) {
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (type $rt (func (param (ref null $cb)) (result (ref null $cb))))
      (func $a)
      (elem declare func $a)
      (func $relay (type $rt) local.get 0)
      (func $tail (type $rt) local.get 0 return_call $relay)
      (func $root ref.func $a call $tail call_ref $cb)))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  // Indices: a=0, relay=1, tail=2, root=3.
  ReferenceFlow flow;
  TASK_CHECK(BuildFlow(*loaded.module, {3}, &flow, &errors));

  const FlowContext* tail = FindContextWithArg(flow, 2, {0});
  TASK_CHECK(tail && tail->may_return);
  TASK_CHECK(SameFunctions(tail->results[0], {0}));
  // The tail call is recorded, but nothing follows it in the tail function.
  TASK_CHECK(tail->calls.size() == 1 && !tail->calls[0].indirect);

  const FlowContext* root = FindContext(flow, 3);
  TASK_CHECK(root);
  const FlowCall* call = FindCall(*root, 2);
  TASK_CHECK(call);
  TASK_CHECK(SameFunctions(call->targets, {0}));
  return 0;
}

static int RecursionVoidAndTailCallRef(void) {
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (func $a)
      (elem declare func $a)
      (func $void_ok nop)
      (func $void_trap unreachable)
      (func $tail_ref (param (ref null $cb))
        local.get 0 return_call_ref $cb)))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  // Indices: a=0, void_ok=1, void_trap=2, tail_ref=3.

  // Void functions still carry an explicit may_return flag.
  ReferenceFlow void_flow;
  TASK_CHECK(BuildFlow(*loaded.module, {1, 2}, &void_flow, &errors));
  const FlowContext* void_ok = FindContext(void_flow, 1);
  const FlowContext* void_trap = FindContext(void_flow, 2);
  TASK_CHECK(void_ok && void_trap);
  TASK_CHECK(void_ok->may_return && void_ok->results.empty());
  TASK_CHECK(!void_trap->may_return && void_trap->results.empty());

  // return_call_ref is an indirect tail call whose result becomes the
  // function's own result; nothing after it is reachable. As a root its
  // reference parameter takes top: every compatible function, external and
  // null are all possible.
  ReferenceFlow tail_flow;
  TASK_CHECK(BuildFlow(*loaded.module, {3}, &tail_flow, &errors));
  const FlowContext* tail = FindContext(tail_flow, 3);
  TASK_CHECK(tail && tail->may_return);
  TASK_CHECK(tail->calls.size() == 1);
  const FlowCall& tail_call = tail->calls[0];
  TASK_CHECK(tail_call.indirect && tail_call.expr_ordinal == 1);
  // All three module functions match the empty $cb signature.
  TASK_CHECK(SameFunctions(tail_call.targets, {0, 1, 2}));
  TASK_CHECK(tail_call.external && tail_call.may_trap_null);
  return 0;
}

static int RecursionIndependentAnalyses(void) {
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (func $a)
      (func $b)
      (elem declare func $a $b)
      (func $relay (param (ref null $cb)) (result (ref null $cb)) local.get 0)
      (func $left ref.func $a call $relay call_ref $cb)
      (func $right ref.func $b call $relay call_ref $cb)))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  // Indices: a=0, b=1, relay=2, left=3, right=4.
  ReferenceFlow left_flow;
  TASK_CHECK(BuildFlow(*loaded.module, {3}, &left_flow, &errors));
  ReferenceFlow right_flow;
  TASK_CHECK(BuildFlow(*loaded.module, {4}, &right_flow, &errors));

  const FlowCall* left_call = FindCall(*FindContext(left_flow, 3), 2);
  const FlowCall* right_call = FindCall(*FindContext(right_flow, 4), 2);
  TASK_CHECK(left_call && right_call);
  TASK_CHECK(SameFunctions(left_call->targets, {0}));
  TASK_CHECK(SameFunctions(right_call->targets, {1}));
  // The left analysis never needed the B-argument relay context, and vice
  // versa.
  TASK_CHECK(FindContextWithArg(left_flow, 2, {1}) == nullptr);
  TASK_CHECK(FindContextWithArg(right_flow, 2, {0}) == nullptr);

  // The report keeps its data after the module/byte inputs go away.
  ReferenceFlow detached;
  {
    Errors local_errors;
    Loaded scoped;
    TASK_CHECK(LoadModule(kWat, &scoped, &local_errors));
    TASK_CHECK(BuildFlow(*scoped.module, {3}, &detached, &local_errors));
  }
  const FlowCall* detached_call = FindCall(*FindContext(detached, 3), 2);
  TASK_CHECK(detached_call && SameFunctions(detached_call->targets, {0}));
  return 0;
}

// ===========================================================================
// consumer group: real CLI launch, library/CLI parity, reversed/duplicate
// roots, failure diagnostics, byte-identical write-back.
// ===========================================================================

static int ConsumerParity(void) {
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (type $rt (func (param (ref null $cb)) (result (ref null $cb))))
      (import "env" "imp" (func $imp (type $rt)))
      (func $a)
      (func $b)
      (elem declare func $a $b)
      (func $relay (type $rt) local.get 0)
      (func $left ref.func $a call $relay call_ref $cb)
      (func $right ref.func $b call $relay call_ref $cb)))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  const std::vector<Index> roots = {5, 4};
  ReferenceFlow flow;
  TASK_CHECK(BuildFlow(*loaded.module, roots, &flow, &errors));

  std::string path = WriteTempWasm(loaded.bytes, "consumer-parity.wasm");
  std::vector<Json> lines;
  int status = RunConsumer(path, roots, &lines);
  TASK_CHECK(status == 0);
  // One line per context plus a final roots line.
  TASK_CHECK(lines.size() == flow.contexts.size() + 1);
  for (size_t i = 0; i < flow.contexts.size(); ++i) {
    TASK_CHECK(JsonMatchesContext(lines[i], flow.contexts[i]));
    TASK_CHECK(static_cast<size_t>(lines[i].Find("context")->number) == i);
  }
  const Json* roots_json = lines.back().Find("roots");
  TASK_CHECK(roots_json && roots_json->elements.size() == flow.roots.size());
  for (size_t i = 0; i < flow.roots.size(); ++i) {
    TASK_CHECK(static_cast<size_t>(roots_json->elements[i].number) ==
               flow.roots[i]);
  }
  return 0;
}

static int ConsumerReversedAndDuplicateRoots(void) {
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (func $a)
      (func $b)
      (elem declare func $a $b)
      (func $left ref.func $a call_ref $cb)
      (func $right ref.func $b call_ref $cb)))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  std::string path = WriteTempWasm(loaded.bytes, "consumer-roots.wasm");

  // Indices: a=0, b=1, left=2, right=3.
  std::vector<Json> forward;
  std::vector<Json> reversed;
  TASK_CHECK(RunConsumer(path, {2, 3}, &forward) == 0);
  // Reverse order plus a duplicate: emitted contexts must be identical and
  // root order must not influence the sorted report.
  TASK_CHECK(RunConsumer(path, {3, 2, 2}, &reversed) == 0);
  TASK_CHECK(forward.size() == reversed.size());
  for (size_t i = 0; i < forward.size(); ++i) {
    TASK_CHECK(JsonEqual(forward[i], reversed[i]));
  }
  // The final roots line lists each root context once, in sorted order.
  const Json* roots_json = forward.back().Find("roots");
  TASK_CHECK(roots_json && roots_json->elements.size() == 2);
  TASK_CHECK(static_cast<size_t>(roots_json->elements[0].number) <
             static_cast<size_t>(roots_json->elements[1].number));
  return 0;
}

static int ConsumerFailure(void) {
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (func $a)
      (elem declare func $a)
      (func $f ref.func $a call_ref $cb)))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  std::string path = WriteTempWasm(loaded.bytes, "consumer-fail.wasm");
  std::vector<Json> lines;
  // Out-of-range root: the tool must fail non-zero and print a diagnostic.
  int status = RunConsumer(path, {99}, &lines, /*capture_stderr=*/true);
  TASK_CHECK(status != 0);
  return 0;
}

static int ConsumerWriteBackIdentical(void) {
  static const char* kWat = R"(
    (module
      (type $cb (func))
      (func $a)
      (elem declare func $a)
      (func $f ref.func $a call_ref $cb)))";
  Errors errors;
  Loaded loaded;
  TASK_CHECK(LoadModule(kWat, &loaded, &errors));
  ReferenceFlow flow;
  TASK_CHECK(BuildFlow(*loaded.module, {1}, &flow, &errors));

  // Analysis must not mutate the module: writing it back reproduces the exact
  // bytes produced by the original round trip.
  MemoryStream stream;
  WriteBinaryOptions options(fixture::Features(), true, false, false);
  TASK_CHECK(Succeeded(
      WriteBinaryModule(&stream, loaded.module.get(), options)));
  TASK_CHECK(stream.output_buffer().data == loaded.bytes);
  return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

const task_case task_cases[] = {
    {"contexts", "separation", ContextsSeparation},
    {"contexts", "top_and_import_prefix", ContextsTopAndImportPrefix},
    {"contexts", "import_as_root_and_ref_func", ContextsImportAsRootAndRefFunc},
    {"contexts", "unsupported_rejected", ContextsUnsupportedRejected},
    {"locals", "strong_update", LocalsStrongUpdate},
    {"locals", "if_merge", LocalsIfMerge},
    {"locals", "select", LocalsSelect},
    {"locals", "ref_as_non_null", LocalsRefAsNonNull},
    {"locals", "multi_result", LocalsMultiResult},
    {"recursion", "mutual_with_exit", RecursionMutualWithExit},
    {"recursion", "pure_and_null_only", RecursionPureAndNullOnly},
    {"recursion", "tail_call", RecursionTailCall},
    {"recursion", "void_and_tail_call_ref", RecursionVoidAndTailCallRef},
    {"recursion", "independent_analyses", RecursionIndependentAnalyses},
    {"consumer", "parity", ConsumerParity},
    {"consumer", "reversed_duplicate_roots", ConsumerReversedAndDuplicateRoots},
    {"consumer", "failure", ConsumerFailure},
    {"consumer", "write_back_identical", ConsumerWriteBackIdentical},
};
extern const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
