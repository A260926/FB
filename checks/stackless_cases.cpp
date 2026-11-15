// Stackless BVH traversal regression tests.
//
// This file is compiled together with task_support/driver.c (which provides main) and must not
// define its own entry point. Cases are registered in the four groups required by the driver:
// ordering, topology, storage and render. The stackless query path is checked against an
// independent brute-force triangle loop and against the legacy stack-based traversal.

// task_support/driver.c is compiled as C++ by the harness, so the support header must be
// included with C++ linkage (it has no extern "C" guard of its own).
#include "task_support/check.h"

#include <bvh/v2/bvh.h>
#include <bvh/v2/node.h>
#include <bvh/v2/vec.h>
#include <bvh/v2/ray.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/bbox.h>
#include <bvh/v2/stack.h>
#include <bvh/v2/stream.h>
#include <bvh/v2/default_builder.h>
#include <bvh/v2/sweep_sah_builder.h>
#include <bvh/v2/binned_sah_builder.h>
#include <bvh/v2/mini_tree_builder.h>
#include <bvh/v2/reinsertion_optimizer.h>
#include <bvh/v2/thread_pool.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// -----------------------------------------------------------------------------------------------
// Heap allocation accounting. The library is header-only, so overriding the global allocation
// functions in this translation unit counts every allocation made by the code under test.
// -----------------------------------------------------------------------------------------------

std::atomic<long> g_alloc_blocks{0};
std::atomic<long> g_free_blocks{0};
std::atomic<long> g_alloc_bytes{0};

} // namespace

void* operator new(std::size_t n) {
    if (void* p = std::malloc(n)) {
        g_alloc_blocks.fetch_add(1, std::memory_order_relaxed);
        g_alloc_bytes.fetch_add(static_cast<long>(n), std::memory_order_relaxed);
        return p;
    }
    throw std::bad_alloc{};
}

void* operator new[](std::size_t n) {
    if (void* p = std::malloc(n)) {
        g_alloc_blocks.fetch_add(1, std::memory_order_relaxed);
        g_alloc_bytes.fetch_add(static_cast<long>(n), std::memory_order_relaxed);
        return p;
    }
    throw std::bad_alloc{};
}

void* operator new(std::size_t n, std::align_val_t a) {
    if (void* p = std::aligned_alloc(static_cast<std::size_t>(a),
            ((n + static_cast<std::size_t>(a) - 1) / static_cast<std::size_t>(a)) *
                static_cast<std::size_t>(a))) {
        g_alloc_blocks.fetch_add(1, std::memory_order_relaxed);
        g_alloc_bytes.fetch_add(static_cast<long>(n), std::memory_order_relaxed);
        return p;
    }
    throw std::bad_alloc{};
}

void* operator new[](std::size_t n, std::align_val_t a) {
    if (void* p = std::aligned_alloc(static_cast<std::size_t>(a),
            ((n + static_cast<std::size_t>(a) - 1) / static_cast<std::size_t>(a)) *
                static_cast<std::size_t>(a))) {
        g_alloc_blocks.fetch_add(1, std::memory_order_relaxed);
        g_alloc_bytes.fetch_add(static_cast<long>(n), std::memory_order_relaxed);
        return p;
    }
    throw std::bad_alloc{};
}

void operator delete(void* p) noexcept {
    if (p) {
        g_free_blocks.fetch_add(1, std::memory_order_relaxed);
        std::free(p);
    }
}

void operator delete[](void* p) noexcept {
    if (p) {
        g_free_blocks.fetch_add(1, std::memory_order_relaxed);
        std::free(p);
    }
}

void operator delete(void* p, std::size_t) noexcept {
    if (p) {
        g_free_blocks.fetch_add(1, std::memory_order_relaxed);
        std::free(p);
    }
}

void operator delete[](void* p, std::size_t) noexcept {
    if (p) {
        g_free_blocks.fetch_add(1, std::memory_order_relaxed);
        std::free(p);
    }
}

void operator delete(void* p, std::align_val_t) noexcept {
    if (p) {
        g_free_blocks.fetch_add(1, std::memory_order_relaxed);
        std::free(p);
    }
}

void operator delete[](void* p, std::align_val_t) noexcept {
    if (p) {
        g_free_blocks.fetch_add(1, std::memory_order_relaxed);
        std::free(p);
    }
}

void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    if (p) {
        g_free_blocks.fetch_add(1, std::memory_order_relaxed);
        std::free(p);
    }
}

void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    if (p) {
        g_free_blocks.fetch_add(1, std::memory_order_relaxed);
        std::free(p);
    }
}

namespace {

using std::size_t;

template <typename T>
using V3 = bvh::v2::Vec<T, 3>;
template <typename T>
using N3 = bvh::v2::Node<T, 3>;
template <typename T>
using B3 = bvh::v2::Bvh<N3<T>>;
template <typename T>
using R3 = bvh::v2::Ray<T, 3>;
template <typename T>
using Tri3 = bvh::v2::Tri<T, 3>;
template <typename T>
using Box3 = bvh::v2::BBox<T, 3>;
template <typename T>
using PTri = bvh::v2::PrecomputedTri<T>;

struct AllocSnapshot {
    long blocks;
    long frees;
    long bytes;
};

static AllocSnapshot alloc_snapshot() {
    return {
        g_alloc_blocks.load(std::memory_order_relaxed),
        g_free_blocks.load(std::memory_order_relaxed),
        g_alloc_bytes.load(std::memory_order_relaxed)
    };
}

static bool alloc_unchanged(const AllocSnapshot& s) {
    auto now = alloc_snapshot();
    return now.blocks == s.blocks && now.frees == s.frees && now.bytes == s.bytes;
}

template <typename T>
struct BruteHit {
    bool hit = false;
    T t = std::numeric_limits<T>::max();
    size_t prim = std::numeric_limits<size_t>::max();
};

// Independent world-space triangle enumeration. When `allowed` is non-null, only primitive ids
// whose flag is set participate (used to restrict the reference to a non-root subtree).
template <typename T>
static BruteHit<T> brute_closest(
    const std::vector<Tri3<T>>& tris,
    R3<T> ray,
    const std::vector<char>* allowed = nullptr)
{
    BruteHit<T> result;
    for (size_t i = 0; i < tris.size(); ++i) {
        if (allowed && !(*allowed)[i])
            continue;
        if (auto h = PTri<T>(tris[i]).intersect(ray)) {
            ray.tmax = std::get<0>(*h);
            result.prim = i;
        }
    }
    result.hit = result.prim != std::numeric_limits<size_t>::max();
    if (result.hit)
        result.t = ray.tmax;
    return result;
}

template <typename T>
static bool brute_any(
    const std::vector<Tri3<T>>& tris,
    const R3<T>& ray,
    const std::vector<char>* allowed = nullptr)
{
    for (size_t i = 0; i < tris.size(); ++i) {
        if (allowed && !(*allowed)[i])
            continue;
        if (PTri<T>(tris[i]).intersect(ray))
            return true;
    }
    return false;
}

template <typename T>
static T tolerance(T t) {
    constexpr T eps = std::is_same_v<T, float> ? T(1e-6) : T(1e-12);
    return eps * std::max(T(1), std::abs(t));
}

template <typename T>
static bool same_hit(const BruteHit<T>& a, const BruteHit<T>& b) {
    if (a.hit != b.hit)
        return false;
    if (!a.hit)
        return true;
    return a.prim == b.prim && std::abs(a.t - b.t) <= tolerance(a.t);
}

// Builds a scene of non-overlapping triangles in distinct depth planes, so a ray never produces
// two equidistant hits (the legacy stack path does not commit to a tie-breaking rule).
template <typename T>
struct Scene {
    std::vector<Tri3<T>> tris;
    std::vector<Box3<T>> boxes;
    std::vector<V3<T>> centers;
    B3<T> bvh;

