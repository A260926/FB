// Differential state machine contract tests. All assertions go through the public
// entry points and real Rapid draws; taskcheck records every content assertion.
package rapid

import (
	"reflect"
	"testing"

	"pgregory.net/rapid/task_support/taskcheck"
)

// memOp is a deterministic cooperative operation that needs pollsLeft Poll calls.
type memOp struct {
	pollsLeft int
	value     []byte
	done      bool
	canceled  bool
	panicPoll bool
	cancels   *int
}

func (o *memOp) Poll() (bool, []byte) {
	if o.done || o.canceled {
		panic("Poll called on a finished operation")
	}
	if o.panicPoll {
		panic("synchronous backend failure")
	}
	o.pollsLeft--
	if o.pollsLeft > 0 {
		return false, nil
	}
	o.done = true
	return true, append([]byte(nil), o.value...)
}

func (o *memOp) Cancel() {
	if o.done {
		panic("Cancel called on a completed operation")
	}
	o.canceled = true
	if o.cancels != nil {
		*o.cancels++
	}
}

type memBackend struct {
	id        int
	ready     map[string]bool // nil means every command is ready
	speed     map[string]int
	value     func(command string, input []byte) []byte
	starts    *int
	cancels   *int
	closeSeq  *[]int
	lastOp    *memOp
	panicPoll bool
}

func (b *memBackend) Ready(name string) bool {
	if b.ready == nil {
		return true
	}
	return b.ready[name]
}

func (b *memBackend) Start(name string, input []byte) DiffOperation {
	*b.starts++
	speed := b.speed[name]
	if speed == 0 {
		speed = 1
	}
	var value []byte
	if b.value != nil {
		value = b.value(name, input)
	} else {
		value = append([]byte(nil), input...)
	}
	op := &memOp{pollsLeft: speed, value: value, panicPoll: b.panicPoll, cancels: b.cancels}
	b.lastOp = op
	return op
}

func (b *memBackend) Close() {
	*b.closeSeq = append(*b.closeSeq, b.id)
}

// memEnv is a fresh set of backends and counters for one property iteration.
type memEnv struct {
	factories []func() DiffBackend
	starts    int
	cancels   int
	closeSeq  []int
	live      []*memBackend
}

func newMemEnv(n int, speeds []int, value func(int, string, []byte) []byte) *memEnv {
	env := &memEnv{}
	env.factories = make([]func() DiffBackend, n)
	for i := 0; i < n; i++ {
		i := i
		env.factories[i] = func() DiffBackend {
			b := &memBackend{
				id:       i,
				speed:    map[string]int{"echo": speeds[i], "shout": speeds[i]},
				starts:   &env.starts,
				cancels:  &env.cancels,
				closeSeq: &env.closeSeq,
			}
			if value != nil {
				b.value = func(command string, input []byte) []byte { return value(i, command, input) }
			}
			env.live = append(env.live, b)
			return b
		}
	}
	return env
}

func countKinds(events []DiffEvent) map[string]int {
	out := map[string]int{}
	for _, e := range events {
		out[e.Kind]++
	}
	return out
}

func eventsOfRound(events []DiffEvent, round int) []DiffEvent {
	var out []DiffEvent
	for _, e := range events {
		if e.Round == round {
			out = append(out, e)
		}
	}
	return out
}

