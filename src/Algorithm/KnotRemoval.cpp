#include "libnurbs/Algorithm/KnotRemoval.hpp"
#include "libnurbs/Core/KnotVector.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    using namespace libnurbs;

    // Multiplicity computed with an exact comparison against the requested
    // value: the callers only remove knots that are literally present in the
    // knot vector, so a tolerance based multiplicity would be ambiguous.
    int ExactMultiplicity(const std::vector<Numeric>& knots, Numeric value)
    {
        int count = 0;
        for (Numeric knot: knots)
        {
            if (knot == value) ++count;
        }
        return count;
    }

    // Standard knot span search on a plain knot vector.  `knots` has
    // points.size() + degree + 1 entries.
    int FindSpan(const std::vector<Numeric>& knots, int degree, Numeric value)
    {
        const int last_span = static_cast<int>(knots.size()) - degree - 2;
        if (value >= knots.back()) return last_span;
        if (value <= knots.front()) return degree;
        int low  = degree;
        int high = static_cast<int>(knots.size()) - degree - 1;
        int mid  = (low + high) / 2;
        while (value < knots[mid] || value >= knots[mid + 1])
        {
            if (value < knots[mid]) high = mid;
            else low = mid;
            mid = (low + high) / 2;
        }
        return mid;
    }

    // One-shot knot insertion, working purely in homogeneous coordinates and
    // mirroring the public Curve::InsertKnot algorithm.
    void InsertKnotRawSingle(const std::vector<Numeric>& knots,
                                  const std::vector<Vec4>& points,
                                  int degree,
                                  Numeric knot_value,
                                  std::vector<Numeric>& out_knots,
                                  std::vector<Vec4>& out_points)
    {
        const int span = FindSpan(knots, degree, knot_value);

        out_points = points;
        out_points.insert(out_points.begin() + span, points[span]);
        Vec4 point_last = points[span - degree];
        for (int i = span - degree + 1; i <= span; ++i)
        {
            const Numeric denominator = knots[i + degree] - knots[i];
            const Numeric alpha       = denominator == 0.0 ? 0.0 : (knot_value - knots[i]) / denominator;
            Vec4 point_new            = alpha * out_points[i] + (1.0 - alpha) * point_last;
            point_last                = out_points[i];
            out_points[i]             = point_new;
        }

        out_knots = knots;
        out_knots.insert(out_knots.begin() + span + 1, knot_value);
    }

    Numeric Distance3(const Vec4& lhs, const Vec4& rhs)
    {
        return (lhs.head<3>() - rhs.head<3>()).norm();
    }

    bool IsFiniteVec(const Vec4& v)
    {
        return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z()) && std::isfinite(v.w());
    }
}

namespace libnurbs
{
    namespace detail
    {
        void InsertKnotRaw(const std::vector<Numeric>& knots,
                           const std::vector<Vec4>& points,
                           int degree,
                           Numeric knot_value,
                           int times,
                           std::vector<Numeric>& out_knots,
                           std::vector<Vec4>& out_points)
        {
            out_knots  = knots;
            out_points = points;
            for (int i = 0; i < times; ++i)
            {
                std::vector<Numeric> next_knots;
                std::vector<Vec4> next_points;
                InsertKnotRawSingle(out_knots, out_points, degree, knot_value, next_knots, next_points);
                out_knots  = std::move(next_knots);
                out_points = std::move(next_points);
            }
        }

