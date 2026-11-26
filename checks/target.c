#include "task_support/check.h"
#include <spvm/context.h>
#include <spvm/state.h>
#include <spvm/specialize.h>
#include <spvm/spirv.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "constants_module.h"

/* ====================================================================== */
/* SPIR-V word constructors. Every module is a small hand-written module  */
/* with explicit result <id>s and a comment describing each instruction.  */
/* ====================================================================== */

#define W(x) (w[n++] = (spvm_word)(x))
#define OP(op, ...) do { const spvm_word _a[] = { __VA_ARGS__ }; size_t _j; \
	w[n++] = (spvm_word)(((1 + sizeof(_a) / sizeof(_a[0])) << SpvWordCountShift) | (op)); \
	for (_j = 0; _j < sizeof(_a) / sizeof(_a[0]); ++_j) w[n++] = _a[_j]; } while (0)
#define OP0(op) (w[n++] = (spvm_word)((1 << SpvWordCountShift) | (op)))
#define HDR(bound) do { W(SpvMagicNumber); W(0x00010000); W(0); W(bound); W(0); } while (0)

static uint32_t f32_bits(float f)
{
	uint32_t b;
	memcpy(&b, &f, 4);
	return b;
}

/* A program plus the state created from it, already run through main. */
typedef struct {
	spvm_program_t p;
	spvm_state_t s;
} run_t;

static run_t spec_run(const spvm_word* w, size_t count, const spvm_spec_entry* e, size_t ec,
	spvm_spec_error* err, spvm_word main_id)
{
	run_t r;
	r.p = spvm_program_create_specialized(spvm_context_initialize(), w, count, e, ec, err);
	r.s = NULL;
	if (r.p != NULL) {
		r.s = spvm_state_create(r.p);
		spvm_state_prepare(r.s, main_id);
		spvm_state_call_function(r.s);
	}
	return r;
}
static void run_delete(run_t* r)
{
	if (r->s) spvm_state_delete(r->s);
	if (r->p) {
		spvm_context_t c = r->p->context;
		spvm_program_delete(r->p);
		spvm_context_deinitialize(c);
	}
}

/* ====================================================================== */
/* VALUES MODULE                                                          */
/* Scalar spec constants, a dependency diamond, vector ops, select and a  */
/* helper that reads through an AccessChain. main stores every result.    */
/* ====================================================================== */

/* id layout:
 * types   1 void, 2 uint, 3 int, 4 float, 5 bool, 6 uvec2, 8 bvec2, 9 vec2f
 * spec    20 int a (SpecId 0, default 10)
 *         21 uint b (SpecId 1, default 20)
 *         22 float f (SpecId 2, default -0.0)
 *         23 bool t (SpecId 3, default true), 24 bool fbool (SpecId 4, default false)
 * consts  30 uint 2, 31 int 3, 32 uint 0, 33 uint 1
 * spec-op 40 a+3, 41 a*3, 42 (a+3)+(a*3) diamond, 43 -a, 44 (a*3)/3,
 *         45 b/2, 46 a==(a*3/3), 47 -a<a, 48 t&&46, 49 !fbool, 50 select
 * vectors 55 uvec2(b,b/2), 56 extract 55[1], 57 shuffle(55,55)->[1,0],
 *         58 insert b/2 at 55[0], 60 bvec2(t,fbool), 61 !60, 62 select(60,55,57)
 * floats  67 vec2f(f,f), 68 extract 67[0]
 * private 71 uvec2 = 55 ; helper 80 (uint* -> uint)
 * outputs 101 uint outA, 103 float outF, 105 bool outB ; main 90          */
static size_t build_values_module(spvm_word* w)
{
	size_t n = 0;
	HDR(200);
	OP(SpvOpCapability, SpvCapabilityShader);
	OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	OP(SpvOpEntryPoint, SpvExecutionModelFragment, 90, 0x6e69616d, 0, 101, 103, 105);
	OP(SpvOpExecutionMode, 90, SpvExecutionModeOriginUpperLeft);
	OP(SpvOpDecorate, 101, SpvDecorationLocation, 0);
	OP(SpvOpDecorate, 103, SpvDecorationLocation, 1);
	OP(SpvOpDecorate, 105, SpvDecorationLocation, 2);
	OP(SpvOpDecorate, 20, SpvDecorationSpecId, 0);
	OP(SpvOpDecorate, 21, SpvDecorationSpecId, 1);
	OP(SpvOpDecorate, 22, SpvDecorationSpecId, 2);
	OP(SpvOpDecorate, 23, SpvDecorationSpecId, 3);
	OP(SpvOpDecorate, 24, SpvDecorationSpecId, 4);

	OP(SpvOpTypeVoid, 1);
	OP(SpvOpTypeInt, 2, 32, 0);
	OP(SpvOpTypeInt, 3, 32, 1);
	OP(SpvOpTypeFloat, 4, 32);
	OP(SpvOpTypeBool, 5);
	OP(SpvOpTypeVector, 6, 2, 2);
	OP(SpvOpTypeVector, 8, 5, 2);
	OP(SpvOpTypeVector, 9, 4, 2);

	OP(SpvOpSpecConstant, 3, 20, 10);
	OP(SpvOpSpecConstant, 2, 21, 20);
	OP(SpvOpSpecConstant, 4, 22, 0x80000000u);
	OP(SpvOpSpecConstantTrue, 5, 23);
	OP(SpvOpSpecConstantFalse, 5, 24);

	OP(SpvOpConstant, 2, 30, 2);
	OP(SpvOpConstant, 3, 31, 3);
	OP(SpvOpConstant, 2, 32, 0);
	OP(SpvOpConstant, 2, 33, 1);

	OP(SpvOpSpecConstantOp, 3, 40, SpvOpIAdd, 20, 31);          /* a + 3 */
	OP(SpvOpSpecConstantOp, 3, 41, SpvOpIMul, 20, 31);          /* a * 3 */
	OP(SpvOpSpecConstantOp, 3, 42, SpvOpIAdd, 40, 41);          /* diamond join */
	OP(SpvOpSpecConstantOp, 3, 43, SpvOpSNegate, 20);           /* -a */
	OP(SpvOpSpecConstantOp, 3, 44, SpvOpSDiv, 41, 31);          /* (a*3)/3 */
	OP(SpvOpSpecConstantOp, 2, 45, SpvOpUDiv, 21, 30);          /* b/2 */
	OP(SpvOpSpecConstantOp, 5, 46, SpvOpIEqual, 20, 44);        /* a == a */
	OP(SpvOpSpecConstantOp, 5, 47, SpvOpSLessThan, 43, 20);     /* -a < a */
	OP(SpvOpSpecConstantOp, 5, 48, SpvOpLogicalAnd, 23, 46);    /* t && true */
	OP(SpvOpSpecConstantOp, 5, 49, SpvOpLogicalNot, 24);        /* !false */
	OP(SpvOpSpecConstantOp, 3, 50, SpvOpSelect, 46, 40, 43);    /* true ? a+3 : -a */

	OP(SpvOpSpecConstantComposite, 6, 55, 21, 45);              /* uvec2(b, b/2) */
	OP(SpvOpSpecConstantOp, 2, 56, SpvOpCompositeExtract, 55, 1);
	OP(SpvOpSpecConstantOp, 6, 57, SpvOpVectorShuffle, 55, 55, 1, 0);
	OP(SpvOpSpecConstantOp, 6, 58, SpvOpCompositeInsert, 45, 55, 0);
	OP(SpvOpSpecConstantComposite, 8, 60, 23, 24);              /* bvec2(t, f) */
	OP(SpvOpSpecConstantOp, 8, 61, SpvOpLogicalNot, 60);
	OP(SpvOpSpecConstantOp, 6, 62, SpvOpSelect, 60, 55, 57);

	OP(SpvOpSpecConstantComposite, 9, 67, 22, 22);              /* vec2(f, f) */
	OP(SpvOpSpecConstantOp, 4, 68, SpvOpCompositeExtract, 67, 0);

	OP(SpvOpTypePointer, 70, SpvStorageClassPrivate, 6);
	OP(SpvOpVariable, 70, 71, SpvStorageClassPrivate, 55);

	OP(SpvOpTypePointer, 72, SpvStorageClassPrivate, 2);
	OP(SpvOpTypePointer, 73, SpvStorageClassOutput, 2);
	OP(SpvOpVariable, 73, 101, SpvStorageClassOutput);
	OP(SpvOpTypePointer, 74, SpvStorageClassOutput, 4);
	OP(SpvOpVariable, 74, 103, SpvStorageClassOutput);
	OP(SpvOpTypePointer, 75, SpvStorageClassOutput, 5);
	OP(SpvOpVariable, 75, 105, SpvStorageClassOutput);

	OP(SpvOpTypeFunction, 76, 1);
	OP(SpvOpTypeFunction, 77, 2, 72);

	/* helper(uint *p) -> uint { return *p; } */
	OP(SpvOpFunction, 2, 80, SpvFunctionControlMaskNone, 77);
	OP(SpvOpFunctionParameter, 72, 81);
	OP(SpvOpLabel, 82);
	OP(SpvOpLoad, 2, 83, 81);
	OP(SpvOpReturnValue, 83);
	OP0(SpvOpFunctionEnd);

	/* void main() */
	OP(SpvOpFunction, 1, 90, SpvFunctionControlMaskNone, 76);
	OP(SpvOpLabel, 91);
	OP(SpvOpAccessChain, 72, 92, 71, 33);          /* &private[1] */
	OP(SpvOpFunctionCall, 2, 93, 80, 92);          /* helper read */
	OP(SpvOpStore, 101, 42);                        /* diamond result */
	OP(SpvOpStore, 103, 68);                        /* float bits */
	OP(SpvOpStore, 105, 48);                        /* bool */
	OP0(SpvOpReturn);
	OP0(SpvOpFunctionEnd);
	return n;
}

