/** @file
 *
 * @brief Implementation of the curve/curve intersection query declared in
 *        tinyspline_intersect.h.
 *
 * Algorithm overview
 * ------------------
 *
 * 1. Input validation and decomposition of each clamped B-spline into its
 *    Bezier pieces (::ts_bspline_to_beziers).  Every piece remembers the
 *    original non-zero knot interval and its lower knot index, so all
 *    reported parameters live on the original knot domains.
 *
 * 2. Conservative exclusion: each restricted arc carries an axis aligned
 *    bounding box of its Bernstein control polygon.  The convex hull
 *    property certifies these boxes as enclosures, so a pair whose boxes are
 *    farther apart than the spatial tolerance cannot intersect.
 *
 * 3. Divide and conquer on the joint parameter domain [0,1]^2 of every
 *    surviving piece pair.  Boxes are split along the axis with the larger
 *    normalized width until both widths are below the corresponding
 *    parameter tolerance.  Neither fixed grid sampling nor endpoint seeded
 *    Newton iteration is used.
 *
 * 4. Coincidence certification: an overlap is recorded only when the two
 *    restricted arcs are algebraically identical under an affine parameter
 *    map (both orientations are tested).  The check combines a coefficient
 *    identity with a dense independent sample evaluation; endpoint,
 *    midpoint or tangent comparisons alone are never accepted.
 *
 * 5. Leaf refinement: an isolated root inside a tolerance sized leaf is
 *    refined with a damped Levenberg-Marquardt iteration on the squared
 *    residual.  Singular Jacobians (tangencies, zero velocity) never cause a
 *    division by zero: the regularized 2x2 normal equation stays invertible
 *    and progress is guarded by a residual descent test.  Several seeds are
 *    tried inside every leaf.  Leaves that still cannot be resolved are
 *    reported as unresolved boxes instead of being silently dropped.
 *
 * Only the C standard library is used.
 */
#include "tinyspline_intersect.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TS_INTERSECT_MAX_DEG
/** Maximum polynomial degree handled internally (3*3 with ample margin). */
#define TS_INTERSECT_MAX_DEG 12
#endif

/** Relative floor for parameter subdivision. */
#define TS_INTERSECT_PREC_FLOOR ((tsReal) 1e-12)

/** Number of subdivisions of the independent identity sample check. */
#define TS_INTERSECT_IDENTITY_SAMPLES 16

/** Maximum LM iterations per seed. */
#define TS_INTERSECT_LM_ITER 32

/**
 * Minimum geometric fraction of an original piece at which an affine
 * identity is accepted as a coincidence.  The gate is based on the control
 * polygon length rather than the parameter span so it is independent of the
 * knot parameterization: on a geometrically tiny sub-arc every smooth curve
 * matches its affine approximation, which would turn an isolated contact
 * into a spurious overlap, whereas a genuine partial coincidence spans a
 * substantial fraction of its original piece.
 */
#define TS_INTERSECT_IDENTITY_MIN_FRAC ((tsReal) 0.0625)

/* ======================================================================= *
 * Small numerical helpers                                                 *
 * ======================================================================= */

static tsReal
ts_int_abs(tsReal x)
{
	return x < 0 ? -x : x;
}

static tsReal
ts_int_max(tsReal a, tsReal b)
{
	return a > b ? a : b;
}

static tsReal
ts_int_min(tsReal a, tsReal b)
{
	return a < b ? a : b;
}

static int
ts_int_finite(tsReal x)
{
	return isfinite((double) x);
}

/**
 * Returns the lower index of the non-zero knot interval starting at @p
 * value, i.e. the index of the last knot equal to @p value.
 */
static size_t
ts_int_interval_index(const tsReal *knots,
                      size_t num_knots,
                      tsReal value)
{
	size_t i, k = 0;
	tsReal eps = TS_INTERSECT_PREC_FLOOR *
	             ts_int_max((tsReal) 1.0, ts_int_abs(value));
	for (i = 0; i < num_knots; i++)
		if (ts_int_abs(knots[i] - value) <= eps)
			k = i;
	return k;
}

/** Counts the multiplicity of the knot at @p value. */
static size_t
ts_int_knot_multiplicity(const tsReal *knots,
                         size_t num_knots,
                         tsReal value)
{
	size_t i, m = 0;
	tsReal eps = TS_INTERSECT_PREC_FLOOR *
	             ts_int_max((tsReal) 1.0, ts_int_abs(value));
	for (i = 0; i < num_knots; i++)
		if (ts_int_abs(knots[i] - value) <= eps)
			m++;
	return m;
}

/* ======================================================================= *
 * Growable arrays                                                          *
 * ======================================================================= */

#define TS_INT_DEFINE_DA(name, type)                                       \
	typedef struct { type *items; size_t n, cap; } name##Array;        \
	static tsError name##_push(name##Array *da, const type *v)         \
	{                                                                    \
		if (da->n == da->cap) {                                     \
			size_t nc = da->cap ? da->cap * 2 : 8;             \
			type *p = (type *) realloc(da->items,             \
			                           nc * sizeof(type));     \
			if (!p) return TS_MALLOC;                          \
			da->items = p; da->cap = nc;                       \
		}                                                            \
		da->items[da->n++] = *v;                                    \
		return TS_SUCCESS;                                           \
	}

TS_INT_DEFINE_DA(ts_int_point, tsIntersectionPoint)
TS_INT_DEFINE_DA(ts_int_overlap, tsIntersectionOverlap)
TS_INT_DEFINE_DA(ts_int_box, tsIntersectionBox)

/* ======================================================================= *
 * Bezier pieces with original parameter intervals                          *
 * ======================================================================= */

static tsReal ts_int_polygon_length(const tsReal *cp, int deg, int dim);

typedef struct
{
	const tsReal *cp; /**< Bernstein control points, (deg+1)*dim values. */
	tsReal u0, u1;    /**< Original knot interval. */
	size_t ui;        /**< Lower index of the original interval. */
	tsReal full_len;  /**< Control polygon length of the full piece. */
} TsPiece;

typedef struct
{
	TsPiece *items;
	size_t n;
	int deg;
	int dim;
	tsBSpline bz;        /**< Owning Bezier decomposition. */
	const tsReal *knots; /**< Original knot vector. */
	size_t num_knots;
} TsPieces;

static void
ts_int_pieces_init(TsPieces *p)
{
	p->items = NULL;
	p->n = 0;
	p->deg = 0;
	p->dim = 0;
	p->bz = ts_bspline_init();
	p->knots = NULL;
	p->num_knots = 0;
}

static void
ts_int_pieces_free(TsPieces *p)
{
	free(p->items);
	ts_bspline_free(&p->bz);
	ts_int_pieces_init(p);
}

/**
 * Decomposes @p spline into Bezier pieces while preserving the original
 * knot intervals.  Returns TS_NO_RESULT for constant splines.
 */
static tsError
ts_int_pieces_build(const tsBSpline *spline,
                    TsPieces *out,
                    tsStatus *status)
{
	tsError err;
	size_t order, num_pieces, i;
	const tsReal *bzcp, *bzknots, *orig_knots;

	TS_TRY(try, err, status)
		TS_CALL(try, err, ts_bspline_to_beziers(spline, &out->bz, status))

		out->deg = (int) ts_bspline_degree(&out->bz);
		out->dim = (int) ts_bspline_dimension(&out->bz);
		order = (size_t) out->deg + 1;
		num_pieces = ts_bspline_num_control_points(&out->bz) / order;
		bzcp = ts_bspline_control_points_ptr(&out->bz);
		bzknots = ts_bspline_knots_ptr(&out->bz);
		orig_knots = ts_bspline_knots_ptr(spline);
		out->knots = orig_knots;
		out->num_knots = ts_bspline_num_knots(spline);

		out->items = (TsPiece *) malloc(
			ts_int_max((size_t) 1, num_pieces) * sizeof(TsPiece));
		if (!out->items) TS_THROW_0(try, err, status, TS_MALLOC,
		                            "out of memory")

		for (i = 0; i < num_pieces; i++) {
			out->items[i].cp = bzcp + i * order * (size_t) out->dim;
			out->items[i].u0 = bzknots[i * order];
			out->items[i].u1 = bzknots[(i + 1) * order];
			out->items[i].ui = ts_int_interval_index(
				orig_knots, out->num_knots, out->items[i].u0);
			out->items[i].full_len = ts_int_polygon_length(
				out->items[i].cp, out->deg, out->dim);
		}
		out->n = num_pieces;
	TS_FINALLY
	TS_END_TRY_RETURN(err)
}

/* ======================================================================= *
 * Bernstein / polynomial utilities                                         *
 * ======================================================================= */

/** Axis aligned bounding box of a Bernstein control polygon. */
static void
ts_int_bernstein_aabb(const tsReal *cp,
                      int deg,
                      int dim,
                      tsReal lo[3],
                      tsReal hi[3])
{
	int k, c;
	for (c = 0; c < 3; c++) {
		lo[c] = (tsReal) 1e300;
		hi[c] = -(tsReal) 1e300;
	}
	for (k = 0; k <= deg; k++)
		for (c = 0; c < dim; c++) {
			tsReal v = cp[k * dim + c];
			lo[c] = ts_int_min(lo[c], v);
			hi[c] = ts_int_max(hi[c], v);
		}
	for (c = dim; c < 3; c++) {
		lo[c] = 0;
		hi[c] = 0;
	}
}

/** Certified exclusion: gap in some component exceeds @p stol. */
static int
ts_int_aabbs_disjoint(const tsReal lo0[3],
                      const tsReal hi0[3],
                      const tsReal lo1[3],
                      const tsReal hi1[3],
                      tsReal stol)
{
	int c;
	for (c = 0; c < 3; c++)
		if (lo0[c] - hi1[c] > stol || lo1[c] - hi0[c] > stol)
			return 1;
	return 0;
}

/**
 * Certified sign exclusion on the tensor product Bernstein coefficients of
 * the difference system D(u,v) = p(v) - q(u).  The Bernstein coefficients of
 * D are exactly p_j - q_i for every pair of control points, so when every
 * coefficient of one component lies strictly beyond the spatial tolerance
 * with a common sign, the Euclidean distance stays above the tolerance
 * throughout the box and no tolerance level intersection can exist.
 * Exact tangencies (a coefficient touching zero) keep the box alive.  A
 * small scale relative margin guards against rounding.
 *
 * Returns 1 when the box is certified to contain no solution.
 */
static int
ts_int_difference_excludes(const tsReal *qcp,
                           const tsReal *pcp,
                           int da, int db, int dim,
                           tsReal scale, tsReal stol)
{
	tsReal thresh = stol + (tsReal) 1e-12 *
	                ts_int_max((tsReal) 1.0, scale);
	int c, i, j;
	for (c = 0; c < dim; c++) {
		int pos = 1, neg = 1;
		for (i = 0; i <= da && (pos || neg); i++)
			for (j = 0; j <= db; j++) {
				tsReal d = pcp[j * dim + c] -
				           qcp[i * dim + c];
				if (d < thresh) pos = 0;
				if (d > -thresh) neg = 0;
				if (!pos && !neg) break;
			}
		if (pos || neg) return 1;
	}
	return 0;
}

