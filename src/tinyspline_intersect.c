/** @file
 *
 * @brief
 * 	Implementation of ts_bspline_intersect. See tinyspline_intersect.h for
 * 	the public contract.
 *
 * The algorithm works on the B\xC3\xA9zier decomposition of the inputs:
 *
 *  - Every non-zero knot interval of an input spline becomes one polynomial
 *    B\xC3\xA9zier piece whose control points are copied. Pieces remember the
 *    original knot interval and its lower knot index, so every result can be
 *    reported on the original parameter axes.
 *  - Piece pairs are explored with a stack of parameter boxes (branch and
 *    bound). A box is excluded only when the component-wise bounding boxes of
 *    the two restricted polynomial arcs, enlarged by the spatial tolerance and
 *    a floating point safety margin, are disjoint. B\xC3\xA9zier control points
 *    bound the curve (convex hull property), so this exclusion is
 *    conservative.
 *  - Coincidence is decided by polynomial identity under an affine parameter
 *    correspondence: the restricted arcs are converted to the power basis,
 *    the second polynomial is composed with the affine map and the resulting
 *    coefficients are compared component by component. Endpoints, midpoints or
 *    tangents alone are never accepted as proof.
 *  - Surviving boxes are recursively split. At parameter-tolerance width, an
 *    interior damped Gauss-Newton solve must converge to a residual within the
 *    spatial tolerance before an isolated intersection is reported; a mere
 *    small distance is never accepted as proof.
 *  - Boxes that cannot be resolved when the budget is exhausted or when
 *    floating point precision prevents further subdivision are returned as
 *    unresolved boxes.
 */

#define TINYSPLINE_EXPORT
#include "tinyspline_intersect.h"

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>



/*! @name Internal Constants
 * @{ */
/** Smallest admissible tolerance. */
#define TS_I_EPS_MIN 1e-8

/** Largest admissible tolerance. */
#define TS_I_EPS_MAX 1e-4

/** Guaranteed bounds on input coordinates and knots. */
#define TS_I_COORD_MAX 100.0
#define TS_I_KNOT_MAX 100.0
#define TS_I_MIN_SPAN 1e-4

/**
 * Absolute safety margin for conservative tests. It is an order of magnitude
 * below the smallest admissible tolerance but far above the rounding errors
 * attainable on the bounded input domains (coordinates and knots are at most
 * 100 in magnitude).
 */
#define TS_I_NUM_MARGIN 1e-9

/** Relative subdivision limit: boxes narrower than this cannot be split. */
#define TS_I_TERMINAL 1e-13

/**
 * Minimum fraction of a piece's full control-point spread that a
 * coincidence candidate arc must cover. Below this ratio the restricted
 * arcs are near-constant and a coefficient identity is numerically
 * meaningless (a tangency sliver would otherwise look like an overlap).
 */
#define TS_I_OVERLAP_MIN_RATIO 1e-3

/** Maximum number of damped Gauss-Newton iterations per terminal box. */
#define TS_I_MAX_NEWTON 40

/**
 * Maximum strip-normal subdivision depth. Each strip split halves a box,
 * so after this many subdivisions every remaining straddling box has
 * parameter width at most domain/2^depth, which on the supported domains and
 * tolerances is well below the 16-tolerance corridor: it lies on the strip
 * and cannot contain a distinct root. Capping the depth here bounds the
 * strip work to O(2^depth) instead of O(1/param_eps).
 */
#define TS_I_STRIP_MAX_DEPTH 20

/** Number of Newton starts when locating the affine correspondence. */
#define TS_I_MAP_STARTS 9

/**
 * Residual below which a minimum is treated as an exact root. The threshold
 * is relative to the magnitude of the evaluated point: on the bounded input
 * domains (coordinates at most 100) evaluation rounding stays several orders
 * of magnitude below it, while a merely tolerance-close approach of two
 * non-intersecting curves stays above it.
 */
#define TS_I_ROOT_REL 1e-12
#define TS_I_ROOT_ABS 1e-13
/*! @} */



/*! @name Internal Data Structures
 * @{ */
/**
 * One polynomial piece of a decomposed curve. The control points are owned
 * copies; the input spline is not borrowed.
 */
typedef struct
{
	/** Lower index of the original non-zero knot interval. */
	size_t ia;

	/** Original parameter at the left end of the piece. */
	tsReal u0;

	/** Original parameter at the right end of the piece. */
	tsReal u1;

	/** Bernstein control points in world coordinates (index 0..degree). */
	tsReal P[4][3];
} tsIBezierPiece;

/** A decomposed input curve. All memory is owned by this structure. */
typedef struct
{
	size_t deg;             /**< Degree of every piece. */
	size_t dim;             /**< Dimension of the control points (2 or 3). */
	size_t num_pieces;      /**< Number of non-zero knot intervals. */
	tsReal umin;            /**< Domain minimum. */
	tsReal umax;            /**< Domain maximum. */
	tsIBezierPiece *pieces; /**< One entry per non-zero knot interval. */
	tsBSpline beziers;      /**< Owned B\xC3\xA9zier decomposition. */

	/**
	 * cont[i] is non-zero when the joint between pieces i and i+1 is
	 * continuous within the spatial tolerance. Has num_pieces - 1 entries.
	 */
	int *cont;
} tsICurve;

/** A working parameter box on the stack. */
typedef struct
{
	tsReal u0, u1; /**< First axis bounds, original parameters. */
	tsReal v0, v1; /**< Second axis bounds, original parameters. */
	size_t ia;     /**< Original interval index of the first curve. */
	size_t ib;     /**< Original interval index of the second curve. */
	size_t depth;  /**< Subdivision depth from the original seed box. */
} tsIBox;

/** A stack of working boxes. */
typedef struct
{
	tsIBox *data; /**< Owned box array. */
	size_t num;   /**< Number of boxes currently on the stack. */
	size_t cap;   /**< Allocated capacity. */
} tsIStack;

/** Mutable working state of one query. */
typedef struct
{
	const tsICurve *A;          /**< First decomposed curve. */
	const tsICurve *B;          /**< Second decomposed curve. */
	tsReal spatial_eps;         /**< Spatial tolerance. */
	tsReal param_eps_a;         /**< Parameter tolerance, first axis. */
	tsReal param_eps_b;         /**< Parameter tolerance, second axis. */
	size_t budget;              /**< Maximum number of expansions. */
	size_t expanded;            /**< Expansions performed so far. */
	int budget_exceeded;        /**< Non-zero once the budget is spent. */
	tsIStack stack;             /**< Pending boxes. */
	tsIntersectionResult *out;  /**< Output being built. */
	size_t cap_iso;             /**< Allocated capacity of isolated. */
	size_t cap_ov;              /**< Allocated capacity of overlaps. */
	size_t cap_box;             /**< Allocated capacity of boxes. */
} tsIWork;
/*! @} */



/*! @name Small Numerical Helpers
 * @{ */
static int
ts_i_isfinite(tsReal x)
{
	return x == x && x > -1e300 && x < 1e300;
}

static tsReal
ts_i_abs(tsReal x)
{
	return x < 0 ? -x : x;
}

static tsReal
ts_i_min(tsReal x, tsReal y)
{
	return x < y ? x : y;
}

static tsReal
ts_i_max(tsReal x, tsReal y)
{
	return x > y ? x : y;
}

/** Squared Euclidean distance of two points in up to three dimensions. */
static tsReal
ts_i_dist2(const tsReal *x, const tsReal *y, size_t dim)
{
	tsReal s = 0, d;
	size_t k;
	for (k = 0; k < dim; ++k) {
		d = x[k] - y[k];
		s += d * d;
	}
	return s;
}

/**
 * Evaluates a Bernstein polynomial of degree \p deg (at most 3) at \p t using
 * de Casteljau's algorithm. \p P holds deg+1 control points of three
 * components each.
 */
static void
ts_i_bez_eval(const tsReal P[4][3], size_t deg, tsReal t, tsReal out[3])
{
	tsReal tmp[4][3];
	size_t i, k, d;
	for (i = 0; i <= deg; ++i)
		for (d = 0; d < 3; ++d)
			tmp[i][d] = P[i][d];
	for (k = 0; k < deg; ++k)
		for (i = 0; i + k < deg; ++i)
			for (d = 0; d < 3; ++d)
				tmp[i][d] = (1 - t) * tmp[i][d] + t * tmp[i + 1][d];
	for (d = 0; d < 3; ++d)
		out[d] = tmp[0][d];
}

/** First derivative of a Bernstein polynomial evaluated at \p t. */
static void
ts_i_bez_deriv(const tsReal P[4][3], size_t deg, tsReal t, tsReal out[3])
{
	tsReal Q[4][3];
	size_t k, d;
	if (deg == 0) {
		for (d = 0; d < 3; ++d) out[d] = 0;
		return;
	}
	for (k = 0; k < deg; ++k)
		for (d = 0; d < 3; ++d)
			Q[k][d] = (tsReal) deg * (P[k + 1][d] - P[k][d]);
	ts_i_bez_eval(Q, deg - 1, t, out);
}

