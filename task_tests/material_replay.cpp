/*
    task_tests/material_replay.cpp

    Regression tests for the frozen-proposal plastic material record.  The
    file defines no main(): task_support/driver.c supplies the runner and
    links against the task_cases table below.

    Three groups are registered:
      primal    - path parity with hand-driven production BSDF tracing,
                  direct light against an independent plastic expression,
                  occlusion, failed samples, zero albedo, hemisphere
                  reweighting invariance and feature rejection.
      gradient  - analytic reverse-mode derivatives against two-step central
                  differences for RGB and roughness, endpoints and zero
                  albedo derivatives, shared-id accumulation and restarts.
      ownership - reuse after the Scene/MemoryArena/materials are gone,
                  interleaved restarts and atomic rejection of bad records.
*/

#include "task_support/check.h"
#include "integrators/material_replay.h"

#include "scene.h"
#include "intersection.h"
#include "shapes/trianglemesh.h"
#include "accelerators/bvh.h"
#include "materials/plastic.h"
#include "materials/mirror.h"
#include "textures/constant.h"
#include "textures/checkerboard.h"
#include "lights/point.h"
#include "lights/distant.h"
#include "lights/diffuse.h"
#include "reflection.h"
#include "montecarlo.h"
#include "volume.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Test helpers -------------------------------------------------------------

void ToRGB(const Spectrum &s, float rgb[3]) { s.ToRGB(rgb); }


Spectrum MakeSpectrum(const float rgb[3]) {
    return Spectrum::FromRGB(rgb);
}


Reference<Primitive> MakeTriangle(const Point &p0, const Point &p1,
                                   const Point &p2,
                                   const Reference<Material> &material) {
    Transform id;
    int ids[3] = {0, 1, 2};
    Point p[3] = {p0, p1, p2};
    Reference<Shape> mesh = new TriangleMesh(
        &id, &id, false, 1, 3, ids, p, NULL, NULL, NULL,
        Reference<Texture<float> >());
    return new GeometricPrimitive(mesh, material, NULL);
}


// Rectangle built from two triangles (one static TriangleMesh).  The corner
// order is chosen so the geometric normal points out of the rectangle.
Reference<Primitive> MakeRect(const Point &a, const Point &b, const Point &c,
                              const Point &d,
                              const Reference<Material> &material) {
    Transform id;
    int ids[6] = {0, 1, 2, 0, 2, 3};
    Point p[4] = {a, b, c, d};
    Reference<Shape> mesh = new TriangleMesh(
        &id, &id, false, 2, 4, ids, p, NULL, NULL, NULL,
        Reference<Texture<float> >());
    return new GeometricPrimitive(mesh, material, NULL);
}


Reference<Material> MakePlastic(const float kd[3], const float ks[3],
                                float roughness) {
    return new PlasticMaterial(
        new ConstantTexture<Spectrum>(MakeSpectrum(kd)),
        new ConstantTexture<Spectrum>(MakeSpectrum(ks)),
        new ConstantTexture<float>(roughness),
        Reference<Texture<float> >());
}


MaterialReplayParams MakeParams(float kd0, float kd1, float kd2,
                                float ks0, float ks1, float ks2,
                                float rough) {
    MaterialReplayParams p;
    p.kd[0] = kd0; p.kd[1] = kd1; p.kd[2] = kd2;
    p.ks[0] = ks0; p.ks[1] = ks1; p.ks[2] = ks2;
    p.roughness = rough;
    return p;
}


// Fully refine a primitive up front so the leaf GeometricPrimitive ids are
// known before the BVH takes ownership.
std::vector<Reference<Primitive> > Refine(Reference<Primitive> prim) {
    std::vector<Reference<Primitive> > leaves;
    prim->FullyRefine(leaves);
    return leaves;
}


bool Close(float a, float b, float absTol, float relTol) {
    return std::fabs(a - b) <= absTol + relTol * std::fabs(b);
}


// Independent textbook plastic BRDF in local coordinates, evaluated in
// double precision so the primal direct-light check does not reuse the
// production code path under test.
struct IndependentPlastic {
    double kd[3], ks[3], roughness;

    static double Fresnel(double cosi) {
        const double ei = 1.5, et = 1.0;
        double c = std::fabs(std::max(-1.0, std::min(1.0, cosi)));
        double s = ei / et * std::sqrt(std::max(0.0, 1.0 - c * c));
        if (s >= 1.0) return 1.0;
        double ct = std::sqrt(std::max(0.0, 1.0 - s * s));
        double rs = ((ei * c) - (et * ct)) / ((ei * c) + (et * ct));
        double rp = ((et * c) - (ei * ct)) / ((et * c) + (ei * ct));
        return 0.5 * (rs * rs + rp * rp);
    }

    void Eval(const Vector &woV, const Vector &wiV, double f[3]) const {
        double wo[3] = {woV.x, woV.y, woV.z};
        double wi[3] = {wiV.x, wiV.y, wiV.z};
        for (int c = 0; c < 3; ++c) f[c] = kd[c] / M_PI;
        double co = std::fabs(wo[2]), ci = std::fabs(wi[2]);
        if (ci == 0.0 || co == 0.0) return;
        double wh[3] = {wo[0] + wi[0], wo[1] + wi[1], wo[2] + wi[2]};
        double len = std::sqrt(wh[0]*wh[0] + wh[1]*wh[1] + wh[2]*wh[2]);
        if (len == 0.0) return;
        for (int i = 0; i < 3; ++i) wh[i] /= len;
        double nDotWh = std::fabs(wh[2]);
        double woDotWh = std::fabs(wo[0]*wh[0] + wo[1]*wh[1] + wo[2]*wh[2]);
        if (woDotWh <= 0.0) return;
        double e = 1.0 / roughness;
        double D = (e + 2.0) / (2.0 * M_PI) * std::pow(nDotWh, e);
        double G = std::min(1.0, std::min(2.0 * nDotWh * co / woDotWh,
                                          2.0 * nDotWh * ci / woDotWh));
        double cosH = wi[0]*wh[0] + wi[1]*wh[1] + wi[2]*wh[2];
        double F = Fresnel(cosH);
        double H = D * G * F / (4.0 * ci * co);
        for (int c = 0; c < 3; ++c) f[c] += ks[c] * H;
    }
};


// Reference path tracer driven through the production BSDF, mirroring the
// frozen-record semantics: no Russian roulette, all lights per vertex,
// failed samples end the path, black background.
Spectrum ReferencePath(const Scene *scene,
                       const std::vector<Reference<Material> > &materials,
                       const std::vector<int> &leafMaterial,
                       const std::vector<Reference<Primitive> > &leaves,
                       const RayDifferential &ray0, int maxVertices,
                       RNG &rng, MemoryArena &arena) {
    Spectrum L(0.f), throughput(1.f);
    RayDifferential ray = ray0;
    Intersection isect;
    if (!scene->Intersect(ray, &isect)) return L;

    for (int depth = 0; depth < maxVertices; ++depth) {
        int mi = -1;
        for (size_t i = 0; i < leaves.size(); ++i)
            if (leaves[i]->primitiveId == isect.primitiveId) { mi = int(i); break; }
        int matIndex = leafMaterial[mi];
        BSDF *bsdf = materials[matIndex]->GetBSDF(isect.dg, isect.dg, arena);
        const Point &p = bsdf->dgShading.p;
        const Normal &n = bsdf->dgShading.nn;
        Vector wo = -ray.d;

        for (size_t li = 0; li < scene->lights.size(); ++li) {
            Vector wi; float pdf = 0.f; VisibilityTester vis;
            LightSample ls(0.5f, 0.5f, 0.5f);
            Spectrum Li = scene->lights[li]->Sample_L(
                p, isect.rayEpsilon, ls, ray.time, &wi, &pdf, &vis);
            if (pdf > 0.f && !Li.IsBlack() && vis.Unoccluded(scene)) {
                Spectrum f = bsdf->f(wo, wi);
                L += throughput * f * Li * AbsDot(wi, n);
            }
        }

        if (depth + 1 == maxVertices) break;
        Vector wi; float pdf = 0.f; BxDFType flags;
        Spectrum f = bsdf->Sample_f(wo, &wi, BSDFSample(rng), &pdf,
                                    BSDF_ALL, &flags);
        if (f.IsBlack() || pdf == 0.f) break;
        throughput *= f * AbsDot(wi, n) / pdf;
        ray = RayDifferential(p, wi, ray, isect.rayEpsilon);
        Intersection next;
        if (!scene->Intersect(ray, &next)) break;
        isect = next;
        arena.FreeAll();
    }
    return L;
}


