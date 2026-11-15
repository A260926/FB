#include "task_support/check.h"

#include <libnurbs/Algorithm/KnotInsertion.hpp>
#include <libnurbs/Core/KnotVector.hpp>
#include <libnurbs/Curve/Curve.hpp>
#include <libnurbs/Surface/Surface.hpp>
#include <libnurbs/Surface/SurfaceMesh.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace libnurbs;

namespace
{
    constexpr double kTight = 1e-11;
    constexpr double kLoose = 1e-8;

    bool Near(double a, double b, double tolerance = kTight)
    {
        return std::abs(a - b) <= tolerance;
    }

    bool Near(const Vec3& a, const Vec3& b, double tolerance = kTight)
    {
        return (a - b).norm() <= tolerance;
    }

    // ---- Analytic reference geometry ----------------------------------------

    struct Bezier2
    {
        Vec3 p0, p1, p2;
        double w0 = 1.0, w1 = 1.0, w2 = 1.0;

        Vec3 Point(double t) const
        {
            const double b0 = (1 - t) * (1 - t);
            const double b1 = 2 * t * (1 - t);
            const double b2 = t * t;
            const Vec3 a = b0 * (w0 * p0) + b1 * (w1 * p1) + b2 * (w2 * p2);
            const double w = b0 * w0 + b1 * w1 + b2 * w2;
            return a / w;
        }

        // Analytic rational derivatives with respect to t.
        void Derivatives(double t, Vec3& d1, Vec3& d2) const
        {
            const Vec3 a = Point(t);
            const Vec3 a1 = 2 * ((w1 * p1 - w0 * p0) * (1 - t) + (w2 * p2 - w1 * p1) * t);
            const Vec3 a2 = 2 * (w2 * p2 - 2 * w1 * p1 + w0 * p0);
            const double w = w0 * (1 - t) * (1 - t) + 2 * w1 * t * (1 - t) + w2 * t * t;
            const double w1d = 2 * ((w1 - w0) * (1 - t) + (w2 - w1) * t);
            const double w2d = 2 * (w2 - 2 * w1 + w0);
            d1 = (a1 - w1d * a) / w;
            d2 = (a2 - 2 * w1d * d1 - w2d * a) / w;
        }
    };

    struct Bezier3
    {
        Vec3 p0, p1, p2, p3;

        Vec3 Point(double t) const
        {
            const double b0 = (1 - t) * (1 - t) * (1 - t);
            const double b1 = 3 * t * (1 - t) * (1 - t);
            const double b2 = 3 * t * t * (1 - t);
            const double b3 = t * t * t;
            return b0 * p0 + b1 * p1 + b2 * p2 + b3 * p3;
        }

        void Derivatives(double t, Vec3& d1, Vec3& d2, Vec3& d3) const
        {
            d1 = 3 * ((p1 - p0) * ((1 - t) * (1 - t)) + (p2 - p1) * (2 * t * (1 - t)) +
                      (p3 - p2) * (t * t));
            d2 = 6 * ((p2 - 2 * p1 + p0) * (1 - t) + (p3 - 2 * p2 + p1) * t);
            d3 = 6 * (p3 - 3 * p2 + 3 * p1 - p0);
        }
    };

    Curve MakeRationalQuarterArc(double a, double b)
    {
        const double k = std::sqrt(2.0) / 2.0;
        Curve c;
        c.Degree = 2;
        c.Knots = KnotVector{{a, a, a, b, b, b}};
        c.ControlPoints = {Vec4(1, 0, 0, 1), Vec4(1, 1, 0, k), Vec4(0, 1, 0, 1)};
        return c;
    }

    // Rational cylinder patch: quarter circle (degree 2) in U, line (degree 1)
    // in V. U in [ua, ub], V in [va, vb].
    Surface MakeRationalCylinder(double ua, double ub, double va, double vb)
    {
        const double k = std::sqrt(2.0) / 2.0;
        Surface s;
        s.DegreeU = 2;
        s.DegreeV = 1;
        s.KnotsU = KnotVector{{ua, ua, ua, ub, ub, ub}};
        s.KnotsV = KnotVector{{va, va, vb, vb}};
        s.ControlPoints = ControlPointGrid(3, 2);
        for (int v = 0; v < 2; ++v)
        {
            const double z = v;
            s.ControlPoints.Get(0, v) = Vec4(1, 0, z, 1);
            s.ControlPoints.Get(1, v) = Vec4(1, 1, z, k);
            s.ControlPoints.Get(2, v) = Vec4(0, 1, z, 1);
        }
        return s;
    }

    Surface MakeBilinearPlane(double ua, double ub, double va, double vb,
                              const Vec3& p00, const Vec3& p10, const Vec3& p01, const Vec3& p11)
    {
        Surface s;
        s.DegreeU = 1;
        s.DegreeV = 1;
        s.KnotsU = KnotVector{{ua, ua, ub, ub}};
        s.KnotsV = KnotVector{{va, va, vb, vb}};
        s.ControlPoints = ControlPointGrid(2, 2);
        s.ControlPoints.Get(0, 0) = Vec4(p00.x(), p00.y(), p00.z(), 1);
        s.ControlPoints.Get(1, 0) = Vec4(p10.x(), p10.y(), p10.z(), 1);
        s.ControlPoints.Get(0, 1) = Vec4(p01.x(), p01.y(), p01.z(), 1);
        s.ControlPoints.Get(1, 1) = Vec4(p11.x(), p11.y(), p11.z(), 1);
        return s;
    }

    // Bilinear position and derivatives, analytic in the original parameters.
    Vec3 BilinearPoint(double u, double v, double ua, double ub, double va, double vb,
                       const Vec3& p00, const Vec3& p10, const Vec3& p01, const Vec3& p11)
    {
        const double su = (u - ua) / (ub - ua);
        const double sv = (v - va) / (vb - va);
        const Vec3 du = p10 - p00;
        const Vec3 dv = p01 - p00;
        const Vec3 mixed = p11 - p10 - p01 + p00;
        return p00 + su * du + sv * dv + su * sv * mixed;
    }

    // ---- Cases ---------------------------------------------------------------

