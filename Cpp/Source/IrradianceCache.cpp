#include "Cpp/Source/IrradianceCache.h"
#include <algorithm>
#include <cmath>

namespace
{
    // 21-bit non-negative cell coordinates (bias keeps scene coordinates in
    // range), packed without hash collisions.
    constexpr int kCellBias = 1 << 20;
    constexpr uint64_t kCellMask = (1ULL << 21) - 1;

    uint64_t CellKey(int cx, int cy, int cz)
    {
        uint64_t ux = static_cast<uint64_t>(cx + kCellBias) & kCellMask;
        uint64_t uy = static_cast<uint64_t>(cy + kCellBias) & kCellMask;
        uint64_t uz = static_cast<uint64_t>(cz + kCellBias) & kCellMask;
        return (ux << 42) | (uy << 21) | uz;
    }

    // Deterministic per-vertex random stream. Depends only on the configured
    // seed and the stable path identity, never on thread or insertion order.
    uint32_t Mix32(uint32_t x)
    {
        x ^= x >> 16;
        x *= 0x7feb352dU;
        x ^= x >> 15;
        x *= 0x846ca68bU;
        x ^= x >> 16;
        return x;
    }

    bool FinitePositive(float v) { return std::isfinite(v) && v > 0.0f; }
}

IrradianceCache::IrradianceCache() = default;

bool IrradianceCache::Configure(const ICConfig& cfg)
{
    if (cfg.gatherSamples <= 0)
        return false;
    if (!std::isfinite(cfg.tau) || cfg.tau <= 0.0f || cfg.tau > 1.0f)
        return false;
    if (!FinitePositive(cfg.minRadius) || !FinitePositive(cfg.maxRadius))
        return false;
    if (!(cfg.minRadius <= cfg.maxRadius))
        return false;

    m_config = cfg;
    m_configured = true;
    Clear();
    return true;
}

void IrradianceCache::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutationMutex);
    m_published.clear();
    m_indexRecords.clear();
    m_cells.clear();
    m_gatherLog.clear();
    m_indexDirty = true;
    m_stats = ICStats();
}

uint32_t IrradianceCache::GatherRng(const ICConfig& cfg, const ICPathId& id)
{
    uint32_t h = Mix32(cfg.seed ^ 0x9e3779b9U);
    h = Mix32(h ^ Mix32(id.frame + 0x1234567U));
    h = Mix32(h ^ Mix32(id.pixel + 0x3456789U));
    h = Mix32(h ^ Mix32(id.sample + 0x56789abU));
    h = Mix32(h ^ Mix32(id.pathDepth + 0x789abcdU));
    return h | 1u;
}

void IrradianceCache::BuildIndex()
{
    m_indexRecords = m_published;
    m_cells.clear();
    const float cellSize = m_config.maxRadius;
    for (int i = 0; i < static_cast<int>(m_indexRecords.size()); ++i)
    {
        const float3& x = m_indexRecords[i].x;
        int cx = static_cast<int>(std::floor(x.getX() / cellSize));
        int cy = static_cast<int>(std::floor(x.getY() / cellSize));
        int cz = static_cast<int>(std::floor(x.getZ() / cellSize));
        uint64_t key = CellKey(cx, cy, cz);

        auto it = std::lower_bound(m_cells.begin(), m_cells.end(), key,
            [](const Cell& c, uint64_t k) { return c.key < k; });
        if (it != m_cells.end() && it->key == key)
        {
            it->records.push_back(i);
        }
        else
        {
            Cell cell;
            cell.key = key;
            cell.records.push_back(i);
            m_cells.insert(it, std::move(cell));
        }
    }

    m_indexDirty = false;
}

void IrradianceCache::BeginFrame(uint32_t frame)
{
    std::lock_guard<std::mutex> lock(m_mutationMutex);
    m_frame = frame;
    if (m_indexDirty)
        BuildIndex();
}

