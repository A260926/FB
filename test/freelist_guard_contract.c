/* Free list guard contract test.
 *
 * Two groups:
 *   - a normal group that drives the real allocator (small block batches, alignment, realloc, remote
 *     frees arranged with join, full page collection and thread exit) and checks live contents and
 *     reuse after free without ever tripping the guard;
 *   - a corruption group that runs each scenario in a separate child process, keeps the mapping live,
 *     overwrites the chain word stored in a freed user block and then drives the corruption through a
 *     real allocation or collection path. Every corruption case must report FREELIST_CORRUPTION and
 *     terminate with SIGABRT, never SIGSEGV, a timeout or a forged pointer.
 *
 * The test only observes internal state through the test only seams in freelist_guard_seams.h; it
 * never bypasses the real free list consumers.
 *
 * SPDX-FileCopyrightText: 2026 Mattias Jansson
 * SPDX-License-Identifier: Unlicense OR MIT
 */

#include "rpmalloc.h"
#include "freelist_guard_seams.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define GUARD_PREFIX "FREELIST_CORRUPTION"
#define CASE_TIMEOUT_SECONDS 10

static volatile int g_error_callback_fired;

static void
error_callback(const char* message) {
	g_error_callback_fired = 1;
	fprintf(stderr, "callback: %s\n", message ? message : "(null)");
}

static void
corrupt_word(void* block, void* value) {
	// The chain word is the first word of the user block, exactly what a write after free can overwrite
	*(void**)block = value;
}

/* A pointer that is not mapped in these test processes. The guard must abort before it is followed */
static void* const UNMAPPED_ADDRESS = (void*)(uintptr_t)0x0000000BAD000000ULL;

/* Drive real allocations of one size class until the list holding a corrupted head is consumed.
 * The guard verifies the chain word at pop (before following it), so reaching the corrupted block
 * aborts; this never dereferences the wild word itself. The bound is generous enough to empty the
 * heap size class cache and then the page holding the block. */
static void* g_drain[100000];

static void
consume_through_lists(size_t size) {
	for (size_t i = 0; i < sizeof(g_drain) / sizeof(g_drain[0]); ++i)
		g_drain[i] = rpmalloc(size);
}

//---------------------------------------------------------------------------------------
// Corruption scenarios (each executed inside a forked child, expected to abort)
//---------------------------------------------------------------------------------------

// Local page free list: free two blocks so the most recently freed block is the local list head,
// then overwrite its chain word and force the head to be popped
static void
case_local_list(void) {
	void* q = rpmalloc(48);
	void* p = rpmalloc(48);
	rpfree(q);
	rpfree(p);
	corrupt_word(p, UNMAPPED_ADDRESS);
	consume_through_lists(48);
}

// Page remote free list: another thread frees into a page owned by this thread, the block waits in
// the page thread free list. Corrupt it before the owner adopts the list on the next allocation
static void*
remote_free_thread(void* arg) {
	rpmalloc_thread_initialize();
	rpfree(arg);
	rpmalloc_thread_finalize();
	return 0;
}

static void
case_page_remote(void) {
	void* p = rpmalloc(64);
	pthread_t thread;
	if (pthread_create(&thread, 0, remote_free_thread, p))
		_exit(3);
	pthread_join(thread, 0);
	corrupt_word(p, UNMAPPED_ADDRESS);
	consume_through_lists(64);
}

// Heap remote free list: fill a small page so it is flagged full, then have another thread free a
// block of the full page, which routes to the heap level thread free list. Corrupt the word and let
// the owner collect through the real heap collection path
struct full_page_arg {
	void* block;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int ready;
	int go;
};

static void*
full_page_remote_thread(void* arg) {
	struct full_page_arg* state = arg;
	rpmalloc_thread_initialize();
	// Page is full, so this lands on the heap level thread free list
	rpfree(state->block);
	pthread_mutex_lock(&state->mutex);
	state->ready = 1;
	pthread_cond_signal(&state->cond);
	while (!state->go)
		pthread_cond_wait(&state->cond, &state->mutex);
	pthread_mutex_unlock(&state->mutex);
	rpmalloc_thread_finalize();
	return 0;
}

