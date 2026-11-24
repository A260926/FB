/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
  Reader domains and grace-period progress for the QSBR runtime adapter.

  Implementation notes
  --------------------
  Publishing order (why a reader that starts after a goal is established never
  blocks that goal, while a reader that started before it does):

    * Establishing a goal is a CAS that advances `domain->sequence` from `s` to
      `s+1`; the goal value is the *new* sequence `s+1`.  It is a release
      operation.  This is the single linearization point for a goal.
    * A reader entering its outermost section loads `sequence` with acquire and
      records that value as its `epoch`, then marks itself ACTIVE.  Because the
      load is acquire and the goal CAS is release, a reader either observes the
      incremented sequence (epoch >= goal, so it started after the goal and
      must not block it) or it observed an older sequence (epoch < goal, so it
      started before and does block it).
    * `sequence` never wraps: it is capped at `MI_QSBR_GOAL_MAX`.  When the cap
      is reached the domain is marked `exhausted`, further goals become
      `MI_QSBR_GOAL_NEVER` and `mi_qsbr_poll` stops advancing (conservative).
      Read sections can still leave and the domain and heaps can still be
      destroyed.

  Why `mi_qsbr_poll` is correct without sleeping or sampling:

    * Enter, leave, register, unregister and termination take `domain->lock`.
      Read sections do *not*: the lock is only held for O(1) bookkeeping.
      Therefore poll, which also takes the lock, sees a consistent snapshot of
      every reader's `state` and `epoch`, and it never waits for a reader to
      leave a section — only for other O(1) bookkeeping to finish.
    * Poll computes `candidate = min(sequence-at-scan, min epoch of ACTIVE
      readers)`.  Every goal `g <= candidate` is safe: any reader still ACTIVE
      has `epoch >= candidate >= g` and thus began after `g` was established.
    * `completed` is advanced with an atomic max (CAS), so a concurrent or
      later poll can never move it backwards.  A freshly entered reader has
      `epoch >= sequence >= completed`, so it cannot make the frontier regress.
    * A terminated/never-completed goal is represented by `MI_QSBR_GOAL_NEVER`
      which is greater than every value `completed` can take; such pages are
      retained until a terminal heap destruction clears their metadata.

  The allocator hooks (`mi_page_qsbr_*` in `src/qsbr/allocator.c`) only touch
  `sequence` (CAS) and `completed` (atomic load); they never take
  `domain->lock`, never scan readers, never allocate and never collect.
-----------------------------------------------------------------------------*/

#define MI_QSBR 1
#include "qsbr-internal.h"
#include "mimalloc/internal.h"     // mi_lock_t, atomics, full mi_heap_t layout

#include <stdlib.h>                // malloc/free for the domain storage
#include <stddef.h>

// ----------------------------------------------------------------------------
// Domain and reader layout
// ----------------------------------------------------------------------------

typedef enum mi_qsbr_slot_state_e {
  MI_QSBR_SLOT_FREE = 0,           // not registered (on the free list)
  MI_QSBR_SLOT_REGISTERED = 1,     // registered but outside any read section
  MI_QSBR_SLOT_ACTIVE = 2          // inside a read section (outermost enter)
} mi_qsbr_slot_state_t;

struct mi_qsbr_reader_s {
  mi_qsbr_domain_t*    domain;     // owning domain (guarded by `domain->lock`)
  mi_qsbr_reader_t*    next_free;  // free-list link (guarded by `domain->lock`)
  uint64_t             epoch;      // outermost-enter boundary (guarded by `domain->lock`)
  uint32_t             depth;      // nesting depth (guarded by `domain->lock`)
  mi_qsbr_slot_state_t state;      // slot state (guarded by `domain->lock`)
};

struct mi_qsbr_domain_s {
  size_t               capacity;      // number of reader slots
  size_t               registered;    // registered readers (guarded by `lock`)
  size_t               in_section;    // readers inside a read section (guarded by `lock`)
  size_t               bound_heaps;   // heaps bound to this domain (guarded by `lock`)
  bool                 terminating;   // a heap termination is in progress (guarded by `lock`)
  _Atomic(uint64_t)    exhausted;     // sequence cap reached; progress frozen
  _Atomic(uint64_t)    sequence;      // last established goal (monotonic, capped)
  _Atomic(uint64_t)    completed;     // cached safe frontier (monotonic)
  mi_qsbr_reader_t*    free_list;     // free reader slots (guarded by `lock`)
  mi_lock_t            lock;          // protects slots, registration, admission and binding
  mi_qsbr_reader_t     readers[];     // capacity slots, stable addresses
};

