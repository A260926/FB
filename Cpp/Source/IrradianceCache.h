#pragma once

// Optional biased irradiance cache for the CPU preview.
//
// The cache stores one record per diffuse (Lambert) shading event. A record
// holds the mean radiance H returned by cosine-weighted hemisphere probes;
// nearby diffuse hits reuse H instead of running their own hemisphere
// transport. Metal, dielectric, emissive materials and the explicit sphere
// light sampling always go through the production path. The cache is off by
// default and deliberately allows biased spatial interpolation.

#include "Maths.h"
#include <stdint.h>
#include <stddef.h>
#include <vector>
#include <string>
#include <atomic>

// The DrawTest() enable flag is kFlagIrradianceCache, declared in Test.h.
// Include Test.h (which includes this header) to get the flag enum.

// Transport limits, shared with the production integrator.
constexpr int kICMaxDepth = 10;
constexpr float kICMinT = 0.001f;
constexpr float kICMaxT = 1.0e7f;

enum ICMaterialType
{
    kICLambert = 0,
    kICMetal = 1,
    kICDielectric = 2,
};

// Material description used by a transport world. Layout matches the
// production Test.cpp Material (enum, three packed floats, roughness, ri).
struct ICMaterial
{
    ICMaterialType type;
    float3pack albedo;
    float3pack emissive;
    float roughness;
    float ri;
};

// Configuration of the cache. An invalid configuration is rejected by
// Configure() and leaves the current state (records and stats) untouched.
struct ICConfig
{
    ICConfig()
        : gatherSamples(0)
        , tau(0.0f)
        , minRadius(0.0f)
        , maxRadius(0.0f)
        , seed(0)
        , enabled(false)
    {}

    // Number of cosine hemisphere probes per gather. Must be a positive int.
    int gatherSamples;
    // Error threshold: a record is usable only when e < tau. 0 < tau <= 1.
    float tau;
    // Record radius clamp range: 0 < minRadius <= maxRadius, both finite.
    float minRadius;
    float maxRadius;
    // RNG seed; gather streams derive from this and the path identity only.
    uint32_t seed;
    // False = cache disabled (production path, identical random stream).
    bool enabled;
};

// One cached irradiance probe result.
struct ICRecord
{
    int sphereId;       // original sphere id of the shaded surface
    int side;           // 0 = front face (ray outside), 1 = back face
    float3pack x;       // hit position
    float3pack n;       // unit normal facing the incoming-light side
    int remaining;      // remaining scatter count at the shading point
    float3pack H;       // mean probe radiance (E/pi); no local albedo in it
    float R;            // harmonic-mean radius
};

// Per-probe public evidence of a gather, for test verification.
struct ICGatherSample
{
    float3pack direction;     // cosine-weighted probe direction
    float firstDistance;      // first-segment distance, clamped to [kICMinT, maxRadius]
    float3pack contribution;  // Li returned by the real recursive transport
};

struct ICGatherResult
{
    std::vector<ICGatherSample> samples;
    float3pack H;
    float radius;
};

// Identity of a shading event. The gather random stream is a pure function
// of this identity and the configured seed; it never depends on the worker
// thread number or on candidate insertion order.
struct ICPathIdentity
{
    int frame;
    int pixelX;
    int pixelY;
    int sample;
    int depth;
};

struct ICStats
{
    uint64_t queries;            // cache lookups
    uint64_t hits;               // lookups that reused a record
    uint64_t gathers;            // lookups that ran hemisphere probes
    uint64_t probeRays;          // hemisphere probe rays cast by gathers
    uint64_t connectionRays;     // visibility connection rays cast by queries
    uint64_t recordChecks;       // individual records inspected via the index
    uint64_t candidates;         // candidates staged during the current round
    uint64_t recordsPublished;   // total readable records
};

// Abstract scene access. The production scene in Test.cpp and the test
// adapter world both implement this; the cache never owns scene data.
struct ICSceneAdapter
{
    virtual ~ICSceneAdapter() = default;
    virtual bool HitWorld(const Ray& r, float tMin, float tMax, Hit& outHit, int& outId) = 0;
    virtual float3 Sky(const Ray& r) = 0;
    // Real recursive transport for a hemisphere probe. Emission of a direct
    // first-hit emissive sphere is stripped; sky is kept. The cache is
    // always off inside a probe. depth is the current global path depth,
    // firstSegment is true only for the probe's first ray. *outFirstT (when
    // non-null) receives the first intersection distance (or a large value
    // on escape, which the caller clamps to maxRadius).
    virtual float3 TraceProbe(const Ray& r, int depth, bool firstSegment,
                              int& inoutRayCount, uint32_t& state, float* outFirstT) = 0;
    virtual int MaterialType(int id) = 0;
    virtual float3 MaterialAlbedo(int id) = 0;
    virtual float3 MaterialEmissive(int id) = 0;
    // Production explicit sphere-light sampling at a Lambert hit.
    virtual float3 SampleLights(const Hit& rec, const Ray& rIn, int selfId,
                                const float3& albedo, int& inoutRayCount, uint32_t& state) = 0;
    // Signature of geometry + materials + emissives. Camera movement must
    // not affect it; any sphere position/radius/material/emissive change does.
    virtual uint64_t SceneSignature() = 0;
};

// A self-contained transport world used by tests and by the example. It runs
// the SAME integrator as the production preview (no second integrator):
// spheres, materials, a configurable sky and the production Trace/Scatter.
struct ICWorld;

