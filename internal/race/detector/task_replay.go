package detector

// Deterministic replay of collaborative task records.
//
// This file is the internal engine behind race.ReplayTaskEvents. It maps each
// task *generation* to a unique FastTrack TID for the duration of one record,
// drives the production Detector OnRead/OnWrite paths and the production
// syncshadow release/acquire paths, and turns the resulting FastTrack reports
// into provenance-bearing task conflicts.
//
// It never touches real shared memory: TaskEvent.Object is an opaque stable
// number that is only converted to a shadow address key.

import (
	"errors"
	"fmt"
	"sort"

	"github.com/kolkov/racedetector/internal/race/epoch"
	"github.com/kolkov/racedetector/internal/race/goroutine"
	"github.com/kolkov/racedetector/internal/race/shadowmem"
)

// Replay state name constants. These are part of the public replay contract.
const (
	ReplayStateSuspended = "suspended"
	ReplayStateRunning   = "running"
	ReplayStateFinished  = "finished"
	ReplayStateCancelled = "cancelled"
)

// ReplayTaskRef identifies one task generation.
type ReplayTaskRef struct {
	ID         uint64
	Generation uint64
}

// ReplayTaskEvent is one recorded operation.
type ReplayTaskEvent struct {
	Op     string
	Task   ReplayTaskRef
	Other  ReplayTaskRef
	Worker uint32
	Object uint64
}

// ReplayTaskConflict is one deduplicated FastTrack conflict report.
type ReplayTaskConflict struct {
	First  uint64
	Second uint64
	A      ReplayTaskRef
	B      ReplayTaskRef
	Object uint64
	Kind   string
}

// ReplayTaskState is the frozen state of one task generation.
type ReplayTaskState struct {
	Task   ReplayTaskRef
	State  string
	Worker uint32
	Active bool
}

// ReplayClockStamp is one sparse vector clock component.
type ReplayClockStamp struct {
	TID   uint16
	Clock uint64
}

// ReplayOutput is the prefix result returned by ReplayTaskEvents.
type ReplayOutput struct {
	Accepted int
	States   []ReplayTaskState
	Reports  []ReplayTaskConflict
	Clocks   [][]ReplayClockStamp
}

type replayRefKey struct {
	id         uint64
	generation uint64
}

type replayGen struct {
	ref      ReplayTaskRef
	key      replayRefKey
	tid      uint16
	ctx      *goroutine.RaceContext
	state    string
	worker   uint32
	ack      bool
	pending  [][]ReplayClockStamp
	terminal []ReplayClockStamp
}

type replayConflictKey struct {
	object uint64
	kind   string
	a      replayRefKey
	b      replayRefKey
}

// replayRaceSignal is delivered by reportRaceV2 through Detector.raceSink.
type replayRaceSignal struct {
	kind string
	addr uintptr
	vs   interface{}
	prev epoch.Epoch
	curr epoch.Epoch
}

type replayEngine struct {
	maxTasks uint16
	det      *Detector

	gens      map[replayRefKey]*replayGen
	latestGen map[uint64]uint64
	workers   map[uint32]*replayGen
	created   int

	refs   []ReplayTaskRef
	clocks [][]ReplayClockStamp

	reports []ReplayTaskConflict
	seen    map[replayConflictKey]struct{}

	// epochSource maps every access epoch the engine handed to FastTrack to
	// its zero-based event index. Epochs are unique inside one record because
	// every generation owns a unique TID and is ticked exactly once per event.
	epochSource map[epoch.Epoch]uint64
	// readObject records which opaque object each read epoch belonged to, so
	// promoted read frontiers can be restricted to real readers of the object.
	readObject map[epoch.Epoch]uint64

	curIndex  uint64
	curGen    *replayGen
	curObject uint64
}

