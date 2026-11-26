// Task tests for wabt::BuildReferenceFlow.
//
// Every input is synthetic WAT that goes through parse, ResolveNames,
// ValidateModule and binary writing, then comes back through ReadBinaryIr
// and validation exactly like the production chain.

#include "../task_support/check.h"
#include "../task_support/fixture.h"

#include "wabt/binary-writer.h"
#include "wabt/reference-flow.h"
#include "wabt/stream.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using wabt::Errors;
using wabt::Index;
using wabt::Module;
using wabt::ReferenceFlow;
using wabt::RefValue;
using wabt::FlowCall;
using wabt::FlowContext;
using wabt::Result;

namespace {

// ---------------------------------------------------------------------------
// Small predicates / finders with diagnostics
// ---------------------------------------------------------------------------

RefValue R(std::vector<Index> funcs, bool may_be_null = false,
           bool external = false) {
  RefValue v;
  v.functions = std::move(funcs);
  v.may_be_null = may_be_null;
  v.external = external;
  return v;
}

bool RvEq(const RefValue& a, const RefValue& b) {
  return a.functions == b.functions && a.may_be_null == b.may_be_null &&
         a.external == b.external;
}

void PrintRv(const RefValue& v) {
  std::fprintf(stderr, "funcs[");
  for (size_t i = 0; i < v.functions.size(); ++i) {
    std::fprintf(stderr, "%s%u", i ? "," : "",
                 static_cast<unsigned>(v.functions[i]));
  }
  std::fprintf(stderr, "] null=%d ext=%d", int(v.may_be_null),
               int(v.external));
}

bool CheckRv(const RefValue& got, const RefValue& want) {
  if (!RvEq(got, want)) {
    std::fprintf(stderr, "RefValue mismatch: got ");
    PrintRv(got);
    std::fprintf(stderr, " want ");
    PrintRv(want);
    std::fprintf(stderr, "\n");
    return false;
  }
  return true;
}

bool ArgsEq(const std::vector<RefValue>& a,
            const std::vector<RefValue>& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (!RvEq(a[i], b[i])) {
      return false;
    }
  }
  return true;
}

size_t FindCtx(const ReferenceFlow& flow,
               Index function,
               const std::vector<RefValue>& args) {
  for (size_t i = 0; i < flow.contexts.size(); ++i) {
    const FlowContext& c = flow.contexts[i];
    if (c.function == function && ArgsEq(c.arguments, args)) {
      return i;
    }
  }
  return static_cast<size_t>(-1);
}

size_t FindCtx0(const ReferenceFlow& flow, Index function) {
  return FindCtx(flow, function, {});
}

const FlowCall* CallAt(const FlowContext& c, size_t ordinal) {
  for (const FlowCall& call : c.calls) {
    if (call.expr_ordinal == ordinal) {
      return &call;
    }
  }
  return nullptr;
}

bool CheckCall(const FlowContext& c,
               size_t ordinal,
               bool indirect,
               const std::vector<Index>& targets) {
  const FlowCall* call = CallAt(c, ordinal);
  if (!call) {
    std::fprintf(stderr, "missing call ordinal %zu\n", ordinal);
    return false;
  }
  if (call->indirect != indirect) {
    std::fprintf(stderr, "ordinal %zu indirect=%d want %d\n", ordinal,
                 int(call->indirect), int(indirect));
    return false;
  }
  if (call->targets != targets) {
    std::fprintf(stderr, "ordinal %zu targets mismatch\n", ordinal);
    return false;
  }
  return true;
}

bool FlowEqual(const ReferenceFlow& a, const ReferenceFlow& b) {
  if (a.roots != b.roots || a.contexts.size() != b.contexts.size()) {
    return false;
  }
  for (size_t i = 0; i < a.contexts.size(); ++i) {
    const FlowContext& x = a.contexts[i];
    const FlowContext& y = b.contexts[i];
    if (x.function != y.function || x.may_return != y.may_return ||
        !ArgsEq(x.arguments, y.arguments) ||
        !ArgsEq(x.results, y.results) || x.calls.size() != y.calls.size()) {
      return false;
    }
    for (size_t j = 0; j < x.calls.size(); ++j) {
      const FlowCall& p = x.calls[j];
      const FlowCall& q = y.calls[j];
      if (p.expr_ordinal != q.expr_ordinal || p.indirect != q.indirect ||
          p.targets != q.targets ||
          p.callee_contexts != q.callee_contexts ||
          p.external != q.external ||
          p.may_trap_null != q.may_trap_null) {
        return false;
      }
    }
  }
  return true;
}

struct BuiltWat {
  Module module;
  Errors errors;
  std::vector<uint8_t> bytes;
  bool ok = false;
};

BuiltWat Build(const char* text) {
  BuiltWat w;
  w.ok = wabt::Succeeded(fixture::RoundTrip(text, &w.module, &w.errors,
                                            &w.bytes));
  if (!w.ok) {
    for (const auto& e : w.errors) {
      std::fprintf(stderr, "fixture error: %s\n", e.message.c_str());
    }
  }
  return w;
}

// ---------------------------------------------------------------------------
// Tiny JSON parser used by the consumer group
// ---------------------------------------------------------------------------

struct JV {
  enum Type { Nul, Bool, Num, Str, Arr, Obj } type = Nul;
  bool boolean = false;
  long long number = 0;
  std::string string;
  std::vector<JV> array;
  std::map<std::string, JV> object;

  const JV* At(const std::string& key) const {
    auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
  }
};

struct JsonParser {
  const std::string& s;
  size_t p = 0;
  explicit JsonParser(const std::string& text) : s(text) {}

