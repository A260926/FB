/*
    examples/material_fit.cpp

    Minimal material-fitting driver for the frozen-proposal record.  A small
    two-mesh plastic scene is rendered once into a MaterialReplay record; a
    ground-truth target image is produced from known target parameters, and
    the seven parameters of each shared material id are then fitted with
    plain gradient descent whose gradients come from the production reverse
    mode (no parameter perturbation and no re-tracing).

    Usage: material-example <output-directory>
    Outputs in that directory:
      fit.ppm        - final fitted image (ASCII PPM, one pixel per ray)
      loss.txt       - initial/final loss and the per-step loss curve
      parameters.txt - initial and final Kd/Ks/roughness for every id
*/

#include "integrators/material_replay.h"

#include "scene.h"
#include "shapes/trianglemesh.h"
#include "accelerators/bvh.h"
#include "materials/plastic.h"
#include "textures/constant.h"
#include "lights/point.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

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


Reference<Material> MakePlastic(const MaterialReplayParams &p) {
    return new PlasticMaterial(
        new ConstantTexture<Spectrum>(Spectrum::FromRGB(p.kd)),
        new ConstantTexture<Spectrum>(Spectrum::FromRGB(p.ks)),
        new ConstantTexture<float>(p.roughness),
        Reference<Texture<float> >());
}


float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }


void ProjectToDomain(MaterialReplayParams *p) {
    for (int c = 0; c < 3; ++c) {
        p->kd[c] = Clamp01(p->kd[c]);
        p->ks[c] = Clamp01(p->ks[c]);
    }
    if (p->roughness < 0.01f) p->roughness = 0.01f;
    if (p->roughness > 1.f) p->roughness = 1.f;
}


void ToRGB(const Spectrum &s, float rgb[3]) { s.ToRGB(rgb); }


double MeanSquaredError(const std::vector<Spectrum> &image,
                        const std::vector<Spectrum> &target) {
    double sum = 0.0;
    for (size_t r = 0; r < image.size(); ++r) {
        float a[3], b[3];
        image[r].ToRGB(a);
        target[r].ToRGB(b);
        for (int c = 0; c < 3; ++c) {
            double d = double(a[c]) - double(b[c]);
            sum += 0.5 * d * d;
        }
    }
    return sum / double(image.size() * 3);
}

} // anonymous namespace


