/*
** tests/allocator_model.c
**
** Direct state-sequence regression for the TLSF C library.  This file does
** not trust tlsf_check() as its oracle: it keeps an independent ledger of
** every live object (generation, request length, owning pool, writable
** interval) together with shadow bytes, and checks every surviving object
** after every operation.  It never recomputes expected addresses from the
** allocator's size classes or block selection rules; it only observes what
** the public API returns and reasons from its own bookkeeping.
**
** Four entry points return a positive assertion count on success, print a
** line beginning with "MISMATCH " and return a negative value on any
** semantic divergence:
**
**   contract_model          - four fixed RNG seeds, 2000 steps each
**   contract_realloc        - hand-written grow/shrink/relocate/failure
**   contract_pool_topology  - hand-written multi-pool / memalign paths
**   contract_replay         - export op text and replay it deterministically
*/

#include "tlsf.h"

#include <stdalign.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Fixed configuration                                                */
/* ------------------------------------------------------------------ */

#define MAX_OBJ        96
#define MAX_REQ        2600
#define MAX_STEPS      2000
#define MAX_WALK       512
#define TRACE_CAP      262144
#define OLD_COPY_CAP   (MAX_REQ + 1)

#define N_ARENA        6
#define POOL_BYTES     12288

#define OP_MALLOC      1
#define OP_MEMALIGN    2
#define OP_REALLOC     3
#define OP_FREE        4
#define OP_EDIT        5
#define OP_VERIFY      6
#define OP_POOL_ADD    7
#define OP_POOL_REM    8
#define OP_DRAIN_POOL  9

#define SEED_COUNT     4
static const uint32_t model_seeds[SEED_COUNT] = {
	1u, 7u, 0xC0FFEEu, 0x9E3779B9u
};

/* Static, strongly aligned storage.  Pools are separate arrays and share
** a single control structure, exactly as the public API intends. */
static alignas(4096) unsigned char g_control_a[8192];
static alignas(4096) unsigned char g_control_b[8192];
static alignas(4096) unsigned char g_arena[N_ARENA][POOL_BYTES];

static unsigned char g_shadow[MAX_OBJ][OLD_COPY_CAP];
static unsigned char g_oldcopy[OLD_COPY_CAP];
static char g_trace[TRACE_CAP];
static char g_replay_buf[TRACE_CAP];

/* ------------------------------------------------------------------ */
/* Deterministic 32-bit integer generator (xorshift)                  */
/* ------------------------------------------------------------------ */

static uint32_t rng_next(uint32_t *state)
{
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

/* ------------------------------------------------------------------ */
/* Ledger                                                             */
/* ------------------------------------------------------------------ */

typedef struct
{
	int live;            /* currently a candidate for free/realloc/edit */
	uint32_t generation; /* identity; different generations never mix  */
	int slot;
	int pool;            /* pool index, -1 when dead                    */
	size_t req_len;      /* requested length, never an internal size    */
	unsigned char *ptr;  /* current writable interval base              */
} object;

typedef struct
{
	int present;
	int arena;           /* index into g_arena                          */
	unsigned char *base;
	size_t bytes;
} pool_record;

typedef struct
{
	int kind;
	int slot;
	int pool;
	size_t size;
	size_t align;
	size_t off;
	size_t len;
	uint32_t stamp;
} op;

typedef struct
{
	unsigned char *ptr;
	size_t size;
	int used;
} walk_block;

typedef struct
{
	int step;
	int kind;
	int slot;
	uint32_t generation;
	int pool;
	size_t off;
	unsigned char got;
	unsigned char want;
	int have;
} mismatch_info;

typedef struct
{
	tlsf_t tlsf;
	object objs[MAX_OBJ];
	pool_record pools[N_ARENA];
	uint32_t generation;
	uint32_t rng;
	uint32_t seed;
	int step;
	int asserts;
	int mismatch;
	int replay_mode;
	int edit_stamp;
	size_t trace_len;
	mismatch_info mm;
} world;

/* ------------------------------------------------------------------ */
/* Content patterns: non-uniform functions of identity and position   */
/* ------------------------------------------------------------------ */

static unsigned char pattern_byte(uint32_t gen, int slot, size_t pos)
{
	uint32_t h = gen * 2654435761u;
	h ^= (uint32_t)(slot + 1) * 2246822519u;
	h ^= (uint32_t)pos * 3266489917u;
	h ^= (uint32_t)(pos * pos) * 65599u;
	h ^= (uint32_t)(pos >> 3) * 40503u;
	h ^= h >> 11;
	return (unsigned char)(h ^ (h >> 5) ^ 0x5Au);
}

static unsigned char edit_byte(uint32_t stamp, int slot, size_t pos)
{
	uint32_t h = stamp * 2246822519u;
	h ^= (uint32_t)(slot + 7) * 3266489917u;
	h ^= (uint32_t)pos * 65599u;
	h ^= h >> 13;
	return (unsigned char)(h ^ 0xA5u);
}

/* ------------------------------------------------------------------ */
/* Failure reporting and trace export                                 */
/* ------------------------------------------------------------------ */

static const char *op_name(int kind)
{
	switch (kind)
	{
	case OP_MALLOC:   return "malloc";
	case OP_MEMALIGN: return "memalign";
	case OP_REALLOC:  return "realloc";
	case OP_FREE:     return "free";
	case OP_EDIT:     return "edit";
	case OP_VERIFY:   return "verify";
	case OP_POOL_ADD: return "pool_add";
	case OP_POOL_REM: return "pool_remove";
	case OP_DRAIN_POOL: return "drain_pool";
	default:          return "unknown";
	}
}

static void export_trace(world *w)
{
	const char *dir = getenv("TMPDIR");
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/allocator_model_replay_s%u.txt",
		(dir && *dir) ? dir : ".", w->seed);
	f = fopen(path, "w");
	if (!f)
	{
		snprintf(path, sizeof(path), "allocator_model_replay_s%u.txt", w->seed);
		f = fopen(path, "w");
	}
	if (f)
	{
		fwrite(g_trace, 1, w->trace_len, f);
		fclose(f);
		fprintf(stderr, "MISMATCH replay-trace exported to %s\n", path);
	}
}

/* Record the first semantic divergence, print one MISMATCH line. */
static int fail_report(world *w, const op *o, const mismatch_info *local)
{
	const mismatch_info *m = local ? local : &w->mm;
	if (w->mismatch)
		return 0;
	w->mismatch = 1;
	w->mm = *m;
	printf("MISMATCH seed=%u step=%d op=%s slot=%d generation=%u pool=%d "
		"offset=%zu got=0x%02x want=0x%02x\n",
		w->seed, w->step, o ? op_name(o->kind) : "none",
		m->slot, m->generation, m->pool, m->off, m->got, m->want);
	export_trace(w);
	return 0;
}