  void SkipWs() {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' ||
                            s[p] == '\r')) {
      ++p;
    }
  }
  bool Parse(JV& out) {
    SkipWs();
    if (p >= s.size()) {
      return false;
    }
    char ch = s[p];
    if (ch == '{') {
      return ParseObject(out);
    }
    if (ch == '[') {
      return ParseArray(out);
    }
    if (ch == '"') {
      out.type = JV::Str;
      return ParseString(out.string);
    }
    if (ch == 't' || ch == 'f') {
      if (s.compare(p, 4, "true") == 0) {
        p += 4;
        out.type = JV::Bool;
        out.boolean = true;
        return true;
      }
      if (s.compare(p, 5, "false") == 0) {
        p += 5;
        out.type = JV::Bool;
        out.boolean = false;
        return true;
      }
      return false;
    }
    if (ch == 'n') {
      if (s.compare(p, 4, "null") == 0) {
        p += 4;
        out.type = JV::Nul;
        return true;
      }
      return false;
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
      char* end = nullptr;
      out.number = std::strtoll(s.c_str() + p, &end, 10);
      p = static_cast<size_t>(end - s.c_str());
      out.type = JV::Num;
      return true;
    }
    return false;
  }
  bool ParseString(std::string& out) {
    if (s[p] != '"') {
      return false;
    }
    ++p;
    while (p < s.size() && s[p] != '"') {
      if (s[p] == '\\' && p + 1 < s.size()) {
        char e = s[p + 1];
        if (e == 'n') {
          out.push_back('\n');
        } else if (e == 't') {
          out.push_back('\t');
        } else {
          out.push_back(e);
        }
        p += 2;
      } else {
        out.push_back(s[p++]);
      }
    }
    if (p >= s.size()) {
      return false;
    }
    ++p;
    return true;
  }
  bool ParseObject(JV& out) {
    out.type = JV::Obj;
    ++p;
    SkipWs();
    if (p < s.size() && s[p] == '}') {
      ++p;
      return true;
    }
    while (true) {
      SkipWs();
      std::string key;
      if (p >= s.size() || s[p] != '"' || !ParseString(key)) {
        return false;
      }
      SkipWs();
      if (p >= s.size() || s[p] != ':') {
        return false;
      }
      ++p;
      JV value;
      if (!Parse(value)) {
        return false;
      }
      out.object[key] = std::move(value);
      SkipWs();
      if (p >= s.size()) {
        return false;
      }
      if (s[p] == ',') {
        ++p;
        continue;
      }
      if (s[p] == '}') {
        ++p;
        return true;
      }
      return false;
    }
  }
  bool ParseArray(JV& out) {
    out.type = JV::Arr;
    ++p;
    SkipWs();
    if (p < s.size() && s[p] == ']') {
      ++p;
      return true;
    }
    while (true) {
      JV value;
      if (!Parse(value)) {
        return false;
      }
      out.array.push_back(std::move(value));
      SkipWs();
      if (p >= s.size()) {
        return false;
      }
      if (s[p] == ',') {
        ++p;
        continue;
      }
      if (s[p] == ']') {
        ++p;
        return true;
      }
      return false;
    }
  }
};

JV JNum(long long n) {
  JV v;
  v.type = JV::Num;
  v.number = n;
  return v;
}
JV JBool(bool b) {
  JV v;
  v.type = JV::Bool;
  v.boolean = b;
  return v;
}
JV JArr(std::vector<JV> items) {
  JV v;
  v.type = JV::Arr;
  v.array = std::move(items);
  return v;
}
JV JIndexArray(const std::vector<Index>& xs) {
  std::vector<JV> items;
  for (Index x : xs) {
    items.push_back(JNum(x));
  }
  return JArr(std::move(items));
}
JV JSizeArray(const std::vector<size_t>& xs) {
  std::vector<JV> items;
  for (size_t x : xs) {
    items.push_back(JNum(static_cast<long long>(x)));
  }
  return JArr(std::move(items));
}
JV JRef(const RefValue& r) {
  JV v;
  v.type = JV::Obj;
  v.object["functions"] = JIndexArray(r.functions);
  v.object["may_be_null"] = JBool(r.may_be_null);
  v.object["external"] = JBool(r.external);
  return v;
}
JV JRefArray(const std::vector<RefValue>& rs) {
  std::vector<JV> items;
  for (const auto& r : rs) {
    items.push_back(JRef(r));
  }
  return JArr(std::move(items));
}
JV JCall(const FlowCall& c) {
  JV v;
  v.type = JV::Obj;
  v.object["expr_ordinal"] = JNum(static_cast<long long>(c.expr_ordinal));
  v.object["indirect"] = JBool(c.indirect);
  v.object["targets"] = JIndexArray(c.targets);
  v.object["callee_contexts"] = JSizeArray(c.callee_contexts);
  v.object["external"] = JBool(c.external);
  v.object["may_trap_null"] = JBool(c.may_trap_null);
  return v;
}
JV JContext(size_t index, const FlowContext& c) {
  JV v;
  v.type = JV::Obj;
  v.object["context"] = JNum(static_cast<long long>(index));
  v.object["function"] = JNum(c.function);
  v.object["arguments"] = JRefArray(c.arguments);
  v.object["may_return"] = JBool(c.may_return);
  v.object["results"] = JRefArray(c.results);
  std::vector<JV> calls;
  for (const auto& call : c.calls) {
    calls.push_back(JCall(call));
  }
  v.object["calls"] = JArr(std::move(calls));
  return v;
}

bool JvEq(const JV& a, const JV& b, const std::string& path) {
  if (a.type != b.type) {
    std::fprintf(stderr, "json type mismatch at %s\n", path.c_str());
    return false;
  }
  switch (a.type) {
    case JV::Nul:
      return true;
    case JV::Bool:
      if (a.boolean != b.boolean) {
        std::fprintf(stderr, "json bool mismatch at %s\n", path.c_str());
        return false;
      }
      return true;
    case JV::Num:
      if (a.number != b.number) {
        std::fprintf(stderr, "json num mismatch at %s: %lld vs %lld\n",
                     path.c_str(), a.number, b.number);
        return false;
      }
      return true;
    case JV::Str:
      if (a.string != b.string) {
        return false;
      }
      return true;
    case JV::Arr:
      if (a.array.size() != b.array.size()) {
        std::fprintf(stderr, "json array size mismatch at %s: %zu vs %zu\n",
                     path.c_str(), a.array.size(), b.array.size());
        return false;
      }
      for (size_t i = 0; i < a.array.size(); ++i) {
        if (!JvEq(a.array[i], b.array[i],
                  path + "[" + std::to_string(i) + "]")) {
          return false;
        }
      }
      return true;
    case JV::Obj:
      if (a.object.size() != b.object.size()) {
        std::fprintf(stderr, "json object key count mismatch at %s\n",
                     path.c_str());
        return false;
      }
      for (const auto& kv : a.object) {
        const JV* other = b.At(kv.first);
        if (!other || !JvEq(kv.second, *other, path + "." + kv.first)) {
          return false;
        }
      }
      return true;
  }
  return false;
}

JV ParseJsonLine(const std::string& line) {
  JsonParser parser(line);
  JV v;
  parser.Parse(v);
  return v;
}

