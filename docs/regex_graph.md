# Compiled-regex control-flow and call graph

This document describes the read-only graph interface added in
`src/reggraph.h` / `src/reggraph.c` for Oniguruma 6.9.5, plus the two
example programs.

The graph is a **static** view of the `Operation` array that
`onig_new()` produced. It answers "which instructions can follow which,
structurally?" It does **not** decide whether any text matches, does not
simulate the matching VM, does not solve repetition counts, capture
state or backtracking feasibility, and does not enumerate runtime call
stacks.

- `src/reggraph.h` - public C API, no internal headers required
- `src/reggraph.c` - implementation, compiled into `libonig`
- `examples/regex_graph.c` - JSON / DOT dumper
- `examples/regex_graph_audit.c` - structural risk candidate report
- `test/regex_graph_contract.c` - four-group contract test
- `test/regex_graph_determinism.c` - cross-process / cross-mode identity
- `test/regex_graph_audit_contract.c` - audit regression
- `test/regex_graph_check.sh` - builds both dispatch modes and runs all
  of the above

## Building and running

Everything uses only in-repo sources, a C compiler and CMake:

```sh
bash test/regex_graph_check.sh
```

The script builds the production library twice - once with the default
direct-threaded opcode table, once with
`-DONIG_DISABLE_DIRECT_THREADING` - then compiles and runs the contract
test, the audit regression, the determinism driver and both examples in
each mode. The determinism stage runs two independent processes per mode
and compares the canonical graph signature, the JSON bytes and the DOT
bytes across processes and across the two dispatch builds; it also
rejects pointer-looking tokens. Build and run logs are kept under
`$TMPDIR/regex_graph_check_log` (override with `REGEX_GRAPH_LOG_DIR`). A
compile failure, a failed assertion, a missing group, a non-`RESULT PASS`
line or a direct/switch difference makes the script exit non-zero.

## Node identity

Every node is one entry of the compiled `regex->ops` array. `index` is
that array index. All compiled instructions are kept, including ones the
graph marks structurally unreachable. The node identity is never derived
from an address or from allocation order.

The opcode is decoded from **the active build mode**:

- direct-threaded (`USE_DIRECT_THREADED_CODE`): `regex->ocs[i]`
- otherwise: `regex->ops[i].opcode`

`opaddr` is never treated as an index. The internal enum value is mapped
through a `switch` on the enum identifiers to the fixed public numbering,
never cast, so a build that disables a guarded opcode still produces the
same public numbers.

## Data ownership

`onig_graph_new()` copies everything it exposes:

- a node table with the opcode, public opcode id, flags and out-degree
- an edge table of `(from, to, type, deferred, conditional)` records
- CALL records, repeat records, the entry set, call-graph edges and SCCs
- for literal instructions, a private copy of the exact bytes the
  instruction compares (`literal`, `literal_len`), decoded away from the
  string pool and the inline 16-byte buffer

No pointer into the `regex_t`, its `Operation` array, its string pool or
its `repeat_range` is retained. A graph remains fully readable after
`onig_free()` and after arbitrary allocator churn. `onig_graph_free()`
releases every array.

Construction is all-or-nothing: on any error the builder frees the
partial state and returns `NULL`, filling `OnigGraphError` with a reason,
an instruction index, an opcode (or `-1`) and a message. No half graph is
ever returned.

## Supported opcodes

Fixed numbering for Oniguruma 6.9.5. "Guard" notes a compile-time macro;
an opcode under a disabled guard is simply never emitted, but its public
number does not change. "Edges" is what the builder emits for that
instruction.

