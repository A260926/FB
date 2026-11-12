// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package lockorder

import (
	"go/ast"
	"go/token"
	"go/types"
	"sort"

	"golang.org/x/tools/go/analysis"
	"golang.org/x/tools/go/cfg"
	"golang.org/x/tools/internal/typesinternal"
)

// abstractLoc is the value of a local binding during abstract interpretation.
// A known location carries a lock identity; unknown means the object's
// identity cannot be determined (heap allocation, index, escape, ...).
type abstractLoc struct {
	id    lockID
	known bool
}

var unknownLoc = abstractLoc{}

// heldLock records a lock that may currently be held and where it was
// acquired. chain holds the call-site positions traversed from the current
// function to the acquisition, outermost first.
type heldLock struct {
	id      lockID
	lockPos token.Pos
	chain   []token.Pos
}

// deferredUnlock is a registered defer mu.Unlock(). The receiver identity is
// captured at registration time and never changes afterward.
type deferredUnlock struct {
	id lockID
}

// sumEdge is an ordering edge inside a function summary: lock From is held
// when lock To is acquired. lockPos is the position of the real Lock call that
// acquired To; chain is the sequence of call sites crossed to reach it.
type sumEdge struct {
	from    lockID
	to      lockID
	lockPos token.Pos
	chain   []token.Pos
}

// acqInfo records that a function acquired a lock, with the position of one
// representative direct acquisition.
type acqInfo struct {
	id  lockID
	pos token.Pos
}

// summary is the parameterized effect of one function.
//
// Edges and acquired locks are expressed in the function's own abstract lock
// vocabulary: package-level globals and parameter slots extended by definite
// field paths. A conforming helper releases every lock it acquires on every
// normal return path, never unlocks a lock it did not acquire, and leaves no
// newly held lock to its caller. A summary that violates this, or whose
// function contains constructs the analyzer does not model, is marked
// incomplete; the provable edges it contains are still retained.
type summary struct {
	edges      map[sumKey]sumEdge
	acquired   map[string]acqInfo // keyed by lockID.key()
	incomplete bool
	unbounded  bool // structurally unbounded recursive field growth
	badHelper  bool // violates the helper lock discipline
}

func newSummary() *summary {
	return &summary{
		edges:    make(map[sumKey]sumEdge),
		acquired: make(map[string]acqInfo),
	}
}

// sumKey identifies an edge within a function summary. The domain is finite:
// lock roots and Lock positions are syntactic objects of the package, and the
// recursive-growth cap bounds field-path length. A crossed call chain is
// deliberately excluded because it can grow without bound under recursive
// inlining; the first observed chain is retained as a representative.
type sumKey struct {
	from, to string
	pos      token.Pos
}

func (s *summary) addEdge(e sumEdge) {
	k := sumKey{from: e.from.key(), to: e.to.key(), pos: e.lockPos}
	if _, ok := s.edges[k]; !ok {
		s.edges[k] = e
	}
}

func (s *summary) addAcquired(id lockID, pos token.Pos) {
	k := id.key()
	if prev, ok := s.acquired[k]; !ok || pos < prev.pos {
		s.acquired[k] = acqInfo{id: id, pos: pos}
	}
}

// equals reports whether two summaries carry the same information, used as
// the fixed-point termination test.
func (s *summary) equals(o *summary) bool {
	if s.incomplete != o.incomplete || s.unbounded != o.unbounded || s.badHelper != o.badHelper {
		return false
	}
	if len(s.edges) != len(o.edges) || len(s.acquired) != len(o.acquired) {
		return false
	}
	for k, e := range s.edges {
		oe, ok := o.edges[k]
		if !ok || oe.lockPos != e.lockPos || !samePosSeq(oe.chain, e.chain) {
			return false
		}
	}
	for k, a := range s.acquired {
		oa, ok := o.acquired[k]
		if !ok || oa.pos != a.pos || !oa.id.equal(a.id) {
			return false
		}
	}
	return true
}

