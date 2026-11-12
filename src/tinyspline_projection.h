/** @file
 *
 * Global point projection for B-Spline curves.
 *
 * Snapping an editor cursor to a curve is a global nearest-point problem:
 * uniform sampling misses fold-backs and ::ts_bspline_bisect assumes a
 * monotone component. The structures and functions in this file implement a
 * reusable projection index that solves the global problem without rescanning
 * the entire curve on every query.
 *
 * The index is an owning snapshot of the source spline: after creation it
 * neither references nor depends on the ::tsBSpline it was created from.
 * Queries on a const index are stateless and can be issued concurrently from
 * multiple threads.
 */

#ifndef TINYSPLINE_PROJECTION_H
#define TINYSPLINE_PROJECTION_H

#include "tinyspline.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The result of a nearest-point query. Apart from the knot and the curve
 * point, a query returns a certified bracket of the true shortest distance
 * from the query point to the whole curve:
 *
 *     distance_lower <= true_minimum <= distance_upper
 *     0 <= distance_upper - distance_lower <= epsilon
 *
 * The epsilon passed to a query is a distance in curve space, not a parameter
 * step.
 */
typedef struct
{
	/**
	 * The knot of the nearest curve point. If the point coincides with a
	 * break knot, ::tsProjection::span_index disambiguates the side (the
	 * one-sided limit) this point belongs to.
	 */
	tsReal knot;

	/**
	 * Left index of the non-zero knot interval (in the source spline's
	 * original knot vector) on which ::tsProjection::knot was found.
	 */
	size_t span_index;

	/**
	 * The actual curve point at ::tsProjection::knot. The first
	 * \c dimension components (the dimensionality of the indexed spline)
	 * are valid; remaining components are set to zero.
	 */
	tsReal point[4];

	/**
	 * Lower bound of the shortest distance from the query point to the
	 * whole curve.
	 */
	tsReal distance_lower;

	/**
	 * Upper bound of the shortest distance. Equals the Euclidean distance
	 * between the query point and ::tsProjection::point.
	 */
	tsReal distance_upper;

	/**
	 * Number of original knot intervals on which local geometry
	 * refinement (Bezier subdivision) was actually performed during the
	 * query. Intervals pruned solely from their bounding-box lower bound
	 * are not counted.
	 */
	size_t visited_pieces;
} tsProjection;

/**
 * A reusable global projection index. Instances own all of their data and
 * use the PIMPL design pattern, just like ::tsBSpline. Initialize with
 * ::ts_projection_index_init, create with ::ts_projection_index_new, deep
 * copy with ::ts_projection_index_copy, and release with
 * ::ts_projection_index_free.
 */
typedef struct
{
	struct tsProjectionIndexImpl *pImpl; /**< The actual implementation. */
} tsProjectionIndex;

/**
 * Creates a new index whose data points to NULL.
 *
 * @return
 * 	A new index whose data points to NULL.
 */
tsProjectionIndex TINYSPLINE_API
ts_projection_index_init(void);

/**
 * Builds a projection index for \p spline and stores the result in \p index.
 * The index is an independent snapshot of \p spline: modifying or freeing
 * \p spline afterwards does not affect the index.
 *
 * Supported splines (double precision build only):
 *
 * - degree 1 to 3,
 * - dimension 1 to 4,
 * - finite control points,
 * - clamped knot vectors (end knots with full multiplicity),
 * - internal knots may have full multiplicity, in which case the curve has a
 *   genuine discontinuity. The two one-sided limits are indexed separately
 *   and are never connected across the break.
 *
 * @param[in] spline
 * 	The spline to index. Must not be NULL.
 * @param[out] index
 * 	The output index. Any previous data is released.
 * @param[out] status
 * 	The status of this function. May be NULL.
 * @return TS_SUCCESS
 * 	On success.
 * @return TS_DIM_ZERO
 * 	If the dimension of \p spline is not in [1, 4].
 * @return TS_DEG_GE_NCTRLP
 * 	If the degree of \p spline is not in [1, 3].
 * @return TS_MULTIPLICITY
 * 	If \p spline is not clamped.
 * @return TS_NO_RESULT
 * 	If a control point is not finite.
 * @return TS_MALLOC
 * 	If allocating memory failed.
 */
tsError TINYSPLINE_API
ts_projection_index_new(const tsBSpline *spline,
                        tsProjectionIndex *index,
                        tsStatus *status);

/**
 * Creates a deep copy of \p src and stores the copied data in \p dest. The
 * two indices are independent; queries on one do not affect the other.
 *
 * @param[in] src
 * 	The index to be deep copied.
 * @param[out] dest
 * 	The output index. Any previous data is released.
 * @param[out] status
 * 	The status of this function. May be NULL.
 * @return TS_SUCCESS
 * 	On success.
 * @return TS_MALLOC
 * 	If allocating memory failed.
 */
tsError TINYSPLINE_API
ts_projection_index_copy(const tsProjectionIndex *src,
                         tsProjectionIndex *dest,
                         tsStatus *status);

/**
 * Releases the data of \p index. After calling this function, the data of
 * \p index points to NULL.
 *
 * @param[out] index
 * 	The index to be released.
 */
void TINYSPLINE_API
ts_projection_index_free(tsProjectionIndex *index);

/**
 * Finds the globally nearest curve point of \p index to \p point.
 *
 * The query does not rescan the whole curve and does not redo the Bezier
 * decomposition: pieces are pruned from certified bounding-box distance
 * bounds, and only candidate pieces are subdivided. The call does not modify
 * the index, so concurrent queries on the same const index are safe.
 *
 * @param[in] index
 * 	The index to query. Must not be NULL.
 * @param[in] point
 * 	The query point. Must contain \p dimension finite components, each with
 * 	absolute value at most 1e6.
 * @param[in] dimension
 * 	Must equal the dimension of the indexed spline.
 * @param[in] epsilon
 * 	Maximum width of the certified distance bracket, as a distance in curve
 * 	space (not a parameter step). Must be finite and at least
 * 	<tt>1e-10 * max(1, diagonal of the control point bounding box)</tt>.
 * @param[out] result
 * 	The query result. Left unchanged on error.
 * @param[out] status
 * 	The status of this function. May be NULL.
 * @return TS_SUCCESS
 * 	On success.
 * @return TS_INDEX_ERROR
 * 	If \p index or \p point is NULL or \p index is empty.
 * @return TS_LCTRLP_DIM_MISMATCH
 * 	If \p dimension does not match the indexed spline.
 * @return TS_NO_RESULT
 * 	If a component of \p point is not finite or exceeds 1e6 in absolute
 * 	value, or if \p epsilon is not a valid tolerance.
 */
tsError TINYSPLINE_API
ts_projection_index_query(const tsProjectionIndex *index,
                          const tsReal *point,
                          size_t dimension,
                          tsReal epsilon,
                          tsProjection *result,
                          tsStatus *status);

#ifdef __cplusplus
}
#endif

#endif /* TINYSPLINE_PROJECTION_H */
