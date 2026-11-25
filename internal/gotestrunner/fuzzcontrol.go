package gotestrunner

// Keyed fuzzing scheduler. A FuzzController admits fuzz targets by key: each
// Submit mints a per-key generation (ticket), at most one process per key is
// running, and replacing or canceling a ticket revokes its output before the
// invalidation call returns. Real process concurrency, including retiring
// generations, is bounded by the same resolveFuzzJobs limit the batch path
// uses. Execution reuses buildFuzzCmd, the fixture environment,
// ManagedProcess and the FuzzTargetOutcome interpretation of RunFuzzTargets —
// only the admission/scheduling layer and the event sink in fuzzoutput.go are
// new.

import (
	"context"
	"errors"
	"os"
	"sync"
)

// FuzzAttempt identifies one admitted generation of a key.
type FuzzAttempt struct {
	Key        string
	Generation uint64
}

// FuzzEvent is one filtered output line attributed to exactly one attempt.
type FuzzEvent struct {
	Attempt  FuzzAttempt
	Stream   string // "stdout" or "stderr"
	Text     string // line payload without label or trailing newline
	Sequence uint64 // 1-based, increasing across both streams of the attempt
}

// FuzzAttemptOutcome is one ticket's final record. Every accepted ticket gets
// exactly one, in global admission order.
type FuzzAttemptOutcome struct {
	Attempt    FuzzAttempt
	Target     FuzzTarget
	Outcome    FuzzTargetOutcome
	Superseded bool
}

// FuzzSessionResult is a detached snapshot handed back by Wait.
type FuzzSessionResult struct {
	Attempts []FuzzAttemptOutcome
}

// Admission errors. They are sentinels so callers can classify a rejected
// Submit without parsing text.
var (
	errFuzzInvalidTicket = errors.New("gotestrunner: fuzz Submit requires a nonempty key, target Func and target Dir")
	errFuzzAdmission     = errors.New("gotestrunner: fuzz session is sealed or its parent context is done")
	errFuzzExhausted     = errors.New("gotestrunner: fuzz key generation space exhausted")
)

// fuzzNodeState is the internal lifecycle of one admitted ticket.
type fuzzNodeState int

const (
	fuzzQueued fuzzNodeState = iota
	fuzzRunning
	fuzzFinished
)

// fuzzNode is the controller's record of one ticket. All fields except out
// (which has its own lock) and the immutable admission values are guarded by
// the controller mutex.
type fuzzNode struct {
	attempt FuzzAttempt
	target  FuzzTarget
	state   fuzzNodeState

	superseded bool
	canceled   bool
	outcome    FuzzTargetOutcome

	// done closes once the final outcome has been recorded. Same-key
	// successors wait on it, so a new process never starts before the old
	// tree has stopped and both output streams have drained.
	done chan struct{}

	// prevDone is the previous generation's done channel for the same key,
	// nil for generation 1.
	prevDone <-chan struct{}

	// Owns this ticket's output revocation and sequence counter.
	out *fuzzEventOut

	// cancel stops this ticket's own process context. Set before the node
	// leaves fuzzQueued, so every invalidation reaches a live cancel.
	cancel context.CancelFunc
}

// FuzzController schedules keyed fuzz attempts. It is safe for concurrent use.
type FuzzController struct {
	ctx    context.Context
	cfg    FuzzRunConfig
	jobs   int
	events fuzzEventSink

	mu sync.Mutex

	sealed     bool
	gens       map[string]uint64
	nodes      map[FuzzAttempt]*fuzzNode
	keyCurrent map[string]FuzzAttempt // latest admitted ticket per key, finished or not
	order      []*fuzzNode            // admission order, once appended never removed
	active     int                    // real slots held, retiring generations included

	// cond is a 1-capacity scheduler kick. The loop always drains it before
	// re-evaluating, so a kick landing while the loop is busy is not lost.
	cond chan struct{}

	waitOnce sync.Once
	allDone  chan struct{} // closed once the scheduler has retired every ticket
}

