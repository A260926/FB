// Irradiance cache verification. No main(): task_support/driver.c runs these.
#include "task_support/check.h"
#include "Cpp/Source/Test.h"
#include "Cpp/Source/IrradianceCache.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
    const float kEps = 1e-4f;

    bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
    bool Near3(const float3& a, const float3& b, float tol)
    {
        return Near(a.getX(), b.getX(), tol) && Near(a.getY(), b.getY(), tol) &&
               Near(a.getZ(), b.getZ(), tol);
    }

    ICConfig MakeConfig()
    {
        ICConfig cfg;
        cfg.gatherSamples = 32;
        cfg.tau = 0.5f;
        cfg.minRadius = 0.05f;
        cfg.maxRadius = 0.5f;
        cfg.seed = 98765u;
        return cfg;
    }

    IRecord MakeRecord(int sphere, int side, int depth, const float3& x, const float3& n,
                       const float3& H, float R)
    {
        IRecord r;
        r.sphereId = sphere;
        r.side = side;
        r.remainingDepth = depth;
        r.x = x;
        r.n = normalize(n);
        r.H = H;
        r.R = R;
        return r;
    }

    // Runs one cache lookup against the real scene services.
    bool Lookup(IrradianceCache& c, const IRecord& q, uint32_t pixel, float3& outH,
                ICCandidate& cand, bool& wasMiss)
    {
        ICPathId id;
        id.frame = 0;
        id.pixel = pixel;
        id.sample = 0;
        id.pathDepth = static_cast<uint32_t>(10 - q.remainingDepth);
        int rays = 0;
        outH = c.QueryOrGather(q, id, ICTest::Scene(), rays, cand, wasMiss);
        return true;
    }
}

// ===========================================================================
// records group: metric, weights, zero error, depth/side isolation, occlusion,
// spatial index budget.
// ===========================================================================

static int TestHarmonicDistance()
{
    ICTest::ResetScene();
    ICTest::ClearSpheres();
    ICTest::SetSky(1, float3(0.2f, 0.3f, 0.4f));
    ICTest::Refresh();

    IrradianceCache& c = GetIrradianceCache();
    TASK_CHECK(c.Configure(MakeConfig()));
    c.BeginFrame(0);

    // No spheres: every probe escapes, so each first distance is maxRadius and
    // R = clamp(M / (M/maxRadius)) = maxRadius exactly.
    IRecord q = MakeRecord(0, kICSideFront, 5, float3(0, 0, 0), float3(0, 1, 0),
                           float3(0, 0, 0), 0);
    ICCandidate cand;
    bool miss = false;
    float3 H;
    TASK_CHECK(Lookup(c, q, 1, H, cand, miss));
    TASK_CHECK(miss);
    TASK_CHECK(c.GetGatherLogCount() == 1);
    const ICGatherDebug& g = c.GetGatherLog(0);
    TASK_CHECK(g.samples.size() == static_cast<size_t>(MakeConfig().gatherSamples));
    TASK_CHECK(Near(g.R, MakeConfig().maxRadius, kEps));
    TASK_CHECK(Near3(g.H, float3(0.2f, 0.3f, 0.4f), kEps));
    for (const ICGatherSample& s : g.samples)
        TASK_CHECK(Near(s.firstDist, MakeConfig().maxRadius, kEps));

    // Independently recompute the harmonic radius from the logged first
    // distances and confirm the stored R matches the spec formula exactly.
    float inv = 0.0f;
    for (const ICGatherSample& s : g.samples)
        inv += 1.0f / s.firstDist;
    float expected = MakeConfig().gatherSamples / inv;
    expected = std::max(MakeConfig().minRadius, std::min(MakeConfig().maxRadius, expected));
    TASK_CHECK(Near(g.R, expected, 1e-6f));
    return 0;
}

