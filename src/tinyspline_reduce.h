/** @file tinyspline_reduce.h
 *
 * Knot removal and spline simplification for non-rational, clamped
 * B-Spline curves.
 *
 * Both entry points reduce the number of control points of a curve by
 * deleting copies of interior knots. They never change the degree, the
 * dimension, the parameter domain, the parameterization, or the two
 * endpoint positions, and they never introduce new knot values: an
 * interior knot is either kept (with its original value) or deleted.
 *
 * The implementation is pure C99 and only depends on the C standard
 * library and libm. No C++ wrapper is provided.
 */

#ifndef TINYSPLINE_REDUCE_H
#define TINYSPLINE_REDUCE_H

#include "tinyspline.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Lower bound (inclusive) of an accepted tolerance.
 */
#define TS_REDUCE_TOLERANCE_MIN 1e-9

/**
 * Upper bound (inclusive) of an accepted tolerance.
 */
#define TS_REDUCE_TOLERANCE_MAX 1e-2

/*! @name Error Bound and Floating Point Semantics
 *
 * @c error_bound is a certified upper bound on the Euclidean distance
 * between the input curve @c source and the returned curve, measured at
 * the same parameter value over the whole parameter domain. It is
 * computed from the common Bezier decomposition of the two curves on the
 * knot vector of @c source. For every non-empty knot interval
 * <tt>[u_i, u_{i+1})</tt> of @c source (including the one-sided limits at
 * discontinuity, i.e. interior knots whose multiplicity equals the
 * order), both curves are represented as degree-<em>p</em> Bezier
 * segments in the local variable
 *
 *     t = (u - u_i) / (u_{i+1} - u_i),  t in [0, 1].
 *
 * With Bezier control points @c A_j and @c C_j of the source and the
 * candidate on that segment, the difference curve has Bernstein control
 * vectors @c D_j = C_j - A_j. Because the Bernstein basis functions of a
 * fixed degree form a partition of unity with non-negative coefficients,
 * the convex hull property gives
 *
 *     || C(u) - A(u) || <= max_j || D_j ||   for all u in [u_i, u_{i+1}].
 *
 * Taking the maximum over all segments and all difference control
 * vectors yields a parameter-domain-wide upper bound. Sampling the
 * distance on a fixed grid is never used as a certificate. If the bound
 * turns out too loose, the difference curve is subdivided (the two
 * curves are re-refined at additional interior parameters) and the bound
 * is recomputed on the finer decomposition; subdivision strictly
 * tightens the convex-hull bound and is repeated, with a capped number of
 * refinement levels, until the bound fits the tolerance or no further
 * improvement is possible.
 *
 * Floating point behavior: every arithmetic step (knot insertion,
 * Bezier extraction, Bernstein coefficient comparison, subdivision) is
 * carried out in the precision of ::tsReal and is subject to ordinary
 * round-off. The certificate is therefore an upper bound on the
 * difference of the curves as represented by the stored, rounded control
 * points and knots, not a bound on an idealized real-number curve. To
 * prevent round-off from silently invalidating the guarantee, every
 * computed bound is inflated by a small safety margin before it is
 * compared with the tolerance and reported through @c error_bound:
 *
 *     reported = computed * (1 + TS_REDUCE_NUMERIC_MARGIN)
 *                + TS_REDUCE_NUMERIC_FLOOR * (1 + max(|u_min|, |u_max|))
 *
 * A candidate is accepted only if the inflated bound does not exceed the
 * tolerance, so accepted results stay inside the requested error budget
 * even when the arithmetic behind the certificate is slightly
 * optimistic. Conversely, the margin may reject a removal that an exact
 * real-arithmetic implementation would accept. For exactly reversible
 * material (curves obtained through exact knot insertion) with
 * coordinate magnitudes below ten, the round-off of the round trip is
 * small enough that the reported bound is below <tt>1e-10</tt> in the
 * default double precision build.
 *
 * @{
 */

/**
 * Relative safety margin added to every computed error bound.
 */
#define TS_REDUCE_NUMERIC_MARGIN 1e-9

/**
 * Absolute, domain-scaled floor added to every computed error bound.
 */
#define TS_REDUCE_NUMERIC_FLOOR 1e-14
/*! @} */

