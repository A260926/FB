/*
 * Knot removal and simplification tests.
 *
 * These tests deliberately do not let the two new APIs testify for each
 * other: error bounds are re-derived independently from B&eacute;zier
 * decompositions and de Casteljau subdivision written in this file, and
 * geometric deviations are cross-checked against analytic expressions and
 * one-sided evaluations.
 */
#include "tinyspline_reduce.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_fails = 0;

#define CHECK(cond)                                                     \
	do {                                                            \
		if (!(cond)) {                                          \
			fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
			        __FILE__, __LINE__, #cond);          \
			g_fails++;                                    \
		}                                                       \
	} while (0)

static tsBSpline
make_spline(size_t n_ctrlp,
            size_t dim,
            size_t degree,
            const tsReal *ctrlp,
            const tsReal *knots)
{
	tsStatus st;
	tsBSpline s = ts_bspline_init();
	ts_bspline_new(n_ctrlp, dim, degree, TS_CLAMPED, &s, &st);
	ts_bspline_set_control_points(&s, ctrlp, &st);
	ts_bspline_set_knots(&s, knots, &st);
	return s;
}

static void
eval_point(const tsBSpline *s, tsReal u, tsReal *out)
{
	tsStatus st;
	tsDeBoorNet net = ts_deboornet_init();
	tsReal *r = NULL;
	ts_bspline_eval(s, u, &net, &st);
	ts_deboornet_result(&net, &r, &st);
	memcpy(out, r, ts_bspline_dimension(s) * sizeof(tsReal));
	free(r);
	ts_deboornet_free(&net);
}

/* Dense, independent pointwise deviation. This is a diagnostic only; it
 * must never exceed the certified error bound. */
static tsReal
sample_max_deviation(const tsBSpline *a, const tsBSpline *b)
{
	tsReal mn, mx, worst = 0;
	size_t i, d;
	const size_t dim = ts_bspline_dimension(a);
	ts_bspline_domain(a, &mn, &mx);
	for (i = 0; i <= 500; i++) {
		tsReal u = mn + (mx - mn) * (tsReal) i / 500.0;
		tsReal pa[3], pb[3];
		eval_point(a, u, pa);
		eval_point(b, u, pb);
		tsReal v = ts_distance(pa, pb, dim);
		if (v > worst)
			worst = v;
	}
	(void) d;
	return worst;
}

/* ------------------------------------------------------------------ */
/* Independent Bezier machinery (not taken from the reduce module)    */
/* ------------------------------------------------------------------ */

static void
de_casteljau_split(tsReal a,
                   tsReal b,
                   tsReal t,
                   const tsReal *P,
                   size_t p,
                   size_t dim,
                   tsReal *work,
                   tsReal *left,
                   tsReal *right)
{
	const size_t n = p + 1;
	const tsReal alpha = (b - t) / (b - a);
	const tsReal beta = (tsReal) 1.0 - alpha;
	size_t r, i, d;
	memcpy(work, P, n * dim * sizeof(tsReal));
	memcpy(left, work, dim * sizeof(tsReal));
	memcpy(right + p * dim, work + p * dim, dim * sizeof(tsReal));
	for (r = 1; r <= p; r++) {
		for (i = 0; i + r <= p; i++)
			for (d = 0; d < dim; d++)
				work[i * dim + d] =
					alpha * work[i * dim + d] +
					beta * work[(i + 1) * dim + d];
		memcpy(left + r * dim, work, dim * sizeof(tsReal));
		memcpy(right + (p - r) * dim,
		       work + (p - r) * dim, dim * sizeof(tsReal));
	}
}

/* Independent convex-hull bound: both curves are expressed as Bezier
 * segments on the source knot partition, and the maximum norm of the
 * coefficient-wise Bernstein difference vectors is returned. */
