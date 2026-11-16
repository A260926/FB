#include "task_support/check.h"

#include "VoxelData.hpp"
#include "VoxelOctree.hpp"
#include "VoxelSurfaceLights.hpp"
#include "VoxelDirect.hpp"
#include "thread/ThreadUtils.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "third-party/lz4.h"

namespace vsl_test {

const double kPi = 3.14159265358979323846;
const double kLumR = 0.2126, kLumG = 0.7152, kLumB = 0.0722;

void ensurePool()
{
    static bool started = false;
    if (!started) {
        ThreadUtils::startThreads(2);
        started = true;
    }
}

// Dense raw voxel grid used as an independent test oracle. The production
// code never expands to this representation; only these test references do.
struct RawGrid {
    uint32 n;
    std::vector<uint32> v;

    explicit RawGrid(uint32 side) : n(side), v(size_t(side) * side * side, 0) {}

    size_t idx(int x, int y, int z) const {
        return size_t(x) + n * (size_t(y) + n * size_t(z));
    }
    uint32 at(int x, int y, int z) const {
        if (x < 0 || y < 0 || z < 0 || x >= int(n) || y >= int(n) || z >= int(n))
            return 0;
        return v[idx(x, y, z)];
    }
    void set(int x, int y, int z, uint32 key) {
        v[idx(x, y, z)] = key;
    }
};

VoxelOctree *buildTree(const RawGrid &g, const std::string &path)
{
    ensurePool();
    {
        std::ofstream f(path, std::ios::binary);
        uint32 n = g.n;
        for (int k = 0; k < 3; ++k)
            f.write(reinterpret_cast<const char *>(&n), 4);
        f.write(reinterpret_cast<const char *>(g.v.data()), g.v.size() * 4);
    }
    // Kept alive for the whole process; the trees reference no VoxelData after
    // construction anyway.
    static std::vector<VoxelData *> inputs;
    inputs.push_back(new VoxelData(path.c_str(), 512u * 1024u * 1024u));
    return new VoxelOctree(inputs.back());
}

double luminance(const Vec3 &c)
{
    return kLumR * c.x + kLumG * c.y + kLumB * c.z;
}

// Scene oracle for the hand-built compact hierarchy. Return values:
//   -1  -> empty region
//    0  -> subdivide this cell further
//   >0  -> a solid leaf carrying that payload over the whole cell
typedef int (*MixedSceneFn)(int depth, int x, int y, int z, int size);

// A legal mixed-depth scene on a 16x16x16 coarse coordinate frame:
//  * payload A: one large depth-2 leaf covering [4,8)^3
//  * payload B: depth-4 leaves partially covering A's +x face (3x3 patch),
//    plus a corner-only contact that shares no face area
//  * payload C: a hollow shell (empty interior) in [0,4)^3
//  * payload D: a disjoint solid used later as a non-emissive occluder
int mixedScene(int depth, int x, int y, int z, int /*size*/)
{
    const int A = 0x1001, B = 0x2002, C = 0x3003, D = 0x4004;
    // Large single leaf: depth-2 cell [4,8)^3.
    if (depth == 2 && x == 4 && y == 4 && z == 4)
        return A;
    // Small leaves on the +x side of the large leaf, depth 4 (coarse cells).
    if (depth == 4 && x == 8 && y >= 4 && y < 7 && z >= 4 && z < 7)
        return B;
    // Corner-only contact at x=8,y=8: shares at most an edge with the A leaf.
    if (depth == 4 && x == 8 && y == 8 && z >= 4 && z < 8)
        return B;
    // Hollow shell in the [0,4) x [8,12) x [0,4) box: only the boundary unit
    // cells are solid, the cavity stays empty. Coarse cells subdivide.
    if (x < 4 && y >= 8 && y < 12 && z < 4) {
        if (depth < 4)
            return 0;
        bool boundary = (x == 0 || x == 3 || y == 8 || y == 11 || z == 0 || z == 3);
        return boundary ? C : -1;
    }
    // Disjoint occluder slab near the far side.
    if (depth == 4 && x == 12 && y >= 4 && y < 12 && z >= 4 && z < 12)
        return D;
    if (depth >= 4)
        return -1;
    return 0;
}

// Fills the descriptor already reserved at descIndex, pushes this node's
// one-word child slots (leaf payload or a reserved internal descriptor), then
// appends internal subtrees. Mirrors the narrow layout of
// VoxelOctree::buildOctree without the wide double-word offset path.
void encodeNode(MixedSceneFn scene, int depth, int x, int y, int z, int size,
                std::vector<uint32> &out, size_t descIndex)
{
    uint32 childMask = 0, leafMask = 0;
    int half = size >> 1;
    struct Child { int oct; int code; int cx, cy, cz; size_t slot; };
    Child children[8];
    int count = 0;
    const int coarseScale = VoxelSurfaceLights::GridSize / 16;
    for (int oct = 0; oct < 8; ++oct) {
        int cx = (oct & 1) ? x : x + half;
        int cy = (oct & 2) ? y : y + half;
        int cz = (oct & 4) ? z : z + half;
        // The scene callback reasons in the 16-cell coarse frame.
        int code = scene(depth + 1, cx / coarseScale, cy / coarseScale,
                         cz / coarseScale, half / coarseScale);
        if (code < 0)
            continue;
        children[count++] = Child{oct, code, cx, cy, cz, 0};
        childMask |= 128u >> oct;
        if (code == 0)
            leafMask |= 128u >> oct; // set bit => internal descriptor slot
    }

    size_t firstSlot = out.size();
    // Slots are stored in reverse octant order like the production builder.
    for (int i = count - 1; i >= 0; --i) {
        Child &ch = children[i];
        ch.slot = out.size();
        out.push_back(ch.code == 0 ? 0u : uint32(ch.code));
    }
    uint64 rel = firstSlot - descIndex;
    out[descIndex] = (childMask << 8) | leafMask | uint32(rel << 18);

    // Internal subtrees are appended after the slot block; the reserved slot
    // becomes their descriptor and receives its own relative child offset.
    for (int i = 0; i < count; ++i) {
        const Child &ch = children[i];
        if (ch.code == 0)
            encodeNode(scene, depth + 1, ch.cx, ch.cy, ch.cz, half, out, ch.slot);
    }
}

// Serializes a narrow compact hierarchy and writes it through the same LZ4
// container VoxelOctree(const char*) reads, exercising save/load compatibility.
VoxelOctree *loadHandBuiltTree(MixedSceneFn scene, const std::string &path)
{
    ensurePool();
    std::vector<uint32> words;
    words.push_back(0); // reserved root descriptor
    encodeNode(scene, 0, 0, 0, 0, VoxelSurfaceLights::GridSize, words, 0);

    std::vector<char> compressed(LZ4_compressBound(int(words.size() * 4)));
    int compSize = LZ4_compress_default(
        reinterpret_cast<const char *>(words.data()),
        compressed.data(), int(words.size() * 4), int(compressed.size()));

    float center[3] = {0.5f, 0.5f, 0.5f};
    uint64 wordCount = words.size();
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char *>(center), sizeof(center));
        f.write(reinterpret_cast<const char *>(&wordCount), sizeof(wordCount));
        uint64 block = uint64(compSize);
        f.write(reinterpret_cast<const char *>(&block), sizeof(block));
        f.write(compressed.data(), compSize);
    }
    return new VoxelOctree(path.c_str());
}

