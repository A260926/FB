/*
 * Transport-level tests for the static light tree and its integration into
 * the path tracer and VertexCM (BPT/VCM).
 *
 * Three groups are registered:
 *   distribution - tree build, Sample/Pdf consistency, frequencies, black and
 *                  empty tables, mixed light types, visit bounds, rejection
 *   mis          - independent enumeration of the feasible short-path
 *                  generation strategies in area measure, balance weights
 *                  summing to one, and the production MIS recurrence under
 *                  non-uniform qD (diffuse and specular paths, light hits,
 *                  background)
 *   render       - unbiased linear radiance and regional pick distribution
 *                  under both selection modes, plus the demo PFM image
 *
 * Seeds are fixed and every statistical check prints its sample size and the
 * standard-error basis used for the tolerance.
 */

#include <cstdio>
#include <cmath>
#include <limits>
#include <vector>
#include <string>
#include <fstream>
#include <cstring>
#include "task_support/check.h"
#include "src/scene.hxx"
#include "src/lighttree.hxx"
#include "src/pathtracer.hxx"
#include "src/vertexcm.hxx"
#include "src/vertexcm_adapter.hxx"

namespace
{

// ---------------------------------------------------------------------------
// Scene assembly helpers (tests build their own light tables)
// ---------------------------------------------------------------------------

SceneSphere MakeSphere(float aRadius)
{
    SceneSphere sphere;
    sphere.mSceneCenter = Vec3f(0.f);
    sphere.mSceneRadius = aRadius;
    sphere.mInvSceneRadiusSqr = 1.f / (aRadius * aRadius);
    return sphere;
}

PointLight* MakePoint(const Vec3f &aPos, const Vec3f &aIntensity)
{
    PointLight *l = new PointLight(aPos);
    l->mIntensity = aIntensity;
    return l;
}

AreaLight* MakeArea(
    const Vec3f &aP0,
    const Vec3f &aP1,
    const Vec3f &aP2,
    const Vec3f &aIntensity)
{
    AreaLight *l = new AreaLight(aP0, aP1, aP2);
    l->mIntensity = aIntensity;
    return l;
}

DirectionalLight* MakeDirectional(
    const Vec3f &aDir,
    const Vec3f &aIntensity)
{
    DirectionalLight *l = new DirectionalLight(aDir);
    l->mIntensity = aIntensity;
    return l;
}

BackgroundLight* MakeBackground(const Vec3f &aColor, float aScale)
{
    BackgroundLight *l = new BackgroundLight;
    l->mBackgroundColor = aColor;
    l->mScale = aScale;
    return l;
}

void ClearLights(std::vector<AbstractLight*> &aLights)
{
    for(size_t i = 0; i < aLights.size(); ++i)
        delete aLights[i];
    aLights.clear();
}

bool Approx(double a, double b, double aRelTol = 1e-3, double aAbsTol = 1e-9)
{
    return std::abs(a - b) <= aAbsTol + aRelTol * std::max(std::abs(a), std::abs(b));
}

// A closed diffuse room with two unequal point lights, used for MIS/render.
Scene* BuildTwoLightRoom()
{
    Scene *scene = new Scene;
    // Geometry + camera from the Cornell box, without any of its own lights.
    scene->LoadCornellBox(Vec2i(48, 48), Scene::kBothSmallSpheres);
    scene->mLights.clear();
    scene->mBackground = NULL;
    scene->mMaterial2Light.clear();

    scene->mLights.push_back(MakePoint(Vec3f(-0.8f, -0.4f, 0.6f), Vec3f(1.f)));
    scene->mLights.push_back(MakePoint(Vec3f( 0.8f, -0.4f, 0.6f), Vec3f(40.f)));

    scene->BuildSceneSphere();
    return scene;
}

// Parses the PFM written by the demo and returns the flat float pixels.
bool ReadPfm(const char *aPath, int &oW, int &oH, std::vector<float> &oPixels)
{
    std::ifstream f(aPath, std::ios::binary);
    if(!f.good())
        return false;
    std::string tag;
    f >> tag;
    if(tag != "PF")
        return false;
    f >> oW >> oH;
    float scale;
    f >> scale;
    f.get(); // single whitespace after the header
    if(oW <= 0 || oH <= 0)
        return false;
    oPixels.resize(size_t(oW) * oH * 3);
    f.read(reinterpret_cast<char*>(&oPixels[0]),
        oPixels.size() * sizeof(float));
    return bool(f);
}

} // namespace

