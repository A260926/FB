/* Regression tests for the read-only x86-64 CFG recovery API.
 *
 * Every firmware fragment is a plain C byte array; no external assembler or
 * binary is involved. The cases exercise cross-predecessor merges, loop
 * driven widening to unknown, PIC negative-offset tables, 32 bit zero
 * extension, partial register invalidation, table accesses crossing region
 * boundaries, late overlapping decode entries and basic block splitting.
 */

#include "support/check.h"

#include <capstone/cfg.h>

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tiny byte-image builder                                             */
/* ------------------------------------------------------------------ */

typedef struct img {
	unsigned char *buf;
	size_t len;
	size_t cap;
	uint64_t base;
} img;

static void img_init(img *m, uint64_t base)
{
	m->cap = 256;
	m->len = 0;
	m->base = base;
	m->buf = malloc(m->cap);
}

static void img_free(img *m)
{
	free(m->buf);
	m->buf = NULL;
}

static void emit(img *m, const void *p, size_t n)
{
	if (m->len + n > m->cap) {
		while (m->len + n > m->cap)
			m->cap *= 2;
		m->buf = realloc(m->buf, m->cap);
	}
	memcpy(m->buf + m->len, p, n);
	m->len += n;
}

static uint64_t img_here(const img *m)
{
	return m->base + m->len;
}

static void emit_u32(img *m, uint32_t v)
{
	unsigned char b[4] = {
		(unsigned char)v, (unsigned char)(v >> 8),
		(unsigned char)(v >> 16), (unsigned char)(v >> 24)
	};
	emit(m, b, 4);
}

static void emit_u64(img *m, uint64_t v)
{
	unsigned char b[8];
	int i;
	for (i = 0; i < 8; ++i)
		b[i] = (unsigned char)(v >> (8 * i));
	emit(m, b, 8);
}

static void emit_jmp_short(img *m, uint64_t target)
{
	uint64_t at = img_here(m);
	unsigned char b[2] = {0xeb,
		(unsigned char)(target - (at + 2))};
	emit(m, b, 2);
}

static void emit_jcc_short(img *m, unsigned char op, uint64_t target)
{
	uint64_t at = img_here(m);
	unsigned char b[2] = {op,
		(unsigned char)(target - (at + 2))};
	emit(m, b, 2);
}

static void emit_jmp_near(img *m, uint64_t target)
{
	uint64_t at = img_here(m);
	int32_t rel = (int32_t)(target - (at + 5));
	unsigned char b[5] = {0xe9, (unsigned char)rel,
		(unsigned char)(rel >> 8), (unsigned char)(rel >> 16),
		(unsigned char)(rel >> 24)};
	emit(m, b, 5);
}

/* 48 8d <modrm> <rel32>: LEA r64, [rip + disp] */
static void emit_lea_rip(img *m, unsigned char modrm, uint64_t target)
{
	uint64_t at = img_here(m);
	int32_t rel = (int32_t)(target - (at + 7));
	unsigned char b[7] = {0x48, 0x8d, modrm, (unsigned char)rel,
		(unsigned char)(rel >> 8), (unsigned char)(rel >> 16),
		(unsigned char)(rel >> 24)};
	emit(m, b, 7);
}

/* ------------------------------------------------------------------ */
/* Report inspection helpers                                           */
/* ------------------------------------------------------------------ */

static const cs_cfg_block *find_block(const cs_cfg_report *r, uint64_t addr)
{
	size_t i;
	for (i = 0; i < r->block_count; ++i) {
		if (r->blocks[i].address == addr)
			return &r->blocks[i];
	}
	return NULL;
}

static const cs_cfg_edge *find_edge(const cs_cfg_report *r, uint64_t src,
		uint64_t dst, cs_cfg_edge_kind kind)
{
	size_t i;
	for (i = 0; i < r->edge_count; ++i) {
		if (r->edges[i].source == src && r->edges[i].target == dst &&
				r->edges[i].kind == kind)
			return &r->edges[i];
	}
	return NULL;
}

static const cs_cfg_diagnostic *find_diag(const cs_cfg_report *r,
		uint64_t addr, cs_cfg_diagnostic_kind kind)
{
	size_t i;
	for (i = 0; i < r->diagnostic_count; ++i) {
		if (r->diagnostics[i].address == addr &&
				r->diagnostics[i].kind == kind)
			return &r->diagnostics[i];
	}
	return NULL;
}

static const cs_cfg_register_value *find_reg(const cs_cfg_report *r,
		uint64_t block_addr, x86_reg reg)
{
	size_t i, k;
	for (i = 0; i < r->block_count; ++i) {
		if (r->blocks[i].address != block_addr)
			continue;
		for (k = 0; k < CS_CFG_GP_REG_COUNT; ++k) {
			const cs_cfg_register_value *v =
				&r->registers[i * CS_CFG_GP_REG_COUNT + k];
			if (v->reg == reg)
				return v;
		}
	}
	return NULL;
}

static const cs_cfg_overlap *find_overlap(const cs_cfg_report *r,
		uint64_t start, uint64_t first, uint64_t second)
{
	size_t i;
	for (i = 0; i < r->overlap_count; ++i) {
		if (r->overlaps[i].start == start &&
				r->overlaps[i].first == first &&
				r->overlaps[i].second == second)
			return &r->overlaps[i];
	}
	return NULL;
}

