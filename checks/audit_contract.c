/*
** Contract test for tlsf_audit(), the read-only bounded heap audit.
**
** A healthy two-pool heap is built with real tlsf_malloc/tlsf_memalign/
** tlsf_realloc/tlsf_free calls inside private mappings bracketed by
** PROT_NONE guard pages. Metadata are then corrupted one item at a time
** and restored after each case. Every audit call is bracketed by a full
** byte comparison of all input ranges (control + both pools), proving
** the audit is read-only also on error paths. Wild pointers used in the
** cases point into the guard pages, so if the audit ever dereferenced a
** pointer before validating it, the process would die instead of
** returning a result; building with -fsanitize=address,undefined (see
** checks/target.sh) makes the same hold for out-of-range and misaligned
** accesses in general.
**
** The test mirrors the TLSF control/block layout and bucket mapping so
** it can aim corruptions at exact fields; it verifies the mirror against
** the live heap before relying on it.
*/

#define _GNU_SOURCE
#include "tlsf.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---- mirrored layout constants (must match tlsf.c) ---- */

#if defined(__LP64__) || defined(_WIN64)
#define T_64 1
#else
#define T_64 0
#endif

#define T_ALIGN_LOG2 (T_64 ? 3 : 2)
#define T_ALIGN (1 << T_ALIGN_LOG2)
#define T_SL_LOG2 5
#define T_SL_COUNT (1 << T_SL_LOG2)
#define T_FL_SHIFT (T_SL_LOG2 + T_ALIGN_LOG2)
#define T_FL_MAX (T_64 ? 32 : 30)
#define T_FL_COUNT (T_FL_MAX - T_FL_SHIFT + 1)
#define T_SMALL (1 << T_FL_SHIFT)

typedef struct tblock tblock;
struct tblock
{
	tblock* prev_phys;
	size_t size;
	tblock* next_free;
	tblock* prev_free;
};

typedef struct tcontrol
{
	tblock null_block;
	unsigned int fl_bitmap;
	unsigned int sl_bitmap[T_FL_COUNT];
	tblock* blocks[T_FL_COUNT][T_SL_COUNT];
} tcontrol;

static int t_fls(size_t x)
{
	int bit = -1;
	while (x)
	{
		++bit;
		x >>= 1;
	}
	return bit;
}

/* Mirror of mapping_insert() in tlsf.c. */
static void t_mapping(size_t size, int* fl, int* sl)
{
	if (size < T_SMALL)
	{
		*fl = 0;
		*sl = (int)(size / (T_SMALL / T_SL_COUNT));
	}
	else
	{
		const int f = t_fls(size);
		*sl = (int)((size >> (f - T_SL_LOG2)) ^ (1 << T_SL_LOG2));
		*fl = f - (T_FL_SHIFT - 1);
	}
}

/* Mirror of the pool layout computation in tlsf_add_pool(). */
static size_t t_pool_bytes(size_t raw)
{
	return (raw - 2 * sizeof(size_t)) & ~(size_t)(T_ALIGN - 1);
}

/* ---- guarded mappings ---- */

typedef struct
{
	unsigned char* base;	/* first usable page */
	size_t len;		/* usable bytes, a page multiple */
	size_t total;		/* mapping length including both guard pages */
	size_t page;
} region;

static int region_map(region* r, size_t len)
{
	const long page = sysconf(_SC_PAGESIZE);
	size_t pages, total;
	unsigned char* m;
	if (page <= 0)
	{
		return -1;
	}
	pages = (len + (size_t)page - 1) / (size_t)page;
	total = (pages + 2) * (size_t)page;
	m = mmap(0, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED)
	{
		return -1;
	}
	if (mprotect(m, (size_t)page, PROT_NONE) != 0
		|| mprotect(m + total - (size_t)page, (size_t)page, PROT_NONE) != 0)
	{
		munmap(m, total);
		return -1;
	}
	r->base = m + page;
	r->len = pages * (size_t)page;
	r->total = total;
	r->page = (size_t)page;
	return 0;
}

static void region_unmap(region* r)
{
	munmap(r->base - r->page, r->total);
}

/* ---- fixture ---- */

#define MAX_BLOCKS 512
#define POOL0_LEN 16384
#define POOL1_LEN 65536

typedef struct
{
	size_t off;	/* offset of the block's size field from the pool start */
	size_t size;	/* block size, flags excluded */
	int used;
} blk;