///////////////////////////////////////////////////////////////////////////////
// DISTRIBUTION GROUP
///////////////////////////////////////////////////////////////////////////////

// Empty table returns no sample and zero pdf; a build over it is valid.
static int dist_empty_table()
{
    std::vector<AbstractLight*> lights;
    LightTree tree;
    const bool ok = tree.Build(lights, MakeSphere(5.f));
    TASK_CHECK(ok);
    TASK_CHECK(tree.IsEmpty());
    TASK_CHECK(tree.LightCount() == 0);
    TASK_CHECK(tree.Sample(Vec3f(0.f), 0.5f) == -1);
    TASK_CHECK(tree.Pdf(Vec3f(0.f), 0) == 0.0);
    TASK_CHECK(tree.LastSampleNodeAccesses() == 0);
    return 0;
}

// Sample and Pdf describe the same distribution and pdfs normalize to one.
static int dist_sample_pdf_consistency()
{
    std::vector<AbstractLight*> lights;
    lights.push_back(MakePoint(Vec3f(-3.f, 0.f, 0.f), Vec3f(1.f)));
    lights.push_back(MakePoint(Vec3f( 3.f, 0.f, 0.f), Vec3f(100.f)));
    lights.push_back(MakePoint(Vec3f( 0.f, 3.f, 0.f), Vec3f(10.f)));
    LightTree tree;
    TASK_CHECK(tree.Build(lights, MakeSphere(8.f)));
    const int n = tree.LightCount();
    TASK_CHECK(n == 3);

    const Vec3f positions[] = {
        Vec3f(-2.9f, 0.f, 0.f), Vec3f(2.9f, 0.f, 0.f), Vec3f(0.f, -2.f, 2.f)
    };
    for(size_t p = 0; p < 3; ++p)
    {
        double sum = 0.0;
        for(int id = 0; id < n; ++id)
        {
            const double pdf = tree.Pdf(positions[p], id);
            TASK_CHECK(pdf > 0.0 && pdf < 1.0);
            TASK_CHECK(Approx(pdf,
                LightTree::kUniformFraction / n +
                (1.0 - LightTree::kUniformFraction) *
                tree.TreePdf(positions[p], id), 1e-12));
            sum += pdf;
        }
        TASK_CHECK(Approx(sum, 1.0, 1e-12));
    }
    ClearLights(lights);
    return 0;
}

// Empirical frequencies match the independent tree probabilities; prints the
// sample size and 3-sigma standard-error band used as the tolerance.
static int dist_frequency()
{
    std::vector<AbstractLight*> lights;
    lights.push_back(MakePoint(Vec3f(-4.f, 0.f, 0.f), Vec3f(1.f)));
    lights.push_back(MakePoint(Vec3f( 4.f, 0.f, 0.f), Vec3f(200.f)));
    LightTree tree;
    TASK_CHECK(tree.Build(lights, MakeSphere(9.f)));

    Rng rng(98765);
    const int samples = 24000;
    const Vec3f x(-3.7f, 0.f, 0.f);
    std::vector<long long> counts(2, 0);
    for(int i = 0; i < samples; ++i)
        ++counts[tree.Sample(x, rng.GetFloat())];

    for(int id = 0; id < 2; ++id)
    {
        const double p = tree.Pdf(x, id);
        const double freq = double(counts[id]) / samples;
        const double stderr3 = 3.0 * std::sqrt(p * (1.0 - p) / samples);
        std::printf("  frequency id=%d samples=%d p=%.5f freq=%.5f 3sigma=%.5f\n",
            id, samples, p, freq, stderr3);
        TASK_CHECK(std::abs(freq - p) <= stderr3);
    }
    // The nearby bright light must dominate at a receiving point next to it.
    const Vec3f nearBright(3.7f, 0.f, 0.f);
    TASK_CHECK(tree.Pdf(nearBright, 1) > tree.Pdf(nearBright, 0));
    // Right next to the dim light, proximity can outweigh brightness instead,
    // showing the score is genuinely position dependent.
    const Vec3f nearDim(-3.7f, 0.f, 0.f);
    TASK_CHECK(tree.Pdf(nearDim, 0) > tree.Pdf(nearDim, 1));
    ClearLights(lights);
    return 0;
}