| # | Public opcode | Guard | Status | Edges |
|---|---------------|-------|--------|-------|
| 0 | `FINISH` | - | supported (exit) | none |
| 1 | `END` | - | supported (exit) | none |
| 2 | `STR_1` | - | supported | fallthrough |
| 3 | `STR_2` | - | supported | fallthrough |
| 4 | `STR_3` | - | supported | fallthrough |
| 5 | `STR_4` | - | supported | fallthrough |
| 6 | `STR_5` | - | supported | fallthrough |
| 7 | `STR_N` | - | supported | fallthrough |
| 8 | `STR_MB2N1` | - | supported | fallthrough |
| 9 | `STR_MB2N2` | - | supported | fallthrough |
| 10 | `STR_MB2N3` | - | supported | fallthrough |
| 11 | `STR_MB2N` | - | supported | fallthrough |
| 12 | `STR_MB3N` | - | supported | fallthrough |
| 13 | `STR_MBN` | - | supported | fallthrough |
| 14 | `CCLASS` | - | supported | fallthrough |
| 15 | `CCLASS_MB` | - | supported | fallthrough |
| 16 | `CCLASS_MIX` | - | supported | fallthrough |
| 17 | `CCLASS_NOT` | - | supported | fallthrough |
| 18 | `CCLASS_MB_NOT` | - | supported | fallthrough |
| 19 | `CCLASS_MIX_NOT` | - | supported | fallthrough |
| 20 | `ANYCHAR` | - | supported | fallthrough |
| 21 | `ANYCHAR_ML` | - | supported | fallthrough |
| 22 | `ANYCHAR_STAR` | - | supported | fallthrough + deferred alt-push |
| 23 | `ANYCHAR_ML_STAR` | - | supported | fallthrough + deferred alt-push |
| 24 | `ANYCHAR_STAR_PEEK_NEXT` | - | supported | fallthrough + conditional deferred push |
| 25 | `ANYCHAR_ML_STAR_PEEK_NEXT` | - | supported | fallthrough + conditional deferred push |
| 26 | `WORD` | - | supported | fallthrough |
| 27 | `WORD_ASCII` | - | supported | fallthrough |
| 28 | `NO_WORD` | - | supported | fallthrough |
| 29 | `NO_WORD_ASCII` | - | supported | fallthrough |
| 30 | `WORD_BOUNDARY` | - | supported | fallthrough |
| 31 | `NO_WORD_BOUNDARY` | - | supported | fallthrough |
| 32 | `WORD_BEGIN` | - | supported | fallthrough |
| 33 | `WORD_END` | - | supported | fallthrough |
| 34 | `TEXT_SEGMENT_BOUNDARY` | - | supported | fallthrough |
| 35 | `BEGIN_BUF` | - | supported | fallthrough |
| 36 | `END_BUF` | - | supported | fallthrough |
| 37 | `BEGIN_LINE` | - | supported | fallthrough |
| 38 | `END_LINE` | - | supported | fallthrough |
| 39 | `SEMI_END_BUF` | - | supported | fallthrough |
| 40 | `CHECK_POSITION` | - | supported | fallthrough |
| 41 | `BACKREF1` | - | **rejected** | - |
| 42 | `BACKREF2` | - | **rejected** | - |
| 43 | `BACKREF_N` | - | **rejected** | - |
| 44 | `BACKREF_N_IC` | - | **rejected** | - |
| 45 | `BACKREF_MULTI` | - | **rejected** | - |
| 46 | `BACKREF_MULTI_IC` | - | **rejected** | - |
| 47 | `BACKREF_WITH_LEVEL` | - | **rejected** | - |
| 48 | `BACKREF_WITH_LEVEL_IC` | - | **rejected** | - |
| 49 | `BACKREF_CHECK` | - | **rejected** | - |
| 50 | `BACKREF_CHECK_WITH_LEVEL` | - | **rejected** | - |
| 51 | `MEM_START` | - | supported | fallthrough |
| 52 | `MEM_START_PUSH` | - | supported | fallthrough |
| 53 | `MEM_END_PUSH` | - | supported | fallthrough |
| 54 | `MEM_END_PUSH_REC` | `USE_CALL` | supported | fallthrough |
| 55 | `MEM_END` | - | supported | fallthrough |
| 56 | `MEM_END_REC` | `USE_CALL` | supported | fallthrough |
| 57 | `FAIL` | - | supported (dynamic exit) | none |
| 58 | `JUMP` | - | supported | jump |
| 59 | `PUSH` | - | supported | fallthrough + deferred alt-push |
| 60 | `PUSH_SUPER` | - | supported | fallthrough + deferred alt-push |
| 61 | `POP` | - | supported | fallthrough |
| 62 | `POP_TO_MARK` | - | supported | fallthrough |
| 63 | `PUSH_OR_JUMP_EXACT1` | `USE_OP_PUSH_OR_JUMP_EXACT` | supported | cond-continue + cond-jump + deferred cond-push |
| 64 | `PUSH_IF_PEEK_NEXT` | - | supported | fallthrough + deferred cond-push |
| 65 | `REPEAT` | - | supported | repeat-enter, optional repeat-skip |
| 66 | `REPEAT_NG` | - | supported | repeat-ng-enter or ng-skip + ng-exit |
| 67 | `REPEAT_INC` | - | supported | repeat-back, repeat-exit |
| 68 | `REPEAT_INC_NG` | - | supported | repeat-ng-exit, repeat-ng-back |
| 69 | `EMPTY_CHECK_START` | - | supported | fallthrough |
| 70 | `EMPTY_CHECK_END` | - | supported | fallthrough + empty-check-skip |
| 71 | `EMPTY_CHECK_END_MEMST` | - | supported | fallthrough + empty-check-skip |
| 72 | `EMPTY_CHECK_END_MEMST_PUSH` | `USE_CALL` | supported | fallthrough + empty-check-skip |
| 73 | `MOVE` | - | supported (bookkeeping) | fallthrough |
| 74 | `STEP_BACK_START` | - | supported (bookkeeping) | conditional fallthrough + cond-push + cond-jump |
| 75 | `STEP_BACK_NEXT` | - | supported (bookkeeping) | fallthrough + conditional deferred alt-push |
| 76 | `CUT_TO_MARK` | - | supported (bookkeeping) | fallthrough |
| 77 | `MARK` | - | supported (bookkeeping) | fallthrough |
| 78 | `SAVE_VAL` | - | supported (bookkeeping) | fallthrough |
| 79 | `UPDATE_VAR` | - | supported (bookkeeping) | fallthrough |
| 80 | `CALL` | `USE_CALL` | supported | call + resume-summary |
| 81 | `RETURN` | `USE_CALL` | supported (dynamic exit) | none |
| 82 | `CALLOUT_CONTENTS` | `USE_CALLOUT` | **rejected** | - |
| 83 | `CALLOUT_NAME` | `USE_CALLOUT` | **rejected** | - |

