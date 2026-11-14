/*
 * tests/clr_methods_cases.c - task cases for the static CLR method index.
 *
 * Every expected value below is listed independently of the implementation:
 * the in-memory image builder only lays out bytes, while the checks compare
 * against hand-written tokens, orders, offsets and byte sequences.
 */

#include "task_support/check.h"

#include <libpe/pe.h>
#include <libpe/clr_methods.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Minimal managed PE builder, width-aware and option-driven.          */
/* ------------------------------------------------------------------ */

#define IMG_SIZE 0x4000u

struct build_opts {
	int ptr;                 /* emit MethodPtr/ParamPtr (#- semantics) */
	int wide;                /* set all HeapSizes bits (4-byte heaps) */
	int shared_rva;          /* Store shares Load's body RVA */
	int zero_il;             /* shrink Load's tiny body to CodeSize 0 */
	int bad_methodptr;       /* MethodPtr is not a permutation */
	int hole_rva;            /* Load points at an unbacked RVA */
	int unsupported_table;   /* add a nonempty table 42 */
};

struct built_image {
	unsigned char *buf;
	size_t size;
};

static void w16(unsigned char *p, uint16_t v) {
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}
static void w32(unsigned char *p, uint32_t v) {
	for (int i = 0; i < 4; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

struct tw {
	unsigned char *tab;
	size_t nt;
	int wide;
};
static void t_u2(struct tw *t, uint16_t v) { w16(t->tab + t->nt, v); t->nt += 2; }
static void t_u4(struct tw *t, uint32_t v) { w32(t->tab + t->nt, v); t->nt += 4; }
static void t_str(struct tw *t, uint32_t idx) {
	if (t->wide) t_u4(t, idx); else t_u2(t, (uint16_t)idx);
}
static void t_guid(struct tw *t, uint32_t idx) {
	if (t->wide) t_u4(t, idx); else t_u2(t, (uint16_t)idx);
}
static void t_blob(struct tw *t, uint32_t idx) {
	if (t->wide) t_u4(t, idx); else t_u2(t, (uint16_t)idx);
}

struct strheap {
	unsigned char *h;
	size_t n;
};
static unsigned s_add(struct strheap *s, const char *v) {
	unsigned off = (unsigned)s->n;
	size_t z = strlen(v) + 1;
	memcpy(s->h + s->n, v, z);
	s->n += z;
	return off;
}

static struct built_image build_image(const struct build_opts *o) {
	struct built_image out = {0};
	out.buf = calloc(1, IMG_SIZE);
	out.size = IMG_SIZE;
	unsigned char *f = out.buf;

	unsigned char tab[2048];
	unsigned char sh[256];
	unsigned char bh[128];
	unsigned char meta[4096];
	memset(tab, 0, sizeof(tab));
	memset(sh, 0, sizeof(sh));
	memset(bh, 0, sizeof(bh));
	memset(meta, 0, sizeof(meta));

	struct strheap strings = { sh, 1 }; /* index 0 is the empty string */
	size_t bn = 1;                        /* blob index 0 is NULL */

	unsigned u_module   = s_add(&strings, "sample.netmodule");
	unsigned u_global   = s_add(&strings, "<Module>");
	unsigned u_outer    = s_add(&strings, "Outer");
	unsigned u_inner    = s_add(&strings, "Inner");
	unsigned u_space    = s_add(&strings, "Demo");
	unsigned u_object   = s_add(&strings, "Object");
	unsigned u_system   = s_add(&strings, "System");
	unsigned u_mscorlib = s_add(&strings, "mscorlib");
	unsigned u_load     = s_add(&strings, "Load");
	unsigned u_store    = s_add(&strings, "Store");
	unsigned u_stub     = s_add(&strings, "Stub");
	unsigned u_a        = s_add(&strings, "a");
	unsigned u_b        = s_add(&strings, "b");
	unsigned u_result   = s_add(&strings, "result");

	/* Blob signatures: compressed length prefix then raw payload. */
	unsigned sig4 = (unsigned)bn;
	bh[bn++] = 4; bh[bn++] = 0; bh[bn++] = 1; bh[bn++] = 8; bh[bn++] = 8;
	unsigned sig3 = (unsigned)bn;
	bh[bn++] = 3; bh[bn++] = 0x20; bh[bn++] = 0; bh[bn++] = 8;

	uint64_t valid = (1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 6)
		| (1ULL << 8) | (1ULL << 35) | (1ULL << 41);
	if (o->ptr)
		valid |= (1ULL << 5) | (1ULL << 7);
	if (o->unsupported_table)
		valid |= (1ULL << 42);

	struct tw t = { tab, 24, o->wide };
	tab[4] = 2; /* MajorVersion */
	tab[5] = 1; /* MinorVersion */
	tab[6] = (unsigned char)(o->wide ? 0x07 : 0x00); /* HeapSizes */
	tab[7] = 0; /* reserved */
	for (int i = 0; i < 8; i++)
		tab[8 + i] = (unsigned char)(valid >> (8 * i));
	for (unsigned id = 0; id < 64; id++) {
		if (!(valid & (1ULL << id)))
			continue;
		t_u4(&t, (id == 2 || id == 5 || id == 6 || id == 7 || id == 8
			|| id == 42) ? 3 : 1);
	}

	/* Module: Generation, Name, Mvid, EncId, EncBaseId. */
	t_u2(&t, 0); t_str(&t, u_module); t_guid(&t, 1);
	t_guid(&t, 0); t_guid(&t, 0);
	/* TypeRef: ResolutionScope(coded u2), Name, Namespace. */
	t_u2(&t, 6); t_str(&t, u_object); t_str(&t, u_system);
	/* Three TypeDef rows. */
	t_u4(&t, 0);    t_str(&t, u_global); t_str(&t, 0);
	t_u2(&t, 0); t_u2(&t, 1); t_u2(&t, 1);
	t_u4(&t, 0x81); t_str(&t, u_outer); t_str(&t, u_space);
	t_u2(&t, 5); t_u2(&t, 1); t_u2(&t, 1);
	t_u4(&t, 2);    t_str(&t, u_inner); t_str(&t, 0);
	t_u2(&t, 5); t_u2(&t, 2); t_u2(&t, 2);

	uint32_t load_rva = o->hole_rva ? 0x9000u : 0x4000u;
	uint32_t store_rva = o->shared_rva ? 0x4000u : 0x4ff8u;

	if (o->ptr) {
		/* MethodPtr: logical -> physical. */
		if (o->bad_methodptr) {
			t_u2(&t, 1); t_u2(&t, 1); t_u2(&t, 2);
		} else {
			t_u2(&t, 3); t_u2(&t, 1); t_u2(&t, 2);
		}
	}
	if (o->ptr) {
		/* Physical rows: Load, Store, Stub. Columns: RVA, Impl, Flags. */
		t_u4(&t, load_rva); t_u2(&t, 0); t_u2(&t, 0x16);
		t_str(&t, u_load); t_blob(&t, sig4); t_u2(&t, 1);
		t_u4(&t, store_rva); t_u2(&t, 0); t_u2(&t, 0x16);
		t_str(&t, u_store); t_blob(&t, sig4); t_u2(&t, 3);
		t_u4(&t, 0); t_u2(&t, 0); t_u2(&t, 0x446);
		t_str(&t, u_stub); t_blob(&t, sig3); t_u2(&t, 4);
	} else {
		/* Physical rows: Stub, Load, Store. */
		t_u4(&t, 0); t_u2(&t, 0); t_u2(&t, 0x446);
		t_str(&t, u_stub); t_blob(&t, sig3); t_u2(&t, 1);
		t_u4(&t, load_rva); t_u2(&t, 0); t_u2(&t, 0x16);
		t_str(&t, u_load); t_blob(&t, sig4); t_u2(&t, 1);
		t_u4(&t, store_rva); t_u2(&t, 0); t_u2(&t, 0x16);
		t_str(&t, u_store); t_blob(&t, sig4); t_u2(&t, 3);
	}
	if (o->ptr) {
		/* ParamPtr: logical -> physical. */
		t_u2(&t, 2); t_u2(&t, 1); t_u2(&t, 3);
	}
	if (o->ptr) {
		/* Physical Param rows: a, result, b. */
		t_u2(&t, 0); t_u2(&t, 1); t_str(&t, u_a);
		t_u2(&t, 0); t_u2(&t, 0); t_str(&t, u_result);
		t_u2(&t, 0); t_u2(&t, 1); t_str(&t, u_b);
	} else {
		/* Physical Param rows: result, a, b. */
		t_u2(&t, 0); t_u2(&t, 0); t_str(&t, u_result);
		t_u2(&t, 0); t_u2(&t, 1); t_str(&t, u_a);
		t_u2(&t, 0); t_u2(&t, 1); t_str(&t, u_b);
	}
	/* AssemblyRef (35): four version u2, Flags u4, blob/str/str/blob. */
	t_u2(&t, 4); t_u2(&t, 0); t_u2(&t, 0); t_u2(&t, 0);
	t_u4(&t, 0); t_blob(&t, 0); t_str(&t, u_mscorlib);
	t_str(&t, 0); t_blob(&t, 0);
	/* NestedClass (41): Nested=Inner(3), Enclosing=Outer(2). */
	t_u2(&t, 3); t_u2(&t, 2);
	/* Unsupported table 42 with one dummy two-byte row. */
	if (o->unsupported_table)
		t_u2(&t, 0);

	/* Metadata root and stream directory. */
	w32(meta, 0x424a5342);
	w16(meta + 4, 1);
	w16(meta + 6, 1);
	w32(meta + 12, 12);
	memcpy(meta + 16, "v4.0.30319", 11);
	w16(meta + 30, 4); /* stream count */
	size_t h = 32, off = 128;
	const char *tables_name = o->ptr ? "#-" : "#~";
	w32(meta + h, (uint32_t)off);
	w32(meta + h + 4, (uint32_t)t.nt);
	strcpy((char *)meta + h + 8, tables_name);
	h += 8 + ((strlen(tables_name) + 1 + 3) & ~(size_t)3);
	memcpy(meta + off, tab, t.nt);
	off = (off + t.nt + 3) & ~(size_t)3;

	w32(meta + h, (uint32_t)off);
	w32(meta + h + 4, (uint32_t)strings.n);
	strcpy((char *)meta + h + 8, "#Strings");
	h += 8 + (((size_t)strlen("#Strings") + 1 + 3) & ~(size_t)3);
	memcpy(meta + off, sh, strings.n);
	off = (off + strings.n + 3) & ~(size_t)3;

	w32(meta + h, (uint32_t)off);
	w32(meta + h + 4, (uint32_t)bn);
	strcpy((char *)meta + h + 8, "#Blob");
	h += 8 + (((size_t)strlen("#Blob") + 1 + 3) & ~(size_t)3);
	memcpy(meta + off, bh, bn);
	off = (off + bn + 3) & ~(size_t)3;

	w32(meta + h, (uint32_t)off);
	w32(meta + h + 4, 16);
	strcpy((char *)meta + h + 8, "#GUID");
	h += 8 + (((size_t)strlen("#GUID") + 1 + 3) & ~(size_t)3);
	for (unsigned i = 0; i < 16; i++)
		meta[off + i] = (unsigned char)(i + 1);
	size_t nm = off + 16;

	/* PE headers. */
	w16(f, 0x5a4d);
	w32(f + 0x3c, 0x80);
	w32(f + 0x80, 0x4550);
	w16(f + 0x84, 0x14c);
	w16(f + 0x86, 3);
	w16(f + 0x94, 224);
	w16(f + 0x96, 0x2102);
	size_t o2 = 0x98;
	w16(f + o2, 0x10b);
	w32(f + o2 + 28, 0x400000);
	w32(f + o2 + 32, 0x1000);
	w32(f + o2 + 36, 0x200);
	w32(f + o2 + 56, 0x6000);
	w32(f + o2 + 60, 0x400);
	w16(f + o2 + 68, 3);
	w32(f + o2 + 92, 16);
	w32(f + o2 + 96 + 14 * 8, 0x2000);
	w32(f + o2 + 96 + 14 * 8 + 4, 72);

	/* Sections. */
	memcpy(f + 0x178, ".meta", 5);
	w32(f + 0x178 + 8, 0x800);
	w32(f + 0x178 + 12, 0x2000);
	w32(f + 0x178 + 16, 0x800);
	w32(f + 0x178 + 20, 0x400);
	w32(f + 0x178 + 36, 0x40000040);
	memcpy(f + 0x1a0, ".il1", 4);
	w32(f + 0x1a0 + 8, 0x1000);
	w32(f + 0x1a0 + 12, 0x4000);
	w32(f + 0x1a0 + 16, 0x1000);
	w32(f + 0x1a0 + 20, 0x1000);
	w32(f + 0x1a0 + 36, 0x60000020);
	memcpy(f + 0x1c8, ".il2", 4);
	w32(f + 0x1c8 + 8, 0x200);
	w32(f + 0x1c8 + 12, 0x5000);
	w32(f + 0x1c8 + 16, 0x200);
	w32(f + 0x1c8 + 20, 0x3000);
	w32(f + 0x1c8 + 36, 0x60000020);

	/* CLI header. */
	w32(f + 0x400, 72);
	w16(f + 0x404, 2);
	w16(f + 0x406, 5);
	w32(f + 0x408, 0x2100);
	w32(f + 0x40c, (uint32_t)nm);
	w32(f + 0x410, 1);
	memcpy(f + 0x500, meta, nm);

	/* Tiny body at RVA 0x4000 / raw 0x1000. */
	f[0x1000] = (unsigned char)(o->zero_il ? 0x02 : ((3 << 2) | 2));
	f[0x1001] = 2;
	f[0x1002] = 0;
	f[0x1003] = 0x2a;
	/* Fat body split over .il1 (8 bytes) and .il2 (10 bytes). */
	unsigned char fat[18] = {
		3, 0x30, 2, 0, 6, 0, 0, 0, 0, 0, 0, 0,
		2, 0x17, 0x58, 0, 0, 0x2a
	};
	memcpy(f + 0x1ff8, fat, 8);
	memcpy(f + 0x3000, fat + 8, 10);
	return out;
}

static pe_ctx_t *open_buf(unsigned char *buf, size_t n) {
	pe_ctx_t *ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL)
		return NULL;
	ctx->map_addr = buf;
	ctx->map_size = (off_t)n;
	ctx->map_end = (uintptr_t)buf + n;
	if (pe_parse(ctx) != LIBPE_E_OK) {
		free(ctx);
		return NULL;
	}
	return ctx;
}

static void close_buf(pe_ctx_t *ctx) {
	free(ctx->pe.sections);
	free(ctx->pe.directories);
	free(ctx);
}

static const pe_clr_method_t *find_method(const pe_clr_methods_t *idx,
	uint32_t token) {
	for (size_t i = 0; i < idx->method_count; i++) {
		if (idx->methods[i].method_token == token)
			return &idx->methods[i];
	}
	return NULL;
}

static int build_ok(const struct build_opts *o, struct built_image *img,
	pe_ctx_t **ctx, pe_clr_methods_t **idx) {
	*img = build_image(o);
	*ctx = open_buf(img->buf, img->size);
	if (*ctx == NULL)
		return 0;
	pe_clr_methods_error_t err;
	pe_clr_methods_err_e rc = pe_clr_methods_build(*ctx, idx, &err);
	return rc == PE_CLR_METHODS_E_OK;
}

/* ------------------------------------------------------------------ */
/* Ownership group                                                     */
/* ------------------------------------------------------------------ */

static int case_plain_layout(void) {
	struct build_opts o = {0};
	struct built_image img;
	pe_ctx_t *ctx;
	pe_clr_methods_t *idx = NULL;
	TASK_CHECK(build_ok(&o, &img, &ctx, &idx));
	TASK_CHECK(idx != NULL && idx->method_count == 3);

	/* Physical MethodDef order in #~: Stub, Load, Store. */
	TASK_CHECK(idx->methods[0].method_token == 0x06000001u);
	TASK_CHECK(strcmp(idx->methods[0].name, "Stub") == 0);
	TASK_CHECK(idx->methods[1].method_token == 0x06000002u);
	TASK_CHECK(strcmp(idx->methods[1].name, "Load") == 0);
	TASK_CHECK(idx->methods[2].method_token == 0x06000003u);
	TASK_CHECK(strcmp(idx->methods[2].name, "Store") == 0);

	const pe_clr_method_t *stub = find_method(idx, 0x06000001);
	const pe_clr_method_t *load = find_method(idx, 0x06000002);
	const pe_clr_method_t *store = find_method(idx, 0x06000003);
	TASK_CHECK(stub && load && store);

	TASK_CHECK(stub->typedef_token == 0x02000002u);
	TASK_CHECK(strcmp(stub->type_name, "Demo.Outer") == 0);
	TASK_CHECK(load->typedef_token == 0x02000003u);
	TASK_CHECK(strcmp(load->type_name, "Demo.Outer+Inner") == 0);
	TASK_CHECK(store->typedef_token == 0x02000003u);

	TASK_CHECK(stub->flags == 0x0446 && stub->impl_flags == 0);
	TASK_CHECK(load->flags == 0x16 && store->flags == 0x16);
	TASK_CHECK(stub->rva == 0 && load->rva == 0x4000 && store->rva == 0x4ff8);

	/* Signatures are payloads after the compressed prefix. */
	TASK_CHECK(stub->signature_size == 3);
	TASK_CHECK(stub->signature[0] == 0x20
		&& stub->signature[1] == 0x00 && stub->signature[2] == 0x08);
	TASK_CHECK(load->signature_size == 4 && load->signature[0] == 0x00
		&& load->signature[1] == 0x01 && load->signature[2] == 0x08
		&& load->signature[3] == 0x08);

	/* Sequence 0 return metadata is retained ahead of the arguments. */
	TASK_CHECK(load->param_count == 2);
	TASK_CHECK(load->params[0].token == 0x08000001u);
	TASK_CHECK(load->params[0].sequence == 0);
	TASK_CHECK(strcmp(load->params[0].name, "result") == 0);
	TASK_CHECK(load->params[1].token == 0x08000002u);
	TASK_CHECK(load->params[1].sequence == 1);
	TASK_CHECK(strcmp(load->params[1].name, "a") == 0);
	TASK_CHECK(store->param_count == 1);
	TASK_CHECK(store->params[0].token == 0x08000003u);
	TASK_CHECK(store->params[0].sequence == 1);
	TASK_CHECK(strcmp(store->params[0].name, "b") == 0);
	TASK_CHECK(stub->param_count == 0);

	pe_clr_methods_free(idx);
	close_buf(ctx);
	free(img.buf);
	return 0;
}

static int case_ptr_layout(void) {
	struct build_opts o = { .ptr = 1 };
	struct built_image img;
	pe_ctx_t *ctx;
	pe_clr_methods_t *idx = NULL;
	TASK_CHECK(build_ok(&o, &img, &ctx, &idx));
	TASK_CHECK(idx->method_count == 3);

	/*
	 * Physical rows are Load, Store, Stub, but MethodPtr reorders the
	 * TypeDef logical lists: Outer owns Stub, Inner owns Load and Store.
	 */
	const pe_clr_method_t *load = find_method(idx, 0x06000001);
	const pe_clr_method_t *store = find_method(idx, 0x06000002);
	const pe_clr_method_t *stub = find_method(idx, 0x06000003);
	TASK_CHECK(load && store && stub);
	TASK_CHECK(strcmp(load->name, "Load") == 0);
	TASK_CHECK(load->typedef_token == 0x02000003u);
	TASK_CHECK(store->typedef_token == 0x02000003u);
	TASK_CHECK(stub->typedef_token == 0x02000002u);
	TASK_CHECK(strcmp(stub->type_name, "Demo.Outer") == 0);
	TASK_CHECK(strcmp(load->type_name, "Demo.Outer+Inner") == 0);

	/*
	 * ParamList stays sliced by physical MethodDef rows. Physical Param
	 * rows are a(1), result(2), b(3); ParamPtr restores logical order,
	 * and emitted tokens always use the physical Param RID.
	 */
	TASK_CHECK(load->param_count == 2);
	TASK_CHECK(load->params[0].token == 0x08000002u);
	TASK_CHECK(load->params[0].sequence == 0);
	TASK_CHECK(strcmp(load->params[0].name, "result") == 0);
	TASK_CHECK(load->params[1].token == 0x08000001u);
	TASK_CHECK(load->params[1].sequence == 1);
	TASK_CHECK(strcmp(load->params[1].name, "a") == 0);
	TASK_CHECK(store->param_count == 1);
	TASK_CHECK(store->params[0].token == 0x08000003u);
	TASK_CHECK(strcmp(store->params[0].name, "b") == 0);
	TASK_CHECK(stub->param_count == 0);

	pe_clr_methods_free(idx);
	close_buf(ctx);
	free(img.buf);
	return 0;
}

static int case_wide_heaps(void) {
	struct build_opts o = { .wide = 1 };
	struct built_image img;
	pe_ctx_t *ctx;
	pe_clr_methods_t *idx = NULL;
	TASK_CHECK(build_ok(&o, &img, &ctx, &idx));
	TASK_CHECK(idx->method_count == 3);

	/* Same identities as #~ narrow-heap layout, reached via u4 heaps. */
	const pe_clr_method_t *stub = find_method(idx, 0x06000001);
	const pe_clr_method_t *load = find_method(idx, 0x06000002);
	const pe_clr_method_t *store = find_method(idx, 0x06000003);
	TASK_CHECK(stub && load && store);
	TASK_CHECK(strcmp(stub->name, "Stub") == 0);
	TASK_CHECK(strcmp(load->name, "Load") == 0);
	TASK_CHECK(strcmp(store->name, "Store") == 0);
	TASK_CHECK(strcmp(load->type_name, "Demo.Outer+Inner") == 0);
	TASK_CHECK(load->param_count == 2);
	TASK_CHECK(strcmp(load->params[0].name, "result") == 0);
	TASK_CHECK(load->params[0].sequence == 0);
	TASK_CHECK(strcmp(load->params[1].name, "a") == 0);
	TASK_CHECK(load->signature_size == 4);
	TASK_CHECK(stub->signature_size == 3);
	/* Rows parsed with wide columns still carry valid file spans. */
	TASK_CHECK(load->method_row.size > 0 && load->method_row.file_offset > 0);
	TASK_CHECK(load->params[0].row.size > 0);

	pe_clr_methods_free(idx);
	close_buf(ctx);
	free(img.buf);
	return 0;
}

static int case_nesting_and_rows(void) {
	struct build_opts o = { .ptr = 1, .wide = 1 };
	struct built_image img;
	pe_ctx_t *ctx;
	pe_clr_methods_t *idx = NULL;
	TASK_CHECK(build_ok(&o, &img, &ctx, &idx));

	const pe_clr_method_t *load = find_method(idx, 0x06000001);
	TASK_CHECK(load != NULL);
	/* '+' joins inner names; namespace only qualifies the outermost type. */
	TASK_CHECK(strcmp(load->type_name, "Demo.Outer+Inner") == 0);
	const pe_clr_method_t *stub = find_method(idx, 0x06000003);
	TASK_CHECK(stub != NULL && strcmp(stub->type_name, "Demo.Outer") == 0);

	/* Row spans identify physical source positions in both layouts. */
	TASK_CHECK(load->method_row.file_offset != stub->method_row.file_offset);
	TASK_CHECK(load->method_row.size > 0);
	/* Inner and Outer are distinct TypeDef rows at distinct offsets. */
	TASK_CHECK(load->typedef_row.file_offset
		!= stub->typedef_row.file_offset);
	TASK_CHECK(load->params[0].row.file_offset > 0);
	TASK_CHECK(load->params[0].row.size > 0);

	pe_clr_methods_free(idx);
	close_buf(ctx);
	free(img.buf);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Bodies group                                                        */
/* ------------------------------------------------------------------ */

static int case_tiny_and_fat(void) {
	struct build_opts o = {0};
	struct built_image img;
	pe_ctx_t *ctx;
	pe_clr_methods_t *idx = NULL;
	TASK_CHECK(build_ok(&o, &img, &ctx, &idx));

	const pe_clr_method_t *stub = find_method(idx, 0x06000001);
	const pe_clr_method_t *load = find_method(idx, 0x06000002);
	const pe_clr_method_t *store = find_method(idx, 0x06000003);
	TASK_CHECK(stub && load && store);

	/* RVA zero keeps the record but carries no body. */
	TASK_CHECK(stub->body.present == 0);

	TASK_CHECK(load->body.present == 1 && load->body.fat == 0);
	TASK_CHECK(load->body.header_rva == 0x4000);
	TASK_CHECK(load->body.header_size == 1);
	TASK_CHECK(load->body.max_stack == 8);
	TASK_CHECK(load->body.il_rva == 0x4001);
	TASK_CHECK(load->body.il_size == 3);
	TASK_CHECK(load->body.il[0] == 0x02 && load->body.il[1] == 0x00
		&& load->body.il[2] == 0x2a);

	TASK_CHECK(store->body.present == 1 && store->body.fat == 1);
	TASK_CHECK(store->body.header_flags == 0x3003);
	TASK_CHECK(store->body.max_stack == 2);
	TASK_CHECK(store->body.local_var_sig_token == 0);
	/* Header length follows the actual DWORD count: 3 DWORDs = 12 bytes. */
	TASK_CHECK(store->body.header_size == 12);
	TASK_CHECK(store->body.il_rva == 0x5004);
	TASK_CHECK(store->body.il_size == 6);
	static const unsigned char want_il[6] = {
		0x02, 0x17, 0x58, 0x00, 0x00, 0x2a
	};
	TASK_CHECK(memcmp(store->body.il, want_il, 6) == 0);

	pe_clr_methods_free(idx);
	close_buf(ctx);
	free(img.buf);
	return 0;
}

static int case_cross_section_spans(void) {
	struct build_opts o = { .ptr = 1 };
	struct built_image img;
	pe_ctx_t *ctx;
	pe_clr_methods_t *idx = NULL;
	TASK_CHECK(build_ok(&o, &img, &ctx, &idx));

	/* In #- physical order Store is token 2. */
	const pe_clr_method_t *store = find_method(idx, 0x06000002);
	TASK_CHECK(store != NULL);
	const pe_clr_body_t *b = &store->body;

	/* Fat header: 8 bytes in .il1, 4 bytes in .il2 at non-adjacent raws. */
	TASK_CHECK(b->header_span_count == 2);
	TASK_CHECK(b->header_spans[0].file_offset == 0x1ff8);
	TASK_CHECK(b->header_spans[0].rva == 0x4ff8);
	TASK_CHECK(b->header_spans[0].size == 8);
	TASK_CHECK(b->header_spans[0].section_index == 1);
	TASK_CHECK(b->header_spans[1].file_offset == 0x3000);
	TASK_CHECK(b->header_spans[1].rva == 0x5000);
	TASK_CHECK(b->header_spans[1].size == 4);
	TASK_CHECK(b->header_spans[1].section_index == 2);

	/* IL follows entirely in .il2, half-open and excluding trailing data. */
	TASK_CHECK(b->il_span_count == 1);
	TASK_CHECK(b->il_spans[0].file_offset == 0x3004);
	TASK_CHECK(b->il_spans[0].rva == 0x5004);
	TASK_CHECK(b->il_spans[0].size == 6);
	TASK_CHECK(b->il_spans[0].section_index == 2);

	/* Tiny body spans stay inside .il1, header separate from IL. */
	const pe_clr_method_t *load = find_method(idx, 0x06000001);
	TASK_CHECK(load != NULL);
	TASK_CHECK(load->body.header_span_count == 1);
	TASK_CHECK(load->body.header_spans[0].file_offset == 0x1000);
	TASK_CHECK(load->body.il_spans[0].file_offset == 0x1001);
	TASK_CHECK(load->body.il_spans[0].size == 3);

	pe_clr_methods_free(idx);
	close_buf(ctx);
	free(img.buf);
	return 0;
}

static int case_query_boundaries(void) {
	struct build_opts o = {0};
	struct built_image img;
	pe_ctx_t *ctx;
	pe_clr_methods_t *idx = NULL;
	TASK_CHECK(build_ok(&o, &img, &ctx, &idx));

	pe_clr_query_hit_t hits[8];
	/* Tiny IL [0x4001, 0x4004): first, inner, last bytes. */
	TASK_CHECK(pe_clr_methods_query(idx, 0x4001, hits, 8) == 1);
	TASK_CHECK(hits[0].method_token == 0x06000002u && hits[0].il_offset == 0);
	TASK_CHECK(pe_clr_methods_query(idx, 0x4002, hits, 8) == 1);
	TASK_CHECK(hits[0].il_offset == 1);
	TASK_CHECK(pe_clr_methods_query(idx, 0x4003, hits, 8) == 1);
	TASK_CHECK(hits[0].il_offset == 2);
	/* Exclusive end, header RVA and method gaps never match. */
	TASK_CHECK(pe_clr_methods_query(idx, 0x4004, hits, 8) == 0);
	TASK_CHECK(pe_clr_methods_query(idx, 0x4000, hits, 8) == 0);
	TASK_CHECK(pe_clr_methods_query(idx, 0x4fff, hits, 8) == 0);

	/* Fat IL [0x5004, 0x500a). */
	TASK_CHECK(pe_clr_methods_query(idx, 0x5004, hits, 8) == 1);
	TASK_CHECK(hits[0].method_token == 0x06000003u);
	TASK_CHECK(pe_clr_methods_query(idx, 0x5009, hits, 8) == 1);
	TASK_CHECK(hits[0].il_offset == 5);
	TASK_CHECK(pe_clr_methods_query(idx, 0x500a, hits, 8) == 0);
	/* NULL buffer is a count-only call. */
	TASK_CHECK(pe_clr_methods_query(idx, 0x5004, NULL, 0) == 1);

	pe_clr_methods_free(idx);
	close_buf(ctx);
	free(img.buf);
	return 0;
}

static int case_shared_and_empty_bodies(void) {
	struct build_opts shared = { .shared_rva = 1 };
	struct built_image simg;
	pe_ctx_t *sctx;
	pe_clr_methods_t *sidx = NULL;
	TASK_CHECK(build_ok(&shared, &simg, &sctx, &sidx));

	/* Two methods share one body: the RVA hits both, in physical order. */
	pe_clr_query_hit_t hits[8];
	size_t n = pe_clr_methods_query(sidx, 0x4001, hits, 8);
	TASK_CHECK(n == 2);
	TASK_CHECK(hits[0].method_token == 0x06000002u);
	TASK_CHECK(hits[1].method_token == 0x06000003u);
	TASK_CHECK(hits[0].il_offset == 0 && hits[1].il_offset == 0);

	pe_clr_methods_free(sidx);
	close_buf(sctx);
	free(simg.buf);

	struct build_opts zero = { .zero_il = 1 };
	struct built_image zimg;
	pe_ctx_t *zctx;
	pe_clr_methods_t *zidx = NULL;
	TASK_CHECK(build_ok(&zero, &zimg, &zctx, &zidx));
	const pe_clr_method_t *load = find_method(zidx, 0x06000002);
	TASK_CHECK(load != NULL);
	TASK_CHECK(load->body.present == 1 && load->body.il_size == 0);
	/* Zero CodeSize never matches, and its header RVA does not either. */
	TASK_CHECK(pe_clr_methods_query(zidx, 0x4000, NULL, 0) == 0);
	TASK_CHECK(pe_clr_methods_query(zidx, 0x4001, NULL, 0) == 0);

	pe_clr_methods_free(zidx);
	close_buf(zctx);
	free(zimg.buf);
	return 0;
}

static int case_lifetime_after_close(void) {
	struct build_opts o = { .ptr = 1, .wide = 1 };
	struct built_image img;
	pe_ctx_t *ctx;
	pe_clr_methods_t *idx = NULL;
	TASK_CHECK(build_ok(&o, &img, &ctx, &idx));

	/* Release the mapping and every parse-time array. */
	close_buf(ctx);
	ctx = NULL;

	const pe_clr_method_t *load = find_method(idx, 0x06000001);
	TASK_CHECK(load != NULL);
	TASK_CHECK(strcmp(load->name, "Load") == 0);
	TASK_CHECK(strcmp(load->type_name, "Demo.Outer+Inner") == 0);
	TASK_CHECK(load->params[0].sequence == 0);
	TASK_CHECK(strcmp(load->params[0].name, "result") == 0);
	TASK_CHECK(load->body.il_size == 3);
	TASK_CHECK(load->body.il[2] == 0x2a);
	TASK_CHECK(load->body.il_spans[0].file_offset == 0x1001);
	pe_clr_query_hit_t hits[4];
	TASK_CHECK(pe_clr_methods_query(idx, 0x4003, hits, 4) == 1);
	TASK_CHECK(hits[0].il_offset == 2);

	pe_clr_methods_free(idx);
	free(img.buf);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Integrity group                                                     */
/* ------------------------------------------------------------------ */

static int case_not_managed(void) {
	struct build_opts o = {0};
	struct built_image img = build_image(&o);
	pe_ctx_t *ctx = open_buf(img.buf, img.size);
	TASK_CHECK(ctx != NULL);
	/* Remove the CLR directory: the PE is valid but no longer managed. */
	ctx->pe.directories[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR]->VirtualAddress = 0;

	pe_clr_methods_t *idx = NULL;
	pe_clr_methods_error_t err;
	pe_clr_methods_err_e rc = pe_clr_methods_build(ctx, &idx, &err);
	TASK_CHECK(rc == PE_CLR_METHODS_E_OK);
	TASK_CHECK(idx != NULL && idx->method_count == 0);
	TASK_CHECK(pe_clr_methods_query(idx, 0x4001, NULL, 0) == 0);
	pe_clr_methods_free(idx);
	close_buf(ctx);
	free(img.buf);
	return 0;
}

static int case_bad_metadata_root(void) {
	struct build_opts o = {0};
	struct built_image img = build_image(&o);
	/* Metadata root lives at raw 0x500. */
	img.buf[0x500] = 'X';
	pe_ctx_t *ctx = open_buf(img.buf, img.size);
	TASK_CHECK(ctx != NULL);

	pe_clr_methods_t *idx = NULL;
	pe_clr_methods_error_t err;
	pe_clr_methods_err_e rc = pe_clr_methods_build(ctx, &idx, &err);
	TASK_CHECK(rc == PE_CLR_METHODS_E_MALFORMED);
	TASK_CHECK(idx == NULL);
	TASK_CHECK(err.location == 0x500);
	TASK_CHECK(err.message != NULL);
	close_buf(ctx);
	free(img.buf);
	return 0;
}

static int case_unsupported_table(void) {
	struct build_opts o = { .unsupported_table = 1 };
	struct built_image img;
	pe_ctx_t *ctx;
	pe_clr_methods_t *idx = NULL;
	pe_clr_methods_error_t err;
	img = build_image(&o);
	ctx = open_buf(img.buf, img.size);
	TASK_CHECK(ctx != NULL);
	pe_clr_methods_err_e rc = pe_clr_methods_build(ctx, &idx, &err);
	TASK_CHECK(rc == PE_CLR_METHODS_E_UNSUPPORTED);
	TASK_CHECK(idx == NULL);
	TASK_CHECK(err.location != UINT64_MAX);
	close_buf(ctx);
	free(img.buf);
	return 0;
}

static int case_bad_permutation(void) {
	struct build_opts o = { .ptr = 1, .bad_methodptr = 1 };
	struct built_image img;
	pe_ctx_t *ctx;
	pe_clr_methods_t *idx = NULL;
	pe_clr_methods_error_t err;
	img = build_image(&o);
	ctx = open_buf(img.buf, img.size);
	TASK_CHECK(ctx != NULL);
	pe_clr_methods_err_e rc = pe_clr_methods_build(ctx, &idx, &err);
	TASK_CHECK(rc == PE_CLR_METHODS_E_MALFORMED);
	TASK_CHECK(idx == NULL);
	TASK_CHECK(err.location != UINT64_MAX);
	close_buf(ctx);
	free(img.buf);
	return 0;
}

static int case_native_body(void) {
	struct build_opts o = {0};
	struct built_image img = build_image(&o);
	/* CorILMethod type bits 0 mark a native body, which is unsupported. */
	img.buf[0x1000] = 0x00;
	pe_ctx_t *ctx = open_buf(img.buf, img.size);
	TASK_CHECK(ctx != NULL);

	pe_clr_methods_t *idx = NULL;
	pe_clr_methods_error_t err;
	pe_clr_methods_err_e rc = pe_clr_methods_build(ctx, &idx, &err);
	TASK_CHECK(rc == PE_CLR_METHODS_E_UNSUPPORTED);
	TASK_CHECK(idx == NULL);
	TASK_CHECK(err.location == 0x4000);
	close_buf(ctx);
	free(img.buf);
	return 0;
}

static int case_unbacked_rva(void) {
	struct build_opts o = { .hole_rva = 1 };
	struct built_image img;
	pe_ctx_t *ctx;
	pe_clr_methods_t *idx = NULL;
	pe_clr_methods_error_t err;
	img = build_image(&o);
	ctx = open_buf(img.buf, img.size);
	TASK_CHECK(ctx != NULL);
	pe_clr_methods_err_e rc = pe_clr_methods_build(ctx, &idx, &err);
	TASK_CHECK(rc == PE_CLR_METHODS_E_MALFORMED);
	TASK_CHECK(idx == NULL);
	TASK_CHECK(err.location == 0x9000);
	close_buf(ctx);
	free(img.buf);
	return 0;
}

static int case_input_preserved(void) {
	struct build_opts good = { .ptr = 1, .wide = 1 };
	struct built_image img = build_image(&good);
	unsigned char *snapshot = malloc(img.size);
	TASK_CHECK(snapshot != NULL);
	memcpy(snapshot, img.buf, img.size);

	pe_ctx_t *ctx = open_buf(img.buf, img.size);
	TASK_CHECK(ctx != NULL);
	pe_clr_methods_t *idx = NULL;
	TASK_CHECK(pe_clr_methods_build(ctx, &idx, NULL) == PE_CLR_METHODS_E_OK);
	TASK_CHECK(memcmp(snapshot, img.buf, img.size) == 0);
	pe_clr_methods_free(idx);
	close_buf(ctx);

	/* A failing build must not mutate the input either. */
	struct built_image bad = build_image(
		&(struct build_opts){ .ptr = 1, .bad_methodptr = 1 });
	unsigned char *bad_snap = malloc(bad.size);
	TASK_CHECK(bad_snap != NULL);
	memcpy(bad_snap, bad.buf, bad.size);
	pe_ctx_t *bctx = open_buf(bad.buf, bad.size);
	TASK_CHECK(bctx != NULL);
	pe_clr_methods_t *bidx = NULL;
	TASK_CHECK(pe_clr_methods_build(bctx, &bidx, NULL)
		== PE_CLR_METHODS_E_MALFORMED);
	TASK_CHECK(bidx == NULL);
	TASK_CHECK(memcmp(bad_snap, bad.buf, bad.size) == 0);
	close_buf(bctx);

	free(bad_snap);
	free(bad.buf);
	free(snapshot);
	free(img.buf);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
	{ "ownership", "plain_layout",        case_plain_layout },
	{ "ownership", "ptr_layout",          case_ptr_layout },
	{ "ownership", "wide_heaps",          case_wide_heaps },
	{ "ownership", "nesting_and_rows",    case_nesting_and_rows },
	{ "bodies",    "tiny_and_fat",        case_tiny_and_fat },
	{ "bodies",    "cross_section_spans", case_cross_section_spans },
	{ "bodies",    "query_boundaries",    case_query_boundaries },
	{ "bodies",    "shared_and_empty",    case_shared_and_empty_bodies },
	{ "bodies",    "lifetime_after_close", case_lifetime_after_close },
	{ "integrity", "not_managed",         case_not_managed },
	{ "integrity", "bad_metadata_root",   case_bad_metadata_root },
	{ "integrity", "unsupported_table",   case_unsupported_table },
	{ "integrity", "bad_permutation",     case_bad_permutation },
	{ "integrity", "native_body",         case_native_body },
	{ "integrity", "unbacked_rva",        case_unbacked_rva },
	{ "integrity", "input_preserved",     case_input_preserved },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
