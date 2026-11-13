/*
 * checks/workgroup.c
 *
 * Offline workgroup executor checks.  Everything here is C99 plus libm;
 * the SPIR-V modules are the word fixtures in workgroup_fixtures.h, which
 * carry their readable assembly in comments.
 *
 * Expected values are computed independently by the reference_* helpers
 * below, which simulate the shader semantics with plain C arrays.  They are
 * never derived from executor output.
 */

#include <spvm/context.h>
#include <spvm/program.h>
#include <spvm/workgroup.h>
#include <spvm/state.h>
#include <spvm/value.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "workgroup_fixtures.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

#define CHECK_EQ_U(a, b) do { \
	unsigned long long check_a = (unsigned long long)(a); \
	unsigned long long check_b = (unsigned long long)(b); \
	checks++; \
	if (check_a != check_b) { \
		failures++; \
		fprintf(stderr, "FAIL %s:%d: %s == %s (%llu != %llu)\n", \
			__FILE__, __LINE__, #a, #b, check_a, check_b); \
	} \
} while (0)

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static spvm_program_t make_program(spvm_context_t ctx, const spvm_word* words, size_t count)
{
	return spvm_program_create(ctx, words, count);
}

static unsigned result_u(spvm_state_t state, const char* name)
{
	spvm_result_t result = spvm_state_get_result_with_value(state, (spvm_string)name);
	if (!result)
		return 0xdeadbeefu;
	return result->members[0].value.u;
}

/* Reference model of the swap module.  Shared storage starts zeroed, every
 * invocation writes i+10, and after the barrier pair each slot receives the
 * value its partner published, so out_val[i] == (i^1) + 10.  The vector and
 * struct members are written by invocation 0 through AccessChain and a
 * Workgroup pointer parameter and must be visible to everyone. */
static void reference_swap(unsigned out_val[4], unsigned out_zero[4],
	unsigned* out_vec, unsigned* out_arg)
{
	unsigned shared[4] = { 0, 0, 0, 0 };

	for (unsigned i = 0; i < 4; i++)
		out_zero[i] = shared[i];

	for (unsigned i = 0; i < 4; i++)
		shared[i] = i + 10;

	unsigned other[4];
	for (unsigned i = 0; i < 4; i++)
		other[i] = shared[i ^ 1];

	for (unsigned i = 0; i < 4; i++)
		shared[i] = other[i];

	for (unsigned i = 0; i < 4; i++)
		out_val[i] = shared[i];

	*out_vec = 77;
	*out_arg = 99;
}

/* Reference model of the reduction module. */
static void reference_reduce(unsigned wgx, unsigned out_sum[4],
	unsigned out_idx[4], unsigned out_total[4], unsigned out_nested[4],
	unsigned out_zero[4])
{
	unsigned data[4];
	unsigned nested = 0;

	for (unsigned i = 0; i < 4; i++)
		out_zero[i] = nested;

	for (unsigned i = 0; i < 4; i++)
		data[i] = (i + 1) + wgx * 100;

	for (unsigned i = 0; i < 2; i++)
		data[i] = data[i] + data[i + 2];

	unsigned total = 0;
	for (unsigned i = 0; i < 2; i++)
		total += data[i];
	data[0] = total;

	for (unsigned i = 0; i < 4; i++) {
		out_sum[i] = data[0];
		out_idx[i] = i;
		out_total[i] = total;
		out_nested[i] = 555; /* invocation 0 wrote it before the first barrier */
	}
}

static spvm_context_t g_ctx = NULL;

static spvm_workgroup_t create_wg(const spvm_word* words, size_t count,
	spvm_word wgx, spvm_word wgy, spvm_word wgz,
	spvm_program_t* out_program, spvm_workgroup_error* error)
{
	spvm_program_t program = make_program(g_ctx, words, count);
	if (!program) {
		fprintf(stderr, "failed to create program\n");
		return NULL;
	}
	spvm_word wgid[3] = { wgx, wgy, wgz };
	spvm_workgroup_t wg = spvm_workgroup_create(program, wgid, error);
	if (!wg) {
		spvm_program_delete(program);
		return NULL;
	}
	*out_program = program;
	return wg;
}

/* Run a group with one of several host stepping orders and dump outputs. */
typedef enum step_order {
	step_seq,
	step_reverse,
	step_round_robin,
	step_strided,
	step_order_count
} step_order;

static const char* order_name(step_order order)
{
	switch (order) {
	case step_seq: return "sequential";
	case step_reverse: return "reverse";
	case step_round_robin: return "round-robin";
	case step_strided: return "strided";
	default: return "?";
	}
}

