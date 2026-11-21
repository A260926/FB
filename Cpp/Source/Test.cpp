#include "Config.h"
#include "Test.h"
#include "Maths.h"
#include "IrradianceCache.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#if CPU_CAN_DO_THREADS
#include "enkiTS/TaskScheduler_c.h"
#endif
#include <atomic>

// 46 spheres (2 emissive) when enabled; 9 spheres (1 emissive) when disabled
#define DO_BIG_SCENE 1

// The test adapter may append spheres; keep fixed-capacity working arrays.
static const int kMaxSpheres = 128;

static Sphere s_Spheres[kMaxSpheres];

struct Material
{
    enum Type { Lambert, Metal, Dielectric };
    Type type;
    float3pack albedo;
    float3pack emissive;
    float roughness;
    float ri;
};

static Material s_SphereMats[kMaxSpheres];

const int kInitialSphereCount = DO_BIG_SCENE ? 46 : 9;
// GetSceneDesc keeps reporting the original compile-time scene layout.
const int kSphereCount = kInitialSphereCount;
static int s_SphereCount = kInitialSphereCount;

static SpheresSoA s_SpheresSoA(kMaxSpheres);

// Compile-time default scene, kept as tables so the test adapter can restore
// it exactly (object and material layout unchanged in disabled mode).
struct DefaultSphere { float3 center; float radius; };
struct DefaultMaterial { int type; float3 albedo; float3 emissive; float roughness; float ri; };

static const DefaultSphere kDefaultSpheres[] =
{
    {float3(0,-100.5,-1), 100},
    {float3(2,0,-1), 0.5f},
    {float3(0,0,-1), 0.5f},
    {float3(-2,0,-1), 0.5f},
    {float3(2,0,1), 0.5f},
    {float3(0,0,1), 0.5f},
    {float3(-2,0,1), 0.5f},
    {float3(0.5f,1,0.5f), 0.5f},
    {float3(-1.5f,1.5f,0.f), 0.3f},
#if DO_BIG_SCENE
    {float3(4,0,-3), 0.5f}, {float3(3,0,-3), 0.5f}, {float3(2,0,-3), 0.5f}, {float3(1,0,-3), 0.5f}, {float3(0,0,-3), 0.5f}, {float3(-1,0,-3), 0.5f}, {float3(-2,0,-3), 0.5f}, {float3(-3,0,-3), 0.5f}, {float3(-4,0,-3), 0.5f},
    {float3(4,0,-4), 0.5f}, {float3(3,0,-4), 0.5f}, {float3(2,0,-4), 0.5f}, {float3(1,0,-4), 0.5f}, {float3(0,0,-4), 0.5f}, {float3(-1,0,-4), 0.5f}, {float3(-2,0,-4), 0.5f}, {float3(-3,0,-4), 0.5f}, {float3(-4,0,-4), 0.5f},
    {float3(4,0,-5), 0.5f}, {float3(3,0,-5), 0.5f}, {float3(2,0,-5), 0.5f}, {float3(1,0,-5), 0.5f}, {float3(0,0,-5), 0.5f}, {float3(-1,0,-5), 0.5f}, {float3(-2,0,-5), 0.5f}, {float3(-3,0,-5), 0.5f}, {float3(-4,0,-5), 0.5f},
    {float3(4,0,-6), 0.5f}, {float3(3,0,-6), 0.5f}, {float3(2,0,-6), 0.5f}, {float3(1,0,-6), 0.5f}, {float3(0,0,-6), 0.5f}, {float3(-1,0,-6), 0.5f}, {float3(-2,0,-6), 0.5f}, {float3(-3,0,-6), 0.5f}, {float3(-4,0,-6), 0.5f},
    {float3(1.5f,1.5f,-2), 0.3f},
#endif // #if DO_BIG_SCENE
};

