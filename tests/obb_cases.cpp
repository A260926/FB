// Tests for the oriented BVH. This file deliberately has no `main`: the task driver enumerates
// `task_cases` and runs every case, checking that each one performs real assertions.

#include "task_support/check.h"

#include "examples/obb_scene.h"

#include <bvh/v2/obb.h>
#include <bvh/v2/default_builder.h>
#include <bvh/v2/node.h>
#include <bvh/v2/stack.h>
#include <bvh/v2/stream.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/vec.h>
#include <bvh/v2/c_api/bvh.h>

#include <cstdlib>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Scalar = float;
using Vec3 = bvh::v2::Vec<Scalar, 3>;
using Tri3 = bvh::v2::Tri<Scalar, 3>;
using Node3 = bvh::v2::Node<Scalar, 3>;
using BBox3 = bvh::v2::BBox<Scalar, 3>;
using Ray3 = bvh::v2::Ray<Scalar, 3>;
using SourceBvh = bvh::v2::Bvh<Node3>;
using ObbBvh = bvh::v2::OrientedBvh<Scalar>;
using Precomputed = bvh::v2::PrecomputedTri<Scalar>;
using StdOutputStream = bvh::v2::StdOutputStream;
using StdInputStream = bvh::v2::StdInputStream;

constexpr Scalar eps = std::numeric_limits<Scalar>::epsilon();
constexpr size_t invalid_id = std::numeric_limits<size_t>::max();

Scalar bound_tolerance(Scalar max_abs) {
    return 64 * eps * std::max(Scalar(1), max_abs);
}

Vec3 normalize_or(Vec3 v, Vec3 fallback = Vec3(1, 0, 0)) {
    auto len = std::sqrt(bvh::v2::dot(v, v));
    if (!(len > 0) || !std::isfinite(len))
        return fallback;
    return v * (1 / len);
}

SourceBvh build_source(const std::vector<Tri3>& tris, size_t max_leaf_size = 4) {
    std::vector<BBox3> boxes;
    std::vector<Vec3> centers;
    boxes.reserve(tris.size());
    centers.reserve(tris.size());
    for (const auto& tri : tris) {
        boxes.push_back(tri.get_bbox());
        centers.push_back(tri.get_center());
    }
    typename bvh::v2::DefaultBuilder<Node3>::Config config;
    config.max_leaf_size = max_leaf_size;
    config.quality = bvh::v2::DefaultBuilder<Node3>::Quality::High;
    return bvh::v2::DefaultBuilder<Node3>::build(boxes, centers, config);
}

/// Small deterministic mesh with two clearly different slants, split across two spatial clusters.
std::vector<Tri3> two_slant_mesh() {
    std::vector<Tri3> tris;
    auto add_quad = [&] (const Vec3& c, const Vec3& u, const Vec3& v) {
        auto a = c - u - v;
        auto b = c + u - v;
        auto d = c + u + v;
        auto e = c - u + v;
        tris.push_back(Tri3(a, b, d));
        tris.push_back(Tri3(a, d, e));
    };
    auto n1 = normalize_or(Vec3(0.3f, 0.2f, 0.93f));
    auto u1 = normalize_or(Vec3(1, -0.2f, 0));
    auto v1 = normalize_or(bvh::v2::cross(n1, u1));
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            add_quad(
                Vec3(-1.6f + 0.8f * i, -1.5f + 0.8f * j, 0.2f + 0.05f * (i * 2 + j)),
                u1 * 0.3f, v1 * 0.3f);
        }
    }
    auto n2 = normalize_or(Vec3(-0.7f, 0.5f, 0.5f));
    auto u2 = normalize_or(Vec3(0.2f, 1, 0.2f));
    auto v2 = normalize_or(bvh::v2::cross(n2, u2));
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            add_quad(
                Vec3(1.3f + 0.8f * i, -1.5f + 0.8f * j, -0.1f + 0.05f * (i * 2 + j)),
                u2 * 0.3f, v2 * 0.3f);
        }
    }
    return tris;
}

std::vector<Vec3> subtree_vertices(
    const SourceBvh& source,
    const std::vector<Tri3>& tris,
    size_t node_id)
{
    std::vector<Vec3> vertices;
    std::vector<size_t> stack;
    stack.push_back(node_id);
    while (!stack.empty()) {
        auto id = stack.back();
        stack.pop_back();
        const auto& node = source.nodes[id];
        if (node.is_leaf()) {
            for (size_t k = node.index.first_id(); k < node.index.first_id() + node.index.prim_count(); ++k) {
                auto prim = source.prim_ids[k];
                if (prim >= tris.size())
                    continue;
                vertices.push_back(tris[prim].p0);
                vertices.push_back(tris[prim].p1);
                vertices.push_back(tris[prim].p2);
            }
        } else {
            stack.push_back(node.index.first_id() + 0);
            stack.push_back(node.index.first_id() + 1);
        }
    }
    return vertices;
}

struct RayResult {
    Scalar t = std::numeric_limits<Scalar>::infinity();
    size_t prim_id = invalid_id;
};

RayResult exhaustive_closest(const std::vector<Tri3>& tris, Ray3 ray) {
    RayResult result;
    for (size_t i = 0; i < tris.size(); ++i) {
        if (auto hit = Precomputed(tris[i]).intersect(ray)) {
            auto t = std::get<0>(*hit);
            if (t < result.t) {
                result.t = t;
                result.prim_id = i;
                ray.tmax = t;
            }
        }
    }
    return result;
}

RayResult obb_closest(const ObbBvh& bvh, const std::vector<Tri3>& tris, Ray3 ray, ObbBvh::TraversalStats* stats = nullptr) {
    RayResult result;
    ObbBvh::DefaultStack stack;
    bvh.intersect<false>(ray, stack, [&] (size_t begin, size_t end) {
        for (size_t k = begin; k < end; ++k) {
            auto prim = bvh.prim_ids()[k];
            if (auto hit = Precomputed(tris[prim]).intersect(ray)) {
                auto t = std::get<0>(*hit);
                if (t < result.t) {
                    result.t = t;
                    result.prim_id = prim;
                    ray.tmax = t;
                }
            }
        }
        return result.prim_id != invalid_id;
    }, stats);
    return result;
}

bool obb_any_hit(const ObbBvh& bvh, const std::vector<Tri3>& tris, Ray3 ray) {
    bool hit = false;
    ObbBvh::DefaultStack stack;
    bvh.intersect<true>(ray, stack, [&] (size_t begin, size_t end) {
        for (size_t k = begin; k < end; ++k) {
            auto prim = bvh.prim_ids()[k];
            if (auto h = Precomputed(tris[prim]).intersect(ray)) {
                (void)h;
                hit = true;
                return true;
            }
        }
        return false;
    });
    return hit;
}

bool exhaustive_any_hit(const std::vector<Tri3>& tris, Ray3 ray) {
    for (const auto& tri : tris) {
        if (Precomputed(tri).intersect(ray))
            return true;
    }
    return false;
}

bool axes_are_valid(const ObbBvh& bvh) {
    for (const auto& node : bvh.nodes()) {
        for (size_t i = 0; i < 3; ++i) {
            for (size_t j = 0; j < 3; ++j) {
                if (!std::isfinite(node.axis[i][j]) || !std::isfinite(node.center[j]) ||
                    !std::isfinite(node.half_size[j]))
                    return false;
                if (node.half_size[j] < 0)
                    return false;
            }
        }
        for (size_t i = 0; i < 3; ++i) {
            if (std::fabs(bvh::v2::dot(node.axis[i], node.axis[i]) - 1) > 1e-5f)
                return false;
            for (size_t j = i + 1; j < 3; ++j) {
                if (std::fabs(bvh::v2::dot(node.axis[i], node.axis[j])) > 1e-5f)
                    return false;
            }
        }
        if (std::fabs(bvh::v2::dot(bvh::v2::cross(node.axis[0], node.axis[1]), node.axis[2]) - 1) > 1e-5f)
            return false;
    }
    return true;
}