struct TestScene {
    Scene *scene;
    std::vector<Reference<Primitive> > leaves;
    std::vector<int> leafMaterial;
    std::vector<Reference<Material> > materials;
    std::vector<Light *> lights;

    TestScene() : scene(NULL) { }

    ~TestScene() {
        // The Scene owns and deletes its lights; only free the Scene here.
        delete scene;
    }
};


// Two perpendicular meshes (floor + wall) sharing or splitting material
// ids, one point light above the floor.  Built so downward rays can create
// real two-bounce paths floor -> wall.
TestScene *BuildCornerScene(const float kdA[3], const float ksA[3], float rA,
                            const float kdB[3], const float ksB[3], float rB,
                            bool shareId, Light *extraLight = NULL) {
    TestScene *ts = new TestScene;
    ts->materials.push_back(MakePlastic(kdA, ksA, rA));
    ts->materials.push_back(MakePlastic(kdB, ksB, rB));

    // Floor rectangle at y = 0 (normal +y) and wall rectangle at z = -1.5
    // facing +z, so downward rays hit the floor and -z continuations reach
    // the wall for real two-bounce paths.
    Reference<Primitive> floor = MakeRect(
        Point(-3, 0, 3), Point(3, 0, 3), Point(3, 0, -1.5f),
        Point(-3, 0, -1.5f), ts->materials[0]);
    Reference<Primitive> wall = MakeRect(
        Point(-3, 0, -1.5f), Point(3, 0, -1.5f),
        Point(3, 3, -1.5f), Point(-3, 3, -1.5f),
        ts->materials[shareId ? 0 : 1]);
    std::vector<Reference<Primitive> > f0 = Refine(floor);
    std::vector<Reference<Primitive> > f1 = Refine(wall);
    for (size_t i = 0; i < f0.size(); ++i) {
        ts->leaves.push_back(f0[i]);
        ts->leafMaterial.push_back(0);
    }
    for (size_t i = 0; i < f1.size(); ++i) {
        ts->leaves.push_back(f1[i]);
        ts->leafMaterial.push_back(shareId ? 0 : 1);
    }

    ts->lights.push_back(new PointLight(Translate(Vector(0.8f, 1.6f, 0.6f)),
                                        Spectrum(3.f)));
    if (extraLight) ts->lights.push_back(extraLight);
    ts->scene = new Scene(new BVHAccel(ts->leaves), ts->lights, NULL);
    return ts;
}


void BindLeaves(MaterialReplay *replay, const TestScene *ts,
                int idA, int idB) {
    for (size_t i = 0; i < ts->leaves.size(); ++i) {
        int id = ts->leafMaterial[i] == 0 ? idA : idB;
        replay->BindPrimitive(id, ts->leaves[i]->primitiveId);
    }
}


std::vector<MaterialReplayRay> MakeRays(int n, uint32_t baseSeed) {
    std::vector<MaterialReplayRay> rays;
    for (int i = 0; i < n; ++i) {
        float t = (i + 0.5f) / n;
        // Rays start above the floor and aim down toward the wall side, so
        // proposal continuations frequently reach the wall (two bounces).
        Point o(-2.2f + 4.0f * t, 1.4f + 0.15f * (i % 4), 1.7f);
        Vector d(0.12f * (t - 0.5f), -0.62f, -0.78f);
        rays.push_back(MaterialReplayRay(o, Normalize(d), baseSeed + i));
    }
    return rays;
}


// Primal cases -------------------------------------------------------------

int PrimalProductionParity() {
    const float kdA[3] = {0.35f, 0.25f, 0.15f};
    const float ksA[3] = {0.2f, 0.18f, 0.16f};
    const float kdB[3] = {0.12f, 0.3f, 0.22f};
    const float ksB[3] = {0.3f, 0.25f, 0.1f};
    TestScene *ts = BuildCornerScene(kdA, ksA, 0.22f, kdB, ksB, 0.4f, false);

    MaterialReplayParams pa = MakeParams(kdA[0], kdA[1], kdA[2],
                                         ksA[0], ksA[1], ksA[2], 0.22f);
    MaterialReplayParams pb = MakeParams(kdB[0], kdB[1], kdB[2],
                                         ksB[0], ksB[1], ksB[2], 0.4f);
    MaterialReplay replay;
    std::string err;
    TASK_CHECK(replay.BindProposal(7, pa, &err));
    TASK_CHECK(replay.BindProposal(9, pb, &err));
    BindLeaves(&replay, ts, 7, 9);

    const int kMaxV = 4;
    std::vector<MaterialReplayRay> rays = MakeRays(12, 1000u);
    TASK_CHECK(replay.Record(ts->scene, rays, kMaxV, &err));
    TASK_CHECK(replay.NumRays() == int(rays.size()));

    bool sawTwoBounce = false;
    for (int i = 0; i < replay.NumRays(); ++i)
        if (replay.PathNumVertices(i) >= 2) sawTwoBounce = true;
    TASK_CHECK(sawTwoBounce);

    std::vector<MaterialReplayParams> targets(2);
    targets[0] = pa; targets[1] = pb; // target == proposal
    std::vector<Spectrum> got;
    TASK_CHECK(replay.Evaluate(targets, &got, &err));

    MemoryArena arena;
    int checked = 0;
    for (size_t r = 0; r < rays.size(); ++r) {
        RNG rng(rays[r].seed);
        RayDifferential rr(rays[r].origin,
                           Normalize(rays[r].direction), 0.f);
        Spectrum ref = ReferencePath(ts->scene, ts->materials,
                                     ts->leafMaterial, ts->leaves,
                                     rr, kMaxV, rng, arena);
        float a[3], b[3];
        ToRGB(ref, a); ToRGB(got[r], b);
        for (int c = 0; c < 3; ++c) {
            TASK_CHECK(Close(a[c], b[c], 2e-5f, 2e-4f));
            if (b[c] != 0.f) ++checked;
        }
        arena.FreeAll();
    }
    TASK_CHECK(checked > 0);
    delete ts;
    return 0;
}


int PrimalDirectLightExpression() {
    const float kd[3] = {0.4f, 0.3f, 0.2f};
    const float ks[3] = {0.25f, 0.2f, 0.3f};
    TestScene *ts = BuildCornerScene(kd, ks, 0.18f, kd, ks, 0.18f, true);

    MaterialReplayParams pp = MakeParams(kd[0], kd[1], kd[2],
                                         ks[0], ks[1], ks[2], 0.18f);
    MaterialReplay replay;
    TASK_CHECK(replay.BindProposal(3, pp));
    BindLeaves(&replay, ts, 3, 3);

    std::vector<MaterialReplayRay> rays = MakeRays(8, 2000u);
    TASK_CHECK(replay.Record(ts->scene, rays, 1)); // direct only

    std::vector<Spectrum> got;
    TASK_CHECK(replay.Evaluate(std::vector<MaterialReplayParams>(1, pp), &got));

    IndependentPlastic ref;
    for (int c = 0; c < 3; ++c) { ref.kd[c] = kd[c]; ref.ks[c] = ks[c]; }
    ref.roughness = 0.18;

    int nonzero = 0;
    for (size_t r = 0; r < rays.size(); ++r) {
        float v[3]; ToRGB(got[r], v);
        if (v[0] == 0.f && v[1] == 0.f && v[2] == 0.f) continue;
        ++nonzero;
        // Re-derive the same single-vertex direct term independently.
        RNG rng(rays[r].seed);
        RayDifferential rr(rays[r].origin, Normalize(rays[r].direction), 0.f);
        Intersection isect;
        TASK_CHECK(ts->scene->Intersect(rr, &isect));
        MemoryArena arena;
        BSDF *bsdf = ts->materials[0]->GetBSDF(isect.dg, isect.dg, arena);
        Vector wo = -Normalize(rays[r].direction);
        Spectrum expect(0.f);
        for (size_t li = 0; li < ts->scene->lights.size(); ++li) {
            Vector wi; float pdf = 0.f; VisibilityTester vis;
            LightSample ls(0.5f, 0.5f, 0.5f);
            Spectrum Li = ts->scene->lights[li]->Sample_L(
                isect.dg.p, isect.rayEpsilon, ls, 0.f, &wi, &pdf, &vis);
            if (!vis.Unoccluded(ts->scene)) continue;
            Vector wiL = bsdf->WorldToLocal(wi), woL = bsdf->WorldToLocal(wo);
            double f[3]; ref.Eval(woL, wiL, f);
            float liRGB[3]; ToRGB(Li, liRGB);
            float cos = AbsDot(wi, isect.dg.nn);
            float rgb[3];
            for (int c = 0; c < 3; ++c) rgb[c] = float(liRGB[c] * f[c] * cos);
            expect += MakeSpectrum(rgb);
        }
        float e[3]; ToRGB(expect, e);
        for (int c = 0; c < 3; ++c)
            TASK_CHECK(Close(v[c], e[c], 2e-5f, 3e-4f));
    }
    TASK_CHECK(nonzero >= 4);
    delete ts;
    return 0;
}