bool IrradianceCache::Visible(const float3& x, const float3& n, const IRecord& r, const ICScene& scene) const
{
    // Offset both ends along their own outward normals before the finite
    // connector test.
    float3 p = x + n * kMinT;
    float3 pr = r.x + r.n * kMinT;
    float3 delta = pr - p;
    float segLen = length(delta);
    if (segLen <= 2.0f * kMinT)
        return true;
    float3 dir = delta * (1.0f / segLen);
    return !scene.occluded(p, dir, segLen, r.sphereId);
}

float3 IrradianceCache::QueryOrGather(const IRecord& query, const ICPathId& pathId, const ICScene& scene,
                                      int& rayCount, ICCandidate& outCandidate, bool& outWasMiss)
{
    // Local counters keep the concurrent lookup lock-free; they are merged
    // into the shared statistics once, under the mutation lock.
    uint64_t stQueries = 1, stHits = 0, stGathers = 0, stProbes = 0;
    uint64_t stConnChecks = 0, stConnBlocked = 0, stExamined = 0;

    struct Weighted
    {
        int index;
        float e;
    };
    // Grows only for an unusually dense neighbourhood, so a usable record is
    // never silently dropped.
    std::vector<Weighted> candidates;
    candidates.reserve(32);
    int zeroCount = 0;

    if (!m_indexDirty)
    {
        const float cellSize = m_config.maxRadius;
        int qx = static_cast<int>(std::floor(query.x.getX() / cellSize));
        int qy = static_cast<int>(std::floor(query.x.getY() / cellSize));
        int qz = static_cast<int>(std::floor(query.x.getZ() / cellSize));

        // A usable record is less than tau * Rr <= maxRadius away, so the
        // 3x3x3 cell neighbourhood around the query is exhaustive.
        for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
            uint64_t key = CellKey(qx + dx, qy + dy, qz + dz);
            auto it = std::lower_bound(m_cells.begin(), m_cells.end(), key,
                [](const Cell& c, uint64_t k) { return c.key < k; });
            if (it == m_cells.end() || it->key != key)
                continue;
            for (int idx : it->records)
            {
                const IRecord& r = m_indexRecords[idx];
                ++stExamined;
                if (r.sphereId != query.sphereId || r.side != query.side ||
                    r.remainingDepth != query.remainingDepth)
                    continue;

                float dist = length(query.x - r.x);
                float nd = dot(query.n, r.n);
                float e = dist / r.R + sqrtf(std::max(0.0f, 1.0f - nd));
                if (!(e < m_config.tau))
                    continue;

                ++stConnChecks;
                if (!Visible(query.x, query.n, r, scene))
                {
                    ++stConnBlocked;
                    continue;
                }

                candidates.push_back(Weighted{idx, e});
                if (e == 0.0f)
                    ++zeroCount;
            }
        }
    }

    if (!candidates.empty())
    {
        ++stHits;
        outWasMiss = false;

        float3 result;
        if (zeroCount > 0)
        {
            // Any exact-match record dominates: average only zero-error ones.
            float3 acc(0, 0, 0);
            int used = 0;
            for (const Weighted& w : candidates)
            {
                if (w.e != 0.0f)
                    continue;
                acc += m_indexRecords[w.index].H;
                ++used;
            }
            result = acc * (1.0f / static_cast<float>(used));
        }
        else
        {
            float3 acc(0, 0, 0);
            float wSum = 0.0f;
            for (const Weighted& w : candidates)
            {
                const IRecord& r = m_indexRecords[w.index];
                float weight = 1.0f / w.e - 1.0f / m_config.tau;
                acc += r.H * weight;
                wSum += weight;
            }
            result = acc * (1.0f / wSum);
        }

        std::lock_guard<std::mutex> lock(m_mutationMutex);
        m_stats.queries += stQueries;
        m_stats.cacheHits += stHits;
        m_stats.connectionChecks += stConnChecks;
        m_stats.connectionBlocked += stConnBlocked;
        m_stats.recordsExamined += stExamined;
        return result;
    }

    // Miss: gather with the real scene transport, cache internally disabled.
    ++stGathers;
    outWasMiss = true;

    uint32_t rng = GatherRng(m_config, pathId);

    // Orthonormal frame around the light-facing normal.
    float3 axis = std::fabs(query.n.getX()) > 0.01f ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 u = normalize(cross(axis, query.n));
    float3 v = cross(query.n, u);

    const int M = m_config.gatherSamples;
    float3 sumLi(0, 0, 0);
    float invDSum = 0.0f;

    ICGatherDebug dbg;
    dbg.sphereId = query.sphereId;
    dbg.side = query.side;
    dbg.remainingDepth = query.remainingDepth;
    dbg.x = query.x;
    dbg.n = query.n;
    dbg.samples.reserve(M);

    float3 origin = query.x + query.n * kMinT;
    for (int j = 0; j < M; ++j)
    {
        float r1 = RandomFloat01(rng);
        float r2 = RandomFloat01(rng);
        float root = sqrtf(r1);
        float phi = 2.0f * kPI * r2;
        float3 dir = u * (cosf(phi) * root) + v * (sinf(phi) * root) +
                     query.n * sqrtf(std::max(0.0f, 1.0f - r1));

        ICProbeInfo info;
        float3 Li = scene.probeLi(origin, dir, query.remainingDepth, rng, rayCount, info);
        ++stProbes;

        // First segment only: clamped to kMinT, truncated at maxRadius; an
        // escaping probe counts as maxRadius. Later bounces never replace it.
        float d = info.hit ? std::min(std::max(info.firstDist, kMinT), m_config.maxRadius)
                           : m_config.maxRadius;

        sumLi += Li;
        invDSum += 1.0f / d;
        dbg.samples.push_back(ICGatherSample{dir, d, Li});
    }

    float3 H = sumLi * (1.0f / static_cast<float>(M));
    float R = std::max(m_config.minRadius,
                std::min(m_config.maxRadius, static_cast<float>(M) / invDSum));

    dbg.H = H;
    dbg.R = R;

    IRecord rec = query;
    rec.H = H;
    rec.R = R;

    outCandidate.id = pathId;
    outCandidate.record = rec;

    {
        std::lock_guard<std::mutex> lock(m_mutationMutex);
        m_stats.queries += stQueries;
        m_stats.gathers += stGathers;
        m_stats.probeRays += stProbes;
        m_stats.recordsExamined += stExamined;
        m_stats.connectionChecks += stConnChecks;
        m_stats.connectionBlocked += stConnBlocked;
        m_gatherLog.push_back(std::move(dbg));
    }
    return H;
}