// ReplayTaskEvents replays a collaborative task record through FastTrack.
//
// On an invalid event it returns the error together with the accepted-prefix
// output; the failing event and every following event are ignored.
func ReplayTaskEvents(events []ReplayTaskEvent, maxTasks uint16) (ReplayOutput, error) {
	empty := ReplayOutput{
		Accepted: 0,
		States:   []ReplayTaskState{},
		Reports:  []ReplayTaskConflict{},
		Clocks:   [][]ReplayClockStamp{},
	}
	if maxTasks == 0 {
		return empty, errors.New("task replay: maxTasks capacity must be greater than zero")
	}

	e := &replayEngine{
		maxTasks:    maxTasks,
		gens:        make(map[replayRefKey]*replayGen),
		latestGen:   make(map[uint64]uint64),
		workers:     make(map[uint32]*replayGen),
		reports:     make([]ReplayTaskConflict, 0),
		seen:        make(map[replayConflictKey]struct{}),
		epochSource: make(map[epoch.Epoch]uint64),
		readObject:  make(map[epoch.Epoch]uint64),
	}
	e.det = NewDetector()
	e.det.raceSink = e.handleRace
	e.det.promotedReader = e.pickPromotedReader

	var replayErr error
	for i := range events {
		ev := events[i]
		e.curIndex = uint64(len(e.clocks))
		if err := e.apply(ev); err != nil {
			replayErr = fmt.Errorf("task replay: event %d op %q: %w", i, ev.Op, err)
			break
		}
	}

	return e.snapshot(), replayErr
}

func (e *replayEngine) snapshot() ReplayOutput {
	states := make([]ReplayTaskState, 0, len(e.gens))
	for _, g := range e.gens {
		states = append(states, ReplayTaskState{
			Task:   g.ref,
			State:  g.state,
			Worker: g.worker,
			// A generation is Active while it has not reached a terminal state.
			Active: g.state == ReplayStateSuspended || g.state == ReplayStateRunning,
		})
	}
	sort.Slice(states, func(i, j int) bool {
		if states[i].Task.ID != states[j].Task.ID {
			return states[i].Task.ID < states[j].Task.ID
		}
		return states[i].Task.Generation < states[j].Task.Generation
	})

	clocks := make([][]ReplayClockStamp, len(e.clocks))
	for i := range e.clocks {
		clocks[i] = append([]ReplayClockStamp(nil), e.clocks[i]...)
	}

	return ReplayOutput{
		Accepted: len(e.clocks),
		States:   states,
		Reports:  append([]ReplayTaskConflict(nil), e.reports...),
		Clocks:   clocks,
	}
}

func refKey(ref ReplayTaskRef) replayRefKey {
	return replayRefKey{id: ref.ID, generation: ref.Generation}
}

func (e *replayEngine) existing(ref ReplayTaskRef) (*replayGen, error) {
	g, ok := e.gens[refKey(ref)]
	if !ok {
		return nil, fmt.Errorf("unknown task ref {ID:%d Generation:%d}", ref.ID, ref.Generation)
	}
	return g, nil
}

// runningRunner resolves the acting task and enforces state and worker binding
// before any mutation.
func (e *replayEngine) runningRunner(ev ReplayTaskEvent) (*replayGen, error) {
	g, err := e.existing(ev.Task)
	if err != nil {
		return nil, err
	}
	if g.state != ReplayStateRunning {
		return nil, fmt.Errorf("task is state %q, expected running", g.state)
	}
	if ev.Worker != g.worker {
		return nil, fmt.Errorf("worker %d is not bound to task (bound worker %d)", ev.Worker, g.worker)
	}
	return g, nil
}

func (e *replayEngine) commit(ref ReplayTaskRef, clock []ReplayClockStamp) {
	e.refs = append(e.refs, ref)
	e.clocks = append(e.clocks, clock)
}

func (e *replayEngine) apply(ev ReplayTaskEvent) error {
	switch ev.Op {
	case "create":
		return e.applyCreate(ev)
	case "switch":
		return e.applySwitch(ev)
	case "suspend":
		return e.applySuspend(ev)
	case "read", "write":
		return e.applyAccess(ev)
	case "release":
		return e.applyRelease(ev)
	case "acquire":
		return e.applyAcquire(ev)
	case "request-cancel":
		return e.applyRequestCancel(ev)
	case "observe-cancel":
		return e.applyObserveCancel(ev)
	case "finish":
		return e.applyFinish(ev)
	case "join":
		return e.applyJoin(ev)
	default:
		return fmt.Errorf("unknown op %q", ev.Op)
	}
}

