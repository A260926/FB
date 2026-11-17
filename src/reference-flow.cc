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

#include "wabt/reference-flow.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "wabt/cast.h"
#include "wabt/ir.h"
#include "wabt/string-format.h"

namespace wabt {

namespace {

// ---------------------------------------------------------------------------
// RefValue lattice operations
// ---------------------------------------------------------------------------

bool IsBottom(const RefValue& v) {
  return v.functions.empty() && !v.may_be_null && !v.external;
}

// Destructive join: *a grows to contain everything in b. Returns true if it
// changed. The function index domain is finite, so repeated joins terminate.
bool JoinInto(RefValue* a, const RefValue& b) {
  std::vector<Index> merged;
  merged.reserve(a->functions.size() + b.functions.size());
  std::merge(a->functions.begin(), a->functions.end(),
             b.functions.begin(), b.functions.end(),
             std::back_inserter(merged));
  merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
  bool changed = merged != a->functions;
  a->functions = std::move(merged);
  changed |= (b.may_be_null && !a->may_be_null);
  changed |= (b.external && !a->external);
  a->may_be_null |= b.may_be_null;
  a->external |= b.external;
  return changed;
}

bool Equal(const RefValue& a, const RefValue& b) {
  return a.functions == b.functions && a.may_be_null == b.may_be_null &&
         a.external == b.external;
}

// True only for function-reference typed slots (funcref or an indexed
// reference to a function type). externref and other GC references are opaque
// to this analysis and never carry function identities.
bool IsFuncRefType(const Module& module, const Type& type) {
  if (!type.IsRef()) return false;
  if (type == Type::FuncRef) return true;
  if (type.IsReferenceWithIndex()) {
    Index index = type.GetReferenceIndex();
    return index < module.types.size() &&
           module.types[index]->kind() == TypeEntryKind::Func;
  }
  return false;
}

// Lexicographic order for context sorting: function set sequence first, then
// the null possibility, then the external possibility.
bool Less(const RefValue& a, const RefValue& b) {
  if (a.functions != b.functions) return a.functions < b.functions;
  if (a.may_be_null != b.may_be_null) return a.may_be_null < b.may_be_null;
  return a.external < b.external;
}

// ---------------------------------------------------------------------------
// Structural type comparison (no GC, no recursive type groups, no subtyping
// declarations: identical parameter/result structure and reference
// nullability, never compared by type index alone).
// ---------------------------------------------------------------------------

class TypeMatcher {
 public:
  explicit TypeMatcher(const Module& module) : module_(module) {}

  bool TypesEqual(const Type& a, const Type& b) {
    if (a == b) return true;
    if (!a.IsRef() || !b.IsRef()) return false;
    if (a.IsNullableRef() != b.IsNullableRef()) return false;

    bool a_indexed = a.IsReferenceWithIndex();
    bool b_indexed = b.IsReferenceWithIndex();
    if (a_indexed != b_indexed) {
      // Non-typed funcref vs an indexed reference to a func type are not the
      // same structural type.
      return false;
    }
    if (!a_indexed) {
      return static_cast<Type::Enum>(a) == static_cast<Type::Enum>(b);
    }

    const auto* fa = GetFuncType(a.GetReferenceIndex());
    const auto* fb = GetFuncType(b.GetReferenceIndex());
    if (!fa || !fb) return false;
    return SignaturesEqual(fa->sig, fb->sig);
  }

  bool SignaturesEqual(const FuncSignature& a, const FuncSignature& b) {
    if (a.param_types.size() != b.param_types.size() ||
        a.result_types.size() != b.result_types.size()) {
      return false;
    }
    for (size_t i = 0; i < a.param_types.size(); ++i) {
      if (!TypesEqual(a.param_types[i], b.param_types[i])) return false;
    }
    for (size_t i = 0; i < a.result_types.size(); ++i) {
      if (!TypesEqual(a.result_types[i], b.result_types[i])) return false;
    }
    return true;
  }

  // True if a function whose type is |fn| is a valid value of reference type
  // |target| (nullability of the slot is irrelevant for a concrete function).
  bool FunctionFits(const FuncSignature& fn, const Type& target) {
    if (target == Type::FuncRef) return true;
    if (target.IsReferenceWithIndex()) {
      const auto* ft = GetFuncType(target.GetReferenceIndex());
      return ft && SignaturesEqual(fn, ft->sig);
    }
    if (target == Type::Reference) return true;
    return false;
  }

 private:
  const FuncType* GetFuncType(Index index) {
    if (index >= module_.types.size()) return nullptr;
    TypeEntry* entry = module_.types[index];
    return entry->kind() == TypeEntryKind::Func
               ? static_cast<FuncType*>(entry)
               : nullptr;
  }

