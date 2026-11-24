# Stateful differential observations

Add the following exported API to package commands. Keep existing Commands, Command, Prop and Replay behavior unchanged. Put the implementation in nonempty `commands/differential.go` and `commands/diff_values.go`, both built without constraints. Additional production Go files are allowed.

```go
type DiffKind uint8
const (
    DiffScalar DiffKind = iota
    DiffCreate
    DiffRef
    DiffList
    DiffObject
)
type DiffValue struct {
    Kind DiffKind
    Text string
    Items []DiffValue
    Fields map[string]DiffValue
}
type DiffOutput struct { Value DiffValue; Release []string }
type HandleBindings struct { /* private fields */ }
func (*HandleBindings) Resolve(identity string) (physical string, ok bool)
type DifferentialCommand interface {
    Command
    RunDifferential(SystemUnderTest, *HandleBindings) (DiffOutput, error)
}
type DifferentialCommands interface {
    Commands
    ImplementationNames() []string
    CloneState(State) State
    NewImplementation(name string, initial State) (SystemUnderTest, error)
    DestroyImplementation(name string, system SystemUnderTest)
}
func DifferentialProp(ctx context.Context, commands DifferentialCommands) gopter.Prop
func ReplayDifferential(ctx context.Context, commands DifferentialCommands, initial State, plan ...Command) *gopter.PropResult
```

The API is an extension of Gopter's stateful fuzzing runner, not an independent tree comparison utility. DifferentialProp must use the existing genActions command generation, sieve, command element shrinkers and deletion shrinker. It must actually execute every accepted candidate on new systems and support the ordinary generated result/argument reporting. Do not regenerate dependent suffixes or introduce a second command RNG. DifferentialProp creates no legacy NewSystemUnderTest; its named implementations replace that single system. Ordinary Prop/Replay and prop.ForAll stay compatible.

Execution and statuses:

- Snapshot ImplementationNames once per trial; at least two distinct nonempty names are required. Execute in this exact order for every command, never by map iteration or completion timing. nil ctx, nil command set, bad names or an unsupported Command give PropError with a nonnil Error. Invalid initial/command precondition gives PropUndecided before invoking that command. Already cancelled ctx gives PropUndecided before any factory. Commands/values are bounded deterministic caller data; no external services, clock scheduling or goroutine race tests are needed.
- Each generated trial, shrink attempt, and replay owns a fresh cloned initial model, independent CloneState copies for each named factory, and empty per-implementation handle maps. Do not pass mutable generation state, parent candidate state, or another implementation's state to a factory. Every successfully created system is destroyed exactly once, in reverse creation order, even on error, cancellation or panic. Factory failure is PropError; panic must not masquerade as equality or difference. Use Gopter's existing panic/error reporting for generated properties; replay may propagate a panic after cleanup.
- Require all runtime PreCondition checks before executing a command. Only RunDifferential is called in the new mode, not Command.Run. Give it a read-only snapshot of that implementation's bindings from the last committed round. Run callbacks synchronously. Deep-copy each returned observation before invoking the next implementation, since adapters may recycle buffers/maps. A callback error is PropError. Check ctx before each callback, after each callback, and before commit. If cancelled, return PropUndecided and stop the trial; do not compare a partially collected round or call NextState/PostCondition for it.
- When all outputs normalize and compare equal, compute NextState exactly once on a clone of the previous committed model. Then call PostCondition with that resulting model and each implementation's DiffOutput, in declared order; these callbacks only read state/output. Any unsuccessful postcondition ends the trial with its actual result. Commit normalized maps and model only if all postconditions succeed and ctx is still active. Physical systems need no rollback: every unsuccessful trial stops and destroys its systems. No later command may execute after mismatch, adapter error, invalid precondition or cancellation.
- Return PropTrue for an equal completed sequence, including an empty sequence. A valid but unequal observation is PropFalse, with a label identifying the first command index and first differing canonical tree path (object keys sorted lexically); differing release sets also count. Unknown references, invalid value shapes and bad releases are adapter errors (PropError), not equality or an implementation difference. Include the implementation name and step in adapter errors; do not report raw system addresses. Cancellation/invalid preconditions are never a differential failure.

Normalization:

- DiffScalar compares Text literally and has no Items/Fields. DiffCreate and DiffRef have nonempty Text (the physical handle) and no Items/Fields. DiffList has empty Text/Fields and compares ordered Items. DiffObject has empty Text/Items and compares Fields by sorted key; missing and empty fields differ. Empty lists/objects are valid. Unknown Kind or a conflicting nonempty field is an adapter error. The caller supplies an acyclic tree.
- Traversal is preorder, object keys in lexical order and list indices in increasing order. Paths use JSON Pointer escaping (`~` -> `~0`, `/` -> `~1`), root path is empty. The logical identity of a previously unseen DiffCreate handle is `decimal-zero-based-command-index:path`, e.g. `0:/handle`. A second Create for the same still-live physical string is an alias of its first identity, not a second allocation. A Ref must already be live, either from a committed previous command or an earlier Create in this output. Normalize both Create/Ref to that logical identity; compare identity rather than the marker spelling. Thus aliases remain aliases, while differing alias graphs are a real difference.
- Each implementation has its own physical-to-logical and logical-to-physical maps. Resolve accepts an exact live logical identity and returns that implementation's physical string; it must not allocate or mutate a binding. Empty/unknown/released identity returns ("",false). The snapshot given to a command cannot include tentative observations from other implementations in the current round.
- Stage map changes per implementation. After normalizing Value, map each Release physical string to a live identity, reject unknown or duplicate releases, and compare the sorted logical release sets as part of this round's result. Releases apply only on commit. The released handle may have been created or referenced in the same output. A later Create reusing its physical string gets the new command/path identity; it must not resolve under the old identity. Never share tentative maps with a sibling implementation, a parent candidate, or the next replay.

Shrinking and regression replay:

- The existing generic firstFailure accepts any non-successful PropResult. In this new mode, only a candidate whose real execution ends PropFalse may replace an existing PropFalse. PropUndecided and PropError candidates are skipped; their statuses must not replace the stored difference or its arguments. A first-run PropUndecided/PropError returns directly without treating it as a failure to minimize. Keep existing general-purpose behavior unchanged. Do not turn every error into False or call a precondition failure a mismatch.
- Reuse existing command shrink metadata and applicability filtering; count accepted shrinking attempts in Args as normal. At least two successive accepted shrinking rounds must be exercised by the new tests, with fresh physical handle numbering per run so map leakage would be visible. ReplayDifferential owns its factories/model/maps and executes exactly the supplied finite commands under the same oracle semantics, without drawing or shrinking. Check cancellation on an empty plan too.

The three required tests use package commands_test, `task_support/taskcheck.Run` and `c.Check` (import github.com/leanovate/gopter/task_support/taskcheck). Include actual generated properties and a final replay, not only calls to normalization helpers. The demo makes two honest in-memory handle stores with different physical numbering, creates two logical handles through the new runner, and prints exactly `equal handles=2` with a newline. No target implementation, demo or target tests have been provided.
