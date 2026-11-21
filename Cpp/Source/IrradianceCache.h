#pragma once

#include "Cpp/Source/Maths.h"
#include <cstdint>
#include <mutex>
#include <vector>
#include <functional>

// ---------------------------------------------------------------------------
// Optional, explicitly biased irradiance cache for the CPU preview.
//
// The cache stores cosine-hemisphere irradiance estimates (H = E/pi, i.e. the
// mean incoming radiance Li over cosine weighted hemisphere samples; the local
// surface albedo is never baked into H) at diffuse hit points. Nearby diffuse
// hits on the same sphere, same surface side and with the same remaining path
// depth can spatially interpolate H instead of tracing a new hemisphere.
//
// The cache never mixes whole frames: candidates produced during a DrawTest
// round are invisible until the round finishes and they are published in a
// stable (frame, pixel, sample, path depth) order.
// ---------------------------------------------------------------------------

enum
{
    kICSideFront = 0, // ray hit the outside of a sphere (dot(n, dir) < 0)
    kICSideBack = 1
};

// Shared ray epsilon used for normal offsets and the harmonic distance clamp.
constexpr float kMinT = 0.001f;

struct ICConfig
{
    ICConfig() : gatherSamples(0), tau(0.0f), minRadius(0.0f), maxRadius(0.0f), seed(0) {}

    int gatherSamples; // M: positive integer, number of hemisphere probes
    float tau;          // finite, 0 < tau <= 1
    float minRadius;    // finite, 0 < minRadius <= maxRadius
    float maxRadius;    // finite
    uint32_t seed;
};

// One stored irradiance record. H is E/pi (no local albedo factor).
struct IRecord
{
    int sphereId = -1;
    int side = kICSideFront;
    int remainingDepth = 0; // scatters still allowed at the record point
    float3 x;               // hit position
    float3 n;               // unit normal facing the incoming light side
    float3 H;               // mean cosine hemisphere Li
    float R = 0.0f;         // cache radius
};

// Per-probe public data of a gather, for test verification.
struct ICGatherSample
{
    float3 dir;        // actual cosine hemisphere direction used
    float firstDist;   // first segment distance, clamped to kMinT, truncated to maxRadius
    float3 Li;         // actual recursive transport contribution of that probe
};

struct ICGatherDebug
{
    int sphereId = -1;
    int side = kICSideFront;
    int remainingDepth = 0;
    float3 x;
    float3 n;
    float R = 0.0f;
    float3 H;
    std::vector<ICGatherSample> samples;
};

struct ICStats
{
    uint64_t queries = 0;             // cache lookups at diffuse vertices
    uint64_t cacheHits = 0;           // lookups served by interpolation
    uint64_t gathers = 0;             // lookups that had to gather
    uint64_t probeRays = 0;           // primary hemisphere probe rays traced
    uint64_t connectionChecks = 0;    // finite connector visibility rays
    uint64_t connectionBlocked = 0;   // connectors blocked by another sphere
    uint64_t recordsExamined = 0;     // individual records touched through the spatial index
    uint64_t candidatesPublished = 0; // records promoted into readable sets
};

// Stable identity of a path vertex; gather random streams derive from this and
// the configured seed, never from worker thread ids or insertion order.
struct ICPathId
{
    uint32_t frame = 0;
    uint32_t pixel = 0;
    uint32_t sample = 0;
    uint32_t pathDepth = 0;
};

// A candidate produced by one row task. Row tasks only append to their own
// candidate vector; the cache publishes them after every row has finished.
struct ICCandidate
{
    ICPathId id;
    IRecord record;
};

// First-segment information reported by the real probe transport.
struct ICProbeInfo
{
    bool hit = false;        // did the probe's first segment hit a sphere
    float firstDist = 0.0f;  // t of the first segment (valid when hit)
};

// Real scene services the cache needs. Transport is provided by the production
// integrator in Test.cpp; the cache itself contains no second integrator.
struct ICScene
{
    // Returns true when the finite segment from p along dir (length segLen) is
    // blocked by any sphere other than ignoreSphereId. End points are already
    // offset along their normals by the caller.
    std::function<bool(const float3& p, const float3& dir, float segLen, int ignoreSphereId)> occluded;

    // Evaluates the real recursive transport for one hemisphere probe, with
    // the cache internally disabled. probeDepth is the record's remaining
    // scatter count; the adapter maps it to the probe ray's path depth.
    std::function<float3(const float3& origin, const float3& dir, int probeDepth,
                         uint32_t& rng, int& rayCount, ICProbeInfo& outInfo)> probeLi;
};

class IrradianceCache
{
public:
    IrradianceCache();

    // Configuration. Invalid configurations return false and change nothing.
    // Any successful configuration change clears every record.
    bool Configure(const ICConfig& cfg);
    const ICConfig& GetConfig() const { return m_config; }
    bool IsConfigured() const { return m_configured; }

    // Drops all published records, candidates and gather logs.
    void Clear();