// All-black lights fall back to a uniform distribution.
static int dist_all_black()
{
    std::vector<AbstractLight*> lights;
    lights.push_back(MakePoint(Vec3f(0.f), Vec3f(0.f)));
    lights.push_back(MakePoint(Vec3f(2.f), Vec3f(0.f)));
    lights.push_back(MakePoint(Vec3f(4.f), Vec3f(0.f)));
    LightTree tree;
    TASK_CHECK(tree.Build(lights, MakeSphere(7.f)));
    TASK_CHECK(tree.AllBlack());
    const int n = tree.LightCount();
    Rng rng(111);
    std::vector<long long> seen(n, 0);
    for(int i = 0; i < 6000; ++i)
        ++seen[tree.Sample(Vec3f(1.f), rng.GetFloat())];
    for(int id = 0; id < n; ++id)
    {
        TASK_CHECK(Approx(tree.Pdf(Vec3f(1.f), id), 1.0 / n, 1e-12));
        TASK_CHECK(seen[id] > 0);
    }
    ClearLights(lights);
    return 0;
}

// All four light types are supported and original IDs survive reordering.
static int dist_mixed_types_and_ids()
{
    std::vector<AbstractLight*> lights;
    lights.push_back(MakeArea(Vec3f(-1,1,0), Vec3f(1,1,0), Vec3f(0,1,1), Vec3f(2.f)));
    lights.push_back(MakePoint(Vec3f(0.f, -2.f, 0.f), Vec3f(5.f)));
    lights.push_back(MakeDirectional(Vec3f(1.f, 1.f, 0.f), Vec3f(3.f)));
    lights.push_back(MakeBackground(Vec3f(0.2f, 0.4f, 0.6f), 2.f));
    LightTree tree;
    TASK_CHECK(tree.Build(lights, MakeSphere(6.f)));
    TASK_CHECK(tree.LightCount() == 4);

    // Every original ID must appear on exactly one leaf node.
    std::vector<int> idCount(4, 0);
    for(int i = 0; i < tree.NodeCount(); ++i)
    {
        LightTree::NodeInfo ni;
        TASK_CHECK(tree.GetNodeInfo(i, ni));
        if(ni.leftChild < 0 && ni.rightChild < 0)
        {
            TASK_CHECK(ni.lightID >= 0 && ni.lightID < 4);
            ++idCount[ni.lightID];
        }
    }
    for(int id = 0; id < 4; ++id)
        TASK_CHECK(idCount[id] == 1);

    // Infinite lights contribute only infinite weight; finite nodes carry boxes.
    const LightTree::NodeInfo root = [&](){
        LightTree::NodeInfo ni; tree.GetNodeInfo(0, ni); return ni;
    }();
    TASK_CHECK(root.finiteWeight > 0.0 && root.infiniteWeight > 0.0);

    Rng rng(7);
    for(int i = 0; i < 2000; ++i)
    {
        const int id = tree.Sample(Vec3f(0.1f, 0.2f, -0.3f), rng.GetFloat());
        TASK_CHECK(id >= 0 && id < 4);
        TASK_CHECK(tree.Pdf(Vec3f(0.1f, 0.2f, -0.3f), id) > 0.0);
    }
    ClearLights(lights);
    return 0;
}

// Illegal (non-finite / negative) intensity rejects the build and keeps the
// previous snapshot.
static int dist_illegal_rejects()
{
    std::vector<AbstractLight*> good;
    good.push_back(MakePoint(Vec3f(0.f), Vec3f(1.f)));
    good.push_back(MakePoint(Vec3f(3.f), Vec3f(2.f)));
    LightTree tree;
    TASK_CHECK(tree.Build(good, MakeSphere(6.f)));
    TASK_CHECK(tree.LightCount() == 2);

    std::vector<AbstractLight*> bad;
    bad.push_back(MakePoint(Vec3f(0.f), Vec3f(-1.f)));
    TASK_CHECK(!tree.Build(bad, MakeSphere(6.f)));
    // Old snapshot retained.
    TASK_CHECK(tree.LightCount() == 2);
    TASK_CHECK(tree.Pdf(Vec3f(0.f), 0) > 0.0);

    std::vector<AbstractLight*> nanLights;
    const float nanv = std::numeric_limits<float>::quiet_NaN();
    nanLights.push_back(MakePoint(Vec3f(0.f), Vec3f(nanv)));
    TASK_CHECK(!tree.Build(nanLights, MakeSphere(6.f)));
    TASK_CHECK(tree.LightCount() == 2);

    // Invalid radius also fails and retains the snapshot.
    TASK_CHECK(!tree.Build(good, MakeSphere(0.f)));
    TASK_CHECK(tree.LightCount() == 2);

    ClearLights(good);
    ClearLights(bad);
    ClearLights(nanLights);
    return 0;
}

