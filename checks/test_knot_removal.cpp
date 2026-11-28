// Regression tests for the knot-removal path (KnotRemoval, Curve::RemoveKnot,
// Surface::RemoveKnotU/V).
//
// The contract exercised here:
//   * zero removals return an independent object identical to the source and
//     leave the shared KnotRemoval arguments untouched;
//   * t removals shrink the control net and the knot vector by exactly t and
//     keep the degree, the remaining knots and the parameter correspondence;
//   * t may be smaller than the requested count, and a rejection after a
//     partial success still leaves a usable curve/surface;
//   * surfaces pick one common number of removals for all rows, on one common
//     knot basis, even when the rows differ in how many removals they support;
//   * the acceptance test is a conservative whole-domain geometric bound that
//     respects the rational denominator and does not grow across requests.

#include "../task_support/check.h"

#include <libnurbs/Algorithm/KnotRemoval.hpp>
#include <libnurbs/Curve/Curve.hpp>
#include <libnurbs/Surface/Surface.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace libnurbs;

namespace
{
    // ----------------------------------------------------------------------
    // Small helpers
    // ----------------------------------------------------------------------

    double Distance3(const Vec3& a, const Vec3& b)
    {
        return (a - b).norm();
    }

    // Sample the largest 3D Euclidean distance between two curves.  Only used
    // to cross-check the production bound; never as an acceptance criterion.
    double SampledCurveError(const Curve& a, const Curve& b, int samples = 2001)
    {
        double worst = 0.0;
        for (int i = 0; i <= samples; ++i)
        {
            const double x = static_cast<double>(i) / samples;
            worst          = std::max(worst, Distance3(a.Evaluate(x), b.Evaluate(x)));
        }
        return worst;
    }

    double SampledSurfaceError(const Surface& a, const Surface& b, int samples = 41)
    {
        double worst = 0.0;
        for (int i = 0; i <= samples; ++i)
        {
            for (int j = 0; j <= samples; ++j)
            {
                const double u = static_cast<double>(i) / samples;
                const double v = static_cast<double>(j) / samples;
                worst          = std::max(worst, Distance3(a.Evaluate(u, v), b.Evaluate(u, v)));
            }
        }
        return worst;
    }

    // Sampled span of a curve/surface, used to verify that the geometric bound
    // is at least as large as the observed error.
    struct DomainError
    {
        double sampled;
    };

    // The conic (rational quarter circle) control points used by the existing
    // unit tests.  Degree 2, knots 0,0,0, .25,.25, .5,.5, .75,.75, 1,1,1.
    Curve CircleCurve()
    {
        const Numeric k = std::sqrt(2.0) / 2.0;
        Curve curve;
        curve.Degree = 2;
        curve.Knots  = KnotVector{{0, 0, 0, 0.25, 0.25, 0.5, 0.5, 0.75, 0.75, 1, 1, 1}};
        curve.ControlPoints = {
            {1, 0, 0, 1},
            {1, 1, 0, k},
            {0, 1, 0, 1},
            {-1, 1, 0, k},
            {-1, 0, 0, 1},
            {-1, -1, 0, k},
            {0, -1, 0, 1},
            {1, -1, 0, k},
            {1, 0, 0, 1},
        };
        return curve;
    }

    // Rational degree-2 curve with a single interior knot of multiplicity one.
    Curve KinkCurve()
    {
        Curve curve;
        curve.Degree = 2;
        curve.Knots  = KnotVector{{0, 0, 0, 0.5, 1, 1, 1}};
        curve.ControlPoints = {
            {0, 0, 0, 1},
            {1, 1, 0, 2},
            {2, -0.5, 1, 0.7},
            {3, 0.2, 0, 1.2},
        };
        return curve;
    }

    Curve BezierCurve()
    {
        Curve curve;
        curve.Degree = 2;
        curve.Knots  = KnotVector{{0, 0, 0, 1, 1, 1}};
        curve.ControlPoints = {
            {0, 0, 0, 1},
            {1, 1, 0, 1},
            {3, 0, 0, 1},
        };
        return curve;
    }

