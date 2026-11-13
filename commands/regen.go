package commands

import (
	"fmt"
	"reflect"

	"github.com/leanovate/gopter"
	"github.com/leanovate/gopter/gen"
	"github.com/leanovate/gopter/prop"
)

// RegenerativeCommands is an optional Commands extension that enables the
// dependency-regenerating shrink mode.
//
// In the default shrink mode a shrunk candidate is discarded as soon as one
// of the surviving commands no longer satisfies its PreCondition in the
// rebuilt model. Commands that carry handles, capacities or session state can
// therefore never be shrunk past the command they depend on.
//
// When commands.Prop receives a Commands value implementing this interface it
// instead repairs every shrunk candidate: from the first changed slot onwards
// the model is rebuilt and incompatible commands are regenerated in their
// slot with the slot's original seed and size, so that valid, smaller
// counterexamples can still be reached.
//
// CloneState has to return a deep copy of the passed state. CommandSize has to
// be a pure function of the command and is only used to decide whether a
// repaired candidate is strictly smaller than its parent.
type RegenerativeCommands interface {
	Commands
	// CloneState returns a deep copy of state
	CloneState(state State) State
	// CommandSize calculates a pure, comparable size of a command
	CommandSize(command Command) uint64
}

// commandSlot keeps everything needed to regenerate the command in this exact
// slot independently of the other slots: its own random seed, the MinSize and
// MaxSize active while it was generated, and the sieve and element shrinker
// produced by GenCommand for its context. Slots keep their identity when other
// slots are deleted: surviving slots are never renumbered and keep their seed.
type commandSlot struct {
	command  Command
	seed     int64
	minSize  int
	maxSize  int
	sieve    func(interface{}) bool
	shrinker gopter.Shrinker
}

func (s *commandSlot) clone() *commandSlot {
	return &commandSlot{
		command:  s.command,
		seed:     s.seed,
		minSize:  s.minSize,
		maxSize:  s.maxSize,
		sieve:    s.sieve,
		shrinker: s.shrinker,
	}
}

// CommandPlan is the immutable argument of a regenerative property check.
// It is reported as both Arg and OrigArg of the PropResult, so the final and
// the original sequences can be replayed directly via Replay.
type CommandPlan struct {
	// initialState is never mutated: every model progression works on a clone
	initialState State
	slots        []*commandSlot
	regen        RegenerativeCommands
}

// InitialState returns an independent deep copy of the model the sequence was
// generated from.
func (p *CommandPlan) InitialState() State {
	return p.regen.CloneState(p.initialState)
}

// Commands returns an independent snapshot of the commands in the sequence.
// The commands themselves are immutable.
func (p *CommandPlan) Commands() []Command {
	commands := make([]Command, len(p.slots))
	for i, slot := range p.slots {
		commands[i] = slot.command
	}
	return commands
}

// Seeds returns an independent snapshot of the per-slot generation seeds.
// Deleted slots leave no entry; surviving slots keep their original seed.
func (p *CommandPlan) Seeds() []int64 {
	seeds := make([]int64, len(p.slots))
	for i, slot := range p.slots {
		seeds[i] = slot.seed
	}
	return seeds
}

// String renders the plan like the actions of the default shrink mode.
func (p *CommandPlan) String() string {
	names := make([]string, 0, len(p.slots))
	for _, slot := range p.slots {
		names = append(names, fmt.Sprintf("%v", slot.command))
	}
	return fmt.Sprintf("initialState=%v sequential=%v", p.initialState, names)
}

func (p *CommandPlan) cloneState() State {
	return p.regen.CloneState(p.initialState)
}

// run evaluates the plan against a real system under test, progressing an
// independent clone of the model. Pass/fail is decided solely by the real
// Run/PostCondition results.
func (p *CommandPlan) run(systemUnderTest SystemUnderTest) *gopter.PropResult {
	state := p.cloneState()
	propResult := &gopter.PropResult{Status: gopter.PropTrue}
	for _, slot := range p.slots {
		if !slot.command.PreCondition(state) {
			return &gopter.PropResult{Status: gopter.PropFalse}
		}
		result := slot.command.Run(systemUnderTest)
		state = slot.command.NextState(state)
		propResult = propResult.And(slot.command.PostCondition(state, result))
	}
	return propResult
}

func clonePlan(plan *CommandPlan) *CommandPlan {
	slots := make([]*commandSlot, len(plan.slots))
	for i, slot := range plan.slots {
		slots[i] = slot.clone()
	}
	return &CommandPlan{
		initialState: plan.initialState,
		slots:        slots,
		regen:        plan.regen,
	}
}

