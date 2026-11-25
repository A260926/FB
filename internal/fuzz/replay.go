// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package fuzz

import (
	"bytes"
	"crypto/sha256"
	"errors"
	"fmt"
	"io"
	"math"
	"reflect"
	"sort"
)

// ReplayPlan describes a deterministic, multi-stage replay of a corpus. No
// mutation, minimization or crashing input generation is performed: the
// coordinator only executes the values supplied by the caller.
//
// BuildID is a caller supplied, non-empty identifier for the build that the
// replay belongs to. Stages are replayed in order. CoverageSize is the number
// of coverage bytes (buckets) that workers report for each execution and must
// be in the range [1, 65536].
type ReplayPlan struct {
	BuildID      string
	Stages       [][]CorpusEntry
	CoverageSize int
}

// ReplayLease identifies a single unit of replay work that has been handed to
// a worker. Epoch changes whenever a session is restored from a snapshot, so
// leases issued by an older process can be detected and rejected.
type ReplayLease struct {
	Epoch    uint64
	Sequence uint64
	Stage    int
	Entry    CorpusEntry
}

// ReplayState is a snapshot of the committed replay state.
type ReplayState struct {
	// Stage is the current stage index. Empty stages are skipped. Once the
	// replay is complete, Stage equals the number of stages in the plan.
	Stage int

	// Committed is the number of feedback results committed in dispatch order.
	Committed uint64

	// Pending is the number of dispatched units that have not yet been
	// committed in order. This includes units whose out-of-order feedback has
	// already been received and buffered.
	Pending int

	// Coverage is the aggregate coverage mask. It is a copy; mutating it does
	// not affect the session.
	Coverage []byte

	// CorpusEntries is the number of distinct values in the replay corpus.
	CorpusEntries int
}

// replayWork is one unit of replay work, in global dispatch order. Sequences
// are one-based indexes into the plan's works slice.
type replayWork struct {
	stage int
	entry CorpusEntry
	data  []byte
}

type replayPlan struct {
	buildID      string
	coverageSize int
	types        []reflect.Type
	stages       int
	stageEnd     []int // stageEnd[i] is the number of works through stage i.
	works        []replayWork
	typeHash     [sha256.Size]byte
	corpusHash   [sha256.Size]byte
}

// ReplaySession hands out replay leases and accepts their feedback.
//
// A ReplaySession must be used by a single caller at a time (no concurrent
// Next/Accept/Snapshot/State calls).
type ReplaySession struct {
	plan  replayPlan
	epoch uint64
	coord *coordinator

	// feedback holds the coverage mask that was accepted for each sequence
	// that has reported feedback. Records are kept for committed sequences as
	// well so that duplicate feedback can be identified exactly.
	feedback map[uint64][]byte

	// redispatch lists sequences that had been dispatched before a restore but
	// whose feedback has not been received. They are reissued, in sequence
	// order, before any never-dispatched work.
	redispatch []uint64

	// dispatched is the highest sequence that has ever been handed out.
	dispatched uint64
}

// NewReplaySession validates plan and returns a fresh replay session.
func NewReplaySession(plan ReplayPlan) (*ReplaySession, error) {
	p, err := buildReplayPlan(plan)
	if err != nil {
		return nil, err
	}
	return &ReplaySession{
		plan:     p,
		epoch:    1,
		coord:    newReplayCoordinator(p.types, p.coverageSize),
		feedback: make(map[uint64][]byte),
	}, nil
}

// RestoreReplaySession validates snap and returns a new session that continues
// the replay described by plan. The returned session has a strictly larger
// epoch than the session that produced snap.
func RestoreReplaySession(plan ReplayPlan, snap []byte) (*ReplaySession, error) {
	p, err := buildReplayPlan(plan)
	if err != nil {
		return nil, err
	}
	st, err := decodeReplaySnapshot(snap, uint64(len(p.works)), p.coverageSize)
	if err != nil {
		return nil, err
	}
	s := &ReplaySession{
		plan:     p,
		coord:    newReplayCoordinator(p.types, p.coverageSize),
		feedback: make(map[uint64][]byte),
	}
	if err := s.applySnapshot(st); err != nil {
		return nil, err
	}
	return s, nil
}