func samePosSeq(a, b []token.Pos) bool {
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

// funcInfo is the per-function analysis state.
type funcInfo struct {
	fn      *types.Func
	decl    *ast.FuncDecl
	sig     *types.Signature
	params  []*types.Var // receiver (if any) first, then input parameters
	scc     int          // strongly connected component id of the static call graph
	summary *summary
	incomp  map[incompKey]struct{}
}

// incompKey identifies one unmodeled construct by its reason and position, so
// distinct sites sharing a reason are all retained.
type incompKey struct {
	reason string
	pos    token.Pos
}

// engine holds all state for one analysis.Pass. A fresh engine is constructed
// per pass, so summaries can never leak between two analyzed packages.
type engine struct {
	pass *analysis.Pass
	info *types.Info
	fset *token.FileSet

	funcs         map[*types.Func]*funcInfo
	order         []*funcInfo // deterministic source order
	maxFieldDepth int         // longest field-selection chain in the package

	allEdges   map[edgeKey]Edge
	incomplete []Incomplete
}

// Edge is one proved ordering relation in package-level vocabulary.
type Edge struct {
	From    lockID
	To      lockID
	LockPos token.Pos
	Chain   []token.Pos
}

type edgeKey struct {
	from, to string
	pos      token.Pos
}

// Incomplete records a construct that prevents complete analysis of a
// function.
type Incomplete struct {
	Pos    token.Pos
	Func   string
	Reason string
}

// Result is the analyzer result for a Pass.
type Result struct {
	Edges       []Edge
	Incomplete  []Incomplete
	Diagnostics []analysis.Diagnostic
}

func newEngine(pass *analysis.Pass) *engine {
	return &engine{
		pass:     pass,
		info:     pass.TypesInfo,
		fset:     pass.Fset,
		funcs:    make(map[*types.Func]*funcInfo),
		allEdges: make(map[edgeKey]Edge),
	}
}

// run performs the whole-package analysis and reports diagnostics.
func (e *engine) run() *Result {
	e.collectFuncs()
	e.computeSCCs()
	e.solve()
	e.collectIncomplete()
	e.collectEdges()
	diags := e.cycleDiagnostics()
	diags = append(diags, e.incompleteDiagnostics()...)
	sortDiagnostics(diags, e.fset)
	for _, d := range diags {
		e.pass.Report(d)
	}
	res := &Result{Diagnostics: diags}
	for _, ed := range sortedEdges(e.allEdges) {
		res.Edges = append(res.Edges, ed)
	}
	res.Incomplete = append(res.Incomplete, e.incomplete...)
	sort.SliceStable(res.Incomplete, func(i, j int) bool {
		if res.Incomplete[i].Pos != res.Incomplete[j].Pos {
			return res.Incomplete[i].Pos < res.Incomplete[j].Pos
		}
		return res.Incomplete[i].Reason < res.Incomplete[j].Reason
	})
	return res
}

// collectFuncs indexes every named function and method with a body. Generic
// functions are skipped: their instantiations cannot carry definite mutex
// identities without monomorphization.
func (e *engine) collectFuncs() {
	for _, file := range e.pass.Files {
		for _, decl := range file.Decls {
			fd, ok := decl.(*ast.FuncDecl)
			if !ok || fd.Body == nil {
				continue
			}
			obj, ok := e.info.Defs[fd.Name].(*types.Func)
			if !ok {
				continue
			}
			if obj.Signature().TypeParams() != nil || obj.Signature().RecvTypeParams() != nil {
				continue
			}
			fi := &funcInfo{
				fn:      obj,
				decl:    fd,
				sig:     obj.Signature(),
				summary: newSummary(),
				incomp:  make(map[incompKey]struct{}),
			}
			if recv := obj.Signature().Recv(); recv != nil {
				fi.params = append(fi.params, recv)
			}
			for i := range obj.Signature().Params().Len() {
				fi.params = append(fi.params, obj.Signature().Params().At(i))
			}
			e.funcs[obj] = fi
			e.order = append(e.order, fi)
			if d := longestFieldChain(fd.Body); d > e.maxFieldDepth {
				e.maxFieldDepth = d
			}
		}
	}
}

// longestFieldChain returns the maximum number of nested field selections in
// any expression of the body. It bounds the definite field paths that can
// exist without recursive structural growth.
func longestFieldChain(body ast.Node) int {
	max := 0
	ast.Inspect(body, func(n ast.Node) bool {
		sel, ok := n.(*ast.SelectorExpr)
		if !ok {
			return true
		}
		depth := 1
		for x := sel.X; ; {
			inner, ok := x.(*ast.SelectorExpr)
			if !ok {
				break
			}
			depth++
			x = inner.X
		}
		if depth > max {
			max = depth
		}
		return true
	})
	return max
}

// callGraph returns the static intra-package call edges, deduplicated per
// caller/callee pair.
func (e *engine) callGraph() map[*funcInfo][]*funcInfo {
	edges := make(map[*funcInfo][]*funcInfo)
	for _, fi := range e.order {
		seen := make(map[*types.Func]bool)
		ast.Inspect(fi.decl.Body, func(n ast.Node) bool {
			call, ok := n.(*ast.CallExpr)
			if !ok {
				return true
			}
			callee := typesinternal.StaticCallee(e.info, call)
			if callee == nil {
				return true
			}
			if cf, ok := e.funcs[callee]; ok && !seen[callee] {
				seen[callee] = true
				edges[fi] = append(edges[fi], cf)
			}
			return true
		})
	}
	return edges
}

// solve recomputes function summaries until the whole package reaches a fixed
// point. Every summary component grows monotonically over a finite lattice
// (see the package doc): edge and acquired-lock sets only gain elements drawn
// from a finite identity set, and the boolean flags only flip false→true, so
// iteration terminates without any depth cap. Recursive and mutually
// recursive groups stabilize together.
func (e *engine) solve() {
	callers := e.callerGraph()
	worklist := append([]*funcInfo(nil), e.order...)
	for len(worklist) > 0 {
		fi := worklist[0]
		worklist = worklist[1:]

		next := e.analyzeFunc(fi)
		if next.equals(fi.summary) {
			continue
		}
		fi.summary = next
		for _, caller := range callers[fi.fn] {
			worklist = append(worklist, caller)
		}
	}
}

// callerGraph maps each function to the functions that statically call it.
func (e *engine) callerGraph() map[*types.Func][]*funcInfo {
	callers := make(map[*types.Func][]*funcInfo)
	for _, fi := range e.order {
		seen := make(map[*types.Func]bool)
		ast.Inspect(fi.decl.Body, func(n ast.Node) bool {
			call, ok := n.(*ast.CallExpr)
			if !ok {
				return true
			}
			callee := typesinternal.StaticCallee(e.info, call)
			if callee == nil {
				return true
			}
			if _, ok := e.funcs[callee]; ok && !seen[callee] {
				seen[callee] = true
				callers[callee] = append(callers[callee], fi)
			}
			return true
		})
	}
	return callers
}

// frame is the abstract state at one CFG block.
type frame struct {
	env    map[*types.Var]abstractLoc
	held   []heldLock
	defers []deferredUnlock
}

func newFrame(fi *funcInfo) *frame {
	f := &frame{env: make(map[*types.Var]abstractLoc)}
	for i, v := range fi.params {
		// Only pointer-shaped parameters can denote a mutex object.
		if _, ok := v.Type().(*types.Pointer); ok {
			f.env[v] = abstractLoc{id: lockID{kind: rootParam, slot: i}, known: true}
		} else {
			f.env[v] = unknownLoc
		}
	}
	return f
}

func (f *frame) clone() *frame {
	c := &frame{
		env:    make(map[*types.Var]abstractLoc, len(f.env)),
		held:   append([]heldLock(nil), f.held...),
		defers: append([]deferredUnlock(nil), f.defers...),
	}
	for k, v := range f.env {
		c.env[k] = v
	}
	return c
}

// join merges an incoming frame into f. A binding survives only when all
// incoming frames agree on one identity; otherwise it becomes unknown, so
// mutually exclusive branches never invent a double-lock holding. Held locks
// and registered defers are united (may-hold approximation).
func (f *frame) join(o *frame) {
	for k, v := range o.env {
		if cur, ok := f.env[k]; !ok {
			f.env[k] = v
		} else if cur.known != v.known || (cur.known && !cur.id.equal(v.id)) {
			f.env[k] = unknownLoc
		}
	}
	for k := range f.env {
		if _, ok := o.env[k]; !ok {
			f.env[k] = unknownLoc
		}
	}
	f.held = unionHeld(f.held, o.held)
	f.defers = unionDefers(f.defers, o.defers)
}

func unionHeld(a, b []heldLock) []heldLock {
	out := append([]heldLock(nil), a...)
	for _, h := range b {
		if !heldContains(out, h.id) {
			out = append(out, h)
		}
	}
	return out
}

func heldContains(list []heldLock, id lockID) bool {
	for i := range list {
		if list[i].id.equal(id) {
			return true
		}
	}
	return false
}

func unionDefers(a, b []deferredUnlock) []deferredUnlock {
	out := append([]deferredUnlock(nil), a...)
	for _, d := range b {
		found := false
		for i := range out {
			if out[i].id.equal(d.id) {
				found = true
				break
			}
		}
		if !found {
			out = append(out, d)
		}
	}
	return out
}

// machine interprets one function body over its CFG.
type machine struct {
	e       *engine
	fi      *funcInfo
	g       *cfg.CFG
	summary *summary
	states  []*frame
	incomp  map[incompKey]struct{} // reason+position pairs, deduplicated
	bad     bool
}

// analyzeFunc runs the intraprocedural fixed point and builds the summary.
func (e *engine) analyzeFunc(fi *funcInfo) *summary {
	g := cfg.New(fi.decl.Body, func(*ast.CallExpr) bool { return true })
	m := &machine{
		e:       e,
		fi:      fi,
		g:       g,
		summary: newSummary(),
		states:  make([]*frame, len(g.Blocks)),
		incomp:  make(map[incompKey]struct{}),
	}

	entry := newFrame(fi)
	worklist := []int32{g.Blocks[0].Index}
	m.states[0] = entry
	for len(worklist) > 0 {
		idx := worklist[0]
		worklist = worklist[1:]
		blk := g.Blocks[idx]
		if !blk.Live {
			continue
		}
		state := m.states[idx].clone()
		cond := conditionalExpr(blk)
		for i, node := range blk.Nodes {
			m.exec(blk, node, state, node == cond && i == len(blk.Nodes)-1)
		}
		for _, succ := range blk.Succs {
			j := succ.Index
			if m.states[j] == nil {
				m.states[j] = state.clone()
				worklist = append(worklist, j)
			} else {
				before := m.states[j].snapshot()
				m.states[j].join(state)
				if m.states[j].snapshot() != before {
					worklist = append(worklist, j)
				}
			}
		}
	}

	m.summary.incomplete = len(m.incomp) > 0
	m.summary.badHelper = m.bad
	fi.incomp = m.incomp
	return m.summary
}

// conditionalExpr returns the branch condition of a two-successor block, or
// nil. The condition is always the block's final node.
func conditionalExpr(b *cfg.Block) ast.Expr {
	if len(b.Succs) != 2 || len(b.Nodes) == 0 {
		return nil
	}
	if e, ok := b.Nodes[len(b.Nodes)-1].(ast.Expr); ok {
		return e
	}
	return nil
}

// snapshot serializes the parts of a frame relevant to the block fixed-point
// test.
func (f *frame) snapshot() string {
	var b []byte
	keys := make([]*types.Var, 0, len(f.env))
	for k := range f.env {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool { return keys[i].Pos() < keys[j].Pos() })
	b = append(b, 'e')
	for _, k := range keys {
		v := f.env[k]
		b = append(b, k.Name()...)
		b = append(b, '=')
		if v.known {
			b = append(b, v.id.key()...)
		} else {
			b = append(b, '?')
		}
		b = append(b, ';')
	}
	b = append(b, 'h')
	for _, h := range f.held {
		b = append(b, h.id.key()...)
		b = append(b, ';')
	}
	b = append(b, 'd')
	for _, d := range f.defers {
		b = append(b, d.id.key()...)
		b = append(b, ';')
	}
	return string(b)
}

// markIncomplete records an unmodeled construct. Provable edges gathered so
// far are retained; the function is never treated as risk-free.
func (m *machine) markIncomplete(pos token.Pos, reason string) {
	m.incomp[incompKey{reason: reason, pos: pos}] = struct{}{}
}

// collectIncomplete gathers per-function incompleteness findings after the
// fixed point has converged.
func (e *engine) collectIncomplete() {
	for _, fi := range e.order {
		for k := range fi.incomp {
			e.incomplete = append(e.incomplete, Incomplete{
				Pos:    k.pos,
				Func:   fi.fn.Name(),
				Reason: k.reason,
			})
		}
		if fi.summary.badHelper {
			e.incomplete = append(e.incomplete, Incomplete{
				Pos:    fi.decl.Pos(),
				Func:   fi.fn.Name(),
				Reason: "helper does not release all locks it acquires on every return path or unlocks a lock it does not hold",
			})
		}
		if fi.summary.unbounded {
			e.incomplete = append(e.incomplete, Incomplete{
				Pos:    fi.decl.Pos(),
				Func:   fi.fn.Name(),
				Reason: "recursive argument substitution grows lock field paths without bound",
			})
		}
	}
}
