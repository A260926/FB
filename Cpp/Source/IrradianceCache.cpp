#include "IrradianceCache.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    uint32_t Mix32(uint32_t x)
    {
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }

    uint32_t IdentitySeed(uint32_t seed, const ICPathIdentity& id)
    {
        uint32_t h = seed | 1u;
        h = Mix32(h ^ (Mix32((uint32_t)id.frame + 0x9e3779b9u)));
        h = Mix32(h ^ (Mix32((uint32_t)id.pixelY + 0x9e3779b9u)));
        h = Mix32(h ^ (Mix32((uint32_t)id.pixelX + 0x9e3779b9u)));
        h = Mix32(h ^ (Mix32((uint32_t)id.sample + 0x9e3779b9u)));
        h = Mix32(h ^ (Mix32((uint32_t)id.depth + 0x9e3779b9u)));
        return h | 1u;
    }

    void TangentFrame(const float3& n, float3& u, float3& v)
    {
        float3 helper = fabsf(n.getX()) > 0.1f ? float3(0, 1, 0) : float3(1, 0, 0);
        u = normalize(cross(helper, n));
        v = cross(n, u);
    }

    // Cosine-weighted hemisphere sample around n; pdf = cos(theta)/pi.
    float3 CosineSampleHemisphere(const float3& n, const float3& u, const float3& v, uint32_t& state)
    {
        float r1 = RandomFloat01(state);
        float r2 = RandomFloat01(state);
        float phi = 2.0f * kPI * r1;
        float r2s = sqrtf(r2);
        float3 dir = u * (cosf(phi) * r2s) + v * (sinf(phi) * r2s) + n * sqrtf(1.0f - r2);
        return normalize(dir);
    }

    bool IsFinite(float v) { return std::isfinite(v); }
}

// ---------------------------------------------------------------------------
// IrradianceCache
// ---------------------------------------------------------------------------

IrradianceCache::IrradianceCache() = default;

bool IrradianceCache::Configure(const ICConfig& config)
{
    if (!config.enabled)
    {
        // Turning the cache off is always valid and clears everything.
        m_config = config;
        Clear();
        return true;
    }
    if (config.gatherSamples <= 0)
        return false;
    if (!(IsFinite(config.tau) && config.tau > 0.0f && config.tau <= 1.0f))
        return false;
    if (!(IsFinite(config.minRadius) && config.minRadius > 0.0f))
        return false;
    if (!(IsFinite(config.maxRadius) && config.maxRadius >= config.minRadius))
        return false;

    m_config = config;
    Clear(); // any accepted configuration change clears the records
    return true;
}

void IrradianceCache::Clear()
{
    m_records.clear();
    m_frozenCount = 0;
    for (auto& bucket : m_rowCandidates)
        bucket.clear();
    m_cells.clear();
    m_gridDim[0] = m_gridDim[1] = m_gridDim[2] = 0;
    m_haveSignature = false;
    m_lastSignature = 0;
}

void IrradianceCache::ResetStats()
{
    m_aQueries = 0;
    m_aHits = 0;
    m_aGathers = 0;
    m_aProbeRays = 0;
    m_aConnectionRays = 0;
    m_aRecordChecks = 0;
}

void IrradianceCache::GetStats(ICStats& out) const
{
    out.queries = m_aQueries.load();
    out.hits = m_aHits.load();
    out.gathers = m_aGathers.load();
    out.probeRays = m_aProbeRays.load();
    out.connectionRays = m_aConnectionRays.load();
    out.recordChecks = m_aRecordChecks.load();
    out.recordsPublished = (uint64_t)m_records.size();
    out.candidates = 0;
    for (const auto& bucket : m_rowCandidates)
        out.candidates += (uint64_t)bucket.size();
}

