#include "libnurbs/Detail/OffsetDetail.hpp"

#include <stdexcept>

namespace libnurbs::offset_detail
{
    template <>
    IVec3 CoefficientHull<IVec3>(const std::vector<IVec3>& coefficients)
    {
        IVec3 hull{};
        hull.x = {std::numeric_limits<Numeric>::infinity(), -std::numeric_limits<Numeric>::infinity()};
        hull.y = hull.x;
        hull.z = hull.x;
        for (const auto& c : coefficients)
        {
            hull.x.lo = std::min(hull.x.lo, c.x.lo);
            hull.x.hi = std::max(hull.x.hi, c.x.hi);
            hull.y.lo = std::min(hull.y.lo, c.y.lo);
            hull.y.hi = std::max(hull.y.hi, c.y.hi);
            hull.z.lo = std::min(hull.z.lo, c.z.lo);
            hull.z.hi = std::max(hull.z.hi, c.z.hi);
        }
        return hull;
    }

    template <>
    IVec4 CoefficientHull<IVec4>(const std::vector<IVec4>& coefficients)
    {
        IVec4 hull{};
        const Numeric inf = std::numeric_limits<Numeric>::infinity();
        hull.x = hull.y = hull.z = hull.w = {inf, -inf};
        for (const auto& c : coefficients)
        {
            hull.x.lo = std::min(hull.x.lo, c.x.lo);
            hull.x.hi = std::max(hull.x.hi, c.x.hi);
            hull.y.lo = std::min(hull.y.lo, c.y.lo);
            hull.y.hi = std::max(hull.y.hi, c.y.hi);
            hull.z.lo = std::min(hull.z.lo, c.z.lo);
            hull.z.hi = std::max(hull.z.hi, c.z.hi);
            hull.w.lo = std::min(hull.w.lo, c.w.lo);
            hull.w.hi = std::max(hull.w.hi, c.w.hi);
        }
        return hull;
    }

    bool ValidateSurfaceStructure(const Surface& surface, bool require_c2, std::string* reason)
    {
        auto fail = [&](const char* message)
        {
            if (reason) *reason = message;
            return false;
        };

        if (surface.DegreeU != 2 && surface.DegreeU != 3) return fail("degree U must be 2 or 3");
        if (surface.DegreeV != 2 && surface.DegreeV != 3) return fail("degree V must be 2 or 3");

        const auto& knots_u = surface.KnotsU.Values();
        const auto& knots_v = surface.KnotsV.Values();
        if (knots_u.empty() || knots_v.empty()) return fail("empty knot vector");

        auto check_axis = [&](const std::vector<Numeric>& knots, int degree, int cp_count,
                              const char* axis) -> bool
        {
            if (static_cast<int>(knots.size()) != cp_count + degree + 1)
                return fail(("knot/control-point count mismatch on " + std::string(axis)).c_str());
            for (int i = 0; i <= degree; ++i)
            {
                if (knots[i] != 0.0) return fail(("domain must be open clamped at 0 on " + std::string(axis)).c_str());
                if (knots[static_cast<int>(knots.size()) - 1 - i] != 1.0)
                    return fail(("domain must be open clamped at 1 on " + std::string(axis)).c_str());
            }
            Numeric previous = 0.0;
            for (int i = degree + 1; i < static_cast<int>(knots.size()) - degree - 1;)
            {
                Numeric value = knots[i];
                if (!std::isfinite(value) || value <= 0.0 || value >= 1.0)
                    return fail(("non-finite or out-of-unit-domain knot on " + std::string(axis)).c_str());
                int multiplicity = 1;
                while (i + multiplicity < static_cast<int>(knots.size()) && knots[i + multiplicity] == value)
                    ++multiplicity;
                int max_multiplicity = require_c2 ? degree - 2 : degree - 1;
                if (multiplicity > max_multiplicity)
                    return fail(("interior knot multiplicity breaks the continuity rule on "
                                 + std::string(axis)).c_str());
                if (value - previous < 1e-3 - 1e-15)
                    return fail(("distinct knot spacing below 1e-3 on " + std::string(axis)).c_str());
                previous = value;
                i += multiplicity;
            }
            // Gap from the last interior breakpoint to the clamped end.
            if (1.0 - previous < 1e-3 - 1e-15)
                return fail(("distinct knot spacing below 1e-3 on " + std::string(axis)).c_str());
            return true;
        };

        if (!check_axis(knots_u, surface.DegreeU, surface.ControlPoints.UCount, "U")) return false;
        if (!check_axis(knots_v, surface.DegreeV, surface.ControlPoints.VCount, "V")) return false;

        if (surface.ControlPoints.UCount <= surface.DegreeU ||
            surface.ControlPoints.VCount <= surface.DegreeV)
            return fail("not enough control points for the degrees");

        for (const auto& cp : surface.ControlPoints.Values)
        {
            if (!std::isfinite(cp.x()) || !std::isfinite(cp.y()) ||
                !std::isfinite(cp.z()) || !std::isfinite(cp.w()))
                return fail("non-finite control point data");
            if (std::abs(cp.x()) > 10.0 || std::abs(cp.y()) > 10.0 || std::abs(cp.z()) > 10.0)
                return fail("control coordinate exceeds the |10| input bound");
            if (!(cp.w() > 0.0)) return fail("weight must be strictly positive");
            if (cp.w() < 0.1 || cp.w() > 10.0) return fail("weight outside [0.1, 10]");
        }
        return true;
    }

