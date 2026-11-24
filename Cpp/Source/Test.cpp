#include "Config.h"
#include "Test.h"
#include "Maths.h"
#include "IrradianceCache.h"
#include <algorithm>
#if CPU_CAN_DO_THREADS
#include "enkiTS/TaskScheduler_c.h"
#endif
#include <atomic>

// 46 spheres (2 emissive) when enabled; 9 spheres (1 emissive) when disabled
#define DO_BIG_SCENE 1

static Sphere s_Spheres[] =
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
const int kSphereCount = sizeof(s_Spheres) / sizeof(s_Spheres[0]);

static SpheresSoA s_SpheresSoA(kSphereCount);

struct Material
{
    enum Type { Lambert, Metal, Dielectric };
    Type type;
    float3pack albedo;
    float3pack emissive;
    float roughness;
    float ri;
};

static Material s_SphereMats[kSphereCount] =
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

static int s_EmissiveSpheres[kSphereCount];
static int s_EmissiveSphereCount;

static Camera s_Cam;

const float kMinT = 0.001f;
const float kMaxT = 1.0e7f;
const int kMaxDepth = 10;


bool HitWorld(const Ray& r, float tMin, float tMax, Hit& outHit, int& outID)
{
    outID = HitSpheres(r, s_SpheresSoA, tMin, tMax, outHit);
    return outID != -1;
}

static uint64_t s_SceneSignature = 0;

static bool Scatter(const Material& mat, const Ray& r_in, const Hit& rec, float3& attenuation,
                    Ray& scattered, float3& outLightE, int& inoutRayCount, uint32_t& state);

static float3 ProductionSky(const Ray& r)
{
#if DO_MITSUBA_COMPARE
    return float3(0.15f,0.21f,0.3f);
#else
    float3 unitDir = r.dir;
    float t = 0.5f*(unitDir.getY() + 1.0f);
    return ((1.0f-t)*float3(1.0f, 1.0f, 1.0f) + t*float3(0.5f, 0.7f, 1.0f)) * 0.3f;
#endif
}

// Recursive transport for an irradiance-cache probe. The cache is always off
// in here: it reuses the real intersection and Scatter() transport. A direct
// first hit on an emissive sphere contributes no self-emission (the current
// shading point accounts for that light once via explicit sampling), while
// the sky is kept; after a specular event or a real scatter, light emission
// keeps the production attribution rules. outFirstT receives the first
// segment distance; an escape reports a large value the caller clamps.
static float3 TraceProbeImpl(const Ray& r, int depth, bool firstSegment,
                             int& inoutRayCount, uint32_t& state, float* outFirstT)
{
    Hit rec;
    int id = 0;
    ++inoutRayCount;
    if (HitWorld(r, kMinT, kMaxT, rec, id))
    {
        if (outFirstT)
            *outFirstT = rec.t;
        const Material& mat = s_SphereMats[id];
        bool emissive = mat.emissive.x > 0 || mat.emissive.y > 0 || mat.emissive.z > 0;
        if (firstSegment && emissive)
            return float3(0, 0, 0); // strip direct first-hit sphere-light emission
        float3 matE = mat.emissive.toFloat3();
        Ray scattered;
        float3 attenuation, lightE;
        if (depth < kMaxDepth && Scatter(mat, r, rec, attenuation, scattered, lightE, inoutRayCount, state))
        {
            (void)0; // deeper bounces retain the production emission attribution
            return matE + lightE + attenuation * TraceProbeImpl(scattered, depth + 1, false, inoutRayCount, state, nullptr);
        }
        return matE;
    }
    if (outFirstT)
        *outFirstT = kMaxT;
    return ProductionSky(r);
}