// ----------------------------------------------------------------------------
// Sequence primitives used by the allocator hooks (lock-free)
// ----------------------------------------------------------------------------

uint64_t _mi_qsbr_domain_next_goal(mi_qsbr_domain_t* domain) {
  if (domain == NULL) return 0;
  for (;;) {
    const uint64_t cur = mi_atomic_load_acquire(&domain->sequence);
    if (cur >= MI_QSBR_GOAL_MAX) {
      mi_atomic_store_release(&domain->exhausted, 1);
      return MI_QSBR_GOAL_NEVER;
    }
    uint64_t expected = cur;
    if (mi_atomic_cas_strong_acq_rel(&domain->sequence, &expected, cur + 1)) {
      return cur + 1;
    }
  }
}

uint64_t _mi_qsbr_domain_completed(mi_qsbr_domain_t* domain) {
  if (domain == NULL) return 0;
  return mi_atomic_load_acquire(&domain->completed);
}

uint64_t _mi_qsbr_domain_target(mi_qsbr_domain_t* domain) {
  if (domain == NULL) return 0;
  return mi_atomic_load_acquire(&domain->sequence);
}

bool _mi_qsbr_domain_is_exhausted(mi_qsbr_domain_t* domain) {
  if (domain == NULL) return false;
  return mi_atomic_load_acquire(&domain->exhausted) != 0;
}

size_t _mi_qsbr_domain_capacity(mi_qsbr_domain_t* domain) {
  return (domain == NULL ? 0 : domain->capacity);
}

// Test-only helper: jump the sequence to its last usable value so that the next
// established goal exercises the exhaustion path.  Safe only when no goal is
// pending (i.e. it is a test driver; it never moves the frontier backwards).
void _mi_qsbr_domain_saturate(mi_qsbr_domain_t* domain) {
  if (domain == NULL) return;
  mi_lock(&domain->lock) {
    const uint64_t cur = mi_atomic_load_acquire(&domain->sequence);
    if (cur < MI_QSBR_GOAL_MAX) {
      mi_atomic_store_release(&domain->sequence, MI_QSBR_GOAL_MAX);
    }
  }
}

// ----------------------------------------------------------------------------
// Creation domain hand-off (used only during `mi_heap_new_qsbr`)
// ----------------------------------------------------------------------------

static mi_decl_thread mi_qsbr_domain_t* _mi_qsbr_creation_domain = NULL;

mi_qsbr_domain_t* _mi_qsbr_creation_domain_get(void) {
  return _mi_qsbr_creation_domain;
}

void _mi_qsbr_creation_domain_set(mi_qsbr_domain_t* domain) {
  _mi_qsbr_creation_domain = domain;
}

// ----------------------------------------------------------------------------
// Domain lifecycle
// ----------------------------------------------------------------------------

mi_qsbr_domain_t* mi_qsbr_domain_new(size_t capacity) {
  if (capacity == 0) return NULL;
  // Bound the allocation so a bogus capacity cannot overflow `size_t`.
  if (capacity > (SIZE_MAX - sizeof(mi_qsbr_domain_t)) / sizeof(mi_qsbr_reader_t)) {
    return NULL;
  }
  mi_qsbr_domain_t* domain = (mi_qsbr_domain_t*)malloc(sizeof(mi_qsbr_domain_t) + capacity * sizeof(mi_qsbr_reader_t));
  if (domain == NULL) return NULL;

  domain->capacity = capacity;
  domain->registered = 0;
  domain->in_section = 0;
  domain->bound_heaps = 0;
  domain->terminating = false;
  mi_atomic_store_release(&domain->exhausted, 0);
  mi_atomic_store_release(&domain->sequence, 0);
  mi_atomic_store_release(&domain->completed, 0);
  mi_lock_init(&domain->lock);

  // Build the free list.  Slots are zero-initialized and explicitly reset so a
  // reused slot never carries an old epoch or nesting depth.
  domain->free_list = NULL;
  for (size_t i = capacity; i-- > 0; ) {
    mi_qsbr_reader_t* r = &domain->readers[i];
    r->domain = NULL;
    r->epoch = 0;
    r->depth = 0;
    r->state = MI_QSBR_SLOT_FREE;
    r->next_free = domain->free_list;
    domain->free_list = r;
  }
  return domain;
}