/*
 * Drive the whole group with spvm_workgroup_step only, using the requested
 * order.  Each pass picks one invocation; waiting/finished invocations are
 * skipped by the driver (stepping them is not an error, just a no-op).
 */
static int drive(spvm_workgroup_t wg, step_order order, spvm_workgroup_error* error)
{
	unsigned count = spvm_workgroup_get_invocation_count(wg);
	unsigned cursor = 0;
	unsigned passes = 0;
	unsigned char was_waiting[64];

	CHECK(count <= sizeof(was_waiting));
	memset(was_waiting, 0, sizeof(was_waiting));

	for (;;) {
		unsigned progressed = 0;
		unsigned runnable = 0;

		for (unsigned pass = 0; pass < count; pass++) {
			unsigned index;
			switch (order) {
			case step_seq:
				index = pass;
				break;
			case step_reverse:
				index = count - 1 - pass;
				break;
			case step_round_robin:
				index = (cursor + pass) % count;
				break;
			case step_strided: {
				/* 0, 2, 1, 3, ... */
				unsigned half = (count + 1) / 2;
				index = (pass & 1) ? (half + pass / 2) : (pass / 2);
				index %= count;
				break;
			}
			default:
				index = pass;
			}

			spvm_state_t state = spvm_workgroup_get_state(wg, index);
			spvm_word before = state->instruction_count;
			spvm_workgroup_step_result result = spvm_workgroup_step(wg, index, error);

			if (result == SPVM_WORKGROUP_STEP_ERROR)
				return 0;
			if (result == SPVM_WORKGROUP_STEP_OK ||
				result == SPVM_WORKGROUP_STEP_RELEASED)
				progressed++;
			/* An invocation that was waiting before this call must not
			 * execute anything. */
			if (was_waiting[index] && state->instruction_count != before) {
				fprintf(stderr, "waiting invocation %u executed an instruction\n", index);
				return 0;
			}
			was_waiting[index] = (result == SPVM_WORKGROUP_STEP_WAITING);

			/* A release wakes every invocation, including those that were
			 * parked before this call. */
			if (result == SPVM_WORKGROUP_STEP_RELEASED)
				memset(was_waiting, 0, sizeof(was_waiting));
		}

		cursor = (cursor + 1) % count;

		/* Count what is still runnable so dead ends are visible. */
		for (unsigned i = 0; i < count; i++) {
			spvm_state_t state = spvm_workgroup_get_state(wg, i);
			if (state->code_current != NULL)
				runnable++;
		}
		if (runnable == 0)
			return 1;
		if (!progressed) {
			fprintf(stderr, "no progress in order %s\n", order_name(order));
			return 0;
		}
		if (++passes > 100000) {
			fprintf(stderr, "driver did not terminate\n");
			return 0;
		}
	}
}

/* ------------------------------------------------------------------ */
/* shared memory: swap                                                 */
/* ------------------------------------------------------------------ */

static void check_swap(step_order order)
{
	spvm_workgroup_error error;
	spvm_program_t program = NULL;
	spvm_workgroup_t wg = create_wg(swap_words,
		sizeof(swap_words) / sizeof(*swap_words), 0, 0, 0, &program, &error);
	CHECK(wg != NULL);
	if (!wg)
		return;

	CHECK_EQ_U(spvm_workgroup_get_invocation_count(wg), 4);
	CHECK(drive(wg, order, &error));

	unsigned exp_val[4], exp_zero[4], exp_vec, exp_arg;
	reference_swap(exp_val, exp_zero, &exp_vec, &exp_arg);

	for (unsigned i = 0; i < 4; i++) {
		spvm_state_t state = spvm_workgroup_get_state(wg, i);
		CHECK(state != NULL);
		CHECK_EQ_U(result_u(state, "out_val"), exp_val[i]);
		CHECK_EQ_U(result_u(state, "out_zero"), exp_zero[i]);
		CHECK_EQ_U(result_u(state, "out_idx"), i);
		CHECK_EQ_U(result_u(state, "out_vec"), exp_vec);
		CHECK_EQ_U(result_u(state, "out_struct"), exp_arg);
		/* Shared vector and struct storage is zeroed on creation. */
		CHECK_EQ_U(result_u(state, "out_vec_zero"), 0);
		CHECK_EQ_U(result_u(state, "out_struct_zero"), 0);
	}

	spvm_workgroup_delete(wg);
	spvm_program_delete(program);
}

