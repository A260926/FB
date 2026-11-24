// Irradiance cache verification. Registered through task_support/check.h;
// this file deliberately has no main(). It drives the real transport via the
// ICWorld adapter (the same integrator as the production preview) and never
// re-implements a second integrator: independent references only resample the
// real adapter rays with a test-side RNG and average.

#include "task_support/check.h"
#include "Cpp/Source/Test.h"
#include "Cpp/Source/IrradianceCache.h"
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
    bool Near(float a, float b, float tol)
    {
        return std::fabs(a - b) <= tol;
    }
    bool Near3(const float3& a, const float3& b, float tol)
    {
        return Near(a.getX(), b.getX(), tol) && Near(a.getY(), b.getY(), tol) && Near(a.getZ(), b.getZ(), tol);
    }

    uint32_t g_rng = 0xabcdef01u;
    float Rng01()
    {
        uint32_t x = g_rng;
        x ^= x << 13; x ^= x >> 17; x ^= x << 15;
        g_rng = x;
        return (x & 0xFFFFFF) / 16777216.0f;
    }
    void Frame(const float3& n, float3& u, float3& v)
    {
        float3 h = fabsf(n.getX()) > 0.1f ? float3(0, 1, 0) : float3(1, 0, 0);
        u = normalize(cross(h, n));
        v = cross(n, u);
    }
    float3 CosineDir(const float3& n)
    {
        float3 u, v; Frame(n, u, v);
        float r1 = Rng01(), r2 = Rng01();
        float phi = 2.0f * kPI * r1;
        float r2s = sqrtf(r2);
        return normalize(u * (cosf(phi) * r2s) + v * (sinf(phi) * r2s) + n * sqrtf(1.0f - r2));
    }

    ICConfig MakeConfig()
    {
        ICConfig c;
        c.enabled = true;
        c.gatherSamples = 32;
        c.tau = 0.5f;
        c.minRadius = 0.1f;
        c.maxRadius = 1.0f;
        c.seed = 0x9e3779b9u;
        return c;
    }

    ICMaterial MakeMat(ICMaterialType type, const float3& albedo, const float3& emissive = float3(0, 0, 0))
    {
        ICMaterial m;
        m.type = type;
        m.albedo = float3pack(albedo);
        m.emissive = float3pack(emissive);
        m.roughness = 0.0f;
        m.ri = 1.5f;
        return m;
    }

    // Independent high-sample reference: resamples the REAL probe transport
    // with a test-side cosine distribution and averages. No second integrator.
    float3 ReferenceGather(ICSceneAdapter& adapter, const float3& pos, const float3& n,
                           int depth, int samples)
    {
        float3 sum(0, 0, 0);
        for (int i = 0; i < samples; ++i)
        {
            float3 d = CosineDir(n);
            Ray probe(pos + n * kICMinT, d);
            int rays = 0;
            uint32_t st = 0x1234u + (uint32_t)i * 2654435761u;
            float firstT = 0.0f;
            sum += adapter.TraceProbe(probe, depth, true, rays, st, &firstT);
        }
        return sum * (1.0f / (float)samples);
    }

    // ------------------------------------------------------------------ records

    int HarmonicDistance()
    {
        // Escape-only world: every probe misses, so each d clamps to maxRadius
        // and the harmonic radius equals maxRadius exactly.
        ICWorld* w = ICWorld_Create(4);
        ICWorld_SetSky(w, 1, float3(0.2f, 0.3f, 0.4f));
        ICWorld_Finish(w);
        IrradianceCache cache;
        TASK_CHECK(cache.Configure(MakeConfig()));
        int rays = 0;
        ICPathIdentity id{0, 0, 0, 0, 0};
        ICGatherResult g = cache.Gather(*ICWorld_GetAdapter(w), float3(0, 0, 0),
                                        float3(0, 1, 0), 5, id, rays);
        TASK_CHECK(g.samples.size() == (size_t)MakeConfig().gatherSamples);
        for (const auto& s : g.samples)
            TASK_CHECK(Near(s.firstDistance, 1.0f, 1e-6f));
        TASK_CHECK(Near(g.radius, 1.0f, 1e-6f));

        // Enclosing sphere hit at ~5 in every direction: harmonic R == 5.
        ICWorld* w2 = ICWorld_Create(4);
        ICWorld_SetSphere(w2, 0, float3(0, 0, 0), 5.0f);
        ICWorld_SetMaterial(w2, 0, MakeMat(kICLambert, float3(0.5f, 0.5f, 0.5f)));
        ICWorld_SetSky(w2, 1, float3(0, 0, 0));
        ICWorld_Finish(w2);
        ICConfig c2 = MakeConfig(); c2.minRadius = 1.0f; c2.maxRadius = 8.0f;
        IrradianceCache cache2;
        TASK_CHECK(cache2.Configure(c2));
        ICGatherResult g2 = cache2.Gather(*ICWorld_GetAdapter(w2), float3(0, 0, 0),
                                          float3(0, 1, 0), 5, id, rays);
        for (const auto& s : g2.samples)
            TASK_CHECK(s.firstDistance > 4.5f && s.firstDistance < 5.5f);
        TASK_CHECK(Near(g2.radius, 5.0f, 2e-3f));

        // Small enclosing sphere (0.5) with minRadius 1 clamps R upward.
        ICWorld* w3 = ICWorld_Create(4);
        ICWorld_SetSphere(w3, 0, float3(0, 0, 0), 0.5f);
        ICWorld_SetMaterial(w3, 0, MakeMat(kICLambert, float3(0.5f, 0.5f, 0.5f)));
        ICWorld_SetSky(w3, 1, float3(0, 0, 0));
        ICWorld_Finish(w3);
        ICConfig c3 = MakeConfig(); c3.minRadius = 1.0f; c3.maxRadius = 8.0f;
        IrradianceCache cache3;
        TASK_CHECK(cache3.Configure(c3));
        ICGatherResult g3 = cache3.Gather(*ICWorld_GetAdapter(w3), float3(0, 0, 0),
                                          float3(0, 1, 0), 5, id, rays);
        TASK_CHECK(Near(g3.radius, 1.0f, 1e-6f));

        ICWorld_Release(w); ICWorld_Release(w2); ICWorld_Release(w3);
        return 0;
    }

    int InterpolationWeights()
    {
        ICWorld* w = ICWorld_Create(8);
        ICWorld_SetSphere(w, 0, float3(0, -100.5f, 0), 100.0f);
        ICWorld_SetMaterial(w, 0, MakeMat(kICLambert, float3(0.8f, 0.8f, 0.8f)));
        ICWorld_Finish(w);
        IrradianceCache cache;
        ICConfig c = MakeConfig(); c.tau = 0.5f;
        TASK_CHECK(cache.Configure(c));

        const float Rr = 1.0f;
        const float dx = 0.1f; // normal term is zero (parallel normals)
        ICRecord a{0, 0, float3pack(float3(dx, 0, 0)), float3pack(float3(0, 1, 0)),
                   5, float3pack(float3(1, 2, 4)), Rr};
        ICRecord b{0, 0, float3pack(float3(-dx, 0, 0)), float3pack(float3(0, 1, 0)),
                   5, float3pack(float3(3, 6, 2)), Rr};
        cache.DebugInjectRecord(a);
        cache.DebugInjectRecord(b);

        float e = dx / Rr; // 0.1 for both
        float wgt = 1.0f / e - 1.0f / c.tau;
        float3 expected = (a.H.toFloat3() * wgt + b.H.toFloat3() * wgt) * (1.0f / (2.0f * wgt));

        float3 got; int used = 0; int rays = 0;
        TASK_CHECK(cache.DebugQuery(*ICWorld_GetAdapter(w), 0, 0, float3(0, 0, 0),
                                    float3(0, 1, 0), 5, got, used, rays));
        TASK_CHECK(used == 2);
        TASK_CHECK(Near3(got, expected, 1e-5f));
        ICWorld_Release(w);
        return 0;
    }

    int ZeroError()
    {
        ICWorld* w = ICWorld_Create(8);
        ICWorld_SetSphere(w, 0, float3(0, -100.5f, 0), 100.0f);
        ICWorld_SetMaterial(w, 0, MakeMat(kICLambert, float3(0.8f, 0.8f, 0.8f)));
        ICWorld_Finish(w);
        IrradianceCache cache;
        TASK_CHECK(cache.Configure(MakeConfig()));

        ICRecord z{0, 0, float3pack(float3(0, 0, 0)), float3pack(float3(0, 1, 0)),
                   5, float3pack(float3(7, 7, 7)), 1.0f};
        ICRecord o{0, 0, float3pack(float3(0.05f, 0, 0)), float3pack(float3(0, 1, 0)),
                   5, float3pack(float3(1, 1, 1)), 1.0f};
        cache.DebugInjectRecord(z);
        cache.DebugInjectRecord(o);

        float3 got; int used = 0; int rays = 0;
        TASK_CHECK(cache.DebugQuery(*ICWorld_GetAdapter(w), 0, 0, float3(0, 0, 0),
                                    float3(0, 1, 0), 5, got, used, rays));
        TASK_CHECK(used == 1);                 // only the zero-error record
        TASK_CHECK(Near3(got, float3(7, 7, 7), 1e-6f));
        ICWorld_Release(w);
        return 0;
    }

    int DepthIsolation()
    {
        ICWorld* w = ICWorld_Create(8);
        ICWorld_SetSphere(w, 0, float3(0, -100.5f, 0), 100.0f);
        ICWorld_SetMaterial(w, 0, MakeMat(kICLambert, float3(0.8f, 0.8f, 0.8f)));
        ICWorld_Finish(w);
        IrradianceCache cache;
        TASK_CHECK(cache.Configure(MakeConfig()));
        for (int rem = 1; rem <= 4; ++rem)
        {
            ICRecord r{0, 0, float3pack(float3(0, 0, 0)), float3pack(float3(0, 1, 0)),
                       rem, float3pack(float3((float)rem, 0, 0)), 1.0f};
            cache.DebugInjectRecord(r);
        }
        for (int rem = 1; rem <= 4; ++rem)
        {
            float3 got; int used = 0; int rays = 0;
            TASK_CHECK(cache.DebugQuery(*ICWorld_GetAdapter(w), 0, 0, float3(0, 0, 0),
                                        float3(0, 1, 0), rem, got, used, rays));
            TASK_CHECK(used == 1);
            TASK_CHECK(Near(got.getX(), (float)rem, 1e-6f));
        }
        float3 got; int used = 0; int rays = 0;
        TASK_CHECK(!cache.DebugQuery(*ICWorld_GetAdapter(w), 0, 0, float3(0, 0, 0),
                                     float3(0, 1, 0), 9, got, used, rays));
        ICWorld_Release(w);
        return 0;
    }

    int SphereAndSide()
    {
        ICWorld* w = ICWorld_Create(8);
        ICWorld_SetSphere(w, 0, float3(0, -100.5f, 0), 100.0f);
        ICWorld_SetSphere(w, 1, float3(3, 0, -3), 0.5f);
        ICWorld_SetMaterial(w, 0, MakeMat(kICLambert, float3(0.8f, 0.8f, 0.8f)));
        ICWorld_SetMaterial(w, 1, MakeMat(kICLambert, float3(0.4f, 0.4f, 0.4f)));
        ICWorld_Finish(w);
        IrradianceCache cache;
        TASK_CHECK(cache.Configure(MakeConfig()));
        ICRecord r{0, 0, float3pack(float3(0, 0, 0)), float3pack(float3(0, 1, 0)),
                   5, float3pack(float3(1, 1, 1)), 1.0f};
        cache.DebugInjectRecord(r);

        float3 got; int used = 0; int rays = 0;
        TASK_CHECK(!cache.DebugQuery(*ICWorld_GetAdapter(w), 1, 0, float3(0, 0, 0),
                                     float3(0, 1, 0), 5, got, used, rays)); // other sphere
        TASK_CHECK(!cache.DebugQuery(*ICWorld_GetAdapter(w), 0, 1, float3(0, 0, 0),
                                     float3(0, 1, 0), 5, got, used, rays)); // other side
        TASK_CHECK(cache.DebugQuery(*ICWorld_GetAdapter(w), 0, 0, float3(0, 0, 0),
                                    float3(0, 1, 0), 5, got, used, rays));  // exact key
        ICWorld_Release(w);
        return 0;
    }

    int FiniteOcclusion()
    {
        // Two nearby points just above a large ground sphere; a third sphere
        // placed on their lifted horizontal connecting segment blocks it.
        ICWorld* w = ICWorld_Create(8);
        ICWorld_SetSphere(w, 0, float3(0, -100.5f, 0), 100.0f); // ground (owner)
        ICWorld_SetMaterial(w, 0, MakeMat(kICLambert, float3(0.8f, 0.8f, 0.8f)));
        ICWorld_Finish(w);
        IrradianceCache cache;
        TASK_CHECK(cache.Configure(MakeConfig())); // tau 0.5, R 1.0 -> e = 0.1

        float3 n(0, 1, 0);
        float3 recPos(0.0f, -0.499f, 0.0f);
        ICRecord r{0, 0, float3pack(recPos), float3pack(n), 5,
                   float3pack(float3(1, 1, 1)), 1.0f};
        cache.DebugInjectRecord(r);

        float3 qpos(0.1f, -0.499f, 0.0f);
        float3 got; int used = 0; int rays = 0;
        ICStats st0; cache.GetStats(st0);
        // Open space: the horizontal connection clears the ground -> visible.
        TASK_CHECK(cache.DebugQuery(*ICWorld_GetAdapter(w), 0, 0, qpos, n, 5, got, used, rays));
        TASK_CHECK(used == 1);
        ICStats st1; cache.GetStats(st1);
        TASK_CHECK(st1.connectionRays > st0.connectionRays); // a real connection ray was cast

        // Insert an occluder (not the owner) on the connecting segment.
        ICWorld_SetSphere(w, 1, float3(0.05f, -0.47f, 0.0f), 0.03f);
        ICWorld_SetMaterial(w, 1, MakeMat(kICLambert, float3(0.2f, 0.2f, 0.2f)));
        ICWorld_Finish(w);
        used = 0; rays = 0;
        TASK_CHECK(!cache.DebugQuery(*ICWorld_GetAdapter(w), 0, 0, qpos, n, 5, got, used, rays));
        ICWorld_Release(w);
        return 0;
    }

    int SpatialIndexBudget()
    {
        ICWorld* w = ICWorld_Create(64);
        ICWorld_SetSphere(w, 0, float3(0, -1000.5f, 0), 1000.0f);
        ICWorld_SetMaterial(w, 0, MakeMat(kICLambert, float3(0.8f, 0.8f, 0.8f)));
        ICWorld_Finish(w);
        IrradianceCache cache;
        ICConfig c = MakeConfig(); c.maxRadius = 1.0f; c.tau = 1.0f;
        TASK_CHECK(cache.Configure(c));

        const int N = 4096;
        const float spacing = 4.1f * c.maxRadius; // > 4 * maxRadius apart
        for (int i = 0; i < N; ++i)
        {
            ICRecord r{0, 0, float3pack(float3(i * spacing, 0, 0)), float3pack(float3(0, 1, 0)),
                       5, float3pack(float3(1, 1, 1)), 1.0f};
            cache.DebugInjectRecord(r);
        }
        cache.DebugRebuildIndex();
        cache.ResetStats();
        int rays = 0;
        for (int i = 0; i < N; ++i)
        {
            float3 got; int used = 0;
            TASK_CHECK(cache.DebugQuery(*ICWorld_GetAdapter(w), 0, 0, float3(i * spacing, 0, 0),
                                        float3(0, 1, 0), 5, got, used, rays));
            TASK_CHECK(used >= 1);
        }
        ICStats st; cache.GetStats(st);
        TASK_CHECK(st.recordChecks <= (uint64_t)16 * (uint64_t)N);
        TASK_CHECK(st.queries == (uint64_t)N);
        ICWorld_Release(w);
        return 0;
    }

    // --------------------------------------------------------------- transport

    int ConstantSkyDiffuse()
    {
        ICWorld* w = ICWorld_Create(4);
        const float3 sky(0.25f, 0.35f, 0.45f);
        ICWorld_SetSky(w, 1, sky);
        ICWorld_Finish(w);
        IrradianceCache cache;
        TASK_CHECK(cache.Configure(MakeConfig()));
        int rays = 0;
        ICPathIdentity id{0, 0, 0, 0, 0};
        ICGatherResult g = cache.Gather(*ICWorld_GetAdapter(w), float3(0, 0, 0),
                                        float3(0, 1, 0), 5, id, rays);
        // Every probe escapes to the constant sky: H is analytically the sky
        // color, and carries no local albedo.
        TASK_CHECK(Near3(g.H.toFloat3(), sky, 1e-6f));
        TASK_CHECK(rays == MakeConfig().gatherSamples);
        ICWorld_Release(w);
        return 0;
    }

    int DirectSphereLight()
    {
        ICWorld* w = ICWorld_Create(8);
        ICWorld_SetSky(w, 1, float3(0, 0, 0));
        // Diffuse receiver sphere; shading point just above its north pole.
        ICWorld_SetSphere(w, 0, float3(0, -1.0f, 0), 1.0f);
        ICWorld_SetMaterial(w, 0, MakeMat(kICLambert, float3(0.7f, 0.7f, 0.7f)));
        // Large, close emissive sphere covering much of the upper hemisphere.
        ICWorld_SetSphere(w, 1, float3(0, 3.0f, 0), 1.2f);
        ICWorld_SetMaterial(w, 1, MakeMat(kICLambert, float3(0, 0, 0), float3(4, 5, 6)));
        ICWorld_Finish(w);

        IrradianceCache cache;
        TASK_CHECK(cache.Configure(MakeConfig()));
        float3 pos(0, 0.001f, 0), n(0, 1, 0);
        int rays = 0;
        ICPathIdentity id{0, 0, 0, 0, 0};
        ICGatherResult g = cache.Gather(*ICWorld_GetAdapter(w), pos, n, 5, id, rays);

        // A probe whose first segment hits the light must contribute exactly
        // zero: direct first-hit self-emission is stripped.
        ICSceneAdapter& ad = *ICWorld_GetAdapter(w);
        bool sawDirectLightProbe = false;
        for (const auto& s : g.samples)
        {
            Ray pr(pos + n * kICMinT, s.direction.toFloat3());
            Hit hit; int hid;
            if (ad.HitWorld(pr, kICMinT, kICMaxT, hit, hid) && hid == 1)
            {
                sawDirectLightProbe = true;
                TASK_CHECK(Near3(s.contribution.toFloat3(), float3(0, 0, 0), 1e-7f));
            }
        }
        TASK_CHECK(sawDirectLightProbe);

        // With a black sky every probe either meets the light directly
        // (stripped) or escapes to black: H must be zero, i.e. the direct
        // sphere light is never baked into the cached hemispherical term.
        TASK_CHECK(Near3(g.H.toFloat3(), float3(0, 0, 0), 1e-7f));

        // H must not contain the receiver albedo: changing only the receiver
        // material leaves every probe radiance unchanged.
        ICWorld_SetMaterial(w, 0, MakeMat(kICLambert, float3(0.11f, 0.22f, 0.33f)));
        ICWorld_Finish(w);
        ICGatherResult g2 = cache.Gather(*ICWorld_GetAdapter(w), pos, n, 5, id, rays);
        TASK_CHECK(Near3(g.H.toFloat3(), g2.H.toFloat3(), 1e-6f));

        // The explicit direct estimate still happens exactly once at the hit.
        Hit rec; rec.pos = pos; rec.normal = n; rec.t = 1.0f;
        Ray rin(pos, float3(0, -1, 0));
        uint32_t st = 0x777u; int lr = 0;
        float3 direct = ICWorld_SampleLights(w, rec, rin, 0, float3(0.7f, 0.7f, 0.7f), lr, st);
        TASK_CHECK(direct.getY() > 0.0f);
        TASK_CHECK(lr == 1);

        // Indirect diffuse: an enclosing Lambert wall around the same light.
        // Probes hit the wall; after that one real scatter the wall's explicit
        // light sampling sees the lamp, so H is positive and matches the
        // independent high-sample reference over the same transport.
        ICWorld* wi = ICWorld_Create(8);
        ICWorld_SetSky(wi, 1, float3(0, 0, 0));
        ICWorld_SetSphere(wi, 0, float3(0, 0, 0), 8.0f);
        ICWorld_SetMaterial(wi, 0, MakeMat(kICLambert, float3(0.6f, 0.6f, 0.6f)));
        ICWorld_SetSphere(wi, 1, float3(0, 3.0f, 0), 1.0f);
        ICWorld_SetMaterial(wi, 1, MakeMat(kICLambert, float3(0, 0, 0), float3(3, 4, 5)));
        ICWorld_Finish(wi);
        ICGatherResult gi = cache.Gather(*ICWorld_GetAdapter(wi), float3(0, 0, 0),
                                         float3(0, 1, 0), 5, id, rays);
        TASK_CHECK(gi.H.toFloat3().getY() > 0.0f);
        g_rng = 0x55aa55aau;
        float3 ref = ReferenceGather(*ICWorld_GetAdapter(wi), float3(0, 0, 0),
                                     float3(0, 1, 0), 1, 4096);
        TASK_CHECK(Near3(gi.H.toFloat3(), ref, 0.10f));

        ICWorld_Release(w);
        ICWorld_Release(wi);
        return 0;
    }

    int LightAfterSpecular()
    {
        ICWorld* w = ICWorld_Create(8);
        ICWorld_SetSky(w, 1, float3(0, 0, 0));
        // Metal at (0,0,-3) radius 1; a -z ray reflects straight back to +z.
        ICWorld_SetSphere(w, 0, float3(0, 0, -3.0f), 1.0f);
        ICWorld_SetMaterial(w, 0, MakeMat(kICMetal, float3(0.7f, 0.8f, 0.9f)));
        // Emissive sphere on the +z reflection line.
        ICWorld_SetSphere(w, 1, float3(0, 0, 5.0f), 0.5f);
        ICWorld_SetMaterial(w, 1, MakeMat(kICLambert, float3(0, 0, 0), float3(2, 3, 4)));
        ICWorld_Finish(w);

        ICSceneAdapter& ad = *ICWorld_GetAdapter(w);
        uint32_t st = 0x42u; int rays = 0; float firstT = 0.0f;

        // Control: a direct first-segment hit on the light is stripped.
        Ray direct(float3(0, 0, 0), float3(0, 0, 1));
        float3 d = ad.TraceProbe(direct, 1, true, rays, st, &firstT);
        TASK_CHECK(Near3(d, float3(0, 0, 0), 1e-7f));

        // After the specular event the same light is kept: albedo * emission.
        Ray viaMetal(float3(0, 0, 0), float3(0, 0, -1));
        float3 kept = ad.TraceProbe(viaMetal, 1, true, rays, st, &firstT);
        TASK_CHECK(Near3(kept, float3(1.4f, 2.4f, 3.6f), 1e-4f));
        ICWorld_Release(w);
        return 0;
    }

    int DepthEndpoint()
    {
        ICWorld* w = ICWorld_Create(8);
        ICWorld_SetSky(w, 1, float3(0, 0, 0));
        ICWorld_SetSphere(w, 0, float3(0, 0, -3.0f), 1.0f);
        ICWorld_SetMaterial(w, 0, MakeMat(kICLambert, float3(0.6f, 0.6f, 0.6f)));
        ICWorld_SetSphere(w, 1, float3(0, 0, 6.0f), 0.5f);
        ICWorld_SetMaterial(w, 1, MakeMat(kICLambert, float3(0, 0, 0), float3(2, 3, 4)));
        ICWorld_Finish(w);

        ICSceneAdapter& ad = *ICWorld_GetAdapter(w);
        uint32_t st = 0x99u; float firstT = 0.0f;

        // At the depth endpoint (depth == kMaxDepth) a non-emissive diffuse
        // hit returns black and casts no scatter ray: the path is not extended.
        int rays = 0;
        Ray toDiffuse(float3(0, 0, 0), float3(0, 0, -1));
        float3 end = ad.TraceProbe(toDiffuse, kICMaxDepth, true, rays, st, &firstT);
        TASK_CHECK(Near3(end, float3(0, 0, 0), 1e-7f));
        TASK_CHECK(rays == 1);

        // The same emission-attribution rule applies at the endpoint: a direct
        // first-segment hit on the lamp is stripped so its light is not added
        // twice, and again only the single intersection ray is cast.
        int rays2 = 0;
        Ray toLight(float3(0, 0, 0), float3(0, 0, 1));
        float3 stripped = ad.TraceProbe(toLight, kICMaxDepth, true, rays2, st, &firstT);
        TASK_CHECK(Near3(stripped, float3(0, 0, 0), 1e-7f));
        TASK_CHECK(rays2 == 1);

        // One step above the endpoint exactly one real scatter remains; the
        // following hop is the terminal one. Rays are: primary intersection,
        // the Lambert explicit-light shadow ray, and one terminal continuation
        // ray -- never anything beyond the depth budget.
        int rays3 = 0;
        float3 oneAbove = ad.TraceProbe(toDiffuse, kICMaxDepth - 1, true, rays3, st, &firstT);
        TASK_CHECK(std::isfinite(oneAbove.getX()));
        TASK_CHECK(rays3 == 3);
        ICWorld_Release(w);
        return 0;
    }

    // ------------------------------------------------------------------ passes

    struct RoundCtx
    {
        IrradianceCache cache;
        ICWorld* world;
        float3 pos, n;
    };

    int ColdStartAndInRound()
    {
        ICWorld* w = ICWorld_Create(8);
        ICWorld_SetSky(w, 1, float3(0.2f, 0.2f, 0.2f));
        ICWorld_SetSphere(w, 0, float3(0, -100.5f, 0), 100.0f);
        ICWorld_SetMaterial(w, 0, MakeMat(kICLambert, float3(0.8f, 0.8f, 0.8f)));
        ICWorld_Finish(w);

        IrradianceCache cache;
        TASK_CHECK(cache.Configure(MakeConfig()));
        cache.BeginDraw(0, 8, ICWorld_GetAdapter(w)->SceneSignature());
        int rays = 0;
        ICPathIdentity id{0, 3, 2, 0, 0};
        float3 H = cache.LookupOrGather(*ICWorld_GetAdapter(w), 0, 0,
                                        float3(0, 0, 0), float3(0, 1, 0), 5, id, rays);
        TASK_CHECK(H.getY() > 0.0f);
        ICStats s1; cache.GetStats(s1);
        TASK_CHECK(s1.gathers == 1 && s1.hits == 0);
        TASK_CHECK(cache.GetRecordCount() == 0); // candidate not readable this round
        TASK_CHECK(s1.candidates == 1);

        // A second lookup at the same point still cannot read the in-round
        // candidate, so it gathers again.
        ICPathIdentity id2{0, 3, 2, 1, 0};
        cache.LookupOrGather(*ICWorld_GetAdapter(w), 0, 0, float3(0, 0, 0),
                             float3(0, 1, 0), 5, id2, rays);
        ICStats s2; cache.GetStats(s2);
        TASK_CHECK(s2.gathers == 2 && s2.hits == 0);
        ICWorld_Release(w);
        return 0;
    }

    int NextRoundHit()
    {
        ICWorld* w = ICWorld_Create(8);
        ICWorld_SetSky(w, 1, float3(0.2f, 0.2f, 0.2f));
        ICWorld_SetSphere(w, 0, float3(0, -100.5f, 0), 100.0f);
        ICWorld_SetMaterial(w, 0, MakeMat(kICLambert, float3(0.8f, 0.8f, 0.8f)));
        ICWorld_Finish(w);

        IrradianceCache cache;
        TASK_CHECK(cache.Configure(MakeConfig()));
        uint64_t sig = ICWorld_GetAdapter(w)->SceneSignature();
        cache.BeginDraw(0, 8, sig);
        int rays = 0;
        ICPathIdentity id{0, 3, 2, 0, 0};
        float3 first = cache.LookupOrGather(*ICWorld_GetAdapter(w), 0, 0,
                                            float3(0, 0, 0), float3(0, 1, 0), 5, id, rays);
        cache.EndDraw();
        cache.BeginDraw(1, 8, sig); // publishes the candidate
        TASK_CHECK(cache.GetRecordCount() == 1);

        ICPathIdentity idb{1, 3, 2, 0, 0};
        float3 second = cache.LookupOrGather(*ICWorld_GetAdapter(w), 0, 0,
                                             float3(0.01f, 0, 0), float3(0, 1, 0), 5, idb, rays);
        ICStats st; cache.GetStats(st);
        TASK_CHECK(st.hits == 1 && st.gathers == 1);
        TASK_CHECK(Near3(first, second, 1e-5f));
        ICWorld_Release(w);
        return 0;
    }

    int InvalidationAndCamera()
    {
        ICWorld* w = ICWorld_Create(8);
        ICWorld_SetSky(w, 1, float3(0.2f, 0.2f, 0.2f));
        ICWorld_SetSphere(w, 0, float3(0, -100.5f, 0), 100.0f);
        ICWorld_SetMaterial(w, 0, MakeMat(kICLambert, float3(0.8f, 0.8f, 0.8f)));
        ICWorld_Finish(w);

        IrradianceCache cache;
        TASK_CHECK(cache.Configure(MakeConfig()));
        uint64_t sig0 = ICWorld_GetAdapter(w)->SceneSignature();
        cache.BeginDraw(0, 8, sig0);
        int rays = 0;
        ICPathIdentity id{0, 0, 0, 0, 0};
        cache.LookupOrGather(*ICWorld_GetAdapter(w), 0, 0, float3(0, 0, 0),
                             float3(0, 1, 0), 5, id, rays);
        cache.BeginDraw(1, 8, sig0); // camera moves, scene unchanged: keep
        TASK_CHECK(cache.GetRecordCount() == 1);
        uint64_t sigRebuild = ICWorld_GetAdapter(w)->SceneSignature();
        TASK_CHECK(sigRebuild == sig0);

        // Move a sphere: the signature changes and the next draw invalidates.
        ICWorld_SetSphere(w, 0, float3(0, -100.6f, 0), 100.0f);
        ICWorld_Finish(w);
        uint64_t sig1 = ICWorld_GetAdapter(w)->SceneSignature();
        TASK_CHECK(sig1 != sig0);
        cache.BeginDraw(2, 8, sig1);
        TASK_CHECK(cache.GetRecordCount() == 0);

        // An emissive change invalidates as well.
        cache.LookupOrGather(*ICWorld_GetAdapter(w), 0, 0, float3(0, 0, 0),
                             float3(0, 1, 0), 5, id, rays);
        cache.BeginDraw(3, 8, sig1);
        TASK_CHECK(cache.GetRecordCount() == 1);
        ICWorld_SetMaterial(w, 0, MakeMat(kICLambert, float3(0.8f, 0.8f, 0.8f), float3(1, 1, 1)));
        ICWorld_Finish(w);
        cache.BeginDraw(4, 8, ICWorld_GetAdapter(w)->SceneSignature());
        TASK_CHECK(cache.GetRecordCount() == 0);
        ICWorld_Release(w);
        return 0;
    }

    int ClearAndInvalidConfig()
    {
        ICWorld* w = ICWorld_Create(4);
        ICWorld_SetSky(w, 1, float3(0.2f, 0.2f, 0.2f));
        ICWorld_Finish(w);
        IrradianceCache cache;
        TASK_CHECK(cache.Configure(MakeConfig()));
        cache.BeginDraw(0, 4, ICWorld_GetAdapter(w)->SceneSignature());
        int rays = 0;
        ICPathIdentity id{0, 0, 0, 0, 0};
        cache.LookupOrGather(*ICWorld_GetAdapter(w), 0, 0, float3(0, 0, 0),
                             float3(0, 1, 0), 5, id, rays);
        cache.BeginDraw(1, 4, ICWorld_GetAdapter(w)->SceneSignature());
        TASK_CHECK(cache.GetRecordCount() == 1);
        cache.Clear();
        TASK_CHECK(cache.GetRecordCount() == 0);

        // Invalid configurations are rejected and leave the current state.
        ICConfig bad = MakeConfig(); bad.gatherSamples = 0;
        TASK_CHECK(!cache.Configure(bad));
        TASK_CHECK(cache.IsEnabled());
        ICConfig bad2 = MakeConfig(); bad2.tau = 1.5f;
        TASK_CHECK(!cache.Configure(bad2));
        ICConfig bad3 = MakeConfig(); bad3.minRadius = 2.0f; bad3.maxRadius = 1.0f;
        TASK_CHECK(!cache.Configure(bad3));
        ICConfig bad4 = MakeConfig(); bad4.tau = std::nanf("");
        TASK_CHECK(!cache.Configure(bad4));

        // Disabling is accepted and clears.
        ICConfig off;
        TASK_CHECK(cache.Configure(off));
        TASK_CHECK(!cache.IsEnabled());
        ICWorld_Release(w);
        return 0;
    }

    int ReuseReducesGathers()
    {
        ICWorld* w = ICWorld_Create(8);
        ICWorld_SetSky(w, 1, float3(0.2f, 0.2f, 0.2f));
        ICWorld_SetSphere(w, 0, float3(0, -100.5f, 0), 100.0f);
        ICWorld_SetMaterial(w, 0, MakeMat(kICLambert, float3(0.8f, 0.8f, 0.8f)));
        ICWorld_Finish(w);
        IrradianceCache cache;
        ICConfig c = MakeConfig(); c.gatherSamples = 16;
        TASK_CHECK(cache.Configure(c));
        uint64_t sig = ICWorld_GetAdapter(w)->SceneSignature();
        cache.BeginDraw(0, 16, sig);
        int rays = 0;
        ICPathIdentity seed{0, 0, 0, 0, 0};
        // Cold round: every lookup gathers.
        for (int i = 0; i < 12; ++i)
        {
            ICPathIdentity id = seed; id.pixelX = i;
            cache.LookupOrGather(*ICWorld_GetAdapter(w), 0, 0, float3(i * 0.002f, 0, 0),
                                 float3(0, 1, 0), 5, id, rays);
        }
        ICStats cold; cache.GetStats(cold);
        TASK_CHECK(cold.gathers == 12 && cold.hits == 0);
        TASK_CHECK(cold.probeRays == (uint64_t)12 * 16);

        cache.BeginDraw(1, 16, sig);
        for (int i = 0; i < 12; ++i)
        {
            ICPathIdentity id = seed; id.frame = 1; id.pixelX = i;
            // Slightly offset query points so each reuse runs a real (short)
            // visibility connection ray; still well within tau.
            cache.LookupOrGather(*ICWorld_GetAdapter(w), 0, 0, float3(i * 0.002f + 0.0003f, 0, 0),
                                 float3(0, 1, 0), 5, id, rays);
        }
        ICStats warm; cache.GetStats(warm);
        TASK_CHECK(warm.hits == 12);            // all reused
        TASK_CHECK(warm.gathers == 12);         // no new gathers in round two
        TASK_CHECK(warm.connectionRays > 0);    // visibility was really checked
        ICWorld_Release(w);
        return 0;
    }

    int ThreeSchedulesAndDisabled()
    {
        const int wdt = 24, hgt = 16;
        auto render = [&](int mode, unsigned flags, bool enableCache, std::vector<float>& buf) -> int {
            buf.assign((size_t)wdt * hgt * 4, 0.0f);
            InitializeTest();
            SetRowScheduleMode(mode);
            UpdateTest(0.0f, 0, wdt, hgt, kFlagProgressive);
            ClearIrradianceCache();
            if (enableCache)
                TASK_CHECK(ConfigureIrradianceCache(MakeConfig()));
            else
            {
                ICConfig off;
                TASK_CHECK(ConfigureIrradianceCache(off));
                TASK_CHECK(!GetIrradianceCache().IsEnabled());
            }
            int rays = 0;
            for (int f = 0; f < 2; ++f)
            {
                UpdateTest(0.0f, f, wdt, hgt, kFlagProgressive);
                DrawTest(0.0f, f, wdt, hgt, buf.data(), rays, flags);
            }
            ShutdownTest();
            return 0;
        };

        std::vector<float> enki, fwd, rev, offBuf;
        const unsigned cacheFlags = kFlagProgressive | kFlagIrradianceCache;
        TASK_CHECK(render(0, cacheFlags, true, enki) == 0);
        TASK_CHECK(render(1, cacheFlags, true, fwd) == 0);
        TASK_CHECK(render(2, cacheFlags, true, rev) == 0);
        TASK_CHECK(enki.size() == fwd.size() && fwd.size() == rev.size());
        TASK_CHECK(std::memcmp(enki.data(), fwd.data(), enki.size() * sizeof(float)) == 0);
        TASK_CHECK(std::memcmp(fwd.data(), rev.data(), rev.size() * sizeof(float)) == 0);

        // Disabled cache: even with the flag set, a disabled config keeps the
        // production path and its original image.
        TASK_CHECK(render(0, cacheFlags, false, offBuf) == 0);
        std::vector<float> base;
        TASK_CHECK(render(0, kFlagProgressive, false, base) == 0);
        TASK_CHECK(std::memcmp(offBuf.data(), base.data(), base.size() * sizeof(float)) == 0);
        return 0;
    }
}

const struct task_case task_cases[] =
{
    {"records", "harmonic_distance", HarmonicDistance},
    {"records", "interpolation_weights", InterpolationWeights},
    {"records", "zero_error", ZeroError},
    {"records", "depth_isolation", DepthIsolation},
    {"records", "sphere_and_side", SphereAndSide},
    {"records", "finite_occlusion", FiniteOcclusion},
    {"records", "spatial_index_budget", SpatialIndexBudget},
    {"transport", "constant_sky_diffuse", ConstantSkyDiffuse},
    {"transport", "direct_sphere_light", DirectSphereLight},
    {"transport", "light_after_specular", LightAfterSpecular},
    {"transport", "depth_endpoint", DepthEndpoint},
    {"passes", "cold_start_and_in_round", ColdStartAndInRound},
    {"passes", "next_round_hit", NextRoundHit},
    {"passes", "invalidation_and_camera", InvalidationAndCamera},
    {"passes", "clear_and_invalid_config", ClearAndInvalidConfig},
    {"passes", "reuse_reduces_gathers", ReuseReducesGathers},
    {"passes", "three_schedules_and_disabled", ThreeSchedulesAndDisabled},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
