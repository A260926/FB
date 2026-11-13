/* ----------------------------------------------------------------------------
Copyright (c) 2019-2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "mimalloc.h"
#include "mimalloc/prim/prim-tls.h"
#include "mimalloc/util/stats.h"
#include "mimalloc/alloc/page.h"
#include "mimalloc/heap/process.h"
#include "mimalloc/profile/sample-guarded.h"

mi_decl_export void mi_theap_guarded_set_sample_rate(mi_theap_t* theap, size_t sample_rate, size_t seed) {
  theap->guarded_sample_rate  = (sample_rate > MI_SAMPLE_RATE_MAX ? MI_SAMPLE_RATE_MAX : sample_rate);
  if (theap->guarded_sample_rate > 1) {
    if (seed == 0) {
      seed = _mi_theap_random_next(theap);
    }
    theap->guarded_sample_countdown = (seed % theap->guarded_sample_rate) + 1;  // start at random count between 1 and `sample_rate`
  }
  else if (theap->guarded_sample_countdown > theap->guarded_sample_rate) {
    theap->guarded_sample_countdown = theap->guarded_sample_rate;
  }
  _mi_theap_update_sample_rate(theap);
}

mi_decl_export void mi_theap_guarded_set_size_bound(mi_theap_t* theap, size_t min, size_t max) {
  theap->guarded_size_min = min;
  theap->guarded_size_max = (min > max ? min : max);
}

// Set the direction of the guard page for guarded objects sampled on this theap after this call.
// Invalid directions are reported through the regular error channel and leave the direction unchanged.
// Without MI_GUARDED the call still links but the setting is safely ignored.
mi_decl_export void mi_theap_guarded_set_direction(mi_theap_t* theap, mi_guard_direction_t direction) {
  if (direction != MI_GUARD_AFTER && direction != MI_GUARD_BEFORE) {
    _mi_error_message(EINVAL, "invalid guarded direction (%d); keeping the current guard direction\n", (int)direction);
    return;
  }
  #if MI_GUARDED
  if (theap == NULL || _mi_is_empty_theap(theap)) {
    // the static empty theap is read-only; a real theap is initialized before any guarded allocation.
    return;
  }
  theap->guarded_guard_before = (direction == MI_GUARD_BEFORE);
  #else
  MI_UNUSED(theap);
  #endif
}

void _mi_theap_guarded_init(mi_theap_t* theap) {
  theap->guarded_guard_before = false;  // default: guard page after the object
  #if MI_GUARDED
  mi_theap_guarded_set_sample_rate(theap,
    (size_t)mi_option_get_clamp(mi_option_guarded_sample_rate, 0, LONG_MAX),
    (size_t)mi_option_get(mi_option_guarded_sample_seed));
  mi_theap_guarded_set_size_bound(theap,
    (size_t)mi_option_get_clamp(mi_option_guarded_min, 0, LONG_MAX),
    (size_t)mi_option_get_clamp(mi_option_guarded_max, 0, LONG_MAX) );
  #else
  mi_theap_guarded_set_sample_rate(theap,0,0);
  #endif
}

#if MI_GUARDED

// We always allocate a guarded allocation at an offset (`mi_page_has_interior_pointers` will be true).
// We then set the first word of the block to `0` for regular offset aligned allocations (in `alloc/alloc-aligned.c`)
// and the first word to a guarded tag for guarded allocations to have a correct `mi_usable_size`
static void* mi_block_ptr_set_guarded_after(mi_block_t* block, size_t obj_size, size_t* usable_size) {
  // todo: we can still make padding work by moving it out of the guard page area
  mi_page_t* const page = mi_ptr_page(block);
  if mi_unlikely(page==NULL) return NULL;  // should not happen, but avoids gcc -Wstringop-overflow warnings
  block = (mi_block_t*)mi_ptr_rederive(mi_page_start(page), block);  // `block` is bounded to the requested size
  mi_page_set_has_interior_pointers(page, true);
  block->next = MI_BLOCK_TAG_GUARDED;

  // set guard page at the end of the block
  const size_t block_size = mi_page_block_size(page);  // must use `block_size` to match `mi_free_local`
  const size_t os_page_size = _mi_os_page_size();
  mi_assert_internal(block_size >= obj_size + os_page_size + sizeof(mi_block_t));
  if (block_size < obj_size + os_page_size + sizeof(mi_block_t)) {
    // should never happen
    mi_free(block);
    return NULL;
  }
  uint8_t* guard_page = (uint8_t*)mi_page_vmem_ptr(page, (uint8_t*)block + block_size - os_page_size);
  // note: the alignment of the guard page relies on blocks being os_page_size aligned which
  // is ensured in `mi_arena_page_alloc_fresh`.
  mi_assert_internal(mi_is_aligned(block, os_page_size));
  mi_assert_internal(mi_is_aligned(guard_page, os_page_size));
  if (!page->memid.is_pinned && mi_is_aligned(guard_page, os_page_size)) {
    const bool ok = _mi_os_guard(guard_page, os_page_size, true /* committed */);
    if mi_unlikely(!ok) {
      _mi_warning_message("failed to set a guard page behind an object (object %p of size %zu)\n", block, block_size);
    }
  }
  else {
    _mi_warning_message("unable to set a guard page behind an object due to pinned memory (large OS pages?) (object %p of size %zu)\n", block, block_size);
  }

  // align pointer just in front of the guard page
  size_t offset = block_size - os_page_size - obj_size;
  mi_assert_internal(offset > sizeof(mi_block_t));
  if (offset > MI_PAGE_MAX_OVERALLOC_ALIGN) {
    // give up to place it right in front of the guard page if the offset is too large for unalignment
    offset = MI_PAGE_MAX_OVERALLOC_ALIGN;
  }
  uint8_t* const p = (uint8_t*)block + offset;
  mi_assert_internal(p == guard_page - obj_size || offset >= MI_PAGE_MAX_OVERALLOC_ALIGN);
  if (usable_size != NULL) { *usable_size = (guard_page - p); mi_assert_internal(mi_usable_size(p)==*usable_size); }
  mi_track_align(block, p, offset, obj_size);
  mi_track_mem_defined(block, sizeof(mi_block_t));
  return p;
}