  const Module& module_;
};

// ---------------------------------------------------------------------------
// Abstract operand stack
// ---------------------------------------------------------------------------

struct Slot {
  bool scalar = false;  // Numeric value with no known constant information.
  RefValue ref;
};

struct State {
  std::vector<Slot> stack;
  // One entry per parameter/local; numeric positions stay bottom and are only
  // accessed when the declared local type is a reference.
  std::vector<RefValue> locals;
};

struct InternalCall {
  size_t expr_ordinal = 0;
  bool indirect = false;
  std::vector<Index> targets;
  std::vector<size_t> callee_contexts;
  bool external = false;
  bool may_trap_null = false;
};

bool Equal(const InternalCall& a, const InternalCall& b) {
  return a.expr_ordinal == b.expr_ordinal && a.indirect == b.indirect &&
         a.targets == b.targets && a.callee_contexts == b.callee_contexts &&
         a.external == b.external && a.may_trap_null == b.may_trap_null;
}

struct ContextRecord {
  Index function = 0;
  std::vector<RefValue> arguments;
  bool is_import = false;
  bool may_return = false;
  std::vector<RefValue> results;
  std::vector<InternalCall> calls;
  bool queued = false;
};

// Per-function data computed once: fixed pre-order ordinals and the supported
// syntax range check, which covers dead code as well.
struct FunctionInfo {
  std::unordered_map<const Expr*, size_t> ordinals;
  bool checked = false;
  bool supported = false;
};

class FlowAnalyzer {
 public:
  FlowAnalyzer(const Module& module, Errors* errors)
      : module_(module), errors_(errors), matcher_(module) {}

  Result Build(const std::vector<Index>& roots, ReferenceFlow* out) {
    CHECK_RESULT(ValidateTypes());
    for (Index root : roots) {
      if (root >= module_.funcs.size()) {
        AddError(Location(),
                 StringPrintf("root function index %u is out of range "
                              "(module has %zu functions)",
                              root, module_.funcs.size()));
        return Result::Error;
      }
    }

    std::vector<Index> sorted_roots = roots;
    std::sort(sorted_roots.begin(), sorted_roots.end());
    sorted_roots.erase(std::unique(sorted_roots.begin(), sorted_roots.end()),
                       sorted_roots.end());

    std::vector<size_t> root_contexts;
    for (Index root : sorted_roots) {
      const Func* func = module_.funcs[root];
      std::vector<RefValue> args = TopArguments(func->decl);
      size_t id = GetOrCreateContext(root, args);
      root_contexts.push_back(id);
    }

    CHECK_RESULT(RunFixpoint());
    EmitReport(root_contexts, out);
    return Result::Ok;
  }

 private:
  // -------------------------------------------------------------------------
  // Prelude checks
  // -------------------------------------------------------------------------

  Result ValidateTypes() {
    for (size_t i = 0; i < module_.types.size(); ++i) {
      if (module_.types[i]->kind() != TypeEntryKind::Func) {
        AddError(module_.types[i]->loc,
                 StringPrintf("type %zu is a GC heap type; only function "
                              "types are supported by reference flow",
                              i));
        return Result::Error;
      }
    }
    // Recursive type groups are out of scope: structural comparison and the
    // top-value expansion must never follow a cycle of indexed references.
    std::vector<std::vector<Index>> edges(module_.types.size());
    for (size_t i = 0; i < module_.types.size(); ++i) {
      const auto* func_type = static_cast<const FuncType*>(module_.types[i]);
      CollectReferencedTypes(func_type->sig.param_types, &edges[i]);
      CollectReferencedTypes(func_type->sig.result_types, &edges[i]);
      std::sort(edges[i].begin(), edges[i].end());
      edges[i].erase(std::unique(edges[i].begin(), edges[i].end()),
                     edges[i].end());
    }
    // Iterative DFS coloring: 0=white, 1=gray(on stack), 2=black(done). A
    // gray reachable node is a back edge, hence a recursive type group.
    std::vector<int> color(module_.types.size(), 0);
    for (Index start = 0; start < module_.types.size(); ++start) {
      if (color[start] != 0) continue;
      std::vector<Index> stack;
      stack.push_back(start);
      while (!stack.empty()) {
        Index node = stack.back();
        if (color[node] == 0) color[node] = 1;
        bool descended = false;
        for (Index next : edges[node]) {
          if (next >= color.size()) continue;
          if (color[next] == 1) {
            AddError(module_.types[next]->loc,
                     StringPrintf("type %u belongs to a recursive type group; "
                                  "recursive reference types are not supported "
                                  "by reference flow",
                                  next));
            return Result::Error;
          }
          if (color[next] == 0) {
            stack.push_back(next);
            descended = true;
            break;  // strict depth-first; resume node when child finishes
          }
        }
        if (!descended) {
          color[node] = 2;
          stack.pop_back();
        }
      }
    }
    return Result::Ok;
  }