typedef struct
{
	region control_region;
	region pool_region[2];
	tlsf_t tlsf;
	tcontrol* tc;		/* control structure, mirrored type */
	unsigned char* pool[2];
	size_t pool_len[2];
	tlsf_audit_pool_t desc[2];
	blk blocks[2][MAX_BLOCKS];
	int nblocks[2];
	unsigned char* orig_control;	/* pristine image */
	unsigned char* orig_pool[2];
	unsigned char* snap_control;	/* per-call snapshot */
	unsigned char* snap_pool[2];
	void* ws;
	size_t ws_size;
} fixture;

static int checks_run;
static int failures;

static void note_fail(const char* name, const char* what, size_t got, size_t want)
{
	printf("FAIL %s: %s = 0x%zx, want 0x%zx\n", name, what, got, want);
	++failures;
}

/* ---- field access helpers (memcpy: no aliasing/alignment traps) ---- */

static size_t rd(const void* base, size_t off)
{
	size_t v;
	memcpy(&v, (const unsigned char*)base + off, sizeof v);
	return v;
}

static void wr(void* base, size_t off, size_t v)
{
	memcpy((unsigned char*)base + off, &v, sizeof v);
}

static void wrp(void* base, size_t off, const void* p)
{
	memcpy((unsigned char*)base + off, &p, sizeof p);
}

/* ---- snapshot helpers ---- */

static void snap_save(const fixture* f, unsigned char* c, unsigned char** p)
{
	int i;
	memcpy(c, f->tc, tlsf_size());
	for (i = 0; i < 2; ++i)
	{
		memcpy(p[i], f->pool[i], f->pool_len[i]);
	}
}

static int snap_same(const fixture* f, const unsigned char* c, unsigned char** p)
{
	int i;
	if (memcmp(c, f->tc, tlsf_size()) != 0)
	{
		return 0;
	}
	for (i = 0; i < 2; ++i)
	{
		if (memcmp(p[i], f->pool[i], f->pool_len[i]) != 0)
		{
			return 0;
		}
	}
	return 1;
}

/* ---- block enumeration and address math ---- */

typedef struct
{
	fixture* f;
	int p;
} walkctx;

static void walk_cb(void* ptr, size_t size, int used, void* user)
{
	walkctx* w = (walkctx*)user;
	fixture* f = w->f;
	blk* b;
	if (f->nblocks[w->p] >= MAX_BLOCKS)
	{
		return;
	}
	b = &f->blocks[w->p][f->nblocks[w->p]++];
	/* The size field sits sizeof(size_t) before the user pointer. */
	b->off = (size_t)((unsigned char*)ptr - f->pool[w->p]) - sizeof(size_t);
	b->size = size;
	b->used = used;
}

static void enumerate(fixture* f)
{
	int p;
	for (p = 0; p < 2; ++p)
	{
		walkctx w;
		w.f = f;
		w.p = p;
		f->nblocks[p] = 0;
		tlsf_walk_pool(f->pool[p], walk_cb, &w);
	}
}

static int pool_of(fixture* f, const void* h)
{
	const size_t a = (size_t)h;
	int p;
	for (p = 0; p < 2; ++p)
	{
		const size_t lo = (size_t)f->pool[p];
		if (a + sizeof(size_t) >= lo && a < lo + f->pool_len[p])
		{
			return p;
		}
	}
	return -1;
}

/* Offset of a block's size field within its pool, given its header. */
static size_t off_of(fixture* f, int p, const tblock* h)
{
	return (size_t)((const unsigned char*)h - f->pool[p]) + offsetof(tblock, size);
}

static size_t head_off(int fl, int sl)
{
	return offsetof(tcontrol, blocks)
		+ ((size_t)fl * T_SL_COUNT + (size_t)sl) * sizeof(tblock*);
}

/* Collect the headers on one bucket's list (healthy heap: safe to walk). */
static int list_nodes(fixture* f, int fl, int sl, tblock** out, int max)
{
	tblock* n = f->tc->blocks[fl][sl];
	int k = 0;
	while (n != &f->tc->null_block && k < max)
	{
		out[k++] = n;
		n = n->next_free;
	}
	return k;
}

/* Verify the mirrored layout against the live heap: physical order,
** sentinel positions, bucket membership of every free block, and the
** library's own checkers. */
static int self_check(fixture* f)
{
	int p, i;
	for (p = 0; p < 2; ++p)
	{
		size_t expect_off = 0;
		for (i = 0; i < f->nblocks[p]; ++i)
		{
			blk* b = &f->blocks[p][i];
			if (b->off != expect_off)
			{
				return -1;
			}
			expect_off += b->size + sizeof(size_t);
			if (!b->used)
			{
				int fl, sl, found = 0;
				tblock* n;
				t_mapping(b->size, &fl, &sl);
				for (n = f->tc->blocks[fl][sl]; n != &f->tc->null_block; n = n->next_free)
				{
					if (pool_of(f, n) == p && off_of(f, p, n) == b->off)
					{
						found = 1;
						break;
					}
				}
				if (!found)
				{
					return -1;
				}
			}
		}
		if (expect_off != t_pool_bytes(f->pool_len[p]) + sizeof(size_t))
		{
			return -1;
		}
	}
	if (tlsf_check(f->tlsf) != 0
		|| tlsf_check_pool(f->pool[0]) != 0
		|| tlsf_check_pool(f->pool[1]) != 0)
	{
		return -1;
	}
	return 0;
}