// Next returns the next unit of replay work. It returns false when no work can
// be dispatched right now: either the replay is complete, or every entry of
// the current stage has been dispatched and the stage barrier is waiting for
// feedback to commit. Callers can distinguish the two cases with State.
func (s *ReplaySession) Next() (ReplayLease, bool, error) {
	if len(s.redispatch) > 0 {
		seq := s.redispatch[0]
		s.redispatch = s.redispatch[1:]
		return s.lease(seq), true, nil
	}
	if s.committed() >= uint64(len(s.plan.works)) {
		return ReplayLease{}, false, nil
	}

	stage := s.currentStage()
	if stage >= s.plan.stages {
		return ReplayLease{}, false, nil
	}

	if s.coord.inputQueue.len == 0 {
		start := int(s.dispatched)
		end := s.plan.stageEnd[stage]
		if start < end {
			for i := start; i < end; i++ {
				s.coord.inputQueue.enqueue(s.plan.works[i].entry)
			}
		}
	}
	if s.coord.inputQueue.len == 0 {
		// The current stage is fully dispatched; wait for commits.
		return ReplayLease{}, false, nil
	}

	seq := s.dispatched + 1
	if seq == 0 || seq > uint64(len(s.plan.works)) {
		return ReplayLease{}, false, errors.New("replay: internal dispatch sequence out of range")
	}
	v, ok := s.coord.inputQueue.peek()
	if !ok {
		return ReplayLease{}, false, errors.New("replay: internal dispatch queue is empty")
	}
	entry, ok := v.(CorpusEntry)
	if !ok {
		return ReplayLease{}, false, errors.New("replay: internal dispatch queue holds a non-corpus value")
	}
	// Use the coordinator's own dispatch accounting: sentInput removes the
	// entry from the queue and increments the pending count.
	s.coord.sentInput(fuzzInput{entry: entry, warmup: true, limit: 1})
	s.dispatched = seq
	return s.lease(seq), true, nil
}

// Accept records coverage feedback for a dispatched lease. It returns true the
// first time valid feedback is received, even if the feedback has to be
// buffered because earlier work has not committed. Exact duplicate feedback
// returns false and has no effect.
func (s *ReplaySession) Accept(lease ReplayLease, coverage []byte) (bool, error) {
	if lease.Epoch != s.epoch {
		return false, fmt.Errorf("replay: lease epoch %d does not match current epoch %d", lease.Epoch, s.epoch)
	}
	if len(coverage) != s.plan.coverageSize {
		return false, fmt.Errorf("replay: coverage length %d, want %d", len(coverage), s.plan.coverageSize)
	}
	seq := lease.Sequence
	if seq == 0 || seq > s.dispatched {
		return false, fmt.Errorf("replay: unknown sequence %d (dispatched %d)", seq, s.dispatched)
	}
	w := &s.plan.works[seq-1]
	if lease.Stage != w.stage {
		return false, fmt.Errorf("replay: lease stage %d does not match dispatched stage %d for sequence %d", lease.Stage, w.stage, seq)
	}
	data, err := replayEntryData(lease.Entry)
	if err != nil {
		return false, fmt.Errorf("replay: sequence %d: %v", seq, err)
	}
	if !bytes.Equal(data, w.data) {
		return false, fmt.Errorf("replay: lease entry bytes do not match dispatched work for sequence %d", seq)
	}

	if prev, ok := s.feedback[seq]; ok {
		if !bytes.Equal(prev, coverage) {
			return false, fmt.Errorf("replay: conflicting feedback for sequence %d", seq)
		}
		return false, nil
	}
	if seq <= s.committed() {
		return false, fmt.Errorf("replay: sequence %d was committed without recorded feedback", seq)
	}

	s.feedback[seq] = append([]byte(nil), coverage...)
	if err := s.drain(); err != nil {
		return false, err
	}
	return true, nil
}