static const DefaultMaterial kDefaultMaterials[] =
{
    { Material::Lambert, float3(0.8f, 0.8f, 0.8f), float3(0,0,0), 0, 0, },
    { Material::Lambert, float3(0.8f, 0.4f, 0.4f), float3(0,0,0), 0, 0, },
    { Material::Lambert, float3(0.4f, 0.8f, 0.4f), float3(0,0,0), 0, 0, },
    { Material::Metal, float3(0.4f, 0.4f, 0.8f), float3(0,0,0), 0, 0 },
    { Material::Metal, float3(0.4f, 0.8f, 0.4f), float3(0,0,0), 0, 0 },
    { Material::Metal, float3(0.4f, 0.8f, 0.4f), float3(0,0,0), 0.2f, 0 },
    { Material::Metal, float3(0.4f, 0.8f, 0.4f), float3(0,0,0), 0.6f, 0 },
    { Material::Dielectric, float3(0.4f, 0.4f, 0.4f), float3(0,0,0), 0, 1.5f },
    { Material::Lambert, float3(0.8f, 0.6f, 0.2f), float3(30,25,15), 0, 0 },
#if DO_BIG_SCENE
    { Material::Lambert, float3(0.1f, 0.1f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.2f, 0.2f, 0.2f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.3f, 0.3f, 0.3f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.4f, 0.4f, 0.4f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.5f, 0.5f, 0.5f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.6f, 0.6f, 0.6f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.7f, 0.7f, 0.7f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.8f, 0.8f, 0.8f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.9f, 0.9f, 0.9f), float3(0,0,0), 0, 0, },
    { Material::Metal, float3(0.1f, 0.1f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.2f, 0.2f, 0.2f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.3f, 0.3f, 0.3f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.4f, 0.4f, 0.4f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.5f, 0.5f, 0.5f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.6f, 0.6f, 0.6f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.7f, 0.7f, 0.7f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.8f, 0.8f, 0.8f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.9f, 0.9f, 0.9f), float3(0,0,0), 0, 0, },
    { Material::Metal, float3(0.8f, 0.1f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.8f, 0.5f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.8f, 0.8f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.4f, 0.8f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.1f, 0.8f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.1f, 0.8f, 0.5f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.1f, 0.8f, 0.8f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.1f, 0.1f, 0.8f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.5f, 0.1f, 0.8f), float3(0,0,0), 0, 0, },
    { Material::Lambert, float3(0.8f, 0.1f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.8f, 0.5f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.8f, 0.8f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.4f, 0.8f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.1f, 0.8f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.1f, 0.8f, 0.5f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.1f, 0.8f, 0.8f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.1f, 0.1f, 0.8f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.5f, 0.1f, 0.8f), float3(0,0,0), 0, 0, },
    { Material::Lambert, float3(0.1f, 0.2f, 0.5f), float3(3,10,20), 0, 0 },
#endif
};

static void LoadDefaultScene()
{
    for (int i = 0; i < kInitialSphereCount; ++i)
    {
        s_Spheres[i] = Sphere(kDefaultSpheres[i].center, kDefaultSpheres[i].radius);
        const DefaultMaterial& d = kDefaultMaterials[i];
        s_SphereMats[i] = Material{ static_cast<Material::Type>(d.type),
                                    float3pack(d.albedo), float3pack(d.emissive),
                                    d.roughness, d.ri };
    }
    s_SphereCount = kInitialSphereCount;
}

static int s_EmissiveSpheres[kMaxSpheres];
static int s_EmissiveSphereCount;

static Camera s_Cam;

const float kMaxT = 1.0e7f;
const int kMaxDepth = 10;

// Sky selection. Mode 0 is the production gradient and keeps the disabled
// cache output bit-identical; mode 1 is a constant environment for tests.
static int s_SkyMode = 0;
static float3 s_SkyConstant(0.15f, 0.21f, 0.3f);

static float3 SkyRadiance(const Ray& r)
{
#if DO_MITSUBA_COMPARE
    return float3(0.15f,0.21f,0.3f);
#else
    if (s_SkyMode == 1)
        return s_SkyConstant;
    float3 unitDir = r.dir;
    float t = 0.5f*(unitDir.getY() + 1.0f);
    return ((1.0f-t)*float3(1.0f, 1.0f, 1.0f) + t*float3(0.5f, 0.7f, 1.0f)) * 0.3f;
#endif
}

bool HitWorld(const Ray& r, float tMin, float tMax, Hit& outHit, int& outID)
{
    outID = HitSpheres(r, s_SpheresSoA, tMin, tMax, outHit);
    return outID != -1;
}

