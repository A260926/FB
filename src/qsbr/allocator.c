/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
  Production allocator entry point for the QSBR runtime adapter.

  This translation unit:
    * enables `MI_QSBR` and fixes the *stable* metadata layout declared in
      `qsbr-internal.h` (`mi_page_qsbr_t` / `mi_heap_qsbr_t`),
    * embeds the whole allocator via `src/static.c`,
    * implements the four host hooks (`mi_heap_qsbr_init`,
      `mi_page_qsbr_clear`, `mi_page_qsbr_is_clear`, `mi_page_qsbr_should_delay`)
      on top of the lock-free domain primitives, and
    * provides the few allocator-side accessors used by `src/qsbr/runtime.c`.

  It does *not* install a libc malloc override: `MI_MALLOC_OVERRIDE` is left
  undefined so this object can coexist with a normal mimalloc library, and the
  caller allocates from the adapter heaps explicitly.  All samples access live
  objects only; nothing here makes reads after `mi_free` legal.
-----------------------------------------------------------------------------*/

#ifndef MI_QSBR
#define MI_QSBR 1
#endif
#if !MI_QSBR
#error "allocator.c is the QSBR build entry point; do not build it with MI_QSBR=0"
#endif

// Stable metadata layout.  These struct tags must be complete before any
// internal header (or `src/static.c`) embeds them by value.
#include "qsbr-internal.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// The whole allocator as a single translation unit.  `MI_QSBR` is 1 here, so
// the page/heap structs include the `qsbr` metadata and the QSBR code paths.
#include "../static.c"

// ----------------------------------------------------------------------------
// Host hooks (called by mimalloc; must not scan readers, take the registry
// lock, allocate, wait, or collect).
// ----------------------------------------------------------------------------

void mi_heap_qsbr_init(mi_heap_qsbr_t* heap) {
  // Bind the domain chosen by `mi_qsbr_heap_new`.  A raw `mi_heap_new_qsbr()`
  // (without the adapter) binds no domain; its pages then behave as if there
  // were no active readers (delay is decided optimistically, see below).
  heap->domain = _mi_qsbr_creation_domain_get();
}

void mi_page_qsbr_clear(mi_page_qsbr_t* page) {
  // Idempotent; accepts zeroed metadata.  Zero means "no goal recorded".
  page->goal = 0;
}

bool mi_page_qsbr_is_clear(const mi_page_qsbr_t* page) {
  return page->goal == 0;
}

bool mi_page_qsbr_should_delay(mi_page_qsbr_t* page, mi_heap_qsbr_t* heap, size_t page_bytes) {
  (void)page_bytes;   // the adapter does not use deferred-memory thresholds
  mi_qsbr_domain_t* const domain = heap->domain;
  if (domain == NULL) {
    // Protected heap created without an adapter domain: no reader can ever be
    // registered, so every page is safe to release.
    return false;
  }
  if (page->goal == 0) {
    page->goal = _mi_qsbr_domain_next_goal(domain);
    if (page->goal == 0) { return false; }   // defensive: domain vanished
  }
  // Release may proceed once the published safe frontier has reached the goal.
  // Chicken bit: `MI_QSBR_GOAL_NEVER` is greater than any completed value, so
  // such a page is retained conservatively.
  return page->goal > _mi_qsbr_domain_completed(domain);
}

// ----------------------------------------------------------------------------
// Allocator-side accessors for `runtime.c`
// ----------------------------------------------------------------------------

mi_qsbr_domain_t* _mi_qsbr_heap_domain(mi_heap_t* heap) {
  if (heap == NULL || !mi_heap_qsbr_is_enabled(heap)) return NULL;
  return heap->qsbr.domain;
}

void _mi_qsbr_heap_destroy_allocator(mi_heap_t* heap) {
  // Terminal destruction: releases all pages without waiting for grace periods.
  // The adapter only calls this while no reader is in a section and no other
  // thread manipulates the heap.
  mi_heap_destroy(heap);
}