static void check_swap_repeated(step_order order)
{
	/* Repeated create/destroy must not leak or retain state. */
	for (int iteration = 0; iteration < 3; iteration++) {
		spvm_workgroup_error error;
		spvm_program_t program = NULL;
		spvm_workgroup_t wg = create_wg(swap_words,
			sizeof(swap_words) / sizeof(*swap_words), 0, 0, 0, &program, &error);
		CHECK(wg != NULL);
		if (!wg)
			continue;
		CHECK(drive(wg, order, &error));
		unsigned exp_val[4], exp_zero[4], exp_vec, exp_arg;
		reference_swap(exp_val, exp_zero, &exp_vec, &exp_arg);
		for (unsigned i = 0; i < 4; i++)
			CHECK_EQ_U(result_u(spvm_workgroup_get_state(wg, i), "out_val"),
				exp_val[i]);
		spvm_workgroup_delete(wg);
		spvm_program_delete(program);
	}
}

/* ------------------------------------------------------------------ */
/* barrier rounds: reduction                                           */
/* ------------------------------------------------------------------ */

static void check_reduce(step_order order, spvm_word wgx)
{
	spvm_workgroup_error error;
	spvm_program_t program = NULL;
	spvm_workgroup_t wg = create_wg(reduce_words,
		sizeof(reduce_words) / sizeof(*reduce_words), wgx, 0, 0, &program, &error);
	CHECK(wg != NULL);
	if (!wg)
		return;

	CHECK(drive(wg, order, &error));

	unsigned exp_sum[4], exp_idx[4], exp_total[4], exp_nested[4], exp_zero[4];
	reference_reduce((unsigned)wgx, exp_sum, exp_idx, exp_total, exp_nested, exp_zero);

	for (unsigned i = 0; i < 4; i++) {
		spvm_state_t state = spvm_workgroup_get_state(wg, i);
		CHECK_EQ_U(result_u(state, "out_sum"), exp_sum[i]);
		CHECK_EQ_U(result_u(state, "out_idx"), exp_idx[i]);
		CHECK_EQ_U(result_u(state, "out_total"), exp_total[i]);
		CHECK_EQ_U(result_u(state, "out_nested"), exp_nested[i]);
		CHECK_EQ_U(result_u(state, "out_zero"), exp_zero[i]);
	}

	spvm_workgroup_delete(wg);
	spvm_program_delete(program);
}

static void check_reduce_run(void)
{
	spvm_workgroup_error error;
	spvm_program_t program = NULL;
	spvm_workgroup_t wg = create_wg(reduce_words,
		sizeof(reduce_words) / sizeof(*reduce_words), 0, 0, 0, &program, &error);
	CHECK(wg != NULL);
	if (!wg)
		return;

	CHECK(spvm_workgroup_run(wg, &error) == SPVM_WORKGROUP_RESULT_FINISHED);

	unsigned exp_sum[4], exp_idx[4], exp_total[4], exp_nested[4], exp_zero[4];
	reference_reduce(0, exp_sum, exp_idx, exp_total, exp_nested, exp_zero);
	for (unsigned i = 0; i < 4; i++) {
		spvm_state_t state = spvm_workgroup_get_state(wg, i);
		CHECK_EQ_U(result_u(state, "out_sum"), exp_sum[i]);
		CHECK_EQ_U(result_u(state, "out_total"), exp_total[i]);
	}

	spvm_workgroup_delete(wg);
	spvm_program_delete(program);
}

/* ------------------------------------------------------------------ */
/* per-invocation storage                                              */
/* ------------------------------------------------------------------ */

static void check_storage_isolation(void)
{
	spvm_workgroup_error error;
	spvm_program_t program = NULL;
	spvm_workgroup_t wg = create_wg(storage_words,
		sizeof(storage_words) / sizeof(*storage_words), 0, 0, 0, &program, &error);
	CHECK(wg != NULL);
	if (!wg)
		return;

	/* Private, Function and Output storage is per invocation: each local
	 * invocation id must flow through its own variables untouched. */
	CHECK(spvm_workgroup_run(wg, &error) == SPVM_WORKGROUP_RESULT_FINISHED);

	for (unsigned i = 0; i < 4; i++) {
		spvm_state_t state = spvm_workgroup_get_state(wg, i);
		CHECK_EQ_U(result_u(state, "out_private"), i);
		CHECK_EQ_U(result_u(state, "out_function"), i * 3);
		CHECK_EQ_U(result_u(state, "out_output"), i + 5);
	}

	spvm_workgroup_delete(wg);
	spvm_program_delete(program);
}

