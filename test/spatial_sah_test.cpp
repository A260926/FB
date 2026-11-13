#include "registry.hpp"

#include <bvh/v2/spatial_sah_builder.h>
#include <bvh/v2/binned_sah_builder.h>
#include <bvh/v2/stack.h>
#include <bvh/v2/stream.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/ray.h>

#include <vector>
#include <span>
#include <string>
#include <sstream>
#include <fstream>
#include <optional>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <algorithm>
#include <functional>

namespace {

using std::size_t;

template <typename T>
using V3 = bvh::v2::Vec<T, 3>;
template <typename T>
using Box = bvh::v2::BBox<T, 3>;
template <typename T>
using Tri3 = bvh::v2::Tri<T, 3>;
template <typename T>
using N3 = bvh::v2::Node<T, 3>;
template <typename T>
using Tree = bvh::v2::Bvh<N3<T>>;
template <typename T>
using Ray3 = bvh::v2::Ray<T, 3>;
template <typename T>
using PTri = bvh::v2::PrecomputedTri<T>;
template <typename T>
using Builder = bvh::v2::SpatialSahBuilder<N3<T>, 16>;

// ---------------------------------------------------------------------------
// Independent geometry reference: convex polygon clipping against an axis
// plane. This is deliberately written independently of the builder.
// ---------------------------------------------------------------------------

template <typename T>
bool vec_ne(const V3<T>& a, const V3<T>& b) {
    for (size_t i = 0; i < 3; ++i)
        if (a[i] != b[i])
            return true;
    return false;
}

template <typename T>
std::vector<V3<T>> reference_clip(
    const std::vector<V3<T>>& polygon, size_t axis, T plane, bool keep_left)
{
    std::vector<V3<T>> out;
    out.reserve(polygon.size() + 1);
    const auto inside = [&] (T d) { return keep_left ? d <= 0 : d >= 0; };
    for (size_t i = 0; i < polygon.size(); ++i) {
        const auto& a = polygon[i];
        const auto& b = polygon[(i + 1) % polygon.size()];
        const T da = a[axis] - plane;
        const T db = b[axis] - plane;
        const bool ai = inside(da);
        const bool bi = inside(db);
        if (ai != bi) {
            const T t = da / (da - db);
            V3<T> q;
            for (size_t k = 0; k < 3; ++k)
                q[k] = k == axis ? plane : a[k] + t * (b[k] - a[k]);
            if (out.empty() || vec_ne(out.back(), q))
                out.push_back(q);
        }
        if (bi && (out.empty() || vec_ne(out.back(), b)))
            out.push_back(b);
    }
    return out;
}

template <typename T>
std::vector<V3<T>> triangle_polygon(const Tri3<T>& tri) {
    return { tri.p0, tri.p1, tri.p2 };
}

/// Reconstructs a fragment polygon by clipping the original triangle to the
/// six closed faces of its cumulative clipping cell.
template <typename T>
std::vector<V3<T>> reconstruct_fragment(const Tri3<T>& tri, const Box<T>& cell) {
    auto poly = triangle_polygon(tri);
    for (size_t axis = 0; axis < 3; ++axis) {
        poly = reference_clip(poly, axis, cell.max[axis], true);
        if (poly.empty())
            return poly;
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        poly = reference_clip(poly, axis, cell.min[axis], false);
        if (poly.empty())
            return poly;
    }
    return poly;
}

template <typename T>
Box<T> polygon_bbox(const std::vector<V3<T>>& poly) {
    auto b = Box<T>::make_empty();
    for (const auto& p : poly)
        b.extend(p);
    return b;
}

template <typename T>
T scalar_eps_scale() {
    return std::numeric_limits<T>::epsilon();
}

template <typename T>
T tolerance(T max_abs) {
    return T(256) * scalar_eps_scale<T>() * std::max(T(1), max_abs);
}

template <typename T>
T max_abs_coords(const std::vector<Tri3<T>>& tris) {
    T m = 0;
    for (const auto& t : tris)
        for (const auto* p : { &t.p0, &t.p1, &t.p2 })
            for (size_t i = 0; i < 3; ++i)
                m = std::max(m, std::abs((*p)[i]));
    return m;
}

template <typename T>
bool box_contains_point(const Box<T>& b, const V3<T>& p, T tol) {
    for (size_t i = 0; i < 3; ++i)
        if (p[i] < b.min[i] - tol || p[i] > b.max[i] + tol)
            return false;
    return true;
}

/// Generic convex-containment test for coplanar 3D polygons. `tol` is a
/// positional (length) tolerance; it is scaled per edge into a signed-distance
/// tolerance so the test is independent of the polygon's size and orientation.
template <typename T>
bool point_in_convex_polygon(const std::vector<V3<T>>& poly, const V3<T>& p, T tol) {
    if (poly.size() < 3)
        return false;
    // Newell-style normal from the polygon edges.
    V3<T> normal {};
    for (size_t i = 0; i < poly.size(); ++i) {
        const auto& a = poly[i];
        const auto& b = poly[(i + 1) % poly.size()];
        normal[0] += a[1] * b[2] - a[2] * b[1];
        normal[1] += a[2] * b[0] - a[0] * b[2];
        normal[2] += a[0] * b[1] - a[1] * b[0];
    }
    const T normal_len = length(normal);
    if (!(normal_len > 0))
        return false;
    T orient = 0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const auto& a = poly[i];
        const auto& b = poly[(i + 1) % poly.size()];
        orient += dot(cross(b - a, p - a), normal);
    }
    for (size_t i = 0; i < poly.size(); ++i) {
        const auto& a = poly[i];
        const auto& b = poly[(i + 1) % poly.size()];
        const auto edge = b - a;
        // g / (|edge| * |normal|) is the signed distance from p to this edge.
        const T g = dot(cross(edge, p - a), normal);
        const T edge_tol = tol * length(edge) * normal_len;
        if (orient >= 0 ? g < -edge_tol : g > edge_tol)
            return false;
    }
    return true;
}