static int check(world *w, int cond, const char *what)
{
	if (!cond)
	{
		mismatch_info m = { w->step, 0, -1, w->generation, -1, 0, 0, 0, 1 };
		printf("MISMATCH seed=%u step=%d check=%s\n", w->seed, w->step, what);
		if (!w->mismatch)
		{
			w->mismatch = 1;
			w->mm = m;
			export_trace(w);
		}
		return 0;
	}
	w->asserts++;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Trace recording / replay text                                      */
/* ------------------------------------------------------------------ */

static void trace_printf(world *w, const char *fmt, ...)
{
	va_list ap;
	int n;
	if (w->replay_mode || w->trace_len >= TRACE_CAP - 128)
		return;
	va_start(ap, fmt);
	n = vsnprintf(g_trace + w->trace_len, TRACE_CAP - w->trace_len, fmt, ap);
	va_end(ap);
	if (n > 0)
		w->trace_len += (size_t)n < TRACE_CAP - w->trace_len
			? (size_t)n : TRACE_CAP - 1 - w->trace_len;
}

/* ------------------------------------------------------------------ */
/* Pool helpers                                                       */
/* ------------------------------------------------------------------ */

static int pool_of_ptr(world *w, unsigned char *ptr)
{
	int i;
	for (i = 0; i < N_ARENA; ++i)
	{
		pool_record *pr = &w->pools[i];
		if (pr->present && ptr >= pr->base && ptr < pr->base + pr->bytes)
			return i;
	}
	return -1;
}

static int pool_live_count(world *w, int pool)
{
	int i, n = 0;
	for (i = 0; i < MAX_OBJ; ++i)
		if (w->objs[i].live && w->objs[i].pool == pool)
			++n;
	return n;
}

static int total_live(world *w)
{
	int i, n = 0;
	for (i = 0; i < MAX_OBJ; ++i)
		if (w->objs[i].live)
			++n;
	return n;
}

static int find_dead_slot(world *w)
{
	int i;
	for (i = 0; i < MAX_OBJ; ++i)
		if (!w->objs[i].live)
			return i;
	return -1;
}

static int pick_live_slot(world *w, uint32_t pick)
{
	int i, n = total_live(w);
	if (!n)
		return -1;
	pick %= (uint32_t)n;
	for (i = 0; i < MAX_OBJ; ++i)
		if (w->objs[i].live && pick-- == 0)
			return i;
	return -1;
}

/* ------------------------------------------------------------------ */
/* Whole-state verification against the independent ledger            */
/* ------------------------------------------------------------------ */

static int verify_all_objects(world *w, const op *o)
{
	int i, j;

	for (i = 0; i < MAX_OBJ; ++i)
	{
		object *a = &w->objs[i];
		if (!a->live)
			continue;

		if (!check(w, a->pool >= 0 && w->pools[a->pool].present,
			"live object owns a present pool"))
			return fail_report(w, o, &(mismatch_info){
				w->step, o ? o->kind : 0, i, a->generation, a->pool,
				0, 0, 0, 1 });

		/* The full request interval must lie inside its pool. */
		{
			pool_record *pr = &w->pools[a->pool];
			if (!check(w, a->ptr >= pr->base
				&& a->ptr + a->req_len <= pr->base + pr->bytes
				&& a->ptr + a->req_len > a->ptr,
				"request interval inside pool"))
				return fail_report(w, o, &(mismatch_info){
					w->step, o ? o->kind : 0, i, a->generation, a->pool,
					a->req_len, 0, 0, 1 });
		}

		/* The allocator's own block must be at least as large as the
		** request, but the request length never comes from block_size. */
		if (!check(w, tlsf_block_size(a->ptr) >= a->req_len,
			"internal block covers request length"))
			return fail_report(w, o, &(mismatch_info){
				w->step, o ? o->kind : 0, i, a->generation, a->pool,
				a->req_len, (unsigned char)tlsf_block_size(a->ptr), 0, 1 });

		/* Shadow bytes over the whole request interval. */
		for (j = 0; j < (int)a->req_len; ++j)
		{
			unsigned char got = a->ptr[j];
			unsigned char want = g_shadow[i][j];
			if (got != want)
			{
				/* Offset relative to the owning pool's storage. */
				size_t pool_off = (size_t)(a->ptr + j
					- w->pools[a->pool].base);
				mismatch_info m = {
					w->step, o ? o->kind : 0, i, a->generation, a->pool,
					pool_off, got, want, 1
				};
				return fail_report(w, o, &m);
			}
		}
		w->asserts++;
	}

	/* Pairwise disjoint live intervals: catches object overlap even if
	** the colliding bytes happen to hold equal values. */
	for (i = 0; i < MAX_OBJ; ++i)
	{
		object *a = &w->objs[i];
		if (!a->live)
			continue;
		for (j = i + 1; j < MAX_OBJ; ++j)
		{
			object *b = &w->objs[j];
			if (!b->live || a->pool != b->pool)
				continue;
			if (a->ptr < b->ptr + b->req_len && b->ptr < a->ptr + a->req_len)
			{
				unsigned char *ov_lo = a->ptr > b->ptr ? a->ptr : b->ptr;
				unsigned char *ov_hi = a->ptr + a->req_len < b->ptr + b->req_len
					? a->ptr + a->req_len : b->ptr + b->req_len;
				size_t off_lo = (size_t)(ov_lo - w->pools[a->pool].base);
				size_t off_hi = (size_t)(ov_hi - w->pools[a->pool].base);
				mismatch_info m = {
					w->step, o ? o->kind : 0, i, a->generation, a->pool,
					off_lo, 0, 0, 1
				};
				printf("MISMATCH seed=%u step=%d op=%s overlap slot=%d gen=%u "
					"pool=%d pool-interval[%zu,%zu) with slot=%d gen=%u\n",
					w->seed, w->step, o ? op_name(o->kind) : "none",
					i, a->generation, a->pool, off_lo, off_hi,
					j, b->generation);
				if (!w->mismatch)
				{
					w->mismatch = 1;
					w->mm = m;
					export_trace(w);
				}
				return 0;
			}
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Independent tlsf_walk_pool topology cross-check                    */
/* ------------------------------------------------------------------ */

typedef struct
{
	world *w;
	int pool;
	walk_block blocks[MAX_WALK];
	int n;
} walk_ctx;

static void walk_collector(void *ptr, size_t size, int used, void *user)
{
	walk_ctx *c = (walk_ctx *)user;
	if (c->n < MAX_WALK)
	{
		c->blocks[c->n].ptr = (unsigned char *)ptr;
		c->blocks[c->n].size = size;
		c->blocks[c->n].used = used;
		++c->n;
	}
}

static int verify_pool_topology(world *w, int pool)
{
	pool_record *pr = &w->pools[pool];
	walk_ctx c;
	walk_block *b;
	int matched[MAX_OBJ];
	int i, k;
	size_t overhead = tlsf_pool_overhead();
	unsigned char *first_user;
	unsigned char *sentinel_user;

	memset(&c, 0, sizeof(c));
	c.w = w;
	c.pool = pool;
	tlsf_walk_pool(pr->base, walk_collector, &c);

	if (!check(w, c.n < MAX_WALK, "walk block count bounded"))
		return 0;
	if (!check(w, tlsf_check_pool(pr->base) == 0, "pool physical integrity"))
		return 0;

	first_user = pr->base + overhead / 2;                 /* base + 8 */
	/* The sentinel header sits at base + pool_bytes and its user
	** address is one block_start_offset further on, which resolves to
	** the byte just past the whole pool storage. */
	sentinel_user = pr->base + pr->bytes;

	/* The walk yields every real block in address order; it stops just
	** before the zero-size sentinel, so the final real block must end
	** exactly where that sentinel begins. */
	if (!check(w, c.n >= 1, "pool has at least one real block"))
		return 0;
	if (!check(w, tlsf_check_pool(pr->base) == 0, "pool physical integrity"))
		return 0;

	b = &c.blocks[0];
	if (!check(w, b->ptr == first_user, "first block at pool origin"))
		return 0;
	for (k = 0; k < c.n; ++k)
	{
		unsigned char *expect_next;
		b = &c.blocks[k];
		if (k > 0)
		{
			walk_block *prev = &c.blocks[k - 1];
			expect_next = prev->ptr + prev->size + overhead / 2;
			if (!check(w, b->ptr == expect_next
				&& b->ptr > prev->ptr, "blocks ordered and contiguous"))
				return 0;
		}
	}
	b = &c.blocks[c.n - 1];
	if (!check(w, b->ptr + b->size + overhead / 2 == sentinel_user,
		"final block ends at the sentinel"))
		return 0;

	memset(matched, 0, sizeof(matched));

	for (k = 0; k < c.n; ++k)
	{
		b = &c.blocks[k];
		if (b->used)
		{
			/* Every used block corresponds to exactly one ledger object
			** whose complete request interval sits inside the block. */
			int hit = -1;
			for (i = 0; i < MAX_OBJ; ++i)
			{
				object *a = &w->objs[i];
				if (a->live && a->pool == pool && a->ptr == b->ptr)
				{
					if (!check(w, hit == -1, "unique ledger object per block"))
						return 0;
					hit = i;
				}
			}
			if (!check(w, hit != -1, "used block has ledger entry"))
				return 0;
			if (!check(w, w->objs[hit].req_len <= b->size,
				"request interval inside used block"))
				return 0;
			matched[hit] = 1;
		}
		else
		{
			/* No live interval may intersect a free block. */
			for (i = 0; i < MAX_OBJ; ++i)
			{
				object *a = &w->objs[i];
				if (a->live && a->pool == pool
					&& a->ptr < b->ptr + b->size
					&& b->ptr < a->ptr + a->req_len)
				{
					mismatch_info m = {
						w->step, 0, i, a->generation, pool,
						(size_t)(b->ptr - pr->base), 0, 0, 1
					};
					return fail_report(w, 0, &m);
				}
			}
		}
	}

	/* And every ledger object in the pool was matched. */
	for (i = 0; i < MAX_OBJ; ++i)
	{
		object *a = &w->objs[i];
		if (a->live && a->pool == pool
			&& !check(w, matched[i], "ledger object backed by used block"))
			return 0;
	}
	return 1;
}

static int verify_all_pools(world *w)
{
	int i;
	if (!check(w, tlsf_check(w->tlsf) == 0,
		"internal bitmaps and free lists"))
		return 0;
	for (i = 0; i < N_ARENA; ++i)
		if (w->pools[i].present && !verify_pool_topology(w, i))
			return 0;
	return 1;
}

/* ------------------------------------------------------------------ */
/* World lifecycle                                                     */
/* ------------------------------------------------------------------ */

static void world_init(world *w, void *control_mem, uint32_t seed,
	int replay_mode)
{
	memset(w, 0, sizeof(*w));
	w->tlsf = tlsf_create(control_mem);
	w->seed = seed;
	w->replay_mode = replay_mode;
	/* Pool 0 is the first region attached to the shared control. */
	w->pools[0].present = 1;
	w->pools[0].arena = 0;
	w->pools[0].base = g_arena[0];
	w->pools[0].bytes = POOL_BYTES;
	tlsf_add_pool(w->tlsf, g_arena[0], POOL_BYTES);
}

static int world_add_pool(world *w, int idx)
{
	pool_record *pr;
	int i;
	if (!check(w, idx >= 0 && idx < N_ARENA, "pool index in range"))
		return 0;
	pr = &w->pools[idx];
	if (!check(w, !pr->present, "pool added once"))
		return 0;

	/* Old identities from a previous life of this storage must stay dead:
	** no live interval may intersect the span being re-attached. */
	for (i = 0; i < MAX_OBJ; ++i)
	{
		object *a = &w->objs[i];
		if (a->live && a->ptr >= g_arena[idx]
			&& a->ptr < g_arena[idx] + POOL_BYTES)
		{
			mismatch_info m = {
				w->step, OP_POOL_ADD, i, a->generation, -1,
				(size_t)(a->ptr - g_arena[idx]), 0, 0, 1
			};
			return fail_report(w, 0, &m);
		}
	}

	{
		pool_t p = tlsf_add_pool(w->tlsf, g_arena[idx], POOL_BYTES);
		if (!check(w, p != 0, "tlsf_add_pool succeeds"))
			return 0;
	}
	pr->present = 1;
	pr->arena = idx;
	pr->base = g_arena[idx];
	pr->bytes = POOL_BYTES;
	return 1;
}

static int world_remove_pool(world *w, int idx)
{
	pool_record *pr;
	if (!check(w, idx >= 0 && idx < N_ARENA, "pool index in range"))
		return 0;
	pr = &w->pools[idx];
	if (!check(w, pr->present, "pool present before removal"))
		return 0;
	/* The ledger, not the free list, decides legality: a pool leaves
	** only after every object charged to it has been freed. */
	if (!check(w, pool_live_count(w, idx) == 0,
		"pool fully free before removal"))
		return 0;
	tlsf_remove_pool(w->tlsf, pr->base);
	pr->present = 0;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Operation application and ledger maintenance                       */
/* ------------------------------------------------------------------ */

static void fill_pattern(world *w, int slot, size_t from, size_t to)
{
	object *a = &w->objs[slot];
	size_t i;
	for (i = from; i < to; ++i)
	{
		unsigned char v = pattern_byte(a->generation, slot, i);
		a->ptr[i] = v;
		g_shadow[slot][i] = v;
	}
}

static int apply_malloc(world *w, const op *o)
{
	unsigned char *p;
	int slot = o->slot;
	int pool;

	p = (unsigned char *)(o->kind == OP_MEMALIGN
		? tlsf_memalign(w->tlsf, o->align, o->size)
		: tlsf_malloc(w->tlsf, o->size));

	/* This library maps every zero-length request to a null result. */
	if (o->size == 0)
		return check(w, p == 0, "zero-length request returns null");

	if (!p)
	{
		/* Allocation failure: nothing in the ledger may change and the
		** sequence simply continues. */
		return check(w, !w->objs[slot].live, "failed alloc keeps slot dead")
			&& verify_all_objects(w, o);
	}

	if (!check(w, slot >= 0 && slot < MAX_OBJ && !w->objs[slot].live,
		"allocation lands in a dead ledger slot"))
		return 0;

	pool = pool_of_ptr(w, p);
	if (!check(w, pool >= 0, "returned pointer belongs to a known pool"))
		return 0;

	if (o->kind == OP_MEMALIGN && ((uintptr_t)p % o->align) != 0)
	{
		mismatch_info m = {
			w->step, o->kind, slot, w->generation + 1, pool,
			(size_t)(p - w->pools[pool].base),
			(unsigned char)((uintptr_t)p % o->align),
			(unsigned char)o->align, 1
		};
		return fail_report(w, o, &m);
	}

	w->generation++;
	{
		object *a = &w->objs[slot];
		a->live = 1;
		a->generation = w->generation;
		a->slot = slot;
		a->pool = pool;
		a->req_len = o->size;
		a->ptr = p;
	}
	fill_pattern(w, slot, 0, o->size);
	w->asserts++;

	if (!verify_all_objects(w, o))
		return 0;
	return 1;
}

static int apply_free(world *w, const op *o)
{
	object *a = &w->objs[o->slot];
	if (!check(w, a->live, "free targets a live ledger object"))
		return 0;
	tlsf_free(w->tlsf, a->ptr);
	/* The old handle leaves the candidate set immediately; its generation
	** is retained in the record so reuse can never resurrect it. */
	a->live = 0;
	a->pool = -1;
	a->ptr = 0;
	return verify_all_objects(w, o);
}

static int apply_realloc(world *w, const op *o)
{
	object *a = &w->objs[o->slot];
	unsigned char *newp;
	size_t keep, i;

	/* realloc(NULL, n) is the documented malloc equivalent. */
	if (!a->live)
	{
		newp = (unsigned char *)tlsf_realloc(w->tlsf, 0, o->size);
		if (o->size == 0)
			return check(w, newp == 0,
				"realloc(NULL, 0) returns null like malloc(0)");
		if (!newp)
			return check(w, !a->live, "failed realloc(NULL) keeps slot dead")
				&& verify_all_objects(w, o);
		{
			int pool = pool_of_ptr(w, newp);
			if (!check(w, pool >= 0, "realloc(NULL) lands in a known pool"))
				return 0;
			w->generation++;
			a->live = 1;
			a->generation = w->generation;
			a->slot = o->slot;
			a->pool = pool;
			a->req_len = o->size;
			a->ptr = newp;
		}
		fill_pattern(w, o->slot, 0, o->size);
		w->asserts++;
		return verify_all_objects(w, o);
	}

	memcpy(g_oldcopy, g_shadow[o->slot], a->req_len);

	newp = (unsigned char *)tlsf_realloc(w->tlsf, a->ptr, o->size);

	/* Zero size is the library's free convention. */
	if (o->size == 0)
	{
		if (!check(w, newp == 0, "realloc to zero frees and returns null"))
			return 0;
		a->live = 0;
		a->pool = -1;
		a->ptr = 0;
		return verify_all_objects(w, o);
	}

	if (!newp)
	{
		/* Failure must preserve the whole old object and the ledger. */
		if (!check(w, a->live && a->req_len > 0,
			"failed realloc keeps ledger entry"))
			return 0;
		for (i = 0; i < a->req_len; ++i)
		{
			if (a->ptr[i] != g_oldcopy[i])
			{
				mismatch_info m = {
					w->step, o->kind, o->slot, a->generation, a->pool,
					(size_t)(a->ptr + i - w->pools[a->pool].base),
					a->ptr[i], g_oldcopy[i], 1
				};
				return fail_report(w, o, &m);
			}
		}
		w->asserts++;
		return verify_all_objects(w, o);
	}

	keep = a->req_len < o->size ? a->req_len : o->size;

	/* Only min(old request, new length) bytes are required to survive,
	** checked against the pre-call shadow before it is rewritten. */
	for (i = 0; i < keep; ++i)
	{
		if (newp[i] != g_oldcopy[i])
		{
			int pool = pool_of_ptr(w, newp);
			mismatch_info m = {
				w->step, o->kind, o->slot, a->generation, pool,
				(size_t)(newp + i - w->pools[pool].base),
				newp[i], g_oldcopy[i], 1
			};
			return fail_report(w, o, &m);
		}
	}
	w->asserts++;

	{
		int pool = pool_of_ptr(w, newp);
		if (!check(w, pool >= 0, "realloc pointer in a known pool"))
			return 0;
		if (!check(w, tlsf_block_size(newp) >= o->size,
			"reallocated block covers new request"))
			return 0;

		w->generation++;
		a->live = 1;
		a->generation = w->generation;
		a->pool = pool;
		a->ptr = newp;
		a->req_len = o->size;

		/* The grown tail is confirmed present before being patterned. */
		for (i = keep; i < o->size; ++i)
		{
			unsigned char v = pattern_byte(a->generation, o->slot, i);
			newp[i] = v;
			g_shadow[o->slot][i] = v;
		}
	}
	return verify_all_objects(w, o);
}

static int apply_edit(world *w, const op *o)
{
	object *a = &w->objs[o->slot];
	size_t i;
	if (!check(w, a->live, "edit targets a live object"))
		return 0;
	if (!check(w, o->off + o->len <= a->req_len && o->len > 0,
		"edit subrange inside request interval"))
		return 0;
	for (i = o->off; i < o->off + o->len; ++i)
	{
		unsigned char v = edit_byte(o->stamp, o->slot, i);
		a->ptr[i] = v;
		g_shadow[o->slot][i] = v;
	}
	w->asserts++;
	return verify_all_objects(w, o);
}

static int apply_op(world *w, const op *o)
{
	int rc = 1;
	w->step++;
	switch (o->kind)
	{
	case OP_MALLOC:
	case OP_MEMALIGN:
		rc = apply_malloc(w, o);
		break;
	case OP_REALLOC:
		rc = apply_realloc(w, o);
		break;
	case OP_FREE:
		rc = apply_free(w, o);
		break;
	case OP_EDIT:
		rc = apply_edit(w, o);
		break;
	case OP_VERIFY:
		rc = verify_all_objects(w, o);
		if (rc)
			rc = verify_all_pools(w);
		break;
	case OP_POOL_ADD:
		rc = world_add_pool(w, o->pool);
		break;
	case OP_POOL_REM:
		rc = world_remove_pool(w, o->pool);
		break;
	case OP_DRAIN_POOL:
		/* Ledger-driven: free every still-live object charged to the
		** pool, in slot order.  No free-list inspection is involved. */
		{
			int i;
			rc = 1;
			for (i = 0; i < MAX_OBJ && rc; ++i)
			{
				if (w->objs[i].live && w->objs[i].pool == o->pool)
				{
					op f = { OP_FREE, i, -1, 0, 0, 0, 0, 0 };
					rc = apply_op(w, &f);
				}
			}
		}
		break;
	default:
		rc = check(w, 0, "unknown op kind");
		break;
	}
	return rc;
}

/* ------------------------------------------------------------------ */
/* RNG-driven generator: legality comes from the ledger only          */
/* ------------------------------------------------------------------ */

static size_t choose_size(world *w)
{
	uint32_t r = rng_next(&w->rng);
	switch (r % 10)
	{
	case 0:
		return (size_t)(1 + rng_next(&w->rng) % 32);
	case 1:
		/* Guaranteed beyond any single pool: deterministic failure. */
		return (size_t)POOL_BYTES * 2 + 4096;
	default:
		return (size_t)(1 + rng_next(&w->rng) % MAX_REQ);
	}
}

static int run_generated(world *w, int steps)
{
	static const size_t aligns[] = { 16, 32, 64, 128, 256 };
	int s;

	/* Emit exactly one replay line for the op that is about to run, so
	** rewritten/coerced ops are never silently dropped from the trace. */
	#define EMIT_TRACE(o) do { \
		switch ((o).kind) \
		{ \
		case OP_MALLOC:   trace_printf(w, "M %d %zu\n", (o).slot, (o).size); break; \
		case OP_MEMALIGN: trace_printf(w, "L %d %zu %zu\n", (o).slot, (o).align, (o).size); break; \
		case OP_REALLOC:  trace_printf(w, "R %d %zu\n", (o).slot, (o).size); break; \
		case OP_FREE:     trace_printf(w, "F %d\n", (o).slot); break; \
		case OP_EDIT:     trace_printf(w, "E %d %zu %zu %u\n", (o).slot, (o).off, (o).len, (o).stamp); break; \
		case OP_VERIFY:   trace_printf(w, "V\n"); break; \
		case OP_POOL_ADD: trace_printf(w, "PA %d\n", (o).pool); break; \
		case OP_POOL_REM: trace_printf(w, "PR %d\n", (o).pool); break; \
		case OP_DRAIN_POOL: trace_printf(w, "PD %d\n", (o).pool); break; \
		default: break; \
		} } while (0)

	for (s = 0; s < steps && !w->mismatch; ++s)
	{
		uint32_t r = rng_next(&w->rng);
		int live = total_live(w);
		int kind;
		op o;

		memset(&o, 0, sizeof(o));
		kind = (int)(r % 100);

		if (kind < 30)
			o.kind = OP_MALLOC;
		else if (kind < 45)
			o.kind = OP_MEMALIGN;
		else if (kind < 65)
			o.kind = live ? OP_REALLOC : OP_MALLOC;
		else if (kind < 80)
			o.kind = live ? OP_FREE : OP_MALLOC;
		else if (kind < 88)
			o.kind = live ? OP_EDIT : OP_MALLOC;
		else if (kind < 92)
			o.kind = OP_VERIFY;
		else if (kind < 96)
			o.kind = OP_POOL_ADD;
		else
			o.kind = OP_POOL_REM;

		switch (o.kind)
		{
		case OP_MALLOC:
			o.slot = find_dead_slot(w);
			if (o.slot < 0)
			{
				o.kind = OP_VERIFY;
				break;
			}
			o.size = (r % 25 == 0) ? 0 : choose_size(w);
			break;

		case OP_MEMALIGN:
			o.slot = find_dead_slot(w);
			if (o.slot < 0)
			{
				o.kind = OP_VERIFY;
				break;
			}
			o.align = aligns[rng_next(&w->rng) % (sizeof(aligns) / sizeof(aligns[0]))];
			o.size = (r % 30 == 0) ? 0 : (size_t)(1 + rng_next(&w->rng) % 900);
			break;

		case OP_REALLOC:
			o.slot = pick_live_slot(w, rng_next(&w->rng));
			if (r % 12 == 0)
				o.size = 0;
			else if (r % 2)
				o.size = (size_t)(1 + rng_next(&w->rng) % (MAX_REQ));
			else
			{
				size_t cur = w->objs[o.slot].req_len;
				o.size = cur > 1 ? (size_t)(1 + rng_next(&w->rng) % cur) : 1;
			}
			break;

		case OP_FREE:
			o.slot = pick_live_slot(w, rng_next(&w->rng));
			break;

		case OP_EDIT:
		{
			size_t req;
			o.slot = pick_live_slot(w, rng_next(&w->rng));
			req = w->objs[o.slot].req_len;
			o.len = (size_t)(1 + rng_next(&w->rng) % (req < 16 ? req : 16));
			o.off = req > o.len ? rng_next(&w->rng) % (req - o.len) : 0;
			o.stamp = (uint32_t)++w->edit_stamp;
			break;
		}

		case OP_VERIFY:
			break;

		case OP_POOL_ADD:
		{
			int idx = 1 + (int)(rng_next(&w->rng) % (N_ARENA - 1));
			int tries = N_ARENA;
			while (tries-- && w->pools[idx].present)
				idx = (idx + 1) % N_ARENA;
			if (w->pools[idx].present)
				o.kind = OP_VERIFY;
			else
				o.pool = idx;
			break;
		}

		case OP_POOL_REM:
		{
			int idx = 1 + (int)(rng_next(&w->rng) % (N_ARENA - 1));
			int tries = N_ARENA;
			while (tries-- && (!w->pools[idx].present
				|| pool_live_count(w, idx) != 0))
				idx = (idx + 1) % N_ARENA;
			if (idx == 0 || !w->pools[idx].present
				|| pool_live_count(w, idx) != 0)
				o.kind = OP_VERIFY;
			else
				o.pool = idx;
			break;
		}
		}

		EMIT_TRACE(o);
		if (!apply_op(w, &o))
			return 0;

		/* Periodic full topology walk even when no explicit verify op
		** was drawn, plus a sweep after every 32 generated steps. */
		if ((s & 31) == 31 && !verify_all_pools(w))
			return 0;
	}

	if (!w->mismatch && !verify_all_pools(w))
		return 0;
	return 1;
#undef EMIT_TRACE
}

/* ------------------------------------------------------------------ */
/* Fingerprinting and replay                                          */
/* ------------------------------------------------------------------ */

static uint32_t world_fingerprint(world *w)
{
	uint32_t fp = 2166136261u;
	int i, j;
	for (i = 0; i < MAX_OBJ; ++i)
	{
		object *a = &w->objs[i];
		if (!a->live)
			continue;
		fp ^= (uint32_t)i; fp *= 16777619u;
		fp ^= a->generation; fp *= 16777619u;
		fp ^= (uint32_t)a->pool; fp *= 16777619u;
		fp ^= (uint32_t)(a->ptr - w->pools[a->pool].base);
		fp *= 16777619u;
		fp ^= (uint32_t)a->req_len; fp *= 16777619u;
		for (j = 0; j < (int)a->req_len; ++j)
		{
			fp ^= g_shadow[i][j];
			fp *= 16777619u;
		}
	}
	return fp;
}

static int replay_text(world *w, const char *text)
{
	const char *p = text;
	while (*p)
	{
		const char *e = strchr(p, '\n');
		char line[160];
		op o;
		int n = 0;
		size_t line_len;

		if (!e)
			e = p + strlen(p);
		line_len = (size_t)(e - p);
		if (line_len >= sizeof(line))
			line_len = sizeof(line) - 1;
		memcpy(line, p, line_len);
		line[line_len] = 0;
		p = *e ? e + 1 : e;

		memset(&o, 0, sizeof(o));
		if (line[0] == '#' || line[0] == 0)
			continue;

		switch (line[0])
		{
		case 'M':
			n = sscanf(line, "M %d %zu", &o.slot, &o.size);
			o.kind = OP_MALLOC;
			break;
		case 'L':
			n = sscanf(line, "L %d %zu %zu",
				&o.slot, &o.align, &o.size);
			o.kind = OP_MEMALIGN;
			break;
		case 'R':
			n = sscanf(line, "R %d %zu", &o.slot, &o.size);
			o.kind = OP_REALLOC;
			break;
		case 'F':
			n = sscanf(line, "F %d", &o.slot);
			o.kind = OP_FREE;
			break;
		case 'E':
			n = sscanf(line, "E %d %zu %zu %u",
				&o.slot, &o.off, &o.len, &o.stamp);
			o.kind = OP_EDIT;
			break;
		case 'V':
			n = 1;
			o.kind = OP_VERIFY;
			break;
		case 'P':
			if (line[1] == 'A')
			{
				n = sscanf(line, "PA %d", &o.pool);
				o.kind = OP_POOL_ADD;
			}
			else if (line[1] == 'D')
			{
				n = sscanf(line, "PD %d", &o.pool);
				o.kind = OP_DRAIN_POOL;
			}
			else
			{
				n = sscanf(line, "PR %d", &o.pool);
				o.kind = OP_POOL_REM;
			}
			break;
		default:
			break;
		}
		if (n <= 0 || !apply_op(w, &o))
			return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Hand-written boundary sequences                                    */
/* ------------------------------------------------------------------ */

typedef struct
{
	op ops[512];
	int n;
} script;

static void script_op(script *sc, op o)
{
	if (sc->n < (int)(sizeof(sc->ops) / sizeof(sc->ops[0])))
		sc->ops[sc->n++] = o;
}

static void s_malloc(script *sc, int slot, size_t size)
{ script_op(sc, (op){ OP_MALLOC, slot, -1, size, 0, 0, 0, 0 }); }
static void s_memalign(script *sc, int slot, size_t align, size_t size)
{ script_op(sc, (op){ OP_MEMALIGN, slot, -1, size, align, 0, 0, 0 }); }
static void s_realloc(script *sc, int slot, size_t size)
{ script_op(sc, (op){ OP_REALLOC, slot, -1, size, 0, 0, 0, 0 }); }
static void s_free(script *sc, int slot)
{ script_op(sc, (op){ OP_FREE, slot, -1, 0, 0, 0, 0, 0 }); }
static void s_verify(script *sc)
{ script_op(sc, (op){ OP_VERIFY, -1, -1, 0, 0, 0, 0, 0 }); }
static void s_pool_add(script *sc, int pool)
{ script_op(sc, (op){ OP_POOL_ADD, -1, pool, 0, 0, 0, 0, 0 }); }
static void s_pool_rem(script *sc, int pool)
{ script_op(sc, (op){ OP_POOL_REM, -1, pool, 0, 0, 0, 0, 0 }); }
static void s_drain(script *sc, int pool)
{ script_op(sc, (op){ OP_DRAIN_POOL, -1, pool, 0, 0, 0, 0, 0 }); }

static int run_script(world *w, const script *sc)
{
	int i;
	for (i = 0; i < sc->n; ++i)
		if (!apply_op(w, &sc->ops[i]))
			return 0;
	return 1;
}

/* Realloc boundary behavior: in-place grow through neighbor coalescing,
** shrink leaving a tail block, forced relocation behind a busy successor,
** failure preserving the old object, and the zero/null conventions. */
static int build_realloc_script(script *sc)
{
	/* Phase 1: grow in place, then shrink, inside one pool. */
	s_malloc(sc, 0, 64);    /* a: first block, user ptr pool+8   */
	s_malloc(sc, 1, 64);    /* b: immediate physical successor   */
	s_free(sc, 1);          /* b joins the free tail             */
	s_realloc(sc, 0, 96);   /* fits a+b combined: in-place grow  */
	s_verify(sc);
	s_realloc(sc, 0, 40);   /* shrink: trimmed tail stays free   */
	s_verify(sc);
	s_free(sc, 0);
	s_verify(sc);

	/* Phase 2: busy successor forces relocation with copy. */
	s_malloc(sc, 2, 128);   /* g: first block                    */
	s_malloc(sc, 3, 128);   /* h: busy successor of g            */
	s_realloc(sc, 2, 400);  /* cannot absorb h: malloc+copy+free */
	s_verify(sc);
	s_free(sc, 2);
	s_free(sc, 3);
	s_verify(sc);

	/* Phase 3: realloc(NULL, n) behaves like malloc. */
	s_realloc(sc, 4, 48);
	s_verify(sc);

	/* Phase 4: realloc to zero frees. */
	s_realloc(sc, 4, 0);
	s_verify(sc);

	/* Phase 5: exhaust the pool, force a failure, keep going. */
	{
		int slot;
		for (slot = 5; slot < MAX_OBJ; ++slot)
			s_malloc(sc, slot, 512);   /* null results are legal       */
		s_verify(sc);
		/* A live object survives; an impossible grow must fail and
		** leave every byte and the ledger untouched. */
		s_realloc(sc, 5, (size_t)POOL_BYTES * 2);
		s_verify(sc);
		s_free(sc, 5);                 /* free one, then alloc again   */
		s_malloc(sc, 5, 256);
		s_verify(sc);
	}
	return 1;
}

/* Pool topology: multiple pools sharing one control, walks, removal
** only when empty, re-attachment of storage, and the memalign small
** prefix-hole adjustment. */
static int build_pool_script(script *sc)
{
	int slot;

	/* Fill pool 0 through the public API until requests start failing. */
	for (slot = 0; slot < 40; ++slot)
		s_malloc(sc, slot, 512);
	s_verify(sc);

	/* More pools share the same control.  After an add, allocations may
	** land in any pool; the ledger records the real placement rather
	** than assuming everything comes from the newest pool. */
	s_pool_add(sc, 1);
	s_malloc(sc, 40, 512);
	s_pool_add(sc, 2);
	s_memalign(sc, 41, 64, 300);
	s_memalign(sc, 42, 128, 200);
	s_verify(sc);

	/* Pool 2 empties - decided purely from ledger ownership - before
	** it is allowed to leave. */
	s_drain(sc, 2);
	s_verify(sc);
	s_pool_rem(sc, 2);
	s_verify(sc);

	/* The same storage comes back; previous-generation identities that
	** used to live there must not count as live.  Fresh slots are used
	** because the earlier memalign objects may have been served by
	** another pool and therefore remain live there. */
	s_pool_add(sc, 2);
	s_malloc(sc, 45, 256);
	s_memalign(sc, 46, 256, 128);
	s_verify(sc);

	/* An oversized request fails and the sequence simply continues. */
	s_malloc(sc, 47, (size_t)POOL_BYTES * 3);
	s_malloc(sc, 48, 128);
	s_verify(sc);

	/* Drain and remove pool 1 through the same ledger-driven path. */
	s_drain(sc, 1);
	s_verify(sc);
	s_pool_rem(sc, 1);
	s_verify(sc);
	return 1;
}

/* Dedicated memalign scenario: force gap in [1, gap_minimum) so the
** allocator must skip to the next aligned boundary and release a small
** leading free block.  Pool base is 4096-aligned; the first user pointer
** is base+8 and the 32-byte block ends at base+40. */
static int build_memalign_gap_script(script *sc)
{
	s_malloc(sc, 0, 32);             /* occupies [base+8, base+40)     */
	/* Free tail begins at base+40: reaching base+64 is a 16-byte gap,
	** below the 24-byte gap minimum, forcing the boundary skip. */
	s_memalign(sc, 1, 64, 256);
	s_verify(sc);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Contract entry points                                              */
/* ------------------------------------------------------------------ */

int contract_model(void)
{
	world w;
	int k;
	int total = 0;

	for (k = 0; k < SEED_COUNT; ++k)
	{
		memset(g_trace, 0, sizeof(g_trace));
		world_init(&w, g_control_a, model_seeds[k], 0);
		w.rng = model_seeds[k] ? model_seeds[k] : 1u;
		if (!run_generated(&w, MAX_STEPS))
			return -1;
		if (!check(&w, total_live(&w) >= 0, "seed run completes"))
			return -1;
		total += w.asserts;
		tlsf_destroy(w.tlsf);
	}
	return total;
}

int contract_realloc(void)
{
	world w;
	script sc;
	unsigned char *p_grow;
	unsigned char *p_shrink;

	memset(&sc, 0, sizeof(sc));
	build_realloc_script(&sc);

	memset(g_trace, 0, sizeof(g_trace));
	world_init(&w, g_control_b, 0, 0);

	/* In-place expectations around the first phase. */
	if (!run_script(&w, &sc))
		return -1;

	/* Re-run just the grow/shrink pair to assert pointer stability. */
	{
		world w2;
		script pair;
		object *a;
		memset(&pair, 0, sizeof(pair));
		s_malloc(&pair, 0, 64);
		s_malloc(&pair, 1, 64);
		s_free(&pair, 1);
		s_realloc(&pair, 0, 96);
		s_realloc(&pair, 0, 40);

		world_init(&w2, g_control_a, 0, 0);
		if (!apply_op(&w2, &pair.ops[0]))
			return -1;
		a = &w2.objs[0];
		p_grow = a->ptr;
		if (!apply_op(&w2, &pair.ops[1]) || !apply_op(&w2, &pair.ops[2])
			|| !apply_op(&w2, &pair.ops[3]))
			return -1;
		if (!check(&w2, w2.objs[0].ptr == p_grow,
			"in-place realloc preserves pointer through grow"))
			return -1;
		p_shrink = w2.objs[0].ptr;
		if (!apply_op(&w2, &pair.ops[4]))
			return -1;
		if (!check(&w2, w2.objs[0].ptr == p_shrink
			&& w2.objs[0].req_len == 40,
			"in-place shrink preserves pointer and request"))
			return -1;

		/* The shrink must leave a real free tail block immediately past
		** the shrunken request, observed independently via the walk. */
		{
			walk_ctx c;
			walk_block *tail = 0;
			int k;
			memset(&c, 0, sizeof(c));
			c.w = &w2;
			c.pool = 0;
			tlsf_walk_pool(w2.pools[0].base, walk_collector, &c);
			for (k = 0; k < c.n; ++k)
			{
				if (!c.blocks[k].used
					&& c.blocks[k].ptr > w2.objs[0].ptr)
				{
					tail = &c.blocks[k];
					break;
				}
			}
			if (!check(&w2, tail != 0, "shrink leaves a free tail block"))
				return -1;
		}
		if (!verify_pool_topology(&w2, 0))
			return -1;
		w.asserts += w2.asserts;
	}

	/* Forced relocation: a busy successor makes in-place growth
	** impossible, so a successful grow must move the object and copy. */
	{
		world w3;
		script rel;
		unsigned char *old_ptr;
		memset(&rel, 0, sizeof(rel));
		s_malloc(&rel, 0, 128);
		s_malloc(&rel, 1, 128);               /* busy physical successor */
		s_realloc(&rel, 0, 400);             /* cannot absorb it: move   */

		world_init(&w3, g_control_a, 0, 0);
		if (!apply_op(&w3, &rel.ops[0]) || !apply_op(&w3, &rel.ops[1]))
			return -1;
		old_ptr = w3.objs[0].ptr;
		if (!apply_op(&w3, &rel.ops[2]))
			return -1;
		if (!check(&w3, w3.objs[0].live
			&& w3.objs[0].ptr != old_ptr,
			"busy successor forces realloc relocation to a new address"))
			return -1;
		if (!check(&w3, w3.objs[1].live,
			"the busy successor object is left allocated and intact"))
			return -1;
		if (!verify_all_objects(&w3, &rel.ops[2]))
			return -1;
		if (!verify_pool_topology(&w3, 0))
			return -1;
		w.asserts += w3.asserts;
	}

	return w.asserts;
}

int contract_pool_topology(void)
{
	world w;
	script sc;

	memset(&sc, 0, sizeof(sc));
	build_pool_script(&sc);
	memset(g_trace, 0, sizeof(g_trace));
	world_init(&w, g_control_b, 0, 0);
	if (!run_script(&w, &sc))
		return -1;

	/* The memalign gap scenario needs a pristine pool whose layout is
	** known, so it runs in its own world on a fresh control. */
	{
		world w2;
		script gap;
		walk_ctx pre, post;
		walk_block *hole = 0;
		unsigned char *free_tail = 0;
		uintptr_t naive_boundary;
		int k;

		memset(&gap, 0, sizeof(gap));
		build_memalign_gap_script(&gap);
		world_init(&w2, g_control_a, 0, 0);

		/* Run only the first allocation, then observe where the free
		** tail begins using the public walk. */
		if (!apply_op(&w2, &gap.ops[0]))
			return -1;
		memset(&pre, 0, sizeof(pre));
		pre.w = &w2;
		pre.pool = 0;
		tlsf_walk_pool(w2.pools[0].base, walk_collector, &pre);
		for (k = 0; k < pre.n; ++k)
		{
			if (!pre.blocks[k].used)
			{
				free_tail = pre.blocks[k].ptr;
				break;
			}
		}
		if (!check(&w2, free_tail != 0, "free tail observed before memalign"))
			return -1;

		/* Where a plain align-up would place the user pointer.  The small
		** prefix gap below the allocator's minimum forces it past this
		** first candidate to the next 64-byte boundary. */
		naive_boundary = ((uintptr_t)free_tail + 63u) & ~(uintptr_t)63u;

		if (!apply_op(&w2, &gap.ops[1]) || !apply_op(&w2, &gap.ops[2]))
			return -1;

		if (!check(&w2, w2.objs[1].live
			&& ((uintptr_t)w2.objs[1].ptr % 64) == 0,
			"gap-adjusted memalign result is 64-aligned"))
			return -1;
		if (!check(&w2, (uintptr_t)w2.objs[1].ptr > naive_boundary,
			"small prefix hole skips the first aligned boundary"))
			return -1;

		memset(&post, 0, sizeof(post));
		post.w = &w2;
		post.pool = 0;
		tlsf_walk_pool(w2.pools[0].base, walk_collector, &post);
		for (k = 0; k < post.n; ++k)
		{
			if (!post.blocks[k].used
				&& post.blocks[k].ptr > w2.objs[0].ptr
				&& post.blocks[k].ptr < w2.objs[1].ptr)
				hole = &post.blocks[k];
		}
		if (!check(&w2, hole != 0 && hole->ptr < w2.objs[1].ptr,
			"memalign released a small leading free block"))
			return -1;
		if (!verify_pool_topology(&w2, 0))
			return -1;
		w.asserts += w2.asserts;
	}
	return w.asserts;
}

int contract_replay(void)
{
	world original, replayed;
	uint32_t fp1, fp2;
	char path[512];
	const char *dir;
	FILE *f;

	/* 1. Generate and execute a history, capturing replay text. */
	memset(g_trace, 0, sizeof(g_trace));
	world_init(&original, g_control_a, 0x1234ABCDu, 0);
	original.rng = 0x1234ABCDu;
	if (!run_generated(&original, 500))
	{
		/* Under a mutated library the original run diverges; the replay
		** must reach the same first semantic deviation. */
		mismatch_info first = original.mm;
		world_init(&replayed, g_control_b, 0x1234ABCDu, 1);
		if (replay_text(&replayed, g_trace))
		{
			check(&replayed, 0, "replay expected same divergence");
			return -1;
		}
		if (!check(&replayed, replayed.mm.have == first.have
			&& replayed.mm.step == first.step
			&& replayed.mm.slot == first.slot
			&& replayed.mm.pool == first.pool
			&& replayed.mm.off == first.off
			&& replayed.mm.got == first.got
			&& replayed.mm.want == first.want,
			"replay reaches identical first deviation"))
			return -1;
		return replayed.asserts > 0 ? -replayed.asserts : -1;
	}
	fp1 = world_fingerprint(&original);

	/* 2. Export the text, then replay it on a fresh world with fresh
	** storage; decisions never depend on absolute addresses. */
	dir = getenv("TMPDIR");
	snprintf(path, sizeof(path), "%s/allocator_model_replay.txt",
		(dir && *dir) ? dir : ".");
	f = fopen(path, "w");
	if (!f)
		f = fopen("allocator_model_replay.txt", "w");
	if (!check(&original, f != 0, "replay text export opens"))
		return -1;
	fwrite(g_trace, 1, original.trace_len, f);
	fclose(f);

	/* Read the exported artifact straight back from disk and replay it,
	** proving the file itself - not the in-memory trace - round-trips. */
	f = fopen(path, "r");
	if (!f)
		f = fopen("allocator_model_replay.txt", "r");
	if (!check(&original, f != 0, "exported replay text reopens"))
		return -1;
	{
		size_t got = fread(g_replay_buf, 1, sizeof(g_replay_buf) - 1, f);
		g_replay_buf[got] = 0;
		fclose(f);
		if (!check(&original, got == original.trace_len,
			"exported replay text is complete"))
			return -1;
	}

	world_init(&replayed, g_control_b, 0x1234ABCDu, 1);
	if (!replay_text(&replayed, g_replay_buf))
		return -1;
	fp2 = world_fingerprint(&replayed);
	if (!check(&replayed, fp1 == fp2,
		"replayed history has identical fingerprint"))
		return -1;
	if (!check(&replayed, replayed.step == original.step,
		"replayed history has identical length"))
		return -1;

	/* 3. Replay a hand-written boundary script as well. */
	{
		world h1, h2;
		script sc;
		uint32_t hfp1, hfp2;
		memset(&sc, 0, sizeof(sc));
		build_realloc_script(&sc);

		world_init(&h1, g_control_a, 0, 0);
		if (!run_script(&h1, &sc))
			return -1;
		hfp1 = world_fingerprint(&h1);

		/* Serialize the script through the same text format. */
		{
			char buf[4096];
			int i;
			size_t pos = 0;
			buf[0] = 0;
			for (i = 0; i < sc.n; ++i)
			{
				op *o = &sc.ops[i];
				int n = 0;
				switch (o->kind)
				{
				case OP_MALLOC:
					n = snprintf(buf + pos, sizeof(buf) - pos,
						"M %d %zu\n", o->slot, o->size);
					break;
				case OP_REALLOC:
					n = snprintf(buf + pos, sizeof(buf) - pos,
						"R %d %zu\n", o->slot, o->size);
					break;
				case OP_FREE:
					n = snprintf(buf + pos, sizeof(buf) - pos,
						"F %d\n", o->slot);
					break;
				case OP_VERIFY:
					n = snprintf(buf + pos, sizeof(buf) - pos, "V\n");
					break;
				}
				if (n > 0)
					pos += (size_t)n < sizeof(buf) - pos
						? (size_t)n : sizeof(buf) - 1 - pos;
			}
			world_init(&h2, g_control_b, 0, 1);
			if (!replay_text(&h2, buf))
				return -1;
			hfp2 = world_fingerprint(&h2);
			if (!check(&h2, hfp1 == hfp2,
				"hand-written script replays identically"))
				return -1;
			replayed.asserts += h2.asserts;
		}
	}

	return original.asserts + replayed.asserts;
}
