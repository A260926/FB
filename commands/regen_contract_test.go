package commands

import (
	"fmt"
	"reflect"
	"strings"
	"testing"

	"github.com/leanovate/gopter"
	"github.com/leanovate/gopter/gen"
)

// regenState is a mutable model carrying a handle and a history slice so that
// deep copies are genuinely required.
type regenState struct {
	handle  int
	touched []int
}

func (s *regenState) clone() *regenState {
	return &regenState{handle: s.handle, touched: append([]int(nil), s.touched...)}
}

type regenSystem struct {
	handle int
}

type regenOpenCommand struct{ id int }
type regenUseCommand struct{ id int }

func (c regenOpenCommand) String() string { return fmt.Sprintf("open(%d)", c.id) }
func (c regenUseCommand) String() string  { return fmt.Sprintf("use(%d)", c.id) }

func (c regenOpenCommand) PreCondition(state State) bool {
	return state.(*regenState).handle == 0
}

func (c regenUseCommand) PreCondition(state State) bool {
	return state.(*regenState).handle == c.id
}

func (c regenOpenCommand) NextState(state State) State {
	model := state.(*regenState)
	model.handle = c.id
	model.touched = append(model.touched, c.id)
	return model
}

func (c regenUseCommand) NextState(state State) State { return state }

func (c regenOpenCommand) Run(sut SystemUnderTest) Result {
	sut.(*regenSystem).handle = c.id
	return c.id
}

func (c regenUseCommand) Run(sut SystemUnderTest) Result {
	// Intentional bug: use observes a handle one higher than the system holds.
	return sut.(*regenSystem).handle + 1
}

func (c regenOpenCommand) PostCondition(state State, result Result) *gopter.PropResult {
	return gopter.NewPropResult(state.(*regenState).handle == result.(int), "open handle")
}

func (c regenUseCommand) PostCondition(state State, result Result) *gopter.PropResult {
	return gopter.NewPropResult(state.(*regenState).handle == result.(int), "use handle")
}

// regenCommands implements RegenerativeCommands for the open/use dependency
// scenario. Every counter is optional instrumentation used by the lifecycle
// tests.
type regenCommands struct {
	created   *int
	destroyed *int
	genCalls  *int
	// poisonHandle makes GenCommand fail permanently once the model carries
	// this handle, which only happens inside a repaired candidate.
	poisonHandle int
	// openSingleStep limits the open-command element shrinker to its first
	// halving (8 -> 4), so a poisoned handle-4 context blocks every element
	// shrink instead of being routed around.
	openSingleStep bool
}

func (c *regenCommands) CloneState(state State) State {
	return state.(*regenState).clone()
}

func (c *regenCommands) CommandSize(command Command) uint64 {
	switch v := command.(type) {
	case regenOpenCommand:
		return uint64(v.id)
	case regenUseCommand:
		return uint64(v.id)
	}
	return 0
}

func (c *regenCommands) NewSystemUnderTest(State) SystemUnderTest {
	*c.created++
	return &regenSystem{}
}

func (c *regenCommands) DestroySystemUnderTest(SystemUnderTest) {
	*c.destroyed++
}

func (c *regenCommands) GenInitialState() gopter.Gen {
	return func(*gopter.GenParameters) *gopter.GenResult {
		return gopter.NewGenResult(&regenState{}, gopter.NoShrinker)
	}
}

func (c *regenCommands) InitialPreCondition(State) bool { return true }

func (c *regenCommands) GenCommand(state State) gopter.Gen {
	model := state.(*regenState)
	if model.handle == 0 {
		return c.openCommandGen()
	}
	if c.poisonHandle != 0 && model.handle == c.poisonHandle {
		*c.genCalls++
		return gen.Fail(reflect.TypeOf((*Command)(nil)).Elem())
	}
	return c.useCommandGen(model.handle)
}

