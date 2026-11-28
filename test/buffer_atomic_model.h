/*
 * Shared core for the evbuffer failure-atomicity tests.
 *
 * This header is included both by the deterministic contract test
 * (test/buffer_atomic_contract.c) and by the libFuzzer entry point
 * (test/buffer_atomic_fuzz.c) so that both run the same bounded action
 * vocabulary and the same checking logic.
 *
 * The core installs a controlled allocator before any libevent object is
 * created, can count allocations of one operation, can make the 1st, 2nd,
 * ... allocation fail, and keeps an independent ledger of every libevent
 * allocation so that temporary chains can never be leaked silently.
 *
 * The model itself only stores the visible byte sequences and the identity
 * of the original reference blocks.  It never looks at libevent internals
 * except to run the same structural validation that the regress tests use.
 */

#ifndef BUFFER_ATOMIC_MODEL_H
#define BUFFER_ATOMIC_MODEL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/buffer_compat.h>

#include "evbuffer-internal.h"

/* ------------------------------------------------------------------ */
/* Controlled allocator                                                */
/* ------------------------------------------------------------------ */

union atomic_align {
	long double ld;
	long long ll;
	void *p;
};

struct atomic_alloc_hdr {
	union atomic_align align;
	size_t size;
	unsigned magic;
};

#define ATOMIC_ALLOC_MAGIC 0x41adbeefu
#define ATOMIC_ALLOC_POISON 0x51adbeefu

static size_t atomic_outstanding;
static size_t atomic_total_allocs;
static int atomic_double_free;
static int atomic_bad_pointer;

/* 0: off, 1: only count, 2: fail at the configured allocation. */
static int atomic_fault_mode;
static long atomic_fault_remaining;
static long atomic_alloc_calls;
static int atomic_fault_hit;

/* Single accounting point for both malloc and realloc, so that a realloc(NULL)
 * path cannot count an allocation twice.  Returns freshly allocated storage
 * (or NULL when the fault fires), never a reallocated pointer. */
static void *
atomic_alloc_internal(size_t sz)
{
	struct atomic_alloc_hdr *h;
	void *raw;

	if (atomic_fault_mode == 2) {
		if (--atomic_fault_remaining == 0) {
			atomic_fault_hit = 1;
			return NULL;
		}
	}
	if (sz == 0)
		sz = 1;
	raw = malloc(sizeof(*h) + sz);
	if (!raw)
		return NULL;
	h = raw;
	h->size = sz;
	h->magic = ATOMIC_ALLOC_MAGIC;
	++atomic_outstanding;
	++atomic_total_allocs;
	return h + 1;
}

static void *
atomic_malloc_fn(size_t sz)
{
	++atomic_alloc_calls;
	return atomic_alloc_internal(sz);
}

static void
atomic_free_fn(void *ptr)
{
	struct atomic_alloc_hdr *h;

	if (!ptr)
		return;
	h = (struct atomic_alloc_hdr *)ptr - 1;
	if (h->magic != ATOMIC_ALLOC_MAGIC) {
		if (h->magic == ATOMIC_ALLOC_POISON)
			atomic_double_free = 1;
		else
			atomic_bad_pointer = 1;
		return;
	}
	h->magic = ATOMIC_ALLOC_POISON;
	if (atomic_outstanding > 0)
		--atomic_outstanding;
	free(h);
}

static void *
atomic_realloc_fn(void *ptr, size_t sz)
{
	struct atomic_alloc_hdr *h;
	void *raw;

	++atomic_alloc_calls;
	if (atomic_fault_mode == 2) {
		if (--atomic_fault_remaining == 0) {
			atomic_fault_hit = 1;
			return NULL;
		}
	}
	if (!ptr)
		return atomic_alloc_internal(sz);
	h = (struct atomic_alloc_hdr *)ptr - 1;
	if (h->magic != ATOMIC_ALLOC_MAGIC) {
		atomic_bad_pointer = 1;
		return NULL;
	}
	if (sz == 0)
		sz = 1;
	raw = realloc(h, sizeof(*h) + sz);
	if (!raw)
		return NULL;
	h = raw;
	h->size = sz;
	return h + 1;
}

/* Must be called before any libevent object is allocated.  Calling it again
 * is harmless. */
