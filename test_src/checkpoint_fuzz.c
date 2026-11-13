/*
 *    Stack-less Just-In-Time compiler
 *
 *    Structured fuzz regression for compiler serialization checkpoints.
 *
 *    The exercise targets the continuation window where an arithmetic
 *    instruction has already been emitted (producing the CPU status flags,
 *    carry or borrow included) but the first flag consumer (an ADDC / SUBC
 *    chain, a condition materialization or a conditional jump) has not been
 *    emitted yet.  The same symbolic plan is exercised three ways:
 *
 *      1. direct compilation in a single compiler instance,
 *      2. compile / serialize / free / deserialize / continue at a flag
 *         live boundary,
 *      3. repeated save / restore cycles at several legal positions.
 *
 *    Every generated program is checked against an independent, purely
 *    unsigned integer model written in C.  No signed C overflow is used,
 *    no architecture condition code helpers or library flag classification
 *    functions participate in the expected values, and machine code bytes
 *    are never compared: only externally visible slot contents, branch
 *    results, carry chains and canaries are compared.
 *
 *    The generated code only touches slots explicitly allocated on the C
 *    side; input and output areas are surrounded by canaries.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sljitLir.h"
#include "../task_support/check.h"

/* --------------------------------------------------------------------- */
/*  Fixed dimensions and canaries                                        */
/* --------------------------------------------------------------------- */

#define PLAN_COUNT		192
#define MODEL_INPUTS		6
#define MODEL_OUTPUTS		7
#define MODEL_CANARY_MAGIC	((sljit_uw)0x534C4A54434B5054ULL) /* "SLJTCKPT" */
/* The reference model always works in 64-bit unsigned math and masks
   down to 32 bits for 32-bit plans; no signed C arithmetic is involved. */
typedef unsigned long long model_u64;

#define MODEL_OUT_FILL		((sljit_uw)0xccccccccccccccccULL)

/* Skeleton step indices (see emit_steps / model_run for the sequence). */
#define STEP_LOAD0		0
#define STEP_LOAD1		1
#define STEP_ACC_INIT		2
#define STEP_PROD1		3
#define STEP_MOV_PRESERVE	4
#define STEP_STORE_PRESERVE	5
#define STEP_CONS1		6
#define STEP_MAT1		7
#define STEP_CONS2		8
#define STEP_USE_MAT		9
#define STEP_STORE_CHAIN	10
#define STEP_PROD2		11
#define STEP_JCC		12
#define STEP_MAT2		13
#define STEP_CONS3		14
#define STEP_FINAL_STORES	15
#define STEP_COUNT		16

/*
 * Checkpoint positions.  A checkpoint at step N serializes after steps
 * [0,N) are emitted; the suffix starts at N.  Each value below names a
 * boundary where the flags produced by the last arithmetic step are still
 * live and the first consumer lies on the restored side of the boundary.
 */
#define HOT_CP_PROD1_WINDOW	STEP_MOV_PRESERVE	/* PROD1 done, CONS1 pending */
#define HOT_CP_MAT1_WINDOW	STEP_MAT1		/* CONS1 done, MAT1 pending  */
#define HOT_CP_JCC_WINDOW	STEP_JCC		/* PROD2 done, JCC pending   */
#define HOT_CP_MAT2_WINDOW	STEP_MAT2		/* JCC done, MAT2 pending    */

#define FAM_ADD			0
#define FAM_SUB			1

#define FORM_REG		0
#define FORM_IMM		1
#define FORM_MEM		2

struct ckpt_slots {
	sljit_uw canary_in;
	sljit_uw in[MODEL_INPUTS];
	sljit_uw canary_mid;
	sljit_uw out[MODEL_OUTPUTS];
	sljit_uw canary_out;
};

struct fuzz_plan {
	sljit_u32 id;
	sljit_u32 seed;
	int is_32;
	int fam1;			/* family of the first chain */
	int fam2;			/* family reselected before JCC */
	int form_prod1;		/* REG / IMM / MEM */
	int form_cons1;		/* REG / MEM */
	int form_cons2;		/* REG / MEM */
	int form_cons3;		/* REG / MEM */
	int cond;			/* SLJIT_CARRY or SLJIT_NOT_CARRY */
	sljit_s32 imm1;
	sljit_s32 imm2;
	model_u64 in[MODEL_INPUTS];
};

struct model_state {
	model_u64 r[6];		/* virtual R0..R5 */
	int flags_defined;
	int family;		/* FAM_ADD / FAM_SUB when defined */
	int z;			/* zero flag, 0/1 */
	int v;			/* variable flag: carry (ADD) or borrow (SUB) */
	model_u64 out[MODEL_OUTPUTS];
};

struct fuzz_stats {
	unsigned plans;
	unsigned width32, width64;
	unsigned add_producers, sub_producers;
	unsigned addc_ops, subc_ops;
	unsigned mat_consumers, jcc_consumers;
	unsigned carry_in0, carry_in1;
	unsigned borrow_in0, borrow_in1;
	unsigned reg_ops, imm_ops, mem_ops;
	unsigned resume_runs;
	unsigned cp_position[STEP_COUNT + 1];
	unsigned buffer_freed_before_continue;
	unsigned double_deserialize;
	unsigned label_refetch;
};

static struct fuzz_stats global_stats;

static void ckpt_fuzz_print_stats(void);

/* --------------------------------------------------------------------- */
/*  Deterministic PRNG and plan generation                               */
/* --------------------------------------------------------------------- */

#define LCG_BASE_SEED		0x434b5054u /* "CKPT" */

static sljit_u32 rng_state;

static sljit_u32 rng_next(void)
{
	rng_state = rng_state * 1103515245u + 12345u;
	return rng_state >> 16;
}

static model_u64 width_mask(const struct fuzz_plan *p)
{
	return p->is_32 ? 0xffffffffULL : 0xffffffffffffffffULL;
}

static model_u64 edge_value(sljit_u32 r, model_u64 mask)
{
	switch (r % 8) {
	case 0:
		return 0;
	case 1:
		return 1;
	case 2:
		return mask;
	case 3:
		return mask - 1;
	case 4:
		return 2;
	case 5:
		return mask & 0x5555555555555555ULL;
	case 6:
		return mask & 0xaaaaaaaaaaaaaaaaULL;
	default:
		return mask & (((model_u64)r << 32) ^ (model_u64)r * 2654435761u);
	}
}