template <typename T>
V3<T> bary_point(const Tri3<T>& tri, T w0, T w1, T w2) {
    return tri.p0 * w0 + tri.p1 * w1 + tri.p2 * w2;
}

// ---------------------------------------------------------------------------
// Independent brute-force ray/triangle intersection (Moller-Trumbore). The
// returned (u, v) are the weights of p1 and p2, matching PrecomputedTri.
// ---------------------------------------------------------------------------

template <typename T>
struct BruteHit {
    size_t id;
    T t, u, v;
};

template <typename T>
std::optional<BruteHit<T>> brute_intersect(
    const Tri3<T>& tri, size_t id, const Ray3<T>& ray, T det_eps)
{
    const auto e1 = tri.p1 - tri.p0;
    const auto e2 = tri.p2 - tri.p0;
    const auto pv = cross(ray.dir, e2);
    const T det = dot(pv, e1);
    if (std::abs(det) < det_eps)
        return std::nullopt;
    const T inv = T(1) / det;
    const auto tv = ray.org - tri.p0;
    const T u = dot(tv, pv) * inv;
    if (u < -det_eps || u > T(1) + det_eps)
        return std::nullopt;
    const auto qv = cross(tv, e1);
    const T v = dot(ray.dir, qv) * inv;
    if (v < -det_eps || u + v > T(1) + det_eps)
        return std::nullopt;
    const T t = dot(e2, qv) * inv;
    if (t < ray.tmin || t > ray.tmax)
        return std::nullopt;
    return BruteHit<T> { id, t, u, v };
}

template <typename T>
std::optional<BruteHit<T>> brute_nearest(
    const std::vector<Tri3<T>>& tris, const Ray3<T>& ray)
{
    const T det_eps = T(1e-9) * std::max(T(1), T(100));
    auto probe = ray;
    std::optional<BruteHit<T>> best;
    for (size_t i = 0; i < tris.size(); ++i) {
        if (auto h = brute_intersect(tris[i], i, probe, det_eps)) {
            probe.tmax = h->t;
            best = h;
        }
    }
    return best;
}

template <typename T>
bool brute_occluded(const std::vector<Tri3<T>>& tris, const Ray3<T>& ray) {
    const T det_eps = T(1e-9) * std::max(T(1), T(100));
    for (size_t i = 0; i < tris.size(); ++i)
        if (brute_intersect(tris[i], i, ray, det_eps))
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// BVH query helpers for both primitive layouts.
// ---------------------------------------------------------------------------

template <typename T>
struct QueryHit {
    bool hit = false;
    size_t prim_id = size_t(-1);
    T t = 0, u = 0, v = 0;
};

/// Layout 0: indirect, read the original triangle through prim_ids.
template <typename T>
QueryHit<T> query_indirect(const Tree<T>& bvh, const std::vector<Tri3<T>>& tris, Ray3<T> ray) {
    bvh::v2::GrowingStack<typename Tree<T>::Index> stack;
    QueryHit<T> result;
    bvh.template intersect<false, true>(ray, bvh.get_root().index, stack,
        [&] (size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const size_t id = bvh.prim_ids[i];
                if (auto q = PTri<T>(tris[id]).intersect(ray)) {
                    std::tie(ray.tmax, result.u, result.v) = *q;
                    result.prim_id = id;
                    result.hit = true;
                }
            }
            return result.hit;
        });
    result.t = ray.tmax;
    return result;
}

/// Layout 1: precomputed triangles permuted into reference-slot order.
template <typename T>
QueryHit<T> query_permuted(const Tree<T>& bvh, const std::vector<PTri<T>>& pre, Ray3<T> ray) {
    bvh::v2::GrowingStack<typename Tree<T>::Index> stack;
    QueryHit<T> result;
    bvh.template intersect<false, true>(ray, bvh.get_root().index, stack,
        [&] (size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (auto q = pre[i].intersect(ray)) {
                    std::tie(ray.tmax, result.u, result.v) = *q;
                    result.prim_id = bvh.prim_ids[i];
                    result.hit = true;
                }
            }
            return result.hit;
        });
    result.t = ray.tmax;
    return result;
}

template <typename T>
bool occluded_indirect(const Tree<T>& bvh, const std::vector<Tri3<T>>& tris, const Ray3<T>& ray) {
    bvh::v2::GrowingStack<typename Tree<T>::Index> stack;
    bool hit = false;
    bvh.template intersect<true, true>(ray, bvh.get_root().index, stack,
        [&] (size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const size_t id = bvh.prim_ids[i];
                if (PTri<T>(tris[id]).intersect(ray)) {
                    hit = true;
                    return true;
                }
            }
            return false;
        });
    return hit;
}

template <typename T>
bool occluded_permuted(const Tree<T>& bvh, const std::vector<PTri<T>>& pre, const Ray3<T>& ray) {
    bvh::v2::GrowingStack<typename Tree<T>::Index> stack;
    bool hit = false;
    bvh.template intersect<true, true>(ray, bvh.get_root().index, stack,
        [&] (size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                if (pre[i].intersect(ray)) {
                    hit = true;
                    return true;
                }
            return false;
        });
    return hit;
}

template <typename T>
std::vector<PTri<T>> permute_precomputed(
    const Tree<T>& bvh, const std::vector<Tri3<T>>& tris)
{
    std::vector<PTri<T>> pre(bvh.prim_ids.size());
    for (size_t i = 0; i < pre.size(); ++i)
        pre[i] = PTri<T>(tris[bvh.prim_ids[i]]);
    return pre;
}