/* ---- audit call driver: snapshots inputs, calls twice, compares ---- */

static void expect(fixture* f, const void* control, size_t control_size,
	const tlsf_audit_pool_t* descs, size_t ndesc, void* ws, size_t ws_size,
	int want_status, int want_code, size_t want_pool, size_t want_off,
	const char* name)
{
	tlsf_audit_info_t i1, i2;
	int s1, s2;
	++checks_run;
	memset(&i1, 0, sizeof i1);
	memset(&i2, 0, sizeof i2);
	snap_save(f, f->snap_control, f->snap_pool);
	s1 = tlsf_audit(control, control_size, descs, ndesc, ws, ws_size, &i1);
	s2 = tlsf_audit(control, control_size, descs, ndesc, ws, ws_size, &i2);
	if (!snap_same(f, f->snap_control, f->snap_pool))
	{
		printf("FAIL %s: audit modified its inputs\n", name);
		++failures;
	}
	if (s1 != want_status)
	{
		note_fail(name, "status", (size_t)s1, (size_t)want_status);
	}
	if (s1 != s2)
	{
		note_fail(name, "repeat status", (size_t)s2, (size_t)s1);
	}
	if (s1 == TLSF_AUDIT_CORRUPT)
	{
		if (i1.code != want_code)
		{
			note_fail(name, "code", (size_t)i1.code, (size_t)want_code);
		}
		if (i1.pool != want_pool)
		{
			note_fail(name, "pool", i1.pool, want_pool);
		}
		if (i1.offset != want_off)
		{
			note_fail(name, "offset", i1.offset, want_off);
		}
		if (i2.code != i1.code || i2.pool != i1.pool || i2.offset != i1.offset)
		{
			note_fail(name, "repeat info", 1, 0);
		}
	}
	if (s1 == TLSF_AUDIT_OK && i1.code != TLSF_AUDIT_CODE_NONE)
	{
		note_fail(name, "ok code", (size_t)i1.code, TLSF_AUDIT_CODE_NONE);
	}
}

static void expect_ok(fixture* f, const char* name)
{
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_OK, 0, 0, 0, name);
}

static void restore(fixture* f)
{
	memcpy(f->tc, f->orig_control, tlsf_size());
	memcpy(f->pool[0], f->orig_pool[0], f->pool_len[0]);
	memcpy(f->pool[1], f->orig_pool[1], f->pool_len[1]);
	if (self_check(f) != 0)
	{
		printf("FAIL: restore did not bring the heap back\n");
		++failures;
	}
}

/* ---- fixture construction: real malloc/memalign/realloc/free ---- */

