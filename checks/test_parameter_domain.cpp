// Checks for arbitrary clamped parameter domains, domain-preserving editing
// operations and the SurfaceMesh export. Registered through task_support/check.h;
// no main() here (the driver provides it).

#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <libnurbs/Curve/Curve.hpp>
#include <libnurbs/Surface/Surface.hpp>
#include <libnurbs/Surface/SurfaceMesh.hpp>

using namespace libnurbs;

namespace
{
    bool Near(double a, double b, double tol = 1e-9)
    {
        return std::abs(a - b) < tol;
    }

    bool Near3(const Vec3& a, const Vec3& b, double tol = 1e-9)
    {
        return (a - b).norm() < tol;
    }

    template <typename F>
    bool ThrowsInvalid(F&& f)
    {
        try
        {
            f();
        }
        catch (const std::invalid_argument&)
        {
            return true;
        }
        catch (...)
        {
            return false;
        }
        return false;
    }

    template <typename F>
    bool ThrowsRuntime(F&& f)
    {
        try
        {
            f();
        }
        catch (const std::runtime_error&)
        {
            return true;
        }
        catch (...)
        {
            return false;
        }
        return false;
    }

    // C(u) = (u, u^2 / 4, 0) on U = [-3, 5]; a monotonic quadratic rail, flat
    // enough that the distance to a point on the curve has a unique minimizer.
    Curve MakeRailCurve()
    {
        Curve curve;
        curve.Degree = 2;
        curve.Knots  = KnotVector{{-3, -3, -3, 5, 5, 5}};
        // Bezier control points of (u, u^2 / 4): blossom of u^2 is u1*u2.
        curve.ControlPoints = {Vec4(-3, 2.25, 0, 1), Vec4(1, -3.75, 0, 1), Vec4(5, 6.25, 0, 1)};
        return curve;
    }

    // Same rail but with an internal knot (two spans).
    Curve MakeRailCurveMultiSpan()
    {
        Curve curve;
        curve.Degree = 2;
        curve.Knots  = KnotVector{{-3, -3, -3, 1, 5, 5, 5}};
        curve.ControlPoints = {Vec4(-3, 2.25, 0, 1), Vec4(-1, -0.75, 0, 1),
                               Vec4(3, 1.25, 0, 1), Vec4(5, 6.25, 0, 1)};
        return curve;
    }

    const Vec3 PLANE_P0{0.3, -0.2, 0.5};
    const Vec3 PLANE_A{0.4, -0.1, 0.2};
    const Vec3 PLANE_B{-0.2, 0.3, 0.6};

