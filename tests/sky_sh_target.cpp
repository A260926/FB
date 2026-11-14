//
// tests/sky_sh_target.cpp
//
// Target tests for the 7-band spherical-harmonic sky diffuse path.
//
// Three groups, registered through task_support/check.h:
//
//   projection  -- full-sphere Hosek projection, coloured ground, snapshot
//                  semantics, non-axisymmetric coefficients. Reference values
//                  come from an independent Simpson/trapezoidal quadrature and
//                  an independently written SH basis, never from another
//                  production entry point.
//
//   convolution -- cosine-power kernel weights checked against closed-form
//                  constants and independent numerical integration, plus
//                  first/second-order band-limited light fields at r = 0,
//                  r = 1 and an intermediate roughness.
//
//   shader      -- a GPU-free C++ adaptation that really includes
//                  sky_sh_basis.inc through its GLSL branch, proving the
//                  shader rebuilds the signal with the same indices and
//                  consumes the pre-convolved texture coefficients directly.
//

#include <cstdint>
#include <cstdio>
#include <cmath>

#include "SunSkySH.hpp"
#include "sky_sh_basis.inc"

#include "../task_support/check.h"

namespace
{
    using SSLib::SkySH;
    using SSLib::SkyHosek;
    using SSLib::kSkySHCoeffCount;
    using SSLib::kSkySHBands;

    const float  kPi   = 3.14159265358979323846f;
    const double kPiD  = 3.14159265358979323846;

    bool Close(float a, float b, float tolerance)
    {
        return std::fabs(a - b) <= tolerance;
    }

    bool CloseV(const Vec3f& a, const Vec3f& b, float tolerance)
    {
        return Close(a.x, b.x, tolerance)
            && Close(a.y, b.y, tolerance)
            && Close(a.z, b.z, tolerance);
    }

    //----------------------------------------------------------------------
    // Independent reference SH basis (doubles, factorial normalisation).
    // Written separately from sky_sh_basis.inc.
    //----------------------------------------------------------------------

    double Factorial(int n)
    {
        double result = 1.0;
        for (int k = 2; k <= n; ++k)
            result *= k;
        return result;
    }

    int RefIndex(int l, int m)
    {
        return l * l + l + m;
    }

    // All 49 real SH basis functions for unit direction (x, y, z).
    void ReferenceBasis(double x, double y, double z, double out[kSkySHCoeffCount])
    {
        for (int i = 0; i < kSkySHCoeffCount; ++i)
            out[i] = 0.0;

        if (z > 1.0) z = 1.0;
        if (z < -1.0) z = -1.0;

        const double phi = std::atan2(y, x);
        const double sinTheta = std::sqrt(std::max(0.0, 1.0 - z * z));

        for (int l = 0; l < kSkySHBands; ++l)
        {
            for (int m = 0; m <= l; ++m)
            {
                // Associated Legendre P_l^m with Condon-Shortley phase.
                double pmm = 1.0;
                if (m > 0)
                {
                    double somx2 = sinTheta;
                    double fact = 1.0;
                    for (int i = 1; i <= m; ++i)
                    {
                        pmm *= -fact * somx2;
                        fact += 2.0;
                    }
                }

                double plm;

                if (l == m)
                {
                    plm = pmm;
                }
                else
                {
                    double pPrev = pmm;
                    double pCur = double(2 * m + 1) * z * pmm;

                    if (l == m + 1)
                    {
                        plm = pCur;
                    }
                    else
                    {
                        for (int ll = m + 2; ll <= l; ++ll)
                        {
                            double pNext =
                                (double(2 * ll - 1) * z * pCur - double(ll + m - 1) * pPrev)
                                / double(ll - m);
                            pPrev = pCur;
                            pCur = pNext;
                        }
                        plm = pCur;
                    }
                }

                const double normalizer =
                    std::sqrt(double(2 * l + 1) / (4.0 * kPiD)
                              * Factorial(l - m) / Factorial(l + m));

                if (m == 0)
                {
                    out[RefIndex(l, 0)] = normalizer * plm;
                }
                else
                {
                    const double scale = std::sqrt(2.0) * normalizer * plm;
                    out[RefIndex(l,  m)] = scale * std::cos(double(m) * phi);
                    out[RefIndex(l, -m)] = scale * std::sin(double(m) * phi);
                }
            }
        }
    }

    //----------------------------------------------------------------------
    // Independent reference projection. The upper hemisphere is integrated
    // with a composite Simpson rule in z (Newton-Cotes, deliberately
    // different from the per-hemisphere Gauss-Legendre product rule used in
    // production) plus a periodic trapezoidal rule in phi; the constant lower
    // hemisphere is added through its exact closed-form SH integral.
    //----------------------------------------------------------------------

    const int kRefZIntervals = 480;   // even
    const int kRefPhiSamples = 720;

    double LegendreP(int l, double x);   // defined below

