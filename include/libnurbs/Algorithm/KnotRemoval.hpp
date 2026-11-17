#pragma once
#include "libnurbs/Core/Typedefs.hpp"
#include <vector>

namespace libnurbs
{
    class KnotVector;

    /**
     * @brief Remove up to `times` copies of an existing interior knot.
     *
     * The knot vector and control points are only modified when at least one
     * removal is accepted. On a zero result the inputs are left bit-for-bit
     * unchanged. On success both containers shrink by exactly the returned
     * number of removals.
     *
     * `points` use the public Cartesian-plus-weight representation; the
     * computations are carried out in homogeneous coordinates.
     *
     * `tolerance` is an upper bound on the 3D Euclidean distance between the
     * input and the returned object at every parameter value of [0, 1].
     * The returned count may therefore be smaller than `times`.
     */
    int KnotRemoval(KnotVector& knot_vector,
                    std::vector<Vec4>& points,
                    int degree,
                    Numeric knot_remove,
                    int times,
                    Numeric tolerance);

    // Internal building blocks shared by the curve and surface removal paths.
    // All control points in this namespace are homogeneous: head<3>() is the
    // weighted Cartesian coordinate and w() the weight.
    namespace detail
    {
        // Index of the last knot <= u (same convention as KnotVector::FindSpanIndex).
        int FindSpan(const std::vector<Numeric>& knots, int degree, Numeric u);

        // Number of entries exactly equal to `value`.
        int CountMultiplicity(const std::vector<Numeric>& knots, Numeric value);

        // Locate an existing knot within a tiny round-off tolerance and return
        // its stored value (the value used by all subsequent arithmetic).
        bool FindExistingKnot(const std::vector<Numeric>& knots,
                              Numeric requested,
                              Numeric& stored_value);

        // Remove one copy of `knot` (A5.8, single step).
        // Returns false when the removal is structurally impossible.
        bool RemoveKnotOnce(const std::vector<Numeric>& knots,
                            const std::vector<Vec4>& points,
                            int degree,
                            Numeric knot,
                            std::vector<Numeric>& out_knots,
                            std::vector<Vec4>& out_points);

        // Insert one copy of `knot` (Boehm), matching Curve::InsertKnot.
        bool InsertKnotOnce(const std::vector<Numeric>& knots,
                            const std::vector<Vec4>& points,
                            int degree,
                            Numeric knot,
                            std::vector<Numeric>& out_knots,
                            std::vector<Vec4>& out_points);

        // Conservative bound on the pointwise Euclidean distance between two
        // rational curves/surfaces represented by their homogeneous control
        // nets on the *same* knot basis. For tensor-product nets the entries
        // are compared index by index, and the rational denominator is taken
        // over the whole net.
        Numeric DistanceBound(const std::vector<Vec4>& reference,
                              const std::vector<Vec4>& candidate);

        // True when both knot vectors describe the same basis (up to the
        // round-off tolerance used for knot matching).
        bool SameKnotBasis(const std::vector<Numeric>& a, const std::vector<Numeric>& b);

        // Translation/rotation invariant round-off allowance for `tolerance`.
        Numeric RoundoffSlack(const std::vector<Vec4>& homogeneous_net);

        // Tensor-product knot removal along one axis. `net` is homogeneous and
        // stored row-major with `count_u` entries per v-row (the layout used by
        // Grid<Vec4>). `degree_along` belongs to the axis being reduced; the
        // other direction is untouched.
        //
        // All control rows are kept on one common knot basis. A row that cannot
        // follow the full count stays on the basis of the last accepted level,
        // so the returned count is the largest prefix every row can reproduce.
        // `tolerance` bounds the Euclidean distance between the input and the
        // returned surface over the whole parameter domain.
        int RemoveKnotTensor(std::vector<Numeric>& knots_along,
                             std::vector<Vec4>& net,
                             int count_u,
                             int count_v,
                             int degree_along,
                             bool along_v,
                             Numeric knot,
                             int times,
                             Numeric tolerance);

        // True when every entry of a homogeneous net is finite and has a
        // positive weight. Used to reject non-finite candidates defensively.
        bool NetIsWellFormed(const std::vector<Vec4>& net);
    }
}