// StartFuzzSession opens a controller over parent ctx. cfg.BuildFlags is
// copied here so later caller mutations cannot reach a running subprocess. A
// nil sink suppresses live output.
func StartFuzzSession(ctx context.Context, cfg FuzzRunConfig, sink func(FuzzEvent)) *FuzzController { //nolint:gocritic // hugeParam: stable API, mirrors RunFuzzTargets
	if cfg.BuildFlags != nil {
		cfg.BuildFlags = append([]string(nil), cfg.BuildFlags...)
	}
	c := &FuzzController{
		ctx:        ctx,
		cfg:        cfg,
		jobs:       resolveFuzzJobs(cfg.Jobs),
		events:     fuzzEventSink{sink: sink},
		gens:       map[string]uint64{},
		nodes:      map[FuzzAttempt]*fuzzNode{},
		keyCurrent: map[string]FuzzAttempt{},
		cond:       make(chan struct{}, 1),
		allDone:    make(chan struct{}),
	}
	go c.schedule()
	return c
}

// Submit admits a new generation for key, replacing whatever was the key's
// current ticket. Invalid submissions and submissions after sealing or parent
// cancellation change nothing and return an error.
func (c *FuzzController) Submit(key string, target FuzzTarget) (FuzzAttempt, error) { //nolint:gocritic // hugeParam: FuzzTarget is passed by value throughout the fuzz APIs
	if key == "" || target.Func == "" || target.Dir == "" {
		return FuzzAttempt{}, errFuzzInvalidTicket
	}

	c.mu.Lock()
	defer c.mu.Unlock()

	if c.sealed || c.ctx.Err() != nil {
		return FuzzAttempt{}, errFuzzAdmission
	}

	gen := c.gens[key] + 1
	if gen == 0 { // uint64 wraparound: no wraparound is allowed
		return FuzzAttempt{}, errFuzzExhausted
	}
	c.gens[key] = gen

	ticket := FuzzAttempt{Key: key, Generation: gen}
	n := &fuzzNode{
		attempt: ticket,
		target:  target, // value copied at admission; FuzzTarget holds strings only
		state:   fuzzQueued,
		done:    make(chan struct{}),
		out:     &fuzzEventOut{c: c, attempt: ticket},
	}
	if prev, ok := c.keyCurrent[key]; ok {
		prevNode := c.nodes[prev]
		n.prevDone = prevNode.done
		// Superseded stays recorded even when the previous generation has
		// already finished; its raw outcome is never rewritten into the
		// successor.
		prevNode.superseded = true
		c.invalidateLocked(prevNode)
	}

	c.keyCurrent[key] = ticket
	c.nodes[ticket] = n
	c.order = append(c.order, n)
	c.kickLocked()
	return ticket, nil
}

// Cancel revokes the exact current unfinished ticket, once. Stale, completed
// and already-canceled tickets return false and cannot touch another
// generation.
func (c *FuzzController) Cancel(ticket FuzzAttempt) bool {
	c.mu.Lock()
	defer c.mu.Unlock()

	n, ok := c.nodes[ticket]
	if !ok || c.keyCurrent[ticket.Key] != ticket {
		return false
	}
	if n.state == fuzzFinished || n.canceled {
		return false
	}
	n.canceled = true
	c.invalidateLocked(n)
	return true
}

// Wait seals admission idempotently, waits for every admitted ticket —
// retiring processes and output readers included — and returns an independent
// snapshot in admission order. Sealing does not stop healthy work.
func (c *FuzzController) Wait() FuzzSessionResult {
	c.waitOnce.Do(func() {
		c.mu.Lock()
		c.sealed = true
		c.kickLocked()
		c.mu.Unlock()
	})
	<-c.allDone

	c.mu.Lock()
	defer c.mu.Unlock()
	return c.snapshotLocked()
}