// Deterministic LCG so runs are reproducible.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed) {}
    double next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(state >> 33) / static_cast<double>(1ull << 31);
    }
    double range(double lo, double hi) { return lo + (hi - lo) * next(); }
};

template <typename T>
void assert_hit_equals(task::Case& tc, const char* label,
    const std::optional<BruteHit<T>>& expected, const QueryHit<T>& got, T tol)
{
    tc.check(expected.has_value() == got.hit, std::string(label) + " hit presence");
    if (expected && got.hit) {
        tc.check(expected->id == got.prim_id, std::string(label) + " primitive id");
        tc.check(std::abs(expected->t - got.t) <= tol, std::string(label) + " hit distance");
        tc.check(std::abs(expected->u - got.u) <= tol &&
                 std::abs(expected->v - got.v) <= tol,
                 std::string(label) + " barycentric coordinates");
    }
}

// ---------------------------------------------------------------------------
// Scene builders.
// ---------------------------------------------------------------------------

// Long, thin, slanted triangles stacked along z: the baseline pathological case.
template <typename T>
std::vector<Tri3<T>> make_slim_slabs(size_t count) {
    std::vector<Tri3<T>> tris;
    for (size_t i = 0; i < count; ++i) {
        const T z = T(1) + T(i) / T(64);
        tris.emplace_back(
            V3<T>(T(-10), T(-10), z),
            V3<T>(T(10), T(10), z),
            V3<T>(T(10), T(9.5), z));
    }
    return tris;
}

// A grid of long, thin horizontal and vertical strips lying in the same tight z
// band. Individual strips span the full node along one axis while being thin on
// the other, which forces spatial splits on both axes: the same reference ends
// up clipped successively along different axes (and more than once per axis).
template <typename T>
std::vector<Tri3<T>> make_cross_grid_scene() {
    std::vector<Tri3<T>> tris;
    // Horizontal thin strips spanning the full x range, stacked along y.
    for (size_t i = 0; i < 8; ++i) {
        const T y = T(-8) + T(i) * T(2);
        const T z = T(1) + T(i) * T(0.01);
        tris.emplace_back(V3<T>(T(-8), y, z),
                          V3<T>(T(8), y, z),
                          V3<T>(T(8), y + T(0.6), z));
    }
    // Vertical thin strips spanning the full y range, stacked along x.
    for (size_t i = 0; i < 8; ++i) {
        const T x = T(-8) + T(i) * T(2);
        const T z = T(1.05) + T(i) * T(0.01);
        tris.emplace_back(V3<T>(x, T(-8), z),
                          V3<T>(x + T(0.6), T(-8), z),
                          V3<T>(x, T(8), z));
    }
    return tris;
}

// A mix of long diagonal thin triangles that overlap heavily in the same tight
// z band (so object partitions cannot separate them and spatial splits win),
// including a vertex placed exactly on a central grid plane and a pair sharing
// an edge. All triangles span x from roughly -8 to 8, which makes the x = 0
// grid line an internal split plane (root range is [-8, 8]).
template <typename T>
std::vector<Tri3<T>> make_clipping_scene() {
    std::vector<Tri3<T>> tris;
    // Long thin diagonal triangle spanning x and y with a near-zero thickness.
    tris.emplace_back(V3<T>(T(-8), T(-8), T(1)),
                      V3<T>(T(8), T(8), T(1)),
                      V3<T>(T(8), T(7.5), T(1)));
    // Triangle with a vertex exactly on the central x = 0 plane.
    tris.emplace_back(V3<T>(T(0), T(-6), T(1.04)),
                      V3<T>(T(-6), T(2), T(1.04)),
                      V3<T>(T(6), T(2), T(1.04)));
    // Two triangles sharing an edge that lies on x = 0.
    tris.emplace_back(V3<T>(T(0), T(-4), T(1.08)),
                      V3<T>(T(0), T(4), T(1.08)),
                      V3<T>(T(-5), T(0), T(1.08)));
    tris.emplace_back(V3<T>(T(0), T(-4), T(1.08)),
                      V3<T>(T(5), T(0), T(1.08)),
                      V3<T>(T(0), T(4), T(1.08)));
    // A fat triangle covering the middle to force competing bins.
    tris.emplace_back(V3<T>(T(-7), T(-2), T(1.12)),
                      V3<T>(T(7), T(-2), T(1.12)),
                      V3<T>(T(0), T(3), T(1.12)));
    return tris;
}

// ===========================================================================
// clipping group
// ===========================================================================