// Dense 16^3 expansion of the same scene, used only as the test oracle.
RawGrid expandMixedReference()
{
    RawGrid g(16);
    for (int z = 0; z < 16; ++z)
    for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) {
        // Replicate the leaf regions at coarse granularity.
        if (x >= 4 && x < 8 && y >= 4 && y < 8 && z >= 4 && z < 8)
            g.set(x, y, z, 0x1001u);
        if (x == 8 && y >= 4 && y < 7 && z >= 4 && z < 7)
            g.set(x, y, z, 0x2002u);
        if (x == 8 && y == 8 && z >= 4 && z < 8)
            g.set(x, y, z, 0x2002u);
        if (x < 4 && y >= 8 && y < 12 && z < 4) {
            bool boundary = (x == 0 || z == 0 || y == 8 || y == 11 ||
                             x == 3 || z == 3);
            if (boundary)
                g.set(x, y, z, 0x3003u);
        }
        if (x == 12 && y >= 4 && y < 12 && z >= 4 && z < 12)
            g.set(x, y, z, 0x4004u);
    }
    return g;
}

// Direction index: axis*2 + side, side 0 = negative, 1 = positive.
struct FaceSets {
    std::vector<uint8> refSet;
    std::vector<uint8> prodSet;
    uint32 n;

    FaceSets(uint32 side) : refSet(size_t(side) * side * side * 6, 0),
                            prodSet(size_t(side) * side * side * 6, 0), n(side) {}

    size_t dirIndex(int x, int y, int z, int dir) const {
        return (size_t(x) + n * (size_t(y) + n * size_t(z))) * 6 + dir;
    }
};

// Fills the reference exposed-face set directly from raw cell adjacency.
double referenceExposed(const RawGrid &g, FaceSets &fs)
{
    const int dirs[6][4] = {
        {-1, 0, 0, 0}, {1, 0, 0, 1},
        {0, -1, 0, 2}, {0, 1, 0, 3},
        {0, 0, -1, 4}, {0, 0, 1, 5}
    };
    size_t count = 0;
    for (int z = 0; z < int(g.n); ++z)
    for (int y = 0; y < int(g.n); ++y)
    for (int x = 0; x < int(g.n); ++x) {
        if (!g.at(x, y, z))
            continue;
        for (int d = 0; d < 6; ++d) {
            if (g.at(x + dirs[d][0], y + dirs[d][1], z + dirs[d][2]) == 0) {
                fs.refSet[fs.dirIndex(x, y, z, d)] = 1;
                ++count;
            }
        }
    }
    double cellArea = 1.0 / double(g.n);
    return double(count) * cellArea * cellArea;
}

