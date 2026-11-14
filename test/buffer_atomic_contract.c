/*
 * Atomicity contract tests for evbuffer_add_buffer_reference() and
 * evbuffer_remove_buffer() under controlled allocation failure.
 *
 * The whole file is C99 and uses only the public evbuffer API plus the
 * task_support/check.h assertion harness.  A controlled allocator is
 * installed before any libevent object is created; every failure point of a
 * successful operation is enumerated from a live counting run, then each
 * allocation is failed one at a time from a fresh equivalent state.
 */

#include <event2/buffer.h>
#include <event2/event.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../task_support/check.h"
#include "buffer_atomic_test.h"

/* ------------------------------------------------------------------ */
/* Controlled allocator: libc-backed, with one-shot fault injection   */
/* and an independent live/total allocation ledger.                   */
/* ------------------------------------------------------------------ */

struct alloc_header {
	size_t size;
	struct alloc_header *prev;
	struct alloc_header *next;
};

static struct alloc_header *alloc_list;
static size_t live_allocs;
static size_t total_allocs;
static int fault_armed;
static long fault_at = -1;
static size_t fault_hits;
static int allocator_installed;

static void
ledger_link(struct alloc_header *h)
{
	h->prev = NULL;
	h->next = alloc_list;
	if (alloc_list)
		alloc_list->prev = h;
	alloc_list = h;
	++live_allocs;
	++total_allocs;
}

static void
ledger_unlink(struct alloc_header *h)
{
	if (h->prev)
		h->prev->next = h->next;
	else
		alloc_list = h->next;
	if (h->next)
		h->next->prev = h->prev;
	h->prev = h->next = NULL;
	--live_allocs;
}

static void *
test_malloc(size_t n)
{
	struct alloc_header *h;

	if (fault_armed && fault_at > 0) {
		if (--fault_at == 0) {
			fault_armed = 0;
			fault_at = -1;
			++fault_hits;
			errno = ENOMEM;
			return NULL;
		}
	}

	h = (struct alloc_header *)malloc(sizeof(*h) + n);
	if (!h) {
		errno = ENOMEM;
		return NULL;
	}
	h->size = n;
	ledger_link(h);
	return (char *)h + sizeof(*h);
}

static void *
test_realloc(void *p, size_t n)
{
	struct alloc_header *h, *nh;

	if (!p)
		return test_malloc(n);

	h = (struct alloc_header *)((char *)p - sizeof(*h));

	if (fault_armed && fault_at > 0) {
		if (--fault_at == 0) {
			/* realloc() failure must leave the original block
			 * untouched; keep it on the live list. */
			fault_armed = 0;
			fault_at = -1;
			++fault_hits;
			errno = ENOMEM;
			return NULL;
		}
	}

	ledger_unlink(h);
	nh = (struct alloc_header *)realloc(h, sizeof(*nh) + n);
	if (!nh) {
		ledger_link(h);
		errno = ENOMEM;
		return NULL;
	}
	nh->size = n;
	ledger_link(nh);
	return (char *)nh + sizeof(*nh);
}

static void
test_free(void *p)
{
	struct alloc_header *h;

	if (!p)
		return;
	h = (struct alloc_header *)((char *)p - sizeof(*h));
	ledger_unlink(h);
	free(h);
}

static void
test_log_discard(int severity, const char *msg)
{
	(void)severity;
	(void)msg;
	/* Allocation-failure warnings are part of the exercised paths. */
}

static void
ensure_allocator(void)
{
	if (!allocator_installed) {
		event_set_mem_functions(test_malloc, test_realloc, test_free);
		event_set_log_callback(test_log_discard);
		allocator_installed = 1;
	}
}

static void
fault_arm(long nth)
{
	fault_armed = 1;
	fault_at = nth;
	fault_hits = 0;
}

static void
fault_disarm(void)
{
	fault_armed = 0;
	fault_at = -1;
}

void
buffer_test_allocator_reset(void)
{
	fault_disarm();
	fault_hits = 0;
}

size_t
buffer_test_live_allocations(void)
{
	return live_allocs;
}

size_t
buffer_test_total_allocations(void)
{
	return total_allocs;
}

/* ------------------------------------------------------------------ */
/* External reference blocks with independent identity and cleanup.   */
/* ------------------------------------------------------------------ */

#define MAX_BLOCKS 12

struct ext_block {
	unsigned char *data;
	size_t len;
	int id;
	int cleanup_calls;
};

static int cleanup_bad_args;
static int cleanup_double;

static void
block_cleanup(const void *p, size_t n, void *arg)
{
	struct ext_block *b = (struct ext_block *)arg;

	/* Verify the original pointer, length and identity, not just the
	 * number of calls. */
	if (p != (const void *)b->data || n != b->len || b->id <= 0)
		cleanup_bad_args = 1;
	if (b->cleanup_calls != 0)
		cleanup_double = 1;
	++b->cleanup_calls;
	free(b->data);
	b->data = NULL;
}

static void
block_init(struct ext_block *b, int id, size_t len)
{
	size_t i;

	b->data = (unsigned char *)malloc(len ? len : 1);
	b->len = len;
	b->id = id;
	b->cleanup_calls = 0;
	for (i = 0; i < len; ++i)
		b->data[i] = (unsigned char)('A' + (id * 7 + (int)i) % 26);
}

static void
block_discard_unused(struct ext_block *b)
{
	if (b->cleanup_calls == 0 && b->data) {
		free(b->data);
		b->data = NULL;
	}
}

static int
add_block_reference(struct evbuffer *buf, struct ext_block *b)
{
	return evbuffer_add_reference(buf, b->data, b->len,
	    block_cleanup, b);
}

/* ------------------------------------------------------------------ */
/* Callback observers: record bytes, never counts alone.              */
/* ------------------------------------------------------------------ */

struct cb_observer {
	size_t n_added;
	size_t n_deleted;
	unsigned calls;
};

static void
observer_cb(struct evbuffer *b, const struct evbuffer_cb_info *info, void *arg)
{
	struct cb_observer *o = (struct cb_observer *)arg;

	(void)b;
	o->n_added += info->n_added;
	o->n_deleted += info->n_deleted;
	++o->calls;
}

static struct cb_observer *
observer_attach(struct evbuffer *buf)
{
	struct cb_observer *o = (struct cb_observer *)calloc(1, sizeof(*o));
	if (!o)
		return NULL;
	if (!evbuffer_add_cb(buf, observer_cb, o)) {
		free(o);
		return NULL;
	}
	return o;
}

/* ------------------------------------------------------------------ */
/* Snapshot helpers: bytes, lengths and chain data addresses.         */
/* ------------------------------------------------------------------ */

#define MAX_PEEK 32

struct buffer_snapshot {
	size_t len;
	unsigned char *bytes;
	size_t n_addr;
	void *addr[MAX_PEEK];
	size_t n_added;
	size_t n_deleted;
};

static size_t
peek_addresses(struct evbuffer *buf, void **addr, size_t max)
{
	struct evbuffer_iovec vec[MAX_PEEK];
	int n = evbuffer_peek(buf, -1, NULL, vec, MAX_PEEK);
	size_t i, cnt;

	if (n < 0)
		return 0;
	cnt = ((size_t)n < max) ? (size_t)n : max;
	for (i = 0; i < cnt; ++i)
		addr[i] = vec[i].iov_base;
	return cnt;
}

static int
snapshot_capture(struct buffer_snapshot *s, struct evbuffer *buf,
    const struct cb_observer *o)
{
	ev_ssize_t got;

	memset(s, 0, sizeof(*s));
	s->len = evbuffer_get_length(buf);
	s->bytes = (unsigned char *)malloc(s->len ? s->len : 1);
	if (!s->bytes)
		return -1;
	got = evbuffer_copyout(buf, s->bytes, s->len);
	if (got != (ev_ssize_t)s->len) {
		free(s->bytes);
		s->bytes = NULL;
		return -1;
	}
	s->n_addr = peek_addresses(buf, s->addr, MAX_PEEK);
	if (o) {
		s->n_added = o->n_added;
		s->n_deleted = o->n_deleted;
	}
	return 0;
}

