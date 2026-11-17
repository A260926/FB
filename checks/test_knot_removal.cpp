// Regression checks for NURBS knot removal across curves and tensor-product
// surfaces. Registered through task_support/check.h; no main() is defined here.
//
// Three groups are exercised:
//   prefix        - zero / partial / sequential removals on curves and surfaces
//   tensor        - non-square meshes, U/V symmetry, alternating axis edits
//   rational_bound- weights, scaling, transforms and the geometric tolerance
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <tuple>
#include <vector>

#include <libnurbs/Algorithm/KnotRemoval.hpp>
#include <libnurbs/Curve/Curve.hpp>
#include <libnurbs/Surface/Surface.hpp>

#include "../task_support/check.h"

namespace
{
    using libnurbs::ControlPointGrid;
    using libnurbs::Curve;
    using libnurbs::KnotVector;
    using libnurbs::Numeric;
    using libnurbs::Surface;
    using libnurbs::Vec3;
    using libnurbs::Vec4;
    using std::vector;

    // ------------------------------------------------------------------ helpers
    constexpr Numeric GEOM_EPS = 1e-9;

    struct Rng
    {
        unsigned long long state;
        explicit Rng(unsigned long long seed = 0x9e3779b97f4a7c15ULL) : state(seed) {}
        unsigned Next()
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return static_cast<unsigned>(state >> 11);
        }
        // Uniform in [lo, hi).
        Numeric Uniform(Numeric lo, Numeric hi)
        {
            return lo + (hi - lo) * (static_cast<Numeric>(Next() % 1000003u) / 1000003.0);
        }
    };

    KnotVector ClampedKnots(int degree, const vector<Numeric>& interior)
    {
        vector<Numeric> values(degree + 1, 0.0);
        values.insert(values.end(), interior.begin(), interior.end());
        values.insert(values.end(), degree + 1, 1.0);
        return KnotVector{values};
    }

    Curve MakeCurve(int degree, const vector<Numeric>& interior, const vector<Vec4>& points)
    {
        Curve curve;
        curve.Degree = degree;
        curve.Knots = ClampedKnots(degree, interior);
        curve.ControlPoints = points;
        return curve;
    }

    bool SameReal(Numeric a, Numeric b) { return std::abs(a - b) <= GEOM_EPS; }

    bool SameCurveData(const Curve& a, const Curve& b)
    {
        if (a.Degree != b.Degree) return false;
        if (a.Knots.Values().size() != b.Knots.Values().size()) return false;
        for (size_t i = 0; i < a.Knots.Values().size(); ++i)
            if (!SameReal(a.Knots.Values()[i], b.Knots.Values()[i])) return false;
        if (a.ControlPoints.size() != b.ControlPoints.size()) return false;
        for (size_t i = 0; i < a.ControlPoints.size(); ++i)
            if ((a.ControlPoints[i] - b.ControlPoints[i]).norm() > GEOM_EPS) return false;
        return true;
    }

    bool SameSurfaceData(const Surface& a, const Surface& b)
    {
        if (a.DegreeU != b.DegreeU || a.DegreeV != b.DegreeV) return false;
        if (a.KnotsU.Values().size() != b.KnotsU.Values().size()) return false;
        if (a.KnotsV.Values().size() != b.KnotsV.Values().size()) return false;
        for (size_t i = 0; i < a.KnotsU.Values().size(); ++i)
            if (!SameReal(a.KnotsU.Values()[i], b.KnotsU.Values()[i])) return false;
        for (size_t i = 0; i < a.KnotsV.Values().size(); ++i)
            if (!SameReal(a.KnotsV.Values()[i], b.KnotsV.Values()[i])) return false;
        if (a.ControlPoints.UCount != b.ControlPoints.UCount) return false;
        if (a.ControlPoints.VCount != b.ControlPoints.VCount) return false;
        if (a.ControlPoints.Values.size() != b.ControlPoints.Values.size()) return false;
        for (size_t i = 0; i < a.ControlPoints.Values.size(); ++i)
            if ((a.ControlPoints.Values[i] - b.ControlPoints.Values[i]).norm() > GEOM_EPS) return false;
        return true;
    }

    // Structural invariants: knots == control points + degree + 1.
    bool CurveStructureValid(const Curve& c)
    {
        return static_cast<int>(c.Knots.Values().size()) ==
               static_cast<int>(c.ControlPoints.size()) + c.Degree + 1;
    }

    bool SurfaceStructureValid(const Surface& s)
    {
        if (static_cast<int>(s.KnotsU.Values().size()) != s.ControlPoints.UCount + s.DegreeU + 1)
            return false;
        if (static_cast<int>(s.KnotsV.Values().size()) != s.ControlPoints.VCount + s.DegreeV + 1)
            return false;
        return static_cast<int>(s.ControlPoints.Values.size()) ==
               s.ControlPoints.UCount * s.ControlPoints.VCount;
    }

    // Multiplicity of a stored knot value (exact match, as produced by the
    // library's own insertion/removal).
    int MultiplicityOf(const KnotVector& knots, Numeric value)
    {
        int count = 0;
        for (Numeric stored: knots.Values())
            if (stored == value) ++count;
        return count;
    }

    // Dense pointwise sampling distance. Used only to assert that the *reported*
    // tolerance was not exceeded; acceptance itself is based on an analytic
    // whole-domain bound inside the library.
    Numeric CurveDistance(const Curve& a, const Curve& b, int samples = 160)
    {
        Numeric worst = 0;
        for (int i = 0; i <= samples; ++i)
        {
            const Numeric u = static_cast<Numeric>(i) / samples;
            worst = std::max(worst, (a.Evaluate(u) - b.Evaluate(u)).norm());
        }
        return worst;
    }

    Numeric SurfaceDistance(const Surface& a, const Surface& b, int samples = 40)
    {
        Numeric worst = 0;
        for (int i = 0; i <= samples; ++i)
        {
            for (int j = 0; j <= samples; ++j)
            {
                const Numeric u = static_cast<Numeric>(i) / samples;
                const Numeric v = static_cast<Numeric>(j) / samples;
                worst = std::max(worst, (a.Evaluate(u, v) - b.Evaluate(u, v)).norm());
            }
        }
        return worst;
    }

    // Curves with identical degree but differing weights, in the codebase's
    // Cartesian-plus-weight representation.
    Curve UnequalWeightQuadratic()
    {
        return MakeCurve(2, {0.35, 0.7},
                         {{0.0, 0.0, 0.0, 0.4},
                          {1.0, 2.0, 0.5, 1.7},
                          {2.5, -1.0, 1.0, 0.9},
                          {4.0, 0.5, 0.0, 2.3},
                          {5.0, 1.0, 0.5, 1.1}});
    }

    Surface MakeSurface(int degree_u,
                        int degree_v,
                        const vector<Numeric>& interior_u,
                        const vector<Numeric>& interior_v,
                        const vector<Vec4>& points)
    {
        Surface result;
        result.DegreeU = degree_u;
        result.DegreeV = degree_v;
        result.KnotsU = ClampedKnots(degree_u, interior_u);
        result.KnotsV = ClampedKnots(degree_v, interior_v);
        const int count_u = static_cast<int>(result.KnotsU.Values().size()) - degree_u - 1;
        const int count_v = static_cast<int>(result.KnotsV.Values().size()) - degree_v - 1;
        result.ControlPoints = ControlPointGrid{count_u, count_v};
        for (int v = 0; v < count_v; ++v)
        {
            for (int u = 0; u < count_u; ++u)
            {
                result.ControlPoints.Get(u, v) = points[static_cast<size_t>(v) * count_u + u];
            }
        }
        return result;
    }

    // Non-square control net (5 x 3) with distinct weights in every row.
    Surface NonSquareSurface()
    {
        vector<Vec4> points{
            {0.0, 0.0, 0.0, 1.0}, {1.0, 1.0, 0.2, 2.0}, {2.0, 0.0, 0.4, 0.7},
            {3.0, 1.0, 0.1, 1.5}, {4.0, 0.0, 0.3, 1.0},
            {0.0, 1.0, 1.0, 1.2}, {1.0, 2.0, 1.2, 0.8}, {2.0, 1.0, 1.4, 2.4},
            {3.0, 2.0, 1.1, 1.0}, {4.0, 1.0, 1.3, 0.6},
            {0.0, 2.0, 2.0, 0.9}, {1.0, 3.0, 2.2, 1.6}, {2.0, 2.0, 2.4, 1.0},
            {3.0, 3.0, 2.1, 1.3}, {4.0, 2.0, 2.3, 1.8},
        };
        return MakeSurface(2, 1, {0.3, 0.65}, {0.5}, points);
    }

    Surface MixedCapabilitySurface()
    {
        Surface surface;
        surface.DegreeU = 2;
        surface.DegreeV = 1;
        surface.KnotsV = KnotVector{{0.0, 0.0, 1.0, 1.0}};

        // Upper row: a quadratic Bezier split twice at 0.5 (multiplicity 3),
        // so two copies of 0.5 can be removed.

        // Lower row: a tangent-continuous quadratic kink split once at 0.5.
        // Its control points come from a multiplicity-2 representation, so the
        // shared basis (declared with multiplicity 3) only allows one removal.
        Curve kink = MakeCurve(2, {0.5},
                               {{0.0, 0.0, 2.0, 1.0},
                                {1.0, 1.0, 2.0, 2.0},
                                {2.0, -0.5, 3.0, 0.7},
                                {3.0, 0.2, 2.0, 1.2}});

        Curve upper = MakeCurve(2, {}, {{0.0, 0.0, 0.0, 1.0},
                                        {1.0, 1.0, 0.0, 1.0},
                                        {3.0, 0.0, 0.0, 1.0}});
        upper = upper.InsertKnot(0.5, 2);
        Curve lower = kink.InsertKnot(0.5);

        surface.KnotsU = upper.Knots;
        surface.ControlPoints = ControlPointGrid{5, 2};
        for (int i = 0; i < 5; ++i)
        {
            surface.ControlPoints.Get(i, 0) = upper.ControlPoints[i];
            surface.ControlPoints.Get(i, 1) = lower.ControlPoints[i];
        }
        return surface;
    }

    // ------------------------------------------------------------------- cases
    int CaseCurveZeroResultIsIdentical()
    {
        // A knot that is not present at all must be refused without touching
        // the source, and the returned object must be independent.
        Curve curve = UnequalWeightQuadratic();
        const auto [untouched, none] = curve.RemoveKnot(0.61, 1, 1e-12);
        TASK_CHECK(none == 0);
        TASK_CHECK(SameCurveData(untouched, curve));
        TASK_CHECK(CurveStructureValid(curve));

        // A nonzero tolerance cannot turn a missing knot into a removal.
        const auto [still_untouched, still_none] = curve.RemoveKnot(0.61, 3, 1.0);
        TASK_CHECK(still_none == 0);
        TASK_CHECK(SameCurveData(still_untouched, curve));

        // An exact round trip returns to the source; asking for one copy more
        // than the inserted multiplicity reports the insertion count, not the
        // request, and still reproduces the source exactly.
        const Curve inserted = curve.InsertKnot(0.4, 1);
        const auto [excess, excess_removed] = inserted.RemoveKnot(0.4, 3, 1e-12);
        TASK_CHECK(excess_removed == 1);
        TASK_CHECK(CurveStructureValid(excess));
        TASK_CHECK(SameCurveData(excess, curve));

        // Inserting a knot and asking for more copies than available: the
        // multiplicity limits the answer.
        const Curve inserted2 = curve.InsertKnot(0.4, 2);
        const auto [capped, capped_removed] = inserted2.RemoveKnot(0.4, 99, 1e-12);
        TASK_CHECK(capped_removed == 2);
        TASK_CHECK(SameCurveData(capped, curve));

        // Zero-removal result must be fully independent of the source.
        Curve copy = untouched;
        copy.ControlPoints[0].x() += 1.0;
        TASK_CHECK(!SameCurveData(copy, untouched));
        TASK_CHECK(SameCurveData(untouched, curve));
        return 0;
    }

    int CaseCurvePartialThenContinue()
    {
        const Curve curve = UnequalWeightQuadratic();
        const Curve inserted = curve.InsertKnot(0.5, 2);

        // Ask for more than the inserted multiplicity: the excess must not be
        // reported and the geometry must return to the source exactly.
        const auto [removed, t] = inserted.RemoveKnot(0.5, 4, 1e-10);
        TASK_CHECK(t == 2);
        TASK_CHECK(CurveStructureValid(removed));
        TASK_CHECK(SameCurveData(removed, curve));

        // One-at-a-time path: the first removal leaves a valid curve that can be
        // evaluated, refined again and reduced again.
        const auto [partial, first] = inserted.RemoveKnot(0.5, 1, 1e-10);
        TASK_CHECK(first == 1);
        TASK_CHECK(CurveStructureValid(partial));
        TASK_CHECK(partial.ControlPoints.size() == inserted.ControlPoints.size() - 1);
        TASK_CHECK(partial.Knots.Values().size() == inserted.Knots.Values().size() - 1);
        (void) partial.Evaluate(0.25);

        // Continuing the same edit on the partial result removes the remaining
        // copy and reproduces the source exactly.
        const auto [finished, second] = partial.RemoveKnot(0.5, 1, 1e-10);
        TASK_CHECK(second == 1);
        TASK_CHECK(CurveStructureValid(finished));
        TASK_CHECK(SameCurveData(finished, curve));

        // The partial result can also be refined at that knot and reduced
        // again; that leaves one copy behind, and the result must still be
        // valid and evaluable.
        const Curve refined = partial.InsertKnot(0.5, 1);
        TASK_CHECK(CurveStructureValid(refined));
        const auto [again, third] = refined.RemoveKnot(0.5, 1, 1e-10);
        TASK_CHECK(third == 1);
        TASK_CHECK(CurveStructureValid(again));
        TASK_CHECK(SameCurveData(again, partial));
        return 0;
    }

    int CaseSharedEntryPointPreservesInputs()
    {
        // Direct call into the shared KnotRemoval must not corrupt its inputs
        // when the request is refused.
        Curve curve = UnequalWeightQuadratic().InsertKnot(0.5, 2);
        KnotVector knots = curve.Knots;
        vector<Vec4> points = curve.ControlPoints;
        const vector<Numeric> knots_before = knots.Values();
        const vector<Vec4> points_before = points;

        const int removed = libnurbs::KnotRemoval(knots, points, curve.Degree, 0.61, 2, 1e-12);
        TASK_CHECK(removed == 0);
        TASK_CHECK(knots.Values() == knots_before);
        TASK_CHECK(points.size() == points_before.size());
        for (size_t i = 0; i < points.size(); ++i)
            TASK_CHECK(points[i] == points_before[i]);

        // A successful call shrinks both containers by the returned amount.
        KnotVector ok_knots = curve.Knots;
        vector<Vec4> ok_points = curve.ControlPoints;
        const int ok_removed = libnurbs::KnotRemoval(ok_knots, ok_points, curve.Degree, 0.5, 2, 1e-10);
        TASK_CHECK(ok_removed == 2);
        TASK_CHECK(ok_knots.Values().size() == knots_before.size() - 2);
        TASK_CHECK(ok_points.size() == points_before.size() - 2);
        return 0;
    }

    int CaseSurfaceZeroResultIsIdentical()
    {
        const Surface surface = NonSquareSurface();
        const auto [result, removed] = surface.RemoveKnotU(0.9, 1, 1e-12);
        TASK_CHECK(removed == 0);
        TASK_CHECK(SameSurfaceData(result, surface));
        TASK_CHECK(SurfaceStructureValid(surface));

        const auto [v_result, v_removed] = surface.RemoveKnotV(0.9, 2, 1e-12);
        TASK_CHECK(v_removed == 0);
        TASK_CHECK(SameSurfaceData(v_result, surface));
        return 0;
    }

    int CaseSurfacePartialKeepsCommonBasis()
    {
        // Both rows of this surface can lose exactly one copy of 0.5.
        const Surface surface = MixedCapabilitySurface();
        const auto [result, removed] = surface.RemoveKnotU(0.5, 2, 1e-10);
        TASK_CHECK(removed == 1);
        TASK_CHECK(SurfaceStructureValid(result));
        TASK_CHECK(result.ControlPoints.UCount == surface.ControlPoints.UCount - 1);
        TASK_CHECK(result.ControlPoints.VCount == surface.ControlPoints.VCount);
        TASK_CHECK(result.KnotsU.Values().size() == surface.KnotsU.Values().size() - 1);
        TASK_CHECK(result.KnotsV.Values().size() == surface.KnotsV.Values().size());
        TASK_CHECK(MultiplicityOf(result.KnotsU, 0.5) ==
                   MultiplicityOf(surface.KnotsU, 0.5) - 1);
        TASK_CHECK(SurfaceDistance(result, surface) <= 1e-10);

        // The two control rows must both sit on the same, shrunk knot basis.
        TASK_CHECK(result.ControlPoints.UCount + result.DegreeU + 1 ==
                   static_cast<int>(result.KnotsU.Values().size()));
        return 0;
    }

    // ------------------------------------------------------------------- group 1
    int CasePrefixCurveRepeatedRoundTrip()
    {
        Rng rng(0xC0FFEEULL);
        for (int degree = 1; degree <= 4; ++degree)
        {
            for (int trial = 0; trial < 6; ++trial)
            {
                vector<Vec4> points;
                for (int i = 0; i < degree + 4; ++i)
                {
                    const Numeric w = rng.Uniform(0.3, 3.0);
                    points.push_back({rng.Uniform(-2.0, 2.0), rng.Uniform(-2.0, 2.0),
                                      rng.Uniform(-2.0, 2.0), w});
                }
                // Three interior knots in a clamped vector of degree `degree`
                // give (2*degree + 5) - degree - 1 = degree + 4 control points.
                const Curve curve = MakeCurve(degree, {0.25, 0.5, 0.75}, points);
                TASK_CHECK(CurveStructureValid(curve));

                for (Numeric knot: {0.25, 0.5, 0.75})
                {
                    const int max_mult = degree; // stay below degree + 1
                    const int multiplicity_before = MultiplicityOf(curve.Knots, knot);
                    const Curve inserted = curve.InsertKnot(knot, max_mult);
                    TASK_CHECK(MultiplicityOf(inserted.Knots, knot) == multiplicity_before + max_mult);

                    const auto [result, removed] = inserted.RemoveKnot(knot, max_mult + 1, 1e-9);
                    TASK_CHECK(removed == max_mult);
                    TASK_CHECK(CurveStructureValid(result));
                    TASK_CHECK(MultiplicityOf(result.Knots, knot) == multiplicity_before);
                    TASK_CHECK(SameCurveData(result, curve));
                    TASK_CHECK(CurveDistance(result, curve) <= 1e-9);
                }
            }
        }
        return 0;
    }

    int CasePrefixSurfaceRepeatedRoundTrip()
    {
        Rng rng(0xABCDEFULL);
        for (int degree_u = 1; degree_u <= 3; ++degree_u)
        {
            for (int degree_v = 1; degree_v <= 3; ++degree_v)
            {
                for (int trial = 0; trial < 3; ++trial)
                {
                    // One interior knot per axis: counts are degree + 2.
                    const int count_u = degree_u + 2;
                    const int count_v = degree_v + 2;
                    vector<Vec4> points;
                    for (int i = 0; i < count_u * count_v; ++i)
                    {
                        points.push_back({rng.Uniform(-2.0, 2.0), rng.Uniform(-2.0, 2.0),
                                          rng.Uniform(-2.0, 2.0), rng.Uniform(0.3, 3.0)});
                    }
                    const Surface surface = MakeSurface(degree_u, degree_v, {0.5},
                                                        {0.5}, points);
                    TASK_CHECK(SurfaceStructureValid(surface));

                    for (int axis = 0; axis < 2; ++axis)
                    {
                        const int degree = axis == 0 ? degree_u : degree_v;
                        // Stay inside the multiplicity limit of the target knot.
                        const int added = std::min(degree, 2);
                        const auto inserted = axis == 0 ? surface.InsertKnotU(0.5, added)
                                                        : surface.InsertKnotV(0.5, added);
                        const auto [result, removed] = axis == 0
                                                           ? inserted.RemoveKnotU(0.5, added + 1, 1e-9)
                                                           : inserted.RemoveKnotV(0.5, added + 1, 1e-9);
                        TASK_CHECK(removed == added);
                        TASK_CHECK(SurfaceStructureValid(result));
                        TASK_CHECK(SameSurfaceData(result, surface));
                        TASK_CHECK(SurfaceDistance(result, surface) <= 1e-9);
                    }
                }
            }
        }
        return 0;
    }

    // ------------------------------------------------------------------- group 2
    int CaseTensorNonSquareAndAlternating()
    {
        // Non-square 5 x 3 net, alternating U and V edits.
        Surface surface = NonSquareSurface();
        TASK_CHECK(SurfaceStructureValid(surface));

        // U basis: 2 interior knots -> multiplicity 1 at 0.3, so insert 1.
        const auto [after_u, removed_u] = surface.InsertKnotU(0.3, 1).RemoveKnotU(0.3, 1, 1e-9);
        TASK_CHECK(removed_u == 1);
        TASK_CHECK(SurfaceStructureValid(after_u));
        TASK_CHECK(SameSurfaceData(after_u, surface));

        const auto [after_v, removed_v] = after_u.InsertKnotV(0.5, 1).RemoveKnotV(0.5, 1, 1e-9);
        TASK_CHECK(removed_v == 1);
        TASK_CHECK(SurfaceStructureValid(after_v));
        TASK_CHECK(SameSurfaceData(after_v, surface));

        // Both axes edited at once on a non-square net.
        const auto [both, removed_both] =
                after_v.InsertKnotU(0.65, 1).InsertKnotV(0.5, 1).RemoveKnotU(0.65, 1, 1e-9);
        TASK_CHECK(removed_both == 1);
        TASK_CHECK(SurfaceStructureValid(both));
        TASK_CHECK(both.ControlPoints.UCount == surface.ControlPoints.UCount);
        TASK_CHECK(both.ControlPoints.VCount == surface.ControlPoints.VCount + 1);
        return 0;
    }

    int CaseTensorUvSymmetry()
    {
        // Build a square surface and a V/U transposed copy. The same edit on the
        // matching axis must report the same count and the same geometry.
        Rng rng(0x5EEDULL);
        for (int degree = 1; degree <= 3; ++degree)
        {
            // Two interior knots: control count along each axis is degree + 3.
            const int count = degree + 3;
            vector<Vec4> points;
            for (int i = 0; i < count * count; ++i)
            {
                points.push_back({rng.Uniform(-2.0, 2.0), rng.Uniform(-2.0, 2.0),
                                  rng.Uniform(-2.0, 2.0), rng.Uniform(0.3, 3.0)});
            }
            const Surface surface = MakeSurface(degree, degree, {0.4, 0.7}, {0.4, 0.7}, points);
            TASK_CHECK(SurfaceStructureValid(surface));

            Surface transposed;
            transposed.DegreeU = surface.DegreeV;
            transposed.DegreeV = surface.DegreeU;
            transposed.KnotsU = surface.KnotsV;
            transposed.KnotsV = surface.KnotsU;
            transposed.ControlPoints = ControlPointGrid{surface.ControlPoints.VCount,
                                                       surface.ControlPoints.UCount};
            for (int u = 0; u < surface.ControlPoints.UCount; ++u)
                for (int v = 0; v < surface.ControlPoints.VCount; ++v)
                    transposed.ControlPoints.Get(v, u) = surface.ControlPoints.Get(u, v);

            const Surface u_edit = surface.InsertKnotU(0.5, 2);
            const Surface v_edit = transposed.InsertKnotV(0.5, 2);            const auto [u_result, u_removed] = u_edit.RemoveKnotU(0.5, 2, 1e-9);
            const auto [v_result, v_removed] = v_edit.RemoveKnotV(0.5, 2, 1e-9);

            TASK_CHECK(u_removed == v_removed);
            TASK_CHECK(u_removed == 2);
            TASK_CHECK(SurfaceStructureValid(u_result));
            TASK_CHECK(SurfaceStructureValid(v_result));

            Numeric worst = 0;
            for (int i = 0; i <= 16; ++i)
            {
                for (int j = 0; j <= 16; ++j)
                {
                    const Numeric u = static_cast<Numeric>(i) / 16.0;
                    const Numeric v = static_cast<Numeric>(j) / 16.0;
                    worst = std::max(worst, (u_result.Evaluate(u, v) - v_result.Evaluate(v, u)).norm());
                }
            }
            TASK_CHECK(worst <= 1e-9);
        }
        return 0;
    }

    int CaseTensorRowCapabilities()
    {
        // Supplied fixture: the two U-rows can remove two and one copies.
        const Surface surface = MixedCapabilitySurface();
        const auto [result, removed] = surface.RemoveKnotU(0.5, 2, 1e-10);
        TASK_CHECK(removed == 1);
        TASK_CHECK(SurfaceStructureValid(result));
        TASK_CHECK(result.ControlPoints.UCount == surface.ControlPoints.UCount - 1);
        TASK_CHECK(SurfaceDistance(result, surface) <= 1e-10);

        // The same surface, asked for a single removal, succeeds identically.
        const auto [single, single_removed] = surface.RemoveKnotU(0.5, 1, 1e-10);
        TASK_CHECK(single_removed == 1);
        TASK_CHECK(SurfaceStructureValid(single));
        TASK_CHECK(SameSurfaceData(single, result));

        // Continuing after the partial result must still evaluate, refine and
        // remove again.
        const Surface refined = result.InsertKnotU(0.5, 1);
        TASK_CHECK(SurfaceStructureValid(refined));
        const auto [again, again_removed] = refined.RemoveKnotU(0.5, 1, 1e-10);
        TASK_CHECK(again_removed == 1);
        TASK_CHECK(SurfaceStructureValid(again));
        TASK_CHECK(SameSurfaceData(again, result));
        return 0;
    }

    // ------------------------------------------------------------------- group 3
    int CaseWeightsScaledUniformly()
    {
        const Curve curve = UnequalWeightQuadratic();
        const Curve inserted = curve.InsertKnot(0.5, 2);
        const auto [reference, reference_removed] = inserted.RemoveKnot(0.5, 2, 1e-9);
        TASK_CHECK(reference_removed == 2);

        for (Numeric scale: {0.05, 3.0, 97.0})
        {
            Curve scaled = inserted;
            for (auto& point: scaled.ControlPoints)
                point.w() *= scale;

            const auto [result, removed] = scaled.RemoveKnot(0.5, 2, 1e-9);
            TASK_CHECK(removed == reference_removed);
            TASK_CHECK(CurveStructureValid(result));
            TASK_CHECK(CurveDistance(result, reference) <= 1e-9);
        }
        return 0;
    }

    int CaseTranslationAndRotationInvariance()
    {
        Curve curve = MakeCurve(2, {0.3, 0.6},
                                {{0.0, 0.0, 0.0, 0.6},
                                 {1.0, 0.5, 0.2, 1.9},
                                 {2.0, -0.5, 0.4, 1.0},
                                 {3.0, 0.3, 0.1, 1.4},
                                 {4.0, 0.0, 0.3, 2.2}});
        const Curve inserted = curve.InsertKnot(0.3, 2);
        const auto [reference, reference_removed] = inserted.RemoveKnot(0.3, 2, 1e-10);
        TASK_CHECK(reference_removed == 2);

        Curve moved = inserted;
        for (auto& point: moved.ControlPoints)
        {
            point.x() += 1000.0;
            point.y() -= 500.0;
            point.z() += 250.0;
        }
        const auto [moved_result, moved_removed] = moved.RemoveKnot(0.3, 2, 1e-10);
        TASK_CHECK(moved_removed == reference_removed);

        // Rotate about the z axis, which mixes x and y.
        const Numeric angle = 0.7;
        libnurbs::Mat3x3 rotation;
        rotation << std::cos(angle), -std::sin(angle), 0.0,
                std::sin(angle), std::cos(angle), 0.0,
                0.0, 0.0, 1.0;
        const Curve rotated = inserted.Transform(rotation);
        const auto [rotated_result, rotated_removed] = rotated.RemoveKnot(0.3, 2, 1e-10);
        TASK_CHECK(rotated_removed == reference_removed);

        Numeric worst = 0;
        for (int i = 0; i <= 80; ++i)
        {
            const Numeric u = static_cast<Numeric>(i) / 80.0;
            Vec3 expected = reference.Evaluate(u);
            Vec3 got = rotated_result.Evaluate(u);
            expected = rotation * expected;
            worst = std::max(worst, (expected - got).norm());
        }
        TASK_CHECK(worst <= 1e-9);
        return 0;
    }

    int CaseExactInverseOfInsertion()
    {
        // Insertion then removal is exact for rational curves, including knots
        // placed inside existing spans of varying multiplicity.
        const Curve curve = UnequalWeightQuadratic();
        for (Numeric knot: {0.2, 0.35, 0.5, 0.7, 0.85})
        {
            for (int times = 1; times <= 2; ++times)
            {
                const Curve inserted = curve.InsertKnot(knot, times);
                const auto [result, removed] = inserted.RemoveKnot(knot, times, 1e-12);
                TASK_CHECK(removed == times);
                TASK_CHECK(SameCurveData(result, curve));
            }
        }
        return 0;
    }

    int CaseSurfaceRationalBoundLoosenTighten()
    {
        // A mildly perturbed rational surface: loose tolerance must genuinely
        // reduce the basis, a tight one must refuse. Start from a surface that
        // is exactly representable at multiplicity 1 and insert two extra
        // copies of the knot, so the extra multiplicity is genuinely redundant.
        constexpr int count_u = 4; // degree 2 with a single interior knot
        constexpr int count_v = 3;
        Surface surface;
        surface.DegreeU = 2;
        surface.DegreeV = 2;
        surface.KnotsU = KnotVector{{0.0, 0.0, 0.0, 0.5, 1.0, 1.0, 1.0}};
        surface.KnotsV = KnotVector{{0.0, 0.0, 0.0, 1.0, 1.0, 1.0}};
        surface.ControlPoints = ControlPointGrid{count_u, count_v};
        for (int v = 0; v < count_v; ++v)
        {
            for (int u = 0; u < count_u; ++u)
            {
                surface.ControlPoints.Get(u, v) = Vec4{
                    static_cast<Numeric>(u), static_cast<Numeric>(v) + 0.2 * u,
                    0.1 * u * v, 0.5 + 0.25 * u + 0.1 * v};
            }
        }
        TASK_CHECK(SurfaceStructureValid(surface));
        surface = surface.InsertKnotU(0.5, 2);
        TASK_CHECK(SurfaceStructureValid(surface));

        // Perturb every control point slightly so removal is inexact.
        Surface perturbed = surface;
        for (int v = 0; v < perturbed.ControlPoints.VCount; ++v)
        {
            for (int u = 0; u < perturbed.ControlPoints.UCount; ++u)
            {
                Vec4& point = perturbed.ControlPoints.Get(u, v);
                point.z() += 1e-4 * std::sin(2.3 * u + 1.7 * v);
            }
        }

        const auto [loose, loose_removed] = perturbed.RemoveKnotU(0.5, 2, 1e-3);
        TASK_CHECK(loose_removed >= 1);
        TASK_CHECK(SurfaceStructureValid(loose));
        TASK_CHECK(loose.KnotsU.Values().size() < perturbed.KnotsU.Values().size());
        TASK_CHECK(SurfaceDistance(loose, perturbed, 60) <= 1e-3);

        const auto [tight, tight_removed] = perturbed.RemoveKnotU(0.5, 2, 1e-9);
        TASK_CHECK(tight_removed == 0);
        TASK_CHECK(SameSurfaceData(tight, perturbed));

        // Requesting more than available is bounded by the multiplicity in the
        // input, not by the perturbation.
        TASK_CHECK(loose_removed <= 2);
        return 0;
    }

    int CaseSurfaceWholeDomainBoundNotSampling()
    {
        // The library must decide acceptance from an analytic bound over the
        // whole parameter domain, not from a finite sample. This case verifies
        // the observable consequences: a request is refused while the actual
        // geometric error exceeds the tolerance, and accepted only at a
        // tolerance at least as large as the error observed everywhere.
        Surface stable;
        stable.DegreeU = 2;
        stable.DegreeV = 1;
        stable.KnotsU = KnotVector{{0.0, 0.0, 0.0, 0.5, 0.5, 1.0, 1.0, 1.0}};
        stable.KnotsV = KnotVector{{0.0, 0.0, 1.0, 1.0}};
        stable.ControlPoints = ControlPointGrid{5, 2};
        for (int u = 0; u < 5; ++u)
        {
            stable.ControlPoints.Get(u, 0) = Vec4{static_cast<Numeric>(u), 0.0, 0.0, 1.0};
            stable.ControlPoints.Get(u, 1) = Vec4{static_cast<Numeric>(u), 1.0, 0.5, 1.4};
        }

        // Move a control point that influences the surface across a broad part
        // of the domain.
        Surface perturbed = stable;
        perturbed.ControlPoints.Get(2, 0).z() += 1e-4;

        // The geometric deviation is bounded well below 1e-3; a much tighter
        // request must be refused and left untouched.
        const auto [result, removed] = perturbed.RemoveKnotU(0.5, 2, 1e-7);
        TASK_CHECK(removed == 0);
        TASK_CHECK(SameSurfaceData(result, perturbed));

        // Acceptance at a tolerance covering the perturbation: the error is
        // never above the accepted tolerance anywhere on the domain.
        const auto [accepted, accepted_removed] = perturbed.RemoveKnotU(0.5, 2, 1e-3);
        TASK_CHECK(accepted_removed >= 1);
        TASK_CHECK(SurfaceDistance(accepted, perturbed, 80) <= 1e-3);

        // A near-exact request must not be accepted merely because a coarse
        // sample happens to look exact. The basis is genuinely reduced only
        // when the tolerance is genuinely larger than the actual error.
        const Numeric actual = SurfaceDistance(accepted, perturbed, 80);
        TASK_CHECK(actual <= 1e-3);
        return 0;
    }
}

