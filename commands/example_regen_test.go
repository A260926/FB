package commands_test

import (
	"fmt"

	"github.com/leanovate/gopter"
	"github.com/leanovate/gopter/commands"
)

// regenExampleState is the model: the handle currently held by the system.
type regenExampleState struct {
	handle int
}

// regenExampleSystem is the real system; its use operation is intentionally
// one ahead of the modeled handle, so every use(handle) fails its
// PostCondition and the property falsifies.
type regenExampleSystem struct {
	handle int
}

type regenOpen struct{ id int }
type regenUse struct{ id int }

func (c regenOpen) String() string { return fmt.Sprintf("open(%d)", c.id) }
func (c regenUse) String() string  { return fmt.Sprintf("use(%d)", c.id) }

func (c regenOpen) PreCondition(state commands.State) bool {
	return state.(*regenExampleState).handle == 0
}

func (c regenUse) PreCondition(state commands.State) bool {
	return state.(*regenExampleState).handle == c.id
}

func (c regenOpen) NextState(state commands.State) commands.State {
	state.(*regenExampleState).handle = c.id
	return state
}

func (c regenUse) NextState(state commands.State) commands.State { return state }

func (c regenOpen) Run(sut commands.SystemUnderTest) commands.Result {
	sut.(*regenExampleSystem).handle = c.id
	return c.id
}

func (c regenUse) Run(sut commands.SystemUnderTest) commands.Result {
	return sut.(*regenExampleSystem).handle + 1
}

func (c regenOpen) PostCondition(state commands.State, result commands.Result) *gopter.PropResult {
	return gopter.NewPropResult(state.(*regenExampleState).handle == result.(int), "open")
}

func (c regenUse) PostCondition(state commands.State, result commands.Result) *gopter.PropResult {
	return gopter.NewPropResult(state.(*regenExampleState).handle == result.(int), "use")
}

// regenExampleCommands implements commands.RegenerativeCommands. GenCommand
// only emits open while no handle is held and use afterwards; the two command
// kinds are linked through the model's handle.
type regenExampleCommands struct{}

func (regenExampleCommands) CloneState(state commands.State) commands.State {
	original := state.(*regenExampleState)
	return &regenExampleState{handle: original.handle}
}

func (regenExampleCommands) CommandSize(command commands.Command) uint64 {
	switch c := command.(type) {
	case regenOpen:
		return uint64(c.id)
	case regenUse:
		return uint64(c.id)
	}
	return 0
}

func (regenExampleCommands) NewSystemUnderTest(commands.State) commands.SystemUnderTest {
	return &regenExampleSystem{}
}

func (regenExampleCommands) DestroySystemUnderTest(commands.SystemUnderTest) {}

func (regenExampleCommands) GenInitialState() gopter.Gen {
	return func(*gopter.GenParameters) *gopter.GenResult {
		return gopter.NewGenResult(&regenExampleState{}, gopter.NoShrinker)
	}
}

func (regenExampleCommands) InitialPreCondition(commands.State) bool { return true }

func (regenExampleCommands) GenCommand(state commands.State) gopter.Gen {
	model := state.(*regenExampleState)
	if model.handle == 0 {
		// open shrinks by halving its id: 8 -> 4 -> 2 -> 1.
		return func(*gopter.GenParameters) *gopter.GenResult {
			command := commands.Command(regenOpen{id: 8})
			return gopter.NewGenResult(command, gopter.Shrinker(func(value interface{}) gopter.Shrink {
				current := value.(regenOpen).id
				half := current / 2
				return func() (interface{}, bool) {
					if half == 0 {
						return nil, false
					}
					next := current - half
					half /= 2
					return regenOpen{id: next}, true
				}
			}))
		}
	}
	return func(*gopter.GenParameters) *gopter.GenResult {
		return gopter.NewGenResult(commands.Command(regenUse{id: model.handle}), gopter.NoShrinker)
	}
}

// Example_regenerativeCommands shows the dependency-regenerating shrink mode.
//
// The generated sequence opens handle 8 and then uses that same handle. In the
// default shrink mode shrinking open(8) to open(4) would leave use(8) without
// a matching handle, so the whole candidate would be filtered out and nothing
// could shrink. With RegenerativeCommands the dependent use command is
// regenerated against the rebuilt model instead, and shrinking continues all
// the way down to open(1)/use(1).
func Example_regenerativeCommands() {
	params := gopter.DefaultGenParameters().CloneWithSeed(20261113)
	params.MinSize = 2
	params.MaxSize = 2
	params.MaxShrinkCount = 30

	result := commands.Prop(regenExampleCommands{})(params)
	arg := result.Args[0]
	original := arg.OrigArg.(*commands.CommandPlan)
	final := arg.Arg.(*commands.CommandPlan)

	fmt.Println("original:", original.Commands())
	fmt.Println("shrunk:   ", final.Commands())

	// The reported plan can be replayed directly on a fresh system.
	replay := commands.Replay(&regenExampleSystem{}, final.InitialState(), final.Commands()...)
	fmt.Println("replay success:", replay.Success())
	// Output:
	// original: [open(8) use(8)]
	// shrunk:    [open(1) use(1)]
	// replay success: false
}