int PrimalZeroAlbedoAndMiss() {
    const float kd[3] = {0.3f, 0.3f, 0.3f};
    const float ks[3] = {0.3f, 0.3f, 0.3f};
    TestScene *ts = BuildCornerScene(kd, ks, 0.25f, kd, ks, 0.25f, true);
    MaterialReplayParams pp = MakeParams(.3f, .3f, .3f, .3f, .3f, .3f, .25f);
    MaterialReplay replay;
    TASK_CHECK(replay.BindProposal(1, pp));
    BindLeaves(&replay, ts, 1, 1);

    std::vector<MaterialReplayRay> rays = MakeRays(6, 3000u);
    rays.push_back(MaterialReplayRay(Point(0, 1, 5), Normalize(Vector(0, 1, 1)),
                                     42u)); // misses into the black background
    TASK_CHECK(replay.Record(ts->scene, rays, 3));
    TASK_CHECK(replay.PathNumVertices(int(rays.size()) - 1) == 0);

    MaterialReplayParams zero = MakeParams(0, 0, 0, 0, 0, 0, 0.25f);
    std::vector<Spectrum> black;
    TASK_CHECK(replay.Evaluate(std::vector<MaterialReplayParams>(1, zero),
                               &black));
    for (size_t r = 0; r < black.size(); ++r) {
        float v[3]; ToRGB(black[r], v);
        for (int c = 0; c < 3; ++c) TASK_CHECK(v[c] == 0.f);
    }

    // Kd = 0 keeps the specular lobe alive: values must not all collapse.
    MaterialReplayParams specOnly = MakeParams(0, 0, 0, .3f, .3f, .3f, .25f);
    std::vector<Spectrum> spec;
    TASK_CHECK(replay.Evaluate(std::vector<MaterialReplayParams>(1, specOnly),
                               &spec));
    int specNonZero = 0;
    for (size_t r = 0; r < spec.size(); ++r) {
        float v[3]; ToRGB(spec[r], v);
        for (int c = 0; c < 3; ++c) if (v[c] != 0.f) ++specNonZero;
    }
    TASK_CHECK(specNonZero > 0);
    delete ts;
    return 0;
}


int PrimalOcclusion() {
    const float kd[3] = {0.35f, 0.35f, 0.35f};
    const float ks[3] = {0.15f, 0.15f, 0.15f};
    TestScene ts;
    ts.materials.push_back(MakePlastic(kd, ks, .2f));

    // Floor and wall plus an occluder between the light and part of the
    // floor.  Every primitive carries the same bound id, so the finite
    // shadow test is the only thing that changes.
    Reference<Primitive> floor = MakeRect(
        Point(-3, 0, 3), Point(3, 0, 3), Point(3, 0, -1.5f),
        Point(-3, 0, -1.5f), ts.materials[0]);
    Reference<Primitive> wall = MakeRect(
        Point(-3, 0, -1.5f), Point(3, 0, -1.5f),
        Point(3, 3, -1.5f), Point(-3, 3, -1.5f),
        ts.materials[0]);
    Reference<Primitive> blocker = MakeTriangle(
        Point(0.3f, 1.1f, 0.1f), Point(1.3f, 1.1f, 1.1f),
        Point(1.3f, 1.1f, 0.1f), ts.materials[0]);
    std::vector<Reference<Primitive> > src;
    src.push_back(floor); src.push_back(wall); src.push_back(blocker);
    for (size_t i = 0; i < src.size(); ++i) {
        std::vector<Reference<Primitive> > r = Refine(src[i]);
        for (size_t j = 0; j < r.size(); ++j) {
            ts.leaves.push_back(r[j]);
            ts.leafMaterial.push_back(0);
        }
    }
    ts.lights.push_back(new PointLight(Translate(Vector(0.8f, 1.6f, 0.6f)),
                                       Spectrum(3.f)));
    ts.scene = new Scene(new BVHAccel(ts.leaves), ts.lights, NULL);

    MaterialReplayParams pp = MakeParams(.35f, .35f, .35f, .15f, .15f, .15f, .2f);
    MaterialReplay replay;
    TASK_CHECK(replay.BindProposal(2, pp));
    BindLeaves(&replay, &ts, 2, 2);

    std::vector<MaterialReplayRay> rays = MakeRays(16, 5000u);
    TASK_CHECK(replay.Record(ts.scene, rays, 1));
    std::vector<Spectrum> got;
    TASK_CHECK(replay.Evaluate(std::vector<MaterialReplayParams>(1, pp), &got));

    int shadowed = 0, lit = 0;
    for (size_t r = 0; r < rays.size(); ++r) {
        float v[3]; ToRGB(got[r], v);
        bool anyLit = false, anyZero = false;
        for (int c = 0; c < 3; ++c) {
            if (v[c] > 0.f) anyLit = true;
            if (v[c] == 0.f) anyZero = true;
        }
        if (anyLit) ++lit;
        if (anyZero && !anyLit) ++shadowed;
    }
    TASK_CHECK(lit > 0);
    TASK_CHECK(shadowed > 0);
    return 0;
}


int PrimalFailedSample() {
    // Broad specular lobe (exponent 1) and grazing rays force proposal
    // samples across the geometric normal, which end as zero-contribution
    // failed samples without any re-draw.
    const float kd[3] = {0.3f, 0.3f, 0.3f};
    const float ks[3] = {0.3f, 0.3f, 0.3f};
    TestScene *ts = BuildCornerScene(kd, ks, 1.f, kd, ks, 1.f, true);
    MaterialReplayParams pp = MakeParams(.3f, .3f, .3f, .3f, .3f, .3f, 1.f);
    MaterialReplay replay;
    TASK_CHECK(replay.BindProposal(4, pp));
    BindLeaves(&replay, ts, 4, 4);

    std::vector<MaterialReplayRay> rays;
    for (int i = 0; i < 240; ++i) {
        float t = (i + 0.5f) / 240.f;
        Point o(-2.5f + 5.f * t, 0.05f, 1.5f);
        rays.push_back(MaterialReplayRay(o, Normalize(Vector(0.4f * (t-.5f),
                                                              -0.06f, -1.f)),
                                         7000u + i));
    }
    TASK_CHECK(replay.Record(ts->scene, rays, 3));

    int failed = -1;
    for (int i = 0; i < replay.NumRays(); ++i)
        if (replay.PathEndedFailed(i)) { failed = i; break; }
    TASK_CHECK(failed >= 0);

    // The failed vertex still keeps its direct lighting, and the path has
    // no continuation beyond it.
    TASK_CHECK(replay.PathNumVertices(failed) >= 1);
    std::vector<Spectrum> got;
    TASK_CHECK(replay.Evaluate(std::vector<MaterialReplayParams>(1, pp), &got));
    float v[3]; ToRGB(got[failed], v);
    TASK_CHECK(std::isfinite(v[0]) && std::isfinite(v[1]) &&
               std::isfinite(v[2]));
    delete ts;
    return 0;
}


int PrimalMultipleLights() {
    const float kd[3] = {0.3f, 0.3f, 0.3f};
    const float ks[3] = {0.2f, 0.2f, 0.2f};
    PointLight *second = new PointLight(Translate(Vector(-1.1f, 1.2f, 1.1f)),
                                        Spectrum(1.5f));
    TestScene *ts = BuildCornerScene(kd, ks, .3f, kd, ks, .3f, true, second);
    MaterialReplayParams pp = MakeParams(.3f, .3f, .3f, .2f, .2f, .2f, .3f);
    MaterialReplay replay;
    TASK_CHECK(replay.BindProposal(5, pp));
    BindLeaves(&replay, ts, 5, 5);

    std::vector<MaterialReplayRay> rays = MakeRays(10, 8000u);
    TASK_CHECK(replay.Record(ts->scene, rays, 2));
    std::vector<Spectrum> got;
    TASK_CHECK(replay.Evaluate(std::vector<MaterialReplayParams>(1, pp), &got));

    MemoryArena arena;
    for (size_t r = 0; r < rays.size(); ++r) {
        RNG rng(rays[r].seed);
        Spectrum ref = ReferencePath(ts->scene, ts->materials,
                                     ts->leafMaterial, ts->leaves,
                                     RayDifferential(rays[r].origin,
                                                    Normalize(rays[r].direction),
                                                    0.f),
                                     2, rng, arena);
        float a[3], b[3]; ToRGB(ref, a); ToRGB(got[r], b);
        for (int c = 0; c < 3; ++c)
            TASK_CHECK(Close(a[c], b[c], 2e-5f, 2e-4f));
        arena.FreeAll();
    }
    delete ts;
    return 0;
}