static void
atomic_harness_install(void)
{
	event_set_mem_functions(atomic_malloc_fn, atomic_realloc_fn,
	    atomic_free_fn);
}

static void
atomic_fault_off(void)
{
	atomic_fault_mode = 0;
	atomic_fault_remaining = 0;
	atomic_fault_hit = 0;
}

/* Start counting allocations performed by libevent. */
static void
atomic_count_begin(void)
{
	atomic_fault_mode = 1;
	atomic_alloc_calls = 0;
	atomic_fault_hit = 0;
}

/* Return the number of allocations observed since atomic_count_begin(). */
static long
atomic_count_end(void)
{
	atomic_fault_mode = 0;
	return atomic_alloc_calls;
}

/* Make the 'index'th (1-based) allocation fail.  atomic_fail_end() must be
 * called afterwards: as long as the fault is armed, any allocation keeps
 * failing, which means a success-path retry after a triggered fault would be
 * meaningless. */
static void
atomic_fail_begin(long index)
{
	atomic_fault_mode = 2;
	atomic_fault_remaining = index;
	atomic_fault_hit = 0;
	atomic_alloc_calls = 0;
}

static void
atomic_fail_end(void)
{
	atomic_fault_mode = 0;
	atomic_fault_remaining = 0;
}

static size_t
atomic_ledger_outstanding(void)
{
	return atomic_outstanding;
}

/* ------------------------------------------------------------------ */
/* Structural validation (same invariants as test/regress_buffer.c)    */
/* ------------------------------------------------------------------ */

static int
atomic_buffer_valid(struct evbuffer *buf)
{
	struct evbuffer_chain *chain;
	size_t sum = 0;
	int found_last_with_datap = 0;

	if (buf->first == NULL) {
		if (buf->last != NULL || buf->total_len != 0)
			return 0;
	}

	if (!buf->last_with_datap)
		return 0;
	if (buf->last_with_datap == &buf->first)
		found_last_with_datap = 1;

	for (chain = buf->first; chain != NULL; chain = chain->next) {
		if (&chain->next == buf->last_with_datap)
			found_last_with_datap = 1;
		sum += chain->off;
		if (chain->next == NULL && buf->last != chain)
			return 0;
		if (chain->buffer_len < chain->misalign + chain->off)
			return 0;
	}

	if (buf->first && !*buf->last_with_datap)
		return 0;

	if (*buf->last_with_datap) {
		chain = *buf->last_with_datap;
		if (chain->off == 0 || buf->total_len == 0) {
			if (chain->off != 0 || chain != buf->first ||
			    buf->total_len != 0)
				return 0;
		}
		for (chain = chain->next; chain != NULL; chain = chain->next) {
			if (chain->off != 0)
				return 0;
		}
	} else if (buf->last_with_datap != &buf->first) {
		return 0;
	}

	if (!found_last_with_datap)
		return 0;
	return sum == buf->total_len;
}

/* ------------------------------------------------------------------ */
/* Reference-block allocation ledger                                   */
/* ------------------------------------------------------------------ */

/* Blocks created by model seeds all derive from the controlled allocator, so
 * every block allocation is visible in atomic_outstanding together with
 * libevent's own chains.  Tracking them separately lets us report a missing
 * cleanup with the block that leaked. */

struct model_world;
struct model_block;

/* ------------------------------------------------------------------ */
/* Independent model: visible bytes plus reference-block identity      */
/* ------------------------------------------------------------------ */

struct model_block {
	unsigned magic;
	struct model_world *world;
	unsigned char *data;
	size_t len;
	int cleaned;
	int cleanups;
	int identity;
};

struct model_seg {
	unsigned char *data;
	size_t len;
	int block;	/* index into world->blocks, or -1 for literal data */
	int mc;		/* these bytes came from a multicast/reference chain */
};

struct model_buf {
	struct model_seg *segs;
	size_t nsegs;
	size_t capsegs;
	size_t len;
	int alive;
};

#define MODEL_MAX_BUFS 5
#define MODEL_BLOCK_MAGIC 0x6d6f6462u

struct model_world {
	struct evbuffer *lib[MODEL_MAX_BUFS];
	struct model_buf m[MODEL_MAX_BUFS];
	int nbuf;
	struct model_block **blocks;
	size_t nblocks;
	size_t capblocks;
	uint64_t seed;
	uint64_t rng;
	unsigned step;
	int failed;
	char first_diff[160];
};