// ---------------------------------------------------------------------------
// CLI launching
// ---------------------------------------------------------------------------

std::string RepoRoot() {
  std::string file = __FILE__;
  if (!file.empty() && file[0] != '/') {
    char cwd[4096];
    if (::getcwd(cwd, sizeof(cwd))) {
      file = std::string(cwd) + "/" + file;
    }
  }
  const std::string suffix = "/test/task_reference_flow.cc";
  size_t pos = file.rfind(suffix);
  if (pos != std::string::npos) {
    return file.substr(0, pos);
  }
  return ".";
}

std::string CliPath() {
  if (const char* env = std::getenv("TASK_REF_FLOW_CLI")) {
    if (::access(env, X_OK) == 0) {
      return env;
    }
  }
  std::string rel = ".task-build/wabt/wasm-ref-flow";
  if (::access(rel.c_str(), X_OK) == 0) {
    return rel;
  }
  std::string abs = RepoRoot() + "/.task-build/wabt/wasm-ref-flow";
  if (::access(abs.c_str(), X_OK) == 0) {
    return abs;
  }
  return abs;
}

struct CliResult {
  int exit_code = -1;
  std::string out;
  std::string err;
};

CliResult RunCli(const std::string& root_args,
                 const std::string& wasm_abs,
                 const std::string& tag) {
  std::string cli = CliPath();
  std::string err_path = RepoRoot() + "/.task-build/cli-err-" + tag + ".txt";
  std::string cmd = "'" + cli + "' " + root_args + " '" + wasm_abs +
                    "' 2>'" + err_path + "'";
  FILE* pipe = ::popen(cmd.c_str(), "r");
  CliResult result;
  if (!pipe) {
    result.exit_code = -127;
    return result;
  }
  char buffer[4096];
  while (size_t n = std::fread(buffer, 1, sizeof(buffer), pipe)) {
    result.out.append(buffer, n);
  }
  int status = ::pclose(pipe);
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  }
  std::ifstream in(err_path, std::ios::binary);
  result.err.assign(std::istreambuf_iterator<char>(in),
                    std::istreambuf_iterator<char>());
  return result;
}

std::string WriteCaseWasm(const BuiltWat& w, const std::string& name) {
  std::string path = RepoRoot() + "/.task-build/" + name;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(w.bytes.data()),
            static_cast<std::streamsize>(w.bytes.size()));
  out.close();
  return path;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// Re-serializes the (read-only analyzed) module with the same options as the
// fixture; used to prove analysis does not mutate the Module.
std::vector<uint8_t> WriteModuleBytes(const Module& module) {
  wabt::Features features;
  features.enable_function_references();
  wabt::MemoryStream stream;
  wabt::WriteBinaryOptions options(features, true, false, false);
  wabt::Result r = wabt::WriteBinaryModule(&stream, &module, options);
  if (wabt::Failed(r)) {
    return {};
  }
  return stream.output_buffer().data;
}

// ---------------------------------------------------------------------------
// Shared synthetic inputs
// ---------------------------------------------------------------------------

const char* kWatSeparation = R"WAT(
(module
  (type $cb (func))
  (func $a (type $cb))
  (func $b (type $cb))
  (elem declare func $a $b)
  (func $id (param $x (ref null $cb)) (result (ref null $cb)) local.get 0)
  (func $ea ref.func $a call $id call_ref $cb)
  (func $eb ref.func $b call $id call_ref $cb)
)
)WAT";

}  // namespace

// ===========================================================================
// contexts group
// ===========================================================================

static int contexts_separation(void) {
  BuiltWat w = Build(kWatSeparation);
  TASK_CHECK(w.ok);

  ReferenceFlow flow;
  Result r = wabt::BuildReferenceFlow(w.module, {3, 4}, &flow, &w.errors);
  TASK_CHECK(wabt::Succeeded(r));
  TASK_CHECK(flow.contexts.size() == 6);
  TASK_CHECK((flow.roots == std::vector<size_t>{4, 5}));

  TASK_CHECK(FindCtx0(flow, 0) == 0);  // $a
  TASK_CHECK(FindCtx0(flow, 1) == 1);  // $b
  size_t id_a = FindCtx(flow, 2, {R({0})});
  size_t id_b = FindCtx(flow, 2, {R({1})});
  TASK_CHECK(id_a == 2);
  TASK_CHECK(id_b == 3);

  const FlowContext& ea = flow.contexts[4];
  TASK_CHECK(ea.function == 3);
  TASK_CHECK(ea.may_return);
  TASK_CHECK(ea.calls.size() == 2);
  TASK_CHECK(CheckCall(ea, 1, false, {2}));
  TASK_CHECK(ea.calls[0].callee_contexts == std::vector<size_t>{id_a});
  TASK_CHECK(CheckCall(ea, 2, true, {0}));
  TASK_CHECK(ea.calls[1].callee_contexts == std::vector<size_t>{0});

  const FlowContext& eb = flow.contexts[5];
  TASK_CHECK(eb.function == 4);
  TASK_CHECK(CheckCall(eb, 1, false, {2}));
  TASK_CHECK(eb.calls[0].callee_contexts == std::vector<size_t>{id_b});
  TASK_CHECK(CheckCall(eb, 2, true, {1}));
  TASK_CHECK(eb.calls[1].callee_contexts == std::vector<size_t>{1});

  // Summaries of the identity helper return exactly their argument.
  TASK_CHECK(flow.contexts[id_a].may_return);
  TASK_CHECK(flow.contexts[id_a].results.size() == 1);
  TASK_CHECK(CheckRv(flow.contexts[id_a].results[0], R({0})));
  TASK_CHECK(CheckRv(flow.contexts[id_b].results[0], R({1})));
  return 0;
}