// Projects every production face onto the raw cells it covers, independently
// recomputing its area from the integer rectangle dimensions.
double productionExposed(const VoxelSurfaceLights &lights, uint32 n, FaceSets &fs,
                         size_t &rectCount)
{
    const int32 G = VoxelSurfaceLights::GridSize;
    const int32 q = G / int32(n); // raw cells are integer multiples of root cells
    double areaSum = 0.0;
    rectCount = 0;

    for (size_t fi = 0; fi < lights.faceCount(); ++fi) {
        const SurfaceLightFace &f = lights.face(int32(fi));
        ++rectCount;

        int a1 = (f.axis + 1) % 3, a2 = (f.axis + 2) % 3;
        int c0lo = f.low0 / q, c0hi = f.high0 / q;
        int c1lo = f.low1 / q, c1hi = f.high1 / q;
        // The face belongs to the occupied leaf cell: for a negative face the
        // plane is the leaf's low bound, for a positive face its high bound.
        int planeCell = (f.sign < 0) ? (f.plane / q) : (f.plane / q - 1);

        double expectArea = double(c0hi - c0lo) * double(c1hi - c1lo) /
                            double(n) / double(n);
        if (std::fabs(expectArea - f.area) > 1e-12)
            return -1.0;
        areaSum += f.area;

        int dir = f.axis * 2 + (f.sign > 0 ? 1 : 0);
        for (int b = c0lo; b < c0hi; ++b)
        for (int c = c1lo; c < c1hi; ++c) {
            int cell[3] = {0, 0, 0};
            cell[a1] = b;
            cell[a2] = c;
            cell[f.axis] = planeCell;
            fs.prodSet[fs.dirIndex(cell[0], cell[1], cell[2], dir)] = 1;
        }
    }
    return areaSum;
}

// Independent area-space direct lighting integral over the light rectangles.
// It deliberately ignores the resource's sample/pdf machinery and assumes an
// unobstructed line of sight, which the calling scenes arrange explicitly.
// Used on single-key scenes where every face shares the given radiance.
Vec3 referenceDirectMono(const VoxelSurfaceLights &lights, const Vec3 &radiance,
                         const Vec3 &P, const Vec3 &N, const Vec3 &albedo, int sub)
{
    Vec3 acc(0.0f, 0.0f, 0.0f);
    for (size_t fi = 0; fi < lights.faceCount(); ++fi) {
        const SurfaceLightFace &f = lights.face(int32(fi));
        const int a1 = (f.axis + 1) % 3, a2 = (f.axis + 2) % 3;
        const double ws = VoxelSurfaceLights::WorldScale;
        for (int i = 0; i < sub; ++i)
        for (int j = 0; j < sub; ++j) {
            double u1 = (i + 0.5) / sub, u2 = (j + 0.5) / sub;
            Vec3 Q(0.0f, 0.0f, 0.0f);
            Q.a[f.axis] = float(1.0 + double(f.plane) * ws);
            Q.a[a1] = float(1.0 + (double(f.low0) + u1 * double(f.high0 - f.low0)) * ws);
            Q.a[a2] = float(1.0 + (double(f.low1) + u2 * double(f.high1 - f.low1)) * ws);

            Vec3 d = Q - P;
            double dist2 = d.dot(d);
            if (!(dist2 > 0.0))
                continue;
            double dist = std::sqrt(dist2);
            Vec3 wi(float(d.x / dist), float(d.y / dist), float(d.z / dist));
            double cr = N.dot(wi);
            double cl = f.normal.dot(Vec3(-wi.x, -wi.y, -wi.z));
            if (cr <= 0.0 || cl <= 0.0)
                continue;
            double w = f.area * cr * cl / (dist2 * kPi * double(sub * sub));
            acc += radiance * albedo * float(w);
        }
    }
    return acc;
}

double avgEstimate(const VoxelSurfaceLights &lights, const Vec3 &P, const Vec3 &N,
                   const Vec3 &albedo, int perDim)
{
    double sum = 0.0;
    for (int i = 0; i < perDim; ++i)
    for (int j = 0; j < perDim; ++j)
    for (int k = 0; k < perDim; ++k) {
        Vec3 r = estimateDirect(lights, P, N, albedo,
                                (i + 0.5) / perDim,
                                (j + 0.5) / perDim,
                                (k + 0.5) / perDim);
        sum += r.x + r.y + r.z;
    }
    return sum / double(perDim * perDim * perDim);
}

// ---------------------------------------------------------------------------
// Scene builders
// ---------------------------------------------------------------------------