void IrradianceCache::BeginDraw(int frameCount, int screenHeight, uint64_t sceneSignature)
{
    (void)frameCount;
    if (m_haveSignature && sceneSignature != m_lastSignature)
    {
        // Geometry/material/light change: records must be gone before drawing.
        m_records.clear();
        m_frozenCount = 0;
        m_cells.clear();
        m_gridDim[0] = m_gridDim[1] = m_gridDim[2] = 0;
        for (auto& bucket : m_rowCandidates)
            bucket.clear();
    }
    m_haveSignature = true;
    m_lastSignature = sceneSignature;

    publishCandidates(); // previous round's candidates become the readable set
    m_rowCandidates.assign((size_t)std::max(0, screenHeight), {});
}

void IrradianceCache::EndDraw()
{
    // Candidates stay frozen; they are published at the start of the next
    // round, so this round never read its own writes.
}

void IrradianceCache::publishCandidates()
{
    for (auto& bucket : m_rowCandidates)
    {
        if (bucket.empty())
            continue;
        std::stable_sort(bucket.begin(), bucket.end(),
            [](const Candidate& a, const Candidate& b)
            {
                if (a.identity.pixelX != b.identity.pixelX)
                    return a.identity.pixelX < b.identity.pixelX;
                if (a.identity.sample != b.identity.sample)
                    return a.identity.sample < b.identity.sample;
                return a.identity.depth < b.identity.depth;
            });
        for (const Candidate& c : bucket)
            m_records.push_back(c.record);
        bucket.clear();
    }
    m_frozenCount = m_records.size();
    rebuildIndex();
}

void IrradianceCache::rebuildIndex()
{
    m_cells.clear();
    m_gridDim[0] = m_gridDim[1] = m_gridDim[2] = 0;
    if (m_records.empty() || m_config.maxRadius <= 0.0f)
        return;

    float mn[3] = {m_records[0].x.x, m_records[0].x.y, m_records[0].x.z};
    float mx = mn[0], my = mn[1], mz = mn[2];
    for (const ICRecord& r : m_records)
    {
        mn[0] = std::min(mn[0], r.x.x); mn[1] = std::min(mn[1], r.x.y); mn[2] = std::min(mn[2], r.x.z);
        mx = std::max(mx, r.x.x); my = std::max(my, r.x.y); mz = std::max(mz, r.x.z);
    }
    float maxv[3] = {mx, my, mz};
    float cell = m_config.maxRadius;
    m_gridOrigin[0] = mn[0]; m_gridOrigin[1] = mn[1]; m_gridOrigin[2] = mn[2];
    for (int k = 0; k < 3; ++k)
        m_gridDim[k] = std::max(1, (int)std::floor((maxv[k] - mn[k]) / cell) + 1);

    m_cells.resize((size_t)m_gridDim[0] * m_gridDim[1] * m_gridDim[2]);
    for (uint32_t i = 0; i < (uint32_t)m_records.size(); ++i)
    {
        const ICRecord& r = m_records[i];
        int ix = (int)std::floor((r.x.x - m_gridOrigin[0]) / cell);
        int iy = (int)std::floor((r.x.y - m_gridOrigin[1]) / cell);
        int iz = (int)std::floor((r.x.z - m_gridOrigin[2]) / cell);
        ix = std::min(std::max(ix, 0), m_gridDim[0] - 1);
        iy = std::min(std::max(iy, 0), m_gridDim[1] - 1);
        iz = std::min(std::max(iz, 0), m_gridDim[2] - 1);
        size_t idx = ((size_t)ix * m_gridDim[1] + iy) * m_gridDim[2] + iz;
        m_cells[idx].indices.push_back(i);
    }
}

void IrradianceCache::DebugRebuildIndex()
{
    m_frozenCount = m_records.size();
    rebuildIndex();
}

bool IrradianceCache::visible(ICSceneAdapter& scene, int ownerSphere,
                              const float3& a, const float3& na,
                              const float3& b, const float3& nb,
                              int& inoutRayCount)
{
    // Finite connecting segment between the two points, each lifted along
    // its own normal by kMinT. The owning sphere is ignored by marching past
    // it; every other sphere blocks the connection.
    float3 p0 = a + na * kICMinT;
    float3 p1 = b + nb * kICMinT;
    float3 origin = p0;
    for (int step = 0; step < 8; ++step)
    {
        float3 delta = p1 - origin;
        float dist = length(delta);
        if (dist <= kICMinT)
            return true;
        Ray conn(origin, delta * (1.0f / dist));
        Hit hit;
        int id;
        m_aConnectionRays.fetch_add(1, std::memory_order_relaxed);
        ++inoutRayCount;
        if (!scene.HitWorld(conn, kICMinT, dist - kICMinT, hit, id))
            return true;
        if (id != ownerSphere)
            return false;
        // The segment grazed the owning sphere; step past it and retry.
        origin = hit.pos + hit.normal * kICMinT;
    }
    return true;
}

