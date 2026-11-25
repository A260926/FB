package fuzz

import (
	"bytes"
	"crypto/sha256"
	"testing"

	"fuzzreplay.local/checkpoint/task_support/taskcheck"
)

// replayTestEntry returns a self-contained corpus entry backed by Values.
func replayTestEntry(s string) CorpusEntry {
	return CorpusEntry{Values: []any{[]byte(s)}, IsSeed: true}
}

// replayTestCoverage derives deterministic, entry-dependent feedback. It is
// independent of the session so two runs can be compared.
func replayTestCoverage(entry CorpusEntry, size int) []byte {
	sum := sha256.Sum256(entry.Data)
	cov := make([]byte, size)
	for i := range cov {
		cov[i] = sum[i%len(sum)]
	}
	return cov
}

// replayTestDrive runs a session to completion, dispatching in batches and
// accepting each batch in reverse dispatch order.
func replayTestDrive(s *ReplaySession, total uint64, batch, covSize int) (ReplayState, error) {
	for {
		var leases []ReplayLease
		for i := 0; i < batch; i++ {
			lease, ok, err := s.Next()
			if err != nil {
				return ReplayState{}, err
			}
			if !ok {
				break
			}
			leases = append(leases, lease)
		}
		if len(leases) == 0 {
			st := s.State()
			if st.Committed == total {
				return st, nil
			}
			return ReplayState{}, errReplayTestStalled
		}
		for i := len(leases) - 1; i >= 0; i-- {
			if _, err := s.Accept(leases[i], replayTestCoverage(leases[i].Entry, covSize)); err != nil {
				return ReplayState{}, err
			}
		}
	}
}

// replayOK records a check and returns its result so tests can guard on it.
func replayOK(c *taskcheck.C, cond bool, format string, args ...any) bool {
	c.Check(cond, format, args...)
	return cond
}

var errReplayTestStalled = &replayTestError{"replay stalled before completion"}

type replayTestError struct{ msg string }

func (e *replayTestError) Error() string { return e.msg }

func replayTestTotal(plan ReplayPlan) uint64 {
	var n uint64
	for _, stage := range plan.Stages {
		n += uint64(len(stage))
	}
	return n
}

func replayTestSameState(a, b ReplayState) bool {
	return a.Stage == b.Stage &&
		a.Committed == b.Committed &&
		a.Pending == b.Pending &&
		a.CorpusEntries == b.CorpusEntries &&
		bytes.Equal(a.Coverage, b.Coverage)
}

// orCoverage returns the union of every coverage slice.
func orCoverage(size int, covs ...[]byte) []byte {
	out := make([]byte, size)
	for _, cov := range covs {
		for i := range out {
			out[i] |= cov[i]
		}
	}
	return out
}