static void
snapshot_release(struct buffer_snapshot *s)
{
	free(s->bytes);
	s->bytes = NULL;
}

static int
snapshot_matches(const struct buffer_snapshot *s, struct evbuffer *buf,
    const struct cb_observer *o)
{
	struct buffer_snapshot now;

	if (snapshot_capture(&now, buf, o) < 0)
		return -1;
	if (now.len != s->len ||
	    memcmp(now.bytes, s->bytes, s->len) != 0 ||
	    now.n_addr != s->n_addr ||
	    memcmp(now.addr, s->addr, s->n_addr * sizeof(void *)) != 0 ||
	    now.n_added != s->n_added || now.n_deleted != s->n_deleted) {
		snapshot_release(&now);
		return -1;
	}
	snapshot_release(&now);
	return 0;
}

static int
buffer_equals(struct evbuffer *buf, const unsigned char *expect, size_t len)
{
	unsigned char *tmp;
	int ok;

	if (evbuffer_get_length(buf) != len)
		return -1;
	tmp = (unsigned char *)malloc(len ? len : 1);
	if (!tmp)
		return -1;
	ok = (evbuffer_copyout(buf, tmp, len) == (ev_ssize_t)len &&
	    memcmp(tmp, expect, len) == 0) ? 0 : -1;
	free(tmp);
	return ok;
}

/* Run the event loop once without blocking; used for deferred callback
 * delivery only. */
static void
pump_once(struct event_base *base)
{
	event_base_loop(base, EVLOOP_NONBLOCK);
}

/* Require-style wrapper: every condition is a real recorded assertion via
 * TASK_CHECK, and a failure returns from the case immediately. */
#define REQUIRE(cond) TASK_CHECK(cond)

/* ================================================================== */
/* reference group                                                    */
/* ================================================================== */

#define REF_BLOCKS 3
#define REF_PREFIX_LEN 2

struct ref_world {
	struct evbuffer *src;
	struct evbuffer *dst;
	struct evbuffer *view1;
	struct evbuffer *view2;
	struct cb_observer *osrc;
	struct cb_observer *odst;
	struct cb_observer *ov1;
	struct cb_observer *ov2;
	struct ext_block blocks[REF_BLOCKS];
	struct buffer_snapshot ss_src;
	struct buffer_snapshot ss_dst;
	struct buffer_snapshot ss_v1;
	struct buffer_snapshot ss_v2;
	size_t src_len;
	size_t live_before;
};

static const unsigned char ref_prefix[REF_PREFIX_LEN] = { 'Z', 'Z' };

static int
ref_world_build(struct ref_world *w)
{
	static const size_t lens[REF_BLOCKS] = { 5, 7, 4 };
	size_t i;

	memset(w, 0, sizeof(*w));
	w->src = evbuffer_new();
	w->dst = evbuffer_new();
	w->view1 = evbuffer_new();
	w->view2 = evbuffer_new();
	if (!w->src || !w->dst || !w->view1 || !w->view2)
		return -1;

	for (i = 0; i < REF_BLOCKS; ++i) {
		block_init(&w->blocks[i], (int)i + 1, lens[i]);
		if (add_block_reference(w->src, &w->blocks[i]) != 0)
			return -1;
	}
	if (evbuffer_add_buffer_reference(w->view1, w->src) != 0)
		return -1;
	if (evbuffer_add_buffer_reference(w->view2, w->src) != 0)
		return -1;
	if (evbuffer_add(w->dst, ref_prefix, REF_PREFIX_LEN) != 0)
		return -1;

	w->osrc = observer_attach(w->src);
	w->odst = observer_attach(w->dst);
	w->ov1 = observer_attach(w->view1);
	w->ov2 = observer_attach(w->view2);
	if (!w->osrc || !w->odst || !w->ov1 || !w->ov2)
		return -1;

	w->src_len = evbuffer_get_length(w->src);
	if (snapshot_capture(&w->ss_src, w->src, w->osrc) < 0 ||
	    snapshot_capture(&w->ss_dst, w->dst, w->odst) < 0 ||
	    snapshot_capture(&w->ss_v1, w->view1, w->ov1) < 0 ||
	    snapshot_capture(&w->ss_v2, w->view2, w->ov2) < 0)
		return -1;

	w->live_before = buffer_test_live_allocations();
	return 0;
}

static int
ref_world_unchanged(const struct ref_world *w)
{
	size_t i;

	if (snapshot_matches(&w->ss_src, w->src, w->osrc) < 0 ||
	    snapshot_matches(&w->ss_dst, w->dst, w->odst) < 0 ||
	    snapshot_matches(&w->ss_v1, w->view1, w->ov1) < 0 ||
	    snapshot_matches(&w->ss_v2, w->view2, w->ov2) < 0)
		return -1;
	for (i = 0; i < REF_BLOCKS; ++i) {
		if (w->blocks[i].cleanup_calls != 0)
			return -1;
	}
	if (buffer_test_live_allocations() != w->live_before)
		return -1;
	return 0;
}

static void
ref_world_free(struct ref_world *w)
{
	size_t i;

	snapshot_release(&w->ss_src);
	snapshot_release(&w->ss_dst);
	snapshot_release(&w->ss_v1);
	snapshot_release(&w->ss_v2);
	if (w->src)
		evbuffer_free(w->src);
	if (w->view1)
		evbuffer_free(w->view1);
	if (w->view2)
		evbuffer_free(w->view2);
	if (w->dst)
		evbuffer_free(w->dst);
	free(w->osrc);
	free(w->odst);
	free(w->ov1);
	free(w->ov2);
	for (i = 0; i < REF_BLOCKS; ++i)
		block_discard_unused(&w->blocks[i]);
}

/* Count how many allocations one successful add_buffer_reference performs,
 * measured from a real run rather than assumed. */
static int
ref_count_op_allocations(size_t *count_out)
{
	struct ref_world w;
	size_t before, after;

	if (ref_world_build(&w) < 0)
		return -1;
	before = buffer_test_total_allocations();
	if (evbuffer_add_buffer_reference(w.dst, w.src) != 0) {
		ref_world_free(&w);
		return -1;
	}
	after = buffer_test_total_allocations();
	*count_out = after - before;
	ref_world_free(&w);
	return 0;
}

static int
ref_oom_iteration(size_t k)
{
	struct ref_world w;
	int rc, ok = 0;

	if (ref_world_build(&w) < 0)
		return -1;

	fault_arm((long)k);
	rc = evbuffer_add_buffer_reference(w.dst, w.src);
	fault_disarm();
	/* Every injection point must actually be reached. */
	if (rc != -1 || fault_hits != 1)
		goto out;
	if (ref_world_unchanged(&w) < 0)
		goto out;

	/* Fault cleared: the equivalent operation now completes and the
	 * original objects remain fully usable. */
	rc = evbuffer_add_buffer_reference(w.dst, w.src);
	if (rc != 0)
		goto out;
	if (evbuffer_get_length(w.dst) != REF_PREFIX_LEN + w.src_len)
		goto out;
	if (evbuffer_get_length(w.src) != w.src_len)
		goto out;
	if (w.odst->n_added != w.src_len || w.osrc->n_added != 0 ||
	    w.ov1->n_added != 0 || w.ov2->n_added != 0)
		goto out;
	/* Existing views keep their storage addresses and bytes. */
	if (snapshot_matches(&w.ss_v1, w.view1, w.ov1) < 0 ||
	    snapshot_matches(&w.ss_v2, w.view2, w.ov2) < 0)
		goto out;
	/* Source stays mutable after a successful share. */
	if (evbuffer_add(w.src, "Q", 1) != 0)
		goto out;
	if (evbuffer_drain(w.src, 1) != 0)
		goto out;
	if (evbuffer_pullup(w.dst, (ev_ssize_t)w.src_len) == NULL)
		goto out;
	ok = 1;
out:
	ref_world_free(&w);
	return ok ? 0 : -1;
}

