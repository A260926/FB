// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file nanovdb/tools/FogAdjoint.cc

    \brief Implementation of the CPU fog forward preview and its analytic
           vector-Jacobian product. See FogAdjoint.h and README.md for the
           interface contract.
*/

#include <nanovdb/tools/FogAdjoint.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace nanovdb {
namespace tools {
namespace fog {

namespace {

// --------------------------------------------------------------------------------------
// small helpers
// --------------------------------------------------------------------------------------

inline bool finite3(const Vec3d& v)
{
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

[[noreturn]] inline void throwInvalid(const char* what)
{
    throw std::invalid_argument(what);
}

/// @brief 3D integer key for the lookup maps (works for negative coordinates).
struct Key3 {
    int32_t v[3];
    bool operator==(const Key3& o) const { return v[0] == o.v[0] && v[1] == o.v[1] && v[2] == o.v[2]; }
};

struct Key3Hash {
    size_t operator()(const Key3& k) const
    {
        // splitmix64 over the three 32-bit lanes
        uint64_t h = 0x9e3779b97f4a7c15ull;
        for (int i = 0; i < 3; ++i) {
            uint64_t x = static_cast<uint32_t>(k.v[i]) + h;
            x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
            x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
            h = x ^ (x >> 31);
        }
        return static_cast<size_t>(h);
    }
};

inline Key3 makeKey(int32_t x, int32_t y, int32_t z)
{
    Key3 k{{x, y, z}};
    return k;
}

/// @brief Edge length (in index cells) of a value region at the given level.
inline int32_t levelEdge(int level)
{
    // level 0 -> 1 (voxel), 1 -> 8, 2 -> 128, 3 -> 4096
    return int32_t(1) << (level == 0 ? 0 : (level == 1 ? 3 : (level == 2 ? 7 : 12)));
}

} // unnamed namespace

// --------------------------------------------------------------------------------------
// Catalog
// --------------------------------------------------------------------------------------

class Catalog::Impl
{
public:
    // map: world = scale * index + translate
    double scale = 1.0;
    Vec3d translate{0.0, 0.0, 0.0};

    // parameter catalog, sorted by (level, origin.x, origin.y, origin.z)
    std::vector<int> levels;
    std::vector<Coord> origins;
    std::vector<double> initial;

    // corner lookup: leaf origin -> dense per-voxel parameter indices (-1 = inactive)
    struct LeafBlock {
        std::array<int32_t, 512> param;
    };
    std::unordered_map<Key3, uint32_t, Key3Hash> leafMap;
    std::vector<LeafBlock> leaves;
    // active constant tiles per level (1,2,3): tile origin -> parameter index
    std::unordered_map<Key3, int32_t, Key3Hash> tileMap[3];

    // support hierarchy: BVH over the trilinear influence boxes of all parameters
    struct Box {
        double lo[3], hi[3]; // half-open influence box in index coordinates
    };
    struct BVHNode {
        double lo[3], hi[3];
        int32_t left = -1, right = -1; // child node indices, -1 for leaves
        uint32_t begin = 0, end = 0;   // leaf range into order
    };
    std::vector<Box> boxes;
    std::vector<uint32_t> order;
    std::vector<BVHNode> nodes;

    /// @brief Resolve the parameter index of the sample node at integer
    ///        coordinate (x,y,z); -1 when the node is inactive or missing.
    int32_t resolve(int32_t x, int32_t y, int32_t z, uint64_t& visits) const
    {
        const Key3 leafKey = makeKey(x & ~7, y & ~7, z & ~7);
        ++visits;
        auto lit = leafMap.find(leafKey);
        if (lit != leafMap.end()) {
            const LeafBlock& lb = leaves[lit->second];
            return lb.param[static_cast<uint32_t>(((x & 7) << 6) | ((y & 7) << 3) | (z & 7))];
        }
        ++visits;
        auto t1 = tileMap[0].find(leafKey); // level-1 tiles have edge 8 as well
        if (t1 != tileMap[0].end()) return t1->second;
        ++visits;
        auto t2 = tileMap[1].find(makeKey(x & ~127, y & ~127, z & ~127));
        if (t2 != tileMap[1].end()) return t2->second;
        ++visits;
        auto t3 = tileMap[2].find(makeKey(x & ~4095, y & ~4095, z & ~4095));
        if (t3 != tileMap[2].end()) return t3->second;
        return -1;
    }

