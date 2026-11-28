# PCRE2 replayable property testing (task_support)

A replayable, differential property test for the real PCRE2 library at all
three code-unit widths (8/16/32). Generated regexes and texts are built once
as Unicode scalar ASTs, encoded independently into UTF-8, UTF-16 and UTF-32,
executed through the genuine `pcre2_compile` / `pcre2_match` interfaces, and
checked against an independent reference interpreter that never links PCRE2.

## Build and run

```sh
cmake -S . -B .task-build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=ON -DPCRE2_BUILD_PCRE2_32=ON \
  -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_TESTS=OFF \
  -DBUILD_SHARED_LIBS=OFF -DPCRE2_SUPPORT_JIT=OFF
cmake --build .task-build -j 4
sh task_support/baseline.sh   # shows the raw library calls still work
sh task_support/check.sh      # builds and runs the property test
```

`check.sh` first runs the baseline, then compiles every module below with
`cc -std=c99` (no new packages, no network) and links the three static
libraries. The driver (`driver.c`) calls `task_cases("model")`,
`task_cases("metamorphic")` and `task_cases("replay")`; each group must
contain at least five live `TASK_CHECK` assertions.

## Module layout (independent responsibilities)

| File | Responsibility |
| --- | --- |
| `task_rng_ast.c` | Deterministic splitmix64 PRNG, Unicode-scalar AST generator, scalar-level pattern serializer, AST rendering, metamorphic transforms |
| `task_oracle.c` | Independent recursive backtracking interpreter over scalar positions (leftmost match, leftmost-first alternation, greedy bounded repetition, PCRE2 capture rules) |
| `task_utf.c` | UTF-8/16/32 encoders and boundary tables built from the emitted code units |
| `task_run8.c`, `task_run16.c`, `task_run32.c` | Real `pcre2_compile`/`pcre2_match` at one width each (separate TUs because `pcre2.h` binds one width per inclusion) |
| `task_engine.c` | Width dispatch, oracle/library comparison, first-difference retention, coverage counters, discrepancy simulator |
| `target.c` | The `task_cases` entry point and the `model` / `metamorphic` / `replay` assertion groups |

## Supported grammar (the only syntax generated)

- literal Unicode scalars;
- non-empty explicit character classes `[...]`;
- concatenation;
- two-branch alternation (leftmost-first);
- capturing groups (numbered 1..N left to right, optionally named `(?P<gN>...)`)
  and non-capturing groups used only by transforms;
- greedy bounded repetition `{min,max}` with `0 <= min <= max <= 4`.

Hard bounds: AST depth at most 5, at most 8 captures, text at most 128
scalars, classes at most 5 members. The scalar pools contain ASCII, non-ASCII
BMP and supplementary-plane scalars and never contain surrogates or other
invalid code points. ASCII metacharacters (e.g. `$`) are emitted as `\x{..}`
so the generated pattern cannot escape the defined grammar.

All patterns are compiled with `PCRE2_UTF` and run with the plain
interpreter (`pcre2_match`); JIT is disabled in the build and never called.
Nothing outside the grammar above is generated, so an unknown result can
never be silently counted as a pass.

## Independent oracle semantics

`task_oracle.c` works purely on scalar offsets and models:

- leftmost overall match, trying start positions 0..length;
- leftmost-first alternation with capture snapshots restored per branch;
- greedy bounded repetition that backtracks from max to min, including
  correct backtracking through nested quantifiers;
- capture rules: a repeated group keeps its last iteration's span; a group
  that never participates stays `UNSET` (`SIZE_MAX`), which is distinct from
  a group that participates and captures the empty string `[p,p)`.

## Boundary tables

Each width maps the library's code-unit offsets back to scalar offsets
through a boundary table constructed from the emitted units (lead bytes in
UTF-8, high surrogates in UTF-16, every unit in UTF-32). An offset landing
inside a multi-unit code point maps to `-1` and is recorded as a boundary
violation; the test fails if one is ever observed.

## Metamorphic transformations

Each transform carries an explicit mapping instead of assuming identical
ovectors:

- adding non-capturing groups around concatenation children, and removing
  them again (language and all captures unchanged);
- wrapping the whole pattern in a new capture: old group `i` maps to new
  group `i+1`, new group 1 equals the whole match, asserted both in the
  oracle and against the library;
- safe renaming of every capture to unique `(?P<gN>...)` names: numbers and
  spans stay identical.

## Seed replay

Cases derive from fixed seeds (`0x5eed000000000001 + index` for the main
corpus). A fixed seed regenerates the identical AST, text, encoding, oracle
answer and all three library answers. The `replay` group runs the full
500-case corpus twice into fingerprints and compares them byte-for-byte
(hashes, oracle ovectors, full `LibResult` records); running the binary
twice also produces byte-identical stdout.

The discrepancy simulator (`simulator_self_check`) is a predetermined,
in-memory logic difference on the test side only: it corrupts an oracle
answer and verifies that the comparison reports it, keeps the first failing
case even when later cases differ, distinguishes UNSET from empty
participation, and surfaces a planted boundary violation. No upstream source
is modified to manufacture a defect.

## Coverage numbers

The default run is 500 generated cases. The `COVERAGE` block reports real,
measured counts (example shape; exact values are printed each run):

- depth histogram (depth 2..5);
- capture-count histogram (every count 1..8 is exercised);
- supplementary-plane scalar count;
- empty captures (participated, zero-length) vs UNSET captures;
- branch, repetition and class node counts;
- oracle match / no-match counts (both paths are compared);
- compile errors, boundary violations and mismatches (all expected zero).

## Result limitations

- The test only covers the bounded, well-defined grammar listed above. It
  does not generate anchors, look-around, back-references, Unicode property
  classes, unbounded quantifiers, case-insensitive matching or any other
  PCRE2 feature; behavior of those features is out of scope.
- It is a differential/metamorphic property test over normal inputs. It is
  not a vulnerability finder and does not attempt to reproduce any known
  bug.
- The interpreter is used exclusively; JIT behavior is not assessed.
- Text length, depth, captures and repetition counts are deliberately
  capped; pathological backtracking performance is outside the scope.
- A green run means the three library widths and the independent oracle
  agree on every generated case and every transform; it does not prove the
  grammar's full semantics for arbitrary PCRE2 patterns.