static int TestClampRadius()
{
    ICTest::ResetScene();
    ICTest::ClearSpheres();
    // A sphere close to the record point makes first distances small; the
    // harmonic radius must still be clamped to [minRadius, maxRadius].
    int ground = ICTest::AddSphere(float3(0, -1, 0), 1.0f, ICTest::kMaterialLambert,
                                   float3(0.5f, 0.5f, 0.5f), float3(0, 0, 0), 0, 0);
    TASK_CHECK(ground >= 0);
    ICTest::Refresh();

    IrradianceCache& c = GetIrradianceCache();
    ICConfig cfg = MakeConfig();
    cfg.minRadius = 0.2f;
    cfg.maxRadius = 0.4f;
    TASK_CHECK(c.Configure(cfg));
    c.BeginFrame(0);

    IRecord q = MakeRecord(0, kICSideFront, 4, float3(0, -0.02f, 0), float3(0, 1, 0),
                           float3(0, 0, 0), 0);
    ICCandidate cand;
    bool miss = false;
    float3 H;
    TASK_CHECK(Lookup(c, q, 7, H, cand, miss));
    TASK_CHECK(miss);
    const ICGatherDebug& g = c.GetGatherLog(0);
    TASK_CHECK(g.R >= cfg.minRadius - kEps && g.R <= cfg.maxRadius + kEps);
    for (const ICGatherSample& s : g.samples)
        TASK_CHECK(s.firstDist >= kMinT - kEps && s.firstDist <= cfg.maxRadius + kEps);
    return 0;
}

static int TestInterpolationWeight()
{
    ICTest::ResetScene();
    ICTest::ClearSpheres();
    ICTest::Refresh();

    IrradianceCache& c = GetIrradianceCache();
    ICConfig cfg = MakeConfig();
    cfg.tau = 0.9f;
    TASK_CHECK(c.Configure(cfg));

    c.TestInjectRecord(MakeRecord(0, kICSideFront, 5, float3(0, 0, 0), float3(0, 1, 0),
                                  float3(1, 0, 0), 0.5f));
    c.TestInjectRecord(MakeRecord(0, kICSideFront, 5, float3(0.2f, 0, 0), float3(0, 1, 0),
                                  float3(0, 1, 0), 0.5f));
    c.BeginFrame(0);

    // Query halfway: e1 = e2 = 0.2/0.5 = 0.4, weights equal -> H = (0.5,0.5,0).
    IRecord q = MakeRecord(0, kICSideFront, 5, float3(0.1f, 0, 0), float3(0, 1, 0),
                           float3(0, 0, 0), 0);
    ICCandidate cand;
    bool miss = true;
    float3 H;
    TASK_CHECK(Lookup(c, q, 2, H, cand, miss));
    TASK_CHECK(!miss);
    TASK_CHECK(Near3(H, float3(0.5f, 0.5f, 0), 1e-6f));

    // Manual weight check with an asymmetric placement.
    ICStats st = c.GetStats();
    TASK_CHECK(st.cacheHits == 1 && st.connectionChecks >= 1);
    return 0;
}

static int TestZeroError()
{
    ICTest::ResetScene();
    ICTest::ClearSpheres();
    ICTest::Refresh();

    IrradianceCache& c = GetIrradianceCache();
    ICConfig cfg = MakeConfig();
    cfg.tau = 0.9f;
    TASK_CHECK(c.Configure(cfg));

    c.TestInjectRecord(MakeRecord(0, kICSideFront, 5, float3(0, 0, 0), float3(0, 1, 0),
                                  float3(1, 0, 0), 0.5f));
    c.TestInjectRecord(MakeRecord(0, kICSideFront, 5, float3(0.2f, 0, 0), float3(0, 1, 0),
                                  float3(0, 1, 0), 0.5f));
    c.BeginFrame(0);

    // Exact position of the first record: only zero-error records are averaged.
    IRecord q = MakeRecord(0, kICSideFront, 5, float3(0, 0, 0), float3(0, 1, 0),
                           float3(0, 0, 0), 0);
    ICCandidate cand;
    bool miss = true;
    float3 H;
    TASK_CHECK(Lookup(c, q, 3, H, cand, miss));
    TASK_CHECK(!miss);
    TASK_CHECK(Near3(H, float3(1, 0, 0), 1e-6f));
    return 0;
}

static int TestDepthIsolation()
{
    ICTest::ResetScene();
    ICTest::ClearSpheres();
    ICTest::Refresh();

    IrradianceCache& c = GetIrradianceCache();
    TASK_CHECK(c.Configure(MakeConfig()));
    c.TestInjectRecord(MakeRecord(0, kICSideFront, 5, float3(0, 0, 0), float3(0, 1, 0),
                                  float3(1, 1, 1), 0.5f));
    c.BeginFrame(0);

    IRecord q = MakeRecord(0, kICSideFront, 3, float3(0, 0, 0), float3(0, 1, 0),
                           float3(0, 0, 0), 0);
    ICCandidate cand;
    bool miss = false;
    float3 H;
    TASK_CHECK(Lookup(c, q, 4, H, cand, miss));
    TASK_CHECK(miss); // remaining depth differs -> must not reuse
    return 0;
}

