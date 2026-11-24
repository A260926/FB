// Copyright 2015 go-fuzz project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package main

import (
	"bytes"
	"fmt"
	"os"
	"sort"
	"sync"
	"testing"

	. "github.com/dwyukov/go-fuzz/go-fuzz-defs"

	"github.com/dwyukov/go-fuzz/task_support/taskcheck"
)

func sigOf(data []byte) string {
	sig := hash(data)
	return fmt.Sprintf("%x", sig[:])
}

func coverWith(slots map[int]byte) []byte {
	c := make([]byte, CoverSize)
	for slot, b := range slots {
		c[slot] = b
	}
	return c
}

func ownerFor(view OwnershipView, slot int) (OwnershipOwner, bool) {
	for _, o := range view.Owners {
		if o.Slot == slot {
			return o, true
		}
	}
	return OwnershipOwner{}, false
}

func nodeFor(view OwnershipView, seed string) (OwnershipNode, bool) {
	for _, n := range view.Nodes {
		if n.Seed == seed {
			return n, true
		}
	}
	return OwnershipNode{}, false
}

// TestOwnershipDebt covers deterministic slot selection, owner
// replacement at equal buckets, bucket upgrades with debt preservation
// and the initial debt of newly appearing slots.
func TestOwnershipDebt(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		hub := authorOwnershipHub()
		c.Check(hub.EnableOwnership() == nil, "enable ownership")

		view := hub.OwnershipSnapshot()
		first, ok := ownerFor(view, 7)
		c.Check(ok && first.Seed == sigOf([]byte("A")) && first.Bucket == 1 && first.Debt == 0,
			"initial slot 7 owner=%v bucket=%v debt=%v", first.Seed, first.Bucket, first.Debt)
		c.Check(view.Issued == 0 && view.Completed == 0, "no claims yet")

		// First claim: debt increments from 0 to 1.
		t1, err := hub.NextOwnership()
		c.Check(err == nil && t1.ID == 1 && t1.Slot == 7 && t1.Parent == sigOf([]byte("A")) && t1.Bucket == 1,
			"first ticket parent=A slot=7")
		c.Check(bytes.Equal(t1.Data, []byte("A")), "ticket carries owner bytes")

		// Bucket upgrade: B reaches bucket 2, debt must be preserved.
		err = hub.ObserveOwnership(1, []byte("B"), coverWith(map[int]byte{7: 2}), nil, 0)
		c.Check(err == nil, "observe bucket upgrade")
		view = hub.OwnershipSnapshot()
		first, _ = ownerFor(view, 7)
		c.Check(first.Seed == sigOf([]byte("B")) && first.Bucket == 2 && first.Debt == 1,
			"upgrade keeps debt at 1, owner now B")
		c.Check(view.Completed == 1, "completed counter")

		// Equal bucket, longer seed cannot replace the shorter owner.
		t2, err := hub.NextOwnership()
		c.Check(err == nil && t2.ID == 2 && t2.Parent == sigOf([]byte("B")), "second ticket parent is current owner B")
		err = hub.ObserveOwnership(2, []byte("CC"), coverWith(map[int]byte{7: 2}), nil, 0)
		c.Check(err == nil, "observe equal-bucket longer seed")
		view = hub.OwnershipSnapshot()
		first, _ = ownerFor(view, 7)
		c.Check(first.Seed == sigOf([]byte("B")) && first.Debt == 2, "shorter B keeps slot, debt now 2")

		// Equal bucket, same length: winner is decided by SHA1, never order.
		t3, err := hub.NextOwnership()
		c.Check(err == nil && t3.ID == 3, "third ticket")
		err = hub.ObserveOwnership(3, []byte("0"), coverWith(map[int]byte{7: 2}), nil, 0)
		c.Check(err == nil, "observe same-length competitor")
		want := sigOf([]byte("B"))
		if sigOf([]byte("0")) < want {
			want = sigOf([]byte("0"))
		}
		view = hub.OwnershipSnapshot()
		first, _ = ownerFor(view, 7)
		c.Check(first.Seed == want && first.Debt == 3, "sha1 tie-break owner=%v debt preserved=3", want)

		// A brand new slot starts at the minimum debt of active slots.
		t4, err := hub.NextOwnership()
		c.Check(err == nil && t4.ID == 4, "fourth ticket")
		err = hub.ObserveOwnership(4, []byte("D"), coverWith(map[int]byte{7: 2, 9: 1}), nil, 0)
		c.Check(err == nil, "observe introduces slot 9")
		view = hub.OwnershipSnapshot()
		slot7, _ := ownerFor(view, 7)
		slot9, ok9 := ownerFor(view, 9)
		c.Check(ok9 && slot9.Seed == sigOf([]byte("D")) && slot9.Bucket == 1 && slot9.Debt == 4,
			"new slot inherits min active debt 4")
		c.Check(slot7.Debt == 4, "slot 7 debt advanced to 4")

		// Tie on debt is broken by slot number.
		t5, err := hub.NextOwnership()
		c.Check(err == nil && t5.Slot == 7, "equal debt picks lower slot number")
		t6, err := hub.NextOwnership()
		c.Check(err == nil && t6.Slot == 9, "indebted slot 9 is picked next")

		// A seed that loses every slot leaves no owner but keeps its node.
		_, err = hub.NextOwnership()
		c.Check(err == nil, "seventh claim")
		err = hub.ObserveOwnership(7, []byte("W"), coverWith(map[int]byte{7: 255, 9: 255}), nil, 0)
		c.Check(err == nil, "W takes both slots at the top bucket")
		view = hub.OwnershipSnapshot()
		for _, o := range view.Owners {
			c.Check(o.Seed == sigOf([]byte("W")), "only W owns slots after takeover")
		}
		n, ok := nodeFor(view, sigOf([]byte("D")))
		c.Check(ok && !n.Active, "displaced D node remains but is inactive")
	})
}