int PrimalHemisphereInvariance() {
    // Fixed stratified canonical samples drive the full plastic mixture of
    // two different proposal BSDFs.  The target BRDF is reweighted by each
    // proposal's own pdf; both importance estimates must converge to the same
    // one-sided hemispherical integral, independent of the proposal shape.
    MemoryArena arena;
    DifferentialGeometry dg(Point(0, 0, 0), Vector(1, 0, 0), Vector(0, 1, 0),
                            Normal(0, 0, 0), Normal(0, 0, 0), 0, 0, NULL);
    Vector wo = Normalize(Vector(0.3f, -0.2f, 0.9f));

    MaterialReplayParams target = MakeParams(.35f, .25f, .15f,
                                             .3f, .2f, .1f, .25f);
    MaterialReplayParams propA = MakeParams(.5f, .5f, .5f, .2f, .2f, .2f, .1f);
    MaterialReplayParams propB = MakeParams(.2f, .3f, .4f, .4f, .3f, .2f, .8f);
    Reference<Material> mTar = MakePlastic(target.kd, target.ks,
                                           target.roughness);
    Reference<Material> mA = MakePlastic(propA.kd, propA.ks, propA.roughness);
    Reference<Material> mB = MakePlastic(propB.kd, propB.ks, propB.roughness);
    BSDF *bTar = mTar->GetBSDF(dg, dg, arena);
    BSDF *bA = mA->GetBSDF(dg, dg, arena);
    BSDF *bB = mB->GetBSDF(dg, dg, arena);

    // Fixed stratified samples (deterministic RNG seed): a jittered 2D set
    // for the direction draws and a jittered 1D set for the component draw.
    const int N = 64;
    const int M = N * N;
    std::vector<float> dir2D(2 * M), comp1D(M);
    RNG stratRng(1234u);
    StratifiedSample2D(&dir2D[0], N, N, stratRng);
    for (int i = 0; i < M; ++i)
        comp1D[i] = min((i + stratRng.RandomFloat()) / M, OneMinusEpsilon);

    double sumA[3] = {0, 0, 0}, sumB[3] = {0, 0, 0};
    double sumRef[3] = {0, 0, 0};
    long countA = 0, countB = 0, countRef = 0;
    for (int k = 0; k < M; ++k) {
        BSDFSample s(dir2D[2 * k], dir2D[2 * k + 1], comp1D[k]);
        Vector wiA, wiB, wiT; float pA, pB, pT; BxDFType ty;
        bA->Sample_f(wo, &wiA, s, &pA, BSDF_ALL, &ty);
        bB->Sample_f(wo, &wiB, s, &pB, BSDF_ALL, &ty);
        bTar->Sample_f(wo, &wiT, s, &pT, BSDF_ALL, &ty);
        Spectrum gA = bTar->f(wo, wiA) *
                      AbsCosTheta(bTar->WorldToLocal(wiA));
        Spectrum gB = bTar->f(wo, wiB) *
                      AbsCosTheta(bTar->WorldToLocal(wiB));
        Spectrum gT = bTar->f(wo, wiT) *
                      AbsCosTheta(bTar->WorldToLocal(wiT));
        float a[3], b[3], t[3];
        gA.ToRGB(a); gB.ToRGB(b); gT.ToRGB(t);
        for (int c = 0; c < 3; ++c) {
            if (pA > 0.f) { sumA[c] += a[c] / pA; ++countA; }
            if (pB > 0.f) { sumB[c] += b[c] / pB; ++countB; }
            if (pT > 0.f) { sumRef[c] += t[c] / pT; ++countRef; }
        }
    }
    for (int c = 0; c < 3; ++c) {
        sumA[c] /= countA; sumB[c] /= countB; sumRef[c] /= countRef;
        TASK_CHECK(std::fabs(sumA[c] - sumB[c]) <=
                   0.06 * (sumRef[c] + 1e-6));
        TASK_CHECK(std::fabs(sumA[c] - sumRef[c]) <=
                   0.05 * (sumRef[c] + 1e-6));
        TASK_CHECK(std::fabs(sumB[c] - sumRef[c]) <=
                   0.05 * (sumRef[c] + 1e-6));
        TASK_CHECK(sumRef[c] > 0.0);
    }
    std::printf("HEMISPHERE reflectance target=[%.4f %.4f %.4f] "
                "proposalA=[%.4f %.4f %.4f] proposalB=[%.4f %.4f %.4f]\n",
                sumRef[0], sumRef[1], sumRef[2], sumA[0], sumA[1], sumA[2],
                sumB[0], sumB[1], sumB[2]);
    return 0;
}


int PrimalTargetReweightIntegral() {
    // After editing target parameters, the mean of the reweighted frozen
    // paths must equal the same-depth integral sampled natively with the
    // target material.  Uses two geometrically identical scenes whose only
    // difference is the proposal material.
    MaterialReplayParams propA = MakeParams(.5f, .4f, .3f, .2f, .2f, .2f, .15f);
    MaterialReplayParams target = MakeParams(.2f, .35f, .45f,
                                             .35f, .25f, .15f, .6f);

    std::vector<Reference<Material> > mA, mB;
    mA.push_back(MakePlastic(propA.kd, propA.ks, propA.roughness));
    mB.push_back(MakePlastic(target.kd, target.ks, target.roughness));

    std::vector<Reference<Primitive> > rectA, rectB;
    rectA.push_back(MakeRect(
        Point(-3, 0, 3), Point(3, 0, 3), Point(3, 0, -1.5f),
        Point(-3, 0, -1.5f), mA[0]));
    rectA.push_back(MakeRect(
        Point(-3, 0, -1.5f), Point(3, 0, -1.5f),
        Point(3, 3, -1.5f), Point(-3, 3, -1.5f), mA[0]));
    rectB.push_back(MakeRect(
        Point(-3, 0, 3), Point(3, 0, 3), Point(3, 0, -1.5f),
        Point(-3, 0, -1.5f), mB[0]));
    rectB.push_back(MakeRect(
        Point(-3, 0, -1.5f), Point(3, 0, -1.5f),
        Point(3, 3, -1.5f), Point(-3, 3, -1.5f), mB[0]));
    std::vector<Reference<Primitive> > leavesA, leavesB;
    for (size_t i = 0; i < rectA.size(); ++i) {
        std::vector<Reference<Primitive> > r = Refine(rectA[i]);
        for (size_t j = 0; j < r.size(); ++j) leavesA.push_back(r[j]);
    }
    for (size_t i = 0; i < rectB.size(); ++i) {
        std::vector<Reference<Primitive> > r = Refine(rectB[i]);
        for (size_t j = 0; j < r.size(); ++j) leavesB.push_back(r[j]);
    }
    std::vector<Light *> lightsA, lightsB;
    lightsA.push_back(new PointLight(Translate(Vector(.8f, 1.6f, .6f)),
                                     Spectrum(3.f)));
    lightsB.push_back(new PointLight(Translate(Vector(.8f, 1.6f, .6f)),
                                     Spectrum(3.f)));
    Scene sceneA(new BVHAccel(leavesA), lightsA, NULL);
    Scene sceneB(new BVHAccel(leavesB), lightsB, NULL);

    const int N = 256, kDepth = 3;
    std::vector<MaterialReplayRay> rays;
    for (int i = 0; i < N; ++i) {
        float tt = (i + 0.5f) / N;
        Point o(-2.2f + 4.0f * tt, 1.4f + 0.15f * (i % 4), 1.7f);
        Vector d(0.12f * (tt - 0.5f), -0.62f, -0.78f);
        rays.push_back(MaterialReplayRay(o, Normalize(d), 20000u + i));
    }

    MaterialReplay recA, recB;
    TASK_CHECK(recA.BindProposal(1, propA));
    TASK_CHECK(recB.BindProposal(1, target));
    for (size_t i = 0; i < leavesA.size(); ++i)
        TASK_CHECK(recA.BindPrimitive(1, leavesA[i]->primitiveId));
    for (size_t i = 0; i < leavesB.size(); ++i)
        TASK_CHECK(recB.BindPrimitive(1, leavesB[i]->primitiveId));
    TASK_CHECK(recA.Record(&sceneA, rays, kDepth));
    TASK_CHECK(recB.Record(&sceneB, rays, kDepth));
    TASK_CHECK(recA.NumRays() == N && recB.NumRays() == N);

    std::vector<Spectrum> reweighted, nativeTarget, nativeProposal;
    TASK_CHECK(recA.Evaluate(std::vector<MaterialReplayParams>(1, target),
                             &reweighted));
    TASK_CHECK(recB.Evaluate(std::vector<MaterialReplayParams>(1, target),
                             &nativeTarget));
    TASK_CHECK(recA.Evaluate(std::vector<MaterialReplayParams>(1, propA),
                             &nativeProposal));

    double meanRW[3] = {0, 0, 0}, meanT[3] = {0, 0, 0}, meanP[3] = {0, 0, 0};
    for (int r = 0; r < N; ++r) {
        float a[3], b[3], c[3];
        reweighted[r].ToRGB(a);
        nativeTarget[r].ToRGB(b);
        nativeProposal[r].ToRGB(c);
        for (int k = 0; k < 3; ++k) {
            meanRW[k] += a[k]; meanT[k] += b[k]; meanP[k] += c[k];
        }
    }
    for (int k = 0; k < 3; ++k) {
        meanRW[k] /= N; meanT[k] /= N; meanP[k] /= N;
        std::printf("REWEIGHT ch=%d proposal_mean=%.6f target_mean=%.6f "
                    "reweighted_mean=%.6f\n",
                    k, meanP[k], meanT[k], meanRW[k]);
        // Reweighting recovers the target integral.
        TASK_CHECK(std::fabs(meanRW[k] - meanT[k]) <=
                   2e-4 + 2e-2 * std::fabs(meanT[k]));
        // And it genuinely moved away from the proposal integral.
        TASK_CHECK(std::fabs(meanRW[k] - meanP[k]) >
                   5e-3 * std::fabs(meanP[k]));
    }
    return 0;
}