    bool ValidateOffsetOptions(Numeric distance, Numeric tolerance, int max_boxes, std::string* reason)
    {
        auto fail = [&](const char* message)
        {
            if (reason) *reason = message;
            return false;
        };
        if (!std::isfinite(distance) || std::abs(distance) > 0.25) return fail("|distance| must be <= 0.25");
        if (!std::isfinite(tolerance) || tolerance < 1e-5 || tolerance > 1e-2)
            return fail("tolerance must lie in [1e-5, 1e-2]");
        if (max_boxes < 1 || max_boxes > 1'000'000) return fail("max_boxes must lie in [1, 1e6]");
        return true;
    }

    std::vector<Numeric> DistinctBreakpoints(const KnotVector& knots)
    {
        std::vector<Numeric> result;
        const auto& values = knots.Values();
        for (Numeric value : values)
        {
            if (value > 0.0 && value < 1.0 &&
                (result.empty() || value != result.back()))
            {
                result.push_back(value);
            }
        }
        return result;
    }

    Surface RefineToBezier(const Surface& surface,
                           const std::vector<Numeric>& break_u,
                           const std::vector<Numeric>& break_v)
    {
        Surface refined = surface;
        // Count multiplicity through the current object on every iteration:
        // InsertKnot* reassigns `refined`, so references to its knot vectors
        // must not be held across that assignment.
        auto multiplicity = [](const KnotVector& knot_vector, Numeric value)
        {
            int count = 0;
            for (Numeric existing : knot_vector.Values())
                if (existing == value) ++count;
            return count;
        };
        for (Numeric breakpoint : break_u)
        {
            int missing = refined.DegreeU - multiplicity(refined.KnotsU, breakpoint);
            for (int k = 0; k < missing; ++k)
                refined = refined.InsertKnotU(breakpoint);
        }
        for (Numeric breakpoint : break_v)
        {
            int missing = refined.DegreeV - multiplicity(refined.KnotsV, breakpoint);
            for (int k = 0; k < missing; ++k)
                refined = refined.InsertKnotV(breakpoint);
        }
        return refined;
    }

