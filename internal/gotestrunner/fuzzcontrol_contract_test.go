//go:build unix

// Keyed FuzzController contract tests. Scheduling is driven through a fake
// "go" tool on PATH — a shell stand-in that EMULATES go test -fuzz output and
// shutdown, coordinated entirely by files and pipes as ack barriers (ready,
// stop, halfway markers), never by sleep-based timing guesses. One test also
// drives the bundled benign FuzzEcho through the real go tool. The stand-in
// never generates a failing or crashing input and reaches no external target.

package gotestrunner_test

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/mvrahden/go-test/internal/gotestrunner"
	"github.com/mvrahden/go-test/task_support/taskcheck"
)

const (
	helperDirEnv  = "GOTEST_FUZZ_HELPER_DIR"
	helperModeEnv = "GOTEST_FUZZ_HELPER_MODE"
)

// fakeGoScript is the go-tool stand-in. It parses the -fuzz flag the real
// runner builds, writes its state under the helper dir, and serves the
// block/halves/advice scenarios. Every per-invocation control file carries
// the invocation ordinal, so same-func generations never share or delete each
// other's barriers. Signals end it the way a graceful go test -fuzz ends on
// Ctrl-C: a trap, a held drain window, exit 0.
const fakeGoScript = `#!/bin/bash
# TEST-ONLY EMULATOR of "go test -fuzz" output and shutdown, not the go tool.
set -u
d="$GOTEST_FUZZ_HELPER_DIR"
fn=""
for a in "$@"; do
  case "$a" in
    -fuzz=^*) fn="${a#-fuzz=^}"; fn="${fn%\$}" ;;
  esac
done
mkdir -p "$d"
echo "$fn $$" >> "$d/runs"
ord=$(grep -c "^$fn " "$d/runs")
progress() {
  printf 'fuzz: elapsed: 0s, execs: %s (1/sec), new interesting: 0 (total: 1)\n' "$1"
}
case "$GOTEST_FUZZ_HELPER_MODE" in
  halves)
    # First half of a stdout line with no newline, a complete stderr line,
    # then a release barrier before the line is completed.
    printf 'partial-'
    echo 'errline' >&2
    touch "$d/halfway-$fn"
    while [ ! -f "$d/release-$fn" ]; do sleep 0.02; done
    printf 'joined\nmore\n'
    echo 'err2' >&2
    exit 0
    ;;
  advice)
    echo '    To re-run:'
    echo '    go test -run=FuzzAdvice/abc'
    echo 'FAIL'
    exit 0
    ;;
  *)
    # block mode: emit, install the signal trap, and only then publish the
    # ready barrier, so a signal can never arrive before the trap exists.
    progress $((ord * 100))
    echo "out-$fn-1"
    echo "err-$fn-1" >&2
    # On signal, publish the stopped barrier, then hold the process alive and
    # draining until the test releases it (bounded safety wait, so a faulty
    # test can never wedge the run past the process manager's own grace).
    trap 'touch "$d/stopped-$fn-$ord"; i=0; while [ ! -f "$d/release-stop-$fn-$ord" ] && [ $i -lt 500 ]; do sleep 0.02; i=$((i+1)); done; break' INT TERM
    touch "$d/ready-$fn-$ord"
    while [ ! -f "$d/stop-$fn-$ord" ]; do sleep 0.02; done
    progress $((ord * 100 + 50))
    echo "drain-out-$fn"
    echo "drain-err-$fn" >&2
    exit 0
    ;;
esac
`

// fuzzHarness owns the fake go tool binding and the barrier helpers one test
// uses. Each top-level test gets a fresh one.
type fuzzHarness struct {
	t        *testing.T
	binDir   string
	state    string
	realPATH string
}

func newFuzzHarness(t *testing.T) *fuzzHarness {
	t.Helper()
	h := &fuzzHarness{t: t, binDir: t.TempDir(), state: t.TempDir()}
	script := filepath.Join(h.binDir, "go")
	if err := os.WriteFile(script, []byte(fakeGoScript), 0o755); err != nil { //nolint:gosec // G306: throwaway test binary
		t.Fatalf("write fake go: %v", err)
	}
	realPATH, ok := os.LookupEnv("PATH")
	if !ok {
		t.Fatal("PATH not set")
	}
	h.realPATH = realPATH
	t.Setenv("PATH", h.binDir+string(os.PathListSeparator)+realPATH)
	t.Setenv(helperDirEnv, h.state)
	t.Setenv(helperModeEnv, "block")
	return h
}

