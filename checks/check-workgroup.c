#include <spvm/context.h>
#include <spvm/program.h>
#include <spvm/state.h>
#include <spvm/workgroup.h>
#include <spvm/spirv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Workgroup executor checks.
 *
 * Every SPIR-V module is built here, in C, as a raw word fixture (the same
 * style as checks/baseline.c); the readable assembly of the main fixture
 * lives in checks/workgroup-reduction.spvasm. Expected values are computed
 * independently with plain host integer arrays, never derived from the
 * executor's output.
 */

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		g_checks++;                                                   \
		if (!(cond)) {                                               \
			g_failures++;                                       \
			printf("FAIL: ");                                  \
			printf(__VA_ARGS__);                               \
			printf("\n");                                      \
		}                                                              \
	} while (0)

/* ------------------------------------------------------------------ */
/* minimal SPIR-V word builder                                         */
/* ------------------------------------------------------------------ */

typedef struct {
	spvm_word* words;
	size_t count;
	size_t cap;
	spvm_word next_id;
} Builder;

static void b_init(Builder* b)
{
	b->cap = 512;
	b->count = 0;
	b->next_id = 1;
	b->words = (spvm_word*)malloc(b->cap * sizeof(spvm_word));
}

static void b_push(Builder* b, spvm_word w)
{
	if (b->count == b->cap) {
		b->cap *= 2;
		b->words = (spvm_word*)realloc(b->words, b->cap * sizeof(spvm_word));
	}
	b->words[b->count++] = w;
}

static spvm_word b_id(Builder* b)
{
	return b->next_id++;
}

static void b_inst(Builder* b, SpvOp op, size_t n, const spvm_word* args)
{
	b_push(b, (spvm_word)(((n + 1) << 16) | (spvm_word)op));
	for (size_t i = 0; i < n; i++)
		b_push(b, args[i]);
}

/* compound-literal instruction emitter, C99 */
#define I(b, op, ...)                                                          \
	b_inst((b), (op),                                                         \
		sizeof((spvm_word[]){ __VA_ARGS__ }) / sizeof(spvm_word),         \
		(spvm_word[]){ __VA_ARGS__ })

static void b_string(Builder* b, const char* s)
{
	size_t len = strlen(s);
	size_t word_count = len / 4 + 1;
	for (size_t w = 0; w < word_count; w++) {
		spvm_word v = 0;
		for (int k = 0; k < 4; k++) {
			size_t idx = w * 4 + k;
			unsigned char c = idx < len ? (unsigned char)s[idx] : 0;
			v |= ((spvm_word)c) << (8 * k);
		}
		b_push(b, v);
	}
}

static void b_header(Builder* b)
{
	b_push(b, SpvMagicNumber);
	b_push(b, 0x00010000); /* SPIR-V 1.0 */
	b_push(b, 0);          /* generator */
	b_push(b, 0);          /* bound, patched at finish */
	b_push(b, 0);          /* schema */
}

static spvm_word* b_finish(Builder* b, size_t* length)
{
	b->words[3] = b->next_id; /* bound = one past the highest id */
	*length = b->count;
	return b->words;
}

typedef struct {
	spvm_word scope_wg;
	spvm_word sem_ar_wg;
} BarrierConsts;

static BarrierConsts b_barrier_consts(Builder* b, spvm_word i32)
{
	BarrierConsts c;
	spvm_word a = b_id(b);
	I(b, SpvOpConstant, i32, a, SpvScopeWorkgroup);
	spvm_word m = b_id(b);
	I(b, SpvOpConstant, i32, m,
		SpvMemorySemanticsAcquireReleaseMask | SpvMemorySemanticsWorkgroupMemoryMask);
	c.scope_wg = a;
	c.sem_ar_wg = m;
	return c;
}

/* ------------------------------------------------------------------ */
/* main fixture: neighbor swap + two barrier reduction rounds          */
/* ------------------------------------------------------------------ */

typedef struct {
	spvm_word shared_var;
	spvm_word lid_var;
	spvm_word wgid_var;
	spvm_word lgid_var;
	spvm_word lidx_var;
} ReductionIds;

