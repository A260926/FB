package detector_test

// Content checks for deterministic collaborative task replay.
//
// These groups are executed by task_support/run.sh through taskcheck.Run.
// They exercise migration, cooperative cancellation, generation reuse and
// provenance/prefix behaviour against the public race.ReplayTaskEvents API.

import (
	"os"
	"reflect"
	"testing"

	"github.com/kolkov/racedetector/race"
	"github.com/kolkov/racedetector/task_support/taskcheck"
)

func trSilenceStderr(t *testing.T) {
	t.Helper()
	old := os.Stderr
	devNull, err := os.Open(os.DevNull)
	if err != nil {
		return
	}
	os.Stderr = devNull
	t.Cleanup(func() {
		os.Stderr = old
		_ = devNull.Close()
	})
}

func trRef(id uint64) race.TaskRef { return race.TaskRef{ID: id, Generation: 1} }

func TestTaskMigration(t *testing.T) {
	trSilenceStderr(t)
	taskcheck.Run(t, func(c *taskcheck.C) {
		a := race.TaskRef{ID: 1, Generation: 1}
		b := race.TaskRef{ID: 2, Generation: 1}

		// One generation migrates workers and keeps its logical clock.
		events := []race.TaskEvent{
			{Op: "create", Task: a},
			{Op: "switch", Task: a, Worker: 1},
			{Op: "write", Task: a, Worker: 1, Object: 7},
			{Op: "suspend", Task: a, Worker: 1},
			{Op: "switch", Task: a, Worker: 2},
			{Op: "read", Task: a, Worker: 2, Object: 7},
		}
		r, err := race.ReplayTaskEvents(events, 8)
		c.Check(err == nil, "migration record must replay: %v", err)
		c.Check(r.Accepted == len(events), "all events accepted")
		c.Check(len(r.Reports) == 0, "same migrated task does not race itself")
		c.Check(len(r.States) == 1, "one generation state")
		st := r.States[0]
		c.Check(st.Task == a, "state identity")
		c.Check(st.State == "running" && st.Active && st.Worker == 2,
			"migrated state running on worker 2, got %+v", st)
		ordered, oerr := r.Ordered(2, 5)
		c.Check(oerr == nil && ordered, "migration preserves task order")

		// While suspended the generation is unbound but still Active.
		pre, perr := race.ReplayTaskEvents(events[:4], 8)
		c.Check(perr == nil && pre.Accepted == 4, "suspend prefix accepted")
		ps := pre.States[0]
		c.Check(ps.State == "suspended" && ps.Worker == 0 && ps.Active,
			"suspended prefix unbound but active, got %+v", ps)

		// Reusing the same worker for an independent task transfers no clock.
		sharedWorker := []race.TaskEvent{
			{Op: "create", Task: a},
			{Op: "switch", Task: a, Worker: 9},
			{Op: "write", Task: a, Worker: 9, Object: 11},
			{Op: "suspend", Task: a, Worker: 9},
			{Op: "create", Task: b},
			{Op: "switch", Task: b, Worker: 9},
			{Op: "read", Task: b, Worker: 9, Object: 11},
		}
		sr, serr := race.ReplayTaskEvents(sharedWorker, 8)
		c.Check(serr == nil, "shared worker record replays: %v", serr)
		c.Check(len(sr.Reports) == 1, "worker reuse without HB produces one report")
		if len(sr.Reports) == 1 {
			rep := sr.Reports[0]
			c.Check(rep.Kind == "write-read", "kind write-read, got %q", rep.Kind)
			c.Check(rep.First == 2 && rep.Second == 6, "provenance 2->6, got %d->%d", rep.First, rep.Second)
			c.Check(rep.A == a && rep.B == b && rep.Object == 11, "report identity")
			ord, _ := sr.Ordered(2, 6)
			c.Check(!ord, "mere worker reuse is not HB")
		}

		// A switch onto an occupied worker is rejected with an accepted prefix.
		occupied := []race.TaskEvent{
			{Op: "create", Task: a},
			{Op: "switch", Task: a, Worker: 1},
			{Op: "create", Task: b},
			{Op: "switch", Task: b, Worker: 1},
		}
		or, oerr2 := race.ReplayTaskEvents(occupied, 8)
		c.Check(oerr2 != nil, "occupied worker switch must fail")
		c.Check(or.Accepted == 3, "prefix of three events, got %d", or.Accepted)
		c.Check(len(or.States) == 2, "both created generations present")
	})
}