// TestDiffSchedule drives the generated public entry through real Draw calls and
// checks ordering, barrier semantics, ownership and stable backend indices.
func TestDiffSchedule(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		Check(t, func(rt *T) {
			env := newMemEnv(2, []int{1, 3}, nil) // backend 0 fast, backend 1 slow
			inputCalls := 0
			shared := []byte("payload")
			cfg := DiffConfig{
				Backends: env.factories,
				Commands: []DiffCommand{{
					Name: "echo",
					Input: func(*T) []byte {
						inputCalls++
						return shared
					},
				}},
				Rounds:   2,
				MaxPolls: 20,
			}

			report := rt.Differential(cfg)
			c.Check(report.Status == "equal", "generated honest run status: %q", report.Status)
			c.Check(report.Rounds == 2, "completed rounds: %d", report.Rounds)
			c.Check(len(report.Plan) == 2, "plan entries: %d", len(report.Plan))
			c.Check(inputCalls == 2, "Input generated once per round, got %d", inputCalls)
			c.Check(env.starts == 4, "two starts per round, got %d", env.starts)

			for r, entry := range report.Plan {
				c.Check(entry.Command == 0, "round %d command index", r)
				c.Check(entry.CancelAfter == 0, "no cancellation configured, got %d", entry.CancelAfter)
				c.Check(string(entry.Input) == "payload", "round %d owned input", r)
				for _, backend := range entry.Polls {
					c.Check(backend >= 0 && backend < 2, "poll backend index in range: %d", backend)
				}
			}

			for r := 0; r < 2; r++ {
				ev := eventsOfRound(report.Events, r)
				kinds := countKinds(ev)
				c.Check(kinds["start"] == 2, "round %d starts: %d", r, kinds["start"])
				c.Check(kinds["done"] == 2, "round %d dones: %d", r, kinds["done"])
				c.Check(kinds["cancel"] == 0, "round %d cancels: %d", r, kinds["cancel"])
				c.Check(kinds["poll"]+kinds["done"] == 4, "round %d total polls: %d", r, kinds["poll"]+kinds["done"])

				doneBackends := map[int]bool{}
				polledAfterDone := map[int]bool{}
				for _, e := range ev {
					switch e.Kind {
					case "start":
						c.Check(string(e.Input) == "payload", "start event input copy")
						c.Check(e.Value == nil, "start event has no value")
					case "poll":
						c.Check(e.Input == nil && e.Value == nil, "poll event carries no payload")
						if doneBackends[e.Backend] {
							polledAfterDone[e.Backend] = true
						}
					case "done":
						c.Check(e.Input == nil, "done event carries no input")
						c.Check(string(e.Value) == "payload", "done event owned raw value")
						doneBackends[e.Backend] = true
					}
				}
				c.Check(len(doneBackends) == 2, "each backend done once: %v", doneBackends)
				c.Check(len(polledAfterDone) == 0, "completed backend polled again: %v", polledAfterDone)
			}

			// Backend indices stay stable as the active set shrinks.
			for _, e := range report.Events {
				c.Check(e.Backend == 0 || e.Backend == 1, "stable backend index: %d", e.Backend)
			}

			// Reported bytes must stay detached from caller- and callback-owned buffers.
			original := append([]byte(nil), shared...)
			shared[0] = 'X'
			c.Check(string(report.Plan[0].Input) == string(original), "plan input detached from caller buffer")
			for _, e := range report.Events {
				if e.Kind == "start" {
					c.Check(string(e.Input) == string(original), "event input detached from caller buffer")
				}
			}
			for _, b := range env.live {
				if b.lastOp != nil {
					b.lastOp.value[0] = 'Y'
				}
			}
			for _, e := range report.Events {
				if e.Kind == "done" {
					c.Check(string(e.Value) == string(original), "done value detached from operation buffer")
				}
			}
			c.Check(reflect.DeepEqual(env.closeSeq, []int{1, 0}), "backends closed once in reverse order: %v", env.closeSeq)
		})

		// Zero rounds still construct and close the backends and report equal.
		Check(t, func(rt *T) {
			env := newMemEnv(2, []int{1, 1}, nil)
			cfg := DiffConfig{Backends: env.factories, MaxPolls: 4}
			rep := rt.Differential(cfg)
			c.Check(rep.Status == "equal" && rep.Rounds == 0, "zero-round status: %q rounds=%d", rep.Status, rep.Rounds)
			c.Check(len(rep.Plan) == 0 && len(rep.Events) == 0, "zero-round empty plan/events")
			c.Check(env.starts == 0, "zero-round starts")
			c.Check(reflect.DeepEqual(env.closeSeq, []int{1, 0}), "zero-round close order: %v", env.closeSeq)
		})

		// Normalization makes differing raw representations compare equal.
		Check(t, func(rt *T) {
			env := newMemEnv(2, []int{1, 1}, func(backend int, _ string, input []byte) []byte {
				if backend == 1 {
					out := append([]byte(nil), input...)
					for i := range out {
						if out[i] >= 'a' && out[i] <= 'z' {
							out[i] -= 'a' - 'A'
						}
					}
					return out
				}
				return append([]byte(nil), input...)
			})
			cfg := DiffConfig{
				Backends: env.factories,
				Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return []byte("MiXeD") }}},
				Rounds:   1,
				MaxPolls: 4,
				Normalize: func(_ int, value []byte) []byte {
					out := append([]byte(nil), value...)
					for i := range out {
						if out[i] >= 'A' && out[i] <= 'Z' {
							out[i] += 'a' - 'A'
						}
					}
					return out
				},
			}
			rep := rt.Differential(cfg)
			c.Check(rep.Status == "equal" && rep.Rounds == 1, "normalized equal run: %q", rep.Status)
			c.Check(reflect.DeepEqual(env.closeSeq, []int{1, 0}), "normalized run close order: %v", env.closeSeq)
		})
	})
}

