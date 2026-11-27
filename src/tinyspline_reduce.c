#define TINYSPLINE_EXPORT
#include "tinyspline_reduce.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Internal constants                                                 */
/* ------------------------------------------------------------------ */

#define TS_REDUCE_MIN_TOL       ((tsReal) 1e-9)
#define TS_REDUCE_MAX_TOL       ((tsReal) 1e-2)
#define TS_REDUCE_MAX_COORD     ((tsReal) 1e4)
#define TS_REDUCE_MAX_KNOT_ABS  ((tsReal) 100.0)
#define TS_REDUCE_MIN_KNOT_GAP  ((tsReal) 1e-5)
/* Maximum depth of the certificate tightening subdivision. */
#define TS_REDUCE_MAX_SUBDIV    12
/* Roundoff slack, see header documentation. */
#define TS_REDUCE_SLACK_FACTOR  64.0

#ifdef TINYSPLINE_FLOAT_PRECISION
#define TS_REDUCE_EPSILON       FLT_EPSILON
#else
#define TS_REDUCE_EPSILON       DBL_EPSILON
#endif

/* ------------------------------------------------------------------ */
/* Small utilities                                                    */
/* ------------------------------------------------------------------ */

/* Exact equality of two stored values, written without == to stay
 * warning-clean under -Wfloat-equal. Multiplicity runs and clamped end
 * knots compare physically identical doubles. */
static int
ts_reduce_same(tsReal x, tsReal y)
{
	return x < y || x > y ? 0 : 1;
}

static int
ts_reduce_finite(tsReal x)
{
#if defined(_MSC_VER)
	return _finite(x) != 0;
#else
	return isfinite(x) != 0;
#endif
}

static tsReal
ts_reduce_norm(const tsReal *x, size_t dim)
{
	tsReal sum = 0;
	size_t i;
	for (i = 0; i < dim; i++)
		sum += x[i] * x[i];
	return (tsReal) sqrt(sum);
}

/* De Casteljau subdivision of a degree p Bezier polygon defined on
 * [a, b] at parameter t. left and right must each hold (p+1)*dim
 * values. work must hold (p+1)*dim values as well. */
static void
ts_reduce_subdivide(tsReal a,
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
		for (i = 0; i + r <= p; i++) {
			for (d = 0; d < dim; d++) {
				work[i * dim + d] =
					alpha * work[i * dim + d] +
					beta  * work[(i + 1) * dim + d];
			}
		}
		memcpy(left + r * dim, work, dim * sizeof(tsReal));
		memcpy(right + (p - r) * dim,
		       work + (p - r) * dim,
		       dim * sizeof(tsReal));
	}
}

/* Maximum norm of the coefficient-wise difference of two Bezier
 * polygons. Also tracks the largest coefficient magnitude seen. */
static tsReal
ts_reduce_polygon_diff(const tsReal *P,
                       const tsReal *Q,
                       size_t p,
                       size_t dim,
                       tsReal *max_mag)
{
	tsReal bound = 0;
	size_t i, d;
	for (i = 0; i <= p; i++) {
		tsReal diff[3];
		tsReal v;
		for (d = 0; d < dim; d++) {
			tsReal x = P[i * dim + d];
			tsReal y = Q[i * dim + d];
			tsReal m;
			diff[d] = x - y;
			m = fabs(x);
			if (fabs(y) > m) m = fabs(y);
			if (m > *max_mag) *max_mag = m;
		}
		v = ts_reduce_norm(diff, dim);
		if (v > bound) bound = v;
	}
	return bound;
}

/* Certified bound for a pair of Bezier polygons on [a, b], with
 * adaptive tightening of the convex hull bound. */