    // Exact SH integral of a constant radiance g over the lower hemisphere
    // for the m = 0 basis function of band l. A constant has no m != 0 modes.
    //   int_{z < 0} Y_l0 dOmega
    //     = 2 pi N(l,0) * int_{-1}^0 P_l(t) dt,
    //   int P_l dt = (P_{l+1} - P_{l-1}) / (2l+1),  P_k(-1) = (-1)^k.
    double AnalyticLowerIntegral(int l)
    {
        const double pAtZeroNext = LegendreP(l + 1, 0.0);
        const double pAtZeroPrev = l >= 1 ? LegendreP(l - 1, 0.0) : 0.0;
        const double pAtMinusOneNext = ((l + 1) % 2 == 0) ? 1.0 : -1.0;
        const double pAtMinusOnePrev =
            (l == 0) ? 0.0 : (((l - 1) % 2 == 0) ? 1.0 : -1.0);

        const double integralPl =
            ((pAtZeroNext - pAtZeroPrev) - (pAtMinusOneNext - pAtMinusOnePrev))
            / double(2 * l + 1);

        const double normalizer = std::sqrt(double(2 * l + 1) / (4.0 * kPiD));

        return 2.0 * kPiD * normalizer * integralPl;
    }

    void ReferenceProject
    (
        const SkyHosek& sky,
        const Vec3f& groundAlbedoRGB,
        double refCoeffs[kSkySHCoeffCount][3],
        double refEsky[3]
    )
    {
        const double phiStep = 2.0 * kPiD / kRefPhiSamples;
        const double h = 1.0 / kRefZIntervals;

        refEsky[0] = refEsky[1] = refEsky[2] = 0.0;

        for (int i = 0; i < kSkySHCoeffCount; ++i)
            for (int c = 0; c < 3; ++c)
                refCoeffs[i][c] = 0.0;

        // Composite Simpson weight for node k of the [0, 1] rule.
        auto simpsonWeight = [&](int k) -> double
        {
            double w = h / 3.0;
            if (k > 0 && k < kRefZIntervals)
                w *= (k % 2 == 1) ? 4.0 : 2.0;
            return w;
        };

        // Upper hemisphere: z in [0, 1], equator included once.
        for (int k = 0; k <= kRefZIntervals; ++k)
        {
            const double zWeight = simpsonWeight(k);
            const double z = k * h;
            const double sinTheta = std::sqrt(std::max(0.0, 1.0 - z * z));

            for (int j = 0; j < kRefPhiSamples; ++j)
            {
                const double phi = j * phiStep;
                const double w = zWeight * phiStep;

                const float fx = float(sinTheta * std::cos(phi));
                const float fy = float(sinTheta * std::sin(phi));
                const float fz = float(z);

                const Vec3f radiance = sky.SkyRGB(Vec3f(fx, fy, fz));

                double basis[kSkySHCoeffCount];
                ReferenceBasis(fx, fy, fz, basis);

                for (int i = 0; i < kSkySHCoeffCount; ++i)
                {
                    const double b = basis[i] * w;
                    refCoeffs[i][0] += double(radiance.x) * b;
                    refCoeffs[i][1] += double(radiance.y) * b;
                    refCoeffs[i][2] += double(radiance.z) * b;
                }

                if (z > 0.0)
                {
                    refEsky[0] += double(radiance.x) * z * w;
                    refEsky[1] += double(radiance.y) * z * w;
                    refEsky[2] += double(radiance.z) * z * w;
                }
            }
        }

        // The lower hemisphere is a constant per channel, so its projection
        // has a closed form: a constant carries only m = 0 modes. This is a
        // genuinely different computation from production's sampled lower
        // hemisphere, and it makes no discretisation error of its own. The
        // strict z < 0 integral does not touch the measure-zero equator that
        // the upper-hemisphere rule already included.
        const double ground[3] =
        {
            groundAlbedoRGB.x * refEsky[0] / kPiD,
            groundAlbedoRGB.y * refEsky[1] / kPiD,
            groundAlbedoRGB.z * refEsky[2] / kPiD
        };

        for (int l = 0; l < kSkySHBands; ++l)
        {
            const double integral = AnalyticLowerIntegral(l);
            const int idx = RefIndex(l, 0);

            refCoeffs[idx][0] += ground[0] * integral;
            refCoeffs[idx][1] += ground[1] * integral;
            refCoeffs[idx][2] += ground[2] * integral;
        }
    }

    // Per-spec tolerance: max(1, max |reference RGB coefficient|) * 2e-4.
    float ProjectionTolerance(const double refCoeffs[kSkySHCoeffCount][3])
    {
        double maxAbs = 1.0;
        for (int i = 0; i < kSkySHCoeffCount; ++i)
            for (int c = 0; c < 3; ++c)
                maxAbs = std::max(maxAbs, std::fabs(refCoeffs[i][c]));

        return float(maxAbs * 2e-4);
    }