// Snapshot returns a self-contained, deterministic encoding of the session's
// committed state, dispatch prefix, and received feedback. It may be called
// with work in flight or with feedback buffered out of order.
func (s *ReplaySession) Snapshot() ([]byte, error) {
	st := &replaySnapshot{
		buildID:       s.plan.buildID,
		coverageSize:  s.plan.coverageSize,
		typeHash:      s.plan.typeHash,
		corpusHash:    s.plan.corpusHash,
		epoch:         s.epoch,
		committed:     s.committed(),
		dispatched:    s.dispatched,
		corpusEntries: uint64(len(s.coord.corpus.entries)),
		coverage:      append([]byte(nil), s.coord.coverageMask...),
		feedback:      make([]replayFeedbackRecord, 0, len(s.feedback)),
	}
	for seq, cov := range s.feedback {
		st.feedback = append(st.feedback, replayFeedbackRecord{
			seq:      seq,
			coverage: append([]byte(nil), cov...),
		})
	}
	sort.Slice(st.feedback, func(i, j int) bool { return st.feedback[i].seq < st.feedback[j].seq })
	return encodeReplaySnapshot(st)
}

// State returns a copy of the currently committed state.
func (s *ReplaySession) State() ReplayState {
	return ReplayState{
		Stage:         s.currentStage(),
		Committed:     s.committed(),
		Pending:       int(s.coord.countWaiting),
		Coverage:      append([]byte(nil), s.coord.coverageMask...),
		CorpusEntries: len(s.coord.corpus.entries),
	}
}

func (s *ReplaySession) committed() uint64 {
	return uint64(s.coord.count)
}

// currentStage returns the stage that owns the next uncommitted sequence.
// Empty stages are skipped, and it returns the number of stages when the whole
// replay has committed.
func (s *ReplaySession) currentStage() int {
	committed := s.committed()
	for i, end := range s.plan.stageEnd {
		if uint64(end) > committed {
			return i
		}
	}
	return s.plan.stages
}

func (s *ReplaySession) lease(seq uint64) ReplayLease {
	w := &s.plan.works[seq-1]
	return ReplayLease{
		Epoch:    s.epoch,
		Sequence: seq,
		Stage:    w.stage,
		Entry:    cloneReplayCorpusEntry(w.entry),
	}
}

// drain commits buffered feedback in dispatch order until a gap is reached.
func (s *ReplaySession) drain() error {
	for {
		seq := s.committed() + 1
		cov, ok := s.feedback[seq]
		if !ok {
			return nil
		}
		if err := s.commit(seq, cov); err != nil {
			return err
		}
	}
}

// commit applies one in-order result through the real coordinator: coverage
// buckets are ORed in, the value is offered to the coordinator's corpus (which
// deduplicates by content hash), and dispatch accounting is updated.
func (s *ReplaySession) commit(seq uint64, coverage []byte) error {
	w := &s.plan.works[seq-1]
	if _, err := s.coord.addCorpusEntries(false, w.entry); err != nil {
		return err
	}
	s.coord.updateCoverage(coverage)
	s.coord.updateStats(fuzzResult{count: 1, limit: 1})
	return nil
}

