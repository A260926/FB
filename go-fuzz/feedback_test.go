package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
	"time"

	. "github.com/dvyukov/go-fuzz/go-fuzz-defs"
)

// These tests exercise the real Worker/TestBinary/go-fuzz-dep subprocess
// pipeline against the deterministic target in baseline/testee.

func newFeedbackHub() *Hub {
	h := &Hub{
		newInputC:  make(chan Input, 16),
		pending:    make(map[Sig][]byte),
		corpusSigs: make(map[Sig]struct{}),
	}
	h.ro.Store(&ROData{
		corpusCover:  make([]byte, CoverSize),
		badInputs:    make(map[Sig]struct{}),
		suppressions: make(map[Sig]struct{}),
	})
	h.maxCover.Store(make([]byte, CoverSize))
	return h
}

func newFeedbackWorker(t *testing.T, h *Hub) *Worker {
	t.Helper()
	name := os.Getenv("CHECK_TESTEE")
	if name == "" {
		t.Fatal("build baseline/testee and set CHECK_TESTEE")
	}
	name, err := filepath.Abs(name)
	if err != nil {
		t.Fatal(err)
	}
	w := &Worker{hub: h}
	w.coverBin = newTestBinary(name, func() {}, &w.stats, 0)
	t.Cleanup(w.coverBin.close)
	return w
}

func recvInput(t *testing.T, h *Hub) Input {
	t.Helper()
	select {
	case inp := <-h.newInputC:
		return inp
	case <-time.After(10 * time.Second):
		t.Fatal("no input published")
		return Input{}
	}
}

func noInput(t *testing.T, h *Hub) {
	t.Helper()
	select {
	case inp := <-h.newInputC:
		t.Fatalf("unexpected input published: [%v]%q", len(inp.data), inp.data)
	default:
	}
}

func TestStableFeedbackReplay(t *testing.T) {
	t.Run("stable", func(t *testing.T) {
		h := newFeedbackHub()
		w := newFeedbackWorker(t, h)
		w.triageInput(CoordinatorInput{Data: []byte("s"), Minimized: true, Smashed: true})
		inp := recvInput(t, h)
		if string(inp.data) != "s" || inp.cover[1] != 1 || inp.cover[2] != 0 || inp.res != 0 {
			t.Fatalf("bad stable input: data=%q cover1=%v cover2=%v res=%v",
				inp.data, inp.cover[1], inp.cover[2], inp.res)
		}
		if inp.execTime == 0 || inp.execTime == 1<<60 {
			t.Fatalf("bad exec time: %v", inp.execTime)
		}
		// The published cover must be a private copy: later executions
		// overwrite the shared memory region and must not clobber it.
		_, _, cover, _, _, crashed, _ := w.coverBin.test([]byte("g"))
		if crashed || cover[6] != 1 {
			t.Fatal("control execution failed")
		}
		if inp.cover[6] != 0 {
			t.Fatal("published cover aliases shared cover memory")
		}
	})

	t.Run("fluctuating", func(t *testing.T) {
		h := newFeedbackHub()
		w := newFeedbackWorker(t, h)
		// 'f' covers block 3 with counters 1,1,0 on consecutive runs;
		// the slot-wise minimum is 0, so the block must not be published.
		w.triageInput(CoordinatorInput{Data: []byte("f"), Minimized: true, Smashed: true})
		inp := recvInput(t, h)
		if inp.cover[3] != 0 {
			t.Fatalf("fluctuating block published as covered: %v", inp.cover[3])
		}
	})

	t.Run("negative", func(t *testing.T) {
		h := newFeedbackHub()
		w := newFeedbackWorker(t, h)
		// 'n' always returns -1: the candidate must not enter the corpus.
		w.triageInput(CoordinatorInput{Data: []byte("n"), Minimized: true, Smashed: true})
		noInput(t, h)
	})

	t.Run("negative_on_replay", func(t *testing.T) {
		h := newFeedbackHub()
		w := newFeedbackWorker(t, h)
		// 'N' returns 0,0,-1 on consecutive runs: a single negative
		// observation out of three rejects the candidate.
		w.triageInput(CoordinatorInput{Data: []byte("N"), Minimized: true, Smashed: true})
		noInput(t, h)
	})

	t.Run("empty", func(t *testing.T) {
		h := newFeedbackHub()
		w := newFeedbackWorker(t, h)
		// Empty input is a legitimate candidate.
		w.triageInput(CoordinatorInput{Data: nil, Minimized: true, Smashed: true})
		inp := recvInput(t, h)
		if len(inp.data) != 0 || !bytes.Equal(inp.cover, make([]byte, CoverSize)) {
			t.Fatalf("bad empty input: data=%q cover nonzero=%v", inp.data, !bytes.Equal(inp.cover, make([]byte, CoverSize)))
		}
	})
}

