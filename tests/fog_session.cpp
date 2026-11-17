// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

// Tests for nanovdb::tools::FogSession.
//
// The optics tests compare the resumable hierarchy traversal against an
// independent brute-force reference: the reference splits the ray at every
// integer index-space plane of every volume, samples the grid at each segment
// midpoint through a ReadAccessor (so it knows nothing about FogSession's
// cursors) and integrates the same closed-form optics.  The hierarchy tests
// check that constant tiles are used instead of individual voxels, and the
// resume tests check budgets, equal-time boundaries, interleaving and the
// completed/idempotent states.

#include "task_support/check.h"

#include <nanovdb/HostBuffer.h>
#include <nanovdb/NanoVDB.h>
#include <nanovdb/math/Ray.h>
#include <nanovdb/tools/CreateNanoGrid.h>
#include <nanovdb/tools/CreatePrimitives.h>
#include <nanovdb/tools/FogSession.h>
#include <nanovdb/tools/GridBuilder.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

// Defined in ex_raytrace_fog_volume/nanovdb.cu; its CPU branch integrates
// through FogSession.
void runNanoVDB(nanovdb::GridHandle<nanovdb::HostBuffer>&, int, int, int, nanovdb::HostBuffer&);

namespace {

using BuildGrid = nanovdb::tools::build::FloatGrid;
using Handle = nanovdb::GridHandle<nanovdb::HostBuffer>;
using Grid = nanovdb::FloatGrid;
using Ray = nanovdb::math::Ray<double>;
using Vec3 = nanovdb::Vec3d;
using Volumes = std::vector<nanovdb::tools::FogVolume>;

constexpr double kTol = 1e-6;

bool close(double a, double b)
{
    const double scale = std::max(1.0, std::max(std::abs(a), std::abs(b)));
    return std::abs(a - b) <= kTol * scale;
}

bool close(const Vec3& a, const Vec3& b)
{
    return close(a[0], b[0]) && close(a[1], b[1]) && close(a[2], b[2]);
}

// ---------------------------------------------------------------------------
// Grid construction helpers
// ---------------------------------------------------------------------------

// Convert a builder grid, applying an index-to-world uniform scale + translation.
Handle convert(BuildGrid& grid, double scale = 1.0, const Vec3& origin = Vec3(0.0))
{
    grid.setTransform(scale, origin);
    return nanovdb::tools::createNanoGrid(grid,
                                          nanovdb::tools::StatsMode::BBox,
                                          nanovdb::CheckMode::Disable);
}

// Activate every voxel of the inclusive index box and give it the same value.
void setBox(BuildGrid& grid, const nanovdb::Coord& lo, const nanovdb::Coord& hi, float value)
{
    for (int i = lo[0]; i <= hi[0]; ++i)
        for (int j = lo[1]; j <= hi[1]; ++j)
            for (int k = lo[2]; k <= hi[2]; ++k)
                grid.setValue(nanovdb::Coord(i, j, k), value);
}

// Add a leaf whose values are all non-zero but whose active mask is empty.
void addInactiveLeaf(BuildGrid& grid, const nanovdb::Coord& origin, float value)
{
    using Leaf = BuildGrid::Node0;
    Leaf* leaf = new Leaf(origin, 0.0f, false);
    for (uint32_t n = 0; n < Leaf::SIZE; ++n)
        leaf->mValues[n] = value;
    grid.tree().root().addNode(leaf);
}

// ---------------------------------------------------------------------------
// Independent brute-force reference
//
// The reference knows nothing about FogSession.  It enumerates each volume's
// constant regions (active voxels and active constant tiles) as index-space
// boxes [origin, origin+dim), intersects every box with the world ray using a
// plain slab test, sorts all box endpoints, and integrates the closed-form
// optics analytically on each resulting world-space segment.
// ---------------------------------------------------------------------------

struct RefVolume
{
    const Grid* grid;
    double      extinction;
    Vec3        emission;
};

struct RefResult
{
    double transmittance = 1.0;
    Vec3   radiance = Vec3(0.0);
};

struct RefBox
{
    nanovdb::Coord origin;
    int            dim;
    float          value;
};

using RefUpper = nanovdb::NanoUpper<float>;
using RefLower = nanovdb::NanoLower<float>;
using RefLeaf = nanovdb::NanoLeaf<float>;

void collectLeaf(const RefLeaf& leaf, std::vector<RefBox>& boxes)
{
    for (uint32_t n = 0; n < RefLeaf::SIZE; ++n)
        if (leaf.valueMask().isOn(n))
            boxes.push_back({leaf.offsetToGlobalCoord(n), 1, leaf.getValue(n)});
}

void collectLower(const RefLower& lower, std::vector<RefBox>& boxes)
{
    for (uint32_t n = 0; n < RefLower::SIZE; ++n) {
        if (lower.childMask().isOn(n))
            collectLeaf(*lower.getChild(n), boxes);
        else if (lower.valueMask().isOn(n))
            boxes.push_back({lower.offsetToGlobalCoord(n), RefLeaf::DIM, lower.data()->getValue(n)});
    }
}

void collectUpper(const RefUpper& upper, std::vector<RefBox>& boxes)
{
    for (uint32_t n = 0; n < RefUpper::SIZE; ++n) {
        if (upper.childMask().isOn(n))
            collectLower(*upper.getChild(n), boxes);
        else if (upper.valueMask().isOn(n))
            boxes.push_back({upper.offsetToGlobalCoord(n), RefLower::DIM, upper.data()->getValue(n)});
    }
}

// Every constant region with an active state; inactive non-zero values are
// deliberately not represented, i.e. the reference treats them as vacuum.
void collectBoxes(const Grid& grid, std::vector<RefBox>& boxes)
{
    const auto& root = grid.tree().root();
    for (auto it = root.cbeginChild(); it; ++it)
        collectUpper(*it, boxes);
    for (auto it = root.cbeginValueOn(); it; ++it)
        boxes.push_back({it.getCoord(), RefUpper::DIM, *it});
}

struct RefHit
{
    std::size_t volume;
    double      tin, tout;
    double      density;
};

RefResult integrateReference(const std::vector<RefVolume>& volumes,
                             const Ray&                     ray,
                             const Vec3&                    background)
{
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<RefHit> hits;
    for (std::size_t vi = 0; vi < volumes.size(); ++vi) {
        const RefVolume& v = volumes[vi];
        const Vec3 eye = v.grid->worldToIndex(ray.eye());
        const Vec3 dir = v.grid->worldToIndexDir(ray.dir());
        std::vector<RefBox> boxes;
        collectBoxes(*v.grid, boxes);

        for (const RefBox& box : boxes) {
            const double density = box.value > 0.0f ? double(box.value) : 0.0;
            if (density == 0.0)
                continue; // no optical contribution

            double tin = -inf, tout = inf;
            bool   hit = true;
            for (int axis = 0; axis < 3 && hit; ++axis) {
                const double lo = double(box.origin[axis]);
                const double hi = lo + double(box.dim);
                if (dir[axis] == 0.0) {
                    hit = eye[axis] >= lo && eye[axis] < hi;
                } else {
                    double a = (lo - eye[axis]) / dir[axis];
                    double b = (hi - eye[axis]) / dir[axis];
                    if (a > b)
                        std::swap(a, b);
                    tin = std::max(tin, a);
                    tout = std::min(tout, b);
                    hit = tout > tin; // zero-length (grazing) boxes never hit
                }
            }
            if (!hit)
                continue;
            tin = std::max(tin, ray.t0());
            tout = std::min(tout, ray.t1());
            if (!(tout > tin))
                continue;
            hits.push_back({vi, tin, tout, density});
        }
    }

    std::vector<double> times{ray.t0(), ray.t1()};
    times.reserve(times.size() + 2 * hits.size());
    for (const RefHit& h : hits) {
        times.push_back(h.tin);
        times.push_back(h.tout);
    }
    std::sort(times.begin(), times.end());

    const double speed = ray.dir().length();
    RefResult result;
    double    t = times.front();
    for (std::size_t n = 1; n < times.size(); ++n) {
        const double next = times[n];
        if (!(next > t))
            continue;
        const double mid = 0.5 * (t + next);

        double sigma = 0.0;
        Vec3   j(0.0);
        for (const RefHit& h : hits) {
            if (mid < h.tin || mid >= h.tout)
                continue;
            const RefVolume& v = volumes[h.volume];
            sigma += h.density * v.extinction;
            j += v.emission * h.density;
        }

        const double length = speed * (next - t);
        if (sigma > 0.0) {
            const double opticalDepth = sigma * length;
            result.radiance += j * (-std::expm1(-opticalDepth) / sigma) * result.transmittance;
            result.transmittance *= std::exp(-opticalDepth);
        } else {
            result.radiance += j * (length * result.transmittance);
        }
        t = next;
    }
    result.radiance += background * result.transmittance;
    return result;
}

// ---------------------------------------------------------------------------
// Session helpers
// ---------------------------------------------------------------------------

RefResult integrateSession(Volumes volumes, const Ray& ray, const Vec3& background,
                           std::size_t budget = (std::size_t(1) << 20),
                           std::size_t* steps = nullptr)
{
    nanovdb::tools::FogSession session(volumes, ray, background);
    std::size_t guard = 0;
    while (!session.advance(budget)) {
        if (++guard > 1000000)
            break; // budget 0 cannot finish an incomplete session
    }
    if (steps)
        *steps = session.spanSteps();
    const nanovdb::tools::FogResult r = session.result();
    return RefResult{r.transmittance, r.radiance};
}

bool matches(const RefResult& a, const RefResult& b)
{
    return close(a.transmittance, b.transmittance) && close(a.radiance, b.radiance);
}

// Two unit-scale, origin-translated grids for the overlap tests: a solid
// box of density 1 and a smaller, offset box of density 0.5.
struct TwoBoxScene
{
    Handle a;
    Handle b;
    Volumes volumes;
};

TwoBoxScene makeTwoBoxScene()
{
    BuildGrid ga(0.0f);
    setBox(ga, nanovdb::Coord(-12, -6, -6), nanovdb::Coord(10, 6, 6), 1.0f);
    BuildGrid gb(0.0f);
    setBox(gb, nanovdb::Coord(-2, -8, -4), nanovdb::Coord(6, 8, 4), 0.5f);

    TwoBoxScene scene;
    scene.a = convert(ga, 1.0, Vec3(0.0));
    scene.b = convert(gb, 1.0, Vec3(3.0, -1.0, 0.5));
    if (scene.a.grid<float>() == nullptr || scene.b.grid<float>() == nullptr)
        throw std::runtime_error("failed to build the two-box scene");
    scene.volumes.push_back({scene.a.grid<float>(), 0.8, Vec3(0.3, 0.1, 0.0)});
    scene.volumes.push_back({scene.b.grid<float>(), 0.4, Vec3(0.0, 0.5, 0.2)});
    return scene;
}

// ===========================================================================
// OPTICS
// ===========================================================================

int optics_uniform_box_scales()
{
    // One active voxel box observed at different voxel scales, translations,
    // directions and ray orientations.  All comparisons go through the
    // independent per-plane reference.
    const struct
    {
        double scale;
        Vec3   origin;
    } cases[] = {
        {1.0, Vec3(0.0)},
        {0.5, Vec3(1.5, -2.0, 0.25)},
        {2.0, Vec3(-4.0, 3.0, 1.0)},
    };

    for (const auto& c : cases) {
        BuildGrid grid(0.0f);
        setBox(grid, nanovdb::Coord(-5, -4, -3), nanovdb::Coord(6, 5, 4), 0.7f);
        Handle handle = convert(grid, c.scale, c.origin);
        const Grid* g = handle.grid<float>();
        TASK_CHECK(g != nullptr);

        const Volumes volumes{{g, 0.9, Vec3(0.2, 0.4, 0.6)}};

        // Non-unit, deliberately non-axis-aligned direction.
        const Vec3 dir(1.3, 0.55, -0.35);
        const Vec3 eye = c.origin - dir * 20.0;
        const Ray  ray(eye, dir, 0.0, 60.0);

        const Vec3     background(0.15, 0.25, 0.05);
        const RefResult reference = integrateReference({{g, 0.9, Vec3(0.2, 0.4, 0.6)}}, ray, background);
        const RefResult actual = integrateSession(volumes, ray, background);
        TASK_CHECK(matches(actual, reference));
        TASK_CHECK(actual.transmittance > 0.0 && actual.transmittance < 1.0);

        // Reversed ray: same segment walked from the far end.
        const Ray reversed(eye + dir * 60.0, -dir, 0.0, 60.0);
        const RefResult refReverse = integrateReference({{g, 0.9, Vec3(0.2, 0.4, 0.6)}}, reversed, background);
        const RefResult actReverse = integrateSession(volumes, reversed, background);
        TASK_CHECK(matches(actReverse, refReverse));
    }
    return 0;
}

int optics_overlapping_colors()
{
    TwoBoxScene scene = makeTwoBoxScene();
    const Ray ray(Vec3(-20.0, 0.3, -0.2), Vec3(1.0, 0.02, 0.01), 0.0, 45.0);
    const Vec3 background(0.05, 0.1, 0.2);

    std::vector<RefVolume> referenceVolumes;
    for (const auto& v : scene.volumes)
        referenceVolumes.push_back({v.grid, v.extinction, v.emission});

    const RefResult reference = integrateReference(referenceVolumes, ray, background);
    const RefResult actual = integrateSession(scene.volumes, ray, background);
    TASK_CHECK(matches(actual, reference));
    // Distinct emissions must produce a non-monochrome result.
    TASK_CHECK(actual.radiance[0] != actual.radiance[1] || actual.radiance[1] != actual.radiance[2]);

    // Varying the direction length must not change the world-space optics
    // when the interval is scaled accordingly.
    const Ray scaled(Vec3(-20.0, 0.3, -0.2), Vec3(7.0, 0.14, 0.07), 0.0, 45.0 / 7.0);
    const RefResult refScaled = integrateReference(referenceVolumes, scaled, background);
    const RefResult actScaled = integrateSession(scene.volumes, scaled, background);
    TASK_CHECK(matches(actScaled, refScaled));
    TASK_CHECK(close(actScaled.transmittance, actual.transmittance));
    return 0;
}

int optics_inactive_nonzero()
{
    // Non-zero but inactive values must be treated as vacuum.  Cover all
    // three hierarchy representations: leaf voxels, a lower-level constant
    // tile and a root-level constant tile.
    BuildGrid grid(0.0f);

    // Active thin slab with density 0.25: the only contributor.  It spans
    // x in [-6, 6] index cells, i.e. 13 voxels [(-6,y,z) .. (6,y,z)] inclusive.
    setBox(grid, nanovdb::Coord(-6, -6, -6), nanovdb::Coord(6, 6, 6), 0.25f);

    // Inactive non-zero voxels inside a leaf, and inactive non-zero tiles at
    // the lower and upper levels.
    addInactiveLeaf(grid, nanovdb::Coord(16, -8, -8), 100.0f);
    grid.tree().root().addTile<1>(nanovdb::Coord(64, 0, 0), 50.0f, false);
    grid.tree().root().addTile<2>(nanovdb::Coord(4096, 0, 0), 25.0f, false);

    Handle handle = convert(grid);
    const Grid* g = handle.grid<float>();
    TASK_CHECK(g != nullptr);
    TASK_CHECK(g->tree().activeTileCount(1) == 0); // tiles are inactive
    TASK_CHECK(g->tree().activeTileCount(2) == 0);
    TASK_CHECK(g->tree().activeVoxelCount() > 0);

    // The ray passes through the active slab, then the inactive non-zero leaf
    // and both inactive tiles.  Only the slab may contribute.
    const Ray ray(Vec3(-40.0, 0.5, 0.5), Vec3(1.0, 0.0, 0.0), 0.0, 12000.0);
    const Volumes volumes{{g, 1.0, Vec3(0.0)}};

    RefResult reference = integrateReference({{g, 1.0, Vec3(0.0)}}, ray, Vec3(0.0));
    RefResult actual = integrateSession(volumes, ray, Vec3(0.0));
    TASK_CHECK(matches(actual, reference));

    // The whole slab is 13 voxels thick: C = 0 and T = exp(-0.25 * 13).
    TASK_CHECK(close(actual.transmittance, std::exp(-0.25 * 13.0)));
    TASK_CHECK(close(actual.radiance[0], 0.0) && close(actual.radiance[1], 0.0) && close(actual.radiance[2], 0.0));

    // Had the inactive values been honoured, transmittance would be far
    // smaller; check that they really were ignored.
    TASK_CHECK(actual.transmittance > std::exp(-10.0));

    // A novel direction through the inactive leaf and tiles only: nothing.
    const Ray vacuum(Vec3(10.0, 0.5, 0.5), Vec3(1.0, 0.0, 0.0), 0.0, 12000.0);
    const RefResult vacuumResult = integrateSession(volumes, vacuum, Vec3(0.7, 0.7, 0.7));
    TASK_CHECK(close(vacuumResult.transmittance, 1.0));
    TASK_CHECK(close(vacuumResult.radiance, Vec3(0.7, 0.7, 0.7)));
    return 0;
}

int optics_zero_extinction_emission()
{
    TwoBoxScene scene = makeTwoBoxScene();

    // Zero extinction, non-zero emission: pure emission along the full path.
    Volumes volumes{{scene.a.grid<float>(), 0.0, Vec3(0.1, 0.2, 0.3)},
                    {scene.b.grid<float>(), 0.0, Vec3(0.4, 0.0, 0.1)}};
    const Ray ray(Vec3(-20.0, 0.0, 0.0), Vec3(1.0, 0.0, 0.0), 0.0, 50.0);
    const Vec3 background(0.2, 0.3, 0.4);

    std::vector<RefVolume> refVolumes{{scene.a.grid<float>(), 0.0, Vec3(0.1, 0.2, 0.3)},
                                      {scene.b.grid<float>(), 0.0, Vec3(0.4, 0.0, 0.1)}};
    const RefResult reference = integrateReference(refVolumes, ray, background);
    const RefResult actual = integrateSession(volumes, ray, background);
    TASK_CHECK(matches(actual, reference));
    TASK_CHECK(close(actual.transmittance, 1.0));

    // Zero extinction and zero emission: the background passes through.
    Volumes empty{{scene.a.grid<float>(), 0.0, Vec3(0.0)},
                  {scene.b.grid<float>(), 0.0, Vec3(0.0)}};
    const RefResult transparent = integrateSession(empty, ray, background);
    TASK_CHECK(close(transparent.transmittance, 1.0));
    TASK_CHECK(close(transparent.radiance, background));
    return 0;
}

int optics_start_inside_and_truncate()
{
    TwoBoxScene scene = makeTwoBoxScene();

    // Start inside the first box and truncate the ray inside the second one.
    const Vec3 eye(0.0, 0.0, 0.0);
    const Vec3 dir(1.0, 0.0, 0.0);
    const Ray  ray(eye, dir, 0.0, 5.0);
    const Vec3 background(0.0, 0.0, 0.0);

    std::vector<RefVolume> refVolumes;
    for (const auto& v : scene.volumes)
        refVolumes.push_back({v.grid, v.extinction, v.emission});

    const RefResult reference = integrateReference(refVolumes, ray, background);
    const RefResult actual = integrateSession(scene.volumes, ray, background);
    TASK_CHECK(matches(actual, reference));
    // Truncation must leave light unabsorbed: T is larger than for the full ray.
    const Ray full(Vec3(0.0, 0.0, 0.0), dir, 0.0, 50.0);
    const RefResult fullResult = integrateSession(scene.volumes, full, background);
    TASK_CHECK(actual.transmittance > fullResult.transmittance);

    // Start and end exactly on voxel faces, both axes aligned with the
    // integer boundary.  Equal-distance boundaries must take effect together
    // and a co-planar zero-length segment must contribute nothing.
    const Ray faceRay(Vec3(0.0, 0.0, 0.0), Vec3(1.0, 0.0, 0.0), 0.0, 10.0);
    const RefResult faceReference = integrateReference(refVolumes, faceRay, background);
    const RefResult faceActual = integrateSession(scene.volumes, faceRay, background);
    TASK_CHECK(matches(faceActual, faceReference));

    // Reverse from the truncated exit: identical segment, opposite walk.
    // (The truncated forward ray reached t=5 whereas this one starts there, so
    // the optically equivalent forward interval is [0, 5] of the same dir.)
    const Ray backRay(Vec3(5.0, 0.0, 0.0), Vec3(-1.0, 0.0, 0.0), 0.0, 5.0);
    const RefResult backReference = integrateReference(refVolumes, backRay, background);
    const RefResult backActual = integrateSession(scene.volumes, backRay, background);
    TASK_CHECK(matches(backActual, backReference));
    const Ray forwardSame(Vec3(0.0, 0.0, 0.0), Vec3(1.0, 0.0, 0.0), 0.0, 5.0);
    TASK_CHECK(close(backActual.transmittance,
                     integrateSession(scene.volumes, forwardSame, background).transmittance));
    return 0;
}

// ===========================================================================
// HIERARCHY
// ===========================================================================

int hierarchy_active_tile_usage()
{
    // A constant 8^3 active tile and a constant 128^3 active tile.  The
    // number of span steps must reflect the tile count, not the voxel count.
    // The builder's addTile() seeds newly created child nodes with the tile's
    // value and state, so create the branch inactive first and then activate
    // exactly the requested constant region.
    BuildGrid lowerTile(0.0f);
    lowerTile.tree().root().addTile<1>(nanovdb::Coord(0, 0, 0), 0.0f, false);
    lowerTile.tree().root().addTile<1>(nanovdb::Coord(0, 0, 0), 0.8f, true);

    BuildGrid upperTile(0.0f);
    upperTile.tree().root().addTile<2>(nanovdb::Coord(0, 0, 0), 0.0f, false);
    upperTile.tree().root().addTile<2>(nanovdb::Coord(0, 0, 0), 0.6f, true);

    Handle lowerHandle = convert(lowerTile);
    Handle upperHandle = convert(upperTile);
    const Grid* lower = lowerHandle.grid<float>();
    const Grid* upper = upperHandle.grid<float>();
    TASK_CHECK(lower != nullptr && upper != nullptr);
    TASK_CHECK(lower->tree().nodeCount(0) == 0); // no leaf nodes at all
    TASK_CHECK(upper->tree().nodeCount(0) == 0);
    TASK_CHECK(lower->tree().activeTileCount(1) > 0);
    TASK_CHECK(upper->tree().activeTileCount(2) > 0);

    const Ray rayLower(Vec3(-2.0, 4.0, 4.0), Vec3(1.0, 0.0, 0.0), 0.0, 12.0);
    const Ray rayUpper(Vec3(-2.0, 64.0, 64.0), Vec3(1.0, 0.0, 0.0), 0.0, 132.0);

    std::size_t stepsLower = 0, stepsUpper = 0;
    const RefResult actualLower = integrateSession({{lower, 0.5, Vec3(0.0)}}, rayLower, Vec3(0.0), 1u << 20, &stepsLower);
    const RefResult actualUpper = integrateSession({{upper, 0.5, Vec3(0.0)}}, rayUpper, Vec3(0.0), 1u << 20, &stepsUpper);

    // Analytic: constant density 0.8 over 8 voxels, 0.6 over 128 voxels.
    TASK_CHECK(close(actualLower.transmittance, std::exp(-0.8 * 0.5 * 8.0)));
    TASK_CHECK(close(actualUpper.transmittance, std::exp(-0.6 * 0.5 * 128.0)));
    TASK_CHECK(stepsLower <= 4);   // background + one tile + background
    TASK_CHECK(stepsUpper <= 4);

    const RefResult refLower = integrateReference({{lower, 0.5, Vec3(0.0)}}, rayLower, Vec3(0.0));
    TASK_CHECK(matches(actualLower, refLower));

    // Construction must not pre-traverse.
    nanovdb::tools::FogSession fresh({{lower, 0.5, Vec3(0.0)}}, rayLower, Vec3(0.0));
    TASK_CHECK(fresh.spanSteps() == 0);
    return 0;
}

int hierarchy_uniform_block_work()
{
    // Independently built active uniform block: x in [0, 4096),
    // y,z in [0, 128), represented by 32 constant 128^3 active tiles.
    BuildGrid block(0.0f);
    block.tree().root().addTile<2>(nanovdb::Coord(0, 0, 0), 0.0f, false);
    for (int x = 0; x < 4096; x += 128)
        block.tree().root().addTile<2>(nanovdb::Coord(x, 0, 0), 1.0f, true);
    Handle handle = convert(block);
    const Grid* g = handle.grid<float>();
    TASK_CHECK(g != nullptr);
    TASK_CHECK(g->tree().nodeCount(0) == 0);
    TASK_CHECK(g->tree().activeTileCount(2) == 32);
    TASK_CHECK(g->activeVoxelCount() == uint64_t(4096) * 128 * 128);

    const double extinction = 0.03;
    const Vec3   emission(0.01, 0.02, 0.03);
    const Ray    ray(Vec3(-10.0, 64.5, 64.5), Vec3(1.0, 0.0, 0.0), 0.0, 4120.0);

    const Volumes volumes{{g, extinction, emission}};
    std::size_t steps = 0;
    const RefResult actual = integrateSession(volumes, ray, Vec3(0.0), 1u << 20, &steps);

    // World-space path through the block is 4096 units.
    const double length = 4096.0;
    const double sigma = 1.0 * extinction;
    const double expectedT = std::exp(-sigma * length);
    const Vec3   expectedC = emission * ((1.0 - expectedT) / sigma); // j/sigma*(1-T), T0=1
    TASK_CHECK(close(actual.transmittance, expectedT));
    TASK_CHECK(close(actual.radiance, expectedC));

    // 32 tiles plus the two background cells outside the block.
    TASK_CHECK(steps <= 64);
    TASK_CHECK(steps >= 32);
    // A single volume must stay well below 64 span steps, as required: each
    // of the 32 constant tiles is one span and each vacuum cell is one span.
    TASK_CHECK(steps < 64);

    // The persistent state is independent of the number of voxels traversed:
    // a second session on the same ray performs exactly the same work.
    std::size_t again = 0;
    integrateSession(volumes, ray, Vec3(0.0), 1u << 20, &again);
    TASK_CHECK(again == steps);

    // Reference agreement on the same large block (sampled per voxel plane).
    const RefResult reference = integrateReference({{g, extinction, emission}}, ray, Vec3(0.0));
    TASK_CHECK(matches(actual, reference));

    // Persistent memory follows the tree depth, not the traversal length: a
    // ray that additionally crosses 200 empty 128^3 background cells on each
    // side still returns, and its span count grows by one cell per boundary
    // rather than by the voxels in those cells.
    const Ray wide(Vec3(-4096.0, 64.5, 64.5), Vec3(1.0, 0.0, 0.0), 0.0, 4120.0 + 2.0 * 4096.0);
    std::size_t wideSteps = 0;
    const RefResult wideResult = integrateSession(volumes, wide, Vec3(0.0), 1u << 20, &wideSteps);
    TASK_CHECK(close(wideResult.transmittance, expectedT));
    TASK_CHECK(close(wideResult.radiance, expectedC));
    // 32 vacated + 32 active + 2 outer background cells; still far below the
    // 4096 * 128^2 voxels those cells contain.
    TASK_CHECK(wideSteps <= 96);
    TASK_CHECK(wideSteps > steps);
    return 0;
}

// ===========================================================================
// RESUME
// ===========================================================================

int resume_budget_sequences()
{
    TwoBoxScene scene = makeTwoBoxScene();
    const Ray ray(Vec3(-20.0, 0.4, -0.3), Vec3(1.0, 0.03, 0.02), 0.0, 50.0);
    const Vec3 background(0.1, 0.2, 0.3);

    // One-shot reference run.
    const RefResult single = integrateSession(scene.volumes, ray, background);

    // Every fixed budget must produce the same optics and the same number of
    // fetched hierarchy spans.
    nanovdb::tools::FogSession probe(scene.volumes, ray, background);
    TASK_CHECK(probe.spanSteps() == 0);
    while (!probe.advance(1u << 20)) {
    }
    const std::size_t expectedSteps = probe.spanSteps();

    for (std::size_t budget : {std::size_t(1), std::size_t(2), std::size_t(3), std::size_t(5)}) {
        nanovdb::tools::FogSession session(scene.volumes, ray, background);
        std::size_t previous = session.spanSteps();
        std::size_t guard = 0;
        while (!session.advance(budget)) {
            const std::size_t now = session.spanSteps();
            TASK_CHECK(now - previous <= budget); // never exceed the budget
            previous = now;
            TASK_CHECK(++guard < 1000000);
        }
        TASK_CHECK(session.spanSteps() - previous <= budget);
        const auto r = session.result();
        TASK_CHECK(matches(RefResult{r.transmittance, r.radiance}, single));
        TASK_CHECK(session.spanSteps() == expectedSteps);
    }

    // Irregular budget sequence.
    {
        const std::size_t sequence[] = {1, 4, 2, 1, 7, 1, 1, 3, 2, 1, 5};
        nanovdb::tools::FogSession session(scene.volumes, ray, background);
        std::size_t i = 0, previous = session.spanSteps(), guard = 0;
        while (!session.advance(sequence[i++ % (sizeof(sequence) / sizeof(sequence[0]))])) {
            const std::size_t now = session.spanSteps();
            TASK_CHECK(now >= previous);
            previous = now;
            TASK_CHECK(++guard < 1000000);
        }
        const auto r = session.result();
        TASK_CHECK(matches(RefResult{r.transmittance, r.radiance}, single));
        TASK_CHECK(session.spanSteps() == expectedSteps);
    }
    return 0;
}

int resume_equal_boundary_pause()
{
    // Two identical volumes have exactly coincident region boundaries.  A
    // budget of one must leave the second volume's matched span pending, and
    // resuming must reach the same answer as a single-shot run.
    BuildGrid grid(0.0f);
    setBox(grid, nanovdb::Coord(-4, -4, -4), nanovdb::Coord(4, 4, 4), 0.9f);
    Handle handle = convert(grid);
    const Grid* g = handle.grid<float>();
    TASK_CHECK(g != nullptr);

    const Volumes volumes{{g, 0.35, Vec3(0.2, 0.0, 0.0)},
                          {g, 0.45, Vec3(0.0, 0.1, 0.3)}};
    const Ray  ray(Vec3(-12.0, 0.25, 0.25), Vec3(1.0, 0.0, 0.0), 0.0, 30.0);
    const Vec3 background(0.05, 0.05, 0.05);

    const RefResult reference = integrateReference({{g, 0.35, Vec3(0.2, 0.0, 0.0)},
                                                    {g, 0.45, Vec3(0.0, 0.1, 0.3)}},
                                                   ray, background);

    nanovdb::tools::FogSession session(volumes, ray, background);
    std::size_t guard = 0;
    std::size_t previous = session.spanSteps();
    bool        sawPending = false;
    while (!session.advance(1)) {
        const std::size_t now = session.spanSteps();
        TASK_CHECK(now - previous <= 1);
        if (now > 0 && now % 2 == 1)
            sawPending = true; // one of a coincident pair fetched, other pending
        previous = now;
        TASK_CHECK(++guard < 1000000);
    }
    TASK_CHECK(sawPending);
    const auto r = session.result();
    TASK_CHECK(matches(RefResult{r.transmittance, r.radiance}, reference));

    // Partial results must not include the background and must be monotone.
    nanovdb::tools::FogSession partial(volumes, ray, background);
    TASK_CHECK(!partial.advance(2));
    const auto firstResult = partial.result();
    TASK_CHECK(firstResult.transmittance <= 1.0);
    TASK_CHECK(!partial.advance(2));
    const auto secondResult = partial.result();
    TASK_CHECK(secondResult.transmittance <= firstResult.transmittance);

    // A budget that stops exactly in the middle of a pair of equal-distance
    // events must retain the pending state: the next single step does not
    // re-fetch and does not integrate ahead of the pending volume.
    // Resuming after an odd step count must still agree with a one-shot run.
    nanovdb::tools::FogSession resumed(volumes, ray, background);
    TASK_CHECK(!resumed.advance(1));   // fetch the first background span
    TASK_CHECK(!resumed.advance(1));   // fetch one volume's region
    const auto queued = resumed.result();
    TASK_CHECK(close(queued.radiance, Vec3(0.0))); // no integration yet
    const std::size_t queuedSteps = resumed.spanSteps();
    TASK_CHECK(queuedSteps == 2);
    while (!resumed.advance(1)) {
    }
    const auto resumedResult = resumed.result();
    TASK_CHECK(matches(RefResult{resumedResult.transmittance, resumedResult.radiance}, reference));
    return 0;
}

int resume_interleaved_sessions()
{
    TwoBoxScene scene = makeTwoBoxScene();
    const Ray rayA(Vec3(-20.0, 0.4, -0.3), Vec3(1.0, 0.03, 0.02), 0.0, 50.0);
    const Ray rayB(Vec3(20.0, -0.2, 0.1), Vec3(-1.0, 0.01, -0.02), 0.0, 50.0);
    const Vec3 background(0.1, 0.2, 0.3);

    const RefResult aloneA = integrateSession(scene.volumes, rayA, background);
    const RefResult aloneB = integrateSession(scene.volumes, rayB, background);

    nanovdb::tools::FogSession sessionA(scene.volumes, rayA, background);
    nanovdb::tools::FogSession sessionB(scene.volumes, rayB, background);
    std::size_t guard = 0;
    bool doneA = false, doneB = false;
    while (!doneA || !doneB) {
        if (!doneA)
            doneA = sessionA.advance(1);
        if (!doneB)
            doneB = sessionB.advance(2);
        TASK_CHECK(++guard < 2000000);
    }
    const auto a = sessionA.result();
    const auto b = sessionB.result();
    TASK_CHECK(matches(RefResult{a.transmittance, a.radiance}, aloneA));
    TASK_CHECK(matches(RefResult{b.transmittance, b.radiance}, aloneB));
    return 0;
}

int resume_completed_repeat_and_zero_budget()
{
    TwoBoxScene scene = makeTwoBoxScene();
    const Ray ray(Vec3(-20.0, 0.4, -0.3), Vec3(1.0, 0.03, 0.02), 0.0, 50.0);
    const Vec3 background(0.1, 0.2, 0.3);

    nanovdb::tools::FogSession session(scene.volumes, ray, background);

    // Budget zero never makes progress and reports incompleteness.
    TASK_CHECK(!session.advance(0));
    TASK_CHECK(session.spanSteps() == 0);
    const auto before = session.result();
    TASK_CHECK(close(before.transmittance, 1.0));
    TASK_CHECK(close(before.radiance, Vec3(0.0)));

    nanovdb::tools::FogResult lastIncomplete;
    std::size_t guard = 0;
    for (;;) {
        lastIncomplete = session.result();
        if (session.advance(3))
            break;
        TASK_CHECK(++guard < 1000000);
    }
    const auto completed = session.result();
    const std::size_t steps = session.spanSteps();

    // Repeated calls after completion are idempotent and return true.
    TASK_CHECK(session.advance(0));
    TASK_CHECK(session.advance(1));
    TASK_CHECK(session.advance(1000));
    const auto repeated = session.result();
    TASK_CHECK(session.spanSteps() == steps);
    TASK_CHECK(close(repeated.transmittance, completed.transmittance));
    TASK_CHECK(close(repeated.radiance, completed.radiance));

    // The last incomplete result holds only the committed C and T; the
    // completed result adds the background exactly once.
    TASK_CHECK(close(completed.transmittance, lastIncomplete.transmittance));
    TASK_CHECK(close(completed.radiance,
                     lastIncomplete.radiance + background * completed.transmittance));
    return 0;
}

int resume_example_pixels()
{
    // Drive the original CPU example, which now integrates through
    // FogSession, and check that it produces real, finite pixels.
    auto handle = nanovdb::tools::createFogVolumeSphere<float>(6.0, Vec3(0.0), 1.0, 3.0);
    TASK_CHECK(static_cast<bool>(handle));
    const int width = 8, height = 8;
    nanovdb::HostBuffer image(sizeof(float) * std::size_t(width) * height);
    TASK_CHECK(image.size() == sizeof(float) * std::size_t(width) * height);

    runNanoVDB(handle, 1, width, height, image);

    const float* pixels = static_cast<const float*>(image.data());
    int          opaque = 0, transparent = 0;
    for (int i = 0; i < width * height; ++i) {
        const float p = pixels[i];
        TASK_CHECK(std::isfinite(p));
        TASK_CHECK(p >= 0.0f && p <= 1.0f);
        if (p > 0.01f)
            ++opaque;
        if (p < 0.99f)
            ++transparent;
    }
    TASK_CHECK(opaque > 0);
    TASK_CHECK(transparent > 0);
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"optics", "uniform_box_scales", optics_uniform_box_scales},
    {"optics", "overlapping_colors", optics_overlapping_colors},
    {"optics", "inactive_nonzero", optics_inactive_nonzero},
    {"optics", "zero_extinction_emission", optics_zero_extinction_emission},
    {"optics", "start_inside_and_truncate", optics_start_inside_and_truncate},
    {"hierarchy", "active_tile_usage", hierarchy_active_tile_usage},
    {"hierarchy", "uniform_block_work", hierarchy_uniform_block_work},
    {"resume", "budget_sequences", resume_budget_sequences},
    {"resume", "equal_boundary_pause", resume_equal_boundary_pause},
    {"resume", "interleaved_sessions", resume_interleaved_sessions},
    {"resume", "completed_repeat_and_zero_budget", resume_completed_repeat_and_zero_budget},
    {"resume", "example_pixels", resume_example_pixels},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