// Explicit light sampling for a Lambert hit. Split out of Scatter so that the
// cache path can reuse the exact production direct-light estimate without
// generating a cosine scatter ray.
static float3 LambertLightE(const Material& mat, const Hit& rec, const Ray& r_in,
                            int& inoutRayCount, uint32_t& state)
{
    float3 lightE(0, 0, 0);
#if DO_LIGHT_SAMPLING
    float3 matAlbedo = mat.albedo.toFloat3();
    for (int j = 0; j < s_EmissiveSphereCount; ++j)
    {
        int i = s_EmissiveSpheres[j];
        const Material& smat = s_SphereMats[i];
        if (&mat == &smat)
            continue; // skip self
        const Sphere& s = s_Spheres[i];

        // create a random direction towards sphere
        // coord system for sampling: sw, su, sv
        float3 sc = s.center.toFloat3();
        float3 sw = normalize(sc - rec.pos);
        float3 su = normalize(cross(fabs(sw.getX())>0.01f ? float3(0,1,0):float3(1,0,0), sw));
        float3 sv = cross(sw, su);
        // sample sphere by solid angle
        float cosAMax = sqrtf(1.0f - s.radius*s.radius / sqLength(rec.pos-sc));
        float eps1 = RandomFloat01(state), eps2 = RandomFloat01(state);
        float cosA = 1.0f - eps1 + eps1 * cosAMax;
        float sinA = sqrtf(1.0f - cosA*cosA);
        float phi = 2 * kPI * eps2;
        float3 l = su * (cosf(phi) * sinA) + sv * (sinf(phi) * sinA) + sw * cosA;

        // shoot shadow ray
        Hit lightHit;
        int hitID;
        ++inoutRayCount;
        if (HitWorld(Ray(rec.pos, l), kMinT, kMaxT, lightHit, hitID) && hitID == i)
        {
            float omega = 2 * kPI * (1-cosAMax);

            float3 rdir = r_in.dir;
            AssertUnit(rdir);
            float3 nl = dot(rec.normal, rdir) < 0 ? rec.normal : -rec.normal;
            float3 smatEmissive = smat.emissive.toFloat3();
            lightE += (matAlbedo * smatEmissive) * (std::max(0.0f, dot(l, nl)) * omega / kPI);
        }
    }
#else
    (void)mat; (void)r_in;
#endif
    return lightE;
}

static bool Scatter(const Material& mat, const Ray& r_in, const Hit& rec, float3& attenuation, Ray& scattered, float3& outLightE, int& inoutRayCount, uint32_t& state)
{
    // Normalize a randomly built direction. For every valid vector this
    // computes exactly normalize(v), so the production path keeps identical
    // results; only a degenerate draw (random vector almost cancelling the
    // normal/reflection, making 1/|v| numerically unreliable) is redirected to
    // a guaranteed-unit fallback instead of a non-unit Ray.
    auto safeNormalize = [](const float3& v, const float3& fallback) -> float3
    {
        auto unit = [](const float3& w) -> float3
        {
            float l2 = sqLength(w);
            if (!std::isfinite(l2) || l2 <= 1e-12f)
                return float3(0, 1, 0); // canonical, guaranteed unit
            return w * (1.0f / sqrtf(l2));
        };
        float l2 = sqLength(v);
        if (!std::isfinite(l2) || l2 <= 1e-12f)
            return unit(fallback);
        float3 n = v * (1.0f / sqrtf(l2)); // same expression as normalize(v)
        if (fabsf(sqLength(n) - 1.0f) >= 0.01f)
            return unit(fallback);
        return n;
    };

    outLightE = float3(0,0,0);
    if (mat.type == Material::Lambert)
    {
        // random point on unit sphere that is tangent to the hit point
        float3 target = rec.pos + rec.normal + RandomUnitVector(state);
        float3 scatterDir = safeNormalize(target - rec.pos, rec.normal);
        scattered = Ray(rec.pos, scatterDir);
        float3 matAlbedo = mat.albedo.toFloat3();
        attenuation = matAlbedo;

        // sample lights
        outLightE = LambertLightE(mat, rec, r_in, inoutRayCount, state);
        return true;
    }
    else if (mat.type == Material::Metal)
    {
        AssertUnit(r_in.dir); AssertUnit(rec.normal);
        float3 refl = reflect(r_in.dir, rec.normal);
        // reflected ray, and random inside of sphere based on roughness
        float roughness = mat.roughness;
#if DO_MITSUBA_COMPARE
        roughness = 0; // until we get better BRDF for metals
#endif
        float3 scatterDir = safeNormalize(refl + roughness*RandomInUnitSphere(state), refl);
        scattered = Ray(rec.pos, scatterDir);
        float3 matAlbedo = mat.albedo.toFloat3();
        attenuation = matAlbedo;
        return dot(scattered.dir, rec.normal) > 0;
    }
    else if (mat.type == Material::Dielectric)
    {
        AssertUnit(r_in.dir); AssertUnit(rec.normal);
        float3 outwardN;
        float3 rdir = r_in.dir;
        float3 refl = reflect(rdir, rec.normal);
        float nint;
        attenuation = float3(1,1,1);
        float3 refr;
        float reflProb;
        float cosine;
        if (dot(rdir, rec.normal) > 0)
        {
            outwardN = -rec.normal;
            nint = mat.ri;
            cosine = mat.ri * dot(rdir, rec.normal);
        }
        else
        {
            outwardN = rec.normal;
            nint = 1.0f / mat.ri;
            cosine = -dot(rdir, rec.normal);
        }
        if (refract(rdir, outwardN, nint, refr))
        {
            reflProb = schlick(cosine, mat.ri);
        }
        else
        {
            reflProb = 1;
        }
        if (RandomFloat01(state) < reflProb)
            scattered = Ray(rec.pos, safeNormalize(refl, outwardN));
        else
            scattered = Ray(rec.pos, safeNormalize(refr, outwardN));
    }
    else
    {
        attenuation = float3(1,0,1);
        return false;
    }
    return true;
}