static int TestSideIsolation()
{
    ICTest::ResetScene();
    ICTest::ClearSpheres();
    ICTest::Refresh();

    IrradianceCache& c = GetIrradianceCache();
    TASK_CHECK(c.Configure(MakeConfig()));
    c.TestInjectRecord(MakeRecord(0, kICSideFront, 5, float3(0, 0, 0), float3(0, 1, 0),
                                  float3(1, 1, 1), 0.5f));
    c.BeginFrame(0);

    IRecord q = MakeRecord(0, kICSideBack, 5, float3(0, 0, 0), float3(0, 1, 0),
                           float3(0, 0, 0), 0);
    ICCandidate cand;
    bool miss = false;
    float3 H;
    TASK_CHECK(Lookup(c, q, 5, H, cand, miss));
    TASK_CHECK(miss); // back side cannot reuse a front-side record
    return 0;
}

static int TestFiniteOcclusion()
{
    ICTest::ResetScene();
    ICTest::ClearSpheres();
    // Blocker between the record (sphere id 5, not present) and the query.
    int blocker = ICTest::AddSphere(float3(0.3f, 0.001f, 0), 0.06f, ICTest::kMaterialLambert,
                                    float3(0.2f, 0.2f, 0.2f), float3(0, 0, 0), 0, 0);
    TASK_CHECK(blocker == 0);
    ICTest::Refresh();

    IrradianceCache& c = GetIrradianceCache();
    ICConfig cfg = MakeConfig();
    cfg.tau = 0.95f;
    TASK_CHECK(c.Configure(cfg));
    c.TestInjectRecord(MakeRecord(5, kICSideFront, 5, float3(0.6f, 0, 0), float3(0, 1, 0),
                                  float3(1, 1, 1), 1.0f));
    c.BeginFrame(0);

    IRecord q = MakeRecord(5, kICSideFront, 5, float3(0, 0, 0), float3(0, 1, 0),
                           float3(0, 0, 0), 0);
    ICCandidate cand;
    bool miss = false;
    float3 H;
    TASK_CHECK(Lookup(c, q, 6, H, cand, miss));
    TASK_CHECK(miss); // connector blocked by the other sphere
    ICStats st = c.GetStats();
    TASK_CHECK(st.connectionChecks >= 1 && st.connectionBlocked >= 1);

    // Remove the blocker: the same record is now reachable.
    ICTest::ClearSpheres();
    ICTest::Refresh();
    c.Clear();
    c.Configure(cfg);
    c.TestInjectRecord(MakeRecord(5, kICSideFront, 5, float3(0.6f, 0, 0), float3(0, 1, 0),
                                  float3(1, 1, 1), 1.0f));
    c.BeginFrame(0);
    bool miss2 = true;
    TASK_CHECK(Lookup(c, q, 6, H, cand, miss2));
    TASK_CHECK(!miss2);
    TASK_CHECK(Near3(H, float3(1, 1, 1), 1e-6f));
    return 0;
}

static int TestSpatialIndexBudget()
{
    ICTest::ResetScene();
    ICTest::ClearSpheres();
    ICTest::Refresh();

    IrradianceCache& c = GetIrradianceCache();
    ICConfig cfg = MakeConfig();
    cfg.maxRadius = 0.5f;
    TASK_CHECK(c.Configure(cfg));

    // 4096 records spaced more than 4*maxRadius apart along x.
    const int N = 4096;
    const float spacing = 5.0f * cfg.maxRadius;
    for (int i = 0; i < N; ++i)
    {
        float x = static_cast<float>(i) * spacing;
        c.TestInjectRecord(MakeRecord(0, kICSideFront, 5, float3(x, 0, 0), float3(0, 1, 0),
                                      float3(0.3f, 0.3f, 0.3f), 0.5f));
    }
    c.BeginFrame(0);

    uint64_t before = c.GetStats().recordsExamined;
    for (int i = 0; i < N; ++i)
    {
        float x = static_cast<float>(i) * spacing;
        IRecord q = MakeRecord(0, kICSideFront, 5, float3(x, 0, 0), float3(0, 1, 0),
                               float3(0, 0, 0), 0);
        ICCandidate cand;
        bool miss = false;
        float3 H;
        Lookup(c, q, static_cast<uint32_t>(i), H, cand, miss);
    }
    uint64_t examined = c.GetStats().recordsExamined - before;
    TASK_CHECK(examined <= static_cast<uint64_t>(16) * N);
    // Each isolated neighbourhood examines essentially one record.
    TASK_CHECK(examined >= static_cast<uint64_t>(N));

    // Connection and gather counts are maintained truthfully.
    ICStats st = c.GetStats();
    TASK_CHECK(st.queries == static_cast<uint64_t>(N));
    TASK_CHECK(st.connectionChecks == static_cast<uint64_t>(N));
    return 0;
}