static tsReal
independent_bernstein_bound(const tsBSpline *orig,
                            const tsBSpline *cand,
                            size_t *out_segments)
{
	tsStatus st;
	tsBSpline bzo = ts_bspline_init(), bzc = ts_bspline_init();
	const tsReal *po, *pc, *uo, *uc;
	tsReal *refined, *work, *left, *right;
	tsReal *breaks_o, *breaks_c;
	size_t order, dim, p, sego, segc, poly, j, k, piece;
	tsReal worst = 0;

	ts_bspline_to_beziers(orig, &bzo, &st);
	ts_bspline_to_beziers(cand, &bzc, &st);
	order = ts_bspline_order(&bzo);
	dim = ts_bspline_dimension(&bzo);
	p = order - 1;
	poly = order * dim;
	sego = ts_bspline_num_control_points(&bzo) / order;
	segc = ts_bspline_num_control_points(&bzc) / order;
	po = ts_bspline_control_points_ptr(&bzo);
	pc = ts_bspline_control_points_ptr(&bzc);
	uo = ts_bspline_knots_ptr(&bzo);
	uc = ts_bspline_knots_ptr(&bzc);

	breaks_o = (tsReal *) malloc((sego + 1) * sizeof(tsReal));
	breaks_c = (tsReal *) malloc((segc + 1) * sizeof(tsReal));
	refined = (tsReal *) malloc(sego * poly * sizeof(tsReal));
	work = (tsReal *) malloc(poly * sizeof(tsReal));
	left = (tsReal *) malloc(poly * sizeof(tsReal));
	right = (tsReal *) malloc(poly * sizeof(tsReal));
	for (j = 0; j <= sego; j++)
		breaks_o[j] = uo[j * order];
	for (j = 0; j <= segc; j++)
		breaks_c[j] = uc[j * order];

	piece = 0;
	for (k = 0; k < segc; k++) {
		tsReal cur = breaks_c[k];
		memcpy(work, pc + k * poly, poly * sizeof(tsReal));
		for (j = 0; j < sego; j++) {
			if (breaks_o[j] <= cur ||
			    breaks_o[j] >= breaks_c[k + 1])
				continue;
			de_casteljau_split(cur, breaks_c[k + 1], breaks_o[j],
			                   work, p, dim, left,
			                   refined + piece * poly, right);
			piece++;
			cur = breaks_o[j];
			memcpy(work, right, poly * sizeof(tsReal));
		}
		memcpy(refined + piece * poly, work,
		       poly * sizeof(tsReal));
		piece++;
	}
	CHECK(piece == sego);

	for (j = 0; j < sego; j++) {
		size_t q;
		if (fabs(breaks_o[j + 1] - breaks_o[j]) < 1e-300)
			continue; /* zero-width interval at a fracture knot */
		for (q = 0; q < order; q++) {
			tsReal diff[3];
			size_t d2;
			for (d2 = 0; d2 < dim; d2++)
				diff[d2] = po[(j * order + q) * dim + d2] -
				           refined[(j * order + q) * dim + d2];
			{
				tsReal v = ts_distance(diff,
				                       (tsReal[3]) {0, 0, 0}, dim);
				if (v > worst)
					worst = v;
			}
		}
	}

	free(breaks_o);
	free(breaks_c);
	free(refined);
	free(work);
	free(left);
	free(right);
	ts_bspline_free(&bzo);
	ts_bspline_free(&bzc);
	if (out_segments != NULL)
		*out_segments = sego;
	return worst;
}

static size_t
multiplicity(const tsBSpline *s, tsReal value)
{
	const tsReal *u = ts_bspline_knots_ptr(s);
	size_t i, m = 0;
	for (i = 0; i < ts_bspline_num_knots(s); i++)
		if (u[i] == value)
			m++;
	return m;
}

/* ------------------------------------------------------------------ */
/* Test sections                                                      */
/* ------------------------------------------------------------------ */

