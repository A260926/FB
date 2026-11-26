/* freelist_guard_contract.c - Contract test for ENABLE_FREELIST_GUARD
 *
 * SPDX-FileCopyrightText: 2026 Mattias Jansson
 * SPDX-License-Identifier: Unlicense OR MIT
 *
 * Two groups:
 *  - normal group: real small block batch allocation, alignment and realloc, with remote frees, full
 *    page collection and thread exit coordinated through a barrier and join, then content and reuse checks.
 *  - corruption group: each case runs in a forked child that keeps the mapping, overwrites a free block link
 *    word (through the test only observation seam when the guard is built in, otherwise with a plain write)
 *    and triggers a real allocation or collection path. Each guarded case must print the FREELIST_CORRUPTION
 *    diagnostic and end in SIGABRT, never SIGSEGV, a timeout or a forged pointer. The switch off control must
 *    neither detect nor abort.
 *
 * The observation seams only expose the in-block link word; every trigger still runs the production chain
 * consumers.
 */

#include "rpmalloc.h"
#if ENABLE_FREELIST_GUARD
#include "rpmalloc_guard_test.h"
#endif

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ENABLE_FREELIST_GUARD
#define ENABLE_FREELIST_GUARD 0
#endif

#define GUARD_PREFIX "FREELIST_CORRUPTION"
#define SMALL_SIZE 16
#define PAGE64_SIZE 64

static int callback_invoked;

static void
error_callback(const char* message) {
	callback_invoked = 1;
	// Emit the fixed prefix so the parent can assert the diagnostic was delivered before abort
	fprintf(stderr, "%s\n", message);
}

#if ENABLE_FREELIST_GUARD
static void*
unmapped_address(void) {
	size_t size = (size_t)getpagesize();
	void* address = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (address == MAP_FAILED)
		return (void*)0xdead0000;
	munmap(address, size);
	return address;
}
#endif

#if ENABLE_FREELIST_GUARD
static void
corrupt_link(void* block, void* forged) {
	rpmalloc_guard_corrupt_link(block, forged);
}
static void
copy_link(void* block, void* other) {
	rpmalloc_guard_copy_link(block, other);
}
#else
static void
corrupt_link(void* block, void* forged) {
	((void**)block)[0] = forged;
}
#endif

static int
fail(const char* what) {
	fprintf(stderr, "FAIL: %s\n", what);
	return 1;
}

////////////
/// Normal group
//////

typedef struct normal_arg_t {
	atomic_int phase;
	void** remote_blocks;
	size_t remote_count;
	void** owned_blocks;
	size_t owned_count;
} normal_arg_t;

static void
wait_phase(atomic_int* phase, int wanted) {
	while (atomic_load(phase) != wanted)
		sched_yield();
}

static void*
normal_worker(void* argp) {
	normal_arg_t* arg = argp;
	rpmalloc_thread_initialize();

	// Remote free into the owner thread pages, coordinated with the owner through the atomic phase handshake
	wait_phase(&arg->phase, 1);
	for (size_t i = 0; i < arg->remote_count; ++i)
		rpfree(arg->remote_blocks[i]);
	atomic_store(&arg->phase, 2);

	// Blocks this thread allocates and keeps alive until exit, exercising thread heap takeover on join
	for (size_t i = 0; i < arg->owned_count; ++i) {
		arg->owned_blocks[i] = rpmalloc(128);
		if (!arg->owned_blocks[i])
			return (void*)1;
		memset(arg->owned_blocks[i], (int)(0x40 + i), 128);
	}
	rpmalloc_thread_finalize();
	return 0;
}