/** Splits a Bernstein polynomial at \p t into a left and right polynomial. */
static void
ts_i_bez_split(const tsReal P[4][3], size_t deg, tsReal t,
               tsReal left[4][3], tsReal right[4][3])
{
	tsReal tmp[4][3];
	size_t i, k, d;
	for (i = 0; i <= deg; ++i)
		for (d = 0; d < 3; ++d)
			tmp[i][d] = P[i][d];
	for (d = 0; d < 3; ++d) {
		left[0][d] = tmp[0][d];
		right[deg][d] = tmp[deg][d];
	}
	for (k = 0; k < deg; ++k) {
		for (i = 0; i + k < deg; ++i)
			for (d = 0; d < 3; ++d)
				tmp[i][d] = (1 - t) * tmp[i][d] + t * tmp[i + 1][d];
		for (d = 0; d < 3; ++d) {
			left[k + 1][d] = tmp[0][d];
			right[deg - k - 1][d] = tmp[deg - k - 1][d];
		}
	}
}

/**
 * Restricts a Bernstein polynomial to the local parameter interval
 * [t0, t1]. The result is the exact same polynomial arc expressed over the
 * local parameter [0, 1].
 */
static void
ts_i_bez_sub(const tsReal P[4][3], size_t deg,
             tsReal t0, tsReal t1, tsReal out[4][3])
{
	tsReal left[4][3], right[4][3], tmp[4][3];
	tsReal s;
	size_t i, d;
	if (t0 <= 0) {
		for (i = 0; i <= deg; ++i)
			for (d = 0; d < 3; ++d)
				tmp[i][d] = P[i][d];
	} else {
		ts_i_bez_split(P, deg, t0, left, right);
		for (i = 0; i <= deg; ++i)
			for (d = 0; d < 3; ++d)
				tmp[i][d] = right[i][d];
	}
	if (t1 >= 1) {
		for (i = 0; i <= deg; ++i)
			for (d = 0; d < 3; ++d)
				out[i][d] = tmp[i][d];
		return;
	}
	s = (t1 - t0) / (1 - t0);
	ts_i_bez_split(tmp, deg, s, left, right);
	for (i = 0; i <= deg; ++i)
		for (d = 0; d < 3; ++d)
			out[i][d] = left[i][d];
}

/** Component-wise bounding box of a Bernstein polynomial's control points. */
static void
ts_i_bez_bbox(const tsReal P[4][3], size_t deg, size_t dim,
              tsReal lo[3], tsReal hi[3])
{
	size_t i, d;
	for (d = 0; d < 3; ++d) {
		lo[d] = P[0][d];
		hi[d] = P[0][d];
	}
	for (i = 1; i <= deg; ++i)
		for (d = 0; d < dim; ++d) {
			lo[d] = ts_i_min(lo[d], P[i][d]);
			hi[d] = ts_i_max(hi[d], P[i][d]);
		}
}

/** Binomial coefficient for n, k at most 3. */
static size_t
ts_i_choose(size_t n, size_t k)
{
	static const size_t c[4][4] = {
		{1, 0, 0, 0}, {1, 1, 0, 0}, {1, 2, 1, 0}, {1, 3, 3, 1}
	};
	return c[n][k];
}

/**
 * Converts one component of a Bernstein polynomial of degree \p deg to power
 * basis coefficients c[0..deg] such that p(t) = Σ c[k] t^k.
 */
static void
ts_i_bern_to_power(const tsReal P[4][3], size_t deg, size_t comp,
                   tsReal c[4])
{
	size_t k, i;
	for (k = 0; k <= deg; ++k) {
		tsReal s = 0;
		for (i = 0; i <= k; ++i) {
			tsReal term = (tsReal) ts_i_choose(deg, i) *
			              (tsReal) ts_i_choose(deg - i, k - i) *
			              P[i][comp];
			if ((k - i) % 2)
				s -= term;
			else
				s += term;
		}
		c[k] = s;
	}
}
/*! @} */



/*! @name Curve Decomposition and Validation
 * @{ */
static void
ts_i_curve_init(tsICurve *c)
{
	c->deg = 0;
	c->dim = 0;
	c->num_pieces = 0;
	c->umin = 0;
	c->umax = 0;
	c->pieces = NULL;
	c->beziers = ts_bspline_init();
	c->cont = NULL;
}

static void
ts_i_curve_free(tsICurve *c)
{
	if (c->pieces) free(c->pieces);
	if (c->cont) free(c->cont);
	ts_bspline_free(&c->beziers);
	ts_i_curve_init(c);
}

/**
 * Validates \p spline and decomposes it into \p curve. Returns a TinySpline
 * error code. The validation enforces the contract of ts_bspline_intersect.
 */
static tsError
ts_i_curve_build(const tsBSpline *spline, tsICurve *curve,
                 tsReal spatial_eps, tsStatus *status)
{
	size_t deg, dim, nctrl, nknots, i, j, d, k, nspans;
	tsReal *knots = NULL;
	tsReal *ctrl = NULL;
	const tsReal *bctrl;
	tsError err;

	if (!spline || !spline->pImpl)
		TS_RETURN_0(status, TS_INDEX_ERROR, "null spline")
	deg = ts_bspline_degree(spline);
	dim = ts_bspline_dimension(spline);
	nctrl = ts_bspline_num_control_points(spline);
	nknots = ts_bspline_num_knots(spline);
	(void) nctrl;

	if (deg < 1 || deg > 3)
		TS_RETURN_0(status, TS_DEG_GE_NCTRLP,
		            "intersection requires degree 1 to 3")
	if (dim != 2 && dim != 3)
		TS_RETURN_0(status, TS_DIM_ZERO,
		            "intersection requires dimension 2 or 3")

	TS_CALL_ROE(err, ts_bspline_knots(spline, &knots, status))
	TS_CALL_ROE(err, ts_bspline_control_points(spline, &ctrl, status))

	/* Clamped ends: the first and last `order` knots must coincide. */
	for (i = 0; i <= deg; ++i)
		if (ts_i_abs(knots[i] - knots[0]) > TS_I_NUM_MARGIN ||
		    ts_i_abs(knots[nknots - 1 - i] -
		             knots[nknots - 1]) > TS_I_NUM_MARGIN)
			goto bad_input;
	for (i = 0; i < nknots; ++i) {
		if (!ts_i_isfinite(knots[i]) ||
		    ts_i_abs(knots[i]) > TS_I_KNOT_MAX)
			goto bad_input;
		if (i > 0 && knots[i] < knots[i - 1])
			goto bad_input;
	}
	for (i = 0; i + 1 < nknots; ++i) {
		if (knots[i + 1] > knots[i] &&
		    knots[i + 1] - knots[i] < TS_I_MIN_SPAN)
			goto bad_input;
	}
	for (i = 0; i < ts_bspline_len_control_points(spline); ++i) {
		if (!ts_i_isfinite(ctrl[i]) ||
		    ts_i_abs(ctrl[i]) > TS_I_COORD_MAX)
			goto bad_input;
	}

	/* Reject entirely constant curves: every control point identical. */
	{
		int constant = 1;
		for (i = 1; i < nctrl && constant; ++i)
			for (d = 0; d < dim; ++d)
				if (ts_i_abs(ctrl[i * dim + d] -
				             ctrl[d]) > TS_I_NUM_MARGIN) {
					constant = 0;
					break;
				}
		if (constant) {
			free(knots);
			free(ctrl);
			TS_RETURN_0(status, TS_NO_RESULT,
			            "constant curves cannot be intersected")
		}
	}

	err = ts_bspline_to_beziers(spline, &curve->beziers, status);
	if (err) {
		free(knots);
		free(ctrl);
		return err;
	}

	/* Count the non-zero knot spans and remember their lower indices. */
	nspans = 0;
	for (k = 0; k + 1 < nknots; ++k)
		if (knots[k + 1] > knots[k])
			++nspans;
	if (nspans == 0 ||
	    ts_bspline_num_control_points(&curve->beziers) !=
	        nspans * (deg + 1)) {
		free(knots);
		free(ctrl);
		TS_RETURN_0(status, TS_NO_RESULT,
		            "bezier decomposition does not match knot spans")
	}

	curve->deg = deg;
	curve->dim = dim;
	curve->num_pieces = nspans;
	curve->umin = knots[0];
	curve->umax = knots[nknots - 1];
	curve->pieces = (tsIBezierPiece *) calloc(nspans,
	                                          sizeof(tsIBezierPiece));
	curve->cont = (int *) calloc(nspans > 1 ? nspans - 1 : 1,
	                             sizeof(int));
	if (!curve->pieces || !curve->cont) {
		free(knots);
		free(ctrl);
		ts_i_curve_free(curve);
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	}

	bctrl = ts_bspline_control_points_ptr(&curve->beziers);
	j = 0;
	for (k = 0; k + 1 < nknots; ++k) {
		if (!(knots[k + 1] > knots[k]))
			continue;
		curve->pieces[j].ia = k;
		curve->pieces[j].u0 = knots[k];
		curve->pieces[j].u1 = knots[k + 1];
		for (i = 0; i <= deg; ++i)
			for (d = 0; d < 3; ++d)
				curve->pieces[j].P[i][d] =
				    d < dim ? bctrl[(j * (deg + 1) + i) * dim + d]
				            : (tsReal) 0;
		++j;
	}
	free(knots);
	free(ctrl);

	/* Continuity table for the right-interval assignment rule. */
	for (j = 0; j + 1 < nspans; ++j) {
		tsReal d2 = ts_i_dist2(curve->pieces[j].P[deg],
		                       curve->pieces[j + 1].P[0], dim);
		curve->cont[j] = d2 <= spatial_eps * spatial_eps;
	}
	return TS_SUCCESS;

bad_input:
	free(knots);
	free(ctrl);
	TS_RETURN_0(status, TS_NO_RESULT, "unsupported spline configuration")
}
/*! @} */