static int contexts_cross_call_return(void) {
  const char* wat = R"WAT(
(module
  (type $cb (func))
  (func $a (type $cb))
  (elem declare func $a)
  (func $id (param $x (ref null $cb)) (result (ref null $cb)) local.get 0)
  (func $twice (param $x (ref null $cb)) (result (ref null $cb))
    local.get 0 call $id call $id)
  (func $root ref.func $a call $twice call_ref $cb)
)
)WAT";
  BuiltWat w = Build(wat);
  TASK_CHECK(w.ok);
  ReferenceFlow flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {3}, &flow, &w.errors)));

  TASK_CHECK(flow.contexts.size() == 4);
  size_t id = FindCtx(flow, 1, {R({0})});
  size_t twice = FindCtx(flow, 2, {R({0})});
  size_t root = FindCtx0(flow, 3);
  TASK_CHECK(id == 1);
  TASK_CHECK(twice == 2);
  TASK_CHECK(root == 3);

  const FlowContext& tc = flow.contexts[twice];
  TASK_CHECK(tc.may_return);
  TASK_CHECK(tc.results.size() == 1);
  TASK_CHECK(CheckRv(tc.results[0], R({0})));
  TASK_CHECK(tc.calls.size() == 2);
  TASK_CHECK(CheckCall(tc, 1, false, {1}));
  TASK_CHECK(CheckCall(tc, 2, false, {1}));
  TASK_CHECK(tc.calls[0].callee_contexts == std::vector<size_t>{id});
  TASK_CHECK(tc.calls[1].callee_contexts == std::vector<size_t>{id});

  const FlowContext& rc = flow.contexts[root];
  TASK_CHECK(CheckCall(rc, 1, false, {2}));
  TASK_CHECK(rc.calls[0].callee_contexts ==
             std::vector<size_t>{twice});
  TASK_CHECK(CheckCall(rc, 2, true, {0}));
  return 0;
}

static int contexts_imports_and_top(void) {
  const char* wat = R"WAT(
(module
  (type $cb (func))
  (import "e" "imp" (func $imp (type $cb)))
  (func $a (type $cb))
  (func $rootTop (param $r (ref null $cb)) local.get 0 call_ref $cb)
  (func $rootCall call $imp)
)
)WAT";
  BuiltWat w = Build(wat);
  TASK_CHECK(w.ok);
  ReferenceFlow flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {2}, &flow, &w.errors)));

  // Root reference argument is top: every compatible module function
  // (including the function import at index 0), external and null.
  size_t root_top = FindCtx(flow, 2, {R({0, 1, 3}, true, true)});
  TASK_CHECK(root_top != static_cast<size_t>(-1));
  const FlowContext& rt = flow.contexts[root_top];
  TASK_CHECK(rt.arguments.size() == 1);
  TASK_CHECK(CheckRv(rt.arguments[0], R({0, 1, 3}, true, true)));

  const FlowCall* ref_call = CallAt(rt, 1);
  TASK_CHECK(ref_call != nullptr);
  TASK_CHECK(ref_call->indirect);
  TASK_CHECK((ref_call->targets == std::vector<Index>{0, 1, 3}));
  TASK_CHECK(ref_call->external);
  TASK_CHECK(ref_call->may_trap_null);

  // Import context exists and is assumed normally returning.
  size_t imp_ctx = FindCtx0(flow, 0);
  TASK_CHECK(imp_ctx != static_cast<size_t>(-1));
  TASK_CHECK(flow.contexts[imp_ctx].may_return);
  TASK_CHECK(ref_call->callee_contexts.size() == 3);

  // Import reference result takes top.
  const char* wat2 = R"WAT(
(module
  (type $cb (func))
  (import "e" "op" (func $op (param (ref null $cb))
                             (result (ref null $cb))))
  (func $a (type $cb))
  (elem declare func $a)
  (func $root (result i32)
    ref.null $cb call $op call_ref $cb i32.const 7)
)
)WAT";
  BuiltWat w2 = Build(wat2);
  TASK_CHECK(w2.ok);
  ReferenceFlow flow2;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w2.module, {2}, &flow2, &w2.errors)));
  TASK_CHECK(flow2.contexts.size() == 3);

  size_t op_ctx = FindCtx(flow2, 0, {R({}, true, false)});
  TASK_CHECK(op_ctx != static_cast<size_t>(-1));
  const FlowContext& opc = flow2.contexts[op_ctx];
  TASK_CHECK(opc.may_return);
  TASK_CHECK(opc.results.size() == 1);
  TASK_CHECK(CheckRv(opc.results[0], R({1}, true, true)));

  const FlowContext& root = flow2.contexts[FindCtx0(flow2, 2)];
  TASK_CHECK(root.may_return);
  TASK_CHECK(root.results.size() == 1);
  TASK_CHECK(RvEq(root.results[0], R({})));  // scalar position is bottom
  const FlowCall* cr = CallAt(root, 2);
  TASK_CHECK(cr != nullptr);
  TASK_CHECK((cr->targets == std::vector<Index>{1}));
  TASK_CHECK(cr->external);
  TASK_CHECK(cr->may_trap_null);
  return 0;
}

// ===========================================================================
// locals group
// ===========================================================================

static int locals_strong_overwrite(void) {
  const char* wat = R"WAT(
(module
  (type $cb (func))
  (func $a (type $cb))
  (func $b (type $cb))
  (elem declare func $a $b)
  (func $ov (local $p (ref null $cb))
    ref.func $a local.set $p
    ref.func $b local.set $p
    local.get $p call_ref $cb)
)
)WAT";
  BuiltWat w = Build(wat);
  TASK_CHECK(w.ok);
  ReferenceFlow flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {2}, &flow, &w.errors)));
  TASK_CHECK(flow.contexts.size() == 2);  // $a is overwritten, never called
  const FlowContext& ov = flow.contexts[FindCtx0(flow, 2)];
  TASK_CHECK(ov.calls.size() == 1);
  TASK_CHECK(CheckCall(ov, 5, true, {1}));  // only $b survives
  TASK_CHECK(!ov.calls[0].may_trap_null);
  return 0;
}

static int locals_branch_merge(void) {
  const char* wat = R"WAT(
(module
  (type $cb (func))
  (func $a (type $cb))
  (func $b (type $cb))
  (elem declare func $a $b)
  (func $join (local $p (ref null $cb))
    i32.const 0
    if ref.func $a local.set $p else ref.func $b local.set $p end
    local.get $p call_ref $cb)
  (func $deadarm (local $p (ref null $cb))
    i32.const 0
    if ref.func $a local.set $p else ref.func $b local.set $p return end
    local.get $p call_ref $cb)
  (func $noelse (local $p (ref null $cb))
    i32.const 0
    if ref.func $a local.set $p end
    local.get $p call_ref $cb)
)
)WAT";
  BuiltWat w = Build(wat);
  TASK_CHECK(w.ok);
  ReferenceFlow flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {2, 3, 4}, &flow, &w.errors)));

  const FlowContext& join = flow.contexts[FindCtx0(flow, 2)];
  TASK_CHECK(CheckCall(join, 7, true, {0, 1}));

  const FlowContext& dead = flow.contexts[FindCtx0(flow, 3)];
  const FlowCall* dc = CallAt(dead, 8);
  TASK_CHECK(dc != nullptr);
  TASK_CHECK((dc->targets == std::vector<Index>{0}));  // only live true arm
  TASK_CHECK(!dc->may_trap_null);

  const FlowContext& noelse = flow.contexts[FindCtx0(flow, 4)];
  const FlowCall* nc = CallAt(noelse, 5);
  TASK_CHECK(nc != nullptr);
  TASK_CHECK((nc->targets == std::vector<Index>{0}));
  TASK_CHECK(nc->may_trap_null);  // identity false arm keeps initial null
  return 0;
}

