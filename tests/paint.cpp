/*
   Task tests for direct box editing of VoxelOctree.

   Three registered groups:
     brush         - paint/erase semantics, coarse leaf split/merge, word counts,
                     depth-20 structural bounds and raymarch results checked against
                     an independent integer-grid material array over eight directions.
     mixed_layout  - an independently hand-encoded descriptor file containing a mixed
                     leaf/internal group, a double-wide group and an extended (35 bit)
                     child offset; loaded and queried directly.
     persistence   - paint -> save -> load round trips without any VoxelData/raw file,
                     center preserved, including a round trip of the hand-built file.

   There is deliberately no main(); task_support/driver.c provides it.
*/

#include "task_support/check.h"

#include "VoxelOctree.hpp"
#include "VoxelData.hpp"
#include "Util.hpp"
#include "thread/ThreadUtils.hpp"
#include "thread/ThreadPool.hpp"
#include "third-party/lz4.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

typedef std::uint32_t u32;
typedef std::uint64_t u64;

#define TASK_CHECK_PRIMITIVE(x) do { if (!(x)) std::abort(); } while (0)

// The thread pool is process-wide: create it once and leave it running so the
// condition-variable machinery is never repeatedly torn down and recreated.
void ensureThreads() {
    if (!ThreadUtils::pool)
        ThreadUtils::startThreads(2);
}

namespace {

const u32 MatA = 0xDEADBEEFu;
const u32 MatB = 0x12345678u;
const u32 MatC = 0x87654321u;
const u32 MatD = 0xABCDEF01u;
const u32 MatE = 0x0F0F0F0Fu;
const u32 MatF = 0xF0F0F0F0u;

struct Box {
    float lo[3], hi[3];
    u32 material;
};

struct RayHit {
    bool hit;
    float t;
    u32 material;
};

// Independent slab-method ray/box intersection; reference for the DDA raymarcher.
bool intersectBox(const float o[3], const float d[3], const Box &b, float &tOut) {
    float tNear = -1e30f, tFar = 1e30f;
    for (int a = 0; a < 3; ++a) {
        float t1 = (b.lo[a] - o[a]) / d[a];
        float t2 = (b.hi[a] - o[a]) / d[a];
        float tn = std::fmin(t1, t2);
        float tf = std::fmax(t1, t2);
        if (tn > tNear) tNear = tn;
        if (tf < tFar) tFar = tf;
    }
    if (tNear > tFar || tFar < 0.0f)
        return false;
    tOut = std::fmax(tNear, 0.0f);
    return true;
}

RayHit referenceCast(const std::vector<Box> &boxes, const float o[3], const float d[3]) {
    RayHit r;
    r.hit = false;
    r.t = 1e30f;
    r.material = 0;
    for (size_t i = 0; i < boxes.size(); ++i) {
        float t;
        if (intersectBox(o, d, boxes[i], t) && (!r.hit || t < r.t)) {
            r.hit = true;
            r.t = t;
            r.material = boxes[i].material;
        }
    }
    return r;
}

// Octant geometry shared with the builder: set bit -> low half on that axis.
void childBox(const float lo[3], const float hi[3], int child, Box &out) {
    float mid[3];
    for (int a = 0; a < 3; ++a)
        mid[a] = 0.5f * (lo[a] + hi[a]);
    for (int a = 0; a < 3; ++a) {
        bool low = (child & (1 << a)) != 0;
        out.lo[a] = low ? lo[a] : mid[a];
        out.hi[a] = low ? mid[a] : hi[a];
    }
}

// Eight incidence directions, jittered so rays never run on a grid plane or edge.
void makeRay(int dirIndex, const float target[3], float o[3], float d[3]) {
    const float base[3] = {1.0f, 0.73f, 1.31f};
    for (int a = 0; a < 3; ++a) {
        float sign = (dirIndex & (1 << a)) ? -1.0f : 1.0f;
        d[a] = sign * base[a];
        o[a] = target[a] - d[a] * 4.0f;
    }
}

bool compareRay(VoxelOctree &tree, const std::vector<Box> &ref,
                const float o[3], const float d[3]) {
    RayHit expect = referenceCast(ref, o, d);
    u32 normal = 0;
    float t = -1.0f;
    bool got = tree.raymarch(Vec3(o[0], o[1], o[2]), Vec3(d[0], d[1], d[2]), 0.0f, normal, t);
    if (got != expect.hit)
        return false;
    if (!got)
        return true;
    if (normal != expect.material)
        return false;
    return std::fabs(t - expect.t) <= 2e-4f * std::fmax(1.0f, std::fabs(expect.t));
}

// Check a tree against the reference boxes from all eight incidence directions.
bool checkAllDirections(VoxelOctree &tree, const std::vector<Box> &ref) {
    const float target[3] = {1.47f, 1.53f, 1.61f};
    for (int dir = 0; dir < 8; ++dir) {
        float o[3], d[3];
        makeRay(dir, target, o, d);
        if (!compareRay(tree, ref, o, d))
            return false;
    }
    return true;
}

// --- Dense integer-grid reference for the small brush scene -----------------

struct Grid {
    int n;
    std::vector<u32> cell;