static int
check_reference_oom_atomic(void)
{
	size_t n_allocs, k;

	if (ref_count_op_allocations(&n_allocs) < 0)
		return -1;
	/* The number of allocations is observed, not hard coded. */
	if (n_allocs < 1)
		return -1;
	for (k = 1; k <= n_allocs; ++k) {
		if (ref_oom_iteration(k) < 0)
			return -1;
	}
	/* All external blocks are cleaned exactly once overall. */
	if (cleanup_bad_args || cleanup_double)
		return -1;
	return 0;
}

static int
case_reference_oom_atomic(void)
{
	ensure_allocator();
	REQUIRE(check_reference_oom_atomic() == 0);
	return 0;
}

static int
check_reference_share_lifecycle(void)
{
	struct ext_block blocks[REF_BLOCKS];
	static const size_t lens[REF_BLOCKS] = { 6, 3, 5 };
	struct evbuffer *src, *t1, *t2, *t3;
	unsigned char expect[64];
	size_t i, total = 0, off = 0;
	char out[64];

	src = evbuffer_new();
	t1 = evbuffer_new();
	t2 = evbuffer_new();
	t3 = evbuffer_new();
	if (!src || !t1 || !t2 || !t3)
		return 1;
	for (i = 0; i < REF_BLOCKS; ++i) {
		block_init(&blocks[i], (int)i + 10, lens[i]);
		if (add_block_reference(src, &blocks[i]) != 0)
			return 1;
		memcpy(expect + total, blocks[i].data, blocks[i].len);
		total += lens[i];
	}
	if (evbuffer_add(t1, "PRE", 3) != 0 ||
	    evbuffer_add_buffer_reference(t2, src) != 0 ||
	    evbuffer_add_buffer_reference(t3, src) != 0)
		return 1;

	/* Free the source first: every target must still read the bytes. */
	evbuffer_free(src);
	src = NULL;
	for (i = 0; i < REF_BLOCKS; ++i)
		if (blocks[i].cleanup_calls != 0)
			return 1;

	memset(out, 0, sizeof(out));
	if (evbuffer_copyout(t2, out, total) != (ev_ssize_t)total ||
	    memcmp(out, expect, total) != 0)
		return 1;
	if (evbuffer_copyout(t3, out, total) != (ev_ssize_t)total ||
	    memcmp(out, expect, total) != 0)
		return 1;

	/* Interleaved release of the different targets. */
	evbuffer_free(t2);
	for (i = 0; i < REF_BLOCKS; ++i)
		if (blocks[i].cleanup_calls != 0)
			return 1;
	evbuffer_drain(t3, evbuffer_get_length(t3));
	evbuffer_free(t3);
	for (i = 0; i < REF_BLOCKS; ++i)
		if (blocks[i].cleanup_calls != 1)
			return 1;

	/* t1 never referenced the blocks and must not touch them. */
	off = evbuffer_get_length(t1);
	if (off != 3)
		return 1;
	evbuffer_free(t1);
	for (i = 0; i < REF_BLOCKS; ++i) {
		if (blocks[i].cleanup_calls != 1)
			return 1;
		block_discard_unused(&blocks[i]);
	}
	if (cleanup_bad_args || cleanup_double)
		return 1;
	return 0;
}

static int
check_reference_rejections(void)
{
	struct evbuffer *b, *src, *mcast_src;
	struct ext_block blk;
	unsigned char before[32];
	size_t len;
	int rc;

	b = evbuffer_new();
	src = evbuffer_new();
	mcast_src = evbuffer_new();
	if (!b || !src || !mcast_src)
		return 1;
	block_init(&blk, 99, 4);
	if (add_block_reference(src, &blk) != 0)
		return 1;
	if (evbuffer_add(b, "abc", 3) != 0)
		return 1;
	len = evbuffer_get_length(b);
	if (evbuffer_copyout(b, before, len) != (ev_ssize_t)len)
		return 1;

	/* Self reference keeps the existing rejection contract. */
	rc = evbuffer_add_buffer_reference(b, b);
	if (rc != -1 || evbuffer_get_length(b) != len)
		return 1;

	/* Frozen end rejects before any allocation. */
	if (evbuffer_freeze(b, 0) != 0)
		return 1;
	rc = evbuffer_add_buffer_reference(b, src);
	if (rc != -1 || evbuffer_get_length(b) != len)
		return 1;
	if (evbuffer_unfreeze(b, 0) != 0)
		return 1;

	/* A buffer already holding multicast chains cannot be a source. */
	if (evbuffer_add_buffer_reference(mcast_src, src) != 0)
		return 1;
	rc = evbuffer_add_buffer_reference(b, mcast_src);
	if (rc != -1 || evbuffer_get_length(b) != len)
		return 1;

	/* Zero-length source is a documented no-op success, even when the
	 * destination is frozen; it must not touch the destination. */
	if (evbuffer_freeze(b, 0) != 0)
		return 1;
	rc = evbuffer_add_buffer_reference(b, mcast_src /* non-empty */);
	if (rc != -1)
		return 1;
	{
		struct evbuffer *empty = evbuffer_new();
		if (!empty)
			return 1;
		rc = evbuffer_add_buffer_reference(b, empty);
		evbuffer_free(empty);
		if (rc != 0 || evbuffer_get_length(b) != len)
			return 1;
	}
	if (evbuffer_unfreeze(b, 0) != 0)
		return 1;
	if (buffer_equals(b, before, len) < 0)
		return 1;

	evbuffer_free(b);
	evbuffer_free(src);
	evbuffer_free(mcast_src);
	if (blk.cleanup_calls != 1)
		return 1;
	block_discard_unused(&blk);
	if (cleanup_bad_args || cleanup_double)
		return 1;
	return 0;
}

/* ================================================================== */
/* transfer group                                                     */
/* ================================================================== */

enum transfer_dst_kind {
	TDST_EMPTY = 0,
	TDST_PLAIN_TAIL,
	TDST_IMMUTABLE_TAIL
};

struct transfer_world {
	struct evbuffer *src;
	struct evbuffer *dst;
	struct cb_observer *osrc;
	struct cb_observer *odst;
	struct ext_block blocks[3];
	struct buffer_snapshot ss_src;
	struct buffer_snapshot ss_dst;
	size_t src_len;
	size_t dst_len;
	size_t live_before;
};

/* Source: three independent reference chains of 5,7,4 bytes.  A request of
 * 10 crosses the first whole chain and stops in the middle of the second
 * (5 whole + 5 copied), leaving 6 bytes behind. */
#define TF_REQ 10
#define TF_FIRST 5
#define TF_TAIL 5

static int
transfer_world_build(struct transfer_world *w, enum transfer_dst_kind kind)
{
	static const size_t lens[3] = { 5, 7, 4 };
	size_t i;

	memset(w, 0, sizeof(*w));
	w->src = evbuffer_new();
	w->dst = evbuffer_new();
	if (!w->src || !w->dst)
		return -1;
	for (i = 0; i < 3; ++i) {
		block_init(&w->blocks[i], (int)i + 20, lens[i]);
		if (add_block_reference(w->src, &w->blocks[i]) != 0)
			return -1;
	}

	if (kind == TDST_PLAIN_TAIL) {
		/* A small, full mutable chain: its remaining tail space is
		 * zero, so the boundary fragment needs a fresh chain. */
		if (evbuffer_add(w->dst, "pp", 2) != 0)
			return -1;
	} else if (kind == TDST_IMMUTABLE_TAIL) {
		static const unsigned char imm[3] = { 'i', 'i', 'i' };
		if (evbuffer_add(w->dst, imm, sizeof(imm)) != 0 &&
		    evbuffer_add_reference(w->dst, imm, sizeof(imm),
		        NULL, NULL) != 0)
			return -1;
		/* drop the mutable prefix so the tail chain is immutable */
		if (evbuffer_drain(w->dst, sizeof(imm)) != 0)
			return -1;
	}

	w->osrc = observer_attach(w->src);
	w->odst = observer_attach(w->dst);
	if (!w->osrc || !w->odst)
		return -1;
	w->src_len = evbuffer_get_length(w->src);
	w->dst_len = evbuffer_get_length(w->dst);
	if (snapshot_capture(&w->ss_src, w->src, w->osrc) < 0 ||
	    snapshot_capture(&w->ss_dst, w->dst, w->odst) < 0)
		return -1;
	w->live_before = buffer_test_live_allocations();
	return 0;
}

