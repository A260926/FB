/*
 * Shared declarations for the deterministic atomicity contract tests and
 * the libFuzzer entry point.  The bounded model engine and the controlled
 * allocator live in buffer_atomic_contract.c; the fuzz driver only feeds
 * seeds into them.
 */
#ifndef BUFFER_ATOMIC_TEST_H_INCLUDED_
#define BUFFER_ATOMIC_TEST_H_INCLUDED_

#include <stddef.h>

/* Outcome of one deterministic model run. */
struct buffer_model_result {
	int failed;
	unsigned seed;
	unsigned op_index;
	/* Name of the first field that diverged between the model and the
	 * real buffers, kept short on purpose. */
	const char *field;
	size_t model_value;
	size_t real_value;
};

/* Run a fixed-seed sequence of bounded operations against real evbuffers
 * and an independent byte/identity model.  Returns a zeroed
 * buffer_model_result (failed == 0) on success. */
struct buffer_model_result buffer_model_run(unsigned seed, unsigned max_ops);

/* Controlled allocator bookkeeping, exposed so the fuzz driver can assert a
 * clean ledger between independent runs. */
void buffer_test_allocator_reset(void);
size_t buffer_test_live_allocations(void);
size_t buffer_test_total_allocations(void);

#endif