// useRealGo restores a PATH without the fake tool for a real go-tool
// scenario. The call belongs to the subtest t, so its parent's fake-tool
// environment comes back when that subtest ends.
func (h *fuzzHarness) useRealGo(t *testing.T) {
	t.Helper()
	var kept []string
	for _, p := range strings.Split(h.realPATH, string(os.PathListSeparator)) {
		if p != h.binDir {
			kept = append(kept, p)
		}
	}
	t.Setenv("PATH", strings.Join(kept, string(os.PathListSeparator)))
	t.Setenv(helperModeEnv, "")
	t.Setenv(helperDirEnv, "")
}

func (h *fuzzHarness) target(fn string) gotestrunner.FuzzTarget {
	return gotestrunner.FuzzTarget{Package: ".", Dir: h.state, Func: fn, Suite: "Helper"}
}

func (h *fuzzHarness) touch(name string) {
	h.t.Helper()
	if err := os.WriteFile(filepath.Join(h.state, name), []byte("x"), 0o600); err != nil { //nolint:gosec // G306: barrier file
		h.t.Fatalf("touch %s: %v", name, err)
	}
}

// waitForBarrier polls an ack file. This is a barrier wait with a generous
// deadline, not a sleep-based timing assumption: the test proceeds the instant
// the helper publishes.
func (h *fuzzHarness) waitForBarrier(name string) {
	h.t.Helper()
	path := filepath.Join(h.state, name)
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(path); err == nil {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	h.t.Fatalf("barrier %s never appeared", name)
}

// runCount counts helper invocations for fn via the append-only runs file.
func (h *fuzzHarness) runCount(fn string) int {
	h.t.Helper()
	data, err := os.ReadFile(filepath.Join(h.state, "runs")) //nolint:gosec // G304: the harness's own state
	if err != nil {
		return 0
	}
	n := 0
	for _, line := range strings.Split(strings.TrimSpace(string(data)), "\n") {
		if strings.HasPrefix(line, fn+" ") {
			n++
		}
	}
	return n
}

func (h *fuzzHarness) waitForRunCount(fn string, want int) {
	h.t.Helper()
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		if h.runCount(fn) == want {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	h.t.Fatalf("func %s ran %d times, want %d", fn, h.runCount(fn), want)
}

// waitForReady blocks until the latest invocation of fn has published its
// ready barrier.
func (h *fuzzHarness) waitForReady(fn string) {
	h.t.Helper()
	h.waitForRunCount(fn, h.runCountOrOne(fn))
	h.waitForBarrier(fmt.Sprintf("ready-%s-%d", fn, h.runCount(fn)))
}

func (h *fuzzHarness) runCountOrOne(fn string) int {
	if n := h.runCount(fn); n > 0 {
		return n
	}
	return 1
}

// waitForStopped blocks until the latest invocation of fn has handled a
// signal and published its stopped barrier.
func (h *fuzzHarness) waitForStopped(fn string) {
	h.t.Helper()
	h.waitForBarrier(fmt.Sprintf("stopped-%s-%d", fn, h.runCount(fn)))
}

// stop asks the latest invocation of fn to finish naturally.
func (h *fuzzHarness) stop(fn string) {
	h.t.Helper()
	h.touch(fmt.Sprintf("stop-%s-%d", fn, h.runCount(fn)))
}

// releaseDrain lets the latest signaled invocation of fn leave its drain.
func (h *fuzzHarness) releaseDrain(fn string) {
	h.t.Helper()
	h.touch(fmt.Sprintf("release-stop-%s-%d", fn, h.runCount(fn)))
}

// eventCollector records every sink callback under its own lock.
type eventCollector struct {
	mu     sync.Mutex
	events []gotestrunner.FuzzEvent
}

func (l *eventCollector) sink(ev gotestrunner.FuzzEvent) {
	l.mu.Lock()
	l.events = append(l.events, ev)
	l.mu.Unlock()
}

func (l *eventCollector) snapshot() []gotestrunner.FuzzEvent {
	l.mu.Lock()
	defer l.mu.Unlock()
	return slices.Clone(l.events)
}

// waitFor blocks until an event on stream with text has been delivered to the
// sink — a sink-side ack barrier, so a test distinguishes "the helper wrote"
// from "the pipeline delivered". It polls with a deadline rather than guessing
// a fixed wait.
func (l *eventCollector) waitFor(t *testing.T, stream, text string) {
	t.Helper()
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		l.mu.Lock()
		for _, ev := range l.events {
			if ev.Stream == stream && ev.Text == text {
				l.mu.Unlock()
				return
			}
		}
		l.mu.Unlock()
		time.Sleep(2 * time.Millisecond)
	}
	t.Fatalf("event %s/%s was not delivered to the sink", stream, text)
}

func eventTexts(events []gotestrunner.FuzzEvent, attempt gotestrunner.FuzzAttempt) []string {
	var out []string
	for _, ev := range events { //nolint:gocritic // rangeValCopy: FuzzEvent is a fixed-size value type
		if ev.Attempt == attempt {
			out = append(out, ev.Text)
		}
	}
	return out
}

// sameOutcome compares the outcome fields that survive snapshotting. Slices
// are compared by content since FuzzTargetOutcome itself is not comparable.
func sameOutcome(a, b gotestrunner.FuzzTargetOutcome) bool { //nolint:gocritic // hugeParam: symmetric value comparison
	return a.Func == b.Func && a.ExitCode == b.ExitCode && a.Canceled == b.Canceled &&
		a.Skipped == b.Skipped && a.Execs == b.Execs && a.Interesting == b.Interesting &&
		a.EngineDeadline == b.EngineDeadline && slices.Equal(a.NewCrashers, b.NewCrashers)
}

// assertSequences pins the per-attempt event contract: sequences start at 1
// and stay contiguous across the two streams, whose order is preserved.
func assertSequences(c *taskcheck.C, events []gotestrunner.FuzzEvent, attempt gotestrunner.FuzzAttempt) {
	var next uint64 = 1
	streamSeen := map[string]uint64{}
	for _, ev := range events { //nolint:gocritic // rangeValCopy: FuzzEvent is a fixed-size value type
		if ev.Attempt != attempt {
			continue
		}
		c.Check(ev.Stream == "stdout" || ev.Stream == "stderr", "event stream must be stdout or stderr, got %q", ev.Stream)
		c.Check(ev.Sequence == next, "%v sequence gap: want %d got %d", attempt, next, ev.Sequence)
		next++
		c.Check(!strings.HasSuffix(ev.Text, "\n"), "event text must not carry a final newline: %q", ev.Text)
		c.Check(!strings.HasPrefix(ev.Text, "["), "event text must not carry a console label: %q", ev.Text)
		if prev, ok := streamSeen[ev.Stream]; ok {
			c.Check(ev.Sequence > prev, "per-stream order broken for %s %v", ev.Stream, attempt)
		}
		streamSeen[ev.Stream] = ev.Sequence
	}
}

// stageBenignModule copies the bundled benign FuzzEcho module into a fresh
// throwaway directory so the real fuzz run never writes into the repo.
func stageBenignModule(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	for name, dst := range map[string]string{
		"go.mod":       filepath.Join("..", "..", "task_support", "benign", "go.mod"),
		"echo_test.go": filepath.Join("..", "..", "task_support", "benign", "echo_test.go"),
	} {
		data, err := os.ReadFile(dst) //nolint:gosec // G304: bundled benign module
		if err != nil {
			t.Fatalf("read %s: %v", dst, err)
		}
		if err := os.WriteFile(filepath.Join(dir, name), data, 0o600); err != nil { //nolint:gosec // G306: staged module
			t.Fatalf("stage %s: %v", name, err)
		}
	}
	return dir
}

// TestFuzzReplacement covers generation increments, old-generation drain
// before a same-key successor, cross-key fairness while a key is blocked,
// real-slot accounting through retirement, queued coalescing and invalid or
// sealed admissions.
func TestFuzzReplacement(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		h := newFuzzHarness(t)
		log := &eventCollector{}
		// Two real slots: a1 keeps one while its successor a2 is blocked on
		// its retirement, so the unrelated key b can take the other slot and
		// prove the FIFO rule skips only blocked same-key successors.
		session := gotestrunner.StartFuzzSession(context.Background(), gotestrunner.FuzzRunConfig{Jobs: 2}, log.sink)

		// Phase 1: one running generation, then a same-key replacement and an
		// unrelated key admitted while the old generation is still draining.
		a1, err := session.Submit("a", h.target("FuzzBlockA"))
		c.Check(err == nil, "first submit: %v", err)
		c.Check(a1 == (gotestrunner.FuzzAttempt{Key: "a", Generation: 1}), "first ticket identity: %+v", a1)
		h.waitForReady("FuzzBlockA")

		a2, err := session.Submit("a", h.target("FuzzBlockA"))
		c.Check(err == nil, "replacement submit: %v", err)
		c.Check(a2.Generation == 2, "replacement increments the per-key generation")
		b1, err := session.Submit("b", h.target("FuzzBlockB"))
		c.Check(err == nil, "unrelated key submit: %v", err)
		c.Check(b1.Generation == 1, "a new key starts at generation 1")

		// b1 fills the second slot while a1 still holds the first and a2 is
		// blocked on a1's retirement: cross-key fairness, real slot maximum.
		h.waitForReady("FuzzBlockB")
		// a1 is superseded and signaled; its trap fires and holds the process
		// in its drain window until the test releases it.
		h.waitForStopped("FuzzBlockA")
		c.Check(h.runCount("FuzzBlockA") == 1, "the same-key successor must not start before the old process tree and streams retire")
		c.Check(h.runCount("FuzzBlockB") == 1, "unrelated eligible work runs while key a is blocked")

		// Release the retired generation's drain; only then may a2 take the
		// freed slot. b1 finishes on its own natural stop.
		h.releaseDrain("FuzzBlockA")
		h.waitForRunCount("FuzzBlockA", 2)
		h.stop("FuzzBlockB")
		h.stop("FuzzBlockA")

		res := session.Wait()
		c.Check(len(res.Attempts) == 3, "every accepted ticket gets one outcome, got %d", len(res.Attempts))

		c.Check(res.Attempts[0].Attempt == a1 && res.Attempts[1].Attempt == a2 && res.Attempts[2].Attempt == b1,
			"outcomes are returned in global admission order: %+v", res.Attempts)

		o1, o2, ob := res.Attempts[0].Outcome, res.Attempts[1].Outcome, res.Attempts[2].Outcome
		c.Check(res.Attempts[0].Superseded && o1.Canceled && !o1.Skipped, "superseded running generation is canceled, not skipped")
		// The old generation keeps its own last-seen progress (100 at start, or
		// 150 if its final drain line is read before the stream closes); either
		// way it is a genuine metric of this ticket, never the successor's.
		c.Check(o1.Execs == 100 || o1.Execs == 150, "the old generation's raw metric stays on its own ticket: got %d", o1.Execs)
		c.Check(!res.Attempts[1].Superseded && !o2.Canceled && !o2.Skipped, "the successor runs its own fresh process")
		c.Check(o2.Execs == 250, "the successor's progress is its own and never the old ticket's: got %d", o2.Execs)
		c.Check(o1.Execs != o2.Execs, "the two generations cannot share a metric")
		c.Check(!res.Attempts[2].Superseded && !ob.Canceled && ob.EffectiveExitCode() == 0, "unrelated key runs healthily")

		// Output attribution: pre-invalidation lines survive, post-revocation
		// drain lines can never appear under the old ticket.
		events := log.snapshot()
		c.Check(slices.Contains(eventTexts(events, a1), "out-FuzzBlockA-1"), "events emitted before invalidation remain valid")
		for _, ev := range events { //nolint:gocritic // rangeValCopy: FuzzEvent is a fixed-size value type
			if ev.Attempt == a1 {
				c.Check(!strings.HasPrefix(ev.Text, "drain-"), "no obsolete output may be attributed to the retired ticket: %q", ev.Text)
			}
		}
		c.Check(slices.Contains(eventTexts(events, a2), "drain-out-FuzzBlockA"), "the successor's own drained output is attributed to it")
		c.Check(slices.Contains(eventTexts(events, b1), "drain-err-FuzzBlockB"), "the other key's drained output is attributed to it")
		assertSequences(c, events, a1)
		assertSequences(c, events, a2)
		assertSequences(c, events, b1)

		// Sealed admission rejects without changing the result.
		_, err = session.Submit("late", h.target("FuzzLate"))
		c.Check(err != nil, "Submit after Wait seals admission")
		c.Check(len(session.Wait().Attempts) == 3, "a rejected submission is absent from results")

		// Phase 2: queued coalescing. Three replacements while the key is
		// occupied leave every ticket an identity-bearing terminal state.
		log2 := &eventCollector{}
		coalesced := gotestrunner.StartFuzzSession(context.Background(), gotestrunner.FuzzRunConfig{Jobs: 1}, log2.sink)
		c1, err := coalesced.Submit("c", h.target("FuzzBlockC"))
		c.Check(err == nil, "coalesce first submit: %v", err)
		h.waitForReady("FuzzBlockC")
		c2, err := coalesced.Submit("c", h.target("FuzzBlockC"))
		c.Check(err == nil, "coalesce second submit: %v", err)
		c3, err := coalesced.Submit("c", h.target("FuzzBlockC"))
		c.Check(err == nil, "coalesce third submit: %v", err)
		c4, err := coalesced.Submit("c", h.target("FuzzBlockC"))
		c.Check(err == nil, "coalesce fourth submit: %v", err)
		c.Check(c2.Generation == 2 && c3.Generation == 3 && c4.Generation == 4, "generations increase per key")

		// c1 is superseded by c2's admission; release its drain so c4 starts.
		h.waitForStopped("FuzzBlockC")
		h.releaseDrain("FuzzBlockC")
		h.waitForRunCount("FuzzBlockC", 2) // c1 retires; c2/c3 skip; c4 starts
		h.stop("FuzzBlockC")
		cres := coalesced.Wait()
		c.Check(len(cres.Attempts) == 4, "every accepted ticket, including coalesced ones, has exactly one outcome")
		want := []struct {
			ticket     gotestrunner.FuzzAttempt
			superseded bool
			skipped    bool
			canceled   bool
		}{
			{c1, true, false, true},
			{c2, true, true, false},
			{c3, true, true, false},
			{c4, false, false, false},
		}
		for i, w := range want { //nolint:gocritic // rangeValCopy: table value
			got := cres.Attempts[i]
			c.Check(got.Attempt == w.ticket, "coalesced outcome %d ticket identity", i)
			c.Check(got.Superseded == w.superseded, "coalesced outcome %d superseded", i)
			c.Check(got.Outcome.Skipped == w.skipped, "coalesced outcome %d skipped", i)
			c.Check(got.Outcome.Canceled == w.canceled, "coalesced outcome %d canceled", i)
		}

		// Phase 3: invalid submissions change nothing.
		bad := gotestrunner.StartFuzzSession(context.Background(), gotestrunner.FuzzRunConfig{Jobs: 1}, nil)
		_, err = bad.Submit("", h.target("FuzzBad"))
		c.Check(err != nil, "empty key is rejected")
		_, err = bad.Submit("z", gotestrunner.FuzzTarget{Dir: h.state})
		c.Check(err != nil, "empty Func is rejected")
		_, err = bad.Submit("z", gotestrunner.FuzzTarget{Func: "FuzzBad"})
		c.Check(err != nil, "empty Dir is rejected")
		c.Check(len(bad.Wait().Attempts) == 0, "invalid submissions leave no outcomes behind")
	})
}