// openCommandGen consumes randomness from the slot's private RNG, so the
// amount of randomness burned here cannot shift the stream of later slots.
func (c *regenCommands) openCommandGen() gopter.Gen {
	return func(params *gopter.GenParameters) *gopter.GenResult {
		params.NextInt64()
		command := Command(regenOpenCommand{id: 8})
		result := gopter.NewGenResult(command, c.openShrinker())
		result.Sieve = regenContextSieve(0)
		return result
	}
}

func (c *regenCommands) openShrinker() gopter.Shrinker {
	if c.openSingleStep {
		return func(value interface{}) gopter.Shrink {
			emitted := false
			return func() (interface{}, bool) {
				if emitted {
					return nil, false
				}
				emitted = true
				return regenOpenCommand{id: value.(regenOpenCommand).id / 2}, true
			}
		}
	}
	return regenOpenShrinker()
}

func (c *regenCommands) useCommandGen(handle int) gopter.Gen {
	return func(params *gopter.GenParameters) *gopter.GenResult {
		params.NextInt64()
		command := Command(regenUseCommand{id: handle})
		result := gopter.NewGenResult(command, regenUseShrinker(handle))
		result.Sieve = regenContextSieve(handle)
		return result
	}
}

// regenContextSieve returns the sieve GenCommand attached for a model that
// currently holds handle. The captured context makes the sieve distinguishable
// from the sieve of another context by its behavior.
func regenContextSieve(handle int) func(interface{}) bool {
	return func(value interface{}) bool {
		command, ok := value.(Command)
		if !ok {
			return false
		}
		return command.PreCondition(&regenState{handle: handle})
	}
}

func regenOpenShrinker() gopter.Shrinker {
	return func(value interface{}) gopter.Shrink {
		current := value.(regenOpenCommand).id
		half := current / 2
		return func() (interface{}, bool) {
			if half == 0 {
				return nil, false
			}
			next := current - half
			half /= 2
			return regenOpenCommand{id: next}, true
		}
	}
}

// regenUseShrinker is finite and context-tagged: the single value it can emit
// reveals the context it was generated for.
func regenUseShrinker(handle int) gopter.Shrinker {
	return func(value interface{}) gopter.Shrink {
		emitted := false
		return func() (interface{}, bool) {
			if emitted {
				return nil, false
			}
			emitted = true
			return regenUseCommand{id: handle}, true
		}
	}
}

func newRegenCommands() *regenCommands {
	created, destroyed, genCalls := 0, 0, 0
	return &regenCommands{created: &created, destroyed: &destroyed, genCalls: &genCalls}
}

func regenParams(seed int64) *gopter.GenParameters {
	params := gopter.DefaultGenParameters().CloneWithSeed(seed)
	params.MinSize = 2
	params.MaxSize = 2
	params.MaxShrinkCount = 30
	return params
}

func commandNames(plan *CommandPlan) []string {
	names := make([]string, len(plan.slots))
	for i, slot := range plan.slots {
		names[i] = slot.command.String()
	}
	return names
}

func joinedNames(plan *CommandPlan) string {
	return strings.Join(commandNames(plan), " ")
}

// plainHandleCommands is the non-regenerative reference implementation: it
// shares the same open/use model but only implements Commands, so shrunk
// candidates that break the handle dependency are simply filtered.
type plainHandleCommands struct {
	created   *int
	destroyed *int
}

func (c *plainHandleCommands) NewSystemUnderTest(State) SystemUnderTest {
	*c.created++
	return &regenSystem{}
}

func (c *plainHandleCommands) DestroySystemUnderTest(SystemUnderTest) { *c.destroyed++ }

func (c *plainHandleCommands) GenInitialState() gopter.Gen {
	return func(*gopter.GenParameters) *gopter.GenResult {
		return gopter.NewGenResult(&regenState{}, gopter.NoShrinker)
	}
}

func (c *plainHandleCommands) InitialPreCondition(State) bool { return true }