// Explicit sphere-light sampling at a Lambert hit, factored out of Scatter()
// so both the production path and the cache adapter use the same code.
static float3 SampleLightsImpl(const Hit& rec, const Ray& rIn, int selfId,
                               const float3& albedo, int& inoutRayCount, uint32_t& state)
{
    float3 lightE(0, 0, 0);
#if DO_LIGHT_SAMPLING
    for (int j = 0; j < s_EmissiveSphereCount; ++j)
    {
        int i = s_EmissiveSpheres[j];
        if (i == selfId)
            continue;
        const Material& smat = s_SphereMats[i];
        const Sphere& s = s_Spheres[i];
        float3 sc = s.center.toFloat3();
        float3 sw = normalize(sc - rec.pos);
        float3 su = normalize(cross(fabs(sw.getX())>0.01f ? float3(0,1,0):float3(1,0,0), sw));
        float3 sv = cross(sw, su);
        float cosAMax = sqrtf(1.0f - s.radius*s.radius / sqLength(rec.pos-sc));
        float eps1 = RandomFloat01(state), eps2 = RandomFloat01(state);
        float cosA = 1.0f - eps1 + eps1 * cosAMax;
        float sinA = sqrtf(1.0f - cosA*cosA);
        float phi = 2 * kPI * eps2;
        float3 l = su * (cosf(phi) * sinA) + sv * (sinf(phi) * sinA) + sw * cosA;
        Hit lightHit;
        int hitID;
        ++inoutRayCount;
        if (HitWorld(Ray(rec.pos, l), kMinT, kMaxT, lightHit, hitID) && hitID == i)
        {
            float omega = 2 * kPI * (1-cosAMax);
            float3 nl = dot(rec.normal, rIn.dir) < 0 ? rec.normal : -rec.normal;
            lightE += (albedo * smat.emissive.toFloat3()) * (std::max(0.0f, dot(l, nl)) * omega / kPI);
        }
    }
#else
    (void)rec; (void)rIn; (void)selfId; (void)albedo; (void)inoutRayCount; (void)state;
#endif
    return lightE;
}

class ProductionSceneAdapter : public ICSceneAdapter
{
public:
    bool HitWorld(const Ray& r, float tMin, float tMax, Hit& outHit, int& outId) override
    {
        return ::HitWorld(r, tMin, tMax, outHit, outId);
    }
    float3 Sky(const Ray& r) override { return ProductionSky(r); }
    float3 TraceProbe(const Ray& r, int depth, bool firstSegment, int& inoutRayCount,
                      uint32_t& state, float* outFirstT) override
    {
        return TraceProbeImpl(r, depth, firstSegment, inoutRayCount, state, outFirstT);
    }
    int MaterialType(int id) override { return (int)s_SphereMats[id].type; }
    float3 MaterialAlbedo(int id) override { return s_SphereMats[id].albedo.toFloat3(); }
    float3 MaterialEmissive(int id) override { return s_SphereMats[id].emissive.toFloat3(); }
    float3 SampleLights(const Hit& rec, const Ray& rIn, int selfId,
                        const float3& albedo, int& inoutRayCount, uint32_t& state) override
    {
        return SampleLightsImpl(rec, rIn, selfId, albedo, inoutRayCount, state);
    }
    uint64_t SceneSignature() override { return s_SceneSignature; }
};

static ProductionSceneAdapter s_ProductionAdapter;


static bool Scatter(const Material& mat, const Ray& r_in, const Hit& rec, float3& attenuation, Ray& scattered, float3& outLightE, int& inoutRayCount, uint32_t& state)
{
    outLightE = float3(0,0,0);
    if (mat.type == Material::Lambert)
    {
        // random point on unit sphere that is tangent to the hit point
        float3 target = rec.pos + rec.normal + RandomUnitVector(state);
        scattered = Ray(rec.pos, normalize(target - rec.pos));
        float3 matAlbedo = mat.albedo.toFloat3();
        attenuation = matAlbedo;

        // sample lights
#if DO_LIGHT_SAMPLING
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
            //l = normalize(l); // NOTE(fg): This is already normalized, by construction.

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
                outLightE += (matAlbedo * smatEmissive) * (std::max(0.0f, dot(l, nl)) * omega / kPI);
            }
        }