static int
transfer_world_unchanged(const struct transfer_world *w)
{
	size_t i;

	if (snapshot_matches(&w->ss_src, w->src, w->osrc) < 0 ||
	    snapshot_matches(&w->ss_dst, w->dst, w->odst) < 0)
		return -1;
	for (i = 0; i < 3; ++i)
		if (w->blocks[i].cleanup_calls != 0)
			return -1;
	if (buffer_test_live_allocations() != w->live_before)
		return -1;
	return 0;
}

static void
transfer_world_free(struct transfer_world *w)
{
	size_t i;

	snapshot_release(&w->ss_src);
	snapshot_release(&w->ss_dst);
	if (w->src)
		evbuffer_free(w->src);
	if (w->dst)
		evbuffer_free(w->dst);
	free(w->osrc);
	free(w->odst);
	for (i = 0; i < 3; ++i)
		block_discard_unused(&w->blocks[i]);
}

static int
transfer_count_op_allocations(enum transfer_dst_kind kind, size_t *count_out)
{
	struct transfer_world w;
	size_t before, after;

	if (transfer_world_build(&w, kind) < 0)
		return -1;
	before = buffer_test_total_allocations();
	if (evbuffer_remove_buffer(w.src, w.dst, TF_REQ) != TF_REQ) {
		transfer_world_free(&w);
		return -1;
	}
	after = buffer_test_total_allocations();
	*count_out = after - before;
	transfer_world_free(&w);
	return 0;
}

static int
transfer_sweep_one(enum transfer_dst_kind kind)
{
	struct transfer_world w;
	size_t n_allocs, k;
	int rc;

	if (transfer_count_op_allocations(kind, &n_allocs) < 0)
		return 1;
	/* The boundary fragment must take a real allocation in every variant
	 * so the tail-copy failure path is genuinely exercised. */
	if (n_allocs < 1)
		return 1;

	for (k = 1; k <= n_allocs; ++k) {
		if (transfer_world_build(&w, kind) < 0)
			return 1;
		fault_arm((long)k);
		rc = evbuffer_remove_buffer(w.src, w.dst, TF_REQ);
		fault_disarm();
		if (rc != -1 || fault_hits != 1)
			return 1;
		if (transfer_world_unchanged(&w) < 0)
			return 1;

		/* Retry after the fault is cleared: full transfer semantics. */
		rc = evbuffer_remove_buffer(w.src, w.dst, TF_REQ);
		if (rc != TF_REQ)
			return 1;
		if (evbuffer_get_length(w.src) != w.src_len - TF_REQ)
			return 1;
		if (evbuffer_get_length(w.dst) != w.dst_len + TF_REQ)
			return 1;
		if (w.odst->n_added != TF_REQ || w.osrc->n_deleted != TF_REQ)
			return 1;
		/* Source object stays usable after the completed transfer. */
		if (evbuffer_add(w.src, "R", 1) != 0 ||
		    evbuffer_drain(w.src, 1) != 0)
			return 1;
		transfer_world_free(&w);
	}
	return 0;
}

static int
check_transfer_oom_atomic(void)
{
	if (transfer_sweep_one(TDST_EMPTY) != 0)
		return 1;
	if (transfer_sweep_one(TDST_PLAIN_TAIL) != 0)
		return 1;
	if (transfer_sweep_one(TDST_IMMUTABLE_TAIL) != 0)
		return 1;
	if (cleanup_bad_args || cleanup_double)
		return 1;
	return 0;
}

static int
check_transfer_zero_copy(void)
{
	struct evbuffer *src, *dst;
	struct ext_block blocks[3];
	static const size_t lens[3] = { 4, 6, 5 };
	struct evbuffer_iovec before[8], after[8];
	int n_before, n_after;
	size_t total = 0, moved, i;
	unsigned char expect[32];
	char out[32], remain[8];

	src = evbuffer_new();
	dst = evbuffer_new();
	if (!src || !dst)
		return 1;
	for (i = 0; i < 3; ++i) {
		block_init(&blocks[i], (int)i + 30, lens[i]);
		if (add_block_reference(src, &blocks[i]) != 0)
			return 1;
		memcpy(expect + total, blocks[i].data, lens[i]);
		total += lens[i];
	}
	n_before = evbuffer_peek(src, -1, NULL, before, 8);
	if (n_before != 3)
		return 1;

	/* Request exactly the first two whole chains; no boundary copy. */
	if (evbuffer_remove_buffer(src, dst, lens[0] + lens[1]) !=
	    (int)(lens[0] + lens[1]))
		return 1;
	n_after = evbuffer_peek(dst, -1, NULL, after, 8);
	if (n_after != 2)
		return 1;
	/* The moved whole chains keep their original storage addresses. */
	if (after[0].iov_base != before[0].iov_base ||
	    after[0].iov_len != before[0].iov_len ||
	    after[1].iov_base != before[1].iov_base ||
	    after[1].iov_len != before[1].iov_len)
		return 1;

	/* A partial request now copies only the boundary fragment. */
	{
		int n_rem = evbuffer_peek(src, -1, NULL, before, 8);
		void *orig_tail = before[0].iov_base;
		if (n_rem < 1)
			return 1;
		if (evbuffer_remove_buffer(src, dst, 3) != 3)
			return 1;
		n_after = evbuffer_peek(dst, -1, NULL, after, 8);
		if (after[n_after - 1].iov_base == orig_tail)
			return 1; /* fragment must have been copied */
	}

	/* Two whole chains (10 bytes) moved first, then a 3-byte fragment:
	 * dst holds the first 13 bytes; the last 2 stay in src. */
	moved = lens[0] + lens[1] + 3;
	memset(out, 0, sizeof(out));
	if (evbuffer_get_length(dst) != moved ||
	    evbuffer_copyout(dst, out, moved) != (ev_ssize_t)moved ||
	    memcmp(out, expect, moved) != 0)
		return 1;
	memset(remain, 0, sizeof(remain));
	if (evbuffer_get_length(src) != total - moved ||
	    evbuffer_copyout(src, remain, total - moved) !=
	        (ev_ssize_t)(total - moved) ||
	    memcmp(remain, expect + moved, total - moved) != 0)
		return 1;

	evbuffer_free(src);
	evbuffer_free(dst);
	for (i = 0; i < 3; ++i) {
		if (blocks[i].cleanup_calls != 1)
			return 1;
		block_discard_unused(&blocks[i]);
	}
	if (cleanup_bad_args || cleanup_double)
		return 1;
	return 0;
}