// invalidateLocked revokes a ticket's output immediately and, when it has a
// process, cancels that process. It blocks until no output callback for the
// ticket is in flight, which is what makes the Submit/Cancel return barrier.
// Callers hold c.mu; callbacks never take it, so the wait cannot deadlock.
func (c *FuzzController) invalidateLocked(n *fuzzNode) {
	n.out.revoke()
	if n.state == fuzzRunning && n.cancel != nil {
		n.cancel()
	}
}

// schedule is the single scheduler goroutine. It parks on cond and, on every
// kick, starts every queued ticket that is eligible and fits a free real slot.
func (c *FuzzController) schedule() {
	for {
		c.mu.Lock()
		if c.pumpLocked() {
			ch := c.cond
			c.mu.Unlock()
			<-ch
			continue
		}
		c.mu.Unlock()
		close(c.allDone)
		return
	}
}

// pumpLocked starts everything currently startable and reports whether the
// scheduler must keep running. It returns false only once admission is sealed
// and every admitted ticket is finalized.
func (c *FuzzController) pumpLocked() bool {
	for c.active < c.jobs {
		n := c.pickLocked()
		if n == nil {
			break
		}
		// The context is born here, under the lock, so an invalidation that
		// lands before runAttempt runs still finds a working cancel.
		ctx, cancel := context.WithCancel(c.ctx)
		n.cancel = cancel
		n.state = fuzzRunning
		c.active++
		go c.runAttempt(n, ctx)
	}

	for _, n := range c.order {
		if n.state != fuzzFinished {
			return true
		}
	}
	// All admitted work is finalized. Keep the loop only while more could be
	// admitted: a sealed session, or one whose parent context is done (nothing
	// may start afterwards), lets the scheduler retire instead of parking.
	return !c.sealed && c.ctx.Err() == nil
}

// pickLocked returns the first queued ticket whose same-key predecessor has
// fully retired and for which a slot is available (the caller checks capacity).
// A blocked same-key successor is skipped, so unrelated eligible keys keep
// moving: admission FIFO across distinct eligible keys.
func (c *FuzzController) pickLocked() *fuzzNode {
	for _, n := range c.order {
		if n.state != fuzzQueued {
			continue
		}
		if n.prevDone != nil {
			select {
			case <-n.prevDone:
			default:
				continue
			}
		}
		return n
	}
	return nil
}

// runAttempt owns one ticket from the moment the scheduler grants it a real
// slot until its outcome is recorded. The slot (active) stays held until this
// goroutine returns, so a retiring generation whose process tree is stopping
// and whose streams are draining still counts against cfg.Jobs. Tickets
// invalidated or parent-canceled before their process would start finalize
// here as Skipped, still passing through the same slot accounting.
func (c *FuzzController) runAttempt(n *fuzzNode, ctx context.Context) {
	defer n.cancel()

	outcome := c.execute(n, ctx)

	c.mu.Lock()
	n.outcome = outcome
	n.state = fuzzFinished
	close(n.done)
	c.active--
	c.kickLocked()
	c.mu.Unlock()
}

// execute runs the ticket's process unless it was never eligible to start,
// translating both paths into one FuzzTargetOutcome.
func (c *FuzzController) execute(n *fuzzNode, ctx context.Context) FuzzTargetOutcome {
	// The invalidation flags are owned by c.mu; read them under it so a
	// Submit or Cancel racing the slot grant cannot trip the race detector.
	// A later invalidation is delivered through ctx cancellation instead.
	c.mu.Lock()
	superseded, canceled := n.superseded, n.canceled
	c.mu.Unlock()
	if superseded || canceled || ctx.Err() != nil {
		// Accepted but never started: a coalesced queue generation, a ticket
		// canceled before its slot, or a queued ticket orphaned by parent
		// cancellation. Each still gets its own final state.
		return FuzzTargetOutcome{
			Func:     n.target.Func,
			Skipped:  true,
			Canceled: canceled || ctx.Err() != nil,
		}
	}
	return c.executeProcess(n, ctx)
}