func TestTaskCancellation(t *testing.T) {
	trSilenceStderr(t)
	taskcheck.Run(t, func(c *taskcheck.C) {
		a := trRef(1)
		b := trRef(2)

		events := []race.TaskEvent{
			{Op: "create", Task: a},                              // 0
			{Op: "create", Task: b},                              // 1
			{Op: "switch", Task: a, Worker: 7},                   // 2
			{Op: "write", Task: a, Worker: 7, Object: 5},         // 3
			{Op: "suspend", Task: a, Worker: 7},                  // 4
			{Op: "switch", Task: b, Worker: 7},                   // 5
			{Op: "write", Task: b, Worker: 7, Object: 5},         // 6 pre-ack race
			{Op: "suspend", Task: b, Worker: 7},                  // 7
			{Op: "switch", Task: a, Worker: 7},                   // 8
			{Op: "request-cancel", Task: a, Other: b, Worker: 7}, // 9
			{Op: "suspend", Task: a, Worker: 7},                  // 10
			{Op: "switch", Task: b, Worker: 7},                   // 11
			{Op: "observe-cancel", Task: b, Worker: 7},           // 12
			{Op: "write", Task: b, Worker: 7, Object: 5},         // 13 cleanup
			{Op: "finish", Task: b, Worker: 7},                   // 14
			{Op: "switch", Task: a, Worker: 7},                   // 15
			{Op: "join", Task: a, Other: b, Worker: 7},           // 16
			{Op: "finish", Task: a, Worker: 7},                   // 17
		}
		r, err := race.ReplayTaskEvents(events, 8)
		c.Check(err == nil, "cancellation record replays: %v", err)
		c.Check(r.Accepted == len(events), "all accepted")

		// Exactly the pre-acknowledgement conflict; cleanup is synchronized.
		c.Check(len(r.Reports) == 1, "one pre-ack report, got %d", len(r.Reports))
		if len(r.Reports) == 1 {
			c.Check(r.Reports[0].First == 3 && r.Reports[0].Second == 6,
				"pre-ack provenance 3->6, got %d->%d", r.Reports[0].First, r.Reports[0].Second)
		}

		byRef := map[race.TaskRef]race.TaskState{}
		for _, st := range r.States {
			byRef[st.Task] = st
		}
		c.Check(byRef[b].State == "cancelled", "acknowledged target cancelled")
		c.Check(byRef[a].State == "finished", "requester finished normally")

		checkOrd := func(x, y uint64, want bool) {
			got, oerr := r.Ordered(x, y)
			c.Check(oerr == nil, "ordered %d,%d in range", x, y)
			c.Check(got == want, "Ordered(%d,%d)=%v want %v", x, y, got, want)
		}
		checkOrd(3, 6, false)  // record order without HB
		checkOrd(9, 12, true)  // request -> acknowledgement
		checkOrd(9, 13, true)  // merged request orders cleanup
		checkOrd(14, 16, true) // finish -> join
		checkOrd(14, 17, true) // finish -> after-join event

		// The request does not stop the target: before observe-cancel the
		// target prefix still shows it suspended/alive, not cancelled.
		beforeObserve, berr := race.ReplayTaskEvents(events[:11], 8)
		c.Check(berr == nil, "pre-observe prefix replays")
		var bState *race.TaskState
		for i := range beforeObserve.States {
			if beforeObserve.States[i].Task == b {
				bState = &beforeObserve.States[i]
			}
		}
		c.Check(bState != nil && bState.State == "suspended" && bState.Active,
			"target stays suspended/alive after bare request, got %+v", bState)

		// A task that finishes without acknowledging cancellation is finished.
		plain := []race.TaskEvent{
			{Op: "create", Task: a},
			{Op: "switch", Task: a, Worker: 1},
			{Op: "write", Task: a, Worker: 1, Object: 9},
			{Op: "finish", Task: a, Worker: 1},
		}
		pr, perr := race.ReplayTaskEvents(plain, 4)
		c.Check(perr == nil, "plain finish replays")
		c.Check(pr.States[0].State == "finished", "unacknowledged task finished")

		// Cancellation must not be treated as a join: replaying the prefix
		// through the pre-acknowledgement write keeps the conflict and the
		// request/ack HB edge is established only at observe-cancel.
		preAck, paerr := race.ReplayTaskEvents(events[:7], 8)
		c.Check(paerr == nil, "pre-ack prefix replays")
		c.Check(len(preAck.Reports) == 1, "conflict exists before acknowledgement")
		preOrd, _ := preAck.Ordered(3, 6)
		c.Check(!preOrd, "pre-ack accesses are not ordered")

		// A terminal join publishes the finished generation's clock, so a
		// later access by the joiner is synchronized.
		joinOK := []race.TaskEvent{
			{Op: "create", Task: a},
			{Op: "switch", Task: a, Worker: 1},
			{Op: "write", Task: a, Worker: 1, Object: 31},
			{Op: "finish", Task: a, Worker: 1},
			{Op: "create", Task: b},
			{Op: "switch", Task: b, Worker: 2},
			{Op: "join", Task: b, Other: a, Worker: 2},
			{Op: "read", Task: b, Worker: 2, Object: 31},
		}
		jr, jerr := race.ReplayTaskEvents(joinOK, 4)
		c.Check(jerr == nil, "terminal join record replays: %v", jerr)
		c.Check(len(jr.Reports) == 0, "join synchronizes terminal write, got %d reports", len(jr.Reports))

		// Without the join the same access is an unsynchronized report.
		noJoin := append([]race.TaskEvent{}, joinOK[:5]...)
		noJoin = append(noJoin,
			race.TaskEvent{Op: "switch", Task: b, Worker: 2},
			race.TaskEvent{Op: "read", Task: b, Worker: 2, Object: 31},
		)
		njr, njerr := race.ReplayTaskEvents(noJoin, 4)
		c.Check(njerr == nil, "no-join record replays: %v", njerr)
		c.Check(len(njr.Reports) == 1, "missing join leaves one report, got %d", len(njr.Reports))

		// After acknowledgement the merged request clock orders both cleanup
		// reads and writes. The read uses a different conflict kind so this
		// cannot be masked by deduplication of the earlier write-write race.
		cleanup := []race.TaskEvent{
			{Op: "create", Task: a},
			{Op: "create", Task: b},
			{Op: "switch", Task: a, Worker: 1},
			{Op: "write", Task: a, Worker: 1, Object: 71},
			{Op: "request-cancel", Task: a, Other: b, Worker: 1},
			{Op: "suspend", Task: a, Worker: 1},
			{Op: "switch", Task: b, Worker: 2},
			{Op: "observe-cancel", Task: b, Worker: 2},
			{Op: "read", Task: b, Worker: 2, Object: 71},
			{Op: "write", Task: b, Worker: 2, Object: 71},
		}
		cr, cerr := race.ReplayTaskEvents(cleanup, 4)
		c.Check(cerr == nil, "cleanup record replays: %v", cerr)
		c.Check(len(cr.Reports) == 0, "acknowledged cleanup is race-free, got %+v", cr.Reports)
	})
}

