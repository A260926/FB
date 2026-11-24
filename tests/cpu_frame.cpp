/*
 * Cloud of Voxels (CoV) project
 * Task tests for the GL-free CPU multi-viewport frame renderer and the
 * per-instance Octree / Camera ownership changes.
 *
 * This file has no main(): task_support/driver.c drives the registered cases.
 */

#include "headers/Octree.h"
#include "headers/Camera.h"
#include "headers/Options.h"
#include "headers/CPUFrame.h"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#include "task_support/check.h"

namespace
{
    constexpr float EPS = 1.0e-5f;

    bool sameFloat(const float a, const float b)
    {
        return fabsf(a - b) <= EPS;
    }

    bool sameVector(const Vec3<float>& a, const Vec3<float>& b)
    {
        return sameFloat(a.x, b.x) && sameFloat(a.y, b.y) &&
            sameFloat(a.z, b.z);
    }

    Mat3f identityBasis()
    {
        return Mat3f{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f}};
    }

    /*
    * Saves and restores the global Options while a case mutates them; the
    * new read-only API must not depend on these values.
    */
    struct OptionsGuard
    {
        bool transparency = Options::TRANSPARENCY;
        bool accelerated = Options::ACCELERATED;
        bool glitches = Options::AVOID_SCAN_GLITCHES;
        SUI proximity = Options::MIN_VOXELS_PROXIMITY;
        SUI maxByRay = Options::MAX_VOXELS_BY_RAY;

        OptionsGuard()
        {
            Options::nologs = true;
            Options::ACCELERATED = false;
            Options::SMOOTH_SIBLINGS = false;
            Options::VOXEL_SHELL_TYPE = 0;
        }

        ~OptionsGuard()
        {
            Options::TRANSPARENCY = transparency;
            Options::ACCELERATED = accelerated;
            Options::AVOID_SCAN_GLITCHES = glitches;
            Options::MIN_VOXELS_PROXIMITY = proximity;
            Options::MAX_VOXELS_BY_RAY = maxByRay;
        }
    };

    Octree* makeTree(const SUI size, const SUI depth, const SI rayLength,
        Voxel* const voxels, const UI count, const bool accelerated)
    {
        Octree* const tree = new Octree();
        tree->initRoot(size, depth, rayLength);
        tree->addVoxels(voxels, count);
        if(accelerated)
        {
            tree->initAccelerator();
        }
        return tree;
    }

    RayQuery makeQuery(const Vec3<float> origin, const Vec3<float> direction,
        const float maxDistance, const bool transparency,
        const SUI proximity = 0, const bool glitches = false)
    {
        RayQuery query;
        query.origin = origin;
        query.direction = direction;
        query.maxDistance = maxDistance;
        query.transparency = transparency;
        query.minVoxelProximity = proximity;
        query.avoidGlitches = glitches;
        query.useNormalizer = false;
        query.normalizer = {0.0f, 0.0f, 0.0f};
        return query;
    }

    /*
    * Legacy scalar reference: the old setRay / initBasis / rayTrace path,
    * which is allowed to mutate its own ray and the global Options.
    */
    std::vector<DynamicVoxel> legacyTrace(Octree& tree, const Mat3f& basis,
        const Vec3<float> origin, const float maxDistance)
    {
        Mat3f localBasis = basis;
        Vec3<float> ray = origin;
        tree.initBasis(&localBasis);
        tree.setRay(&ray);
        std::vector<DynamicVoxel> hits;
        tree.rayTrace(&hits);
        (void)maxDistance;
        return hits;
    }

    bool sameHits(const std::vector<RayHit>& a,
        const std::vector<RayHit>& b)
    {
        if(a.size() != b.size())
        {
            return false;
        }
        for(size_t i = 0; i < a.size(); ++i)
        {
            if(!sameFloat(a[i].depth, b[i].depth) || a[i].color != b[i].color)
            {
                return false;
            }
        }
        return true;
    }

    Vec3<float> referenceAxis(float matrix[16], const int column)
    {
        Vec3<float> axis = {matrix[column * 4 + 0], matrix[column * 4 + 1],
            matrix[column * 4 + 2]};
        math::normalize(&axis);
        return axis;
    }

    cpuframe::ViewportInput makeViewport(const Octree* const tree,
        const Mat3f& basis, const Vec3<float>& position,
        const unsigned int width, const unsigned int height)
    {
        cpuframe::ViewportInput v;
        v.tree = tree;
        v.basis = basis;
        v.position = position;
        v.width = width;
        v.height = height;
        v.perspectiveFactor = 0.0f;
        v.zoom = 1.0f;
        v.maxRayDistance = (float)tree->getRayLength();
        v.maxVoxelsByRay = 8;
        v.minVoxelProximity = 0;
        v.transparency = false;
        v.avoidGlitches = false;
        v.background = {0, 0, 0, 0};
        return v;
    }

    /*
    * Independent re-implementation of one frame using the legacy scalar ray
    * and the Core::process back-to-front blending formula.
    */
    std::vector<unsigned char> legacyFrame(const cpuframe::ViewportInput& v)
    {
        std::vector<unsigned char> pixels((size_t)4 * v.width * v.height, 0);
        Octree* const tree = const_cast<Octree*>(v.tree);
        const int halfW = (int)(v.width / 2);
        const int halfH = (int)(v.height / 2);
        Options::TRANSPARENCY = v.transparency;
        Options::AVOID_SCAN_GLITCHES = v.avoidGlitches;
        Options::MIN_VOXELS_PROXIMITY = v.minVoxelProximity;

        for(unsigned int py = 0; py < v.height; ++py)
        {
            const int curpixY = (int)py - halfH;
            for(unsigned int px = 0; px < v.width; ++px)
            {
                const int curpixX = (int)px - halfW;
                Mat3f basis = v.basis;
                Vec3<float> ray = math::vecxscl(basis.i, (float)curpixX);
                math::vecadd(math::vecxscl(basis.j, (float)curpixY), &ray);
                math::vecadd(v.position, &ray);
                tree->initBasis(&basis);
                tree->setRay(&ray);
                if(v.perspectiveFactor > 0.0f)
                {
                    tree->setPerspectiveRay(v.zoom * (float)curpixX,
                        v.zoom * (float)curpixY, v.perspectiveFactor);
                }
                std::vector<DynamicVoxel> voxels;
                tree->rayTrace(&voxels);

                unsigned char* const out =
                    pixels.data() + 4 * ((size_t)py * v.width + px);
                if(voxels.empty())
                {
                    out[0] = v.background.r;
                    out[1] = v.background.g;
                    out[2] = v.background.b;
                    out[3] = v.background.a;
                    continue;
                }

                unsigned int i = (unsigned int)voxels.size();
                unsigned int end = 0;
                if(i > v.maxVoxelsByRay)
                {
                    end = i - v.maxVoxelsByRay;
                }
                unsigned int ro = 0, go = 0, bo = 0, alpha = 0;
                while(i-- > end)
                {
                    const unsigned int color = voxels[i].voxel->color;
                    const unsigned int r = (color & 0xFF000000) >> 24;
                    const unsigned int g = (color & 0x00FF0000) >> 16;
                    const unsigned int b = (color & 0x0000FF00) >> 8;
                    unsigned int a = (color & 0x000000FF);
                    if(a > alpha)
                    {
                        alpha = a;
                    }
                    const float darkness = tree->getDarkness(voxels[i].depth);
                    const float af = (a / 255.0f) * darkness;
                    ro = (unsigned char)(r * af + (1.0f - af) * ro);
                    go = (unsigned char)(g * af + (1.0f - af) * go);
                    bo = (unsigned char)(b * af + (1.0f - af) * bo);
                }
                out[0] = (unsigned char)ro;
                out[1] = (unsigned char)go;
                out[2] = (unsigned char)bo;
                out[3] = 0xFF;
            }
        }
        return pixels;
    }
}

