/* Tests for VoxelOctree::paintBox and wordCount.
 *
 * Every group keeps its own independent ground truth: small scenes are mirrored
 * in a dense integer-grid material array and queried through exact ray/box
 * intersection, while large scenes are checked through an independent
 * descriptor walker that only relies on the file format. Descriptor files used
 * by the mixed-layout group are assembled by hand rather than produced by the
 * code under test. */

#include "../task_support/check.h"

#include "VoxelOctree.hpp"
#include "VoxelData.hpp"
#include "IntTypes.hpp"

#include "third-party/lz4.h"

#include "thread/ThreadUtils.hpp"
#include "thread/ThreadPool.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

/* ------------------------------------------------------------------ */
/* Octree file helpers (the on-disk structure produced by save()).    */

static const size_t BlockSize = 64*1024*1024;

static bool writeOctFile(const char *path, const float center[3],
                         const std::vector<uint32> &words) {
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return false;

    fwrite(center, sizeof(float), 3, fp);
    uint64 size = uint64(words.size());
    fwrite(&size, sizeof(uint64), 1, fp);

    if (size) {
        std::unique_ptr<char[]> buffer(new char[LZ4_compressBound(BlockSize)]);
        const char *src = reinterpret_cast<const char *>(words.data());
        LZ4_stream_t *stream = LZ4_createStream();
        LZ4_resetStream(stream);

        for (uint64 offset = 0; offset < size*4; offset += BlockSize) {
            int outSize = int(std::min<uint64>(size*4 - offset, BlockSize));
            uint64 compSize = LZ4_compress_continue(stream, src + offset,
                                                    buffer.get(), outSize);
            fwrite(&compSize, sizeof(uint64), 1, fp);
            fwrite(buffer.get(), 1, size_t(compSize), fp);
        }
        LZ4_freeStream(stream);
    }

    fclose(fp);
    return true;
}

struct OctFile {
    float center[3];
    std::vector<uint32> words;
};

static bool readOctFile(const char *path, OctFile &out) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return false;

    if (fread(out.center, sizeof(float), 3, fp) != 3) {
        fclose(fp);
        return false;
    }
    uint64 size = 0;
    if (fread(&size, sizeof(uint64), 1, fp) != 1) {
        fclose(fp);
        return false;
    }

    out.words.assign(size_t(size), 0);
    if (size) {
        std::unique_ptr<char[]> buffer(new char[LZ4_compressBound(BlockSize)]);
        LZ4_streamDecode_t *stream = LZ4_createStreamDecode();
        LZ4_setStreamDecode(stream, reinterpret_cast<const char *>(out.words.data()), 0);

        for (uint64 offset = 0; offset < size*4; offset += BlockSize) {
            uint64 compSize = 0;
            if (fread(&compSize, sizeof(uint64), 1, fp) != 1 ||
                fread(buffer.get(), 1, size_t(compSize), fp) != compSize) {
                LZ4_freeStreamDecode(stream);
                fclose(fp);
                return false;
            }
            int outSize = int(std::min<uint64>(size*4 - offset, BlockSize));
            LZ4_decompress_fast_continue(stream, buffer.get(),
                reinterpret_cast<char *>(out.words.data()) + offset, outSize);
        }
        LZ4_freeStreamDecode(stream);
    }

    fclose(fp);
    return true;
}

/* ------------------------------------------------------------------ */
/* Independent descriptor traversal.                                  */

struct WalkerStats {
    long nodes;
    long leaves;
    int mixedGroups;     /* groups holding both leaves and interior nodes */
    int wideGroups;      /* groups using two-word child slots            */
    int extendedNodes;   /* descriptors with a 64 bit offset             */
    bool valid;
};

/* Walks the reachable descriptors and reports structural facts. A dense grid
 * is filled when one is supplied. */
