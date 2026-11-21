#include "libnurbs/Surface/OffsetFit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

#include "libnurbs/Algorithm/OffsetBounds.hpp"
#include "libnurbs/Basis/BSplineBasis.hpp"
#include "libnurbs/Detail/OffsetDetail.hpp"

namespace libnurbs
{
    using namespace offset_detail;

    namespace
    {
        /// Exact offset target T(u,v) = S + d * normalize(S_u x S_v).
        Vec3 OffsetTarget(const Surface& source, Numeric u, Numeric v, Numeric distance)
        {
            const Grid<Vec3> jet = source.EvaluateAll(u, v, 1, 1);
            const Vec3 s = jet.Get(0, 0);
            const Vec3 su = jet.Get(1, 0);
            const Vec3 sv = jet.Get(0, 1);
            const Vec3 normal = su.cross(sv).normalized();
            return s + distance * normal;
        }

        /// All non-vanishing B-spline values at @p x for a clamped vector,
        /// expanded to full length @p control_count (zeros elsewhere).
        VecX FullBasisValues(int degree, const std::vector<Numeric>& knots,
                             int control_count, Numeric x)
        {
            const int span = KnotVector{knots}.FindSpanIndex(degree, x);
            const VecX local = BSplineBasis::Evaluate(degree, knots, span, x);
            VecX result = VecX::Zero(control_count);
            for (int i = 0; i <= degree; ++i)
                result[span - degree + i] = local[i];
            return result;
        }

        /**
         * @brief Solve one genuine tensor-product least-squares system for
         *        all bicubic control coefficients.
         *
         * C(u,v) = sum P_ij N_i(u) M_j(v) is fitted to the sampled offset
         * target on a uniform parameter grid. The one-dimensional design
         * matrices use Householder QR; with A = Q_a R_a, B = Q_b R_b the
         * problem R_a P_c R_b^T = Q_a^T T_c Q_b is solved by two triangular
         * systems per Cartesian coordinate. Every coefficient comes out of
         * the solver; nothing is stitched together from independent patches
         * and no old control point is reused.
         */
        ControlPointGrid SolveLeastSquares(const Surface& source,
                                           const std::vector<Numeric>& knots_u,
                                           const std::vector<Numeric>& knots_v,
                                           Numeric distance,
                                           int sample_u, int sample_v)
        {
            const int n_u = static_cast<int>(knots_u.size()) - 4;
            const int n_v = static_cast<int>(knots_v.size()) - 4;

            MatX a = MatX::Zero(sample_u, n_u);
            MatX b = MatX::Zero(sample_v, n_v);
            for (int row = 0; row < sample_u; ++row)
            {
                const Numeric u = static_cast<Numeric>(row) / static_cast<Numeric>(sample_u - 1);
                a.row(row) = FullBasisValues(3, knots_u, n_u, u);
            }
            for (int row = 0; row < sample_v; ++row)
            {
                const Numeric v = static_cast<Numeric>(row) / static_cast<Numeric>(sample_v - 1);
                b.row(row) = FullBasisValues(3, knots_v, n_v, v);
            }

            // One target matrix per Cartesian coordinate: target_c(p, q) is
            // the offset target coordinate c at sample (u_p, v_q).
            std::array<MatX, 3> target{
                MatX::Zero(sample_u, sample_v),
                MatX::Zero(sample_u, sample_v),
                MatX::Zero(sample_u, sample_v)};
            for (int row_v = 0; row_v < sample_v; ++row_v)
            {
                const Numeric v = static_cast<Numeric>(row_v) / static_cast<Numeric>(sample_v - 1);
                for (int row_u = 0; row_u < sample_u; ++row_u)
                {
                    const Numeric u = static_cast<Numeric>(row_u) / static_cast<Numeric>(sample_u - 1);
                    const Vec3 t = OffsetTarget(source, u, v, distance);
                    target[0](row_u, row_v) = t.x();
                    target[1](row_u, row_v) = t.y();
                    target[2](row_u, row_v) = t.z();
                }
            }

            // QR factorization of each one-dimensional design matrix. A
            // direct QR solve avoids squaring the condition number the way
            // normal equations do, which matters once knot spans become
            // short. Thin Q factors are extracted explicitly.
            Eigen::HouseholderQR<MatX> qr_a(a);
            Eigen::HouseholderQR<MatX> qr_b(b);
            const MatX q_a = qr_a.householderQ() * MatX::Identity(sample_u, n_u);
            const MatX q_b = qr_b.householderQ() * MatX::Identity(sample_v, n_v);
            const MatX r_a = qr_a.matrixQR().topRows(n_u).triangularView<Eigen::Upper>();
            const MatX r_b = qr_b.matrixQR().topRows(n_v).triangularView<Eigen::Upper>();

            // Model T_c ~= A P_c B^T = (Q_a R_a) P_c (Q_b R_b)^T, hence
            // R_a P_c R_b^T = Q_a^T T_c Q_b. Two triangular solves recover
            // every control coefficient directly from the least-squares
            // problem; nothing is stitched from independent patches.
            ControlPointGrid grid(n_u, n_v);
            for (int c = 0; c < 3; ++c)
            {
                const MatX z_matrix = q_a.transpose() * target[c] * q_b; // n_u x n_v
                // Step 1: R_a X = Z with X = P_c R_b^T.
                const MatX x_matrix = r_a.triangularView<Eigen::Upper>().solve(z_matrix);
                // Step 2: X^T = R_b P_c^T.
                const MatX p_transposed =
                    r_b.triangularView<Eigen::Upper>().solve(x_matrix.transpose());
                for (int j = 0; j < n_v; ++j)
                    for (int i = 0; i < n_u; ++i)
                        grid.Get(i, j)[c] = p_transposed(j, i);
            }
            for (int j = 0; j < n_v; ++j)
                for (int i = 0; i < n_u; ++i)
                    grid.Get(i, j).w() = 1.0;
            return grid;
        }