    Grid(int n) : n(n), cell(size_t(n) * n * n, 0) {}

    u32 &at(int x, int y, int z) {
        return cell[size_t(x) + n * (size_t(y) + n * z)];
    }

    void paint(const std::array<u32, 3> &lo, const std::array<u32, 3> &hi, u32 mat) {
        for (u32 z = lo[2]; z < hi[2]; ++z)
            for (u32 y = lo[1]; y < hi[1]; ++y)
                for (u32 x = lo[0]; x < hi[0]; ++x)
                    at(int(x), int(y), int(z)) = mat;
    }

    std::vector<Box> boxes() const {
        std::vector<Box> result;
        float inv = 1.0f / float(n);
        for (int z = 0; z < n; ++z)
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    u32 m = cell[size_t(x) + n * (size_t(y) + n * z)];
                    if (!m)
                        continue;
                    Box b;
                    b.lo[0] = 1.0f + x * inv;
                    b.lo[1] = 1.0f + y * inv;
                    b.lo[2] = 1.0f + z * inv;
                    b.hi[0] = 1.0f + (x + 1) * inv;
                    b.hi[1] = 1.0f + (y + 1) * inv;
                    b.hi[2] = 1.0f + (z + 1) * inv;
                    b.material = m;
                    result.push_back(b);
                }
        return result;
    }
};

// Write a descriptor word array using the original file/LZ4 container format.
void writeOctFile(const char *path, const Vec3 &center, const std::vector<u32> &words) {
    FILE *fp = std::fopen(path, "wb");
    if (!fp) {
        std::perror(path);
        return;
    }
    std::fwrite(center.a, sizeof(float), 3, fp);
    u64 size = words.size();
    std::fwrite(&size, sizeof(u64), 1, fp);

    LZ4_stream_t *stream = LZ4_createStream();
    LZ4_resetStream(stream);
    std::vector<char> buffer(LZ4_compressBound(int(size * sizeof(u32))));
    int compSize = LZ4_compress_continue(
        stream, reinterpret_cast<const char *>(words.data()), buffer.data(),
        int(size * sizeof(u32)));
    u64 comp64 = compSize;
    std::fwrite(&comp64, sizeof(u64), 1, fp);
    std::fwrite(buffer.data(), 1, compSize, fp);
    LZ4_freeStream(stream);
    std::fclose(fp);
}