/*
 * ownership group
 */

static int case_camera_rotation_isolated(void)
{
    Camera isolated;
    isolated.rotate({0.0f, 1.0f, 0.0f}, 0.2f);
    isolated.rotate({0.0f, 1.0f, 0.0f}, 0.2f);

    Camera interleaved;
    interleaved.rotate({0.0f, 1.0f, 0.0f}, 0.2f);
    Camera other;
    other.rotate({1.0f, 0.0f, 0.0f}, 0.0f);
    interleaved.rotate({0.0f, 1.0f, 0.0f}, 0.2f);

    Mat3f basisA;
    Mat3f basisB;
    isolated.getBasis(&basisA);
    interleaved.getBasis(&basisB);
    TASK_CHECK(sameVector(basisA.k, basisB.k));
    TASK_CHECK(sameVector(basisA.i, basisB.i));
    TASK_CHECK(sameVector(basisA.j, basisB.j));

    /*
    * Independent matrix reference: R(0.2) * R(0.2), axes extracted and
    * normalized exactly like Camera::updateNearInfo.
    */
    float r1[16], r12[16];
    math::eulerorientation({0.0f, 1.0f, 0.0f}, 0.2f, r1);
    math::mat4xmat4(r1, r1, r12);
    TASK_CHECK(sameVector(basisA.i, referenceAxis(r12, 0)));
    TASK_CHECK(sameVector(basisA.j, referenceAxis(r12, 1)));
    TASK_CHECK(sameVector(basisA.k, referenceAxis(r12, 2)));

    Camera fresh;
    Mat3f basisFresh;
    fresh.getBasis(&basisFresh);
    TASK_CHECK(sameVector(basisFresh.k, {0.0f, 0.0f, 1.0f}));
    TASK_CHECK(fabsf(basisA.k.x - sinf(0.4f)) <= EPS);
    return 0;
}

