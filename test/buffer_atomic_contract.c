/*
 * Failure-atomicity contract tests for evbuffer_add_buffer_reference() and
 * evbuffer_remove_buffer().
 *
 * Four groups ("reference", "transfer", "callbacks", "model") are registered
 * through task_cases/task_case_count; there is deliberately no main() here.
 * The shared bounded action / checking core lives in buffer_atomic_model.h so
 * that this deterministic driver and the libFuzzer driver run exactly the
 * same model code.
 */

#include "../task_support/check.h"
#include "buffer_atomic_model.h"

/* ------------------------------------------------------------------ */
/* Reference-block registry used by the explicit contract tests        */
/* ------------------------------------------------------------------ */

#define CB_MAX 12

struct cblock {
	unsigned char *ptr;	/* original pointer, never changed */
	size_t len;		/* original length, never changed */
	unsigned char *live;	/* owned memory until cleanup */
	int id;
	int cleanups;
	int bad;
};

static struct cblock cblocks[CB_MAX];
static int cblock_count;

static void
cblock_reset(void)
{
	memset(cblocks, 0, sizeof(cblocks));
	cblock_count = 0;
}

static struct cblock *
cblock_new(const char *bytes, size_t len)
{
	struct cblock *b;

	if (cblock_count >= CB_MAX)
		abort();
	b = &cblocks[cblock_count];
	b->ptr = malloc(len ? len : 1);
	if (!b->ptr)
		abort();
	memcpy(b->ptr, bytes, len);
	b->live = b->ptr;
	b->len = len;
	b->id = cblock_count;
	b->cleanups = 0;
	b->bad = 0;
	++cblock_count;
	return b;
}

static void
cblock_cleanup(const void *data, size_t datalen, void *extra)
{
	struct cblock *b = extra;

	if (b == NULL || data != (const void *)b->ptr || datalen != b->len) {
		if (b)
			b->bad = 1;
		fprintf(stderr,
		    "CTEST FAIL cleanup: pointer/length mismatch\n");
		return;
	}
	if (b->cleanups != 0) {
		b->bad = 1;
		fprintf(stderr, "CTEST FAIL cleanup: block %d twice\n", b->id);
		return;
	}
	b->cleanups = 1;
	free(b->live);
	b->live = NULL;
}

static int
cblock_cleanups(void)
{
	int i, n = 0;

	for (i = 0; i < cblock_count; ++i) {
		if (cblocks[i].bad)
			return -1;
		n += cblocks[i].cleanups;
	}
	return n;
}

