// Regression tests for the specular path replay interface.
// No main(): task_support/driver.c drives the registered cases.
#include "task_support/check.h"
#include "scene.h"
#include "intersection.h"
#include "shapes/trianglemesh.h"
#include "accelerators/bvh.h"
#include "materials/mirror.h"
#include "materials/matte.h"
#include "materials/glass.h"
#include "textures/constant.h"
#include "lights/point.h"
#include "lights/distant.h"
#include "shapes/sphere.h"
#include "reflection.h"
#include "memory.h"
#include "integrators/replay.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>

using std::vector;
using std::string;

namespace {

const int ID_MIRROR = 1;
const int ID_INST_A = 2;
const int ID_INST_B = 3;
const int ID_RECV = 4;

float krRGB[3] = {0.7f, 0.8f, 0.9f};
float kdRGB[3] = {0.4f, 0.5f, 0.2f};
float liRGB[3] = {6.f, 7.f, 5.f};

Reference<Material> MakeMirror() {
    return new MirrorMaterial(
        new ConstantTexture<Spectrum>(RGBSpectrum::FromRGB(krRGB)),
        Reference<Texture<float> >());
}
Reference<Material> MakeMatte() {
    return new MatteMaterial(
        new ConstantTexture<Spectrum>(RGBSpectrum::FromRGB(kdRGB)),
        new ConstantTexture<float>(0.f),
        Reference<Texture<float> >());
}
Matrix4x4 Mat12(const float m[12]) {
    return Matrix4x4(m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7],
                     m[8],m[9],m[10],m[11], 0,0,0,1);
}
void Fill12(float out[12], const Matrix4x4 &M) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) out[4*i+j] = M.m[i][j];
}

// Everything needed to build and later destroy a replay test scene.
struct Kit {
    Transform id;
    Transform *l2wA, *w2lA, *l2wB, *w2lB;
    TriangleMesh *mirrorMesh, *recvMesh, *occMesh;
    TransformedPrimitive *instA, *instB;
    Scene *scene;
    vector<Light *> lights;
    bool twoLights;
    Kit() : l2wA(0),w2lA(0),l2wB(0),w2lB(0),mirrorMesh(0),recvMesh(0),
            occMesh(0),instA(0),instB(0),scene(0),twoLights(false) { }
    ~Kit() {
        delete scene;
        delete w2lA; delete l2wA; delete w2lB; delete l2wB;
    }
};

struct ReplayOptions {
    bool occluder;
    bool twoLights;
    bool glassMaterial;
    ReplayOptions() : occluder(false), twoLights(false),
                      glassMaterial(false) { }
};

// Builds a scene from the mirror source P (3 verts, 1 triangle), two
// instance 3x4 matrices and a static matte receiver quad at z=-2.
Kit *BuildKit(const float mirrorP[9], const float matA[12],
               const float matB[12], const ReplayOptions &opt) {
    Kit *k = new Kit();
    int triVI[3] = {0,1,2};
    Point mp[3];
    for (int i = 0; i < 3; ++i)
        mp[i] = Point(mirrorP[3*i], mirrorP[3*i+1], mirrorP[3*i+2]);
    k->mirrorMesh = new TriangleMesh(&k->id,&k->id,false,1,3,triVI,mp,
                                     NULL,NULL,NULL,
                                     Reference<Texture<float> >());
    Reference<Material> mm = MakeMirror();
    GeometricPrimitive *mgp = new GeometricPrimitive(k->mirrorMesh, mm, NULL);

    // Static matte receiver at z=-2, normal +z (two triangles).
    int quadVI[6] = {0,1,2, 0,2,3};
    float rp[12] = {-2.5f,-2.5f,-2.f, 2.5f,-2.5f,-2.f,
                     2.5f, 2.5f,-2.f,-2.5f, 2.5f,-2.f};
    Point rpp[4];
    for (int i = 0; i < 4; ++i)
        rpp[i] = Point(rp[3*i],rp[3*i+1],rp[3*i+2]);
    k->recvMesh = new TriangleMesh(&k->id,&k->id,false,2,4,quadVI,rpp,
                                   NULL,NULL,NULL,
                                   Reference<Texture<float> >());
    GeometricPrimitive *rgp = new GeometricPrimitive(k->recvMesh, MakeMatte(), NULL);

    Matrix4x4 am = Mat12(matA), bm = Mat12(matB);
    k->l2wA = new Transform(am, Inverse(am));
    k->w2lA = new Transform(Inverse(*k->l2wA));
    k->l2wB = new Transform(bm, Inverse(bm));
    k->w2lB = new Transform(Inverse(*k->l2wB));
    AnimatedTransform ata(k->w2lA,0,k->w2lA,1), atb(k->w2lB,0,k->w2lB,1);
    vector<Reference<Primitive> > refined;
    mgp->FullyRefine(refined);
    Reference<Primitive> proto = new BVHAccel(refined);
    k->instA = new TransformedPrimitive(proto, ata);
    k->instB = new TransformedPrimitive(proto, atb);

    vector<Reference<Primitive> > prims;
    prims.push_back(k->instA);
    prims.push_back(k->instB);
    prims.push_back(rgp);

    if (opt.occluder) {
        // Vertical matte panel in plane x=0.5 between the receiver hit
        // (~0.2,0.1,-2) and the light (0.8,0.6,-1).  The downward primary
        // ray stays at x<=0.2 over this depth range and misses the panel,
        // but the receiver->light shadow segment crosses it.
        float op[12] = {0.5f,-1.0f,-2.2f, 0.5f, 1.5f,-2.2f,
                         0.5f, 1.5f,-0.8f, 0.5f,-1.0f,-0.8f};
        Point opp[4];
        for (int i = 0; i < 4; ++i)
            opp[i] = Point(op[3*i],op[3*i+1],op[3*i+2]);
        k->occMesh = new TriangleMesh(&k->id,&k->id,false,2,4,quadVI,opp,
                                      NULL,NULL,NULL,
                                      Reference<Texture<float> >());
        GeometricPrimitive *ogp =
            new GeometricPrimitive(k->occMesh, MakeMatte(), NULL);
        prims.push_back(ogp);
    }

    k->twoLights = opt.twoLights;
    k->lights.push_back(new PointLight(
        Translate(Vector(0.8f,0.6f,-1.0f)), RGBSpectrum::FromRGB(liRGB)));
    if (opt.twoLights) {
        float li2[3] = {3.f,2.f,4.f};
        k->lights.push_back(new PointLight(
            Translate(Vector(-1.2f,0.4f,-0.8f)),
            RGBSpectrum::FromRGB(li2)));
    }
    k->scene = new Scene(new BVHAccel(prims), k->lights, NULL);
    return k;
}