    // Legendre P_l used by the independent kernel integration.
    double LegendreP(int l, double x)
    {
        double pPrev = 1.0;
        double pCur = x;

        if (l == 0) return pPrev;

        for (int k = 2; k <= l; ++k)
        {
            double pNext = (double(2 * k - 1) * x * pCur - double(k - 1) * pPrev) / double(k);
            pPrev = pCur;
            pCur = pNext;
        }

        return pCur;
    }

    // Independent composite-Simpson value of (n+1) * int_0^1 t^n P_l(t) dt.
    double ReferenceKernelWeight(int l, double n)
    {
        const int intervals = 2000;
        const double h = 1.0 / intervals;
        double sum = 0.0;

        for (int k = 0; k <= intervals; ++k)
        {
            const double t = k * h;
            const double f = std::pow(t, n) * LegendreP(l, t);
            double s = 1.0;
            if (k > 0 && k < intervals)
                s = (k % 2 == 1) ? 4.0 : 2.0;
            sum += s * f;
        }

        return (n + 1.0) * sum * h / 3.0;
    }
}

//--------------------------------------------------------------------------
// projection group
//--------------------------------------------------------------------------

static int CheckProjectionAgainstReference
(
    const Vec3f& sun,
    const Vec3f& albedo,
    float turbidity,
    float overcast
)
{
    SkyHosek sky;
    sky.Update(sun, turbidity, albedo, overcast);

    SkySH sh;
    sh.Project(sky, albedo);

    double refCoeffs[kSkySHCoeffCount][3];
    double refEsky[3];
    ReferenceProject(sky, albedo, refCoeffs, refEsky);

    const float tolerance = ProjectionTolerance(refCoeffs);
    const Vec3f* coeffs = sh.Coefficients();

    for (int i = 0; i < kSkySHCoeffCount; ++i)
    {
        TASK_CHECK(Close(coeffs[i].x, float(refCoeffs[i][0]), tolerance));
        TASK_CHECK(Close(coeffs[i].y, float(refCoeffs[i][1]), tolerance));
        TASK_CHECK(Close(coeffs[i].z, float(refCoeffs[i][2]), tolerance));
    }

    // Esky snapshot agrees with the independent irradiance quadrature.
    const Vec3f esky = sh.SkyIrradiance();
    const float eskyTol = float(std::max(1.0,
        std::max(std::fabs(refEsky[0]), std::max(std::fabs(refEsky[1]), std::fabs(refEsky[2]))))
        * 2e-4f);

    TASK_CHECK(Close(esky.x, float(refEsky[0]), eskyTol));
    TASK_CHECK(Close(esky.y, float(refEsky[1]), eskyTol));
    TASK_CHECK(Close(esky.z, float(refEsky[2]), eskyTol));

    return 0;
}

static int ProjectionOffAxisSun()
{
    // Sun at azimuth 0, coloured linear-RGB ground albedo.
    return CheckProjectionAgainstReference
    (
        norm(Vec3f(0.6f, 0.0f, 0.8f)),
        Vec3f(0.7f, 0.1f, 0.25f),
        3.0f, 0.0f
    );
}

static int ProjectionRotatedSun()
{
    // Same elevation, azimuth rotated 90 degrees, different coloured ground.
    return CheckProjectionAgainstReference
    (
        norm(Vec3f(0.0f, 0.6f, 0.8f)),
        Vec3f(0.8f, 0.2f, 0.05f),
        4.0f, 0.1f
    );
}

static int ProjectionOverheadSunIsAxisymmetric()
{
    const Vec3f sun(0.0f, 0.0f, 1.0f);
    const Vec3f albedo(0.2f, 0.3f, 0.4f);

    SkyHosek sky;
    sky.Update(sun, 3.0f, albedo, 0.0f);

    SkySH sh;
    sh.Project(sky, albedo);

    const Vec3f* c = sh.Coefficients();

    // With the sun on +Z the radiance is axisymmetric: every m != 0
    // coefficient must vanish up to quadrature noise.
    for (int l = 1; l < kSkySHBands; ++l)
        for (int m = -l; m <= l; ++m)
            if (m != 0)
            {
                const Vec3f& v = c[SkySHBasis::Index(l, m)];
                TASK_CHECK(std::fabs(v.x) < 1e-2f);
                TASK_CHECK(std::fabs(v.y) < 1e-2f);
                TASK_CHECK(std::fabs(v.z) < 1e-2f);
            }

    // And the same projection still matches the independent reference.
    double refCoeffs[kSkySHCoeffCount][3];
    double refEsky[3];
    ReferenceProject(sky, albedo, refCoeffs, refEsky);
    const float tolerance = ProjectionTolerance(refCoeffs);

    for (int i = 0; i < kSkySHCoeffCount; ++i)
        TASK_CHECK(CloseV(c[i],
            Vec3f(float(refCoeffs[i][0]), float(refCoeffs[i][1]), float(refCoeffs[i][2])),
            tolerance));

    return 0;
}