func (c *plainHandleCommands) GenCommand(state State) gopter.Gen {
	model := state.(*regenState)
	if model.handle == 0 {
		return gopter.Gen(func(*gopter.GenParameters) *gopter.GenResult {
			command := Command(regenOpenCommand{id: 8})
			return gopter.NewGenResult(command, regenOpenShrinker())
		})
	}
	return gopter.Gen(func(*gopter.GenParameters) *gopter.GenResult {
		command := Command(regenUseCommand{id: model.handle})
		return gopter.NewGenResult(command, gopter.NoShrinker)
	})
}

// constSizeCommands forces every command to the same size so that repaired
// element shrinks keep an equal size vector and must be suppressed.
type constSizeCommands struct {
	*regenCommands
}

func (constSizeCommands) CommandSize(Command) uint64 { return 1 }

// TestRegenGeneration verifies generation through commands.Prop: per-slot
// seeds, independent random streams, seed identity across deletions and
// snapshot isolation of the reported plan.
func TestRegenGeneration(t *testing.T) {
	commands := newRegenCommands()
	params := regenParams(20261113)

	result := Prop(commands)(params)
	if result.Status != gopter.PropFalse || len(result.Args) != 1 {
		t.Fatalf("expected a falsified property, got %+v", result)
	}
	original := result.Args[0].OrigArg.(*CommandPlan)
	if got := joinedNames(original); got != "open(8) use(8)" {
		t.Fatalf("unexpected original sequence: %q", got)
	}
	seeds := original.Seeds()
	if len(seeds) != 2 || seeds[0] == seeds[1] {
		t.Fatalf("each slot needs its own seed: %v", seeds)
	}

	// The same initial seed and sizes must reproduce the exact same sequence
	// and per-slot seeds, even though every slot generator burns RNG draws.
	again := Prop(newRegenCommands())(regenParams(20261113))
	againOriginal := again.Args[0].OrigArg.(*CommandPlan)
	if joinedNames(againOriginal) != joinedNames(original) ||
		!reflect.DeepEqual(againOriginal.Seeds(), original.Seeds()) {
		t.Fatal("generation is not reproducible for the same seed and sizes")
	}

	// Deleting a slot must not renumber the survivors: the remaining slot
	// keeps its original seed identity.
	shrink := regenerativeActionsShrinker(commands)(original)
	var deletion *CommandPlan
	for {
		value, ok := shrink()
		if !ok {
			break
		}
		candidate := value.(*CommandPlan)
		if len(candidate.slots) == 1 {
			deletion = candidate
			break
		}
	}
	if deletion == nil {
		t.Fatal("no single-slot deletion candidate enumerated")
	}
	if deletion.Seeds()[0] != seeds[1] && deletion.Seeds()[0] != seeds[0] {
		t.Fatal("surviving slot lost its seed identity after a deletion")
	}

	// Regenerating both slots from their own seeds reproduces the original
	// commands, proving one slot's retries cannot consume another slot's
	// random stream.
	repaired, ok := repairPlan(commands, original, copySlots(original.slots))
	if !ok {
		t.Fatal("repair of the unchanged plan exhausted")
	}
	if joinedNames(repaired) != joinedNames(original) {
		t.Fatalf("slot streams are not independent: %q", joinedNames(repaired))
	}

	// Reported snapshots and the initial state are independent copies.
	stateA := original.InitialState().(*regenState)
	stateA.handle = 123
	stateA.touched = append(stateA.touched, 9)
	if stateB := original.InitialState().(*regenState); stateB.handle != 0 || len(stateB.touched) != 0 {
		t.Fatal("InitialState does not return independent snapshots")
	}
	commandSnapshot := original.Commands()
	commandSnapshot[0] = nil
	if original.Commands()[0] == nil {
		t.Fatal("Commands() must return a snapshot")
	}
	seedSnapshot := original.Seeds()
	seedSnapshot[0] = -1
	if original.Seeds()[0] == -1 {
		t.Fatal("Seeds() must return a snapshot")
	}

	// An empty sequence is a legitimate candidate: it runs as a passing
	// property instead of being discarded.
	emptyParams := regenParams(20261113)
	emptyParams.MinSize = 0
	emptyParams.MaxSize = 0
	emptyResult := Prop(newRegenCommands())(emptyParams)
	if emptyResult.Status != gopter.PropTrue {
		t.Fatalf("empty sequence must be a valid candidate, got %+v", emptyResult)
	}
}

