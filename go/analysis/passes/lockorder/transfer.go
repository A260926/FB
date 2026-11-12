// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package lockorder

import (
	"go/ast"
	"go/token"
	"go/types"
	"sort"

	"golang.org/x/tools/go/cfg"
	"golang.org/x/tools/internal/typesinternal"
)

// exec applies the abstract effect of one CFG node to state. isCond marks
// the branch condition of a two-successor block: lock-helper calls appearing
// inside it are reported as incomplete rather than executed as straight-line
// code.
func (m *machine) exec(blk *cfg.Block, node ast.Node, state *frame, isCond bool) {
	if isCond && containsLockRelevantCall(m.e.info, node) {
		m.markIncomplete(node.Pos(), "lock-related call inside a branch condition is not interpreted")
		return
	}
	switch n := node.(type) {
	case *ast.AssignStmt:
		m.execAssign(n, state)
	case *ast.ValueSpec:
		m.execValueSpec(n, state)
	case *ast.ExprStmt:
		m.evalCalls(n.X, state)
	case *ast.CallExpr:
		m.execCall(n, state)
	case *ast.DeferStmt:
		m.execDefer(n, state)
	case *ast.GoStmt:
		m.markIncomplete(n.Pos(), "goroutine launch is not analyzed")
	case *ast.FuncLit:
		m.markIncomplete(n.Pos(), "function literal (closure) is not analyzed")
	case *ast.ReturnStmt:
		m.evalCalls(n, state)
		m.checkReturnState(state)
	case *ast.Ident:
		// Range keys/values and type-switch bindings enter the CFG as bare
		// identifier nodes; their per-iteration identity is unknown.
		if obj, ok := m.e.info.Defs[n].(*types.Var); ok {
			if _, seen := state.env[obj]; !seen {
				state.env[obj] = unknownLoc
				if _, ptr := obj.Type().(*types.Pointer); ptr {
					m.markIncomplete(n.Pos(), "iteration or type-switch pointer has no definite lock identity")
				}
			}
		}
	default:
		// Any other node (send, incdec, switch tag, range header, ...) may
		// still contain calls whose lock effects must be accounted for.
		m.evalCalls(n, state)
	}
}

// containsLockRelevantCall reports whether n contains a call whose lock
// effects the analysis would otherwise model: a static call to a local
// function or a sync.Mutex Lock/Unlock.
func containsLockRelevantCall(info *types.Info, n ast.Node) bool {
	found := false
	ast.Inspect(n, func(x ast.Node) bool {
		if found {
			return false
		}
		call, ok := x.(*ast.CallExpr)
		if !ok {
			return true
		}
		if kind, _, _ := classifySyncCall(info, call); kind == mutexLockUnlock {
			found = true
			return false
		}
		if typesinternal.ClassifyCall(info, call) == typesinternal.CallStatic {
			fn, _ := info.Uses[typesinternal.UsedIdent(info, call.Fun)].(*types.Func)
			if fn != nil {
				found = true
			}
		}
		return true
	})
	return found
}

// evalCalls executes every call nested within n in Go evaluation order:
// argument calls before the call that consumes them, and siblings
// left-to-right. Calls are collected in preorder with their depth and then
// run deepest-first with source order preserved, i.e. a postorder. Function
// literals make the enclosing function incomplete; defer and go statements
// are separate execution contexts and are skipped here.
func (m *machine) evalCalls(n ast.Node, state *frame) {
	if n == nil {
		return
	}
	type placed struct {
		call  *ast.CallExpr
		depth int
		order int
	}
	var calls []placed
	depth := 0
	order := 0
	ast.Inspect(n, func(x ast.Node) bool {
		if x == nil {
			depth--
			return true
		}
		switch x.(type) {
		case *ast.FuncLit:
			m.markIncomplete(x.Pos(), "function literal (closure) is not analyzed")
			return false
		case *ast.DeferStmt, *ast.GoStmt:
			return false
		}
		if call, ok := x.(*ast.CallExpr); ok {
			calls = append(calls, placed{call, depth, order})
			order++
		}
		depth++
		return true
	})
	sort.SliceStable(calls, func(i, j int) bool {
		if calls[i].depth != calls[j].depth {
			return calls[i].depth > calls[j].depth
		}
		return calls[i].order < calls[j].order
	})
	for _, p := range calls {
		m.execCall(p.call, state)
	}
}

