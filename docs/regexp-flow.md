# Flow-sensitive RegExp constructor checks

`security/detect-unsafe-regex` and `security/detect-non-literal-regexp` share a
flow-sensitive value analysis (implemented in `utils/regexp-flow.js`) that
computes, for every `RegExp(...)` and `new RegExp(...)` call, what the first
argument may evaluate to **at that call site**. The analyzed program is never
executed: all values are computed by a static dataflow analysis. Both rules
read the same per-call result, so enabling only one of them or both produces
consistent reports, and each rule reports at most one message per call site no
matter how many candidate patterns or loop revisits were involved.

## What is analyzed

The analysis runs independently on the `Program` and on every function
declaration, function expression, arrow function, and class static block. For
each of them it tracks the bindings declared in that unit (including nested
block, `for`, `switch`, `catch`, and `with` scopes), using the real lexical
bindings from the scope manager, so same-named variables in different blocks
stay distinct.

Per program point, each tracked binding maps to a value with four parts:

- `strings`: known candidate patterns, at most **8 distinct strings**, each at
  most **4096 UTF-16 code units**.
- `statik`: the expression may be a static value that cannot be evaluated
  locally (see "Static exceptions" below).
- `dynamic`: the expression may be a dynamic/unknown value.
- `overflowed`: the value exceeded a budget and collapsed to unknown.

### Supported expressions

- String literals.
- `+` concatenation of strings (candidate sets are combined pairwise).
- String template literals (quasi parts and interpolations are concatenated).
- Identifiers bound to a tracked local of the current function: the value at
  the current program point is used.

### Supported writes

- `let`/`const`/`var` declarations with an identifier and an initializer.
- `=` and `+=` assignments to an identifier in a standalone expression
  statement.

The right-hand side is evaluated against the state **before** the write, then
the left-hand side is updated. Copying a string to another variable copies the
current candidates; later writes to the source variable do not change the
copy. Uninitialized variables and parameters start unknown and become known
again after a definite overwrite assignment.

### What is not evaluated

Number conversions, object properties, and return values of other functions
are not inferred. Calls, member accesses, `await`/`yield` results, arithmetic
other than `+`, conditional expressions, and anything else evaluate to
"unknown", keeping their static flavor when they are recognized static
expressions.

## How states merge

Control-flow joins (`if`/`else`, `switch` fallthrough, loop back edges,
`break`/`continue` exits, `try`/`catch`/`finally` exits) merge the incoming
states per binding:

- Known candidate sets are unioned; known dangerous candidates are never
  washed away by an unknown branch.
- `dynamic` and `statik` are sticky: once a value may be dynamic or static on
  some path, the joined value keeps that.
- Conditions are never evaluated, so both sides of a branch are considered
  reachable, and correlations between different variables are not tracked.
- A branch that ends in `return` or `throw` contributes nothing to the join,
  so a dangerous assignment on such a branch does not pollute the calls after
  it.

## How loops stabilize

Loops are handled by a fixpoint computation over the control-flow graph: the
state at the loop head is the merge of the pre-loop state and the state at the
end of the body (the back edge, including `continue` paths), iterated until no
state changes anymore. A call inside the loop therefore sees writes from the
end of the previous iteration, not just the first traversal. A `while`/`for`
loop may run zero times, so the head state also flows to the loop exit; a
`do`-`while` body runs at least once, so only the post-body state flows out.

Termination does not depend on unrolling loops a fixed number of times. The
value lattice has finite height because of the budgets: a value may hold at
most 8 distinct strings of at most 4096 UTF-16 code units each. When a merge
or a concatenation exceeds either budget, the whole value collapses to
unknown (`overflowed`) without keeping an arbitrary subset of the patterns.
Overflow is sticky through later merges — partial candidates are never
recovered from a degraded value — and only a definite overwrite assignment
(`p = "..."` outside `try`/`catch`/`finally`/`with`) can make the binding
known again. Every transfer function is monotonic in this lattice, so the
worklist iteration reaches the least fixpoint and stops.

## Degradation boundaries

The analysis is deliberately conservative in these cases:

- **Outer scopes**: reading a variable of an outer function is unknown (the
  closure may run at any time). If the binding is written by any nested
  function, it is unknown everywhere in the owning function. Recognized
  static read-only bindings keep their static meaning (no new dynamic
  warnings).
- **`try`/`catch`/`finally` and `with`**: writes inside them may not complete,
  so they merge with the previous value instead of overwriting. `finally`
  blocks are applied on normal and on `break`/`continue`/`return`/`throw`
  paths. The `catch` parameter and the caught control flow are
  over-approximated.
- **Destructuring writes** (`let {a} = ...`, `[p] = ...`): every bound name
  becomes unknown.
- **Direct `eval`**: if a function's subtree contains a direct `eval(...)`
  call, every local of that function is unknown everywhere.
- **Async pauses**: `await`/`yield` evaluate to unknown values. Local bindings
  keep their values across suspension points (no other code can mutate them);
  bindings captured and mutated by callbacks are already unknown via the
  closure rule above.
- **Unsupported assignment forms** (compound operators other than `+=`,
  `++`/`--`, assignments nested inside other expressions): the target becomes
  unknown.

In all of these cases the affected bindings are treated as unknown rather
than keeping stale constants.

## Static exceptions

Expressions that are static but cannot be evaluated locally keep their
previous special meaning through `isStaticExpression`: `path.join()` and the
other recognized `path` constructors, `url.fileURLToPath()`,
`require.resolve()`, `import.meta.url`/`dirname`/`filename`, `process.cwd()`,
`__dirname`/`__filename`, and read-only bindings initialized from them. They
are never executed and never produce a non-literal warning, and because their
string value is unknown they also cannot produce an unsafe-pattern report.
Other consumers of `isStaticExpression` are unaffected.

## Reporting

- `detect-unsafe-regex` runs `safe-regex` on every known candidate pattern of
  the call-site value and reports `Unsafe Regular Expression (new RegExp)` if
  any candidate is unsafe. Unknown alone never produces an unsafe report.
  Regex literals and literal constructor arguments keep their existing
  messages and code paths.
- `detect-non-literal-regexp` reports `Found non-literal argument to RegExp Constructor`
  when the call-site value may be dynamic. Fully known values and recognized
  static exceptions are not reported.
- When a parameter or local binding shadows `RegExp`, neither rule treats the
  call as the built-in constructor.

The analysis result is cached per parsed file (keyed by the AST), so reusing a
`Linter` across files or swapping rule configurations cannot leak state
between runs.