static int build_fixture(fixture* f)
{
	void* a[8];
	void* h;
	void* j;
	void* k;
	void* l;
	void* l2;
	void* m;
	int i;

	memset(f, 0, sizeof *f);
	if (region_map(&f->control_region, tlsf_size()) != 0
		|| region_map(&f->pool_region[0], POOL0_LEN) != 0
		|| region_map(&f->pool_region[1], POOL1_LEN) != 0)
	{
		return -1;
	}
	/* Put the control structure at the end of its mapping so that a
	** read past tlsf_size() lands in the guard page. */
	f->tc = (tcontrol*)(f->control_region.base + f->control_region.len - tlsf_size());
	f->pool[0] = f->pool_region[0].base;
	f->pool[1] = f->pool_region[1].base;
	f->pool_len[0] = f->pool_region[0].len;
	f->pool_len[1] = f->pool_region[1].len;

	f->tlsf = tlsf_create(f->tc);
	if (!f->tlsf
		|| !tlsf_add_pool(f->tlsf, f->pool[0], f->pool_len[0])
		|| !tlsf_add_pool(f->tlsf, f->pool[1], f->pool_len[1]))
	{
		return -1;
	}

	/* Fragment pool 0: eight neighbours, three non-adjacent frees give a
	** three-node list in one bucket; a larger free block and the pool
	** remainder give two more buckets. */
	for (i = 0; i < 8; ++i)
	{
		a[i] = tlsf_malloc(f->tlsf, 128);
	}
	tlsf_free(f->tlsf, a[1]);
	tlsf_free(f->tlsf, a[3]);
	tlsf_free(f->tlsf, a[5]);
	h = tlsf_malloc(f->tlsf, 2000);
	j = tlsf_memalign(f->tlsf, 64, 90);
	k = tlsf_malloc(f->tlsf, 300);
	m = tlsf_malloc(f->tlsf, 500);
	/* Force pool 1 usage and free one large block there. */
	l = tlsf_malloc(f->tlsf, 20000);
	l2 = tlsf_malloc(f->tlsf, 20000);
	k = tlsf_realloc(f->tlsf, k, 1200);
	tlsf_free(f->tlsf, h);
	tlsf_free(f->tlsf, l);
	if (!a[0] || !h || !j || !k || !l2 || !m)
	{
		return -1;
	}

	f->desc[0].start = f->pool[0];
	f->desc[0].size = f->pool_len[0];
	f->desc[1].start = f->pool[1];
	f->desc[1].size = f->pool_len[1];

	f->ws_size = tlsf_audit_workspace_size(f->desc, 2);
	f->ws = malloc(f->ws_size);
	f->orig_control = malloc(tlsf_size());
	f->snap_control = malloc(tlsf_size());
	for (i = 0; i < 2; ++i)
	{
		f->orig_pool[i] = malloc(f->pool_len[i]);
		f->snap_pool[i] = malloc(f->pool_len[i]);
	}
	if (!f->ws || !f->orig_control || !f->snap_control
		|| !f->orig_pool[0] || !f->orig_pool[1]
		|| !f->snap_pool[0] || !f->snap_pool[1])
	{
		return -1;
	}

	enumerate(f);
	if (self_check(f) != 0)
	{
		return -1;
	}
	snap_save(f, f->orig_control, f->orig_pool);
	return 0;
}

static void destroy_fixture(fixture* f)
{
	int i;
	free(f->ws);
	free(f->orig_control);
	free(f->snap_control);
	for (i = 0; i < 2; ++i)
	{
		free(f->orig_pool[i]);
		free(f->snap_pool[i]);
		region_unmap(&f->pool_region[i]);
	}
	region_unmap(&f->control_region);
}

/* ---- case groups ---- */

static void run_param_cases(fixture* f)
{
	tlsf_audit_pool_t bad[2];
	size_t need = f->ws_size;

	bad[0] = f->desc[0];
	bad[1] = f->desc[1];

	expect(f, 0, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_BAD_PARAM, 0, 0, 0, "param-null-control");
	expect(f, (const char*)f->tc + 2, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_BAD_PARAM, 0, 0, 0, "param-misaligned-control");
	expect(f, f->tc, tlsf_size() - sizeof(size_t), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_BAD_PARAM, 0, 0, 0, "param-short-control");
	expect(f, f->tc, tlsf_size(), 0, 2, f->ws, f->ws_size,
		TLSF_AUDIT_BAD_PARAM, 0, 0, 0, "param-null-pools");

	bad[0].start = f->pool[0] + 2;
	expect(f, f->tc, tlsf_size(), bad, 2, f->ws, f->ws_size,
		TLSF_AUDIT_BAD_PARAM, 0, 0, 0, "param-misaligned-pool");
	bad[0] = f->desc[0];
	bad[0].size = tlsf_pool_overhead();
	expect(f, f->tc, tlsf_size(), bad, 2, f->ws, f->ws_size,
		TLSF_AUDIT_BAD_PARAM, 0, 0, 0, "param-tiny-pool");
	bad[0] = f->desc[0];
	bad[0].size = tlsf_block_size_max() + tlsf_pool_overhead() + T_ALIGN;
	expect(f, f->tc, tlsf_size(), bad, 2, f->ws, f->ws_size,
		TLSF_AUDIT_BAD_PARAM, 0, 0, 0, "param-huge-pool");
	bad[0] = f->desc[0];

	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, need - 1,
		TLSF_AUDIT_NO_SPACE, 0, 0, 0, "no-space");
	expect(f, f->tc, tlsf_size(), f->desc, 2, 0, 0,
		TLSF_AUDIT_NO_SPACE, 0, 0, 0, "no-space-null");
}

static void run_ok_cases(fixture* f)
{
	void* wsb = malloc(f->ws_size + sizeof(size_t));
	if (!wsb)
	{
		printf("FAIL: out of memory\n");
		++failures;
		return;
	}
	expect_ok(f, "ok-fragmented");
	/* The workspace size includes alignment slack, so an unaligned
	** workspace of the advertised size must still be accepted. */
	expect(f, f->tc, tlsf_size(), f->desc, 2, (char*)wsb + 1, f->ws_size,
		TLSF_AUDIT_OK, 0, 0, 0, "ok-misaligned-workspace");
	free(wsb);
}