static int values_scalar_and_dag(void)
{
	spvm_word w[1024];
	size_t count = build_values_module(w);
	spvm_spec_entry e[2] = { {0, 7}, {1, 10} };     /* a=7 (signed), b=10 (uint) */
	spvm_spec_error err;
	run_t r = spec_run(w, count, e, 2, &err, 90);
	TASK_CHECK(r.p != NULL && r.s != NULL);
	TASK_CHECK(err.code == SPVM_SPEC_OK);

	/* independently computed expectations for a=7, b=10 */
	int32_t a = 7; uint32_t b = 10;
	TASK_CHECK(r.s->results[40].members[0].value.s == a + 3);
	TASK_CHECK(r.s->results[41].members[0].value.s == a * 3);
	TASK_CHECK(r.s->results[42].members[0].value.s == (a + 3) + (a * 3));
	TASK_CHECK(r.s->results[43].members[0].value.s == -a);
	TASK_CHECK(r.s->results[44].members[0].value.s == a);
	TASK_CHECK(r.s->results[45].members[0].value.u == b / 2);
	TASK_CHECK(r.s->results[46].members[0].value.b == 1);
	TASK_CHECK(r.s->results[47].members[0].value.b == 1);
	TASK_CHECK(r.s->results[48].members[0].value.b == 1);
	TASK_CHECK(r.s->results[49].members[0].value.b == 1);
	TASK_CHECK(r.s->results[50].members[0].value.s == a + 3);

	/* values actually produced by running the shader, not just the API */
	TASK_CHECK(r.s->results[101].members[0].value.s == (a + 3) + (a * 3));
	TASK_CHECK(r.s->results[105].members[0].value.b == 1);
	TASK_CHECK(r.s->results[93].members[0].value.u == b / 2); /* helper via AccessChain */
	run_delete(&r);
	return 0;
}

static int values_defaults_empty_table(void)
{
	spvm_word w[1024];
	size_t count = build_values_module(w);
	spvm_spec_error err;
	/* empty table: defaults must still be evaluated, not ignored */
	run_t r = spec_run(w, count, NULL, 0, &err, 90);
	TASK_CHECK(r.p != NULL && r.s != NULL);
	TASK_CHECK(err.code == SPVM_SPEC_OK);
	TASK_CHECK(r.s->results[20].members[0].value.s == 10);
	TASK_CHECK(r.s->results[21].members[0].value.u == 20);
	TASK_CHECK(r.s->results[42].members[0].value.s == 43); /* (10+3)+(10*3) */
	TASK_CHECK(r.s->results[55].members[0].value.u == 20);
	TASK_CHECK(r.s->results[55].members[1].value.u == 10);
	TASK_CHECK(r.s->results[101].members[0].value.s == 43);
	run_delete(&r);
	return 0;
}

static int values_vector_ops(void)
{
	spvm_word w[1024];
	size_t count = build_values_module(w);
	spvm_spec_entry e[2] = { {0, 7}, {1, 10} };
	spvm_spec_error err;
	run_t r = spec_run(w, count, e, 2, &err, 90);
	TASK_CHECK(r.p != NULL && r.s != NULL);

	uint32_t b = 10, half = b / 2;
	/* 55 = (b, b/2) */
	TASK_CHECK(r.s->results[55].member_count == 2);
	TASK_CHECK(r.s->results[55].members[0].value.u == b);
	TASK_CHECK(r.s->results[55].members[1].value.u == half);
	/* 56 extract index 1 */
	TASK_CHECK(r.s->results[56].members[0].value.u == half);
	/* 57 shuffle [1,0] of 55 -> (b/2, b) */
	TASK_CHECK(r.s->results[57].members[0].value.u == half);
	TASK_CHECK(r.s->results[57].members[1].value.u == b);
	/* 58 insert b/2 at index 0 -> (b/2, b/2) */
	TASK_CHECK(r.s->results[58].members[0].value.u == half);
	TASK_CHECK(r.s->results[58].members[1].value.u == half);
	/* 60 (true,false), 61 logical-not -> (false,true) */
	TASK_CHECK(r.s->results[61].members[0].value.b == 0);
	TASK_CHECK(r.s->results[61].members[1].value.b == 1);
	/* 62 component select: cond (true,false) over 55 and (b/2,b) -> (b, b) */
	TASK_CHECK(r.s->results[62].members[0].value.u == b);
	TASK_CHECK(r.s->results[62].members[1].value.u == b);
	run_delete(&r);
	return 0;
}

