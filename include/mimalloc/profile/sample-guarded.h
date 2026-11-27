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
#include "sample-profile.h"

void          _mi_page_unguard_all(mi_page_t* page);
mi_decl_restrict void* _mi_theap_malloc_guarded(mi_theap_t* theap, size_t size, bool zero, mi_page_t** ppage) mi_attr_noexcept;
void          _mi_page_block_unguard(mi_page_t* page, mi_block_t* block, void* p);
void          _mi_theap_guarded_init(mi_theap_t* theap);

// A guarded block stores its tag in the first word of the underlying heap block.
// The tag records the direction of the guard page so a free can unprotect the right
// page without consulting the freeing thread's current theap configuration.
#define MI_BLOCK_TAG_GUARDED         (~MI_BLOCK_TAG_ALIGNED)            // guard page behind the object (MI_GUARD_AFTER)
#define MI_BLOCK_TAG_GUARDED_BEFORE  (MI_BLOCK_TAG_GUARDED - 1)         // guard page before the object (MI_GUARD_BEFORE)

static inline bool mi_block_tag_is_guarded(mi_encoded_t tag) {
  return (tag == MI_BLOCK_TAG_GUARDED || tag == MI_BLOCK_TAG_GUARDED_BEFORE);
}

static inline bool mi_block_tag_guard_before(mi_encoded_t tag) {
  return (tag == MI_BLOCK_TAG_GUARDED_BEFORE);
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

#endif
