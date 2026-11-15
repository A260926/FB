/*
    pbrt source code is Copyright(c) 1998-2016
                        Matt Pharr, Greg Humphreys, and Wenzel Jakob.

    pbrt example task tests: instanced heterogeneous media (type
    "instancegrid").

    These tests exercise the real scene plumbing: a prototype is defined
    inside ObjectBegin, placed with distinct affine transforms, and queried
    through a real BVH, Scene::IntersectTr, VisibilityTester::Tr and
    VolPathIntegrator.  Reference values come from independent world-space
    line integrals and independently accumulated optical thickness, never
    from comparing one new medium method against another.
 */

#include "task_support/check.h"

#include "api.h"
#include "accelerators/bvh.h"
#include "cameras/perspective.h"
#include "film.h"
#include "filters/box.h"
#include "integrators/volpath.h"
#include "interaction.h"
#include "lights/point.h"
#include "materials/matte.h"
#include "media/homogeneous.h"
#include "media/instancegrid.h"
#include "memory.h"
#include "paramset.h"
#include "parallel.h"
#include "primitive.h"
#include "sampler.h"
#include "samplers/random.h"
#include "scene.h"
#include "shapes/sphere.h"
#include "spectrum.h"
#include "textures/constant.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace pbrt;

namespace {

// ---------------------------------------------------------------------------
// Initialization and deterministic sampling.
// ---------------------------------------------------------------------------

// pbrt's parallel machinery and profiling require pbrtInit before any scene
// work; initialize once for the whole process.  One thread keeps the test
// deterministic and still exercises the same code paths (ParallelFor simply
// runs inline).
void EnsurePbrtInit() {
    static bool initialized = false;
    if (!initialized) {
        Options options;
        options.quiet = true;
        options.nThreads = 1;
        pbrtInit(options);
        initialized = true;
    }
}

// A Sampler implementation whose stream is driven by a caller-supplied seed.
// It lets reference integrals and medium queries be interleaved or run
// concurrently while remaining reproducible.  All sample counts and standard
// errors printed by the tests describe these streams.
class EvalSampler : public Sampler {
  public:
    EvalSampler(int64_t count, uint64_t seed) : Sampler(count), rng(seed) {}
    Float Get1D() { return rng.UniformFloat(); }
    Point2f Get2D() {
        return Point2f(rng.UniformFloat(), rng.UniformFloat());
    }
    std::unique_ptr<Sampler> Clone(int seed) {
        return std::unique_ptr<Sampler>(
            new EvalSampler(samplesPerPixel, uint64_t(seed)));
    }

