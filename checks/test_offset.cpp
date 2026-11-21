#include "task_support/check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>

#include <libnurbs/Surface/Surface.hpp>
#include <libnurbs/Surface/OffsetFit.hpp>
#include <libnurbs/Algorithm/OffsetBounds.hpp>

using namespace libnurbs;

namespace
{
    // ----------------------------------------------------------------------
    // Analytic reference surfaces and their exact normal offsets.
    // ----------------------------------------------------------------------

    struct AnalyticPoint
    {
        Vec3 Point;
        Vec3 Normal;
    };

    AnalyticPoint PlaneReference(double u, double v, double /*distance*/)
    {
        return {Vec3(u, v, 0.0), Vec3(0.0, 0.0, 1.0)};
    }

    AnalyticPoint CylinderReference(double u, double v, double /*distance*/)
    {
        // Exact rational-quadratic unit quarter circle matching the NURBS
        // control polygon (1,0),(1,1),(0,1) with weights (1, sqrt2/2, 1).
        // The parameter is the rational Bézier parameter, not the polar
        // angle. S_u x S_v points toward the axis (inward radial).
        const double c = std::sqrt(2.0) / 2.0;
        const double omu = 1.0 - u;
        const double weight = omu * omu + 2.0 * c * u * omu + u * u;
        const double x = (omu * omu + 2.0 * c * u * omu) / weight;
        const double z = (2.0 * c * u * omu + u * u) / weight;
        return {Vec3(x, v, z), Vec3(-x, 0.0, -z)};
    }

    AnalyticPoint PolynomialReference(double u, double v, double /*distance*/)
    {
        const double z = 0.2 * u * u + 0.15 * u * v + 0.1 * v * v;
        const Vec3 su(1.0, 0.0, 0.4 * u + 0.15 * v);
        const Vec3 sv(0.0, 1.0, 0.15 * u + 0.2 * v);
        return {Vec3(u, v, z), su.cross(sv).normalized()};
    }

    Vec3 AnalyticTarget(AnalyticPoint (*ref)(double, double, double),
                        double u, double v, double distance)
    {
        const AnalyticPoint rp = ref(u, v, distance);
        return rp.Point + distance * rp.Normal;
    }

    // ----------------------------------------------------------------------
    // NURBS constructions of the same three surfaces.
    // ----------------------------------------------------------------------

    Surface MakePlaneSurface()
    {
        Surface s;
        s.DegreeU = s.DegreeV = 3;
        s.KnotsU = s.KnotsV = KnotVector{{0, 0, 0, 0, 1, 1, 1, 1}};
        s.ControlPoints = ControlPointGrid(4, 4);
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
                s.ControlPoints.Get(i, j) = Vec4(i / 3.0, j / 3.0, 0.0, 1.0);
        return s;
    }