// Front guard layout inside one os-page-aligned block of `block_size` bytes:
//
//   [ accessible tag page ][ inaccessible guard page ][ accessible user bytes ... ]
//   ^block                 ^block+psize               ^p == block+2*psize
//
// The allocator tag lives at `block->next` in the accessible leading page, so the first user
// byte at `p` carries no tag and the information needed to locate the block and remove the guard
// stays safely readable. `p` is directly adjacent to the guard page so `p[-1]` faults.
static void* mi_block_ptr_set_guarded_before(mi_block_t* block, size_t obj_size, size_t* usable_size) {
  mi_page_t* const page = mi_ptr_page(block);
  if mi_unlikely(page==NULL) return NULL;  // should not happen
  block = (mi_block_t*)mi_ptr_rederive(mi_page_start(page), block);
  const size_t os_page_size = _mi_os_page_size();
  const size_t block_size = mi_page_block_size(page);

  // the block must be os-page aligned (ensured by the page allocator for page-sized blocks) and
  // leave a full tag page, a guard page and room for the object.
  mi_assert_internal(mi_is_aligned(block, os_page_size));
  mi_assert_internal(block_size >= obj_size + 2 * os_page_size);
  if mi_unlikely(!mi_is_aligned(block, os_page_size) || block_size < obj_size + 2 * os_page_size) {
    _mi_error_message(ENOMEM, "unable to place a front guard page for an object of size %zu\n", obj_size);
    mi_free(block);
    return NULL;
  }

  uint8_t* const guard_page = (uint8_t*)mi_page_vmem_ptr(page, (uint8_t*)block + os_page_size);
  mi_assert_internal(mi_is_aligned(guard_page, os_page_size));

  // a front guard cannot be established on pinned memory; fail rather than hand out an unprotected object
  if mi_unlikely(page->memid.is_pinned || !mi_is_aligned(guard_page, os_page_size)) {
    _mi_error_message(ENOMEM, "unable to set a guard page in front of an object due to pinned memory (large OS pages?) (object %p of size %zu)\n", block, block_size);
    mi_free(block);
    return NULL;
  }

  if mi_unlikely(!_mi_os_guard(guard_page, os_page_size, true /* committed */)) {
    _mi_error_message(ENOMEM, "failed to set a guard page in front of an object (object %p of size %zu)\n", block, block_size);
    mi_free(block);
    return NULL;
  }

  // protection established: only now record the tag and interior layout
  mi_page_set_has_interior_pointers(page, true);
  block->next = MI_BLOCK_TAG_GUARDED_FRONT;  // tag in the readable leading page

  uint8_t* const p = (uint8_t*)block + 2 * os_page_size;  // directly after the guard page, os-page aligned
  const size_t usable = mi_guarded_usable_size(block_size, block);
  mi_assert_internal(usable >= obj_size);
  if (usable_size != NULL) { *usable_size = usable; mi_assert_internal(mi_usable_size(p)==usable); }
  mi_track_align(block, p, (size_t)(p - (uint8_t*)block), obj_size);
  mi_track_mem_defined(block, sizeof(mi_block_t));
  return p;
}

