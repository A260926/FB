# Function-reference flow analysis

`BuildReferenceFlow` is a read-only abstract analysis over the WABT `Module`
IR (normally produced by `ReadBinaryIr` and re-validated with
`ValidateModule`).  It answers a narrow question:

> For a given calling context (a function and the abstract values of its
> function-reference parameters), which concrete functions can a
> `call`/`call_ref`/`return_call`/`return_call_ref` site reach, and which
> function references can the context return normally?

It does **not** instantiate the module, execute or interpret any function,
call host imports, inspect tables/memory/globals, or read process state.
Every result is owned by the returned `ReferenceFlow`; the `Module` may be
destroyed after the call.

```cpp
#include "wabt/reference-flow.h"

wabt::Result wabt::BuildReferenceFlow(const Module& module,
                                      const std::vector<Index>& roots,
                                      ReferenceFlow* out,
                                      Errors* errors);
```

`roots` are complete function indices (function imports occupy the leading
indices).  Root reference parameters receive **top**: every module function
structurally compatible with the declared reference type, plus the unknown
`external` component, plus `may_be_null` when the declaration is nullable.

## Abstract values

`RefValue` is a may-set of function identities plus two independent flags:

- `functions`: sorted, deduplicated complete function indices, including
  known function imports and `ref.func` identities;
- `external`: an additional unknown function from outside this module's
  function index space;
- `may_be_null`: the value may be null.  Null is never encoded as function 0.

`functions == {} && !may_be_null && !external` is **bottom** (no known normal
value).  It is distinct from a value that is normally a non-null or a
null-only reference.  Scalar result positions are bottom `RefValue` entries
in `FlowContext::results`, which always has one entry per declared result.

Numeric values carry no information but still occupy their stack slots, so
operand alignment is preserved.  Numeric and `select` conditions are not
evaluated: both `if` arms are analyzed from the same stack/locals snapshot.

## Contexts and the fixed point

A context key is `(function, reference-argument tuple)`.  Only
function-reference parameters participate in the tuple, in declaration order.
The same tuple shares one context; different tuples are independent, which is
what keeps one helper called with `ref.func $a` apart from the same helper
called with `ref.func $b`.

Local contexts start with `may_return == false` and bottom results.  Import
contexts are immediately `may_return == true`; their reference results are
top.  Analysis iterates to a monotone fixed point:

- summaries only grow (function sets / null / external join by union);
- new argument tuples create new contexts;
- whenever a summary grows, all dependent contexts are re-analyzed;
- there is no recursion-depth cutoff and recursion is never widened to top
  merely because it is cyclic.

Pure recursion keeps `may_return == false`.  Mutual recursion with a real
return arm propagates the reference set back to the earliest caller.  After
convergence, contexts unreachable from the final roots through final
`callee_contexts` edges are removed (this drops transient contexts created
with a smaller argument set), and the remaining contexts are sorted by
`(function index, argument tuple)` and reindexed.

## Statements modeled

- `local.get`/`local.set`/`local.tee`: `set`/`tee` strongly overwrite.
  Nullable reference locals start as null; non-null locals start at bottom.
- `ref.func`, `ref.null`, `ref.is_null` (scalar only), `ref.as_non_null`
  (removes the null component; bottom after removal means a definite trap and
  no normal continuation).
- `select`/typed `select`: joins the two candidate references; the condition
  is unknown.
- `block`/`if` with their real (multi-value) signatures.  Both `if` arms
  start from the same post-condition stack and locals snapshot; only normally
  continuing arms join.  An arm that returns, traps (`unreachable`), or calls
  a context that cannot return does not contribute to the join.  A missing
  `else` is the identity arm.
- `call` and `call_ref` consume their full declared parameter list; for
  `call_ref` the stack-top operand is the callee reference.  Each concrete
  target gets its own context for the current arguments.  Only candidates with
  `may_return == true` contribute return values.  An `external` component
  contributes import-style top results.
- A null component sets `FlowCall::may_trap_null` but no callee.  A
  null-only or bottom callee has no normal continuation; its call record is
  still published (even when it is null-only), while call sites after the
  point where the abstract state dies are not published.
- `return_call`/`return_call_ref` feed the callee summary directly into the
  current context summary and leave no normal fall-through.
- Explicit `unreachable` and definite null-only `call_ref` /
  `ref.as_non_null` kill continuation.  Possible traps of numeric operations
  are not modeled and do not remove paths.

Supported body nodes are exactly: `const`, `nop`, `drop`, `select`,
`local.get/set/tee`, `ref.func/null/is_null/as_non_null`, scalar
`unary/binary/compare/convert`, `block`, `if`, `return`, `unreachable`,
`call`, `call_ref`, `return_call`, `return_call_ref`.  `loop`,
`br/br_if/br_table`, `call_indirect`, table/memory/global operations,
exception handling, SIMD and any other node produce `Result::Error` with the
complete function index, the fixed expression ordinal and the expression
`Location` when available.  The whole body is checked even in dead code.

Expression ordinals are fixed depth-first preorder numbers from 0 per
function body: the control expression is numbered before its children, and
for `if` the true block is numbered before the false block.  `else`/`end` do
not occupy numbers.  Dead-code nodes still receive ordinals.

Function types are compared structurally (parameters/results and reference
nullability), never by type index.  The supported type scope excludes GC
struct/array types, recursive type groups and explicit subtype declarations.

## CLI consumer

`wasm-ref-flow` is a read-only binary consumer:

```
wasm-ref-flow FILE --root N [--root N ...] [--root=N ...]
```

It reads and validates the binary, calls `BuildReferenceFlow`, and writes
deterministic line-delimited JSON: one object per sorted context, then a final
`{"roots":[...]}` line.  Each context line has

```json
{"context":0,"function":3,"arguments":[{"functions":[1],"may_be_null":false,"external":false}],
 "may_return":true,
 "results":[{"functions":[1],"may_be_null":false,"external":false}],
 "calls":[{"expr_ordinal":2,"indirect":true,"targets":[1],
           "callee_contexts":[0],"external":false,"may_trap_null":false}]}
```

and the last line is e.g. `{"roots":[4,5]}`.  Failures print diagnostics and
exit non-zero.

## Precision limits

The analysis is context-sensitive only in function-reference parameter
values; scalar parameters do not distinguish contexts.  Control flow is
flow-sensitive within a body but path-insensitive at joins (all live arms
merge), and numeric predicates are never decided.  Unknown behavior is
represented only by the documented `external`/top sources; recursion and
unresolved flow are never silently promoted to top.  Tables, memories,
globals, imports' bodies and all unsupported instructions are outside the
model (unsupported instructions are a reported error rather than an
optimistic skip).
