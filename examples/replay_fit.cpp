// Windowless mirror placement example.
//
// A single instanced mirror bounces a fixed grid of rays onto a matte
// receiver lit by one point light.  The goal is to recover the mirror
// instance's local-to-parent translation so the rendered linear RGB matches
// a target image.  Each iteration replays the paths through the existing
// pbrt intersection machinery, differentiates the objective with the
// hand-written replay adjoint, takes a small gradient step with backtracking
// (the steps are deliberately tiny so the traced path branch never changes),
// and rebuilds the scene from the updated 3x4 matrix.
//
// Outputs (all written into the directory given as argv[1]):
//   fit.ppm       final 48x48 linear RGB image, no tonemapping/clipping beyond
//                 byte quantization
//   loss.txt      initial loss and the loss after every descent step
//   parameters.txt initial and final row-major 3x4 instance matrices

#include "scene.h"
#include "shapes/trianglemesh.h"
#include "accelerators/bvh.h"
#include "materials/mirror.h"
#include "materials/matte.h"
#include "textures/constant.h"
#include "lights/point.h"
#include "integrators/replay.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>

using std::vector;
using std::string;

namespace {

const int PARAM_INSTANCE = 7;
const int IMG = 48;

float KrRGB[3] = {0.75f, 0.8f, 0.85f};
float KdRGB[3] = {0.45f, 0.5f, 0.35f};
float LiRGB[3] = {7.f, 6.f, 5.f};

Reference<Material> MakeMirror() {
    return new MirrorMaterial(
        new ConstantTexture<Spectrum>(RGBSpectrum::FromRGB(KrRGB)),
        Reference<Texture<float> >());
}
Reference<Material> MakeMatte() {
    return new MatteMaterial(
        new ConstantTexture<Spectrum>(RGBSpectrum::FromRGB(KdRGB)),
        new ConstantTexture<float>(0.f),
        Reference<Texture<float> >());
}

void Fill12(float out[12], const Matrix4x4 &M) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) out[4*i+j] = M.m[i][j];
}

struct SceneKit {
    Transform id, *l2w, *w2l;
    TriangleMesh *mirrorMesh;
    TransformedPrimitive *instance;
    Scene *scene;
    vector<Light *> lights;
};

// Fixed mirror source triangle and receiver; only the instance matrix moves.
SceneKit *BuildScene(const float matrix[12]) {
    SceneKit *k = new SceneKit();
    int tri[3] = {0,1,2};
    // Large mirror so every grid ray strikes it; the reflected rays then
    // land across the matte receiver and produce a strong placement signal.
    Point mp[3] = { Point(-3.f,-3.f,0.f), Point(3.f,-3.f,0.f),
                    Point(0.f, 3.4f,0.f) };
    k->mirrorMesh = new TriangleMesh(&k->id,&k->id,false,1,3,tri,mp,
                                     NULL,NULL,NULL,
                                     Reference<Texture<float> >());
    GeometricPrimitive *mgp =
        new GeometricPrimitive(k->mirrorMesh, MakeMirror(), NULL);

    int quad[6] = {0,1,2, 0,2,3};
    Point rp[4] = { Point(-2.5f,-2.5f,-2.f), Point(2.5f,-2.5f,-2.f),
                    Point(2.5f, 2.5f,-2.f), Point(-2.5f,2.5f,-2.f) };
    TriangleMesh *receiver =
        new TriangleMesh(&k->id,&k->id,false,2,4,quad,rp,NULL,NULL,NULL,
                         Reference<Texture<float> >());
    GeometricPrimitive *rgp =
        new GeometricPrimitive(receiver, MakeMatte(), NULL);

    float m4[4][4];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) m4[i][j] = matrix[4*i+j];
    m4[3][0] = m4[3][1] = m4[3][2] = 0.f; m4[3][3] = 1.f;
    Matrix4x4 lm(m4);
    k->l2w = new Transform(lm, Inverse(lm));
    k->w2l = new Transform(Inverse(*k->l2w));
    AnimatedTransform at(k->w2l, 0, k->w2l, 1);
    vector<Reference<Primitive> > refined;
    mgp->FullyRefine(refined);
    Reference<Primitive> proto = new BVHAccel(refined);
    k->instance = new TransformedPrimitive(proto, at);

    vector<Reference<Primitive> > prims;
    prims.push_back(k->instance);
    prims.push_back(rgp);
    k->lights.push_back(new PointLight(
        Translate(Vector(0.9f, 0.7f, -1.0f)),
        RGBSpectrum::FromRGB(LiRGB)));
    k->scene = new Scene(new BVHAccel(prims), k->lights, NULL);
    return k;
}

void DestroyScene(SceneKit *k) {
    delete k->scene;
    delete k->w2l;
    delete k->l2w;
    delete k;
}

// A regular grid of upward rays; those that miss the mirror return black.
vector<ReplayRay> MakeRays() {
    vector<ReplayRay> rays;
    rays.reserve(IMG * IMG);
    for (int y = 0; y < IMG; ++y) {
        for (int x = 0; x < IMG; ++x) {
            float fx = (x + 0.5f) / IMG - 0.5f;
            float fy = (y + 0.5f) / IMG - 0.5f;
            Point o(fx * 0.8f, fy * 0.8f, 0.f);
            Vector d(fx * 0.15f, fy * 0.15f, 1.f);
            rays.push_back(ReplayRay(o, Normalize(d)));
        }
    }
    return rays;
}

double ComputeLoss(const ReplayColor *colors, int n,
                   const vector<ReplayColor> &target) {
    double loss = 0.0;
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) {
            double e = colors[i].rgb[c] - target[i].rgb[c];
            loss += 0.5 * e * e;
        }
    return loss;
}