static void
case_heap_remote(void) {
	enum { CAP = 20000 };
	void** blocks = (void**)rpmalloc((size_t)CAP * sizeof(void*));
	if (!blocks)
		_exit(3);
	size_t count = 0;
	// Allocate 16 byte blocks until the page of the first block is provably full
	do {
		if (count >= CAP)
			_exit(4);
		blocks[count++] = rpmalloc(16);
	} while (!rpmalloc_test_block_page_full(blocks[0]));

	void* p = blocks[0];
	struct full_page_arg state;
	pthread_mutex_init(&state.mutex, 0);
	pthread_cond_init(&state.cond, 0);
	state.block = p;
	state.ready = 0;
	state.go = 0;
	pthread_t thread;
	if (pthread_create(&thread, 0, full_page_remote_thread, &state))
		_exit(3);
	pthread_mutex_lock(&state.mutex);
	while (!state.ready)
		pthread_cond_wait(&state.cond, &state.mutex);
	pthread_mutex_unlock(&state.mutex);

	corrupt_word(p, UNMAPPED_ADDRESS);

	pthread_mutex_lock(&state.mutex);
	state.go = 1;
	pthread_cond_signal(&state.cond);
	pthread_mutex_unlock(&state.mutex);
	pthread_join(thread, 0);
	pthread_mutex_destroy(&state.mutex);
	pthread_cond_destroy(&state.cond);

	// Refill collects the heap level list and follows the corrupted word, which must abort first
	consume_through_lists(16);
}

// Thread batched free chain: three frees from another thread leave a two block chain (q -> r) batched
// in thread local storage. The chain head word is corrupted while the chain is unpublished, then the
// thread flushes it on finalize and the owner adopts it through the real path
struct batch_arg {
	void* blocks[3];
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int ready;
	int go;
};

static void*
batch_thread(void* arg) {
	struct batch_arg* state = arg;
	rpmalloc_thread_initialize();
	rpfree(state->blocks[0]);  // run of one, pushed immediately and marks the chain page
	rpfree(state->blocks[1]);  // begins the batched chain, head
	rpfree(state->blocks[2]);  // appends to the chain
	pthread_mutex_lock(&state->mutex);
	state->ready = 1;
	pthread_cond_signal(&state->cond);
	while (!state->go)
		pthread_cond_wait(&state->cond, &state->mutex);
	pthread_mutex_unlock(&state->mutex);
	// Flush publishes the chain; the splice only rewrites the tail word, not the corrupted head
	rpmalloc_thread_finalize();
	return 0;
}

static void
case_batch_chain(void) {
	struct batch_arg state;
	pthread_mutex_init(&state.mutex, 0);
	pthread_cond_init(&state.cond, 0);
	state.ready = 0;
	state.go = 0;
	// Consecutive tiny allocations come from the same fresh page
	state.blocks[0] = rpmalloc(32);
	state.blocks[1] = rpmalloc(32);
	state.blocks[2] = rpmalloc(32);
	pthread_t thread;
	if (pthread_create(&thread, 0, batch_thread, &state))
		_exit(3);
	pthread_mutex_lock(&state.mutex);
	while (!state.ready)
		pthread_cond_wait(&state.cond, &state.mutex);
	pthread_mutex_unlock(&state.mutex);

	// Corrupt the chain head word. Its shadow still records the next chained block
	corrupt_word(state.blocks[1], UNMAPPED_ADDRESS);

	pthread_mutex_lock(&state.mutex);
	state.go = 1;
	pthread_cond_signal(&state.cond);
	pthread_mutex_unlock(&state.mutex);
	pthread_join(thread, 0);
	pthread_mutex_destroy(&state.mutex);
	pthread_cond_destroy(&state.cond);

	consume_through_lists(32);
}

// Copy another node's chain word (here the null terminator of a deeper block) into the list head. The
// overwritten value is a benign, possibly null word rather than a wild address, proving the check
// compares against the shadow fact instead of testing address validity
static void
case_copy_other_word(void) {
	void* q = rpmalloc(48);
	void* p = rpmalloc(48);
	rpfree(q);
	rpfree(p);
	void* other_word = *(void**)q;  // q is deeper in the list, its word is the prior head (null here)
	corrupt_word(p, other_word);
	consume_through_lists(48);
}

// Write an explicitly unmapped address. The guard must validate and abort with SIGABRT before the
// pointer is dereferenced, so this must never surface as SIGSEGV
static void
case_unmapped_word(void) {
	void* q = rpmalloc(48);
	void* p = rpmalloc(48);
	rpfree(q);
	rpfree(p);
	corrupt_word(p, UNMAPPED_ADDRESS);
	consume_through_lists(48);
}

typedef void (*case_fn)(void);

struct corruption_case {
	const char* name;
	case_fn fn;
};

static const struct corruption_case corruption_cases[] = {
    {"local", case_local_list},
    {"page_remote", case_page_remote},
    {"heap_remote", case_heap_remote},
    {"batch_chain", case_batch_chain},
    {"copy_other_word", case_copy_other_word},
    {"unmapped_word", case_unmapped_word},
};

