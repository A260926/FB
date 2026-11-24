// Copyright 2026 Gregory Petrosyan <gregory.petrosyan@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package rapid

import "bytes"

// diffOp tracks one started operation through the cooperative poll schedule.
// Backend indices stay fixed for the whole run, even as the unfinished set shrinks.
type diffOp struct {
	backend int
	op      DiffOperation
	done    bool
	value   []byte
}

// diffRoundExec holds the live state of the round currently being executed.
type diffRoundExec struct {
	command   int
	input     []byte
	polls     []int
	ops       []*diffOp
	pollCount int
}

func unfinishedIndices(ops []*diffOp) []int {
	var out []int
	for _, o := range ops {
		if !o.done {
			out = append(out, o.backend)
		}
	}
	return out
}

func (r *diffRunner) startRound(round, command int, input []byte) *diffRoundExec {
	ops := make([]*diffOp, len(r.backends))
	for i, b := range r.backends {
		r.addEvent(round, command, i, diffStart, input, nil)
		// Every backend gets its own copy, detached from the plan and from peers.
		ops[i] = &diffOp{
			backend: i,
			op:      b.Start(r.config.Commands[command].Name, cloneBytes(input)),
		}
	}
	return &diffRoundExec{
		command: command,
		input:   cloneBytes(input),
		ops:     ops,
	}
}

// cancelUnfinished cancels every still-unfinished operation once, in backend order,
// recording one cancel event per operation.
func (r *diffRunner) cancelUnfinished(round int, exec *diffRoundExec) {
	for _, o := range exec.ops {
		if !o.done {
			r.addEvent(round, exec.command, o.backend, diffCancel, nil, nil)
			o.op.Cancel()
		}
	}
}

// pollOne executes one cooperative Poll on the chosen original backend index and
// records a poll or done event. It never touches a completed operation.
func (r *diffRunner) pollOne(round int, exec *diffRoundExec, backend int) {
	o := exec.ops[backend]
	exec.pollCount++
	exec.polls = append(exec.polls, backend)

	done, value := o.op.Poll()
	if done {
		o.done = true
		o.value = cloneBytes(value)
		r.addEvent(round, exec.command, backend, diffDone, nil, o.value)
	} else {
		r.addEvent(round, exec.command, backend, diffPoll, nil, nil)
	}
}

func allDone(ops []*diffOp) bool {
	for _, o := range ops {
		if !o.done {
			return false
		}
	}
	return true
}

// compareBarrier normalizes each backend's raw result once, in declared order, and
// byte-compares the owned normalized copies. Comparison happens only after every
// backend has completed.
func (r *diffRunner) compareBarrier(exec *diffRoundExec) bool {
	first := r.normalize(exec.ops[0].backend, exec.ops[0].value)
	for i := 1; i < len(exec.ops); i++ {
		if !bytes.Equal(first, r.normalize(exec.ops[i].backend, exec.ops[i].value)) {
			return false
		}
	}
	return true
}

func (r *diffRunner) appendPlanEntry(exec *diffRoundExec, cancelAfter int) {
	r.plan = append(r.plan, DiffRound{
		Command:     exec.command,
		Input:       cloneBytes(exec.input),
		Polls:       append([]int(nil), exec.polls...),
		CancelAfter: cancelAfter,
	})
}

// runDiffGenerated drives a run whose choices come from the caller's Rapid
// bitstream. Generated command indices, parameters and poll choices all go through
// normal shrinkable generator grouping, with no extra RNG.
func runDiffGenerated(t *T, config DiffConfig) (report DiffReport) {
	runner := newDiffRunner(config)
	defer runner.closeBackends()

	if config.Rounds == 0 {
		return runner.report(diffEqual)
	}

	for round := 0; round < config.Rounds; round++ {
		common := runner.readyCommands()
		if len(common) == 0 {
			return runner.report(diffInvalid)
		}

		commandPos := IntRange(0, len(common)-1).Draw(t, "diff command")
		command := common[commandPos]
		input := config.Commands[command].Input(t)

		exec := runner.startRound(round, command, input)
		status, cancelAfter := runner.scheduleGenerated(t, round, exec)
		runner.appendPlanEntry(exec, cancelAfter)

		switch status {
		case diffEqual:
			runner.rounds++
		case diffMismatch, diffCancelled, diffInvalid:
			return runner.report(status)
		}
	}

	return runner.report(diffEqual)
}