static int case_tree_instance_ownership(void)
{
    OptionsGuard guard;
    Voxel largeVoxels[] = {{{0, 0, 0}, 0xF03020FF}, {{4, 0, 0}, 0x30F020FF}};
    Octree* const large = makeTree(32, 5, 24, largeVoxels, 2, false);
    TASK_CHECK((int)large->getSize() == 32);
    TASK_CHECK((int)large->getHalf() == 16);
    TASK_CHECK(large->getRayLength() == 24);
    TASK_CHECK(!large->isAccelerated());

    /*
    * Building the smaller tree afterwards must not retag or shrink the
    * large tree's bounds.
    */
    Voxel smallVoxels[] = {{{0, 0, 0}, 0x112233FF}};
    Octree* const small = makeTree(16, 4, 20, smallVoxels, 1, false);
    TASK_CHECK((int)small->getSize() == 16);
    TASK_CHECK((int)small->getHalf() == 8);
    TASK_CHECK((int)large->getSize() == 32);
    TASK_CHECK((int)large->getHalf() == 16);

    Voxel laterLarge = {{9, 0, 0}, 0x123456FF};
    large->addVoxels(&laterLarge, 1);
    Voxel rejectedBySmall = {{9, 0, 0}, 0xAABBCCFF};
    small->addVoxels(&rejectedBySmall, 1);

    const Vec3<float> direction = {0.0f, 0.0f, 1.0f};
    std::vector<RayHit> largeHits = large->traceRay(makeQuery(
        {9.0f, 0.0f, -6.0f}, direction, 24.0f, false));
    TASK_CHECK(largeHits.size() == 1);
    TASK_CHECK(largeHits[0].color == 0x123456FF);

    std::vector<RayHit> smallMiss = small->traceRay(makeQuery(
        {9.0f, 0.0f, -6.0f}, direction, 20.0f, false));
    TASK_CHECK(smallMiss.empty());
    std::vector<RayHit> smallHit = small->traceRay(makeQuery(
        {0.0f, 0.0f, -6.0f}, direction, 20.0f, false));
    TASK_CHECK(smallHit.size() == 1 && smallHit[0].color == 0x112233FF);

    bool threw = false;
    try
    {
        large->initRoot(32, 5, 24);
    }
    catch(const std::logic_error&)
    {
        threw = true;
    }
    TASK_CHECK(threw);

    delete small;
    /*
    * Destroying the small tree first leaves the large tree fully usable.
    */
    largeHits = large->traceRay(makeQuery(
        {9.0f, 0.0f, -6.0f}, direction, 24.0f, false));
    TASK_CHECK(largeHits.size() == 1);
    delete large;
    return 0;
}

static int case_accelerator_instance_ownership(void)
{
    OptionsGuard guard;
    Voxel voxels[] = {{{0, 0, 0}, 0x102030FF}, {{3, -2, 1}, 0x405060FF}};
    Octree* const plain = makeTree(16, 4, 20, voxels, 2, false);
    Octree* const fast = makeTree(16, 4, 20, voxels, 2, true);
    TASK_CHECK(!plain->isAccelerated());
    TASK_CHECK(fast->isAccelerated());

    const Vec3<float> direction = {0.0f, 0.0f, 1.0f};
    for(int x = -6; x <= 6; ++x)
    {
        for(int y = -6; y <= 6; ++y)
        {
            const std::vector<RayHit> a = plain->traceRay(makeQuery(
                {(float)x, (float)y, -8.0f}, direction, 20.0f, false));
            const std::vector<RayHit> b = fast->traceRay(makeQuery(
                {(float)x, (float)y, -8.0f}, direction, 20.0f, false));
            TASK_CHECK(sameHits(a, b));
        }
    }
    delete fast;
    const std::vector<RayHit> stillAlive = plain->traceRay(makeQuery(
        {0.0f, 0.0f, -8.0f}, direction, 20.0f, false));
    TASK_CHECK(stillAlive.size() == 1);
    delete plain;
    return 0;
}

