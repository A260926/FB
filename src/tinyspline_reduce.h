/** @file
 *
 * Knot removal and curve simplification for clamped, non-rational
 * B-Spline curves.
 *
 * The functions in this module reduce the number of control points of a
 * curve by deleting copies of @em internal knots only. Knots are never
 * moved, resampled, or replaced by another knot vector: the resulting
 * curve lives on the same parameter axis as the input curve.
 *
 * @section reduce_scope Supported Inputs
 *
 * The following inputs are supported (the default double precision build
 * is assumed):
 *
 * - degree 1, 2, or 3,
 * - dimension 2 or 3 (i.e. non-rational curves),
 * - clamped knot vectors: the first and the last knot each have
 *   multiplicity degree + 1,
 * - finite control points with coordinates of absolute value not greater
 *   than 1e4,
 * - knot absolute values not greater than 100 and distinct knot values
 *   spaced by at least 1e-5.
 *
 * Inputs outside of this scope are rejected with an error code and a
 * descriptive ::tsStatus message.
 *
 * @section reduce_tolerance Tolerance Semantics
 *
 * The @c tolerance argument is an upper bound on the @em Euclidean
 * distance between the original curve and the resulting curve at the
 * @em same parameter value, uniformly over the whole parameter domain.
 * Valid tolerances are in the closed interval [1e-9, 1e-2].
 *
 * A knot removal is accepted only if a certified error bound (see
 * ::ts_bspline_remove_knot) does not exceed the tolerance. If a removal
 * cannot be reconstructed within the tolerance, this is a regular
 * rejection and not an interface error: the function returns
 * ::TS_SUCCESS, the number of actually removed knots may be smaller than
 * requested (even zero), and the output curve contains all removals
 * accepted so far.
 *
 * @section reduce_barrier Fracture Barriers
 *
 * An internal knot whose multiplicity equals the order of the curve
 * (degree + 1) splits the curve into two independent pieces that may be
 * discontinuous. Such a knot is a @em fracture barrier. It is never
 * removed, even if the control points (or the geometry) on its two sides
 * happen to be close in space. Barriers are silently skipped (they cause
 * a regular rejection with zero removals for
 * ::ts_bspline_remove_knot and are passed over by
 * ::ts_bspline_simplify), they do not raise an error.
 *
 * @section reduce_certificate Error Bound Certificate
 *
 * The value reported through @c error_bound is a rigorous upper bound on
 * the distance between the @c source curve and the output curve over the
 * entire parameter domain, not the maximum deviation found by sampling
 * and not the local error of the last removal. It is computed as
 * follows:
 *
 * -# The original curve and the candidate curve are decomposed into
 *    B&eacute;zier segments on the knot partition of the @c source curve
 *    (the coarser candidate partition is refined with de Casteljau
 *    splits, so both curves are represented on the same intervals with
 *    B&eacute;zier coefficients of identical degree).
 * -# On every non-zero knot interval, the two B&eacute;zier curves are
 *    subtracted coefficient-wise. Because the Bernstein basis functions
 *    are non-negative and sum to one, the norm of the difference curve
 *    on that interval is bounded by the maximum Euclidean norm of its
 *    difference control vectors (convex hull property).
 * -# Every non-zero interval of the original knot vector contributes,
 *    including the one-sided limits on both sides of a fracture knot.
 * -# If the resulting bound is too loose, the difference B&eacute;zier
 *    polygon is subdivided (at fixed parameter positions) and the bound
 *    is recomputed on the sub-polygons, which can only tighten it. The
 *    certificate is never replaced by fixed-density sampling.
 *
 * @section reduce_float Floating Point Treatment
 *
 * The convex hull argument is an exact mathematical statement for the
 * floating point B&eacute;zier coefficients actually stored in the two
 * curves. The arithmetic used to obtain those coefficients (knot
 * removal, B&eacute;zier extraction, de Casteljau subdivision) is subject
 * to ordinary IEEE-754 roundoff. To keep the reported value a valid
 * upper bound despite that roundoff, a conservative slack term is added
 * to the geometric bound:
 *
 * @code
 * slack = 64 * DBL_EPSILON * (1 + max_magnitude_of_all_coefficients)
 * @endcode
 *
 * The slack scales with the magnitude of the involved coordinates and is
 * negligible compared with the supported tolerance range. In particular,
 * material that is exactly invertible (a knot inserted with
 * ::ts_bspline_insert_knot and removed again) with coordinate magnitudes
 * below 10 yields an error bound below 1e-10.
 *
 * @section reduce_ownership Memory Ownership and Aliasing
 *
 * On success, the output spline owns freshly allocated memory that is
 * independent of @c source (a deep copy even when nothing was removed).
 * @c source and @c out may point to the same object; in this case the
 * input is replaced in place only after the whole operation succeeded.
 * On error, @c out, @c removed, and @c error_bound are not modified. The
 * functions only allocate memory with the standard library and never
 * touch the global state.
 */