static int ProjectionNonAxialCoefficientsPresent()
{
    SkyHosek sky;
    sky.Update(norm(Vec3f(0.6f, 0.0f, 0.8f)), 3.0f, Vec3f(0.1f, 0.1f, 0.1f), 0.0f);

    SkySH sh;
    sh.Project(sky, Vec3f(0.1f, 0.1f, 0.1f));

    const Vec3f* c = sh.Coefficients();

    // The theta/gamma product leaves clear non-axisymmetric energy: the
    // l=1, m=+/-1 pair and an l=2, m=+/-1 pair must be substantially nonzero.
    const Vec3f c11 = c[SkySHBasis::Index(1, 1)];
    const Vec3f c1m = c[SkySHBasis::Index(1, -1)];
    const Vec3f c21 = c[SkySHBasis::Index(2, 1)];

    TASK_CHECK(std::fabs(c11.x) + std::fabs(c1m.x) > 100.0f);
    TASK_CHECK(std::fabs(c21.y) > 10.0f);

    return 0;
}

static int ProjectionAzimuthRotationRelation()
{
    // Rotating the sun azimuth by +90 degrees is equivalent to rotating the
    // field by -90 degrees, which mixes each +/-m pair in a known way.
    SkyHosek skyA;
    skyA.Update(norm(Vec3f(0.6f, 0.0f, 0.8f)), 3.0f, Vec3f(0.2f, 0.2f, 0.2f), 0.0f);

    SkyHosek skyB;
    skyB.Update(norm(Vec3f(0.0f, 0.6f, 0.8f)), 3.0f, Vec3f(0.2f, 0.2f, 0.2f), 0.0f);

    SkySH shA, shB;
    shA.Project(skyA, Vec3f(0.2f, 0.2f, 0.2f));
    shB.Project(skyB, Vec3f(0.2f, 0.2f, 0.2f));

    const Vec3f* a = shA.Coefficients();
    const Vec3f* b = shB.Coefficients();

    for (int l = 0; l < kSkySHBands; ++l)
    {
        // m = 0 coefficients are unchanged by a z-axis rotation.
        const Vec3f d0 = b[SkySHBasis::Index(l, 0)] - a[SkySHBasis::Index(l, 0)];
        TASK_CHECK(sqrlen(d0) < 1e-12f * (sqrlen(a[SkySHBasis::Index(l, 0)]) + 1.0f));

        for (int m = 1; m <= l; ++m)
        {
            const float angle = float(m) * kPi * 0.5f;
            const Vec3f ap = a[SkySHBasis::Index(l,  m)];
            const Vec3f am = a[SkySHBasis::Index(l, -m)];

            const Vec3f expectP = ap * std::cos(angle) - am * std::sin(angle);
            const Vec3f expectM = ap * std::sin(angle) + am * std::cos(angle);

            const float scale = std::sqrt(sqrlen(ap) + sqrlen(am)) + 1.0f;

            TASK_CHECK(sqrlen(b[SkySHBasis::Index(l,  m)] - expectP) < 1e-8f * scale * scale);
            TASK_CHECK(sqrlen(b[SkySHBasis::Index(l, -m)] - expectM) < 1e-8f * scale * scale);
        }
    }

    return 0;
}

static int ProjectionColouredGroundIsPerChannel()
{
    // Difference between a coloured-ground and a black-ground projection of
    // the same sky must equal the analytic lower-hemisphere SH integral of
    // the constant ground radiance, independently per RGB channel.
    SkyHosek sky;
    const Vec3f albedo(0.7f, 0.1f, 0.25f);
    sky.Update(norm(Vec3f(0.6f, 0.0f, 0.8f)), 3.0f, albedo, 0.0f);

    SkySH shColour, shBlack;
    shColour.Project(sky, albedo);
    shBlack.Project(sky, Vec3f(vl_0));

    const Vec3f esky = shColour.SkyIrradiance();
    const Vec3f groundRadiance = Vec3f
    (
        albedo.x * esky.x / kPi,
        albedo.y * esky.y / kPi,
        albedo.z * esky.z / kPi
    );

    const Vec3f* colour = shColour.Coefficients();
    const Vec3f* black = shBlack.Coefficients();

    for (int l = 0; l < kSkySHBands; ++l)
    {
        // The coloured-ground minus black-ground difference must equal the
        // exact lower-hemisphere SH integral of the constant ground radiance,
        // independently per RGB channel.
        const double lowerIntegral = AnalyticLowerIntegral(l);

        const Vec3f expected
        (
            float(groundRadiance.x * lowerIntegral),
            float(groundRadiance.y * lowerIntegral),
            float(groundRadiance.z * lowerIntegral)
        );

        const Vec3f delta = colour[SkySHBasis::Index(l, 0)]
                          - black[SkySHBasis::Index(l, 0)];

        const float tol = 1e-3f * (std::sqrt(sqrlen(expected)) + 1.0f);
        TASK_CHECK(sqrlen(delta - expected) < tol * tol);

        // A constant lower hemisphere has no m != 0 modes. The two float32
        // projections only differ there through cancellation rounding, so use
        // a tolerance scaled to the coefficient magnitude.
        for (int m = -l; m <= l; ++m)
            if (m != 0)
            {
                const int idx = SkySHBasis::Index(l, m);
                const Vec3f d = colour[idx] - black[idx];
                const float scale = std::sqrt(sqrlen(colour[idx]))
                                  + std::sqrt(sqrlen(black[idx])) + 1.0f;
                TASK_CHECK(std::sqrt(sqrlen(d)) < 1e-5f * scale);
            }
    }

    return 0;
}