/*! @name Box Stack
 * @{ */
static void
ts_i_stack_init(tsIStack *s)
{
	s->data = NULL;
	s->num = 0;
	s->cap = 0;
}

static void
ts_i_stack_free(tsIStack *s)
{
	if (s->data) free(s->data);
	ts_i_stack_init(s);
}

static tsError
ts_i_stack_push(tsIStack *s, const tsIBox *b, tsStatus *status)
{
	if (s->num == s->cap) {
		size_t ncap = s->cap ? s->cap * 2 : 64;
		tsIBox *nd = (tsIBox *) realloc(s->data,
		                                ncap * sizeof(tsIBox));
		if (!nd) TS_RETURN_0(status, TS_MALLOC, "out of memory")
		s->data = nd;
		s->cap = ncap;
	}
	s->data[s->num++] = *b;
	return TS_SUCCESS;
}
/*! @} */



/*! @name Result Accumulation
 * @{ */
static tsError
ts_i_ensure(void **ptr, size_t *cap, size_t elem_size, size_t need,
            tsStatus *status)
{
	if (need <= *cap)
		return TS_SUCCESS;
	{
		size_t ncap = *cap ? *cap * 2 : 16;
		void *nd;
		while (ncap < need)
			ncap *= 2;
		nd = realloc(*ptr, ncap * elem_size);
		if (!nd) TS_RETURN_0(status, TS_MALLOC, "out of memory")
		*ptr = nd;
		*cap = ncap;
	}
	return TS_SUCCESS;
}

/**
 * Reassigns an interval index at a continuous joint to the right-hand
 * interval. True breaks keep both side identities. The very last domain point
 * remains with the last interval.
 */
static void
ts_i_assign_side(const tsICurve *c, tsReal u, size_t *idx, tsReal eps)
{
	size_t i = *idx;
	while (i + 1 < c->num_pieces &&
	       ts_i_abs(u - c->pieces[i].u1) <= 0.5 * eps &&
	       c->cont[i]) {
		++i;
	}
	*idx = i;
}

/**
 * Adds an isolated intersection, deduplicating on the parameter pair. Records
 * whose parameter pair belongs to a recorded affine overlap are suppressed.
 */
static tsError
ts_i_add_isolated(tsIWork *w, const tsIntersectionIsolated *rec,
                  tsStatus *status)
{
	tsIntersectionResult *o = w->out;
	size_t k;
	for (k = 0; k < o->num_isolated; ++k) {
		tsIntersectionIsolated *q = &o->isolated[k];
		if (ts_i_abs(q->u - rec->u) <= w->param_eps_a &&
		    ts_i_abs(q->v - rec->v) <= w->param_eps_b &&
		    q->ia == rec->ia && q->ib == rec->ib) {
			/* Keep the record with the tighter enclosing intervals. */
			if ((rec->u_max - rec->u_min) + (rec->v_max - rec->v_min) <
			    (q->u_max - q->u_min) + (q->v_max - q->v_min))
				*q = *rec;
			return TS_SUCCESS;
		}
	}
	{
		tsError err = ts_i_ensure((void **) &o->isolated, &w->cap_iso,
		                          sizeof(tsIntersectionIsolated),
		                          o->num_isolated + 1, status);
		if (err) return err;
		o->isolated[o->num_isolated++] = *rec;
	}
	return TS_SUCCESS;
}

static tsError
ts_i_add_overlap(tsIWork *w, const tsIntersectionOverlap *rec,
                 tsStatus *status)
{
	tsIntersectionResult *o = w->out;
	tsError err = ts_i_ensure((void **) &o->overlaps, &w->cap_ov,
	                          sizeof(tsIntersectionOverlap),
	                          o->num_overlaps + 1, status);
	if (err) return err;
	o->overlaps[o->num_overlaps++] = *rec;
	return TS_SUCCESS;
}

static tsError
ts_i_add_box(tsIWork *w, const tsIBox *b, int reason, tsStatus *status)
{
	tsIntersectionResult *o = w->out;
	tsError err = ts_i_ensure((void **) &o->boxes, &w->cap_box,
	                          sizeof(tsIntersectionBox),
	                          o->num_boxes + 1, status);
	if (err) return err;
	o->boxes[o->num_boxes].u_min = b->u0;
	o->boxes[o->num_boxes].u_max = b->u1;
	o->boxes[o->num_boxes].v_min = b->v0;
	o->boxes[o->num_boxes].v_max = b->v1;
	o->boxes[o->num_boxes].ia = b->ia;
	o->boxes[o->num_boxes].ib = b->ib;
	o->boxes[o->num_boxes].reason = reason;
	++o->num_boxes;
	return TS_SUCCESS;
}
/*! @} */



/*! @name Restricted Arcs and Conservative Exclusion
 * @{ */
/** Restriction of a piece to a sub-interval of its original domain. */
static void
ts_i_restrict(const tsICurve *c, const tsIBezierPiece *p,
              tsReal x0, tsReal x1, tsReal Q[4][3])
{
	tsReal span = p->u1 - p->u0;
	tsReal t0 = (x0 - p->u0) / span;
	tsReal t1 = (x1 - p->u0) / span;
	if (t0 < 0) t0 = 0;
	if (t1 > 1) t1 = 1;
	ts_i_bez_sub(p->P, c->deg, t0, t1, Q);
}

/**
 * Conservative exclusion test. Returns non-zero if no point of the two arcs
 * can be within spatial_eps of each other.
 */
static int
ts_i_can_exclude(tsIWork *w, const tsReal QA[4][3], size_t degA,
                 const tsReal QB[4][3], size_t degB, size_t dim)
{
	tsReal loA[3], hiA[3], loB[3], hiB[3];
	tsReal tol = w->spatial_eps + TS_I_NUM_MARGIN;
	size_t d;
	ts_i_bez_bbox(QA, degA, dim, loA, hiA);
	ts_i_bez_bbox(QB, degB, dim, loB, hiB);
	for (d = 0; d < dim; ++d) {
		if (loA[d] - tol > hiB[d])
			return 1;
		if (loB[d] - tol > hiA[d])
			return 1;
	}
	return 0;
}
/*! @} */



/*! @name Affine Coincidence Test
 * @{ */
/**
 * Locates a root of g(t) = QB_p(t) - target in [0, 1] by damped Newton
 * iteration from several starting points. Returns the best root found, i.e.
 * the parameter at which the full 3D mismatch of QB(t) and the reference
 * point is minimal. Returns 0 if no iterate stays inside [0, 1] with a small
 * residual. No division by a zero derivative is performed: a zero derivative
 * triggers a bisection-style step instead.
 */
static int
ts_i_locate_parameter(const tsReal QB[4][3], size_t degB, size_t dim,
                      size_t pivot, tsReal target, const tsReal ref[3],
                      tsReal *t_out)
{
	tsReal best = 0, best_res = 1e300;
	int found = 0;
	size_t s;
	for (s = 0; s < TS_I_MAP_STARTS; ++s) {
		tsReal t = (tsReal) s / (TS_I_MAP_STARTS - 1);
		size_t it;
		for (it = 0; it < 30; ++it) {
			tsReal pt[3], dv[3], g, gp, nt;
			ts_i_bez_eval(QB, degB, t, pt);
			ts_i_bez_deriv(QB, degB, t, dv);
			g = pt[pivot] - target;
			gp = dv[pivot];
			if (ts_i_abs(g) <= TS_I_NUM_MARGIN)
				break;
			if (ts_i_abs(gp) <= TS_I_NUM_MARGIN) {
				/* Zero velocity: step towards the target sign. */
				nt = t + (g > 0 ? -0.05 : 0.05);
			} else {
				nt = t - g / gp;
			}
			if (nt < 0) nt = 0;
			if (nt > 1) nt = 1;
			if (ts_i_abs(nt - t) < 1e-15)
				break;
			t = nt;
		}
		{
			tsReal pt[3], res;
			ts_i_bez_eval(QB, degB, t, pt);
			res = sqrt(ts_i_dist2(pt, ref, dim));
			if (res < best_res) {
				best_res = res;
				best = t;
				found = 1;
			}
		}
	}
	if (found && best_res <= 1e-8) {
		*t_out = best;
		return 1;
	}
	return 0;
}

/**
 * Composes the power-basis polynomial c(t) = Σ_{l=0}^m c[l] t^l with the
 * affine map t = alpha*s + beta and writes the coefficients of the result in
 * d[0..m].
 */
