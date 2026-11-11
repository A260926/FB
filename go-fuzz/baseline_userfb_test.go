package main

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"testing"
	"time"

	. "github.com/dvyukov/go-fuzz/go-fuzz-defs"
)

// This test records the starting behavior; it is not a regression for the requested change.
func TestUserFBBaseline(t *testing.T) {
	name := os.Getenv("USERFB_TESTEE")
	if name == "" {
		t.Fatal("build userfb_baseline/testee and set USERFB_TESTEE")
	}
	name, err := filepath.Abs(name)
	if err != nil {
		t.Fatal(err)
	}
	oldMin := *flagMinimize
	*flagMinimize = time.Second
	t.Cleanup(func() { *flagMinimize = oldMin })
	newWorker := func() *Worker {
		h := &Hub{newInputC: make(chan Input, 4)}
		h.ro.Store(&ROData{corpusCover: make([]byte, CoverSize), badInputs: make(map[Sig]struct{}), suppressions: make(map[Sig]struct{})})
		h.maxCover.Store(make([]byte, CoverSize))
		w := &Worker{hub: h}
		w.coverBin = newTestBinary(name, func() {}, &w.stats, 0)
		t.Cleanup(w.coverBin.close)
		return w
	}
	t.Run("normal", func(t *testing.T) {
		w := newWorker()
		res, _, cover, _, _, crash, hang := w.coverBin.test([]byte("s"))
		if res != 0 || crash || hang || cover[1] != 1 || cover[2] != 0 {
			t.Fatal("normal execution failed")
		}
		w.noteNewInput([]byte("s"), cover, res, 0, execFuzz)
		w.noteNewInput([]byte("s"), cover, res, 0, execFuzz)
		if len(w.triageQueue) != 1 {
			t.Fatal("duplicate queued")
		}
		_, _, cover, _, _, crash, _ = w.coverBin.test(nil)
		if crash || !bytes.Equal(cover, make([]byte, CoverSize)) {
			t.Fatal("empty input boundary failed")
		}
		fmt.Println("BASELINE_NORMAL stable=1 duplicate_queue=1 empty_cover=0")
	})
	t.Run("unstable", func(t *testing.T) {
		w := newWorker()
		w.triageInput(CoordinatorInput{Data: []byte("f"), Minimized: true, Smashed: true})
		inp := <-w.hub.newInputC
		if inp.cover[3] != 1 {
			t.Fatalf("expected starting max-of-three behavior: %d", inp.cover[3])
		}
		fmt.Println("BASELINE_UNSTABLE observations=1,1,0 published=1")
	})
	t.Run("stale_after_minimize", func(t *testing.T) {
		w := newWorker()
		ro := w.hub.ro.Load().(*ROData)
		ro.corpusCover[2] = 255 // this auxiliary block is already covered elsewhere
		w.triageInput(CoordinatorInput{Data: []byte("sZZ")})
		inp := <-w.hub.newInputC
		published := inp.cover[2]
		_, _, actual, _, _, crashed, _ := w.coverBin.test(inp.data)
		if string(inp.data) != "s" || published != 1 || actual[2] != 0 || crashed {
			t.Fatal("starting stale metadata witness changed")
		}
		fmt.Println("BASELINE_STALE original=sZZ final=s published_block2=1 actual_block2=0")
	})
	t.Run("negative_result", func(t *testing.T) {
		w := newWorker()
		w.triageInput(CoordinatorInput{Data: []byte("n"), Minimized: true, Smashed: true})
		inp := <-w.hub.newInputC
		if inp.res != 0 || inp.cover[4] != 1 {
			t.Fatal("starting negative result witness changed")
		}
		fmt.Println("BASELINE_NEGATIVE fuzz_result=-1 published_result=0")
	})
	t.Run("poisoned_admission", func(t *testing.T) {
		w := newWorker()
		w.testInput([]byte("c"), 0, execFuzz)
		if len(w.triageQueue) != 1 {
			t.Fatal("initial candidate was not queued")
		}
		input := w.triageQueue[0]
		w.triageQueue = nil
		w.triageInput(input) // the next real subprocess execution crashes
		if len(w.crasherQueue) != 1 || len(w.hub.newInputC) != 0 {
			t.Fatal("crash was not routed")
		}
		w.testInput([]byte("g"), 0, execFuzz)
		if len(w.triageQueue) != 0 || w.hub.maxCover.Load().([]byte)[6] != 1 {
			t.Fatal("starting admission witness changed")
		}
		fmt.Println("BASELINE_POISON rejected_crash=1 later_stable_queue=0 pending_block6=1")
	})
}