// Read back the raw descriptor words written by VoxelOctree::save (single block).
std::vector<u32> readOctWords(const char *path) {
    FILE *fp = std::fopen(path, "rb");
    TASK_CHECK_PRIMITIVE(fp != 0);
    float center[3];
    u64 size = 0;
    std::fread(center, sizeof(float), 3, fp);
    std::fread(&size, sizeof(u64), 1, fp);
    u64 comp64 = 0;
    std::fread(&comp64, sizeof(u64), 1, fp);
    std::vector<char> comp(comp64);
    std::fread(comp.data(), 1, comp64, fp);
    std::fclose(fp);

    std::vector<u32> words(size);
    int dec = LZ4_decompress_safe(comp.data(),
                                  reinterpret_cast<char *>(words.data()),
                                  int(comp64), int(size * sizeof(u32)));
    TASK_CHECK_PRIMITIVE(dec == int(size * sizeof(u32)));
    return words;
}

// Walk a descriptor array the same way the ray marcher addresses it and count the
// wide (0x10000) and individually extended (0x20000) internal descriptors.
void countExtensionDescriptors(const std::vector<u32> &w, u64 idx, bool isRoot,
                               int &wide, int &extended) {
    u32 word = w[size_t(idx)];
    u32 childMask = (word >> 8) & 0xFF;
    u32 internalMask = word & 0xFF;
    bool doubleWide = (word & 0x10000) != 0;
    if (doubleWide)
        ++wide;
    if (!isRoot && (word & 0x20000))
        ++extended;

    u64 groupBase;
    if (isRoot) {
        groupBase = idx + 1;
    } else {
        u64 off = word >> 18;
        if (word & 0x20000)
            off = (off << 32) | u64(w[size_t(idx + 1)]);
        groupBase = idx + off;
    }

    u64 wordPos = 0;
    for (int i = 7; i >= 0; --i) {
        u32 bit = 128u >> i;
        if (!(childMask & bit))
            continue;
        u64 childWord = groupBase + wordPos;
        if (internalMask & bit) {
            countExtensionDescriptors(w, childWord, false, wide, extended);
            wordPos += doubleWide ? 2 : 1;
        } else {
            wordPos += 1;
        }
    }
}

VoxelOctree *treeFromRaw(const char *rawPath, int n) {
    FILE *f = std::fopen(rawPath, "wb");
    int dim[3] = {n, n, n};
    std::fwrite(dim, sizeof(int), 3, f);
    u32 zero = 0;
    for (int i = 0; i < n * n * n; ++i)
        std::fwrite(&zero, sizeof(u32), 1, f);
    std::fclose(f);

    // Kept alive for the test run: VoxelOctree stores a non-owning pointer until
    // the first paintBox clears it.
    VoxelData *vox = new VoxelData(rawPath, 1024 * 1024);
    return new VoxelOctree(vox);
}

// ============================================================================
// brush group
// ============================================================================

int brushOverlapAndErase() {
    ensureThreads();
    const int n = 8;
    std::unique_ptr<VoxelOctree> tree(treeFromRaw(".task-build/brush.raw", n));
    Grid ref(n);

    // Full-scene fill collapses to root plus eight identical leaves.
    std::array<u32, 3> zero = {0, 0, 0};
    std::array<u32, 3> all = {u32(n), u32(n), u32(n)};
    tree->paintBox(3, zero, all, MatA);
    ref.paint(zero, all, MatA);
    TASK_CHECK(tree->wordCount() == 9);

    // Overlapping overwrite with a full 32 bit material; crosses the mid planes.
    std::array<u32, 3> lo = {2, 2, 2};
    std::array<u32, 3> hi = {6, 6, 6};
    tree->paintBox(3, lo, hi, MatB);
    ref.paint(lo, hi, MatB);
    TASK_CHECK(checkAllDirections(*tree, ref.boxes()));

    // Erase a box straddling the x and y mid planes, splitting coarse leaves.
    std::array<u32, 3> elo = {3, 3, 1};
    std::array<u32, 3> ehi = {5, 5, 7};
    tree->paintBox(3, elo, ehi, 0);
    ref.paint(elo, ehi, 0);
    TASK_CHECK(checkAllDirections(*tree, ref.boxes()));

    // A finer grid (depth 4) only relocates the brush; existing content is untouched.
    std::array<u32, 3> flo = {0, 0, 0};
    std::array<u32, 3> fhi = {2, 2, 2}; // N=16 cells: the [0,1/8) corner region.
    tree->paintBox(4, flo, fhi, MatC);
    std::array<u32, 3> clo = {0, 0, 0};
    std::array<u32, 3> chi = {1, 1, 1};
    ref.paint(clo, chi, MatC);
    TASK_CHECK(checkAllDirections(*tree, ref.boxes()));

    // Empty brush changes nothing.
    u64 before = tree->wordCount();
    std::array<u32, 3> empty = {3, 3, 3};
    tree->paintBox(3, empty, empty, MatD);
    TASK_CHECK(tree->wordCount() == before);
    TASK_CHECK(checkAllDirections(*tree, ref.boxes()));

    // Full erase leaves a single-word empty root.
    tree->paintBox(3, zero, all, 0);
    TASK_CHECK(tree->wordCount() == 1);

    return 0;
}