static int case_new_api_ignores_options_mutation(void)
{
    OptionsGuard guard;
    Voxel voxels[] = {{{0, 0, 0}, 0x11223380}};
    Octree* const tree = makeTree(16, 4, 20, voxels, 1, false);
    const RayQuery query = makeQuery({0.0f, 0.0f, -6.0f},
        {0.0f, 0.0f, 1.0f}, 20.0f, true, 0);
    const std::vector<RayHit> before = tree->traceRay(query);

    Options::TRANSPARENCY = false;
    Options::AVOID_SCAN_GLITCHES = true;
    Options::MIN_VOXELS_PROXIMITY = 9;
    Options::ACCELERATED = true;
    const std::vector<RayHit> after = tree->traceRay(query);
    TASK_CHECK(sameHits(before, after));
    TASK_CHECK(after.size() == 1 && (after[0].color & 0xFF) == 0x80);
    delete tree;
    return 0;
}

/*
 * queries group
 */

static int case_readonly_matches_scalar_legacy(void)
{
    OptionsGuard guard;
    Voxel voxels[] = {
        {{0, 0, 0}, 0xA0B0C0FF},
        {{4, 0, 0}, 0x102030FF},
        {{-4, 2, 0}, 0x445566FF}
    };
    Octree* const tree = makeTree(32, 5, 24, voxels, 3, false);
    const Mat3f basis = identityBasis();
    const Vec3<float> direction = {0.0f, 0.0f, 1.0f};

    Options::TRANSPARENCY = false;
    for(int x = -8; x <= 8; x += 2)
    {
        for(int y = -4; y <= 4; y += 2)
        {
            const Vec3<float> origin = {(float)x, (float)y, -6.0f};
            const std::vector<DynamicVoxel> legacy =
                legacyTrace(*tree, basis, origin, 24.0f);
            const std::vector<RayHit> readonly = tree->traceRay(
                makeQuery(origin, direction, 24.0f, false));
            TASK_CHECK(legacy.size() == readonly.size());
            for(size_t i = 0; i < readonly.size(); ++i)
            {
                TASK_CHECK(sameFloat(legacy[i].depth, readonly[i].depth));
                TASK_CHECK(legacy[i].voxel->color == readonly[i].color);
            }
        }
    }
    delete tree;
    return 0;
}