  static void CollectReferencedTypes(const TypeVector& types,
                                     std::vector<Index>* out) {
    for (const Type& type : types) {
      if (type.IsReferenceWithIndex()) {
        out->push_back(type.GetReferenceIndex());
      }
    }
  }

  // Walk every node of an expression list in fixed depth-first pre-order,
  // assigning ordinals and checking the supported syntax range. Control
  // nodes receive the ordinal before their children; if numbers true before
  // false; else/end markers do not occupy ordinals.
  Result WalkForOrdinals(const ExprList& exprs, FunctionInfo* info,
                         Index func_index, size_t* ordinal) {
    for (const Expr& expr : exprs) {
      size_t this_ordinal = (*ordinal)++;
      info->ordinals.emplace(&expr, this_ordinal);
      if (!IsSupported(expr.type())) {
        AddError(expr.loc,
                 StringPrintf("function %u expression %zu (%s) is not in "
                              "the supported reference-flow syntax range",
                              func_index, this_ordinal,
                              GetExprTypeName(expr)));
        return Result::Error;
      }
      switch (expr.type()) {
        case ExprType::Block: {
          const auto* block = &cast<BlockExpr>(&expr)->block;
          CHECK_RESULT(
              WalkForOrdinals(block->exprs, info, func_index, ordinal));
          break;
        }
        case ExprType::If: {
          const auto* if_expr = cast<IfExpr>(&expr);
          CHECK_RESULT(WalkForOrdinals(if_expr->true_.exprs, info, func_index,
                                       ordinal));
          CHECK_RESULT(
              WalkForOrdinals(if_expr->false_, info, func_index, ordinal));
          break;
        }
        case ExprType::Loop: {
          const auto* block = &cast<LoopExpr>(&expr)->block;
          CHECK_RESULT(
              WalkForOrdinals(block->exprs, info, func_index, ordinal));
          break;
        }
        case ExprType::Try: {
          const auto* try_expr = cast<TryExpr>(&expr);
          CHECK_RESULT(
              WalkForOrdinals(try_expr->block.exprs, info, func_index,
                              ordinal));
          for (const Catch& catch_ : try_expr->catches) {
            CHECK_RESULT(
                WalkForOrdinals(catch_.exprs, info, func_index, ordinal));
          }
          break;
        }
        case ExprType::TryTable: {
          const auto* try_expr = cast<TryTableExpr>(&expr);
          CHECK_RESULT(
              WalkForOrdinals(try_expr->block.exprs, info, func_index,
                              ordinal));
          break;
        }
        default:
          break;
      }
    }
    return Result::Ok;
  }

  Result EnsureFunctionChecked(Index func_index) {
    if (func_index >= func_info_.size()) func_info_.resize(func_index + 1);
    FunctionInfo* info = &func_info_[func_index];
    if (info->checked) {
      return info->supported ? Result::Ok : Result::Error;
    }
    info->checked = true;
    const Func* func = module_.funcs[func_index];
    size_t ordinal = 0;
    Result result = WalkForOrdinals(func->exprs, info, func_index, &ordinal);
    info->supported = Succeeded(result);
    return result;
  }