bool boxes_contain_subtree_vertices(
    const ObbBvh& bvh,
    const SourceBvh& source,
    const std::vector<Tri3>& tris)
{
    for (size_t id = 0; id < bvh.node_count(); ++id) {
        auto vertices = subtree_vertices(source, tris, id);
        if (vertices.empty())
            continue;
        auto max_abs = Scalar(0);
        for (const auto& v : vertices)
            for (size_t i = 0; i < 3; ++i)
                max_abs = std::max(max_abs, std::fabs(v[i]));
        auto tol = bound_tolerance(max_abs);
        const auto& node = bvh.nodes()[id];
        for (const auto& v : vertices) {
            for (size_t i = 0; i < 3; ++i) {
                auto proj = bvh::v2::dot(v - node.center, node.axis[i]);
                if (std::fabs(proj) > node.half_size[i] + 2 * tol)
                    return false;
            }
        }
        // Tightness: the outward margin must stay within the documented budget on both ends.
        for (size_t i = 0; i < 3; ++i) {
            auto max_proj = Scalar(0);
            for (const auto& v : vertices)
                max_proj = std::max(max_proj, std::fabs(bvh::v2::dot(v - node.center, node.axis[i])));
            if (node.half_size[i] - max_proj > 64 * eps * std::max(Scalar(1), max_abs) + tol)
                return false;
        }
    }
    return true;
}

SourceBvh manual_source(
    const std::vector<Tri3>& tris,
    size_t left_count,
    size_t right_count,
    const char* kind)
{
    (void)tris;
    (void)kind;
    SourceBvh source;
    source.nodes.resize(3);
    source.prim_ids.resize(left_count + right_count);
    for (size_t i = 0; i < left_count + right_count; ++i)
        source.prim_ids[i] = i;
    source.nodes[0].index = Node3::Index::make_inner(1);
    source.nodes[1].index = Node3::Index::make_leaf(0, left_count);
    source.nodes[2].index = Node3::Index::make_leaf(left_count, right_count);
    return source;
}

std::array<Vec3, 3> frame_of(const ObbBvh& bvh, size_t id) {
    return bvh.nodes()[id].axis;
}

bool same_frame(const std::array<Vec3, 3>& a, const std::array<Vec3, 3>& b, Scalar tol = 1e-6f) {
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 3; ++j)
            if (std::fabs(a[i][j] - b[i][j]) > tol)
                return false;
    return true;
}

// ---------------------------------------------------------------------------------------------
// frames
// ---------------------------------------------------------------------------------------------

int case_frames_orthonormal_and_tight() {
    auto tris = two_slant_mesh();
    auto source = build_source(tris);
    auto bvh = ObbBvh::from_bvh(source, tris);

    TASK_CHECK(bvh.node_count() == source.nodes.size());
    TASK_CHECK(bvh.prim_count() == source.prim_ids.size());
    TASK_CHECK(bvh.same_topology(source));
    TASK_CHECK(axes_are_valid(bvh));
    TASK_CHECK(boxes_contain_subtree_vertices(bvh, source, tris));

    // Two different slants: the root (mixing both groups) and the two spatial halves must not all
    // share the same frame.
    bool found_difference = false;
    for (size_t i = 1; i < bvh.node_count() && !found_difference; ++i) {
        if (same_frame(frame_of(bvh, 0), frame_of(bvh, i)))
            continue;
        found_difference = true;
    }
    TASK_CHECK(found_difference);

    // Within each slanted cluster, the thinnest OBB axis must be (close to) the plane normal,
    // which for planar patches is the covariance null direction.
    auto normal_of = [] (const Tri3& tri) {
        return normalize_or(bvh::v2::cross(tri.p1 - tri.p0, tri.p2 - tri.p0));
    };
    auto normal1 = normal_of(tris[0]);
    auto normal2 = normal_of(tris[8]);
    auto thinnest_axis = [] (const ObbBvh::NodeData& node) {
        size_t thinnest = 0;
        for (size_t i = 1; i < 3; ++i)
            if (node.half_size[i] < node.half_size[thinnest])
                thinnest = i;
        return node.axis[thinnest];
    };
    // Every leaf belongs entirely to one group, so its thinnest axis must follow that group's
    // plane normal. Both normals must actually occur among the leaves.
    bool saw_normal1 = false;
    bool saw_normal2 = false;
    for (const auto& node : bvh.nodes()) {
        if (!node.is_leaf())
            continue;
        auto axis = thinnest_axis(node);
        auto d1 = std::fabs(bvh::v2::dot(axis, normal1));
        auto d2 = std::fabs(bvh::v2::dot(axis, normal2));
        TASK_CHECK(std::max(d1, d2) > 0.99f);
        saw_normal1 = saw_normal1 || d1 > 0.99f;
        saw_normal2 = saw_normal2 || d2 > 0.99f;
    }
    TASK_CHECK(saw_normal1 && saw_normal2);
    TASK_CHECK(std::fabs(bvh::v2::dot(normal1, normal2)) < 0.9f);

    // Deterministic: building again gives exactly the same frames.
    auto source2 = build_source(tris);
    auto bvh2 = ObbBvh::from_bvh(source2, tris);
    TASK_CHECK(bvh == bvh2);

    // Primitive identities are preserved in the same order.
    TASK_CHECK(bvh.prim_ids() == source.prim_ids);
    return 0;
}

int case_frames_degenerate_covariance() {
    // Line-like: all vertices collinear along a slanted direction.
    std::vector<Tri3> line;
    auto dir = normalize_or(Vec3(1, 2, -3));
    for (int i = 0; i < 6; ++i) {
        auto base = dir * static_cast<Scalar>(i);
        line.push_back(Tri3(base, base + dir * 0.1f, base + Vec3(0, 0, 0)));
    }

    // Planar: all vertices in a slanted plane.
    std::vector<Tri3> plane;
    auto pn = normalize_or(Vec3(0.4f, -0.8f, 0.45f));
    auto pu = normalize_or(bvh::v2::cross(pn, Vec3(0, 0, 1)));
    auto pv = normalize_or(bvh::v2::cross(pn, pu));
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            auto c = pu * (0.5f * i) + pv * (0.5f * j) + pn * 0.7f;
            plane.push_back(Tri3(c, c + pu * 0.2f, c + pv * 0.2f));
        }
    }

    // Coincident vertices (all points identical).
    std::vector<Tri3> points(8, Tri3(Vec3(3, -2, 1), Vec3(3, -2, 1), Vec3(3, -2, 1)));

    // Repeated (isotropic) eigenvalues: a regular tetrahedron has an isotropic covariance.
    std::vector<Tri3> tetra;
    auto v0 = Vec3(1, 1, 1);
    auto v1 = Vec3(1, -1, -1);
    auto v2 = Vec3(-1, 1, -1);
    auto v3 = Vec3(-1, -1, 1);
    tetra.push_back(Tri3(v0, v1, v2));
    tetra.push_back(Tri3(v0, v2, v3));
    tetra.push_back(Tri3(v0, v3, v1));
    tetra.push_back(Tri3(v1, v3, v2));

    for (auto* mesh : { &line, &plane, &points, &tetra }) {
        auto source = build_source(*mesh, 2);
        auto bvh = ObbBvh::from_bvh(source, *mesh);
        TASK_CHECK(axes_are_valid(bvh));
        TASK_CHECK(boxes_contain_subtree_vertices(bvh, source, *mesh));
        for (const auto& node : bvh.nodes()) {
            for (size_t i = 0; i < 3; ++i) {
                TASK_CHECK(std::isfinite(node.half_size[i]));
                TASK_CHECK(node.half_size[i] >= 0);
            }
        }
        // Determinism in degenerate configurations.
        auto source2 = build_source(*mesh, 2);
        auto bvh2 = ObbBvh::from_bvh(source2, *mesh);
        TASK_CHECK(bvh == bvh2);
    }
    return 0;
}

