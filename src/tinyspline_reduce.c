/** @file tinyspline_reduce.c
 *
 * Knot removal and spline simplification for non-rational, clamped
 * B-Spline curves. See tinyspline_reduce.h for the documented semantics.
 *
 * The implementation uses only C99, the C standard library and libm.
 *
 * Candidate control polygons for a single knot removal are obtained
 * with the two directional inverse knot insertion formulas (the R and T
 * chains of the NURBS Book) and with their component-wise average. Each
 * candidate is accepted only if a certified, parameter-domain-wide error
 * bound against the original source curve fits the requested tolerance.
 */

#define TINYSPLINE_EXPORT
#include "tinyspline_reduce.h"

#include <math.h>   /* fabs, sqrt, isfinite */
#include <stdlib.h> /* malloc, free, realloc */
#include <string.h> /* memcpy */
#include <stdio.h>  /* sprintf (via TS_RETURN macros) */

/* Maximum supported degree (documented input range). */
#define TS_REDUCE_MAX_DEG 3

/* Maximum number of difference-curve subdivision levels used to
 * tighten the certified error bound. */
#define TS_REDUCE_MAX_SUBDIV 8

/* Input limits described in tinyspline_reduce.h. */
#define TS_REDUCE_MAX_COORD 1e4
#define TS_REDUCE_MAX_KNOT_ABS 100.0
#define TS_REDUCE_MIN_KNOT_GAP 1e-5

/*
 * A blending coefficient of the inverse-insertion formulas is treated as
 * exactly 0 (or 1) when it is below this threshold. The coefficient is a
 * ratio of knot differences, so a value this small means the
 * corresponding chain is not defined and must be skipped.
 */
#define TS_REDUCE_ALPHA_EPS 1e-12

typedef struct
{
	tsReal *knots;   /* distinct knot values, ascending */
	size_t count;    /* number of distinct values */
	size_t cap;      /* allocated capacity */
} tsReduceList;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static tsReal
ts_reduce_norm(const tsReal *v, size_t dim)
{
	tsReal sum = 0;
	size_t i;
	for (i = 0; i < dim; i++)
		sum += v[i] * v[i];
	return (tsReal) sqrt(sum);
}

static int
ts_reduce_finite_real(tsReal x)
{
	return isfinite(x) ? 1 : 0;
}

/* Distinct interior knot values of `spline` in ascending order. */
static tsError
ts_reduce_distinct_knots(const tsBSpline *spline,
                         tsReduceList *list,
                         tsStatus *status)
{
	const tsReal *knots = ts_bspline_knots_ptr(spline);
	const size_t n = ts_bspline_num_knots(spline);
	const size_t order = ts_bspline_order(spline);
	size_t i;
	list->count = 0;
	/* Interior indices are [order, n - order). */
	for (i = order; i < n - order; i++) {
		if (list->count > 0 &&
		    ts_knots_equal(list->knots[list->count - 1], knots[i]))
			continue;
		if (list->count == list->cap) {
			size_t ncap = list->cap == 0 ? 8 : list->cap * 2;
			tsReal *grown = (tsReal *) realloc(
				list->knots, ncap * sizeof(tsReal));
			if (!grown)
				TS_RETURN_0(status, TS_MALLOC, "out of memory")
			list->knots = grown;
			list->cap = ncap;
		}
		list->knots[list->count++] = knots[i];
	}
	TS_RETURN_SUCCESS(status)
}

static size_t
ts_reduce_multiplicity(const tsBSpline *spline, tsReal value)
{
	const tsReal *knots = ts_bspline_knots_ptr(spline);
	const size_t n = ts_bspline_num_knots(spline);
	size_t i, m = 0;
	for (i = 0; i < n; i++)
		if (ts_knots_equal(knots[i], value))
			m++;
	return m;
}

/* Builds a spline from an explicit knot vector/control point array. The
 * knot array must contain num_ctrlp + deg + 1 values. */
