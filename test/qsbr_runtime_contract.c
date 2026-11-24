/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
  Contract tests for the QSBR runtime adapter (`include/mimalloc/qsbr-runtime.h`).

  These exercise the *production* allocator built through
  `src/qsbr/allocator.c` (MI_QSBR enabled) and observe page retention/release
  only through legal means: allocations of live objects, `mi_free`,
  `mi_heap_collect`, and `mi_heap_visit_blocks`.  No sample reads a freed
  address, installs exception handlers, or builds malformed/crashing input.

  Readers and writers are real threads, arranged with condition variables and
  joins.
-----------------------------------------------------------------------------*/

#include "mimalloc/qsbr-runtime.h"
#include "task_support/check.h"

#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

// ----------------------------------------------------------------------------
// Shared helpers
// ----------------------------------------------------------------------------

typedef struct mi_area_count_s {
  size_t pages;
} mi_area_count_t;

static bool count_area(const mi_heap_t* heap, const mi_heap_area_t* area,
                       void* block, size_t block_size, void* arg) {
  (void)heap; (void)area; (void)block; (void)block_size;
  ((mi_area_count_t*)arg)->pages++;
  return true;
}

// Number of pages a heap retains right now (legal heap visitation).
// Returns SIZE_MAX if the visit failed.
static size_t retained_pages(mi_heap_t* heap) {
  mi_area_count_t c = { 0 };
  if (!mi_heap_visit_blocks(heap, false, &count_area, &c)) return SIZE_MAX;
  return c.pages;
}

// A tiny latch used to arrange two real threads deterministically.
typedef struct latch_s {
  pthread_mutex_t mu;
  pthread_cond_t  cv;
  bool            ready;      // worker has reached the rendezvous
  bool            go;         // main may proceed / worker may continue
  bool            done;       // worker completed
} latch_t;

static void latch_init(latch_t* l) {
  pthread_mutex_init(&l->mu, NULL);
  pthread_cond_init(&l->cv, NULL);
  l->ready = false; l->go = false; l->done = false;
}
static void latch_destroy(latch_t* l) {
  pthread_mutex_destroy(&l->mu);
  pthread_cond_destroy(&l->cv);
}
static void latch_wait_ready(latch_t* l) {
  pthread_mutex_lock(&l->mu);
  while (!l->ready) pthread_cond_wait(&l->cv, &l->mu);
  pthread_mutex_unlock(&l->mu);
}
static void latch_set_ready(latch_t* l) {
  pthread_mutex_lock(&l->mu);
  l->ready = true;
  pthread_cond_broadcast(&l->cv);
  pthread_mutex_unlock(&l->mu);
}
static void latch_wait_go(latch_t* l) {
  pthread_mutex_lock(&l->mu);
  while (!l->go) pthread_cond_wait(&l->cv, &l->mu);
  pthread_mutex_unlock(&l->mu);
}
static void latch_go(latch_t* l) {
  pthread_mutex_lock(&l->mu);
  l->go = true;
  pthread_cond_broadcast(&l->cv);
  pthread_mutex_unlock(&l->mu);
}
static void latch_wait_done(latch_t* l) {
  pthread_mutex_lock(&l->mu);
  while (!l->done) pthread_cond_wait(&l->cv, &l->mu);
  pthread_mutex_unlock(&l->mu);
}
static void latch_set_done(latch_t* l) {
  pthread_mutex_lock(&l->mu);
  l->done = true;
  pthread_cond_broadcast(&l->cv);
  pthread_mutex_unlock(&l->mu);
}

// A 4 MiB allocation is a single-block page: easy to observe as retained/released.
#define BIG  ((size_t)(4 * 1024 * 1024))

// Allocate a page's worth of memory and verify it is readable (live object).
static unsigned char* alloc_big(mi_heap_t* heap) {
  unsigned char* p = (unsigned char*)mi_heap_malloc(heap, BIG);
  if (p == NULL) return NULL;
  p[0] = 7;
  p[BIG - 1] = 9;
  return p;
}