// Per-path context. Null (or useCache false) selects the exact production
// Trace behaviour.
struct TraceCtx
{
    bool useCache = false;
    uint32_t frame = 0;
    uint32_t pixel = 0;
    uint32_t sample = 0;
    std::vector<ICCandidate>* candidates = nullptr;
};

// Defined below; Trace only needs the bound scene services up front.
static float3 ProbeTrace(const Ray& r, int depth, int limit, bool doMaterialE,
                         int& inoutRayCount, uint32_t& state,
                         bool suppressFirstEmission, ICProbeInfo& info);
namespace ICTest { const ICScene& Scene(); }

static float3 Trace(const Ray& r, int depth, int& inoutRayCount, uint32_t& state,
                    const TraceCtx* ctx, bool doMaterialE = true)
{
    Hit rec;
    int id = 0;
    ++inoutRayCount;
    if (HitWorld(r, kMinT, kMaxT, rec, id))
    {
        // Cache-only robustness: re-normalize intersection normals that drift
        // out of the unit-length tolerance, so the cache metric and every
        // downstream Ray/Scatter see a unit normal. The production (cache
        // disabled) path is untouched and stays bit-identical.
        if (ctx && ctx->useCache && fabsf(sqLength(rec.normal) - 1.0f) >= 0.01f)
            rec.normal = normalize(rec.normal);

        Ray scattered;
        float3 attenuation;
        float3 lightE;
        const Material& mat = s_SphereMats[id];
        float3 matE = mat.emissive.toFloat3();

        const bool cacheDiffuse =
            ctx && ctx->useCache && mat.type == Material::Lambert && depth < kMaxDepth;
        if (cacheDiffuse)
        {
            // Normal facing the incoming-light side, and which sphere side.
            // Re-normalize only when the intersection normal drifted, since
            // the cache's error metric assumes unit normals.
            float3 faceN = dot(rec.normal, r.dir) < 0.0f ? rec.normal : -rec.normal;
            if (fabsf(sqLength(faceN) - 1.0f) >= 0.01f)
                faceN = normalize(faceN);
            float3 nl = faceN;
            int side = dot(rec.normal, r.dir) < 0.0f ? kICSideFront : kICSideBack;

            // Direct sphere-light estimate: exactly once, via production code.
            lightE = LambertLightE(mat, rec, r, inoutRayCount, state);

            IRecord query;
            query.sphereId = id;
            query.side = side;
            query.remainingDepth = kMaxDepth - depth;
            query.x = rec.pos;
            query.n = nl;

            ICPathId pathId;
            pathId.frame = ctx->frame;
            pathId.pixel = ctx->pixel;
            pathId.sample = ctx->sample;
            pathId.pathDepth = static_cast<uint32_t>(depth);

            ICCandidate candidate;
            bool wasMiss = false;
            float3 H = GetIrradianceCache().QueryOrGather(
                query, pathId, ICTest::Scene(), inoutRayCount, candidate, wasMiss);
            if (wasMiss)
                ctx->candidates->push_back(candidate);

#if DO_LIGHT_SAMPLING
            // Same emission ownership as the production Trace: a Lambert
            // vertex reached while the previous bounce already sampled this
            // light explicitly must not add its emission again.
            if (!doMaterialE)
                matE = float3(0, 0, 0);
#endif
            // H is E/pi with no local albedo; apply this surface's albedo once.
            return matE + lightE + mat.albedo.toFloat3() * H;
        }

        if (depth < kMaxDepth && Scatter(mat, r, rec, attenuation, scattered, lightE, inoutRayCount, state))
        {
#if DO_LIGHT_SAMPLING
            if (!doMaterialE) matE = float3(0,0,0); // don't add material emission if told so
            // dor Lambert materials, we just did explicit light (emissive) sampling and already
            // for their contribution, so if next ray bounce hits the light again, don't add
            // emission
            doMaterialE = (mat.type != Material::Lambert);
#endif
            return matE + lightE + attenuation * Trace(scattered, depth+1, inoutRayCount, state, ctx, doMaterialE);
        }
        else
        {
            return matE;
        }
    }
    else
    {
        // sky
        return SkyRadiance(r);
    }
}