static bool walkDescriptors(const std::vector<uint32> &words, int depth,
                            std::vector<uint32> *grid, WalkerStats &stats) {
    stats.nodes = 0;
    stats.leaves = 0;
    stats.mixedGroups = 0;
    stats.wideGroups = 0;
    stats.extendedNodes = 0;
    stats.valid = true;

    if (words.empty())
        return false;

    int side = 1 << depth;
    struct Entry { uint64 index; int x, y, z, level; };
    std::vector<Entry> stack;
    stack.push_back(Entry{0, 0, 0, 0, 0});
    std::vector<char> seen(words.size(), 0);

    while (!stack.empty()) {
        Entry entry = stack.back();
        stack.pop_back();

        if (entry.index >= words.size() || entry.level > depth || seen[entry.index]) {
            stats.valid = false;
            return false;
        }
        seen[entry.index] = 1;
        stats.nodes++;

        uint32 word = words[entry.index];
        uint32 occupied = (word >> 8) & 0xFF;
        uint32 interior = word & 0xFF;

        if (interior & ~occupied) {
            stats.valid = false;
            return false;
        }
        if (interior && (occupied & ~interior))
            stats.mixedGroups++;
        if (word & (1u << 16))
            stats.wideGroups++;
        if (word & (1u << 17))
            stats.extendedNodes++;

        if (occupied == 0)
            continue;

        uint64 offset = word >> 18;
        if (word & (1u << 17)) {
            if (entry.index + 1 >= words.size()) {
                stats.valid = false;
                return false;
            }
            offset = (offset << 32) | uint64(words[entry.index + 1]);
        }

        uint64 groupStart = entry.index + offset;
        int stride = (word & (1u << 16)) ? 2 : 1;
        int rank = 0;
        int size = 1 << (depth - entry.level);

        for (int b = 0; b < 8; b++) {
            if (!(occupied & (1u << b)))
                continue;

            uint64 slot = groupStart + uint64(rank)*uint64(stride);
            rank++;
            if (slot >= words.size()) {
                stats.valid = false;
                return false;
            }

            int cx = entry.x + ((b & 1) ? size/2 : 0);
            int cy = entry.y + ((b & 2) ? size/2 : 0);
            int cz = entry.z + ((b & 4) ? size/2 : 0);

            if (interior & (1u << b)) {
                stack.push_back(Entry{slot, cx, cy, cz, entry.level + 1});
            } else {
                stats.leaves++;
                uint32 material = words[size_t(slot)];
                if (!material)
                    continue;
                if (grid) {
                    int half = size/2;
                    for (int zz = 0; zz < half; zz++)
                        for (int yy = 0; yy < half; yy++)
                            for (int xx = 0; xx < half; xx++) {
                                size_t idx = size_t(cx + xx) +
                                             size_t(side)*(size_t(cy + yy) +
                                             size_t(side)*size_t(cz + zz));
                                if (idx >= grid->size()) {
                                    stats.valid = false;
                                    return false;
                                }
                                (*grid)[idx] = material;
                            }
                }
            }
        }
    }
    return stats.valid;
}

/* ------------------------------------------------------------------ */
/* Dense integer-grid reference model.                                */

struct Model {
    int depth;
    int side;
    std::vector<uint32> cells;

    Model(int depth_) : depth(depth_), side(1 << depth_),
        cells(size_t(1 << depth_)*size_t(1 << depth_)*size_t(1 << depth_), 0) {}

    void paint(unsigned gridDepth, const std::array<uint32, 3> &lo,
               const std::array<uint32, 3> &hi, uint32 material) {
        int shift = depth - int(gridDepth);
        for (int z = int(lo[2]) << shift; z < int(hi[2]) << shift; z++)
            for (int y = int(lo[1]) << shift; y < int(hi[1]) << shift; y++)
                for (int x = int(lo[0]) << shift; x < int(hi[0]) << shift; x++)
                    cells[size_t(x) + size_t(side)*(size_t(y) + size_t(side)*size_t(z))] = material;
    }

    uint32 at(int x, int y, int z) const {
        return cells[size_t(x) + size_t(side)*(size_t(y) + size_t(side)*size_t(z))];
    }
};

static void paintAndMirror(VoxelOctree &tree, Model &model, unsigned gridDepth,
                           std::array<uint32, 3> lo, std::array<uint32, 3> hi,
                           uint32 material) {
    tree.paintBox(gridDepth, lo, hi, material);
    model.paint(gridDepth, lo, hi, material);
}

struct RefHit {
    bool hit;
    uint32 material;
    float t;
};

/* Exact nearest ray/box intersection over every non-empty model cell. Rays in
 * the tests are chosen to avoid coplanar and shared-edge cases. */
static RefHit traceModel(const Model &model, const Vec3 &o, const Vec3 &d) {
    RefHit best;
    best.hit = false;
    best.material = 0;
    best.t = 1e30f;

    float cell = 1.0f/model.side;
    for (int z = 0; z < model.side; z++)
        for (int y = 0; y < model.side; y++)
            for (int x = 0; x < model.side; x++) {
                uint32 material = model.at(x, y, z);
                if (!material)
                    continue;

                float b[3] = { 1.0f + x*cell, 1.0f + y*cell, 1.0f + z*cell };
                float e[3] = { b[0] + cell, b[1] + cell, b[2] + cell };
                float origin[3] = { o.x, o.y, o.z };
                float dir[3] = { d.x, d.y, d.z };

                float tn = -1e30f, tf = 1e30f;
                bool hits = true;
                for (int i = 0; i < 3; i++) {
                    if (std::fabs(dir[i]) < 1e-12f) {
                        if (origin[i] < b[i] || origin[i] >= e[i]) {
                            hits = false;
                            break;
                        }
                    } else {
                        float t1 = (b[i] - origin[i])/dir[i];
                        float t2 = (e[i] - origin[i])/dir[i];
                        if (t1 > t2)
                            std::swap(t1, t2);
                        tn = std::max(tn, t1);
                        tf = std::min(tf, t2);
                    }
                }
                if (!hits || tn > tf || tf < 0)
                    continue;

                float t = std::max(tn, 0.0f);
                if (t < best.t) {
                    best.hit = true;
                    best.material = material;
                    best.t = t;
                }
            }

    return best;
}