static int locals_select(void) {
  const char* wat = R"WAT(
(module
  (type $cb (func))
  (func $a (type $cb))
  (func $b (type $cb))
  (elem declare func $a $b)
  (func $sel
    ref.func $a ref.func $b i32.const 0
    select (result (ref null 0))
    call_ref $cb)
)
)WAT";
  BuiltWat w = Build(wat);
  TASK_CHECK(w.ok);
  ReferenceFlow flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {2}, &flow, &w.errors)));
  const FlowContext& sel = flow.contexts[FindCtx0(flow, 2)];
  TASK_CHECK(sel.calls.size() == 1);
  TASK_CHECK(CheckCall(sel, 4, true, {0, 1}));
  return 0;
}

static int locals_multi_results(void) {
  const char* wat = R"WAT(
(module
  (type $cb (func))
  (func $a (type $cb))
  (elem declare func $a)
  (func $pair (result i32 (ref null $cb)) i32.const 1 ref.func $a)
  (func $use call $pair call_ref $cb drop)
)
)WAT";
  BuiltWat w = Build(wat);
  TASK_CHECK(w.ok);
  ReferenceFlow flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {2}, &flow, &w.errors)));
  TASK_CHECK(flow.contexts.size() == 3);

  size_t pair = FindCtx0(flow, 1);
  const FlowContext& pc = flow.contexts[pair];
  TASK_CHECK(pc.may_return);
  TASK_CHECK(pc.results.size() == 2);
  TASK_CHECK(CheckRv(pc.results[0], R({})));     // i32 position
  TASK_CHECK(CheckRv(pc.results[1], R({0})));   // ref position

  const FlowContext& use = flow.contexts[FindCtx0(flow, 2)];
  TASK_CHECK(use.may_return);
  TASK_CHECK(CheckCall(use, 0, false, {1}));
  TASK_CHECK(use.calls[0].callee_contexts == std::vector<size_t>{pair});
  TASK_CHECK(CheckCall(use, 1, true, {0}));
  return 0;
}

// ===========================================================================
// recursion group
// ===========================================================================

static int recursion_mutual_exit(void) {
  const char* wat = R"WAT(
(module
  (type $cb (func))
  (func $a (type $cb))
  (elem declare func $a)
  (func $bike (param $x (ref null $cb)) (result (ref null $cb))
    i32.const 0
    if (result (ref null $cb))
      local.get 0
    else
      local.get 0 call $bick
    end)
  (func $bick (param $x (ref null $cb)) (result (ref null $cb))
    local.get 0 call $bike)
  (func $root ref.func $a call $bick call_ref $cb)
)
)WAT";
  BuiltWat w = Build(wat);
  TASK_CHECK(w.ok);
  ReferenceFlow flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {3}, &flow, &w.errors)));
  TASK_CHECK(flow.contexts.size() == 4);

  size_t bike = FindCtx(flow, 1, {R({0})});
  size_t bick = FindCtx(flow, 2, {R({0})});
  TASK_CHECK(bike == 1);
  TASK_CHECK(bick == 2);
  TASK_CHECK(flow.contexts[bike].may_return);
  TASK_CHECK(flow.contexts[bick].may_return);
  TASK_CHECK(CheckRv(flow.contexts[bike].results[0], R({0})));
  TASK_CHECK(CheckRv(flow.contexts[bick].results[0], R({0})));

  const FlowContext& bc = flow.contexts[bike];
  TASK_CHECK(CheckCall(bc, 4, false, {2}));
  TASK_CHECK(bc.calls[0].callee_contexts == std::vector<size_t>{bick});
  const FlowContext& kc = flow.contexts[bick];
  TASK_CHECK(CheckCall(kc, 1, false, {1}));
  TASK_CHECK(kc.calls[0].callee_contexts == std::vector<size_t>{bike});

  const FlowContext& root = flow.contexts[FindCtx0(flow, 3)];
  TASK_CHECK(CheckCall(root, 1, false, {2}));
  TASK_CHECK(root.calls[0].callee_contexts ==
             std::vector<size_t>{bick});
  TASK_CHECK(CheckCall(root, 2, true, {0}));
  return 0;
}

static int recursion_pure(void) {
  const char* wat = R"WAT(
(module
  (type $cb (func))
  (func $a (type $cb))
  (elem declare func $a)
  (func $spin (result (ref null $cb)) call $spin)
  (func $sroot call $spin call_ref $cb ref.func $a call_ref $cb)
)
)WAT";
  BuiltWat w = Build(wat);
  TASK_CHECK(w.ok);
  ReferenceFlow flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {2}, &flow, &w.errors)));

  // $a is never referenced by a reachable published call, so only the spin
  // context and its root survive.
  TASK_CHECK(flow.contexts.size() == 2);
  size_t spin = FindCtx0(flow, 1);
  size_t root = FindCtx0(flow, 2);
  TASK_CHECK(spin == 0);
  TASK_CHECK(root == 1);

  const FlowContext& sc = flow.contexts[spin];
  TASK_CHECK(!sc.may_return);
  TASK_CHECK(sc.results.size() == 1);
  TASK_CHECK(RvEq(sc.results[0], R({})));
  TASK_CHECK(sc.calls.size() == 1);
  TASK_CHECK(CheckCall(sc, 0, false, {1}));
  TASK_CHECK(sc.calls[0].callee_contexts == std::vector<size_t>{spin});

  const FlowContext& rc = flow.contexts[root];
  TASK_CHECK(!rc.may_return);
  TASK_CHECK(rc.calls.size() == 1);  // later call_refs are unreachable
  TASK_CHECK(CheckCall(rc, 0, false, {1}));
  TASK_CHECK(rc.calls[0].callee_contexts == std::vector<size_t>{spin});
  TASK_CHECK((flow.roots == std::vector<size_t>{root}));
  return 0;
}