/* ------------------------------------------------------------------ */
/* builtins                                                            */
/* ------------------------------------------------------------------ */

static void check_builtins(void)
{
	spvm_workgroup_error error;
	spvm_program_t program = NULL;
	const spvm_word wgid[3] = { 2, 3, 1 };
	spvm_program_t prog = make_program(g_ctx, builtins_words,
		sizeof(builtins_words) / sizeof(*builtins_words));
	CHECK(prog != NULL);
	if (!prog)
		return;
	spvm_workgroup_t wg = spvm_workgroup_create(prog, wgid, &error);
	CHECK(wg != NULL);
	if (!wg) {
		spvm_program_delete(prog);
		return;
	}
	program = prog;
	(void)program;

	CHECK(spvm_workgroup_run(wg, &error) == SPVM_WORKGROUP_RESULT_FINISHED);

	/* 2x2x2, x fastest */
	for (unsigned index = 0; index < 8; index++) {
		unsigned x = index % 2;
		unsigned y = (index / 2) % 2;
		unsigned z = index / 4;
		spvm_state_t state = spvm_workgroup_get_state(wg, index);

		CHECK_EQ_U(result_u(state, "out_lx"), x);
		CHECK_EQ_U(result_u(state, "out_ly"), y);
		CHECK_EQ_U(result_u(state, "out_lz"), z);
		CHECK_EQ_U(result_u(state, "out_index"), index);
		CHECK_EQ_U(result_u(state, "out_wx"), 2);
		CHECK_EQ_U(result_u(state, "out_wy"), 3);
		CHECK_EQ_U(result_u(state, "out_wz"), 1);
		CHECK_EQ_U(result_u(state, "out_gx"), 2 * 2 + x);
		CHECK_EQ_U(result_u(state, "out_gy"), 3 * 2 + y);
		CHECK_EQ_U(result_u(state, "out_gz"), 1 * 2 + z);
	}

	spvm_workgroup_delete(wg);
	spvm_program_delete(prog);
}

/* ------------------------------------------------------------------ */
/* two workgroups on one program                                       */
/* ------------------------------------------------------------------ */

static void check_interleaved_groups(void)
{
	spvm_workgroup_error error;
	spvm_program_t program = make_program(g_ctx, reduce_words,
		sizeof(reduce_words) / sizeof(*reduce_words));
	CHECK(program != NULL);
	if (!program)
		return;

	spvm_word wgid_a[3] = { 0, 0, 0 };
	spvm_word wgid_b[3] = { 1, 0, 0 };
	spvm_workgroup_t a = spvm_workgroup_create(program, wgid_a, &error);
	spvm_workgroup_t b = spvm_workgroup_create(program, wgid_b, &error);
	CHECK(a != NULL);
	CHECK(b != NULL);
	if (!a || !b) {
		if (a) spvm_workgroup_delete(a);
		if (b) spvm_workgroup_delete(b);
		spvm_program_delete(program);
		return;
	}

	/* Interleave the two groups one instruction at a time; each group's
	 * shared storage must stay private to it. */
	unsigned count = spvm_workgroup_get_invocation_count(a);
	unsigned finished = 0;
	unsigned guard = 0;

	while (finished < 2 * count) {
		for (unsigned i = 0; i < count; i++) {
			spvm_state_t state_a = spvm_workgroup_get_state(a, i);
			spvm_state_t state_b = spvm_workgroup_get_state(b, i);
			int a_was_live = state_a->code_current != NULL;
			int b_was_live = state_b->code_current != NULL;

			spvm_workgroup_step_result ra = spvm_workgroup_step(a, i, &error);
			spvm_workgroup_step_result rb = spvm_workgroup_step(b, i, &error);
			if (ra == SPVM_WORKGROUP_STEP_ERROR || rb == SPVM_WORKGROUP_STEP_ERROR) {
				fprintf(stderr, "interleaved step failed: %s\n", error.message);
				finished = 2 * count;
				break;
			}
			if (a_was_live && state_a->code_current == NULL)
				finished++;
			if (b_was_live && state_b->code_current == NULL)
				finished++;
		}
		if (++guard > 100000) {
			CHECK(0 && "interleaved groups did not terminate");
			break;
		}
	}

	unsigned exp_sum_a[4], exp_idx[4], exp_total_a[4], exp_nested[4], exp_zero[4];
	unsigned exp_sum_b[4], exp_total_b[4];
	reference_reduce(0, exp_sum_a, exp_idx, exp_total_a, exp_nested, exp_zero);
	reference_reduce(1, exp_sum_b, exp_idx, exp_total_b, exp_nested, exp_zero);

	for (unsigned i = 0; i < 4; i++) {
		spvm_state_t sa = spvm_workgroup_get_state(a, i);
		spvm_state_t sb = spvm_workgroup_get_state(b, i);
		CHECK_EQ_U(result_u(sa, "out_sum"), exp_sum_a[i]);
		CHECK_EQ_U(result_u(sb, "out_sum"), exp_sum_b[i]);
		CHECK_EQ_U(result_u(sa, "out_total"), exp_total_a[i]);
		CHECK_EQ_U(result_u(sb, "out_total"), exp_total_b[i]);
	}

	/* Deleting one group must not invalidate the other. */
	spvm_workgroup_delete(a);
	for (unsigned i = 0; i < 4; i++)
		CHECK_EQ_U(result_u(spvm_workgroup_get_state(b, i), "out_sum"), exp_sum_b[i]);

	/* A third group created from the same program still works. */
	spvm_word wgid_c[3] = { 2, 0, 0 };
	spvm_workgroup_t c = spvm_workgroup_create(program, wgid_c, &error);
	CHECK(c != NULL);
	if (c) {
		CHECK(spvm_workgroup_run(c, &error) == SPVM_WORKGROUP_RESULT_FINISHED);
		unsigned exp_sum_c[4], exp_total_c[4];
		reference_reduce(2, exp_sum_c, exp_idx, exp_total_c, exp_nested, exp_zero);
		for (unsigned i = 0; i < 4; i++)
			CHECK_EQ_U(result_u(spvm_workgroup_get_state(c, i), "out_sum"),
				exp_sum_c[i]);
		spvm_workgroup_delete(c);
	}

	spvm_workgroup_delete(b);
	spvm_program_delete(program);
}