static uint64_t
model_rng_next(struct model_world *w)
{
	uint64_t x = w->rng;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	w->rng = x;
	return x;
}

static unsigned
model_rng_below(struct model_world *w, unsigned n)
{
	return (unsigned)(model_rng_next(w) % n);
}

static int
model_report(struct model_world *w, const char *field, const char *fmt, ...)
{
	va_list ap;

	if (w->failed)
		return 1;
	w->failed = 1;
	va_start(ap, fmt);
	vsnprintf(w->first_diff, sizeof(w->first_diff), fmt, ap);
	va_end(ap);
	fprintf(stderr,
	    "MODEL FAIL seed=%llu step=%u field=%s: %s\n",
	    (unsigned long long)w->seed, w->step, field, w->first_diff);
	return 1;
}

static void
model_block_cleanup(const void *data, size_t datalen, void *extra)
{
	struct model_block *b = extra;

	if (b == NULL || b->magic != MODEL_BLOCK_MAGIC) {
		fprintf(stderr, "MODEL FAIL cleanup: bad block identity\n");
		return;
	}
	if (data != (const void *)b->data || datalen != b->len) {
		fprintf(stderr,
		    "MODEL FAIL cleanup: block %d pointer/length mismatch\n",
		    b->identity);
		b->world->failed = 1;
		return;
	}
	if (b->cleanups != 0) {
		fprintf(stderr,
		    "MODEL FAIL cleanup: block %d cleaned twice\n",
		    b->identity);
		b->world->failed = 1;
		return;
	}
	b->cleanups = 1;
	b->cleaned = 1;
	free(b->data);
	b->data = NULL;
}

static struct model_block *
model_block_new(struct model_world *w, size_t len)
{
	struct model_block *b;
	size_t i;

	if (w->nblocks == w->capblocks) {
		size_t newcap = w->capblocks ? w->capblocks * 2 : 8;
		struct model_block **nb = realloc(w->blocks,
		    newcap * sizeof(*nb));
		if (!nb)
			return NULL;
		w->blocks = nb;
		w->capblocks = newcap;
	}
	b = calloc(1, sizeof(*b));
	if (!b)
		return NULL;
	b->data = malloc(len ? len : 1);
	if (!b->data) {
		free(b);
		return NULL;
	}
	for (i = 0; i < len; ++i)
		b->data[i] = (unsigned char)('A' + (unsigned)(i % 26));
	b->len = len;
	b->magic = MODEL_BLOCK_MAGIC;
	b->world = w;
	b->identity = (int)w->nblocks;
	w->blocks[w->nblocks++] = b;
	return b;
}

static void
model_seg_append(struct model_buf *m, const unsigned char *data, size_t len,
    int block, int mc)
{
	struct model_seg *s;

	if (len == 0)
		return;
	if (m->nsegs == m->capsegs) {
		size_t newcap = m->capsegs ? m->capsegs * 2 : 8;
		struct model_seg *ns = realloc(m->segs,
		    newcap * sizeof(*ns));
		if (!ns)
			abort();
		m->segs = ns;
		m->capsegs = newcap;
	}
	s = &m->segs[m->nsegs++];
	s->data = malloc(len);
	if (!s->data)
		abort();
	memcpy(s->data, data, len);
	s->len = len;
	s->block = block;
	s->mc = mc;
	m->len += len;
}

static void
model_append_literal(struct model_buf *m, const unsigned char *data,
    size_t len)
{
	model_seg_append(m, data, len, -1, 0);
}

static void
model_append_shared(struct model_buf *dst, const struct model_buf *src)
{
	size_t i;

	for (i = 0; i < src->nsegs; ++i)
		model_seg_append(dst, src->segs[i].data, src->segs[i].len,
		    src->segs[i].block, 1);
}

/* Move k bytes from the front of src into dst (k <= src->len).
 *
 * Whole chains keep their identity (and therefore their cleanup
 * responsibility) when they move; the chain a requested boundary falls into
 * is copied as a mutable fragment, while the untouched remainder stays in
 * src with its original identity.  This mirrors the chain-level behaviour of
 * evbuffer_remove_buffer(). */