  private:
    RNG rng;
};

uint64_t MixSeed(uint64_t a, uint64_t b) {
    uint64_t h = a * 0x9e3779b97f4a7c15ULL + b;
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    return h;
}

struct Estimate {
    double mean = 0, sumSq = 0, stderr = 0;
    int64_t n = 0;
};

void Accumulate(Estimate *e, double x) {
    e->n++;
    double delta = x - e->mean;
    e->mean += delta / double(e->n);
    e->sumSq += delta * (x - e->mean);
    if (e->n > 1)
        e->stderr = std::sqrt(
            std::max(0.0, e->sumSq / double(e->n - 1) / double(e->n)));
}

// ---------------------------------------------------------------------------
// Prototype/scene construction helpers.
// ---------------------------------------------------------------------------

// A non-symmetric density field on a 4x4x4 grid.  Non-symmetry makes an
// incorrect placement transform visible: the two placements then see
// different densities at the same world point.
std::vector<Float> TestDensity() {
    const int n = 4;
    std::vector<Float> d(n * n * n);
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                d[(z * n + y) * n + x] =
                    0.1f * (x + 1) + 0.05f * y + 0.02f * z;
    return d;
}

ParamSet MediumParams(const std::vector<Float> &density, int n,
                      const Point3f &p0, const Point3f &p1) {
    ParamSet ps;
    {
        std::unique_ptr<Float[]> v(new Float[3]);
        v[0] = v[1] = v[2] = 0.25f;
        ps.AddRGBSpectrum("sigma_a", std::move(v), 3);
    }
    {
        std::unique_ptr<Float[]> v(new Float[3]);
        v[0] = v[1] = v[2] = 0.75f;
        ps.AddRGBSpectrum("sigma_s", std::move(v), 3);
    }
    ps.AddFloat("g", std::unique_ptr<Float[]>(new Float[1]{0.f}), 1);
    ps.AddInt("nx", std::unique_ptr<int[]>(new int[1]{n}), 1);
    ps.AddInt("ny", std::unique_ptr<int[]>(new int[1]{n}), 1);
    ps.AddInt("nz", std::unique_ptr<int[]>(new int[1]{n}), 1);
    {
        std::unique_ptr<Float[]> v(new Float[density.size()]);
        std::memcpy(v.get(), density.data(), density.size() * sizeof(Float));
        ps.AddFloat("density", std::move(v), int(density.size()));
    }
    ps.AddPoint3f("p0", std::unique_ptr<Point3f[]>(new Point3f[1]{p0}), 1);
    ps.AddPoint3f("p1", std::unique_ptr<Point3f[]>(new Point3f[1]{p1}), 1);
    return ps;
}

// Builds a prototype medium exactly as MakeNamedMedium would, including the
// definition transform's role as density->prototype.
std::shared_ptr<InstanceGridMedium> MakePrototype(
    const Transform &definitionTransform = Transform()) {
    std::vector<Float> d = TestDensity();
    ParamSet ps = MediumParams(d, 4, Point3f(-1, -1, -1), Point3f(1, 1, 1));
    std::string error;
    std::shared_ptr<InstanceGridMedium> m =
        CreateInstanceGridMedium(ps, definitionTransform, "smoke", &error);
    if (!m) fprintf(stderr, "prototype creation failed: %s\n", error.c_str());
    return m;
}

std::shared_ptr<Material> MakeMatte() {
    return std::make_shared<MatteMaterial>(
        std::make_shared<ConstantTexture<Spectrum>>(Spectrum(0.5f)),
        std::make_shared<ConstantTexture<Float>>(0.f), nullptr);
}

// The prototype geometry is a closed unit sphere carrying the prototype
// medium interface.  Its density cube is the prototype-space box [-1,1]^3,
// so along the principal axes the sphere and the density volume coincide.
std::shared_ptr<Primitive> MakeSmokeSphere(const MediumInterface &mi,
                                           bool withMaterial) {
    static Transform identity;
    static std::shared_ptr<Shape> sphere = std::make_shared<Sphere>(
        &identity, &identity, false, 1.f, -1.f, 1.f, 360.f);
    std::shared_ptr<Material> material = withMaterial ? MakeMatte() : nullptr;
    return std::make_shared<GeometricPrimitive>(sphere, material, nullptr, mi);
}

// A world-space scene holding one placement of the shared prototype.
struct PlacementScene {
    std::shared_ptr<InstanceGridMedium> prototype;
    std::shared_ptr<InstanceGridMedium> view;
    std::shared_ptr<Primitive> inst;
    std::shared_ptr<Scene> scene;
};

PlacementScene BuildPlacementScene(const Transform &placement,
                                   bool withMaterial) {
    EnsurePbrtInit();
    PlacementScene ps;
    ps.prototype = MakePrototype();
    MediumInterface protoMI(ps.prototype.get(), nullptr);
    std::shared_ptr<Primitive> prim = MakeSmokeSphere(protoMI, withMaterial);
    std::vector<std::shared_ptr<Primitive>> prims{prim};
    std::shared_ptr<Primitive> protoAggregate =
        std::make_shared<BVHAccel>(prims);
    ps.view = InstanceGridMedium::CreatePlacement(
        ps.prototype->GridShared(), placement, "smoke", nullptr);
    std::vector<std::shared_ptr<InstanceGridMedium>> protos{ps.prototype};
    std::vector<std::shared_ptr<InstanceGridMedium>> views{ps.view};
    std::shared_ptr<const MediumRemap> remap =
        CreateInstanceMediumRemap(protos, views);
    AnimatedTransform at(&placement, 0.f, &placement, 1.f);
    ps.inst = std::make_shared<TransformedPrimitive>(protoAggregate, at, remap);
    std::vector<std::shared_ptr<Primitive>> scenePrims{ps.inst};
    ps.scene = std::make_shared<Scene>(
        std::make_shared<BVHAccel>(scenePrims),
        std::vector<std::shared_ptr<Light>>());
    return ps;
}

// Two placements of one prototype, used for identity/disjointness checks.
struct TwoPlacementScene {
    std::shared_ptr<InstanceGridMedium> prototype;
    std::shared_ptr<InstanceGridMedium> viewA, viewB;
    std::shared_ptr<Primitive> protoPrim, protoAggregate;
    std::shared_ptr<Primitive> instA, instB;
    std::shared_ptr<Scene> scene;
    Transform placementA, placementB;
};

TwoPlacementScene BuildTwoPlacementScene(bool withMaterial) {
    EnsurePbrtInit();
    TwoPlacementScene ts;
    ts.prototype = MakePrototype();
    MediumInterface protoMI(ts.prototype.get(), nullptr);
    std::shared_ptr<Primitive> prim = MakeSmokeSphere(protoMI, withMaterial);
    std::vector<std::shared_ptr<Primitive>> prims{prim};
    std::shared_ptr<Primitive> protoAggregate =
        std::make_shared<BVHAccel>(prims);
    ts.protoPrim = prim;
    ts.protoAggregate = protoAggregate;

    ts.placementA = Translate(Vector3f(-1.5f, 0, 0));
    ts.placementB = Translate(Vector3f(1.5f, 0, 0)) *
                    Rotate(20.f, Vector3f(0.3f, 1.f, 0.2f)) *
                    Scale(0.85f, 0.7f, 0.8f);
    ts.viewA = InstanceGridMedium::CreatePlacement(
        ts.prototype->GridShared(), ts.placementA, "smoke", nullptr);
    ts.viewB = InstanceGridMedium::CreatePlacement(
        ts.prototype->GridShared(), ts.placementB, "smoke", nullptr);
    std::vector<std::shared_ptr<InstanceGridMedium>> protos{ts.prototype};
    std::vector<std::shared_ptr<InstanceGridMedium>> viewsA{ts.viewA};
    std::vector<std::shared_ptr<InstanceGridMedium>> viewsB{ts.viewB};
    std::shared_ptr<const MediumRemap> remapA =
        CreateInstanceMediumRemap(protos, viewsA);
    std::shared_ptr<const MediumRemap> remapB =
        CreateInstanceMediumRemap(protos, viewsB);
    AnimatedTransform atA(&ts.placementA, 0.f, &ts.placementA, 1.f);
    AnimatedTransform atB(&ts.placementB, 0.f, &ts.placementB, 1.f);
    ts.instA = std::make_shared<TransformedPrimitive>(protoAggregate, atA,
                                                      remapA);
    ts.instB = std::make_shared<TransformedPrimitive>(protoAggregate, atB,
                                                      remapB);
    std::vector<std::shared_ptr<Primitive>> scenePrims{ts.instA, ts.instB};
    ts.scene = std::make_shared<Scene>(
        std::make_shared<BVHAccel>(scenePrims),
        std::vector<std::shared_ptr<Light>>());
    return ts;
}

// ---------------------------------------------------------------------------
// Independent reference calculations.
// ---------------------------------------------------------------------------

// Independent optical thickness through the density field: evaluate the
// shared immutable density through the placement's inverse transform and
// integrate sigma_t along a world-space straight line with midpoint
// quadrature.  This does not use Medium::Tr or Medium::Sample at all.  The
// integration interval is clipped to the density box exactly as the medium
// clips it, so the reference measures the same path.
double ReferenceOpticalThickness(const InstanceGridMedium *medium,
                                 const Point3f &oWorld,
                                 const Vector3f &dWorld, Float tMax,
                                 int steps = 20000) {
    Transform w2d = Inverse(medium->DensityToWorld());
    const InstanceGridDensity *grid = medium->Grid();
    // Clip the world-space segment to the density cube using the same
    // world-to-density ray the medium uses.
    Float dLen = dWorld.Length();
    if (!(dLen > 0)) return 0;
    Ray r(oWorld, dWorld / dLen, tMax * dLen);
    Ray rd(w2d(oWorld), w2d(dWorld / dLen), tMax * dLen);
    Float t0, t1;
    if (!Bounds3f(Point3f(0, 0, 0), Point3f(1, 1, 1)).IntersectP(rd, &t0, &t1))
        return 0;
    t0 = std::max(t0, (Float)0);
    if (t0 >= t1) return 0;
    double dt = double(t1 - t0) / steps;
    double tau = 0;
    Vector3f dUnit = dWorld / dLen;
    for (int i = 0; i < steps; ++i) {
        double t = t0 + (i + 0.5) * dt;
        Point3f pWorld = oWorld + dUnit * Float(t);
        Float density = grid->Density(rd(Float(t)));
        tau += double(density) * double(grid->SigmaT()) * dt;
    }
    return tau;
}

// ---------------------------------------------------------------------------
// GROUP: boundary
// ---------------------------------------------------------------------------

// Each placement is a closed boundary whose inside interface is that
// placement's own medium and whose outside is vacuum.  The prototype medium
// must never leak into world intersections, and spawned rays must keep the
// placement identity in both directions.
int TestBoundaryIdentity() {
    TwoPlacementScene ts = BuildTwoPlacementScene(true);
    for (int which = 0; which < 2; ++which) {
        const InstanceGridMedium *expected =
            (which == 0) ? ts.viewA.get() : ts.viewB.get();
        const Transform &placement =
            (which == 0) ? ts.placementA : ts.placementB;
        Point3f target = placement(Point3f(0, 0, 0));
        Vector3f dir(0, 0, -1);
        Ray ray(Point3f(target.x, target.y, 2.5f), dir);
        SurfaceInteraction si;
        TASK_CHECK(ts.scene->Intersect(ray, &si));
        TASK_CHECK(si.primitive->GetMaterial() != nullptr);
        TASK_CHECK(si.mediumInterface.inside == (const Medium *)expected);
        TASK_CHECK(si.mediumInterface.outside == nullptr);
        TASK_CHECK(si.mediumInterface.inside != (const Medium *)ts.prototype.get());
        // Continuing along the ray direction stays inside this placement's
        // medium; reversing leaves to vacuum.
        TASK_CHECK(si.SpawnRay(dir).medium == (const Medium *)expected);
        TASK_CHECK(si.SpawnRay(-dir).medium == nullptr);
        TASK_CHECK(si.SpawnRay(dir).time == ray.time);
    }
    // Two placements share one density object and one prototype BVH but keep
    // distinct medium identities.
    TASK_CHECK(ts.viewA.get() != ts.viewB.get());
    TASK_CHECK(ts.viewA->Grid() == ts.viewB->Grid());
    TASK_CHECK(ts.viewA->Grid() == ts.prototype->Grid());
    // The prototype primitive is shared: both instance intersections report
    // the same underlying GeometricPrimitive.
    for (int which = 0; which < 2; ++which) {
        Point3f c = (which == 0) ? Point3f(-1.5f, 0, 0) : Point3f(1.5f, 0, 0);
        Ray ray(Point3f(c.x, c.y, 2.5f), Vector3f(0, 0, -1));
        SurfaceInteraction si;
        TASK_CHECK(ts.scene->Intersect(ray, &si));
        TASK_CHECK(si.primitive == ts.protoPrim.get());
    }
    return 0;
}

// A material-less boundary must be crossed by the same medium-walking logic
// Scene::IntersectTr uses, switching identity at each crossing and never
// selecting the prototype.
int TestBoundaryNoMaterialWalk() {
    TwoPlacementScene ts = BuildTwoPlacementScene(false);
    Ray ray(Point3f(-1.5f, 0.f, 2.f), Vector3f(0, 0, -1));
    int crossings = 0;
    const Medium *seen[4] = {nullptr, nullptr, nullptr, nullptr};
    while (true) {
        SurfaceInteraction si;
        if (!ts.scene->Intersect(ray, &si)) break;
        TASK_CHECK(si.primitive->GetMaterial() == nullptr);
        const Medium *m = si.GetMedium(ray.d);
        TASK_CHECK(m == (const Medium *)ts.viewA.get() ||
                   m == (const Medium *)ts.viewB.get() || m == nullptr);
        if (crossings < 4) seen[crossings] = m;
        ++crossings;
        TASK_CHECK(crossings <= 4);
        ray = si.SpawnRay(ray.d);
    }
    // Entry into A is viewA's inside; exit past the far surface is vacuum.
    TASK_CHECK(crossings == 2);
    TASK_CHECK(seen[0] == (const Medium *)ts.viewA.get());
    TASK_CHECK(seen[1] == nullptr);
    return 0;
}

// The two placements must never shadow or overlap one another.
int TestBoundaryDisjoint() {
    TwoPlacementScene ts = BuildTwoPlacementScene(true);
    for (int which = 0; which < 2; ++which) {
        const InstanceGridMedium *expected =
            (which == 0) ? ts.viewA.get() : ts.viewB.get();
        const InstanceGridMedium *other =
            (which == 0) ? ts.viewB.get() : ts.viewA.get();
        Point3f c = (which == 0) ? Point3f(-1.5f, 0, 0) : Point3f(1.5f, 0, 0);
        Ray ray(Point3f(c.x, c.y, 2.5f), Vector3f(0, 0, -1));
        SurfaceInteraction si;
        TASK_CHECK(ts.scene->Intersect(ray, &si));
        TASK_CHECK(si.mediumInterface.inside == (const Medium *)expected);
        TASK_CHECK(si.mediumInterface.inside != (const Medium *)other);
    }
    Ray between(Point3f(0, 0, 2.5f), Vector3f(0, 0, -1));
    SurfaceInteraction si;
    TASK_CHECK(!ts.scene->Intersect(between, &si));
    return 0;
}

// An ordinary world named medium referenced on the *outside* of a
// boundary keeps its original identity; only instancegrid media are remapped.
// A separate, ordinary homogeneous medium sits outside the placement and the
// interface's outside slot must still point at it after the instance
// intersection is remapped.
int TestBoundaryWorldMediumPreserved() {
    EnsurePbrtInit();
    std::shared_ptr<InstanceGridMedium> prototype = MakePrototype();
    // Ordinary world homogeneous medium.
    std::shared_ptr<HomogeneousMedium> worldMedium =
        std::make_shared<HomogeneousMedium>(Spectrum(0.1f), Spectrum(0.2f),
                                            0.f);
    std::shared_ptr<Primitive> prim =
        MakeSmokeSphere(MediumInterface(prototype.get(), worldMedium.get()),
                        true);
    std::vector<std::shared_ptr<Primitive>> prims{prim};
    std::shared_ptr<Primitive> protoAggregate =
        std::make_shared<BVHAccel>(prims);
    Transform placement = Translate(Vector3f(0.5f, 0, 0));
    std::shared_ptr<InstanceGridMedium> view =
        InstanceGridMedium::CreatePlacement(prototype->GridShared(), placement,
                                            "smoke", nullptr);
    std::vector<std::shared_ptr<InstanceGridMedium>> protos{prototype};
    std::vector<std::shared_ptr<InstanceGridMedium>> views{view};
    std::shared_ptr<const MediumRemap> remap =
        CreateInstanceMediumRemap(protos, views);
    AnimatedTransform at(&placement, 0.f, &placement, 1.f);
    std::shared_ptr<Primitive> inst =
        std::make_shared<TransformedPrimitive>(protoAggregate, at, remap);
    std::vector<std::shared_ptr<Primitive>> scenePrims{inst};
    Scene scene(std::make_shared<BVHAccel>(scenePrims),
                std::vector<std::shared_ptr<Light>>());

    Point3f target = placement(Point3f(0, 0, 0));
    Ray ray(Point3f(target.x, target.y, 2.5f), Vector3f(0, 0, -1));
    SurfaceInteraction si;
    TASK_CHECK(scene.Intersect(ray, &si));
    // Inside is remapped to the placement; outside is the untouched world
    // medium, not the prototype and not a placement view.
    TASK_CHECK(si.mediumInterface.inside == (const Medium *)view.get());
    TASK_CHECK(si.mediumInterface.outside == (const Medium *)worldMedium.get());
    TASK_CHECK(si.mediumInterface.outside != (const Medium *)prototype.get());
    TASK_CHECK(si.SpawnRay(ray.d).medium == (const Medium *)view.get());
    TASK_CHECK(si.SpawnRay(-ray.d).medium ==
               (const Medium *)worldMedium.get());
    return 0;
}

// ---------------------------------------------------------------------------
// GROUP: metric
// ---------------------------------------------------------------------------

// Stretching a smoke must increase optical thickness: density positions are
// sampled through the combined transform, sigma_t stays in inverse world
// length, and the scale is not absorbed and then counted twice.  Tr is
// compared with an independent world-space line integral.
int TestMetricScaleIncreasesOpticalThickness() {
    EnsurePbrtInit();
    std::shared_ptr<InstanceGridMedium> prototype = MakePrototype();
    std::shared_ptr<InstanceGridMedium> mShrink =
        InstanceGridMedium::CreatePlacement(prototype->GridShared(),
                                            Scale(0.5f, 0.5f, 0.5f), "smoke",
                                            nullptr);
    std::shared_ptr<InstanceGridMedium> mStretch =
        InstanceGridMedium::CreatePlacement(prototype->GridShared(),
                                            Scale(2.f, 2.f, 2.f), "smoke",
                                            nullptr);
    TASK_CHECK(mShrink && mStretch);
    double tauShrink = ReferenceOpticalThickness(mShrink.get(), Point3f(0, 0, 0),
                                                 Vector3f(1, 0, 0), 1.f, 4096);
    double tauStretch =
        ReferenceOpticalThickness(mStretch.get(), Point3f(0, 0, 0),
                                  Vector3f(1, 0, 0), 4.f, 16384);
    TASK_CHECK(tauStretch > 1.5 * tauShrink);

    InstanceGridMedium *media[2] = {mShrink.get(), mStretch.get()};
    Float tMaxes[2] = {1.f, 4.f};
    for (int ci = 0; ci < 2; ++ci) {
        InstanceGridMedium *m = media[ci];
        Float tMax = tMaxes[ci];
        Estimate e;
        const int nSamples = 40000;
        for (int i = 0; i < nSamples; ++i) {
            EvalSampler sampler(1, MixSeed(0x6e7a1 + ci, i));
            Ray ray(Point3f(0, 0, 0), Vector3f(1, 0, 0), tMax);
            Accumulate(&e, double(m->Tr(ray, sampler)[0]));
        }
        double expected = std::exp(-ReferenceOpticalThickness(
            m, Point3f(0, 0, 0), Vector3f(1, 0, 0), tMax, 131072));
        double tol = std::max(5.0 * e.stderr, 0.01 * expected) + 1e-3;
        printf("METRIC scale case=%d n=%lld tr=%.5f expected=%.5f "
               "stderr=%.5f tol=%.5f\n",
               ci, (long long)e.n, e.mean, expected, e.stderr, tol);
        TASK_CHECK(std::abs(e.mean - expected) < tol);
    }
    return 0;
}

// Non-unit ray directions and finite tMax are interpreted in world distance:
// scaling the direction must not change Tr, and shortening tMax must
// transmit more light.  Both are checked against independent integrals.
int TestMetricNonUnitDirectionAndTMax() {
    EnsurePbrtInit();
    std::shared_ptr<InstanceGridMedium> prototype = MakePrototype();
    Transform placement =
        Translate(Vector3f(0.3f, -0.2f, 0.1f)) * Scale(1.3f, 0.9f, 1.1f);
    std::shared_ptr<InstanceGridMedium> m = InstanceGridMedium::CreatePlacement(
        prototype->GridShared(), placement, "smoke", nullptr);
    TASK_CHECK(m != nullptr);
    Point3f o = placement(Point3f(-0.9f, 0.1f, -0.1f));
    Vector3f dUnit = Normalize(placement(Point3f(1, 0.2f, 0)) -
                               placement(Point3f(0, 0, 0)));
    Float full = 2.4f;

    Estimate eUnit, eScaled, eShort;
    const int n = 40000;
    for (int i = 0; i < n; ++i) {
        {
            EvalSampler s(1, MixSeed(0x6e7a2, i));
            Accumulate(&eUnit,
                       double(m->Tr(Ray(o, dUnit, full), s)[0]));
        }
        {
            EvalSampler s(1, MixSeed(0x6e7a2, i));
            Accumulate(&eScaled,
                       double(m->Tr(Ray(o, dUnit * 3.7f, full), s)[0]));
        }
        {
            EvalSampler s(1, MixSeed(0x6e7a2, i));
            Accumulate(&eShort,
                       double(m->Tr(Ray(o, dUnit, full * 0.35f), s)[0]));
        }
    }
    TASK_CHECK(std::abs(eUnit.mean - eScaled.mean) <
               4.0 * std::sqrt(eUnit.stderr * eUnit.stderr +
                               eScaled.stderr * eScaled.stderr) +
                   0.004);
    TASK_CHECK(eShort.mean > eUnit.mean + 0.01);
    double refFull = std::exp(
        -ReferenceOpticalThickness(m.get(), o, dUnit, full, 131072));
    double refShort = std::exp(-ReferenceOpticalThickness(
        m.get(), o, dUnit, full * 0.35f, 131072));
    TASK_CHECK(std::abs(eUnit.mean - refFull) <
               std::max(5.0 * eUnit.stderr, 0.01 * refFull) + 1e-3);
    TASK_CHECK(std::abs(eShort.mean - refShort) <
               std::max(5.0 * eShort.stderr, 0.01 * refShort) + 1e-3);
    printf("METRIC direction n=%lld unit=%.5f scaled=%.5f short=%.5f "
           "ref=%.5f refShort=%.5f stderr=%.5f\n",
           (long long)eUnit.n, eUnit.mean, eScaled.mean, eShort.mean, refFull,
           refShort, eUnit.stderr);
    return 0;
}

// Trilinear interpolation at voxel centers, half-voxel midpoints and grid
// edges matches the shared grid definition; the shared density is never
// rewritten and every placement sees the same array.
int TestMetricHalfVoxelAndSharedDensity() {
    EnsurePbrtInit();
    std::shared_ptr<InstanceGridMedium> prototype = MakePrototype();
    std::vector<Float> original = TestDensity();
    Transform placement = Translate(Vector3f(0.7f, -0.4f, 0.25f)) *
                          Scale(0.8f, 1.4f, 1.0f);
    std::shared_ptr<InstanceGridMedium> m = InstanceGridMedium::CreatePlacement(
        prototype->GridShared(), placement, "smoke", nullptr);
    TASK_CHECK(m != nullptr);
    const InstanceGridDensity *grid = m->Grid();
    const int n = 4;
    // Voxel centers: interpolant equals the stored voxel value.
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                Point3f u((x + 0.5f) / n, (y + 0.5f) / n, (z + 0.5f) / n);
                Float got = m->DensityWorld(m->DensityToWorld()(u));
                Float expected = original[(z * n + y) * n + x];
                TASK_CHECK(std::abs(got - expected) < 2e-4);
            }
    // Half-voxel midpoint between two centers is the average of the values.
    Point3f ua(0.5f / n, 0.5f / n, 0.5f / n);
    Point3f ub(1.5f / n, 0.5f / n, 0.5f / n);
    Point3f um(1.0f / n, 0.5f / n, 0.5f / n);
    Float da = m->DensityWorld(m->DensityToWorld()(ua));
    Float db = m->DensityWorld(m->DensityToWorld()(ub));
    Float dm = m->DensityWorld(m->DensityToWorld()(um));
    TASK_CHECK(std::abs(dm - 0.5f * (da + db)) < 2e-4);
    // Grid edges clamp to the boundary values and stay finite / non-negative.
    for (int axis = 0; axis < 3; ++axis) {
        for (Float edge : {0.f, 1.f}) {
            Point3f u(0.5f, 0.5f, 0.5f);
            u[axis] = edge;
            Float d = m->DensityWorld(m->DensityToWorld()(u));
            TASK_CHECK(std::isfinite(d) && d >= 0);
        }
    }
    // The shared array is untouched by any of the above.
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                TASK_CHECK(std::abs(grid->At(x, y, z) -
                                    original[(z * n + y) * n + x]) < 1e-6);
    TASK_CHECK(prototype->Grid() == grid);
    return 0;
}