static int recursion_null_only(void) {
  const char* wat = R"WAT(
(module
  (type $cb (func))
  (func $a (type $cb))
  (elem declare func $a)
  (func $nroot ref.null $cb call_ref $cb ref.func $a call_ref $cb)
)
)WAT";
  BuiltWat w = Build(wat);
  TASK_CHECK(w.ok);
  ReferenceFlow flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {1}, &flow, &w.errors)));
  TASK_CHECK(flow.contexts.size() == 1);
  const FlowContext& c = flow.contexts[0];
  TASK_CHECK(c.function == 1);
  TASK_CHECK(!c.may_return);
  TASK_CHECK(c.calls.size() == 1);  // second call_ref not published
  const FlowCall* call = CallAt(c, 1);
  TASK_CHECK(call != nullptr);
  TASK_CHECK(call->indirect);
  TASK_CHECK(call->targets.empty());
  TASK_CHECK(call->callee_contexts.empty());
  TASK_CHECK(!call->external);
  TASK_CHECK(call->may_trap_null);
  return 0;
}

static int recursion_tail(void) {
  const char* wat = R"WAT(
(module
  (type $cb (func))
  (func $a (type $cb))
  (elem declare func $a)
  (func $id (param $x (ref null $cb)) (result (ref null $cb)) local.get 0)
  (func $t (param $x (ref null $cb)) (result (ref null $cb))
    local.get 0 return_call $id)
  (func $root ref.func $a call $t call_ref $cb)
)
)WAT";
  BuiltWat w = Build(wat);
  TASK_CHECK(w.ok);
  ReferenceFlow flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {3}, &flow, &w.errors)));
  size_t id = FindCtx(flow, 1, {R({0})});
  size_t tc = FindCtx(flow, 2, {R({0})});
  const FlowContext& tail = flow.contexts[tc];
  TASK_CHECK(tail.may_return);
  TASK_CHECK(CheckRv(tail.results[0], R({0})));
  TASK_CHECK(tail.calls.size() == 1);
  TASK_CHECK(tail.calls[0].expr_ordinal == 1);
  TASK_CHECK(!tail.calls[0].indirect);
  TASK_CHECK((tail.calls[0].targets == std::vector<Index>{1}));
  TASK_CHECK(tail.calls[0].callee_contexts == std::vector<size_t>{id});

  const char* wat2 = "(module (func $s return_call $s) "
                     "(func $r call $s))";
  BuiltWat w2 = Build(wat2);
  TASK_CHECK(w2.ok);
  ReferenceFlow flow2;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w2.module, {1}, &flow2, &w2.errors)));
  TASK_CHECK(flow2.contexts.size() == 2);
  size_t s = FindCtx0(flow2, 0);
  const FlowContext& sc = flow2.contexts[s];
  TASK_CHECK(!sc.may_return);
  TASK_CHECK(sc.calls.size() == 1);
  TASK_CHECK(sc.calls[0].callee_contexts == std::vector<size_t>{s});
  return 0;
}

static int recursion_prune_and_reanalyze(void) {
  const char* wat = R"WAT(
(module
  (type $cb (func))
  (func $a (type $cb))
  (func $b (type $cb))
  (elem declare func $a $b)
  (func $sink (param $x (ref null $cb)) local.get $x call_ref $cb)
  (func $magic (param $x (ref null $cb)) (result (ref null $cb))
    i32.const 0
    if (result (ref null $cb))
      local.get $x
    else
      ref.func $b call $magic
    end)
  (func $root ref.func $a call $magic call $sink)
)
)WAT";
  BuiltWat w = Build(wat);
  TASK_CHECK(w.ok);
  ReferenceFlow flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {4}, &flow, &w.errors)));

  // Exactly one sink context survives, with the widened argument; the
  // transient sink({$a}) context is pruned.
  size_t sink_ab = FindCtx(flow, 2, {R({0, 1})});
  TASK_CHECK(sink_ab != static_cast<size_t>(-1));
  TASK_CHECK(FindCtx(flow, 2, {R({0})}) == static_cast<size_t>(-1));
  size_t sink_count = 0;
  for (const FlowContext& c : flow.contexts) {
    if (c.function == 2) {
      ++sink_count;
    }
  }
  TASK_CHECK(sink_count == 1);

  size_t magic_a = FindCtx(flow, 3, {R({0})});
  size_t magic_b = FindCtx(flow, 3, {R({1})});
  TASK_CHECK(magic_a != static_cast<size_t>(-1));
  TASK_CHECK(magic_b != static_cast<size_t>(-1));
  // Summary grew after re-analysis of callers.
  TASK_CHECK(CheckRv(flow.contexts[magic_a].results[0], R({0, 1})));

  const FlowContext& sink = flow.contexts[sink_ab];
  TASK_CHECK(CheckCall(sink, 1, true, {0, 1}));

  const FlowContext& root = flow.contexts[FindCtx0(flow, 4)];
  TASK_CHECK(CheckCall(root, 1, false, {3}));
  TASK_CHECK(root.calls[0].callee_contexts ==
             std::vector<size_t>{magic_a});
  TASK_CHECK(CheckCall(root, 2, false, {2}));
  TASK_CHECK(root.calls[1].callee_contexts ==
             std::vector<size_t>{sink_ab});

  // Repeated analysis is stable with no cross-run cache.
  ReferenceFlow again;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {4}, &again, &w.errors)));
  TASK_CHECK(FlowEqual(flow, again));

  // Interleaving a different module does not perturb either report.
  BuiltWat other = Build(kWatSeparation);
  TASK_CHECK(other.ok);
  ReferenceFlow sep;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(other.module, {3, 4}, &sep, &other.errors)));
  ReferenceFlow third;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {4}, &third, &w.errors)));
  TASK_CHECK(FlowEqual(flow, third));
  TASK_CHECK(sep.contexts.size() == 6);
  return 0;
}

// ===========================================================================
// consumer group
// ===========================================================================