ICGatherResult IrradianceCache::Gather(ICSceneAdapter& scene, const float3& pos,
                                       const float3& normal, int remaining,
                                       const ICPathIdentity& identity,
                                       int& inoutRayCount) const
{
    ICGatherResult out;
    out.samples.resize((size_t)m_config.gatherSamples);

    uint32_t state = IdentitySeed(m_config.seed, identity);
    float3 u, v;
    TangentFrame(normal, u, v);

    float3 sumH(0, 0, 0);
    float sumInvD = 0.0f;
    const int M = m_config.gatherSamples;
    for (int j = 0; j < M; ++j)
    {
        float3 dir = CosineSampleHemisphere(normal, u, v, state);
        Ray probe(pos + normal * kICMinT, dir);

        // The probe continues the path one hop beyond the shading point. A
        // point with `remaining` scatters left sits at global depth
        // (kICMaxDepth - remaining), so the probe starts one hop deeper and
        // can never extend the path beyond the budget.
        int probeStartDepth = kICMaxDepth - remaining + 1;
        float firstT = m_config.maxRadius; // escape is recorded as maxRadius
        float3 Li = scene.TraceProbe(probe, probeStartDepth, true,
                                     inoutRayCount, state, &firstT);
        float d = std::max(kICMinT, std::min(firstT, m_config.maxRadius));

        out.samples[j].direction = float3pack(dir);
        out.samples[j].firstDistance = d;
        out.samples[j].contribution = float3pack(Li);
        sumH += Li;
        sumInvD += 1.0f / d;
    }

    float3 H = sumH * (1.0f / (float)M);
    float R = m_config.minRadius;
    if (sumInvD > 0.0f)
        R = std::max(m_config.minRadius,
                     std::min(m_config.maxRadius, (float)M / sumInvD));

    out.H = float3pack(H);
    out.radius = R;
    return out;
}

bool IrradianceCache::DebugQuery(ICSceneAdapter& scene, int sphereId, int side,
                                 const float3& pos, const float3& normal, int remaining,
                                 float3& outH, int& outUsed, int& inoutRayCount)
{
    struct Entry { float e; float w; float3 H; };
    std::vector<Entry> entries;
    m_aQueries.fetch_add(1, std::memory_order_relaxed);

    if (m_gridDim[0] > 0)
    {
        float cell = m_config.maxRadius;
        int qx = (int)std::floor((pos.getX() - m_gridOrigin[0]) / cell);
        int qy = (int)std::floor((pos.getY() - m_gridOrigin[1]) / cell);
        int qz = (int)std::floor((pos.getZ() - m_gridOrigin[2]) / cell);
        for (int ix = std::max(0, qx - 1); ix <= std::min(m_gridDim[0] - 1, qx + 1); ++ix)
        for (int iy = std::max(0, qy - 1); iy <= std::min(m_gridDim[1] - 1, qy + 1); ++iy)
        for (int iz = std::max(0, qz - 1); iz <= std::min(m_gridDim[2] - 1, qz + 1); ++iz)
        {
            const Cell& c = m_cells[((size_t)ix * m_gridDim[1] + iy) * m_gridDim[2] + iz];
            for (uint32_t idx : c.indices)
            {
                const ICRecord& r = m_records[idx];
                // Every record the spatial index hands to the query is checked.
                m_aRecordChecks.fetch_add(1, std::memory_order_relaxed);
                if (r.sphereId != sphereId || r.side != side || r.remaining != remaining)
                    continue; // key isolation: sphere, face, remaining depth
                float3 rx = r.x.toFloat3();
                float3 rn = r.n.toFloat3();
                float nd = dot(normal, rn);
                float e = length(pos - rx) / r.R + sqrtf(std::max(0.0f, 1.0f - nd));
                if (!(e < m_config.tau))
                    continue;
                if (!visible(scene, sphereId, pos, normal, rx, rn, inoutRayCount))
                    continue;
                float w = e > 0.0f ? (1.0f / e - 1.0f / m_config.tau) : 0.0f;
                entries.push_back(Entry{e, w, r.H.toFloat3()});
            }
        }
    }

    if (entries.empty())
        return false;

    bool anyZero = false;
    for (const Entry& e : entries) anyZero = anyZero || (e.e == 0.0f);

    float3 acc(0, 0, 0);
    float wsum = 0.0f;
    for (const Entry& e : entries)
    {
        if (anyZero && e.e != 0.0f)
            continue; // zero-error records alone are averaged
        float w = anyZero ? 1.0f : e.w;
        acc += e.H * w;
        wsum += w;
    }
    outH = acc * (1.0f / wsum);
    outUsed = 0;
    for (const Entry& e : entries)
        if (!anyZero || e.e == 0.0f)
            ++outUsed;
    m_aHits.fetch_add(1, std::memory_order_relaxed);
    return true;
}