    // Non-square tensor product surface: U rows have degree 2 and different
    // removal abilities, V has degree 1.  Row 0 is a Bezier curve refined twice
    // at u = 0.5 (exactly two removals); row 1 is the kink curve refined once
    // (exactly one removal).  Both rows live on the same U knot vector.
    Surface MixedSurface()
    {
        Surface surface;
        surface.DegreeU = 2;
        surface.DegreeV = 1;
        surface.KnotsU  = BezierCurve().InsertKnot(0.5, 2).Knots;
        surface.KnotsV  = KnotVector{{0, 0, 1, 1}};

        const Curve row0 = BezierCurve().InsertKnot(0.5, 2);
        const Curve row1 = KinkCurve().InsertKnot(0.5);

        surface.ControlPoints = ControlPointGrid(static_cast<int>(row0.ControlPoints.size()), 2);
        for (int i = 0; i < static_cast<int>(row0.ControlPoints.size()); ++i)
        {
            surface.ControlPoints.Get(i, 0) = row0.ControlPoints[i];
            Vec4 lifted                     = row1.ControlPoints[i];
            lifted.z() += 2.0;
            surface.ControlPoints.Get(i, 1) = lifted;
        }
        return surface;
    }

    // A surface that is exactly representable before an inserted knot, so the
    // inverse removal must succeed at a tight tolerance.
    Surface ExactlyRefinedSurface()
    {
        return MixedSurface().InsertKnotU(0.25, 1);
    }

    // A gently perturbed copy of ExactlyRefinedSurface: the removal of the
    // inserted knot is now approximate.
    Surface PerturbedSurface()
    {
        Surface surface = ExactlyRefinedSurface();
        Vec4& point     = surface.ControlPoints.Get(2, 0);
        point.head<3>() += Vec3(2e-4, -1e-4, 3e-4);
        return surface;
    }

    // Choose the interior knot of the shared U vector with the largest
    // multiplicity, breaking ties by value.  Returns 0 when there is none.
    Numeric PickRemovableUKnot(const Surface& surface)
    {
        Numeric best = 0.0;
        int best_mult = 0;
        for (const auto& value: surface.KnotsU.Values())
        {
            if (value <= 0.0 || value >= 1.0) continue;
            const int mult = surface.KnotsU.GetMultiplicity(value);
            if (mult > best_mult)
            {
                best      = value;
                best_mult = mult;
            }
        }
        return best;
    }

    bool SameKnotVector(const KnotVector& a, const KnotVector& b)
    {
        return a.Values() == b.Values();
    }
}

// ==========================================================================
// prefix group: curve-level prefix behaviour, zero / partial success and the
// shared KnotRemoval routine.
// ==========================================================================

static int TestCurveExactInversePrefix()
{
    const Curve source = CircleCurve();
    const Curve refined = source.InsertKnot(0.2, 2);

    // Requesting two removals must really remove two copies and land back on
    // the original curve (including the original control net).
    const auto [two, removed_two] = refined.RemoveKnot(0.2, 2, 1e-12);
    TASK_CHECK(removed_two == 2);
    TASK_CHECK(two.Knots.Values().size() == refined.Knots.Values().size() - 2);
    TASK_CHECK(two.ControlPoints.size() == refined.ControlPoints.size() - 2);
    TASK_CHECK(two.Knots.GetMultiplicity(0.2) == 0);
    TASK_CHECK(SameKnotVector(two.Knots, source.Knots));
    TASK_CHECK(SampledCurveError(refined, two) <= 1e-10);

    // The returned control points are the analytic inverse (allowing rounding).
    TASK_CHECK(two.ControlPoints.size() == source.ControlPoints.size());
    for (size_t i = 0; i < two.ControlPoints.size(); ++i)
    {
        TASK_CHECK((two.ControlPoints[i] - source.ControlPoints[i]).norm() <= 1e-10);
    }

    // A single requested removal returns a curve with one fewer copy.
    const auto [one, removed_one] = refined.RemoveKnot(0.2, 1, 1e-12);
    TASK_CHECK(removed_one == 1);
    TASK_CHECK(one.Knots.GetMultiplicity(0.2) == 1);
    TASK_CHECK(SampledCurveError(refined, one) <= 1e-10);
    return 0;
}

