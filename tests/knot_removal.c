/** @file knot_removal.c
 *
 * Independent tests for ts_bspline_remove_knot and ts_bspline_simplify.
 * The verification logic (Bezier extraction, Bernstein difference
 * coefficients, analytic references) is implemented here from scratch on
 * top of the public TinySpline evaluation/insertion API; the two new
 * entry points are never used to certify each other.
 */

#include "tinyspline.h"
#include "tinyspline_reduce.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CK(cond)                                                          \
	do {                                                            \
		g_checks++;                                           \
		if (!(cond)) {                                        \
			printf("ASSERT FAIL %s:%d: %s\n",            \
			       __FILE__, __LINE__, #cond);          \
			g_failures++;                               \
		}                                                     \
	} while (0)

#define CK_NEAR(a, b, eps)                                               \
	do {                                                            \
		g_checks++;                                           \
		if (fabs((tsReal)(a) - (tsReal)(b)) > (eps)) {       \
			printf("ASSERT FAIL %s:%d: |%g - %g| > %g\n",  \
			       __FILE__, __LINE__,                    \
			       (double)(a), (double)(b), (double)(eps));\
			g_failures++;                               \
		}                                                     \
	} while (0)

/* ------------------------------------------------------------------ */
/* Construction helpers                                                */
/* ------------------------------------------------------------------ */

static tsBSpline
make_spline(size_t ncp, size_t dim, size_t deg,
            const tsReal *knots, const tsReal *cp)
{
	tsBSpline s = ts_bspline_init();
	tsStatus st;
	if (ts_bspline_new(ncp, dim, deg, TS_OPENED, &s, &st) != TS_SUCCESS ||
	    ts_bspline_set_knots(&s, knots, &st) != TS_SUCCESS ||
	    ts_bspline_set_control_points(&s, cp, &st) != TS_SUCCESS) {
		printf("make_spline failed: %s\n", st.message);
		exit(2);
	}
	return s;
}

static tsBSpline
make_clamped_uniform(size_t ncp, size_t dim, size_t deg, const tsReal *cp)
{
	tsBSpline s = ts_bspline_init();
	tsStatus st;
	if (ts_bspline_new(ncp, dim, deg, TS_CLAMPED, &s, &st) != TS_SUCCESS) {
		printf("make_clamped_uniform failed: %s\n", st.message);
		exit(2);
	}
	if (ts_bspline_set_control_points(&s, cp, &st) != TS_SUCCESS) {
		printf("set cp failed: %s\n", st.message);
		exit(2);
	}
	return s;
}

static size_t
knot_multiplicity(const tsBSpline *s, tsReal value)
{
	const tsReal *k = ts_bspline_knots_ptr(s);
	size_t n = ts_bspline_num_knots(s), i, m = 0;
	for (i = 0; i < n; i++)
		if (ts_knots_equal(k[i], value))
			m++;
	return m;
}

/* ------------------------------------------------------------------ */
/* Independent evaluation and Bezier machinery                         */
/* ------------------------------------------------------------------ */

static int
eval_point(const tsBSpline *s, tsReal u, tsReal *out)
{
	tsDeBoorNet net = ts_deboornet_init();
	tsReal *r = NULL;
	int ok = 0;
	if (ts_bspline_eval(s, u, &net, NULL) == TS_SUCCESS &&
	    ts_deboornet_result(&net, &r, NULL) == TS_SUCCESS) {
		memcpy(out, r, ts_bspline_dimension(s) * sizeof(tsReal));
		ok = 1;
	}
	free(r);
	ts_deboornet_free(&net);
	return ok;
}

/* Dense, independent sampling maximum (a diagnostic only; never used as
 * the acceptance certificate). */
static tsReal
sampled_distance(const tsBSpline *a, const tsBSpline *b, int steps)
{
	tsReal lo, hi, worst = 0;
	size_t dim = ts_bspline_dimension(a);
	int i;
	ts_bspline_domain(a, &lo, &hi);
	for (i = 0; i <= steps; i++) {
		tsReal u = lo + (hi - lo) * i / (tsReal) steps;
		tsReal pa[3], pb[3];
		if (!eval_point(a, u, pa) || !eval_point(b, u, pb))
			continue;
		{
			tsReal d = ts_distance(pa, pb, dim);
			if (d > worst)
				worst = d;
		}
	}
	return worst;
}

/*
 * Independently refines `s` to full (order) multiplicity at every value
 * in `targets`, using only the public knot insertion entry point. The
 * result is the common Bezier decomposition on that knot union.
 */
static tsBSpline
independent_refine_on(const tsBSpline *s, const tsReal *targets,
                      size_t ntargets)
{
	tsBSpline work = ts_bspline_init();
	tsBSpline tmp = ts_bspline_init();
	const size_t order = ts_bspline_order(s);
	size_t i, m;
	tsStatus st;
	if (ts_bspline_copy(s, &work, &st) != TS_SUCCESS)
		exit(2);
	for (i = 0; i < ntargets; i++) {
		m = knot_multiplicity(&work, targets[i]);
		if (m >= order)
			continue;
		if (ts_bspline_insert_knot(&work, targets[i], order - m,
		                           &tmp, &(size_t){0}, &st) != TS_SUCCESS) {
			printf("independent refinement failed: %s\n", st.message);
			exit(2);
		}
		ts_bspline_free(&work);
		ts_bspline_move(&tmp, &work);
	}
	ts_bspline_free(&tmp);
	return work;
}

/* Distinct interior knot values of `s` in ascending order. */
static size_t
independent_distinct_knots(const tsBSpline *s, tsReal *out, size_t cap)
{
	const tsReal *knots = ts_bspline_knots_ptr(s);
	size_t n = ts_bspline_num_knots(s);
	size_t order = ts_bspline_order(s);
	size_t i, count = 0;
	for (i = order; i < n - order; i++) {
		if (count > 0 && ts_knots_equal(out[count - 1], knots[i]))
			continue;
		if (count >= cap)
			exit(2);
		out[count++] = knots[i];
	}
	return count;
}

/*
 * Independent certified bound: common Bezier segments on the SOURCE knot
 * vector. Both curves are fully refined on the source's distinct knots,
 * their Bernstein difference control vectors are formed segment by
 * segment and the maximum Euclidean norm is returned. The convex hull
 * property makes this a genuine parameter-domain-wide upper bound.
 */
static tsReal
independent_bernstein_bound(const tsBSpline *source, const tsBSpline *cand)
{
	tsReal targets[256];
	size_t ntargets = independent_distinct_knots(source, targets, 256);
	tsBSpline ba = independent_refine_on(source, targets, ntargets);
	tsBSpline bc = independent_refine_on(cand, targets, ntargets);
	const size_t dim = ts_bspline_dimension(source);
	const size_t p = ts_bspline_degree(source);
	const tsReal *ca = ts_bspline_control_points_ptr(&ba);
	const tsReal *cc = ts_bspline_control_points_ptr(&bc);
	const size_t nca = ts_bspline_num_control_points(&ba);
	const size_t ncb = ts_bspline_num_control_points(&bc);
	tsReal worst = 0;
	size_t i;
	CK(nca == ncb);
	CK(ts_bspline_num_knots(&ba) == ts_bspline_num_knots(&bc));
	if (nca != ncb) {
		ts_bspline_free(&ba);
		ts_bspline_free(&bc);
		return 1e300;
	}
	for (i = 0; i < nca; i++) {
		tsReal d[3];
		size_t k;
		for (k = 0; k < dim; k++)
			d[k] = cc[i * dim + k] - ca[i * dim + k];
		{
			tsReal norm = ts_distance(d,
			            (tsReal[3]){0, 0, 0}, dim);
			if (norm > worst)
				worst = norm;
		}
	}
	/* Degree must match the documented supported range. */
	CK(p >= 1 && p <= 3);
	ts_bspline_free(&ba);
	ts_bspline_free(&bc);
	return worst;
}

/* Certifies `cand` against `source`: reported bound is an upper bound,
 * dominates the sampled distance, and respects the tolerance. */
static void
check_certificate(const tsBSpline *source, const tsBSpline *cand,
                  tsReal reported, tsReal tolerance)
{
	tsReal independent = independent_bernstein_bound(source, cand);
	tsReal sampled = sampled_distance(source, cand, 300);
	CK(reported <= tolerance + 1e-15);
	/* The independent convex-hull bound and the reported bound must both
	 * dominate the actual sampled deviation. */
	CK(sampled <= independent + 1e-9 * (1 + independent));
	CK(sampled <= reported + 1e-9 * (1 + reported));
	/* Both certificates must be of the same order of magnitude. */
	CK(reported <= independent * (1 + 1e-6) + 1e-12 ||
	   independent <= reported * (1 + 1e-6) + 1e-12);
}

/* ------------------------------------------------------------------ */
/* Individual scenarios                                                */
/* ------------------------------------------------------------------ */

/* Analytic straight line: every interior knot is exactly removable. */
static void
test_straight_line(void)
{
	/* degree 1, 2D, line from (0,0) to (4,2), extra collinear knots */
	tsReal knots[] = {0, 0, 1, 2, 3, 4, 4};
	tsReal cp[] = {0, 0, 1, 0.5, 2, 1, 3, 1.5, 4, 2};
	tsBSpline s = make_spline(5, 2, 1, knots, cp);
	tsBSpline out = ts_bspline_init();
	tsStatus st;
	size_t removed;
	tsReal bound;
	tsError e;

	e = ts_bspline_remove_knot(&s, 2.0, 1, 1e-9, &out,
	                           &removed, &bound, &st);
	CK(e == TS_SUCCESS);
	CK(removed == 1);
	CK(ts_bspline_num_control_points(&out) == 4);
	CK(knot_multiplicity(&out, 2.0) == 0);
	CK(bound < 1e-10);
	/* Analytic line points must be reproduced exactly. */
	{
		tsReal p[3];
		CK(eval_point(&out, 0.0, p));
		CK_NEAR(p[0], 0.0, 1e-12); CK_NEAR(p[1], 0.0, 1e-12);
		CK(eval_point(&out, 2.0, p));
		CK_NEAR(p[0], 2.0, 1e-12); CK_NEAR(p[1], 1.0, 1e-12);
		CK(eval_point(&out, 4.0, p));
		CK_NEAR(p[0], 4.0, 1e-12); CK_NEAR(p[1], 2.0, 1e-12);
	}
	check_certificate(&s, &out, bound, 1e-9);
	ts_bspline_free(&out);

	/* Full simplify collapses the polyline to two control points. */
	e = ts_bspline_simplify(&s, 1e-9, &out, &removed, &bound, &st);
	CK(e == TS_SUCCESS);
	CK(removed == 3);
	CK(ts_bspline_num_control_points(&out) == 2);
	CK(ts_bspline_num_knots(&out) == 4);
	CK(bound < 1e-10);
	check_certificate(&s, &out, bound, 1e-9);
	ts_bspline_free(&out);
	ts_bspline_free(&s);
}

/* Cubic curve: exact knot insertion must be exactly invertible. */
static void
test_cubic_exact_inverse(void)
{
	tsReal cp[] = {0, 0, 0,  1, 2, 0,  2, -1, 1,
	               3, 0, 1,  4, 2, 0,  5, 0, 1};
	tsBSpline base = make_clamped_uniform(6, 3, 3, cp);
	tsBSpline ins = ts_bspline_init();
	tsBSpline red = ts_bspline_init();
	tsStatus st;
	size_t k, removed;
	tsReal bound;

	CK(ts_bspline_insert_knot(&base, 0.35, 1, &ins, &k, &st) == TS_SUCCESS);
	CK(ts_bspline_insert_knot(&ins, 0.7, 2, &red, &k, &st) == TS_SUCCESS);
	ts_bspline_free(&ins);
	ins = red; red = ts_bspline_init();
	CK(ts_bspline_num_control_points(&ins) == 9);

	CK(ts_bspline_remove_knot(&ins, 0.35, 1, 1e-9, &red,
	                          &removed, &bound, &st) == TS_SUCCESS);
	CK(removed == 1);
	CK(bound < 1e-10);
	check_certificate(&ins, &red, bound, 1e-9);
	ts_bspline_free(&red);

	/* Remove all inserted copies at once; must reproduce base. */
	CK(ts_bspline_remove_knot(&ins, 0.7, 2, 1e-9, &red,
	                          &removed, &bound, &st) == TS_SUCCESS);
	CK(removed == 2);
	CK(ts_bspline_num_control_points(&red) == 7);
	CK(bound < 1e-10);

	/* Then the remaining inserted knot. */
	{
		tsBSpline red2 = ts_bspline_init();
		size_t rem2;
		tsReal b2;
		CK(ts_bspline_remove_knot(&red, 0.35, 1, 1e-9, &red2,
		                          &rem2, &b2, &st) == TS_SUCCESS);
		CK(rem2 == 1);
		CK(ts_bspline_num_control_points(&red2) == 6);
		CK(b2 < 1e-10);
		check_certificate(&base, &red2, b2, 1e-9);
		/* Control points match the original to round-off. */
		{
			const tsReal *a = ts_bspline_control_points_ptr(&base);
			const tsReal *b = ts_bspline_control_points_ptr(&red2);
			size_t i;
			for (i = 0; i < 18; i++)
				CK(fabs(a[i] - b[i]) < 1e-10);
		}
		ts_bspline_free(&red2);
	}
	ts_bspline_free(&red);
	ts_bspline_free(&ins);
	ts_bspline_free(&base);
}

/* Non-unit parameter domain [2, 10]. */
static void
test_non_unit_domain(void)
{
	tsReal knots[] = {2, 2, 2, 2, 5, 8, 10, 10, 10, 10};
	tsReal cp[] = {0, 0, 0,  1, 2, 0,  2, -1, 1,
	               3, 0, 1,  4, 2, 0,  5, 0, 1};
	tsBSpline s = make_spline(6, 3, 3, knots, cp);
	tsBSpline ins = ts_bspline_init(), red = ts_bspline_init();
	tsStatus st;
	size_t k, removed;
	tsReal bound, lo, hi;

	CK(ts_bspline_insert_knot(&s, 6.5, 2, &ins, &k, &st) == TS_SUCCESS);
	CK(ts_bspline_remove_knot(&ins, 6.5, 2, 1e-9, &red,
	                          &removed, &bound, &st) == TS_SUCCESS);
	CK(removed == 2);
	CK(ts_bspline_num_control_points(&red) == 6);
	CK(bound < 1e-10);

	/* Domain and endpoints are preserved. */
	ts_bspline_domain(&red, &lo, &hi);
	CK_NEAR(lo, 2.0, 1e-12);
	CK_NEAR(hi, 10.0, 1e-12);
	{
		tsReal pa[3], pb[3];
		CK(eval_point(&s, 2.0, pa));
		CK(eval_point(&red, 2.0, pb));
		CK(ts_distance(pa, pb, 3) < 1e-12);
		CK(eval_point(&s, 10.0, pa));
		CK(eval_point(&red, 10.0, pb));
		CK(ts_distance(pa, pb, 3) < 1e-12);
	}
	/* Knot values were not moved. */
	{
		const tsReal *rk = ts_bspline_knots_ptr(&red);
		CK_NEAR(rk[4], 5.0, 1e-12);
		CK_NEAR(rk[5], 8.0, 1e-12);
	}
	check_certificate(&s, &red, bound, 1e-9);
	ts_bspline_free(&s);
	ts_bspline_free(&ins);
	ts_bspline_free(&red);
}

/* C0 interior knot (multiplicity == degree) of a cubic curve. */
static void
test_c0_interior(void)
{
	/* Single shared joint control point P3 = (1,0); genuine corner. */
	tsReal knots[] = {0, 0, 0, 0, 0.5, 0.5, 0.5, 1, 1, 1, 1};
	tsReal cp[] = {0, 0, 0, 1, 1, 1, 1, 0,
	               2, -1, 2, 0, 3, 0};
	tsBSpline s = make_spline(7, 2, 3, knots, cp);
	tsBSpline out = ts_bspline_init();
	tsStatus st;
	size_t removed;
	tsReal bound;

	CK(knot_multiplicity(&s, 0.5) == 3);
	/* Tight tolerance: the corner cannot be smoothed away. */
	CK(ts_bspline_remove_knot(&s, 0.5, 1, 1e-7, &out,
	                          &removed, &bound, &st) == TS_SUCCESS);
	CK(removed == 0);
	CK(ts_bspline_num_control_points(&out) == 7);
	CK(knot_multiplicity(&out, 0.5) == 3);
	ts_bspline_free(&out);

	/* But a C0 knot obtained by triple insertion of a smooth curve is
	 * exactly reversible. */
	{
		tsReal smooth_cp[] = {0, 0, 1, 1, 2, 0, 3, 1};
		tsBSpline smooth = make_clamped_uniform(4, 2, 3, smooth_cp);
		tsBSpline tri = ts_bspline_init(), back = ts_bspline_init();
		size_t kk;
		CK(ts_bspline_insert_knot(&smooth, 0.5, 3, &tri, &kk,
		                          &st) == TS_SUCCESS);
		CK(knot_multiplicity(&tri, 0.5) == 3);
		CK(ts_bspline_remove_knot(&tri, 0.5, 3, 1e-9, &back,
		                          &removed, &bound, &st) == TS_SUCCESS);
		CK(removed == 3);
		CK(ts_bspline_num_control_points(&back) == 4);
		CK(bound < 1e-10);
		check_certificate(&smooth, &back, bound, 1e-9);
		ts_bspline_free(&smooth);
		ts_bspline_free(&tri);
		ts_bspline_free(&back);
	}
	ts_bspline_free(&s);
}

/* True discontinuity barrier: interior multiplicity == order. */
static void
test_fracture_barrier(void)
{
	tsReal knots[] = {0, 0, 0, 0, 0.5, 0.5, 0.5, 0.5, 1, 1, 1, 1};
	tsReal cp[] = {0, 0, 0, 1, 1, 1, 1, 0,
	               9, 9, 9, 8, 8, 8, 8, 9};
	tsBSpline s = make_spline(8, 2, 3, knots, cp);
	tsBSpline out = ts_bspline_init();
	tsStatus st;
	size_t removed;
	tsReal bound;

	CK(ts_bspline_remove_knot(&s, 0.5, 3, 1e-2, &out,
	                          &removed, &bound, &st) == TS_SUCCESS);
	CK(removed == 0);
	CK(ts_bspline_num_control_points(&out) == 8);
	CK(knot_multiplicity(&out, 0.5) == 4);
	ts_bspline_free(&out);

	CK(ts_bspline_simplify(&s, 1e-2, &out, &removed, &bound,
	                       &st) == TS_SUCCESS);
	CK(removed == 0);
	CK(knot_multiplicity(&out, 0.5) == 4);

	/* One-sided evaluation exposes the two different limit points. */
	{
		tsDeBoorNet net = ts_deboornet_init();
		tsReal *r = NULL;
		CK(ts_bspline_eval(&out, 0.5, &net, &st) == TS_SUCCESS);
		CK(ts_deboornet_num_result(&net) == 2);
		CK(ts_deboornet_result(&net, &r, &st) == TS_SUCCESS);
		CK(ts_distance(r, r + 2, 2) > 1.0);
		free(r);
		ts_deboornet_free(&net);
	}
	ts_bspline_free(&out);
	ts_bspline_free(&s);
}

/* Spatially close but parametrically distinct pieces: proximity must not
 * cause a barrier knot to be removed. */
static void
test_close_but_distinct_pieces(void)
{
	/* Two pieces whose joint points are only 1e-4 apart in space but the
	 * knot is a full-multiplicity fracture barrier. */
	tsReal knots[] = {0, 0, 0, 0, 0.5, 0.5, 0.5, 0.5, 1, 1, 1, 1};
	tsReal cp[] = {0, 0, 0, 1, 1, 1, 1, 0,
	               1.0001, 0, 2, 0, 2, 1, 3, 1};
	tsBSpline s = make_spline(8, 2, 3, knots, cp);
	tsBSpline out = ts_bspline_init();
	tsStatus st;
	size_t removed;
	tsReal bound;

	CK(ts_bspline_simplify(&s, 1e-2, &out, &removed, &bound,
	                       &st) == TS_SUCCESS);
	CK(removed == 0);
	CK(knot_multiplicity(&out, 0.5) == 4);
	ts_bspline_free(&out);
	ts_bspline_free(&s);
}

/* The mandatory approximate degree-1 case. */
static void
test_mandatory_linear_approx(void)
{
	tsReal knots[] = {0, 0, 0.5, 1, 1};
	tsReal cp[] = {0, 0, 0.5, 1e-5, 1, 0};
	tsBSpline s = make_spline(3, 2, 1, knots, cp);
	tsBSpline out = ts_bspline_init();
	tsStatus st;
	size_t removed;
	tsReal bound;

	/* Generous tolerance: .5 must be removed. */
	CK(ts_bspline_remove_knot(&s, 0.5, 1, 2e-5, &out,
	                          &removed, &bound, &st) == TS_SUCCESS);
	CK(removed == 1);
	CK(ts_bspline_num_control_points(&out) == 2);
	CK(knot_multiplicity(&out, 0.5) == 0);
	CK(bound <= 2e-5);
	/* The true maximum deviation of this polyline is exactly 1e-5 at the
	 * midpoint; the convex hull certificate captures it. */
	CK(bound >= 1e-5 - 1e-12);
	{
		tsReal sampled = sampled_distance(&s, &out, 500);
		CK(sampled <= bound + 1e-12);
		CK_NEAR(sampled, 1e-5, 1e-9);
	}
	/* Endpoints fixed. */
	{
		tsReal p[3];
		CK(eval_point(&out, 0, p));
		CK_NEAR(p[0], 0, 1e-15); CK_NEAR(p[1], 0, 1e-15);
		CK(eval_point(&out, 1, p));
		CK_NEAR(p[0], 1, 1e-15); CK_NEAR(p[1], 0, 1e-15);
	}
	ts_bspline_free(&out);

	/* Tight tolerance: .5 must survive. */
	CK(ts_bspline_remove_knot(&s, 0.5, 1, 1e-6, &out,
	                          &removed, &bound, &st) == TS_SUCCESS);
	CK(removed == 0);
	CK(ts_bspline_num_control_points(&out) == 3);
	CK(knot_multiplicity(&out, 0.5) == 1);
	ts_bspline_free(&out);
	ts_bspline_free(&s);
}

/* Cumulative perturbations: each single removal changes the local
 * polygon only slightly, but deleting every knot would make the curve
 * collapse onto its endpoint chord whose deviation from the original
 * exceeds the budget. Because every step is certified against the
 * original curve (not the running polygon), simplify must stop before
 * the accumulated deviation leaves the tolerance. */
static void
test_cumulative_budget(void)
{
	/* Degree-1 chain whose interior bumps grow from 2e-6 to 8e-6. */
	tsReal knots[] = {0, 0, .2, .4, .6, .8, 1, 1};
	tsReal cp[] = {0, 0,
	                .2, 2e-6,
	                .4, 4e-6,
	                .6, 6e-6,
	                .8, 8e-6,
	                1, 0};
	tsBSpline s = make_spline(6, 2, 1, knots, cp);
	tsBSpline out = ts_bspline_init();
	tsStatus st;
	size_t removed;
	tsReal bound;
	const tsReal tol = 5e-6;

	CK(ts_bspline_simplify(&s, tol, &out, &removed, &bound,
	                       &st) == TS_SUCCESS);
	CK(bound <= tol);
	/* The endpoint chord reaches y = 8e-6, above the 5e-6 budget, so at
	 * least one interior knot must survive even though each individual
	 * removal is locally small. */
	CK(ts_bspline_num_control_points(&out) >= 3);
	check_certificate(&s, &out, bound, tol);
	{
		tsReal sampled = sampled_distance(&s, &out, 500);
		CK(sampled <= bound + 1e-12);
		CK(sampled <= tol);
	}
	ts_bspline_free(&out);

	/* A much larger tolerance removes every interior knot. */
	CK(ts_bspline_simplify(&s, 1e-2, &out, &removed, &bound,
	                       &st) == TS_SUCCESS);
	CK(removed == 4);
	CK(ts_bspline_num_control_points(&out) == 2);
	CK(bound <= 1e-2);
	ts_bspline_free(&out);
	ts_bspline_free(&s);
}

/* count == 0 yields an independent deep copy; aliasing and error
 * preservation are exercised. */
static void
test_copy_alias_and_errors(void)
{
	tsReal knots[] = {0, 0, 0.5, 1, 1};
	tsReal cp[] = {0, 0, 0.5, 1e-5, 1, 0};
	tsBSpline s = make_spline(3, 2, 1, knots, cp);
	tsBSpline out = ts_bspline_init();
	tsStatus st;
	size_t removed;
	tsReal bound = -1;
	const tsReal *p0, *p1;

	/* count == 0: deep copy, independent memory. */
	CK(ts_bspline_remove_knot(&s, 0.5, 0, 1e-5, &out,
	                          &removed, &bound, &st) == TS_SUCCESS);
	CK(removed == 0);
	CK(bound == 0);
	CK(ts_bspline_num_control_points(&out) == 3);
	p0 = ts_bspline_control_points_ptr(&s);
	p1 = ts_bspline_control_points_ptr(&out);
	CK(p0 != p1);
	ts_bspline_free(&out);

	/* In-place simplify keeps the object usable afterwards. */
	CK(ts_bspline_simplify(&s, 2e-5, &s, &removed, &bound,
	                       &st) == TS_SUCCESS);
	CK(removed == 1);
	{
		tsReal p[3];
		CK(eval_point(&s, 0.5, p));
		CK_NEAR(p[0], 0.5, 1e-12);
	}

	/* In-place call that errors must leave the object intact. */
	{
		size_t cp_before = ts_bspline_num_control_points(&s);
		tsError e = ts_bspline_remove_knot(&s, 0.9, 1, 1e-5, &s,
		                                   &removed, &bound, &st);
		CK(e == TS_INDEX_ERROR);
		CK(ts_bspline_num_control_points(&s) == cp_before);
		{
			tsReal p[3];
			CK(eval_point(&s, 0.5, p));
			CK_NEAR(p[0], 0.5, 1e-12);
		}
	}

	/* Error path: outputs keep their previous values. */
	{
		tsBSpline guard = ts_bspline_init();
		size_t rem_guard = 777;
		tsReal bnd_guard = 123.0;
		CK(ts_bspline_remove_knot(&s, 0.0, 1, 1e-5, &guard,
		                          &rem_guard, &bnd_guard,
		                          &st) == TS_U_UNDEFINED);
		CK(rem_guard == 777);
		CK(bnd_guard == 123.0);
		/* The untouched output still has no implementation. */
		CK(guard.pImpl == NULL);
		ts_bspline_free(&guard);
	}
	ts_bspline_free(&s);
}

/* Knot matching and validation errors. */
static void
test_match_and_validation(void)
{
	tsReal knots[] = {0, 0, 0.5, 1, 1};
	tsReal cp[] = {0, 0, 0.5, 1e-5, 1, 0};
	tsBSpline s = make_spline(3, 2, 1, knots, cp);
	tsBSpline out = ts_bspline_init();
	tsStatus st;
	size_t removed;
	tsReal bound;

	CK(ts_bspline_remove_knot(&s, 0.31, 1, 1e-5, &out, &removed,
	                          &bound, &st) == TS_INDEX_ERROR);
	CK(st.code == TS_INDEX_ERROR);
	CK(ts_bspline_remove_knot(&s, 0.0, 1, 1e-5, &out, &removed,
	                          &bound, &st) == TS_U_UNDEFINED);
	CK(ts_bspline_remove_knot(&s, 1.0, 1, 1e-5, &out, &removed,
	                          &bound, &st) == TS_U_UNDEFINED);
	CK(ts_bspline_remove_knot(&s, 0.5, 1, 1e-10, &out, &removed,
	                          &bound, &st) == TS_U_UNDEFINED);
	CK(ts_bspline_remove_knot(&s, 0.5, 1, 1e-1, &out, &removed,
	                          &bound, &st) == TS_U_UNDEFINED);
	CK(ts_bspline_simplify(&s, 1e-10, &out, &removed, &bound,
	                       &st) == TS_U_UNDEFINED);
	/* Status message is populated. */
	CK(st.message[0] != '\0');
	ts_bspline_free(&out);
	ts_bspline_free(&s);

	/* A non-clamped knot vector (end multiplicity below the order) is
	 * rejected explicitly. */
	{
		tsReal k3[] = {0.0, 0.2, 0.5, 0.8, 1.0};
		tsReal c3[] = {0, 0, 0.5, 1e-5, 1, 0};
		tsBSpline nc = ts_bspline_init();
		if (ts_bspline_new(3, 2, 1, TS_OPENED, &nc, &st) == TS_SUCCESS &&
		    ts_bspline_set_knots(&nc, k3, &st) == TS_SUCCESS &&
		    ts_bspline_set_control_points(&nc, c3, &st) == TS_SUCCESS) {
			tsBSpline o3 = ts_bspline_init();
			CK(ts_bspline_remove_knot(&nc, 0.5, 1, 1e-5, &o3,
			                          &removed, &bound,
			                          &st) == TS_MULTIPLICITY);
			CK(st.code == TS_MULTIPLICITY);
			CK(ts_bspline_simplify(&nc, 1e-5, &o3, &removed,
			                       &bound, &st) == TS_MULTIPLICITY);
			ts_bspline_free(&o3);
		}
		ts_bspline_free(&nc);
	}

	/* Ambiguous match: two epsilon-distinct stored knots (gap 1.5e-4,
	 * so not equal per TS_KNOT_EPSILON) each lie within TS_KNOT_EPSILON
	 * of the query 0.500075, so the removal target is not unique. */
	{
		tsReal k2[] = {0, 0, 0.5, 0.50015, 1, 1};
		tsReal c2[] = {0, 0, 0.25, 1e-5, 0.75, 1e-5, 1, 0};
		tsBSpline t = ts_bspline_init();
		tsStatus st2;
		if (ts_bspline_new(4, 2, 1, TS_OPENED, &t, &st2) == TS_SUCCESS &&
		    ts_bspline_set_knots(&t, k2, &st2) == TS_SUCCESS &&
		    ts_bspline_set_control_points(&t, c2, &st2) == TS_SUCCESS) {
			tsBSpline o2 = ts_bspline_init();
			tsError e = ts_bspline_remove_knot(
				&t, 0.500075, 1, 1e-5, &o2, &removed,
				&bound, &st2);
			CK(e == TS_INDEX_ERROR);
			CK(st2.code == TS_INDEX_ERROR);
			ts_bspline_free(&o2);
		}
		ts_bspline_free(&t);
	}
}

/* Degree 2 coverage and repeated accept/reject alternation. */
static void
test_quadratic_and_alternation(void)
{
	tsReal cp[] = {0, 0, 1, 2, 2, -1, 3, 0, 4, 1};
	tsBSpline s = make_clamped_uniform(5, 2, 2, cp);
	tsBSpline ins = ts_bspline_init(), red = ts_bspline_init();
	tsBSpline pert = ts_bspline_init();
	tsStatus st;
	size_t k, removed;
	tsReal bound;
	int round;

	CK(ts_bspline_insert_knot(&s, 0.4, 2, &ins, &k, &st) == TS_SUCCESS);
	CK(ts_bspline_num_control_points(&ins) == 7);

	/* Perturbed material: one interior control point nudged by 5e-7 so
	 * that removal cannot meet the tightest tolerance. */
	CK(ts_bspline_copy(&ins, &pert, &st) == TS_SUCCESS);
	{
		tsReal *pc = NULL;
		CK(ts_bspline_control_points(&pert, &pc, &st) == TS_SUCCESS);
		pc[2 * 2 + 1] += 5e-7;
		CK(ts_bspline_set_control_points(&pert, pc, &st) == TS_SUCCESS);
		free(pc);
	}

	for (round = 0; round < 2; round++) {
		tsBSpline attempt = ts_bspline_init();
		/* Tight but in-range tolerance: reconstruction rejected. */
		CK(ts_bspline_remove_knot(&pert, 0.4, 1, 1e-9, &attempt,
		                          &removed, &bound, &st) == TS_SUCCESS);
		CK(removed == 0);
		/* Rejection leaves the input fully evaluable. */
		{
			tsReal p[3];
			CK(eval_point(&pert, 0.4, p));
		}
		ts_bspline_free(&attempt);

		/* Looser tolerance: one copy is accepted. */
		CK(ts_bspline_remove_knot(&pert, 0.4, 1, 1e-6, &attempt,
		                          &removed, &bound, &st) == TS_SUCCESS);
		CK(removed == 1);
		CK(bound <= 1e-6);
		ts_bspline_free(&pert);
		pert = attempt;
	}
	CK(ts_bspline_num_control_points(&pert) == 5);
	ts_bspline_free(&pert);

	/* The exact material is fully reversible with two separate calls,
	 * and remains evaluable after each accept. */
	for (round = 0; round < 2; round++) {
		CK(ts_bspline_remove_knot(&ins, 0.4, 1, 1e-9, &red,
		                          &removed, &bound, &st) == TS_SUCCESS);
		CK(removed == 1);
		CK(bound < 1e-10);
		{
			tsReal p[3];
			CK(eval_point(&red, 0.4, p));
		}
		ts_bspline_free(&ins);
		ins = red;
		red = ts_bspline_init();
	}
	CK(ts_bspline_num_control_points(&ins) == 5);
	check_certificate(&s, &ins, bound, 1e-9);
	ts_bspline_free(&red);
	ts_bspline_free(&ins);
	ts_bspline_free(&s);
}

/* Non-finite coordinates and out-of-range input are explicit errors. */
static void
test_input_validation(void)
{
	tsStatus st;
	tsBSpline out = ts_bspline_init();
	size_t removed;
	tsReal bound;

	/* Non-finite control point. */
	{
		tsReal knots[] = {0, 0, 0.5, 1, 1};
		tsReal cp[] = {0, 0, 0.5, (tsReal) NAN, 1, 0};
		tsBSpline s = make_spline(3, 2, 1, knots, cp);
		CK(ts_bspline_remove_knot(&s, 0.5, 1, 1e-5, &out,
		                          &removed, &bound,
		                          &st) == TS_NO_RESULT);
		ts_bspline_free(&s);
	}

	/* Coordinate above the 1e4 limit. */
	{
		tsReal knots[] = {0, 0, 0.5, 1, 1};
		tsReal cp[] = {0, 0, 0.5, 2e4, 1, 0};
		tsBSpline s = make_spline(3, 2, 1, knots, cp);
		CK(ts_bspline_simplify(&s, 1e-5, &out, &removed,
		                       &bound, &st) == TS_NO_RESULT);
		ts_bspline_free(&s);
	}

	/* Unsupported degree 4 and dimension 1. */
	{
		tsBSpline d4 = ts_bspline_init();
		tsBSpline d1 = ts_bspline_init();
		tsReal cp5[10] = {0};
		tsReal cp1[5] = {0, 1, 2, 3, 4};
		if (ts_bspline_new(5, 2, 4, TS_CLAMPED, &d4, &st) == TS_SUCCESS &&
		    ts_bspline_set_control_points(&d4, cp5, &st) == TS_SUCCESS)
			CK(ts_bspline_simplify(&d4, 1e-5, &out, &removed,
			                       &bound, &st) == TS_DEG_GE_NCTRLP);
		if (ts_bspline_new(5, 1, 1, TS_CLAMPED, &d1, &st) == TS_SUCCESS &&
		    ts_bspline_set_control_points(&d1, cp1, &st) == TS_SUCCESS)
			CK(ts_bspline_simplify(&d1, 1e-5, &out, &removed,
			                       &bound, &st) == TS_DIM_ZERO);
		ts_bspline_free(&d4);
		ts_bspline_free(&d1);
	}

	/* Knot outside the +-100 range. */
	{
		tsReal knots[] = {100, 100, 100.5, 101, 101};
		tsReal cp[] = {0, 0, 0.5, 1e-5, 1, 0};
		tsBSpline s = make_spline(3, 2, 1, knots, cp);
		CK(ts_bspline_remove_knot(&s, 100.5, 1, 1e-5, &out,
		                          &removed, &bound,
		                          &st) == TS_NO_RESULT);
		ts_bspline_free(&s);
	}
	ts_bspline_free(&out);
}

/* A 3D approximate removal that is not an exact inverse. */
static void
test_three_d_approx(void)
{
	/* Degree 1 in 3D with a small off-chord middle control point. */
	tsReal knots[] = {0, 0, 0.5, 1, 1};
	tsReal cp[] = {0, 0, 0,  0.5, 4e-6, 2e-6,  1, 0, 0};
	tsBSpline s = make_spline(3, 3, 1, knots, cp);
	tsBSpline out = ts_bspline_init();
	tsStatus st;
	size_t removed;
	tsReal bound;

	CK(ts_bspline_remove_knot(&s, 0.5, 1, 1e-5, &out,
	                          &removed, &bound, &st) == TS_SUCCESS);
	CK(removed == 1);
	CK(ts_bspline_num_control_points(&out) == 2);
	CK(ts_bspline_dimension(&out) == 3);
	CK(bound <= 1e-5);
	/* The middle-point deviation norm is sqrt((4e-6)^2 + (2e-6)^2). */
	CK_NEAR(bound, sqrt(20e-12), 1e-9);
	/* Endpoints preserved in all three components. */
	{
		tsReal p[3];
		CK(eval_point(&out, 0, p));
		CK_NEAR(p[0], 0, 1e-15); CK_NEAR(p[2], 0, 1e-15);
		CK(eval_point(&out, 1, p));
		CK_NEAR(p[0], 1, 1e-15); CK_NEAR(p[2], 0, 1e-15);
	}
	check_certificate(&s, &out, bound, 1e-5);
	ts_bspline_free(&out);

	/* Tight tolerance keeps the knot. */
	CK(ts_bspline_remove_knot(&s, 0.5, 1, 1e-9, &out,
	                          &removed, &bound, &st) == TS_SUCCESS);
	CK(removed == 0);
	CK(ts_bspline_num_control_points(&out) == 3);
	ts_bspline_free(&out);
	ts_bspline_free(&s);
}

/* count larger than the multiplicity and partial acceptance. */
static void
test_count_cap_and_partial(void)
{
	tsReal cp[] = {0, 0, 1, 2, 2, -1, 3, 0, 4, 1};
	tsBSpline s = make_clamped_uniform(5, 2, 2, cp);
	tsBSpline ins = ts_bspline_init();
	tsBSpline out = ts_bspline_init();
	tsStatus st;
	size_t k, removed;
	tsReal bound;

	/* Insert two copies so 0.4 has multiplicity 2. */
	CK(ts_bspline_insert_knot(&s, 0.4, 2, &ins, &k, &st) == TS_SUCCESS);
	CK(knot_multiplicity(&ins, 0.4) == 2);

	/* Request far more than available: capped to the multiplicity, all
	 * exactly reversed. */
	CK(ts_bspline_remove_knot(&ins, 0.4, 100, 1e-9, &out,
	                          &removed, &bound, &st) == TS_SUCCESS);
	CK(removed == 2);
	CK(ts_bspline_num_control_points(&out) == 5);
	CK(bound < 1e-10);
	ts_bspline_free(&out);

	/* Request exactly one: only one copy is taken. */
	CK(ts_bspline_remove_knot(&ins, 0.4, 1, 1e-9, &out,
	                          &removed, &bound, &st) == TS_SUCCESS);
	CK(removed == 1);
	CK(knot_multiplicity(&out, 0.4) == 1);
	CK(ts_bspline_num_control_points(&out) == 6);
	ts_bspline_free(&out);
	ts_bspline_free(&ins);
	ts_bspline_free(&s);
}

/* A successful call never mutates the source and the output owns its own
 * memory. */
static void
test_source_immutability(void)
{
	tsReal knots[] = {0, 0, 0.5, 1, 1};
	tsReal cp[] = {0, 0, 0.5, 1e-5, 1, 0};
	tsBSpline s = make_spline(3, 2, 1, knots, cp);
	tsBSpline out = ts_bspline_init();
	tsStatus st;
	size_t removed;
	tsReal bound;
	const tsReal *before;
	const tsReal *before_k;
	size_t i;

	before = ts_bspline_control_points_ptr(&s);
	before_k = ts_bspline_knots_ptr(&s);
	CK(ts_bspline_remove_knot(&s, 0.5, 1, 2e-5, &out,
	                          &removed, &bound, &st) == TS_SUCCESS);
	CK(removed == 1);

	/* Source control points and knots are byte-for-byte unchanged. */
	CK(ts_bspline_control_points_ptr(&s) == before);
	CK(ts_bspline_knots_ptr(&s) == before_k);
	CK(ts_bspline_num_control_points(&s) == 3);
	CK(ts_bspline_num_knots(&s) == 5);
	for (i = 0; i < 6; i++)
		CK(ts_bspline_control_points_ptr(&s)[i] == cp[i]);
	for (i = 0; i < 5; i++)
		CK(ts_bspline_knots_ptr(&s)[i] == knots[i]);

	/* Output is a separate allocation. */
	CK(ts_bspline_control_points_ptr(&out) != before);
	CK(ts_bspline_num_control_points(&out) == 2);
	ts_bspline_free(&out);
	ts_bspline_free(&s);
}

int
main(void)
{
	test_straight_line();
	test_cubic_exact_inverse();
	test_non_unit_domain();
	test_c0_interior();
	test_fracture_barrier();
	test_close_but_distinct_pieces();
	test_mandatory_linear_approx();
	test_cumulative_budget();
	test_copy_alias_and_errors();
	test_match_and_validation();
	test_quadratic_and_alternation();
	test_input_validation();
	test_three_d_approx();
	test_count_cap_and_partial();
	test_source_immutability();

	if (g_failures) {
		printf("KNOT_REMOVAL_FAILURES=%d (checks=%d)\n",
		       g_failures, g_checks);
		return 1;
	}
	printf("KNOT_REMOVAL_OK (checks=%d)\n", g_checks);
	return 0;
}
