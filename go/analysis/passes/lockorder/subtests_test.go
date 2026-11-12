// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package lockorder_test

import (
	"strings"
	"testing"
)

// testAliases covers lock identity through local aliases, address
// expressions, definite field paths, overwriting assignments, distinct
// go/types bindings for same-named block variables, and the constructs that
// must force incompleteness instead of a guessed identity.
func testAliases(t *testing.T) {
	res, fset, diags := runLockOrder(t, "contract/aliases.go", "example.com/aliases")
	got := projectEdges(t, res, fset)

	// Both straight-line functions and the loop establish the same single
	// global order; aliasLocks reaches gmu only through the
	// p := &gmu; q := p chain. The loop back edge converges at a fixed point
	// without inventing a reverse edge.
	want := []edgePos{
		{from: "gmu", to: "gholder.mu", line: 33, col: 2},
		{from: "gmu", to: "gholder.mu", line: 43, col: 2},
		{from: "gmu", to: "gholder.mu", line: 155, col: 3},
	}
	assertEdges(t, got, want)
	assertNoCycles(t, diags)

	// The user-defined Lock/Unlock methods and the mutually exclusive
	// branches contribute no mutex edges at all.
	for _, e := range got {
		if e.from == "arg0" || e.to == "arg0" {
			t.Errorf("parameter vocabulary leaked into the package-level graph: %+v", e)
		}
	}

	// Unknown index, heap allocation, goroutine/closure write, and interface
	// dispatch each mark the function incomplete.
	assertIncompleteReason(t, res, "no definite identity")
	assertIncompleteReason(t, res, "goroutine")
	assertIncompleteReason(t, res, "dynamic dispatch")
	// RWMutex, the TryLock family, and lock calls inside branch conditions
	// are explicit boundaries rather than silently treated as Mutex locks.
	assertIncompleteReason(t, res, "RWMutex")
	assertIncompleteReason(t, res, "TryLock")
	assertIncompleteReason(t, res, "branch condition")
}

// testSummaries covers parameterized helper summaries: slot substitution at
// distinct call sites, a helper's internal order, caller-held locks ordering
// against helper acquisitions, local/global mixing, the helper lock
// discipline, cross-package calls, and recursive summaries reaching a fixed
// point (including structurally unbounded field growth).
func testSummaries(t *testing.T) {
	res, fset, diags := runLockOrder(t, "contract/summaries.go", "example.com/summaries")
	got := projectEdges(t, res, fset)

	want := []edgePos{
		// nested(): caller-held mu1 orders against pair's first acquisition.
		{from: "mu1", to: "mu2", line: 21, col: 2, chain: [][2]int{{38, 2}}},
		// sameLock(): pair(&mu1, &mu1) produces the A->A self loop.
		{from: "mu1", to: "mu1", line: 22, col: 2, chain: [][2]int{{33, 2}}},
		// callerSlots(): pair's internal edge at the first call site.
		{from: "mu1", to: "mu2", line: 22, col: 2, chain: [][2]int{{28, 2}}},
		// nested(): mu1 held against pair's second acquisition.
		{from: "mu1", to: "mu3", line: 22, col: 2, chain: [][2]int{{38, 2}}},
		// callerSlots(): second call substitutes mu2, mu3 into the same
		// summary without mixing arguments with the first call.
		{from: "mu2", to: "mu3", line: 22, col: 2, chain: [][2]int{{29, 2}}},
		// callMixed(): the addressable local b substitutes the box parameter;
		// the local-root edge b.mu -> mu1 survives inlining.
		{from: "b.mu", to: "mu1", line: 44, col: 2, chain: [][2]int{{51, 2}}},
		// callBad(): direct straight-line acquisitions.
		{from: "mu1", to: "mu2", line: 60, col: 2},
		// recFinite(): the finite recursion converges without any depth cap
		// and still reports its provable edge.
		{from: "mu1", to: "mu2", line: 77, col: 2},
	}
	assertEdges(t, got, want)

	// The self loop from passing one lock into both helper slots is a risk.
	assertHasCycle(t, diags, "mu1 -> mu1")

	assertIncompleteReason(t, res, "does not release every lock")
	assertIncompleteReason(t, res, "cross-package")
	assertIncompleteReason(t, res, "grows lock field paths")

	// Package isolation: analyze a different package in between two runs of
	// this one. Each Pass builds a fresh engine, so summaries and edges from
	// the intervening package can neither appear here nor change this run.
	other, _, _ := runLockOrder(t, "contract/aliases.go", "example.com/aliases")
	for _, ed := range other.Edges {
		if ed.From.String() == "mu1" || ed.To.String() == "mu1" {
			t.Fatalf("summaries lock name leaked into the aliases package: %s -> %s", ed.From, ed.To)
		}
	}
	again, fsetAgain, _ := runLockOrder(t, "contract/summaries.go", "example.com/summaries")
	assertSameEdgeSet(t,
		edgeKeys(res, fset),
		edgeKeys(again, fsetAgain),
	)

	// Helper lock discipline: helpers must not unlock a caller's lock or a
	// lock they never acquired, while a conforming helper stays clean.
	disc, _, discDiags := runLockOrder(t, "contract/discipline.go", "example.com/discipline")
	assertIncompleteReason(t, disc, "not held")
	assertIncompleteReason(t, disc, "did not acquire")
	// Passing the same lock into both slots of the clean helper is a real
	// A->A self-loop risk and must still be reported despite the discipline
	// findings elsewhere in the package.
	assertHasCycle(t, discDiags, "dmu -> dmu")
}