static int values_integer_wrap_and_divide(void)
{
	spvm_word w[1024];
	size_t n = 0;
	HDR(80);
	OP(SpvOpCapability, SpvCapabilityShader);
	OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	OP(SpvOpEntryPoint, SpvExecutionModelFragment, 40, 0x6e69616d, 0);
	OP(SpvOpExecutionMode, 40, SpvExecutionModeOriginUpperLeft);
	OP(SpvOpDecorate, 10, SpvDecorationSpecId, 0);
	OP(SpvOpDecorate, 11, SpvDecorationSpecId, 1);
	OP(SpvOpDecorate, 14, SpvDecorationSpecId, 2);
	OP(SpvOpTypeVoid, 1);
	OP(SpvOpTypeInt, 2, 32, 0); /* uint */
	OP(SpvOpTypeInt, 3, 32, 1); /* int */
	OP(SpvOpSpecConstant, 2, 10, 0);
	OP(SpvOpSpecConstant, 3, 11, 0);
	OP(SpvOpSpecConstant, 2, 14, 0);
	OP(SpvOpConstant, 2, 12, 1);
	OP(SpvOpConstant, 2, 13, 0x100);
	/* unsigned wrap: 0xFFFFFFFF + 1 -> 0 ; 0x10000000 * 0x100 -> 0 */
	OP(SpvOpSpecConstantOp, 2, 20, SpvOpIAdd, 10, 12);
	OP(SpvOpSpecConstantOp, 2, 21, SpvOpIMul, 14, 13);
	/* signed negate of INT_MIN stays INT_MIN by two's-complement wrap */
	OP(SpvOpSpecConstantOp, 3, 22, SpvOpSNegate, 11);
	OP(SpvOpFunction, 1, 40, SpvFunctionControlMaskNone, 1);
	OP(SpvOpLabel, 41);
	OP0(SpvOpReturn);
	OP0(SpvOpFunctionEnd);
	(void)n;

	spvm_spec_entry e[3] = { {0, 0xFFFFFFFFu}, {1, (uint32_t)INT32_MIN}, {2, 0x10000000u} };
	spvm_spec_error err;
	run_t r = spec_run(w, n, e, 3, &err, 40);
	TASK_CHECK(r.p != NULL && r.s != NULL);
	TASK_CHECK(r.s->results[20].members[0].value.u == (0xFFFFFFFFu + 1u));
	TASK_CHECK(r.s->results[21].members[0].value.u == (0x10000000u * 0x100u));
	TASK_CHECK((uint32_t)r.s->results[22].members[0].value.u == (uint32_t)INT32_MIN);
	run_delete(&r);
	return 0;
}

static int values_negzero_bit_preservation(void)
{
	spvm_word w[1024];
	size_t count = build_values_module(w);

	/* default: f is negative zero; composite then extract must keep the bits */
	spvm_spec_error err;
	run_t r = spec_run(w, count, NULL, 0, &err, 90);
	TASK_CHECK(r.p != NULL && r.s != NULL);
	TASK_CHECK(f32_bits(r.s->results[68].members[0].value.f) == 0x80000000u);
	TASK_CHECK(f32_bits(r.s->results[103].members[0].value.f) == 0x80000000u);
	run_delete(&r);

	/* override with the pi bit pattern; no float arithmetic is performed */
	uint32_t pi = f32_bits(3.14159265f);
	spvm_spec_entry e[1] = { {2, pi} };
	r = spec_run(w, count, e, 1, &err, 90);
	TASK_CHECK(r.p != NULL && r.s != NULL);
	TASK_CHECK(f32_bits(r.s->results[22].members[0].value.f) == pi);
	TASK_CHECK(f32_bits(r.s->results[68].members[0].value.f) == pi);
	TASK_CHECK(f32_bits(r.s->results[103].members[0].value.f) == pi);
	run_delete(&r);
	return 0;
}

static int values_bool_override(void)
{
	spvm_word w[1024];
	size_t count = build_values_module(w);
	/* force the true spec constant to false; 48 = t && true becomes false */
	spvm_spec_entry e[1] = { {3, 0} };
	spvm_spec_error err;
	run_t r = spec_run(w, count, e, 1, &err, 90);
	TASK_CHECK(r.p != NULL && r.s != NULL);
	TASK_CHECK(r.s->results[23].members[0].value.b == 0);
	TASK_CHECK(r.s->results[48].members[0].value.b == 0);
	TASK_CHECK(r.s->results[105].members[0].value.b == 0);
	run_delete(&r);
	return 0;
}

/* Extra opcodes and nested-struct handling. Ordinary constants (12,13) feed the
 * spec-op chain; 31 is struct{ struct{uint,uint}, uint }. */
static size_t build_extra_module(spvm_word* w)
{
	size_t n = 0;
	HDR(120);
	OP(SpvOpCapability, SpvCapabilityShader);
	OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	OP(SpvOpEntryPoint, SpvExecutionModelFragment, 70, 0x6e69616d, 0, 90, 91, 92);
	OP(SpvOpExecutionMode, 70, SpvExecutionModeOriginUpperLeft);
	OP(SpvOpDecorate, 90, SpvDecorationLocation, 0);
	OP(SpvOpDecorate, 92, SpvDecorationLocation, 2);
	OP(SpvOpDecorate, 10, SpvDecorationSpecId, 0);
	OP(SpvOpDecorate, 11, SpvDecorationSpecId, 1);
	OP(SpvOpDecorate, 23, SpvDecorationSpecId, 2);

	OP(SpvOpTypeVoid, 1);
	OP(SpvOpTypeInt, 2, 32, 0); /* uint */
	OP(SpvOpTypeInt, 3, 32, 1); /* int */
	OP(SpvOpTypeBool, 5);
	OP(SpvOpSpecConstant, 3, 10, 4);   /* x (signed) */
	OP(SpvOpSpecConstant, 2, 11, 9);   /* y (unsigned) */
	OP(SpvOpConstant, 2, 12, 3);       /* ordinary constant 3 */
	OP(SpvOpConstant, 2, 13, 100);     /* ordinary constant 100 */
	OP(SpvOpSpecConstantTrue, 5, 23);
	OP(SpvOpSpecConstantFalse, 5, 25);

	OP(SpvOpSpecConstantOp, 3, 20, SpvOpISub, 10, 12);        /* x - 3 */
	OP(SpvOpSpecConstantOp, 5, 21, SpvOpULessThan, 11, 13);   /* y < 100 */
	OP(SpvOpSpecConstantOp, 5, 22, SpvOpULessThan, 13, 11);   /* 100 < y */
	OP(SpvOpSpecConstantOp, 5, 24, SpvOpLogicalOr, 25, 21);   /* false || (y<100) */

	OP(SpvOpTypeStruct, 30, 2, 2);                 /* inner { uint, uint } */
	OP(SpvOpTypeStruct, 31, 30, 2);               /* outer { inner, uint } */
	OP(SpvOpConstantComposite, 30, 32, 12, 13);   /* inner { 3, 100 } */
	OP(SpvOpSpecConstantComposite, 31, 33, 32, 11);
	OP(SpvOpSpecConstantOp, 2, 34, SpvOpCompositeExtract, 33, 0, 1); /* inner[1] */
	OP(SpvOpSpecConstantOp, 31, 35, SpvOpCompositeInsert, 11, 33, 0, 0);
	OP(SpvOpSpecConstantOp, 2, 36, SpvOpCompositeExtract, 35, 0, 0); /* inserted y */

	OP(SpvOpTypePointer, 40, SpvStorageClassOutput, 2);
	OP(SpvOpVariable, 40, 90, SpvStorageClassOutput);
	OP(SpvOpVariable, 40, 91, SpvStorageClassOutput);
	OP(SpvOpTypePointer, 41, SpvStorageClassOutput, 5);
	OP(SpvOpVariable, 41, 92, SpvStorageClassOutput);

	OP(SpvOpTypeFunction, 50, 1);
	OP(SpvOpFunction, 1, 70, SpvFunctionControlMaskNone, 50);
	OP(SpvOpLabel, 71);
	OP(SpvOpStore, 90, 20);
	OP(SpvOpStore, 91, 36);
	OP(SpvOpStore, 92, 24);
	OP0(SpvOpReturn);
	OP0(SpvOpFunctionEnd);
	return n;
}

