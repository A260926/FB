// Copyright 2026 Gregory Petrosyan <gregory.petrosyan@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package rapid

import (
	"fmt"
)

// DiffOperation is a single cooperative, in-memory operation started on a [DiffBackend].
// Each Poll performs one synchronous step; Cancel is called at most once and only
// while the operation is still unfinished.
type DiffOperation interface {
	Poll() (done bool, value []byte)
	Cancel()
}

// DiffBackend is one local implementation under differential test. Backend methods
// are synchronous, deterministic, may mutate the backend's own state, and must not
// call into Rapid.
type DiffBackend interface {
	Ready(command string) bool
	Start(command string, input []byte) DiffOperation
	Close()
}

// DiffCommand names a legal API command and supplies the per-round input generator.
// Input is invoked exactly once for a generated command and never during replay.
type DiffCommand struct {
	Name  string
	Input func(*T) []byte
}

// DiffConfig configures a [*T.Differential] run or a [ReplayDifferential] run.
type DiffConfig struct {
	Backends  []func() DiffBackend
	Commands  []DiffCommand
	Rounds    int
	MaxPolls  int
	CancelAt  int
	Normalize func(backend int, value []byte) []byte
}

// DiffRound is one entry of a differential plan: the config command index, the
// input generated once for every backend, the ordered unfinished-backend poll
// choices as original backend indices, and the cancellation boundary, if any.
type DiffRound struct {
	Command     int
	Input       []byte
	Polls       []int
	CancelAfter int
}

// DiffEvent records one lifecycle step. Kind is one of "start", "poll", "done",
// or "cancel". Input is set (and copied) only for start events; Value is set
// (and copied) only for done events.
type DiffEvent struct {
	Round   int
	Command int
	Backend int
	Kind    string
	Input   []byte
	Value   []byte
}

// DiffReport is the fully owned result of a generated or replayed run.
type DiffReport struct {
	Status string
	Rounds int
	Plan   []DiffRound
	Events []DiffEvent
}

const (
	diffEqual     = "equal"
	diffInvalid   = "invalid"
	diffCancelled = "cancelled"
	diffMismatch  = "mismatch"

	diffStart  = "start"
	diffPoll   = "poll"
	diffDone   = "done"
	diffCancel = "cancel"
)

func cloneBytes(b []byte) []byte {
	if b == nil {
		return nil
	}
	return append([]byte(nil), b...)
}

func cloneRound(r DiffRound) DiffRound {
	return DiffRound{
		Command:     r.Command,
		Input:       cloneBytes(r.Input),
		Polls:       append([]int(nil), r.Polls...),
		CancelAfter: r.CancelAfter,
	}
}

func clonePlan(plan []DiffRound) []DiffRound {
	out := make([]DiffRound, len(plan))
	for i, r := range plan {
		out[i] = cloneRound(r)
	}
	return out
}

func cloneEvents(events []DiffEvent) []DiffEvent {
	out := make([]DiffEvent, len(events))
	for i, e := range events {
		out[i] = DiffEvent{
			Round:   e.Round,
			Command: e.Command,
			Backend: e.Backend,
			Kind:    e.Kind,
			Input:   cloneBytes(e.Input),
			Value:   cloneBytes(e.Value),
		}
	}
	return out
}

// validateDiffConfig checks everything that can be checked before a backend is
// constructed, so invalid configuration never constructs anything.
func validateDiffConfig(config DiffConfig) bool {
	if len(config.Backends) < 2 || config.Rounds < 0 || config.MaxPolls <= 0 || config.CancelAt < 0 {
		return false
	}
	for _, factory := range config.Backends {
		if factory == nil {
			return false
		}
	}
	if config.Rounds > 0 && len(config.Commands) == 0 {
		return false
	}
	names := make(map[string]struct{}, len(config.Commands))
	for _, cmd := range config.Commands {
		if cmd.Name == "" || cmd.Input == nil {
			return false
		}
		if _, ok := names[cmd.Name]; ok {
			return false
		}
		names[cmd.Name] = struct{}{}
	}
	return true
}

// Differential runs a cooperative differential check across freshly constructed
// local implementations. Command choices, parameters and poll steps are drawn from
// t's bitstream, making the whole schedule shrinkable. Mismatches fail the test via
// [*T.Fatalf]; unusable generated data is discarded with [*T.Skip].
func (t *T) Differential(config DiffConfig) DiffReport {
	t.Helper()

	if !validateDiffConfig(config) {
		t.Skip("invalid differential configuration")
	}

	report := runDiffGenerated(t, config)
	switch report.Status {
	case diffEqual, diffCancelled:
		return report
	case diffInvalid:
		t.Skip("differential run is inconclusive")
	default:
		t.Fatalf("differential mismatch after %d round(s): %s", report.Rounds, summarizeDiffReport(report))
	}
	return report
}

// ReplayDifferential re-executes an explicit plan against fresh backends without
// touching any Rapid bitstream or input callback, returning the report instead of
// calling testing methods.
func ReplayDifferential(config DiffConfig, plan []DiffRound) DiffReport {
	if !validateDiffConfig(config) {
		return DiffReport{Status: diffInvalid}
	}
	return runDiffReplay(config, plan)
}

func summarizeDiffReport(report DiffReport) string {
	return fmt.Sprintf("status=%s rounds=%d plan=%d events=%d", report.Status, report.Rounds, len(report.Plan), len(report.Events))
}

// diffRunner owns the constructed backends and the accumulated report pieces.
type diffRunner struct {
	config   DiffConfig
	backends []DiffBackend
	plan     []DiffRound
	events   []DiffEvent
	rounds   int
}

func newDiffRunner(config DiffConfig) *diffRunner {
	r := &diffRunner{config: config}
	r.backends = make([]DiffBackend, len(config.Backends))
	for i, factory := range config.Backends {
		r.backends[i] = factory()
	}
	return r
}

// closeBackends closes every constructed backend exactly once, in reverse order,
// including during panic unwinding. Clearing the slice makes a second cleanup a
// no-op.
func (r *diffRunner) closeBackends() {
	for i := len(r.backends) - 1; i >= 0; i-- {
		if r.backends[i] != nil {
			r.backends[i].Close()
		}
	}
	r.backends = nil
}

func (r *diffRunner) addEvent(round, command, backend int, kind string, input, value []byte) {
	r.events = append(r.events, DiffEvent{
		Round:   round,
		Command: command,
		Backend: backend,
		Kind:    kind,
		Input:   cloneBytes(input),
		Value:   cloneBytes(value),
	})
}

// readyCommands returns the config indices of commands every backend reports Ready
// for, in config.Commands order.
func (r *diffRunner) readyCommands() []int {
	var common []int
	for i, cmd := range r.config.Commands {
		ready := true
		for _, b := range r.backends {
			if !b.Ready(cmd.Name) {
				ready = false
				break
			}
		}
		if ready {
			common = append(common, i)
		}
	}
	return common
}

// normalize applies the configured normalization once per backend, in declared
// order, on an independent copy of the raw result.
func (r *diffRunner) normalize(backend int, value []byte) []byte {
	if r.config.Normalize == nil {
		return cloneBytes(value)
	}
	return cloneBytes(r.config.Normalize(backend, cloneBytes(value)))
}

// report assembles a fully owned snapshot of the run.
func (r *diffRunner) report(status string) DiffReport {
	return DiffReport{
		Status: status,
		Rounds: r.rounds,
		Plan:   clonePlan(r.plan),
		Events: cloneEvents(r.events),
	}
}