func (m *machine) execAssign(a *ast.AssignStmt, state *frame) {
	// Account for every call on the right-hand sides first.
	for _, rhs := range a.Rhs {
		m.evalCalls(rhs, state)
	}

	// Multi-value assignment from one call: returned pointers cannot carry
	// definite identities.
	if len(a.Rhs) == 1 && len(a.Lhs) > 1 {
		for _, lhs := range a.Lhs {
			m.bindUnknown(lhs, state, "multi-value call result has no definite lock identity")
		}
		return
	}

	if len(a.Lhs) == len(a.Rhs) {
		for i := range a.Lhs {
			m.assignOne(a.Lhs[i], a.Rhs[i], a.Tok == token.DEFINE, state)
		}
	}
}

func (m *machine) execValueSpec(spec *ast.ValueSpec, state *frame) {
	for _, value := range spec.Values {
		m.evalCalls(value, state)
	}
	for i, name := range spec.Names {
		obj, _ := m.e.info.Defs[name].(*types.Var)
		if obj == nil {
			continue
		}
		if i < len(spec.Values) {
			m.assignIdent(obj, spec.Values[i], state)
			continue
		}
		// A zero-valued local is still a definite, addressable object when
		// its type contains a sync.Mutex value; an uninitialized pointer has
		// no identity.
		if loc, ok := m.localMutexIdentity(obj); ok {
			state.env[obj] = loc
		} else {
			state.env[obj] = unknownLoc
		}
	}
}

// localMutexIdentity gives an addressable local variable its own root when
// its static type is a struct embedding or containing a sync.Mutex value.
func (m *machine) localMutexIdentity(obj *types.Var) (abstractLoc, bool) {
	t := obj.Type()
	if _, ptr := t.(*types.Pointer); ptr {
		return unknownLoc, false
	}
	if !typeContainsMutex(t) {
		return unknownLoc, false
	}
	return abstractLoc{id: lockID{kind: rootLocal, root: obj}, known: true}, true
}

// typeContainsMutex reports whether t is a struct type that has a sync.Mutex
// value somewhere in its definite field tree.
func typeContainsMutex(t types.Type) bool {
	st, ok := t.Underlying().(*types.Struct)
	if !ok {
		return false
	}
	for i := range st.NumFields() {
		ft := st.Field(i).Type()
		if isMutexType(ft) {
			return true
		}
		if typeContainsMutex(ft) {
			return true
		}
	}
	return false
}

// assignOne handles one lhs/rhs pair of an assignment.
func (m *machine) assignOne(lhs, rhs ast.Expr, define bool, state *frame) {
	switch l := ast.Unparen(lhs).(type) {
	case *ast.Ident:
		if l.Name == "_" {
			return
		}
		var obj *types.Var
		if define {
			obj, _ = m.e.info.Defs[l].(*types.Var)
		} else {
			obj, _ = m.e.info.Uses[l].(*types.Var)
		}
		if obj == nil {
			return
		}
		m.assignIdent(obj, rhs, state)
	case *ast.SelectorExpr, *ast.StarExpr, *ast.IndexExpr:
		// Writes through a field, pointer dereference, or index may retarget
		// a lock object; the result cannot be tracked definitively.
		m.markIncomplete(lhs.Pos(), "write through a field, pointer, or index may retarget a lock")
	default:
		m.markIncomplete(lhs.Pos(), "unsupported assignment target")
	}
}

// assignIdent updates the binding of obj from the right-hand side.
func (m *machine) assignIdent(obj *types.Var, rhs ast.Expr, state *frame) {
	rhs = ast.Unparen(rhs)
	switch r := rhs.(type) {
	case *ast.UnaryExpr:
		if r.Op == token.AND {
			// p := &x: p denotes the same object as the addressable x.
			if loc, ok := m.resolve(r.X, state); ok {
				state.env[obj] = loc
				return
			}
			state.env[obj] = unknownLoc
			m.markIncomplete(rhs.Pos(), "address taken of an expression with no definite identity")
			return
		}
		state.env[obj] = unknownLoc
	case *ast.CompositeLit:
		// A local variable initialized with a composite literal is itself a
		// definite addressable object if its type embeds a mutex; anything
		// else (notably pointer literals) is an untracked allocation.
		if loc, ok := m.localMutexIdentity(obj); ok {
			state.env[obj] = loc
			return
		}
		state.env[obj] = unknownLoc
		m.markIncomplete(rhs.Pos(), "composite literal allocation has no definite lock identity")
	case *ast.FuncLit:
		state.env[obj] = unknownLoc
		m.markIncomplete(rhs.Pos(), "function literal (closure) is not analyzed")
	case *ast.CallExpr:
		state.env[obj] = unknownLoc
		if _, ok := obj.Type().(*types.Pointer); ok {
			m.markIncomplete(rhs.Pos(), "pointer returned by a call has no definite lock identity")
		}
	case *ast.IndexExpr, *ast.IndexListExpr:
		state.env[obj] = unknownLoc
		m.markIncomplete(rhs.Pos(), "indexed expression has no definite lock identity")
	default:
		if loc, ok := m.resolve(rhs, state); ok {
			state.env[obj] = loc
			return
		}
		state.env[obj] = unknownLoc
	}
}