static tsError
ts_reduce_build(const tsReal *knots,
                const tsReal *ctrlp,
                size_t num_ctrlp,
                size_t dim,
                size_t deg,
                tsBSpline *out,
                tsStatus *status)
{
	tsError err;
	TS_TRY(try, err, status)
		TS_CALL(try, err, ts_bspline_new(
		        num_ctrlp, dim, deg, TS_OPENED, out, status))
		TS_CALL(try, err, ts_bspline_set_knots(out, knots, status))
		TS_CALL(try, err, ts_bspline_set_control_points(
		        out, ctrlp, status))
	TS_END_TRY_RETURN(err)
}

/* Refines `spline` so that every value in `target` has multiplicity
 * `order` (the degree plus one). End knots already have that multiplicity
 * in clamped input, so only interior values are inserted. */
static tsError
ts_reduce_refine_full(const tsBSpline *spline,
                      const tsReduceList *target,
                      tsBSpline *refined,
                      tsStatus *status)
{
	tsBSpline work = ts_bspline_init();
	tsBSpline tmp = ts_bspline_init();
	size_t i, m, need, k;
	tsError err;
	TS_TRY(try, err, status)
		TS_CALL(try, err, ts_bspline_copy(spline, &work, status))
		for (i = 0; i < target->count; i++) {
			m = ts_reduce_multiplicity(&work, target->knots[i]);
			need = ts_bspline_order(spline);
			if (m >= need)
				continue;
			TS_CALL(try, err, ts_bspline_insert_knot(
			        &work, target->knots[i], need - m,
			        &tmp, &k, status))
			ts_bspline_free(&work);
			ts_bspline_move(&tmp, &work);
		}
		ts_bspline_move(&work, refined);
	TS_FINALLY
		ts_bspline_free(&work);
		ts_bspline_free(&tmp);
	TS_END_TRY_RETURN(err)
}

/* Inflates a raw round-off-level bound so that accepted results stay
 * inside the requested budget even if the floating point certificate is
 * slightly optimistic. */
static tsReal
ts_reduce_inflate(tsReal raw, tsReal u_min, tsReal u_max)
{
	tsReal scale = fabs(u_min);
	if (fabs(u_max) > scale)
		scale = fabs(u_max);
	return raw * ((tsReal) 1.0 + TS_REDUCE_NUMERIC_MARGIN) +
	       TS_REDUCE_NUMERIC_FLOOR * ((tsReal) 1.0 + scale);
}

/* Maximum Euclidean norm of the common-Bezier difference control
 * vectors. Both splines must share the same fully refined knot vector,
 * so their control points enumerate the same Bezier segments. */
static tsReal
ts_reduce_bezier_diff_norm(const tsBSpline *a, const tsBSpline *b)
{
	const size_t dim = ts_bspline_dimension(a);
	const tsReal *pa = ts_bspline_control_points_ptr(a);
	const tsReal *pb = ts_bspline_control_points_ptr(b);
	const size_t nc = ts_bspline_num_control_points(a);
	tsReal diff[3];
	tsReal worst = 0;
	size_t i, d;
	for (i = 0; i < nc; i++) {
		for (d = 0; d < dim; d++)
			diff[d] = pb[i * dim + d] - pa[i * dim + d];
		{
			tsReal norm = ts_reduce_norm(diff, dim);
			if (norm > worst)
				worst = norm;
		}
	}
	return worst;
}

/* Certified upper bound of the distance between `source` and `cand`.
 * If `fits` is non-NULL it is set to 1 exactly when the (inflated)
 * bound does not exceed `tolerance`. A loose convex-hull bound is
 * tightened by subdividing the difference curve; fixed-density sampling
 * is never used. */
