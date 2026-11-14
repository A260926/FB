/* Free list guard mapping failure contract.
 *
 * The shadow records live in their own mapping. If that mapping cannot be reserved the span that
 * needs it must fail its allocation as well: rpmalloc must return a null pointer and leave no free
 * list running without shadow protection, rather than half protecting the allocator.
 *
 * This installs a minimal POSIX memory interface and makes the map call fail for the large,
 * unaligned shadow mapping (span and huge block mappings request span sized alignment; the small
 * heap control mappings are far below the size threshold). Once the failure is cleared the
 * allocator must recover and run normally without tripping the guard.
 *
 * SPDX-FileCopyrightText: 2026 Mattias Jansson
 * SPDX-License-Identifier: Unlicense OR MIT
 */

#include "rpmalloc.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <string.h>
#include <stddef.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

//! Fail the shadow mapping while non-zero. The shadow is the only large mapping requested with no
//  alignment (span mappings carry span sized alignment, heap control mappings are page sized)
static int g_fail_large_unaligned;

static void*
test_memory_map(size_t size, size_t alignment, size_t* offset, size_t* mapped_size) {
	(void)alignment;
	if (g_fail_large_unaligned && (alignment == 0) && (size >= (64u * 1024u * 1024u)))
		return 0;
	size_t map_size = size + alignment;
	void* ptr = mmap(0, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ptr == MAP_FAILED)
		return 0;
	if (alignment) {
		size_t padding = ((uintptr_t)ptr & (alignment - 1));
		if (padding)
			padding = alignment - padding;
		if (padding)
			munmap(ptr, padding);
		ptr = (char*)ptr + padding;
		if (alignment - padding)
			munmap((char*)ptr + size, alignment - padding);
		*offset = 0;
		map_size = size;
	}
	if (offset)
		*offset = 0;
	if (mapped_size)
		*mapped_size = map_size;
	return ptr;
}

static int
test_memory_commit(void* address, size_t size) {
	(void)address;
	(void)size;
	return 0;  // mmap gives committed memory
}

static int
test_memory_decommit(void* address, size_t size) {
	(void)address;
	(void)size;
	return 0;
}

static void
test_memory_unmap(void* address, size_t offset, size_t mapped_size) {
	(void)offset;
	munmap((char*)address - (ptrdiff_t)offset, mapped_size);
}

static void
error_callback(const char* message) {
	fprintf(stderr, "unexpected error callback: %s\n", message ? message : "(null)");
}

int
main(void) {
	rpmalloc_interface_t interface;
	memset(&interface, 0, sizeof(interface));
	interface.memory_map = test_memory_map;
	interface.memory_commit = test_memory_commit;
	interface.memory_decommit = test_memory_decommit;
	interface.memory_unmap = test_memory_unmap;
	interface.error_callback = error_callback;

	if (rpmalloc_initialize(&interface)) {
		printf("mapfail: initialize failed\n");
		return 1;
	}

	// With the shadow mapping refused, a block allocation must fail cleanly with a null pointer
	g_fail_large_unaligned = 1;
	void* p = rpmalloc(64);
	if (p != 0) {
		printf("mapfail: allocation did not fail when the shadow mapping failed\n");
		return 1;
	}

	// Recovery: once the shadow can be mapped the allocator works and never raises the guard
	g_fail_large_unaligned = 0;
	for (int round = 0; round < 8; ++round) {
		enum { N = 512 };
		void* blocks[N];
		for (int i = 0; i < N; ++i) {
			blocks[i] = rpmalloc((size_t)(16 + (i * 29) % 180));
			if (!blocks[i]) {
				printf("mapfail: recovery allocation failed\n");
				return 1;
			}
			memset(blocks[i], i & 0xFF, 16);
		}
		for (int i = 0; i < N; ++i)
			rpfree(blocks[i]);
		rpmalloc_thread_collect();
	}

	void* a = rpmalloc(128);
	void* b = rpmalloc(128);
	if (!a || !b) {
		printf("mapfail: post recovery allocation failed\n");
		return 1;
	}
	rpfree(a);
	rpfree(b);

	rpmalloc_thread_finalize();
	rpmalloc_finalize();
	printf("mapfail: PASS (shadow map failure fails the allocation, recovery is clean)\n");
	return 0;
}
