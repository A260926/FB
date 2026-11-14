#include <bvh/v2/bbox.h>
#include <bvh/v2/bvh.h>
#include <bvh/v2/default_builder.h>
#include <bvh/v2/node.h>
#include <bvh/v2/ray.h>
#include <bvh/v2/stack.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/vec.h>

#include "task_support/check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

namespace {

using Scalar = float;
using Vec3 = bvh::v2::Vec<Scalar, 3>;
using BBox = bvh::v2::BBox<Scalar, 3>;
using Node = bvh::v2::Node<Scalar, 3>;
using Bvh = bvh::v2::Bvh<Node>;
using Ray = bvh::v2::Ray<Scalar, 3>;
using Tri = bvh::v2::Tri<Scalar, 3>;

constexpr size_t max_leaf = 4;

//// Helpers ---------------------------------------------------------------------------------------

bool bbox_equal(const BBox& a, const BBox& b) {
    for (size_t axis = 0; axis < 3; ++axis) {
        if (a.min[axis] != b.min[axis] || a.max[axis] != b.max[axis])
            return false;
    }
    return true;
}

BBox make_box(float x, float y, float z, float half = 0.0f) {
    return BBox(Vec3(x - half, y - half, z - half), Vec3(x + half, y + half, z + half));
}

BBox triangle_bbox(const Vec3& a, const Vec3& b, const Vec3& c) {
    BBox box = BBox::make_empty();
    box.extend(a);
    box.extend(b);
    box.extend(c);
    return box;
}

/// A triangle whose plane faces the ray origin; its bounds are used as the BVH primitive data.
struct Triangle {
    Vec3 a, b, c;

    BBox bbox() const { return triangle_bbox(a, b, c); }
    Vec3 center() const { return bbox().get_center(); }