// Small symmetric block: every face has a known integer-box reference.
VoxelOctree *makeBlock(VoxelSurfaceLights *&lights, uint32 key)
{
    RawGrid g(8);
    for (int z = 2; z < 6; ++z)
    for (int y = 2; y < 6; ++y)
    for (int x = 2; x < 6; ++x)
        g.set(x, y, z, key);
    VoxelOctree *tree = buildTree(g, ".task-build/tc_block.raw");
    lights = new VoxelSurfaceLights(tree);
    return tree;
}

// Dense 64^3 region which forces long (double-word) child offsets.
VoxelOctree *makeWide()
{
    RawGrid g(64);
    const uint32 W = 0x9009u;
    for (int z = 8; z < 56; ++z)
    for (int y = 8; y < 56; ++y)
    for (int x = 8; x < 56; ++x)
        g.set(x, y, z, W);
    VoxelOctree *tree = buildTree(g, ".task-build/tc_wide.raw");
    int wide = 0;
    for (uint64 i = 0; i < tree->octreeSize(); ++i)
        wide += !!(tree->octreeData()[i] & 0x10000);
    if (wide <= 0)
        std::printf("warning: wide scene produced no double-word descriptors\n");
    return tree;
}

// Emitter slab plus an optional one-cell occluder wall for transport tests.
VoxelOctree *makeTransport(RawGrid &g, bool withWall, uint32 emitter, uint32 blocker)
{
    g = RawGrid(16);
    for (int y = 4; y < 12; ++y)
    for (int z = 4; z < 12; ++z)
        g.set(12, y, z, emitter);
    if (withWall)
        for (int y = 4; y < 12; ++y)
        for (int z = 4; z < 12; ++z)
            g.set(8, y, z, blocker);
    return buildTree(g, ".task-build/tc_transport.raw");
}

// ---------------------------------------------------------------------------
// exposure group
// ---------------------------------------------------------------------------

int exposure_block_faces()
{
    const uint32 key = 0x3456789au;
    VoxelSurfaceLights *lights = nullptr;
    VoxelOctree *tree = makeBlock(lights, key);
    RawGrid g(8);
    for (int z = 2; z < 6; ++z)
    for (int y = 2; y < 6; ++y)
    for (int x = 2; x < 6; ++x)
        g.set(x, y, z, key);

    FaceSets fs(8);
    double refArea = referenceExposed(g, fs);
    size_t rects = 0;
    double prodArea = productionExposed(*lights, 8, fs, rects);

    TASK_CHECK(prodArea >= 0.0);
    TASK_CHECK(std::fabs(prodArea - refArea) < 1e-9);
    TASK_CHECK(std::fabs(lights->surfaceArea() - refArea) < 1e-12);
    TASK_CHECK(fs.refSet == fs.prodSet);
    TASK_CHECK(rects == 6 * 16); // 4x4 raw cells per face, no merging at this depth

    // Every descriptor carries the outward unit normal and the full payload.
    for (size_t i = 0; i < lights->faceCount(); ++i) {
        const SurfaceLightFace &f = lights->face(int32(i));
        TASK_CHECK(f.payload == key);
        TASK_CHECK(std::fabs(f.normal.length() - 1.0f) < 1e-6f);
        TASK_CHECK(std::fabs(std::fabs(f.normal.a[f.axis]) - 1.0f) < 1e-6f);
    }

    // Internal faces of touching, differently-keyed leaves never emit.
    RawGrid two(8);
    for (int z = 3; z < 5; ++z)
    for (int y = 3; y < 5; ++y) {
        two.set(3, y, z, 0xAAu);
        two.set(4, y, z, 0xBBu); // shares the x=4 plane with the 0xAA leaf
    }
    VoxelOctree *tree2 = buildTree(two, ".task-build/tc_two.raw");
    VoxelSurfaceLights lights2(tree2);
    for (size_t i = 0; i < lights2.faceCount(); ++i) {
        const SurfaceLightFace &f = lights2.face(int32(i));
        TASK_CHECK(!(f.axis == 0 && f.plane == 4 * VoxelSurfaceLights::GridSize / 8));
    }
    delete lights;
    (void)tree;
    return 0;
}

int exposure_mixed_depth()
{
    VoxelOctree *tree = loadHandBuiltTree(mixedScene, ".task-build/tc_mixed.oct");
    VoxelSurfaceLights lights(tree);
    RawGrid g = expandMixedReference();

    FaceSets fs(16);
    double refArea = referenceExposed(g, fs);
    size_t rects = 0;
    double prodArea = productionExposed(lights, 16, fs, rects);
    TASK_CHECK(prodArea >= 0.0);
    TASK_CHECK(std::fabs(prodArea - refArea) < 1e-9);
    TASK_CHECK(fs.refSet == fs.prodSet);

    // The large depth-2 A leaf must survive as coarse rectangles rather than
    // being split into the unit faces of its deep neighbours.
    size_t rawCellFaces = 0;
    for (uint8 b : fs.refSet) rawCellFaces += b;
    TASK_CHECK(rects < rawCellFaces);
    bool foundCoarse = false;
    const int32 q = VoxelSurfaceLights::GridSize / 16;
    for (size_t i = 0; i < lights.faceCount(); ++i) {
        const SurfaceLightFace &f = lights.face(int32(i));
        if ((f.high0 - f.low0) > q || (f.high1 - f.low1) > q)
            foundCoarse = true;
    }
    TASK_CHECK(foundCoarse);

    // A coarse 4x4-cell face of the big leaf is one rectangle (not 16 units).
    bool foundFourByFour = false;
    for (size_t i = 0; i < lights.faceCount(); ++i) {
        const SurfaceLightFace &f = lights.face(int32(i));
        if (f.payload == 0x1001u &&
            (f.high0 - f.low0) == 4 * q && (f.high1 - f.low1) == 4 * q)
            foundFourByFour = true;
    }
    TASK_CHECK(foundFourByFour);
    TASK_CHECK(lights.faceCount() > 0);
    return 0;
}