static tsReal
ts_reduce_pair_bound(tsReal a,
                     tsReal b,
                     const tsReal *P,
                     const tsReal *Q,
                     size_t p,
                     size_t dim,
                     unsigned depth,
                     tsReal *max_mag)
{
	tsReal direct = ts_reduce_polygon_diff(P, Q, p, dim, max_mag);
	tsReal refined;
	tsReal mid, lp[16], lq[16], rp[16], rq[16], wp[16], wq[16];

	if (depth == 0)
		return direct;

	mid = (a + b) * (tsReal) 0.5;
	ts_reduce_subdivide(a, b, mid, P, p, dim, wp, lp, rp);
	ts_reduce_subdivide(a, b, mid, Q, p, dim, wq, lq, rq);
	refined = ts_reduce_pair_bound(a, mid, lp, lq, p, dim,
	                               depth - 1, max_mag);
	{
		tsReal rhs = ts_reduce_pair_bound(mid, b, rp, rq, p, dim,
		                                  depth - 1, max_mag);
		if (rhs > refined) refined = rhs;
	}
	return refined < direct ? refined : direct;
}

/* ------------------------------------------------------------------ */
/* Bezier extraction                                                  */
/* ------------------------------------------------------------------ */

typedef struct
{
	tsBSpline spline;
	tsReal *ctrlp;
	tsReal *knots;
	tsReal *breaks;
	size_t segments;
	size_t order;
	size_t dim;
} ts_reduce_beziers;

static tsError
ts_reduce_extract(const tsBSpline *spline,
                  ts_reduce_beziers *bz,
                  tsStatus *status)
{
	tsError err;
	size_t j;
	bz->spline = ts_bspline_init();
	TS_CALL_ROE(err, ts_bspline_to_beziers(spline, &bz->spline, status))
	bz->order = ts_bspline_order(&bz->spline);
	bz->dim = ts_bspline_dimension(&bz->spline);
	bz->segments = ts_bspline_num_control_points(&bz->spline) / bz->order;
	bz->ctrlp = (tsReal *) (void *)
		ts_bspline_control_points_ptr(&bz->spline);
	bz->knots = (tsReal *) (void *)
		ts_bspline_knots_ptr(&bz->spline);
	bz->breaks = (tsReal *) malloc((bz->segments + 1) * sizeof(tsReal));
	if (!bz->breaks) {
		ts_bspline_free(&bz->spline);
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	}
	for (j = 0; j <= bz->segments; j++)
		bz->breaks[j] = bz->knots[j * bz->order];
	return TS_SUCCESS;
}

static void
ts_reduce_extract_free(ts_reduce_beziers *bz)
{
	if (bz->breaks)
		free(bz->breaks);
	ts_bspline_free(&bz->spline);
}

/* ------------------------------------------------------------------ */
/* Certified error bound between two splines                          */
/* ------------------------------------------------------------------ */