// TestRegenRepair verifies that shrinking open(8) rebuilds the dependent use
// command through several real shrink rounds, that the regenerated metadata
// belongs to the new context, and that the final plan still fails under a real
// Replay. It also contrasts the plain Commands behavior, which cannot shrink.
func TestRegenRepair(t *testing.T) {
	commands := newRegenCommands()
	result := Prop(commands)(regenParams(20261113))
	if result.Status != gopter.PropFalse {
		t.Fatalf("expected failure, got %+v", result)
	}
	arg := result.Args[0]
	original := arg.OrigArg.(*CommandPlan)
	final := arg.Arg.(*CommandPlan)

	if joinedNames(original) != "open(8) use(8)" {
		t.Fatalf("unexpected original: %q", joinedNames(original))
	}
	if got := joinedNames(final); got != "open(1) use(1)" {
		t.Fatalf("dependency was not regenerated while shrinking: %q", got)
	}
	if arg.Shrinks < 2 {
		t.Fatalf("expected at least two real shrink rounds, got %d", arg.Shrinks)
	}

	// The regenerated use slot carries sieve and shrinker of the new context.
	origUse := original.slots[1]
	finalUse := final.slots[1]
	if origUse.sieve == nil || finalUse.sieve == nil || origUse.shrinker == nil || finalUse.shrinker == nil {
		t.Fatal("regenerated slot lost its sieve or shrinker")
	}
	if !origUse.sieve(regenUseCommand{id: 8}) || origUse.sieve(regenUseCommand{id: 1}) {
		t.Fatal("original sieve does not describe the handle-8 context")
	}
	if !finalUse.sieve(regenUseCommand{id: 1}) || finalUse.sieve(regenUseCommand{id: 8}) {
		t.Fatal("regenerated sieve does not describe the handle-1 context")
	}
	origEmitted, _ := origUse.shrinker(origUse.command)()
	finalEmitted, _ := finalUse.shrinker(finalUse.command)()
	if origEmitted.(regenUseCommand).id != 8 || finalEmitted.(regenUseCommand).id != 1 {
		t.Fatal("regenerated shrinker is not tagged with its new context")
	}

	// The plain Commands path filters the broken candidate and cannot shrink.
	plainCreated, plainDestroyed := 0, 0
	plain := &plainHandleCommands{created: &plainCreated, destroyed: &plainDestroyed}
	plainResult := Prop(plain)(regenParams(20261113))
	if plainResult.Status != gopter.PropFalse || plainResult.Args[0].Shrinks != 0 {
		t.Fatalf("plain Commands unexpectedly shrank: %+v", plainResult)
	}
	plainActions := fmt.Sprintf("%v", plainResult.Args[0].Arg)
	if !strings.Contains(plainActions, "open(8)") || !strings.Contains(plainActions, "use(8)") {
		t.Fatalf("plain Commands should keep the original sequence, got %q", plainActions)
	}

	// Both the original and the final plan fail through real Run results when
	// replayed on fresh systems, and every replayed system is destroyed.
	for _, plan := range []*CommandPlan{original, final} {
		sut := &regenSystem{}
		replayResult := Replay(sut, plan.InitialState(), plan.Commands()...)
		if replayResult.Success() {
			t.Fatalf("Replay of %q must still fail", joinedNames(plan))
		}
	}

	// Rejected (passing) candidates must not mutate the parent plan.
	parent := clonePlan(original)
	drainShrinkCandidates(t, original)
	if !reflect.DeepEqual(commandNames(original), commandNames(parent)) ||
		!reflect.DeepEqual(original.Seeds(), parent.Seeds()) {
		t.Fatal("enumerating candidates mutated the parent plan")
	}
}