int brushSplitMerge() {
    ensureThreads();
    const int n = 8;
    std::unique_ptr<VoxelOctree> tree(treeFromRaw(".task-build/merge.raw", n));
    Grid ref(n);
    std::array<u32, 3> zero = {0, 0, 0};
    std::array<u32, 3> all = {u32(n), u32(n), u32(n)};

    tree->paintBox(3, zero, all, MatD);
    ref.paint(zero, all, MatD);
    TASK_CHECK(tree->wordCount() == 9);

    // Dig one finest cell out of a uniform solid: coarse leaves split along the path.
    std::array<u32, 3> clo = {4, 4, 4};
    std::array<u32, 3> chi = {5, 5, 5};
    tree->paintBox(3, clo, chi, 0);
    ref.paint(clo, chi, 0);
    TASK_CHECK(tree->wordCount() > 9);
    TASK_CHECK(checkAllDirections(*tree, ref.boxes()));

    // Put the same material back: sibling leaves merge again down to nine words.
    tree->paintBox(3, clo, chi, MatD);
    ref.paint(clo, chi, MatD);
    TASK_CHECK(tree->wordCount() == 9);
    TASK_CHECK(checkAllDirections(*tree, ref.boxes()));

    // A different material in that cell must not merge, but stays compact.
    tree->paintBox(3, clo, chi, MatA);
    ref.paint(clo, chi, MatA);
    TASK_CHECK(tree->wordCount() > 9);
    TASK_CHECK(checkAllDirections(*tree, ref.boxes()));

    return 0;
}

int brushDepth20() {
    ensureThreads();
    std::unique_ptr<VoxelOctree> tree(treeFromRaw(".task-build/depth20.raw", 8));

    const unsigned depth = 20;
    const u32 N = 1u << depth;
    std::array<u32, 3> zero = {0, 0, 0};
    std::array<u32, 3> all = {N, N, N};

    // Whole-scene fill at the finest supported grid must not touch the dense grid.
    tree->paintBox(depth, zero, all, MatB);
    TASK_CHECK(tree->wordCount() == 9);

    // Remove a single finest cell. The touched path is only `depth` nodes long.
    std::array<u32, 3> lo = {12345, 23456, 34567};
    std::array<u32, 3> hi = {12346, 23457, 34568};
    tree->paintBox(depth, lo, hi, 0);
    TASK_CHECK(tree->wordCount() <= 512);

    // Restore it; identical solid leaves merge and the tree returns to nine words.
    tree->paintBox(depth, lo, hi, MatB);
    TASK_CHECK(tree->wordCount() == 9);

    return 0;
}

// ============================================================================
// mixed_layout group: independently encoded legal descriptors
// ============================================================================