int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output-directory>\n",
                     argc > 0 ? argv[0] : "material-example");
        return 2;
    }
    const std::string outDir = argv[1];

    // Ground truth parameters (what the fit has to recover) and the frozen
    // proposal / initial guess parameters.
    MaterialReplayParams truthFloor;
    truthFloor.kd[0] = 0.50f; truthFloor.kd[1] = 0.30f; truthFloor.kd[2] = 0.20f;
    truthFloor.ks[0] = 0.15f; truthFloor.ks[1] = 0.15f; truthFloor.ks[2] = 0.15f;
    truthFloor.roughness = 0.20f;
    MaterialReplayParams truthWall;
    truthWall.kd[0] = 0.20f; truthWall.kd[1] = 0.40f; truthWall.kd[2] = 0.30f;
    truthWall.ks[0] = 0.20f; truthWall.ks[1] = 0.20f; truthWall.ks[2] = 0.25f;
    truthWall.roughness = 0.35f;

    MaterialReplayParams startFloor;
    startFloor.kd[0] = startFloor.kd[1] = startFloor.kd[2] = 0.25f;
    startFloor.ks[0] = startFloor.ks[1] = startFloor.ks[2] = 0.30f;
    startFloor.roughness = 0.50f;
    MaterialReplayParams startWall;
    startWall.kd[0] = startWall.kd[1] = startWall.kd[2] = 0.25f;
    startWall.ks[0] = startWall.ks[1] = startWall.ks[2] = 0.30f;
    startWall.roughness = 0.50f;

    // Small static scene: floor, wall and one point light.
    std::vector<Reference<Material> > materials;
    materials.push_back(MakePlastic(startFloor));
    materials.push_back(MakePlastic(startWall));
    std::vector<Reference<Primitive> > rects;
    rects.push_back(MakeRect(
        Point(-3, 0, 3), Point(3, 0, 3), Point(3, 0, -1.5f),
        Point(-3, 0, -1.5f), materials[0]));
    rects.push_back(MakeRect(
        Point(-3, 0, -1.5f), Point(3, 0, -1.5f),
        Point(3, 3, -1.5f), Point(-3, 3, -1.5f), materials[1]));
    std::vector<Reference<Primitive> > leaves;
    std::vector<int> leafId;
    for (size_t i = 0; i < rects.size(); ++i) {
        std::vector<Reference<Primitive> > r;
        rects[i]->FullyRefine(r);
        for (size_t j = 0; j < r.size(); ++j) {
            leaves.push_back(r[j]);
            leafId.push_back(int(i));
        }
    }
    std::vector<Light *> lights;
    lights.push_back(new PointLight(Translate(Vector(0.8f, 1.6f, 0.6f)),
                                    Spectrum(3.f)));
    Scene scene(new BVHAccel(leaves), lights, NULL);

    // Fixed ray batch on a regular image grid with per-ray fixed seeds.
    const int W = 24, H = 24;
    std::vector<MaterialReplayRay> rays;
    rays.reserve(W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float u = (x + 0.5f) / W;
            float v = (y + 0.5f) / H;
            Point o(-2.2f + 4.4f * u, 1.5f, 1.9f - 0.4f * v);
            Vector d(0.10f * (u - 0.5f), -0.70f, -0.72f - 0.05f * v);
            rays.push_back(MaterialReplayRay(o, Normalize(d),
                                             1234u + uint32_t(y * W + x)));
        }

    MaterialReplay replay;
    std::string err;
    if (!replay.BindProposal(0, startFloor, &err) ||
        !replay.BindProposal(1, startWall, &err)) {
        std::fprintf(stderr, "bind failed: %s\n", err.c_str());
        return 1;
    }
    for (size_t i = 0; i < leaves.size(); ++i) {
        int id = leafId[i];
        if (!replay.BindPrimitive(id, leaves[i]->primitiveId, &err)) {
            std::fprintf(stderr, "primitive bind failed: %s\n", err.c_str());
            return 1;
        }
    }
    const int kMaxVertices = 4;
    if (!replay.Record(&scene, rays, kMaxVertices, &err)) {
        std::fprintf(stderr, "record failed: %s\n", err.c_str());
        return 1;
    }

    std::vector<MaterialReplayParams> truth(2);
    truth[0] = truthFloor;
    truth[1] = truthWall;
    std::vector<Spectrum> targetImage;
    if (!replay.Evaluate(truth, &targetImage, &err)) {
        std::fprintf(stderr, "target evaluate failed: %s\n", err.c_str());
        return 1;
    }

    // Gradient descent on the frozen paths.
    std::vector<MaterialReplayParams> params(2);
    params[0] = startFloor;
    params[1] = startWall;
    const int kSteps = 60;
    const float lrColor = 0.6f;
    const float lrRough = 0.05f;
    const double invN = 1.0 / double(rays.size());

    std::vector<double> lossCurve;
    std::vector<Spectrum> image;
    if (!replay.Evaluate(params, &image, &err)) return 1;
    double initialLoss = MeanSquaredError(image, targetImage);
    lossCurve.push_back(initialLoss);

    for (int step = 0; step < kSteps; ++step) {
        if (!replay.Evaluate(params, &image, &err)) return 1;

        // Adjoint of 0.5/N * sum (L - T)^2 with respect to each radiance.
        std::vector<Spectrum> adjoints(rays.size());
        for (size_t r = 0; r < rays.size(); ++r) {
            float l[3], t[3];
            image[r].ToRGB(l);
            targetImage[r].ToRGB(t);
            float g[3];
            for (int c = 0; c < 3; ++c)
                g[c] = float((double(l[c]) - double(t[c])) * invN);
            adjoints[r] = Spectrum::FromRGB(g);
        }

        std::vector<MaterialReplayGradient> grad;
        if (!replay.Backward(params, adjoints, &grad, &err)) return 1;
        for (size_t i = 0; i < params.size(); ++i) {
            for (int c = 0; c < 3; ++c) {
                params[i].kd[c] -= lrColor * grad[i].dKd[c];
                params[i].ks[c] -= lrColor * grad[i].dKs[c];
            }
            params[i].roughness -= lrRough * grad[i].dRoughness;
            ProjectToDomain(&params[i]);
        }

        if (!replay.Evaluate(params, &image, &err)) return 1;
        lossCurve.push_back(MeanSquaredError(image, targetImage));
    }

    double finalLoss = lossCurve.back();

    // fit.ppm: ASCII P6-style P3 image of the final fitted radiance.
    const std::string ppmPath = outDir + "/fit.ppm";
    FILE *ppm = std::fopen(ppmPath.c_str(), "w");
    if (!ppm) {
        std::fprintf(stderr, "cannot write %s\n", ppmPath.c_str());
        return 1;
    }
    std::fprintf(ppm, "P3\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float rgb[3];
            image[y * W + x].ToRGB(rgb);
            int px[3];
            for (int c = 0; c < 3; ++c) {
                float v = rgb[c];
                // Simple filmic-ish compression keeps highlights in range.
                v = v / (1.f + v);
                px[c] = int(Clamp01(v) * 255.f + 0.5f);
            }
            std::fprintf(ppm, "%d %d %d  ", px[0], px[1], px[2]);
        }
        std::fprintf(ppm, "\n");
    }
    std::fclose(ppm);

    // loss.txt: initial/final loss and the full per-step curve.
    const std::string lossPath = outDir + "/loss.txt";
    FILE *lf = std::fopen(lossPath.c_str(), "w");
    if (!lf) {
        std::fprintf(stderr, "cannot write %s\n", lossPath.c_str());
        return 1;
    }
    std::fprintf(lf, "initial_loss %.10g\n", initialLoss);
    std::fprintf(lf, "final_loss %.10g\n", finalLoss);
    std::fprintf(lf, "relative_reduction %.10g\n",
                 initialLoss > 0.0 ? (initialLoss - finalLoss) / initialLoss
                                   : 0.0);
    std::fprintf(lf, "steps %d\n", kSteps);
    for (size_t i = 0; i < lossCurve.size(); ++i)
        std::fprintf(lf, "step %zu loss %.10g\n", i, lossCurve[i]);
    std::fclose(lf);

    // parameters.txt: initial and fitted parameters per shared id.
    const std::string paramPath = outDir + "/parameters.txt";
    FILE *pf = std::fopen(paramPath.c_str(), "w");
    if (!pf) {
        std::fprintf(stderr, "cannot write %s\n", paramPath.c_str());
        return 1;
    }
    const MaterialReplayParams initial[2] = {startFloor, startWall};
    for (int i = 0; i < replay.NumIds(); ++i) {
        std::fprintf(pf, "id %d\n", replay.IdAt(i));
        std::fprintf(pf,
            "  initial  Kd %.6f %.6f %.6f Ks %.6f %.6f %.6f roughness %.6f\n",
            initial[i].kd[0], initial[i].kd[1], initial[i].kd[2],
            initial[i].ks[0], initial[i].ks[1], initial[i].ks[2],
            initial[i].roughness);
        std::fprintf(pf,
            "  fitted   Kd %.6f %.6f %.6f Ks %.6f %.6f %.6f roughness %.6f\n",
            params[i].kd[0], params[i].kd[1], params[i].kd[2],
            params[i].ks[0], params[i].ks[1], params[i].ks[2],
            params[i].roughness);
        std::fprintf(pf,
            "  truth    Kd %.6f %.6f %.6f Ks %.6f %.6f %.6f roughness %.6f\n",
            truth[i].kd[0], truth[i].kd[1], truth[i].kd[2],
            truth[i].ks[0], truth[i].ks[1], truth[i].ks[2],
            truth[i].roughness);
    }
    std::fclose(pf);

    std::printf("material fit: initial_loss=%.6g final_loss=%.6g "
                "reduction=%.2f%%\n",
                initialLoss, finalLoss,
                100.0 * (initialLoss > 0.0
                             ? (initialLoss - finalLoss) / initialLoss
                             : 0.0));
    return 0;
}