func TestReplayBarrier(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Validation of plan-level constraints.
		if _, err := NewReplaySession(ReplayPlan{BuildID: "", CoverageSize: 1}); err == nil {
			c.Check(false, "empty BuildID should be rejected")
		} else {
			c.Check(true, "")
		}
		if _, err := NewReplaySession(ReplayPlan{BuildID: "b", CoverageSize: 0}); err == nil {
			c.Check(false, "CoverageSize 0 should be rejected")
		} else {
			c.Check(true, "")
		}
		if _, err := NewReplaySession(ReplayPlan{BuildID: "b", CoverageSize: 65537}); err == nil {
			c.Check(false, "CoverageSize 65537 should be rejected")
		} else {
			c.Check(true, "")
		}
		if _, err := NewReplaySession(ReplayPlan{BuildID: "b", CoverageSize: 2, Stages: [][]CorpusEntry{{
			{Path: "does-not-exist"},
		}}}); err == nil {
			c.Check(false, "Path-only entry should be rejected")
		} else {
			c.Check(true, "")
		}
		if _, err := NewReplaySession(ReplayPlan{BuildID: "b", CoverageSize: 2, Stages: [][]CorpusEntry{
			{replayTestEntry("x")},
			{{Values: []any{"not-bytes", int64(1)}}},
		}}); err == nil {
			c.Check(false, "mixed parameter types should be rejected")
		} else {
			c.Check(true, "")
		}
		if _, err := NewReplaySession(ReplayPlan{BuildID: "b", CoverageSize: 2, Stages: [][]CorpusEntry{{
			{Data: []byte("not a corpus file")},
		}}}); err == nil {
			c.Check(false, "malformed Data should be rejected")
		} else {
			c.Check(true, "")
		}

		plan := ReplayPlan{
			BuildID:      "barrier-build",
			CoverageSize: 4,
			Stages: [][]CorpusEntry{
				{replayTestEntry("a0"), replayTestEntry("a1")},
				{replayTestEntry("b0")},
				{replayTestEntry("c0"), replayTestEntry("c1")},
			},
		}
		s, err := NewReplaySession(plan)
		if !replayOK(c, err == nil, "new session: %v", err) {
			return
		}

		l1, ok, err := s.Next()
		c.Check(err == nil && ok && l1.Sequence == 1 && l1.Stage == 0 && l1.Epoch == 1,
			"first dispatch: %+v ok=%v err=%v", l1, ok, err)
		l2, ok, err := s.Next()
		c.Check(err == nil && ok && l2.Sequence == 2 && l2.Stage == 0,
			"second dispatch: %+v ok=%v err=%v", l2, ok, err)
		_, ok, err = s.Next()
		c.Check(err == nil && !ok, "stage 0 must be fully dispatched, got ok=%v err=%v", ok, err)

		cov1 := replayTestCoverage(l1.Entry, 4)
		cov2 := replayTestCoverage(l2.Entry, 4)

		// Out-of-order feedback is buffered and changes nothing.
		accepted, err := s.Accept(l2, cov2)
		c.Check(err == nil && accepted, "buffer seq 2: accepted=%v err=%v", accepted, err)
		st := s.State()
		c.Check(st.Stage == 0 && st.Committed == 0 && st.Pending == 2 && st.CorpusEntries == 0,
			"buffered feedback must not commit: %+v", st)
		c.Check(bytes.Equal(st.Coverage, make([]byte, 4)), "buffered feedback must not change coverage: %x", st.Coverage)

		// Committing the in-order predecessor releases both and exposes stage 1.
		accepted, err = s.Accept(l1, cov1)
		c.Check(err == nil && accepted, "accept seq 1: accepted=%v err=%v", accepted, err)
		st = s.State()
		c.Check(st.Stage == 1 && st.Committed == 2 && st.Pending == 0 && st.CorpusEntries == 2,
			"stage barrier release: %+v", st)

		// Stage 1 is now visible; dispatch and commit it.
		l3, ok, err := s.Next()
		c.Check(err == nil && ok && l3.Sequence == 3 && l3.Stage == 1,
			"stage 1 dispatch: %+v ok=%v err=%v", l3, ok, err)
		cov3 := replayTestCoverage(l3.Entry, 4)
		if _, err := s.Accept(l3, cov3); err != nil {
			c.Check(false, "accept seq 3: %v", err)
		}
		st = s.State()
		c.Check(st.Stage == 2 && st.Committed == 3 && st.Pending == 0,
			"stage 2 visibility: %+v", st)

		l4, ok, err := s.Next()
		c.Check(err == nil && ok && l4.Sequence == 4 && l4.Stage == 2,
			"stage 2 first dispatch: %+v ok=%v err=%v", l4, ok, err)
		l5, ok, err := s.Next()
		c.Check(err == nil && ok && l5.Sequence == 5 && l5.Stage == 2,
			"stage 2 second dispatch: %+v ok=%v err=%v", l5, ok, err)
		_, ok, err = s.Next()
		c.Check(err == nil && !ok, "final stage must be fully dispatched, got ok=%v err=%v", ok, err)

		cov4 := replayTestCoverage(l4.Entry, 4)
		cov5 := replayTestCoverage(l5.Entry, 4)
		if _, err := s.Accept(l5, cov5); err != nil {
			c.Check(false, "buffer seq 5: %v", err)
		}
		st = s.State()
		c.Check(st.Committed == 3 && st.Pending == 2, "out-of-order tail must stay buffered: %+v", st)
		if _, err := s.Accept(l4, cov4); err != nil {
			c.Check(false, "commit seq 4: %v", err)
		}

		st = s.State()
		c.Check(st.Stage == 3 && st.Committed == 5 && st.Pending == 0 && st.CorpusEntries == 5,
			"completed state: %+v", st)
		c.Check(bytes.Equal(st.Coverage, orCoverage(4, cov1, cov2, cov3, cov4, cov5)),
			"completed coverage: %x", st.Coverage)
		_, ok, err = s.Next()
		c.Check(err == nil && !ok, "Next after completion must be false, got ok=%v err=%v", ok, err)

		// Empty stages are skipped entirely.
		empty := ReplayPlan{
			BuildID:      "empty-stages",
			CoverageSize: 2,
			Stages: [][]CorpusEntry{
				{},
				{replayTestEntry("e0")},
				{},
				{replayTestEntry("e1")},
				{replayTestEntry("e2")},
				{},
			},
		}
		es, err := NewReplaySession(empty)
		if !replayOK(c, err == nil, "empty-stage session: %v", err) {
			return
		}
		var got []int
		for {
			lease, ok, err := es.Next()
			if err != nil {
				c.Check(false, "empty-stage Next: %v", err)
				return
			}
			if !ok {
				if es.State().Committed == 3 {
					break
				}
				c.Check(false, "empty-stage replay stalled: %+v", es.State())
				return
			}
			got = append(got, lease.Stage)
			if _, err := es.Accept(lease, replayTestCoverage(lease.Entry, 2)); err != nil {
				c.Check(false, "empty-stage Accept seq %d: %v", lease.Sequence, err)
				return
			}
		}
		c.Check(len(got) == 3 && got[0] == 1 && got[1] == 3 && got[2] == 4,
			"empty stages should be skipped, stages=%v", got)
		c.Check(es.State().Stage == 6, "final stage after empty stages: %+v", es.State())
	})
}