// scheduleGenerated runs one generated round to its terminal barrier. The returned
// status is equal, mismatch, cancelled or invalid; cancelAfter is config.CancelAt
// only when that poll boundary was actually reached, and 0 otherwise.
func (r *diffRunner) scheduleGenerated(t *T, round int, exec *diffRoundExec) (status string, cancelAfter int) {
	for {
		unfinished := unfinishedIndices(exec.ops)
		if len(unfinished) == 0 {
			if r.compareBarrier(exec) {
				return diffEqual, 0
			}
			return diffMismatch, 0
		}

		if exec.pollCount >= r.config.MaxPolls {
			r.cancelUnfinished(round, exec)
			return diffInvalid, 0
		}

		pos := IntRange(0, len(unfinished)-1).Draw(t, "diff backend")
		backend := unfinished[pos]
		r.pollOne(round, exec, backend)

		// Cancellation at an explicit Poll boundary takes precedence over
		// completion and comparison, even if this Poll finished the last op.
		if r.config.CancelAt > 0 && exec.pollCount == r.config.CancelAt {
			r.cancelUnfinished(round, exec)
			return diffCancelled, r.config.CancelAt
		}

		if allDone(exec.ops) {
			if r.compareBarrier(exec) {
				return diffEqual, 0
			}
			return diffMismatch, 0
		}
	}
}

// runDiffReplay drives a run from an explicit plan with no draws and no Input
// callbacks. Each round's shape is validated before any of its operations start,
// and the first invalid action stops the run without touching later entries.
func runDiffReplay(config DiffConfig, plan []DiffRound) (report DiffReport) {
	runner := newDiffRunner(config)
	defer runner.closeBackends()

	if config.Rounds == 0 {
		if len(plan) != 0 {
			return runner.report(diffInvalid)
		}
		return runner.report(diffEqual)
	}

	if len(plan) == 0 {
		return runner.report(diffInvalid)
	}

	// Execute at most config.Rounds entries; an over-long plan is invalid only if
	// no earlier round first ends the run with a non-equal result.
	limit := len(plan)
	if limit > config.Rounds {
		limit = config.Rounds
	}
	for round := 0; round < limit; round++ {
		entry := plan[round]
		if !validateReplayRound(len(runner.backends), len(config.Commands), config.MaxPolls, runner.readyCommands(), entry) {
			return runner.report(diffInvalid)
		}

		exec := runner.startRound(round, entry.Command, entry.Input)
		status := runner.scheduleReplay(round, exec, entry.Polls, entry.CancelAfter)
		// The report mirrors the explicitly attempted plan entry, owned by the report.
		runner.plan = append(runner.plan, cloneRound(entry))

		switch status {
		case diffEqual:
			runner.rounds++
		case diffMismatch, diffCancelled, diffInvalid:
			return runner.report(status)
		}
	}

	if len(plan) != config.Rounds {
		return runner.report(diffInvalid)
	}
	return runner.report(diffEqual)
}

// validateReplayRound checks the round shape before any operation starts: index
// ranges, the cancellation boundary, poll budget, per-poll backend indices and
// common-readiness of the command. Conditions that depend on runtime completion
// (polling a completed backend, running out of choices, polls after completion)
// are enforced while the round executes; later plan entries are never touched.
func validateReplayRound(numBackends, numCommands, maxPolls int, common []int, entry DiffRound) bool {
	if entry.Command < 0 || entry.Command >= numCommands {
		return false
	}
	if entry.CancelAfter < 0 || entry.CancelAfter > len(entry.Polls) {
		return false
	}
	if len(entry.Polls) > maxPolls {
		return false
	}
	for _, backend := range entry.Polls {
		if backend < 0 || backend >= numBackends {
			return false
		}
	}

	for _, idx := range common {
		if idx == entry.Command {
			return true
		}
	}
	return false
}

// scheduleReplay executes a structurally validated round from explicit poll
// choices with no draws. Runtime shape violations cancel any unfinished operation
// and end the run as invalid; an inconclusive round is never called a difference.
func (r *diffRunner) scheduleReplay(round int, exec *diffRoundExec, polls []int, cancelAt int) string {
	for i, backend := range polls {
		if allDone(exec.ops) {
			return diffInvalid // extra poll choices after completion
		}
		if exec.ops[backend].done {
			r.cancelUnfinished(round, exec)
			return diffInvalid // poll of a completed backend
		}

		r.pollOne(round, exec, backend)

		if cancelAt > 0 && i+1 == cancelAt {
			if i+1 != len(polls) {
				r.cancelUnfinished(round, exec)
				return diffInvalid // poll choices after the cancellation boundary
			}
			r.cancelUnfinished(round, exec)
			return diffCancelled
		}
	}

	if cancelAt > 0 {
		r.cancelUnfinished(round, exec)
		return diffInvalid // configured boundary lies beyond the supplied polls
	}
	if !allDone(exec.ops) {
		r.cancelUnfinished(round, exec)
		return diffInvalid // insufficient poll choices
	}

	if r.compareBarrier(exec) {
		return diffEqual
	}
	return diffMismatch
}