    uint32_t buildBVH(uint32_t begin, uint32_t end)
    {
        BVHNode node;
        for (int a = 0; a < 3; ++a) {
            node.lo[a] = boxes[order[begin]].lo[a];
            node.hi[a] = boxes[order[begin]].hi[a];
        }
        for (uint32_t i = begin + 1; i < end; ++i) {
            for (int a = 0; a < 3; ++a) {
                node.lo[a] = std::min(node.lo[a], boxes[order[i]].lo[a]);
                node.hi[a] = std::max(node.hi[a], boxes[order[i]].hi[a]);
            }
        }
        const uint32_t index = static_cast<uint32_t>(nodes.size());
        nodes.push_back(node);
        if (end - begin <= 4) {
            nodes[index].begin = begin;
            nodes[index].end = end;
            return index;
        }
        // split on the axis with the largest centroid spread
        double cLo[3] = {0, 0, 0}, cHi[3] = {0, 0, 0};
        for (int a = 0; a < 3; ++a) {
            cLo[a] = cHi[a] = 0.5 * (boxes[order[begin]].lo[a] + boxes[order[begin]].hi[a]);
        }
        for (uint32_t i = begin + 1; i < end; ++i) {
            for (int a = 0; a < 3; ++a) {
                const double c = 0.5 * (boxes[order[i]].lo[a] + boxes[order[i]].hi[a]);
                cLo[a] = std::min(cLo[a], c);
                cHi[a] = std::max(cHi[a], c);
            }
        }
        int axis = 0;
        double best = -1.0;
        for (int a = 0; a < 3; ++a) {
            const double ext = cHi[a] - cLo[a];
            if (ext > best) { best = ext; axis = a; }
        }
        const uint32_t mid = begin + (end - begin) / 2;
        std::nth_element(order.begin() + begin, order.begin() + mid, order.begin() + end,
                         [&](uint32_t i, uint32_t j) {
                             return 0.5 * (boxes[i].lo[axis] + boxes[i].hi[axis]) <
                                    0.5 * (boxes[j].lo[axis] + boxes[j].hi[axis]);
                         });
        nodes[index].left = static_cast<int32_t>(buildBVH(begin, mid));
        nodes[index].right = static_cast<int32_t>(buildBVH(mid, end));
        return index;
    }

    void finishBuild()
    {
        const uint32_t n = static_cast<uint32_t>(boxes.size());
        order.resize(n);
        std::iota(order.begin(), order.end(), 0u);
        if (n) buildBVH(0, n);
    }