static void
test_analytic_line(void)
{
	tsStatus st;
	tsBSpline line, work = ts_bspline_init(), out = ts_bspline_init();
	size_t k, removed, i;
	tsReal bound;
	const tsReal A[2] = {0, 0};
	const tsReal B[2] = {10, 0};
	tsReal ctrlp[6] = {A[0], A[1], B[0], B[1]};
	tsReal knots[4] = {0, 0, 1, 1};
	tsReal insert_us[4] = {0.125, 0.375, 0.625, 0.875};

	line = make_spline(2, 2, 1, ctrlp, knots);
	for (i = 0; i < 4; i++) {
		ts_bspline_insert_knot(&line, insert_us[i], 1,
		                       &work, &k, &st);
		ts_bspline_free(&line);
		line = work;
		work = ts_bspline_init();
	}
	CHECK(ts_bspline_num_control_points(&line) == 6);

	CHECK(ts_bspline_simplify(&line, 1e-9, &out, &removed,
	                          &bound, &st) == TS_SUCCESS);
	CHECK(removed == 4);
	CHECK(ts_bspline_num_control_points(&out) == 2);
	CHECK(multiplicity(&out, 0) == 2);
	CHECK(multiplicity(&out, 1) == 2);
	CHECK(bound < 1e-10);

	for (i = 0; i <= 20; i++) {
		tsReal u = (tsReal) i / 20.0;
		tsReal want[2] = {A[0] + u * (B[0] - A[0]),
		                  A[1] + u * (B[1] - A[1])};
		tsReal got[3];
		eval_point(&out, u, got);
		CHECK(fabs(got[0] - want[0]) < 1e-10);
		CHECK(fabs(got[1] - want[1]) < 1e-10);
	}
	/* Endpoints are preserved exactly. */
	{
		tsReal p0[3], p1[3];
		eval_point(&out, 0, p0);
		eval_point(&out, 1, p1);
		CHECK(fabs(p0[0] - A[0]) < 1e-12 && fabs(p0[1] - A[1]) < 1e-12);
		CHECK(fabs(p1[0] - B[0]) < 1e-12 && fabs(p1[1] - B[1]) < 1e-12);
	}
	ts_bspline_free(&line);
	ts_bspline_free(&work);
	ts_bspline_free(&out);
}

static void
test_cubic_exact_inverse(void)
{
	tsStatus st;
	tsBSpline base, ins = ts_bspline_init(), red = ts_bspline_init();
	size_t k, removed;
	tsReal bound;
	tsReal ctrlp[8] = {0, 0, 1, 2, 3, 1, 4, 0};
	tsReal knots[8] = {0, 0, 0, 0, 1, 1, 1, 1};
	size_t seg;
	tsReal indep;

	base = make_spline(4, 2, 3, ctrlp, knots);
	ts_bspline_insert_knot(&base, 0.4, 2, &ins, &k, &st);
	CHECK(ts_bspline_num_control_points(&ins) == 6);
	CHECK(multiplicity(&ins, 0.4) == 2);

	CHECK(ts_bspline_remove_knot(&ins, 0.4, 2, 1e-9, &red,
	                             &removed, &bound, &st) == TS_SUCCESS);
	CHECK(removed == 2);
	CHECK(ts_bspline_num_control_points(&red) == 4);
	CHECK(multiplicity(&red, 0.4) == 0);
	CHECK(ts_bspline_degree(&red) == 3);
	CHECK(ts_bspline_dimension(&red) == 2);
	CHECK(bound < 1e-10);

	/* Independent certificate and dense sampling. For exactly invertible
	 * material the independently recomputed Bezier coefficients coincide
	 * bit-for-bit, so the convex hull bound is zero while de Boor
	 * evaluation still shows floating point roundoff; everything is far
	 * below the 1e-10 contract. */
	indep = independent_bernstein_bound(&ins, &red, &seg);
	CHECK(seg == 2);
	CHECK(indep < 1e-10);
	CHECK(bound < 1e-10);
	CHECK(sample_max_deviation(&ins, &red) < 1e-10);

	/* Control points of the exact inverse must match the originals. */
	{
		const tsReal *p = ts_bspline_control_points_ptr(&red);
		size_t i;
		for (i = 0; i < 8; i++)
			CHECK(fabs(p[i] - ctrlp[i]) < 1e-9);
	}
	ts_bspline_free(&base);
	ts_bspline_free(&ins);
	ts_bspline_free(&red);
}