bool mi_qsbr_domain_delete(mi_qsbr_domain_t* domain) {
  if (domain == NULL) return false;
  bool ok = false;
  mi_lock(&domain->lock) {
    if (domain->registered == 0 && domain->bound_heaps == 0 && !domain->terminating) {
      ok = true;
    }
  }
  if (!ok) return false;
  mi_lock_done(&domain->lock);
  free(domain);
  return true;
}

// ----------------------------------------------------------------------------
// Reader lifecycle
// ----------------------------------------------------------------------------

mi_qsbr_reader_t* mi_qsbr_register(mi_qsbr_domain_t* domain) {
  if (domain == NULL) return NULL;
  mi_qsbr_reader_t* reader = NULL;
  mi_lock(&domain->lock) {
    // Registration alone grants no protection and does not block progress, so
    // it is allowed even while a heap termination is in progress; only `enter`
    // is gated by `terminating`.
    if (domain->free_list != NULL) {
      reader = domain->free_list;
      domain->free_list = reader->next_free;
      // Reset all per-section state: a reused slot inherits nothing.
      reader->next_free = NULL;
      reader->domain = domain;
      reader->epoch = 0;
      reader->depth = 0;
      reader->state = MI_QSBR_SLOT_REGISTERED;
      domain->registered++;
    }
  }
  return reader;
}

bool mi_qsbr_unregister(mi_qsbr_reader_t* reader) {
  if (reader == NULL) return false;
  mi_qsbr_domain_t* domain = reader->domain;
  if (domain == NULL) return false;
  bool ok = false;
  mi_lock(&domain->lock) {
    if (reader->state == MI_QSBR_SLOT_REGISTERED && reader->depth == 0) {
      reader->state = MI_QSBR_SLOT_FREE;
      reader->epoch = 0;
      reader->depth = 0;
      reader->domain = NULL;
      reader->next_free = domain->free_list;
      domain->free_list = reader;
      domain->registered--;
      ok = true;
    }
  }
  return ok;
}

bool mi_qsbr_enter(mi_qsbr_reader_t* reader) {
  if (reader == NULL) return false;
  mi_qsbr_domain_t* domain = reader->domain;
  if (domain == NULL) return false;
  bool ok = false;
  mi_lock(&domain->lock) {
    if (!domain->terminating && reader->state != MI_QSBR_SLOT_FREE) {
      if (reader->depth == 0) {
        // Outermost enter: publish the boundary.  Nested enters do not refresh
        // it, so an inner enter cannot shorten the grace period of an old page.
        reader->epoch = mi_atomic_load_acquire(&domain->sequence);
        reader->state = MI_QSBR_SLOT_ACTIVE;
        domain->in_section++;
      }
      reader->depth++;
      ok = true;
    }
  }
  return ok;
}

bool mi_qsbr_leave(mi_qsbr_reader_t* reader) {
  if (reader == NULL) return false;
  mi_qsbr_domain_t* domain = reader->domain;
  if (domain == NULL) return false;
  bool ok = false;
  mi_lock(&domain->lock) {
    if (reader->state == MI_QSBR_SLOT_ACTIVE && reader->depth > 0) {
      reader->depth--;
      if (reader->depth == 0) {
        // Only the outermost leave ends protection.
        reader->state = MI_QSBR_SLOT_REGISTERED;
        reader->epoch = 0;
        if (domain->in_section > 0) { domain->in_section--; }
      }
      ok = true;
    }
    // Outside any read section (or a free slot): reject without changing state.
  }
  return ok;
}

// ----------------------------------------------------------------------------
// Grace-period progress
// ----------------------------------------------------------------------------