int PrimalRejectUnsupported() {
    const float kd[3] = {0.3f, 0.3f, 0.3f};
    const float ks[3] = {0.2f, 0.2f, 0.2f};
    MaterialReplayParams pp = MakeParams(.3f, .3f, .3f, .2f, .2f, .2f, .2f);

    // Non-point light is rejected before recording.
    {
        TestScene *ts = BuildCornerScene(kd, ks, .2f, kd, ks, .2f, true);
        std::vector<Light *> mixed;
        mixed.push_back(new PointLight(Translate(Vector(0.8f, 1.6f, 0.6f)),
                                       Spectrum(3.f)));
        mixed.push_back(new DistantLight(Transform(), Spectrum(1.f),
                                         Vector(0, -1, 0)));
        Scene bad(new BVHAccel(ts->leaves), mixed, NULL);
        MaterialReplay replay;
        TASK_CHECK(replay.BindProposal(1, pp));
        BindLeaves(&replay, ts, 1, 1);
        std::string err;
        TASK_CHECK(!replay.Record(&bad, MakeRays(2, 1u), 2, &err));
        TASK_CHECK(!err.empty());
        delete ts;
    }

    // A mirror material on a bound primitive is rejected at the first hit.
    {
        TestScene ts;
        ts.materials.push_back(MakePlastic(kd, ks, .2f));
        Reference<Texture<Spectrum> > kr =
            new ConstantTexture<Spectrum>(Spectrum(.8f));
        ts.materials.push_back(new MirrorMaterial(
            kr, Reference<Texture<float> >()));
        Reference<Primitive> floor = MakeRect(
            Point(-3, 0, 3), Point(3, 0, 3), Point(3, 0, -1.5f),
            Point(-3, 0, -1.5f), ts.materials[0]);
        Reference<Primitive> wall = MakeRect(
            Point(-3, 0, -1.5f), Point(3, 0, -1.5f),
            Point(3, 3, -1.5f), Point(-3, 3, -1.5f),
            ts.materials[1]);
        std::vector<Reference<Primitive> > l0 = Refine(floor);
        std::vector<Reference<Primitive> > l1 = Refine(wall);
        for (size_t i = 0; i < l0.size(); ++i) {
            ts.leaves.push_back(l0[i]); ts.leafMaterial.push_back(0);
        }
        for (size_t i = 0; i < l1.size(); ++i) {
            ts.leaves.push_back(l1[i]); ts.leafMaterial.push_back(1);
        }
        ts.lights.push_back(new PointLight(Translate(Vector(0.8f, 1.6f, 0.6f)),
                                           Spectrum(3.f)));
        ts.scene = new Scene(new BVHAccel(ts.leaves), ts.lights, NULL);

        MaterialReplay replay;
        TASK_CHECK(replay.BindProposal(1, pp));
        TASK_CHECK(replay.BindProposal(2, pp));
        BindLeaves(&replay, &ts, 1, 2);
        std::vector<MaterialReplayRay> rays = MakeRays(6, 2u);
        rays.push_back(MaterialReplayRay(Point(0, 1.f, 0.5f),
                                         Vector(0, 0, -1.f), 99u));
        std::string err;
        TASK_CHECK(!replay.Record(ts.scene, rays, 3, &err));
        TASK_CHECK(!err.empty());
    }

    // An unbound primitive is rejected.
    {
        TestScene *ts = BuildCornerScene(kd, ks, .2f, kd, ks, .2f, false);
        MaterialReplay replay;
        TASK_CHECK(replay.BindProposal(1, pp));
        // Only floor leaves bound; the direct wall hit must fail the record.
        for (size_t i = 0; i < ts->leaves.size(); ++i)
            if (ts->leafMaterial[i] == 0)
                replay.BindPrimitive(1, ts->leaves[i]->primitiveId);
        std::vector<MaterialReplayRay> rays = MakeRays(10, 3u);
        rays.push_back(MaterialReplayRay(Point(0, 1.f, 0.5f),
                                         Vector(0, 0, -1.f), 98u));
        std::string err;
        TASK_CHECK(!replay.Record(ts->scene, rays, 3, &err));
        delete ts;
    }

    // A textured plastic on a bound primitive is rejected even though its
    // BxDF component types are identical to a constant plastic.
    {
        TestScene ts;
        Transform id;
        int meshIds[6] = {0, 1, 2, 0, 2, 3};
        Point pts[4] = {
            Point(-3, 0, 3), Point(3, 0, 3), Point(3, 0, -1.5f),
            Point(-3, 0, -1.5f)};
        float uvs[8] = {0, 0, 1, 0, 1, 1, 0, 1};
        Reference<Shape> mesh = new TriangleMesh(
            &id, &id, false, 2, 4, meshIds, pts, NULL, NULL, uvs,
            Reference<Texture<float> >());
        Reference<Texture<Spectrum> > c1 =
            new ConstantTexture<Spectrum>(Spectrum(0.1f));
        Reference<Texture<Spectrum> > c2 =
            new ConstantTexture<Spectrum>(Spectrum(0.6f));
        Reference<Texture<Spectrum> > checker =
            new Checkerboard2DTexture<Spectrum>(
                new UVMapping2D(), c1, c2, "none");
        ts.materials.push_back(new PlasticMaterial(
            checker, new ConstantTexture<Spectrum>(Spectrum(0.2f)),
            new ConstantTexture<float>(0.2f),
            Reference<Texture<float> >()));
        Reference<Primitive> prim =
            new GeometricPrimitive(mesh, ts.materials[0], NULL);
        std::vector<Reference<Primitive> > r = Refine(prim);
        for (size_t i = 0; i < r.size(); ++i) {
            ts.leaves.push_back(r[i]);
            ts.leafMaterial.push_back(0);
        }
        ts.lights.push_back(new PointLight(
            Translate(Vector(0.8f, 1.6f, 0.6f)), Spectrum(3.f)));
        ts.scene = new Scene(new BVHAccel(ts.leaves), ts.lights, NULL);

        MaterialReplay replay;
        TASK_CHECK(replay.BindProposal(1, pp));
        BindLeaves(&replay, &ts, 1, 1);
        std::string err;
        TASK_CHECK(!replay.Record(ts.scene, MakeRays(6, 4u), 2, &err));
        TASK_CHECK(!err.empty());
    }

    // An area-lit primitive is rejected even with a point light also in the
    // scene (the primitive's GetAreaLight must not be non-null).
    {
        TestScene ts;
        ts.materials.push_back(MakePlastic(kd, ks, .2f));
        Transform id;
        int meshIds[6] = {0, 1, 2, 0, 2, 3};
        Point pts[4] = {
            Point(-3, 0, 3), Point(3, 0, 3), Point(3, 0, -1.5f),
            Point(-3, 0, -1.5f)};
        Reference<Shape> areaShape = new TriangleMesh(
            &id, &id, false, 2, 4, meshIds, pts, NULL, NULL, NULL,
            Reference<Texture<float> >());
        AreaLight *area = new DiffuseAreaLight(
            Transform(), Spectrum(2.f), 1, areaShape);
        Reference<Primitive> prim =
            new GeometricPrimitive(areaShape, ts.materials[0], area);
        std::vector<Reference<Primitive> > r = Refine(prim);
        for (size_t i = 0; i < r.size(); ++i) {
            ts.leaves.push_back(r[i]);
            ts.leafMaterial.push_back(0);
        }
        ts.lights.push_back(new PointLight(
            Translate(Vector(0.8f, 1.6f, 0.6f)), Spectrum(3.f)));
        ts.scene = new Scene(new BVHAccel(ts.leaves), ts.lights, NULL);

        MaterialReplay replay;
        TASK_CHECK(replay.BindProposal(1, pp));
        BindLeaves(&replay, &ts, 1, 1);
        std::vector<MaterialReplayRay> rays = MakeRays(6, 5u);
        std::string err;
        TASK_CHECK(!replay.Record(ts.scene, rays, 2, &err));
        TASK_CHECK(!err.empty());
    }

    // Parameter validation.
    {
        MaterialReplay replay;
        MaterialReplayParams bad = pp;
        bad.roughness = 0.005f;
        TASK_CHECK(!replay.BindProposal(1, bad));
        bad = pp; bad.kd[1] = -0.1f;
        TASK_CHECK(!replay.BindProposal(1, bad));
        MaterialReplayParams zeroKs = pp; zeroKs.ks[0] = 0.f;
        TASK_CHECK(!replay.BindProposal(1, zeroKs));
        MaterialReplayParams inf = pp; inf.kd[0] = INFINITY;
        TASK_CHECK(!replay.BindProposal(1, inf));
    }
    return 0;
}