static void plan_init(struct fuzz_plan *p, sljit_u32 id)
{
	model_u64 mask;
	int i;

	memset(p, 0, sizeof(*p));
	p->id = id;
	p->seed = LCG_BASE_SEED ^ (id * 0x9e3779b9u) ^ 0x13572468u;
	rng_state = p->seed;

	p->is_32 = (id & 1) != 0;
	mask = width_mask(p);

	p->fam1 = (int)(rng_next() & 1);
	p->fam2 = (int)(rng_next() & 1);
	p->form_prod1 = (int)(rng_next() % 3);
	p->form_cons1 = (int)(rng_next() & 1);
	p->form_cons2 = (int)(rng_next() & 1);
	p->form_cons3 = (int)(rng_next() & 1);
	p->cond = (rng_next() & 1) ? SLJIT_CARRY : SLJIT_NOT_CARRY;

	/* Small, always representable ARM64 ADD/SUB immediates. */
	p->imm1 = (sljit_s32)(rng_next() % 2000) + 1;
	p->imm2 = (sljit_s32)(rng_next() % 2000) + 1;

	for (i = 0; i < MODEL_INPUTS; i++)
		p->in[i] = edge_value(rng_next(), mask) & mask;

	/* Keep every input within the modelled width. */
	for (i = 0; i < MODEL_INPUTS; i++)
		p->in[i] &= mask;
}

static void plan_print_encoding(const struct fuzz_plan *p)
{
	printf("PLAN id=%u seed=0x%08x width=%u fam1=%d fam2=%d forms=%d%d%d%d cond=%s imm=%d,%d in=",
		p->id, p->seed, p->is_32 ? 32u : (unsigned)(sizeof(sljit_uw) * 8),
		p->fam1, p->fam2, p->form_prod1, p->form_cons1, p->form_cons2,
		p->form_cons3, p->cond == SLJIT_CARRY ? "CARRY" : "NOT_CARRY",
		(int)p->imm1, (int)p->imm2);
	for (int i = 0; i < MODEL_INPUTS; i++)
		printf("%s0x%llx", i ? "," : "", (unsigned long long)p->in[i]);
	printf("\n");
}

/* --------------------------------------------------------------------- */
/*  Independent unsigned integer model                                   */
/* --------------------------------------------------------------------- */

/* The reference model must never read an undefined flag state, mirroring
   the rule that a condition / carry consumer needs a defined producer. */
static void model_require_flags(const struct model_state *m)
{
	TASK_CHECK(m->flags_defined);
}

static int eval_condition(const struct model_state *m, int cond)
{
	/* Only CARRY / NOT_CARRY consumers are generated; v already holds the
	   sljit semantic: carry for ADD family, borrow for SUB family. */
	int taken_cond;
	model_require_flags(m);
	taken_cond = cond == SLJIT_CARRY ? m->v : (1 - m->v);
	return taken_cond;
}

/*
 * Modular arithmetic primitives.  Every operation stays inside the target
 * width: operands are masked and intermediate sums/differences are reduced
 * modulo 2^bits.  Carry and borrow are recovered from unsigned comparisons
 * of the reduced values, so neither signed C arithmetic nor a wider-than-
 * mask integer type is ever needed.
 */
static model_u64 mod_add(model_u64 a, model_u64 b, model_u64 mask, int *carry)
{
	model_u64 r = (a + b) & mask;
	*carry = r < a;			/* wrapped past the modulus */
	return r;
}

static model_u64 mod_add_carry(model_u64 a, model_u64 b, int cin,
	model_u64 mask, int *carry)
{
	model_u64 t = (a + b) & mask;
	model_u64 r = (t + (model_u64)cin) & mask;
	*carry = (t < a) || (cin && t == mask);
	return r;
}

static model_u64 mod_sub(model_u64 a, model_u64 b, model_u64 mask, int *borrow)
{
	model_u64 r = (a - b) & mask;
	*borrow = a < b;
	return r;
}

static model_u64 mod_sub_borrow(model_u64 a, model_u64 b, int bin,
	model_u64 mask, int *borrow)
{
	model_u64 t = (a - b) & mask;
	model_u64 r = (t - (model_u64)bin) & mask;
	*borrow = (a < b) || (bin && t == 0);
	return r;
}