static const cs_cfg_indirect_target *find_indirect(
		const cs_cfg_report *r, uint64_t site, uint64_t target)
{
	size_t i;
	for (i = 0; i < r->indirect_target_count; ++i) {
		if (r->indirect_targets[i].site == site &&
				r->indirect_targets[i].target == target)
			return &r->indirect_targets[i];
	}
	return NULL;
}

static cs_cfg_region mkregion(uint64_t addr, const unsigned char *p,
		size_t n, bool executable, bool read_only)
{
	cs_cfg_region r;
	r.address = addr;
	r.data = p;
	r.size = n;
	r.executable = executable;
	r.read_only = read_only;
	return r;
}

/* ------------------------------------------------------------------ */
/* normal group                                                        */
/* ------------------------------------------------------------------ */

/* Conditional jump keeps both sides; values merge at the join. */
static void test_normal_merge_constants(void)
{
	img m;
	cs_cfg_region region;
	cs_cfg_report *r = NULL;
	uint64_t entry, je_at, jmp_at, l_true, l_merge;
	const cs_cfg_register_value *rdx;
	const cs_cfg_block *bm;
	static const unsigned char xor_eax[] = {0x31, 0xc0};
	static const unsigned char test_eax[] = {0x85, 0xc0};
	static const unsigned char mov_ecx_1[] = {
		0xb9, 0x01, 0x00, 0x00, 0x00
	};
	static const unsigned char mov_ecx_2[] = {
		0xb9, 0x02, 0x00, 0x00, 0x00
	};
	static const unsigned char mov_edx_ecx[] = {0x89, 0xca};
	static const unsigned char ret[] = {0xc3};

	img_init(&m, 0x1000);
	entry = img_here(&m);
	emit(&m, xor_eax, sizeof xor_eax);   /* eax = {0} */
	emit(&m, test_eax, sizeof test_eax); /* flags only */
	je_at = img_here(&m);
	emit_jcc_short(&m, 0x74, je_at + 2); /* placeholder je */
	emit(&m, mov_ecx_1, sizeof mov_ecx_1);
	jmp_at = img_here(&m);
	emit_jmp_short(&m, jmp_at + 2);      /* placeholder jmp */
	l_true = img_here(&m);
	emit(&m, mov_ecx_2, sizeof mov_ecx_2);
	l_merge = img_here(&m);
	emit(&m, mov_edx_ecx, sizeof mov_edx_ecx);
	emit(&m, ret, sizeof ret);
	m.buf[je_at - m.base + 1] =
		(unsigned char)(l_true - (je_at + 2));
	m.buf[jmp_at - m.base + 1] =
		(unsigned char)(l_merge - (jmp_at + 2));

	region = mkregion(0x1000, m.buf, m.len, true, false);
	UNIT_CHECK(cs_cfg_recover_x86_64(&region, 1, &entry, 1, &r) ==
			CS_ERR_OK);
	UNIT_CHECK(r != NULL);
	UNIT_CHECK(r->incomplete == false);

	/* blocks, edges and registers are inspected independently */
	UNIT_CHECK(r->block_count == 4);
	UNIT_CHECK(find_block(r, entry) != NULL);
	UNIT_CHECK(find_block(r, l_true) != NULL);
	UNIT_CHECK(find_block(r, l_merge) != NULL);

	UNIT_CHECK(r->edge_count == 4);
	UNIT_CHECK(find_edge(r, entry, l_true,
			CS_CFG_EDGE_TRUE_BRANCH) != NULL);
	UNIT_CHECK(find_edge(r, entry, je_at + 2,
			CS_CFG_EDGE_FALSE_BRANCH) != NULL);
	UNIT_CHECK(find_edge(r, je_at + 2, l_merge,
			CS_CFG_EDGE_JUMP) != NULL);
	UNIT_CHECK(find_edge(r, l_true, l_merge,
			CS_CFG_EDGE_FALLTHROUGH) != NULL);

	bm = find_block(r, l_merge);
	UNIT_CHECK(bm != NULL && bm->terminator == CS_CFG_TERM_RET);
	rdx = find_reg(r, l_merge, X86_REG_RDX);
	UNIT_CHECK(rdx != NULL);
	UNIT_CHECK(rdx->kind == CS_CFG_VALUE_SET);
	UNIT_CHECK(rdx->count == 2);
	UNIT_CHECK(rdx->values[0] == 1 && rdx->values[1] == 2);

	cs_cfg_report_free(r);
	img_free(&m);
}