template <typename T>
void test_fragment_boxes_match_reference_clip(task::Case& tc) {
    auto tris = make_cross_grid_scene<T>();
    typename Builder<T>::Config cfg;
    cfg.max_extra_references = 256;
    auto result = Builder<T>::build(std::span<const Tri3<T>>(tris), cfg);
    const T max_abs = max_abs_coords(tris);
    const T tol = tolerance(max_abs);
    const T pad = T(32) * scalar_eps_scale<T>() * std::max(T(1), max_abs);

    tc.check(result.spatial_split_count > 0, "clipping scene must trigger spatial splits");
    tc.check(result.reference_bounds.size() == result.bvh.prim_ids.size(),
             "reference_bounds aligned with prim_ids slots");
    tc.check(result.reference_cells.size() == result.bvh.prim_ids.size(),
             "reference_cells aligned with prim_ids slots");

    size_t multi_axis_cells = 0;
    size_t same_axis_twice = 0;
    for (size_t slot = 0; slot < result.bvh.prim_ids.size(); ++slot) {
        const size_t id = result.bvh.prim_ids[slot];
        const auto& orig_box = tris[id].get_bbox();
        const auto& cell = result.reference_cells[slot];
        const auto& bounds = result.reference_bounds[slot];

        // The cumulative cell can never extend past the original triangle box.
        for (size_t k = 0; k < 3; ++k) {
            tc.check(cell.min[k] >= orig_box.min[k] - tol &&
                     cell.max[k] <= orig_box.max[k] + tol,
                     "cell stays within the original triangle box");
        }

        auto poly = reconstruct_fragment(tris[id], cell);
        tc.check(poly.size() >= 3, "reconstructed fragment is a non-degenerate polygon");

        const auto tight = polygon_bbox(poly);
        for (const auto& p : poly)
            tc.check(box_contains_point(bounds, p, tol),
                     "reported fragment bounds contain the clipped polygon");
        for (size_t k = 0; k < 3; ++k) {
            tc.check(std::abs(bounds.min[k] - (tight.min[k] - pad)) <= tol,
                     "fragment min bound is tight (not the original triangle box)");
            tc.check(std::abs(bounds.max[k] - (tight.max[k] + pad)) <= tol,
                     "fragment max bound is tight (not the original triangle box)");
            tc.check(poly.empty() || box_contains_point(cell, poly.front(), tol),
                     "cell contains the fragment region");
        }

        size_t clamped_axes = 0;
        bool twice = false;
        for (size_t k = 0; k < 3; ++k) {
            const bool lo = std::abs(cell.min[k] - orig_box.min[k]) > tol * T(4);
            const bool hi = std::abs(cell.max[k] - orig_box.max[k]) > tol * T(4);
            if (lo || hi)
                ++clamped_axes;
            if (lo && hi)
                twice = true;
        }
        if (clamped_axes >= 2)
            ++multi_axis_cells;
        if (twice)
            ++same_axis_twice;
    }
    tc.check(multi_axis_cells > 0,
             "some references were clipped successively along different axes");
    tc.check(same_axis_twice > 0,
             "some references were clipped more than once on the same axis");
}

template <typename T>
void test_coverage_with_vertex_and_shared_edge(task::Case& tc) {
    auto tris = make_clipping_scene<T>();
    typename Builder<T>::Config cfg;
    cfg.max_extra_references = 96;
    auto result = Builder<T>::build(std::span<const Tri3<T>>(tris), cfg);
    const T max_abs = max_abs_coords(tris);
    const T tol = tolerance(max_abs);

    // Group reconstructed fragment polygons per original triangle.
    std::vector<std::vector<std::vector<V3<T>>>> fragments(tris.size());
    for (size_t slot = 0; slot < result.bvh.prim_ids.size(); ++slot) {
        const size_t id = result.bvh.prim_ids[slot];
        fragments[id].push_back(
            reconstruct_fragment(tris[id], result.reference_cells[slot]));
    }

    size_t total_points = 0, covered_points = 0;
    const size_t lattice = 18;
    for (size_t id = 0; id < tris.size(); ++id) {
        for (size_t a = 0; a <= lattice; ++a) {
            for (size_t b = 0; b + a <= lattice; ++b) {
                const size_t c = lattice - a - b;
                const T w0 = T(c) / T(lattice);
                const T w1 = T(a) / T(lattice);
                const T w2 = T(b) / T(lattice);
                const auto p = bary_point(tris[id], w0, w1, w2);
                ++total_points;
                for (const auto& poly : fragments[id]) {
                    if (point_in_convex_polygon(poly, p, tol)) {
                        ++covered_points;
                        break;
                    }
                }
            }
        }
        // The three vertices must always stay covered.
        for (const auto& vertex : { tris[id].p0, tris[id].p1, tris[id].p2 }) {
            bool covered = false;
            for (const auto& poly : fragments[id])
                covered |= point_in_convex_polygon(poly, vertex, tol);
            tc.check(covered, "triangle vertex stays covered after spatial clipping");
        }
    }
    tc.check(total_points > 0 && covered_points == total_points,
             "adjacent fragments together cover every point of every original triangle");

    // The shared edge (x = 0) must be covered on both sides: sample its midpoint
    // and check it belongs to fragments of both owning triangles.
    const V3<T> edge_mid(T(0), T(0), T(1.08));
    for (size_t id : { size_t(2), size_t(3) }) {
        bool covered = false;
        for (const auto& poly : fragments[id])
            covered |= point_in_convex_polygon(poly, edge_mid, tol);
        tc.check(covered, "point on a shared edge is covered by each adjacent triangle");
    }
}

// ===========================================================================
// budget group
// ===========================================================================

template <typename T>
void test_zero_budget_object_only(task::Case& tc) {
    auto tris = make_slim_slabs<T>(24);
    typename Builder<T>::Config cfg;
    cfg.max_extra_references = 0;
    auto result = Builder<T>::build(std::span<const Tri3<T>>(tris), cfg);

    tc.check(result.spatial_split_count == 0, "zero budget means no spatial split");
    tc.check(result.bvh.prim_ids.size() == tris.size(),
             "zero budget keeps exactly one reference per input triangle");

    std::vector<size_t> seen(tris.size(), 0);
    for (size_t id : result.bvh.prim_ids)
        ++seen[id];
    const bool permutation = std::all_of(seen.begin(), seen.end(),
        [] (size_t count) { return count == 1; });
    tc.check(permutation, "prim_ids is a permutation of the input triangles");

    // Even with no spatial splits, nearest queries still work.
    Ray3<T> ray(V3<T>(T(0), T(-0.125), T(0)), V3<T>(T(0), T(0), T(1)), T(0), T(100));
    auto got = query_indirect(result.bvh, tris, ray);
    auto expected = brute_nearest(tris, ray);
    assert_hit_equals(tc, "zero-budget query", expected, got, tolerance(T(10)));
}

