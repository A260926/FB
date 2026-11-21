
// tests/portal_sampling.cpp*
//
// Task-case tests for portal-assisted environment sampling. Registered with
// the task driver (task_support/driver.c) via task_cases/task_case_count; no
// main() here. Cases are grouped into "crop" (angular grid cropping of the
// portal distribution), "mixture" (the portal/environment mixture exposed by
// InfiniteAreaLight::Sample_Li/Pdf_Li and the light factory) and "transport"
// (end-to-end EstimateDirect transport with real occlusion).

#include "task_support/check.h"

#include "api.h"
#include "accelerators/bvh.h"
#include "integrator.h"
#include "lights/infinite.h"
#include "lights/portal_distribution.h"
#include "materials/matte.h"
#include "paramset.h"
#include "primitive.h"
#include "samplers/random.h"
#include "sampling.h"
#include "scene.h"
#include "shapes/sphere.h"
#include "shapes/triangle.h"
#include "textures/constant.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace pbrt;

// ---------------------------------------------------------------------------
// Shared helpers

static void EnsureInit() {
    static bool inited = false;
    if (!inited) {
        Options o;
        o.nThreads = 1;
        pbrtInit(o);
        inited = true;
    }
}

// 1x1 unit environment map shared by the distribution-level tests.
static const MIPMap<RGBSpectrum> &UnitMap() {
    static RGBSpectrum one(1.f);
    static MIPMap<RGBSpectrum> map(Point2i(1, 1), &one);
    return map;
}

// Independent (test-side) cropped mass of the rectangle [a0,a1]x[b0,b1]:
// sums cell density times true rectangular overlap over the whole grid.
static double IndependentMass(const PortalDistribution &dist, double a0,
                              double a1, double b0, double b1) {
    int R = dist.Resolution();
    double h = Pi / R, half = Pi / 2, total = 0;
    for (int j = 0; j < R; ++j) {
        double bLo = -half + j * h;
        double ob = std::min(b1, bLo + h) - std::max(b0, bLo);
        if (ob <= 0) continue;
        for (int i = 0; i < R; ++i) {
            double aLo = -half + i * h;
            double oa = std::min(a1, aLo + h) - std::max(a0, aLo);
            if (oa <= 0) continue;
            total += (double)dist.CellDensity(i, j) * oa * ob;
        }
    }
    return total;
}

// Dense midpoint quadrature of the piecewise-constant cell density over the
// rectangle; a second, quadrature-based mass estimate.
static double QuadratureMass(const PortalDistribution &dist, double a0,
                             double a1, double b0, double b1, int n) {
    int R = dist.Resolution();
    double h = Pi / R, sum = 0;
    for (int y = 0; y < n; ++y) {
        double b = b0 + (y + .5) / n * (b1 - b0);
        int j = Clamp((int)((b + Pi / 2) / h), 0, R - 1);
        for (int x = 0; x < n; ++x) {
            double a = a0 + (x + .5) / n * (a1 - a0);
            int i = Clamp((int)((a + Pi / 2) / h), 0, R - 1);
            sum += dist.CellDensity(i, j);
        }
    }
    return sum * (a1 - a0) * (b1 - b0) / ((double)n * n);
}

static int CellOf(const PortalDistribution &dist, double v) {
    int R = dist.Resolution();
    return Clamp((int)((v + Pi / 2) / (Pi / R)), 0, R - 1);
}

// Parameters for the production light factory.
struct LightParams {
    std::vector<Float> portals;
    bool specifyResolution = false;
    int resolution = 64;
    bool specifyMix = false;
    Float mix = 0.9f;
    Spectrum L = Spectrum(1.f);
    std::string mapname;
};

static std::shared_ptr<InfiniteAreaLight> MakeLight(const Transform &t,
                                                    const LightParams &params) {
    ParamSet ps;
    if (!params.portals.empty()) {
        std::unique_ptr<Float[]> v(new Float[params.portals.size()]);
        std::copy(params.portals.begin(), params.portals.end(), v.get());
        ps.AddFloat("portals", std::move(v), (int)params.portals.size());
    }
    if (params.specifyResolution) {
        std::unique_ptr<int[]> v(new int[1]);
        v[0] = params.resolution;
        ps.AddInt("portalresolution", std::move(v), 1);
    }
    if (params.specifyMix) {
        std::unique_ptr<Float[]> v(new Float[1]);
        v[0] = params.mix;
        ps.AddFloat("portalmix", std::move(v), 1);
    }
    {
        Float rgb[3];
        params.L.ToRGB(rgb);
        std::unique_ptr<Float[]> v(new Float[3]);
        v[0] = rgb[0];
        v[1] = rgb[1];
        v[2] = rgb[2];
        ps.AddRGBSpectrum("L", std::move(v), 3);
    }
    if (!params.mapname.empty()) {
        std::unique_ptr<std::string[]> v(new std::string[1]);
        v[0] = params.mapname;
        ps.AddString("mapname", std::move(v), 1);
    }
    return CreateInfiniteLight(t, ps);
}

// ---------------------------------------------------------------------------
// crop group: angular grid construction and cropping