// TestFuzzCancellation covers the exact-ticket cancel rule, stale tickets,
// canceled-never-started skips, parent cancellation, healthy Wait sealing and
// independent concurrent Wait snapshots.
func TestFuzzCancellation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		h := newFuzzHarness(t)

		// Phase 1: cancel the exact current running ticket, once; stale,
		// repeated and completed tickets cannot cancel anything.
		log := &eventCollector{}
		session := gotestrunner.StartFuzzSession(context.Background(), gotestrunner.FuzzRunConfig{Jobs: 1}, log.sink)
		x1, err := session.Submit("x", h.target("FuzzBlockX"))
		c.Check(err == nil, "x submit: %v", err)
		h.waitForReady("FuzzBlockX")

		c.Check(session.Cancel(x1), "cancel of the current unfinished ticket succeeds")
		c.Check(!session.Cancel(x1), "the same ticket cannot be canceled twice")
		c.Check(!session.Cancel(gotestrunner.FuzzAttempt{Key: "x", Generation: 99}), "a stale ticket cannot cancel another generation")
		h.waitForStopped("FuzzBlockX")
		h.releaseDrain("FuzzBlockX")

		res := session.Wait()
		c.Check(len(res.Attempts) == 1, "the pure-cancel session has one outcome")
		c.Check(res.Attempts[0].Attempt == x1, "the outcome keeps the canceled ticket's identity")
		c.Check(res.Attempts[0].Outcome.Canceled && !res.Attempts[0].Outcome.Skipped && !res.Attempts[0].Superseded,
			"a running canceled ticket is Canceled with its raw outcome attached and is not marked superseded")
		c.Check(!session.Cancel(x1), "a completed ticket can no longer be canceled")
		for _, ev := range log.snapshot() { //nolint:gocritic // rangeValCopy: FuzzEvent is a fixed-size value type
			if ev.Attempt == x1 {
				c.Check(!strings.HasPrefix(ev.Text, "drain-"), "after Cancel returns no later output can carry the old ticket")
			}
		}

		// A replacement admitted after a cancel is a fresh, unaffected ticket.
		log2 := &eventCollector{}
		replaced := gotestrunner.StartFuzzSession(context.Background(), gotestrunner.FuzzRunConfig{Jobs: 1}, log2.sink)
		z1, err := replaced.Submit("z", h.target("FuzzBlockZ"))
		c.Check(err == nil, "z1 submit: %v", err)
		h.waitForReady("FuzzBlockZ")
		c.Check(replaced.Cancel(z1), "z1 cancel succeeds")
		h.waitForStopped("FuzzBlockZ")
		h.releaseDrain("FuzzBlockZ")
		z2, err := replaced.Submit("z", h.target("FuzzBlockZ"))
		c.Check(err == nil, "a new generation after cancel is admitted: %v", err)
		c.Check(z2.Generation == 2, "the post-cancel generation keeps incrementing")
		h.waitForRunCount("FuzzBlockZ", 2)
		h.stop("FuzzBlockZ")
		rres := replaced.Wait()
		c.Check(len(rres.Attempts) == 2, "both z generations have outcomes")
		c.Check(rres.Attempts[0].Attempt == z1 && rres.Attempts[0].Outcome.Canceled && rres.Attempts[0].Superseded,
			"the old ticket keeps its canceled raw outcome and records the supersession")
		c.Check(rres.Attempts[1].Attempt == z2 && !rres.Attempts[1].Outcome.Canceled && !rres.Attempts[1].Superseded,
			"the later generation is untouched by the old cancel")
		for _, ev := range log2.snapshot() { //nolint:gocritic // rangeValCopy: FuzzEvent is a fixed-size value type
			if ev.Attempt == z1 {
				c.Check(!strings.HasPrefix(ev.Text, "drain-"), "the retired ticket's late drain is not attributed to it")
			}
		}

		// Phase 2: a different key's ticket canceled while still queued never
		// starts and cannot touch the running ticket.
		queued := gotestrunner.StartFuzzSession(context.Background(), gotestrunner.FuzzRunConfig{Jobs: 1}, nil)
		y1, err := queued.Submit("y", h.target("FuzzBlockY"))
		c.Check(err == nil, "y1 submit: %v", err)
		h.waitForReady("FuzzBlockY")
		y2, err := queued.Submit("q", h.target("FuzzBlockQ"))
		c.Check(err == nil, "queued other-key submit: %v", err)
		c.Check(queued.Cancel(y2), "the queued current ticket can be canceled")
		c.Check(!queued.Cancel(y2), "a queued cancel also succeeds only once")
		c.Check(!queued.Cancel(gotestrunner.FuzzAttempt{Key: "q", Generation: 404}), "unknown ticket cancel is false")
		h.stop("FuzzBlockY")
		qres := queued.Wait()
		c.Check(len(qres.Attempts) == 2, "queued session outcome count")
		c.Check(qres.Attempts[0].Attempt == y1 && !qres.Attempts[0].Outcome.Canceled, "the running ticket is unaffected by the other key's queued cancel")
		c.Check(qres.Attempts[1].Attempt == y2 && qres.Attempts[1].Outcome.Skipped && qres.Attempts[1].Outcome.Canceled,
			"a canceled-but-never-started ticket is Skipped and Canceled")
		c.Check(h.runCount("FuzzBlockQ") == 0, "a skipped queued ticket never spawned a process")

		// Phase 3: parent cancellation cancels admitted work and forbids starts.
		parent, cancelParent := context.WithCancel(context.Background())
		parented := gotestrunner.StartFuzzSession(parent, gotestrunner.FuzzRunConfig{Jobs: 1}, nil)
		p1, err := parented.Submit("p", h.target("FuzzBlockP"))
		c.Check(err == nil, "p1 submit: %v", err)
		h.waitForReady("FuzzBlockP")
		p2, err := parented.Submit("p", h.target("FuzzBlockP2"))
		c.Check(err == nil, "p2 submit: %v", err)
		cancelParent()
		_, err = parented.Submit("p", h.target("FuzzBlockP3"))
		c.Check(err != nil, "nothing is admitted after parent cancellation")
		// Let the signaled process leave its drain window promptly.
		h.waitForStopped("FuzzBlockP")
		h.releaseDrain("FuzzBlockP")
		pres := parented.Wait()
		c.Check(len(pres.Attempts) == 2, "only admitted tickets are present after parent cancel")
		c.Check(pres.Attempts[0].Attempt == p1 && pres.Attempts[0].Outcome.Canceled && !pres.Attempts[0].Outcome.Skipped,
			"the running ticket is canceled by the parent")
		c.Check(pres.Attempts[1].Attempt == p2 && pres.Attempts[1].Outcome.Skipped && pres.Attempts[1].Outcome.Canceled,
			"the queued ticket is skipped and canceled")
		c.Check(h.runCount("FuzzBlockP2") == 0, "parent cancellation starts no queued process")

		// Phase 4: Wait seals without stopping healthy work.
		healthy := gotestrunner.StartFuzzSession(context.Background(), gotestrunner.FuzzRunConfig{Jobs: 1}, nil)
		w1, err := healthy.Submit("w", h.target("FuzzBlockW"))
		c.Check(err == nil, "w submit: %v", err)
		h.waitForReady("FuzzBlockW")
		var wg sync.WaitGroup
		var hres gotestrunner.FuzzSessionResult
		wg.Add(1)
		go func() {
			defer wg.Done()
			hres = healthy.Wait()
		}()
		if _, err := os.Stat(filepath.Join(h.state, "stopped-FuzzBlockW-1")); err == nil {
			c.Check(false, "sealing Wait must not signal a healthy ticket")
		}
		h.stop("FuzzBlockW")
		wg.Wait()
		c.Check(len(hres.Attempts) == 1 && hres.Attempts[0].Attempt == w1 && !hres.Attempts[0].Outcome.Canceled,
			"sealed healthy work runs to its own end")

		// Phase 5: concurrent and repeated Wait calls return independent,
		// equivalent snapshots.
		snap := gotestrunner.StartFuzzSession(context.Background(), gotestrunner.FuzzRunConfig{Jobs: 2}, nil)
		q1, err := snap.Submit("q", h.target("FuzzBlockQ1"))
		c.Check(err == nil, "q1 submit: %v", err)
		q2, err := snap.Submit("q2", h.target("FuzzBlockQ2"))
		c.Check(err == nil, "q2 submit: %v", err)
		h.waitForReady("FuzzBlockQ1")
		h.waitForReady("FuzzBlockQ2")
		h.stop("FuzzBlockQ1")
		h.stop("FuzzBlockQ2")

		const waiters = 5
		results := make([]gotestrunner.FuzzSessionResult, waiters)
		var swg sync.WaitGroup
		for i := range waiters {
			swg.Add(1)
			go func(i int) {
				defer swg.Done()
				results[i] = snap.Wait()
			}(i)
		}
		swg.Wait()
		again := snap.Wait()
		for i, r := range results { //nolint:gocritic // rangeValCopy: FuzzSessionResult value
			c.Check(len(r.Attempts) == 2, "waiter %d sees both tickets", i)
			c.Check(r.Attempts[0].Attempt == q1 && r.Attempts[1].Attempt == q2, "waiter %d admission order", i)
			c.Check(slices.EqualFunc(r.Attempts, again.Attempts, func(a, b gotestrunner.FuzzAttemptOutcome) bool {
				return a.Attempt == b.Attempt && a.Superseded == b.Superseded && sameOutcome(a.Outcome, b.Outcome)
			}), "waiter %d snapshot equals the repeated one", i)
		}
		// Detached snapshots: mutating one cannot reach the controller or a
		// later snapshot.
		results[0].Attempts[0].Outcome.Func = "mutated"
		results[0].Attempts = append(results[0].Attempts, gotestrunner.FuzzAttemptOutcome{})
		fresh := snap.Wait()
		c.Check(fresh.Attempts[0].Outcome.Func == "FuzzBlockQ1" && len(fresh.Attempts) == 2,
			"snapshots are detached and independent")
	})
}