// Two perpendicular full-span strips crossed with two corner triangles. A
// spatial plane along one strip cuts exactly the perpendicular strip once, so
// the cheapest spatial split duplicates precisely one reference.
template <typename T>
std::vector<Tri3<T>> make_single_straddler_scene() {
    std::vector<Tri3<T>> tris;
    // Horizontal strip spanning the full x range, thin in y.
    tris.emplace_back(V3<T>(T(-10), T(-0.5), T(1)),
                      V3<T>(T(10), T(-0.5), T(1)),
                      V3<T>(T(10), T(0.5), T(1)));
    // Vertical strip spanning the full y range, thin in x.
    tris.emplace_back(V3<T>(T(-0.5), T(-10), T(1.1)),
                      V3<T>(T(0.5), T(-10), T(1.1)),
                      V3<T>(T(-0.5), T(10), T(1.1)));
    // Two small corner triangles that stay fully on one side of the planes.
    tris.emplace_back(V3<T>(T(-9), T(8), T(1.2)),
                      V3<T>(T(-8), T(8), T(1.2)),
                      V3<T>(T(-8.5), T(9), T(1.2)));
    tris.emplace_back(V3<T>(T(8), T(-9), T(1.3)),
                      V3<T>(T(9), T(-9), T(1.3)),
                      V3<T>(T(8.5), T(-8), T(1.3)));
    return tris;
}

template <typename T>
void test_exact_single_duplication(task::Case& tc) {
    auto tris = make_single_straddler_scene<T>();
    const size_t n = tris.size();

    typename Builder<T>::Config unlimited;
    unlimited.max_extra_references = 128;
    auto full = Builder<T>::build(std::span<const Tri3<T>>(tris), unlimited);
    tc.check(full.spatial_split_count > 0, "scene benefits from a spatial split");
    tc.check(full.bvh.prim_ids.size() > n, "unlimited build duplicates references");

    typename Builder<T>::Config cfg;
    cfg.max_extra_references = 1;
    auto r1 = Builder<T>::build(std::span<const Tri3<T>>(tris), cfg);
    auto r2 = Builder<T>::build(std::span<const Tri3<T>>(tris), cfg);
    tc.check(r1.bvh.prim_ids.size() == n + 1,
             "budget of one extra reference yields exactly one duplication");
    tc.check(r1.spatial_split_count >= 1, "the single affordable duplication is a spatial split");
    tc.check(r1.bvh == r2.bvh &&
             r1.spatial_split_count == r2.spatial_split_count,
             "repeated builds are deterministic and do not share budget state");

    // Every input triangle remains reachable.
    std::vector<size_t> coverage(n, 0);
    for (size_t id : r1.bvh.prim_ids)
        ++coverage[id];
    tc.check(std::all_of(coverage.begin(), coverage.end(),
              [] (size_t c) { return c >= 1; }),
             "no triangle is lost when the budget is exhausted");
}

template <typename T>
void test_multi_level_budget_competition(task::Case& tc) {
    // Every slab straddles the central plane, so a root spatial split would
    // duplicate all of them at once and compete for the small shared budget.
    auto tris = make_slim_slabs<T>(12);
    const size_t n = tris.size();

    typename Builder<T>::Config cfg;
    cfg.max_extra_references = 3;
    auto r1 = Builder<T>::build(std::span<const Tri3<T>>(tris), cfg);
    auto r2 = Builder<T>::build(std::span<const Tri3<T>>(tris), cfg);

    tc.check(r1.bvh.prim_ids.size() <= n + 3,
             "total reference count never exceeds the shared budget");
    tc.check(r1.bvh.prim_ids.size() >= n, "object partitioning keeps all primitives");
    tc.check(r1.bvh == r2.bvh, "independent builds with a tight budget agree");

    std::vector<size_t> coverage(n, 0);
    for (size_t id : r1.bvh.prim_ids)
        ++coverage[id];
    tc.check(std::all_of(coverage.begin(), coverage.end(),
              [] (size_t c) { return c >= 1; }),
             "the full tree is built even when spatial splits are unaffordable");

    const T tol = tolerance(max_abs_coords(tris));
    std::vector<std::vector<std::vector<V3<T>>>> fragments(tris.size());
    for (size_t slot = 0; slot < r1.bvh.prim_ids.size(); ++slot) {
        const size_t id = r1.bvh.prim_ids[slot];
        fragments[id].push_back(reconstruct_fragment(tris[id], r1.reference_cells[slot]));
    }
    size_t points = 0, covered = 0;
    for (size_t id = 0; id < tris.size(); ++id) {
        for (size_t a = 0; a <= 12; ++a)
            for (size_t b = 0; b + a <= 12; ++b) {
                const auto p = bary_point(tris[id],
                    T(12 - a - b) / T(12), T(a) / T(12), T(b) / T(12));
                ++points;
                for (const auto& poly : fragments[id])
                    if (point_in_convex_polygon(poly, p, tol)) { ++covered; break; }
            }
    }
    tc.check(points > 0 && covered == points,
             "coverage is complete after object splits take over from exhausted budget");
}

// ===========================================================================
// queries group
// ===========================================================================