// TestDiffBarriers covers completion barriers, mismatches, cancellation boundaries
// and budget exhaustion, using explicit plans plus generated cancellation runs.
func TestDiffBarriers(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Different completion order, identical results: slow backend 1 finishes first.
		env := newMemEnv(2, []int{1, 3}, nil)
		cfg := DiffConfig{
			Backends: env.factories,
			Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return nil }}},
			Rounds:   1,
			MaxPolls: 10,
		}
		rep := ReplayDifferential(cfg, []DiffRound{{
			Command: 0, Input: []byte("abc"), Polls: []int{1, 1, 1, 0},
		}})
		c.Check(rep.Status == "equal" && rep.Rounds == 1, "interleaved equal run: %q rounds=%d", rep.Status, rep.Rounds)
		doneOrder := []int{}
		for _, e := range rep.Events {
			if e.Kind == "done" {
				doneOrder = append(doneOrder, e.Backend)
			}
		}
		c.Check(reflect.DeepEqual(doneOrder, []int{1, 0}), "completion order differs: %v", doneOrder)
		c.Check(env.starts == 2, "both implementations started: %d", env.starts)
		c.Check(reflect.DeepEqual(env.closeSeq, []int{1, 0}), "reverse close order: %v", env.closeSeq)

		// First content difference: mismatch stops the trial and excludes the round.
		mmEnv := newMemEnv(2, []int{1, 1}, func(backend int, _ string, input []byte) []byte {
			if backend == 1 {
				return append(append([]byte(nil), input...), '!')
			}
			return append([]byte(nil), input...)
		})
		mmCfg := DiffConfig{
			Backends: mmEnv.factories,
			Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return nil }}},
			Rounds:   3,
			MaxPolls: 4,
		}
		mmRep := ReplayDifferential(mmCfg, []DiffRound{{
			Command: 0, Input: []byte("abc"), Polls: []int{0, 1},
		}})
		c.Check(mmRep.Status == "mismatch", "mismatch status: %q", mmRep.Status)
		c.Check(mmRep.Rounds == 0, "mismatched round not counted: %d", mmRep.Rounds)
		c.Check(len(mmRep.Plan) == 1, "mismatch plan prefix: %d", len(mmRep.Plan))
		c.Check(countKinds(mmRep.Events)["cancel"] == 0, "mismatch does not cancel: %v", countKinds(mmRep.Events))
		c.Check(mmEnv.starts == 2 && len(mmEnv.closeSeq) == 2, "mismatch lifecycle starts=%d closes=%d", mmEnv.starts, len(mmEnv.closeSeq))

		// The same divergence through the generated entry fails via Fatalf (stopTest),
		// which is what lets Check/MakeFuzz observe it. A standalone T keeps this out
		// of the parent test's failure state.
		fatalEnv := newMemEnv(2, []int{1, 1}, func(backend int, _ string, input []byte) []byte {
			if backend == 1 {
				return append(append([]byte(nil), input...), '!')
			}
			return append([]byte(nil), input...)
		})
		fatalCfg := DiffConfig{
			Backends: fatalEnv.factories,
			Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return []byte("abc") }}},
			Rounds:   2,
			MaxPolls: 4,
		}
		func() {
			defer func() {
				_, ok := recover().(stopTest)
				c.Check(ok, "generated mismatch fails via Fatalf/stopTest")
			}()
			standalone := newT(nilTB{}, newRandomBitStream(791114, false), false, nil)
			standalone.Differential(fatalCfg)
			c.Check(false, "diverging generated run must not return normally")
		}()
		c.Check(reflect.DeepEqual(fatalEnv.closeSeq, []int{1, 0}), "mismatch still closes backends in reverse order")

		// Generated run with cancellation interleaved with completion: CancelAt=1
		// always lands after the first Poll; the polled backend may already be done.
		Check(t, func(rt *T) {
			genEnv := newMemEnv(2, []int{1, 2}, nil)
			genCfg := DiffConfig{
				Backends: genEnv.factories,
				Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return []byte("x") }}},
				Rounds:   2,
				MaxPolls: 10,
				CancelAt: 1,
			}
			cr := rt.Differential(genCfg)
			c.Check(cr.Status == "cancelled", "generated cancel status: %q", cr.Status)
			c.Check(cr.Rounds == 0, "cancelled round not counted: %d", cr.Rounds)
			c.Check(len(cr.Plan) == 1 && cr.Plan[0].CancelAfter == 1, "recorded cancel boundary: %+v", cr.Plan)

			ev := eventsOfRound(cr.Events, 0)
			kinds := countKinds(ev)
			c.Check(kinds["start"] == 2, "cancel round starts: %d", kinds["start"])
			c.Check(kinds["poll"]+kinds["done"] == 1, "exactly one poll before the boundary")
			c.Check(kinds["cancel"] >= 1, "at least one unfinished operation canceled")
			finished := map[int]bool{}
			canceled := map[int]bool{}
			for _, e := range ev {
				switch e.Kind {
				case "done":
					finished[e.Backend] = true
				case "cancel":
					canceled[e.Backend] = true
					c.Check(e.Input == nil && e.Value == nil, "cancel event payload")
				}
			}
			for b := range canceled {
				c.Check(!finished[b], "completed backend %d must not be canceled", b)
			}
			c.Check(len(finished)+len(canceled) == 2, "every backend reaches exactly one terminal state")
			c.Check(genEnv.starts == 2, "cancelled trial starts once each: %d", genEnv.starts)
			c.Check(reflect.DeepEqual(genEnv.closeSeq, []int{1, 0}), "cancelled trial closes all backends: %v", genEnv.closeSeq)
		})

		// Cancellation boundary that completes the last operation still wins over
		// comparison, and no completed operation receives a Cancel call.
		boundaryEnv := newMemEnv(2, []int{1, 1}, nil)
		boundaryCfg := DiffConfig{
			Backends: boundaryEnv.factories,
			Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return nil }}},
			Rounds:   1,
			MaxPolls: 4,
		}
		bRep := ReplayDifferential(boundaryCfg, []DiffRound{{
			Command: 0, Input: []byte("z"), Polls: []int{0, 1}, CancelAfter: 2,
		}})
		c.Check(bRep.Status == "cancelled", "boundary precedence status: %q", bRep.Status)
		c.Check(bRep.Plan[0].CancelAfter == 2, "replayed boundary recorded: %d", bRep.Plan[0].CancelAfter)
		c.Check(countKinds(bRep.Events)["cancel"] == 0, "nothing unfinished to cancel: %v", countKinds(bRep.Events))
		c.Check(len(boundaryEnv.closeSeq) == 2, "boundary run closes backends")

		// MaxPolls exhaustion is inconclusive: pending operations get one Cancel.
		budgetEnv := newMemEnv(2, []int{5, 5}, nil)
		budgetCfg := DiffConfig{
			Backends: budgetEnv.factories,
			Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return nil }}},
			Rounds:   1,
			MaxPolls: 2,
		}
		budgetRep := ReplayDifferential(budgetCfg, []DiffRound{{
			Command: 0, Input: []byte("q"), Polls: []int{0, 1},
		}})
		c.Check(budgetRep.Status == "invalid", "budget exhaustion status: %q", budgetRep.Status)
		c.Check(budgetRep.Rounds == 0 && budgetRep.Plan[0].CancelAfter == 0, "budget round inconclusive")
		c.Check(countKinds(budgetRep.Events)["cancel"] == 2, "both pending operations canceled once")
		c.Check(budgetEnv.starts == 2 && len(budgetEnv.closeSeq) == 2, "budget lifecycle")

		// The same exhaustion reached through generated Draw calls is discarded via
		// Skip while still canceling every pending operation exactly once.
		Check(t, func(rt *T) {
			genBudgetEnv := newMemEnv(2, []int{5, 5}, nil)
			genBudgetCfg := DiffConfig{
				Backends: genBudgetEnv.factories,
				Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return []byte("slow") }}},
				Rounds:   1,
				MaxPolls: 2,
			}
			func() {
				defer func() {
					_, ok := recover().(invalidData)
					c.Check(ok, "generated budget exhaustion is discarded as invalid data")
				}()
				rt.Differential(genBudgetCfg)
				c.Check(false, "budget-exhausted run must not return normally")
			}()
			c.Check(genBudgetEnv.cancels == 2, "both pending ops canceled once on generated exhaustion: %d", genBudgetEnv.cancels)
			c.Check(genBudgetEnv.starts == 2, "generated budget run starts both: %d", genBudgetEnv.starts)
			c.Check(reflect.DeepEqual(genBudgetEnv.closeSeq, []int{1, 0}), "generated budget run closes backends")
		})

		// A configured CancelAt beyond the actual completion point records zero.
		Check(t, func(rt *T) {
			lateEnv := newMemEnv(2, []int{1, 1}, nil)
			lateCfg := DiffConfig{
				Backends: lateEnv.factories,
				Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return []byte("late") }}},
				Rounds:   1,
				MaxPolls: 4,
				CancelAt: 10,
			}
			lr := rt.Differential(lateCfg)
			c.Check(lr.Status == "equal", "late cancel boundary never reached: %q", lr.Status)
			c.Check(lr.Plan[0].CancelAfter == 0, "unreached boundary records zero: %d", lr.Plan[0].CancelAfter)
			c.Check(reflect.DeepEqual(lateEnv.closeSeq, []int{1, 0}), "late-boundary run closes backends")
		})

		// A generated round with no common-ready command is discarded via Skip:
		// recover the invalidData panic so the parent test keeps running.
		Check(t, func(rt *T) {
			nrStarts := 0
			nrClose := []int{}
			blocked := func(id int) func() DiffBackend {
				return func() DiffBackend {
					// An empty readiness map rejects every command.
					return &memBackend{
						id:       id,
						ready:    map[string]bool{},
						speed:    map[string]int{"echo": 1},
						starts:   &nrStarts,
						closeSeq: &nrClose,
					}
				}
			}
			noReadyCfg := DiffConfig{
				Backends: []func() DiffBackend{blocked(0), blocked(1)},
				Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return []byte("none") }}},
				Rounds:   1,
				MaxPolls: 4,
			}
			func() {
				defer func() {
					_, ok := recover().(invalidData)
					c.Check(ok, "no-common-command generated run is discarded as invalid data")
				}()
				rt.Differential(noReadyCfg)
				c.Check(false, "no-common-command run must not return normally")
			}()
			c.Check(nrStarts == 0, "no-common-command starts nothing: %d", nrStarts)
			c.Check(reflect.DeepEqual(nrClose, []int{1, 0}), "no-common-command still closes backends")
		})

		// A backend panic still unwinds through the reverse-order cleanup.
		panicStarts := 0
		panicClose := []int{}
		panicCfg := DiffConfig{
			Backends: []func() DiffBackend{
				func() DiffBackend {
					return &memBackend{id: 0, speed: map[string]int{"echo": 1}, starts: &panicStarts, closeSeq: &panicClose, panicPoll: true}
				},
				func() DiffBackend {
					return &memBackend{id: 1, speed: map[string]int{"echo": 1}, starts: &panicStarts, closeSeq: &panicClose}
				},
			},
			Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return nil }}},
			Rounds:   1,
			MaxPolls: 4,
		}
		func() {
			defer func() {
				c.Check(recover() != nil, "backend panic propagates")
				c.Check(reflect.DeepEqual(panicClose, []int{1, 0}), "panic cleanup reverse order: %v", panicClose)
			}()
			ReplayDifferential(panicCfg, []DiffRound{{
				Command: 0, Input: []byte("p"), Polls: []int{0},
			}})
			c.Check(false, "panicking backend must not return normally")
		}()
	})
}

