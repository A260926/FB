// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Package lockorder defines an Analyzer that reports potential cycles in
// the order in which sync.Mutex locks are acquired within a package.
//
// # Analyzer lockorder
//
// lockorder: report potential lock ordering cycles
//
// The lockorder analyzer builds a conservative, whole-package model of
// sync.Mutex acquisition order. For every lock acquisition it records an
// ordering edge from each lock that may still be held to the newly acquired
// lock, and it reports a diagnostic for every strongly connected component of
// the resulting graph that contains a cycle.
//
// A reported cycle is a risk hint, not a proof of deadlock: the analyzer does
// not reason about goroutines, the feasibility of reaching two sites
// concurrently, or the truth of branch conditions. Two holding states that
// occur only in mutually exclusive branches are never merged into a
// double-lock holding. The analyzed program is never executed; the analysis
// consumes only the parser and go/types outputs plus a syntactic CFG.
//
// # Lock identity
//
// Locks are recognized through the type information, not by method name: only
// calls whose declared method belongs to sync.Mutex (Lock and Unlock) count.
// A user-defined method named Lock is ignored. An identity is rooted at a
// package-level variable, a pointer-shaped parameter or method receiver, or an
// addressable local variable whose type contains a sync.Mutex value, and is
// extended by a definite chain of named struct fields.
//
// Local aliases are tracked with go/types bindings: p := &mu and q := p
// denote the same object, definite overwriting assignments retarget a binding,
// and same-named variables in different blocks are distinct objects. The
// analyzer never guesses identity through a map or slice index, a heap
// allocation, interface dynamic dispatch, a closure write, or pointer escape.
// Such a construct marks the enclosing function analysis-incomplete; edges
// already proved are still reported, and incompleteness is never read as
// "no risk".
//
// # Support boundaries
//
// The following are deliberately out of scope and, when encountered, mark the
// relevant function incomplete rather than being silently treated as
// straight-line code:
//
//   - sync.RWMutex and the TryLock family (only sync.Mutex Lock/Unlock);
//   - goroutine launches, function literals and closure effects,
//     panic/recover, and defer statements other than defer mu.Unlock();
//   - lock-relevant calls evaluated inside a branch condition;
//   - interface method calls and other dynamic function calls;
//   - calls into other packages, whose bodies are not expanded;
//   - writes through fields, pointers, or indices that could retarget a lock;
//   - generic functions, which cannot carry definite identities without
//     monomorphization.
//
// A deferred mutex Unlock captures its receiver at the point the defer is
// registered. Registration does not release the lock; release happens only on
// function exit, and repointing the local alias after registration cannot
// change which lock the deferred call unlocks.
//
// # Function summaries and the fixed point
//
// Each function gets a parameterized summary: its internal ordering edges and
// the set of locks it acquires, expressed in terms of globals and parameter
// slots. A supported helper releases every lock it acquires on every normal
// return path (directly or through a deferred Unlock), never unlocks a lock it
// does not hold, and leaves no newly held lock to its caller. A function that
// violates this is marked incomplete. At a call site the caller substitutes
// concrete actual-argument identities for the callee's parameter slots, so
// different calls never share arguments, and a lock the caller already holds
// is ordered against every lock the callee acquires.
//
// Summaries are recomputed in a worklist until nothing changes. Termination
// is a finite-lattice argument, not an arbitrary unroll bound. The edge and
// acquired-lock sets only grow, and every element is drawn from a finite
// domain: the package's syntactic lock roots, its definite field paths, and
// its real Lock positions. Boolean incompleteness flags only move false to
// true. Recursive and mutually recursive functions therefore converge to a
// genuine fixed point. When a recursive substitution would grow a field path
// structurally without bound (for example f(p.next)), the analyzer caps the
// path, marks that call chain incomplete, and keeps every relation already
// proved along it; an exact summary over an infinite identity set is not
// required. A call that produces A followed by A is retained as a self-loop
// risk.
//
// # Diagnostics
//
// Each cycle diagnostic gives one stable closed path through a strongly
// connected component of the concrete package-level graph. Its Related
// information lists the real Lock position of every edge and, where an edge
// was reached through a helper, the intra-package call sites crossed. Output
// is sorted deterministically by file position and never depends on map
// iteration order.
package lockorder