// Gradient cases ------------------------------------------------------------

struct GradientFixture {
    TestScene *ts;
    MaterialReplay *replay;
    std::vector<MaterialReplayRay> rays;
    std::vector<Spectrum> adjoint;
    static const int kIds = 2;

    double Scalar(const std::vector<MaterialReplayParams> &p) const {
        std::vector<Spectrum> rgb;
        replay->Evaluate(p, &rgb);
        double total = 0.0;
        for (size_t r = 0; r < rgb.size(); ++r) {
            float a[3], b[3]; adjoint[r].ToRGB(a); rgb[r].ToRGB(b);
            for (int c = 0; c < 3; ++c) total += a[c] * b[c];
        }
        return total / std::max<size_t>(1, rgb.size());
    }
};


GradientFixture *MakeGradientFixture() {
    GradientFixture *f = new GradientFixture;
    const float kdA[3] = {0.4f, 0.3f, 0.2f};
    const float ksA[3] = {0.25f, 0.2f, 0.15f};
    const float kdB[3] = {0.15f, 0.35f, 0.3f};
    const float ksB[3] = {0.3f, 0.2f, 0.25f};
    f->ts = BuildCornerScene(kdA, ksA, .2f, kdB, ksB, .45f, false);

    MaterialReplayParams pa = MakeParams(.4f, .3f, .2f, .25f, .2f, .15f, .2f);
    MaterialReplayParams pb = MakeParams(.15f, .35f, .3f, .3f, .2f, .25f, .45f);
    f->replay = new MaterialReplay;
    f->replay->BindProposal(10, pa);
    f->replay->BindProposal(20, pb);
    BindLeaves(f->replay, f->ts, 10, 20);

    f->rays = MakeRays(24, 9000u);
    f->replay->Record(f->ts->scene, f->rays, 4);
    bool two = false;
    for (int i = 0; i < f->replay->NumRays(); ++i)
        if (f->replay->PathNumVertices(i) >= 2) two = true;
    if (!two) { /* fixture relies on real two-bounce paths */ }

    RNG arng(12345u);
    for (size_t r = 0; r < f->rays.size(); ++r) {
        float g[3] = {arng.RandomFloat() - .3f,
                      arng.RandomFloat() - .5f,
                      arng.RandomFloat() - .4f};
        f->adjoint.push_back(MakeSpectrum(g));
    }
    return f;
}


void AnalyticGradient(const GradientFixture &f,
                      const std::vector<MaterialReplayParams> &p,
                      std::vector<MaterialReplayGradient> *g) {
    f.replay->Backward(p, f.adjoint, g);
    double inv = 1.0 / std::max<size_t>(1, f.rays.size());
    for (size_t i = 0; i < g->size(); ++i) {
        for (int c = 0; c < 3; ++c) {
            (*g)[i].dKd[c] *= float(inv);
            (*g)[i].dKs[c] *= float(inv);
        }
        (*g)[i].dRoughness *= float(inv);
    }
}


int GradientCentralDifferences() {
    GradientFixture *f = MakeGradientFixture();
    std::vector<MaterialReplayParams> base(2);
    base[0] = MakeParams(.4f, .3f, .2f, .25f, .2f, .15f, .2f);
    base[1] = MakeParams(.15f, .35f, .3f, .3f, .2f, .25f, .45f);

    std::vector<MaterialReplayGradient> analytic;
    AnalyticGradient(*f, base, &analytic);

    const float steps[2] = {0.01f, 0.0025f};
    struct Entry { int id; int kind; int channel; };
    std::vector<Entry> entries;
    for (int id = 0; id < GradientFixture::kIds; ++id) {
        for (int c = 0; c < 3; ++c) entries.push_back({id, 0, c}); // Kd
        for (int c = 0; c < 3; ++c) entries.push_back({id, 1, c}); // Ks
        entries.push_back({id, 2, 0});                              // roughness
    }

    for (int si = 0; si < 2; ++si) {
        float h = steps[si];
        std::printf("GRADIENT central-difference step=%.5f\n", h);
        for (size_t e = 0; e < entries.size(); ++e) {
            std::vector<MaterialReplayParams> lo = base, hi = base;
            float *plo, *phi;
            const Entry &en = entries[e];
            if (en.kind == 0) {
                plo = lo[en.id].kd; phi = hi[en.id].kd;
            } else if (en.kind == 1) {
                plo = lo[en.id].ks; phi = hi[en.id].ks;
            } else {
                plo = &lo[en.id].roughness; phi = &hi[en.id].roughness;
            }
            plo[en.channel] -= h; phi[en.channel] += h;
            double fd = (f->Scalar(hi) - f->Scalar(lo)) / (2.0 * h);
            float an = en.kind == 0 ? analytic[en.id].dKd[en.channel]
                       : en.kind == 1 ? analytic[en.id].dKs[en.channel]
                                      : analytic[en.id].dRoughness;
            double err = std::fabs(fd - an);
            std::printf("  id=%d kind=%d ch=%d analytic=%.6f fd=%.6f "
                        "abs_err=%.3e\n", en.id, en.kind, en.channel,
                        an, fd, err);
            TASK_CHECK(err <= 3e-3 + 8e-3 * std::fabs(an));
        }
    }
    delete f->replay; delete f->ts; delete f;
    return 0;
}


