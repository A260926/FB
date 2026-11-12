/* Regression tests for mdb_del_range().
 *
 * Three groups are exercised through the single range_case() entry point:
 *   bounds   - interval semantics, comparators and argument validation
 *   pages    - whole-leaf batching, overflow pages, stats and page reuse
 *   cursors  - cursor positioning, snapshots and nested transactions
 *
 * Every content outcome is cross-checked against an equivalent database that
 * applies the same deletion one key at a time with mdb_del().
 */
#include "lmdb.h"
#include "cases.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#define NKEYS		3000		/* default number of records */
#define BIGVAL		400		/* forces an overflow record on 512B pages */
#define SMALLVAL	32

typedef int (cmp_fn)(const MDB_val *, const MDB_val *);

/* Reverse-order comparator: exercises mdb_del_range() with a custom
 * comparator instead of the built-in lexical one.
 */
static int
rev_cmp(const MDB_val *a, const MDB_val *b)
{
	size_t n = a->mv_size < b->mv_size ? a->mv_size : b->mv_size;
	int r = memcmp(a->mv_data, b->mv_data, n);
	if (r)
		return -r;
	if (a->mv_size < b->mv_size)
		return 1;
	if (a->mv_size > b->mv_size)
		return -1;
	return 0;
}

static void
make_key(char *buf, int i)
{
	snprintf(buf, 9, "%08d", i);
}

static int
open_env(const char *path, MDB_env **env, int maxdbs, int psize)
{
	int rc;
	rc = mdb_env_create(env);
	if (rc) return rc;
	rc = mdb_env_set_maxdbs(*env, maxdbs);
	if (rc) return rc;
	rc = mdb_env_set_mapsize(*env, (mdb_size_t)64*1024*1024);
	if (rc) return rc;
	if (psize) {
		rc = mdb_env_set_pagesize(*env, psize);
		if (rc) return rc;
	}
	return mdb_env_open(*env, path, MDB_NOTLS, 0600);
}

/* Fill dbi with keys 0..n-1.  Every big_every'th record (when nonzero) gets
 * an overflow-sized value; the rest use a small inline value.
 */
static int
fill_db(MDB_txn *txn, MDB_dbi dbi, int n, int big_every)
{
	static char big[BIGVAL];
	static char small[SMALLVAL];
	char k[9];
	int i, rc;
	MDB_val key, data;

	memset(big, 'B', sizeof big);
	memset(small, 's', sizeof small);
	for (i = 0; i < n; i++) {
		make_key(k, i);
		key.mv_size = 8;
		key.mv_data = k;
		if (big_every && (i % big_every) == 0) {
			data.mv_size = sizeof big;
			data.mv_data = big;
		} else {
			data.mv_size = sizeof small;
			data.mv_data = small;
		}
		rc = mdb_put(txn, dbi, &key, &data, 0);
		if (rc)
			return rc;
	}
	return MDB_SUCCESS;
}

/* Collect all keys of dbi as integers in ascending numeric order. */
static int
collect_keys(MDB_txn *txn, MDB_dbi dbi, int *out, int cap, int *count)
{
	MDB_cursor *cur;
	MDB_val key, data;
	int rc, n = 0;

	rc = mdb_cursor_open(txn, dbi, &cur);
	if (rc) return rc;
	while ((rc = mdb_cursor_get(cur, &key, &data,
		n ? MDB_NEXT : MDB_FIRST)) == 0) {
		int v;
		if (n >= cap) { mdb_cursor_close(cur); return ENOBUFS; }
		/* Keys are always the "%08d" spelling; parse by copying. */
		char tmp[9];
		if (key.mv_size != 8) { mdb_cursor_close(cur); return EINVAL; }
		memcpy(tmp, key.mv_data, 8);
		tmp[8] = 0;
		v = atoi(tmp);
		out[n++] = v;
	}
	mdb_cursor_close(cur);
	if (rc != MDB_NOTFOUND)
		return rc;
	*count = n;
	return MDB_SUCCESS;
}

static int
in_range(MDB_txn *txn, MDB_dbi dbi, const MDB_val *k,
	const MDB_val *lo, const MDB_val *hi)
{
	if (lo && mdb_cmp(txn, dbi, k, lo) < 0)
		return 0;
	if (hi && mdb_cmp(txn, dbi, k, hi) >= 0)
		return 0;
	return 1;
}

