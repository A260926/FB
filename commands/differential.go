package commands

import (
	"context"
	"errors"
	"fmt"

	"github.com/leanovate/gopter"
)

// DifferentialCommand is a stateful command that can be compared across
// multiple local implementations. In differential mode RunDifferential is
// called instead of Command.Run.
type DifferentialCommand interface {
	Command
	// RunDifferential executes the command against one implementation and
	// returns its observation. bindings is a read-only snapshot of that
	// implementation's committed handle bindings.
	RunDifferential(systemUnderTest SystemUnderTest, bindings *HandleBindings) (DiffOutput, error)
}

// DifferentialCommands is a Commands suite backed by several named local
// implementations that should be compared against each other.
type DifferentialCommands interface {
	Commands

	// ImplementationNames returns the implementation names in exact
	// execution/declaration order. Must contain at least two distinct,
	// nonempty names. It is snapshotted once per trial.
	ImplementationNames() []string
	// CloneState produces an independent copy of the model state.
	CloneState(state State) State
	// NewImplementation creates a fresh named system backed by the given
	// independent state copy.
	NewImplementation(name string, initial State) (SystemUnderTest, error)
	// DestroyImplementation destroys a system previously created by
	// NewImplementation.
	DestroyImplementation(name string, system SystemUnderTest)
}

// DifferentialProp creates a gopter.Prop that generates stateful actions with
// the existing command machinery and executes every generated, shrunk and
// replayed candidate against all named implementations.
func DifferentialProp(ctx context.Context, differentialCommands DifferentialCommands) gopter.Prop {
	return gopter.SaveProp(func(genParams *gopter.GenParameters) *gopter.PropResult {
		if configErr := checkDifferentialRequired(ctx, differentialCommands); configErr != nil {
			return &gopter.PropResult{Status: gopter.PropError, Error: configErr}
		}

		genResult := genActions(differentialCommands)(genParams)
		value, ok := genResult.Retrieve()
		if !ok {
			return &gopter.PropResult{Status: gopter.PropUndecided}
		}
		original := value.(*actions)
		originalFormatted := fmt.Sprintf("%+v", original)

		runCandidate := func(candidate *actions) *gopter.PropResult {
			plan := make([]Command, 0, len(candidate.sequentialCommands))
			for _, sc := range candidate.sequentialCommands {
				plan = append(plan, sc.command)
			}
			return executeDifferential(ctx, differentialCommands, candidate.initialStateProvider(), plan)
		}

		firstFailure := runCandidate(original)

		// Only a genuine PropFalse may start and drive differential
		// minimization. PropTrue/PropUndecided/PropError on the first run are
		// returned directly and are never minimized.
		if firstFailure.Status != gopter.PropFalse {
			return firstFailure.AddArgs(gopter.NewPropArg(
				genResult, 0, original, originalFormatted, original, originalFormatted,
			))
		}

		lastFailure := firstFailure
		lastValue := original
		lastFormatted := originalFormatted

		acceptedShrinks := 0
		shrink := genResult.Shrinker(lastValue).Filter(genResult.Sieve)
		nextFailure, nextValue, nextFormatted := firstDifferentialFailure(shrink, runCandidate)

		for nextFailure != nil && acceptedShrinks < genParams.MaxShrinkCount {
			acceptedShrinks++
			lastFailure = nextFailure
			lastValue = nextValue
			lastFormatted = nextFormatted

			shrink = genResult.Shrinker(lastValue).Filter(genResult.Sieve)
			nextFailure, nextValue, nextFormatted = firstDifferentialFailure(shrink, runCandidate)
		}

		arg := gopter.NewPropArg(
			genResult,
			acceptedShrinks,
			lastValue,
			lastFormatted,
			original,
			originalFormatted,
		)
		return lastFailure.WithArgs(firstFailure.Args).AddArgs(arg)
	})
}

// firstDifferentialFailure mirrors prop.firstFailure but only accepts
// candidates whose real execution ends PropFalse. PropUndecided and
// PropError candidates are skipped instead of replacing a stored
// differential failure.
func firstDifferentialFailure(shrink gopter.Shrink, check func(*actions) *gopter.PropResult) (*gopter.PropResult, *actions, string) {
	value, ok := shrink()
	for ok {
		candidate, candidateOK := value.(*actions)
		valueFormatted := fmt.Sprintf("%+v", value)
		if candidateOK {
			result := check(candidate)
			if result != nil && result.Status == gopter.PropFalse {
				return result, candidate, valueFormatted
			}
		}
		value, ok = shrink()
	}
	return nil, nil, ""
}

