// Command task_replay demonstrates deterministic collaborative task replay.
//
// Two logical tasks take turns on one worker (proving a worker carries no
// clock between its tasks), use cooperative cancellation acknowledgement, and
// finish with a terminal join. The record drives the real FastTrack detector
// and sync shadow, but performs no real shared-memory access.
package main

import (
	"fmt"
	"os"

	"github.com/kolkov/racedetector/race"
)

func main() {
	a := race.TaskRef{ID: 1, Generation: 1}
	b := race.TaskRef{ID: 2, Generation: 1}

	events := []race.TaskEvent{
		{Op: "create", Task: a},
		{Op: "switch", Task: a, Worker: 7},
		{Op: "write", Task: a, Worker: 7, Object: 100}, // 2
		{Op: "suspend", Task: a, Worker: 7},

		// B is an independent root: reusing worker 7 must not inherit A's clock.
		{Op: "create", Task: b},
		{Op: "switch", Task: b, Worker: 7},
		{Op: "read", Task: b, Worker: 7, Object: 100}, // 6: unsynchronized read -> report
		{Op: "suspend", Task: b, Worker: 7},

		{Op: "switch", Task: a, Worker: 7},
		{Op: "request-cancel", Task: a, Other: b, Worker: 7}, // 9
		{Op: "suspend", Task: a, Worker: 7},

		{Op: "switch", Task: b, Worker: 7},
		{Op: "observe-cancel", Task: b, Worker: 7}, // 12: merges request clock
		{Op: "write", Task: b, Worker: 7, Object: 200},
		{Op: "finish", Task: b, Worker: 7}, // 14: cancelled terminal state

		{Op: "switch", Task: a, Worker: 7},
		{Op: "join", Task: a, Other: b, Worker: 7},
		{Op: "read", Task: a, Worker: 7, Object: 200}, // happens-after B via join
		{Op: "finish", Task: a, Worker: 7},
	}

	result, err := race.ReplayTaskEvents(events, 8)
	if err != nil {
		fmt.Fprintln(os.Stderr, "replay failed:", err)
		os.Exit(1)
	}

	check := func(ok bool, msg string) {
		if !ok {
			panic("task_replay example: " + msg)
		}
	}

	check(result.Accepted == len(events), "all events must be accepted")
	check(len(result.States) == 2, "two generation states")

	byRef := map[race.TaskRef]race.TaskState{}
	for _, st := range result.States {
		byRef[st.Task] = st
	}
	check(byRef[a].State == "finished" && !byRef[a].Active && byRef[a].Worker == 0,
		"A must be finished and unbound")
	check(byRef[b].State == "cancelled" && !byRef[b].Active && byRef[b].Worker == 0,
		"B must be cancelled after acknowledging cancellation")

	check(len(result.Reports) == 1, "exactly one unsynchronized access report")
	rep := result.Reports[0]
	check(rep.Kind == "write-read", "report kind must be write-read")
	check(rep.First == 2 && rep.Second == 6, "report provenance must name events 2 and 6")
	check(rep.A == a && rep.B == b && rep.Object == 100, "report task/object identity")

	ordered := func(x, y uint64) bool {
		v, oerr := result.Ordered(x, y)
		check(oerr == nil, "ordered query in range")
		return v
	}
	check(!ordered(2, 6), "worker reuse alone is not happens-before")
	check(ordered(9, 12), "cancellation request happens-before acknowledgement")
	check(ordered(14, 17), "finish happens-before read after terminal join")
	self, selfErr := result.Ordered(3, 3)
	check(selfErr == nil && !self, "self query is never strictly ordered")
	if _, err := result.Ordered(0, uint64(len(events))); err == nil {
		panic("out-of-range Ordered query must fail")
	}

	fmt.Println("TASK_REPLAY_EXAMPLE_OK")
}