    static Scene make(size_t count) {
        Scene s;
        s.tris.reserve(count);
        s.boxes.reserve(count);
        s.centers.reserve(count);
        const T x_spacing = T(1.7);
        const T x_origin = T(-0.85) * T(count);
        for (size_t i = 0; i < count; ++i) {
            T x = x_origin + T(i) * x_spacing;
            T z = T(4) + T(0.37) * T(i);
            s.tris.emplace_back(
                V3<T>(x, T(-1), z),
                V3<T>(x + T(1), T(-1), z),
                V3<T>(x, T(1), z));
            s.boxes.push_back(s.tris.back().get_bbox());
            s.centers.push_back(s.tris.back().get_center());
        }
        return s;
    }

    void build_default() { bvh = bvh::v2::DefaultBuilder<N3<T>>::build(boxes, centers); }
    void build_sweep() { bvh = bvh::v2::SweepSahBuilder<N3<T>>::build(boxes, centers); }
    void build_binned() {
        typename bvh::v2::BinnedSahBuilder<N3<T>>::Config config;
        bvh = bvh::v2::BinnedSahBuilder<N3<T>>::build(boxes, centers, config);
    }

    V3<T> target(size_t i) const {
        const T x_origin = T(-0.85) * T(tris.size());
        return V3<T>(x_origin + T(i) * T(1.7) + T(0.3), T(0), T(4) + T(0.37) * T(i));
    }
};

template <typename T>
static R3<T> aim(const V3<T>& target, const V3<T>& dir, T dist = T(6), T tmax = T(1e30)) {
    return R3<T>(target - dir * dist, dir, T(0), tmax);
}

// Maps every primitive slot of a BVH to the id of the leaf node that owns it.
template <typename T>
static std::vector<size_t> slot_owner(const B3<T>& bvh) {
    std::vector<size_t> owner(bvh.prim_ids.size(), std::numeric_limits<size_t>::max());
    for (size_t node_id = 0; node_id < bvh.nodes.size(); ++node_id) {
        const auto& node = bvh.nodes[node_id];
        if (!node.is_leaf())
            continue;
        for (size_t s = node.index.first_id(), e = s + node.index.prim_count(); s < e; ++s)
            owner[s] = node_id;
    }
    return owner;
}

// Explicit (test-only, may use a stack) walk of the nodes below a given root.
template <typename T>
static std::vector<size_t> subtree_node_ids(const B3<T>& bvh, size_t root) {
    std::vector<size_t> result;
    std::vector<size_t> pending{root};
    while (!pending.empty()) {
        size_t id = pending.back();
        pending.pop_back();
        result.push_back(id);
        const auto& node = bvh.nodes[id];
        if (!node.is_leaf()) {
            pending.push_back(node.index.first_id() + 1);
            pending.push_back(node.index.first_id() + 0);
        }
    }
    return result;
}

// Primitive-id mask of every leaf below the given subtree root.
template <typename T>
static std::vector<char> subtree_prim_mask(const B3<T>& bvh, size_t root, size_t prim_count) {
    std::vector<char> mask(prim_count, false);
    for (size_t id : subtree_node_ids(bvh, root)) {
        const auto& node = bvh.nodes[id];
        if (!node.is_leaf())
            continue;
        for (size_t s = node.index.first_id(), e = s + node.index.prim_count(); s < e; ++s)
            mask[bvh.prim_ids[s]] = true;
    }
    return mask;
}

template <typename T>
static bool check_parent_table(const B3<T>& bvh) {
    if (bvh.parent_ids.size() != bvh.nodes.size())
        return false;
    if (bvh.nodes.empty())
        return bvh.parent_ids.empty();
    if (bvh.parent_ids[0] != 0)
        return false;
    std::vector<size_t> expected(bvh.nodes.size(), 0);
    std::vector<char> referenced(bvh.nodes.size(), false);
    referenced[0] = true;
    for (size_t i = 0; i < bvh.nodes.size(); ++i) {
        const auto& node = bvh.nodes[i];
        if (node.is_leaf())
            continue;
        size_t left = node.index.first_id();
        size_t right = left + 1;
        if (right >= bvh.nodes.size() || referenced[left] || referenced[right])
            return false;
        expected[left] = i;
        expected[right] = i;
        referenced[left] = true;
        referenced[right] = true;
    }
    for (size_t i = 1; i < bvh.nodes.size(); ++i)
        if (!referenced[i] || expected[i] != bvh.parent_ids[i])
            return false;
    return true;
}

// Runs the stackless closest-hit query and optionally records the leaf node ids in visit order.
template <bool IsRobust, typename T>
static BruteHit<T> stackless_closest(
    const B3<T>& bvh,
    const std::vector<Tri3<T>>& tris,
    R3<T> ray,
    size_t root_node_id,
    const std::vector<size_t>* owner = nullptr,
    std::vector<size_t>* visited_leaves = nullptr)
{
    BruteHit<T> result;
    bvh.template intersect_stackless<false, IsRobust>(ray, root_node_id,
        [&] (size_t begin, size_t end) {
            if (begin >= end)
                return false;
            if (visited_leaves)
                visited_leaves->push_back(owner ? (*owner)[begin] : begin);
            for (size_t i = begin; i < end; ++i) {
                if (auto h = PTri<T>(tris[bvh.prim_ids[i]]).intersect(ray)) {
                    ray.tmax = std::get<0>(*h);
                    result.prim = bvh.prim_ids[i];
                }
            }
            return false;
        });
    result.hit = result.prim != std::numeric_limits<size_t>::max();
    if (result.hit)
        result.t = ray.tmax;
    return result;
}

template <bool IsRobust, typename T>
static bool stackless_any(
    const B3<T>& bvh,
    const std::vector<Tri3<T>>& tris,
    const R3<T>& ray,
    size_t root_node_id,
    std::vector<size_t>* visited_leaves = nullptr,
    const std::vector<size_t>* owner = nullptr)
{
    bool hit = false;
    bvh.template intersect_stackless<true, IsRobust>(ray, root_node_id,
        [&] (size_t begin, size_t end) {
            if (visited_leaves)
                visited_leaves->push_back(owner ? (*owner)[begin] : begin);
            for (size_t i = begin; i < end; ++i) {
                if (PTri<T>(tris[bvh.prim_ids[i]]).intersect(ray)) {
                    hit = true;
                    return true;
                }
            }
            return false;
        });
    return hit;
}

// Legacy stack-based reference, kept as a compatibility comparison.
template <bool IsRobust, typename T>
static BruteHit<T> stacked_closest(
    const B3<T>& bvh,
    const std::vector<Tri3<T>>& tris,
    R3<T> ray,
    size_t root_node_id)
{
    BruteHit<T> result;
    bvh::v2::GrowingStack<typename N3<T>::Index> stack;
    bvh.template intersect<false, IsRobust>(ray, bvh.nodes[root_node_id].index, stack,
        [&] (size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (auto h = PTri<T>(tris[bvh.prim_ids[i]]).intersect(ray)) {
                    ray.tmax = std::get<0>(*h);
                    result.prim = bvh.prim_ids[i];
                }
            }
            return false;
        });
    result.hit = result.prim != std::numeric_limits<size_t>::max();
    if (result.hit)
        result.t = ray.tmax;
    return result;
}

template <bool IsRobust, typename T>
static bool stacked_any(
    const B3<T>& bvh,
    const std::vector<Tri3<T>>& tris,
    const R3<T>& ray,
    size_t root_node_id)
{
    bool hit = false;
    bvh::v2::GrowingStack<typename N3<T>::Index> stack;
    bvh.template intersect<true, IsRobust>(ray, bvh.nodes[root_node_id].index, stack,
        [&] (size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (PTri<T>(tris[bvh.prim_ids[i]]).intersect(ray)) {
                    hit = true;
                    return true;
                }
            }
            return false;
        });
    return hit;
}

// Hand-assembled BVH: nodes are described up front and the parent table is built through the
// public maintenance entry point, exactly like a caller mutating `nodes` directly would do.
template <typename T>
struct HandTree {
    B3<T> bvh;
    std::vector<Tri3<T>> tris;

