/* rpmalloc_guard_test.h - Test only observation seams for the free list guard
 *
 * SPDX-FileCopyrightText: 2026 Mattias Jansson
 * SPDX-License-Identifier: Unlicense OR MIT
 *
 * These functions exist only in test builds compiled with both ENABLE_FREELIST_GUARD=1 and
 * RPMALLOC_GUARD_TEST_SEAMS=1. They expose read only views and controlled corruption of the free
 * list link words so the guard contract can be driven through the real allocation and collection
 * paths. Production builds define neither macro and contain no reference to this header.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Non-zero when the library was built with ENABLE_FREELIST_GUARD enabled
int
rpmalloc_guard_is_enabled(void);

//! Return the current in-block free list link word of a free block, the word the guard validates.
//  The block must be a currently free, still mapped block start obtained from a real allocation and
//  free, never an arbitrary pointer.
void*
rpmalloc_guard_read_link(void* block);

//! Overwrite the in-block free list link word of a free block with the given value, simulating a write
//  after free while the mapping stays in place. Only the link word is touched, the shadow record is left
//  intact, so the next real chain consumer must detect the mismatch. The block must be a currently free,
//  still mapped block start.
void
rpmalloc_guard_corrupt_link(void* block, void* forged);

//! Copy the link word of another free block into the given block, simulating a write after free that
//  reproduces a valid-looking but wrong pointer. Both blocks must be currently free, still mapped block
//  starts.
void
rpmalloc_guard_copy_link(void* block, void* other);

#ifdef __cplusplus
}
#endif