static tsError
ts_reduce_certified_bound(const tsBSpline *source,
                          const tsBSpline *cand,
                          tsReal tolerance,
                          int *fits,
                          tsReal *bound,
                          tsStatus *status)
{
	tsReduceList target = {NULL, 0, 0};
	tsBSpline rs = ts_bspline_init();
	tsBSpline rc = ts_bspline_init();
	tsReal u_min, u_max;
	tsReal best = 0;
	int have_best = 0;
	size_t level;
	tsError err;

	ts_bspline_domain(source, &u_min, &u_max);
	TS_TRY(try, err, status)
		TS_CALL(try, err, ts_reduce_distinct_knots(
		        source, &target, status))

		for (level = 0; level <= TS_REDUCE_MAX_SUBDIV; level++) {
			tsReal raw, inflated;
			size_t rnk, i;

			ts_bspline_free(&rs);
			ts_bspline_free(&rc);
			TS_CALL(try, err, ts_reduce_refine_full(
			        source, &target, &rs, status))
			TS_CALL(try, err, ts_reduce_refine_full(
			        cand, &target, &rc, status))

			rnk = ts_bspline_num_knots(&rs);
			if (ts_bspline_num_knots(&rc) != rnk)
				TS_THROW_0(try, err, status, TS_NO_RESULT,
				           "internal knot vector mismatch")
			raw = ts_reduce_bezier_diff_norm(&rs, &rc);
			inflated = ts_reduce_inflate(raw, u_min, u_max);
			if (!have_best || inflated < best) {
				best = inflated;
				have_best = 1;
			}
			if (best <= tolerance)
				break;
			if (level == TS_REDUCE_MAX_SUBDIV)
				break;

			/* Subdivide every nonzero interval at its midpoint.
			 * Midpoints inside TS_KNOT_EPSILON of an existing knot
			 * cannot be distinguished by the library's knot matching
			 * and must not be inserted; if none can be added the
			 * convex hull bound cannot be tightened further. */
			{
				tsReduceList finer;
				finer.cap = 2 * target.count + 1;
				finer.count = 0;
				finer.knots = (tsReal *) malloc(
					finer.cap * sizeof(tsReal));
				if (!finer.knots)
					TS_THROW_0(try, err, status, TS_MALLOC,
					           "out of memory")
				for (i = 0; i < target.count; i++) {
					size_t j;
					int usable;
					finer.knots[finer.count++] =
						target.knots[i];
					if (i + 1 >= target.count)
						continue;
					{
						tsReal mid = (tsReal) 0.5 *
							(target.knots[i] +
							 target.knots[i + 1]);
						usable = 1;
						for (j = 0; j < finer.count; j++)
							if (fabs(mid - finer.knots[j]) <
							    (tsReal) 2.0 * TS_KNOT_EPSILON)
								usable = 0;
						if (usable)
							finer.knots[finer.count++] = mid;
					}
				}
				if (finer.count == target.count) {
					free(finer.knots);
					break;
				}
				free(target.knots);
				target = finer;
			}
		}
	TS_FINALLY
		free(target.knots);
		ts_bspline_free(&rs);
		ts_bspline_free(&rc);
	TS_END_TRY
	if (err)
		return err;
	if (fits)
		*fits = have_best && best <= tolerance;
	*bound = best;
	TS_RETURN_SUCCESS(status)
}

/* ------------------------------------------------------------------ */
/* Input validation                                                    */
/* ------------------------------------------------------------------ */