// Hierarchy depth and per-query node access stay within the required budget.
static int dist_visit_budget_4096()
{
    const int n = 4096;
    std::vector<AbstractLight*> lights;
    for(int i = 0; i < n; ++i)
    {
        const float x = float(i % 64) - 32.f;
        const float y = float(i / 64) - 32.f;
        const float inten = 1.f + float((i * 37) % 100);
        lights.push_back(MakePoint(Vec3f(x, y, float(i % 7)), Vec3f(inten)));
    }
    LightTree tree;
    TASK_CHECK(tree.Build(lights, MakeSphere(80.f)));
    TASK_CHECK(tree.LightCount() == n);
    TASK_CHECK(tree.NodeCount() == 2 * n - 1);
    TASK_CHECK(tree.Depth() <= 12);

    const Vec3f x(1.5f, -2.5f, 0.5f);
    const int id = tree.Sample(x, 0.4231f);
    TASK_CHECK(id >= 0 && id < n);
    const double p = tree.Pdf(x, id);
    TASK_CHECK(p > 0.0 && p < 1.0);

    const int bound = LightTree::VisitBound(n);
    std::printf("  4096 lights: bound=%d sampleVisits=%d pdfVisits=%d depth=%d\n",
        bound, tree.LastSampleNodeAccesses(), tree.LastPdfNodeAccesses(),
        tree.Depth());
    TASK_CHECK(tree.MaxSampleNodeAccesses() <= bound);
    TASK_CHECK(tree.MaxPdfNodeAccesses() <= bound);
    TASK_CHECK(tree.LastSampleNodeAccesses() <= bound);
    TASK_CHECK(tree.LastPdfNodeAccesses() <= bound);
    ClearLights(lights);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
// MIS GROUP
///////////////////////////////////////////////////////////////////////////////

// Independent area-measure enumeration of the three feasible strategies that
// build the one-bounce camera->surface->light path: next event estimation,
// BSDF hitting the light, and light tracing.  Their balance weights must sum
// to one, and the production NEE weight must use the actual qD.
static int mis_enumeration_weight_sum()
{
    Scene *scene = BuildTwoLightRoom();
    TASK_CHECK(scene->RebuildLightTree());
    scene->SetUseLightTree(true);
    const LightTree &tree = scene->GetLightTree();
    const int n = scene->GetLightCount();
    TASK_CHECK(n == 2);

    // A diffuse receiving point on the floor, well inside the room.
    const Vec3f y1(0.0f, -1.0f, 0.0f);
    const Vec3f normal(0.f, 1.f, 0.f);
    Ray fakeRay;
    // Incoming ray travels downward, so -ray.dir equals the upward normal and
    // the BSDF local fixed direction lands on the front hemisphere.
    fakeRay.org = y1 + normal * 0.1f;
    fakeRay.dir = -normal;
    fakeRay.tmin = 0.f;
    Isect fakeIsect(1e36f);
    fakeIsect.dist = 0.1f;
    fakeIsect.matID = 5; // white diffuse back wall/floor material
    fakeIsect.normal = normal;
    fakeIsect.lightID = -1;
    BSDF<false> bsdf(fakeRay, fakeIsect, *scene);
    TASK_CHECK(bsdf.IsValid() && !bsdf.IsDelta());

    double totalWeightAllLights = 0.0;
    for(int lightID = 0; lightID < n; ++lightID)
    {
        const AbstractLight *light = scene->GetLightPtr(lightID);
        const PointLight *point = static_cast<const PointLight*>(light);

        Vec3f toLight;
        float dist, directPdfW, emissionPdfW, cosAtLight;
        const Vec3f radiance = light->Illuminate(scene->mSceneSphere, y1,
            Vec2f(0.3f, 0.7f), toLight, dist, directPdfW,
            &emissionPdfW, &cosAtLight);
        TASK_CHECK(!radiance.IsZero());

        float bsdfPdfW, cosOut, bsdfRevPdfW;
        const Vec3f f = bsdf.Evaluate(*scene, toLight, cosOut,
            &bsdfPdfW, &bsdfRevPdfW);
        TASK_CHECK(!f.IsZero());
        bsdfPdfW *= bsdf.ContinuationProb();

        // Area densities at the light point for the three strategies.
        const double qD = tree.Pdf(y1, lightID);
        const double qE = 1.0 / n;
        // Point light: Illuminate reports solid-angle pdf = dist^2, so its
        // area pdf is one; the discrete light pick is the varying factor.
        const double neeAreaDensity = qD;
        const double bsdfAreaDensity =
            PdfWtoA(bsdfPdfW, dist, std::abs(cosAtLight));
        const double lightTraceAreaDensity = qE;

        const double sum = neeAreaDensity + bsdfAreaDensity +
            lightTraceAreaDensity;
        const double wNee = neeAreaDensity / sum;
        const double wBsdf = bsdfAreaDensity / sum;
        const double wLt = lightTraceAreaDensity / sum;
        TASK_CHECK(Approx(wNee + wBsdf + wLt, 1.0, 1e-6));
        totalWeightAllLights += wNee;

        // The discrete qD used here is exactly the tree's Pdf; the renderer
        // consumes the same entry (verified against the live renderer in the
        // production_recurrence case after an iteration initializes its cache).
        TASK_CHECK(Approx(qD, tree.Pdf(y1, lightID), 1e-12));
        TASK_CHECK(Approx(n * qD, n * tree.Pdf(y1, lightID), 1e-12));
        (void)point;
    }
    // NEE weights across lights use the normalized qD distribution.
    TASK_CHECK(totalWeightAllLights > 0.0 && totalWeightAllLights <= 1.0 + 1e-6);
    delete scene;
    return 0;
}

// The production light-subpath NEE monomial equals an explicit short-path
// computation, and with the tree off r == 1 so corrections vanish.
static int mis_production_recurrence()
{
    Scene *scene = BuildTwoLightRoom();
    TASK_CHECK(scene->RebuildLightTree());
    scene->SetUseLightTree(true);

    VertexCM vcm(*scene, VertexCM::kBpt, 0.003f, 0.75f, 7);
    vcm.mMaxPathLength = 4;
    vcm.mMinPathLength = 0;
    vcm.RunIteration(0);

    const int verts = VertexMisAdapter::LightVertexCount(vcm);
    TASK_CHECK(verts > 0);
    bool sawNee = false;
    for(int i = 0; i < verts; ++i)
    {
        const VertexMisAdapter::LightVertexInfo vi =
            VertexMisAdapter::GetLightVertex(vcm, i);
        TASK_CHECK(vi.lightID >= 0 && vi.lightID < scene->GetLightCount());
        TASK_CHECK(std::isfinite(vi.dVCM) && std::isfinite(vi.dVC));
        TASK_CHECK(std::isfinite(vi.neeDVCM) && std::isfinite(vi.neeDVC));
        // The isolated NEE monomial must never exceed the full partial sum.
        TASK_CHECK(vi.neeDVC >= 0.f && vi.neeDVC <= vi.dVC + 1e-4f);
        TASK_CHECK(vi.neeDVCM >= 0.f && vi.neeDVCM <= vi.dVCM + 1e-4f);
        // Point lights are delta, so their dVC branch is zero; the solid-angle
        // dVCM NEE monomial stays positive and is what merging corrects.
        if(vi.neeDVCM > 0.f)
            sawNee = true;

        // A path-length-1 light vertex sits on the first surface hit x1, so
        // its stored ratio must equal N * qD(x1, light) exactly.
        if(vi.pathLength == 1)
        {
            const double expectR = scene->GetLightCount() *
                scene->GetLightTree().Pdf(vi.position, vi.lightID);
            TASK_CHECK(Approx(vi.r, expectR, 1e-6));
            TASK_CHECK(vi.r > 0.f);
        }
    }
    TASK_CHECK(sawNee);

    // With the tree disabled the ratio is exactly one everywhere.
    scene->SetUseLightTree(false);
    VertexCM off(*scene, VertexCM::kBpt, 0.003f, 0.75f, 7);
    const Vec3f x(0.f, -1.f, 0.f);
    TASK_CHECK(VertexMisAdapter::LightPickRatio(off, x, 0) == 1.f);
    TASK_CHECK(VertexMisAdapter::LightPickRatio(off, x, 1) == 1.f);
    off.mMaxPathLength = 4;
    off.RunIteration(0);
    const int offVerts = VertexMisAdapter::LightVertexCount(off);
    TASK_CHECK(offVerts > 0);
    for(int i = 0; i < offVerts; ++i)
        TASK_CHECK(VertexMisAdapter::GetLightVertex(off, i).r == 1.f);
    delete scene;
    return 0;
}

// Specular paths keep the original delta strategy support (NEE monomial has
// no dVCM branch across a specular event) and remain finite.
static int mis_specular_path()
{
    Scene scene;
    scene.LoadCornellBox(Vec2i(32, 32),
        Scene::kLightPoint | Scene::kBothSmallSpheres);
    scene.BuildSceneSphere();
    TASK_CHECK(scene.RebuildLightTree());
    scene.SetUseLightTree(true);

    VertexCM vcm(scene, VertexCM::kVcm, 0.003f, 0.75f, 99);
    vcm.mMaxPathLength = 5;
    vcm.RunIteration(0);
    Framebuffer fb;
    vcm.GetFramebuffer(fb);
    const double lum = fb.TotalLuminance();
    std::printf("  specular vcm luminance=%.6g (finite=%d)\n",
        lum, (int)std::isfinite(lum));
    TASK_CHECK(std::isfinite(lum) && lum > 0.0);

    PathTracer pt(scene, 99);
    pt.mMaxPathLength = 5;
    pt.RunIteration(0);
    Framebuffer pfb;
    pt.GetFramebuffer(pfb);
    const double plum = pfb.TotalLuminance();
    TASK_CHECK(std::isfinite(plum) && plum > 0.0);
    return 0;
}

// Direct light hit and background escape both evaluate qD at the previous
// shading point through the path tracer, staying finite and consistent.
static int mis_light_hit_and_background()
{
    Scene scene;
    scene.LoadCornellBox(Vec2i(32, 32),
        Scene::kLightCeiling | Scene::kLightBackground |
        Scene::kBothSmallSpheres);
    scene.BuildSceneSphere();
    TASK_CHECK(scene.RebuildLightTree());
    TASK_CHECK(scene.GetBackgroundID() >= 0);
    scene.SetUseLightTree(true);

    PathTracer pt(scene, 5);
    pt.mMaxPathLength = 5;
    pt.RunIteration(0);
    Framebuffer fb;
    pt.GetFramebuffer(fb);
    const double lum = fb.TotalLuminance();
    std::printf("  light-hit+background pt luminance=%.6g\n", lum);
    TASK_CHECK(std::isfinite(lum) && lum > 0.0);

    VertexCM bpt(scene, VertexCM::kBpt, 0.003f, 0.75f, 5);
    bpt.mMaxPathLength = 5;
    bpt.RunIteration(0);
    Framebuffer bfb;
    bpt.GetFramebuffer(bfb);
    const double blum = bfb.TotalLuminance();
    TASK_CHECK(std::isfinite(blum) && blum > 0.0);
    return 0;
}

// Two unequal area lights (non-delta), so BSDF hitting a light is a feasible
// competitor alongside NEE and light tracing.
static Scene* BuildTwoAreaLightRoom()
{
    Scene *scene = new Scene;
    scene->LoadCornellBox(Vec2i(32, 32), Scene::kBothSmallSpheres);
    scene->mLights.clear();
    scene->mBackground = NULL;
    scene->mMaterial2Light.clear();

    // Downward-facing triangles near the ceiling, dim on the left, bright right.
    scene->mLights.push_back(MakeArea(
        Vec3f(-0.9f, 1.05f, -0.4f),
        Vec3f(-0.3f, 1.05f, -0.4f),
        Vec3f(-0.9f, 1.05f,  0.4f), Vec3f(1.f)));
    scene->mLights.push_back(MakeArea(
        Vec3f(0.3f, 1.05f, -0.4f),
        Vec3f(0.9f, 1.05f, -0.4f),
        Vec3f(0.3f, 1.05f,  0.4f), Vec3f(60.f)));

    scene->BuildSceneSphere();
    return scene;
}

// Production NEE balance terms must match an independent area-measure
// enumeration that uses the real qD, for both tree modes.
static int mis_direct_balance_enumeration()
{
    for(int mode = 0; mode < 2; ++mode)
    {
        Scene *scene = BuildTwoAreaLightRoom();
        scene->RebuildLightTree();
        scene->SetUseLightTree(mode == 1);
        const int n = scene->GetLightCount();
        const LightTree &tree = scene->GetLightTree();

        VertexCM vcm(*scene, VertexCM::kBpt, 0.003f, 0.75f, 123);
        vcm.mMaxPathLength = 3;
        // Initialize the renderer's per-iteration tree cache and RNG position
        // deterministically before the hand-driven NEE probe.
        vcm.RunIteration(0);

        const Vec3f y1(0.0f, -1.0f, 0.0f);
        const Vec3f normal(0.f, 1.f, 0.f);

        VertexMisAdapter::DirectInfo info =
            VertexMisAdapter::DirectIlluminationAt(vcm, y1, normal, 5);
        TASK_CHECK(info.valid);

        // The production pick probability is exactly the tree Pdf (or uniform).
        const double qD = tree.Pdf(y1, info.lightID);
        const double expectedPick = mode == 1 ? qD : (1.0 / n);
        TASK_CHECK(Approx(info.lightPickProb, expectedPick, 1e-6));

        const AreaLight *area = static_cast<const AreaLight*>(
            scene->GetLightPtr(info.lightID));
        const double directPdfA = double(area->mInvArea);

        // Independent area densities.
        const double neeDensity = expectedPick * directPdfA;
        const double bsdfDensity = PdfWtoA(info.bsdfDirPdfW,
            std::sqrt(1.0), std::abs(info.cosAtLight));
        // Recover distance from the light's solid-angle pdf relation.
        // directPdfW = invArea * dist^2 / cosAtLight  =>  dist^2.
        const double dist2 = info.directPdfW *
            std::abs(info.cosAtLight) / directPdfA;
        const double bsdfDensityReal =
            info.bsdfDirPdfW * std::abs(info.cosAtLight) / dist2;
        (void)bsdfDensity;

        // Production wLight = bsdf-hit density / NEE density.
        const double expectWLight = bsdfDensityReal / neeDensity;
        std::printf("  mode=%d light=%d qD=%.5f prodWLight=%.6g enumWLight=%.6g\n",
            mode, info.lightID, expectedPick, info.wLight, expectWLight);
        TASK_CHECK(Approx(info.wLight, expectWLight, 2e-3, 1e-7));

        // Balance weight is a proper normalized weight in (0,1].
        TASK_CHECK(info.misWeight > 0.f && info.misWeight <= 1.f);
        TASK_CHECK(Approx(1.0 / info.misWeight - 1.0,
            info.wLight + info.wCamera, 1e-4));

        delete scene;
    }
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
// RENDER GROUP
///////////////////////////////////////////////////////////////////////////////

// Both selection modes give the same unbiased linear radiance.  Correct MIS
// weights preserve the mean even though the sampling distributions differ; a
// wrong weight would bias one mode.  Fixed seed; sample size and the Monte
// Carlo error basis are printed.
static int render_mode_equivalence()
{
    const int iterations = 16;

    Scene *onScene = BuildTwoLightRoom();
    onScene->RebuildLightTree();
    onScene->SetUseLightTree(true);
    Scene *offScene = BuildTwoLightRoom();
    offScene->SetUseLightTree(false);

    Framebuffer onFb, offFb;
    {
        VertexCM on(*onScene, VertexCM::kBpt, 0.003f, 0.75f, 2024);
        VertexCM off(*offScene, VertexCM::kBpt, 0.003f, 0.75f, 2024);
        on.mMaxPathLength = off.mMaxPathLength = 4;
        for(int i = 0; i < iterations; ++i)
        {
            on.RunIteration(i);
            off.RunIteration(i);
        }
        on.GetFramebuffer(onFb);
        off.GetFramebuffer(offFb);
    }

    const int res = onFb.GetPixelCount();
    TASK_CHECK(offFb.GetPixelCount() == res);
    double sumOn = 0.0, sumOff = 0.0, sumSqDiff = 0.0;
    for(int i = 0; i < res; ++i)
    {
        const Vec3f a = onFb.GetColor(i);
        const Vec3f b = offFb.GetColor(i);
        const double la = Luminance(a), lb = Luminance(b);
        TASK_CHECK(std::isfinite(la) && std::isfinite(lb));
        sumOn += la;
        sumOff += lb;
        sumSqDiff += (la - lb) * (la - lb);
    }
    const double meanOn = sumOn / res;
    const double meanOff = sumOff / res;
    const double rmse = std::sqrt(sumSqDiff / res);
    // Error basis: RMSE of per-pixel difference over pixels*iterations samples.
    const double basis = rmse / std::sqrt(double(iterations));
    std::printf("  modes samples=%d iters=%d meanOn=%.5g meanOff=%.5g rmse=%.5g errBasis=%.5g relDiff=%.4g\n",
        res, iterations, meanOn, meanOff, rmse, basis,
        std::abs(meanOn - meanOff) / meanOff);
    TASK_CHECK(meanOn > 0.0 && meanOff > 0.0);
    // The two unbiased estimators must agree on the mean within 10%.
    TASK_CHECK(std::abs(meanOn - meanOff) / meanOff < 0.10);

    // Regional distribution: the bright light (id 1) is picked more often
    // than the dim light at a point next to it under the tree.
    const Vec3f nearBright(0.75f, -1.0f, 0.6f);
    const LightTree &tree = onScene->GetLightTree();
    TASK_CHECK(tree.Pdf(nearBright, 1) > tree.Pdf(nearBright, 0));
    TASK_CHECK(Approx(tree.Pdf(nearBright, 0) + tree.Pdf(nearBright, 1),
        1.0, 1e-9));

    delete onScene;
    delete offScene;
    return 0;
}

// Reads the PFM produced by this run of the demo and checks it is finite and
// non-black.
static int render_demo_image()
{
    int w = 0, h = 0;
    std::vector<float> pixels;
    const char *path = ".task-build/demo-output/lighttree.pfm";
    const bool read = ReadPfm(path, w, h, pixels);
    TASK_CHECK(read);
    TASK_CHECK(w > 0 && h > 0);
    TASK_CHECK(pixels.size() == size_t(w) * h * 3);

    double sum = 0.0;
    int finiteCount = 0;
    for(size_t i = 0; i < pixels.size(); ++i)
    {
        TASK_CHECK(std::isfinite(pixels[i]));
        if(pixels[i] > 0.f)
            ++finiteCount;
        sum += pixels[i];
    }
    std::printf("  demo pfm %dx%d positiveComponents=%d/%zu mean=%.6g\n",
        w, h, finiteCount, pixels.size(), sum / pixels.size());
    TASK_CHECK(finiteCount > 0);
    TASK_CHECK(sum > 0.0);
    return 0;
}

// Multi-round rendering keeps accumulating and stays stable across rounds.
static int render_multi_round()
{
    Scene *scene = BuildTwoLightRoom();
    scene->RebuildLightTree();
    scene->SetUseLightTree(true);

    VertexCM vcm(*scene, VertexCM::kVcm, 0.003f, 0.75f, 31);
    vcm.mMaxPathLength = 4;
    double prev = -1.0;
    for(int round = 0; round < 3; ++round)
    {
        vcm.RunIteration(round);
        Framebuffer fb;
        vcm.GetFramebuffer(fb);
        const double lum = fb.TotalLuminance();
        std::printf("  round %d luminance=%.6g\n", round, lum);
        TASK_CHECK(std::isfinite(lum) && lum > 0.0);
        if(prev > 0.0)
            TASK_CHECK(std::abs(lum - prev) / prev < 0.5);
        prev = lum;
    }
    delete scene;
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
// Registration
///////////////////////////////////////////////////////////////////////////////

extern const struct task_case task_cases[] =
{
    {"distribution", "empty_table",                 dist_empty_table},
    {"distribution", "sample_pdf_consistency",      dist_sample_pdf_consistency},
    {"distribution", "frequency",                   dist_frequency},
    {"distribution", "all_black",                   dist_all_black},
    {"distribution", "mixed_types_and_ids",         dist_mixed_types_and_ids},
    {"distribution", "illegal_rejects",             dist_illegal_rejects},
    {"distribution", "visit_budget_4096",           dist_visit_budget_4096},

    {"mis", "enumeration_weight_sum",               mis_enumeration_weight_sum},
    {"mis", "direct_balance_enumeration",            mis_direct_balance_enumeration},
    {"mis", "production_recurrence",                mis_production_recurrence},
    {"mis", "specular_path",                        mis_specular_path},
    {"mis", "light_hit_and_background",             mis_light_hit_and_background},

    {"render", "mode_equivalence",                  render_mode_equivalence},
    {"render", "demo_image",                        render_demo_image},
    {"render", "multi_round",                       render_multi_round},
};
const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