static int TestCurveRefusedReturnsIndependentIdenticalCopy()
{
    const Curve source = KinkCurve();
    const Curve backup = source;

    // 0.5 has multiplicity one but the knot is geometrically essential, so a
    // tight tolerance must refuse it and leave the source untouched.
    const auto [refused, removed] = source.RemoveKnot(0.5, 1, 1e-10);
    TASK_CHECK(removed == 0);
    TASK_CHECK(refused.ControlPoints.size() == source.ControlPoints.size());
    TASK_CHECK(SameKnotVector(refused.Knots, source.Knots));
    TASK_CHECK(SampledCurveError(refused, source) == 0.0);
    for (size_t i = 0; i < source.ControlPoints.size(); ++i)
    {
        TASK_CHECK((refused.ControlPoints[i] - source.ControlPoints[i]).norm() == 0.0);
    }

    // The zero-success value is independent: mutating it must not touch the
    // source.
    Curve independent = refused;
    independent.ControlPoints[0].x() = 12345.0;
    TASK_CHECK(source.ControlPoints[0].x() != 12345.0);
    TASK_CHECK(backup.ControlPoints[0].x() == source.ControlPoints[0].x());

    // Removing a knot value that is not present is also a zero-success result.
    const auto [absent, absent_removed] = source.RemoveKnot(0.33, 3, 1e-8);
    TASK_CHECK(absent_removed == 0);
    TASK_CHECK(SameKnotVector(absent.Knots, source.Knots));
    TASK_CHECK(absent.ControlPoints.size() == source.ControlPoints.size());
    return 0;
}

static int TestCurveRequestClampedAndPartial()
{
    const Curve kink = KinkCurve();

    // Requesting more removals than the multiplicity is clamped to the
    // multiplicity; result size shrinks by the actual count.
    const auto [clamped, removed] = kink.RemoveKnot(0.5, 5, 1e-6);
    TASK_CHECK(removed == 0 || removed == 1);
    if (removed == 1)
    {
        TASK_CHECK(clamped.ControlPoints.size() == kink.ControlPoints.size() - 1);
        TASK_CHECK(clamped.Knots.Values().size() == kink.Knots.Values().size() - 1);
        TASK_CHECK(clamped.Knots.GetMultiplicity(0.5) == 0);
        TASK_CHECK(SampledCurveError(kink, clamped) <= 1e-6 + 1e-12);
    }

    // Partial success: one removal is exact, the second is not.
    const Curve refined           = kink.InsertKnot(0.25, 3);
    const auto [partial, partial_count] = refined.RemoveKnot(0.25, 3, 1e-12);
    TASK_CHECK(partial_count >= 1 && partial_count <= 3);
    TASK_CHECK(partial.ControlPoints.size() == refined.ControlPoints.size() - partial_count);
    TASK_CHECK(partial.Knots.Values().size() == refined.Knots.Values().size() - partial_count);
    TASK_CHECK(SampledCurveError(refined, partial) <= 1e-10);

    // A rejection after the partial success leaves a curve that can be
    // evaluated, refined and reduced again.
    const auto [after_reject, reject_count] = partial.RemoveKnot(0.25, 1, 1e-12);
    TASK_CHECK(reject_count == 0);
    TASK_CHECK(SameKnotVector(after_reject.Knots, partial.Knots));
    const Vec3 value = after_reject.Evaluate(0.37);
    TASK_CHECK(std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z()));

    const Curve reinserted             = after_reject.InsertKnot(0.37, 1);
    const auto [again, again_count]    = reinserted.RemoveKnot(0.37, 1, 1e-12);
    TASK_CHECK(again_count == 1);
    TASK_CHECK(SampledCurveError(reinserted, again) <= 1e-10);
    return 0;
}