// Cell weights: luminance of the environment at the cell center times the
// solid angle Jacobian, constant per cell.
static int TestCropCellWeights() {
    EnsureInit();
    RGBSpectrum texels[8];
    for (int k = 0; k < 8; ++k) texels[k] = RGBSpectrum(0.25f * (k + 1));
    MIPMap<RGBSpectrum> map(Point2i(4, 2), texels);
    std::vector<PortalRect> portals = {{-1, -1, 1, 1, 2}};
    PortalDistribution dist(portals, 8, 0.5f, map);
    TASK_CHECK(dist.Resolution() == 8);
    int R = dist.Resolution();
    double h = Pi / R;
    for (int j = 0; j < R; ++j)
        for (int i = 0; i < R; ++i) {
            Float a = (Float)(-Pi / 2 + (i + .5) * h);
            Float b = (Float)(-Pi / 2 + (j + .5) * h);
            Vector3f w = PortalDistribution::Direction(a, b);
            Point2f st(SphericalPhi(w) * Inv2Pi, SphericalTheta(w) * InvPi);
            Float expect = map.Lookup(st).y() *
                           PortalDistribution::Jacobian(a, b);
            TASK_CHECK(std::abs(dist.CellDensity(i, j) - expect) <=
                       1e-5 * std::max<Float>(1, std::abs(expect)));
        }
    // The direction/d Jacobian pair is consistent with w = normalize(tan a,
    // tan b, 1): check the mapping and the Jacobian at a few points.
    Vector3f w0 = PortalDistribution::Direction(0, 0);
    TASK_CHECK(std::abs(w0.x) < 1e-7 && std::abs(w0.y) < 1e-7 &&
               std::abs(w0.z - 1) < 1e-7);
    TASK_CHECK(std::abs(PortalDistribution::Jacobian(0, 0) - 1) < 1e-6);
    Float a = 0.3f, b = -0.4f;
    Vector3f w = PortalDistribution::Direction(a, b);
    TASK_CHECK(std::abs(std::atan2(w.x, w.z) - a) < 1e-6);
    TASK_CHECK(std::abs(std::atan2(w.y, w.z) - b) < 1e-6);
    // d_omega = cos(theta) * (sec^2 a sec^2 b) / r^2 da db with r = |w|;
    // cross-check against cos^3-based form: J = 1/(cos^2 a cos^2 b) * cos^3.
    double ta = std::tan(a), tb = std::tan(b);
    double r2 = 1 + ta * ta + tb * tb;
    double expectJ = (1 + ta * ta) * (1 + tb * tb) / (r2 * std::sqrt(r2));
    TASK_CHECK(std::abs(PortalDistribution::Jacobian(a, b) - expectJ) < 1e-6);
    return 0;
}

// Cropped portal mass: partial edge cells must contribute their true covered
// area. Checked against an independent exact overlap sum and against dense
// midpoint quadrature.
static int TestCropPartialMass() {
    EnsureInit();
    std::vector<PortalRect> portals = {{-1, -1, 1, 1, 2}};
    PortalDistribution dist(portals, 8, 0.9f, UnitMap());
    Point3f p(0.3f, -0.2f, 0.1f);
    auto rcv = dist.BuildReceiver(p);
    TASK_CHECK(rcv.projections.size() == 1);
    TASK_CHECK(rcv.totalMass > 0);
    // Independently recomputed projection of the portal rectangle.
    double dz = 2.0 - p.z;
    double a0 = std::atan2(-1.0 - p.x, dz), a1 = std::atan2(1.0 - p.x, dz);
    double b0 = std::atan2(-1.0 - p.y, dz), b1 = std::atan2(1.0 - p.y, dz);
    const auto &proj = rcv.projections[0];
    TASK_CHECK(std::abs(proj.a0 - a0) < 1e-6);
    TASK_CHECK(std::abs(proj.a1 - a1) < 1e-6);
    TASK_CHECK(std::abs(proj.b0 - b0) < 1e-6);
    TASK_CHECK(std::abs(proj.b1 - b1) < 1e-6);
    // With R = 8 the rectangle edges fall strictly inside cells on all four
    // sides, so this exercises partial coverage of edge cells.
    double h = Pi / 8;
    TASK_CHECK(std::abs((a0 + Pi / 2) / h - std::round((a0 + Pi / 2) / h)) >
               1e-3);
    TASK_CHECK(std::abs((a1 + Pi / 2) / h - std::round((a1 + Pi / 2) / h)) >
               1e-3);
    TASK_CHECK(std::abs((b0 + Pi / 2) / h - std::round((b0 + Pi / 2) / h)) >
               1e-3);
    TASK_CHECK(std::abs((b1 + Pi / 2) / h - std::round((b1 + Pi / 2) / h)) >
               1e-3);
    double expect = IndependentMass(dist, a0, a1, b0, b1);
    TASK_CHECK(std::abs(proj.mass - expect) <= 1e-6 * expect);
    TASK_CHECK(std::abs(rcv.totalMass - expect) <= 1e-6 * expect);
    double quad = QuadratureMass(dist, a0, a1, b0, b1, 2048);
    TASK_CHECK(std::abs(quad - expect) <= 2e-3 * expect);
    // A rectangle exactly aligned with cell boundaries picks up exactly the
    // full cells it covers.
    double edge = 2.0 * std::tan(Pi / 8);
    std::vector<PortalRect> aligned = {
        {(Float)-edge, (Float)-edge, 0, 0, 2}};
    PortalDistribution distAligned(aligned, 8, 0.9f, UnitMap());
    auto rcvAligned = distAligned.BuildReceiver(Point3f(0, 0, 0));
    TASK_CHECK(rcvAligned.projections.size() == 1);
    double fullCell =
        (double)distAligned.CellDensity(3, 3) * (Pi / 8) * (Pi / 8);
    TASK_CHECK(std::abs(rcvAligned.totalMass - fullCell) <=
               1e-6 * fullCell);
    // Receiver coplanar with or behind the portal plane: no valid portals.
    auto coplanar = dist.BuildReceiver(Point3f(0.3f, -0.2f, 2.0f));
    TASK_CHECK(coplanar.projections.empty() && coplanar.totalMass == 0);
    auto behind = dist.BuildReceiver(Point3f(0.3f, -0.2f, 2.5f));
    TASK_CHECK(behind.projections.empty() && behind.totalMass == 0);
    return 0;
}

