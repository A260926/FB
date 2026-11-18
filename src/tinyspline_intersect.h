/** @file
 *
 * @brief
 * 	Conservative intersection query for two B-Spline curves.
 *
 * This module adds a two-curve intersection query to TinySpline without
 * changing any of the existing evaluation, subdivision or bisection
 * interfaces. Given two non-rational, clamped B-Spline curves of degree 1 to
 * 3 in 2D or 3D, ::ts_bspline_intersect computes
 *
 *  - isolated intersection points (::tsIntersectionIsolated),
 *  - exactly coincident parameter ranges (::tsIntersectionOverlap), and
 *  - unresolved parameter boxes (::tsIntersectionBox) that may still contain
 *    solutions when the work box budget is exhausted or floating point
 *    precision does not allow further subdivision.
 *
 * All parameters are reported on the original knot domains of the respective
 * input splines; local [0, 1] parameters of B\xC3\xA9zier segments are never
 * exposed. The query never borrows knots or control points of its inputs and
 * never mutates them.
 */

#ifndef TINYSPLINE_INTERSECT_H
#define TINYSPLINE_INTERSECT_H

#include "tinyspline.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A single, isolated intersection of two curves.
 *
 * An isolated intersection is reported when the two back-substituted curve
 * points are at most the spatial tolerance apart and the enclosing parameter
 * intervals are no wider than the respective parameter tolerances. The same
 * spatial point reached by two different parameter pairs (for instance on a
 * self-intersecting curve) yields two independent records: deduplication is
 * performed on parameter pairs, not on spatial positions.
 */
typedef struct
{
	/** Lower bound of the enclosing interval on the first parameter axis. */
	tsReal u_min;

	/** Upper bound of the enclosing interval on the first parameter axis. */
	tsReal u_max;

	/** Lower bound of the enclosing interval on the second parameter axis. */
	tsReal v_min;

	/** Upper bound of the enclosing interval on the second parameter axis. */
	tsReal v_max;

	/** Representative parameter of the first curve within [u_min, u_max]. */
	tsReal u;

	/** Representative parameter of the second curve within [v_min, v_max]. */
	tsReal v;

	/**
	 * Three-dimensional intersection position. For two-dimensional curves
	 * the third component is set to zero.
	 */
	tsReal point[3];

	/**
	 * Lower index of the original non-zero knot interval of the first
	 * curve, i.e. the result is located in
	 * <tt>[knots[ia], knots[ia + 1]]</tt> of the first curve. Reports on a
	 * continuous knot are assigned to the interval to the right; the very
	 * last domain point is assigned to the last interval.
	 */
	size_t ia;

	/** Lower index of the original non-zero knot interval of the second
	 *  curve. See ::tsIntersectionIsolated::ia. */
	size_t ib;
} tsIntersectionIsolated;

/**
 * A range of exact, affine-reparametrized coincidence of two curves.
 *
 * Coincidence is established by restricting both curves to the corresponding
 * original knot intervals and checking the restricted vector-valued
 * polynomials for identity under an affine parameter correspondence
 * <tt>v = a*u + b</tt>. Merely parallel or tolerance-close curves are never
 * classified as overlaps. Isolated intersections whose parameter pair lies on
 * this correspondence are suppressed, while isolated intersections outside
 * the correspondence are reported independently.
 */
typedef struct
{
	/** Lower index of the original non-zero knot interval of the first
	 *  curve. */
	size_t ia;

	/** Lower index of the original non-zero knot interval of the second
	 *  curve. */
	size_t ib;

	/** First curve parameter at the first endpoint of the coincident range. */
	tsReal u0;

	/** Second curve parameter at the first endpoint of the coincident range. */
	tsReal v0;

	/** First curve parameter at the second endpoint of the coincident range. */
	tsReal u1;

	/** Second curve parameter at the second endpoint of the coincident range. */
	tsReal v1;

	/**
	 * Slope of the affine correspondence <tt>v = a*u + b</tt>. May be
	 * negative (reversed parameterization). The endpoints are stored with
	 * their actual direction, so <tt>a < 0</tt> implies <tt>v1 < v0</tt>.
	 */
	tsReal a;

	/** Intercept of the affine correspondence <tt>v = a*u + b</tt>. */
	tsReal b;
} tsIntersectionOverlap;

/**
 * An unresolved parameter box. Every box covers parameter regions that have
 * neither been conservatively excluded nor assigned to an intersection
 * result, so the union of all boxes always contains every possible remaining
 * solution. Boxes are reported on the original knot domains and carry the
 * lower indices of the original non-zero knot intervals they belong to.
 */
typedef struct
{
	/** Lower bound on the first parameter axis. */
	tsReal u_min;

	/** Upper bound on the first parameter axis. */
	tsReal u_max;

	/** Lower bound on the second parameter axis. */
	tsReal v_min;

	/** Upper bound on the second parameter axis. */
	tsReal v_max;

	/** Lower index of the original non-zero knot interval of the first
	 *  curve. */
	size_t ia;

	/** Lower index of the original non-zero knot interval of the second
	 *  curve. */
	size_t ib;

	/**
	 * Zero if the box is unresolved because the work box budget was
	 * exhausted, one if floating point precision did not allow further
	 * subdivision.
	 */
	int reason;
} tsIntersectionBox;

