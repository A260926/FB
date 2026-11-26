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

#include "wabt/reference-flow.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "wabt/cast.h"
#include "wabt/ir.h"
#include "wabt/string-format.h"

namespace wabt {

namespace {

// ---------------------------------------------------------------------------
// RefValue lattice helpers
// ---------------------------------------------------------------------------

bool IsBottom(const RefValue& v) {
  return v.functions.empty() && !v.may_be_null && !v.external;
}

// Strict total order used for context keys and final sorting.  The function
// set is compared lexicographically first, then the null and external bits.
int CompareRef(const RefValue& a, const RefValue& b) {
  size_t n = std::min(a.functions.size(), b.functions.size());
  for (size_t i = 0; i < n; ++i) {
    if (a.functions[i] != b.functions[i]) {
      return a.functions[i] < b.functions[i] ? -1 : 1;
    }
  }
  if (a.functions.size() != b.functions.size()) {
    return a.functions.size() < b.functions.size() ? -1 : 1;
  }
  if (a.may_be_null != b.may_be_null) {
    return a.may_be_null < b.may_be_null ? -1 : 1;  // false before true
  }
  if (a.external != b.external) {
    return a.external < b.external ? -1 : 1;
  }
  return 0;
}

int CompareRefTuples(const std::vector<RefValue>& a,
                     const std::vector<RefValue>& b) {
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    int c = CompareRef(a[i], b[i]);
    if (c != 0) {
      return c;
    }
  }
  if (a.size() != b.size()) {
    return a.size() < b.size() ? -1 : 1;
  }
  return 0;
}

void SortUniqueFunctions(RefValue& v) {
  std::sort(v.functions.begin(), v.functions.end());
  v.functions.erase(std::unique(v.functions.begin(), v.functions.end()),
                    v.functions.end());
}

// Joins `src` into `dst`; returns true when `dst` grew.
bool GrowRef(RefValue& dst, const RefValue& src) {
  RefValue old = dst;
  std::vector<Index> merged;
  merged.reserve(dst.functions.size() + src.functions.size());
  std::set_union(dst.functions.begin(), dst.functions.end(),
                 src.functions.begin(), src.functions.end(),
                 std::back_inserter(merged));
  dst.functions = std::move(merged);
  dst.may_be_null = dst.may_be_null || src.may_be_null;
  dst.external = dst.external || src.external;
  return CompareRef(old, dst) != 0;
}

RefValue JoinRef(const RefValue& a, const RefValue& b) {
  RefValue r = a;
  GrowRef(r, b);
  return r;
}

RefValue SingleFunc(Index f) {
  RefValue v;
  v.functions.push_back(f);
  return v;
}

RefValue NullOnly() {
  RefValue v;
  v.may_be_null = true;
  return v;
}

// ---------------------------------------------------------------------------
// Type classification and structural function-type compatibility
// ---------------------------------------------------------------------------

enum class RefKind {
  NonRef,
  Func,     // function reference; `ft` is null for generic funcref
  OtherRef  // extern/exn/GC reference, not tracked by this analysis
};

RefKind Classify(Type t, const Module& module, const FuncType** ft = nullptr) {
  if (ft) {
    *ft = nullptr;
  }
  if (!t.IsRef()) {
    return RefKind::NonRef;
  }
  if (t == Type::FuncRef) {
    return RefKind::Func;  // generic funcref/(ref func), nullability via Type
  }
  if (t.IsReferenceWithIndex()) {
    Index ti = t.GetReferenceIndex();
    if (ti < module.types.size()) {
      if (const FuncType* fty = dyn_cast<FuncType>(module.types[ti])) {
        if (ft) {
          *ft = fty;
        }
        return RefKind::Func;
      }
    }
    return RefKind::OtherRef;
  }
  return RefKind::OtherRef;
}

bool IsFuncRefType(Type t, const Module& module) {
  return Classify(t, module) == RefKind::Func;
}

// Structural subtyping on the restricted value-type set.  Indexed reference
// types are compared through their definitions rather than their type
// indices, as required by the task contract.
bool IsSubtype(Type a, Type b, const Module& module);

bool SignatureSubtype(const FuncSignature& sa,
                      const FuncSignature& sb,
                      const Module& module) {
  if (sa.param_types.size() != sb.param_types.size() ||
      sa.result_types.size() != sb.result_types.size()) {
    return false;
  }
  // Parameters are contravariant: sb.param <: sa.param.
  for (size_t i = 0; i < sa.param_types.size(); ++i) {
    if (!IsSubtype(sb.param_types[i], sa.param_types[i], module)) {
      return false;
    }
  }
  // Results are covariant: sa.result <: sb.result.
  for (size_t i = 0; i < sa.result_types.size(); ++i) {
    if (!IsSubtype(sa.result_types[i], sb.result_types[i], module)) {
      return false;
    }
  }
  return true;
}

bool IsSubtype(Type a, Type b, const Module& module) {
  if (a == b) {
    return true;
  }
  if (!a.IsRef() || !b.IsRef()) {
    return false;  // scalar kinds only match exactly
  }
  // Nullable is a supertype of non-nullable.  For a <: b, b non-null
  // requires a non-null.
  if (a.IsNullableRef() && !b.IsNullableRef()) {
    return false;
  }

  const FuncType* fa = nullptr;
  const FuncType* fb = nullptr;
  RefKind ka = Classify(a, module, &fa);
  RefKind kb = Classify(b, module, &fb);

  if (kb == RefKind::Func) {
    if (b == Type::FuncRef) {
      // Generic `func` heap type is the top of all function references.
      return ka == RefKind::Func;
    }
    if (ka != RefKind::Func || !fa || !fb) {
      // A generic funcref is not a subtype of an indexed function type.
      return false;
    }
    return SignatureSubtype(fa->sig, fb->sig, module);
  }

  if (kb == RefKind::OtherRef) {
    if (ka != RefKind::OtherRef) {
      return false;
    }
    // Non-function heap types are outside the supported structural scope.
    // Identical indexed types already hit the `a == b` fast path; here only
    // equal generic heap kinds relate (nullability was checked above).
    return (a == Type::ExternRef && b == Type::ExternRef) ||
           (a == Type::ExnRef && b == Type::ExnRef);
  }
  return false;
}

// Top abstract value for a declared function-reference type: every module
// function whose signature is compatible, plus external and null components
// according to the declaration.
RefValue TopFor(Type t, const Module& module) {
  RefValue out;
  const FuncType* expected = nullptr;
  if (Classify(t, module, &expected) != RefKind::Func) {
    return out;
  }
  for (Index f = 0; f < module.funcs.size(); ++f) {
    const Func* fn = module.funcs[f];
    bool compatible = true;
    if (expected) {
      compatible = SignatureSubtype(fn->decl.sig, expected->sig, module);
    }
    if (compatible) {
      out.functions.push_back(f);
    }
  }
  SortUniqueFunctions(out);
  out.external = true;
  out.may_be_null = t.IsNullableRef();
  return out;
}

// ---------------------------------------------------------------------------
// Abstract stack/local slots
// ---------------------------------------------------------------------------

struct Slot {
  bool is_ref = false;
  RefValue ref;