static void
ts_i_compose_affine(const tsReal c[4], size_t m,
                    tsReal alpha, tsReal beta, tsReal d[4])
{
	size_t k, l;
	for (k = 0; k <= 3; ++k)
		d[k] = 0;
	for (l = 0; l <= m; ++l) {
		tsReal bp[4]; /* beta powers */
		size_t k2;
		bp[0] = 1;
		for (k2 = 1; k2 <= l; ++k2)
			bp[k2] = bp[k2 - 1] * beta;
		for (k = 0; k <= l; ++k) {
			d[k] += c[l] * (tsReal) ts_i_choose(l, k) *
			        bp[l - k] *
			        ((tsReal) pow(alpha, (int) k));
		}
	}
}

/**
 * Tests whether the two restricted arcs QA(s) and QB(t) are the same
 * polynomial arc under an affine correspondence t = alpha*s + beta. On
 * success the correspondence is converted back to the original parameter
 * axes and stored in \p rec.
 *
 * The decision is a polynomial identity check: the affine map is recovered
 * from endpoint positions and tangents, both polynomials are converted to the
 * power basis, QB is composed with the map and all coefficients are compared
 * component-wise. Merely parallel or tolerance-close arcs fail the identity
 * check because their constant offset exceeds the identity tolerance.
 */
static int
ts_i_try_overlap(tsIWork *w,
                 const tsReal QA[4][3], size_t degA,
                 const tsReal QB[4][3], size_t degB,
                 size_t dim, const tsIBox *box,
                 tsReal min_extent,
                 tsIntersectionOverlap *rec)
{
	tsReal pA0[3], pA1[3], dA0[3], dA1[3], dv[3];
	tsReal alpha = 0, beta = 0;
	tsReal du = box->u1 - box->u0;
	tsReal dv_span = box->v1 - box->v0;
	tsReal extent, tol;
	size_t d, pivot, i;
	int use_right = 0;
	(void) w;

	if (du <= 0 || dv_span <= 0)
		return 0;

	/* Geometric extent of the restricted first arc. Coincidence is only
	 * claimed for non-degenerate arcs: on a sliver-sized box two merely
	 * tangent curves have power-basis coefficients that differ by less
	 * than any absolute tolerance, which would turn a single tangency
	 * into a fake overlap. */
	extent = 0;
	for (i = 0; i <= degA; ++i) {
		tsReal e = sqrt(ts_i_dist2(QA[0], QA[i], dim));
		extent = ts_i_max(extent, e);
	}
	if (extent < min_extent)
		return 0;

	ts_i_bez_eval(QA, degA, 0, pA0);
	ts_i_bez_eval(QA, degA, 1, pA1);
	ts_i_bez_deriv(QA, degA, 0, dA0);
	ts_i_bez_deriv(QA, degA, 1, dA1);

	/* Pick the component with the strongest tangent as pivot. The left
	 * endpoint is preferred; an isolated zero-velocity point at the left
	 * end makes us pivot on the right end instead. */
	pivot = 0;
	for (d = 1; d < dim; ++d)
		if (ts_i_abs(dA0[d]) > ts_i_abs(dA0[pivot]))
			pivot = d;
	if (ts_i_abs(dA0[pivot]) <= TS_I_NUM_MARGIN) {
		use_right = 1;
		pivot = 0;
		for (d = 1; d < dim; ++d)
			if (ts_i_abs(dA1[d]) > ts_i_abs(dA1[pivot]))
				pivot = d;
		if (ts_i_abs(dA1[pivot]) <= TS_I_NUM_MARGIN)
			return 0;
	}

	if (!use_right) {
		/* Find beta with QB(beta) = QA(0). */
		if (!ts_i_locate_parameter(QB, degB, dim, pivot,
		                           pA0[pivot], pA0, &beta))
			return 0;
		ts_i_bez_deriv(QB, degB, beta, dv);
		if (ts_i_abs(dv[pivot]) <= TS_I_NUM_MARGIN)
			return 0;
		alpha = dA0[pivot] / dv[pivot];
	} else {
		/* Find t* with QB(t*) = QA(1), then alpha from the right-end
		 * tangents and beta = t* - alpha. */
		tsReal tstar;
		if (!ts_i_locate_parameter(QB, degB, dim, pivot,
		                           pA1[pivot], pA1, &tstar))
			return 0;
		ts_i_bez_deriv(QB, degB, tstar, dv);
		if (ts_i_abs(dv[pivot]) <= TS_I_NUM_MARGIN)
			return 0;
		alpha = dA1[pivot] / dv[pivot];
		beta = tstar - alpha;
	}
	if (!ts_i_isfinite(alpha) || !ts_i_isfinite(beta) ||
	    ts_i_abs(alpha) < TS_I_NUM_MARGIN)
		return 0;
	/* The correspondence must map the box into itself. */
	if (beta < -1e-7 || beta > 1 + 1e-7 ||
	    ts_i_min(beta, beta + alpha) < -1e-7 ||
	    ts_i_max(beta, beta + alpha) > 1 + 1e-7)
		return 0;

	/* Exact identity check on the power-basis coefficients. */
	for (d = 0; d < dim; ++d) {
		tsReal ca[4], cb[4], composed[4];
		size_t k;
		tsReal cmax = 0;
		ts_i_bern_to_power(QA, degA, d, ca);
		ts_i_bern_to_power(QB, degB, d, cb);
		ts_i_compose_affine(cb, degB, alpha, beta, composed);
		for (k = 0; k <= degA; ++k)
			cmax = ts_i_max(cmax, ts_i_abs(ca[k]));
		tol = 1e-9 * ts_i_max(1, cmax) + 1e-12;
		for (k = 0; k <= ts_i_max(degA, degB); ++k) {
			tsReal x = k <= degA ? ca[k] : 0;
			tsReal y = k <= degB ? composed[k] : 0;
			if (ts_i_abs(x - y) > tol)
				return 0;
		}
	}

	/* Independent sanity check at interior parameters. */
	{
		tsReal s_test[5] = {0, 0.25, 0.5, 0.75, 1};
		size_t k;
		for (k = 0; k < 5; ++k) {
			tsReal s = s_test[k];
			tsReal t = alpha * s + beta;
			tsReal pa[3], pb[3];
			ts_i_bez_eval(QA, degA, s, pa);
			ts_i_bez_eval(QB, degB, t, pb);
			if (sqrt(ts_i_dist2(pa, pb, dim)) >
			    1e-9 * ts_i_max(1, extent))
				return 0;
		}
	}

	rec->ia = box->ia;
	rec->ib = box->ib;
	rec->u0 = box->u0;
	rec->u1 = box->u1;
	rec->v0 = box->v0 + beta * dv_span;
	rec->v1 = box->v0 + (beta + alpha) * dv_span;
	rec->a = alpha * dv_span / du;
	rec->b = rec->v0 - rec->a * rec->u0;
	return 1;
}
/*! @} */



/*! @name Terminal Box Solving
 * @{ */
/** Evaluates F(u,v) = A(u) - B(v) and its Jacobian in world coordinates. */
static void
ts_i_residual(const tsICurve *A, const tsICurve *B,
              const tsIBezierPiece *pa, const tsIBezierPiece *pb,
              tsReal u, tsReal v, tsReal F[3], tsReal JA[3], tsReal JB[3])
{
	tsReal pa_pt[3], pb_pt[3], da[3], db[3];
	tsReal ta = (u - pa->u0) / (pa->u1 - pa->u0);
	tsReal tb = (v - pb->u0) / (pb->u1 - pb->u0);
	size_t d;
	ts_i_bez_eval(pa->P, A->deg, ta, pa_pt);
	ts_i_bez_eval(pb->P, B->deg, tb, pb_pt);
	ts_i_bez_deriv(pa->P, A->deg, ta, da);
	ts_i_bez_deriv(pb->P, B->deg, tb, db);
	for (d = 0; d < 3; ++d) {
		F[d] = pa_pt[d] - pb_pt[d];
		/* Chain rule: derivative with respect to the original parameter. */
		JA[d] = da[d] / (pa->u1 - pa->u0);
		JB[d] = db[d] / (pb->u1 - pb->u0);
	}
}

/**
 * One constrained damped Gauss-Newton descent from a given start. The
 * Jacobian may be rank deficient (tangent contact) or contain a zero column
 * (isolated zero-velocity point); ridge regularization keeps every 2x2
 * system invertible. Steps are clamped to the box. The best candidate found
 * by this descent is merged into the running (best_u, best_v, best_res)
 * triple supplied by the caller.
 */