// Independently hand-encode a legal descriptor array no production code emits.
// It exercises every addressing variant at once:
//   root : a MIXED single-wide group (two leaf records + one internal child A);
//   A    : an ALL-INTERNAL double-wide group (0x10000), two 2-word child slots;
//   B    : a leaf group reached through a 35 bit extended offset (0x20000), placed
//          beyond the 14 bit short-address range by an unreachable filler block.
// Mask polarity: a SET low-byte bit marks an internal child, a CLEAR bit a leaf;
// the high byte only records presence. A double-wide group is always all internal.
std::vector<u32> buildIndependentDescriptors(Vec3 &center) {
    center = Vec3(0.25f, -0.5f, 3.0f);
    const u32 farGroup = 20010; // B's group base, past the 0x3FFF short range

    std::vector<u32> w;
    w.push_back(0);   // 0: root descriptor (group base 1, single-wide mixed)
    w.push_back(MatD); // 1: root leaf child i=7
    w.push_back(MatC); // 2: root leaf child i=2
    w.push_back(0);   // 3: A descriptor (group base 4, double-wide all internal)
    w.push_back(0);   // 4: E descriptor (group base 8)
    w.push_back(0);   // 5: E address extension (unused, zero)
    w.push_back(0);   // 6: B descriptor (group far away, extended offset)
    w.push_back(0);   // 7: B address extension word
    w.push_back(MatF); // 8: E leaf child i=7
    w.push_back(MatE); // 9: E leaf child i=6
    w.resize(farGroup, 0);         // 10..farGroup-1: unreachable filler
    w.push_back(MatB);             // farGroup+0: B leaf child i=4
    w.push_back(MatA);             // farGroup+1: B leaf child i=3

    // Root: present {0,2,7}, only i=0 internal; single wide; group offset 1.
    u32 rootPresent = (128 >> 0) | (128 >> 2) | (128 >> 7); // 0xA1
    w[0] = (rootPresent << 8) | (128 >> 0) | (1u << 18);

    // A: present {0,1}, both internal -> double wide; group base 4, offset 1.
    u32 aPresent = (128 >> 0) | (128 >> 1);                 // 0xC0
    w[3] = (aPresent << 8) | aPresent | 0x10000 | (1u << 18);

    // E: present {6,7}, both leaves; group base 8, short offset 4.
    u32 ePresent = (128 >> 6) | (128 >> 7);                 // 0x03
    w[4] = (ePresent << 8) | (4u << 18);

    // B: present {3,4}, both leaves; group base farGroup via extended offset.
    u32 bPresent = (128 >> 3) | (128 >> 4);                 // 0x18
    u64 bOffset = u64(farGroup) - 6;
    TASK_CHECK_PRIMITIVE(bOffset > 0x3FFF);
    w[6] = (bPresent << 8) | 0x20000 | u32((bOffset >> 32) << 18);
    w[7] = u32(bOffset);
    return w;
}

// Reference geometry for the descriptor array above.
std::vector<Box> independentReference() {
    float rootLo[3] = {1, 1, 1}, rootHi[3] = {2, 2, 2};
    std::vector<Box> result;

    Box leaf7, leaf2, aBox, eBox, bBox, e7, e6, b4, b3;
    childBox(rootLo, rootHi, 7, leaf7);
    leaf7.material = MatD;
    childBox(rootLo, rootHi, 2, leaf2);
    leaf2.material = MatC;
    childBox(rootLo, rootHi, 0, aBox);   // A = root octant 0
    childBox(aBox.lo, aBox.hi, 1, eBox); // E = A octant 1
    childBox(eBox.lo, eBox.hi, 7, e7);
    e7.material = MatF;
    childBox(eBox.lo, eBox.hi, 6, e6);
    e6.material = MatE;
    childBox(aBox.lo, aBox.hi, 0, bBox); // B = A octant 0
    childBox(bBox.lo, bBox.hi, 4, b4);
    b4.material = MatB;
    childBox(bBox.lo, bBox.hi, 3, b3);
    b3.material = MatA;

    result.push_back(leaf7);
    result.push_back(leaf2);
    result.push_back(e7);
    result.push_back(e6);
    result.push_back(b4);
    result.push_back(b3);
    return result;
}