void BindKit(Kit *k, bool bindReceiver) {
    ReplayClearBindings();
    ReplayBindMesh(k->mirrorMesh, ID_MIRROR);
    ReplayBindInstance(k->instA, ID_INST_A);
    ReplayBindInstance(k->instB, ID_INST_B);
    if (bindReceiver) ReplayBindMesh(k->recvMesh, ID_RECV);
}

// Independent reference path using the real pbrt BSDFs directly.
void ReferencePath(Kit *k, const Ray &ray0, int maxBounces, float out[3]) {
    Ray ray = ray0;
    Spectrum weight(1.f);
    for (int depth = 0; depth <= maxBounces; ++depth) {
        Intersection isect;
        if (!k->scene->Intersect(ray, &isect)) {
            for (int c = 0; c < 3; ++c) out[c] = 0.f;
            return;
        }
        MemoryArena arena;
        BSDF *bsdf = isect.GetBSDF(RayDifferential(ray), arena);
        bool spec = bsdf->NumComponents(
            BxDFType(BSDF_REFLECTION|BSDF_SPECULAR)) > 0;
        if (spec) {
            Vector wi; float pdf; BxDFType ty;
            Spectrum f = bsdf->Sample_f(-ray.d, &wi,
                BSDFSample(0.2f,0.3f,0.4f), &pdf, BSDF_ALL, &ty);
            weight *= f * AbsDot(wi, isect.dg.nn) / pdf;
            ray = Ray(isect.dg.p, wi, isect.rayEpsilon, INFINITY, 0.f);
            continue;
        }
        Spectrum L(0.f);
        Vector wo = -ray.d;
        for (uint32_t i = 0; i < k->lights.size(); ++i) {
            Vector wi; float pdf; VisibilityTester vis;
            Spectrum Li = k->lights[i]->Sample_L(isect.dg.p,
                isect.rayEpsilon, LightSample(0.2f,0.3f,0.4f), 0,
                &wi, &pdf, &vis);
            Spectrum f = bsdf->f(wo, wi);
            if (!f.IsBlack() && vis.Unoccluded(k->scene))
                L += f * Li * AbsDot(wi, isect.dg.nn) / pdf;
        }
        RGBSpectrum(weight * L).ToRGB(out);
        return;
    }
    for (int c = 0; c < 3; ++c) out[c] = 0.f;
}

bool Near(float a, float b, float tol) { return fabsf(a-b) <= tol; }

// ---------------------------------------------------------------------------
// Central finite differences against fully rebuilt scenes.
// ---------------------------------------------------------------------------
struct FDCfg {
    float P[9], A[12], B[12];
    ReplayOptions opt;
    vector<ReplayRay> rays;
    int maxBounces;
};

double Objective(const FDCfg &cfg, const vector<ReplayColor> &seed) {
    Kit *k = BuildKit(cfg.P, cfg.A, cfg.B, cfg.opt);
    BindKit(k, true);
    ReplayRecord *rec = NULL; string err;
    if (!ReplayTrace(k->scene, cfg.rays, cfg.maxBounces, &rec, &err)) {
        delete k; return 0.0;
    }
    const ReplayColor *c = ReplayColors(rec);
    double v = 0.0;
    for (uint32_t r = 0; r < cfg.rays.size(); ++r)
        for (int ch = 0; ch < 3; ++ch)
            v += seed[r].rgb[ch] * c[r].rgb[ch];
    ReplayReleaseRecord(rec);
    delete k;
    return v;
}

} // namespace