/* Absolute eight-byte jump table resolved through known registers. */
static void test_normal_abs_jump_table(void)
{
	img code, data;
	cs_cfg_region regions[2];
	cs_cfg_report *r = NULL;
	uint64_t entry, t0, t1, jmp_site;
	const cs_cfg_block *bt0;

	img_init(&data, 0x4000);
	img_init(&code, 0x1000);

	entry = img_here(&code);
	{
		const unsigned char xor_eax[] = {0x31, 0xc0};
		const unsigned char mov_table_load[] = {
			0x48, 0x8b, 0x04, 0xc3 /* mov rax,[rbx+rax*8] */
		};
		const unsigned char jmp_rax[] = {0xff, 0xe0};
		const unsigned char mov_r8d_7[] = {
			0x41, 0xb8, 0x07, 0x00, 0x00, 0x00
		};
		const unsigned char mov_r9d_9[] = {
			0x41, 0xb9, 0x09, 0x00, 0x00, 0x00
		};
		const unsigned char ret[] = {0xc3};
		emit(&code, xor_eax, sizeof xor_eax);
		emit_lea_rip(&code, 0x1d, 0x4000); /* rbx = table base */
		emit(&code, mov_table_load, sizeof mov_table_load);
		jmp_site = img_here(&code);
		emit(&code, jmp_rax, sizeof jmp_rax);

		t0 = img_here(&code);
		emit(&code, mov_r8d_7, sizeof mov_r8d_7);
		emit(&code, ret, sizeof ret);
		t1 = img_here(&code);
		emit(&code, mov_r9d_9, sizeof mov_r9d_9);
		emit(&code, ret, sizeof ret);
	}
	emit_u64(&data, t0);
	emit_u64(&data, t1);

	regions[0] = mkregion(0x1000, code.buf, code.len, true, false);
	regions[1] = mkregion(0x4000, data.buf, data.len, false, true);
	UNIT_CHECK(cs_cfg_recover_x86_64(regions, 2, &entry, 1, &r) ==
			CS_ERR_OK);
	UNIT_CHECK(r->incomplete == false);
	UNIT_CHECK(find_indirect(r, jmp_site, t0) != NULL);
	UNIT_CHECK(find_edge(r, entry, t0, CS_CFG_EDGE_INDIRECT) != NULL);
	bt0 = find_block(r, t0);
	UNIT_CHECK(bt0 != NULL && bt0->terminator == CS_CFG_TERM_RET);
	/* index zero only: the second table entry is never read */
	UNIT_CHECK(r->indirect_target_count == 1);
	UNIT_CHECK(find_indirect(r, jmp_site, t1) == NULL);
	UNIT_CHECK(r->diagnostic_count == 0);

	cs_cfg_report_free(r);
	img_free(&code);
	img_free(&data);
}

/* CALL records its target but only traces the return successor; the callee
 * is entered only when it is also an explicit entry. Return successors lose
 * all general purpose register knowledge. */
static void test_normal_call_semantics(void)
{
	img m;
	cs_cfg_region region;
	cs_cfg_report *r = NULL;
	uint64_t entry, call_site, after_call, callee;
	uint64_t two_entries[2];
	const cs_cfg_register_value *rax;
	static const unsigned char mov_eax_41[] = {
		0xb8, 0x41, 0x00, 0x00, 0x00
	};
	static const unsigned char test_eax[] = {0x85, 0xc0};
	static const unsigned char ret[] = {0xc3};
	static const unsigned char mov_eax_1[] = {
		0xb8, 0x01, 0x00, 0x00, 0x00
	};

	img_init(&m, 0x1000);
	entry = img_here(&m);
	emit(&m, mov_eax_41, sizeof mov_eax_41);
	call_site = img_here(&m);
	{
		/* placeholder call, patched once callee is known */
		const unsigned char call[5] = {0xe8, 0, 0, 0, 0};
		emit(&m, call, sizeof call);
	}
	after_call = img_here(&m);
	emit(&m, test_eax, sizeof test_eax);
	emit(&m, ret, sizeof ret);
	callee = img_here(&m);
	emit(&m, mov_eax_1, sizeof mov_eax_1);
	emit(&m, ret, sizeof ret);
	{
		int32_t rel = (int32_t)(callee - (call_site + 5));
		m.buf[call_site - m.base + 1] = (unsigned char)rel;
		m.buf[call_site - m.base + 2] = (unsigned char)(rel >> 8);
		m.buf[call_site - m.base + 3] = (unsigned char)(rel >> 16);
		m.buf[call_site - m.base + 4] = (unsigned char)(rel >> 24);
	}

	region = mkregion(0x1000, m.buf, m.len, true, false);

	/* single entry: the callee is recorded as an edge but not entered */
	UNIT_CHECK(cs_cfg_recover_x86_64(&region, 1, &entry, 1, &r) ==
			CS_ERR_OK);
	/* edge sources are block starts; the call terminates the block that
	 * begins at entry */
	UNIT_CHECK(find_edge(r, entry, callee,
			CS_CFG_EDGE_CALL) != NULL);
	UNIT_CHECK(find_edge(r, entry, after_call,
			CS_CFG_EDGE_CALL_RETURN) != NULL);
	UNIT_CHECK(find_block(r, callee) == NULL);
	UNIT_CHECK(find_block(r, after_call) != NULL);
	rax = find_reg(r, after_call, X86_REG_RAX);
	UNIT_CHECK(rax != NULL && rax->kind == CS_CFG_VALUE_UNKNOWN);
	UNIT_CHECK(r->incomplete == false);
	cs_cfg_report_free(r);
	r = NULL;

	/* the same callee supplied as an explicit entry is now recovered */
	two_entries[0] = entry;
	two_entries[1] = callee;
	UNIT_CHECK(cs_cfg_recover_x86_64(&region, 1, two_entries, 2, &r) ==
			CS_ERR_OK);
	UNIT_CHECK(find_block(r, callee) != NULL);
	UNIT_CHECK(find_edge(r, entry, callee,
			CS_CFG_EDGE_CALL) != NULL);
	UNIT_CHECK(find_edge(r, entry, after_call,
			CS_CFG_EDGE_CALL_RETURN) != NULL);
	cs_cfg_report_free(r);
	img_free(&m);
}