  static Slot Scalar() { return Slot{}; }
  static Slot Ref(RefValue v) {
    Slot s;
    s.is_ref = true;
    s.ref = std::move(v);
    return s;
  }
};

Slot JoinSlots(const Slot& a, const Slot& b) {
  if (a.is_ref && b.is_ref) {
    return Slot::Ref(JoinRef(a.ref, b.ref));
  }
  if (a.is_ref || b.is_ref) {
    // Validated modules only merge equal static types; this is a defensive
    // fallback that keeps the reference information when one side has it.
    return a.is_ref ? a : b;
  }
  return Slot::Scalar();
}

std::vector<RefValue> MakeContextArgs(const FuncSignature& sig,
                                      const std::vector<Slot>& args,
                                      const Module& module) {
  std::vector<RefValue> out;
  for (size_t i = 0; i < sig.param_types.size(); ++i) {
    Type pt = sig.param_types[i];
    if (!IsFuncRefType(pt, module)) {
      continue;
    }
    if (i < args.size() && args[i].is_ref) {
      out.push_back(args[i].ref);
    } else {
      // Defensive: should be unreachable for a validated module.  Use top so
      // no possible target is silently dropped.
      out.push_back(TopFor(pt, module));
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Internal context
// ---------------------------------------------------------------------------

struct Context {
  Index function = kInvalidIndex;
  std::vector<RefValue> arguments;
  bool import = false;
  bool may_return = false;
  std::vector<RefValue> results;
  // callee_contexts hold internal context ids until finalization.
  std::vector<FlowCall> calls;
};

struct ContextKey {
  Index function;
  std::vector<RefValue> arguments;
};

struct ContextKeyLess {
  bool operator()(const ContextKey& a, const ContextKey& b) const {
    if (a.function != b.function) {
      return a.function < b.function;
    }
    return CompareRefTuples(a.arguments, b.arguments) < 0;
  }
};

struct LState {
  std::vector<Slot> stack;
  std::vector<Slot> locals;
  bool live = true;
};

struct Frame {
  std::vector<FlowCall> calls;
  bool normal_return = false;
  std::vector<RefValue> return_results;
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

class Engine {
 public:
  Engine(const Module& module, Errors* errors)
      : module_(module), errors_(errors) {
    prepared_.assign(module_.funcs.size(), false);
  }

  Result Run(const std::vector<Index>& root_funcs,
             ReferenceFlow* out) {
    std::vector<size_t> roots;
    roots.reserve(root_funcs.size());
    for (Index f : root_funcs) {
      std::vector<RefValue> args;
      const Func* fn = module_.funcs[f];
      for (Type t : fn->decl.sig.param_types) {
        if (IsFuncRefType(t, module_)) {
          args.push_back(TopFor(t, module_));
        }
      }
      size_t id = RequestContext(f, std::move(args));
      if (!ok_) {
        return Result::Error;
      }
      roots.push_back(id);
    }

    // Monotone fixed point.  Every pass re-analyzes all contexts; new
    // contexts appended during a pass are analyzed in the same sweep.  A
    // pass that neither grows a summary nor creates a context reaches the
    // fixed point.  The domain is finite (subsets of module functions x
    // null/external bits per argument/result position), so no depth cutoff
    // is needed and recursion cycles are handled by revisiting callers.
    while (ok_) {
      bool grew = false;
      size_t generation_at_start = generation_;
      for (size_t i = 0; i < contexts_.size(); ++i) {
        if (!ok_) {
          return Result::Error;
        }
        if (contexts_[i]->import) {
          continue;
        }
        if (AnalyzeContext(i)) {
          grew = true;
        }
      }
      if (!grew && generation_ == generation_at_start) {
        break;
      }
    }
    if (!ok_) {
      return Result::Error;
    }

    *out = Finalize(roots);
    return Result::Ok;
  }

 private:
  const Module& module_;
  Errors* errors_;
  bool ok_ = true;
  std::vector<char> prepared_;
  std::unordered_map<const Expr*, size_t> ordinals_;
  std::vector<std::unique_ptr<Context>> contexts_;
  std::map<ContextKey, size_t, ContextKeyLess> key_map_;
  size_t generation_ = 0;

  void Fail(const Location& loc,
            Index function,
            size_t ordinal,
            const std::string& message) {
    ok_ = false;
    if (errors_) {
      errors_->emplace_back(
          ErrorLevel::Error, loc, module_.filename,
          StringPrintf("reference-flow: function %u expr %zu: %s",
                       static_cast<unsigned>(function), ordinal,
                       message.c_str()));
    }
  }

  size_t Ordinal(const Expr& expr) const {
    auto it = ordinals_.find(&expr);
    if (it != ordinals_.end()) {
      return it->second;
    }
    return 0;
  }

  // ----- body numbering / support check -----------------------------------

  static bool IsSupported(ExprType type) {
    switch (type) {
      case ExprType::Const:
      case ExprType::Nop:
      case ExprType::Drop:
      case ExprType::Select:
      case ExprType::LocalGet:
      case ExprType::LocalSet:
      case ExprType::LocalTee:
      case ExprType::RefFunc:
      case ExprType::RefNull:
      case ExprType::RefIsNull:
      case ExprType::RefAsNonNull:
      case ExprType::Unary:
      case ExprType::Binary:
      case ExprType::Compare:
      case ExprType::Convert:
      case ExprType::Block:
      case ExprType::If:
      case ExprType::Return:
      case ExprType::Unreachable:
      case ExprType::Call:
      case ExprType::CallRef:
      case ExprType::ReturnCall:
      case ExprType::ReturnCallRef:
        return true;
      default:
        return false;
    }
  }

  void PrepareList(const ExprList& exprs,
                   Index function,
                   size_t& next,
                   bool& failed) {
    for (const Expr& expr : exprs) {
      size_t ordinal = next++;
      ordinals_.emplace(&expr, ordinal);

      if (!IsSupported(expr.type())) {
        Fail(expr.loc, function, ordinal,
             std::string("unsupported expression: ") +
                 GetExprTypeName(expr.type()));
        failed = true;
        return;
      }

      switch (expr.type()) {
        case ExprType::Block: {
          const auto& be = cast<BlockExpr>(&expr);
          PrepareList(be->block.exprs, function, next, failed);
          break;
        }
        case ExprType::If: {
          const auto& ie = cast<IfExpr>(&expr);
          PrepareList(ie->true_.exprs, function, next, failed);
          if (failed) {
            return;
          }
          PrepareList(ie->false_, function, next, failed);
          break;
        }
        default:
          break;
      }
      if (failed) {
        return;
      }
    }
  }

  void PrepareFunction(Index function) {
    if (function >= prepared_.size() || prepared_[function]) {
      return;
    }
    prepared_[function] = true;
    const Func* fn = module_.funcs[function];
    size_t next = 0;
    bool failed = false;
    PrepareList(fn->exprs, function, next, failed);
  }

  // ----- contexts ----------------------------------------------------------

  size_t RequestContext(Index function, std::vector<RefValue> arguments) {
    ContextKey lookup{function, arguments};
    auto found = key_map_.find(lookup);
    if (found != key_map_.end()) {
      return found->second;
    }

    auto ctx = std::make_unique<Context>();
    ctx->function = function;
    ctx->arguments = std::move(arguments);
    ctx->import = function < module_.num_func_imports;
    const Func* fn = module_.funcs[function];
    ctx->results.resize(fn->GetNumResults());

    if (ctx->import) {
      // Imports are not executed; assume normal return and give reference
      // results the same top treatment as root reference arguments.
      ctx->may_return = true;
      for (size_t i = 0; i < fn->decl.sig.result_types.size(); ++i) {
        Type rt = fn->decl.sig.result_types[i];
        if (IsFuncRefType(rt, module_)) {
          ctx->results[i] = TopFor(rt, module_);
        }
      }
    }

    size_t id = contexts_.size();
    ContextKey key{function, ctx->arguments};
    key_map_.emplace(std::move(key), id);
    contexts_.push_back(std::move(ctx));
    ++generation_;

    if (!contexts_[id]->import) {
      PrepareFunction(function);
    }
    return id;
  }

  // ----- stack helpers -----------------------------------------------------

  static Slot Pop(LState& state) {
    if (state.stack.empty()) {
      // Defensive: a validated module never underflows on a live path.
      return Slot::Scalar();
    }
    Slot s = std::move(state.stack.back());
    state.stack.pop_back();
    return s;
  }

  static std::vector<Slot> PopArgs(LState& state, size_t count) {
    std::vector<Slot> args(count);
    for (size_t i = count; i > 0; --i) {
      args[i - 1] = Pop(state);
    }
    return args;
  }

  void PushResults(LState& state,
                   const FuncSignature& sig,
                   const std::vector<RefValue>& merged) {
    for (size_t i = 0; i < sig.result_types.size(); ++i) {
      if (IsFuncRefType(sig.result_types[i], module_)) {
        RefValue v = i < merged.size() ? merged[i] : RefValue{};
        state.stack.push_back(Slot::Ref(std::move(v)));
      } else {
        state.stack.push_back(Slot::Scalar());
      }
    }
  }

  // Merges the normal-return summary of `callee_id` into `any_return` and
  // `merged` (only called for candidates that may return normally).
  void CollectCalleeResults(size_t callee_id,
                            bool& any_return,
                            std::vector<RefValue>& merged) {
    Context* callee = contexts_[callee_id].get();
    if (!callee->may_return) {
      return;
    }
    any_return = true;
    for (size_t i = 0; i < merged.size() && i < callee->results.size(); ++i) {
      GrowRef(merged[i], callee->results[i]);
    }
  }

  // ----- analysis ----------------------------------------------------------

  bool AnalyzeContext(size_t id) {
    Context& self = *contexts_[id];
    const Func* fn = module_.funcs[self.function];
    active_function_ = self.function;

    LState state;
    Index num_params = fn->GetNumParams();
    state.locals.resize(fn->GetNumParamsAndLocals());

    size_t arg_index = 0;
    for (Index i = 0; i < fn->GetNumParamsAndLocals(); ++i) {
      Type t = fn->GetLocalType(i);
      if (!IsFuncRefType(t, module_)) {
        state.locals[i] = Slot::Scalar();
        continue;
      }
      if (i < num_params) {
        RefValue v = arg_index < self.arguments.size()
                         ? self.arguments[arg_index]
                         : RefValue{};
        ++arg_index;
        state.locals[i] = Slot::Ref(std::move(v));
      } else {
        // Nullable reference locals start as null; (validation-permitted)
        // non-null locals start at bottom.
        RefValue init;
        if (t.IsNullableRef()) {
          init.may_be_null = true;
        }
        state.locals[i] = Slot::Ref(std::move(init));
      }
    }

    Frame run;
    run.return_results.resize(fn->GetNumResults());
    EvalList(fn->exprs, state, run);

    if (ok_ && state.live) {
      AccumulateResults(state, fn->decl.sig, run);
    }

    bool grew = false;
    if (run.normal_return && !self.may_return) {
      self.may_return = true;
      grew = true;
    }
    for (size_t i = 0; i < self.results.size() &&
                        i < run.return_results.size();
         ++i) {
      if (GrowRef(self.results[i], run.return_results[i])) {
        grew = true;
      }
    }
    self.calls = std::move(run.calls);
    return grew;
  }

  void AccumulateResults(LState& state,
                         const FuncSignature& sig,
                         Frame& run) {
    run.normal_return = true;
    size_t n = sig.result_types.size();
    if (run.return_results.size() < n) {
      run.return_results.resize(n);
    }
    size_t base = state.stack.size() >= n ? state.stack.size() - n : 0;
    for (size_t i = 0; i < n; ++i) {
      Type rt = sig.result_types[i];
      if (!IsFuncRefType(rt, module_)) {
        continue;  // scalar result position stays bottom
      }
      if (base + i < state.stack.size() && state.stack[base + i].is_ref) {
        GrowRef(run.return_results[i], state.stack[base + i].ref);
      } else {
        GrowRef(run.return_results[i], TopFor(rt, module_));
      }
    }
  }

  void EvalList(const ExprList& exprs, LState& state, Frame& run) {
    for (const Expr& expr : exprs) {
      if (!ok_) {
        return;
      }
      if (!state.live) {
        break;  // dead zone: no further effects or published call sites
      }
      EvalExpr(expr, state, run);
    }
  }

  void EvalExpr(const Expr& expr, LState& state, Frame& run) {
    switch (expr.type()) {
      case ExprType::Const:
        state.stack.push_back(Slot::Scalar());
        return;

      case ExprType::Nop:
        return;

      case ExprType::Unreachable:
        state.live = false;
        return;

      case ExprType::Drop:
        Pop(state);
        return;

      case ExprType::Unary:
      case ExprType::Convert:
        Pop(state);
        state.stack.push_back(Slot::Scalar());
        return;

      case ExprType::Binary:
      case ExprType::Compare:
        Pop(state);
        Pop(state);
        state.stack.push_back(Slot::Scalar());
        return;

      case ExprType::RefIsNull:
        Pop(state);
        state.stack.push_back(Slot::Scalar());
        return;

      case ExprType::RefFunc: {
        const auto& e = cast<RefFuncExpr>(&expr);
        Index f = module_.GetFuncIndex(e->var);
        state.stack.push_back(Slot::Ref(SingleFunc(f)));
        return;
      }

      case ExprType::RefNull: {
        const auto& e = cast<RefNullExpr>(&expr);
        Type t = e->type.to_type();
        if (IsFuncRefType(t, module_)) {
          state.stack.push_back(Slot::Ref(NullOnly()));
        } else {
          state.stack.push_back(Slot::Scalar());
        }
        return;
      }

      case ExprType::RefAsNonNull: {
        Slot s = Pop(state);
        if (!s.is_ref) {
          state.stack.push_back(Slot::Scalar());
          return;
        }
        RefValue v = s.ref;
        v.may_be_null = false;  // strong removal of the null component
        if (IsBottom(v)) {
          // Definitely null (or otherwise bottom): traps, no continuation.
          state.live = false;
          return;
        }
        state.stack.push_back(Slot::Ref(std::move(v)));
        return;
      }

      case ExprType::Select: {
        Pop(state);  // condition (unknown numeric value)
        Slot second = Pop(state);
        Slot first = Pop(state);
        state.stack.push_back(JoinSlots(first, second));
        return;
      }

      case ExprType::LocalGet: {
        const auto& e = cast<LocalGetExpr>(&expr);
        Index i = module_.funcs[active_function_]->GetLocalIndex(e->var);
        if (i < state.locals.size()) {
          state.stack.push_back(state.locals[i]);
        }
        return;
      }

      case ExprType::LocalSet: {
        const auto& e = cast<LocalSetExpr>(&expr);
        Index i = module_.funcs[active_function_]->GetLocalIndex(e->var);
        Slot v = Pop(state);
        if (i < state.locals.size()) {
          state.locals[i] = std::move(v);  // strong overwrite
        }
        return;
      }

      case ExprType::LocalTee: {
        const auto& e = cast<LocalTeeExpr>(&expr);
        Index i = module_.funcs[active_function_]->GetLocalIndex(e->var);
        Slot v = Pop(state);
        if (i < state.locals.size()) {
          state.locals[i] = v;  // strong overwrite
        }
        state.stack.push_back(std::move(v));
        return;
      }

      case ExprType::Block: {
        const auto& e = cast<BlockExpr>(&expr);
        // Block parameters are already on the stack; the signature only
        // matters for result positions, which the simulated stack leaves.
        EvalList(e->block.exprs, state, run);
        return;
      }

      case ExprType::If: {
        const auto& e = cast<IfExpr>(&expr);
        Pop(state);  // condition: never evaluated numerically
        LState base = state;

        LState true_state = base;
        EvalList(e->true_.exprs, true_state, run);

        LState false_state = base;  // missing else == identity arm
        EvalList(e->false_, false_state, run);

        std::vector<LState*> live_arms;
        if (true_state.live) {
          live_arms.push_back(&true_state);
        }
        if (false_state.live) {
          live_arms.push_back(&false_state);
        }
        if (live_arms.empty()) {
          state.live = false;
          return;
        }

        size_t stack_size = live_arms[0]->stack.size();
        for (size_t a = 1; a < live_arms.size(); ++a) {
          stack_size = std::min(stack_size, live_arms[a]->stack.size());
        }
        state.stack.resize(stack_size);
        for (size_t i = 0; i < stack_size; ++i) {
          Slot merged = live_arms[0]->stack[i];
          for (size_t a = 1; a < live_arms.size(); ++a) {
            if (i < live_arms[a]->stack.size()) {
              merged = JoinSlots(merged, live_arms[a]->stack[i]);
            }
          }
          state.stack[i] = std::move(merged);
        }

        size_t local_size = state.locals.size();
        for (size_t i = 0; i < local_size; ++i) {
          Slot merged = live_arms[0]->locals[i];
          for (size_t a = 1; a < live_arms.size(); ++a) {
            if (i < live_arms[a]->locals.size()) {
              merged = JoinSlots(merged, live_arms[a]->locals[i]);
            }
          }
          state.locals[i] = std::move(merged);
        }
        state.live = true;
        return;
      }

      case ExprType::Return: {
        const Func* fn = module_.funcs[active_function_];
        AccumulateResults(state, fn->decl.sig, run);
        state.live = false;
        return;
      }

      case ExprType::Call: {
        const auto& e = cast<CallExpr>(&expr);
        Index f = module_.GetFuncIndex(e->var);
        if (f >= module_.funcs.size()) {
          return;
        }
        const FuncSignature& sig = module_.funcs[f]->decl.sig;
        std::vector<Slot> args = PopArgs(state, sig.param_types.size());
        std::vector<RefValue> key = MakeContextArgs(sig, args, module_);
        size_t callee = RequestContext(f, std::move(key));

        FlowCall call;
        call.expr_ordinal = Ordinal(expr);
        call.indirect = false;
        call.targets = {f};
        call.callee_contexts = {callee};
        run.calls.push_back(std::move(call));

        bool any_return = false;
        std::vector<RefValue> merged(sig.result_types.size());
        CollectCalleeResults(callee, any_return, merged);
        if (!any_return) {
          state.live = false;
          return;
        }
        PushResults(state, sig, merged);
        return;
      }

      case ExprType::ReturnCall: {
        const auto& e = cast<ReturnCallExpr>(&expr);
        Index f = module_.GetFuncIndex(e->var);
        if (f >= module_.funcs.size()) {
          state.live = false;
          return;
        }
        const FuncSignature& sig = module_.funcs[f]->decl.sig;
        std::vector<Slot> args = PopArgs(state, sig.param_types.size());
        std::vector<RefValue> key = MakeContextArgs(sig, args, module_);
        size_t callee = RequestContext(f, std::move(key));

        FlowCall call;
        call.expr_ordinal = Ordinal(expr);
        call.indirect = false;
        call.targets = {f};
        call.callee_contexts = {callee};
        run.calls.push_back(std::move(call));

        CollectCalleeResults(callee, run.normal_return, run.return_results);
        state.live = false;  // tail call has no normal continuation
        return;
      }

      case ExprType::CallRef:
      case ExprType::ReturnCallRef: {
        const bool tail = expr.type() == ExprType::ReturnCallRef;
        const Var* sig_var = nullptr;
        if (!tail) {
          sig_var = &cast<CallRefExpr>(&expr)->sig_type;
        } else {
          sig_var = &cast<ReturnCallRefExpr>(&expr)->sig_type;
        }
        Index sig_index = module_.GetFuncTypeIndex(*sig_var);
        const FuncSignature* sig = nullptr;
        if (sig_index < module_.types.size()) {
          if (const FuncType* ft =
                  dyn_cast<FuncType>(module_.types[sig_index])) {
            sig = &ft->sig;
          }
        }
        if (!sig) {
          // Validated modules always resolve; treat as no continuation.
          Pop(state);
          state.live = false;
          return;
        }

        Slot callee_slot = Pop(state);  // stack top: function reference
        std::vector<Slot> args = PopArgs(state, sig->param_types.size());

        RefValue callee;
        if (callee_slot.is_ref) {
          callee = callee_slot.ref;
        } else {
          // Defensive sound fallback: top for the expected reference type.
          Type expected(Type::RefNull, sig_index);
          callee = TopFor(expected, module_);
        }

        FlowCall call;
        call.expr_ordinal = Ordinal(expr);
        call.indirect = true;
        call.targets = callee.functions;  // already sorted/deduped
        call.external = callee.external;
        call.may_trap_null = callee.may_be_null;

        bool any_return = false;
        std::vector<RefValue> merged(sig->result_types.size());

        call.callee_contexts.reserve(callee.functions.size());
        for (Index f : callee.functions) {
          if (f >= module_.funcs.size()) {
            continue;
          }
          const FuncSignature& fsig = module_.funcs[f]->decl.sig;
          std::vector<RefValue> key = MakeContextArgs(fsig, args, module_);
          size_t callee_ctx = RequestContext(f, std::move(key));
          call.callee_contexts.push_back(callee_ctx);
          CollectCalleeResults(callee_ctx, any_return, merged);
        }

        if (callee.external) {
          // Unknown external target: import-style top results.
          any_return = true;
          for (size_t i = 0; i < sig->result_types.size(); ++i) {
            if (IsFuncRefType(sig->result_types[i], module_)) {
              GrowRef(merged[i], TopFor(sig->result_types[i], module_));
            }
          }
        }

        std::sort(call.callee_contexts.begin(), call.callee_contexts.end());
        call.callee_contexts.erase(
            std::unique(call.callee_contexts.begin(),
                         call.callee_contexts.end()),
            call.callee_contexts.end());
        run.calls.push_back(std::move(call));

        if (tail) {
          if (any_return) {
            run.normal_return = true;
            for (size_t i = 0;
                 i < run.return_results.size() && i < merged.size(); ++i) {
              GrowRef(run.return_results[i], merged[i]);
            }
          }
          state.live = false;
          return;
        }

        if (!any_return) {
          // Null-only traps and bottom calls have no normal continuation;
          // non-returning concrete candidates contribute nothing either.
          state.live = false;
          return;
        }
        PushResults(state, *sig, merged);
        return;
      }

      default:
        // PrepareList rejects everything else before analysis runs.
        return;
    }
  }

  // The function currently being analyzed.  Evaluation is depth-first and
  // never nests another function analysis, so a single member is enough to
  // resolve local indices.
  Index active_function_ = 0;

  // ----- finalization ------------------------------------------------------

  ReferenceFlow Finalize(const std::vector<size_t>& root_ids) {
    // Reachability over final callee_context edges.
    std::vector<char> reachable(contexts_.size(), false);
    std::vector<size_t> worklist;
    for (size_t id : root_ids) {
      if (id < contexts_.size() && !reachable[id]) {
        reachable[id] = true;
        worklist.push_back(id);
      }
    }
    while (!worklist.empty()) {
      size_t id = worklist.back();
      worklist.pop_back();
      for (const FlowCall& call : contexts_[id]->calls) {
        for (size_t to : call.callee_contexts) {
          if (to < contexts_.size() && !reachable[to]) {
            reachable[to] = true;
            worklist.push_back(to);
          }
        }
      }
    }

    std::vector<size_t> kept;
    for (size_t id = 0; id < contexts_.size(); ++id) {
      if (reachable[id]) {
        kept.push_back(id);
      }
    }
    std::sort(kept.begin(), kept.end(), [&](size_t a, size_t b) {
      const ContextKeyLess less;
      return less(ContextKey{contexts_[a]->function, contexts_[a]->arguments},
                  ContextKey{contexts_[b]->function, contexts_[b]->arguments});
    });

    std::vector<size_t> remap(contexts_.size(),
                              std::numeric_limits<size_t>::max());
    for (size_t ni = 0; ni < kept.size(); ++ni) {
      remap[kept[ni]] = ni;
    }

    ReferenceFlow result;
    result.contexts.resize(kept.size());
    for (size_t ni = 0; ni < kept.size(); ++ni) {
      const Context& src = *contexts_[kept[ni]];
      FlowContext& dst = result.contexts[ni];
      dst.function = src.function;
      dst.arguments = src.arguments;
      dst.may_return = src.may_return;
      dst.results = src.results;
      dst.calls.reserve(src.calls.size());
      for (const FlowCall& src_call : src.calls) {
        FlowCall call = src_call;
        std::vector<size_t> mapped;
        mapped.reserve(call.callee_contexts.size());
        for (size_t internal_id : call.callee_contexts) {
          if (internal_id < remap.size() &&
              remap[internal_id] != std::numeric_limits<size_t>::max()) {
            mapped.push_back(remap[internal_id]);
          }
        }
        std::sort(mapped.begin(), mapped.end());
        mapped.erase(std::unique(mapped.begin(), mapped.end()), mapped.end());
        call.callee_contexts = std::move(mapped);
        dst.calls.push_back(std::move(call));
      }
      std::sort(dst.calls.begin(), dst.calls.end(),
                [](const FlowCall& a, const FlowCall& b) {
                  return a.expr_ordinal < b.expr_ordinal;
                });
    }

    for (size_t id : root_ids) {
      if (id < remap.size() &&
          remap[id] != std::numeric_limits<size_t>::max()) {
        result.roots.push_back(remap[id]);
      }
    }
    std::sort(result.roots.begin(), result.roots.end());
    result.roots.erase(std::unique(result.roots.begin(), result.roots.end()),
                       result.roots.end());
    return result;
  }
};

}  // namespace

Result BuildReferenceFlow(const Module& module,
                          const std::vector<Index>& roots,
                          ReferenceFlow* out,
                          Errors* errors) {
  if (!out) {
    return Result::Error;
  }
  for (Index root : roots) {
    if (root >= module.funcs.size()) {
      if (errors) {
        errors->emplace_back(
            ErrorLevel::Error, Location(), module.filename,
            StringPrintf("reference-flow: invalid root function index %u "
                         "(module has %zu functions)",
                         static_cast<unsigned>(root), module.funcs.size()));
      }
      return Result::Error;
    }
  }

  std::vector<Index> unique_roots = roots;
  std::sort(unique_roots.begin(), unique_roots.end());
  unique_roots.erase(
      std::unique(unique_roots.begin(), unique_roots.end()),
      unique_roots.end());

  Engine engine(module, errors);
  return engine.Run(unique_roots, out);
}

}  // namespace wabt