// executeProcess is the controller's twin of runOneFuzzTarget: same command
// construction, fixture environment, ManagedProcess grace strategy and
// outcome interpretation, with the per-attempt event writers replacing the
// console line writers.
func (c *FuzzController) executeProcess(n *fuzzNode, ctx context.Context) FuzzTargetOutcome {
	t := n.target

	env, never := c.cfg.Fixtures.Env(t)
	if never != "" {
		// The shared fixture this target binds never came up; the batch path
		// treats this as a target-level failure without starting the tool.
		return FuzzTargetOutcome{Func: t.Func, ExitCode: 1}
	}

	before := snapshotCrashers(t)
	cmd := buildFuzzCmd(ctx, t, c.cfg, 0) // no per-target -fuzztime: admission owns the clocks
	if len(env) > 0 {
		cmd.Env = append(os.Environ(), env...)
	}

	stdoutW := newFuzzStream("stdout", t.Func, n.out)
	stderrW := newFuzzStream("stderr", t.Func, n.out)
	cmd.Stdout = stdoutW
	cmd.Stderr = stderrW

	// Same graceful Ctrl-C shutdown as RunFuzzTargets: go test -fuzz saves
	// what it found and tears fixtures down before exiting.
	mp := NewManagedProcess(cmd, ProcessConfig{
		Grace:         GraceFixed,
		GraceDuration: fuzzGrace,
		CtrlC:         true,
	})
	if err := mp.Start(); err != nil {
		_ = stdoutW.Close()
		_ = stderrW.Close()
		return FuzzTargetOutcome{Func: t.Func, ExitCode: 2}
	}

	_ = mp.WaitWithGrace(ctx)
	// Sampled at the same point as the batch path: the shutdown this session
	// caused is distinguishable from a verdict only while the context is done.
	canceled := ctx.Err() != nil
	// Closing flushes a final partial line; ManagedProcess has finished both
	// output copies by now, so no Write can follow.
	_ = stdoutW.Close()
	_ = stderrW.Close()

	exitCode := 0
	if cmd.ProcessState != nil {
		exitCode, _ = ExitStatusVerdict(cmd.ProcessState.ExitCode())
	}

	progress := stdoutW.progress
	if stderrW.progress.Execs > progress.Execs {
		progress = stderrW.progress
	}
	return FuzzTargetOutcome{
		Func:           t.Func,
		ExitCode:       exitCode,
		Canceled:       canceled,
		NewCrashers:    newCrasherNames(before, snapshotCrashers(t)),
		Execs:          progress.Execs,
		Interesting:    progress.Interesting,
		EngineDeadline: exitCode == 1 && (stdoutW.deadlineFail || stderrW.deadlineFail),
	}
}

// snapshotLocked builds a fresh, detached result in admission order. Slices
// are copied so a caller cannot mutate controller-owned memory.
func (c *FuzzController) snapshotLocked() FuzzSessionResult {
	res := FuzzSessionResult{Attempts: make([]FuzzAttemptOutcome, 0, len(c.order))}
	for _, n := range c.order {
		record := FuzzAttemptOutcome{
			Attempt:    n.attempt,
			Target:     n.target,
			Superseded: n.superseded,
			Outcome:    n.outcome,
		}
		if n.outcome.NewCrashers != nil {
			record.Outcome.NewCrashers = append([]string(nil), n.outcome.NewCrashers...)
		}
		res.Attempts = append(res.Attempts, record)
	}
	return res
}

// kickLocked wakes the scheduler. A full channel means a wake is already
// pending; the scheduler re-evaluates all state whenever it drains it.
func (c *FuzzController) kickLocked() {
	select {
	case c.cond <- struct{}{}:
	default:
	}
}

// deliver is the single entry point fuzzStream uses for live events.
func (c *FuzzController) deliver(ev FuzzEvent) {
	c.events.emit(ev)
}