static int TestDirectKnotRemovalContract()
{
    const Curve source = KinkCurve();
    KnotVector knots   = source.Knots;
    std::vector<Vec4> points = source.ControlPoints;
    const KnotVector knots_before = knots;
    const std::vector<Vec4> points_before = points;

    // Rejected removal: arguments must be preserved exactly.
    const int rejected = KnotRemoval(knots, points, source.Degree, 0.5, 1, 1e-10);
    TASK_CHECK(rejected == 0);
    TASK_CHECK(SameKnotVector(knots, knots_before));
    TASK_CHECK(points == points_before);

    // Successful removal: sizes shrink by exactly the returned count and the
    // result reproduces the source curve.
    const Curve refined = KinkCurve().InsertKnot(0.25, 2);
    KnotVector refined_knots = refined.Knots;
    std::vector<Vec4> refined_points = refined.ControlPoints;
    const int removed = KnotRemoval(refined_knots, refined_points, refined.Degree, 0.25, 2, 1e-12);
    TASK_CHECK(removed == 2);
    TASK_CHECK(refined_knots.Values().size() == refined.Knots.Values().size() - 2);
    TASK_CHECK(refined_points.size() == refined.ControlPoints.size() - 2);

    Curve reconstructed;
    reconstructed.Degree        = refined.Degree;
    reconstructed.Knots         = refined_knots;
    reconstructed.ControlPoints = refined_points;
    TASK_CHECK(SampledCurveError(refined, reconstructed) <= 1e-10);
    return 0;
}

static int TestCurveZeroRemovalsAreIdenticalAndIndependent()
{
    // Zero-success results are independent objects: mutating the returned
    // curve (and its control point storage) must not affect the source.
    const Curve source = CircleCurve();
    auto [refused, removed] = source.RemoveKnot(0.5, 1, 1e-12);
    TASK_CHECK(removed == 0);
    TASK_CHECK(refused.ControlPoints.size() == source.ControlPoints.size());
    TASK_CHECK(SameKnotVector(refused.Knots, source.Knots));

    const Vec4 source_first = source.ControlPoints.front();
    const Vec4 source_last  = source.ControlPoints.back();
    const Vec4 mutated = source_first + Vec4(1.0, 2.0, 3.0, 0.25);
    refused.ControlPoints.front() = mutated;
    TASK_CHECK(source.ControlPoints.front() == source_first);

    // Two independent zero-success results must not alias each other either.
    const auto [first, first_removed]   = source.RemoveKnot(0.5, 1, 1e-12);
    const auto [second, second_removed] = source.RemoveKnot(0.5, 1, 1e-12);
    TASK_CHECK(first_removed == 0 && second_removed == 0);
    TASK_CHECK(first.ControlPoints.front() == second.ControlPoints.front());
    TASK_CHECK(first.ControlPoints.back() == source_last);
    return 0;
}

// ==========================================================================
// tensor group: surface row coupling, U/V symmetry, non-square grids and
// alternating operations.
// ==========================================================================

static int TestSurfaceMixedRowsPartialSuccess()
{
    const Surface source = MixedSurface();
    const Numeric knot   = PickRemovableUKnot(source);
    TASK_CHECK(knot == 0.5);

    // Row 0 supports two removals, row 1 supports one.  One common removal is
    // the largest achievable on a shared knot basis.
    const auto [reduced, removed] = source.RemoveKnotU(knot, 2, 1e-10);
    TASK_CHECK(removed == 1);
    TASK_CHECK(reduced.ControlPoints.UCount == source.ControlPoints.UCount - 1);
    TASK_CHECK(reduced.ControlPoints.VCount == source.ControlPoints.VCount);
    TASK_CHECK(reduced.KnotsU.Values().size() == source.KnotsU.Values().size() - 1);
    TASK_CHECK(reduced.KnotsU.GetMultiplicity(knot) == 1);
    TASK_CHECK(SampledSurfaceError(source, reduced) <= 1e-10);

    // The individual rows genuinely differ in ability: check them with the
    // curve API on the same knot vector.
    for (int v = 0; v < source.ControlPoints.VCount; ++v)
    {
        Curve row;
        row.Degree        = source.DegreeU;
        row.Knots         = source.KnotsU;
        row.ControlPoints = source.ControlPoints.GetV(v);
        const auto [row_reduced, row_removed] = row.RemoveKnot(knot, 2, 1e-10);
        TASK_CHECK(row_removed == (v == 0 ? 2 : 1));
        (void) row_reduced;
    }
    return 0;
}