    int case_knot_domain_bounds()
    {
        KnotVector knots{{-3, -3, -3, -1, 1, 5, 5, 5}};
        TASK_CHECK(knots.IsValid());
        TASK_CHECK(knots.IsClamped(2));
        auto [begin, end] = knots.GetDomain(2);
        TASK_CHECK(Near(begin, -3.0, 0) && Near(end, 5.0, 0));

        // The Curve/Surface wrappers expose the same original domain.
        {
            Curve line;
            line.Degree = 1;
            line.Knots = KnotVector{{-3, -3, 5, 5}};
            line.ControlPoints = {Vec4(0, 0, 0, 1), Vec4(1, 0, 0, 1)};
            auto [line_begin, line_end] = line.GetDomain();
            TASK_CHECK(Near(line_begin, -3.0, 0) && Near(line_end, 5.0, 0));

            Surface patch;
            patch.DegreeU = 1;
            patch.DegreeV = 1;
            patch.KnotsU = KnotVector{{-3, -3, 5, 5}};
            patch.KnotsV = KnotVector{{10, 10, 12, 12}};
            patch.ControlPoints = ControlPointGrid(2, 2);
            auto [pu0, pu1] = patch.GetDomainU();
            auto [pv0, pv1] = patch.GetDomainV();
            TASK_CHECK(Near(pu0, -3.0, 0) && Near(pu1, 5.0, 0));
            TASK_CHECK(Near(pv0, 10.0, 0) && Near(pv1, 12.0, 0));
        }

        // Distinct knots are preserved verbatim.
        TASK_CHECK(knots.Values().size() == 8);
        TASK_CHECK(Near(knots.Values()[3], -1.0, 0) && Near(knots.Values()[4], 1.0, 0));

        // Interior multiplicity at most the degree is accepted; degree 1 is not.
        bool degree2_ok = true;
        try
        {
            (void)knots.GetDomain(2);
        }
        catch (const std::invalid_argument&)
        {
            degree2_ok = false;
        }
        TASK_CHECK(degree2_ok);

        bool degree1_rejected = false;
        try
        {
            (void)KnotVector{{-3, -3, -1, -1, 5, 5}}.GetDomain(1);
        }
        catch (const std::invalid_argument&)
        {
            degree1_rejected = true;
        }
        TASK_CHECK(degree1_rejected);

        // Unsupported vectors throw std::invalid_argument.
        auto rejected = [](const std::vector<double>& values)
        {
            try
            {
                KnotVector knots_value(values);
                return false;
            }
            catch (const std::invalid_argument&)
            {
                return true;
            }
        };
        TASK_CHECK(rejected({-3, -3, 5, 1}));          // decreasing
        TASK_CHECK(rejected({-3, -3, 0.0, 0.0 + 1e-8, 5, 5, 5})); // too close
        TASK_CHECK(rejected({-3, -3, -3, 5, 5}));      // end multiplicity mismatch
        TASK_CHECK(rejected({-3, -3, 20000.0, 5, 5})); // out of the numeric range
        TASK_CHECK(rejected({-3, -3, std::numeric_limits<double>::quiet_NaN(), 5, 5}));

        // Reverse uses a + b - u; the domain stays [a, b].
        KnotVector reversed = knots;
        reversed.Reverse();
        TASK_CHECK(Near(reversed.Values().front(), -3.0, 0));
        TASK_CHECK(Near(reversed.Values().back(), 5.0, 0));
        TASK_CHECK(Near(reversed.Values()[3], 1.0, 0)); // original 1 -> 2 - 1 = 1
        TASK_CHECK(Near(reversed.Values()[4], 3.0, 0)); // original -1 -> 2 + 1 = 3
        return 0;
    }

    int case_curve_analytic_derivatives()
    {
        const double a = -3.0;
        const double b = 5.0;
        const double span = b - a;

        // Degree 1 line: exact first derivative, zero second derivative.
        {
            Curve line;
            line.Degree = 1;
            line.Knots = KnotVector{{a, a, b, b}};
            line.ControlPoints = {Vec4(1, -2, 0.5, 1), Vec4(9, 4, 2.5, 1)};
            const Vec3 slope = (Vec3(9, 4, 2.5) - Vec3(1, -2, 0.5)) / span;
            for (double u : {-3.0, -1.2, 0.0, 2.7, 5.0})
            {
                auto values = line.EvaluateAll(u, 2);
                TASK_CHECK(Near(values[0], Vec3(1, -2, 0.5) + (u - a) * slope, kTight));
                TASK_CHECK(Near(values[1], slope, kTight));
                TASK_CHECK(Near(values[2], Vec3(0, 0, 0), kTight));
            }
        }

        // Degree 2 rational quarter circle, analytic quotient-rule derivatives.
        {
            Curve arc = MakeRationalQuarterArc(a, b);
            const Bezier2 reference{Vec3(1, 0, 0), Vec3(1, 1, 0), Vec3(0, 1, 0),
                                    1.0, std::sqrt(2.0) / 2.0, 1.0};
            for (double t : {0.1, 0.35, 0.5, 0.8})
            {
                const double u = a + t * span;
                Vec3 d1;
                Vec3 d2;
                reference.Derivatives(t, d1, d2);
                auto values = arc.EvaluateAll(u, 2);
                TASK_CHECK(Near(values[0], reference.Point(t), kTight));
                TASK_CHECK(Near(values[1], d1 / span, kLoose));
                TASK_CHECK(Near(values[2], d2 / (span * span), kLoose));
                // The arc lies on the unit circle: radius one and tangent
                // perpendicular to the position.
                TASK_CHECK(Near(values[0].head<2>().norm(), 1.0, kTight));
                TASK_CHECK(Near(values[0].head<2>().dot(values[1].head<2>()), 0.0, 1e-9));
            }
        }

        // Degree 3 non-rational Bezier, analytic Bernstein derivatives.
        {
            Curve cubic;
            cubic.Degree = 3;
            cubic.Knots = KnotVector{{a, a, a, a, b, b, b, b}};
            cubic.ControlPoints = {Vec4(0, 0, 0, 1), Vec4(1, 3, 0, 1),
                                   Vec4(4, 2, 1, 1), Vec4(6, 0, 2, 1)};
            const Bezier3 reference{Vec3(0, 0, 0), Vec3(1, 3, 0), Vec3(4, 2, 1), Vec3(6, 0, 2)};
            for (double t : {0.05, 0.3, 0.62, 0.95})
            {
                const double u = a + t * span;
                Vec3 d1;
                Vec3 d2;
                Vec3 d3;
                reference.Derivatives(t, d1, d2, d3);
                auto values = cubic.EvaluateAll(u, 3);
                TASK_CHECK(Near(values[0], reference.Point(t), kTight));
                TASK_CHECK(Near(values[1], d1 / span, kLoose));
                TASK_CHECK(Near(values[2], d2 / (span * span), kLoose));
                TASK_CHECK(Near(values[3], d3 / (span * span * span), kLoose));
            }
        }
        return 0;
    }

