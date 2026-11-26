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

// Read-only abstract flow analysis for function references.
//
// BuildReferenceFlow walks the IR produced by ReadBinaryIr (after validation)
// and summarizes, per (function, reference-argument tuple) context, which
// function indices may be returned normally and which concrete/external
// targets each call/call_ref site may reach.  It never instantiates or runs
// the module and never inspects tables, memories or imports' effects.
//
// See docs/reference-flow.md and task_support/reference-flow.md for the
// abstract domain and the supported input scope.

#ifndef WABT_REFERENCE_FLOW_H_
#define WABT_REFERENCE_FLOW_H_

#include <cstddef>
#include <vector>

#include "wabt/base-types.h"
#include "wabt/error.h"
#include "wabt/result.h"

namespace wabt {

struct Module;

// An abstract function-reference value.
//
// `functions` is the sorted, deduplicated set of complete function indices
// (function imports occupy the leading indices).  `external` records an
// additional unknown function originating outside this module's index space;
// it coexists with known indices.  `may_be_null` is a separate possibility:
// null is never represented as function 0.
//
// An empty `functions` vector with both booleans false is bottom (no known
// normal value).  In FlowContext::results a bottom entry at a scalar result
// position simply marks that position scalar.
struct RefValue {
  std::vector<Index> functions;
  bool may_be_null = false;
  bool external = false;
};

// One call/call_ref/return_call/return_call_ref site inside a context.
struct FlowCall {
  // Fixed depth-first preorder ordinal of the expression in its function
  // body (control expressions are numbered before their children; dead code
  // is numbered too, but unreachable call sites are not published).
  size_t expr_ordinal = 0;
  // False for call/return_call, true for call_ref/return_call_ref.
  bool indirect = false;
  // Concrete module function-index candidates.
  std::vector<Index> targets;
  // Indices into ReferenceFlow::contexts of the resolved target contexts
  // (local functions and known function imports).
  std::vector<size_t> callee_contexts;
  // The reference may also be an unknown external function.
  bool external = false;
  // The reference may be null, which traps a call_ref.
  bool may_trap_null = false;
};

struct FlowContext {
  Index function = 0;
  // Only function-reference parameters, in declaration order.
  std::vector<RefValue> arguments;
  // Whether at least one analysis path returns/falls through normally.
  bool may_return = false;
  // One entry per declared result position; scalar positions are bottom.
  std::vector<RefValue> results;
  std::vector<FlowCall> calls;
};

struct ReferenceFlow {
  // Sorted context indices of the requested root functions.
  std::vector<size_t> roots;
  // Sorted and reachability-pruned contexts.
  std::vector<FlowContext> contexts;
};

// Runs the analysis on an already-parsed/resolved/validated Module.
//
// `roots` contains complete function indices (imports allowed) that receive
// top reference arguments.  On success fills `*out` and returns Ok.  On
// invalid roots or unsupported body nodes returns Error, appends to
// `errors` when non-null and leaves `*out` untouched.  The Module is not
// modified and may be released after the call: the report owns all data.
Result BuildReferenceFlow(const Module& module,
                          const std::vector<Index>& roots,
                          ReferenceFlow* out,
                          Errors* errors);

}  // namespace wabt

#endif  // WABT_REFERENCE_FLOW_H_