func (e *replayEngine) applyCreate(ev ReplayTaskEvent) error {
	if e.created >= int(e.maxTasks) {
		return fmt.Errorf("cumulative generation capacity maxTasks=%d reached", e.maxTasks)
	}
	if ev.Task.Generation == 0 {
		return errors.New("task generation must start at 1")
	}

	wantGen := e.latestGen[ev.Task.ID] + 1
	if wantGen == 0 {
		wantGen = 1
	}
	if ev.Task.Generation != wantGen {
		return fmt.Errorf("generation must be strictly %d for task ID %d, got %d",
			wantGen, ev.Task.ID, ev.Task.Generation)
	}
	if wantGen > 1 {
		prev, ok := e.gens[replayRefKey{id: ev.Task.ID, generation: wantGen - 1}]
		if !ok {
			return fmt.Errorf("previous generation %d is missing", wantGen-1)
		}
		if prev.state != ReplayStateFinished && prev.state != ReplayStateCancelled {
			return fmt.Errorf("previous generation %d has not ended (state %q)",
				prev.ref.Generation, prev.state)
		}
	}

	var ctx *goroutine.RaceContext
	if ev.Other == (ReplayTaskRef{}) {
		// Independent root. TIDs are handed out once and never reused.
		ctx = goroutine.Alloc(uint16(e.created + 1))
	} else {
		parent, err := e.existing(ev.Other)
		if err != nil {
			return fmt.Errorf("create parent: %w", err)
		}
		if parent.state != ReplayStateRunning {
			return fmt.Errorf("create parent {ID:%d Generation:%d} is state %q, expected running",
				parent.ref.ID, parent.ref.Generation, parent.state)
		}
		// Fork edge: child inherits the parent creation clock.
		ctx = goroutine.AllocWithParentClock(uint16(e.created+1), parent.ctx.C)
	}

	tid := uint16(e.created + 1)
	g := &replayGen{
		ref:   ev.Task,
		key:   refKey(ev.Task),
		tid:   tid,
		ctx:   ctx,
		state: ReplayStateSuspended,
	}
	e.gens[g.key] = g
	e.latestGen[ev.Task.ID] = ev.Task.Generation
	e.created++

	e.commit(ev.Task, snapshotClock(ctx.C))
	return nil
}

func (e *replayEngine) applySwitch(ev ReplayTaskEvent) error {
	g, err := e.existing(ev.Task)
	if err != nil {
		return err
	}
	if g.state != ReplayStateSuspended {
		return fmt.Errorf("switch requires suspended task, state is %q", g.state)
	}
	if occupant := e.workers[ev.Worker]; occupant != nil {
		return fmt.Errorf("worker %d is occupied by {ID:%d Generation:%d}",
			ev.Worker, occupant.ref.ID, occupant.ref.Generation)
	}

	g.state = ReplayStateRunning
	g.worker = ev.Worker
	e.workers[ev.Worker] = g

	g.ctx.IncrementClock()
	e.commit(ev.Task, snapshotClock(g.ctx.C))
	return nil
}

func (e *replayEngine) applySuspend(ev ReplayTaskEvent) error {
	g, err := e.runningRunner(ev)
	if err != nil {
		return err
	}

	delete(e.workers, g.worker)
	g.worker = 0
	g.state = ReplayStateSuspended

	g.ctx.IncrementClock()
	e.commit(ev.Task, snapshotClock(g.ctx.C))
	return nil
}

func (e *replayEngine) applyAccess(ev ReplayTaskEvent) error {
	g, err := e.runningRunner(ev)
	if err != nil {
		return err
	}
	if ev.Object == 0 {
		return errors.New("read/write event requires a non-zero stable Object number")
	}

	idx := e.curIndex
	addr := uintptr(ev.Object)
	entry := g.ctx.GetEpoch()
	_, entryClock := entry.Decode()
	e.epochSource[entry] = idx
	if ev.Op == "read" {
		e.readObject[entry] = ev.Object
	}
	e.curGen = g
	e.curObject = ev.Object

	if ev.Op == "read" {
		e.det.OnRead(addr, g.ctx)
	} else {
		e.det.OnWrite(addr, g.ctx)
	}

	// FastTrack's race-report early returns skip IncrementClock. Enforce one
	// canonical tick per accepted event so every frozen event clock is unique
	// and strictly ordered within a generation.
	if uint64(g.ctx.C.Get(g.tid)) == entryClock {
		g.ctx.IncrementClock()
	}

	e.commit(ev.Task, snapshotClock(g.ctx.C))
	return nil
}