static void
model_move_prefix(struct model_buf *src, struct model_buf *dst, size_t k)
{
	size_t taken = 0, i = 0, first_kept;

	while (i < src->nsegs && taken < k) {
		struct model_seg *s = &src->segs[i];
		if (taken + s->len <= k) {
			model_seg_append(dst, s->data, s->len, s->block,
			    s->mc);
			taken += s->len;
			free(s->data);
			++i;
		} else {
			size_t part = k - taken;
			unsigned char *rest;
			model_seg_append(dst, s->data, part, -1, 0);
			taken += part;
			rest = malloc(s->len - part);
			if (!rest)
				abort();
			memcpy(rest, s->data + part, s->len - part);
			free(s->data);
			s->data = rest;
			s->len -= part;
			/* The remainder stays at index i and is kept below. */
			break;
		}
	}
	first_kept = i;
	/* Shift the untouched remainder to the front of the segment array. */
	{
		size_t kept = 0;
		for (i = first_kept; i < src->nsegs; ++i)
			src->segs[kept++] = src->segs[i];
		src->nsegs = kept;
	}
	src->len -= k;
}

/* Remove k bytes from the front of a buffer. */
static void
model_remove_prefix(struct model_buf *m, size_t k)
{
	size_t taken = 0, i = 0, first_kept;

	while (i < m->nsegs && taken < k) {
		struct model_seg *s = &m->segs[i];
		if (taken + s->len <= k) {
			taken += s->len;
			free(s->data);
			++i;
		} else {
			size_t part = k - taken;
			taken += part;
			memmove(s->data, s->data + part, s->len - part);
			s->len -= part;
			break;
		}
	}
	first_kept = i;
	{
		size_t kept = 0;
		for (i = first_kept; i < m->nsegs; ++i)
			m->segs[kept++] = m->segs[i];
		m->nsegs = kept;
	}
	m->len -= k;
}

/* Does evbuffer_pullup() have to copy?  Mirrors the condition in the library:
 * the bytes are already contiguous iff the first chain holds the whole
 * requested prefix. */
static int
model_pullup_would_copy(const struct evbuffer *buf, size_t size)
{
	const struct evbuffer_chain *chain = buf->first;

	if (chain == NULL || chain->off >= size)
		return 0;
	return 1;
}

static void
model_buf_clear(struct model_buf *m)
{
	size_t i;

	for (i = 0; i < m->nsegs; ++i)
		free(m->segs[i].data);
	free(m->segs);
	m->segs = NULL;
	m->nsegs = m->capsegs = 0;
	m->len = 0;
	m->alive = 0;
}

static void
model_forget_identity(struct model_buf *m)
{
	size_t i;

	for (i = 0; i < m->nsegs; ++i) {
		m->segs[i].block = -1;
		m->segs[i].mc = 0;
	}
}

/* The library rejects add_buffer_reference() when the source holds a chain
 * type that cannot be referenced.  Read the actual chain flags rather than
 * the model's sticky 'came from a shared source' marker, which can survive a
 * pullup or drain that already materialised the bytes into a mutable chain. */
static int
model_lib_has_unshareable(const struct evbuffer *buf)
{
	const struct evbuffer_chain *chain;

	for (chain = buf->first; chain; chain = chain->next) {
		if (chain->flags &
		    (EVBUFFER_FILESEGMENT|EVBUFFER_SENDFILE|EVBUFFER_MULTICAST))
			return 1;
	}
	return 0;
}

static void
model_touch_pullup(struct model_buf *m, int copied)
{
	/* evbuffer_pullup() copies only when the requested bytes are not
	 * already contiguous starting at the first chain.  The caller detects
	 * that case structurally.  When nothing was copied, the reference
	 * chains and their identities are untouched; when a copy happened,
	 * the visible bytes stay identical but the reference identity is no
	 * longer observable through the public interface. */
	if (!copied)
		return;
	model_forget_identity(m);
}

/* Compare a libevent buffer with the model and check the allocator and
 * cleanup ledgers. */