static tsError
ts_reduce_validate(const tsBSpline *source,
                   tsReal tolerance,
                   tsBSpline *out,
                   size_t *removed,
                   tsReal *error_bound,
                   tsStatus *status)
{
	const size_t deg = ts_bspline_degree(source);
	const size_t dim = ts_bspline_dimension(source);
	const size_t order = ts_bspline_order(source);
	const size_t n_knots = ts_bspline_num_knots(source);
	const tsReal *knots = ts_bspline_knots_ptr(source);
	const tsReal *ctrlp = ts_bspline_control_points_ptr(source);
	const size_t len = ts_bspline_len_control_points(source);
	size_t i;

	if (!removed || !error_bound || !out)
		TS_RETURN_0(status, TS_NO_RESULT,
		            "output parameter must not be NULL")
	if (!(deg >= 1 && deg <= TS_REDUCE_MAX_DEG))
		TS_RETURN_1(status, TS_DEG_GE_NCTRLP,
		            "unsupported degree: %lu",
		            (unsigned long) deg)
	if (!(dim == 2 || dim == 3))
		TS_RETURN_1(status, TS_DIM_ZERO,
		            "unsupported dimension: %lu",
		            (unsigned long) dim)
	if (!(tolerance >= TS_REDUCE_TOLERANCE_MIN &&
	      tolerance <= TS_REDUCE_TOLERANCE_MAX))
		TS_RETURN_0(status, TS_U_UNDEFINED,
		            "tolerance outside [1e-9, 1e-2]")

	/* Clamped end knots with multiplicity degree + 1. */
	for (i = 0; i < order; i++)
		if (!ts_knots_equal(knots[i], knots[0]) ||
		    !ts_knots_equal(knots[n_knots - 1 - i],
		                    knots[n_knots - 1]))
			TS_RETURN_0(status, TS_MULTIPLICITY,
			            "spline is not clamped")

	for (i = 0; i < len; i++) {
		if (!ts_reduce_finite_real(ctrlp[i]))
			TS_RETURN_0(status, TS_NO_RESULT,
			            "non-finite control point")
		if (fabs(ctrlp[i]) > TS_REDUCE_MAX_COORD)
			TS_RETURN_0(status, TS_NO_RESULT,
			            "control point exceeds coordinate limit")
	}
	for (i = 0; i < n_knots; i++) {
		if (!ts_reduce_finite_real(knots[i]) ||
		    fabs(knots[i]) > TS_REDUCE_MAX_KNOT_ABS)
			TS_RETURN_0(status, TS_NO_RESULT,
			            "knot outside supported value range")
	}
	for (i = 1; i < n_knots; i++) {
		if (knots[i] < knots[i - 1])
			TS_RETURN_0(status, TS_KNOTS_DECR,
			            "decreasing knot vector")
		if (!ts_knots_equal(knots[i], knots[i - 1]) &&
		    knots[i] - knots[i - 1] < TS_REDUCE_MIN_KNOT_GAP)
			TS_RETURN_0(status, TS_NO_RESULT,
			            "distinct knots closer than 1e-5")
	}
	TS_RETURN_SUCCESS(status)
}

/* ------------------------------------------------------------------ */
/* Single knot removal candidate                                       */
/* ------------------------------------------------------------------ */

/*
 * Removes one copy of the knot whose last occurrence is at index `k` of
 * `work`. The NURBS Book inverse-insertion formulas produce two
 * directional reconstructions (R and T chains); together with their
 * component-wise average they form up to three candidates. Every
 * candidate keeps the control points outside the affected window, hence
 * both curve endpoints and the parameterization remain fixed.
 *
 * Sets *accepted to 1 and fills `cand` if a candidate's certified
 * deviation from `source` fits `tolerance`.
 */
