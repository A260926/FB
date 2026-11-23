/*
 *    Stack-less Just-In-Time compiler
 *
 *    Copyright Zoltan Herczeg (hzmester@freemail.hu). All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are
 * permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice, this list of
 *      conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright notice, this list
 *      of conditions and the following disclaimer in the documentation and/or other materials
 *      provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE COPYRIGHT HOLDER(S) OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Structured fuzz regression for compiler serialization / deserialization
 * checkpoints that sit between an arithmetic instruction which defines the
 * CPU status flags and the first instruction consuming those flags
 * (carry/borrow chaining, condition materialization and conditional jumps).
 *
 * The same symbolic plan is executed three ways:
 *   - an independent unsigned modular model computed in plain C,
 *   - a one-shot compiled program,
 *   - programs compiled across one or several save/free/restore cycles.
 *
 * The expected values never come from emulated ARM condition codes or from
 * sljit status classification helpers: carry (ADD family) and borrow
 * (SUB family) are derived from unsigned modular arithmetic only. Every
 * consumed flag is produced by an earlier instruction of the same plan,
 * so no undefined flag state is ever compared.
 */

#include "sljitLir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../task_support/check.h"

/* --------------------------------------------------------------------- */
/* Fixed dimensions                                                      */
/* --------------------------------------------------------------------- */

/* Three chained arithmetic segments: a root ADD/SUB and two ADDC/SUBC. */
#define NCHAIN			3
/* Working slots addressed relative to the pointer argument. */
#define SLOT_C0			0
#define SLOT_C1			1
#define SLOT_C2			2
#define SLOT_BRANCH		3
#define SLOT_ACC		4
#define SLOT_V0			5
#define SLOT_V1			6
#define SLOT_V2			7
#define SLOT_POOL0		8
#define SLOT_POOL1		9
#define SLOT_POOL2		10
#define SLOT_POOL3		11
#define NWORK			12
/* C side allocation: input canary, work slots, output canary. */
#define NALLOC			(NWORK + 2)
#define CANARY_IN		0
#define CANARY_OUT		(NWORK + 1)
#define CANARY_MAGIC		((sljit_uw)0x51A7C0DEC0DE51A7UL)

/* Register assignment (scratch registers R(1)..R(11) are all requested). */
#define REG_CHAIN0		SLJIT_R(1)
#define REG_CHAIN1		SLJIT_R(2)
#define REG_CHAIN2		SLJIT_R(3)
#define REG_MOVTMP		SLJIT_R(4)
#define REG_ACC			SLJIT_R(5)
#define REG_C0			SLJIT_R(6)
#define REG_C1			SLJIT_R(7)
#define REG_C2			SLJIT_R(8)
#define REG_IN_A		SLJIT_R(9)
#define REG_IN_B		SLJIT_R(10)
#define REG_POOL		SLJIT_R(11)

/* Emission boundaries. A checkpoint may be inserted at each of these. */
enum {
	CP_AFTER_ENTER = 0,
	CP_PREMAT0,		/* step 0 arithmetic emitted, materialization not yet */
	CP_POSTMAT0,
	CP_PREMAT1,		/* step 1 arithmetic emitted, materialization not yet */
	CP_POSTMAT1,
	CP_PREMAT2,		/* step 2 arithmetic emitted, materialization not yet */
	CP_POSTMAT2,
	CP_PREJUMP,		/* flags live, conditional jump not yet emitted */
	CP_NBOUND
};

#define MAX_CYCLES		4

/* Operand forms for the second arithmetic operand. */
enum {
	FORM_REG = 0,
	FORM_IMM,
	FORM_MEM
};

/* --------------------------------------------------------------------- */
/* Symbolic plan                                                         */
/* --------------------------------------------------------------------- */

struct plan {
	sljit_u32 seed;
	int width32;		/* nonzero: 32 bit plan, otherwise machine word */
	int sub_family;		/* nonzero: SUB/SUBC (borrow), zero: ADD/ADDC (carry) */
	int use_not_carry;	/* nonzero: consume SLJIT_NOT_CARRY, else SLJIT_CARRY */
	int branch_tail;	/* nonzero: conditional jump consumer, else materialize */
	int form[NCHAIN];
	sljit_uw opval[NCHAIN];	/* numeric second operand, independent of form */
	sljit_uw in_a;
	sljit_uw in_b;
	sljit_uw branch_take;
	sljit_uw branch_fall;
};

/* Expected state computed by the independent model. */
struct model_result {
	sljit_uw value[NCHAIN];	/* chain segment results */
	sljit_uw carry[NCHAIN];	/* carry out (ADD) / borrow out (SUB), 0 or 1 */
	sljit_uw materialized[NCHAIN];
	sljit_uw branch_value;
	sljit_uw acc;
	sljit_uw mask;
};