uint64_t mi_qsbr_poll(mi_qsbr_domain_t* domain) {
  if (domain == NULL) return 0;
  uint64_t done;
  mi_lock(&domain->lock) {
    if (mi_atomic_load_acquire(&domain->exhausted) != 0) {
      // Sequence space exhausted: freeze progress conservatively.  Read
      // sections can still leave and destruction still works.
      done = mi_atomic_load_acquire(&domain->completed);
    }
    else {
      // Consistent snapshot: enter/leave hold the same lock, so no reader is
      // mid-enter and every ACTIVE reader has a valid epoch.
      const uint64_t seq_start = mi_atomic_load_acquire(&domain->sequence);
      uint64_t candidate = seq_start;
      for (size_t i = 0; i < domain->capacity; i++) {
        const mi_qsbr_reader_t* r = &domain->readers[i];
        if (r->state == MI_QSBR_SLOT_ACTIVE && r->epoch < candidate) {
          candidate = r->epoch;
        }
      }
      // Advance monotonically.  `completed` never decreases.
      done = mi_atomic_load_acquire(&domain->completed);
      while (candidate > done) {
        uint64_t expected = done;
        if (mi_atomic_cas_strong_acq_rel(&domain->completed, &expected, candidate)) {
          done = candidate;
          break;
        }
        done = expected;
      }
      done = mi_atomic_load_acquire(&domain->completed);
    }
  }
  return done;
}

// ----------------------------------------------------------------------------
// Heap binding
// ----------------------------------------------------------------------------

mi_heap_t* mi_qsbr_heap_new(mi_qsbr_domain_t* domain) {
  if (domain == NULL) return NULL;
  // Reserve the binding *before* creating the heap so a concurrent
  // `mi_qsbr_domain_delete` cannot free the domain mid-creation.
  bool admitted = false;
  mi_lock(&domain->lock) {
    if (!domain->terminating) {
      domain->bound_heaps++;
      admitted = true;
    }
  }
  if (!admitted) return NULL;

  // Bind the domain through the thread-local hand-off consumed by
  // `mi_heap_qsbr_init` during heap initialization.  Afterwards the binding
  // lives in the heap metadata and is independent of the creating thread.
  _mi_qsbr_creation_domain_set(domain);
  mi_heap_t* heap = mi_heap_new_qsbr();
  _mi_qsbr_creation_domain_set(NULL);
  if (heap == NULL || _mi_qsbr_heap_domain(heap) != domain) {
    // Roll back the reservation; if a stray heap was created, do not leak it.
    if (heap != NULL) { _mi_qsbr_heap_destroy_allocator(heap); }
    mi_lock(&domain->lock) {
      if (domain->bound_heaps > 0) { domain->bound_heaps--; }
    }
    return NULL;
  }
  return heap;
}

bool mi_qsbr_heap_destroy(mi_qsbr_domain_t* domain, mi_heap_t* heap) {
  if (domain == NULL || heap == NULL) return false;
  if (_mi_qsbr_heap_domain(heap) != domain) return false;   // another domain's heap

  bool claimed = false;    // we set `terminating` ourselves
  bool ok = false;
  mi_lock(&domain->lock) {
    if (!domain->terminating) {
      // Mutual exclusion with admission: hold the lock so no enter/leave is
      // mid-update, set `terminating` so later enters fail, then check that no
      // read section is active.  Because admission also takes this lock, a
      // reader either incremented `in_section` before us (we see it) or will
      // run after us and observe `terminating` (and fail).
      domain->terminating = true;
      claimed = true;
      if (domain->in_section == 0) {
        ok = true;
      }
    }
  }
  if (!ok) {
    if (claimed) {
      mi_lock(&domain->lock) { domain->terminating = false; }
    }
    return false;
  }

  // No active reader and no new admission possible: terminate the heap.  The
  // caller guarantees no concurrent allocator operations on this heap.  The
  // lock is not held here so poll/register are not blocked behind a longer
  // destruction; `terminating` keeps new readers out.
  _mi_qsbr_heap_destroy_allocator(heap);

  mi_lock(&domain->lock) {
    if (domain->bound_heaps > 0) { domain->bound_heaps--; }
    domain->terminating = false;
  }
  return true;
}