#endif
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
        scattered = Ray(rec.pos, normalize(refl + roughness*RandomInUnitSphere(state)));
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
            scattered = Ray(rec.pos, normalize(refl));
        else
            scattered = Ray(rec.pos, normalize(refr));
    }
    else
    {
        attenuation = float3(1,0,1);
        return false;
    }
    return true;
}

static float3 Trace(const Ray& r, int depth, int& inoutRayCount, uint32_t& state,
                    bool doMaterialE = true, const ICPathIdentity* identity = nullptr)
{
    Hit rec;
    int id = 0;
    ++inoutRayCount;
    if (HitWorld(r, kMinT, kMaxT, rec, id))
    {
        Ray scattered;
        float3 attenuation;
        float3 lightE;
        const Material& mat = s_SphereMats[id];
        float3 matE = mat.emissive.toFloat3();
        bool matEmissive = mat.emissive.x > 0 || mat.emissive.y > 0 || mat.emissive.z > 0;

        IrradianceCache& cache = GetIrradianceCache();
        if (identity && cache.IsEnabled() && depth < kMaxDepth &&
            mat.type == Material::Lambert && !matEmissive)
        {
            // Cache path for non-emissive Lambert surfaces. Metal, dielectric
            // and emissive materials keep the production Scatter() path.
#if DO_LIGHT_SAMPLING
            if (!doMaterialE)
                matE = float3(0, 0, 0);
#endif
            bool front = dot(rec.normal, r.dir) < 0;
            int side = front ? 0 : 1;
            float3 n = front ? rec.normal : -rec.normal; // faces the incoming light
            int remaining = kMaxDepth - depth;
            ICPathIdentity pid = *identity;
            pid.depth = depth;

            // The sphere-light direct estimate is still evaluated exactly once
            // here, with the production sampler; the cached H excludes it.
            float3 albedo = mat.albedo.toFloat3();
            float3 directE = SampleLightsImpl(rec, r, id, albedo, inoutRayCount, state);
            float3 H = cache.LookupOrGather(s_ProductionAdapter, id, side,
                                            rec.pos, n, remaining, pid, inoutRayCount);
            // Reuse multiplies the local albedo by H a single time.
            return matE + directE + albedo * H;
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
            return matE + lightE + attenuation * Trace(scattered, depth+1, inoutRayCount, state, doMaterialE, identity);
        }
        else
        {
            return matE;
        }
    }
    else
    {
        // sky
#if DO_MITSUBA_COMPARE
        return float3(0.15f,0.21f,0.3f); // easier compare with Mitsuba's constant environment light
#else
        float3 unitDir = r.dir;
        float t = 0.5f*(unitDir.getY() + 1.0f);
        return ((1.0f-t)*float3(1.0f, 1.0f, 1.0f) + t*float3(0.5f, 0.7f, 1.0f)) * 0.3f;
#endif
    }
}

#if CPU_CAN_DO_THREADS
static enkiTaskScheduler* g_TS;
#endif

static int s_RowScheduleMode = 0;

void SetRowScheduleMode(int mode)
{
    if (mode >= 0 && mode <= 2)
        s_RowScheduleMode = mode;
}

int GetRowScheduleMode()
{
    return s_RowScheduleMode;
}

void InitializeTest()
{
    s_RowScheduleMode = 0;
    #if CPU_CAN_DO_THREADS
    g_TS = enkiNewTaskScheduler();
    enkiInitTaskScheduler(g_TS);
    #endif
}

void ShutdownTest()
{
    #if CPU_CAN_DO_THREADS
    enkiDeleteTaskScheduler(g_TS);
    #endif
}

struct JobData
{
    float time;
    int frameCount;
    int screenWidth, screenHeight;
    float* backbuffer;
    Camera* cam;
    std::atomic<int> rayCount;
    unsigned testFlags;
    bool cacheOn;
};