static int values_extra_ops_and_nested_struct(void)
{
	spvm_word w[1024];
	size_t count = build_extra_module(w);
	spvm_spec_error err;

	/* default: x=4, y=9 */
	run_t r = spec_run(w, count, NULL, 0, &err, 70);
	TASK_CHECK(r.p != NULL && r.s != NULL);
	TASK_CHECK(r.s->results[20].members[0].value.s == 4 - 3);
	TASK_CHECK(r.s->results[21].members[0].value.b == 1);
	TASK_CHECK(r.s->results[22].members[0].value.b == 0);
	TASK_CHECK(r.s->results[24].members[0].value.b == 1);
	TASK_CHECK(r.s->results[33].members[0].member_count == 2);
	TASK_CHECK(r.s->results[33].members[0].members[1].value.u == 100);
	TASK_CHECK(r.s->results[34].members[0].value.u == 100);
	TASK_CHECK(r.s->results[35].members[0].members[0].value.u == 9);
	TASK_CHECK(r.s->results[35].members[0].members[1].value.u == 100);
	TASK_CHECK(r.s->results[35].members[1].value.u == 9);
	TASK_CHECK(r.s->results[36].members[0].value.u == 9);
	TASK_CHECK(r.s->results[90].members[0].value.s == 1);
	TASK_CHECK(r.s->results[91].members[0].value.u == 9);
	TASK_CHECK(r.s->results[92].members[0].value.b == 1);
	run_delete(&r);

	/* override x=10, y=2: subtract changes, comparisons flip as expected */
	spvm_spec_entry e[2] = { {0, 10}, {1, 2} };
	r = spec_run(w, count, e, 2, &err, 70);
	TASK_CHECK(r.p != NULL && r.s != NULL);
	TASK_CHECK(r.s->results[20].members[0].value.s == 7);
	TASK_CHECK(r.s->results[21].members[0].value.b == 1);  /* 2 < 100 */
	TASK_CHECK(r.s->results[22].members[0].value.b == 0);
	TASK_CHECK(r.s->results[34].members[0].value.u == 100); /* ordinary-constant path */
	TASK_CHECK(r.s->results[36].members[0].value.u == 2);
	TASK_CHECK(r.s->results[91].members[0].value.u == 2);
	run_delete(&r);
	return 0;
}

/* Specialization mirror of checks/constants_module.h: the same ids, types and
 * shader, but the constants enter through specialization. With no overrides the
 * specialized program must produce byte-identical results to the ordinary one. */
static size_t build_spec_mirror_module(spvm_word* w)
{
	size_t n = 0;
	HDR(60);
	OP(SpvOpCapability, SpvCapabilityShader);
	OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	OP(SpvOpEntryPoint, SpvExecutionModelFragment, 40, 0x6e69616d, 0, 25);
	OP(SpvOpExecutionMode, 40, SpvExecutionModeOriginUpperLeft);
	OP(SpvOpDecorate, 25, SpvDecorationLocation, 0);
	OP(SpvOpDecorate, 13, SpvDecorationSpecId, 0); /* default 7 */
	OP(SpvOpDecorate, 14, SpvDecorationSpecId, 1); /* default 11 */
	OP(SpvOpDecorate, 15, SpvDecorationSpecId, 2); /* default -0.0 */
	OP(SpvOpTypeVoid, 1);
	OP(SpvOpTypeInt, 2, 32, 0); OP(SpvOpTypeInt, 3, 32, 1);
	OP(SpvOpTypeFloat, 4, 32); OP(SpvOpTypeBool, 5);
	OP(SpvOpTypeVector, 6, 2, 2);
	OP(SpvOpConstant, 2, 10, 0); OP(SpvOpConstant, 2, 11, 1);
	OP(SpvOpConstant, 2, 12, 3);
	OP(SpvOpSpecConstant, 2, 13, 7);
	OP(SpvOpSpecConstant, 2, 14, 11);
	OP(SpvOpSpecConstant, 4, 15, 0x80000000u);
	OP(SpvOpSpecConstantComposite, 6, 16, 13, 14);
	OP(SpvOpTypeArray, 17, 2, 12);
	OP(SpvOpSpecConstantComposite, 17, 18, 13, 14, 13);
	OP(SpvOpTypeStruct, 19, 17, 6, 4);
	OP(SpvOpSpecConstantComposite, 19, 20, 18, 16, 15);
	OP(SpvOpTypePointer, 21, SpvStorageClassPrivate, 19);
	OP(SpvOpVariable, 21, 22, SpvStorageClassPrivate, 20);
	OP(SpvOpTypePointer, 23, SpvStorageClassPrivate, 2);
	OP(SpvOpTypePointer, 24, SpvStorageClassOutput, 2);
	OP(SpvOpVariable, 24, 25, SpvStorageClassOutput);
	OP(SpvOpTypeFunction, 26, 1); OP(SpvOpTypeFunction, 27, 2, 23);
	OP(SpvOpFunction, 2, 30, SpvFunctionControlMaskNone, 27);
	OP(SpvOpFunctionParameter, 23, 31); OP(SpvOpLabel, 32);
	OP(SpvOpLoad, 2, 33, 31); OP(SpvOpReturnValue, 33); OP0(SpvOpFunctionEnd);
	OP(SpvOpFunction, 1, 40, SpvFunctionControlMaskNone, 26);
	OP(SpvOpLabel, 41);
	OP(SpvOpAccessChain, 23, 42, 22, 10, 11);
	OP(SpvOpFunctionCall, 2, 43, 30, 42);
	OP(SpvOpAccessChain, 23, 44, 22, 11, 10);
	OP(SpvOpLoad, 2, 45, 44); OP(SpvOpIAdd, 2, 46, 43, 45);
	OP(SpvOpStore, 25, 46); OP0(SpvOpReturn); OP0(SpvOpFunctionEnd);
	return n;
}

static int values_parity_with_ordinary_module(void)
{
	spvm_word ordinary[512];
	size_t on = constants_module(ordinary);
	spvm_word mirror[512];
	size_t mn = build_spec_mirror_module(mirror);

	spvm_context_t ctx = spvm_context_initialize();

	/* ordinary-constant reference program */
	spvm_program_t po = spvm_program_create(ctx, ordinary, on);
	spvm_state_t so = spvm_state_create(po);
	spvm_state_prepare(so, 40);
	spvm_state_call_function(so);

	/* specialized program, default values, no overrides */
	spvm_spec_error err;
	spvm_program_t ps = spvm_program_create_specialized(ctx, mirror, mn, NULL, 0, &err);
	TASK_CHECK(ps != NULL && err.code == SPVM_SPEC_OK);
	spvm_state_t ss = spvm_state_create(ps);
	spvm_state_prepare(ss, 40);
	spvm_state_call_function(ss);

	/* identical shapes, helper reads, output and negative-zero bit pattern */
	TASK_CHECK(ss->results[22].member_count == so->results[22].member_count);
	TASK_CHECK(ss->results[22].members[0].member_count == so->results[22].members[0].member_count);
	TASK_CHECK(ss->results[22].members[0].members[1].value.u == 11);
	TASK_CHECK(ss->results[43].members[0].value.u == so->results[43].members[0].value.u);
	TASK_CHECK(ss->results[25].members[0].value.u == so->results[25].members[0].value.u);
	TASK_CHECK(ss->results[25].members[0].value.u == 18);
	uint32_t spec_bits = f32_bits(ss->results[22].members[2].value.f);
	uint32_t ord_bits = f32_bits(so->results[22].members[2].value.f);
	TASK_CHECK(spec_bits == 0x80000000u && spec_bits == ord_bits);

	spvm_state_delete(ss); spvm_program_delete(ps);
	spvm_state_delete(so); spvm_program_delete(po);
	spvm_context_deinitialize(ctx);
	return 0;
}


/* ====================================================================== */
/* LAYOUT MODULE                                                          */
/* A spec integer sizes a uint array embedded in a struct; a Private var  */
/* is initialized from the spec composite. A helper reads through it.      */
/* ====================================================================== */

