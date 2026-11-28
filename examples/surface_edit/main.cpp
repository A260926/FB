// Surface knot-removal example.
//
// Demonstrates the editing workflow used by the library: refine a surface with
// knot insertion, then remove the inserted knots again.  Removal is "exact"
// when the surface really is a refinement (within a tight tolerance) and
// approximate otherwise; the returned count is the number of copies that were
// actually removed, which may be smaller than requested.
//
// Build (only the headers bundled with this repository are required):
//
//   c++ -std=c++23 -O0 -ffp-contract=off \
//       -Iinclude -Itask_support/vendor/eigen \
//       examples/surface_edit/main.cpp src/... (or link libnurbs) \
//       -o surface_edit
//
// The CMake target `SurfaceEditExample` in examples/surface_edit/CMakeLists.txt
// does the same against an installed/bundled Eigen.

#include <libnurbs/Curve/Curve.hpp>
#include <libnurbs/Surface/Surface.hpp>

#include <cmath>
#include <iomanip>
#include <iostream>

using namespace libnurbs;

namespace
{
    // A degree (2,1) tensor-product surface.  The U rows deliberately have
    // different removal abilities: row 0 can give back two inserted copies of
    // u = 0.5, row 1 only one.  Both rows live on the same U knot vector.
    Surface MakeMixedSurface(int& u_count, int& v_count)
    {
        Curve row0;
        row0.Degree = 2;
        row0.Knots  = KnotVector{{0, 0, 0, 1, 1, 1}};
        row0.ControlPoints = {{0, 0, 0, 1}, {1, 1, 0, 1}, {3, 0, 0, 1}};
        row0 = row0.InsertKnot(0.5, 2);

        Curve row1;
        row1.Degree = 2;
        row1.Knots  = KnotVector{{0, 0, 0, 0.5, 1, 1, 1}};
        row1.ControlPoints = {{0, 0, 0, 1}, {1, 1, 0, 2}, {2, -0.5, 1, 0.7}, {3, 0.2, 0, 1.2}};
        row1 = row1.InsertKnot(0.5, 1);

        Surface surface;
        surface.DegreeU = 2;
        surface.DegreeV = 1;
        surface.KnotsU  = row0.Knots;
        surface.KnotsV  = KnotVector{{0, 0, 1, 1}};
        surface.ControlPoints = ControlPointGrid(static_cast<int>(row0.ControlPoints.size()), 2);
        for (int i = 0; i < static_cast<int>(row0.ControlPoints.size()); ++i)
        {
            surface.ControlPoints.Get(i, 0) = row0.ControlPoints[i];
            Vec4 lifted                     = row1.ControlPoints[i];
            lifted.z() += 2.0;
            surface.ControlPoints.Get(i, 1) = lifted;
        }
        u_count = surface.ControlPoints.UCount;
        v_count = surface.ControlPoints.VCount;
        return surface;
    }

    double SampledError(const Surface& a, const Surface& b, int samples = 40)
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

    void Report(const char* title, const Surface& before, const std::tuple<Surface, int>& result)
    {
        const Surface& after = std::get<0>(result);
        const int removed    = std::get<1>(result);
        std::cout << title << "\n"
                  << "  removed=" << removed
                  << "  control grid=" << after.ControlPoints.UCount << "x" << after.ControlPoints.VCount
                  << "  knotsU=" << after.KnotsU.Values().size()
                  << "  mult(0.5)=" << after.KnotsU.GetMultiplicity(0.5)
                  << "  sampled error=" << SampledError(before, after) << "\n";
    }
}

int main()
{
    std::cout << std::scientific << std::setprecision(3);

    int u_count = 0;
    int v_count = 0;
    Surface surface = MakeMixedSurface(u_count, v_count);

    std::cout << "source: " << u_count << "x" << v_count
              << " control points, U mult(0.5)=" << surface.KnotsU.GetMultiplicity(0.5) << "\n\n";

    // 1. Refine and invert exactly.  Inserting one copy at u = 0.4 and removing
    //    it again must return the original surface.
    {
        const Surface refined = surface.InsertKnotU(0.4, 1);
        Report("exact: insert u=0.4 once, then remove", refined, refined.RemoveKnotU(0.4, 1, 1e-12));
    }

    // 2. The two control rows differ: row 0 supports two removals of u = 0.5,
    //    row 1 supports one.  A shared knot basis means the answer is one.
    Report("shared basis: request 2 removals of u=0.5", surface, surface.RemoveKnotU(0.5, 2, 1e-10));

    // 3. Requesting more copies than exist is clamped.
    Report("clamped: request 5 removals of u=0.5", surface, surface.RemoveKnotU(0.5, 5, 1e-10));

    // 4. Approximate removal: perturb a control point, then remove under a loose
    //    and a tight tolerance.  The tight request must refuse and return an
    //    identical copy of the input.
    {
        Surface perturbed = surface.InsertKnotU(0.25, 1);
        perturbed.ControlPoints.Get(2, 0).head<3>() += Vec3(2e-4, -1e-4, 3e-4);
        Report("approximate, loose tolerance 1e-2", perturbed, perturbed.RemoveKnotU(0.25, 1, 1e-2));
        Report("approximate, tight tolerance 1e-9", perturbed, perturbed.RemoveKnotU(0.25, 1, 1e-9));
    }

    // 5. Uniform positive weight scaling does not change the decision or the
    //    geometry.
    {
        Surface scaled = surface;
        for (auto& point: scaled.ControlPoints.Values)
        {
            point.w() *= 7.5;
        }
        Report("weights x7.5", scaled, scaled.RemoveKnotU(0.5, 2, 1e-10));
    }

    // 6. U and V behave symmetrically; removal along V of a refined V.
    {
        const Surface refined = surface.InsertKnotV(0.5, 2);
        const auto [reduced, removed] = refined.RemoveKnotV(0.5, 2, 1e-12);
        std::cout << "V symmetry: insert 2, remove 2 -> removed=" << removed
                  << "  V count=" << reduced.ControlPoints.VCount
                  << "  sampled error=" << SampledError(refined, reduced) << "\n";
    }

    return 0;
}