Anything else, including a value outside this table, is rejected with
`ONIG_GRAPH_ERR_UNKNOWN_OPCODE`. The builder never draws a default
sequential edge for an opcode it has not implemented.

"Bookkeeping" opcodes are the instructions that captures, atomic groups,
`\K` and lookaround compile into. They are modelled as ordinary
single-successor instructions; the graph does not evaluate the runtime
condition they consult. Lookaround therefore builds successfully and its
inner instructions appear as ordinary nodes. Backreferences and callouts
are rejected on purpose.

## Edge types

Every edge is `(from, to, type, deferred, conditional)`.

| Type | Name | Meaning |
|------|------|---------|
| 0 | `fallthrough` | Ordinary sequential success successor (`from + 1`). |
| 1 | `jump` | Unconditional `OP_JUMP` relocation. |
| 2 | `alt-push` | Address pushed on the VM backtrack stack by `PUSH`/`PUSH_SUPER` (and the `ANYCHAR_*_STAR` loops). Control continues at `from + 1`; the target is deferred. |
| 3 | `cond-continue` | `PUSH_OR_JUMP_EXACT1` / `PUSH_IF_PEEK_NEXT`: immediate `from + 1` when the peeked condition holds. |
| 4 | `cond-jump` | Immediate relocation when the peeked condition does not hold. A VM execution target, not a stack entry. |
| 5 | `cond-push` | Address pushed on the backtrack stack when the peeked condition holds. Deferred. |
| 6 | `repeat-enter` | `OP_REPEAT`: immediate entry into the body. |
| 7 | `repeat-skip` | `OP_REPEAT` with `lower == 0`: the zero-iteration path, a deferred alternative past `REPEAT_INC`. |
| 8 | `repeat-back` | `OP_REPEAT_INC`: another iteration (immediate body entry, conditional on the count). |
| 9 | `repeat-exit` | `OP_REPEAT_INC`: loop exit. Immediate when `n >= upper`; also emitted deferred when an exit alternative is pushed (`lower <= n < upper`). Never deferred for an unbounded upper. |
| 10 | `repeat-ng-enter` | `OP_REPEAT_NG` with `lower > 0`: immediate body entry. |
| 11 | `repeat-ng-skip` | `OP_REPEAT_NG` with `lower == 0`: deferred body alternative. |
| 12 | `repeat-ng-back` | `OP_REPEAT_INC_NG`: body back edge. |
| 13 | `repeat-ng-exit` | `OP_REPEAT_INC_NG`: loop exit. |
| 14 | `call` | `OP_CALL`: absolute call edge to the callee. No fallthrough. |
| 15 | `resume-summary` | Summary continuation after a call (`from + 1`). **Not** a VM execution edge; see below. |
| 16 | `empty-check-skip` | `EMPTY_CHECK_END*`: when the body matched empty the VM skips the following instruction, so this points at `from + 2`. |