/**
 * Output object of ::ts_bspline_intersect. Instances are created with
 * ::ts_intersections_init, copied with ::ts_intersections_copy, released
 * with ::ts_intersections_free and reused across queries. The query replaces
 * the whole content on success or on an unresolved (::tsIntersectionResult
 * ::complete == 0) result; if the query fails, the previous content is left
 * untouched.
 */
typedef struct
{
	/** Array of isolated intersections. NULL iff num_isolated == 0. */
	tsIntersectionIsolated *isolated;

	/** Number of elements in ::tsIntersectionResult::isolated. */
	size_t num_isolated;

	/** Array of coincident parameter ranges. NULL iff num_overlaps == 0. */
	tsIntersectionOverlap *overlaps;

	/** Number of elements in ::tsIntersectionResult::overlaps. */
	size_t num_overlaps;

	/** Array of unresolved parameter boxes. NULL iff num_boxes == 0. */
	tsIntersectionBox *boxes;

	/** Number of elements in ::tsIntersectionResult::boxes. */
	size_t num_boxes;

	/**
	 * Non-zero if the entire parameter domains have been either excluded or
	 * assigned to results. Zero if unresolved boxes remain; in this case an
	 * empty intersection must never be concluded.
	 */
	int complete;

	/**
	 * Number of working boxes actually expanded (subdivided) during the
	 * query. Does not include boxes that were excluded without subdivision.
	 */
	size_t expanded;
} tsIntersectionResult;

/**
 * Initializes an empty result object. Always succeeds and leaves the object in
 * a state that can safely be passed to ::ts_bspline_intersect,
 * ::ts_intersections_copy and ::ts_intersections_free.
 *
 * @param[out] result
 * 	The result object to initialize.
 */
void TINYSPLINE_API
ts_intersections_init(tsIntersectionResult *result);

/**
 * Creates a deep copy of \p src in \p dest. \p dest must have been initialized
 * with ::ts_intersections_init. All arrays are owned by the destination, so
 * releasing the source never affects the copy.
 *
 * @param[in] src
 * 	The result to copy.
 * @param[out] dest
 * 	Receives the copy. Its previous content is released.
 * @param[out] status
 * 	The status of this function. May be NULL.
 * @return TS_SUCCESS
 * 	On success.
 * @return TS_MALLOC
 * 	If allocating memory failed.
 */
tsError TINYSPLINE_API
ts_intersections_copy(const tsIntersectionResult *src,
                      tsIntersectionResult *dest,
                      tsStatus *status);

/**
 * Releases all arrays owned by \p result and resets it to the empty state.
 * Reinitializes the object such that it can be reused or freed again.
 *
 * @param[out] result
 * 	The result object to release.
 */
void TINYSPLINE_API
ts_intersections_free(tsIntersectionResult *result);

/**
 * Intersects two non-rational, clamped B-Spline curves.
 *
 * Both curves must have the same dimensionality, which must be 2 or 3, and a
 * degree between 1 and 3. All control points must be finite. Internal knots
 * may have multiplicity up to the order (degree + 1), so genuinely
 * discontinuous (broken) curves are supported. Constant (entirely
 * zero-velocity) curves are rejected with TS_NO_RESULT; an isolated
 * zero-velocity point inside an otherwise regular curve is handled without
 * dividing by it.
 *
 * The tolerances must lie in <tt>[1e-8, 1e-4]</tt>. \p spatial_eps bounds the
 * accepted distance of the two back-substituted positions, \p param_eps_a and
 * \p param_eps_b bound the widths of the reported parameter intervals on the
 * respective original parameter axes. \p budget must be positive and bounds the
 * number of working boxes that may be expanded (see
 * ::tsIntersectionResult::expanded).
 *
 * @param[in] a
 * 	The first curve.
 * @param[in] b
 * 	The second curve. May be the same object as \p a.
 * @param[in] spatial_eps
 * 	Spatial tolerance in <tt>[1e-8, 1e-4]</tt>.
 * @param[in] param_eps_a
 * 	Parameter tolerance for the first curve in <tt>[1e-8, 1e-4]</tt>.
 * @param[in] param_eps_b
 * 	Parameter tolerance for the second curve in <tt>[1e-8, 1e-4]</tt>.
 * @param[in] budget
 * 	Positive work box budget.
 * @param[out] result
 * 	Reusable output object. On success (including an unresolved query) its
 * 	previous content is replaced; on failure it is left unchanged.
 * @param[out] status
 * 	The status of this function. May be NULL.
 * @return TS_SUCCESS
 * 	On success, including the case where unresolved boxes remain (inspect
 * 	::tsIntersectionResult::complete).
 * @return TS_INDEX_ERROR
 * 	If any argument pointer is NULL.
 * @return TS_DIM_ZERO
 * 	If the dimensions differ or are neither 2 nor 3.
 * @return TS_DEG_GE_NCTRLP
 * 	If a degree is outside <tt>[1, 3]</tt>.
 * @return TS_NO_RESULT
 * 	If a curve is constant (has no non-degenerate arc), or if a control
 * 	point is not finite.
 * @return TS_MALLOC
 * 	If allocating memory failed.
 */
tsError TINYSPLINE_API
ts_bspline_intersect(const tsBSpline *a,
                     const tsBSpline *b,
                     tsReal spatial_eps,
                     tsReal param_eps_a,
                     tsReal param_eps_b,
                     size_t budget,
                     tsIntersectionResult *result,
                     tsStatus *status);

#ifdef __cplusplus
}
#endif

#endif /* TINYSPLINE_INTERSECT_H */