        bool RemoveSingleKnotCopy(const std::vector<Numeric>& knots,
                                  const std::vector<Vec4>& points,
                                  int degree,
                                  Numeric knot_remove,
                                  std::vector<Numeric>& out_knots,
                                  std::vector<Vec4>& out_points)
        {
            const int multiplicity = ExactMultiplicity(knots, knot_remove);
            if (multiplicity <= 0) return false;
            if (points.size() != knots.size() - degree - 1) return false;

            const int span   = FindSpan(knots, degree, knot_remove);
            const int n      = static_cast<int>(points.size()) - 1;
            const int m      = n + degree + 1;
            const int order  = degree + 1;
            const int last   = span - multiplicity;
            const int first  = span - degree;
            const int offset = first - 1;

            if (offset < 0 || last + 1 >= static_cast<int>(points.size())) return false;

            std::vector<Vec4> temp(2 * degree + 1);
            temp[0]                  = points[offset];
            temp[last + 1 - offset] = points[last + 1];

            int i  = first;
            int j  = last;
            int ii = 1;
            int jj = last - offset;

            // Solve for the replacement control points of a single removal.
            while (j - i > 0)
            {
                const Numeric denominator_i = knots[i + order] - knots[i];
                const Numeric denominator_j = knots[j + order] - knots[j];
                if (denominator_i == 0.0 || denominator_j == 0.0) return false;

                const Numeric alpha_i = (knot_remove - knots[i]) / denominator_i;
                const Numeric alpha_j = (knot_remove - knots[j]) / denominator_j;
                if (alpha_i == 0.0 || alpha_j == 1.0) return false;

                temp[ii] = (points[i] - (1.0 - alpha_i) * temp[ii - 1]) / alpha_i;
                temp[jj] = (points[j] - alpha_j * temp[jj + 1]) / (1.0 - alpha_j);

                ++i;
                ++ii;
                --j;
                --jj;
            }

            // Consistency check at the meeting point.  This is the classical
            // single-knot error indicator.
            Numeric discrepancy;
            if (j - i < 0)
            {
                discrepancy = Distance3(temp[ii - 1], temp[jj + 1]);
            }
            else
            {
                const Numeric denominator = knots[i + order] - knots[i];
                if (denominator == 0.0) return false;
                const Numeric alpha = (knot_remove - knots[i]) / denominator;
                const Vec4 predicted =
                    alpha * temp[ii + 1] + (1.0 - alpha) * temp[ii - 1];
                discrepancy = Distance3(predicted, points[i]);
            }
            if (!std::isfinite(discrepancy)) return false;

            // Materialise the candidate control points.
            std::vector<Vec4> candidate = points;
            i                           = first;
            j                           = last;
            while (j - i > 0)
            {
                candidate[i] = temp[i - offset];
                candidate[j] = temp[j - offset];
                ++i;
                --j;
            }

            // Compact the control point list (single copy).
            int target = (2 * span - multiplicity - degree) / 2;
            int source = target;
            for (int k = source + 1; k <= n; ++k)
            {
                candidate[target] = candidate[k];
                ++target;
            }
            candidate.resize(candidate.size() - 1);

            // Compact the knot vector (single copy).
            std::vector<Numeric> candidate_knots = knots;
            for (int k = span + 1; k <= m; ++k)
            {
                candidate_knots[k - 1] = candidate_knots[k];
            }
            candidate_knots.resize(candidate_knots.size() - 1);

            out_knots  = std::move(candidate_knots);
            out_points = std::move(candidate);
            return true;
        }