func TestTaskGeneration(t *testing.T) {
	trSilenceStderr(t)
	taskcheck.Run(t, func(c *taskcheck.C) {
		x1 := race.TaskRef{ID: 7, Generation: 1}
		x2 := race.TaskRef{ID: 7, Generation: 2}
		y := race.TaskRef{ID: 8, Generation: 1}

		// Zero capacity is rejected.
		_, zerr := race.ReplayTaskEvents(nil, 0)
		c.Check(zerr != nil, "maxTasks=0 rejected")

		// maxTasks counts cumulative generations: two fit, third is refused.
		capEvents := []race.TaskEvent{
			{Op: "create", Task: x1},
			{Op: "switch", Task: x1, Worker: 1},
			{Op: "finish", Task: x1, Worker: 1},
			{Op: "create", Task: x2},
			{Op: "create", Task: y},
		}
		cr, cerr := race.ReplayTaskEvents(capEvents, 2)
		c.Check(cerr != nil, "third cumulative generation rejected at capacity")
		c.Check(cr.Accepted == 4, "capacity prefix 4, got %d", cr.Accepted)

		// Generation must be exactly previous+1.
		skip := []race.TaskEvent{
			{Op: "create", Task: x1},
			{Op: "switch", Task: x1, Worker: 1},
			{Op: "finish", Task: x1, Worker: 1},
			{Op: "create", Task: race.TaskRef{ID: 7, Generation: 3}},
		}
		_, serr := race.ReplayTaskEvents(skip, 4)
		c.Check(serr != nil, "generation skip rejected")

		// New generation cannot start while the old one has not ended.
		notEnded := []race.TaskEvent{
			{Op: "create", Task: x1},
			{Op: "switch", Task: x1, Worker: 1},
			{Op: "create", Task: x2},
		}
		_, nerr := race.ReplayTaskEvents(notEnded, 4)
		c.Check(nerr != nil, "overlapping generations rejected")

		// States sort by ID then Generation even when created out of order.
		unsorted := []race.TaskEvent{
			{Op: "create", Task: race.TaskRef{ID: 9, Generation: 1}},
			{Op: "create", Task: race.TaskRef{ID: 1, Generation: 1}},
			{Op: "switch", Task: race.TaskRef{ID: 1, Generation: 1}, Worker: 1},
			{Op: "finish", Task: race.TaskRef{ID: 1, Generation: 1}, Worker: 1},
			{Op: "create", Task: race.TaskRef{ID: 1, Generation: 2}},
		}
		ur, uerr := race.ReplayTaskEvents(unsorted, 8)
		c.Check(uerr == nil, "sorted-states record replays: %v", uerr)
		c.Check(len(ur.States) == 3, "three states")
		c.Check(ur.States[0].Task.ID == 1 && ur.States[0].Task.Generation == 1, "first sorted 1/1")
		c.Check(ur.States[1].Task.ID == 1 && ur.States[1].Task.Generation == 2, "second sorted 1/2")
		c.Check(ur.States[2].Task.ID == 9, "third sorted 9/1")

		// Business ID reuse keeps generation identity in reports and the
		// same worker does not merge identities. Stable writer y goes first so
		// the production shadow retains y as the previous write.
		reuse := []race.TaskEvent{
			{Op: "create", Task: y},                        // 0
			{Op: "switch", Task: y, Worker: 3},             // 1
			{Op: "write", Task: y, Worker: 3, Object: 42},  // 2
			{Op: "create", Task: x1},                       // 3
			{Op: "switch", Task: x1, Worker: 4},            // 4
			{Op: "write", Task: x1, Worker: 4, Object: 42}, // 5 report y->x1
			{Op: "finish", Task: y, Worker: 3},             // 6
		}
		rr, rerr := race.ReplayTaskEvents(reuse, 8)
		c.Check(rerr == nil, "reuse prefix replays: %v", rerr)
		c.Check(len(rr.Reports) == 1, "first generation conflict reported")

		reuseExt := []race.TaskEvent{
			{Op: "finish", Task: x1, Worker: 4},            // 7
			{Op: "create", Task: x2},                       // 8
			{Op: "switch", Task: x2, Worker: 4},            // 9 same worker as x1
			{Op: "write", Task: x2, Worker: 4, Object: 42}, // 10 report y->x2
		}
		reuse2 := append(append([]race.TaskEvent{}, reuse...), reuseExt...)
		r2, r2err := race.ReplayTaskEvents(reuse2, 8)
		c.Check(r2err == nil, "reuse record replays: %v", r2err)
		c.Check(len(r2.Reports) == 2, "new generation yields a distinct report, got %d", len(r2.Reports))
		if len(r2.Reports) == 2 {
			c.Check(r2.Reports[0].A == y && r2.Reports[0].B == x1, "first report identities")
			c.Check(r2.Reports[1].A == y && r2.Reports[1].B == x2,
				"second report carries new generation, got %+v", r2.Reports[1].B)
			c.Check(r2.Reports[1].First == 2 && r2.Reports[1].Second == 10,
				"second provenance 2->10, got %d->%d", r2.Reports[1].First, r2.Reports[1].Second)
		}
	})
}