static void
test_non_unit_domain(void)
{
	tsStatus st;
	tsBSpline s, work = ts_bspline_init(), out = ts_bspline_init();
	size_t k, removed;
	tsReal bound, mn, mx;
	tsReal ctrlp[15] = {0, 0, 0, 1, 2, 1, 3, -1, 0, 4, 1, 2, 6, 0, 1};
	tsReal knots[8] = {-2, -2, -2, 0.5, 3.25, 7, 7, 7};
	tsReal indep;

	s = make_spline(5, 3, 2, ctrlp, knots);
	ts_bspline_insert_knot(&s, -0.5, 2, &work, &k, &st);
	ts_bspline_free(&s);
	s = work;
	work = ts_bspline_init();
	ts_bspline_insert_knot(&s, 5, 1, &work, &k, &st);
	ts_bspline_free(&s);
	s = work;
	work = ts_bspline_init();
	CHECK(ts_bspline_num_control_points(&s) == 8);

	CHECK(ts_bspline_simplify(&s, 1e-9, &out, &removed,
	                          &bound, &st) == TS_SUCCESS);
	CHECK(removed == 3);
	CHECK(ts_bspline_num_control_points(&out) == 5);
	CHECK(bound < 1e-10);
	ts_bspline_domain(&out, &mn, &mx);
	CHECK(fabs(mn - (-2)) < 1e-12);
	CHECK(fabs(mx - 7) < 1e-12);

	indep = independent_bernstein_bound(&s, &out, NULL);
	CHECK(indep < 1e-10);
	CHECK(sample_max_deviation(&s, &out) < 1e-10);

	/* The surviving knot values are still the original values. */
	CHECK(multiplicity(&out, 0.5) == 1);
	CHECK(multiplicity(&out, 3.25) == 1);
	CHECK(multiplicity(&out, -0.5) == 0);
	CHECK(multiplicity(&out, 5) == 0);
	ts_bspline_free(&s);
	ts_bspline_free(&work);
	ts_bspline_free(&out);
}

static void
test_c0_interior_knot(void)
{
	tsStatus st;
	tsBSpline base, work = ts_bspline_init(), s;
	tsBSpline tight = ts_bspline_init(), loose = ts_bspline_init();
	size_t k, removed;
	tsReal bound;
	tsReal ctrlp[8] = {0, 0, 1, 2, 3, 1, 4, 0};
	tsReal knots[8] = {0, 0, 0, 0, 1, 1, 1, 1};
	tsReal indep;

	/* A cubic with .5 inserted three times has multiplicity 3 at .5,
	 * i.e. a C0 join below the full order-4 fracture barrier. */
	base = make_spline(4, 2, 3, ctrlp, knots);
	ts_bspline_insert_knot(&base, 0.5, 3, &work, &k, &st);
	s = work;
	CHECK(multiplicity(&s, 0.5) == 3);
	/* Small interior perturbation; the shared join control point is
	 * left untouched, so the curve stays continuous at .5. */
	{
		tsReal *cp;
		ts_bspline_control_points(&s, &cp, &st);
		cp[2 * 2 + 1] += 2e-4;
		ts_bspline_set_control_points(&s, cp, &st);
		free(cp);
	}

	/* Tight budget: the join-changing removal is rejected. */
	CHECK(ts_bspline_remove_knot(&s, 0.5, 1, 1e-9, &tight,
	                             &removed, &bound, &st) == TS_SUCCESS);
	CHECK(removed == 0);
	CHECK(ts_bspline_num_control_points(&tight) == 7);
	CHECK(bound < 1e-10);
	indep = independent_bernstein_bound(&s, &tight, NULL);
	CHECK(sample_max_deviation(&s, &tight) <= indep);
	ts_bspline_free(&tight);

	/* Generous budget accepts one copy, certified against the source. */
	CHECK(ts_bspline_remove_knot(&s, 0.5, 1, 1e-2, &loose,
	                             &removed, &bound, &st) == TS_SUCCESS);
	CHECK(removed == 1);
	CHECK(ts_bspline_num_control_points(&loose) == 6);
	CHECK(bound <= 1e-2);
	indep = independent_bernstein_bound(&s, &loose, NULL);
	CHECK(sample_max_deviation(&s, &loose) <= bound);
	CHECK(sample_max_deviation(&s, &loose) <= indep);
	ts_bspline_free(&base);
	ts_bspline_free(&s);
	ts_bspline_free(&loose);
}