static void run_corrupt_cases(fixture* f)
{
	const size_t z0 = f->pool_len[0];
	const size_t z1 = f->pool_len[1];
	const size_t pool0_bytes = t_pool_bytes(z0);
	const size_t sent1_off = t_pool_bytes(z1) + sizeof(size_t);

	/* Victims located on the pristine heap. */
	blk* first0 = &f->blocks[0][0];		/* first block of pool 0 */
	blk* mid0 = 0;				/* used block, free predecessor */
	blk* any0 = 0;				/* any used middle block */
	int ne_fl[8], ne_sl[8], n_ne = 0;	/* non-empty buckets, index order */
	int li_fl = -1;				/* a bucket with >= 2 nodes */
	tblock* li_nodes[8];
	int li_n = 0;
	int fl, sl, i;

	for (i = 1; i < f->nblocks[0]; ++i)
	{
		if (f->blocks[0][i].used && !f->blocks[0][i - 1].used && !mid0)
		{
			mid0 = &f->blocks[0][i];
		}
		if (f->blocks[0][i].used && !any0)
		{
			any0 = &f->blocks[0][i];
		}
	}
	for (fl = 0; fl < T_FL_COUNT; ++fl)
	{
		for (sl = 0; sl < T_SL_COUNT; ++sl)
		{
			if (f->tc->sl_bitmap[fl] & (1u << sl))
			{
				int n;
				if (n_ne < 8)
				{
					ne_fl[n_ne] = fl;
					ne_sl[n_ne] = sl;
					++n_ne;
				}
				n = list_nodes(f, fl, sl, li_nodes, 8);
				if (n >= 2 && li_fl < 0)
				{
					li_fl = fl;
					li_n = n;
				}
			}
		}
	}
	if (!mid0 || !any0 || n_ne < 2 || li_n < 2)
	{
		printf("FAIL: fixture does not have the needed structure\n");
		++failures;
		return;
	}

	/* -- physical coverage -- */

	/* First block claims a free predecessor; the audit must not read
	** prev_phys_block in front of the pool (front guard page). */
	wr(f->pool[0], first0->off, rd(f->pool[0], first0->off) | 2u);
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_CORRUPT, TLSF_AUDIT_PREV_FREE, 0, first0->off, "prev-free-set");
	restore(f);
	expect_ok(f, "ok-after-prev-free-set");

	/* Block with a free predecessor loses its prev-free flag. */
	wr(f->pool[0], mid0->off, rd(f->pool[0], mid0->off) & ~(size_t)2u);
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_CORRUPT, TLSF_AUDIT_PREV_FREE, 0, mid0->off, "prev-free-clear");
	restore(f);
	expect_ok(f, "ok-after-prev-free-clear");

	/* Back pointer of a block whose predecessor is free. */
	wrp(f->pool[0], mid0->off - sizeof(size_t), f->pool[0] + mid0->off);
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_CORRUPT, TLSF_AUDIT_PREV_PHYS, 0, mid0->off - sizeof(size_t),
		"prev-phys");
	restore(f);
	expect_ok(f, "ok-after-prev-phys");

	/* Used block after a free block turns free: not coalesced. */
	wr(f->pool[0], mid0->off, rd(f->pool[0], mid0->off) | 1u);
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_CORRUPT, TLSF_AUDIT_UNMERGED, 0, mid0->off, "unmerged");
	restore(f);
	expect_ok(f, "ok-after-unmerged");

	/* Size wraparound: huge size must be rejected before any offset
	** arithmetic on it. */
	wr(f->pool[0], any0->off,
		(rd(f->pool[0], any0->off) & 3u) | ~(size_t)7u);
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_CORRUPT, TLSF_AUDIT_SIZE, 0, any0->off, "size-huge");
	restore(f);
	expect_ok(f, "ok-after-size-huge");

	/* First block overruns the pool: the next header would land exactly
	** on the guard page behind the mapping. */
	wr(f->pool[0], first0->off,
		(rd(f->pool[0], first0->off) & 3u) | (pool0_bytes + sizeof(size_t)));
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_CORRUPT, TLSF_AUDIT_SIZE, 0, first0->off, "size-overrun");
	restore(f);
	expect_ok(f, "ok-after-size-overrun");

	/* Below the minimum block size. */
	wr(f->pool[0], any0->off, (rd(f->pool[0], any0->off) & 3u) | sizeof(size_t));
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_CORRUPT, TLSF_AUDIT_SIZE, 0, any0->off, "size-small");
	restore(f);
	expect_ok(f, "ok-after-size-small");