    void add_leaf(size_t id, const Box3<T>& bbox, std::initializer_list<size_t> prims) {
        ensure(id);
        bvh.nodes[id].set_bbox(bbox);
        size_t first = bvh.prim_ids.size();
        for (size_t p : prims)
            bvh.prim_ids.push_back(p);
        bvh.nodes[id].index = N3<T>::Index::make_leaf(first, prims.size());
    }

    void add_inner(size_t id, const Box3<T>& bbox, size_t left_id) {
        ensure(id);
        bvh.nodes[id].set_bbox(bbox);
        bvh.nodes[id].index = N3<T>::Index::make_inner(left_id);
    }

    void finish() {
        // Every node id used by a test tree is configured explicitly above, so nodes are simply
        // ordered and the parent table is rebuilt through the public maintenance entry point.
        bvh.rebuild_parent_ids();
    }

    void ensure(size_t id) {
        while (bvh.nodes.size() <= id)
            bvh.nodes.emplace_back();
    }
};

// Reference stackless order computed with an explicit work list and the exact rules from the
// specification: closest-hit enters the child with the smallest entry distance (left on ties),
// any-hit keeps left-to-right order, and every node is re-tested with the live `ray.tmax` right
// before being entered, so a tighter `tmax` installed by a near leaf culls far subtrees. Leaves
// run the same triangle tests as the real query, so both models observe identical tmax changes.
// This independent model is what the implementation is checked against.
template <bool IsAnyHit, bool IsRobust, typename T>
static std::vector<size_t> reference_leaf_order(
    const B3<T>& bvh,
    const std::vector<Tri3<T>>& tris,
    R3<T> ray,
    size_t root_node_id,
    const std::vector<size_t>& owner)
{
    auto hit_node = [&] (size_t id) {
        const auto& n = bvh.nodes[id];
        auto inv_dir = ray.template get_inv_dir<!IsRobust>();
        auto inv_org = -inv_dir * ray.org;
        auto inv_dir_pad = ray.pad_inv_dir(inv_dir);
        auto octant = ray.get_octant();
        if constexpr (IsRobust)
            return n.intersect_robust(ray, inv_dir, inv_dir_pad, octant);
        else
            return n.intersect_fast(ray, inv_dir, inv_org, octant);
    };

    std::vector<size_t> pending{root_node_id};
    std::vector<size_t> leaves;

    while (!pending.empty()) {
        size_t id = pending.back();
        pending.pop_back();

        // Re-test with the latest tmax before entering (far-child re-test rule).
        auto entry = hit_node(id);
        if (entry.first > entry.second)
            continue;

        const auto& node = bvh.nodes[id];
        if (node.is_leaf()) {
            leaves.push_back(owner[node.index.first_id()]);
            bool any = false;
            for (size_t k = node.index.first_id(), e = k + node.index.prim_count(); k < e; ++k) {
                if (auto h = PTri<T>(tris[bvh.prim_ids[k]]).intersect(ray)) {
                    ray.tmax = std::get<0>(*h);
                    any = true;
                }
            }
            if constexpr (IsAnyHit) {
                if (any)
                    return leaves;
            }
            continue;
        }

        size_t l = node.index.first_id();
        size_t r = l + 1;
        auto il = hit_node(l);
        auto ir = hit_node(r);
        bool hl = il.first <= il.second;
        bool hr = ir.first <= ir.second;

        size_t order[2];
        bool hit[2];
        order[0] = l; order[1] = r; hit[0] = hl; hit[1] = hr;
        if constexpr (!IsAnyHit) {
            if (hl && hr && il.first > ir.first) {
                order[0] = r; order[1] = l; hit[0] = hr; hit[1] = hl;
            }
        }
        // Push far first so the near child is popped first.
        for (int k = 1; k >= 0; --k) {
            if (hit[k])
                pending.push_back(order[k]);
        }
    }
    return leaves;
}

// -----------------------------------------------------------------------------------------------
// ordering group
// -----------------------------------------------------------------------------------------------

// Closest-hit visits the near child before the far child at every level, and the produced hit
// matches the brute-force result. Covers negative and zero direction components and rays starting
// inside boxes.
static int ordering_near_before_far() {
    Scene<float> s = Scene<float>::make(24);
    s.build_default();
    auto owner = slot_owner(s.bvh);

    const V3<float> dirs[] = {
        V3<float>(0, 0, 1),
        V3<float>(0, 1, -1),
        V3<float>(1, -1, 1),
        V3<float>(-1, 0, 1),
        V3<float>(0, 0, -1),
        V3<float>(0.5f, -0.25f, 1),
    };

    for (const auto& dir : dirs) {
        for (size_t target_id = 0; target_id < s.tris.size(); ++target_id) {
            auto ray = aim<float>(s.target(target_id), dir);
            auto expected = brute_closest(s.tris, ray);
            TASK_CHECK(expected.hit);

            std::vector<size_t> visited;
            auto got = stackless_closest<true>(s.bvh, s.tris, ray, 0, &owner, &visited);
            TASK_CHECK(same_hit(expected, got));

            // The first visited leaf must contain the closest primitive.
            TASK_CHECK(!visited.empty());
            bool first_contains_closest = false;
            {
                const auto& n = s.bvh.nodes[visited.front()];
                for (size_t k = n.index.first_id(), e = k + n.index.prim_count(); k < e; ++k)
                    first_contains_closest |= s.bvh.prim_ids[k] == expected.prim;
            }
            TASK_CHECK(first_contains_closest);

            auto ref = reference_leaf_order<false, true>(s.bvh, s.tris, ray, 0, owner);
            TASK_CHECK(visited == ref);
        }
    }

    // Rays whose origin lies inside the root bounding box: the entry tmin can be negative and
    // traversal must still agree with brute force and with the legacy stacked path.
    const auto& root_box = s.bvh.get_root().get_bbox();
    V3<float> inside_origin(
        (root_box.min[0] + root_box.max[0]) * 0.5f,
        (root_box.min[1] + root_box.max[1]) * 0.5f,
        (root_box.min[2] + root_box.max[2]) * 0.5f);
    for (size_t target_id = 0; target_id < s.tris.size(); target_id += 3) {
        V3<float> dir = s.target(target_id) - inside_origin;
        R3<float> inside_ray(inside_origin, dir, 0, 1e30f);
        auto expected = brute_closest(s.tris, inside_ray);
        TASK_CHECK(expected.hit);
        TASK_CHECK(same_hit(expected, stackless_closest<true>(s.bvh, s.tris, inside_ray, 0)));
        TASK_CHECK(same_hit(expected, stacked_closest<true>(s.bvh, s.tris, inside_ray, 0)));
    }
    return 0;
}

