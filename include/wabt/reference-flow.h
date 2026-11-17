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

#ifndef WABT_REFERENCE_FLOW_H_
#define WABT_REFERENCE_FLOW_H_

#include <cstddef>
#include <vector>

#include "wabt/common.h"
#include "wabt/error.h"
#include "wabt/result.h"

namespace wabt {

struct Module;

// An abstract set of possible function-reference values for one reference
// typed slot.  Numeric values never appear here; they stay on the abstract
// operand stack as untyped scalar slots.
struct RefValue {
  // Sorted, deduplicated complete function indices (including the function
  // import prefix). Null is never encoded as function 0.
  std::vector<Index> functions;
  // The slot may hold a null reference.
  bool may_be_null = false;
  // The slot may hold a function reference whose identity is not known to
  // this module (an external/opaque function). Coexists with the known
  // in-module set.
  bool external = false;
};

// One static call site. The identity of the site is its fixed pre-order
// expression ordinal inside the owning function, never a scan counter or an
// IR pointer.
struct FlowCall {
  size_t expr_ordinal = 0;
  // true for call_ref/return_call_ref, false for call/return_call.
  bool indirect = false;
  // Concrete in-module candidate function indices, sorted and deduplicated.
  std::vector<Index> targets;
  // Indices into ReferenceFlow::contexts for the contexts of the concrete
  // (local or known-import) candidates, in the same order as |targets|.
  std::vector<size_t> callee_contexts;
  // An unknown external function is also a candidate.
  bool external = false;
  // For indirect calls, the reference operand may be null, so the call may
  // trap before invoking anything.
  bool may_trap_null = false;
};

// One analysis context: a function analyzed for one tuple of reference
// arguments. Numeric arguments do not participate in the context key.
struct FlowContext {
  Index function = 0;
  // Reference arguments only, in original parameter order.
  std::vector<RefValue> arguments;
  // Whether at least one path reaches the function tail or a return with a
  // complete result list. Void functions have this flag too.
  bool may_return = false;
  // One entry per declared result position; scalar positions hold a bottom
  // (empty) RefValue.
  std::vector<RefValue> results;
  // Call sites, sorted by expr_ordinal.
  std::vector<FlowCall> calls;
};

struct ReferenceFlow {
  // Indices into |contexts|, sorted and deduplicated.
  std::vector<size_t> roots;
  std::vector<FlowContext> contexts;
};

// Build a read-only function-reference flow report for |module| starting at
// |roots|. The module must already have names resolved and be valid; binary
// modules must come through ReadBinaryIr plus ValidateModule, with the
// function_references feature enabled.
//
// This never instantiates, runs or interprets the module, calls imports, or
// inspects tables or memory. On failure Result::Error is returned with
// diagnostics in |errors| and |out| is left untouched. The returned report
// owns all of its data and does not reference the module afterwards.
Result BuildReferenceFlow(const Module& module,
                          const std::vector<Index>& roots,
                          ReferenceFlow* out,
                          Errors* errors);

}  // namespace wabt

#endif /* WABT_REFERENCE_FLOW_H_ */
