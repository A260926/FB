#pragma once

// Internal helpers shared by the offset fitter and the independent
// certification. Not part of the public API. Interval operations below are
// inflated outward by a few ULPs on every result, emulating directed
// rounding so that numerical enclosures stay conservative under ordinary
// round-to-nearest arithmetic.

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <libnurbs/Core/Typedefs.hpp>
#include <libnurbs/Surface/Surface.hpp>

namespace libnurbs::offset_detail
{
    inline constexpr Numeric kIntervalEps = 4.0 * std::numeric_limits<Numeric>::epsilon();
    inline constexpr Numeric kAbsoluteFloor = 4.0 * std::numeric_limits<Numeric>::epsilon();

    /// Closed real interval [lo, hi], always lo <= hi.
    struct Interval
    {
        Numeric lo{0};
        Numeric hi{0};

        Interval() = default;
        Interval(Numeric v) : lo(v), hi(v) {} // NOLINT(google-explicit-constructor)
        Interval(Numeric l, Numeric h) : lo(l), hi(h) {}

        [[nodiscard]] Numeric Mag() const
        {
            return std::max(std::abs(lo), std::abs(hi));
        }
    };

    inline Interval Inflate(Numeric value)
    {
        Numeric delta = std::max(kIntervalEps * std::abs(value), kAbsoluteFloor);
        return {value - delta, value + delta};
    }

    inline Interval Add(Interval a, Interval b)
    {
        Numeric lo = a.lo + b.lo;
        Numeric hi = a.hi + b.hi;
        return {lo - std::max(kIntervalEps * std::abs(lo), kAbsoluteFloor),
                hi + std::max(kIntervalEps * std::abs(hi), kAbsoluteFloor)};
    }

    inline Interval Sub(Interval a, Interval b)
    {
        Numeric lo = a.lo - b.hi;
        Numeric hi = a.hi - b.lo;
        return {lo - std::max(kIntervalEps * std::abs(lo), kAbsoluteFloor),
                hi + std::max(kIntervalEps * std::abs(hi), kAbsoluteFloor)};
    }

    inline Interval Mul(Interval a, Interval b)
    {
        Numeric v1 = a.lo * b.lo, v2 = a.lo * b.hi;
        Numeric v3 = a.hi * b.lo, v4 = a.hi * b.hi;
        Numeric lo = std::min({v1, v2, v3, v4});
        Numeric hi = std::max({v1, v2, v3, v4});
        Numeric delta = std::max(kIntervalEps * std::max(std::abs(lo), std::abs(hi)), kAbsoluteFloor);
        return {lo - delta, hi + delta};
    }

    inline Interval Div(Interval a, Interval b)
    {
        // Caller guarantees b.lo > 0 or b.hi < 0.
        Numeric v1 = a.lo / b.lo, v2 = a.lo / b.hi;
        Numeric v3 = a.hi / b.lo, v4 = a.hi / b.hi;
        Numeric lo = std::min({v1, v2, v3, v4});
        Numeric hi = std::max({v1, v2, v3, v4});
        Numeric delta = std::max(kIntervalEps * std::max(std::abs(lo), std::abs(hi)), kAbsoluteFloor);
        return {lo - delta, hi + delta};
    }

    inline Interval SqrtInt(Interval a)
    {
        Numeric lo = a.lo > 0 ? std::sqrt(a.lo) : 0.0;
        Numeric hi = std::sqrt(a.hi);
        Numeric delta = std::max(kIntervalEps * hi, kAbsoluteFloor);
        return {std::max(0.0, lo - delta), hi + delta};
    }

    struct IVec3
    {
        Interval x, y, z;
    };

    struct IVec4
    {
        Interval x, y, z, w;
    };

    inline IVec4 operator+(const IVec4& a, const IVec4& b)
    {
        return {Add(a.x, b.x), Add(a.y, b.y), Add(a.z, b.z), Add(a.w, b.w)};
    }

    inline IVec4 operator-(const IVec4& a, const IVec4& b)
    {
        return {Sub(a.x, b.x), Sub(a.y, b.y), Sub(a.z, b.z), Sub(a.w, b.w)};
    }

    inline IVec4 operator*(const IVec4& a, Interval s)
    {
        return {Mul(a.x, s), Mul(a.y, s), Mul(a.z, s), Mul(a.w, s)};
    }

    inline IVec4 ToInterval(const Vec4& v)
    {
        return {Inflate(v.x()), Inflate(v.y()), Inflate(v.z()), Inflate(v.w())};
    }

    inline IVec3 Cross(const IVec3& a, const IVec3& b)
    {
        return {Sub(Mul(a.y, b.z), Mul(a.z, b.y)),
                Sub(Mul(a.z, b.x), Mul(a.x, b.z)),
                Sub(Mul(a.x, b.y), Mul(a.y, b.x))};
    }

    /// Componentwise convex hull of interval-valued Bernstein coefficients.
    template <typename VecT>
    VecT CoefficientHull(const std::vector<VecT>& coefficients);

    /**
     * @brief Structural validation shared by both entry points.
     *
     * @p require_c2 forces the source rule (interior continuity >= C2).
     * Candidate surfaces only need clamped unit domains, degrees 2/3 and
     * strictly positive finite weights.
     */
    bool ValidateSurfaceStructure(const Surface& surface, bool require_c2, std::string* reason);

    bool ValidateOffsetOptions(Numeric distance, Numeric tolerance, int max_boxes, std::string* reason);

    /// Distinct interior breakpoints of a clamped knot vector (values in
    /// (0,1), each reported once).
    std::vector<Numeric> DistinctBreakpoints(const KnotVector& knots);

    /**
     * @brief Refine @p surface by knot insertion until every breakpoint in
     *        @p break_u / @p break_v has multiplicity equal to the axis
     *        degree, turning every knot span into an independent Bernstein-
     *        Bezier patch. Geometry is unchanged up to round-off; the input
     *        surface is not modified.
     */
    Surface RefineToBezier(const Surface& surface,
                           const std::vector<Numeric>& break_u,
                           const std::vector<Numeric>& break_v);

    /**
     * @brief De Casteljau split of a one-dimensional Bernstein polygon at
     *        local parameter @p t. Returns the left and right sub-polygons,
     *        each of the same degree. Interval arithmetic keeps the split
     *        conservative.
     */
    std::pair<std::vector<IVec4>, std::vector<IVec4>>
    DeCasteljauSplit(const std::vector<IVec4>& polygon, Numeric t);

    /**
     * @brief Tensor-product Bezier patch in homogeneous coordinates.
     */
    struct BezierPatch
    {
        int DegreeU{0};
        int DegreeV{0};
        std::vector<IVec4> Coeffs; // (DegreeU+1) x (DegreeV+1), index v*nu+i

        [[nodiscard]] const IVec4& At(int i, int j) const
        {
            return Coeffs[j * (DegreeU + 1) + i];
        }

        [[nodiscard]] IVec4& At(int i, int j)
        {
            return Coeffs[j * (DegreeU + 1) + i];
        }
    };

    /// Split a tensor patch along U at local parameter t.
    std::pair<BezierPatch, BezierPatch> SplitPatchU(const BezierPatch& patch, Numeric t);

    /// Split a tensor patch along V at local parameter t.
    std::pair<BezierPatch, BezierPatch> SplitPatchV(const BezierPatch& patch, Numeric t);

    /// Homogeneous U-derivative patch (degree reduced by one in U).
    BezierPatch DerivativePatchU(const BezierPatch& patch);

    /// Homogeneous V-derivative patch (degree reduced by one in V).
    BezierPatch DerivativePatchV(const BezierPatch& patch);
}