// applySnapshot validates an entire decoded snapshot against the plan and then
// installs it. All validation happens before any session state is changed.
func (s *ReplaySession) applySnapshot(st *replaySnapshot) error {
	if st.buildID != s.plan.buildID {
		return fmt.Errorf("replay: snapshot build %q does not match plan build %q", st.buildID, s.plan.buildID)
	}
	if st.coverageSize != s.plan.coverageSize {
		return fmt.Errorf("replay: snapshot coverage size %d does not match plan coverage size %d", st.coverageSize, s.plan.coverageSize)
	}
	if st.typeHash != s.plan.typeHash {
		return errors.New("replay: snapshot parameter types do not match plan")
	}
	if st.corpusHash != s.plan.corpusHash {
		return errors.New("replay: snapshot stage corpus does not match plan")
	}
	total := uint64(len(s.plan.works))
	if st.dispatched > total {
		return fmt.Errorf("replay: snapshot dispatched %d exceeds plan work %d", st.dispatched, total)
	}
	if st.committed > st.dispatched {
		return fmt.Errorf("replay: snapshot committed %d exceeds dispatched %d", st.committed, st.dispatched)
	}
	if st.epoch == math.MaxUint64 {
		return errors.New("replay: snapshot epoch cannot be incremented")
	}
	if st.dispatched > 0 {
		stage := s.plan.works[st.dispatched-1].stage
		start := 0
		if stage > 0 {
			start = s.plan.stageEnd[stage-1]
		}
		if st.committed < uint64(start) {
			return errors.New("replay: snapshot dispatched past a stage barrier")
		}
	}

	seen := make(map[uint64]bool, len(st.feedback))
	coverageOf := make(map[uint64][]byte, len(st.feedback))
	for _, rec := range st.feedback {
		if rec.seq == 0 || rec.seq > st.dispatched {
			return fmt.Errorf("replay: snapshot feedback for unknown sequence %d", rec.seq)
		}
		if seen[rec.seq] {
			return fmt.Errorf("replay: snapshot has duplicate feedback for sequence %d", rec.seq)
		}
		if len(rec.coverage) != s.plan.coverageSize {
			return fmt.Errorf("replay: snapshot feedback for sequence %d has coverage length %d", rec.seq, len(rec.coverage))
		}
		seen[rec.seq] = true
		coverageOf[rec.seq] = rec.coverage
	}
	for seq := uint64(1); seq <= st.committed; seq++ {
		if !seen[seq] {
			return fmt.Errorf("replay: snapshot is missing feedback for committed sequence %d", seq)
		}
	}
	if seen[st.committed+1] {
		return fmt.Errorf("replay: snapshot has uncommitted feedback for in-order sequence %d", st.committed+1)
	}
	aggregate := make([]byte, s.plan.coverageSize)
	for seq := uint64(1); seq <= st.committed; seq++ {
		cov := coverageOf[seq]
		for i := range aggregate {
			aggregate[i] |= cov[i]
		}
	}
	if !bytes.Equal(aggregate, st.coverage) {
		return errors.New("replay: snapshot coverage does not match committed feedback")
	}
	if uint64(len(st.coverage)) != uint64(s.plan.coverageSize) {
		return errors.New("replay: snapshot coverage has wrong length")
	}

	var hashes = make(map[[sha256.Size]byte]bool)
	for seq := uint64(1); seq <= st.committed; seq++ {
		h := sha256.Sum256(s.plan.works[seq-1].data)
		hashes[h] = true
	}
	if uint64(len(hashes)) != st.corpusEntries {
		return fmt.Errorf("replay: snapshot corpus entry count %d does not match committed values %d", st.corpusEntries, len(hashes))
	}

	// Validation is complete. Install the state.
	s.epoch = st.epoch + 1
	s.dispatched = st.dispatched
	s.feedback = make(map[uint64][]byte, len(coverageOf))
	for seq, cov := range coverageOf {
		s.feedback[seq] = append([]byte(nil), cov...)
	}
	s.coord.count = int64(st.committed)
	s.coord.countWaiting = int64(st.dispatched - st.committed)
	// Feed the committed coverage through the coordinator's own bucket merge
	// so restored and uninterrupted sessions share one code path.
	s.coord.updateCoverage(st.coverage)
	for seq := uint64(1); seq <= st.committed; seq++ {
		if _, err := s.coord.addCorpusEntries(false, s.plan.works[seq-1].entry); err != nil {
			return err
		}
	}
	for seq := s.committed() + 1; seq <= s.dispatched; seq++ {
		if _, ok := s.feedback[seq]; !ok {
			s.redispatch = append(s.redispatch, seq)
		}
	}
	return nil
}

// buildReplayPlan validates and deeply copies the caller's plan.
func buildReplayPlan(plan ReplayPlan) (replayPlan, error) {
	p := replayPlan{
		buildID:      plan.BuildID,
		coverageSize: plan.CoverageSize,
		stages:       len(plan.Stages),
		stageEnd:     make([]int, len(plan.Stages)),
	}
	if plan.BuildID == "" {
		return p, errors.New("replay: BuildID must be non-empty")
	}
	if plan.CoverageSize < 1 || plan.CoverageSize > 65536 {
		return p, fmt.Errorf("replay: CoverageSize %d must be between 1 and 65536", plan.CoverageSize)
	}

	var types []reflect.Type
	haveTypes := false
	count := 0
	for stage, entries := range plan.Stages {
		for index := range entries {
			work, vals, err := normalizeReplayEntry(stage, index, entries[index])
			if err != nil {
				return p, err
			}
			if !haveTypes {
				types = replayValueTypes(vals)
				haveTypes = true
			} else if err := CheckCorpus(vals, types); err != nil {
				return p, fmt.Errorf("replay: stage %d entry %d has incompatible parameter types: %v", stage, index, err)
			}
			p.works = append(p.works, work)
			count++
		}
		p.stageEnd[stage] = count
	}
	p.types = types
	p.typeHash = replayTypeHash(types)
	p.corpusHash = replayCorpusHash(p.stageEnd, p.works)
	return p, nil
}

