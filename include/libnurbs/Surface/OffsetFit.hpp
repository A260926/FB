#pragma once

#include <vector>
#include <libnurbs/Core/Typedefs.hpp>
#include <libnurbs/Surface/Surface.hpp>
#include <libnurbs/Algorithm/OffsetBounds.hpp>

namespace libnurbs
{
    /**
     * @brief Terminal state of a normal-offset fit.
     *
     * - @c success: an owning bicubic Surface was produced and the
     *   independent CertifyNormalOffset check passed for it. The surface is
     *   only published in this state.
     * - @c budget_exhausted: the control-point budget (or the certification
     *   leaf-box budget) was reached before a certified fit existed. No
     *   half-finished surface is returned.
     * - @c invalid_input: an illegal source structure, non-positive weight,
     *   non-finite parameter or illegal budget was detected before any
     *   kernel assertion could fire.
     */
    enum class OffsetFitStatus
    {
        success,
        budget_exhausted,
        invalid_input,
    };

    /**
     * @brief User options for FitNormalOffset.
     */
    struct OffsetOptions
    {
        /// Signed offset distance d. Positive d follows S_u x S_v. |d| <= 0.25.
        Numeric distance{0};

        /// Certification tolerance in [1e-5, 1e-2].
        Numeric tolerance{1e-3};

        /// Hard limit on the total number of control points of the published
        /// bicubic surface (UCount * VCount). Default 256.
        int max_control_points{256};

        /// Maximum number of leaf boxes handed to one independent
        /// certification run. Default 4096.
        int max_boxes{4096};
    };

    /**
     * @brief Snapshot of one refinement iteration.
     *
     * A record is only valid for the surface of its own iteration: after a
     * degree or geometry change every residual and enclosure is recomputed
     * from scratch and stale figures are never reused as evidence.
     */
    struct OffsetFitIteration
    {
        int KnotCountU{0};          ///< Full knot-vector length on U.
        int KnotCountV{0};          ///< Full knot-vector length on V.
        int ControlPointCount{0};   ///< UCount * VCount after the refit.
        Numeric SampleResidual{0};  ///< Max ||C - T|| over the fit samples.
        Numeric ContinuousBound{0}; ///< Worst certified error upper bound,
                                    ///< or infinity when not certified.
        Numeric CrossLowerBound{0}; ///< Worst cross-product lower bound.
        bool Certified{false};      ///< Whether the independent check passed.
        int BoxesUsed{0};           ///< Leaf boxes consumed by the check.
    };

    /**
     * @brief Result of FitNormalOffset.
     */
    struct OffsetFitResult
    {
        OffsetFitStatus Status{OffsetFitStatus::invalid_input};

        /// Owning fitted surface. Defined only when Status == success.
        /// It is always an open clamped, unit-domain, bicubic (degree 3 x 3)
        /// non-rational surface (all weights equal to 1) whose interior
        /// knot multiplicities are at most 2.
        Surface Fit{};

        /// Per-iteration records: knot counts, control-point counts, the
        /// sampling residual and the continuous enclosure of that same
        /// surface.
        std::vector<OffsetFitIteration> Iterations{};

        /// Final independent certification (valid on success; otherwise the
        /// last attempted check, which never released a surface).
        OffsetCertification Certification{};

        [[nodiscard]] bool Success() const
        {
            return Status == OffsetFitStatus::success;
        }
    };

    /**
     * @brief Fit a bicubic NURBS surface to the normal offset
     *        T(u,v) = S(u,v) + d * normalize(S_u x S_v).
     *
     * Algorithm:
     *
     * 1. The source is validated explicitly (3D, degrees 2 or 3, open
     *    clamped unit knot vectors, interior continuity >= C2, strictly
     *    positive finite weights, finite coordinates, knot spacing and
     *    option ranges) before any legacy kernel routine is entered.
     *
     * 2. The fit starts from the coarsest bicubic basis: clamped uniform
     *    knot vectors with no interior knots, i.e. a 4 x 4 control net. All
     *    control coefficients are solved for in a genuine tensor-product
     *    least-squares system. The one-dimensional design matrices are
     *    factored with Householder QR (avoiding the squared condition number
     *    of normal equations) and the two triangular factors recover every
     *    coefficient from the exact offset target T on an oversampled
     *    parameter grid; control points are never merely shifted, and
     *    independent patch coefficients are never stitched into a net.
     *
     * 3. Each iteration records knot counts, control-point count, sampling
     *    residual and the continuous enclosure of that surface. When the
     *    residual is too large, the clamped uniform mesh grows by one span
     *    on the axis whose sampled residual and intrinsic curvature
     *    complexity is largest (the other axis follows only when it needs
     *    to), so refinement is genuinely anisotropic and an extruded surface
     *    accumulates knots only along its curved axis. Every interior knot
     *    keeps multiplicity 1 (the published limit is 2). The full system is
     *    re-solved on the refined space and the new surface is re-checked
     *    from scratch. The maximal dense net is never built up front.
     *
     * 4. The owning Surface is published only after CertifyNormalOffset
     *    returns @c certified. If the next refinement would exceed
     *    @c max_control_points, or a certification run exhausts
     *    @c max_boxes before the tolerance is met, the result is
     *    @c budget_exhausted without a surface.
     *
     * The source surface is never modified and no fit attempt mutates it.
     *
     * @param source  Valid source surface (see validation above).
     * @param options Offset distance, tolerance and budgets.
     */
    [[nodiscard]] OffsetFitResult FitNormalOffset(const Surface& source,
                                                  const OffsetOptions& options);
}