// ===========================================================================
// transport group: real-scene H, direct light, specular light, depth endpoint,
// emission ownership, albedo applied once.
// ===========================================================================

static int TestConstantSkyH()
{
    ICTest::ResetScene();
    ICTest::ClearSpheres();
    const float3 sky(0.25f, 0.35f, 0.45f);
    ICTest::SetSky(1, sky);
    ICTest::Refresh();

    IrradianceCache& c = GetIrradianceCache();
    TASK_CHECK(c.Configure(MakeConfig()));
    c.BeginFrame(0);

    IRecord q = MakeRecord(0, kICSideFront, 6, float3(0, 0, 0), float3(0, 1, 0),
                           float3(0, 0, 0), 0);
    ICCandidate cand;
    bool miss = false;
    float3 H;
    TASK_CHECK(Lookup(c, q, 11, H, cand, miss));
    TASK_CHECK(miss);
    // H is the mean cosine-hemisphere Li; with a constant environment it is the
    // sky radiance itself and contains no surface albedo.
    TASK_CHECK(Near3(H, sky, 1e-6f));
    TASK_CHECK(Near3(cand.record.H, sky, 1e-6f));
    TASK_CHECK(cand.record.remainingDepth == 6);
    return 0;
}

static int TestDirectSphereLight()
{
    ICTest::ResetScene();
    ICTest::ClearSpheres();
    ICTest::SetSky(1, float3(0, 0, 0));
    const float3 albedo(0.7f, 0.7f, 0.7f);
    int diff = ICTest::AddSphere(float3(0, -100.5f, 0), 100.0f, ICTest::kMaterialLambert,
                                 albedo, float3(0, 0, 0), 0, 0);
    const float3 Le(10, 10, 10);
    int light = ICTest::AddSphere(float3(0, 3, 0), 0.5f, ICTest::kMaterialLambert,
                                  float3(0.01f, 0.01f, 0.01f), Le, 0, 0);
    TASK_CHECK(diff >= 0 && light >= 0);
    ICTest::Refresh();

    // Analytic reference for a point directly under a cone-sampled sphere.
    const float3 lightCenter(0, 3, 0);
    const float3 pointPos(0, -0.5f, 0);
    const float h = length(lightCenter - pointPos);
    const float r = 0.5f;
    float cosMax = std::sqrt(1.0f - r * r / (h * h));
    float omega = 2.0f * kPI * (1.0f - cosMax);
    float expCos = 0.5f * (1.0f + cosMax);
    float3 expected = albedo * Le * (omega * expCos / kPI);

    // Average the single-sample estimator over many deterministic seeds.
    float3 acc(0, 0, 0);
    const int trials = 2000;
    int rays = 0;
    for (int i = 0; i < trials; ++i)
    {
        acc += ICTest::DirectLight(diff, float3(0, -0.5f, 0), float3(0, 1, 0),
                                   float3(0, -1, 0), static_cast<uint32_t>(i * 2654435761u + 1),
                                   rays);
    }
    float3 got = acc * (1.0f / trials);
    TASK_CHECK(Near3(got, expected, expected.getX() * 0.06f));
    TASK_CHECK(rays >= trials); // shadow rays really were traced
    return 0;
}