int exposure_long_offsets()
{
    VoxelOctree *tree = makeWide();
    VoxelSurfaceLights lights(tree);

    RawGrid g(64);
    const uint32 W = 0x9009u;
    for (int z = 8; z < 56; ++z)
    for (int y = 8; y < 56; ++y)
    for (int x = 8; x < 56; ++x)
        g.set(x, y, z, W);

    FaceSets fs(64);
    double refArea = referenceExposed(g, fs);
    size_t rects = 0;
    double prodArea = productionExposed(lights, 64, fs, rects);
    TASK_CHECK(prodArea >= 0.0);
    TASK_CHECK(std::fabs(prodArea - refArea) < 1e-9);
    TASK_CHECK(fs.refSet == fs.prodSet);
    double side = 48.0 / 64.0;
    TASK_CHECK(std::fabs(lights.surfaceArea() - 6.0 * side * side) < 1e-9);
    // This dense legal file must contain both compact double-word encodings:
    // 0x10000 (per-child double-word spans) and 0x20000 (long 64-bit offsets).
    int wideDescriptors = 0, wideOffsets = 0;
    for (uint64 i = 0; i < tree->octreeSize(); ++i) {
        wideDescriptors += !!(tree->octreeData()[i] & 0x10000);
        wideOffsets += !!(tree->octreeData()[i] & 0x20000);
    }
    TASK_CHECK(wideDescriptors > 0);
    TASK_CHECK(wideOffsets > 0);
    return 0;
}

// ---------------------------------------------------------------------------
// distribution group
// ---------------------------------------------------------------------------

int distribution_mass_and_pdf()
{
    const uint32 key = 0x4004u;
    VoxelSurfaceLights *lights = nullptr;
    makeBlock(lights, key);

    TASK_CHECK(lights->totalMass() == 0.0);
    SurfaceLightSample s;
    TASK_CHECK(!lights->sample(0.25, 0.5, 0.5, s));

    Vec3 rad(1.5f, 0.25f, 0.75f);
    lights->setEmission(key, rad);
    double y = luminance(rad);
    double total = lights->surfaceArea() * y;
    TASK_CHECK(std::fabs(lights->totalMass() - total) < 1e-9);

    // Joint area density is Y / totalMass, proportional to area, never 1/area.
    // It normalizes as an area density: integral of pdfArea over the surface
    // (sum of pdfArea * face area) equals one, not the raw count.
    double pdfAreaIntegral = 0.0;
    for (size_t i = 0; i < lights->faceCount(); ++i) {
        const SurfaceLightFace &f = lights->face(int32(i));
        double expect = y / total;
        TASK_CHECK(std::fabs(lights->pdfArea(int32(i)) - expect) < 1e-12);
        TASK_CHECK(std::fabs(lights->pdfArea(int32(i)) - 1.0 / f.area) > 1e-6);
        pdfAreaIntegral += lights->pdfArea(int32(i)) * f.area;
    }
    TASK_CHECK(std::fabs(pdfAreaIntegral - 1.0) < 1e-9);

    // Empirical face probabilities from fixed stratified samples. Face
    // selection consumes only u0, so the stratification is a dense 1-D sweep
    // over u0 (far more bins than faces) with deterministic in-plane values;
    // every face is reached and its share matches its area*Y weight.
    const uint64 bins = 60000;
    std::vector<uint64> hist(lights->faceCount(), 0);
    for (uint64 t = 0; t < bins; ++t) {
        double u0 = double(t + 0.5) / double(bins);
        double u1 = double((t * 7 + 3) % 101) / 101.0;
        double u2 = double((t * 13 + 5) % 97) / 97.0;
        TASK_CHECK(lights->sample(u0, u1, u2, s));
        TASK_CHECK(s.faceId >= 0 && size_t(s.faceId) < lights->faceCount());
        TASK_CHECK(s.payload == key);
        TASK_CHECK(std::fabs(s.pdfArea - lights->pdfArea(s.faceId)) < 1e-12);
        ++hist[s.faceId];
    }
    for (size_t i = 0; i < lights->faceCount(); ++i) {
        double empirical = double(hist[i]) / double(bins);
        double expected = lights->face(int32(i)).area * y / total;
        TASK_CHECK(hist[i] > 0); // every face reachable
        TASK_CHECK(std::fabs(empirical - expected) < 0.002);
    }

    // In-face positions are area-uniform: mean of each coordinate ~ 0.5.
    double mean1 = 0.0, mean2 = 0.0;
    uint64 picked = 0;
    for (int t = 0; t < 4000; ++t) {
        double u0 = (t * 0.6180339887) - std::floor(t * 0.6180339887);
        lights->sample(u0, ((t * 7) % 101) / 101.0,
                       ((t * 13) % 97) / 97.0, s);
        const SurfaceLightFace &f = lights->face(s.faceId);
        int a1 = (f.axis + 1) % 3, a2 = (f.axis + 2) % 3;
        const double ws = VoxelSurfaceLights::WorldScale;
        double v1 = (double(s.position.a[a1]) - 1.0) / ws;
        double v2 = (double(s.position.a[a2]) - 1.0) / ws;
        mean1 += (v1 - f.low0) / double(f.high0 - f.low0);
        mean2 += (v2 - f.low1) / double(f.high1 - f.low1);
        ++picked;
    }
    TASK_CHECK(std::fabs(mean1 / double(picked) - 0.5) < 0.03);
    TASK_CHECK(std::fabs(mean2 / double(picked) - 0.5) < 0.03);
    delete lights;
    return 0;
}