static int TestSurfaceCommonBasisForAllRows()
{
    const Surface source = MixedSurface();
    const auto [reduced, removed] = source.RemoveKnotU(0.5, 2, 1e-10);
    TASK_CHECK(removed >= 1);

    // Every row of the result must have the same number of control points and
    // must be evaluated against the single shared knot vector: no row may have
    // been shrunk differently.
    const int expected_count = reduced.ControlPoints.UCount;
    TASK_CHECK(expected_count == source.ControlPoints.UCount - removed);
    for (int v = 0; v < reduced.ControlPoints.VCount; ++v)
    {
        Curve row;
        row.Degree        = reduced.DegreeU;
        row.Knots         = reduced.KnotsU;
        row.ControlPoints = reduced.ControlPoints.GetV(v);
        // The number of control points has to match the shared basis.
        TASK_CHECK(static_cast<int>(row.ControlPoints.size()) == expected_count);
        // A row on the shared basis must evaluate to a finite 3D point.
        const Vec3 value = row.Evaluate(0.43);
        TASK_CHECK(std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z()));
    }

    // The common basis keeps the parameter correspondence: the reduced knot
    // vector is the source vector with `removed` copies of 0.5 taken away and
    // every other knot unchanged.
    std::vector<Numeric> expected = source.KnotsU.Values();
    for (int i = 0; i < removed; ++i)
    {
        expected.erase(std::find(expected.begin(), expected.end(), 0.5));
    }
    TASK_CHECK(reduced.KnotsU.Values() == expected);
    TASK_CHECK(reduced.KnotsU.Values().size() == source.KnotsU.Values().size() - removed);
    return 0;
}

static int TestSurfaceUVSymmetry()
{
    // Build the transpose of the mixed surface: U <-> V with the grid
    // transposed.  Removing the multi-knot along U of the transpose must give
    // the same counts as removing along V of the original transposed back.
    const Surface source = MixedSurface();
    const Numeric knot   = 0.5;

    // Remove along V after explicitly inserting the same knot in V.  Request
    // two, expect the common capability (one).
    const Surface inserted = source.InsertKnotV(knot, 2);
    const auto [v_reduced, v_removed] = inserted.RemoveKnotV(knot, 2, 1e-12);
    TASK_CHECK(v_removed == 2);
    TASK_CHECK(v_reduced.ControlPoints.VCount == inserted.ControlPoints.VCount - 2);
    TASK_CHECK(v_reduced.ControlPoints.UCount == inserted.ControlPoints.UCount);
    TASK_CHECK(v_reduced.KnotsV.Values().size() == inserted.KnotsV.Values().size() - 2);
    TASK_CHECK(SampledSurfaceError(inserted, v_reduced) <= 1e-10);

    // Now the same operation with U and V swapped on a transposed surface.
    Surface transposed;
    transposed.DegreeU = source.DegreeV;
    transposed.DegreeV = source.DegreeU;
    transposed.KnotsU  = source.KnotsV;
    transposed.KnotsV  = source.KnotsU;
    transposed.ControlPoints = ControlPointGrid(source.ControlPoints.VCount, source.ControlPoints.UCount);
    for (int u = 0; u < source.ControlPoints.UCount; ++u)
    {
        for (int v = 0; v < source.ControlPoints.VCount; ++v)
        {
            transposed.ControlPoints.Get(v, u) = source.ControlPoints.Get(u, v);
        }
    }

    const Surface transposed_inserted = transposed.InsertKnotU(knot, 2);
    const auto [t_reduced, t_removed] = transposed_inserted.RemoveKnotU(knot, 2, 1e-12);
    TASK_CHECK(t_removed == v_removed);
    TASK_CHECK(t_reduced.ControlPoints.UCount == v_reduced.ControlPoints.VCount);
    TASK_CHECK(t_reduced.ControlPoints.VCount == v_reduced.ControlPoints.UCount);
    TASK_CHECK(SameKnotVector(t_reduced.KnotsU, v_reduced.KnotsV));
    TASK_CHECK(SameKnotVector(t_reduced.KnotsV, v_reduced.KnotsU));
    return 0;
}