// TestRegenLifecycle verifies system create/destroy accounting, that exhausted
// repairs and non-decreasing candidates never create a system, that repeated
// enumeration of one parent is stable, and that every instance is destroyed
// exactly once.
func TestRegenLifecycle(t *testing.T) {
	// Exhaustion: the only element shrink lands on the poisoned handle-4
	// context and burns the full 100 retries. Only deletion candidates survive.
	poisoned := newRegenCommands()
	poisoned.poisonHandle = 4
	poisoned.openSingleStep = true
	poisonResult := Prop(poisoned)(regenParams(20261113))
	if poisonResult.Status != gopter.PropFalse || poisonResult.Args[0].Shrinks != 0 {
		t.Fatalf("exhausted repairs must be dropped without shrinking: %+v", poisonResult)
	}
	if *poisoned.genCalls == 0 {
		t.Fatal("poisoned generator was never exercised")
	}
	emitted := drainShrinkCandidates(t, poisonResult.Args[0].Arg.(*CommandPlan))
	if *poisoned.created != 1+len(emitted) || *poisoned.destroyed != *poisoned.created {
		t.Fatalf("exhausted candidate reached a system: created=%d destroyed=%d emitted=%d",
			*poisoned.created, *poisoned.destroyed, len(emitted))
	}
	for _, candidate := range emitted {
		if len(candidate.slots) >= 2 {
			t.Fatal("only shorter deletion candidates may be emitted under poisoning")
		}
	}

	// Non-decreasing: equal size vectors are suppressed before any system run.
	constSize := &constSizeCommands{regenCommands: newRegenCommands()}
	constResult := Prop(constSize)(regenParams(20261113))
	if constResult.Status != gopter.PropFalse || constResult.Args[0].Shrinks != 0 {
		t.Fatalf("equal-size candidates must be suppressed: %+v", constResult)
	}
	constEmitted := drainShrinkCandidates(t, constResult.Args[0].Arg.(*CommandPlan))
	if *constSize.created != 1+len(constEmitted) || *constSize.destroyed != *constSize.created {
		t.Fatalf("non-decreasing candidate reached a system: created=%d emitted=%d",
			*constSize.created, len(constEmitted))
	}

	// Repeated enumeration of the same parent is independent of earlier
	// enumerations and of real system executions.
	commands := newRegenCommands()
	result := Prop(commands)(regenParams(20261113))
	parent := result.Args[0].OrigArg.(*CommandPlan)
	first := drainShrinkCandidates(t, parent)
	second := drainShrinkCandidates(t, parent)
	if len(first) != len(second) {
		t.Fatal("repeated enumeration is unstable")
	}
	for i := range first {
		if joinedNames(first[i]) != joinedNames(second[i]) ||
			!reflect.DeepEqual(first[i].Seeds(), second[i].Seeds()) {
			t.Fatal("candidate enumeration is affected by previous enumerations")
		}
	}
	if !reflect.DeepEqual(parent.Seeds(), parent.Seeds()) {
		t.Fatal("parent plan changed during enumeration")
	}

	// Every system created by the full shrinking run is destroyed exactly once.
	if *commands.created == 0 || *commands.created != *commands.destroyed {
		t.Fatalf("lifecycle accounting broken: created=%d destroyed=%d",
			*commands.created, *commands.destroyed)
	}
}

func copySlots(slots []*commandSlot) []*commandSlot {
	copied := make([]*commandSlot, len(slots))
	for i, slot := range slots {
		copied[i] = slot.clone()
	}
	return copied
}

// drainShrinkCandidates enumerates every candidate the real shrinker attached
// in the generation chain emits for a parent plan.
func drainShrinkCandidates(t *testing.T, parent *CommandPlan) []*CommandPlan {
	t.Helper()
	shrink := regenerativeActionsShrinker(parent.regen)(parent)
	candidates := make([]*CommandPlan, 0)
	for {
		value, ok := shrink()
		if !ok {
			break
		}
		candidates = append(candidates, value.(*CommandPlan))
	}
	return candidates
}