int distribution_two_keys_and_blackout()
{
    VoxelOctree *tree = loadHandBuiltTree(mixedScene, ".task-build/tc_mixed2.oct");
    VoxelSurfaceLights lights(tree);
    const uint32 A = 0x1001u, B = 0x2002u;

    double areaA = 0.0, areaB = 0.0;
    for (size_t i = 0; i < lights.faceCount(); ++i) {
        const SurfaceLightFace &f = lights.face(int32(i));
        if (f.payload == A) areaA += f.area;
        if (f.payload == B) areaB += f.area;
    }
    TASK_CHECK(areaA > 0.0 && areaB > 0.0);

    Vec3 red(2.0f, 0.0f, 0.0f), green(0.0f, 3.0f, 0.0f);
    lights.setEmission(A, red);
    lights.setEmission(B, green);
    double massA = areaA * luminance(red), massB = areaB * luminance(green);
    TASK_CHECK(std::fabs(lights.totalMass() - (massA + massB)) < 1e-9);

    // Dense stratified u0 sweep (face selection depends only on u0).
    const uint64 bins = 60000;
    uint64 countA = 0, countB = 0;
    SurfaceLightSample s;
    for (uint64 t = 0; t < bins; ++t) {
        double u0 = double(t + 0.5) / double(bins);
        double u1 = double((t * 7 + 3) % 101) / 101.0;
        double u2 = double((t * 13 + 5) % 97) / 97.0;
        TASK_CHECK(lights.sample(u0, u1, u2, s));
        if (s.payload == A) ++countA;
        if (s.payload == B) ++countB;
    }
    TASK_CHECK(countA + countB == bins);
    double ratio = double(countA) / double(countB);
    TASK_CHECK(std::fabs(ratio - massA / massB) / (massA / massB) < 0.02);

    // Full dark restore: black table samples nothing and all PDFs vanish.
    std::vector<int32> ids;
    for (size_t i = 0; i < lights.faceCount(); ++i) ids.push_back(int32(i));
    lights.setEmission(A, Vec3(0, 0, 0));
    lights.setEmission(B, Vec3(0, 0, 0));
    TASK_CHECK(lights.totalMass() == 0.0);
    TASK_CHECK(!lights.sample(0.1, 0.2, 0.3, s));
    for (int32 id : ids)
        TASK_CHECK(lights.pdfArea(id) == 0.0);
    // faceIds survive the blackout.
    TASK_CHECK(lights.faceCount() == ids.size());

    // Invalid radiance throws and leaves the (black) distribution untouched.
    bool threw = false;
    try { lights.setEmission(A, Vec3(-1.0f, 0.0f, 0.0f)); }
    catch (const std::invalid_argument &) { threw = true; }
    try { lights.setEmission(A, Vec3(std::nanf(""), 0.0f, 0.0f)); }
    catch (const std::invalid_argument &) { threw = true; }
    TASK_CHECK(threw);
    TASK_CHECK(lights.totalMass() == 0.0);
    return 0;
}