    int case_surface_analytic_derivatives()
    {
        const double ua = -3.0, ub = 5.0, va = 10.0, vb = 12.0;
        const Vec3 p00(1, 2, 3), p10(9, 2, 3), p01(1, 6, 5), p11(9, 6, 5);
        Surface plane = MakeBilinearPlane(ua, ub, va, vb, p00, p10, p01, p11);

        const Vec3 du = (p10 - p00) / (ub - ua);
        const Vec3 dv = (p01 - p00) / (vb - va);
        const Vec3 mixed = (p11 - p10 - p01 + p00) / ((ub - ua) * (vb - va));

        for (double u : {-3.0, -0.4, 3.3, 5.0})
        {
            for (double v : {10.0, 10.4, 11.6, 12.0})
            {
                auto values = plane.EvaluateAll(u, v, 1, 1);
                const double su = (u - ua) / (ub - ua);
                const double sv = (v - va) / (vb - va);
                TASK_CHECK(Near(values.Get(0, 0), BilinearPoint(u, v, ua, ub, va, vb, p00, p10, p01, p11), kTight));
                TASK_CHECK(Near(values.Get(1, 0), du + sv * mixed * (vb - va), kLoose));
                TASK_CHECK(Near(values.Get(0, 1), dv + su * mixed * (ub - ua), kLoose));
                TASK_CHECK(Near(values.Get(1, 1), mixed, kLoose));
            }
        }
        return 0;
    }

    int case_affine_reparameterization()
    {
        // Unit-domain reference surface.
        Surface reference;
        reference.DegreeU = 2;
        reference.DegreeV = 1;
        reference.KnotsU = KnotVector{{0, 0, 0, 1, 1, 1}};
        reference.KnotsV = KnotVector{{0, 0, 1, 1}};
        reference.ControlPoints = ControlPointGrid(3, 2);
        reference.ControlPoints.Values = {Vec4(1, 0, 0, 1), Vec4(1, 1, 0, 1), Vec4(0, 1, 0, 2),
                                          Vec4(1, 0, 1, 1), Vec4(1, 1, 1, 1), Vec4(0, 1, 1, 2)};

        // Apply u' = -3 + 8u, v' = 10 + 2v to the knots.
        Surface mapped = reference;
        mapped.KnotsU = KnotVector{{-3, -3, -3, 5, 5, 5}};
        mapped.KnotsV = KnotVector{{10, 10, 12, 12}};

        for (double u : {0.05, 0.37, 0.72})
        {
            for (double v : {0.11, 0.46, 0.93})
            {
                auto ref = reference.EvaluateAll(u, v, 1, 1);
                auto map = mapped.EvaluateAll(-3 + 8 * u, 10 + 2 * v, 1, 1);
                TASK_CHECK(Near(map.Get(0, 0), ref.Get(0, 0), kTight));
                TASK_CHECK(Near(map.Get(1, 0), ref.Get(1, 0) / 8.0, kLoose));
                TASK_CHECK(Near(map.Get(0, 1), ref.Get(0, 1) / 2.0, kLoose));
                TASK_CHECK(Near(map.Get(1, 1), ref.Get(1, 1) / 16.0, kLoose));
            }
        }

        // Both axes independently reparameterised.
        Surface both = reference;
        both.KnotsU = KnotVector{{1, 1, 1, 4, 4, 4}};       // u' = 1 + 3u
        both.KnotsV = KnotVector{{-7, -7, -3, -3}};        // v' = -7 + 4v
        for (double u : {0.2, 0.6})
        {
            for (double v : {0.3, 0.9})
            {
                auto ref = reference.EvaluateAll(u, v, 1, 1);
                auto map = both.EvaluateAll(1 + 3 * u, -7 + 4 * v, 1, 1);
                TASK_CHECK(Near(map.Get(0, 0), ref.Get(0, 0), kTight));
                TASK_CHECK(Near(map.Get(1, 0), ref.Get(1, 0) / 3.0, kLoose));
                TASK_CHECK(Near(map.Get(0, 1), ref.Get(0, 1) / 4.0, kLoose));
            }
        }
        return 0;
    }