static int ProjectionSnapshotAndReproject()
{
    SkyHosek sky;
    sky.Update(norm(Vec3f(0.6f, 0.0f, 0.8f)), 3.0f, Vec3f(0.3f, 0.3f, 0.3f), 0.0f);

    SkySH sh;
    sh.Project(sky, Vec3f(0.3f, 0.3f, 0.3f));

    Vec3f snapshot[kSkySHCoeffCount];
    for (int i = 0; i < kSkySHCoeffCount; ++i)
        snapshot[i] = sh.Coefficients()[i];

    // Mutating the model after Project must not touch the snapshot.
    sky.Update(norm(Vec3f(0.0f, 0.2f, 0.98f)), 8.0f, Vec3f(0.9f, 0.9f, 0.9f), 0.8f);

    for (int i = 0; i < kSkySHCoeffCount; ++i)
        TASK_CHECK(sh.Coefficients()[i] == snapshot[i]);

    // Re-project with a black-ground, near-overhead sky: every result is
    // replaced, and coefficients that were nonzero can now vanish.
    sh.Project(sky, Vec3f(vl_0));

    double refCoeffs[kSkySHCoeffCount][3];
    double refEsky[3];
    ReferenceProject(sky, Vec3f(vl_0), refCoeffs, refEsky);
    const float tolerance = ProjectionTolerance(refCoeffs);

    bool anyChanged = false;

    for (int i = 0; i < kSkySHCoeffCount; ++i)
    {
        const Vec3f expected
        (
            static_cast<float>(refCoeffs[i][0]),
            static_cast<float>(refCoeffs[i][1]),
            static_cast<float>(refCoeffs[i][2])
        );
        TASK_CHECK(CloseV(sh.Coefficients()[i], expected, tolerance));
        if (sh.Coefficients()[i] != snapshot[i])
            anyChanged = true;
    }

    TASK_CHECK(anyChanged);

    return 0;
}

static int ProjectionPreservesHDRAndSign()
{
    // LoadCoefficients keeps arbitrary HDR and negative values untouched.
    Vec3f loaded[kSkySHCoeffCount];

    for (int i = 0; i < kSkySHCoeffCount; ++i)
        loaded[i] = Vec3f(-1.0e6f + float(i) * 3.0e4f, 2.0e5f, -7.0f);

    SkySH sh;
    sh.LoadCoefficients(loaded);

    for (int i = 0; i < kSkySHCoeffCount; ++i)
        TASK_CHECK(sh.Coefficients()[i] == loaded[i]);

    const Vec3f v = sh.Evaluate(norm(Vec3f(0.3f, -0.5f, 0.8f)), 0.0f);
    TASK_CHECK(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z));

    return 0;
}

//--------------------------------------------------------------------------
// convolution group
//--------------------------------------------------------------------------

static int ConvolutionZeroRoughnessIsIdentity()
{
    for (int l = 0; l < kSkySHBands; ++l)
        TASK_CHECK(Close(SkySHBasis::KernelWeight(l, 0.0f), 1.0f, 1e-7f));

    // r slightly above 1 clamps to the r = 1 kernel.
    for (int l = 0; l < kSkySHBands; ++l)
        TASK_CHECK(Close(SkySHBasis::KernelWeight(l, 1.0f),
                         SkySHBasis::KernelWeight(l, 1.7f), 1e-7f));

    return 0;
}

static int ConvolutionRoughnessOneClosedForm()
{
    // n = 2 / 1^2 - 1 = 1. Closed-form weights:
    //   l=0: 1, l=1: 2/3, l=2: 1/4, l=3: 0,
    //   l=4: -1/24, l=5: 0, l=6: 1/64.
    const float expected[kSkySHBands] =
    {
        1.0f, 2.0f / 3.0f, 0.25f, 0.0f, -1.0f / 24.0f, 0.0f, 1.0f / 64.0f
    };

    for (int l = 0; l < kSkySHBands; ++l)
        TASK_CHECK(Close(SkySHBasis::KernelWeight(l, 1.0f), expected[l], 1e-6f));

    return 0;
}