static int case_hand_computed_colored_voxels(void)
{
    OptionsGuard guard;
    Voxel voxels[] = {{{0, 0, 0}, 0x112233FF}};
    Octree* const tree = makeTree(16, 4, 20, voxels, 1, false);

    std::vector<RayHit> hit = tree->traceRay(makeQuery(
        {0.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 20.0f, false));
    TASK_CHECK(hit.size() == 1);
    TASK_CHECK(hit[0].color == 0x112233FF);
    TASK_CHECK(hit[0].depth >= 0.0f);

    /*
    * A ray offset by one unit must miss the single voxel: hand-checkable.
    */
    std::vector<RayHit> miss = tree->traceRay(makeQuery(
        {1.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 20.0f, false));
    TASK_CHECK(miss.empty());

    /*
    * Opaque mode stops at the first voxel even when more lie on the ray.
    */
    Voxel stack[] = {
        {{0, 0, 0}, 0x111111FF},
        {{0, 0, 2}, 0x222222FF},
        {{0, 0, 4}, 0x333333FF}
    };
    Octree* const stacked = makeTree(16, 4, 20, stack, 3, false);
    std::vector<RayHit> opaque = stacked->traceRay(makeQuery(
        {0.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 1.0f}, 20.0f, false));
    TASK_CHECK(opaque.size() == 1);
    TASK_CHECK(opaque[0].color == 0x111111FF);
    delete stacked;
    delete tree;
    return 0;
}

static int case_transparent_layer_filtering(void)
{
    OptionsGuard guard;
    Voxel stack[] = {
        {{0, 0, 0}, 0x11111180},
        {{0, 0, 1}, 0x22222280},
        {{0, 0, 2}, 0x33333380},
        {{0, 0, 5}, 0x44444480}
    };
    Octree* const tree = makeTree(16, 4, 20, stack, 4, false);
    const Vec3<float> origin = {0.0f, 0.0f, -6.0f};
    const Vec3<float> direction = {0.0f, 0.0f, 1.0f};

    std::vector<RayHit> all = tree->traceRay(
        makeQuery(origin, direction, 20.0f, true, 0));
    TASK_CHECK(all.size() == 4);
    for(size_t i = 1; i < all.size(); ++i)
    {
        TASK_CHECK(all[i].depth > all[i - 1].depth);
    }

    /*
    * Minimum layer spacing drops layers closer than three depth units.
    */
    std::vector<RayHit> spaced = tree->traceRay(
        makeQuery(origin, direction, 20.0f, true, 3));
    TASK_CHECK(spaced.size() >= 2 && spaced.size() < all.size());
    TASK_CHECK(spaced[0].color == 0x11111180);
    delete tree;
    return 0;
}

static int case_perspective_matches_legacy(void)
{
    OptionsGuard guard;
    Voxel voxels[] = {
        {{0, 0, 0}, 0x708090FF},
        {{2, 2, 0}, 0xA0B0C080}
    };
    Octree* const tree = makeTree(32, 5, 24, voxels, 2, false);
    Options::TRANSPARENCY = true;
    Options::MIN_VOXELS_PROXIMITY = 0;
    const Mat3f basis = identityBasis();
    const float perspective = 0.01f;
    const float zoom = 0.5f;

    for(int curx = -4; curx <= 4; curx += 2)
    {
        for(int cury = -4; cury <= 4; cury += 2)
        {
            Vec3<float> origin = {(float)curx, (float)cury, -10.0f};
            Mat3f legacyBasis = basis;
            Vec3<float> legacyRay = origin;
            tree->initBasis(&legacyBasis);
            tree->setRay(&legacyRay);
            tree->setPerspectiveRay(zoom * (float)curx,
                zoom * (float)cury, perspective);
            std::vector<DynamicVoxel> legacy;
            tree->rayTrace(&legacy);

            RayQuery query = makeQuery(origin, basis.k, 24.0f, true, 0);
            const Vec3<float> bent = {
                basis.i.x * zoom * curx + basis.j.x * zoom * cury,
                basis.i.y * zoom * curx + basis.j.y * zoom * cury,
                basis.i.z * zoom * curx + basis.j.z * zoom * cury
            };
            query.direction.x = basis.k.x + bent.x * perspective;
            query.direction.y = basis.k.y + bent.y * perspective;
            query.direction.z = basis.k.z + bent.z * perspective;
            const float norm = sqrtf(query.direction.x * query.direction.x +
                query.direction.y * query.direction.y +
                query.direction.z * query.direction.z);
            query.direction.x /= norm;
            query.direction.y /= norm;
            query.direction.z /= norm;
            query.useNormalizer = true;
            query.normalizer = {1000.0f, 1000.0f, 1.0f};

            const std::vector<RayHit> readonly = tree->traceRay(query);
            TASK_CHECK(legacy.size() == readonly.size());
            for(size_t i = 0; i < readonly.size(); ++i)
            {
                TASK_CHECK(sameFloat(legacy[i].depth, readonly[i].depth));
                TASK_CHECK(legacy[i].voxel->color == readonly[i].color);
            }
        }
    }
    delete tree;
    return 0;
}

static int case_caller_inputs_untouched_and_repeatable(void)
{
    OptionsGuard guard;
    Voxel voxels[] = {{{0, 0, 0}, 0x556677FF}};
    Octree* const tree = makeTree(16, 4, 20, voxels, 1, false);
    RayQuery query = makeQuery({0.0f, 0.0f, -6.0f},
        {0.0f, 0.0f, 1.0f}, 20.0f, false);
    const RayQuery snapshot = query;
    const std::vector<RayHit> first = tree->traceRay(query);
    TASK_CHECK(sameVector(query.origin, snapshot.origin));
    TASK_CHECK(sameVector(query.direction, snapshot.direction));
    TASK_CHECK(query.maxDistance == snapshot.maxDistance);
    const std::vector<RayHit> second = tree->traceRay(query);
    TASK_CHECK(sameHits(first, second));
    delete tree;
    return 0;
}

static int case_avoid_glitches_matches_legacy(void)
{
    OptionsGuard guard;
    Voxel voxels[] = {
        {{0, 0, 0}, 0x123456FF},
        {{2, 1, 0}, 0x654321FF},
        {{-1, 2, 1}, 0xABCDEFFF}
    };
    Octree* const tree = makeTree(32, 5, 24, voxels, 3, false);
    const Mat3f basis = identityBasis();
    Options::TRANSPARENCY = false;
    Options::AVOID_SCAN_GLITCHES = true;

    for(int x = -5; x <= 5; ++x)
    {
        for(int y = -5; y <= 5; ++y)
        {
            const Vec3<float> origin = {(float)x, (float)y, -8.0f};
            const std::vector<DynamicVoxel> legacy =
                legacyTrace(*tree, basis, origin, 24.0f);
            const std::vector<RayHit> readonly = tree->traceRay(
                makeQuery(origin, {0.0f, 0.0f, 1.0f}, 24.0f,
                false, 0, true));
            TASK_CHECK(legacy.size() == readonly.size());
            for(size_t i = 0; i < readonly.size(); ++i)
            {
                TASK_CHECK(sameFloat(legacy[i].depth, readonly[i].depth));
                TASK_CHECK(legacy[i].voxel->color == readonly[i].color);
            }
        }
    }
    delete tree;
    return 0;
}

static int case_concurrent_readonly_queries(void)
{
    OptionsGuard guard;
    Voxel voxels[] = {
        {{0, 0, 0}, 0x123456FF},
        {{3, 3, 0}, 0x65432180},
        {{-2, 1, 1}, 0xABCDEF80}
    };
    Octree* const tree = makeTree(16, 4, 20, voxels, 3, true);
    const Vec3<float> direction = {0.0f, 0.0f, 1.0f};

    std::vector<RayHit> reference[9];
    for(int x = -4; x <= 4; ++x)
    {
        reference[x + 4] = tree->traceRay(makeQuery(
            {(float)x, 0.0f, -8.0f}, direction, 20.0f, true, 1));
    }

    constexpr unsigned int THREADS = 8;
    std::vector<std::thread> pool;
    std::vector<int> results(THREADS, 1);
    for(unsigned int t = 0; t < THREADS; ++t)
    {
        pool.emplace_back([&, t]()
        {
            for(int round = 0; round < 50; ++round)
            {
                for(int x = -4; x <= 4; ++x)
                {
                    const std::vector<RayHit> seen = tree->traceRay(
                        makeQuery({(float)x, 0.0f, -8.0f}, direction,
                        20.0f, true, 1));
                    if(!sameHits(seen, reference[x + 4]))
                    {
                        results[t] = 0;
                    }
                }
            }
        });
    }
    for(std::thread& thread : pool)
    {
        thread.join();
    }
    for(int result : results)
    {
        TASK_CHECK(result == 1);
    }
    TASK_CHECK(tree->isAccelerated());
    delete tree;
    return 0;
}

/*
 * frames group
 */

static int case_batch_validation(void)
{
    OptionsGuard guard;
    Voxel voxels[] = {{{0, 0, 0}, 0x112233FF}};
    Octree* const tree = makeTree(16, 4, 20, voxels, 1, false);
    cpuframe::ViewportInput good = makeViewport(tree, identityBasis(),
        {0.0f, 0.0f, -8.0f}, 8, 8);

    auto rejects = [&](cpuframe::ViewportInput v,
        const unsigned int workers, const unsigned int blockSize)
    {
        try
        {
            cpuframe::renderBatch(std::vector<cpuframe::ViewportInput>{v},
                workers, blockSize);
        }
        catch(const std::invalid_argument&)
        {
            return true;
        }
        return false;
    };

    cpuframe::ViewportInput bad = good;
    bad.tree = NULL;
    TASK_CHECK(rejects(bad, 2, 2));
    bad = good; bad.width = 7;
    TASK_CHECK(rejects(bad, 2, 2));
    bad = good; bad.height = 0;
    TASK_CHECK(rejects(bad, 2, 2));
    bad = good; bad.zoom = 0.0f;
    TASK_CHECK(rejects(bad, 2, 2));
    bad = good; bad.perspectiveFactor = -0.1f;
    TASK_CHECK(rejects(bad, 2, 2));
    bad = good; bad.maxRayDistance = 0.0f;
    TASK_CHECK(rejects(bad, 2, 2));
    bad = good; bad.maxVoxelsByRay = 0;
    TASK_CHECK(rejects(bad, 2, 2));
    bad = good; bad.basis.k.x = std::nanf("");
    TASK_CHECK(rejects(bad, 2, 2));
    bad = good; bad.basis.i.x = 2.0f;
    TASK_CHECK(rejects(bad, 2, 2));
    bad = good; bad.basis.j.x = 0.5f;
    TASK_CHECK(rejects(bad, 2, 2));
    bad = good; bad.position.y = INFINITY;
    TASK_CHECK(rejects(bad, 2, 2));

    TASK_CHECK(rejects(good, 0, 2));
    TASK_CHECK(rejects(good, 33, 2));
    TASK_CHECK(rejects(good, 2, 0));

    /*
    * A bad viewport among good ones must throw and produce no frame.
    */
    std::vector<cpuframe::ViewportInput> mixed = {good, good};
    mixed[1].width = 3;
    bool threw = false;
    try
    {
        cpuframe::renderBatch(mixed, 4, 2);
    }
    catch(const std::invalid_argument&)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    delete tree;
    return 0;
}

static int case_frame_determinism_and_stats(void)
{
    OptionsGuard guard;
    Voxel voxels[] = {
        {{0, 0, 0}, 0xFF0000FF},
        {{2, 0, 0}, 0x00FF00FF},
        {{-2, 1, 0}, 0x0000FF80},
        {{1, -1, 1}, 0xFFFF0080}
    };
    Octree* const tree = makeTree(32, 5, 24, voxels, 4, false);
    cpuframe::ViewportInput viewport = makeViewport(tree, identityBasis(),
        {0.0f, 0.0f, -10.0f}, 16, 12);
    viewport.transparency = true;
    viewport.maxVoxelsByRay = 4;

    const cpuframe::BatchResult one = cpuframe::renderBatch(
        std::vector<cpuframe::ViewportInput>{viewport}, 1, 3,
        cpuframe::TaskOrder::Sequential);
    const cpuframe::BatchResult two = cpuframe::renderBatch(
        std::vector<cpuframe::ViewportInput>{viewport}, 2, 1,
        cpuframe::TaskOrder::Interleaved);
    const cpuframe::BatchResult four = cpuframe::renderBatch(
        std::vector<cpuframe::ViewportInput>{viewport}, 4, 5,
        cpuframe::TaskOrder::Sequential);

    TASK_CHECK(one.frames.size() == 1 && two.frames.size() == 1 &&
        four.frames.size() == 1);
    TASK_CHECK(one.frames[0].rgba == two.frames[0].rgba);
    TASK_CHECK(one.frames[0].rgba == four.frames[0].rgba);
    TASK_CHECK(one.stats.pixelQueries == 16ULL * 12ULL);
    TASK_CHECK(two.stats.pixelQueries == 16ULL * 12ULL);
    TASK_CHECK(four.stats.pixelQueries == 16ULL * 12ULL);
    TASK_CHECK(one.stats.viewportsCompleted == 1);
    TASK_CHECK(two.stats.viewportsCompleted == 1);
    TASK_CHECK(four.stats.viewportsCompleted == 1);
    TASK_CHECK(one.stats.workerThreads == 1);
    TASK_CHECK(two.stats.workerThreads == 2);
    TASK_CHECK(four.stats.workerThreads == 4);
    TASK_CHECK(one.frames[0].rgba.size() == (size_t)4 * 16 * 12);
    delete tree;
    return 0;
}

static int case_multi_viewport_ownership_and_legacy_match(void)
{
    OptionsGuard guard;
    Voxel largeVoxels[] = {
        {{0, 0, 0}, 0x102030FF},
        {{3, 2, 0}, 0x40506080}
    };
    Octree* const large = makeTree(32, 5, 24, largeVoxels, 2, false);
    Voxel smallVoxels[] = {{{0, 0, 0}, 0x708090FF}};
    Octree* const small = makeTree(16, 4, 20, smallVoxels, 1, true);

    Camera cameraA;
    cameraA.rotate({0.0f, 1.0f, 0.0f}, 0.1f);
    Mat3f basisA;
    cameraA.getBasis(&basisA);
    Camera cameraB;
    cameraB.rotate({1.0f, 0.0f, 0.0f}, 0.15f);
    Mat3f basisB;
    cameraB.getBasis(&basisB);

    cpuframe::ViewportInput first = makeViewport(large, basisA,
        {0.0f, 0.0f, -12.0f}, 16, 16);
    first.transparency = true;
    cpuframe::ViewportInput second = makeViewport(large, basisB,
        {0.0f, 0.0f, -12.0f}, 16, 16);
    cpuframe::ViewportInput third = makeViewport(small, identityBasis(),
        {0.0f, 0.0f, -8.0f}, 8, 8);
    third.background = {9, 8, 7, 255};

    const std::vector<cpuframe::ViewportInput> inputs = {first, second, third};
    const cpuframe::BatchResult batch = cpuframe::renderBatch(inputs, 4, 2,
        cpuframe::TaskOrder::Interleaved);
    TASK_CHECK(batch.frames.size() == 3);
    TASK_CHECK(batch.stats.viewportsCompleted == 3);
    TASK_CHECK(batch.stats.workerThreads == 4);
    TASK_CHECK(batch.stats.pixelQueries ==
        16ULL * 16ULL + 16ULL * 16ULL + 8ULL * 8ULL);

    /*
    * Input order is preserved and output storage is never shared.
    */
    TASK_CHECK(batch.frames[0].tree == large);
    TASK_CHECK(batch.frames[1].tree == large);
    TASK_CHECK(batch.frames[2].tree == small);
    TASK_CHECK(batch.frames[0].rgba.data() != batch.frames[1].rgba.data());
    TASK_CHECK(batch.frames[1].rgba.data() != batch.frames[2].rgba.data());

    const std::vector<unsigned char> expected0 = legacyFrame(first);
    const std::vector<unsigned char> expected1 = legacyFrame(second);
    const std::vector<unsigned char> expected2 = legacyFrame(third);
    TASK_CHECK(batch.frames[0].rgba == expected0);
    TASK_CHECK(batch.frames[1].rgba == expected1);
    TASK_CHECK(batch.frames[2].rgba == expected2);

    /*
    * The fully missing background color is emitted on every miss pixel.
    */
    cpuframe::ViewportInput emptyView = makeViewport(small, identityBasis(),
        {0.0f, 0.0f, -8.0f}, 8, 8);
    emptyView.background = {10, 20, 30, 255};
    Octree empty;
    empty.initRoot(16, 4, 20);
    emptyView.tree = &empty;
    const cpuframe::BatchResult emptyBatch = cpuframe::renderBatch(
        std::vector<cpuframe::ViewportInput>{emptyView}, 2, 4);
    static const unsigned char expectedBg[4] = {10, 20, 30, 255};
    for(size_t p = 0; p < emptyBatch.frames[0].rgba.size(); p += 4)
    {
        TASK_CHECK(emptyBatch.frames[0].rgba[p + 0] == expectedBg[0]);
        TASK_CHECK(emptyBatch.frames[0].rgba[p + 1] == expectedBg[1]);
        TASK_CHECK(emptyBatch.frames[0].rgba[p + 2] == expectedBg[2]);
        TASK_CHECK(emptyBatch.frames[0].rgba[p + 3] == expectedBg[3]);
    }
    delete small;
    delete large;
    return 0;
}

static int case_two_independent_batches_interleaved(void)
{
    OptionsGuard guard;
    Voxel voxelsA[] = {{{0, 0, 0}, 0xAA0000FF}, {{1, 1, 0}, 0x00AA0080}};
    Octree* const treeA = makeTree(32, 5, 24, voxelsA, 2, false);
    Voxel voxelsB[] = {{{0, 0, 0}, 0x0000AAFF}};
    Octree* const treeB = makeTree(16, 4, 20, voxelsB, 1, true);

    cpuframe::ViewportInput viewA = makeViewport(treeA, identityBasis(),
        {0.0f, 0.0f, -10.0f}, 16, 16);
    viewA.transparency = true;
    cpuframe::ViewportInput viewB = makeViewport(treeB, identityBasis(),
        {0.0f, 0.0f, -8.0f}, 12, 10);
    viewB.perspectiveFactor = 0.01f;

    const std::vector<cpuframe::ViewportInput> batchAInputs = {viewA};
    const std::vector<cpuframe::ViewportInput> batchBInputs = {viewB};
    const cpuframe::BatchResult serialA = cpuframe::renderBatch(
        batchAInputs, 4, 2);
    const cpuframe::BatchResult serialB = cpuframe::renderBatch(
        batchBInputs, 4, 3, cpuframe::TaskOrder::Interleaved);

    cpuframe::BatchResult concurrentA;
    cpuframe::BatchResult concurrentB;
    std::thread runnerA([&]()
    {
        concurrentA = cpuframe::renderBatch(batchAInputs, 4, 2);
    });
    std::thread runnerB([&]()
    {
        concurrentB = cpuframe::renderBatch(batchBInputs, 4, 3,
            cpuframe::TaskOrder::Interleaved);
    });
    runnerA.join();
    runnerB.join();

    TASK_CHECK(concurrentA.frames[0].rgba == serialA.frames[0].rgba);
    TASK_CHECK(concurrentB.frames[0].rgba == serialB.frames[0].rgba);
    TASK_CHECK(concurrentA.stats.pixelQueries == 16ULL * 16ULL);
    TASK_CHECK(concurrentB.stats.pixelQueries == 12ULL * 10ULL);
    delete treeB;
    delete treeA;
    return 0;
}

const task_case task_cases[] = {
    {"ownership", "camera_rotation_isolated", case_camera_rotation_isolated},
    {"ownership", "tree_instance_ownership", case_tree_instance_ownership},
    {"ownership", "accelerator_instance_ownership",
        case_accelerator_instance_ownership},
    {"ownership", "new_api_ignores_options_mutation",
        case_new_api_ignores_options_mutation},

    {"queries", "readonly_matches_scalar_legacy",
        case_readonly_matches_scalar_legacy},
    {"queries", "hand_computed_colored_voxels",
        case_hand_computed_colored_voxels},
    {"queries", "transparent_layer_filtering",
        case_transparent_layer_filtering},
    {"queries", "perspective_matches_legacy",
        case_perspective_matches_legacy},
    {"queries", "caller_inputs_untouched_and_repeatable",
        case_caller_inputs_untouched_and_repeatable},
    {"queries", "avoid_glitches_matches_legacy",
        case_avoid_glitches_matches_legacy},
    {"queries", "concurrent_readonly_queries",
        case_concurrent_readonly_queries},

    {"frames", "batch_validation", case_batch_validation},
    {"frames", "frame_determinism_and_stats",
        case_frame_determinism_and_stats},
    {"frames", "multi_viewport_ownership_and_legacy_match",
        case_multi_viewport_ownership_and_legacy_match},
    {"frames", "two_independent_batches_interleaved",
        case_two_independent_batches_interleaved}
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