float3 IrradianceCache::LookupOrGather(ICSceneAdapter& scene, int sphereId, int side,
                                       const float3& pos, const float3& normal,
                                       int remaining, const ICPathIdentity& identity,
                                       int& inoutRayCount)
{
    float3 H;
    int used = 0;
    if (DebugQuery(scene, sphereId, side, pos, normal, remaining, H, used, inoutRayCount))
        return H;

    // No usable record: run a real gather and stage a candidate for next round.
    m_aGathers.fetch_add(1, std::memory_order_relaxed);
    ICGatherResult g = Gather(scene, pos, normal, remaining, identity, inoutRayCount);
    m_aProbeRays.fetch_add((uint64_t)m_config.gatherSamples, std::memory_order_relaxed);

    Candidate cand;
    cand.record.sphereId = sphereId;
    cand.record.side = side;
    cand.record.x = float3pack(pos);
    cand.record.n = float3pack(normal);
    cand.record.remaining = remaining;
    cand.record.H = g.H;
    cand.record.R = g.radius;
    cand.identity = identity;

    int row = std::min(std::max(identity.pixelY, 0),
                       (int)m_rowCandidates.size() - 1);
    if (!m_rowCandidates.empty())
        m_rowCandidates[(size_t)row].push_back(cand);
    return g.H.toFloat3();
}

void IrradianceCache::DebugInjectRecord(const ICRecord& record)
{
    m_records.push_back(record);
    m_frozenCount = m_records.size();
    rebuildIndex();
}

// ---------------------------------------------------------------------------
// Self-contained transport world (production integrator, no second one)
// ---------------------------------------------------------------------------

struct ICWorld
{
    int capacity;
    int count = 0;
    std::vector<Sphere> spheres;
    std::vector<ICMaterial> materials;
    std::vector<int> emissives;
    SpheresSoA* soa = nullptr;
    int skyMode = 0;
    float3pack constantSky{0.2f, 0.2f, 0.2f};
    uint64_t signature = 0;

    struct Adapter;
    Adapter* adapter = nullptr;

    ICWorld(int cap)
        : capacity(cap)
        , spheres((size_t)cap)
        , materials((size_t)cap)
    {}
    ~ICWorld();

    float3 sky(const Ray& r) const
    {
        if (skyMode == 1)
            return constantSky.toFloat3();
        float t = 0.5f * (r.dir.getY() + 1.0f);
        return ((1.0f - t) * float3(1, 1, 1) + t * float3(0.5f, 0.7f, 1.0f)) * 0.3f;
    }

    bool hitWorld(const Ray& r, float tMin, float tMax, Hit& outHit, int& outId)
    {
        outId = HitSpheres(r, *soa, tMin, tMax, outHit);
        return outId != -1;
    }