static void TraceRowJob(uint32_t start, uint32_t end, uint32_t threadnum, void* data_)
{
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
                if (data.cacheOn)
                {
                    // Gather RNG derives from this identity and the seed, never
                    // from thread number or candidate order.
                    ICPathIdentity identity{data.frameCount, x, (int)y, s, 0};
                    col += Trace(r, 0, rayCount, state, true, &identity);
                }
                else
                {
                    col += Trace(r, 0, rayCount, state);
                }
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

    s_EmissiveSphereCount = 0;
    uint64_t sig = 1469598103934665603ull;
    auto foldBytes = [&](const void* p, size_t n)
    {
        const unsigned char* b = (const unsigned char*)p;
        for (size_t k = 0; k < n; ++k)
        {
            sig ^= b[k];
            sig *= 1099511628211ull;
        }
    };
    for (int i = 0; i < kSphereCount; ++i)
    {
        Sphere& s = s_Spheres[i];
        s.UpdateDerivedData();
        s_SpheresSoA.centerX[i] = s.center.x;
        s_SpheresSoA.centerY[i] = s.center.y;
        s_SpheresSoA.centerZ[i] = s.center.z;
        s_SpheresSoA.sqRadius[i] = s.radius * s.radius;
        s_SpheresSoA.invRadius[i] = s.invRadius;

        // The signature covers geometry, materials and emissives but never
        // the camera, so camera movement keeps the cache valid.
        foldBytes(&s.center.x, sizeof(float) * 3);
        foldBytes(&s.radius, sizeof(float));
        foldBytes(&s_SphereMats[i], sizeof(s_SphereMats[i]));

        // Remember IDs of emissive spheres (light sources)
        const Material& smat = s_SphereMats[i];
        if (smat.emissive.x > 0 || smat.emissive.y > 0 || smat.emissive.z > 0)
        {
            s_EmissiveSpheres[s_EmissiveSphereCount] = i;
            s_EmissiveSphereCount++;
        }
    }
    s_SceneSignature = sig;

    s_Cam = Camera(lookfrom, lookat, float3(0, 1, 0), 60, float(screenWidth) / float(screenHeight), aperture, distToFocus);
}

void DrawTest(float time, int frameCount, int screenWidth, int screenHeight, float* backbuffer, int& outRayCount, unsigned testFlags)
{
    JobData args;
    args.time = time;
    args.frameCount = frameCount;
    args.screenWidth = screenWidth;
    args.screenHeight = screenHeight;
    args.backbuffer = backbuffer;
    args.cam = &s_Cam;
    args.testFlags = testFlags;
    args.rayCount = 0;

    IrradianceCache& cache = GetIrradianceCache();
    args.cacheOn = (testFlags & kFlagIrradianceCache) != 0 && cache.IsEnabled();
    if (args.cacheOn)
    {
        // Freeze the readable set for this round; this round's candidates are
        // invisible until the next draw. A scene change invalidates first.
        cache.BeginDraw(frameCount, screenHeight, s_SceneSignature);
    }

    #if CPU_CAN_DO_THREADS
    if (args.cacheOn && s_RowScheduleMode == 1)
    {
        TraceRowJob(0, (uint32_t)screenHeight, 0, &args); // sequential forward
    }
    else if (args.cacheOn && s_RowScheduleMode == 2)
    {
        for (int y = screenHeight - 1; y >= 0; --y) // sequential reverse
            TraceRowJob((uint32_t)y, (uint32_t)y + 1, 0, &args);
    }
    else
    {
        enkiTaskSet* task = enkiCreateTaskSet(g_TS, TraceRowJob);
        enkiAddTaskSetMinRange(g_TS, task, &args, screenHeight, 4);
        enkiWaitForTaskSet(g_TS, task);
        enkiDeleteTaskSet(g_TS, task);
    }
    #else
    TraceRowJob(0, screenHeight, 0, &args);
    #endif

    if (args.cacheOn)
        cache.EndDraw();

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
