# Page shield (`SNMALLOC_ENABLE_PAGE_SHIELD`)

An optional, owner-local **empty OS page access barrier** for partially used
small slabs.  When a page of a small slab contains no outstanding object,
and every free slot intersecting that page has been removed from snmalloc's
reachable free lists, the page can be `mprotect(PROT_NONE)`'d.  Live objects
elsewhere in the same slab keep working.  The feature is a memory-safety
mitigation only: it turns a stray access to a genuinely empty page into a
fault and gives read-only diagnostics.

The switch is **off by default**.  With it off, none of the hook call sites
in `mem/corealloc.h` / `mem/metadata.h` are compiled (they are guarded by
`SNMALLOC_ENABLE_PAGE_SHIELD && SNMALLOC_PAGE_SHIELD_HEADER`), so snmalloc
remains the ordinary header-only library and gains no link dependency.
Turning it on compiles the narrow hooks and links `src/page_shield.cc`,
which implements:

```cpp
size_t snmalloc::page_shield_collect(Alloc&, size_t max_bytes) noexcept;
size_t snmalloc::page_shield_release(Alloc&) noexcept;
bool   snmalloc::page_shield_query(Alloc&, uintptr_t,
                                  page_shield_info&) noexcept;
```

All three, and the registry they touch, are serialized by the caller being
the current owner of that `Alloc`, exactly like its other consumer
operations.  Other threads keep freeing objects they own through their own
`Alloc`; the global registry has its own mutex only to protect its maps.

## Where the state lives

All shield state is **external metadata** in `src/page_shield.cc` (a
`std::map` keyed by aligned slab address plus a `meta* -> slab` index).  It
is never stored on a protected user page and never allocated through the
managed `snmalloc::Alloc` (the task link lines do not compile snmalloc's
`operator new` translation unit, so the registry uses the system C++
allocator).  Per slab we keep:

- geometry (`sizeclass`, slab/object/page sizes, page count),
- `live`, the number of outstanding objects,
- `outstanding[page]`, the number of outstanding objects intersecting each
  OS page,
- `shielded[page]`, the current protection state,
- `parked[]`, the addresses of free slots that were removed from
  `meta.free_queue` because they intersect a shielded page.

The counters are maintained by exactly-once hooks:

- `finish_alloc()` calls `page_shield_hook_alloc_address()` after a small
  object is delivered (`+1` for every page the object spans).  Hooking
  `finish_alloc` rather than the several `small_alloc` return paths makes the
  CheckInit restart path count the object exactly once, on the real pool
  allocator.
- The owner-local branch of `Allocator::dealloc()` calls the `-1` hook.
- A remote free is counted only when the owner receives the batch in
  `dealloc_local_objects_fast()`: the ring is walked for its `length` nodes
  and each is decremented.  A remote *send* deliberately does not decrement,
  so an unpublished/unreceived object stays outstanding and its pages are
  ineligible for shielding.

## Chain reachability

A free slot stores its encoded `next` pointer (and, under
`SNMALLOC_CHECK_CLIENT`, a signed back edge) in the first words of the slot.
A protected page is therefore safe only if **no reachable chain can land on
one of its slots**.  Before calling `mprotect(PROT_NONE)` we:

1. `alloc.flush()` first.  This receives published remote frees and drains
   the per-sizeclass `small_fast_free_lists[]` back into
   `meta.free_queue`, so after the flush every free slot of a slab resides
   in that slab's `freelist::Builder free_queue`.
2. Close/drain the (possibly two-list, randomized) builder with the public
   `close()` / `Iter::take()` API, collecting every free node.
3. Rebuild `free_queue` with only the nodes that do **not** intersect a
   page we are shielding.  Nodes that intersect a shielded page are moved to
   the external `parked[]` list; nothing in snmalloc then points at them.

Consequently none of the reachable writers/readers touch a protected page:

- **fast list**: drained before shielding; later refills only ever receive
  rebuilt, unprotected nodes, and parked nodes are re-threaded before a list
  is closed (see below).
- **`free_queue`**: contains only accessible nodes after the rebuild.
- **remote receive**: a shielded page has `outstanding[page] == 0`, so no
  in-flight/unreceived object intersects it; a later receive cannot target
  it.  Received batches are appended with the normal
  `append_segment` onto accessible queues.
- **`clear_slab()`**: before its validation walk, the
  `page_shield_hook_clear_slab()` hook restores every protected page RW and
  re-threads all parked nodes, so the original count checks see the complete
  `sizeclass_to_slab_object_count()` list.