func TestReplayFeedback(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Two distinct sequences with identical bytes must both execute and
		// count, but only the first contributes to the corpus.
		plan := ReplayPlan{
			BuildID:      "feedback-build",
			CoverageSize: 3,
			Stages: [][]CorpusEntry{{
				replayTestEntry("identical"),
				replayTestEntry("identical"),
			}},
		}
		s, err := NewReplaySession(plan)
		if !replayOK(c, err == nil, "new session: %v", err) {
			return
		}
		l1, ok, err := s.Next()
		c.Check(err == nil && ok && l1.Sequence == 1, "seq 1 dispatch: %+v ok=%v err=%v", l1, ok, err)
		l2, ok, err := s.Next()
		c.Check(err == nil && ok && l2.Sequence == 2, "seq 2 dispatch: %+v ok=%v err=%v", l2, ok, err)
		if !replayOK(c, l1.Entry.Data != nil && bytes.Equal(l1.Entry.Data, l2.Entry.Data),
			"same-byte entries: %q vs %q", l1.Entry.Data, l2.Entry.Data) {
			return
		}
		cov := replayTestCoverage(l1.Entry, 3)
		if accepted, err := s.Accept(l2, cov); !replayOK(c, err == nil && accepted, "buffer seq 2: accepted=%v err=%v", accepted, err) {
			return
		}
		if accepted, err := s.Accept(l1, cov); !replayOK(c, err == nil && accepted, "accept seq 1: accepted=%v err=%v", accepted, err) {
			return
		}
		st := s.State()
		c.Check(st.Committed == 2 && st.CorpusEntries == 1,
			"same bytes must count twice but dedupe in corpus: %+v", st)
		c.Check(bytes.Equal(st.Coverage, cov), "coverage after commits: %x", st.Coverage)

		// Exact duplicates are rejected without side effects.
		before := s.State()
		accepted, err := s.Accept(l1, cov)
		c.Check(err == nil && !accepted, "duplicate seq 1: accepted=%v err=%v", accepted, err)
		accepted, err = s.Accept(l2, cov)
		c.Check(err == nil && !accepted, "duplicate seq 2: accepted=%v err=%v", accepted, err)
		c.Check(replayTestSameState(before, s.State()), "duplicates changed state: before=%+v after=%+v", before, s.State())

		// Conflicting feedback for a known sequence is an error.
		other := make([]byte, 3)
		other[0] = 0xff
		_, err = s.Accept(l1, other)
		c.Check(err != nil, "conflicting bytes for seq 1 should error")
		c.Check(replayTestSameState(before, s.State()), "conflict changed state: %+v", s.State())

		// Unknown and stale sequences.
		_, err = s.Accept(ReplayLease{Epoch: 1, Sequence: 9, Stage: 0, Entry: l1.Entry}, cov)
		c.Check(err != nil, "unknown sequence should error")
		_, err = s.Accept(ReplayLease{Epoch: 1, Sequence: 0, Stage: 0, Entry: l1.Entry}, cov)
		c.Check(err != nil, "sequence 0 should error")
		_, err = s.Accept(ReplayLease{Epoch: 2, Sequence: 1, Stage: 0, Entry: l1.Entry}, cov)
		c.Check(err != nil, "wrong epoch should error")

		// Wrong coverage length.
		_, err = s.Accept(l1, []byte{1, 2})
		c.Check(err != nil, "short coverage should error")

		// Lease contents must match what was dispatched.
		tampered := l1
		tampered.Entry = cloneReplayCorpusEntry(l1.Entry)
		tampered.Entry.Data[0] ^= 0xff
		_, err = s.Accept(tampered, cov)
		c.Check(err != nil, "tampered lease entry should error")
		wrongStage := l1
		wrongStage.Stage = 4
		_, err = s.Accept(wrongStage, cov)
		c.Check(err != nil, "wrong stage should error")

		// Mutating a returned lease must not reach session state.
		if !replayOK(c, bytes.Equal(s.plan.works[0].entry.Data, l1.Entry.Data),
			"internal work mutated by caller") {
			return
		}
		origLeaseData := append([]byte(nil), l1.Entry.Data...)
		l1.Entry.Data[0] ^= 0xff
		c.Check(bytes.Equal(s.plan.works[0].entry.Data, origLeaseData),
			"mutating a lease entry changed the session: %q", s.plan.works[0].entry.Data)

		// Mutating a returned State coverage slice must not reach the session.
		snapBefore, err := s.Snapshot()
		if !replayOK(c, err == nil, "snapshot: %v", err) {
			return
		}
		mut := s.State()
		mut.Coverage[0] ^= 0xff
		snapAfter, err := s.Snapshot()
		c.Check(err == nil && bytes.Equal(snapBefore, snapAfter),
			"mutating State coverage changed the session (err=%v)", err)

		// Entry Data must win over Path: a valid self-contained entry with a
		// bogus path is accepted and never reads the filesystem.
		s2, err := NewReplaySession(ReplayPlan{
			BuildID:      "path-build",
			CoverageSize: 1,
			Stages: [][]CorpusEntry{{
				{Path: "definitely/missing/file", Data: marshalCorpusFile([]byte("payload"))},
			}},
		})
		if !replayOK(c, err == nil, "path-plus-data session: %v", err) {
			return
		}
		pl, ok, err := s2.Next()
		c.Check(err == nil && ok && bytes.Equal(pl.Entry.Data, marshalCorpusFile([]byte("payload"))),
			"Data should be used instead of Path: %+v ok=%v err=%v", pl, ok, err)
	})
}