// TestOwnershipLineage covers parent-edge unions, success without new
// coverage, self edges, inactive-but-retained nodes, ordering
// convergence, idempotent/conflicting/late feedback, diagnostics and
// concurrent snapshots with detached slices.
func TestOwnershipLineage(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		hub := authorOwnershipHub()
		c.Check(hub.EnableOwnership() == nil, "enable ownership")

		// Successful result without new coverage still keeps the parent edge.
		t1, err := hub.NextOwnership()
		c.Check(err == nil && t1.Parent == sigOf([]byte("A")), "ticket 1 parent A")
		err = hub.ObserveOwnership(1, []byte("XX"), coverWith(map[int]byte{7: 1}), nil, 0)
		c.Check(err == nil, "observe no-new-coverage child")
		view := hub.OwnershipSnapshot()
		n, ok := nodeFor(view, sigOf([]byte("XX")))
		c.Check(ok && len(n.Parents) == 1 && n.Parents[0] == sigOf([]byte("A")), "parent edge retained without coverage")
		c.Check(!n.Active, "non-owner seed is inactive")
		slot7, _ := ownerFor(view, 7)
		c.Check(slot7.Seed == sigOf([]byte("A")), "slot stays with A at equal bucket and shorter length")

		// Y upgrades slot 7 and is parented by A.
		t2, err := hub.NextOwnership()
		c.Check(err == nil, "ticket 2")
		err = hub.ObserveOwnership(2, []byte("Y"), coverWith(map[int]byte{7: 2}), nil, 0)
		c.Check(err == nil, "Y upgrades slot 7, parent A")

		// Z opens slot 11; slot 11 is born at the current minimum debt.
		t3, err := hub.NextOwnership()
		c.Check(err == nil && t3.Parent == sigOf([]byte("Y")), "ticket 3 parent Y")
		err = hub.ObserveOwnership(3, []byte("Z"), coverWith(map[int]byte{7: 2, 11: 1}), nil, 0)
		c.Check(err == nil, "Z opens slot 11")

		// One claim on slot 7 makes slot 11 the least-indebted slot, so
		// the next claim deterministically parents from its owner Z.
		t4, err := hub.NextOwnership()
		c.Check(err == nil, "ticket 4")
		err = hub.ObserveOwnership(4, []byte("M"), coverWith(map[int]byte{7: 2}), nil, 0)
		c.Check(err == nil, "neutral observation advances slot 7 debt")
		t5, err := hub.NextOwnership()
		c.Check(err == nil && t5.Parent == sigOf([]byte("Z")), "ticket 5 parent Z via slot 11")
		// Same bytes as the existing Y node: it gains a second parent.
		err = hub.ObserveOwnership(5, []byte("Y"), coverWith(map[int]byte{7: 2, 11: 2}), nil, 0)
		c.Check(err == nil, "Y re-observed with a second parent")
		view = hub.OwnershipSnapshot()
		n, _ = nodeFor(view, sigOf([]byte("Y")))
		c.Check(len(n.Parents) == 2, "Y has two parents, got %v", n.Parents)
		c.Check(sort.StringsAreSorted(n.Parents), "parent edges are sorted")
		c.Check(n.Parents[0] == sigOf([]byte("A")) && n.Parents[1] == sigOf([]byte("Z")),
			"Y lineage is A and Z")

		// Self edges are ignored: feed back the ticket parent's own bytes.
		t6, err := hub.NextOwnership()
		c.Check(err == nil, "ticket 6")
		st := hub.ownershipState()
		parentBytes := st.records[t6.Parent].data
		parentNode, _ := nodeFor(hub.OwnershipSnapshot(), t6.Parent)
		parentsBefore := len(parentNode.Parents)
		err = hub.ObserveOwnership(6, parentBytes, coverWith(map[int]byte{7: 2, 11: 2}), nil, 0)
		c.Check(err == nil, "self observation accepted")
		view = hub.OwnershipSnapshot()
		n, _ = nodeFor(view, t6.Parent)
		c.Check(len(n.Parents) == parentsBefore, "no self edge added")

		// Nodes and edges survive after a seed loses every slot. Q reaches
		// the top bucket and takes both slots away from their owners.
		t7, err := hub.NextOwnership()
		c.Check(err == nil, "ticket 7")
		err = hub.ObserveOwnership(7, []byte("Q"), coverWith(map[int]byte{7: 255, 11: 255}), nil, 0)
		c.Check(err == nil, "Q takes over both slots at top bucket")
		view = hub.OwnershipSnapshot()
		for _, o := range view.Owners {
			c.Check(o.Seed == sigOf([]byte("Q")), "only Q owns slots after takeover")
		}
		n, ok = nodeFor(view, sigOf([]byte("Z")))
		c.Check(ok && !n.Active && len(n.Parents) == 1 && n.Parents[0] == sigOf([]byte("Y")),
			"inactive Z node and its edge remain")
		ro := hub.ro.Load().(*ROData)
		activeSigs := map[string]bool{}
		for _, inp := range ro.corpus {
			activeSigs[sigOf(inp.data)] = true
		}
		c.Check(activeSigs[sigOf([]byte("Q"))] && !activeSigs[sigOf([]byte("Z"))],
			"published corpus only contains active owners")

		// Negative results record node/parent edges but never coverage.
		beforeCover := append([]byte{}, ro.corpusCover...)
		t8, err := hub.NextOwnership()
		c.Check(err == nil, "ticket 8")
		shortCover := make([]byte, 3) // invalid length, but must be ignored for negative results
		err = hub.ObserveOwnership(8, []byte("NEG"), shortCover, nil, -1)
		c.Check(err == nil, "negative result ignores cover length")
		view = hub.OwnershipSnapshot()
		n, ok = nodeFor(view, sigOf([]byte("NEG")))
		c.Check(ok && !n.Active && len(n.Parents) == 1, "negative result keeps a lineage node only")
		ro = hub.ro.Load().(*ROData)
		c.Check(bytes.Equal(ro.corpusCover, beforeCover), "negative result did not touch coverage")

		// Duplicate identical feedback is idempotent; conflicts error and
		// leave state untouched; unknown ids are rejected.
		completed := hub.OwnershipSnapshot().Completed
		err = hub.ObserveOwnership(8, []byte("NEG"), shortCover, nil, -1)
		c.Check(err == nil, "duplicate feedback is idempotent")
		c.Check(hub.OwnershipSnapshot().Completed == completed, "duplicate does not recount")
		err = hub.ObserveOwnership(8, []byte("OTHER"), coverWith(map[int]byte{7: 1}), nil, 0)
		c.Check(err == errOwnershipConflict, "conflicting feedback errors, got %v", err)
		c.Check(hub.OwnershipSnapshot().Completed == completed, "conflict changes nothing")
		err = hub.ObserveOwnership(4242, []byte("LATE"), coverWith(map[int]byte{7: 1}), nil, 0)
		c.Check(err == errOwnershipUnknownID, "unknown ticket id errors")
		// A fresh, uncompleted ticket with a wrong-length cover is rejected.
		fresh, err := hub.NextOwnership()
		c.Check(err == nil, "fresh ticket for bad-cover check")
		err = hub.ObserveOwnership(fresh.ID, []byte("XX"), make([]byte, CoverSize-1), nil, 0)
		c.Check(err == errOwnershipBadCover, "wrong cover length errors")
		c.Check(hub.OwnershipSnapshot().Completed == completed, "rejected feedback changes nothing")

		// Enable is once-only and cannot follow claims.
		c.Check(hub.EnableOwnership() == errOwnershipAfterClaim, "second enable after claims fails")

		// Enabling without any covered slot fails without half state.
		empty := authorOwnershipHub()
		emptyRO := empty.ro.Load().(*ROData)
		emptyRO.corpus[0].cover = make([]byte, CoverSize)
		emptyRO.corpusCover = make([]byte, CoverSize)
		c.Check(empty.EnableOwnership() == errOwnershipNoOwner, "enable without coverage fails")
		c.Check(empty.ownershipState() == nil, "failed enable leaves no scheduler state")
		_, err = empty.NextOwnership()
		c.Check(err == errOwnershipNotEnabled, "no claims after failed enable")

		// Diagnostics: classified via extractSuppression, never coverage,
		// representative is the shortest seed (then SHA1), not first-come.
		t9, err := hub.NextOwnership()
		c.Check(err == nil, "ticket 9")
		diagText := []byte("panic: boom\n")
		err = hub.ObserveOwnership(t9.ID, []byte("LONGDIAG"), nil, diagText, 0)
		c.Check(err == nil, "diagnostic observed")
		view = hub.OwnershipSnapshot()
		c.Check(len(view.Diagnostics) == 1, "one diagnostic class")
		key := string(extractSuppression(diagText))
		c.Check(view.Diagnostics[0].Key == key && view.Diagnostics[0].Seed == sigOf([]byte("LONGDIAG")),
			"diagnostic key and first representative")
		ro = hub.ro.Load().(*ROData)
		c.Check(bytes.Equal(ro.corpusCover, beforeCover), "diagnostic contributed no coverage")
		t10, err := hub.NextOwnership()
		c.Check(err == nil, "ticket 10")
		err = hub.ObserveOwnership(t10.ID, []byte("D"), nil, diagText, 0)
		c.Check(err == nil, "shorter diagnostic representative observed")
		view = hub.OwnershipSnapshot()
		c.Check(len(view.Diagnostics) == 1 && view.Diagnostics[0].Seed == sigOf([]byte("D")),
			"shortest seed becomes representative, not first-come")

		// Arrival-order convergence with no claims interleaved: all claims
		// are issued first, then the same feedback is committed in two
		// different orders. Owners and lineage must be identical.
		build := func(order []uint64) OwnershipView {
			h := authorOwnershipHub()
			if err := h.EnableOwnership(); err != nil {
				t.Fatal(err)
			}
			type fb struct {
				id   uint64
				data []byte
				cov  []byte
			}
			feedbacks := []fb{
				{1, []byte("Y"), coverWith(map[int]byte{7: 2})},
				{2, []byte("Z"), coverWith(map[int]byte{7: 2, 11: 1})},
				{3, []byte("Q"), coverWith(map[int]byte{7: 255, 11: 255})},
			}
			for range feedbacks {
				if _, err := h.NextOwnership(); err != nil {
					t.Fatal(err)
				}
			}
			for _, idx := range order {
				f := feedbacks[idx]
				if err := h.ObserveOwnership(f.id, f.data, f.cov, nil, 0); err != nil {
					t.Fatal(err)
				}
			}
			return h.OwnershipSnapshot()
		}
		v1 := build([]uint64{0, 1, 2})
		v2 := build([]uint64{2, 1, 0})
		c.Check(viewsEqualOwners(v1.Owners, v2.Owners), "owners converge regardless of arrival order")
		c.Check(viewsEqualNodes(v1.Nodes, v2.Nodes), "lineage converges regardless of arrival order")

		// Concurrent claims and observations must never produce a mixed
		// snapshot; returned slices must not alias internal state.
		parHub := authorOwnershipHub()
		c.Check(parHub.EnableOwnership() == nil, "enable ownership for parallel run")
		var wg sync.WaitGroup
		for g := 0; g < 4; g++ {
			wg.Add(1)
			go func(g int) {
				defer wg.Done()
				for i := 0; i < 25; i++ {
					ticket, err := parHub.NextOwnership()
					if err != nil {
						t.Errorf("next: %v", err)
						return
					}
					data := []byte(fmt.Sprintf("g%di%d", g, i))
					cov := coverWith(map[int]byte{7: 1, 11 + (i % 4): 1})
					if err := parHub.ObserveOwnership(ticket.ID, data, cov, nil, 0); err != nil {
						t.Errorf("observe: %v", err)
						return
					}
					snap := parHub.OwnershipSnapshot()
					if !sort.SliceIsSorted(snap.Owners, func(a, b int) bool {
						return snap.Owners[a].Slot < snap.Owners[b].Slot
					}) {
						t.Errorf("owners not sorted by slot")
						return
					}
					if !sort.SliceIsSorted(snap.Nodes, func(a, b int) bool {
						return snap.Nodes[a].Seed < snap.Nodes[b].Seed
					}) {
						t.Errorf("nodes not sorted by seed")
						return
					}
				}
			}(g)
		}
		wg.Wait()
		snap := parHub.OwnershipSnapshot()
		c.Check(snap.Issued == snap.Completed && snap.Issued == 100, "all concurrent claims confirmed")
		c.Check(sort.SliceIsSorted(snap.Diagnostics, func(a, b int) bool {
			return snap.Diagnostics[a].Key < snap.Diagnostics[b].Key
		}), "diagnostics sorted by key")

		// Mutating returned values must not corrupt internal state.
		ticket, err := parHub.NextOwnership()
		c.Check(err == nil, "claim after concurrent run")
		ticket.Data[0] ^= 0xff
		for i := range snap.Owners {
			snap.Owners[i].Debt = 999
		}
		for i := range snap.Nodes {
			if len(snap.Nodes[i].Parents) > 0 {
				snap.Nodes[i].Parents[0] = "corrupted"
			}
		}
		again := parHub.OwnershipSnapshot()
		c.Check(again.Issued == snap.Issued+1, "snapshot mutation did not alias counters")
		clean, _ := ownerFor(again, 7)
		c.Check(clean.Debt != 999, "owner slice is detached from internal state")
		for _, nd := range again.Nodes {
			for _, p := range nd.Parents {
				c.Check(p != "corrupted", "parent slice is detached from internal state")
			}
		}
	})
}