/** De Casteljau split of scalar Bernstein coefficients at @p t. */
static void
ts_int_bernstein_split(const tsReal *b,
                       int deg,
                       tsReal t,
                       tsReal *left,
                       tsReal *right)
{
	tsReal tmp[TS_INTERSECT_MAX_DEG + 1];
	int k, j;
	for (k = 0; k <= deg; k++) tmp[k] = b[k];
	left[0] = tmp[0];
	right[deg] = tmp[deg];
	for (j = 0; j < deg; j++) {
		for (k = 0; k + j < deg; k++)
			tmp[k] = (1 - t) * tmp[k] + t * tmp[k + 1];
		left[j + 1] = tmp[0];
		right[deg - j - 1] = tmp[deg - j - 1];
	}
}

/**
 * Restricts the scalar Bernstein polynomial @p b (degree @p deg) to the
 * local interval [@p t0, @p t1].  The result is expressed in Bernstein form
 * on [0,1] of the restricted arc.
 */
static void
ts_int_bernstein_restrict(const tsReal *b,
                          int deg,
                          tsReal t0,
                          tsReal t1,
                          tsReal *out)
{
	tsReal work[TS_INTERSECT_MAX_DEG + 1];
	tsReal left[TS_INTERSECT_MAX_DEG + 1];
	tsReal right[TS_INTERSECT_MAX_DEG + 1];
	tsReal s;
	int k;

	if (t0 <= 0.0) {
		for (k = 0; k <= deg; k++) work[k] = b[k];
	} else {
		ts_int_bernstein_split(b, deg, t0, left, right);
		for (k = 0; k <= deg; k++) work[k] = right[k];
	}
	s = (t1 - t0) / (1 - t0);
	if (s >= 1.0) {
		for (k = 0; k <= deg; k++) out[k] = work[k];
		return;
	}
	ts_int_bernstein_split(work, deg, s, left, right);
	for (k = 0; k <= deg; k++) out[k] = left[k];
}

/** Restricts a vector valued Bernstein arc (interleaved components). */
static void
ts_int_arc_restrict(const tsReal *cp,
                    int deg,
                    int dim,
                    tsReal t0,
                    tsReal t1,
                    tsReal *out)
{
	tsReal b[TS_INTERSECT_MAX_DEG + 1];
	tsReal r[TS_INTERSECT_MAX_DEG + 1];
	int k, c;
	for (c = 0; c < dim; c++) {
		for (k = 0; k <= deg; k++) b[k] = cp[k * dim + c];
		ts_int_bernstein_restrict(b, deg, t0, t1, r);
		for (k = 0; k <= deg; k++) out[k * dim + c] = r[k];
	}
}

/** Binomial coefficients up to @p n. */
static void
ts_int_binom(int n, tsReal *out)
{
	int k;
	out[0] = 1;
	for (k = 1; k <= n; k++)
		out[k] = out[k - 1] * (tsReal) ((n - k + 1.0) / k);
}

/** Converts scalar Bernstein coefficients to monomial coefficients. */
static void
ts_int_bernstein_to_monomial(const tsReal *b,
                             int deg,
                             tsReal *m)
{
	tsReal cbn[TS_INTERSECT_MAX_DEG + 1];
	tsReal cbk[TS_INTERSECT_MAX_DEG + 1];
	tsReal cbnk[TS_INTERSECT_MAX_DEG + 1];
	int p, k;
	ts_int_binom(deg, cbn);
	for (p = 0; p <= deg; p++) m[p] = 0;
	for (k = 0; k <= deg; k++) {
		ts_int_binom(k, cbk);
		for (p = k; p <= deg; p++) {
			ts_int_binom(deg - k, cbnk);
			m[p] += (((p - k) & 1) ? -1 : 1) *
			        cbn[k] * cbnk[p - k] * b[k];
		}
	}
}

/** Evaluates a monomial polynomial with Horner's scheme. */
static tsReal
ts_int_poly_eval(const tsReal *m, int deg, tsReal t)
{
	tsReal v = m[deg];
	int k;
	for (k = deg - 1; k >= 0; k--) v = v * t + m[k];
	return v;
}

/** Monomial derivative. */
static int
ts_int_poly_derivative(const tsReal *m, int deg, tsReal *out)
{
	int k;
	for (k = 1; k <= deg; k++) out[k - 1] = m[k] * (tsReal) k;
	return deg - 1;
}

/** Multiplies two monomial polynomials. */
static int
ts_int_poly_multiply(const tsReal *a, int da,
                     const tsReal *b, int db, tsReal *out)
{
	int i, j;
	for (i = 0; i <= da + db; i++) out[i] = 0;
	for (i = 0; i <= da; i++)
		for (j = 0; j <= db; j++)
			out[i + j] += a[i] * b[j];
	return da + db;
}

/**
 * Horner composition: result(t) = outer(inner(t)).
 * The result degree is deg_outer * deg_inner (<= 9 for the supported
 * degrees).
 */
static int
ts_int_poly_compose(const tsReal *outer, int deg_outer,
                    const tsReal *inner, int deg_inner, tsReal *out)
{
	tsReal acc[TS_INTERSECT_MAX_DEG + 1];
	tsReal tmp[TS_INTERSECT_MAX_DEG + 1];
	int deg_acc = 0, k;
	acc[0] = outer[deg_outer];
	for (k = deg_outer - 1; k >= 0; k--) {
		int nd = ts_int_poly_multiply(acc, deg_acc,
		                              inner, deg_inner, tmp);
		tmp[0] += outer[k];
		deg_acc = nd;
		{
			int j;
			for (j = 0; j <= deg_acc; j++) acc[j] = tmp[j];
		}
	}
	for (k = 0; k <= deg_acc; k++) out[k] = acc[k];
	return deg_acc;
}

/** Control polygon length proxy. */
static tsReal
ts_int_polygon_length(const tsReal *cp, int deg, int dim)
{
	tsReal len = 0;
	int k, c;
	for (k = 0; k < deg; k++) {
		tsReal d2 = 0;
		for (c = 0; c < dim; c++) {
			tsReal d = cp[(k + 1) * dim + c] - cp[k * dim + c];
			d2 += d * d;
		}
		len += sqrt(d2);
	}
	return len;
}

static int
ts_int_arc_is_constant(const tsReal *cp, int deg, int dim, tsReal scale)
{
	tsReal eps = (tsReal) 1e-10 * ts_int_max((tsReal) 1.0, scale);
	int k, c;
	for (k = 1; k <= deg; k++)
		for (c = 0; c < dim; c++)
			if (ts_int_abs(cp[k * dim + c] - cp[c]) > eps)
				return 0;
	return 1;
}

/**
 * Recursive sign-definiteness certification for one scalar Bernstein
 * derivative polynomial.  A component is monotone when its derivative never
 * changes sign; the convex hull property makes the Bernstein coefficient
 * range a certified enclosure, so sign definite coefficients settle the
 * question immediately.  When the raw coefficients straddle zero only because
 * of a loose enclosure, de Casteljau subdivision tightens the range until
 * the sign becomes definite.  A depth cap keeps the test finite; reaching it
 * means monotonicity cannot be certified, not that the arc is non-monotone.
 */
static int
ts_int_derivative_sign_definite(const tsReal *d, int deg,
                                tsReal eps, int depth)
{
	tsReal left[TS_INTERSECT_MAX_DEG + 1];
	tsReal right[TS_INTERSECT_MAX_DEG + 1];
	int pos = 1, neg = 1, k;
	for (k = 0; k <= deg; k++) {
		if (d[k] < -eps) pos = 0;
		if (d[k] > eps) neg = 0;
	}
	if (pos || neg) return 1;
	/* Coefficients genuinely on both sides: a sign change exists. */
	{
		int has_pos = 0, has_neg = 0;
		for (k = 0; k <= deg; k++) {
			if (d[k] > eps) has_pos = 1;
			if (d[k] < -eps) has_neg = 1;
		}
		if (has_pos && has_neg) return 0;
	}
	if (depth >= 18) return 0;
	ts_int_bernstein_split(d, deg, 0.5, left, right);
	return ts_int_derivative_sign_definite(left, deg, eps, depth + 1) &&
	       ts_int_derivative_sign_definite(right, deg, eps, depth + 1);
}

/**
 * Certified injectivity test for a restricted Bernstein arc: every component
 * derivative must be sign definite (zeros allowed) and at least one
 * component must be strictly monotone.  Under these conditions two different
 * parameters can never map to the same point, so any continuum of solutions
 * inside a box where both arcs pass this test follows a single, one-to-one
 * parameter correspondence.  The derivative sign is certified by
 * subdivision, which stays correct when the raw Bernstein coefficients give
 * a loose enclosure of a one-signed derivative.
 */
static int
ts_int_arc_strictly_monotone(const tsReal *cp, int deg, int dim,
                             tsReal scale)
{
	tsReal eps = (tsReal) 1e-10 * ts_int_max((tsReal) 1.0, scale);
	tsReal deriv[TS_INTERSECT_MAX_DEG + 1];
	int strict = 0, c, k;
	for (c = 0; c < dim; c++) {
		for (k = 0; k < deg; k++)
			deriv[k] = (tsReal) deg *
			    (cp[(k + 1) * dim + c] - cp[k * dim + c]);
		if (!ts_int_derivative_sign_definite(deriv, deg - 1,
		                                     eps, 0))
			return 0;
		for (k = 0; k < deg; k++)
			if (ts_int_abs(deriv[k]) > eps) strict = 1;
	}
	return strict;
}

/* ======================================================================= *
 * Coincidence certification                                                *
 * ======================================================================= */

/**
 * Elevates scalar Bernstein coefficients from degree @p deg to degree
 * @p deg + 1.
 */
static void
ts_int_bernstein_elevate_once(const tsReal *b, int deg, tsReal *out)
{
	int k;
	out[0] = b[0];
	for (k = 1; k <= deg; k++)
		out[k] = ((tsReal) k / (deg + 1)) * b[k - 1] +
		         ((tsReal) (deg + 1 - k) / (deg + 1)) * b[k];
	out[deg + 1] = b[deg];
}

/** Elevates a vector valued Bernstein arc to degree @p target. */
static void
ts_int_arc_elevate(const tsReal *cp, int deg, int target, int dim,
                   tsReal *out)
{
	tsReal cur[(TS_INTERSECT_MAX_DEG + 1) * 3];
	tsReal nxt[(TS_INTERSECT_MAX_DEG + 1) * 3];
	tsReal b[TS_INTERSECT_MAX_DEG + 1];
	tsReal e[TS_INTERSECT_MAX_DEG + 1];
	int d, k, c;
	for (k = 0; k <= deg; k++)
		for (c = 0; c < dim; c++)
			cur[k * dim + c] = cp[k * dim + c];
	for (d = deg; d < target; d++) {
		for (c = 0; c < dim; c++) {
			for (k = 0; k <= d; k++) b[k] = cur[k * dim + c];
			ts_int_bernstein_elevate_once(b, d, e);
			for (k = 0; k <= d + 1; k++)
				nxt[k * dim + c] = e[k];
		}
		for (k = 0; k <= d + 1; k++)
			for (c = 0; c < dim; c++)
				cur[k * dim + c] = nxt[k * dim + c];
	}
	for (k = 0; k <= target; k++)
		for (c = 0; c < dim; c++)
			out[k * dim + c] = cur[k * dim + c];
}

/**
 * Tests whether the two restricted arcs are algebraically identical under
 * the local affine map v = slope * u + offset.  Arcs of different degree
 * are first elevated to their common degree.  A coefficient identity on
 * every component must hold, followed by a dense independent sample check.
 * Endpoint, midpoint or tangent-only comparisons are never accepted.
 */