func TestReplayRestart(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		plan := ReplayPlan{
			BuildID:      "restart-build",
			CoverageSize: 4,
			Stages: [][]CorpusEntry{
				{replayTestEntry("x"), replayTestEntry("y")},
				{replayTestEntry("x")},
				{replayTestEntry("z"), replayTestEntry("y")},
			},
		}
		total := replayTestTotal(plan)

		// Reference: the same plan without interruption.
		fullSession, err := NewReplaySession(plan)
		if !replayOK(c, err == nil, "full session: %v", err) {
			return
		}
		want, err := replayTestDrive(fullSession, total, 2, 4)
		if !replayOK(c, err == nil, "full drive: %v", err) {
			return
		}
		c.Check(want.Committed == total && want.Pending == 0 && want.Stage == 3,
			"full final state: %+v", want)
		c.Check(want.CorpusEntries == 3, "full corpus should dedupe x/y/z: %+v", want)

		// Interrupted: commit stages 0 and 1, dispatch stage 2, buffer only the
		// last result, then snapshot with work in flight.
		s, err := NewReplaySession(plan)
		if !replayOK(c, err == nil, "restart session: %v", err) {
			return
		}
		l1, ok, err := s.Next()
		l2, ok2, err2 := s.Next()
		if !replayOK(c, err == nil && err2 == nil && ok && ok2, "dispatch stage 0: %v %v", err, err2) {
			return
		}
		cov1 := replayTestCoverage(l1.Entry, 4)
		cov2 := replayTestCoverage(l2.Entry, 4)
		if _, err := s.Accept(l2, cov2); err != nil {
			c.Check(false, "accept seq 2: %v", err)
			return
		}
		if _, err := s.Accept(l1, cov1); err != nil {
			c.Check(false, "accept seq 1: %v", err)
			return
		}
		l3, ok, err := s.Next()
		if !replayOK(c, err == nil && ok && l3.Sequence == 3 && l3.Stage == 1, "dispatch seq 3: %+v", l3) {
			return
		}
		cov3 := replayTestCoverage(l3.Entry, 4)
		if _, err := s.Accept(l3, cov3); err != nil {
			c.Check(false, "accept seq 3: %v", err)
			return
		}
		l4, ok, err := s.Next()
		l5, ok2, err2 := s.Next()
		if !replayOK(c, err == nil && err2 == nil && ok && ok2 && l4.Sequence == 4 && l5.Sequence == 5,
			"dispatch stage 2: %+v %+v", l4, l5) {
			return
		}
		cov4 := replayTestCoverage(l4.Entry, 4)
		cov5 := replayTestCoverage(l5.Entry, 4)
		if _, err := s.Accept(l5, cov5); err != nil {
			c.Check(false, "buffer seq 5: %v", err)
			return
		}

		mid := s.State()
		c.Check(mid.Committed == 3 && mid.Pending == 2 && mid.Stage == 2,
			"mid-run state: %+v", mid)

		snap, err := s.Snapshot()
		if !replayOK(c, err == nil && len(snap) > 0, "snapshot: %v", err) {
			return
		}
		// Snapshot determinism: identical state, identical bytes.
		snap2, err := s.Snapshot()
		c.Check(err == nil && bytes.Equal(snap, snap2), "consecutive snapshots differ")
		// The returned buffer belongs to the caller.
		owned := append([]byte(nil), snap...)
		snap[0] ^= 0xff
		snap3, err := s.Snapshot()
		c.Check(err == nil && bytes.Equal(snap3, owned), "mutating a snapshot changed later snapshots")
		snap = owned

		// Corrupt, truncated, and trailing-data snapshots are all rejected.
		truncated := snap[:len(snap)-1]
		_, err = RestoreReplaySession(plan, truncated)
		c.Check(err != nil, "truncated snapshot should be rejected")
		corrupt := append([]byte(nil), snap...)
		corrupt[len(corrupt)/2] ^= 0x01
		_, err = RestoreReplaySession(plan, corrupt)
		c.Check(err != nil, "corrupt snapshot should be rejected")
		trailing := append(append([]byte(nil), snap...), 0x00)
		if _, err = RestoreReplaySession(plan, trailing); err == nil {
			c.Check(false, "trailing data should be rejected")
		} else {
			c.Check(true, "")
		}

		// Mismatched build, coverage size, or plan are rejected.
		otherBuild := plan
		otherBuild.BuildID = "some-other-build"
		_, err = RestoreReplaySession(otherBuild, snap)
		c.Check(err != nil, "different BuildID should be rejected")
		otherCov := plan
		otherCov.CoverageSize = 5
		_, err = RestoreReplaySession(otherCov, snap)
		c.Check(err != nil, "different CoverageSize should be rejected")
		otherPlan := ReplayPlan{
			BuildID:      plan.BuildID,
			CoverageSize: plan.CoverageSize,
			Stages: [][]CorpusEntry{
				{replayTestEntry("x"), replayTestEntry("y")},
				{replayTestEntry("x")},
				{replayTestEntry("z"), replayTestEntry("other")},
			},
		}
		_, err = RestoreReplaySession(otherPlan, snap)
		c.Check(err != nil, "different stage corpus should be rejected")

		// Failed restores must not have disturbed the original session.
		c.Check(replayTestSameState(mid, s.State()), "failed restores changed original session: %+v", s.State())
		again, err := s.Snapshot()
		c.Check(err == nil && bytes.Equal(again, snap), "failed restores changed original snapshots")

		// Restore for real.
		r, err := RestoreReplaySession(plan, snap)
		if !replayOK(c, err == nil, "restore: %v", err) {
			return
		}
		rst := r.State()
		c.Check(replayTestSameState(mid, rst), "restored state differs: mid=%+v restored=%+v", mid, rst)

		// Old leases are invalid; only the work without feedback is reissued.
		if _, err := r.Accept(l4, cov4); err == nil {
			c.Check(false, "old-epoch lease should be rejected")
		} else {
			c.Check(true, "")
		}
		rl, ok, err := r.Next()
		if !replayOK(c, err == nil && ok && rl.Sequence == 4 && rl.Stage == 2 && rl.Epoch > l4.Epoch,
			"redispatch of seq 4: %+v ok=%v err=%v (old epoch %d)", rl, ok, err, l4.Epoch) {
			return
		}
		_, ok, err = r.Next()
		c.Check(err == nil && !ok,
			"seq 5 already has feedback and must not be reissued, ok=%v err=%v", ok, err)
		c.Check(r.State().Pending == 2, "redispatch and buffered work stay pending: %+v", r.State())

		if _, err := r.Accept(rl, cov4); err != nil {
			c.Check(false, "accept redispatched seq 4: %v", err)
			return
		}
		// Commit of seq 4 releases the buffered seq 5 feedback.
		got := r.State()
		c.Check(got.Committed == 5 && got.Pending == 0, "post-restore commit: %+v", got)

		got, err = replayTestDrive(r, total, 2, 4)
		if !replayOK(c, err == nil, "restored drive: %v", err) {
			return
		}
		c.Check(replayTestSameState(want, got),
			"restarted replay diverged: want=%+v got=%+v", want, got)
		c.Check(got.CorpusEntries == want.CorpusEntries && bytes.Equal(got.Coverage, want.Coverage),
			"restarted corpus/coverage diverged")

		// A snapshot of a completed session can itself be restored.
		doneSnap, err := r.Snapshot()
		if !replayOK(c, err == nil, "completed snapshot: %v", err) {
			return
		}
		done, err := RestoreReplaySession(plan, doneSnap)
		if !replayOK(c, err == nil, "restore completed: %v", err) {
			return
		}
		_, ok, err = done.Next()
		c.Check(err == nil && !ok, "completed restore must have no work, ok=%v err=%v", ok, err)
		c.Check(replayTestSameState(want, done.State()), "completed restore state: %+v", done.State())

		// Mutating the caller's plan after construction must not affect the
		// session.
		mutable := ReplayPlan{
			BuildID:      "mutation-build",
			CoverageSize: 2,
			Stages:       [][]CorpusEntry{{replayTestEntry("original")}},
		}
		ms, err := NewReplaySession(mutable)
		if !replayOK(c, err == nil, "mutation session: %v", err) {
			return
		}
		mutable.Stages[0][0].Values[0].([]byte)[0] = 'X'
		if ml, ok, err := ms.Next(); replayOK(c, err == nil && ok, "mutation dispatch: %v", err) && ok {
			c.Check(bytes.Equal(ml.Entry.Data, marshalCorpusFile([]byte("original"))),
				"caller plan mutation leaked into session: %q", ml.Entry.Data)
		}
	})
}