/* Reordered regions and entries produce the identical canonical report. */
static void test_normal_deterministic_ordering(void)
{
	img code, data;
	cs_cfg_region fwd[2], rev[2];
	cs_cfg_report *a = NULL, *b = NULL;
	uint64_t entry, extra, target;
	uint64_t entries_fwd[2], entries_rev[2];
	size_t i;

	img_init(&data, 0x4000);
	img_init(&code, 0x1000);
	entry = img_here(&code);
	{
		const unsigned char xor_eax[] = {0x31, 0xc0};
		const unsigned char load[] = {0x48, 0x8b, 0x04, 0xc3};
		const unsigned char jmp_rax[] = {0xff, 0xe0};
		const unsigned char ret[] = {0xc3};
		emit(&code, xor_eax, sizeof xor_eax);
		emit_lea_rip(&code, 0x1d, 0x4000);
		emit(&code, load, sizeof load);
		emit(&code, jmp_rax, sizeof jmp_rax);
		target = img_here(&code);
		emit(&code, ret, sizeof ret);
		extra = img_here(&code);
		emit(&code, ret, sizeof ret);
	}
	emit_u64(&data, target);

	fwd[0] = mkregion(0x1000, code.buf, code.len, true, false);
	fwd[1] = mkregion(0x4000, data.buf, data.len, false, true);
	rev[0] = fwd[1];
	rev[1] = fwd[0];
	entries_fwd[0] = entry;
	entries_fwd[1] = extra;
	entries_rev[0] = extra;
	entries_rev[1] = entry;

	UNIT_CHECK(cs_cfg_recover_x86_64(fwd, 2, entries_fwd, 2, &a) ==
			CS_ERR_OK);
	UNIT_CHECK(cs_cfg_recover_x86_64(rev, 2, entries_rev, 2, &b) ==
			CS_ERR_OK);

	UNIT_CHECK(a->block_count == b->block_count);
	UNIT_CHECK(a->edge_count == b->edge_count);
	UNIT_CHECK(a->diagnostic_count == b->diagnostic_count);
	UNIT_CHECK(a->indirect_target_count == b->indirect_target_count);
	UNIT_CHECK(a->overlap_count == b->overlap_count);
	UNIT_CHECK(a->entry_count == b->entry_count);
	for (i = 0; i < a->block_count; ++i) {
		UNIT_CHECK(a->blocks[i].address == b->blocks[i].address);
		UNIT_CHECK(a->blocks[i].end == b->blocks[i].end);
		UNIT_CHECK(a->blocks[i].terminator ==
				b->blocks[i].terminator);
	}
	for (i = 0; i < a->edge_count; ++i) {
		UNIT_CHECK(a->edges[i].source == b->edges[i].source);
		UNIT_CHECK(a->edges[i].target == b->edges[i].target);
		UNIT_CHECK(a->edges[i].kind == b->edges[i].kind);
	}
	for (i = 0; i < a->diagnostic_count; ++i) {
		UNIT_CHECK(a->diagnostics[i].address ==
				b->diagnostics[i].address);
		UNIT_CHECK(a->diagnostics[i].kind ==
				b->diagnostics[i].kind);
	}
	for (i = 0; i < a->indirect_target_count; ++i) {
		UNIT_CHECK(a->indirect_targets[i].site ==
				b->indirect_targets[i].site);
		UNIT_CHECK(a->indirect_targets[i].target ==
				b->indirect_targets[i].target);
	}

	cs_cfg_report_free(a);
	cs_cfg_report_free(b);
	img_free(&code);
	img_free(&data);
}

/* ------------------------------------------------------------------ */
/* boundary group                                                      */
/* ------------------------------------------------------------------ */

/* A fixed point over the loop back edge accumulates values until the
 * eight-entry cap promotes the register to unknown. */
static void test_boundary_loop_unknown(void)
{
	img m;
	cs_cfg_region region;
	cs_cfg_report *r = NULL;
	uint64_t entry, loop, tail;
	const cs_cfg_register_value *rax;

	img_init(&m, 0x1000);
	entry = img_here(&m);
	{
		const unsigned char mov_eax_1[] = {
			0xb8, 0x01, 0x00, 0x00, 0x00
		};
		const unsigned char add_eax_1[] = {0x83, 0xc0, 0x01};
		const unsigned char cmp_eax_9[] = {0x83, 0xf8, 0x09};
		const unsigned char ret[] = {0xc3};
		emit(&m, mov_eax_1, sizeof mov_eax_1);
		loop = img_here(&m);
		emit(&m, add_eax_1, sizeof add_eax_1);
		emit(&m, cmp_eax_9, sizeof cmp_eax_9);
		emit_jcc_short(&m, 0x75, loop); /* jne loop */
		tail = img_here(&m);
		emit(&m, ret, sizeof ret);
	}

	region = mkregion(0x1000, m.buf, m.len, true, false);
	UNIT_CHECK(cs_cfg_recover_x86_64(&region, 1, &entry, 1, &r) ==
			CS_ERR_OK);
	UNIT_CHECK(r->incomplete == false);
	UNIT_CHECK(r->block_count == 3);
	UNIT_CHECK(find_edge(r, loop, loop, CS_CFG_EDGE_TRUE_BRANCH) != NULL);
	UNIT_CHECK(find_edge(r, loop, tail,
			CS_CFG_EDGE_FALSE_BRANCH) != NULL);
	rax = find_reg(r, tail, X86_REG_RAX);
	UNIT_CHECK(rax != NULL);
	UNIT_CHECK(rax->kind == CS_CFG_VALUE_UNKNOWN);

	cs_cfg_report_free(r);
	img_free(&m);
}