## Allocation and restoring slots

Allocation always consumes ordinary, unprotected slots first because those
are the only nodes left in the reachable fast list / `free_queue`.  Two
hooks handle exhaustion:

- `BackendSlabMetadata::alloc_free_list()` calls
  `page_shield_hook_pre_free_list(meta, &entropy)` before closing the queue.
  If the queue is empty but slots are parked for that metadata, the minimum
  run is un-parked and re-threaded.
- `Allocator::small_refill()`, when `alloc_classes[sc].length == 0`, calls
  `page_shield_hook_parked_meta(this, sc)`.  If this owner has a sleeping
  (`laden`) slab of that size class with parked slots, the hook restores the
  minimum pages, re-threads the slots, detaches the metadata node from
  `laden`, and `small_refill` enqueues it on `available` before the normal
  pop/refill logic.  The randomized "occasionally skip the last list and
  open a fresh slab" mitigation is suppressed for a metadata that was just
  un-parked, so a restored slot is actually delivered.

`flush()`, `debug_is_empty()`, `cleanup_unused()` and allocator pooling need
no special unprotection: their free-list validation only walks the rebuilt
accessible queue, and the `clear_slab` hook performs the full restore when a
slab becomes genuinely unused.  While a slab still has live objects, an
ordinary `flush()` or thread exit never removes its page barriers.

## Sleeping / `needed` versus protected slots

`needed_` is snmalloc's threshold countdown, **not** a free-slot count and
not our `outstanding`.  Shielding deliberately does **not** modify
`needed_` or `sleeping_`: parking nodes out of `free_queue` only changes
where the free pointers live, while the original deallocation accounting
already happened when those objects were returned.  Therefore:

- an awake/available metadata stays on `alloc_classes[sc].available`; if its
  rebuilt queue is empty, `pre_free_list` re-parks on demand before
  `close()` would dereference a null head;
- a sleeping metadata stays in `laden`; allocation does not normally wake
  it, which is exactly why the length-zero `parked_meta` activation path
  exists;
- when the last real objects are freed, snmalloc's own countdown reaches
  zero through the unchanged accounting and drives the normal
  `dealloc_local_slabs()` → `clear_slab()` path, where our hook restores the
  full list for validation and backend return.

The only bookkeeping touch in `small_refill` is re-seeding a zero `needed`
to the waking threshold for a metadata we explicitly move from `laden` to
`available`, balancing the subsequent `length--` / possible `unused--`;
`alloc_free_list()` immediately recomputes the real value via
`set_sleeping()`.

## Budget shrinking and ascending retention

`page_shield_collect(alloc, max_bytes)` computes
`K = floor(max_bytes / runtime_page_size)` and forms the union of newly
eligible empty pages and previously shielded pages for this owner.  Pages
are sorted by **integer page address ascending**; the first `K` are retained
and every other currently shielded page is restored.  Reconciliation is
performed per slab as: restore all of that slab's shielded pages and
re-thread all parked nodes, then drain the whole queue once and re-park only
the nodes intersecting the newly retained page set, followed by the
`PROT_NONE` transitions.  This makes repeated collect calls and budget
tightening total and idempotent.  The return value is the number of pages
actually protected after reconciliation times the runtime page size; a zero
budget restores everything and returns zero.

## Cross-page objects and cross-page restore

An object spanning an OS page boundary contributes one outstanding count to
**every** page its `[address, address + sizeclass_size)` range intersects, so
none of those pages is eligible while the object is live/in-flight.  When a
parked slot must be reused, "the pages touched by at least one usable slot"
means the full page range of the chosen slot.  `unpark_minimal()` restores
exactly that range and re-threads every parked slot whose entire span is now
RW; slots still intersecting a protected page remain parked.  Thus a
delivered object is always fully accessible.

## Diagnostics

`page_shield_query()` uses only the external registry, the snmalloc pagemap
metadata entry and a read-only OS VM-region permission probe
(`mach_vm_region` on macOS, `/proc/self/maps` on Linux).  It never
dereferences the queried address, never unprotects and never pumps
reclamation.  `outstanding > 0` is `active`; otherwise the OS permission
decides `free` (RW) versus `shielded` (no read).  Addresses outside a small
slab currently owned by the referenced `Alloc` return `false` and leave the
caller's `out` structure untouched.

On a failed permission transition the implementation prints
`PAGE_SHIELD_PERMISSION_FAILURE` to stderr and aborts rather than continuing
with chains whose protection is unknown.
