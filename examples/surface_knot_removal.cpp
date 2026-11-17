// Surface knot-removal example.
//
// Build (uses only the headers bundled under task_support/vendor/eigen):
//
//   c++ -std=c++23 -O2 -Iinclude -Itask_support/vendor/eigen \
//       examples/surface_knot_removal.cpp src/**/*.cpp -o surface_knot_removal
//
// (or link against a built libnurbs). The program prints how many redundant
// knot copies were removed, the resulting control-net size, and the observed
// geometric error sampled on the parameter domain.
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <vector>

#include <libnurbs/Algorithm/KnotRemoval.hpp>
#include <libnurbs/Curve/Curve.hpp>
#include <libnurbs/Surface/Surface.hpp>

using namespace libnurbs;

namespace
{
    Curve QuadraticBezier()
    {
        Curve curve;
        curve.Degree = 2;
        curve.Knots = KnotVector{{0.0, 0.0, 0.0, 1.0, 1.0, 1.0}};
        curve.ControlPoints = {
            {0.0, 0.0, 0.0, 1.0},
            {1.0, 2.0, 1.0, 1.5},
            {3.0, 0.0, 0.0, 0.7},
        };
        return curve;
    }

    Curve LinearRow()
    {
        Curve row;
        row.Degree = 1;
        row.Knots = KnotVector{{0.0, 0.0, 1.0, 1.0}};
        row.ControlPoints = {{0.0, 0.0, 2.0, 1.0}, {3.0, 1.0, 2.0, 1.0}};
        return row;
    }

    Surface MakeSurface(const Curve& u_curve, const Curve& v_curve)
    {
        Surface surface;
        surface.DegreeU = u_curve.Degree;
        surface.DegreeV = v_curve.Degree;
        surface.KnotsU = u_curve.Knots;
        surface.KnotsV = v_curve.Knots;
        surface.ControlPoints = ControlPointGrid{static_cast<int>(u_curve.ControlPoints.size()),
                                                 static_cast<int>(v_curve.ControlPoints.size())};
        for (int i = 0; i < surface.ControlPoints.UCount; ++i)
        {
            for (int j = 0; j < surface.ControlPoints.VCount; ++j)
            {
                const Vec4& a = u_curve.ControlPoints[i];
                const Vec4& b = v_curve.ControlPoints[j];
                surface.ControlPoints.Get(i, j) = Vec4{a.x() + b.x(), a.y() + b.y(),
                                                       a.z() + b.z(), a.w() * b.w()};
            }
        }
        return surface;
    }

    double SurfaceDistance(const Surface& a, const Surface& b, int samples = 64)
    {
        double worst = 0.0;
        for (int i = 0; i <= samples; ++i)
        {
            for (int j = 0; j <= samples; ++j)
            {
                const double u = static_cast<double>(i) / samples;
                const double v = static_cast<double>(j) / samples;
                worst = std::max(worst, (a.Evaluate(u, v) - b.Evaluate(u, v)).norm());
            }
        }
        return worst;
    }
}

int main()
{
    std::cout << std::setprecision(6) << std::scientific;

    // A surface whose U direction carries a quadratic Bezier and a linear row.
    Surface surface = MakeSurface(QuadraticBezier(), LinearRow());

    // Insert two redundant copies of u = 0.5, then ask for both back.
    const Surface refined = surface.InsertKnotU(0.5, 2);
    std::cout << "control net before: " << surface.ControlPoints.UCount << " x "
              << surface.ControlPoints.VCount << " U-knots=" << surface.KnotsU.Values().size() << "\n";
    std::cout << "control net refined: " << refined.ControlPoints.UCount << " x "
              << refined.ControlPoints.VCount << " U-knots=" << refined.KnotsU.Values().size() << "\n";

    const auto [removed_surface, removed] = refined.RemoveKnotU(0.5, 2, 1e-9);
    std::cout << "requested 2 removals, performed " << removed << "\n";
    std::cout << "control net after: " << removed_surface.ControlPoints.UCount << " x "
              << removed_surface.ControlPoints.VCount << " U-knots="
              << removed_surface.KnotsU.Values().size() << "\n";
    std::cout << "sampled geometric error vs refined input: "
              << SurfaceDistance(removed_surface, refined) << "\n";

    // A perturbation that is not representable keeps the basis intact under a
    // tight tolerance, and is removed under a loose one.
    Surface perturbed = refined;
    for (int j = 0; j < perturbed.ControlPoints.VCount; ++j)
    {
        for (int i = 0; i < perturbed.ControlPoints.UCount; ++i)
        {
            perturbed.ControlPoints.Get(i, j).z() += 1e-4 * std::sin(1.7 * i + 2.3 * j);
        }
    }

    for (double tolerance: {1e-9, 1e-3})
    {
        const auto [result, count] = perturbed.RemoveKnotU(0.5, 2, tolerance);
        std::cout << "tolerance " << tolerance << ": removed " << count
                  << ", U-knots=" << result.KnotsU.Values().size()
                  << ", sampled error " << (count ? SurfaceDistance(result, perturbed, 48) : 0.0)
                  << "\n";
    }

    // Zero removals return an independent copy identical to the source.
    const auto [unchanged, none] = surface.RemoveKnotU(0.25, 1, 0.0);
    std::cout << "missing knot: removed " << none << ", U-knots unchanged="
              << (unchanged.KnotsU.Values() == surface.KnotsU.Values()) << "\n";
    return 0;
}