    // Mirrors Test.cpp Scatter exactly: same branches, same RNG consumption.
    bool scatter(const ICMaterial& mat, const Ray& rIn, const Hit& rec,
                 float3& attenuation, Ray& scattered, float3& outLightE,
                 int& rayCount, uint32_t& state)
    {
        outLightE = float3(0, 0, 0);
        if (mat.type == kICLambert)
        {
            float3 target = rec.pos + rec.normal + RandomUnitVector(state);
            scattered = Ray(rec.pos, normalize(target - rec.pos));
            float3 matAlbedo = mat.albedo.toFloat3();
            attenuation = matAlbedo;
            for (int j = 0; j < (int)emissives.size(); ++j)
            {
                int i = emissives[(size_t)j];
                const ICMaterial& smat = materials[(size_t)i];
                if (&mat == &smat)
                    continue;
                const Sphere& s = spheres[(size_t)i];
                float3 sc = s.center.toFloat3();
                float3 sw = normalize(sc - rec.pos);
                float3 su = normalize(cross(fabsf(sw.getX()) > 0.01f ? float3(0, 1, 0) : float3(1, 0, 0), sw));
                float3 sv = cross(sw, su);
                float cosAMax = sqrtf(1.0f - s.radius * s.radius / sqLength(rec.pos - sc));
                float eps1 = RandomFloat01(state), eps2 = RandomFloat01(state);
                float cosA = 1.0f - eps1 + eps1 * cosAMax;
                float sinA = sqrtf(1.0f - cosA * cosA);
                float phi = 2 * kPI * eps2;
                float3 l = su * (cosf(phi) * sinA) + sv * (sinf(phi) * sinA) + sw * cosA;
                Hit lightHit;
                int hitId;
                ++rayCount;
                if (hitWorld(Ray(rec.pos, l), kICMinT, kICMaxT, lightHit, hitId) && hitId == i)
                {
                    float omega = 2 * kPI * (1 - cosAMax);
                    float3 nl = dot(rec.normal, rIn.dir) < 0 ? rec.normal : -rec.normal;
                    outLightE += (matAlbedo * smat.emissive.toFloat3()) *
                                 (std::max(0.0f, dot(l, nl)) * omega / kPI);
                }
            }
            return true;
        }
        else if (mat.type == kICMetal)
        {
            float3 refl = reflect(rIn.dir, rec.normal);
            scattered = Ray(rec.pos, normalize(refl + mat.roughness * RandomInUnitSphere(state)));
            attenuation = mat.albedo.toFloat3();
            return dot(scattered.dir, rec.normal) > 0;
        }
        else
        {
            float3 outwardN;
            float3 rdir = rIn.dir;
            float3 refl = reflect(rdir, rec.normal);
            float nint;
            attenuation = float3(1, 1, 1);
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
                reflProb = schlick(cosine, mat.ri);
            else
                reflProb = 1;
            if (RandomFloat01(state) < reflProb)
                scattered = Ray(rec.pos, normalize(refl));
            else
                scattered = Ray(rec.pos, normalize(refr));
            return true;
        }
    }

    float3 sampleLights(const Hit& rec, const Ray& rIn, int selfId,
                        const float3& albedo, int& rayCount, uint32_t& state)
    {
        float3 e(0, 0, 0);
        for (int j = 0; j < (int)emissives.size(); ++j)
        {
            int i = emissives[(size_t)j];
            if (i == selfId)
                continue;
            const ICMaterial& smat = materials[(size_t)i];
            const Sphere& s = spheres[(size_t)i];
            float3 sc = s.center.toFloat3();
            float3 sw = normalize(sc - rec.pos);
            float3 su = normalize(cross(fabsf(sw.getX()) > 0.01f ? float3(0, 1, 0) : float3(1, 0, 0), sw));
            float3 sv = cross(sw, su);
            float cosAMax = sqrtf(1.0f - s.radius * s.radius / sqLength(rec.pos - sc));
            float eps1 = RandomFloat01(state), eps2 = RandomFloat01(state);
            float cosA = 1.0f - eps1 + eps1 * cosAMax;
            float sinA = sqrtf(1.0f - cosA * cosA);
            float phi = 2 * kPI * eps2;
            float3 l = su * (cosf(phi) * sinA) + sv * (sinf(phi) * sinA) + sw * cosA;
            Hit lightHit;
            int hitId;
            ++rayCount;
            if (hitWorld(Ray(rec.pos, l), kICMinT, kICMaxT, lightHit, hitId) && hitId == i)
            {
                float omega = 2 * kPI * (1 - cosAMax);
                float3 nl = dot(rec.normal, rIn.dir) < 0 ? rec.normal : -rec.normal;
                e += (albedo * smat.emissive.toFloat3()) *
                     (std::max(0.0f, dot(l, nl)) * omega / kPI);
            }
        }
        return e;
    }

