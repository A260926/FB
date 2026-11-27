/** @file
 *
 * @if jp
 * @brief 2 本の B-spline 曲線の交点・重合区間・未決ボックスを求める。
 * @else
 * @brief Intersection query for pairs of B-spline curves.
 * @endif
 *
 * @if jp
 * TinySpline の既存の評価・分割・二分インタフェースを保ちつつ、2 本の
 * 非有理クランプ B-spline 曲線（2 次元または 3 次元、次数 1 ～ 3）の
 * 交点を、パラメータ空間の分割統治によって求める。
 * @else
 * This module adds a curve/curve intersection query to TinySpline without
 * changing any existing interface.  The supported inputs are non-rational,
 * clamped B-spline curves of degree 1..3, with both curves sharing the same
 * dimensionality (2 or 3).  All parameters are reported on the original knot
 * domains of the input splines; the local [0,1] parameterization of Bezier
 * segments obtained from ::ts_bspline_to_beziers is never leaked to the
 * caller.
 * @endif
 *
 * @if jp
 * 結果は三種類の配列を持つ ::tsIntersection に格納される:
 * - 孤立交点 (::tsIntersectionPoint)
 * - 重合区間 (::tsIntersectionOverlap)
 * - まだ除外も帰着もできていないパラメータ箱 (::tsIntersectionBox)
 * @else
 * A query yields three independent lists inside ::tsIntersection:
 * - isolated intersections (::tsIntersectionPoint),
 * - coincident parameter segments (::tsIntersectionOverlap),
 * - unresolved parameter boxes (::tsIntersectionBox).
 * @endif
 *
 * @if jp
 * 完全に領域が除外されたか結果に帰着した場合だけ complete が真になる。
 * 予算切れや数値的な極限で分割できなくなった場合は complete が偽のまま
 * 残りの可能性を覆う未決ボックスが返される。未決は空の交差集合ではない。
 * @else
 * ::tsIntersection.complete is true only when the whole parameter domain has
 * either been conservatively excluded or assigned to a result.  When the box
 * budget is exhausted, or floating point precision prevents further
 * subdivision, the remaining candidate boxes are reported with
 * ::tsIntersection.complete set to false.  An incomplete result must never
 * be confused with an empty intersection.
 * @endif
 *
 * @if jp
 * 本モジュールは C 標準ライブラリのみを使用する (geometry ライブラリ不使用)。
 * @else
 * The implementation depends only on the C standard library; no external
 * geometry library is required.
 * @endif
 */
#ifndef TINYSPLINE_INTERSECT_H
#define TINYSPLINE_INTERSECT_H

#include "tinyspline.h"

