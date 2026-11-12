// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package fuzzer

import (
	"context"
	"testing"

	"github.com/google/syzkaller/pkg/flatrpc"
	"github.com/google/syzkaller/pkg/fuzzer/queue"
	"github.com/google/syzkaller/prog"
	"github.com/google/syzkaller/sys/targets"
	"github.com/stretchr/testify/require"
)

// hintsPagingExecutor is a deterministic in-process executor: it replies to
// comparison collection requests with a fixed set of comparisons and to hint
// mutant executions with an empty result (or a crash at the stopAt-th hint
// execution, 1-based). It counts requests and detects any request submitted
// after a stop.
type hintsPagingExecutor struct {
	comps     []*flatrpc.Comparison
	stopAt    int
	seedReqs  int
	hintReqs  int
	extraReqs int
	stopped   bool
}

func (e *hintsPagingExecutor) Submit(req *queue.Request) {
	if e.stopped {
		e.extraReqs++
	}
	switch {
	case req.ExecOpts.ExecFlags&flatrpc.ExecFlagCollectComps != 0:
		e.seedReqs++
		req.Done(&queue.Result{Info: &flatrpc.ProgInfo{
			Calls: []*flatrpc.CallInfo{{Comps: e.comps}},
		}})
	case req.ExecOpts.ExecFlags&flatrpc.ExecFlagCollectSignal != 0:
		e.hintReqs++
		if e.stopAt != 0 && e.hintReqs == e.stopAt {
			e.stopped = true
			req.Done(&queue.Result{Status: queue.Crashed})
			return
		}
		req.Done(&queue.Result{Info: &flatrpc.ProgInfo{
			Calls: []*flatrpc.CallInfo{{}},
		}})
	default:
		e.extraReqs++
		req.Done(&queue.Result{Status: queue.ExecFailure})
	}
}

func TestHintsJobPaging(t *testing.T) {
	target, err := prog.GetTarget(targets.TestOS, targets.TestArch64Fuzz)
	require.NoError(t, err)
	p, err := target.Deserialize([]byte(`test$hint_data(&AUTO="ababababababababab")`), prog.Strict)
	require.NoError(t, err)

	comps := []*flatrpc.Comparison{
		{Pc: 0x10, Op1: 0xab, Op2: 0x11, IsConst: true},
		{Pc: 0x20, Op1: 0xabab, Op2: 0x2222, IsConst: true},
		{Pc: 0x30, Op1: 0xabababab, Op2: 0x33333333, IsConst: true},
		{Pc: 0x40, Op1: 0xabababababababab, Op2: 0x4444444444444444, IsConst: true},
	}
	compMap := make(prog.CompMap)
	for _, c := range comps {
		compMap.Add(c.Pc, c.Op1, c.Op2, c.IsConst)
	}
	// The fixture must produce enough mutants to span several pages of 16.
	total := 0
	p.MutateWithHints(0, compMap, func(*prog.Prog) bool {
		total++
		return true
	})
	require.Equal(t, 25, total)

	newFuzzer := func() *Fuzzer {
		return &Fuzzer{
			Stats: newStats(target),
			Config: &Config{
				NewInputFilter: func(string) bool { return true },
			},
			Cover: newCover(),
			ctx:   context.Background(),
		}
	}

	t.Run("full", func(t *testing.T) {
		fuzzer := newFuzzer()
		exec := &hintsPagingExecutor{comps: comps}
		job := &hintsJob{exec: exec, p: p.Clone(), call: 0, info: &JobInfo{}}
		job.run(fuzzer)
		// Comparisons are collected exactly 3 times, intersected once,
		// and then every mutant is executed exactly once across the pages.
		require.Equal(t, 3, exec.seedReqs)
		require.Equal(t, total, exec.hintReqs)
		require.Equal(t, 0, exec.extraReqs)
		require.Equal(t, 3, fuzzer.statExecSeed.Val())
		require.Equal(t, total, fuzzer.statExecHint.Val())
		require.Equal(t, int32(3+total), job.info.Execs.Load())
	})

	t.Run("stop", func(t *testing.T) {
		fuzzer := newFuzzer()
		// Crash the first mutant of the second page (page size is 16).
		exec := &hintsPagingExecutor{comps: comps, stopAt: 17}
		job := &hintsJob{exec: exec, p: p.Clone(), call: 0, info: &JobInfo{}}
		job.run(fuzzer)
		// The remaining mutants of the page are dropped and no further
		// pages are requested or executed.
		require.Equal(t, 3, exec.seedReqs)
		require.Equal(t, 17, exec.hintReqs)
		require.Equal(t, 0, exec.extraReqs)
		require.Equal(t, 3, fuzzer.statExecSeed.Val())
		require.Equal(t, 17, fuzzer.statExecHint.Val())
		require.Equal(t, int32(3+17), job.info.Execs.Load())
	})

	t.Run("no_comps", func(t *testing.T) {
		fuzzer := newFuzzer()
		exec := &hintsPagingExecutor{}
		job := &hintsJob{exec: exec, p: p.Clone(), call: 0, info: &JobInfo{}}
		job.run(fuzzer)
		// Without comparisons the job finishes normally after the
		// three seed executions.
		require.Equal(t, 3, exec.seedReqs)
		require.Equal(t, 0, exec.hintReqs)
		require.Equal(t, 0, exec.extraReqs)
		require.Equal(t, 3, fuzzer.statExecSeed.Val())
		require.Equal(t, 0, fuzzer.statExecHint.Val())
		require.Equal(t, int32(3), job.info.Execs.Load())
	})
}