`deferred = 1` marks edges that are reached through the VM backtrack
stack; `deferred = 0` marks edges the VM can follow immediately.
`conditional = 1` marks edges whose traversal depends on a runtime
condition that this graph does not solve (a peek, an empty-check result,
or an iteration count).

Deliberate modelling choices:

- Consumer instructions draw only their **success** successor. A normal
  match failure is not wired to every possible alternative. `FAIL`,
  `END` and `FINISH` are exits: no outgoing edges.
- `FAIL` pops the backtrack stack, so it is a dynamic exit. Its node
  carries `ONIG_GRAPH_NODE_FAIL_EXIT` (alias
  `ONIG_GRAPH_NODE_DYNAMIC_BACKTRACK_EXIT`). `RETURN` pops the dynamic
  call stack and carries `ONIG_GRAPH_NODE_DYNAMIC_RETURN`. Neither has
  an outgoing edge, and neither enumerates a runtime stack.
- `CALL` has a `call` edge and **no** fallthrough. The instruction after
  a call is recorded in the call record as `resume_index`, and may be
  reached through a `resume-summary` edge. That summary edge is a
  structural relationship, not what the VM executes when it returns.

### Relative and absolute addresses

- Relative addresses (`JUMP`, `PUSH`, `PUSH_SUPER`,
  `PUSH_OR_JUMP_EXACT1`, `PUSH_IF_PEEK_NEXT`, `REPEAT`,
  `STEP_BACK_START`) are resolved exactly as `regexec.c` does:
  target = instruction index + stored relative address.
- `CALL` uses the absolute operation index stored in `call.addr`.
- `REPEAT_INC` does not store a body address at all; the body comes from
  `repeat_range[id].u.pcode`.

## Repeat records

`OnigGraphRepeat` is decoded from `regex->repeat_range`:

- `entry_index` - the `OP_REPEAT` / `OP_REPEAT_NG` index
- `inc_index` - the matching `OP_REPEAT_INC` / `_NG` index
- `body_index` - `repeat_range[id].u.pcode` converted to a legal
  operation index (byte offset divided by `sizeof(Operation)`)
- `lower`, `upper` - from `repeat_range[id]`; an unbounded upper is
  reported as `ONIG_GRAPH_INFINITE_REPEAT` (`0x7fffffff`)
- `greedy` - 1 for `OP_REPEAT`, 0 for `OP_REPEAT_NG`

The union's pointer bytes are never written out. The body pointer is
validated: a pointer outside the `Operation` array, or one that does not
land on an `Operation` boundary, is
`ONIG_GRAPH_ERR_INVALID_REPEAT`; it is never truncated to a nearby
index. A missing entry/increment pair, a mismatched entry/increment
kind, or an exit target that does not equal `inc_index + 1` is
`ONIG_GRAPH_ERR_INCONSISTENT_REPEAT`.