int GradientEndpointsAndZeroAlbedo() {
    GradientFixture *f = MakeGradientFixture();

    // Roughness at its lower endpoint: one-sided interior derivative.  The
    // Blinn exponent e = 1/roughness is stiff there, so use small steps and
    // verify convergence toward the analytic value.
    std::vector<MaterialReplayParams> p(2);
    p[0] = MakeParams(.3f, .3f, .3f, .2f, .2f, .2f, 0.01f);
    p[1] = MakeParams(.2f, .2f, .2f, .25f, .25f, .25f, .5f);
    std::vector<MaterialReplayGradient> g;
    AnalyticGradient(*f, p, &g);
    const float endSteps[2] = {1e-4f, 2.5e-5f};
    double s0 = f->Scalar(p);
    for (int si = 0; si < 2; ++si) {
        float h = endSteps[si];
        std::vector<MaterialReplayParams> up = p;
        up[0].roughness += h;
        double fdRough = (f->Scalar(up) - s0) / h;
        std::printf("GRADIENT endpoint roughness step=%.2e analytic=%.6f "
                    "onesided_fd=%.6f abs_err=%.3e\n",
                    h, g[0].dRoughness, fdRough,
                    std::fabs(fdRough - g[0].dRoughness));
        if (si == 1)
            TASK_CHECK(std::fabs(fdRough - g[0].dRoughness) <=
                       3e-3 + 8e-3 * std::fabs(g[0].dRoughness));
    }

    // Zero target Kd: its derivative must survive without color division.
    p[0] = MakeParams(0, 0, 0, .2f, .2f, .2f, .3f);
    p[1] = MakeParams(.2f, .2f, .2f, .25f, .25f, .25f, .5f);
    AnalyticGradient(*f, p, &g);
    int kdDerivNonZero = 0;
    for (int c = 0; c < 3; ++c) if (g[0].dKd[c] != 0.f) ++kdDerivNonZero;
    TASK_CHECK(kdDerivNonZero == 3);

    // Zero target Ks still leaves a finite, generally nonzero roughness
    // derivative path through the Blinn normalization.
    p[0] = MakeParams(.3f, .3f, .3f, 0, 0, 0, .3f);
    AnalyticGradient(*f, p, &g);
    TASK_CHECK(std::isfinite(g[0].dRoughness));
    TASK_CHECK(std::isfinite(g[0].dKd[0]));

    // Kd at its upper endpoint one-sided check.
    p[0] = MakeParams(1, 1, 1, .2f, .2f, .2f, .3f);
    AnalyticGradient(*f, p, &g);
    const float kdStep = 2.5e-5f;
    std::vector<MaterialReplayParams> down = p;
    down[0].kd[0] -= kdStep;
    double fdKd = (f->Scalar(p) - f->Scalar(down)) / kdStep;
    TASK_CHECK(std::fabs(fdKd - g[0].dKd[0]) <=
               3e-3 + 8e-3 * std::fabs(g[0].dKd[0]));

    // Kd at its lower endpoint (zero albedo): the one-sided derivative must
    // remain the interior derivative without any color division.
    p[0] = MakeParams(0, 0, 0, .2f, .2f, .2f, .3f);
    AnalyticGradient(*f, p, &g);
    std::vector<MaterialReplayParams> up = p;
    up[0].kd[1] += kdStep;
    double fdKd0 = (f->Scalar(up) - f->Scalar(p)) / kdStep;
    TASK_CHECK(std::fabs(fdKd0 - g[0].dKd[1]) <=
               3e-3 + 8e-3 * std::fabs(g[0].dKd[1]));

    // Roughness at its upper endpoint one-sided check.
    p[0] = MakeParams(.3f, .3f, .3f, .2f, .2f, .2f, 1.f);
    AnalyticGradient(*f, p, &g);
    const float roughStep = 1e-4f;
    down = p;
    down[0].roughness -= roughStep;
    double fdRoughHi = (f->Scalar(p) - f->Scalar(down)) / roughStep;
    std::printf("GRADIENT endpoint roughness=1 step=%.2e analytic=%.6f "
                "onesided_fd=%.6f abs_err=%.3e\n",
                roughStep, g[0].dRoughness, fdRoughHi,
                std::fabs(fdRoughHi - g[0].dRoughness));
    TASK_CHECK(std::fabs(fdRoughHi - g[0].dRoughness) <=
               3e-3 + 8e-3 * std::fabs(g[0].dRoughness));
    delete f->replay; delete f->ts; delete f;
    return 0;
}


int GradientSharedIdAccumulation() {
    const float kd[3] = {0.35f, 0.25f, 0.2f};
    const float ks[3] = {0.2f, 0.2f, 0.2f};
    TestScene *ts = BuildCornerScene(kd, ks, .25f, kd, ks, .25f, true);
    MaterialReplayParams pp = MakeParams(.35f, .25f, .2f, .2f, .2f, .2f, .25f);
    MaterialReplay replay;
    TASK_CHECK(replay.BindProposal(8, pp));
    BindLeaves(&replay, ts, 8, 8);

    std::vector<MaterialReplayRay> rays = MakeRays(20, 11000u);
    TASK_CHECK(replay.Record(ts->scene, rays, 4));
    int multi = 0;
    for (int i = 0; i < replay.NumRays(); ++i)
        if (replay.PathNumVertices(i) >= 2) ++multi;
    TASK_CHECK(multi > 0); // the same id is hit repeatedly on these paths

    std::vector<Spectrum> adj;
    for (size_t r = 0; r < rays.size(); ++r)
        adj.push_back(Spectrum(1.f));
    std::vector<MaterialReplayGradient> g1, g2;
    TASK_CHECK(replay.Backward(std::vector<MaterialReplayParams>(1, pp),
                               adj, &g1));
    TASK_CHECK(replay.Backward(std::vector<MaterialReplayParams>(1, pp),
                               adj, &g2));
    // Restarts begin from zero and therefore agree exactly.
    for (int c = 0; c < 3; ++c) {
        TASK_CHECK(g1[0].dKd[c] == g2[0].dKd[c]);
        TASK_CHECK(g1[0].dKs[c] == g2[0].dKs[c]);
    }
    TASK_CHECK(g1[0].dRoughness == g2[0].dRoughness);

    // Every repeated occurrence accumulates: the gradient must be nonzero.
    int nonzero = 0;
    for (int c = 0; c < 3; ++c) {
        if (g1[0].dKd[c] != 0.f) ++nonzero;
        if (g1[0].dKs[c] != 0.f) ++nonzero;
    }
    TASK_CHECK(nonzero == 6);
    delete ts;
    return 0;
}


int GradientIllegalTargets() {
    GradientFixture *f = MakeGradientFixture();
    std::vector<MaterialReplayParams> bad(2);
    bad[0] = MakeParams(.3f, .3f, .3f, .2f, .2f, .2f, .2f);
    bad[1] = MakeParams(.2f, .2f, .2f, .25f, .25f, .25f, .5f);
    std::vector<MaterialReplayGradient> g;
    TASK_CHECK(f->replay->Backward(bad, f->adjoint, &g));
    TASK_CHECK(g.size() == 2);

    bad[1].roughness = 1.5f;
    std::string err;
    TASK_CHECK(!f->replay->Backward(bad, f->adjoint, &g, &err));
    TASK_CHECK(!err.empty());
    TASK_CHECK(g.size() == 2); // existing output is not corrupted

    std::vector<Spectrum> wrongAdj(f->adjoint.size() / 2);
    TASK_CHECK(!f->replay->Backward(
        std::vector<MaterialReplayParams>(2, MaterialReplayParams()),
        wrongAdj, &g));
    delete f->replay; delete f->ts; delete f;
    return 0;
}


// Ownership cases -----------------------------------------------------------

MaterialReplay *RecordInDisposableScene(
        std::vector<MaterialReplayRay> *raysOut,
        std::vector<MaterialReplayParams> *proposalOut) {
    const float kdA[3] = {0.4f, 0.3f, 0.2f};
    const float ksA[3] = {0.2f, 0.2f, 0.2f};
    const float kdB[3] = {0.2f, 0.4f, 0.3f};
    const float ksB[3] = {0.25f, 0.2f, 0.15f};
    MaterialReplay *replay = new MaterialReplay;
    MaterialReplayParams pa = MakeParams(.4f, .3f, .2f, .2f, .2f, .2f, .2f);
    MaterialReplayParams pb = MakeParams(.2f, .4f, .3f, .25f, .2f, .15f, .4f);
    replay->BindProposal(30, pa);
    replay->BindProposal(40, pb);
    proposalOut->push_back(pa); proposalOut->push_back(pb);

    {
        TestScene tsHolder;
        tsHolder.materials.push_back(MakePlastic(kdA, ksA, .2f));
        tsHolder.materials.push_back(MakePlastic(kdB, ksB, .4f));
        Reference<Primitive> floor = MakeRect(
            Point(-3, 0, 3), Point(3, 0, 3), Point(3, 0, -1.5f),
            Point(-3, 0, -1.5f), tsHolder.materials[0]);
        Reference<Primitive> wall = MakeRect(
            Point(-3, 0, -1.5f), Point(3, 0, -1.5f),
            Point(3, 3, -1.5f), Point(-3, 3, -1.5f),
            tsHolder.materials[1]);
        std::vector<Reference<Primitive> > l0 = Refine(floor);
        std::vector<Reference<Primitive> > l1 = Refine(wall);
        for (size_t i = 0; i < l0.size(); ++i) {
            tsHolder.leaves.push_back(l0[i]);
            tsHolder.leafMaterial.push_back(0);
        }
        for (size_t i = 0; i < l1.size(); ++i) {
            tsHolder.leaves.push_back(l1[i]);
            tsHolder.leafMaterial.push_back(1);
        }
        tsHolder.lights.push_back(new PointLight(
            Translate(Vector(0.7f, 1.5f, 0.5f)), Spectrum(3.f)));
        Scene scene(new BVHAccel(tsHolder.leaves), tsHolder.lights, NULL);
        BindLeaves(replay, &tsHolder, 30, 40);

        MemoryArena localArena; // destroyed with this scope
        *raysOut = MakeRays(16, 13000u);
        std::string err;
        if (!replay->Record(&scene, *raysOut, 4, &err)) {
            delete replay;
            return NULL;
        }
        (void)localArena;
        // Scene, MemoryArena and every material leave scope here.
    }
    return replay;
}