static Vec3 normalize(Vec3 v) {
    float len = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    return Vec3(v.x/len, v.y/len, v.z/len);
}

/* Compares raymarch against the model with exact (rayScale = 0) traversal. */
static int checkRays(VoxelOctree &tree, const Model &model,
                     const std::vector<Vec3> &origins, const std::vector<Vec3> &dirs) {
    for (size_t i = 0; i < origins.size(); i++) {
        Vec3 d = normalize(dirs[i]);
        RefHit ref = traceModel(model, origins[i], d);

        uint32 material = 0;
        float t = -1.0f;
        bool hit = tree.raymarch(origins[i], d, 0.0f, material, t);

        TASK_CHECK(hit == ref.hit);
        if (hit && ref.hit) {
            TASK_CHECK(material == ref.material);
            TASK_CHECK(std::fabs(t - ref.t) < 3e-3f);
        }
    }
    return 0;
}

/* Casts from outside the unit scene cube towards a target point, with a small
 * lateral offset so the ray never grazes a cell plane or edge. */
static void addAimedRays(std::vector<Vec3> &origins, std::vector<Vec3> &dirs,
                         float tx, float ty, float tz, const Vec3 &offset) {
    const Vec3 directions[8] = {
        Vec3( 1.0f, 0.0130f, 0.0170f),
        Vec3(-1.0f, 0.0110f, 0.0190f),
        Vec3( 0.0130f, 1.0f, 0.0170f),
        Vec3( 0.0110f,-1.0f, 0.0190f),
        Vec3( 0.0130f, 0.0170f, 1.0f),
        Vec3( 0.0110f, 0.0190f,-1.0f),
        Vec3( 0.7000f, 0.7010f, 0.7130f),
        Vec3(-0.7000f,-0.7010f, 0.7130f)
    };

    for (int i = 0; i < 8; i++) {
        Vec3 d = normalize(directions[i]);
        Vec3 target(tx + offset.x, ty + offset.y, tz + offset.z);
        Vec3 origin = target - d*3.0f;
        origins.push_back(origin);
        dirs.push_back(d);
    }
}

/* ------------------------------------------------------------------ */
/* Shared helpers.                                                    */

static bool ensureThreads() {
    if (!ThreadUtils::pool)
        ThreadUtils::startThreads(2);
    return ThreadUtils::pool != 0;
}

static const char *tempPath(const char *name) {
    static char buffer[8][512];
    static int slot = 0;
    char *out = buffer[slot];
    slot = (slot + 1) % 8;
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir)
        dir = ".";
    std::snprintf(out, 512, "%s/svo_paint_%s", dir, name);
    return out;
}

/* Builds the small brush scene and its independent model. */
static void buildBrushScene(VoxelOctree &tree, Model &model) {
    std::array<uint32, 3> allLo = {{0, 0, 0}};
    std::array<uint32, 3> allHi = {{8, 8, 8}};
    paintAndMirror(tree, model, 3, allLo, allHi, 0xA1B2C3D4u);

    std::array<uint32, 3> midLo = {{2, 2, 2}};
    std::array<uint32, 3> midHi = {{6, 6, 6}};
    paintAndMirror(tree, model, 3, midLo, midHi, 0x11223344u);

    /* A thin slab crossing the x mid-plane erases through several octants. */
    std::array<uint32, 3> slabLo = {{0, 3, 0}};
    std::array<uint32, 3> slabHi = {{8, 4, 8}};
    paintAndMirror(tree, model, 3, slabLo, slabHi, 0);
}

/* ------------------------------------------------------------------ */
/* Group: brush                                                       */

static int case_brush_rays() {
    VoxelOctree tree(tempPath("does-not-exist.oct"));
    Model model(3);
    buildBrushScene(tree, model);
    TASK_CHECK(tree.wordCount() > 0);

    std::vector<Vec3> origins;
    std::vector<Vec3> dirs;

    /* Every non-empty cell gets rays from eight generic directions. */
    for (int z = 0; z < model.side; z++)
        for (int y = 0; y < model.side; y++)
            for (int x = 0; x < model.side; x++) {
                if (!model.at(x, y, z))
                    continue;
                Vec3 centre(1.0f + (x + 0.5f)/8.0f,
                            1.0f + (y + 0.5f)/8.0f,
                            1.0f + (z + 0.5f)/8.0f);
                addAimedRays(origins, dirs, centre.x, centre.y, centre.z,
                             Vec3(0.0031f, 0.0057f, 0.0092f));
            }

    return checkRays(tree, model, origins, dirs);
}