// ----------------------------------------------------------------------------
// EPOCHS: old readers block, new readers do not roll back the frontier,
// nesting, slot reuse.
// ----------------------------------------------------------------------------

// An old reader (entered before the page was retired) blocks release; once it
// leaves, an ordinary collect reclaims the page.
static int epoch_old_reader_blocks(void) {
  mi_qsbr_domain_t* d = mi_qsbr_domain_new(2);
  TASK_CHECK(d != NULL);
  mi_qsbr_reader_t* old = mi_qsbr_register(d);
  TASK_CHECK(old != NULL);
  mi_heap_t* h = mi_qsbr_heap_new(d);
  TASK_CHECK(h != NULL);

  unsigned char* p = alloc_big(h);
  TASK_CHECK(p != NULL);

  TASK_CHECK(mi_qsbr_enter(old));       // begins before retirement
  mi_free(p);
  mi_heap_collect(h, true);
  TASK_CHECK(retained_pages(h) == 1);
  const uint64_t before = mi_qsbr_poll(d);
  TASK_CHECK(retained_pages(h) == 1);
  TASK_CHECK(mi_qsbr_enter(old));       // nested; must not refresh the boundary
  TASK_CHECK(mi_qsbr_leave(old));       // still inside
  const uint64_t mid = mi_qsbr_poll(d);
  TASK_CHECK(mid >= before);
  TASK_CHECK(retained_pages(h) == 1);
  TASK_CHECK(mi_qsbr_leave(old));       // outermost leave ends protection
  const uint64_t after = mi_qsbr_poll(d);
  TASK_CHECK(after > before);
  mi_heap_collect(h, true);
  TASK_CHECK(retained_pages(h) == 0);

  TASK_CHECK(mi_qsbr_heap_destroy(d, h));
  TASK_CHECK(mi_qsbr_unregister(old));
  TASK_CHECK(mi_qsbr_domain_delete(d));
  return 0;
}

// A reader that enters *after* a goal was established must not block it, and
// must not make the completed frontier regress.
static int epoch_new_reader_no_rollback(void) {
  mi_qsbr_domain_t* d = mi_qsbr_domain_new(3);
  TASK_CHECK(d != NULL);
  mi_qsbr_reader_t* first = mi_qsbr_register(d);
  TASK_CHECK(first != NULL);
  mi_heap_t* h = mi_qsbr_heap_new(d);
  TASK_CHECK(h != NULL);

  unsigned char* p = alloc_big(h);
  TASK_CHECK(p != NULL);
  mi_free(p);
  mi_heap_collect(h, true);             // establishes an old goal with no readers
  TASK_CHECK(retained_pages(h) == 1);
  uint64_t frontier = mi_qsbr_poll(d);
  TASK_CHECK(retained_pages(h) == 1);   // still delayed until collected again

  // A registered but idle reader must not block progress: registration is not
  // a read section.
  mi_qsbr_reader_t* idle = mi_qsbr_register(d);
  TASK_CHECK(idle != NULL);
  const uint64_t idle_frontier = mi_qsbr_poll(d);
  TASK_CHECK(idle_frontier >= frontier);
  TASK_CHECK(retained_pages(h) == 1);   // not yet collected again
  mi_heap_collect(h, true);
  TASK_CHECK(retained_pages(h) == 0);   // idle reader did not hold it back

  // New reader enters after the goal; it must not hold that goal back either.
  mi_qsbr_reader_t* fresh = mi_qsbr_register(d);
  TASK_CHECK(fresh != NULL);
  unsigned char* q = alloc_big(h);
  TASK_CHECK(q != NULL);
  mi_free(q);
  mi_heap_collect(h, true);             // establishes a fresh goal with no reader in a section
  TASK_CHECK(retained_pages(h) == 1);
  TASK_CHECK(mi_qsbr_enter(fresh));
  const uint64_t newer = mi_qsbr_poll(d);
  TASK_CHECK(newer >= frontier);        // monotone, never regresses
  mi_heap_collect(h, true);
  // The new reader entered after retirement, so it does not protect the page.
  TASK_CHECK(retained_pages(h) == 0);
  TASK_CHECK(mi_qsbr_poll(d) >= newer);

  TASK_CHECK(mi_qsbr_leave(fresh));
  TASK_CHECK(!mi_qsbr_leave(fresh));    // leave outside a section is rejected
  TASK_CHECK(mi_qsbr_unregister(fresh));
  TASK_CHECK(mi_qsbr_unregister(idle));

  TASK_CHECK(mi_qsbr_heap_destroy(d, h));
  TASK_CHECK(mi_qsbr_unregister(first));
  TASK_CHECK(mi_qsbr_domain_delete(d));
  return 0;
}

