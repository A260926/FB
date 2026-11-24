// diffdemo runs two honest in-memory implementations of the same echo API at
// different cooperative speeds through one explicit differential plan.
package main

import (
	"fmt"

	"pgregory.net/rapid"
)

type echoOp struct {
	remaining int
	value     []byte
	returned  []byte
}

func (o *echoOp) Poll() (bool, []byte) {
	o.remaining--
	if o.remaining > 0 {
		return false, nil
	}
	// Keep the returned backing array so the framework's copy can be checked.
	o.returned = append([]byte(nil), o.value...)
	return true, o.returned
}

func (*echoOp) Cancel() { panic("honest demo operation must not be canceled") }

type echoBackend struct {
	speed int
}

func (*echoBackend) Ready(string) bool { return true }

func (b *echoBackend) Start(_ string, input []byte) rapid.DiffOperation {
	return &echoOp{
		remaining: b.speed,
		value:     append([]byte(nil), input...),
	}
}

func (*echoBackend) Close() {}

func main() {
	cfg := rapid.DiffConfig{
		Backends: []func() rapid.DiffBackend{
			func() rapid.DiffBackend { return &echoBackend{speed: 1} },
			func() rapid.DiffBackend { return &echoBackend{speed: 2} },
		},
		Commands: []rapid.DiffCommand{{
			Name:  "echo",
			Input: func(*rapid.T) []byte { panic("replay must not generate parameters") },
		}},
		Rounds:   2,
		MaxPolls: 8,
	}

	// Backend 0 finishes in one poll, backend 1 needs two; both rounds use the
	// same schedule and the same generated-once input.
	plan := []rapid.DiffRound{
		{Command: 0, Input: []byte("hello"), Polls: []int{0, 1, 1}},
		{Command: 0, Input: []byte("world"), Polls: []int{0, 1, 1}},
	}

	report := rapid.ReplayDifferential(cfg, plan)
	if report.Status != "equal" || report.Rounds != 2 {
		panic(fmt.Sprintf("diffdemo unexpected report: status=%s rounds=%d", report.Status, report.Rounds))
	}

	fmt.Println("equal rounds=2")
}
