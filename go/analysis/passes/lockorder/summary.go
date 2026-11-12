// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package lockorder

import (
	"go/ast"
	"go/token"
	"sort"

	"golang.org/x/tools/go/analysis"
)

// computeSCCs assigns a Tarjan strongly connected component id to every
// function over the static intra-package call graph. Recursive and mutually
// recursive functions share an id with at least one other member (or form a
// singleton carrying a self edge).
func (e *engine) computeSCCs() {
	graph := e.callGraph()
	index := 0
	indices := make(map[*funcInfo]int)
	low := make(map[*funcInfo]int)
	onStack := make(map[*funcInfo]bool)
	var stack []*funcInfo
	sccID := 0

	var strongConnect func(v *funcInfo)
	strongConnect = func(v *funcInfo) {
		indices[v] = index
		low[v] = index
		index++
		stack = append(stack, v)
		onStack[v] = true

		for _, w := range graph[v] {
			if _, ok := indices[w]; !ok {
				strongConnect(w)
				if low[w] < low[v] {
					low[v] = low[w]
				}
			} else if onStack[w] && indices[w] < low[v] {
				low[v] = indices[w]
			}
		}

		if low[v] == indices[v] {
			for {
				w := stack[len(stack)-1]
				stack = stack[:len(stack)-1]
				onStack[w] = false
				w.scc = sccID
				if w == v {
					break
				}
			}
			sccID++
		}
	}
	for _, fi := range e.order {
		if _, ok := indices[fi]; !ok {
			strongConnect(fi)
		}
	}
}

// substitution maps a callee's abstract lock vocabulary onto the caller's
// vocabulary at one call site. A slot whose actual argument has no definite
// identity is absent.
type substitution struct {
	slots   map[int]lockID // callee parameter slot -> caller lock identity
	growing bool           // a same-SCC mapping grows a field path structurally
	cap     int            // maximum composed field-path length
}

// buildSubstitution evaluates the actual arguments of call against the
// caller's frame.
func (m *machine) buildSubstitution(call *ast.CallExpr, callee *funcInfo, state *frame) substitution {
	sub := substitution{slots: make(map[int]lockID), cap: m.e.maxFieldDepth + 1}
	for i := range callee.params {
		var arg ast.Expr
		if i < len(call.Args) {
			arg = call.Args[i]
		} else if i == 0 && callee.sig.Recv() != nil {
			// Method call: receiver is the selector's X.
			if sel, ok := call.Fun.(*ast.SelectorExpr); ok {
				arg = sel.X
			}
		}
		if arg == nil {
			continue
		}
		loc, ok := m.resolve(arg, state)
		if !ok {
			continue
		}
		sub.slots[i] = loc.id

		// Structural growth: a recursive call such as f(p.next) feeds a
		// parameter-rooted identity carrying a field path back into the same
		// SCC. Exact summaries over the resulting infinite identity set are
		// not required; the chain is explicitly marked incomplete.
		if callee.scc == m.fi.scc && loc.id.kind == rootParam && len(loc.id.fields) > 0 {
			sub.growing = true
		}
	}
	return sub
}

// apply maps a callee-vocabulary lock identity through the substitution.
// Package-level globals and local roots are intrinsic and pass through. A
// parameter slot without a definite actual identity yields false. Field paths
// are composed as ordinary definite field paths; structurally unbounded
// recursive growth is cut at the cap, the call having already been marked
// incomplete.
func (s substitution) apply(id lockID) (lockID, bool) {
	if id.kind == rootGlobal || id.kind == rootLocal {
		return id, true
	}
	base, ok := s.slots[id.slot]
	if !ok {
		return lockID{}, false
	}
	out := base
	for _, f := range id.fields {
		if s.growing && len(out.fields) >= s.cap {
			return lockID{}, false
		}
		out = out.extend(f)
	}
	return out, true
}

// inlineCall incorporates a static call to a package-local helper.
func (m *machine) inlineCall(call *ast.CallExpr, callee *funcInfo, state *frame) {
	sub := m.buildSubstitution(call, callee, state)
	if sub.growing {
		m.summary.unbounded = true
		m.markIncomplete(call.Pos(), "recursive call grows lock field paths structurally; this call chain is not fully analyzed")
	}
	if callee.summary.incomplete {
		m.markIncomplete(call.Pos(), "called function "+callee.fn.Name()+" is itself analysis-incomplete")
	}
	if callee.summary.badHelper {
		m.bad = true
		m.markIncomplete(call.Pos(), "called function "+callee.fn.Name()+" does not release every lock it acquires")
	}
	m.summary.unbounded = m.summary.unbounded || callee.summary.unbounded

	// Internal ordering edges of the callee, rewritten at this call site.
	for _, e := range callee.summary.edges {
		from, ok1 := sub.apply(e.from)
		to, ok2 := sub.apply(e.to)
		if !ok1 || !ok2 {
			continue
		}
		chain := make([]token.Pos, 0, len(e.chain)+1)
		chain = append(chain, call.Pos())
		chain = append(chain, e.chain...)
		m.summary.addEdge(sumEdge{from: from, to: to, lockPos: e.lockPos, chain: chain})
	}

	// Locks the caller already held order against every lock the callee
	// acquires; the callee's own acquisitions carry its real Lock positions.
	for _, a := range callee.summary.acquired {
		to, ok := sub.apply(a.id)
		if !ok {
			continue
		}
		for _, h := range state.held {
			chain := make([]token.Pos, 0, len(h.chain)+1)
			chain = append(chain, call.Pos())
			chain = append(chain, h.chain...)
			m.summary.addEdge(sumEdge{from: h.id, to: to, lockPos: a.pos, chain: chain})
		}
	}
}