/* ------------------------------------------------------------------ */
/* diagnostics                                                         */
/* ------------------------------------------------------------------ */

static void check_early_return(void)
{
	spvm_workgroup_error error;
	spvm_program_t program = NULL;
	spvm_workgroup_t wg = create_wg(early_words,
		sizeof(early_words) / sizeof(*early_words), 0, 0, 0, &program, &error);
	CHECK(wg != NULL);
	if (!wg)
		return;

	/* Invocation 1 reaches the barrier while invocation 0 is still runnable,
	 * so it legitimately waits. */
	spvm_workgroup_step_result r1 = SPVM_WORKGROUP_STEP_OK;
	unsigned steps = 0;
	while (r1 == SPVM_WORKGROUP_STEP_OK && steps++ < 100)
		r1 = spvm_workgroup_step(wg, 1, &error);
	CHECK(r1 == SPVM_WORKGROUP_STEP_WAITING);
	CHECK(spvm_workgroup_step(wg, 1, &error) == SPVM_WORKGROUP_STEP_WAITING);

	spvm_state_t waiting_state = spvm_workgroup_get_state(wg, 1);
	spvm_word waiting_count = waiting_state->instruction_count;

	/* Invocation 0 now returns; a finished invocation while another waits
	 * must be diagnosed with invocation, instruction and round. */
	spvm_workgroup_step_result r0 = SPVM_WORKGROUP_STEP_OK;
	steps = 0;
	while (r0 == SPVM_WORKGROUP_STEP_OK && steps++ < 100)
		r0 = spvm_workgroup_step(wg, 0, &error);

	CHECK(r0 == SPVM_WORKGROUP_STEP_ERROR);
	CHECK(error.code == SPVM_WORKGROUP_ERROR_FINISHED_WHILE_WAITING);
	CHECK(error.invocation == 0);
	CHECK(error.instruction >= 0);
	CHECK(error.round >= 0);
	CHECK(strstr(error.message, "invocation") != NULL);

	/* The waiting invocation was not moved. */
	CHECK(waiting_state->code_current != NULL);
	CHECK_EQ_U(waiting_state->instruction_count, waiting_count);

	/* run() reports the stored failure. */
	CHECK(spvm_workgroup_run(wg, &error) == SPVM_WORKGROUP_RESULT_ERROR);
	CHECK(error.code == SPVM_WORKGROUP_ERROR_FINISHED_WHILE_WAITING);

	const spvm_workgroup_error* stored = spvm_workgroup_get_error(wg);
	CHECK(stored != NULL);
	CHECK(stored->code == error.code);

	spvm_workgroup_delete(wg);
	spvm_program_delete(program);
}