static int consumer_cli_matches_library(void) {
  BuiltWat w = Build(kWatSeparation);
  TASK_CHECK(w.ok);
  ReferenceFlow flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {3, 4}, &flow, &w.errors)));

  std::string wasm = WriteCaseWasm(w, "consumer-separation.wasm");
  CliResult cli = RunCli("--root 3 --root 4", wasm, "match");
  TASK_CHECK(cli.exit_code == 0);
  TASK_CHECK(!cli.out.empty());

  std::vector<std::string> lines;
  size_t start = 0;
  while (true) {
    size_t nl = cli.out.find('\n', start);
    std::string line = cli.out.substr(
        start, nl == std::string::npos ? std::string::npos : nl - start);
    if (!line.empty()) {
      lines.push_back(line);
    }
    if (nl == std::string::npos) {
      break;
    }
    start = nl + 1;
  }
  TASK_CHECK(lines.size() == flow.contexts.size() + 1);

  for (size_t i = 0; i < flow.contexts.size(); ++i) {
    JV got = ParseJsonLine(lines[i]);
    JV want = JContext(i, flow.contexts[i]);
    TASK_CHECK(JvEq(got, want, "line" + std::to_string(i)));
  }
  JV roots_got = ParseJsonLine(lines.back());
  JV roots_want;
  roots_want.type = JV::Obj;
  roots_want.object["roots"] = JSizeArray(flow.roots);
  TASK_CHECK(JvEq(roots_got, roots_want, "roots"));
  return 0;
}

static int consumer_roots_order_and_dupes(void) {
  BuiltWat w = Build(kWatSeparation);
  TASK_CHECK(w.ok);
  std::string wasm = WriteCaseWasm(w, "consumer-roots.wasm");

  CliResult canonical = RunCli("--root 3 --root 4", wasm, "canon");
  CliResult reversed = RunCli("--root 4 --root 3", wasm, "rev");
  CliResult duped = RunCli("--root 3 --root 4 --root 3 --root 4", wasm, "dup");
  TASK_CHECK(canonical.exit_code == 0);
  TASK_CHECK(reversed.exit_code == 0);
  TASK_CHECK(duped.exit_code == 0);
  TASK_CHECK(reversed.out == canonical.out);
  TASK_CHECK(duped.out == canonical.out);
  return 0;
}

static int consumer_failures(void) {
  BuiltWat w = Build(kWatSeparation);
  TASK_CHECK(w.ok);
  std::string wasm = WriteCaseWasm(w, "consumer-fail.wasm");

  CliResult bad_root = RunCli("--root 999", wasm, "badroot");
  TASK_CHECK(bad_root.exit_code != 0);
  TASK_CHECK(Contains(bad_root.err, "invalid root"));

  CliResult missing = RunCli("--root 0",
                             RepoRoot() + "/.task-build/does-not-exist.wasm",
                             "missing");
  TASK_CHECK(missing.exit_code != 0);

  const char* loop_wat = R"WAT(
(module (func $f loop br 0 end) (func $r call $f))
)WAT";
  BuiltWat lw = Build(loop_wat);
  TASK_CHECK(lw.ok);  // valid Wasm, just outside the analysis scope
  std::string lwasm = WriteCaseWasm(lw, "consumer-loop.wasm");
  CliResult loop_cli = RunCli("--root 1", lwasm, "loop");
  TASK_CHECK(loop_cli.exit_code != 0);
  TASK_CHECK(Contains(loop_cli.err, "unsupported"));

  // Library failure leaves the caller's report untouched.
  ReferenceFlow sentinel;
  sentinel.roots.push_back(123);
  sentinel.contexts.resize(1);
  Errors errors;
  Result rr = wabt::BuildReferenceFlow(lw.module, {1}, &sentinel, &errors);
  TASK_CHECK(wabt::Failed(rr));
  TASK_CHECK(!errors.empty());
  TASK_CHECK(sentinel.roots.size() == 1);
  TASK_CHECK(sentinel.roots[0] == 123);
  TASK_CHECK(sentinel.contexts.size() == 1);

  // Out-of-range root is also an Error without touching the report.
  ReferenceFlow sentinel2;
  Errors errors2;
  TASK_CHECK(wabt::Failed(wabt::BuildReferenceFlow(
      w.module, {9999}, &sentinel2, &errors2)));
  TASK_CHECK(sentinel2.contexts.empty());
  return 0;
}

static int consumer_ownership_and_readonly(void) {
  ReferenceFlow flow;
  std::vector<uint8_t> before;
  std::vector<uint8_t> fixture_bytes;
  {
    Module module;
    Errors errors;
    std::vector<uint8_t> bytes;
    bool ok = wabt::Succeeded(
        fixture::RoundTrip(kWatSeparation, &module, &errors, &bytes));
    TASK_CHECK(ok);
    fixture_bytes = bytes;
    before = WriteModuleBytes(module);
    TASK_CHECK(wabt::Succeeded(
        wabt::BuildReferenceFlow(module, {3, 4}, &flow, &errors)));
    std::vector<uint8_t> after = WriteModuleBytes(module);
    TASK_CHECK(before == after);  // analysis does not mutate the Module
  }
  // The input Module is gone; the report owns all data and stays readable.
  TASK_CHECK(flow.contexts.size() == 6);
  TASK_CHECK((flow.roots == std::vector<size_t>{4, 5}));
  TASK_CHECK(flow.contexts[4].function == 3);
  TASK_CHECK(flow.contexts[4].calls.size() == 2);
  TASK_CHECK(CheckRv(flow.contexts[2].arguments[0], R({0})));
  TASK_CHECK(!fixture_bytes.empty());
  return 0;
}

// ===========================================================================
// Additional precision / ordering checks
// ===========================================================================

static int contexts_provided_reference_inputs(void) {
  std::string text =
      fixture::Load((RepoRoot() + "/task_support/reference-inputs.wat").c_str());
  TASK_CHECK(!text.empty());
  BuiltWat w;
  w.ok = wabt::Succeeded(fixture::RoundTrip(text, &w.module, &w.errors,
                                            &w.bytes));
  TASK_CHECK(w.ok);
  TASK_CHECK(w.module.funcs.size() == 16);
  TASK_CHECK(w.module.num_func_imports == 1);

  ReferenceFlow flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {4, 5}, &flow, &w.errors)));
  TASK_CHECK(flow.contexts.size() == 6);
  TASK_CHECK((flow.roots == std::vector<size_t>{4, 5}));
  size_t relay_a = FindCtx(flow, 3, {R({1})});
  size_t relay_b = FindCtx(flow, 3, {R({2})});
  TASK_CHECK(relay_a == 2);
  TASK_CHECK(relay_b == 3);
  TASK_CHECK(CheckCall(flow.contexts[4], 2, true, {1}));
  TASK_CHECK(CheckCall(flow.contexts[5], 2, true, {2}));

  ReferenceFlow null_flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {13}, &null_flow, &w.errors)));
  TASK_CHECK(null_flow.contexts.size() == 1);
  TASK_CHECK(!null_flow.contexts[0].may_return);
  const FlowCall* call = CallAt(null_flow.contexts[0], 1);
  TASK_CHECK(call != nullptr);
  TASK_CHECK(call->may_trap_null);
  TASK_CHECK(call->targets.empty());
  return 0;
}