static void
test_true_fracture(void)
{
	tsStatus st;
	tsBSpline s, out = ts_bspline_init();
	size_t removed;
	tsReal bound;
	/* Multiplicity 4 == order: a genuine break with a spatial gap. */
	tsReal ctrlp[16] = {0, 0, 1, 1, 2, 0, 2.5, 0,
	                    3, 30, 3.5, 20, 4, 1, 5, 0};
	tsReal knots[12] = {0, 0, 0, 0, 0.5, 0.5, 0.5, 0.5,
	                    1, 1, 1, 1};

	s = make_spline(8, 2, 3, ctrlp, knots);
	CHECK(ts_bspline_simplify(&s, 1e-2, &out, &removed,
	                          &bound, &st) == TS_SUCCESS);
	CHECK(removed == 0);
	CHECK(ts_bspline_num_control_points(&out) == 8);
	CHECK(multiplicity(&out, 0.5) == 4);

	/* One-sided evaluation at the fracture yields two distinct limits,
	 * equal to the two adjacent control points. */
	{
		tsDeBoorNet net = ts_deboornet_init();
		tsReal *r = NULL;
		ts_bspline_eval(&s, 0.5, &net, &st);
		CHECK(ts_deboornet_num_result(&net) == 2);
		ts_deboornet_result(&net, &r, &st);
		CHECK(fabs(r[0] - 2.5) < 1e-12 && fabs(r[1] - 0) < 1e-12);
		CHECK(fabs(r[2] - 3) < 1e-12 && fabs(r[3] - 30) < 1e-12);
		free(r);
		ts_deboornet_free(&net);
	}
	ts_bspline_free(&s);
	ts_bspline_free(&out);
}

static void
test_spatially_close_pieces(void)
{
	tsStatus st;
	tsBSpline base, work = ts_bspline_init(), s;
	tsBSpline tight = ts_bspline_init(), loose = ts_bspline_init();
	size_t k, removed;
	tsReal bound;
	tsReal ctrlp[8] = {0, 0, 1, 2, 3, 1, 4, 0};
	tsReal knots[8] = {0, 0, 0, 0, 1, 1, 1, 1};

	/* Two parametric pieces separated at .5, built as an exact knot
	 * insertion followed by a small interior perturbation. The two
	 * sides stay spatially close; the question is nevertheless decided
	 * by the parametric error budget, not by spatial coincidence. */
	base = make_spline(4, 2, 3, ctrlp, knots);
	ts_bspline_insert_knot(&base, 0.5, 1, &work, &k, &st);
	s = work;
	{
		tsReal *cp;
		ts_bspline_control_points(&s, &cp, &st);
		cp[2 * 2 + 1] += 2e-4;
		ts_bspline_set_control_points(&s, cp, &st);
		free(cp);
	}
	CHECK(multiplicity(&s, 0.5) == 1);

	CHECK(ts_bspline_remove_knot(&s, 0.5, 1, 1e-8, &tight,
	                             &removed, &bound, &st) == TS_SUCCESS);
	CHECK(removed == 0);
	CHECK(ts_bspline_num_control_points(&tight) == 5);
	ts_bspline_free(&tight);

	CHECK(ts_bspline_remove_knot(&s, 0.5, 1, 1e-2, &loose,
	                             &removed, &bound, &st) == TS_SUCCESS);
	CHECK(removed == 1);
	CHECK(ts_bspline_num_control_points(&loose) == 4);
	CHECK(bound <= 1e-2);
	{
		tsReal indep = independent_bernstein_bound(&s, &loose, NULL);
		CHECK(sample_max_deviation(&s, &loose) <= indep);
		CHECK(sample_max_deviation(&s, &loose) <= bound);
	}

	/* One-sided evaluation at the join still works and is finite. */
	{
		tsReal ps[3], pl[3];
		eval_point(&s, 0.5, ps);
		eval_point(&loose, 0.5, pl);
		CHECK(isfinite(ps[0]) && isfinite(pl[0]));
	}
	ts_bspline_free(&base);
	ts_bspline_free(&s);
	ts_bspline_free(&loose);
}