    /// Returns the ray parameter of the intersection, or nullopt if the ray misses the triangle or
    /// the hit lies outside the ray's range.
    std::optional<float> hit(const Ray& ray) const {
        const auto e1 = b - a;
        const auto e2 = c - a;
        const auto p = bvh::v2::cross(ray.dir, e2);
        const auto det = bvh::v2::dot(e1, p);
        if (std::abs(det) < 1e-9f)
            return std::nullopt;
        const auto inv_det = 1.f / det;
        const auto t = ray.org - a;
        const auto u = bvh::v2::dot(t, p) * inv_det;
        if (u < 0.f || u > 1.f)
            return std::nullopt;
        const auto q = bvh::v2::cross(t, e1);
        const auto v = bvh::v2::dot(ray.dir, q) * inv_det;
        if (v < 0.f || u + v > 1.f)
            return std::nullopt;
        const auto dist = bvh::v2::dot(e2, q) * inv_det;
        if (dist < ray.tmin || dist > ray.tmax)
            return std::nullopt;
        return dist;
    }
};

/// Independently checks each live triangle against the ray, returning the closest hit (or
/// `SIZE_MAX` if there is none). This is the oracle used by several test cases.
std::pair<size_t, float> brute_force(const std::map<size_t, Triangle>& tris, const Ray& ray) {
    size_t best_id = SIZE_MAX;
    float best_t = ray.tmax;
    for (const auto& [id, tri] : tris) {
        auto t = tri.hit(ray);
        if (!t)
            continue;
        if (*t <= best_t) {
            best_t = *t;
            best_id = id;
        }
    }
    return { best_id, best_t };
}

/// Traverses using the BVH's `prim_ids` indirection (the contract exposed by the C API).
std::pair<size_t, float> trace(const Bvh& bvh, const std::map<size_t, Triangle>& tris, Ray ray) {
    size_t best_id = SIZE_MAX;
    if (bvh.nodes.empty())
        return { best_id, ray.tmax };
    bvh::v2::SmallStack<Node::Index, 64> stack;
    bvh.intersect<false, true>(ray, bvh.get_root().index, stack,
        [&](size_t begin, size_t end) {
            bool was_hit = false;
            for (size_t i = begin; i < end; ++i) {
                const auto id = bvh.prim_ids[i];
                auto found = tris.find(id);
                if (found == tris.end())
                    return false;
                if (auto t = found->second.hit(ray); t && *t <= ray.tmax) {
                    ray.tmax = *t;
                    best_id = id;
                    was_hit = true;
                }
            }
            return was_hit;
        });
    return { best_id, ray.tmax };
}

std::map<size_t, BBox> live_boxes(const std::map<size_t, Triangle>& tris) {
    std::map<size_t, BBox> result;
    for (const auto& [id, tri] : tris)
        result.emplace(id, tri.bbox());
    return result;
}

/// Validates the structural invariants maintained by editable mode.
int validate_structure(const Bvh& bvh, const std::map<size_t, BBox>& boxes, const char** why) {
    const size_t node_count = bvh.nodes.size();
    if (node_count == 0) {
        if (!bvh.prim_ids.empty() || !boxes.empty()) {
            *why = "empty tree must have no nodes and no primitive slots";
            return 0;
        }
        return 1;
    }

    std::vector<size_t> parents(node_count, SIZE_MAX);
    std::vector<bool> reached(node_count, false);
    parents[0] = 0;
    std::vector<size_t> pending { 0 };
    size_t visited = 0;
    while (!pending.empty()) {
        const size_t node_id = pending.back();
        pending.pop_back();
        if (node_id >= node_count) {
            *why = "child index out of bounds";
            return 0;
        }
        if (reached[node_id]) {
            *why = "node reached more than once";
            return 0;
        }
        reached[node_id] = true;
        ++visited;
        const auto& node = bvh.nodes[node_id];
        if (node.is_leaf())
            continue;
        const size_t left = node.index.first_id();
        if (left + 1 >= node_count) {
            *why = "child pair out of bounds";
            return 0;
        }
        parents[left + 0] = node_id;
        parents[left + 1] = node_id;
        pending.push_back(left + 0);
        pending.push_back(left + 1);
    }
    if (visited != node_count) {
        *why = "some nodes are unreachable from the root";
        return 0;
    }

    std::set<size_t> ids;
    std::vector<bool> slot_used(bvh.prim_ids.size(), false);
    for (size_t i = 0; i < node_count; ++i) {
        const auto& node = bvh.nodes[i];
        if (node.is_leaf()) {
            const size_t first = node.index.first_id();
            const size_t count = static_cast<size_t>(node.index.prim_count());
            if (count == 0 || first + count > bvh.prim_ids.size()) {
                *why = "invalid leaf range";
                return 0;
            }
            BBox leaf_box = BBox::make_empty();
            for (size_t j = first; j < first + count; ++j) {
                if (slot_used[j]) {
                    *why = "slot referenced by two leaves";
                    return 0;
                }
                slot_used[j] = true;
                const size_t id = bvh.prim_ids[j];
                auto it = boxes.find(id);
                if (it == boxes.end() || !ids.insert(id).second) {
                    *why = "leaf references an unknown or duplicate primitive ID";
                    return 0;
                }
                leaf_box.extend(it->second);
            }
            if (!bbox_equal(leaf_box, node.get_bbox())) {
                *why = "leaf box is not the union of its primitive boxes";
                return 0;
            }
        } else {
            const size_t left = node.index.first_id();
            BBox inner_box = bvh.nodes[left].get_bbox();
            inner_box.extend(bvh.nodes[left + 1].get_bbox());
            if (!bbox_equal(inner_box, node.get_bbox())) {
                *why = "inner box is not the union of its children's boxes";
                return 0;
            }
        }
        if (i != 0) {
            const size_t parent = parents[i];
            if (parent == SIZE_MAX || bvh.nodes[parent].is_leaf()) {
                *why = "parent link broken";
                return 0;
            }
            if (bvh.nodes[parent].index.first_id() != Bvh::get_left_sibling_id(i)) {
                *why = "node does not belong to a contiguous sibling pair";
                return 0;
            }
        }
    }
    if (ids.size() != boxes.size()) {
        *why = "wrong number of active primitive IDs";
        return 0;
    }
    return 1;
}

/// Runs the standard structural checks and reports failures through TASK_CHECK. `label` is only
/// used to keep the first failing check informative.
int check_tree(const Bvh& bvh, const std::map<size_t, Triangle>& tris) {
    const char* why = "unknown";
    TASK_CHECK(validate_structure(bvh, live_boxes(tris), &why) || (std::fprintf(stderr, "%s\n", why), 0));
    return 0;
}

//// Scene construction ----------------------------------------------------------------------------

/// Builds a BVH from the triangles in `tris`. Every triangle uses its external ID as the builder
/// primitive index, and the boxes are supplied by ID.
Bvh build_tree(const std::map<size_t, Triangle>& tris, size_t max_leaf_size = max_leaf) {
    std::vector<BBox> boxes;
    std::vector<Vec3> centers;
    for (const auto& [id, tri] : tris) {
        boxes.push_back(tri.bbox());
        centers.push_back(tri.center());
    }
    bvh::v2::DefaultBuilder<Node>::Config config;
    config.quality = bvh::v2::DefaultBuilder<Node>::Quality::Low;
    config.max_leaf_size = max_leaf_size;
    return bvh::v2::DefaultBuilder<Node>::build(boxes, centers, config);
}

/// Returns the boxes of `tris` indexed by external ID, as required by `enable_edits`.
std::vector<BBox> boxes_by_id(const std::map<size_t, Triangle>& tris) {
    std::vector<BBox> boxes;
    for (const auto& [id, tri] : tris)
        boxes.push_back(tri.bbox());
    return boxes;
}

double stats_total(const bvh::v2::EditStats& stats) {
    return static_cast<double>(stats.node_visits) + static_cast<double>(stats.primitive_visits);
}

//// Test cases ------------------------------------------------------------------------------------

/// Inserts an occluding triangle in front of the currently closest one and checks that the query
/// finds it, both against a brute-force oracle and against an explicit expected ID.
int case_insert_occluder(void) {
    std::map<size_t, Triangle> tris;
    tris.emplace(0, Triangle { Vec3(-1, -1, 4), Vec3(1, -1, 4), Vec3(0, 1, 4) });
    tris.emplace(1, Triangle { Vec3(-3, -1, 7), Vec3(-1, -1, 7), Vec3(-2, 1, 7) });
    auto bvh = build_tree(tris);
    TASK_CHECK(bvh.enable_edits(boxes_by_id(tris)));
    TASK_CHECK(check_tree(bvh, tris) == 0);

    Ray ray { Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 100 };
    TASK_CHECK(trace(bvh, tris, ray).first == 0);

    const size_t occluder_id = 17;
    Triangle occluder { Vec3(-1, -1, 2), Vec3(1, -1, 2), Vec3(0, 1, 2) };
    TASK_CHECK(bvh.insert_primitive(occluder_id, occluder.bbox()));
    tris.emplace(occluder_id, occluder);
    TASK_CHECK(check_tree(bvh, tris) == 0);

    const auto expected = brute_force(tris, ray);
    const auto actual = trace(bvh, tris, ray);
    TASK_CHECK(expected.first == occluder_id);
    TASK_CHECK(actual.first == expected.first);
    TASK_CHECK(std::abs(actual.second - expected.second) < 1e-5f);
    TASK_CHECK(bvh.last_edit_stats().primitive_visits >= 1);
    TASK_CHECK(bvh.last_edit_stats().node_visits >= 1);
    return 0;
}

/// Erases the closest triangle and verifies the next one becomes visible; also checks the erased
/// ID cannot be erased again until it is reinserted.
int case_erase_nearest(void) {
    std::map<size_t, Triangle> tris;
    tris.emplace(0, Triangle { Vec3(-1, -1, 2), Vec3(1, -1, 2), Vec3(0, 1, 2) });
    tris.emplace(1, Triangle { Vec3(-2, -2, 5), Vec3(2, -2, 5), Vec3(0, 2, 5) });
    tris.emplace(2, Triangle { Vec3(-2, -2, 9), Vec3(2, -2, 9), Vec3(0, 2, 9) });
    auto bvh = build_tree(tris);
    TASK_CHECK(bvh.enable_edits(boxes_by_id(tris)));

    Ray ray { Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 100 };
    TASK_CHECK(trace(bvh, tris, ray).first == 0);
    TASK_CHECK(bvh.erase_primitive(0));
    tris.erase(0);
    TASK_CHECK(bvh.last_edit_stats().primitive_visits >= 1);
    TASK_CHECK(check_tree(bvh, tris) == 0);

    const auto expected = brute_force(tris, ray);
    const auto actual = trace(bvh, tris, ray);
    TASK_CHECK(expected.first == 1);
    TASK_CHECK(actual.first == expected.first);
    TASK_CHECK(std::abs(actual.second - expected.second) < 1e-5f);

    TASK_CHECK(!bvh.erase_primitive(0));
    TASK_CHECK(trace(bvh, tris, ray).first == 1);

    Triangle back { Vec3(-1, -1, 1), Vec3(1, -1, 1), Vec3(0, 1, 1) };
    TASK_CHECK(bvh.insert_primitive(0, back.bbox()));
    tris.emplace(0, back);
    TASK_CHECK(trace(bvh, tris, ray).first == 0);
    TASK_CHECK(check_tree(bvh, tris) == 0);
    return 0;
}

/// Sparse IDs: IDs need not be contiguous, must be unique, and invalid operations fail without
/// modifying the tree.
int case_sparse_ids(void) {
    // At enable time, boxes are given for the original primitive numbers 0..N-1.
    std::map<size_t, Triangle> tris;
    tris.emplace(0, Triangle { Vec3(-1, -1, 1), Vec3(1, -1, 1), Vec3(0, 1, 1) });
    tris.emplace(1, Triangle { Vec3(-1, -1, 3), Vec3(1, -1, 3), Vec3(0, 1, 3) });
    tris.emplace(2, Triangle { Vec3(-1, -1, 5), Vec3(1, -1, 5), Vec3(0, 1, 5) });
    auto bvh = build_tree(tris);
    TASK_CHECK(bvh.enable_edits(boxes_by_id(tris)));
    TASK_CHECK(check_tree(bvh, tris) == 0);

    Ray ray { Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 100 };
    TASK_CHECK(trace(bvh, tris, ray).first == 0);

    // After enabling, external IDs may be sparse.
    Triangle sparse_near { Vec3(-0.5f, -0.5f, 0.5f), Vec3(0.5f, -0.5f, 0.5f), Vec3(0, 0.5f, 0.5f) };
    TASK_CHECK(bvh.insert_primitive(91, sparse_near.bbox()));
    tris.emplace(91, sparse_near);
    Triangle far { Vec3(30, 30, 2), Vec3(31, 30, 2), Vec3(30.5f, 31, 2) };
    TASK_CHECK(bvh.insert_primitive(100000, far.bbox()));
    tris.emplace(100000, far);
    TASK_CHECK(check_tree(bvh, tris) == 0);
    TASK_CHECK(trace(bvh, tris, ray).first == 91);

    // Inserting an existing ID fails and leaves the content unchanged.
    const auto before_nodes = bvh.nodes.size();
    const auto before_ids = bvh.prim_ids.size();
    TASK_CHECK(!bvh.insert_primitive(91, make_box(0, 0, 2)));
    TASK_CHECK(!bvh.erase_primitive(1234));
    TASK_CHECK(!bvh.insert_primitive(static_cast<size_t>(1) << 31, make_box(0, 0, 2)));
    TASK_CHECK(bvh.nodes.size() == before_nodes && bvh.prim_ids.size() == before_ids);
    TASK_CHECK(check_tree(bvh, tris) == 0);

    // A sparse ID can be erased and reinserted, including with a different box.
    TASK_CHECK(bvh.erase_primitive(91));
    tris.erase(91);
    TASK_CHECK(trace(bvh, tris, ray).first == 0);
    Triangle reused { Vec3(-1, -1, 0.25f), Vec3(1, -1, 0.25f), Vec3(0, 1, 0.25f) };
    TASK_CHECK(bvh.insert_primitive(91, reused.bbox()));
    tris.emplace(91, reused);
    TASK_CHECK(check_tree(bvh, tris) == 0);
    TASK_CHECK(trace(bvh, tris, ray).first == 91);
    TASK_CHECK(brute_force(tris, ray).first == trace(bvh, tris, ray).first);
    return 0;
}

/// Multi-primitive leaves: growing and shrinking leaves, and leaving a leaf with one primitive.
int case_multi_primitive_leaves(void) {
    std::map<size_t, Triangle> tris;
    for (size_t i = 0; i < 9; ++i) {
        const float z = 2.f + static_cast<float>(i);
        const float x = static_cast<float>(i % 3) - 1.f;
        const float y = static_cast<float>(i / 3) - 1.f;
        tris.emplace(i, Triangle {
            Vec3(x, y, z), Vec3(x + 0.8f, y, z), Vec3(x + 0.4f, y + 0.8f, z) });
    }
    auto bvh = build_tree(tris, 8);
    TASK_CHECK(bvh.enable_edits(boxes_by_id(tris)));
    TASK_CHECK(check_tree(bvh, tris) == 0);

    // Force a leaf to hold several primitives, then grow it past the capacity-1 leaves by
    // inserting nearby primitives until a split occurs.
    size_t next_id = 9;
    for (int i = 0; i < 16; ++i) {
        Triangle tri { Vec3(4.f + i * 0.1f, 4.f, 2.f), Vec3(4.5f + i * 0.1f, 4.f, 2.f),
                       Vec3(4.2f + i * 0.1f, 4.5f, 2.f) };
        TASK_CHECK(bvh.insert_primitive(next_id, tri.bbox()));
        tris.emplace(next_id, tri);
        ++next_id;
        TASK_CHECK(check_tree(bvh, tris) == 0);
    }

    // Erase most of the cluster, then verify the remaining primitives are still found and that
    // the leaves shrink as expected.
    for (size_t id = 9; id < next_id; ++id) {
        if (id % 3 != 0)
            continue;
        TASK_CHECK(bvh.erase_primitive(id));
        tris.erase(id);
    }
    TASK_CHECK(check_tree(bvh, tris) == 0);

    Ray ray { Vec3(4.2f, 4.1f, 0), Vec3(0, 0, 1), 0, 100 };
    TASK_CHECK(trace(bvh, tris, ray).first == brute_force(tris, ray).first);
    return 0;
}

/// Root shrinking: erase primitives so that the tree collapses to a root leaf, then to empty, and
/// check the node count follows.
int case_root_shrink(void) {
    std::map<size_t, Triangle> tris;
    tris.emplace(0, Triangle { Vec3(-1, -1, 2), Vec3(1, -1, 2), Vec3(0, 1, 2) });
    tris.emplace(1, Triangle { Vec3(-1, -1, 4), Vec3(1, -1, 4), Vec3(0, 1, 4) });
    tris.emplace(2, Triangle { Vec3(-1, -1, 6), Vec3(1, -1, 6), Vec3(0, 1, 6) });
    tris.emplace(3, Triangle { Vec3(-1, -1, 8), Vec3(1, -1, 8), Vec3(0, 1, 8) });
    auto bvh = build_tree(tris);
    TASK_CHECK(bvh.enable_edits(boxes_by_id(tris)));
    TASK_CHECK(bvh.nodes.size() >= 5);

    TASK_CHECK(bvh.erase_primitive(3));
    tris.erase(3);
    TASK_CHECK(check_tree(bvh, tris) == 0);
    TASK_CHECK(bvh.erase_primitive(2));
    tris.erase(2);
    TASK_CHECK(check_tree(bvh, tris) == 0);
    TASK_CHECK(bvh.erase_primitive(1));
    tris.erase(1);
    TASK_CHECK(check_tree(bvh, tris) == 0);

    // One primitive left: the root itself must become a leaf.
    TASK_CHECK(bvh.nodes.size() == 1);
    TASK_CHECK(bvh.nodes[0].is_leaf());
    TASK_CHECK(bvh.erase_primitive(0));
    tris.erase(0);
    TASK_CHECK(bvh.nodes.empty());
    TASK_CHECK(bvh.prim_ids.empty());
    TASK_CHECK(check_tree(bvh, tris) == 0);

    // Queries on the empty tree must not invoke the callback, including with the native
    // `Index{0}` start index.
    Ray ray { Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 100 };
    bvh::v2::SmallStack<Node::Index, 64> stack;
    size_t calls = 0;
    bvh.template intersect<false, true>(ray, Node::Index(0), stack, [&](size_t, size_t) {
        ++calls;
        return false;
    });
    TASK_CHECK(calls == 0);
    TASK_CHECK(trace(bvh, tris, ray).first == SIZE_MAX);

    // Insertion after emptying restores a working tree.
    Triangle revived { Vec3(-1, -1, 3), Vec3(1, -1, 3), Vec3(0, 1, 3) };
    TASK_CHECK(bvh.insert_primitive(0, revived.bbox()));
    tris.emplace(0, revived);
    TASK_CHECK(check_tree(bvh, tris) == 0);
    TASK_CHECK(trace(bvh, tris, ray).first == 0);

    // The original refit() entry point keeps working on the edited tree.
    bvh.refit([&](Node& node) {
        BBox box = BBox::make_empty();
        for (size_t i = node.index.first_id(); i < node.index.first_id() + node.index.prim_count(); ++i)
            box.extend(tris.at(bvh.prim_ids[i]).bbox());
        node.set_bbox(box);
    });
    TASK_CHECK(check_tree(bvh, tris) == 0);

    // `traverse_bottom_up` still visits every node of the edited tree.
    size_t visited_nodes = 0;
    bvh.traverse_bottom_up(
        [&](Node&) { ++visited_nodes; },
        [&](Node&) { ++visited_nodes; });
    TASK_CHECK(visited_nodes == bvh.nodes.size());
    return 0;
}

/// Identity across many operations: every active ID appears exactly once, and every query result
/// matches an independent brute-force pass.
int case_identity_random_edits(void) {
    std::map<size_t, Triangle> tris;
    std::vector<size_t> free_ids;
    for (size_t i = 0; i < 48; ++i) {
        const float x = static_cast<float>((i * 7) % 11) - 5.f;
        const float y = static_cast<float>((i * 5) % 9) - 4.f;
        const float z = 2.f + static_cast<float>((i * 13) % 23);
        tris.emplace(i, Triangle {
            Vec3(x, y, z), Vec3(x + 0.6f, y, z), Vec3(x + 0.3f, y + 0.6f, z) });
    }
    auto bvh = build_tree(tris);
    TASK_CHECK(bvh.enable_edits(boxes_by_id(tris)));

    size_t next_id = 48;
    unsigned state = 0x9e3779b9u;
    auto next_random = [&] {
        state = state * 1664525u + 1013904223u;
        return state;
    };
    for (int step = 0; step < 600; ++step) {
        const bool do_insert = tris.empty() || (next_random() % 3u) != 0u;
        if (do_insert) {
            const size_t id = (next_random() % 4u == 0u) ? next_id : (next_random() % (next_id + 16));
            if (tris.count(id))
                continue;
            const float x = static_cast<float>(next_random() % 2000) * 0.02f - 20.f;
            const float y = static_cast<float>(next_random() % 2000) * 0.02f - 20.f;
            const float z = 2.f + static_cast<float>(next_random() % 3000) * 0.01f;
            Triangle tri { Vec3(x, y, z), Vec3(x + 0.7f, y, z), Vec3(x + 0.35f, y + 0.7f, z) };
            TASK_CHECK(bvh.insert_primitive(id, tri.bbox()));
            tris.emplace(id, tri);
            if (id == next_id)
                ++next_id;
        } else {
            auto it = tris.begin();
            std::advance(it, static_cast<long>(next_random() % tris.size()));
            const size_t id = it->first;
            TASK_CHECK(bvh.erase_primitive(id));
            tris.erase(it);
        }
        if (tris.empty()) {
            TASK_CHECK(bvh.nodes.empty());
            TASK_CHECK(bvh.prim_ids.empty());
            continue;
        }
        TASK_CHECK(check_tree(bvh, tris) == 0);
        if (step % 13 == 0) {
            Ray ray { Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 1000 };
            TASK_CHECK(trace(bvh, tris, ray).first == brute_force(tris, ray).first);
        }
    }

    // Storage must stay proportional to the live set rather than to the number of edits.
    TASK_CHECK(bvh.prim_ids.size() <= tris.size() + 32);
    TASK_CHECK(bvh.nodes.size() <= 2 * tris.size() + 2);
    return 0;
}

/// Persistence: an editable tree saves and reloads its mode, active IDs and boxes, and edits can
/// continue after loading. Two loaded handles evolve independently.
int case_persistence_editable(void) {
    std::map<size_t, Triangle> tris;
    tris.emplace(0, Triangle { Vec3(-1, -1, 2), Vec3(1, -1, 2), Vec3(0, 1, 2) });
    tris.emplace(1, Triangle { Vec3(-1, -1, 4), Vec3(1, -1, 4), Vec3(0, 1, 4) });
    tris.emplace(2, Triangle { Vec3(-1, -1, 6), Vec3(1, -1, 6), Vec3(0, 1, 6) });
    auto bvh = build_tree(tris);
    TASK_CHECK(bvh.enable_edits(boxes_by_id(tris)));

    Triangle extra { Vec3(-1, -1, 1), Vec3(1, -1, 1), Vec3(0, 1, 1) };
    TASK_CHECK(bvh.insert_primitive(21, extra.bbox()));
    tris.emplace(21, extra);

    std::stringstream bytes(std::ios::in | std::ios::out | std::ios::binary);
    bvh::v2::StdOutputStream out(bytes);
    bvh.serialize(out);
    bytes.seekg(0);
    bvh::v2::StdInputStream in(bytes);
    auto restored = Bvh::deserialize(in);

    TASK_CHECK(restored.edit_state.has_value());
    TASK_CHECK(check_tree(restored, tris) == 0);
    Ray ray { Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 100 };
    TASK_CHECK(trace(restored, tris, ray).first == 21);

    // Editing continues after load without calling enable_edits again.
    restored.erase_primitive(21);
    tris.erase(21);
    TASK_CHECK(check_tree(restored, tris) == 0);
    TASK_CHECK(trace(restored, tris, ray).first == 0);

    // The original handle is unaffected by edits to the restored one.
    TASK_CHECK(trace(bvh, std::map<size_t, Triangle> {
        { 0, Triangle { Vec3(-1, -1, 2), Vec3(1, -1, 2), Vec3(0, 1, 2) } },
        { 1, Triangle { Vec3(-1, -1, 4), Vec3(1, -1, 4), Vec3(0, 1, 4) } },
        { 2, Triangle { Vec3(-1, -1, 6), Vec3(1, -1, 6), Vec3(0, 1, 6) } },
        { 21, extra } }, ray).first == 21);

    // A branch edit on a second, independent copy of the restored handle.
    std::stringstream again(std::ios::in | std::ios::out | std::ios::binary);
    bvh::v2::StdOutputStream out2(again);
    restored.serialize(out2);
    again.seekg(0);
    bvh::v2::StdInputStream in2(again);
    auto branch = Bvh::deserialize(in2);
    TASK_CHECK(branch.edit_state.has_value());
    Triangle branch_tri { Vec3(-1, -1, 1.5f), Vec3(1, -1, 1.5f), Vec3(0, 1, 1.5f) };
    TASK_CHECK(branch.insert_primitive(77, branch_tri.bbox()));
    auto branch_tris = tris;
    branch_tris.emplace(77, branch_tri);
    TASK_CHECK(check_tree(branch, branch_tris) == 0);
    TASK_CHECK(trace(branch, branch_tris, ray).first == 77);
    TASK_CHECK(trace(restored, tris, ray).first == 0);
    return 0;
}

/// Persistence of a non-editable tree keeps the original format, and the legacy static file still
/// loads. A loaded tree can also be switched to editable mode.
int case_persistence_legacy(void) {
    std::map<size_t, Triangle> tris;
    tris.emplace(0, Triangle { Vec3(-1, -1, 2), Vec3(1, -1, 2), Vec3(0, 1, 2) });
    tris.emplace(1, Triangle { Vec3(-1, -1, 4), Vec3(1, -1, 4), Vec3(0, 1, 4) });
    auto bvh = build_tree(tris);

    std::stringstream bytes(std::ios::in | std::ios::out | std::ios::binary);
    bvh::v2::StdOutputStream out(bytes);
    bvh.serialize(out);
    bytes.seekg(0);
    bvh::v2::StdInputStream in(bytes);
    auto restored = Bvh::deserialize(in);
    TASK_CHECK(!restored.edit_state.has_value());
    TASK_CHECK(restored == bvh);

    Ray ray { Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 100 };
    TASK_CHECK(trace(restored, tris, ray).first == 0);
    TASK_CHECK(restored.enable_edits(boxes_by_id(tris)));
    TASK_CHECK(restored.erase_primitive(0));
    tris.erase(0);
    TASK_CHECK(check_tree(restored, tris) == 0);
    TASK_CHECK(trace(restored, tris, ray).first == 1);

    // The bundled legacy file has the original layout (node count first).
    std::FILE* file = std::fopen("checks/legacy-static.bvh", "rb");
    TASK_CHECK(file != nullptr);
    struct FileInputStream : bvh::v2::InputStream {
        std::FILE* file;
        explicit FileInputStream(std::FILE* f) : file(f) {}
        size_t read_raw(void* data, size_t size) override {
            return std::fread(data, 1, size, file);
        }
    };
    FileInputStream stream { file };
    auto legacy = Bvh::deserialize(stream);
    std::fclose(file);
    TASK_CHECK(!legacy.edit_state.has_value());
    TASK_CHECK(legacy.nodes.size() == 3);
    TASK_CHECK(legacy.prim_ids.size() == 2);
    TASK_CHECK(legacy.prim_ids[0] == 0 && legacy.prim_ids[1] == 1);
    return 0;
}

/// Locality: a large, clustered scene is built, then a single local cluster is edited. The work
/// counters must stay far below the total scene size, proving the edit does not scan the whole
/// tree or all primitive boxes.
int case_locality_clustered(void) {
    constexpr size_t cluster_count = 64;
    constexpr size_t per_cluster = 32;
    std::map<size_t, Triangle> tris;
    for (size_t c = 0; c < cluster_count; ++c) {
        const float cx = static_cast<float>(c % 8) * 40.f;
        const float cy = static_cast<float>(c / 8) * 40.f;
        for (size_t i = 0; i < per_cluster; ++i) {
            const size_t id = c * per_cluster + i;
            const float x = cx + static_cast<float>(i % 8) * 0.5f;
            const float y = cy + static_cast<float>(i / 8) * 0.5f;
            const float z = 2.f + static_cast<float>(i % 5) * 0.25f;
            tris.emplace(id, Triangle {
                Vec3(x, y, z), Vec3(x + 0.4f, y, z), Vec3(x + 0.2f, y + 0.4f, z) });
        }
    }
    auto bvh = build_tree(tris, 4);
    TASK_CHECK(bvh.enable_edits(boxes_by_id(tris)));
    TASK_CHECK(check_tree(bvh, tris) == 0);

    const size_t total_prims = tris.size();
    const size_t total_nodes = bvh.nodes.size();

    // Insert into a single cluster: the insertion path only touches one root-to-leaf path.
    Triangle local { Vec3(1.f, 1.f, 1.5f), Vec3(1.4f, 1.f, 1.5f), Vec3(1.2f, 1.4f, 1.5f) };
    const size_t local_id = 1000000;
    TASK_CHECK(bvh.insert_primitive(local_id, local.bbox()));
    auto insert_stats = bvh.last_edit_stats();
    TASK_CHECK(insert_stats.node_visits < total_nodes / 4);
    TASK_CHECK(insert_stats.primitive_visits <= 2);

    tris.emplace(local_id, local);
    TASK_CHECK(check_tree(bvh, tris) == 0);

    // Erase a primitive from that same cluster: only its leaf and ancestors are touched.
    const size_t victim = 3;
    TASK_CHECK(bvh.erase_primitive(victim));
    auto erase_stats = bvh.last_edit_stats();
    tris.erase(victim);
    TASK_CHECK(erase_stats.node_visits < total_nodes / 4);
    TASK_CHECK(erase_stats.primitive_visits < total_prims / 4);
    TASK_CHECK(stats_total(erase_stats) < static_cast<double>(total_prims) / 4.0);
    TASK_CHECK(check_tree(bvh, tris) == 0);

    // The edit's work does not scale with the scene: compare against a much bigger scene.
    std::map<size_t, Triangle> big = tris;
    for (size_t c = cluster_count; c < cluster_count * 4; ++c) {
        const float cx = static_cast<float>(c % 8) * 40.f;
        const float cy = static_cast<float>(c / 8) * 40.f;
        for (size_t i = 0; i < per_cluster; ++i) {
            const size_t id = c * per_cluster + i;
            const float x = cx + static_cast<float>(i % 8) * 0.5f;
            const float y = cy + static_cast<float>(i / 8) * 0.5f;
            const float z = 2.f + static_cast<float>(i % 5) * 0.25f;
            big.emplace(id, Triangle {
                Vec3(x, y, z), Vec3(x + 0.4f, y, z), Vec3(x + 0.2f, y + 0.4f, z) });
        }
    }
    auto big_bvh = build_tree(big, 4);
    TASK_CHECK(big_bvh.enable_edits(boxes_by_id(big)));
    const size_t big_nodes = big_bvh.nodes.size();
    const size_t big_prims = big.size();
    TASK_CHECK(big_bvh.insert_primitive(local_id + 1, local.bbox()));
    auto big_stats = big_bvh.last_edit_stats();
    // The edit cost tracks the depth of one root-to-leaf path, not the scene size: it must stay a
    // small fraction of the much larger tree, and close to the small-scene cost.
    TASK_CHECK(big_stats.node_visits < big_nodes / 8);
    TASK_CHECK(big_stats.node_visits <= insert_stats.node_visits + 16);
    TASK_CHECK(big_stats.primitive_visits <= 2);
    TASK_CHECK(stats_total(big_stats) < static_cast<double>(big_prims) / 8.0);
    return 0;
}

/// Clears a tree completely, reinserts a primitive, and checks queries and structure recover.
int case_clear_and_reinsert(void) {
    std::map<size_t, Triangle> tris;
    for (size_t i = 0; i < 6; ++i) {
        const float z = 2.f + static_cast<float>(i);
        tris.emplace(i, Triangle {
            Vec3(-1, -1, z), Vec3(1, -1, z), Vec3(0, 1, z) });
    }
    auto bvh = build_tree(tris);
    TASK_CHECK(bvh.enable_edits(boxes_by_id(tris)));
    for (size_t i = 0; i < 6; ++i) {
        TASK_CHECK(bvh.erase_primitive(i));
        tris.erase(i);
        TASK_CHECK(check_tree(bvh, tris) == 0);
    }
    TASK_CHECK(bvh.nodes.empty() && bvh.prim_ids.empty());

    Ray ray { Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 100 };
    TASK_CHECK(trace(bvh, tris, ray).first == SIZE_MAX);
    Triangle revived { Vec3(-1, -1, 7), Vec3(1, -1, 7), Vec3(0, 1, 7) };
    TASK_CHECK(bvh.insert_primitive(42, revived.bbox()));
    tris.emplace(42, revived);
    TASK_CHECK(check_tree(bvh, tris) == 0);
    TASK_CHECK(trace(bvh, tris, ray).first == 42);
    TASK_CHECK(bvh.nodes.size() == 1);

    // Growing the single-primitive root leaf keeps a valid topology.
    Triangle second { Vec3(-1, -1, 9), Vec3(1, -1, 9), Vec3(0, 1, 9) };
    TASK_CHECK(bvh.insert_primitive(43, second.bbox()));
    tris.emplace(43, second);
    TASK_CHECK(check_tree(bvh, tris) == 0);
    TASK_CHECK(trace(bvh, tris, ray).first == 42);
    return 0;
}

/// Misuse: operations before enabling fail and leave contents unchanged, and enabling twice fails.
int case_not_enabled(void) {
    std::map<size_t, Triangle> tris;
    tris.emplace(0, Triangle { Vec3(-1, -1, 2), Vec3(1, -1, 2), Vec3(0, 1, 2) });
    auto bvh = build_tree(tris);
    const auto nodes_before = bvh.nodes.size();
    const auto ids_before = bvh.prim_ids.size();
    TASK_CHECK(!bvh.insert_primitive(1, make_box(0, 0, 3)));
    TASK_CHECK(!bvh.erase_primitive(0));
    TASK_CHECK(bvh.nodes.size() == nodes_before && bvh.prim_ids.size() == ids_before);
    TASK_CHECK(bvh.last_edit_stats().node_visits == 0);
    TASK_CHECK(bvh.last_edit_stats().primitive_visits == 0);

    TASK_CHECK(bvh.enable_edits(boxes_by_id(tris)));
    TASK_CHECK(!bvh.enable_edits(boxes_by_id(tris)));
    TASK_CHECK(bvh.erase_primitive(0));
    tris.erase(0);

    // An empty Bvh can be enabled with an empty array and then used.
    Bvh empty;
    TASK_CHECK(empty.enable_edits({}));
    TASK_CHECK(empty.nodes.empty());
    Triangle tri { Vec3(-1, -1, 5), Vec3(1, -1, 5), Vec3(0, 1, 5) };
    TASK_CHECK(empty.insert_primitive(7, tri.bbox()));
    std::map<size_t, Triangle> only { { 7, tri } };
    TASK_CHECK(check_tree(empty, only) == 0);
    Ray ray { Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 100 };
    TASK_CHECK(trace(empty, only, ray).first == 7);
    return 0;
}

/// Regression: erasing a leaf whose parent is stored in the last sibling pair must still leave a
/// consistent topology. The old code refit from a stale parent index in that case.
int case_compaction_regression(void) {
    // Build a tree whose rightmost subtree is deep, so that a leaf near the root has its parent in
    // the last pair after repeated edits force compaction.
    std::map<size_t, Triangle> tris;
    for (size_t i = 0; i < 12; ++i) {
        const float z = 2.f + static_cast<float>(i) * 0.5f;
        tris.emplace(i, Triangle {
            Vec3(-1, -1, z), Vec3(1, -1, z), Vec3(0, 1, z) });
    }
    auto bvh = build_tree(tris, 1);
    TASK_CHECK(bvh.enable_edits(boxes_by_id(tris)));

    // Erase the deepest primitives first, then the shallow ones; each step must stay valid.
    for (size_t i = 12; i-- > 0;) {
        TASK_CHECK(bvh.erase_primitive(i));
        tris.erase(i);
        TASK_CHECK(check_tree(bvh, tris) == 0);
    }
    TASK_CHECK(bvh.nodes.empty());
    TASK_CHECK(bvh.prim_ids.empty());
    return 0;
}

/// Repeatedly inserting and erasing the same small batch of primitives must not keep growing the
/// slot storage or the node array.
int case_bounded_storage(void) {
    std::map<size_t, Triangle> tris;
    for (size_t i = 0; i < 40; ++i) {
        const float x = static_cast<float>(i % 8);
        const float y = static_cast<float>(i / 8);
        tris.emplace(i, Triangle {
            Vec3(x, y, 2), Vec3(x + 0.5f, y, 2), Vec3(x + 0.25f, y + 0.5f, 2) });
    }
    auto bvh = build_tree(tris, 2);
    TASK_CHECK(bvh.enable_edits(boxes_by_id(tris)));
    const size_t baseline_slots = bvh.prim_ids.size();
    const size_t baseline_nodes = bvh.nodes.size();

    // Repeat a cycle where the batch is fully erased and reinserted, with fresh IDs every time.
    size_t next_id = 1000;
    for (int cycle = 0; cycle < 60; ++cycle) {
        std::vector<size_t> inserted;
        for (size_t i = 0; i < 8; ++i) {
            Triangle tri { Vec3(0, 0, 0.5f), Vec3(0.5f, 0, 0.5f), Vec3(0.25f, 0.5f, 0.5f) };
            const size_t id = next_id++;
            TASK_CHECK(bvh.insert_primitive(id, tri.bbox()));
            tris.emplace(id, tri);
            inserted.push_back(id);
        }
        TASK_CHECK(check_tree(bvh, tris) == 0);
        for (size_t id : inserted) {
            TASK_CHECK(bvh.erase_primitive(id));
            tris.erase(id);
        }
        TASK_CHECK(check_tree(bvh, tris) == 0);
    }

    // Storage must return to the baseline (within a small slack), not track the edit count.
    TASK_CHECK(bvh.prim_ids.size() <= baseline_slots + 16);
    TASK_CHECK(bvh.nodes.size() <= baseline_nodes + 8);
    // The active data is intact and queryable.
    Ray ray { Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 100 };
    TASK_CHECK(trace(bvh, tris, ray).first == brute_force(tris, ray).first);
    return 0;
}

} // namespace

extern const struct task_case task_cases[];
const struct task_case task_cases[] = {
    { "topology", "insert_occluder", case_insert_occluder },
    { "topology", "erase_nearest", case_erase_nearest },
    { "topology", "multi_primitive_leaves", case_multi_primitive_leaves },
    { "topology", "root_shrink", case_root_shrink },
    { "topology", "clear_and_reinsert", case_clear_and_reinsert },
    { "topology", "compaction_regression", case_compaction_regression },
    { "identity", "sparse_ids", case_sparse_ids },
    { "identity", "random_edits", case_identity_random_edits },
    { "identity", "not_enabled", case_not_enabled },
    { "identity", "bounded_storage", case_bounded_storage },
    { "persistence", "editable_roundtrip", case_persistence_editable },
    { "persistence", "legacy_and_original_format", case_persistence_legacy },
    { "locality", "clustered_scene", case_locality_clustered },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