// ReplayDifferential executes an explicit finite command plan against fresh
// factories, models and handle maps under the same oracle semantics as
// DifferentialProp. It draws and shrinks nothing. Panics from factories,
// commands or callbacks are propagated after systems have been destroyed.
func ReplayDifferential(ctx context.Context, differentialCommands DifferentialCommands, initial State, plan ...Command) *gopter.PropResult {
	return executeDifferential(ctx, differentialCommands, initial, plan)
}

// differentialRuntime holds the live system and committed handle table of
// one named implementation for the duration of a single trial.
type differentialRuntime struct {
	name      string
	system    SystemUnderTest
	committed *handleTable
}

func checkDifferentialRequired(ctx context.Context, differentialCommands DifferentialCommands) error {
	if ctx == nil {
		return errors.New("commands: differential context must not be nil")
	}
	if differentialCommands == nil {
		return errors.New("commands: differential command set must not be nil")
	}
	return nil
}

// snapshotImplementationNames takes the single per-trial snapshot of the
// declared implementation order and validates it.
func snapshotImplementationNames(differentialCommands DifferentialCommands) ([]string, error) {
	declared := differentialCommands.ImplementationNames()
	names := append([]string(nil), declared...)
	if len(names) < 2 {
		return nil, fmt.Errorf("commands: differential comparison requires at least two implementation names, got %d", len(names))
	}
	seen := make(map[string]struct{}, len(names))
	for _, name := range names {
		if name == "" {
			return nil, errors.New("commands: differential implementation names must be nonempty")
		}
		if _, duplicate := seen[name]; duplicate {
			return nil, fmt.Errorf("commands: differential implementation names must be distinct, duplicate %q", name)
		}
		seen[name] = struct{}{}
	}
	return names, nil
}