static spvm_word* build_reduction(spvm_word sx, spvm_word sy, spvm_word sz,
	ReductionIds* out, size_t* length)
{
	Builder b;
	b_init(&b);
	b_header(&b);

	/* reserve every global/entry id up front so the entry interface is known */
	spvm_word entry = b_id(&b);
	spvm_word lid = b_id(&b);
	spvm_word wgid = b_id(&b);
	spvm_word lgid = b_id(&b);
	spvm_word lidx = b_id(&b);
	spvm_word shared = b_id(&b);

	I(&b, SpvOpCapability, SpvCapabilityShader);
	I(&b, SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	{
		size_t hdr = b.count;
		b_push(&b, 0); /* entry-point header placeholder, patched below */
		b_push(&b, SpvExecutionModelGLCompute);
		b_push(&b, entry);
		b_string(&b, "main");
		b_push(&b, lid);
		b_push(&b, wgid);
		b_push(&b, lgid);
		b_push(&b, lidx);
		b.words[hdr] = (spvm_word)(((b.count - hdr) << 16) | SpvOpEntryPoint);
	}
	I(&b, SpvOpExecutionMode, entry, SpvExecutionModeLocalSize, sx, sy, sz);

	/* types */
	spvm_word voidt = b_id(&b); I(&b, SpvOpTypeVoid, voidt);
	spvm_word fn_void = b_id(&b); I(&b, SpvOpTypeFunction, fn_void, voidt);
	spvm_word i32 = b_id(&b); I(&b, SpvOpTypeInt, i32, 32, 1);
	spvm_word u32 = b_id(&b); I(&b, SpvOpTypeInt, u32, 32, 0);
	spvm_word boolt = b_id(&b); I(&b, SpvOpTypeBool, boolt);
	spvm_word v3u32 = b_id(&b); I(&b, SpvOpTypeVector, v3u32, u32, 3);
	spvm_word total = sx * sy * sz;
	spvm_word c_n = b_id(&b); I(&b, SpvOpConstant, u32, c_n, total);
	spvm_word arr_ty = b_id(&b); I(&b, SpvOpTypeArray, arr_ty, i32, c_n);

	spvm_word ptr_wg_arr = b_id(&b); I(&b, SpvOpTypePointer, ptr_wg_arr, SpvStorageClassWorkgroup, arr_ty);
	spvm_word ptr_wg_i = b_id(&b); I(&b, SpvOpTypePointer, ptr_wg_i, SpvStorageClassWorkgroup, i32);
	spvm_word ptr_fn_i = b_id(&b); I(&b, SpvOpTypePointer, ptr_fn_i, SpvStorageClassFunction, i32);
	spvm_word ptr_in_v3 = b_id(&b); I(&b, SpvOpTypePointer, ptr_in_v3, SpvStorageClassInput, v3u32);
	spvm_word ptr_in_u = b_id(&b); I(&b, SpvOpTypePointer, ptr_in_u, SpvStorageClassInput, u32);

	spvm_word c0 = b_id(&b); I(&b, SpvOpConstant, i32, c0, 0);
	spvm_word c1 = b_id(&b); I(&b, SpvOpConstant, i32, c1, 1);
	spvm_word c2 = b_id(&b); I(&b, SpvOpConstant, i32, c2, 2);
	spvm_word c10 = b_id(&b); I(&b, SpvOpConstant, i32, c10, 10);
	spvm_word half = b_id(&b); I(&b, SpvOpConstant, i32, half, total / 2);
	BarrierConsts bc = b_barrier_consts(&b, i32);

	/* builtin input variables (ids reserved above) */
	I(&b, SpvOpVariable, ptr_in_v3, lid, SpvStorageClassInput);
	I(&b, SpvOpVariable, ptr_in_v3, wgid, SpvStorageClassInput);
	I(&b, SpvOpVariable, ptr_in_v3, lgid, SpvStorageClassInput);
	I(&b, SpvOpVariable, ptr_in_u, lidx, SpvStorageClassInput);
	I(&b, SpvOpDecorate, lid, SpvDecorationBuiltIn, SpvBuiltInLocalInvocationId);
	I(&b, SpvOpDecorate, wgid, SpvDecorationBuiltIn, SpvBuiltInWorkgroupId);
	I(&b, SpvOpDecorate, lgid, SpvDecorationBuiltIn, SpvBuiltInGlobalInvocationId);
	I(&b, SpvOpDecorate, lidx, SpvDecorationBuiltIn, SpvBuiltInLocalInvocationIndex);

	/* shared workgroup array, zero initialized */
	I(&b, SpvOpVariable, ptr_wg_arr, shared, SpvStorageClassWorkgroup);

	/* helper: void storePtr(int* p, int v) -- plain pointer parameter */
	spvm_word fn_store_t = b_id(&b); I(&b, SpvOpTypeFunction, fn_store_t, voidt, ptr_wg_i, i32);
	spvm_word store_fn = b_id(&b);
	I(&b, SpvOpFunction, voidt, store_fn, 0, fn_store_t);
	spvm_word p_store = b_id(&b); I(&b, SpvOpFunctionParameter, ptr_wg_i, p_store);
	spvm_word v_store = b_id(&b); I(&b, SpvOpFunctionParameter, i32, v_store);
	spvm_word l_store = b_id(&b); I(&b, SpvOpLabel, l_store);
	I(&b, SpvOpStore, p_store, v_store);
	I(&b, SpvOpReturn);
	I(&b, SpvOpFunctionEnd);

	/* helper: int loadPtr(int* p) */
	spvm_word fn_load_t = b_id(&b); I(&b, SpvOpTypeFunction, fn_load_t, i32, ptr_wg_i);
	spvm_word load_fn = b_id(&b);
	I(&b, SpvOpFunction, i32, load_fn, 0, fn_load_t);
	spvm_word p_load = b_id(&b); I(&b, SpvOpFunctionParameter, ptr_wg_i, p_load);
	spvm_word l_load = b_id(&b); I(&b, SpvOpLabel, l_load);
	spvm_word r_load = b_id(&b); I(&b, SpvOpLoad, i32, r_load, p_load);
	I(&b, SpvOpReturnValue, r_load);
	I(&b, SpvOpFunctionEnd);

	/* main */
	I(&b, SpvOpFunction, voidt, entry, 0, fn_void);
	spvm_word entry_lbl = b_id(&b); I(&b, SpvOpLabel, entry_lbl);
	spvm_word k_var = b_id(&b); I(&b, SpvOpVariable, ptr_fn_i, k_var, SpvStorageClassFunction, c0);

	spvm_word lid_load = b_id(&b); I(&b, SpvOpLoad, v3u32, lid_load, lid);
	spvm_word tid_u = b_id(&b); I(&b, SpvOpCompositeExtract, u32, tid_u, lid_load, 0);
	spvm_word tid = b_id(&b); I(&b, SpvOpBitcast, i32, tid, tid_u);

	/* storePtr(&shared[tid], (tid+1)*10) */
	spvm_word base_ptr = b_id(&b); I(&b, SpvOpAccessChain, ptr_wg_i, base_ptr, shared, tid);
	spvm_word t_plus = b_id(&b); I(&b, SpvOpIAdd, i32, t_plus, tid, c1);
	spvm_word t_init = b_id(&b); I(&b, SpvOpIMul, i32, t_init, t_plus, c10);
	spvm_word call0 = b_id(&b); I(&b, SpvOpFunctionCall, voidt, call0, store_fn, base_ptr, t_init);

	/* neighbor swap: even invocations swap with tid+1 */
	spvm_word mod2 = b_id(&b); I(&b, SpvOpSMod, i32, mod2, tid, c2);
	spvm_word even = b_id(&b); I(&b, SpvOpIEqual, boolt, even, mod2, c0);
	spvm_word swap_do = b_id(&b);
	spvm_word swap_join = b_id(&b);
	I(&b, SpvOpBranchConditional, even, swap_do, swap_join);

	I(&b, SpvOpLabel, swap_do);
	spvm_word pa = b_id(&b); I(&b, SpvOpAccessChain, ptr_wg_i, pa, shared, tid);
	spvm_word va = b_id(&b); I(&b, SpvOpFunctionCall, i32, va, load_fn, pa);
	spvm_word idx1 = b_id(&b); I(&b, SpvOpIAdd, i32, idx1, tid, c1);
	spvm_word pb = b_id(&b); I(&b, SpvOpAccessChain, ptr_wg_i, pb, shared, idx1);
	spvm_word vb = b_id(&b); I(&b, SpvOpFunctionCall, i32, vb, load_fn, pb);
	spvm_word call_pa = b_id(&b); I(&b, SpvOpFunctionCall, voidt, call_pa, store_fn, pa, vb);
	spvm_word call_pb = b_id(&b); I(&b, SpvOpFunctionCall, voidt, call_pb, store_fn, pb, va);
	I(&b, SpvOpBranch, swap_join);

	I(&b, SpvOpLabel, swap_join);
	I(&b, SpvOpControlBarrier, bc.scope_wg, bc.scope_wg, bc.sem_ar_wg);

	/* two-iteration reduction loop, the same static barrier hit twice */
	spvm_word red_cond = b_id(&b);
	spvm_word red_body = b_id(&b);
	spvm_word red_do = b_id(&b);
	spvm_word red_skip = b_id(&b);
	spvm_word red_join = b_id(&b);
	spvm_word red_done = b_id(&b);
	I(&b, SpvOpBranch, red_cond);

	I(&b, SpvOpLabel, red_cond);
	spvm_word kv = b_id(&b); I(&b, SpvOpLoad, i32, kv, k_var);
	spvm_word k_lt = b_id(&b); I(&b, SpvOpSLessThan, boolt, k_lt, kv, c2);
	I(&b, SpvOpBranchConditional, k_lt, red_body, red_done);

	I(&b, SpvOpLabel, red_body);
	spvm_word thr = b_id(&b); I(&b, SpvOpShiftRightArithmetic, i32, thr, half, kv);
	spvm_word active = b_id(&b); I(&b, SpvOpSLessThan, boolt, active, tid, thr);
	I(&b, SpvOpBranchConditional, active, red_do, red_skip);

	I(&b, SpvOpLabel, red_do);
	spvm_word qa = b_id(&b); I(&b, SpvOpAccessChain, ptr_wg_i, qa, shared, tid);
	spvm_word la = b_id(&b); I(&b, SpvOpFunctionCall, i32, la, load_fn, qa);
	spvm_word qidx = b_id(&b); I(&b, SpvOpIAdd, i32, qidx, tid, thr);
	spvm_word qb = b_id(&b); I(&b, SpvOpAccessChain, ptr_wg_i, qb, shared, qidx);
	spvm_word lb = b_id(&b); I(&b, SpvOpFunctionCall, i32, lb, load_fn, qb);
	spvm_word sumv = b_id(&b); I(&b, SpvOpIAdd, i32, sumv, la, lb);
	spvm_word call_sum = b_id(&b); I(&b, SpvOpFunctionCall, voidt, call_sum, store_fn, qa, sumv);
	I(&b, SpvOpBranch, red_join);

	I(&b, SpvOpLabel, red_skip);
	I(&b, SpvOpBranch, red_join);

	I(&b, SpvOpLabel, red_join);
	I(&b, SpvOpControlBarrier, bc.scope_wg, bc.scope_wg, bc.sem_ar_wg);
	spvm_word k_next = b_id(&b); I(&b, SpvOpIAdd, i32, k_next, kv, c1);
	I(&b, SpvOpStore, k_var, k_next);
	I(&b, SpvOpBranch, red_cond);

	I(&b, SpvOpLabel, red_done);
	I(&b, SpvOpReturn);
	I(&b, SpvOpFunctionEnd);

	out->shared_var = shared;
	out->lid_var = lid;
	out->wgid_var = wgid;
	out->lgid_var = lgid;
	out->lidx_var = lidx;

	return b_finish(&b, length);
}

/* Independent host-side expectation for the reduction fixture. */
static void expected_reduction(int n, int* out)
{
	for (int i = 0; i < n; i++)
		out[i] = (i + 1) * 10;

	for (int i = 0; i + 1 < n; i += 2) {
		int t = out[i];
		out[i] = out[i + 1];
		out[i + 1] = t;
	}

	for (int k = 0; k < 2; k++) {
		int stride = (n / 2) >> k;
		for (int tid = 0; tid < stride; tid++)
			out[tid] += out[tid + stride];
	}
}

/* ------------------------------------------------------------------ */
/* struct + CopyMemory + nested member alias fixture                   */
/* ------------------------------------------------------------------ */

typedef struct {
	spvm_word s1;
	spvm_word s2;
} StructIds;

static spvm_word* build_struct_copy(spvm_word sx, spvm_word sy, spvm_word sz,
	StructIds* out, size_t* length)
{
	Builder b;
	b_init(&b);
	b_header(&b);

	spvm_word entry = b_id(&b);
	spvm_word s1 = b_id(&b);
	spvm_word s2 = b_id(&b);

	I(&b, SpvOpCapability, SpvCapabilityShader);
	I(&b, SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	{
		size_t hdr = b.count;
		b_push(&b, 0);
		b_push(&b, SpvExecutionModelGLCompute);
		b_push(&b, entry);
		b_string(&b, "main");
		b.words[hdr] = (spvm_word)(((b.count - hdr) << 16) | SpvOpEntryPoint);
	}
	I(&b, SpvOpExecutionMode, entry, SpvExecutionModeLocalSize, sx, sy, sz);

	spvm_word voidt = b_id(&b); I(&b, SpvOpTypeVoid, voidt);
	spvm_word fn_void = b_id(&b); I(&b, SpvOpTypeFunction, fn_void, voidt);
	spvm_word i32 = b_id(&b); I(&b, SpvOpTypeInt, i32, 32, 1);
	spvm_word u32 = b_id(&b); I(&b, SpvOpTypeInt, u32, 32, 0);
	spvm_word boolt = b_id(&b); I(&b, SpvOpTypeBool, boolt);
	spvm_word v3u32 = b_id(&b); I(&b, SpvOpTypeVector, v3u32, u32, 3);
	spvm_word c2 = b_id(&b); I(&b, SpvOpConstant, i32, c2, 2);
	spvm_word arr_ty = b_id(&b); I(&b, SpvOpTypeArray, arr_ty, i32, c2);
	spvm_word struct_ty = b_id(&b); I(&b, SpvOpTypeStruct, struct_ty, i32, arr_ty);

	spvm_word ptr_wg_s = b_id(&b); I(&b, SpvOpTypePointer, ptr_wg_s, SpvStorageClassWorkgroup, struct_ty);
	spvm_word ptr_wg_i = b_id(&b); I(&b, SpvOpTypePointer, ptr_wg_i, SpvStorageClassWorkgroup, i32);
	spvm_word ptr_in_v3 = b_id(&b); I(&b, SpvOpTypePointer, ptr_in_v3, SpvStorageClassInput, v3u32);
	spvm_word c0 = b_id(&b); I(&b, SpvOpConstant, i32, c0, 0);
	spvm_word c1 = b_id(&b); I(&b, SpvOpConstant, i32, c1, 1);
	spvm_word c111 = b_id(&b); I(&b, SpvOpConstant, i32, c111, 111);
	spvm_word c222 = b_id(&b); I(&b, SpvOpConstant, i32, c222, 222);
	BarrierConsts bc = b_barrier_consts(&b, i32);

	spvm_word lid = b_id(&b);
	I(&b, SpvOpVariable, ptr_in_v3, lid, SpvStorageClassInput);
	I(&b, SpvOpDecorate, lid, SpvDecorationBuiltIn, SpvBuiltInLocalInvocationId);
	I(&b, SpvOpVariable, ptr_wg_s, s1, SpvStorageClassWorkgroup);
	I(&b, SpvOpVariable, ptr_wg_s, s2, SpvStorageClassWorkgroup);

	I(&b, SpvOpFunction, voidt, entry, 0, fn_void);
	spvm_word lbl = b_id(&b); I(&b, SpvOpLabel, lbl);

	spvm_word lid_load = b_id(&b); I(&b, SpvOpLoad, v3u32, lid_load, lid);
	spvm_word tid_u = b_id(&b); I(&b, SpvOpCompositeExtract, u32, tid_u, lid_load, 0);
	spvm_word tid = b_id(&b); I(&b, SpvOpBitcast, i32, tid, tid_u);
	spvm_word is_zero = b_id(&b); I(&b, SpvOpIEqual, boolt, is_zero, tid, c0);
	spvm_word write_lbl = b_id(&b);
	spvm_word join = b_id(&b);
	I(&b, SpvOpBranchConditional, is_zero, write_lbl, join);

	I(&b, SpvOpLabel, write_lbl);
	/* s1.a = 111 (struct member alias through AccessChain) */
	spvm_word pa = b_id(&b); I(&b, SpvOpAccessChain, ptr_wg_i, pa, s1, c0);
	I(&b, SpvOpStore, pa, c111);
	/* s1.b[0] = 222 (nested array-in-struct alias) */
	spvm_word pb = b_id(&b); I(&b, SpvOpAccessChain, ptr_wg_i, pb, s1, c1, c0);
	I(&b, SpvOpStore, pb, c222);
	I(&b, SpvOpBranch, join);

	I(&b, SpvOpLabel, join);
	I(&b, SpvOpControlBarrier, bc.scope_wg, bc.scope_wg, bc.sem_ar_wg);
	/* whole-struct CopyMemory, executed by every invocation */
	I(&b, SpvOpCopyMemory, s2, s1);
	I(&b, SpvOpReturn);
	I(&b, SpvOpFunctionEnd);

	out->s1 = s1;
	out->s2 = s2;
	return b_finish(&b, length);
}

/* ------------------------------------------------------------------ */
/* early return before the barrier fixture                             */
/* ------------------------------------------------------------------ */

static spvm_word* build_early_return(spvm_word sx, spvm_word sy, spvm_word sz, size_t* length)
{
	Builder b;
	b_init(&b);
	b_header(&b);

	spvm_word entry = b_id(&b);
	I(&b, SpvOpCapability, SpvCapabilityShader);
	I(&b, SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	{
		size_t hdr = b.count;
		b_push(&b, 0);
		b_push(&b, SpvExecutionModelGLCompute);
		b_push(&b, entry);
		b_string(&b, "main");
		b.words[hdr] = (spvm_word)(((b.count - hdr) << 16) | SpvOpEntryPoint);
	}
	I(&b, SpvOpExecutionMode, entry, SpvExecutionModeLocalSize, sx, sy, sz);

	spvm_word voidt = b_id(&b); I(&b, SpvOpTypeVoid, voidt);
	spvm_word fn_void = b_id(&b); I(&b, SpvOpTypeFunction, fn_void, voidt);
	spvm_word i32 = b_id(&b); I(&b, SpvOpTypeInt, i32, 32, 1);
	spvm_word u32 = b_id(&b); I(&b, SpvOpTypeInt, u32, 32, 0);
	spvm_word boolt = b_id(&b); I(&b, SpvOpTypeBool, boolt);
	spvm_word v3u32 = b_id(&b); I(&b, SpvOpTypeVector, v3u32, u32, 3);
	spvm_word ptr_in_v3 = b_id(&b); I(&b, SpvOpTypePointer, ptr_in_v3, SpvStorageClassInput, v3u32);
	spvm_word c0 = b_id(&b); I(&b, SpvOpConstant, i32, c0, 0);
	BarrierConsts bc = b_barrier_consts(&b, i32);

	spvm_word lid = b_id(&b);
	I(&b, SpvOpVariable, ptr_in_v3, lid, SpvStorageClassInput);
	I(&b, SpvOpDecorate, lid, SpvDecorationBuiltIn, SpvBuiltInLocalInvocationId);

	I(&b, SpvOpFunction, voidt, entry, 0, fn_void);
	spvm_word lbl = b_id(&b); I(&b, SpvOpLabel, lbl);
	spvm_word lid_load = b_id(&b); I(&b, SpvOpLoad, v3u32, lid_load, lid);
	spvm_word tid_u = b_id(&b); I(&b, SpvOpCompositeExtract, u32, tid_u, lid_load, 0);
	spvm_word tid = b_id(&b); I(&b, SpvOpBitcast, i32, tid, tid_u);
	spvm_word is_zero = b_id(&b); I(&b, SpvOpIEqual, boolt, is_zero, tid, c0);
	spvm_word ret_lbl = b_id(&b);
	spvm_word bar_lbl = b_id(&b);
	I(&b, SpvOpBranchConditional, is_zero, ret_lbl, bar_lbl);
	I(&b, SpvOpLabel, ret_lbl);
	I(&b, SpvOpReturn);
	I(&b, SpvOpLabel, bar_lbl);
	I(&b, SpvOpControlBarrier, bc.scope_wg, bc.scope_wg, bc.sem_ar_wg);
	I(&b, SpvOpReturn);
	I(&b, SpvOpFunctionEnd);

	return b_finish(&b, length);
}

/* ------------------------------------------------------------------ */
/* same helper barrier reached from two different call sites           */
/* ------------------------------------------------------------------ */

static spvm_word* build_call_site_mismatch(spvm_word sx, spvm_word sy, spvm_word sz, size_t* length)
{
	Builder b;
	b_init(&b);
	b_header(&b);

	spvm_word entry = b_id(&b);
	I(&b, SpvOpCapability, SpvCapabilityShader);
	I(&b, SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	{
		size_t hdr = b.count;
		b_push(&b, 0);
		b_push(&b, SpvExecutionModelGLCompute);
		b_push(&b, entry);
		b_string(&b, "main");
		b.words[hdr] = (spvm_word)(((b.count - hdr) << 16) | SpvOpEntryPoint);
	}
	I(&b, SpvOpExecutionMode, entry, SpvExecutionModeLocalSize, sx, sy, sz);

	spvm_word voidt = b_id(&b); I(&b, SpvOpTypeVoid, voidt);
	spvm_word fn_void = b_id(&b); I(&b, SpvOpTypeFunction, fn_void, voidt);
	spvm_word i32 = b_id(&b); I(&b, SpvOpTypeInt, i32, 32, 1);
	spvm_word u32 = b_id(&b); I(&b, SpvOpTypeInt, u32, 32, 0);
	spvm_word boolt = b_id(&b); I(&b, SpvOpTypeBool, boolt);
	spvm_word v3u32 = b_id(&b); I(&b, SpvOpTypeVector, v3u32, u32, 3);
	spvm_word ptr_in_v3 = b_id(&b); I(&b, SpvOpTypePointer, ptr_in_v3, SpvStorageClassInput, v3u32);
	spvm_word c0 = b_id(&b); I(&b, SpvOpConstant, i32, c0, 0);
	BarrierConsts bc = b_barrier_consts(&b, i32);

	/* void helper() { barrier; } */
	spvm_word helper = b_id(&b);
	I(&b, SpvOpFunction, voidt, helper, 0, fn_void);
	spvm_word h_lbl = b_id(&b); I(&b, SpvOpLabel, h_lbl);
	I(&b, SpvOpControlBarrier, bc.scope_wg, bc.scope_wg, bc.sem_ar_wg);
	I(&b, SpvOpReturn);
	I(&b, SpvOpFunctionEnd);

	spvm_word lid = b_id(&b);
	I(&b, SpvOpVariable, ptr_in_v3, lid, SpvStorageClassInput);
	I(&b, SpvOpDecorate, lid, SpvDecorationBuiltIn, SpvBuiltInLocalInvocationId);

	I(&b, SpvOpFunction, voidt, entry, 0, fn_void);
	spvm_word lbl = b_id(&b); I(&b, SpvOpLabel, lbl);
	spvm_word lid_load = b_id(&b); I(&b, SpvOpLoad, v3u32, lid_load, lid);
	spvm_word tid_u = b_id(&b); I(&b, SpvOpCompositeExtract, u32, tid_u, lid_load, 0);
	spvm_word tid = b_id(&b); I(&b, SpvOpBitcast, i32, tid, tid_u);
	spvm_word is_zero = b_id(&b); I(&b, SpvOpIEqual, boolt, is_zero, tid, c0);
	spvm_word site_a = b_id(&b);
	spvm_word site_b = b_id(&b);
	spvm_word join = b_id(&b);
	I(&b, SpvOpBranchConditional, is_zero, site_a, site_b);

	I(&b, SpvOpLabel, site_a);
	I(&b, SpvOpFunctionCall, voidt, b_id(&b), helper); /* call site A */
	I(&b, SpvOpBranch, join);

	I(&b, SpvOpLabel, site_b);
	I(&b, SpvOpFunctionCall, voidt, b_id(&b), helper); /* call site B */
	I(&b, SpvOpBranch, join);

	I(&b, SpvOpLabel, join);
	I(&b, SpvOpReturn);
	I(&b, SpvOpFunctionEnd);

	return b_finish(&b, length);
}

/* ------------------------------------------------------------------ */
/* trivial module used for 3D builtin numbering and lifecycle checks   */
/* ------------------------------------------------------------------ */

typedef struct {
	spvm_word lid;
	spvm_word wgid;
	spvm_word lgid;
	spvm_word lidx;
} BuiltinIds;

static spvm_word* build_trivial(spvm_word sx, spvm_word sy, spvm_word sz,
	BuiltinIds* out, size_t* length)
{
	Builder b;
	b_init(&b);
	b_header(&b);

	spvm_word entry = b_id(&b);
	spvm_word lid = b_id(&b);
	spvm_word wgid = b_id(&b);
	spvm_word lgid = b_id(&b);
	spvm_word lidx = b_id(&b);

	I(&b, SpvOpCapability, SpvCapabilityShader);
	I(&b, SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	{
		size_t hdr = b.count;
		b_push(&b, 0);
		b_push(&b, SpvExecutionModelGLCompute);
		b_push(&b, entry);
		b_string(&b, "main");
		b_push(&b, lid);
		b_push(&b, wgid);
		b_push(&b, lgid);
		b_push(&b, lidx);
		b.words[hdr] = (spvm_word)(((b.count - hdr) << 16) | SpvOpEntryPoint);
	}
	I(&b, SpvOpExecutionMode, entry, SpvExecutionModeLocalSize, sx, sy, sz);

	spvm_word voidt = b_id(&b); I(&b, SpvOpTypeVoid, voidt);
	spvm_word fn_void = b_id(&b); I(&b, SpvOpTypeFunction, fn_void, voidt);
	spvm_word u32 = b_id(&b); I(&b, SpvOpTypeInt, u32, 32, 0);
	spvm_word v3u32 = b_id(&b); I(&b, SpvOpTypeVector, v3u32, u32, 3);
	spvm_word ptr_in_v3 = b_id(&b); I(&b, SpvOpTypePointer, ptr_in_v3, SpvStorageClassInput, v3u32);
	spvm_word ptr_in_u = b_id(&b); I(&b, SpvOpTypePointer, ptr_in_u, SpvStorageClassInput, u32);

	I(&b, SpvOpVariable, ptr_in_v3, lid, SpvStorageClassInput);
	I(&b, SpvOpVariable, ptr_in_v3, wgid, SpvStorageClassInput);
	I(&b, SpvOpVariable, ptr_in_v3, lgid, SpvStorageClassInput);
	I(&b, SpvOpVariable, ptr_in_u, lidx, SpvStorageClassInput);
	I(&b, SpvOpDecorate, lid, SpvDecorationBuiltIn, SpvBuiltInLocalInvocationId);
	I(&b, SpvOpDecorate, wgid, SpvDecorationBuiltIn, SpvBuiltInWorkgroupId);
	I(&b, SpvOpDecorate, lgid, SpvDecorationBuiltIn, SpvBuiltInGlobalInvocationId);
	I(&b, SpvOpDecorate, lidx, SpvDecorationBuiltIn, SpvBuiltInLocalInvocationIndex);

	I(&b, SpvOpFunction, voidt, entry, 0, fn_void);
	spvm_word lbl = b_id(&b); I(&b, SpvOpLabel, lbl);
	I(&b, SpvOpReturn);
	I(&b, SpvOpFunctionEnd);

	out->lid = lid;
	out->wgid = wgid;
	out->lgid = lgid;
	out->lidx = lidx;
	return b_finish(&b, length);
}

/* module containing an atomic: workgroup creation must reject it */
static spvm_word* build_atomic_module(size_t* length)
{
	Builder b;
	b_init(&b);
	b_header(&b);

	spvm_word entry = b_id(&b);
	I(&b, SpvOpCapability, SpvCapabilityShader);
	I(&b, SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	{
		size_t hdr = b.count;
		b_push(&b, 0);
		b_push(&b, SpvExecutionModelGLCompute);
		b_push(&b, entry);
		b_string(&b, "main");
		b.words[hdr] = (spvm_word)(((b.count - hdr) << 16) | SpvOpEntryPoint);
	}
	I(&b, SpvOpExecutionMode, entry, SpvExecutionModeLocalSize, 1, 1, 1);

	spvm_word voidt = b_id(&b); I(&b, SpvOpTypeVoid, voidt);
	spvm_word fn_void = b_id(&b); I(&b, SpvOpTypeFunction, fn_void, voidt);
	spvm_word u32 = b_id(&b); I(&b, SpvOpTypeInt, u32, 32, 0);
	spvm_word c0 = b_id(&b); I(&b, SpvOpConstant, u32, c0, 0);
	spvm_word c1 = b_id(&b); I(&b, SpvOpConstant, u32, c1, 1);

	I(&b, SpvOpFunction, voidt, entry, 0, fn_void);
	spvm_word lbl = b_id(&b); I(&b, SpvOpLabel, lbl);
	/* static presence alone must trigger rejection */
	I(&b, SpvOpAtomicIAdd, u32, b_id(&b), c0, c1, c0, c0, c1);
	I(&b, SpvOpReturn);
	I(&b, SpvOpFunctionEnd);

	return b_finish(&b, length);
}

/* ------------------------------------------------------------------ */
/* runners                                                             */
/* ------------------------------------------------------------------ */

static spvm_word g_schedule[64];

static void make_order(spvm_word n, int mode)
{
	if (mode == 0) { /* forward */
		for (spvm_word i = 0; i < n; i++) g_schedule[i] = i;
	} else if (mode == 1) { /* reverse */
		for (spvm_word i = 0; i < n; i++) g_schedule[i] = n - 1 - i;
	} else { /* outside-in interleaved */
		spvm_word lo = 0, hi = n;
		for (spvm_word i = 0; i < n; i++)
			g_schedule[i] = (i % 2 == 0) ? lo++ : --hi;
	}
}

/*
 * Liveness cannot use instruction_count: declaration opcodes such as
 * OpVariable have no execute handler, so spvm_state_step_opcode advances
 * code_current without bumping instruction_count. A step returns RUNNING iff
 * code_current actually moved; the only productive batch with no RUNNING step
 * is the one whose final arrival releases a barrier, which is caught by the
 * change in the completed round.
 */
static int run_with_order(spvm_workgroup_t wg, spvm_word n, int mode, spvm_wg_status* last)
{
	make_order(n, mode);
	int batches = 0;
	for (;;) {
		spvm_word prev_round = spvm_workgroup_current_round(wg);
		int moved = 0;
		int all_done = 1;

		for (spvm_word k = 0; k < n; k++) {
			spvm_word i = g_schedule[k];
			spvm_wg_status s = spvm_workgroup_step(wg, i);
			if (s == SPVM_WG_STATUS_ERROR_BARRIER_SCOPE ||
				s == SPVM_WG_STATUS_ERROR_BARRIER_MISMATCH ||
				s == SPVM_WG_STATUS_ERROR_PEER_FINISHED ||
				s == SPVM_WG_STATUS_ERROR_UNSUPPORTED ||
				s == SPVM_WG_STATUS_ERROR_INVALID_ARGUMENT) {
				*last = s;
				return 0;
			}
			if (s == SPVM_WG_STATUS_RUNNING)
				moved = 1;
			if (s != SPVM_WG_STATUS_FINISHED)
				all_done = 0;
		}

		if (all_done)
			break;

		spvm_word cur_round = spvm_workgroup_current_round(wg);
		if (!moved && cur_round == prev_round) {
			/* no invocation advanced and no barrier round completed: deadlock */
			*last = SPVM_WG_STATUS_ERROR_BARRIER_MISMATCH;
			return 0;
		}
		if (++batches > 1000000) {
			*last = SPVM_WG_STATUS_ERROR_UNSUPPORTED;
			return 0;
		}
	}
	*last = SPVM_WG_STATUS_FINISHED;
	return 1;
}

static void test_reduction_orderings(spvm_context_t ctx)
{
	const spvm_word n = 4;
	int expected[4];
	expected_reduction((int)n, expected); /* [100, 40, 40, 30] */

	for (int mode = 0; mode < 3; mode++) {
		size_t length = 0;
		ReductionIds ids;
		spvm_word* words = build_reduction(n, 1, 1, &ids, &length);
		spvm_program_t prog = spvm_program_create(ctx, words, length);
		CHECK(prog != NULL, "reduction program creation (mode %d)", mode);

		spvm_workgroup_t wg = spvm_workgroup_create(prog, 0, 0, 0);
		CHECK(wg != NULL, "reduction workgroup creation (mode %d)", mode);
		CHECK(spvm_workgroup_invocation_count(wg) == n, "invocation count");

		spvm_wg_status status;
		int ok = run_with_order(wg, n, mode, &status);
		CHECK(ok && status == SPVM_WG_STATUS_FINISHED,
			"reduction run mode %d failed with status %d", mode, (int)status);

		for (spvm_word inv = 0; inv < n; inv++) {
			spvm_state_t st = spvm_workgroup_get_state(wg, inv);
			spvm_result_t sr = &st->results[ids.shared_var];
			for (spvm_word e = 0; e < n; e++) {
				int got = sr->members[e].value.s;
				CHECK(got == expected[e],
					"mode %d invocation %u shared[%u] = %d, expected %d",
					mode, inv, e, got, expected[e]);
			}
		}

		spvm_workgroup_delete(wg);
		spvm_program_delete(prog);
		free(words);
	}
}

static void test_waiting_instruction_count(spvm_context_t ctx)
{
	size_t length = 0;
	ReductionIds ids;
	spvm_word* words = build_reduction(4, 1, 1, &ids, &length);
	spvm_program_t prog = spvm_program_create(ctx, words, length);
	spvm_workgroup_t wg = spvm_workgroup_create(prog, 0, 0, 0);

	spvm_state_t st0 = spvm_workgroup_get_state(wg, 0);
	spvm_wg_status s = SPVM_WG_STATUS_RUNNING;
	int guard = 0;
	while (s == SPVM_WG_STATUS_RUNNING && guard++ < 10000)
		s = spvm_workgroup_step(wg, 0);

	CHECK(s == SPVM_WG_STATUS_WAITING, "invocation 0 should block at first barrier, got %d", (int)s);
	spvm_word count_at_wait = st0->instruction_count;

	spvm_wg_status again = spvm_workgroup_step(wg, 0);
	CHECK(again == SPVM_WG_STATUS_WAITING, "stepping a waiting invocation must keep waiting");
	CHECK(st0->instruction_count == count_at_wait,
		"instruction_count grew while waiting: %u -> %u",
		count_at_wait, st0->instruction_count);

	/* release the round by advancing the peers, then 0 resumes */
	for (spvm_word i = 1; i < 4; i++) {
		spvm_wg_status p = SPVM_WG_STATUS_RUNNING;
		int g = 0;
		while (p == SPVM_WG_STATUS_RUNNING && g++ < 10000)
			p = spvm_workgroup_step(wg, i);
		CHECK(p == SPVM_WG_STATUS_WAITING, "peer %u should reach the barrier, got %d", i, (int)p);
	}
	spvm_wg_status resumed = spvm_workgroup_step(wg, 0);
	CHECK(resumed == SPVM_WG_STATUS_RUNNING, "released invocation should resume, got %d", (int)resumed);
	CHECK(st0->instruction_count > count_at_wait, "instruction_count should grow after release");

	spvm_wg_status fin = spvm_workgroup_run(wg);
	CHECK(fin == SPVM_WG_STATUS_FINISHED, "draining after manual steps failed: %d", (int)fin);

	spvm_workgroup_delete(wg);
	spvm_program_delete(prog);
	free(words);
}

static spvm_member_t builtin_members(spvm_state_t st, SpvBuiltIn which, spvm_word* count)
{
	return spvm_state_get_builtin(st, which, count);
}

static void test_3d_builtins(spvm_context_t ctx)
{
	const spvm_word sx = 2, sy = 2, sz = 2;
	size_t length = 0;
	BuiltinIds ids;
	spvm_word* words = build_trivial(sx, sy, sz, &ids, &length);
	spvm_program_t prog = spvm_program_create(ctx, words, length);
	spvm_workgroup_t wg = spvm_workgroup_create(prog, 5, 6, 7);
	CHECK(wg != NULL, "3D workgroup creation");
	CHECK(spvm_workgroup_invocation_count(wg) == sx * sy * sz, "3D invocation count");

	for (spvm_word z = 0; z < sz; z++) {
		for (spvm_word y = 0; y < sy; y++) {
			for (spvm_word x = 0; x < sx; x++) {
				spvm_word index = (z * sy + y) * sx + x;
				spvm_state_t st = spvm_workgroup_get_state(wg, index);

				spvm_word cnt = 0;
				spvm_member_t lid = builtin_members(st, SpvBuiltInLocalInvocationId, &cnt);
				CHECK(cnt == 3 && lid[0].value.s == (int)x && lid[1].value.s == (int)y &&
					lid[2].value.s == (int)z,
					"inv %u LocalInvocationId = (%d,%d,%d), want (%u,%u,%u)",
					index, lid ? lid[0].value.s : -999, lid ? lid[1].value.s : -999,
					lid ? lid[2].value.s : -999, x, y, z);

				spvm_member_t lidx_m = builtin_members(st, SpvBuiltInLocalInvocationIndex, &cnt);
				CHECK(cnt == 1 && (spvm_word)lidx_m[0].value.u == index,
					"inv %u LocalInvocationIndex = %u", index, lidx_m ? lidx_m[0].value.u : 999);

				spvm_member_t wgid_m = builtin_members(st, SpvBuiltInWorkgroupId, &cnt);
				CHECK(cnt == 3 && wgid_m[0].value.s == 5 && wgid_m[1].value.s == 6 &&
					wgid_m[2].value.s == 7,
					"inv %u WorkgroupId", index);

				spvm_member_t gid = builtin_members(st, SpvBuiltInGlobalInvocationId, &cnt);
				CHECK(cnt == 3 && gid[0].value.s == (int)(5 * sx + x) &&
					gid[1].value.s == (int)(6 * sy + y) &&
					gid[2].value.s == (int)(7 * sz + z),
					"inv %u GlobalInvocationId", index);
			}
		}
	}

	CHECK(spvm_workgroup_run(wg) == SPVM_WG_STATUS_FINISHED, "trivial 3D run");
	spvm_workgroup_delete(wg);
	spvm_program_delete(prog);
	free(words);
}

static void test_two_groups_interleave(spvm_context_t ctx)
{
	const spvm_word n = 4;
	int expected[4];
	expected_reduction((int)n, expected);

	size_t length = 0;
	ReductionIds ids;
	spvm_word* words = build_reduction(n, 1, 1, &ids, &length);
	spvm_program_t prog = spvm_program_create(ctx, words, length);

	spvm_workgroup_t a = spvm_workgroup_create(prog, 0, 0, 0);
	spvm_workgroup_t b = spvm_workgroup_create(prog, 7, 8, 9);
	CHECK(a && b, "two interleaved workgroups created from one program");

	/* verify builtins already differ before any instruction runs */
	spvm_word cnt = 0;
	spvm_member_t ga = builtin_members(spvm_workgroup_get_state(a, 0), SpvBuiltInGlobalInvocationId, &cnt);
	spvm_member_t gb = builtin_members(spvm_workgroup_get_state(b, 0), SpvBuiltInGlobalInvocationId, &cnt);
	CHECK(ga[0].value.s == 0 && gb[0].value.s == 7 * 4 && gb[1].value.s == 8,
		"interleaved groups report distinct GlobalInvocationId");

	/* fine-grained interleave: A0 B0 A1 B1 ... passes until both finish */
	int guard = 0;
	for (;;) {
		int adone = 1, bdone = 1;
		for (spvm_word i = 0; i < n; i++) {
			spvm_wg_status sa = spvm_workgroup_step(a, i);
			spvm_wg_status sb = spvm_workgroup_step(b, i);
			if (sa != SPVM_WG_STATUS_FINISHED) adone = 0;
			if (sb != SPVM_WG_STATUS_FINISHED) bdone = 0;
			CHECK(sa != SPVM_WG_STATUS_ERROR_BARRIER_MISMATCH &&
				sb != SPVM_WG_STATUS_ERROR_BARRIER_MISMATCH,
				"interleaved group barrier mismatch");
		}
		if (adone && bdone)
			break;
		if (guard++ > 100000) {
			CHECK(0, "interleaved groups did not finish");
			break;
		}
	}

	for (spvm_word g = 0; g < 2; g++) {
		spvm_workgroup_t wg = g == 0 ? a : b;
		for (spvm_word inv = 0; inv < n; inv++) {
			spvm_state_t st = spvm_workgroup_get_state(wg, inv);
			for (spvm_word e = 0; e < n; e++)
				CHECK(st->results[ids.shared_var].members[e].value.s == expected[e],
					"group %u inv %u shared[%u] independent storage", g, inv, e);
		}
	}

	/* deleting one group must not invalidate the other */
	spvm_workgroup_delete(a);
	spvm_state_t b0 = spvm_workgroup_get_state(b, 0);
	CHECK(b0->results[ids.shared_var].members[0].value.s == expected[0],
		"group B survives group A deletion");
	spvm_workgroup_delete(b);

	spvm_program_delete(prog);
	free(words);
}

static void test_struct_copy_memory(spvm_context_t ctx)
{
	size_t length = 0;
	StructIds ids;
	spvm_word* words = build_struct_copy(2, 1, 1, &ids, &length);
	spvm_program_t prog = spvm_program_create(ctx, words, length);
	spvm_workgroup_t wg = spvm_workgroup_create(prog, 0, 0, 0);
	CHECK(spvm_workgroup_run(wg) == SPVM_WG_STATUS_FINISHED, "struct copy run");

	/* every invocation must observe the copy through the shared struct */
	for (spvm_word inv = 0; inv < 2; inv++) {
		spvm_state_t st = spvm_workgroup_get_state(wg, inv);
		spvm_result_t s2 = &st->results[ids.s2];
		CHECK(s2->members[0].value.s == 111, "s2.a through CopyMemory, inv %u", inv);
		CHECK(s2->members[1].members[0].value.s == 222, "s2.b[0] nested alias, inv %u", inv);
		CHECK(s2->members[1].members[1].value.s == 0, "s2.b[1] stays zero-initialized, inv %u", inv);
	}

	spvm_workgroup_delete(wg);
	spvm_program_delete(prog);
	free(words);
}

static void drain_expect_error(spvm_context_t ctx, spvm_word* words, size_t length,
	spvm_wg_status want, const char* name)
{
	spvm_program_t prog = spvm_program_create(ctx, words, length);
	spvm_workgroup_t wg = spvm_workgroup_create(prog, 0, 0, 0);
	CHECK(wg != NULL, "%s workgroup creation", name);

	spvm_wg_status got = spvm_workgroup_run(wg);
	CHECK(got == want, "%s: expected status %d, got %d", name, (int)want, (int)got);
	const char* err = spvm_workgroup_error(wg);
	CHECK(err != NULL && strstr(err, "round") != NULL, "%s diagnostic must name the round", name);

	spvm_workgroup_delete(wg);
	spvm_program_delete(prog);
}

static void test_diagnostics(spvm_context_t ctx)
{
	size_t length = 0;

	spvm_word* early = build_early_return(2, 1, 1, &length);
	drain_expect_error(ctx, early, length, SPVM_WG_STATUS_ERROR_PEER_FINISHED, "early-return");
	free(early);

	spvm_word* mismatch = build_call_site_mismatch(2, 1, 1, &length);
	drain_expect_error(ctx, mismatch, length, SPVM_WG_STATUS_ERROR_BARRIER_MISMATCH, "call-site mismatch");
	free(mismatch);
}

static void test_unsupported_rejected(spvm_context_t ctx)
{
	size_t length = 0;
	spvm_word* words = build_atomic_module(&length);
	spvm_program_t prog = spvm_program_create(ctx, words, length);
	spvm_workgroup_t wg = spvm_workgroup_create(prog, 0, 0, 0);
	CHECK(wg == NULL, "module with an atomic opcode must be rejected");
	spvm_program_delete(prog);
	free(words);
}

static void test_lifecycle(spvm_context_t ctx)
{
	size_t length = 0;
	BuiltinIds ids;
	spvm_word* words = build_trivial(2, 1, 1, &ids, &length);
	spvm_program_t prog = spvm_program_create(ctx, words, length);

	for (int i = 0; i < 25; i++) {
		spvm_workgroup_t wg = spvm_workgroup_create(prog, 0, 0, 0);
		CHECK(wg != NULL, "repeated create iteration %d", i);
		spvm_workgroup_delete(wg);
	}

	spvm_workgroup_t wg = spvm_workgroup_create(prog, 0, 0, 0);
	CHECK(spvm_workgroup_run(wg) == SPVM_WG_STATUS_FINISHED, "workgroup works after repeated cycles");
	spvm_workgroup_delete(wg);

	spvm_program_delete(prog);
	free(words);
}

int main(void)
{
	spvm_context_t ctx = spvm_context_initialize();

	test_reduction_orderings(ctx);
	test_waiting_instruction_count(ctx);
	test_3d_builtins(ctx);
	test_two_groups_interleave(ctx);
	test_struct_copy_memory(ctx);
	test_diagnostics(ctx);
	test_unsupported_rejected(ctx);
	test_lifecycle(ctx);

	spvm_context_deinitialize(ctx);

	printf("%d checks, %d failures\n", g_checks, g_failures);
	if (g_failures == 0)
		puts("WORKGROUP_CHECKS_OK");
	return g_failures ? 1 : 0;
}