// Recursive transport used by cache hemisphere probes and the test adapter.
// It reuses the real intersection and Scatter code (cache internally
// disabled); the only change versus Trace is that the self-emission of a
// sphere a probe directly strikes on its first segment is removed. The sky is
// untouched, and a light seen after a specular event or one real scatter still
// contributes, exactly as in production. Emission ownership between bounces
// (doMaterialE) is threaded exactly as in the production Trace.
static float3 ProbeTrace(const Ray& r, int depth, int limit, bool doMaterialE,
                         int& inoutRayCount, uint32_t& state,
                         bool suppressFirstEmission, ICProbeInfo& info)
{
    Hit rec;
    int id = 0;
    ++inoutRayCount;
    if (HitWorld(r, kMinT, kMaxT, rec, id))
    {
        if (depth == 0)
        {
            info.hit = true;
            info.firstDist = rec.t;
        }
        Ray scattered;
        float3 attenuation;
        float3 lightE;
        const Material& mat = s_SphereMats[id];
        float3 matE = mat.emissive.toFloat3();
#if DO_LIGHT_SAMPLING
        if (!doMaterialE)
            matE = float3(0, 0, 0);
#endif
        if (depth == 0 && suppressFirstEmission)
            matE = float3(0, 0, 0); // direct first-hit sphere light removed

        // Probe-only robustness: HitSpheres normals can drift slightly from
        // unit length on small spheres; re-normalize only when needed (the
        // production Trace path is untouched and stays bit-identical).
        if (fabsf(sqLength(rec.normal) - 1.0f) >= 0.01f)
            rec.normal = normalize(rec.normal);

        if (depth < limit && Scatter(mat, r, rec, attenuation, scattered, lightE, inoutRayCount, state))
        {
#if DO_LIGHT_SAMPLING
            // Lambert vertices already accounted for this light explicitly.
            bool nextDoMaterialE = (mat.type != Material::Lambert);
#else
            bool nextDoMaterialE = true;
#endif
            return matE + lightE +
                attenuation * ProbeTrace(scattered, depth + 1, limit, nextDoMaterialE,
                                         inoutRayCount, state, suppressFirstEmission, info);
        }
        return matE;
    }
    else
    {
        return SkyRadiance(r);
    }
}

// Finite connector occlusion for cache visibility tests. Ignores the sphere
// that owns the record; every other sphere blocks.
static bool ICacheOccluded(const float3& p, const float3& dir, float segLen, int ignoreSphereId)
{
    Hit hit;
    int hitID;
    if (HitWorld(Ray(p, dir), kMinT, segLen, hit, hitID) && hitID != ignoreSphereId)
        return true;
    return false;
}

// Binds the real scene services to the cache. The cache owns no integrator.
static float3 ICacheProbeLi(const float3& origin, const float3& dir, int probeDepth,
                            uint32_t& rng, int& rayCount, ICProbeInfo& outInfo)
{
    Ray probe(origin, dir);
    return ProbeTrace(probe, 0, probeDepth, true, rayCount, rng, true, outInfo);
}

namespace ICTest
{
    const ICScene& Scene()
    {
        static const ICScene s_scene = ICScene{ &ICacheOccluded, &ICacheProbeLi };
        return s_scene;
    }
}

#if CPU_CAN_DO_THREADS
static enkiTaskScheduler* g_TS;
#endif

void InitializeTest()
{
    LoadDefaultScene();
    #if CPU_CAN_DO_THREADS
    g_TS = enkiNewTaskScheduler();
    enkiInitTaskScheduler(g_TS);
    #endif
}

void ShutdownTest()
{
    #if CPU_CAN_DO_THREADS
    enkiDeleteTaskScheduler(g_TS);
    g_TS = nullptr;
    #endif
}

static int s_RowSchedule = ICTest::kScheduleEnkiTS;