static void
ts_i_newton_descent(tsIWork *w, const tsIBezierPiece *pa,
                    const tsIBezierPiece *pb, const tsIBox *box,
                    tsReal start_u, tsReal start_v,
                    tsReal *best_u, tsReal *best_v, tsReal *best_res)
{
	const tsICurve *A = w->A, *B = w->B;
	size_t dim = A->dim, it, d;
	tsReal u = start_u, v = start_v;
	tsReal lambda = 1e-12;
	tsReal eta_u = TS_I_TERMINAL *
	               ts_i_max(1, ts_i_abs(box->u0) + ts_i_abs(box->u1));
	tsReal eta_v = TS_I_TERMINAL *
	               ts_i_max(1, ts_i_abs(box->v0) + ts_i_abs(box->v1));
	int stagnant = 0;

	for (it = 0; it < TS_I_MAX_NEWTON; ++it) {
		tsReal F[3], JA[3], JB[3];
		tsReal g00, g01, g11, h0, h1, det, du, dvn;
		tsReal nu, nv, res2 = 0, nres2 = 0;
		ts_i_residual(A, B, pa, pb, u, v, F, JA, JB);
		for (d = 0; d < dim; ++d)
			res2 += F[d] * F[d];
		if (res2 < *best_res) {
			*best_res = res2;
			*best_u = u;
			*best_v = v;
		}
		if (res2 <= 0.0)
			return;
		/* Normal equations of the damped least-squares step. */
		g00 = g01 = g11 = h0 = h1 = 0;
		for (d = 0; d < dim; ++d) {
			g00 += JA[d] * JA[d];
			g01 += JA[d] * JB[d];
			g11 += JB[d] * JB[d];
			h0 += JA[d] * F[d];
			h1 += JB[d] * F[d];
		}
		g00 += lambda;
		g11 += lambda;
		det = g00 * g11 - g01 * g01;
		if (det <= 0 || !ts_i_isfinite(det)) {
			lambda *= 10;
			if (lambda > 1)
				return;
			continue;
		}
		du = (g11 * h0 - g01 * h1) / det;
		dvn = (g00 * h1 - g01 * h0) / det;
		nu = u - du;
		/* F = A(u) - B(v): the second Jacobian column is -JB, so the
		 * Gauss-Newton update on v has the opposite sign of the u
		 * update. Using the positive-column normal-equation products
		 * above, the correct step is v += dvn. */
		nv = v + dvn;
		/* Clamp to the box: edge minima are legitimate outcomes of a
		 * constrained solve, unlike an escaping Newton step. */
		if (nu < box->u0) nu = box->u0;
		if (nu > box->u1) nu = box->u1;
		if (nv < box->v0) nv = box->v0;
		if (nv > box->v1) nv = box->v1;
		ts_i_residual(A, B, pa, pb, nu, nv, F, JA, JB);
		for (d = 0; d < dim; ++d)
			nres2 += F[d] * F[d];
		if (nres2 < *best_res) {
			*best_res = nres2;
			*best_u = nu;
			*best_v = nv;
		}
		if (nres2 < res2) {
			if (ts_i_abs(nu - u) <= eta_u &&
			    ts_i_abs(nv - v) <= eta_v) {
				++stagnant;
				if (stagnant >= 3)
					return;
			} else {
				stagnant = 0;
			}
			u = nu;
			v = nv;
			lambda = ts_i_max(lambda * 0.5, 1e-14);
		} else {
			lambda *= 10;
			if (lambda > 1)
				return;
		}
	}
}

/**
 * Locates the minimum-distance point of a box and returns its residual.
 *
 * The four box corners and the box midpoint are evaluated directly, and a
 * damped Gauss-Newton descent is run from each of them. Evaluating the
 * corners is essential because subdivision boundaries routinely run through
 * exact roots (including tangent contacts): the corner itself has residual
 * zero even when a constrained Newton step clamps before reaching it. The
 * best candidate over all starts is returned; \p on_edge tells the caller
 * whether that candidate lies on the box boundary.
 */
/**
 * Unconstrained damped Gauss-Newton refinement starting from \p (u,v).
 * The step is not clamped to a box (the true root may lie a few parameter
 * tolerances away from a grid-point terminal box), but a backtracking line
 * search keeps every accepted step residual-decreasing, which avoids the
 * oscillation of an undamped step near a self-intersection. The caller
 * validates that the result stays inside the allowed neighborhood.
 */
static void
ts_i_newton_free(tsIWork *w, const tsIBezierPiece *pa,
                 const tsIBezierPiece *pb,
                 tsReal start_u, tsReal start_v,
                 tsReal *u_out, tsReal *v_out, tsReal *res_out)
{
	const tsICurve *A = w->A, *B = w->B;
	size_t dim = A->dim, it, d;
	tsReal u = start_u, v = start_v;
	tsReal best_res = 1e300, bu = u, bv = v;

	for (it = 0; it < TS_I_MAX_NEWTON; ++it) {
		tsReal F[3], JA[3], JB[3];
		tsReal g00, g01, g11, h0, h1, det, du, dvn, res2 = 0;
		tsReal step_scale;
		ts_i_residual(A, B, pa, pb, u, v, F, JA, JB);
		for (d = 0; d < dim; ++d)
			res2 += F[d] * F[d];
		if (res2 < best_res) {
			best_res = res2;
			bu = u;
			bv = v;
		}
		if (res2 <= 0.0)
			break;
		g00 = g01 = g11 = h0 = h1 = 0;
		for (d = 0; d < dim; ++d) {
			g00 += JA[d] * JA[d];
			g01 += JA[d] * JB[d];
			g11 += JB[d] * JB[d];
			h0 += JA[d] * F[d];
			h1 += JB[d] * F[d];
		}
		/* Small ridge keeps singular (tangent) systems invertible. */
		det = (g00 + 1e-12) * (g11 + 1e-12) - g01 * g01;
		if (det <= 0 || !ts_i_isfinite(det))
			break;
		du = ((g11 + 1e-12) * h0 - g01 * h1) / det;
		dvn = ((g00 + 1e-12) * h1 - g01 * h0) / det;
		/* Opposite sign on v because F = A(u) - B(v). */
		step_scale = 1;
		for (d = 0; d < 20; ++d) {
			tsReal Fn[3], JAn[3], JBn[3], rn2 = 0;
			tsReal nu = u - step_scale * du;
			tsReal nv = v + step_scale * dvn;
			size_t d3;
			ts_i_residual(A, B, pa, pb, nu, nv, Fn, JAn, JBn);
			for (d3 = 0; d3 < dim; ++d3)
				rn2 += Fn[d3] * Fn[d3];
			if (rn2 < res2) {
				u = nu;
				v = nv;
				if (rn2 < best_res) {
					best_res = rn2;
					bu = u;
					bv = v;
				}
				break;
			}
			step_scale *= 0.5;
		}
		if (step_scale < 1e-12)
			break;
	}
	*u_out = bu;
	*v_out = bv;
	*res_out = sqrt(best_res);
}

/**
 * Locates the minimum-distance point of a box and returns its residual.
 *
 * The four box corners and the box midpoint are evaluated directly, and a
 * damped Gauss-Newton descent is run from each of them. When the minimum is
 * within a few spatial tolerances but above machine zero (a terminal box
 * sitting on a grid point next to the true root), an unconstrained
 * Gauss-Newton refinement is run and accepted only inside a widened
 * neighborhood.
 */
static int
ts_i_solve(tsIWork *w, const tsIBezierPiece *pa, const tsIBezierPiece *pb,
           const tsIBox *box, tsReal *u_out, tsReal *v_out,
           tsReal pt[3], tsReal *res_out, int *on_edge)
{
	const tsICurve *A = w->A;
	tsReal mu = (box->u0 + box->u1) * 0.5;
	tsReal mv = (box->v0 + box->v1) * 0.5;
	tsReal eta_u = TS_I_TERMINAL *
	               ts_i_max(1, ts_i_abs(box->u0) + ts_i_abs(box->u1));
	tsReal eta_v = TS_I_TERMINAL *
	               ts_i_max(1, ts_i_abs(box->v0) + ts_i_abs(box->v1));
	tsReal best_u = mu, best_v = mv, best_res = 1e300;
	tsReal su[5], sv[5];
	size_t k;

	su[0] = box->u0; sv[0] = box->v0;
	su[1] = box->u0; sv[1] = box->v1;
	su[2] = box->u1; sv[2] = box->v0;
	su[3] = box->u1; sv[3] = box->v1;
	su[4] = mu;       sv[4] = mv;
	for (k = 0; k < 5; ++k)
		ts_i_newton_descent(w, pa, pb, box, su[k], sv[k],
		                    &best_u, &best_v, &best_res);

	/* Refinement neighborhood. A box produced by the strip-depth cap may
	 * be centered on a grid point next to the true root, and the
	 * box-constrained descent cannot take the required step. Run an
	 * unconstrained Gauss-Newton refinement and accept it only if it
	 * converges to machine zero inside a widened neighborhood. */
	if (sqrt(best_res) <= 4 * w->spatial_eps) {
		tsReal wu = ts_i_max(box->u1 - box->u0,
		                     8 * w->param_eps_a);
		tsReal wv = ts_i_max(box->v1 - box->v0,
		                     8 * w->param_eps_b);
		tsReal fu, fv, fres;
		ts_i_newton_free(w, pa, pb, best_u, best_v,
		                 &fu, &fv, &fres);
		if (fres < sqrt(best_res) &&
		    fu >= box->u0 - wu && fu <= box->u1 + wu &&
		    fv >= box->v0 - wv && fv <= box->v1 + wv) {
			best_u = fu;
			best_v = fv;
			best_res = fres * fres;
		}
	}

	*u_out = best_u;
	*v_out = best_v;
	*res_out = sqrt(best_res);
	*on_edge = (best_u <= box->u0 + eta_u ||
	            best_u >= box->u1 - eta_u ||
	            best_v <= box->v0 + eta_v ||
	            best_v >= box->v1 - eta_v);
	ts_i_bez_eval(pa->P, A->deg,
	              (best_u - pa->u0) / (pa->u1 - pa->u0), pt);
	return 1;
}