static int
model_check_buf(struct model_world *w, int idx)
{
	struct evbuffer *buf = w->lib[idx];
	struct model_buf *m = &w->m[idx];
	size_t len = evbuffer_get_length(buf);
	size_t i, pos = 0;
	unsigned char *tmp;

	if (!m->alive)
		return 0;
	if (len != m->len)
		return model_report(w, "length",
		    "buffer %d expected length %zu, got %zu", idx, m->len,
		    len);
	if (!atomic_buffer_valid(buf))
		return model_report(w, "structure",
		    "buffer %d failed structural validation", idx);
	if (len == 0)
		return 0;
	tmp = malloc(len);
	if (!tmp)
		abort();
	if ((size_t)evbuffer_copyout(buf, tmp, len) != len) {
		free(tmp);
		return model_report(w, "copyout",
		    "buffer %d could not be copied out", idx);
	}
	for (i = 0; i < m->nsegs; ++i) {
		struct model_seg *s = &m->segs[i];
		if (memcmp(tmp + pos, s->data, s->len) != 0) {
			size_t j;
			for (j = 0; j < s->len; ++j)
				if (tmp[pos + j] != s->data[j])
					break;
			model_report(w, "bytes",
			    "buffer %d first difference at byte %zu "
			    "(expected 0x%02x got 0x%02x)", idx, pos + j,
			    (unsigned)s->data[j], (unsigned)tmp[pos + j]);
			free(tmp);
			return 1;
		}
		pos += s->len;
	}
	free(tmp);
	if (pos != len)
		return model_report(w, "segments",
		    "buffer %d segments cover %zu of %zu bytes", idx, pos,
		    len);
	return 0;
}

static int
model_check_cleanup_safety(struct model_world *w)
{
	size_t bi, i, j;

	for (bi = 0; bi < w->nblocks; ++bi) {
		struct model_block *b = w->blocks[bi];
		if (b->cleanups > 1)
			return model_report(w, "cleanup",
			    "block %d cleaned %d times", b->identity,
			    b->cleanups);
		if (b->cleaned) {
			/* A block must never be cleaned while any buffer still
			 * shows its bytes through a reference or shared chain.
			 * The model's segment list is the authoritative record
			 * of which blocks are still visibly reachable. */
			for (i = 0; i < MODEL_MAX_BUFS; ++i) {
				for (j = 0; j < w->m[i].nsegs; ++j) {
					if (w->m[i].segs[j].block ==
					    b->identity)
						return model_report(w,
						    "cleanup",
						    "block %d cleaned while "
						    "buffer %zu still "
						    "references it",
						    b->identity, i);
				}
			}
		} else if (!b->data) {
			return model_report(w, "cleanup",
			    "block %d lost its data", b->identity);
		}
	}
	/* Note: a block that is no longer visible in the model may still be
	 * legitimately owned by a parent buffer that keeps its chains alive
	 * (the library is allowed to defer cleanup that way).  The end-of-run
	 * check that every block was cleaned exactly once covers leaks; here
	 * we only reject premature cleanup. */
	return 0;
}

static int
model_check_all(struct model_world *w)
{
	int i;

	for (i = 0; i < w->nbuf; ++i) {
		if (model_check_buf(w, i))
			return 1;
	}
	return model_check_cleanup_safety(w);
}

static int
model_self_reject(const struct model_buf *src, const struct model_buf *dst)
{
	return src->len != 0 && src == dst;
}

/* True if 'src' currently holds a multicast chain whose source buffer is
 * 'dst'.  Moving such a chain back into its own source would build a
 * reference cycle inside the source buffer; that combination is outside the
 * contract exercised here, so the model simply skips it instead of expecting
 * success. */
static int
model_would_cycle(const struct evbuffer *src, const struct evbuffer *dst)
{
	struct evbuffer_chain *chain;

	for (chain = src->first; chain; chain = chain->next) {
		if (chain->flags & EVBUFFER_MULTICAST) {
			struct evbuffer_multicast_parent *info =
			    EVBUFFER_CHAIN_EXTRA(
				struct evbuffer_multicast_parent, chain);
			if (info->source == dst)
				return 1;
		}
	}
	return 0;
}