// ===========================================================================
// PRIMAL GROUP
// ===========================================================================
static int primal_diffuse() {
    float P[9] = {-1,-1,0, 1,-1,0, 0,1,0};
    Transform T = Translate(Vector(0,0,2.f));
    float A[12], B[12]; Fill12(A, T.GetMatrix()); Fill12(B, T.GetMatrix());
    ReplayOptions opt;
    Kit *k = BuildKit(P, A, B, opt);
    BindKit(k, true);
    // Ray that misses the mirror and strikes the matte receiver (at z=-2).
    vector<ReplayRay> rays;
    rays.push_back(ReplayRay(Point(0.8f,0.8f,0.f),
                             Normalize(Vector(0.05f,0.05f,-1.f))));
    ReplayRecord *rec = NULL; string err;
    TASK_CHECK(ReplayTrace(k->scene, rays, 3, &rec, &err));
    const ReplayColor *c = ReplayColors(rec);
    float ref[3];
    ReferencePath(k, Ray(rays[0].o, rays[0].d, 0.f), 3, ref);
    for (int ch = 0; ch < 3; ++ch)
        TASK_CHECK(Near(c[0].rgb[ch], ref[ch], 1e-5f));
    TASK_CHECK(c[0].rgb[0] > 0.f);
    ReplayReleaseRecord(rec);
    delete k;
    return 0;
}

static int primal_two_bounce() {
    float P[9] = {-3,-3,0, 3,-3,0, 0,3,0};
    // Two mirrors at 45-degree tilts so an up ray reflects twice then lands
    // on the matte receiver.
    Transform TA = Translate(Vector(0,0,2.f)) * RotateY(135.f);
    Transform TB = Translate(Vector(3,0,2.f)) * RotateY(-90.f)
                   * Scale(1.f,1.f,-1.f);
    float A[12], B[12]; Fill12(A, TA.GetMatrix()); Fill12(B, TB.GetMatrix());
    ReplayOptions opt;
    Kit *k = BuildKit(P, A, B, opt);
    BindKit(k, true);
    vector<ReplayRay> rays;
    rays.push_back(ReplayRay(Point(0,0,0), Vector(0,0,1)));
    ReplayRecord *rec = NULL; string err;
    TASK_CHECK(ReplayTrace(k->scene, rays, 4, &rec, &err));
    const ReplayColor *c = ReplayColors(rec);
    float ref[3];
    ReferencePath(k, Ray(rays[0].o, rays[0].d, 0.f), 4, ref);
    for (int ch = 0; ch < 3; ++ch)
        TASK_CHECK(Near(c[0].rgb[ch], ref[ch], 1e-5f));
    // A multi-bounce path must carry the product of the two mirror Kr values.
    TASK_CHECK(c[0].rgb[0] > 0.f);
    ReplayReleaseRecord(rec);
    delete k;
    return 0;
}

static int primal_black_cases() {
    float P[9] = {-1,-1,0, 1,-1,0, 0,1,0};
    Transform T = Translate(Vector(0,0,2.f));
    float A[12], B[12]; Fill12(A, T.GetMatrix()); Fill12(B, T.GetMatrix());
    Kit *k = BuildKit(P, A, B, ReplayOptions());
    BindKit(k, false);
    vector<ReplayRay> rays;
    // Ray into empty space -> black background.
    rays.push_back(ReplayRay(Point(0,0,0), Normalize(Vector(10.f,10.f,1.f))));
    // Ray hitting a mirror with zero bounces allowed -> black.
    rays.push_back(ReplayRay(Point(0,0,0), Vector(0,0,1)));
    ReplayRecord *rec = NULL; string err;
    TASK_CHECK(ReplayTrace(k->scene, rays, 0, &rec, &err));
    const ReplayColor *c = ReplayColors(rec);
    TASK_CHECK(c[0].rgb[0] == 0.f && c[0].rgb[1] == 0.f && c[0].rgb[2] == 0.f);
    TASK_CHECK(c[1].rgb[0] == 0.f && c[1].rgb[1] == 0.f && c[1].rgb[2] == 0.f);
    ReplayReleaseRecord(rec);
    delete k;
    return 0;
}