/* 8/16 bit alias writes invalidate the whole 64 bit identity: stale
 * constants must not survive to feed an indirect target. */
static void test_boundary_partial_alias(void)
{
	img m;
	cs_cfg_region region;
	cs_cfg_report *r = NULL;
	uint64_t entry;
	const cs_cfg_register_value *rax, *rbx, *r12;

	img_init(&m, 0x1000);
	entry = img_here(&m);
	{
		const unsigned char mov_eax_big[] = {
			0xb8, 0x44, 0x43, 0x42, 0x41
		};
		const unsigned char mov_al_5[] = {0xb0, 0x05};
		const unsigned char mov_rbx_1[] = {
			0x48, 0xc7, 0xc3, 0x01, 0x00, 0x00, 0x00
		};
		const unsigned char mov_bx_7[] = {0x66, 0xbb, 0x07, 0x00};
		const unsigned char mov_r12b_9[] = {0x41, 0xb4, 0x09};
		const unsigned char ret[] = {0xc3};
		emit(&m, mov_eax_big, sizeof mov_eax_big);
		emit(&m, mov_al_5, sizeof mov_al_5);
		emit(&m, mov_rbx_1, sizeof mov_rbx_1);
		emit(&m, mov_bx_7, sizeof mov_bx_7);
		emit(&m, mov_r12b_9, sizeof mov_r12b_9);
		emit(&m, ret, sizeof ret);
	}

	region = mkregion(0x1000, m.buf, m.len, true, false);
	UNIT_CHECK(cs_cfg_recover_x86_64(&region, 1, &entry, 1, &r) ==
			CS_ERR_OK);
	UNIT_CHECK(r->block_count == 1);
	rax = find_reg(r, entry, X86_REG_RAX);
	rbx = find_reg(r, entry, X86_REG_RBX);
	r12 = find_reg(r, entry, X86_REG_R12);
	UNIT_CHECK(rax != NULL && rax->kind == CS_CFG_VALUE_UNKNOWN);
	UNIT_CHECK(rbx != NULL && rbx->kind == CS_CFG_VALUE_UNKNOWN);
	UNIT_CHECK(r12 != NULL && r12->kind == CS_CFG_VALUE_UNKNOWN);

	cs_cfg_report_free(r);
	img_free(&m);
}

/* 32 bit writes zero extend and 32 bit arithmetic wraps at 2^32. */
static void test_boundary_wrap32_zero_extend(void)
{
	img m;
	cs_cfg_region region;
	cs_cfg_report *r = NULL;
	uint64_t entry;
	const cs_cfg_register_value *rax, *rcx;

	img_init(&m, 0x1000);
	entry = img_here(&m);
	{
		const unsigned char mov_eax_max[] = {
			0xb8, 0xff, 0xff, 0xff, 0xff
		};
		const unsigned char add_eax_1[] = {0x83, 0xc0, 0x01};
		const unsigned char mov_ecx_eax[] = {0x89, 0xc1};
		const unsigned char sub_eax_1[] = {0x83, 0xe8, 0x01};
		const unsigned char ret[] = {0xc3};
		emit(&m, mov_eax_max, sizeof mov_eax_max);
		emit(&m, add_eax_1, sizeof add_eax_1); /* wraps to 0 */
		emit(&m, mov_ecx_eax, sizeof mov_ecx_eax);
		emit(&m, sub_eax_1, sizeof sub_eax_1); /* 0 - 1 -> 0xffffffff */
		emit(&m, ret, sizeof ret);
	}

	region = mkregion(0x1000, m.buf, m.len, true, false);
	UNIT_CHECK(cs_cfg_recover_x86_64(&region, 1, &entry, 1, &r) ==
			CS_ERR_OK);
	rax = find_reg(r, entry, X86_REG_RAX);
	rcx = find_reg(r, entry, X86_REG_RCX);
	UNIT_CHECK(rax != NULL && rax->kind == CS_CFG_VALUE_SET);
	UNIT_CHECK(rax->count == 1 && rax->values[0] == 0xffffffffULL);
	UNIT_CHECK(rcx != NULL && rcx->kind == CS_CFG_VALUE_SET);
	UNIT_CHECK(rcx->count == 1 && rcx->values[0] == 0);

	cs_cfg_report_free(r);
	img_free(&m);
}