static int TestSurfaceNonSquareAlternating()
{
    const Surface source = MixedSurface();
    TASK_CHECK(source.ControlPoints.UCount != source.ControlPoints.VCount);

    const Surface refined = source.InsertKnotU(0.3, 1).InsertKnotV(0.7, 1);
    const auto [u_done, u_count] = refined.RemoveKnotU(0.3, 1, 1e-12);
    const auto [uv_done, v_count] = u_done.RemoveKnotV(0.7, 1, 1e-12);
    TASK_CHECK(u_count == 1);
    TASK_CHECK(v_count == 1);
    TASK_CHECK(uv_done.ControlPoints.UCount == refined.ControlPoints.UCount - 1);
    TASK_CHECK(uv_done.ControlPoints.VCount == refined.ControlPoints.VCount - 1);
    TASK_CHECK(SampledSurfaceError(refined, uv_done) <= 1e-10);
    TASK_CHECK(uv_done.KnotsU.GetMultiplicity(0.3) == 0);
    TASK_CHECK(uv_done.KnotsV.GetMultiplicity(0.7) == 0);

    // Reverse order must agree too.
    const auto [v_first, v_first_count] = refined.RemoveKnotV(0.7, 1, 1e-12);
    const auto [vu_done, u_second_count] = v_first.RemoveKnotU(0.3, 1, 1e-12);
    TASK_CHECK(v_first_count == 1);
    TASK_CHECK(u_second_count == 1);
    TASK_CHECK(SampledSurfaceError(refined, vu_done) <= 1e-10);
    return 0;
}

static int TestSurfaceRejectedCopyIndependent()
{
    // A perturbed surface is not an exact refinement any more; an extremely
    // tight tolerance must refuse and return an independent copy.
    const Surface plain = PerturbedSurface();
    const auto [refused, removed] = plain.RemoveKnotU(0.25, 1, 1e-13);
    TASK_CHECK(removed == 0);

    if (removed == 0)
    {
        TASK_CHECK(refused.ControlPoints.UCount == plain.ControlPoints.UCount);
        TASK_CHECK(refused.ControlPoints.VCount == plain.ControlPoints.VCount);
        TASK_CHECK(SameKnotVector(refused.KnotsU, plain.KnotsU));
        // Independent storage.
        Surface copy = refused;
        copy.ControlPoints.Get(0, 0).x() = 999.0;
        TASK_CHECK(plain.ControlPoints.Get(0, 0).x() != 999.0);
    }
    return 0;
}

// ==========================================================================
// rational_bound group: weights, invariance and the geometric bound itself.
// ==========================================================================

static int TestWeightsScaledUniformly()
{
    const Surface source = MixedSurface();
    Surface scaled       = source;
    const Numeric factor = 7.5;
    for (auto& point: scaled.ControlPoints.Values)
    {
        point.w() *= factor;
    }

    const auto [source_reduced, source_removed] = source.RemoveKnotU(0.5, 2, 1e-10);
    const auto [scaled_reduced, scaled_removed] = scaled.RemoveKnotU(0.5, 2, 1e-10);
    TASK_CHECK(source_removed == scaled_removed);
    TASK_CHECK(scaled_reduced.ControlPoints.UCount == source_reduced.ControlPoints.UCount);
    TASK_CHECK(SameKnotVector(scaled_reduced.KnotsU, source_reduced.KnotsU));
    // Geometry is unchanged by the uniform weight scaling.
    TASK_CHECK(SampledSurfaceError(scaled, scaled_reduced) <= 1e-10);
    TASK_CHECK(SampledSurfaceError(source, source_reduced) <= 1e-10);

    // Curve variant.
    const Curve circle = CircleCurve();
    Curve circle_scaled = circle;
    for (auto& point: circle_scaled.ControlPoints)
    {
        point.w() *= 3.25;
    }
    const Curve refined        = circle_scaled.InsertKnot(0.2, 2);
    const auto [reduced, count] = refined.RemoveKnot(0.2, 2, 1e-12);
    TASK_CHECK(count == 2);
    TASK_CHECK(SampledCurveError(refined, reduced) <= 1e-10);
    return 0;
}