    // Production recursive transport (cache always off).
    float3 trace(const Ray& r, int depth, int& rayCount, uint32_t& state, bool doMaterialE)
    {
        Hit rec;
        int id;
        ++rayCount;
        if (hitWorld(r, kICMinT, kICMaxT, rec, id))
        {
            Ray scattered;
            float3 attenuation, lightE;
            const ICMaterial& mat = materials[(size_t)id];
            float3 matE = mat.emissive.toFloat3();
            if (depth < kICMaxDepth && scatter(mat, r, rec, attenuation, scattered, lightE, rayCount, state))
            {
                if (!doMaterialE)
                    matE = float3(0, 0, 0);
                bool nextE = (mat.type != kICLambert);
                return matE + lightE + attenuation * trace(scattered, depth + 1, rayCount, state, nextE);
            }
            return matE;
        }
        return sky(r);
    }

    // Probe transport: a direct first hit on an emissive sphere contributes
    // nothing (the current shading point counts that light once via explicit
    // sampling); the sky is always kept. Deeper bounces keep the production
    // emission attribution rules.
    float3 traceProbe(const Ray& r, int depth, bool firstSegment,
                      int& rayCount, uint32_t& state, float* outFirstT)
    {
        Hit rec;
        int id;
        ++rayCount;
        if (hitWorld(r, kICMinT, kICMaxT, rec, id))
        {
            if (outFirstT)
                *outFirstT = std::max(kICMinT, std::min(rec.t, 1.0e30f));
            const ICMaterial& mat = materials[(size_t)id];
            bool emissive = mat.emissive.x > 0.0f || mat.emissive.y > 0.0f || mat.emissive.z > 0.0f;
            if (firstSegment && emissive)
                return float3(0, 0, 0); // strip direct first-hit self-emission
            float3 matE = emissive ? mat.emissive.toFloat3() : float3(0, 0, 0);
            Ray scattered;
            float3 attenuation, lightE;
            if (depth < kICMaxDepth && scatter(mat, r, rec, attenuation, scattered, lightE, rayCount, state))
                return matE + lightE + attenuation * traceProbe(scattered, depth + 1, false, rayCount, state, nullptr);
            return matE;
        }
        if (outFirstT)
            *outFirstT = 1.0e30f; // callers clamp escapes to maxRadius
        return sky(r);
    }
};

struct ICWorld::Adapter : ICSceneAdapter
{
    ICWorld* w;
    explicit Adapter(ICWorld* world) : w(world) {}

    bool HitWorld(const Ray& r, float tMin, float tMax, Hit& outHit, int& outId) override
    {
        return w->hitWorld(r, tMin, tMax, outHit, outId);
    }
    float3 Sky(const Ray& r) override { return w->sky(r); }
    float3 TraceProbe(const Ray& r, int depth, bool firstSegment, int& rayCount,
                      uint32_t& state, float* outFirstT) override
    {
        return w->traceProbe(r, depth, firstSegment, rayCount, state, outFirstT);
    }
    int MaterialType(int id) override { return (int)w->materials[(size_t)id].type; }
    float3 MaterialAlbedo(int id) override { return w->materials[(size_t)id].albedo.toFloat3(); }
    float3 MaterialEmissive(int id) override { return w->materials[(size_t)id].emissive.toFloat3(); }
    float3 SampleLights(const Hit& rec, const Ray& rIn, int selfId,
                        const float3& albedo, int& rayCount, uint32_t& state) override
    {
        return w->sampleLights(rec, rIn, selfId, albedo, rayCount, state);
    }
    uint64_t SceneSignature() override { return w->signature; }
};