func TestStableFeedbackMinimize(t *testing.T) {
	oldMin := *flagMinimize
	t.Cleanup(func() { *flagMinimize = oldMin })

	t.Run("metrics_match_final_bytes", func(t *testing.T) {
		*flagMinimize = time.Second
		h := newFeedbackHub()
		w := newFeedbackWorker(t, h)
		ro := h.ro.Load().(*ROData)
		ro.corpusCover[2] = 255 // this auxiliary block is already covered elsewhere
		w.triageInput(CoordinatorInput{Data: []byte("sZZ")})
		inp := recvInput(t, h)
		if string(inp.data) != "s" {
			t.Fatalf("expected minimization to %q, got %q", "s", inp.data)
		}
		// Block 2 only fires for inputs longer than 1 byte; the published
		// cover must describe the final bytes, not the original ones.
		if inp.cover[1] != 1 || inp.cover[2] != 0 {
			t.Fatalf("stale cover after minimize: block1=%v block2=%v", inp.cover[1], inp.cover[2])
		}
		if inp.res != 0 || inp.execTime == 0 || inp.execTime == 1<<60 {
			t.Fatalf("bad metrics: res=%v execTime=%v", inp.res, inp.execTime)
		}
		// Cross-check: re-observing the published bytes reproduces the
		// published evidence exactly.
		obs, ok := w.observeStable(inp.data, &w.execs[execTriageInput])
		if !ok || obs.res != inp.res || !bytes.Equal(obs.cover, inp.cover) {
			t.Fatalf("published metrics do not match final bytes: ok=%v res=%v coverEqual=%v",
				ok, obs.res, bytes.Equal(obs.cover, inp.cover))
		}
	})

	t.Run("budget_exhausted", func(t *testing.T) {
		*flagMinimize = 0 // minimize budget expires immediately
		h := newFeedbackHub()
		w := newFeedbackWorker(t, h)
		w.triageInput(CoordinatorInput{Data: []byte("sZZ")})
		inp := recvInput(t, h)
		// The search stops at the original candidate, which already has
		// complete evidence; the metrics must match those bytes.
		if string(inp.data) != "sZZ" {
			t.Fatalf("expected search to stop at original candidate, got %q", inp.data)
		}
		obs, ok := w.observeStable(inp.data, &w.execs[execTriageInput])
		if !ok || obs.res != inp.res || !bytes.Equal(obs.cover, inp.cover) {
			t.Fatal("published metrics do not match final bytes after budget expiry")
		}
	})
}

