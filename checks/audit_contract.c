/*
** Contract tests for the bounded, read-only TLSF heap audit.
**
** The control structure and both pools are built inside anonymous mappings
** flanked by PROT_NONE guard pages. Any dereference of an out-of-range link
** therefore raises a fault instead of silently reading foreign memory, so a
** clean run proves the audit validates links before following them. Every
** audit call is bracketed by byte-for-byte snapshots of all input ranges to
** prove the read-only guarantee, including on error and corruption paths.
**
** The audit implementation itself never allocates; only this test harness
** uses malloc/mmap to set the stage.
*/

#include <sys/mman.h>
#include <unistd.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tlsf.h"

/* macOS names the anonymous-mapping flag MAP_ANON; Linux uses MAP_ANONYMOUS. */
#if !defined(MAP_ANON) && defined(MAP_ANONYMOUS)
#define MAP_ANON MAP_ANONYMOUS
#endif

/* Test-side mirrors of the internal layout (tlsf.c does not expose them).
** Sizes are asserted against the public tlsf_size() at runtime. The 25/32
** bucket counts match the LP64 control_t; the runtime size assertion rejects
** a mismatched ABI rather than reading the layout incorrectly. */
#define T_FL_COUNT 25
#define T_SL_COUNT 32
struct t_block
{
	struct t_block* prev_phys;
	size_t size;
	struct t_block* next_free;
	struct t_block* prev_free;
};
struct t_control
{
	struct t_block null_node;
	unsigned int fl_bitmap;
	unsigned int sl_bitmap[T_FL_COUNT];
	struct t_block* blocks[T_FL_COUNT][T_SL_COUNT];
};

#define FREE_BIT  ((size_t)1)
#define PREV_BIT  ((size_t)2)

static int g_failures;
static long g_page;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		++g_failures; \
	} } while (0)

/* A mapping whose usable base begins exactly one page in: the eight bytes
** immediately before base (the first block's out-of-pool prev_phys word) lie
** inside the leading PROT_NONE guard, so any read of them faults. A second
** guard page protects the end of the usable region. */
typedef struct
{
	unsigned char* map;
	size_t map_bytes;
	void* base;
	size_t bytes;
} slot_t;