func viewsEqualOwners(a, b []OwnershipOwner) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func viewsEqualNodes(a, b []OwnershipNode) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i].Seed != b[i].Seed || a[i].Active != b[i].Active ||
			len(a[i].Parents) != len(b[i].Parents) {
			return false
		}
		for j := range a[i].Parents {
			if a[i].Parents[j] != b[i].Parents[j] {
				return false
			}
		}
	}
	return true
}

// TestOwnershipWorker drives the real benign testee subprocess through
// the production Worker.OwnershipStep path: one mutation, one target
// call and three triage observations per step, with real parent edges.
func TestOwnershipWorker(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		if os.Getenv("GOFUZZ_BENIGN_BIN") == "" {
			t.Fatal("missing benign testee")
		}
		hub := authorOwnershipHub()
		c.Check(hub.EnableOwnership() == nil, "enable ownership")
		w := authorOwnershipWorker(t, hub)

		const steps = 4
		var lastTicket OwnershipTicket
		for step := 0; step < steps; step++ {
			fuzzBefore := w.execs[execFuzz]
			triageBefore := w.execs[execTriageInput]
			ticket, err := w.OwnershipStep()
			c.Check(err == nil, "step %v completes: %v", step, err)
			c.Check(w.execs[execFuzz]-fuzzBefore == 1, "step %v makes exactly one fuzz call", step)
			c.Check(w.execs[execTriageInput]-triageBefore == 3, "step %v runs three triage observations", step)
			c.Check(ticket.ID == uint64(step+1), "ticket ids increase from 1 and are never reused")
			lastTicket = ticket
		}
		c.Check(w.execs[execFuzz] == steps, "total fuzz calls %v", w.execs[execFuzz])
		c.Check(w.execs[execTriageInput] == 3*steps, "total triage observations %v", w.execs[execTriageInput])
		c.Check(w.execs[execSonar] == 0 && w.execs[execSmash] == 0 && w.execs[execVersifier] == 0,
			"sonar, smash and versifier never run in ownership mode")

		view := hub.OwnershipSnapshot()
		c.Check(view.Issued == steps && view.Completed == steps, "all claims confirmed")
		c.Check(len(view.Owners) >= 1, "benign target always keeps slot 7 owned")
		c.Check(len(view.Nodes) >= 2, "mutated children created lineage nodes")

		// Real parent edges: every edge must point at a known node, and at
		// least one edge must exist in the recorded lineage.
		known := map[string]bool{}
		for _, nd := range view.Nodes {
			known[nd.Seed] = true
		}
		edgeFound := false
		for _, nd := range view.Nodes {
			for _, p := range nd.Parents {
				c.Check(known[p], "parent edge points to a known node")
				if known[p] {
					edgeFound = true
				}
			}
		}
		c.Check(edgeFound, "at least one real parent edge was recorded")
		c.Check(known[lastTicket.Parent], "last ticket parent is a known lineage node")

		// Published corpus is exactly the active owner set and the bucket
		// table matches the scheduling view.
		ro := hub.ro.Load().(*ROData)
		active := map[string]bool{}
		for _, o := range view.Owners {
			active[o.Seed] = true
		}
		c.Check(len(ro.corpus) == countActive(view.Nodes), "corpus size matches active nodes")
		for _, inp := range ro.corpus {
			c.Check(active[sigOf(inp.data)], "corpus only holds current owners")
		}
		for _, o := range view.Owners {
			c.Check(ro.corpusCover[o.Slot] == o.Bucket, "bucket table matches scheduling view at slot %v", o.Slot)
		}
		c.Check(bytes.Equal(hub.maxCover.Load().([]byte), ro.corpusCover), "maxCover mirrors the bucket table")
	})
}

func countActive(nodes []OwnershipNode) int {
	n := 0
	for _, node := range nodes {
		if node.Active {
			n++
		}
	}
	return n
}
