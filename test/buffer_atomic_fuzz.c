/*
 * libFuzzer entry point for the evbuffer failure-atomicity model.
 *
 * It shares the exact bounded action vocabulary and checking core with the
 * deterministic contract test (test/buffer_atomic_contract.c) through
 * buffer_atomic_model.h.  The fuzzer input is only used to derive additional
 * seeds for the same bounded model; it never drives data sizes directly, so
 * every input stays within the same finite operation set.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "buffer_atomic_model.h"

/* The harness must be installed before any libevent object exists.  This
 * constructor runs before LLVMFuzzerTestOneInput on the first call, but we
 * also guard it explicitly in case a libFuzzer build inlines differently. */
static int fuzz_harness_ready;

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint64_t seed = 0x5eed000000000000ull;
	size_t i;

	if (!fuzz_harness_ready) {
		atomic_harness_install();
		fuzz_harness_ready = 1;
	}
	atomic_fault_off();

	/* Fold the input into a seed; the model still runs a bounded number of
	 * operations and performs its own bounds checking. */
	for (i = 0; i < size; ++i)
		seed = (seed * 1099511628211ull) ^ data[i];
	seed |= 1;

	if (model_run_seed(seed, 80)) {
		/* model_run_seed already printed the seed, step and the first
		 * field difference; abort so libFuzzer records the crasher. */
		fprintf(stderr, "fuzz model failure seed=0x%llx\n",
		    (unsigned long long)seed);
		abort();
	}
	if (atomic_ledger_outstanding() != 0) {
		fprintf(stderr,
		    "fuzz ledger failure: %zu allocations outstanding\n",
		    atomic_ledger_outstanding());
		abort();
	}
	return 0;
}

