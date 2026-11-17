#include "libnurbs/Algorithm/KnotRemoval.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include "libnurbs/Core/KnotVector.hpp"

namespace
{
    using namespace libnurbs;

    // Knot values are stored in [0, 1]; a relative tolerance keeps the
    // "same knot" test stable for values produced by arithmetic.
    constexpr Numeric KNOT_EQUAL_TOL = 1e-12;

    bool SameKnot(Numeric a, Numeric b)
    {
        return std::abs(a - b) <= KNOT_EQUAL_TOL * std::max({Numeric(1), std::abs(a), std::abs(b)});
    }

    bool AllFinite(const Vec4& v)
    {
        return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z()) && std::isfinite(v.w());
    }

    bool SameKnots(const std::vector<Numeric>& a, const std::vector<Numeric>& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (!SameKnot(a[i], b[i])) return false;
        }
        return true;
    }
}

namespace libnurbs::detail
{
    int FindSpan(const std::vector<Numeric>& knots, int degree, Numeric u)
    {
        const int count = static_cast<int>(knots.size());
        if (count < degree + 2) return -1;
        const int last_span = count - degree - 2;
        if (u >= knots.back()) return last_span;
        if (u <= knots.front()) return degree;
        // upper_bound - 1: index of the last knot <= u.
        const auto it = std::upper_bound(knots.begin(), knots.end(), u);
        return static_cast<int>(std::distance(knots.begin(), it)) - 1;
    }

    int CountMultiplicity(const std::vector<Numeric>& knots, Numeric value)
    {
        int count = 0;
        for (Numeric k: knots)
        {
            if (SameKnot(k, value)) ++count;
        }
        return count;
    }

    bool FindExistingKnot(const std::vector<Numeric>& knots, Numeric requested, Numeric& stored_value)
    {
        for (Numeric k: knots)
        {
            if (SameKnot(k, requested))
            {
                stored_value = k;
                return true;
            }
        }
        return false;
    }

    bool RemoveKnotOnce(const std::vector<Numeric>& knots,
                        const std::vector<Vec4>& points,
                        int degree,
                        Numeric knot,
                        std::vector<Numeric>& out_knots,
                        std::vector<Vec4>& out_points)
    {
        const int s = CountMultiplicity(knots, knot);
        if (s < 1) return false;

        const int p = degree;
        const int n = static_cast<int>(points.size()) - 1;
        const int m = n + p + 1;
        if (p < 1 || n < p) return false;
        if (static_cast<int>(knots.size()) != m + 1) return false;

        const int r = FindSpan(knots, p, knot);
        if (r < 0 || r >= static_cast<int>(knots.size())) return false;
        if (!SameKnot(knots[r], knot)) return false;

        const int order = p + 1;
        const int first = r - p;
        const int last = r - s;
        const int off = first - 1;

        // The recurrences read points[first - 1] and points[last + 1], which
        // are only available for interior knots of a clamped vector.
        if (off < 0 || last + 1 > n) return false;
        if (first > last + 1) return false;

        std::vector<Vec4> temp(2 * p + 3, Vec4::Zero());
        const int top = last + 1 - off;
        if (top < 0 || top >= static_cast<int>(temp.size())) return false;

        temp[0] = points[off];
        temp[top] = points[last + 1];

        int i = first;
        int j = last;
        int ii = 1;
        int jj = last - off;
        if (ii >= static_cast<int>(temp.size()) || jj + 1 >= static_cast<int>(temp.size())) return false;

        while (j - i > 0)
        {
            const Numeric di = knots[i + order] - knots[i];
            const Numeric dj = knots[j + order] - knots[j];
            if (!(di > 0) || !(dj > 0)) return false;

            const Numeric alphai = (knot - knots[i]) / di;
            const Numeric alphaj = (knot - knots[j]) / dj;
            if (alphai == 0.0 || alphaj == 1.0) return false;

            temp[ii] = (points[i] - (1.0 - alphai) * temp[ii - 1]) / alphai;
            temp[jj] = (points[j] - alphaj * temp[jj + 1]) / (1.0 - alphaj);
            if (!AllFinite(temp[ii]) || !AllFinite(temp[jj])) return false;

            ++i;
            ++ii;
            --j;
            --jj;
        }

        out_points = points;
        i = first;
        j = last;
        while (j - i > 0)
        {
            out_points[i] = temp[i - off];
            out_points[j] = temp[j - off];
            ++i;
            --j;
        }

        // Deleted index for a single removal (Piegl & Tiller A5.8).
        const int kdel = (2 * r - s - p) / 2;
        if (kdel < 0 || kdel >= static_cast<int>(out_points.size())) return false;
        out_points.erase(out_points.begin() + kdel);

        out_knots = knots;
        if (r < 0 || r >= static_cast<int>(out_knots.size())) return false;
        out_knots.erase(out_knots.begin() + r);

        if (static_cast<int>(out_knots.size()) != static_cast<int>(out_points.size()) + p + 1) return false;
        return true;
    }