func (e *replayEngine) applyRelease(ev ReplayTaskEvent) error {
	g, err := e.runningRunner(ev)
	if err != nil {
		return err
	}
	if ev.Object == 0 {
		return errors.New("release event requires a non-zero Object number")
	}

	addr := uintptr(ev.Object)
	_, entryClock := g.ctx.GetEpoch().Decode()
	e.det.OnRelease(addr, g.ctx)
	// OnRelease always ticks, but keep the canonical guarantee explicit.
	if uint64(g.ctx.C.Get(g.tid)) == entryClock {
		g.ctx.IncrementClock()
	}
	// Production OnRelease publishes its pre-tick clock. Republish the frozen
	// post-event stamp so a later acquire joins exactly Ordered(release, acquire).
	e.det.syncShadow.GetOrCreate(addr).SetReleaseClock(g.ctx.C)

	e.commit(ev.Task, snapshotClock(g.ctx.C))
	return nil
}

func (e *replayEngine) applyAcquire(ev ReplayTaskEvent) error {
	g, err := e.runningRunner(ev)
	if err != nil {
		return err
	}
	if ev.Object == 0 {
		return errors.New("acquire event requires a non-zero Object number")
	}

	e.det.OnAcquire(uintptr(ev.Object), g.ctx)

	e.commit(ev.Task, snapshotClock(g.ctx.C))
	return nil
}

func (e *replayEngine) applyRequestCancel(ev ReplayTaskEvent) error {
	g, err := e.runningRunner(ev)
	if err != nil {
		return err
	}
	other, err := e.existing(ev.Other)
	if err != nil {
		return fmt.Errorf("request-cancel target: %w", err)
	}

	// The request carries the requester event stamp. The requester advances;
	// the target does not merge anything until observe-cancel.
	g.ctx.IncrementClock()
	stamp := snapshotClock(g.ctx.C)
	other.pending = append(other.pending, stamp)

	e.commit(ev.Task, stamp)
	return nil
}

func (e *replayEngine) applyObserveCancel(ev ReplayTaskEvent) error {
	g, err := e.runningRunner(ev)
	if err != nil {
		return err
	}

	// observe-cancel is the cooperative acknowledgement point: merge every
	// pending request clock here (and only here) and mark the generation as
	// cancellation-confirmed. The request itself never does this.
	g.ack = true
	for _, c := range g.pending {
		joinStamp(g.ctx.C, c)
	}
	g.pending = nil

	g.ctx.IncrementClock()
	e.commit(ev.Task, snapshotClock(g.ctx.C))
	return nil
}

func (e *replayEngine) applyFinish(ev ReplayTaskEvent) error {
	g, err := e.runningRunner(ev)
	if err != nil {
		return err
	}

	g.ctx.IncrementClock()
	stamp := snapshotClock(g.ctx.C)
	g.terminal = stamp

	if g.ack {
		g.state = ReplayStateCancelled
	} else {
		g.state = ReplayStateFinished
	}
	delete(e.workers, g.worker)
	g.worker = 0

	e.commit(ev.Task, stamp)
	return nil
}

func (e *replayEngine) applyJoin(ev ReplayTaskEvent) error {
	g, err := e.runningRunner(ev)
	if err != nil {
		return err
	}
	other, err := e.existing(ev.Other)
	if err != nil {
		return fmt.Errorf("join target: %w", err)
	}
	if other.state != ReplayStateFinished && other.state != ReplayStateCancelled {
		return fmt.Errorf("join target {ID:%d Generation:%d} is state %q, expected terminal",
			other.ref.ID, other.ref.Generation, other.state)
	}

	// Terminal join edge: merge the finished generation's published clock.
	joinStamp(g.ctx.C, other.terminal)
	g.ctx.IncrementClock()

	e.commit(ev.Task, snapshotClock(g.ctx.C))
	return nil
}