/* types 1 void 2 uint 3 int ; spec 20 length (SpecId0 default 2),
 * 21 fill (SpecId1 default 7); const 30=0 31=1 32=2
 * 40 uint[length], 41 struct{ uint[length], uint }
 * 45 array composite (fill, 2) [extra tail zero-fills when grown]
 * 46 struct composite (45, fill)
 * 51 Private struct = 46 ; helper 60 ; main 70 ; outputs 81,83          */
static size_t build_layout_module(spvm_word* w)
{
	size_t n = 0;
	HDR(120);
	OP(SpvOpCapability, SpvCapabilityShader);
	OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	OP(SpvOpEntryPoint, SpvExecutionModelFragment, 70, 0x6e69616d, 0, 81, 83);
	OP(SpvOpExecutionMode, 70, SpvExecutionModeOriginUpperLeft);
	OP(SpvOpDecorate, 81, SpvDecorationLocation, 0);
	OP(SpvOpDecorate, 83, SpvDecorationLocation, 1);
	OP(SpvOpDecorate, 20, SpvDecorationSpecId, 0);
	OP(SpvOpDecorate, 21, SpvDecorationSpecId, 1);

	OP(SpvOpTypeVoid, 1);
	OP(SpvOpTypeInt, 2, 32, 0);
	OP(SpvOpTypeInt, 3, 32, 1);
	OP(SpvOpSpecConstant, 3, 20, 2);
	OP(SpvOpSpecConstant, 2, 21, 7);
	OP(SpvOpConstant, 2, 30, 0);
	OP(SpvOpConstant, 2, 31, 1);
	OP(SpvOpConstant, 2, 32, 2);

	OP(SpvOpTypeArray, 40, 2, 20);
	OP(SpvOpTypeStruct, 41, 40, 2);
	OP(SpvOpSpecConstantComposite, 40, 45, 21, 32);   /* (fill, 2) */
	OP(SpvOpSpecConstantComposite, 41, 46, 45, 21);

	OP(SpvOpTypePointer, 50, SpvStorageClassPrivate, 41);
	OP(SpvOpVariable, 50, 51, SpvStorageClassPrivate, 46);

	OP(SpvOpTypePointer, 52, SpvStorageClassPrivate, 2);
	OP(SpvOpTypePointer, 53, SpvStorageClassOutput, 2);
	OP(SpvOpVariable, 53, 81, SpvStorageClassOutput);
	OP(SpvOpVariable, 53, 83, SpvStorageClassOutput);

	OP(SpvOpTypeFunction, 55, 1);
	OP(SpvOpTypeFunction, 56, 2, 52);
	OP(SpvOpFunction, 2, 60, SpvFunctionControlMaskNone, 56);
	OP(SpvOpFunctionParameter, 52, 61);
	OP(SpvOpLabel, 62);
	OP(SpvOpLoad, 2, 63, 61);
	OP(SpvOpReturnValue, 63);
	OP0(SpvOpFunctionEnd);

	OP(SpvOpFunction, 1, 70, SpvFunctionControlMaskNone, 55);
	OP(SpvOpLabel, 71);
	/* helper(&private.array[1]) */
	OP(SpvOpAccessChain, 52, 72, 51, 30, 31);
	OP(SpvOpFunctionCall, 2, 73, 60, 72);
	OP(SpvOpStore, 81, 73);
	/* private.field1 (the trailing uint) */
	OP(SpvOpAccessChain, 52, 74, 51, 31);
	OP(SpvOpLoad, 2, 75, 74);
	OP(SpvOpStore, 83, 75);
	OP0(SpvOpReturn);
	OP0(SpvOpFunctionEnd);
	return n;
}

static int layout_default_shape(void)
{
	spvm_word w[1024];
	size_t count = build_layout_module(w);
	spvm_spec_error err;
	run_t r = spec_run(w, count, NULL, 0, &err, 70);
	TASK_CHECK(r.p != NULL && r.s != NULL);
	/* default length is 2 */
	TASK_CHECK(r.s->results[40].member_count == 2);
	TASK_CHECK(r.s->results[51].members[0].member_count == 2);
	TASK_CHECK(r.s->results[51].member_count == 2);
	TASK_CHECK(r.s->results[51].members[0].members[0].value.u == 7);
	TASK_CHECK(r.s->results[51].members[0].members[1].value.u == 2);
	TASK_CHECK(r.s->results[51].members[1].value.u == 7);
	TASK_CHECK(r.s->results[81].members[0].value.u == 2);  /* helper read */
	TASK_CHECK(r.s->results[83].members[0].value.u == 7);
	run_delete(&r);
	return 0;
}

static int layout_grown_shape(void)
{
	spvm_word w[1024];
	size_t count = build_layout_module(w);
	spvm_spec_entry e[2] = { {0, 5}, {1, 9} };   /* length 5, fill 9 */
	spvm_spec_error err;
	run_t r = spec_run(w, count, e, 2, &err, 70);
	TASK_CHECK(r.p != NULL && r.s != NULL);
	TASK_CHECK(r.s->results[40].member_count == 5);
	TASK_CHECK(r.s->results[45].member_count == 5);
	TASK_CHECK(r.s->results[51].members[0].member_count == 5);
	/* listed constituents survive, grown tail is zero-filled */
	TASK_CHECK(r.s->results[45].members[0].value.u == 9);
	TASK_CHECK(r.s->results[45].members[1].value.u == 2);
	TASK_CHECK(r.s->results[45].members[2].value.u == 0);
	TASK_CHECK(r.s->results[45].members[4].value.u == 0);
	TASK_CHECK(r.s->results[51].members[0].members[3].value.u == 0);
	TASK_CHECK(r.s->results[51].members[1].value.u == 9);
	/* runtime access chain still reaches element 1 of the longer array */
	TASK_CHECK(r.s->results[81].members[0].value.u == 2);
	run_delete(&r);
	return 0;
}

static int layout_nested_aggregate_state_shape(void)
{
	spvm_word w[1024];
	size_t count = build_layout_module(w);
	spvm_spec_entry e[1] = { {0, 4} };
	spvm_spec_error err;
	run_t r = spec_run(w, count, e, 1, &err, 70);
	TASK_CHECK(r.p != NULL && r.s != NULL);
	/* every state built from the program must share the specialized shape */
	spvm_state_t s2 = spvm_state_create(r.p);
	TASK_CHECK(s2->results[51].members[0].member_count == 4);
	TASK_CHECK(s2->results[40].member_count == 4);
	TASK_CHECK(s2->results[46].members[0].member_count == 4);
	spvm_state_delete(s2);
	run_delete(&r);
	return 0;
}

/* ====================================================================== */
/* ERROR / ISOLATION                                                      */
/* ====================================================================== */

/* minimal module with one decorated scalar spec constant %10 (uint) */
static size_t build_min_spec(spvm_word* w, spvm_word spec_literal)
{
	size_t n = 0;
	HDR(40);
	OP(SpvOpCapability, SpvCapabilityShader);
	OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	OP(SpvOpEntryPoint, SpvExecutionModelFragment, 20, 0x6e69616d, 0);
	OP(SpvOpExecutionMode, 20, SpvExecutionModeOriginUpperLeft);
	OP(SpvOpDecorate, 10, SpvDecorationSpecId, 0);
	OP(SpvOpTypeVoid, 1);
	OP(SpvOpTypeInt, 2, 32, 0);
	OP(SpvOpSpecConstant, 2, 10, spec_literal);
	OP(SpvOpTypeFunction, 5, 1);
	OP(SpvOpFunction, 1, 20, SpvFunctionControlMaskNone, 5);
	OP(SpvOpLabel, 21);
	OP0(SpvOpReturn);
	OP0(SpvOpFunctionEnd);
	return n;
}