        Numeric GeometryBound(const std::vector<std::vector<Vec4>>& original_rows,
                              const std::vector<std::vector<Vec4>>& candidate_rows)
        {
            if (original_rows.empty() || original_rows.size() != candidate_rows.size()) return std::numeric_limits<Numeric>::infinity();

            // A single anchor taken from the original control net.  Everything is
            // expressed relative to it, which makes the bound invariant under
            // translation and rigid motion; the common weight scale cancels out.
            const Vec4 anchor_h = original_rows.front().front();
            if (!IsFiniteVec(anchor_h) || anchor_h.w() <= 0.0) return std::numeric_limits<Numeric>::infinity();
            const Vec3 anchor = anchor_h.head<3>() / anchor_h.w();

            Numeric max_control_difference = 0.0;
            Numeric max_weight_difference  = 0.0;
            Numeric max_candidate_offset   = 0.0;

            for (size_t r = 0; r < original_rows.size(); ++r)
            {
                const auto& original  = original_rows[r];
                const auto& candidate = candidate_rows[r];
                if (original.size() != candidate.size()) return std::numeric_limits<Numeric>::infinity();

                for (size_t k = 0; k < original.size(); ++k)
                {
                    const Vec4& a = original[k];
                    const Vec4& c = candidate[k];
                    if (!IsFiniteVec(a) || !IsFiniteVec(c)) return std::numeric_limits<Numeric>::infinity();
                    if (a.w() <= 0.0 || c.w() <= 0.0) return std::numeric_limits<Numeric>::infinity();

                    max_weight_difference = std::max(max_weight_difference, std::abs(a.w() - c.w()));

                    const Vec3 a_offset = a.head<3>() - a.w() * anchor;
                    const Vec3 c_offset = c.head<3>() - c.w() * anchor;
                    max_control_difference = std::max(max_control_difference, (a_offset - c_offset).norm());
                    max_candidate_offset = std::max(max_candidate_offset, c_offset.norm());
                }
            }

            // The bound uses the smallest original weight, so make sure that is
            // exactly the minimum over the source net.
            Numeric original_min = std::numeric_limits<Numeric>::infinity();
            Numeric candidate_min = std::numeric_limits<Numeric>::infinity();
            for (const auto& row: original_rows)
            {
                for (const Vec4& a: row) original_min = std::min(original_min, a.w());
            }
            for (const auto& row: candidate_rows)
            {
                for (const Vec4& c: row) candidate_min = std::min(candidate_min, c.w());
            }
            if (original_min <= 0.0 || candidate_min <= 0.0) return std::numeric_limits<Numeric>::infinity();

            // Rational distance bound.  Write the original and candidate
            // homogeneous nets relative to one common anchor a:
            //   A_i = Ws_i * a + A_off_i,    B_i = Wc_i * a + B_off_i.
            // For any parameter the numerator nets are N = sum N_i A_off_i and
            // M = sum N_i B_off_i (N_i >= 0, sum N_i = 1), while the denominators
            // are Ws = sum N_i Ws_i and Wc = sum N_i Wc_i.  Then
            //   S - C = (N - M)/Ws + M * (Wc - Ws) / (Ws * Wc).
            // Both nets share the same basis, so |N - M| <= max_i |A_off_i -
            // B_off_i|, |M| <= max_i |B_off_i| and |Wc - Ws| <= max_i |Wc_i -
            // Ws_i|.  With positive weights, Ws >= WsMin and Wc >= WcMin, so
            //   |S - C| <= dA / WsMin + Bmax * dW / (WsMin * WcMin)
            // uniformly over the whole parameter domain.  This uses the
            // controls of every row, hence also the rational denominator that
            // couples all rows of a tensor-product surface.
            // (M is bounded by the candidate offset net, which is the one that
            // actually enters S - C; using the candidate here keeps the bound
            // tight for approximate removals.)
            const Numeric first_term  = max_control_difference / original_min;
            const Numeric second_term = max_candidate_offset * max_weight_difference /
                                        (original_min * candidate_min);
            const Numeric bound = first_term + second_term;
            if (!std::isfinite(bound)) return std::numeric_limits<Numeric>::infinity();
            return bound;
        }

