// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package lockorder

import (
	"go/ast"
	"go/token"
	"go/types"
	"strconv"
	"strings"
)

// lockRootKind classifies the root of a lock identity.
type lockRootKind uint8

const (
	rootGlobal lockRootKind = iota // package-level variable
	rootParam                      // pointer-shaped parameter or method receiver
	rootLocal                      // addressable local variable
)

// lockID is an abstract lock identity, independent of any particular call.
//
// A global lock is a package-level variable (possibly reached through a chain
// of struct fields). A parameter lock is slot i of the analyzed function's
// parameter tuple (a method receiver occupies slot 0), again possibly extended
// by struct fields. A local lock is an addressable local variable, keyed by its
// go/types binding; distinct same-named variables in different blocks get
// distinct objects and thus distinct identities. Field paths are definite:
// every step is a named struct field, never a map key, slice index, or pointer
// dereference of unknown provenance.
//
// lockID is immutable, and key returns a unique string encoding, so identities
// are usable as map keys.
type lockID struct {
	kind   lockRootKind
	slot   int        // parameter slot; unused for rootGlobal/rootLocal
	root   *types.Var // backing variable for rootGlobal/rootLocal
	fields []string   // definite struct field path, innermost field last
}

// key returns a stable textual encoding unique per identity.
func (l lockID) key() string {
	var b strings.Builder
	switch l.kind {
	case rootGlobal:
		b.WriteString("g:")
		b.WriteString(l.root.Pkg().Path())
		b.WriteByte('.')
		b.WriteString(l.root.Name())
	case rootParam:
		b.WriteString("p:")
		b.WriteString(strconv.Itoa(l.slot))
	case rootLocal:
		b.WriteString("l:")
		b.WriteString(strconv.Itoa(int(l.root.Pos())))
	}
	for _, f := range l.fields {
		b.WriteByte('.')
		b.WriteString(f)
	}
	return b.String()
}

func (l lockID) equal(o lockID) bool {
	if l.kind != o.kind || l.slot != o.slot || l.root != o.root || len(l.fields) != len(o.fields) {
		return false
	}
	for i := range l.fields {
		if l.fields[i] != o.fields[i] {
			return false
		}
	}
	return true
}

// String renders the identity as it appears in diagnostics.
func (l lockID) String() string {
	var name string
	switch l.kind {
	case rootGlobal, rootLocal:
		name = l.root.Name()
	case rootParam:
		name = paramName(l.slot)
	}
	for _, f := range l.fields {
		name += "." + f
	}
	return name
}

// extend returns the identity reached by selecting field f.
func (l lockID) extend(f string) lockID {
	fields := make([]string, 0, len(l.fields)+1)
	fields = append(fields, l.fields...)
	fields = append(fields, f)
	return lockID{kind: l.kind, slot: l.slot, root: l.root, fields: fields}
}

// paramName names a parameter slot in a function-independent way.
func paramName(slot int) string {
	if slot < len(paramNames) {
		return paramNames[slot]
	}
	return "arg" + strconv.Itoa(slot)
}

// paramNames covers the common small-parameter cases with fixed names.
var paramNames = []string{"arg0", "arg1", "arg2", "arg3", "arg4", "arg5", "arg6", "arg7"}

// isMutexType reports whether t is (possibly an alias for) the defined
// sync.Mutex type. RWMutex deliberately does not match.
func isMutexType(t types.Type) bool {
	return namedIs(t, "sync", "Mutex")
}

func namedIs(t types.Type, pkgPath, name string) bool {
	named, ok := types.Unalias(t).(*types.Named)
	if !ok {
		return false
	}
	obj := named.Obj()
	return obj != nil && obj.Pkg() != nil && obj.Pkg().Path() == pkgPath && obj.Name() == name
}

func deref(t types.Type) types.Type {
	if p, ok := t.(*types.Pointer); ok {
		return p.Elem()
	}
	return t
}

// mutexCallKind classifies a static selector call whose declared method
// belongs to package sync.
type mutexCallKind uint8

const (
	mutexNone       mutexCallKind = iota
	mutexLockUnlock               // sync.Mutex.Lock or sync.Mutex.Unlock
	mutexRWMutex                  // any sync.RWMutex method
	mutexTryLock                  // sync.Mutex.TryLock
)

// classifySyncCall reports how a static selector call relates to the sync
// mutex types. User-defined methods named Lock/Unlock never match because the
// method's declaring package must be sync.
func classifySyncCall(info *types.Info, call *ast.CallExpr) (kind mutexCallKind, method string, recv ast.Expr) {
	sel, ok := call.Fun.(*ast.SelectorExpr)
	if !ok {
		return mutexNone, "", nil
	}
	fn, isFn := info.Uses[sel.Sel].(*types.Func)
	if !isFn || fn.Pkg() == nil || fn.Pkg().Path() != "sync" {
		return mutexNone, "", nil
	}
	sig := fn.Signature()
	if sig.Recv() == nil {
		return mutexNone, "", nil
	}
	base := deref(sig.Recv().Type())
	switch {
	case namedIs(base, "sync", "RWMutex"):
		return mutexRWMutex, sel.Sel.Name, sel.X
	case namedIs(base, "sync", "Mutex") && (sel.Sel.Name == "Lock" || sel.Sel.Name == "Unlock"):
		return mutexLockUnlock, sel.Sel.Name, sel.X
	case namedIs(base, "sync", "Mutex"):
		return mutexTryLock, sel.Sel.Name, sel.X
	}
	return mutexNone, "", nil
}

// posString renders a token.Pos as file:line:column for diagnostics.
func posString(fset *token.FileSet, pos token.Pos) string {
	return fset.Position(pos).String()
}