static int
ts_int_affine_identity(const tsReal *qcp,
                       const tsReal *pcp,
                       int da,
                       int db,
                       int dim,
                       tsReal slope,
                       tsReal offset,
                       tsReal scale)
{
	tsReal qhi[(TS_INTERSECT_MAX_DEG + 1) * 3];
	tsReal phi[(TS_INTERSECT_MAX_DEG + 1) * 3];
	tsReal map[2];
	tsReal qm[TS_INTERSECT_MAX_DEG + 1];
	tsReal pm[TS_INTERSECT_MAX_DEG + 1];
	tsReal comp[TS_INTERSECT_MAX_DEG + 1];
	tsReal bc[TS_INTERSECT_MAX_DEG + 1];
	tsReal tol = (tsReal) 1e-9 * ts_int_max((tsReal) 1.0, scale);
	int deg = da > db ? da : db;
	int c, k, s;

	ts_int_arc_elevate(qcp, da, deg, dim, qhi);
	ts_int_arc_elevate(pcp, db, deg, dim, phi);
	map[0] = offset;
	map[1] = slope;
	for (c = 0; c < dim; c++) {
		for (k = 0; k <= deg; k++) bc[k] = qhi[k * dim + c];
		ts_int_bernstein_to_monomial(bc, deg, qm);
		for (k = 0; k <= deg; k++) bc[k] = phi[k * dim + c];
		ts_int_bernstein_to_monomial(bc, deg, pm);
		ts_int_poly_compose(qm, deg, map, 1, comp);
		for (k = 0; k <= deg; k++)
			if (ts_int_abs(pm[k] - comp[k]) > tol)
				return 0;
		for (s = 0; s <= TS_INTERSECT_IDENTITY_SAMPLES; s++) {
			tsReal u = (tsReal) s /
			           TS_INTERSECT_IDENTITY_SAMPLES;
			tsReal v = slope * u + offset;
			tsReal d = ts_int_poly_eval(pm, deg, v) -
			           ts_int_poly_eval(qm, deg, u);
			if (ts_int_abs(d) > tol * 4) return 0;
		}
	}
	return 1;
}

/* ======================================================================= *
 * Piece level coincidence detection                                        *
 * ======================================================================= */

/** Evaluates one component of a Bernstein arc at local parameter t. */
static tsReal
ts_int_bern_eval_component(const tsReal *cp, int deg, int dim,
                           int component, tsReal t)
{
	tsReal b[TS_INTERSECT_MAX_DEG + 1];
	int k;
	for (k = 0; k <= deg; k++)
		b[k] = cp[k * dim + component];
	/* de Casteljau. */
	for (k = 0; k < deg; k++) {
		int i;
		for (i = 0; i + 1 <= deg - k; i++)
			b[i] = (1 - t) * b[i] + t * b[i + 1];
	}
	return b[0];
}

/**
 * Inverts a strictly monotone scalar Bernstein component: returns the local
 * parameter t in [0,1] at which the component equals @p target.  Bisection
 * is used so a flat or rank deficient derivative never causes a division by
 * zero.  @p increasing selects the branch direction.
 */
static tsReal
ts_int_bern_invert_component(const tsReal *cp, int deg, int dim,
                             int component, tsReal target,
                             int increasing)
{
	tsReal lo = 0, hi = 1;
	int it;
	for (it = 0; it < 80; it++) {
		tsReal mid = (lo + hi) / 2;
		tsReal val = ts_int_bern_eval_component(
			cp, deg, dim, component, mid);
		if (hi - lo < (tsReal) 1e-14)
			return mid;
		if ((val < target) == increasing)
			lo = mid;
		else
			hi = mid;
	}
	return (lo + hi) / 2;
}

/**
 * Picks a component whose restricted Bernstein derivative is strictly
 * monotone (and therefore invertible) on the full local [0,1] arc.  Returns
 * the component index or -1 when none qualifies.
 */
static int
ts_int_invertible_component(const tsReal *cp, int deg, int dim,
                            tsReal scale, int *increasing)
{
	int c;
	for (c = 0; c < dim; c++) {
		int pos = 1, neg = 1, strict = 0, k;
		tsReal eps = (tsReal) 1e-9 *
		             ts_int_max((tsReal) 1.0, scale);
		for (k = 0; k < deg; k++) {
			tsReal d = (tsReal) deg *
			    (cp[(k + 1) * dim + c] - cp[k * dim + c]);
			if (d < -eps) pos = 0;
			if (d > eps) neg = 0;
			if (ts_int_abs(d) > eps) strict = 1;
		}
		if (strict && (pos || neg)) {
			*increasing = pos ? 1 : 0;
			return c;
		}
	}
	return -1;
}

/** Local parameter sub-range shared by two coincident pieces. */
typedef struct
{
	tsReal lu0, lu1, lv0, lv1;
} TsLocalBox;

/**
 * Detects an algebraic coincidence between two whole Bezier pieces and, on
 * success, reports the maximal shared sub-arc.
 *
 * The method is independent of how much of each piece is shared:
 *  - a strictly monotone (hence invertible) coordinate component is chosen
 *    on each arc;
 *  - the intersection of the two component coordinate ranges gives the
 *    geometrically shared sub-arc;
 *  - both arcs are restricted to that sub-arc and re-parameterized to
 *    [0,1], where an affine correspondence is necessarily either the
 *    identity (t -> t) or the reversal (t -> 1-t);
 *  - one of those two hypotheses must be an exact polynomial identity on
 *    every component (plus a dense independent sample check).
 *
 * On success @p box receives the shared sub-arc in the original pieces'
 * local coordinates and @p slope / @p offset the map
 * lv = slope * lu + offset in that same frame.  Returns 1 on a certified
 * coincidence, 0 otherwise.
 */
static int
ts_int_piece_affine_coincidence(const TsPiece *qa,
                                const TsPiece *pb,
                                int da, int db, int dim,
                                tsReal scale,
                                TsLocalBox *box,
                                tsReal *slope_out,
                                tsReal *offset_out)
{
	const tsReal *qcp = qa->cp;
	const tsReal *pcp = pb->cp;
	tsReal qlo[3], qhi[3], plo[3], phi[3];
	tsReal clo, chi, u0, u1, v0, v1;
	tsReal qsub[(TS_INTERSECT_MAX_DEG + 1) * 3];
	tsReal psub[(TS_INTERSECT_MAX_DEG + 1) * 3];
	int qinc, pinc, comp, same_dir;

	comp = ts_int_invertible_component(qcp, da, dim, scale, &qinc);
	if (comp < 0)
		return 0;
	if (ts_int_invertible_component(pcp, db, dim, scale,
	                                &pinc) < 0)
		return 0;
	same_dir = (qinc == pinc);

	ts_int_bernstein_aabb(qcp, da, dim, qlo, qhi);
	ts_int_bernstein_aabb(pcp, db, dim, plo, phi);
	clo = ts_int_max(qlo[comp], plo[comp]);
	chi = ts_int_min(qhi[comp], phi[comp]);
	if (chi < clo - (tsReal) 1e-9 * ts_int_max(1.0, scale))
		return 0;

	/* Recover the local parameter ranges that map into the shared
	 * coordinate range.  Bisection inversion never divides by a zero
	 * velocity. */
	u0 = ts_int_bern_invert_component(qcp, da, dim, comp, clo, qinc);
	u1 = ts_int_bern_invert_component(qcp, da, dim, comp, chi, qinc);
	if (u1 < u0) { tsReal t = u0; u0 = u1; u1 = t; }
	v0 = ts_int_bern_invert_component(pcp, db, dim, comp, clo, pinc);
	v1 = ts_int_bern_invert_component(pcp, db, dim, comp, chi, pinc);
	if (v1 < v0) { tsReal t = v0; v0 = v1; v1 = t; }
	if (u1 - u0 < (tsReal) 1e-10 || v1 - v0 < (tsReal) 1e-10)
		return 0;

	/* Restrict and re-parameterize both arcs to [0,1]. */
	ts_int_arc_restrict(qcp, da, dim, u0, u1, qsub);
	ts_int_arc_restrict(pcp, db, dim, v0, v1, psub);

	/* On the normalized sub-arcs the only affine maps are identity and
	 * reversal.  Require the geometrically meaningful segment gate. */
	{
		tsReal lq = ts_int_polygon_length(qsub, da, dim);
		tsReal lp = ts_int_polygon_length(psub, db, dim);
		if (lq < TS_INTERSECT_IDENTITY_MIN_FRAC * qa->full_len ||
		    lp < TS_INTERSECT_IDENTITY_MIN_FRAC * pb->full_len)
			return 0;
	}

	if (same_dir) {
		if (!ts_int_affine_identity(qsub, psub, da, db, dim,
		                           1.0, 0.0, scale))
			return 0;
	} else {
		if (!ts_int_affine_identity(qsub, psub, da, db, dim,
		                           -1.0, 1.0, scale))
			return 0;
	}

	/* Translate the normalized relation back to the pieces' local
	 * frame lv = slope*lu + offset. */
	if (same_dir) {
		*slope_out = (v1 - v0) / (u1 - u0);
		*offset_out = v0 - *slope_out * u0;
	} else {
		*slope_out = -(v1 - v0) / (u1 - u0);
		*offset_out = v1 - *slope_out * u0;
	}
	box->lu0 = u0;
	box->lu1 = u1;
	box->lv0 = ts_int_min(v0, v1);
	box->lv1 = ts_int_max(v0, v1);
	return 1;
}


/** Certified overlap relation in original parameters (for strip pruning). */
typedef struct
{
	tsReal u0, u1;     /**< Segment range on the first original axis. */
	tsReal alpha, beta; /**< v = alpha * u + beta. */
	size_t ui, vi;
} TsRelation;

typedef struct
{
	TsRelation *items;
	size_t n, cap;
} TsRelationList;

static tsError
ts_int_relation_push(TsRelationList *r, const TsRelation *v)
{
	if (r->n == r->cap) {
		size_t nc = r->cap ? r->cap * 2 : 8;
		TsRelation *p = (TsRelation *) realloc(
			r->items, nc * sizeof(TsRelation));
		if (!p) return TS_MALLOC;
		r->items = p;
		r->cap = nc;
	}
	r->items[r->n++] = *v;
	return TS_SUCCESS;
}

typedef struct
{
	const TsPieces *pa;
	const TsPieces *pb;
	const tsBSpline *sa; /**< Original first spline for final verification. */
	const tsBSpline *sb; /**< Original second spline for final verification. */
	tsReal stol, utol, vtol;
	size_t budget;
	size_t expanded;
	int incomplete;
	int reason;
	ts_int_pointArray points;
	ts_int_overlapArray overlaps;
	ts_int_boxArray boxes;
	TsRelationList relations;
} TsContext;

/** A box in local piece coordinates, always a sub-box of [0,1]^2. */
typedef struct
{
	tsReal lu0, lu1, lv0, lv1;
	size_t ui, vi;
	/**
	 * Nonzero once the restricted arcs are known to be non-degenerate
	 * and strictly monotone (up to orientation), which certifies that
	 * every coincidence inside the box follows a single affine map.
	 */
	int monotone;
} TsWorkBox;

typedef struct
{
	TsWorkBox *items;
	size_t n, cap;
} TsWorkStack;