    std::pair<std::vector<IVec4>, std::vector<IVec4>>
    DeCasteljauSplit(const std::vector<IVec4>& polygon, Numeric t)
    {
        const int degree = static_cast<int>(polygon.size()) - 1;
        std::vector<IVec4> current = polygon;
        std::vector<IVec4> left(degree + 1);
        std::vector<IVec4> right(degree + 1);
        left[0] = current.front();
        right[degree] = current.back();
        for (int r = 1; r <= degree; ++r)
        {
            std::vector<IVec4> next(current.size() - 1);
            for (size_t i = 0; i < next.size(); ++i)
            {
                next[i] = current[i] * Interval(1.0 - t) + current[i + 1] * Interval(t);
            }
            left[r] = next.front();
            right[degree - r] = next.back();
            current = std::move(next);
        }
        return {left, right};
    }

    std::pair<BezierPatch, BezierPatch> SplitPatchU(const BezierPatch& patch, Numeric t)
    {
        const int nu = patch.DegreeU + 1;
        const int nv = patch.DegreeV + 1;
        BezierPatch left{}, right{};
        left.DegreeU = right.DegreeU = patch.DegreeU;
        left.DegreeV = right.DegreeV = patch.DegreeV;
        left.Coeffs.resize(nu * nv);
        right.Coeffs.resize(nu * nv);
        for (int j = 0; j < nv; ++j)
        {
            std::vector<IVec4> row(nu);
            for (int i = 0; i < nu; ++i) row[i] = patch.At(i, j);
            auto [lrow, rrow] = DeCasteljauSplit(row, t);
            for (int i = 0; i < nu; ++i)
            {
                left.At(i, j) = lrow[i];
                right.At(i, j) = rrow[i];
            }
        }
        return {left, right};
    }

    std::pair<BezierPatch, BezierPatch> SplitPatchV(const BezierPatch& patch, Numeric t)
    {
        const int nu = patch.DegreeU + 1;
        const int nv = patch.DegreeV + 1;
        BezierPatch left{}, right{};
        left.DegreeU = right.DegreeU = patch.DegreeU;
        left.DegreeV = right.DegreeV = patch.DegreeV;
        left.Coeffs.resize(nu * nv);
        right.Coeffs.resize(nu * nv);
        for (int i = 0; i < nu; ++i)
        {
            std::vector<IVec4> column(nv);
            for (int j = 0; j < nv; ++j) column[j] = patch.At(i, j);
            auto [lcol, rcol] = DeCasteljauSplit(column, t);
            for (int j = 0; j < nv; ++j)
            {
                left.At(i, j) = lcol[j];
                right.At(i, j) = rcol[j];
            }
        }
        return {left, right};
    }

    BezierPatch DerivativePatchU(const BezierPatch& patch)
    {
        BezierPatch result{};
        result.DegreeU = patch.DegreeU - 1;
        result.DegreeV = patch.DegreeV;
        const int nu = result.DegreeU + 1;
        const int nv = result.DegreeV + 1;
        result.Coeffs.resize(nu * nv);
        const Numeric factor = static_cast<Numeric>(patch.DegreeU);
        for (int j = 0; j < nv; ++j)
        {
            for (int i = 0; i < nu; ++i)
            {
                IVec4 diff = patch.At(i + 1, j) - patch.At(i, j);
                result.At(i, j) = diff * Interval(factor);
            }
        }
        return result;
    }

    BezierPatch DerivativePatchV(const BezierPatch& patch)
    {
        BezierPatch result{};
        result.DegreeU = patch.DegreeU;
        result.DegreeV = patch.DegreeV - 1;
        const int nu = result.DegreeU + 1;
        const int nv = result.DegreeV + 1;
        result.Coeffs.resize(nu * nv);
        const Numeric factor = static_cast<Numeric>(patch.DegreeV);
        for (int j = 0; j < nv; ++j)
        {
            for (int i = 0; i < nu; ++i)
            {
                IVec4 diff = patch.At(i, j + 1) - patch.At(i, j);
                result.At(i, j) = diff * Interval(factor);
            }
        }
        return result;
    }
}