// Alternating near/far structure and live-tmax culling: a near leaf tightens `ray.tmax` so that a
// far subtree which was intersected at the parent must be skipped when it is finally reached.
static int ordering_tmax_cull_and_ties() {
    HandTree<float> tree;
    //          0
    //        /   \
    //       1     2        (1 is near along +z, 2 is far)
    //      / \   / \
    //     3   4 5   6
    const auto wide = [] (float z0, float z1) {
        return Box3<float>(V3<float>(-10, -10, z0), V3<float>(10, 10, z1));
    };
    // Triangles are pushed to the side so the ordering ray crosses every box but no triangle;
    // hit reporting is checked separately with rays aimed at each triangle.
    tree.tris = {
        Tri3<float>(V3<float>(-9, -1, 10), V3<float>(-8, -1, 10), V3<float>(-9, 1, 10)), // 0
        Tri3<float>(V3<float>(-7, -1, 11), V3<float>(-6, -1, 11), V3<float>(-7, 1, 11)), // 1
        Tri3<float>(V3<float>(7, -1, 30), V3<float>(8, -1, 30), V3<float>(7, 1, 30)),    // 2
        Tri3<float>(V3<float>(8, -1, 31), V3<float>(9, -1, 31), V3<float>(8, 1, 31)),    // 3
    };
    tree.add_leaf(3, wide(10, 10.5f), {0});
    tree.add_leaf(4, wide(11, 11.5f), {1});
    tree.add_leaf(5, wide(30, 30.5f), {2});
    tree.add_leaf(6, wide(31, 31.5f), {3});
    tree.add_inner(1, wide(10, 11.5f), 3);
    tree.add_inner(2, wide(30, 31.5f), 5);
    tree.add_inner(0, wide(10, 31.5f), 1);
    tree.finish();
    TASK_CHECK(check_parent_table(tree.bvh));

    auto owner = slot_owner(tree.bvh);
    R3<float> ray(V3<float>(0, 0, 0), V3<float>(0, 0, 1), 0, 100);

    // Without any triangle hit both halves are visited; near subtree (node 1) comes first, and
    // inside each subtree the near leaf precedes the far one.
    std::vector<size_t> visited;
    auto got = stackless_closest<true>(tree.bvh, tree.tris, ray, 0, &owner, &visited);
    TASK_CHECK(!got.hit);
    TASK_CHECK(visited.size() == 4);
    TASK_CHECK(visited[0] == 3 && visited[1] == 4);
    TASK_CHECK(visited[2] == 5 && visited[3] == 6);
    auto ref = reference_leaf_order<false, true>(tree.bvh, tree.tris, ray, 0, owner);
    TASK_CHECK(ref == visited);

    // A ray aimed at the nearest triangle reports it, and the same order is produced.
    R3<float> aimed(V3<float>(-8.5f, 0, 0), V3<float>(0, 0, 1), 0, 100);
    std::vector<size_t> aimed_visited;
    auto aimed_got = stackless_closest<true>(tree.bvh, tree.tris, aimed, 0, &owner, &aimed_visited);
    TASK_CHECK(aimed_got.hit && aimed_got.prim == 0);
    TASK_CHECK(std::abs(aimed_got.t - 10.f) < 1e-5f);
    TASK_CHECK(aimed_visited.front() == 3);

    // Ray starting inside the root box, with a tmax that excludes every remaining box: no leaf
    // may be visited, thanks to the far-child re-test against the live tmax.
    R3<float> inside(V3<float>(0, 0, 20), V3<float>(0, 0, 1), 0, 5);
    std::vector<size_t> inside_visited;
    auto inside_got = stackless_closest<true>(tree.bvh, tree.tris, inside, 0, &owner, &inside_visited);
    TASK_CHECK(!inside_got.hit);
    TASK_CHECK(inside_visited.empty());

    // Any-hit ends on the first callback that returns true. Aimed at prim 0, the first visited
    // leaf (node 3) terminates the traversal immediately.
    std::vector<size_t> any_visited;
    TASK_CHECK(stackless_any<true>(tree.bvh, tree.tris, aimed, 0, &any_visited, &owner));
    TASK_CHECK(any_visited.size() == 1 && any_visited[0] == 3);
    // A ray that hits nothing never terminates early and still walks every leaf.
    std::vector<size_t> any_miss;
    TASK_CHECK(!stackless_any<true>(tree.bvh, tree.tris, ray, 0, &any_miss, &owner));
    TASK_CHECK(any_miss == visited);

    // Explicit near-hit fallback: the near leaf records a hit that tightens `tmax`, so the far
    // subtree (which was intersected at the root with the original, larger tmax) must be culled
    // when the walk returns to the root and re-tests it with the live tmax.
    {
        HandTree<float> cull;
        cull.tris = {
            Tri3<float>(V3<float>(-0.5f, -0.5f, 10), V3<float>(0.5f, -0.5f, 10),
                        V3<float>(-0.5f, 0.5f, 10)),                                   // 0 near
            Tri3<float>(V3<float>(-0.5f, -0.5f, 22), V3<float>(0.5f, -0.5f, 22),
                        V3<float>(-0.5f, 0.5f, 22)),                                   // 1 far
            Tri3<float>(V3<float>(-0.5f, -0.5f, 26), V3<float>(0.5f, -0.5f, 26),
                        V3<float>(-0.5f, 0.5f, 26)),                                   // 2 far
        };
        // Layout: root 0 -> left leaf 1 (near) and right inner 2 -> leaves 3, 4 (far).
        cull.add_leaf(1, wide(9, 11), {0});
        cull.add_leaf(3, wide(21, 23), {1});
        cull.add_leaf(4, wide(25, 27), {2});
        cull.add_inner(2, wide(21, 27), 3);
        cull.add_inner(0, wide(9, 27), 1);
        cull.finish();
        TASK_CHECK(check_parent_table(cull.bvh));
        auto cull_owner = slot_owner(cull.bvh);
        R3<float> cull_ray(V3<float>(0, 0, 0), V3<float>(0, 0, 1), 0, 100);
        std::vector<size_t> cull_visited;
        auto cull_got = stackless_closest<true>(cull.bvh, cull.tris, cull_ray, 0,
            &cull_owner, &cull_visited);
        TASK_CHECK(cull_got.hit && cull_got.prim == 0);
        TASK_CHECK(std::abs(cull_got.t - 10.f) < 1e-5f);
        // Only the near leaf (node 1) is visited; the far subtree is culled on the way back up
        // because the internal tmax was tightened to the near hit (~10, below the far boxes).
        TASK_CHECK(cull_visited.size() == 1 && cull_visited[0] == 1);
        // A ray through every box but clear of the triangles still reaches all three leaves.
        R3<float> miss_ray(V3<float>(8, 0, 0), V3<float>(0, 0, 1), 0, 100);
        std::vector<size_t> miss_visited;
        auto miss_got = stackless_closest<true>(cull.bvh, cull.tris, miss_ray, 0,
            &cull_owner, &miss_visited);
        TASK_CHECK(!miss_got.hit);
        TASK_CHECK(miss_visited.size() == 3);
        // The legacy stacked path reaches the same closest hit.
        TASK_CHECK(same_hit(cull_got, stacked_closest<true>(cull.bvh, cull.tris, cull_ray, 0)));
    }

    // Equal entry distances: the two leaf boxes share their near plane, so the left child must
    // win. Their triangles sit on opposite sides of the ordering ray, so both leaves are visited
    // without a hit and the tie-breaking order is directly observable.
    HandTree<float> tie_tree;
    tie_tree.tris = {
        Tri3<float>(V3<float>(-2, -1, 10), V3<float>(-1, -1, 10), V3<float>(-2, 1, 10)),
        Tri3<float>(V3<float>(1, -1, 10), V3<float>(2, -1, 10), V3<float>(1, 1, 10)),
    };
    tie_tree.add_leaf(1, wide(9, 11), {0});
    tie_tree.add_leaf(2, wide(9, 11), {1});
    tie_tree.add_inner(0, wide(9, 11), 1);
    tie_tree.finish();
    TASK_CHECK(check_parent_table(tie_tree.bvh));
    auto tie_owner = slot_owner(tie_tree.bvh);
    std::vector<size_t> tie_visited;
    auto tie_got = stackless_closest<true>(tie_tree.bvh, tie_tree.tris, ray, 0, &tie_owner, &tie_visited);
    TASK_CHECK(!tie_got.hit);
    TASK_CHECK(tie_visited.size() == 2 && tie_visited[0] == 1 && tie_visited[1] == 2);
    auto tie_ref = reference_leaf_order<false, true>(tie_tree.bvh, tie_tree.tris, ray, 0, tie_owner);
    TASK_CHECK(tie_ref == tie_visited);
    return 0;
}