// Sheared and reflected non-uniform placements sample the combined transform
// correctly, and the prototype and a sibling instance are never modified.
int TestMetricShearReflection() {
    EnsurePbrtInit();
    std::shared_ptr<InstanceGridMedium> prototype = MakePrototype();
    std::vector<Float> original = TestDensity();

    Float shearM[4][4] = {{1.f, 0.35f, 0.f, 0.1f},
                          {0.f, 1.f, 0.2f, -0.2f},
                          {0.f, 0.f, 1.3f, 0.f},
                          {0.f, 0.f, 0.f, 1.f}};
    Transform shear(shearM);
    Transform reflect = Scale(-1.1f, 0.9f, 1.2f);

    struct Case {
        const char *name;
        Transform placement;
        bool reflected;
    } cases[2] = {{"shear", shear, false}, {"reflect", reflect, true}};

    // A sibling instance built before the probes, to detect cross-writes.
    std::shared_ptr<InstanceGridMedium> sibling =
        InstanceGridMedium::CreatePlacement(prototype->GridShared(),
                                            Scale(0.6f, 0.6f, 0.6f), "smoke",
                                            nullptr);
    TASK_CHECK(sibling != nullptr);

    for (int ci = 0; ci < 2; ++ci) {
        std::shared_ptr<InstanceGridMedium> m =
            InstanceGridMedium::CreatePlacement(prototype->GridShared(),
                                                cases[ci].placement, "smoke",
                                                nullptr);
        TASK_CHECK(m != nullptr);
        RNG rng(0x5eed + ci);
        for (int i = 0; i < 200; ++i) {
            Point3f u(rng.UniformFloat(), rng.UniformFloat(),
                      rng.UniformFloat());
            Point3f pWorld = m->DensityToWorld()(u);
            Float got = m->DensityWorld(pWorld);
            Float expected = prototype->Grid()->Density(u);
            TASK_CHECK(std::abs(got - expected) < 1e-4);
            TASK_CHECK(std::isfinite(got) && got >= 0);
        }
        if (cases[ci].reflected) {
            // Reflection must map density coordinates through the mirrored
            // transform, so the asymmetric field reads different values at
            // mirrored parameter positions.
            Point3f ua(0.2f, 0.4f, 0.6f), ub(0.8f, 0.4f, 0.6f);
            Float da = m->DensityWorld(m->DensityToWorld()(ua));
            Float db = m->DensityWorld(m->DensityToWorld()(ub));
            TASK_CHECK(std::abs(da - prototype->Grid()->Density(ua)) < 1e-4);
            TASK_CHECK(std::abs(db - prototype->Grid()->Density(ub)) < 1e-4);
            TASK_CHECK(std::abs(da - db) > 1e-3);
        }
        // Neither the prototype nor the sibling were rewritten.
        for (int z = 0; z < 4; ++z)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    TASK_CHECK(std::abs(m->Grid()->At(x, y, z) -
                                        original[(z * 4 + y) * 4 + x]) < 1e-6);
                    TASK_CHECK(std::abs(sibling->Grid()->At(x, y, z) -
                                        original[(z * 4 + y) * 4 + x]) < 1e-6);
                }
        TASK_CHECK(sibling->Grid() == prototype->Grid());
    }
    return 0;
}