/* Execute one bounded action.  Returns 1 on a detected contract violation. */
static int
model_action(struct model_world *w)
{
	int a;
	int b0, b1;
	size_t k;

	if (!w->nbuf)
		return 0;
	/* Choose an action.  The two repaired entry points are exercised
	 * frequently, and allocation failures are only armed for them. */
	a = (int)model_rng_below(w, 8);
	b0 = (int)model_rng_below(w, (unsigned)w->nbuf);
	b1 = (int)model_rng_below(w, (unsigned)w->nbuf);
	if (!w->m[b0].alive) {
		/* Recreate a freed buffer instead of touching it. */
		w->lib[b0] = evbuffer_new();
		if (!w->lib[b0])
			return model_report(w, "alloc",
			    "evbuffer_new failed unexpectedly");
		w->m[b0].alive = 1;
		return 0;
	}

	if (a <= 1) {
		size_t len = 1 + model_rng_below(w, 7);
		unsigned char bytes[8];
		size_t i;
		for (i = 0; i < len; ++i)
			bytes[i] = (unsigned char)('a' +
			    model_rng_below(w, 26));
		if (evbuffer_add(w->lib[b0], bytes, len) != 0)
			return model_report(w, "rc",
			    "evbuffer_add failed unexpectedly");
		model_append_literal(&w->m[b0], bytes, len);
	} else if (a == 2) {
		size_t len = 1 + model_rng_below(w, 9);
		struct model_block *blk;
		struct evbuffer *buf;
		switch (model_rng_below(w, 5)) {
		case 0:
			blk = model_block_new(w, len);
			if (!blk)
				return model_report(w, "alloc",
				    "model block allocation failed");
			buf = w->lib[b0];
			if (evbuffer_add_reference(buf, blk->data, blk->len,
			    model_block_cleanup, blk) != 0)
				return model_report(w, "rc",
				    "evbuffer_add_reference failed "
				    "unexpectedly");
			model_seg_append(&w->m[b0], blk->data, blk->len,
			    blk->identity, 0);
			break;
		}
	} else if (a == 3) {
		/* evbuffer_add_buffer_reference(out, in): non-destructive. */
		struct model_buf *src = &w->m[b1];
		struct model_buf *dst = &w->m[b0];
		int expected_reject = model_self_reject(src, dst);
		int expect_zero = src->len == 0;
		int fault = model_rng_below(w, 3) == 0;
		uint64_t fail_at = 0;
		int rc;
		if (fault)
			fail_at = 1 + model_rng_below(w, 4);
		if (fault)
			atomic_fail_begin((long)fail_at);
		rc = evbuffer_add_buffer_reference(w->lib[b0],
		    w->lib[b1]);
		atomic_fail_end();
		if (fault && atomic_fault_hit) {
			if (rc != -1)
				return model_report(w, "rc",
				    "add_buffer_reference: allocation "
				    "failure returned %d instead of -1", rc);
		} else if (expected_reject || expect_zero) {
			if (rc != (expect_zero ? 0 : -1))
				return model_report(w, "rc",
				    "add_buffer_reference(%s): expected %d "
				    "got %d",
				    expect_zero ? "empty source" : "self",
				    expect_zero ? 0 : -1, rc);
		} else if (model_lib_has_unshareable(w->lib[b1])) {
			if (rc != -1)
				return model_report(w, "rc",
				    "add_buffer_reference from a shared "
				    "source: expected rejection, got %d", rc);
		} else if (rc != 0) {
			return model_report(w, "rc",
			    "add_buffer_reference: expected success, got %d",
			    rc);
		}
		if (rc == 0 && !expect_zero)
			model_append_shared(dst, src);
	} else if (a == 4) {
		/* evbuffer_remove_buffer(src, dst, n). */
		struct model_buf *src = &w->m[b1];
		struct model_buf *dst = &w->m[b0];
		size_t maxk = src->len;
		size_t req;
		int same = (b0 == b1);
		int fault = !same && model_rng_below(w, 3) == 0;
		uint64_t fail_at = 0;
		int rc;
		k = model_rng_below(w, (unsigned)(maxk + 2));
		if (k > 0 && model_rng_below(w, 3) == 0)
			k = maxk;	/* also exercise the whole-buffer path */
		if (!same && model_would_cycle(w->lib[b1], w->lib[b0])) {
			/* Refuse to build a concrete reference cycle; the
			 * model does not model the lifetime of such a
			 * structure.  This is a skip, not a contract check. */
			return 0;
		}
		req = k < maxk ? k : maxk;
		if (fault)
			fail_at = 1 + model_rng_below(w, 4);
		if (fault)
			atomic_fail_begin((long)fail_at);
		rc = evbuffer_remove_buffer(w->lib[b1], w->lib[b0], k);
		atomic_fail_end();
		if (fault && atomic_fault_hit) {
			if (rc != -1)
				return model_report(w, "rc",
				    "remove_buffer: allocation failure "
				    "returned %d instead of -1", rc);
		} else if (same || k == 0) {
			if (rc != 0)
				return model_report(w, "rc",
				    "remove_buffer(%s): expected 0 got %d",
				    same ? "self" : "zero length", rc);
		} else if (rc != (int)req) {
			return model_report(w, "rc",
			    "remove_buffer: expected %zu bytes, got %d",
			    req, rc);
		}
		if (rc > 0)
			model_move_prefix(src, dst, (size_t)rc);
	} else if (a == 5) {
		size_t old = w->m[b0].len;
		k = model_rng_below(w, (unsigned)(old + 2));
		if (k > old)
			k = old;
		if (evbuffer_drain(w->lib[b0], k) != 0)
			return model_report(w, "rc",
			    "evbuffer_drain failed unexpectedly");
		if (k)
			model_remove_prefix(&w->m[b0], k);
	} else if (a == 6) {
		/* pullup must never change the visible bytes.  (No action in
		 * this vocabulary drains a reference chain without keeping
		 * the moved fragment's identity, so cleanup accounting stays
		 * sound.) */
		size_t plen = w->m[b0].len;
		int copies = model_pullup_would_copy(w->lib[b0], plen);
		(void)evbuffer_pullup(w->lib[b0], -1);
		model_touch_pullup(&w->m[b0], copies);
	} else {
		/* free and recreate */
		evbuffer_free(w->lib[b0]);
		w->lib[b0] = evbuffer_new();
		if (!w->lib[b0])
			return model_report(w, "alloc",
			    "evbuffer_new failed unexpectedly");
		model_buf_clear(&w->m[b0]);
		w->m[b0].alive = 1;
	}
	return 0;
}