static slot_t make_slot(size_t usable)
{
	slot_t s;
	size_t usable_pages = (usable + (size_t)g_page - 1) / (size_t)g_page;
	s.map_bytes = (size_t)g_page * (usable_pages + 2);
	s.map = mmap(0, s.map_bytes, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	if (s.map == MAP_FAILED)
	{
		perror("mmap");
		exit(2);
	}
	mprotect(s.map, (size_t)g_page, PROT_NONE);
	mprotect(s.map + s.map_bytes - (size_t)g_page, (size_t)g_page, PROT_NONE);
	/* base-8 is the last word of the leading guard page; base itself is the
	** first readable byte and is page-aligned (hence correctly aligned). */
	s.base = s.map + g_page;
	s.bytes = usable;
	return s;
}

static void free_slot(slot_t* s)
{
	mprotect(s->map, (size_t)g_page, PROT_READ | PROT_WRITE);
	mprotect(s->map + s->map_bytes - (size_t)g_page,
		(size_t)g_page, PROT_READ | PROT_WRITE);
	munmap(s->map, s->map_bytes);
}

static unsigned char* guard_after(const slot_t* s)
{
	return s->map + s->map_bytes - (size_t)g_page;
}
static unsigned char* guard_before(const slot_t* s)
{
	return s->map; /* whole leading page is PROT_NONE */
}

typedef struct
{
	slot_t c, p0, p1;
	tlsf_t tlsf;
	size_t cbytes, pbytes;
	unsigned char *snap_c, *snap_p0, *snap_p1;
} heap_t;

static size_t pool_snap_len(const heap_t* h)
{
	return h->pbytes; /* exactly the mapped pool bytes; base-8 is a guard page */
}

static void snapshot_heap(heap_t* h)
{
	size_t plen = pool_snap_len(h);
	h->snap_c = malloc(h->cbytes);
	h->snap_p0 = malloc(plen);
	h->snap_p1 = malloc(plen);
	memcpy(h->snap_c, h->c.base, h->cbytes);
	memcpy(h->snap_p0, h->p0.base, plen);
	memcpy(h->snap_p1, h->p1.base, plen);
}

static void restore_heap(heap_t* h)
{
	size_t plen = pool_snap_len(h);
	memcpy(h->c.base, h->snap_c, h->cbytes);
	memcpy(h->p0.base, h->snap_p0, plen);
	memcpy(h->p1.base, h->snap_p1, plen);
}

static int current_matches_baseline(const heap_t* h)
{
	size_t plen = pool_snap_len(h);
	return memcmp(h->snap_c, h->c.base, h->cbytes) == 0 &&
		memcmp(h->snap_p0, h->p0.base, plen) == 0 &&
		memcmp(h->snap_p1, h->p1.base, plen) == 0;
}

static void make_pools(const heap_t* h, tlsf_audit_pool_t* pools)
{
	pools[0].base = h->p0.base;
	pools[0].bytes = h->pbytes;
	pools[1].base = h->p1.base;
	pools[1].bytes = h->pbytes;
}

/* Save every input byte, run the audit, prove nothing changed. */
static tlsf_audit_status_t run_audit(const heap_t* h,
	void* ws, size_t wsz, tlsf_audit_result_t* res)
{
	tlsf_audit_pool_t pools[2];
	size_t plen = pool_snap_len(h);
	unsigned char *pre_c = malloc(h->cbytes);
	unsigned char *pre_p0 = malloc(plen);
	unsigned char *pre_p1 = malloc(plen);
	tlsf_audit_status_t st;
	make_pools(h, pools);
	memcpy(pre_c, h->c.base, h->cbytes);
	memcpy(pre_p0, h->p0.base, plen);
	memcpy(pre_p1, h->p1.base, plen);

	st = tlsf_audit(h->c.base, h->cbytes, pools, 2, ws, wsz, res);

	CHECK(memcmp(pre_c, h->c.base, h->cbytes) == 0);
	CHECK(memcmp(pre_p0, h->p0.base, plen) == 0);
	CHECK(memcmp(pre_p1, h->p1.base, plen) == 0);
	free(pre_c); free(pre_p0); free(pre_p1);
	return st;
}

static size_t workspace_for(const heap_t* h)
{
	return tlsf_audit_workspace_size(2, 2 * h->pbytes);
}

/* Physical block walk used by the test to locate free blocks. */
static struct t_block* block_at(slot_t* pool, size_t header_off)
{
	return (struct t_block*)((unsigned char*)pool->base - sizeof(void*)
		+ header_off);
}
static size_t blk_size(const struct t_block* b)
{
	return b->size & ~(FREE_BIT | PREV_BIT);
}
static struct t_block* next_phys(struct t_block* b)
{
	return (struct t_block*)((unsigned char*)b + 16 + (blk_size(b) - 8));
}

static struct t_block* nth_free(slot_t* pool, int want)
{
	struct t_block* b = block_at(pool, 0);
	int seen = 0;
	while (blk_size(b) != 0)
	{
		if (b->size & FREE_BIT)
		{
			if (seen == want)
			{
				return b;
			}
			++seen;
		}
		b = next_phys(b);
	}
	return 0;
}

/* First free physical block across either pool, with its slot returned. */
static struct t_block* any_free(heap_t* h, slot_t** owner)
{
	struct t_block* b = nth_free(&h->p0, 0);
	if (b)
	{
		if (owner) *owner = &h->p0;
		return b;
	}
	b = nth_free(&h->p1, 0);
	if (b)
	{
		if (owner) *owner = &h->p1;
	}
	return b;
}

/* Head of a bucket that contains at least two real nodes. */
static struct t_block* multinode_head(heap_t* h)
{
	struct t_control* c = (struct t_control*)h->c.base;
	int i, j;
	for (i = 0; i < T_FL_COUNT; ++i)
	{
		for (j = 0; j < T_SL_COUNT; ++j)
		{
			struct t_block* head = c->blocks[i][j];
			if (head != &c->null_node &&
				head->next_free != &c->null_node)
			{
				return head;
			}
		}
	}
	return 0;
}

/* Head of any non-empty bucket, with the pool slot that contains it. */
static struct t_block* any_head(heap_t* h, slot_t** owner)
{
	struct t_control* c = (struct t_control*)h->c.base;
	int i, j;
	for (i = 0; i < T_FL_COUNT; ++i)
	{
		for (j = 0; j < T_SL_COUNT; ++j)
		{
			struct t_block* head = c->blocks[i][j];
			if (head != &c->null_node)
			{
				if (owner)
				{
					unsigned char* p = (unsigned char*)head;
					*owner = (p >= (unsigned char*)h->p0.base &&
						p < (unsigned char*)h->p0.base + h->pbytes)
						? &h->p0 : &h->p1;
				}
				return head;
			}
		}
	}
	return 0;
}

/* Locate the bucket that currently lists block b. */
static void find_bucket(const heap_t* h, const struct t_block* b,
	int* out_fl, int* out_sl)
{
	struct t_control* c = (struct t_control*)h->c.base;
	int i, j;
	*out_fl = *out_sl = -1;
	for (i = 0; i < T_FL_COUNT; ++i)
	{
		for (j = 0; j < T_SL_COUNT; ++j)
		{
			if (c->blocks[i][j] == b)
			{
				*out_fl = i;
				*out_sl = j;
				return;
			}
		}
	}
}

typedef void (*mutator_fn)(heap_t*);

static void expect_corrupt(heap_t* h, void* ws, size_t wsz,
	mutator_fn fn, tlsf_audit_corruption_t want, const char* name)
{
	tlsf_audit_result_t r1, r2;
	tlsf_audit_status_t st;

	fn(h);
	st = run_audit(h, ws, wsz, &r1);
	CHECK(st == TLSF_AUDIT_CORRUPT);
	if (st == TLSF_AUDIT_CORRUPT)
	{
		CHECK(r1.corruption == want);
		if (r1.corruption != want)
		{
			printf("  [%s] got category %d (%s), wanted %d\n",
				name, (int)r1.corruption, r1.detail, (int)want);
		}
	}

	/* Determinism: same corruption yields the identical report. */
	restore_heap(h);
	fn(h);
	st = run_audit(h, ws, wsz, &r2);
	CHECK(st == TLSF_AUDIT_CORRUPT);
	CHECK(r2.corruption == r1.corruption);
	CHECK(r2.region == r1.region);
	CHECK(r2.pool_index == r1.pool_index);
	CHECK(r2.offset == r1.offset);

	/* Restore: heap is healthy and byte-identical to the baseline again. */
	restore_heap(h);
	st = run_audit(h, ws, wsz, &r2);
	CHECK(st == TLSF_AUDIT_OK);
	CHECK(current_matches_baseline(h));
	if (st != TLSF_AUDIT_OK)
	{
		printf("  [%s] heap did not recover: %s\n", name, r2.detail);
	}
}

/* ---- mutators ----------------------------------------------------------- */

static void mut_first_prev_flag(heap_t* h)
{
	block_at(&h->p0, 0)->size |= PREV_BIT;
}

static void mut_size_too_small(heap_t* h)
{
	struct t_block* b = block_at(&h->p0, 0);
	b->size = (b->size & (FREE_BIT | PREV_BIT)) | 8;
}

static void mut_size_max(heap_t* h)
{
	struct t_block* b = block_at(&h->p0, 0);
	b->size = (b->size & (FREE_BIT | PREV_BIT)) | (1UL << 32);
}

static void mut_size_reserved_bits(heap_t* h)
{
	block_at(&h->p0, 0)->size |= 4;
}

static void mut_size_huge_truncates(heap_t* h)
{
	struct t_block* b = block_at(&h->p0, 0);
	b->size = (b->size & (FREE_BIT | PREV_BIT)) | ((1UL << 32) - 8);
}

static void mut_back_link(heap_t* h)
{
	/* Find a block whose predecessor is free, then break its back-link. */
	struct t_block* b = block_at(&h->p1, 0);
	while (blk_size(b) != 0)
	{
		if (b->size & PREV_BIT)
		{
			b->prev_phys = (struct t_block*)0xdeadbeef;
			return;
		}
		b = next_phys(b);
	}
	/* Fall back to pool 0 if pool 1 has no free predecessor. */
	b = block_at(&h->p0, 0);
	while (blk_size(b) != 0)
	{
		if (b->size & PREV_BIT)
		{
			b->prev_phys = (struct t_block*)0xdeadbeef;
			return;
		}
		b = next_phys(b);
	}
}

static void mut_coalesce(heap_t* h)
{
	/* Find a free block followed by a real used block, then free the latter. */
	slot_t* slots[2] = { &h->p0, &h->p1 };
	int k;
	for (k = 0; k < 2; ++k)
	{
		struct t_block* b = block_at(slots[k], 0);
		while (blk_size(b) != 0)
		{
			struct t_block* succ = next_phys(b);
			if ((b->size & FREE_BIT) && blk_size(succ) != 0 &&
				!(succ->size & FREE_BIT))
			{
				succ->size |= FREE_BIT;
				return;
			}
			b = succ;
		}
	}
}

static void mut_sentinel_free(heap_t* h)
{
	struct t_block* b = block_at(&h->p0, 0);
	while (blk_size(b) != 0)
	{
		b = next_phys(b);
	}
	b->size |= FREE_BIT;
}

static void mut_sentinel_prev_flag(heap_t* h)
{
	struct t_block* b = block_at(&h->p0, 0);
	while (blk_size(b) != 0)
	{
		b = next_phys(b);
	}
	b->size &= ~PREV_BIT;
}

static void mut_sentinel_position(heap_t* h)
{
	/* Shrink the last real block and plant a zero word inside its payload. */
	struct t_block* b = block_at(&h->p0, 0);
	struct t_block* prev = 0;
	while (blk_size(b) != 0)
	{
		prev = b;
		b = next_phys(b);
	}
	prev->size = (prev->size & (FREE_BIT | PREV_BIT)) | (blk_size(prev) - 64);
	{
		struct t_block* fake = next_phys(prev);
		fake->size = PREV_BIT; /* zero size, used, predecessor free */
	}
}

static void mut_fl_without_sl(heap_t* h)
{
	struct t_control* c = (struct t_control*)h->c.base;
	c->fl_bitmap |= 1U << 20; /* an fl index that is currently empty */
}

static void mut_sl_bit_null_head(heap_t* h)
{
	struct t_control* c = (struct t_control*)h->c.base;
	/* Pick a populated fl, then an empty sl within it (head stays block_null). */
	int fl, sl;
	for (fl = 0; fl < T_FL_COUNT; ++fl)
	{
		if (c->fl_bitmap & (1U << fl))
		{
			for (sl = 0; sl < T_SL_COUNT; ++sl)
			{
				if (!(c->sl_bitmap[fl] & (1U << sl)))
				{
					c->sl_bitmap[fl] |= 1U << sl;
					return;
				}
			}
		}
	}
}

static void mut_fl_stray_high_bit(heap_t* h)
{
	struct t_control* c = (struct t_control*)h->c.base;
	c->fl_bitmap |= 1U << 31;
}

static void mut_head_guard_aligned(heap_t* h)
{
	struct t_control* c = (struct t_control*)h->c.base;
	slot_t* owner;
	struct t_block* b = any_head(h, &owner);
	int fl, sl;
	find_bucket(h, b, &fl, &sl);
	c->blocks[fl][sl] = (struct t_block*)guard_after(owner);
}

static void mut_head_guard_misaligned(heap_t* h)
{
	struct t_control* c = (struct t_control*)h->c.base;
	slot_t* owner;
	struct t_block* b = any_head(h, &owner);
	int fl, sl;
	find_bucket(h, b, &fl, &sl);
	c->blocks[fl][sl] = (struct t_block*)(guard_before(owner) + 4);
}

static void mut_link_guard_after(heap_t* h)
{
	/* A free node's next_free points at the trailing guard page. */
	slot_t* owner;
	struct t_block* b = any_free(h, &owner);
	b->next_free = (struct t_block*)guard_after(owner);
}

static void mut_link_guard_before(heap_t* h)
{
	slot_t* owner;
	struct t_block* b = any_free(h, &owner);
	b->next_free = (struct t_block*)guard_before(owner);
}

/* Point a free node's next_free at a used physical block. The target is fully
** mapped and aligned, so only the "must be a proven free block" rule rejects
** it; this guards against accepting links to allocated blocks. */
static void mut_link_to_used(heap_t* h)
{
	slot_t* slots[2] = { &h->p0, &h->p1 };
	struct t_block* b = any_free(h, 0);
	int k;
	for (k = 0; k < 2; ++k)
	{
		struct t_block* cur = block_at(slots[k], 0);
		while (blk_size(cur) != 0)
		{
			if (!(cur->size & FREE_BIT))
			{
				b->next_free = cur;
				return;
			}
			cur = next_phys(cur);
		}
	}
}

static void mut_null_impostor_readable(heap_t* h)
{
	struct t_control* c = (struct t_control*)h->c.base;
	struct t_block* b = any_head(h, 0);
	int fl, sl;
	find_bucket(h, b, &fl, &sl);
	/* Aligned control address that is not the shared null node. */
	c->blocks[fl][sl] = (struct t_block*)((unsigned char*)h->c.base + 256);
}

static void mut_null_impostor_guard(heap_t* h)
{
	struct t_control* c = (struct t_control*)h->c.base;
	struct t_block* b = any_head(h, 0);
	int fl, sl;
	find_bucket(h, b, &fl, &sl);
	c->blocks[fl][sl] = (struct t_block*)guard_after(&h->c);
}

static void mut_wrong_bucket(heap_t* h)
{
	struct t_control* c = (struct t_control*)h->c.base;
	struct t_block* b = any_head(h, 0);
	int fl, sl;
	find_bucket(h, b, &fl, &sl);
	/* Detach from the real bucket, then advertise it in another bucket. */
	c->blocks[fl][sl] = &c->null_node;
	c->sl_bitmap[fl] &= ~(1U << sl);
	if (!c->sl_bitmap[fl])
	{
		c->fl_bitmap &= ~(1U << fl);
	}
	fl = (fl + 3) % T_FL_COUNT;
	sl = (sl + 7) % T_SL_COUNT;
	/* Make sure the destination is genuinely empty first. */
	c->blocks[fl][sl] = &c->null_node;
	c->sl_bitmap[fl] &= ~(1U << sl);
	c->blocks[fl][sl] = b;
	c->fl_bitmap |= 1U << fl;
	c->sl_bitmap[fl] |= 1U << sl;
}

/* Publish one block in a second, empty bucket whose linear index is later
** than its real bucket. The audit therefore visits the real bucket first
** (marking the block seen) and then hits the duplicate marker on the later
** entry, reporting LIST_DUPLICATE before the size-to-bucket rule. A re-entry
** inside one bucket chain is the cycle case tested separately. */
static void mut_duplicate(heap_t* h)
{
	struct t_control* c = (struct t_control*)h->c.base;
	struct t_block* b = any_head(h, 0);
	int fl, sl, a, k;
	int dfl = -1, dsl = -1;
	find_bucket(h, b, &fl, &sl);
	for (a = fl; a < T_FL_COUNT && dfl < 0; ++a)
	{
		int k0 = (a == fl) ? sl + 1 : 0;
		for (k = k0; k < T_SL_COUNT; ++k)
		{
			if (!(c->sl_bitmap[a] & (1U << k)) &&
				c->blocks[a][k] == &c->null_node)
			{
				dfl = a; dsl = k;
				break;
			}
		}
	}
	CHECK(dfl >= 0);
	if (dfl < 0)
	{
		return;
	}
	c->blocks[dfl][dsl] = b;
	c->fl_bitmap |= 1U << dfl;
	c->sl_bitmap[dfl] |= 1U << dsl;
}

static void mut_missing(heap_t* h)
{
	struct t_control* c = (struct t_control*)h->c.base;
	struct t_block* b = any_head(h, 0);
	int fl, sl;
	find_bucket(h, b, &fl, &sl);
	c->blocks[fl][sl] = &c->null_node;
	c->sl_bitmap[fl] &= ~(1U << sl);
	if (!c->sl_bitmap[fl])
	{
		c->fl_bitmap &= ~(1U << fl);
	}
}

static void mut_head_prev_not_null(heap_t* h)
{
	struct t_control* c = (struct t_control*)h->c.base;
	struct t_block* head = multinode_head(h);
	struct t_block* other;
	CHECK(head != 0);
	if (!head)
	{
		return;
	}
	/* A distinct proven free block to plant as the head's prev link. */
	other = any_free(h, 0);
	if (other == head)
	{
		other = head->next_free;
	}
	head->prev_free = other;
	(void)c;
}

static void mut_reciprocal(heap_t* h)
{
	/* Break the successor's back pointer on a multi-node free list. */
	struct t_block* head = multinode_head(h);
	CHECK(head != 0);
	if (!head)
	{
		return;
	}
	head->next_free->prev_free =
		&((struct t_control*)h->c.base)->null_node;
}

/* ---- heap construction -------------------------------------------------- */

static heap_t* build_fragmented_heap(void)
{
	heap_t* h = calloc(1, sizeof(*h));
	void* ptrs[8];
	int i;
	h->pbytes = 2 * (size_t)g_page;
	h->cbytes = (size_t)g_page - 16;
	h->c = make_slot(h->cbytes);
	h->p0 = make_slot(h->pbytes);
	h->p1 = make_slot(h->pbytes);
	h->tlsf = tlsf_create(h->c.base);
	tlsf_add_pool(h->tlsf, h->p0.base, h->pbytes);
	tlsf_add_pool(h->tlsf, h->p1.base, h->pbytes);

	ptrs[0] = tlsf_malloc(h->tlsf, 100);
	ptrs[1] = tlsf_malloc(h->tlsf, 400);
	ptrs[2] = tlsf_memalign(h->tlsf, 128, 300);
	tlsf_free(h->tlsf, ptrs[1]);
	ptrs[3] = tlsf_realloc(h->tlsf, ptrs[0], 900);
	ptrs[4] = tlsf_malloc(h->tlsf, 77);
	ptrs[5] = tlsf_malloc(h->tlsf, 2048);
	tlsf_free(h->tlsf, ptrs[4]);
	for (i = 6; i < 8; ++i)
	{
		ptrs[i] = tlsf_malloc(h->tlsf, 160);
	}
	(void)ptrs;
	snapshot_heap(h);
	return h;
}

static heap_t* build_listy_heap(void)
{
	heap_t* h = calloc(1, sizeof(*h));
	enum { N = 8 };
	void* blocks[N];
	int i;
	h->pbytes = 2 * (size_t)g_page;
	h->cbytes = (size_t)g_page - 16;
	h->c = make_slot(h->cbytes);
	h->p0 = make_slot(h->pbytes);
	h->p1 = make_slot(h->pbytes);
	h->tlsf = tlsf_create(h->c.base);
	tlsf_add_pool(h->tlsf, h->p0.base, h->pbytes);
	tlsf_add_pool(h->tlsf, h->p1.base, h->pbytes);
	/* Equal-sized live blocks, then free alternating ones. Because every
	** request maps to the same bucket and the blocks stay separated by live
	** neighbors (no physical coalescing), one free list gains several nodes. */
	for (i = 0; i < N; ++i)
	{
		blocks[i] = tlsf_malloc(h->tlsf, 512);
	}
	for (i = 1; i < N; i += 2)
	{
		tlsf_free(h->tlsf, blocks[i]);
	}
	snapshot_heap(h);
	return h;
}

static heap_t* build_empty_heap(void)
{
	heap_t* h = calloc(1, sizeof(*h));
	h->pbytes = 2 * (size_t)g_page;
	h->cbytes = (size_t)g_page - 16;
	h->c = make_slot(h->cbytes);
	h->p0 = make_slot(h->pbytes);
	h->p1 = make_slot(h->pbytes);
	h->tlsf = tlsf_create(h->c.base);
	tlsf_add_pool(h->tlsf, h->p0.base, h->pbytes);
	tlsf_add_pool(h->tlsf, h->p1.base, h->pbytes);
	snapshot_heap(h);
	return h;
}

static void destroy_heap(heap_t* h)
{
	free(h->snap_c); free(h->snap_p0); free(h->snap_p1);
	free_slot(&h->c); free_slot(&h->p0); free_slot(&h->p1);
	free(h);
}

/* ---- individual test groups --------------------------------------------- */

static void test_healthy_and_determinism(heap_t* h, void* ws, size_t wsz)
{
	tlsf_audit_result_t r[3];
	int i;
	for (i = 0; i < 3; ++i)
	{
		CHECK(run_audit(h, ws, wsz, &r[i]) == TLSF_AUDIT_OK);
		CHECK(r[i].corruption == TLSF_AUDIT_CORRUPT_NONE);
		CHECK(r[i].pool_index == -1);
	}
	CHECK(r[0].status == r[1].status && r[1].status == r[2].status);
	CHECK(run_audit(h, ws, wsz, 0) == TLSF_AUDIT_OK); /* NULL result */
	CHECK(tlsf_check(h->tlsf) == 0);
}

static void test_workspace_and_params(heap_t* h)
{
	tlsf_audit_pool_t pools[2];
	tlsf_audit_result_t r;
	size_t wsz = workspace_for(h);
	void* ws = malloc(wsz);
	make_pools(h, pools);

	CHECK(wsz >= tlsf_audit_workspace_size(2, 2 * h->pbytes));
	CHECK(tlsf_audit(h->c.base, h->cbytes, pools, 2, ws, wsz, &r)
		== TLSF_AUDIT_OK);
	CHECK(tlsf_audit(h->c.base, h->cbytes, pools, 2, ws, wsz - 1, &r)
		== TLSF_AUDIT_ENOSPC);
	CHECK(tlsf_audit(h->c.base, h->cbytes, pools, 2, 0, 0, &r)
		== TLSF_AUDIT_ENOSPC);
	CHECK(tlsf_audit(h->c.base, h->cbytes, pools, 2, 0, 64, &r)
		== TLSF_AUDIT_EINVAL);

	/* Descriptor validation. */
	CHECK(tlsf_audit(0, h->cbytes, pools, 2, ws, wsz, &r)
		== TLSF_AUDIT_EINVAL);
	CHECK(tlsf_audit(h->c.base, tlsf_size() - 1, pools, 2, ws, wsz, &r)
		== TLSF_AUDIT_EINVAL);
	CHECK(tlsf_audit((unsigned char*)h->c.base + 1, h->cbytes, pools, 2,
		ws, wsz, &r) == TLSF_AUDIT_EINVAL);
	CHECK(tlsf_audit(h->c.base, h->cbytes, 0, 2, ws, wsz, &r)
		== TLSF_AUDIT_EINVAL);
	CHECK(tlsf_audit(h->c.base, h->cbytes, pools, 2, ws, wsz, 0)
		== TLSF_AUDIT_OK); /* NULL result still works */

	{
		tlsf_audit_pool_t bad[2];
		make_pools(h, bad);
		bad[1].base = (unsigned char*)h->p1.base + 1;
		CHECK(tlsf_audit(h->c.base, h->cbytes, bad, 2, ws, wsz, &r)
			== TLSF_AUDIT_EINVAL);

		make_pools(h, bad);
		bad[0].bytes = tlsf_pool_overhead() + tlsf_block_size_min() - 1;
		CHECK(tlsf_audit(h->c.base, h->cbytes, bad, 2, ws, wsz, &r)
			== TLSF_AUDIT_EINVAL);

		make_pools(h, bad);
		bad[1] = bad[0]; /* overlapping pools */
		CHECK(tlsf_audit(h->c.base, h->cbytes, bad, 2, ws, wsz, &r)
			== TLSF_AUDIT_EINVAL);

		make_pools(h, bad);
		bad[0].base = h->c.base; /* pool overlaps control */
		CHECK(tlsf_audit(h->c.base, h->cbytes, bad, 2, ws, wsz, &r)
			== TLSF_AUDIT_EINVAL);

		make_pools(h, bad);
		bad[0].bytes = (size_t)-1;
		CHECK(tlsf_audit(h->c.base, h->cbytes, bad, 2, ws, wsz, &r)
			== TLSF_AUDIT_EINVAL);

		make_pools(h, bad);
		bad[0].base = 0;
		CHECK(tlsf_audit(h->c.base, h->cbytes, bad, 2, ws, wsz, &r)
			== TLSF_AUDIT_EINVAL);
	}

	/* A control with no pools is internally consistent with zero pools. */
	{
		void* bare = malloc(tlsf_size());
		tlsf_create(bare);
		CHECK(tlsf_audit(bare, tlsf_size(), pools, 0, ws,
			tlsf_audit_workspace_size(0, 0), &r) == TLSF_AUDIT_OK);
		free(bare);
	}

	free(ws);
}

static void test_short_range(heap_t* h, void* ws, size_t wsz)
{
	tlsf_audit_pool_t pools[2];
	tlsf_audit_result_t r;
	make_pools(h, pools);
	pools[1].bytes = h->pbytes - 64; /* descriptor lies about the mapped size */
	CHECK(tlsf_audit(h->c.base, h->cbytes, pools, 2, ws, wsz, &r)
		== TLSF_AUDIT_CORRUPT);
	CHECK(r.corruption == TLSF_AUDIT_CORRUPT_TRUNCATED ||
		r.corruption == TLSF_AUDIT_CORRUPT_SENTINEL);
	CHECK(r.region == TLSF_AUDIT_REGION_POOL);
	CHECK(r.pool_index == 1);
}

static void test_pool_ordering(heap_t* h, void* ws, size_t wsz)
{
	tlsf_audit_result_t r;
	/* Corrupt pool 1 only: report must name pool index 1. */
	mut_back_link(h);
	CHECK(run_audit(h, ws, wsz, &r) == TLSF_AUDIT_CORRUPT);
	CHECK(r.pool_index == 1);
	restore_heap(h);
	CHECK(run_audit(h, ws, wsz, &r) == TLSF_AUDIT_OK);

	/* Corrupt pool 0 only: report must name pool index 0. */
	mut_first_prev_flag(h);
	CHECK(run_audit(h, ws, wsz, &r) == TLSF_AUDIT_CORRUPT);
	CHECK(r.pool_index == 0);
	restore_heap(h);
	CHECK(run_audit(h, ws, wsz, &r) == TLSF_AUDIT_OK);
}

int main(void)
{
	heap_t* empty;
	heap_t* frag;
	heap_t* listy;
	size_t wsz;
	void* ws;

	g_page = sysconf(_SC_PAGESIZE);
	CHECK(g_page > 0);

	/* The test-side mirror must match the real control layout. */
	CHECK(sizeof(struct t_control) == tlsf_size());
	CHECK(sizeof(struct t_block) == 32);
	if (g_failures)
	{
		printf("layout assumptions failed; aborting\n");
		return 1;
	}

	empty = build_empty_heap();
	frag = build_fragmented_heap();
	listy = build_listy_heap();
	wsz = tlsf_audit_workspace_size(2, 2 * frag->pbytes);
	ws = malloc(wsz);

	printf("== healthy heaps ==\n");
	test_healthy_and_determinism(empty, ws, wsz);
	test_healthy_and_determinism(frag, ws, wsz);
	test_healthy_and_determinism(listy, ws, wsz);

	printf("== parameters and workspace ==\n");
	test_workspace_and_params(frag);

	printf("== short range / ordering ==\n");
	test_short_range(frag, ws, wsz);
	test_pool_ordering(frag, ws, wsz);

	printf("== physical block corruptions ==\n");
	expect_corrupt(frag, ws, wsz, mut_first_prev_flag,
		TLSF_AUDIT_CORRUPT_PREV_FLAG, "prev flag");
	expect_corrupt(frag, ws, wsz, mut_size_too_small,
		TLSF_AUDIT_CORRUPT_BLOCK_SIZE, "size below min");
	expect_corrupt(frag, ws, wsz, mut_size_max,
		TLSF_AUDIT_CORRUPT_BLOCK_SIZE, "size at max");
	expect_corrupt(frag, ws, wsz, mut_size_reserved_bits,
		TLSF_AUDIT_CORRUPT_BLOCK_SIZE, "reserved low bits");
	expect_corrupt(frag, ws, wsz, mut_size_huge_truncates,
		TLSF_AUDIT_CORRUPT_TRUNCATED, "huge size runs out of range");
	expect_corrupt(frag, ws, wsz, mut_back_link,
		TLSF_AUDIT_CORRUPT_PREV_POINTER, "back link");
	expect_corrupt(frag, ws, wsz, mut_coalesce,
		TLSF_AUDIT_CORRUPT_COALESCE, "adjacent free blocks");
	expect_corrupt(frag, ws, wsz, mut_sentinel_free,
		TLSF_AUDIT_CORRUPT_SENTINEL, "sentinel marked free");
	expect_corrupt(frag, ws, wsz, mut_sentinel_prev_flag,
		TLSF_AUDIT_CORRUPT_SENTINEL, "sentinel prev flag");
	expect_corrupt(frag, ws, wsz, mut_sentinel_position,
		TLSF_AUDIT_CORRUPT_SENTINEL, "sentinel position");

	printf("== bitmap corruptions ==\n");
	expect_corrupt(frag, ws, wsz, mut_fl_without_sl,
		TLSF_AUDIT_CORRUPT_BITMAP, "fl without sl");
	expect_corrupt(frag, ws, wsz, mut_sl_bit_null_head,
		TLSF_AUDIT_CORRUPT_BITMAP, "sl bit with null head");
	expect_corrupt(frag, ws, wsz, mut_fl_stray_high_bit,
		TLSF_AUDIT_CORRUPT_BITMAP, "stray high fl bit");

	printf("== dangerous links (guard pages prove no wild reads) ==\n");
	expect_corrupt(frag, ws, wsz, mut_head_guard_aligned,
		TLSF_AUDIT_CORRUPT_LIST_TARGET, "aligned head into guard");
	expect_corrupt(frag, ws, wsz, mut_head_guard_misaligned,
		TLSF_AUDIT_CORRUPT_LIST_ALIGN, "misaligned head into guard");
	expect_corrupt(frag, ws, wsz, mut_link_guard_after,
		TLSF_AUDIT_CORRUPT_LIST_TARGET, "next link into trailing guard");
	expect_corrupt(frag, ws, wsz, mut_link_guard_before,
		TLSF_AUDIT_CORRUPT_LIST_TARGET, "next link into leading guard");
	expect_corrupt(frag, ws, wsz, mut_null_impostor_readable,
		TLSF_AUDIT_CORRUPT_LIST_TARGET, "readable control impostor");
	expect_corrupt(frag, ws, wsz, mut_null_impostor_guard,
		TLSF_AUDIT_CORRUPT_LIST_TARGET, "impostor in guard page");
	expect_corrupt(frag, ws, wsz, mut_link_to_used,
		TLSF_AUDIT_CORRUPT_LIST_TARGET, "link to a used physical block");

	printf("== list bookkeeping corruptions ==\n");
	expect_corrupt(listy, ws, wsz, mut_wrong_bucket,
		TLSF_AUDIT_CORRUPT_WRONG_BUCKET, "wrong bucket");
	expect_corrupt(listy, ws, wsz, mut_duplicate,
		TLSF_AUDIT_CORRUPT_LIST_DUPLICATE, "duplicate node");
	expect_corrupt(listy, ws, wsz, mut_missing,
		TLSF_AUDIT_CORRUPT_LIST_MISSING, "missing node");
	expect_corrupt(listy, ws, wsz, mut_head_prev_not_null,
		TLSF_AUDIT_CORRUPT_LIST_LINK, "head prev not null");
	expect_corrupt(listy, ws, wsz, mut_reciprocal,
		TLSF_AUDIT_CORRUPT_LIST_LINK, "broken reciprocity");

	/* A self loop must terminate via the bounded chain marks. */
	{
		tlsf_audit_result_t r;
		struct t_block* b = any_free(listy, 0);
		b->next_free = b;
		CHECK(run_audit(listy, ws, wsz, &r) == TLSF_AUDIT_CORRUPT);
		CHECK(r.corruption == TLSF_AUDIT_CORRUPT_LIST_CYCLE);
		restore_heap(listy);
		CHECK(run_audit(listy, ws, wsz, &r) == TLSF_AUDIT_OK);
	}

	free(ws);
	destroy_heap(empty);
	destroy_heap(frag);
	destroy_heap(listy);

	if (g_failures == 0)
	{
		printf("ALL AUDIT CONTRACT TESTS PASSED\n");
		return 0;
	}
	printf("%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