/**
 * Removes up to @p count copies of the interior knot @p knot from
 * @p source and stores the resulting curve in @p out.
 *
 * The degree, dimension, parameter domain and endpoint positions of the
 * result are identical to those of @p source. Knot values are never
 * moved and no new knot values are created. Each successful removal
 * deletes exactly one control point and one knot.
 *
 * Knot matching follows ::ts_knots_equal (::TS_KNOT_EPSILON): @p knot is
 * matched against the stored knot vector. If two or more distinct knot
 * values of @p source fall into the matching range of @p knot, the call
 * fails with ::TS_INDEX_ERROR. End knots may not be removed: targeting
 * the lower or the upper domain bound fails with ::TS_U_UNDEFINED.
 *
 * An interior knot whose multiplicity equals the order (degree + 1) is a
 * discontinuity barrier. Such a knot splits the curve into two
 * independently defined pieces and is never removed, even if the pieces
 * happen to be geometrically close. Targeting a barrier knot removes no
 * copy of it (the call still succeeds; see below).
 *
 * A removal that cannot be reconstructed within @p tolerance is a normal
 * rejection, not an interface error: the function returns ::TS_SUCCESS,
 * @p removed may be smaller than @p count (including zero) and @p out
 * contains the curve with the removals that were accepted. At least the
 * exact inverse of ::ts_bspline_insert_knot is supported. For
 * non-exactly-reversible material, candidate control points are obtained
 * from endpoint-preserving local least-squares reconstruction and from
 * the two directional inverse-insertion formulas; a candidate is kept
 * only if the certified error bound against @p source fits @p tolerance.
 *
 * Every accepted removal is certified against the original @p source
 * curve (never against the intermediate curve of an earlier removal), so
 * the tolerance budget cannot be consumed step by step.
 *
 * If @p count is zero, an independent deep copy of @p source is stored in
 * @p out, @p removed is set to zero and @p error_bound to zero.
 *
 * @p source and @p out may point to the same object. On success, @p out
 * owns its memory independently of @p source. On error, the objects
 * behind @p out, @p removed and @p error_bound are left untouched.
 *
 * Supported input (violations return an error): default double
 * precision; degree 1 to 3; dimension 2 or 3; non-rational clamped
 * curves with end multiplicity degree + 1; all control points finite
 * with coordinates of absolute value at most 1e4; distinct knot values
 * at least 1e-5 apart; knot absolute values at most 100. Interior
 * multiplicities may reach the order.
 *
 * @param[in] source
 * 	The curve to reduce.
 * @param[in] knot
 * 	The interior knot whose copies shall be removed.
 * @param[in] count
 * 	Maximum number of copies to remove; zero yields a deep copy.
 * @param[in] tolerance
 * 	Maximum accepted Euclidean distance at any shared parameter value,
 * 	in [::TS_REDUCE_TOLERANCE_MIN, ::TS_REDUCE_TOLERANCE_MAX].
 * @param[out] out
 * 	Receives the reduced curve (or a deep copy if nothing is removed).
 * @param[out] removed
 * 	Receives the number of copies actually removed. Must not be NULL.
 * @param[out] error_bound
 * 	Receives the certified upper bound of the deviation from @p source
 * 	over the whole parameter domain. Must not be NULL. The reported value
 * 	never exceeds @p tolerance.
 * @param[out] status
 * 	Optional status object receiving an error code and message.
 * @return TS_SUCCESS
 * 	On success, including the case where no copy could be removed.
 * @return TS_MALLOC
 * 	If memory allocation failed.
 * @return TS_DIM_ZERO
 * 	If the dimension is not 2 or 3 (also covers unsupported degrees).
 * @return TS_DEG_GE_NCTRLP
 * 	If the degree is outside the supported range.
 * @return TS_U_UNDEFINED
 * 	If @p knot targets a domain endpoint, is outside the domain or the
 * 	tolerance is outside its valid range.
 * @return TS_INDEX_ERROR
 * 	If @p knot cannot be matched, or if several distinct stored knots
 * 	fall into the matching range.
 * @return TS_MULTIPLICITY
 * 	If the input curve is not clamped as required by the supported
 * 	range.
 * @return TS_NO_RESULT
 * 	If the input contains non-finite control points or otherwise violates
 * 	the supported input range.
 */
tsError TINYSPLINE_API
ts_bspline_remove_knot(const tsBSpline *source,
                       tsReal knot,
                       size_t count,
                       tsReal tolerance,
                       tsBSpline *out,
                       size_t *removed,
                       tsReal *error_bound,
                       tsStatus *status);

/**
 * Repeatedly removes interior knots of @p source until no further copy
 * can be deleted within @p tolerance.
 *
 * The stored interior knot values are processed from small to large. One
 * copy at a time is considered; discontinuity barriers (interior knots
 * with multiplicity equal to the order) are skipped. Whenever a removal
 * is accepted, the scan restarts from the smallest interior knot; the
 * function finishes only when a complete pass accepts nothing.
 *
 * The candidate construction and acceptance rules are identical to
 * ::ts_bspline_remove_knot, but the total error is always measured
 * against the curve passed by the caller. The per-step calls are
 * internal operations on working copies; this function does not chain
 * public ::ts_bspline_remove_knot calls and therefore never compares an
 * intermediate curve against itself. @p removed is the total number of
 * deleted control points; the result is not required to have a globally
 * minimal number of control points.
 *
 * Memory and error semantics are identical to
 * ::ts_bspline_remove_knot: @p source and @p out may alias, successful
 * results own independent memory, and on error the output parameters are
 * not modified.
 *
 * @param[in] source
 * 	The curve to simplify.
 * @param[in] tolerance
 * 	Maximum accepted Euclidean distance at any shared parameter value,
 * 	in [::TS_REDUCE_TOLERANCE_MIN, ::TS_REDUCE_TOLERANCE_MAX].
 * @param[out] out
 * 	Receives the simplified curve (or a deep copy if nothing is removed).
 * @param[out] removed
 * 	Receives the total number of removed knot copies. Must not be NULL.
 * @param[out] error_bound
 * 	Receives the certified upper bound of the deviation from the
 * 	original @p source over the whole parameter domain. Must not be NULL.
 * @param[out] status
 * 	Optional status object receiving an error code and message.
 * @return TS_SUCCESS
 * 	On success, including the case where nothing could be removed.
 * @return TS_MALLOC
 * 	If memory allocation failed.
 * @return TS_DIM_ZERO
 * 	If the dimension is not 2 or 3.
 * @return TS_DEG_GE_NCTRLP
 * 	If the degree is outside the supported range.
 * @return TS_U_UNDEFINED
 * 	If the tolerance is outside its valid range.
 * @return TS_MULTIPLICITY
 * 	If the input curve is not clamped as required by the supported
 * 	range.
 * @return TS_NO_RESULT
 * 	If the input contains non-finite control points or otherwise violates
 * 	the supported input range.
 */
tsError TINYSPLINE_API
ts_bspline_simplify(const tsBSpline *source,
                    tsReal tolerance,
                    tsBSpline *out,
                    size_t *removed,
                    tsReal *error_bound,
                    tsStatus *status);

#ifdef __cplusplus
}
#endif

#endif /* TINYSPLINE_REDUCE_H */