/**
 * Terminal handling for a box at parameter-tolerance width (or at the
 * floating point subdivision limit).
 *
 * The multi-start constrained solve locates the minimum-distance point of
 * the box together with its residual and whether it lies on the boundary.
 *
 *  - A minimum at machine scale relative to the point magnitude certifies an
 *    actual root: the back-substituted distance is within the spatial
 *    tolerance and the box is within both parameter tolerances.
 *  - A tolerance-close but non-exact minimum is no proof of a root. An edge
 *    minimum faces the neighboring box and is pruned; an interior minimum
 *    keeps the box unresolved.
 *  - A minimum beyond four spatial tolerances proves the box empty; the
 *    band between one and four tolerances is the pathological near-approach
 *    region that may stay unresolved.
 *  - Duplicates of an already certified parameter pair and points of a
 *    recorded affine overlap are suppressed.
 */
static tsError
ts_i_terminal(tsIWork *w, const tsIBox *box, tsStatus *status)
{
	const tsICurve *A = w->A;
	const tsIBezierPiece *pa = &w->A->pieces[box->ia];
	const tsIBezierPiece *pb = &w->B->pieces[box->ib];
	tsReal u, v, pt[3], res;
	tsReal pscale = 0, exact_tol;
	int on_edge, exact, on_strip = 0;
	tsIntersectionResult *o = w->out;
	size_t k, dd;

	ts_i_solve(w, pa, pb, box, &u, &v, pt, &res, &on_edge);

	/* A terminal box is at most one parameter tolerance wide on each
	 * axis. If it intersects a recorded coincidence corridor (32
	 * tolerances wide on the strip-normal scalar), it cannot contain a
	 * distinct isolated root: such roots are guaranteed to be at least 64
	 * tolerances apart. The box therefore belongs to the coincidence and
	 * is dropped; only boxes strictly outside every corridor are solved. */
	{
		tsReal eps_v = w->param_eps_b;
		tsReal eps_u = w->param_eps_a;
		for (k = 0; k < o->num_overlaps; ++k) {
			const tsIntersectionOverlap *ov = &o->overlaps[k];
			tsReal pc[4], pmin, pmax;
			size_t i2;
			if (ov->ia != box->ia || ov->ib != box->ib)
				continue;
			if (box->u1 < ov->u0 - eps_u ||
			    box->u0 > ov->u1 + eps_u)
				continue;
			pc[0] = box->v0 - ov->a * box->u0 - ov->b;
			pc[1] = box->v0 - ov->a * box->u1 - ov->b;
			pc[2] = box->v1 - ov->a * box->u0 - ov->b;
			pc[3] = box->v1 - ov->a * box->u1 - ov->b;
			pmin = pmax = pc[0];
			for (i2 = 1; i2 < 4; ++i2) {
				pmin = ts_i_min(pmin, pc[i2]);
				pmax = ts_i_max(pmax, pc[i2]);
			}
			/* Intersects the corridor [pmin,pmax] n [-16e,16e]. */
			if (pmin <= 16 * eps_v && pmax >= -16 * eps_v) {
				on_strip = 1;
				break;
			}
		}
	}
	if (on_strip)
		return TS_SUCCESS;

	/* A root is "exact" only at machine scale relative to the evaluated
	 * point magnitude. This keeps a tangency tolerance tube from
	 * certifying every close approach as an isolated root. */
	for (dd = 0; dd < A->dim; ++dd)
		pscale = ts_i_max(pscale, ts_i_abs(pt[dd]));
	exact_tol = TS_I_ROOT_REL * pscale + TS_I_ROOT_ABS;
	exact = res <= exact_tol;

	/* Suppress points that are part of a recorded affine overlap. The
	 * check follows the correspondence itself, so it also covers reports
	 * on the interval next to a joint. */
	for (k = 0; k < o->num_overlaps; ++k) {
		const tsIntersectionOverlap *ov = &o->overlaps[k];
		tsReal mv = ov->a * u + ov->b;
		if (u >= ov->u0 - w->param_eps_a &&
		    u <= ov->u1 + w->param_eps_a &&
		    ts_i_abs(mv - v) <= w->param_eps_b)
			return TS_SUCCESS;
	}

	if (!exact) {
		/* Close but not exact, or beyond the tolerance: edge minima are
		 * handled by the neighboring box; only interior minima can
		 * leave a genuinely unresolved region. */
		if (on_edge || res > 4 * w->spatial_eps)
			return TS_SUCCESS;
		return ts_i_add_box(w, box, 1, status);
	}

	/* Same certified root as an existing report: keep one record. The
	 * merge radius is a few parameter tolerances because adjacent
	 * terminal boxes can host the same root; distinct roots are
	 * guaranteed to be at least 64 tolerances apart. */
	for (k = 0; k < o->num_isolated; ++k) {
		const tsIntersectionIsolated *q = &o->isolated[k];
		if (ts_i_abs(q->u - u) <= 4 * w->param_eps_a &&
		    ts_i_abs(q->v - v) <= 4 * w->param_eps_b)
			return TS_SUCCESS;
	}

	{
		tsIntersectionIsolated rec;
		size_t ia = box->ia, ib = box->ib;
		memset(&rec, 0, sizeof(rec));
		rec.u_min = box->u0;
		rec.u_max = box->u1;
		rec.v_min = box->v0;
		rec.v_max = box->v1;
		rec.u = u;
		rec.v = v;
		rec.point[0] = pt[0];
		rec.point[1] = pt[1];
		rec.point[2] = pt[2];
		ts_i_assign_side(w->A, u, &ia, w->param_eps_a);
		ts_i_assign_side(w->B, v, &ib, w->param_eps_b);
		rec.ia = ia;
		rec.ib = ib;
		return ts_i_add_isolated(w, &rec, status);
	}
}
/*! @} */



/*! @name Box Processing and Subdivision
 * @{ */
/** Splits a box along its widest tolerance-normalized axis. */
static tsError
ts_i_split(tsIWork *w, const tsIBox *box, tsStatus *status)
{
	tsReal du = box->u1 - box->u0;
	tsReal dv = box->v1 - box->v0;
	tsIBox c1 = *box, c2 = *box;
	tsReal m;
	int split_u;

	++w->expanded;
	if (w->expanded >= w->budget)
		w->budget_exceeded = 1;

	split_u = (du / w->param_eps_a) >= (dv / w->param_eps_b);
	if (split_u) {
		m = (box->u0 + box->u1) * 0.5;
		if (m - box->u0 <= TS_I_TERMINAL ||
		    box->u1 - m <= TS_I_TERMINAL)
			return ts_i_terminal(w, box, status);
		c1.u1 = m;
		c2.u0 = m;
	} else {
		m = (box->v0 + box->v1) * 0.5;
		if (m - box->v0 <= TS_I_TERMINAL ||
		    box->v1 - m <= TS_I_TERMINAL)
			return ts_i_terminal(w, box, status);
		c1.v1 = m;
		c2.v0 = m;
	}
	c1.depth = c2.depth = box->depth + 1;
	{
		tsError err = ts_i_stack_push(&w->stack, &c1, status);
		if (err) return err;
		return ts_i_stack_push(&w->stack, &c2, status);
	}
}

/**
 * Classifies a box against the already recorded overlap strips.
 *
 * Returns 1 if the whole box lies inside a coincidence corridor (its
 * parameter region is covered by an overlap result and needs no further
 * work), 0 if it is disjoint from every corridor, and -1 if it straddles
 * one. The corridor around v = a*u + b is sixteen parameter tolerances wide
 * on the normal scalar p = v - a*u - b; distinct isolated roots are
 * guaranteed to lie at least 64 tolerances away, so widening the exact strip
 * to this corridor never hides a distinct root.
 */