static void model_run(const struct fuzz_plan *p, struct model_state *m)
{
	const model_u64 mask = width_mask(p);
	model_u64 a, b;

	memset(m, 0, sizeof(*m));

	/*
	 * Virtual register file: R0=chain accumulator, R1=condition
	 * accumulator, R2=flag-preserving MOV copy then PROD2 target,
	 * R3=materialized condition, R4=loaded in[2] used as register
	 * operand of the chained consumers, R5=lifetime tail B only.
	 */

	/* STEP_LOAD0 / STEP_LOAD1 */
	m->r[0] = p->in[0] & mask;
	m->r[4] = p->in[2] & mask;

	/* STEP_ACC_INIT */
	m->r[1] = 0;

	/* STEP_PROD1: R0 = R0 <op> operand, SET_CARRY | SET_Z.  For ADD the
	   variable flag is carry-out; for SUB it is borrow, both derived by
	   unsigned comparison inside the modular helpers. */
	if (p->form_prod1 == FORM_REG)
		b = m->r[4];
	else if (p->form_prod1 == FORM_IMM)
		b = (model_u64)(p->imm1) & mask;
	else
		b = p->in[1] & mask;
	a = m->r[0];
	if (p->fam1 == FAM_ADD)
		m->r[0] = mod_add(a, b, mask, &m->v);
	else
		m->r[0] = mod_sub(a, b, mask, &m->v);
	m->family = p->fam1;
	m->z = m->r[0] == 0;
	m->flags_defined = 1;

	/* STEP_MOV_PRESERVE: R2 = R0 (MOV must preserve the flags). */
	m->r[2] = m->r[0];

	/* STEP_STORE_PRESERVE: out6 = R2 (flags preserved). */
	m->out[6] = m->r[2];

	/* STEP_CONS1: first chained consumer, fed by the PROD1 flag. */
	model_require_flags(m);
	b = p->form_cons1 == FORM_REG ? m->r[4] : (p->in[3] & mask);
	a = m->r[0];
	if (m->family == FAM_ADD) {
		m->r[0] = mod_add_carry(a, b, m->v, mask, &m->v);
		if (m->v)
			global_stats.carry_in1++;
		else
			global_stats.carry_in0++;
	} else {
		m->r[0] = mod_sub_borrow(a, b, m->v, mask, &m->v);
		if (m->v)
			global_stats.borrow_in1++;
		else
			global_stats.borrow_in0++;
	}
	m->z = m->r[0] == 0;
	m->flags_defined = 1;

	/* STEP_MAT1: R3 = materialized condition, flags unchanged. */
	m->r[3] = (model_u64)eval_condition(m, p->cond);

	/* STEP_CONS2: second chained consumer, the chain is now three deep. */
	model_require_flags(m);
	b = p->form_cons2 == FORM_REG ? m->r[4] : (p->in[4] & mask);
	a = m->r[0];
	if (m->family == FAM_ADD)
		m->r[0] = mod_add_carry(a, b, m->v, mask, &m->v);
	else
		m->r[0] = mod_sub_borrow(a, b, m->v, mask, &m->v);
	m->z = m->r[0] == 0;

	/* STEP_USE_MAT: accumulator consumes the materialized condition.
	   Plain ADD without requested flags: model marks flags undefined. */
	m->r[1] = (m->r[1] + m->r[3]) & mask;
	m->flags_defined = 0;

	/* STEP_STORE_CHAIN: out0 = R0. */
	m->out[0] = m->r[0];

	/* STEP_PROD2: R2 = R2 <op> imm2, SET_CARRY | SET_Z. */
	a = m->r[2];
	b = (model_u64)p->imm2 & mask;
	if (p->fam2 == FAM_ADD)
		m->r[2] = mod_add(a, b, mask, &m->v);
	else
		m->r[2] = mod_sub(a, b, mask, &m->v);
	m->family = p->fam2;
	m->z = m->r[2] == 0;
	m->flags_defined = 1;

	/* STEP_JCC: conditional branch writes distinct constants. */
	m->out[1] = eval_condition(m, p->cond) ? 0xa7ULL : 0x51ULL;

	/* STEP_MAT2: materialize again (flags survive jumps and stores). */
	m->r[3] = (model_u64)eval_condition(m, p->cond);
	m->out[2] = m->r[3];

	/* STEP_CONS3: consumer matching the second family. */
	model_require_flags(m);
	b = p->form_cons3 == FORM_REG ? m->r[4] : (p->in[5] & mask);
	a = m->r[0];
	if (m->family == FAM_ADD)
		m->r[0] = mod_add_carry(a, b, m->v, mask, &m->v);
	else
		m->r[0] = mod_sub_borrow(a, b, m->v, mask, &m->v);
	m->out[3] = m->r[0];

	/* STEP_FINAL_STORES. */
	m->out[4] = m->r[1];
	m->out[5] = m->r[3];
}

/* --------------------------------------------------------------------- */
/*  Emission of the symbolic plan                                        */
/* --------------------------------------------------------------------- */

typedef void (SLJIT_FUNC *jit_entry_fn)(void *ptr);

static sljit_s32 mov_op(const struct fuzz_plan *p)
{
	return p->is_32 ? SLJIT_MOV32 : SLJIT_MOV;
}

static sljit_s32 add_op(const struct fuzz_plan *p)
{
	return p->is_32 ? SLJIT_ADD32 : SLJIT_ADD;
}

static sljit_s32 sub_op(const struct fuzz_plan *p)
{
	return p->is_32 ? SLJIT_SUB32 : SLJIT_SUB;
}

static sljit_s32 addc_op(const struct fuzz_plan *p)
{
	return p->is_32 ? SLJIT_ADDC32 : SLJIT_ADDC;
}

static sljit_s32 subc_op(const struct fuzz_plan *p)
{
	return p->is_32 ? SLJIT_SUBC32 : SLJIT_SUBC;
}

static sljit_sw in_offset(int idx)
{
	return (sljit_sw)SLJIT_OFFSETOF(struct ckpt_slots, in[idx]);
}

static sljit_sw out_offset(int idx)
{
	return (sljit_sw)SLJIT_OFFSETOF(struct ckpt_slots, out[idx]);
}

/* Emit the second operand of a producer according to the selected form. */
static sljit_s32 emit_producer_src2(struct sljit_compiler *c, const struct fuzz_plan *p,
	int form, sljit_s32 *src, sljit_sw *srcw)
{
	if (form == FORM_REG) {
		*src = SLJIT_R4;
		*srcw = 0;
		global_stats.reg_ops++;
	} else if (form == FORM_IMM) {
		*src = SLJIT_IMM;
		*srcw = p->imm1;
		global_stats.imm_ops++;
	} else {
		*src = SLJIT_MEM1(SLJIT_S0);
		*srcw = in_offset(1);
		global_stats.mem_ops++;
	}
	return sljit_get_compiler_error(c);
}

static sljit_s32 emit_consumer_src2(struct sljit_compiler *c,
	int form, int input_idx, sljit_s32 *src, sljit_sw *srcw)
{
	if (form == FORM_REG) {
		*src = SLJIT_R4;
		*srcw = 0;
		global_stats.reg_ops++;
	} else {
		*src = SLJIT_MEM1(SLJIT_S0);
		*srcw = in_offset(input_idx);
		global_stats.mem_ops++;
	}
	return sljit_get_compiler_error(c);
}

/* Re-declare the carry/zero flags after an sljit_emit_label, which
   conservatively marks flags as unknown at a control flow merge.  Jumps,
   MOVs and stores do not modify the hardware flags, so the producer's
   flags are still live on both paths. */
static void declare_live_flags(struct sljit_compiler *c, const struct fuzz_plan *p, int family)
{
	sljit_s32 f = SLJIT_SET_CARRY | SLJIT_SET_Z
		| (family == FAM_ADD ? SLJIT_CURRENT_FLAGS_ADD : SLJIT_CURRENT_FLAGS_SUB);
	if (p->is_32)
		f |= SLJIT_CURRENT_FLAGS_32;
	sljit_set_current_flags(c, f);
}