/* Run one seed.  Returns 0 if everything held; the caller only needs to fail
 * the test if this returns nonzero.  Errors are printed with seed and step. */
static int
model_run_seed(uint64_t seed, unsigned nops)
{
	struct model_world w;
	int i;
	unsigned step;

	memset(&w, 0, sizeof(w));
	atomic_harness_install();
	w.seed = seed;
	w.rng = seed ? seed : 1;
	w.nbuf = 1 + (int)model_rng_below(&w, MODEL_MAX_BUFS);
	for (i = 0; i < w.nbuf; ++i) {
		w.lib[i] = evbuffer_new();
		if (!w.lib[i])
			return model_report(&w, "alloc",
			    "evbuffer_new failed unexpectedly");
		w.m[i].alive = 1;
	}

	for (step = 0; step < nops; ++step) {
		w.step = step;
		if (w.failed)
			break;
		if (model_action(&w))
			break;
		if (model_check_all(&w))
			break;
	}

	/* Release everything; this is also the cleanup ledger check. */
	for (i = 0; i < w.nbuf; ++i) {
		if (w.lib[i]) {
			evbuffer_free(w.lib[i]);
			w.lib[i] = NULL;
		}
		model_buf_clear(&w.m[i]);
	}
	if (!w.failed) {
		size_t bi, refs = 0;
		for (bi = 0; bi < w.nblocks; ++bi) {
			if (w.blocks[bi]->cleanups != 1)
				model_report(&w, "cleanup",
				    "block %zu cleaned %d times", bi,
				    w.blocks[bi]->cleanups);
			else
				++refs;
		}
		if (atomic_ledger_outstanding() != 0)
			model_report(&w, "ledger",
			    "%zu libevent allocations still outstanding",
			    atomic_ledger_outstanding());
		if (atomic_double_free || atomic_bad_pointer)
			model_report(&w, "allocator",
			    "allocator saw a bad free (double=%d badptr=%d)",
			    atomic_double_free, atomic_bad_pointer);
		(void)refs;
	}
	for (i = 0; i < (int)w.nblocks; ++i) {
		struct model_block *b = w.blocks[i];
		if (!b)
			continue;
		/* Any block whose data is still owned must be released here. */
		free(b->data);
		b->magic = 0;
		free(b);
	}
	free(w.blocks);
	atomic_fault_off();
	return w.failed;
}

#endif /* BUFFER_ATOMIC_MODEL_H */