    // Freezes the readable set for one DrawTest round and (re)builds the
    // spatial index. Candidates from the current round stay invisible.
    void BeginFrame(uint32_t frame);

    // Looks up H for a diffuse vertex. On a miss, gathers using the real scene
    // transport and fills outCandidate (the caller stores it in its row-local
    // candidate buffer). On a hit, outCandidate is left untouched.
    float3 QueryOrGather(const IRecord& query, const ICPathId& pathId, const ICScene& scene,
                         int& rayCount, ICCandidate& outCandidate, bool& outWasMiss);

    // Publishes row-local candidates in stable (frame, pixel, sample, depth)
    // order. They become readable on the next BeginFrame.
    void PublishCandidates(std::vector<ICCandidate>& rowCandidates, uint32_t frame);

    // Read-only access to the currently readable records.
    size_t GetRecordCount() const { return m_published.size(); }
    const IRecord& GetRecord(size_t i) const { return m_published[i]; }
    const std::vector<IRecord>& GetRecords() const { return m_published; }

    ICStats GetStats() const;
    void ResetStats();

    // Public gather data (all gathers since the last Clear/Configure).
    size_t GetGatherLogCount() const { return m_gatherLog.size(); }
    const ICGatherDebug& GetGatherLog(size_t i) const { return m_gatherLog[i]; }
    void ClearGatherLog() { m_gatherLog.clear(); }

    // Test adapter: synthesize a record directly into the readable set.
    void TestInjectRecord(const IRecord& record);
    void TestBuildSpatialIndex();

private:
    struct Cell
    {
        uint64_t key;
        std::vector<int> records;
    };

    bool Visible(const float3& x, const float3& n, const IRecord& r, const ICScene& scene) const;
    void BuildIndex();
    static uint32_t GatherRng(const ICConfig& cfg, const ICPathId& id);

    ICConfig m_config;
    bool m_configured = false;
    uint32_t m_frame = 0;

    std::vector<IRecord> m_published;    // frozen readable set
    std::vector<IRecord> m_indexRecords; // records indexed this round
    bool m_indexDirty = true;
    // Sparse hash grid, cell size = maxRadius, kept sorted by key. A record
    // usable by a query lies within maxRadius, i.e. inside the 3x3x3 cell
    // neighbourhood, so out-of-domain records are never touched.
    std::vector<Cell> m_cells;

    std::vector<ICGatherDebug> m_gatherLog;
    ICStats m_stats;
    // Row tasks query concurrently; only stats and the gather log mutate, and
    // only behind this lock. The readable set and index stay immutable.
    mutable std::mutex m_mutationMutex;
};

// Global instance shared by Test.cpp, the example and the tests.
IrradianceCache& GetIrradianceCache();

// ---------------------------------------------------------------------------
// Test adapter implemented in Test.cpp. It drives the real production scene
// and integrator (the same HitWorld/Scatter/Trace code the renderer uses) so
// tests can set up spheres, pick the sky and fire real rays without a second
// integrator.
// ---------------------------------------------------------------------------
namespace ICTest
{
    enum Schedule
    {
        kScheduleEnkiTS = 0,
        kScheduleForward = 1, // rows in ascending order, single thread
        kScheduleReverse = 2  // rows in descending order, single thread
    };

    // Material type ids matching the production Material::Type enum.
    enum MaterialType
    {
        kMaterialLambert = 0,
        kMaterialMetal = 1,
        kMaterialDielectric = 2
    };

    struct RayResult
    {
        bool hit;
        int sphereId;
        float t;
        float3 pos;
        float3 normal;
        float3 Li;
        int rays;
    };

    // Restores the compile-time scene, the production gradient sky and the
    // default row schedule, and clears the cache.
    void ResetScene();

    // Drops every sphere so all rays escape to the sky; clears the cache.
    void ClearSpheres();

    int AddSphere(const float3& center, float radius, int materialType,
                  const float3& albedo, const float3& emissive, float roughness, float ri);
    void SetSphere(int id, const float3& center, float radius);
    void SetMaterial(int id, int materialType, const float3& albedo,
                     const float3& emissive, float roughness, float ri);
    // Sky mode 0 = production gradient, 1 = constant radiance.
    void SetSky(int mode, const float3& constant);
    void Refresh();
    int SphereCount();
    void SetSchedule(int mode);

    // Runs the production explicit sphere-light estimator (LambertLightE) for
    // one diffuse point, so tests can check the direct term in isolation.
    float3 DirectLight(int sphereId, const float3& pos, const float3& normal,
                       const float3& rdir, uint32_t seed, int& rayCount);

    // Fires one real production ray with an explicit scatter limit (the cache
    // is not consulted) and reports the first hit plus full transport.
    RayResult FireRay(const float3& origin, const float3& dir, int limit, uint32_t seed);

    // Runs the real DrawTest path into an external backbuffer.
    void RunDraw(int width, int height, int frame, unsigned flags,
                 std::vector<float>& backbuffer, int& outRays);

    // The real scene services bound to the cache.
    const ICScene& Scene();
}