static int contexts_root_order_library(void) {
  BuiltWat w = Build(kWatSeparation);
  TASK_CHECK(w.ok);
  ReferenceFlow a, b;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {3, 4}, &a, &w.errors)));
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {4, 3}, &b, &w.errors)));
  TASK_CHECK(FlowEqual(a, b));
  TASK_CHECK((a.roots == std::vector<size_t>{4, 5}));
  return 0;
}

static int contexts_multi_arg_tuples(void) {
  const char* wat = R"WAT(
(module
  (type $cb (func))
  (func $a (type $cb))
  (func $b (type $cb))
  (elem declare func $a $b)
  (func $sink2 (param $x (ref null $cb)) (param $y (ref null $cb)))
  (func $p1 ref.func $a ref.func $b call $sink2)
  (func $p2 ref.func $b ref.func $a call $sink2)
)
)WAT";
  BuiltWat w = Build(wat);
  TASK_CHECK(w.ok);
  ReferenceFlow flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {3, 4}, &flow, &w.errors)));
  TASK_CHECK(flow.contexts.size() == 4);
  size_t ab = FindCtx(flow, 2, {R({0}), R({1})});
  size_t ba = FindCtx(flow, 2, {R({1}), R({0})});
  TASK_CHECK(ab == 0);  // lexicographically ({0},{1}) first
  TASK_CHECK(ba == 1);
  const FlowContext& p1 = flow.contexts[FindCtx0(flow, 3)];
  const FlowContext& p2 = flow.contexts[FindCtx0(flow, 4)];
  TASK_CHECK(p1.calls[0].callee_contexts == std::vector<size_t>{ab});
  TASK_CHECK(p2.calls[0].callee_contexts == std::vector<size_t>{ba});
  TASK_CHECK((flow.roots == std::vector<size_t>{2, 3}));
  return 0;
}

static int locals_as_non_null(void) {
  const char* wat = R"WAT(
(module
  (type $cb (func))
  (func $a (type $cb))
  (func $b (type $cb))
  (elem declare func $a $b)
  (func $nn (local $p (ref null $cb))
    ref.func $a local.set $p
    local.get $p ref.as_non_null
    call_ref $cb)
  (func $kill
    ref.null $cb ref.as_non_null
    call_ref $cb
    ref.func $b call_ref $cb)
)
)WAT";
  BuiltWat w = Build(wat);
  TASK_CHECK(w.ok);
  ReferenceFlow flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {2, 3}, &flow, &w.errors)));

  const FlowContext& nn = flow.contexts[FindCtx0(flow, 2)];
  const FlowCall* call = CallAt(nn, 4);
  TASK_CHECK(call != nullptr);
  TASK_CHECK((call->targets == std::vector<Index>{0}));
  TASK_CHECK(!call->may_trap_null);

  const FlowContext& kill = flow.contexts[FindCtx0(flow, 3)];
  TASK_CHECK(!kill.may_return);
  TASK_CHECK(kill.calls.empty());  // both call_refs unreachable
  return 0;
}

static int recursion_return_call_ref(void) {
  const char* wat = R"WAT(
(module
  (type $cb (func))
  (type $mk (func (result (ref null $cb))))
  (func $a (type $cb))
  (func $gen (type $mk) ref.func $a)
  (elem declare func $a $gen)
  (func $via (result (ref null $cb))
    ref.func $gen return_call_ref $mk)
)
)WAT";
  BuiltWat w = Build(wat);
  TASK_CHECK(w.ok);
  ReferenceFlow flow;
  TASK_CHECK(wabt::Succeeded(
      wabt::BuildReferenceFlow(w.module, {2}, &flow, &w.errors)));
  TASK_CHECK(flow.contexts.size() == 2);
  size_t gen = FindCtx0(flow, 1);
  size_t via = FindCtx0(flow, 2);
  TASK_CHECK(gen == 0);
  TASK_CHECK(via == 1);
  const FlowContext& vc = flow.contexts[via];
  TASK_CHECK(vc.may_return);
  TASK_CHECK(vc.results.size() == 1);
  TASK_CHECK(CheckRv(vc.results[0], R({0})));
  TASK_CHECK(vc.calls.size() == 1);
  TASK_CHECK(vc.calls[0].indirect);
  TASK_CHECK(vc.calls[0].expr_ordinal == 1);
  TASK_CHECK((vc.calls[0].targets == std::vector<Index>{1}));
  TASK_CHECK(vc.calls[0].callee_contexts == std::vector<size_t>{gen});
  return 0;
}

// ---------------------------------------------------------------------------
// Registry consumed by task_support/driver.c
// ---------------------------------------------------------------------------

const task_case task_cases[] = {
    {"contexts", "provided_reference_inputs",
     contexts_provided_reference_inputs},
    {"contexts", "separation", contexts_separation},
    {"contexts", "cross_call_return", contexts_cross_call_return},
    {"contexts", "imports_and_top", contexts_imports_and_top},
    {"contexts", "root_order_library", contexts_root_order_library},
    {"contexts", "multi_arg_tuples", contexts_multi_arg_tuples},
    {"locals", "strong_overwrite", locals_strong_overwrite},
    {"locals", "branch_merge", locals_branch_merge},
    {"locals", "select", locals_select},
    {"locals", "multi_results", locals_multi_results},
    {"locals", "as_non_null", locals_as_non_null},
    {"recursion", "mutual_exit", recursion_mutual_exit},
    {"recursion", "pure", recursion_pure},
    {"recursion", "null_only", recursion_null_only},
    {"recursion", "tail", recursion_tail},
    {"recursion", "prune_and_reanalyze", recursion_prune_and_reanalyze},
    {"recursion", "return_call_ref", recursion_return_call_ref},
    {"consumer", "cli_matches_library", consumer_cli_matches_library},
    {"consumer", "roots_order_and_dupes", consumer_roots_order_and_dupes},
    {"consumer", "failures", consumer_failures},
    {"consumer", "ownership_and_readonly", consumer_ownership_and_readonly},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