void WritePPM(const string &path, const ReplayColor *colors) {
    FILE *f = fopen(path.c_str(), "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", IMG, IMG);
    unsigned char row[IMG * IMG * 3];
    for (int i = 0; i < IMG * IMG; ++i) {
        for (int c = 0; c < 3; ++c) {
            float v = colors[i].rgb[c];
            if (v < 0.f) v = 0.f;
            if (v > 1.f) v = 1.f;
            row[3*i+c] = (unsigned char)std::lrintf(v * 255.f);
        }
    }
    fwrite(row, 1, sizeof(row), f);
    fclose(f);
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
        return 1;
    }
    string dir = argv[1];
    vector<ReplayRay> rays = MakeRays();

    // Ground-truth placement: a tilted mirror translated to z = 2.
    Transform good = Translate(Vector(0.f, 0.f, 2.f)) * RotateY(12.f);
    float goodM[12]; Fill12(goodM, good.GetMatrix());

    // Render the target image from the ground-truth parameters.
    SceneKit *targetKit = BuildScene(goodM);
    ReplayClearBindings();
    ReplayBindInstance(targetKit->instance, PARAM_INSTANCE);
    ReplayRecord *targetRec = NULL; string err;
    if (!ReplayTrace(targetKit->scene, rays, 3, &targetRec, &err)) {
        std::fprintf(stderr, "target trace failed: %s\n", err.c_str());
        return 1;
    }
    vector<ReplayColor> target(ReplayColors(targetRec),
                               ReplayColors(targetRec) + rays.size());
    ReplayReleaseRecord(targetRec);
    DestroyScene(targetKit);

    // Start the fit with the mirror translated off the optimum.
    Transform start = Translate(Vector(0.35f, -0.25f, 2.f)) * RotateY(12.f);
    float params[12]; Fill12(params, start.GetMatrix());
    float initial[12]; std::memcpy(initial, params, sizeof(params));

    FILE *lossFile = fopen((dir + "/loss.txt").c_str(), "w");
    std::fprintf(lossFile, "iteration loss\n");

    const int kSteps = 8;
    double baseStep = 0.02;
    for (int iter = 0; iter <= kSteps; ++iter) {
        SceneKit *kit = BuildScene(params);
        ReplayClearBindings();
        ReplayBindInstance(kit->instance, PARAM_INSTANCE);
        ReplayRecord *rec = NULL;
        if (!ReplayTrace(kit->scene, rays, 3, &rec, &err)) {
            std::fprintf(stderr, "trace failed at iter %d: %s\n",
                         iter, err.c_str());
            return 1;
        }
        const ReplayColor *colors = ReplayColors(rec);
        double loss = ComputeLoss(colors, (int)rays.size(), target);
        std::fprintf(lossFile, "%d %.8f\n", iter, loss);

        if (iter == kSteps) {
            WritePPM(dir + "/fit.ppm", colors);
            ReplayReleaseRecord(rec);
            DestroyScene(kit);
            break;
        }

        // d(loss)/d(RGB) = RGB - target.
        vector<ReplayColor> seed(rays.size());
        for (uint32_t i = 0; i < rays.size(); ++i)
            for (int c = 0; c < 3; ++c)
                seed[i].rgb[c] = colors[i].rgb[c] - target[i].rgb[c];
        ReplayGradient *grad = ReplayBackward(rec, seed);
        const ReplayGradientBlock *block = grad->Find(PARAM_INSTANCE);
        if (!block) {
            std::fprintf(stderr, "missing instance gradient block\n");
            return 1;
        }

        // Backtracking gradient descent on the three translation entries
        // (matrix columns 3, 7, 11).  Tiny steps keep the path branch fixed.
        float trial[12];
        double step = baseStep;
        bool accepted = false;
        for (int back = 0; back < 10; ++back) {
            std::memcpy(trial, params, sizeof(params));
            const int tIdx[3] = {3, 7, 11};
            for (int q = 0; q < 3; ++q)
                trial[tIdx[q]] -= (float)step * block->values[tIdx[q]];
            SceneKit *trialKit = BuildScene(trial);
            ReplayClearBindings();
            ReplayBindInstance(trialKit->instance, PARAM_INSTANCE);
            ReplayRecord *trialRec = NULL;
            if (!ReplayTrace(trialKit->scene, rays, 3, &trialRec, &err)) {
                DestroyScene(trialKit); step *= 0.5; continue;
            }
            double trialLoss = ComputeLoss(ReplayColors(trialRec),
                                           (int)rays.size(), target);
            ReplayReleaseRecord(trialRec);
            DestroyScene(trialKit);
            // Accept any non-increasing step; near the optimum the changes
            // are below single-precision resolution.
            if (trialLoss <= loss + 1e-12) {
                std::memcpy(params, trial, sizeof(params));
                accepted = true;
                break;
            }
            step *= 0.5;
        }
        ReplayFreeGradient(grad);
        ReplayReleaseRecord(rec);
        DestroyScene(kit);
        if (!accepted) break;   // already at the local optimum
    }
    fclose(lossFile);

    FILE *parFile = fopen((dir + "/parameters.txt").c_str(), "w");
    std::fprintf(parFile, "# row-major 3x4 local-to-parent instance matrix\n");
    std::fprintf(parFile, "initial\n");
    for (int i = 0; i < 3; ++i)
        std::fprintf(parFile, "%.8f %.8f %.8f %.8f\n",
                     initial[4*i], initial[4*i+1], initial[4*i+2],
                     initial[4*i+3]);
    std::fprintf(parFile, "final\n");
    for (int i = 0; i < 3; ++i)
        std::fprintf(parFile, "%.8f %.8f %.8f %.8f\n",
                     params[4*i], params[4*i+1], params[4*i+2],
                     params[4*i+3]);
    fclose(parFile);
    return 0;
}