// A homogeneous density field has a closed-form world-space transmittance, so
// it pins down the world-length metric absolutely (not via quadrature):
//   sigma_t = sigma_a + sigma_s = 1, density = 1
// Trilinear interpolation of a constant field is exactly 1 in the interior of
// the grid and tapers over the half voxel adjacent to each face (the same
// behaviour as GridDensityMedium).  Restricting the segment to the interior
// voxel range therefore gives Tr = exp(-sigma_t * worldLength) exactly, with
// worldLength = (grid world length) * (1 - 1/n).  Stretching the grid by s
// must scale worldLength by s, so Tr = exp(-s * L), which would be wrong if
// the scale were also folded into sigma_t.
int TestMetricHomogeneousAnalytic() {
    EnsurePbrtInit();
    // Unit density on an 8x8x8 grid spanning [-1,1]^3 in grid coordinates.
    const int n = 8;
    std::vector<Float> d(n * n * n, 1.0f);
    ParamSet ps = MediumParams(d, n, Point3f(-1, -1, -1), Point3f(1, 1, 1));
    std::string error;
    std::shared_ptr<InstanceGridMedium> prototype =
        CreateInstanceGridMedium(ps, Transform(), "uniform", &error);
    TASK_CHECK(prototype != nullptr);
    TASK_CHECK(prototype->Grid()->SigmaT() == 1.f);

    struct Case {
        Float scale;
    };
    // World-space mapping of a normalized density coordinate u along x is
    // x(u) = -s + u * 2s.  Sampling only the interior voxel range
    // u in [0.5/n, 1 - 0.5/n] keeps the interpolant exactly 1, so the world
    // length is 2s * (1 - 1/n) and Tr = exp(-(1 - 1/n) * 2s).
    Case cases[2] = {{2.f}, {0.5f}};
    for (int ci = 0; ci < 2; ++ci) {
        Float s = cases[ci].scale;
        double u0 = 0.5 / n, u1 = 1.0 - 0.5 / n;
        double worldLen = (u1 - u0) * 2.0 * s;
        double expected = std::exp(-worldLen);
        std::shared_ptr<InstanceGridMedium> m =
            InstanceGridMedium::CreatePlacement(
                prototype->GridShared(), Scale(s, s, s), "uniform", &error);
        TASK_CHECK(m != nullptr);
        Point3f o = m->DensityToWorld()(Point3f(Float(u0), 0.5f, 0.5f));
        Float tMax = Float(worldLen);
        TASK_CHECK(tMax > 0);
        Estimate e;
        const int samples = 30000;
        for (int i = 0; i < samples; ++i) {
            EvalSampler sampler(1, MixSeed(0x0a17 + ci, i));
            Ray ray(o, Vector3f(1, 0, 0), tMax);
            Accumulate(&e, double(m->Tr(ray, sampler)[0]));
        }
        double tol = 6.0 * e.stderr + 0.006;
        printf("METRIC analytic scale=%g n=%lld tr=%.5f exact=%.5f "
               "stderr=%.5f tol=%.5f\n",
               s, (long long)e.n, e.mean, expected, e.stderr, tol);
        TASK_CHECK(std::abs(e.mean - expected) < tol);
    }
    return 0;
}