static void
test_mandatory_degree1_approximation(void)
{
	tsStatus st;
	tsBSpline s, out = ts_bspline_init();
	size_t removed;
	tsReal bound;
	tsReal ctrlp[6] = {0, 0, 0.5, 1e-5, 1, 0};
	tsReal knots[5] = {0, 0, 0.5, 1, 1};
	tsReal indep;

	s = make_spline(3, 2, 1, ctrlp, knots);

	CHECK(ts_bspline_remove_knot(&s, 0.5, 1, 2e-5, &out,
	                             &removed, &bound, &st) == TS_SUCCESS);
	CHECK(removed == 1);
	CHECK(ts_bspline_num_control_points(&out) == 2);
	CHECK(multiplicity(&out, 0.5) == 0);
	CHECK(bound <= 2e-5);
	indep = independent_bernstein_bound(&s, &out, NULL);
	CHECK(indep <= 1e-5 + 1e-12);
	CHECK(fabs(bound - 1e-5) < 1e-12); /* exact analytic deviation */
	CHECK(sample_max_deviation(&s, &out) <= indep);
	CHECK(sample_max_deviation(&s, &out) <= bound);
	ts_bspline_free(&out);
	out = ts_bspline_init();

	CHECK(ts_bspline_remove_knot(&s, 0.5, 1, 1e-6, &out,
	                             &removed, &bound, &st) == TS_SUCCESS);
	CHECK(removed == 0);
	CHECK(ts_bspline_num_control_points(&out) == 3);
	CHECK(bound < 1e-12);
	ts_bspline_free(&s);
	ts_bspline_free(&out);
}

static void
test_cumulative_budget(void)
{
	tsStatus st;
	tsBSpline base, s, work = ts_bspline_init();
	tsBSpline out = ts_bspline_init(), full = ts_bspline_init();
	size_t k, removed, i;
	tsReal bound, full_bound;
	tsReal ctrlp[8] = {0, 0, 1, 2, 3, 1, 4, 0};
	tsReal knots[8] = {0, 0, 0, 0, 1, 1, 1, 1};
	const tsReal us[4] = {0.2, 0.4, 0.6, 0.8};
	const tsReal tol = 5e-5;
	const tsReal h = 3e-5;

	base = make_spline(4, 2, 3, ctrlp, knots);
	s = ts_bspline_init();
	ts_bspline_copy(&base, &s, &st);
	for (i = 0; i < 4; i++) {
		ts_bspline_insert_knot(&s, us[i], 1, &work, &k, &st);
		ts_bspline_free(&s);
		s = work;
		work = ts_bspline_init();
	}
	/* Perturb alternating interior control points. */
	{
		tsReal *cp;
		size_t n = ts_bspline_num_control_points(&s);
		ts_bspline_control_points(&s, &cp, &st);
		for (i = 2; i < n - 1; i += 2)
			cp[i * 2 + 1] += h;
		ts_bspline_set_control_points(&s, cp, &st);
		free(cp);
	}

	/* Every single removal, judged against the original source on its
	 * own, is affordable. */
	for (i = 0; i < 4; i++) {
		tsBSpline single = ts_bspline_init();
		tsReal eb;
		size_t rem;
		CHECK(ts_bspline_remove_knot(&s, us[i], 1, tol, &single,
		                             &rem, &eb, &st) == TS_SUCCESS);
		CHECK(rem == 1);
		CHECK(eb <= tol);
		ts_bspline_free(&single);
	}

	/* Removing everything accumulates more than the budget. */
	CHECK(ts_bspline_simplify(&s, 1e-2, &full, &removed,
	                          &full_bound, &st) == TS_SUCCESS);
	CHECK(removed == 4);
	CHECK(full_bound > tol);
	CHECK(sample_max_deviation(&s, &full) <= full_bound);

	/* Simplify under the real budget must stay within it and therefore
	 * keep at least one knot. Its certificate is independently checked. */
	CHECK(ts_bspline_simplify(&s, tol, &out, &removed,
	                          &bound, &st) == TS_SUCCESS);
	CHECK(bound <= tol);
	CHECK(removed < 4);
	{
		tsReal indep = independent_bernstein_bound(&s, &out, NULL);
		CHECK(sample_max_deviation(&s, &out) <= indep);
		CHECK(sample_max_deviation(&s, &out) <= bound);
	}
	ts_bspline_free(&base);
	ts_bspline_free(&s);
	ts_bspline_free(&work);
	ts_bspline_free(&out);
	ts_bspline_free(&full);
}

