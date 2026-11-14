
// tests/transient.cpp
//
// GoogleTest coverage for the transient (time-resolved) volumetric path
// tracer.  The three required suites are TransientPulse, TransientTransport
// and TransientFilm; every case ends with TASK_CHECK content assertions and
// no main() is defined (the bundled gtest_main provides one).

#include "task_support/contract.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "pbrt.h"
#include "api.h"
#include "accelerators/bvh.h"
#include "cameras/perspective.h"
#include "filters/box.h"
#include "filters/gaussian.h"
#include "film.h"
#include "integrators/transientvolpath.h"
#include "integrators/volpath.h"
#include "lights/diffuse.h"
#include "lights/point.h"
#include "materials/matte.h"
#include "materials/mirror.h"
#include "media/homogeneous.h"
#include "memory.h"
#include "primitive.h"
#include "samplers/random.h"
#include "scene.h"
#include "shapes/sphere.h"
#include "shapes/triangle.h"
#include "textures/constant.h"
#include "transform.h"
#include "transient.h"

using namespace pbrt;

namespace {

constexpr double kC = 299792458.0;
constexpr double kNs = 1e-9;

// Deterministic sampler: a fixed, repeatable sample stream so transport can
// be hand-checked without Monte Carlo noise.
class FixedSampler : public Sampler {
  public:
    FixedSampler() : Sampler(1) {}
    Float Get1D() override { return .25f; }
    Point2f Get2D() override { return Point2f(.25f, .375f); }
    std::unique_ptr<Sampler> Clone(int) override {
        return std::unique_ptr<Sampler>(new FixedSampler);
    }
};

// Independent piecewise-linear interval mass, written separately from
// TemporalPulse::Integral so the test does not simply re-call the code under
// test.  Nodes (t,v) define a linear spline; returns its integral over
// [a, b).
double ReferenceIntervalMass(const std::vector<double> &t,
                              const std::vector<double> &v, double a,
                              double b) {
    if (!(b > a)) return 0.0;
    double mass = 0.0;
    for (size_t i = 1; i < t.size(); ++i) {
        const double lo = std::max(a, t[i - 1]);
        const double hi = std::min(b, t[i]);
        if (hi <= lo) continue;
        auto h = [&](double x) {
            const double u = (x - t[i - 1]) / (t[i] - t[i - 1]);
            return v[i - 1] + u * (v[i] - v[i - 1]);
        };
        mass += 0.5 * (h(lo) + h(hi)) * (hi - lo);
    }
    return mass;
}

// Build a two-triangle quad of half-extent _r_ at z = _z_.  When
// _facePlusZ_ is set the winding is reversed so the shading normal faces +z
// (toward a light placed on the +z side); the default winding faces -z.
std::shared_ptr<Primitive> MakeQuad(
    const std::shared_ptr<Material> &material,
    const std::shared_ptr<AreaLight> &area, float r, float z,
    const MediumInterface &mi = MediumInterface(), bool facePlusZ = false) {
    static const Transform id;
    int idxFwd[] = {0, 2, 1, 0, 3, 2};
    int idxRev[] = {0, 1, 2, 0, 2, 3};
    const int *idx = facePlusZ ? idxRev : idxFwd;
    Point3f p[] = {{-r, -r, z}, {r, -r, z}, {r, r, z}, {-r, r, z}};
    auto shapes = CreateTriangleMesh(&id, &id, false, 1, idx, 4, p, nullptr,
                                     nullptr, nullptr, nullptr, nullptr);
    return std::make_shared<GeometricPrimitive>(shapes[0], material, area, mi);
}

std::shared_ptr<Material> MakeMatte(Float kd) {
    auto tex = std::make_shared<ConstantTexture<Spectrum>>(Spectrum(kd));
    auto sig = std::make_shared<ConstantTexture<Float>>(0.f);
    return std::make_shared<MatteMaterial>(tex, sig, nullptr);
}

// Relative difference robust to near-zero references.
double RelErr(double a, double b) {
    const double d = std::fabs(a - b);
    const double s = std::max(1e-30, std::fabs(a) + std::fabs(b));
    return 2.0 * d / s;
}

}  // namespace

// =========================================================================
// TransientPulse
// =========================================================================