func executeDifferential(
	ctx context.Context,
	differentialCommands DifferentialCommands,
	initial State,
	plan []Command,
) *gopter.PropResult {
	if configErr := checkDifferentialRequired(ctx, differentialCommands); configErr != nil {
		return &gopter.PropResult{Status: gopter.PropError, Error: configErr}
	}

	// Snapshot the declared implementation order exactly once per trial.
	names, namesErr := snapshotImplementationNames(differentialCommands)
	if namesErr != nil {
		return &gopter.PropResult{Status: gopter.PropError, Error: namesErr}
	}

	// An already cancelled context is undecided before any factory runs.
	if cancellation := cancellationResult(ctx); cancellation != nil {
		return cancellation
	}

	if !differentialCommands.InitialPreCondition(initial) {
		return &gopter.PropResult{Status: gopter.PropUndecided}
	}

	// The committed model starts from an independent clone of the raw
	// initial state.
	model := differentialCommands.CloneState(initial)

	runtimes := make([]*differentialRuntime, len(names))
	// Every successfully created system is destroyed exactly once, in
	// reverse creation order, including on error, cancellation and panic.
	defer func() {
		for i := len(runtimes) - 1; i >= 0; i-- {
			if runtime := runtimes[i]; runtime != nil && runtime.system != nil {
				differentialCommands.DestroyImplementation(runtime.name, runtime.system)
			}
		}
	}()

	for i, name := range names {
		// Each factory receives its own independent state clone, never the
		// mutable generation state or another implementation's state.
		implementationInitial := differentialCommands.CloneState(initial)
		system, factoryErr := differentialCommands.NewImplementation(name, implementationInitial)
		if factoryErr != nil {
			return &gopter.PropResult{
				Status: gopter.PropError,
				Error:  fmt.Errorf("commands: implementation %q factory failed: %w", name, factoryErr),
			}
		}
		runtimes[i] = &differentialRuntime{
			name:      name,
			system:    system,
			committed: newHandleTable(),
		}
	}

	for step, command := range plan {
		if cancellation := cancellationResult(ctx); cancellation != nil {
			return cancellation
		}

		if command == nil {
			return &gopter.PropResult{
				Status: gopter.PropError,
				Error:  fmt.Errorf("commands: nil command at step %d", step),
			}
		}

		// Runtime precondition checks happen before executing a command.
		if !command.PreCondition(model) {
			return &gopter.PropResult{Status: gopter.PropUndecided}
		}

		differentialCommand, supported := command.(DifferentialCommand)
		if !supported {
			return &gopter.PropResult{
				Status: gopter.PropError,
				Error: fmt.Errorf(
					"commands: command %q at step %d does not implement DifferentialCommand (RunDifferential)",
					command.String(), step,
				),
			}
		}

		outputs := make([]DiffOutput, len(runtimes))
		for i, runtime := range runtimes {
			if cancellation := cancellationResult(ctx); cancellation != nil {
				return cancellation
			}

			bindings := runtime.committed.snapshot()
			output, runErr := differentialCommand.RunDifferential(runtime.system, bindings)
			if runErr != nil {
				return &gopter.PropResult{
					Status: gopter.PropError,
					Error: fmt.Errorf(
						"commands: differential adapter error in implementation %q at step %d (%s): %w",
						runtime.name, step, command.String(), runErr,
					),
				}
			}
			// Deep copy before invoking the next implementation: adapters
			// may recycle the buffers/maps they returned.
			outputs[i] = deepCopyOutput(output)

			if cancellation := cancellationResult(ctx); cancellation != nil {
				return cancellation
			}
		}

		// Stage each implementation's maps independently. Nothing is shared
		// between siblings and nothing is committed yet.
		type stagedRound struct {
			value    DiffValue
			releases []string
			table    *handleTable
		}
		stagedRounds := make([]stagedRound, len(runtimes))

		for i, runtime := range runtimes {
			table, normalizedValue, releases, normalizeErr := normalizeRound(step, runtime.committed, outputs[i])
			if normalizeErr != nil {
				return &gopter.PropResult{
					Status: gopter.PropError,
					Error: fmt.Errorf(
						"commands: differential adapter error in implementation %q at step %d (%s): %w",
						runtime.name, step, command.String(), normalizeErr,
					),
				}
			}
			stagedRounds[i] = stagedRound{
				value:    normalizedValue,
				releases: releases,
				table:    table,
			}
		}

		// Compare against the first declared implementation. Execution order
		// is declaration order; comparison reports the first canonical path.
		base := stagedRounds[0]
		for i := 1; i < len(stagedRounds); i++ {
			other := stagedRounds[i]
			if path, reason, differ := compareNormalized(base.value, other.value); differ {
				label := fmt.Sprintf(
					"differential mismatch command=%d path=%q identity=%d:%s between %q and %q: %s",
					step, path, step, path, runtimes[0].name, runtimes[i].name, reason,
				)
				return &gopter.PropResult{
					Status: gopter.PropFalse,
					Labels: []string{label},
				}
			}
			if !releasesEqual(base.releases, other.releases) {
				label := fmt.Sprintf(
					"differential mismatch command=%d path=%q identity=%d:%s between %q and %q: release sets differ: %v != %v",
					step, "", step, "", runtimes[0].name, runtimes[i].name,
					base.releases, other.releases,
				)
				return &gopter.PropResult{
					Status: gopter.PropFalse,
					Labels: []string{label},
				}
			}
		}

		// All observations are equivalent: compute NextState exactly once on
		// a clone of the previous committed model.
		nextModel := command.NextState(differentialCommands.CloneState(model))

		// Postconditions observe the resulting model and each implementation's
		// original DiffOutput, in declared order.
		for i := range runtimes {
			postResult := command.PostCondition(nextModel, outputs[i])
			if postResult != nil && !postResult.Success() {
				return postResult
			}
		}

		// Re-check cancellation immediately before committing the round.
		if cancellation := cancellationResult(ctx); cancellation != nil {
			return cancellation
		}

		model = nextModel
		for i := range runtimes {
			runtimes[i].committed = stagedRounds[i].table
		}
	}

	return &gopter.PropResult{Status: gopter.PropTrue}
}

func cancellationResult(ctx context.Context) *gopter.PropResult {
	if ctx != nil && ctx.Err() != nil {
		return &gopter.PropResult{Status: gopter.PropUndecided}
	}
	return nil
}

// DifferentialReplayPlan exposes the concrete command plan carried by a
// generated (and possibly shrunk) actions argument. It returns a fresh
// initial state value and the commands in order. It is intended for tests
// and tooling that need to feed the exact minimized candidate to
// ReplayDifferential.
func (a *actions) DifferentialReplayPlan() (State, []Command) {
	if a == nil {
		return nil, nil
	}
	var initial State
	if a.initialStateProvider != nil {
		initial = a.initialStateProvider()
	}
	plan := make([]Command, 0, len(a.sequentialCommands))
	for _, sc := range a.sequentialCommands {
		plan = append(plan, sc.command)
	}
	return initial, plan
}