    /// @brief Intersect the index-space ray oi + t*di, t in [t0,t1], with all
    ///        influence boxes and append the merged hit intervals (t space).
    void rayIntervals(const double* oi, const double* di, double t0, double t1,
                      std::vector<std::pair<double, double>>& intervals, uint64_t& visits) const
    {
        if (nodes.empty()) return;
        std::vector<std::pair<double, double>> hits;
        std::vector<uint32_t> stack;
        stack.reserve(64);
        stack.push_back(0);
        auto slabTest = [&](const double* lo, const double* hi, double& ta, double& tb) {
            ta = t0;
            tb = t1;
            for (int a = 0; a < 3; ++a) {
                if (di[a] == 0.0) {
                    if (oi[a] < lo[a] || oi[a] > hi[a]) return false;
                    continue;
                }
                const double inv = 1.0 / di[a];
                double u0 = (lo[a] - oi[a]) * inv;
                double u1 = (hi[a] - oi[a]) * inv;
                if (u0 > u1) std::swap(u0, u1);
                ta = std::max(ta, u0);
                tb = std::min(tb, u1);
                if (ta > tb) return false;
            }
            return true;
        };
        while (!stack.empty()) {
            const uint32_t ni = stack.back();
            stack.pop_back();
            ++visits;
            const BVHNode& node = nodes[ni];
            double ta, tb;
            if (!slabTest(node.lo, node.hi, ta, tb)) continue;
            if (node.left < 0) {
                for (uint32_t i = node.begin; i < node.end; ++i) {
                    ++visits;
                    const Box& b = boxes[order[i]];
                    double ba, bb;
                    if (slabTest(b.lo, b.hi, ba, bb) && ba < bb) hits.emplace_back(ba, bb);
                }
            } else {
                stack.push_back(static_cast<uint32_t>(node.left));
                stack.push_back(static_cast<uint32_t>(node.right));
            }
        }
        if (hits.empty()) return;
        std::sort(hits.begin(), hits.end());
        double curA = hits[0].first, curB = hits[0].second;
        for (size_t i = 1; i < hits.size(); ++i) {
            if (hits[i].first <= curB) {
                curB = std::max(curB, hits[i].second);
            } else {
                intervals.emplace_back(curA, curB);
                curA = hits[i].first;
                curB = hits[i].second;
            }
        }
        intervals.emplace_back(curA, curB);
    }
};

namespace {
struct RawParam {
    int level;
    Coord origin;
    double value;
    bool operator<(const RawParam& o) const
    {
        if (level != o.level) return level < o.level;
        if (origin[0] != o.origin[0]) return origin[0] < o.origin[0];
        if (origin[1] != o.origin[1]) return origin[1] < o.origin[1];
        return origin[2] < o.origin[2];
    }
};
} // unnamed namespace

Catalog::Catalog(const FloatGrid& grid)
    : mImpl(new Impl)
{
    // ---- validate the map: positive uniform scale + translation only ----
    const Map& map = grid.map();
    const double* M = map.mMatD;
    const double* T = map.mVecD;
    const double s = M[0];
    if (!std::isfinite(s) || s <= 0.0 || M[4] != s || M[8] != s)
        throwInvalid("fog::Catalog: grid map must be a positive uniform scale plus translation");
    for (int i = 0; i < 9; ++i) {
        if (i == 0 || i == 4 || i == 8) continue;
        if (M[i] != 0.0)
            throwInvalid("fog::Catalog: grid map must be a positive uniform scale plus translation");
    }
    if (!std::isfinite(T[0]) || !std::isfinite(T[1]) || !std::isfinite(T[2]))
        throwInvalid("fog::Catalog: grid map translation must be finite");
    mImpl->scale = s;
    mImpl->translate = Vec3d(T[0], T[1], T[2]);

    // ---- gather active value regions from the tree ----
    std::vector<RawParam> params;
    const auto& tree = grid.tree();
    const auto& root = tree.root();
    for (auto it = root.cbeginValueOn(); it; ++it)
        params.push_back(RawParam{3, it.getOrigin(), static_cast<double>(*it)});
    for (auto up = root.cbeginChild(); up; ++up) {
        const auto& upper = *up;
        for (auto it = upper.cbeginValueOn(); it; ++it)
            params.push_back(RawParam{2, it.getOrigin(), static_cast<double>(*it)});
        for (auto lo = upper.cbeginChild(); lo; ++lo) {
            const auto& lower = *lo;
            for (auto it = lower.cbeginValueOn(); it; ++it)
                params.push_back(RawParam{1, it.getOrigin(), static_cast<double>(*it)});
            for (auto lf = lower.cbeginChild(); lf; ++lf) {
                const auto& leaf = *lf;
                for (auto it = leaf.cbeginValueOn(); it; ++it)
                    params.push_back(RawParam{0, it.getCoord(), static_cast<double>(*it)});
            }
        }
    }
    std::sort(params.begin(), params.end());

    const uint64_t count = params.size();
    mImpl->levels.reserve(count);
    mImpl->origins.reserve(count);
    mImpl->initial.reserve(count);
    mImpl->boxes.resize(count);

    // leaf origin -> position inside mImpl->leaves while filling
    std::unordered_map<Key3, uint32_t, Key3Hash> leafIndex;
    for (uint64_t i = 0; i < count; ++i) {
        const RawParam& p = params[i];
        mImpl->levels.push_back(p.level);
        mImpl->origins.push_back(p.origin);
        mImpl->initial.push_back(p.value);
        const int32_t edge = levelEdge(p.level);
        for (int a = 0; a < 3; ++a) {
            // trilinear influence of the value region, dilated by one cell
            mImpl->boxes[i].lo[a] = static_cast<double>(p.origin[a]) - 1.0;
            mImpl->boxes[i].hi[a] = static_cast<double>(p.origin[a] + edge);
        }
        if (p.level == 0) {
            const Key3 leafKey = makeKey(p.origin[0] & ~7, p.origin[1] & ~7, p.origin[2] & ~7);
            auto found = leafIndex.find(leafKey);
            uint32_t slot;
            if (found == leafIndex.end()) {
                slot = static_cast<uint32_t>(mImpl->leaves.size());
                leafIndex.emplace(leafKey, slot);
                Impl::LeafBlock block;
                block.param.fill(-1);
                mImpl->leaves.push_back(block);
            } else {
                slot = found->second;
            }
            const uint32_t off = static_cast<uint32_t>(((p.origin[0] & 7) << 6) |
                                                       ((p.origin[1] & 7) << 3) | (p.origin[2] & 7));
            mImpl->leaves[slot].param[off] = static_cast<int32_t>(i);
        } else {
            mImpl->tileMap[p.level - 1].emplace(makeKey(p.origin[0], p.origin[1], p.origin[2]),
                                                static_cast<int32_t>(i));
        }
    }
    mImpl->leafMap.reserve(leafIndex.size());
    for (const auto& kv : leafIndex) mImpl->leafMap.emplace(kv.first, kv.second);
    mImpl->finishBuild();
}

Catalog::~Catalog() = default;
Catalog::Catalog(Catalog&&) noexcept = default;
Catalog& Catalog::operator=(Catalog&&) noexcept = default;

uint64_t Catalog::paramCount() const { return static_cast<uint64_t>(mImpl->levels.size()); }
int Catalog::paramLevel(uint64_t i) const { return mImpl->levels.at(static_cast<size_t>(i)); }
Coord Catalog::paramOrigin(uint64_t i) const { return mImpl->origins.at(static_cast<size_t>(i)); }
const std::vector<double>& Catalog::initialDensity() const { return mImpl->initial; }
double Catalog::mapScale() const { return mImpl->scale; }
Vec3d Catalog::mapTranslate() const { return mImpl->translate; }

/// @brief Friend bridge used by the free functions (declared in the header).
struct CatalogAccess {
    static const Catalog::Impl& get(const Catalog& c) { return *c.mImpl; }
};

// --------------------------------------------------------------------------------------
// Record
// --------------------------------------------------------------------------------------

class Record::Impl
{
public:
    struct Segment {
        std::array<int32_t, 8> param; // -1 = inactive corner
        std::array<double, 8> w;      // trilinear weights
        double rho = 0.0;             // sampled density
        double len = 0.0;             // world segment length
        double tIn = 1.0;             // transmittance on segment entry
    };
    struct RayRec {
        std::vector<Segment> segments;
        std::array<double, 3> background{{0.0, 0.0, 0.0}};
    };
    std::vector<RayRec> rays;
    std::array<double, 3> emission{{0.0, 0.0, 0.0}};
    double extinction = 0.0;
    uint64_t paramCount = 0;
};

Record::Record()
    : mImpl(new Impl)
{
}
Record::~Record() = default;
Record::Record(Record&&) noexcept = default;
Record& Record::operator=(Record&&) noexcept = default;

uint64_t Record::rayCount() const { return static_cast<uint64_t>(mImpl->rays.size()); }
uint64_t Record::paramCount() const { return mImpl->paramCount; }
uint64_t Record::segmentCount() const
{
    uint64_t n = 0;
    for (const auto& r : mImpl->rays) n += r.segments.size();
    return n;
}

/// @brief Friend bridge used by the free functions (declared in the header).
struct RecordAccess {
    static const Record::Impl& get(const Record& r) { return *r.mImpl; }
    static std::unique_ptr<Record::Impl>& ptr(Record& r) { return r.mImpl; }
};

// --------------------------------------------------------------------------------------
// forward
// --------------------------------------------------------------------------------------

ForwardResult forward(const Catalog& catalog,
                      const std::vector<double>& density,
                      double extinction,
                      const Vec3d& emission,
                      const std::vector<Ray>& rays,
                      double h,
                      Record* record)
{
    // ---- validate everything before doing any work ----
    const Catalog::Impl& impl = CatalogAccess::get(catalog);
    const uint64_t paramCount = static_cast<uint64_t>(impl.levels.size());
    if (density.size() != paramCount)
        throwInvalid("fog::forward: density vector size does not match the catalog");
    for (double v : density) {
        if (!std::isfinite(v) || v < 0.0)
            throwInvalid("fog::forward: density values must be finite and non-negative");
    }
    if (!std::isfinite(extinction) || extinction < 0.0)
        throwInvalid("fog::forward: extinction must be finite and non-negative");
    if (!finite3(emission) || emission[0] < 0.0 || emission[1] < 0.0 || emission[2] < 0.0)
        throwInvalid("fog::forward: emission must be finite and non-negative");
    if (!std::isfinite(h) || h <= 0.0)
        throwInvalid("fog::forward: step h must be finite and positive");
    for (const Ray& r : rays) {
        if (!finite3(r.origin) || !finite3(r.direction) || !finite3(r.background))
            throwInvalid("fog::forward: rays must be finite");
        const double len2 = r.direction.lengthSqr();
        if (!std::isfinite(len2) || len2 <= 0.0)
            throwInvalid("fog::forward: ray directions must be non-zero");
        if (!std::isfinite(r.t0) || !std::isfinite(r.t1) || !(r.t0 < r.t1))
            throwInvalid("fog::forward: ray windows must be finite and increasing");
    }

    ForwardResult out;
    out.rgb.resize(rays.size());
    out.transmittance.resize(rays.size());

    if (record) {
        RecordAccess::ptr(*record).reset(new Record::Impl);
        Record::Impl& recImpl = *RecordAccess::ptr(*record);
        recImpl.paramCount = paramCount;
        recImpl.extinction = extinction;
        recImpl.emission = {{emission[0], emission[1], emission[2]}};
        recImpl.rays.resize(rays.size());
    }

    const double scale = impl.scale;
    const double invScale = 1.0 / scale;
    const double tr[3] = {impl.translate[0], impl.translate[1], impl.translate[2]};
    const double k = extinction;
    const double e[3] = {emission[0], emission[1], emission[2]};
    std::vector<std::pair<double, double>> intervals;

    for (size_t ri = 0; ri < rays.size(); ++ri) {
        const Ray& ray = rays[ri];
        const double lenD = ray.direction.length();
        const double D = (ray.t1 - ray.t0) * lenD; // world length
        // segment partition: n segments, the last takes the remaining length L <= h
        uint64_t n = static_cast<uint64_t>(std::floor(D / h));
        if (n * h < D) ++n;
        if (n == 0) n = 1;
        const double lastL = D - static_cast<double>(n - 1) * h;

        // ray in index coordinates (same parameter t)
        const double oi[3] = {(ray.origin[0] - tr[0]) * invScale,
                              (ray.origin[1] - tr[1]) * invScale,
                              (ray.origin[2] - tr[2]) * invScale};
        const double di[3] = {ray.direction[0] * invScale,
                              ray.direction[1] * invScale,
                              ray.direction[2] * invScale};

        intervals.clear();
        impl.rayIntervals(oi, di, ray.t0, ray.t1, intervals, out.hierarchyVisits);

        double C[3] = {0.0, 0.0, 0.0};
        double T = 1.0;
        Record::Impl::RayRec* recRay = record ? &RecordAccess::ptr(*record)->rays[ri] : nullptr;
        if (recRay) {
            recRay->background = {{ray.background[0], ray.background[1], ray.background[2]}};
            recRay->segments.clear();
        }

        const double eps = h * 1e-9;
        for (const auto& iv : intervals) {
            // convert the t-interval to world distance from t0 and clip to [0, D]
            double sa = (iv.first - ray.t0) * lenD;
            double sb = (iv.second - ray.t0) * lenD;
            sa = std::max(sa, 0.0);
            sb = std::min(sb, D);
            if (!(sa < sb)) continue;
            int64_t i0 = static_cast<int64_t>(std::ceil(sa / h - 0.5 - 1e-9));
            if (i0 < 0) i0 = 0;
            for (int64_t i = i0; i < static_cast<int64_t>(n); ++i) {
                const bool last = (i == static_cast<int64_t>(n - 1));
                const double m = last ? (static_cast<double>(n - 1) * h + 0.5 * lastL)
                                      : ((static_cast<double>(i) + 0.5) * h);
                if (m > sb + eps) break;
                if (m < sa - eps) continue;
                ++out.sampleEvaluations;
                const double t = ray.t0 + m / lenD;
                const double px = ray.origin[0] + t * ray.direction[0];
                const double py = ray.origin[1] + t * ray.direction[1];
                const double pz = ray.origin[2] + t * ray.direction[2];
                const double ix = (px - tr[0]) * invScale;
                const double iy = (py - tr[1]) * invScale;
                const double iz = (pz - tr[2]) * invScale;
                const int32_t bx = static_cast<int32_t>(std::floor(ix));
                const int32_t by = static_cast<int32_t>(std::floor(iy));
                const int32_t bz = static_cast<int32_t>(std::floor(iz));
                const double fx = ix - std::floor(ix);
                const double fy = iy - std::floor(iy);
                const double fz = iz - std::floor(iz);

                std::array<int32_t, 8> param;
                std::array<double, 8> w;
                double rho = 0.0;
                for (int c = 0; c < 8; ++c) {
                    const int dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
                    const double wc = (dx ? fx : 1.0 - fx) * (dy ? fy : 1.0 - fy) * (dz ? fz : 1.0 - fz);
                    const int32_t p = impl.resolve(bx + dx, by + dy, bz + dz, out.hierarchyVisits);
                    param[static_cast<size_t>(c)] = p;
                    w[static_cast<size_t>(c)] = wc;
                    if (p >= 0) rho += wc * density[static_cast<size_t>(p)];
                }

                const double L = last ? lastL : h;
                const double x = rho * k * L;
                const double a = std::exp(-x);
                // (1-a)/(rho*k) with the continuous limit L at a zero denominator
                const double f = (rho * k != 0.0) ? (-std::expm1(-x)) / (rho * k) : L;
                const double s = rho * f; // emitted radiance factor of this segment
                C[0] += T * s * e[0];
                C[1] += T * s * e[1];
                C[2] += T * s * e[2];
                const double tIn = T;
                T *= a;
                if (recRay) {
                    Record::Impl::Segment seg;
                    seg.param = param;
                    seg.w = w;
                    seg.rho = rho;
                    seg.len = L;
                    seg.tIn = tIn;
                    recRay->segments.push_back(seg);
                }
            }
        }
        out.rgb[ri] = Vec3d(C[0] + T * ray.background[0],
                            C[1] + T * ray.background[1],
                            C[2] + T * ray.background[2]);
        out.transmittance[ri] = T;
    }
    return out;
}

// --------------------------------------------------------------------------------------
// vjp
// --------------------------------------------------------------------------------------

VJPResult vjp(const Record& record,
              const std::vector<Vec3d>& rgbAdjoint,
              const std::vector<double>& transmittanceAdjoint)
{
    const Record::Impl& impl = RecordAccess::get(record);
    if (rgbAdjoint.size() != impl.rays.size() || transmittanceAdjoint.size() != impl.rays.size())
        throwInvalid("fog::vjp: adjoint sizes do not match the record");
    for (const Vec3d& g : rgbAdjoint) {
        if (!finite3(g)) throwInvalid("fog::vjp: adjoints must be finite");
    }
    for (double q : transmittanceAdjoint) {
        if (!std::isfinite(q)) throwInvalid("fog::vjp: adjoints must be finite");
    }

    VJPResult out;
    out.dDensity.assign(static_cast<size_t>(impl.paramCount), 0.0);
    const double k = impl.extinction;
    const double e[3] = {impl.emission[0], impl.emission[1], impl.emission[2]};
    double dk = 0.0;
    double de[3] = {0.0, 0.0, 0.0};

    for (size_t ri = 0; ri < impl.rays.size(); ++ri) {
        const Record::Impl::RayRec& rr = impl.rays[ri];
        const Vec3d& g = rgbAdjoint[ri];
        const double* bg = rr.background.data();
        double gC[3] = {g[0], g[1], g[2]};
        // RGB = C_n + T_n * bg, T_final = T_n
        double gT = transmittanceAdjoint[ri] + g[0] * bg[0] + g[1] * bg[1] + g[2] * bg[2];
        const double gCdotE = gC[0] * e[0] + gC[1] * e[1] + gC[2] * e[2];
        for (auto it = rr.segments.rbegin(); it != rr.segments.rend(); ++it) {
            const Record::Impl::Segment& s = *it;
            double a, phi, dphiDrho, dphiDk, daDrho, daDk;
            if (k > 0.0) {
                const double x = s.rho * k * s.len;
                a = std::exp(-x);
                const double u = -std::expm1(-x); // 1 - a
                phi = u / k;
                dphiDrho = s.len * a;
                dphiDk = (x * a - u) / (k * k);
                daDrho = -k * s.len * a;
                daDk = -s.rho * s.len * a;
            } else {
                // continuous limits at k = 0 (right derivatives)
                a = 1.0;
                phi = s.rho * s.len;
                dphiDrho = s.len;
                dphiDk = -0.5 * s.rho * s.rho * s.len * s.len;
                daDrho = 0.0;
                daDk = -s.rho * s.len;
            }
            const double gS[3] = {gC[0] * s.tIn, gC[1] * s.tIn, gC[2] * s.tIn};
            const double gA = gT * s.tIn;
            const double gSE = gS[0] * e[0] + gS[1] * e[1] + gS[2] * e[2];
            const double drho = gSE * dphiDrho + gA * daDrho;
            dk += gSE * dphiDk + gA * daDk;
            de[0] += gS[0] * phi;
            de[1] += gS[1] * phi;
            de[2] += gS[2] * phi;
            for (int c = 0; c < 8; ++c) {
                const int32_t p = s.param[static_cast<size_t>(c)];
                if (p >= 0) out.dDensity[static_cast<size_t>(p)] += drho * s.w[static_cast<size_t>(c)];
            }
            // propagate the adjoints to the previous segment
            gT = gT * a + gCdotE * phi;
        }
    }
    out.dExtinction = dk;
    out.dEmission = Vec3d(de[0], de[1], de[2]);
    return out;
}

} // namespace fog
} // namespace tools
} // namespace nanovdb
