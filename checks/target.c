#include <spvm/context.h>
#include <spvm/state.h>
#include <spvm/specialize.h>
#include <spvm/spirv.h>
#include "task_support/check.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* Small SPIR-V word builder. Every BOP() emits one instruction whose  */
/* meaning is commented at the call site, so the modules read like     */
/* annotated SPIR-V assembly.                                          */
/* ================================================================== */

typedef struct {
	spvm_word w[2048];
	size_t n;
} builder;

#define BW(b, x) ((b)->w[(b)->n++] = (spvm_word)(x))
#define BOP(b, op, ...) do { \
	const spvm_word _a[] = { __VA_ARGS__ }; \
	size_t _k; \
	BW(b, (spvm_word)(((1u + sizeof(_a) / sizeof(_a[0])) << SpvWordCountShift) | (op))); \
	for (_k = 0; _k < sizeof(_a) / sizeof(_a[0]); ++_k) BW(b, _a[_k]); \
} while (0)
#define BOP0(b, op) BW(b, (spvm_word)((1u << SpvWordCountShift) | (op)))

/* "main" packed little-endian is 0x6e69616d; OpEntryPoint needs a NUL word. */
#define MAIN_NAME 0x6e69616du

static void header(builder* b, spvm_word bound)
{
	BW(b, SpvMagicNumber);
	BW(b, 0x00010000u); /* version 1.0 */
	BW(b, 0u);          /* generator */
	BW(b, bound);       /* id bound */
	BW(b, 0u);          /* reserved */
	BOP(b, SpvOpCapability, SpvCapabilityShader);
	BOP(b, SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
}

static uint32_t float_bits(float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	return u;
}

static spvm_state_t make_state(spvm_context_t ctx, spvm_program_t p)
{
	return spvm_state_create(p);
}

static void run_main(spvm_state_t s, spvm_word main_id)
{
	spvm_state_prepare(s, main_id);
	spvm_state_call_function(s);
}

/* ================================================================== */
/* VALUES MODULE                                                       */
/*                                                                     */
/* Scalar spec leaves (uint/int/float/bool), an ordinary-constant      */
/* dependency diamond, vector composite extract/insert/shuffle, the    */
/* full integer/bool op set and a -0.0 float round trip. main() sums   */
/* several resolved constants at runtime through ordinary IAdd so the  */
/* stored output proves the values really reach the executor.          */
/* ================================================================== */

/* id layout ----------------------------------------------------------------
 * types   : 1 void, 2 uint, 3 int, 4 float, 5 bool, 6 uvec3, 9 fvec2
 * ordinary: 14=uint100, 15=int2, 16=float(pi bits)
 * specs    : 10 u SpecId7, 11 i SpecId8, 12 f SpecId9, 13 bool SpecId10
 *            40 = SpecConstantTrue (default only, no override)
 * spec ops : 20..47, 50..51
 * output   : 61 ; main : 80
 * ------------------------------------------------------------------------ */
static size_t values_module(spvm_word* out)
{
	builder b;
	b.n = 0;
	header(&b, 120);

	/* entry point: fragment main, interface = {output 61} */
	BOP(&b, SpvOpEntryPoint, SpvExecutionModelFragment, 80, MAIN_NAME, 0, 61);
	BOP(&b, SpvOpExecutionMode, 80, SpvExecutionModeOriginUpperLeft);
	BOP(&b, SpvOpDecorate, 61, SpvDecorationLocation, 0);

	/* types */
	BOP(&b, SpvOpTypeVoid, 1);
	BOP(&b, SpvOpTypeInt, 2, 32, 0);       /* uint */
	BOP(&b, SpvOpTypeInt, 3, 32, 1);       /* int  */
	BOP(&b, SpvOpTypeFloat, 4, 32);        /* float */
	BOP(&b, SpvOpTypeBool, 5);
	BOP(&b, SpvOpTypeVector, 6, 2, 3);     /* uvec3 */
	BOP(&b, SpvOpTypeVector, 9, 4, 2);     /* fvec2 */

	/* ordinary constants (legal dependencies of specialization values) */
	BOP(&b, SpvOpConstant, 2, 14, 100u);   /* uint 100 */
	BOP(&b, SpvOpConstant, 3, 15, 2u);     /* int 2 */
	BOP(&b, SpvOpConstant, 4, 16, 0x40490FDBu); /* float pi */

	/* specialization leaves with SpecId decorations */
	BOP(&b, SpvOpDecorate, 10, SpvDecorationSpecId, 7);
	BOP(&b, SpvOpDecorate, 11, SpvDecorationSpecId, 8);
	BOP(&b, SpvOpDecorate, 12, SpvDecorationSpecId, 9);
	BOP(&b, SpvOpDecorate, 13, SpvDecorationSpecId, 10);
	BOP(&b, SpvOpSpecConstant, 2, 10, 5u);          /* uint, default 5 */
	BOP(&b, SpvOpSpecConstant, 3, 11, 0xFFFFFFFDu); /* int, default -3 */
	BOP(&b, SpvOpSpecConstant, 4, 12, 0x3FC00000u); /* float, default 1.5 */
	BOP(&b, SpvOpSpecConstantFalse, 5, 13);         /* bool, default false */
	BOP(&b, SpvOpSpecConstantTrue, 5, 40);          /* bool, default true */

	/* diamond: 20 = 10 + 14; 21 = 20 * 10; 22 = 20 - 10; 23 = 21 + 22 */
	BOP(&b, SpvOpSpecConstantOp, 2, 20, SpvOpIAdd, 10, 14);
	BOP(&b, SpvOpSpecConstantOp, 2, 21, SpvOpIMul, 20, 10);
	BOP(&b, SpvOpSpecConstantOp, 2, 22, SpvOpISub, 20, 10);
	BOP(&b, SpvOpSpecConstantOp, 2, 23, SpvOpIAdd, 21, 22);

	/* vector composite + literal extract/shuffle/insert */
	BOP(&b, SpvOpSpecConstantComposite, 6, 30, 10, 14, 20);          /* uvec3 */
	BOP(&b, SpvOpSpecConstantOp, 2, 31, SpvOpCompositeExtract, 30, 1);
	BOP(&b, SpvOpSpecConstantOp, 6, 32, SpvOpVectorShuffle, 30, 30, 2, 0, 1);
	BOP(&b, SpvOpSpecConstantOp, 6, 33, SpvOpCompositeInsert, 14, 32, 1);

	/* scalar integer ops */
	BOP(&b, SpvOpSpecConstantOp, 3, 34, SpvOpSNegate, 11);
	BOP(&b, SpvOpSpecConstantOp, 3, 35, SpvOpSDiv, 11, 15);
	BOP(&b, SpvOpSpecConstantOp, 2, 36, SpvOpUDiv, 20, 14);

	/* boolean ops */
	BOP(&b, SpvOpSpecConstantOp, 5, 41, SpvOpLogicalOr, 40, 13);
	BOP(&b, SpvOpSpecConstantOp, 5, 42, SpvOpLogicalAnd, 13, 40);
	BOP(&b, SpvOpSpecConstantOp, 5, 43, SpvOpLogicalNot, 13);
	BOP(&b, SpvOpSpecConstantOp, 5, 44, SpvOpIEqual, 10, 14);
	BOP(&b, SpvOpSpecConstantOp, 5, 45, SpvOpSLessThan, 11, 15);
	BOP(&b, SpvOpSpecConstantOp, 5, 46, SpvOpULessThan, 14, 10);
	BOP(&b, SpvOpSpecConstantOp, 2, 47, SpvOpSelect, 13, 10, 14);

	/* float -0.0 must keep its bits through override -> composite -> extract */
	BOP(&b, SpvOpSpecConstantComposite, 9, 50, 12, 16);
	BOP(&b, SpvOpSpecConstantOp, 4, 51, SpvOpCompositeExtract, 50, 0);

	/* output variable */
	BOP(&b, SpvOpTypePointer, 60, SpvStorageClassOutput, 2);
	BOP(&b, SpvOpVariable, 60, 61, SpvStorageClassOutput);

	/* void main(): out = ((23 + 31) + 36) + 47 */
	BOP(&b, SpvOpTypeFunction, 71, 1);
	BOP(&b, SpvOpFunction, 1, 80, SpvFunctionControlMaskNone, 71);
	BOP(&b, SpvOpLabel, 81);
	BOP(&b, SpvOpIAdd, 2, 82, 23, 31);
	BOP(&b, SpvOpIAdd, 2, 83, 82, 36);
	BOP(&b, SpvOpIAdd, 2, 84, 83, 47);
	BOP(&b, SpvOpStore, 61, 84);
	BOP0(&b, SpvOpReturn);
	BOP0(&b, SpvOpFunctionEnd);

	memcpy(out, b.w, b.n * sizeof(spvm_word));
	return b.n;
}

/* Overrides applied to values_module: 7<-6, 8<-7, 9<-(-0.0), 10<-true. */
static const spvm_spec_entry values_entries[] = {
	{ 7, 6u },
	{ 8, 7u },
	{ 9, 0x80000000u },
	{ 10, 1u },
};

static int case_values_override(void)
{
	spvm_word words[2048];
	size_t n = values_module(words);

	spvm_context_t ctx = spvm_context_initialize();
	spvm_spec_error err;
	spvm_program_t p = spvm_program_create_specialized(ctx, words, n,
		values_entries, sizeof(values_entries) / sizeof(values_entries[0]), &err);
	TASK_CHECK(p != NULL);
	TASK_CHECK(err.code == SPVM_SPEC_OK && err.result_id == 0);

	spvm_state_t s = make_state(ctx, p);
	TASK_CHECK(s != NULL);

	/* independent expectations, computed by hand for the override table */
	TASK_CHECK(s->results[10].members[0].value.u == 6u);
	TASK_CHECK(s->results[11].members[0].value.s == 7);
	TASK_CHECK(s->results[13].members[0].value.b == 1);
	TASK_CHECK(s->results[40].members[0].value.b == 1); /* undecorated default */

	/* diamond: 20=106, 21=636, 22=100, 23=736 */
	TASK_CHECK(s->results[20].members[0].value.u == 106u);
	TASK_CHECK(s->results[21].members[0].value.u == 636u);
	TASK_CHECK(s->results[22].members[0].value.u == 100u);
	TASK_CHECK(s->results[23].members[0].value.u == 736u);

	/* uvec3 30 = (6,100,106); extract 31 = 100. Vector components live
	 * directly in members[i].value (leaf members have no nested members). */
	TASK_CHECK(s->results[30].member_count == 3);
	TASK_CHECK(s->results[30].members[0].value.u == 6u);
	TASK_CHECK(s->results[30].members[2].value.u == 106u);
	TASK_CHECK(s->results[31].members[0].value.u == 100u);

	/* shuffle 32 = (106,6,100); insert 100 at index 1 -> 33 = (106,100,100) */
	TASK_CHECK(s->results[32].members[0].value.u == 106u);
	TASK_CHECK(s->results[32].members[1].value.u == 6u);
	TASK_CHECK(s->results[33].members[1].value.u == 100u);
	TASK_CHECK(s->results[33].members[2].value.u == 100u);

	/* integer ops: negate 7 -> -7; 7/2 = 3; 106/100 = 1 */
	TASK_CHECK(s->results[34].members[0].value.s == -7);
	TASK_CHECK(s->results[35].members[0].value.s == 3);
	TASK_CHECK(s->results[36].members[0].value.u == 1u);

	/* booleans */
	TASK_CHECK(s->results[41].members[0].value.b == 1);  /* or  */
	TASK_CHECK(s->results[42].members[0].value.b == 1);  /* and */
	TASK_CHECK(s->results[43].members[0].value.b == 0);  /* not true */
	TASK_CHECK(s->results[44].members[0].value.b == 0);  /* 6 == 100 */
	TASK_CHECK(s->results[45].members[0].value.b == 0);  /* 7 < 2 */
	TASK_CHECK(s->results[46].members[0].value.b == 0);  /* 100 < 6 */
	TASK_CHECK(s->results[47].members[0].value.u == 6u); /* select true -> 10 */

	/* negative zero survives override, composite and extract */
	TASK_CHECK(float_bits(s->results[51].members[0].value.f) == 0x80000000u);

	run_main(s, 80);
	/* 23(736) + 31(100) + 36(1) + 47(6) = 843 */
	TASK_CHECK(s->results[61].members[0].value.u == 843u);

	spvm_state_delete(s);
	spvm_program_delete(p);
	spvm_context_deinitialize(ctx);
	return 0;
}

/* Empty table: defaults must still be fully evaluated (no legacy ignore). */
static int case_values_default(void)
{
	spvm_word words[2048];
	size_t n = values_module(words);

	spvm_context_t ctx = spvm_context_initialize();
	/* NULL error sink is allowed. */
	spvm_program_t p = spvm_program_create_specialized(ctx, words, n, NULL, 0, NULL);
	TASK_CHECK(p != NULL);
	spvm_state_t s = make_state(ctx, p);

	TASK_CHECK(s->results[10].members[0].value.u == 5u);       /* default uint */
	TASK_CHECK(s->results[11].members[0].value.s == -3);       /* default int */
	TASK_CHECK(s->results[13].members[0].value.b == 0);        /* default false */
	/* default diamond: 20=105, 21=525, 22=100, 23=625 */
	TASK_CHECK(s->results[23].members[0].value.u == 625u);
	/* default float 1.5 keeps its bits */
	TASK_CHECK(float_bits(s->results[12].members[0].value.f) == 0x3FC00000u);

	run_main(s, 80);
	/* 625 + 100 + 1 + select(false)->100 = 826 */
	TASK_CHECK(s->results[61].members[0].value.u == 826u);

	spvm_state_delete(s);
	spvm_program_delete(p);
	spvm_context_deinitialize(ctx);
	return 0;
}

/* ================================================================== */
/* WRAP MODULE                                                         */
/*                                                                     */
/* Pins 32-bit two's-complement wrapping independently of C signed    */
/* overflow, plus INT_MIN negation and a bool override.               */
/* ================================================================== */
static size_t wrap_module(spvm_word* out)
{
	builder b;
	b.n = 0;
	header(&b, 60);
	BOP(&b, SpvOpTypeVoid, 1);
	BOP(&b, SpvOpTypeInt, 2, 32, 0); /* uint */
	BOP(&b, SpvOpTypeInt, 3, 32, 1); /* int  */
	BOP(&b, SpvOpTypeBool, 5);

	BOP(&b, SpvOpConstant, 2, 8, 1u);             /* uint 1 */
	BOP(&b, SpvOpConstant, 2, 9, 0xFFFFFFFFu);    /* uint max */

	/* spec leaves */
	BOP(&b, SpvOpDecorate, 10, SpvDecorationSpecId, 1); /* uint */
	BOP(&b, SpvOpDecorate, 11, SpvDecorationSpecId, 2); /* int  */
	BOP(&b, SpvOpDecorate, 13, SpvDecorationSpecId, 3); /* bool */
	BOP(&b, SpvOpSpecConstant, 2, 10, 0x80000000u);
	BOP(&b, SpvOpSpecConstant, 3, 11, 0x80000000u);     /* INT32_MIN */
	BOP(&b, SpvOpSpecConstantTrue, 5, 13);

	BOP(&b, SpvOpSpecConstantOp, 2, 20, SpvOpIAdd, 9, 8);   /* max+1 -> 0   */
	BOP(&b, SpvOpSpecConstantOp, 2, 21, SpvOpIAdd, 10, 10); /* wraps        */
	BOP(&b, SpvOpSpecConstantOp, 2, 22, SpvOpIMul, 9, 9);   /* max*max -> 1 */
	BOP(&b, SpvOpSpecConstantOp, 2, 23, SpvOpISub, 8, 9);   /* 1-max -> 2   */
	BOP(&b, SpvOpSpecConstantOp, 3, 24, SpvOpSNegate, 11);  /* -INT_MIN     */
	BOP(&b, SpvOpSpecConstantOp, 5, 26, SpvOpLogicalNot, 13);

	memcpy(out, b.w, b.n * sizeof(spvm_word));
	return b.n;
}

static int case_values_wrap(void)
{
	spvm_word words[1024];
	size_t n = wrap_module(words);
	spvm_context_t ctx = spvm_context_initialize();

	spvm_spec_entry ent[3] = {
		{ 1, 0x7FFFFFFFu }, /* uint max */
		{ 2, 0x80000000u }, /* keep INT_MIN */
		{ 3, 0u },          /* force bool false */
	};
	spvm_program_t p = spvm_program_create_specialized(ctx, words, n, ent, 3, NULL);
	TASK_CHECK(p != NULL);
	spvm_state_t s = make_state(ctx, p);

	TASK_CHECK(s->results[20].members[0].value.u == 0u);          /* max + 1 */
	TASK_CHECK(s->results[21].members[0].value.u == 0xFFFFFFFEu); /* max + max */
	TASK_CHECK(s->results[22].members[0].value.u == 1u);          /* max * max */
	TASK_CHECK(s->results[23].members[0].value.u == 2u);          /* 1 - max */
	TASK_CHECK((uint32_t)s->results[24].members[0].value.s == 0x80000000u);
	TASK_CHECK(s->results[13].members[0].value.b == 0);           /* overridden */
	TASK_CHECK(s->results[26].members[0].value.b == 1);           /* not false */

	spvm_state_delete(s);
	spvm_program_delete(p);
	spvm_context_deinitialize(ctx);
	return 0;
}

/* ================================================================== */
/* LAYOUT MODULE                                                       */
/*                                                                     */
/* %10 (SpecId 1) is the length of array type 17 inside struct 19; a   */
/* Private variable 22 is null-initialized from it and must take the   */
/* final specialized shape. A second, fixed-shape nested aggregate     */
/* (array of uvec2 inside a struct) initializes Private variable 38    */
/* with concrete values. main() writes through an AccessChain, reads   */
/* through a helper parameter and an AccessChain, and sums everything. */
/* ================================================================== */
static size_t layout_module(spvm_word* out)
{
	builder b;
	b.n = 0;
	header(&b, 120);

	BOP(&b, SpvOpEntryPoint, SpvExecutionModelFragment, 80, MAIN_NAME, 0, 61);
	BOP(&b, SpvOpExecutionMode, 80, SpvExecutionModeOriginUpperLeft);
	BOP(&b, SpvOpDecorate, 61, SpvDecorationLocation, 0);

	BOP(&b, SpvOpTypeVoid, 1);
	BOP(&b, SpvOpTypeInt, 2, 32, 0); /* uint */
	BOP(&b, SpvOpTypeVector, 6, 2, 2); /* uvec2 */

	/* scalar constants */
	BOP(&b, SpvOpConstant, 2, 14, 1u);
	BOP(&b, SpvOpConstant, 2, 15, 2u);
	BOP(&b, SpvOpConstant, 2, 16, 3u);
	BOP(&b, SpvOpConstant, 2, 24, 4u);
	BOP(&b, SpvOpConstant, 2, 18, 7u);
	BOP(&b, SpvOpConstant, 2, 26, 2u); /* fixed nested-array length */
	BOP(&b, SpvOpConstant, 2, 40, 0u); /* index 0 */
	BOP(&b, SpvOpConstant, 2, 41, 1u); /* index 1 */
	BOP(&b, SpvOpConstant, 2, 42, 2u); /* index 2 */

	/* spec-driven array length, default 2, overridable via SpecId 1 */
	BOP(&b, SpvOpDecorate, 10, SpvDecorationSpecId, 1);
	BOP(&b, SpvOpSpecConstant, 2, 10, 2u);

	/* struct 19 { uint[spec 10]; uint } */
	BOP(&b, SpvOpTypeArray, 17, 2, 10);
	BOP(&b, SpvOpTypeStruct, 19, 17, 2);
	BOP(&b, SpvOpConstantNull, 19, 20);
	BOP(&b, SpvOpTypePointer, 21, SpvStorageClassPrivate, 19);
	BOP(&b, SpvOpVariable, 21, 22, SpvStorageClassPrivate, 20);

	/* fixed nested aggregate: array 27 of uvec2, length 2 */
	BOP(&b, SpvOpConstantComposite, 6, 30, 14, 15); /* (1,2) */
	BOP(&b, SpvOpConstantComposite, 6, 31, 16, 24); /* (3,4) */
	BOP(&b, SpvOpTypeArray, 27, 6, 26);
	BOP(&b, SpvOpSpecConstantComposite, 27, 32, 30, 31); /* [(1,2),(3,4)] */
	BOP(&b, SpvOpTypeStruct, 35, 27, 2);
	BOP(&b, SpvOpSpecConstantComposite, 35, 36, 32, 18);
	BOP(&b, SpvOpTypePointer, 37, SpvStorageClassPrivate, 35);
	BOP(&b, SpvOpVariable, 37, 38, SpvStorageClassPrivate, 36);

	/* helper(uint* p) -> *p */
	BOP(&b, SpvOpTypePointer, 23, SpvStorageClassPrivate, 2);
	BOP(&b, SpvOpTypeFunction, 70, 2, 23);
	BOP(&b, SpvOpTypeFunction, 71, 1);
	BOP(&b, SpvOpFunction, 2, 75, SpvFunctionControlMaskNone, 70);
	BOP(&b, SpvOpFunctionParameter, 23, 76);
	BOP(&b, SpvOpLabel, 77);
	BOP(&b, SpvOpLoad, 2, 78, 76);
	BOP(&b, SpvOpReturnValue, 78);
	BOP0(&b, SpvOpFunctionEnd);

	/* output */
	BOP(&b, SpvOpTypePointer, 60, SpvStorageClassOutput, 2);
	BOP(&b, SpvOpVariable, 60, 61, SpvStorageClassOutput);

	/* main():
	 *   &p = &var22.arr[2]; *p = 1 + 7 (=8)
	 *   h  = helper(&p)                       (=8)
	 *   q  = var38.arr[1][0]                  (=3)
	 *   out = h + q + 7                       (=18)
	 */
	BOP(&b, SpvOpFunction, 1, 80, SpvFunctionControlMaskNone, 71);
	BOP(&b, SpvOpLabel, 81);
	BOP(&b, SpvOpAccessChain, 23, 82, 22, 40, 42);
	BOP(&b, SpvOpIAdd, 2, 83, 14, 18);
	BOP(&b, SpvOpStore, 82, 83);
	BOP(&b, SpvOpAccessChain, 23, 84, 22, 40, 42);
	BOP(&b, SpvOpLoad, 2, 85, 84);
	BOP(&b, SpvOpFunctionCall, 2, 86, 75, 84);
	BOP(&b, SpvOpAccessChain, 23, 87, 38, 40, 41, 40);
	BOP(&b, SpvOpLoad, 2, 88, 87);
	BOP(&b, SpvOpIAdd, 2, 89, 86, 88);
	BOP(&b, SpvOpIAdd, 2, 90, 89, 18);
	BOP(&b, SpvOpStore, 61, 90);
	BOP0(&b, SpvOpReturn);
	BOP0(&b, SpvOpFunctionEnd);

	memcpy(out, b.w, b.n * sizeof(spvm_word));
	return b.n;
}

static int case_layout(void)
{
	spvm_word words[2048];
	size_t n = layout_module(words);
	spvm_context_t ctx = spvm_context_initialize();

	/* default specialization: length stays 2 */
	spvm_program_t p0 = spvm_program_create_specialized(ctx, words, n, NULL, 0, NULL);
	TASK_CHECK(p0 != NULL);
	spvm_state_t s0 = make_state(ctx, p0);
	TASK_CHECK(s0->results[22].members[0].member_count == 2); /* spec array */
	TASK_CHECK(s0->results[22].member_count == 2);            /* struct {arr,u} */

	/* override length to 3 and execute */
	spvm_spec_entry e = { 1, 3u };
	spvm_spec_error err;
	spvm_program_t p3 = spvm_program_create_specialized(ctx, words, n, &e, 1, &err);
	TASK_CHECK(p3 != NULL && err.code == SPVM_SPEC_OK);
	spvm_state_t s3 = make_state(ctx, p3);

	/* spec-sized private aggregate reshaped to length 3 */
	TASK_CHECK(s3->results[22].members[0].member_count == 3);
	TASK_CHECK(s3->results[22].members[1].member_count == 0); /* scalar tail */

	/* fixed nested initialization keeps concrete values and shape */
	TASK_CHECK(s3->results[38].members[0].member_count == 2);          /* array */
	TASK_CHECK(s3->results[38].members[0].members[1].member_count == 2); /* vec */
	TASK_CHECK(s3->results[38].members[0].members[1].members[0].value.u == 3u);
	TASK_CHECK(s3->results[38].members[0].members[0].members[1].value.u == 2u);
	TASK_CHECK(s3->results[38].members[1].value.u == 7u);

	run_main(s3, 80);
	TASK_CHECK(s3->results[85].members[0].value.u == 8u); /* direct load */
	TASK_CHECK(s3->results[86].members[0].value.u == 8u); /* via helper ptr */
	TASK_CHECK(s3->results[88].members[0].value.u == 3u); /* nested load */
	TASK_CHECK(s3->results[61].members[0].value.u == 18u);
	/* the runtime store really landed in the reshaped array */
	TASK_CHECK(s3->results[22].members[0].members[2].value.u == 8u);

	spvm_state_delete(s3);
	spvm_state_delete(s0);
	spvm_program_delete(p3);
	spvm_program_delete(p0);
	spvm_context_deinitialize(ctx);
	return 0;
}

/* A nested struct whose innermost array length is itself computed by a
 * SpecConstantOp (IAdd of two leaves): proves derived lengths, not just leaf
 * overrides, drive the final aggregate shape and runtime AccessChains. */
static size_t derived_layout_module(spvm_word* out)
{
	builder b;
	b.n = 0;
	header(&b, 120);
	BOP(&b, SpvOpEntryPoint, SpvExecutionModelFragment, 80, MAIN_NAME, 0, 61);
	BOP(&b, SpvOpExecutionMode, 80, SpvExecutionModeOriginUpperLeft);
	BOP(&b, SpvOpDecorate, 61, SpvDecorationLocation, 0);

	BOP(&b, SpvOpTypeVoid, 1);
	BOP(&b, SpvOpTypeInt, 2, 32, 0); /* uint */
	BOP(&b, SpvOpConstant, 2, 7, 0u);  /* index 0 */
	BOP(&b, SpvOpConstant, 2, 8, 1u);  /* value 1 / subtract 1 */
	BOP(&b, SpvOpConstant, 2, 9, 77u);

	/* two leaves and a derived length 12 = 10 + 11 */
	BOP(&b, SpvOpDecorate, 10, SpvDecorationSpecId, 1);
	BOP(&b, SpvOpDecorate, 11, SpvDecorationSpecId, 2);
	BOP(&b, SpvOpSpecConstant, 2, 10, 1u);
	BOP(&b, SpvOpSpecConstant, 2, 11, 1u);
	BOP(&b, SpvOpSpecConstantOp, 2, 12, SpvOpIAdd, 10, 11);
	BOP(&b, SpvOpSpecConstantOp, 2, 13, SpvOpISub, 12, 8); /* last index */

	/* struct25 { struct19 { uint[12] }, uint } */
	BOP(&b, SpvOpTypeArray, 17, 2, 12);
	BOP(&b, SpvOpTypeStruct, 19, 17);
	BOP(&b, SpvOpTypeStruct, 25, 19, 2);
	BOP(&b, SpvOpConstantNull, 25, 26);
	BOP(&b, SpvOpTypePointer, 27, SpvStorageClassPrivate, 25);
	BOP(&b, SpvOpVariable, 27, 28, SpvStorageClassPrivate, 26);

	BOP(&b, SpvOpTypePointer, 23, SpvStorageClassPrivate, 2);
	BOP(&b, SpvOpTypePointer, 60, SpvStorageClassOutput, 2);
	BOP(&b, SpvOpVariable, 60, 61, SpvStorageClassOutput);
	BOP(&b, SpvOpTypeFunction, 71, 1);

	/* main(): store 77 at inner.arr[len-1], load it back to the output */
	BOP(&b, SpvOpFunction, 1, 80, SpvFunctionControlMaskNone, 71);
	BOP(&b, SpvOpLabel, 81);
	BOP(&b, SpvOpAccessChain, 23, 82, 28, 7, 7, 13);
	BOP(&b, SpvOpStore, 82, 9);
	BOP(&b, SpvOpLoad, 2, 83, 82);
	BOP(&b, SpvOpStore, 61, 83);
	BOP0(&b, SpvOpReturn);
	BOP0(&b, SpvOpFunctionEnd);

	memcpy(out, b.w, b.n * sizeof(spvm_word));
	return b.n;
}

static int case_layout_derived(void)
{
	spvm_word words[1024];
	size_t n = derived_layout_module(words);
	spvm_context_t ctx = spvm_context_initialize();

	/* default: length = 1+1 = 2 */
	spvm_program_t p0 = spvm_program_create_specialized(ctx, words, n, NULL, 0, NULL);
	TASK_CHECK(p0 != NULL);
	spvm_state_t s0 = make_state(ctx, p0);
	TASK_CHECK(s0->results[28].members[0].members[0].member_count == 2);
	run_main(s0, 80);
	TASK_CHECK(s0->results[61].members[0].value.u == 77u);
	TASK_CHECK(s0->results[28].members[0].members[0].members[1].value.u == 77u);
	spvm_state_delete(s0);
	spvm_program_delete(p0);

	/* override: length = 2+3 = 5, last index 4 */
	spvm_spec_entry ent[2] = { {1, 2u}, {2, 3u} };
	spvm_program_t p5 = spvm_program_create_specialized(ctx, words, n, ent, 2, NULL);
	TASK_CHECK(p5 != NULL);
	spvm_state_t s5 = make_state(ctx, p5);
	TASK_CHECK(s5->results[28].members[0].members[0].member_count == 5);
	run_main(s5, 80);
	TASK_CHECK(s5->results[61].members[0].value.u == 77u);
	TASK_CHECK(s5->results[28].members[0].members[0].members[4].value.u == 77u);
	spvm_state_delete(s5);
	spvm_program_delete(p5);

	spvm_context_deinitialize(ctx);
	return 0;
}

/* ================================================================== */
/* ISOLATION                                                           */
/* ================================================================== */

/* Two variants of one source, several states each: mutating one state's
 * private aggregate must not touch any other state or the other variant,
 * and destroying one variant must leave the other fully functional. */
static int case_isolation_variants(void)
{
	spvm_word words[2048];
	size_t n = layout_module(words);
	spvm_context_t ctx = spvm_context_initialize();

	spvm_spec_entry e2 = { 1, 2u };
	spvm_spec_entry e4 = { 1, 4u };
	spvm_program_t pa = spvm_program_create_specialized(ctx, words, n, &e2, 1, NULL);
	spvm_program_t pb = spvm_program_create_specialized(ctx, words, n, &e4, 1, NULL);
	TASK_CHECK(pa != NULL && pb != NULL);

	spvm_state_t a1 = make_state(ctx, pa), a2 = make_state(ctx, pa);
	spvm_state_t b1 = make_state(ctx, pb), b2 = make_state(ctx, pb);
	TASK_CHECK(a1 && a2 && b1 && b2);

	TASK_CHECK(a1->results[22].members[0].member_count == 2);
	TASK_CHECK(b1->results[22].members[0].member_count == 4);

	/* mutate a private aggregate leaf in one state only */
	a1->results[22].members[0].members[0].value.u = 999u;
	TASK_CHECK(a2->results[22].members[0].members[0].value.u == 0u);
	TASK_CHECK(b1->results[22].members[0].members[0].value.u == 0u);
	TASK_CHECK(b2->results[22].members[0].members[0].value.u == 0u);

	/* states die before their program; variant A then disappears */
	spvm_state_delete(a1);
	spvm_state_delete(a2);
	spvm_program_delete(pa);

	/* variant B survives and still executes correctly (length 4) */
	TASK_CHECK(b1->results[38].members[0].members[1].members[0].value.u == 3u);
	run_main(b1, 80);
	TASK_CHECK(b1->results[61].members[0].value.u == 18u);
	TASK_CHECK(b2->results[61].members[0].value.u == 0u); /* untouched state */

	spvm_state_delete(b1);
	spvm_state_delete(b2);
	spvm_program_delete(pb);
	spvm_context_deinitialize(ctx);
	return 0;
}

/* On success the program owns its source and entries: free the caller
 * buffers immediately, then build states and execute. */
static int case_ownership(void)
{
	spvm_word words[2048];
	size_t n = values_module(words);

	spvm_word* src = (spvm_word*)malloc(n * sizeof(spvm_word));
	spvm_spec_entry* ent = (spvm_spec_entry*)malloc(2 * sizeof(spvm_spec_entry));
	TASK_CHECK(src != NULL && ent != NULL);
	memcpy(src, words, n * sizeof(spvm_word));
	ent[0].spec_id = 7; ent[0].bits = 6u;
	ent[1].spec_id = 10; ent[1].bits = 1u;

	spvm_context_t ctx = spvm_context_initialize();
	spvm_program_t p = spvm_program_create_specialized(ctx, src, n, ent, 2, NULL);
	TASK_CHECK(p != NULL);

	free(ent);  /* entries released before any state exists */
	free(src);  /* source released before any state exists */

	spvm_state_t s1 = make_state(ctx, p);
	spvm_state_t s2 = make_state(ctx, p);
	TASK_CHECK(s1 && s2);
	TASK_CHECK(s1->results[10].members[0].value.u == 6u);
	TASK_CHECK(s2->results[10].members[0].value.u == 6u);
	run_main(s1, 80);
	TASK_CHECK(s1->results[61].members[0].value.u ==
		(736u + 100u + 1u + 6u)); /* partial override: spec 8,9 default */

	spvm_state_delete(s1);
	spvm_state_delete(s2);
	spvm_program_delete(p);
	spvm_context_deinitialize(ctx);
	return 0;
}

/* One uint spec leaf with SpecId 1 and an ordinary uint zero. */
static size_t err_one_spec(spvm_word* out)
{
	builder b;
	b.n = 0;
	header(&b, 40);
	BOP(&b, SpvOpTypeInt, 2, 32, 0);
	BOP(&b, SpvOpConstant, 2, 9, 0u);
	BOP(&b, SpvOpDecorate, 10, SpvDecorationSpecId, 1);
	BOP(&b, SpvOpSpecConstant, 2, 10, 5u);
	memcpy(out, b.w, b.n * sizeof(spvm_word));
	return b.n;
}

static int case_errors(void)
{
	spvm_context_t ctx = spvm_context_initialize();
	spvm_word m[512];
	spvm_spec_error err;
	size_t n;

	/* duplicate table rows */
	n = err_one_spec(m);
	spvm_spec_entry dup[2] = { {1, 1u}, {1, 2u} };
	TASK_CHECK(spvm_program_create_specialized(ctx, m, n, dup, 2, &err) == NULL);
	TASK_CHECK(err.code == SPVM_SPEC_DUPLICATE_ENTRY && err.result_id == 0);

	/* unknown spec id (no definition carries SpecId 99) */
	spvm_spec_entry unknown = { 99, 0u };
	TASK_CHECK(spvm_program_create_specialized(ctx, m, n, &unknown, 1, &err) == NULL);
	TASK_CHECK(err.code == SPVM_SPEC_UNKNOWN_ID && err.result_id == 0);

	/* bool override only accepts 0/1 */
	{
		builder b; b.n = 0; header(&b, 40);
		BOP(&b, SpvOpTypeBool, 5);
		BOP(&b, SpvOpDecorate, 10, SpvDecorationSpecId, 1);
		BOP(&b, SpvOpSpecConstantFalse, 5, 10);
		spvm_spec_entry badbool = { 1, 2u };
		TASK_CHECK(spvm_program_create_specialized(ctx, b.w, b.n, &badbool, 1, &err) == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_INVALID_VALUE && err.result_id == 10);
	}

	/* same SpecId bound to two definitions */
	{
		builder b; b.n = 0; header(&b, 40);
		BOP(&b, SpvOpTypeInt, 2, 32, 0);
		BOP(&b, SpvOpDecorate, 10, SpvDecorationSpecId, 1);
		BOP(&b, SpvOpDecorate, 11, SpvDecorationSpecId, 1);
		BOP(&b, SpvOpSpecConstant, 2, 10, 1u);
		BOP(&b, SpvOpSpecConstant, 2, 11, 2u);
		TASK_CHECK(spvm_program_create_specialized(ctx, b.w, b.n, NULL, 0, &err) == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_AMBIGUOUS_ID && err.result_id == 11);
	}

	/* unsigned division by zero */
	{
		builder b; b.n = 0; header(&b, 40);
		BOP(&b, SpvOpTypeInt, 2, 32, 0);
		BOP(&b, SpvOpConstant, 2, 9, 0u);
		BOP(&b, SpvOpDecorate, 10, SpvDecorationSpecId, 1);
		BOP(&b, SpvOpSpecConstant, 2, 10, 5u);
		BOP(&b, SpvOpSpecConstantOp, 2, 12, SpvOpUDiv, 10, 9);
		TASK_CHECK(spvm_program_create_specialized(ctx, b.w, b.n, NULL, 0, &err) == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_DIVISION_BY_ZERO && err.result_id == 12);
	}

	/* signed INT_MIN / -1 overflows */
	{
		builder b; b.n = 0; header(&b, 40);
		BOP(&b, SpvOpTypeInt, 3, 32, 1);
		BOP(&b, SpvOpConstant, 3, 9, 0x80000000u);
		BOP(&b, SpvOpConstant, 3, 8, 0xFFFFFFFFu);
		BOP(&b, SpvOpSpecConstantOp, 3, 12, SpvOpSDiv, 9, 8);
		TASK_CHECK(spvm_program_create_specialized(ctx, b.w, b.n, NULL, 0, &err) == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_INT_OVERFLOW && err.result_id == 12);
	}

	/* out-of-bounds composite extract */
	{
		builder b; b.n = 0; header(&b, 40);
		BOP(&b, SpvOpTypeInt, 2, 32, 0);
		BOP(&b, SpvOpTypeVector, 6, 2, 2);
		BOP(&b, SpvOpConstant, 2, 9, 1u);
		BOP(&b, SpvOpConstant, 2, 10, 2u);
		BOP(&b, SpvOpSpecConstantComposite, 6, 20, 9, 10);
		BOP(&b, SpvOpSpecConstantOp, 2, 21, SpvOpCompositeExtract, 20, 5);
		TASK_CHECK(spvm_program_create_specialized(ctx, b.w, b.n, NULL, 0, &err) == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_BAD_SHAPE && err.result_id == 21);
	}

	/* vector shuffle with an undefined (0xFFFFFFFF) selector */
	{
		builder b; b.n = 0; header(&b, 40);
		BOP(&b, SpvOpTypeInt, 2, 32, 0);
		BOP(&b, SpvOpTypeVector, 6, 2, 2);
		BOP(&b, SpvOpConstant, 2, 9, 1u);
		BOP(&b, SpvOpConstant, 2, 10, 2u);
		BOP(&b, SpvOpSpecConstantComposite, 6, 20, 9, 10);
		BOP(&b, SpvOpSpecConstantOp, 6, 21, SpvOpVectorShuffle, 20, 20, 0u, 0xFFFFFFFFu);
		TASK_CHECK(spvm_program_create_specialized(ctx, b.w, b.n, NULL, 0, &err) == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_BAD_REFERENCE && err.result_id == 21);
	}

	/* unsupported specialization op (float arithmetic) */
	{
		builder b; b.n = 0; header(&b, 40);
		BOP(&b, SpvOpTypeFloat, 4, 32);
		BOP(&b, SpvOpConstant, 4, 9, float_bits(1.0f));
		BOP(&b, SpvOpConstant, 4, 10, float_bits(2.0f));
		BOP(&b, SpvOpSpecConstantOp, 4, 12, SpvOpFAdd, 9, 10);
		TASK_CHECK(spvm_program_create_specialized(ctx, b.w, b.n, NULL, 0, &err) == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_UNSUPPORTED_OP && err.result_id == 12);
	}

	/* 64-bit scalar specialization is rejected, not silently truncated */
	{
		builder b; b.n = 0; header(&b, 40);
		BOP(&b, SpvOpTypeInt, 7, 64, 0);
		BOP(&b, SpvOpDecorate, 10, SpvDecorationSpecId, 1);
		BOP(&b, SpvOpSpecConstant, 7, 10, 0u, 0u);
		TASK_CHECK(spvm_program_create_specialized(ctx, b.w, b.n, NULL, 0, &err) == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_UNSUPPORTED_TYPE && err.result_id == 10);
	}

	/* matrix specialization is rejected */
	{
		builder b; b.n = 0; header(&b, 40);
		BOP(&b, SpvOpTypeFloat, 4, 32);
		BOP(&b, SpvOpTypeVector, 6, 4, 2);
		BOP(&b, SpvOpTypeMatrix, 8, 6, 2);
		BOP(&b, SpvOpConstant, 4, 9, float_bits(1.0f));
		BOP(&b, SpvOpConstant, 4, 10, float_bits(2.0f));
		BOP(&b, SpvOpConstantComposite, 6, 20, 9, 10);
		BOP(&b, SpvOpSpecConstantComposite, 8, 21, 20, 20);
		TASK_CHECK(spvm_program_create_specialized(ctx, b.w, b.n, NULL, 0, &err) == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_UNSUPPORTED_TYPE && err.result_id == 21);
	}

	/* cyclic specialization dependency */
	{
		builder b; b.n = 0; header(&b, 40);
		BOP(&b, SpvOpTypeInt, 2, 32, 0);
		BOP(&b, SpvOpSpecConstantOp, 2, 10, SpvOpIAdd, 11, 11);
		BOP(&b, SpvOpSpecConstantOp, 2, 11, SpvOpIAdd, 10, 10);
		TASK_CHECK(spvm_program_create_specialized(ctx, b.w, b.n, NULL, 0, &err) == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_CYCLE && err.result_id != 0);
	}

	/* reference to an id that exists but is not a constant definition */
	{
		builder b; b.n = 0; header(&b, 40);
		BOP(&b, SpvOpTypeInt, 2, 32, 0);
		BOP(&b, SpvOpTypeBool, 5);
		BOP(&b, SpvOpSpecConstantOp, 2, 10, SpvOpIAdd, 5, 5);
		TASK_CHECK(spvm_program_create_specialized(ctx, b.w, b.n, NULL, 0, &err) == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_BAD_REFERENCE && err.result_id == 5);
	}

	/* non-positive array length */
	{
		builder b; b.n = 0; header(&b, 40);
		BOP(&b, SpvOpTypeInt, 2, 32, 0);
		BOP(&b, SpvOpDecorate, 10, SpvDecorationSpecId, 1);
		BOP(&b, SpvOpSpecConstant, 2, 10, 0u);
		BOP(&b, SpvOpTypeArray, 17, 2, 10);
		TASK_CHECK(spvm_program_create_specialized(ctx, b.w, b.n, NULL, 0, &err) == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_BAD_SHAPE && err.result_id == 17);
	}

	/* reference to an id beyond the module's id bound */
	{
		builder b; b.n = 0; header(&b, 40);
		BOP(&b, SpvOpTypeInt, 2, 32, 0);
		BOP(&b, SpvOpSpecConstantOp, 2, 10, SpvOpIAdd, 50, 50);
		TASK_CHECK(spvm_program_create_specialized(ctx, b.w, b.n, NULL, 0, &err) == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_BAD_REFERENCE && err.result_id == 50);
	}

	/* composite constituent count does not match the vector type width */
	{
		builder b; b.n = 0; header(&b, 40);
		BOP(&b, SpvOpTypeInt, 2, 32, 0);
		BOP(&b, SpvOpTypeVector, 6, 2, 2);
		BOP(&b, SpvOpConstant, 2, 9, 1u);
		BOP(&b, SpvOpSpecConstantComposite, 6, 20, 9); /* uvec2 needs two */
		TASK_CHECK(spvm_program_create_specialized(ctx, b.w, b.n, NULL, 0, &err) == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_BAD_SHAPE && err.result_id == 20);
	}

	/* composite insert index out of bounds */
	{
		builder b; b.n = 0; header(&b, 40);
		BOP(&b, SpvOpTypeInt, 2, 32, 0);
		BOP(&b, SpvOpTypeVector, 6, 2, 2);
		BOP(&b, SpvOpConstant, 2, 9, 1u);
		BOP(&b, SpvOpConstant, 2, 10, 2u);
		BOP(&b, SpvOpSpecConstantComposite, 6, 20, 9, 10);
		BOP(&b, SpvOpSpecConstantOp, 6, 21, SpvOpCompositeInsert, 9, 20, 7);
		TASK_CHECK(spvm_program_create_specialized(ctx, b.w, b.n, NULL, 0, &err) == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_BAD_SHAPE && err.result_id == 21);
	}

	/* malformed module header */
	{
		spvm_word bad[5] = { 0xdeadbeefu, 0x10000u, 0u, 40u, 0u };
		TASK_CHECK(spvm_program_create_specialized(ctx, bad, 5, NULL, 0, &err) == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_INVALID_MODULE && err.result_id == 0);
	}

	spvm_context_deinitialize(ctx);
	return 0;
}

/* A failed creation must leave the context usable for an immediate retry. */
static int case_retry(void)
{
	spvm_word words[2048];
	size_t n = err_one_spec(words);
	spvm_context_t ctx = spvm_context_initialize();

	spvm_spec_entry dup[2] = { {1, 1u}, {1, 2u} };
	spvm_spec_error err;
	TASK_CHECK(spvm_program_create_specialized(ctx, words, n, dup, 2, &err) == NULL);
	TASK_CHECK(err.code == SPVM_SPEC_DUPLICATE_ENTRY);

	/* same source, now a valid table */
	spvm_spec_entry good = { 1, 42u };
	spvm_program_t p = spvm_program_create_specialized(ctx, words, n, &good, 1, &err);
	TASK_CHECK(p != NULL && err.code == SPVM_SPEC_OK);
	spvm_state_t s = make_state(ctx, p);
	TASK_CHECK(s->results[10].members[0].value.u == 42u);

	spvm_state_delete(s);
	spvm_program_delete(p);
	spvm_context_deinitialize(ctx);
	return 0;
}

const struct task_case task_cases[] = {
	{ "values", "leaf_diamond_ops", case_values_override },
	{ "values", "empty_table_defaults", case_values_default },
	{ "values", "wrapping_and_negate", case_values_wrap },
	{ "layout", "reshape_nested_helper", case_layout },
	{ "layout", "derived_length_nested", case_layout_derived },
	{ "isolation", "variant_independence", case_isolation_variants },
	{ "isolation", "input_ownership", case_ownership },
	{ "isolation", "error_codes", case_errors },
	{ "isolation", "retry_after_failure", case_retry },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
