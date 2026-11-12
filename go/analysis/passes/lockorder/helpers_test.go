// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package lockorder_test

import (
	"go/token"
	"strings"
	"testing"

	"golang.org/x/tools/go/analysis"
	"golang.org/x/tools/go/analysis/passes/lockorder"
)

// edgePos is one expected ordering edge. Endpoint names, the exact
// line/column of the acquisition, and the crossed call sites are written
// independently of the sample source so a failure pinpoints what moved.
type edgePos struct {
	from  string
	to    string
	line  int
	col   int
	chain [][2]int // {line, column} of each traversed call site
}

func projectEdges(t *testing.T, res *lockorder.Result, fset *token.FileSet) []edgePos {
	t.Helper()
	var got []edgePos
	for _, ed := range res.Edges {
		p := fset.Position(ed.LockPos)
		e := edgePos{from: ed.From.String(), to: ed.To.String(), line: p.Line, col: p.Column}
		for _, c := range ed.Chain {
			cp := fset.Position(c)
			e.chain = append(e.chain, [2]int{cp.Line, cp.Column})
		}
		got = append(got, e)
	}
	sortEdges(got)
	return got
}

func sortEdges(s []edgePos) {
	less := func(i, j int) bool {
		if s[i].from != s[j].from {
			return s[i].from < s[j].from
		}
		if s[i].to != s[j].to {
			return s[i].to < s[j].to
		}
		if s[i].line != s[j].line {
			return s[i].line < s[j].line
		}
		return s[i].col < s[j].col
	}
	// Insertion sort keeps expectations readable; lists are tiny.
	for i := 1; i < len(s); i++ {
		for j := i; j > 0 && less(j, j-1); j-- {
			s[j], s[j-1] = s[j-1], s[j]
		}
	}
}

func assertEdges(t *testing.T, got, want []edgePos) {
	t.Helper()
	sortEdges(want)
	if len(got) != len(want) {
		t.Fatalf("edge count = %d\n got %v\nwant %v", len(got), got, want)
	}
	for i := range want {
		if !edgesEqual(got[i], want[i]) {
			t.Errorf("edge %d = %+v, want %+v", i, got[i], want[i])
		}
	}
}

func edgesEqual(a, b edgePos) bool {
	if a.from != b.from || a.to != b.to || a.line != b.line || a.col != b.col {
		return false
	}
	if len(a.chain) != len(b.chain) {
		return false
	}
	for i := range a.chain {
		if a.chain[i] != b.chain[i] {
			return false
		}
	}
	return true
}

// edgeKey is a comparable, position-stable fingerprint of one result edge.
type edgeKey struct {
	from, to string
	line     int
	col      int
}

func edgeKeys(res *lockorder.Result, fset *token.FileSet) []edgeKey {
	var out []edgeKey
	for _, ed := range res.Edges {
		p := fset.Position(ed.LockPos)
		out = append(out, edgeKey{from: ed.From.String(), to: ed.To.String(), line: p.Line, col: p.Column})
	}
	return out
}

// assertSameEdgeSet fails unless the two runs report the same edges.
func assertSameEdgeSet(t *testing.T, a, b []edgeKey) {
	t.Helper()
	counts := make(map[edgeKey]int)
	for _, k := range a {
		counts[k]++
	}
	for _, k := range b {
		counts[k]--
	}
	for k, n := range counts {
		if n != 0 {
			t.Fatalf("edge sets differ at %+v (delta %d):\n a=%v\n b=%v", k, n, a, b)
		}
	}
}

// cycleMessages returns the messages of all cycle diagnostics.
func cycleMessages(diags []analysis.Diagnostic) []string {
	var out []string
	for _, d := range diags {
		if d.Category == "lock-order-cycle" {
			out = append(out, d.Message)
		}
	}
	return out
}

func assertNoCycles(t *testing.T, diags []analysis.Diagnostic) {
	t.Helper()
	if msgs := cycleMessages(diags); len(msgs) != 0 {
		t.Fatalf("expected no cycle, got %v", msgs)
	}
}

func assertHasCycle(t *testing.T, diags []analysis.Diagnostic, sub string) {
	t.Helper()
	for _, msg := range cycleMessages(diags) {
		if strings.Contains(msg, sub) {
			return
		}
	}
	t.Fatalf("expected a cycle diagnostic containing %q; got %v", sub, cycleMessages(diags))
}

// assertIncompleteReason fails unless at least one incompleteness finding
// mentions reasonSub.
func assertIncompleteReason(t *testing.T, res *lockorder.Result, reasonSub string) {
	t.Helper()
	for _, inc := range res.Incomplete {
		if strings.Contains(inc.Reason, reasonSub) {
			return
		}
	}
	t.Fatalf("expected an incompleteness finding containing %q; got %v", reasonSub, res.Incomplete)
}
