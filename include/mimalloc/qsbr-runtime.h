/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
  Runtime reader domains for QSBR-protected mimalloc heaps.

  This is the public, C-callable adapter on top of the internal QSBR hooks in
  `include/mimalloc/qsbr.h`.  A *domain* owns a fixed number of reader slots
  and a monotonically increasing QSBR sequence.  A *reader* is a slot handle.
  A heap created with `mi_qsbr_heap_new` is bound to one domain for its whole
  lifetime (independently of the creating thread), and empty pages of that
  heap are only returned to their arena or to the OS after every reader that
  began a read section before the page was retired has left it.

  QSBR only protects *page lifetime*.  It does not make reads after `mi_free`
  legal, and it does not forbid the allocator from reusing blocks of the same
  size class within the same heap while readers are active.  A reader must
  only access live objects; it may observe page retention/release through
  legal heap visitation.

  Threading rules:
  - A single reader's `enter`/`leave`/`unregister` calls must be serialized by
    the caller.  Different readers of the same or different domains may be
    used concurrently.
  - `mi_qsbr_poll` may run on any ordinary thread.  It never waits for active
    readers to leave, and it never performs a heap collection itself.
  - After `mi_qsbr_unregister` the reader pointer must not be used again.
  - `mi_qsbr_heap_destroy` requires that no other thread performs heap
    allocation, free, collect, or lifecycle operations on that heap during the
    call; concurrent reader enter/leave is handled.
-----------------------------------------------------------------------------*/

#pragma once
#ifndef MI_QSBR_RUNTIME_H
#define MI_QSBR_RUNTIME_H

#include "mimalloc.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle types.
typedef struct mi_qsbr_domain_s mi_qsbr_domain_t;
typedef struct mi_qsbr_reader_s mi_qsbr_reader_t;

// Create a domain that can hold up to `capacity` simultaneously registered
// readers.  Returns NULL if `capacity == 0` or if storage cannot be obtained.
// The returned domain must be released with `mi_qsbr_domain_delete`.
mi_decl_nodiscard mi_decl_export mi_qsbr_domain_t* mi_qsbr_domain_new(size_t capacity);

// Delete a domain.  Succeeds only when no reader is registered, no heap is
// bound to the domain, and no termination is in progress.  On success the
// domain pointer is invalid.
mi_decl_export bool mi_qsbr_domain_delete(mi_qsbr_domain_t* domain);

// Register a reader in the domain.  Returns NULL if the domain is at capacity,
// is shutting down, or storage cannot be obtained.  Registration itself does
// not constitute a read section; a freshly registered (or reused) slot never
// inherits an old epoch or nesting depth.
mi_decl_nodiscard mi_decl_export mi_qsbr_reader_t* mi_qsbr_register(mi_qsbr_domain_t* domain);

// Unregister a reader.  Fails (returning false) while the reader is inside a
// read section.  On success the reader pointer is invalid and its slot may be
// reused by a later registration.
mi_decl_export bool mi_qsbr_unregister(mi_qsbr_reader_t* reader);

// Enter a read section; returns false if the domain is terminating or the
// reader is invalid.  Nested enters are supported: only the outermost enter
// publishes the reader's epoch and only the outermost leave ends protection.
mi_decl_export bool mi_qsbr_enter(mi_qsbr_reader_t* reader);

// Leave a read section.  Returns false, without changing any state, when the
// reader is not inside a read section.
mi_decl_export bool mi_qsbr_leave(mi_qsbr_reader_t* reader);

// Advance the domain in an ordinary thread context and return the completed
// (safe) sequence.  A page with goal `g` may be released once the returned
// value is `>= g`.  Polling never blocks on active readers.
mi_decl_export uint64_t mi_qsbr_poll(mi_qsbr_domain_t* domain);

// Create a heap that has QSBR protection enabled and is bound to `domain` for
// its whole lifetime.  Returns NULL on failure.
mi_decl_nodiscard mi_decl_export mi_heap_t* mi_qsbr_heap_new(mi_qsbr_domain_t* domain);

// Terminate `heap`, which must be bound to `domain`.  Returns false if the heap
// belongs to another domain, or if any reader in the domain is inside a read
// section (the heap is then left fully usable).  On success the heap is
// destroyed and all its pages are released.
//
// Successful termination and admission of new readers are mutually exclusive:
// during the (short) termination window new `mi_qsbr_enter` calls may return
// false.  The caller guarantees that no other thread allocates, frees,
// collects, or otherwise manipulates the heap concurrently.
mi_decl_export bool mi_qsbr_heap_destroy(mi_qsbr_domain_t* domain, mi_heap_t* heap);

#ifdef __cplusplus
}
#endif

#endif  // MI_QSBR_RUNTIME_H