    int case_out_of_domain_and_illegal_knots()
    {
        Curve curve = MakeRationalQuarterArc(-3.0, 5.0);
        bool threw = false;
        try
        {
            (void)curve.Evaluate(-3.5);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        TASK_CHECK(threw);

        threw = false;
        try
        {
            (void)curve.Evaluate(5.5);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        TASK_CHECK(threw);

        Surface surface = MakeRationalCylinder(-3.0, 5.0, 10.0, 12.0);
        threw = false;
        try
        {
            (void)surface.Evaluate(0.0, 9.0);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        TASK_CHECK(threw);

        threw = false;
        try
        {
            (void)surface.EvaluateDerivative(6.0, 11.0, 1, 0);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        TASK_CHECK(threw);

        // Inserting a knot outside the domain is invalid.
        threw = false;
        try
        {
            (void)curve.InsertKnot(-4.0);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        TASK_CHECK(threw);
        return 0;
    }

    int case_curve_inverse_monotone_quadratic()
    {
        const double a = -3.0;
        const double b = 5.0;
        Curve guide;
        guide.Degree = 2;
        guide.Knots = KnotVector{{a, a, a, b, b, b}};
        guide.ControlPoints = {Vec4(0, 0, 0, 1), Vec4(2, 0.5, 0, 1), Vec4(4, 3, 0, 1)};

        for (double t : {0.1, 0.35, 0.6, 0.88})
        {
            const double target = a + t * (b - a);
            const Vec3 point = guide.Evaluate(target);
            // Start from a different interior parameter that is not the answer.
            const double start = target < 1.0 ? 4.0 : -2.0;
            const double found = guide.SearchParameter(point, start, 1e-10);
            TASK_CHECK(Near((guide.Evaluate(found) - point).norm(), 0.0, 1e-8));
            const double found_binary = guide.BinarySearchParameter(point, 1e-10);
            TASK_CHECK(Near((guide.Evaluate(found_binary) - point).norm(), 0.0, 1e-7));
        }

        // An out-of-domain initial guess is projected onto the nearest boundary.
        const Vec3 start_point = guide.Evaluate(a);
        TASK_CHECK(Near(guide.SearchParameter(start_point, -50.0, 1e-10), a, 1e-8));
        const Vec3 end_point = guide.Evaluate(b);
        TASK_CHECK(Near(guide.SearchParameter(end_point, 50.0, 1e-10), b, 1e-8));
        return 0;
    }

    int case_surface_inverse_rational_cylinder()
    {
        const double ua = 10.0, ub = 12.0, va = -3.0, vb = 5.0;
        Surface cylinder = MakeRationalCylinder(ua, ub, va, vb);
        auto [du0, du1] = cylinder.GetDomainU();
        auto [dv0, dv1] = cylinder.GetDomainV();
        TASK_CHECK(Near(du0, ua, 0) && Near(du1, ub, 0));
        TASK_CHECK(Near(dv0, va, 0) && Near(dv1, vb, 0));

        const double targets[][2] = {{10.2, -2.5}, {10.55, -0.4}, {11.1, 1.2}, {11.85, 4.4}};
        for (const auto& uv : targets)
        {
            const Vec3 point = cylinder.Evaluate(uv[0], uv[1]);

            auto [su, sv] = cylinder.SearchParameter(point, 11.9, 4.9, 1e-10);
            TASK_CHECK(Near((cylinder.Evaluate(su, sv) - point).norm(), 0.0, 1e-8));

            // direction 1 searches u with v held constant.
            auto [ou, ov] = cylinder.SearchParameterOn(point, 1, uv[1], 11.9, 1e-10);
            TASK_CHECK(Near((cylinder.Evaluate(ou, ov) - point).norm(), 0.0, 1e-8));
            TASK_CHECK(Near(ov, uv[1], 0));

            // direction 0 searches v with u held constant.
            auto [pu, pv] = cylinder.SearchParameterOn(point, 0, uv[0], 4.9, 1e-10);
            TASK_CHECK(Near((cylinder.Evaluate(pu, pv) - point).norm(), 0.0, 1e-8));
            TASK_CHECK(Near(pu, uv[0], 0));

            auto [bu, bv] = cylinder.BinarySearchParameterOn(point, 1, uv[1], 1e-10);
            TASK_CHECK(Near((cylinder.Evaluate(bu, bv) - point).norm(), 0.0, 1e-7));
            auto [cu, cv] = cylinder.BinarySearchParameterOn(point, 0, uv[0], 1e-10);
            TASK_CHECK(Near((cylinder.Evaluate(cu, cv) - point).norm(), 0.0, 1e-7));
        }
        return 0;
    }

    int case_wide_domain_inverse()
    {
        // Deliberately wide, asymmetric original domains stress the search
        // step scaling. epsilon is a spatial residual, so the recovered
        // parameter need not be exact, but the reconstructed point must be.
        for (const auto& domain : std::vector<std::array<double, 4>>{
                 {100.0, 200.0, -50.0, 50.0},
                 {-3.0, 5.0, 10.0, 12.0},
                 {0.25, 0.5, 1.0, 3.0}})
        {
            const double ua = domain[0], ub = domain[1], va = domain[2], vb = domain[3];
            const double k = std::sqrt(2.0) / 2.0;
            Surface s;
            s.DegreeU = 2;
            s.DegreeV = 1;
            s.KnotsU = KnotVector{{ua, ua, ua, ub, ub, ub}};
            s.KnotsV = KnotVector{{va, va, vb, vb}};
            s.ControlPoints = ControlPointGrid(3, 2);
            for (int v = 0; v < 2; ++v)
            {
                s.ControlPoints.Get(0, v) = Vec4(1, 0, v, 1);
                s.ControlPoints.Get(1, v) = Vec4(1, 1, v, k);
                s.ControlPoints.Get(2, v) = Vec4(0, 1, v, 1);
            }
            for (double t : {0.1, 0.35, 0.7, 0.9})
            {
                for (double w : {0.2, 0.55, 0.85})
                {
                    const double tu = ua + t * (ub - ua);
                    const double tv = va + w * (vb - va);
                    const Vec3 point = s.Evaluate(tu, tv);

                    // Start from the opposite corner, not the solution.
                    auto [su, sv] = s.SearchParameter(point, ub, vb, 1e-10);
                    TASK_CHECK(Near((s.Evaluate(su, sv) - point).norm(), 0.0, 1e-7));

                    auto [ou, ov] = s.SearchParameterOn(point, 1, tv, ub, 1e-10);
                    TASK_CHECK(Near((s.Evaluate(ou, ov) - point).norm(), 0.0, 1e-7));
                    TASK_CHECK(Near(ov, tv, 0));

                    auto [bu, bv] = s.BinarySearchParameterOn(point, 1, tv, 1e-10);
                    TASK_CHECK(Near((s.Evaluate(bu, bv) - point).norm(), 0.0, 1e-6));
                }
            }
        }

        // A monotone rational curve on a wide domain.
        Curve c;
        c.Degree = 2;
        c.Knots = KnotVector{{100, 100, 100, 200, 200, 200}};
        c.ControlPoints = {Vec4(1, 0, 0, 1), Vec4(1, 1, 0, 0.7), Vec4(0, 1, 0, 1)};
        for (double t : {0.15, 0.4, 0.75})
        {
            const double target = 100 + t * 100;
            const Vec3 point = c.Evaluate(target);
            const double found = c.SearchParameter(point, 200.0, 1e-10);
            TASK_CHECK(Near((c.Evaluate(found) - point).norm(), 0.0, 1e-7));
            const double found_binary = c.BinarySearchParameter(point, 1e-10);
            TASK_CHECK(Near((c.Evaluate(found_binary) - point).norm(), 0.0, 1e-6));
        }
        return 0;
    }

    int case_insert_knot_curve()
    {
        const double a = -3.0;
        const double b = 5.0;
        Curve curve = MakeRationalQuarterArc(a, b);
        for (double knot : {-2.0, 0.4, 4.5})
        {
            Curve inserted = curve.InsertKnot(knot);
            TASK_CHECK(inserted.Knots.Count() == curve.Knots.Count() + 1);
            TASK_CHECK((int)inserted.ControlPoints.size() == inserted.Knots.Count() - inserted.Degree - 1);
            for (double u : {-3.0, -2.5, -1.0, 0.0, 1.5, 3.6, 5.0})
            {
                TASK_CHECK(Near(inserted.Evaluate(u), curve.Evaluate(u), kLoose));
                TASK_CHECK(Near(inserted.EvaluateDerivative(u, 1), curve.EvaluateDerivative(u, 1), 1e-7));
            }
        }

        // Repeating an existing interior knot raises its multiplicity.
        Curve multi = MakeRationalQuarterArc(a, b).InsertKnot(0.5, 2);
        TASK_CHECK(multi.Knots.GetMultiplicity(0.5) == 2);
        // A third insertion would exceed degree 2.
        bool threw = false;
        try
        {
            (void)multi.InsertKnot(0.5);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        TASK_CHECK(threw);
        return 0;
    }

    int case_insert_knot_surface_both_axes()
    {
        const double ua = -3.0, ub = 5.0, va = 10.0, vb = 12.0;
        Surface surface = MakeRationalCylinder(ua, ub, va, vb);
        Surface inserted = surface.InsertKnotU(0.0).InsertKnotV(11.0);
        TASK_CHECK(inserted.KnotsU.Count() == surface.KnotsU.Count() + 1);
        TASK_CHECK(inserted.KnotsV.Count() == surface.KnotsV.Count() + 1);
        TASK_CHECK(inserted.ControlPoints.UCount == inserted.KnotsU.Count() - inserted.DegreeU - 1);
        TASK_CHECK(inserted.ControlPoints.VCount == inserted.KnotsV.Count() - inserted.DegreeV - 1);
        TASK_CHECK(inserted.ControlPoints.Values.size() ==
                   (size_t)(inserted.ControlPoints.UCount * inserted.ControlPoints.VCount));
        for (double u : {-3.0, -1.0, 0.0, 2.0, 5.0})
        {
            for (double v : {10.0, 11.0, 12.0})
            {
                TASK_CHECK(Near(inserted.Evaluate(u, v), surface.Evaluate(u, v), kLoose));
                TASK_CHECK(Near(inserted.EvaluateDerivative(u, v, 1, 0),
                                surface.EvaluateDerivative(u, v, 1, 0), 1e-8));
                TASK_CHECK(Near(inserted.EvaluateDerivative(u, v, 0, 1),
                                surface.EvaluateDerivative(u, v, 0, 1), 1e-8));
            }
        }
        return 0;
    }

    int case_elevate_degree_multi_span()
    {
        const double a = -3.0;
        const double b = 5.0;
        Curve curve;
        curve.Degree = 2;
        curve.Knots = KnotVector{{a, a, a, -1, 1, b, b, b}};
        curve.ControlPoints = {Vec4(1, 0, 0, 1), Vec4(1, 1, 0, 0.7), Vec4(0, 1, 0, 1.4),
                               Vec4(-1, 1, 0, 0.8), Vec4(-1, 0, 0, 1)};

        Curve elevated = curve.ElevateDegree(1);
        TASK_CHECK(elevated.Degree == 3);
        // Control net must be allocated from the actual knot count, not a
        // single-segment size formula.
        TASK_CHECK((int)elevated.ControlPoints.size() == elevated.Knots.Count() - elevated.Degree - 1);
        TASK_CHECK(elevated.Knots.IsClamped(3));
        for (double u : {a, -2.0, -1.0, 0.25, 2.0, 4.5, b})
        {
            TASK_CHECK(Near(elevated.Evaluate(u), curve.Evaluate(u), kLoose));
            TASK_CHECK(Near(elevated.EvaluateDerivative(u, 1), curve.EvaluateDerivative(u, 1), 1e-7));
        }

        Curve elevated_twice = curve.ElevateDegree(2);
        TASK_CHECK(elevated_twice.Degree == 4);
        TASK_CHECK((int)elevated_twice.ControlPoints.size() ==
                   elevated_twice.Knots.Count() - elevated_twice.Degree - 1);
        for (double u : {-2.0, 0.0, 3.0})
        {
            TASK_CHECK(Near(elevated_twice.Evaluate(u), curve.Evaluate(u), kLoose));
        }
        return 0;
    }

    int case_elevate_degree_surface_multi_span()
    {
        const double ua = -3.0, ub = 5.0, va = 10.0, vb = 12.0;
        // Give each direction an interior knot so a single-segment formula
        // would under-allocate the control grid.
        Surface surface = MakeRationalCylinder(ua, ub, va, vb).InsertKnotV(11.0);
        surface = surface.InsertKnotU(-1.0);

        Surface elevated_u = surface.ElevateDegreeU(1);
        TASK_CHECK(elevated_u.DegreeU == surface.DegreeU + 1);
        TASK_CHECK(elevated_u.KnotsU.Count() - elevated_u.DegreeU - 1 == elevated_u.ControlPoints.UCount);
        TASK_CHECK(elevated_u.ControlPoints.VCount == surface.ControlPoints.VCount);
        TASK_CHECK(elevated_u.ControlPoints.Values.size() ==
                   (size_t)(elevated_u.ControlPoints.UCount * elevated_u.ControlPoints.VCount));

        Surface elevated_both = surface.ElevateDegreeU(1).ElevateDegreeV(1);
        TASK_CHECK(elevated_both.KnotsU.Count() - elevated_both.DegreeU - 1 == elevated_both.ControlPoints.UCount);
        TASK_CHECK(elevated_both.KnotsV.Count() - elevated_both.DegreeV - 1 == elevated_both.ControlPoints.VCount);
        TASK_CHECK(elevated_both.ControlPoints.Values.size() ==
                   (size_t)(elevated_both.ControlPoints.UCount * elevated_both.ControlPoints.VCount));
        for (double u : {ua, -1.0, 0.5, 3.0, ub})
        {
            for (double v : {va, 11.0, vb})
            {
                TASK_CHECK(Near(elevated_u.Evaluate(u, v), surface.Evaluate(u, v), kLoose));
                TASK_CHECK(Near(elevated_both.Evaluate(u, v), surface.Evaluate(u, v), kLoose));
                TASK_CHECK(Near(elevated_both.EvaluateDerivative(u, v, 1, 0),
                                surface.EvaluateDerivative(u, v, 1, 0), 1e-7));
            }
        }
        return 0;
    }

    int case_reverse_axis()
    {
        const double a = -3.0;
        const double b = 5.0;
        Curve curve;
        curve.Degree = 2;
        curve.Knots = KnotVector{{a, a, a, -1, 1, b, b, b}};
        curve.ControlPoints = {Vec4(1, 0, 0, 1), Vec4(1, 1, 0, 0.7), Vec4(0, 1, 0, 1.4),
                               Vec4(-1, 1, 0, 0.8), Vec4(-1, 0, 0, 1)};

        Curve reversed = curve;
        reversed.Knots.Reverse();
        std::reverse(reversed.ControlPoints.begin(), reversed.ControlPoints.end());
        for (double u : {a, -2.2, -0.5, 0.7, 2.4, b})
        {
            TASK_CHECK(Near(reversed.Evaluate(a + b - u), curve.Evaluate(u), kLoose));
            TASK_CHECK(Near(reversed.EvaluateDerivative(a + b - u, 1),
                            -curve.EvaluateDerivative(u, 1), 1e-7));
        }

        // AlignParameterDomain reverse path: u runs along -X, v along +Y.
        Surface surface;
        surface.DegreeU = 1;
        surface.DegreeV = 1;
        surface.KnotsU = KnotVector{{a, a, b, b}};
        surface.KnotsV = KnotVector{{10, 10, 12, 12}};
        surface.ControlPoints = ControlPointGrid(2, 2);
        surface.ControlPoints.Get(0, 0) = Vec4(0, 0, 0, 1);
        surface.ControlPoints.Get(1, 0) = Vec4(-2, 0, 0, 1);
        surface.ControlPoints.Get(0, 1) = Vec4(0, 2, 0, 1);
        surface.ControlPoints.Get(1, 1) = Vec4(-2, 2, 0, 1);

        Surface aligned = surface.AlignParameterDomain(Surface::AlignAxis::XAxis,
                                                       Surface::AlignAxis::YAxis);
        auto [aligned_ua, aligned_ub] = aligned.GetDomainU();
        auto [aligned_va, aligned_vb] = aligned.GetDomainV();
        TASK_CHECK(Near(aligned_ua, a, 0) && Near(aligned_ub, b, 0));
        TASK_CHECK(Near(aligned_va, 10.0, 0) && Near(aligned_vb, 12.0, 0));
        for (double u : {a, -1.5, 0.5, 3.0, b})
        {
            for (double v : {10.0, 11.0, 12.0})
            {
                TASK_CHECK(Near(aligned.Evaluate(aligned_ua + aligned_ub - u, v),
                                surface.Evaluate(u, v), kLoose));
                TASK_CHECK(Near(aligned.EvaluateDerivative(aligned_ua + aligned_ub - u, v, 1, 0),
                                -surface.EvaluateDerivative(u, v, 1, 0), 1e-8));
            }
        }
        return 0;
    }

    int case_file_roundtrip()
    {
        const double ua = -3.0, ub = 5.0, va = 10.0, vb = 12.0;
        Surface original = MakeRationalCylinder(ua, ub, va, vb)
                               .InsertKnotU(-1.0)
                               .InsertKnotV(11.0)
                               .ElevateDegreeU(1);

        for (bool binary : {false, true})
        {
            std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
            original.SaveToFile(stream, binary);
            stream.seekg(0);
            Surface loaded;
            loaded.LoadFromFile(stream);

            TASK_CHECK(loaded.DegreeU == original.DegreeU);
            TASK_CHECK(loaded.DegreeV == original.DegreeV);
            TASK_CHECK(loaded.KnotsU.Values().size() == original.KnotsU.Values().size());
            TASK_CHECK(loaded.KnotsV.Values().size() == original.KnotsV.Values().size());
            for (size_t i = 0; i < original.KnotsU.Values().size(); ++i)
            {
                TASK_CHECK(loaded.KnotsU.Values()[i] == original.KnotsU.Values()[i]);
            }
            for (size_t i = 0; i < original.KnotsV.Values().size(); ++i)
            {
                TASK_CHECK(loaded.KnotsV.Values()[i] == original.KnotsV.Values()[i]);
            }
            TASK_CHECK(loaded.ControlPoints.UCount == original.ControlPoints.UCount);
            TASK_CHECK(loaded.ControlPoints.VCount == original.ControlPoints.VCount);
            for (int v = 0; v < original.ControlPoints.VCount; ++v)
            {
                for (int u = 0; u < original.ControlPoints.UCount; ++u)
                {
                    TASK_CHECK(Near(loaded.ControlPoints.Get(u, v).w(),
                                    original.ControlPoints.Get(u, v).w(), 0));
                    TASK_CHECK(Near(loaded.ControlPoints.Get(u, v).head<3>(),
                                    original.ControlPoints.Get(u, v).head<3>(), kTight));
                }
            }
            for (double u : {ua, -1.0, 1.5, ub})
            {
                for (double v : {va, 11.0, vb})
                {
                    TASK_CHECK(Near(loaded.Evaluate(u, v), original.Evaluate(u, v), kTight));
                    TASK_CHECK(Near(loaded.EvaluateDerivative(u, v, 1, 0),
                                    original.EvaluateDerivative(u, v, 1, 0), kLoose));
                    TASK_CHECK(Near(loaded.EvaluateDerivative(u, v, 0, 1),
                                    original.EvaluateDerivative(u, v, 0, 1), kLoose));
                    TASK_CHECK(Near(loaded.EvaluateDerivative(u, v, 1, 1),
                                    original.EvaluateDerivative(u, v, 1, 1), kLoose));
                }
            }
        }
        return 0;
    }

    int case_curve_file_roundtrip()
    {
        const double a = -3.0, b = 5.0;
        Curve original = MakeRationalQuarterArc(a, b).InsertKnot(0.5).ElevateDegree(1);
        for (bool binary : {false, true})
        {
            std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
            original.SaveToFile(stream, binary);
            stream.seekg(0);
            Curve loaded;
            loaded.LoadFromFile(stream);
            TASK_CHECK(loaded.Degree == original.Degree);
            TASK_CHECK(loaded.Knots.Values() == original.Knots.Values());
            TASK_CHECK(loaded.ControlPoints.size() == original.ControlPoints.size());
            for (size_t i = 0; i < original.ControlPoints.size(); ++i)
            {
                TASK_CHECK(Near(loaded.ControlPoints[i].w(), original.ControlPoints[i].w(), 0));
                TASK_CHECK(Near(loaded.ControlPoints[i].head<3>(),
                                original.ControlPoints[i].head<3>(), kTight));
            }
            for (double u : {a, -1.5, 0.0, 1.5, b})
            {
                TASK_CHECK(Near(loaded.Evaluate(u), original.Evaluate(u), kTight));
                TASK_CHECK(Near(loaded.EvaluateDerivative(u, 1),
                                original.EvaluateDerivative(u, 1), kLoose));
            }
        }
        return 0;
    }

    int case_mesh_plane()
    {
        const double ua = -3.0, ub = 5.0, va = 10.0, vb = 12.0;
        const Vec3 p00(1, 2, 3), p10(9, 2, 3), p01(1, 6, 3), p11(9, 6, 3);
        Surface plane = MakeBilinearPlane(ua, ub, va, vb, p00, p10, p01, p11);

        auto mesh = BuildSurfaceMesh(plane, {-3.0, 0.0, 5.0}, {10.0, 12.0});
        TASK_CHECK(mesh.positions.size() == 6);
        TASK_CHECK(mesh.triangles.size() == 4);
        TASK_CHECK(mesh.corner_uv.size() == mesh.triangles.size() * 3);
        TASK_CHECK(mesh.corner_normals.size() == mesh.triangles.size() * 3);

        for (size_t t = 0; t < mesh.triangles.size(); ++t)
        {
            const Vec2 a = mesh.corner_uv[3 * t];
            const Vec2 b2 = mesh.corner_uv[3 * t + 1];
            const Vec2 c = mesh.corner_uv[3 * t + 2];
            const double area = (b2.x() - a.x()) * (c.y() - a.y()) - (b2.y() - a.y()) * (c.x() - a.x());
            TASK_CHECK(area > 0); // CCW in UV
            for (int corner = 0; corner < 3; ++corner)
            {
                const int index = mesh.triangles[t][corner];
                const Vec2 uv = mesh.corner_uv[3 * t + corner];
                const Vec3 expected = plane.Evaluate(uv.x(), uv.y());
                TASK_CHECK(Near(mesh.positions[index], expected, kTight));
                TASK_CHECK(Near(mesh.corner_normals[3 * t + corner], Vec3(0, 0, 1), kLoose));
            }
        }
        return 0;
    }

    int case_mesh_merges_interior_knots()
    {
        const double ua = -3.0, ub = 5.0, va = 10.0, vb = 12.0;
        // Interior knots at U=-1 and V=11, not passed as sites.
        Surface surface = MakeRationalCylinder(ua, ub, va, vb).InsertKnotU(-1.0).InsertKnotV(11.0);
        auto mesh = BuildSurfaceMesh(surface, {ua, ub}, {va, vb});
        // 4 U stations (ua, -1, ub plus the degree-2 interior? no: knots -3,-3,-3,-1,5,5,5)
        // -> U stations: -3, -1, 5 (3). V stations: 10, 11, 12 (3).
        TASK_CHECK(mesh.positions.size() == 9);
        TASK_CHECK(mesh.triangles.size() == 8);

        // Verify only a subset of triangles touch the merged interior stations.
        bool saw_interior_u = false;
        bool saw_interior_v = false;
        for (const auto& uv : mesh.corner_uv)
        {
            if (Near(uv.x(), -1.0, 0)) saw_interior_u = true;
            if (Near(uv.y(), 11.0, 0)) saw_interior_v = true;
        }
        TASK_CHECK(saw_interior_u);
        TASK_CHECK(saw_interior_v);
        return 0;
    }

    int case_mesh_crease_normals()
    {
        // Non-square control net with a C0 crease at U = -1 (multiplicity 2).
        Surface surface;
        surface.DegreeU = 2;
        surface.DegreeV = 1;
        surface.KnotsU = KnotVector{{-3, -3, -3, -1, -1, 3, 3, 3}};
        surface.KnotsV = KnotVector{{7, 7, 9, 9}};
        surface.ControlPoints = ControlPointGrid(5, 2);
        const double heights[5] = {0.0, 0.0, 0.0, 1.0, 2.0};
        for (int v = 0; v < 2; ++v)
        {
            for (int u = 0; u < 5; ++u)
            {
                surface.ControlPoints.Get(u, v) = Vec4(u, 2.0 * v, heights[u], 1);
            }
        }

        auto mesh = BuildSurfaceMesh(surface, {-3.0, 0.0, 3.0}, {7.0, 9.0});
        const Vec3 crease_position = surface.Evaluate(-1.0, 7.0);

        Vec3 first_normal = Vec3::Zero();
        Vec3 second_normal = Vec3::Zero();
        int corners = 0;
        for (size_t t = 0; t < mesh.triangles.size(); ++t)
        {
            for (int corner = 0; corner < 3; ++corner)
            {
                const Vec2 uv = mesh.corner_uv[3 * t + corner];
                if (!Near(uv.x(), -1.0, 0) || !Near(uv.y(), 7.0, 0)) continue;
                const int index = mesh.triangles[t][corner];
                TASK_CHECK(Near(mesh.positions[index], crease_position, kTight));
                const Vec3 normal = mesh.corner_normals[3 * t + corner];
                TASK_CHECK(Near(normal.norm(), 1.0, 1e-9));
                if (corners == 0) first_normal = normal;
                else if (corners == 1) second_normal = normal;
                ++corners;
            }
        }
        // Shared position, but a distinct normal on each side of the crease.
        TASK_CHECK(corners >= 2);
        TASK_CHECK((first_normal - second_normal).norm() > 1e-3);
        return 0;
    }

    int case_mesh_position_identity()
    {
        // A folded planform NURBS: two distinct U parameters map to the same
        // spatial point. Positions must be keyed by parameter identity, not
        // merged by spatial proximity.
        Surface surface;
        surface.DegreeU = 1;
        surface.DegreeV = 1;
        surface.KnotsU = KnotVector{{0, 0, 0.5, 1, 1}};
        surface.KnotsV = KnotVector{{0, 0, 1, 1}};
        surface.ControlPoints = ControlPointGrid(3, 2);
        for (int v = 0; v < 2; ++v)
        {
            surface.ControlPoints.Get(0, v) = Vec4(0, v, 0, 1);
            surface.ControlPoints.Get(1, v) = Vec4(2, v, 0, 1);
            surface.ControlPoints.Get(2, v) = Vec4(0, v, 0, 1);
        }
        // Stations: 0, 0.25, 0.5, 0.75, 1 -> 5 x 2 grid.
        auto mesh = BuildSurfaceMesh(surface, {0.0, 0.25, 0.75, 1.0}, {0.0, 1.0});
        TASK_CHECK(mesh.positions.size() == 10);
        TASK_CHECK(mesh.triangles.size() == 8);
        // u=0.25 and u=0.75 both evaluate to x=1, y=v; parameter identity
        // keeps them as separate vertices.
        const Vec3 at_quarter = surface.Evaluate(0.25, 0.0);
        const Vec3 at_three_quarter = surface.Evaluate(0.75, 0.0);
        TASK_CHECK(Near(at_quarter, at_three_quarter, kTight));
        int distinct_vertices_with_same_position = 0;
        for (const auto& position : mesh.positions)
        {
            if (Near(position, at_quarter, kTight)) ++distinct_vertices_with_same_position;
        }
        TASK_CHECK(distinct_vertices_with_same_position == 2);
        return 0;
    }

    int case_mesh_invalid_and_degenerate()
    {
        const double ua = -3.0, ub = 5.0, va = 10.0, vb = 12.0;
        Surface plane = MakeBilinearPlane(ua, ub, va, vb,
                                          Vec3(0, 0, 0), Vec3(8, 0, 0),
                                          Vec3(0, 2, 0), Vec3(8, 2, 0));

        auto rejected = [](const Surface& surface, const std::vector<double>& us,
                           const std::vector<double>& vs)
        {
            try
            {
                (void)BuildSurfaceMesh(surface, us, vs);
                return false;
            }
            catch (const std::invalid_argument&)
            {
                return true;
            }
        };
        TASK_CHECK(rejected(plane, {-3.0, 4.0}, {10.0, 12.0}));       // missing endpoint
        TASK_CHECK(rejected(plane, {-3.0, 5.0, 6.0}, {10.0, 12.0}));  // outside domain
        TASK_CHECK(rejected(plane, {-3.0, 0.0, 0.0, 5.0}, {10.0, 12.0})); // not strictly increasing
        TASK_CHECK(rejected(plane, {-3.0, 5.0}, {12.0, 10.0}));       // decreasing
        TASK_CHECK(rejected(plane, {-3.0}, {10.0, 12.0}));            // too few

        // Degenerate one-sided normal: the U direction collapses to a point.
        Surface degenerate;
        degenerate.DegreeU = 1;
        degenerate.DegreeV = 1;
        degenerate.KnotsU = KnotVector{{-2, -2, 2, 2}};
        degenerate.KnotsV = KnotVector{{0, 0, 1, 1}};
        degenerate.ControlPoints = ControlPointGrid(2, 2);
        degenerate.ControlPoints.Values = {Vec4(0, 0, 0, 1), Vec4(0, 1, 0, 1),
                                           Vec4(0, 0, 0, 1), Vec4(0, 1, 0, 1)};
        bool runtime_threw = false;
        try
        {
            (void)BuildSurfaceMesh(degenerate, {-2.0, 2.0}, {0.0, 1.0});
        }
        catch (const std::runtime_error&)
        {
            runtime_threw = true;
        }
        TASK_CHECK(runtime_threw);

        // The inputs remain untouched after a failed call.
        TASK_CHECK(degenerate.KnotsU.Values().size() == 4);
        TASK_CHECK(degenerate.ControlPoints.Values.size() == 4);
        TASK_CHECK(degenerate.ControlPoints.UCount == 2 && degenerate.ControlPoints.VCount == 2);
        return 0;
    }

    } // namespace

extern const struct task_case task_cases[];
extern const size_t task_case_count;

const struct task_case task_cases[] = {
    {"domain", "knot_domain_bounds", case_knot_domain_bounds},
    {"domain", "curve_analytic_derivatives", case_curve_analytic_derivatives},
    {"domain", "surface_analytic_derivatives", case_surface_analytic_derivatives},
    {"domain", "affine_reparameterization", case_affine_reparameterization},
    {"domain", "out_of_domain_and_illegal_knots", case_out_of_domain_and_illegal_knots},
    {"domain", "curve_inverse_monotone_quadratic", case_curve_inverse_monotone_quadratic},
    {"domain", "surface_inverse_rational_cylinder", case_surface_inverse_rational_cylinder},
    {"domain", "wide_domain_inverse", case_wide_domain_inverse},
    {"editing", "insert_knot_curve", case_insert_knot_curve},
    {"editing", "insert_knot_surface_both_axes", case_insert_knot_surface_both_axes},
    {"editing", "elevate_degree_multi_span", case_elevate_degree_multi_span},
    {"editing", "elevate_degree_surface_multi_span", case_elevate_degree_surface_multi_span},
    {"editing", "reverse_axis", case_reverse_axis},
    {"editing", "surface_file_roundtrip", case_file_roundtrip},
    {"editing", "curve_file_roundtrip", case_curve_file_roundtrip},
    {"mesh", "mesh_plane", case_mesh_plane},
    {"mesh", "mesh_merges_interior_knots", case_mesh_merges_interior_knots},
    {"mesh", "mesh_crease_normals", case_mesh_crease_normals},
    {"mesh", "mesh_position_identity", case_mesh_position_identity},
    {"mesh", "mesh_invalid_and_degenerate", case_mesh_invalid_and_degenerate},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

