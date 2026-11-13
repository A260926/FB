# PCRE2 replayable property fuzzing

This directory adds a **replayable, metamorphic property test** for the real
PCRE2 library. Normal generated inputs are run through an *independent*
reference model and through the genuine PCRE2 interpretive matcher in all
three code-unit widths (8/16/32); results are compared in Unicode-scalar
space. The test chain is built on the real library API
(`pcre2_compile_*` / `pcre2_match_*`), not on a self-contained demo.

## Build and run

```sh
cmake -S . -B .task-build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=ON -DPCRE2_BUILD_PCRE2_32=ON \
  -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_TESTS=OFF \
  -DBUILD_SHARED_LIBS=OFF -DPCRE2_SUPPORT_JIT=OFF
cmake --build .task-build -j 4
sh task_support/baseline.sh   # original-library smoke call
sh task_support/check.sh      # the property suite (this work)
```

`check.sh` first runs the baseline, then compiles every module below as C99
and links the three real static libraries.

## Modules (generation, encoding, oracle, execution are independent)

| File | Responsibility |
|------|----------------|
| `task_rng.h` | Deterministic splitmix64 PRNG; one 64-bit seed drives a whole run. |
| `task_ast.[ch]` | Logical Unicode-scalar AST, scalar pools, arena and bounds. |
| `task_gen.[ch]` | Restricted random AST + scalar subject generator. |
| `task_enc.[ch]` | UTF-8/16/32 encoding and per-width code-unit boundary tables. |
| `task_render.[ch]` | AST -> PCRE pattern source and stable AST/scalar serialization. |
| `task_oracle.[ch]` | Independent backtracking interpreter on scalars (never calls PCRE2). |
| `task_engine.[ch]` | Real PCRE2 compile/match for all widths; offset mapping and name lookup. |
| `task_meta.[ch]` | Metamorphic AST transforms with explicit capture-number maps. |
| `task_harness.[ch]` | Suite driver, comparator, coverage counters, replay/fault probes. |
| `target.c` | `int task_cases(const char* group)` with the `model`/`metamorphic`/`replay` assertions. |
| `driver.c`, `check.h` | Supplied test entry point and assertion macro. |

## Grammar scope (the only syntax generated)

- single literal scalar;
- explicit, **non-empty** character class, optionally negated;
- concatenation (2-3 children);
- alternation (2-3 branches, left branch tried first);
- non-capturing group `(?: ... )`;
- capturing group, numbered in preorder (PCRE order), at most 8, optionally
  named;
- greedy bounded repetition with bounds in `0..4` (e.g. `{2}`, `{0,3}`).

Pattern depth is at most 5 and a subject holds at most 128 scalars. The
generator only emits constructs it fully supports; results outside this
grammar are never counted as passes. All patterns are compiled with
`PCRE2_UTF` and matched with the plain interpreter (`pcre2_match`, no JIT).

The scalar pool deliberately covers all three ranges and contains **no
surrogate code points** (D800-DFFF):

- ASCII: `a b c A 0`;
- non-ASCII BMP: U+00E9, U+03A9, U+4E2D, U+2603;
- supplementary plane: U+1D11E, U+1F600, U+20000.

Non-ASCII pattern scalars are emitted as `\x{...}` so the pattern source is
pure ASCII and valid in every width (a supplementary scalar cannot be a raw
UTF-16 source unit).

## Reference model semantics

`task_oracle` is a backtracking matcher over scalar arrays using a persistent
(immutable, tail-linked) continuation, so two rules that are easy to get wrong
match PCRE exactly:

- **leftmost overall match**, leftmost alternation branch first, with branch
  fall-through when a branch succeeds locally but makes the surrounding
  continuation fail (e.g. `(a|ab)c` on `abc` takes `ab`);
- **greedy bounded repetition** with PCRE's LIFO backtracking, including
  nested quantifiers (`(c{2,3}){1,3}` on seven `c`s matches six, not seven).

Capture state is snapshotted around every speculative attempt, reproducing the
overwrite/rollback rules. A capture that never participates stays **unset**; a
capture that participates but matches the empty string is recorded as a set
span `[p,p)`. These two cases are compared separately and are never conflated.

The interpreter has its own step/frame/snapshot limits; hitting one returns
`TASK_ORACLE_LIMIT`, which the harness reports as an error rather than a pass.

## Offset boundary tables

Each encoded subject carries a boundary table for its own width. A code unit
that starts a scalar maps to that scalar's index; every interior unit (UTF-8
continuation byte, or the low surrogate of a UTF-16 supplementary scalar) maps
to `-1`. Every PCRE2 ovector offset is mapped through the matching table. An
offset that lands inside a code point becomes an explicit
`TASK_ENG_BAD_BOUNDARY` failure instead of being silently reinterpreted.

## Metamorphic transforms and capture maps

Each transform builds a new tree in a separate arena and returns an explicit
capture-number mapping; comparisons go through that mapping rather than
assuming identical ovector layout:

- **add non-capturing groups** around concatenands (identity numbering);
- **remove** redundant non-capturing groups (identity numbering);
- **wrap** the whole pattern in a new outer capture (old `i` -> new `i+1`;
  the new capture 1 must equal the whole-match span);
- **safe rename** of named captures (numbering unchanged); the new name is
  verified to resolve to the same number and the old name to be absent, via
  `pcre2_substring_number_from_name_*` in every width.

The whole-match language is preserved, so the leftmost match span must be
unchanged on the same subject; mapped captures must keep identical
participation and spans.

## Seed replay and output

A fixed 64-bit seed reproduces every case byte-for-byte: the same AST, scalar
subject, encoded forms and results. A FNV-1a fingerprint over every oracle and
engine result is printed; identical seeds yield identical fingerprints, and a
different seed yields a different one.

The run prints the seed, the logical AST, the scalar subject, the oracle
result and each width's result for representative cases, plus the **first
difference** block (`first difference: none` when all agree). Defaults are
500 generated model cases and 250 metamorphic source cases.

Coverage counters are real, measured per run:

- depth histogram (depth 1..5) and capture-count histogram (0..8);
- cases containing supplementary scalars, alternation and repetition;
- empty-but-participating captures and never-participating (unset) captures;
- match vs no-match counts (both must be non-zero so inputs are not filtered
  to an empty universe); compile errors are surfaced, never swallowed.

## Predetermined logical-difference simulator

Without modifying any upstream PCRE2 code, the suite proves its own checker:

- `task_simulate_logical_difference()` hand-builds a fixed case, confirms the
  honest oracle and engine agree, then corrupts *copies* of the oracle result
  (capture-end shift, match verdict flip, participation flip) and requires the
  comparator to reject each one;
- the `replay` group injects a one-shot fault into the harness's in-memory
  oracle copy at a known case index and asserts the run fails, the difference
  is recorded, and the **first** failing case is retained. A subsequent clean
  replay passes again.

## Result limitations

- Coverage is restricted to the bounded, well-defined grammar above; this is a
  property test of normal inputs, not an attempt to find or reproduce any
  known PCRE2 vulnerability.
- Only `PCRE2_UTF` interpretive matching is exercised; JIT is disabled and not
  tested. Anchors, lookaround, backreferences, Unicode properties, recursion,
  unbounded quantifiers and other syntax are intentionally out of scope.
- Subjects are at most 128 scalars and patterns stay within fixed arena
  limits; the reference interpreter enforces its own resource caps.
- Agreement is reported in scalar space after per-width boundary mapping; the
  raw code-unit offsets themselves are only validated to lie on boundaries.