/* Empty entries give an empty graph; bad arguments are rejected. */
static void test_boundary_empty_and_bad_args(void)
{
	static const unsigned char code[] = {0xc3};
	static const unsigned char data[] = {0, 0, 0, 0};
	cs_cfg_region regions[2];
	cs_cfg_report *r = NULL;
	uint64_t entry = 0x1000;
	cs_cfg_region wrap, overlap[2];
	uint64_t bad_entry = 0x4000;

	regions[0] = mkregion(0x1000, code, sizeof code, true, false);
	regions[1] = mkregion(0x4000, data, sizeof data, false, true);

	UNIT_CHECK(cs_cfg_recover_x86_64(regions, 2, NULL, 0, &r) ==
			CS_ERR_OK);
	UNIT_CHECK(r != NULL);
	UNIT_CHECK(r->block_count == 0 && r->edge_count == 0);
	UNIT_CHECK(r->diagnostic_count == 0 && r->incomplete == false);
	cs_cfg_report_free(r);
	r = NULL;

	UNIT_CHECK(cs_cfg_recover_x86_64(regions, 2, NULL, 1, &r) ==
			CS_ERR_OPTION);
	UNIT_CHECK(r == NULL);
	UNIT_CHECK(cs_cfg_recover_x86_64(regions, 2, &bad_entry, 1, &r) ==
			CS_ERR_OPTION);
	UNIT_CHECK(r == NULL);
	UNIT_CHECK(cs_cfg_recover_x86_64(regions, 2, &entry, 1, NULL) ==
			CS_ERR_OPTION);

	overlap[0] = mkregion(0x1000, code, sizeof code, true, false);
	overlap[1] = mkregion(0x1000, data, sizeof data, true, false);
	UNIT_CHECK(cs_cfg_recover_x86_64(overlap, 2, &entry, 1, &r) ==
			CS_ERR_OPTION);
	UNIT_CHECK(r == NULL);

	wrap = mkregion(UINT64_MAX - 3, code, 10, true, false);
	UNIT_CHECK(cs_cfg_recover_x86_64(&wrap, 1, &entry, 1, &r) ==
			CS_ERR_OPTION);
	UNIT_CHECK(r == NULL);
}

/* An unknown index register makes the table address, and therefore the
 * indirect jump target, unknown. */
static void test_boundary_unknown_index_unresolved(void)
{
	img code, data;
	cs_cfg_region regions[2];
	cs_cfg_report *r = NULL;
	uint64_t entry, jmp_site;
	static const unsigned char jmp_mem[] = {
		0xff, 0x24, 0x0b /* jmp [rbx + rcx] */
	};

	img_init(&code, 0x1000);
	img_init(&data, 0x4000);
	entry = img_here(&code);
	emit_lea_rip(&code, 0x1d, 0x4000);   /* rbx = known table base */
	jmp_site = img_here(&code);
	emit(&code, jmp_mem, sizeof jmp_mem); /* rcx stays unknown */
	emit_u64(&data, 0x1234);

	regions[0] = mkregion(0x1000, code.buf, code.len, true, false);
	regions[1] = mkregion(0x4000, data.buf, data.len, false, true);
	UNIT_CHECK(cs_cfg_recover_x86_64(regions, 2, &entry, 1, &r) ==
			CS_ERR_OK);
	UNIT_CHECK(r->incomplete == true);
	UNIT_CHECK(r->indirect_target_count == 0);
	UNIT_CHECK(find_diag(r, jmp_site,
			CS_CFG_DIAG_UNRESOLVED_INDIRECT) != NULL);

	cs_cfg_report_free(r);
	img_free(&code);
	img_free(&data);
}

/* ------------------------------------------------------------------ */
/* integration group                                                   */
/* ------------------------------------------------------------------ */

/* PIC jump table: signed 32 bit displacement plus table base, including a
 * negative displacement that reaches code before the table. */
static void test_integration_pic_negative_offset(void)
{
	static unsigned char code[0x1000];
	static const uint64_t CODE = 0x1000;
	static const uint64_t DATA = 0x2000;
	static const uint64_t TARGET = DATA - 8; /* 0x1ff8 */
	img data_img;
	cs_cfg_region regions[2];
	cs_cfg_report *r = NULL;
	uint64_t entry, jmp_site;
	const cs_cfg_register_value *rax;
	const cs_cfg_block *bt;
	img m;

	/* code image lives in the fixed size array */
	memset(code, 0, sizeof code);
	m.cap = sizeof code;
	m.buf = code;
	m.base = CODE;
	m.len = 0;

	entry = img_here(&m);
	{
		const unsigned char movsxd[] = {0x48, 0x63, 0x03};
		const unsigned char lea_add[] = {0x48, 0x8d, 0x04, 0x03};
		const unsigned char jmp_rax[] = {0xff, 0xe0};
		emit_lea_rip(&m, 0x1d, DATA);           /* rbx = table */
		emit(&m, movsxd, sizeof movsxd);        /* rax = (s32)-8 */
		emit(&m, lea_add, sizeof lea_add);      /* rax = table-8 */
		jmp_site = img_here(&m);
		emit(&m, jmp_rax, sizeof jmp_rax);
	}
	/* target code placed at 0x1ff8 inside the same executable region */
	m.len = (size_t)(TARGET - CODE);
	{
		const unsigned char mov_r8d_1[] = {
			0x41, 0xb8, 0x01, 0x00, 0x00, 0x00
		};
		const unsigned char ret[] = {0xc3};
		emit(&m, mov_r8d_1, sizeof mov_r8d_1);
		emit(&m, ret, sizeof ret);
	}

	img_init(&data_img, DATA);
	emit_u32(&data_img, (uint32_t)(-8));
	emit_u32(&data_img, 16);

	regions[0] = mkregion(CODE, code, sizeof code, true, false);
	regions[1] = mkregion(DATA, data_img.buf, data_img.len, false, true);
	UNIT_CHECK(cs_cfg_recover_x86_64(regions, 2, &entry, 1, &r) ==
			CS_ERR_OK);
	UNIT_CHECK(r->incomplete == false);
	UNIT_CHECK(find_indirect(r, jmp_site, TARGET) != NULL);
	bt = find_block(r, TARGET);
	UNIT_CHECK(bt != NULL && bt->terminator == CS_CFG_TERM_RET);
	rax = find_reg(r, entry, X86_REG_RAX);
	UNIT_CHECK(rax != NULL && rax->kind == CS_CFG_VALUE_SET);
	UNIT_CHECK(rax->count == 1 && rax->values[0] == TARGET);
	UNIT_CHECK(find_edge(r, entry, TARGET, CS_CFG_EDGE_INDIRECT) != NULL);

	cs_cfg_report_free(r);
	img_free(&data_img);
}