    bool InsertKnotOnce(const std::vector<Numeric>& knots,
                        const std::vector<Vec4>& points,
                        int degree,
                        Numeric knot,
                        std::vector<Numeric>& out_knots,
                        std::vector<Vec4>& out_points)
    {
        const int p = degree;
        const int n = static_cast<int>(points.size()) - 1;
        if (p < 1 || n < p) return false;
        if (static_cast<int>(knots.size()) != n + p + 2) return false;

        const int k = FindSpan(knots, p, knot);
        if (k < p || k > n) return false;

        out_knots = knots;
        out_points = points;
        out_knots.insert(out_knots.begin() + k + 1, knot);
        out_points.insert(out_points.begin() + k, points[k]);

        for (int i = k - p + 1; i <= k; ++i)
        {
            const Numeric d = knots[i + p] - knots[i];
            if (!(d > 0)) return false;
            const Numeric alpha = (knot - knots[i]) / d;
            out_points[i] = alpha * points[i] + (1.0 - alpha) * points[i - 1];
            if (!AllFinite(out_points[i])) return false;
        }
        return true;
    }

    Numeric DistanceBound(const std::vector<Vec4>& reference, const std::vector<Vec4>& candidate)
    {
        const Numeric inf = std::numeric_limits<Numeric>::infinity();
        if (reference.empty() || reference.size() != candidate.size()) return inf;

        // Centre on the first reference control point so the estimate is
        // translation invariant.
        const Numeric w0 = reference.front().w();
        if (!(w0 > 0)) return inf;
        const Vec3 c0 = reference.front().head<3>() / w0;

        long double max_centred = 0.0L;  // max |w_i (C_i - c0)|
        long double max_diff    = 0.0L;  // max |a_i - b_i|
        long double max_dw      = 0.0L;  // max |w_i - v_i|
        long double min_w       = std::numeric_limits<long double>::infinity();
        long double min_v       = std::numeric_limits<long double>::infinity();

        for (size_t i = 0; i < reference.size(); ++i)
        {
            const Numeric wr = reference[i].w();
            const Numeric wc = candidate[i].w();
            if (!(wr > 0) || !(wc > 0)) return inf;

            // a = w (C - c0), b = v (D - c0); a - b = (wC - vD) - (w - v) c0.
            const Vec3 a = reference[i].head<3>() - wr * c0;
            const Vec3 diff = a - (candidate[i].head<3>() - wc * c0);
            max_centred = std::max(max_centred, static_cast<long double>(a.norm()));
            max_diff = std::max(max_diff, static_cast<long double>(diff.norm()));
            max_dw = std::max(max_dw, static_cast<long double>(std::abs(wr - wc)));
            min_w = std::min(min_w, static_cast<long double>(wr));
            min_v = std::min(min_v, static_cast<long double>(wc));
        }

        if (!(min_w > 0.0L) || !(min_v > 0.0L)) return inf;

        // With a partition-of-unity basis (also true for tensor products),
        //   A = sum N a, B = sum N b, W = sum N w, V = sum N v,
        //   C - D = (A/W)(V - W)/V + (A - B)/V.
        // Hence |C - D| <= max|a| max|w - v| / (min w * min v)
        //                 + max|a - b| / min v,
        // a bound over the whole parameter domain. The tensor-product net is
        // bounded entrywise, so every control row participates in the
        // rational denominator.
        const long double bound = (max_centred * max_dw) / (min_w * min_v) + max_diff / min_v;
        if (!std::isfinite(bound)) return inf;
        return static_cast<Numeric>(bound);
    }

    bool SameKnotBasis(const std::vector<Numeric>& a, const std::vector<Numeric>& b)
    {
        return SameKnots(a, b);
    }

    Numeric RoundoffSlack(const std::vector<Vec4>& homogeneous_net)
    {
        // Round-off allowance. Knots are stored in [0, 1], so this is an
        // absolute quantity; it is translation and rotation invariant, hence
        // never changes which removals are accepted for a moved or rotated
        // model. It is intentionally tiny compared with any meaningful user
        // tolerance, so a genuinely inexact removal is still rejected.
        Numeric diam = 0.0;
        if (!homogeneous_net.empty() && homogeneous_net.front().w() > 0)
        {
            const Vec3 c0 = homogeneous_net.front().head<3>() / homogeneous_net.front().w();
            for (const auto& p: homogeneous_net)
            {
                if (!(p.w() > 0)) continue;
                diam = std::max(diam, (p.head<3>() / p.w() - c0).norm());
            }
        }
        return 64 * std::numeric_limits<Numeric>::epsilon() * (1.0 + diam);
    }
}