static int case_brush_overlap_and_erase() {
    VoxelOctree tree(tempPath("does-not-exist.oct"));
    Model model(3);
    buildBrushScene(tree, model);

    /* Overlapping coverage order matters: the later box wins. */
    std::array<uint32, 3> lo = {{1, 1, 1}};
    std::array<uint32, 3> hi = {{5, 5, 5}};
    paintAndMirror(tree, model, 3, lo, hi, 0x55667788u);

    std::array<uint32, 3> lo2 = {{3, 3, 3}};
    std::array<uint32, 3> hi2 = {{7, 7, 7}};
    paintAndMirror(tree, model, 3, lo2, hi2, 0x99AABBCCu);

    /* Erase a box that spans the mid-plane in all three axes. */
    std::array<uint32, 3> lo3 = {{2, 2, 3}};
    std::array<uint32, 3> hi3 = {{6, 6, 5}};
    paintAndMirror(tree, model, 3, lo3, hi3, 0);

    OctFile file;
    tree.save(tempPath("overlap.oct"));
    TASK_CHECK(readOctFile(tempPath("overlap.oct"), file));

    std::vector<uint32> grid(model.cells.size(), 0);
    WalkerStats stats;
    TASK_CHECK(walkDescriptors(file.words, model.depth, &grid, stats));
    TASK_CHECK(grid == model.cells);
    TASK_CHECK(file.words.size() == tree.wordCount());

    std::vector<Vec3> origins;
    std::vector<Vec3> dirs;
    addAimedRays(origins, dirs, 1.5f, 1.5f, 1.5f, Vec3(0.0f, 0.0f, 0.0f));
    addAimedRays(origins, dirs, 1.25f, 1.75f, 1.375f, Vec3(0.0061f, 0.0029f, 0.0043f));
    return checkRays(tree, model, origins, dirs);
}

