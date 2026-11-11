# DOM XSS flow regressions

`tests/regress/domxss-flow.test.cjs` (node:test) and
`tools/check-domxss-mutants.cjs` pin down the data-flow behavior of
`no-unsanitized/property` and `no-unsanitized/method` across three layers:
binding writes, expression graphs, and sink configuration. All samples are
inert strings that ESLint reads statically; no DOM operation is executed.

## Generation scope

A small deterministic generator builds straight-line programs from
declaration records and statement lists. It covers only syntax the rules
already handle:

- `const`/`let` declarations, with or without initializer;
- assignment statements (writing references) in any position relative to
  the sink, including writes that textually follow the sink;
- string concatenation (`a + b`) and template interpolation (`` `${a}` ``);
- the recognized sanitizers: `` escapeHTML`...` `` tagged templates and
  `unwrapSafeHTML(...)` calls;
- raw external input, modeled as the undeclared global `input`;
- the sinks `innerHTML`, `outerHTML` (property rule) and
  `insertAdjacentHTML` (method rule, HTML argument at index 1).

A few cases cannot be expressed as straight-line generated programs and
stay handwritten: `var` declarations, function parameters, unresolvable
bindings, reference cycles (`let a = b; let b = a;`), self references
(`let s = s;`) and writes that read their own binding (`w = w + "?"`),
which are cycles through the write.

## Independent expectations

Expected reports are derived from the generator's own declaration records,
never by calling `RuleHelper` or copying detector output. The model
re-derives the documented conservative policy:

- literals and template text are safe; the recognized sanitizers are safe;
- raw input is unsafe;
- concatenation and interpolation are safe iff every value branch is safe;
- a binding reference is safe iff it resolves to a `const`/`let` whose
  initializer and every writing reference are safe — a tainted write
  poisons the binding even when it sits after the sink;
- `var`, parameters, unresolvable bindings and cycles (including a write
  that reads its own binding) are unsafe; repeated reads of one safe
  binding are not a cycle.

Each generated use maps to one expected report with rule id, line and
column, so a regression is attributable to the exact layer that lost the
safety information.

## Mutants and minimal counterexamples

`tools/check-domxss-mutants.cjs` runs the test file against the original
sources (must pass), then against three independently mutated copies. Each
copy must still parse and load, and detection only counts when the same
number of tests as the baseline run executed and failed. Minimal
counterexamples:

1. **ignore-later-writes** — writing references are no longer inspected
   (`if (ref.isWrite())` → `if (false)` in `lib/ruleHelper.js`).

   ```js
   let x = "ok";
   el.innerHTML = x;
   x = input;
   ```

   Correct: 1 report (the late write poisons `x`). Mutant: the write is
   never evaluated, so a tainted reassignment goes unnoticed.

2. **either-side-safe-concat** — concatenation passes when only one side
   is safe (`&&` → `||` in the `BinaryExpression` branch of
   `lib/ruleHelper.js`).

   ```js
   el.innerHTML = "ok" + input;
   ```

   Correct: 1 report (every branch must be safe). Mutant: the safe left
   operand alone allows the whole expression.

3. **position-arg-as-html** — `insertAdjacentHTML` checks argument 0
   instead of argument 1 (`properties: [1]` → `[0]` in
   `lib/rules/method.js`).

   ```js
   el.insertAdjacentHTML("beforeend", input);
   ```

   Correct: 1 report for argument 1. Mutant: the safe `"beforeend"`
   literal is checked instead and the tainted HTML argument escapes.

The script prints each mutant's name, the failing test names and the
assertion diagnostics, exits non-zero if any mutant escapes, and verifies
the repository sources are byte-identical afterwards.