// checkReturnState enforces the helper contract on one normal return path:
// every lock the function itself acquired and still holds must be covered by
// a deferred Unlock registered in this function. Locks the caller already
// held on entry are not the helper's responsibility. Unlocking a lock that is
// not held (see doUnlock) independently violates the contract.
func (m *machine) checkReturnState(state *frame) {
	deferred := make(map[string]bool)
	for _, d := range state.defers {
		deferred[d.id.key()] = true
	}
	for _, h := range state.held {
		if _, acquired := m.summary.acquired[h.id.key()]; !acquired {
			continue // inherited from the caller
		}
		if deferred[h.id.key()] {
			continue // released on function exit by a defer
		}
		m.bad = true
		m.markIncomplete(h.lockPos, "helper returns while still holding a lock it acquired")
	}
}

// collectEdges lifts the converged per-function summaries into one
// package-level edge set. Only concrete lock identities (package-level
// variables and addressable locals) enter this graph: parameter-slot edges
// are a function's private vocabulary and become concrete solely through the
// argument substitution performed while inlining. Edges are deduplicated by
// endpoint pair and the real Lock position.
func (e *engine) collectEdges() {
	for _, fi := range e.order {
		for _, ed := range fi.summary.edges {
			if ed.from.kind == rootParam || ed.to.kind == rootParam {
				continue
			}
			k := edgeKey{from: ed.from.key(), to: ed.to.key(), pos: ed.lockPos}
			if _, ok := e.allEdges[k]; !ok {
				e.allEdges[k] = Edge{
					From:    ed.from,
					To:      ed.to,
					LockPos: ed.lockPos,
					Chain:   append([]token.Pos(nil), ed.chain...),
				}
			}
		}
	}
}

// cycleDiagnostics finds every strongly connected component of the lock graph
// that contains a cycle (including a self loop) and emits one stable
// diagnostic per component.
func (e *engine) cycleDiagnostics() []analysis.Diagnostic {
	// Build adjacency over distinct endpoint pairs, keeping the
	// lexicographically smallest representative edge deterministically.
	nodes := make(map[string]lockID)
	adj := make(map[string]map[string]Edge)
	for _, ed := range e.allEdges {
		fk, tk := ed.From.key(), ed.To.key()
		nodes[fk] = ed.From
		nodes[tk] = ed.To
		if adj[fk] == nil {
			adj[fk] = make(map[string]Edge)
		}
		if prev, ok := adj[fk][tk]; !ok || ed.LockPos < prev.LockPos {
			adj[fk][tk] = ed
		}
	}

	sccs := tarjanLockGraph(nodes, adj)
	var diags []analysis.Diagnostic
	for _, memberKeys := range sccs {
		cycle, ok := findCycle(memberKeys, adj)
		if !ok {
			continue // singleton without a self loop
		}
		diags = append(diags, e.cycleDiagnostic(cycle, adj, nodes))
	}
	return diags
}

// tarjanLockGraph returns the SCCs of the lock graph in a deterministic
// order independent of map iteration.
func tarjanLockGraph(nodes map[string]lockID, adj map[string]map[string]Edge) [][]string {
	keys := make([]string, 0, len(nodes))
	for k := range nodes {
		keys = append(keys, k)
	}
	sort.Strings(keys)

	index := 0
	indices := make(map[string]int)
	low := make(map[string]int)
	onStack := make(map[string]bool)
	var stack []string
	var sccs [][]string

	var visit func(v string)
	visit = func(v string) {
		indices[v] = index
		low[v] = index
		index++
		stack = append(stack, v)
		onStack[v] = true

		neighbors := sortedNeighbors(adj[v])
		for _, w := range neighbors {
			if _, ok := indices[w]; !ok {
				visit(w)
				if low[w] < low[v] {
					low[v] = low[w]
				}
			} else if onStack[w] && indices[w] < low[v] {
				low[v] = indices[w]
			}
		}

		if low[v] == indices[v] {
			var scc []string
			for {
				w := stack[len(stack)-1]
				stack = stack[:len(stack)-1]
				onStack[w] = false
				scc = append(scc, w)
				if w == v {
					break
				}
			}
			sort.Strings(scc)
			sccs = append(sccs, scc)
		}
	}
	for _, k := range keys {
		if _, ok := indices[k]; !ok {
			visit(k)
		}
	}
	// Stable component order: smallest node key first.
	sort.Slice(sccs, func(i, j int) bool { return sccs[i][0] < sccs[j][0] })
	return sccs
}