int mixedQuery() {
    Vec3 center;
    std::vector<u32> words = buildIndependentDescriptors(center);
    writeOctFile(".task-build/mixed.oct", center, words);

    VoxelOctree loaded(".task-build/mixed.oct");
    TASK_CHECK(loaded.wordCount() == words.size());
    TASK_CHECK(loaded.center() == center);

    std::vector<Box> ref = independentReference();
    const float target[3] = {1.62f, 1.41f, 1.55f};
    for (int dir = 0; dir < 8; ++dir) {
        float o[3], d[3];
        makeRay(dir, target, o, d);
        TASK_CHECK(compareRay(loaded, ref, o, d));
    }

    // Erase the i=7 corner (cell [0,1) on a depth-1 grid). Re-serialization must
    // drop the unreachable filler instead of expanding anything densely.
    std::array<u32, 3> lo = {0, 0, 0};
    std::array<u32, 3> hi = {1, 1, 1};
    loaded.paintBox(1, lo, hi, 0);
    TASK_CHECK(loaded.wordCount() < 100);

    std::vector<Box> after;
    for (size_t i = 0; i < ref.size(); ++i)
        if (ref[i].material != MatD)
            after.push_back(ref[i]);
    for (int dir = 0; dir < 8; ++dir) {
        float o[3], d[3];
        makeRay(dir, target, o, d);
        TASK_CHECK(compareRay(loaded, after, o, d));
    }
    TASK_CHECK(loaded.center() == center);
    return 0;
}

int mixedRepaintMaterial() {
    Vec3 center;
    std::vector<u32> words = buildIndependentDescriptors(center);
    writeOctFile(".task-build/mixed2.oct", center, words);
    VoxelOctree loaded(".task-build/mixed2.oct");

    // Overwrite the whole i=0 octant with one solid material, replacing the internal
    // node A (and both of its nested groups) by a coarse leaf while the root's leaf
    // children stay leaves.
    std::array<u32, 3> lo = {1, 1, 1};
    std::array<u32, 3> hi = {2, 2, 2};
    loaded.paintBox(1, lo, hi, MatD);

    // Post-edit reference: the two untouched root leaves plus a fully solid octant 0.
    float rootLo[3] = {1, 1, 1}, rootHi[3] = {2, 2, 2};
    std::vector<Box> ref;
    Box leaf7, leaf2, solid0;
    childBox(rootLo, rootHi, 7, leaf7);
    leaf7.material = MatD;
    childBox(rootLo, rootHi, 2, leaf2);
    leaf2.material = MatC;
    childBox(rootLo, rootHi, 0, solid0);
    solid0.material = MatD;
    ref.push_back(leaf7);
    ref.push_back(leaf2);
    ref.push_back(solid0);

    const float target[3] = {1.55f, 1.6f, 1.45f};
    for (int dir = 0; dir < 8; ++dir) {
        float o[3], d[3];
        makeRay(dir, target, o, d);
        TASK_CHECK(compareRay(loaded, ref, o, d));
    }
    TASK_CHECK(loaded.wordCount() < 100);
    return 0;
}