  static bool IsSupported(ExprType type) {
    switch (type) {
      case ExprType::Nop:
      case ExprType::Drop:
      case ExprType::Const:
      case ExprType::Unary:
      case ExprType::Binary:
      case ExprType::Compare:
      case ExprType::Convert:
      case ExprType::Select:
      case ExprType::LocalGet:
      case ExprType::LocalSet:
      case ExprType::LocalTee:
      case ExprType::RefFunc:
      case ExprType::RefNull:
      case ExprType::RefIsNull:
      case ExprType::RefAsNonNull:
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

  // -------------------------------------------------------------------------
  // Top values (root arguments, import results, unknown external results)
  // -------------------------------------------------------------------------

  RefValue TopRefValue(const Type& type) {
    RefValue value;
    value.may_be_null = type.IsNullableRef();
    value.external = true;
    for (Index i = 0; i < module_.funcs.size(); ++i) {
      const Func* func = module_.funcs[i];
      if (matcher_.FunctionFits(func->decl.sig, type)) {
        value.functions.push_back(i);
      }
    }
    std::sort(value.functions.begin(), value.functions.end());
    value.functions.erase(
        std::unique(value.functions.begin(), value.functions.end()),
        value.functions.end());
    return value;
  }

  std::vector<RefValue> TopArguments(const FuncDeclaration& decl) {
    std::vector<RefValue> args;
    for (Type type : decl.sig.param_types) {
      if (IsFuncRefType(module_, type)) args.push_back(TopRefValue(type));
    }
    return args;
  }

  std::vector<RefValue> TopResults(const FuncDeclaration& decl) {
    std::vector<RefValue> results;
    results.reserve(decl.GetNumResults());
    for (Type type : decl.sig.result_types) {
      if (IsFuncRefType(module_, type)) {
        results.push_back(TopRefValue(type));
      } else {
        results.emplace_back();
      }
    }
    return results;
  }

  // -------------------------------------------------------------------------
  // Context management
  // -------------------------------------------------------------------------

  bool SameArguments(const std::vector<RefValue>& a,
                     const std::vector<RefValue>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      if (!Equal(a[i], b[i])) return false;
    }
    return true;
  }

  size_t GetOrCreateContext(Index function,
                            const std::vector<RefValue>& arguments) {
    for (size_t id = 0; id < contexts_.size(); ++id) {
      if (contexts_[id].function == function &&
          SameArguments(contexts_[id].arguments, arguments)) {
        return id;
      }
    }

    ContextRecord record;
    record.function = function;
    record.arguments = arguments;
    const Func* func = module_.funcs[function];
    record.results.resize(func->GetNumResults());

    if (function < module_.num_func_imports) {
      // Imports are never executed; they are assumed to return normally with
      // top reference results.
      record.is_import = true;
      record.may_return = true;
      record.results = TopResults(func->decl);
    }

    size_t id = contexts_.size();
    contexts_.push_back(std::move(record));
    dependents_.emplace_back();
    if (!contexts_[id].is_import) Enqueue(id);
    return id;
  }

  void Enqueue(size_t id) {
    if (!contexts_[id].queued && !contexts_[id].is_import) {
      contexts_[id].queued = true;
      queue_.push_back(id);
    }
  }

  // -------------------------------------------------------------------------
  // Fixpoint driver
  // -------------------------------------------------------------------------

  Result RunFixpoint() {
    while (!queue_.empty()) {
      size_t id = queue_.back();
      queue_.pop_back();
      contexts_[id].queued = false;
      CHECK_RESULT(AnalyzeContext(id));
    }
    return Result::Ok;
  }

  Result AnalyzeContext(size_t id) {
    // No reference into contexts_ is held across the scan: the scan creates
    // further contexts, which can reallocate the vector.
    Index function = contexts_[id].function;
    CHECK_RESULT(EnsureFunctionChecked(function));
    const Func* func = module_.funcs[function];
    const FunctionInfo& info = func_info_[function];

    State state;
    state.locals.resize(func->GetNumParamsAndLocals());
    size_t ref_arg = 0;
    for (Index p = 0; p < func->GetNumParams(); ++p) {
      if (IsFuncRefType(module_, func->GetParamType(p))) {
        state.locals[p] = contexts_[id].arguments[ref_arg++];
      }
    }
    for (Index local = func->GetNumParams();
         local < func->GetNumParamsAndLocals(); ++local) {
      Type type = func->GetLocalType(local);
      if (IsFuncRefType(module_, type) && type.IsNullableRef()) {
        // Validated nullable references start as null; non-nullable ones are
        // bottom until first assigned.
        state.locals[local].may_be_null = true;
      }
    }

    Scan scan(this, id, info, func);
    bool reaches_end = Succeeded(scan.List(func->exprs, &state));
    if (reaches_end) {
      scan.RecordReturn(state.stack);
    }

    ContextRecord& context = contexts_[id];
    bool changed = context.may_return != scan.may_return;
    context.may_return = scan.may_return;
    for (size_t i = 0; i < context.results.size(); ++i) {
      changed |= JoinInto(&context.results[i], scan.results[i]);
    }
    if (context.calls.size() != scan.calls.size()) {
      changed = true;
    } else {
      for (size_t i = 0; i < context.calls.size(); ++i) {
        if (!Equal(context.calls[i], scan.calls[i])) changed = true;
      }
    }
    context.calls = std::move(scan.calls);

    if (changed) {
      for (size_t dependent : dependents_[id]) Enqueue(dependent);
    }
    return Result::Ok;
  }

  // -------------------------------------------------------------------------
  // Body scanning
  // -------------------------------------------------------------------------

  class Scan {
   public:
    Scan(FlowAnalyzer* analyzer, size_t context_id,
         const FunctionInfo& info, const Func* func)
        : analyzer_(analyzer),
          context_id_(context_id),
          info_(info),
          func_(func) {
      may_return = false;
      results.resize(func->GetNumResults());
    }

    // Analyze a list until it ends normally (Result::Ok) or until a node with
    // no normal continuation (return/unreachable/no-return call) is reached
    // (Result::Error, carrying no diagnostic).
    Result List(const ExprList& exprs, State* state) {
      for (const Expr& expr : exprs) {
        if (Failed(Step(expr, state))) return Result::Error;
      }
      return Result::Ok;
    }

    void RecordReturn(const std::vector<Slot>& stack) {
      may_return = true;
      size_t nresults = func_->GetNumResults();
      size_t base = stack.size() - nresults;
      for (size_t i = 0; i < nresults; ++i) {
        if (analyzer_->IsTrackedRef(
                func_->GetResultType(static_cast<Index>(i)))) {
          JoinInto(&results[i], stack[base + i].ref);
        }
      }
    }

    bool may_return;
    std::vector<RefValue> results;
    std::vector<InternalCall> calls;

   private:
    FlowAnalyzer* analyzer_;
    size_t context_id_;
    const FunctionInfo& info_;
    const Func* func_;

    size_t Ordinal(const Expr& expr) const {
      auto it = info_.ordinals.find(&expr);
      assert(it != info_.ordinals.end());
      return it->second;
    }

    static void PushScalar(State* state) { state->stack.push_back(Slot{}); }

    static void PushRef(State* state, const RefValue& value) {
      Slot slot;
      slot.ref = value;
      state->stack.push_back(std::move(slot));
    }

    static Slot Pop(State* state) {
      Slot slot = state->stack.back();
      state->stack.pop_back();
      return slot;
    }

    Result Step(const Expr& expr, State* state) {
      switch (expr.type()) {
        case ExprType::Nop:
          return Result::Ok;

        case ExprType::Drop:
          Pop(state);
          return Result::Ok;

        case ExprType::Const: {
          const auto& c = cast<ConstExpr>(&expr)->const_;
          if (c.type().IsRef()) {
            RefValue value;
            if (c.ref_bits() == Const::kRefNullBits) value.may_be_null = true;
            PushRef(state, value);
          } else {
            PushScalar(state);
          }
          return Result::Ok;
        }

        case ExprType::Unary:
          return ScalarOpcode(cast<UnaryExpr>(&expr)->opcode, state);
        case ExprType::Binary:
          return ScalarOpcode(cast<BinaryExpr>(&expr)->opcode, state);
        case ExprType::Compare:
          return ScalarOpcode(cast<CompareExpr>(&expr)->opcode, state);
        case ExprType::Convert:
          return ScalarOpcode(cast<ConvertExpr>(&expr)->opcode, state);

        case ExprType::Select:
          return DoSelect(cast<::wabt::SelectExpr>(&expr), state);

        case ExprType::LocalGet: {
          Index local = func_->GetLocalIndex(cast<LocalGetExpr>(&expr)->var);
          if (func_->GetLocalType(local).IsRef()) {
            PushRef(state, state->locals[local]);
          } else {
            PushScalar(state);
          }
          return Result::Ok;
        }

        case ExprType::LocalSet: {
          Index local = func_->GetLocalIndex(cast<LocalSetExpr>(&expr)->var);
          Slot value = Pop(state);
          if (func_->GetLocalType(local).IsRef()) {
            // Strong update: overwrite, do not union.
            state->locals[local] = value.ref;
          }
          return Result::Ok;
        }

        case ExprType::LocalTee: {
          Index local = func_->GetLocalIndex(cast<LocalTeeExpr>(&expr)->var);
          if (func_->GetLocalType(local).IsRef()) {
            state->locals[local] = state->stack.back().ref;
          }
          return Result::Ok;
        }

        case ExprType::RefFunc: {
          RefValue value;
          value.functions.push_back(
              cast<RefFuncExpr>(&expr)->var.index());
          PushRef(state, value);
          return Result::Ok;
        }

        case ExprType::RefNull:
          PushRef(state, RefValue{.may_be_null = true});
          return Result::Ok;

        case ExprType::RefIsNull:
          Pop(state);
          PushScalar(state);
          return Result::Ok;

        case ExprType::RefAsNonNull: {
          Slot value = Pop(state);
          value.ref.may_be_null = false;
          if (IsBottom(value.ref)) {
            // No possible non-null value: no normal continuation.
            return Result::Error;
          }
          state->stack.push_back(std::move(value));
          return Result::Ok;
        }

        case ExprType::Block:
          return DoBlock(cast<BlockExpr>(&expr)->block, state);

        case ExprType::If:
          return DoIf(cast<IfExpr>(&expr), state);

        case ExprType::Return:
          RecordReturn(state->stack);
          return Result::Error;

        case ExprType::Unreachable:
          return Result::Error;

        case ExprType::Call:
          return DirectCall(cast<CallExpr>(&expr), state,
                            /*tail=*/false);
        case ExprType::ReturnCall:
          return DirectCall(cast<ReturnCallExpr>(&expr), state,
                            /*tail=*/true);
        case ExprType::CallRef:
          return IndirectCall(cast<CallRefExpr>(&expr), state,
                              /*tail=*/false);
        case ExprType::ReturnCallRef:
          return IndirectCall(cast<ReturnCallRefExpr>(&expr), state,
                              /*tail=*/true);

        default:
          // The pre-pass guarantees this is unreachable.
          assert(false);
          return Result::Error;
      }
    }

    static Result ScalarOpcode(const Opcode& opcode, State* state) {
      int params = 0;
      for (int i = 1; i <= 3; ++i) {
        if (opcode.GetParamType(i) != Type::Void) ++params;
      }
      for (int i = 0; i < params; ++i) state->stack.pop_back();
      int results = (opcode.GetResultType() != Type::Void) +
                    (opcode.GetResultType2() != Type::Void);
      for (int i = 0; i < results; ++i) PushScalar(state);
      return Result::Ok;
    }

    Result DoSelect(const ::wabt::SelectExpr* expr, State* state) {
      Pop(state);  // Numeric condition, never evaluated.
      size_t count = expr->IsUntyped() ? 1 : expr->result_type.size();
      std::vector<Slot> right(count);
      std::vector<Slot> left(count);
      for (size_t i = 0; i < count; ++i) right[i] = Pop(state);
      for (size_t i = 0; i < count; ++i) left[i] = Pop(state);
      for (size_t i = 0; i < count; ++i) {
        if (left[i].scalar || right[i].scalar) {
          PushScalar(state);
        } else {
          RefValue merged = left[i].ref;
          JoinInto(&merged, right[i].ref);
          PushRef(state, merged);
        }
      }
      return Result::Ok;
    }

    Result DoBlock(const Block& block, State* state) {
      const FuncSignature& sig = block.decl.sig;
      std::vector<Slot> inputs;
      for (size_t i = 0; i < sig.param_types.size(); ++i) {
        inputs.push_back(Pop(state));
      }
      std::reverse(inputs.begin(), inputs.end());

      State inner;
      inner.stack = std::move(inputs);
      inner.locals = state->locals;
      if (Failed(List(block.exprs, &inner))) return Result::Error;

      size_t nresults = sig.result_types.size();
      assert(inner.stack.size() >= nresults);
      state->stack.insert(state->stack.end(),
                          inner.stack.end() -
                              static_cast<ptrdiff_t>(nresults),
                          inner.stack.end());
      state->locals = std::move(inner.locals);
      return Result::Ok;
    }

    Result DoIf(const IfExpr* expr, State* state) {
      const FuncSignature& sig = expr->true_.decl.sig;
      Pop(state);  // Numeric condition, conservatively unknown.
      std::vector<Slot> inputs;
      for (size_t i = 0; i < sig.param_types.size(); ++i) {
        inputs.push_back(Pop(state));
      }
      std::reverse(inputs.begin(), inputs.end());

      State true_state;
      true_state.stack = inputs;
      true_state.locals = state->locals;
      bool true_returns = Succeeded(List(expr->true_.exprs, &true_state));

      State false_state;
      false_state.stack = inputs;
      false_state.locals = state->locals;
      bool false_returns = Succeeded(List(expr->false_, &false_state));

      if (!true_returns && !false_returns) return Result::Error;

      size_t nresults = sig.result_types.size();
      std::vector<Slot> merged;
      State* donors[2] = {true_returns ? &true_state : nullptr,
                         false_returns ? &false_state : nullptr};
      for (State* donor : donors) {
        if (!donor) continue;
        assert(donor->stack.size() >= nresults);
        if (merged.empty()) {
          merged.assign(donor->stack.end() -
                            static_cast<ptrdiff_t>(nresults),
                        donor->stack.end());
        } else {
          for (size_t i = 0; i < nresults; ++i) {
            Slot& slot = merged[i];
            const Slot& other =
                donor->stack[donor->stack.size() - nresults + i];
            if (slot.scalar || other.scalar) {
              slot.scalar = true;
            } else {
              JoinInto(&slot.ref, other.ref);
            }
          }
        }
      }
      state->stack.insert(state->stack.end(), merged.begin(), merged.end());

      // Merge locals only from arms that complete normally.
      for (size_t i = 0; i < state->locals.size(); ++i) {
        RefValue combined;
        bool any = false;
        for (State* donor : donors) {
          if (!donor) continue;
          JoinInto(&combined, donor->locals[i]);
          any = true;
        }
        if (any) state->locals[i] = std::move(combined);
      }
      return Result::Ok;
    }

    // Pops the declared parameters of |sig|; function-reference arguments (in
    // original parameter order) go into |ref_args|. Other reference kinds
    // (e.g. externref) never name module functions and are not keyed.
    void PopArguments(const FuncSignature& sig, State* state,
                      std::vector<RefValue>* ref_args) {
      size_t nparams = sig.param_types.size();
      std::vector<Slot> popped(nparams);
      for (size_t i = 0; i < nparams; ++i) popped[i] = Pop(state);
      std::reverse(popped.begin(), popped.end());
      for (size_t i = 0; i < nparams; ++i) {
        if (analyzer_->IsTrackedRef(sig.param_types[i])) {
          ref_args->push_back(popped[i].ref);
        }
      }
    }

    Result DirectCall(const class Expr* expr, State* state, bool tail) {
      Index callee;
      if (expr->type() == ExprType::Call) {
        callee = cast<CallExpr>(expr)->var.index();
      } else {
        callee = cast<ReturnCallExpr>(expr)->var.index();
      }
      const Func* func = analyzer_->module_.funcs[callee];
      const FuncSignature& sig = func->decl.sig;

      std::vector<RefValue> ref_args;
      PopArguments(sig, state, &ref_args);
      size_t callee_context =
          analyzer_->GetOrCreateContext(callee, ref_args);
      analyzer_->NoteDependent(callee_context, context_id_);

      InternalCall call;
      call.expr_ordinal = Ordinal(*expr);
      call.indirect = false;
      call.targets.push_back(callee);
      call.callee_contexts.push_back(callee_context);
      calls.push_back(std::move(call));

      const ContextRecord& target =
          analyzer_->contexts_[callee_context];
      if (target.may_return) {
        if (tail) {
          AdoptReturn(target.results);
          return Result::Error;
        }
        PushResults(sig, target.results, state);
        return Result::Ok;
      }
      // A call that cannot return normally has no continuation.
      return Result::Error;
    }

    Result IndirectCall(const class Expr* expr, State* state, bool tail) {
      const Var& sig_var = expr->type() == ExprType::CallRef
                               ? cast<CallRefExpr>(expr)->sig_type
                               : cast<ReturnCallRefExpr>(expr)->sig_type;
      const FuncType* func_type = analyzer_->module_.GetFuncType(sig_var);
      assert(func_type);
      const FuncSignature& sig = func_type->sig;

      Slot reference = Pop(state);
      std::vector<RefValue> ref_args;
      PopArguments(sig, state, &ref_args);

      InternalCall call;
      call.expr_ordinal = Ordinal(*expr);
      call.indirect = true;
      call.may_trap_null = reference.ref.may_be_null;
      call.external = reference.ref.external;

      bool any_returning = false;
      std::vector<RefValue> merged_results(func_type->GetNumResults());
      for (Index candidate : reference.ref.functions) {
        const Func* func = analyzer_->module_.funcs[candidate];
        if (!analyzer_->matcher_.SignaturesEqual(func->decl.sig, sig)) {
          continue;
        }
        call.targets.push_back(candidate);
        size_t callee_context =
            analyzer_->GetOrCreateContext(candidate, ref_args);
        analyzer_->NoteDependent(callee_context, context_id_);
        call.callee_contexts.push_back(callee_context);
        const ContextRecord& target =
            analyzer_->contexts_[callee_context];
        if (target.may_return) {
          any_returning = true;
          for (size_t i = 0; i < merged_results.size(); ++i) {
            JoinInto(&merged_results[i], target.results[i]);
          }
        }
      }

      if (reference.ref.external) {
        // Unknown external candidate: import-style top results, assumed able
        // to return normally.
        any_returning = true;
        FuncDeclaration external_decl;
        external_decl.sig = sig;
        std::vector<RefValue> top =
            analyzer_->TopResults(external_decl);
        for (size_t i = 0; i < merged_results.size(); ++i) {
          JoinInto(&merged_results[i], top[i]);
        }
      }

      calls.push_back(std::move(call));

      if (!any_returning) {
        // Bottom or null-only reference: no concrete continuation. The call
        // record is still published; later sites are not.
        return Result::Error;
      }
      if (tail) {
        AdoptReturn(merged_results);
        return Result::Error;
      }
      PushResults(sig, merged_results, state);
      return Result::Ok;
    }

    void AdoptReturn(const std::vector<RefValue>& callee_results) {
      may_return = true;
      for (size_t i = 0; i < results.size(); ++i) {
        JoinInto(&results[i], callee_results[i]);
      }
    }

    static void PushResults(const FuncSignature& sig,
                            const std::vector<RefValue>& context_results,
                            State* state) {
      for (size_t i = 0; i < sig.result_types.size(); ++i) {
        if (sig.result_types[i].IsRef()) {
          PushRef(state, context_results[i]);
        } else {
          PushScalar(state);
        }
      }
    }
  };

  void NoteDependent(size_t callee_context, size_t dependent) {
    dependents_[callee_context].insert(dependent);
  }

  bool IsTrackedRef(const Type& type) const {
    return IsFuncRefType(module_, type);
  }

  // -------------------------------------------------------------------------
  // Finalization
  // -------------------------------------------------------------------------

  void EmitReport(const std::vector<size_t>& root_contexts,
                  ReferenceFlow* out) {
    // Keep only contexts reachable from the final roots through concrete
    // callee contexts; contexts that existed only for smaller intermediate
    // argument sets are discarded.
    std::vector<bool> reachable(contexts_.size(), false);
    std::vector<size_t> work = root_contexts;
    while (!work.empty()) {
      size_t id = work.back();
      work.pop_back();
      if (reachable[id]) continue;
      reachable[id] = true;
      for (const InternalCall& call : contexts_[id].calls) {
        for (size_t callee : call.callee_contexts) {
          if (!reachable[callee]) work.push_back(callee);
        }
      }
    }

    std::vector<size_t> kept;
    for (size_t id = 0; id < contexts_.size(); ++id) {
      if (reachable[id]) kept.push_back(id);
    }
    std::sort(kept.begin(), kept.end(), [&](size_t a, size_t b) {
      const ContextRecord& x = contexts_[a];
      const ContextRecord& y = contexts_[b];
      if (x.function != y.function) return x.function < y.function;
      size_t n = std::min(x.arguments.size(), y.arguments.size());
      for (size_t i = 0; i < n; ++i) {
        if (!Equal(x.arguments[i], y.arguments[i])) {
          return Less(x.arguments[i], y.arguments[i]);
        }
      }
      return x.arguments.size() < y.arguments.size();
    });

    // Build the full internal-id -> sorted-index remap before emitting so a
    // context that is emitted early can still name a callee ordered later.
    std::unordered_map<size_t, size_t> remap;
    remap.reserve(kept.size());
    for (size_t i = 0; i < kept.size(); ++i) {
      remap.emplace(kept[i], i);
    }

    ReferenceFlow report;
    for (size_t i = 0; i < kept.size(); ++i) {
      const ContextRecord& from = contexts_[kept[i]];
      FlowContext context;
      context.function = from.function;
      context.arguments = from.arguments;
      context.may_return = from.may_return;
      context.results = from.results;
      for (const InternalCall& call : from.calls) {
        FlowCall flow_call;
        flow_call.expr_ordinal = call.expr_ordinal;
        flow_call.indirect = call.indirect;
        flow_call.targets = call.targets;
        flow_call.external = call.external;
        flow_call.may_trap_null = call.may_trap_null;
        for (size_t callee : call.callee_contexts) {
          flow_call.callee_contexts.push_back(remap.at(callee));
        }
        context.calls.push_back(std::move(flow_call));
      }
      std::sort(context.calls.begin(), context.calls.end(),
                [](const FlowCall& a, const FlowCall& b) {
                  return a.expr_ordinal < b.expr_ordinal;
                });
      report.contexts.push_back(std::move(context));
    }

    for (size_t root : root_contexts) report.roots.push_back(remap.at(root));
    std::sort(report.roots.begin(), report.roots.end());
    report.roots.erase(
        std::unique(report.roots.begin(), report.roots.end()),
        report.roots.end());

    *out = std::move(report);
  }

  void AddError(const Location& loc, const std::string& message) {
    errors_->emplace_back(ErrorLevel::Error, loc, module_.filename, message);
  }

  const Module& module_;
  Errors* errors_;
  TypeMatcher matcher_;
  std::vector<FunctionInfo> func_info_;
  std::vector<ContextRecord> contexts_;
  std::vector<std::unordered_set<size_t>> dependents_;
  std::vector<size_t> queue_;
};

}  // namespace

Result BuildReferenceFlow(const Module& module,
                          const std::vector<Index>& roots,
                          ReferenceFlow* out,
                          Errors* errors) {
  FlowAnalyzer analyzer(module, errors);
  return analyzer.Build(roots, out);
}

}  // namespace wabt