// Nested enter/leave bookkeeping, including rejection of a leave outside any
// section without changing state.
static int epoch_nested_sections(void) {
  mi_qsbr_domain_t* d = mi_qsbr_domain_new(2);
  TASK_CHECK(d != NULL);
  mi_qsbr_reader_t* r = mi_qsbr_register(d);
  TASK_CHECK(r != NULL);

  TASK_CHECK(!mi_qsbr_leave(r));        // outside: false, no state change
  TASK_CHECK(mi_qsbr_enter(r));
  TASK_CHECK(mi_qsbr_enter(r));
  TASK_CHECK(mi_qsbr_enter(r));
  TASK_CHECK(!mi_qsbr_unregister(r));   // in a section: refused
  TASK_CHECK(mi_qsbr_leave(r));
  TASK_CHECK(mi_qsbr_leave(r));
  TASK_CHECK(!mi_qsbr_unregister(r));   // still one level deep
  TASK_CHECK(mi_qsbr_leave(r));         // outermost
  TASK_CHECK(!mi_qsbr_leave(r));
  TASK_CHECK(mi_qsbr_unregister(r));
  TASK_CHECK(mi_qsbr_domain_delete(d));
  return 0;
}

// A reused slot inherits neither the old epoch nor the old nesting depth.
static int epoch_slot_reuse_resets(void) {
  mi_qsbr_domain_t* d = mi_qsbr_domain_new(1);
  TASK_CHECK(d != NULL);
  mi_qsbr_reader_t* r1 = mi_qsbr_register(d);
  TASK_CHECK(r1 != NULL);
  TASK_CHECK(mi_qsbr_enter(r1));
  // Filling the single slot must fail while registered.
  TASK_CHECK(mi_qsbr_register(d) == NULL);
  TASK_CHECK(mi_qsbr_leave(r1));
  TASK_CHECK(mi_qsbr_unregister(r1));

  // Re-register: the only slot is reused.  It must start idle (no depth).
  mi_qsbr_reader_t* r2 = mi_qsbr_register(d);
  TASK_CHECK(r2 != NULL);
  TASK_CHECK(!mi_qsbr_leave(r2));       // depth reset to 0
  TASK_CHECK(mi_qsbr_enter(r2));
  TASK_CHECK(mi_qsbr_leave(r2));
  TASK_CHECK(mi_qsbr_unregister(r2));
  TASK_CHECK(mi_qsbr_domain_delete(d));
  return 0;
}

// ----------------------------------------------------------------------------
// DOMAINS: two independent domains, two heaps in one domain.
// ----------------------------------------------------------------------------