static void
test_aliasing_and_reuse(void)
{
	tsStatus st;
	tsBSpline s, work = ts_bspline_init();
	size_t k, removed;
	tsReal bound;
	tsReal ctrlp[12] = {0, 0, 0, 1, 2, 0, 3, 1, 0, 4, 0, 0};
	tsReal knots[8] = {0, 0, 0, 0, 1, 1, 1, 1};

	s = make_spline(4, 3, 3, ctrlp, knots);
	ts_bspline_insert_knot(&s, 0.25, 1, &work, &k, &st);
	ts_bspline_free(&s);
	s = work;
	work = ts_bspline_init();
	ts_bspline_insert_knot(&s, 0.75, 1, &work, &k, &st);
	ts_bspline_free(&s);
	s = work;
	work = ts_bspline_init();

	/* In-place successful removal. */
	CHECK(ts_bspline_remove_knot(&s, 0.25, 1, 1e-9, &s,
	                             &removed, &bound, &st) == TS_SUCCESS);
	CHECK(removed == 1);
	CHECK(bound < 1e-10);

	/* A failing call (unknown knot) leaves the spline untouched and
	 * evaluation still works. */
	{
		size_t n_before = ts_bspline_num_control_points(&s);
		tsDeBoorNet net = ts_deboornet_init();
		tsReal *r = NULL;
		CHECK(ts_bspline_remove_knot(&s, 0.99, 1, 1e-9, &s,
		                             &removed, &bound, &st)
		      == TS_U_UNDEFINED);
		CHECK(ts_bspline_num_control_points(&s) == n_before);
		CHECK(ts_bspline_eval(&s, 0.5, &net, &st) == TS_SUCCESS);
		CHECK(ts_deboornet_result(&net, &r, &st) == TS_SUCCESS);
		CHECK(isfinite(r[0]) && isfinite(r[1]));
		free(r);
		ts_deboornet_free(&net);
	}

	/* Another successful in-place removal after the failure. */
	CHECK(ts_bspline_remove_knot(&s, 0.75, 1, 1e-9, &s,
	                             &removed, &bound, &st) == TS_SUCCESS);
	CHECK(removed == 1);
	CHECK(ts_bspline_num_control_points(&s) == 4);
	CHECK(bound < 1e-10);

	/* Simplify in place on an already minimal curve is a no-op copy. */
	CHECK(ts_bspline_simplify(&s, 1e-9, &s, &removed,
	                          &bound, &st) == TS_SUCCESS);
	CHECK(removed == 0);
	CHECK(ts_bspline_num_control_points(&s) == 4);
	ts_bspline_free(&s);
	ts_bspline_free(&work);
}