int OwnershipAfterSceneDestroyed() {
    std::vector<MaterialReplayRay> rays;
    std::vector<MaterialReplayParams> proposal;
    MaterialReplay *replay = RecordInDisposableScene(&rays, &proposal);
    TASK_CHECK(replay != NULL);
    TASK_CHECK(replay->NumRays() == int(rays.size()));

    std::vector<Spectrum> rgb;
    TASK_CHECK(replay->Evaluate(proposal, &rgb));
    int nonzero = 0;
    for (size_t r = 0; r < rgb.size(); ++r) {
        float v[3]; rgb[r].ToRGB(v);
        for (int c = 0; c < 3; ++c) {
            TASK_CHECK(std::isfinite(v[c]));
            if (v[c] != 0.f) ++nonzero;
        }
    }
    TASK_CHECK(nonzero > 0);

    std::vector<Spectrum> adj(rays.size(), Spectrum(1.f));
    std::vector<MaterialReplayGradient> g;
    TASK_CHECK(replay->Backward(proposal, adj, &g));
    TASK_CHECK(g.size() == 2);
    for (size_t i = 0; i < g.size(); ++i)
        for (int c = 0; c < 3; ++c) {
            TASK_CHECK(std::isfinite(g[i].dKd[c]));
            TASK_CHECK(std::isfinite(g[i].dKs[c]));
        }

    // Editing targets long after the source scene is gone still works.
    std::vector<MaterialReplayParams> edited = proposal;
    edited[0].kd[0] = 0.05f; edited[1].roughness = 0.9f;
    std::vector<Spectrum> rgb2;
    TASK_CHECK(replay->Evaluate(edited, &rgb2));
    delete replay;
    return 0;
}


int OwnershipInterleavedRestarts() {
    std::vector<MaterialReplayRay> rays;
    std::vector<MaterialReplayParams> proposal;
    MaterialReplay *replay = RecordInDisposableScene(&rays, &proposal);
    TASK_CHECK(replay != NULL);

    std::vector<Spectrum> adj(rays.size(), Spectrum(1.f));
    std::vector<Spectrum> ref;
    TASK_CHECK(replay->Evaluate(proposal, &ref));

    for (int round = 0; round < 4; ++round) {
        std::vector<MaterialReplayParams> targets = proposal;
        targets[0].roughness = 0.1f + 0.2f * round;
        targets[1].ks[2] = 0.1f + 0.05f * round;

        std::vector<Spectrum> rgb;
        TASK_CHECK(replay->Evaluate(targets, &rgb));
        std::vector<MaterialReplayGradient> g;
        TASK_CHECK(replay->Backward(targets, adj, &g));

        // Interleave a second evaluation; results must be identical because
        // every call starts from scratch.
        std::vector<Spectrum> rgbAgain;
        TASK_CHECK(replay->Evaluate(targets, &rgbAgain));
        for (size_t r = 0; r < rgb.size(); ++r) {
            float a[3], b[3]; rgb[r].ToRGB(a); rgbAgain[r].ToRGB(b);
            for (int c = 0; c < 3; ++c) TASK_CHECK(a[c] == b[c]);
        }
        std::vector<MaterialReplayGradient> gAgain;
        TASK_CHECK(replay->Backward(targets, adj, &gAgain));
        for (size_t i = 0; i < g.size(); ++i) {
            for (int c = 0; c < 3; ++c) {
                TASK_CHECK(g[i].dKd[c] == gAgain[i].dKd[c]);
                TASK_CHECK(g[i].dKs[c] == gAgain[i].dKs[c]);
            }
            TASK_CHECK(g[i].dRoughness == gAgain[i].dRoughness);
        }
    }
    delete replay;
    return 0;
}


class TinyVolume : public VolumeRegion {
public:
    BBox WorldBound() const { return BBox(Point(-1,-1,-1), Point(1,1,1)); }
    bool IntersectP(const Ray &, float *, float *) const { return false; }
    Spectrum sigma_a(const Point &, const Vector &, float) const {
        return Spectrum(0.f);
    }
    Spectrum sigma_s(const Point &, const Vector &, float) const {
        return Spectrum(0.f);
    }
    Spectrum Lve(const Point &, const Vector &, float) const {
        return Spectrum(0.f);
    }
    float p(const Point &, const Vector &, const Vector &, float) const {
        return 0.f;
    }
    Spectrum tau(const Ray &, float, float) const { return Spectrum(0.f); }
};


int OwnershipAtomicRejection() {
    const float kd[3] = {0.3f, 0.3f, 0.3f};
    const float ks[3] = {0.2f, 0.2f, 0.2f};
    MaterialReplayParams pp = MakeParams(.3f, .3f, .3f, .2f, .2f, .2f, .2f);

    // A good record first.
    TestScene *good = BuildCornerScene(kd, ks, .2f, kd, ks, .2f, true);
    MaterialReplay replay;
    TASK_CHECK(replay.BindProposal(6, pp));
    BindLeaves(&replay, good, 6, 6);
    std::vector<MaterialReplayRay> rays = MakeRays(8, 15000u);
    TASK_CHECK(replay.Record(good->scene, rays, 3));
    std::vector<Spectrum> before;
    TASK_CHECK(replay.Evaluate(std::vector<MaterialReplayParams>(1, pp),
                               &before));
    TASK_CHECK(replay.NumRays() == int(rays.size()));

    // Media are rejected and leave the existing record untouched.  The
    // temporary Scene owns its own lights and heap volume region.
    {
        std::vector<Light *> ownLights;
        ownLights.push_back(new PointLight(
            Translate(Vector(0.8f, 1.6f, 0.6f)), Spectrum(3.f)));
        TinyVolume *volume = new TinyVolume;
        Scene withMedia(new BVHAccel(good->leaves), ownLights, volume);
        std::string err;
        TASK_CHECK(!replay.Record(&withMedia, MakeRays(2, 1u), 2, &err));
        TASK_CHECK(!err.empty());
    }
    // Illegal ray batches reject atomically as well.
    {
        std::vector<MaterialReplayRay> bad = rays;
        bad[0].direction = Vector(0, 0, 0);
        std::string err;
        TASK_CHECK(!replay.Record(good->scene, bad, 3, &err));
    }
    std::vector<Spectrum> after;
    TASK_CHECK(replay.Evaluate(std::vector<MaterialReplayParams>(1, pp),
                               &after));
    TASK_CHECK(after.size() == before.size());
    for (size_t r = 0; r < before.size(); ++r) {
        float a[3], b[3]; before[r].ToRGB(a); after[r].ToRGB(b);
        for (int c = 0; c < 3; ++c) TASK_CHECK(a[c] == b[c]);
    }

    // Invalid target parameters do not damage the record either.
    MaterialReplayParams illegal = pp; illegal.kd[2] = 2.f;
    std::vector<Spectrum> untouched;
    TASK_CHECK(!replay.Evaluate(std::vector<MaterialReplayParams>(1, illegal),
                                &untouched));
    TASK_CHECK(replay.Evaluate(std::vector<MaterialReplayParams>(1, pp),
                               &after));
    delete good;
    return 0;
}

} // anonymous namespace


// Test table consumed by task_support/driver.c (compiled as C++ in this
// harness, so default language linkage matches check.h's declarations).
const struct task_case task_cases[] = {
    {"primal", "production_path_parity", PrimalProductionParity},
    {"primal", "direct_light_independent_expression", PrimalDirectLightExpression},
    {"primal", "zero_albedo_and_black_background", PrimalZeroAlbedoAndMiss},
    {"primal", "occlusion_finite_shadows", PrimalOcclusion},
    {"primal", "failed_sample_zero_contribution", PrimalFailedSample},
    {"primal", "multiple_point_lights", PrimalMultipleLights},
    {"primal", "target_reweight_integral", PrimalTargetReweightIntegral},
    {"primal", "hemisphere_reweight_invariance", PrimalHemisphereInvariance},
    {"primal", "reject_unsupported_features", PrimalRejectUnsupported},
    {"gradient", "central_differences_two_steps", GradientCentralDifferences},
    {"gradient", "endpoints_and_zero_albedo", GradientEndpointsAndZeroAlbedo},
    {"gradient", "shared_id_repeats_and_restarts", GradientSharedIdAccumulation},
    {"gradient", "illegal_targets_rejected", GradientIllegalTargets},
    {"ownership", "evaluate_after_scene_destroyed", OwnershipAfterSceneDestroyed},
    {"ownership", "interleaved_evaluate_backward_restarts", OwnershipInterleavedRestarts},
    {"ownership", "atomic_rejection_keeps_record", OwnershipAtomicRejection},
};
const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