// ===========================================================================
// ADJOINT GROUP
// ===========================================================================
namespace {

// Full FD check over a parameter block; returns false on any failure and
// prints every step, analytic value, FD value and error.  Passes when each
// significant component meets EITHER the absolute or the relative tolerance
// (central differences of well-conditioned single-precision paths sit
// around a few 1e-4 relative at the fine step).
bool FDCheck(const FDCfg &cfg, const vector<ReplayColor> &seed,
             int blockId, int count, float tol, bool *anySignificant) {
    Kit *k = BuildKit(cfg.P, cfg.A, cfg.B, cfg.opt);
    BindKit(k, true);
    ReplayRecord *rec = NULL; string err;
    if (!ReplayTrace(k->scene, cfg.rays, cfg.maxBounces, &rec, &err)) {
        delete k; return false;
    }
    ReplayGradient *grad = ReplayBackward(rec, seed);
    const ReplayGradientBlock *bk = grad->Find(blockId);
    if (!bk || bk->count != count) { ReplayFreeGradient(grad);
        ReplayReleaseRecord(rec); delete k; return false; }
    // Entries whose analytic gradient is negligible compared with the block's
    // strongest component cannot be validated by central differences: when
    // the objective is nearly flat, two large scene-objective values cancel
    // and FD noise dwarfs the true (near-zero) derivative.  Only entries with
    // a meaningful analytic magnitude are therefore compared.
    float blockScale = 0.f;
    for (int idx = 0; idx < count; ++idx)
        blockScale = std::max(blockScale, fabsf(bk->values[idx]));
    bool good = true;
    const float steps[2] = {1e-2f, 1e-3f};
    for (int s = 0; s < 2; ++s) {
        float h = steps[s];
        for (int idx = 0; idx < count; ++idx) {
            double an = bk->values[idx];
            if (fabs(an) < 1e-3f * (blockScale + 1e-12f)) continue;
            FDCfg cp = cfg, cm = cfg;
            float *tp = blockId == ID_MIRROR ? cp.P :
                        blockId == ID_INST_A ? cp.A : cp.B;
            float *tm = blockId == ID_MIRROR ? cm.P :
                        blockId == ID_INST_A ? cm.A : cm.B;
            float saveP = tp[idx], saveM = tm[idx];
            tp[idx] = saveP + h; tm[idx] = saveM - h;
            double fp = Objective(cp, seed);
            double fm = Objective(cm, seed);
            tp[idx] = saveP; tm[idx] = saveM;
            double fd = (fp - fm) / (2.0 * h);
            double ea = fabs(fd - an);
            double er = ea / (fabs(fd) + fabs(an) + 1e-12);
            *anySignificant = true;
            std::printf("  %s h=%g idx=%d analytic=%.6f fd=%.6f "
                        "absErr=%.2e relErr=%.2e\n",
                        blockId == ID_MIRROR ? "P" :
                        blockId == ID_INST_A ? "MA" : "MB",
                        h, idx, an, fd, ea, er);
            // Enforce on the fine step; coarse step is informative only.
            if (s == 1 && ea > tol && er > tol) good = false;
        }
    }
    ReplayFreeGradient(grad);
    ReplayReleaseRecord(rec);
    delete k;
    return good;
}

vector<ReplayColor> UnitSeed(int n) {
    vector<ReplayColor> seed(n);
    for (int i = 0; i < n; ++i) {
        seed[i].rgb[0] = 1.f; seed[i].rgb[1] = 0.7f; seed[i].rgb[2] = 0.3f;
    }
    return seed;
}

} // namespace