    Surface MakeCylinderSurface()
    {
        Surface s;
        s.DegreeU = s.DegreeV = 2;
        s.KnotsU = s.KnotsV = KnotVector{{0, 0, 0, 1, 1, 1}};
        s.ControlPoints = ControlPointGrid(3, 3);
        const double wq = std::sqrt(2.0) / 2.0;
        const Vec3 ring[3] = {Vec3(1, 0, 0), Vec3(1, 0, 1), Vec3(0, 0, 1)};
        const double weight[3] = {1.0, wq, 1.0};
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i)
                s.ControlPoints.Get(i, j) = Vec4(ring[i].x(), j / 2.0, ring[i].z(), weight[i]);
        return s;
    }

    Surface MakePolynomialSurface()
    {
        Surface s;
        s.DegreeU = s.DegreeV = 2;
        s.KnotsU = s.KnotsV = KnotVector{{0, 0, 0, 1, 1, 1}};
        s.ControlPoints = ControlPointGrid(3, 3);
        const double a[3] = {0, 0, 1};   // u^2 Bernstein coefficients
        const double b[3] = {0, 0.5, 1}; // u   Bernstein coefficients
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i)
            {
                const double z = 0.2 * a[i] + 0.15 * b[i] * b[j] + 0.1 * a[j];
                s.ControlPoints.Get(i, j) = Vec4(i / 2.0, j / 2.0, z, 1.0);
            }
        return s;
    }

    // ----------------------------------------------------------------------
    // Shared checks.
    // ----------------------------------------------------------------------

    bool IsUnitClamped(const KnotVector& knots, int degree)
    {
        const auto& values = knots.Values();
        if (static_cast<int>(values.size()) < 2 * (degree + 1)) return false;
        for (int i = 0; i <= degree; ++i)
            if (values[i] != 0.0 || values[values.size() - 1 - i] != 1.0) return false;
        return true;
    }

    int MaxInteriorMultiplicity(const KnotVector& knots)
    {
        const auto& values = knots.Values();
        int worst = 0;
        for (int i = 0; i < static_cast<int>(values.size());)
        {
            const double value = values[i];
            int multiplicity = 0;
            while (i + multiplicity < static_cast<int>(values.size()) &&
                   values[i + multiplicity] == value)
                ++multiplicity;
            if (value > 0.0 && value < 1.0) worst = std::max(worst, multiplicity);
            i += multiplicity;
        }
        return worst;
    }

    /// Verify the published fit has every required structural property.
    bool CheckPublishedStructure(const Surface& fit, int max_control_points)
    {
        if (fit.DegreeU != 3 || fit.DegreeV != 3) return false;
        if (!IsUnitClamped(fit.KnotsU, 3) || !IsUnitClamped(fit.KnotsV, 3)) return false;
        if (MaxInteriorMultiplicity(fit.KnotsU) > 2) return false;
        if (MaxInteriorMultiplicity(fit.KnotsV) > 2) return false;
        if (fit.ControlPoints.UCount * fit.ControlPoints.VCount > max_control_points) return false;
        for (const auto& cp : fit.ControlPoints.Values)
        {
            if (cp.w() != 1.0) return false; // all weights exactly one
            if (!std::isfinite(cp.x()) || !std::isfinite(cp.y()) || !std::isfinite(cp.z()))
                return false;
        }
        return true;
    }

    /// Independent dense comparison against the analytic offset target.
    double DenseAnalyticError(AnalyticPoint (*ref)(double, double, double),
                              const Surface& fit, double distance, int grid = 48)
    {
        double worst = 0.0;
        for (int j = 0; j <= grid; ++j)
            for (int i = 0; i <= grid; ++i)
            {
                const double u = static_cast<double>(i) / grid;
                const double v = static_cast<double>(j) / grid;
                const Vec3 target = AnalyticTarget(ref, u, v, distance);
                worst = std::max(worst, (fit.Evaluate(u, v) - target).norm());
            }
        return worst;
    }

    /// Verify that every certification box covers its analytic samples and
    /// that the reported bounds are conservative; also verify partition
    /// coverage and disjoint interiors.
    bool CheckPartition(const OffsetCertification& cert)
    {
        if (cert.Status != OffsetCertStatus::certified) return false;
        if (cert.Boxes.empty()) return false;

        double area = 0.0;
        for (const auto& box : cert.Boxes)
        {
            if (!(box.CrossLowerBound > 0.0)) return false;
            if (!(box.ErrorUpperBound > 0.0) || !std::isfinite(box.ErrorUpperBound)) return false;
            if (!(box.Umin < box.Umax && box.Vmin < box.Vmax)) return false;
            area += (box.Umax - box.Umin) * (box.Vmax - box.Vmin);
        }
        if (std::abs(area - 1.0) > 1e-9) return false;

        for (size_t i = 0; i < cert.Boxes.size(); ++i)
            for (size_t k = i + 1; k < cert.Boxes.size(); ++k)
            {
                const auto& a = cert.Boxes[i];
                const auto& b = cert.Boxes[k];
                const double overlap_u = std::min(a.Umax, b.Umax) - std::max(a.Umin, b.Umin);
                const double overlap_v = std::min(a.Vmax, b.Vmax) - std::max(a.Vmin, b.Vmin);
                if (overlap_u > 1e-12 && overlap_v > 1e-12) return false; // shared edges only
            }
        return true;
    }

    /// Bounds must dominate an independent dense probe inside each box.
    bool CheckBoundsAgainstProbe(const Surface& source, const Surface& candidate,
                                 const OffsetCertification& cert, double distance)
    {
        for (const auto& box : cert.Boxes)
        {
            for (int j = 0; j <= 6; ++j)
                for (int i = 0; i <= 6; ++i)
                {
                    const double u = box.Umin + (box.Umax - box.Umin) * i / 6.0;
                    const double v = box.Vmin + (box.Vmax - box.Vmin) * j / 6.0;
                    const Grid<Vec3> jet = source.EvaluateAll(u, v, 1, 1);
                    const Vec3 cross = jet.Get(1, 0).cross(jet.Get(0, 1));
                    const Vec3 target = jet.Get(0, 0) + distance * cross.normalized();
                    const double error = (candidate.Evaluate(u, v) - target).norm();
                    // A small slack accounts for the probe's own round-off;
                    // the certified bound itself carries no such slack.
                    if (cross.norm() < box.CrossLowerBound * (1.0 - 1e-12)) return false;
                    if (error > box.ErrorUpperBound * (1.0 + 1e-12) + 1e-14) return false;
                }
        }
        return true;
    }

    Surface DeepCopy(const Surface& s)
    {
        Surface copy = s;
        copy.KnotsU = KnotVector{s.KnotsU.Values()};
        copy.KnotsV = KnotVector{s.KnotsV.Values()};
        copy.ControlPoints = ControlPointGrid(s.ControlPoints.UCount, s.ControlPoints.VCount);
        copy.ControlPoints.Values = s.ControlPoints.Values;
        return copy;
    }

    bool SurfacesEqual(const Surface& a, const Surface& b)
    {
        if (a.DegreeU != b.DegreeU || a.DegreeV != b.DegreeV) return false;
        if (a.KnotsU.Values() != b.KnotsU.Values()) return false;
        if (a.KnotsV.Values() != b.KnotsV.Values()) return false;
        if (a.ControlPoints.UCount != b.ControlPoints.UCount ||
            a.ControlPoints.VCount != b.ControlPoints.VCount) return false;
        for (size_t i = 0; i < a.ControlPoints.Values.size(); ++i)
            if ((a.ControlPoints.Values[i] - b.ControlPoints.Values[i]).norm() > 0.0) return false;
        return true;
    }

    // ======================================================================
    // certificates group
    // ======================================================================

    int CertPlane()
    {
        const Surface source = MakePlaneSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 1e-5;
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Success());
        TASK_CHECK(CheckPartition(fit.Certification));
        TASK_CHECK(CheckBoundsAgainstProbe(source, fit.Fit, fit.Certification, 0.1));
        TASK_CHECK(fit.Certification.WorstErrorUpperBound <= 1e-5);
        TASK_CHECK(DenseAnalyticError(PlaneReference, fit.Fit, 0.1) < 1e-5);
        return 0;
    }

    int CertCylinder()
    {
        const Surface source = MakeCylinderSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 1e-3;
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Success());
        TASK_CHECK(CheckPartition(fit.Certification));
        TASK_CHECK(CheckBoundsAgainstProbe(source, fit.Fit, fit.Certification, 0.1));
        TASK_CHECK(fit.Certification.WorstErrorUpperBound <= 1e-3);
        TASK_CHECK(DenseAnalyticError(CylinderReference, fit.Fit, 0.1) < 1e-3);
        return 0;
    }

    int CertPolynomial()
    {
        const Surface source = MakePolynomialSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 2e-3;
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Success());
        TASK_CHECK(CheckPartition(fit.Certification));
        TASK_CHECK(CheckBoundsAgainstProbe(source, fit.Fit, fit.Certification, 0.1));
        TASK_CHECK(fit.Certification.WorstErrorUpperBound <= 2e-3);
        TASK_CHECK(DenseAnalyticError(PolynomialReference, fit.Fit, 0.1) < 2e-3);
        return 0;
    }

    /// A localized bump whose peak sits between fit sample sites must never
    /// be certified: the continuous enclosure has to catch it.
    int CertRejectsBogusCandidate()
    {
        const Surface source = MakeCylinderSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 1e-3;
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Success());

        Surface bogus = DeepCopy(fit.Fit);
        // Smooth Gaussian bump centered away from any regular grid node.
        for (auto& cp : bogus.ControlPoints.Values)
        {
            const double bump = 0.02 * std::exp(-std::pow((cp.x() - 0.71) * 9.0, 2.0)
                                                 - std::pow((cp.y() - 0.37) * 9.0, 2.0));
            cp.z() += bump;
        }
        const OffsetCertification cert = CertifyNormalOffset(source, bogus, 0.1, 1e-3, 4096);
        TASK_CHECK(cert.Status != OffsetCertStatus::certified);

        // The bump genuinely exceeds the tolerance somewhere, confirming the
        // rejection is meaningful rather than a budget artifact.
        bool exceeds = false;
        for (int j = 1; j < 64 && !exceeds; ++j)
            for (int i = 1; i < 64 && !exceeds; ++i)
            {
                const double u = (i + 0.31) / 65.0;
                const double v = (j + 0.17) / 65.0;
                const Grid<Vec3> jet = source.EvaluateAll(u, v, 1, 1);
                const Vec3 target = jet.Get(0, 0) + 0.1 * jet.Get(1, 0).cross(jet.Get(0, 1)).normalized();
                if ((bogus.Evaluate(u, v) - target).norm() > 1e-3) exceeds = true;
            }
        TASK_CHECK(exceeds);
        return 0;
    }

    int CertLooseVsStrictTolerance()
    {
        const Surface source = MakeCylinderSurface();
        // Coarse 4x4 candidate produced by the first fit iteration.
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 1e-2; // loose but legal
        const OffsetFitResult loose = FitNormalOffset(source, options);
        TASK_CHECK(loose.Success());
        TASK_CHECK(loose.Certification.Status == OffsetCertStatus::certified);

        // The same coarse surface cannot meet the strictest legal tolerance.
        const OffsetCertification strict =
            CertifyNormalOffset(source, loose.Fit, 0.1, 1e-5, 4096);
        TASK_CHECK(strict.Status == OffsetCertStatus::unresolved);
        return 0;
    }

    int CertDoesNotMutateInputs()
    {
        const Surface source = MakeCylinderSurface();
        const Surface source_snapshot = DeepCopy(source);
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 1e-3;
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Success());
        const Surface candidate_snapshot = DeepCopy(fit.Fit);

        const OffsetCertification cert =
            CertifyNormalOffset(source, fit.Fit, 0.1, 1e-3, 256);
        TASK_CHECK(cert.Status == OffsetCertStatus::certified ||
                   cert.Status == OffsetCertStatus::unresolved);
        TASK_CHECK(SurfacesEqual(source, source_snapshot));
        TASK_CHECK(SurfacesEqual(fit.Fit, candidate_snapshot));
        return 0;
    }

    int CertInvalidInputs()
    {
        const Surface source = MakeCylinderSurface();
        const Surface good_candidate = MakePlaneSurface();

        TASK_CHECK(CertifyNormalOffset(source, good_candidate, 0.3, 1e-3, 4096).Status
                   == OffsetCertStatus::invalid_input);
        TASK_CHECK(CertifyNormalOffset(source, good_candidate, 0.1, 1e-9, 4096).Status
                   == OffsetCertStatus::invalid_input);
        TASK_CHECK(CertifyNormalOffset(source, good_candidate, 0.1, 1e-1, 4096).Status
                   == OffsetCertStatus::invalid_input);
        TASK_CHECK(CertifyNormalOffset(source, good_candidate, 0.1, 1e-3, 0).Status
                   == OffsetCertStatus::invalid_input);
        TASK_CHECK(CertifyNormalOffset(source, good_candidate,
                                       std::numeric_limits<double>::quiet_NaN(), 1e-3, 4096).Status
                   == OffsetCertStatus::invalid_input);

        Surface bad_weight = MakeCylinderSurface();
        bad_weight.ControlPoints.Get(1, 1).w() = 0.0;
        TASK_CHECK(CertifyNormalOffset(bad_weight, good_candidate, 0.1, 1e-3, 4096).Status
                   == OffsetCertStatus::invalid_input);

        Surface bad_domain = MakeCylinderSurface();
        bad_domain.KnotsU = KnotVector{{0, 0, 0, 0.5, 1, 1}}; // degree 2 with a 0-clamp only
        TASK_CHECK(CertifyNormalOffset(bad_domain, good_candidate, 0.1, 1e-3, 4096).Status
                   == OffsetCertStatus::invalid_input);
        return 0;
    }

    // ======================================================================
    // fitting group
    // ======================================================================

    int FitPlane()
    {
        const Surface source = MakePlaneSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 1e-5;
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Status == OffsetFitStatus::success);
        TASK_CHECK(CheckPublishedStructure(fit.Fit, options.max_control_points));
        TASK_CHECK(DenseAnalyticError(PlaneReference, fit.Fit, 0.1) < 1e-5);
        TASK_CHECK(!fit.Iterations.empty());
        return 0;
    }

    int FitCylinder()
    {
        const Surface source = MakeCylinderSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 1e-3;
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Status == OffsetFitStatus::success);
        TASK_CHECK(CheckPublishedStructure(fit.Fit, options.max_control_points));
        TASK_CHECK(DenseAnalyticError(CylinderReference, fit.Fit, 0.1) < 1e-3);
        return 0;
    }

    int FitPolynomial()
    {
        const Surface source = MakePolynomialSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 2e-3;
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Status == OffsetFitStatus::success);
        TASK_CHECK(CheckPublishedStructure(fit.Fit, options.max_control_points));
        TASK_CHECK(DenseAnalyticError(PolynomialReference, fit.Fit, 0.1) < 2e-3);
        return 0;
    }

    int FitNegativeDistance()
    {
        const Surface source = MakeCylinderSurface();
        OffsetOptions options;
        options.distance = -0.2;
        options.tolerance = 1e-3;
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Status == OffsetFitStatus::success);
        TASK_CHECK(CheckPublishedStructure(fit.Fit, options.max_control_points));
        TASK_CHECK(DenseAnalyticError(CylinderReference, fit.Fit, -0.2) < 1e-3);
        // The NURBS normal points toward the axis, so a negative distance
        // moves outward and grows the radius beyond 1.
        TASK_CHECK(fit.Fit.Evaluate(0.5, 0.5).norm() > 1.0);
        return 0;
    }

    int FitAsymmetricRefinement()
    {
        // The cylinder is extruded along V, so refinement must concentrate
        // on the angular U axis rather than growing both axes equally.
        const Surface source = MakeCylinderSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 1e-3;
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Success());
        TASK_CHECK(fit.Fit.ControlPoints.UCount > fit.Fit.ControlPoints.VCount);
        TASK_CHECK(fit.Fit.ControlPoints.VCount == 4); // single V span is enough
        return 0;
    }

    int FitIterationRecords()
    {
        const Surface source = MakeCylinderSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 1e-3;
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Success());
        TASK_CHECK(fit.Iterations.size() >= 2);
        for (const auto& record : fit.Iterations)
        {
            TASK_CHECK(record.KnotCountU >= 8 && record.KnotCountV >= 8);
            TASK_CHECK(record.ControlPointCount >= 16);
            TASK_CHECK(record.ControlPointCount <= options.max_control_points);
            TASK_CHECK(std::isfinite(record.SampleResidual));
            TASK_CHECK(record.BoxesUsed >= 1 && record.BoxesUsed <= options.max_boxes);
            TASK_CHECK(record.CrossLowerBound >= 0.0);
        }
        // Only the final record may be certified, and its continuous bound
        // must be no worse than its own sampling residual can explain.
        TASK_CHECK(fit.Iterations.back().Certified);
        TASK_CHECK(fit.Iterations.back().ContinuousBound <= options.tolerance);
        for (size_t k = 1; k < fit.Iterations.size(); ++k)
            TASK_CHECK(fit.Iterations[k].ControlPointCount >=
                       fit.Iterations[k - 1].ControlPointCount);
        return 0;
    }

    int FitSourceIsolation()
    {
        const Surface source = MakePolynomialSurface();
        const Surface snapshot = DeepCopy(source);
        OffsetOptions options;
        options.distance = 0.05;
        options.tolerance = 2e-3;
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Success());
        TASK_CHECK(SurfacesEqual(source, snapshot));
        return 0;
    }

    int FitSerializationAndKnotInsertion()
    {
        const Surface source = MakeCylinderSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 1e-3;
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Success());

        for (bool binary : {false, true})
        {
            std::stringstream stream;
            fit.Fit.SaveToFile(stream, binary);
            Surface restored;
            restored.LoadFromFile(stream);
            double worst = 0.0;
            for (int j = 0; j <= 16; ++j)
                for (int i = 0; i <= 16; ++i)
                {
                    const double u = i / 16.0;
                    const double v = j / 16.0;
                    worst = std::max(worst,
                                     (restored.Evaluate(u, v) - fit.Fit.Evaluate(u, v)).norm());
                }
            TASK_CHECK(worst < 1e-10);

            // Knot insertion on the restored surface must preserve geometry.
            const Surface inserted = restored.InsertKnotU(0.37).InsertKnotV(0.61);
            double insertion_error = 0.0;
            for (int j = 0; j <= 16; ++j)
                for (int i = 0; i <= 16; ++i)
                {
                    const double u = i / 16.0;
                    const double v = j / 16.0;
                    insertion_error = std::max(
                        insertion_error, (inserted.Evaluate(u, v) - fit.Fit.Evaluate(u, v)).norm());
                }
            TASK_CHECK(insertion_error < 1e-10);
        }
        return 0;
    }

    int FitTransformationInvariance()
    {
        const Surface source = MakePolynomialSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 2e-3;
        const OffsetFitResult base = FitNormalOffset(source, options);
        TASK_CHECK(base.Success());

        // Translation invariance: offsets translate with the surface.
        const Vec3 translation(0.7, -0.4, 0.25);
        Surface translated = DeepCopy(source);
        for (auto& cp : translated.ControlPoints.Values) cp.head<3>() += translation;
        const OffsetFitResult fit_t = FitNormalOffset(translated, options);
        TASK_CHECK(fit_t.Success());
        double translation_error = 0.0;
        for (int j = 0; j <= 12; ++j)
            for (int i = 0; i <= 12; ++i)
            {
                const double u = i / 12.0, v = j / 12.0;
                translation_error = std::max(
                    translation_error,
                    (fit_t.Fit.Evaluate(u, v) - base.Fit.Evaluate(u, v) - translation).norm());
            }
        TASK_CHECK(translation_error < 2e-3);

        // Proper rotation invariance.
        Mat3x3 rotation;
        const double angle = 0.6;
        rotation << std::cos(angle), 0.0, std::sin(angle),
                    0.0, 1.0, 0.0,
                    -std::sin(angle), 0.0, std::cos(angle);
        const Surface rotated = source.Transform(rotation);
        const OffsetFitResult fit_r = FitNormalOffset(rotated, options);
        TASK_CHECK(fit_r.Success());
        double rotation_error = 0.0;
        for (int j = 0; j <= 12; ++j)
            for (int i = 0; i <= 12; ++i)
            {
                const double u = i / 12.0, v = j / 12.0;
                rotation_error = std::max(
                    rotation_error,
                    (fit_r.Fit.Evaluate(u, v) - rotation * base.Fit.Evaluate(u, v)).norm());
            }
        TASK_CHECK(rotation_error < 2e-3);

        // Uniform positive weight scaling leaves the geometry unchanged and
        // must lead to an equivalent fit.
        Surface rescaled = DeepCopy(source);
        for (auto& cp : rescaled.ControlPoints.Values) cp.w() *= 2.5;
        const OffsetFitResult fit_w = FitNormalOffset(rescaled, options);
        TASK_CHECK(fit_w.Success());
        double weight_error = 0.0;
        for (int j = 0; j <= 12; ++j)
            for (int i = 0; i <= 12; ++i)
            {
                const double u = i / 12.0, v = j / 12.0;
                weight_error = std::max(
                    weight_error, (fit_w.Fit.Evaluate(u, v) - base.Fit.Evaluate(u, v)).norm());
            }
        TASK_CHECK(weight_error < 2e-3);
        return 0;
    }

    int FitSwapAndSignInvariance()
    {
        const Surface source = MakePolynomialSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 2e-3;
        const OffsetFitResult base = FitNormalOffset(source, options);
        TASK_CHECK(base.Success());

        // Swap U/V: normal flips, so distance flips sign too.
        Surface swapped;
        swapped.DegreeU = source.DegreeV;
        swapped.DegreeV = source.DegreeU;
        swapped.KnotsU = KnotVector{source.KnotsV.Values()};
        swapped.KnotsV = KnotVector{source.KnotsU.Values()};
        swapped.ControlPoints = ControlPointGrid(source.ControlPoints.VCount,
                                                 source.ControlPoints.UCount);
        for (int j = 0; j < source.ControlPoints.VCount; ++j)
            for (int i = 0; i < source.ControlPoints.UCount; ++i)
                swapped.ControlPoints.Get(j, i) = source.ControlPoints.Get(i, j);

        OffsetOptions swapped_options = options;
        swapped_options.distance = -options.distance;
        const OffsetFitResult fit_s = FitNormalOffset(swapped, swapped_options);
        TASK_CHECK(fit_s.Success());
        double swap_error = 0.0;
        for (int j = 0; j <= 12; ++j)
            for (int i = 0; i <= 12; ++i)
            {
                const double u = i / 12.0, v = j / 12.0;
                swap_error = std::max(
                    swap_error,
                    (fit_s.Fit.Evaluate(v, u) - base.Fit.Evaluate(u, v)).norm());
            }
        TASK_CHECK(swap_error < 2e-3);
        return 0;
    }

    // ======================================================================
    // budgets group
    // ======================================================================

    int BudgetControlPoints()
    {
        const Surface source = MakeCylinderSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 1e-3;
        options.max_control_points = 16; // only the coarsest 4x4 net fits
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Status == OffsetFitStatus::budget_exhausted);
        // No half-finished surface is published on failure.
        TASK_CHECK(fit.Fit.DegreeU == INVALID_DEGREE);
        TASK_CHECK(fit.Fit.ControlPoints.Values.empty());
        for (const auto& record : fit.Iterations)
            TASK_CHECK(record.ControlPointCount <= 16);
        return 0;
    }

    int BudgetBoxes()
    {
        const Surface source = MakeCylinderSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 1e-3;
        options.max_boxes = 2; // far too small to close the curved proof
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Status == OffsetFitStatus::budget_exhausted);
        TASK_CHECK(fit.Fit.DegreeU == INVALID_DEGREE);
        TASK_CHECK(fit.Certification.Status == OffsetCertStatus::unresolved);
        return 0;
    }

    int BudgetInvalid()
    {
        const Surface source = MakeCylinderSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 1e-3;

        options.max_control_points = 15; // below the coarsest bicubic net
        TASK_CHECK(FitNormalOffset(source, options).Status == OffsetFitStatus::invalid_input);
        options.max_control_points = 256;

        options.max_boxes = 0;
        TASK_CHECK(FitNormalOffset(source, options).Status == OffsetFitStatus::invalid_input);
        options.max_boxes = 4096;

        options.distance = 0.3;
        TASK_CHECK(FitNormalOffset(source, options).Status == OffsetFitStatus::invalid_input);
        options.distance = 0.1;

        options.tolerance = 5e-6;
        TASK_CHECK(FitNormalOffset(source, options).Status == OffsetFitStatus::invalid_input);

        Surface bad = MakeCylinderSurface();
        bad.ControlPoints.Get(0, 0).w() = -1.0;
        TASK_CHECK(FitNormalOffset(bad, options).Status == OffsetFitStatus::invalid_input);
        return 0;
    }

    int BudgetNeverExceeded()
    {
        const Surface source = MakeCylinderSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 1e-3;
        options.max_control_points = 30; // allows a few refinements only
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Status == OffsetFitStatus::budget_exhausted ||
                   fit.Status == OffsetFitStatus::success);
        for (const auto& record : fit.Iterations)
        {
            TASK_CHECK(record.ControlPointCount <= options.max_control_points);
            TASK_CHECK(record.BoxesUsed <= options.max_boxes);
        }
        if (fit.Success())
            TASK_CHECK(fit.Fit.ControlPoints.UCount * fit.Fit.ControlPoints.VCount
                       <= options.max_control_points);
        return 0;
    }

    int BudgetNoStaleEvidence()
    {
        // Each iteration's bound must correspond to that iteration's own
        // surface: the continuous bound is finite only when a real check ran,
        // and a successful final iteration never reports an earlier residual.
        const Surface source = MakeCylinderSurface();
        OffsetOptions options;
        options.distance = 0.1;
        options.tolerance = 1e-3;
        const OffsetFitResult fit = FitNormalOffset(source, options);
        TASK_CHECK(fit.Success());
        for (const auto& record : fit.Iterations)
        {
            TASK_CHECK(std::isfinite(record.ContinuousBound));
            TASK_CHECK(record.SampleResidual >= 0.0);
        }
        const auto& last = fit.Iterations.back();
        TASK_CHECK(last.Certified);
        TASK_CHECK(std::abs(last.ContinuousBound -
                            fit.Certification.WorstErrorUpperBound) <= 1e-12);
        return 0;
    }
}