        /// Dense, independent sampling residual (not used as proof, only as
        /// an iteration diagnostic and a refinement guide).
        Numeric SampleResidual(const Surface& source, const Surface& candidate,
                               Numeric distance, int grid_count,
                               std::vector<Numeric>* span_error_u = nullptr,
                               std::vector<Numeric>* span_error_v = nullptr)
        {
            Numeric worst = 0.0;
            if (span_error_u) span_error_u->clear();
            if (span_error_v) span_error_v->clear();
            for (int j = 0; j < grid_count; ++j)
            {
                const Numeric v = static_cast<Numeric>(j) / static_cast<Numeric>(grid_count - 1);
                for (int i = 0; i < grid_count; ++i)
                {
                    const Numeric u = static_cast<Numeric>(i) / static_cast<Numeric>(grid_count - 1);
                    const Numeric error = (candidate.Evaluate(u, v)
                                           - OffsetTarget(source, u, v, distance)).norm();
                    worst = std::max(worst, error);
                    if (span_error_u)
                    {
                        if (static_cast<int>(span_error_u->size()) <= i)
                            span_error_u->resize(i + 1, 0.0);
                        (*span_error_u)[i] = std::max((*span_error_u)[i], error);
                    }
                    if (span_error_v)
                    {
                        if (static_cast<int>(span_error_v->size()) <= j)
                            span_error_v->resize(j + 1, 0.0);
                        (*span_error_v)[j] = std::max((*span_error_v)[j], error);
                    }
                }
            }
            return worst;
        }

        /// Clamped uniform degree-3 knot vector with @p segments interior
        /// spans (segments-1 distinct interior knots, multiplicity 1).
        std::vector<Numeric> UniformBicubicKnots(int segments)
        {
            std::vector<Numeric> knots;
            knots.reserve(segments + 7);
            for (int i = 0; i < 4; ++i) knots.push_back(0.0);
            for (int k = 1; k < segments; ++k)
                knots.push_back(static_cast<Numeric>(k) / static_cast<Numeric>(segments));
            for (int i = 0; i < 4; ++i) knots.push_back(1.0);
            return knots;
        }