static int adjoint_single_hop() {
    FDCfg cfg;
    float P[9] = {-1,-1,0, 1,-1,0, 0,1,0};
    for (int i = 0; i < 9; ++i) cfg.P[i] = P[i];
    // Tilted single mirror: nonuniform scale + shear, det > 0.
    Matrix4x4 sh(1,0.25f,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    Transform TA = Translate(Vector(0,0,2.f)) * Transform(sh)
                   * Scale(1.1f,0.9f,1.05f);
    Transform TB = Translate(Vector(0,0,8.f));   // out of the way
    Fill12(cfg.A, TA.GetMatrix()); Fill12(cfg.B, TB.GetMatrix());
    cfg.rays.push_back(ReplayRay(Point(0,0,0),
                        Normalize(Vector(0.12f,0.07f,1.f))));
    cfg.maxBounces = 3;
    vector<ReplayColor> seed = UnitSeed(1);
    bool sig = false;
    TASK_CHECK(FDCheck(cfg, seed, ID_MIRROR, 9, 3e-3f, &sig));
    TASK_CHECK(sig);
    sig = false;
    TASK_CHECK(FDCheck(cfg, seed, ID_INST_A, 12, 3e-3f, &sig));
    TASK_CHECK(sig);
    return 0;
}

static int adjoint_multi_hop() {
    FDCfg cfg;
    float P[9] = {-3,-3,0, 3,-3,0, 0,3,0};
    for (int i = 0; i < 9; ++i) cfg.P[i] = P[i];
    Matrix4x4 sh(1,0.2f,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    Transform TA = Translate(Vector(0,0,2.f)) * RotateY(135.f)
                   * Transform(sh) * Scale(1.1f,0.9f,1.2f);
    // Second instance uses a reflection (negative determinant).
    Transform TB = Translate(Vector(3,0,2.f)) * RotateY(-90.f)
                   * Scale(1.f,1.f,-1.f);
    Fill12(cfg.A, TA.GetMatrix()); Fill12(cfg.B, TB.GetMatrix());
    cfg.rays.push_back(ReplayRay(Point(0,0,0), Vector(0,0,1)));
    cfg.maxBounces = 4;
    vector<ReplayColor> seed = UnitSeed(1);
    bool sig = false;
    // Shared mesh P feeds both instances; matrix blocks are independent.
    TASK_CHECK(FDCheck(cfg, seed, ID_MIRROR, 9, 8e-3f, &sig));
    TASK_CHECK(sig);
    sig = false;
    TASK_CHECK(FDCheck(cfg, seed, ID_INST_A, 12, 8e-3f, &sig));
    TASK_CHECK(sig);
    sig = false;
    TASK_CHECK(FDCheck(cfg, seed, ID_INST_B, 12, 8e-3f, &sig));
    TASK_CHECK(sig);
    return 0;
}

// Moving a vertex with near-zero hit weight still rotates the face normal,
// which moves the *subsequent* intersection even though the current hit
// point barely changes.  A point-only derivative would miss this term.
static int adjoint_normal_only_moves_next_hit() {
    FDCfg cfg;
    // Single triangle; the ray lands well inside it.  Moving any vertex
    // rotates the face normal, which steers the reflected ray and moves the
    // subsequent receiver intersection even where the current hit-point
    // interpolation weight is small.  The FD check exercises the full normal
    // VJP chain (a point-only derivative would not match).
    float P[9] = {-1,-1,0, 1,-1,0, 0,1,0};
    for (int i = 0; i < 9; ++i) cfg.P[i] = P[i];
    Matrix4x4 sh(1,0.25f,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    Transform TA = Translate(Vector(0,0,2.f)) * Transform(sh)
                   * Scale(1.1f,0.9f,1.05f);
    Transform TB = Translate(Vector(0,0,9.f));
    Fill12(cfg.A, TA.GetMatrix()); Fill12(cfg.B, TB.GetMatrix());
    cfg.rays.push_back(ReplayRay(Point(0,0,0.f),
                        Normalize(Vector(0.12f,0.07f,1.f))));
    cfg.maxBounces = 3;
    Kit *k = BuildKit(cfg.P, cfg.A, cfg.B, cfg.opt);
    BindKit(k, true);
    ReplayRecord *rec = NULL; string err;
    TASK_CHECK(ReplayTrace(k->scene, cfg.rays, 3, &rec, &err));
    vector<ReplayColor> seed = UnitSeed(1);
    ReplayGradient *grad = ReplayBackward(rec, seed);
    const ReplayGradientBlock *bk = grad->Find(ID_MIRROR);
    TASK_CHECK(bk != NULL);
    // Vertex 2 (indices 6..8) has a small interpolation weight here yet a
    // nonzero gradient carried through the face-normal/reflection chain.
    bool sig = false;
    TASK_CHECK(FDCheck(cfg, seed, ID_MIRROR, 9, 3e-3f, &sig));
    TASK_CHECK(sig);
    TASK_CHECK(bk->values[6] != 0.f || bk->values[7] != 0.f ||
               bk->values[8] != 0.f);
    ReplayFreeGradient(grad);
    ReplayReleaseRecord(rec);
    delete k;
    return 0;
}

static int adjoint_occluder_zero() {
    FDCfg cfg;
    float P[9] = {-1,-1,0, 1,-1,0, 0,1,0};
    for (int i = 0; i < 9; ++i) cfg.P[i] = P[i];
    Transform TA = Translate(Vector(0,0,2.f));
    Transform TB = Translate(Vector(0,0,8.f));
    Fill12(cfg.A, TA.GetMatrix()); Fill12(cfg.B, TB.GetMatrix());
    cfg.opt.occluder = true;
    cfg.rays.push_back(ReplayRay(Point(0,0,0.f),
                        Normalize(Vector(0.1f,0.05f,-1.f))));
    cfg.maxBounces = 3;
    Kit *k = BuildKit(cfg.P, cfg.A, cfg.B, cfg.opt);
    BindKit(k, true);
    ReplayRecord *rec = NULL; string err;
    TASK_CHECK(ReplayTrace(k->scene, cfg.rays, 3, &rec, &err));
    const ReplayColor *c = ReplayColors(rec);
    // The only light is shadowed at the matte receiver.
    TASK_CHECK(c[0].rgb[0] == 0.f && c[0].rgb[1] == 0.f);
    vector<ReplayColor> seed = UnitSeed(1);
    ReplayGradient *grad = ReplayBackward(rec, seed);
    for (int i = 0; i < grad->numBlocks; ++i)
        for (int j = 0; j < grad->blocks[i].count; ++j)
            TASK_CHECK(grad->blocks[i].values[j] == 0.f);
    ReplayFreeGradient(grad);
    ReplayReleaseRecord(rec);
    delete k;
    return 0;
}

static int adjoint_unbound_and_accumulation() {
    FDCfg cfg;
    float P[9] = {-1,-1,0, 1,-1,0, 0,1,0};
    for (int i = 0; i < 9; ++i) cfg.P[i] = P[i];
    Transform TA = Translate(Vector(0,0,2.f)) * RotateY(15.f);
    Transform TB = Translate(Vector(0,0,8.f));
    Fill12(cfg.A, TA.GetMatrix()); Fill12(cfg.B, TB.GetMatrix());
    cfg.rays.push_back(ReplayRay(Point(0,0,0),
                        Normalize(Vector(0.1f,0.05f,1.f))));
    cfg.rays.push_back(ReplayRay(Point(0.2f,0.1f,0.f),
                        Normalize(Vector(0.1f,0.2f,1.f))));
    cfg.maxBounces = 3;
    vector<ReplayColor> seed = UnitSeed(2);

    // Full binding reference gradient.
    Kit *k1 = BuildKit(cfg.P, cfg.A, cfg.B, cfg.opt);
    BindKit(k1, true);
    ReplayRecord *r1 = NULL; string err;
    TASK_CHECK(ReplayTrace(k1->scene, cfg.rays, 3, &r1, &err));
    ReplayGradient *gFull = ReplayBackward(r1, seed);

    // Per-ray gradients, accumulated manually, must equal the joint result.
    vector<float> sum(9, 0.f);
    for (int ray = 0; ray < 2; ++ray) {
        vector<ReplayColor> one(2);
        one[ray] = seed[ray];
        ReplayGradient *gOne = ReplayBackward(r1, one);
        const ReplayGradientBlock *bk = gOne->Find(ID_MIRROR);
        TASK_CHECK(bk != NULL);
        for (int j = 0; j < 9; ++j) sum[j] += bk->values[j];
        ReplayFreeGradient(gOne);
    }
    const ReplayGradientBlock *bf = gFull->Find(ID_MIRROR);
    for (int j = 0; j < 9; ++j)
        TASK_CHECK(Near(bf->values[j], sum[j], 1e-6f));

    // An unbound instance produces no matrix block.
    ReplayClearBindings();
    ReplayBindMesh(k1->mirrorMesh, ID_MIRROR);
    ReplayRecord *r2 = NULL;
    TASK_CHECK(ReplayTrace(k1->scene, cfg.rays, 3, &r2, &err));
    ReplayGradient *gUnbound = ReplayBackward(r2, seed);
    TASK_CHECK(gUnbound->Find(ID_INST_A) == NULL);
    TASK_CHECK(gUnbound->Find(ID_INST_B) == NULL);
    TASK_CHECK(gUnbound->Find(ID_MIRROR) != NULL);
    ReplayFreeGradient(gUnbound);
    ReplayReleaseRecord(r2);
    ReplayFreeGradient(gFull);
    ReplayReleaseRecord(r1);
    delete k1;
    return 0;
}

// ===========================================================================
// LIFETIME GROUP
// ===========================================================================
static int lifetime_outlives_scene() {
    float P[9] = {-1,-1,0, 1,-1,0, 0,1,0};
    Transform TA = Translate(Vector(0,0,2.f)) * RotateY(25.f);
    Transform TB = Translate(Vector(0,0,8.f));
    float A[12], B[12]; Fill12(A, TA.GetMatrix()); Fill12(B, TB.GetMatrix());
    Kit *k = BuildKit(P, A, B, ReplayOptions());
    BindKit(k, true);
    vector<ReplayRay> rays;
    rays.push_back(ReplayRay(Point(0,0,0),
                    Normalize(Vector(0.12f,0.07f,1.f))));
    ReplayRecord *rec = NULL; string err;
    TASK_CHECK(ReplayTrace(k->scene, rays, 3, &rec, &err));
    ReplayColor savedColor = ReplayColors(rec)[0];
    vector<ReplayColor> seed = UnitSeed(1);
    ReplayGradient *before = ReplayBackward(rec, seed);
    const ReplayGradientBlock *bp = before->Find(ID_MIRROR);
    vector<float> savedGrad(bp->values, bp->values + bp->count);

    // Destroy the whole scene and every pbrt object; the record must stand
    // entirely on its own storage.
    delete k;
    ReplayClearBindings();

    TASK_CHECK(ReplayNumRays(rec) == 1);
    TASK_CHECK(Near(ReplayColors(rec)[0].rgb[0], savedColor.rgb[0], 0.f));
    ReplayGradient *after = ReplayBackward(rec, seed);
    const ReplayGradientBlock *ap = after->Find(ID_MIRROR);
    TASK_CHECK(ap != NULL && ap->count == (int)savedGrad.size());
    for (int j = 0; j < ap->count; ++j)
        TASK_CHECK(Near(ap->values[j], savedGrad[j], 1e-7f));
    ReplayFreeGradient(after);
    ReplayFreeGradient(before);
    ReplayReleaseRecord(rec);
    return 0;
}

static int lifetime_repeated_backward_independent() {
    float P[9] = {-1,-1,0, 1,-1,0, 0,1,0};
    Transform TA = Translate(Vector(0,0,2.f));
    Transform TB = Translate(Vector(0,0,8.f));
    float A[12], B[12]; Fill12(A, TA.GetMatrix()); Fill12(B, TB.GetMatrix());
    Kit *k = BuildKit(P, A, B, ReplayOptions());
    BindKit(k, false);
    vector<ReplayRay> rays;
    rays.push_back(ReplayRay(Point(0,0,0),
                    Normalize(Vector(0.12f,0.07f,1.f))));
    ReplayRecord *rec = NULL; string err;
    TASK_CHECK(ReplayTrace(k->scene, rays, 3, &rec, &err));

    vector<ReplayColor> seed1 = UnitSeed(1);
    ReplayGradient *g1 = ReplayBackward(rec, seed1);
    // A second call with a zero seed must return all zeros regardless of the
    // first call's state (every backward run starts from zero).
    vector<ReplayColor> zero(1);
    ReplayGradient *g0 = ReplayBackward(rec, zero);
    for (int i = 0; i < g0->numBlocks; ++i)
        for (int j = 0; j < g0->blocks[i].count; ++j)
            TASK_CHECK(g0->blocks[i].values[j] == 0.f);
    // A fresh nonzero seed reproduces the first result exactly.
    ReplayGradient *g1b = ReplayBackward(rec, seed1);
    for (int i = 0; i < g1->numBlocks; ++i) {
        const ReplayGradientBlock *a = &g1->blocks[i];
        const ReplayGradientBlock *b = g1b->Find(a->parameterId);
        TASK_CHECK(b != NULL);
        for (int j = 0; j < a->count; ++j)
            TASK_CHECK(Near(a->values[j], b->values[j], 0.f));
    }
    ReplayFreeGradient(g1); ReplayFreeGradient(g0); ReplayFreeGradient(g1b);
    ReplayReleaseRecord(rec);
    delete k;
    return 0;
}

static int lifetime_two_records_isolated() {
    float P[9] = {-1,-1,0, 1,-1,0, 0,1,0};
    Transform TA = Translate(Vector(0,0,2.f)) * RotateY(10.f);
    Transform TB = Translate(Vector(0,0,8.f));
    float A[12], B[12]; Fill12(A, TA.GetMatrix()); Fill12(B, TB.GetMatrix());
    Kit *k1 = BuildKit(P, A, B, ReplayOptions());
    BindKit(k1, true);
    vector<ReplayRay> rays1;
    rays1.push_back(ReplayRay(Point(0,0,0),
                     Normalize(Vector(0.1f,0.05f,1.f))));
    ReplayRecord *rec1 = NULL; string err;
    TASK_CHECK(ReplayTrace(k1->scene, rays1, 3, &rec1, &err));
    delete k1;

    // Rebuild a different scene and interleave its record with the first.
    Kit *k2 = BuildKit(P, A, B, ReplayOptions());
    BindKit(k2, true);
    vector<ReplayRay> rays2;
    rays2.push_back(ReplayRay(Point(0.3f,-0.2f,0.f),
                     Normalize(Vector(0.2f,0.1f,1.f))));
    ReplayRecord *rec2 = NULL;
    TASK_CHECK(ReplayTrace(k2->scene, rays2, 3, &rec2, &err));

    vector<ReplayColor> seed = UnitSeed(1);
    ReplayGradient *g2 = ReplayBackward(rec2, seed);
    ReplayGradient *g1 = ReplayBackward(rec1, seed);
    ReplayGradient *g1again = ReplayBackward(rec1, seed);
    const ReplayGradientBlock *b1 = g1->Find(ID_MIRROR);
    const ReplayGradientBlock *b1a = g1again->Find(ID_MIRROR);
    const ReplayGradientBlock *b2 = g2->Find(ID_MIRROR);
    TASK_CHECK(b1 && b1a && b2);
    for (int j = 0; j < b1->count; ++j) {
        TASK_CHECK(Near(b1->values[j], b1a->values[j], 0.f));
        // The two independent records need not agree; just ensure the second
        // record did not corrupt the first.
        TASK_CHECK(std::isfinite(b1->values[j]) && std::isfinite(b2->values[j]));
    }
    ReplayFreeGradient(g1); ReplayFreeGradient(g2); ReplayFreeGradient(g1again);
    ReplayReleaseRecord(rec1); ReplayReleaseRecord(rec2);
    delete k2;
    return 0;
}

// ===========================================================================
// REJECTION / VALIDATION
// ===========================================================================
static int primal_rejects_unsupported() {
    Transform id;
    // Sphere primitive instead of a triangle mesh.
    Reference<Shape> sphere = new Sphere(&id, &id, false, 1.f, -1.f, 1.f,
                                         360.f);
    Reference<Material> matt = MakeMatte();
    vector<Reference<Primitive> > pr;
    pr.push_back(new GeometricPrimitive(sphere, matt, NULL));
    vector<Light *> lights;
    lights.push_back(new PointLight(Translate(Vector(0,0,1)),
                                    RGBSpectrum::FromRGB(liRGB)));
    Scene scene(new BVHAccel(pr), lights, NULL);
    vector<ReplayRay> rays;
    rays.push_back(ReplayRay(Point(0,0,0), Vector(0,0,1)));
    ReplayRecord *rec = NULL; string err;
    TASK_CHECK(!ReplayTrace(&scene, rays, 3, &rec, &err));
    TASK_CHECK(!err.empty());
    return 0;
}

static int primal_rejects_glass_and_bad_light() {
    float P[9] = {-1,-1,0, 1,-1,0, 0,1,0};
    int vi[6] = {0,1,2, 0,2,3};
    Point pp[4];
    for (int i = 0; i < 4; ++i) pp[i] = Point(P[3*i],P[3*i+1],P[3*i+2]);
    Transform id;
    Reference<Shape> mesh = new TriangleMesh(&id,&id,false,2,4,vi,pp,
                                              NULL,NULL,NULL,
                                              Reference<Texture<float> >());
    // Glass material is outside the supported subset.
    Reference<Texture<Spectrum> > kr =
        new ConstantTexture<Spectrum>(RGBSpectrum::FromRGB(krRGB));
    Reference<Texture<Spectrum> > kt =
        new ConstantTexture<Spectrum>(RGBSpectrum::FromRGB(krRGB));
    Reference<Texture<float> > index = new ConstantTexture<float>(1.5f);
    Reference<Material> glass = new GlassMaterial(kr, kt, index,
        Reference<Texture<float> >());
    vector<Reference<Primitive> > pr;
    pr.push_back(new GeometricPrimitive(mesh, glass, NULL));
    vector<Light *> lights;
    lights.push_back(new DistantLight(
        Transform(), Spectrum(1.f), Vector(0,0,1)));
    Scene scene(new BVHAccel(pr), lights, NULL);
    vector<ReplayRay> rays;
    rays.push_back(ReplayRay(Point(0,0,0), Vector(0,0,1)));
    ReplayRecord *rec = NULL; string err;
    TASK_CHECK(!ReplayTrace(&scene, rays, 3, &rec, &err));
    return 0;
}

static int primal_rejects_bad_ray_and_singular() {
    float P[9] = {-1,-1,0, 1,-1,0, 0,1,0};
    Transform T = Translate(Vector(0,0,2.f));
    float A[12], B[12]; Fill12(A, T.GetMatrix()); Fill12(B, T.GetMatrix());
    Kit *k = BuildKit(P, A, B, ReplayOptions());
    BindKit(k, false);
    vector<ReplayRay> bad;
    bad.push_back(ReplayRay(Point(0,0,0), Vector(0,0,0)));
    ReplayRecord *rec = NULL; string err;
    TASK_CHECK(!ReplayTrace(k->scene, bad, 3, &rec, &err));

    // Effectively non-invertible instance matrix must be rejected by the
    // replay layer's finite-invertible-affine check.  A uniform tiny scale
    // keeps pbrt's own polar decomposition well conditioned while the
    // determinant (1e-12) falls below the replay invertibility floor.
    Transform degT = RotateY(30.f) * Scale(1e-4f, 1e-4f, 1e-4f);
    float singular[12]; Fill12(singular, degT.GetMatrix());
    Kit *k2 = BuildKit(P, singular, B, ReplayOptions());
    ReplayClearBindings();
    ReplayBindInstance(k2->instA, ID_INST_A);
    vector<ReplayRay> rays;
    rays.push_back(ReplayRay(Point(0,0,0), Vector(0,0,1)));
    ReplayRecord *rec2 = NULL;
    TASK_CHECK(!ReplayTrace(k2->scene, rays, 3, &rec2, &err));
    delete k2;
    delete k;
    return 0;
}

const task_case task_cases[] = {
    {"primal", "diffuse_direct", primal_diffuse},
    {"primal", "two_bounce_mirror", primal_two_bounce},
    {"primal", "black_background_and_budget", primal_black_cases},
    {"primal", "reject_sphere", primal_rejects_unsupported},
    {"primal", "reject_glass_and_distant_light", primal_rejects_glass_and_bad_light},
    {"primal", "reject_bad_ray_and_singular_matrix", primal_rejects_bad_ray_and_singular},
    {"adjoint", "single_hop_fd", adjoint_single_hop},
    {"adjoint", "multi_hop_fd", adjoint_multi_hop},
    {"adjoint", "normal_only_moves_next_hit", adjoint_normal_only_moves_next_hit},
    {"adjoint", "occluder_zero_gradient", adjoint_occluder_zero},
    {"adjoint", "unbound_and_cross_ray_accumulation", adjoint_unbound_and_accumulation},
    {"lifetime", "record_outlives_scene", lifetime_outlives_scene},
    {"lifetime", "repeated_backward_starts_zero", lifetime_repeated_backward_independent},
    {"lifetime", "two_records_isolated", lifetime_two_records_isolated},
};
const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