namespace
{
    using namespace libnurbs;

    // Builds the candidate after removing `t` copies from `knots`, then brings
    // it back to the original basis. Returns false when the removal is
    // structurally impossible or the refinement does not recover the basis.
    bool BuildRemovalCandidate(const std::vector<Numeric>& knots,
                               const std::vector<Vec4>& points,
                               int degree,
                               Numeric knot,
                               int t,
                               std::vector<Numeric>& candidate_knots,
                               std::vector<Vec4>& candidate_points,
                               std::vector<Vec4>& refined_points)
    {
        std::vector<Numeric> current_knots = knots;
        std::vector<Vec4> current_points = points;
        for (int q = 0; q < t; ++q)
        {
            std::vector<Numeric> next_knots;
            std::vector<Vec4> next_points;
            if (!detail::RemoveKnotOnce(current_knots, current_points, degree, knot, next_knots, next_points))
            {
                return false;
            }
            current_knots = std::move(next_knots);
            current_points = std::move(next_points);
        }

        std::vector<Numeric> refined_knots = current_knots;
        refined_points = current_points;
        for (int q = 0; q < t; ++q)
        {
            std::vector<Numeric> step_knots;
            std::vector<Vec4> step_points;
            if (!detail::InsertKnotOnce(refined_knots, refined_points, degree, knot, step_knots, step_points))
            {
                return false;
            }
            refined_knots = std::move(step_knots);
            refined_points = std::move(step_points);
        }
        if (!detail::SameKnotBasis(refined_knots, knots) || refined_points.size() != points.size())
        {
            return false;
        }

        candidate_knots = std::move(current_knots);
        candidate_points = std::move(current_points);
        return true;
    }

    // Shared curve-level driver: removes up to `times` copies of `knot`,
    // validating each candidate against the *original* basis by refinement.
    int RemoveKnotFromRow(const std::vector<Numeric>& knots,
                          const std::vector<Vec4>& homogeneous_points,
                          int degree,
                          Numeric knot,
                          int times,
                          Numeric tolerance,
                          std::vector<Numeric>& out_knots,
                          std::vector<Vec4>& out_points)
    {
        const int multiplicity = detail::CountMultiplicity(knots, knot);
        const int limit = std::min(times, multiplicity);
        const Numeric slack = detail::RoundoffSlack(homogeneous_points);
        std::vector<Numeric> current_knots = knots;
        std::vector<Vec4> current_points = homogeneous_points;

        int best = 0;
        for (int t = 1; t <= limit; ++t)
        {
            std::vector<Numeric> candidate_knots;
            std::vector<Vec4> candidate_points;
            std::vector<Vec4> refined_points;
            if (!BuildRemovalCandidate(knots, homogeneous_points, degree, knot, t,
                                       candidate_knots, candidate_points, refined_points))
            {
                break;
            }

            const Numeric bound = detail::DistanceBound(homogeneous_points, refined_points);
            if (!(bound <= tolerance + slack))
            {
                break;
            }

            best = t;
            current_knots = std::move(candidate_knots);
            current_points = std::move(candidate_points);
        }

        if (best == 0) return 0;
        out_knots = std::move(current_knots);
        out_points = std::move(current_points);
        return best;
    }
}

namespace libnurbs
{
    int KnotRemoval(KnotVector& knot_vector,
                    std::vector<Vec4>& points,
                    int degree, Numeric knot_remove, int times, Numeric tolerance)
    {
        if (degree < 1 || times <= 0) return 0;

        auto& knots = knot_vector.Values();
        if (static_cast<int>(points.size()) != static_cast<int>(knots.size()) - degree - 1) return 0;

        Numeric knot;
        if (!detail::FindExistingKnot(knots, knot_remove, knot)) return 0;
        // Only interior knots may be removed.
        if (!(knot > 0.0) || !(knot < 1.0)) return 0;

        // Work in homogeneous coordinates; the public representation stores
        // Cartesian coordinates plus weight.
        std::vector<Vec4> homogeneous(points.size());
        for (size_t i = 0; i < points.size(); ++i)
        {
            homogeneous[i] = ToHomo(points[i]);
        }

        std::vector<Numeric> out_knots;
        std::vector<Vec4> out_points;
        const int removed = RemoveKnotFromRow(knots, homogeneous, degree, knot, times, tolerance, out_knots, out_points);

        // On zero the inputs must be left completely untouched.
        if (removed == 0) return 0;

        knots = std::move(out_knots);
        points.resize(out_points.size());
        for (size_t i = 0; i < out_points.size(); ++i)
        {
            points[i] = FromHomo(out_points[i]);
        }
        return removed;
    }
}