// bindUnknown marks an assignment target as carrying no definite identity.
func (m *machine) bindUnknown(lhs ast.Expr, state *frame, reason string) {
	id, ok := ast.Unparen(lhs).(*ast.Ident)
	if !ok || id.Name == "_" {
		return
	}
	obj, _ := m.e.info.Defs[id].(*types.Var)
	if obj == nil {
		obj, _ = m.e.info.Uses[id].(*types.Var)
	}
	if obj == nil {
		return
	}
	state.env[obj] = unknownLoc
	if _, ok := obj.Type().(*types.Pointer); ok {
		m.markIncomplete(lhs.Pos(), reason)
	}
}

// resolve returns the abstract lock location denoted by an expression used as
// a mutex receiver, pointer value, or address operand. The second result is
// false when no definite identity can be given.
func (m *machine) resolve(e ast.Expr, state *frame) (abstractLoc, bool) {
	switch e := ast.Unparen(e).(type) {
	case *ast.Ident:
		obj, _ := m.e.info.Uses[e].(*types.Var)
		if obj == nil {
			obj, _ = m.e.info.Defs[e].(*types.Var)
		}
		if obj == nil {
			return unknownLoc, false
		}
		if loc, ok := state.env[obj]; ok {
			return loc, loc.known
		}
		if isPackageLevel(obj) {
			return abstractLoc{id: lockID{kind: rootGlobal, root: obj}, known: true}, true
		}
		return unknownLoc, false

	case *ast.SelectorExpr:
		base, ok := m.resolve(e.X, state)
		if !ok {
			return unknownLoc, false
		}
		return m.extendField(base, e)

	case *ast.UnaryExpr:
		if e.Op == token.AND {
			// &x denotes the same object as the addressable x.
			return m.resolve(e.X, state)
		}
		return unknownLoc, false

	case *ast.StarExpr:
		// *p where p is a tracked pointer denotes the same object.
		return m.resolve(e.X, state)

	case *ast.ParenExpr:
		return m.resolve(e.X, state)
	}
	return unknownLoc, false
}

// extendField extends a known location by one struct field selection,
// validating the selection against go/types. A pointer field deeper than the
// first hop cannot be followed because its pointee has no definite identity.
func (m *machine) extendField(base abstractLoc, sel *ast.SelectorExpr) (abstractLoc, bool) {
	if !base.known {
		return unknownLoc, false
	}
	t := m.e.info.Types[sel.X].Type
	if t == nil {
		return unknownLoc, false
	}
	if p, ptr := t.(*types.Pointer); ptr {
		if len(base.id.fields) != 0 {
			return unknownLoc, false
		}
		t = p.Elem()
	}
	st, ok := t.Underlying().(*types.Struct)
	if !ok {
		return unknownLoc, false
	}
	if fieldIndex(st, sel.Sel.Name) < 0 {
		return unknownLoc, false
	}
	return abstractLoc{id: base.id.extend(sel.Sel.Name), known: true}, true
}

func fieldIndex(st *types.Struct, name string) int {
	for i := range st.NumFields() {
		if st.Field(i).Name() == name {
			return i
		}
	}
	return -1
}

func isPackageLevel(v *types.Var) bool {
	return v.Parent() != nil && v.Parent() == v.Pkg().Scope()
}