func TestTaskProvenance(t *testing.T) {
	trSilenceStderr(t)
	taskcheck.Run(t, func(c *taskcheck.C) {
		a := trRef(1)
		b := trRef(2)

		// Exact write-write provenance.
		events := []race.TaskEvent{
			{Op: "create", Task: a},
			{Op: "switch", Task: a, Worker: 1},
			{Op: "write", Task: a, Worker: 1, Object: 9},
			{Op: "create", Task: b},
			{Op: "switch", Task: b, Worker: 2},
			{Op: "write", Task: b, Worker: 2, Object: 9},
		}
		r, err := race.ReplayTaskEvents(events, 8)
		c.Check(err == nil, "ww record replays")
		c.Check(len(r.Reports) == 1, "one ww report")
		if len(r.Reports) == 1 {
			p := r.Reports[0]
			c.Check(p.First == 2 && p.Second == 5, "indices 2/5, got %d/%d", p.First, p.Second)
			c.Check(p.A == a && p.B == b, "A/B refs")
			c.Check(p.Object == 9 && p.Kind == "write-write", "object/kind")
		}

		// Repeated same-identity conflicts are deduplicated.
		dup := []race.TaskEvent{
			{Op: "create", Task: a},
			{Op: "switch", Task: a, Worker: 1},
			{Op: "write", Task: a, Worker: 1, Object: 3},
			{Op: "create", Task: b},
			{Op: "switch", Task: b, Worker: 2},
			{Op: "write", Task: b, Worker: 2, Object: 3},
			{Op: "write", Task: b, Worker: 2, Object: 3},
		}
		dr, derr := race.ReplayTaskEvents(dup, 8)
		c.Check(derr == nil, "dup record replays")
		c.Check(len(dr.Reports) == 1, "same object/kind/generations deduplicated, got %d", len(dr.Reports))

		// Promoted read frontier: earliest event index among real readers.
		r1 := race.TaskRef{ID: 11, Generation: 1}
		r2 := race.TaskRef{ID: 12, Generation: 1}
		r3 := race.TaskRef{ID: 13, Generation: 1}
		w := race.TaskRef{ID: 14, Generation: 1}
		promoted := []race.TaskEvent{
			{Op: "create", Task: r1}, {Op: "switch", Task: r1, Worker: 1}, {Op: "read", Task: r1, Worker: 1, Object: 77},
			{Op: "create", Task: r2}, {Op: "switch", Task: r2, Worker: 2}, {Op: "read", Task: r2, Worker: 2, Object: 77},
			{Op: "create", Task: r3}, {Op: "switch", Task: r3, Worker: 3}, {Op: "read", Task: r3, Worker: 3, Object: 77},
			{Op: "create", Task: w}, {Op: "switch", Task: w, Worker: 4}, {Op: "write", Task: w, Worker: 4, Object: 77},
		}
		pr, perr := race.ReplayTaskEvents(promoted, 8)
		c.Check(perr == nil, "promoted record replays: %v", perr)
		c.Check(len(pr.Reports) == 1, "one promoted read-write report")
		if len(pr.Reports) == 1 {
			p := pr.Reports[0]
			c.Check(p.Kind == "read-write", "read-write kind, got %q", p.Kind)
			c.Check(p.First == 2, "earliest frontier reader event 2, got %d", p.First)
			c.Check(p.Second == 11, "writer event 11, got %d", p.Second)
			c.Check(p.A == r1 && p.B == w, "reader/writer identity")
		}

		// Ordinary textual order of independent tasks is not HB.
		ord, oerr := r.Ordered(0, 3)
		c.Check(oerr == nil && !ord, "create A before create B textually is not HB")
		self, serr := r.Ordered(2, 2)
		c.Check(serr == nil && !self, "self query false")
		_, berr := r.Ordered(0, 99)
		c.Check(berr != nil, "out of range is error")

		// Error prefix: identity/binding/state checked before mutation.
		bad := []race.TaskEvent{
			{Op: "create", Task: a},
			{Op: "switch", Task: a, Worker: 1},
			{Op: "read", Task: a, Worker: 2, Object: 3},
			{Op: "write", Task: a, Worker: 2, Object: 3},
		}
		br, berr2 := race.ReplayTaskEvents(bad, 4)
		c.Check(berr2 != nil, "worker mismatch errors")
		c.Check(br.Accepted == 2, "only prefix accepted")
		c.Check(len(br.States) == 1, "prefix state present")
		_, oe := br.Ordered(0, 2)
		c.Check(oe != nil, "suffix index out of range on prefix result")

		unknown := []race.TaskEvent{{Op: "frobnicate", Task: a}}
		ur, uerr := race.ReplayTaskEvents(unknown, 4)
		c.Check(uerr != nil && ur.Accepted == 0, "unknown op rejects event 0")

		zeroObject := []race.TaskEvent{
			{Op: "create", Task: a},
			{Op: "switch", Task: a, Worker: 1},
			{Op: "read", Task: a, Worker: 1, Object: 0},
		}
		_, zerr := race.ReplayTaskEvents(zeroObject, 4)
		c.Check(zerr != nil, "zero object rejected for read")

		// Determinism and input/result ownership.
		same, _ := race.ReplayTaskEvents(events, 8)
		c.Check(reflect.DeepEqual(r, same), "same events give identical result")
		original := events[2].Object
		events[2].Object = 123456
		mutated, _ := race.ReplayTaskEvents(events, 8)
		c.Check(!reflect.DeepEqual(r, mutated), "mutated input changes new replay")
		c.Check(r.Reports[0].Object == 9, "old result unaffected by input mutation")
		events[2].Object = original

		// Churn replays (and their pooled clocks) must not alter old results.
		for i := 0; i < 25; i++ {
			_, _ = race.ReplayTaskEvents(promoted, 64)
		}
		c.Check(r.Reports[0].First == 2 && r.States[0].Task == a, "old result stable after pool churn")
	})
}