static tsError
ts_int_stack_push(TsWorkStack *s, TsWorkBox b)
{
	if (s->n == s->cap) {
		size_t nc = s->cap ? s->cap * 2 : 16;
		TsWorkBox *p = (TsWorkBox *) realloc(
			s->items, nc * sizeof(TsWorkBox));
		if (!p) return TS_MALLOC;
		s->items = p;
		s->cap = nc;
	}
	s->items[s->n++] = b;
	return TS_SUCCESS;
}

/* ======================================================================= *
 * Levenberg-Marquardt leaf refinement                                      *
 * ======================================================================= */

/**
 * Refines a solution of p(v) = q(u) starting from (@p u, @p v).  All
 * components are solved jointly.  The 2x2 normal equation is regularized
 * with a Levenberg-Marquardt damping factor, so singular Jacobians
 * (tangencies, isolated zero velocity points) never divide by zero.  A step
 * is accepted only when it decreases the residual.  Returns the achieved
 * Euclidean residual.
 */
static tsReal
ts_int_refine_lm(const tsReal qm[3][TS_INTERSECT_MAX_DEG + 1],
                 const tsReal pm[3][TS_INTERSECT_MAX_DEG + 1],
                 int da, int db, int dim,
                 tsReal u, tsReal v,
                 tsReal *u_out, tsReal *v_out)
{
	tsReal dq[3][TS_INTERSECT_MAX_DEG + 1];
	tsReal dp[3][TS_INTERSECT_MAX_DEG + 1];
	tsReal lambda = (tsReal) 1e-6;
	tsReal best = 0;
	int c, iter;

	for (c = 0; c < dim; c++) {
		ts_int_poly_derivative(qm[c], da, dq[c]);
		ts_int_poly_derivative(pm[c], db, dp[c]);
	}

	for (iter = 0; iter < TS_INTERSECT_LM_ITER; iter++) {
		tsReal F[3], Ju[3], Jv[3];
		tsReal a00, a01, a11, b0, b1, det, du, dv, residual = 0;
		u = ts_int_min(1, ts_int_max(0, u));
		v = ts_int_min(1, ts_int_max(0, v));
		for (c = 0; c < dim; c++) {
			tsReal qu = ts_int_poly_eval(qm[c], da, u);
			tsReal pv = ts_int_poly_eval(pm[c], db, v);
			F[c] = pv - qu;
			Ju[c] = -ts_int_poly_eval(dq[c], da - 1, u);
			Jv[c] = ts_int_poly_eval(dp[c], db - 1, v);
			residual += F[c] * F[c];
		}
		residual = sqrt(residual);
		if (iter == 0) best = residual;
		if (residual <= (tsReal) 1e-14) break;

		/* Normal equations A * d = -b with A = J^T J, b = J^T F. */
		a00 = a01 = a11 = b0 = b1 = 0;
		for (c = 0; c < dim; c++) {
			a00 += Ju[c] * Ju[c];
			a01 += Ju[c] * Jv[c];
			a11 += Jv[c] * Jv[c];
			b0 += Ju[c] * F[c];
			b1 += Jv[c] * F[c];
		}
		/* Regularize; grow lambda until the step descends. */
		{
			tsReal nu = u, nv = v, nr = residual;
			int tries;
			for (tries = 0; tries < 12; tries++) {
				tsReal l = lambda * (1 + a00 + a11);
				det = (a00 + l) * (a11 + l) - a01 * a01;
				if (det <= 0) { lambda *= 10; continue; }
				du = (-b0 * (a11 + l) + a01 * b1) / det;
				dv = ((a00 + l) * (-b1) + b0 * a01) / det;
				nu = ts_int_min(1, ts_int_max(0, u + du));
				nv = ts_int_min(1, ts_int_max(0, v + dv));
				nr = 0;
				for (c = 0; c < dim; c++) {
					tsReal f =
					    ts_int_poly_eval(pm[c], db, nv) -
					    ts_int_poly_eval(qm[c], da, nu);
					nr += f * f;
				}
				nr = sqrt(nr);
				if (nr < residual) {
					lambda = ts_int_max(
						(tsReal) 1e-12, lambda / 3);
					break;
				}
				lambda *= 10;
			}
			if (nr >= residual)
				break; /* No descent possible from here. */
			u = nu;
			v = nv;
			best = nr;
		}
	}
	*u_out = ts_int_min(1, ts_int_max(0, u));
	*v_out = ts_int_min(1, ts_int_max(0, v));
	return best;
}

/* ======================================================================= *
 * Emission helpers                                                         *
 * ======================================================================= */

static tsError
ts_int_emit_box(TsContext *ctx,
                const TsPiece *pa,
                const TsPiece *pb,
                const TsWorkBox *b,
                int reason)
{
	tsIntersectionBox box;
	box.u0 = pa->u0 + b->lu0 * (pa->u1 - pa->u0);
	box.u1 = pa->u0 + b->lu1 * (pa->u1 - pa->u0);
	box.v0 = pb->u0 + b->lv0 * (pb->u1 - pb->u0);
	box.v1 = pb->u0 + b->lv1 * (pb->u1 - pb->u0);
	box.ui = pa->ui;
	box.vi = pb->ui;
	box.reason = reason;
	ctx->incomplete = 1;
	ctx->reason = reason;
	return ts_int_box_push(&ctx->boxes, &box);
}

/**
 * Records an overlap certified in box-local coordinates under
 * lv = slope * lu + offset, converting the relation to original parameters.
 */
static tsError
ts_int_emit_overlap(TsContext *ctx,
                    const TsPiece *pa,
                    const TsPiece *pb,
                    const TsWorkBox *b,
                    tsReal slope,
                    tsReal offset)
{
	tsIntersectionOverlap ov;
	tsReal dua = pa->u1 - pa->u0;
	tsReal dub = pb->u1 - pb->u0;
	tsReal alpha = dub / dua * slope;
	tsReal beta = pb->u0 + dub * offset - alpha * pa->u0;
	tsReal uf = pa->u0 + b->lu0 * dua;
	tsReal ut = pa->u0 + b->lu1 * dua;

	ov.u_from = uf;
	ov.u_to = ut;
	ov.v_from = alpha * uf + beta;
	ov.v_to = alpha * ut + beta;
	ov.alpha = alpha;
	ov.beta = beta;
	ov.ui = pa->ui;
	ov.vi = pb->ui;
	{
		tsError err = ts_int_overlap_push(&ctx->overlaps, &ov);
		if (err) return err;
	}
	{
		TsRelation rel;
		rel.u0 = uf;
		rel.u1 = ut;
		rel.alpha = alpha;
		rel.beta = beta;
		rel.ui = pa->ui;
		rel.vi = pb->ui;
		return ts_int_relation_push(&ctx->relations, &rel);
	}
}

/** True if an original-parameter point lies on a certified relation. */
static int
ts_int_on_relation(const TsContext *ctx,
                   size_t ui, size_t vi,
                   tsReal u, tsReal v,
                   tsReal utol, tsReal vtol)
{
	size_t i;
	for (i = 0; i < ctx->relations.n; i++) {
		const TsRelation *r = &ctx->relations.items[i];
		tsReal rv, eps;
		if (r->ui != ui || r->vi != vi) continue;
		if (u < r->u0 - utol || u > r->u1 + utol) continue;
		rv = r->alpha * u + r->beta;
		eps = ts_int_max(vtol, ts_int_abs(r->alpha) * utol) * 4;
		if (ts_int_abs(rv - v) <= eps) return 1;
	}
	return 0;
}

/* ======================================================================= *
 * Certified root isolation inside a leaf box                               *
 * ======================================================================= */

/** Stack frame for the tensor product subdivision of one leaf. */
typedef struct
{
	tsReal qcp[(TS_INTERSECT_MAX_DEG + 1) * 3];
	tsReal pcp[(TS_INTERSECT_MAX_DEG + 1) * 3];
	tsReal u0, u1, v0, v1;
	int depth;
} TsLeafCell;

/** Hard caps guaranteeing termination of every leaf refinement. */
#define TS_INTERSECT_LEAF_MAX_DEPTH 26
#define TS_INTERSECT_LEAF_MAX_CELLS 320

/**
 * Convergence threshold for an isolated root.  Structural zero evidence is
 * accepted only at the machine scale: a difference coefficient below this
 * relative threshold is indistinguishable from an exact zero after rounding.
 * The band of near-zero but non-zero coefficients around a tangency is then
 * narrower (in parameter space) than the parameter tolerance, so exactly one
 * leaf can claim each genuine root.
 */
static tsReal
ts_int_converge_tol(tsReal scale)
{
	return (tsReal) 1e-12 * ts_int_max((tsReal) 1.0, scale);
}

typedef struct
{
	TsLeafCell *items;
	size_t n, cap;
} TsLeafStack;

static tsError
ts_int_leaf_stack_push(TsLeafStack *s, const TsLeafCell *c)
{
	if (s->n == s->cap) {
		size_t nc = s->cap ? s->cap * 2 : 16;
		TsLeafCell *p = (TsLeafCell *) realloc(
			s->items, nc * sizeof(TsLeafCell));
		if (!p) return TS_MALLOC;
		s->items = p;
		s->cap = nc;
	}
	s->items[s->n++] = *c;
	return TS_SUCCESS;
}

/**
 * Splits one component of a vector valued Bernstein arc at t = 0.5.
 */
static void
ts_int_cell_split_arc(tsReal *cp, int deg, int dim, int along,
                      tsReal *left, tsReal *right)
{
	tsReal b[TS_INTERSECT_MAX_DEG + 1];
	tsReal bl[TS_INTERSECT_MAX_DEG + 1];
	tsReal br[TS_INTERSECT_MAX_DEG + 1];
	int k, c;
	for (c = 0; c < dim; c++) {
		for (k = 0; k <= deg; k++) b[k] = cp[k * dim + c];
		ts_int_bernstein_split(b, deg, 0.5, bl, br);
		for (k = 0; k <= deg; k++) {
			left[k * dim + c] = bl[k];
			right[k * dim + c] = br[k];
		}
	}
	(void) along;
}

/**
 * Converts the Bernstein arcs of a leaf cell to monomial form and returns
 * the Euclidean residual of p(v) - q(u) at the cell midpoint, optionally
 * polishing it with the damped LM iteration.  Singular Jacobians are
 * regularized internally, so zero velocity never divides by zero.
 */
static tsReal
ts_int_leaf_residual(const TsLeafCell *cell,
                     int da, int db, int dim,
                     int polish,
                     tsReal *u_out, tsReal *v_out)
{
	tsReal qm[3][TS_INTERSECT_MAX_DEG + 1];
	tsReal pm[3][TS_INTERSECT_MAX_DEG + 1];
	tsReal bc[TS_INTERSECT_MAX_DEG + 1];
	tsReal ru = (cell->u0 + cell->u1) / 2;
	tsReal rv = (cell->v0 + cell->v1) / 2;
	tsReal res = 0;
	int k, c;
	for (c = 0; c < dim; c++) {
		for (k = 0; k <= da; k++)
			bc[k] = cell->qcp[k * dim + c];
		ts_int_bernstein_to_monomial(bc, da, qm[c]);
		for (k = 0; k <= db; k++)
			bc[k] = cell->pcp[k * dim + c];
		ts_int_bernstein_to_monomial(bc, db, pm[c]);
	}
	if (polish) {
		tsReal ru2, rv2;
		(void) ts_int_refine_lm(qm, pm, da, db, dim,
		                        ru, rv, &ru2, &rv2);
		ru = ru2;
		rv = rv2;
	}
	for (c = 0; c < dim; c++) {
		tsReal f = ts_int_poly_eval(pm[c], db, rv) -
		           ts_int_poly_eval(qm[c], da, ru);
		res += f * f;
	}
	*u_out = ru;
	*v_out = rv;
	return sqrt(res);
}