// planSize is the lexicographic measure used to reject repaired candidates
// that did not become strictly smaller.
type planSize struct {
	length int
	sizes  []uint64
}

func measurePlan(plan *CommandPlan) planSize {
	sizes := make([]uint64, len(plan.slots))
	for i, slot := range plan.slots {
		sizes[i] = plan.regen.CommandSize(slot.command)
	}
	return planSize{length: len(plan.slots), sizes: sizes}
}

func lessPlanSize(smaller, larger planSize) bool {
	if smaller.length != larger.length {
		return smaller.length < larger.length
	}
	for i := range smaller.sizes {
		if smaller.sizes[i] != larger.sizes[i] {
			return smaller.sizes[i] < larger.sizes[i]
		}
	}
	return false
}

// propRegenerative is the property used when the Commands value implements
// RegenerativeCommands. Every attempt gets a fresh system and independent
// model clones; the system is still destroyed through the existing defer path.
func propRegenerative(commands RegenerativeCommands) gopter.Prop {
	return prop.ForAll(func(plan *CommandPlan) *gopter.PropResult {
		systemUnderTest := commands.NewSystemUnderTest(plan.cloneState())
		defer commands.DestroySystemUnderTest(systemUnderTest)

		return plan.run(systemUnderTest)
	}, genActionsRegenerative(commands))
}

func genActionsRegenerative(commands RegenerativeCommands) gopter.Gen {
	genInitialState := commands.GenInitialState()
	genInitialStateProvider := gopter.Gen(func(params *gopter.GenParameters) *gopter.GenResult {
		seed := params.NextInt64()
		return gopter.NewGenResult(func() State {
			paramsWithSeed := params.CloneWithSeed(seed)
			if initialState, ok := genInitialState(paramsWithSeed).Retrieve(); ok {
				return initialState
			}
			return nil
		}, gopter.NoShrinker)
	}).SuchThat(func(initialStateProvider func() State) bool {
		state := initialStateProvider()
		return state != nil && commands.InitialPreCondition(state)
	})
	return genInitialStateProvider.FlatMap(func(v interface{}) gopter.Gen {
		initialStateProvider := v.(func() State)
		return genSizedCommandPlan(commands, initialStateProvider).
			WithShrinker(regenerativeActionsShrinker(commands)).
			SuchThat(func(plan *CommandPlan) bool {
				state := plan.cloneState()
				for _, slot := range plan.slots {
					if !slot.command.PreCondition(state) {
						return false
					}
					state = slot.command.NextState(state)
				}
				return true
			})
	}, reflect.TypeOf(&CommandPlan{}))
}

func genSizedCommandPlan(commands RegenerativeCommands, initialStateProvider func() State) gopter.Gen {
	return func(genParams *gopter.GenParameters) *gopter.GenResult {
		plan := &CommandPlan{
			initialState: initialStateProvider(),
			slots:        make([]*commandSlot, 0, genParams.MaxSize),
			regen:        commands,
		}
		// The generation model is an independent clone; NextState may mutate it.
		state := commands.CloneState(plan.initialState)
		for i := 0; i < genParams.MaxSize; i++ {
			slot, nextState, ok := generateSlot(commands, state, genParams)
			if !ok {
				return gopter.NewEmptyResult(reflect.TypeOf(&CommandPlan{}))
			}
			plan.slots = append(plan.slots, slot)
			state = nextState
		}
		return gopter.NewGenResult(plan, gopter.NoShrinker)
	}
}

// generateSlot draws a single slot seed from the shared parameters and then
// runs GenCommand on a private, seed-locked RNG. Retries inside the slot
// therefore never consume randomness meant for later slots.
func generateSlot(commands RegenerativeCommands, state State, genParams *gopter.GenParameters) (*commandSlot, State, bool) {
	seed := genParams.NextInt64()
	result, ok := retryCommandResult(commands, state, seed, genParams.MinSize, genParams.MaxSize)
	if !ok {
		return nil, nil, false
	}
	value, _ := result.Retrieve()
	command := value.(Command)
	slot := &commandSlot{
		command:  command,
		seed:     seed,
		minSize:  genParams.MinSize,
		maxSize:  genParams.MaxSize,
		sieve:    result.Sieve,
		shrinker: result.Shrinker,
	}
	return slot, command.NextState(state), true
}