Some repetition constructs are fully expanded by the compiler into
`PUSH`/`JUMP` chains (for example `a{0,3}` or `(?:ab){2,5}`), so they
have no `repeat_range` record. They appear as their literal instruction
sequence, which the graph still models correctly with `alt-push` and
`jump` edges.

## Call graph, resume summaries, SCCs

The **entry set** is main entry `0` plus every `CALL` target. Entries are
listed in ascending order. Different entries can share instructions.

For each entry the builder walks the graph from that entry, following
every edge type **except** `call` (the callee is a separate node in the
call graph, not a continuation) but **including** `resume-summary`
edges. Every `CALL` instruction met during that walk contributes a
call-graph edge from the entry to that call's target. Multiple call sites
to the same target collapse into one `OnigGraphCallEdge`, with
`first_call_index` and `call_count` describing the sites. Adjacent
address ranges are never treated as a function boundary.

Strongly connected components are computed on this call graph. A
component is marked `recursive` when it has size greater than one or has
a self loop. Its stable `id` is the smallest entry index in it. Components
and members are reported in ascending order.

**Structural reachability** is also reported per node
(`ONIG_GRAPH_NODE_REACHABLE` / `ONIG_GRAPH_NODE_UNREACHABLE`). It is
computed from main entry `0` over the same edge set, so it follows call
targets and resume summaries. It is conservative: a node marked
reachable is not guaranteed to be reachable on any real execution, and an
unreachable node is one that no static edge path from `0` reaches at all.

## The graph does not prove safety

The graph is **conservative and structural**. It never claims that a
match is possible, that a repetition count is attainable, that a
backtrack stack is feasible, or that a pattern is safe. The audit
example is explicitly not a ReDoS proof: it does not run long inputs and
does not attempt to infer exponential complexity from timing.

## Complexity

Let `N` be the number of compiled operations, `E` the number of emitted
edges (bounded by a small constant times `N`), and `M` the number of call
instructions.

- Building the graph: `O(N log E)` dominated by the edge sort.
- Reachability: `O(N + E)`.
- Call-graph construction: each entry walk visits at most `N` nodes and
  `E` edges, so `O(|entries| * (N + E))`, with `|entries| <= M + 1`.
- SCCs: `O(|entries| + |call edges|)`.
- Memory: `O(N + E + M + repeats)`.

The worst case is therefore quadratic in the number of call sites, which
is the price of not assuming that address ranges are functions.

## Examples

```sh
# JSON of a pattern read from a file (length is the file's byte length)
./examples/regex_graph --pattern-file pattern.txt --format json

# DOT
./examples/regex_graph --pattern-file pattern.txt --format dot

# Structural risk candidates (never a safety verdict)
./examples/regex_graph_audit --pattern-file pattern.txt
```

`regex_graph` reports compile failures and graph failures on stderr and
exits non-zero. `regex_graph_audit` emits `coverage-insufficient` (exit
0) when the pattern uses a rejected opcode, so an unsupported pattern can
never be read as "no risk".

The audit report's three rules are:

- `recursive-call-scc` - a recursive call-graph SCC, with its members and
  the call edges and call sites inside it
- `nested-repeat` - a repeat body containing another repeat entry
- `repeat-body-alt` - a deferred alternative edge inside a repeat body

Each candidate carries the pattern identifier, the rule name, the
involved `Operation` indices and edges, and is explicitly labelled a
structural risk candidate.

## Determinism

JSON arrays and DOT statements are emitted in fixed order: nodes by
index, edges by `(from, type, to)`, CALL records by `call_index`, repeat
records by `entry_index`, entries ascending, call-graph edges by
`(from_entry, to_entry)`, SCCs by stable id. Binary literal strings are
escaped by length as `\u00XX` plus a byte array; no identifier is derived
from a pointer or from allocation order. The default and direct-threaded
builds produce identical output for the same pattern.