struct JobData
{
    float time;
    int frameCount;
    int screenWidth, screenHeight;
    float* backbuffer;
    Camera* cam;
    std::atomic<int> rayCount;
    unsigned testFlags;
    bool useCache;
    std::vector<ICCandidate>* candidates; // one vector per screen row
};

static void TraceRowJob(uint32_t start, uint32_t end, uint32_t threadnum, void* data_)
{
    (void)threadnum;
    JobData& data = *(JobData*)data_;
    float* backbuffer = data.backbuffer + start * data.screenWidth * 4;
    float invWidth = 1.0f / data.screenWidth;
    float invHeight = 1.0f / data.screenHeight;
    float lerpFac = float(data.frameCount) / float(data.frameCount+1);
    if (data.testFlags & kFlagAnimate)
        lerpFac *= DO_ANIMATE_SMOOTHING;
    if (!(data.testFlags & kFlagProgressive))
        lerpFac = 0;
    int rayCount = 0;
    for (uint32_t y = start; y < end; ++y)
    {
        uint32_t state = (y * 9781 + data.frameCount * 6271) | 1;
        for (int x = 0; x < data.screenWidth; ++x)
        {
            float3 col(0, 0, 0);
            for (int s = 0; s < DO_SAMPLES_PER_PIXEL; s++)
            {
                float u = float(x + RandomFloat01(state)) * invWidth;
                float v = float(y + RandomFloat01(state)) * invHeight;
                Ray r = data.cam->GetRay(u, v, state);

                TraceCtx ctx;
                ctx.useCache = data.useCache;
                ctx.frame = static_cast<uint32_t>(data.frameCount);
                ctx.pixel = static_cast<uint32_t>(y * data.screenWidth + x);
                ctx.sample = static_cast<uint32_t>(s);
                ctx.candidates = data.useCache ? &data.candidates[y] : nullptr;

                col += Trace(r, 0, rayCount, state, data.useCache ? &ctx : nullptr);
            }
            col *= 1.0f / float(DO_SAMPLES_PER_PIXEL);

            float3 prev(backbuffer[0], backbuffer[1], backbuffer[2]);
            col = prev * lerpFac + col * (1-lerpFac);
            col.store(backbuffer);
            backbuffer += 4;
        }
    }
    data.rayCount += rayCount;
}

// Snapshot used to invalidate the cache when geometry/materials/emission move.
// Camera motion is deliberately absent.
struct SceneSnapshot
{
    struct Item
    {
        float3pack center;
        float radius;
        Material mat;
    };
    Item items[kMaxSpheres];
    int count = 0;
    bool valid = false;
};
static SceneSnapshot s_Snapshot;

static void RefreshScene(bool allowInvalidation)
{
    s_EmissiveSphereCount = 0;
    for (int i = 0; i < s_SphereCount; ++i)
    {
        Sphere& s = s_Spheres[i];
        s.UpdateDerivedData();
        s_SpheresSoA.centerX[i] = s.center.x;
        s_SpheresSoA.centerY[i] = s.center.y;
        s_SpheresSoA.centerZ[i] = s.center.z;
        s_SpheresSoA.sqRadius[i] = s.radius * s.radius;
        s_SpheresSoA.invRadius[i] = s.invRadius;

        // Remember IDs of emissive spheres (light sources)
        const Material& smat = s_SphereMats[i];
        if (smat.emissive.x > 0 || smat.emissive.y > 0 || smat.emissive.z > 0)
        {
            s_EmissiveSpheres[s_EmissiveSphereCount] = i;
            s_EmissiveSphereCount++;
        }
    }

    // Retire any removed spheres so the SIMD intersection cannot hit stale
    // geometry left in the structure-of-arrays storage.
    for (int i = s_SphereCount; i < s_SpheresSoA.simdCount; ++i)
    {
        s_SpheresSoA.centerX[i] = s_SpheresSoA.centerY[i] = s_SpheresSoA.centerZ[i] = 10000.0f;
        s_SpheresSoA.sqRadius[i] = 0.0f;
        s_SpheresSoA.invRadius[i] = 0.0f;
    }

    bool changed = false;
    if (allowInvalidation)
    {
        if (!s_Snapshot.valid || s_Snapshot.count != s_SphereCount)
        {
            changed = true;
        }
        else
        {
            for (int i = 0; i < s_SphereCount; ++i)
            {
                const SceneSnapshot::Item& a = s_Snapshot.items[i];
                const Sphere& s = s_Spheres[i];
                const Material& m = s_SphereMats[i];
                if (a.radius != s.radius ||
                    a.center.x != s.center.x || a.center.y != s.center.y || a.center.z != s.center.z ||
                    a.mat.type != m.type ||
                    a.mat.albedo.x != m.albedo.x || a.mat.albedo.y != m.albedo.y || a.mat.albedo.z != m.albedo.z ||
                    a.mat.emissive.x != m.emissive.x || a.mat.emissive.y != m.emissive.y || a.mat.emissive.z != m.emissive.z ||
                    a.mat.roughness != m.roughness || a.mat.ri != m.ri)
                {
                    changed = true;
                    break;
                }
            }
        }
        if (changed && GetIrradianceCache().IsConfigured())
            GetIrradianceCache().Clear();
    }

    s_Snapshot.count = s_SphereCount;
    for (int i = 0; i < s_SphereCount; ++i)
    {
        s_Snapshot.items[i].center = s_Spheres[i].center;
        s_Snapshot.items[i].radius = s_Spheres[i].radius;
        s_Snapshot.items[i].mat = s_SphereMats[i];
    }
    s_Snapshot.valid = true;
}

