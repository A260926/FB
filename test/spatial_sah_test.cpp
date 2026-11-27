#include "registry.hpp"

#include <bvh/v2/spatial_sah_builder.h>
#include <bvh/v2/binned_sah_builder.h>
#include <bvh/v2/stack.h>
#include <bvh/v2/stream.h>
#include <bvh/v2/ray.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/node.h>

#include <span>
#include <vector>
#include <array>
#include <string>
#include <iostream>
#include <sstream>
#include <cmath>
#include <cstdint>
#include <optional>
#include <tuple>
#include <set>
#include <algorithm>
#include <limits>

namespace {

using namespace bvh::v2;

// ---------------------------------------------------------------------------
// Independent test-side reference geometry: polygon clipping and surface area,
// written separately from the builder under test.
// ---------------------------------------------------------------------------
template <typename S>
using V3 = Vec<S, 3>;
template <typename S>
using T3 = Tri<S, 3>;
template <typename S>
using B3 = BBox<S, 3>;
template <typename S>
using N3 = Node<S, 3>;

template <typename S>
std::vector<V3<S>> clip_halfspace(
    std::vector<V3<S>> poly, size_t axis, bool keep_low, S plane)
{
    std::vector<V3<S>> out;
    if (poly.empty())
        return out;
    auto inside = [&](const V3<S>& p) {
        return keep_low ? p[axis] <= plane : p[axis] >= plane;
    };
    for (size_t i = 0; i < poly.size(); ++i) {
        const V3<S>& a = poly[i];
        const V3<S>& b = poly[(i + 1) % poly.size()];
        const bool ia = inside(a), ib = inside(b);
        auto cross_point = [&] {
            S t = (plane - a[axis]) / (b[axis] - a[axis]);
            return V3<S>::generate([&](size_t k) {
                return k == axis ? plane : a[k] + t * (b[k] - a[k]);
            });
        };
        if (ib) {
            if (!ia)
                out.push_back(cross_point());
            out.push_back(b);
        } else if (ia) {
            out.push_back(cross_point());
        }
    }
    return out;
}

template <typename S>
std::vector<V3<S>> clip_to_aabb(std::vector<V3<S>> poly, const B3<S>& cell) {
    for (size_t axis = 0; axis < 3; ++axis) {
        poly = clip_halfspace(poly, axis, true, cell.max[axis]);
        poly = clip_halfspace(poly, axis, false, cell.min[axis]);
    }
    return poly;
}

template <typename S>
S polygon_area(const std::vector<V3<S>>& p) {
    if (p.size() < 3)
        return S(0);
    V3<S> acc(0, 0, 0);
    for (size_t i = 0; i < p.size(); ++i)
        acc = acc + cross(p[i], p[(i + 1) % p.size()]);
    return S(0.5) * std::sqrt(dot(acc, acc));
}

template <typename S>
B3<S> polygon_bbox(const std::vector<V3<S>>& p) {
    B3<S> box(p[0]);
    for (size_t i = 1; i < p.size(); ++i)
        box.extend(p[i]);
    return box;
}

template <typename S>
S max_abs_coord(const std::vector<T3<S>>& tris) {
    S m = 0;
    for (const auto& t : tris)
        for (const auto* p : { &t.p0, &t.p1, &t.p2 })
            for (size_t a = 0; a < 3; ++a)
                m = robust_max(m, std::abs((*p)[a]));
    return m;
}

// Builds the result and verifies, using the independent reference clipper, that:
//  - every reference clips to a non-empty polygon lying inside its tight bounds,
//  - the tight bounds lie inside the accumulated cell,
//  - the fragments of every original triangle partition its area exactly once.
template <typename S>
void verify_partition(
    task::Case& tc,
    const typename SpatialSahBuilder<N3<S>, 16>::Result& r,
    const std::vector<T3<S>>& tris)
{
    const S tol = S(64) * std::numeric_limits<S>::epsilon() *
                  robust_max(S(1), max_abs_coord(tris)) + S(1e-6);
    std::vector<S> summed(tris.size(), 0);
    bool all_non_empty = true;
    bool all_contained = true;
    bool cells_contain_bounds = true;

    for (size_t slot = 0; slot < r.bvh.prim_ids.size(); ++slot) {
        const size_t id = r.bvh.prim_ids[slot];
        tc.check(id < tris.size(), "prim id points at an existing triangle");

        std::vector<V3<S>> tri = { tris[id].p0, tris[id].p1, tris[id].p2 };
        auto poly = clip_to_aabb(tri, r.reference_cells[slot]);
        if (poly.empty())
            all_non_empty = false;
        summed[id] += polygon_area(poly);

        const B3<S> pb = polygon_bbox(poly);
        const B3<S>& tight = r.reference_bounds[slot];
        const B3<S>& cell = r.reference_cells[slot];
        for (size_t a = 0; a < 3; ++a) {
            if (pb.min[a] < tight.min[a] - tol || pb.max[a] > tight.max[a] + tol)
                all_contained = false;
            if (tight.min[a] < cell.min[a] - tol || tight.max[a] > cell.max[a] + tol)
                cells_contain_bounds = false;
        }
    }

    bool area_partition = true;
    for (size_t id = 0; id < tris.size(); ++id) {
        S original = polygon_area(std::vector<V3<S>>{ tris[id].p0, tris[id].p1, tris[id].p2 });
        if (std::abs(summed[id] - original) > tol * robust_max(S(1), original))
            area_partition = false;
    }

    tc.check(all_non_empty, "every spatial reference clips to a non-empty fragment");
    tc.check(all_contained, "independent clip polygon is covered by the tight reference bounds");
    tc.check(cells_contain_bounds, "tight reference bounds stay inside the accumulated cell");
    tc.check(area_partition, "reference fragments partition each original triangle without overlap");
}

// ---------------------------------------------------------------------------
// Independent exhaustive triangle intersection (Moller-Trumbore), kept separate
// from the library's PrecomputedTri routine.
// ---------------------------------------------------------------------------
template <typename S>
struct BruteHit {
    size_t id;
    S t, u, v;
};

template <typename S>
std::optional<BruteHit<S>> brute_nearest(
    const std::vector<T3<S>>& tris, const Ray<S, 3>& ray)
{
    std::optional<BruteHit<S>> best;
    for (size_t id = 0; id < tris.size(); ++id) {
        const auto& t = tris[id];
        // Canonical textbook Moller-Trumbore, written independently of the library routine.
        V3<S> e1 = t.p1 - t.p0;
        V3<S> e2 = t.p2 - t.p0;
        V3<S> h = cross(ray.dir, e2);
        S a = dot(e1, h);
        if (std::abs(a) <= S(0))
            continue;
        S f = S(1) / a;
        V3<S> s = ray.org - t.p0;
        S u = f * dot(s, h);
        V3<S> q = cross(s, e1);
        S v = f * dot(ray.dir, q);
        S w = S(1) - u - v;
        // Accept vertices and edges lying (within rounding) on the triangle, mirroring the
        // library's epsilon tolerance so on-edge hits are not dropped by either implementation.
        const S tol = -std::numeric_limits<S>::epsilon();
        if (u < tol || v < tol || w < tol)
            continue;
        S tt = f * dot(e2, q);
        if (tt < ray.tmin || tt > ray.tmax)
            continue;
        if (!best || tt < best->t)
            best = BruteHit<S>{ id, tt, u, v };
    }
    return best;
}

template <typename S>
bool brute_any_hit(const std::vector<T3<S>>& tris, const Ray<S, 3>& ray) {
    return brute_nearest(tris, ray).has_value();
}

// Traces a nearest-hit query using prim_ids indirection.
template <typename S>
struct TreeHit {
    long id = -1;
    S t = 0, u = 0, v = 0;
    size_t tests = 0;
};

template <typename S>
TreeHit<S> trace_indirect(
    const Bvh<N3<S>>& bvh, const std::vector<T3<S>>& tris, const Ray<S, 3>& ray_in)
{
    Ray<S, 3> ray = ray_in;
    GrowingStack<typename N3<S>::Index> stack;
    TreeHit<S> out;
    bvh.template intersect<false, true>(ray, bvh.get_root().index, stack,
        [&](size_t begin, size_t end) {
            bool found = false;
            for (size_t i = begin; i < end; ++i) {
                ++out.tests;
                size_t id = bvh.prim_ids[i];
                auto q = PrecomputedTri<S>(tris[id]).intersect(ray);
                if (q) {
                    ray.tmax = std::get<0>(*q);
                    out.t = std::get<0>(*q);
                    out.u = std::get<1>(*q);
                    out.v = std::get<2>(*q);
                    out.id = long(id);
                    found = true;
                }
            }
            return found;
        });
    return out;
}

// Traces a nearest-hit query using a precomputed array reordered by reference slot.
template <typename S>
TreeHit<S> trace_precomputed(
    const Bvh<N3<S>>& bvh,
    const std::vector<PrecomputedTri<S>>& ordered,
    const Ray<S, 3>& ray_in)
{
    Ray<S, 3> ray = ray_in;
    GrowingStack<typename N3<S>::Index> stack;
    TreeHit<S> out;
    bvh.template intersect<false, true>(ray, bvh.get_root().index, stack,
        [&](size_t begin, size_t end) {
            bool found = false;
            for (size_t i = begin; i < end; ++i) {
                ++out.tests;
                auto q = ordered[i].intersect(ray);
                if (q) {
                    ray.tmax = std::get<0>(*q);
                    out.t = std::get<0>(*q);
                    out.u = std::get<1>(*q);
                    out.v = std::get<2>(*q);
                    out.id = long(bvh.prim_ids[i]);
                    found = true;
                }
            }
            return found;
        });
    return out;
}

template <typename S>
bool trace_occluded_indirect(
    const Bvh<N3<S>>& bvh, const std::vector<T3<S>>& tris, const Ray<S, 3>& ray_in)
{
    Ray<S, 3> ray = ray_in;
    GrowingStack<typename N3<S>::Index> stack;
    bool hit = false;
    bvh.template intersect<true, true>(ray, bvh.get_root().index, stack,
        [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                size_t id = bvh.prim_ids[i];
                if (PrecomputedTri<S>(tris[id]).intersect(ray)) {
                    hit = true;
                    return true;
                }
            }
            return false;
        });
    return hit;
}