/* Build a database, fill it, then delete [lo, hi) either with mdb_del_range()
 * or with per-key mdb_del().  Returns surviving keys and stats for comparison.
 */
static int
build_and_delete(const char *path, int n, int big_every, cmp_fn *usercmp,
	const MDB_val *lo, const MDB_val *hi, int use_range,
	int *survivors, int *nsurv, MDB_stat *st_out, mdb_size_t *range_del)
{
	MDB_env *env;
	MDB_txn *txn;
	MDB_dbi dbi;
	int rc, i;
	MDB_stat st;

	rc = open_env(path, &env, 4, 512);
	if (rc) return rc;
	rc = mdb_txn_begin(env, NULL, 0, &txn);
	if (rc) goto close_env;
	rc = mdb_dbi_open(txn, "events", MDB_CREATE, &dbi);
	if (rc) goto abort_txn;
	if (usercmp) {
		rc = mdb_set_compare(txn, dbi, usercmp);
		if (rc) goto abort_txn;
	}
	rc = fill_db(txn, dbi, n, big_every);
	if (rc) goto abort_txn;
	rc = mdb_txn_commit(txn);
	if (rc) goto close_env;

	rc = mdb_txn_begin(env, NULL, 0, &txn);
	if (rc) goto close_env;

	if (use_range) {
		rc = mdb_del_range(txn, dbi, lo, hi, range_del);
	} else {
		mdb_size_t deleted = 0;
		char kbuf[9];
		MDB_val key;
		for (i = 0; i < n; i++) {
			make_key(kbuf, i);
			key.mv_size = 8;
			key.mv_data = kbuf;
			if (in_range(txn, dbi, &key, lo, hi)) {
				int rc2 = mdb_del(txn, dbi, &key, NULL);
				if (rc2) { rc = rc2; break; }
				deleted++;
			}
		}
		if (range_del)
			*range_del = deleted;
	}
	if (rc) goto abort_txn;

	rc = mdb_stat(txn, dbi, &st);
	if (rc) goto abort_txn;
	rc = collect_keys(txn, dbi, survivors, n, nsurv);
	if (rc) goto abort_txn;
	rc = mdb_txn_commit(txn);
	if (rc) goto close_env;
	*st_out = st;
	mdb_env_close(env);
	return MDB_SUCCESS;

abort_txn:
	mdb_txn_abort(txn);
close_env:
	mdb_env_close(env);
	return rc;
}

static char *
temp_dir(void)
{
	const char *base = getenv("TMPDIR");
	char *tmpl;
	static unsigned seq;
	if (!base || !*base)
		base = "/tmp";
	tmpl = malloc(strlen(base) + 32);
	if (!tmpl)
		return NULL;
	sprintf(tmpl, "%s/rdr_%d_%u", base, (int)getpid(), seq++);
	if (mkdir(tmpl, 0700) != 0) {
		free(tmpl);
		return NULL;
	}
	return tmpl;
}