static int TestTranslationAndRigidRotationInvariance()
{
    const Surface source = PerturbedSurface();
    const Numeric knot   = 0.25;

    // Baseline decision at a deliberately loose tolerance.
    const auto [base_reduced, base_removed] = source.RemoveKnotU(knot, 1, 5e-3);
    TASK_CHECK(base_removed == 1);

    // Translation.
    Surface translated = source;
    const Vec3 shift(1.5, -2.0, 0.3);
    for (auto& point: translated.ControlPoints.Values)
    {
        // The control points are stored in Cartesian coordinates, so a rigid
        // translation moves the xyz part directly (weights stay unchanged).
        point.head<3>() += shift;
    }
    const auto [translated_reduced, translated_removed] = translated.RemoveKnotU(knot, 1, 5e-3);
    TASK_CHECK(translated_removed == base_removed);

    // Rigid rotation about Z.
    Surface rotated = source;
    const double angle = 0.7;
    Mat3x3 rotation;
    rotation << std::cos(angle), -std::sin(angle), 0.0,
                std::sin(angle), std::cos(angle), 0.0,
                0.0, 0.0, 1.0;
    for (auto& point: rotated.ControlPoints.Values)
    {
        point.head<3>() = rotation * point.head<3>();
    }
    const auto [rotated_reduced, rotated_removed] = rotated.RemoveKnotU(knot, 1, 5e-3);
    TASK_CHECK(rotated_removed == base_removed);

    // Exact refinement still inverts after the transforms.
    for (const Surface* candidate: {&translated, &rotated})
    {
        const Surface refined = candidate->InsertKnotU(0.4, 2);
        const auto [reduced, count] = refined.RemoveKnotU(0.4, 2, 1e-10);
        TASK_CHECK(count == 2);
        TASK_CHECK(SampledSurfaceError(refined, reduced) <= 1e-9);
    }
    return 0;
}

static int TestPerturbedLooseAcceptsTightRejects()
{
    const Surface source = PerturbedSurface();
    const Numeric knot   = 0.25;

    const auto [loose_reduced, loose_removed] = source.RemoveKnotU(knot, 1, 1e-2);
    TASK_CHECK(loose_removed == 1);
    TASK_CHECK(loose_reduced.ControlPoints.UCount == source.ControlPoints.UCount - 1);
    TASK_CHECK(loose_reduced.KnotsU.GetMultiplicity(knot) == 0);

    // The accepted result must be inside the requested tolerance, measured on
    // the whole parametric domain.
    const double observed = SampledSurfaceError(source, loose_reduced);
    TASK_CHECK(observed <= 1e-2);

    const auto [tight_reduced, tight_removed] = source.RemoveKnotU(knot, 1, 1e-10);
    TASK_CHECK(tight_removed == 0);
    TASK_CHECK(tight_reduced.ControlPoints.UCount == source.ControlPoints.UCount);
    TASK_CHECK(SameKnotVector(tight_reduced.KnotsU, source.KnotsU));

    // The same decision under a uniform positive weight scale.
    Surface scaled = source;
    for (auto& point: scaled.ControlPoints.Values)
    {
        point.w() *= 11.0;
    }
    const auto [scaled_loose, scaled_loose_removed] = scaled.RemoveKnotU(knot, 1, 1e-2);
    TASK_CHECK(scaled_loose_removed == 1);
    TASK_CHECK(SampledSurfaceError(scaled, scaled_loose) <= 1e-2);
    return 0;
}

static int TestCurveToleranceAndBound()
{
    // The removal error for a deliberately perturbed refinement must respect
    // the tolerance over the whole curve.
    Curve source = KinkCurve().InsertKnot(0.25, 1);
    source.ControlPoints[2].head<3>() += Vec3(1e-3, 2e-3, -1e-3);

    const auto [loose, loose_count] = source.RemoveKnot(0.25, 1, 5e-3);
    TASK_CHECK(loose_count == 1);
    TASK_CHECK(SampledCurveError(source, loose) <= 5e-3);

    const auto [tight, tight_count] = source.RemoveKnot(0.25, 1, 1e-12);
    TASK_CHECK(tight_count == 0);

    // A conservative bound must dominate the actual error.  Use the production
    // bound on the accepted result: refine the reduced curve back to the
    // source knot basis and compare homogeneous nets.
    std::vector<std::vector<Vec4>> original_rows{};
    std::vector<std::vector<Vec4>> candidate_rows{};
    std::vector<Vec4> original_homo;
    for (const auto& point: source.ControlPoints)
    {
        original_homo.push_back(ToHomo(point));
    }
    std::vector<Numeric> refined_knots;
    std::vector<Vec4> refined_points;
    libnurbs::detail::InsertKnotRaw(loose.Knots.Values(), [&]
    {
        std::vector<Vec4> points;
        for (const auto& point: loose.ControlPoints) points.push_back(ToHomo(point));
        return points;
    }(), source.Degree, 0.25, 1, refined_knots, refined_points);

    original_rows.push_back(original_homo);
    candidate_rows.push_back(refined_points);
    const Numeric bound = libnurbs::detail::GeometryBound(original_rows, candidate_rows);
    TASK_CHECK(bound >= SampledCurveError(source, loose) - 1e-12);
    TASK_CHECK(bound <= 5e-3);
    return 0;
}