template <typename S>
std::vector<PrecomputedTri<S>> make_ordered(
    const Bvh<N3<S>>& bvh, const std::vector<T3<S>>& tris)
{
    std::vector<PrecomputedTri<S>> ordered;
    ordered.reserve(bvh.prim_ids.size());
    for (size_t id : bvh.prim_ids)
        ordered.emplace_back(tris[id]);
    return ordered;
}

// ---------------------------------------------------------------------------
// Scene factories.
// ---------------------------------------------------------------------------

// The 32 long, diagonal slivers from the baseline.
template <typename S>
std::vector<T3<S>> make_sliver_scene() {
    std::vector<T3<S>> tris;
    for (size_t i = 0; i < 32; ++i) {
        S z = S(1) + S(i) / S(64);
        tris.emplace_back(V3<S>(-10, -10, z), V3<S>(10, 10, z), V3<S>(10, S(9.5), z));
    }
    return tris;
}

// Isotropic right triangles with a y-parallel edge at x=0 and an x-parallel edge at y=0. After an
// x clip the fragment keeps a comparable y extent, so the same fragment is subsequently clipped
// along y as well. The copies share (near) identical centers, which forces the builder to use
// spatial splits to reach leaf size 1.
template <typename S>
std::vector<T3<S>> make_multiaxis_scene(size_t copies = 8) {
    std::vector<T3<S>> tris;
    for (size_t i = 0; i < copies; ++i) {
        S dz = S(i) * S(1e-4);
        tris.emplace_back(
            V3<S>(0, 0, 1 + dz), V3<S>(40, 0, 1 + dz), V3<S>(0, 40, 1 + dz));
    }
    return tris;
}