// The editor's own serializer must promote groups to wide/extended addressing once
// a child offset exceeds the 14 bit short range. Build a genuinely large reachable
// tree (a hittable solid slab plus a sparse lattice) rather than relying on the
// compact hand-made file above.
int editorWideOffsets() {
    ensureThreads();
    std::unique_ptr<VoxelOctree> tree(treeFromRaw(".task-build/editorwide.raw", 8));

    const unsigned depth = 12;
    const u32 N = 1u << depth;
    // Solid slab over the first x half: a reliable exact-query target.
    std::array<u32, 3> slabLo = {0, 0, 0};
    std::array<u32, 3> slabHi = {N / 2, N, N};
    tree->paintBox(depth, slabLo, slabHi, MatA);

    // Many small solid boxes in the second x half push the reachable word count past
    // the 14 bit offset boundary, forcing wide groups in the serialized output.
    const int per = 10;
    const u32 step = N / u32(per);
    for (int iy = 0; iy < per; ++iy)
        for (int iz = 0; iz < per; ++iz)
            for (int ix = 0; ix < per / 2; ++ix) {
                u32 x = N / 2 + u32(ix) * step + 2;
                u32 y = u32(iy) * step + 2;
                u32 z = u32(iz) * step + 2;
                std::array<u32, 3> lo = {x, y, z};
                std::array<u32, 3> hi = {x + 4, y + 4, z + 4};
                if (hi[0] > N || hi[1] > N || hi[2] > N)
                    continue;
                tree->paintBox(depth, lo, hi, MatB);
            }

    TASK_CHECK(tree->wordCount() > 0x3FFF);
    tree->save(".task-build/editorwide.oct");

    std::vector<u32> words = readOctWords(".task-build/editorwide.oct");
    TASK_CHECK(words.size() == size_t(tree->wordCount()));
    int wide = 0, extended = 0;
    countExtensionDescriptors(words, 0, true, wide, extended);
    TASK_CHECK(wide > 0);     // the editor emitted double-wide groups itself
    TASK_CHECK(extended > 0);  // some child offsets needed the 35 bit extension word

    // The original load constructor reads the wide/extended file back unchanged.
    VoxelOctree reloaded(".task-build/editorwide.oct");
    TASK_CHECK(reloaded.wordCount() == tree->wordCount());

    // Exact query through the solid slab returns the full material and a stable t,
    // both before and after the round trip.
    Vec3 o(0.2f, 1.4f, 1.6f), d(1.0f, 0.01f, 0.02f);
    u32 n1 = 0, n2 = 0;
    float t1 = -1.0f, t2 = -1.0f;
    TASK_CHECK(tree->raymarch(o, d, 0.0f, n1, t1));
    TASK_CHECK(reloaded.raymarch(o, d, 0.0f, n2, t2));
    TASK_CHECK(n1 == MatA && n2 == MatA);
    TASK_CHECK(std::fabs(t1 - t2) <= 1e-5f);

    // Keep editing the object with no VoxelData/raw backing: a full erase empties it.
    std::array<u32, 3> eraseLo = {0, 0, 0};
    std::array<u32, 3> eraseHi = {N, N, N};
    reloaded.paintBox(depth, eraseLo, eraseHi, 0);
    TASK_CHECK(reloaded.wordCount() == 1);
    u32 n3 = 0;
    float t3 = -1.0f;
    TASK_CHECK(!reloaded.raymarch(o, d, 0.0f, n3, t3));
    return 0;
}

// ============================================================================
// persistence group
// ============================================================================

int paintSaveLoad() {
    ensureThreads();
    std::unique_ptr<VoxelOctree> tree(treeFromRaw(".task-build/persist.raw", 8));
    std::array<u32, 3> lo = {1, 2, 3};
    std::array<u32, 3> hi = {7, 6, 5};
    tree->paintBox(3, lo, hi, MatA);
    std::array<u32, 3> lo2 = {3, 0, 0};
    std::array<u32, 3> hi2 = {4, 8, 8};
    tree->paintBox(3, lo2, hi2, 0);
    u64 wordsBefore = tree->wordCount();

    tree->save(".task-build/persist.oct");
    VoxelOctree reloaded(".task-build/persist.oct");
    TASK_CHECK(reloaded.wordCount() == wordsBefore);
    TASK_CHECK(reloaded.center() == tree->center());

    // No VoxelData and no raw file backing this object: keep editing it.
    std::array<u32, 3> fill = {0, 0, 0};
    std::array<u32, 3> fillHi = {8, 8, 8};
    reloaded.paintBox(3, fill, fillHi, MatC);
    TASK_CHECK(reloaded.wordCount() == 9);
    reloaded.save(".task-build/persist2.oct");

    VoxelOctree again(".task-build/persist2.oct");
    TASK_CHECK(again.wordCount() == 9);
    TASK_CHECK(again.center() == tree->center());
    u32 normal = 0;
    float t = -1.0f;
    TASK_CHECK(again.raymarch(Vec3(0.0f, 1.4f, 1.6f), Vec3(1.0f, 0.01f, 0.02f), 0.0f, normal, t));
    TASK_CHECK(normal == MatC);

    return 0;
}

