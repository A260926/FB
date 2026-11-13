package commands_test

import (
	"github.com/leanovate/gopter"
	"github.com/leanovate/gopter/commands"
	"testing"
)

func TestSeedReplay(t *testing.T) {
	sut := 0
	inc := &commands.ProtoCommand{
		Name:          "inc",
		RunFunc:       func(s commands.SystemUnderTest) commands.Result { p := s.(*int); *p++; return *p },
		NextStateFunc: func(s commands.State) commands.State { return s.(int) + 1 },
		PostConditionFunc: func(s commands.State, r commands.Result) *gopter.PropResult {
			if s.(int) != r.(int) {
				return &gopter.PropResult{Status: gopter.PropFalse}
			}
			return &gopter.PropResult{Status: gopter.PropTrue}
		},
	}
	if !commands.Replay(&sut, 0, inc, inc).Success() {
		t.Fatal("replay failed")
	}
	if sut != 2 {
		t.Fatal(sut)
	}
	t.Log("SEED_REPLAY two real commands completed")
}