func sortedNeighbors(m map[string]Edge) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// findCycle returns a stable closed path through the SCC. For a self loop the
// path has one node; otherwise the path lists the nodes of the cycle in order
// without repeating the first node at the end.
func findCycle(scc []string, adj map[string]map[string]Edge) ([]string, bool) {
	member := make(map[string]bool, len(scc))
	for _, k := range scc {
		member[k] = true
	}
	if len(scc) == 1 {
		k := scc[0]
		if _, ok := adj[k][k]; ok {
			return []string{k}, true
		}
		return nil, false
	}

	start := scc[0]
	var path []string
	onPath := make(map[string]bool)
	var dfs func(v string) bool
	dfs = func(v string) bool {
		path = append(path, v)
		onPath[v] = true
		for _, w := range sortedNeighbors(adj[v]) {
			if !member[w] {
				continue
			}
			if w == start && len(path) >= 2 {
				return true
			}
			if !onPath[w] && dfs(w) {
				return true
			}
		}
		path = path[:len(path)-1]
		onPath[v] = false
		return false
	}
	if dfs(start) {
		return path, true
	}
	return nil, false
}

// cycleDiagnostic builds the diagnostic for one closed path, attaching every
// edge's real acquisition point and the call chain crossed to reach it.
func (e *engine) cycleDiagnostic(cycle []string, adj map[string]map[string]Edge, nodes map[string]lockID) analysis.Diagnostic {
	var names []string
	for _, k := range cycle {
		names = append(names, nodes[k].String())
	}
	var firstEdge Edge
	if len(cycle) == 1 {
		firstEdge = adj[cycle[0]][cycle[0]]
	} else {
		firstEdge = adj[cycle[0]][cycle[1]]
	}

	d := analysis.Diagnostic{
		Pos:      firstEdge.LockPos,
		Category: "lock-order-cycle",
		Message:  "potential lock ordering cycle: " + joinNames(names) + " -> " + names[0],
	}
	for i := range cycle {
		from := cycle[i]
		to := cycle[(i+1)%len(cycle)]
		ed := adj[from][to]
		rel := analysis.RelatedInformation{
			Pos:     ed.LockPos,
			Message: "acquisition of " + ed.To.String() + " while holding " + ed.From.String(),
		}
		d.Related = append(d.Related, rel)
		for _, cp := range ed.Chain {
			d.Related = append(d.Related, analysis.RelatedInformation{
				Pos:     cp,
				Message: "through call at " + posString(e.fset, cp),
			})
		}
	}
	return d
}

func joinNames(names []string) string {
	out := ""
	for i, n := range names {
		if i > 0 {
			out += " -> "
		}
		out += n
	}
	return out
}

// incompleteDiagnostics turns incompleteness findings into diagnostics with a
// distinct category. They describe limits of the analysis, never findings of
// safety.
func (e *engine) incompleteDiagnostics() []analysis.Diagnostic {
	var diags []analysis.Diagnostic
	seen := make(map[string]bool)
	for _, inc := range e.incomplete {
		key := inc.Reason + "@" + posString(e.fset, inc.Pos)
		if seen[key] {
			continue
		}
		seen[key] = true
		diags = append(diags, analysis.Diagnostic{
			Pos:      inc.Pos,
			Category: "lock-order-incomplete",
			Message:  "lock order analysis of " + inc.Func + " is incomplete here: " + inc.Reason,
		})
	}
	return diags
}

// sortDiagnostics orders diagnostics stably by file position, then category,
// so output never depends on map iteration order.
func sortDiagnostics(diags []analysis.Diagnostic, fset *token.FileSet) {
	sort.SliceStable(diags, func(i, j int) bool {
		pi, pj := fset.Position(diags[i].Pos), fset.Position(diags[j].Pos)
		if pi.Filename != pj.Filename {
			return pi.Filename < pj.Filename
		}
		if pi.Line != pj.Line {
			return pi.Line < pj.Line
		}
		if pi.Column != pj.Column {
			return pi.Column < pj.Column
		}
		return diags[i].Category < diags[j].Category
	})
}

// sortedEdges returns all proved edges in stable position order.
func sortedEdges(m map[edgeKey]Edge) []Edge {
	out := make([]Edge, 0, len(m))
	for _, ed := range m {
		out = append(out, ed)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].LockPos != out[j].LockPos {
			return out[i].LockPos < out[j].LockPos
		}
		if out[i].From.key() != out[j].From.key() {
			return out[i].From.key() < out[j].From.key()
		}
		return out[i].To.key() < out[j].To.key()
	})
	return out
}