extern "C" const struct task_case task_cases[] = {
    {"certificates", "plane_partition_and_bounds", CertPlane},
    {"certificates", "cylinder_partition_and_bounds", CertCylinder},
    {"certificates", "polynomial_partition_and_bounds", CertPolynomial},
    {"certificates", "reject_bump_between_samples", CertRejectsBogusCandidate},
    {"certificates", "loose_vs_strict_tolerance", CertLooseVsStrictTolerance},
    {"certificates", "certification_input_isolation", CertDoesNotMutateInputs},
    {"certificates", "invalid_inputs_rejected", CertInvalidInputs},

    {"fitting", "plane_offset", FitPlane},
    {"fitting", "cylinder_offset", FitCylinder},
    {"fitting", "polynomial_offset", FitPolynomial},
    {"fitting", "negative_distance", FitNegativeDistance},
    {"fitting", "asymmetric_refinement", FitAsymmetricRefinement},
    {"fitting", "iteration_records", FitIterationRecords},
    {"fitting", "source_isolation", FitSourceIsolation},
    {"fitting", "serialization_and_knot_insertion", FitSerializationAndKnotInsertion},
    {"fitting", "transformation_invariance", FitTransformationInvariance},
    {"fitting", "swap_axes_and_flip_sign", FitSwapAndSignInvariance},

    {"budgets", "control_point_budget", BudgetControlPoints},
    {"budgets", "leaf_box_budget", BudgetBoxes},
    {"budgets", "invalid_budgets_rejected", BudgetInvalid},
    {"budgets", "budgets_never_exceeded", BudgetNeverExceeded},
    {"budgets", "no_stale_evidence", BudgetNoStaleEvidence},
};

extern "C" const size_t task_case_count = 22;
