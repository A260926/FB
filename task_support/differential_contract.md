# Cooperative differential runs

Implement these public declarations in package rapid (types may be split between the two required production files):

```go
type DiffOperation interface { Poll() (done bool, value []byte); Cancel() }
type DiffBackend interface { Ready(command string) bool; Start(command string, input []byte) DiffOperation; Close() }
type DiffCommand struct { Name string; Input func(*T) []byte }
type DiffConfig struct {
    Backends []func() DiffBackend
    Commands []DiffCommand
    Rounds int
    MaxPolls int
    CancelAt int
    Normalize func(backend int, value []byte) []byte
}
type DiffRound struct { Command int; Input []byte; Polls []int; CancelAfter int }
type DiffEvent struct { Round int; Command int; Backend int; Kind string; Input []byte; Value []byte }
type DiffReport struct { Status string; Rounds int; Plan []DiffRound; Events []DiffEvent }
func (t *T) Differential(config DiffConfig) DiffReport
func ReplayDifferential(config DiffConfig, plan []DiffRound) DiffReport
```

Only these new APIs are required; existing APIs keep their behavior. Production must be in `differential.go` and `differential_schedule.go`, each nonempty and compiled without build constraints. Other Go files may be added. There is no implementation in the support material.

- A run owns at least two newly constructed backends in declared order; always close all constructed backends once in reverse order, including panic unwinding. Rounds >= 0, MaxPolls > 0, CancelAt >= 0, factories and command Input functions nonnil, command names nonempty/unique. Bad configuration returns Status `invalid` without constructing backends. Empty Commands is valid only for zero rounds. For zero rounds, construct and close backends and return `equal`, zero completed rounds, no events or plan entries. Backend methods and Normalize are synchronous, deterministic, may mutate their own state, and do not call Rapid. Panic propagates after cleanup, never as a mismatch.
- At each generated round, compute the intersection of Ready across backends, in config.Commands order. Draw one index in this intersection with IntRange and then call that command's Input exactly once on the same T. Record the original config command index and a deep copy of input. Start all implementations in declared order, each receiving its own copy; append `start` events with stable backend indices, input copies and nil Value. No common command gives `invalid` and no new round starts. Stop a trial at the first non-equal outcome.
- A Poll is a single cooperative step, never a goroutine or wall clock wait. Choose one currently unfinished backend with IntRange over the ordered unfinished indices; record the original backend index. Append `poll` (done=false, nil Value) or `done` (done=true, copied raw Value) events, with nil Input. Never poll a completed operation. Count polls per round. MaxPolls is a hard per-round cap. The pending operations are canceled once before returning `invalid` on budget exhaustion. No fairness beyond the recorded choices is promised.
- CancelAt > 0 means cancel just after that numbered Poll in a generated round. Record CancelAfter as CancelAt only if that cancellation boundary was actually reached in this round; otherwise record 0, including when all operations complete or the poll budget is exhausted before the configured boundary. CancelAt = 0 also records CancelAfter = 0. For replay, use each plan entry's CancelAfter instead of config.CancelAt. A cancellation reached at that boundary takes precedence over completion and comparison, even when that Poll completed the last operation. Cancel only unfinished operations, in backend order, append one `cancel` event for each (nil Input/Value), return `cancelled`, and never start another round. An already completed operation is never canceled. MaxPolls exhaustion is checked after cancellation/completion at the same boundary. No partially executed round contributes to Rounds.
- At the completion barrier, call Normalize once per backend in declared order (nil means identity), giving it its own result copy. Byte-compare copied normalized values. Compare only after every backend completed. Equal increments Rounds; unequal returns `mismatch` and excludes that round from Rounds. Do not compare pending or cancelled results. Normalize is caller supplied; the framework provides scheduling and barrier semantics, not domain-specific normalization.
- Generated command choices, parameter draws, and poll choices all use the caller's Rapid bitstream and normal shrinkable grouping conventions. Do not use an extra RNG. Returned Plan and Events own all nested slices, including callback results; later callback mutation or caller edits cannot affect another report. Plan contains the attempted round prefix, including cancellation or mismatch. A generated cancelled trial returns normally. Differential reports a mismatch through t.Fatalf (so Check/MakeFuzz can find it); invalid data uses t.Skip, preserving Rapid's discard semantics. Equal returns normally.
- Replay makes fresh backends, consumes the supplied command/input/poll sequence with no draws or Input callback, and returns its report instead of calling testing methods. Command must be common-ready before any Start in that round. Plan length must equal config.Rounds unless an earlier round stops with a non-equal result. Invalid index, poll of a completed backend, insufficient poll choices, extra poll choices after completion/cancellation, negative CancelAfter, or a positive CancelAfter beyond the supplied polls gives `invalid`. Validate each round's shape before starting it, and stop on the first invalid action; never execute later entries. Replay's Plan contains owned copies of attempted entries, and valid generated plans reproduce all events/status/completed rounds. Cancellation/invalid replay cancels any unfinished operations and closes all backends. Previously completed rounds remain counted. No incomplete round is called a difference.

New tests use package rapid and task_support/taskcheck.Run/Check. At least one property must run through Check and one through MakeFuzz with fixed finite input; no unbounded fuzz campaign. A mismatch may be observed via Replay or an isolated normal Go test subprocess, not by letting a parent test fail. The demo uses two honest in-memory implementations, two completed commands, different poll counts, and prints exactly `equal rounds=2` followed by a newline.
