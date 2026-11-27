/* ----------------------------------------------------------------------------
Copyright (c) 2019-2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "mimalloc.h"
#include "mimalloc/prim/prim-tls.h"
#include "mimalloc/util/stats.h"
#include "mimalloc/util/options.h"
#include "mimalloc/alloc/page.h"
#include "mimalloc/heap/theap.h"
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

// Select the guard page direction for guarded objects later sampled by this theap.
// Only effective when building with MI_GUARDED; otherwise the setting is stored but ignored.
mi_decl_export void mi_theap_guarded_set_direction(mi_theap_t* theap, mi_guard_direction_t direction) mi_attr_noexcept {
  if mi_unlikely(theap == NULL) {
    _mi_error_message(EINVAL, "cannot set the guarded direction on a null theap\n");
    return;
  }
  if mi_unlikely(direction != MI_GUARD_AFTER && direction != MI_GUARD_BEFORE) {
    _mi_error_message(EINVAL, "invalid guarded direction (%d); leaving the direction unchanged\n", (int)direction);
    return;
  }
  theap->guarded_direction = direction;
}

void _mi_theap_guarded_init(mi_theap_t* theap) {
  theap->guarded_direction = MI_GUARD_AFTER;  // new theaps keep the existing tail-guard behavior by default
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
// Offset of the inaccessible page inside a front-guard block, and of the first user byte.
// A front-guard block (os-page aligned) is laid out as:
//   [ header page ][ guard page ][ user bytes ... ]
// the tag stays in the readable header (so a free can locate the block and its direction safely),
// and the user pointer starts immediately after the guard page. Everything is derivable from the
// block start and the os page size, so no per-object out-of-line records are needed and a free from
// any thread (or after thread exit / heap destroy) unprotects the same page that was protected.
static inline size_t mi_guard_before_guard_offset(size_t os_page_size) {
  return os_page_size;
}
static inline size_t mi_guard_before_user_offset(size_t os_page_size) {
  return 2 * os_page_size;
}
#endif


// We always allocate a guarded allocation at an offset (`mi_page_has_interior_pointers` will be true).
// We then set the first word of the block to `0` for regular offset aligned allocations (in `alloc/alloc-aligned.c`)
// and the first word to a guarded tag for guarded allocations to have a correct `mi_usable_size`
static void* mi_block_ptr_set_guarded(mi_block_t* block, size_t obj_size, size_t* usable_size) {
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

#if MI_GUARDED
// Set a guard page immediately before the first user byte.
// Layout (the os-page-aligned block): [header page][guard page][user bytes ...].
// The header keeps the `MI_BLOCK_TAG_GUARDED_BEFORE` tag and stays readable; the user pointer
// is the first byte after the guard page so both `p[-1]` reads and writes fault.
// If the protection cannot be established, this reports an error and fails the allocation;
// it never returns writable memory claimed to be guarded.
static void* mi_block_ptr_set_guarded_before(mi_block_t* block, size_t obj_size, size_t* usable_size) {
  mi_page_t* const page0 = mi_ptr_page(block);
  if mi_unlikely(page0 == NULL) return NULL;
  mi_page_t* const page = page0;
  block = (mi_block_t*)mi_ptr_rederive(mi_page_start(page), block);

  const size_t block_size   = mi_page_block_size(page);  // must use `block_size` to match `mi_free_local`
  const size_t os_page_size = _mi_os_page_size();
  // blocks in a MI_GUARDED build with a block size that is a multiple of the os page size are os-page aligned
  // (see `arena-page.c:mi_page_meta_in_front_block_start`)
  mi_assert_internal(mi_is_aligned(block, os_page_size));
  const size_t guard_offset = mi_guard_before_guard_offset(os_page_size);
  const size_t user_offset  = mi_guard_before_user_offset(os_page_size);
  mi_assert_internal(block_size >= user_offset + obj_size);
  if mi_unlikely(!mi_is_aligned(block, os_page_size) || block_size < user_offset + obj_size) {
    _mi_error_message(ENOMEM, "unable to set a guard page in front of an object (object %p of size %zu)\n", block, block_size);
    mi_free(block);  // should never happen
    return NULL;
  }
  if mi_unlikely(page->memid.is_pinned) {
    // pinned memory (e.g. huge OS pages) cannot be protected; do not hand out an unprotected "guarded" object
    _mi_error_message(ENOMEM, "unable to set a guard page in front of an object due to pinned memory (large OS pages?) (object %p of size %zu)\n", block, block_size);
    mi_free(block);
    return NULL;
  }

  void* const guard_page = mi_page_vmem_ptr(page, (uint8_t*)block + guard_offset);
  mi_assert_internal(mi_is_aligned(guard_page, os_page_size));
  if mi_unlikely(!_mi_os_guard(guard_page, os_page_size, true /* committed */)) {
    _mi_error_message(ENOMEM, "failed to set a guard page in front of an object (object %p of size %zu)\n", block, block_size);
    mi_free(block);
    return NULL;
  }

  mi_page_set_has_interior_pointers(page, true);
  block->next = MI_BLOCK_TAG_GUARDED_BEFORE;  // tag is in the readable header page, never on the guard page

  uint8_t* const p = (uint8_t*)block + user_offset;  // first user byte, immediately after the inaccessible page
  mi_assert_internal(mi_is_aligned(p, MI_MAX_ALIGN_SIZE));  // minimal alignment is preserved
  if (usable_size != NULL) {
    *usable_size = (block_size - user_offset);  // neither the header nor the guard page is usable space
    mi_assert_internal(mi_usable_size(p) == *usable_size);
  }
  mi_track_align(block, p, user_offset, obj_size);
  mi_track_mem_defined(block, sizeof(mi_block_t));
  return p;
}
#endif

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
  const size_t bsize    = mi_align_up(mi_align_up(obj_size, MI_MAX_ALIGN_SIZE) + sizeof(mi_block_t), MI_MAX_ALIGN_SIZE);
  #if MI_GUARDED
  const bool guard_before = (theap->guarded_direction == MI_GUARD_BEFORE);
  #else
  const bool guard_before = false;
  #endif
  size_t req_size;
  if (guard_before) {
    // header page + guard page in front of the object bytes; reject on overflow instead of wrapping
    if mi_unlikely(bsize > MI_MAX_ALLOC_SIZE - 2 * os_page_size) {
      _mi_error_message(EOVERFLOW, "(guarded) allocation request is too large (%zu bytes)\n", size);
      return NULL;
    }
    req_size = mi_align_up(2 * os_page_size + bsize, os_page_size);
  }
  else {
    req_size = mi_align_up(bsize + os_page_size, os_page_size);
  }
  // const size_t threshold = mi_theap_disable_profiler(theap);
  mi_block_t* const block = (mi_block_t*)_mi_malloc_generic_no_sample(theap, req_size, false /* don't zero */, ppage);
  // mi_theap_enable_profiler(theap,threshold);
  if (block==NULL) return NULL;
  size_t usable_size = 0;
  void* p;
  #if MI_GUARDED
  if (guard_before) {
    p = mi_block_ptr_set_guarded_before(block, obj_size, &usable_size);
  }
  else
  #endif
  {
    p = mi_block_ptr_set_guarded(block, obj_size, &usable_size);
  }
  if (p == NULL) return NULL;
  if (zero) {
    // we have to zero afterwards as padding might have written inside the block (if the `blocksize > reqsize + os_page_size`)
    #if MI_GUARDED
    if (guard_before) {
      // zero the whole user region (up to the block end) so a later realloc/rezalloc never observes
      // stale bytes beyond the requested size. The inaccessible guard page and the readable header
      // page both lie below `p` and are never touched.
      const size_t user_offset = mi_guard_before_user_offset(os_page_size);
      const size_t zero_size  = mi_page_block_size(mi_ptr_page(block)) - user_offset;
      mi_assert_internal(zero_size >= obj_size);
      mi_memzero(p, zero_size);
    }
    else
    #endif
    {
      mi_memzero(p,obj_size);
    }
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
#if MI_GUARDED
void _mi_page_block_unguard(mi_page_t* page, mi_block_t* block, void* p) {
  MI_UNUSED_RELEASE(p);
  mi_assert_internal(mi_block_ptr_is_guarded(block, p));
  mi_assert_internal(mi_page_has_interior_pointers(page));
  mi_assert_internal((uint8_t*)p - (uint8_t*)block >= (ptrdiff_t)sizeof(mi_block_t));
  mi_assert_internal(mi_block_tag_is_guarded(block->next));

  const size_t psize = _mi_os_page_size();
  if (mi_block_tag_guard_before(block->next)) {
    // the guard page sits at a fixed offset inside the block; unprotect the page recorded by the object's tag,
    // never the page implied by the freeing thread's current configuration
    mi_assert_internal(mi_is_aligned(block, psize));
    if (page->memid.is_pinned) return;  // no guard page was set (see `mi_block_ptr_set_guarded_before`)
    void* const gpage = mi_page_vmem_ptr(page, (uint8_t*)block + mi_guard_before_guard_offset(psize));
    mi_assert_internal(mi_is_aligned(gpage, psize));
    _mi_os_unguard(gpage, psize, true /* committed */);
    return;
  }

  const size_t bsize = mi_page_block_size(page);
  mi_assert_internal(bsize > psize);
  if (page->memid.is_pinned) return;  // no guard page was set (see `mi_block_ptr_set_guarded`)
  void* gpage = mi_page_vmem_ptr(page, (uint8_t*)block + bsize - psize);
  mi_assert_internal(mi_is_aligned(gpage, psize));
  _mi_os_unguard(gpage, psize, true /* committed */);
}

// unguard a whole page (called from `mi_heap_destroy`)
void _mi_page_unguard_all(mi_page_t* page) {
  if mi_likely(!mi_page_has_interior_pointers(page) || page->memid.is_pinned) return;
  // unguard all at once as we cannot know which blocks are guarded; this covers both the tail
  // guard pages and the interior front guard pages of every block in the page.
  uint8_t* const start = (uint8_t*)mi_page_vmem_ptr(page, mi_page_start(page));
  const size_t psize = mi_page_committed(page);
  _mi_os_unguard(start, psize, true /* committed */);
}
#else
void _mi_page_block_unguard(mi_page_t* page, mi_block_t* block, void* p) {
  MI_UNUSED(page); MI_UNUSED(block); MI_UNUSED(p);
}
void _mi_page_unguard_all(mi_page_t* page) {
  MI_UNUSED(page);
}
#endif
