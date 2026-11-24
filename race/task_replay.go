// This file is the public entry point for deterministic collaborative task
// replay. It converts the frozen internal detector result into the stable
// race-package API and owns the immutable event clocks used by Ordered.

package race

import (
	"fmt"

	"github.com/kolkov/racedetector/internal/race/detector"
)

// TaskRef identifies one generation of a logical task.
//
// The first generation of a business ID is 1; later generations strictly
// increment by one and only exist after the previous generation has ended.
type TaskRef struct {
	ID         uint64
	Generation uint64
}

// TaskEvent is one recorded collaborative-task operation.
//
// Supported Op values:
//
//	create, switch, suspend,
//	read, write, release, acquire,
//	request-cancel, observe-cancel,
//	finish, join.
//
// Object is an opaque, non-zero stable object number for the memory/sync
// object operated on by read/write/release/acquire; it is never dereferenced.
type TaskEvent struct {
	Op     string
	Task   TaskRef
	Other  TaskRef
	Worker uint32
	Object uint64
}

// TaskConflict is one deduplicated FastTrack conflict report.
//
// First/Second are zero-based event sequence numbers. First/A identify the
// earlier conflicting access and Second/B the current one.
type TaskConflict struct {
	First  uint64
	Second uint64
	A      TaskRef
	B      TaskRef
	Object uint64
	Kind   string
}

// TaskState is the frozen state of one task generation.
//
// State is one of "suspended", "running", "finished", "cancelled". Worker is
// the currently bound worker (0 while unbound) and Active reports whether the
// generation has not reached a terminal state.
type TaskState struct {
	Task   TaskRef
	State  string
	Worker uint32
	Active bool
}

// clockEntry is one sparse component of a frozen event vector clock.
type clockEntry struct {
	tid   uint16
	clock uint64
}

// TaskReplayResult is the immutable result of ReplayTaskEvents.
//
// It owns every reported state, conflict and frozen event clock. Replaying
// again, reusing internal context pools, or mutating the input slice after the
// call cannot change a returned result.
type TaskReplayResult struct {
	// Accepted is the number of accepted prefix events.
	Accepted int
	// States is sorted by Task.ID then Task.Generation.
	States []TaskState
	// Reports are the deduplicated conflicts in detection order.
	Reports []TaskConflict

	clocks [][]clockEntry
}

// ReplayTaskEvents replays a collaborative task record through the production
// FastTrack detector and synchronization shadow without real shared memory.
//
// maxTasks bounds the cumulative number of created generations (not the
// current live count) and must be non-zero.
//
// If event k is invalid, the accepted prefix is events [0,k) and the returned
// result describes exactly that prefix while the error identifies k.
func ReplayTaskEvents(events []TaskEvent, maxTasks uint16) (TaskReplayResult, error) {
	in := make([]detector.ReplayTaskEvent, len(events))
	for i := range events {
		in[i] = detector.ReplayTaskEvent{
			Op:     events[i].Op,
			Task:   toInternalRef(events[i].Task),
			Other:  toInternalRef(events[i].Other),
			Worker: events[i].Worker,
			Object: events[i].Object,
		}
	}

	out, err := detector.ReplayTaskEvents(in, maxTasks)

	res := TaskReplayResult{
		Accepted: out.Accepted,
		States:   make([]TaskState, len(out.States)),
		Reports:  make([]TaskConflict, len(out.Reports)),
		clocks:   make([][]clockEntry, len(out.Clocks)),
	}
	for i, st := range out.States {
		res.States[i] = TaskState{
			Task:   fromInternalRef(st.Task),
			State:  st.State,
			Worker: st.Worker,
			Active: st.Active,
		}
	}
	for i, rep := range out.Reports {
		res.Reports[i] = TaskConflict{
			First:  rep.First,
			Second: rep.Second,
			A:      fromInternalRef(rep.A),
			B:      fromInternalRef(rep.B),
			Object: rep.Object,
			Kind:   rep.Kind,
		}
	}
	for i, c := range out.Clocks {
		entries := make([]clockEntry, len(c))
		for j, s := range c {
			entries[j] = clockEntry{tid: s.TID, clock: s.Clock}
		}
		res.clocks[i] = entries
	}
	return res, err
}

// Ordered answers strict happens-before between the frozen clocks of events
// first and second.
//
// Querying an event against itself returns false. Indices outside the accepted
// event range return an error. Mere textual event order is not happens-before:
// unrelated tasks remain unordered.
func (r TaskReplayResult) Ordered(first, second uint64) (bool, error) {
	n := uint64(len(r.clocks))
	if first >= n || second >= n {
		return false, fmt.Errorf(
			"race: Ordered indices (%d,%d) out of range for %d accepted events",
			first, second, n)
	}
	if first == second {
		return false, nil
	}
	return frozenStrictLess(r.clocks[first], r.clocks[second]), nil
}

func toInternalRef(r TaskRef) detector.ReplayTaskRef {
	return detector.ReplayTaskRef{ID: r.ID, Generation: r.Generation}
}

func fromInternalRef(r detector.ReplayTaskRef) TaskRef {
	return TaskRef{ID: r.ID, Generation: r.Generation}
}

// frozenStrictLess reports strict vector-clock order a < b.
func frozenStrictLess(a, b []clockEntry) bool {
	leq := true
	equal := true
	i, j := 0, 0
	for i < len(a) || j < len(b) {
		hasA, hasB := i < len(a), j < len(b)
		var ta uint16
		var ca uint64
		var tb uint16
		var cb uint64
		if hasA {
			ta, ca = a[i].tid, a[i].clock
		}
		if hasB {
			tb, cb = b[j].tid, b[j].clock
		}
		switch {
		case hasA && hasB && ta == tb:
			if ca > cb {
				leq = false
			}
			if ca != cb {
				equal = false
			}
			i++
			j++
		case hasB && (!hasA || tb < ta):
			equal = false
			j++
		default:
			if ca > 0 {
				leq = false
			}
			equal = false
			i++
		}
	}
	return leq && !equal
}