template <typename T>
void test_nearest_hit_brute_force(task::Case& tc) {
    auto tris = make_clipping_scene<T>();
    typename Builder<T>::Config cfg;
    cfg.max_extra_references = 48;
    auto result = Builder<T>::build(std::span<const Tri3<T>>(tris), cfg);
    auto pre = permute_precomputed(result.bvh, tris);
    tc.check(pre.size() == result.bvh.prim_ids.size(),
             "permuted precomputed array has one entry per reference slot");

    const T tol = sizeof(T) == 4 ? T(1e-4) : T(1e-9);
    Rng rng(uint64_t(sizeof(T)) * 7919 + 11);
    size_t hit_rays = 0, miss_rays = 0;
    for (int trial = 0; trial < 120; ++trial) {
        Ray3<T> ray;
        ray.org = V3<T>(T(rng.range(-9, 9)), T(rng.range(-9, 9)), T(rng.range(-4, 0)));
        // Unnormalized directions with varying magnitudes, some pointing away.
        ray.dir = V3<T>(T(rng.range(-0.6, 0.6)), T(rng.range(-0.6, 0.6)),
                        T(rng.range(0.4, 1.6)));
        ray.tmin = T(0);
        ray.tmax = T(40);
        auto expected = brute_nearest(tris, ray);
        expected ? ++hit_rays : ++miss_rays;

        auto got_indirect = query_indirect(result.bvh, tris, ray);
        assert_hit_equals(tc, "indirect nearest", expected, got_indirect, tol);

        auto got_permuted = query_permuted(result.bvh, pre, ray);
        assert_hit_equals(tc, "permuted nearest", expected, got_permuted, tol);
    }
    tc.check(hit_rays > 0, "random nearest-hit suite contains real hits");
    tc.check(miss_rays > 0, "random nearest-hit suite contains genuine misses");
}

template <typename T>
void test_occlusion_finite_window(task::Case& tc) {
    auto tris = make_slim_slabs<T>(16);
    typename Builder<T>::Config cfg;
    cfg.max_extra_references = 32;
    auto result = Builder<T>::build(std::span<const Tri3<T>>(tris), cfg);
    auto pre = permute_precomputed(result.bvh, tris);

    Rng rng(uint64_t(sizeof(T)) * 104729 + 7);
    size_t occluded = 0, clear = 0;
    for (int trial = 0; trial < 100; ++trial) {
        Ray3<T> ray;
        ray.org = V3<T>(T(rng.range(-10, 10)), T(rng.range(-10, 10)), T(rng.range(-2, 0.5)));
        ray.dir = V3<T>(T(rng.range(-0.4, 0.4)), T(rng.range(-0.4, 0.4)),
                        T(rng.range(0.5, 1.5)));
        ray.tmin = T(0);
        // A deliberately finite, sometimes short window.
        ray.tmax = T(rng.range(0.5, 12));
        const bool expected = brute_occluded(tris, ray);
        expected ? ++occluded : ++clear;

        tc.check(occluded_indirect(result.bvh, tris, ray) == expected,
                 "indirect any-hit matches brute force");
        tc.check(occluded_permuted(result.bvh, pre, ray) == expected,
                 "permuted any-hit matches brute force");
    }
    tc.check(occluded > 0, "shadow suite contains occluded rays");
    tc.check(clear > 0, "shadow suite contains rays that escape within the finite window");

    // A ray whose window closes before the nearest slab must report a miss.
    Ray3<T> short_ray(V3<T>(0), V3<T>(T(0), T(0), T(1)), T(0), T(0.25));
    tc.check(!occluded_indirect(result.bvh, tris, short_ray),
             "a finite window ending before geometry is not occluded");
}

// ===========================================================================
// integration group
// ===========================================================================

struct TraceStats {
    size_t triangle_tests = 0;
    bool hit = false;
    size_t id = size_t(-1);
};

template <typename T>
TraceStats trace_indirect_counted(
    const Tree<T>& bvh, const std::vector<Tri3<T>>& tris, Ray3<T> ray)
{
    bvh::v2::GrowingStack<typename Tree<T>::Index> stack;
    TraceStats stats;
    bvh.template intersect<false, true>(ray, bvh.get_root().index, stack,
        [&] (size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                ++stats.triangle_tests; // misses are counted just like hits.
                const size_t id = bvh.prim_ids[i];
                if (auto q = PTri<T>(tris[id]).intersect(ray)) {
                    ray.tmax = std::get<0>(*q);
                    stats.hit = true;
                    stats.id = id;
                }
            }
            return stats.hit;
        });
    return stats;
}

void test_baseline_fewer_triangle_tests(task::Case& tc) {
    using T = double;
    auto tris = make_slim_slabs<T>(32);
    std::vector<Box<T>> boxes(tris.size());
    std::vector<V3<T>> centers(tris.size());
    for (size_t i = 0; i < tris.size(); ++i) {
        boxes[i] = tris[i].get_bbox();
        centers[i] = tris[i].get_center();
    }
    auto object_tree = bvh::v2::BinnedSahBuilder<N3<T>, 16>::build(
        std::span<const Box<T>>(boxes), std::span<const V3<T>>(centers));

    typename Builder<T>::Config cfg;
    cfg.max_extra_references = 256;
    auto spatial = Builder<T>::build(std::span<const Tri3<T>>(tris), cfg);
    tc.check(spatial.spatial_split_count > 0,
             "the slim-slab scene must produce real spatial splits");
    tc.check(spatial.bvh.prim_ids.size() > tris.size(),
             "spatial references are tighter and more numerous than input faces");

    // The same ray set for both trees: one nearest hit plus eight miss rays.
    size_t object_tests = 0, spatial_tests = 0;
    Ray3<T> hit_ray(V3<T>(T(0), T(-0.125), T(0)), V3<T>(T(0), T(0), T(1)), T(0), T(100));
    object_tests += trace_indirect_counted(object_tree, tris, hit_ray).triangle_tests;
    spatial_tests += trace_indirect_counted(spatial.bvh, tris, hit_ray).triangle_tests;
    for (int i = 0; i < 8; ++i) {
        Ray3<T> miss(V3<T>(T(-5), T(2) + T(i) * T(0.5), T(0)),
                     V3<T>(T(0), T(0), T(1)), T(0), T(100));
        object_tests += trace_indirect_counted(object_tree, tris, miss).triangle_tests;
        spatial_tests += trace_indirect_counted(spatial.bvh, tris, miss).triangle_tests;
    }
    tc.check(spatial_tests < object_tests,
             "spatial BVH performs strictly fewer triangle tests (miss rays included)");
    tc.check(object_tests >= 256, "object tree exhibits the baseline 256 wasted intersections");

    auto nearest = trace_indirect_counted(
        spatial.bvh, tris,
        Ray3<T>(V3<T>(T(0), T(-0.125), T(0)), V3<T>(T(0), T(0), T(1)), T(0), T(100)));
    tc.check(nearest.hit && nearest.id == 0, "nearest-hit identity is preserved");
}