        /**
         * @brief Directional complexity of the offset target, used to drive
         *        anisotropic refinement. The mean magnitude of the source
         *        second parametric derivatives measures how much resolution
         *        each axis actually needs (the offset normal inherits the
         *        same curvature scaling), so a surface that is straight in V
         *        accumulates knots only on U.
         */
        std::pair<Numeric, Numeric> TargetAxisComplexity(const Surface& source,
                                                         Numeric distance)
        {
            constexpr int kProbe = 17;
            Numeric acc_u = 0.0;
            Numeric acc_v = 0.0;
            for (int j = 0; j < kProbe; ++j)
            {
                const Numeric v = static_cast<Numeric>(j) / static_cast<Numeric>(kProbe - 1);
                for (int i = 0; i < kProbe; ++i)
                {
                    const Numeric u = static_cast<Numeric>(i) / static_cast<Numeric>(kProbe - 1);
                    const Grid<Vec3> jet = source.EvaluateAll(u, v, 2, 2);
                    const Vec3 suu = jet.Get(2, 0);
                    const Vec3 svv = jet.Get(0, 2);
                    // Normal curvature contribution of the offset layer.
                    const Vec3 su = jet.Get(1, 0);
                    const Vec3 sv = jet.Get(0, 1);
                    const Numeric cross_length = su.cross(sv).norm();
                    const Numeric weight = 1.0 + std::abs(distance) / std::max(cross_length, 1e-12);
                    acc_u += suu.norm() * weight;
                    acc_v += svv.norm() * weight;
                }
            }
            const Numeric normalizer = static_cast<Numeric>(kProbe * kProbe);
            return {acc_u / normalizer, acc_v / normalizer};
        }

        Surface BuildCandidate(const std::vector<Numeric>& knots_u,
                               const std::vector<Numeric>& knots_v,
                               ControlPointGrid grid)
        {
            Surface candidate{};
            candidate.DegreeU = 3;
            candidate.DegreeV = 3;
            candidate.KnotsU = KnotVector{knots_u};
            candidate.KnotsV = KnotVector{knots_v};
            candidate.ControlPoints = std::move(grid);
            return candidate;
        }
    }