// TestFuzzAttribution covers event identity and payload semantics: a real go
// tool run of FuzzEcho, half-line reassembly across two streams, filtered
// engine advice, a nil sink, sequence integrity and batch-path compatibility.
func TestFuzzAttribution(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		h := newFuzzHarness(t)

		// Phase 1: the real go tool drives the bundled benign FuzzEcho through
		// the same execution path the controller schedules.
		t.Run("real FuzzEcho", func(t *testing.T) {
			h.useRealGo(t)
			echoDir := stageBenignModule(t)
			log := &eventCollector{}
			session := gotestrunner.StartFuzzSession(context.Background(), gotestrunner.FuzzRunConfig{
				Jobs:       1,
				BuildFlags: []string{"-fuzztime=20x", "-parallel=1"},
			}, log.sink)
			ticket, err := session.Submit("echo", gotestrunner.FuzzTarget{
				Package: ".", Dir: echoDir, Func: "FuzzEcho", Suite: "Echo",
			})
			c.Check(err == nil, "real echo submit: %v", err)
			res := session.Wait()
			c.Check(len(res.Attempts) == 1, "one real fuzz outcome")
			o := res.Attempts[0].Outcome
			c.Check(res.Attempts[0].Attempt == ticket && !res.Attempts[0].Superseded, "real run ticket identity")
			c.Check(!o.Skipped && !o.Canceled && o.EffectiveExitCode() == 0, "real run exits cleanly")
			c.Check(o.Execs >= 20, "real run reports at least the 20x executions, got %d", o.Execs)

			events := log.snapshot()
			c.Check(len(events) > 0, "the real engine's lines are delivered as events")
			sawProgress := false
			for _, ev := range events { //nolint:gocritic // rangeValCopy: FuzzEvent is a fixed-size value type
				c.Check(ev.Attempt == ticket, "every event belongs to the admitted ticket")
				c.Check(ev.Stream == "stdout" || ev.Stream == "stderr", "real event stream label")
				if strings.HasPrefix(ev.Text, "fuzz: elapsed:") {
					sawProgress = true
				}
			}
			c.Check(sawProgress, "a real engine progress line is delivered as a payload")
			assertSequences(c, events, ticket)

			// The original batch API keeps its behavior on the same module.
			batch := gotestrunner.RunFuzzTargets(context.Background(),
				[]gotestrunner.FuzzTarget{{Package: ".", Dir: echoDir, Func: "FuzzEcho", Suite: "Echo"}},
				gotestrunner.FuzzRunConfig{Jobs: 1, BuildFlags: []string{"-fuzztime=20x", "-parallel=1"}})
			c.Check(len(batch.Outcomes) == 1 && batch.ExitCode() == 0 && batch.Outcomes[0].Execs >= 20,
				"RunFuzzTargets remains compatible on the benign target")
		})

		// Phase 2: a half stdout line and a complete stderr line, then the
		// stdout line completed after a release barrier — partial-line
		// behavior and per-stream order across both streams.
		t.Run("half lines across two streams", func(t *testing.T) {
			t.Setenv(helperModeEnv, "halves")
			log := &eventCollector{}
			session := gotestrunner.StartFuzzSession(context.Background(), gotestrunner.FuzzRunConfig{Jobs: 1}, log.sink)
			h1, err := session.Submit("h", h.target("FuzzHalves"))
			c.Check(err == nil, "halves submit: %v", err)
			// Gate on actual sink delivery, not merely the helper's write.
			log.waitFor(t, "stderr", "errline")

			early := log.snapshot()
			c.Check(len(early) == 1 && early[0].Stream == "stderr" && early[0].Text == "errline" && early[0].Sequence == 1,
				"the unterminated stdout half stays buffered while the complete stderr line is delivered")
			h.touch("release-FuzzHalves")
			res := session.Wait()
			c.Check(len(res.Attempts) == 1 && !res.Attempts[0].Outcome.Canceled, "half-line run completes")

			events := log.snapshot()
			c.Check(len(events) == 4, "exactly the completed lines are delivered, got %+v", events)
			var stdoutTexts, stderrTexts []string
			for _, ev := range events { //nolint:gocritic // rangeValCopy: FuzzEvent is a fixed-size value type
				c.Check(ev.Attempt == h1, "half-line event ticket identity")
				switch ev.Stream {
				case "stdout":
					stdoutTexts = append(stdoutTexts, ev.Text)
				case "stderr":
					stderrTexts = append(stderrTexts, ev.Text)
				}
			}
			// Per-stream order and the reassembled partial line are exact;
			// cross-stream interleaving is scheduled by the kernel and not
			// part of the contract.
			c.Check(strings.Join(stdoutTexts, "|") == "partial-joined|more", "stdout reassembles and orders its lines: %v", stdoutTexts)
			c.Check(strings.Join(stderrTexts, "|") == "errline|err2", "stderr keeps its order: %v", stderrTexts)
			assertSequences(c, events, h1)
		})

		// Phase 3: the engine's re-run advice is filtered exactly as the
		// console line writer filters it, while the FAIL line survives.
		t.Run("filtered advice payload", func(t *testing.T) {
			t.Setenv(helperModeEnv, "advice")
			log := &eventCollector{}
			session := gotestrunner.StartFuzzSession(context.Background(), gotestrunner.FuzzRunConfig{Jobs: 1}, log.sink)
			a1, err := session.Submit("a", h.target("FuzzAdvice"))
			c.Check(err == nil, "advice submit: %v", err)
			session.Wait()
			texts := eventTexts(log.snapshot(), a1)
			c.Check(slices.Contains(texts, "FAIL"), "the FAIL line is delivered")
			for _, text := range texts {
				c.Check(text != "To re-run:" && !strings.HasPrefix(text, "go test -run="),
					"re-run advice is filtered, got %q", text)
			}
		})

		// Phase 4: a nil sink suppresses live output without changing results.
		t.Run("nil sink", func(t *testing.T) {
			t.Setenv(helperModeEnv, "block")
			session := gotestrunner.StartFuzzSession(context.Background(), gotestrunner.FuzzRunConfig{Jobs: 1}, nil)
			n1, err := session.Submit("n", h.target("FuzzNil"))
			c.Check(err == nil, "nil-sink submit: %v", err)
			h.waitForReady("FuzzNil")
			h.stop("FuzzNil")
			res := session.Wait()
			c.Check(len(res.Attempts) == 1 && res.Attempts[0].Attempt == n1 && res.Attempts[0].Outcome.EffectiveExitCode() == 0,
				"a nil sink still produces the full result")
		})
	})
}
