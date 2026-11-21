/*
 * Tests for the CPU side ownership, read-only queries and batched frames.
 */

#include "task_support/check.h"

#include "headers/CPUFrame.h"
#include "headers/Camera.h"
#include "headers/Octree.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    struct RaySettings
    {
        bool transparency;
        unsigned int minVoxelsProximity;
        bool avoidScanGlitches;
    };

    RaySettings plainRay()
    {
        RaySettings settings;
        settings.transparency = false;
        settings.minVoxelsProximity = 0;
        settings.avoidScanGlitches = false;
        return settings;
    }

    void resetOptions()
    {
        Options::nologs = true;
        Options::ACCELERATED = false;
        Options::SMOOTH_SIBLINGS = false;
        Options::AVOID_SCAN_GLITCHES = false;
        Options::TRANSPARENCY = false;
        Options::INTERNAL_BLENDING = true;
        Options::QUADS_AS_PIXELS = false;
        Options::VOXEL_SHELL_TYPE = 0;
        Options::SHELL_COLOR = 0;
        Options::SHELL_LITE_THICKNESS = 0;
        Options::PIXEL_STEP = 1;
        Options::MAX_VOXELS_BY_RAY = 1;
        Options::MIN_VOXELS_PROXIMITY = 0;
        Options::PERSPECTIVE_FACTOR = 0.0f;
        Options::ZOOM_FACTOR = 1.0f;
        Options::OCTREE_SIZE = 16;
        Options::SCR_WIDTH = 256;
        Options::SCR_HEIGHT = 256;
        Options::SCR_HALF_WIDTH = 128;
        Options::SCR_HALF_HEIGHT = 128;
        Options::MAX_RAY_LENGTH = 16;
    }

    Mat3f identityBasis()
    {
        Mat3f basis = {
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f}
        };
        return basis;
    }

    Mat3f cameraBasis(const Vec3<float> axis, const float angle,
    const Vec3<float>& position)
    {
        Camera camera;
        camera.setPosition(position.x, position.y, position.z);
        camera.rotate(axis, angle);
        Mat3f basis;
        camera.getBasis(&basis);
        return basis;
    }

    /*
    * The insertion entry point keeps a pointer to the caller's voxel, as the
    * production mesh does, so the storage has to outlive the tree. A stable
    * arena keeps the tests' short lived voxels valid until process exit.
    */
    std::vector<std::unique_ptr<Voxel> >& voxelArena()
    {
        static std::vector<std::unique_ptr<Voxel> > arena;
        return arena;
    }

    void insertVoxel(Octree& tree, const int x, const int y, const int z,
    const unsigned int color)
    {
        std::unique_ptr<Voxel> voxel(new Voxel());
        voxel->coordinates.x = (SI)x;
        voxel->coordinates.y = (SI)y;
        voxel->coordinates.z = (SI)z;
        voxel->color = color;
        Voxel* const stored = voxel.get();
        voxelArena().push_back(std::move(voxel));
        tree.addVoxels(stored, 1);
    }

    Octree::RayQuery query(const Vec3<float>& origin,
    const Vec3<float>& direction, const float maxDistance,
    const RaySettings& settings)
    {
        Octree::RayQuery q;
        q.origin = origin;
        q.direction = direction;
        q.maxDistance = maxDistance;
        q.transparency = settings.transparency;
        q.minVoxelsProximity = (float)settings.minVoxelsProximity;
        q.avoidScanGlitches = settings.avoidScanGlitches;
        return q;
    }

    struct LegacyHit
    {
        float depth;
        UC r, g, b, a;
    };

    /*
    * Scalar reference built on the legacy mutating entry point. The tree is
    * only used to host the ray state, the ray itself is a local copy.
    */
    std::vector<LegacyHit> legacyTrace(Octree& tree, const Vec3<float>& origin,
    const Vec3<float>& direction, const RaySettings& settings)
    {
        Options::TRANSPARENCY = settings.transparency;
        Options::MIN_VOXELS_PROXIMITY = (SUI)settings.minVoxelsProximity;
        Options::AVOID_SCAN_GLITCHES = settings.avoidScanGlitches;

        Mat3f basis = identityBasis();
        basis.k = direction;
        tree.initBasis(&basis);
        Vec3<float> ray = origin;
        tree.setRay(&ray);

        std::vector<DynamicVoxel> voxels;
        tree.rayTrace(&voxels);

        std::vector<LegacyHit> hits;
        for(size_t i = 0; i < voxels.size(); i++)
        {
            const UI color = voxels[i].voxel->color;
            LegacyHit hit;
            hit.depth = voxels[i].depth;
            hit.r = (UC)((color & 0xFF000000) >> 24);
            hit.g = (UC)((color & 0x00FF0000) >> 16);
            hit.b = (UC)((color & 0x0000FF00) >> 8);
            hit.a = (UC)(color & 0x000000FF);
            hits.push_back(hit);
        }
        return hits;
    }

    bool sameHits(const std::vector<Octree::RayHit>& actual,
    const std::vector<LegacyHit>& expected)
    {
        if(actual.size() != expected.size())
        {
            return false;
        }
        for(size_t i = 0; i < actual.size(); i++)
        {
            if(std::fabs(actual[i].depth - expected[i].depth) > 1e-4f ||
                actual[i].color.r != expected[i].r ||
                actual[i].color.g != expected[i].g ||
                actual[i].color.b != expected[i].b ||
                actual[i].color.a != expected[i].a)
            {
                return false;
            }
        }
        return true;
    }

    LegacyHit legacyHit(const Octree::RayHit& hit)
    {
        LegacyHit legacy;
        legacy.depth = hit.depth;
        legacy.r = hit.color.r;
        legacy.g = hit.color.g;
        legacy.b = hit.color.b;
        legacy.a = hit.color.a;
        return legacy;
    }

    CPUFrameViewport makeViewport(const Octree* octree, const int width,
    const int height, const Vec3<float>& position, const Mat3f& basis,
    const Color background = {0, 0, 0, 255})
    {
        CPUFrameViewport viewport;
        viewport.octree = octree;
        viewport.basis = basis;
        viewport.position = position;
        viewport.width = width;
        viewport.height = height;
        viewport.pixelStep = 1;
        viewport.perspectiveFactor = 0.0f;
        viewport.zoom = 1.0f;
        viewport.background = background;
        viewport.transparentR = 1.0f;
        viewport.transparentG = 1.0f;
        viewport.transparentB = 1.0f;
        viewport.transparentA = 1.0f;
        return viewport;
    }

    CPUFrameOptions frameOptions(const int workers, const int tileRows)
    {
        CPUFrameOptions options = cpuFrameDefaultOptions();
        options.workers = workers;
        options.tileRows = tileRows;
        return options;
    }

    CPUFrameOptions blendOptions(const int workers, const int tileRows,
    const unsigned int maxVoxelsByRay = 8,
    const unsigned int minVoxelsProximity = 0)
    {
        CPUFrameOptions options = frameOptions(workers, tileRows);
        options.transparency = true;
        options.maxVoxelsByRay = maxVoxelsByRay;
        options.minVoxelsProximity = (float)minVoxelsProximity;
        return options;
    }

    bool sameResults(const std::vector<CPUFrameResult>& a,
    const std::vector<CPUFrameResult>& b)
    {
        if(a.size() != b.size())
        {
            return false;
        }
        for(size_t i = 0; i < a.size(); i++)
        {
            if(a[i].width != b[i].width || a[i].height != b[i].height ||
                a[i].pixels != b[i].pixels)
            {
                return false;
            }
        }
        return true;
    }

    const UC* pixelAt(const CPUFrameResult& result, const int x, const int y)
    {
        return &result.pixels[((size_t)y * (size_t)result.width + x) * 4];
    }

    template<typename Function>
    bool throwsInvalidArgument(Function function)
    {
        try
        {
            function();
        }
        catch(const std::invalid_argument&)
        {
            return true;
        }
        catch(...)
        {
            return false;
        }
        return false;
    }
}