void UpdateTest(float time, int frameCount, int screenWidth, int screenHeight, unsigned testFlags)
{
    if (testFlags & kFlagAnimate)
    {
        s_Spheres[1].center.y = cosf(time) + 1.0f;
        s_Spheres[8].center.z = sinf(time) * 0.3f;
    }
    float3 lookfrom(0, 2, 3);
    float3 lookat(0, 0, 0);
    float distToFocus = 3;
#if DO_MITSUBA_COMPARE
    float aperture = 0.0f;
#else
    float aperture = 0.1f;
#endif
#if DO_BIG_SCENE
    aperture *= 0.2f;
#endif

    // Geometry/material changes invalidate the cache; the camera does not.
    RefreshScene(true);

    s_Cam = Camera(lookfrom, lookat, float3(0, 1, 0), 60, float(screenWidth) / float(screenHeight), aperture, distToFocus);
}

void DrawTest(float time, int frameCount, int screenWidth, int screenHeight, float* backbuffer, int& outRayCount, unsigned testFlags)
{
    const bool useCache = (testFlags & kFlagIrradianceCache) != 0 &&
                          GetIrradianceCache().IsConfigured();
    if (useCache)
        GetIrradianceCache().BeginFrame(static_cast<uint32_t>(frameCount));

    JobData args;
    args.time = time;
    args.frameCount = frameCount;
    args.screenWidth = screenWidth;
    args.screenHeight = screenHeight;
    args.backbuffer = backbuffer;
    args.cam = &s_Cam;
    args.testFlags = testFlags;
    args.rayCount = 0;
    args.useCache = useCache;
    std::vector<std::vector<ICCandidate>> rowCandidates(screenHeight);
    args.candidates = rowCandidates.data();

    #if CPU_CAN_DO_THREADS
    if (s_RowSchedule == ICTest::kScheduleEnkiTS)
    {
        if (!g_TS)
        {
            g_TS = enkiNewTaskScheduler();
            enkiInitTaskScheduler(g_TS);
        }
        enkiTaskSet* task = enkiCreateTaskSet(g_TS, TraceRowJob);
        bool threaded = true;
        enkiAddTaskSetMinRange(g_TS, task, &args, screenHeight, threaded ? 4 : screenHeight);
        enkiWaitForTaskSet(g_TS, task);
        enkiDeleteTaskSet(g_TS, task);
    }
    else if (s_RowSchedule == ICTest::kScheduleForward)
    {
        TraceRowJob(0, screenHeight, 0, &args);
    }
    else
    {
        for (int y = screenHeight - 1; y >= 0; --y)
            TraceRowJob(static_cast<uint32_t>(y), static_cast<uint32_t>(y + 1), 0, &args);
    }
    #else
    TraceRowJob(0, screenHeight, 0, &args);
    #endif

    if (useCache)
    {
        // Rows only wrote their own candidate vectors. Publish them globally in
        // stable (frame, pixel, sample, depth) order; they are readable only on
        // the next round.
        std::vector<ICCandidate> all;
        for (int y = 0; y < screenHeight; ++y)
            all.insert(all.end(), rowCandidates[y].begin(), rowCandidates[y].end());
        GetIrradianceCache().PublishCandidates(all, static_cast<uint32_t>(frameCount));
    }

    outRayCount = args.rayCount;
}

void GetObjectCount(int& outCount, int& outObjectSize, int& outMaterialSize, int& outCamSize)
{
    outCount = kSphereCount;
    outObjectSize = sizeof(Sphere);
    outMaterialSize = sizeof(Material);
    outCamSize = sizeof(Camera);
}