ICWorld::~ICWorld()
{
    delete soa;
    delete adapter;
}

ICWorld* ICWorld_Create(int maxSpheres)
{
    ICWorld* w = new ICWorld(maxSpheres);
    w->adapter = new ICWorld::Adapter(w);
    return w;
}

void ICWorld_Release(ICWorld* world) { delete world; }

void ICWorld_SetSphere(ICWorld* world, int id, const float3& center, float radius)
{
    world->spheres[(size_t)id] = Sphere(center, radius);
    world->count = std::max(world->count, id + 1);
}

void ICWorld_SetMaterial(ICWorld* world, int id, const ICMaterial& material)
{
    world->materials[(size_t)id] = material;
}

void ICWorld_SetSky(ICWorld* world, int skyMode, const float3& constantSky)
{
    world->skyMode = skyMode;
    world->constantSky = float3pack(constantSky);
}

void ICWorld_Finish(ICWorld* world)
{
    delete world->soa;
    world->soa = new SpheresSoA(world->capacity);
    world->emissives.clear();
    uint64_t h = 1469598103934665603ull;
    auto fold = [&](const void* p, size_t n)
    {
        const unsigned char* b = (const unsigned char*)p;
        for (size_t i = 0; i < n; ++i)
        {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    for (int i = 0; i < world->count; ++i)
    {
        Sphere& s = world->spheres[(size_t)i];
        s.UpdateDerivedData();
        world->soa->centerX[i] = s.center.x;
        world->soa->centerY[i] = s.center.y;
        world->soa->centerZ[i] = s.center.z;
        world->soa->sqRadius[i] = s.radius * s.radius;
        world->soa->invRadius[i] = s.invRadius;
        const ICMaterial& m = world->materials[(size_t)i];
        fold(&s.center.x, sizeof(float) * 3);
        fold(&s.radius, sizeof(float));
        fold(&m, sizeof(m));
        if (m.emissive.x > 0 || m.emissive.y > 0 || m.emissive.z > 0)
            world->emissives.push_back(i);
    }
    int skyMode = world->skyMode;
    fold(&skyMode, sizeof(skyMode));
    float3pack sky = world->constantSky;
    fold(&sky, sizeof(sky));
    world->signature = h;
}

int ICWorld_SphereCount(ICWorld* world) { return world->count; }
const Sphere* ICWorld_GetSpheres(ICWorld* world) { return world->spheres.data(); }
const ICMaterial* ICWorld_GetMaterials(ICWorld* world) { return world->materials.data(); }
ICSceneAdapter* ICWorld_GetAdapter(ICWorld* world) { return world->adapter; }

float3 ICWorld_TraceRay(ICWorld* world, const Ray& r, int depth,
                        int& inoutRayCount, uint32_t& state)
{
    return world->trace(r, depth, inoutRayCount, state, true);
}

float3 ICWorld_SampleLights(ICWorld* world, const Hit& rec, const Ray& rIn,
                            int selfId, const float3& albedo,
                            int& inoutRayCount, uint32_t& state)
{
    return world->sampleLights(rec, rIn, selfId, albedo, inoutRayCount, state);
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

IrradianceCache& GetIrradianceCache()
{
    static IrradianceCache s_cache;
    return s_cache;
}

bool ConfigureIrradianceCache(const ICConfig& config)
{
    return GetIrradianceCache().Configure(config);
}

void ClearIrradianceCache()
{
    GetIrradianceCache().Clear();
}

void GetIrradianceCacheStats(ICStats& out)
{
    GetIrradianceCache().GetStats(out);
}

size_t GetIrradianceCacheRecordCount()
{
    return GetIrradianceCache().GetRecordCount();
}

const ICRecord* GetIrradianceCacheRecords(size_t& outCount)
{
    IrradianceCache& c = GetIrradianceCache();
    outCount = c.GetRecordCount();
    return c.GetRecords();
}
