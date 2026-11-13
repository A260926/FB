/* ----------------------------------------------------------------------------
Copyright (c) 2018-2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef MI_INTERNAL_SAMPLE_GUARDED_H
#define MI_INTERNAL_SAMPLE_GUARDED_H

#include "../types.h"
#include "../prim/os.h"
#include "sample-profile.h"

void          _mi_page_unguard_all(mi_page_t* page);
mi_decl_restrict void* _mi_theap_malloc_guarded(mi_theap_t* theap, size_t size, bool zero, mi_page_t** ppage) mi_attr_noexcept;
void          _mi_page_block_unguard(mi_page_t* page, mi_block_t* block, void* p);
void          _mi_theap_guarded_init(mi_theap_t* theap);

// A guarded block always has its user pointer at an interior offset. The block `next`
// field is used as a tag to distinguish the kind of sampled/aligned block.
// `MI_BLOCK_TAG_GUARDED`       : guard page directly after the object (overflow detection).
// `MI_BLOCK_TAG_GUARDED_FRONT` : guard page directly before the object (underflow detection).
#define MI_BLOCK_TAG_GUARDED        (~MI_BLOCK_TAG_ALIGNED)
#define MI_BLOCK_TAG_GUARDED_FRONT  ((mi_encoded_t)2)

static inline bool mi_block_tag_is_guarded(mi_encoded_t tag) {
  return (tag == MI_BLOCK_TAG_GUARDED || tag == MI_BLOCK_TAG_GUARDED_FRONT);
}

// `true` if the block carries a guard page in front of its object (instead of behind it).
static inline bool mi_block_tag_is_guarded_front(const mi_block_t* block) {
#if MI_GUARDED
  return (block->next == MI_BLOCK_TAG_GUARDED_FRONT);
#else
  MI_UNUSED(block);
  return false;
#endif
}

static inline bool mi_block_ptr_is_guarded(const mi_block_t* block, const void* p) {
#if MI_GUARDED
  const ptrdiff_t offset = (uint8_t*)p - (uint8_t*)block;
  return (offset >= (ptrdiff_t)(sizeof(mi_block_t)) && mi_block_tag_is_guarded(block->next));
#else
  MI_UNUSED(block); MI_UNUSED(p);
  return false;
#endif
}

// Usable bytes of a known guarded block, excluding its guard page and in-block allocator area.
// Reads the block's own tag so the result is independent of the freeing thread's current direction.
// `block_size` is the page's block size (`mi_page_block_size`).
static inline size_t mi_guarded_usable_size(size_t block_size, const mi_block_t* block) {
#if MI_GUARDED
  const size_t psize = _mi_os_page_size();
  if (mi_block_tag_is_guarded_front(block)) {
    // front layout: [tag page][guard page][user bytes ...]; both leading pages are not usable
    return (block_size - 2 * psize);
  }
  return (block_size - psize);  // after layout: [user bytes ...][guard page]
#else
  MI_UNUSED(block_size); MI_UNUSED(block);
  return 0;
#endif
}

#endif