// Left/right alternation of the near child at successive levels, with no triangle hits so that
// every leaf is observed: the near choice must alternate between the two children level by level,
// and the produced order must match the independent reference model.
static int ordering_alternating_near_child() {
    HandTree<float> tree;
    const auto wide_box = [] (float z0, float z1) {
        return Box3<float>(V3<float>(-100, -100, z0), V3<float>(100, 100, z1));
    };
    // Triangles are pushed away from the ray (x = 50..51) while their boxes still contain it.
    for (int i = 0; i < 8; ++i) {
        float z = 10.f + 5.f * i;
        tree.tris.emplace_back(
            V3<float>(50, -1, z), V3<float>(51, -1, z), V3<float>(50, 1, z));
    }
    static const size_t leaf_ids[8] = {7, 8, 9, 10, 11, 12, 13, 14};
    static const float leaf_z[8] = {40, 30, 10, 20, 15, 25, 35, 45};
    for (int k = 0; k < 8; ++k)
        tree.add_leaf(leaf_ids[k], wide_box(leaf_z[k], leaf_z[k] + 2), {size_t(k)});
    tree.add_inner(3, wide_box(30, 42), 7);
    tree.add_inner(4, wide_box(10, 22), 9);
    tree.add_inner(5, wide_box(15, 27), 11);
    tree.add_inner(6, wide_box(35, 47), 13);
    tree.add_inner(1, wide_box(10, 42), 3);  // right child (4) is nearer
    tree.add_inner(2, wide_box(15, 47), 5);  // left child (5) is nearer
    tree.add_inner(0, wide_box(10, 47), 1);  // left child (1) is nearer
    tree.finish();
    TASK_CHECK(check_parent_table(tree.bvh));

    auto owner = slot_owner(tree.bvh);
    R3<float> ray(V3<float>(0, 0, 0), V3<float>(0, 0, 1), 0, 100);
    std::vector<size_t> visited;
    auto got = stackless_closest<false>(tree.bvh, tree.tris, ray, 0, &owner, &visited);
    TASK_CHECK(!got.hit);
    static const size_t expected_order[8] = {9, 10, 8, 7, 11, 12, 13, 14};
    TASK_CHECK(visited.size() == 8);
    for (size_t i = 0; i < 8; ++i)
        TASK_CHECK(visited[i] == expected_order[i]);
    auto ref = reference_leaf_order<false, false>(tree.bvh, tree.tris, ray, 0, owner);
    TASK_CHECK(ref == visited);
    return 0;
}

// -----------------------------------------------------------------------------------------------
// topology group
// -----------------------------------------------------------------------------------------------

// Builds a legal binary chain of the requested depth: inner node `2*k` has a leaf child `2*k+1`
// and a child `2*k+2` which is the next inner node (or the final leaf at the deepest level).
template <typename T>
static HandTree<T> make_chain(size_t depth) {
    HandTree<T> tree;
    auto full_box = Box3<T>(V3<T>(T(-1), T(-1), T(0)), V3<T>(T(1), T(1), T(depth + 2)));
    size_t prim = 0;
    for (size_t k = 0; k < depth; ++k) {
        size_t inner_id = 2 * k;
        size_t left_id = inner_id + 1;
        size_t right_id = inner_id + 2;

        T side_z = T(k) + T(0.25);
        tree.tris.emplace_back(
            V3<T>(T(-0.5), T(-0.5), side_z),
            V3<T>( T(0.5), T(-0.5), side_z),
            V3<T>(T(-0.5),  T(0.5), side_z));
        tree.add_leaf(left_id, full_box, {prim++});

        if (k == depth - 1) {
            T end_z = T(depth) + T(0.5);
            tree.tris.emplace_back(
                V3<T>(T(-0.5), T(-0.5), end_z),
                V3<T>( T(0.5), T(-0.5), end_z),
                V3<T>(T(-0.5),  T(0.5), end_z));
            tree.add_leaf(right_id, full_box, {prim++});
        }
        tree.add_inner(inner_id, full_box, left_id);
    }
    tree.finish();
    return tree;
}

template <typename T>
static int chain_case() {
    constexpr size_t depth = 256;
    HandTree<T> tree = make_chain<T>(depth);
    TASK_CHECK(tree.bvh.nodes.size() == 2 * depth + 1);
    TASK_CHECK(check_parent_table(tree.bvh));
    TASK_CHECK(tree.bvh.parent_ids.size() == tree.bvh.nodes.size());

    // Every leaf child points back to the chain inner at its level.
    for (size_t k = 0; k < depth; ++k) {
        TASK_CHECK(tree.bvh.parent_ids[2 * k + 1] == 2 * k);
        TASK_CHECK(tree.bvh.parent_ids[2 * k + 2] == 2 * k);
    }
    // The deepest leaf reaches the root in exactly `depth` parent steps.
    size_t steps = 0;
    for (size_t cur = 2 * depth; cur != 0; cur = tree.bvh.parent_ids[cur])
        ++steps;
    TASK_CHECK(steps == depth);

    // One ray intersects every leaf box; the stackless walk must therefore report every leaf
    // exactly once (no replay, no omission) and still find the nearest primitive.
    R3<T> ray(V3<T>(0, 0, T(-1)), V3<T>(0, 0, 1), 0, T(1e30));
    auto expected = brute_closest(tree.tris, ray);
    TASK_CHECK(expected.hit);

    auto owner = slot_owner(tree.bvh);
    std::vector<size_t> visited;
    auto got = stackless_closest<true>(tree.bvh, tree.tris, ray, 0, &owner, &visited);
    TASK_CHECK(same_hit(expected, got));

    size_t leaf_count = 0;
    for (const auto& n : tree.bvh.nodes)
        leaf_count += n.is_leaf();
    TASK_CHECK(leaf_count == depth + 1);
    TASK_CHECK(visited.size() == leaf_count);
    std::vector<size_t> sorted_visited = visited;
    std::sort(sorted_visited.begin(), sorted_visited.end());
    sorted_visited.erase(std::unique(sorted_visited.begin(), sorted_visited.end()),
        sorted_visited.end());
    TASK_CHECK(sorted_visited.size() == leaf_count);

    // Same closest hit through the legacy stacked path (robust ray-box test).
    auto stacked = stacked_closest<true>(tree.bvh, tree.tris, ray, 0);
    TASK_CHECK(same_hit(expected, stacked));

    // The fast ray-box test uses a raw reciprocal, so exercise it with a tilted ray whose
    // components are all nonzero; it still crosses every box of the chain and hits the first
    // side triangle, which the brute-force reference confirms.
    R3<T> fast_ray(V3<T>(0, 0, T(-1)), V3<T>(T(-0.0031), T(-0.0023), T(1)), 0, T(1e30));
    auto fast_expected = brute_closest(tree.tris, fast_ray);
    TASK_CHECK(fast_expected.hit && fast_expected.prim == 0);
    TASK_CHECK(same_hit(fast_expected, stackless_closest<false>(tree.bvh, tree.tris, fast_ray, 0)));
    TASK_CHECK(same_hit(fast_expected, stacked_closest<false>(tree.bvh, tree.tris, fast_ray, 0)));

    // After all preparation is done, queries themselves must not perform any heap allocation,
    // regardless of tree depth.
    auto snapshot = alloc_snapshot();
    for (int k = 0; k < 32; ++k) {
        R3<T> q(V3<T>(0, 0, T(-1)), V3<T>(0, 0, 1), 0, T(1e30));
        auto h = stackless_closest<true>(tree.bvh, tree.tris, q, 0);
        TASK_CHECK(h.hit);
        R3<T> shadow(V3<T>(0, 0, T(-1)), V3<T>(0, 0, 1), 0, T(1e30));
        TASK_CHECK(stackless_any<true>(tree.bvh, tree.tris, shadow, 0));
    }
    TASK_CHECK(alloc_unchanged(snapshot));
    return 0;
}

static int topology_deep_chain() {
    TASK_CHECK(chain_case<float>() == 0);
    TASK_CHECK(chain_case<double>() == 0);
    return 0;
}

