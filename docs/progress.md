# Progress Analysis

`mpc_analyze_progress(root)` performs a read-only static analysis of a
parser object graph and returns an `mpc_progress_report_t`.  It exists to
find combinators that can keep running without advancing the input
position — the kind of in-layer spin that the recursion depth guard does
not catch, because `many` and similar loops never recurse.

The analysis works directly on parser objects.  It does not parse grammar
text, run a parse, invoke semantic or destructor callbacks, or mutate
`retained`, `name` or any optimisation state.  Parsers can still be parsed,
redefined and deleted afterwards, and an existing report is unaffected by
redefining a shared named parser because the report owns copies of every
array, path and name it exposes.

## Effects

Every parser call is classified by four possible effects, measured
relative to the position at which that specific node call was entered:

| Flag | Meaning |
| --- | --- |
| `MPC_PROGRESS_FAIL_EMPTY` | failed without consuming input |
| `MPC_PROGRESS_FAIL_CONSUMED` | failed after consuming input |
| `MPC_PROGRESS_SUCC_EMPTY` | succeeded without consuming input |
| `MPC_PROGRESS_SUCC_CONSUMED` | succeeded after consuming input |

Only finite, normal returns are modelled.  Protective aborts such as
running out of memory or hitting `MPC_MAX_RECURSION_DEPTH` are not effects.
Character constraints are treated as both satisfiable and unsatisfiable;
callbacks are assumed to return normally without changing the parser or
input.  A `check`/`check_with` verdict is treated as both possible, so a
successful child result can either be accepted or be rejected while
keeping the child's consumption.

Effect sets start empty and grow to the least fixed point of a monotone
transfer function.  Nodes are not uniformly filled with all four effects:
a node only carries an effect when some finite control-flow path through
the real graph produces it.

### Node abstractions

* Character classes (`any`, `single`, `oneof`, `noneof`, `range`,
  `satisfy`) and non-empty `string`: zero-consumption failure or
  consuming success.  An empty `string` only has zero-consumption
  success (its internal mark/rewind never surfaces).
* `anchor`, `soi`, `eoi`: zero-consumption success or zero-consumption
  failure.
* `pass`, `state`, `lift` and `lift_val`: only zero-consumption success.
* `fail` and undefined nodes: only zero-consumption failure.
* `apply`, `apply_to`, `expect`: the child's effects unchanged.
* `check`, `check_with`: the child's failure effects, plus a rejection
  outcome at the child's position for each of the child's success effects.
* `not`: a failing child succeeds while retaining the position; a
  succeeding child is rewound and fails at the entry position.
* `maybe`: child successes pass through; child failures become successes
  while retaining the position reached.
* `and`: any failure rewinds the whole sequence to its entry, so the
  parser never returns a consumed failure.  Later items require a
  succeeding prefix.
* `or`: failed alternatives do not rewind for the next alternative.
  Later items therefore require a failing prefix, and their entry
  position reflects everything the earlier failing alternatives consumed.
* `many`, `many1` and `sepby1`: the failed attempt that terminates the
  repetition is a real call and its retained position contributes to the
  returned effects; it is not treated as never having happened.  A body
  that cannot fail has no finite return, so its effect set is empty and
  the stall surfaces as a repetition risk instead.
* `count` with a positive count composes exactly that many finite body
  calls.  `count` with `n <= 0` is unsupported.
* `predictive` is unsupported because it changes backtracking semantics
  in a way the position model does not capture.

## Edges, reachability and zero-consumption calls

Two questions are kept separate once the effect fixpoint is stable:

1. **Reachability** — which nodes control flow can call from the root at
   all.  An `and` item requires every earlier item to be able to succeed;
   an `or` item requires every earlier item to be able to fail; the
   `sepby1` separator requires the leading element to succeed.  A
   subgraph only reached after input was consumed is still reachable and
   is still checked for its own local stagnation.
2. **Zero-consumption edges** — whether a child call can happen while the
   position still equals the parent's entry position.  This depends only
   on the prefix: later `and` items need a zero-consumption *successful*
   prefix, later `or` items need zero-consumption *failing* prefixes, and
   the `sepby1` separator needs a zero-consumption successful element.
   "Can fail" is not conflated with "can fail without consuming".

## Risks

* **Cycle** (`MPC_PROGRESS_RISK_CYCLE`): a recursive cycle made entirely
  of zero-consumption call edges inside a reachable strongly connected
  component.  Each cyclic SCC contributes one finding whose
  `node_path` is a closed path (the first node index is repeated at the
  end).  Right recursion entered only after consumption is not a
  zero-consumption cycle.
* **Repetition** (`MPC_PROGRESS_RISK_REPETITION`): a reachable `many` or
  `many1` whose body can succeed empty, or a reachable `sepby1` whose
  complete separator-plus-element unit can succeed empty after the first
  element has succeeded.  An empty separator paired with a consuming
  element does not stall.

A finding means stagnation is structurally possible.  It does not promise
that some concrete input string deterministically triggers it.

## Unsupported nodes and completeness

`count` with `n <= 0` and every `predictive` node are recorded in the
report's `unsupported` array with a reason, regardless of whether they
are reachable — an unsupported node that control flow cannot reach is
still part of the object graph and is still listed.  Any such node sets
`complete` to zero: the report then must not be read as a proof of
absence of risk, although the per-node effects and findings are still
computed and reported on a best-effort basis.  The production parser is
not modified to work around these nodes.

## Identity, ordering and ownership

Node identity is the parser object pointer: shared and recursive parsers
appear once.  Indices are assigned by depth-first first visit from the
root, children left to right; for `sepby1` the structural child order is
element then separator.  Repeated analyses produce identical indices,
finding order and paths.

Use `mpc_progress_report_delete` to release a report.  It frees only
memory owned by the report and never calls user callbacks or touches the
analysed parsers.
