# Ownership-model fuzzing: input format and compatibility

This documents the wire format consumed by `qjs_ownership_run`
(`fuzz/ownership_replay.h`) and `LLVMFuzzerTestOneInput`
(`fuzz/ownership_fuzz.c`). The normative semantics live in
`task_support/contract.md`; this file records the concrete encoding and the
compatibility boundaries that callers rely on.

## Scope

The harness exercises host-reference **ownership responsibilities** through
legal public C-API call sequences only. It does not read bytecode, does not
evaluate foreign scripts, and never constructs an invalid object. It looks
for early finalization, repeated finalization and missed cleanup.

## Wire format

- A plan has **no header**.
- It is a sequence of 0 to 1024 records; every record is exactly three
  bytes `(op, a, b)`, so the total input size is a multiple of three and at
  most `1024 * 3 = 3072` bytes.
- A zero-length input is the valid empty plan and reports all-zero counters.
- Unused operands must be zero.
- Any input whose length is not a multiple of three, exceeds 3072 bytes,
  contains an unknown opcode or operand, or violates a logical precondition
  is rejected **as a whole before the engine is called**
  (`qjs_ownership_run` returns 1, the fuzzer entry swallows it).

### Records

| op | name         | a        | b        | effect |
|----|--------------|----------|----------|--------|
| 0  | NEW          | empty root | 0      | create a tracked finalizable object in root `a` |
| 1  | DUP          | live root | empty root | duplicate root `a` into root `b` (an alias) |
| 2  | DROP         | root     | 0        | release root `a` and clear it |
| 3  | THROW_TAKE   | root     | 0        | `JS_Throw` consumes root `a`, replacing old pending |
| 4  | CATCH        | empty root | 0      | `JS_GetException` transfers pending into root `a` |
| 5  | GC           | 0        | 0        | `JS_RunGC`, then compare the identity finalize set |
| 6  | STORE_TAKE   | tracked root | 0   | transfer root `a` into the fixed holder's `held` property |
| 7  | CLEAR        | 0        | 0        | set holder `held` to undefined, releasing its responsibility |
| 8  | REJECT_TAKE  | tracked root | 0   | assign into a pre-made read-only property; the normal TypeError still consumes the argument |
| 9  | CALL_THROW   | tracked root | 0   | call a native callback that duplicates the borrowed argument before `JS_Throw` |

Root slots are 0..15. At most 64 tracked identities exist; identities are
allocated monotonically and never reused. Slot overwrites are forbidden, so
every write targets an empty slot and every release targets a populated one.

### Tracked identities versus untracked exceptions

A tracked identity is an object of the executor's custom finalizable class.
The TypeError produced by REJECT_TAKE is an **untracked** exception with no
logical identity. Once CATCH moves it into a root, that root may be DROP,
DUP or THROW_TAKE, but it cannot participate in STORE_TAKE, REJECT_TAKE or
CALL_THROW and can never become a new tracked identity.

### Responsibility categories

The model records three responsibilities separately, even when several point
at the same identity:

- `root` — one entry per occupied root slot,
- `holder` — the single fixed `held` property,
- `pending` — the current pending exception.

`JS_Throw` transfers a reference and replaces the old pending exception;
`JS_GetException` transfers it to a root. A successful assignment and a
normally rejected assignment both consume their value argument. A native
callback that throws a borrowed argument must duplicate it first.

## Observability and reporting

`struct qjs_ownership_report` returns `steps`, `created`, `finalized`,
`comparisons` and `exceptions`, zeroed then filled from real observations.
After every step the harness checks that no identity still carrying a
responsibility has been finalized. At each GC and during mandatory end-of-run
cleanup it compares the expected and observed finalization sets **identity by
identity**; equal cardinalities are never accepted as a substitute. The first
divergence is reported with the action index, object identity, responsibility
category and expected/actual values (for example
`ownership mismatch at step 6 op=GC id=0 category=gc_finalize_set expected=0 actual=1`);
the heap is never dumped. Return codes are `0` success, `1` invalid whole
plan, `2` model/engine divergence. The replay code never terminates the
process.

Each plan is replayed twice, each time in a new runtime/context; the two
stepwise responsibility/finalization traces must match (finalization order is
not constrained). Every run explicitly releases all roots, the holder
responsibility and any unretrieved pending exception before tearing the
runtime down.

## Compatibility boundaries

- Public API only: the executor uses the documented QuickJS C API and never
  reads internal reference counts or engine structs. The model translation
  units (`ownership_model.*`) do not include `quickjs.h` and hold no
  `JSValue`.
- Engine, the original `fuzz.c` and existing public-API behavior are
  unchanged; no engine source, public header or the fixed runner
  (`task_support/`) is modified.
- Fixed limits: 16 root slots, 64 tracked identities, 1024 records, 3072
  input bytes. These are part of the format; inputs beyond them are rejected,
  not truncated.
- Determinism: the executor allocates no per-plan global state outside the
  fresh runtime, so identical bytes yield identical reports across runs.
- Tooling: standard C11 plus the system C compiler and libraries; the build
  is driven solely by `bash task_support/run.sh target`.