static int
run_corruption_case(const struct corruption_case* testcase) {
	int pipefd[2];
	if (pipe(pipefd) != 0) {
		perror("pipe");
		return 1;
	}
	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return 1;
	}
	if (pid == 0) {
		// Child: redirect output, bound the run and execute the scenario. A guard abort is expected.
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		alarm(CASE_TIMEOUT_SECONDS);
		testcase->fn();
		// Returning means the corruption was not detected: a forged pointer or a silent failure
		fprintf(stderr, "case %s returned without aborting\n", testcase->name);
		_exit(20);
	}
	close(pipefd[1]);
	char buffer[1024];
	size_t length = 0;
	ssize_t got;
	while ((got = read(pipefd[0], buffer + length, sizeof(buffer) - length - 1)) > 0) {
		length += (size_t)got;
		if (length >= sizeof(buffer) - 1)
			break;
	}
	buffer[length] = 0;
	close(pipefd[0]);

	int status = 0;
	if (waitpid(pid, &status, 0) < 0) {
		perror("waitpid");
		return 1;
	}

	int aborted = WIFSIGNALED(status) && (WTERMSIG(status) == SIGABRT);
	int reported = strstr(buffer, GUARD_PREFIX) != 0;
	int segfaulted = WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV);

	int pass = aborted && reported && !segfaulted;
	printf("  [%s] %-16s exit=%s sig=%d reported=%s%s\n", pass ? "PASS" : "FAIL", testcase->name,
	       WIFEXITED(status) ? "normal" : (WIFSIGNALED(status) ? "signal" : "other"),
	       WIFSIGNALED(status) ? WTERMSIG(status) : -1, reported ? "yes" : "no",
	       segfaulted ? " SIGSEGV!" : "");
	if (!pass && length)
		printf("      child output: %s\n", buffer);
	return pass ? 0 : 1;
}

//---------------------------------------------------------------------------------------
// Normal behaviour group: must never trip the guard
//---------------------------------------------------------------------------------------

struct remote_block_arg {
	void* block;
	size_t size;
};

static void*
normal_remote_worker(void* arg) {
	struct remote_block_arg* state = (struct remote_block_arg*)arg;
	rpmalloc_thread_initialize();
	unsigned char* block = (unsigned char*)state->block;
	// Touch within the usable size, then free across the thread boundary
	for (size_t i = 0; i < state->size; ++i)
		block[i] = (unsigned char)(i + 1);
	rpfree(block);
	rpmalloc_thread_finalize();
	return 0;
}