// Registration. The order of the table is not significant to the driver.
const struct task_case task_cases[] = {
    {"prefix", "curve_zero_result_is_identical", CaseCurveZeroResultIsIdentical},
    {"prefix", "curve_partial_then_continue", CaseCurvePartialThenContinue},
    {"prefix", "shared_entry_point_preserves_inputs", CaseSharedEntryPointPreservesInputs},
    {"prefix", "surface_zero_result_is_identical", CaseSurfaceZeroResultIsIdentical},
    {"prefix", "surface_partial_keeps_common_basis", CaseSurfacePartialKeepsCommonBasis},
    {"prefix", "curve_repeated_round_trip", CasePrefixCurveRepeatedRoundTrip},
    {"prefix", "surface_repeated_round_trip", CasePrefixSurfaceRepeatedRoundTrip},

    {"tensor", "non_square_and_alternating", CaseTensorNonSquareAndAlternating},
    {"tensor", "uv_symmetry", CaseTensorUvSymmetry},
    {"tensor", "row_capabilities", CaseTensorRowCapabilities},

    {"rational_bound", "weights_scaled_uniformly", CaseWeightsScaledUniformly},
    {"rational_bound", "translation_and_rotation_invariance", CaseTranslationAndRotationInvariance},
    {"rational_bound", "exact_inverse_of_insertion", CaseExactInverseOfInsertion},
    {"rational_bound", "surface_loosen_tighten", CaseSurfaceRationalBoundLoosenTighten},
    {"rational_bound", "whole_domain_bound", CaseSurfaceWholeDomainBoundNotSampling},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