int distribution_incremental_stats()
{
    VoxelOctree *tree = loadHandBuiltTree(mixedScene, ".task-build/tc_mixed3.oct");
    VoxelSurfaceLights a(tree), b(tree);
    const uint32 A = 0x1001u, B = 0x2002u, C = 0x3003u;

    uint64 buildVisits = a.lastUpdateStats().geometryNodeVisits;
    TASK_CHECK(buildVisits > 0);

    a.setEmission(A, Vec3(1, 0, 0));
    SurfaceLightStats sa = a.lastUpdateStats();
    TASK_CHECK(sa.massNodesVisited > 0 && sa.massNodesUpdated > 0);
    // Toggling lights never rescans the compact geometry.
    TASK_CHECK(a.lastUpdateStats().geometryNodeVisits == buildVisits);

    // Repeating the identical assignment is exactly zero work.
    a.setEmission(A, Vec3(1, 0, 0));
    TASK_CHECK(a.lastUpdateStats().massNodesVisited == 0);
    TASK_CHECK(a.lastUpdateStats().massNodesUpdated == 0);

    // Recoloring A touches only A's patches and their mass ancestors.
    a.setEmission(A, Vec3(2, 0, 0));
    SurfaceLightStats sa2 = a.lastUpdateStats();
    TASK_CHECK(sa2.massNodesVisited > 0 && sa2.massNodesUpdated > 0);
    TASK_CHECK(sa2.massNodesUpdated <= sa2.massNodesVisited);

    // Updating B must not alter A's contribution (key isolation): record the
    // total with only A lit, light B, then black B and expect exact return.
    a.setEmission(B, Vec3(0, 0, 0));
    a.setEmission(C, Vec3(0, 0, 0));
    double massAOnly = a.totalMass();
    a.setEmission(B, Vec3(0, 1, 0));
    SurfaceLightStats sb = a.lastUpdateStats();
    TASK_CHECK(sb.massNodesVisited > 0);
    a.setEmission(B, Vec3(0, 0, 0));
    TASK_CHECK(std::fabs(a.totalMass() - massAOnly) < 1e-12);

    // A payload key that no leaf carries performs no mass work at all and
    // leaves the distribution untouched.
    a.setEmission(0xDEADBEEFu, Vec3(9, 9, 9));
    TASK_CHECK(a.lastUpdateStats().massNodesVisited == 0);
    TASK_CHECK(a.lastUpdateStats().massNodesUpdated == 0);
    TASK_CHECK(std::fabs(a.totalMass() - massAOnly) < 1e-12);

    // C stays dark; assigning it later grows the mass without rescanning.
    a.setEmission(B, Vec3(0, 1, 0));
    double massWithB = a.totalMass();
    a.setEmission(C, Vec3(0, 0, 1));
    TASK_CHECK(a.totalMass() > massWithB);
    TASK_CHECK(a.lastUpdateStats().geometryNodeVisits == buildVisits);

    // Two resources on one tree keep independent emission tables.
    double areaA = 0.0;
    for (size_t i = 0; i < b.faceCount(); ++i)
        if (b.face(int32(i)).payload == A) areaA += b.face(int32(i)).area;
    b.setEmission(A, Vec3(0, 0, 5));
    TASK_CHECK(std::fabs(b.totalMass() - a.totalMass()) > 1e-9);
    b.setEmission(B, Vec3(0, 0, 0)); // no-op on a resource that never lit B
    TASK_CHECK(b.lastUpdateStats().massNodesVisited == 0 &&
                b.lastUpdateStats().massNodesUpdated == 0);
    TASK_CHECK(std::fabs(b.totalMass() -
                areaA * luminance(Vec3(0, 0, 5))) < 1e-9);

    // Interleaved updates preserve both distributions.
    a.setEmission(A, Vec3(0.5f, 0.5f, 0.5f));
    b.setEmission(A, Vec3(0.0f, 0.0f, 5.0f));
    TASK_CHECK(std::fabs(a.totalMass() - b.totalMass()) > 1e-9);
    return 0;
}

// ---------------------------------------------------------------------------
// transport group
// ---------------------------------------------------------------------------

int transport_unblocked_matches_integral()
{
    RawGrid g(16);
    const uint32 E = 0x5005u;
    VoxelOctree *tree = makeTransport(g, false, E, 0x6006u);
    VoxelSurfaceLights lights(tree);
    Vec3 radiance(1.2f, 0.8f, 0.4f);
    lights.setEmission(E, radiance);

    Vec3 P(1.0f + 3.5f / 16.0f, 1.5f, 1.5f), N(1, 0, 0), albedo(0.6f, 0.6f, 0.6f);
    Vec3 ref = referenceDirectMono(lights, radiance, P, N, albedo, 24);
    double got = avgEstimate(lights, P, N, albedo, 14);
    double want = ref.x + ref.y + ref.z;
    TASK_CHECK(want > 1e-4);
    TASK_CHECK(std::fabs(got - want) / want < 0.08);
    return 0;
}