static int
run_normal_group(void) {
	// Batch of small allocations with content and alignment checks
	enum { BATCH = 256 };
	void* batch[BATCH];
	size_t batch_size[BATCH];
	for (int i = 0; i < BATCH; ++i) {
		size_t size = (size_t)(16 + (i * 37) % 200);
		batch_size[i] = size;
		batch[i] = rpmalloc(size);
		if (!batch[i]) {
			printf("normal group: allocation %d failed\n", i);
			return 1;
		}
		memset(batch[i], i & 0xFF, size);
	}
	void* aligned = rpaligned_alloc(128, 512);
	if (!aligned || (((uintptr_t)aligned % 128) != 0)) {
		printf("normal group: aligned allocation failed\n");
		return 1;
	}

	// The shadow of a freed block must mirror the chain word the allocator just stored. Allocate two
	// blocks of a fresh size class, free them so the most recent one is the local list head, then read
	// both the trusted shadow and the user visible word through the test seam
	if (rpmalloc_test_guard_enabled()) {
		void* f0 = rpmalloc(176);
		void* f1 = rpmalloc(176);
		rpfree(f0);
		rpfree(f1);  // f1 is now a list head, its word points at f0 or another block
		void* stored_word = *(void**)f1;
		uintptr_t shadow_word = rpmalloc_test_shadow_expected(f1);
		if ((shadow_word == RPMALLOC_TEST_SHADOW_NA) || (shadow_word != (uintptr_t)stored_word)) {
			printf("normal group: freed block shadow does not mirror its chain word\n");
			return 1;
		}
		// Both stay freed and are reclaimed through the ordinary churn that follows
	}

	// Realloc preserves content
	unsigned char* grow = (unsigned char*)rpmalloc(73);
	if (!grow)
		return 1;
	memset(grow, 0x52, 73);
	grow = (unsigned char*)rprealloc(grow, 4096);
	if (!grow)
		return 1;
	for (int i = 0; i < 73; ++i) {
		if (grow[i] != 0x52) {
			printf("normal group: realloc did not preserve content\n");
			return 1;
		}
	}

	// Remote frees of dedicated, correctly sized blocks arranged through join, then collection
	enum { REMOTE = 8 };
	struct remote_block_arg remote_args[REMOTE];
	pthread_t workers[REMOTE];
	for (int i = 0; i < REMOTE; ++i) {
		remote_args[i].size = 64;
		remote_args[i].block = rpmalloc(remote_args[i].size);
		if (!remote_args[i].block)
			return 1;
		if (pthread_create(&workers[i], 0, normal_remote_worker, &remote_args[i]))
			return 1;
	}
	for (int i = 0; i < REMOTE; ++i)
		pthread_join(workers[i], 0);
	rpmalloc_thread_collect();

	// Surviving batch blocks keep their contents
	for (int i = 0; i < BATCH; ++i) {
		unsigned char expect = (unsigned char)(i & 0xFF);
		for (size_t b = 0; b < batch_size[i]; ++b) {
			if (((unsigned char*)batch[i])[b] != expect) {
				printf("normal group: live block %d content corrupted\n", i);
				return 1;
			}
		}
	}
	for (int i = 0; i < BATCH; ++i)
		rpfree(batch[i]);
	rpfree(aligned);
	rpfree(grow);

	// Freed blocks are reused normally, with the shadow cycling allocated/free without false reports
	void* reused = 0;
	void* marker = rpmalloc(200);
	rpfree(marker);
	for (int attempt = 0; attempt < 1000; ++attempt) {
		void* candidate = rpmalloc(200);
		if (candidate == marker) {
			reused = candidate;
			break;
		}
		rpfree(candidate);
	}
	if (!reused) {
		printf("normal group: freed block was not reused\n");
		return 1;
	}
	rpfree(reused);

	// Full page churn and thread exit: fill and release whole pages, collecting between rounds
	for (int round = 0; round < 4; ++round) {
		enum { CHURN = 4096 };
		void** churn = (void**)rpmalloc(CHURN * sizeof(void*));
		if (!churn)
			return 1;
		for (int i = 0; i < CHURN; ++i) {
			churn[i] = rpmalloc(16);
			if (!churn[i])
				return 1;
		}
		for (int i = 0; i < CHURN; ++i)
			rpfree(churn[i]);
		rpfree(churn);
		rpmalloc_thread_collect();
	}

	printf("normal group: PASS (alloc, align, realloc, remote free, collect, reuse, thread exit)\n");
	return 0;
}

//---------------------------------------------------------------------------------------
// Guard-off control: the same local corruption must not raise the guard diagnostic
//---------------------------------------------------------------------------------------

static int
run_disabled_control(void) {
	g_error_callback_fired = 0;
	void* q = rpmalloc(48);
	void* p = rpmalloc(48);
	rpfree(q);
	rpfree(p);
	// Copy the deeper (null) word. With the guard off this is accepted as a normal list head and the
	// allocation returns a valid block instead of aborting
	void* other_word = *(void**)q;
	corrupt_word(p, other_word);
	void* returned = rpmalloc(48);
	if (g_error_callback_fired) {
		printf("disabled control: FAIL, error callback fired with guard disabled\n");
		return 1;
	}
	if (returned != p) {
		printf("disabled control: note head block not returned (got %p want %p)\n", returned, p);
	}
	printf("disabled control: PASS (no %s diagnostic, allocator kept original behaviour)\n", GUARD_PREFIX);
	return 0;
}

int
main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	setvbuf(stdout, 0, _IOLBF, 0);

	rpmalloc_interface_t interface;
	memset(&interface, 0, sizeof(interface));
	interface.error_callback = error_callback;
	if (rpmalloc_initialize(&interface)) {
		printf("rpmalloc_initialize failed\n");
		return 1;
	}

	int failures = 0;

	if (!rpmalloc_test_guard_enabled()) {
		failures += run_disabled_control();
		rpmalloc_thread_finalize();
		rpmalloc_finalize();
		return failures ? 1 : 0;
	}

	failures += run_normal_group();

	printf("corruption group (each must report %s and die with SIGABRT):\n", GUARD_PREFIX);
	for (size_t i = 0; i < sizeof(corruption_cases) / sizeof(corruption_cases[0]); ++i)
		failures += run_corruption_case(&corruption_cases[i]);

	rpmalloc_thread_finalize();
	rpmalloc_finalize();

	if (failures) {
		printf("FREELIST GUARD CONTRACT: FAIL (%d failing case(s))\n", failures);
		return 1;
	}
	printf("FREELIST GUARD CONTRACT: PASS\n");
	return 0;
}