// retryCommandResult reruns GenCommand on a private RNG derived from the
// slot's original seed and size. The 100-attempt limit mirrors gen.RetryUntil
// in the default generation path.
//
// The state passed to GenCommand and the one captured by the resulting sieve
// are a private clone: NextState is allowed to mutate the working model in
// place, so the generator must never hold a pointer that later progression
// changes.
func retryCommandResult(commands RegenerativeCommands, state State, seed int64, minSize, maxSize int) (*gopter.GenResult, bool) {
	isolatedState := commands.CloneState(state)
	slotParams := (&gopter.GenParameters{}).CloneWithSeed(seed)
	slotParams.MinSize = minSize
	slotParams.MaxSize = maxSize
	result := gen.RetryUntil(commands.GenCommand(isolatedState), func(command Command) bool {
		return command.PreCondition(isolatedState)
	}, 100)(slotParams)
	if _, ok := result.Retrieve(); !ok {
		return nil, false
	}
	return result, true
}

// regenerativeActionsShrinker builds the raw deletion/element-shrink stream
// and maps every raw candidate through the dependency repair. Candidates that
// cannot be repaired or are not strictly smaller than their parent never leave
// the shrinker and therefore never reach a real system.
func regenerativeActionsShrinker(commands RegenerativeCommands) gopter.Shrinker {
	return func(v interface{}) gopter.Shrink {
		plan := v.(*CommandPlan)
		parentSize := measurePlan(plan)
		elementShrinker := gopter.Shrinker(func(v interface{}) gopter.Shrink {
			slot := v.(*commandSlot)
			return slot.shrinker(slot.command).Filter(slot.sieve).Map(func(command Command) *commandSlot {
				return &commandSlot{
					command:  command,
					seed:     slot.seed,
					minSize:  slot.minSize,
					maxSize:  slot.maxSize,
					sieve:    slot.sieve,
					shrinker: slot.shrinker,
				}
			})
		})
		rawShrink := gen.SliceShrinker(elementShrinker)(plan.slots)
		return func() (interface{}, bool) {
			for {
				raw, ok := rawShrink()
				if !ok {
					return nil, false
				}
				candidate, repaired := repairPlan(commands, plan, raw.([]*commandSlot))
				if !repaired {
					// A slot exhausted its retries: drop the whole candidate,
					// it is neither a failure nor a success, and keep looking.
					continue
				}
				if !lessPlanSize(measurePlan(candidate), parentSize) {
					// Equal or larger repaired candidates must not reach a
					// system instance, otherwise shrinking could oscillate.
					continue
				}
				return candidate, true
			}
		}
	}
}

// repairPlan rebuilds the model from a fresh initial state and repairs every
// slot from the first changed position onwards. It never mutates the parent
// plan and returns ok=false as soon as a slot exhausts its regeneration
// retries.
func repairPlan(commands RegenerativeCommands, parent *CommandPlan, raw []*commandSlot) (*CommandPlan, bool) {
	candidate := &CommandPlan{
		initialState: parent.initialState,
		slots:        make([]*commandSlot, 0, len(raw)),
		regen:        commands,
	}
	model := commands.CloneState(parent.initialState)

	firstChange := len(raw)
	for i := range raw {
		if i >= len(parent.slots) || raw[i] != parent.slots[i] {
			firstChange = i
			break
		}
	}

	for i, oldSlot := range raw {
		if i < firstChange {
			// The unchanged prefix is valid in the same context by construction.
			candidate.slots = append(candidate.slots, oldSlot.clone())
			model = oldSlot.command.NextState(model)
			continue
		}

		// Re-obtain GenCommand's value and metadata in the rebuilt context with
		// the slot's original seed and sizes.
		fresh, ok := retryCommandResult(commands, model, oldSlot.seed, oldSlot.minSize, oldSlot.maxSize)
		if !ok {
			return nil, false
		}
		value, _ := fresh.Retrieve()
		generated := value.(Command)

		command := oldSlot.command
		if !reflect.TypeOf(command).AssignableTo(fresh.ResultType) ||
			!command.PreCondition(model) ||
			(fresh.Sieve != nil && !fresh.Sieve(command)) {
			// The old command no longer fits the rebuilt context: replace it
			// with the freshly generated applicable command.
			command = generated
		}
		// The command that changes a parameter must itself satisfy the new context.
		if !command.PreCondition(model) || (fresh.Sieve != nil && !fresh.Sieve(command)) {
			return nil, false
		}

		candidate.slots = append(candidate.slots, &commandSlot{
			command: command,
			seed:    oldSlot.seed,
			minSize: oldSlot.minSize,
			maxSize: oldSlot.maxSize,
			// Keep the regenerated metadata: later shrink rounds must continue
			// with the sieve/shrinker of the current context.
			sieve:    fresh.Sieve,
			shrinker: fresh.Shrinker,
		})
		model = command.NextState(model)
	}
	return candidate, true
}