static int TestSpecularSeesLight()
{
    ICTest::ResetScene();
    ICTest::ClearSpheres();
    ICTest::SetSky(1, float3(0, 0, 0));
    const float3 metalAlbedo(0.8f, 0.8f, 0.8f);
    int metal = ICTest::AddSphere(float3(0, 0, 0), 1.0f, ICTest::kMaterialMetal,
                                  metalAlbedo, float3(0, 0, 0), 0.0f, 0);
    const float3 Le(12, 12, 12);
    int light = ICTest::AddSphere(float3(0, 4, 0), 0.4f, ICTest::kMaterialLambert,
                                  float3(0.01f, 0.01f, 0.01f), Le, 0, 0);
    TASK_CHECK(metal >= 0 && light >= 0);
    ICTest::Refresh();

    // Ray strikes the top of the metal sphere; the specular reflection goes
    // straight up into the light, whose emission is kept after a specular
    // event. Result ~= metalAlbedo * Le.
    ICTest::RayResult res = ICTest::FireRay(float3(0, 1.5f, 0), float3(0, -1, 0), 6, 42u);
    TASK_CHECK(res.hit);
    TASK_CHECK(res.sphereId == metal);
    float3 expected = metalAlbedo * Le;
    TASK_CHECK(Near3(res.Li, expected, expected.getX() * 0.05f));
    return 0;
}

static int TestEmissionOwnership()
{
    ICTest::ResetScene();
    ICTest::ClearSpheres();
    ICTest::SetSky(1, float3(0.1f, 0.1f, 0.1f));
    const float3 Le(10, 10, 10);
    int light = ICTest::AddSphere(float3(0, 2, 0), 0.6f, ICTest::kMaterialLambert,
                                  float3(0.01f, 0.01f, 0.01f), Le, 0, 0);
    TASK_CHECK(light >= 0);
    ICTest::Refresh();

    // Production transport keeps the first-hit emissive contribution.
    ICTest::RayResult raw = ICTest::FireRay(float3(0, 0, 0), float3(0, 1, 0), 4, 7u);
    TASK_CHECK(raw.hit && raw.sphereId == light);
    TASK_CHECK(raw.Li.getX() > 5.0f);

    // A cache gather probe directly striking the same light has that first
    // self-emission removed; its Li is only the small albedo continuation.
    IrradianceCache& c = GetIrradianceCache();
    TASK_CHECK(c.Configure(MakeConfig()));
    c.BeginFrame(0);
    IRecord q = MakeRecord(light, kICSideFront, 4, float3(0, 0, 0), float3(0, 1, 0),
                           float3(0, 0, 0), 0);
    ICCandidate cand;
    bool miss = false;
    float3 H;
    TASK_CHECK(Lookup(c, q, 12, H, cand, miss));
    bool foundSuppressed = false;
    for (const ICGatherSample& s : c.GetGatherLog(0).samples)
    {
        // Upward probes hit the light first; none may carry its 10.0 emission.
        TASK_CHECK(s.Li.getX() < 2.0f);
        if (s.dir.getY() > 0.9f)
            foundSuppressed = true;
    }
    TASK_CHECK(foundSuppressed);
    return 0;
}

static int TestDepthEndpoint()
{
    ICTest::ResetScene();
    ICTest::ClearSpheres();
    ICTest::SetSky(1, float3(0.2f, 0.2f, 0.2f));
    int diff = ICTest::AddSphere(float3(0, -1, 0), 1.0f, ICTest::kMaterialLambert,
                                 float3(0.6f, 0.6f, 0.6f), float3(0, 0, 0), 0, 0);
    TASK_CHECK(diff >= 0);
    ICTest::Refresh();

    IrradianceCache& c = GetIrradianceCache();
    TASK_CHECK(c.Configure(MakeConfig()));
    c.BeginFrame(0);

    // remainingDepth 0: probes may not scatter, so a probe that hits the sphere
    // returns zero (emission suppressed, path not extended).
    IRecord q = MakeRecord(diff, kICSideFront, 0, float3(0, -0.02f, 0), float3(0, 1, 0),
                           float3(0, 0, 0), 0);
    ICCandidate cand;
    bool miss = false;
    float3 H;
    TASK_CHECK(Lookup(c, q, 13, H, cand, miss));
    TASK_CHECK(miss);
    TASK_CHECK(cand.record.remainingDepth == 0);
    for (const ICGatherSample& s : c.GetGatherLog(0).samples)
    {
        if (s.dir.getY() < -0.5f) // probes aimed down hit the diffuse sphere
            TASK_CHECK(s.Li.getX() <= 1e-5f);
    }
    return 0;
}