/**
 * Joint exact-zero test for a cell.  Every component must independently be
 * compatible with an exact zero: its tensor product difference coefficients
 * either straddle zero (a sign change guarantees a root by the intermediate
 * value property) or include a coefficient that lands on zero at the machine
 * convergence scale (an even multiplicity tangency, which never changes
 * sign).  If a single component keeps a strict common sign beyond that
 * scale, the cell cannot contain a simultaneous zero of all components and
 * is pruned, however small the arc distance gets.  This is what separates a
 * genuine tangency root from a nearby parallel pass.
 */
static int
ts_int_cell_allows_joint_zero(const TsLeafCell *cell,
                              int da, int db, int dim,
                              tsReal scale)
{
	tsReal eps = ts_int_converge_tol(scale);
	int c, i, j;
	for (c = 0; c < dim; c++) {
		int seen_pos = 0, seen_neg = 0, touched = 0;
		for (i = 0; i <= da; i++)
			for (j = 0; j <= db; j++) {
				tsReal d = cell->pcp[j * dim + c] -
				           cell->qcp[i * dim + c];
				if (d > eps) seen_pos = 1;
				else if (d < -eps) seen_neg = 1;
				else touched = 1;
			}
		if (!touched && !(seen_pos && seen_neg))
			return 0;
	}
	return 1;
}

/**
 * Final verification against the ORIGINAL splines using the same de Boor
 * evaluation the caller uses for back substitution.  This is the authoritative
 * acceptance test: a candidate that only looks like an intersection on the
 * Bernstein representation but does not back-substitute within the spatial
 * tolerance on the original spline (for example a point numerically
 * indistinguishable from an end knot) is rejected.  Returns the Euclidean
 * distance between the two evaluated positions.
 */
static tsReal
ts_int_original_residual(const TsContext *ctx, int dim,
                         tsReal u, tsReal v)
{
	tsDeBoorNet na = ts_deboornet_init();
	tsDeBoorNet nb = ts_deboornet_init();
	tsStatus status;
	tsReal dist = 0;
	int c;
	ts_bspline_eval(ctx->sa, u, &na, &status);
	ts_bspline_eval(ctx->sb, v, &nb, &status);
	{
		const tsReal *qa = ts_deboornet_result_ptr(&na);
		const tsReal *qb = ts_deboornet_result_ptr(&nb);
		for (c = 0; c < dim; c++) {
			tsReal f = qb[c] - qa[c];
			dist += f * f;
		}
	}
	ts_deboornet_free(&na);
	ts_deboornet_free(&nb);
	return sqrt(dist);
}

/**
 * Certifies the roots inside a tolerance sized leaf whose arcs are given in
 * Bernstein form on [0,1].
 *
 * The midpoint is first polished with LM; a converged residual within the
 * spatial tolerance is an accepted root.  Otherwise the tensor product
 * difference is subdivided with strict Bernstein sign exclusion, which
 * provably discards every cell whose closest approach stays beyond the
 * tolerance (local distance minima can therefore never be reported).
 * Subdivision is bounded by a depth and a cell cap: ambiguity surviving
 * the caps is reported as an unresolved parameter box instead of looping.
 * Tangencies need no sign change because a zero touching the coefficient
 * range keeps its cell alive until LM confirms the residual.
 */
static tsError
ts_int_solve_leaf(TsContext *ctx,
                  const TsPiece *pa,
                  const TsPiece *pb,
                  const TsWorkBox *b,
                  const tsReal *qbox,
                  const tsReal *pbox,
                  int da, int db, int dim)
{
	TsLeafStack stack;
	TsLeafCell root;
	tsReal found_u[16], found_v[16];
	size_t num_found = 0;
	size_t cells_seen = 0;
	int unresolved = 0;
	tsError err = TS_SUCCESS;
	size_t i;

	stack.items = NULL;
	stack.n = stack.cap = 0;
	memcpy(root.qcp, qbox,
	       (size_t) (da + 1) * (size_t) dim * sizeof(tsReal));
	memcpy(root.pcp, pbox,
	       (size_t) (db + 1) * (size_t) dim * sizeof(tsReal));
	root.u0 = 0; root.u1 = 1;
	root.v0 = 0; root.v1 = 1;
	root.depth = 0;

	/* Fast path: a leaf that already admits a joint exact zero and whose
	 * polished midpoint meets the spatial tolerance is solved without
	 * subdivision.  Covers ordinary transverse roots and well conditioned
	 * tangencies. */
	{
		tsReal root_scale = 1, ru, rv, res;
		int k2, c2;
		for (k2 = 0; k2 <= da; k2++)
			for (c2 = 0; c2 < dim; c2++)
				root_scale = ts_int_max(root_scale,
				    ts_int_abs(root.qcp[k2 * dim + c2]));
		for (k2 = 0; k2 <= db; k2++)
			for (c2 = 0; c2 < dim; c2++)
				root_scale = ts_int_max(root_scale,
				    ts_int_abs(root.pcp[k2 * dim + c2]));
		if (ts_int_cell_allows_joint_zero(&root, da, db, dim,
		                                  root_scale)) {
			res = ts_int_leaf_residual(&root, da, db, dim, 1,
			                           &ru, &rv);
			if (res <= ctx->stol) {
				found_u[0] = ru;
				found_v[0] = rv;
				num_found = 1;
				goto emit_points;
			}
		}
	}

	/* Every surviving cell is first required to admit a simultaneous
	 * exact zero of all components; this prunes near-miss and local
	 * distance minimum cells.  The midpoint of an admissible cell is
	 * then polished and accepted once its back-substituted distance is
	 * within the spatial tolerance.  Subdivision localizes the root so
	 * the representative gets close enough even for tangencies, whose
	 * rank deficient Jacobian cannot make large Newton steps. */
	err = ts_int_leaf_stack_push(&stack, &root);
	if (err) goto done;

	while (stack.n > 0) {
		TsLeafCell cell = stack.items[--stack.n];
		tsReal scale = 1;
		tsReal wu = cell.u1 - cell.u0;
		tsReal wv = cell.v1 - cell.v0;
		int k, c;
		cells_seen++;

		for (k = 0; k <= da; k++)
			for (c = 0; c < dim; c++)
				scale = ts_int_max(scale,
				    ts_int_abs(cell.qcp[k * dim + c]));
		for (k = 0; k <= db; k++)
			for (c = 0; c < dim; c++)
				scale = ts_int_max(scale,
				    ts_int_abs(cell.pcp[k * dim + c]));

		/* Certified exclusion of the difference system. */
		if (ts_int_difference_excludes(cell.qcp, cell.pcp,
		                               da, db, dim, scale,
		                               ctx->stol))
			continue;

		/* Reject cells that cannot contain a joint exact zero. */
		if (!ts_int_cell_allows_joint_zero(&cell, da, db, dim,
		                                   scale))
			continue;

		/* Polish the cell midpoint and map the result back to the
		 * leaf parameter frame so all roots share one frame. */
		{
			tsReal ru, rv, res, lu, lv;
			res = ts_int_leaf_residual(&cell, da, db, dim, 1,
			                           &ru, &rv);
			lu = cell.u0 + ru * (cell.u1 - cell.u0);
			lv = cell.v0 + rv * (cell.v1 - cell.v0);
			if (res <= ctx->stol) {
				size_t j, dup = 0;
				for (j = 0; j < num_found; j++)
					if (ts_int_abs(found_u[j] - lu) < 1e-9 &&
					    ts_int_abs(found_v[j] - lv) < 1e-9) {
						dup = 1;
						break;
					}
				if (!dup && num_found < 16) {
					found_u[num_found] = lu;
					found_v[num_found] = lv;
					num_found++;
				}
				continue;
			}
		}

		/* Hard caps: keep the query finite.  Anything still
		 * ambiguous is surfaced as an unresolved box, never silently
		 * dropped and never looped on forever. */
		if (cell.depth >= TS_INTERSECT_LEAF_MAX_DEPTH ||
		    cells_seen >= TS_INTERSECT_LEAF_MAX_CELLS) {
			unresolved = 1;
			break;
		}

		/* Subdivide the wider axis; de Casteljau keeps every
		 * descendant coefficient set certified. */
		{
			TsLeafCell c1 = cell, c2 = cell;
			c1.depth = c2.depth = cell.depth + 1;
			if (wu >= wv) {
				ts_int_cell_split_arc(cell.qcp, da, dim, 0,
				                      c1.qcp, c2.qcp);
				c1.u1 = (cell.u0 + cell.u1) / 2;
				c2.u0 = (cell.u0 + cell.u1) / 2;
			} else {
				ts_int_cell_split_arc(cell.pcp, db, dim, 1,
				                      c1.pcp, c2.pcp);
				c1.v1 = (cell.v0 + cell.v1) / 2;
				c2.v0 = (cell.v0 + cell.v1) / 2;
			}
			err = ts_int_leaf_stack_push(&stack, &c1);
			if (err) goto done;
			err = ts_int_leaf_stack_push(&stack, &c2);
			if (err) goto done;
		}
	}

	if (unresolved) {
		TsWorkBox wb = *b;
		err = ts_int_emit_box(ctx, pa, pb, &wb,
		                      TS_INTERSECT_REASON_PRECISION);
		if (err) goto done;
	}

emit_points:
	for (i = 0; i < num_found; i++) {
		tsReal lu = b->lu0 + found_u[i] * (b->lu1 - b->lu0);
		tsReal lv = b->lv0 + found_v[i] * (b->lv1 - b->lv0);
		tsReal u = pa->u0 + lu * (pa->u1 - pa->u0);
		tsReal v = pb->u0 + lv * (pb->u1 - pb->u0);
		tsIntersectionPoint pt;
		tsReal hu = ctx->utol / 2;
		tsReal hv = ctx->vtol / 2;
		tsReal qm[3][TS_INTERSECT_MAX_DEG + 1];
		tsReal bc[TS_INTERSECT_MAX_DEG + 1];
		tsReal global_res = 0;
		int c, k;

		if (ts_int_on_relation(ctx, pa->ui, pb->ui, u, v,
		                       ctx->utol, ctx->vtol))
			continue;

		/* Geometric verification on the ORIGINAL full pieces.  de
		 * Casteljau evaluation of the Bernstein control polygons is
		 * numerically stable and rejects a leaf whose arcs only pass
		 * inside the tolerance band without meeting (a local distance
		 * minimum or a per-component, non-simultaneous zero). */
		global_res = 0;
		for (c = 0; c < dim; c++) {
			tsReal qv = ts_int_bern_eval_component(
				pa->cp, da, dim, c, lu);
			tsReal pv = ts_int_bern_eval_component(
				pb->cp, db, dim, c, lv);
			tsReal f = pv - qv;
			global_res += f * f;
		}
		global_res = sqrt(global_res);
		if (global_res > ctx->stol)
			continue;

		/* Authoritative verification with the same de Boor evaluation the
		 * caller uses for back substitution on the original splines.
		 * This rejects candidates that are geometrically coincident on
		 * the Bernstein representation but are numerically swallowed by
		 * an end knot on the original parameter axis, so every reported
		 * point back-substitutes within the spatial tolerance. */
		if (ts_int_original_residual(ctx, dim, u, v) > ctx->stol)
			continue;

		/* Monomial forms of the full first piece are still needed to
		 * evaluate the reported position. */
		for (c = 0; c < dim; c++) {
			for (k = 0; k <= da; k++)
				bc[k] = pa->cp[k * dim + c];
			ts_int_bernstein_to_monomial(bc, da, qm[c]);
		}

		/* Build a tolerance interval around the representative and then
		 * clamp it to the piece domain.  The representative is always
		 * kept inside the reported interval, even at a domain end. */
		pt.u0 = u - hu;
		pt.u1 = u + hu;
		pt.v0 = v - hv;
		pt.v1 = v + hv;
		if (pt.u0 < pa->u0) {
			pt.u0 = pa->u0;
			pt.u1 = ts_int_min(pa->u1, pa->u0 + ctx->utol);
		}
		if (pt.u1 > pa->u1) {
			pt.u1 = pa->u1;
			pt.u0 = ts_int_max(pa->u0, pa->u1 - ctx->utol);
		}
		if (pt.v0 < pb->u0) {
			pt.v0 = pb->u0;
			pt.v1 = ts_int_min(pb->u1, pb->u0 + ctx->vtol);
		}
		if (pt.v1 > pb->u1) {
			pt.v1 = pb->u1;
			pt.v0 = ts_int_max(pb->u0, pb->u1 - ctx->vtol);
		}
		if (u < pt.u0) pt.u0 = u;
		if (u > pt.u1) pt.u1 = u;
		if (v < pt.v0) pt.v0 = v;
		if (v > pt.v1) pt.v1 = v;
		pt.u = u;
		pt.v = v;
		pt.point[0] = ts_int_poly_eval(qm[0], da, lu);
		pt.point[1] = dim > 1 ?
		    ts_int_poly_eval(qm[1], da, lu) : 0;
		pt.point[2] = dim > 2 ?
		    ts_int_poly_eval(qm[2], da, lu) : 0;
		pt.ui = pa->ui;
		pt.vi = pb->ui;
		err = ts_int_point_push(&ctx->points, &pt);
		if (err) goto done;
	}
done:
	free(stack.items);
	return err;
}