TEST(TransientPulse, NormalizationAndNodeRules) {
    // Default triangular shape [0,1,2] ns with values [0,1,0], built from
    // seconds.  Trapezoid area = 1 ns; after normalization it is unit area.
    TemporalPulse pulse(std::vector<double>{0.0, 1 * kNs, 2 * kNs},
                        std::vector<double>{0.0, 1.0, 0.0});
    TASK_CHECK(pulse.IsNormalized());
    TASK_CHECK(std::fabs(pulse.Integral(0.0, 2 * kNs) - 1.0) < 1e-12);
    // Outside the support the pulse is zero.
    TASK_CHECK(pulse.Integral(2 * kNs, 5 * kNs) == 0.0);
    TASK_CHECK(pulse.Integral(-1 * kNs, 0.0) == 0.0);
    // Half-open interval ending exactly on a node keeps exact mass.
    TASK_CHECK(std::fabs(pulse.Integral(0.0, 1 * kNs) - 0.5) < 1e-12);

    // Re-scaling the raw shape must not change the normalized total energy.
    TemporalPulse big(std::vector<double>{0.0, 1 * kNs, 2 * kNs},
                      std::vector<double>{0.0, 7.0, 0.0});
    TASK_CHECK(std::fabs(big.Integral(0.0, 2 * kNs) - 1.0) < 1e-12);

    // Invalid constructions throw std::invalid_argument.
    bool threw = false;
    try {
        TemporalPulse bad(std::vector<double>{1.0}, std::vector<double>{1.0});
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try {
        TemporalPulse bad(std::vector<double>{0.0, 1.0, 1.0},
                          std::vector<double>{0.0, 1.0, 0.0});
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try {  // mismatched lengths
        TemporalPulse bad(std::vector<double>{0.0, 1.0, 2.0},
                          std::vector<double>{0.0, 1.0});
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try {  // zero total energy
        TemporalPulse bad(std::vector<double>{0.0, 1.0, 2.0},
                          std::vector<double>{0.0, 0.0, 0.0});
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try {  // negative / non-finite time
        TemporalPulse bad(std::vector<double>{0.0, -1.0, 2.0},
                          std::vector<double>{0.0, 1.0, 0.0});
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);
}

TEST(TransientPulse, ShiftedBinMassMatchesIndependentIntegral) {
    // An asymmetric, multi-node pulse so segment clipping is exercised.
    const std::vector<double> t{0.0, 0.5 * kNs, 1.5 * kNs, 3.0 * kNs};
    const std::vector<double> raw{0.0, 2.0, 1.0, 0.0};
    TemporalPulse pulse(t, raw);

    // Raw normalized node values for the reference integrator.
    double rawArea = ReferenceIntervalMass(t, raw, 0.0, 3 * kNs);
    std::vector<double> nv = raw;
    for (double &x : nv) x /= rawArea;

    const double flight = 1.234 * kNs;  // arbitrary propagation delay
    const double binW = 0.25 * kNs;
    for (int k = 0; k < 20; ++k) {
        const double b0 = k * binW;
        const double b1 = b0 + binW;
        // Shifted mass: h(t - flight) on [b0,b1) == h on [b0-flight,b1-flight)
        const double expected =
            ReferenceIntervalMass(t, nv, b0 - flight, b1 - flight);
        const double got = pulse.ShiftedIntegral(b0, b1, flight);
        TASK_CHECK(RelErr(got, expected) < 1e-12);
    }

    // A delay larger than the support pushes all mass out of an early window.
    TASK_CHECK(pulse.ShiftedIntegral(0.0, 1.0 * kNs, 100 * kNs) == 0.0);
    // A window covering the whole shifted support collects the unit pulse.
    const double full =
        pulse.ShiftedIntegral(-10 * kNs, 100 * kNs, flight);
    TASK_CHECK(std::fabs(full - 1.0) < 1e-12);
}

// =========================================================================
// TransientTransport
// =========================================================================

TEST(TransientTransport, VacuumSurfaceDistanceAndTime) {
    // Camera at origin looks down +z.  Matte wall at z=6, point light at
    // z=3.  Camera->wall = 6 m, wall->light = 3 m: full optical path 9 m.
    auto mat = MakeMatte(.5f);
    auto wall = MakeQuad(mat, nullptr, 2.f, 6.f);
    auto bvh = std::make_shared<BVHAccel>(
        std::vector<std::shared_ptr<Primitive>>{wall});
    auto pulse = std::make_shared<TemporalPulse>(
        std::vector<double>{0.0, kNs, 2 * kNs},
        std::vector<double>{0.0, 1.0, 0.0});
    auto light = std::make_shared<PointLight>(
        Translate(Vector3f(0, 0, 3)), MediumInterface(), Spectrum(100.f),
        pulse);
    Scene scene(bvh, {light});

    FixedSampler sampler;
    MemoryArena arena;
    RayDifferential ray(Ray(Point3f(0, 0, 0), Vector3f(0, 0, 1)));
    auto out = TraceTransient(ray, scene, sampler, arena, 3, 1.f, 1.0);

    TASK_CHECK(!out.empty());
    const double expectedTime = 9.0 / kC;
    bool foundTime = false;
    double total = 0;
    for (const auto &c : out) {
        TASK_CHECK(c.emitter == light.get());
        TASK_CHECK(!c.value.IsBlack());
        if (RelErr(c.flightTime, expectedTime) < 1e-6) foundTime = true;
        total += c.value.y();
    }
    TASK_CHECK(foundTime);
    // Deterministic geometry: the absolute timing error must be well inside
    // the 1e-4 ns steady-comparison target.
    double absNsErr = 1e30;
    for (const auto &c : out)
        absNsErr = std::min(absNsErr,
                            std::fabs(c.flightTime - expectedTime) * 1e9);
    TASK_CHECK(absNsErr < 1e-4);
    TASK_CHECK(total > 0.0);

    // A non-unit-length direction must leave geometry and timing unchanged.
    RayDifferential ray2(Ray(Point3f(0, 0, 0), Vector3f(0, 0, 2.5)));
    FixedSampler sampler2;
    MemoryArena arena2;
    auto out2 = TraceTransient(ray2, scene, sampler2, arena2, 3, 1.f, 1.0);
    TASK_CHECK(out2.size() == out.size());
    for (size_t i = 0; i < out2.size(); ++i)
        TASK_CHECK(RelErr(out2[i].flightTime, out[i].flightTime) < 1e-12);
}

TEST(TransientTransport, MediumBoundaryTransmittanceAndGroupIndex) {
    // Weakly scattering gray homogeneous medium.  sigma_t = 0.02/m makes the
    // fixed 0.25 distance draw (-ln .75 / sigma_t ~= 14.4 m) exceed the 4 m
    // wall distance, so the primary ray reaches the surface and the only
    // attenuation in the direct estimator is the shadow-segment Tr.
    const Float sigA = .01f, sigS = .01f;  // sigma_t = 0.02 / m
    const Float gi = 1.3f;
    HomogeneousMedium medium(Spectrum(sigA), Spectrum(sigS), 0.f, gi);
    const MediumInterface mi(&medium);
    auto mat = MakeMatte(.5f);
    // The wall carries the medium on both sides so shadow rays stay in it.
    auto wall = MakeQuad(mat, nullptr, 2.f, 4.f, mi);
    auto bvh = std::make_shared<BVHAccel>(
        std::vector<std::shared_ptr<Primitive>>{wall});
    auto pulse = std::make_shared<TemporalPulse>(
        std::vector<double>{0.0, kNs, 2 * kNs},
        std::vector<double>{0.0, 1.0, 0.0});
    // Light at z=1, wall at z=4: camera->wall 4 m, wall->light 3 m, total
    // optical path 7 m.
    auto light = std::make_shared<PointLight>(
        Translate(Vector3f(0, 0, 1)), mi, Spectrum(50.f), pulse);
    Scene scene(bvh, {light});

    FixedSampler sampler;
    MemoryArena arena;
    RayDifferential ray(Ray(Point3f(0, 0, 0), Vector3f(0, 0, 1), Infinity, 0.f,
                            &medium));
    // maxDepth 1 keeps only the direct connection at the wall vertex, so the
    // only medium attenuation is the 3 m shadow-segment transmittance.
    auto out = TraceTransient(ray, scene, sampler, arena, 1, 1.f, 1.0);
    TASK_CHECK(!out.empty());

    // Flight time reflects the group index over the full 7 m path.
    const double expectedTime = 7.0 * double(gi) / kC;
    bool timeOk = false;
    for (const auto &c : out)
        if (RelErr(c.flightTime, expectedTime) < 1e-5) timeOk = true;
    TASK_CHECK(timeOk);

    // Vacuum twin with identical geometry and sample stream.  The point
    // light is a delta source (single MIS branch), so the medium/vacuum
    // estimator ratio is exactly the 3 m shadow-segment transmittance.
    auto wallV = MakeQuad(MakeMatte(.5f), nullptr, 2.f, 4.f);
    auto bvhV = std::make_shared<BVHAccel>(
        std::vector<std::shared_ptr<Primitive>>{wallV});
    auto lightV = std::make_shared<PointLight>(
        Translate(Vector3f(0, 0, 1)), MediumInterface(), Spectrum(50.f),
        pulse);
    Scene sceneV(bvhV, {lightV});
    FixedSampler samplerV;
    MemoryArena arenaV;
    RayDifferential rayV(Ray(Point3f(0, 0, 0), Vector3f(0, 0, 1)));
    auto outV = TraceTransient(rayV, sceneV, samplerV, arenaV, 1, 1.f, 1.0);
    TASK_CHECK(!outV.empty());
    double medSum = 0, vacSum = 0;
    for (auto &c : out) medSum += c.value.y();
    for (auto &c : outV) vacSum += c.value.y();
    const double expectedTr = std::exp(-double(sigA + sigS) * 3.0);
    TASK_CHECK(medSum > 0.0 && vacSum > 0.0);
    TASK_CHECK(RelErr(medSum / vacSum, expectedTr) < 1e-4);
}

TEST(TransientTransport, OccludedConnectionContributesNothing) {
    // Camera ray hits a matte wall at z=6; the only light is a point light
    // *behind* that wall at z=9.  The light-sampling shadow connection must
    // be blocked, and a delta point light has no BSDF-sampling MIS branch, so
    // no timed contribution survives.
    auto wall = MakeQuad(MakeMatte(.5f), nullptr, 2.f, 6.f);
    auto bvh = std::make_shared<BVHAccel>(
        std::vector<std::shared_ptr<Primitive>>{wall});
    auto pulse = std::make_shared<TemporalPulse>(
        std::vector<double>{0.0, kNs, 2 * kNs},
        std::vector<double>{0.0, 1.0, 0.0});
    auto light = std::make_shared<PointLight>(
        Translate(Vector3f(0, 0, 9)), MediumInterface(), Spectrum(100.f),
        pulse);
    Scene scene(bvh, {light});

    FixedSampler sampler;
    MemoryArena arena;
    RayDifferential ray(Ray(Point3f(0, 0, 0), Vector3f(0, 0, 1)));
    auto out = TraceTransient(ray, scene, sampler, arena, 2, 1.f, 1.0);
    TASK_CHECK(std::all_of(out.begin(), out.end(),
                           [](const TimedContribution &c) {
                               return c.value.IsBlack();
                           }));
}

TEST(TransientTransport, VolumeScatteringVertexFlightTime) {
    // Gray medium sigma_t = 1/m.  With the fixed 0.25 distance draw, the
    // first scattering event is at z = -ln(0.75).  A point light at z=3
    // makes the total camera->event->light path exactly 3 m.
    HomogeneousMedium medium(Spectrum(.2f), Spectrum(.8f), 0.f, 1.25f);
    auto bvh = std::make_shared<BVHAccel>(
        std::vector<std::shared_ptr<Primitive>>{});
    auto pulse = std::make_shared<TemporalPulse>(
        std::vector<double>{0.0, kNs, 2 * kNs},
        std::vector<double>{0.0, 1.0, 0.0});
    auto light = std::make_shared<PointLight>(
        Translate(Vector3f(0, 0, 3)), MediumInterface(&medium),
        Spectrum(100.f), pulse);
    Scene scene(bvh, {light});

    FixedSampler sampler;
    MemoryArena arena;
    RayDifferential ray(Ray(Point3f(0, 0, 0), Vector3f(0, 0, 1), Infinity, 0.f,
                            &medium));
    auto out = TraceTransient(ray, scene, sampler, arena, 3, 1.f, 1.0);
    const double expectedTime = 3.0 * 1.25 / kC;
    bool timeOk = false;
    double total = 0;
    for (const auto &c : out) {
        total += c.value.y();
        if (RelErr(c.flightTime, expectedTime) < 1e-4) timeOk = true;
    }
    TASK_CHECK(total > 0.0);
    TASK_CHECK(timeOk);
}

TEST(TransientTransport, SpecularBounceHitsAreaLight) {
    // Mirror wall at z=5 reflects the +z camera ray back toward -z, where a
    // two-sided area light sits at z=-3.  Path length = 5 + 8 = 13 m.
    auto kr = std::make_shared<ConstantTexture<Spectrum>>(Spectrum(1.f));
    auto mirror = std::make_shared<MirrorMaterial>(kr, nullptr);
    auto mirrorPrim = MakeQuad(mirror, nullptr, 2.f, 5.f);

    static const Transform id;
    int idx[] = {0, 2, 1, 0, 3, 2};
    Point3f lp[] = {{-1, -1, -3}, {1, -1, -3}, {1, 1, -3}, {-1, 1, -3}};
    auto lampShapes = CreateTriangleMesh(&id, &id, false, 1, idx, 4, lp,
                                         nullptr, nullptr, nullptr, nullptr,
                                         nullptr);
    auto pulse = std::make_shared<TemporalPulse>(
        std::vector<double>{0.0, kNs, 2 * kNs},
        std::vector<double>{0.0, 1.0, 0.0});
    auto area = std::make_shared<DiffuseAreaLight>(
        id, MediumInterface(), Spectrum(8.f), 1, lampShapes[0], true, pulse);
    auto lampPrim = std::make_shared<GeometricPrimitive>(
        lampShapes[0], MakeMatte(.1f), area, MediumInterface());
    auto bvh = std::make_shared<BVHAccel>(
        std::vector<std::shared_ptr<Primitive>>{mirrorPrim, lampPrim});
    Scene scene(bvh, {area});

    FixedSampler sampler;
    MemoryArena arena;
    RayDifferential ray(Ray(Point3f(0, 0, 0), Vector3f(0, 0, 1)));
    auto out = TraceTransient(ray, scene, sampler, arena, 3, 1.f, 1.0);
    const double expectedTime = 13.0 / kC;
    bool emitterHit = false, timeOk = false;
    for (const auto &c : out) {
        if (c.emitter == area.get()) emitterHit = true;
        if (RelErr(c.flightTime, expectedTime) < 1e-4) timeOk = true;
    }
    TASK_CHECK(emitterHit);
    TASK_CHECK(timeOk);
}

TEST(TransientTransport, MisBranchesKeepDistinctEndpointsAndTimes) {
    // A large two-sided emissive sphere surrounds the scene.  On the matte
    // wall vertex at z=2 the light-sampling branch connects to one sampled
    // point on the sphere, while the BSDF-sampling branch shoots a separate
    // cosine-weighted ray to a *different* point on the same sphere.  Both
    // branches therefore reach the area light, and their emitting endpoints
    // (and hence flight times) must differ rather than being copied.
    static Transform id;
    auto sphereShape = std::make_shared<Sphere>(
        &id, &id, false, 50.f, -50.f, 50.f, 360.f);
    auto pulse = std::make_shared<TemporalPulse>(
        std::vector<double>{0.0, kNs, 2 * kNs},
        std::vector<double>{0.0, 1.0, 0.0});
    auto area = std::make_shared<DiffuseAreaLight>(
        id, MediumInterface(), Spectrum(4.f), 1, sphereShape, true, pulse);
    auto lampPrim = std::make_shared<GeometricPrimitive>(
        sphereShape, MakeMatte(.1f), area, MediumInterface());
    // Default -z normal so the camera-side BSDF responds (wo points -z).
    auto wall = MakeQuad(MakeMatte(.5f), nullptr, 1.f, 2.f);
    auto bvh = std::make_shared<BVHAccel>(
        std::vector<std::shared_ptr<Primitive>>{lampPrim, wall});
    Scene scene(bvh, {area});

    FixedSampler sampler;
    MemoryArena arena;
    RayDifferential ray(Ray(Point3f(0, 0, 0), Vector3f(0, 0, 1)));
    auto out = TraceTransient(ray, scene, sampler, arena, 2, 1.f, 1.0);

    std::vector<double> times;
    double sum = 0;
    for (const auto &c : out) {
        if (!c.value.IsBlack() && c.emitter == area.get()) {
            times.push_back(c.flightTime);
            sum += c.value.y();
        }
    }
    TASK_CHECK(times.size() >= 2);
    TASK_CHECK(sum > 0.0);
    // At least two distinct arrival times prove the branches did not share
    // one endpoint distance.
    bool distinct = false;
    for (size_t i = 0; i < times.size() && !distinct; ++i)
        for (size_t j = i + 1; j < times.size(); ++j)
            if (std::fabs(times[i] - times[j]) > 1e-12) distinct = true;
    TASK_CHECK(distinct);
}

TEST(TransientTransport, EmptyBoundarySwitchesMediumAndKeepsTime) {
    // A material-less quad at z=3 is a pure medium boundary between medA
    // (camera side, group index 1) and medB (far side, group index 2).  The
    // camera ray and the wall->light shadow connection both cross it.  The
    // boundary consumes no scattering depth and each segment keeps its own
    // group index.
    HomogeneousMedium medA(Spectrum(.001f), Spectrum(.001f), 0.f, 1.f);
    HomogeneousMedium medB(Spectrum(.001f), Spectrum(.001f), 0.f, 2.f);
    // Default quad normal faces -z.  A +z ray hits the back side, where
    // GetMedium() returns "inside"; so inside = medB, outside = medA.
    const MediumInterface boundaryMI(&medB, &medA);
    static const Transform id;
    int bIdx[] = {0, 2, 1, 0, 3, 2};
    Point3f bP[] = {{-50, -50, 3}, {50, -50, 3}, {50, 50, 3}, {-50, 50, 3}};
    auto bShapes = CreateTriangleMesh(&id, &id, false, 1, bIdx, 4, bP, nullptr,
                                      nullptr, nullptr, nullptr, nullptr);
    auto boundary = std::make_shared<GeometricPrimitive>(
        bShapes[0], nullptr, nullptr, boundaryMI);

    // Opaque matte wall at z=6 (in medB), carrying medB on both sides.
    auto wall = MakeQuad(MakeMatte(.5f), nullptr, 1.f, 6.f,
                         MediumInterface(&medB));
    auto bvh = std::make_shared<BVHAccel>(
        std::vector<std::shared_ptr<Primitive>>{boundary, wall});

    auto pulse = std::make_shared<TemporalPulse>(
        std::vector<double>{0.0, kNs, 2 * kNs},
        std::vector<double>{0.0, 1.0, 0.0});
    // Point light at z=1.5, in medA on the camera side of the boundary, so
    // the wall->light shadow connection crosses the empty boundary back into
    // medA and exercises medium switching on both the camera and shadow rays.
    auto light = std::make_shared<PointLight>(
        Translate(Vector3f(0, 0, 1.5f)), MediumInterface(&medA),
        Spectrum(60.f), pulse);
    Scene scene(bvh, {light});

    FixedSampler sampler;
    MemoryArena arena;
    // Camera ray starts in medA.
    RayDifferential ray(Ray(Point3f(0, 0, 0), Vector3f(0, 0, 1), Infinity, 0.f,
                            &medA));
    // Confirm the empty boundary assigns medB to the spawned +z ray.
    {
        Ray probe(Point3f(0, 0, 0), Vector3f(0, 0, 1), Infinity, 0.f, &medA);
        SurfaceInteraction hit;
        TASK_CHECK(scene.Intersect(probe, &hit));
        Ray after = hit.SpawnRay(probe.d);
        TASK_CHECK(after.medium == &medB);
    }
    auto out = TraceTransient(ray, scene, sampler, arena, 2, 1.f, 1.0);
    TASK_CHECK(!out.empty());

    // Camera->wall: 3 m medA (n=1) + 3 m medB (n=2) = 9 group-metres.
    // Wall->light shadow: wall z6 -> boundary z3 is 3 m medB (n=2), then
    // boundary z3 -> light z1.5 is 1.5 m medA (n=1): 6 + 1.5 = 7.5
    // group-metres.  Total optical path = 9 + 7.5 = 16.5 group-metres.
    const double expectedTime = 16.5 / kC;
    bool timeOk = false;
    for (const auto &c : out) {
        if (RelErr(c.flightTime, expectedTime) < 1e-4) timeOk = true;
    }
    TASK_CHECK(timeOk);
}

TEST(TransientTransport, FullWindowSumMatchesVolPath) {
    // With a time window wide enough to capture the entire (normalized, unit
    // integral) pulse, summing every transient contribution must equal the
    // uniform-one-light volpath estimate under the same sample sequence.
    auto wall = MakeQuad(MakeMatte(.5f), nullptr, 2.f, 6.f);
    auto bvh = std::make_shared<BVHAccel>(
        std::vector<std::shared_ptr<Primitive>>{wall});
    auto pulse = std::make_shared<TemporalPulse>(
        std::vector<double>{0.0, kNs, 2 * kNs},
        std::vector<double>{0.0, 1.0, 0.0});
    auto light = std::make_shared<PointLight>(
        Translate(Vector3f(0, 0, 3)), MediumInterface(), Spectrum(40.f),
        pulse);
    Scene scene(bvh, {light});

    const uint64_t seed = 987654321ULL;
    const int nSamples = 256;
    double transientMean = 0, volpathMean = 0;
    std::vector<double> diffs;
    diffs.reserve(nSamples);
    for (int s = 0; s < nSamples; ++s) {
        RayDifferential ray(Ray(Point3f(0, 0, 0), Vector3f(0, 0, 1)));

        RandomSampler ts(1, int(seed));
        MemoryArena ta;
        auto timed = TraceTransient(ray, scene, ts, ta, 5, 1.f, 1.0);
        Spectrum tSum(0.f);
        for (const auto &c : timed) tSum += c.value;

        auto vs = std::make_shared<RandomSampler>(1, int(seed));
        VolPathIntegrator vp(5, nullptr, vs,
                             Bounds2i(Point2i(0, 0), Point2i(1, 1)), 1,
                             "uniform");
        vp.Preprocess(scene, *vs);
        MemoryArena va;
        Spectrum v = vp.Li(ray, scene, *vs, va, 0);

        transientMean += tSum.y();
        volpathMean += v.y();
        diffs.push_back(double(tSum.y()) - double(v.y()));
    }
    transientMean /= nSamples;
    volpathMean /= nSamples;
    double meanDiff = 0;
    for (double d : diffs) meanDiff += d;
    meanDiff /= nSamples;
    double var = 0;
    for (double d : diffs) var += (d - meanDiff) * (d - meanDiff);
    var /= std::max(1, nSamples - 1);
    const double se = std::sqrt(var / nSamples);
    std::printf(
        "TRANSIENT_VS_VOLPATH seed=%llu samples=%d transient=%.6f "
        "volpath=%.6f meanDiff=%.3e stderr=%.3e\n",
        (unsigned long long)seed, nSamples, transientMean, volpathMean,
        meanDiff, se);
    TASK_CHECK(std::fabs(transientMean) > 0.0);
    TASK_CHECK(RelErr(transientMean, volpathMean) < 2e-4);
}

// =========================================================================
// TransientFilm
// =========================================================================

namespace {

std::shared_ptr<TemporalPulse> TestPulse() {
    return std::make_shared<TemporalPulse>(
        std::vector<double>{0.0, kNs, 2 * kNs},
        std::vector<double>{0.0, 1.0, 0.0});
}

std::unique_ptr<TransientFilm> MakeTestFilm(
    std::unique_ptr<Filter> filter, const Point2i &res = Point2i(4, 4),
    const Bounds2f &crop = Bounds2f(Point2f(0, 0), Point2f(1, 1)),
    int bins = 100, Float binWidth = .1f) {
    return std::unique_ptr<TransientFilm>(new TransientFilm(
        res, crop, std::move(filter), 1., "ignored.csv", 1., bins, 0.f,
        binWidth));
}

}  // namespace

TEST(TransientFilm, SplitContributionSharesOneDenominator) {
    auto film = MakeTestFilm(
        std::unique_ptr<Filter>(new BoxFilter(Vector2f(.5f, .5f))));
    auto light = std::make_shared<PointLight>(
        Transform(), MediumInterface(), Spectrum(1.f), TestPulse());
    auto tile = film->GetFilmTile(film->croppedPixelBounds);

    // One camera sample split into three contributions spanning different
    // bins: the spatial denominator must still be counted once.
    std::vector<TimedContribution> multi;
    multi.emplace_back(Spectrum(1.f), 0.5 * kNs, light.get());
    multi.emplace_back(Spectrum(2.f), 1.0 * kNs, light.get());
    multi.emplace_back(Spectrum(3.f), 1.5 * kNs, light.get());
    tile->AddTransientSample(Point2f(1.5f, 1.5f), multi, 1.f);

    // Same total energy as a single contribution.
    auto tile2 = film->GetFilmTile(film->croppedPixelBounds);
    std::vector<TimedContribution> single;
    single.emplace_back(Spectrum(6.f), 0.5 * kNs, light.get());
    tile2->AddTransientSample(Point2f(1.5f, 1.5f), single, 1.f);

    auto mergeAndWeight = [&](std::unique_ptr<FilmTile> t) {
        film->MergeFilmTile(std::move(t));
    };
    // Compare denominators indirectly: re-read with a fresh second film.
    auto filmA = MakeTestFilm(
        std::unique_ptr<Filter>(new BoxFilter(Vector2f(.5f, .5f))));
    auto ta = filmA->GetFilmTile(filmA->croppedPixelBounds);
    ta->AddTransientSample(Point2f(1.5f, 1.5f), multi, 1.f);
    filmA->MergeFilmTile(std::move(ta));
    auto filmB = MakeTestFilm(
        std::unique_ptr<Filter>(new BoxFilter(Vector2f(.5f, .5f))));
    auto tb = filmB->GetFilmTile(filmB->croppedPixelBounds);
    tb->AddTransientSample(Point2f(1.5f, 1.5f), single, 1.f);
    filmB->MergeFilmTile(std::move(tb));
    (void)mergeAndWeight;

    auto a = filmA->GetTransientRGB(Point2i(1, 1));
    auto b = filmB->GetTransientRGB(Point2i(1, 1));
    double sumA = 0, sumB = 0;
    for (auto &c : a) sumA += c[0] + c[1] + c[2];
    for (auto &c : b) sumB += c[0] + c[1] + c[2];
    // Both samples carry total energy 6 with the same single denominator, so
    // the integrated (sum-over-bin) normalized values match.  The data path
    // goes through float Spectrum/XYZ conversion, hence a float tolerance.
    TASK_CHECK(RelErr(sumA, sumB) < 1e-6);
    TASK_CHECK(sumA > 0.0);
}

TEST(TransientFilm, OutOfWindowAndZeroWeightStillDenominator) {
    auto film = MakeTestFilm(
        std::unique_ptr<Filter>(new BoxFilter(Vector2f(.5f, .5f))));
    auto light = std::make_shared<PointLight>(
        Transform(), MediumInterface(), Spectrum(1.f), TestPulse());
    auto tile = film->GetFilmTile(film->croppedPixelBounds);

    // Flight time far beyond the 10 ns window: all binned numerators are
    // zero, but the denominator is still added once.
    std::vector<TimedContribution> outside;
    outside.emplace_back(Spectrum(5.f), 50 * kNs, light.get());
    tile->AddTransientSample(Point2f(2.5f, 2.5f), outside, 1.f);
    // A zero camera-weight sample likewise adds denominator only.
    tile->AddTransientSample(Point2f(2.5f, 2.5f), outside, 0.f);
    film->MergeFilmTile(std::move(tile));

    auto rgb = film->GetTransientRGB(Point2i(2, 2));
    for (auto &c : rgb)
        TASK_CHECK(c[0] == 0.0 && c[1] == 0.0 && c[2] == 0.0);

    // A later in-window contribution is normalized by both prior samples'
    // weights, proving the denominator accumulated (it is not zero/infinite).
    auto tile2 = film->GetFilmTile(film->croppedPixelBounds);
    std::vector<TimedContribution> inside;
    inside.emplace_back(Spectrum(3.f), 0.5 * kNs, light.get());
    tile2->AddTransientSample(Point2f(2.5f, 2.5f), inside, 1.f);
    film->MergeFilmTile(std::move(tile2));
    auto rgb2 = film->GetTransientRGB(Point2i(2, 2));
    double sum = 0;
    for (auto &c : rgb2) sum += c[0] + c[1] + c[2];
    TASK_CHECK(sum > 0.0 && std::isfinite(sum));
}

TEST(TransientFilm, NonBoxFilterCropAndMultiTile) {
    // Crop to a sub-window and use a Gaussian (non-box) filter; rendering
    // through several tiles must merge to finite, spatially-normalized bins.
    Bounds2f crop(Point2f(.25f, .25f), Point2f(.75f, .75f));
    TransientFilm film(Point2i(8, 8), crop,
                       std::unique_ptr<Filter>(new GaussianFilter(
                           Vector2f(2.f, 2.f), 2.f)),
                       1., "ignored.csv", 2., 50, 0.f, .2f);
    auto light = std::make_shared<PointLight>(
        Transform(), MediumInterface(), Spectrum(1.f), TestPulse());

    // Feed samples through two disjoint tiles that overlap the crop.
    auto t1 = film.GetFilmTile(Bounds2i(Point2i(0, 0), Point2i(4, 4)));
    auto t2 = film.GetFilmTile(Bounds2i(Point2i(4, 4), Point2i(8, 8)));
    std::vector<TimedContribution> c;
    c.emplace_back(Spectrum(2.f), 0.6 * kNs, light.get());
    t1->AddTransientSample(Point2f(2.5f, 2.5f), c, .75f);
    t2->AddTransientSample(Point2f(5.5f, 5.5f), c, .5f);
    film.MergeFilmTile(std::move(t1));
    film.MergeFilmTile(std::move(t2));

    TASK_CHECK(InsideExclusive(Point2i(2, 2), film.croppedPixelBounds));
    auto a = film.GetTransientRGB(Point2i(2, 2));
    auto b = film.GetTransientRGB(Point2i(5, 5));
    TASK_CHECK(a.size() == 50 && b.size() == 50);
    double sa = 0, sb = 0;
    for (auto &x : a) {
        TASK_CHECK(std::isfinite(x[0]) && std::isfinite(x[1]) &&
                   std::isfinite(x[2]));
        sa += x[0] + x[1] + x[2];
    }
    for (auto &x : b) sb += x[0] + x[1] + x[2];
    // Film scale of 2 is applied to both; rayWeight multiplies the numerator.
    TASK_CHECK(sa > 0.0 && sb > 0.0);
    TASK_CHECK(RelErr(sa / .75, sb / .5) < 1e-6);
}

TEST(TransientFilm, ClearZeroesEnergyAndWeights) {
    auto film = MakeTestFilm(
        std::unique_ptr<Filter>(new BoxFilter(Vector2f(.5f, .5f))));
    auto light = std::make_shared<PointLight>(
        Transform(), MediumInterface(), Spectrum(1.f), TestPulse());
    auto tile = film->GetFilmTile(film->croppedPixelBounds);
    std::vector<TimedContribution> c;
    c.emplace_back(Spectrum(2.f), 0.5 * kNs, light.get());
    tile->AddTransientSample(Point2f(1.5f, 1.5f), c, 1.f);
    film->MergeFilmTile(std::move(tile));
    TASK_CHECK(film->GetTransientRGB(Point2i(1, 1)).size() == 100);
    double before = 0;
    for (auto &x : film->GetTransientRGB(Point2i(1, 1)))
        before += x[0] + x[1] + x[2];
    TASK_CHECK(before > 0.0);
    film->Clear();
    double after = 0;
    for (auto &x : film->GetTransientRGB(Point2i(1, 1)))
        after += x[0] + x[1] + x[2];
    TASK_CHECK(after == 0.0);
}

TEST(TransientFilm, CsvMatchesMemoryReadout) {
    const std::string path = "transient_gtest_tmp.csv";
    const int bins = 4;
    TransientFilm film(Point2i(2, 2),
                       Bounds2f(Point2f(0, 0), Point2f(1, 1)),
                       std::unique_ptr<Filter>(
                           new BoxFilter(Vector2f(.5f, .5f))),
                       1., path, 1., bins, 0.f, 1.f);
    auto light = std::make_shared<PointLight>(
        Transform(), MediumInterface(), Spectrum(1.f), TestPulse());
    auto tile = film.GetFilmTile(film.croppedPixelBounds);
    std::vector<TimedContribution> c;
    c.emplace_back(Spectrum(1.5f), 0.5 * kNs, light.get());
    tile->AddTransientSample(Point2f(.5f, .5f), c, 1.f);
    film.MergeFilmTile(std::move(tile));

    // Snapshot the in-memory readout before writing the CSV.
    std::vector<std::vector<std::array<double, 3>>> memory(
        2 * 2, std::vector<std::array<double, 3>>(bins));
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            memory[y * 2 + x] = film.GetTransientRGB(Point2i(x, y));
    film.WriteImage();

    std::ifstream in(path);
    TASK_CHECK(in.good());
    std::string header;
    std::getline(in, header);
    TASK_CHECK(header == "x,y,bin,t0_ns,t1_ns,r,g,b");
    int rows = 0;
    int lastX = -1, lastY = -1, lastBin = -1;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        ++rows;
        std::stringstream ss(line);
        std::vector<std::string> f;
        std::string tok;
        while (std::getline(ss, tok, ',')) f.push_back(tok);
        TASK_CHECK(f.size() == 8);
        const int x = std::stoi(f[0]), y = std::stoi(f[1]),
                  b = std::stoi(f[2]);
        // Rows must be ordered by y, then x, then bin.
        TASK_CHECK(y > lastY ||
                   (y == lastY && (x > lastX ||
                                   (x == lastX && b > lastBin))));
        lastX = x;
        lastY = y;
        lastBin = b;
        const auto &m = memory[y * 2 + x][b];
        TASK_CHECK(RelErr(std::stod(f[5]), m[0]) < 1e-12);
        TASK_CHECK(RelErr(std::stod(f[6]), m[1]) < 1e-12);
        TASK_CHECK(RelErr(std::stod(f[7]), m[2]) < 1e-12);
    }
    TASK_CHECK(rows == 2 * 2 * bins);  // every cropped pixel, every bin
    std::remove(path.c_str());
}

TEST(TransientFilm, ParseSceneAndRenderSmallImage) {
    // Drive the full scene-creation path (parser -> API -> Render -> CSV).
    Options options;
    options.quiet = true;
    const std::string path = "transient_parse_tmp.csv";
    pbrtInit(options);
    const std::string scene =
        "Film \"transient\" \"integer xresolution\" [2] \"integer "
        "yresolution\" [2] \"string filename\" [\"" + path +
        "\"] \"integer bins\" [40] \"float binwidth\" [1]\n"
        "Sampler \"random\" \"integer pixelsamples\" [4]\n"
        "Integrator \"transientvolpath\" \"integer maxdepth\" [2]\n"
        "Camera \"perspective\" \"float fov\" [20]\n"
        "WorldBegin\n"
        "AttributeBegin\nTranslate 0 0 3\n"
        "LightSource \"point\" \"color I\" [50 50 50]\nAttributeEnd\n"
        "AttributeBegin\nMaterial \"matte\" \"color Kd\" [.5 .5 .5]\n"
        "Shape \"trianglemesh\" \"integer indices\" [0 2 1 0 3 2] "
        "\"point P\" [-2 -2 6 2 -2 6 2 2 6 -2 2 6]\nAttributeEnd\n"
        "WorldEnd\n";
    pbrtParseString(scene);
    pbrtCleanup();

    std::ifstream in(path);
    TASK_CHECK(in.good());
    std::string header;
    std::getline(in, header);
    TASK_CHECK(header == "x,y,bin,t0_ns,t1_ns,r,g,b");
    int rows = 0;
    bool anyEnergy = false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        ++rows;
        std::stringstream ss(line);
        std::string tok;
        std::vector<std::string> fields;
        while (std::getline(ss, tok, ',')) fields.push_back(tok);
        TASK_CHECK(fields.size() == 8);
        if (std::stod(fields[5]) + std::stod(fields[6]) +
                std::stod(fields[7]) >
            0)
            anyEnergy = true;
    }
    TASK_CHECK(rows == 2 * 2 * 40);
    TASK_CHECK(anyEnergy);
    std::remove(path.c_str());
}