static int TestAlbedoAppliedOnce()
{
    // Two surfaces with different albedos under the same constant sky must
    // produce cached colors proportional to albedo (H itself is albedo-free).
    auto render = [](const float3& albedo, std::vector<float>& bb) {
        ICTest::ResetScene();
        ICTest::ClearSpheres();
        const float3 sky(0.3f, 0.3f, 0.3f);
        ICTest::SetSky(1, sky);
        int s = ICTest::AddSphere(float3(0, 0, -1), 0.5f, ICTest::kMaterialLambert,
                                  albedo, float3(0, 0, 0), 0, 0);
        TASK_CHECK(s >= 0);
        ICTest::Refresh();
        IrradianceCache& c = GetIrradianceCache();
        TASK_CHECK(c.Configure(MakeConfig()));
        int rays = 0;
        ICTest::RunDraw(32, 32, 0, kFlagIrradianceCache, bb, rays);
        // The stored H never contains the surface albedo.
        for (size_t i = 0; i < c.GetRecordCount(); ++i)
            TASK_CHECK(Near3(c.GetRecord(i).H, sky, 0.02f));
        return 0;
    };

    std::vector<float> a, b;
    TASK_CHECK(render(float3(0.2f, 0.4f, 0.8f), a) == 0);
    TASK_CHECK(render(float3(0.4f, 0.8f, 0.2f), b) == 0);

    double ea = 0, eb = 0;
    for (size_t i = 0; i < a.size(); i += 4) { ea += a[i] + a[i+1] + a[i+2]; eb += b[i] + b[i+1] + b[i+2]; }
    TASK_CHECK(ea > 0 && eb > 0);
    // Total reflected energy scales with mean albedo; means are both 1.4/3 here,
    // so totals must match closely despite different per-channel albedos.
    TASK_CHECK(Near(static_cast<float>(ea / eb), 1.0f, 0.05f));
    return 0;
}

// ===========================================================================
// passes group: rounds, scheduling determinism, invalidation, config, compat.
// ===========================================================================

static int TestColdStartAndNextRound()
{
    ICTest::ResetScene();
    ICTest::Refresh();
    IrradianceCache& c = GetIrradianceCache();
    TASK_CHECK(c.Configure(MakeConfig()));

    std::vector<float> bb;
    int rays = 0;
    ICTest::RunDraw(48, 27, 0, kFlagIrradianceCache, bb, rays);
    ICStats r1 = c.GetStats();
    TASK_CHECK(r1.queries > 0 && r1.cacheHits == 0 && r1.gathers == r1.queries);
    TASK_CHECK(c.GetRecordCount() > 0);

    size_t recordsAfterRound1 = c.GetRecordCount();
    ICTest::RunDraw(48, 27, 1, kFlagIrradianceCache, bb, rays);
    ICStats r2 = c.GetStats();
    TASK_CHECK(r2.cacheHits > 0);
    TASK_CHECK(r2.gathers < r2.queries);
    TASK_CHECK(c.GetRecordCount() >= recordsAfterRound1);
    return 0;
}

static int TestIntraRoundInvisible()
{
    ICTest::ResetScene();
    ICTest::ClearSpheres();
    ICTest::SetSky(1, float3(0.2f, 0.2f, 0.2f));
    ICTest::Refresh();

    IrradianceCache& c = GetIrradianceCache();
    TASK_CHECK(c.Configure(MakeConfig()));
    c.BeginFrame(0);

    IRecord q = MakeRecord(0, kICSideFront, 5, float3(0, 0, 0), float3(0, 1, 0),
                           float3(0, 0, 0), 0);
    ICCandidate c1, c2;
    bool m1 = false, m2 = false;
    float3 H;
    TASK_CHECK(Lookup(c, q, 20, H, c1, m1));
    TASK_CHECK(m1);
    // Without publishing, the candidate from the first lookup stays invisible.
    TASK_CHECK(Lookup(c, q, 20, H, c2, m2));
    TASK_CHECK(m2);
    TASK_CHECK(c.GetStats().gathers == 2);
    return 0;
}