// Invalid density values, dimension overflow, singular matrices and
// non-finite transforms are rejected with an explicit error string.
int TestMetricValidation() {
    EnsurePbrtInit();
    std::vector<Float> d = TestDensity();
    std::string error;

    auto expectDefinitionFailure = [&](const ParamSet &ps,
                                       const Transform &definition) {
        error.clear();
        TASK_CHECK(
            !CreateInstanceGridMedium(ps, definition, "bad", &error));
        TASK_CHECK(!error.empty());
    };

    {  // negative density
        std::vector<Float> bad = d;
        bad[5] = -0.25f;
        expectDefinitionFailure(
            MediumParams(bad, 4, Point3f(-1, -1, -1), Point3f(1, 1, 1)),
            Transform());
    }
    {  // NaN density
        std::vector<Float> bad = d;
        bad[0] = std::numeric_limits<Float>::quiet_NaN();
        expectDefinitionFailure(
            MediumParams(bad, 4, Point3f(-1, -1, -1), Point3f(1, 1, 1)),
            Transform());
    }
    {  // density count mismatch
        ParamSet ps = MediumParams(d, 4, Point3f(-1, -1, -1),
                                   Point3f(1, 1, 1));
        ps.AddInt("nx", std::unique_ptr<int[]>(new int[1]{5}), 1);
        expectDefinitionFailure(ps, Transform());
    }
    {  // dimension overflow
        ParamSet ps = MediumParams(d, 4, Point3f(-1, -1, -1),
                                   Point3f(1, 1, 1));
        ps.AddInt("nx", std::unique_ptr<int[]>(new int[1]{1 << 20}), 1);
        ps.AddInt("ny", std::unique_ptr<int[]>(new int[1]{1 << 20}), 1);
        ps.AddInt("nz", std::unique_ptr<int[]>(new int[1]{1 << 20}), 1);
        expectDefinitionFailure(ps, Transform());
    }
    {  // degenerate density box
        expectDefinitionFailure(
            MediumParams(d, 4, Point3f(1, -1, -1), Point3f(1, 1, 1)),
            Transform());
    }
    {  // singular definition transform (collapses z)
        expectDefinitionFailure(
            MediumParams(d, 4, Point3f(-1, -1, -1), Point3f(1, 1, 1)),
            Scale(1.f, 1.f, 0.f));
    }
    {  // non-finite definition transform entry
        Float nan = std::numeric_limits<Float>::quiet_NaN();
        Float badM[4][4] = {{1, 0, 0, nan}, {0, 1, 0, 0},
                            {0, 0, 1, 0},   {0, 0, 0, 1}};
        error.clear();
        TASK_CHECK(!ValidateInstanceTransform(Transform(badM), &error));
        TASK_CHECK(!error.empty());
    }
    {  // singular and non-finite placements
        std::shared_ptr<InstanceGridMedium> prototype = MakePrototype();
        error.clear();
        TASK_CHECK(!InstanceGridMedium::CreatePlacement(
            prototype->GridShared(), Scale(0.f, 1.f, 1.f), "bad", &error));
        TASK_CHECK(!error.empty());
        Float nan = std::numeric_limits<Float>::quiet_NaN();
        Float badM[4][4] = {{1, 0, 0, nan}, {0, 1, 0, 0},
                            {0, 0, 1, 0},   {0, 0, 0, 1}};
        error.clear();
        TASK_CHECK(!InstanceGridMedium::CreatePlacement(
            prototype->GridShared(), Transform(badM), "bad", &error));
        TASK_CHECK(!error.empty());
    }
    {  // a valid configuration still succeeds
        error.clear();
        ParamSet ps = MediumParams(d, 4, Point3f(-1, -1, -1),
                                   Point3f(1, 1, 1));
        TASK_CHECK(CreateInstanceGridMedium(ps, Transform(), "good", &error) !=
                   nullptr);
        TASK_CHECK(error.empty());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// GROUP: transport
// ---------------------------------------------------------------------------

// Scene::IntersectTr through a material-less closed boundary: the sampled
// transmittance matches an independent world-space line integral, and the
// placement identity is the one the camera ray entered.
int TestTransportIntersectTr() {
    TwoPlacementScene ts = BuildTwoPlacementScene(false);
    Point3f o(-1.5f, 0.f, 2.f);
    Vector3f d(0, 0, -1);
    Float tMax = 3.f;
    Estimate sampled, reference;
    const int n = 3000;
    for (int i = 0; i < n; ++i) {
        EvalSampler sampler(1, MixSeed(0x7a1, i));
        SurfaceInteraction si;
        Spectrum tr;
        TASK_CHECK(!ts.scene->IntersectTr(Ray(o, d, tMax), sampler, &si, &tr));
        Accumulate(&sampled, double(tr[0]));
        double tau =
            ReferenceOpticalThickness(ts.viewA.get(), o, d, tMax, 131072);
        Accumulate(&reference, std::exp(-tau));
    }
    TASK_CHECK(std::abs(sampled.mean - reference.mean) <
               std::max(5.0 * sampled.stderr, 0.01 * reference.mean) + 1e-3);
    printf("TRANSPORT intersectTr n=%lld tr=%.5f independent=%.5f "
           "stderr=%.5f\n",
           (long long)sampled.n, sampled.mean, reference.mean,
           sampled.stderr);
    return 0;
}

// VisibilityTester::Tr across a closed boundary uses the placement's medium
// and matches an independent line integral.  The placement is axis-aligned so
// the spherical boundary and the density volume coincide along the chord.
int TestTransportVisibilityTr() {
    Transform placement = Translate(Vector3f(1.5f, 0, 0)) *
                          Scale(1.2f, 1.0f, 1.4f);
    PlacementScene ps = BuildPlacementScene(placement, false);
    Point3f p0(1.5f, 0.f, 2.5f), p1(1.5f, 0.f, -2.5f);
    Estimate sampled, reference;
    const int n = 3000;
    for (int i = 0; i < n; ++i) {
        EvalSampler sampler(1, MixSeed(0x7a2, i));
        Interaction a(p0, 0.f, MediumInterface((const Medium *)nullptr));
        Interaction b(p1, 0.f, MediumInterface((const Medium *)nullptr));
        VisibilityTester vis(a, b);
        Accumulate(&sampled, double(vis.Tr(*ps.scene, sampler)[0]));
        double tau = ReferenceOpticalThickness(
            ps.view.get(), p0, Normalize(p1 - p0), Distance(p0, p1), 131072);
        Accumulate(&reference, std::exp(-tau));
    }
    TASK_CHECK(std::abs(sampled.mean - reference.mean) <
               std::max(5.0 * sampled.stderr, 0.015 * reference.mean) + 1e-3);
    printf("TRANSPORT visibilityTr n=%lld tr=%.5f independent=%.5f "
           "stderr=%.5f\n",
           (long long)sampled.n, sampled.mean, reference.mean,
           sampled.stderr);
    return 0;
}

// Medium::Sample returns a world-space position and phase direction, the
// original time, and this placement's identity; GetMedium()/SpawnRay keep it.
// The first-scattering distribution is checked against an independently
// accumulated optical thickness (its cumulative CDF), not against Tr.
int TestTransportFirstScatterDistribution() {
    EnsurePbrtInit();
    std::shared_ptr<InstanceGridMedium> prototype = MakePrototype();
    Transform placement =
        Translate(Vector3f(-0.4f, 0.2f, 0.f)) * Scale(1.4f, 0.8f, 1.f);
    std::shared_ptr<InstanceGridMedium> m = InstanceGridMedium::CreatePlacement(
        prototype->GridShared(), placement, "smoke", nullptr);
    TASK_CHECK(m != nullptr);
    Point3f o = placement(Point3f(0.5f, 0.5f, -0.05f));
    Vector3f d = Normalize(placement(Point3f(0.5f, 0.5f, 1.f)) -
                           placement(Point3f(0.5f, 0.5f, 0.f)));
    Float time = 0.37f;
    Float tMax = 3.0f;

    // Independent cumulative optical thickness along the same world line.
    const int steps = 16384;
    Transform w2d = Inverse(m->DensityToWorld());
    std::vector<double> cum(steps + 1, 0.0);
    double dt = double(tMax) / steps;
    for (int i = 0; i < steps; ++i) {
        Point3f p = o + d * Float((i + 0.5) * dt);
        cum[i + 1] = cum[i] + double(m->Grid()->Density(w2d(p))) *
                                  double(m->Grid()->SigmaT()) * dt;
    }
    double totalTau = cum[steps];
    TASK_CHECK(totalTau > 0.1);

    const int n = 4000;
    int events = 0;
    std::vector<int> eventBin(steps + 1, 0);
    Estimate tauAtEvent;
    for (int i = 0; i < n; ++i) {
        EvalSampler sampler(1, MixSeed(0x7a3, i));
        MemoryArena arena;
        Ray ray(o, d, tMax, time);
        MediumInteraction mi;
        Spectrum weight = m->Sample(ray, sampler, arena, &mi);
        TASK_CHECK(!weight.IsBlack());
        if (mi.IsValid()) {
            ++events;
            // On a scattering event the medium is spectrally uniform, so
            // every channel of the scattering weight is the albedo
            // sigma_s/sigma_t.
            Float albedo =
                m->Grid()->SigmaS()[0] / m->Grid()->SigmaT();
            TASK_CHECK(std::abs(weight[0] - albedo) < 1e-5f);
            TASK_CHECK(std::abs(weight[1] - albedo) < 1e-5f);
            TASK_CHECK(std::abs(weight[2] - albedo) < 1e-5f);
            // World-space position on the ray.
            Float t = Dot(Vector3f(mi.p - o), d) / d.LengthSquared();
            TASK_CHECK(t > 0 && t < tMax);
            TASK_CHECK(Distance(o + d * t, mi.p) < 1e-3);
            TASK_CHECK(mi.time == time);
            TASK_CHECK(mi.GetMedium() == (const Medium *)m.get());
            TASK_CHECK(mi.GetMedium() != (const Medium *)prototype.get());
            // Phase sampling produces a unit world-space direction, and the
            // subsequent spawn ray stays in this placement's medium.
            Vector3f wi;
            mi.phase->Sample_p(mi.wo, &wi, Point2f(0.3f, 0.7f));
            TASK_CHECK(std::abs(wi.Length() - 1.f) < 1e-3);
            Ray spawned = mi.SpawnRay(wi);
            TASK_CHECK(spawned.medium == (const Medium *)m.get());
            TASK_CHECK(spawned.time == time);
            int bin = std::min(steps, std::max(0, int(t / dt)));
            eventBin[bin]++;
            double frac = double(t) / dt;
            int bi = std::min(steps - 1, int(frac));
            double w = frac - bi;
            Accumulate(&tauAtEvent, cum[bi] * (1 - w) + cum[bi + 1] * w);
        }
        arena.Reset();
    }
    TASK_CHECK(events > n / 20);
    double pNoScatter = std::exp(-totalTau);
    double measuredRate = double(events) / n;
    double stderrRate = std::sqrt(measuredRate * (1 - measuredRate) / n);
    TASK_CHECK(std::abs(measuredRate - (1 - pNoScatter)) <
               5.0 * stderrRate + 0.01);

    // Compare the first-scatter depth distribution with the independent CDF
    // exp(-tau) in coarse bins.
    double chi2 = 0;
    int bins = 0;
    const int binCount = 64;
    for (int b = 0; b < binCount; ++b) {
        int lo = b * steps / binCount;
        int hi = (b + 1) * steps / binCount;
        double pRef = std::exp(-cum[lo]) - std::exp(-cum[hi]);
        double measured = 0;
        for (int k = lo; k < hi; ++k) measured += eventBin[k];
        measured /= n;
        double var = std::max(pRef * (1 - pRef) / n, 1e-9);
        chi2 += (measured - pRef) * (measured - pRef) / var;
        ++bins;
    }
    TASK_CHECK(chi2 < 4.0 * bins + 40);
    printf("TRANSPORT firstScatter n=%lld events=%d rate=%.4f expected=%.4f "
           "meanTauAtEvent=%.4f totalTau=%.4f bins=%d chi2=%.1f\n",
           (long long)n, events, measuredRate, 1 - pNoScatter, tauAtEvent.mean,
           totalTau, bins, chi2);
    return 0;
}

// VolPathIntegrator must return non-black radiance for paths that cross each
// placement, using the real integrator and a real point light.
int TestTransportVolPath() {
    EnsurePbrtInit();
    TwoPlacementScene ts = BuildTwoPlacementScene(true);
    Point2i resolution(8, 8);
    MemoryArena arena;
    int nonBlack = 0;
    double total = 0;
    {
        std::vector<std::shared_ptr<Light>> lights;
        lights.push_back(std::make_shared<PointLight>(
            Translate(Vector3f(0.f, 3.f, 1.f)), MediumInterface(),
            Spectrum(60.f)));
        std::vector<std::shared_ptr<Primitive>> prims{ts.instA, ts.instB};
        std::shared_ptr<Primitive> aggregate = std::make_shared<BVHAccel>(prims);
        std::shared_ptr<Scene> scene =
            std::make_shared<Scene>(aggregate, lights);

        // The camera takes ownership of the film.
        Film *film = new Film(resolution, Bounds2f(Point2f(0, 0), Point2f(1, 1)),
                              std::unique_ptr<Filter>(
                                  new BoxFilter(Vector2f(0.5f, 0.5f))),
                              1.f, "", 1.f);
        Transform camT = Translate(Vector3f(0, 0, -4));
        AnimatedTransform camToWorld(&camT, 0.f, &camT, 1.f);
        std::shared_ptr<PerspectiveCamera> camera =
            std::make_shared<PerspectiveCamera>(
                camToWorld, Bounds2f(Point2f(-1, -1), Point2f(1, 1)), 0.f,
                1.f, 0.f, 1e6f, 45.f, film, nullptr);
        std::shared_ptr<Sampler> sampler =
            std::make_shared<RandomSampler>(16, 7);
        VolPathIntegrator integrator(4, camera, sampler,
                                     Bounds2i(Point2i(0, 0), resolution));
        integrator.Preprocess(*scene, *sampler);
        for (int y = 0; y < resolution.y; ++y)
            for (int x = 0; x < resolution.x; ++x) {
                sampler->StartPixel(Point2i(x, y));
                for (int s = 0; s < 16; ++s) {
                    CameraSample cs;
                    cs.pFilm = Point2f(Float(x) + 0.5f, Float(y) + 0.5f);
                    cs.time = 0.5f;
                    cs.pLens = Point2f(0.5f, 0.5f);
                    RayDifferential ray;
                    camera->GenerateRayDifferential(cs, &ray);
                    Spectrum L =
                        integrator.Li(ray, *scene, *sampler, arena, 0);
                    if (L.MaxComponentValue() > 1e-5f) ++nonBlack;
                    total += L.MaxComponentValue();
                    sampler->StartNextSample();
                    arena.Reset();
                }
            }
    }
    TASK_CHECK(nonBlack > 0);
    printf("TRANSPORT volpath nonBlack=%d of %d totalRadiance=%.4f\n",
           nonBlack, resolution.x * resolution.y * 16, total);
    return 0;
}

// Interleaved queries over both placements must always report the identity of
// the placement actually hit, and never the prototype or the sibling.
int TestTransportInterleavedIdentity() {
    TwoPlacementScene ts = BuildTwoPlacementScene(true);
    for (int i = 0; i < 500; ++i) {
        int which = i & 1;
        const InstanceGridMedium *expected =
            (which == 0) ? ts.viewA.get() : ts.viewB.get();
        Point3f c = (which == 0) ? Point3f(-1.5f, 0, 0) : Point3f(1.5f, 0, 0);
        Ray ray(Point3f(c.x, c.y, 2.5f), Vector3f(0, 0, -1));
        SurfaceInteraction si;
        TASK_CHECK(ts.scene->Intersect(ray, &si));
        TASK_CHECK(si.mediumInterface.inside == (const Medium *)expected);
        TASK_CHECK(si.mediumInterface.inside != (const Medium *)ts.prototype.get());
        TASK_CHECK(si.mediumInterface.outside != (const Medium *)ts.prototype.get());
    }
    return 0;
}

// Concurrent queries over one read-only scene and shared density agree with
// the single-threaded result and never corrupt the shared array.
int TestTransportConcurrent() {
    EnsurePbrtInit();
    std::shared_ptr<InstanceGridMedium> prototype = MakePrototype();
    std::vector<Float> original = TestDensity();
    Transform placement =
        Translate(Vector3f(0.1f, -0.2f, 0.3f)) * Scale(1.2f, 1.3f, 0.9f);
    std::shared_ptr<InstanceGridMedium> m = InstanceGridMedium::CreatePlacement(
        prototype->GridShared(), placement, "smoke", nullptr);
    TASK_CHECK(m != nullptr);
    Point3f o = placement(Point3f(0.1f, 0.1f, -0.2f));
    Vector3f d = Normalize(placement(Point3f(0.2f, 0.3f, 1)) -
                           placement(Point3f(0, 0, 0)));
    Float tMax = 2.2f;

    Estimate ref;
    for (int i = 0; i < 4000; ++i) {
        EvalSampler s(1, MixSeed(0xc0ffee, i));
        Accumulate(&ref, double(m->Tr(Ray(o, d, tMax), s)[0]));
    }

    const int nThreads = 8;
    const int perThread = 4000;
    std::vector<Estimate> results(nThreads);
    std::vector<int> failures(nThreads, 0);
    std::vector<std::thread> threads;
    for (int t = 0; t < nThreads; ++t)
        threads.push_back(std::thread([&, t]() {
            for (int i = 0; i < perThread; ++i) {
                EvalSampler s(1, MixSeed(0xc0ffee, i));
                Float tr = m->Tr(Ray(o, d, tMax), s)[0];
                if (!std::isfinite(tr) || tr < 0 || tr > 1.0001f)
                    failures[t]++;
                Accumulate(&results[t], double(tr));
            }
        }));
    for (auto &th : threads) th.join();
    for (int t = 0; t < nThreads; ++t) {
        TASK_CHECK(failures[t] == 0);
        TASK_CHECK(std::abs(results[t].mean - ref.mean) <
                   std::max(6.0 * results[t].stderr, 0.01 * ref.mean) + 1e-4);
    }
    for (int z = 0; z < 4; ++z)
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                TASK_CHECK(std::abs(m->Grid()->At(x, y, z) -
                                    original[(z * 4 + y) * 4 + x]) < 1e-6);
    TASK_CHECK(prototype->Grid() == m->Grid());
    printf("TRANSPORT concurrent nThreads=%d perThread=%d ref=%.5f "
           "stderr=%.5f\n",
           nThreads, perThread, ref.mean, ref.stderr);
    return 0;
}

// Reads the example image generated by `task_support/run.sh target` and
// verifies its dimensions and that it contains finite, non-black pixels.  The
// PFM reader here is intentionally self-contained so the test does not rely
// on the production medium at all; it only inspects the rendered result.
int TestTransportExampleImage() {
    const char *path = ".task-build/instance-smoke.pfm";
    FILE *fp = fopen(path, "rb");
    TASK_CHECK(fp != nullptr);
    char magic[3] = {0, 0, 0};
    int width = 0, height = 0;
    float scale = 0;
    TASK_CHECK(fscanf(fp, "%2s", magic) == 1);
    TASK_CHECK(strcmp(magic, "PF") == 0);
    TASK_CHECK(fscanf(fp, "%d %d", &width, &height) == 2);
    TASK_CHECK(fscanf(fp, "%f", &scale) == 1);
    // Consume the single whitespace after the header before the raster.
    int c = fgetc(fp);
    TASK_CHECK(c == '\n' || c == '\r' || c == ' ' || c == '\t');
    if (c == '\r') (void)fgetc(fp);
    // Expected resolution matches scenes/instance-smoke.pbrt.
    TASK_CHECK(width == 96 && height == 64);
    TASK_CHECK(scale != 0);
    const size_t count = size_t(width) * size_t(height) * 3;
    std::vector<float> pixels(count);
    TASK_CHECK(fread(pixels.data(), sizeof(float), count, fp) == count);
    fclose(fp);
    int finite = 0, nonBlack = 0;
    for (size_t i = 0; i < count; ++i) {
        if (std::isfinite(pixels[i])) ++finite;
        if (std::isfinite(pixels[i]) && pixels[i] > 1e-4f) ++nonBlack;
    }
    TASK_CHECK(finite == int(count));
    TASK_CHECK(nonBlack > 0);
    printf("TRANSPORT exampleImage %dx%d finite=%d nonBlack=%d of %zu\n",
           width, height, finite, nonBlack, count);
    return 0;
}

}  // namespace

// The driver links this table and runs every case; each case must perform at
// least one TASK_CHECK.
const struct task_case task_cases[] = {
    {"boundary", "identity", TestBoundaryIdentity},
    {"boundary", "no_material_walk", TestBoundaryNoMaterialWalk},
    {"boundary", "disjoint", TestBoundaryDisjoint},
    {"boundary", "world_medium_preserved", TestBoundaryWorldMediumPreserved},
    {"metric", "scale_optical_thickness",
     TestMetricScaleIncreasesOpticalThickness},
    {"metric", "non_unit_direction", TestMetricNonUnitDirectionAndTMax},
    {"metric", "half_voxel_shared", TestMetricHalfVoxelAndSharedDensity},
    {"metric", "shear_reflection", TestMetricShearReflection},
    {"metric", "homogeneous_analytic", TestMetricHomogeneousAnalytic},
    {"metric", "validation", TestMetricValidation},
    {"transport", "intersect_tr", TestTransportIntersectTr},
    {"transport", "visibility_tr", TestTransportVisibilityTr},
    {"transport", "first_scatter", TestTransportFirstScatterDistribution},
    {"transport", "volpath", TestTransportVolPath},
    {"transport", "interleaved_identity", TestTransportInterleavedIdentity},
    {"transport", "concurrent", TestTransportConcurrent},
    {"transport", "example_image", TestTransportExampleImage},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