// Starting from a non-root node only visits that subtree, and termination happens at its top.
static int topology_subtree_isolation_and_edges() {
    Scene<float> s = Scene<float>::make(40);
    s.build_sweep();
    TASK_CHECK(check_parent_table(s.bvh));

    const auto& root = s.bvh.nodes[0];
    TASK_CHECK(!root.is_leaf());
    size_t sub_root = root.index.first_id() + 1; // right child of the global root
    auto allowed = subtree_prim_mask(s.bvh, sub_root, s.tris.size());
    auto in_subtree = subtree_node_ids(s.bvh, sub_root);
    auto owner = slot_owner(s.bvh);

    for (size_t target_id = 0; target_id < s.tris.size(); ++target_id) {
        auto ray = aim(s.target(target_id), V3<float>(0.3f, -0.2f, 1));
        auto expected = brute_closest(s.tris, ray, &allowed);
        std::vector<size_t> visited;
        auto got = stackless_closest<true>(s.bvh, s.tris, ray, sub_root, &owner, &visited);
        TASK_CHECK(same_hit(expected, got));
        for (size_t id : visited)
            TASK_CHECK(std::find(in_subtree.begin(), in_subtree.end(), id) != in_subtree.end());

        bool any_expected = brute_any(s.tris, ray, &allowed);
        TASK_CHECK(stackless_any<true>(s.bvh, s.tris, ray, sub_root) == any_expected);
    }

    // A primitive outside the subtree that is globally the nearest must never be reported.
    size_t outside = std::numeric_limits<size_t>::max();
    for (size_t i = 0; i < allowed.size(); ++i) {
        if (!allowed[i]) { outside = i; break; }
    }
    TASK_CHECK(outside != std::numeric_limits<size_t>::max());
    {
        auto ray = aim(s.target(outside), V3<float>(0, 0, 1));
        auto got = stackless_closest<true>(s.bvh, s.tris, ray, sub_root);
        TASK_CHECK(got.prim != outside);
    }

    // Default-constructed empty tree: empty parent table and queries are no-ops.
    B3<float> empty;
    TASK_CHECK(empty.nodes.empty() && empty.parent_ids.empty());
    empty.rebuild_parent_ids();
    TASK_CHECK(empty.parent_ids.empty());
    R3<float> ray(V3<float>(0), V3<float>(0, 0, 1), 0, 100);
    TASK_CHECK(!stackless_any<true>(empty, s.tris, ray, 0));
    auto none = stackless_closest<true>(empty, s.tris, ray, 0);
    TASK_CHECK(!none.hit);
    // Out-of-range start indices are ignored as well.
    TASK_CHECK(!stackless_any<false>(s.bvh, s.tris, ray, s.bvh.nodes.size() + 10));

    // Single-leaf tree.
    Scene<float> one = Scene<float>::make(1);
    one.build_default();
    TASK_CHECK(one.bvh.nodes.size() == 1 && one.bvh.nodes[0].is_leaf());
    TASK_CHECK(one.bvh.parent_ids.size() == 1 && one.bvh.parent_ids[0] == 0);
    auto one_ray = aim(one.target(0), V3<float>(0, 0, 1));
    auto one_got = stackless_closest<true>(one.bvh, one.tris, one_ray, 0);
    TASK_CHECK(one_got.hit && one_got.prim == 0);
    TASK_CHECK(stackless_any<false>(one.bvh, one.tris, one_ray, 0));
    return 0;
}