// The combined portal branch density integrates to one over the hemisphere.
static int TestCropNormalization() {
    EnsureInit();
    std::vector<PortalRect> portals = {
        {-1, -1, 1, 1, 2}, {0.3f, -0.5f, 2, 0.7f, 3}};
    PortalDistribution dist(portals, 16, 0.9f, UnitMap());
    auto rcv = dist.BuildReceiver(Point3f(0.1f, 0.2f, 0.0f));
    TASK_CHECK(rcv.projections.size() == 2);
    TASK_CHECK(rcv.totalMass > 0);
    const int n = 900;
    double sum = 0;
    for (int y = 0; y < n; ++y) {
        double b = -Pi / 2 + (y + .5) / n * Pi;
        for (int x = 0; x < n; ++x) {
            double a = -Pi / 2 + (x + .5) / n * Pi;
            Vector3f w = PortalDistribution::Direction((Float)a, (Float)b);
            sum += (double)dist.Pdf(rcv, w) *
                   PortalDistribution::Jacobian((Float)a, (Float)b);
        }
    }
    double integral = sum * (Pi / n) * (Pi / n);
    TASK_CHECK(std::abs(integral - 1) < 2e-3);
    return 0;
}

// Overlapping portals: the inverse density sums the contributions of every
// portal covering a direction.
static int TestCropOverlapDensity() {
    EnsureInit();
    std::vector<PortalRect> portals = {
        {-1, -1, 1, 1, 2}, {0.3f, -0.5f, 2, 0.7f, 3}};
    PortalDistribution dist(portals, 16, 0.9f, UnitMap());
    auto rcv = dist.BuildReceiver(Point3f(0, 0, 0));
    TASK_CHECK(rcv.projections.size() == 2);
    // Independent masses of the two projected rectangles.
    double a10 = std::atan2(-1.0, 2.0), a11 = std::atan2(1.0, 2.0);
    double a20 = std::atan2(0.3, 3.0), a21 = std::atan2(2.0, 3.0);
    double b20 = std::atan2(-0.5, 3.0), b21 = std::atan2(0.7, 3.0);
    double m1 = IndependentMass(dist, a10, a11, a10, a11);
    double m2 = IndependentMass(dist, a20, a21, b20, b21);
    TASK_CHECK(std::abs(rcv.totalMass - (m1 + m2)) <= 1e-6 * (m1 + m2));
    auto checkPdf = [&](double a, double b, int n) -> int {
        Vector3f w = PortalDistribution::Direction((Float)a, (Float)b);
        double expect =
            n == 0 ? 0.0
                   : (double)dist.CellDensity(CellOf(dist, a),
                                              CellOf(dist, b)) *
                         n /
                         (rcv.totalMass *
                          PortalDistribution::Jacobian((Float)a, (Float)b));
        Float got = dist.Pdf(rcv, w);
        TASK_CHECK(std::abs(got - expect) <=
                   1e-5 * std::max(1.0, std::abs(expect)));
        return 0;
    };
    TASK_CHECK(checkPdf(0.2, 0.05, 2) == 0);   // inside both rectangles
    TASK_CHECK(checkPdf(0.05, 0.05, 1) == 0);  // inside the first only
    TASK_CHECK(checkPdf(0.5, 0.05, 1) == 0);   // inside the second only
    TASK_CHECK(checkPdf(0.05, 0.4, 1) == 0);   // first only (b > b21)
    TASK_CHECK(checkPdf(1.0, 0.2, 0) == 0);    // outside both
    TASK_CHECK(checkPdf(-0.4, -0.4, 1) == 0);  // first only
    // Directions at or beyond the horizon have no portal density.
    TASK_CHECK(dist.Pdf(rcv, Vector3f(0, 0, -1)) == 0);
    TASK_CHECK(dist.Pdf(rcv, Vector3f(1, 0, 0)) == 0);
    // Samples from the combined branch always land inside a portal and get
    // a nonzero density.
    for (int k = 0; k < 64; ++k) {
        Point2f u((k + .5f) / 64, (k % 7 + .5f) / 7);
        Vector3f w = dist.Sample(rcv, u);
        TASK_CHECK(w.z > 0);
        double a = std::atan2(w.x, w.z), b = std::atan2(w.y, w.z);
        bool inFirst = a >= a10 && a <= a11 && b >= a10 && b <= a11;
        bool inSecond = a >= a20 && a <= a21 && b >= b20 && b <= b21;
        TASK_CHECK(inFirst || inSecond);
        TASK_CHECK(dist.Pdf(rcv, w) > 0);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// mixture group: the light-level portal/environment mixture

// Pdf_Li must be the total per-steradian density of the two branches, and
// Sample_Li must return the same total density.
static int TestMixturePdfFormula() {
    EnsureInit();
    Transform id;
    LightParams lp;
    lp.portals = {-1, -1, 1, 1, 2, 0.3f, -0.5f, 2, 0.7f, 3};
    lp.specifyResolution = true;
    lp.resolution = 16;
    lp.specifyMix = true;
    lp.mix = 0.8f;
    auto light = MakeLight(id, lp);
    TASK_CHECK(light != nullptr);
    auto envLight = MakeLight(id, LightParams());
    TASK_CHECK(envLight != nullptr);
    std::vector<PortalRect> rects = {{-1, -1, 1, 1, 2},
                                     {0.3f, -0.5f, 2, 0.7f, 3}};
    PortalDistribution dist(rects, 16, 0.8f, UnitMap());
    Interaction ref(Point3f(0.1f, 0.2f, 0.0f), 0, MediumInterface());
    auto rcv = dist.BuildReceiver(Point3f(0.1f, 0.2f, 0.0f));
    TASK_CHECK(rcv.totalMass > 0);
    const Float m = 0.8f;
    // Probe directions covering the overlap, a single portal, no portal and
    // the lower hemisphere. (Identity transform: world == light space.)
    std::vector<Vector3f> probes = {
        PortalDistribution::Direction(0.2f, 0.05f),
        PortalDistribution::Direction(0.05f, 0.3f),
        PortalDistribution::Direction(0.5f, 0.05f),
        PortalDistribution::Direction(1.0f, 0.2f),
        PortalDistribution::Direction(-0.3f, -0.4f),
        Vector3f(0, 0, -1), Vector3f(0, 1, 0)};
    for (const Vector3f &w : probes) {
        Float envPdf = envLight->Pdf_Li(ref, w);
        Float expect = (1 - m) * envPdf + m * dist.Pdf(rcv, w);
        Float got = light->Pdf_Li(ref, w);
        TASK_CHECK(std::abs(got - expect) <=
                   2e-5 * std::max<Float>(1, std::abs(expect)));
    }
    // Sampled directions carry the same total density that Pdf_Li reports,
    // and portal-branch samples land inside a portal rectangle.
    double a10 = std::atan2(-1.0 - 0.1, 2.0), a11 = std::atan2(1.0 - 0.1, 2.0);
    double a20 = std::atan2(0.3 - 0.1, 3.0), a21 = std::atan2(2.0 - 0.1, 3.0);
    double b10 = std::atan2(-1.0 - 0.2, 2.0), b11 = std::atan2(1.0 - 0.2, 2.0);
    double b20 = std::atan2(-0.5 - 0.2, 3.0), b21 = std::atan2(0.7 - 0.2, 3.0);
    int portalSamples = 0, envSamples = 0;
    for (int k = 0; k < 64; ++k) {
        Point2f u((k + .5f) / 64, .37f);
        Vector3f wi;
        Float pdf;
        VisibilityTester vis;
        Spectrum li = light->Sample_Li(ref, u, &wi, &pdf, &vis);
        Float q = light->Pdf_Li(ref, wi);
        TASK_CHECK(pdf > 0 && std::isfinite(pdf));
        TASK_CHECK(std::abs(q - pdf) <= 1e-3 * std::max<Float>(1, q));
        double a = std::atan2(wi.x, wi.z), b = std::atan2(wi.y, wi.z);
        bool inPortal = (a >= a10 && a <= a11 && b >= b10 && b <= b11) ||
                        (a >= a20 && a <= a21 && b >= b20 && b <= b21);
        if (u[0] < m) {
            ++portalSamples;
            TASK_CHECK(inPortal);
            // Radiance is the actual environment value, not a block
            // constant: unit map -> exactly 1.
            TASK_CHECK(std::abs(li[0] - 1) < 1e-6);
        } else {
            ++envSamples;
        }
    }
    TASK_CHECK(portalSamples > 0 && envSamples > 0);
    return 0;
}

// Fallbacks: inactive portals (receiver behind or coplanar), zero mixture
// weight, and portals facing black texels all reduce to the original
// whole-map sampling, bit-for-bit comparable with a portal-free light.
static int TestMixtureFallback() {
    EnsureInit();
    Transform id;
    LightParams lp;
    lp.portals = {-1, -1, 1, 1, 2};
    lp.specifyResolution = true;
    lp.resolution = 16;
    lp.specifyMix = true;
    lp.mix = 0.9f;
    auto light = MakeLight(id, lp);
    auto envLight = MakeLight(id, LightParams());
    TASK_CHECK(light != nullptr && envLight != nullptr);
    auto checkSame = [&](const Interaction &ref, const InfiniteAreaLight &a,
                         const InfiniteAreaLight &b) -> int {
        for (int k = 0; k < 16; ++k) {
            Point2f u((k + .5f) / 16, .61f);
            Vector3f w1, w2;
            Float p1, p2;
            VisibilityTester v1, v2;
            Spectrum l1 = a.Sample_Li(ref, u, &w1, &p1, &v1);
            Spectrum l2 = b.Sample_Li(ref, u, &w2, &p2, &v2);
            TASK_CHECK(p1 == p2);
            TASK_CHECK(w1 == w2);
            TASK_CHECK(l1 == l2);
            TASK_CHECK(a.Pdf_Li(ref, w1) == b.Pdf_Li(ref, w1));
        }
        return 0;
    };
    // Receiver coplanar with (z == 2) or behind (z > 2) the portal plane.
    for (Float z : {2.0f, 2.5f, 5.0f}) {
        Interaction ref(Point3f(0.2f, -0.1f, z), 0, MediumInterface());
        TASK_CHECK(checkSame(ref, *light, *envLight) == 0);
    }
    // portalmix == 0: portals are never sampled.
    LightParams lp0 = lp;
    lp0.mix = 0.0f;
    auto light0 = MakeLight(id, lp0);
    TASK_CHECK(light0 != nullptr);
    Interaction ref0(Point3f(0.2f, -0.1f, 0.0f), 0, MediumInterface());
    TASK_CHECK(checkSame(ref0, *light0, *envLight) == 0);
    // Portal facing only black texels: zero branch mass, full fallback.
    const char *pfmPath = "portal_test_black.pfm";
    FILE *f = fopen(pfmPath, "wb");
    TASK_CHECK(f != nullptr);
    // 8x4 map. Trilinear lookup with repeat wrapping blends neighboring
    // rows, so the black region must cover texel rows 0, 1 and 3 to keep
    // everything below theta ~ 67.5 degrees (v < 0.375) pure black; row 2
    // stays white so the whole-map distribution keeps its mass. pbrt flips
    // PFM rows on read, so the file holds data rows 3, 2, 1, 0 in order.
    fprintf(f, "PF\n8 4\n-1.0\n");
    for (int row = 0; row < 4; ++row) {
        float rgb[24];
        float v = (row == 1) ? 1.f : 0.f;  // file row 1 == data row 2
        for (int k = 0; k < 24; ++k) rgb[k] = v;
        TASK_CHECK(fwrite(rgb, sizeof(float), 24, f) == 24);
    }
    fclose(f);
    LightParams lb;
    lb.portals = {-0.5f, -0.5f, 0.5f, 0.5f, 2};
    lb.specifyResolution = true;
    lb.resolution = 16;
    lb.specifyMix = true;
    lb.mix = 0.9f;
    lb.mapname = pfmPath;
    auto blackLight = MakeLight(id, lb);
    LightParams eb;
    eb.mapname = pfmPath;
    auto blackEnv = MakeLight(id, eb);
    TASK_CHECK(blackLight != nullptr && blackEnv != nullptr);
    Interaction refb(Point3f(0, 0, 0), 0, MediumInterface());
    TASK_CHECK(checkSame(refb, *blackLight, *blackEnv) == 0);
    // The same light seen from a receiver whose portal projection faces
    // bright texels must not fall back (sanity that the fallback above is
    // caused by the black region and not by the map itself).
    Interaction refBright(Point3f(10, 0, 0), 0, MediumInterface());
    bool differs = false;
    for (int k = 0; k < 16 && !differs; ++k) {
        Point2f u((k + .5f) / 16, .61f);
        Vector3f w1, w2;
        Float p1, p2;
        VisibilityTester v1, v2;
        blackLight->Sample_Li(refBright, u, &w1, &p1, &v1);
        blackEnv->Sample_Li(refBright, u, &w2, &p2, &v2);
        if (p1 != p2 || !(w1 == w2)) differs = true;
    }
    TASK_CHECK(differs);
    std::remove(pfmPath);
    return 0;
}

// Factory validation and backwards compatibility.
static int TestMixtureFactory() {
    EnsureInit();
    Transform id;
    // Valid configurations, including defaults and parameter boundaries.
    {
        LightParams lp;
        lp.portals = {-1, -1, 1, 1, 2};
        TASK_CHECK(MakeLight(id, lp) != nullptr);  // default res/mix
        lp.specifyResolution = true;
        lp.resolution = 2;
        TASK_CHECK(MakeLight(id, lp) != nullptr);
        lp.resolution = 512;
        TASK_CHECK(MakeLight(id, lp) != nullptr);
        lp.resolution = 64;
        lp.specifyMix = true;
        lp.mix = 0.0f;
        TASK_CHECK(MakeLight(id, lp) != nullptr);
        lp.mix = 0.999f;
        TASK_CHECK(MakeLight(id, lp) != nullptr);
        lp.specifyMix = false;
        lp.portals = {-1, -1, 1, 1, 2, 0, 0, 1, 1, 3, -2, -2, -1, -1, 4};
        TASK_CHECK(MakeLight(id, lp) != nullptr);
    }
    // Rigid transforms (rotation + translation) are accepted.
    {
        LightParams lp;
        lp.portals = {-1, -1, 1, 1, 2};
        Transform t =
            Translate(Vector3f(1, 2, 3)) * Rotate(37, Vector3f(0.3f, 1, 0.2f));
        TASK_CHECK(MakeLight(t, lp) != nullptr);
    }
    // Non-rigid transforms are rejected with portals, accepted without.
    {
        LightParams lp;
        lp.portals = {-1, -1, 1, 1, 2};
        TASK_CHECK(MakeLight(Scale(2, 1, 1), lp) == nullptr);
        TASK_CHECK(MakeLight(Scale(1, 1, 0.5f), lp) == nullptr);
        Float shearM[4][4] = {{1, 0.5f, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
        TASK_CHECK(MakeLight(Transform(shearM), lp) == nullptr);
        LightParams np;
        TASK_CHECK(MakeLight(Scale(2, 1, 1), np) != nullptr);
    }
    // Malformed portal arrays are rejected.
    {
        LightParams lp;
        lp.portals = {-1, -1, 1, 1};  // not a multiple of five
        TASK_CHECK(MakeLight(id, lp) == nullptr);
        lp.portals = {-1, -1, 1, 1, 2, 0, 0};
        TASK_CHECK(MakeLight(id, lp) == nullptr);
        lp.portals = {1, -1, -1, 1, 2};  // xmax < xmin
        TASK_CHECK(MakeLight(id, lp) == nullptr);
        lp.portals = {-1, -1, -1, 1, 2};  // zero width
        TASK_CHECK(MakeLight(id, lp) == nullptr);
        lp.portals = {-1, 2, 1, 2, 2};  // zero height
        TASK_CHECK(MakeLight(id, lp) == nullptr);
        Float nan = std::numeric_limits<Float>::quiet_NaN();
        Float inf = std::numeric_limits<Float>::infinity();
        lp.portals = {-1, -1, nan, 1, 2};
        TASK_CHECK(MakeLight(id, lp) == nullptr);
        lp.portals = {-1, -1, 1, 1, inf};
        TASK_CHECK(MakeLight(id, lp) == nullptr);
        lp.portals = {-1, -1, 1, 1, -inf};
        TASK_CHECK(MakeLight(id, lp) == nullptr);
    }
    // Out-of-range resolution and mixture weights are rejected.
    {
        LightParams lp;
        lp.portals = {-1, -1, 1, 1, 2};
        lp.specifyResolution = true;
        lp.resolution = 1;
        TASK_CHECK(MakeLight(id, lp) == nullptr);
        lp.resolution = 513;
        TASK_CHECK(MakeLight(id, lp) == nullptr);
        lp.resolution = 0;
        TASK_CHECK(MakeLight(id, lp) == nullptr);
        lp.resolution = 64;
        lp.specifyMix = true;
        lp.mix = 1.0f;
        TASK_CHECK(MakeLight(id, lp) == nullptr);
        lp.mix = -0.1f;
        TASK_CHECK(MakeLight(id, lp) == nullptr);
        lp.mix = std::numeric_limits<Float>::infinity();
        TASK_CHECK(MakeLight(id, lp) == nullptr);
        lp.mix = std::numeric_limits<Float>::quiet_NaN();
        TASK_CHECK(MakeLight(id, lp) == nullptr);
    }
    // The old constructor call keeps working and matches the factory.
    {
        InfiniteAreaLight old(id, Spectrum(2.f), 1, "");
        LightParams np;
        np.L = Spectrum(2.f);
        auto viaFactory = MakeLight(id, np);
        TASK_CHECK(viaFactory != nullptr);
        Interaction ref(Point3f(0, 0, -4), 0, MediumInterface());
        for (int k = 0; k < 8; ++k) {
            Point2f u((k + .5f) / 8, .13f + .73f * (k % 7) / 7);
            Vector3f w1, w2;
            Float p1, p2;
            VisibilityTester v1, v2;
            Spectrum l1 = old.Sample_Li(ref, u, &w1, &p1, &v1);
            Spectrum l2 = viaFactory->Sample_Li(ref, u, &w2, &p2, &v2);
            TASK_CHECK(p1 == p2);
            TASK_CHECK(w1 == w2);
            TASK_CHECK(l1 == l2);
            TASK_CHECK(old.Pdf_Li(ref, w1) == viaFactory->Pdf_Li(ref, w1));
        }
    }
    return 0;
}

// Le, Sample_Le and Pdf_Le are untouched by portals.
static int TestMixtureLeUnchanged() {
    EnsureInit();
    Transform id;
    LightParams lp;
    lp.portals = {-1, -1, 1, 1, 2};
    lp.specifyResolution = true;
    lp.resolution = 16;
    lp.specifyMix = true;
    lp.mix = 0.9f;
    lp.L = Spectrum(2.f);
    auto light = MakeLight(id, lp);
    LightParams ep;
    ep.L = Spectrum(2.f);
    auto envLight = MakeLight(id, ep);
    TASK_CHECK(light != nullptr && envLight != nullptr);
    // Give both lights the same scene bounds so Sample_Le/Pdf_Le agree.
    static Transform identity;
    std::vector<std::shared_ptr<Primitive>> prims;
    prims.push_back(std::make_shared<GeometricPrimitive>(
        std::make_shared<Sphere>(&identity, &identity, false, 1, -1, 1, 360),
        nullptr, nullptr, MediumInterface()));
    std::vector<std::shared_ptr<Light>> lights = {light, envLight};
    Scene scene(std::make_shared<BVHAccel>(prims), lights);
    for (int k = 0; k < 8; ++k) {
        Point2f u1((k + .5f) / 8, .29f), u2(.27f, .63f);
        Ray r1, r2;
        Normal3f n1, n2;
        Float pp1, pd1, pp2, pd2;
        Spectrum e1 = light->Sample_Le(u1, u2, .4f, &r1, &n1, &pp1, &pd1);
        Spectrum e2 = envLight->Sample_Le(u1, u2, .4f, &r2, &n2, &pp2, &pd2);
        TASK_CHECK(pp1 == pp2 && pd1 == pd2);
        TASK_CHECK(r1.o == r2.o && r1.d == r2.d && r1.time == r2.time);
        TASK_CHECK(e1 == e2);
        Float qp1, qd1, qp2, qd2;
        light->Pdf_Le(r1, n1, &qp1, &qd1);
        envLight->Pdf_Le(r1, n1, &qp2, &qd2);
        TASK_CHECK(qp1 == qp2 && qd1 == qd2);
        RayDifferential ray(Point3f(1, 2, 3), Vector3f(0.2f, 0.5f, 0.8f));
        TASK_CHECK(light->Le(ray) == envLight->Le(ray));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// transport group: EstimateDirect with real occlusion

static void AddQuad(std::vector<Point3f> &P, std::vector<int> &idx, Point3f a,
                    Point3f b, Point3f c, Point3f d) {
    int base = (int)P.size();
    P.push_back(a);
    P.push_back(b);
    P.push_back(c);
    P.push_back(d);
    idx.push_back(base + 0);
    idx.push_back(base + 1);
    idx.push_back(base + 2);
    idx.push_back(base + 0);
    idx.push_back(base + 2);
    idx.push_back(base + 3);
}

// A closed room (x in [-3,3], y in [0,3], z in [-2,2]); with door == true
// the z = 2 wall has a doorway opening x in [-1,1], y in [0.5,2.5].
static std::shared_ptr<Scene> MakeRoomScene(std::shared_ptr<Light> light,
                                            bool door) {
    EnsureInit();
    std::vector<Point3f> P;
    std::vector<int> idx;
    AddQuad(P, idx, {-3, 0, -2}, {3, 0, -2}, {3, 0, 2}, {-3, 0, 2});
    AddQuad(P, idx, {-3, 3, -2}, {-3, 3, 2}, {3, 3, 2}, {3, 3, -2});
    AddQuad(P, idx, {-3, 0, -2}, {-3, 3, -2}, {3, 3, -2}, {3, 0, -2});
    AddQuad(P, idx, {-3, 0, -2}, {-3, 0, 2}, {-3, 3, 2}, {-3, 3, -2});
    AddQuad(P, idx, {3, 0, -2}, {3, 3, -2}, {3, 3, 2}, {3, 0, 2});
    if (door) {
        AddQuad(P, idx, {-3, 0, 2}, {3, 0, 2}, {3, 0.5f, 2}, {-3, 0.5f, 2});
        AddQuad(P, idx, {-3, 2.5f, 2}, {3, 2.5f, 2}, {3, 3, 2}, {-3, 3, 2});
        AddQuad(P, idx, {-3, 0.5f, 2}, {-1, 0.5f, 2}, {-1, 2.5f, 2},
                {-3, 2.5f, 2});
        AddQuad(P, idx, {1, 0.5f, 2}, {3, 0.5f, 2}, {3, 2.5f, 2},
                {1, 2.5f, 2});
    } else {
        AddQuad(P, idx, {-3, 0, 2}, {-3, 3, 2}, {3, 3, 2}, {3, 0, 2});
    }
    static Transform identity;
    auto shapes = CreateTriangleMesh(&identity, &identity, false,
                                     (int)idx.size() / 3, idx.data(),
                                     (int)P.size(), P.data(), nullptr, nullptr,
                                     nullptr, nullptr, nullptr);
    auto Kd = std::make_shared<ConstantTexture<Spectrum>>(Spectrum(1.f));
    auto sigma = std::make_shared<ConstantTexture<Float>>(0.f);
    auto matte = std::make_shared<MatteMaterial>(Kd, sigma, nullptr);
    std::vector<std::shared_ptr<Primitive>> prims;
    for (auto &s : shapes)
        prims.push_back(std::make_shared<GeometricPrimitive>(
            s, matte, nullptr, MediumInterface()));
    std::vector<std::shared_ptr<Light>> lights = {light};
    return std::make_shared<Scene>(std::make_shared<BVHAccel>(prims), lights);
}

// Independent reference: irradiance integral \int_door cos(theta) d_omega at
// a floor point from a unit-radiance doorway, by midpoint quadrature.
static double DoorCosineIntegral(const Point3f &p, double x0, double y0,
                                 double x1, double y1, double z, int n) {
    double dz = z - p.z, sum = 0;
    for (int iy = 0; iy < n; ++iy) {
        double y = y0 + (iy + .5) / n * (y1 - y0);
        for (int ix = 0; ix < n; ++ix) {
            double x = x0 + (ix + .5) / n * (x1 - x0);
            double dx = x - p.x, dy = y - p.y;
            double r2 = dx * dx + dy * dy + dz * dz;
            sum += dz * dy / (r2 * r2);
        }
    }
    return sum * (x1 - x0) * (y1 - y0) / ((double)n * n);
}

static double Frac(double x) { return x - std::floor(x); }

// End-to-end: the mean direct lighting from EstimateDirect at a floor point
// inside the room matches the analytic doorway irradiance, using the
// production light factory, a BVH scene and real shadow rays.
static int TestTransportDirectMean() {
    EnsureInit();
    Transform id;
    LightParams lp;
    lp.portals = {-1, 0.5f, 1, 2.5f, 2};  // exactly the doorway
    lp.specifyResolution = true;
    lp.resolution = 64;
    lp.specifyMix = true;
    lp.mix = 0.9f;
    auto light = MakeLight(id, lp);
    TASK_CHECK(light != nullptr);
    auto scene = MakeRoomScene(light, true);
    // Receiver point on the floor, found by a real ray intersection.
    RayDifferential ray(Point3f(0.2f, 2.5f, 0.0f), Vector3f(0, -1, 0));
    SurfaceInteraction isect;
    TASK_CHECK(scene->Intersect(ray, &isect));
    TASK_CHECK(std::abs(isect.p.x - 0.2f) < 1e-6 &&
               std::abs(isect.p.z - 0.0f) < 1e-6);
    MemoryArena arena;
    isect.ComputeScatteringFunctions(ray, arena);
    TASK_CHECK(isect.bsdf != nullptr);
    // Reference: L_o = Kd/Pi * L_env * \int_door cos d_omega.
    double reference =
        DoorCosineIntegral(isect.p, -1, 0.5, 1, 2.5, 2, 1024) / Pi;
    TASK_CHECK(reference > 0.01 && reference < 0.5);
    // Fixed sample budget: K independent deterministic streams of N
    // EstimateDirect calls each. The error tolerance is six standard errors
    // of the grand mean, estimated from the stream means (plus a small
    // absolute floor), so the acceptance band is derived from the observed
    // estimator variance rather than picked ad hoc.
    const int K = 8, N = 256;
    RandomSampler sampler(1, 0);
    double means[K];
    for (int k = 0; k < K; ++k) {
        Spectrum sum(0.f);
        for (int i = 0; i < N; ++i) {
            // Each stream gets its own shifted stratification so that the
            // stream means are (nearly) independent and the standard error
            // below is meaningful.
            Point2f uLight((Float)Frac((i + .5) / N + k * 0.4613931169748673),
                           (Float)Frac((i + .5) * 0.7548776662466927 +
                                       k * 0.3183098861837907));
            Point2f uScat((Float)Frac((i + .5) * 0.5698402909980532 +
                                      k * 0.1937420172512169),
                          (Float)Frac((i + .5) * 0.6715334546156616 +
                                      k * 0.2749203746380512));
            sum += EstimateDirect(isect, uScat, *light, uLight, *scene,
                                  sampler, arena);
            arena.Reset();
        }
        means[k] = sum[0] / N;
        // Grey environment and grey material: channels agree.
        TASK_CHECK(std::abs(sum[1] / N - means[k]) <= 1e-5 * means[k] + 1e-7);
        TASK_CHECK(std::abs(sum[2] / N - means[k]) <= 1e-5 * means[k] + 1e-7);
    }
    double grand = 0;
    for (int k = 0; k < K; ++k) grand += means[k];
    grand /= K;
    double var = 0;
    for (int k = 0; k < K; ++k) var += (means[k] - grand) * (means[k] - grand);
    var /= (K - 1);
    double stdErr = std::sqrt(var / K);
    // The estimator is stable: stream means agree to within 25% rms.
    TASK_CHECK(std::sqrt(var) < 0.25 * grand);
    double tol = 6 * stdErr + 1e-4;
    fprintf(stderr,
            "transport direct_mean: mean=%.6f reference=%.6f stdErr=%.6f "
            "tol=%.6f\n",
            grand, reference, stdErr, tol);
    TASK_CHECK(std::abs(grand - reference) < tol);
    return 0;
}

// A fully enclosed receiver gets exactly zero direct light: every shadow
// ray and every BSDF ray is blocked by real geometry.
static int TestTransportOccludedZero() {
    EnsureInit();
    Transform id;
    LightParams lp;
    lp.portals = {-1, 0.5f, 1, 2.5f, 2};
    lp.specifyResolution = true;
    lp.resolution = 64;
    lp.specifyMix = true;
    lp.mix = 0.9f;
    auto light = MakeLight(id, lp);
    TASK_CHECK(light != nullptr);
    auto scene = MakeRoomScene(light, false);  // no doorway
    RayDifferential ray(Point3f(0.2f, 2.5f, 0.0f), Vector3f(0, -1, 0));
    SurfaceInteraction isect;
    TASK_CHECK(scene->Intersect(ray, &isect));
    MemoryArena arena;
    isect.ComputeScatteringFunctions(ray, arena);
    TASK_CHECK(isect.bsdf != nullptr);
    RandomSampler sampler(1, 0);
    for (int i = 0; i < 32; ++i) {
        Point2f uLight((i + .5f) / 32, .43f);
        Point2f uScat(.17f, (i + .5f) / 32);
        Spectrum Ld = EstimateDirect(isect, uScat, *light, uLight, *scene,
                                     sampler, arena);
        TASK_CHECK(Ld[0] == 0 && Ld[1] == 0 && Ld[2] == 0);
        arena.Reset();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Case registry (consumed by task_support/driver.c)

const task_case task_cases[] = {
    {"crop", "cell_weights", TestCropCellWeights},
    {"crop", "partial_mass", TestCropPartialMass},
    {"crop", "normalization", TestCropNormalization},
    {"crop", "overlap_density", TestCropOverlapDensity},
    {"mixture", "pdf_formula", TestMixturePdfFormula},
    {"mixture", "fallback", TestMixtureFallback},
    {"mixture", "factory", TestMixtureFactory},
    {"mixture", "le_unchanged", TestMixtureLeUnchanged},
    {"transport", "direct_mean", TestTransportDirectMean},
    {"transport", "occluded_zero", TestTransportOccludedZero},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