// ---------------------------------------------------------------------------
// ownership: camera rotation, tree geometry, accelerator and options
// ---------------------------------------------------------------------------

static int ownership_interleaved_cameras()
{
    Camera first;
    Camera second;

    first.rotate({0.0f, 1.0f, 0.0f}, 0.2f);
    second.rotate({1.0f, 0.0f, 0.0f}, 0.35f);
    first.rotate({0.0f, 1.0f, 0.0f}, 0.2f);
    second.rotate({1.0f, 0.0f, 0.0f}, 0.35f);

    Camera referenceFirst;
    referenceFirst.rotate({0.0f, 1.0f, 0.0f}, 0.2f);
    referenceFirst.rotate({0.0f, 1.0f, 0.0f}, 0.2f);

    Camera referenceSecond;
    referenceSecond.rotate({1.0f, 0.0f, 0.0f}, 0.35f);
    referenceSecond.rotate({1.0f, 0.0f, 0.0f}, 0.35f);

    Mat3f actual, expected, actualSecond, expectedSecond;
    first.getBasis(&actual);
    referenceFirst.getBasis(&expected);
    second.getBasis(&actualSecond);
    referenceSecond.getBasis(&expectedSecond);

    const Vec3<float>* actualVectors[3] = {&actual.i, &actual.j, &actual.k};
    const Vec3<float>* expectedVectors[3] = {&expected.i, &expected.j, &expected.k};
    for(int i = 0; i < 3; i++)
    {
        TASK_CHECK(std::fabs(actualVectors[i]->x - expectedVectors[i]->x) < 1e-6f);
        TASK_CHECK(std::fabs(actualVectors[i]->y - expectedVectors[i]->y) < 1e-6f);
        TASK_CHECK(std::fabs(actualVectors[i]->z - expectedVectors[i]->z) < 1e-6f);
    }
    TASK_CHECK(std::fabs(actualSecond.i.x - expectedSecond.i.x) < 1e-6f);
    TASK_CHECK(std::fabs(actualSecond.i.y - expectedSecond.i.y) < 1e-6f);
    TASK_CHECK(std::fabs(actualSecond.i.z - expectedSecond.i.z) < 1e-6f);
    TASK_CHECK(std::fabs(actualSecond.k.z - expectedSecond.k.z) < 1e-6f);
    return 0;
}