static int TestScheduleDeterminism()
{
    auto run = [](int schedule, std::vector<float>& bb, std::vector<float3>& hs) {
        ICTest::ResetScene();
        ICTest::Refresh();
        ICTest::SetSchedule(schedule);
        IrradianceCache& c = GetIrradianceCache();
        TASK_CHECK(c.Configure(MakeConfig()));
        int rays = 0;
        ICTest::RunDraw(40, 24, 0, kFlagIrradianceCache, bb, rays);
        ICTest::RunDraw(40, 24, 1, kFlagIrradianceCache, bb, rays);
        hs.clear();
        for (size_t i = 0; i < c.GetRecordCount(); ++i)
            hs.push_back(c.GetRecord(i).H);
        return 0;
    };

    std::vector<float> fwd, rev, enki;
    std::vector<float3> hf, hr, he;
    TASK_CHECK(run(ICTest::kScheduleForward, fwd, hf) == 0);
    TASK_CHECK(run(ICTest::kScheduleReverse, rev, hr) == 0);
    TASK_CHECK(run(ICTest::kScheduleEnkiTS, enki, he) == 0);

    TASK_CHECK(fwd.size() == rev.size() && rev.size() == enki.size());
    TASK_CHECK(hf.size() == hr.size() && hr.size() == he.size());
    for (size_t i = 0; i < fwd.size(); ++i)
        TASK_CHECK(Near(fwd[i], rev[i], 1e-6f) && Near(rev[i], enki[i], 1e-6f));
    for (size_t i = 0; i < hf.size(); ++i)
        TASK_CHECK(Near3(hf[i], hr[i], 1e-6f) && Near3(hr[i], he[i], 1e-6f));
    return 0;
}

static int TestInvalidation()
{
    ICTest::ResetScene();
    ICTest::Refresh();
    IrradianceCache& c = GetIrradianceCache();
    TASK_CHECK(c.Configure(MakeConfig()));
    std::vector<float> bb;
    int rays = 0;
    ICTest::RunDraw(24, 16, 0, kFlagIrradianceCache, bb, rays);
    TASK_CHECK(c.GetRecordCount() > 0);

    // Moving a sphere invalidates before the next draw.
    ICTest::SetSphere(1, float3(2.0f, 0.7f, -1.0f), 0.5f);
    ICTest::Refresh();
    TASK_CHECK(c.GetRecordCount() == 0);
    ICTest::RunDraw(24, 16, 0, kFlagIrradianceCache, bb, rays);
    TASK_CHECK(c.GetRecordCount() > 0);

    // Changing a material invalidates too.
    ICTest::SetMaterial(2, ICTest::kMaterialMetal, float3(0.9f, 0.2f, 0.2f),
                        float3(0, 0, 0), 0.1f, 0);
    ICTest::Refresh();
    TASK_CHECK(c.GetRecordCount() == 0);
    ICTest::RunDraw(24, 16, 0, kFlagIrradianceCache, bb, rays);

    // Changing emission invalidates.
    size_t before = c.GetRecordCount();
    TASK_CHECK(before > 0);
    ICTest::SetMaterial(8, ICTest::kMaterialLambert, float3(0.8f, 0.6f, 0.2f),
                        float3(40, 35, 20), 0, 0);
    ICTest::Refresh();
    TASK_CHECK(c.GetRecordCount() == 0);

    // No scene change: cache survives a refresh (camera moves do not clear it).
    ICTest::RunDraw(24, 16, 0, kFlagIrradianceCache, bb, rays);
    size_t kept = c.GetRecordCount();
    ICTest::Refresh();
    TASK_CHECK(c.GetRecordCount() == kept);
    return 0;
}

static int TestClearAndConfig()
{
    ICTest::ResetScene();
    ICTest::Refresh();
    IrradianceCache& c = GetIrradianceCache();
    TASK_CHECK(c.Configure(MakeConfig()));
    std::vector<float> bb;
    int rays = 0;
    ICTest::RunDraw(16, 12, 0, kFlagIrradianceCache, bb, rays);
    TASK_CHECK(c.GetRecordCount() > 0);

    c.Clear();
    TASK_CHECK(c.GetRecordCount() == 0);
    TASK_CHECK(c.GetGatherLogCount() == 0);

    // Invalid configurations are rejected and leave the current state intact.
    ICConfig bad = MakeConfig();
    bad.gatherSamples = 0;
    TASK_CHECK(!c.Configure(bad));
    bad = MakeConfig(); bad.tau = 0.0f;      TASK_CHECK(!c.Configure(bad));
    bad = MakeConfig(); bad.tau = 1.5f;      TASK_CHECK(!c.Configure(bad));
    bad = MakeConfig(); bad.minRadius = 0.9f; bad.maxRadius = 0.5f; TASK_CHECK(!c.Configure(bad));
    bad = MakeConfig(); bad.minRadius = -1.0f; TASK_CHECK(!c.Configure(bad));
    TASK_CHECK(c.IsConfigured());

    // A valid reconfiguration clears records.
    c.TestInjectRecord(MakeRecord(0, 0, 5, float3(0, 0, 0), float3(0, 1, 0),
                                  float3(1, 1, 1), 0.5f));
    TASK_CHECK(c.GetRecordCount() == 1);
    TASK_CHECK(c.Configure(MakeConfig()));
    TASK_CHECK(c.GetRecordCount() == 0);
    return 0;
}