template <typename T>
struct RenderPixel {
    uint8_t r = 0, g = 0, b = 0;
    T u = 0, v = 0, t = 0;
    size_t id = size_t(-1);
    bool hit = false;
};

template <typename T>
struct ColoredTri {
    Tri3<T> tri;
    V3<T> c0, c1, c2; // colors at p0, p1, p2, components in [0, 1]
};

template <typename T>
RenderPixel<T> render_brute(
    const std::vector<ColoredTri<T>>& scene, const Ray3<T>& ray)
{
    const T det_eps = T(1e-9) * T(100);
    RenderPixel<T> pixel;
    auto probe = ray;
    std::optional<BruteHit<T>> best;
    for (size_t i = 0; i < scene.size(); ++i) {
        if (auto h = brute_intersect(scene[i].tri, i, probe, det_eps)) {
            probe.tmax = h->t;
            best = h;
        }
    }
    if (!best)
        return pixel;
    const auto& s = scene[best->id];
    const T w0 = T(1) - best->u - best->v;
    const auto color = s.c0 * w0 + s.c1 * best->u + s.c2 * best->v;
    auto to_byte = [] (T x) {
        x = std::min(T(1), std::max(T(0), x));
        return static_cast<uint8_t>(std::lround(x * T(255)));
    };
    pixel.hit = true;
    pixel.id = best->id;
    pixel.t = best->t;
    pixel.u = best->u;
    pixel.v = best->v;
    pixel.r = to_byte(color[0]);
    pixel.g = to_byte(color[1]);
    pixel.b = to_byte(color[2]);
    return pixel;
}

template <typename T>
RenderPixel<T> render_bvh(
    const Tree<T>& bvh, const std::vector<ColoredTri<T>>& scene,
    const std::vector<PTri<T>>& pre, const Ray3<T>& ray)
{
    RenderPixel<T> pixel;
    auto got = query_permuted(bvh, pre, ray);
    if (!got.hit)
        return pixel;
    const auto& s = scene[got.prim_id];
    const T w0 = T(1) - got.u - got.v;
    const auto color = s.c0 * w0 + s.c1 * got.u + s.c2 * got.v;
    auto to_byte = [] (T x) {
        x = std::min(T(1), std::max(T(0), x));
        return static_cast<uint8_t>(std::lround(x * T(255)));
    };
    pixel.hit = true;
    pixel.id = got.prim_id;
    pixel.t = got.t;
    pixel.u = got.u;
    pixel.v = got.v;
    pixel.r = to_byte(color[0]);
    pixel.g = to_byte(color[1]);
    pixel.b = to_byte(color[2]);
    return pixel;
}

template <typename T>
void test_ppm_and_serialization(task::Case& tc) {
    std::vector<ColoredTri<T>> scene;
    scene.push_back(ColoredTri<T> {
        Tri3<T>(V3<T>(T(-6), T(-6), T(5)),
                V3<T>(T(6), T(-6), T(5)),
                V3<T>(T(0), T(6), T(5))),
        V3<T>(T(1), T(0), T(0)),
        V3<T>(T(0), T(1), T(0)),
        V3<T>(T(0), T(0), T(1)) });
    scene.push_back(ColoredTri<T> {
        Tri3<T>(V3<T>(T(-5), T(-4), T(8)),
                V3<T>(T(5), T(-4), T(8)),
                V3<T>(T(5), T(4), T(8))),
        V3<T>(T(1), T(1), T(0)),
        V3<T>(T(0), T(1), T(1)),
        V3<T>(T(1), T(0), T(1)) });
    std::vector<Tri3<T>> tris;
    for (auto& s : scene)
        tris.push_back(s.tri);

    typename Builder<T>::Config cfg;
    cfg.max_extra_references = 16;
    auto built = Builder<T>::build(std::span<const Tri3<T>>(tris), cfg);
    auto pre = permute_precomputed(built.bvh, tris);

    constexpr size_t width = 28, height = 28;
    std::vector<RenderPixel<T>> brute_pixels(width * height);
    std::vector<RenderPixel<T>> bvh_pixels(width * height);
    size_t colored = 0;
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            Ray3<T> ray;
            ray.org = V3<T>(
                T(-8) + T(16) * (T(x) + T(0.5)) / T(width),
                T(-8) + T(16) * (T(y) + T(0.5)) / T(height),
                T(0));
            ray.dir = V3<T>(T(0), T(0), T(1)); // unnormalized magnitude 1
            ray.tmin = T(0);
            ray.tmax = T(100);
            const size_t idx = y * width + x;
            brute_pixels[idx] = render_brute(scene, ray);
            bvh_pixels[idx] = render_bvh(built.bvh, scene, pre, ray);
            const auto& a = brute_pixels[idx];
            const auto& b = bvh_pixels[idx];
            tc.check(a.hit == b.hit && a.id == b.id &&
                     a.r == b.r && a.g == b.g && a.b == b.b,
                     "PPM pixel color and face match brute force");
            const T tol = sizeof(T) == 4 ? T(1e-4) : T(1e-9);
            if (a.hit) {
                ++colored;
                tc.check(std::abs(a.t - b.t) <= tol &&
                         std::abs(a.u - b.u) <= tol &&
                         std::abs(a.v - b.v) <= tol,
                         "PPM pixel barycentric interpolation matches brute force");
            }
        }
    }
    tc.check(colored > 0, "rendered image contains shaded pixels");

    // Emit a small windowless binary PPM (P6).
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = tmp ? tmp : ".";
    std::string path = dir + "/spatial_sah_render.ppm";
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << width << " " << height << "\n255\n";
    for (const auto& p : bvh_pixels)
        out << p.r << p.g << p.b;
    out.close();
    std::ifstream in(path, std::ios::binary);
    std::string header;
    std::getline(in, header);
    tc.check(out.good() && header == "P6", "windowless PPM image is written to disk");

    // Serialization round trip must preserve queries.
    std::stringstream bytes;
    bvh::v2::StdOutputStream sout(bytes);
    built.bvh.serialize(sout);
    bvh::v2::StdInputStream sin(bytes);
    auto restored = Tree<T>::deserialize(sin);
    tc.check(restored == built.bvh, "BVH survives serialize/deserialize unchanged");
    Ray3<T> probe(V3<T>(T(0), T(0), T(0)), V3<T>(T(0), T(0), T(1)), T(0), T(100));
    auto before = query_permuted(built.bvh, pre, probe);
    auto after = query_permuted(restored, pre, probe);
    tc.check(before.hit == after.hit && before.prim_id == after.prim_id &&
             std::abs(before.t - after.t) <= T(sizeof(T) == 4 ? 1e-4 : 1e-9),
             "deserialized BVH returns the same nearest hit");
}