int independentRoundTrip() {
    Vec3 center;
    std::vector<u32> words = buildIndependentDescriptors(center);
    writeOctFile(".task-build/indep.oct", center, words);

    // The original loading constructor must accept the independently built file,
    // and re-saving it through the production code must remain readable.
    VoxelOctree first(".task-build/indep.oct");
    TASK_CHECK(first.wordCount() == words.size());
    first.save(".task-build/indep_out.oct");

    VoxelOctree second(".task-build/indep_out.oct");
    std::vector<Box> ref = independentReference();
    const float target[3] = {1.62f, 1.41f, 1.55f};
    for (int dir = 0; dir < 8; ++dir) {
        float o[3], d[3];
        makeRay(dir, target, o, d);
        TASK_CHECK(compareRay(second, ref, o, d));
    }
    TASK_CHECK(second.center() == center);
    return 0;
}

int approximateRayScaleKept() {
    // rayScale=0 stays an exact query; a positive rayScale keeps the original coarse
    // early-hit behavior (which deliberately returns without filling in a material).
    ensureThreads();

    // Paint-built uniform solid: root plus eight identical leaves, nine words.
    std::unique_ptr<VoxelOctree> painted(treeFromRaw(".task-build/scale.raw", 8));
    std::array<u32, 3> zero = {0, 0, 0};
    std::array<u32, 3> all = {8, 8, 8};
    painted->paintBox(3, zero, all, MatB);

    // Reference uniform solid built through the original VoxelData construction path.
    FILE *rf = std::fopen(".task-build/scale_ref.raw", "wb");
    int dim[3] = {8, 8, 8};
    std::fwrite(dim, sizeof(int), 3, rf);
    for (int i = 0; i < 512; ++i)
        std::fwrite(&MatB, sizeof(u32), 1, rf);
    std::fclose(rf);
    VoxelData refVox(".task-build/scale_ref.raw", 1024 * 1024);
    VoxelOctree built(&refVox);

    Vec3 o(0.0f, 1.4f, 1.6f), d(1.0f, 0.01f, 0.02f);

    // Exact query returns the full 32 bit material and the leaf-level entry distance.
    u32 nExact = 0;
    float tExact = -1.0f;
    TASK_CHECK(painted->raymarch(o, d, 0.0f, nExact, tExact));
    TASK_CHECK(nExact == MatB);

    u32 nBuilt = 0;
    float tBuilt = -1.0f;
    TASK_CHECK(built.raymarch(o, d, 0.0f, nBuilt, tBuilt));
    TASK_CHECK(nBuilt == MatB);
    TASK_CHECK(std::fabs(tExact - tBuilt) <= 1e-5f);

    // Approximate query: same hit distance on both trees, proving the coarse path is
    // unchanged even though it does not promise a material.
    float tApproxPainted = -1.0f, tApproxBuilt = -1.0f;
    u32 unused1 = 0, unused2 = 0;
    TASK_CHECK(painted->raymarch(o, d, 1.0f, unused1, tApproxPainted));
    TASK_CHECK(built.raymarch(o, d, 1.0f, unused2, tApproxBuilt));
    TASK_CHECK(tApproxPainted >= 0.0f && tApproxBuilt >= 0.0f);
    TASK_CHECK(std::fabs(tApproxPainted - tApproxBuilt) <= 1e-5f);

    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"brush", "overlap_and_erase", brushOverlapAndErase},
    {"brush", "split_and_merge", brushSplitMerge},
    {"brush", "depth20_bounds", brushDepth20},
    {"mixed_layout", "independent_query", mixedQuery},
    {"mixed_layout", "coarse_repaint", mixedRepaintMaterial},
    {"mixed_layout", "editor_wide_offsets", editorWideOffsets},
    {"persistence", "paint_save_load", paintSaveLoad},
    {"persistence", "independent_round_trip", independentRoundTrip},
    {"persistence", "ray_scale_behavior", approximateRayScaleKept},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