// A triangle whose apex sits on a splitting plane and a pair sharing an edge.
template <typename S>
std::vector<T3<S>> make_vertex_edge_scene() {
    std::vector<T3<S>> tris;
    tris.emplace_back(V3<S>(0, 0, 1), V3<S>(-20, 20, 1), V3<S>(20, 20, 1));   // apex on x=0
    tris.emplace_back(V3<S>(-20, 0, 1), V3<S>(20, 0, 1), V3<S>(20, 20, 1));    // above shared edge y=0
    tris.emplace_back(V3<S>(-20, 0, 1), V3<S>(20, -20, 1), V3<S>(20, 0, 1));   // below shared edge y=0
    for (int i = 0; i < 5; ++i) {
        S x = -20 + S(i) * S(0.6);
        tris.emplace_back(V3<S>(x, 21, 2), V3<S>(x + S(0.5), 21, 2), V3<S>(x + S(0.25), S(20.5), 2));
    }
    return tris;
}

// Flat, zero-thickness triangles stacked closely together.
template <typename S>
std::vector<T3<S>> make_flat_scene() {
    std::vector<T3<S>> tris;
    for (size_t i = 0; i < 12; ++i) {
        S y = S(i);
        tris.emplace_back(V3<S>(0, y, 5), V3<S>(16, y, 5), V3<S>(16, y + S(0.3), 5));
    }
    return tris;
}

// A handful of small left triangles plus one long diagonal straddler, arranged
// so a budget of one extra reference produces exactly one duplication.
template <typename S>
std::vector<T3<S>> make_single_straddle_scene() {
    std::vector<T3<S>> tris;
    for (int i = 0; i < 3; ++i) {
        S y = -20 + S(i) * S(0.5);
        tris.emplace_back(V3<S>(-20, y, 1), V3<S>(-10, y, 1), V3<S>(-10, y + S(0.4), 1));
    }
    tris.emplace_back(V3<S>(-15, -20, 0), V3<S>(20, 20, 0), V3<S>(20, 19, 0));
    return tris;
}

template <typename S>
using Builder = SpatialSahBuilder<N3<S>, 16>;