// execCall dispatches one function or method call.
func (m *machine) execCall(call *ast.CallExpr, state *frame) {
	switch typesinternal.ClassifyCall(m.e.info, call) {
	case typesinternal.CallConversion, typesinternal.CallBuiltin:
		if id := typesinternal.UsedIdent(m.e.info, call.Fun); id != nil {
			switch id.Name {
			case "panic", "recover":
				m.markIncomplete(call.Pos(), "panic/recover is not modeled")
			}
		}
		return

	case typesinternal.CallInterface:
		m.markIncomplete(call.Pos(), "interface method dynamic dispatch is not modeled")
		return

	case typesinternal.CallDynamic:
		m.markIncomplete(call.Pos(), "dynamic function call is not modeled")
		return

	case typesinternal.CallStatic:
		switch kind, method, recv := classifySyncCall(m.e.info, call); kind {
		case mutexLockUnlock:
			switch method {
			case "Lock":
				m.doLock(recv, call.Pos(), state)
			case "Unlock":
				m.doUnlock(recv, call.Pos(), state)
			}
			return
		case mutexRWMutex:
			m.markIncomplete(call.Pos(), "sync.RWMutex operations are not modeled")
			return
		case mutexTryLock:
			m.markIncomplete(call.Pos(), "sync.Mutex."+method+" (TryLock family) is not modeled")
			return
		}
		fn := typesinternal.StaticCallee(m.e.info, call)
		if fn == nil {
			m.markIncomplete(call.Pos(), "unresolved static call is not modeled")
			return
		}
		if fn.Pkg() != m.e.pass.Pkg {
			m.markIncomplete(call.Pos(), "cross-package function body is not expanded")
			return
		}
		callee, ok := m.e.funcs[fn]
		if !ok {
			m.markIncomplete(call.Pos(), "function without an analyzable body is not expanded")
			return
		}
		m.inlineCall(call, callee, state)
	}
}

// doLock acquires a lock: every lock that may already be held yields an
// ordering edge to it. Re-acquiring a held lock yields a self edge.
func (m *machine) doLock(recv ast.Expr, pos token.Pos, state *frame) {
	loc, ok := m.resolve(recv, state)
	if !ok {
		m.markIncomplete(pos, "lock receiver has no definite identity")
		return
	}
	for _, h := range state.held {
		m.summary.addEdge(sumEdge{
			from:    h.id,
			to:      loc.id,
			lockPos: pos,
			chain:   append([]token.Pos(nil), h.chain...),
		})
	}
	m.summary.addAcquired(loc.id, pos)
	if !heldContains(state.held, loc.id) {
		state.held = append(state.held, heldLock{id: loc.id, lockPos: pos})
	}
}

// doUnlock releases exactly the one lock whose identity is definite. A
// supported helper may only unlock a lock it acquired itself; unlocking an
// inherited caller lock (or one that is not held) violates the helper
// discipline and marks the function incomplete.
func (m *machine) doUnlock(recv ast.Expr, pos token.Pos, state *frame) {
	loc, ok := m.resolve(recv, state)
	if !ok {
		m.markIncomplete(pos, "unlock receiver has no definite identity")
		return
	}
	for i, h := range state.held {
		if h.id.equal(loc.id) {
			if _, selfAcquired := m.summary.acquired[loc.id.key()]; !selfAcquired {
				m.bad = true
				m.markIncomplete(pos, "helper unlocks a lock it did not acquire")
			}
			state.held = append(state.held[:i], state.held[i+1:]...)
			return
		}
	}
	// Unlocking a lock that is not held here violates the helper discipline.
	m.bad = true
	m.markIncomplete(pos, "unlock of a lock that is not held")
}

// execDefer handles defer statements. A deferred mutex Unlock captures its
// receiver at registration time and releases only when the function exits;
// repointing the local alias afterward cannot change the captured object.
// Anything else deferred is unmodeled.
func (m *machine) execDefer(d *ast.DeferStmt, state *frame) {
	call := d.Call
	kind, method, recv := classifySyncCall(m.e.info, call)
	if kind == mutexLockUnlock && method == "Unlock" {
		loc, ok := m.resolve(recv, state)
		if !ok {
			m.markIncomplete(call.Pos(), "deferred unlock receiver has no definite identity")
			return
		}
		if _, selfAcquired := m.summary.acquired[loc.id.key()]; !selfAcquired {
			m.bad = true
			m.markIncomplete(call.Pos(), "helper defers unlock of a lock it did not acquire")
		}
		state.defers = append(state.defers, deferredUnlock{id: loc.id})
		return
	}
	m.markIncomplete(call.Pos(), "non-mutex defer statement is not modeled")
}