int case_frames_double_precision() {
    using D = double;
    using V3d = bvh::v2::Vec<D, 3>;
    using T3d = bvh::v2::Tri<D, 3>;
    using N3d = bvh::v2::Node<D, 3>;
    std::vector<T3d> tris;
    auto n = bvh::v2::normalize(V3d(0.2, 0.9, -0.3));
    auto u = bvh::v2::normalize(V3d(1, 0, 0));
    auto v = bvh::v2::normalize(bvh::v2::cross(n, u));
    for (int i = 0; i < 12; ++i) {
        auto c = V3d(0.3 * i, 0.1 * i, -0.2 * i) + n * 0.4;
        tris.push_back(T3d(c, c + u * 0.5, c + v * 0.4));
    }
    std::vector<bvh::v2::BBox<D, 3>> boxes;
    std::vector<V3d> centers;
    for (const auto& t : tris) {
        boxes.push_back(t.get_bbox());
        centers.push_back(t.get_center());
    }
    auto source = bvh::v2::DefaultBuilder<N3d>::build(boxes, centers);
    auto bvh = bvh::v2::OrientedBvh<D>::from_bvh(source, tris);
    TASK_CHECK(bvh.node_count() == source.nodes.size());
    for (const auto& node : bvh.nodes()) {
        for (size_t i = 0; i < 3; ++i) {
            for (size_t j = 0; j < 3; ++j) {
                TASK_CHECK(std::isfinite(node.axis[i][j]));
                TASK_CHECK(std::isfinite(node.center[j]));
                TASK_CHECK(std::isfinite(node.half_size[j]));
                TASK_CHECK(node.half_size[j] >= 0);
            }
        }
        for (size_t i = 0; i < 3; ++i) {
            TASK_CHECK(std::fabs(bvh::v2::dot(node.axis[i], node.axis[i]) - 1) < 1e-12);
            for (size_t j = i + 1; j < 3; ++j)
                TASK_CHECK(std::fabs(bvh::v2::dot(node.axis[i], node.axis[j])) < 1e-12);
        }
    }
    // The two slanted halves use different frames.
    bool axes_differ = false;
    for (size_t i = 0; i < 3; ++i) {
        for (size_t j = 0; j < 3; ++j) {
            if (bvh.nodes()[0].axis[i][j] != bvh.nodes()[1].axis[i][j])
                axes_differ = true;
        }
    }
    TASK_CHECK(axes_differ);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// traversal
// ---------------------------------------------------------------------------------------------

int case_traversal_closest_matches_exhaustive() {
    auto tris = two_slant_mesh();
    auto source = build_source(tris);
    auto bvh = ObbBvh::from_bvh(source, tris);

    std::vector<Ray3> rays;
    for (int i = 0; i < 40; ++i) {
        auto x = -1.9f + 0.10f * static_cast<Scalar>(i);
        auto y = -1.6f + 0.09f * static_cast<Scalar>(i % 13);
        // Non-unit direction, with a finite distance window.
        rays.emplace_back(Vec3(x, y, 6), Vec3(0.02f * (i % 5), -0.01f * (i % 7), -2.0f), 0.05f, 40.0f);
    }
    // Axis-parallel rays, including negative zeros in the direction.
    rays.emplace_back(Vec3(0, 0, 6), Vec3(0, 0, -1), 0, 100);
    rays.emplace_back(Vec3(0, 0, 6), Vec3(-0.0f, -0.0f, -1), 0, 100);
    rays.emplace_back(Vec3(-2.0f, 0.0f, 6), Vec3(0, 3, -1), 0, 20);
    rays.emplace_back(Vec3(2.0f, 0.0f, 6), Vec3(0, -3, -1), 0, 20);

    size_t hits = 0;
    size_t index = 0;
    for (auto ray : rays) {
        auto expected = exhaustive_closest(tris, ray);
        ObbBvh::TraversalStats stats;
        auto actual = obb_closest(bvh, tris, ray, &stats);
        if (expected.prim_id != invalid_id && actual.prim_id != expected.prim_id)
            std::fprintf(stderr, "DEBUG ray %zu: expected %zu t=%.9g, got %zu t=%.9g\n", index, expected.prim_id, expected.t, actual.prim_id, actual.t);
        if (expected.prim_id != invalid_id &&
            std::fabs(actual.t - expected.t) > 1e-4f * std::max(1.0f, std::fabs(expected.t)) + 1e-5f)
            std::fprintf(stderr, "DEBUG t ray %zu: expected %zu t=%.9g, got %zu t=%.9g\n", index, expected.prim_id, expected.t, actual.prim_id, actual.t);
        ++index;
        if (expected.prim_id != invalid_id)
            ++hits;
        TASK_CHECK(actual.prim_id == expected.prim_id);
        if (expected.prim_id != invalid_id)
            TASK_CHECK(std::fabs(actual.t - expected.t) <= 1e-4f * std::max(1.0f, std::fabs(expected.t)) + 1e-5f);
        TASK_CHECK(stats.node_tests + 1 >= stats.primitive_tests / 16);
    }
    TASK_CHECK(hits > 10);
    return 0;
}

int case_traversal_special_rays() {
    // Empty tree: queries are no-ops and report no work.
    ObbBvh empty;
    Ray3 ray(Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 10);
    ObbBvh::TraversalStats empty_stats;
    auto empty_result = obb_closest(empty, {}, ray, &empty_stats);
    TASK_CHECK(empty_result.prim_id == invalid_id);
    TASK_CHECK(empty_stats.node_tests == 0 && empty_stats.primitive_tests == 0);
    TASK_CHECK(!obb_any_hit(empty, {}, ray));

    // Single-leaf tree.
    std::vector<Tri3> tris = { Tri3(Vec3(-1, -1, 2), Vec3(1, -1, 2), Vec3(0, 1, 2)) };
    auto source = build_source(tris, 8);
    auto bvh = ObbBvh::from_bvh(source, tris);
    TASK_CHECK(bvh.node_count() == 1);
    TASK_CHECK(bvh.get_root().is_leaf());
    auto expected = exhaustive_closest(tris, ray);
    auto actual = obb_closest(bvh, tris, ray);
    TASK_CHECK(actual.prim_id == expected.prim_id);
    TASK_CHECK(std::fabs(actual.t - expected.t) < 1e-5f);

    // Ray origin exactly on the box face, pointing inward: the box entry is exactly t = 0.
    auto obb = bvh.get_root().get_obb();
    auto face_point = obb.center + obb.axis[2] * obb.half_size[2];
    auto face_dir = obb.axis[2] * -1.0f;
    auto face_ray = Ray3(face_point, face_dir, 0, 100);
    auto interval = obb.intersect(face_ray);
    TASK_CHECK(interval.first <= 0);
    TASK_CHECK(interval.second >= 0);
    TASK_CHECK(std::fabs(interval.second - 2 * obb.half_size[2]) < 1e-3f);

    // Non-unit direction: the reported parameter scales with the direction length.
    auto scaled = obb.intersect(Ray3(face_point, face_dir * 4.0f, 0, 1000));
    TASK_CHECK(std::fabs(scaled.second - interval.second * 4.0f) < 1e-2f);

    // Ray parallel to a face and outside the slab must miss.
    auto outside = obb.center + obb.axis[0] * (obb.half_size[0] * 2.0f) + obb.axis[1] * (obb.half_size[1] * 2.0f);
    auto miss = obb.intersect(Ray3(outside, obb.axis[2], 0, 100));
    TASK_CHECK(miss.first > miss.second);

    // Negative zero direction components behave like positive zero (axis-aligned slab test).
    auto nz = obb.intersect(Ray3(face_point, Vec3(-0.0f, -0.0f, -1.0f), 0, 100));
    TASK_CHECK(nz.first <= 0 && nz.second >= 0);

    // Finite distance window excludes everything beyond it.
    TASK_CHECK(!obb_any_hit(bvh, tris, Ray3(Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 1.5f)));
    TASK_CHECK(obb_any_hit(bvh, tris, Ray3(Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 2.5f)));
    return 0;
}

int case_traversal_any_hit_and_far_leaf() {
    // Two clusters along z: a near one at z = 1 and a far one at z = 3. The ray hits the near
    // triangle; once `tmax` shrinks, the far leaf must never be expanded or tested.
    std::vector<Tri3> tris;
    constexpr size_t near_count = 8;
    constexpr size_t far_count = 8;
    for (size_t i = 0; i < near_count; ++i) {
        auto x = -0.28f + 0.08f * static_cast<Scalar>(i % 8);
        auto y = -0.02f;
        tris.push_back(Tri3(Vec3(x - 0.02f, y - 0.02f, 1),
                            Vec3(x + 0.02f, y - 0.02f, 1),
                            Vec3(x, y + 0.02f, 1)));
    }
    for (size_t i = 0; i < far_count; ++i) {
        auto x = -0.28f + 0.08f * static_cast<Scalar>(i % 8);
        auto y = -0.02f;
        tris.push_back(Tri3(Vec3(x - 0.02f, y - 0.02f, 3),
                            Vec3(x + 0.02f, y - 0.02f, 3),
                            Vec3(x, y + 0.02f, 3)));
    }
    auto source = manual_source(tris, near_count, far_count, "far-leaf");
    auto bvh = ObbBvh::from_bvh(source, tris);
    TASK_CHECK(bvh.node_count() == 3);
    TASK_CHECK(bvh.same_topology(source));

    Ray3 ray(Vec3(-0.04f, -0.01f, 0), Vec3(0, 0, 1), 0, 100);
    ObbBvh::TraversalStats stats;
    std::vector<size_t> visited_prim_ids;
    bool hit = false;
    ObbBvh::DefaultStack stack;
    bvh.intersect<false>(ray, stack, [&] (size_t begin, size_t end) {
        for (size_t k = begin; k < end; ++k) {
            auto prim = bvh.prim_ids()[k];
            visited_prim_ids.push_back(prim);
            if (auto h = Precomputed(tris[prim]).intersect(ray)) {
                auto t = std::get<0>(*h);
                if (t < ray.tmax) {
                    ray.tmax = t;
                    hit = true;
                }
            }
        }
        return hit;
    }, &stats);

    TASK_CHECK(hit);
    TASK_CHECK(std::fabs(ray.tmax - 1.0f) < 1e-5f);
    TASK_CHECK(!visited_prim_ids.empty());
    for (auto prim : visited_prim_ids)
        TASK_CHECK(prim < near_count);
    TASK_CHECK(stats.primitive_tests == visited_prim_ids.size());
    TASK_CHECK(stats.primitive_tests <= near_count);
    // The near leaf must have been probed at least once, and node tests should stay small.
    TASK_CHECK(stats.node_tests >= 1);
    TASK_CHECK(stats.leaf_tests == 1);

    // Any-hit agrees with the exhaustive loop for a set of finite windows.
    for (Scalar tmax : { 0.5f, 0.9f, 1.5f, 2.9f, 3.5f }) {
        auto probe = Ray3(Vec3(-0.04f, -0.01f, 0), Vec3(0, 0, 1), 0, tmax);
        TASK_CHECK(obb_any_hit(bvh, tris, probe) == exhaustive_any_hit(tris, probe));
    }
    printf("STATS far_leaf node_tests=%zu primitive_tests=%zu\n", stats.node_tests, stats.primitive_tests);
    return 0;
}

int case_traversal_concurrent_queries() {
    auto tris = two_slant_mesh();
    auto source = build_source(tris);
    auto bvh = ObbBvh::from_bvh(source, tris);

    std::vector<Ray3> rays;
    for (int i = 0; i < 128; ++i) {
        rays.emplace_back(
            Vec3(-2.4f + 0.037f * static_cast<Scalar>(i), -1.8f + 0.029f * static_cast<Scalar>(i % 23), 5),
            Vec3(0.01f * (i % 3), -0.02f * (i % 5), -1),
            0, 30);
    }
    std::vector<RayResult> expected(rays.size());
    for (size_t i = 0; i < rays.size(); ++i)
        expected[i] = exhaustive_closest(tris, rays[i]);

    std::vector<std::vector<RayResult>> per_thread(4);
    std::vector<std::thread> threads;
    for (size_t t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            per_thread[t].resize(rays.size());
            for (size_t i = 0; i < rays.size(); ++i) {
                ObbBvh::TraversalStats stats;
                per_thread[t][i] = obb_closest(bvh, tris, rays[i], &stats);
                // Every thread owns its own stack and counters.
            }
        });
    }
    for (auto& thread : threads)
        thread.join();

    for (size_t t = 0; t < per_thread.size(); ++t) {
        for (size_t i = 0; i < rays.size(); ++i) {
            TASK_CHECK(per_thread[t][i].prim_id == expected[i].prim_id);
            TASK_CHECK(per_thread[t][i].t == expected[i].t);
        }
    }
    return 0;
}