static int
cblock_bad(void)
{
	int i;

	for (i = 0; i < cblock_count; ++i)
		if (cblocks[i].bad)
			return 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Snapshot of a buffer: bytes, length and every visible iovec address  */
/* ------------------------------------------------------------------ */

#define VIEW_MAX_VECS 64

struct view {
	size_t len;
	unsigned char *bytes;
	int nvec;
	struct evbuffer_iovec vec[VIEW_MAX_VECS];
};

static void
view_clear(struct view *v)
{
	free(v->bytes);
	memset(v, 0, sizeof(*v));
}

static int
view_capture(struct evbuffer *buf, struct view *v)
{
	struct evbuffer_iovec vec[VIEW_MAX_VECS];
	size_t len;

	memset(v, 0, sizeof(*v));
	len = evbuffer_get_length(buf);
	v->len = len;
	if (len) {
		v->bytes = malloc(len);
		if (!v->bytes)
			abort();
		if ((size_t)evbuffer_copyout(buf, v->bytes, len) != len)
			return -1;
	}
	v->nvec = evbuffer_peek(buf, -1, NULL, vec, VIEW_MAX_VECS);
	if (v->nvec < 0)
		v->nvec = 0;
	if (v->nvec > VIEW_MAX_VECS)
		v->nvec = VIEW_MAX_VECS;
	memcpy(v->vec, vec, sizeof(vec[0]) * (size_t)v->nvec);
	return 0;
}

static int
view_equal(const struct view *a, const struct view *b, const char *what)
{
	int i;

	if (a->len != b->len || a->nvec != b->nvec)
		return 0;
	if (a->len && memcmp(a->bytes, b->bytes, a->len))
		return 0;
	for (i = 0; i < a->nvec; ++i) {
		if (a->vec[i].iov_base != b->vec[i].iov_base)
			return 0;
		if (a->vec[i].iov_len != b->vec[i].iov_len)
			return 0;
	}
	(void)what;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Callback recorder                                                   */
/* ------------------------------------------------------------------ */

struct cb_log {
	int calls;
	size_t added;
	size_t deleted;
	size_t last_added;
	size_t last_deleted;
};

static void
cb_record(struct evbuffer *buf, const struct evbuffer_cb_info *info,
    void *arg)
{
	struct cb_log *log = arg;
	(void)buf;
	log->calls++;
	log->added += info->n_added;
	log->deleted += info->n_deleted;
	log->last_added = info->n_added;
	log->last_deleted = info->n_deleted;
}

static void
cb_log_clear(struct cb_log *log)
{
	memset(log, 0, sizeof(*log));
}

/* ------------------------------------------------------------------ */
/* Common: install allocator once before any libevent object exists    */
/* ------------------------------------------------------------------ */

static int
ensure_harness(void)
{
	atomic_harness_install();
	return 0;
}

/* ------------------------------------------------------------------ */
/* Reference group                                                     */
/* ------------------------------------------------------------------ */

/*
 * Build the reference scenario:
 *   source: three independent external reference blocks (4 bytes each)
 *   t1, t2: two buffers that already share the whole source non-destructively
 *   dst:    a destination that already has a literal prefix and a callback
 * Returns 0 on success.
 */
struct ref_scenario {
	struct evbuffer *src;
	struct evbuffer *t1;
	struct evbuffer *t2;
	struct evbuffer *dst;
	struct cblock *blk[3];
	struct cb_log src_log, t1_log, t2_log, dst_log;
};

static void
ref_scenario_free(struct ref_scenario *sc)
{
	/* Target-first release order: the library may defer block cleanup
	 * through parent-buffer ownership, but the last release must clean
	 * each block exactly once. */
	evbuffer_free(sc->dst);
	evbuffer_free(sc->t1);
	evbuffer_free(sc->t2);
	evbuffer_free(sc->src);
	memset(sc, 0, sizeof(*sc));
}

static int
ref_scenario_build(struct ref_scenario *sc)
{
	static const char *a = "AAAA";
	static const char *b = "BBBB";
	static const char *c = "CCCC";

	memset(sc, 0, sizeof(*sc));
	cblock_reset();
	sc->blk[0] = cblock_new(a, 4);
	sc->blk[1] = cblock_new(b, 4);
	sc->blk[2] = cblock_new(c, 4);

	sc->src = evbuffer_new();
	sc->t1 = evbuffer_new();
	sc->t2 = evbuffer_new();
	sc->dst = evbuffer_new();
	if (!sc->src || !sc->t1 || !sc->t2 || !sc->dst)
		return 1;
	if (evbuffer_add_reference(sc->src, sc->blk[0]->ptr, 4,
	     cblock_cleanup, sc->blk[0]) < 0 ||
	    evbuffer_add_reference(sc->src, sc->blk[1]->ptr, 4,
	     cblock_cleanup, sc->blk[1]) < 0 ||
	    evbuffer_add_reference(sc->src, sc->blk[2]->ptr, 4,
	     cblock_cleanup, sc->blk[2]) < 0)
		return 1;
	if (evbuffer_add_buffer_reference(sc->t1, sc->src) != 0)
		return 1;
	if (evbuffer_add_buffer_reference(sc->t2, sc->src) != 0)
		return 1;
	if (evbuffer_add(sc->dst, "pre", 3) != 0)
		return 1;

	if (!evbuffer_add_cb(sc->src, cb_record, &sc->src_log) ||
	    !evbuffer_add_cb(sc->t1, cb_record, &sc->t1_log) ||
	    !evbuffer_add_cb(sc->t2, cb_record, &sc->t2_log) ||
	    !evbuffer_add_cb(sc->dst, cb_record, &sc->dst_log))
		return 1;
	return 0;
}

/* Count allocations of one successful add_buffer_reference() on a scenario
 * equivalent to the one built above. */
static long
ref_measure_allocations(void)
{
	struct ref_scenario sc;
	long n;

	if (ref_scenario_build(&sc))
		return -1;
	atomic_count_begin();
	if (evbuffer_add_buffer_reference(sc.dst, sc.src) != 0) {
		atomic_count_end();
		ref_scenario_free(&sc);
		return -1;
	}
	n = atomic_count_end();
	/* Tear down; snapshot-free run only measures allocations. */
	ref_scenario_free(&sc);
	return n;
}

static int
case_reference_oom_sweep(void)
{
	long total, fail_at;

	if (ensure_harness())
		return 1;
	atomic_fault_off();
	if (atomic_ledger_outstanding() != 0) {
		TASK_CHECK(0);
		return 1;
	}

	total = ref_measure_allocations();
	TASK_CHECK(total > 0);
	if (total <= 0)
		return 1;
	TASK_CHECK(atomic_double_free == 0 && atomic_bad_pointer == 0);
	TASK_CHECK(cblock_cleanups() >= 0);

	for (fail_at = 1; fail_at <= total; ++fail_at) {
		struct ref_scenario sc;
		struct view vsrc_before, vt1_before, vt2_before, vdst_before;
		struct view vsrc_after, vt1_after, vt2_after, vdst_after;
		int rc, hit;

		if (ref_scenario_build(&sc)) {
			TASK_CHECK(0);
			return 1;
		}
		if (view_capture(sc.src, &vsrc_before) ||
		    view_capture(sc.t1, &vt1_before) ||
		    view_capture(sc.t2, &vt2_before) ||
		    view_capture(sc.dst, &vdst_before)) {
			TASK_CHECK(0);
			return 1;
		}
		cb_log_clear(&sc.src_log);
		cb_log_clear(&sc.t1_log);
		cb_log_clear(&sc.t2_log);
		cb_log_clear(&sc.dst_log);

		atomic_fail_begin(fail_at);
		rc = evbuffer_add_buffer_reference(sc.dst, sc.src);
		hit = atomic_fault_hit;
		atomic_fail_end();

		TASK_CHECK(hit);
		TASK_CHECK(rc == -1);
		TASK_CHECK(cblock_cleanups() == 0);

		if (view_capture(sc.src, &vsrc_after) ||
		    view_capture(sc.t1, &vt1_after) ||
		    view_capture(sc.t2, &vt2_after) ||
		    view_capture(sc.dst, &vdst_after)) {
			TASK_CHECK(0);
			return 1;
		}
		TASK_CHECK(view_equal(&vsrc_before, &vsrc_after, "source"));
		TASK_CHECK(view_equal(&vt1_before, &vt1_after, "target1"));
		TASK_CHECK(view_equal(&vt2_before, &vt2_after, "target2"));
		TASK_CHECK(view_equal(&vdst_before, &vdst_after, "dest"));
		TASK_CHECK(sc.src_log.calls == 0 && sc.t1_log.calls == 0 &&
		    sc.t2_log.calls == 0 && sc.dst_log.calls == 0);
		TASK_CHECK(sc.src->n_add_for_cb == 0 &&
		    sc.src->n_del_for_cb == 0);
		TASK_CHECK(sc.dst->n_add_for_cb == 0 &&
		    sc.dst->n_del_for_cb == 0);

		/* Now disable the fault and finish the normal operation on the
		 * untouched state. */
		view_clear(&vsrc_before);
		view_clear(&vt1_before);
		view_clear(&vt2_before);
		view_clear(&vdst_before);

		rc = evbuffer_add_buffer_reference(sc.dst, sc.src);
		TASK_CHECK(rc == 0);
		TASK_CHECK(evbuffer_get_length(sc.dst) == 15);
		TASK_CHECK(evbuffer_get_length(sc.src) == 12);
		TASK_CHECK(evbuffer_get_length(sc.t1) == 12);
		TASK_CHECK(evbuffer_get_length(sc.t2) == 12);
		{
			unsigned char out[32];
			TASK_CHECK(evbuffer_copyout(sc.dst, out, 15) == 15);
			TASK_CHECK(memcmp(out, "preAAAABBBBCCCC", 15) == 0);
			TASK_CHECK(evbuffer_copyout(sc.src, out, 12) == 12);
			TASK_CHECK(memcmp(out, "AAAABBBBCCCC", 12) == 0);
		}
		TASK_CHECK(sc.dst_log.calls == 1);
		TASK_CHECK(sc.dst_log.last_added == 12);
		TASK_CHECK(sc.dst_log.last_deleted == 0);
		TASK_CHECK(sc.src_log.calls == 0);
		TASK_CHECK(cblock_cleanups() == 0);

		view_clear(&vsrc_after);
		view_clear(&vt1_after);
		view_clear(&vt2_after);
		view_clear(&vdst_after);

		ref_scenario_free(&sc);
		TASK_CHECK(cblock_cleanups() == 3);
		TASK_CHECK(cblock_bad() == 0);
		TASK_CHECK(atomic_ledger_outstanding() == 0);
		TASK_CHECK(atomic_double_free == 0 && atomic_bad_pointer == 0);
	}

	return 0;
}

static int
case_reference_cleanup_once(void)
{
	struct ref_scenario sc;
	unsigned char out[16];

	if (ensure_harness())
		return 1;
	atomic_fault_off();
	if (ref_scenario_build(&sc)) {
		TASK_CHECK(0);
		return 1;
	}
	TASK_CHECK(evbuffer_get_length(sc.src) == 12);
	TASK_CHECK(evbuffer_get_length(sc.t1) == 12);
	TASK_CHECK(evbuffer_get_length(sc.t2) == 12);

	/* Free the source first; the two already-successful targets and the
	 * destination's prefix must still be correct.  The library may defer
	 * block cleanup because the sharing chains still hold the parent
	 * buffer, which is allowed, but nothing may be cleaned yet while the
	 * targets can still read the bytes. */
	evbuffer_free(sc.src);
	sc.src = NULL;

	TASK_CHECK(evbuffer_copyout(sc.t1, out, 12) == 12);
	TASK_CHECK(memcmp(out, "AAAABBBBCCCC", 12) == 0);
	TASK_CHECK(evbuffer_copyout(sc.t2, out, 12) == 12);
	TASK_CHECK(memcmp(out, "AAAABBBBCCCC", 12) == 0);
	TASK_CHECK(evbuffer_copyout(sc.dst, out, 3) == 3);
	TASK_CHECK(memcmp(out, "pre", 3) == 0);

	/* Releasing one target must not free blocks the other still needs nor
	 * clean any block twice. */
	evbuffer_free(sc.t1);
	sc.t1 = NULL;
	TASK_CHECK(evbuffer_copyout(sc.t2, out, 12) == 12);
	TASK_CHECK(memcmp(out, "AAAABBBBCCCC", 12) == 0);
	TASK_CHECK(cblock_bad() == 0);

	evbuffer_free(sc.t2);
	sc.t2 = NULL;
	evbuffer_free(sc.dst);
	TASK_CHECK(cblock_cleanups() == 3);
	TASK_CHECK(cblock_bad() == 0);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

static int
case_reference_reject_shared_source(void)
{
	struct ref_scenario sc;
	struct view before, after;
	int rc;

	if (ensure_harness())
		return 1;
	atomic_fault_off();
	if (ref_scenario_build(&sc)) {
		TASK_CHECK(0);
		return 1;
	}
	/* t1 already shares src; referencing t1 again must be rejected by
	 * the pre-existing chain-type check and leave everything unchanged. */
	if (view_capture(sc.t1, &before)) {
		TASK_CHECK(0);
		return 1;
	}
	cb_log_clear(&sc.t1_log);
	cb_log_clear(&sc.dst_log);
	rc = evbuffer_add_buffer_reference(sc.dst, sc.t1);
	TASK_CHECK(rc == -1);
	TASK_CHECK(sc.t1_log.calls == 0 && sc.dst_log.calls == 0);
	if (view_capture(sc.t1, &after)) {
		TASK_CHECK(0);
		return 1;
	}
	TASK_CHECK(view_equal(&before, &after, "shared source"));
	TASK_CHECK(evbuffer_get_length(sc.dst) == 3);
	view_clear(&before);
	view_clear(&after);
	ref_scenario_free(&sc);
	TASK_CHECK(cblock_cleanups() == 3);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

static int
case_reference_reject_freeze_and_self(void)
{
	struct ref_scenario sc;
	unsigned char *saved;
	size_t saved_len;
	int rc;

	if (ensure_harness())
		return 1;
	atomic_fault_off();
	if (ref_scenario_build(&sc)) {
		TASK_CHECK(0);
		return 1;
	}
	saved_len = evbuffer_get_length(sc.dst);
	saved = malloc(saved_len);
	if (!saved)
		abort();
	evbuffer_copyout(sc.dst, saved, saved_len);

	evbuffer_freeze(sc.dst, 0);
	rc = evbuffer_add_buffer_reference(sc.dst, sc.src);
	TASK_CHECK(rc == -1);
	evbuffer_unfreeze(sc.dst, 0);
	rc = evbuffer_add_buffer_reference(sc.dst, sc.dst);
	TASK_CHECK(rc == -1);
	TASK_CHECK(evbuffer_get_length(sc.dst) == saved_len);
	{
		unsigned char out[8];
		evbuffer_copyout(sc.dst, out, saved_len);
		TASK_CHECK(memcmp(out, saved, saved_len) == 0);
	}
	TASK_CHECK(sc.dst_log.calls == 0);
	/* The same source can still be referenced normally afterwards. */
	rc = evbuffer_add_buffer_reference(sc.dst, sc.src);
	TASK_CHECK(rc == 0);
	TASK_CHECK(evbuffer_get_length(sc.dst) == saved_len + 12);
	free(saved);
	ref_scenario_free(&sc);
	TASK_CHECK(cblock_cleanups() == 3);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

static int
case_reference_zero_length(void)
{
	struct evbuffer *empty = NULL, *dst = NULL;
	struct cb_log log;

	if (ensure_harness())
		return 1;
	atomic_fault_off();
	cb_log_clear(&log);
	empty = evbuffer_new();
	dst = evbuffer_new();
	if (!empty || !dst) {
		TASK_CHECK(0);
		return 1;
	}
	TASK_CHECK(evbuffer_add(dst, "x", 1) == 0);
	if (!evbuffer_add_cb(dst, cb_record, &log)) {
		TASK_CHECK(0);
		return 1;
	}
	TASK_CHECK(evbuffer_add_buffer_reference(dst, empty) == 0);
	TASK_CHECK(log.calls == 0);
	TASK_CHECK(evbuffer_get_length(dst) == 1);
	evbuffer_free(empty);
	evbuffer_free(dst);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Transfer group                                                      */
/* ------------------------------------------------------------------ */

/*
 * Transfer scenario: a source built from independent reference blocks so that
 * each block is exactly one chain (4 bytes).  The request crosses two full
 * chains and stops in the middle of the third, forcing exactly the tail-copy
 * allocation path.
 */
struct xfer_scenario {
	struct evbuffer *src;
	struct evbuffer *dst;
	struct cblock *tailblk;	/* only for the immutable-tail case */
	struct cblock *blk[3];
	struct view src_before;
	struct cb_log src_log, dst_log;
};

static void
xfer_free(struct xfer_scenario *x)
{
	if (x->dst)
		evbuffer_free(x->dst);
	if (x->src)
		evbuffer_free(x->src);
	memset(x, 0, sizeof(*x));
}

static int
xfer_build(struct xfer_scenario *x, int dst_kind)
{
	static const char *a = "aaaa";
	static const char *b = "bbbb";
	static const char *c = "cccc";

	memset(x, 0, sizeof(*x));
	cblock_reset();
	x->blk[0] = cblock_new(a, 4);
	x->blk[1] = cblock_new(b, 4);
	x->blk[2] = cblock_new(c, 4);
	x->src = evbuffer_new();
	if (!x->src)
		return 1;
	if (evbuffer_add_reference(x->src, x->blk[0]->ptr, 4,
	     cblock_cleanup, x->blk[0]) < 0 ||
	    evbuffer_add_reference(x->src, x->blk[1]->ptr, 4,
	     cblock_cleanup, x->blk[1]) < 0 ||
	    evbuffer_add_reference(x->src, x->blk[2]->ptr, 4,
	     cblock_cleanup, x->blk[2]) < 0)
		return 1;

	x->dst = evbuffer_new();
	if (!x->dst)
		return 1;
	if (dst_kind == 0) {
		/* empty destination */
	} else if (dst_kind == 1) {
		/* normal tail chain */
		if (evbuffer_add(x->dst, "DD", 2) != 0)
			return 1;
	} else {
		/* immutable reference tail chain */
		static const char *t = "TT";
		x->tailblk = cblock_new(t, 2);
		if (evbuffer_add_reference(x->dst, x->tailblk->ptr, 2,
		    cblock_cleanup, x->tailblk) != 0)
			return 1;
	}
	if (!evbuffer_add_cb(x->src, cb_record, &x->src_log) ||
	    !evbuffer_add_cb(x->dst, cb_record, &x->dst_log))
		return 1;
	return 0;
}

static long
xfer_measure_allocations(int dst_kind)
{
	struct xfer_scenario x;
	long n;

	if (xfer_build(&x, dst_kind))
		return -1;
	atomic_count_begin();
	if (evbuffer_remove_buffer(x.src, x.dst, 10) != 10) {
		atomic_count_end();
		xfer_free(&x);
		return -1;
	}
	n = atomic_count_end();
	xfer_free(&x);
	return n;
}

static int
xfer_sweep_one(int dst_kind)
{
	long total, fail_at;

	total = xfer_measure_allocations(dst_kind);
	TASK_CHECK(total > 0);
	if (total <= 0)
		return 1;

	for (fail_at = 1; fail_at <= total; ++fail_at) {
		struct xfer_scenario x;
		struct view src_before, src_after, dst_before, dst_after;
		int rc, hit;

		if (xfer_build(&x, dst_kind)) {
			TASK_CHECK(0);
			return 1;
		}
		if (view_capture(x.src, &src_before) ||
		    view_capture(x.dst, &dst_before)) {
			TASK_CHECK(0);
			return 1;
		}
		cb_log_clear(&x.src_log);
		cb_log_clear(&x.dst_log);
		atomic_fail_begin(fail_at);
		rc = evbuffer_remove_buffer(x.src, x.dst, 10);
		hit = atomic_fault_hit;
		atomic_fail_end();
		TASK_CHECK(hit);
		TASK_CHECK(rc == -1);
		if (view_capture(x.src, &src_after) ||
		    view_capture(x.dst, &dst_after)) {
			TASK_CHECK(0);
			return 1;
		}
		TASK_CHECK(view_equal(&src_before, &src_after, "src"));
		TASK_CHECK(view_equal(&dst_before, &dst_after, "dst"));
		TASK_CHECK(x.src_log.calls == 0 && x.dst_log.calls == 0);
		TASK_CHECK(x.src->n_del_for_cb == 0);
		TASK_CHECK(x.dst->n_add_for_cb == 0);
		view_clear(&src_before);
		view_clear(&dst_before);

		/* Disable the fault and finish normally. */
		rc = evbuffer_remove_buffer(x.src, x.dst, 10);
		TASK_CHECK(rc == 10);
		TASK_CHECK(evbuffer_get_length(x.src) == 2);
		TASK_CHECK(evbuffer_get_length(x.dst) ==
		    (dst_kind == 0 ? 10 : 12));
		TASK_CHECK(x.src_log.calls == 1);
		TASK_CHECK(x.dst_log.calls == 1);
		TASK_CHECK(x.src_log.last_deleted == 10);
		TASK_CHECK(x.dst_log.last_added == 10);
		{
			unsigned char out[16];
			size_t dl = evbuffer_get_length(x.dst);
			TASK_CHECK(evbuffer_copyout(x.dst, out, dl) ==
			    (ev_ssize_t)dl);
			if (dst_kind == 0)
				TASK_CHECK(memcmp(out, "aaaabbbbcc", 10) == 0);
			else if (dst_kind == 1)
				TASK_CHECK(memcmp(out, "DDaaaabbbbcc", 12) ==
				    0);
			else
				TASK_CHECK(memcmp(out, "TTaaaabbbbcc", 12) ==
				    0);
			TASK_CHECK(evbuffer_copyout(x.src, out, 2) == 2);
			TASK_CHECK(memcmp(out, "cc", 2) == 0);
		}
		view_clear(&src_after);
		view_clear(&dst_after);
		xfer_free(&x);
		/* Two blocks were fully moved and one was split; all three
		 * originals must be cleaned exactly once. */
		TASK_CHECK(cblock_bad() == 0);
		TASK_CHECK(atomic_ledger_outstanding() == 0);
	}
	return 0;
}

static int
case_transfer_oom_empty_dest(void)
{
	if (ensure_harness())
		return 1;
	atomic_fault_off();
	if (atomic_ledger_outstanding() != 0) {
		TASK_CHECK(0);
		return 1;
	}
	return xfer_sweep_one(0);
}

static int
case_transfer_oom_normal_tail(void)
{
	if (ensure_harness())
		return 1;
	atomic_fault_off();
	if (atomic_ledger_outstanding() != 0) {
		TASK_CHECK(0);
		return 1;
	}
	return xfer_sweep_one(1);
}

static int
case_transfer_oom_immutable_tail(void)
{
	if (ensure_harness())
		return 1;
	atomic_fault_off();
	if (atomic_ledger_outstanding() != 0) {
		TASK_CHECK(0);
		return 1;
	}
	return xfer_sweep_one(2);
}

static int
case_transfer_full_chains_not_copied(void)
{
	struct xfer_scenario x;
	struct evbuffer_iovec vec[8];
	struct view src_before;
	int n, i;

	if (ensure_harness())
		return 1;
	atomic_fault_off();
	if (xfer_build(&x, 0)) {
		TASK_CHECK(0);
		return 1;
	}
	if (view_capture(x.src, &src_before)) {
		TASK_CHECK(0);
		return 1;
	}
	TASK_CHECK(src_before.nvec == 3);
	TASK_CHECK(evbuffer_remove_buffer(x.src, x.dst, 10) == 10);

	/* The first two complete chains must still live at the exact storage
	 * the source had: zero-copy movement, not a byte-wise copy.  The
	 * library copies a multicast child chain header for each moved chain,
	 * but the chain's buffer pointer (the actual data storage) is shared,
	 * which is what iov_base exposes. */
	n = evbuffer_peek(x.dst, -1, NULL, vec, 8);
	TASK_CHECK(n >= 3);
	for (i = 0; i < 2; ++i) {
		TASK_CHECK(vec[i].iov_base == src_before.vec[i].iov_base);
		TASK_CHECK(vec[i].iov_len == 4);
	}
	/* The crossing fragment was copied into fresh storage. */
	TASK_CHECK(vec[2].iov_base != src_before.vec[2].iov_base);
	TASK_CHECK(vec[2].iov_len == 2);
	TASK_CHECK(evbuffer_get_length(x.src) == 2);
	view_clear(&src_before);
	xfer_free(&x);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

static int
case_transfer_source_freed_first(void)
{
	struct xfer_scenario x;
	unsigned char out[16];

	if (ensure_harness())
		return 1;
	atomic_fault_off();
	if (xfer_build(&x, 0)) {
		TASK_CHECK(0);
		return 1;
	}
	TASK_CHECK(evbuffer_remove_buffer(x.src, x.dst, 10) == 10);
	evbuffer_free(x.src);
	x.src = NULL;
	TASK_CHECK(evbuffer_copyout(x.dst, out, 10) == 10);
	TASK_CHECK(memcmp(out, "aaaabbbbcc", 10) == 0);
	evbuffer_free(x.dst);
	x.dst = NULL;
	/* Two blocks moved into dst, one block's remainder stayed in src;
	 * both buffers are gone, so all three originals were released. */
	TASK_CHECK(cblock_cleanups() == 3);
	TASK_CHECK(cblock_bad() == 0);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

static int
case_transfer_target_freed_first(void)
{
	struct xfer_scenario x;
	unsigned char out[16];

	if (ensure_harness())
		return 1;
	atomic_fault_off();
	if (xfer_build(&x, 0)) {
		TASK_CHECK(0);
		return 1;
	}
	TASK_CHECK(evbuffer_remove_buffer(x.src, x.dst, 10) == 10);
	evbuffer_free(x.dst);
	x.dst = NULL;
	TASK_CHECK(evbuffer_get_length(x.src) == 2);
	TASK_CHECK(evbuffer_copyout(x.src, out, 2) == 2);
	TASK_CHECK(memcmp(out, "cc", 2) == 0);
	evbuffer_free(x.src);
	x.src = NULL;
	TASK_CHECK(cblock_cleanups() == 3);
	TASK_CHECK(cblock_bad() == 0);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

static int
case_transfer_shared_source_later_moved(void)
{
	struct evbuffer *src = NULL, *share = NULL, *dst = NULL;
	struct cblock *b0;
	unsigned char out[16];
	int i;

	if (ensure_harness())
		return 1;
	atomic_fault_off();
	cblock_reset();
	b0 = cblock_new("abcd", 4);
	src = evbuffer_new();
	share = evbuffer_new();
	dst = evbuffer_new();
	if (!src || !share || !dst) {
		TASK_CHECK(0);
		return 1;
	}
	if (evbuffer_add_reference(src, b0->ptr, 4, cblock_cleanup, b0) !=
	    0) {
		TASK_CHECK(0);
		return 1;
	}
	for (i = 0; i < 3; ++i) {
		TASK_CHECK(evbuffer_add(src, "efgh", 4) == 0);
	}
	/* Establish the sharing relation first. */
	if (evbuffer_add_buffer_reference(share, src) != 0) {
		TASK_CHECK(0);
		return 1;
	}
	/* The shared source is then transferred destructively. */
	if (evbuffer_add_buffer(dst, src) < 0) {
		TASK_CHECK(0);
		return 1;
	}
	TASK_CHECK(evbuffer_get_length(src) == 0);
	TASK_CHECK(evbuffer_get_length(dst) == 16);
	TASK_CHECK(evbuffer_copyout(dst, out, 16) == 16);
	/* dst now holds the copied bytes of the shared source's chains; the
	 * source was drained by the destructive add. */
	/* The pre-existing share still observes the original 16 bytes, even
	 * though the source buffer was drained. */
	TASK_CHECK(evbuffer_copyout(share, out, 16) == 16);
	TASK_CHECK(memcmp(out, "abcd", 4) == 0);
	TASK_CHECK(memcmp(out + 4, "efghefghefgh", 12) == 0);
	evbuffer_free(src);
	evbuffer_free(dst);
	TASK_CHECK(evbuffer_copyout(share, out, 16) == 16);
	TASK_CHECK(memcmp(out, "abcd", 4) == 0);
	evbuffer_free(share);
	TASK_CHECK(cblock_cleanups() == 1);
	TASK_CHECK(cblock_bad() == 0);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

static int
case_transfer_reject_reference_target(void)
{
	struct xfer_scenario x;
	struct view before, after;
	int rc;

	if (ensure_harness())
		return 1;
	atomic_fault_off();
	if (xfer_build(&x, 0)) {
		TASK_CHECK(0);
		return 1;
	}
	/* Share src once, then try to reference the resulting target. */
	TASK_CHECK(evbuffer_add_buffer_reference(x.dst, x.src) == 0);
	{
		struct evbuffer *third = evbuffer_new();
		if (!third) {
			TASK_CHECK(0);
			return 1;
		}
		if (view_capture(x.dst, &before)) {
			TASK_CHECK(0);
			return 1;
		}
		cb_log_clear(&x.dst_log);
		rc = evbuffer_add_buffer_reference(third, x.dst);
		TASK_CHECK(rc == -1);
		TASK_CHECK(x.dst_log.calls == 0);
		if (view_capture(x.dst, &after)) {
			TASK_CHECK(0);
			return 1;
		}
		TASK_CHECK(view_equal(&before, &after, "reference target"));
		view_clear(&before);
		view_clear(&after);
		evbuffer_free(third);
	}
	xfer_free(&x);
	TASK_CHECK(cblock_bad() == 0);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Callbacks group                                                     */
/* ------------------------------------------------------------------ */

static int
case_callbacks_sync_failure_clears_nothing(void)
{
	struct evbuffer *src = NULL, *dst = NULL;
	struct cb_log src_log, dst_log;
	int rc;

	if (ensure_harness())
		return 1;
	atomic_fault_off();
	cb_log_clear(&src_log);
	cb_log_clear(&dst_log);
	src = evbuffer_new();
	dst = evbuffer_new();
	if (!src || !dst) {
		TASK_CHECK(0);
		return 1;
	}
	TASK_CHECK(evbuffer_add(src, "abcd", 4) == 0);
	TASK_CHECK(evbuffer_add(src, "efgh", 4) == 0);
	TASK_CHECK(evbuffer_add(dst, "pre", 3) == 0);
	if (!evbuffer_add_cb(src, cb_record, &src_log) ||
	    !evbuffer_add_cb(dst, cb_record, &dst_log)) {
		TASK_CHECK(0);
		return 1;
	}
	/* Clear any pre-existing event bookkeeping. */
	src->n_add_for_cb = src->n_del_for_cb = 0;
	dst->n_add_for_cb = dst->n_del_for_cb = 0;

	atomic_fail_begin(1);
	rc = evbuffer_remove_buffer(src, dst, 4);
	atomic_fail_end();
	if (atomic_fault_hit) {
		TASK_CHECK(rc == -1);
		TASK_CHECK(src_log.calls == 0 && dst_log.calls == 0);
		TASK_CHECK(src->n_del_for_cb == 0);
		TASK_CHECK(dst->n_add_for_cb == 0);
		/* Retry without the fault must report the real byte amounts. */
		rc = evbuffer_remove_buffer(src, dst, 4);
		TASK_CHECK(rc == 4);
		TASK_CHECK(src_log.calls == 1 && dst_log.calls == 1);
		TASK_CHECK(src_log.last_deleted == 4);
		TASK_CHECK(dst_log.last_added == 4);
		TASK_CHECK(src_log.calls == 1 && src_log.added == 0);
		TASK_CHECK(dst_log.calls == 1 && dst_log.deleted == 0);
	} else {
		/* The first allocation may not be on this op's path in some
		 * builds; the sweep in the transfer group covers it. */
		atomic_fault_off();
	}
	evbuffer_free(src);
	evbuffer_free(dst);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

static int
case_callbacks_sync_success_amounts(void)
{
	struct evbuffer *src = NULL, *dst = NULL;
	struct cb_log src_log, dst_log;
	int rc;

	if (ensure_harness())
		return 1;
	atomic_fault_off();
	cb_log_clear(&src_log);
	cb_log_clear(&dst_log);
	src = evbuffer_new();
	dst = evbuffer_new();
	if (!src || !dst) {
		TASK_CHECK(0);
		return 1;
	}
	TASK_CHECK(evbuffer_add(src, "abcdef", 6) == 0);
	TASK_CHECK(evbuffer_add(dst, "xy", 2) == 0);
	if (!evbuffer_add_cb(src, cb_record, &src_log) ||
	    !evbuffer_add_cb(dst, cb_record, &dst_log)) {
		TASK_CHECK(0);
		return 1;
	}
	rc = evbuffer_remove_buffer(src, dst, 4);
	TASK_CHECK(rc == 4);
	TASK_CHECK(evbuffer_get_length(src) == 2);
	TASK_CHECK(evbuffer_get_length(dst) == 6);
	TASK_CHECK(src_log.calls == 1 && src_log.last_deleted == 4);
	TASK_CHECK(src_log.added == 0);
	TASK_CHECK(dst_log.calls == 1 && dst_log.last_added == 4);
	TASK_CHECK(dst_log.deleted == 0);
	/* Byte-amount contract: callback values match the real length delta,
	 * not merely the number of invocations. */
	{
		unsigned char out[8];
		TASK_CHECK(evbuffer_copyout(dst, out, 6) == 6);
		TASK_CHECK(memcmp(out, "xyabcd", 6) == 0);
	}
	evbuffer_free(src);
	evbuffer_free(dst);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

static int
case_callbacks_deferred_failure_keeps_pending(void)
{
	struct event_base *base = NULL;
	struct evbuffer *src = NULL, *dst = NULL;
	struct cb_log src_log, dst_log;
	int rc;

	if (ensure_harness())
		return 1;
	atomic_fault_off();
	cb_log_clear(&src_log);
	cb_log_clear(&dst_log);
	base = event_base_new();
	src = evbuffer_new();
	dst = evbuffer_new();
	if (!base || !src || !dst) {
		TASK_CHECK(0);
		return 1;
	}
	if (evbuffer_defer_callbacks(src, base) < 0 ||
	    evbuffer_defer_callbacks(dst, base) < 0) {
		TASK_CHECK(0);
		return 1;
	}
	if (!evbuffer_add_cb(src, cb_record, &src_log) ||
	    !evbuffer_add_cb(dst, cb_record, &dst_log)) {
		TASK_CHECK(0);
		return 1;
	}
	/* Clear everything that happened while setting up. */
	TASK_CHECK(event_base_loop(base, EVLOOP_NONBLOCK) != -1);
	cb_log_clear(&src_log);
	cb_log_clear(&dst_log);

	TASK_CHECK(evbuffer_add(src, "abcd", 4) == 0);
	/* A deferred notification is now queued but not yet delivered. */
	TASK_CHECK(src->n_add_for_cb == 4);
	/* Moving 2 of 4 bytes across the single-chain boundary needs exactly
	 * one allocation for the copied fragment, so failing the first
	 * allocation deterministically takes that path. */
	atomic_fail_begin(1);
	rc = evbuffer_remove_buffer(src, dst, 2);
	TASK_CHECK(atomic_fault_hit);
	atomic_fail_end();
	TASK_CHECK(rc == -1);
	/* Failure must not have added or removed anything from the queue:
	 * the pending notification from the earlier add is still there. */
	TASK_CHECK(event_base_loop(base, EVLOOP_NONBLOCK) != -1);
	TASK_CHECK(src_log.calls == 1);
	TASK_CHECK(src_log.last_added == 4);
	TASK_CHECK(src_log.last_deleted == 0);
	TASK_CHECK(src_log.added == 4);
	TASK_CHECK(src_log.deleted == 0);
	evbuffer_free(src);
	evbuffer_free(dst);
	event_base_free(base);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

static int
case_callbacks_deferred_success_amounts(void)
{
	struct event_base *base = NULL;
	struct evbuffer *src = NULL, *dst = NULL;
	struct cb_log src_log, dst_log;

	if (ensure_harness())
		return 1;
	atomic_fault_off();
	cb_log_clear(&src_log);
	cb_log_clear(&dst_log);
	base = event_base_new();
	src = evbuffer_new();
	dst = evbuffer_new();
	if (!base || !src || !dst) {
		TASK_CHECK(0);
		return 1;
	}
	if (evbuffer_defer_callbacks(src, base) < 0 ||
	    evbuffer_defer_callbacks(dst, base) < 0) {
		TASK_CHECK(0);
		return 1;
	}
	if (!evbuffer_add_cb(src, cb_record, &src_log) ||
	    !evbuffer_add_cb(dst, cb_record, &dst_log)) {
		TASK_CHECK(0);
		return 1;
	}
	TASK_CHECK(event_base_loop(base, EVLOOP_NONBLOCK) != -1);
	cb_log_clear(&src_log);
	cb_log_clear(&dst_log);
	TASK_CHECK(evbuffer_add(src, "abcdef", 6) == 0);
	TASK_CHECK(evbuffer_add(dst, "xy", 2) == 0);
	TASK_CHECK(event_base_loop(base, EVLOOP_NONBLOCK) != -1);
	cb_log_clear(&src_log);
	cb_log_clear(&dst_log);

	TASK_CHECK(evbuffer_remove_buffer(src, dst, 4) == 4);
	TASK_CHECK(event_base_loop(base, EVLOOP_NONBLOCK) != -1);
	TASK_CHECK(src_log.calls == 1 && src_log.last_deleted == 4);
	TASK_CHECK(src_log.added == 0);
	TASK_CHECK(dst_log.calls == 1 && dst_log.last_added == 4);
	TASK_CHECK(dst_log.deleted == 0);
	TASK_CHECK(evbuffer_get_length(dst) == 6);
	evbuffer_free(src);
	evbuffer_free(dst);
	event_base_free(base);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Model group                                                         */
/* ------------------------------------------------------------------ */

#define MODEL_OPS 80

static int
run_model_range(uint64_t first, uint64_t count)
{
	uint64_t i;

	for (i = 0; i < count; ++i) {
		if (model_run_seed(first + i, MODEL_OPS))
			return 1;
	}
	return 0;
}

static int
case_model_seeds_1_40(void)
{
	if (ensure_harness())
		return 1;
	TASK_CHECK(run_model_range(1, 40) == 0);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

static int
case_model_seeds_41_80(void)
{
	if (ensure_harness())
		return 1;
	TASK_CHECK(run_model_range(41, 40) == 0);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

static int
case_model_seeds_81_120(void)
{
	if (ensure_harness())
		return 1;
	TASK_CHECK(run_model_range(81, 40) == 0);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

static int
case_model_seeds_121_160(void)
{
	if (ensure_harness())
		return 1;
	TASK_CHECK(run_model_range(121, 40) == 0);
	TASK_CHECK(atomic_ledger_outstanding() == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
	{ "reference", "oom_sweep", case_reference_oom_sweep },
	{ "reference", "cleanup_once", case_reference_cleanup_once },
	{ "reference", "reject_shared_source",
	  case_reference_reject_shared_source },
	{ "reference", "reject_freeze_and_self",
	  case_reference_reject_freeze_and_self },
	{ "reference", "zero_length", case_reference_zero_length },

	{ "transfer", "oom_empty_dest", case_transfer_oom_empty_dest },
	{ "transfer", "oom_normal_tail", case_transfer_oom_normal_tail },
	{ "transfer", "oom_immutable_tail",
	  case_transfer_oom_immutable_tail },
	{ "transfer", "full_chains_not_copied",
	  case_transfer_full_chains_not_copied },
	{ "transfer", "source_freed_first",
	  case_transfer_source_freed_first },
	{ "transfer", "target_freed_first",
	  case_transfer_target_freed_first },
	{ "transfer", "shared_source_later_moved",
	  case_transfer_shared_source_later_moved },
	{ "transfer", "reject_reference_target",
	  case_transfer_reject_reference_target },

	{ "callbacks", "sync_failure_clears_nothing",
	  case_callbacks_sync_failure_clears_nothing },
	{ "callbacks", "sync_success_amounts",
	  case_callbacks_sync_success_amounts },
	{ "callbacks", "deferred_failure_keeps_pending",
	  case_callbacks_deferred_failure_keeps_pending },
	{ "callbacks", "deferred_success_amounts",
	  case_callbacks_deferred_success_amounts },

	{ "model", "seeds_1_40", case_model_seeds_1_40 },
	{ "model", "seeds_41_80", case_model_seeds_41_80 },
	{ "model", "seeds_81_120", case_model_seeds_81_120 },
	{ "model", "seeds_121_160", case_model_seeds_121_160 },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);