// TestDiffReplay exercises illegal plans, full generated-plan round trips,
// repeated deterministic replay, buffer ownership and the MakeFuzz entry point.
func TestDiffReplay(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		baseEnv := newMemEnv(2, []int{1, 2}, nil)
		baseCfg := DiffConfig{
			Backends: baseEnv.factories,
			Commands: []DiffCommand{
				{Name: "echo", Input: func(*T) []byte { return nil }},
				{Name: "shout", Input: func(*T) []byte { return nil }},
			},
			Rounds:   2,
			MaxPolls: 8,
		}
		goodRound := DiffRound{Command: 0, Input: []byte("go"), Polls: []int{0, 1, 1}}

		illegal := map[string]DiffRound{
			"command index too large":   {Command: 7, Input: []byte("go"), Polls: []int{0}},
			"negative command index":    {Command: -1, Input: []byte("go"), Polls: []int{0}},
			"negative cancel boundary":  {Command: 0, Input: []byte("go"), Polls: []int{0}, CancelAfter: -1},
			"boundary beyond polls":     {Command: 0, Input: []byte("go"), Polls: []int{0}, CancelAfter: 2},
			"poll backend out of range": {Command: 0, Input: []byte("go"), Polls: []int{2}},
			"poll of completed backend": {Command: 0, Input: []byte("go"), Polls: []int{0, 0}},
			"insufficient poll choices": {Command: 0, Input: []byte("go"), Polls: []int{0}},
			"polls after completion":    {Command: 0, Input: []byte("go"), Polls: []int{0, 1, 1, 1}},
			"polls over budget":         {Command: 0, Input: []byte("go"), Polls: []int{0, 1, 1, 0, 1, 1, 0, 1, 1}},
		}
		for name, entry := range illegal {
			rep := ReplayDifferential(baseCfg, []DiffRound{entry})
			c.Check(rep.Status == "invalid", "%s: status %q", name, rep.Status)
			c.Check(rep.Rounds == 0, "%s: no round counted", name)
		}
		c.Check(len(baseEnv.closeSeq) == 2*len(illegal), "every illegal replay still closed its backends: %v", baseEnv.closeSeq)

		// Polls after the cancellation boundary are invalid even if indices are fine.
		rep := ReplayDifferential(baseCfg, []DiffRound{{
			Command: 0, Input: []byte("go"), Polls: []int{0, 1}, CancelAfter: 1,
		}})
		c.Check(rep.Status == "invalid", "polls after cancel boundary: %q", rep.Status)

		// A command outside the common Ready set never starts anything.
		notReadyStarts := 0
		notReadyClose := []int{}
		notReadyCfg := DiffConfig{
			Backends: []func() DiffBackend{
				func() DiffBackend {
					return &memBackend{id: 0, speed: map[string]int{"echo": 1}, starts: &notReadyStarts, closeSeq: &notReadyClose}
				},
				func() DiffBackend {
					return &memBackend{id: 1, speed: map[string]int{"echo": 1}, ready: map[string]bool{"other": true}, starts: &notReadyStarts, closeSeq: &notReadyClose}
				},
			},
			Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return nil }}},
			Rounds:   1,
			MaxPolls: 4,
		}
		nrRep := ReplayDifferential(notReadyCfg, []DiffRound{{Command: 0, Input: []byte("go"), Polls: []int{0, 1}}})
		c.Check(nrRep.Status == "invalid", "not common-ready: %q", nrRep.Status)
		c.Check(notReadyStarts == 0, "no starts before readiness check: %d", notReadyStarts)
		c.Check(len(notReadyClose) == 2, "not-ready run still closes backends")

		// Plan length must equal Rounds when every round is equal.
		shortRep := ReplayDifferential(baseCfg, []DiffRound{goodRound})
		c.Check(shortRep.Status == "invalid", "short equal plan: %q", shortRep.Status)
		c.Check(shortRep.Rounds == 1, "completed prefix still counted: %d", shortRep.Rounds)
		emptyRep := ReplayDifferential(baseCfg, nil)
		c.Check(emptyRep.Status == "invalid" && emptyRep.Rounds == 0, "empty plan with rounds: %q", emptyRep.Status)

		// Over-long plans execute only the configured prefix; the length mismatch is
		// invalid unless an earlier round ends the run with a non-equal result.
		overCfg := baseCfg
		overCfg.Rounds = 1
		overRep := ReplayDifferential(overCfg, []DiffRound{goodRound, goodRound})
		c.Check(overRep.Status == "invalid", "over-long equal plan: %q", overRep.Status)
		c.Check(overRep.Rounds == 1 && len(overRep.Plan) == 1, "over-long executed only configured rounds")

		overMmEnv := newMemEnv(2, []int{1, 1}, func(backend int, _ string, input []byte) []byte {
			if backend == 1 {
				return append(append([]byte(nil), input...), '!')
			}
			return append([]byte(nil), input...)
		})
		overMmCfg := DiffConfig{
			Backends: overMmEnv.factories,
			Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return nil }}},
			Rounds:   1,
			MaxPolls: 4,
		}
		mmRound := DiffRound{Command: 0, Input: []byte("abc"), Polls: []int{0, 1}}
		overMm := ReplayDifferential(overMmCfg, []DiffRound{mmRound, mmRound})
		c.Check(overMm.Status == "mismatch", "mismatch takes precedence over plan length: %q", overMm.Status)
		c.Check(overMm.Rounds == 0 && overMmEnv.starts == 2, "later plan entry never started: rounds=%d starts=%d", overMm.Rounds, overMmEnv.starts)

		zeroCfg := baseCfg
		zeroCfg.Rounds = 0
		c.Check(ReplayDifferential(zeroCfg, nil).Status == "equal", "zero-round replay")
		c.Check(ReplayDifferential(zeroCfg, []DiffRound{goodRound}).Status == "invalid", "zero-round rejects entries")

		// Bad configuration is invalid without constructing any backend.
		constructed := 0
		guarded := func() DiffBackend {
			constructed++
			return &memBackend{id: 0, starts: new(int), closeSeq: new([]int)}
		}
		badConfigs := []DiffConfig{
			{Backends: []func() DiffBackend{guarded}, MaxPolls: 1, Rounds: 0},
			{Backends: []func() DiffBackend{guarded, guarded}, MaxPolls: 0, Rounds: 1, Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return nil }}}},
			{Backends: []func() DiffBackend{guarded, guarded}, MaxPolls: 1, Rounds: -1},
			{Backends: []func() DiffBackend{guarded, guarded}, MaxPolls: 1, Rounds: 1},
			{Backends: []func() DiffBackend{guarded, guarded}, MaxPolls: 1, Rounds: 1, Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return nil }}, {Name: "echo", Input: func(*T) []byte { return nil }}}},
			{Backends: []func() DiffBackend{guarded, guarded}, MaxPolls: 1, Rounds: 1, Commands: []DiffCommand{{Name: "", Input: func(*T) []byte { return nil }}}},
			{Backends: []func() DiffBackend{guarded, guarded}, MaxPolls: 1, Rounds: 1, Commands: []DiffCommand{{Name: "echo"}}},
			{Backends: []func() DiffBackend{guarded, nil}, MaxPolls: 1, Rounds: 0},
			{Backends: []func() DiffBackend{guarded, guarded}, MaxPolls: 1, Rounds: 1, CancelAt: -1, Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return nil }}}},
		}
		for i, bad := range badConfigs {
			c.Check(ReplayDifferential(bad, nil).Status == "invalid", "bad config %d", i)
		}
		c.Check(constructed == 0, "invalid config constructs no backends: %d", constructed)

		// Full trajectory round trip: a generated plan reproduces every event.
		Check(t, func(rt *T) {
			rtEnv := newMemEnv(2, []int{1, 2}, nil)
			rtCfg := DiffConfig{
				Backends: rtEnv.factories,
				Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return []byte("trip") }}},
				Rounds:   2,
				MaxPolls: 10,
			}
			gen := rt.Differential(rtCfg)
			c.Check(gen.Status == "equal", "round-trip generated status: %q", gen.Status)

			replayed := ReplayDifferential(rtCfg, gen.Plan)
			c.Check(replayed.Status == gen.Status, "replay status %q vs %q", replayed.Status, gen.Status)
			c.Check(replayed.Rounds == gen.Rounds, "replay rounds %d vs %d", replayed.Rounds, gen.Rounds)
			c.Check(reflect.DeepEqual(replayed.Plan, gen.Plan), "replayed plan matches generated plan")
			c.Check(reflect.DeepEqual(replayed.Events, gen.Events), "replayed events match generated events")

			again := ReplayDifferential(rtCfg, gen.Plan)
			c.Check(reflect.DeepEqual(again, replayed), "same input replays to the same full trajectory")
			// One generated run plus two replays, each closing its own backends once.
			c.Check(reflect.DeepEqual(rtEnv.closeSeq, []int{1, 0, 1, 0, 1, 0}), "every run closes its backends in reverse order: %v", rtEnv.closeSeq)
		})

		// Cancellation round trip as well: the plan's CancelAfter drives replay.
		Check(t, func(rt *T) {
			cancelEnv := newMemEnv(2, []int{1, 2}, nil)
			cancelCfg := DiffConfig{
				Backends: cancelEnv.factories,
				Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return []byte("c") }}},
				Rounds:   1,
				MaxPolls: 6,
				CancelAt: 1,
			}
			gen := rt.Differential(cancelCfg)
			c.Check(gen.Status == "cancelled", "generated cancel: %q", gen.Status)
			replayed := ReplayDifferential(cancelCfg, gen.Plan)
			c.Check(replayed.Status == "cancelled", "replayed cancel: %q", replayed.Status)
			c.Check(reflect.DeepEqual(replayed.Events, gen.Events), "cancel event trajectories match")
			c.Check(reflect.DeepEqual(cancelEnv.closeSeq, []int{1, 0, 1, 0}), "cancel round trip closes both runs' backends: %v", cancelEnv.closeSeq)
		})

		// Replay owns its copies: mutating caller buffers cannot change the report.
		ownEnv := newMemEnv(2, []int{1, 1}, nil)
		ownCfg := DiffConfig{
			Backends: ownEnv.factories,
			Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return nil }}},
			Rounds:   1,
			MaxPolls: 4,
		}
		callerPlan := []DiffRound{{Command: 0, Input: []byte("own"), Polls: []int{0, 1}}}
		ownRep := ReplayDifferential(ownCfg, callerPlan)
		c.Check(ownRep.Status == "equal", "ownership baseline: %q", ownRep.Status)
		callerPlan[0].Input[0] = 'E'
		callerPlan[0].Polls[0] = 1
		c.Check(string(ownRep.Plan[0].Input) == "own", "report plan input is owned")
		c.Check(reflect.DeepEqual(ownRep.Plan[0].Polls, []int{0, 1}), "report plan polls are owned")

		// MakeFuzz with a fixed finite byte input still draws and replays for real.
		MakeFuzz(func(rt *T) {
			fuzzEnv := newMemEnv(2, []int{1, 1}, nil)
			fuzzCfg := DiffConfig{
				Backends: fuzzEnv.factories,
				Commands: []DiffCommand{{Name: "echo", Input: func(*T) []byte { return []byte("fuzz") }}},
				Rounds:   1,
				MaxPolls: 4,
			}
			gen := rt.Differential(fuzzCfg)
			c.Check(gen.Status == "equal", "MakeFuzz generated run: %q", gen.Status)
			replayed := ReplayDifferential(fuzzCfg, gen.Plan)
			c.Check(reflect.DeepEqual(replayed.Events, gen.Events), "MakeFuzz replay trajectory")
			c.Check(replayed.Rounds == gen.Rounds, "MakeFuzz replay rounds")
			c.Check(reflect.DeepEqual(fuzzEnv.closeSeq, []int{1, 0, 1, 0}), "MakeFuzz runs close their backends: %v", fuzzEnv.closeSeq)
		})(t, makeFixedBytes(1024))
	})
}

func makeFixedBytes(n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte(i*7 + 3)
	}
	return b
}