static int
ts_i_overlap_relation(tsIWork *w, const tsIBox *box)
{
	tsIntersectionResult *o = w->out;
	const tsReal cover = 16;
	size_t k;
	int any_straddle = 0;
	tsReal eps_v = w->param_eps_b;
	tsReal eps_u = w->param_eps_a;

	for (k = 0; k < o->num_overlaps; ++k) {
		const tsIntersectionOverlap *ov = &o->overlaps[k];
		tsReal a = ov->a, b = ov->b;
		tsReal pc[4], pmin, pmax;
		size_t i2;
		if (ov->ia != box->ia || ov->ib != box->ib)
			continue;
		if (box->u1 < ov->u0 - eps_u ||
		    box->u0 > ov->u1 + eps_u)
			continue;
		pc[0] = box->v0 - a * box->u0 - b;
		pc[1] = box->v0 - a * box->u1 - b;
		pc[2] = box->v1 - a * box->u0 - b;
		pc[3] = box->v1 - a * box->u1 - b;
		pmin = pmax = pc[0];
		for (i2 = 1; i2 < 4; ++i2) {
			pmin = ts_i_min(pmin, pc[i2]);
			pmax = ts_i_max(pmax, pc[i2]);
		}
		if (pmin > cover * eps_v || pmax < -cover * eps_v)
			continue; /* disjoint from this strip */
		/* Fully inside the corridor. */
		if (pmin >= -cover * eps_v && pmax <= cover * eps_v)
			return 1;
		/* Thin in the strip-normal direction while crossing it: such a
		 * box cannot contain a distinct root (they are 64 tolerances
		 * apart), so it is covered even if it is long along the strip. */
		if (pmax - pmin <= 2 * cover * eps_v)
			return 1;
		/* A genuine straddle requires corridor margin on both sides. */
		if (pmin < -cover * eps_v && pmax > cover * eps_v)
			any_straddle = 1;
	}
	return any_straddle ? -1 : 0;
}

static tsError
ts_i_process(tsIWork *w, const tsIBox *box, tsStatus *status);

/**
 * Bisects a straddling box along the axis that dominates its strip-normal
 * thickness dv + |a|*du. The dominant-axis split moves one sibling off the
 * corridor (it is then excluded or searched normally) while the other keeps
 * straddling with half the normal thickness. Repeating this reaches the
 * corridor in logarithmically many normal subdivisions instead of filling
 * the domain with uniformly small boxes.
 */
static tsError
ts_i_strip_split(tsIWork *w, const tsIBox *box, tsStatus *status)
{
	tsIntersectionResult *o = w->out;
	tsReal slope = 1;
	size_t k;
	tsIBox c1 = *box, c2 = *box;
	tsReal m;
	tsError err;

	for (k = 0; k < o->num_overlaps; ++k) {
		const tsIntersectionOverlap *ov = &o->overlaps[k];
		if (ov->ia == box->ia && ov->ib == box->ib) {
			slope = ov->a;
			break;
		}
	}
	++w->expanded;
	if (w->expanded >= w->budget)
		w->budget_exceeded = 1;

	if (box->v1 - box->v0 >=
	    ts_i_abs(slope) * (box->u1 - box->u0)) {
		m = (box->v0 + box->v1) * 0.5;
		if (m - box->v0 <= TS_I_TERMINAL ||
		    box->v1 - m <= TS_I_TERMINAL)
			return ts_i_terminal(w, box, status);
		c1.v1 = m;
		c2.v0 = m;
	} else {
		m = (box->u0 + box->u1) * 0.5;
		if (m - box->u0 <= TS_I_TERMINAL ||
		    box->u1 - m <= TS_I_TERMINAL)
			return ts_i_terminal(w, box, status);
		c1.u1 = m;
		c2.u0 = m;
	}
	c1.depth = c2.depth = box->depth + 1;
	err = ts_i_stack_push(&w->stack, &c1, status);
	if (err)
		return err;
	return ts_i_stack_push(&w->stack, &c2, status);
}

static tsError
ts_i_process(tsIWork *w, const tsIBox *box, tsStatus *status)
{
	const tsICurve *A = w->A, *B = w->B;
	const tsIBezierPiece *pa = &A->pieces[box->ia];
	const tsIBezierPiece *pb = &B->pieces[box->ib];
	tsReal QA[4][3], QB[4][3];
	tsIntersectionOverlap ov;
	tsReal du = box->u1 - box->u0;
	tsReal dv = box->v1 - box->v0;
	tsReal spread_a, spread_b, min_extent;
	tsReal ru, rv;
	size_t i, k;
	int relation;
	tsError err;

	ts_i_restrict(A, pa, box->u0, box->u1, QA);
	ts_i_restrict(B, pb, box->v0, box->v1, QB);

	if (ts_i_can_exclude(w, QA, A->deg, QB, B->deg, A->dim))
		return TS_SUCCESS;

	/* A box inside a coincidence corridor is done. A straddling box is
	 * bisected along the strip-normal direction, so the off-strip
	 * complement separates from the corridor in logarithmically many
	 * steps. This keeps the off-diagonal self-intersections visible
	 * alongside the diagonal overlap. */
	relation = ts_i_overlap_relation(w, box);
	if (relation == 1)
		return TS_SUCCESS;
	if (relation == -1) {
		if (du <= 16 * w->param_eps_a &&
		    dv <= 16 * w->param_eps_b)
			return TS_SUCCESS;
		if (du <= w->param_eps_a && dv <= w->param_eps_b)
			return ts_i_terminal(w, box, status);
		/* Bounded strip depth. A straddler at this depth has corridor
		 * width box size, but a box that has drifted well off the strip
		 * still hosts an isolated root and must be solved: the cap drops
		 * a box only when it intersects the corridor itself. */
		if (box->depth >= TS_I_STRIP_MAX_DEPTH) {
			tsIntersectionResult *o = w->out;
			int near_strip = 0;
			for (k = 0; k < o->num_overlaps; ++k) {
				const tsIntersectionOverlap *ov2 =
				    &o->overlaps[k];
				tsReal pc[4], pmin2, pmax2;
				size_t i2;
				if (ov2->ia != box->ia ||
				    ov2->ib != box->ib)
					continue;
				pc[0] = box->v0 - ov2->a * box->u0 -
				        ov2->b;
				pc[1] = box->v0 - ov2->a * box->u1 -
				        ov2->b;
				pc[2] = box->v1 - ov2->a * box->u0 -
				        ov2->b;
				pc[3] = box->v1 - ov2->a * box->u1 -
				        ov2->b;
				pmin2 = pmax2 = pc[0];
				for (i2 = 1; i2 < 4; ++i2) {
					pmin2 = ts_i_min(pmin2, pc[i2]);
					pmax2 = ts_i_max(pmax2, pc[i2]);
				}
				if (pmin2 <= 16 * w->param_eps_b &&
				    pmax2 >= -16 * w->param_eps_b) {
					near_strip = 1;
					break;
				}
			}
			if (near_strip)
				return TS_SUCCESS;
			return ts_i_terminal(w, box, status);
		}
		return ts_i_strip_split(w, box, status);
	}

	/* Detect a new coincidence. The strip is recorded, but the box is
	 * still bisected so its complement is explored. */
	spread_a = 0;
	spread_b = 0;
	for (i = 1; i <= A->deg; ++i) {
		spread_a = ts_i_max(spread_a,
		                    sqrt(ts_i_dist2(pa->P[0], pa->P[i], A->dim)));
		spread_b = ts_i_max(spread_b,
		                    sqrt(ts_i_dist2(pb->P[0], pb->P[i], B->dim)));
	}
	min_extent = TS_I_OVERLAP_MIN_RATIO *
	             ts_i_min(spread_a, spread_b);
	if (ts_i_try_overlap(w, QA, A->deg, QB, B->deg, A->dim, box,
	                     min_extent, &ov)) {
		err = ts_i_add_overlap(w, &ov, status);
		if (err)
			return err;
	}

	if (du <= w->param_eps_a && dv <= w->param_eps_b)
		return ts_i_terminal(w, box, status);

	/* Precision limit: solve directly instead of declaring unresolved, so
	 * exact contacts are still certified at machine scale. */
	ru = du / ts_i_max(1, ts_i_abs(box->u0) + ts_i_abs(box->u1));
	rv = dv / ts_i_max(1, ts_i_abs(box->v0) + ts_i_abs(box->v1));
	if (ru < TS_I_TERMINAL || rv < TS_I_TERMINAL)
		return ts_i_terminal(w, box, status);
	return ts_i_split(w, box, status);
}
/*! @} */




/*! @name Overlap Fusion and Result Ordering
 * @{ */
static int
ts_i_cmp_isolated(const void *x, const void *y)
{
	const tsIntersectionIsolated *p =
	    (const tsIntersectionIsolated *) x;
	const tsIntersectionIsolated *q =
	    (const tsIntersectionIsolated *) y;
	if (p->ia != q->ia)
		return p->ia < q->ia ? -1 : 1;
	if (p->ib != q->ib)
		return p->ib < q->ib ? -1 : 1;
	if (p->u < q->u)
		return -1;
	if (p->u > q->u)
		return 1;
	if (p->v < q->v)
		return -1;
	if (p->v > q->v)
		return 1;
	return 0;
}

static int
ts_i_cmp_overlap(const void *x, const void *y)
{
	const tsIntersectionOverlap *p =
	    (const tsIntersectionOverlap *) x;
	const tsIntersectionOverlap *q =
	    (const tsIntersectionOverlap *) y;
	if (p->ia != q->ia)
		return p->ia < q->ia ? -1 : 1;
	if (p->ib != q->ib)
		return p->ib < q->ib ? -1 : 1;
	if (p->u0 < q->u0)
		return -1;
	if (p->u0 > q->u0)
		return 1;
	return 0;
}