static void check_barrier_after_finished(void)
{
	spvm_workgroup_error error;
	spvm_program_t program = NULL;
	spvm_workgroup_t wg = create_wg(early_words,
		sizeof(early_words) / sizeof(*early_words), 0, 0, 0, &program, &error);
	CHECK(wg != NULL);
	if (!wg)
		return;

	/* Finish invocation 0 first. */
	spvm_workgroup_step_result r0 = SPVM_WORKGROUP_STEP_OK;
	unsigned steps = 0;
	while (r0 == SPVM_WORKGROUP_STEP_OK && steps++ < 100)
		r0 = spvm_workgroup_step(wg, 0, &error);
	CHECK(r0 == SPVM_WORKGROUP_STEP_FINISHED);

	/* Invocation 1 arriving at the barrier now finds a finished peer. */
	spvm_workgroup_step_result r1 = SPVM_WORKGROUP_STEP_OK;
	steps = 0;
	while (r1 == SPVM_WORKGROUP_STEP_OK && steps++ < 100)
		r1 = spvm_workgroup_step(wg, 1, &error);

	CHECK(r1 == SPVM_WORKGROUP_STEP_ERROR);
	CHECK(error.code == SPVM_WORKGROUP_ERROR_FINISHED_WHILE_WAITING);
	CHECK(error.invocation == 0);
	CHECK(error.instruction >= 0);
	CHECK(error.round == 0);
	CHECK(strstr(error.message, "invocation") != NULL);

	spvm_workgroup_delete(wg);
	spvm_program_delete(program);
}

static void check_callsite_mismatch(void)
{
	spvm_workgroup_error error;
	spvm_program_t program = NULL;
	spvm_workgroup_t wg = create_wg(callsite_words,
		sizeof(callsite_words) / sizeof(*callsite_words), 0, 0, 0, &program, &error);
	CHECK(wg != NULL);
	if (!wg)
		return;

	spvm_workgroup_result run = spvm_workgroup_run(wg, &error);
	CHECK(run == SPVM_WORKGROUP_RESULT_ERROR);
	CHECK(error.code == SPVM_WORKGROUP_ERROR_BARRIER_STACK_MISMATCH ||
		error.code == SPVM_WORKGROUP_ERROR_BARRIER_SITE_MISMATCH);
	CHECK(error.invocation >= 0 && error.invocation < 2);
	CHECK(error.instruction >= 0);
	CHECK(error.round >= 0);
	CHECK(strstr(error.message, "invocation") != NULL);

	spvm_workgroup_delete(wg);
	spvm_program_delete(program);
}

static void check_barrier_site_mismatch(void)
{
	spvm_workgroup_error error;
	spvm_program_t program = NULL;
	spvm_workgroup_t wg = create_wg(barsites_words,
		sizeof(barsites_words) / sizeof(*barsites_words), 0, 0, 0, &program, &error);
	CHECK(wg != NULL);
	if (!wg)
		return;

	spvm_workgroup_result run = spvm_workgroup_run(wg, &error);
	CHECK(run == SPVM_WORKGROUP_RESULT_ERROR);
	CHECK(error.code == SPVM_WORKGROUP_ERROR_BARRIER_SITE_MISMATCH);
	CHECK(error.round == 0);
	CHECK(error.instruction >= 0);
	CHECK(error.barrier >= 0);
	CHECK(strstr(error.message, "round") != NULL);

	spvm_workgroup_delete(wg);
	spvm_program_delete(program);
}

static void check_round_mismatch(void)
{
	spvm_workgroup_error error;
	spvm_program_t program = NULL;
	spvm_workgroup_t wg = create_wg(roundskip_words,
		sizeof(roundskip_words) / sizeof(*roundskip_words), 0, 0, 0, &program, &error);
	CHECK(wg != NULL);
	if (!wg)
		return;

	spvm_workgroup_result run = spvm_workgroup_run(wg, &error);
	CHECK(run == SPVM_WORKGROUP_RESULT_ERROR);
	/* Invocation 0 is one round ahead and also at a different barrier, so
	 * either mismatch is a correct diagnosis of the divergence. */
	CHECK(error.code == SPVM_WORKGROUP_ERROR_BARRIER_ROUND_MISMATCH ||
		error.code == SPVM_WORKGROUP_ERROR_BARRIER_SITE_MISMATCH);
	CHECK(error.round >= 0);
	CHECK(error.instruction >= 0);
	CHECK(error.barrier >= 0);
	CHECK(error.invocation >= 0 && error.invocation < 2);
	CHECK(strstr(error.message, "round") != NULL);

	spvm_workgroup_delete(wg);
	spvm_program_delete(program);
}