    // S(u, v) = P0 + A u + B v on U = [-3, 5] (internal knot 1), V = [10, 12].
    Surface MakePlane()
    {
        Surface s;
        s.DegreeU = 2;
        s.DegreeV = 1;
        s.KnotsU  = KnotVector{{-3, -3, -3, 1, 5, 5, 5}};
        s.KnotsV  = KnotVector{{10, 10, 12, 12}};
        const double greville_u[4] = {-3, -1, 3, 5};
        const double greville_v[2] = {10, 12};
        s.ControlPoints = ControlPointGrid(4, 2);
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 4; ++i)
            {
                Vec3 p = PLANE_P0 + PLANE_A * greville_u[i] + PLANE_B * greville_v[j];
                s.ControlPoints.Get(i, j) = Vec4(p.x(), p.y(), p.z(), 1.0);
            }
        return s;
    }

    const Vec3 BIL_C{0.1, 0.2, -0.3};
    const Vec3 BIL_D{0.05, -0.02, 0.1};

    // S(u, v) = P0 + A u + B v + C u v + D u^2 on U = [-3, 5], V = [10, 12].
    Surface MakeBilinear()
    {
        Surface s;
        s.DegreeU = 2;
        s.DegreeV = 1;
        s.KnotsU  = KnotVector{{-3, -3, -3, 1, 5, 5, 5}};
        s.KnotsV  = KnotVector{{10, 10, 12, 12}};
        const double greville_u[4] = {-3, -1, 3, 5};
        const double blossom_u2[4] = {9, -3, 5, 25}; // k[i+1]*k[i+2]
        const double greville_v[2] = {10, 12};
        s.ControlPoints = ControlPointGrid(4, 2);
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 4; ++i)
            {
                Vec3 p = PLANE_P0 + PLANE_A * greville_u[i] + PLANE_B * greville_v[j]
                         + BIL_C * (greville_u[i] * greville_v[j]) + BIL_D * blossom_u2[i];
                s.ControlPoints.Get(i, j) = Vec4(p.x(), p.y(), p.z(), 1.0);
            }
        return s;
    }

    constexpr double CYL_R   = 2.0;
    constexpr double CYL_TH0 = 0.2;
    constexpr double CYL_TH1 = 1.4;

    // Rational cylinder band: (R cos t, R sin t, v), t in [0.2, 1.4] as u sweeps
    // U = [-3, 5]; V = [10, 12] maps to z. Quadratic rational in u.
    Surface MakeCylinder()
    {
        Surface s;
        s.DegreeU = 2;
        s.DegreeV = 1;
        s.KnotsU  = KnotVector{{-3, -3, -3, 5, 5, 5}};
        s.KnotsV  = KnotVector{{10, 10, 12, 12}};
        const double thm  = 0.5 * (CYL_TH0 + CYL_TH1);
        const double half = 0.5 * (CYL_TH1 - CYL_TH0);
        const double w1   = std::cos(half);
        const double p0x = CYL_R * std::cos(CYL_TH0), p0y = CYL_R * std::sin(CYL_TH0);
        const double p1x = CYL_R / w1 * std::cos(thm), p1y = CYL_R / w1 * std::sin(thm);
        const double p2x = CYL_R * std::cos(CYL_TH1), p2y = CYL_R * std::sin(CYL_TH1);
        s.ControlPoints = ControlPointGrid(3, 2);
        const double zs[2] = {10, 12};
        for (int j = 0; j < 2; ++j)
        {
            s.ControlPoints.Get(0, j) = Vec4(p0x, p0y, zs[j], 1.0);
            s.ControlPoints.Get(1, j) = Vec4(p1x, p1y, zs[j], w1);
            s.ControlPoints.Get(2, j) = Vec4(p2x, p2y, zs[j], 1.0);
        }
        return s;
    }

    // C0 crease surface on a non-square (5 x 2) control net:
    // S(u, v) = (u, f(u), v) with f(u) = u + 1 for u <= 0.5, 2u + 0.5 for u >= 0.5.
    // U = [-1, 2] with internal knot 0.5 of multiplicity 2 (= degree), V = [3, 7].
    Surface MakeCrease()
    {
        Surface s;
        s.DegreeU = 2;
        s.DegreeV = 1;
        s.KnotsU  = KnotVector{{-1, -1, -1, 0.5, 0.5, 2, 2, 2}};
        s.KnotsV  = KnotVector{{3, 3, 7, 7}};
        const double greville_u[5] = {-1, -0.25, 0.5, 1.25, 2};
        auto f = [](double u) { return u <= 0.5 ? u + 1.0 : 2.0 * u + 0.5; };
        const double zs[2] = {3, 7};
        s.ControlPoints = ControlPointGrid(5, 2);
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 5; ++i)
                s.ControlPoints.Get(i, j) = Vec4(greville_u[i], f(greville_u[i]), zs[j], 1.0);
        return s;
    }

    void ReparamKnots(KnotVector& knots, double alpha, double beta)
    {
        for (auto& k : knots.Values())
            k = alpha * k + beta;
    }

    bool KnotsEqual(const KnotVector& a, const KnotVector& b)
    {
        if (a.Count() != b.Count()) return false;
        for (int i = 0; i < a.Count(); ++i)
            if (a(i) != b(i)) return false;
        return true;
    }

    // ------------------------------------------------------------------ domain

    int DomainCurveRail()
    {
        Curve rail = MakeRailCurve();
        TASK_CHECK(Near(rail.DomainStart(), -3));
        TASK_CHECK(Near(rail.DomainEnd(), 5));
        for (double u : {-2.5, 0.3, 4.9})
        {
            TASK_CHECK(Near3(rail.Evaluate(u), Vec3(u, 0.25 * u * u, 0), 1e-9));
            auto all = rail.EvaluateAll(u, 2);
            TASK_CHECK(Near3(all[0], Vec3(u, 0.25 * u * u, 0), 1e-9));
            TASK_CHECK(Near3(all[1], Vec3(1, 0.5 * u, 0), 1e-9));
            TASK_CHECK(Near3(all[2], Vec3(0, 0.5, 0), 1e-9));
        }
        // Inverse solve from an in-domain, non-solution initial value.
        const double u_star = 1.7;
        const Vec3 point    = rail.Evaluate(u_star);
        double u_found      = rail.SearchParameter(point, -2.0, 1e-8);
        TASK_CHECK((rail.Evaluate(u_found) - point).norm() < 1e-6);
        TASK_CHECK(Near(u_found, u_star, 1e-4));
        double u_bin = rail.BinarySearchParameter(point, 1e-8);
        TASK_CHECK((rail.Evaluate(u_bin) - point).norm() < 1e-6);
        TASK_CHECK(Near(u_bin, u_star, 1e-4));

        // Positive affine reparametrization u' = 0.5 u + 1: positions unchanged,
        // derivatives scale by the chain rule, inverse solve still converges.
        Curve re = rail;
        ReparamKnots(re.Knots, 0.5, 1.0);
        TASK_CHECK(Near(re.DomainStart(), -0.5));
        TASK_CHECK(Near(re.DomainEnd(), 3.5));
        for (double u : {-2.5, 0.3, 4.9})
        {
            const double up = 0.5 * u + 1.0;
            TASK_CHECK(Near3(re.Evaluate(up), rail.Evaluate(u), 1e-9));
            TASK_CHECK(Near3(re.EvaluateDerivative(up, 1), rail.EvaluateDerivative(u, 1) / 0.5, 1e-8));
            TASK_CHECK(Near3(re.EvaluateDerivative(up, 2), rail.EvaluateDerivative(u, 2) / 0.25, 1e-8));
        }
        const double up_star = 0.5 * u_star + 1.0;
        double up_found      = re.SearchParameter(point, 3.4, 1e-8);
        TASK_CHECK((re.Evaluate(up_found) - point).norm() < 1e-6);
        TASK_CHECK(Near(up_found, up_star, 1e-4));
        double up_bin = re.BinarySearchParameter(point, 1e-8);
        TASK_CHECK((re.Evaluate(up_bin) - point).norm() < 1e-6);
        TASK_CHECK(Near(up_bin, up_star, 1e-4));
        return 0;
    }

    int DomainPlane()
    {
        Surface plane = MakePlane();
        for (double u : {-2.7, 0.4, 4.5})
            for (double v : {10.3, 11.2, 11.9})
            {
                TASK_CHECK(Near3(plane.Evaluate(u, v), PLANE_P0 + PLANE_A * u + PLANE_B * v, 1e-9));
                auto all = plane.EvaluateAll(u, v, 2, 2);
                TASK_CHECK(Near3(all.Get(1, 0), PLANE_A, 1e-9));
                TASK_CHECK(Near3(all.Get(0, 1), PLANE_B, 1e-9));
                TASK_CHECK(all.Get(2, 0).norm() < 1e-9);
                TASK_CHECK(all.Get(1, 1).norm() < 1e-9);
                TASK_CHECK(all.Get(0, 2).norm() < 1e-9);
            }
        // Inverse solves from in-domain, non-solution initial values.
        const double u_star = 0.4, v_star = 11.2;
        const Vec3 point = plane.Evaluate(u_star, v_star);
        auto [u1, v1] = plane.SearchParameter(point, -2.9, 10.1, 1e-8);
        TASK_CHECK((plane.Evaluate(u1, v1) - point).norm() < 1e-6);
        TASK_CHECK(Near(u1, u_star, 1e-4) && Near(v1, v_star, 1e-4));
        // Default initial value 0.5 lies outside the V domain [10, 12] and must be
        // projected onto the nearest boundary.
        auto [u2, v2] = plane.SearchParameter(point);
        TASK_CHECK((plane.Evaluate(u2, v2) - point).norm() < 1e-6);
        TASK_CHECK(Near(u2, u_star, 1e-4) && Near(v2, v_star, 1e-4));
        auto [u3, v3] = plane.SearchParameterOn(point, 1, v_star, -2.9, 1e-8);
        TASK_CHECK(Near(u3, u_star, 1e-4) && Near(v3, v_star, 1e-9));
        auto [u4, v4] = plane.SearchParameterOn(point, 0, u_star, 10.1, 1e-8);
        TASK_CHECK(Near(v4, v_star, 1e-4) && Near(u4, u_star, 1e-9));
        auto [u5, v5] = plane.BinarySearchParameterOn(point, 1, v_star, 1e-8);
        TASK_CHECK(Near(u5, u_star, 1e-4) && Near(v5, v_star, 1e-12));
        auto [u6, v6] = plane.BinarySearchParameterOn(point, 0, u_star, 1e-8);
        TASK_CHECK(Near(v6, v_star, 1e-4) && Near(u6, u_star, 1e-12));
        // Inverse solve also works after independent positive affine
        // reparametrization of each axis.
        Surface re = plane;
        ReparamKnots(re.KnotsU, 0.5, 1.0);  // u' = 0.5 u + 1
        ReparamKnots(re.KnotsV, 1.5, -2.0); // v' = 1.5 v - 2
        const double up_star = 0.5 * u_star + 1.0;
        const double vp_star = 1.5 * v_star - 2.0;
        TASK_CHECK(Near3(re.Evaluate(up_star, vp_star), point, 1e-9));
        auto [u7, v7] = re.SearchParameter(point, -0.4, 15.8, 1e-8);
        TASK_CHECK((re.Evaluate(u7, v7) - point).norm() < 1e-6);
        TASK_CHECK(Near(u7, up_star, 1e-4) && Near(v7, vp_star, 1e-4));
        auto [u8, v8] = re.BinarySearchParameterOn(point, 1, vp_star, 1e-8);
        TASK_CHECK(Near(u8, up_star, 1e-4) && Near(v8, vp_star, 1e-12));
        return 0;
    }

    int DomainMixedPartials()
    {
        Surface s = MakeBilinear();
        auto analytic = [&](double u, double v, int ou, int ov) -> Vec3
        {
            if (ou == 0 && ov == 0) return PLANE_P0 + PLANE_A * u + PLANE_B * v + BIL_C * (u * v) + BIL_D * (u * u);
            if (ou == 1 && ov == 0) return PLANE_A + BIL_C * v + BIL_D * (2 * u);
            if (ou == 0 && ov == 1) return PLANE_B + BIL_C * u;
            if (ou == 1 && ov == 1) return BIL_C;
            if (ou == 2 && ov == 0) return BIL_D * 2.0;
            if (ou == 0 && ov == 2) return Vec3::Zero();
            return Vec3::Zero();
        };
        for (double u : {-2.7, 0.4, 4.5})
            for (double v : {10.3, 11.7})
            {
                auto all = s.EvaluateAll(u, v, 2, 2);
                for (int ou = 0; ou <= 2; ++ou)
                    for (int ov = 0; ov <= 2; ++ov)
                        TASK_CHECK(Near3(all.Get(ou, ov), analytic(u, v, ou, ov), 1e-8));
            }
        // Independent positive affine reparametrization of each axis.
        Surface re = s;
        ReparamKnots(re.KnotsU, 0.5, 1.0);  // u' = 0.5 u + 1
        ReparamKnots(re.KnotsV, 1.5, -2.0); // v' = 1.5 v - 2
        TASK_CHECK(Near(re.DomainStartU(), -0.5) && Near(re.DomainEndU(), 3.5));
        TASK_CHECK(Near(re.DomainStartV(), 13.0) && Near(re.DomainEndV(), 16.0));
        for (double u : {-2.7, 0.4, 4.5})
            for (double v : {10.3, 11.7})
            {
                const double up = 0.5 * u + 1.0;
                const double vp = 1.5 * v - 2.0;
                auto all = re.EvaluateAll(up, vp, 2, 2);
                TASK_CHECK(Near3(all.Get(0, 0), analytic(u, v, 0, 0), 1e-8));
                TASK_CHECK(Near3(all.Get(1, 0), analytic(u, v, 1, 0) / 0.5, 1e-7));
                TASK_CHECK(Near3(all.Get(0, 1), analytic(u, v, 0, 1) / 1.5, 1e-7));
                TASK_CHECK(Near3(all.Get(1, 1), analytic(u, v, 1, 1) / 0.75, 1e-7));
                TASK_CHECK(Near3(all.Get(2, 0), analytic(u, v, 2, 0) / 0.25, 1e-7));
                TASK_CHECK(all.Get(0, 2).norm() < 1e-9);
            }
        return 0;
    }

    int DomainCylinder()
    {
        Surface cyl = MakeCylinder();
        TASK_CHECK(cyl.IsRational());
        for (double u : {-2.9, 0.7, 4.9})
            for (double v : {10.0, 10.6, 12.0})
            {
                Vec3 p = cyl.Evaluate(u, v);
                TASK_CHECK(Near(std::hypot(p.x(), p.y()), CYL_R, 1e-9));
                TASK_CHECK(Near(p.z(), v, 1e-12));
                const double theta = std::atan2(p.y(), p.x());
                auto all = cyl.EvaluateAll(u, v, 2, 1);
                Vec3 su  = all.Get(1, 0);
                Vec3 sv  = all.Get(0, 1);
                Vec3 suu = all.Get(2, 0);
                const Vec3 radial(std::cos(theta), std::sin(theta), 0);
                // Tangent is perpendicular to the radius and wraps forward.
                TASK_CHECK(Near(su.normalized().dot(radial), 0.0, 1e-8));
                TASK_CHECK(Near3(su.normalized(), Vec3(-std::sin(theta), std::cos(theta), 0), 1e-8));
                TASK_CHECK(Near3(sv, Vec3(0, 0, 1), 1e-12));
                // Analytic unit normal points outward.
                TASK_CHECK(Near3(su.cross(sv).normalized(), radial, 1e-8));
                // Mixed partial vanishes; second u derivative has no z part and
                // pulls inward (centripetal component).
                TASK_CHECK(all.Get(1, 1).norm() < 1e-10);
                TASK_CHECK(Near(suu.z(), 0.0, 1e-12));
                TASK_CHECK(suu.dot(radial) < 0);
            }
        TASK_CHECK(Near(std::atan2(cyl.Evaluate(-3, 10).y(), cyl.Evaluate(-3, 10).x()), CYL_TH0, 1e-12));
        TASK_CHECK(Near(std::atan2(cyl.Evaluate(5, 10).y(), cyl.Evaluate(5, 10).x()), CYL_TH1, 1e-12));

        // Inverse solves from in-domain, non-solution initial values.
        const double u_star = -1.0, v_star = 11.0;
        const Vec3 point = cyl.Evaluate(u_star, v_star);
        auto [u1, v1] = cyl.SearchParameter(point, 4.9, 10.2, 1e-8);
        TASK_CHECK((cyl.Evaluate(u1, v1) - point).norm() < 1e-6);
        TASK_CHECK(Near(u1, u_star, 1e-4) && Near(v1, v_star, 1e-4));
        auto [u2, v2] = cyl.SearchParameterOn(point, 1, v_star, 4.9, 1e-8);
        TASK_CHECK(Near(u2, u_star, 1e-4) && Near(v2, v_star, 1e-12));
        auto [u3, v3] = cyl.BinarySearchParameterOn(point, 1, v_star, 1e-8);
        TASK_CHECK(Near(u3, u_star, 1e-4) && Near(v3, v_star, 1e-12));

        // The same must hold after independent positive affine reparametrization.
        Surface re = cyl;
        ReparamKnots(re.KnotsU, 0.5, 1.0);  // u' = 0.5 u + 1  -> [-0.5, 3.5]
        ReparamKnots(re.KnotsV, 1.5, -2.0); // v' = 1.5 v - 2  -> [13, 16]
        const double up_star = 0.5 * u_star + 1.0;
        const double vp_star = 1.5 * v_star - 2.0;
        TASK_CHECK(Near3(re.Evaluate(up_star, vp_star), point, 1e-9));
        auto [u4, v4] = re.SearchParameter(point, 3.4, 15.9, 1e-8);
        TASK_CHECK((re.Evaluate(u4, v4) - point).norm() < 1e-6);
        TASK_CHECK(Near(u4, up_star, 1e-4) && Near(v4, vp_star, 1e-4));
        auto [u5, v5] = re.SearchParameterOn(point, 1, vp_star, 3.4, 1e-8);
        TASK_CHECK(Near(u5, up_star, 1e-4) && Near(v5, vp_star, 1e-12));
        auto [u6, v6] = re.BinarySearchParameterOn(point, 1, vp_star, 1e-8);
        TASK_CHECK(Near(u6, up_star, 1e-4) && Near(v6, vp_star, 1e-12));
        return 0;
    }

    int DomainValidation()
    {
        // Illegal knot vectors are rejected with std::invalid_argument.
        TASK_CHECK(ThrowsInvalid([] { KnotVector kv{{0, 0, 1, 0.5, 1, 1}}; }));        // decreasing
        TASK_CHECK(ThrowsInvalid([] { KnotVector kv{{0, 0, 5e-7, 1, 1}}; }));         // gap < 1e-6
        TASK_CHECK(ThrowsInvalid([] { KnotVector kv{{0, 0, 10001, 10001}}; }));       // |knot| > 1e4
        TASK_CHECK(ThrowsInvalid([] { KnotVector kv{{2, 2, 2, 2}}; }));               // zero width
        TASK_CHECK(ThrowsInvalid([] { KnotVector kv{{0, 0, NAN, 1, 1}}; }));          // non-finite
        // Knot vector valid on its own but illegal for the degree.
        Curve bad_end;
        bad_end.Degree = 2;
        bad_end.Knots  = KnotVector{{0, 0, 0, 0, 1, 1, 1}}; // end multiplicity 4 != degree + 1
        bad_end.ControlPoints.assign(4, Vec4(0, 0, 0, 1));
        TASK_CHECK(ThrowsInvalid([&] { (void)bad_end.Evaluate(0.5); }));
        Curve bad_mult;
        bad_mult.Degree = 1;
        bad_mult.Knots  = KnotVector{{0, 0, 0.5, 0.5, 0.5, 1, 1}}; // internal mult 3 > degree 1
        bad_mult.ControlPoints.assign(5, Vec4(0, 0, 0, 1));
        TASK_CHECK(ThrowsInvalid([&] { (void)bad_mult.Evaluate(0.5); }));
        Curve bad_degree;
        bad_degree.Degree = 4; // outside the supported range [1, 3]
        bad_degree.Knots  = KnotVector{{0, 0, 0, 0, 0, 1, 1, 1, 1, 1}};
        bad_degree.ControlPoints.assign(5, Vec4(0, 0, 0, 1));
        TASK_CHECK(ThrowsInvalid([&] { (void)bad_degree.Evaluate(0.5); }));
        Curve bad_weight;
        bad_weight.Degree = 1;
        bad_weight.Knots  = KnotVector{{0, 0, 1, 1}};
        bad_weight.ControlPoints = {Vec4(0, 0, 0, 1), Vec4(1, 0, 0, 20.0)};
        TASK_CHECK(ThrowsInvalid([&] { (void)bad_weight.Evaluate(0.5); }));
        bad_weight.ControlPoints = {Vec4(0, 0, 0, 0.05), Vec4(1, 0, 0, 1.0)};
        TASK_CHECK(ThrowsInvalid([&] { (void)bad_weight.Evaluate(0.5); }));
        // Out-of-domain evaluation throws; the domain ends themselves are valid.
        Curve rail = MakeRailCurve();
        TASK_CHECK(ThrowsInvalid([&] { (void)rail.Evaluate(-3.5); }));
        TASK_CHECK(ThrowsInvalid([&] { (void)rail.Evaluate(5.5); }));
        TASK_CHECK(Near3(rail.Evaluate(-3), Vec3(-3, 2.25, 0), 1e-12));
        TASK_CHECK(Near3(rail.Evaluate(5), Vec3(5, 6.25, 0), 1e-12));
        Surface plane = MakePlane();
        TASK_CHECK(ThrowsInvalid([&] { (void)plane.Evaluate(0, 9.9); }));
        TASK_CHECK(ThrowsInvalid([&] { (void)plane.Evaluate(-3.1, 11); }));
        TASK_CHECK(ThrowsInvalid([&] { (void)plane.EvaluateDerivative(0, 12.1, 1, 0); }));
        TASK_CHECK(Near3(plane.Evaluate(-3, 10), PLANE_P0 + PLANE_A * (-3) + PLANE_B * 10, 1e-9));
        TASK_CHECK(Near3(plane.Evaluate(5, 12), PLANE_P0 + PLANE_A * 5 + PLANE_B * 12, 1e-9));
        // Search constants outside the domain are rejected.
        const Vec3 point = plane.Evaluate(0.4, 11.2);
        TASK_CHECK(ThrowsInvalid([&] { (void)plane.SearchParameterOn(point, 0, 9.9); }));
        TASK_CHECK(ThrowsInvalid([&] { (void)plane.BinarySearchParameterOn(point, 1, 12.1); }));
        // Knot insertion outside the open domain is rejected.
        TASK_CHECK(ThrowsInvalid([&] { (void)rail.InsertKnot(-3.0); }));
        TASK_CHECK(ThrowsInvalid([&] { (void)rail.InsertKnot(6.0); }));
        TASK_CHECK(ThrowsInvalid([&] { (void)plane.InsertKnotU(5.0); }));
        return 0;
    }

    // ----------------------------------------------------------------- editing

    int EditingKnotInsertion()
    {
        // Curve: insert new and repeated knots, geometry must not change.
        Curve rail = MakeRailCurve();
        std::vector<double> new_knots{0.0, 2.0, 2.0};
        Curve refined = rail.InsertKnot(new_knots);
        TASK_CHECK(refined.Knots.Count() == 9);
        TASK_CHECK(Near(refined.DomainStart(), -3) && Near(refined.DomainEnd(), 5));
        for (double u : {-2.9, -0.1, 0.0, 1.3, 2.0, 4.9})
        {
            TASK_CHECK(Near3(refined.Evaluate(u), rail.Evaluate(u), 1e-9));
            TASK_CHECK(Near3(refined.EvaluateDerivative(u, 1), rail.EvaluateDerivative(u, 1), 1e-8));
            TASK_CHECK(Near3(refined.EvaluateDerivative(u, 2), rail.EvaluateDerivative(u, 2), 1e-8));
        }
        // Surface: insert on both axes, including a full-multiplicity (C0) knot.
        Surface cyl = MakeCylinder();
        Surface ref = cyl.InsertKnotU(0.0).InsertKnotU(1.0).InsertKnotU(1.0).InsertKnotV(11.0);
        TASK_CHECK(ref.KnotsU.GetMultiplicity(1.0) == 2);
        for (double u : {-2.9, 0.0, 1.0, 3.3})
            for (double v : {10.2, 11.0, 11.8})
            {
                TASK_CHECK(Near3(ref.Evaluate(u, v), cyl.Evaluate(u, v), 1e-9));
                TASK_CHECK(Near3(ref.EvaluateDerivative(u, v, 1, 0),
                                 cyl.EvaluateDerivative(u, v, 1, 0), 1e-7));
                TASK_CHECK(Near3(ref.EvaluateDerivative(u, v, 0, 1),
                                 cyl.EvaluateDerivative(u, v, 0, 1), 1e-9));
            }
        // Insertion commutes with positive affine reparametrization.
        Surface s1 = cyl.InsertKnotU(0.5);
        ReparamKnots(s1.KnotsU, 0.5, 1.0);
        Surface s2 = cyl;
        ReparamKnots(s2.KnotsU, 0.5, 1.0);
        s2 = s2.InsertKnotU(0.5 * 0.5 + 1.0);
        for (double up : {-0.4, 1.25, 3.4})
            TASK_CHECK(Near3(s1.Evaluate(up, 11.0), s2.Evaluate(up, 11.0), 1e-9));
        return 0;
    }

    int EditingDegreeElevation()
    {
        // Multi-span curve elevation keeps the geometry exactly.
        Curve rail = MakeRailCurveMultiSpan();
        Curve up   = rail.ElevateDegree();
        TASK_CHECK(up.Degree == 3);
        TASK_CHECK((int)up.ControlPoints.size() == up.Knots.Count() - up.Degree - 1);
        for (double u : {-2.9, 0.2, 1.0, 4.9})
        {
            TASK_CHECK(Near3(up.Evaluate(u), Vec3(u, 0.25 * u * u, 0), 1e-8));
            TASK_CHECK(Near3(up.EvaluateDerivative(u, 1), Vec3(1, 0.5 * u, 0), 1e-7));
        }
        // Multi-span surface elevation: the control net must be sized from the
        // actual elevated knot vector, not from a single-span formula.
        Surface crease = MakeCrease();
        Surface eup    = crease.ElevateDegreeU();
        TASK_CHECK(eup.DegreeU == 3);
        TASK_CHECK(eup.ControlPoints.UCount == eup.KnotsU.Count() - eup.DegreeU - 1);
        TASK_CHECK(eup.ControlPoints.UCount == 7); // {-1x4, 0.5x3, 2x4} -> 11 - 3 - 1
        TASK_CHECK(eup.ControlPoints.VCount == 2);
        Surface evp = crease.ElevateDegreeV();
        TASK_CHECK(evp.DegreeV == 2);
        TASK_CHECK(evp.ControlPoints.VCount == evp.KnotsV.Count() - evp.DegreeV - 1);
        TASK_CHECK(evp.ControlPoints.VCount == 3);
        auto f = [](double u) { return u <= 0.5 ? u + 1.0 : 2.0 * u + 0.5; };
        for (double u : {-0.9, 0.5, 1.7})
            for (double v : {3.4, 6.5})
            {
                TASK_CHECK(Near3(eup.Evaluate(u, v), Vec3(u, f(u), v), 1e-8));
                TASK_CHECK(Near3(evp.Evaluate(u, v), Vec3(u, f(u), v), 1e-8));
            }
        // Elevated rational cylinder: geometry and derivatives preserved.
        Surface cyl = MakeCylinder();
        Surface eup2 = cyl.ElevateDegreeU().ElevateDegreeV();
        TASK_CHECK(eup2.ControlPoints.UCount == eup2.KnotsU.Count() - eup2.DegreeU - 1);
        TASK_CHECK(eup2.ControlPoints.VCount == eup2.KnotsV.Count() - eup2.DegreeV - 1);
        for (double u : {-2.9, 0.7, 4.9})
            for (double v : {10.4, 11.6})
            {
                TASK_CHECK(Near3(eup2.Evaluate(u, v), cyl.Evaluate(u, v), 1e-8));
                TASK_CHECK(Near3(eup2.EvaluateDerivative(u, v, 1, 0),
                                 cyl.EvaluateDerivative(u, v, 1, 0), 1e-6));
            }
        return 0;
    }

    int EditingReverseAlign()
    {
        // Reversing a knot vector maps u -> a + b - u over the original domain.
        Curve curve;
        curve.Degree = 2;
        curve.Knots  = KnotVector{{-3, -3, -3, 0, 3, 5, 5, 5}};
        curve.ControlPoints = {Vec4(-1, 0, 0, 1), Vec4(-0.5, 1, 0.2, 1), Vec4(0, 0, 0.4, 1),
                               Vec4(0.5, 1, 0.6, 1), Vec4(1, 0, 0.8, 1)};
        Curve rev = curve;
        rev.Knots.Reverse();
        std::reverse(rev.ControlPoints.begin(), rev.ControlPoints.end());
        const std::vector<double> expected{-3, -3, -3, -1, 2, 5, 5, 5};
        TASK_CHECK(rev.Knots.Count() == (int)expected.size());
        for (int i = 0; i < rev.Knots.Count(); ++i)
            TASK_CHECK(Near(rev.Knots(i), expected[i], 1e-12));
        for (double u : {-2.9, -0.4, 1.3, 4.9})
            TASK_CHECK(Near3(rev.Evaluate(u), curve.Evaluate(2.0 - u), 1e-9));

        // AlignParameterDomain flip path: u runs along -X and v along -Y, so both
        // axes must be reversed with the a + b - u correspondence.
        Surface s;
        s.DegreeU = 2;
        s.DegreeV = 1;
        s.KnotsU  = KnotVector{{1, 1, 1, 2, 5, 5, 5}};
        s.KnotsV  = KnotVector{{2, 2, 4, 4}};
        const double greville_u[4] = {1, 1.5, 3.5, 5};
        const double greville_v[2] = {2, 4};
        s.ControlPoints = ControlPointGrid(4, 2);
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 4; ++i)
                s.ControlPoints.Get(i, j) = Vec4(6 - greville_u[i], 6 - greville_v[j], 0, 1);
        Surface aligned = s.AlignParameterDomain(Surface::AlignAxis::XAxis, Surface::AlignAxis::YAxis);
        const std::vector<double> expected_u{1, 1, 1, 4, 5, 5, 5};
        TASK_CHECK(aligned.KnotsU.Count() == (int)expected_u.size());
        for (int i = 0; i < aligned.KnotsU.Count(); ++i)
            TASK_CHECK(Near(aligned.KnotsU(i), expected_u[i], 1e-12));
        for (double u : {1.2, 2.0, 4.8})
            for (double v : {2.3, 3.8})
                TASK_CHECK(Near3(aligned.Evaluate(6 - u, 6 - v), s.Evaluate(u, v), 1e-9));
        return 0;
    }

    int EditingFileRoundtrip()
    {
        Surface cyl = MakeCylinder().ElevateDegreeU().InsertKnotU(0.0).InsertKnotV(11.0);
        Curve rail  = MakeRailCurveMultiSpan().ElevateDegree().InsertKnot(2.0);
        for (bool binary : {false, true})
        {
            std::stringstream io(std::ios::in | std::ios::out | std::ios::binary);
            cyl.SaveToFile(io, binary);
            io.seekg(0);
            Surface back;
            back.LoadFromFile(io);
            TASK_CHECK(KnotsEqual(back.KnotsU, cyl.KnotsU));
            TASK_CHECK(KnotsEqual(back.KnotsV, cyl.KnotsV));
            TASK_CHECK(back.ControlPoints.Values.size() == cyl.ControlPoints.Values.size());
            for (size_t i = 0; i < cyl.ControlPoints.Values.size(); ++i)
                TASK_CHECK(back.ControlPoints.Values[i].w() == cyl.ControlPoints.Values[i].w());
            for (double u : {-2.9, 0.0, 4.9})
                for (double v : {10.4, 11.0, 11.9})
                {
                    TASK_CHECK(Near3(back.Evaluate(u, v), cyl.Evaluate(u, v), 1e-10));
                    TASK_CHECK(Near3(back.EvaluateDerivative(u, v, 1, 0),
                                     cyl.EvaluateDerivative(u, v, 1, 0), 1e-8));
                    TASK_CHECK(Near3(back.EvaluateDerivative(u, v, 0, 1),
                                     cyl.EvaluateDerivative(u, v, 0, 1), 1e-10));
                }

            std::stringstream io2(std::ios::in | std::ios::out | std::ios::binary);
            rail.SaveToFile(io2, binary);
            io2.seekg(0);
            Curve cback;
            cback.LoadFromFile(io2);
            TASK_CHECK(KnotsEqual(cback.Knots, rail.Knots));
            TASK_CHECK(cback.ControlPoints.size() == rail.ControlPoints.size());
            for (size_t i = 0; i < rail.ControlPoints.size(); ++i)
                TASK_CHECK(cback.ControlPoints[i].w() == rail.ControlPoints[i].w());
            for (double u : {-2.9, 0.2, 2.0, 4.9})
            {
                TASK_CHECK(Near3(cback.Evaluate(u), rail.Evaluate(u), 1e-10));
                TASK_CHECK(Near3(cback.EvaluateDerivative(u, 1), rail.EvaluateDerivative(u, 1), 1e-8));
            }
        }
        return 0;
    }

    // -------------------------------------------------------------------- mesh

    // Locate the triangle and corner referencing a given UV within a cell corner set.
    double UvCrossZ(const SurfaceMesh::UV& a, const SurfaceMesh::UV& b, const SurfaceMesh::UV& c)
    {
        return (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
    }

    int MeshPlaneGrid()
    {
        Surface plane = MakePlane();
        std::vector<double> us{-3, 0, 5};
        std::vector<double> vs{10, 12};
        SurfaceMesh mesh = BuildSurfaceMesh(plane, us, vs);
        // Internal knot 1 is merged into the u stations.
        const std::vector<double> grid_u{-3, 0, 1, 5};
        const std::vector<double> grid_v{10, 12};
        const int nu = 4, nv = 2;
        TASK_CHECK(mesh.positions.size() == (size_t)(nu * nv));
        TASK_CHECK(mesh.triangles.size() == 2 * (nu - 1) * (nv - 1));
        TASK_CHECK(mesh.corner_uv.size() == mesh.triangles.size());
        TASK_CHECK(mesh.corner_normals.size() == mesh.triangles.size());
        const Vec3 normal = PLANE_A.cross(PLANE_B).normalized();
        for (size_t t = 0; t < mesh.triangles.size(); ++t)
        {
            const auto& tri = mesh.triangles[t];
            const auto& uv  = mesh.corner_uv[t];
            TASK_CHECK(UvCrossZ(uv[0], uv[1], uv[2]) > 0); // positive UV winding
            for (int k = 0; k < 3; ++k)
            {
                const int idx = tri[k];
                const int i   = idx % nu;
                const int j   = idx / nu;
                // corner_uv stores the original-domain parameter of the station.
                TASK_CHECK(Near(uv[k].x(), grid_u[i], 1e-12));
                TASK_CHECK(Near(uv[k].y(), grid_v[j], 1e-12));
                // Positions sit exactly on the analytic plane.
                TASK_CHECK(Near3(mesh.positions[idx],
                                 PLANE_P0 + PLANE_A * grid_u[i] + PLANE_B * grid_v[j], 1e-9));
                TASK_CHECK(Near3(mesh.corner_normals[t][k], normal, 1e-9));
            }
        }
        return 0;
    }

    int MeshCreaseSidedness()
    {
        Surface crease = MakeCrease();
        std::vector<double> us{-1, 2};
        std::vector<double> vs{3, 7};
        SurfaceMesh mesh = BuildSurfaceMesh(crease, us, vs);
        // Stations after merging the internal C0 knot: u in {-1, 0.5, 2}, v in {3, 7}.
        const int nu = 3;
        TASK_CHECK(mesh.positions.size() == 6);
        TASK_CHECK(mesh.triangles.size() == 4);
        const Vec3 normal_left  = Vec3(1, -1, 0).normalized();
        const Vec3 normal_right = Vec3(2, -1, 0).normalized();
        TASK_CHECK((normal_left - normal_right).norm() > 0.1); // genuinely different sides
        auto f = [](double u) { return u <= 0.5 ? u + 1.0 : 2.0 * u + 0.5; };
        int crease_position_refs = 0;
        for (size_t t = 0; t < mesh.triangles.size(); ++t)
        {
            const auto& tri = mesh.triangles[t];
            const auto& uv  = mesh.corner_uv[t];
            TASK_CHECK(UvCrossZ(uv[0], uv[1], uv[2]) > 0);
            // The cell this triangle was cut from (its lower-left corner).
            double cell_u = std::min({uv[0].x(), uv[1].x(), uv[2].x()});
            for (int k = 0; k < 3; ++k)
            {
                const int idx = tri[k];
                const int i   = idx % nu;
                const int j   = idx / nu;
                const double su = (i == 0) ? -1 : (i == 1 ? 0.5 : 2.0);
                const double sv = (j == 0) ? 3.0 : 7.0;
                TASK_CHECK(Near(uv[k].x(), su, 1e-12) && Near(uv[k].y(), sv, 1e-12));
                TASK_CHECK(Near3(mesh.positions[idx], Vec3(su, f(su), sv), 1e-9));
                if (i == 1)
                {
                    // On the C0 crease: the position is shared by parameter identity
                    // (station index 1 on both sides), but each cell keeps the
                    // one-sided analytic normal of its own interior.
                    TASK_CHECK(idx == 1 || idx == 4);
                    ++crease_position_refs;
                    if (cell_u < 0.5)
                        TASK_CHECK(Near3(mesh.corner_normals[t][k], normal_left, 1e-9));
                    else
                        TASK_CHECK(Near3(mesh.corner_normals[t][k], normal_right, 1e-9));
                }
                else
                {
                    const Vec3& expect = (i == 0) ? normal_left : normal_right;
                    TASK_CHECK(Near3(mesh.corner_normals[t][k], expect, 1e-9));
                }
            }
        }
        // The four triangles touch the crease stations in 6 corners total.
        TASK_CHECK(crease_position_refs == 6);
        return 0;
    }

    int MeshCylinderNormals()
    {
        Surface cyl = MakeCylinder();
        std::vector<double> us{-3, -1, 5};
        std::vector<double> vs{10, 11, 12};
        SurfaceMesh mesh = BuildSurfaceMesh(cyl, us, vs);
        TASK_CHECK(mesh.positions.size() == 9);
        TASK_CHECK(mesh.triangles.size() == 8);
        for (size_t t = 0; t < mesh.triangles.size(); ++t)
        {
            const auto& uv = mesh.corner_uv[t];
            TASK_CHECK(UvCrossZ(uv[0], uv[1], uv[2]) > 0);
            for (int k = 0; k < 3; ++k)
            {
                const Vec3& p = mesh.positions[mesh.triangles[t][k]];
                TASK_CHECK(Near(std::hypot(p.x(), p.y()), CYL_R, 1e-9));
                TASK_CHECK(Near(p.z(), uv[k].y(), 1e-9));
                const double theta = std::atan2(p.y(), p.x());
                TASK_CHECK(Near3(mesh.corner_normals[t][k],
                                 Vec3(std::cos(theta), std::sin(theta), 0), 1e-8));
                TASK_CHECK(Near(mesh.corner_normals[t][k].norm(), 1.0, 1e-12));
            }
        }
        return 0;
    }

    int MeshInvalidInput()
    {
        Surface plane = MakePlane();
        const Vec3 before = plane.Evaluate(0.4, 11.2);
        std::vector<double> good_us{-3, 0, 5};
        std::vector<double> good_vs{10, 12};

        std::vector<std::vector<double>> bad_us_list{
            {}, {-3}, {-2, 5}, {-3, 4}, {-3, 1, 0, 5}, {-3, 6}, {-3.5, 5}};
        for (const auto& bad : bad_us_list)
        {
            std::vector<double> us  = bad;
            std::vector<double> vs  = good_vs;
            std::vector<double> us_copy = us, vs_copy = vs;
            TASK_CHECK(ThrowsInvalid([&] { BuildSurfaceMesh(plane, us, vs); }));
            TASK_CHECK(us == us_copy && vs == vs_copy); // inputs unchanged
        }
        std::vector<std::vector<double>> bad_vs_list{
            {}, {12}, {10, 11.5}, {11, 12}, {10, 10.5, 10.4, 12}};
        for (const auto& bad : bad_vs_list)
        {
            std::vector<double> us = good_us;
            std::vector<double> vs = bad;
            TASK_CHECK(ThrowsInvalid([&] { BuildSurfaceMesh(plane, us, vs); }));
        }
        TASK_CHECK(Near3(plane.Evaluate(0.4, 11.2), before, 1e-12)); // surface unchanged

        // Degenerate one-sided normal: coincident control points collapse Su at v = 0.
        Surface flat;
        flat.DegreeU = 1;
        flat.DegreeV = 1;
        flat.KnotsU  = KnotVector{{0, 0, 1, 1}};
        flat.KnotsV  = KnotVector{{0, 0, 1, 1}};
        flat.ControlPoints = ControlPointGrid(2, 2);
        flat.ControlPoints.Get(0, 0) = Vec4(0, 0, 0, 1);
        flat.ControlPoints.Get(1, 0) = Vec4(0, 0, 0, 1);
        flat.ControlPoints.Get(0, 1) = Vec4(0, 1, 0, 1);
        flat.ControlPoints.Get(1, 1) = Vec4(1, 1, 0, 1);
        std::vector<double> us{0, 1}, vs{0, 1};
        TASK_CHECK(ThrowsRuntime([&] { BuildSurfaceMesh(flat, us, vs); }));
        TASK_CHECK(us == std::vector<double>({0, 1}) && vs == std::vector<double>({0, 1}));
        return 0;
    }
} // namespace

const task_case task_cases[] = {
    {"domain", "curve_rail", DomainCurveRail},
    {"domain", "plane", DomainPlane},
    {"domain", "mixed_partials", DomainMixedPartials},
    {"domain", "cylinder", DomainCylinder},
    {"domain", "validation", DomainValidation},
    {"editing", "knot_insertion", EditingKnotInsertion},
    {"editing", "degree_elevation", EditingDegreeElevation},
    {"editing", "reverse_align", EditingReverseAlign},
    {"editing", "file_roundtrip", EditingFileRoundtrip},
    {"mesh", "plane_grid", MeshPlaneGrid},
    {"mesh", "crease_sidedness", MeshCreaseSidedness},
    {"mesh", "cylinder_normals", MeshCylinderNormals},
    {"mesh", "invalid_input", MeshInvalidInput},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