#ifdef __cplusplus
extern "C" {
#endif

/*! @name Tolerance Bounds
 *
 * The tolerance arguments of ::ts_bspline_intersect must lie in the closed
 * interval [::TS_INTERSECT_TOL_MIN, ::TS_INTERSECT_TOL_MAX].  Coordinates are
 * bounded by 100 in absolute value and knot values by 100 as well, so these
 * fixed ranges cover every supported input without further scaling.
 *
 * @{
 */
/** Lower bound (inclusive) of any supported tolerance. */
#define TS_INTERSECT_TOL_MIN ((tsReal) 1e-8)

/** Upper bound (inclusive) of any supported tolerance. */
#define TS_INTERSECT_TOL_MAX ((tsReal) 1e-4)
/*! @} */

/*! @name Default Box Budget
 *
 * Default number of parameter boxes that may be expanded by a single query
 * when the caller passes zero as budget.  The benchmark materials complete
 * well below one million boxes.
 *
 * @{
 */
#define TS_INTERSECT_DEFAULT_BUDGET ((size_t) 1000000)
/*! @} */

/**
 * An isolated intersection of the two queried splines.
 *
 * Every field stores values on the <b>original knot domains</b> of the input
 * splines, never local Bezier parameters.  For two dimensional splines the
 * third component of ::tsIntersectionPoint.point is set to zero.
 */
typedef struct
{
	/**
	 * Small parameter interval of the first spline that contains the
	 * intersection: <tt>[u0, u1]</tt> with
	 * <tt>u1 - u0 <= utol</tt>.
	 */
	tsReal u0, u1;

	/**
	 * Small parameter interval of the second spline that contains the
	 * intersection: <tt>[v0, v1]</tt> with
	 * <tt>v1 - v0 <= vtol</tt>.
	 */
	tsReal v0, v1;

	/**
	 * Representative parameter of the first spline inside
	 * <tt>[u0, u1]</tt>.  Re-evaluating the first spline at ::u and the
	 * second spline at ::v yields points whose Euclidean distance does
	 * not exceed the spatial tolerance.
	 */
	tsReal u;

	/**
	 * Representative parameter of the second spline inside
	 * <tt>[v0, v1]</tt>.
	 */
	tsReal v;

	/**
	 * Three dimensional position of the intersection.  The stored point
	 * is the position of the first spline evaluated at ::u.  For two
	 * dimensional splines the third component is zero.
	 */
	tsReal point[3];

	/**
	 * Lower index of the original non-zero knot interval of the first
	 * spline: the spline is evaluated on the original interval
	 * <tt>[knots[i], knots[i+1]]</tt> with non-zero length.  Reports on
	 * a continuous knot are assigned to the right interval; the right
	 * most end of the domain belongs to the last interval.
	 */
	size_t ui;

	/**
	 * Lower index of the original non-zero knot interval of the second
	 * spline.  See ::ui.
	 */
	size_t vi;
} tsIntersectionPoint;

/**
 * A maximal coincident segment shared by the two queried splines inside a
 * single pair of original knot intervals.
 *
 * Coincidence means that, after restriction to the reported intervals, the
 * two polynomial arcs are algebraically identical up to an affine change of
 * parameter.  The correspondence between the parameter axes is
 *
 *     v = alpha * u + beta
 *
 * which also covers reversed arcs (::tsIntersectionOverlap.alpha < 0),
 * reparameterization, knot insertion and exact degree elevation.  The
 * endpoint fields always follow the direction of the corresponding axis:
 * ::u_from maps to ::v_from and ::u_to maps to ::v_to.
 */
typedef struct
{
	/** First spline parameter at the start of the coincident segment. */
	tsReal u_from;

	/** First spline parameter at the end of the coincident segment. */
	tsReal u_to;

	/** Second spline parameter paired with ::u_from. */
	tsReal v_from;

	/** Second spline parameter paired with ::u_to. */
	tsReal v_to;

	/** Slope of <tt>v = alpha * u + beta</tt> (may be negative). */
	tsReal alpha;

	/** Intercept of <tt>v = alpha * u + beta</tt>. */
	tsReal beta;

	/** Lower index of the original non-zero knot interval of spline 1. */
	size_t ui;

	/** Lower index of the original non-zero knot interval of spline 2. */
	size_t vi;
} tsIntersectionOverlap;

/**
 * A parameter box that could neither be excluded from the intersection nor
 * reduced to isolated points or overlapping segments.  Its coordinates are
 * always expressed on the original knot domains of the input splines.
 */
typedef struct
{
	/** Lower bound of the box on the parameter axis of the first spline. */
	tsReal u0;

	/** Upper bound of the box on the parameter axis of the first spline. */
	tsReal u1;

	/** Lower bound of the box on the parameter axis of the second spline. */
	tsReal v0;

	/** Upper bound of the box on the parameter axis of the second spline. */
	tsReal v1;

	/** Lower index of the original non-zero knot interval at ::u0. */
	size_t ui;

	/** Lower index of the original non-zero knot interval at ::v0. */
	size_t vi;

	/**
	 * Nonzero reason code (see the ::tsIntersectionReason constants)
	 * explaining why the box remains unresolved.
	 */
	int reason;
} tsIntersectionBox;

/**
 * Reasons why a parameter box stays unresolved.
 */
typedef enum
{
	/** No problem.  Only used internally; never stored in a result box. */
	TS_INTERSECT_REASON_NONE = 0,

	/** The box budget was exhausted while this box was still active. */
	TS_INTERSECT_REASON_BUDGET = 1,

	/** Floating point precision does not permit further subdivision. */
	TS_INTERSECT_REASON_PRECISION = 2,

	/**
	 * The two polynomial arcs agree on infinitely many parameter pairs
	 * through a correspondence that is not a single affine map, for
	 * example a constant curve or a non-affine coincidence.  Such a box
	 * cannot be expressed with ::tsIntersectionOverlap.
	 */
	TS_INTERSECT_REASON_NON_AFFINE = 3
} tsIntersectionReason;

/**
 * The result of a curve/curve intersection query.
 *
 * Instances are created with ::ts_intersection_init, deep copied with
 * ::ts_intersection_copy and released with ::ts_intersection_free.  The
 * arrays are owned by the instance and never alias the knots or control
 * points of the splines passed to ::ts_bspline_intersect.  Reusing an
 * instance across queries is supported: on success or an incomplete result
 * the previous contents are replaced atomically, while an erroneous call
 * leaves the previous contents untouched.
 */
typedef struct
{
	/** Array of isolated intersections (::num_points entries). */
	tsIntersectionPoint *points;

	/** Number of isolated intersections. */
	size_t num_points;

	/** Array of coincident segments (::num_overlaps entries). */
	tsIntersectionOverlap *overlaps;

	/** Number of coincident segments. */
	size_t num_overlaps;

	/** Array of unresolved parameter boxes (::num_boxes entries). */
	tsIntersectionBox *boxes;

	/** Number of unresolved parameter boxes. */
	size_t num_boxes;

	/**
	 * Nonzero if and only if the entire parameter domain was excluded
	 * or assigned to a result.  Zero means at least one unresolved box
	 * remains; in that case the intersection lists are still valid but
	 * may be incomplete.
	 */
	int complete;

	/**
	 * Number of parameter boxes that were actually expanded while
	 * processing the query.  Useful for diagnostics and for choosing a
	 * budget for the next call.
	 */
	size_t boxes_expanded;
} tsIntersection;

/**
 * Creates an empty result with all arrays set to NULL and ::complete set to
 * zero.  The returned instance is safe to pass to every other function of
 * this module, including ::ts_intersection_free.
 *
 * @return
 * 	An initialized, empty result.
 */
tsIntersection TINYSPLINE_API
ts_intersection_init(void);

/**
 * Creates a deep copy of @p src in @p dest.  @p dest must be initialized
 * with ::ts_intersection_init (or obtained from a previous call of this
 * module).  The previous contents of @p dest are released.  @p src and
 * @p dest may be the same instance.
 *
 * @param[in] src
 * 	The result to copy.
 * @param[out] dest
 * 	The destination of the copy.
 * @param[out] status
 * 	The status of this function.  May be NULL.
 * @return TS_SUCCESS
 * 	On success.
 * @return TS_MALLOC
 * 	If allocating memory failed.
 */
tsError TINYSPLINE_API
ts_intersection_copy(const tsIntersection *src,
                     tsIntersection *dest,
                     tsStatus *status);

/**
 * Releases the arrays owned by @p result and resets it to the state produced
 * by ::ts_intersection_init.
 *
 * @param[in,out] result
 * 	The result to release.
 */
void TINYSPLINE_API
ts_intersection_free(tsIntersection *result);

/**
 * Computes the intersection of two B-spline curves.
 *
 * The query covers every pair of original non-zero knot intervals.  Boxes of
 * the joint parameter domain are first tested with conservative bounding
 * volume exclusion; surviving boxes are solved locally with polynomial
 * algebra, which finds tangencies (no sign change) and fold-backs (multiple
 * parameters at the same point) without relying on fixed grid sampling or on
 * Newton iterations seeded at the endpoints.  Coincident polynomial arcs are
 * reported as affine parameter correspondences rather than as point clouds.
 *
 * On success (including an incomplete, budget-limited result) the previous
 * contents of @p result are replaced.  On error the previous contents are
 * preserved.  The output never references the memory of @p a or @p b, so the
 * input splines may be released as soon as the call returns.
 *
 * @param[in] a
 * 	The first spline.  Must be non-NULL, non-rational, clamped, of
 * 	degree 1..3 and dimensionality 2 or 3.  Degree 0 (constant)
 * 	splines are rejected with TS_NO_RESULT.
 * @param[in] b
 * 	The second spline, with the same dimensionality as @p a.
 * @param[in] stol
 * 	Spatial tolerance: two back-substituted positions count as an
 * 	intersection when their Euclidean distance is at most @p stol.
 * 	Must be in [::TS_INTERSECT_TOL_MIN, ::TS_INTERSECT_TOL_MAX].
 * @param[in] utol
 * 	Parameter tolerance on the axis of @p a.  Isolated intersections
 * 	are reported with an interval of width at most @p utol.  Must be
 * 	in [::TS_INTERSECT_TOL_MIN, ::TS_INTERSECT_TOL_MAX].
 * @param[in] vtol
 * 	Parameter tolerance on the axis of @p b, with the same bounds as
 * 	@p utol.
 * @param[in] budget
 * 	Maximum number of parameter boxes that may be expanded.  Zero
 * 	selects ::TS_INTERSECT_DEFAULT_BUDGET.
 * @param[out] result
 * 	An initialized, reusable result object.  Its previous contents are
 * 	replaced on success and preserved on error.
 * @param[out] status
 * 	The status of this function.  May be NULL.
 * @return TS_SUCCESS
 * 	On success, whether or not @p result is complete.
 * @return TS_INDEX_ERROR
 * 	If one of the arguments is NULL or the result object is invalid.
 * @return TS_DEG_GE_NCTRLP
 * 	If a spline is constant (degree 0) or has an unsupported degree.
 * @return TS_DIM_ZERO
 * 	If the splines have different or unsupported dimensionalities.
 * @return TS_NO_RESULT
 * 	If a tolerance is outside the supported range, the budget is
 * 	invalid or the splines are not clamped/non-rational.
 * @return TS_MALLOC
 * 	If allocating memory failed.
 */
tsError TINYSPLINE_API
ts_bspline_intersect(const tsBSpline *a,
                     const tsBSpline *b,
                     tsReal stol,
                     tsReal utol,
                     tsReal vtol,
                     size_t budget,
                     tsIntersection *result,
                     tsStatus *status);

#ifdef __cplusplus
}
#endif

#endif