static int
ts_i_cmp_box(const void *x, const void *y)
{
	const tsIntersectionBox *p = (const tsIntersectionBox *) x;
	const tsIntersectionBox *q = (const tsIntersectionBox *) y;
	if (p->ia != q->ia)
		return p->ia < q->ia ? -1 : 1;
	if (p->ib != q->ib)
		return p->ib < q->ib ? -1 : 1;
	if (p->u_min < q->u_min)
		return -1;
	if (p->u_min > q->u_min)
		return 1;
	if (p->v_min < q->v_min)
		return -1;
	if (p->v_min > q->v_min)
		return 1;
	return 0;
}

/**
 * Fuses overlap records that describe the same affine correspondence on the
 * same pair of original intervals and whose parameter ranges abut. This
 * collapses the sub-boxes that independently detected one continuous
 * coincident range while keeping records on different original interval
 * pairs separate.
 */
static void
ts_i_fuse_overlaps(tsIntersectionResult *o, tsReal eps_a, tsReal eps_b)
{
	size_t i, j;
	for (i = 0; i < o->num_overlaps; ++i) {
		tsIntersectionOverlap *p = &o->overlaps[i];
		for (j = i + 1; j < o->num_overlaps;) {
			tsIntersectionOverlap *q = &o->overlaps[j];
			tsReal touch = q->u0 <= p->u1 + eps_a &&
			               q->u0 >= p->u0 - eps_a;
			tsReal same_map =
			    ts_i_abs(q->a - p->a) <=
			        1e-7 * ts_i_max(1, ts_i_abs(p->a)) &&
			    ts_i_abs(q->b - p->b) <= eps_b;
			if (q->ia == p->ia && q->ib == p->ib &&
			    touch && same_map) {
				p->u1 = ts_i_max(p->u1, q->u1);
				p->v0 = ts_i_min(p->v0, q->v0);
				p->v1 = ts_i_max(p->v1, q->v1);
				*q = o->overlaps[o->num_overlaps - 1];
				--o->num_overlaps;
				continue;
			}
			++j;
		}
	}
}
/*! @} */



/*! @name Public API
 * @{ */
void
ts_intersections_init(tsIntersectionResult *result)
{
	if (!result)
		return;
	result->isolated = NULL;
	result->num_isolated = 0;
	result->overlaps = NULL;
	result->num_overlaps = 0;
	result->boxes = NULL;
	result->num_boxes = 0;
	result->complete = 0;
	result->expanded = 0;
}

tsError
ts_intersections_copy(const tsIntersectionResult *src,
                      tsIntersectionResult *dest,
                      tsStatus *status)
{
	tsIntersectionResult tmp;
	if (!src || !dest)
		TS_RETURN_0(status, TS_INDEX_ERROR, "null result")
	/* A defensive init keeps free() safe even if the caller passes a
	 * destination that was not initialized with ts_intersections_init. */
	ts_intersections_init(&tmp);
	ts_intersections_init(dest);
	if (src->num_isolated) {
		tmp.isolated = (tsIntersectionIsolated *) malloc(
		    src->num_isolated * sizeof(tsIntersectionIsolated));
		if (!tmp.isolated)
			TS_RETURN_0(status, TS_MALLOC, "out of memory")
		memcpy(tmp.isolated, src->isolated,
		       src->num_isolated * sizeof(tsIntersectionIsolated));
		tmp.num_isolated = src->num_isolated;
	}
	if (src->num_overlaps) {
		tmp.overlaps = (tsIntersectionOverlap *) malloc(
		    src->num_overlaps * sizeof(tsIntersectionOverlap));
		if (!tmp.overlaps) {
			free(tmp.isolated);
			TS_RETURN_0(status, TS_MALLOC, "out of memory")
		}
		memcpy(tmp.overlaps, src->overlaps,
		       src->num_overlaps * sizeof(tsIntersectionOverlap));
		tmp.num_overlaps = src->num_overlaps;
	}
	if (src->num_boxes) {
		tmp.boxes = (tsIntersectionBox *) malloc(
		    src->num_boxes * sizeof(tsIntersectionBox));
		if (!tmp.boxes) {
			free(tmp.isolated);
			free(tmp.overlaps);
			TS_RETURN_0(status, TS_MALLOC, "out of memory")
		}
		memcpy(tmp.boxes, src->boxes,
		       src->num_boxes * sizeof(tsIntersectionBox));
		tmp.num_boxes = src->num_boxes;
	}
	tmp.complete = src->complete;
	tmp.expanded = src->expanded;
	ts_intersections_free(dest);
	*dest = tmp;
	return TS_SUCCESS;
}

void
ts_intersections_free(tsIntersectionResult *result)
{
	if (!result)
		return;
	if (result->isolated) free(result->isolated);
	if (result->overlaps) free(result->overlaps);
	if (result->boxes) free(result->boxes);
	ts_intersections_init(result);
}

tsError
ts_bspline_intersect(const tsBSpline *a,
                     const tsBSpline *b,
                     tsReal spatial_eps,
                     tsReal param_eps_a,
                     tsReal param_eps_b,
                     size_t budget,
                     tsIntersectionResult *result,
                     tsStatus *status)
{
	tsICurve A, B;
	tsIWork work;
	tsIntersectionResult out;
	tsError err;
	size_t i, j;

	if (status) {
		status->code = TS_SUCCESS;
		status->message[0] = '\0';
	}
	if (!a || !b || !result)
		TS_RETURN_0(status, TS_INDEX_ERROR, "null argument")
	if (spatial_eps < TS_I_EPS_MIN || spatial_eps > TS_I_EPS_MAX ||
	    param_eps_a < TS_I_EPS_MIN || param_eps_a > TS_I_EPS_MAX ||
	    param_eps_b < TS_I_EPS_MIN || param_eps_b > TS_I_EPS_MAX)
		TS_RETURN_0(status, TS_INDEX_ERROR, "tolerance out of range")
	if (budget == 0)
		TS_RETURN_0(status, TS_INDEX_ERROR, "budget must be positive")

	ts_i_curve_init(&A);
	ts_i_curve_init(&B);
	err = ts_i_curve_build(a, &A, spatial_eps, status);
	if (err)
		return err;
	err = ts_i_curve_build(b, &B, spatial_eps, status);
	if (err) {
		ts_i_curve_free(&A);
		return err;
	}
	if (A.dim != B.dim) {
		ts_i_curve_free(&A);
		ts_i_curve_free(&B);
		TS_RETURN_0(status, TS_DIM_ZERO,
		            "curve dimensions differ")
	}

	memset(&work, 0, sizeof(work));
	ts_intersections_init(&out);
	ts_i_stack_init(&work.stack);
	work.A = &A;
	work.B = &B;
	work.spatial_eps = spatial_eps;
	work.param_eps_a = param_eps_a;
	work.param_eps_b = param_eps_b;
	work.budget = budget;
	work.out = &out;

	/* Seed one box per pair of original non-zero knot intervals. */
	for (i = 0; i < A.num_pieces && !err; ++i)
		for (j = 0; j < B.num_pieces; ++j) {
			tsIBox seed;
			seed.u0 = A.pieces[i].u0;
			seed.u1 = A.pieces[i].u1;
			seed.v0 = B.pieces[j].u0;
			seed.v1 = B.pieces[j].u1;
			seed.ia = i;
			seed.ib = j;
			seed.depth = 0;
			err = ts_i_stack_push(&work.stack, &seed, status);
			if (err)
				break;
		}

	/* Branch and bound over the whole parameter domain. */
	while (!err && work.stack.num > 0) {
		tsIBox cur = work.stack.data[--work.stack.num];
		if (work.budget_exceeded) {
			err = ts_i_add_box(&work, &cur, 0, status);
			continue;
		}
		err = ts_i_process(&work, &cur, status);
		/* A split may have spent the last expansion: drain the rest
		 * of the stack into the unresolved boxes. */
		while (!err && work.budget_exceeded && work.stack.num > 0) {
			tsIBox rest = work.stack.data[--work.stack.num];
			err = ts_i_add_box(&work, &rest, 0, status);
		}
	}

	ts_i_stack_free(&work.stack);
	ts_i_curve_free(&A);
	ts_i_curve_free(&B);
	if (err) {
		/* Failure leaves the caller's previous result untouched. */
		ts_intersections_free(&out);
		return err;
	}

	ts_i_fuse_overlaps(&out, param_eps_a, param_eps_b);
	if (out.num_isolated > 1)
		qsort(out.isolated, out.num_isolated,
		      sizeof(tsIntersectionIsolated), ts_i_cmp_isolated);
	if (out.num_overlaps > 1)
		qsort(out.overlaps, out.num_overlaps,
		      sizeof(tsIntersectionOverlap), ts_i_cmp_overlap);
	if (out.num_boxes > 1)
		qsort(out.boxes, out.num_boxes,
		      sizeof(tsIntersectionBox), ts_i_cmp_box);
	out.complete = out.num_boxes == 0;
	out.expanded = work.expanded;

	/* Success or unresolved: replace the caller's result wholesale. */
	ts_intersections_free(result);
	*result = out;
	return TS_SUCCESS;
}
/*! @} */