static void
test_count_larger_than_multiplicity(void)
{
	tsStatus st;
	tsBSpline base, work = ts_bspline_init(), s;
	tsBSpline out = ts_bspline_init();
	size_t k, removed;
	tsReal bound;
	tsReal ctrlp[10] = {0, 0, 1, 2, 3, 1, 4, 0, 5, 1};
	tsReal knots[9] = {0, 0, 0, 0, 0.75, 1, 1, 1, 1};

	/* Insert .25 once and .75 twice, then ask for far more removals
	 * than copies exist. The call must stop when the knot is gone. */
	base = make_spline(5, 2, 3, ctrlp, knots);
	s = ts_bspline_init();
	ts_bspline_copy(&base, &s, &st);
	ts_bspline_insert_knot(&s, 0.25, 1, &work, &k, &st);
	ts_bspline_free(&s);
	s = work;
	work = ts_bspline_init();
	ts_bspline_insert_knot(&s, 0.75, 2, &work, &k, &st);
	ts_bspline_free(&s);
	s = work;
	work = ts_bspline_init();
	CHECK(ts_bspline_num_control_points(&s) == 8);

	CHECK(ts_bspline_remove_knot(&s, 0.25, 10, 1e-9, &out,
	                             &removed, &bound, &st) == TS_SUCCESS);
	CHECK(removed == 1);
	CHECK(multiplicity(&out, 0.25) == 0);
	CHECK(bound < 1e-10);
	ts_bspline_free(&out);
	out = ts_bspline_init();

	CHECK(ts_bspline_remove_knot(&s, 0.75, 10, 1e-9, &out,
	                             &removed, &bound, &st) == TS_SUCCESS);
	CHECK(removed == 2);
	CHECK(multiplicity(&out, 0.75) == 1);
	CHECK(bound < 1e-10);
	ts_bspline_free(&base);
	ts_bspline_free(&s);
	ts_bspline_free(&work);
	ts_bspline_free(&out);
}

static void
test_error_contracts(void)
{
	tsStatus st;
	tsBSpline s, out = ts_bspline_init();
	size_t removed = 999;
	tsReal bound = 7;
	tsReal ctrlp[8] = {0, 0, 1, 2, 3, 1, 4, 0};
	tsReal knots[8] = {0, 0, 0, 0, 1, 1, 1, 1};

	s = make_spline(4, 2, 3, ctrlp, knots);

	/* Invalid tolerance. */
	CHECK(ts_bspline_remove_knot(&s, 0.5, 1, 1e-12, &out,
	                             &removed, &bound, &st) == TS_NO_RESULT);
	CHECK(removed == 999 && bound == 7);
	CHECK(ts_bspline_simplify(&s, 0.5, &out, &removed,
	                          &bound, &st) == TS_NO_RESULT);

	/* End knots and missing knots. */
	CHECK(ts_bspline_remove_knot(&s, 0, 1, 1e-6, &out,
	                             &removed, &bound, &st)
	      == TS_U_UNDEFINED);
	CHECK(ts_bspline_remove_knot(&s, 1, 1, 1e-6, &out,
	                             &removed, &bound, &st)
	      == TS_U_UNDEFINED);
	CHECK(ts_bspline_remove_knot(&s, 0.7, 1, 1e-6, &out,
	                             &removed, &bound, &st)
	      == TS_U_UNDEFINED);

	/* NULL spline pointer. */
	CHECK(ts_bspline_simplify(NULL, 1e-6, &out, &removed,
	                          &bound, &st) == TS_INDEX_ERROR);

	/* Non-finite data. */
	{
		tsBSpline bad;
		tsReal cp[8] = {0, 0, 1, INFINITY, 3, 1, 4, 0};
		bad = make_spline(4, 2, 3, cp, knots);
		CHECK(ts_bspline_simplify(&bad, 1e-6, &out, &removed,
		                          &bound, &st) == TS_NO_RESULT);
		ts_bspline_free(&bad);
	}
	ts_bspline_free(&s);
	ts_bspline_free(&out);
}

int
main(void)
{
	test_analytic_line();
	test_cubic_exact_inverse();
	test_non_unit_domain();
	test_c0_interior_knot();
	test_true_fracture();
	test_spatially_close_pieces();
	test_mandatory_degree1_approximation();
	test_cumulative_budget();
	test_aliasing_and_reuse();
	test_count_larger_than_multiplicity();
	test_error_contracts();

	if (g_fails != 0) {
		fprintf(stderr, "KNOT_REMOVAL_FAILED (%d checks)\n", g_fails);
		return 1;
	}
	puts("KNOT_REMOVAL_OK");
	return 0;
}