        int RemoveKnotFromRows(const std::vector<Numeric>& knots,
                               const std::vector<std::vector<Vec4>>& rows,
                               int degree,
                               Numeric knot_remove,
                               int times,
                               Numeric tolerance,
                               std::vector<Numeric>& out_knots,
                               std::vector<std::vector<Vec4>>& out_rows)
        {
            if (rows.empty() || times <= 0) return 0;
            const int multiplicity = ExactMultiplicity(knots, knot_remove);
            if (multiplicity <= 0) return 0;
            const int request = std::min(times, multiplicity);
            if (request <= 0) return 0;

            // Convert all rows to homogeneous coordinates once.
            std::vector<std::vector<Vec4>> homogeneous_rows(rows.size());
            for (size_t r = 0; r < rows.size(); ++r)
            {
                homogeneous_rows[r].resize(rows[r].size());
                for (size_t k = 0; k < rows[r].size(); ++k)
                {
                    const Vec4& point = rows[r][k];
                    if (!IsFiniteVec(point) || point.w() <= 0.0) return 0;
                    homogeneous_rows[r][k] = ToHomo(point);
                }
            }

            // Sequential single-copy candidate for each row.  Every candidate
            // stays on the same knot basis as the others at the same index.
            std::vector<std::vector<std::vector<Vec4>>> candidates(rows.size());
            std::vector<std::vector<std::vector<Numeric>>> candidate_knots(rows.size());
            for (size_t r = 0; r < rows.size(); ++r)
            {
                auto& list        = candidates[r];
                auto& knots_list  = candidate_knots[r];
                list.push_back(homogeneous_rows[r]);
                knots_list.push_back(knots);

                std::vector<Numeric> current_knots = knots;
                std::vector<Vec4> current_points   = homogeneous_rows[r];
                for (int step = 1; step <= request; ++step)
                {
                    std::vector<Numeric> next_knots;
                    std::vector<Vec4> next_points;
                    if (!RemoveSingleKnotCopy(current_knots, current_points, degree, knot_remove, next_knots, next_points))
                    {
                        break;
                    }
                    bool positive = true;
                    for (const Vec4& point: next_points)
                    {
                        if (!IsFiniteVec(point) || point.w() <= 0.0)
                        {
                            positive = false;
                            break;
                        }
                    }
                    if (!positive) break;

                    list.push_back(next_points);
                    knots_list.push_back(next_knots);
                    current_knots = std::move(next_knots);
                    current_points = std::move(next_points);
                }
            }

            int common_capacity = request;
            for (const auto& list: candidates)
            {
                common_capacity = std::min(common_capacity, static_cast<int>(list.size()) - 1);
            }
            if (common_capacity <= 0) return 0;

            // Try the largest common removal first; it is the most useful
            // result and the bound is evaluated against the original input.
            for (int step = common_capacity; step >= 1; --step)
            {
                // All rows must agree on the knot basis after `step` removals.
                const std::vector<Numeric>& reference = candidate_knots.front()[step];
                bool consistent                       = true;
                for (const auto& knots_list: candidate_knots)
                {
                    if (knots_list[step] != reference)
                    {
                        consistent = false;
                        break;
                    }
                }
                if (!consistent) continue;

                // Refine every candidate back onto the common original knot
                // basis so control points can be compared one to one.
                std::vector<std::vector<Vec4>> refined_rows(rows.size());
                for (size_t r = 0; r < rows.size(); ++r)
                {
                    std::vector<Numeric> refined_knots;
                    InsertKnotRaw(reference, candidates[r][step], degree, knot_remove, step,
                                  refined_knots, refined_rows[r]);
                }

                const Numeric bound = GeometryBound(homogeneous_rows, refined_rows);
                if (bound <= tolerance)
                {
                    out_knots = reference;
                    out_rows.resize(rows.size());
                    for (size_t r = 0; r < rows.size(); ++r)
                    {
                        const auto& step_points = candidates[r][step];
                        out_rows[r].resize(step_points.size());
                        for (size_t k = 0; k < step_points.size(); ++k)
                        {
                            out_rows[r][k] = FromHomo(step_points[k]);
                        }
                    }
                    return step;
                }
            }

            return 0;
        }
    }

    int KnotRemoval(KnotVector& knot_vector,
                    std::vector<Vec4>& points,
                    int degree, Numeric knot_remove, int times, Numeric tolerance)
    {
        std::vector<std::vector<Vec4>> rows{points};
        std::vector<Numeric> out_knots;
        std::vector<std::vector<Vec4>> out_rows;

        const int removed = detail::RemoveKnotFromRows(
            knot_vector.Values(), rows, degree, knot_remove, times, tolerance, out_knots, out_rows);

        if (removed <= 0) return 0;

        knot_vector.Values() = out_knots;
        points               = out_rows.front();
        return removed;
    }
}