// Allocate a block with a guard page behind (or in front of) it.
mi_decl_restrict void* _mi_theap_malloc_guarded(mi_theap_t* theap, size_t size, bool zero, mi_page_t** ppage) mi_attr_noexcept
{
  // allocate multiple of page size ending in a guard page
  // ensure minimal alignment requirement?
  if mi_unlikely(size >= MI_MAX_ALLOC_SIZE - MI_PADDING_SIZE) {  // check up front so the `req_size` won't overflow
    _mi_error_message(EOVERFLOW, "(guarded) allocation request is too large (%zu bytes)\n", size);
    return NULL;
  }
  const size_t os_page_size = _mi_os_page_size();
  const size_t obj_size = (mi_option_is_enabled(mi_option_unsafe_guarded_precise) ? size : mi_align_up(size, MI_MAX_ALIGN_SIZE));
  const bool guard_before = theap->guarded_guard_before;

  size_t req_size;
  if (guard_before) {
    // [tag page][guard page][user pages]; guard against overflow when rounding and adding the pages
    const size_t user_pages = mi_align_up(obj_size, os_page_size);
    if mi_unlikely(user_pages > MI_MAX_ALLOC_SIZE - 2 * os_page_size) {
      _mi_error_message(EOVERFLOW, "(guarded) allocation request is too large (%zu bytes)\n", size);
      return NULL;
    }
    req_size = user_pages + 2 * os_page_size;  // all os-page multiples, so blocks stay os-page aligned
  }
  else {
    const size_t bsize    = mi_align_up(mi_align_up(obj_size, MI_MAX_ALIGN_SIZE) + sizeof(mi_block_t), MI_MAX_ALIGN_SIZE);
    if mi_unlikely(bsize > MI_MAX_ALLOC_SIZE - os_page_size) {
      _mi_error_message(EOVERFLOW, "(guarded) allocation request is too large (%zu bytes)\n", size);
      return NULL;
    }
    req_size = mi_align_up(bsize + os_page_size, os_page_size);
  }

  // const size_t threshold = mi_theap_disable_profiler(theap);
  mi_block_t* const block = (mi_block_t*)_mi_malloc_generic_no_sample(theap, req_size, false /* don't zero */, ppage);
  // mi_theap_enable_profiler(theap,threshold);
  if (block==NULL) return NULL;
  size_t usable_size = 0;
  void* const p = (guard_before ? mi_block_ptr_set_guarded_before(block, obj_size, &usable_size)
                                : mi_block_ptr_set_guarded_after(block, obj_size, &usable_size));
  if (p == NULL) return NULL;
  if (zero) {
    mi_memzero(p,obj_size);  // we have to zero afterwards as padding might have written inside the block (if the `blocksize > reqsize + os_page_size`)
  }

  // stats
  mi_track_malloc(p, usable_size, zero);
  if (!mi_theap_is_initialized(theap)) { theap = _mi_theap_default(); }
  mi_theap_stat_counter_increase(theap, malloc_guarded_count, 1);
  #if MI_STATS
  // adjust request stats to only count the allocated size of the block (and not the guard page)
  mi_theap_stat_counter_decrease(theap, malloc_requested, req_size);
  mi_theap_stat_counter_increase(theap, malloc_requested, size);
  #endif
  #if MI_DEBUG>3
  if (zero) {
    mi_assert_expensive(mi_mem_is_zero(p, size));
  }
  #endif
  #if MI_PAGE_META_SMALL_IS_ALIGNED && MI_DEBUG>=2
  // we should never allocate something allocated as small in a non-small page or otherwise aligned mi_free_small may fail.
  if (size <= MI_SMALL_SIZE_MAX) {
    mi_page_t* const page = mi_ptr_page(p);
    mi_assert_internal(mi_page_block_size(page) <= MI_SMALL_MAX_OBJ_SIZE);
  }
  #endif
  return mi_ptr_bounded_data(p, size - MI_PADDING_SIZE);
}