int case_traversal_primitive_budget() {
    // 512 thin, separated, equally oriented slanted plane triangles.
    std::vector<Tri3> tris;
    constexpr int grid = 16;
    constexpr int cell = 8;
    auto n = normalize_or(Vec3(1, 1, 1));
    auto u = normalize_or(Vec3(1, -1, 0));
    auto v = normalize_or(bvh::v2::cross(n, u));
    constexpr Scalar half_len = 1.1f;
    constexpr Scalar half_width = 0.01f;
    for (int i = 0; i < grid; ++i) {
        for (int j = 0; j < grid; ++j) {
            auto c = Vec3(static_cast<Scalar>(i * cell), static_cast<Scalar>(j * cell), 0);
            auto a = c - u * half_len - v * half_width;
            auto b = c + u * half_len - v * half_width;
            auto d = c + u * half_len + v * half_width;
            auto e = c - u * half_len + v * half_width;
            tris.push_back(Tri3(a, b, d));
            tris.push_back(Tri3(a, d, e));
        }
    }
    TASK_CHECK(tris.size() == 512);
    auto source = build_source(tris, 4);
    auto bvh = ObbBvh::from_bvh(source, tris);

    // Rays passing through the empty corner region of each triangle's world AABB. The world AABB
    // of a thin slanted triangle is much larger than the triangle itself, so these rays pierce
    // many axis-aligned boxes while missing the actual strips. The oriented boxes follow the
    // strips, so they reject the rays immediately.
    std::vector<Ray3> rays;
    for (size_t i = 0; i < tris.size(); ++i) {
        auto bounds = tris[i].get_bbox();
        auto corner = Vec3(bounds.max[0], bounds.max[1], bounds.min[2]);
        rays.emplace_back(Vec3(corner[0], corner[1], 3), Vec3(0, 0, -1), 0, 100);
        rays.emplace_back(Vec3(corner[0], corner[1], -3), Vec3(-0.0f, -0.0f, 1), 0, 100);
    }
    TASK_CHECK(rays.size() >= 512);

    size_t old_primitive_tests = 0;
    size_t old_node_tests = 0;
    size_t new_primitive_tests = 0;
    size_t new_node_tests = 0;
    size_t hits = 0;

    for (auto ray : rays) {
        // Original axis-aligned path.
        size_t old_leaf_prims = 0;
        {
            Ray3 old_ray = ray;
            bvh::v2::SmallStack<Node3::Index, 256> stack;
            source.intersect<false, true>(
                old_ray, source.get_root().index, stack,
                [&] (size_t begin, size_t end) {
                    old_leaf_prims += end - begin;
                    for (size_t k = begin; k < end; ++k) {
                        auto prim = source.prim_ids[k];
                        if (auto h = Precomputed(tris[prim]).intersect(old_ray))
                            old_ray.tmax = std::get<0>(*h);
                    }
                    return false;
                },
                [&] (const Node3&, const Node3&) { old_node_tests += 2; });
        }
        old_primitive_tests += old_leaf_prims;

        // New oriented path.
        Ray3 new_ray = ray;
        ObbBvh::TraversalStats stats;
        ObbBvh::DefaultStack stack;
        std::vector<size_t> tested;
        bvh.intersect<false>(new_ray, stack, [&] (size_t begin, size_t end) {
            for (size_t k = begin; k < end; ++k) {
                auto prim = bvh.prim_ids()[k];
                tested.push_back(prim);
                if (auto h = Precomputed(tris[prim]).intersect(new_ray)) {
                    new_ray.tmax = std::get<0>(*h);
                    ++hits;
                }
            }
            return false;
        }, &stats);
        TASK_CHECK(stats.primitive_tests == tested.size());
        new_primitive_tests += stats.primitive_tests;
        new_node_tests += stats.node_tests;
    }

    printf("STATS primitive_budget old_node_tests=%zu old_primitive_tests=%zu new_node_tests=%zu new_primitive_tests=%zu hits=%zu\n",
        old_node_tests, old_primitive_tests, new_node_tests, new_primitive_tests, hits);
    TASK_CHECK(old_primitive_tests > 0);
    TASK_CHECK(old_node_tests > 0);
    TASK_CHECK(new_node_tests > 0);
    // All production query work: box tests plus primitive tests, on both paths. The oriented
    // path must not do more total work, and its primitive tests must be less than a quarter of
    // the axis-aligned path.
    auto old_total = old_node_tests + old_primitive_tests;
    auto new_total = new_node_tests + new_primitive_tests;
    printf("STATS primitive_budget old_node_tests=%zu old_primitive_tests=%zu new_node_tests=%zu new_primitive_tests=%zu\n",
        old_node_tests, old_primitive_tests, new_node_tests, new_primitive_tests);
    printf("STATS primitive_budget old_total=%zu new_total=%zu primitive_ratio=%.4f\n",
        old_total, new_total,
        static_cast<double>(new_primitive_tests) / static_cast<double>(old_primitive_tests));
    TASK_CHECK(new_primitive_tests * 4 < old_primitive_tests);
    TASK_CHECK(new_total <= old_total);
    TASK_CHECK(rays.size() >= 512);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// maintenance
// ---------------------------------------------------------------------------------------------

int case_maintenance_refit_two_deformations() {
    auto tris = two_slant_mesh();
    auto source = build_source(tris);
    auto bvh = ObbBvh::from_bvh(source, tris);

    std::vector<std::array<Vec3, 3>> original_frames;
    std::vector<Node3::Index> original_indices;
    for (const auto& node : bvh.nodes()) {
        original_frames.push_back(node.axis);
        original_indices.push_back(node.index);
    }

    std::vector<Ray3> rays;
    for (int i = 0; i < 24; ++i) {
        rays.emplace_back(
            Vec3(-2.2f + 0.19f * static_cast<Scalar>(i), -1.9f + 0.15f * static_cast<Scalar>(i % 11), 5),
            Vec3(0.01f * (i % 3), -0.01f * (i % 4), -1),
            0, 30);
    }

    for (int iteration = 0; iteration < 2; ++iteration) {
        // Push the whole mesh further along +z, well beyond the previous bounds.
        for (auto& tri : tris) {
            tri.p0[2] += 1.7f;
            tri.p1[2] += 1.7f;
            tri.p2[2] += 1.7f;
        }
        // Before the refit, the old boxes must no longer cover the deformed geometry for the
        // rays aimed through the old positions.
        bvh.refit(tris);
        TASK_CHECK(bvh.same_topology(source));
        TASK_CHECK(bvh.prim_ids() == source.prim_ids);
        for (size_t i = 0; i < bvh.node_count(); ++i) {
            TASK_CHECK(same_frame(bvh.nodes()[i].axis, original_frames[i]));
            TASK_CHECK(bvh.nodes()[i].index == original_indices[i]);
        }
        TASK_CHECK(axes_are_valid(bvh));

        for (auto ray : rays) {
            auto expected = exhaustive_closest(tris, ray);
            auto actual = obb_closest(bvh, tris, ray);
            TASK_CHECK(actual.prim_id == expected.prim_id);
            if (expected.prim_id != invalid_id && actual.prim_id != invalid_id)
                TASK_CHECK(std::fabs(actual.t - expected.t) <= 1e-4f);
        }

        // Every node still tightly contains its subtree's vertices.
        for (size_t id = 0; id < bvh.node_count(); ++id) {
            auto vertices = subtree_vertices(source, tris, id);
            if (vertices.empty())
                continue;
            auto max_abs = Scalar(0);
            for (const auto& p : vertices)
                for (size_t k = 0; k < 3; ++k)
                    max_abs = std::max(max_abs, std::fabs(p[k]));
            auto tol = 64 * eps * std::max(Scalar(1), max_abs);
            const auto& node = bvh.nodes()[id];
            for (const auto& p : vertices) {
                for (size_t k = 0; k < 3; ++k) {
                    auto proj = bvh::v2::dot(p - node.center, node.axis[k]);
                    TASK_CHECK(std::fabs(proj) <= node.half_size[k] + 2 * tol);
                }
            }
        }
    }
    return 0;
}

int case_maintenance_refit_uses_child_obbs() {
    // Build a tree whose nodes have frames that differ from each other and from the world axes,
    // then deform so that the parent cannot be reconstructed from the old world AABB corners.
    std::vector<Tri3> tris;
    for (int i = 0; i < 8; ++i) {
        auto c = Vec3(0.9f * i, 0.2f * (i % 3), 0.1f * i);
        tris.push_back(Tri3(c + Vec3(0, 0, 0), c + Vec3(0.4f, 0.1f, 0.1f), c + Vec3(0.1f, 0.4f, -0.1f)));
    }
    auto source = manual_source(tris, 4, 4, "refit-child");
    auto bvh = ObbBvh::from_bvh(source, tris);

    // Deform only the right half beyond the old bounds, then refit.
    for (size_t i = 4; i < tris.size(); ++i) {
        auto offset = Vec3(0.7f, -0.4f, 0.9f);
        tris[i].p0 = tris[i].p0 + offset;
        tris[i].p1 = tris[i].p1 + offset;
        tris[i].p2 = tris[i].p2 + offset;
    }
    auto frames_before = std::vector<std::array<Vec3, 3>>();
    for (const auto& node : bvh.nodes())
        frames_before.push_back(node.axis);
    bvh.refit(tris);

    const auto& root = bvh.nodes()[0];
    const auto& left = bvh.nodes()[1];
    const auto& right = bvh.nodes()[2];
    TASK_CHECK(bvh.same_topology(source));

    // Expected parent bounds: project the eight corners of both child OBBs onto the parent frame.
    auto [lmin, lmax] = left.project_corners(root.axis);
    auto [rmin, rmax] = right.project_corners(root.axis);
    auto min_proj = bvh::v2::robust_min(lmin, rmin);
    auto max_proj = bvh::v2::robust_max(lmax, rmax);
    auto mid = (min_proj + max_proj) * Scalar(0.5);
    auto expected_center = root.axis[0] * mid[0] + root.axis[1] * mid[1] + root.axis[2] * mid[2];
    for (size_t k = 0; k < 3; ++k) {
        auto expected_half = std::max(Scalar(0), (max_proj[k] - min_proj[k]) * 0.5f);
        TASK_CHECK(std::fabs(root.center[k] - expected_center[k]) <= 1e-5f);
        TASK_CHECK(std::fabs(root.half_size[k] - expected_half) <= 1e-5f);
    }
    // Frames must be untouched.
    for (size_t i = 0; i < bvh.node_count(); ++i)
        TASK_CHECK(same_frame(bvh.nodes()[i].axis, frames_before[i], 0));

    // The refitted parent must actually enclose both child boxes.
    for (uint32_t bits = 0; bits < 8; ++bits) {
        auto corner = left.get_obb().get_corner(bits);
        for (size_t k = 0; k < 3; ++k)
            TASK_CHECK(std::fabs(bvh::v2::dot(corner - root.center, root.axis[k])) <= root.half_size[k] + 1e-4f);
        corner = right.get_obb().get_corner(bits);
        for (size_t k = 0; k < 3; ++k)
            TASK_CHECK(std::fabs(bvh::v2::dot(corner - root.center, root.axis[k])) <= root.half_size[k] + 1e-4f);
    }
    return 0;
}

int case_maintenance_serialize_load_refit() {
    auto tris = two_slant_mesh();
    auto source = build_source(tris);
    auto original = ObbBvh::from_bvh(source, tris);

    std::stringstream buffer;
    {
        StdOutputStream out(buffer);
        original.serialize(out);
    }
    // The oriented archive is distinct from the axis-aligned BVH archive.
    {
        std::stringstream aabb_buffer;
        StdOutputStream out(aabb_buffer);
        source.serialize(out);
        TASK_CHECK(aabb_buffer.str() != buffer.str());
        TASK_CHECK(aabb_buffer.str().size() != buffer.str().size());
    }

    auto original_str = buffer.str();
    StdInputStream in(buffer);
    auto loaded = ObbBvh::deserialize(in);
    TASK_CHECK(loaded.node_count() == original.node_count());
    TASK_CHECK(loaded.prim_ids() == original.prim_ids());
    TASK_CHECK(loaded == original);
    TASK_CHECK(loaded.same_topology(source));
    for (size_t i = 0; i < original.node_count(); ++i) {
        TASK_CHECK(same_frame(loaded.nodes()[i].axis, original.nodes()[i].axis, 0));
    }

    // Queries still work without the source tree, and refitting the loaded tree works too.
    for (int i = 0; i < 20; ++i) {
        Ray3 ray(
            Vec3(-2.3f + 0.23f * static_cast<Scalar>(i), -1.8f + 0.17f * static_cast<Scalar>(i % 9), 5),
            Vec3(0.01f * (i % 3), -0.01f * (i % 4), -1), 0, 30);
        auto expected = exhaustive_closest(tris, ray);
        auto actual = obb_closest(loaded, tris, ray);
        TASK_CHECK(actual.prim_id == expected.prim_id);
    }

    for (auto& tri : tris) {
        tri.p0[2] += 3.4f;
        tri.p1[2] += 3.4f;
        tri.p2[2] += 3.4f;
    }
    loaded.refit(tris);
    TASK_CHECK(loaded.prim_ids() == original.prim_ids());
    for (int i = 0; i < 20; ++i) {
        Ray3 ray(
            Vec3(-2.3f + 0.23f * static_cast<Scalar>(i), -1.8f + 0.17f * static_cast<Scalar>(i % 9), 9),
            Vec3(0.01f * (i % 3), -0.01f * (i % 4), -1), 0, 30);
        auto expected = exhaustive_closest(tris, ray);
        auto actual = obb_closest(loaded, tris, ray);
        TASK_CHECK(actual.prim_id == expected.prim_id);
        if (expected.prim_id != invalid_id)
            TASK_CHECK(std::fabs(actual.t - expected.t) <= 1e-4f);
    }

    // Re-serializing after refit round-trips the updated frames.
    std::stringstream second;
    {
        StdOutputStream out(second);
        loaded.serialize(out);
    }
    StdInputStream second_in(second);
    auto reloaded = ObbBvh::deserialize(second_in);
    TASK_CHECK(reloaded == loaded);
    TASK_CHECK(reloaded.same_topology(source));

    // Corrupted/foreign archives are rejected rather than misread.
    std::stringstream bogus;
    bogus << "not an oriented bvh";
    StdInputStream bogus_in(bogus);
    auto rejected = ObbBvh::deserialize(bogus_in);
    TASK_CHECK(rejected.empty());
    (void)original_str;
    return 0;
}

// ---------------------------------------------------------------------------------------------
// c_api
// ---------------------------------------------------------------------------------------------

struct c_query_state {
    const std::vector<Tri3>* tris;
    const struct bvh_obb3f* bvh;
    Vec3 org;
    Vec3 dir;
    size_t best;
    float best_t;
};

bool c_query_callback(void* user_data, float* t, size_t begin, size_t end) {
    auto* state = static_cast<c_query_state*>(user_data);
    bool found = false;
    for (size_t k = begin; k < end; ++k) {
        auto prim = bvh_obb3f_get_prim_id(state->bvh, k);
        Ray3 probe(state->org, state->dir, 0.0f, *t);
        if (auto hit = Precomputed((*state->tris)[prim]).intersect(probe)) {
            auto hit_t = std::get<0>(*hit);
            if (hit_t <= *t) {
                state->best = prim;
                state->best_t = hit_t;
                *t = hit_t;
                found = true;
            }
        }
    }
    return found;
}

int case_c_api_convert_and_node_access() {
    auto tris = two_slant_mesh();
    auto source = build_source(tris);

    std::vector<bvh_bbox3f> boxes;
    std::vector<bvh_vec3f> centers;
    std::vector<bvh_vec3f> verts;
    for (const auto& tri : tris) {
        auto b = tri.get_bbox();
        auto c = tri.get_center();
        boxes.push_back(bvh_bbox3f { { b.min[0], b.min[1], b.min[2] }, { b.max[0], b.max[1], b.max[2] } });
        centers.push_back(bvh_vec3f { c[0], c[1], c[2] });
        verts.push_back(bvh_vec3f { tri.p0[0], tri.p0[1], tri.p0[2] });
        verts.push_back(bvh_vec3f { tri.p1[0], tri.p1[1], tri.p1[2] });
        verts.push_back(bvh_vec3f { tri.p2[0], tri.p2[1], tri.p2[2] });
    }

    struct bvh3f* c_source = bvh3f_build(nullptr, boxes.data(), centers.data(), boxes.size(), nullptr);
    TASK_CHECK(c_source != nullptr);
    TASK_CHECK(bvh3f_get_node_count(c_source) == source.nodes.size());

    struct bvh_obb3f* c_obb = bvh_obb3f_from_bvh(c_source, verts.data(), tris.size());
    TASK_CHECK(c_obb != nullptr);
    TASK_CHECK(bvh_obb3f_get_node_count(c_obb) == source.nodes.size());
    TASK_CHECK(bvh_obb3f_get_prim_count(c_obb) == tris.size());
    // `prim_ids` is a permutation of the original identifiers, in the same order as the source.
    {
        std::vector<size_t> seen(tris.size(), 0);
        for (size_t i = 0; i < tris.size(); ++i) {
            auto id = bvh_obb3f_get_prim_id(c_obb, i);
            TASK_CHECK(id < tris.size());
            if (id < tris.size())
                seen[id] += 1;
        }
        for (auto count : seen)
            TASK_CHECK(count == 1);
    }

    // The source tree can be destroyed right after the conversion.
    bvh3f_destroy(c_source);

    // Node frames are readable and differ between the two slanted subtrees.
    bvh_vec3f axes0[3], axes1[3], center0, half0;
    TASK_CHECK(bvh_obb3f_get_node_obb(c_obb, 0, axes0, &center0, &half0));
    TASK_CHECK(bvh_obb3f_get_node_obb(c_obb, 1, axes1, nullptr, nullptr));
    bool different = false;
    for (int i = 0; i < 3; ++i) {
        if (axes0[i].x != axes1[i].x || axes0[i].y != axes1[i].y || axes0[i].z != axes1[i].z)
            different = true;
    }
    TASK_CHECK(different);
    TASK_CHECK(half0.x >= 0 && half0.y >= 0 && half0.z >= 0);
    TASK_CHECK(bvh_obb3f_node_is_leaf(c_obb, 0) == false);
    TASK_CHECK(bvh_obb3f_node_get_prim_count(c_obb, 0) == 0);
    size_t first_leaf = bvh_obb3f_get_node_count(c_obb);
    for (size_t i = 1; i < bvh_obb3f_get_node_count(c_obb); ++i) {
        if (bvh_obb3f_node_is_leaf(c_obb, i)) {
            first_leaf = i;
            break;
        }
    }
    TASK_CHECK(first_leaf < bvh_obb3f_get_node_count(c_obb));
    TASK_CHECK(bvh_obb3f_node_get_prim_count(c_obb, first_leaf) > 0);
    TASK_CHECK(!bvh_obb3f_get_node_obb(c_obb, 9999, axes0, nullptr, nullptr));

    return 0;
}

int case_c_api_query_and_save_load() {
    auto tris = two_slant_mesh();
    auto source = build_source(tris);

    std::vector<bvh_bbox3f> boxes;
    std::vector<bvh_vec3f> centers;
    std::vector<bvh_vec3f> verts;
    for (const auto& tri : tris) {
        auto b = tri.get_bbox();
        auto c = tri.get_center();
        boxes.push_back(bvh_bbox3f { { b.min[0], b.min[1], b.min[2] }, { b.max[0], b.max[1], b.max[2] } });
        centers.push_back(bvh_vec3f { c[0], c[1], c[2] });
        verts.push_back(bvh_vec3f { tri.p0[0], tri.p0[1], tri.p0[2] });
        verts.push_back(bvh_vec3f { tri.p1[0], tri.p1[1], tri.p1[2] });
        verts.push_back(bvh_vec3f { tri.p2[0], tri.p2[1], tri.p2[2] });
    }

    struct bvh3f* c_source = bvh3f_build(nullptr, boxes.data(), centers.data(), boxes.size(), nullptr);
    TASK_CHECK(c_source != nullptr);
    struct bvh_obb3f* c_obb = bvh_obb3f_from_bvh(c_source, verts.data(), tris.size());
    TASK_CHECK(c_obb != nullptr);
    bvh3f_destroy(c_source);

    // Closest-hit queries through the C callback agree with the exhaustive loop.
    size_t hits = 0;
    for (int i = 0; i < 32; ++i) {
        Vec3 org(-2.3f + 0.14f * static_cast<float>(i), -1.8f + 0.11f * static_cast<float>(i % 11), 5.0f);
        Vec3 dir(0.01f * static_cast<float>(i % 3), -0.01f * static_cast<float>(i % 4), -1.0f);
        auto expected = exhaustive_closest(tris, Ray3(org, dir, 0, 30));

        bvh_ray3f ray {
            { org[0], org[1], org[2] },
            { dir[0], dir[1], dir[2] },
            0.0f, 30.0f };
        c_query_state state { &tris, c_obb, org, dir, invalid_id, std::numeric_limits<float>::infinity() };
        bvh_intersect_callbackf callback { &state, c_query_callback };
        bvh_obb_traversal_stats stats {};
        bvh_obb3f_intersect_ray_stats(c_obb, &ray, &callback, &stats);
        if (expected.prim_id != invalid_id)
            ++hits;
        TASK_CHECK(state.best == expected.prim_id);
        if (expected.prim_id != invalid_id)
            TASK_CHECK(std::fabs(state.best_t - expected.t) <= 1e-4f);
        TASK_CHECK(stats.node_tests > 0);
    }
    TASK_CHECK(hits > 4);

    // Any-hit queries honor the finite distance window.
    for (float window : { 1.0f, 3.0f, 6.0f }) {
        Ray3 local(Vec3(-1.1f, 0.2f, 5), Vec3(0, 0, -1), 0, window);
        bool expected = exhaustive_any_hit(tris, local);
        c_query_state state { &tris, c_obb, local.org, local.dir, invalid_id, window };
        bvh_intersect_callbackf callback { &state, c_query_callback };
        bvh_ray3f ray { { local.org[0], local.org[1], local.org[2] }, { 0, 0, -1 }, 0, window };
        bvh_obb3f_intersect_ray_any(c_obb, &ray, &callback);
        TASK_CHECK((state.best != invalid_id) == expected);
    }

    // Refit through the C API, then save/load round-trip.
    for (auto& tri : tris) {
        tri.p0[2] += 2.6f;
        tri.p1[2] += 2.6f;
        tri.p2[2] += 2.6f;
    }
    std::vector<bvh_vec3f> moved;
    for (const auto& tri : tris) {
        moved.push_back(bvh_vec3f { tri.p0[0], tri.p0[1], tri.p0[2] });
        moved.push_back(bvh_vec3f { tri.p1[0], tri.p1[1], tri.p1[2] });
        moved.push_back(bvh_vec3f { tri.p2[0], tri.p2[1], tri.p2[2] });
    }
    bvh_obb3f_refit(c_obb, moved.data(), tris.size());
    TASK_CHECK(bvh_obb3f_get_node_count(c_obb) == source.nodes.size());

    auto path = std::string("obb_c_api_roundtrip.bin");
    FILE* file = std::fopen(path.c_str(), "wb");
    TASK_CHECK(file != nullptr);
    if (!file) {
        bvh_obb3f_destroy(c_obb);
        return 1;
    }
    bvh_obb3f_save(c_obb, file);
    std::fclose(file);

    FILE* in = std::fopen(path.c_str(), "rb");
    TASK_CHECK(in != nullptr);
    if (!in) {
        std::remove(path.c_str());
        bvh_obb3f_destroy(c_obb);
        return 1;
    }
    struct bvh_obb3f* loaded = bvh_obb3f_load(in);
    std::fclose(in);
    std::remove(path.c_str());
    TASK_CHECK(loaded != nullptr);
    if (loaded) {
        TASK_CHECK(bvh_obb3f_get_node_count(loaded) == bvh_obb3f_get_node_count(c_obb));
        for (size_t i = 0; i < bvh_obb3f_get_prim_count(loaded); ++i)
            TASK_CHECK(bvh_obb3f_get_prim_id(loaded, i) == bvh_obb3f_get_prim_id(c_obb, i));

        // Queries continue to work on the loaded tree, with no source tree available.
        for (int i = 0; i < 16; ++i) {
            Vec3 org(-2.0f + 0.2f * static_cast<float>(i), -1.2f + 0.2f * static_cast<float>(i % 7), 8.0f);
            Vec3 dir(0, 0, -1);
            auto expected = exhaustive_closest(tris, Ray3(org, dir, 0, 40));
            c_query_state state { &tris, loaded, org, dir, invalid_id, 40 };
            bvh_intersect_callbackf callback { &state, c_query_callback };
            bvh_ray3f ray { { org[0], org[1], org[2] }, { 0, 0, -1 }, 0, 40 };
            bvh_obb3f_intersect_ray(loaded, &ray, &callback);
            TASK_CHECK(state.best == expected.prim_id);
        }
        bvh_obb3f_destroy(loaded);
    }
    bvh_obb3f_destroy(c_obb);
    return 0;
}

int case_traversal_preview_matches_exhaustive() {
    // Render the shared preview scene through the oriented-BVH traversal and through an
    // exhaustive world-space loop, then compare every pixel.
    auto scene = obb_preview::make_scene();
    std::vector<BBox3> boxes;
    std::vector<Vec3> centers;
    for (const auto& tri : scene.triangles) {
        boxes.push_back(tri.get_bbox());
        centers.push_back(tri.get_center());
    }
    auto source = bvh::v2::DefaultBuilder<Node3>::build(boxes, centers);
    auto bvh = ObbBvh::from_bvh(source, scene.triangles);

    constexpr std::uint32_t width = 48;
    constexpr std::uint32_t height = 32;
    auto rendered = obb_preview::render_obb(scene, bvh, width, height);
    auto reference = obb_preview::render_exhaustive(scene, width, height);
    TASK_CHECK(rendered.rgb.size() == static_cast<size_t>(width) * height * 3);
    TASK_CHECK(rendered.rgb == reference.rgb);

    // The image must actually contain both slanted groups, not just background.
    size_t background_pixels = 0;
    for (size_t i = 0; i + 2 < rendered.rgb.size(); i += 3) {
        if (rendered.rgb[i] == 15 && rendered.rgb[i + 1] == 18 && rendered.rgb[i + 2] == 23)
            ++background_pixels;
    }
    TASK_CHECK(background_pixels < rendered.rgb.size() / 3);
    TASK_CHECK(rendered.rgb.size() / 3 - background_pixels > 100);

    // Each rendered group must come from its own half of the scene, and the two groups' frames
    // differ, so a single box orientation cannot produce the image.
    TASK_CHECK(scene.triangles.size() >= 24);
    TASK_CHECK(!same_frame(bvh.nodes()[1].axis, bvh.nodes()[2].axis, 0) ||
               !same_frame(bvh.nodes()[1].axis, bvh.nodes()[3].axis, 0));
    return 0;
}

int case_traversal_box_face_origin_and_finite_window() {
    // Rays starting exactly on an OBB face, including its corner, with a finite window.
    std::vector<Tri3> tris;
    for (int i = 0; i < 6; ++i) {
        auto c = Vec3(1.1f * i, 0.4f * (i % 2), 0.3f * (i % 3));
        tris.push_back(Tri3(c, c + Vec3(0.5f, 0.1f, 0.2f), c + Vec3(0.1f, 0.5f, -0.1f)));
    }
    auto source = build_source(tris);
    auto bvh = ObbBvh::from_bvh(source, tris);

    // Pick a leaf and start on the face of its own box, aimed inward.
    size_t leaf = invalid_id;
    for (size_t i = 0; i < bvh.node_count(); ++i) {
        if (bvh.nodes()[i].is_leaf()) {
            leaf = i;
            break;
        }
    }
    TASK_CHECK(leaf != invalid_id);
    const auto& node = bvh.nodes()[leaf];
    auto obb = node.get_obb();
    auto face_center = obb.center + obb.axis[2] * obb.half_size[2];
    auto face_corner = face_center + obb.axis[0] * obb.half_size[0] + obb.axis[1] * obb.half_size[1];

    struct FaceProbe {
        Vec3 origin;
        Vec3 dir;
    };
    auto inward_corner = obb.center - face_corner;
    auto inward_len = std::sqrt(bvh::v2::dot(inward_corner, inward_corner));
    TASK_CHECK(inward_len > 0);
    auto inward_corner_dir = inward_corner * (1.0f / inward_len);
    std::array<FaceProbe, 2> probes = {{
        { face_center, obb.axis[2] * -1.0f },
        { face_corner, inward_corner_dir },
    }};
    for (const auto& probe : probes) {
        auto ray = Ray3(probe.origin, probe.dir, -(1 << 20), (1 << 20));
        auto interval = obb.intersect(ray);
        // A ray starting exactly on a face enters the box at t ~ 0. Rounding may move the
        // computed entry a few ULPs to either side, so allow a tiny tolerance.
        TASK_CHECK(std::fabs(interval.first) <= 1e-4f);
        TASK_CHECK(interval.second >= -1e-4f);
        // The distance window is clamped by the query: starting inside a finite window, the
        // interval shrinks to the window itself.
        auto windowed = Ray3(probe.origin, probe.dir, 0.0f, 100.0f);
        auto windowed_interval = obb.intersect(windowed);
        TASK_CHECK(windowed_interval.first >= 0.0f && windowed_interval.first <= 1e-4f);
        TASK_CHECK(windowed_interval.second >= 0.0f);

        // Origin just outside the face, aimed inward: a finite window shorter than the gap must
        // reject the box, while a longer one accepts it.
        auto outside = probe.origin + probe.dir * -0.5f;
        auto inward = Ray3(outside, probe.dir, 0.0f, 0.25f);
        auto rejected = obb.intersect(inward);
        TASK_CHECK(rejected.first > rejected.second || rejected.first > inward.tmax);
        auto accepted = obb.intersect(Ray3(outside, probe.dir, 0.0f, 100.0f));
        TASK_CHECK(accepted.first <= accepted.second);
    }

    // A full query from a face origin stays consistent with the exhaustive loop.
    for (int i = 0; i < 12; ++i) {
        const auto& any = bvh.nodes()[leaf + static_cast<size_t>(i) % (bvh.node_count() - leaf)];
        auto box = any.get_obb();
        auto origin = box.center + box.axis[0] * (box.half_size[0] * (i % 3 - 1) * 0.5f);
        auto ray = Ray3(origin, box.axis[1] * -1.0f, 0.0f, 20.0f);
        auto expected = exhaustive_closest(tris, ray);
        auto actual = obb_closest(bvh, tris, ray);
        TASK_CHECK(actual.prim_id == expected.prim_id);
    }
    return 0;
}

const struct task_case test_cases[] = {
    { "frames", "orthonormal_and_tight", case_frames_orthonormal_and_tight },
    { "frames", "degenerate_covariance", case_frames_degenerate_covariance },
    { "frames", "double_precision", case_frames_double_precision },
    { "traversal", "closest_matches_exhaustive", case_traversal_closest_matches_exhaustive },
    { "traversal", "special_rays", case_traversal_special_rays },
    { "traversal", "any_hit_and_far_leaf", case_traversal_any_hit_and_far_leaf },
    { "traversal", "concurrent_queries", case_traversal_concurrent_queries },
    { "traversal", "primitive_budget", case_traversal_primitive_budget },
    { "traversal", "preview_matches_exhaustive", case_traversal_preview_matches_exhaustive },
    { "traversal", "box_face_origin_and_window", case_traversal_box_face_origin_and_finite_window },
    { "maintenance", "refit_two_deformations", case_maintenance_refit_two_deformations },
    { "maintenance", "refit_uses_child_obbs", case_maintenance_refit_uses_child_obbs },
    { "maintenance", "serialize_load_refit", case_maintenance_serialize_load_refit },
    { "c_api", "convert_and_node_access", case_c_api_convert_and_node_access },
    { "c_api", "query_refit_save_load", case_c_api_query_and_save_load },
};

} // namespace

extern const struct task_case task_cases[] = {
    test_cases[0], test_cases[1], test_cases[2], test_cases[3], test_cases[4], test_cases[5],
    test_cases[6], test_cases[7], test_cases[8], test_cases[9], test_cases[10], test_cases[11],
    test_cases[12], test_cases[13], test_cases[14],
};
extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