func TestStableFeedbackAdmission(t *testing.T) {
	t.Run("withdraw_allows_stable_replacement", func(t *testing.T) {
		h := newFeedbackHub()
		w := newFeedbackWorker(t, h)
		// A candidate reserves block 6, then crashes during triage.
		w.testInput([]byte("c"), 0, execFuzz)
		if len(w.triageQueue) != 1 {
			t.Fatal("initial candidate was not queued")
		}
		if h.maxCover.Load().([]byte)[6] == 0 {
			t.Fatal("no speculative reservation recorded")
		}
		input := w.triageQueue[0]
		w.triageQueue = nil
		w.triageInput(input) // the next real subprocess execution crashes
		if len(w.crasherQueue) != 1 {
			t.Fatal("crash was not routed to the crasher queue")
		}
		noInput(t, h)
		// The failed candidate's reservation is withdrawn...
		if got := h.maxCover.Load().([]byte)[6]; got != 0 {
			t.Fatalf("reservation not withdrawn: maxCover[6]=%v", got)
		}
		// ...so a stable input covering the same block is queued and
		// admitted instead of being blocked by the stale placeholder.
		w.testInput([]byte("g"), 0, execFuzz)
		if len(w.triageQueue) != 1 {
			t.Fatal("stable input blocked by stale reservation")
		}
		input = w.triageQueue[0]
		w.triageQueue = nil
		w.triageInput(input)
		inp := recvInput(t, h)
		if inp.cover[6] != 1 {
			t.Fatalf("stable cover not published: %v", inp.cover[6])
		}
	})

	t.Run("negative_withdraws", func(t *testing.T) {
		h := newFeedbackHub()
		w := newFeedbackWorker(t, h)
		// The first observation of 'N' succeeds and reserves block 5,
		// but the replay observes a negative result.
		w.testInput([]byte("N"), 0, execFuzz)
		if len(w.triageQueue) != 1 {
			t.Fatal("candidate was not queued")
		}
		if h.maxCover.Load().([]byte)[5] == 0 {
			t.Fatal("no speculative reservation recorded")
		}
		input := w.triageQueue[0]
		w.triageQueue = nil
		w.triageInput(input)
		noInput(t, h)
		if len(w.crasherQueue) != 0 {
			t.Fatal("negative result must not be treated as a crash")
		}
		if got := h.maxCover.Load().([]byte)[5]; got != 0 {
			t.Fatalf("reservation not withdrawn: maxCover[5]=%v", got)
		}
	})

	t.Run("interleaved_workers", func(t *testing.T) {
		h := newFeedbackHub()
		w1 := newFeedbackWorker(t, h)
		w2 := newFeedbackWorker(t, h)
		// Worker 1 reserves block 6 (counter bucket 1) with a candidate
		// that will crash during triage.
		w1.testInput([]byte("c"), 0, execFuzz)
		// Worker 2 reserves the same block with a higher counter bucket.
		w2.testInput([]byte("G"), 0, execFuzz)
		if len(w1.triageQueue) != 1 || len(w2.triageQueue) != 1 {
			t.Fatalf("candidates not queued: %v %v", len(w1.triageQueue), len(w2.triageQueue))
		}
		// The same unconfirmed candidate appearing again (even on another
		// worker) is still queued only once.
		w2.testInput([]byte("c"), 0, execFuzz)
		if len(w2.triageQueue) != 1 {
			t.Fatal("duplicate candidate queued")
		}
		if got := h.maxCover.Load().([]byte)[6]; got != 3 {
			t.Fatalf("reservations not merged: maxCover[6]=%v", got)
		}
		// Worker 1's candidate crashes on re-observation; withdrawing it
		// must keep worker 2's reservation for the same slot intact.
		input1 := w1.triageQueue[0]
		w1.triageQueue = nil
		w1.triageInput(input1)
		if len(w1.crasherQueue) != 1 {
			t.Fatal("crash was not routed to the crasher queue")
		}
		if got := h.maxCover.Load().([]byte)[6]; got != 3 {
			t.Fatalf("other candidate's reservation lost: maxCover[6]=%v", got)
		}
		// Worker 2's candidate confirms; the coverage becomes permanent.
		input2 := w2.triageQueue[0]
		w2.triageQueue = nil
		w2.triageInput(input2)
		inp := recvInput(t, h)
		inp.mine = false // this test has no coordinator to notify
		if !h.handleNewInput(inp) {
			t.Fatal("hub loop terminated")
		}
		ro := h.ro.Load().(*ROData)
		if ro.corpusCover[6] != 3 {
			t.Fatalf("confirmed cover wrong: %v", ro.corpusCover[6])
		}
		if len(h.pending) != 0 {
			t.Fatal("placeholder leaked after confirmation")
		}
		if got := h.maxCover.Load().([]byte)[6]; got != 3 {
			t.Fatalf("confirmed cover not reflected: maxCover[6]=%v", got)
		}
		// A lower-bucket input for the same block is no longer new.
		w1.testInput([]byte("g"), 0, execFuzz)
		if len(w1.triageQueue) != 0 {
			t.Fatal("covered block queued again")
		}
	})
}