/* A table-derived target lands inside an already decoded instruction: both
 * decode chains survive, the overlap is reported, and an exact-boundary
 * conditional target splits an existing block and reconnects the edges. */
static void test_integration_overlap_split(void)
{
	img code, data;
	cs_cfg_region regions[2];
	cs_cfg_report *r = NULL;
	uint64_t entry, split, fall, mid = 0x1003;
	uint64_t jcc_at, jmp_indirect_at;
	const cs_cfg_block *be, *bs, *bmid;
	const cs_cfg_overlap *ov;
	static const unsigned char mov_r8d_1[] = {
		0x41, 0xb8, 0x01, 0x00, 0x00, 0x00
	};
	static const unsigned char xor_eax[] = {0x31, 0xc0};
	static const unsigned char cmp_eax_1[] = {0x83, 0xf8, 0x01};
	static const unsigned char mov_edx_2[] = {
		0xba, 0x02, 0x00, 0x00, 0x00
	};
	static const unsigned char mov_ecx_5[] = {
		0xb9, 0x05, 0x00, 0x00, 0x00
	};
	static const unsigned char mov_rax_mem[] = {0x48, 0x8b, 0x03};
	static const unsigned char jmp_rax[] = {0xff, 0xe0};

	img_init(&code, 0x1000);
	img_init(&data, 0x4000);
	entry = img_here(&code);
	emit(&code, mov_r8d_1, sizeof mov_r8d_1); /* 0x1000..0x1006 */
	emit(&code, xor_eax, sizeof xor_eax);
	emit(&code, cmp_eax_1, sizeof cmp_eax_1);
	jcc_at = img_here(&code);
	emit_jcc_short(&code, 0x75, jcc_at + 2);  /* placeholder jne */
	fall = img_here(&code);
	emit(&code, mov_edx_2, sizeof mov_edx_2);
	split = img_here(&code);
	code.buf[jcc_at - code.base + 1] =
		(unsigned char)(split - (jcc_at + 2));
	emit(&code, mov_ecx_5, sizeof mov_ecx_5);
	emit_lea_rip(&code, 0x1d, 0x4000);         /* rbx = table */
	emit(&code, mov_rax_mem, sizeof mov_rax_mem);
	jmp_indirect_at = img_here(&code);
	emit(&code, jmp_rax, sizeof jmp_rax);
	emit_u64(&data, mid);

	regions[0] = mkregion(0x1000, code.buf, code.len, true, false);
	regions[1] = mkregion(0x4000, data.buf, data.len, false, true);
	UNIT_CHECK(cs_cfg_recover_x86_64(regions, 2, &entry, 1, &r) ==
			CS_ERR_OK);

	/* overlap: both chains retained and the bytes are reported */
	ov = find_overlap(r, 0x1003, 0x1000, 0x1003);
	UNIT_CHECK(ov != NULL);
	be = find_block(r, entry);
	UNIT_CHECK(be != NULL && be->overlap_count >= 1);
	bmid = find_block(r, mid);
	UNIT_CHECK(bmid != NULL);

	/* exact boundary split with reconnected edges */
	bs = find_block(r, split);
	UNIT_CHECK(bs != NULL);
	UNIT_CHECK(find_edge(r, entry, split,
			CS_CFG_EDGE_TRUE_BRANCH) != NULL);
	UNIT_CHECK(find_edge(r, entry, fall,
			CS_CFG_EDGE_FALSE_BRANCH) != NULL);
	UNIT_CHECK(find_indirect(r, jmp_indirect_at, mid) != NULL);

	cs_cfg_report_free(r);
	img_free(&code);
	img_free(&data);
}

/* Unmapped gaps between executable regions are never linearly swept: the
 * chain stops at the gap with an addressed diagnostic, while a second region
 * reached through an explicit entry is recovered independently. */
static void test_integration_data_gap(void)
{
	static const unsigned char head[] = {
		0xb8, 0x01, 0x00, 0x00, 0x00 /* mov eax,1; region ends */
	};
	static const unsigned char tail[] = {
		0x41, 0xb8, 0x01, 0x00, 0x00, 0x00, /* mov r8d,1 */
		0xc3                                  /* ret */
	};
	cs_cfg_region regions[2];
	cs_cfg_report *r = NULL;
	uint64_t entries[2];

	regions[0] = mkregion(0x1000, head, sizeof head, true, false);
	regions[1] = mkregion(0x2000, tail, sizeof tail, true, false);
	entries[0] = 0x1000;
	entries[1] = 0x2000;

	UNIT_CHECK(cs_cfg_recover_x86_64(regions, 2, entries, 2, &r) ==
			CS_ERR_OK);
	UNIT_CHECK(r->incomplete == true);
	UNIT_CHECK(r->block_count == 2);
	UNIT_CHECK(find_block(r, 0x1000) != NULL);
	UNIT_CHECK(find_block(r, 0x1005) == NULL);
	UNIT_CHECK(find_block(r, 0x2000) != NULL);
	/* nothing was swept out of the hole between the regions */
	{
		size_t i;
		for (i = 0; i < r->block_count; ++i) {
			UNIT_CHECK(r->blocks[i].address < 0x1100 ||
				r->blocks[i].address >= 0x2000);
		}
	}
	UNIT_CHECK(find_diag(r, 0x1005, CS_CFG_DIAG_OUTSIDE_REGION) != NULL);

	cs_cfg_report_free(r);
}