    OffsetFitResult FitNormalOffset(const Surface& source, const OffsetOptions& options)
    {
        OffsetFitResult result{};

        // Explicit rejection before any legacy kernel routine can assert.
        std::string reason;
        if (!ValidateSurfaceStructure(source, true, &reason))
        {
            result.Status = OffsetFitStatus::invalid_input;
            return result;
        }
        if (!std::isfinite(options.distance) || std::abs(options.distance) > 0.25 ||
            !std::isfinite(options.tolerance) || options.tolerance < 1e-5 ||
            options.tolerance > 1e-2 ||
            options.max_control_points < 16 || options.max_control_points > 10'000 ||
            options.max_boxes < 1 || options.max_boxes > 1'000'000)
        {
            result.Status = OffsetFitStatus::invalid_input;
            return result;
        }

        // Uniform meshes start with a single span per axis and grow only on
        // the axis the residual actually needs.
        int segments_u = 1;
        int segments_v = 1;
        const auto [complexity_u, complexity_v] = TargetAxisComplexity(source, options.distance);

        constexpr int kMaxIterations = 48;
        Numeric previous_residual = std::numeric_limits<Numeric>::infinity();

        for (int iteration = 0; iteration < kMaxIterations; ++iteration)
        {
            std::vector<Numeric> knots_u = UniformBicubicKnots(segments_u);
            std::vector<Numeric> knots_v = UniformBicubicKnots(segments_v);
            const int n_u = segments_u + 3;
            const int n_v = segments_v + 3;
            if (n_u * n_v > options.max_control_points)
            {
                result.Status = OffsetFitStatus::budget_exhausted;
                return result;
            }

            // Several samples per knot span keep the least-squares problem
            // well posed even on fine meshes and prevent inter-sample
            // oscillation, while staying bounded.
            const int sample_u = std::clamp(6 * segments_u + 2, 10, 96);
            const int sample_v = std::clamp(6 * segments_v + 2, 10, 96);

            ControlPointGrid grid = SolveLeastSquares(source, knots_u, knots_v,
                                                      options.distance,
                                                      sample_u, sample_v);
            Surface candidate = BuildCandidate(knots_u, knots_v, std::move(grid));

            // Independent dense diagnostic grid (evidence for this surface
            // only; never reused after a geometry change).
            constexpr int kDiagnosticGrid = 41;
            std::vector<Numeric> span_errors_u;
            std::vector<Numeric> span_errors_v;
            const Numeric residual = SampleResidual(source, candidate, options.distance,
                                                    kDiagnosticGrid,
                                                    &span_errors_u, &span_errors_v);

            OffsetCertification certification = CertifyNormalOffset(
                source, candidate, options.distance, options.tolerance, options.max_boxes);

            OffsetFitIteration record{};
            record.KnotCountU = static_cast<int>(knots_u.size());
            record.KnotCountV = static_cast<int>(knots_v.size());
            record.ControlPointCount = n_u * n_v;
            record.SampleResidual = residual;
            record.ContinuousBound = certification.WorstErrorUpperBound;
            record.CrossLowerBound = certification.WorstCrossLowerBound;
            record.Certified = certification.IsCertified();
            record.BoxesUsed = certification.BoxesUsed;
            result.Iterations.push_back(record);
            result.Certification = certification;

            if (certification.Status == OffsetCertStatus::invalid_input)
            {
                // Structural validation already passed for the source; an
                // invalid status here indicates an internal inconsistency.
                result.Status = OffsetFitStatus::invalid_input;
                return result;
            }

            if (certification.IsCertified())
            {
                result.Status = OffsetFitStatus::success;
                result.Fit = std::move(candidate); // owning surface published only now
                return result;
            }

            // Stop honestly when the fit itself stops improving (the dense
            // residual is recomputed for every new surface, so it never
            // carries stale evidence) or when no legal mesh remains inside
            // the control-point budget. A stall visible only in the
            // certification bound is treated as a leaf-box limitation and
            // does not abort refinement on its own.
            const bool no_progress = residual >= previous_residual * (1.0 - 1e-12)
                                     && iteration > 0;
            previous_residual = residual;
            const bool budget_full = (n_u + 1) * (n_v + 1) > options.max_control_points
                                     && n_u * (n_v + 1) > options.max_control_points;
            if (no_progress || budget_full)
            {
                result.Status = OffsetFitStatus::budget_exhausted;
                return result;
            }

            // Choose the refinement axis from a score combining the sampled
            // residual of that direction with its intrinsic curvature
            // complexity. An extruded surface (zero curvature along one
            // axis) then accumulates knots only on the axis that actually
            // needs them, giving genuinely asymmetric refinement.
            const Numeric mean_u = span_errors_u.empty() ? 0.0
                                   : std::accumulate(span_errors_u.begin(), span_errors_u.end(), 0.0)
                                     / static_cast<Numeric>(span_errors_u.size());
            const Numeric mean_v = span_errors_v.empty() ? 0.0
                                   : std::accumulate(span_errors_v.begin(), span_errors_v.end(), 0.0)
                                     / static_cast<Numeric>(span_errors_v.size());
            const Numeric complexity_scale = std::max({complexity_u, complexity_v,
                                                        std::numeric_limits<Numeric>::min()});
            const Numeric score_u = mean_u * (0.25 + complexity_u / complexity_scale);
            const Numeric score_v = mean_v * (0.25 + complexity_v / complexity_scale);

            auto budget_allows = [&](int next_segments_u, int next_segments_v)
            {
                return (next_segments_u + 3) * (next_segments_v + 3) <= options.max_control_points;
            };

            const bool refine_u = score_u >= score_v;

            if (refine_u)
            {
                if (!budget_allows(segments_u + 1, segments_v))
                {
                    result.Status = OffsetFitStatus::budget_exhausted;
                    return result;
                }
                ++segments_u;
                // Follow up on the second axis only when its score is
                // comparable; never pre-build knots on a straight axis.
                if (budget_allows(segments_u, segments_v + 1) && score_v >= 0.5 * score_u)
                {
                    ++segments_v;
                }
            }
            else
            {
                if (!budget_allows(segments_u, segments_v + 1))
                {
                    result.Status = OffsetFitStatus::budget_exhausted;
                    return result;
                }
                ++segments_v;
                if (budget_allows(segments_u + 1, segments_v) && score_u >= 0.5 * score_v)
                {
                    ++segments_u;
                }
            }
        }

        result.Status = OffsetFitStatus::budget_exhausted;
        return result;
    }
}