static int ConvolutionIntermediateMatchesIntegral()
{
    // r = 0.5 -> n = 7. Cross-check every band against an independent
    // numerical integration of t^n P_l(t).
    const float r = 0.5f;
    const double n = 2.0 / double(r * r) - 1.0;
    TASK_CHECK(Close(float(n), 7.0f, 1e-6f));

    for (int l = 0; l < kSkySHBands; ++l)
    {
        const double reference = ReferenceKernelWeight(l, n);
        TASK_CHECK(Close(SkySHBasis::KernelWeight(l, r), float(reference), 2e-5f));
    }

    // Same for a second interior roughness.
    const float r2 = 0.25f;
    const double n2 = 2.0 / double(r2 * r2) - 1.0;

    for (int l = 0; l < kSkySHBands; ++l)
    {
        const double reference = ReferenceKernelWeight(l, n2);
        TASK_CHECK(Close(SkySHBasis::KernelWeight(l, r2), float(reference), 2e-5f));
    }

    return 0;
}

static int ConvolutionFirstOrderLightField()
{
    // Pure l = 1 field with deliberately mixed-sign coefficients.
    Vec3f coeffs[kSkySHCoeffCount];
    for (int i = 0; i < kSkySHCoeffCount; ++i)
        coeffs[i] = Vec3f(vl_0);

    coeffs[SkySHBasis::Index(1,  0)] = Vec3f( 12.0f, -4.0f, 7.0f);
    coeffs[SkySHBasis::Index(1,  1)] = Vec3f(-8.0f,  9.0f, 2.0f);
    coeffs[SkySHBasis::Index(1, -1)] = Vec3f( 5.0f, -6.0f, 3.0f);

    SkySH sh;
    sh.LoadCoefficients(coeffs);

    const Vec3f dirs[] =
    {
        norm(Vec3f(0.3f, -0.5f, 0.8f)),
        norm(Vec3f(-0.9f, 0.2f, 0.4f)),
        norm(Vec3f(0.1f, 0.7f, -0.7f))
    };

    const float rough[] = { 0.0f, 0.5f, 1.0f };

    for (const Vec3f& d : dirs)
    {
        double basis[kSkySHCoeffCount];
        ReferenceBasis(d.x, d.y, d.z, basis);

        Vec3f raw(vl_0);
        for (int m = -1; m <= 1; ++m)
            raw += coeffs[SkySHBasis::Index(1, m)]
                 * float(basis[SkySHBasis::Index(1, m)]);

        for (float r : rough)
        {
            const float w = SkySHBasis::KernelWeight(1, r);
            const Vec3f expected = raw * w;
            TASK_CHECK(CloseV(sh.Evaluate(d, r), expected, 1e-4f * (std::sqrt(sqrlen(expected)) + 1.0f)));
        }
    }

    return 0;
}

static int ConvolutionSecondOrderLightField()
{
    // Pure l = 2 field, including the negative r = 1 band weight.
    Vec3f coeffs[kSkySHCoeffCount];
    for (int i = 0; i < kSkySHCoeffCount; ++i)
        coeffs[i] = Vec3f(vl_0);

    coeffs[SkySHBasis::Index(2, -2)] = Vec3f( 3.0f, -2.0f, 5.0f);
    coeffs[SkySHBasis::Index(2, -1)] = Vec3f(-4.0f,  6.0f, 1.0f);
    coeffs[SkySHBasis::Index(2,  0)] = Vec3f( 7.0f,  3.0f, -8.0f);
    coeffs[SkySHBasis::Index(2,  1)] = Vec3f( 2.0f, -5.0f, 4.0f);
    coeffs[SkySHBasis::Index(2,  2)] = Vec3f(-6.0f,  4.0f, 2.0f);

    SkySH sh;
    sh.LoadCoefficients(coeffs);

    const Vec3f d = norm(Vec3f(0.4f, 0.6f, -0.7f));

    double basis[kSkySHCoeffCount];
    ReferenceBasis(d.x, d.y, d.z, basis);

    Vec3f raw(vl_0);
    for (int m = -2; m <= 2; ++m)
        raw += coeffs[SkySHBasis::Index(2, m)] * float(basis[SkySHBasis::Index(2, m)]);

    TASK_CHECK(CloseV(sh.Evaluate(d, 0.0f), raw, 1e-4f * (std::sqrt(sqrlen(raw)) + 1.0f)));
    TASK_CHECK(CloseV(sh.Evaluate(d, 1.0f), raw * 0.25f, 1e-4f * (std::sqrt(sqrlen(raw)) + 1.0f)));
    TASK_CHECK(CloseV(sh.Evaluate(d, 0.5f),
        raw * SkySHBasis::KernelWeight(2, 0.5f), 1e-4f * (std::sqrt(sqrlen(raw)) + 1.0f)));

    return 0;
}