// ===========================================================================
// clipping group
// ===========================================================================
void register_clipping(task::Registry& reg) {
    reg.add("clipping", "multi_axis_polygon_clipping", [](task::Case& tc) {
        using S = double;
        auto tris = make_multiaxis_scene<S>();
        typename Builder<S>::Config cfg;
        cfg.max_extra_references = 400;
        cfg.min_leaf_size = cfg.max_leaf_size = 1;
        auto r = Builder<S>::build(std::span<const T3<S>>(tris), cfg);

        tc.check(r.spatial_split_count > 0, "multi-axis scene must trigger real spatial splits");
        size_t multi_axis_refs = 0;
        for (size_t slot = 0; slot < r.bvh.prim_ids.size(); ++slot) {
            const auto tb = tris[r.bvh.prim_ids[slot]].get_bbox();
            const auto& cell = r.reference_cells[slot];
            int tightened_axes = 0;
            // Restrict to the two in-plane axes: this proves one fragment was clipped in
            // succession along two genuinely different axes, not merely separated in z.
            for (size_t a = 0; a < 2; ++a)
                if (cell.min[a] > tb.min[a] + S(1e-9) || cell.max[a] < tb.max[a] - S(1e-9))
                    ++tightened_axes;
            if (tightened_axes >= 2)
                ++multi_axis_refs;
        }
        tc.check(multi_axis_refs > 0, "a reference is clipped in succession along two axes");
        verify_partition(tc, r, tris);
    });

    reg.add("clipping", "vertex_plane_and_shared_edge", [](task::Case& tc) {
        using S = float;
        auto tris = make_vertex_edge_scene<S>();
        typename Builder<S>::Config cfg;
        cfg.max_extra_references = 128;
        cfg.min_leaf_size = cfg.max_leaf_size = 1;
        auto r = Builder<S>::build(std::span<const T3<S>>(tris), cfg);

        // A cell face must pass through the apex x=0 or the shared edge plane y=0,
        // proving the on-plane vertex/edge is handled without losing coverage.
        bool plane_through_feature = false;
        for (const auto& cell : r.reference_cells)
            for (size_t a = 0; a < 2; ++a)
                if (std::abs(cell.min[a]) < S(1e-5) || std::abs(cell.max[a]) < S(1e-5))
                    plane_through_feature = true;
        tc.check(plane_through_feature, "a clipping plane passes through a vertex or shared edge");
        tc.check(r.bvh.prim_ids.size() >= tris.size(), "no original triangle was dropped");
        verify_partition(tc, r, tris);
    });

    reg.add("clipping", "flat_zero_thickness_coverage", [](task::Case& tc) {
        using S = double;
        auto tris = make_flat_scene<S>();
        typename Builder<S>::Config cfg;
        cfg.max_extra_references = 64;
        auto r = Builder<S>::build(std::span<const T3<S>>(tris), cfg);

        bool degenerate_boxes_kept = true;
        for (const auto& b : r.reference_bounds)
            if (!(b.min[2] <= b.max[2]))
                degenerate_boxes_kept = false;
        tc.check(degenerate_boxes_kept, "zero-thickness fragment boxes stay valid and closed");
        std::set<size_t> ids(r.bvh.prim_ids.begin(), r.bvh.prim_ids.end());
        tc.check(ids.size() == tris.size(), "every flat triangle remains represented");
        verify_partition(tc, r, tris);
    });
}