// testDeferred covers the rule that a deferred mutex Unlock captures its
// receiver at registration, does not release at the registration point, and
// is unaffected by later repointing of the local alias. Other defer forms
// mark the function incomplete.
func testDeferred(t *testing.T) {
	res, fset, diags := runLockOrder(t, "contract/deferred.go", "example.com/deferred")
	got := projectEdges(t, res, fset)

	want := []edgePos{
		// deferBasic: da is still held when db is acquired.
		{from: "da", to: "db", line: 10, col: 2},
		// deferRepoint: the defer captured da; repointing p to db does not
		// change the deferred unlock, so da is still held at this Lock.
		{from: "da", to: "db", line: 19, col: 2},
	}
	assertEdges(t, got, want)
	assertNoCycles(t, diags)

	for _, e := range got {
		if e.from == "db" {
			t.Errorf("db must never be the outer lock: %+v", e)
		}
	}
	assertIncompleteReason(t, res, "non-mutex defer")
}

// testCycles covers cycle reporting over concrete package-level locks:
// direct opposite orders, an order reached through an inlined helper with its
// call chain attached, a self loop, and the rule that mutually exclusive
// branches never combine into a double-lock holding.
func testCycles(t *testing.T) {
	res, fset, diags := runLockOrder(t, "contract/cycles.go", "example.com/cycles")
	got := projectEdges(t, res, fset)

	want := []edgePos{
		{from: "A", to: "B", line: 9, col: 2},
		{from: "B", to: "A", line: 16, col: 2},
		// viaCall(): the helper's internal edge, rewritten at the call site.
		{from: "B", to: "A", line: 23, col: 2, chain: [][2]int{{29, 2}}},
		// selfLoop(): helper(&C, &C) is an A->A risk with the call chain.
		{from: "C", to: "C", line: 23, col: 2, chain: [][2]int{{33, 2}}},
	}
	assertEdges(t, got, want)

	assertHasCycle(t, diags, "A -> B -> A")
	assertHasCycle(t, diags, "C -> C")

	// Every cycle diagnostic must carry the real acquisition points, and the
	// self-loop diagnostic must also carry the traversed call site.
	for _, d := range diags {
		if d.Category != "lock-order-cycle" {
			continue
		}
		if len(d.Related) == 0 {
			t.Fatalf("cycle diagnostic has no related positions: %s", d.Message)
		}
		if strings.Contains(d.Message, "C -> C") {
			foundCall := false
			for _, r := range d.Related {
				if fset.Position(r.Pos).Line == 33 {
					foundCall = true
				}
			}
			if !foundCall {
				t.Errorf("self-loop diagnostic missing call-site position; got %+v", d.Related)
			}
		}
	}

	// branchOnly only ever holds one lock per path: it contributes no edges.
	if n := len(got); n != len(want) {
		t.Fatalf("branch-only code must add no edges; got %d want %d", n, len(want))
	}
	if len(res.Incomplete) != 0 {
		t.Errorf("the cycles sample should be fully analyzed; got %v", res.Incomplete)
	}
}