/**
 * True if the whole leaf is covered by a certified overlap relation strip:
 * every corner satisfies the relation within the leaf tolerance.
 */
static int
ts_int_leaf_covered_by_relation(const TsContext *ctx,
                                const TsPiece *pa,
                                const TsPiece *pb,
                                const TsWorkBox *b)
{
	tsReal us[2], vs[2];
	int iu, iv;
	us[0] = b->lu0;
	us[1] = b->lu1;
	vs[0] = b->lv0;
	vs[1] = b->lv1;
	for (iu = 0; iu < 2; iu++)
		for (iv = 0; iv < 2; iv++) {			tsReal u = pa->u0 + us[iu] * (pa->u1 - pa->u0);
			tsReal v = pb->u0 + vs[iv] * (pb->u1 - pb->u0);
			if (!ts_int_on_relation(ctx, pa->ui, pb->ui, u, v,
			                        ctx->utol, ctx->vtol))
				return 0;
		}
	return 1;
}

/**
 * True when the whole box is inside the tolerance rectangle of an isolated
 * point already recorded for this interval pair.  Distinct isolated roots are
 * separated by far more than a tolerance on every supported input, so this
 * only absorbs the redundant leaves that cluster around one already found
 * root (fold-backs and high multiplicity contacts), which keeps the box
 * budget from being spent re-converging to the same parameters.
 */
static int
ts_int_box_covered_by_point(const TsContext *ctx,
                            const TsPiece *pa,
                            const TsPiece *pb,
                            const TsWorkBox *b)
{
	tsReal u0 = pa->u0 + b->lu0 * (pa->u1 - pa->u0);
	tsReal u1 = pa->u0 + b->lu1 * (pa->u1 - pa->u0);
	tsReal v0 = pb->u0 + b->lv0 * (pb->u1 - pb->u0);
	tsReal v1 = pb->u0 + b->lv1 * (pb->u1 - pb->u0);
	size_t i;
	for (i = 0; i < ctx->points.n; i++) {
		const tsIntersectionPoint *p = &ctx->points.items[i];
		if (p->ui != pa->ui || p->vi != pb->ui) continue;
		if (u0 >= p->u0 - ctx->utol && u1 <= p->u1 + ctx->utol &&
		    v0 >= p->v0 - ctx->vtol && v1 <= p->v1 + ctx->vtol)
			return 1;
	}
	return 0;
}

/**
 * Exact relation containment for an injective box.  When both restricted
 * arcs are strictly monotone inside the box, each parameter of the first
 * spline has at most one matching parameter on the second.  If a certified
 * affine relation is defined across the box's whole u-range and its image
 * lies inside the box's v-range, every solution the box can contain belongs
 * to that relation, so the box is fully covered and needs no isolated root
 * search.  An off-diagonal box fails the image containment because the
 * relation never enters its v-range.
 */
static int
ts_int_box_inside_relation(const TsContext *ctx,
                           const TsPiece *pa,
                           const TsPiece *pb,
                           const TsWorkBox *b)
{
	tsReal u0 = pa->u0 + b->lu0 * (pa->u1 - pa->u0);
	tsReal u1 = pa->u0 + b->lu1 * (pa->u1 - pa->u0);
	tsReal v0 = pb->u0 + b->lv0 * (pb->u1 - pb->u0);
	tsReal v1 = pb->u0 + b->lv1 * (pb->u1 - pb->u0);
	tsReal pad = ts_int_max(ctx->utol, ctx->vtol);
	size_t i;
	for (i = 0; i < ctx->relations.n; i++) {
		const TsRelation *r = &ctx->relations.items[i];
		tsReal f0, f1, flo, fhi;
		if (r->ui != pa->ui || r->vi != pb->ui) continue;
		if (u0 < r->u0 - pad || u1 > r->u1 + pad) continue;
		f0 = r->alpha * u0 + r->beta;
		f1 = r->alpha * u1 + r->beta;
		flo = ts_int_min(f0, f1);
		fhi = ts_int_max(f0, f1);
		if (flo >= v0 - pad && fhi <= v1 + pad)
			return 1;
	}
	return 0;
}

/* ======================================================================= *
 * Pair solving                                                             *
 * ======================================================================= */

/**
 * Piece level coincidence pre-pass.  Detects whether the two whole Bezier
 * pieces are algebraically identical under one affine parameter map and, if
 * so, emits the maximal coincident segment by clipping the map to the two
 * piece domains.  This catches coincidences that span only part of a piece,
 * which box corner fitting cannot discover.  Returns 1 when an overlap was
 * emitted, 0 otherwise.  Emission failure propagates through @p err.
 */
static int
ts_int_try_piece_coincidence(TsContext *ctx,
                             const TsPiece *pa,
                             const TsPiece *pb,
                             int da, int db, int dim,
                             tsError *err)
{
	tsReal scale = 1, slope, offset;
	TsLocalBox local;
	TsWorkBox whole;
	int k, c;

	for (k = 0; k <= da; k++)
		for (c = 0; c < dim; c++)
			scale = ts_int_max(scale,
			    ts_int_abs(pa->cp[k * dim + c]));
	for (k = 0; k <= db; k++)
		for (c = 0; c < dim; c++)
			scale = ts_int_max(scale,
			    ts_int_abs(pb->cp[k * dim + c]));

	if (ts_int_arc_is_constant(pa->cp, da, dim, scale) ||
	    ts_int_arc_is_constant(pb->cp, db, dim, scale))
		return 0;
	if (!ts_int_piece_affine_coincidence(pa, pb, da, db, dim,
	                                     scale, &local,
	                                     &slope, &offset))
		return 0;

	whole.lu0 = local.lu0;
	whole.lu1 = local.lu1;
	whole.lv0 = local.lv0;
	whole.lv1 = local.lv1;
	whole.ui = pa->ui;
	whole.vi = pb->ui;
	whole.monotone = 1;
	*err = ts_int_emit_overlap(ctx, pa, pb, &whole, slope, offset);
	return *err == TS_SUCCESS;
}