// handleRace is the Detector.raceSink hook invoked from reportRaceV2 before
// detector-level deduplication/output. It converts a real FastTrack judgment
// into a provenance-bearing, task-identity-deduplicated conflict.
func (e *replayEngine) handleRace(sig replayRaceSignal) {
	second := e.curIndex
	g := e.curGen
	if g == nil {
		return
	}

	first := uint64(0)
	haveFirst := false
	if sig.prev != 0 {
		if idx, ok := e.epochSource[sig.prev]; ok {
			first, haveFirst = idx, true
		}
	}
	if !haveFirst {
		if idx, ep := e.chooseReader(sig.vs, g.ctx); ep != 0 {
			first, haveFirst = idx, true
		}
	}
	if !haveFirst {
		// Every production conflict in a well-formed record has resolvable
		// provenance; never emit an epoch0/unknown-source report.
		return
	}

	aRef := e.refs[first]
	key := replayConflictKey{
		object: e.curObject,
		kind:   sig.kind,
		a:      refKey(aRef),
		b:      g.key,
	}
	if _, dup := e.seen[key]; dup {
		return
	}
	e.seen[key] = struct{}{}

	e.reports = append(e.reports, ReplayTaskConflict{
		First:  first,
		Second: second,
		A:      aRef,
		B:      g.ref,
		Object: e.curObject,
		Kind:   sig.kind,
	})
}

// pickPromotedReader is installed as Detector.promotedReader. The production
// OnWrite promoted-read branch calls it instead of falling back to epoch 0.
func (e *replayEngine) pickPromotedReader(vs *shadowmem.VarState, writer *goroutine.RaceContext) epoch.Epoch {
	_, ep := e.chooseReader(vs, writer)
	return ep
}

// chooseReader resolves the previous reader for a read-write conflict.
//
// Among the real readers of the current object's read frontier that do not
// happen-before the writer, it returns the one with the smallest event index.
// If frontier HB filtering leaves no candidate (possible with inherited VC
// components), it falls back to the smallest-index real frontier reader.
func (e *replayEngine) chooseReader(vsIface interface{}, writer *goroutine.RaceContext) (uint64, epoch.Epoch) {
	vs, ok := vsIface.(*shadowmem.VarState)
	if !ok || vs == nil {
		return 0, 0
	}

	var conflictIdx, anyIdx uint64
	var conflictEp, anyEp epoch.Epoch
	haveConflict, haveAny := false, false

	consider := func(ep epoch.Epoch) {
		if ep == 0 {
			return
		}
		idx, known := e.epochSource[ep]
		if !known {
			return
		}
		// Restrict to actual reads of this object.
		if obj, ok := e.readObject[ep]; !ok || obj != e.curObject {
			return
		}
		if !haveAny || idx < anyIdx {
			anyIdx, anyEp, haveAny = idx, ep, true
		}
		if !ep.HappensBefore(writer.C) {
			if !haveConflict || idx < conflictIdx {
				conflictIdx, conflictEp, haveConflict = idx, ep, true
			}
		}
	}

	if vs.IsPromoted() {
		rc := vs.GetReadClock()
		if rc != nil {
			maxTID := uint32(rc.GetMaxTID())
			for tid := uint32(0); tid <= maxTID; tid++ {
				clock := rc.Get(uint16(tid))
				if clock == 0 {
					continue
				}
				consider(epoch.NewEpoch(uint16(tid), uint64(clock)))
			}
		}
	} else {
		for _, ep := range vs.GetReadEpochs() {
			consider(ep)
		}
	}

	if haveConflict {
		return conflictIdx, conflictEp
	}
	if haveAny {
		return anyIdx, anyEp
	}
	return 0, 0
}

// snapshotClock copies the non-zero components of a vector clock into a small
// immutable slice that does not alias pooled VectorClock storage.
func snapshotClock(vc interface {
	GetMaxTID() uint16
	Get(uint16) uint32
}) []ReplayClockStamp {
	maxTID := uint32(vc.GetMaxTID())
	out := make([]ReplayClockStamp, 0, maxTID+1)
	for tid := uint32(0); tid <= maxTID; tid++ {
		c := vc.Get(uint16(tid))
		if c != 0 {
			out = append(out, ReplayClockStamp{TID: uint16(tid), Clock: uint64(c)})
		}
	}
	return out
}

// joinStamp point-wise merges a frozen sparse clock into a live vector clock.
func joinStamp(dst vectorClockJoiner, stamp []ReplayClockStamp) {
	for _, c := range stamp {
		if cur := dst.Get(c.TID); uint64(cur) < c.Clock {
			dst.Set(c.TID, uint32(c.Clock))
		}
	}
}

// vectorClockJoiner is the minimal VectorClock surface joinStamp needs.
type vectorClockJoiner interface {
	Get(uint16) uint32
	Set(uint16, uint32)
}