static int case_brush_split_and_merge() {
    VoxelOctree tree(tempPath("does-not-exist.oct"));
    Model model(4);

    std::array<uint32, 3> lo = {{0, 0, 0}};
    std::array<uint32, 3> hi = {{16, 16, 16}};
    paintAndMirror(tree, model, 4, lo, hi, 0xCAFEF00Du);

    /* A full single-material field is the root plus eight coarse leaves. */
    TASK_CHECK(tree.wordCount() == 9);

    std::array<uint32, 3> holeLo = {{5, 6, 7}};
    std::array<uint32, 3> holeHi = {{6, 7, 8}};
    paintAndMirror(tree, model, 4, holeLo, holeHi, 0);

    /* Splitting one coarse leaf must not erase the surrounding material. */
    TASK_CHECK(tree.wordCount() > 9);
    TASK_CHECK(tree.wordCount() <= 64);

    std::vector<uint32> grid(model.cells.size(), 0);
    tree.save(tempPath("split.oct"));
    OctFile file;
    TASK_CHECK(readOctFile(tempPath("split.oct"), file));
    WalkerStats stats;
    TASK_CHECK(walkDescriptors(file.words, 4, &grid, stats));
    TASK_CHECK(grid == model.cells);
    TASK_CHECK(grid[size_t(5) + 16*(size_t(6) + 16*size_t(7))] == 0);
    TASK_CHECK(grid[size_t(4) + 16*(size_t(6) + 16*size_t(7))] == 0xCAFEF00Du);

    std::vector<Vec3> origins;
    std::vector<Vec3> dirs;
    for (int z = 0; z < 16; z += 3)
        for (int y = 0; y < 16; y += 3)
            for (int x = 0; x < 16; x += 3) {
                if (!model.at(x, y, z))
                    continue;
                addAimedRays(origins, dirs, 1.0f + (x + 0.5f)/16.0f,
                             1.0f + (y + 0.5f)/16.0f,
                             1.0f + (z + 0.5f)/16.0f,
                             Vec3(0.0023f, 0.0037f, 0.0019f));
            }
    TASK_CHECK(checkRays(tree, model, origins, dirs) == 0);

    paintAndMirror(tree, model, 4, holeLo, holeHi, 0xCAFEF00Du);
    TASK_CHECK(tree.wordCount() == 9);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Group: mixed_layout                                                */

/* Hand-assembled mixed descriptor file: the root group holds two interior
 * children and two coarse leaves. Interior children are one word descriptors
 * whose own groups live behind the root group. */
static std::vector<uint32> mixedWords() {
    /* Root descriptor at 0 with a group at 1 holding four occupied children:
     * octants 0 and 1 are interior descriptors in slots 1 and 2, octants 2
     * and 3 are coarse leaves in slots 3 and 4. Each interior child owns a
     * group with one fine leaf, stored behind it. */
    std::vector<uint32> words(7, 0);
    words[0] = (0x0Fu << 8) | 0x03u | (1u << 18);
    words[1] = (1u << 8) | (4u << 18);          /* group at 1 + 4 = 5 */
    words[2] = (1u << 8) | (4u << 18);          /* group at 2 + 4 = 6 */
    words[3] = 0x11111111u;
    words[4] = 0x22222222u;
    words[5] = 0x33333333u;
    words[6] = 0x44444444u;
    return words;
}

static int case_mixed_loaded_file() {
    std::vector<uint32> words = mixedWords();
    const float center[3] = {0.5f, 0.5f, 0.5f};
    TASK_CHECK(writeOctFile(tempPath("mixed.oct"), center, words));

    VoxelOctree tree(tempPath("mixed.oct"));
    TASK_CHECK(tree.wordCount() == words.size());

    std::vector<uint32> grid;
    WalkerStats stats;
    TASK_CHECK(walkDescriptors(words, 2, 0, stats));
    TASK_CHECK(stats.nodes == 3);
    TASK_CHECK(stats.leaves >= 4);
    TASK_CHECK(stats.mixedGroups >= 1);

    /* Interior child 0 occupies [1,1.5)^3; its single fine leaf is
     * [1,1.25)^3. Cast from outside the cube along +x: it is the first
     * non-empty region on that line. */
    uint32 material = 0;
    float t = -1.0f;
    Vec3 d = normalize(Vec3(1.0f, 0.021f, 0.013f));
    TASK_CHECK(tree.raymarch(Vec3(0.5f, 1.0625f, 1.0625f), Vec3(1.0f, 0.0f, 0.0f),
                             0.0f, material, t));
    TASK_CHECK(material == 0x33333333u);

    /* Interior child 1 covers [1.5,2) x [1,1.5)^2; its fine leaf is
     * [1.5,1.75) x [1,1.25)^2. Approach it from inside the empty x half of
     * that interior node. */
    TASK_CHECK(tree.raymarch(Vec3(1.9f, 1.0625f, 1.0625f), Vec3(-1.0f, 0.0f, 0.0f),
                             0.0f, material, t));
    TASK_CHECK(material == 0x44444444u);
    TASK_CHECK(std::fabs(t - 0.15f) < 2e-3f);

    /* Coarse leaf 2 covers [1,1.5) x [1.5,2) x [1,1.5): it is the first solid
     * region met when entering through the y = 2 face. */
    TASK_CHECK(tree.raymarch(Vec3(1.0625f, 2.5f, 1.0625f), Vec3(0.0f, -1.0f, 0.0f),
                             0.0f, material, t));
    TASK_CHECK(material == 0x11111111u);

    /* Coarse leaf 3 covers [1.5,2)^2 x [1,1.5). */
    TASK_CHECK(tree.raymarch(Vec3(1.5625f, 2.5f, 1.0625f), Vec3(0.0f, -1.0f, 0.0f),
                             0.0f, material, t));
    TASK_CHECK(material == 0x22222222u);

    /* Nothing exists above z = 1.5. */
    TASK_CHECK(!tree.raymarch(Vec3(0.5f, 1.25f, 1.75f), Vec3(1.0f, 0.0f, 0.0f),
                              0.0f, material, t));

    /* Editing a hand-built file replaces child 0 but leaves its sibling
     * interior node and both coarse leaves untouched. */
    std::array<uint32, 3> lo = {{0, 0, 0}};
    std::array<uint32, 3> hi = {{1, 1, 1}};
    tree.paintBox(2, lo, hi, 0xABABABABu);
    TASK_CHECK(tree.raymarch(Vec3(0.5f, 1.0625f, 1.0625f), Vec3(1.0f, 0.0f, 0.0f),
                             0.0f, material, t));
    TASK_CHECK(material == 0xABABABABu);
    TASK_CHECK(tree.raymarch(Vec3(1.9f, 1.0625f, 1.0625f), Vec3(-1.0f, 0.0f, 0.0f),
                             0.0f, material, t));
    TASK_CHECK(material == 0x44444444u);
    TASK_CHECK(tree.raymarch(Vec3(1.0625f, 2.5f, 1.0625f), Vec3(0.0f, -1.0f, 0.0f),
                             0.0f, material, t));
    TASK_CHECK(material == 0x11111111u);
    TASK_CHECK(tree.raymarch(Vec3(1.5625f, 2.5f, 1.0625f), Vec3(0.0f, -1.0f, 0.0f),
                             0.0f, material, t));
    TASK_CHECK(material == 0x22222222u);

    /* The untouched empty half is still empty after the edit. */
    TASK_CHECK(!tree.raymarch(Vec3(0.5f, 1.25f, 1.75f), Vec3(1.0f, 0.0f, 0.0f),
                              0.0f, material, t));

    return 0;
}

/* Hand-assembled file using two-word child slots and a 64 bit offset. */
static std::vector<uint32> wideExtendedWords() {
    std::vector<uint32> words(16390, 0);
    /* Root: one interior child at index 2, so the child descriptor can carry
     * an extra high-offset word at index 3. */
    words[0] = 0x01u | (1u << 8) | (2u << 18);
    uint64 offset = 0x4000;
    words[2] = (1u << 8) | (1u << 17) |
               (uint32((offset >> 32) & 0x3FFF) << 18);
    words[3] = uint32(offset);                  /* low 32 offset bits */
    /* Child group at 2 + 0x4000 = 16386; its single slot holds the leaf. */
    words[16386] = 0xDEADBEEFu;
    return words;
}

static int case_mixed_wide_and_extended() {
    std::vector<uint32> words = wideExtendedWords();
    const float center[3] = {0.25f, 0.125f, 0.375f};
    TASK_CHECK(writeOctFile(tempPath("wide.oct"), center, words));

    VoxelOctree tree(tempPath("wide.oct"));
    TASK_CHECK(tree.wordCount() == words.size());

    WalkerStats stats;
    TASK_CHECK(walkDescriptors(words, 2, 0, stats));
    TASK_CHECK(stats.extendedNodes >= 1);

    uint32 material = 0;
    float t = -1.0f;
    Vec3 target(1.0625f, 1.0625f, 1.0625f);
    Vec3 d = normalize(Vec3(1.0f, 0.031f, 0.017f));
    Vec3 o = target - d*3.0f;
    TASK_CHECK(tree.raymarch(o, d, 0.0f, material, t));
    TASK_CHECK(material == 0xDEADBEEFu);

    /* The saving path must keep the same structure and the old constructor
     * must read it back. */
    tree.save(tempPath("wide_saved.oct"));
    VoxelOctree reloaded(tempPath("wide_saved.oct"));
    TASK_CHECK(reloaded.wordCount() == words.size());
    TASK_CHECK(reloaded.center() == tree.center());
    TASK_CHECK(reloaded.raymarch(o, d, 0.0f, material, t));
    TASK_CHECK(material == 0xDEADBEEFu);
    return 0;
}

static int case_mixed_structure_large() {
    VoxelOctree tree(tempPath("does-not-exist.oct"));

    /* Depth 20 full fill is the compact 9 word form. */
    std::array<uint32, 3> lo = {{0, 0, 0}};
    std::array<uint32, 3> hi = {{1u << 20, 1u << 20, 1u << 20}};
    tree.paintBox(20, lo, hi, 0x13579BDFu);
    TASK_CHECK(tree.wordCount() == 9);

    /* Erasing one finest cell splits one leaf along the path only. */
    std::array<uint32, 3> holeLo = {{123456, 777777, 999999}};
    std::array<uint32, 3> holeHi = {{123457, 777778, 1000000}};
    tree.paintBox(20, holeLo, holeHi, 0);
    TASK_CHECK(tree.wordCount() <= 512);
    TASK_CHECK(tree.wordCount() > 9);

    /* Filling it back merges everything to the original 9 words. */
    tree.paintBox(20, holeLo, holeHi, 0x13579BDFu);
    TASK_CHECK(tree.wordCount() == 9);

    /* Carving many holes grows a genuine mixed layout: groups that hold both
     * coarse leaves and interior records, with addresses far beyond the short
     * range. This must stay far away from a dense expansion. */
    unsigned seed = 0x1234ABCDu;
    for (int i = 0; i < 420; i++) {
        seed = seed*1103515245u + 12345u;
        uint32 x = (seed >> 6) % (1u << 20);
        seed = seed*1103515245u + 12345u;
        uint32 y = (seed >> 6) % (1u << 20);
        seed = seed*1103515245u + 12345u;
        uint32 z = (seed >> 6) % (1u << 20);
        std::array<uint32, 3> hlo = {{x, y, z}};
        std::array<uint32, 3> hhi = {{x + 1, y + 1, z + 1}};
        tree.paintBox(20, hlo, hhi, 0);
    }

    TASK_CHECK(tree.wordCount() > 0x3FFF);
    TASK_CHECK(tree.wordCount() < (uint64(1) << 30));

    tree.save(tempPath("large.oct"));
    OctFile file;
    TASK_CHECK(readOctFile(tempPath("large.oct"), file));
    TASK_CHECK(file.words.size() == tree.wordCount());

    WalkerStats stats;
    TASK_CHECK(walkDescriptors(file.words, 20, 0, stats));
    TASK_CHECK(stats.nodes > 0);
    TASK_CHECK(stats.mixedGroups >= 1);
    TASK_CHECK(stats.extendedNodes >= 1);
    TASK_CHECK(stats.wideGroups >= 1);
    TASK_CHECK(uint64(stats.nodes) < uint64(1) << 20);

    /* The independent walker agrees with the edited tree about a solid spot
     * far from any carved hole. */
    uint32 material = 0;
    float t = -1.0f;
    Vec3 target(1.0f + 0.5f, 1.0f + 0.5f, 1.0f + 0.5f);
    Vec3 d = normalize(Vec3(1.0f, 0.0137f, 0.0211f));
    Vec3 o = target - d*1.5f;
    TASK_CHECK(tree.raymarch(o, d, 0.0f, material, t));
    TASK_CHECK(material == 0x13579BDFu);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Group: persistence                                                 */

static int case_persistence_built_edit_save() {
    TASK_CHECK(ensureThreads());

    const char *raw = tempPath("persist.raw");
    FILE *fp = fopen(raw, "wb");
    TASK_CHECK(fp != 0);

    int dim[3] = {8, 8, 8};
    fwrite(dim, sizeof(int), 3, fp);
    for (int z = 0; z < 8; z++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                uint32 value = (x == 2 && y == 3 && z == 4) ? 0x12345678u : 0;
                fwrite(&value, sizeof(uint32), 1, fp);
            }
    fclose(fp);

    VoxelData data(raw, 1024*1024);
    VoxelOctree tree(&data);
    Vec3 center = tree.center();

    uint32 material = 0;
    float t = -1.0f;
    TASK_CHECK(tree.raymarch(Vec3(0.0f, 1.4375f, 1.5625f),
                             Vec3(1.0f, 0.01f, 0.01f), 0.0f, material, t));
    TASK_CHECK(material == 0x12345678u);

    std::array<uint32, 3> lo = {{0, 0, 0}};
    std::array<uint32, 3> hi = {{8, 8, 8}};
    tree.paintBox(3, lo, hi, 0xCAFEBABEu);
    TASK_CHECK(tree.wordCount() == 9);
    TASK_CHECK(tree.center() == center);

    std::array<uint32, 3> holeLo = {{1, 1, 1}};
    std::array<uint32, 3> holeHi = {{2, 2, 2}};
    tree.paintBox(3, holeLo, holeHi, 0);
    TASK_CHECK(tree.wordCount() > 9);

    const char *saved = tempPath("persist.oct");
    tree.save(saved);

    VoxelOctree loaded(saved);
    TASK_CHECK(loaded.wordCount() == tree.wordCount());
    TASK_CHECK(loaded.center() == center);

    /* The old constructor reads the file back and the edit is visible. */
    Vec3 holeCentre(1.0f + 1.5f/8.0f, 1.0f + 1.5f/8.0f, 1.0f + 1.5f/8.0f);
    Vec3 d = normalize(Vec3(1.0f, 0.017f, 0.013f));
    Vec3 origin = holeCentre - d*2.0f;
    TASK_CHECK(loaded.raymarch(origin, d, 0.0f, material, t));
    TASK_CHECK(material == 0xCAFEBABEu);

    /* The erased cell is enclosed, so start inside it: the first solid cell
     * starts at its far x face and is the surrounding material, not another
     * copy of the erased value. */
    float holeMin = 1.0f + 1.0f/8.0f;
    float holeMax = 1.0f + 2.0f/8.0f;
    Vec3 insideHole(holeMin + 0.5f*(holeMax - holeMin), holeCentre.y, holeCentre.z);
    TASK_CHECK(loaded.raymarch(insideHole, Vec3(1.0f, 0.0f, 0.0f), 0.0f, material, t));
    TASK_CHECK(material == 0xCAFEBABEu);
    TASK_CHECK(std::fabs(t - 0.5f*(holeMax - holeMin)) < 3e-3f);

    /* Just outside the hole the ray starts inside solid material. */
    Vec3 solidPoint(holeMin + 0.5f*(holeMax - holeMin),
                    1.0f + 3.5f/8.0f, 1.0f + 3.5f/8.0f);
    TASK_CHECK(loaded.raymarch(solidPoint, Vec3(1.0f, 0.0f, 0.0f), 0.0f, material, t));
    TASK_CHECK(material == 0xCAFEBABEu);
    TASK_CHECK(t < 1e-3f);

    return 0;
}

static int case_persistence_without_source() {
    /* No raw file and no VoxelData: start from an empty object, paint, save,
     * reload, keep painting. The default center must survive untouched. */
    VoxelOctree tree(tempPath("missing.oct"));
    TASK_CHECK(tree.wordCount() == 0);
    Vec3 center = tree.center();
    TASK_CHECK(center == Vec3());

    std::array<uint32, 3> lo = {{1, 2, 3}};
    std::array<uint32, 3> hi = {{2, 3, 4}};
    tree.paintBox(3, lo, hi, 0x01020304u);
    TASK_CHECK(tree.wordCount() > 0);
    TASK_CHECK(tree.center() == center);

    const char *saved = tempPath("nosource.oct");
    tree.save(saved);

    OctFile file;
    TASK_CHECK(readOctFile(saved, file));
    TASK_CHECK(file.words.size() == tree.wordCount());
    TASK_CHECK(file.center[0] == 0.0f && file.center[1] == 0.0f && file.center[2] == 0.0f);

    VoxelOctree loaded(saved);
    TASK_CHECK(loaded.wordCount() == tree.wordCount());
    TASK_CHECK(loaded.center() == center);

    /* Keep painting the reloaded object with a different brush grid. The
     * second box is [6,7)^3 on the depth-4 grid, far from the depth-3 box,
     * which covers x [1,2), y [4,6), z [6,8) in sixteenths. */
    std::array<uint32, 3> lo2 = {{6, 6, 6}};
    std::array<uint32, 3> hi2 = {{7, 7, 7}};
    loaded.paintBox(4, lo2, hi2, 0x05060708u);
    TASK_CHECK(loaded.wordCount() > file.words.size());

    uint32 material = 0;
    float t = -1.0f;
    Vec3 d = normalize(Vec3(1.0f, 0.019f, 0.011f));

    /* The depth-3 box is [1,2)x[2,3)x[3,4) there, i.e. x [1.125,1.25),
     * y [1.25,1.375), z [1.375,1.5). */
    Vec3 target(1.0f + 1.5f/8.0f, 1.0f + 2.5f/8.0f, 1.0f + 3.5f/8.0f);
    TASK_CHECK(loaded.raymarch(target - d*2.0f, d, 0.0f, material, t));
    TASK_CHECK(material == 0x01020304u);

    /* The depth-4 box is [6,7)^3 there, so its centre sits at
     * 1 + (6.5, 6.5, 6.5)/16. */
    Vec3 targetB(1.0f + 6.5f/16.0f, 1.0f + 6.5f/16.0f, 1.0f + 6.5f/16.0f);
    TASK_CHECK(loaded.raymarch(targetB - d*2.0f, d, 0.0f, material, t));
    TASK_CHECK(material == 0x05060708u);

    loaded.save(tempPath("nosource2.oct"));
    VoxelOctree again(tempPath("nosource2.oct"));
    TASK_CHECK(again.wordCount() == loaded.wordCount());
    TASK_CHECK(again.raymarch(targetB - d*2.0f, d, 0.0f, material, t));
    TASK_CHECK(material == 0x05060708u);
    TASK_CHECK(again.raymarch(target - d*2.0f, d, 0.0f, material, t));
    TASK_CHECK(material == 0x01020304u);

    return 0;
}

static int case_persistence_center_and_empty() {
    TASK_CHECK(ensureThreads());

    /* A tree built from raw data keeps its own center through edits. */
    const char *raw = tempPath("center.raw");
    FILE *fp = fopen(raw, "wb");
    TASK_CHECK(fp != 0);
    int dim[3] = {4, 8, 16};
    fwrite(dim, sizeof(int), 3, fp);
    for (int i = 0; i < 4*8*16; i++) {
        uint32 value = (i % 5 == 0) ? 0x00ABCDEFu : 0;
        fwrite(&value, sizeof(uint32), 1, fp);
    }
    fclose(fp);

    VoxelData data(raw, 1024*1024);
    VoxelOctree tree(&data);
    Vec3 center = tree.center();

    std::array<uint32, 3> lo = {{0, 0, 0}};
    std::array<uint32, 3> hi = {{1u << 20, 1u << 20, 1u << 20}};
    tree.paintBox(20, lo, hi, 0x0BADF00Du);
    TASK_CHECK(tree.wordCount() == 9);
    TASK_CHECK(tree.center() == center);

    std::array<uint32, 3> emptyLo = {{0, 0, 0}};
    std::array<uint32, 3> emptyHi = {{1u << 20, 1u << 20, 1u << 20}};
    tree.paintBox(20, emptyLo, emptyHi, 0);
    TASK_CHECK(tree.wordCount() == 1);
    TASK_CHECK(tree.center() == center);

    uint32 material = 0;
    float t = -1.0f;
    TASK_CHECK(!tree.raymarch(Vec3(0.5f, 1.5f, 1.5f), Vec3(1, 0, 0),
                              0.0f, material, t));

    tree.save(tempPath("empty.oct"));
    OctFile file;
    TASK_CHECK(readOctFile(tempPath("empty.oct"), file));
    TASK_CHECK(file.words.size() == 1);

    VoxelOctree loaded(tempPath("empty.oct"));
    TASK_CHECK(loaded.wordCount() == 1);
    TASK_CHECK(loaded.center() == center);

    /* An empty box is a no-op, including for an empty tree. */
    std::array<uint32, 3> sameLo = {{3, 3, 3}};
    std::array<uint32, 3> sameHi = {{3, 3, 3}};
    tree.paintBox(3, sameLo, sameHi, 0x12345678u);
    TASK_CHECK(tree.wordCount() == 1);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */

extern const struct task_case task_cases[] = {
    { "brush", "rays", case_brush_rays },
    { "brush", "overlap_erase", case_brush_overlap_and_erase },
    { "brush", "split_merge", case_brush_split_and_merge },
    { "mixed_layout", "loaded_mixed_file", case_mixed_loaded_file },
    { "mixed_layout", "wide_extended", case_mixed_wide_and_extended },
    { "mixed_layout", "large_structure", case_mixed_structure_large },
    { "persistence", "built_edit_save", case_persistence_built_edit_save },
    { "persistence", "without_source", case_persistence_without_source },
    { "persistence", "center_and_empty", case_persistence_center_and_empty }
};

extern const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