namespace libnurbs::detail
{
    bool NetIsWellFormed(const std::vector<Vec4>& net)
    {
        if (net.empty()) return false;
        for (const auto& p: net)
        {
            if (!AllFinite(p) || !(p.w() > 0)) return false;
        }
        return true;
    }

    int RemoveKnotTensor(std::vector<Numeric>& knots_along,
                         std::vector<Vec4>& net,
                         int count_u,
                         int count_v,
                         int degree_along,
                         bool along_v,
                         Numeric knot,
                         int times,
                         Numeric tolerance)
    {
        if (degree_along < 1 || times <= 0) return 0;
        if (count_u < 1 || count_v < 1) return 0;
        if (static_cast<int>(net.size()) != count_u * count_v) return 0;
        if (static_cast<int>(knots_along.size()) !=
            (along_v ? count_v : count_u) + degree_along + 1) return 0;

        Numeric stored_knot;
        if (!FindExistingKnot(knots_along, knot, stored_knot)) return 0;
        if (!(stored_knot > 0.0) || !(stored_knot < 1.0)) return 0;

        const int multiplicity = CountMultiplicity(knots_along, stored_knot);
        const int limit = std::min(times, multiplicity);
        if (limit < 1) return 0;

        const int row_count = along_v ? count_u : count_v;
        const int row_length = along_v ? count_v : count_u;

        auto row_of = [&](int row) -> std::vector<Vec4>
        {
            std::vector<Vec4> values(row_length);
            for (int k = 0; k < row_length; ++k)
            {
                values[k] = along_v ? net[k * count_u + row] : net[row * count_u + k];
            }
            return values;
        };
        auto store_row = [&](int row, const std::vector<Vec4>& values, std::vector<Vec4>& target)
        {
            for (int k = 0; k < row_length; ++k)
            {
                (along_v ? target[k * count_u + row] : target[row * count_u + k]) = values[k];
            }
        };

        const Numeric slack = RoundoffSlack(net);

        std::vector<std::vector<Vec4>> base_rows(row_count);
        for (int row = 0; row < row_count; ++row)
        {
            base_rows[row] = row_of(row);
        }

        std::vector<std::vector<Vec4>> accepted_rows(row_count);
        int best = 0;
        for (int t = 1; t <= limit; ++t)
        {
            std::vector<std::vector<Vec4>> next_rows(row_count);
            std::vector<std::vector<Vec4>> next_refined(row_count);
            bool ok = true;
            for (int row = 0; row < row_count; ++row)
            {
                std::vector<Numeric> ignored_knots;
                std::vector<Vec4> candidate;
                std::vector<Vec4> refined;
                if (!BuildRemovalCandidate(knots_along, base_rows[row], degree_along, stored_knot, t,
                                           ignored_knots, candidate, refined))
                {
                    ok = false;
                    break;
                }
                if (static_cast<int>(refined.size()) != row_length)
                {
                    ok = false;
                    break;
                }
                next_rows[row] = std::move(candidate);
                next_refined[row] = std::move(refined);
            }
            if (!ok) break;

            // Rebuild the full nets in their original layout so the error can
            // be bounded jointly, with every row in the rational denominator.
            std::vector<Vec4> refined_net(net.size());
            for (int row = 0; row < row_count; ++row)
            {
                store_row(row, next_refined[row], refined_net);
            }
            if (!NetIsWellFormed(refined_net)) break;

            const Numeric bound = DistanceBound(net, refined_net);
            if (!(bound <= tolerance + slack)) break;

            accepted_rows = std::move(next_rows);
            best = t;
        }

        if (best == 0) return 0;

        // Rebuild the shrinking net from the accepted rows; the knot vector
        // loses exactly the same number of entries.
        const int new_row_length = row_length - best;
        const int new_count_u = along_v ? count_u : new_row_length;
        const int new_count_v = along_v ? new_row_length : count_v;
        std::vector<Vec4> updated(static_cast<size_t>(new_count_u) * new_count_v);
        for (int row = 0; row < row_count; ++row)
        {
            for (int k = 0; k < new_row_length; ++k)
            {
                (along_v ? updated[k * new_count_u + row] : updated[row * new_count_u + k]) = accepted_rows[row][k];
            }
        }

        std::vector<Numeric> reduced_knots = knots_along;
        const int last_index = FindSpan(knots_along, degree_along, stored_knot);
        const int first_index = last_index - best + 1;
        if (first_index < 0 || last_index >= static_cast<int>(reduced_knots.size())) return 0;
        reduced_knots.erase(reduced_knots.begin() + first_index,
                            reduced_knots.begin() + last_index + 1);

        knots_along = std::move(reduced_knots);
        net = std::move(updated);
        return best;
    }
}