static tsError
ts_reduce_certified_bound(const tsBSpline *orig,
                          const tsBSpline *cand,
                          tsReal *bound,
                          tsStatus *status)
{
	ts_reduce_beziers bo = {0};
	ts_reduce_beziers bc = {0};
	tsError err;
	tsReal max_mag = 0;
	tsReal worst = 0;
	tsReal slack;
	size_t p, dim;
	size_t j, k, piece;
	tsReal *refined = NULL;     /* candidate polygons on orig partition */
	tsReal *work = NULL, *left = NULL, *right = NULL;
	size_t poly_size;

	bo.breaks = bc.breaks = NULL;
	TS_CALL_ROE(err, ts_reduce_extract(orig, &bo, status))
	TS_TRY(try, err, status)
		TS_CALL(try, err, ts_reduce_extract(cand, &bc, status))

		if (bo.order != bc.order || bo.dim != bc.dim) {
			TS_THROW_0(try, err, status, TS_NO_RESULT,
			           "internal error: incompatible bezier decompositions")
		}
		p = bo.order - 1;
		dim = bo.dim;
		poly_size = bo.order * dim;

		refined = (tsReal *) malloc(bo.segments * poly_size *
		                            sizeof(tsReal));
		work = (tsReal *) malloc(poly_size * sizeof(tsReal));
		left = (tsReal *) malloc(poly_size * sizeof(tsReal));
		right = (tsReal *) malloc(poly_size * sizeof(tsReal));
		if (!refined || !work || !left || !right) {
			TS_THROW_0(try, err, status, TS_MALLOC, "out of memory")
		}

		/* Refine the candidate Bezier decomposition until its
		 * partition coincides with the one of `orig`. Candidate
		 * knots are a subset of the original knots, so the split
		 * parameters are exact shared break values. */
		piece = 0;
		for (k = 0; k < bc.segments; k++) {
			const tsReal ca = bc.breaks[k];
			const tsReal cb = bc.breaks[k + 1];
			tsReal cur_a = ca;
			memcpy(work,
			       bc.ctrlp + k * poly_size,
			       poly_size * sizeof(tsReal));
			for (j = 0; j < bo.segments; j++) {
				if (bo.breaks[j] < ca ||
				    !(bo.breaks[j] > cur_a &&
				      bo.breaks[j] < cb))
					continue;
				/* Split the current remainder at the
				 * original break bo.breaks[j]. */
				ts_reduce_subdivide(
					cur_a, cb, bo.breaks[j],
					work, p, dim,
					left,
					refined + piece * poly_size,
					right);
				piece++;
				cur_a = bo.breaks[j];
				memcpy(work, right,
				       poly_size * sizeof(tsReal));
			}
			/* Final remainder covers [cur_a, cb]. */
			memcpy(refined + piece * poly_size,
			       work, poly_size * sizeof(tsReal));
			piece++;
		}
		if (piece != bo.segments) {
			TS_THROW_0(try, err, status, TS_NO_RESULT,
			           "internal error: bezier partitions do not nest")
		}

		for (j = 0; j < bo.segments; j++) {
			tsReal v;
			/* Fracture knots yield adjacent equal break values and
			 * therefore zero-width intervals; the one-sided limits
			 * are covered by the non-zero intervals on either side. */
			if (ts_reduce_same(bo.breaks[j], bo.breaks[j + 1]))
				continue;
			v = ts_reduce_pair_bound(
				bo.breaks[j], bo.breaks[j + 1],
				bo.ctrlp + j * poly_size,
				refined + j * poly_size,
				p, dim,
				TS_REDUCE_MAX_SUBDIV, &max_mag);
			if (v > worst) worst = v;
		}

		slack = TS_REDUCE_SLACK_FACTOR * TS_REDUCE_EPSILON *
		        ((tsReal) 1.0 + max_mag);
		*bound = worst + slack;
	TS_FINALLY
		free(refined);
		free(work);
		free(left);
		free(right);
		ts_reduce_extract_free(&bo);
		ts_reduce_extract_free(&bc);
	TS_END_TRY_RETURN(err)
}

/* ------------------------------------------------------------------ */
/* Input validation                                                   */
/* ------------------------------------------------------------------ */