#if T_64
	/* Multiple of 4 but not of 8: misaligned on 64-bit. */
	wr(f->pool[0], any0->off, (rd(f->pool[0], any0->off) & 3u) | (size_t)44u);
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_CORRUPT, TLSF_AUDIT_SIZE, 0, any0->off, "size-misaligned");
	restore(f);
	expect_ok(f, "ok-after-size-misaligned");
#endif

	/* Zero size in the middle of the pool: misplaced sentinel. */
	wr(f->pool[0], any0->off, rd(f->pool[0], any0->off) & 3u);
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_CORRUPT, TLSF_AUDIT_SENTINEL, 0, any0->off, "sentinel-mid");
	restore(f);
	expect_ok(f, "ok-after-sentinel-mid");

	/* The real sentinel of pool 1 marked free. */
	wr(f->pool[1], sent1_off, rd(f->pool[1], sent1_off) | 1u);
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_CORRUPT, TLSF_AUDIT_SENTINEL, 1, sent1_off, "sentinel-free");
	restore(f);
	expect_ok(f, "ok-after-sentinel-free");

	/* Declared pool range shorter than the real layout: the scan must
	** stop at the declared end instead of reading past it. */
	{
		tlsf_audit_pool_t shorty[2];
		size_t short_bytes, want_off;
		shorty[0] = f->desc[0];
		shorty[1] = f->desc[1];
		shorty[0].size = z0 - 4096;
		short_bytes = t_pool_bytes(shorty[0].size);
		want_off = 0;
		for (i = 0; i < f->nblocks[0]; ++i)
		{
			blk* b = &f->blocks[0][i];
			if (b->off > short_bytes || b->size > short_bytes - b->off)
			{
				want_off = b->off;
				break;
			}
		}
		expect(f, f->tc, tlsf_size(), shorty, 2, f->ws, f->ws_size,
			TLSF_AUDIT_CORRUPT, TLSF_AUDIT_SIZE, 0, want_off, "short-range");
		/* no heap bytes were changed; nothing to restore */
		expect_ok(f, "ok-after-short-range");
	}

	/* -- bitmaps -- */

	/* First-level bitmap bit outside the index range. */
	f->tc->fl_bitmap |= 1u << (T_FL_COUNT + 1);
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_CORRUPT, TLSF_AUDIT_BITMAP, TLSF_AUDIT_CONTROL,
		offsetof(tcontrol, fl_bitmap), "bitmap-fl-high");
	restore(f);
	expect_ok(f, "ok-after-bitmap-fl-high");

	/* First-level bit clear while the second-level map is non-zero. */
	f->tc->fl_bitmap &= ~(1u << ne_fl[0]);
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_CORRUPT, TLSF_AUDIT_BITMAP, TLSF_AUDIT_CONTROL,
		offsetof(tcontrol, sl_bitmap) + (size_t)ne_fl[0] * sizeof(unsigned int),
		"bitmap-fl-clear");
	restore(f);
	expect_ok(f, "ok-after-bitmap-fl-clear");

	/* Non-empty bucket whose head points at the null block. */
	f->tc->blocks[ne_fl[0]][ne_sl[0]] = &f->tc->null_block;
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_CORRUPT, TLSF_AUDIT_BITMAP, TLSF_AUDIT_CONTROL,
		head_off(ne_fl[0], ne_sl[0]), "bitmap-head-null");
	restore(f);
	expect_ok(f, "ok-after-bitmap-head-null");

	/* Empty bucket with a non-null head. */
	{
		int esl = 0;
		tblock* h;
		while (f->tc->sl_bitmap[ne_fl[0]] & (1u << esl))
		{
			++esl;
		}
		h = f->tc->blocks[ne_fl[0]][ne_sl[0]];
		f->tc->blocks[ne_fl[0]][esl] = h;
		expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
			TLSF_AUDIT_CORRUPT, TLSF_AUDIT_BITMAP, TLSF_AUDIT_CONTROL,
			head_off(ne_fl[0], esl), "bitmap-head-set");
		restore(f);
		expect_ok(f, "ok-after-bitmap-head-set");
	}

	/* -- list targets -- */

	/* Head pointing past the pool mapping (into the guard page). */
	f->tc->blocks[ne_fl[0]][ne_sl[0]] = (tblock*)(f->pool[1] + z1 + 64);
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_CORRUPT, TLSF_AUDIT_LIST_TARGET, TLSF_AUDIT_CONTROL,
		head_off(ne_fl[0], ne_sl[0]), "target-wild-head");
	restore(f);
	expect_ok(f, "ok-after-target-wild-head");

	/* Unaligned head. */
	f->tc->blocks[ne_fl[0]][ne_sl[0]] = (tblock*)(f->pool[0] + 4);
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_CORRUPT, TLSF_AUDIT_LIST_TARGET, TLSF_AUDIT_CONTROL,
		head_off(ne_fl[0], ne_sl[0]), "target-unaligned-head");
	restore(f);
	expect_ok(f, "ok-after-target-unaligned-head");

	/* Head pointing into the control range: an aligned address there
	** still is not the null block. */
	f->tc->blocks[ne_fl[0]][ne_sl[0]] = (tblock*)((char*)f->tc + 64);
	expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
		TLSF_AUDIT_CORRUPT, TLSF_AUDIT_LIST_TARGET, TLSF_AUDIT_CONTROL,
		head_off(ne_fl[0], ne_sl[0]), "target-control-head");
	restore(f);
	expect_ok(f, "ok-after-target-control-head");

	/* Head pointing at a used block. */
	{
		tblock* u = (tblock*)(f->pool[0] + any0->off - offsetof(tblock, size));
		f->tc->blocks[ne_fl[0]][ne_sl[0]] = u;
		expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
			TLSF_AUDIT_CORRUPT, TLSF_AUDIT_LIST_TARGET, TLSF_AUDIT_CONTROL,
			head_off(ne_fl[0], ne_sl[0]), "target-used-head");
		restore(f);
		expect_ok(f, "ok-after-target-used-head");
	}

	/* A list node's next_free pointing into the guard page. */
	{
		tblock* h = f->tc->blocks[ne_fl[0]][ne_sl[0]];
		int hp = pool_of(f, h);
		size_t ho = off_of(f, hp, h);
		wrp(f->pool[hp], ho + offsetof(tblock, next_free) - offsetof(tblock, size),
			f->pool[0] + z0 + 32);
		expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
			TLSF_AUDIT_CORRUPT, TLSF_AUDIT_LIST_TARGET, (size_t)hp,
			ho + offsetof(tblock, next_free) - offsetof(tblock, size),
			"target-wild-next");
		restore(f);
		expect_ok(f, "ok-after-target-wild-next");
	}

	/* -- list linkage, membership, duplicates -- */

	/* prev_free of the second node: wild value must be compared, never
	** dereferenced (it points into the guard page). */
	{
		tblock* n = li_nodes[1];
		int np = pool_of(f, n);
		size_t no = off_of(f, np, n);
		wrp(f->pool[np], no + offsetof(tblock, prev_free) - offsetof(tblock, size),
			f->pool[0] + z0 + 16);
		expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
			TLSF_AUDIT_CORRUPT, TLSF_AUDIT_LIST_LINK, (size_t)np,
			no + offsetof(tblock, prev_free) - offsetof(tblock, size),
			"link-prev");
		restore(f);
		expect_ok(f, "ok-after-link-prev");
	}

	/* Swap the heads of two non-empty buckets: the lower bucket now
	** starts with a block whose size belongs to the other bucket. */
	{
		tblock* hx = f->tc->blocks[ne_fl[0]][ne_sl[0]];
		tblock* hy = f->tc->blocks[ne_fl[1]][ne_sl[1]];
		int hp = pool_of(f, hy);
		size_t ho = off_of(f, hp, hy);
		f->tc->blocks[ne_fl[0]][ne_sl[0]] = hy;
		f->tc->blocks[ne_fl[1]][ne_sl[1]] = hx;
		expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
			TLSF_AUDIT_CORRUPT, TLSF_AUDIT_LIST_BUCKET, (size_t)hp, ho,
			"bucket-swap");
		restore(f);
		expect_ok(f, "ok-after-bucket-swap");
	}

	/* Self-loop: a node's next_free is itself. */
	{
		tblock* h = f->tc->blocks[ne_fl[0]][ne_sl[0]];
		int hp = pool_of(f, h);
		size_t ho = off_of(f, hp, h);
		wrp(f->pool[hp], ho + offsetof(tblock, next_free) - offsetof(tblock, size), h);
		expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
			TLSF_AUDIT_CORRUPT, TLSF_AUDIT_LIST_DUP, (size_t)hp, ho, "dup-self");
		restore(f);
		expect_ok(f, "ok-after-dup-self");
	}

	/* One block heads two different buckets. */
	{
		tblock* hx = f->tc->blocks[ne_fl[0]][ne_sl[0]];
		int hp = pool_of(f, hx);
		size_t ho = off_of(f, hp, hx);
		f->tc->blocks[ne_fl[1]][ne_sl[1]] = hx;
		expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
			TLSF_AUDIT_CORRUPT, TLSF_AUDIT_LIST_DUP, (size_t)hp, ho, "dup-cross");
		restore(f);
		expect_ok(f, "ok-after-dup-cross");
	}

	/* Splice a middle node out of its list: physically free but no
	** longer listed. */
	{
		tblock* n = li_nodes[1];
		int np = pool_of(f, n);
		size_t no = off_of(f, np, n);
		tblock* prev = n->prev_free;
		tblock* next = n->next_free;
		prev->next_free = next;
		if (next != &f->tc->null_block)
		{
			next->prev_free = prev;
		}
		expect(f, f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size,
			TLSF_AUDIT_CORRUPT, TLSF_AUDIT_LIST_MISSING, (size_t)np, no,
			"missing");
		restore(f);
		expect_ok(f, "ok-after-missing");
	}

	/* info may be NULL. */
	wr(f->pool[0], first0->off, rd(f->pool[0], first0->off) | 2u);
	{
		int s = tlsf_audit(f->tc, tlsf_size(), f->desc, 2, f->ws, f->ws_size, 0);
		if (s != TLSF_AUDIT_CORRUPT)
		{
			note_fail("info-null", "status", (size_t)s, TLSF_AUDIT_CORRUPT);
		}
		++checks_run;
	}
	restore(f);
	expect_ok(f, "ok-after-info-null");
}