#ifndef TINYSPLINE_REDUCE_H
#define TINYSPLINE_REDUCE_H

#include "tinyspline.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Removes up to \p count copies of the internal knot \p knot from
 * \p source and stores the resulting curve in \p out. Each successful
 * removal deletes exactly one control point.
 *
 * The knot is matched against the knot vector of \p source using the
 * ::TS_KNOT_EPSILON convention (::ts_knots_equal). If two distinct knot
 * values fall into the matching range of \p knot, ::TS_NO_RESULT is
 * returned. End knots cannot be removed (::TS_U_UNDEFINED). A knot that
 * cannot be located is reported with ::TS_U_UNDEFINED. Knots with full
 * internal multiplicity (the order of the curve, so called fracture
 * barriers) are never removed: the function succeeds with
 * <tt>*removed == 0</tt> in that case.
 *
 * Every accepted removal is certified against the @em original
 * \p source curve (never against the intermediate curve), using the
 * certificate described in the header documentation. If removing the
 * next copy cannot be certified within \p tolerance, the loop stops and
 * the curve with all removals accepted so far is returned.
 *
 * If \p count is 0, an independent deep copy of \p source is stored in
 * \p out and <tt>*removed</tt> is set to 0.
 *
 * @param[in] source
 * 	The curve to reduce. Must be a clamped, non-rational curve of
 * 	degree 1 to 3 and dimension 2 or 3 with finite control points.
 * @param[in] knot
 * 	The internal knot whose copies shall be removed. Matching follows
 * 	::TS_KNOT_EPSILON.
 * @param[in] count
 * 	Maximum number of copies to remove.
 * @param[in] tolerance
 * 	Maximum permitted uniform Euclidean deviation, in [1e-9, 1e-2].
 * @param[out] out
 * 	Receives the resulting curve. Owns independent memory on success.
 * 	May alias \p source.
 * @param[out] removed
 * 	Receives the number of knots actually removed. May be NULL.
 * @param[out] error_bound
 * 	Receives the certified upper bound of the distance between
 * 	\p source and \p out over the whole parameter domain. The reported
 * 	value is always less than or equal to \p tolerance on success.
 * 	May be NULL.
 * @param[out] status
 * 	Receives an error code and message on failure. May be NULL.
 * @return TS_SUCCESS
 * 	On success, including regular rejections where fewer than
 * 	\p count copies (or none) could be removed.
 * @return TS_MALLOC
 * 	If memory allocation failed.
 * @return TS_U_UNDEFINED
 * 	If \p knot is an end knot or cannot be found in \p source.
 * @return TS_NO_RESULT
 * 	If \p tolerance is out of range, \p source is not clamped or not
 * 	within the supported scope, contains non-finite data, or the knot
 * 	match is ambiguous because several distinct knots match.
 * @return TS_INDEX_ERROR
 * 	If a required pointer argument is NULL.
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
 * Repeatedly removes internal knots from \p source until no further
 * removal can be certified within \p tolerance.
 *
 * The procedure scans the internal knot values of the current curve in
 * ascending order and tries to remove one copy at a time, skipping
 * fracture barriers. As soon as a removal is accepted, the scan restarts
 * from the smallest internal knot. A full pass without any accepted
 * removal terminates the procedure.
 *
 * Every candidate is judged by the same rules as in
 * ::ts_bspline_remove_knot, but the error certificate is always computed
 * against the curve originally passed to this function: the tolerance is
 * the total error budget for the whole simplification and is never
 * relaxed between successive removals. The result does not claim to use
 * the globally minimum number of control points, only that no further
 * knot can be removed under the given budget.
 *
 * @param[in] source
 * 	The curve to simplify. Must be a clamped, non-rational curve of
 * 	degree 1 to 3 and dimension 2 or 3 with finite control points.
 * @param[in] tolerance
 * 	Maximum permitted uniform Euclidean deviation, in [1e-9, 1e-2].
 * @param[out] out
 * 	Receives the simplified curve. Owns independent memory on success.
 * 	May alias \p source.
 * @param[out] removed
 * 	Receives the total number of removed knots. May be NULL.
 * @param[out] error_bound
 * 	Receives the certified upper bound of the distance between
 * 	\p source and \p out over the whole parameter domain. The reported
 * 	value is always less than or equal to \p tolerance on success.
 * 	May be NULL.
 * @param[out] status
 * 	Receives an error code and message on failure. May be NULL.
 * @return TS_SUCCESS
 * 	On success, including the case where no knot could be removed.
 * @return TS_MALLOC
 * 	If memory allocation failed.
 * @return TS_NO_RESULT
 * 	If \p tolerance is out of range, \p source is not clamped or not
 * 	within the supported scope, or contains non-finite data.
 * @return TS_INDEX_ERROR
 * 	If a required pointer argument is NULL.
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