static int ConvolutionRealProjectionBandScaling()
{
    // On a real projection, convolution must only rescale whole bands:
    // evaluate via per-band coefficients and compare with Evaluate.
    SkyHosek sky;
    sky.Update(norm(Vec3f(0.6f, 0.0f, 0.8f)), 3.0f, Vec3f(0.5f, 0.2f, 0.1f), 0.0f);

    SkySH sh;
    sh.Project(sky, Vec3f(0.5f, 0.2f, 0.1f));

    const Vec3f d = norm(Vec3f(-0.2f, 0.8f, 0.55f));

    float basis[kSkySHCoeffCount];
    SkySHBasis::EvalBasis(d.x, d.y, d.z, basis);

    const float rough[] = { 0.0f, 0.33f, 0.66f, 1.0f };

    for (float r : rough)
    {
        Vec3f expected(vl_0);

        for (int l = 0; l < kSkySHBands; ++l)
        {
            const float w = SkySHBasis::KernelWeight(l, r);
            for (int m = -l; m <= l; ++m)
            {
                const int i = SkySHBasis::Index(l, m);
                expected += sh.Coefficients()[i] * (w * basis[i]);
            }
        }

        TASK_CHECK(CloseV(sh.Evaluate(d, r), expected, 1e-3f * (std::sqrt(sqrlen(expected)) + 1.0f)));
    }

    return 0;
}

//--------------------------------------------------------------------------
// shader group: GPU-free adaptation of the GLSL branch of the shared include
//--------------------------------------------------------------------------

namespace SkySHGLSLAdapt
{
    // Minimal GLSL runtime shim: just enough of the GLSL built-ins used by
    // the shader branch of sky_sh_basis.inc to compile as standard C++14.
    struct vec3
    {
        float x, y, z;
        vec3() : x(0.0f), y(0.0f), z(0.0f) {}
        explicit vec3(float s) : x(s), y(s), z(s) {}
        vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    };

    inline float sqrt(float v)            { return std::sqrt(v); }
    inline float cos(float v)             { return std::cos(v); }
    inline float sin(float v)             { return std::sin(v); }
    inline float atan(float y, float x)   { return std::atan2(y, x); }
    inline float max(float a, float b)    { return a > b ? a : b; }
    inline float clamp(float v, float lo, float hi)
    {
        return max(lo, v < hi ? v : hi);
    }

    // Pull in the actual shader branch of the shared file, so this is the
    // exact code shipped to the GPU rather than a hand-written copy.
#define inout
#define out
#undef SKY_SH_BASIS_INC
#define SKY_SH_FORCE_GLSL
#include "sky_sh_basis.inc"
#undef SKY_SH_FORCE_GLSL
#undef inout
#undef out
}

static int ShaderAdaptedBasisMatchesIndices()
{
    // The adapted GLSL basis, the production C++ basis and the independent
    // reference must agree at every index for a set of directions.
    const Vec3f dirs[] =
    {
        norm(Vec3f(0.3f, -0.5f, 0.8f)),
        norm(Vec3f(-0.9f, 0.2f, 0.4f)),
        norm(Vec3f(0.1f, 0.7f, -0.7f)),
        Vec3f(0.0f, 0.0f, 1.0f),
        norm(Vec3f(1.0f, 1.0f, 0.0f))
    };

    for (const Vec3f& d : dirs)
    {
        float cppBasis[kSkySHCoeffCount];
        SkySHBasis::EvalBasis(d.x, d.y, d.z, cppBasis);

        float glslBasis[kSkySHCoeffCount];
        SkySHGLSLAdapt::skySHEvalBasis(SkySHGLSLAdapt::vec3(d.x, d.y, d.z), glslBasis);

        double refBasis[kSkySHCoeffCount];
        ReferenceBasis(d.x, d.y, d.z, refBasis);

        for (int i = 0; i < kSkySHCoeffCount; ++i)
        {
            TASK_CHECK(Close(glslBasis[i], cppBasis[i], 2e-6f));
            TASK_CHECK(Close(glslBasis[i], float(refBasis[i]), 2e-6f));
        }
    }

    // Explicit index spot checks for the documented convention.
    const Vec3f d = norm(Vec3f(0.4f, 0.6f, 0.7f));
    float b[kSkySHCoeffCount];
    SkySHBasis::EvalBasis(d.x, d.y, d.z, b);

    TASK_CHECK(Close(b[SkySHBasis::Index(0, 0)], 0.28209479f, 1e-6f));
    TASK_CHECK(Close(b[SkySHBasis::Index(1, 0)], 0.48860251f * d.z, 1e-5f));

    return 0;
}

static int ShaderAdaptedKernelWeights()
{
    // The real GLSL and C++ paths are both float32. Under the C++ adaptation
    // unsuffixed literals such as 231.0 are double rather than float, so a
    // few high-band weights differ by a float ulp; tolerate that rounding.
    const float tolerance = 2e-6f;

    for (int l = 0; l < kSkySHBands; ++l)
    {
        TASK_CHECK(Close(SkySHGLSLAdapt::skySHKernelWeight(l, 0.0f),
                         SkySHBasis::KernelWeight(l, 0.0f), tolerance));
        TASK_CHECK(Close(SkySHGLSLAdapt::skySHKernelWeight(l, 0.5f),
                         SkySHBasis::KernelWeight(l, 0.5f), tolerance));
        TASK_CHECK(Close(SkySHGLSLAdapt::skySHKernelWeight(l, 1.0f),
                         SkySHBasis::KernelWeight(l, 1.0f), tolerance));
    }

    return 0;
}

