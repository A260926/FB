# Function-reference flow analysis

`BuildReferenceFlow` is a read-only, static analysis of where function
references flow inside a validated Wasm module. It is intended for tooling that
needs call-graph information for `call_ref` without ever running the module.

## What the analysis is

The analysis is an abstract interpretation over function-reference values.

- A reference slot is summarized by a `RefValue`: a set of concrete module
  function indices, plus two independent possibilities — the slot may be
  **null**, or it may hold an **external** function whose identity the module
  does not know.
- Numeric values carry no constant information. They occupy an abstract stack
  slot so that operand order is preserved, but numeric conditions are never
  evaluated and never prune a branch.
- A **context** is a function analyzed for one tuple of reference arguments.
  Numeric arguments do not participate in the context key, so the same helper
  invoked with different reference arguments produces separate contexts and
  separate summaries.

Contexts are analyzed to a least fixed point with a work queue: when a
callee's summary grows (new normal-return information, new result identities,
or new reachable call edges), every caller that depends on it is revisited.
There is no recursion depth cutoff and a recursive cycle is never widened
straight to top. All reference sets start at bottom and only grow through
actual data flow, so a pure recursion loop stays at bottom and is reported as
not returning normally.

## Precision guarantees

- **Context separation.** The same helper called with `ref.func A` in one
  entry and `ref.func B` in another yields two contexts; a downstream
  `call_ref` in each entry sees only the matching candidate, never a merged
  `{A, B}`.
- **Strong local update.** `local.set` overwrites a reference local rather
  than unioning into it; `local.tee` updates while leaving the value on the
  stack.
- **Branches.** Both arms of an `if` start from the same operand stack and a
  snapshot of the locals. Only arms that can complete normally contribute to
  the merged state; an arm that returns, traps, or ends in a non-returning
  call contributes no tail state. The numeric condition is treated as
  unknown, so both arms are kept.
- **`select`.** The condition is not evaluated; the result joins both
  candidate references (and both scalar possibilities for numeric selects).
- **`ref.as_non_null`.** Removes the null possibility; if nothing concrete
  remains, that point has no normal continuation.
- **Calls.** Direct and tail calls and `call_ref` / `return_call_ref` consume
  the full declared parameter list. Each concrete candidate is analyzed with
  the current reference arguments. Only candidates that can return normally
  contribute results to the continuation. An unknown external candidate uses
  import-style top results.
- **No normal return vs. null return.** These are distinct. A function that
  never reaches a normal exit (`may_return == false`) is different from one
  that returns normally a reference whose only known possibility is null.
- **Null-only `call_ref`.** A call whose reference can only be null keeps a
  call record with `may_trap_null` and no targets, and provides no normal
  continuation; later call sites in that straight-line path are not
  published.
- **Tail calls.** `return_call` / `return_call_ref` feed their results
  directly into the current function's summary; nothing after them has a
  normal continuation.

## Roots, imports and top

Root reference arguments and import reference results use a **top** value:

- every module function whose type is structurally compatible with the slot,
- an external possibility,
- and, when the declared type is nullable, a null possibility.

Imports are never executed; they are assumed capable of returning normally.
A `ref.func` that names an imported function still reports that function's
real index and is not replaced by the external possibility. Function indices
always include the function-import prefix; other imports do not occupy
function indices.

## Supported syntax

Supported function-body nodes are: `const`, `nop`, `drop`, untyped and typed
`select`, `local.get/set/tee`, `ref.func/null/is_null/as_non_null`, scalar
unary/binary/compare/convert operations, `block`, `if`, `return`,
`unreachable`, `call`, `call_ref`, `return_call`, and `return_call_ref`.

Any other node — including `loop`, `br*`, `br_table`, table and memory
operations, globals, exception handling, SIMD, and GC heap operations — is
rejected with an error that names the function and the fixed pre-order
expression ordinal. The supported-range check walks the entire body, so an
unsupported node in unreachable code is still rejected.

Type sections may contain only function types. GC heap types and recursive
type groups are rejected. Structural type matching compares parameters,
results, and reference nullability rather than treating equal type indices as
equal types.

## Deliberate limitations

- Tables, memories, element-segment contents, and globals are not analyzed.
  A reference that only exists through a table or global is therefore not
  tracked; such values are represented as bottom unless they merge with a
  known or external value.
- Numeric and vector values are opaque scalars; no constant propagation,
  arithmetic, or numeric condition evaluation is performed.
- Traps from arithmetic, memory access, or integer overflow are not modeled;
  only explicit `unreachable` and known null-only function-reference calls
  create a no-continuation point.
- `externref` and other non-function references occupy abstract slots but can
  never name a module function, so they never contribute function identities.
- The report describes possible concrete in-module targets and the external
  possibility; it does not reason about host behavior beyond assuming imports
  may return.

## Output

`ReferenceFlow` owns all of its vectors and maps and does not retain
references into the `Module`, the input bytes, or any string view. It remains
valid after the inputs are destroyed. Repeated or interleaved analyses use
independent state and never mutate the source module. The companion
`wasm-ref-flow` tool prints one deterministic JSON object per context followed
by a roots object, one line each.