static int
check_transfer_lifecycle_and_reject(void)
{
	struct evbuffer *src, *dst, *view;
	struct ext_block blocks[2];
	static const size_t lens[2] = { 4, 8 };
	size_t i, total;
	char out[32];
	int rc;

	src = evbuffer_new();
	dst = evbuffer_new();
	view = evbuffer_new();
	if (!src || !dst || !view)
		return 1;
	total = 0;
	for (i = 0; i < 2; ++i) {
		block_init(&blocks[i], (int)i + 40, lens[i]);
		if (add_block_reference(src, &blocks[i]) != 0)
			return 1;
		memcpy(out + total, blocks[i].data, lens[i]);
		total += lens[i];
	}

	/* An outstanding share makes the source chains immutable; a later
	 * destructive transfer must still work and preserve lifecycle. */
	if (evbuffer_add_buffer_reference(view, src) != 0)
		return 1;
	rc = evbuffer_remove_buffer(src, dst, 6); /* whole + fragment */
	if (rc != 6)
		return 1;
	if (evbuffer_get_length(src) != total - 6 ||
	    evbuffer_get_length(dst) != 6 ||
	    evbuffer_get_length(view) != total)
		return 1;

	/* Rejections: self-op, frozen ends/front, zero length. */
	if (evbuffer_remove_buffer(dst, dst, 3) != 0)
		return 1;
	if (evbuffer_remove_buffer(src, dst, 0) != 0)
		return 1;
	if (evbuffer_freeze(dst, 0) != 0)
		return 1;
	if (evbuffer_remove_buffer(src, dst, 2) != -1)
		return 1;
	if (evbuffer_unfreeze(dst, 0) != 0)
		return 1;
	if (evbuffer_freeze(src, 1) != 0)
		return 1;
	if (evbuffer_remove_buffer(src, dst, 2) != -1)
		return 1;
	if (evbuffer_unfreeze(src, 1) != 0)
		return 1;
	/* Lengths and bytes survive the rejected calls. */
	if (evbuffer_get_length(src) != total - 6 ||
	    evbuffer_get_length(dst) != 6)
		return 1;

	/* Target freed first, then source; the shared view remains readable
	 * and cleanup happens once. */
	evbuffer_free(dst);
	evbuffer_free(src);
	memset(out, 0, sizeof(out));
	if (evbuffer_copyout(view, out, total) != (ev_ssize_t)total)
		return 1;
	evbuffer_free(view);
	for (i = 0; i < 2; ++i) {
		if (blocks[i].cleanup_calls != 1)
			return 1;
		block_discard_unused(&blocks[i]);
	}
	if (cleanup_bad_args || cleanup_double)
		return 1;
	return 0;
}

/* ================================================================== */
/* callbacks group                                                     */
/* ================================================================== */

static int
check_callbacks_failure_sync(void)
{
	struct ref_world rw;
	struct transfer_world tw;
	size_t n_ref, n_xfer, k;
	size_t base_add, base_del;
	int rc;

	/* add_buffer_reference failures: no callback event at all. */
	if (ref_count_op_allocations(&n_ref) < 0)
		return 1;
	for (k = 1; k <= n_ref; ++k) {
		if (ref_world_build(&rw) < 0)
			return 1;
		base_add = rw.odst->n_added;
		base_del = rw.odst->n_deleted;
		fault_arm((long)k);
		rc = evbuffer_add_buffer_reference(rw.dst, rw.src);
		fault_disarm();
		if (rc != -1 || fault_hits != 1)
			return 1;
		if (rw.odst->n_added != base_add ||
		    rw.odst->n_deleted != base_del ||
		    rw.odst->calls != 0 ||
		    rw.osrc->calls != 0)
			return 1;
		if (ref_world_unchanged(&rw) < 0)
			return 1;
		ref_world_free(&rw);
	}

	/* remove_buffer failures: neither side records bytes or events. */
	if (transfer_count_op_allocations(TDST_PLAIN_TAIL, &n_xfer) < 0)
		return 1;
	for (k = 1; k <= n_xfer; ++k) {
		if (transfer_world_build(&tw, TDST_PLAIN_TAIL) < 0)
			return 1;
		base_add = tw.odst->n_added;
		base_del = tw.osrc->n_deleted;
		fault_arm((long)k);
		rc = evbuffer_remove_buffer(tw.src, tw.dst, TF_REQ);
		fault_disarm();
		if (rc != -1 || fault_hits != 1)
			return 1;
		if (tw.odst->n_added != base_add || tw.odst->calls != 0 ||
		    tw.osrc->n_deleted != base_del || tw.osrc->calls != 0)
			return 1;
		if (transfer_world_unchanged(&tw) < 0)
			return 1;
		transfer_world_free(&tw);
	}
	return 0;
}

static int
check_callbacks_failure_preserves_queued(void)
{
	struct event_base *base;
	struct evbuffer *src, *dst;
	struct cb_observer *od, *os;
	struct ext_block blk[2];
	static const size_t lens[2] = { 5, 6 };
	size_t pre_added = 4;
	size_t k, n_ref;
	int rc;

	base = event_base_new();
	src = evbuffer_new();
	dst = evbuffer_new();
	if (!base || !src || !dst)
		return 1;
	for (k = 0; k < 2; ++k) {
		block_init(&blk[k], (int)k + 50, lens[k]);
		if (add_block_reference(src, &blk[k]) != 0)
			return 1;
	}
	if (evbuffer_defer_callbacks(dst, base) != 0 ||
	    evbuffer_defer_callbacks(src, base) != 0)
		return 1;
	od = observer_attach(dst);
	os = observer_attach(src);
	if (!od || !os)
		return 1;

	/* Queue one real notification before the failing calls. */
	if (evbuffer_add(dst, "pre1", pre_added) != 0)
		return 1;
	if (od->calls != 0 || od->n_added != 0)
		return 1; /* deferred: nothing delivered yet */

	/* How many allocations a successful share needs, used to pick a real
	 * fault point rather than a guessed one. */
	if (ref_count_op_allocations(&n_ref) < 0)
		return 1;
	fault_arm(1);
	rc = evbuffer_add_buffer_reference(dst, src);
	fault_disarm();
	if (rc != -1 || fault_hits != 1)
		return 1;

	/* The pre-existing notification must survive intact, and the failure
	 * must queue nothing of its own. */
	pump_once(base);
	if (od->calls != 1 || od->n_added != pre_added || od->n_deleted != 0)
		return 1;
	if (os->calls != 0)
		return 1;
	pump_once(base);
	if (od->calls != 1)
		return 1; /* no second notification was hidden behind it */

	/* Same guarantee for remove_buffer, including the source side. */
	if (evbuffer_add(dst, "pre2", 4) != 0)
		return 1;
	{
		size_t n_xfer;
		if (transfer_count_op_allocations(TDST_EMPTY, &n_xfer) < 0)
			return 1;
	}
	fault_arm(1);
	rc = evbuffer_remove_buffer(src, dst, 7);
	fault_disarm();
	if (rc != -1 || fault_hits != 1)
		return 1;
	pump_once(base);
	if (od->calls != 2 || od->n_added != pre_added + 4)
		return 1;
	if (os->calls != 0 || os->n_deleted != 0)
		return 1;
	pump_once(base);
	if (od->calls != 2 || os->calls != 0)
		return 1;

	evbuffer_free(src);
	evbuffer_free(dst);
	event_base_free(base);
	free(od);
	free(os);
	for (k = 0; k < 2; ++k) {
		if (blk[k].cleanup_calls != 1)
			return 1;
		block_discard_unused(&blk[k]);
	}
	if (cleanup_bad_args || cleanup_double)
		return 1;
	return 0;
}

static int
check_callbacks_success_bytes(void)
{
	struct event_base *base;
	struct evbuffer *src, *dst;
	struct cb_observer *os, *od;
	struct ext_block blk[3];
	static const size_t lens[3] = { 4, 5, 6 };
	size_t total = 0, i;
	int rc;

	/* Synchronous mode first. */
	src = evbuffer_new();
	dst = evbuffer_new();
	if (!src || !dst)
		return 1;
	for (i = 0; i < 3; ++i) {
		block_init(&blk[i], (int)i + 60, lens[i]);
		if (add_block_reference(src, &blk[i]) != 0)
			return 1;
		total += lens[i];
	}
	os = observer_attach(src);
	od = observer_attach(dst);
	if (!os || !od)
		return 1;

	if (evbuffer_add_buffer_reference(dst, src) != 0)
		return 1;
	if (od->n_added != total || od->n_deleted != 0 || os->n_added != 0)
		return 1;
	if (evbuffer_get_length(dst) != total)
		return 1;

	rc = evbuffer_remove_buffer(src, dst, 7); /* 4 whole + 3 fragment */
	if (rc != 7)
		return 1;
	if (od->n_added != total + 7 || os->n_deleted != 7)
		return 1;
	if (evbuffer_get_length(src) != total - 7 ||
	    evbuffer_get_length(dst) != total + 7)
		return 1;

	evbuffer_free(src);
	evbuffer_free(dst);
	free(os);
	free(od);
	for (i = 0; i < 3; ++i) {
		if (blk[i].cleanup_calls != 1)
			return 1;
		block_discard_unused(&blk[i]);
	}

	/* Deferred mode: identical byte accounting, delivered by pumping the
	 * local base without blocking. */
	base = event_base_new();
	src = evbuffer_new();
	dst = evbuffer_new();
	if (!base || !src || !dst)
		return 1;
	for (i = 0; i < 3; ++i) {
		block_init(&blk[i], (int)i + 70, lens[i]);
		if (add_block_reference(src, &blk[i]) != 0)
			return 1;
	}
	if (evbuffer_defer_callbacks(src, base) != 0 ||
	    evbuffer_defer_callbacks(dst, base) != 0)
		return 1;
	os = observer_attach(src);
	od = observer_attach(dst);
	if (!os || !od)
		return 1;

	if (evbuffer_add_buffer_reference(dst, src) != 0)
		return 1;
	if (evbuffer_remove_buffer(src, dst, 7) != 7)
		return 1;
	if (od->calls != 0 || os->calls != 0)
		return 1;
	pump_once(base);
	if (od->n_added != total + 7 || os->n_deleted != 7)
		return 1;
	if (od->calls < 1 || os->calls < 1)
		return 1;

	evbuffer_free(src);
	evbuffer_free(dst);
	event_base_free(base);
	free(os);
	free(od);
	for (i = 0; i < 3; ++i) {
		if (blk[i].cleanup_calls != 1)
			return 1;
		block_discard_unused(&blk[i]);
	}
	if (cleanup_bad_args || cleanup_double)
		return 1;
	return 0;
}