// ===========================================================================
// budget group
// ===========================================================================
void register_budget(task::Registry& reg) {
    reg.add("budget", "zero_budget_never_duplicates", [](task::Case& tc) {
        using S = double;
        auto tris = make_sliver_scene<S>();
        typename Builder<S>::Config cfg;
        cfg.max_extra_references = 0;
        auto r = Builder<S>::build(std::span<const T3<S>>(tris), cfg);

        tc.check(r.spatial_split_count == 0, "no spatial split occurs with a zero budget");
        tc.check(r.bvh.prim_ids.size() == tris.size(), "zero budget keeps exactly one reference per face");
        std::set<size_t> ids(r.bvh.prim_ids.begin(), r.bvh.prim_ids.end());
        tc.check(ids.size() == tris.size(), "all original faces are still present");
        verify_partition(tc, r, tris);
    });

    reg.add("budget", "exactly_one_duplication", [](task::Case& tc) {
        using S = double;
        auto tris = make_single_straddle_scene<S>();
        typename Builder<S>::Config cfg;
        cfg.max_extra_references = 1;
        cfg.min_leaf_size = cfg.max_leaf_size = 1;
        auto r = Builder<S>::build(std::span<const T3<S>>(tris), cfg);

        const size_t extra = r.bvh.prim_ids.size() - tris.size();
        tc.check(extra == 1, "the single shared reference is duplicated exactly once");
        tc.check(r.spatial_split_count == 1, "exactly one spatial split is committed");
        tc.check(r.bvh.prim_ids.size() <= tris.size() + cfg.max_extra_references,
            "the global reference cap is respected");
        verify_partition(tc, r, tris);
    });

    reg.add("budget", "multi_level_competition_and_repeatability", [](task::Case& tc) {
        using S = float;
        auto tris = make_multiaxis_scene<S>(8);

        typename Builder<S>::Config small;
        small.max_extra_references = 8;
        small.min_leaf_size = small.max_leaf_size = 1;
        auto tight = Builder<S>::build(std::span<const T3<S>>(tris), small);

        const size_t tight_extra = tight.bvh.prim_ids.size() - tris.size();
        tc.check(tight_extra <= small.max_extra_references,
            "competing subtrees together never exceed the shared tree budget");
        tc.check(tight.bvh.prim_ids.size() == tris.size() + tight_extra,
            "reference accounting stays consistent when the budget runs out");

        typename Builder<S>::Config large = small;
        large.max_extra_references = 400;
        auto roomy = Builder<S>::build(std::span<const T3<S>>(tris), large);
        tc.check(roomy.spatial_split_count > tight.spatial_split_count,
            "a larger shared budget permits more spatial splits");

        // Independent builds must not share budget state and must be deterministic.
        auto repeat = Builder<S>::build(std::span<const T3<S>>(tris), large);
        tc.check(repeat.bvh.prim_ids == roomy.bvh.prim_ids &&
                     repeat.spatial_split_count == roomy.spatial_split_count,
            "repeated builds are deterministic and do not leak budget between calls");
        verify_partition(tc, roomy, tris);
    });
}