static int ownership_tree_sizes_and_bounds()
{
    resetOptions();
    RaySettings settings = plainRay();

    Octree large;
    large.initRoot(32, 5, 24);
    Octree small;
    small.initRoot(16, 4, 24);

    insertVoxel(large, 12, 0, 0, 0x112233FF);
    insertVoxel(small, 2, 0, 0, 0x445566FF);
    // Inserted into the large tree after the small one was built: the small
    // tree bounds must not reject it.
    insertVoxel(large, 13, 0, 0, 0x778899FF);
    // Out of the large tree bounds.
    insertVoxel(large, 15, 0, 0, 0xFF0000FF);
    // Out of the small tree bounds.
    insertVoxel(small, 12, 0, 0, 0x00FF00FF);

    TASK_CHECK(large.getRootSize() == 32);
    TASK_CHECK(small.getRootSize() == 16);

    std::vector<Octree::RayHit> hits = large.queryRay(query(
    {12.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 24.0f, settings));
    TASK_CHECK(hits.size() == 1);
    TASK_CHECK(hits.size() == 1 && hits[0].color.r == 0x11 &&
    hits[0].color.g == 0x22 && hits[0].color.b == 0x33);
    TASK_CHECK(hits.size() == 1 && std::fabs(hits[0].depth - 6.0f) < 1e-3f);

    hits = large.queryRay(query(
    {13.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 24.0f, settings));
    TASK_CHECK(hits.size() == 1 && hits[0].color.r == 0x77);

    hits = large.queryRay(query(
    {15.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 24.0f, settings));
    TASK_CHECK(hits.empty());

    hits = small.queryRay(query(
    {2.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 24.0f, settings));
    TASK_CHECK(hits.size() == 1 && hits[0].color.r == 0x44);

    hits = small.queryRay(query(
    {12.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 24.0f, settings));
    TASK_CHECK(hits.empty());
    return 0;
}

static int ownership_accelerator_per_instance()
{
    resetOptions();
    RaySettings settings = plainRay();

    Octree large;
    large.initRoot(32, 5, 24);
    insertVoxel(large, 12, 0, 0, 0xAABBCCFF);
    Octree small;
    small.initRoot(16, 4, 24);
    insertVoxel(small, 2, 0, 0, 0xDDEE00FF);

    large.initAccelerator();
    small.initAccelerator();
    // Calling it again is a no-op.
    large.initAccelerator();

    std::vector<Octree::RayHit> hits = large.queryRay(query(
    {12.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 24.0f, settings));
    TASK_CHECK(hits.size() == 1 && hits[0].color.r == 0xAA);
    hits = small.queryRay(query(
    {2.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 24.0f, settings));
    TASK_CHECK(hits.size() == 1 && hits[0].color.r == 0xDD);

    // The same voxels without an accelerator reach the same result.
    Octree largePlain;
    largePlain.initRoot(32, 5, 24);
    insertVoxel(largePlain, 12, 0, 0, 0xAABBCCFF);
    const std::vector<Octree::RayHit> expected = largePlain.queryRay(query(
    {12.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 24.0f, settings));
    const std::vector<Octree::RayHit> acceleratedHit = large.queryRay(query(
    {12.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 24.0f, settings));
    TASK_CHECK(expected.size() == 1);
    TASK_CHECK(sameHits(acceleratedHit,
    std::vector<LegacyHit>{legacyHit(expected[0])}));
    return 0;
}

static int ownership_single_init_and_options_snapshot()
{
    resetOptions();
    RaySettings settings = plainRay();

    Octree tree;
    tree.initRoot(16, 4, 16);
    // A second initialization must be ignored.
    tree.initRoot(64, 6, 32);
    TASK_CHECK(tree.getRootSize() == 16);

    insertVoxel(tree, 5, 0, 0, 0x010203FF);
    TASK_CHECK(tree.getRayLength() == 16.0f);

    // Later defaults changes must not retroactively move the tree bounds.
    Options::OCTREE_SIZE = 64;
    Options::MAX_RAY_LENGTH = 64;
    insertVoxel(tree, 6, 0, 0, 0x0A0B0CFF); // out of bounds for half == 8
    insertVoxel(tree, -5, 0, 0, 0x0D0E0FFF);

    std::vector<Octree::RayHit> hits = tree.queryRay(query(
    {5.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 16.0f, settings));
    TASK_CHECK(hits.size() == 1 && hits[0].color.r == 0x01);
    hits = tree.queryRay(query(
    {6.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 16.0f, settings));
    TASK_CHECK(hits.empty());
    hits = tree.queryRay(query(
    {-5.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 16.0f, settings));
    TASK_CHECK(hits.size() == 1 && hits[0].color.r == 0x0D);
    return 0;
}

// ---------------------------------------------------------------------------
// queries: standalone read-only entry point
// ---------------------------------------------------------------------------

static int queries_match_legacy_scalar()
{
    resetOptions();

    Octree tree;
    tree.initRoot(16, 4, 16);
    insertVoxel(tree, 0, 0, 0, 0xFF8040FF);
    insertVoxel(tree, 3, 0, 0, 0x2040FFFF);
    insertVoxel(tree, 0, 2, 0, 0xFFFFFFFF);

    const Vec3<float> origins[] = {
        {0.0f, 0.0f, -6.0f},
        {3.0f, 0.0f, -6.0f},
        {0.0f, 2.0f, -6.0f},
        {-4.0f, 0.0f, -6.0f},
        {0.0f, 0.0f, -8.0f}
    };
    const Vec3<float> directions[] = {
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f},
        {0.2f, 0.0f, 1.0f}
    };

    for(size_t o = 0; o < sizeof(origins) / sizeof(origins[0]); o++)
    {
        for(size_t d = 0; d < sizeof(directions) / sizeof(directions[0]); d++)
        {
            Vec3<float> direction = directions[d];
            math::normalize(&direction);
            const std::vector<LegacyHit> expected =
            legacyTrace(tree, origins[o], direction, plainRay());
            const std::vector<Octree::RayHit> actual =
            tree.queryRay(query(origins[o], direction, 16.0f, plainRay()));
            TASK_CHECK(sameHits(actual, expected));
        }
    }

    // Perspective direction generation matches setPerspectiveRay.
    {
        const float zoom = 1.0f;
        const float factor = 0.05f;
        const float screenX = 3.0f;
        const float screenY = -2.0f;
        Mat3f basis = identityBasis();
        Mat3f legacyBasis = identityBasis();
        tree.initBasis(&legacyBasis);
        Vec3<float> ray = {0.0f, 0.0f, -6.0f};
        tree.setRay(&ray);
        tree.setPerspectiveRay(zoom * screenX, zoom * screenY, factor);

        Vec3<float> direction = {0.0f, 0.0f, 1.0f};
        direction.x += (basis.i.x * (zoom * screenX) +
        basis.j.x * (zoom * screenY)) * factor;
        direction.y += (basis.i.y * (zoom * screenX) +
        basis.j.y * (zoom * screenY)) * factor;
        direction.z += (basis.i.z * (zoom * screenX) +
        basis.j.z * (zoom * screenY)) * factor;
        math::normalize(&direction);
        TASK_CHECK(std::fabs(direction.x - legacyBasis.k.x) < 1e-6f);
        TASK_CHECK(std::fabs(direction.y - legacyBasis.k.y) < 1e-6f);
        TASK_CHECK(std::fabs(direction.z - legacyBasis.k.z) < 1e-6f);
    }

    // The read-only query did not disturb the legacy path.
    std::vector<LegacyHit> legacyAfter = legacyTrace(tree,
    {0.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, plainRay());
    TASK_CHECK(legacyAfter.size() == 1);
    TASK_CHECK(legacyAfter.size() == 1 && legacyAfter[0].r == 0xFF &&
    legacyAfter[0].g == 0x80 && legacyAfter[0].b == 0x40);
    return 0;
}

static int queries_transparent_opaque_and_filtering()
{
    resetOptions();

    Octree tree;
    tree.initRoot(16, 4, 16);
    insertVoxel(tree, 0, 0, 0, 0xFF000080); // transparent red
    insertVoxel(tree, 0, 0, 3, 0x00FF0080); // transparent green
    insertVoxel(tree, 0, 0, 5, 0x0000FFFF); // opaque blue

    RaySettings settings;
    settings.transparency = true;
    settings.minVoxelsProximity = 0;
    settings.avoidScanGlitches = false;

    std::vector<Octree::RayHit> hits = tree.queryRay(query(
    {0.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 16.0f, settings));
    // The opaque voxel stops the ray, two transparent hits before it.
    TASK_CHECK(hits.size() == 3);
    TASK_CHECK(hits.size() == 3 && hits[0].color.a == 0x80 &&
    hits[0].color.r == 0xFF && hits[1].color.a == 0x80 &&
    hits[1].color.g == 0xFF && hits[2].color.a == 0xFF &&
    hits[2].color.b == 0xFF);
    TASK_CHECK(std::fabs(hits[0].depth - 6.0f) < 1e-3f);
    TASK_CHECK(std::fabs(hits[1].depth - 9.0f) < 1e-3f);
    TASK_CHECK(std::fabs(hits[2].depth - 11.0f) < 1e-3f);
    TASK_CHECK(hits[0].depth < hits[1].depth && hits[1].depth < hits[2].depth);

    // Match the legacy transparent behaviour, same settings.
    const std::vector<LegacyHit> expected =
    legacyTrace(tree, {0.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, settings);
    TASK_CHECK(expected.size() == 3);
    TASK_CHECK(sameHits(tree.queryRay(query(
    {0.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 16.0f, settings)), expected));

    // The minimum proximity filters close transparent hits.
    settings.minVoxelsProximity = 4;
    hits = tree.queryRay(query(
    {0.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 16.0f, settings));
    TASK_CHECK(hits.size() == 2);
    TASK_CHECK(hits.size() == 2 && hits[0].color.r == 0xFF &&
    hits[1].color.b == 0xFF);

    // Opaque only returns the first hit.
    const std::vector<Octree::RayHit> opaque = tree.queryRay(query(
    {0.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 16.0f, plainRay()));
    TASK_CHECK(opaque.size() == 1 && opaque[0].color.r == 0xFF);
    return 0;
}

static int queries_miss_and_accelerator_toggle()
{
    resetOptions();
    RaySettings settings = plainRay();

    Octree plain;
    plain.initRoot(16, 4, 16);
    insertVoxel(plain, 0, 0, 0, 0x112233FF);
    insertVoxel(plain, 7, 0, 0, 0x445566FF);

    std::vector<Octree::RayHit> expected = plain.queryRay(query(
    {0.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 16.0f, settings));

    Octree accelerated;
    accelerated.initRoot(16, 4, 16);
    insertVoxel(accelerated, 0, 0, 0, 0x112233FF);
    insertVoxel(accelerated, 7, 0, 0, 0x445566FF);
    accelerated.initAccelerator();

    std::vector<Octree::RayHit> hits = accelerated.queryRay(query(
    {0.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 16.0f, settings));
    TASK_CHECK(hits.size() == 1);
    TASK_CHECK(sameHits(hits,
    std::vector<LegacyHit>{legacyHit(expected[0])}));

    // Miss on both paths.
    TASK_CHECK(accelerated.queryRay(query(
    {-8.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 16.0f, settings)).empty());
    TASK_CHECK(plain.queryRay(query(
    {0.0f, 0.0f, 8.0f}, {0.0f, 0.0f, 1.0f}, 16.0f, settings)).empty());

    // A short maximum distance never reaches the voxel.
    TASK_CHECK(plain.queryRay(query(
    {0.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 5.0f, settings)).empty());
    return 0;
}

static int queries_input_immutability_and_copies()
{
    resetOptions();

    Octree tree;
    tree.initRoot(16, 4, 16);
    insertVoxel(tree, 0, 0, 0, 0xFF8040FF);

    Octree::RayQuery original = query(
    {0.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 16.0f, plainRay());
    Octree::RayQuery mutated = original;

    const std::vector<Octree::RayHit> hits = tree.queryRay(original);
    TASK_CHECK(hits.size() == 1);
    TASK_CHECK(std::memcmp(&original, &mutated, sizeof(original)) == 0);

    // The result owns copies of the colors: overwrite the caller storage and
    // run another query, the previous result is unchanged.
    const std::vector<Octree::RayHit> firstCopy = hits;
    mutated.origin = {2.0f, 0.0f, -6.0f};
    mutated.direction = {0.0f, 0.0f, -1.0f};
    const std::vector<Octree::RayHit> second = tree.queryRay(mutated);
    TASK_CHECK(second.empty());
    TASK_CHECK(sameHits(firstCopy,
    std::vector<LegacyHit>{legacyHit(hits[0])}));

    // Results stay valid after their tree is gone: they are copies, not views.
    Octree* temporary = new Octree();
    temporary->initRoot(16, 4, 16);
    insertVoxel(*temporary, 0, 0, 0, 0x010203FF);
    const std::vector<Octree::RayHit> detached = temporary->queryRay(query(
    {0.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 16.0f, plainRay()));
    delete temporary;
    TASK_CHECK(detached.size() == 1 && detached[0].color.r == 0x01 &&
    detached[0].color.g == 0x02 && detached[0].color.b == 0x03);
    return 0;
}

static int queries_concurrent_same_tree()
{
    resetOptions();

    Octree tree;
    tree.initRoot(16, 4, 16);
    insertVoxel(tree, 0, 0, 0, 0xFF0000FF);
    insertVoxel(tree, 3, 0, 0, 0x00FF00FF);
    insertVoxel(tree, 0, 2, 0, 0x0000FFFF);

    struct Probe
    {
        Vec3<float> origin;
        Vec3<float> direction;
    };
    const Probe probes[] = {
        {{0.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}},
        {{3.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.0f, 2.0f, -6.0f}, {0.0f, 0.0f, 1.0f}},
        {{-3.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}}
    };
    const size_t probeCount = sizeof(probes) / sizeof(probes[0]);

    std::vector<std::vector<Octree::RayHit> > expected(probeCount);
    for(size_t p = 0; p < probeCount; p++)
    {
        expected[p] = tree.queryRay(query(probes[p].origin,
        probes[p].direction, 16.0f, plainRay()));
    }

    std::atomic<bool> ok(true);
    std::vector<std::thread> threads;
    for(int t = 0; t < 8; t++)
    {
        threads.push_back(std::thread([&tree, &probes, &expected, &ok]() {
            for(int iteration = 0; iteration < 100; iteration++)
            {
                for(size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); p++)
                {
                    const std::vector<Octree::RayHit> actual =
                    tree.queryRay(query(probes[p].origin, probes[p].direction,
                    16.0f, plainRay()));
                    if(actual.size() != expected[p].size())
                    {
                        ok = false;
                        return;
                    }
                    for(size_t i = 0; i < actual.size(); i++)
                    {
                        if(std::fabs(actual[i].depth -
                            expected[p][i].depth) > 1e-5f ||
                            std::memcmp(&actual[i].color,
                            &expected[p][i].color, sizeof(Color)) != 0)
                        {
                            ok = false;
                            return;
                        }
                    }
                }
            }
        }));
    }
    for(size_t t = 0; t < threads.size(); t++)
    {
        threads[t].join();
    }
    TASK_CHECK(ok.load());
    TASK_CHECK(expected[0].size() == 1);
    TASK_CHECK(expected[1].size() == 1);
    TASK_CHECK(expected[2].size() == 1);
    TASK_CHECK(expected[3].empty());
    return 0;
}

static int queries_color_hand_computed()
{
    resetOptions();

    Octree tree;
    tree.initRoot(16, 4, 16);
    insertVoxel(tree, 0, 0, 0, 0xFF8040FF);

    const std::vector<Octree::RayHit> hits = tree.queryRay(query(
    {0.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 16.0f, plainRay()));
    TASK_CHECK(hits.size() == 1);
    TASK_CHECK(std::fabs(hits[0].depth - 6.0f) < 1e-4f);
    TASK_CHECK(hits[0].color.r == 0xFF && hits[0].color.g == 0x80 &&
    hits[0].color.b == 0x40 && hits[0].color.a == 0xFF);

    // Depth darkening: 1 - depth / ray length.
    const float darkness = tree.getDarkness(6.0f);
    TASK_CHECK(std::fabs(darkness - 0.625f) < 1e-6f);
    const Color shaded =
    tree.getColorDepth(0xFF, 0x80, 0x40, 6.0f);
    TASK_CHECK(shaded.r == 159 && shaded.g == 80 && shaded.b == 40);
    return 0;
}

// ---------------------------------------------------------------------------
// frames: batched multi viewport rendering
// ---------------------------------------------------------------------------

static int frames_hand_computed_pixel_and_background()
{
    resetOptions();

    Octree tree;
    tree.initRoot(16, 4, 16);
    insertVoxel(tree, 0, 0, 0, 0xFF8040FF);

    std::vector<CPUFrameViewport> viewports;
    viewports.push_back(makeViewport(&tree, 8, 8, {0.0f, 0.0f, -6.0f},
    identityBasis()));
    CPUFrameStats stats;
    const std::vector<CPUFrameResult> results =
    cpuFrameRender(viewports, frameOptions(2, 3), &stats);

    TASK_CHECK(results.size() == 1);
    TASK_CHECK(results[0].width == 8 && results[0].height == 8);
    TASK_CHECK(results[0].pixels.size() == 8 * 8 * 4);

    const UC* center = pixelAt(results[0], 4, 4);
    // 255 * (1 - 6/16) = 159, 128 * 0.625 = 80, 64 * 0.625 = 40.
    TASK_CHECK(center[0] == 159 && center[1] == 80 && center[2] == 40 &&
    center[3] == 0xFF);
    const UC* corner = pixelAt(results[0], 0, 0);
    TASK_CHECK(corner[0] == 0 && corner[1] == 0 && corner[2] == 0 &&
    corner[3] == 0xFF);

    // Misses use the background RGBA.
    viewports.clear();
    viewports.push_back(makeViewport(&tree, 8, 8, {0.0f, 0.0f, -6.0f},
    identityBasis(), {7, 11, 13, 42}));
    const std::vector<CPUFrameResult> misses =
    cpuFrameRender(viewports, frameOptions(1, 1));
    const UC* miss = pixelAt(misses[0], 0, 0);
    TASK_CHECK(miss[0] == 7 && miss[1] == 11 && miss[2] == 13 &&
    miss[3] == 42);
    return 0;
}

static int frames_transparent_blending()
{
    resetOptions();
    // The legacy globals stay at their defaults: the batch carries its own
    // blending settings and must not depend on them.
    TASK_CHECK(Options::TRANSPARENCY == false);

    Octree tree;
    tree.initRoot(16, 4, 16);
    insertVoxel(tree, 0, 0, 0, 0xFF000080);
    insertVoxel(tree, 0, 0, 3, 0x00FF0080);

    std::vector<CPUFrameViewport> viewports;
    viewports.push_back(makeViewport(&tree, 8, 8, {0.0f, 0.0f, -6.0f},
    identityBasis()));
    const std::vector<CPUFrameResult> results =
    cpuFrameRender(viewports, blendOptions(2, 2));

    // From back to front: green at depth 9, then red at depth 6.
    const float aFar = (128.0f / 255.0f) * (1.0f - 9.0f / 16.0f);
    const float aNear = (128.0f / 255.0f) * (1.0f - 6.0f / 16.0f);
    const float expectedR = 255.0f * aNear;
    const float expectedG = 255.0f * aFar * (1.0f - aNear);
    const UC* center = pixelAt(results[0], 4, 4);
    TASK_CHECK(std::fabs((float)center[0] - expectedR) <= 1.0f);
    TASK_CHECK(std::fabs((float)center[1] - expectedG) <= 1.0f);
    TASK_CHECK(center[2] == 0);
    TASK_CHECK(center[3] == 0xFF);

    // An opaque voxel in front stops the ray and is blended alone.
    insertVoxel(tree, 0, 0, -3, 0x123456FF);
    const std::vector<CPUFrameResult> opaque =
    cpuFrameRender(viewports, blendOptions(1, 4));
    const UC* opaqueCenter = pixelAt(opaque[0], 4, 4);
    const float opaqueDarkness = 1.0f - 3.0f / 16.0f;
    TASK_CHECK(opaqueCenter[0] == (UC)(0x12 * opaqueDarkness));
    TASK_CHECK(opaqueCenter[1] == (UC)(0x34 * opaqueDarkness));
    TASK_CHECK(opaqueCenter[2] == (UC)(0x56 * opaqueDarkness));
    TASK_CHECK(opaqueCenter[3] == 0xFF);
    return 0;
}

static int frames_perspective_and_orthographic()
{
    resetOptions();

    Octree tree;
    tree.initRoot(16, 4, 16);
    insertVoxel(tree, 4, 0, 0, 0xFFFFFFFF);

    std::vector<CPUFrameViewport> viewports;
    viewports.push_back(makeViewport(&tree, 16, 16, {0.0f, 0.0f, -6.0f},
    identityBasis(), {1, 2, 3, 255}));
    const std::vector<CPUFrameResult> orthographic =
    cpuFrameRender(viewports, frameOptions(1, 1));

    viewports[0].perspectiveFactor = 0.1f;
    viewports[0].zoom = 1.0f;
    const std::vector<CPUFrameResult> perspective =
    cpuFrameRender(viewports, frameOptions(2, 4));

    // Pixel (12, 8) is the screen x == 4 column.
    const UC* orthoPixel = pixelAt(orthographic[0], 12, 8);
    const UC* perspPixel = pixelAt(perspective[0], 12, 8);
    TASK_CHECK(orthoPixel[0] != 1 || orthoPixel[1] != 2 ||
    orthoPixel[2] != 3);
    TASK_CHECK(perspPixel[0] == 1 && perspPixel[1] == 2 &&
    perspPixel[2] == 3);

    // The centre pixel is unaffected by the perspective fan.
    const UC* orthoCenter = pixelAt(orthographic[0], 8, 8);
    const UC* perspCenter = pixelAt(perspective[0], 8, 8);
    TASK_CHECK(std::memcmp(orthoCenter, perspCenter, 4) == 0);
    return 0;
}

static int frames_two_cameras_same_tree()
{
    resetOptions();

    Octree tree;
    tree.initRoot(16, 4, 16);
    insertVoxel(tree, 0, 0, 0, 0xFF8040FF);
    insertVoxel(tree, 3, 0, 0, 0x2040FFFF);
    insertVoxel(tree, 0, 2, 0, 0xFFFF40FF);

    const Mat3f basisA = identityBasis();
    const Mat3f basisB = cameraBasis({0.0f, 1.0f, 0.0f}, 0.4f,
    {0.0f, 0.0f, -6.0f});

    std::vector<CPUFrameViewport> viewports;
    viewports.push_back(makeViewport(&tree, 8, 8, {0.0f, 0.0f, -6.0f},
    basisA, {5, 6, 7, 255}));
    viewports.push_back(makeViewport(&tree, 8, 8, {0.0f, 0.0f, -6.0f},
    basisB, {5, 6, 7, 255}));

    const std::vector<CPUFrameResult> batch =
    cpuFrameRender(viewports, frameOptions(4, 2));
    TASK_CHECK(batch.size() == 2);
    TASK_CHECK(!(batch[0].pixels == batch[1].pixels));

    std::vector<CPUFrameViewport> single;
    single.push_back(viewports[0]);
    const std::vector<CPUFrameResult> aloneA =
    cpuFrameRender(single, frameOptions(1, 1));
    single[0] = viewports[1];
    const std::vector<CPUFrameResult> aloneB =
    cpuFrameRender(single, frameOptions(1, 1));

    TASK_CHECK(sameResults(std::vector<CPUFrameResult>{batch[0]}, aloneA));
    TASK_CHECK(sameResults(std::vector<CPUFrameResult>{batch[1]}, aloneB));

    // Output storage is not shared.
    TASK_CHECK(batch[0].pixels.data() != batch[1].pixels.data());
    TASK_CHECK(batch[0].pixels.size() == 8 * 8 * 4);
    TASK_CHECK(batch[1].pixels.size() == 8 * 8 * 4);
    return 0;
}

static int frames_different_sized_trees_in_order()
{
    resetOptions();

    Octree large;
    large.initRoot(32, 5, 24);
    insertVoxel(large, 4, 0, 0, 0xFF0000FF);
    Octree small;
    small.initRoot(16, 4, 16);
    insertVoxel(small, 0, 0, 0, 0x00FF00FF);

    std::vector<CPUFrameViewport> viewports;
    viewports.push_back(makeViewport(&large, 6, 6, {4.0f, 0.0f, -10.0f},
    identityBasis(), {9, 9, 9, 255}));
    viewports.push_back(makeViewport(&small, 8, 4, {0.0f, 0.0f, -6.0f},
    identityBasis(), {9, 9, 9, 255}));

    const std::vector<CPUFrameResult> batch =
    cpuFrameRender(viewports, frameOptions(3, 2));
    TASK_CHECK(batch.size() == 2);
    TASK_CHECK(batch[0].width == 6 && batch[0].height == 6);
    TASK_CHECK(batch[1].width == 8 && batch[1].height == 4);
    TASK_CHECK(batch[0].pixels.size() == 6 * 6 * 4);
    TASK_CHECK(batch[1].pixels.size() == 8 * 4 * 4);

    std::vector<CPUFrameViewport> single;
    single.push_back(viewports[0]);
    const std::vector<CPUFrameResult> aloneLarge =
    cpuFrameRender(single, frameOptions(1, 5));
    single[0] = viewports[1];
    const std::vector<CPUFrameResult> aloneSmall =
    cpuFrameRender(single, frameOptions(1, 1));
    TASK_CHECK(sameResults(std::vector<CPUFrameResult>{batch[0]}, aloneLarge));
    TASK_CHECK(sameResults(std::vector<CPUFrameResult>{batch[1]}, aloneSmall));

    const UC* centerLarge = pixelAt(batch[0], 3, 3);
    TASK_CHECK(centerLarge[3] == 0xFF);
    const UC* centerSmall = pixelAt(batch[1], 4, 2);
    TASK_CHECK(centerSmall[3] == 0xFF);
    return 0;
}

static int frames_worker_and_tiling_determinism()
{
    resetOptions();

    Octree treeA;
    treeA.initRoot(16, 4, 16);
    insertVoxel(treeA, 0, 0, 0, 0xFF8040FF);
    insertVoxel(treeA, 3, 0, 0, 0x2040FFFF);
    insertVoxel(treeA, 0, 2, 0, 0xFFFF40FF);

    Octree treeB;
    treeB.initRoot(32, 5, 24);
    insertVoxel(treeB, 6, 0, 0, 0x00FF80FF);
    insertVoxel(treeB, 0, 0, 0, 0x8000FFFF);

    std::vector<CPUFrameViewport> viewports;
    viewports.push_back(makeViewport(&treeA, 8, 8, {0.0f, 0.0f, -6.0f},
    cameraBasis({0.0f, 1.0f, 0.0f}, 0.1f, {0.0f, 0.0f, -6.0f}),
    {5, 6, 7, 255}));
    viewports.push_back(makeViewport(&treeB, 10, 10, {0.0f, 0.0f, -8.0f},
    cameraBasis({1.0f, 0.0f, 0.0f}, 0.2f, {0.0f, 0.0f, -8.0f}),
    {5, 6, 7, 255}));
    viewports.push_back(makeViewport(&treeA, 8, 6, {0.0f, 0.0f, -6.0f},
    identityBasis(), {5, 6, 7, 255}));

    const std::vector<CPUFrameResult> reference =
    cpuFrameRender(viewports, frameOptions(1, 64));

    const int workerCounts[] = {1, 2, 4};
    const int tileRows[] = {1, 2, 3, 5};
    for(size_t w = 0; w < sizeof(workerCounts) / sizeof(workerCounts[0]); w++)
    {
        for(size_t r = 0; r < sizeof(tileRows) / sizeof(tileRows[0]); r++)
        {
            const std::vector<CPUFrameResult> candidate =
            cpuFrameRender(viewports,
            frameOptions(workerCounts[w], tileRows[r]));
            TASK_CHECK(sameResults(reference, candidate));
        }
    }

    // Two independent batches rendered in an interleaved fashion.
    resetOptions();
    Octree other;
    other.initRoot(16, 4, 16);
    insertVoxel(other, 0, 0, 0, 0x102030FF);
    std::vector<CPUFrameViewport> otherViewports;
    otherViewports.push_back(makeViewport(&other, 6, 6, {0.0f, 0.0f, -5.0f},
    identityBasis(), {1, 1, 1, 255}));
    const std::vector<CPUFrameResult> otherReference =
    cpuFrameRender(otherViewports, frameOptions(1, 1));

    const std::vector<CPUFrameResult> first =
    cpuFrameRender(viewports, frameOptions(2, 3));
    const std::vector<CPUFrameResult> second =
    cpuFrameRender(otherViewports, frameOptions(4, 2));
    const std::vector<CPUFrameResult> third =
    cpuFrameRender(viewports, frameOptions(4, 1));

    TASK_CHECK(sameResults(reference, first));
    TASK_CHECK(sameResults(otherReference, second));
    TASK_CHECK(sameResults(reference, third));
    return 0;
}

static int frames_validation_rejects_whole_batch()
{
    resetOptions();

    Octree tree;
    tree.initRoot(16, 4, 16);
    insertVoxel(tree, 0, 0, 0, 0xFFFFFFFF);

    const CPUFrameViewport good = makeViewport(&tree, 4, 4,
    {0.0f, 0.0f, -6.0f}, identityBasis());

    TASK_CHECK(throwsInvalidArgument([&]() {
        cpuFrameRender(std::vector<CPUFrameViewport>{good},
        frameOptions(0, 1));
    }));
    TASK_CHECK(throwsInvalidArgument([&]() {
        cpuFrameRender(std::vector<CPUFrameViewport>{good},
        frameOptions(33, 1));
    }));
    TASK_CHECK(throwsInvalidArgument([&]() {
        cpuFrameRender(std::vector<CPUFrameViewport>{good},
        frameOptions(1, 0));
    }));

    CPUFrameViewport bad = good;
    bad.octree = NULL;
    TASK_CHECK(throwsInvalidArgument([&]() {
        cpuFrameRender(std::vector<CPUFrameViewport>{good, bad},
        frameOptions(2, 1));
    }));

    bad = good;
    bad.width = 5;
    TASK_CHECK(throwsInvalidArgument([&]() {
        cpuFrameRender(std::vector<CPUFrameViewport>{good, bad},
        frameOptions(2, 1));
    }));

    bad = good;
    bad.height = 0;
    TASK_CHECK(throwsInvalidArgument([&]() {
        cpuFrameRender(std::vector<CPUFrameViewport>{good, bad},
        frameOptions(2, 1));
    }));

    bad = good;
    bad.pixelStep = 2;
    TASK_CHECK(throwsInvalidArgument([&]() {
        cpuFrameRender(std::vector<CPUFrameViewport>{good, bad},
        frameOptions(2, 1));
    }));

    bad = good;
    bad.perspectiveFactor = -1.0f;
    TASK_CHECK(throwsInvalidArgument([&]() {
        cpuFrameRender(std::vector<CPUFrameViewport>{good, bad},
        frameOptions(2, 1));
    }));

    bad = good;
    bad.zoom = 0.0f;
    TASK_CHECK(throwsInvalidArgument([&]() {
        cpuFrameRender(std::vector<CPUFrameViewport>{good, bad},
        frameOptions(2, 1));
    }));

    bad = good;
    bad.position.x = std::numeric_limits<float>::quiet_NaN();
    TASK_CHECK(throwsInvalidArgument([&]() {
        cpuFrameRender(std::vector<CPUFrameViewport>{good, bad},
        frameOptions(2, 1));
    }));

    bad = good;
    bad.perspectiveFactor = std::numeric_limits<float>::infinity();
    TASK_CHECK(throwsInvalidArgument([&]() {
        cpuFrameRender(std::vector<CPUFrameViewport>{good, bad},
        frameOptions(2, 1));
    }));

    bad = good;
    bad.transparentG = std::numeric_limits<float>::quiet_NaN();
    TASK_CHECK(throwsInvalidArgument([&]() {
        cpuFrameRender(std::vector<CPUFrameViewport>{good, bad},
        frameOptions(2, 1));
    }));

    // Zero length basis vector.
    bad = good;
    bad.basis.i = {0.0f, 0.0f, 0.0f};
    TASK_CHECK(throwsInvalidArgument([&]() {
        cpuFrameRender(std::vector<CPUFrameViewport>{good, bad},
        frameOptions(2, 1));
    }));

    // Non unit basis vector.
    bad = good;
    bad.basis.j = {0.0f, 2.0f, 0.0f};
    TASK_CHECK(throwsInvalidArgument([&]() {
        cpuFrameRender(std::vector<CPUFrameViewport>{good, bad},
        frameOptions(2, 1));
    }));

    // Non orthogonal basis vectors.
    bad = good;
    bad.basis.j = {1.0f, 0.0f, 0.0f};
    TASK_CHECK(throwsInvalidArgument([&]() {
        cpuFrameRender(std::vector<CPUFrameViewport>{good, bad},
        frameOptions(2, 1));
    }));

    // The failed call must not have started any work nor filled the stats.
    CPUFrameStats stats;
    stats.viewports = 999;
    stats.pixelQueries = 999;
    stats.workers = 999;
    TASK_CHECK(throwsInvalidArgument([&]() {
        cpuFrameRender(std::vector<CPUFrameViewport>{good, bad},
        frameOptions(2, 1), &stats);
    }));
    TASK_CHECK(stats.viewports == 999 && stats.pixelQueries == 999 &&
    stats.workers == 999);
    return 0;
}

static int frames_legacy_pixel_path_matches_batch()
{
    resetOptions();

    Octree tree;
    tree.initRoot(16, 4, 16);
    insertVoxel(tree, 0, 0, 0, 0xFF804080);
    insertVoxel(tree, 0, 0, 2, 0x2040FFC0);
    insertVoxel(tree, 2, 0, 0, 0xFFFF40FF);
    insertVoxel(tree, 0, 1, 0, 0x40FF80FF);

    const Vec3<float> position = {0.0f, 0.0f, -6.0f};
    Mat3f basis = cameraBasis({0.0f, 1.0f, 0.0f}, 0.12f, position);

    CPUFrameViewport viewport = makeViewport(&tree, 10, 8, position, basis,
    {3, 4, 5, 255});
    viewport.perspectiveFactor = 0.05f;
    viewport.zoom = 1.0f;
    viewport.background = {3, 4, 5, 255};

    CPUFrameOptions options = blendOptions(3, 3, 6, 1);
    const std::vector<CPUFrameResult> batch =
    cpuFrameRender(std::vector<CPUFrameViewport>{viewport}, options);

    CPUFramePixelConfig config;
    config.pixelStep = 1;
    config.perspectiveFactor = viewport.perspectiveFactor;
    config.zoom = viewport.zoom;
    config.background = viewport.background;
    config.transparentR = viewport.transparentR;
    config.transparentG = viewport.transparentG;
    config.transparentB = viewport.transparentB;
    config.transparentA = viewport.transparentA;
    config.maxVoxelsByRay = options.maxVoxelsByRay;
    config.transparency = options.transparency;
    config.minVoxelsProximity = options.minVoxelsProximity;
    config.avoidScanGlitches = options.avoidScanGlitches;
    config.maxRayDistance = tree.getRayLength();

    std::vector<UC> legacy((size_t)viewport.width * viewport.height * 4, 0);
    for(int y = 0; y < viewport.height; y++)
    {
        for(int x = 0; x < viewport.width; x++)
        {
            cpuFrameRenderPixel(tree, basis, position,
            x - viewport.width / 2, y - viewport.height / 2, config,
            &legacy[((size_t)y * viewport.width + x) * 4]);
        }
    }

    TASK_CHECK(batch[0].pixels == legacy);
    return 0;
}

static int frames_stats_and_empty_input()
{
    resetOptions();

    Octree tree;
    tree.initRoot(16, 4, 16);
    insertVoxel(tree, 0, 0, 0, 0xFFFFFFFF);

    std::vector<CPUFrameViewport> viewports;
    viewports.push_back(makeViewport(&tree, 6, 4, {0.0f, 0.0f, -6.0f},
    identityBasis()));
    viewports.push_back(makeViewport(&tree, 8, 2, {0.0f, 0.0f, -6.0f},
    identityBasis()));

    CPUFrameStats stats;
    const std::vector<CPUFrameResult> results =
    cpuFrameRender(viewports, frameOptions(3, 2), &stats);
    TASK_CHECK(results.size() == 2);
    TASK_CHECK(stats.viewports == 2);
    TASK_CHECK(stats.pixelQueries == 6 * 4 + 8 * 2);
    TASK_CHECK(stats.workers == 3);

    const std::vector<CPUFrameResult> empty =
    cpuFrameRender(std::vector<CPUFrameViewport>(), frameOptions(1, 1),
    &stats);
    TASK_CHECK(empty.empty());
    TASK_CHECK(stats.viewports == 0);
    TASK_CHECK(stats.pixelQueries == 0);
    TASK_CHECK(stats.workers == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    {"ownership", "interleaved_cameras", ownership_interleaved_cameras},
    {"ownership", "tree_sizes_and_bounds", ownership_tree_sizes_and_bounds},
    {"ownership", "accelerator_per_instance", ownership_accelerator_per_instance},
    {"ownership", "single_init_and_options_snapshot",
    ownership_single_init_and_options_snapshot},
    {"queries", "match_legacy_scalar", queries_match_legacy_scalar},
    {"queries", "transparent_opaque_and_filtering",
    queries_transparent_opaque_and_filtering},
    {"queries", "miss_and_accelerator_toggle", queries_miss_and_accelerator_toggle},
    {"queries", "input_immutability_and_copies",
    queries_input_immutability_and_copies},
    {"queries", "concurrent_same_tree", queries_concurrent_same_tree},
    {"queries", "color_hand_computed", queries_color_hand_computed},
    {"frames", "hand_computed_pixel_and_background",
    frames_hand_computed_pixel_and_background},
    {"frames", "transparent_blending", frames_transparent_blending},
    {"frames", "perspective_and_orthographic",
    frames_perspective_and_orthographic},
    {"frames", "two_cameras_same_tree", frames_two_cameras_same_tree},
    {"frames", "different_sized_trees_in_order",
    frames_different_sized_trees_in_order},
    {"frames", "worker_and_tiling_determinism",
    frames_worker_and_tiling_determinism},
    {"frames", "legacy_pixel_path_matches_batch",
    frames_legacy_pixel_path_matches_batch},
    {"frames", "validation_rejects_whole_batch",
    frames_validation_rejects_whole_batch},
    {"frames", "stats_and_empty_input", frames_stats_and_empty_input}
};

const size_t task_case_count =
sizeof(task_cases) / sizeof(task_cases[0]);