// Remove guard page when building with MI_GUARDED
void _mi_page_block_unguard(mi_page_t* page, mi_block_t* block, void* p) {
  // if (!mi_block_ptr_is_guarded(block,p)) return;
  MI_UNUSED_RELEASE(p);
  mi_assert_internal(mi_block_ptr_is_guarded(block, p));
  mi_assert_internal(mi_page_has_interior_pointers(page));
  mi_assert_internal((uint8_t*)p - (uint8_t*)block >= (ptrdiff_t)sizeof(mi_block_t));
  mi_assert_internal(mi_block_tag_is_guarded(block->next));

  const size_t bsize = mi_page_block_size(page);
  const size_t psize = _mi_os_page_size();
  if (page->memid.is_pinned) return;  // no guard page was set (see the guarded setters)
  void* gpage;
  if (mi_block_tag_is_guarded_front(block)) {
    // front layout: guard page sits between the readable tag page and the user pointer
    mi_assert_internal(bsize > 2 * psize);
    gpage = mi_page_vmem_ptr(page, (uint8_t*)block + psize);
  }
  else {
    mi_assert_internal(bsize > psize);
    gpage = mi_page_vmem_ptr(page, (uint8_t*)block + bsize - psize);
  }
  mi_assert_internal(mi_is_aligned(gpage, psize));
  _mi_os_unguard(gpage, psize, true /* committed */);
}

// unguard a whole page (called from `mi_heap_destroy`)
void _mi_page_unguard_all(mi_page_t* page) {
  if mi_likely(!mi_page_has_interior_pointers(page) || page->memid.is_pinned) return;
  uint8_t* const start = (uint8_t*)mi_page_vmem_ptr(page, mi_page_start(page));
  const size_t psize = mi_page_committed(page);
  _mi_os_unguard(start, psize, true /* committed */);  // unguard all at once as we cannot know which blocks are guarded
}

#else

void _mi_page_block_unguard(mi_page_t* page, mi_block_t* block, void* p) {
  MI_UNUSED(page); MI_UNUSED(block); MI_UNUSED(p);
}
void _mi_page_unguard_all(mi_page_t* page) {
  MI_UNUSED(page);
}
#endif
