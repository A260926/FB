#pragma once
#include "libnurbs/Core/Typedefs.hpp"
#include <vector>

namespace libnurbs
{
    class KnotVector;

    /**
     * @brief Remove up to `times` copies of an existing interior knot.
     *
     * The points are given in the public Cartesian representation (x, y, z, w).
     * `tolerance` is an upper bound on the 3D Euclidean distance between the
     * source curve and the returned curve at the same parameter value, taken
     * over the whole parameter domain [0, 1].
     *
     * On return the knot vector and control points are only modified when at
     * least one copy was actually removed; if nothing could be removed within
     * tolerance the arguments are left untouched and 0 is returned.
     *
     * @return the number of copies actually removed (0 .. min(times, multiplicity)).
     */
    int KnotRemoval(KnotVector& knot_vector,
                    std::vector<Vec4>& points,
                    int degree,
                    Numeric knot_remove,
                    int times,
                    Numeric tolerance);

    namespace detail
    {
        /**
         * @brief Remove one copy of a knot from a curve in homogeneous coordinates.
         *
         * `knots` / `points` are the source curve; `out_knots` / `out_points`
         * receive the reduced curve.  Returns false if the removal is not
         * numerically admissible (degenerate denominator or non-finite data).
         */
        bool RemoveSingleKnotCopy(const std::vector<Numeric>& knots,
                                  const std::vector<Vec4>& points,
                                  int degree,
                                  Numeric knot_remove,
                                  std::vector<Numeric>& out_knots,
                                  std::vector<Vec4>& out_points);

        /**
         * @brief Insert a knot `times` times into a curve in homogeneous coordinates.
         */
        void InsertKnotRaw(const std::vector<Numeric>& knots,
                           const std::vector<Vec4>& points,
                           int degree,
                           Numeric knot_value,
                           int times,
                           std::vector<Numeric>& out_knots,
                           std::vector<Vec4>& out_points);

        /**
         * @brief Conservative geometric bound for a (possibly rational) curve or
         *        tensor-product surface.
         *
         * Both arguments hold homogeneous control points over one common knot
         * basis per direction, with identical row/column counts.  The returned
         * value bounds the 3D Euclidean distance between the two objects over the
         * whole parameter domain.  The bound is derived from the control nets
         * only (not from sampling) and involves the weights of every row, so it
         * is also valid for rational surfaces.
         *
         * Returns +infinity when the candidate contains non-finite values or
         * non-positive weights.
         */
        Numeric GeometryBound(const std::vector<std::vector<Vec4>>& original_rows,
                              const std::vector<std::vector<Vec4>>& candidate_rows);

        /**
         * @brief Remove a knot from one or more parallel curves sharing a knot vector.
         *
         * All rows share `knots` and are understood as the rows of a tensor
         * product grid along one axis.  Exactly one common number of removed
         * copies is chosen for every row, so the result stays on a single knot
         * basis.  Rows are given in Cartesian coordinates; on success
         * `out_knots` / `out_rows` receive the reduced representation.
         *
         * @return the number of copies actually removed (0 possible).  When 0 is
         *         returned the output arguments are untouched.
         */
        int RemoveKnotFromRows(const std::vector<Numeric>& knots,
                               const std::vector<std::vector<Vec4>>& rows,
                               int degree,
                               Numeric knot_remove,
                               int times,
                               Numeric tolerance,
                               std::vector<Numeric>& out_knots,
                               std::vector<std::vector<Vec4>>& out_rows);
    }
}