static int isolation_error_coverage(void)
{
	spvm_word w[1024];
	spvm_spec_error err;
	spvm_context_t ctx = spvm_context_initialize();

	/* unknown spec id */
	{
		size_t count = build_min_spec(w, 1);
		spvm_spec_entry e[1] = { {99, 1} };
		spvm_program_t p = spvm_program_create_specialized(ctx, w, count, e, 1, &err);
		TASK_CHECK(p == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_ERR_UNKNOWN_SPEC_ID);
		TASK_CHECK(err.result_id == 0);
	}
	/* duplicate table entry */
	{
		size_t count = build_min_spec(w, 1);
		spvm_spec_entry e[2] = { {0, 1}, {0, 2} };
		spvm_program_t p = spvm_program_create_specialized(ctx, w, count, e, 2, &err);
		TASK_CHECK(p == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_ERR_DUPLICATE_ENTRY);
	}
	/* same spec id bound to two definitions */
	{
		size_t n = 0;
		HDR(40);
		OP(SpvOpCapability, SpvCapabilityShader);
		OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
		OP(SpvOpEntryPoint, SpvExecutionModelFragment, 20, 0x6e69616d, 0);
		OP(SpvOpExecutionMode, 20, SpvExecutionModeOriginUpperLeft);
		OP(SpvOpDecorate, 10, SpvDecorationSpecId, 5);
		OP(SpvOpDecorate, 11, SpvDecorationSpecId, 5);
		OP(SpvOpTypeVoid, 1);
		OP(SpvOpTypeInt, 2, 32, 0);
		OP(SpvOpSpecConstant, 2, 10, 1);
		OP(SpvOpSpecConstant, 2, 11, 2);
		OP(SpvOpTypeFunction, 5, 1);
		OP(SpvOpFunction, 1, 20, SpvFunctionControlMaskNone, 5);
		OP(SpvOpLabel, 21);
		OP0(SpvOpReturn);
		OP0(SpvOpFunctionEnd);
		spvm_spec_entry e[1] = { {5, 3} };
		spvm_program_t p = spvm_program_create_specialized(ctx, w, n, e, 1, &err);
		TASK_CHECK(p == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_ERR_AMBIGUOUS_SPEC_ID);
		TASK_CHECK(err.result_id == 10 || err.result_id == 11);
	}
	/* invalid bool override */
	{
		size_t n = 0;
		HDR(40);
		OP(SpvOpCapability, SpvCapabilityShader);
		OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
		OP(SpvOpEntryPoint, SpvExecutionModelFragment, 20, 0x6e69616d, 0);
		OP(SpvOpExecutionMode, 20, SpvExecutionModeOriginUpperLeft);
		OP(SpvOpDecorate, 10, SpvDecorationSpecId, 0);
		OP(SpvOpTypeVoid, 1);
		OP(SpvOpTypeBool, 5);
		OP(SpvOpSpecConstantTrue, 5, 10);
		OP(SpvOpTypeFunction, 6, 1);
		OP(SpvOpFunction, 1, 20, SpvFunctionControlMaskNone, 6);
		OP(SpvOpLabel, 21);
		OP0(SpvOpReturn);
		OP0(SpvOpFunctionEnd);
		spvm_spec_entry e[1] = { {0, 2} };
		spvm_program_t p = spvm_program_create_specialized(ctx, w, n, e, 1, &err);
		TASK_CHECK(p == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_ERR_INVALID_BOOL);
		TASK_CHECK(err.result_id == 10);
	}
	/* divide by zero */
	{
		size_t n = 0;
		HDR(40);
		OP(SpvOpCapability, SpvCapabilityShader);
		OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
		OP(SpvOpEntryPoint, SpvExecutionModelFragment, 20, 0x6e69616d, 0);
		OP(SpvOpExecutionMode, 20, SpvExecutionModeOriginUpperLeft);
		OP(SpvOpDecorate, 10, SpvDecorationSpecId, 0);
		OP(SpvOpTypeVoid, 1);
		OP(SpvOpTypeInt, 2, 32, 0);
		OP(SpvOpSpecConstant, 2, 10, 1);
		OP(SpvOpSpecConstantOp, 2, 12, SpvOpUDiv, 10, 10);
		OP(SpvOpTypeFunction, 5, 1);
		OP(SpvOpFunction, 1, 20, SpvFunctionControlMaskNone, 5);
		OP(SpvOpLabel, 21);
		OP0(SpvOpReturn);
		OP0(SpvOpFunctionEnd);
		spvm_spec_entry e[1] = { {0, 0} };
		spvm_program_t p = spvm_program_create_specialized(ctx, w, n, e, 1, &err);
		TASK_CHECK(p == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_ERR_DIVIDE_BY_ZERO);
		TASK_CHECK(err.result_id == 12);
	}
	/* INT_MIN / -1 */
	{
		size_t n = 0;
		HDR(40);
		OP(SpvOpCapability, SpvCapabilityShader);
		OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
		OP(SpvOpEntryPoint, SpvExecutionModelFragment, 20, 0x6e69616d, 0);
		OP(SpvOpExecutionMode, 20, SpvExecutionModeOriginUpperLeft);
		OP(SpvOpDecorate, 10, SpvDecorationSpecId, 0);
		OP(SpvOpTypeVoid, 1);
		OP(SpvOpTypeInt, 3, 32, 1);
		OP(SpvOpSpecConstant, 3, 10, 0);
		OP(SpvOpConstant, 3, 11, (uint32_t)-1);
		OP(SpvOpSpecConstantOp, 3, 12, SpvOpSDiv, 10, 11);
		OP(SpvOpTypeFunction, 5, 1);
		OP(SpvOpFunction, 1, 20, SpvFunctionControlMaskNone, 5);
		OP(SpvOpLabel, 21);
		OP0(SpvOpReturn);
		OP0(SpvOpFunctionEnd);
		spvm_spec_entry e[1] = { {0, (uint32_t)INT32_MIN} };
		spvm_program_t p = spvm_program_create_specialized(ctx, w, n, e, 1, &err);
		TASK_CHECK(p == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_ERR_SIGNED_OVERFLOW);
		TASK_CHECK(err.result_id == 12);
	}
	/* unsupported op: float arithmetic inside SpecConstantOp */
	{
		size_t n = 0;
		HDR(40);
		OP(SpvOpCapability, SpvCapabilityShader);
		OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
		OP(SpvOpEntryPoint, SpvExecutionModelFragment, 20, 0x6e69616d, 0);
		OP(SpvOpExecutionMode, 20, SpvExecutionModeOriginUpperLeft);
		OP(SpvOpTypeVoid, 1);
		OP(SpvOpTypeFloat, 4, 32);
		OP(SpvOpConstant, 4, 10, 0);
		OP(SpvOpSpecConstantOp, 4, 12, SpvOpFAdd, 10, 10);
		OP(SpvOpTypeFunction, 5, 1);
		OP(SpvOpFunction, 1, 20, SpvFunctionControlMaskNone, 5);
		OP(SpvOpLabel, 21);
		OP0(SpvOpReturn);
		OP0(SpvOpFunctionEnd);
		spvm_program_t p = spvm_program_create_specialized(ctx, w, n, NULL, 0, &err);
		TASK_CHECK(p == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_ERR_UNSUPPORTED_OP);
		TASK_CHECK(err.result_id == 12);
	}
	/* 64-bit specialization scalar is rejected, not silently truncated */
	{
		size_t n = 0;
		HDR(40);
		OP(SpvOpCapability, SpvCapabilityShader);
		OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
		OP(SpvOpEntryPoint, SpvExecutionModelFragment, 20, 0x6e69616d, 0);
		OP(SpvOpExecutionMode, 20, SpvExecutionModeOriginUpperLeft);
		OP(SpvOpDecorate, 10, SpvDecorationSpecId, 0);
		OP(SpvOpTypeVoid, 1);
		OP(SpvOpTypeInt, 2, 64, 0);
		OP(SpvOpSpecConstant, 2, 10, 0, 0);
		OP(SpvOpTypeFunction, 5, 1);
		OP(SpvOpFunction, 1, 20, SpvFunctionControlMaskNone, 5);
		OP(SpvOpLabel, 21);
		OP0(SpvOpReturn);
		OP0(SpvOpFunctionEnd);
		spvm_spec_entry e64[1] = { {0, 1} };
		spvm_program_t p = spvm_program_create_specialized(ctx, w, n, e64, 1, &err);
		TASK_CHECK(p == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_ERR_UNSUPPORTED_TYPE);
		TASK_CHECK(err.result_id == 10);
	}
	/* matrix-typed specialization value is rejected */
	{
		size_t n = 0;
		HDR(40);
		OP(SpvOpCapability, SpvCapabilityShader);
		OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
		OP(SpvOpEntryPoint, SpvExecutionModelFragment, 20, 0x6e69616d, 0);
		OP(SpvOpExecutionMode, 20, SpvExecutionModeOriginUpperLeft);
		OP(SpvOpTypeVoid, 1);
		OP(SpvOpTypeFloat, 4, 32);
		OP(SpvOpTypeVector, 6, 4, 2);
		OP(SpvOpTypeMatrix, 7, 6, 2);
		OP(SpvOpConstant, 4, 11, 0);
		OP(SpvOpConstant, 4, 12, 0);
		OP(SpvOpConstantComposite, 6, 13, 11, 12);
		OP(SpvOpSpecConstantComposite, 7, 14, 13, 13);
		OP(SpvOpTypeFunction, 5, 1);
		OP(SpvOpFunction, 1, 20, SpvFunctionControlMaskNone, 5);
		OP(SpvOpLabel, 21);
		OP0(SpvOpReturn);
		OP0(SpvOpFunctionEnd);
		spvm_program_t p = spvm_program_create_specialized(ctx, w, n, NULL, 0, &err);
		TASK_CHECK(p == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_ERR_UNSUPPORTED_TYPE);
		TASK_CHECK(err.result_id == 14);
	}
	/* dependency on a non-constant (a variable) is an unknown reference */
	{
		size_t n = 0;
		HDR(40);
		OP(SpvOpCapability, SpvCapabilityShader);
		OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
		OP(SpvOpEntryPoint, SpvExecutionModelFragment, 20, 0x6e69616d, 0);
		OP(SpvOpExecutionMode, 20, SpvExecutionModeOriginUpperLeft);
		OP(SpvOpTypeVoid, 1);
		OP(SpvOpTypeInt, 2, 32, 0);
		OP(SpvOpTypePointer, 6, SpvStorageClassPrivate, 2);
		OP(SpvOpVariable, 6, 14, SpvStorageClassPrivate);
		OP(SpvOpConstant, 2, 15, 1);
		OP(SpvOpSpecConstantOp, 2, 16, SpvOpIAdd, 15, 14);
		OP(SpvOpTypeFunction, 5, 1);
		OP(SpvOpFunction, 1, 20, SpvFunctionControlMaskNone, 5);
		OP(SpvOpLabel, 21);
		OP0(SpvOpReturn);
		OP0(SpvOpFunctionEnd);
		spvm_program_t p = spvm_program_create_specialized(ctx, w, n, NULL, 0, &err);
		TASK_CHECK(p == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_ERR_UNKNOWN_REF);
		TASK_CHECK(err.result_id == 14);
	}
	/* out-of-range composite extract */
	{
		size_t n = 0;
		HDR(40);
		OP(SpvOpCapability, SpvCapabilityShader);
		OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
		OP(SpvOpEntryPoint, SpvExecutionModelFragment, 20, 0x6e69616d, 0);
		OP(SpvOpExecutionMode, 20, SpvExecutionModeOriginUpperLeft);
		OP(SpvOpTypeVoid, 1);
		OP(SpvOpTypeInt, 2, 32, 0);
		OP(SpvOpTypeVector, 6, 2, 2);
		OP(SpvOpConstant, 2, 10, 1);
		OP(SpvOpConstant, 2, 11, 2);
		OP(SpvOpConstantComposite, 6, 12, 10, 11);
		OP(SpvOpSpecConstantOp, 2, 13, SpvOpCompositeExtract, 12, 5);
		OP(SpvOpTypeFunction, 5, 1);
		OP(SpvOpFunction, 1, 20, SpvFunctionControlMaskNone, 5);
		OP(SpvOpLabel, 21);
		OP0(SpvOpReturn);
		OP0(SpvOpFunctionEnd);
		spvm_program_t p = spvm_program_create_specialized(ctx, w, n, NULL, 0, &err);
		TASK_CHECK(p == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_ERR_INDEX_OUT_OF_RANGE);
		TASK_CHECK(err.result_id == 13);
	}
	/* undefined (0xFFFFFFFF) shuffle selector */
	{
		size_t n = 0;
		HDR(40);
		OP(SpvOpCapability, SpvCapabilityShader);
		OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
		OP(SpvOpEntryPoint, SpvExecutionModelFragment, 20, 0x6e69616d, 0);
		OP(SpvOpExecutionMode, 20, SpvExecutionModeOriginUpperLeft);
		OP(SpvOpTypeVoid, 1);
		OP(SpvOpTypeInt, 2, 32, 0);
		OP(SpvOpTypeVector, 6, 2, 2);
		OP(SpvOpConstant, 2, 10, 1);
		OP(SpvOpConstant, 2, 11, 2);
		OP(SpvOpConstantComposite, 6, 12, 10, 11);
		OP(SpvOpSpecConstantOp, 6, 13, SpvOpVectorShuffle, 12, 12, 0, 0xFFFFFFFFu);
		OP(SpvOpTypeFunction, 5, 1);
		OP(SpvOpFunction, 1, 20, SpvFunctionControlMaskNone, 5);
		OP(SpvOpLabel, 21);
		OP0(SpvOpReturn);
		OP0(SpvOpFunctionEnd);
		spvm_program_t p = spvm_program_create_specialized(ctx, w, n, NULL, 0, &err);
		TASK_CHECK(p == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_ERR_INDEX_OUT_OF_RANGE);
		TASK_CHECK(err.result_id == 13);
	}
	/* dependency cycle via composite + extract */
	{
		size_t n = 0;
		HDR(40);
		OP(SpvOpCapability, SpvCapabilityShader);
		OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
		OP(SpvOpEntryPoint, SpvExecutionModelFragment, 20, 0x6e69616d, 0);
		OP(SpvOpExecutionMode, 20, SpvExecutionModeOriginUpperLeft);
		OP(SpvOpTypeVoid, 1);
		OP(SpvOpTypeInt, 2, 32, 0);
		OP(SpvOpTypeVector, 6, 2, 2);
		OP(SpvOpSpecConstantComposite, 6, 12, 13, 13); /* depends on 13 */
		OP(SpvOpSpecConstantOp, 2, 13, SpvOpCompositeExtract, 12, 0); /* back on 12 */
		OP(SpvOpTypeFunction, 5, 1);
		OP(SpvOpFunction, 1, 20, SpvFunctionControlMaskNone, 5);
		OP(SpvOpLabel, 21);
		OP0(SpvOpReturn);
		OP0(SpvOpFunctionEnd);
		spvm_program_t p = spvm_program_create_specialized(ctx, w, n, NULL, 0, &err);
		TASK_CHECK(p == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_ERR_CYCLE);
		TASK_CHECK(err.result_id == 12 || err.result_id == 13);
	}
	/* non-positive array length */
	{
		size_t n = 0;
		HDR(40);
		OP(SpvOpCapability, SpvCapabilityShader);
		OP(SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
		OP(SpvOpEntryPoint, SpvExecutionModelFragment, 20, 0x6e69616d, 0);
		OP(SpvOpExecutionMode, 20, SpvExecutionModeOriginUpperLeft);
		OP(SpvOpDecorate, 10, SpvDecorationSpecId, 0);
		OP(SpvOpTypeVoid, 1);
		OP(SpvOpTypeInt, 3, 32, 1);
		OP(SpvOpSpecConstant, 3, 10, 2);
		OP(SpvOpTypeArray, 12, 3, 10);
		OP(SpvOpTypeFunction, 5, 1);
		OP(SpvOpFunction, 1, 20, SpvFunctionControlMaskNone, 5);
		OP(SpvOpLabel, 21);
		OP0(SpvOpReturn);
		OP0(SpvOpFunctionEnd);
		spvm_spec_entry e[1] = { {0, 0} };
		spvm_program_t p = spvm_program_create_specialized(ctx, w, n, e, 1, &err);
		TASK_CHECK(p == NULL);
		TASK_CHECK(err.code == SPVM_SPEC_ERR_BAD_ARRAY_LENGTH);
		TASK_CHECK(err.result_id == 12);
	}
	/* NULL error sink must not crash and still reports failure via NULL */
	{
		size_t count = build_min_spec(w, 1);
		spvm_spec_entry e[1] = { {99, 1} };
		spvm_program_t p = spvm_program_create_specialized(ctx, w, count, e, 1, NULL);
		TASK_CHECK(p == NULL);
	}
	spvm_context_deinitialize(ctx);
	return 0;
}

static int isolation_retry_after_failure(void)
{
	spvm_word w[1024];
	size_t count = build_min_spec(w, 1);
	spvm_context_t ctx = spvm_context_initialize();
	spvm_spec_error err;

	spvm_spec_entry bad[1] = { {99, 1} };
	TASK_CHECK(spvm_program_create_specialized(ctx, w, count, bad, 1, &err) == NULL);
	TASK_CHECK(err.code == SPVM_SPEC_ERR_UNKNOWN_SPEC_ID);

	/* the same context must still build a valid variant afterwards */
	spvm_spec_entry good[1] = { {0, 42} };
	spvm_program_t p = spvm_program_create_specialized(ctx, w, count, good, 1, &err);
	TASK_CHECK(p != NULL);
	TASK_CHECK(err.code == SPVM_SPEC_OK);
	spvm_state_t s = spvm_state_create(p);
	TASK_CHECK(s->results[10].members[0].value.u == 42);
	spvm_state_delete(s);
	spvm_program_delete(p);
	spvm_context_deinitialize(ctx);
	return 0;
}

static int isolation_input_buffer_release(void)
{
	spvm_word src[1024];
	size_t count = build_min_spec(src, 1);

	/* give the program only heap copies it must take ownership of */
	spvm_word* words = (spvm_word*)malloc(count * sizeof(spvm_word));
	spvm_spec_entry* entries = (spvm_spec_entry*)malloc(sizeof(spvm_spec_entry));
	memcpy(words, src, count * sizeof(spvm_word));
	entries[0].spec_id = 0;
	entries[0].bits = 123;

	spvm_context_t ctx = spvm_context_initialize();
	spvm_spec_error err;
	spvm_program_t p = spvm_program_create_specialized(ctx, words, count, entries, 1, &err);
	free(words);
	free(entries);
	TASK_CHECK(p != NULL);
	TASK_CHECK(err.code == SPVM_SPEC_OK);

	/* running after the caller's buffers are gone must still work */
	spvm_state_t s = spvm_state_create(p);
	TASK_CHECK(s->results[10].members[0].value.u == 123);
	spvm_state_delete(s);
	spvm_program_delete(p);
	spvm_context_deinitialize(ctx);
	return 0;
}

static int isolation_variants_independent(void)
{
	spvm_word w[1024];
	size_t count = build_layout_module(w);
	spvm_context_t ctx = spvm_context_initialize();
	spvm_spec_error err;

	/* two variants of the same module, interleaved creation */
	spvm_spec_entry va[2] = { {0, 3}, {1, 7} };   /* length 3, fill 7 */
	spvm_spec_entry vb[2] = { {0, 5}, {1, 9} };   /* length 5, fill 9 */
	spvm_program_t pa = spvm_program_create_specialized(ctx, w, count, va, 2, &err);
	TASK_CHECK(pa != NULL);
	spvm_program_t pb = spvm_program_create_specialized(ctx, w, count, vb, 2, &err);
	TASK_CHECK(pb != NULL);

	/* several states per variant */
	spvm_state_t sa1 = spvm_state_create(pa);
	spvm_state_t sb1 = spvm_state_create(pb);
	spvm_state_t sb2 = spvm_state_create(pb);
	TASK_CHECK(sa1->results[40].member_count == 3);
	TASK_CHECK(sb1->results[40].member_count == 5);
	TASK_CHECK(sb2->results[40].member_count == 5);

	/* mutate a private aggregate in one state of variant A only */
	sa1->results[51].members[0].members[0].value.u = 777;
	TASK_CHECK(sb1->results[51].members[0].members[0].value.u == 9);
	TASK_CHECK(sb2->results[51].members[0].members[0].value.u == 9);

	/* tear down variant A (states first, then program); B must survive */
	spvm_state_delete(sa1);
	spvm_program_delete(pa);
	TASK_CHECK(sb1->results[40].member_count == 5);
	TASK_CHECK(sb1->results[51].members[0].member_count == 5);
	TASK_CHECK(sb1->results[51].members[0].members[1].value.u == 2);
	TASK_CHECK(sb2->results[51].members[1].value.u == 9);

	/* B still executes correctly on the shared context */
	spvm_state_prepare(sb1, 70);
	spvm_state_call_function(sb1);
	TASK_CHECK(sb1->results[81].members[0].value.u == 2);
	TASK_CHECK(sb1->results[83].members[0].value.u == 9);

	spvm_state_delete(sb1);
	spvm_state_delete(sb2);
	spvm_program_delete(pb);
	spvm_context_deinitialize(ctx);
	return 0;
}

/* ====================================================================== */
/* Registration                                                           */
/* ====================================================================== */

const struct task_case task_cases[] = {
	{ "values", "scalar_and_dag", values_scalar_and_dag },
	{ "values", "defaults_empty_table", values_defaults_empty_table },
	{ "values", "vector_ops", values_vector_ops },
	{ "values", "integer_wrap_and_divide", values_integer_wrap_and_divide },
	{ "values", "negzero_bit_preservation", values_negzero_bit_preservation },
	{ "values", "bool_override", values_bool_override },
	{ "values", "extra_ops_and_nested_struct", values_extra_ops_and_nested_struct },
	{ "values", "parity_with_ordinary_module", values_parity_with_ordinary_module },
	{ "layout", "default_shape", layout_default_shape },
	{ "layout", "grown_shape", layout_grown_shape },
	{ "layout", "nested_aggregate_shape", layout_nested_aggregate_state_shape },
	{ "isolation", "error_coverage", isolation_error_coverage },
	{ "isolation", "retry_after_failure", isolation_retry_after_failure },
	{ "isolation", "input_buffer_release", isolation_input_buffer_release },
	{ "isolation", "variants_independent", isolation_variants_independent },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