void GetSceneDesc(void* outObjects, void* outMaterials, void* outCam, void* outEmissives, int* outEmissiveCount)
{
    memcpy(outObjects, s_Spheres, kSphereCount * sizeof(s_Spheres[0]));
    memcpy(outMaterials, s_SphereMats, kSphereCount * sizeof(s_SphereMats[0]));
    memcpy(outCam, &s_Cam, sizeof(s_Cam));
    memcpy(outEmissives, s_EmissiveSpheres, s_EmissiveSphereCount * sizeof(s_EmissiveSpheres[0]));
    *outEmissiveCount = s_EmissiveSphereCount;
}

// ===========================================================================
// Test adapter: drives the real production scene/integrator for tests and the
// example without maintaining a second integrator.
// ===========================================================================
namespace ICTest
{
    void ResetScene()
    {
        GetIrradianceCache().Clear();
        LoadDefaultScene();
        s_Snapshot.valid = false;
        s_SkyMode = 0;
        s_RowSchedule = kScheduleEnkiTS;
    }

    void ClearSpheres()
    {
        GetIrradianceCache().Clear();
        s_SphereCount = 0;
        s_Snapshot.valid = false;
    }

    int AddSphere(const float3& center, float radius, int materialType,
                  const float3& albedo, const float3& emissive, float roughness, float ri)
    {
        if (s_SphereCount >= kMaxSpheres)
            return -1;
        int id = s_SphereCount++;
        s_Spheres[id] = Sphere(center, radius);
        s_SphereMats[id] = Material{ static_cast<Material::Type>(materialType),
                                     float3pack(albedo), float3pack(emissive), roughness, ri };
        return id;
    }

    void SetSphere(int id, const float3& center, float radius)
    {
        if (id < 0 || id >= s_SphereCount) return;
        s_Spheres[id] = Sphere(center, radius);
    }

    void SetMaterial(int id, int materialType, const float3& albedo,
                     const float3& emissive, float roughness, float ri)
    {
        if (id < 0 || id >= s_SphereCount) return;
        s_SphereMats[id] = Material{ static_cast<Material::Type>(materialType),
                                    float3pack(albedo), float3pack(emissive), roughness, ri };
    }

    void SetSky(int mode, const float3& constant)
    {
        s_SkyMode = mode;
        s_SkyConstant = constant;
    }

    void Refresh()
    {
        RefreshScene(true);
    }

    int SphereCount() { return s_SphereCount; }

    void SetSchedule(int mode) { s_RowSchedule = mode; }

    float3 DirectLight(int sphereId, const float3& pos, const float3& normal,
                       const float3& rdir, uint32_t seed, int& rayCount)
    {
        if (sphereId < 0 || sphereId >= s_SphereCount)
            return float3(0, 0, 0);
        Hit rec;
        rec.pos = pos;
        rec.normal = normalize(normal);
        rec.t = 0.0f;
        Ray rIn(pos, normalize(rdir));
        uint32_t state = seed | 1u;
        return LambertLightE(s_SphereMats[sphereId], rec, rIn, rayCount, state);
    }

    RayResult FireRay(const float3& origin, const float3& dir, int limit, uint32_t seed)
    {
        RayResult res;
        std::memset(&res, 0, sizeof(res));
        Hit rec;
        int id = 0;
        int rayCount = 0;
        uint32_t state = seed | 1u;
        Ray r(origin, dir);
        ++rayCount;
        res.hit = HitWorld(r, kMinT, kMaxT, rec, id);
        if (res.hit)
        {
            res.sphereId = id;
            res.pos = rec.pos;
            res.normal = rec.normal;
            res.t = rec.t;
        }
        // Re-run the full production transport with an explicit scatter limit;
        // this is the same code path the cache probes use but with the first
        // hit emission retained (production ownership).
        ICProbeInfo probeInfo;
        res.Li = ProbeTrace(r, 0, limit, true, rayCount, state, false, probeInfo);
        res.rays = rayCount;
        return res;
    }

    void RunDraw(int width, int height, int frame, unsigned flags,
                 std::vector<float>& backbuffer, int& outRays)
    {
        RefreshScene(false);
        s_Cam = Camera(float3(0, 2, 3), float3(0, 0, 0), float3(0, 1, 0), 60,
                       float(width) / float(height), 0.0f, 3.0f);
        backbuffer.assign(width * height * 4, 0.0f);
        DrawTest(0.0f, frame, width, height, backbuffer.data(), outRays, flags);
    }
}
