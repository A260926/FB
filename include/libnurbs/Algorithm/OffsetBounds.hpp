#pragma once

#include <vector>
#include <libnurbs/Core/Typedefs.hpp>
#include <libnurbs/Surface/Surface.hpp>

namespace libnurbs
{
    /**
     * @brief Status returned by the independent normal-offset certification.
     *
     * - @c certified:   the leaf partition covers [0,1]^2, every leaf has a
     *                   strictly positive lower bound on the source cross
     *                   product length and a finite upper bound on
     *                   ||candidate - T|| not exceeding the tolerance.
     * - @c unresolved:  the structural inputs are valid but the proof could
     *                   not be closed within @c max_boxes leaf boxes.
     * - @c invalid_input: a structural or numerical precondition is violated
     *                   (bad degree, open/non-unit domain, non-positive or
     *                   non-finite weights, non-finite geometry, illegal
     *                   distance/tolerance/budget). Neither surface is
     *                   modified in any status.
     */
    enum class OffsetCertStatus
    {
        certified,
        unresolved,
        invalid_input,
    };

    /**
     * @brief One leaf parameter box of the certification partition.
     *
     * The boxes are axis-aligned rectangles [Umin,Umax] x [Vmin,Vmax]. Their
     * interiors are pairwise disjoint and their union is exactly [0,1]^2.
     */
    struct OffsetCertBox
    {
        Numeric Umin{0};
        Numeric Umax{0};
        Numeric Vmin{0};
        Numeric Vmax{0};

        /// Rigorous, non-negatively rounded lower bound of
        /// ||S_u(u,v) x S_v(u,v)|| over the whole box.
        Numeric CrossLowerBound{0};

        /// Rigorous, upward-rounded upper bound of
        /// ||C(u,v) - (S(u,v) + d * (S_u x S_v)/||S_u x S_v||)||
        /// over the whole box, including floating-point round-off.
        Numeric ErrorUpperBound{0};
    };

    /**
     * @brief Full result of a certification run.
     */
    struct OffsetCertification
    {
        OffsetCertStatus Status{OffsetCertStatus::invalid_input};
        std::vector<OffsetCertBox> Boxes{};

        /// Number of leaf boxes actually used; never exceeds max_boxes.
        int BoxesUsed{0};

        /// Worst (smallest) cross-product lower bound over all leaves.
        Numeric WorstCrossLowerBound{0};

        /// Worst (largest) error upper bound over all leaves.
        Numeric WorstErrorUpperBound{0};

        [[nodiscard]] bool IsCertified() const
        {
            return Status == OffsetCertStatus::certified;
        }
    };

    /**
     * @brief Independently certify that @p candidate approximates the normal
     *        offset T(u,v) = S(u,v) + d * normalize(S_u x S_v).
     *
     * The check never mutates @p source or @p candidate and never refits the
     * candidate: subdivision only tightens the proof.
     *
     * Enclosure basis (all rounding is performed with directed interval
     * arithmetic, so every bound is conservative):
     *
     * 1. Each NURBS surface is, per knot span, a rational Bernstein-Bezier
     *    patch. Greville knot insertion is used to bring source and candidate
     *    onto a common knot partition; the Bezier control coefficients are
     *    the existing control points once every interior breakpoint has
     *    multiplicity equal to the degree, so the convex-hull / Bernstein
     *    coefficient property holds exactly (up to rounding): every value of
     *    a component over the span box is a convex combination of its
     *    coefficients. Sub-patches after subdivision come from interval de
     *    Casteljau splits.
     *
     * 2. First, second and third parametric derivatives are enclosed from
     *    the differentiated Bernstein forms. Because a (sub-)patch is
     *    parametrized locally on [0,1]^2 while covering a physical box of
     *    widths h_u, h_v, the chain-rule factors 1/h_u, 1/h_v convert local
     *    jets to physical (u,v) derivatives before any enclosure is used.
     *    Rational derivatives follow the same quotient rule as
     *    Surface::EvaluateAll, evaluated with interval arithmetic and a
     *    strictly positive weight interval.
     *
     * 3. On every leaf the cross product G(u,v) = S_u x S_v is enclosed by a
     *    centered (Moore) Taylor form: the exact value at the box center, an
     *    interval gradient term and a quadratic Hessian remainder
     *    (1/2 * delta^T H delta), intersected with the plain coefficient
     *    hull. This covers the whole continuous G over the box; it is not a
     *    maximum over corners, centers or any finite sample set. Its norm
     *    lower bound is only accepted when strictly positive.
     *
     * 4. The unit normal n = G/||G|| is enclosed two ways and the boxes
     *    intersected: the direct interval quotient, and a centered Lipschitz
     *    ball around the exact center normal, whose radius
     *    (||G_u|| h_u + ||G_v|| h_v) / r_min follows from
     *    dn = (I - n n^T) dG / r with ||I - n n^T||_2 = 1. Point normal
     *    derivatives use the standard radius/normal differentiation
     *    identities at the center.
     *
     * 5. The error F = C - T is bounded with a second-order centered Taylor
     *    form: the exact F and its Jacobian at the box center plus a
     *    quadratic remainder built from interval Hessian boxes of F, again
     *    intersected with the convex-hull coefficient enclosure. Using the
     *    point Jacobian makes the remainder O(h^2), so the bound tightens
     *    quadratically under subdivision. The norm bound is the outward-
     *    rounded interval norm of the resulting vector box.
     *
     * 6. Subdivision bisects the longest parameter side and only tightens
     *    the proof; the candidate is never modified. The leaf list always
     *    partitions [0,1]^2 with pairwise disjoint interiors, and the check
     *    stops with @c unresolved as soon as the next split would exceed
     *    @p max_boxes.
     *
     * @param source     Source surface S: 3D, degrees 2 or 3 on both axes,
     *                   open clamped unit domain, at least C2 in the interior,
     *                   strictly positive finite weights, finite coordinates.
     * @param candidate  Candidate surface C with the same structural rules.
     * @param distance   Offset distance d, |d| <= 0.25. Positive d points
     *                   along S_u x S_v.
     * @param tolerance  Allowed error upper bound, in [1e-5, 1e-2].
     * @param max_boxes  Maximum number of leaf boxes in one check, >= 1.
     *                   Defaults to 4096.
     * @return OffsetCertification with the partition and per-box bounds.
     */
    [[nodiscard]] OffsetCertification CertifyNormalOffset(const Surface& source,
                                                          const Surface& candidate,
                                                          Numeric distance,
                                                          Numeric tolerance,
                                                          int max_boxes = 4096);
}