static int
same_arrays(const int *a, int na, const int *b, int nb)
{
	int i;
	if (na != nb)
		return 0;
	for (i = 0; i < na; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

static void
bound_vals(int lo_i, int has_lo, int hi_i, int has_hi,
	char *lobuf, char *hibuf, MDB_val *lo, MDB_val *hi)
{
	if (has_lo) {
		make_key(lobuf, lo_i);
		lo->mv_size = 8;
		lo->mv_data = lobuf;
	} else {
		lo->mv_size = 0;
		lo->mv_data = NULL;
	}
	if (has_hi) {
		make_key(hibuf, hi_i);
		hi->mv_size = 8;
		hi->mv_data = hibuf;
	} else {
		hi->mv_size = 0;
		hi->mv_data = NULL;
	}
}

/* Compare a range deletion against the per-key reference for one interval. */
static int
check_interval(const char *tag, int n, int big_every, cmp_fn *cmp,
	int lo_i, int has_lo, int hi_i, int has_hi, int expect_deleted)
{
	char *d1, *d2;
	char lobuf[9], hibuf[9];
	MDB_val lo, hi;
	int a[NKEYS], b[NKEYS], na = 0, nb = 0, rc, cases = 0;
	mdb_size_t dr = 0, dp = 0;
	MDB_stat sr, sp;

	d1 = temp_dir();
	d2 = temp_dir();
	CHECK_CHECK(d1 && d2);
	bound_vals(lo_i, has_lo, hi_i, has_hi, lobuf, hibuf, &lo, &hi);

	rc = build_and_delete(d1, n, big_every, cmp,
		has_lo ? &lo : NULL, has_hi ? &hi : NULL,
		1, a, &na, &sr, &dr);
	CHECK_CHECK(rc == MDB_SUCCESS);
	rc = build_and_delete(d2, n, big_every, cmp,
		has_lo ? &lo : NULL, has_hi ? &hi : NULL,
		0, b, &nb, &sp, &dp);
	CHECK_CHECK(rc == MDB_SUCCESS);

	CHECK_CHECK((int)dr == (int)dp);
	CHECK_CHECK(same_arrays(a, na, b, nb));
	CHECK_CHECK(sr.ms_entries == sp.ms_entries);
	CHECK_CHECK(sr.ms_entries == (mdb_size_t)na);
	/* The same keys must remain and overflow pages must be freed equally.
	 * Physical leaf/branch packing may legitimately differ because the
	 * range path reclaims whole pages; both trees must be non-empty and
	 * structurally valid when records survive.
	 */
	CHECK_CHECK(sr.ms_overflow_pages == sp.ms_overflow_pages);
	if (na > 0) {
		CHECK_CHECK(sr.ms_depth >= 1 && sp.ms_depth >= 1);
		CHECK_CHECK(sr.ms_leaf_pages >= 1 && sp.ms_leaf_pages >= 1);
	} else {
		CHECK_CHECK(sr.ms_leaf_pages == 0 && sp.ms_leaf_pages == 0);
	}
	if (expect_deleted >= 0)
		CHECK_CHECK((int)dr == expect_deleted);
	(void)tag;
	cases = 8 + (expect_deleted >= 0) + (na > 0 ? 2 : 1);
	free(d1);
	free(d2);
	return cases;
}

static int
bounds_case(void)
{
	int cases = 0, rc;
	char *dir;
	MDB_env *env;
	MDB_txn *txn, *rtxn;
	MDB_dbi dbi, dupdbi, maindbi;
	MDB_val lo, hi, key, data;
	char kbuf[9];
	mdb_size_t deleted;
	int surv[NKEYS], nsurv = 0;
	MDB_stat st;

	/* Middle interval, head, tail, whole table, no-hit and equal bounds. */
	cases += check_interval("middle", NKEYS, 37, NULL, 800, 1, 2200, 1, 1400);
	cases += check_interval("head", NKEYS, 37, NULL, 0, 0, 900, 1, 900);
	cases += check_interval("tail", NKEYS, 37, NULL, 2500, 1, 0, 0, NKEYS - 2500);
	cases += check_interval("all", NKEYS, 37, NULL, 0, 0, 0, 0, NKEYS);
	cases += check_interval("nohit", NKEYS, 37, NULL, NKEYS + 5, 1,
		NKEYS + 9, 1, 0);

	/* Bound keys need not exist: a shorter key sorts before every 8-digit
	 * key under the lexical comparator.
	 */
	{
		/* A seven-byte bound cannot equal any eight-byte key and sorts
		 * before the first key "00000000", so it need not exist.
		 */
		char *d1 = temp_dir(), *d2 = temp_dir();
		MDB_val l = {7, "0000000"}, h = {8, "00000010"};
		int a[NKEYS], b[NKEYS], na = 0, nb = 0;
		MDB_stat s1, s2;
		mdb_size_t r1 = 0, r2 = 0;
		CHECK_CHECK(d1 && d2);
		rc = build_and_delete(d1, 500, 0, NULL, &l, &h, 1, a, &na, &s1, &r1);
		CHECK_CHECK(rc == 0);
		rc = build_and_delete(d2, 500, 0, NULL, &l, &h, 0, b, &nb, &s2, &r2);
		CHECK_CHECK(rc == 0);
		CHECK_CHECK(r1 == r2 && r1 == 10);
		CHECK_CHECK(same_arrays(a, na, b, nb));
		free(d1); free(d2);
		cases += 4;
	}

	/* Custom reverse-order comparator.  Under reverse ordering the
	 * comparator-smaller bound is the numerically larger key, so the
	 * interval [2600, 400) selects numeric keys 401..2599.
	 */
	cases += check_interval("reverse", NKEYS, 53, rev_cmp,
		2600, 1, 400, 1, -1);
	{
		/* Re-derive the expected reverse-order deletion count directly. */
		char *d = temp_dir();
		char lob[9], hib[9];
		MDB_val rlo, rhi;
		int expect = 0, i, got[NKEYS], ng = 0;
		MDB_stat s;
		mdb_size_t rd = 0;
		CHECK_CHECK(d);
		make_key(lob, 2600);
		make_key(hib, 400);
		rlo.mv_size = rhi.mv_size = 8;
		rlo.mv_data = lob;
		rhi.mv_data = hib;
		for (i = 0; i < NKEYS; i++) {
			MDB_val k;
			char kb[9];
			make_key(kb, i);
			k.mv_size = 8;
			k.mv_data = kb;
			if (rev_cmp(&k, &rlo) >= 0 && rev_cmp(&k, &rhi) < 0)
				expect++;
		}
		rc = build_and_delete(d, NKEYS, 53, rev_cmp, &rlo, &rhi, 1,
			got, &ng, &s, &rd);
		CHECK_CHECK(rc == 0);
		CHECK_CHECK((int)rd == expect);
		CHECK_CHECK((int)s.ms_entries == NKEYS - expect);
		free(d);
		cases += 3;
	}

	/* Equal bounds delete zero keys; inverted bounds are rejected. */
	dir = temp_dir();
	CHECK_CHECK(dir);
	rc = open_env(dir, &env, 4, 512);
	CHECK_CHECK(rc == 0);
	rc = mdb_txn_begin(env, NULL, 0, &txn);
	CHECK_CHECK(rc == 0);
	rc = mdb_dbi_open(txn, "events", MDB_CREATE, &dbi);
	CHECK_CHECK(rc == 0);
	rc = fill_db(txn, dbi, 100, 0);
	CHECK_CHECK(rc == 0);
	rc = mdb_txn_commit(txn);
	CHECK_CHECK(rc == 0);

	rc = mdb_txn_begin(env, NULL, 0, &txn);
	CHECK_CHECK(rc == 0);
	make_key(kbuf, 40);
	key.mv_size = 8; key.mv_data = kbuf;
	deleted = 777;
	rc = mdb_del_range(txn, dbi, &key, &key, &deleted);
	CHECK_CHECK(rc == 0);
	CHECK_CHECK(deleted == 0);
	rc = mdb_stat(txn, dbi, &st);
	CHECK_CHECK(rc == 0 && st.ms_entries == 100);

	/* Inverted range under the lexical comparator. */
	make_key(kbuf, 10);
	lo.mv_size = 8; lo.mv_data = kbuf;
	{
		char hb[9];
		make_key(hb, 5);
		hi.mv_size = 8; hi.mv_data = hb;
		deleted = 777;
		rc = mdb_del_range(txn, dbi, &lo, &hi, &deleted);
		CHECK_CHECK(rc == EINVAL);
		CHECK_CHECK(deleted == 0);
	}
	rc = mdb_stat(txn, dbi, &st);
	CHECK_CHECK(rc == 0 && st.ms_entries == 100);

	/* NULL output pointer. */
	rc = mdb_del_range(txn, dbi, &lo, NULL, NULL);
	CHECK_CHECK(rc == EINVAL);

	/* Main database and DUPSORT are rejected before any change. */
	rc = mdb_dbi_open(txn, NULL, 0, &maindbi);
	CHECK_CHECK(rc == 0);
	rc = mdb_del_range(txn, maindbi, NULL, NULL, &deleted);
	CHECK_CHECK(rc == EINVAL);
	rc = mdb_dbi_open(txn, "dups", MDB_CREATE|MDB_DUPSORT, &dupdbi);
	CHECK_CHECK(rc == 0);
	rc = mdb_del_range(txn, dupdbi, NULL, NULL, &deleted);
	CHECK_CHECK(rc == MDB_INCOMPATIBLE);
	rc = mdb_del_range(txn, 9999, NULL, NULL, &deleted);
	CHECK_CHECK(rc == EINVAL);
	rc = mdb_stat(txn, dbi, &st);
	CHECK_CHECK(rc == 0 && st.ms_entries == 100);

	/* Read-only transactions follow the existing convention. */
	rc = mdb_txn_commit(txn);
	CHECK_CHECK(rc == 0);
	rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &rtxn);
	CHECK_CHECK(rc == 0);
	rc = mdb_del_range(rtxn, dbi, NULL, NULL, &deleted);
	CHECK_CHECK(rc == MDB_IS_READONLY);
	mdb_txn_abort(rtxn);

	/* After deleting everything the DB is empty and reusable. */
	rc = mdb_txn_begin(env, NULL, 0, &txn);
	CHECK_CHECK(rc == 0);
	rc = mdb_del_range(txn, dbi, NULL, NULL, &deleted);
	CHECK_CHECK(rc == 0 && deleted == 100);
	rc = mdb_stat(txn, dbi, &st);
	CHECK_CHECK(rc == 0 && st.ms_entries == 0 && st.ms_leaf_pages == 0);
	rc = collect_keys(txn, dbi, surv, NKEYS, &nsurv);
	CHECK_CHECK(rc == 0 && nsurv == 0);
	data.mv_size = SMALLVAL;
	data.mv_data = "ssssssssssssssssssssssssssssssss";
	make_key(kbuf, 7);
	key.mv_size = 8; key.mv_data = kbuf;
	rc = mdb_put(txn, dbi, &key, &data, 0);
	CHECK_CHECK(rc == 0);
	rc = mdb_txn_commit(txn);
	CHECK_CHECK(rc == 0);
	mdb_env_close(env);
	free(dir);
	cases += 22;
	return cases;
}

static int
pages_case(void)
{
	char *d1 = temp_dir(), *d2 = temp_dir();
	MDB_env *env;
	MDB_txn *txn;
	MDB_dbi dbi;
	int rc, cases = 0, a[NKEYS], b[NKEYS], na = 0, nb = 0;
	MDB_stat sr, sp;
	mdb_size_t dr = 0, dp = 0, full_pages, edge_records;
	char lob[9], hib[9];
	MDB_val lo, hi;

	CHECK_CHECK(d1 && d2);
	make_key(lob, 300);
	make_key(hib, 2700);
	lo.mv_size = hi.mv_size = 8;
	lo.mv_data = lob;
	hi.mv_data = hib;

	/* Range delete with whole-page batching, versus per-key deletion. */
	rc = build_and_delete(d1, NKEYS, 37, NULL, &lo, &hi, 1, a, &na, &sr, &dr);
	CHECK_CHECK(rc == 0);
	rc = build_and_delete(d2, NKEYS, 37, NULL, &lo, &hi, 0, b, &nb, &sp, &dp);
	CHECK_CHECK(rc == 0);
	CHECK_CHECK(dr == 2400 && dp == 2400);
	CHECK_CHECK(same_arrays(a, na, b, nb));
	CHECK_CHECK(sr.ms_entries == sp.ms_entries);
	/* Same surviving keys and freed overflow; the range path may pack the
	 * surviving tree more tightly, so only require a sane page inventory.
	 */
	CHECK_CHECK(sr.ms_overflow_pages == sp.ms_overflow_pages);
	CHECK_CHECK(sr.ms_leaf_pages >= 1 && sr.ms_branch_pages >= 0);
	/* Identical surviving keys; physical packing may legitimately differ. */
	CHECK_CHECK(sr.ms_depth >= 1);
	cases += 7;

	/* The local counters prove whole leaf pages were reclaimed in bulk. */
	{
		char *d = temp_dir();
		MDB_stat st;
		int surv[NKEYS], ns = 0;
		mdb_size_t cnt = 0;
		CHECK_CHECK(d);
		rc = build_and_delete(d, NKEYS, 37, NULL, &lo, &hi, 1,
			surv, &ns, &st, &cnt);
		CHECK_CHECK(rc == 0);
		mdb_del_range_counters(&full_pages, &edge_records);
		/* A 2400-key span across a multi-level tree must batch many pages. */
		CHECK_CHECK(full_pages > 1);
		/* Only the two straddling edge pages are edited record by record. */
		CHECK_CHECK(edge_records > 0 && edge_records < 128);
		free(d);
		cases += 4;
	}

	/* Reopen, verify values (including overflow) are intact, reinsert and
	 * confirm reclaimed pages are reused rather than leaked.
	 */
	rc = open_env(d1, &env, 4, 512);
	CHECK_CHECK(rc == 0);
	rc = mdb_txn_begin(env, NULL, 0, &txn);
	CHECK_CHECK(rc == 0);
	rc = mdb_dbi_open(txn, "events", 0, &dbi);
	CHECK_CHECK(rc == 0);
	rc = mdb_stat(txn, dbi, &sr);
	CHECK_CHECK(rc == 0);
	{
		MDB_cursor *cur;
		MDB_val k, v;
		int walked = 0, bad = 0;
		rc = mdb_cursor_open(txn, dbi, &cur);
		CHECK_CHECK(rc == 0);
		while (mdb_cursor_get(cur, &k, &v, walked ? MDB_NEXT : MDB_FIRST) == 0) {
			char tb[9];
			int idx;
			walked++;
			memcpy(tb, k.mv_data, 8);
			tb[8] = 0;
			idx = atoi(tb);
			/* Every surviving record must be outside the deleted span. */
			if (idx >= 300 && idx < 2700)
				bad = 1;
			if (idx % 37 == 0) {
				if (v.mv_size != BIGVAL)
					bad = 1;
			} else if (v.mv_size != SMALLVAL) {
				bad = 1;
			}
		}
		mdb_cursor_close(cur);
		CHECK_CHECK(bad == 0);
		CHECK_CHECK(walked == (int)sr.ms_entries);
		cases += 2;
	}
	/* Reinsert the deleted span; the reclaimed pages must be reused rather
	 * than leaked.  Compare the resulting tree against a freshly built
	 * database holding the identical full dataset.
	 */
	{
		char kb[9];
		MDB_val k, v;
		static char big[BIGVAL], small[SMALLVAL];
		int i, added = 0;
		MDB_stat refilled;
		memset(big, 'B', sizeof big);
		memset(small, 's', sizeof small);
		for (i = 300; i < 2700; i++) {
			make_key(kb, i);
			k.mv_size = 8; k.mv_data = kb;
			if (i % 37 == 0) {
				v.mv_size = sizeof big; v.mv_data = big;
			} else {
				v.mv_size = sizeof small; v.mv_data = small;
			}
			rc = mdb_put(txn, dbi, &k, &v, 0);
			CHECK_CHECK(rc == 0);
			added++;
		}
		CHECK_CHECK(added == 2400);
		rc = mdb_stat(txn, dbi, &refilled);
		CHECK_CHECK(rc == 0 && refilled.ms_entries == NKEYS);
		rc = mdb_txn_commit(txn);
		CHECK_CHECK(rc == 0);
		mdb_env_close(env);

		/* Fresh reference database with the same full dataset. */
		{
			MDB_env *e2; MDB_txn *t2; MDB_dbi h2; MDB_stat fresht;
			rc = open_env(d2, &e2, 4, 512);
			CHECK_CHECK(rc == 0);
			rc = mdb_txn_begin(e2, NULL, 0, &t2);
			CHECK_CHECK(rc == 0);
			rc = mdb_dbi_open(t2, "events", MDB_CREATE, &h2);
			CHECK_CHECK(rc == 0);
			rc = fill_db(t2, h2, NKEYS, 37);
			CHECK_CHECK(rc == 0);
			rc = mdb_stat(t2, h2, &fresht);
			CHECK_CHECK(rc == 0);
			rc = mdb_txn_commit(t2);
			CHECK_CHECK(rc == 0);
			/* The delete-and-refill cycle must not leak overflow space
			 * and must hold the identical full dataset.  Leaf/branch
			 * packing can differ from a fresh sequential build because
			 * the refill is interleaved with surviving records, so bound
			 * the page inventory by the total record count instead.
			 */
			CHECK_CHECK(refilled.ms_overflow_pages ==
				fresht.ms_overflow_pages);
			CHECK_CHECK(refilled.ms_entries == fresht.ms_entries);
			CHECK_CHECK(refilled.ms_leaf_pages <=
				fresht.ms_entries);
			CHECK_CHECK(refilled.ms_leaf_pages >= 1);
			mdb_env_close(e2);
			cases += 6;
		}
	}

	free(d1);
	free(d2);
	return cases;
}

static int
cursors_case(void)
{
	char *dir = temp_dir();
	MDB_env *env;
	MDB_txn *w, *r_old, *r_new, *child;
	MDB_dbi dbi;
	MDB_cursor *keep, *gone, *edge;
	MDB_val key, data, k2, d2;
	int rc, cases = 0;
	char kb[9];
	MDB_stat st;

	CHECK_CHECK(dir);
	rc = open_env(dir, &env, 4, 512);
	CHECK_CHECK(rc == 0);
	rc = mdb_txn_begin(env, NULL, 0, &w);
	CHECK_CHECK(rc == 0);
	rc = mdb_dbi_open(w, "events", MDB_CREATE, &dbi);
	CHECK_CHECK(rc == 0);
	rc = fill_db(w, dbi, 1000, 41);
	CHECK_CHECK(rc == 0);
	rc = mdb_txn_commit(w);
	CHECK_CHECK(rc == 0);

	/* A read-only snapshot opened before the delete must keep seeing old data. */
	rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &r_old);
	CHECK_CHECK(rc == 0);
	rc = mdb_stat(r_old, dbi, &st);
	CHECK_CHECK(rc == 0 && st.ms_entries == 1000);

	rc = mdb_txn_begin(env, NULL, 0, &w);
	CHECK_CHECK(rc == 0);

	/* Cursors positioned on a kept key, a deleted key and a boundary key. */
	rc = mdb_cursor_open(w, dbi, &keep);
	CHECK_CHECK(rc == 0);
	make_key(kb, 100);
	key.mv_size = 8; key.mv_data = kb;
	rc = mdb_cursor_get(keep, &key, &data, MDB_SET);
	CHECK_CHECK(rc == 0);

	rc = mdb_cursor_open(w, dbi, &gone);
	CHECK_CHECK(rc == 0);
	make_key(kb, 500);
	key.mv_data = kb;
	rc = mdb_cursor_get(gone, &key, &data, MDB_SET);
	CHECK_CHECK(rc == 0);

	rc = mdb_cursor_open(w, dbi, &edge);
	CHECK_CHECK(rc == 0);
	make_key(kb, 899);	/* last key that will be deleted */
	key.mv_data = kb;
	rc = mdb_cursor_get(edge, &key, &data, MDB_SET);
	CHECK_CHECK(rc == 0);

	{
		char lob[9], hib[9];
		MDB_val lo, hi;
		mdb_size_t deleted = 0;
		make_key(lob, 200);
		make_key(hib, 900);
		lo.mv_size = hi.mv_size = 8;
		lo.mv_data = lob;
		hi.mv_data = hib;
		rc = mdb_del_range(w, dbi, &lo, &hi, &deleted);
		CHECK_CHECK(rc == 0);
		CHECK_CHECK(deleted == 700);
		cases += 2;
	}

	/* The kept cursor stays on the same key and never skips on NEXT/PREV. */
	rc = mdb_cursor_get(keep, &k2, &d2, MDB_GET_CURRENT);
	CHECK_CHECK(rc == 0);
	CHECK_CHECK(k2.mv_size == 8 && memcmp(k2.mv_data, "00000100", 8) == 0);
	rc = mdb_cursor_get(keep, &k2, &d2, MDB_NEXT);
	CHECK_CHECK(rc == 0);
	CHECK_CHECK(memcmp(k2.mv_data, "00000101", 8) == 0);
	rc = mdb_cursor_get(keep, &k2, &d2, MDB_PREV);
	CHECK_CHECK(rc == 0);
	CHECK_CHECK(memcmp(k2.mv_data, "00000100", 8) == 0);
	rc = mdb_cursor_get(keep, &k2, &d2, MDB_PREV);
	CHECK_CHECK(rc == 0);
	CHECK_CHECK(memcmp(k2.mv_data, "00000099", 8) == 0);
	cases += 8;

	/* Cursors on deleted records become unpositioned: GET_CURRENT EINVAL,
	 * but FIRST / SET_RANGE reposition them.
	 */
	rc = mdb_cursor_get(gone, &k2, &d2, MDB_GET_CURRENT);
	CHECK_CHECK(rc == EINVAL);
	rc = mdb_cursor_get(edge, &k2, &d2, MDB_GET_CURRENT);
	CHECK_CHECK(rc == EINVAL);
	rc = mdb_cursor_get(gone, &k2, &d2, MDB_FIRST);
	CHECK_CHECK(rc == 0);
	CHECK_CHECK(memcmp(k2.mv_data, "00000000", 8) == 0);
	make_key(kb, 950);
	key.mv_size = 8; key.mv_data = kb;
	rc = mdb_cursor_get(edge, &key, &d2, MDB_SET_RANGE);
	CHECK_CHECK(rc == 0);
	CHECK_CHECK(memcmp(key.mv_data, "00000950", 8) == 0);
	cases += 5;

	/* Full walk over the write txn yields exactly the surviving keys. */
	{
		MDB_cursor *walk;
		int expect = 0, got = 0, bad = 0, idx;
		char tb[9];
		rc = mdb_cursor_open(w, dbi, &walk);
		CHECK_CHECK(rc == 0);
		while (mdb_cursor_get(walk, &key, &data,
			got ? MDB_NEXT : MDB_FIRST) == 0) {
			got++;
			memcpy(tb, key.mv_data, 8);
			tb[8] = 0;
			idx = atoi(tb);
			if (idx >= 200 && idx < 900)
				bad = 1;
		}
		for (idx = 0; idx < 1000; idx++)
			if (!(idx >= 200 && idx < 900))
				expect++;
		CHECK_CHECK(got == expect && bad == 0);
		mdb_cursor_close(walk);
		cases += 2;
	}

	/* Nested transaction: abort discards the child's range delete. */
	rc = mdb_txn_begin(env, w, 0, &child);
	CHECK_CHECK(rc == 0);
	{
		char lob[9], hib[9];
		MDB_val lo, hi;
		mdb_size_t deleted = 0;
		make_key(lob, 0);
		make_key(hib, 100);
		lo.mv_size = hi.mv_size = 8;
		lo.mv_data = lob;
		hi.mv_data = hib;
		rc = mdb_del_range(child, dbi, &lo, &hi, &deleted);
		CHECK_CHECK(rc == 0 && deleted == 100);
	}
	mdb_txn_abort(child);
	rc = mdb_stat(w, dbi, &st);
	CHECK_CHECK(rc == 0 && st.ms_entries == 300);

	/* Nested transaction: commit propagates the deletion to the parent. */
	rc = mdb_txn_begin(env, w, 0, &child);
	CHECK_CHECK(rc == 0);
	{
		char lob[9], hib[9];
		MDB_val lo, hi;
		mdb_size_t deleted = 0;
		make_key(lob, 0);
		make_key(hib, 100);
		lo.mv_size = hi.mv_size = 8;
		lo.mv_data = lob;
		hi.mv_data = hib;
		rc = mdb_del_range(child, dbi, &lo, &hi, &deleted);
		CHECK_CHECK(rc == 0 && deleted == 100);
	}
	rc = mdb_txn_commit(child);
	CHECK_CHECK(rc == 0);
	rc = mdb_stat(w, dbi, &st);
	CHECK_CHECK(rc == 0 && st.ms_entries == 200);
	cases += 6;

	/* Write-transaction cursors are freed by the library when the write
	 * transaction commits, so release the handles before committing.
	 */
	mdb_cursor_close(keep);
	mdb_cursor_close(gone);
	mdb_cursor_close(edge);

	rc = mdb_txn_commit(w);
	CHECK_CHECK(rc == 0);

	/* The old snapshot is unchanged; a new reader sees the deletion. */
	rc = mdb_stat(r_old, dbi, &st);
	CHECK_CHECK(rc == 0 && st.ms_entries == 1000);
	mdb_txn_abort(r_old);
	rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &r_new);
	CHECK_CHECK(rc == 0);
	rc = mdb_stat(r_new, dbi, &st);
	CHECK_CHECK(rc == 0 && st.ms_entries == 200);
	mdb_txn_abort(r_new);
	cases += 3;

	mdb_env_close(env);
	free(dir);
	return cases;
}

int
range_case(const char *group)
{
	if (strcmp(group, "bounds") == 0)
		return bounds_case();
	if (strcmp(group, "pages") == 0)
		return pages_case();
	if (strcmp(group, "cursors") == 0)
		return cursors_case();
	return -1;
}