static tsError
ts_reduce_try_one(const tsBSpline *work,
                  size_t k,
                  const tsBSpline *source,
                  tsReal tolerance,
                  tsBSpline *cand,
                  tsReal *bound_out,
                  int *accepted,
                  tsStatus *status)
{
	const size_t p = ts_bspline_degree(work);
	const size_t dim = ts_bspline_dimension(work);
	const size_t order = p + 1;
	const size_t n_old = ts_bspline_num_control_points(work);
	const size_t nk_old = ts_bspline_num_knots(work);
	const tsReal *U = ts_bspline_knots_ptr(work);
	const tsReal *P = ts_bspline_control_points_ptr(work);
	const tsReal u = U[k];

	size_t m, first, last, i, d, c;
	size_t lo, hi;
	tsReal *Uprime = NULL;
	tsReal *Q = NULL;
	tsReal *R = NULL;
	tsReal *T = NULL;
	tsReal alpha[TS_REDUCE_MAX_DEG + 1];
	tsReal bound = 0;
	tsError err;

	*accepted = 0;

	/* Current multiplicity of u (k points at its last occurrence). */
	lo = hi = k;
	while (lo > 0 && ts_knots_equal(U[lo - 1], u))
		lo--;
	while (hi + 1 < nk_old && ts_knots_equal(U[hi + 1], u))
		hi++;
	m = hi - lo + 1;
	k = hi;
	if (m >= order)
		TS_RETURN_SUCCESS(status) /* discontinuity barrier */

	first = k - p;
	last = k - m;

	Uprime = (tsReal *) malloc((nk_old - 1) * sizeof(tsReal));
	Q = (tsReal *) malloc((n_old - 1) * dim * sizeof(tsReal));
	R = (tsReal *) malloc((last - first + 1) * dim * sizeof(tsReal));
	T = (tsReal *) malloc((last - first + 1) * dim * sizeof(tsReal));
	if (!Uprime || !Q || !R || !T) {
		free(Uprime);
		free(Q);
		free(R);
		free(T);
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	}

	/* Knot vector after deleting one occurrence of u. */
	memcpy(Uprime, U, k * sizeof(tsReal));
	memcpy(Uprime + k, U + k + 1,
	       (nk_old - 1 - k) * sizeof(tsReal));

	/* alpha_i on the target knot vector U'. */
	for (i = 0; i <= TS_REDUCE_MAX_DEG; i++)
		alpha[i] = 0;
	for (i = first + 1; i <= last; i++) {
		tsReal den = Uprime[i + p] - Uprime[i];
		alpha[i - first] = den > 0
			? (u - Uprime[i]) / den
			: (tsReal) 1.0;
	}

	/* R chain (left to right) and T chain (right to left). */
	for (d = 0; d < dim; d++)
		R[d] = P[first * dim + d];
	for (i = first + 1; i <= last; i++) {
		tsReal a = alpha[i - first];
		if (a <= TS_REDUCE_ALPHA_EPS) {
			/* Chain is undefined; mark it non-finite below. */
			for (d = 0; d < dim; d++)
				R[(i - first) * dim + d] =
					(tsReal) NAN;
		} else {
			for (d = 0; d < dim; d++) {
				tsReal v = (P[i * dim + d] -
				            ((tsReal) 1.0 - a) *
				            R[(i - 1 - first) * dim + d]) / a;
				R[(i - first) * dim + d] = v;
			}
		}
	}
	for (d = 0; d < dim; d++)
		T[(last - first) * dim + d] = P[(last + 1) * dim + d];
	for (i = last; i > first; i--) {
		tsReal a = alpha[i - first]; /* alpha_i */
		if (a >= (tsReal) 1.0 - TS_REDUCE_ALPHA_EPS) {
			for (d = 0; d < dim; d++)
				T[(i - 1 - first) * dim + d] =
					(tsReal) NAN;
		} else {
			for (d = 0; d < dim; d++) {
				tsReal v =
					(P[i * dim + d] -
				         a * T[(i - first) * dim + d]) /
					((tsReal) 1.0 - a);
				T[(i - 1 - first) * dim + d] = v;
			}
		}
	}

	/* Try R polygon, T polygon, average. */
	for (c = 0; c < 3 && !*accepted; c++) {
		int finite_cand = 1;
		int fits = 0;
		size_t j;

		/* Unchanged sides. */
		for (i = 0; i < first; i++)
			for (d = 0; d < dim; d++)
				Q[i * dim + d] = P[i * dim + d];
		for (i = last + 1; i < n_old - 1; i++)
			for (d = 0; d < dim; d++)
				Q[i * dim + d] = P[(i + 1) * dim + d];
		/* Middle window. */
		for (i = first; i <= last; i++) {
			for (d = 0; d < dim; d++) {
				tsReal rv = R[(i - first) * dim + d];
				tsReal tv = T[(i - first) * dim + d];
				if (c == 0)
					Q[i * dim + d] = rv;
				else if (c == 1)
					Q[i * dim + d] = tv;
				else
					Q[i * dim + d] =
						(tsReal) 0.5 * (rv + tv);
			}
		}
		for (j = 0; j < (n_old - 1) * dim; j++) {
			if (!ts_reduce_finite_real(Q[j]) ||
			    fabs(Q[j]) > TS_REDUCE_MAX_COORD)
				finite_cand = 0;
		}
		if (!finite_cand)
			continue;

		ts_bspline_free(cand);
		err = ts_reduce_build(Uprime, Q,
		                      n_old - 1, dim, p, cand, status);
		if (err) {
			free(Uprime);
			free(Q);
			free(R);
			free(T);
			return err;
		}
		err = ts_reduce_certified_bound(
			source, cand, tolerance, &fits, &bound, status);
		if (err) {
			free(Uprime);
			free(Q);
			free(R);
			free(T);
			return err;
		}
		if (fits) {
			*accepted = 1;
			*bound_out = bound;
		}
	}

	free(Uprime);
	free(Q);
	free(R);
	free(T);
	TS_RETURN_SUCCESS(status)
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

tsError
ts_bspline_remove_knot(const tsBSpline *source,
                       tsReal knot,
                       size_t count,
                       tsReal tolerance,
                       tsBSpline *out,
                       size_t *removed,
                       tsReal *error_bound,
                       tsStatus *status)
{
	tsError err;
	tsBSpline work = ts_bspline_init();
	tsBSpline cand = ts_bspline_init();
	const tsReal *knots;
	size_t nk, i;
	size_t match_values = 0;
	tsReal matched = 0;
	tsReal min, max;
	size_t done = 0;
	tsReal bound = 0;

	TS_TRY(try, err, status)
		TS_CALL(try, err, ts_reduce_validate(
		        source, tolerance, out, removed,
		        error_bound, status))

		ts_bspline_domain(source, &min, &max);
		if (knot < min && !ts_knots_equal(knot, min))
			TS_THROW_0(try, err, status, TS_U_UNDEFINED,
			           "knot below parameter domain")
		if (knot > max && !ts_knots_equal(knot, max))
			TS_THROW_0(try, err, status, TS_U_UNDEFINED,
			           "knot above parameter domain")

		knots = ts_bspline_knots_ptr(source);
		nk = ts_bspline_num_knots(source);
		for (i = 0; i < nk; i++) {
			if (ts_knots_equal(knot, knots[i])) {
				if (match_values == 0 ||
				    !ts_knots_equal(matched, knots[i])) {
					matched = knots[i];
					match_values++;
				}
			}
		}
		if (match_values == 0)
			TS_THROW_0(try, err, status, TS_INDEX_ERROR,
			           "knot does not match any stored knot")
		if (match_values > 1)
			TS_THROW_0(try, err, status, TS_INDEX_ERROR,
			           "knot matches several distinct stored knots")
		if (ts_knots_equal(matched, min) ||
		    ts_knots_equal(matched, max))
			TS_THROW_0(try, err, status, TS_U_UNDEFINED,
			           "end knots cannot be removed")

		TS_CALL(try, err, ts_bspline_copy(source, &work, status))

		if (count > 0) {
			size_t m0 = ts_reduce_multiplicity(&work, matched);
			size_t limit = count < m0 ? count : m0;
			if (m0 >= ts_bspline_order(source))
				limit = 0; /* discontinuity barrier */
			for (i = 0; i < limit; i++) {
				const tsReal *wk =
					ts_bspline_knots_ptr(&work);
				size_t k2, wnk =
					ts_bspline_num_knots(&work);
				int accepted = 0;
				tsReal step_bound = 0;
				for (k2 = 0; k2 < wnk; k2++)
					if (ts_knots_equal(wk[k2], matched))
						break;
				while (k2 + 1 < wnk &&
				       ts_knots_equal(wk[k2 + 1], matched))
					k2++;
				TS_CALL(try, err, ts_reduce_try_one(
				        &work, k2, source, tolerance,
				        &cand, &step_bound, &accepted, status))
				if (!accepted)
					break;
				ts_bspline_free(&work);
				ts_bspline_move(&cand, &work);
				done++;
			}
			if (done > 0) {
				int fits = 0;
				TS_CALL(try, err, ts_reduce_certified_bound(
				        source, &work, tolerance,
				        &fits, &bound, status))
				if (!fits)
					TS_THROW_0(try, err, status, TS_NO_RESULT,
					           "internal error budget violation")
			}
		}
	TS_CATCH(err)
		ts_bspline_free(&work);
		ts_bspline_free(&cand);
		return err;
	TS_FINALLY
	TS_END_TRY

	/* Commit only after every step succeeded. */
	ts_bspline_free(out);
	ts_bspline_move(&work, out);
	ts_bspline_free(&cand);
	*removed = done;
	*error_bound = bound;
	TS_RETURN_SUCCESS(status)
}

tsError
ts_bspline_simplify(const tsBSpline *source,
                    tsReal tolerance,
                    tsBSpline *out,
                    size_t *removed,
                    tsReal *error_bound,
                    tsStatus *status)
{
	tsError err;
	tsBSpline work = ts_bspline_init();
	tsBSpline cand = ts_bspline_init();
	tsReduceList values = {NULL, 0, 0};
	size_t total = 0;
	tsReal bound = 0;
	int keep_going = 1;

	TS_TRY(try, err, status)
		TS_CALL(try, err, ts_reduce_validate(
		        source, tolerance, out, removed,
		        error_bound, status))
		TS_CALL(try, err, ts_bspline_copy(source, &work, status))

		while (keep_going) {
			size_t vi;
			int accepted_in_pass = 0;
			ts_bspline_free(&cand);
			free(values.knots);
			values.knots = NULL;
			values.count = values.cap = 0;
			TS_CALL(try, err, ts_reduce_distinct_knots(
			        &work, &values, status))
			for (vi = 0; vi < values.count; vi++) {
				const tsReal *wk;
				size_t k2, wnk, m;
				int accepted = 0;
				tsReal step_bound = 0;
				m = ts_reduce_multiplicity(
					&work, values.knots[vi]);
				if (m >= ts_bspline_order(&work))
					continue; /* discontinuity barrier */
				wk = ts_bspline_knots_ptr(&work);
				wnk = ts_bspline_num_knots(&work);
				for (k2 = 0; k2 < wnk; k2++)
					if (ts_knots_equal(
					        wk[k2], values.knots[vi]))
						break;
				while (k2 + 1 < wnk &&
				       ts_knots_equal(wk[k2 + 1],
				                      values.knots[vi]))
					k2++;
				TS_CALL(try, err, ts_reduce_try_one(
				        &work, k2, source, tolerance,
				        &cand, &step_bound, &accepted, status))
				if (accepted) {
					ts_bspline_free(&work);
					ts_bspline_move(&cand, &work);
					total++;
					accepted_in_pass = 1;
					break; /* restart from the smallest knot */
				}
			}
			if (!accepted_in_pass)
				keep_going = 0;
		}

		if (total > 0) {
			int fits = 0;
			TS_CALL(try, err, ts_reduce_certified_bound(
			        source, &work, tolerance,
			        &fits, &bound, status))
			if (!fits)
				TS_THROW_0(try, err, status, TS_NO_RESULT,
				           "internal error budget violation")
		}
	TS_CATCH(err)
		free(values.knots);
		ts_bspline_free(&work);
		ts_bspline_free(&cand);
		return err;
	TS_FINALLY
		free(values.knots);
	TS_END_TRY

	ts_bspline_free(out);
	ts_bspline_move(&work, out);
	ts_bspline_free(&cand);
	*removed = total;
	*error_bound = bound;
	TS_RETURN_SUCCESS(status)
}