static tsError
ts_reduce_validate(const tsBSpline *source,
                   tsReal tolerance,
                   tsBSpline *out,
                   tsStatus *status)
{
	size_t deg, order, dim, n_ctrlp, n_knots, i;
	const tsReal *ctrlp, *knots;
	tsReal min, max;

	if (source == NULL || out == NULL) {
		TS_RETURN_0(status, TS_INDEX_ERROR, "null spline argument")
	}
	if (source->pImpl == NULL) {
		TS_RETURN_0(status, TS_INDEX_ERROR, "uninitialized spline")
	}
	if (!(tolerance >= TS_REDUCE_MIN_TOL &&
	      tolerance <= TS_REDUCE_MAX_TOL &&
	      ts_reduce_finite(tolerance))) {
		TS_RETURN_1(status, TS_NO_RESULT,
		            "tolerance (%g) outside [1e-9, 1e-2]",
		            (double) tolerance)
	}

	deg = ts_bspline_degree(source);
	dim = ts_bspline_dimension(source);
	n_ctrlp = ts_bspline_num_control_points(source);
	n_knots = ts_bspline_num_knots(source);
	ctrlp = ts_bspline_control_points_ptr(source);
	knots = ts_bspline_knots_ptr(source);
	order = deg + 1;

	if (deg < 1 || deg > 3) {
		TS_RETURN_1(status, TS_NO_RESULT,
		            "degree (%lu) outside supported range 1..3",
		            (unsigned long) deg)
	}
	if (dim != 2 && dim != 3) {
		TS_RETURN_1(status, TS_NO_RESULT,
		            "dimension (%lu) outside supported range 2..3",
		            (unsigned long) dim)
	}

	/* Clamped knot vector: both end knots have multiplicity order. */
	for (i = 0; i < order; i++) {
		if (!ts_reduce_same(knots[i], knots[0]) ||
		    !ts_reduce_same(knots[n_knots - 1 - i],
		                    knots[n_knots - 1])) {
			TS_RETURN_0(status, TS_NO_RESULT,
			            "spline is not clamped")
		}
	}
	min = knots[0];
	max = knots[n_knots - 1];
	if (!(max > min)) {
		TS_RETURN_0(status, TS_NO_RESULT, "empty parameter domain")
	}

	for (i = 0; i < n_ctrlp * dim; i++) {
		if (!ts_reduce_finite(ctrlp[i])) {
			TS_RETURN_0(status, TS_NO_RESULT,
			            "non-finite control point component")
		}
		if (fabs(ctrlp[i]) > TS_REDUCE_MAX_COORD) {
			TS_RETURN_1(status, TS_NO_RESULT,
			            "control point component (%g) exceeds 1e4",
			            (double) ctrlp[i])
		}
	}

	for (i = 0; i < n_knots; i++) {
		if (!ts_reduce_finite(knots[i])) {
			TS_RETURN_0(status, TS_NO_RESULT,
			            "non-finite knot")
		}
		if (fabs(knots[i]) > TS_REDUCE_MAX_KNOT_ABS) {
			TS_RETURN_1(status, TS_NO_RESULT,
			            "knot (%g) exceeds absolute value 100",
			            (double) knots[i])
		}
	}

	/* Distinct knot values must be sufficiently separated, and no
	 * multiplicity may exceed the order. */
	{
		size_t run = 1;
		for (i = 1; i < n_knots; i++) {
			if (ts_reduce_same(knots[i], knots[i - 1])) {
				run++;
				if (run > order) {
					TS_RETURN_0(status, TS_NO_RESULT,
					            "knot multiplicity exceeds order")
				}
			} else {
				if (knots[i] < knots[i - 1]) {
					TS_RETURN_0(status, TS_NO_RESULT,
					            "decreasing knot vector")
				}
				if (knots[i] - knots[i - 1] <
				    TS_REDUCE_MIN_KNOT_GAP) {
					TS_RETURN_0(status, TS_NO_RESULT,
					            "distinct knots spaced by less than 1e-5")
				}
				run = 1;
			}
		}
	}
	return TS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Knot matching                                                       */
/* ------------------------------------------------------------------ */

/* Locates the distinct knot value matching `knot`. Returns the index of
 * its last occurrence in `idx`, its multiplicity in `mult` and the
 * stored value in `value`. Distinct values falling into the same
 * TS_KNOT_EPSILON range are an ambiguous match. */
static tsError
ts_reduce_match_knot(const tsBSpline *spline,
                     tsReal knot,
                     tsReal *value,
                     size_t *idx,
                     size_t *mult,
                     tsStatus *status)
{
	const tsReal *knots = ts_bspline_knots_ptr(spline);
	const size_t n_knots = ts_bspline_num_knots(spline);
	size_t i;
	size_t matches = 0;
	tsReal matched = 0;
	tsReal previous = 0;

	/* Count the distinct stored knot values whose TS_KNOT_EPSILON
	 * neighborhood contains `knot`. Distinctness is exact (the input
	 * contract guarantees distinct values are spaced by >= 1e-5), while
	 * matching itself follows the TS_KNOT_EPSILON convention. */
	for (i = 0; i < n_knots; i++) {
		if (i > 0 && ts_reduce_same(knots[i], previous))
			continue;
		previous = knots[i];
		if (ts_knots_equal(knots[i], knot)) {
			matches++;
			matched = knots[i];
		}
	}
	if (matches == 0) {
		TS_RETURN_1(status, TS_U_UNDEFINED,
		            "knot (%g) not found", (double) knot)
	}
	if (matches > 1) {
		TS_RETURN_1(status, TS_NO_RESULT,
		            "knot (%g) matches several distinct knot values",
		            (double) knot)
	}

	/* Locate last occurrence and multiplicity of the stored value. */
	*idx = 0;
	*mult = 0;
	for (i = 0; i < n_knots; i++) {
		if (ts_reduce_same(knots[i], matched)) {
			*idx = i;
			(*mult)++;
		}
	}
	*value = matched;
	return TS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Single knot removal candidate construction                          */
/* ------------------------------------------------------------------ */

/* Builds a spline with one occurrence of the knot at last index `k`
 * (current multiplicity `mult`) removed. Returns TS_SUCCESS if a finite
 * candidate could be built, TS_NO_RESULT if the inverse equations are
 * not numerically solvable (a regular rejection, not an API failure).
 *
 * The two-sided inverse insertion recurrences below are derived directly
 * from the knot insertion identity for the last insertion that created
 * the removed knot copy. U is the knot vector before removal,
 * u = U[k], p the degree, Q = current control points, V = sought
 * control points:
 *
 *   first = k - p, last = k - mult, span = last - first
 *   a_i   = (u - U[i]) / (U[i+p+1] - U[i]),  b_i = 1 - a_i
 *   Q_i   = b_i V_{i-1} + a_i V_i
 *
 * The span interior points V[first] .. V[last-1] are recovered twice:
 * once from the left, anchored at the unchanged point
 * V[first-1] = Q[first-1], and once from the right, anchored at the
 * unchanged point V[last+1] = Q[last+1]. Both anchors exist because the
 * knot is internal (first >= 1) and last + 1 < num(control points). For
 * exactly invertible material both recursions reproduce the original
 * points; otherwise the left half is taken from the left recursion, the
 * right half from the right recursion, averaging the exact middle.
 * V[last] = Q[last+1] is an unchanged boundary point. A span of zero
 * (e.g. degree 1 with a simple knot) has no interior points. */
static tsError
ts_reduce_build_candidate(const tsBSpline *current,
                          size_t k,
                          size_t mult,
                          tsBSpline *candidate,
                          tsStatus *status)
{
	const size_t deg = ts_bspline_degree(current);
	const size_t dim = ts_bspline_dimension(current);
	const size_t n_ctrlp = ts_bspline_num_control_points(current);
	const size_t n_knots = ts_bspline_num_knots(current);
	const tsReal *P = ts_bspline_control_points_ptr(current);
	const tsReal *U = ts_bspline_knots_ptr(current);

	const size_t first = k - deg;
	const size_t last = k - mult;
	const size_t span = last - first;

	tsReal *left = NULL, *right = NULL, *new_ctrlp = NULL;
	tsReal *new_knots = NULL;
	tsError err;
	size_t j, d;

	*candidate = ts_bspline_init();

	left = (tsReal *) calloc(span ? span : 1, dim * sizeof(tsReal));
	right = (tsReal *) calloc(span ? span : 1, dim * sizeof(tsReal));
	if (!left || !right) {
		free(left);
		free(right);
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	}

	if (span > 0) {
		/* Left recursion: left[j] = V[first+j], anchored at
		 * V[first-1] = Q[first-1]. */
		tsReal prev[3];
		for (d = 0; d < dim; d++)
			prev[d] = P[(first - 1) * dim + d];
		for (j = 0; j < span; j++) {
			const size_t i = first + j;
			tsReal denom = U[i + deg + 1] - U[i];
			tsReal alpha = (denom > 0 || denom < 0)
			                   ? (U[k] - U[i]) / denom : 0;
			tsReal beta = (tsReal) 1.0 - alpha;
			for (d = 0; d < dim; d++) {
				if (ts_reduce_same(alpha, 0))
					left[j * dim + d] = prev[d];
				else
					left[j * dim + d] =
						(P[i * dim + d] - beta * prev[d]) / alpha;
				prev[d] = left[j * dim + d];
				if (!ts_reduce_finite(prev[d])) {
					free(left);
					free(right);
					TS_RETURN_0(status, TS_NO_RESULT,
					            "knot removal is numerically unstable")
				}
			}
		}

		/* Right recursion: right[j] = V[first+j], anchored at
		 * V[last+1] = Q[last+1]. */
		tsReal next[3];
		for (d = 0; d < dim; d++)
			next[d] = P[(last + 1) * dim + d];
		for (j = span; j >= 1; j--) {
			const size_t i = first + j;
			tsReal denom = U[i + deg + 1] - U[i];
			tsReal alpha = (denom > 0 || denom < 0)
			                   ? (U[k] - U[i]) / denom : 0;
			tsReal beta = (tsReal) 1.0 - alpha;
			for (d = 0; d < dim; d++) {
				if (ts_reduce_same(beta, 0))
					right[(j - 1) * dim + d] = next[d];
				else
					right[(j - 1) * dim + d] =
						(P[i * dim + d] - alpha * next[d]) / beta;
				next[d] = right[(j - 1) * dim + d];
				if (!ts_reduce_finite(next[d])) {
					free(left);
					free(right);
					TS_RETURN_0(status, TS_NO_RESULT,
					            "knot removal is numerically unstable")
				}
			}
		}
	}

	TS_TRY(try, err, status)
		TS_CALL(try, err, ts_bspline_new(
		        n_ctrlp - 1, dim, deg, TS_OPENED, candidate, status))

		new_knots = (tsReal *) malloc((n_knots - 1) * sizeof(tsReal));
		new_ctrlp = (tsReal *) malloc((n_ctrlp - 1) * dim *
		                              sizeof(tsReal));
		if (!new_knots || !new_ctrlp) {
			TS_THROW_0(try, err, status, TS_MALLOC, "out of memory")
		}

		/* Knot vector: drop the occurrence at index k. */
		memcpy(new_knots, U, k * sizeof(tsReal));
		memcpy(new_knots + k, U + k + 1,
		       (n_knots - 1 - k) * sizeof(tsReal));
		TS_CALL(try, err, ts_bspline_set_knots(
		        candidate, new_knots, status))

		/* Control points outside the affected region are kept. */
		memcpy(new_ctrlp, P, first * dim * sizeof(tsReal));
		for (j = 0; j < span; j++) {
			tsReal *dst = new_ctrlp + (first + j) * dim;
			if (span % 2 == 1 && j == span / 2) {
				/* Odd span: average the center slot. */
				for (d = 0; d < dim; d++)
					dst[d] = (tsReal) 0.5 *
					         (left[j * dim + d] +
					          right[j * dim + d]);
			} else if (2 * j < span) {
				memcpy(dst, left + j * dim,
				       dim * sizeof(tsReal));
			} else {
				memcpy(dst, right + j * dim,
				       dim * sizeof(tsReal));
			}
		}
		/* Boundary point V[last] = Q[last+1]. This also handles
		 * span == 0 (e.g. degree 1 with a simple knot). */
		memcpy(new_ctrlp + last * dim,
		       P + (last + 1) * dim,
		       dim * sizeof(tsReal));
		memcpy(new_ctrlp + (last + 1) * dim,
		       P + (last + 2) * dim,
		       (n_ctrlp - last - 2) * dim * sizeof(tsReal));
		TS_CALL(try, err, ts_bspline_set_control_points(
		        candidate, new_ctrlp, status))
	TS_CATCH(err)
		ts_bspline_free(candidate);
	TS_FINALLY
		free(left);
		free(right);
		free(new_knots);
		free(new_ctrlp);
	TS_END_TRY_RETURN(err)
}

/* Cheap, sound rejection filter: if the candidate deviates from the
 * original source curve by more than the tolerance at any evaluated
 * parameter, it cannot possibly satisfy the global budget. A rejection
 * here can never be wrong because the certificate is an upper bound of
 * the very same pointwise distances. It is only an optimization; the
 * certified check remains the authority. */
static int
ts_reduce_sample_filter(const tsBSpline *source,
                        const tsBSpline *candidate,
                        tsReal value,
                        tsReal tolerance,
                        tsStatus *status)
{
	const tsReal *knots = ts_bspline_knots_ptr(source);
	const size_t n_knots = ts_bspline_num_knots(source);
	const size_t order = ts_bspline_order(source);
	const size_t dim = ts_bspline_dimension(source);
	tsReal dom_min, dom_max;
	tsReal params[3];
	size_t n_params, i;

	ts_bspline_domain(source, &dom_min, &dom_max);
	n_params = 0;
	params[n_params++] = value;

	/* Midpoint of the intervals adjacent to the removed knot. */
	{
		size_t i0 = 0;
		while (i0 < n_knots && knots[i0] < value)
			i0++;
		if (i0 > order && knots[i0 - 1] > dom_min)
			params[n_params++] =
				(knots[i0 - 1] + value) * (tsReal) 0.5;
		if (i0 < n_knots - order && knots[i0] < dom_max)
			params[n_params++] =
				(value + knots[i0]) * (tsReal) 0.5;
	}

	for (i = 0; i < n_params; i++) {
		tsDeBoorNet na = ts_deboornet_init();
		tsDeBoorNet nb = ts_deboornet_init();
		tsReal *pa = NULL, *pb = NULL;
		tsError err;
		err = ts_bspline_eval(source, params[i], &na, status);
		if (err == TS_SUCCESS)
			err = ts_bspline_eval(candidate, params[i], &nb, status);
		if (err == TS_SUCCESS)
			err = ts_deboornet_result(&na, &pa, status);
		if (err == TS_SUCCESS)
			err = ts_deboornet_result(&nb, &pb, status);
		if (err == TS_SUCCESS &&
		    ts_distance(pa, pb, dim) > tolerance) {
			free(pa);
			free(pb);
			ts_deboornet_free(&na);
			ts_deboornet_free(&nb);
			return 0;
		}
		free(pa);
		free(pb);
		ts_deboornet_free(&na);
		ts_deboornet_free(&nb);
		if (err != TS_SUCCESS)
			return -1;
	}
	return 1;
}

/* Tries a single removal of a knot whose last index is `k` with current
 * multiplicity `mult`. On acceptance, `work` is replaced by the reduced
 * spline and *accepted is set to 1. The certificate always refers to
 * `source`. Regular rejections yield TS_SUCCESS with *accepted == 0. */
static tsError
ts_reduce_try_one(const tsBSpline *source,
                  tsBSpline *work,
                  size_t k,
                  size_t mult,
                  tsReal tolerance,
                  int *accepted,
                  tsStatus *status)
{
	tsBSpline candidate = ts_bspline_init();
	tsReal value;
	tsReal bound;
	tsError err;
	int rc;

	*accepted = 0;

	err = ts_reduce_build_candidate(work, k, mult, &candidate, status);
	if (err != TS_SUCCESS) {
		if (err == TS_NO_RESULT) {
			if (status != NULL) {
				status->code = TS_SUCCESS;
				status->message[0] = '\0';
			}
			return TS_SUCCESS;
		}
		return err;
	}

	value = ts_bspline_knots_ptr(work)[k];
	rc = ts_reduce_sample_filter(source, &candidate, value,
	                             tolerance, status);
	if (rc < 0) {
		ts_bspline_free(&candidate);
		return status != NULL ? status->code : TS_NO_RESULT;
	}
	if (rc == 0) {
		ts_bspline_free(&candidate);
		return TS_SUCCESS;
	}

	err = ts_reduce_certified_bound(source, &candidate, &bound, status);
	if (err != TS_SUCCESS) {
		ts_bspline_free(&candidate);
		return err;
	}
	if (bound > tolerance) {
		ts_bspline_free(&candidate);
		return TS_SUCCESS;
	}

	ts_bspline_free(work);
	ts_bspline_move(&candidate, work);
	*accepted = 1;
	return TS_SUCCESS;
}

/* Commits a finished working spline into the public output, handling
 * aliasing and pre-existing output memory. */
static tsError
ts_reduce_commit(const tsBSpline *source,
                 tsBSpline *out,
                 tsBSpline *work)
{
	if (source == out)
		ts_bspline_free(out);
	else if (out->pImpl != NULL)
		ts_bspline_free(out);
	ts_bspline_move(work, out);
	return TS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
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
	tsReal value;
	size_t idx = 0, mult = 0;
	size_t done = 0;
	tsReal bound;

	if (status != NULL) {
		status->code = TS_SUCCESS;
		status->message[0] = '\0';
	}

	TS_CALL_ROE(err, ts_reduce_validate(source, tolerance, out, status))

	TS_TRY(try, err, status)
		TS_CALL(try, err, ts_bspline_copy(source, &work, status))

		if (count == 0) {
			/* Independent copy, nothing to remove. */
		} else {
			TS_CALL(try, err, ts_reduce_match_knot(
			        source, knot, &value, &idx, &mult, status))

			/* End knots may not be removed. */
			{
				tsReal dom_min, dom_max;
				ts_bspline_domain(source, &dom_min, &dom_max);
				if (ts_reduce_same(value, dom_min) ||
				    ts_reduce_same(value, dom_max)) {
					TS_THROW_0(try, err, status, TS_U_UNDEFINED,
					           "end knots cannot be removed")
				}
			}

			/* Fracture barrier: full internal multiplicity. */
			if (mult != ts_bspline_order(source)) {
				while (done < count) {
					int accepted = 0;
					tsError match_err;
					/* Re-locate the knot in the current working
					 * curve. Once all copies are consumed the knot
					 * is simply gone and the loop ends. */
					match_err = ts_reduce_match_knot(
						&work, value, &value, &idx, &mult,
						status);
					if (match_err != TS_SUCCESS) {
						/* Knot fully consumed: not a failure. */
						if (status != NULL) {
							status->code = TS_SUCCESS;
							status->message[0] = '\0';
						}
						break;
					}
					if (mult == ts_bspline_order(&work))
						break; /* barrier reached */
					TS_CALL(try, err, ts_reduce_try_one(
					        source, &work, idx, mult,
					        tolerance, &accepted, status))
					if (!accepted)
						break; /* regular rejection */
					done++;
				}
			}
		}

		TS_CALL(try, err, ts_reduce_certified_bound(
		        source, &work, &bound, status))
		if (bound > tolerance) {
			TS_THROW_0(try, err, status, TS_NO_RESULT,
			           "internal error: certified bound exceeds tolerance")
		}
		ts_reduce_commit(source, out, &work);
		if (removed != NULL)
			*removed = done;
		if (error_bound != NULL)
			*error_bound = bound;
	TS_FINALLY
		ts_bspline_free(&work);
	TS_END_TRY_RETURN(err)
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
	size_t total = 0;
	tsReal bound;
	int restart;

	if (status != NULL) {
		status->code = TS_SUCCESS;
		status->message[0] = '\0';
	}

	TS_CALL_ROE(err, ts_reduce_validate(source, tolerance, out, status))

	TS_TRY(try, err, status)
		TS_CALL(try, err, ts_bspline_copy(source, &work, status))

		/* Repeated ascending passes over the internal knots. Every
		 * accepted removal restarts the scan. All certificates are
		 * computed against the original `source` curve inside
		 * ts_reduce_try_one, so the tolerance is a single global
		 * error budget. */
		restart = 1;
		while (restart) {
			const tsReal *knots = ts_bspline_knots_ptr(&work);
			const size_t order = ts_bspline_order(&work);
			const size_t n_knots = ts_bspline_num_knots(&work);
			size_t i;
			restart = 0;
			for (i = order; i + order < n_knots; ) {
				tsReal v = knots[i];
				size_t k = i;
				size_t mult = 1;
				int accepted = 0;
				while (k + 1 < n_knots &&
				       ts_reduce_same(knots[k + 1], v)) {
					k++;
					mult++;
				}
				if (mult < order) { /* skip barriers */
					TS_CALL(try, err, ts_reduce_try_one(
					        source, &work, k, mult,
					        tolerance, &accepted, status))
					if (accepted) {
						total++;
						restart = 1;
						break;
					}
				}
				i = k + 1;
			}
		}

		TS_CALL(try, err, ts_reduce_certified_bound(
		        source, &work, &bound, status))
		if (bound > tolerance) {
			TS_THROW_0(try, err, status, TS_NO_RESULT,
			           "internal error: certified bound exceeds tolerance")
		}
		ts_reduce_commit(source, out, &work);
		if (removed != NULL)
			*removed = total;
		if (error_bound != NULL)
			*error_bound = bound;
	TS_FINALLY
		ts_bspline_free(&work);
	TS_END_TRY_RETURN(err)
}