// ===========================================================================
// Edge cases: empty input and index overflow.
// ===========================================================================

template <typename T>
void test_empty_input(task::Case& tc) {
    auto result = Builder<T>::build(std::span<const Tri3<T>> {});
    tc.check(result.bvh.nodes.empty() && result.bvh.prim_ids.empty(),
             "empty input produces an empty tree");
    tc.check(result.reference_bounds.empty() && result.reference_cells.empty(),
             "empty input produces empty reference arrays");
    tc.check(result.spatial_split_count == 0, "empty input reports no spatial splits");
}

void test_length_error_on_overflow(task::Case& tc) {
    using SmallNode = bvh::v2::Node<float, 3, 8, 4>; // 4 index bits -> max first id 15
    using SmallBuilder = bvh::v2::SpatialSahBuilder<SmallNode, 16>;
    std::vector<Tri3<float>> tris;
    for (size_t i = 0; i < 40; ++i) {
        const float y = static_cast<float>(i) * 0.25f;
        tris.emplace_back(V3<float>(-8, y, 1), V3<float>(8, y, 1), V3<float>(8, y + 0.1f, 1));
    }

    SmallBuilder::Config cfg;
    cfg.max_extra_references = 0;
    bool threw = false;
    try {
        (void)SmallBuilder::build(std::span<const Tri3<float>>(tris), cfg);
    } catch (const std::length_error&) {
        threw = true;
    }
    tc.check(threw, "a reference count beyond the node index throws length_error");

    SmallBuilder::Config bad;
    bad.min_leaf_size = 4;
    bad.max_leaf_size = 2; // invalid: min > max
    bool bad_threw = false;
    try {
        (void)SmallBuilder::build(std::span<const Tri3<float>>(tris), bad);
    } catch (const std::length_error&) {
        bad_threw = true;
    }
    tc.check(bad_threw, "invalid leaf size parameters throw length_error");
}

} // namespace

void register_spatial_tests(task::Registry& registry) {
    registry.add("clipping", "fragment_boxes_match_reference_clip_double",
        [](task::Case& tc) { test_fragment_boxes_match_reference_clip<double>(tc); });
    registry.add("clipping", "coverage_vertex_shared_edge_float",
        [](task::Case& tc) { test_coverage_with_vertex_and_shared_edge<float>(tc); });
    registry.add("clipping", "empty_input_double",
        [](task::Case& tc) { test_empty_input<double>(tc); });

    registry.add("budget", "zero_budget_object_only_double",
        [](task::Case& tc) { test_zero_budget_object_only<double>(tc); });
    registry.add("budget", "exact_single_duplication_float",
        [](task::Case& tc) { test_exact_single_duplication<float>(tc); });
    registry.add("budget", "multi_level_competition_double",
        [](task::Case& tc) { test_multi_level_budget_competition<double>(tc); });
    registry.add("budget", "length_error_on_overflow_float",
        [](task::Case& tc) { test_length_error_on_overflow(tc); });

    registry.add("queries", "nearest_hit_brute_force_double",
        [](task::Case& tc) { test_nearest_hit_brute_force<double>(tc); });
    registry.add("queries", "nearest_hit_brute_force_float",
        [](task::Case& tc) { test_nearest_hit_brute_force<float>(tc); });
    registry.add("queries", "occlusion_finite_window_float",
        [](task::Case& tc) { test_occlusion_finite_window<float>(tc); });

    registry.add("integration", "baseline_slim_triangles_fewer_tests",
        [](task::Case& tc) { test_baseline_fewer_triangle_tests(tc); });
    registry.add("integration", "ppm_render_serialization_double",
        [](task::Case& tc) { test_ppm_and_serialization<double>(tc); });
    registry.add("integration", "ppm_render_serialization_float",
        [](task::Case& tc) { test_ppm_and_serialization<float>(tc); });
}
