/* Test only observation seams for the free list guard.
 *
 * SPDX-FileCopyrightText: 2026 Mattias Jansson
 * SPDX-License-Identifier: Unlicense OR MIT
 */

#pragma once

#include <stdint.h>

//! Sentinel returned for a block with no shadow record (guard disabled or a huge block span)
#define RPMALLOC_TEST_SHADOW_NA ((uintptr_t)0xDEADBEEFDEADBEEFULL)

//! Returns 1 when ENABLE_FREELIST_GUARD is compiled in, 0 otherwise
int
rpmalloc_test_guard_enabled(void);

//! Return the trusted shadow chain word recorded for a block, or RPMALLOC_TEST_SHADOW_NA
uintptr_t
rpmalloc_test_shadow_expected(const void* ptr);

//! Return non-zero if the page holding the block is currently flagged full
int
rpmalloc_test_block_page_full(const void* ptr);