/* ------------------------------------------------------------------ */
/* barrier round bookkeeping and waiting instruction counts            */
/* ------------------------------------------------------------------ */

static void check_loop_barrier(void)
{
	spvm_workgroup_error error;
	spvm_program_t program = NULL;
	spvm_workgroup_t wg = create_wg(loop_words,
		sizeof(loop_words) / sizeof(*loop_words), 0, 0, 0, &program, &error);
	CHECK(wg != NULL);
	if (!wg)
		return;

	/* Both invocations execute the same barrier instruction twice; each
	 * pass is its own round and they must rendezvous twice. */
	unsigned releases = 0;
	unsigned guard = 0;

	for (;;) {
		int all_done = 1;
		for (unsigned i = 0; i < 2; i++) {
			spvm_workgroup_step_result r = spvm_workgroup_step(wg, i, &error);
			CHECK(r != SPVM_WORKGROUP_STEP_ERROR);
			if (r == SPVM_WORKGROUP_STEP_ERROR)
				goto done;
			if (r == SPVM_WORKGROUP_STEP_RELEASED)
				releases++;
			if (r != SPVM_WORKGROUP_STEP_FINISHED)
				all_done = 0;
		}
		if (all_done)
			break;
		if (++guard > 10000) {
			CHECK(0 && "loop did not terminate");
			break;
		}
	}

	CHECK_EQ_U(releases, 2); /* two rounds, one release each */
	for (unsigned i = 0; i < 2; i++)
		CHECK_EQ_U(result_u(spvm_workgroup_get_state(wg, i), "out_counter"), 2);

done:
	spvm_workgroup_delete(wg);
	spvm_program_delete(program);
}

static void check_wait_does_not_advance(void)
{
	spvm_workgroup_error error;
	spvm_program_t program = NULL;
	spvm_workgroup_t wg = create_wg(reduce_words,
		sizeof(reduce_words) / sizeof(*reduce_words), 0, 0, 0, &program, &error);
	CHECK(wg != NULL);
	if (!wg)
		return;

	/* Let invocation 0 run to its first barrier. */
	spvm_workgroup_step_result r = SPVM_WORKGROUP_STEP_OK;
	while (r == SPVM_WORKGROUP_STEP_OK)
		r = spvm_workgroup_step(wg, 0, &error);
	CHECK(r == SPVM_WORKGROUP_STEP_WAITING);

	spvm_state_t state = spvm_workgroup_get_state(wg, 0);
	spvm_word count_before = state->instruction_count;
	spvm_source code_before = state->code_current;

	for (unsigned i = 0; i < 50; i++) {
		CHECK(spvm_workgroup_step(wg, 0, &error) == SPVM_WORKGROUP_STEP_WAITING);
		CHECK_EQ_U(state->instruction_count, count_before);
		CHECK(state->code_current == code_before);
	}

	spvm_workgroup_delete(wg);
	spvm_program_delete(program);
}

/* ------------------------------------------------------------------ */
/* creation validation                                                 */
/* ------------------------------------------------------------------ */

static void check_invalid_barrier(void)
{
	/* Patch the swap module so its barriers use Device scope; creation
	 * succeeds, execution must reject the barrier with a diagnostic. */
	size_t count = sizeof(swap_words) / sizeof(*swap_words);
	spvm_word* words = (spvm_word*)malloc(count * sizeof(spvm_word));
	CHECK(words != NULL);
	if (!words)
		return;
	memcpy(words, swap_words, count * sizeof(spvm_word));

	/* Find the OpControlBarrier's execution-scope operand and patch the
	 * OpConstant that defines it from Workgroup (2) to Device (1). */
	int patched = 0;
	spvm_word scope_id = -1;
	for (size_t i = 5; i + 3 < count; i++) {
		if ((words[i] & 0xffff) == SpvOpControlBarrier &&
			(words[i] >> 16) == 4) {
			scope_id = words[i + 1];
			break;
		}
	}
	if (scope_id >= 0) {
		for (size_t i = 5; i + 3 < count; i++) {
			if ((words[i] & 0xffff) == SpvOpConstant &&
				(words[i] >> 16) == 4 && words[i + 2] == scope_id &&
				words[i + 3] == SpvScopeWorkgroup) {
				words[i + 3] = SpvScopeDevice;
				patched = 1;
				break;
			}
		}
	}
	CHECK(patched);
	if (!patched) {
		free(words);
		return;
	}

	spvm_program_t program = make_program(g_ctx, words, count);
	spvm_word wgid[3] = { 0, 0, 0 };
	spvm_workgroup_error error;
	spvm_workgroup_t wg = spvm_workgroup_create(program, wgid, &error);
	CHECK(wg != NULL);
	if (wg) {
		/* Unsupported barrier scope must be reported before any invocation
		 * is released. */
		spvm_workgroup_result result = spvm_workgroup_run(wg, &error);
		CHECK(result == SPVM_WORKGROUP_RESULT_ERROR);
		CHECK(error.code == SPVM_WORKGROUP_ERROR_UNSUPPORTED_BARRIER ||
			error.code == SPVM_WORKGROUP_ERROR_FINISHED_WHILE_WAITING);
		spvm_workgroup_delete(wg);
	}
	spvm_program_delete(program);
	free(words);
}