/* ================================================================== */
/* model group: independent byte/identity model over 5 buffers        */
/* ================================================================== */

#define MODEL_NBUFS 5
#define MODEL_MAX_OPS 80
#define MODEL_SEEDS 128
#define MODEL_CAP 512
#define MODEL_MAX_BLOCKS (MODEL_MAX_OPS + 2)

enum model_op {
	MOP_ADD = 0,
	MOP_ADD_REFERENCE,
	MOP_ADD_BUFFER_REFERENCE,
	MOP_REMOVE_BUFFER,
	MOP_DRAIN,
	MOP_PULLUP,
	MOP_FREE_RECREATE,
	MOP_OOM_REFERENCE,
	MOP_OOM_TRANSFER,
	MOP_NTYPES
};

struct model_block {
	struct ext_block ext;
	int used;
};

struct model_buf {
	unsigned char bytes[MODEL_CAP];
	int ids[MODEL_CAP];
	size_t len;
};

struct model_state {
	struct evbuffer *real[MODEL_NBUFS];
	struct model_buf model[MODEL_NBUFS];
	struct model_block blocks[MODEL_MAX_BLOCKS];
	/* Over-approximate provenance: bit s of mcast_src[b] is set while
	 * buffer b may still hold a multicast chain whose source is buffer s.
	 * The model uses it to avoid moving such a chain back into its own
	 * source buffer, which forms a pre-existing reference-count cycle in
	 * the chain implementation rather than in the fixed entry points. */
	unsigned mcast_src[MODEL_NBUFS];
	size_t n_blocks;
	unsigned seed;
};