ICWorld* ICWorld_Create(int maxSpheres);
void ICWorld_Release(ICWorld* world);
void ICWorld_SetSphere(ICWorld* world, int id, const float3& center, float radius);
void ICWorld_SetMaterial(ICWorld* world, int id, const ICMaterial& material);
// skyMode 0 = production gradient sky, skyMode 1 = constant sky color.
void ICWorld_SetSky(ICWorld* world, int skyMode, const float3& constantSky);
// Build SoA intersection data and the emissive sphere list after edits.
void ICWorld_Finish(ICWorld* world);
int ICWorld_SphereCount(ICWorld* world);
const Sphere* ICWorld_GetSpheres(ICWorld* world);
const ICMaterial* ICWorld_GetMaterials(ICWorld* world);
ICSceneAdapter* ICWorld_GetAdapter(ICWorld* world);
// Fire a real single ray through the production integrator (cache off).
float3 ICWorld_TraceRay(ICWorld* world, const Ray& r, int depth,
                        int& inoutRayCount, uint32_t& state);
// Production explicit light sampling at a Lambert hit of this world.
float3 ICWorld_SampleLights(ICWorld* world, const Hit& rec, const Ray& rIn,
                            int selfId, const float3& albedo,
                            int& inoutRayCount, uint32_t& state);

class IrradianceCache
{
public:
    IrradianceCache();

    // Replaces the configuration. Returns false and changes nothing when the
    // configuration is invalid or disabled-with-zero fields; any accepted
    // change clears all records and candidates.
    bool Configure(const ICConfig& config);
    const ICConfig& GetConfig() const { return m_config; }
    bool IsEnabled() const { return m_config.enabled; }

    void Clear();
    void ResetStats();
    void GetStats(ICStats& out) const;

    // Round management. BeginDraw freezes the readable set: candidates from
    // the previous round are published (in stable identity order) and become
    // the only records visible during this round. A scene signature change
    // invalidates every record before the round starts.
    void BeginDraw(int frameCount, int screenHeight, uint64_t sceneSignature);
    void EndDraw();

    // Looks up H for a Lambert shading point. On a miss, runs a real gather,
    // stages a candidate (invisible until the next round) and returns its H.
    // The returned radiance does NOT include the local albedo; callers
    // multiply once themselves.
    float3 LookupOrGather(ICSceneAdapter& scene, int sphereId, int side,
                          const float3& pos, const float3& normal,
                          int remaining, const ICPathIdentity& identity,
                          int& inoutRayCount);

    // Runs a gather directly and exposes every probe direction, first
    // segment distance and per-sample contribution. Does not stage a record.
    ICGatherResult Gather(ICSceneAdapter& scene, const float3& pos,
                          const float3& normal, int remaining,
                          const ICPathIdentity& identity,
                          int& inoutRayCount) const;

    // Read-only access to the currently readable records.
    size_t GetRecordCount() const { return m_records.size(); }
    const ICRecord* GetRecords() const { return m_records.empty() ? nullptr : m_records.data(); }

    // Test support: inject a record straight into the readable set and
    // rebuild the spatial index. Not used by the production path.
    void DebugInjectRecord(const ICRecord& record);
    void DebugRebuildIndex();
    // Runs a pure query without gathering; returns false when nothing is
    // usable. Exposes the weighted H and the number of records used.
    bool DebugQuery(ICSceneAdapter& scene, int sphereId, int side,
                    const float3& pos, const float3& normal, int remaining,
                    float3& outH, int& outUsed, int& inoutRayCount);

private:
    struct Candidate
    {
        ICRecord record;
        ICPathIdentity identity;
    };

    void publishCandidates();
    void rebuildIndex();
    bool visible(ICSceneAdapter& scene, int ownerSphere,
                 const float3& a, const float3& na,
                 const float3& b, const float3& nb,
                 int& inoutRayCount);

    ICConfig m_config;
    std::vector<ICRecord> m_records;            // readable set
    size_t m_frozenCount = 0;                   // records visible this round
    std::vector<std::vector<Candidate>> m_rowCandidates;
    uint64_t m_lastSignature = 0;
    bool m_haveSignature = false;

    // Uniform spatial grid over the readable set; cell size = maxRadius.
    struct Cell { std::vector<uint32_t> indices; };
    float m_gridOrigin[3] = {0, 0, 0};
    int m_gridDim[3] = {0, 0, 0};
    std::vector<Cell> m_cells;

    // Atomic because row jobs query the cache concurrently; the readable set
    // itself is immutable for the whole round.
    std::atomic<uint64_t> m_aQueries{0};
    std::atomic<uint64_t> m_aHits{0};
    std::atomic<uint64_t> m_aGathers{0};
    std::atomic<uint64_t> m_aProbeRays{0};
    std::atomic<uint64_t> m_aConnectionRays{0};
    std::atomic<uint64_t> m_aRecordChecks{0};
};

// Production singleton accessors used by Test.cpp, the example and the README.
IrradianceCache& GetIrradianceCache();
bool ConfigureIrradianceCache(const ICConfig& config);
void ClearIrradianceCache();
void GetIrradianceCacheStats(ICStats& out);
size_t GetIrradianceCacheRecordCount();
const ICRecord* GetIrradianceCacheRecords(size_t& outCount);