// normalizeReplayEntry validates one corpus entry and returns its canonical,
// self-contained form. Entries must carry Data or Values; a Path alone is not
// sufficient and must never be used to read from the filesystem.
func normalizeReplayEntry(stage, index int, entry CorpusEntry) (replayWork, []any, error) {
	hasData := entry.Data != nil
	hasValues := len(entry.Values) > 0
	if !hasData && !hasValues {
		return replayWork{}, nil, fmt.Errorf("replay: stage %d entry %d must set Data or Values (Path is not read)", stage, index)
	}

	var vals []any
	if hasData {
		decoded, err := unmarshalCorpusFile(entry.Data)
		if err != nil {
			return replayWork{}, nil, fmt.Errorf("replay: stage %d entry %d has malformed corpus data: %v", stage, index, err)
		}
		vals = decoded
	}
	if hasValues {
		if err := checkReplayValues(entry.Values); err != nil {
			return replayWork{}, nil, fmt.Errorf("replay: stage %d entry %d: %v", stage, index, err)
		}
		if hasData {
			if err := CheckCorpus(entry.Values, replayValueTypes(vals)); err != nil {
				return replayWork{}, nil, fmt.Errorf("replay: stage %d entry %d Values do not match Data: %v", stage, index, err)
			}
		} else {
			vals = entry.Values
		}
	}

	data := entry.Data
	if !hasData {
		data = marshalCorpusFile(vals...)
	}
	ce := cloneReplayCorpusEntry(entry)
	ce.Data = append([]byte(nil), data...)
	return replayWork{stage: stage, entry: ce, data: ce.Data}, vals, nil
}

// checkReplayValues verifies that vals can be encoded by the existing corpus
// file encoder. This avoids panics from marshalCorpusFile on unsupported
// values.
func checkReplayValues(vals []any) error {
	if len(vals) == 0 {
		return errors.New("corpus Values must not be empty")
	}
	for i, v := range vals {
		if !replayValueSupported(v) {
			return fmt.Errorf("unsupported corpus value type %T at index %d", v, i)
		}
	}
	return nil
}

func replayValueSupported(v any) bool {
	switch v.(type) {
	case int, int8, int16, int32, int64,
		uint, uint8, uint16, uint32, uint64,
		bool, float32, float64, string, []byte:
		return true
	}
	return false
}

func replayValueTypes(vals []any) []reflect.Type {
	types := make([]reflect.Type, len(vals))
	for i, v := range vals {
		types[i] = reflect.TypeOf(v)
	}
	return types
}

func replayEntryData(entry CorpusEntry) ([]byte, error) {
	if entry.Data != nil {
		return entry.Data, nil
	}
	if entry.Values != nil {
		if err := checkReplayValues(entry.Values); err != nil {
			return nil, err
		}
		return marshalCorpusFile(entry.Values...), nil
	}
	return nil, errors.New("corpus entry must set Data or Values")
}

// cloneReplayCorpusEntry returns a deep copy of entry that shares no mutable
// memory with the original.
func cloneReplayCorpusEntry(entry CorpusEntry) CorpusEntry {
	out := entry
	if entry.Data != nil {
		out.Data = append([]byte(nil), entry.Data...)
	}
	if entry.Values != nil {
		out.Values = make([]any, len(entry.Values))
		for i, v := range entry.Values {
			if b, ok := v.([]byte); ok {
				out.Values[i] = append([]byte(nil), b...)
			} else {
				out.Values[i] = v
			}
		}
	}
	return out
}

// newReplayCoordinator builds the coordinator used by a replay session. It
// deliberately does not run newCoordinator: replay must not pre-populate the
// corpus with every planned value, and an empty plan has no parameter types
// from which newCoordinator could synthesize a seed.
func newReplayCoordinator(types []reflect.Type, coverageSize int) *coordinator {
	return &coordinator{
		opts: CoordinateFuzzingOpts{
			Log:   io.Discard,
			Types: types,
		},
		corpus:       corpus{hashes: make(map[[sha256.Size]byte]bool)},
		coverageMask: make([]byte, coverageSize),
	}
}