void IrradianceCache::PublishCandidates(std::vector<ICCandidate>& rowCandidates, uint32_t frame)
{
    if (rowCandidates.empty())
        return;

    std::stable_sort(rowCandidates.begin(), rowCandidates.end(),
        [](const ICCandidate& a, const ICCandidate& b)
        {
            const ICPathId& x = a.id;
            const ICPathId& y = b.id;
            if (x.frame != y.frame) return x.frame < y.frame;
            if (x.pixel != y.pixel) return x.pixel < y.pixel;
            if (x.sample != y.sample) return x.sample < y.sample;
            return x.pathDepth < y.pathDepth;
        });

    std::lock_guard<std::mutex> lock(m_mutationMutex);
    for (const ICCandidate& c : rowCandidates)
    {
        (void)frame;
        m_published.push_back(c.record);
        ++m_stats.candidatesPublished;
    }
    m_indexDirty = true;
    rowCandidates.clear();
}

void IrradianceCache::TestInjectRecord(const IRecord& record)
{
    std::lock_guard<std::mutex> lock(m_mutationMutex);
    m_published.push_back(record);
    m_indexDirty = true;
}

void IrradianceCache::TestBuildSpatialIndex()
{
    std::lock_guard<std::mutex> lock(m_mutationMutex);
    BuildIndex();
}

ICStats IrradianceCache::GetStats() const
{
    // Called between rounds in practice; the lock still makes the read race
    // free against concurrent row-task merges.
    std::lock_guard<std::mutex> lock(m_mutationMutex);
    return m_stats;
}

void IrradianceCache::ResetStats()
{
    std::lock_guard<std::mutex> lock(m_mutationMutex);
    m_stats = ICStats();
}

IrradianceCache& GetIrradianceCache()
{
    static IrradianceCache s_cache;
    return s_cache;
}