// ===========================================================================
// queries group
// ===========================================================================
void register_queries(task::Registry& reg) {
    reg.add("queries", "nearest_hit_identity_and_barycentrics_indirect", [](task::Case& tc) {
        using S = double;
        // Scatter triangles across the volume, with some long straddlers.
        std::vector<T3<S>> tris = make_sliver_scene<S>();
        for (int i = 0; i < 12; ++i) {
            S x = -8 + S(i) * S(1.3);
            tris.emplace_back(V3<S>(x, -12, S(i) * S(0.2)),
                              V3<S>(x + S(0.8), -12, S(i) * S(0.2)),
                              V3<S>(x + S(0.4), -11, S(i) * S(0.2) + S(0.3)));
        }
        typename Builder<S>::Config cfg;
        cfg.max_extra_references = 200;
        auto r = Builder<S>::build(std::span<const T3<S>>(tris), cfg);

        // Non-normalized directions on purpose (note the z components of 2 and 3).
        const std::array<Ray<S, 3>, 6> rays = {
            Ray<S, 3>(V3<S>(0, S(-0.125), 0), V3<S>(0, 0, 2), 0, 100),
            Ray<S, 3>(V3<S>(1, 1, 0), V3<S>(S(0.05), 0, 3), 0, 100),
            Ray<S, 3>(V3<S>(0, S(-0.125), 0), V3<S>(0, 0, 1), 0, 100),
            Ray<S, 3>(V3<S>(-9, S(-11.5), 0), V3<S>(0, 0, 1), 0, 100),
            Ray<S, 3>(V3<S>(7, 9, 0), V3<S>(0, 0, 1), 0, 100),
            Ray<S, 3>(V3<S>(-5, 2, 0), V3<S>(0, 0, 1), 0, 100),
        };

        size_t matched = 0, miss_matched = 0;
        const S tol = S(1e-7);
        for (const auto& ray : rays) {
            auto got = trace_indirect(r.bvh, tris, ray);
            auto expect = brute_nearest(tris, ray);
            tc.check((got.id == -1) == !expect.has_value(), "hit/miss status matches brute force");
            if (expect) {
                bool ok = got.id == long(expect->id) &&
                          std::abs(got.t - expect->t) < tol &&
                          std::abs(got.u - expect->u) < tol &&
                          std::abs(got.v - expect->v) < tol;
                if (ok) ++matched;
                tc.check(ok, "nearest face id, distance and barycentrics match the brute force result");
            } else {
                ++miss_matched;
            }
        }
        tc.check(matched >= 3 && miss_matched >= 1, "both genuine hits and misses were exercised");
    });

    reg.add("queries", "any_hit_shadow_window_precomputed", [](task::Case& tc) {
        using S = float;
        auto tris = make_sliver_scene<S>();
        typename Builder<S>::Config cfg;
        cfg.max_extra_references = 128;
        auto r = Builder<S>::build(std::span<const T3<S>>(tris), cfg);
        auto ordered = make_ordered(r.bvh, tris);

        tc.check(ordered.size() == r.bvh.prim_ids.size(),
            "the precomputed reference array has one entry per reference slot");

        struct Q { V3<S> org; V3<S> dir; S tmax; };
        const std::array<Q, 6> qs = {
            Q{ V3<S>(0, S(-0.125), 0), V3<S>(0, 0, 1), 100 },   // hits
            Q{ V3<S>(0, S(-0.125), 0), V3<S>(0, 0, 1), S(0.5) },// window closes before the hit
            Q{ V3<S>(-5, 2, 0), V3<S>(0, 0, 1), 100 },          // misses
            Q{ V3<S>(3, 5, 0), V3<S>(S(0.2), 0, 1), 100 },
            Q{ V3<S>(-9, 9, 0), V3<S>(0, 0, 1), 100 },
            Q{ V3<S>(2, -8, 0), V3<S>(0, 0, 1), S(1.05) },      // tight window around first layer
        };
        size_t agree = 0;
        for (const auto& q : qs) {
            Ray<S, 3> ray(q.org, q.dir, 0, q.tmax);
            bool tree_says = trace_occluded_indirect(r.bvh, tris, ray);
            bool brute_says = brute_any_hit(tris, ray);
            if (tree_says == brute_says) ++agree;
            tc.check(tree_says == brute_says, "any-hit result with the finite window matches brute force");
        }
        tc.check(agree == qs.size(), "every shadow-ray window case agrees");

        // The precomputed layout must produce identical nearest results.
        Ray<S, 3> ray(V3<S>(0, S(-0.125), 0), V3<S>(0, 0, 1), 0, 100);
        auto pc = trace_precomputed(r.bvh, ordered, ray);
        auto bf = brute_nearest(tris, ray);
        tc.check(bf && pc.id == long(bf->id) && std::abs(pc.t - bf->t) < S(1e-4),
            "precomputed-slot layout reports the same nearest face and distance");
    });

    reg.add("queries", "duplicated_face_still_traces_correctly", [](task::Case& tc) {
        using S = double;
        auto tris = make_sliver_scene<S>();
        typename Builder<S>::Config cfg;
        cfg.max_extra_references = 256;
        auto r = Builder<S>::build(std::span<const T3<S>>(tris), cfg);
        tc.check(r.spatial_split_count > 0, "the scene really contains duplicated original faces");

        size_t duplicate_faces = 0;
        {
            std::vector<size_t> count(tris.size(), 0);
            for (size_t id : r.bvh.prim_ids) ++count[id];
            for (size_t c : count)
                if (c > 1) ++duplicate_faces;
        }
        tc.check(duplicate_faces > 0, "at least one original face is reached through several references");

        // A ray aimed at a duplicated face returns the original face id regardless of which
        // spatial reference is visited; no mailboxing is required.
        Ray<S, 3> ray(V3<S>(0, S(-0.125), 0), V3<S>(0, 0, 1), 0, 100);
        auto got = trace_indirect(r.bvh, tris, ray);
        auto bf = brute_nearest(tris, ray);
        tc.check(bf && got.id == long(bf->id) && std::abs(got.t - bf->t) < S(1e-9),
            "a face reached through duplicated references keeps its original id and distance");

        // The same query through the per-slot precomputed layout (double precision) must agree.
        auto ordered = make_ordered(r.bvh, tris);
        auto pc = trace_precomputed(r.bvh, ordered, ray);
        tc.check(pc.id == got.id && std::abs(pc.t - got.t) < S(1e-9) &&
                     std::abs(pc.u - got.u) < S(1e-9) && std::abs(pc.v - got.v) < S(1e-9),
            "indirect and per-slot precomputed layouts agree in double precision");
    });
}