static int TestSurfaceBoundRespectsWholeDomain()
{
    // A perturbation that only becomes visible away from the perturbed control
    // point is still caught by the whole-domain bound.
    Surface source = MixedSurface().InsertKnotU(0.25, 1);
    source.ControlPoints.Get(3, 1).head<3>() += Vec3(4e-4, -6e-4, 2e-4);

    const double observed = SampledSurfaceError(source, source);
    TASK_CHECK(observed == 0.0);

    const auto [loose, loose_count] = source.RemoveKnotU(0.25, 1, 1e-2);
    TASK_CHECK(loose_count == 1);
    TASK_CHECK(SampledSurfaceError(source, loose) <= 1e-2);

    const auto [tight, tight_count] = source.RemoveKnotU(0.25, 1, 1e-9);
    TASK_CHECK(tight_count == 0);
    return 0;
}

static int TestErrorRelativeToOriginalInput()
{
    // Two sequential requests must each be bounded relative to the input of
    // that call; a successful second request must not silently accumulate a
    // larger error than allowed.
    const Surface source = MixedSurface();
    const Surface refined = source.InsertKnotU(0.25, 1).InsertKnotU(0.75, 2);

    const auto [first, first_count] = refined.RemoveKnotU(0.25, 1, 1e-2);
    TASK_CHECK(first_count == 1);
    TASK_CHECK(SampledSurfaceError(refined, first) <= 1e-2);

    const auto [second, second_count] = first.RemoveKnotU(0.75, 2, 1e-2);
    TASK_CHECK(second_count == 2);
    TASK_CHECK(SampledSurfaceError(first, second) <= 1e-2);
    return 0;
}

// ==========================================================================
// Registration
// ==========================================================================

const struct task_case task_cases[] = {
    // prefix
    {"prefix", "curve_exact_inverse_prefix", TestCurveExactInversePrefix},
    {"prefix", "curve_refused_identical_independent", TestCurveRefusedReturnsIndependentIdenticalCopy},
    {"prefix", "curve_request_clamped_partial", TestCurveRequestClampedAndPartial},
    {"prefix", "direct_knot_removal_contract", TestDirectKnotRemovalContract},
    {"prefix", "curve_zero_independent", TestCurveZeroRemovalsAreIdenticalAndIndependent},

    // tensor
    {"tensor", "surface_mixed_rows_partial", TestSurfaceMixedRowsPartialSuccess},
    {"tensor", "surface_common_basis", TestSurfaceCommonBasisForAllRows},
    {"tensor", "surface_uv_symmetry", TestSurfaceUVSymmetry},
    {"tensor", "surface_non_square_alternating", TestSurfaceNonSquareAlternating},
    {"tensor", "surface_rejected_copy_independent", TestSurfaceRejectedCopyIndependent},

    // rational_bound
    {"rational_bound", "weights_scaled_uniformly", TestWeightsScaledUniformly},
    {"rational_bound", "translation_rotation_invariance", TestTranslationAndRigidRotationInvariance},
    {"rational_bound", "perturbed_loose_tight", TestPerturbedLooseAcceptsTightRejects},
    {"rational_bound", "curve_tolerance_bound", TestCurveToleranceAndBound},
    {"rational_bound", "surface_bound_whole_domain", TestSurfaceBoundRespectsWholeDomain},
    {"rational_bound", "error_relative_to_input", TestErrorRelativeToOriginalInput},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