static int domains_two_domains_independent(void) {
  mi_qsbr_domain_t* d1 = mi_qsbr_domain_new(1);
  mi_qsbr_domain_t* d2 = mi_qsbr_domain_new(1);
  TASK_CHECK(d1 != NULL && d2 != NULL);
  mi_qsbr_reader_t* r1 = mi_qsbr_register(d1);
  mi_qsbr_reader_t* r2 = mi_qsbr_register(d2);
  TASK_CHECK(r1 != NULL && r2 != NULL);
  mi_heap_t* h1 = mi_qsbr_heap_new(d1);
  mi_heap_t* h2 = mi_qsbr_heap_new(d2);
  TASK_CHECK(h1 != NULL && h2 != NULL);

  unsigned char* p1 = alloc_big(h1);
  unsigned char* p2 = alloc_big(h2);
  TASK_CHECK(p1 != NULL && p2 != NULL);

  TASK_CHECK(mi_qsbr_enter(r1));        // only domain 1 has an active reader
  mi_free(p1);
  mi_free(p2);
  mi_heap_collect(h1, true);
  mi_heap_collect(h2, true);
  TASK_CHECK(retained_pages(h1) == 1);
  TASK_CHECK(retained_pages(h2) == 1);

  mi_qsbr_poll(d1);
  mi_qsbr_poll(d2);
  mi_heap_collect(h2, true);            // domain 2 has no active reader
  TASK_CHECK(retained_pages(h2) == 0);
  TASK_CHECK(retained_pages(h1) == 1);  // domain 1 still protected

  TASK_CHECK(mi_qsbr_leave(r1));
  mi_qsbr_poll(d1);
  mi_heap_collect(h1, true);
  TASK_CHECK(retained_pages(h1) == 0);

  // Cross-domain destroy is rejected.
  TASK_CHECK(!mi_qsbr_heap_destroy(d1, h2));
  TASK_CHECK(!mi_qsbr_heap_destroy(d2, h1));
  TASK_CHECK(mi_qsbr_heap_destroy(d1, h1));
  TASK_CHECK(mi_qsbr_heap_destroy(d2, h2));
  TASK_CHECK(mi_qsbr_unregister(r1));
  TASK_CHECK(mi_qsbr_unregister(r2));
  TASK_CHECK(mi_qsbr_domain_delete(d1));
  TASK_CHECK(mi_qsbr_domain_delete(d2));
  return 0;
}

static int domains_two_heaps_one_domain(void) {
  mi_qsbr_domain_t* d = mi_qsbr_domain_new(2);
  TASK_CHECK(d != NULL);
  mi_qsbr_reader_t* r = mi_qsbr_register(d);
  TASK_CHECK(r != NULL);
  mi_heap_t* h1 = mi_qsbr_heap_new(d);
  mi_heap_t* h2 = mi_qsbr_heap_new(d);
  TASK_CHECK(h1 != NULL && h2 != NULL && h1 != h2);

  unsigned char* p1 = alloc_big(h1);
  unsigned char* p2 = alloc_big(h2);
  TASK_CHECK(p1 != NULL && p2 != NULL);
  TASK_CHECK(mi_qsbr_enter(r));
  mi_free(p1);
  mi_free(p2);
  mi_heap_collect(h1, true);
  mi_heap_collect(h2, true);
  TASK_CHECK(retained_pages(h1) == 1 && retained_pages(h2) == 1);

  TASK_CHECK(!mi_qsbr_heap_destroy(d, h1));   // active reader blocks termination
  TASK_CHECK(mi_qsbr_leave(r));
  mi_qsbr_poll(d);
  mi_heap_collect(h1, true);
  mi_heap_collect(h2, true);
  TASK_CHECK(retained_pages(h1) == 0 && retained_pages(h2) == 0);

  TASK_CHECK(mi_qsbr_heap_destroy(d, h1));    // each heap ends independently
  TASK_CHECK(retained_pages(h2) == 0);
  TASK_CHECK(mi_qsbr_heap_destroy(d, h2));
  TASK_CHECK(mi_qsbr_unregister(r));
  TASK_CHECK(mi_qsbr_domain_delete(d));
  return 0;
}

// ----------------------------------------------------------------------------
// LIFECYCLE: capacity, heap binding across threads, remote free after the
// creating thread exits, reject-then-continue, bounded slot storage.
// ----------------------------------------------------------------------------

