package commands

import (
	"fmt"
	"github.com/leanovate/gopter"
	"github.com/leanovate/gopter/gen"
	"testing"
)

type seedHandle struct {
	id   int
	open bool
}

func (h seedHandle) String() string {
	if h.open {
		return fmt.Sprintf("open(%d)", h.id)
	}
	return fmt.Sprintf("use(%d)", h.id)
}
func (h seedHandle) PreCondition(s State) bool {
	if h.open {
		return s.(int) == 0
	}
	return s.(int) == h.id
}
func (h seedHandle) NextState(s State) State {
	if h.open {
		return h.id
	}
	return s
}
func (h seedHandle) Run(s SystemUnderTest) Result {
	if h.open {
		*s.(*int) = h.id
		return h.id
	}
	return -1
}
func (h seedHandle) PostCondition(s State, r Result) *gopter.PropResult {
	return gopter.NewPropResult(s.(int) == r.(int), "local handle result")
}

func seedHandleCommands(created, destroyed *int) *ProtoCommands {
	return &ProtoCommands{
		InitialStateGen: gen.Const(0),
		GenCommandFunc: func(s State) gopter.Gen {
			if s.(int) == 0 {
				return gen.Const(Command(seedHandle{id: 8, open: true})).WithShrinker(func(v interface{}) gopter.Shrink {
					h := v.(seedHandle)
					done := false
					return func() (interface{}, bool) {
						if done || h.id <= 1 {
							return nil, false
						}
						done = true
						h.id /= 2
						return h, true
					}
				})
			}
			return gen.Const(Command(seedHandle{id: s.(int)}))
		},
		NewSystemUnderTestFunc:     func(State) SystemUnderTest { *created++; return new(int) },
		DestroySystemUnderTestFunc: func(SystemUnderTest) { *destroyed++ },
	}
}

// Starting-point witness: dependency-breaking candidates are filtered already;
// the missing capability is repairing the surviving use command after open shrinks.
func TestSeedStatefulBaseline(t *testing.T) {
	created, destroyed := 0, 0
	c := seedHandleCommands(&created, &destroyed)
	p := gopter.DefaultGenParameters().CloneWithSeed(1641)
	p.MaxSize = 2
	p.MaxShrinkCount = 30
	v, ok := genActions(c)(p.CloneWithSeed(1641)).Retrieve()
	if !ok {
		t.Fatal("generation exhausted")
	}
	a := v.(*actions)
	if len(a.sequentialCommands) != 2 {
		t.Fatal(a)
	}
	res := Prop(c)(p)
	if res.Success() || len(res.Args) != 1 {
		t.Fatalf("expected local failure: %+v", res)
	}
	final := res.Args[0].Arg.(*actions)
	state := final.initialStateProvider()
	valid := true
	for _, s := range final.sequentialCommands {
		if !s.command.PreCondition(state) {
			valid = false
			break
		}
		state = s.command.NextState(state)
	}
	if created == 0 || created != destroyed || !valid {
		t.Fatal("bad lifecycle or invalid final plan")
	}
	fmt.Printf("SEED_STATEFUL generated=%v final=%v valid=%t created=%d destroyed=%d\n", a, final, valid, created, destroyed)
}