// ===========================================================================
// integration group
// ===========================================================================
void register_integration(task::Registry& reg) {
    reg.add("integration", "sliver_scene_fewer_triangle_tests_than_object_tree", [](task::Case& tc) {
        using S = double;
        auto tris = make_sliver_scene<S>();
        std::vector<B3<S>> boxes;
        std::vector<V3<S>> centers;
        for (const auto& t : tris) {
            boxes.push_back(t.get_bbox());
            centers.push_back(t.get_center());
        }
        auto object_tree = BinnedSahBuilder<N3<S>, 16>::build(
            std::span<const B3<S>>(boxes), std::span<const V3<S>>(centers));

        typename Builder<S>::Config cfg;
        cfg.max_extra_references = tris.size() * 8;
        auto spatial = Builder<S>::build(std::span<const T3<S>>(tris), cfg);
        tc.check(spatial.spatial_split_count > 0, "the spatial builder performs real splits on the baseline scene");

        // One nearest ray plus the eight leaving-surface rays from the baseline. Misses count.
        std::vector<Ray<S, 3>> rays;
        rays.emplace_back(V3<S>(0, S(-0.125), 0), V3<S>(0, 0, 1), 0, 100);
        for (int i = 0; i < 8; ++i)
            rays.emplace_back(V3<S>(-5, S(2 + i * 0.5), 0), V3<S>(0, 0, 1), 0, 100);

        size_t object_tests = 0, spatial_tests = 0;
        size_t object_miss_tests = 0, spatial_miss_tests = 0;
        for (size_t k = 0; k < rays.size(); ++k) {
            auto oh = trace_indirect(object_tree, tris, rays[k]);
            auto sh = trace_indirect(spatial.bvh, tris, rays[k]);
            object_tests += oh.tests;
            spatial_tests += sh.tests;
            if (oh.id == -1)
                object_miss_tests += oh.tests;
            if (sh.id == -1)
                spatial_miss_tests += sh.tests;
        }
        // Record the actual triangle-test counts (miss rays included), as required.
        std::cout << "SLIVER_TRI_TESTS object=" << object_tests
                  << " spatial=" << spatial_tests
                  << " object_miss_only=" << object_miss_tests
                  << " spatial_miss_only=" << spatial_miss_tests << "\n";
        tc.check(object_miss_tests > 0 && spatial_miss_tests > 0,
            "miss rays are part of the recorded triangle-test statistics");
        tc.check(spatial_tests < object_tests,
            "the spatial tree performs strictly fewer triangle tests (miss rays included)");

        auto nearest = trace_indirect(spatial.bvh, tris, rays[0]);
        auto bf = brute_nearest(tris, rays[0]);
        tc.check(bf && nearest.id == long(bf->id) && std::abs(nearest.t - bf->t) < S(1e-9),
            "nearest-hit correctness is preserved while reducing intersection work");
    });

    reg.add("integration", "ppm_render_and_serialization_roundtrip", [](task::Case& tc) {
        using S = float;
        // Small colorful scene: a few slanted triangles with per-vertex colors.
        std::vector<T3<S>> tris;
        tris.emplace_back(V3<S>(-6, -6, 2), V3<S>(6, -6, 2), V3<S>(0, 6, 2));
        tris.emplace_back(V3<S>(-6, 6, 4), V3<S>(6, 6, 4), V3<S>(0, -6, 4));
        tris.emplace_back(V3<S>(-8, 0, 7), V3<S>(8, 0, 7), V3<S>(0, 8, 7));
        const std::array<std::array<V3<S>, 3>, 3> colors = {{
            {{ V3<S>(1, 0, 0), V3<S>(0, 1, 0), V3<S>(0, 0, 1) }},
            {{ V3<S>(1, 1, 0), V3<S>(0, 1, 1), V3<S>(1, 0, 1) }},
            {{ V3<S>(1, 1, 1), V3<S>(S(0.2), S(0.2), S(0.2)), V3<S>(S(0.5), 0, S(0.8)) }},
        }};

        typename Builder<S>::Config cfg;
        cfg.max_extra_references = 64;
        auto built = Builder<S>::build(std::span<const T3<S>>(tris), cfg);
        auto ordered = make_ordered(built.bvh, tris);

        const int W = 24, H = 18;
        auto render = [&](const Bvh<N3<S>>& bvh) {
            std::vector<V3<S>> pixels(size_t(W) * H, V3<S>(0, 0, 0));
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) {
                    S px = S(-8) + S(16) * (S(x) + S(0.5)) / S(W);
                    S py = S(-8) + S(16) * (S(y) + S(0.5)) / S(H);
                    Ray<S, 3> ray(V3<S>(px, py, 0), V3<S>(0, 0, 1), 0, 100);
                    auto hit = trace_precomputed(bvh, ordered, ray);
                    if (hit.id >= 0) {
                        const auto& c = colors[size_t(hit.id)];
                        S w = S(1) - hit.u - hit.v;
                        pixels[size_t(y) * W + x] =
                            c[0] * w + c[1] * hit.u + c[2] * hit.v;
                    }
                }
            }
            return pixels;
        };

        auto tree_pixels = render(built.bvh);

        // Independent brute-force reference image, including barycentric interpolation.
        std::vector<V3<S>> ref_pixels(size_t(W) * H, V3<S>(0, 0, 0));
        size_t colored = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                S px = S(-8) + S(16) * (S(x) + S(0.5)) / S(W);
                S py = S(-8) + S(16) * (S(y) + S(0.5)) / S(H);
                Ray<S, 3> ray(V3<S>(px, py, 0), V3<S>(0, 0, 1), 0, 100);
                auto hit = brute_nearest(tris, ray);
                if (hit) {
                    ++colored;
                    const auto& c = colors[hit->id];
                    S w = S(1) - hit->u - hit->v;
                    ref_pixels[size_t(y) * W + x] = c[0] * w + c[1] * hit->u + c[2] * hit->v;
                }
            }
        }
        tc.check(colored > W * H / 2, "the reference image contains a substantial amount of geometry");
        size_t mismatched = 0;
        for (size_t i = 0; i < tree_pixels.size(); ++i)
            for (size_t a = 0; a < 3; ++a)
                if (std::abs(tree_pixels[i][a] - ref_pixels[i][a]) > S(1e-4))
                    ++mismatched;
        tc.check(mismatched == 0, "every rendered pixel and barycentric color matches brute force");

        // Serialize the BVH (no extra cache format) and render again from the deserialized tree.
        std::stringstream bytes;
        StdOutputStream out(bytes);
        built.bvh.serialize(out);
        StdInputStream in(bytes);
        auto restored = Bvh<N3<S>>::deserialize(in);
        tc.check(restored == built.bvh, "serialization round trip preserves the whole tree");
        auto roundtrip_pixels = render(restored);
        size_t rt_mismatch = 0;
        for (size_t i = 0; i < roundtrip_pixels.size(); ++i)
            for (size_t a = 0; a < 3; ++a)
                if (std::abs(roundtrip_pixels[i][a] - ref_pixels[i][a]) > S(1e-4))
                    ++rt_mismatch;
        tc.check(rt_mismatch == 0, "the deserialized tree renders an identical image");

        // Emit a small windowless binary PPM (P6) built from the tree image and sanity-check it.
        std::ostringstream ppm(std::ios::binary);
        ppm << "P6\n" << W << " " << H << "\n255\n";
        for (const auto& col : tree_pixels) {
            std::array<uint8_t, 3> rgb;
            for (size_t a = 0; a < 3; ++a) {
                S v = robust_min(S(1), robust_max(S(0), col[a]));
                rgb[a] = static_cast<uint8_t>(std::lround(v * S(255)));
            }
            ppm.put(char(rgb[0]));
            ppm.put(char(rgb[1]));
            ppm.put(char(rgb[2]));
        }
        std::string ppm_bytes = ppm.str();
        tc.check(ppm_bytes.rfind("P6", 0) == 0 &&
                     ppm_bytes.size() == std::string("P6\n24 18\n255\n").size() + size_t(W) * H * 3,
            "a valid windowless PPM with one byte per channel per pixel is produced");
    });

    reg.add("integration", "empty_input_and_overflow_contract", [](task::Case& tc) {
        using S = double;

        auto empty = Builder<S>::build(std::span<const T3<S>>{});
        tc.check(empty.bvh.nodes.empty() && empty.bvh.prim_ids.empty(),
            "an empty input produces an empty tree");
        tc.check(empty.reference_bounds.empty() && empty.reference_cells.empty() &&
                     empty.spatial_split_count == 0,
            "an empty input produces empty reference arrays and no spatial splits");

        // A node type with 8 index bits and 2 primitive-count bits can only address 64 slots.
        using SmallNode = Node<S, 3, 8, 2>;
        using SmallBuilder = SpatialSahBuilder<SmallNode, 16>;
        std::vector<T3<S>> many;
        for (size_t i = 0; i < 100; ++i) {
            S x = S(i) * S(0.1);
            many.emplace_back(V3<S>(x, 0, 0), V3<S>(x + S(0.05), 1, 0), V3<S>(x + S(0.05), 1, 1));
        }
        bool threw_on_overflow = false;
        try {
            // Leaf sizes must stay legal for this small node type (max 3 primitives per leaf).
            typename SmallBuilder::Config small_cfg;
            small_cfg.min_leaf_size = 1;
            small_cfg.max_leaf_size = 3;
            [[maybe_unused]] auto overflowing =
                SmallBuilder::build(std::span<const T3<S>>(many), small_cfg);
        } catch (const std::length_error&) {
            threw_on_overflow = true;
        }
        tc.check(threw_on_overflow, "a reference/index overflow throws std::length_error instead of truncating");

        // The same scene built with a wide node type succeeds and stays complete.
        auto fine = Builder<S>::build(std::span<const T3<S>>(many));
        std::set<size_t> ids(fine.bvh.prim_ids.begin(), fine.bvh.prim_ids.end());
        tc.check(ids.size() == many.size(), "a non-overflowing build still covers every input face");

        // A leaf size that the node index cannot represent is rejected up front.
        bool illegal_leaf = false;
        try {
            typename SmallBuilder::Config bad_cfg;
            bad_cfg.max_leaf_size = 4; // exceeds the 2-bit primitive count (max 3)
            [[maybe_unused]] auto illegal =
                SmallBuilder::build(std::span<const T3<S>>(many), bad_cfg);
        } catch (const std::invalid_argument&) {
            illegal_leaf = true;
        }
        tc.check(illegal_leaf, "an unrepresentable leaf size throws std::invalid_argument");
    });
}

} // namespace

void register_spatial_tests(task::Registry& registry) {
    register_clipping(registry);
    register_budget(registry);
    register_queries(registry);
    register_integration(registry);
}