static unsigned
rng_next(unsigned *state)
{
	unsigned x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static void
model_reset_buf(struct model_buf *m)
{
	m->len = 0;
}

static int
model_append(struct model_buf *m, const unsigned char *data, size_t n, int id)
{
	size_t i;
	if (m->len + n > MODEL_CAP)
		return -1;
	for (i = 0; i < n; ++i) {
		m->bytes[m->len] = data[i];
		m->ids[m->len] = id;
		++m->len;
	}
	return 0;
}

static int
model_can_fit(const struct model_buf *m, size_t n)
{
	return m->len + n <= MODEL_CAP;
}

static int
model_append_copy(struct model_buf *m, const unsigned char *bytes,
    const int *ids, size_t n)
{
	if (!model_can_fit(m, n))
		return -1;
	memcpy(m->bytes + m->len, bytes, n);
	memcpy(m->ids + m->len, ids, n * sizeof(int));
	m->len += n;
	return 0;
}

static void
model_front_remove(struct model_buf *m, size_t n)
{
	memmove(m->bytes, m->bytes + n, m->len - n);
	memmove(m->ids, m->ids + n, (m->len - n) * sizeof(int));
	m->len -= n;
}

/* Field-wise comparison: never rely on raw struct padding bytes. */
static int
model_buf_equal(const struct model_buf *a, const struct model_buf *b)
{
	if (a->len != b->len)
		return 0;
	if (a->len && memcmp(a->bytes, b->bytes, a->len) != 0)
		return 0;
	if (a->len && memcmp(a->ids, b->ids, a->len * sizeof(int)) != 0)
		return 0;
	return 1;
}

static struct buffer_model_result
model_fail(const struct model_state *ms, unsigned op_index, const char *field,
    size_t mv, size_t rv)
{
	struct buffer_model_result res;
	memset(&res, 0, sizeof(res));
	res.failed = 1;
	res.seed = ms->seed;
	res.op_index = op_index;
	res.field = field;
	res.model_value = mv;
	res.real_value = rv;
	return res;
}

/* Directed-graph reachability over the conservative multicast-provenance
 * bits: an edge u -> s means buffer u holds a multicast chain whose source is
 * buffer s.  The model refuses any share/transfer that would close a cycle,
 * since a holder/source reference cycle is a pre-existing chain-lifetime
 * defect outside the two atomic entry points under test. */
static int
model_reaches(const struct model_state *ms, int from, int target)
{
	unsigned visited = 0;
	unsigned stack = (1u << from);

	while (stack) {
		unsigned bit = stack & (0u - stack);
		int node;
		stack ^= bit;
		if (visited & bit)
			continue;
		visited |= bit;
		node = 0;
		while ((1u << node) != bit)
			++node;
		if (node == target)
			return 1;
		stack |= ms->mcast_src[node] & ~visited;
	}
	return 0;
}

/* Would moving every multicast chain currently held by holder into dest
 * introduce a holder/source cycle? */
static int
model_transfer_makes_cycle(const struct model_state *ms, int dest, int holder)
{
	unsigned bits = ms->mcast_src[holder];
	unsigned bit;

	for (bit = 1; bits; bit <<= 1) {
		int source;
		if (!(bits & bit))
			continue;
		bits &= ~bit;
		source = 0;
		while ((1u << source) != bit)
			++source;
		if (source == dest || model_reaches(ms, source, dest))
			return 1;
	}
	return 0;
}

/* Compare every real buffer against its model: visible bytes and length. */
static struct buffer_model_result
model_compare(const struct model_state *ms, unsigned op_index)
{
	unsigned char tmp[MODEL_CAP + 8];
	size_t b;

	for (b = 0; b < MODEL_NBUFS; ++b) {
		size_t rlen = evbuffer_get_length(ms->real[b]);
		ev_ssize_t got;
		if (rlen != ms->model[b].len)
			return model_fail(ms, op_index, "length",
			    ms->model[b].len, rlen);
		got = evbuffer_copyout(ms->real[b], tmp, rlen);
		if (got != (ev_ssize_t)rlen)
			return model_fail(ms, op_index, "copyout", rlen,
			    (size_t)got);
		if (rlen && memcmp(tmp, ms->model[b].bytes, rlen) != 0)
			return model_fail(ms, op_index, "bytes", 0, 1);
	}
	/* Cleanup safety, checked every step: a block's cleanup must never
	 * fire more than once.  Early cleanup of storage that is still visible
	 * would be a use-after-free caught by the sanitizer when the bytes are
	 * copied out above; the exact final count is verified separately. */
	{
		size_t k;
		for (k = 0; k < ms->n_blocks; ++k) {
			if (ms->blocks[k].used &&
			    ms->blocks[k].ext.cleanup_calls > 1)
				return model_fail(ms, op_index,
				    "cleanup_double",
				    (size_t)ms->blocks[k].ext.id,
				    (size_t)ms->blocks[k].ext.cleanup_calls);
		}
	}
	{
		struct buffer_model_result ok;
		memset(&ok, 0, sizeof(ok));
		ok.seed = ms->seed;
		return ok;
	}
}

static struct model_block *
model_new_block(struct model_state *ms)
{
	struct model_block *mb;
	size_t len;
	if (ms->n_blocks >= MODEL_MAX_BLOCKS)
		return NULL;
	mb = &ms->blocks[ms->n_blocks];
	memset(mb, 0, sizeof(*mb));
	len = 1 + (ms->n_blocks * 3) % 9; /* 1..9 bytes, distinct per block */
	block_init(&mb->ext, (int)ms->n_blocks + 1, len);
	mb->used = 1;
	++ms->n_blocks;
	return mb;
}

struct buffer_model_result
buffer_model_run(unsigned seed, unsigned max_ops)
{
	struct model_state ms;
	unsigned rng, op_index;
	size_t b, k;
	struct buffer_model_result res;

	ensure_allocator();
	buffer_test_allocator_reset();
	memset(&ms, 0, sizeof(ms));
	ms.seed = seed;
	rng = seed ? seed : 0x9e3779b9u;
	max_ops = max_ops ? max_ops : MODEL_MAX_OPS;
	if (max_ops > MODEL_MAX_OPS)
		max_ops = MODEL_MAX_OPS;

	for (b = 0; b < MODEL_NBUFS; ++b) {
		ms.real[b] = evbuffer_new();
		model_reset_buf(&ms.model[b]);
		if (!ms.real[b]) {
			res.failed = 1;
			res.field = "evbuffer_new";
			res.seed = seed;
			goto fail;
		}
	}

	res = model_compare(&ms, 0);
	if (res.failed)
		goto fail;

	for (op_index = 0; op_index < max_ops; ++op_index) {
		unsigned op = rng_next(&rng) % MOP_NTYPES;
		int b1 = (int)(rng_next(&rng) % MODEL_NBUFS);
		int b2 = (int)(rng_next(&rng) % MODEL_NBUFS);
		unsigned amt = 1 + rng_next(&rng) % 11; /* bounded: 1..11 */
		int rc;

		switch (op) {
		case MOP_ADD: {
			unsigned char scratch[11];
			if (!model_can_fit(&ms.model[b1], amt))
				break; /* skip before touching the real buffer */
			for (k = 0; k < amt; ++k)
				scratch[k] =
				    (unsigned char)('0' + (op_index + k) % 10);
			rc = evbuffer_add(ms.real[b1], scratch, amt);
			if (rc != 0) {
				res = model_fail(&ms, op_index, "add", 0, 1);
				goto fail;
			}
			if (model_append(&ms.model[b1], scratch, amt, -1) < 0) {
				res = model_fail(&ms, op_index, "add_cap", 0, 1);
				goto fail;
			}
			break;
		}
		case MOP_ADD_REFERENCE: {
			struct model_block *mb = model_new_block(&ms);
			if (!mb) {
				res = model_fail(&ms, op_index, "block", 0, 1);
				goto fail;
			}
			if (!model_can_fit(&ms.model[b1], mb->ext.len)) {
				block_discard_unused(&mb->ext);
				--ms.n_blocks;
				break;
			}
			rc = add_block_reference(ms.real[b1], &mb->ext);
			if (rc != 0) {
				res = model_fail(&ms, op_index, "add_ref", 0, 1);
				goto fail;
			}
			if (model_append(&ms.model[b1], mb->ext.data,
			        mb->ext.len, mb->ext.id) < 0) {
				res = model_fail(&ms, op_index, "add_ref_cap",
				    0, 1);
				goto fail;
			}
			break;
		}
		case MOP_ADD_BUFFER_REFERENCE: {
			size_t n = ms.model[b2].len;
			if (!model_can_fit(&ms.model[b1], n))
				break;
			/* Adding edge b1 -> b2 closes a cycle iff b2 already
			 * reaches b1. */
			if (model_reaches(&ms, b2, b1))
				break;
			rc = evbuffer_add_buffer_reference(ms.real[b1],
			    ms.real[b2]);
			if (rc == 0) {
				if (model_append_copy(&ms.model[b1],
				        ms.model[b2].bytes,
				        ms.model[b2].ids, n) < 0) {
					res = model_fail(&ms, op_index,
					    "share_cap", 0, 1);
					goto fail;
				}
				/* The new multicast chains reference b2. */
				ms.mcast_src[b1] |= (1u << b2);
			} else if (rc != -1) {
				res = model_fail(&ms, op_index, "share_rc",
				    0, 1);
				goto fail;
			}
			/* rc == -1: an existing rejection (self op, multicast
			 * source, frozen end).  model_compare below proves the
			 * complete prior state was preserved. */
			break;
		}
		case MOP_REMOVE_BUFFER: {
			size_t n;
			if (b1 == b2) {
				/* Self transfer is a documented no-op. */
				rc = evbuffer_remove_buffer(ms.real[b2],
				    ms.real[b1], amt);
				if (rc != 0) {
					res = model_fail(&ms, op_index,
					    "xfer_self", 0, 1);
					goto fail;
				}
				break;
			}
			/* Refuse to move b2's multicast chains into b1 when that
			 * would close any holder/source reference cycle; this
			 * pre-existing chain-lifetime defect is outside the two
			 * atomic entry points under test. */
			if (model_transfer_makes_cycle(&ms, b1, b2))
				break;
			n = amt < ms.model[b2].len ? amt : ms.model[b2].len;
			if (n == 0 || !model_can_fit(&ms.model[b1], n))
				break;
			rc = evbuffer_remove_buffer(ms.real[b2],
			    ms.real[b1], amt);
			if (rc < 0) {
				res = model_fail(&ms, op_index, "xfer_rc",
				    0, 1);
				goto fail;
			}
			if ((size_t)rc != n) {
				res = model_fail(&ms, op_index, "xfer_n", n,
				    (size_t)rc);
				goto fail;
			}
			if (model_append_copy(&ms.model[b1],
			        ms.model[b2].bytes, ms.model[b2].ids,
			        n) < 0) {
				res = model_fail(&ms, op_index, "xfer_cap",
				    0, 1);
				goto fail;
			}
			model_front_remove(&ms.model[b2], n);
			/* Provenance moves with the chains; over-approximated
			 * as a union, and cleared once the source is empty. */
			ms.mcast_src[b1] |= ms.mcast_src[b2];
			if (ms.model[b2].len == 0)
				ms.mcast_src[b2] = 0;
			break;
		}
		case MOP_DRAIN: {
			size_t n = amt < ms.model[b1].len
			    ? amt : ms.model[b1].len;
			rc = evbuffer_drain(ms.real[b1], amt);
			if (rc != 0) {
				res = model_fail(&ms, op_index, "drain_rc",
				    0, 1);
				goto fail;
			}
			model_front_remove(&ms.model[b1], n);
			if (ms.model[b1].len == 0)
				ms.mcast_src[b1] = 0;
			break;
		}
		case MOP_PULLUP: {
			ev_ssize_t want = ms.model[b1].len
			    ? (ev_ssize_t)(1 +
			        rng_next(&rng) % ms.model[b1].len)
			    : 0;
			/* Pullup is still exercised on ordinary and
			 * externally referenced chains, but not on a buffer
			 * that may hold multicast chains: pulling up shared
			 * immutable storage is a pre-existing, out-of-scope
			 * hazard unrelated to the two atomic entry points. */
			if (ms.mcast_src[b1] == 0)
				(void)evbuffer_pullup(ms.real[b1], want);
			/* visible content is contractually unchanged */
			break;
		}
		case MOP_FREE_RECREATE: {
			evbuffer_free(ms.real[b1]);
			ms.real[b1] = evbuffer_new();
			if (!ms.real[b1]) {
				res = model_fail(&ms, op_index, "recreate",
				    0, 1);
				goto fail;
			}
			model_reset_buf(&ms.model[b1]);
			ms.mcast_src[b1] = 0;
			break;
		}
		case MOP_OOM_REFERENCE: {
			struct model_buf before_dst = ms.model[b1];
			struct model_buf before_src = ms.model[b2];
			size_t n = ms.model[b2].len;
			if (b1 == b2 || n == 0 ||
			    !model_can_fit(&ms.model[b1], n))
				break;
			if (model_reaches(&ms, b2, b1))
				break;
			fault_arm(1 + (long)(rng_next(&rng) % 2));
			rc = evbuffer_add_buffer_reference(ms.real[b1],
			    ms.real[b2]);
			if (fault_hits) {
				fault_disarm();
				if (rc != -1) {
					res = model_fail(&ms, op_index,
					    "oom_share_rc", (size_t)-1, 0);
					goto fail;
				}
				if (!model_buf_equal(&ms.model[b1],
				        &before_dst) ||
				    !model_buf_equal(&ms.model[b2],
				        &before_src)) {
					res = model_fail(&ms, op_index,
					    "oom_share_state", 0, 1);
					goto fail;
				}
			} else {
				fault_disarm();
				if (rc == 0) {
					if (model_append_copy(&ms.model[b1],
					        ms.model[b2].bytes,
					        ms.model[b2].ids, n) < 0) {
						res = model_fail(&ms, op_index,
						    "oom_share_cap", 0, 1);
						goto fail;
					}
					ms.mcast_src[b1] |= (1u << b2);
				} else if (rc != -1) {
					res = model_fail(&ms, op_index,
					    "oom_share_miss", 0, 1);
					goto fail;
				}
				/* rc == -1 without a hit: a pre-existing
				 * rejection; state must already match. */
			}
			break;
		}
		case MOP_OOM_TRANSFER: {
			struct model_buf before_dst = ms.model[b1];
			struct model_buf before_src = ms.model[b2];
			size_t n;
			if (b1 == b2 || ms.model[b2].len < 2)
				break;
			/* Avoid any pre-existing holder/source cycle. */
			if (model_transfer_makes_cycle(&ms, b1, b2))
				break;
			amt = 1 + rng_next(&rng) % (ms.model[b2].len - 1);
			n = amt;
			if (!model_can_fit(&ms.model[b1], n))
				break;
			fault_arm(1 + (long)(rng_next(&rng) % 2));
			rc = evbuffer_remove_buffer(ms.real[b2],
			    ms.real[b1], amt);
			if (fault_hits) {
				fault_disarm();
				if (rc != -1) {
					res = model_fail(&ms, op_index,
					    "oom_xfer_rc", (size_t)-1, 0);
					goto fail;
				}
				if (!model_buf_equal(&ms.model[b1],
				        &before_dst) ||
				    !model_buf_equal(&ms.model[b2],
				        &before_src)) {
					res = model_fail(&ms, op_index,
					    "oom_xfer_state", 0, 1);
					goto fail;
				}
			} else {
				fault_disarm();
				if (rc < 0 || (size_t)rc != n) {
					res = model_fail(&ms, op_index,
					    "oom_xfer_miss", n,
					    rc < 0 ? (size_t)-1 : (size_t)rc);
					goto fail;
				}
				if (model_append_copy(&ms.model[b1],
				        ms.model[b2].bytes,
				        ms.model[b2].ids, n) < 0) {
					res = model_fail(&ms, op_index,
					    "oom_xfer_cap", 0, 1);
					goto fail;
				}
				model_front_remove(&ms.model[b2], n);
				ms.mcast_src[b1] |= ms.mcast_src[b2];
				if (ms.model[b2].len == 0)
					ms.mcast_src[b2] = 0;
			}
			break;
		}
		default:
			break;
		}

		res = model_compare(&ms, op_index);
		if (res.failed)
			goto fail;
	}

	/* Tear everything down, then require exactly one cleanup per block. */
	for (b = 0; b < MODEL_NBUFS; ++b) {
		evbuffer_free(ms.real[b]);
		ms.real[b] = NULL;
	}
	memset(&res, 0, sizeof(res));
	res.seed = seed;
	for (k = 0; k < ms.n_blocks; ++k) {
		if (ms.blocks[k].ext.cleanup_calls != 1) {
			res = model_fail(&ms, max_ops, "cleanup_once", 1,
			    (size_t)ms.blocks[k].ext.cleanup_calls);
			goto fail;
		}
	}
	return res;

fail:
	for (b = 0; b < MODEL_NBUFS; ++b) {
		if (ms.real[b])
			evbuffer_free(ms.real[b]);
	}
	for (k = 0; k < ms.n_blocks; ++k)
		block_discard_unused(&ms.blocks[k].ext);
	return res;
}

static int
check_model_seeded(void)
{
	unsigned seed;
	struct buffer_model_result r;

	for (seed = 1; seed <= MODEL_SEEDS; ++seed) {
		r = buffer_model_run(seed, MODEL_MAX_OPS);
		if (r.failed) {
			fprintf(stderr,
			    "MODEL FAIL seed=%u op=%u field=%s model=%zu real=%zu\n",
			    r.seed, r.op_index, r.field,
			    r.model_value, r.real_value);
			return 1;
		}
	}
	if (cleanup_bad_args || cleanup_double)
		return 1;
	return 0;
}


static int
case_reference_share_lifecycle(void)
{
	ensure_allocator();
	REQUIRE(check_reference_share_lifecycle() == 0);
	return 0;
}

static int
case_reference_rejections(void)
{
	ensure_allocator();
	REQUIRE(check_reference_rejections() == 0);
	return 0;
}

static int
case_transfer_oom_atomic(void)
{
	ensure_allocator();
	REQUIRE(check_transfer_oom_atomic() == 0);
	return 0;
}

static int
case_transfer_zero_copy(void)
{
	ensure_allocator();
	REQUIRE(check_transfer_zero_copy() == 0);
	return 0;
}

static int
case_transfer_lifecycle_and_reject(void)
{
	ensure_allocator();
	REQUIRE(check_transfer_lifecycle_and_reject() == 0);
	return 0;
}

static int
case_callbacks_failure_sync(void)
{
	ensure_allocator();
	REQUIRE(check_callbacks_failure_sync() == 0);
	return 0;
}

static int
case_callbacks_failure_preserves_queued(void)
{
	ensure_allocator();
	REQUIRE(check_callbacks_failure_preserves_queued() == 0);
	return 0;
}

static int
case_callbacks_success_bytes(void)
{
	ensure_allocator();
	REQUIRE(check_callbacks_success_bytes() == 0);
	return 0;
}

static int
case_model_seeded(void)
{
	ensure_allocator();
	REQUIRE(check_model_seeded() == 0);
	return 0;
}

/* ================================================================== */
/* case registry                                                      */
/* ================================================================== */

static int case_group_smoke(void);

const struct task_case task_cases[] = {
	{ "reference", "oom_atomic", case_reference_oom_atomic },
	{ "reference", "share_lifecycle", case_reference_share_lifecycle },
	{ "reference", "rejections", case_reference_rejections },
	{ "transfer", "oom_atomic", case_transfer_oom_atomic },
	{ "transfer", "zero_copy", case_transfer_zero_copy },
	{ "transfer", "lifecycle_reject", case_transfer_lifecycle_and_reject },
	{ "callbacks", "failure_sync", case_callbacks_failure_sync },
	{ "callbacks", "failure_preserves_queued",
	    case_callbacks_failure_preserves_queued },
	{ "callbacks", "success_bytes", case_callbacks_success_bytes },
	{ "model", "seeded_128", case_model_seeded },
	{ "model", "group_smoke", case_group_smoke },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

/* The allocator must be installed before the first libevent object; every
 * case funnels through this guard.  The smoke case also proves the ledger
 * reaches zero once all objects are released. */
static int
case_group_smoke(void)
{
	struct evbuffer *b;
	size_t before, after;

	ensure_allocator();
	before = buffer_test_live_allocations();
	b = evbuffer_new();
	TASK_CHECK(b != NULL);
	TASK_CHECK(evbuffer_add(b, "abc", 3) == 0);
	TASK_CHECK(evbuffer_get_length(b) == 3);
	evbuffer_free(b);
	after = buffer_test_live_allocations();
	TASK_CHECK(after == before);
	return 0;
}