static int ShaderTexturePathReconstruction()
{
    // Exercise the exact GLSL skyColourSH() summation on the CPU: sample the
    // filled texture and dot with the adapted basis. The texture already
    // carries kernel-weighted coefficients, so no second convolution occurs.
    SkyHosek sky;
    sky.Update(norm(Vec3f(0.6f, 0.0f, 0.8f)), 3.0f, Vec3f(0.6f, 0.3f, 0.15f), 0.0f);

    SkySH sh;
    sh.Project(sky, Vec3f(0.6f, 0.3f, 0.15f));

    const Vec3f dirs[] =
    {
        norm(Vec3f(0.3f, -0.5f, 0.8f)),
        norm(Vec3f(-0.9f, 0.2f, 0.4f)),
        norm(Vec3f(0.1f, 0.7f, -0.7f))
    };

    const float rough[] = { 0.0f, 0.5f, 1.0f };

    for (float r : rough)
    {
        float texture[kSkySHCoeffCount][4];
        sh.FillTexture(texture, r);

        for (const Vec3f& d : dirs)
        {
            float glslBasis[kSkySHCoeffCount];
            SkySHGLSLAdapt::skySHEvalBasis(SkySHGLSLAdapt::vec3(d.x, d.y, d.z), glslBasis);

            Vec3f shaderResult(vl_0);

            for (int i = 0; i < kSkySHCoeffCount; ++i)
            {
                TASK_CHECK(Close(texture[i][3], 0.0f, 0.0f));   // alpha is always 0
                shaderResult += Vec3f(texture[i][0], texture[i][1], texture[i][2]) * glslBasis[i];
            }

            const Vec3f expected = sh.Evaluate(d, r);
            const float tol = 1e-3f * (std::sqrt(sqrlen(expected)) + 1.0f);
            TASK_CHECK(CloseV(shaderResult, expected, tol));
        }
    }

    return 0;
}

static int ShaderTextureHoldsPreconvolvedCoefficients()
{
    // At r > 0 the texture must differ from the raw coefficients exactly by
    // the per-band kernel weight: proving the shader consumes pre-convolved
    // data and must not convolve again.
    Vec3f coeffs[kSkySHCoeffCount];
    for (int i = 0; i < kSkySHCoeffCount; ++i)
        coeffs[i] = Vec3f(float(i + 1) * 10.0f, -float(i + 1) * 5.0f, float(i) * 1.5f);

    SkySH sh;
    sh.LoadCoefficients(coeffs);

    float raw[kSkySHCoeffCount][4];
    float weighted[kSkySHCoeffCount][4];
    sh.FillTexture(raw, 0.0f);
    sh.FillTexture(weighted, 0.5f);

    for (int i = 0; i < kSkySHCoeffCount; ++i)
    {
        int l = 0;
        while ((l + 1) * (l + 1) <= i)
            ++l;

        const float w = SkySHBasis::KernelWeight(l, 0.5f);

        for (int c = 0; c < 3; ++c)
        {
            TASK_CHECK(Close(raw[i][c], coeffs[i][c], 1e-6f));
            TASK_CHECK(Close(weighted[i][c], coeffs[i][c] * w, 1e-4f));
        }
    }

    return 0;
}

//--------------------------------------------------------------------------
// Registration
//--------------------------------------------------------------------------

const struct task_case task_cases[] =
{
    { "projection", "off_axis_sun",            ProjectionOffAxisSun },
    { "projection", "rotated_sun",             ProjectionRotatedSun },
    { "projection", "overhead_axisymmetric",   ProjectionOverheadSunIsAxisymmetric },
    { "projection", "non_axial_coefficients",  ProjectionNonAxialCoefficientsPresent },
    { "projection", "azimuth_rotation",        ProjectionAzimuthRotationRelation },
    { "projection", "coloured_ground",         ProjectionColouredGroundIsPerChannel },
    { "projection", "snapshot_and_reproject",  ProjectionSnapshotAndReproject },
    { "projection", "hdr_and_negative_load",   ProjectionPreservesHDRAndSign },

    { "convolution", "r0_identity",            ConvolutionZeroRoughnessIsIdentity },
    { "convolution", "r1_closed_form",         ConvolutionRoughnessOneClosedForm },
    { "convolution", "intermediate_integral",  ConvolutionIntermediateMatchesIntegral },
    { "convolution", "first_order_field",      ConvolutionFirstOrderLightField },
    { "convolution", "second_order_field",     ConvolutionSecondOrderLightField },
    { "convolution", "real_band_scaling",      ConvolutionRealProjectionBandScaling },

    { "shader", "adapted_basis_indices",       ShaderAdaptedBasisMatchesIndices },
    { "shader", "adapted_kernel_weights",      ShaderAdaptedKernelWeights },
    { "shader", "texture_path_reconstruction", ShaderTexturePathReconstruction },
    { "shader", "texture_preconvolved",        ShaderTextureHoldsPreconvolvedCoefficients }
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