static int
normal_group(void) {
	enum { BATCH = 512, REMOTE = 128, WORKER_OWNED = 64 };
	unsigned char* batch[BATCH];
	for (int i = 0; i < BATCH; ++i) {
		batch[i] = rpmalloc(SMALL_SIZE);
		if (!batch[i])
			return fail("small batch allocation");
		memset(batch[i], (unsigned char)(i & 0xff), SMALL_SIZE);
	}
	for (int i = 0; i < BATCH; ++i) {
		if (batch[i][0] != (unsigned char)(i & 0xff))
			return fail("live small block content overwritten");
	}

	void* aligned = rpaligned_alloc(256, 1024);
	if (!aligned || ((uintptr_t)aligned % 256))
		return fail("aligned allocation");
	memset(aligned, 0xa5, 1024);

	// realloc must preserve the payload
	unsigned char* grown = rprealloc(batch[0], 4096);
	if (!grown)
		return fail("realloc");
	if (grown[0] != (unsigned char)(0 & 0xff))
		return fail("realloc did not preserve content");

	// Arrange remote frees and thread exit with a barrier and join
	static void* remote[REMOTE];
	for (int i = 0; i < REMOTE; ++i)
		remote[i] = rpmalloc(PAGE64_SIZE);
	normal_arg_t arg;
	memset(&arg, 0, sizeof(arg));
	arg.remote_blocks = remote;
	arg.remote_count = REMOTE;
	static void* worker_owned[WORKER_OWNED];
	arg.owned_blocks = worker_owned;
	arg.owned_count = WORKER_OWNED;
	pthread_t thread;
	if (pthread_create(&thread, 0, normal_worker, &arg))
		return fail("thread create");
	atomic_store(&arg.phase, 1);
	wait_phase(&arg.phase, 2);
	pthread_join(thread, 0);

	// Collect the remote frees (page and heap remote lists) and anything the exiting thread left
	rpmalloc_thread_collect();
	for (int i = 0; i < WORKER_OWNED; ++i) {
		unsigned char expected = (unsigned char)(0x40 + i);
		if (!worker_owned[i] || ((unsigned char*)worker_owned[i])[0] != expected)
			return fail("worker block content after join");
		rpfree(worker_owned[i]);
	}

	// Full page collection: exhaust small pages and drive them through the real free and collect paths
	enum { FULL = 5000 };
	static void* full[FULL];
	for (int i = 0; i < FULL; ++i) {
		full[i] = rpmalloc(SMALL_SIZE);
		if (!full[i])
			return fail("full page allocation");
	}
	for (int i = 0; i < FULL; ++i)
		rpfree(full[i]);
	rpmalloc_thread_collect();

	// Normal reuse after the free storm: same class allocations must succeed and stay readable
	for (int i = 0; i < FULL; ++i) {
		full[i] = rpmalloc(SMALL_SIZE);
		if (!full[i])
			return fail("reuse after collection");
		memset(full[i], 0x77, SMALL_SIZE);
	}
	for (int i = 0; i < FULL; ++i)
		rpfree(full[i]);
	for (int i = 1; i < BATCH; ++i)
		rpfree(batch[i]);
	rpfree(grown);
	rpfree(aligned);
	rpmalloc_thread_collect();

	if (callback_invoked)
		return fail("error callback fired on an uncorrupted free list");
	printf("normal group: batch/align/realloc/remote/full-page/thread-exit OK\n");
	return 0;
}

////////////
/// Corruption group
//////

struct child_result {
	int signaled;
	int signal;
	int normal_exit;
	int had_prefix;
};