/* Cross-boundary table read widens to unknown; outside targets, illegal and
 * truncated instructions each get an addressed diagnostic. */
static void test_integration_cross_boundary_diags(void)
{
	img code, data;
	cs_cfg_region fwd[2], rev[2];
	cs_cfg_report *a = NULL, *b = NULL;
	uint64_t e1, e2, e3, e4, jmp_site;
	uint64_t entries[4], entries_rev[4];
	static const unsigned char filler[12] = {0};

	img_init(&code, 0x1000);
	img_init(&data, 0x4000);

	e1 = img_here(&code);
	{
		const unsigned char load_cross[] = {
			0x48, 0x8b, 0x43, 0x08 /* mov rax,[rbx+8] */
		};
		const unsigned char jmp_rax[] = {0xff, 0xe0};
		emit_lea_rip(&code, 0x1d, 0x4000);
		emit(&code, load_cross, sizeof load_cross);
		jmp_site = img_here(&code);
		emit(&code, jmp_rax, sizeof jmp_rax);
	}

	e2 = img_here(&code);
	emit_jmp_near(&code, 0x9000); /* outside every region */

	e3 = img_here(&code);
	{
		const unsigned char illegal[] = {0xd6};
		emit(&code, illegal, sizeof illegal);
	}

	e4 = img_here(&code);
	{
		const unsigned char truncated[] = {0x0f};
		emit(&code, truncated, sizeof truncated);
	}
	/* data region is only 12 bytes: the qword at +8 needs 16 */
	emit(&data, filler, sizeof filler);

	fwd[0] = mkregion(0x1000, code.buf, code.len, true, false);
	fwd[1] = mkregion(0x4000, data.buf, data.len, false, true);
	rev[0] = fwd[1];
	rev[1] = fwd[0];
	entries[0] = e1;
	entries[1] = e2;
	entries[2] = e3;
	entries[3] = e4;
	entries_rev[0] = e4;
	entries_rev[1] = e3;
	entries_rev[2] = e2;
	entries_rev[3] = e1;

	UNIT_CHECK(cs_cfg_recover_x86_64(fwd, 2, entries, 4, &a) ==
			CS_ERR_OK);
	UNIT_CHECK(a->incomplete == true);

	/* diagnostics are listed independently by kind and address */
	UNIT_CHECK(find_diag(a, jmp_site,
			CS_CFG_DIAG_UNRESOLVED_INDIRECT) != NULL);
	UNIT_CHECK(find_diag(a, 0x9000, CS_CFG_DIAG_OUTSIDE_REGION) != NULL);
	UNIT_CHECK(find_diag(a, e3, CS_CFG_DIAG_ILLEGAL) != NULL);
	UNIT_CHECK(find_diag(a, e4, CS_CFG_DIAG_TRUNCATED) != NULL);
	UNIT_CHECK(a->diagnostic_count == 4);
	/* valid blocks are still recovered around the failures */
	UNIT_CHECK(find_block(a, e1) != NULL);
	UNIT_CHECK(find_block(a, e2) != NULL);

	/* reordered inputs give the same canonical diagnostics */
	UNIT_CHECK(cs_cfg_recover_x86_64(rev, 2, entries_rev, 4, &b) ==
			CS_ERR_OK);
	UNIT_CHECK(b->diagnostic_count == a->diagnostic_count);
	UNIT_CHECK(b->block_count == a->block_count);
	UNIT_CHECK(b->edge_count == a->edge_count);
	{
		size_t i;
		for (i = 0; i < a->diagnostic_count; ++i) {
			UNIT_CHECK(a->diagnostics[i].address ==
					b->diagnostics[i].address);
			UNIT_CHECK(a->diagnostics[i].kind ==
					b->diagnostics[i].kind);
		}
	}

	cs_cfg_report_free(a);
	cs_cfg_report_free(b);
	img_free(&code);
	img_free(&data);
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

const unit_case unit_cases[] = {
	{"normal_merge_constants", "normal",
		test_normal_merge_constants},
	{"normal_abs_jump_table", "normal",
		test_normal_abs_jump_table},
	{"normal_call_semantics", "normal",
		test_normal_call_semantics},
	{"normal_deterministic_ordering", "normal",
		test_normal_deterministic_ordering},
	{"boundary_loop_unknown", "boundary",
		test_boundary_loop_unknown},
	{"boundary_partial_alias", "boundary",
		test_boundary_partial_alias},
	{"boundary_wrap32_zero_extend", "boundary",
		test_boundary_wrap32_zero_extend},
	{"boundary_empty_and_bad_args", "boundary",
		test_boundary_empty_and_bad_args},
	{"boundary_unknown_index_unresolved", "boundary",
		test_boundary_unknown_index_unresolved},
	{"integration_pic_negative_offset", "integration",
		test_integration_pic_negative_offset},
	{"integration_overlap_split", "integration",
		test_integration_overlap_split},
	{"integration_data_gap", "integration",
		test_integration_data_gap},
	{"integration_cross_boundary_diags", "integration",
		test_integration_cross_boundary_diags},
};

const size_t unit_case_count =
	sizeof(unit_cases) / sizeof(unit_cases[0]);