static tsError
ts_int_solve_pair(TsContext *ctx,
                  const TsPiece *pa,
                  const TsPiece *pb)
{
	TsWorkStack stack;
	tsError err = TS_SUCCESS;
	const int da = ctx->pa->deg;
	const int db = ctx->pb->deg;
	const int dim = ctx->pa->dim;
	TsWorkBox first;
	first.lu0 = 0;
	first.lu1 = 1;
	first.lv0 = 0;
	first.lv1 = 1;
	first.ui = pa->ui;
	first.vi = pb->ui;
	first.monotone = 0;

	/* Whole-piece coincidence (covers partial overlaps).  When found,
	 * the emitted relation strip prunes the coincident cells while the
	 * subdivision below still finds off-relation isolated roots. */
	{
		int emitted = ts_int_try_piece_coincidence(
			ctx, pa, pb, da, db, dim, &err);
		if (err) return err;
		(void) emitted;
	}

	stack.items = NULL;
	stack.n = stack.cap = 0;
	err = ts_int_stack_push(&stack, first);
	if (err) goto cleanup;

	while (stack.n > 0) {
		TsWorkBox b = stack.items[--stack.n];
		tsReal qbox[(TS_INTERSECT_MAX_DEG + 1) * 3];
		tsReal pbox[(TS_INTERSECT_MAX_DEG + 1) * 3];
		tsReal lo0[3], hi0[3], lo1[3], hi1[3];
		tsReal scale = 1, wu, wv;
		int k, c;

		ctx->expanded++;
		if (ctx->expanded > ctx->budget) {
			err = ts_int_emit_box(ctx, pa, pb, &b,
			                      TS_INTERSECT_REASON_BUDGET);
			if (err) goto cleanup;
			while (stack.n > 0) {
				TsWorkBox r = stack.items[--stack.n];
				err = ts_int_emit_box(ctx, pa, pb, &r,
				                      TS_INTERSECT_REASON_BUDGET);
				if (err) goto cleanup;
			}
			goto cleanup;
		}

		ts_int_arc_restrict(pa->cp, da, dim, b.lu0, b.lu1, qbox);
		ts_int_arc_restrict(pb->cp, db, dim, b.lv0, b.lv1, pbox);
		for (k = 0; k <= da; k++)
			for (c = 0; c < dim; c++)
				scale = ts_int_max(scale,
				    ts_int_abs(qbox[k * dim + c]));
		for (k = 0; k <= db; k++)
			for (c = 0; c < dim; c++)
				scale = ts_int_max(scale,
				    ts_int_abs(pbox[k * dim + c]));

		/* Conservative convex hull exclusion (cheap first filter). */
		ts_int_bernstein_aabb(qbox, da, dim, lo0, hi0);
		ts_int_bernstein_aabb(pbox, db, dim, lo1, hi1);
		if (ts_int_aabbs_disjoint(lo0, hi0, lo1, hi1, ctx->stol))
			continue;

		/* Tighter certified exclusion on the tensor product
		 * difference coefficients; this also rejects boxes whose
		 * closest approach is a non-zero local distance minimum. */
		if (ts_int_difference_excludes(qbox, pbox, da, db,
		                               dim, scale, ctx->stol))
			continue;

		wu = (b.lu1 - b.lu0) * (pa->u1 - pa->u0);
		wv = (b.lv1 - b.lv0) * (pb->u1 - pb->u0);

		/* Injectivity is preserved by restriction, so the flag is
		 * inherited; otherwise test the restricted arcs. */
		if (!b.monotone &&
		    ts_int_arc_strictly_monotone(qbox, da, dim, scale) &&
		    ts_int_arc_strictly_monotone(pbox, db, dim, scale))
			b.monotone = 1;

		/* Large scale pruning for an injective box that lies entirely
		 * inside a certified affine relation: such a box contains no
		 * isolated solution, so it is absorbed at the ancestor level and
		 * the coincident pair stays linear instead of being subdivided
		 * along the whole diagonal. */
		if (b.monotone &&
		    ts_int_box_inside_relation(ctx, pa, pb, &b))
			continue;

		/* Strip containment that also applies to folded (non-injective)
		 * arcs.  A rectangle whose four corners all lie within the narrow
		 * relation strip is covered by the certified overlap because the
		 * relation residual is linear in both parameters.  The strip is a
		 * few tolerances wide, while every genuine isolated root is
		 * separated from a relation by far more than that, so an
		 * off-relation intersection can never fall inside it.  Capping the
		 * diagonal band here keeps coincident, fold-back pairs linear in
		 * the box budget. */
		if (ts_int_leaf_covered_by_relation(ctx, pa, pb, &b))
			continue;

		/* Coincidence certification on non-constant arcs.  The identity
		 * test is gated on the geometric length of the restricted arcs as
		 * a fraction of their original pieces, rather than on the
		 * parameter span, so partial coincidences on non-uniform or
		 * reparameterized domains are still recognized.  A geometrically
		 * tiny sub-arc is excluded: there every smooth curve matches its
		 * affine approximation, which would turn an isolated contact into
		 * a spurious overlap. */
		if (!ts_int_arc_is_constant(qbox, da, dim, scale) &&
		    !ts_int_arc_is_constant(pbox, db, dim, scale)) {
			tsReal lq = ts_int_polygon_length(qbox, da, dim);
			tsReal lp = ts_int_polygon_length(pbox, db, dim);
			if (lq > 0 && lp > 0 &&
			    (b.lu1 - b.lu0) > 0 && (b.lv1 - b.lv0) > 0 &&
			    lq >= TS_INTERSECT_IDENTITY_MIN_FRAC * pa->full_len &&
			    lp >= TS_INTERSECT_IDENTITY_MIN_FRAC * pb->full_len) {
				tsReal sf = (b.lv1 - b.lv0) / (b.lu1 - b.lu0);
				tsReal of = b.lv0 - sf * b.lu0;
				tsReal sr = (b.lv0 - b.lv1) / (b.lu1 - b.lu0);
				tsReal orr = b.lv1 - sr * b.lu0;
				int fwd = ts_int_affine_identity(
				        qbox, pbox, da, db, dim, sf, of, scale);
				int rev = !fwd && ts_int_affine_identity(
				        qbox, pbox, da, db, dim, sr, orr, scale);
				if (fwd || rev) {
					err = ts_int_emit_overlap(
					    ctx, pa, pb, &b,
					    fwd ? sf : sr,
					    fwd ? of : orr);
					if (err) goto cleanup;
					/* A monotone box has no other solutions than
					 * the certified relation; non-monotone boxes
					 * (fold-backs) keep subdividing so off-diagonal
					 * isolated roots are still found. */
					if (b.monotone)
						continue;
				}
			}
		}

		/* Leaf sized box. */
		if (wu <= ctx->utol && wv <= ctx->vtol) {
			if (ts_int_leaf_covered_by_relation(ctx, pa, pb, &b))
				continue;
			/* Skip redundant leaves that only re-converge to a root
			 * already recorded for this interval pair. */
			if (ts_int_box_covered_by_point(ctx, pa, pb, &b))
				continue;
			err = ts_int_solve_leaf(ctx, pa, pb, &b,
			                        qbox, pbox, da, db, dim);
			if (err) goto cleanup;
			continue;
		}
		if (wu <= TS_INTERSECT_PREC_FLOOR ||
		    wv <= TS_INTERSECT_PREC_FLOOR) {
			err = ts_int_emit_box(ctx, pa, pb, &b,
			                      TS_INTERSECT_REASON_PRECISION);
			if (err) goto cleanup;
			continue;
		}

		/* Split the axis with the larger normalized width. */
		{
			tsReal nu = wu / ctx->utol;
			tsReal nv = wv / ctx->vtol;
			TsWorkBox c1 = b, c2 = b;
			if (nu >= nv) {
				tsReal m = (b.lu0 + b.lu1) / 2;
				c1.lu1 = m;
				c2.lu0 = m;
			} else {
				tsReal m = (b.lv0 + b.lv1) / 2;
				c1.lv1 = m;
				c2.lv0 = m;
			}
			err = ts_int_stack_push(&stack, c1);
			if (err) goto cleanup;
			err = ts_int_stack_push(&stack, c2);
			if (err) goto cleanup;
		}
	}
cleanup:
	free(stack.items);
	return err;
}

/* ======================================================================= *
 * Post processing                                                          *
 * ======================================================================= */

/**
 * Continuous-knot boundary rule: a point at the right end of a non-final
 * piece is owned by the piece on the right.  True breaks (knot multiplicity
 * equal to the order) keep both identities.  The right most domain end is
 * owned by the last piece and is never dropped.
 */
static void
ts_int_apply_boundary_rule(const TsPieces *pa,
                           const TsPieces *pb,
                           ts_int_pointArray *pts,
                           tsReal utol, tsReal vtol)
{
	size_t i, w = 0;
	for (i = 0; i < pts->n; i++) {
		tsIntersectionPoint *p = &pts->items[i];
		size_t pi;
		int drop = 0;
		for (pi = 0; pi + 1 < pa->n; pi++) {
			const TsPiece *piece = &pa->items[pi];
			if (p->ui == piece->ui &&
			    ts_int_abs(p->u - piece->u1) <= utol) {
				size_t m = ts_int_knot_multiplicity(
					pa->knots, pa->num_knots, piece->u1);
				if (m < (size_t) pa->deg + 1) drop = 1;
			}
		}
		for (pi = 0; pi + 1 < pb->n; pi++) {
			const TsPiece *piece = &pb->items[pi];
			if (p->vi == piece->ui &&
			    ts_int_abs(p->v - piece->u1) <= vtol) {
				size_t m = ts_int_knot_multiplicity(
					pb->knots, pb->num_knots, piece->u1);
				if (m < (size_t) pb->deg + 1) drop = 1;
			}
		}
		if (!drop) pts->items[w++] = *p;
	}
	pts->n = w;
}

static int
ts_int_point_compare(const void *x, const void *y)
{
	const tsIntersectionPoint *a = x;
	const tsIntersectionPoint *b = y;
	if (a->ui != b->ui)
		return a->ui < b->ui ? -1 : 1;
	if (a->vi != b->vi)
		return a->vi < b->vi ? -1 : 1;
	if (a->u < b->u) return -1;
	if (a->u > b->u) return 1;
	if (a->v < b->v) return -1;
	if (a->v > b->v) return 1;
	return 0;
}

static int
ts_int_overlap_compare(const void *x, const void *y)
{
	const tsIntersectionOverlap *a = x;
	const tsIntersectionOverlap *b = y;
	if (a->ui != b->ui)
		return a->ui < b->ui ? -1 : 1;
	if (a->vi != b->vi)
		return a->vi < b->vi ? -1 : 1;
	if (a->u_from < b->u_from) return -1;
	if (a->u_from > b->u_from) return 1;
	return 0;
}

static int
ts_int_box_compare(const void *x, const void *y)
{
	const tsIntersectionBox *a = x;
	const tsIntersectionBox *b = y;
	if (a->ui != b->ui)
		return a->ui < b->ui ? -1 : 1;
	if (a->vi != b->vi)
		return a->vi < b->vi ? -1 : 1;
	if (a->u0 < b->u0) return -1;
	if (a->u0 > b->u0) return 1;
	return 0;
}

/**
 * Classifies a point against every certified relation on its interval pair
 * by the sign of its relation residual.  Points lying on the relation are
 * removed at emission time, so every point reaching this stage has a nonzero
 * residual and the sign is well defined.  This fully separates the two
 * orientations of a self-intersection: the swapped parameter pairs sit on
 * opposite sides of the diagonal relation, so they can never be chained into
 * one cluster even though they are close in both axes.  Returns zero only
 * when no relation applies to the interval pair.
 */
static int
ts_int_relation_side(const TsContext *ctx,
                     const tsIntersectionPoint *p)
{
	size_t i;
	for (i = 0; i < ctx->relations.n; i++) {
		const TsRelation *r = &ctx->relations.items[i];
		tsReal d;
		if (r->ui != p->ui || r->vi != p->vi) continue;
		d = p->v - (r->alpha * p->u + r->beta);
		return d > 0 ? 1 : -1;
	}
	return 0;
}

/**
 * Dedups isolated points by parameter pair using orientation aware connected
 * component clustering.  After sorting, adjacent reports on the same
 * interval pair and the same side of every certified relation merge when
 * both parameter axes agree within the merge band; the band is chained
 * through the cluster so every noisy representative of one root collapses.
 * The orientation key breaks the chain between the two swapped orientations
 * of a self-intersection, and distinct roots - separated by far more than
 * the band on every supported input - never merge.  The same spatial point
 * reached through a distinct parameter pair is therefore retained.
 */
static void
ts_int_dedup_points(TsContext *ctx,
                    ts_int_pointArray *pts,
                    tsReal utol, tsReal vtol)
{
	const tsReal uband = (tsReal) 16.0 * utol;
	const tsReal vband = (tsReal) 16.0 * vtol;
	size_t i, w = 0;
	if (pts->n == 0) return;
	qsort(pts->items, pts->n, sizeof(tsIntersectionPoint),
	      ts_int_point_compare);
	for (i = 0; i < pts->n; i++) {
		int side = ts_int_relation_side(ctx, &pts->items[i]);
		if (w > 0) {
			tsIntersectionPoint *top = &pts->items[w - 1];
			int top_side = ts_int_relation_side(ctx, top);
			if (top->ui == pts->items[i].ui &&
			    top->vi == pts->items[i].vi &&
			    top_side == side &&
			    ts_int_abs(pts->items[i].u - top->u) <= uband &&
			    ts_int_abs(pts->items[i].v - top->v) <= vband) {
				/* Same root: retain the first report, whose
				 * representative already lies inside its own
				 * tolerance sized interval.  The retained interval
				 * keeps its required width bound. */
				continue;
			}
		}
		pts->items[w++] = pts->items[i];
	}
	pts->n = w;
}

/**
 * Merges overlap records that describe the same affine relation on the same
 * pair of original intervals, producing the maximal parameter segment per
 * pair and orientation.
 */
