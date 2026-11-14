/*
 * libFuzzer entry point for the bounded evbuffer atomicity model.
 *
 * It shares the exact action sequence and checking core with the
 * deterministic driver by including the contract translation unit; the
 * contract file deliberately has no main().  Failures report the seed, the
 * operation index and the first diverging field only -- never the full
 * buffer contents.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "buffer_atomic_contract.c"

/* The contract translation unit references the task assertion entry point
 * through TASK_CHECK; the fuzz target is standalone (it does not link the
 * task driver), so provide the minimal local definition. */
int
task_check(int value, const char *expr, const char *file, int line)
{
	(void)expr;
	(void)file;
	(void)line;
	return value;
}

static unsigned
seed_from_input(const uint8_t *data, size_t size)
{
	unsigned seed = 0x9e3779b9u;
	size_t i;

	for (i = 0; i < size && i < 4; ++i)
		seed = (seed << 8) | data[i];
	return seed ? seed : 0x9e3779b9u;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	unsigned seed, max_ops;
	struct buffer_model_result r;

	seed = seed_from_input(data, size);
	/* Keep every run finite and bounded by the shared engine limit. */
	max_ops = size > 4 ? 8u + (unsigned)(data[4] % (MODEL_MAX_OPS - 8u))
	    : MODEL_MAX_OPS;

	r = buffer_model_run(seed, max_ops);
	if (r.failed) {
		fprintf(stderr,
		    "FUZZ FAIL seed=%u op=%u field=%s model=%zu real=%zu\n",
		    r.seed, r.op_index, r.field ? r.field : "?",
		    r.model_value, r.real_value);
		/* A non-zero return makes libFuzzer keep the crashing input. */
		return 1;
	}
	return 0;
}

/* Standalone smoke driver for builds without a libFuzzer harness.  The
 * sanitizer/CI build uses the deterministic task driver instead. */
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PROJECT
int
main(void)
{
	unsigned i;

	ensure_allocator();
	for (i = 1; i <= 64; ++i) {
		struct buffer_model_result r =
		    buffer_model_run(i * 2654435761u, MODEL_MAX_OPS);
		if (r.failed) {
			fprintf(stderr,
			    "FUZZ FAIL seed=%u op=%u field=%s model=%zu "
			    "real=%zu\n",
			    r.seed, r.op_index, r.field ? r.field : "?",
			    r.model_value, r.real_value);
			return 1;
		}
	}
	puts("FUZZ_SMOKE_OK");
	return 0;
}
#endif
