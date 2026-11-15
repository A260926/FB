#pragma once

#include <libnurbs/Core/Typedefs.hpp>
#include <vector>

namespace libnurbs
{
    class KnotVector;

    /**
     * @brief Insert a knot into a curve's knot vector and control points.
     *
     * The operation keeps the geometry unchanged in the original parameter
     * space, works for any clamped domain and handles an existing knot by
     * increasing its multiplicity. The final multiplicity must not exceed the
     * curve degree.
     *
     * @throws std::invalid_argument for degenerate input or an over-full knot.
     */
    void InsertKnotCurve(KnotVector& knot_vector,
                         std::vector<Vec4>& points,
                         int degree,
                         Numeric value,
                         int times = 1);
}