static void
ts_int_merge_overlaps(ts_int_overlapArray *ovs)
{
	size_t i, w = 0;
	if (ovs->n == 0) return;
	qsort(ovs->items, ovs->n, sizeof(tsIntersectionOverlap),
	      ts_int_overlap_compare);
	for (i = 0; i < ovs->n; i++) {
		tsIntersectionOverlap *cur = &ovs->items[i];
		if (w > 0) {
			tsIntersectionOverlap *top = &ovs->items[w - 1];
			tsReal eps = (tsReal) 1e-10 * ts_int_max(
				(tsReal) 1.0, ts_int_abs(cur->u_to));
			int same_pair = top->ui == cur->ui &&
			                top->vi == cur->vi;
			int same_dir = (top->alpha >= 0) ==
			               (cur->alpha >= 0);
			int same_map = ts_int_abs(top->alpha -
			                          cur->alpha) <=
			    (tsReal) 1e-8 * ts_int_max((tsReal) 1.0,
			                               ts_int_abs(top->alpha)) &&
			    ts_int_abs(top->beta - cur->beta) <=
			    (tsReal) 1e-8 * ts_int_max((tsReal) 1.0,
			                               ts_int_abs(top->beta));
			int touching = cur->u_from <= top->u_to + eps;
			if (same_pair && same_dir && same_map && touching) {
				if (cur->u_to > top->u_to) {
					top->u_to = cur->u_to;
					top->v_to = top->alpha * top->u_to +
					            top->beta;
				}
				if (cur->u_from < top->u_from) {
					top->u_from = cur->u_from;
					top->v_from = top->alpha * top->u_from +
					              top->beta;
				}
				continue;
			}
		}
		ovs->items[w++] = *cur;
	}
	ovs->n = w;
}

/* ======================================================================= *
 * Public API                                                               *
 * ======================================================================= */

tsIntersection
ts_intersection_init(void)
{
	tsIntersection r;
	r.points = NULL;
	r.num_points = 0;
	r.overlaps = NULL;
	r.num_overlaps = 0;
	r.boxes = NULL;
	r.num_boxes = 0;
	r.complete = 0;
	r.boxes_expanded = 0;
	return r;
}

tsError
ts_intersection_copy(const tsIntersection *src,
                     tsIntersection *dest,
                     tsStatus *status)
{
	tsIntersection tmp;
	tsError err = TS_SUCCESS;

	TS_TRY(try, err, status)
		if (!src || !dest)
			TS_THROW_0(try, err, status, TS_INDEX_ERROR,
			           "intersection copy: NULL argument")
		tmp = ts_intersection_init();
		if (src->num_points > 0) {
			tmp.points = (tsIntersectionPoint *) malloc(
				src->num_points * sizeof(tsIntersectionPoint));
			if (!tmp.points)
				TS_THROW_0(try, err, status, TS_MALLOC,
				           "out of memory")
			memcpy(tmp.points, src->points,
			       src->num_points * sizeof(tsIntersectionPoint));
			tmp.num_points = src->num_points;
		}
		if (src->num_overlaps > 0) {
			tmp.overlaps = (tsIntersectionOverlap *) malloc(
				src->num_overlaps *
				sizeof(tsIntersectionOverlap));
			if (!tmp.overlaps)
				TS_THROW_0(try, err, status, TS_MALLOC,
				           "out of memory")
			memcpy(tmp.overlaps, src->overlaps,
			       src->num_overlaps *
			       sizeof(tsIntersectionOverlap));
			tmp.num_overlaps = src->num_overlaps;
		}
		if (src->num_boxes > 0) {
			tmp.boxes = (tsIntersectionBox *) malloc(
				src->num_boxes * sizeof(tsIntersectionBox));
			if (!tmp.boxes)
				TS_THROW_0(try, err, status, TS_MALLOC,
				           "out of memory")
			memcpy(tmp.boxes, src->boxes,
			       src->num_boxes * sizeof(tsIntersectionBox));
			tmp.num_boxes = src->num_boxes;
		}
		tmp.complete = src->complete;
		tmp.boxes_expanded = src->boxes_expanded;

		ts_intersection_free(dest);
		*dest = tmp;
	TS_FINALLY
		if (err) {
			ts_intersection_free(&tmp);
		}
	TS_END_TRY_RETURN(err)
}

void
ts_intersection_free(tsIntersection *result)
{
	if (!result) return;
	free(result->points);
	free(result->overlaps);
	free(result->boxes);
	*result = ts_intersection_init();
}

/** Validates one spline against the supported input contract. */
static tsError
ts_int_validate_spline(const tsBSpline *s,
                       const char *which,
                       int *dim_out,
                       tsStatus *status)
{
	size_t deg, dim, nctrl, nknots, i;
	const tsReal *cp, *knots;

	if (!s || !s->pImpl)
		TS_RETURN_1(status, TS_INDEX_ERROR,
		            "%s spline: NULL argument", which)
	deg = ts_bspline_degree(s);
	dim = ts_bspline_dimension(s);
	nctrl = ts_bspline_num_control_points(s);
	nknots = ts_bspline_num_knots(s);
	cp = ts_bspline_control_points_ptr(s);
	knots = ts_bspline_knots_ptr(s);
	(void) nctrl;

	if (deg < 1 || deg > 3)
		TS_RETURN_0(status, TS_DEG_GE_NCTRLP,
		            "unsupported degree: only 1..3 are supported; "
		            "constant splines are rejected")
	if (dim != 2 && dim != 3)
		TS_RETURN_0(status, TS_DIM_ZERO,
		            "unsupported dimension: only 2 or 3 are supported")

	/* Clamped: end knots must have full order multiplicity. */
	{
		tsReal kep = TS_INTERSECT_PREC_FLOOR * ts_int_max(
			(tsReal) 1.0,
			ts_int_max(ts_int_abs(knots[0]),
			           ts_int_abs(knots[nknots - 1])));
		for (i = 0; i <= deg; i++)
			if (ts_int_abs(knots[i] - knots[0]) > kep ||
			    ts_int_abs(knots[nknots - 1 - i] -
			                knots[nknots - 1]) > kep)
				TS_RETURN_0(status, TS_NO_RESULT,
				            "spline is not clamped")
	}

	for (i = 0; i < nctrl * dim; i++) {
		if (!ts_int_finite(cp[i]))
			TS_RETURN_0(status, TS_NO_RESULT,
			            "control point is not finite")
		if (ts_int_abs(cp[i]) > 100)
			TS_RETURN_0(status, TS_NO_RESULT,
			            "control point exceeds coordinate bound 100")
	}
	for (i = 0; i < nknots; i++) {
		if (!ts_int_finite(knots[i]))
			TS_RETURN_0(status, TS_NO_RESULT,
			            "knot is not finite")
		if (ts_int_abs(knots[i]) > 100)
			TS_RETURN_0(status, TS_NO_RESULT,
			            "knot exceeds knot bound 100")
	}
	for (i = 1; i < nknots; i++) {
		tsReal d = knots[i] - knots[i - 1];
		if (d > 0 && d < (tsReal) 1e-4)
			TS_RETURN_0(status, TS_NO_RESULT,
			            "non-zero knot interval is smaller than 1e-4")
	}
	*dim_out = (int) dim;
	return TS_SUCCESS;
}

tsError
ts_bspline_intersect(const tsBSpline *a,
                     const tsBSpline *b,
                     tsReal stol,
                     tsReal utol,
                     tsReal vtol,
                     size_t budget,
                     tsIntersection *result,
                     tsStatus *status)
{
	tsError err;
	TsPieces pa, pb;
	TsContext ctx;
	tsIntersection out;
	tsReal tol;
	int dima = 0, dimb = 0;
	size_t i, j;

	ts_int_pieces_init(&pa);
	ts_int_pieces_init(&pb);
	ctx.pa = ctx.pb = NULL;
	ctx.points.items = NULL;
	ctx.points.n = ctx.points.cap = 0;
	ctx.overlaps.items = NULL;
	ctx.overlaps.n = ctx.overlaps.cap = 0;
	ctx.boxes.items = NULL;
	ctx.boxes.n = ctx.boxes.cap = 0;
	ctx.relations.items = NULL;
	ctx.relations.n = ctx.relations.cap = 0;
	out = ts_intersection_init();

	TS_TRY(try, err, status)
		if (!result)
			TS_THROW_0(try, err, status, TS_INDEX_ERROR,
			           "result must not be NULL")
		if (!ts_int_finite(stol) || !ts_int_finite(utol) ||
		    !ts_int_finite(vtol))
			TS_THROW_0(try, err, status, TS_NO_RESULT,
			           "tolerance must be finite")
		tol = TS_INTERSECT_TOL_MIN;
		if (stol < tol || stol > TS_INTERSECT_TOL_MAX ||
		    utol < tol || utol > TS_INTERSECT_TOL_MAX ||
		    vtol < tol || vtol > TS_INTERSECT_TOL_MAX)
			TS_THROW_0(try, err, status, TS_NO_RESULT,
			           "tolerances must be in [1e-8, 1e-4]")
		if (budget == 0)
			budget = TS_INTERSECT_DEFAULT_BUDGET;

		TS_CALL(try, err, ts_int_validate_spline(
		        a, "first", &dima, status))
		TS_CALL(try, err, ts_int_validate_spline(
		        b, "second", &dimb, status))
		if (dima != dimb)
			TS_THROW_0(try, err, status, TS_DIM_ZERO,
			           "splines must have the same dimensionality")

		TS_CALL(try, err, ts_int_pieces_build(a, &pa, status))
		TS_CALL(try, err, ts_int_pieces_build(b, &pb, status))
		if (pa.n == 0 || pb.n == 0)
			TS_THROW_0(try, err, status, TS_NO_RESULT,
			           "spline has no non-zero knot interval")

		ctx.pa = &pa;
		ctx.pb = &pb;
		ctx.sa = a;
		ctx.sb = b;
		ctx.stol = stol;
		ctx.utol = utol;
		ctx.vtol = vtol;
		ctx.budget = budget;
		ctx.expanded = 0;
		ctx.incomplete = 0;
		ctx.reason = TS_INTERSECT_REASON_NONE;

		for (i = 0; i < pa.n; i++)
			for (j = 0; j < pb.n; j++) {
				TS_CALL(try, err, ts_int_solve_pair(
				        &ctx, &pa.items[i], &pb.items[j]))
			}

		ts_int_apply_boundary_rule(&pa, &pb, &ctx.points,
		                           utol, vtol);
		ts_int_dedup_points(&ctx, &ctx.points, utol, vtol);
		ts_int_merge_overlaps(&ctx.overlaps);
		if (ctx.boxes.n > 0)
			qsort(ctx.boxes.items, ctx.boxes.n,
			      sizeof(tsIntersectionBox), ts_int_box_compare);

		out.points = ctx.points.items;
		out.num_points = ctx.points.n;
		out.overlaps = ctx.overlaps.items;
		out.num_overlaps = ctx.overlaps.n;
		out.boxes = ctx.boxes.items;
		out.num_boxes = ctx.boxes.n;
		out.complete = ctx.incomplete ? 0 : 1;
		out.boxes_expanded = ctx.expanded;

		/* Ownership transferred; detach context arrays. */
		ctx.points.items = NULL;
		ctx.points.n = ctx.points.cap = 0;
		ctx.overlaps.items = NULL;
		ctx.overlaps.n = ctx.overlaps.cap = 0;
		ctx.boxes.items = NULL;
		ctx.boxes.n = ctx.boxes.cap = 0;

		/* Success or incomplete: replace the caller's result. */
		ts_intersection_free(result);
		*result = out;
		out = ts_intersection_init();
	TS_FINALLY
		free(ctx.points.items);
		free(ctx.overlaps.items);
		free(ctx.boxes.items);
		free(ctx.relations.items);
		ts_int_pieces_free(&pa);
		ts_int_pieces_free(&pb);
		ts_intersection_free(&out);
	TS_END_TRY_RETURN(err)
}
