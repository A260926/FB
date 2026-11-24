/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
  Internal glue for the QSBR runtime adapter.

  This header is included by both `src/qsbr/allocator.c` (which embeds the whole
  allocator via `src/static.c`) and `src/qsbr/runtime.c` (which implements the
  domains and readers).  It fixes the *stable* allocator metadata layout and
  declares the handful of lock-free accessors the allocator hooks may use while
  it holds allocator locks.

  The hook contract forbids scanning the reader registry, taking the registry
  lock, allocating, waiting for readers, or recursively collecting from inside
  a hook.  `_mi_qsbr_domain_next_goal` and `_mi_qsbr_domain_completed` are
  therefore pure atomic operations on the domain; nothing else is exposed.
-----------------------------------------------------------------------------*/

#pragma once
#ifndef MI_QSBR_INTERNAL_RUNTIME_H
#define MI_QSBR_INTERNAL_RUNTIME_H

#include "mimalloc/qsbr-runtime.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Stable per-page QSBR metadata.  Plain, zero-initializable data: a zeroed value
// means "no goal recorded" (and is reported clear by `mi_page_qsbr_is_clear`).
struct mi_page_qsbr_s {
  uint64_t goal;                        // 0 = no goal; otherwise the target sequence
};

// Stable per-heap QSBR metadata.  The domain pointer is bound once at heap
// initialization and never changes, so free-side hooks on abandoned pages can
// use it without touching a possibly dangling theap.  NULL means the heap was
// created as a raw `mi_heap_new_qsbr()` without an adapter domain.
struct mi_heap_qsbr_s {
  mi_qsbr_domain_t* domain;
};

// Establish (or return) the next grace-period goal.  Lock-free; may be called
// while allocator locks are held.  Returns 0 only if `domain` is NULL.
uint64_t _mi_qsbr_domain_next_goal(mi_qsbr_domain_t* domain);

// The cached, published safe frontier.  A goal `g` may be released once
// `completed >= g`.  Lock-free.
uint64_t _mi_qsbr_domain_completed(mi_qsbr_domain_t* domain);

// Thread-local hand-off used only around `mi_heap_new_qsbr()` so that the
// `mi_heap_qsbr_init` hook can bind the heap to the requested domain before the
// heap is published.  Not used after initialization, so the binding does not
// depend on the creating thread after creation.
mi_qsbr_domain_t* _mi_qsbr_creation_domain_get(void);
void              _mi_qsbr_creation_domain_set(mi_qsbr_domain_t* domain);

// Return the domain a heap is bound to, or NULL for an unprotected heap (or a
// QSBR heap created without an adapter domain).  Implemented by `allocator.c`
// so that `runtime.c` does not depend on the allocator's internal layout.
mi_qsbr_domain_t* _mi_qsbr_heap_domain(mi_heap_t* heap);

// Release the allocator state of a heap (a thin wrapper over `mi_heap_destroy`)
// so that `runtime.c` needs no other allocator internals.
void _mi_qsbr_heap_destroy_allocator(mi_heap_t* heap);

// ----------------------------------------------------------------------------
// Sequence bounds and introspection.
//
// Goals are numbered 1..MI_QSBR_GOAL_MAX.  `MI_QSBR_GOAL_NEVER` is a sentinel
// used only when the 64-bit sequence is exhausted: it is never <= a completed
// frontier, so a page carrying it is retained conservatively forever (until a
// terminal heap destruction clears it).
// ----------------------------------------------------------------------------
#define MI_QSBR_GOAL_MAX     (UINT64_MAX - 1)
#define MI_QSBR_GOAL_NEVER   (UINT64_MAX)

// Current goal counter (number of goals established, capped at MI_QSBR_GOAL_MAX).
uint64_t _mi_qsbr_domain_target(mi_qsbr_domain_t* domain);
// Whether the domain ran out of sequence numbers; progress is frozen but the
// domain can still leave read sections and be destroyed.
bool     _mi_qsbr_domain_is_exhausted(mi_qsbr_domain_t* domain);
// Number of concurrently registrable readers (the `capacity` given at creation).
size_t   _mi_qsbr_domain_capacity(mi_qsbr_domain_t* domain);

// Test-only: drive the goal counter to its last value so the next established
// goal exercises the exhaustion path.  Only meaningful when no goal is pending.
void     _mi_qsbr_domain_saturate(mi_qsbr_domain_t* domain);

#ifdef __cplusplus
}
#endif

#endif  // MI_QSBR_INTERNAL_RUNTIME_H