// Registration storage is bounded and reused; failures leave no stray reader or
// half-bound heap.
static int lifecycle_capacity_and_reuse(void) {
  mi_qsbr_domain_t* d = mi_qsbr_domain_new(2);
  TASK_CHECK(d != NULL);
  mi_qsbr_reader_t* r1 = mi_qsbr_register(d);
  mi_qsbr_reader_t* r2 = mi_qsbr_register(d);
  TASK_CHECK(r1 != NULL && r2 != NULL);
  TASK_CHECK(mi_qsbr_register(d) == NULL);   // at capacity
  TASK_CHECK(mi_qsbr_unregister(r1));
  mi_qsbr_reader_t* r3 = mi_qsbr_register(d); // reuses the freed slot
  TASK_CHECK(r3 != NULL);
  TASK_CHECK(mi_qsbr_register(d) == NULL);
  TASK_CHECK(mi_qsbr_unregister(r2));
  TASK_CHECK(mi_qsbr_unregister(r3));
  TASK_CHECK(mi_qsbr_domain_delete(d));

  TASK_CHECK(mi_qsbr_domain_new(0) == NULL);  // zero capacity fails
  return 0;
}

// The heap binds to its domain independently of the creating thread: after the
// creator exits, a different thread can still free and collect in the heap and
// the domain's reader still decides when the page is released.
typedef struct creator_args_s {
  mi_heap_t* heap;
  void*      block;
  int        ok;
} creator_args_t;

static void* lifecycle_creator_thread(void* arg) {
  creator_args_t* a = (creator_args_t*)arg;
  unsigned char* p = alloc_big(a->heap);
  if (p == NULL) { a->ok = 0; return NULL; }
  a->block = p;
  a->ok = 1;
  return NULL;   // thread exits with the heap alive
}

static int lifecycle_remote_free_after_creator_exit(void) {
  mi_qsbr_domain_t* d = mi_qsbr_domain_new(2);
  TASK_CHECK(d != NULL);
  mi_qsbr_reader_t* r = mi_qsbr_register(d);
  TASK_CHECK(r != NULL);
  mi_heap_t* h = mi_qsbr_heap_new(d);
  TASK_CHECK(h != NULL);

  creator_args_t args = { h, NULL, 0 };
  pthread_t th;
  TASK_CHECK(pthread_create(&th, NULL, lifecycle_creator_thread, &args) == 0);
  TASK_CHECK(pthread_join(th, NULL) == 0);
  TASK_CHECK(args.ok && args.block != NULL);
  TASK_CHECK(retained_pages(h) == 1);      // the creator's page survives it

  TASK_CHECK(mi_qsbr_enter(r));            // still entered from this thread
  mi_free(args.block);                     // remote free: creator already exited
  mi_heap_collect(h, true);
  TASK_CHECK(retained_pages(h) == 1);
  mi_qsbr_poll(d);
  TASK_CHECK(retained_pages(h) == 1);
  TASK_CHECK(mi_qsbr_leave(r));
  mi_qsbr_poll(d);
  mi_heap_collect(h, true);
  TASK_CHECK(retained_pages(h) == 0);      // same domain governs the release

  TASK_CHECK(mi_qsbr_heap_destroy(d, h));
  TASK_CHECK(mi_qsbr_unregister(r));
  TASK_CHECK(mi_qsbr_domain_delete(d));
  return 0;
}

// A worker thread enters a read section, blocks termination while it is active,
// then leaves; the heap remains usable after a refused destroy and is reclaimed
// once the reader is gone.
typedef struct worker_args_s {
  mi_qsbr_domain_t* domain;
  mi_qsbr_reader_t* reader;
  latch_t*          ready;     // set once the worker is inside the section
  latch_t*          release;   // main signals the worker to leave
  latch_t*          left;      // set once the worker has left
  int               ok;
} worker_args_t;

static void* lifecycle_reader_thread(void* arg) {
  worker_args_t* w = (worker_args_t*)arg;
  w->ok = 0;
  if (!mi_qsbr_enter(w->reader)) return NULL;
  latch_set_ready(w->ready);
  latch_wait_go(w->release);
  if (!mi_qsbr_leave(w->reader)) return NULL;
  latch_set_done(w->left);
  w->ok = 1;
  return NULL;
}