/* Deterministic PRNG: same seed always regenerates the same plan. */
static sljit_u32 next_rand(sljit_u32 *state)
{
	sljit_u32 x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static const sljit_sw imm_table[] = {
	0, 1, 3, 7, 17, 255, 1000, 4095, -1, -4, -17, -255
};
#define NIMM ((int)(sizeof(imm_table) / sizeof(imm_table[0])))

/* Edge biased inputs so both carry == 0 and carry == 1 are forced. */
static const sljit_uw edge_inputs[][2] = {
	{ 0UL, 0UL },
	{ 1UL, 1UL },
	{ 0xFFFFFFFFUL, 1UL },
	{ 0xFFFFFFFFUL, 0xFFFFFFFFUL },
	{ 0x80000000UL, 0x7FFFFFFFUL },
	{ 0x12345678UL, 0x9ABCDEF0UL },
	{ 0UL, 0xFFFFFFFFUL },
	{ 0xDEADBEEFUL, 0x21524110UL },
#if (defined SLJIT_64BIT_ARCHITECTURE && SLJIT_64BIT_ARCHITECTURE)
	{ 0xFFFFFFFFFFFFFFFFUL, 1UL },
	{ 0xFFFFFFFFFFFFFFFFUL, 0xFFFFFFFFFFFFFFFFUL },
	{ 0x8000000000000000UL, 0x7FFFFFFFFFFFFFFFUL },
	{ 0x0123456789ABCDEFUL, 0xFEDCBA9876543210UL }
#endif
};
#define NEDGE ((int)(sizeof(edge_inputs) / sizeof(edge_inputs[0])))

static sljit_uw width_mask(int width32)
{
#if (defined SLJIT_64BIT_ARCHITECTURE && SLJIT_64BIT_ARCHITECTURE)
	if (width32)
		return 0xFFFFFFFFUL;
#else
	(void)width32;
#endif
	return ~(sljit_uw)0;
}

static struct plan make_plan(sljit_u32 seed)
{
	struct plan p;
	sljit_u32 r = seed ? seed : 1u;
	sljit_uw mask;
	int j;
	int edge = (int)(next_rand(&r) % 7u == 0u);
	int edge_idx;

	memset(&p, 0, sizeof(p));
	p.seed = seed;
	p.width32 = (int)(next_rand(&r) & 1u);
	p.sub_family = (int)(next_rand(&r) & 1u);
	p.use_not_carry = (int)(next_rand(&r) & 1u);
	p.branch_tail = (int)(next_rand(&r) & 1u);
	mask = width_mask(p.width32);

	if (edge) {
		edge_idx = (int)(next_rand(&r) % (sljit_u32)NEDGE);
		p.in_a = edge_inputs[edge_idx][0] & mask;
		p.in_b = edge_inputs[edge_idx][1] & mask;
	} else {
		p.in_a = ((sljit_uw)next_rand(&r) | ((sljit_uw)next_rand(&r) << 32)) & mask;
		p.in_b = ((sljit_uw)next_rand(&r) | ((sljit_uw)next_rand(&r) << 32)) & mask;
	}

	for (j = 0; j < NCHAIN; j++) {
		p.form[j] = (int)(next_rand(&r) % 3u);
		p.opval[j] = (sljit_uw)imm_table[next_rand(&r) % (sljit_u32)NIMM] & mask;
	}

	p.branch_take = (sljit_uw)(0x5A000000UL | (next_rand(&r) & 0xFFFUL)) & mask;
	p.branch_fall = (sljit_uw)(0xA5000000UL | (next_rand(&r) & 0xFFFUL)) & mask;
	return p;
}

/* --------------------------------------------------------------------- */
/* Independent unsigned modular model                                    */
/* --------------------------------------------------------------------- */

/*
 * ADD family:
 *   root ADD:  r = a + b,                 carry = 1 iff unsigned overflow
 *   ADDC:      r = a + b + carry_in,      carry = 1 iff unsigned overflow
 * SUB family:
 *   root SUB:  r = a - b,                 borrow = 1 iff a < b
 *   SUBC:      r = a - b - borrow_in,     borrow = 1 iff unsigned underflow
 *
 * All math is done on sljit_uw (unsigned): wrapping is defined by C and no
 * signed arithmetic ever overflows. Carry/borrow are derived by unsigned
 * comparison, never from architecture condition code semantics.
 */
/*
 * Effective second operand for a chain step. REG form uses REG_POOL /
 * REG_IN_B, both loaded on entry from the C side in_b slot; IMM form uses
 * the plan constant; MEM form reads an explicitly allocated C slot. The
 * model and the JIT resolve every form to this same value.
 */
static sljit_uw effective_operand(const struct plan *p, int step)
{
	sljit_uw mask = width_mask(p->width32);

	switch (p->form[step]) {
	case FORM_IMM:
		return p->opval[step] & mask;
	case FORM_MEM:
		/* Step 0 reads the in_b pool slot, later steps their own slot. */
		return (step == 0 ? p->in_b : p->opval[step]) & mask;
	default: /* FORM_REG */
		return p->in_b & mask;
	}
}

static struct model_result run_model(const struct plan *p)
{
	struct model_result m;
	sljit_uw mask = width_mask(p->width32);
	sljit_uw v = p->in_a & mask;
	sljit_uw cin = 0;
	int j;

	memset(&m, 0, sizeof(m));
	m.mask = mask;

	for (j = 0; j < NCHAIN; j++) {
		sljit_uw x = effective_operand(p, j);
		sljit_uw t, cout;

		if (!p->sub_family) {
			if (j == 0) {
				/* Plain ADD. */
				cout = (v > mask - x) ? 1u : 0u;
				t = (v + x) & mask;
			} else {
				sljit_uw c1, c2;
				c1 = (v > mask - x) ? 1u : 0u;
				t = (v + x) & mask;
				c2 = (t > mask - cin) ? 1u : 0u;
				cout = c1 | c2;
				t = (t + cin) & mask;
			}
		} else {
			if (j == 0) {
				/* Plain SUB. */
				cout = (v < x) ? 1u : 0u;
				t = (v - x) & mask;
			} else {
				/*
				 * SUBC chains the prior borrow: r = v - x - cin.
				 * The two unsigned comparisons detect a borrow
				 * generated at either subtraction stage.
				 */
				sljit_uw b1, b2;
				b1 = (v < x) ? 1u : 0u;
				t = (v - x) & mask;
				b2 = (t < cin) ? 1u : 0u;
				cout = b1 | b2;
				t = (t - cin) & mask;
			}
		}

		m.value[j] = t;
		m.carry[j] = cout;
		m.materialized[j] = p->use_not_carry ? (1u - cout) : cout;
		v = t;
		cin = cout;
	}

	/*
	 * Conditional jump consumer: jump is taken when the condition holds.
	 * Materialize-only plans record the last condition in this slot instead.
	 */
	{
		sljit_uw cond = p->use_not_carry ? (1u - m.carry[NCHAIN - 1])
			: m.carry[NCHAIN - 1];
		m.branch_value = p->branch_tail
			? (cond ? p->branch_take : p->branch_fall)
			: cond;
	}

	/* The materialized conditions participate in later computation. */
	m.acc = (m.materialized[0] + m.materialized[1] + m.materialized[2]) & mask;
	if (p->branch_tail)
		m.acc = (m.acc + m.branch_value) & mask;
	return m;
}

/* --------------------------------------------------------------------- */
/* Coverage counters and diagnostics                                     */
/* --------------------------------------------------------------------- */

struct stats {
	int plans;
	int width32;
	int width64;
	int family_add;
	int family_sub;
	int cond_carry;
	int cond_not_carry;
	int tail_materialize;
	int tail_branch;
	int form_reg;
	int form_imm;
	int form_mem;
	int carry_zero;
	int carry_one;
	int direct_runs;
	int resume_runs;
	int multi_runs;
	int resume_at[CP_NBOUND];
	int lifetime_runs;
	int runtime_failures;
};

static void account_plan(struct stats *st, const struct plan *p,
	const struct model_result *m)
{
	int j;
	st->plans++;
	st->width32 += p->width32;
	st->width64 += !p->width32;
	st->family_add += !p->sub_family;
	st->family_sub += p->sub_family;
	st->cond_carry += !p->use_not_carry;
	st->cond_not_carry += p->use_not_carry;
	st->tail_materialize += !p->branch_tail;
	st->tail_branch += p->branch_tail;
	for (j = 0; j < NCHAIN; j++) {
		if (p->form[j] == FORM_REG)
			st->form_reg++;
		else if (p->form[j] == FORM_IMM)
			st->form_imm++;
		else
			st->form_mem++;
		st->carry_zero += (m->carry[j] == 0);
		st->carry_one += (m->carry[j] != 0);
	}
}

static void print_category(const char *name, int value)
{
	if (value != 0)
		printf("CATEGORY %s %d\n", name, value);
}

static void print_stats(const char *group, const struct stats *st)
{
	printf("STATS %s begin\n", group);
	print_category("plans", st->plans);
	print_category("width32", st->width32);
	print_category("width64", st->width64);
	print_category("family_add", st->family_add);
	print_category("family_sub", st->family_sub);
	print_category("cond_carry", st->cond_carry);
	print_category("cond_not_carry", st->cond_not_carry);
	print_category("tail_materialize", st->tail_materialize);
	print_category("tail_branch", st->tail_branch);
	print_category("form_reg", st->form_reg);
	print_category("form_imm", st->form_imm);
	print_category("form_mem", st->form_mem);
	print_category("carry_seen_zero", st->carry_zero);
	print_category("carry_seen_one", st->carry_one);
	print_category("direct_runs", st->direct_runs);
	print_category("resume_runs", st->resume_runs);
	print_category("multi_runs", st->multi_runs);
	print_category("resume_after_enter", st->resume_at[CP_AFTER_ENTER]);
	print_category("resume_premat0", st->resume_at[CP_PREMAT0]);
	print_category("resume_postmat0", st->resume_at[CP_POSTMAT0]);
	print_category("resume_premat1", st->resume_at[CP_PREMAT1]);
	print_category("resume_postmat1", st->resume_at[CP_POSTMAT1]);
	print_category("resume_premat2", st->resume_at[CP_PREMAT2]);
	print_category("resume_postmat2", st->resume_at[CP_POSTMAT2]);
	print_category("resume_prejump", st->resume_at[CP_PREJUMP]);
	print_category("lifetime_runs", st->lifetime_runs);
	print_category("runtime_failures", st->runtime_failures);
	printf("STATS %s end\n", group);
	fflush(stdout);
}

static void print_encoding(int index, const struct plan *p)
{
	printf("ENCODING %d seed=0x%08lx width=%s family=%s cond=%s tail=%s "
		"forms=%d,%d,%d opval=0x%lx,0x%lx,0x%lx in_a=0x%lx in_b=0x%lx "
		"take=0x%lx fall=0x%lx\n",
		index, (unsigned long)p->seed, p->width32 ? "32" : "64",
		p->sub_family ? "sub" : "add",
		p->use_not_carry ? "not_carry" : "carry",
		p->branch_tail ? "branch" : "materialize",
		p->form[0], p->form[1], p->form[2],
		(unsigned long)p->opval[0], (unsigned long)p->opval[1],
		(unsigned long)p->opval[2],
		(unsigned long)p->in_a, (unsigned long)p->in_b,
		(unsigned long)p->branch_take, (unsigned long)p->branch_fall);
}

static void print_diag(const char *group, int index, const struct plan *p,
	const struct model_result *m, const sljit_uw *slots,
	int boundary, int resumes, int first_diff, sljit_uw got, sljit_uw want)
{
	int j;
	printf("DIAG group=%s plan=%d boundary=%d resumes=%d first_diff_slot=%d "
		"got=0x%lx want=0x%lx\n",
		group, index, boundary, resumes, first_diff,
		(unsigned long)got, (unsigned long)want);
	print_encoding(index, p);
	printf("DIAG in_a=0x%lx in_b=0x%lx canary_in=0x%lx canary_out=0x%lx\n",
		(unsigned long)p->in_a, (unsigned long)p->in_b,
		(unsigned long)slots[CANARY_IN], (unsigned long)slots[CANARY_OUT]);
	for (j = 0; j < NCHAIN; j++) {
		printf("DIAG step%d model_value=0x%lx model_carry=%lu materialized=0x%lx\n",
			j, (unsigned long)m->value[j],
			(unsigned long)m->carry[j],
			(unsigned long)m->materialized[j]);
	}
	fflush(stdout);
}

/* --------------------------------------------------------------------- */
/* C side memory slots with canaries                                     */
/* --------------------------------------------------------------------- */

static void init_slots(sljit_uw *slots, const struct plan *p)
{
	int i;
	for (i = 0; i < NALLOC; i++)
		slots[i] = 0;
	slots[CANARY_IN] = CANARY_MAGIC;
	slots[CANARY_OUT] = CANARY_MAGIC;
	/* Preloaded memory operand pool; only explicitly allocated slots. */
	slots[1 + SLOT_POOL0] = p->opval[0];
	slots[1 + SLOT_POOL1] = p->opval[1];
	slots[1 + SLOT_POOL2] = p->opval[2];
	slots[1 + SLOT_POOL3] = p->in_b;
}

/* --------------------------------------------------------------------- */
/* JIT emission, optionally interrupted by serialization checkpoints    */
/* --------------------------------------------------------------------- */

struct emit_ctx {
	struct sljit_compiler *compiler;
	int cycles[MAX_CYCLES];
	int ncycles;
	int next_cycle;
	int resumes;
	int failed;		/* API / allocation failure: never a content bug */
};

static int do_checkpoint(struct emit_ctx *ctx, int boundary)
{
	sljit_uw *buf;
	sljit_uw size;
	struct sljit_compiler *new_compiler;

	if (ctx->next_cycle >= ctx->ncycles || ctx->cycles[ctx->next_cycle] != boundary)
		return SLJIT_SUCCESS;

	buf = sljit_serialize_compiler(ctx->compiler, 0, &size);
	if (buf == NULL) {
		ctx->failed = 1;
		return SLJIT_ERR_ALLOC_FAILED;
	}
	sljit_free_compiler(ctx->compiler);
	new_compiler = sljit_deserialize_compiler(buf, size, 0, NULL);
	/* The restored compiler must remain usable after the buffer is freed. */
	SLJIT_FREE(buf, NULL);
	if (new_compiler == NULL) {
		ctx->failed = 1;
		return SLJIT_ERR_ALLOC_FAILED;
	}
	ctx->compiler = new_compiler;
	ctx->next_cycle++;
	ctx->resumes++;
	return SLJIT_SUCCESS;
}

static int emit_arith(struct sljit_compiler *c, const struct plan *p, int step)
{
	sljit_s32 width = p->width32 ? SLJIT_32 : 0;
	sljit_s32 op;
	sljit_s32 dst = (step == 0) ? REG_CHAIN0 : (step == 1) ? REG_CHAIN1 : REG_CHAIN2;
	sljit_s32 src1 = (step == 0) ? REG_IN_A
		: (step == 1) ? REG_CHAIN0 : REG_CHAIN1;
	sljit_sw src2w = 0;
	sljit_s32 src2;

	if (step == 0)
		op = (p->sub_family ? SLJIT_SUB : SLJIT_ADD) | SLJIT_SET_CARRY | width;
	else
		op = (p->sub_family ? SLJIT_SUBC : SLJIT_ADDC) | SLJIT_SET_CARRY | width;

	switch (p->form[step]) {
	case FORM_REG:
		src2 = (step == 0) ? REG_IN_B : REG_POOL;
		break;
	case FORM_IMM:
		src2 = SLJIT_IMM;
		src2w = (sljit_sw)p->opval[step];
		break;
	default: /* FORM_MEM: memory operand from an explicitly allocated slot */
		src2 = SLJIT_MEM1(SLJIT_S0);
		src2w = (sljit_sw)((step == 0 ? SLOT_POOL3 : SLOT_POOL0 + step) * sizeof(sljit_sw));
		break;
	}
	return sljit_emit_op2(c, op, dst, 0, src1, 0, src2, src2w);
}

static int emit_width_mov(struct sljit_compiler *c, const struct plan *p,
	sljit_s32 dst, sljit_sw dstw, sljit_s32 src, sljit_sw srcw)
{
	return sljit_emit_op1(c, p->width32 ? SLJIT_MOV32 : SLJIT_MOV,
		dst, dstw, src, srcw);
}

static int store_width(struct sljit_compiler *c, const struct plan *p,
	int slot, sljit_s32 src)
{
	sljit_s32 op = p->width32 ? SLJIT_MOV_U32 : SLJIT_MOV;
	return sljit_emit_op1(c, op, SLJIT_MEM1(SLJIT_S0),
		(sljit_sw)(slot * sizeof(sljit_sw)), src, 0);
}

/*
 * Emits everything after the step 0 arithmetic + flags preserving MOV.
 * The compiler on entry holds live flags produced by that arithmetic.
 * Checkpoints listed in ctx->cycles are honored at their boundaries.
 * When with_return is zero the return instruction is omitted so the caller
 * can append its own legal tail (used by the lifetime group).
 */
static int emit_suffix(struct emit_ctx *ctx, const struct plan *p, int with_return)
{
	struct sljit_compiler *c = ctx->compiler;
	sljit_s32 width = p->width32 ? SLJIT_32 : 0;
	sljit_s32 cond = p->use_not_carry ? SLJIT_NOT_CARRY : SLJIT_CARRY;
	sljit_s32 flag_mov = p->width32 ? SLJIT_MOV32 : SLJIT_MOV;
	sljit_s32 chain_regs[NCHAIN] = { REG_CHAIN0, REG_CHAIN1, REG_CHAIN2 };
	sljit_s32 cond_regs[NCHAIN] = { REG_C0, REG_C1, REG_C2 };
	int carry_slots[NCHAIN] = { SLOT_C0, SLOT_C1, SLOT_C2 };
	int pre_boundaries[NCHAIN] = { -1, CP_PREMAT1, CP_PREMAT2 };
	int post_boundaries[NCHAIN] = { CP_POSTMAT0, CP_POSTMAT1, CP_POSTMAT2 };
	struct sljit_jump *jump;
	struct sljit_label *label;
	int j;

	/* Step 0 flags are already live; materialize them first. */
	for (j = 0; j < NCHAIN; j++) {
		if (j > 0) {
			/* Later arithmetic segments define their own flags first. */
			if (emit_arith(c, p, j) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			if (emit_width_mov(c, p, REG_MOVTMP, 0, chain_regs[j], 0)
					!= SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			if (do_checkpoint(ctx, pre_boundaries[j]) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			c = ctx->compiler;
		}

		/* First consumer of the flags: materialize the condition as 0/1. */
		if (sljit_emit_op_flags(c, flag_mov, cond_regs[j], 0, cond)
				!= SLJIT_SUCCESS)
			return sljit_get_compiler_error(c);
		if (store_width(c, p, carry_slots[j], cond_regs[j]) != SLJIT_SUCCESS)
			return sljit_get_compiler_error(c);

		if (do_checkpoint(ctx, post_boundaries[j]) != SLJIT_SUCCESS)
			return sljit_get_compiler_error(c);
		c = ctx->compiler;
	}

	/* Second consumer shape: a conditional jump writing two different values. */
	if (p->branch_tail) {
		if (do_checkpoint(ctx, CP_PREJUMP) != SLJIT_SUCCESS)
			return sljit_get_compiler_error(c);
		c = ctx->compiler;

		/*
		 * A taken conditional jump skips the fall-through body, so the
		 * taken value is loaded first and the not-taken value is written
		 * between the jump and its label.
		 */
		if (emit_width_mov(c, p, REG_MOVTMP, 0, SLJIT_IMM,
				(sljit_sw)p->branch_take) != SLJIT_SUCCESS)
			return sljit_get_compiler_error(c);
		jump = sljit_emit_jump(c, cond);
		if (jump == NULL)
			return sljit_get_compiler_error(c);
		if (emit_width_mov(c, p, REG_MOVTMP, 0, SLJIT_IMM,
				(sljit_sw)p->branch_fall) != SLJIT_SUCCESS)
			return sljit_get_compiler_error(c);
		label = sljit_emit_label(c);
		if (label == NULL)
			return sljit_get_compiler_error(c);
		sljit_set_label(jump, label);
		if (store_width(c, p, SLOT_BRANCH, REG_MOVTMP) != SLJIT_SUCCESS)
			return sljit_get_compiler_error(c);
	} else {
		/* Without a branch this slot simply mirrors the last condition. */
		if (store_width(c, p, SLOT_BRANCH, cond_regs[NCHAIN - 1])
				!= SLJIT_SUCCESS)
			return sljit_get_compiler_error(c);
	}

	/* Materialized conditions feed a later computation (accumulator). */
	if (emit_width_mov(c, p, REG_ACC, 0, REG_C0, 0) != SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);
	if (sljit_emit_op2(c, SLJIT_ADD | width, REG_ACC, 0, REG_ACC, 0, REG_C1, 0)
			!= SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);
	if (sljit_emit_op2(c, SLJIT_ADD | width, REG_ACC, 0, REG_ACC, 0, REG_C2, 0)
			!= SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);
	if (p->branch_tail) {
		if (sljit_emit_op2(c, SLJIT_ADD | width, REG_ACC, 0, REG_ACC, 0,
				REG_MOVTMP, 0) != SLJIT_SUCCESS)
			return sljit_get_compiler_error(c);
	}
	if (store_width(c, p, SLOT_ACC, REG_ACC) != SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);

	/* Chain snapshots: every segment result participates in the next one. */
	for (j = 0; j < NCHAIN; j++) {
		if (store_width(c, p, SLOT_V0 + j, chain_regs[j]) != SLJIT_SUCCESS)
			return sljit_get_compiler_error(c);
	}

	if (with_return && sljit_emit_return_void(c) != SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);
	return SLJIT_SUCCESS;
}

/*
 * Emits the whole plan. Checkpoints listed in ctx->cycles are inserted at
 * the matching emission boundary. Returns SLJIT_SUCCESS, or a library error
 * (ctx->failed set for allocation/serialization failures).
 */
static int emit_plan(struct emit_ctx *ctx, const struct plan *p)
{
	struct sljit_compiler *c = ctx->compiler;
	sljit_s32 chain_regs[NCHAIN] = { REG_CHAIN0, REG_CHAIN1, REG_CHAIN2 };

	if (sljit_emit_enter(c, 0, SLJIT_ARGS1V(P), 12, 3, 0) != SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);
	if (do_checkpoint(ctx, CP_AFTER_ENTER) != SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);
	c = ctx->compiler;

	/* Load the C side inputs; these registers stay defined until the end. */
	if (emit_width_mov(c, p, REG_IN_A, 0, SLJIT_IMM, (sljit_sw)p->in_a)
			!= SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);
	if (emit_width_mov(c, p, REG_IN_B, 0, SLJIT_IMM, (sljit_sw)p->in_b)
			!= SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);
	if (emit_width_mov(c, p, REG_POOL, 0, SLJIT_MEM1(SLJIT_S0),
			(sljit_sw)(SLOT_POOL3 * sizeof(sljit_sw))) != SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);

	/* Step 0 arithmetic defines the flags; the MOV preserves them. */
	if (emit_arith(c, p, 0) != SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);
	if (emit_width_mov(c, p, REG_MOVTMP, 0, chain_regs[0], 0) != SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);
	if (do_checkpoint(ctx, CP_PREMAT0) != SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);

	return emit_suffix(ctx, p, 1);
}

typedef void (SLJIT_FUNC *plan_func_t)(sljit_sw base);

/*
 * Compiles a plan with the requested checkpoint schedule, executes it once
 * and compares every touched slot, the carry chain and both canaries against
 * the independent model.
 *
 * Returns:
 *    1  - content matched (one or more TASK_CHECK comparisons executed)
 *    0  - library / allocation / unsupported failure (runtime failure only)
 *   -1  - content difference (TASK_CHECK already recorded the failure)
 */
static int build_run_compare(const char *group, int index, const struct plan *p,
	const struct model_result *m, const int *cycles, int ncycles,
	int *resumes_out, struct stats *st)
{
	struct emit_ctx ctx;
	sljit_uw slots[NALLOC];
	void *code;
	plan_func_t func;
	sljit_uw mask = width_mask(p->width32);
	int j;
	int first_diff = -1;
	sljit_uw got = 0, want = 0;

	memset(&ctx, 0, sizeof(ctx));
	ctx.compiler = sljit_create_compiler(NULL);
	if (ctx.compiler == NULL)
		return 0;
	if (ncycles > MAX_CYCLES) {
		sljit_free_compiler(ctx.compiler);
		return 0;
	}
	for (j = 0; j < ncycles; j++)
		ctx.cycles[j] = cycles[j];
	ctx.ncycles = ncycles;

	if (emit_plan(&ctx, p) != SLJIT_SUCCESS || ctx.failed) {
		sljit_free_compiler(ctx.compiler);
		return 0;
	}

	code = sljit_generate_code(ctx.compiler, 0, NULL);
	if (code == NULL || sljit_get_compiler_error(ctx.compiler) != SLJIT_ERR_COMPILED) {
		if (code != NULL)
			sljit_free_code(code, NULL);
		sljit_free_compiler(ctx.compiler);
		return 0;
	}
	sljit_free_compiler(ctx.compiler);

	init_slots(slots, p);
	func = (plan_func_t)code;
	func((sljit_sw)&slots[1]);

	/* Canaries first: any corruption is a real content difference. */
	if (slots[CANARY_IN] != CANARY_MAGIC || slots[CANARY_OUT] != CANARY_MAGIC) {
		first_diff = (slots[CANARY_IN] != CANARY_MAGIC) ? CANARY_IN : CANARY_OUT;
		got = slots[first_diff];
		want = CANARY_MAGIC;
	}

	/* Carry materialization outputs. */
	for (j = 0; j < NCHAIN && first_diff < 0; j++) {
		got = slots[1 + SLOT_C0 + j] & mask;
		want = m->materialized[j] & mask;
		if (got != want)
			first_diff = SLOT_C0 + j;
	}
	/* Branch / condition result. */
	if (first_diff < 0) {
		got = slots[1 + SLOT_BRANCH] & mask;
		want = m->branch_value & mask;
		if (got != want)
			first_diff = SLOT_BRANCH;
	}
	/* Accumulator built from the materialized conditions. */
	if (first_diff < 0) {
		got = slots[1 + SLOT_ACC] & mask;
		want = m->acc & mask;
		if (got != want)
			first_diff = SLOT_ACC;
	}
	/* Carry chain segment results, compared at the plan bit width. */
	for (j = 0; j < NCHAIN && first_diff < 0; j++) {
		got = slots[1 + SLOT_V0 + j] & mask;
		want = m->value[j] & mask;
		if (got != want)
			first_diff = SLOT_V0 + j;
	}
	/* Memory operand pool must be untouched. */
	for (j = SLOT_POOL0; j <= SLOT_POOL3 && first_diff < 0; j++) {
		got = slots[1 + j];
		want = (j == SLOT_POOL3) ? p->in_b : p->opval[j - SLOT_POOL0];
		if (got != want)
			first_diff = j;
	}

	sljit_free_code(code, NULL);

	if (resumes_out != NULL)
		*resumes_out = ctx.resumes;
	if (ncycles != 0) {
		for (j = 0; j < ncycles; j++) {
			if (cycles[j] >= 0 && cycles[j] < CP_NBOUND)
				st->resume_at[cycles[j]]++;
		}
	}

	if (first_diff >= 0) {
		print_diag(group, index, p, m, slots,
			ncycles ? cycles[0] : CP_AFTER_ENTER, ctx.resumes,
			first_diff, got, want);
		TASK_CHECK(0);
		return -1;
	}

	TASK_CHECK(slots[CANARY_IN] == CANARY_MAGIC);
	TASK_CHECK(slots[CANARY_OUT] == CANARY_MAGIC);
	for (j = 0; j < NCHAIN; j++)
		TASK_CHECK((slots[1 + SLOT_C0 + j] & mask) == (m->materialized[j] & mask));
	TASK_CHECK((slots[1 + SLOT_BRANCH] & mask) == (m->branch_value & mask));
	TASK_CHECK((slots[1 + SLOT_ACC] & mask) == (m->acc & mask));
	for (j = 0; j < NCHAIN; j++)
		TASK_CHECK((slots[1 + SLOT_V0 + j] & mask) == (m->value[j] & mask));
	return 1;
}

/* --------------------------------------------------------------------- */
/* Group 1: independent model coverage and one-shot compilation          */
/* --------------------------------------------------------------------- */

#define PLAN_COUNT 160
#define FIXED_SEED 0x43484B50ul /* "CHKP" */

int contract_model(void)
{
	struct stats st;
	int i;

	memset(&st, 0, sizeof(st));
	for (i = 0; i < PLAN_COUNT; i++) {
		struct plan p = make_plan((sljit_u32)(FIXED_SEED + (unsigned)i * 2654435761u));
		struct model_result m = run_model(&p);
		int no_cycle = 0;
		int rc;

		account_plan(&st, &p, &m);
		print_encoding(i, &p);
		rc = build_run_compare("model", i, &p, &m, &no_cycle, 0, NULL, &st);
		if (rc < 0)
			return -1;
		if (rc == 0) {
			st.runtime_failures++;
			continue;
		}
		st.direct_runs++;
	}

	/* Both incoming carry values must actually have been exercised. */
	TASK_CHECK(st.carry_zero > 0);
	TASK_CHECK(st.carry_one > 0);
	/* Both consumer shapes and both flag families are represented. */
	TASK_CHECK(st.tail_branch > 0 && st.tail_materialize > 0);
	TASK_CHECK(st.family_add > 0 && st.family_sub > 0);
	TASK_CHECK(st.form_reg > 0 && st.form_imm > 0 && st.form_mem > 0);
	TASK_CHECK(st.width32 > 0 && st.width64 > 0);

	print_stats("model", &st);
	if (st.runtime_failures == st.plans)
		return -2;
	return st.plans - st.runtime_failures;
}

/* --------------------------------------------------------------------- */
/* Group 2: save / free / restore between flag producer and consumers    */
/* --------------------------------------------------------------------- */

static int resume_boundaries_for(const struct plan *p, int *out)
{
	int n = 0;
	out[n++] = CP_PREMAT0;
	out[n++] = CP_PREMAT1;
	out[n++] = CP_PREMAT2;
	if (p->branch_tail)
		out[n++] = CP_PREJUMP;
	return n;
}

int contract_resume(void)
{
	struct stats st;
	int i;
	int executed = 0;

	memset(&st, 0, sizeof(st));
	for (i = 0; i < PLAN_COUNT; i++) {
		struct plan p = make_plan((sljit_u32)(FIXED_SEED + (unsigned)i * 2654435761u));
		struct model_result m = run_model(&p);
		int boundaries[MAX_CYCLES];
		int nb;
		int k;
		int resumes = 0;
		int rc;

		account_plan(&st, &p, &m);

		/* One save/free/restore cycle at every legal flag-live boundary. */
		nb = resume_boundaries_for(&p, boundaries);
		for (k = 0; k < nb; k++) {
			rc = build_run_compare("resume", i, &p, &m, &boundaries[k], 1,
				&resumes, &st);
			if (rc < 0)
				return -1;
			if (rc == 0) {
				st.runtime_failures++;
				continue;
			}
			TASK_CHECK(resumes == 1);
			st.resume_runs++;
			executed++;
		}

		/* Repeated checkpoints of the same plan at several positions. */
		{
			int multi[3] = { CP_PREMAT0, CP_PREMAT1, CP_PREMAT2 };
			rc = build_run_compare("resume", i, &p, &m, multi, 3,
				&resumes, &st);
			if (rc < 0)
				return -1;
			if (rc == 0) {
				st.runtime_failures++;
				continue;
			}
			TASK_CHECK(resumes == 3);
			st.multi_runs++;
			executed++;
		}
	}

	TASK_CHECK(st.resume_runs > 0);
	TASK_CHECK(st.multi_runs > 0);
	TASK_CHECK(st.resume_at[CP_PREMAT0] > 0);
	TASK_CHECK(st.resume_at[CP_PREMAT2] > 0);
	print_stats("resume", &st);
	if (executed == 0)
		return -2;
	return executed;
}

/* --------------------------------------------------------------------- */
/* Group 3: object lifetime and cross-compiler pointer rules             */
/* --------------------------------------------------------------------- */

/*
 * Builds the shared prefix through the step 0 arithmetic + flags preserving
 * MOV, so the restored compilers start with live, defined status flags.
 */
static int emit_prefix(struct sljit_compiler *c, const struct plan *p)
{
	sljit_s32 chain_regs[NCHAIN] = { REG_CHAIN0, REG_CHAIN1, REG_CHAIN2 };

	if (sljit_emit_enter(c, 0, SLJIT_ARGS1V(P), 12, 3, 0) != SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);
	if (emit_width_mov(c, p, REG_IN_A, 0, SLJIT_IMM, (sljit_sw)p->in_a)
			!= SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);
	if (emit_width_mov(c, p, REG_IN_B, 0, SLJIT_IMM, (sljit_sw)p->in_b)
			!= SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);
	if (emit_width_mov(c, p, REG_POOL, 0, SLJIT_MEM1(SLJIT_S0),
			(sljit_sw)(SLOT_POOL3 * sizeof(sljit_sw))) != SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);
	if (emit_arith(c, p, 0) != SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);
	if (emit_width_mov(c, p, REG_MOVTMP, 0, chain_regs[0], 0) != SLJIT_SUCCESS)
		return sljit_get_compiler_error(c);
	return SLJIT_SUCCESS;
}

static int check_model_slots(const sljit_uw *slots, const struct plan *p,
	const struct model_result *m)
{
	sljit_uw mask = width_mask(p->width32);
	int j;

	if (slots[CANARY_IN] != CANARY_MAGIC || slots[CANARY_OUT] != CANARY_MAGIC)
		return 0;
	for (j = 0; j < NCHAIN; j++) {
		if ((slots[1 + SLOT_C0 + j] & mask) != (m->materialized[j] & mask))
			return 0;
		if ((slots[1 + SLOT_V0 + j] & mask) != (m->value[j] & mask))
			return 0;
	}
	if ((slots[1 + SLOT_BRANCH] & mask) != (m->branch_value & mask))
		return 0;
	if ((slots[1 + SLOT_ACC] & mask) != (m->acc & mask))
		return 0;
	return 1;
}

/* One serialization restored twice; each compiler gets its own legal tail. */
static int lifetime_two_restores(const struct plan *p, const struct model_result *m)
{
	struct sljit_compiler *prefix;
	struct sljit_compiler *ca;
	struct sljit_compiler *cb;
	struct emit_ctx ctx_a;
	struct emit_ctx ctx_b;
	sljit_uw *buf;
	sljit_uw size;
	sljit_uw slots_a[NALLOC];
	sljit_uw slots_b[NALLOC];
	sljit_uw mask = width_mask(p->width32);
	void *code_a;
	void *code_b;
	plan_func_t fa;
	plan_func_t fb;
	sljit_s32 marker_op = p->width32 ? SLJIT_MOV_U32 : SLJIT_MOV;

	prefix = sljit_create_compiler(NULL);
	if (prefix == NULL)
		return 0;
	if (emit_prefix(prefix, p) != SLJIT_SUCCESS) {
		sljit_free_compiler(prefix);
		return 0;
	}

	/* A single saved image is the source of both restored compilers. */
	buf = sljit_serialize_compiler(prefix, 0, &size);
	if (buf == NULL) {
		sljit_free_compiler(prefix);
		return 0;
	}
	sljit_free_compiler(prefix);
	ca = sljit_deserialize_compiler(buf, size, 0, NULL);
	cb = sljit_deserialize_compiler(buf, size, 0, NULL);
	/* The serialization buffer is released before either compiler is used. */
	SLJIT_FREE(buf, NULL);
	if (ca == NULL || cb == NULL) {
		if (ca != NULL)
			sljit_free_compiler(ca);
		if (cb != NULL)
			sljit_free_compiler(cb);
		return 0;
	}

	/* Same shared suffix, then two different legal marker tails. */
	memset(&ctx_a, 0, sizeof(ctx_a));
	ctx_a.compiler = ca;
	memset(&ctx_b, 0, sizeof(ctx_b));
	ctx_b.compiler = cb;
	if (emit_suffix(&ctx_a, p, 0) != SLJIT_SUCCESS
		|| emit_suffix(&ctx_b, p, 0) != SLJIT_SUCCESS) {
		sljit_free_compiler(ca);
		sljit_free_compiler(cb);
		return 0;
	}

	if (sljit_emit_op1(ca, marker_op, SLJIT_MEM1(SLJIT_S0),
			(sljit_sw)(SLOT_POOL3 * sizeof(sljit_sw)),
			SLJIT_IMM, (sljit_sw)(0x1111u & mask)) != SLJIT_SUCCESS
		|| sljit_emit_return_void(ca) != SLJIT_SUCCESS
		|| sljit_emit_op1(cb, marker_op, SLJIT_MEM1(SLJIT_S0),
			(sljit_sw)(SLOT_POOL3 * sizeof(sljit_sw)),
			SLJIT_IMM, (sljit_sw)(0x2222u & mask)) != SLJIT_SUCCESS
		|| sljit_emit_return_void(cb) != SLJIT_SUCCESS) {
		sljit_free_compiler(ca);
		sljit_free_compiler(cb);
		return 0;
	}

	code_a = sljit_generate_code(ca, 0, NULL);
	code_b = sljit_generate_code(cb, 0, NULL);
	if (code_a == NULL || sljit_get_compiler_error(ca) != SLJIT_ERR_COMPILED
		|| code_b == NULL || sljit_get_compiler_error(cb) != SLJIT_ERR_COMPILED) {
		if (code_a != NULL)
			sljit_free_code(code_a, NULL);
		if (code_b != NULL)
			sljit_free_code(code_b, NULL);
		sljit_free_compiler(ca);
		sljit_free_compiler(cb);
		return 0;
	}
	sljit_free_compiler(ca);
	sljit_free_compiler(cb);

	init_slots(slots_a, p);
	init_slots(slots_b, p);
	fa = (plan_func_t)code_a;
	fb = (plan_func_t)code_b;
	fa((sljit_sw)&slots_a[1]);

	/* Freeing the first finished code must not affect the second. */
	sljit_free_code(code_a, NULL);
	fb((sljit_sw)&slots_b[1]);
	sljit_free_code(code_b, NULL);

	/* The shared prefix + suffix produce the model result in both. */
	TASK_CHECK(check_model_slots(slots_a, p, m));
	TASK_CHECK(check_model_slots(slots_b, p, m));
	/* The two distinct tails wrote different values to their own slots. */
	TASK_CHECK((slots_a[1 + SLOT_POOL3] & mask) == (0x1111u & mask));
	TASK_CHECK((slots_b[1 + SLOT_POOL3] & mask) == (0x2222u & mask));
	/* Canaries in both independent C side allocations. */
	TASK_CHECK(slots_a[CANARY_IN] == CANARY_MAGIC);
	TASK_CHECK(slots_a[CANARY_OUT] == CANARY_MAGIC);
	TASK_CHECK(slots_b[CANARY_IN] == CANARY_MAGIC);
	TASK_CHECK(slots_b[CANARY_OUT] == CANARY_MAGIC);
	return 1;
}

/*
 * The conditional jump is created before the checkpoint and left unlabeled.
 * After restore the old jump pointer belongs to a freed compiler, so both the
 * jump and the merge label are re-acquired through the public enumeration /
 * index API of the new compiler and bound there.
 */
static int lifetime_jump_pointers(void)
{
	struct sljit_compiler *c;
	struct sljit_jump *jump;
	struct sljit_jump *join;
	struct sljit_label *label_take;
	struct sljit_label *label_end;
	sljit_uw *buf;
	sljit_uw size;
	sljit_uw slots[NALLOC];
	void *code;
	plan_func_t func;
	/* Chosen so the unsigned 64 bit ADD overflows and produces a carry. */
	sljit_uw a = ~(sljit_uw)0 - 4u;
	sljit_uw x = 6;
	sljit_uw fall = 0x3333;
	sljit_uw take = 0x4444;
	sljit_uw expected;
	sljit_uw mask = ~(sljit_uw)0;
	int i;

	c = sljit_create_compiler(NULL);
	if (c == NULL)
		return 0;
	if (sljit_emit_enter(c, 0, SLJIT_ARGS1V(P), 12, 3, 0) != SLJIT_SUCCESS
		|| sljit_emit_op1(c, SLJIT_MOV, REG_IN_A, 0, SLJIT_IMM, (sljit_sw)a)
			!= SLJIT_SUCCESS
		/* Flags producer; the consumer jump is emitted before any save. */
		|| sljit_emit_op2(c, SLJIT_ADD | SLJIT_SET_CARRY, REG_CHAIN0, 0,
			REG_IN_A, 0, SLJIT_IMM, (sljit_sw)x) != SLJIT_SUCCESS) {
		sljit_free_compiler(c);
		return 0;
	}
	jump = sljit_emit_jump(c, SLJIT_CARRY);
	if (jump == NULL) {
		sljit_free_compiler(c);
		return 0;
	}

	buf = sljit_serialize_compiler(c, 0, &size);
	if (buf == NULL) {
		sljit_free_compiler(c);
		return 0;
	}
	sljit_free_compiler(c);
	c = sljit_deserialize_compiler(buf, size, 0, NULL);
	SLJIT_FREE(buf, NULL);
	if (c == NULL)
		return 0;

	/* Re-acquire the conditional jump via enumeration, not the old pointer. */
	jump = sljit_get_first_jump(c);
	if (jump == NULL || sljit_get_next_jump(jump) != NULL) {
		sljit_free_compiler(c);
		return 0;
	}

	/* Not-taken path writes the fall value then jumps over the taken body. */
	if (sljit_emit_op1(c, SLJIT_MOV, REG_MOVTMP, 0, SLJIT_IMM, (sljit_sw)fall)
			!= SLJIT_SUCCESS) {
		sljit_free_compiler(c);
		return 0;
	}
	join = sljit_emit_jump(c, SLJIT_JUMP);
	if (join == NULL) {
		sljit_free_compiler(c);
		return 0;
	}

	/* Both labels belong to the restored compiler and are found by index. */
	label_take = sljit_emit_label(c);
	if (label_take == NULL || sljit_get_label_index(label_take) != 0) {
		sljit_free_compiler(c);
		return 0;
	}
	sljit_set_label(jump, label_take);
	if (sljit_emit_op1(c, SLJIT_MOV, REG_MOVTMP, 0, SLJIT_IMM, (sljit_sw)take)
			!= SLJIT_SUCCESS) {
		sljit_free_compiler(c);
		return 0;
	}
	label_end = sljit_emit_label(c);
	if (label_end == NULL || sljit_get_label_index(label_end) != 1) {
		sljit_free_compiler(c);
		return 0;
	}
	sljit_set_label(join, label_end);

	if (sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(SLJIT_S0),
			(sljit_sw)(SLOT_BRANCH * sizeof(sljit_sw)),
			REG_MOVTMP, 0) != SLJIT_SUCCESS
		|| sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(SLJIT_S0),
			(sljit_sw)(SLOT_V0 * sizeof(sljit_sw)),
			REG_CHAIN0, 0) != SLJIT_SUCCESS
		|| sljit_emit_return_void(c) != SLJIT_SUCCESS) {
		sljit_free_compiler(c);
		return 0;
	}

	code = sljit_generate_code(c, 0, NULL);
	if (code == NULL || sljit_get_compiler_error(c) != SLJIT_ERR_COMPILED) {
		if (code != NULL)
			sljit_free_code(code, NULL);
		sljit_free_compiler(c);
		return 0;
	}
	sljit_free_compiler(c);

	for (i = 0; i < NALLOC; i++)
		slots[i] = 0;
	slots[CANARY_IN] = CANARY_MAGIC;
	slots[CANARY_OUT] = CANARY_MAGIC;
	func = (plan_func_t)code;
	func((sljit_sw)&slots[1]);
	sljit_free_code(code, NULL);

	/* Independent expectation: unsigned ADD carry, no ARM condition code used. */
	expected = (a > mask - x) ? 1u : 0u;
	TASK_CHECK(expected == 1u);
	TASK_CHECK(slots[1 + SLOT_BRANCH] == (expected ? take : fall));
	TASK_CHECK(slots[1 + SLOT_V0] == ((a + x) & mask));
	TASK_CHECK(slots[CANARY_IN] == CANARY_MAGIC);
	TASK_CHECK(slots[CANARY_OUT] == CANARY_MAGIC);
	return 1;
}

int contract_lifetime(void)
{
	struct stats st;
	int i;
	int ok_runs = 0;

	memset(&st, 0, sizeof(st));

	/*
	 * Alternate families and widths so the buffer-free and dual-restore
	 * exercises cover both carry kinds and both bit widths.
	 */
	for (i = 0; i < 8; i++) {
		struct plan p = make_plan((sljit_u32)(FIXED_SEED + 101u + (unsigned)i * 40503u));
		struct model_result m = run_model(&p);
		int cycles[1] = { CP_PREMAT0 };
		int resumes = 0;
		int rc;

		account_plan(&st, &p, &m);

		/* Restored compiler keeps running after the buffer is freed. */
		rc = build_run_compare("lifetime", i, &p, &m, cycles, 1, &resumes, &st);
		if (rc < 0)
			return -1;
		if (rc == 0) {
			st.runtime_failures++;
			continue;
		}
		TASK_CHECK(resumes == 1);
		st.lifetime_runs++;
		ok_runs++;

		rc = lifetime_two_restores(&p, &m);
		if (rc < 0)
			return -1;
		if (rc == 0) {
			st.runtime_failures++;
			continue;
		}
		st.lifetime_runs++;
		ok_runs++;
	}

	{
		int rc = lifetime_jump_pointers();
		if (rc < 0)
			return -1;
		if (rc == 0)
			st.runtime_failures++;
		else {
			st.lifetime_runs++;
			ok_runs++;
		}
	}

	TASK_CHECK(st.lifetime_runs > 0);
	print_stats("lifetime", &st);
	if (ok_runs == 0)
		return -2;
	return ok_runs;
}
