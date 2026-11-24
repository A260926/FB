/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
  QSBR runtime adapter example: a legal read section keeps a retired page alive;
  once the reader leaves, a normal collect returns the page.

  Usage and lifetime
  ------------------
  * `mi_qsbr_domain_new(capacity)` creates a reader domain and its bounded
    slot storage.  Release it with `mi_qsbr_domain_delete` only after every
    reader is unregistered and every heap bound to it is destroyed.
  * `mi_qsbr_register`/`mi_qsbr_unregister` add and remove a reader slot; the
    slot storage is reused, and a fresh registration never inherits an old
    epoch or nesting depth.
  * `mi_qsbr_enter`/`mi_qsbr_leave` bracket a read section.  They nest: only the
    outermost leave ends protection.  A single reader's enter/leave are
    serialized by the caller; different readers may run concurrently.
  * `mi_qsbr_heap_new(domain)` creates a heap whose empty pages are only
    returned to the arena/OS after all readers that entered before the page was
    retired have left.  The heap is bound to the domain for its whole lifetime,
    independent of the creating thread.
  * `mi_qsbr_poll(domain)` advances the grace period from an ordinary thread.
    It never blocks on active readers and does not itself collect the heap;
    call the normal `mi_heap_collect` afterwards to actually return ready pages.
  * `mi_qsbr_heap_destroy(domain, heap)` terminates the heap only when both the
    domain and the heap match and no reader is in a section.  The caller
    guarantees no concurrent allocator operations on that heap.

  QSBR protects page lifetime only.  It does not make reads after `mi_free`
  legal and does not forbid same-heap, same-size-class block reuse.  This
  example only touches live objects and observes retention/release through the
  legal heap visitor (`mi_heap_visit_blocks`).
-----------------------------------------------------------------------------*/

#include "mimalloc/qsbr-runtime.h"

#include <stdio.h>
#include <stdbool.h>

// Count the pages a heap currently retains, using only the public visitor.
static bool count_area(const mi_heap_t* heap, const mi_heap_area_t* area,
                       void* block, size_t block_size, void* arg) {
  (void)heap; (void)area; (void)block; (void)block_size;
  (*(size_t*)arg)++;
  return true;
}

static size_t retained_pages(mi_heap_t* heap) {
  size_t count = 0;
  if (!mi_heap_visit_blocks(heap, false, &count_area, &count)) { return (size_t)-1; }
  return count;
}

int main(void) {
  mi_qsbr_domain_t* domain = mi_qsbr_domain_new(4);
  if (domain == NULL) return 1;
  mi_qsbr_reader_t* reader = mi_qsbr_register(domain);
  if (reader == NULL) return 2;
  mi_heap_t* heap = mi_qsbr_heap_new(domain);
  if (heap == NULL) return 3;

  // A large allocation is a single-block page; it is easy to observe.
  const size_t size = 4 * 1024 * 1024;
  unsigned char* block = (unsigned char*)mi_heap_malloc(heap, size);
  if (block == NULL) return 4;
  block[0] = 11;
  block[size - 1] = 31;

  // Enter a read section *before* the page is retired.
  if (!mi_qsbr_enter(reader)) return 5;

  // Free the only block and collect.  The page is empty but must be delayed:
  // the reader entered before this grace period.
  mi_free(block);
  mi_heap_collect(heap, true);
  const size_t while_reading = retained_pages(heap);
  const uint64_t poll_inside = mi_qsbr_poll(domain);
  printf("example: retained while reading=%zu poll=%llu\n",
         while_reading, (unsigned long long)poll_inside);

  // Leave the read section.  Now the grace period can finish.
  if (!mi_qsbr_leave(reader)) return 6;
  mi_qsbr_poll(domain);
  mi_heap_collect(heap, true);
  const size_t after_reading = retained_pages(heap);
  printf("example: retained after leaving=%zu\n", after_reading);

  // Teardown: destroy the heap (no active readers), then the reader and domain.
  if (!mi_qsbr_heap_destroy(domain, heap)) return 7;
  if (!mi_qsbr_unregister(reader)) return 8;
  if (!mi_qsbr_domain_delete(domain)) return 9;

  if (while_reading != 1 || after_reading != 0) return 10;
  puts("QSBR_RUNTIME_EXAMPLE_OK");
  return 0;
}