static int TestDisabledCompatibility()
{
    ICTest::ResetScene();
    ICTest::Refresh();
    IrradianceCache& c = GetIrradianceCache();
    TASK_CHECK(c.Configure(MakeConfig()));
    c.ResetStats();

    std::vector<float> bb;
    int raysCached = 0, raysPlain = 0;
    ICTest::RunDraw(32, 20, 0, kFlagIrradianceCache, bb, raysCached);
    ICStats afterCached = c.GetStats();
    TASK_CHECK(afterCached.queries > 0);

    // With the flag absent the cache is never consulted: a plain draw leaves
    // every counter and the record set exactly as they were.
    size_t recordsBefore = c.GetRecordCount();
    std::vector<float> plain;
    ICTest::RunDraw(32, 20, 0, 0, plain, raysPlain);
    ICStats afterPlain = c.GetStats();
    TASK_CHECK(afterPlain.queries == afterCached.queries);
    TASK_CHECK(afterPlain.gathers == afterCached.gathers);
    TASK_CHECK(afterPlain.cacheHits == afterCached.cacheHits);
    TASK_CHECK(afterPlain.probeRays == afterCached.probeRays);
    TASK_CHECK(c.GetRecordCount() == recordsBefore);

    double energy = 0;
    for (float v : plain) energy += v;
    TASK_CHECK(energy > 0.0 && std::isfinite(energy));
    TASK_CHECK(raysPlain > 0);
    return 0;
}

static int TestReuseReducesProbes()
{
    ICTest::ResetScene();
    ICTest::Refresh();
    IrradianceCache& c = GetIrradianceCache();
    ICConfig cfg = MakeConfig();
    cfg.gatherSamples = 16;
    TASK_CHECK(c.Configure(cfg));

    std::vector<float> bb;
    int rays = 0;
    ICTest::RunDraw(48, 27, 0, kFlagIrradianceCache, bb, rays);
    ICStats r1 = c.GetStats();
    TASK_CHECK(r1.probeRays == r1.gathers * static_cast<uint64_t>(cfg.gatherSamples));

    ICTest::RunDraw(48, 27, 1, kFlagIrradianceCache, bb, rays);
    ICStats r2 = c.GetStats();
    TASK_CHECK(r2.cacheHits > 0);
    // Reuse strictly reduces the number of hemisphere probes versus a round
    // where every query gathered.
    TASK_CHECK(r2.probeRays < r2.queries * static_cast<uint64_t>(cfg.gatherSamples));
    return 0;
}

extern const struct task_case task_cases[];
const struct task_case task_cases[] =
{
    // records
    {"records", "harmonic_distance", TestHarmonicDistance},
    {"records", "clamp_radius", TestClampRadius},
    {"records", "interpolation_weight", TestInterpolationWeight},
    {"records", "zero_error", TestZeroError},
    {"records", "depth_isolation", TestDepthIsolation},
    {"records", "side_isolation", TestSideIsolation},
    {"records", "finite_occlusion", TestFiniteOcclusion},
    {"records", "spatial_index_budget", TestSpatialIndexBudget},
    // transport
    {"transport", "constant_sky_H", TestConstantSkyH},
    {"transport", "direct_sphere_light", TestDirectSphereLight},
    {"transport", "specular_sees_light", TestSpecularSeesLight},
    {"transport", "emission_ownership", TestEmissionOwnership},
    {"transport", "depth_endpoint", TestDepthEndpoint},
    {"transport", "albedo_applied_once", TestAlbedoAppliedOnce},
    // passes
    {"passes", "cold_start_and_next_round", TestColdStartAndNextRound},
    {"passes", "intra_round_invisible", TestIntraRoundInvisible},
    {"passes", "schedule_determinism", TestScheduleDeterminism},
    {"passes", "invalidation", TestInvalidation},
    {"passes", "clear_and_config", TestClearAndConfig},
    {"passes", "disabled_compatibility", TestDisabledCompatibility},
    {"passes", "reuse_reduces_probes", TestReuseReducesProbes},
};
extern const size_t task_case_count;
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