static int lifecycle_destroy_rejected_then_continue(void) {
  mi_qsbr_domain_t* d = mi_qsbr_domain_new(2);
  TASK_CHECK(d != NULL);
  mi_qsbr_reader_t* r = mi_qsbr_register(d);
  TASK_CHECK(r != NULL);
  mi_heap_t* h = mi_qsbr_heap_new(d);
  TASK_CHECK(h != NULL);

  unsigned char* p = alloc_big(h);
  TASK_CHECK(p != NULL);
  mi_free(p);
  mi_heap_collect(h, true);
  TASK_CHECK(retained_pages(h) == 1);

  // A worker enters a read section; its enter/leave are serialized on that
  // thread only, while the main thread drives destroy.
  latch_t ready, release, left;
  latch_init(&ready); latch_init(&release); latch_init(&left);
  worker_args_t w = { d, r, &ready, &release, &left, 0 };
  pthread_t th;
  TASK_CHECK(pthread_create(&th, NULL, lifecycle_reader_thread, &w) == 0);
  latch_wait_ready(&ready);

  TASK_CHECK(!mi_qsbr_heap_destroy(d, h));   // refused: reader active elsewhere
  // The heap is still usable after the refusal: a new allocation succeeds.
  unsigned char* q = alloc_big(h);
  TASK_CHECK(q != NULL);
  TASK_CHECK(retained_pages(h) >= 1);
  mi_free(q);

  latch_go(&release);
  latch_wait_done(&left);
  TASK_CHECK(pthread_join(th, NULL) == 0);
  TASK_CHECK(w.ok);

  mi_qsbr_poll(d);
  mi_heap_collect(h, true);
  TASK_CHECK(retained_pages(h) == 0);        // finally reclaimed
  TASK_CHECK(mi_qsbr_heap_destroy(d, h));    // now allowed
  TASK_CHECK(mi_qsbr_unregister(r));
  TASK_CHECK(mi_qsbr_domain_delete(d));
  latch_destroy(&ready); latch_destroy(&release); latch_destroy(&left);
  return 0;
}

// Domain and heap lifetime: a domain with a bound heap cannot be deleted, and a
// heap with readers cannot be destroyed; both succeed once freed of obligations.
static int lifecycle_domain_and_heap_lifetimes(void) {
  mi_qsbr_domain_t* d = mi_qsbr_domain_new(1);
  TASK_CHECK(d != NULL);
  mi_heap_t* h = mi_qsbr_heap_new(d);
  TASK_CHECK(h != NULL);
  TASK_CHECK(!mi_qsbr_domain_delete(d));    // still has a heap
  TASK_CHECK(mi_qsbr_heap_destroy(d, h));
  TASK_CHECK(mi_qsbr_domain_delete(d));     // now empty

  // Reader-only domain: cannot delete until the reader unregisters.
  mi_qsbr_domain_t* d2 = mi_qsbr_domain_new(1);
  TASK_CHECK(d2 != NULL);
  mi_qsbr_reader_t* r = mi_qsbr_register(d2);
  TASK_CHECK(r != NULL);
  TASK_CHECK(!mi_qsbr_domain_delete(d2));
  TASK_CHECK(mi_qsbr_unregister(r));
  TASK_CHECK(mi_qsbr_domain_delete(d2));
  return 0;
}

// ----------------------------------------------------------------------------
// Case registration
// ----------------------------------------------------------------------------

const struct task_case task_cases[] = {
  { "epochs",    "old_reader_blocks",           &epoch_old_reader_blocks },
  { "epochs",    "new_reader_no_rollback",      &epoch_new_reader_no_rollback },
  { "epochs",    "nested_sections",             &epoch_nested_sections },
  { "epochs",    "slot_reuse_resets",           &epoch_slot_reuse_resets },
  { "domains",   "two_domains_independent",     &domains_two_domains_independent },
  { "domains",   "two_heaps_one_domain",        &domains_two_heaps_one_domain },
  { "lifecycle", "capacity_and_reuse",          &lifecycle_capacity_and_reuse },
  { "lifecycle", "remote_free_after_exit",      &lifecycle_remote_free_after_creator_exit },
  { "lifecycle", "destroy_rejected_continue",   &lifecycle_destroy_rejected_then_continue },
  { "lifecycle", "domain_and_heap_lifetimes",   &lifecycle_domain_and_heap_lifetimes },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