static void check_rejects_unsupported(void)
{
	spvm_workgroup_error error;
	spvm_word wgid[3] = { 0, 0, 0 };

	struct {
		const spvm_word* words;
		size_t count;
		spvm_workgroup_error_code code;
		const char* what;
	} cases[] = {
		{ bad_image_words,
		  sizeof(bad_image_words) / sizeof(*bad_image_words),
		  SPVM_WORKGROUP_ERROR_UNSUPPORTED_FEATURE, "image type" },
		{ recursive_words,
		  sizeof(recursive_words) / sizeof(*recursive_words),
		  SPVM_WORKGROUP_ERROR_UNSUPPORTED_FEATURE, "recursion" },
		{ no_local_size_words,
		  sizeof(no_local_size_words) / sizeof(*no_local_size_words),
		  SPVM_WORKGROUP_ERROR_INVALID_PROGRAM, "missing LocalSize" },
		{ two_entry_points_words,
		  sizeof(two_entry_points_words) / sizeof(*two_entry_points_words),
		  SPVM_WORKGROUP_ERROR_INVALID_PROGRAM, "two entry points" },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		spvm_program_t program = make_program(g_ctx, cases[i].words, cases[i].count);
		CHECK(program != NULL);
		if (!program)
			continue;

		memset(&error, 0, sizeof(error));
		spvm_workgroup_t wg = spvm_workgroup_create(program, wgid, &error);
		checks++;
		if (wg) {
			failures++;
			fprintf(stderr, "FAIL: %s was accepted\n", cases[i].what);
			spvm_workgroup_delete(wg);
		} else if (error.code != cases[i].code) {
			failures++;
			fprintf(stderr, "FAIL: %s gave error %d (%s), expected %d\n",
				cases[i].what, error.code, error.message, cases[i].code);
		}
		spvm_program_delete(program);
	}

	/* NULL arguments are rejected without crashing. */
	memset(&error, 0, sizeof(error));
	CHECK(spvm_workgroup_create(NULL, wgid, &error) == NULL);
	CHECK(error.code == SPVM_WORKGROUP_ERROR_INVALID_ARGUMENT);

	spvm_program_t program = make_program(g_ctx, swap_words,
		sizeof(swap_words) / sizeof(*swap_words));
	CHECK(program != NULL);
	if (program) {
		memset(&error, 0, sizeof(error));
		CHECK(spvm_workgroup_create(program, NULL, &error) == NULL);
		CHECK(error.code == SPVM_WORKGROUP_ERROR_INVALID_ARGUMENT);
		spvm_program_delete(program);
	}
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
	g_ctx = spvm_context_initialize();
	CHECK(g_ctx != NULL);

	check_swap(step_seq);
	check_swap(step_reverse);
	check_swap(step_round_robin);
	check_swap(step_strided);
	check_swap_repeated(step_reverse);

	check_reduce(step_seq, 0);
	check_reduce(step_reverse, 0);
	check_reduce(step_round_robin, 0);
	check_reduce(step_strided, 0);
	check_reduce(step_seq, 3);
	check_reduce(step_reverse, 3);
	check_reduce_run();

	check_storage_isolation();
	check_builtins();
	check_interleaved_groups();

	check_early_return();
	check_barrier_after_finished();
	check_callsite_mismatch();
	check_barrier_site_mismatch();
	check_round_mismatch();
	check_loop_barrier();
	check_wait_does_not_advance();
	check_invalid_barrier();
	check_rejects_unsupported();

	spvm_context_deinitialize(g_ctx);

	if (failures) {
		fprintf(stderr, "WORKGROUP_CHECKS_FAILED (%d of %d)\n", failures, checks);
		return 1;
	}
	printf("WORKGROUP_CHECKS_OK (%d checks)\n", checks);
	return 0;
}