int transport_occluder_and_thin_blocker()
{
    RawGrid g(16);
    const uint32 E = 0x5005u, D = 0x6006u;
    VoxelOctree *tree = makeTransport(g, true, E, D);
    VoxelSurfaceLights lights(tree);
    lights.setEmission(E, Vec3(2, 2, 2)); // D is never lit: non-emissive blocker

    Vec3 P(1.0f + 3.5f / 16.0f, 1.5f, 1.5f), N(1, 0, 0), albedo(0.5f, 0.5f, 0.5f);
    double blocked = avgEstimate(lights, P, N, albedo, 12);
    TASK_CHECK(blocked < 1e-5);

    // A one-cell-thick partial blocker occludes only the rays through its
    // 3x3 in-plane footprint (no epsilon gap), while rays around it survive.
    // Compare against the exact same scene with the blocker cells removed.
    RawGrid thin(16), open(16);
    for (int y = 4; y < 12; ++y)
    for (int z = 4; z < 12; ++z) {
        thin.set(12, y, z, E);
        open.set(12, y, z, E);
    }
    for (int y = 7; y < 10; ++y)
    for (int z = 7; z < 10; ++z)
        thin.set(8, y, z, D);
    VoxelOctree *thinTree = buildTree(thin, ".task-build/tc_thin.raw");
    VoxelOctree *openTree = buildTree(open, ".task-build/tc_open.raw");
    VoxelSurfaceLights thinLights(thinTree), openLights(openTree);
    thinLights.setEmission(E, Vec3(2, 2, 2));
    openLights.setEmission(E, Vec3(2, 2, 2));
    double thinV = avgEstimate(thinLights, P, N, albedo, 12);
    double openV = avgEstimate(openLights, P, N, albedo, 12);
    TASK_CHECK(openV > 1e-3);
    TASK_CHECK(thinV < openV);             // the thin slab removes some light
    TASK_CHECK(thinV > 0.0);               // but does not block the whole emitter
    return 0;
}

int transport_endpoints_and_backfaces()
{
    RawGrid g(16);
    const uint32 E = 0x5005u;
    VoxelOctree *tree = makeTransport(g, false, E, 0);
    VoxelSurfaceLights lights(tree);
    lights.setEmission(E, Vec3(1, 1, 1));

    Vec3 albedo(0.5f, 0.5f, 0.5f);
    Vec3 P(1.0f + 3.5f / 16.0f, 1.5f, 1.5f);

    // Receiver normal facing away gives zero.
    TASK_CHECK((estimateDirect(lights, P, Vec3(-1, 0, 0), albedo, 0.3, 0.4, 0.5)
                .length()) == 0.0f);

    // Point on a receiver surface: no self-occlusion at the start contact.
    RawGrid surf(16);
    for (int y = 4; y < 12; ++y)
    for (int z = 4; z < 12; ++z) {
        surf.set(12, y, z, E);
        surf.set(3, y, z, 0x7007u);
    }
    VoxelOctree *tree2 = buildTree(surf, ".task-build/tc_surf.raw");
    VoxelSurfaceLights lights2(tree2);
    lights2.setEmission(E, Vec3(1, 1, 1));
    Vec3 Psurf(1.0f + 4.0f / 16.0f, 1.5f, 1.5f), N(1, 0, 0);
    double v = avgEstimate(lights2, Psurf, N, albedo, 12);
    TASK_CHECK(v > 1e-4);

    // Zero distance (receiver coincides with a light sample point) is zero.
    SurfaceLightSample s;
    TASK_CHECK(lights.sample(0.5, 0.5, 0.5, s));
    Vec3 zero = estimateDirect(lights, s.position, N, albedo, 0.5, 0.5, 0.5);
    TASK_CHECK(zero.length() == 0.0f);

    // Validation of inputs.
    bool threw = false;
    try { estimateDirect(lights, P, Vec3(1, 1, 0), albedo, 0.1, 0.2, 0.3); }
    catch (const std::invalid_argument &) { threw = true; }
    try { estimateDirect(lights, P, N, Vec3(-1, 0, 0), 0.1, 0.2, 0.3); }
    catch (const std::invalid_argument &) { threw = true; }
    try { estimateDirect(lights, P, N, albedo, 1.0, 0.2, 0.3); }
    catch (const std::invalid_argument &) { threw = true; }
    TASK_CHECK(threw);
    return 0;
}

} // namespace vsl_test

const task_case task_cases[] = {
    {"exposure", "block_faces_and_internal_elimination", vsl_test::exposure_block_faces},
    {"exposure", "mixed_depth_partial_coverage", vsl_test::exposure_mixed_depth},
    {"exposure", "long_double_word_offsets", vsl_test::exposure_long_offsets},
    {"distribution", "mass_pdf_and_area_uniformity", vsl_test::distribution_mass_and_pdf},
    {"distribution", "two_keys_blackout_validation", vsl_test::distribution_two_keys_and_blackout},
    {"distribution", "incremental_update_stats", vsl_test::distribution_incremental_stats},
    {"transport", "unblocked_vs_independent_integral", vsl_test::transport_unblocked_matches_integral},
    {"transport", "occluder_and_thin_blocker", vsl_test::transport_occluder_and_thin_blocker},
    {"transport", "endpoints_backfaces_validation", vsl_test::transport_endpoints_and_backfaces}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