/* Emit steps [from, to). The JCC step is atomic: its jump is fully linked. */
static sljit_s32 emit_steps(struct sljit_compiler *c, const struct fuzz_plan *p, int from, int to)
{
	sljit_s32 src;
	sljit_sw srcw;
	struct sljit_jump *jcc, *jend;
	struct sljit_label *lyes, *lend;
	int i;

	for (i = from; i < to; i++) {
		switch (i) {
		case STEP_LOAD0:
			if (sljit_emit_enter(c, 0, SLJIT_ARGS1V(P), 6, 1, 0) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			if (sljit_emit_op1(c, mov_op(p), SLJIT_R0, 0,
				SLJIT_MEM1(SLJIT_S0), in_offset(0)) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			break;
		case STEP_LOAD1:
			if (sljit_emit_op1(c, mov_op(p), SLJIT_R4, 0,
				SLJIT_MEM1(SLJIT_S0), in_offset(2)) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			break;
		case STEP_ACC_INIT:
			if (sljit_emit_op1(c, mov_op(p), SLJIT_R1, 0,
				SLJIT_IMM, 0) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			break;
		case STEP_PROD1:
			if (emit_producer_src2(c, p, p->form_prod1, &src, &srcw) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			if (p->fam1 == FAM_ADD) {
				global_stats.add_producers++;
				if (sljit_emit_op2(c, add_op(p) | SLJIT_SET_CARRY | SLJIT_SET_Z,
					SLJIT_R0, 0, SLJIT_R0, 0, src, srcw) != SLJIT_SUCCESS)
					return sljit_get_compiler_error(c);
			} else {
				global_stats.sub_producers++;
				if (sljit_emit_op2(c, sub_op(p) | SLJIT_SET_CARRY | SLJIT_SET_Z,
					SLJIT_R0, 0, SLJIT_R0, 0, src, srcw) != SLJIT_SUCCESS)
					return sljit_get_compiler_error(c);
			}
			break;
		case STEP_MOV_PRESERVE:
			/* MOV must leave the status flags untouched. */
			if (sljit_emit_op1(c, mov_op(p), SLJIT_R2, 0,
				SLJIT_R0, 0) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			break;
		case STEP_STORE_PRESERVE:
			if (sljit_emit_op1(c, mov_op(p), SLJIT_MEM1(SLJIT_S0), out_offset(6),
				SLJIT_R2, 0) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			break;
		case STEP_CONS1:
			if (emit_consumer_src2(c, p->form_cons1, 3, &src, &srcw) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			if (p->fam1 == FAM_ADD) {
				global_stats.addc_ops++;
				if (sljit_emit_op2(c, addc_op(p) | SLJIT_SET_CARRY, SLJIT_R0, 0,
					SLJIT_R0, 0, src, srcw) != SLJIT_SUCCESS)
					return sljit_get_compiler_error(c);
			} else {
				global_stats.subc_ops++;
				if (sljit_emit_op2(c, subc_op(p) | SLJIT_SET_CARRY, SLJIT_R0, 0,
					SLJIT_R0, 0, src, srcw) != SLJIT_SUCCESS)
					return sljit_get_compiler_error(c);
			}
			break;
		case STEP_MAT1:
			global_stats.mat_consumers++;
			if (sljit_emit_op_flags(c, mov_op(p), SLJIT_R3, 0, p->cond) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			break;
		case STEP_CONS2:
			if (emit_consumer_src2(c, p->form_cons2, 4, &src, &srcw) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			if (p->fam1 == FAM_ADD) {
				global_stats.addc_ops++;
				if (sljit_emit_op2(c, addc_op(p) | SLJIT_SET_CARRY, SLJIT_R0, 0,
					SLJIT_R0, 0, src, srcw) != SLJIT_SUCCESS)
					return sljit_get_compiler_error(c);
			} else {
				global_stats.subc_ops++;
				if (sljit_emit_op2(c, subc_op(p) | SLJIT_SET_CARRY, SLJIT_R0, 0,
					SLJIT_R0, 0, src, srcw) != SLJIT_SUCCESS)
					return sljit_get_compiler_error(c);
			}
			break;
		case STEP_USE_MAT:
			if (sljit_emit_op2(c, add_op(p), SLJIT_R1, 0,
				SLJIT_R1, 0, SLJIT_R3, 0) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			break;
		case STEP_STORE_CHAIN:
			if (sljit_emit_op1(c, mov_op(p), SLJIT_MEM1(SLJIT_S0), out_offset(0),
				SLJIT_R0, 0) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			break;
		case STEP_PROD2:
			if (p->fam2 == FAM_ADD) {
				global_stats.add_producers++;
				if (sljit_emit_op2(c, add_op(p) | SLJIT_SET_CARRY | SLJIT_SET_Z,
					SLJIT_R2, 0, SLJIT_R2, 0,
					SLJIT_IMM, p->imm2) != SLJIT_SUCCESS)
					return sljit_get_compiler_error(c);
			} else {
				global_stats.sub_producers++;
				if (sljit_emit_op2(c, sub_op(p) | SLJIT_SET_CARRY | SLJIT_SET_Z,
					SLJIT_R2, 0, SLJIT_R2, 0,
					SLJIT_IMM, p->imm2) != SLJIT_SUCCESS)
					return sljit_get_compiler_error(c);
			}
			break;
		case STEP_JCC: {
			global_stats.jcc_consumers++;
			jcc = sljit_emit_jump(c, p->cond);
			if (jcc == NULL)
				return sljit_get_compiler_error(c);
			/* Fall-through: condition false. */
			if (sljit_emit_op1(c, mov_op(p), SLJIT_MEM1(SLJIT_S0), out_offset(1),
				SLJIT_IMM, 0x51) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			jend = sljit_emit_jump(c, SLJIT_JUMP);
			if (jend == NULL)
				return sljit_get_compiler_error(c);
			lyes = sljit_emit_label(c);
			if (lyes == NULL)
				return sljit_get_compiler_error(c);
			sljit_set_label(jcc, lyes);
			if (sljit_emit_op1(c, mov_op(p), SLJIT_MEM1(SLJIT_S0), out_offset(1),
				SLJIT_IMM, 0xa7) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			lend = sljit_emit_label(c);
			if (lend == NULL)
				return sljit_get_compiler_error(c);
			sljit_set_label(jend, lend);
			/* Both merge paths only executed jumps and flag
			   preserving MOV/stores, so PROD2 flags are live. */
			declare_live_flags(c, p, p->fam2);
			break;
		}
		case STEP_MAT2:
			global_stats.mat_consumers++;
			if (sljit_emit_op_flags(c, mov_op(p), SLJIT_R3, 0, p->cond) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			if (sljit_emit_op1(c, mov_op(p), SLJIT_MEM1(SLJIT_S0), out_offset(2),
				SLJIT_R3, 0) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			break;
		case STEP_CONS3:
			if (emit_consumer_src2(c, p->form_cons3, 5, &src, &srcw) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			if (p->fam2 == FAM_ADD) {
				global_stats.addc_ops++;
				if (sljit_emit_op2(c, addc_op(p) | SLJIT_SET_CARRY, SLJIT_R0, 0,
					SLJIT_R0, 0, src, srcw) != SLJIT_SUCCESS)
					return sljit_get_compiler_error(c);
			} else {
				global_stats.subc_ops++;
				if (sljit_emit_op2(c, subc_op(p) | SLJIT_SET_CARRY, SLJIT_R0, 0,
					SLJIT_R0, 0, src, srcw) != SLJIT_SUCCESS)
					return sljit_get_compiler_error(c);
			}
			if (sljit_emit_op1(c, mov_op(p), SLJIT_MEM1(SLJIT_S0), out_offset(3),
				SLJIT_R0, 0) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			break;
		case STEP_FINAL_STORES:
			if (sljit_emit_op1(c, mov_op(p), SLJIT_MEM1(SLJIT_S0), out_offset(4),
				SLJIT_R1, 0) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			if (sljit_emit_op1(c, mov_op(p), SLJIT_MEM1(SLJIT_S0), out_offset(5),
				SLJIT_R3, 0) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			if (sljit_emit_return_void(c) != SLJIT_SUCCESS)
				return sljit_get_compiler_error(c);
			break;
		default:
			return SLJIT_ERR_BAD_ARGUMENT;
		}
	}
	return SLJIT_SUCCESS;
}

/* --------------------------------------------------------------------- */
/*  Slot management and output comparison                                */
/* --------------------------------------------------------------------- */

static void slots_init(struct ckpt_slots *s, const struct fuzz_plan *p)
{
	int i;

	s->canary_in = MODEL_CANARY_MAGIC;
	s->canary_mid = MODEL_CANARY_MAGIC;
	s->canary_out = MODEL_CANARY_MAGIC;
	for (i = 0; i < MODEL_INPUTS; i++)
		s->in[i] = (sljit_uw)p->in[i];
	for (i = 0; i < MODEL_OUTPUTS; i++)
		s->out[i] = MODEL_OUT_FILL;
}

static void print_mismatch(const struct fuzz_plan *p, int cp_pos, int resumes,
	const struct ckpt_slots *s, const struct model_state *m, int slot)
{
	model_u64 mask = width_mask(p);

	printf("MISMATCH plan_id=%u cp_pos=%d resumes=%d slot=%d got=0x%llx want=0x%llx\n",
		p->id, cp_pos, resumes, slot,
		(unsigned long long)(s->out[slot] & (sljit_uw)mask),
		(unsigned long long)(m->out[slot] & mask));
	plan_print_encoding(p);
	printf("INITIAL");
	for (int i = 0; i < MODEL_INPUTS; i++)
		printf(" in[%d]=0x%llx", i, (unsigned long long)p->in[i]);
	printf("\n");
	fflush(stdout);
}

/* Returns SLJIT_SUCCESS when every observable value matches the model. */
static int compare_run(const struct fuzz_plan *p, int cp_pos, int resumes,
	const struct ckpt_slots *s, const struct model_state *m)
{
	model_u64 mask = width_mask(p);
	int i;

	/* Canaries and the input area must be untouched. */
	TASK_CHECK(s->canary_in == MODEL_CANARY_MAGIC);
	TASK_CHECK(s->canary_mid == MODEL_CANARY_MAGIC);
	TASK_CHECK(s->canary_out == MODEL_CANARY_MAGIC);
	for (i = 0; i < MODEL_INPUTS; i++)
		TASK_CHECK(s->in[i] == (sljit_uw)p->in[i]);

	for (i = 0; i < MODEL_OUTPUTS; i++) {
		model_u64 got = (model_u64)s->out[i] & mask;
		model_u64 want = m->out[i] & mask;
		if (got != want) {
			print_mismatch(p, cp_pos, resumes, s, m, i);
			TASK_CHECK(0);
		}
	}
	return SLJIT_SUCCESS;
}

/* Compile a plan, optionally crossing one or more serialization points.
   cp_pos[] contains step boundaries; cp_count may be zero. */
static int compile_and_run(const struct fuzz_plan *p, const int *cp_pos, int cp_count,
	int free_buffer_early, struct ckpt_slots *slots)
{
	struct sljit_compiler *c;
	struct sljit_compiler *restored;
	sljit_uw *buf;
	sljit_uw buf_size;
	void *code;
	jit_entry_fn fn;
	int emitted = 0;
	int k;
	int resumes = 0;

	c = sljit_create_compiler(NULL);
	if (c == NULL) {
		printf("SETUP_FAILURE create_compiler plan=%u\n", p->id);
		return -1;
	}

	for (k = 0; k < cp_count; k++) {
		int target = cp_pos[k];
		if (emit_steps(c, p, emitted, target) != SLJIT_SUCCESS) {
			printf("SETUP_FAILURE emit_prefix plan=%u err=%d\n",
				p->id, (int)sljit_get_compiler_error(c));
			sljit_free_compiler(c);
			return -1;
		}
		emitted = target;

		buf = sljit_serialize_compiler(c, 0, &buf_size);
		if (buf == NULL) {
			printf("SETUP_FAILURE serialize plan=%u err=%d\n",
				p->id, (int)sljit_get_compiler_error(c));
			sljit_free_compiler(c);
			return -1;
		}
		sljit_free_compiler(c);

		restored = sljit_deserialize_compiler(buf, buf_size, 0, NULL);
		if (restored == NULL) {
			printf("SETUP_FAILURE deserialize plan=%u\n", p->id);
			SLJIT_FREE(buf, NULL);
			return -1;
		}
		if (free_buffer_early) {
			/* The resumed compiler must survive buffer release. */
			SLJIT_FREE(buf, NULL);
			buf = NULL;
			global_stats.buffer_freed_before_continue++;
		}
		c = restored;
		resumes++;
		global_stats.resume_runs++;
		global_stats.cp_position[target]++;
		if (buf != NULL) {
			SLJIT_FREE(buf, NULL);
			buf = NULL;
		}
	}

	if (emit_steps(c, p, emitted, STEP_COUNT) != SLJIT_SUCCESS) {
		printf("SETUP_FAILURE emit_suffix plan=%u err=%d\n",
			p->id, (int)sljit_get_compiler_error(c));
		sljit_free_compiler(c);
		return -1;
	}

	code = sljit_generate_code(c, 0, NULL);
	if (code == NULL) {
		printf("SETUP_FAILURE generate plan=%u err=%d\n",
			p->id, (int)sljit_get_compiler_error(c));
		sljit_free_compiler(c);
		return -1;
	}
	sljit_free_compiler(c);

	fn = (jit_entry_fn)code;
	fn(slots);
	sljit_free_code(code, NULL);
	return resumes;
}

/* --------------------------------------------------------------------- */
/*  Group 1: independent model versus direct compilation                  */
/* --------------------------------------------------------------------- */

int contract_model(void)
{
	struct fuzz_plan p;
	struct model_state m;
	struct ckpt_slots slots;
	unsigned only;
	unsigned i;
	int rc;
	const char *only_env = getenv("SLJIT_CKPT_ONLY");

	only = only_env ? (unsigned)strtoul(only_env, NULL, 0) : UINT32_MAX;

	for (i = 0; i < PLAN_COUNT; i++) {
		plan_init(&p, i);
		if (i != only && only != UINT32_MAX)
			continue;
		plan_print_encoding(&p);

		model_run(&p, &m);

		slots_init(&slots, &p);
		rc = compile_and_run(&p, NULL, 0, 0, &slots);
		if (rc < 0)
			return -1;
		if (compare_run(&p, -1, 0, &slots, &m) != SLJIT_SUCCESS)
			return -2;

		global_stats.plans++;
		global_stats.width32 += p.is_32 ? 1 : 0;
		global_stats.width64 += p.is_32 ? 0 : 1;
		if (only != UINT32_MAX)
			break;
	}

	if (global_stats.plans == 0) {
		printf("SETUP_FAILURE no plans selected\n");
		return -1;
	}
	ckpt_fuzz_print_stats();
	return (int)global_stats.plans;
}

/* --------------------------------------------------------------------- */
/*  Group 2: single and repeated serialization checkpoints                */
/* --------------------------------------------------------------------- */

static int resume_one_plan(const struct fuzz_plan *p, int print_encoding)
{
	struct model_state m;
	struct ckpt_slots slots;
	static const int hot_positions[] = {
		HOT_CP_PROD1_WINDOW, HOT_CP_MAT1_WINDOW,
		HOT_CP_JCC_WINDOW, HOT_CP_MAT2_WINDOW
	};
	int single_cp[1];
	int rc;
	unsigned k;

	if (print_encoding)
		plan_print_encoding(p);
	model_run(p, &m);

	/* Route B: one save / free / restore right before the JCC consumer.
	   This boundary crosses the status_flags_state that guides the
	   conditional jump condition code selection on ARM64. */
	single_cp[0] = HOT_CP_JCC_WINDOW;
	slots_init(&slots, p);
	rc = compile_and_run(p, single_cp, 1, 1, &slots);
	if (rc < 0)
		return -1;
	if (compare_run(p, HOT_CP_JCC_WINDOW, rc, &slots, &m) != SLJIT_SUCCESS)
		return -2;

	/* Route C: every hot boundary gets its own save / restore cycle,
	   including the window immediately before condition materialization. */
	for (k = 0; k < sizeof(hot_positions) / sizeof(hot_positions[0]); k++) {
		single_cp[0] = hot_positions[k];
		slots_init(&slots, p);
		rc = compile_and_run(p, single_cp, 1, k == 0, &slots);
		if (rc < 0)
			return -1;
		if (compare_run(p, hot_positions[k], rc, &slots, &m) != SLJIT_SUCCESS)
			return -2;
	}

	/* Route C repeated: two checkpoints in one compiler lifetime. The
	   first serialization buffer is released before the second cycle. */
	{
		int two_cp[2];
		two_cp[0] = HOT_CP_MAT1_WINDOW;
		two_cp[1] = HOT_CP_JCC_WINDOW;
		slots_init(&slots, p);
		rc = compile_and_run(p, two_cp, 2, 1, &slots);
		if (rc < 0)
			return -1;
		if (compare_run(p, HOT_CP_JCC_WINDOW, rc, &slots, &m) != SLJIT_SUCCESS)
			return -2;
	}

	return 0;
}

int contract_resume(void)
{
	struct fuzz_plan p;
	unsigned only;
	unsigned i;
	unsigned done = 0;
	const char *only_env = getenv("SLJIT_CKPT_ONLY");

	only = only_env ? (unsigned)strtoul(only_env, NULL, 0) : UINT32_MAX;

	for (i = 0; i < PLAN_COUNT; i++) {
		plan_init(&p, i);
		if (i != only && only != UINT32_MAX)
			continue;

		if (resume_one_plan(&p, done == 0) != 0)
			return -1;
		done++;
		global_stats.plans++;
		global_stats.width32 += p.is_32 ? 1 : 0;
		global_stats.width64 += p.is_32 ? 0 : 1;
		if (only != UINT32_MAX)
			break;
	}

	if (done == 0) {
		printf("SETUP_FAILURE no resume plans selected\n");
		return -1;
	}
	ckpt_fuzz_print_stats();
	return (int)done;
}

/* --------------------------------------------------------------------- */
/*  Group 3: lifetime and ownership rules                                  */
/* --------------------------------------------------------------------- */

/* One serialized buffer restored twice; each compiler gets a different
   legal tail.  Generated code and compilers are released independently. */
static int lifetime_double_deserialize(const struct fuzz_plan *p)
{
	struct sljit_compiler *c, *ca, *cb;
	sljit_uw *buf;
	sljit_uw buf_size;
	void *code_a, *code_b;
	struct ckpt_slots slots_a, slots_b;
	struct model_state m;
	jit_entry_fn fn;

	c = sljit_create_compiler(NULL);
	if (c == NULL) {
		printf("SETUP_FAILURE lifetime create\n");
		return -1;
	}
	if (emit_steps(c, p, 0, HOT_CP_JCC_WINDOW) != SLJIT_SUCCESS) {
		printf("SETUP_FAILURE lifetime prefix err=%d\n", (int)sljit_get_compiler_error(c));
		sljit_free_compiler(c);
		return -1;
	}
	buf = sljit_serialize_compiler(c, 0, &buf_size);
	if (buf == NULL) {
		printf("SETUP_FAILURE lifetime serialize\n");
		sljit_free_compiler(c);
		return -1;
	}
	sljit_free_compiler(c);

	ca = sljit_deserialize_compiler(buf, buf_size, 0, NULL);
	cb = sljit_deserialize_compiler(buf, buf_size, 0, NULL);
	SLJIT_FREE(buf, NULL);
	if (ca == NULL || cb == NULL) {
		printf("SETUP_FAILURE lifetime double deserialize\n");
		if (ca)
			sljit_free_compiler(ca);
		if (cb)
			sljit_free_compiler(cb);
		return -1;
	}
	global_stats.double_deserialize++;

	/* Tail A: finish the shared plan unchanged. */
	if (emit_steps(ca, p, HOT_CP_JCC_WINDOW, STEP_COUNT) != SLJIT_SUCCESS) {
		printf("SETUP_FAILURE lifetime tail A err=%d\n", (int)sljit_get_compiler_error(ca));
		sljit_free_compiler(ca);
		sljit_free_compiler(cb);
		return -1;
	}

	/* Tail B: append a different legal suffix.  Materialize the inverse
	   condition into R5 and park it in out[6], a slot the shared suffix
	   never touches (out[6] is written in the prefix as the flag
	   preserving MOV copy).  Then run the unchanged shared tail. */
	{
		int inv_cond = p->cond == SLJIT_CARRY ? SLJIT_NOT_CARRY : SLJIT_CARRY;
		if (sljit_emit_op_flags(cb, mov_op(p), SLJIT_R5, 0, inv_cond) != SLJIT_SUCCESS ||
			sljit_emit_op1(cb, mov_op(p), SLJIT_MEM1(SLJIT_S0), out_offset(6),
				SLJIT_R5, 0) != SLJIT_SUCCESS) {
			printf("SETUP_FAILURE lifetime tail B prefix err=%d\n",
				(int)sljit_get_compiler_error(cb));
			sljit_free_compiler(ca);
			sljit_free_compiler(cb);
			return -1;
		}
		if (emit_steps(cb, p, HOT_CP_JCC_WINDOW, STEP_COUNT) != SLJIT_SUCCESS) {
			printf("SETUP_FAILURE lifetime tail B err=%d\n",
				(int)sljit_get_compiler_error(cb));
			sljit_free_compiler(ca);
			sljit_free_compiler(cb);
			return -1;
		}
	}

	code_a = sljit_generate_code(ca, 0, NULL);
	code_b = sljit_generate_code(cb, 0, NULL);
	if (code_a == NULL || code_b == NULL) {
		printf("SETUP_FAILURE lifetime generate\n");
		if (code_a)
			sljit_free_code(code_a, NULL);
		if (code_b)
			sljit_free_code(code_b, NULL);
		sljit_free_compiler(ca);
		sljit_free_compiler(cb);
		return -1;
	}
	sljit_free_compiler(ca);
	sljit_free_compiler(cb);

	model_run(p, &m);
	slots_init(&slots_a, p);
	slots_init(&slots_b, p);

	fn = (jit_entry_fn)code_a;
	fn(&slots_a);
	if (compare_run(p, HOT_CP_JCC_WINDOW, 1, &slots_a, &m) != SLJIT_SUCCESS) {
		sljit_free_code(code_a, NULL);
		sljit_free_code(code_b, NULL);
		return -2;
	}

	/* Release A while B's code is still live and executable. */
	sljit_free_code(code_a, NULL);
	fn = (jit_entry_fn)code_b;
	fn(&slots_b);

	/* Model B: the shared suffix outputs match model A; the only extra
	   write is out[6], which holds the inverse materialization of the
	   PROD2 flags, i.e. 1 - model MAT2 value. */
	{
		model_u64 mask = width_mask(p);
		int i;
		TASK_CHECK(slots_b.canary_in == MODEL_CANARY_MAGIC);
		TASK_CHECK(slots_b.canary_mid == MODEL_CANARY_MAGIC);
		TASK_CHECK(slots_b.canary_out == MODEL_CANARY_MAGIC);
		for (i = 0; i < MODEL_INPUTS; i++)
			TASK_CHECK(slots_b.in[i] == (sljit_uw)p->in[i]);
		for (i = 0; i < MODEL_OUTPUTS; i++) {
			model_u64 want = m.out[i] & mask;
			model_u64 got = (model_u64)slots_b.out[i] & mask;
			if (i == 6)
				want = (1u - (m.out[2] & 1u)) & 1u;
			if (got != want) {
				printf("MISMATCH lifetime_b plan_id=%u slot=%d got=0x%llx want=0x%llx\n",
					p->id, i, (unsigned long long)got,
					(unsigned long long)want);
				plan_print_encoding(p);
				TASK_CHECK(0);
			}
		}
		/* Branch slot and chain slots must be identical to tail A. */
		TASK_CHECK((slots_b.out[1] & (sljit_uw)mask) == (m.out[1] & mask));
		TASK_CHECK((slots_b.out[0] & (sljit_uw)mask) == (m.out[0] & mask));
	}

	sljit_free_code(code_b, NULL);
	return 0;
}

/* Jump created before serialization must be re-linked through the public
   list / index API on the restored compiler; old pointers never cross. */
static int lifetime_label_refetch(const struct fuzz_plan *p)
{
	struct sljit_compiler *c;
	sljit_uw *buf;
	sljit_uw buf_size;
	void *code;
	struct ckpt_slots slots;
	struct model_state m;
	struct sljit_jump *jcc, *jend, *refetched;
	struct sljit_label *lyes, *lend;
	jit_entry_fn fn;
	int model_taken;

	c = sljit_create_compiler(NULL);
	if (c == NULL) {
		printf("SETUP_FAILURE refetch create\n");
		return -1;
	}
	/* Prefix through PROD2 so flags are live, then an unlinked jump. */
	if (emit_steps(c, p, 0, HOT_CP_JCC_WINDOW) != SLJIT_SUCCESS) {
		printf("SETUP_FAILURE refetch prefix err=%d\n", (int)sljit_get_compiler_error(c));
		sljit_free_compiler(c);
		return -1;
	}
	jcc = sljit_emit_jump(c, p->cond);
	if (jcc == NULL) {
		printf("SETUP_FAILURE refetch jump\n");
		sljit_free_compiler(c);
		return -1;
	}

	buf = sljit_serialize_compiler(c, 0, &buf_size);
	if (buf == NULL) {
		printf("SETUP_FAILURE refetch serialize\n");
		sljit_free_compiler(c);
		return -1;
	}
	sljit_free_compiler(c);

	c = sljit_deserialize_compiler(buf, buf_size, 0, NULL);
	SLJIT_FREE(buf, NULL);
	if (c == NULL) {
		printf("SETUP_FAILURE refetch deserialize\n");
		return -1;
	}
	global_stats.label_refetch++;

	/* Re-acquire every jump / label through the public list API. */
	refetched = sljit_get_first_jump(c);
	if (refetched == NULL || sljit_get_next_jump(refetched) != NULL) {
		printf("SETUP_FAILURE refetch jump list\n");
		sljit_free_compiler(c);
		return -1;
	}
	TASK_CHECK(sljit_jump_has_label(refetched) == 0);

	if (sljit_emit_op1(c, mov_op(p), SLJIT_MEM1(SLJIT_S0), out_offset(1),
		SLJIT_IMM, 0x51) != SLJIT_SUCCESS) {
		printf("SETUP_FAILURE refetch false path\n");
		sljit_free_compiler(c);
		return -1;
	}
	jend = sljit_emit_jump(c, SLJIT_JUMP);
	if (jend == NULL) {
		sljit_free_compiler(c);
		return -1;
	}
	lyes = sljit_emit_label(c);
	if (lyes == NULL) {
		sljit_free_compiler(c);
		return -1;
	}
	/* The label creation index must remain stable across serialization. */
	TASK_CHECK(sljit_get_label_index(lyes) == 0);
	sljit_set_label(refetched, lyes);

	if (sljit_emit_op1(c, mov_op(p), SLJIT_MEM1(SLJIT_S0), out_offset(1),
		SLJIT_IMM, 0xa7) != SLJIT_SUCCESS) {
		printf("SETUP_FAILURE refetch true path\n");
		sljit_free_compiler(c);
		return -1;
	}
	lend = sljit_emit_label(c);
	if (lend == NULL) {
		sljit_free_compiler(c);
		return -1;
	}
	sljit_set_label(jend, lend);
	declare_live_flags(c, p, p->fam2);

	/* Finish with the remaining shared steps (MAT2, CONS3, stores). */
	if (emit_steps(c, p, STEP_MAT2, STEP_COUNT) != SLJIT_SUCCESS) {
		printf("SETUP_FAILURE refetch suffix err=%d\n", (int)sljit_get_compiler_error(c));
		sljit_free_compiler(c);
		return -1;
	}

	code = sljit_generate_code(c, 0, NULL);
	if (code == NULL) {
		printf("SETUP_FAILURE refetch generate err=%d\n", (int)sljit_get_compiler_error(c));
		sljit_free_compiler(c);
		return -1;
	}
	sljit_free_compiler(c);

	model_run(p, &m);
	model_taken = (m.out[1] == 0xa7ULL);
	slots_init(&slots, p);
	fn = (jit_entry_fn)code;
	fn(&slots);

	/* The branch result is the direct witness of flag-state restoration. */
	TASK_CHECK((slots.canary_in) == MODEL_CANARY_MAGIC);
	TASK_CHECK((slots.canary_mid) == MODEL_CANARY_MAGIC);
	TASK_CHECK((slots.canary_out) == MODEL_CANARY_MAGIC);
	TASK_CHECK((slots.out[1] & width_mask(p)) == (model_taken ? 0xa7ULL : 0x51ULL));
	TASK_CHECK((slots.out[2] & width_mask(p)) == (m.out[2] & width_mask(p)));
	TASK_CHECK((slots.out[3] & width_mask(p)) == (m.out[3] & width_mask(p)));
	TASK_CHECK((slots.out[4] & width_mask(p)) == (m.out[4] & width_mask(p)));
	TASK_CHECK((slots.out[5] & width_mask(p)) == (m.out[5] & width_mask(p)));
	for (int i = 0; i < MODEL_INPUTS; i++)
		TASK_CHECK(slots.in[i] == (sljit_uw)p->in[i]);

	sljit_free_code(code, NULL);
	return 0;
}

int contract_lifetime(void)
{
	struct fuzz_plan p;
	unsigned only;
	unsigned i;
	unsigned done = 0;
	const char *only_env = getenv("SLJIT_CKPT_ONLY");

	only = only_env ? (unsigned)strtoul(only_env, NULL, 0) : UINT32_MAX;

	for (i = 0; i < PLAN_COUNT; i++) {
		plan_init(&p, i);
		if (i != only && only != UINT32_MAX)
			continue;
		if (done == 0)
			plan_print_encoding(&p);

		if (lifetime_double_deserialize(&p) != 0)
			return -1;
		if (lifetime_label_refetch(&p) != 0)
			return -1;
		done++;
		global_stats.plans++;
		global_stats.width32 += p.is_32 ? 1 : 0;
		global_stats.width64 += p.is_32 ? 0 : 1;
		if (only != UINT32_MAX)
			break;
	}

	if (done == 0) {
		printf("SETUP_FAILURE no lifetime plans selected\n");
		return -1;
	}
	ckpt_fuzz_print_stats();
	return (int)done;
}

/* --------------------------------------------------------------------- */
/*  Shared category statistics (printed by the driver after each group    */
/*  via this destructor-free helper called through weak usage).            */
/* --------------------------------------------------------------------- */

void ckpt_fuzz_print_stats(void)
{
	const struct {
		const char *name;
		unsigned value;
	} items[] = {
		{"plans", global_stats.plans},
		{"width32", global_stats.width32},
		{"width64", global_stats.width64},
		{"add_producers", global_stats.add_producers},
		{"sub_producers", global_stats.sub_producers},
		{"addc_ops", global_stats.addc_ops},
		{"subc_ops", global_stats.subc_ops},
		{"mat_consumers", global_stats.mat_consumers},
		{"jcc_consumers", global_stats.jcc_consumers},
		{"carry_in0", global_stats.carry_in0},
		{"carry_in1", global_stats.carry_in1},
		{"borrow_in0", global_stats.borrow_in0},
		{"borrow_in1", global_stats.borrow_in1},
		{"reg_ops", global_stats.reg_ops},
		{"imm_ops", global_stats.imm_ops},
		{"mem_ops", global_stats.mem_ops},
		{"resume_runs", global_stats.resume_runs},
		{"buffer_freed_before_continue", global_stats.buffer_freed_before_continue},
		{"double_deserialize", global_stats.double_deserialize},
		{"label_refetch", global_stats.label_refetch},
	};
	unsigned i;

	for (i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
		if (items[i].value != 0)
			printf("STAT %s=%u\n", items[i].name, items[i].value);
	}
	for (i = 0; i <= STEP_COUNT; i++) {
		if (global_stats.cp_position[i] != 0)
			printf("STAT cp_position_%u=%u\n", i, global_stats.cp_position[i]);
	}
	fflush(stdout);
}