/* Empty heap (fresh pool, no allocations) and a heap with no pools at
** all must both pass. */
static void run_empty_cases(void)
{
	region rc, rp;
	unsigned char* control;
	tlsf_t t;
	tlsf_audit_pool_t d;
	tlsf_audit_info_t info;
	unsigned char* before_c;
	unsigned char* before_p;
	size_t need;
	void* ws;
	int s;

	if (region_map(&rc, tlsf_size()) != 0 || region_map(&rp, 8192) != 0)
	{
		printf("FAIL: cannot map empty-heap regions\n");
		++failures;
		return;
	}
	control = rc.base + rc.len - tlsf_size();
	t = tlsf_create(control);
	if (!t)
	{
		printf("FAIL: cannot build empty heap\n");
		++failures;
		region_unmap(&rc);
		region_unmap(&rp);
		return;
	}
	d.start = rp.base;
	d.size = rp.len;
	need = tlsf_audit_workspace_size(&d, 1);
	ws = malloc(need ? need : 1);
	before_c = malloc(tlsf_size());
	before_p = malloc(rp.len);
	if (!ws || !before_c || !before_p)
	{
		printf("FAIL: out of memory\n");
		++failures;
	}
	else
	{
		/* No pools at all: no workspace needed. */
		++checks_run;
		memcpy(before_c, control, tlsf_size());
		s = tlsf_audit(control, tlsf_size(), 0, 0, 0, 0, &info);
		if (s != TLSF_AUDIT_OK)
		{
			note_fail("ok-no-pools", "status", (size_t)s, TLSF_AUDIT_OK);
		}
		if (memcmp(before_c, control, tlsf_size()) != 0)
		{
			printf("FAIL ok-no-pools: audit modified its inputs\n");
			++failures;
		}

		/* A fresh pool with no allocations yet. */
		if (!tlsf_add_pool(t, rp.base, rp.len))
		{
			printf("FAIL: cannot add empty pool\n");
			++failures;
		}
		++checks_run;
		memcpy(before_c, control, tlsf_size());
		memcpy(before_p, rp.base, rp.len);
		s = tlsf_audit(control, tlsf_size(), &d, 1, ws, need, &info);
		if (s != TLSF_AUDIT_OK)
		{
			note_fail("ok-empty", "status", (size_t)s, TLSF_AUDIT_OK);
		}
		if (memcmp(before_c, control, tlsf_size()) != 0
			|| memcmp(before_p, rp.base, rp.len) != 0)
		{
			printf("FAIL ok-empty: audit modified its inputs\n");
			++failures;
		}
	}
	free(ws);
	free(before_c);
	free(before_p);
	region_unmap(&rc);
	region_unmap(&rp);
}

int main(void)
{
	fixture fx;

	if (sizeof(tcontrol) != tlsf_size()
		|| sizeof(tblock) - sizeof(tblock*) != tlsf_block_size_min()
		|| T_ALIGN != tlsf_align_size()
		|| 2 * sizeof(size_t) != tlsf_pool_overhead())
	{
		printf("FAIL: mirrored layout does not match the library\n");
		return 2;
	}

	if (build_fixture(&fx) != 0)
	{
		printf("FAIL: fixture construction failed\n");
		return 2;
	}

	run_ok_cases(&fx);
	run_param_cases(&fx);
	run_corrupt_cases(&fx);
	run_empty_cases();

	destroy_fixture(&fx);

	if (failures)
	{
		printf("audit-contract: %d of %d checks FAILED\n", failures, checks_run);
		return 1;
	}
	printf("audit-contract: all %d checks passed\n", checks_run);
	return 0;
}