//! Run a child body in a forked process, capturing stderr and the termination status. require_prefix asks for
//! the FREELIST_CORRUPTION diagnostic, which only a configured error callback can print.
static int
run_child(const char* name, void (*body)(void), int require_prefix) {
	int pipefd[2];
	if (pipe(pipefd))
		return fail("pipe");
	// Drop buffered parent output so the forked child does not inherit and reprint it
	fflush(stdout);
	fflush(stderr);
	pid_t pid = fork();
	if (pid < 0)
		return fail("fork");
	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		alarm(20);
		body();
		// A body that returns did not abort, which is only expected for the switch off control
		_exit(0);
	}
	close(pipefd[1]);
	char buffer[4096];
	size_t length = 0;
	for (;;) {
		ssize_t got = read(pipefd[0], buffer + length, sizeof(buffer) - length - 1);
		if (got <= 0)
			break;
		length += (size_t)got;
		if (length >= sizeof(buffer) - 1)
			break;
	}
	buffer[length] = 0;
	close(pipefd[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	(void)require_prefix;
	struct child_result result = {0};
	result.signaled = WIFSIGNALED(status);
	result.signal = result.signaled ? WTERMSIG(status) : 0;
	result.normal_exit = WIFEXITED(status);
	result.had_prefix = strstr(buffer, GUARD_PREFIX) != 0;
	printf("  %-28s signal=%d exit=%d prefix=%d\n", name, result.signal, result.normal_exit, result.had_prefix);
#if ENABLE_FREELIST_GUARD
	if (!result.signaled || (result.signal != SIGABRT))
		return fail("guarded corruption did not abort with SIGABRT");
	if (require_prefix && !result.had_prefix)
		return fail("guarded corruption did not report the FREELIST_CORRUPTION diagnostic");
#else
	if (result.signaled || result.had_prefix)
		return fail("switch off control reported or aborted");
	if (!result.normal_exit)
		return fail("switch off control did not exit normally");
#endif
	return 0;
}

//! Build a two block local free chain B -> A and return both block starts
static void
build_local_chain(void** a, void** b) {
	*a = rpmalloc(SMALL_SIZE);
	*b = rpmalloc(SMALL_SIZE);
	rpfree(*a);
	rpfree(*b);  // b is the local free list head, b -> a
}

#if ENABLE_FREELIST_GUARD

static void
child_local_unmapped(void) {
	void *a, *b;
	build_local_chain(&a, &b);
	corrupt_link(a, unmapped_address());
	for (int i = 0; i < 100000; ++i) {
		void* p = rpmalloc(SMALL_SIZE);
		if (!p)
			_exit(2);
	}
}

static void
child_local_copy(void) {
	void *a, *b, *c, *d;
	build_local_chain(&a, &b);
	c = rpmalloc(SMALL_SIZE);
	d = rpmalloc(SMALL_SIZE);
	rpfree(c);
	rpfree(d);  // d -> c is a separate valid link word
	copy_link(a, d);
	for (int i = 0; i < 100000; ++i) {
		if (!rpmalloc(SMALL_SIZE))
			_exit(2);
	}
}

static void
child_no_callback(void) {
	rpmalloc_finalize();
	rpmalloc_interface_t iface = {0};
	if (rpmalloc_initialize(&iface))
		_exit(3);
	void *a, *b;
	build_local_chain(&a, &b);
	corrupt_link(a, unmapped_address());
	for (int i = 0; i < 100000; ++i) {
		if (!rpmalloc(SMALL_SIZE))
			_exit(2);
	}
}

//! Allocate a pool and expose a subset for a worker thread to free cross thread into the non full page
typedef struct remote_arg_t {
	void** blocks;
	size_t count;
} remote_arg_t;

static void*
remote_free_worker(void* argp) {
	remote_arg_t* arg = argp;
	rpmalloc_thread_initialize();
	for (size_t i = 0; i < arg->count; ++i)
		rpfree(arg->blocks[i]);
	rpmalloc_thread_finalize();
	return 0;
}

static void
child_page_remote(void) {
	enum { N = 256, FREED = 32 };
	static void* blocks[N];
	for (int i = 0; i < N; ++i)
		blocks[i] = rpmalloc(PAGE64_SIZE);
	remote_arg_t arg = {blocks, FREED};
	pthread_t thread;
	pthread_create(&thread, 0, remote_free_worker, &arg);
	pthread_join(thread, 0);
	// The freed blocks are held in the page thread free list, corrupt one before the owner adopts the chain
	corrupt_link(blocks[FREED / 2], unmapped_address());
	for (int i = 0; i < 100000; ++i) {
		if (!rpmalloc(PAGE64_SIZE))
			_exit(2);
	}
}

static void
child_heap_remote(void) {
	// Exhaust whole small pages so they are flagged full; a cross thread free then lands in the heap level
	// remote list, collected through rpmalloc_thread_collect
	enum { N = 6000, FREED = 16 };
	static void* blocks[N];
	for (int i = 0; i < N; ++i)
		blocks[i] = rpmalloc(SMALL_SIZE);
	remote_arg_t arg = {blocks, FREED};
	pthread_t thread;
	pthread_create(&thread, 0, remote_free_worker, &arg);
	pthread_join(thread, 0);
	corrupt_link(blocks[0], unmapped_address());
	rpmalloc_thread_collect();  // real heap remote list consumer
}

static void*
batch_chain_worker(void* argp) {
	remote_arg_t* arg = argp;
	rpmalloc_thread_initialize();
	// Consecutive frees into one small, non full page batch in the thread local chain; the first is pushed,
	// the rest stay chained until the collect below flushes them as one batch
	for (size_t i = 0; i < arg->count; ++i)
		rpfree(arg->blocks[i]);
	// Corrupt an interior, still chained block before the batch is published
	copy_link(arg->blocks[2], arg->blocks[0]);
	rpmalloc_thread_collect();
	rpmalloc_thread_finalize();
	return 0;
}

static void
child_batch_chain(void) {
	enum { N = 256, CHAINED = 8 };
	static void* blocks[N];
	for (int i = 0; i < N; ++i)
		blocks[i] = rpmalloc(SMALL_SIZE);
	remote_arg_t arg = {blocks, CHAINED};
	pthread_t thread;
	pthread_create(&thread, 0, batch_chain_worker, &arg);
	pthread_join(thread, 0);
	for (int i = 0; i < 100000; ++i) {
		if (!rpmalloc(SMALL_SIZE))
			_exit(2);
	}
}

#endif  // ENABLE_FREELIST_GUARD

#if !ENABLE_FREELIST_GUARD
static void
child_switch_off_control(void) {
	// With the guard compiled out a wrong link word is neither detected nor aborted. Use a safe null write so
	// the control exercises the consumer without dereferencing an unmapped address.
	void *a, *b;
	build_local_chain(&a, &b);
	corrupt_link(a, 0);
	for (int i = 0; i < 100000; ++i) {
		if (!rpmalloc(SMALL_SIZE))
			_exit(2);
	}
}
#endif

int
main(void) {
	rpmalloc_interface_t iface = {0};
	iface.error_callback = error_callback;
	if (rpmalloc_initialize(&iface))
		return fail("rpmalloc_initialize");
	rpmalloc_thread_initialize();

	if (normal_group())
		return 1;

	printf("corruption group (%s):\n", ENABLE_FREELIST_GUARD ? "guard on" : "guard off");
#if ENABLE_FREELIST_GUARD
	if (run_child("local chain, unmapped word", child_local_unmapped, 1))
		return 1;
	if (run_child("local chain, copied word", child_local_copy, 1))
		return 1;
	if (run_child("page remote chain", child_page_remote, 1))
		return 1;
	if (run_child("heap remote full page", child_heap_remote, 1))
		return 1;
	if (run_child("thread batch chain", child_batch_chain, 1))
		return 1;
	// No callback configured: termination is still mandatory, but no diagnostic prefix can be emitted
	if (run_child("no error callback set", child_no_callback, 0))
		return 1;
#else
	if (run_child("switch off control", child_switch_off_control, 0))
		return 1;
#endif

	rpmalloc_thread_finalize();
	rpmalloc_finalize();
	printf("freelist guard contract: all checks passed\n");
	return 0;
}