// Many threads read the same immutable BVH concurrently; every query must independently match
// the brute-force result, proving the read-only parent table and traversal state are per-ray.
static int topology_concurrent_reads() {
    Scene<float> s = Scene<float>::make(48);
    s.build_default();
    const auto& bvh = s.bvh;

    constexpr size_t thread_count = 4;
    constexpr size_t rays_per_thread = 200;
    std::atomic<unsigned> errors{0};
    std::vector<std::thread> threads;
    for (size_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            for (size_t k = 0; k < rays_per_thread; ++k) {
                size_t target_id = (t * rays_per_thread + k) % s.tris.size();
                const V3<float> dirs[3] = {
                    V3<float>(0.2f, -0.1f, 1.f),
                    V3<float>(-0.3f, 0.15f, 1.f),
                    V3<float>(0.1f, 0.25f, -1.f),
                };
                auto ray = aim(s.target(target_id), dirs[k % 3]);
                auto expected = brute_closest(s.tris, ray);
                auto got = stackless_closest<true>(bvh, s.tris, ray, 0);
                if (!same_hit(expected, got)) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (stackless_any<true>(bvh, s.tris, ray, 0) != expected.hit) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (auto& th : threads)
        th.join();
    TASK_CHECK(errors.load() == 0);
    return 0;
}

// -----------------------------------------------------------------------------------------------
// storage group
// -----------------------------------------------------------------------------------------------

// Cross-checks both scalar types, both ray-box modes and both hit modes against the brute-force
// reference and the legacy stacked implementation, exercising every builder entry point. The
// robust ray-box test (safe inverse) covers directions with zero components; the fast test uses
// only non-degenerate directions, matching the library's documented valid-input convention.
template <typename T>
static int query_matrix(const B3<T>& bvh, const std::vector<Tri3<T>>& tris) {
    TASK_CHECK(check_parent_table(bvh));
    const V3<T> robust_dirs[] = {
        V3<T>(0, 0, 1), V3<T>(0, 0, -1), V3<T>(1, 0, 1), V3<T>(-1, 1, -1),
        V3<T>(0, 1, 0), V3<T>(0.25, -0.5, 1),
    };
    const V3<T> fast_dirs[] = {
        V3<T>(0.31, 0.17, 1), V3<T>(-0.43, 0.29, -1), V3<T>(0.25, -0.5, 1),
        V3<T>(-0.11, -0.23, 1),
    };

    auto run_mode = [&] (bool fast_mode, const V3<T>* dirs, size_t dir_count) -> int {
        for (size_t d = 0; d < dir_count; ++d) {
            const V3<T>& dir = dirs[d];
            for (size_t i = 0; i < tris.size(); ++i) {
                auto center = tris[i].get_center();
                R3<T> ray(center - dir * T(6), dir, T(0), T(1e30));
                auto expected = brute_closest(tris, ray);

                if (fast_mode) {
                    TASK_CHECK(same_hit(expected, stackless_closest<false>(bvh, tris, ray, 0)));
                    TASK_CHECK(same_hit(expected, stacked_closest<false>(bvh, tris, ray, 0)));
                    TASK_CHECK(stackless_any<false>(bvh, tris, ray, 0) == expected.hit);
                    TASK_CHECK(stacked_any<false>(bvh, tris, ray, 0) == expected.hit);
                } else {
                    TASK_CHECK(same_hit(expected, stackless_closest<true>(bvh, tris, ray, 0)));
                    TASK_CHECK(same_hit(expected, stacked_closest<true>(bvh, tris, ray, 0)));
                    TASK_CHECK(stackless_any<true>(bvh, tris, ray, 0) == expected.hit);
                    TASK_CHECK(stacked_any<true>(bvh, tris, ray, 0) == expected.hit);
                }

                // A finite shadow ray that stops before the first hit always stays clear.
                R3<T> short_ray(ray.org, ray.dir, T(0),
                    expected.hit ? expected.t * T(0.5) : T(1));
                TASK_CHECK(!brute_any(tris, short_ray));
                TASK_CHECK(!stackless_any<true>(bvh, tris, short_ray, 0));
                TASK_CHECK(!stackless_any<false>(bvh, tris, short_ray, 0));
            }
        }
        return 0;
    };
    if (run_mode(false, robust_dirs, sizeof(robust_dirs) / sizeof(robust_dirs[0])) != 0)
        return 1;
    if (run_mode(true, fast_dirs, sizeof(fast_dirs) / sizeof(fast_dirs[0])) != 0)
        return 1;
    return 0;
}

static int storage_builders_and_archive() {
    // float, single-threaded builders.
    {
        Scene<float> s = Scene<float>::make(60);
        s.build_sweep();
        TASK_CHECK(query_matrix(s.bvh, s.tris) == 0);
        s.build_binned();
        TASK_CHECK(query_matrix(s.bvh, s.tris) == 0);
        s.build_default();
        TASK_CHECK(query_matrix(s.bvh, s.tris) == 0);

        // Thread-pool entry point with a forced parallel build (mini-tree path).
        bvh::v2::ThreadPool pool(2);
        typename bvh::v2::DefaultBuilder<N3<float>>::Config config;
        config.parallel_threshold = 1;
        auto parallel = bvh::v2::DefaultBuilder<N3<float>>::build(
            pool, std::span{s.boxes.data(), s.boxes.size()},
            std::span{s.centers.data(), s.centers.size()}, config);
        TASK_CHECK(check_parent_table(parallel));
        TASK_CHECK(query_matrix(parallel, s.tris) == 0);
    }

    // double, including the low-quality binned default.
    {
        Scene<double> s = Scene<double>::make(40);
        s.build_default();
        TASK_CHECK(query_matrix(s.bvh, s.tris) == 0);
        typename bvh::v2::DefaultBuilder<N3<double>>::Config low;
        low.quality = bvh::v2::DefaultBuilder<N3<double>>::Quality::Low;
        s.bvh = bvh::v2::DefaultBuilder<N3<double>>::build(s.boxes, s.centers, low);
        TASK_CHECK(check_parent_table(s.bvh));
        TASK_CHECK(query_matrix(s.bvh, s.tris) == 0);

        // Archive byte layout is unchanged: counts, then raw nodes, then raw primitive ids, with
        // nothing appended for the parent table.
        std::stringstream archive(std::ios::in | std::ios::out | std::ios::binary);
        bvh::v2::StdOutputStream out(archive);
        s.bvh.serialize(out);
        const std::string bytes = archive.str();
        using Idx = N3<double>::Index::Type;
        // Exact legacy byte layout: two counts, then per node its 6 bounds and one index, then
        // the primitive ids. The parent table must not add a single byte.
        const size_t expected_size =
            2 * sizeof(Idx) +
            s.bvh.nodes.size() * (6 * sizeof(double) + sizeof(Idx)) +
            s.bvh.prim_ids.size() * sizeof(Idx);
        TASK_CHECK(bytes.size() == expected_size);

        // Independently emit the legacy layout and require byte-for-byte equality.
        std::stringstream legacy(std::ios::in | std::ios::out | std::ios::binary);
        bvh::v2::StdOutputStream legacy_out(legacy);
        legacy_out.write(static_cast<Idx>(s.bvh.nodes.size()));
        legacy_out.write(static_cast<Idx>(s.bvh.prim_ids.size()));
        for (auto& n : s.bvh.nodes)
            n.serialize(legacy_out);
        for (auto id : s.bvh.prim_ids)
            legacy_out.write(static_cast<Idx>(id));
        TASK_CHECK(legacy.str() == bytes);

        bvh::v2::StdInputStream in(archive);
        auto loaded = B3<double>::deserialize(in);
        TASK_CHECK(loaded.parent_ids.size() == loaded.nodes.size());
        TASK_CHECK(loaded.parent_ids == s.bvh.parent_ids);
        TASK_CHECK(loaded.nodes == s.bvh.nodes && loaded.prim_ids == s.bvh.prim_ids);
        TASK_CHECK(query_matrix(loaded, s.tris) == 0);
    }
    return 0;
}

// Degraded geometry goes through a real reinsertion (connectivity changes), then subtree
// extraction, save/load and refit are checked step by step.
static int storage_reinsert_extract_refit() {
    std::vector<Tri3<float>> tris;
    unsigned seed = 12345;
    auto rnd = [&] {
        seed = seed * 1103515245u + 12345u;
        return float(seed % 1000) / 1000.f;
    };
    for (int i = 0; i < 200; ++i) {
        float scale = (i % 5 == 0) ? 8.f : 0.3f;
        float x = rnd() * 40 - 20, y = rnd() * 40 - 20;
        float z = (i % 5 == 0) ? rnd() * 4 : rnd() * 0.5f;
        tris.emplace_back(V3<float>(x, y, z), V3<float>(x + scale, y, z),
            V3<float>(x, y + scale, z));
    }
    std::vector<Box3<float>> boxes;
    std::vector<V3<float>> centers;
    for (auto& t : tris) {
        boxes.push_back(t.get_bbox());
        centers.push_back(t.get_center());
    }

    auto bvh = bvh::v2::SweepSahBuilder<N3<float>>::build(boxes, centers);
    TASK_CHECK(check_parent_table(bvh));
    auto topology_sig = [&] {
        size_t h = 1469598103934665603ULL;
        for (size_t i = 0; i < bvh.nodes.size(); ++i) {
            size_t v = bvh.nodes[i].is_leaf()
                ? bvh.nodes[i].index.first_id() + 7777
                : bvh.nodes[i].index.first_id() + 13;
            h ^= v + i * 31;
            h *= 1099511628211ULL;
        }
        return h;
    };
    size_t before = topology_sig();
    bvh::v2::ReinsertionOptimizer<N3<float>>::optimize(bvh);
    TASK_CHECK(topology_sig() != before); // genuine connectivity change occurred
    TASK_CHECK(check_parent_table(bvh));
    TASK_CHECK(query_matrix(bvh, tris) == 0);

    // Extract a non-root subtree: parent chain, leaf set and restricted queries.
    size_t sub_root = bvh.nodes[0].index.first_id() + 1;
    auto sub = bvh.extract_bvh(sub_root);
    TASK_CHECK(check_parent_table(sub));
    TASK_CHECK(sub.parent_ids.size() == sub.nodes.size());
    auto allowed = subtree_prim_mask(bvh, sub_root, tris.size());
    std::vector<char> sub_prim_seen(tris.size(), false);
    for (size_t id : sub.prim_ids)
        sub_prim_seen[id] = true;
    TASK_CHECK(sub_prim_seen == allowed);
    {
        auto center = tris[sub.prim_ids.front()].get_center();
        R3<float> ray(center - V3<float>(0, 0, 6), V3<float>(0, 0, 1), 0, 1e30f);
        TASK_CHECK(same_hit(brute_closest(tris, ray, &allowed),
            stackless_closest<true>(sub, tris, ray, 0)));
    }

    // Save/load the subtree: parent table is reconstructed, queries are unchanged.
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    bvh::v2::StdOutputStream out(ss);
    sub.serialize(out);
    bvh::v2::StdInputStream in(ss);
    auto sub_loaded = B3<float>::deserialize(in);
    TASK_CHECK(sub_loaded.parent_ids == sub.parent_ids);
    TASK_CHECK(sub_loaded == sub);
    for (size_t i = 0; i < sub.prim_ids.size(); ++i) {
        auto center = tris[sub.prim_ids[i]].get_center();
        R3<float> ray(center - V3<float>(0, 0, 6), V3<float>(0, 0, 1), 0, 1e30f);
        TASK_CHECK(same_hit(stackless_closest<true>(sub, tris, ray, 0),
            stackless_closest<true>(sub_loaded, tris, ray, 0)));
    }

    // Move the geometry and refit: the topology (and therefore parent table) stays valid, and
    // queries keep matching brute force.
    std::vector<Tri3<float>> moved = tris;
    for (auto& t : moved) {
        t.p0[2] += 3; t.p1[2] += 3; t.p2[2] += 3;
    }
    sub_loaded.refit([&] (N3<float>& node) {
        auto box = Box3<float>::make_empty();
        for (size_t i = node.index.first_id(),
             e = i + node.index.prim_count(); i < e; ++i)
            box.extend(moved[sub_loaded.prim_ids[i]].get_bbox());
        node.set_bbox(box);
    });
    TASK_CHECK(check_parent_table(sub_loaded));
    for (size_t i = 0; i < sub_loaded.prim_ids.size(); ++i) {
        auto center = moved[sub_loaded.prim_ids[i]].get_center();
        R3<float> ray(center - V3<float>(0, 0, 6), V3<float>(0, 0, 1), 0, 1e30f);
        TASK_CHECK(same_hit(brute_closest(moved, ray, &allowed),
            stackless_closest<true>(sub_loaded, moved, ray, 0)));
    }

    // Direct, external mutation of `nodes` requires an explicit parent-table rebuild.
    B3<float> manual = bvh.extract_bvh(sub_root);
    manual.nodes[0].set_bbox(manual.nodes[0].get_bbox()); // touch without changing topology
    manual.rebuild_parent_ids();
    TASK_CHECK(manual.parent_ids == sub.parent_ids);
    return 0;
}

// -----------------------------------------------------------------------------------------------
// render group
// -----------------------------------------------------------------------------------------------

struct RenderScene {
    std::vector<Tri3<float>> tris;
    std::vector<V3<float>> colors;
    std::vector<Box3<float>> boxes;
    std::vector<V3<float>> centers;
    B3<float> bvh;
    V3<float> light{V3<float>(-20, 30, 40)};

    static RenderScene make() {
        RenderScene r;
        const std::array<V3<float>, 6> palette = {
            V3<float>(0.85f, 0.10f, 0.10f), V3<float>(0.10f, 0.70f, 0.20f),
            V3<float>(0.15f, 0.25f, 0.90f), V3<float>(0.90f, 0.75f, 0.10f),
            V3<float>(0.70f, 0.20f, 0.80f), V3<float>(0.10f, 0.80f, 0.85f),
        };
        for (int gy = 0; gy < 3; ++gy) {
            for (int gx = 0; gx < 5; ++gx) {
                size_t i = r.tris.size();
                float x = float(gx) * 2.2f - 4.4f;
                float y = float(gy) * 2.0f - 2.0f;
                float z = 4.f + 0.41f * float(i); // distinct planes: no equidistant hits
                r.tris.emplace_back(
                    V3<float>(x, y, z), V3<float>(x + 1.6f, y, z),
                    V3<float>(x, y + 1.5f, z));
                r.colors.push_back(palette[i % palette.size()]);
            }
        }
        for (auto& t : r.tris) {
            r.boxes.push_back(t.get_bbox());
            r.centers.push_back(t.get_center());
        }
        r.bvh = bvh::v2::DefaultBuilder<N3<float>>::build(r.boxes, r.centers);
        return r;
    }
};

static int render_ppm_image(bool robust) {
    auto scene = RenderScene::make();
    constexpr size_t width = 18, height = 14;

    auto trace = [&] (R3<float> ray) -> V3<int> {
        size_t prim = std::numeric_limits<size_t>::max();
        auto leaf = [&] (size_t begin, size_t end) {
            for (size_t k = begin; k < end; ++k) {
                if (auto h = PTri<float>(scene.tris[scene.bvh.prim_ids[k]]).intersect(ray)) {
                    ray.tmax = std::get<0>(*h);
                    prim = scene.bvh.prim_ids[k];
                }
            }
            return false;
        };
        if (robust)
            scene.bvh.template intersect_stackless<false, true>(ray, 0, leaf);
        else
            scene.bvh.template intersect_stackless<false, false>(ray, 0, leaf);
        if (prim == std::numeric_limits<size_t>::max())
            return V3<int>(20, 20, 24); // background

        // Shadow ray toward the light, finite and starting at the hit point.
        auto hit_point = ray.org + ray.dir * ray.tmax;
        auto to_light = scene.light - hit_point;
        R3<float> shadow(hit_point, to_light, 1e-4f, 1.0f);
        bool occluded = false;
        auto shadow_leaf = [&] (size_t begin, size_t end) {
            for (size_t k = begin; k < end; ++k) {
                if (PTri<float>(scene.tris[scene.bvh.prim_ids[k]]).intersect(shadow)) {
                    occluded = true;
                    return true;
                }
            }
            return false;
        };
        if (robust)
            scene.bvh.template intersect_stackless<true, true>(shadow, 0, shadow_leaf);
        else
            scene.bvh.template intersect_stackless<true, false>(shadow, 0, shadow_leaf);
        float shade = occluded ? 0.25f : 1.f;
        auto c = scene.colors[prim] * shade * 255.f;
        return V3<int>(
            std::min(255, int(c[0] + 0.5f)),
            std::min(255, int(c[1] + 0.5f)),
            std::min(255, int(c[2] + 0.5f)));
    };

    auto brute_trace = [&] (R3<float> ray) -> V3<int> {
        auto closest = brute_closest(scene.tris, ray);
        if (!closest.hit)
            return V3<int>(20, 20, 24);
        auto hit_point = ray.org + ray.dir * closest.t;
        auto to_light = scene.light - hit_point;
        R3<float> shadow(hit_point, to_light, 1e-4f, 1.0f);
        bool lit = !brute_any(scene.tris, shadow);
        float shade = lit ? 1.f : 0.25f;
        auto c = scene.colors[closest.prim] * shade * 255.f;
        return V3<int>(
            std::min(255, int(c[0] + 0.5f)),
            std::min(255, int(c[1] + 0.5f)),
            std::min(255, int(c[2] + 0.5f)));
    };

    std::string ppm = "P3\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    size_t colored = 0;
    for (size_t py = 0; py < height; ++py) {
        for (size_t px = 0; px < width; ++px) {
            // Extents are chosen so pixel rays avoid landing exactly on triangle edges/vertices,
            // and the slight tilt keeps every direction component nonzero so the fast ray-box
            // path (which uses a raw reciprocal) is exercised with valid non-degenerate input.
            float u = (float(px) + 0.5f) / float(width) * 9.8f - 4.9f;
            float v = (float(py) + 0.5f) / float(height) * 7.8f - 3.9f;
            R3<float> ray(V3<float>(u, v, 0), V3<float>(0.0013f, -0.0009f, 1.f), 0, 1e30f);
            auto pixel = trace(ray);
            auto reference = brute_trace(ray);
            TASK_CHECK(pixel[0] == reference[0] && pixel[1] == reference[1] &&
                       pixel[2] == reference[2]);
            if (pixel[0] != 20 || pixel[1] != 20 || pixel[2] != 24)
                ++colored;
            ppm += std::to_string(pixel[0]) + " " + std::to_string(pixel[1]) + " " +
                   std::to_string(pixel[2]) + "\n";
        }
    }
    TASK_CHECK(colored > width); // a meaningful part of the image carries primitive colors
    TASK_CHECK(ppm.rfind("P3", 0) == 0);

    // Persist the small image so the run produces an actual P3 PPM artifact.
    std::ofstream file(".task-build/stackless_render.ppm", std::ios::binary | std::ios::trunc);
    TASK_CHECK(static_cast<bool>(file));
    file << ppm;
    TASK_CHECK(static_cast<bool>(file));

    // Round-trip the header of the generated image.
    std::istringstream parsed(ppm);
    std::string magic;
    size_t w2 = 0, h2 = 0, maxv = 0;
    parsed >> magic >> w2 >> h2 >> maxv;
    TASK_CHECK(magic == "P3" && w2 == width && h2 == height && maxv == 255);
    return 0;
}

static int render_ppm_robust() { return render_ppm_image(true); }
static int render_ppm_fast() { return render_ppm_image(false); }

// -----------------------------------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------------------------------

} // namespace

const task_case task_cases[] = {
    {"ordering", "near_before_far", ordering_near_before_far},
    {"ordering", "tmax_cull_and_ties", ordering_tmax_cull_and_ties},
    {"ordering", "alternating_near_child", ordering_alternating_near_child},
    {"topology", "deep_chain", topology_deep_chain},
    {"topology", "subtree_isolation_and_edges", topology_subtree_isolation_and_edges},
    {"topology", "concurrent_reads", topology_concurrent_reads},
    {"storage", "builders_and_archive", storage_builders_and_archive},
    {"storage", "reinsert_extract_refit", storage_reinsert_extract_refit},
    {"render", "ppm_robust", render_ppm_robust},
    {"render", "ppm_fast", render_ppm_fast},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
