#ifndef BVH_V2_SPATIAL_SAH_BUILDER_H
#define BVH_V2_SPATIAL_SAH_BUILDER_H

#include "bvh/v2/bvh.h"
#include "bvh/v2/vec.h"
#include "bvh/v2/bbox.h"
#include "bvh/v2/tri.h"
#include "bvh/v2/node.h"
#include "bvh/v2/split_heuristic.h"
#include "bvh/v2/top_down_sah_builder.h"

#include <span>
#include <array>
#include <vector>
#include <stack>
#include <algorithm>
#include <numeric>
#include <limits>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace bvh::v2 {

/// Top-down, triangle-only SAH builder that supports spatial splits in addition to regular object
/// splits. With a spatial split, a triangle that straddles the splitting plane is not assigned to a
/// single child: instead, its current fragment is clipped against the plane and a (tighter) spatial
/// reference is created on each side. Several spatial references can therefore point at the same
/// original triangle, but the original triangle itself is never split: `prim_ids` always contains
/// indices into the array of triangles passed by the caller, and the same index can appear several
/// times.
///
/// Only three-dimensional nodes over `float` or `double` are supported.
template <typename Node, size_t BinCount = 16>
class SpatialSahBuilder {
    static_assert(Node::dimension == 3, "SpatialSahBuilder only supports 3-dimensional nodes");
    static_assert(
        std::is_same_v<typename Node::Scalar, float> ||
        std::is_same_v<typename Node::Scalar, double>,
        "SpatialSahBuilder only supports float or double scalars");

public:
    using Scalar = typename Node::Scalar;
    using Vec    = bvh::v2::Vec<Scalar, 3>;
    using BBox   = bvh::v2::BBox<Scalar, 3>;
    using Tri    = bvh::v2::Tri<Scalar, 3>;
    using Index  = typename Node::Index;

    struct Config : TopDownSahBuilder<Node>::Config {
        /// Maximum number of additional spatial references that the whole tree is allowed to
        /// contain relative to the number of input triangles. A value of 0 disables spatial
        /// splits entirely. This budget is shared across the whole tree: candidate splits are
        /// always evaluated freely, but references are only duplicated when a spatial split is
        /// actually committed.
        size_t max_extra_references = 0;
    };

    struct Result {
        Bvh<Node> bvh;

        /// Tight bounding box of every reference, ordered by the slots in `bvh.prim_ids`.
        std::vector<BBox> reference_bounds;

        /// Closed spatial region covered by every reference (the intersection of all the
        /// half-spaces introduced by its ancestor spatial splits), ordered by `bvh.prim_ids`.
        std::vector<BBox> reference_cells;

        /// Number of inner nodes that were produced by a spatial split.
        size_t spatial_split_count = 0;
    };

    [[nodiscard]] static Result build(std::span<const Tri> tris, const Config& config = {}) {
        return SpatialSahBuilder(tris, config).build();
    }

private:
    /// A convex fragment of an original triangle. Clipping a convex polygon against one
    /// half-space adds at most one vertex, so a fragment created by `d` spatial splits has at most
    /// `3 + d` vertices. The capacity below comfortably covers any tree depth that can be
    /// represented by the node index; the clipping routine has a defensive fallback for the
    /// (unreachable in practice) case where it would overflow.
    static constexpr size_t MaxPolyVerts = 64;

    struct Ref {
        size_t prim_id = 0;
        std::array<Vec, MaxPolyVerts> poly {};
        uint8_t poly_count = 0;
        BBox tight;       // Tight bounds of the clipped polygon (with the allowed slack).
        BBox cell;        // Accumulated closed clipping region.
        Vec center;
    };

    struct WorkItem {
        size_t node_id = 0;
        std::vector<Ref> refs;
        BBox bbox = BBox::make_empty();
    };

    enum class SplitKind { None, Object, Spatial };

    struct Split {
        SplitKind kind = SplitKind::None;
        Scalar cost = std::numeric_limits<Scalar>::max();
        size_t axis = 0;
        Scalar plane = 0;
        size_t left_count = 0;   // Object split: refs on the left.
        size_t right_count = 0;  // Object split: refs on the right.
        size_t straddle_count = 0; // Spatial split: duplicated references.
        BBox left_bbox = BBox::make_empty();
        BBox right_bbox = BBox::make_empty();
    };

    struct Bin {
        BBox bbox = BBox::make_empty();
        size_t prim_count = 0;

        BVH_ALWAYS_INLINE void add(const BBox& other) {
            bbox.extend(other);
            ++prim_count;
        }

        BVH_ALWAYS_INLINE Scalar get_cost(const SplitHeuristic<Scalar>& sah) const {
            return sah.get_leaf_cost(0, prim_count, bbox);
        }
    };

    std::span<const Tri> tris_;
    const Config& config_;
    Scalar slack_;

    SpatialSahBuilder(std::span<const Tri> tris, const Config& config)
        : tris_(tris), config_(config)
    {
        if (config_.min_leaf_size == 0 || config_.min_leaf_size > config_.max_leaf_size)
            throw std::invalid_argument("SpatialSahBuilder: invalid leaf size configuration");
        if (config_.max_leaf_size > static_cast<size_t>(Index::max_prim_count))
            throw std::invalid_argument(
                "SpatialSahBuilder: max_leaf_size cannot be represented by the node index");
    }

    /// Clips a convex polygon against the axis-aligned half-space `x[axis] <= plane` (when
    /// `keep_left` is true) or `x[axis] >= plane` (otherwise). The half-space is closed, so
    /// vertices lying exactly on the plane are kept. Returns the number of vertices written to
    /// `out` (which must hold at least `n + 1` slots).
    static size_t clip_polygon(
        const Vec* in, size_t n, size_t axis, Scalar plane, bool keep_left, Vec* out)
    {
        auto inside = [&] (const Vec& v) {
            return keep_left ? v[axis] <= plane : v[axis] >= plane;
        };

        size_t count = 0;
        if (n == 0)
            return 0;
        for (size_t i = 0; i < n; ++i) {
            const Vec& s = in[i];
            const Vec& e = in[(i + 1) % n];
            const bool in_s = inside(s);
            const bool in_e = inside(e);
            if (in_e) {
                if (!in_s)
                    out[count++] = intersect_edge(s, e, axis, plane);
                out[count++] = e;
            } else if (in_s) {
                out[count++] = intersect_edge(s, e, axis, plane);
            }
        }
        return count;
    }

    static Vec intersect_edge(const Vec& s, const Vec& e, size_t axis, Scalar plane) {
        const Scalar t = (plane - s[axis]) / (e[axis] - s[axis]);
        return Vec::generate([&] (size_t i) {
            return i == axis ? plane : s[i] + t * (e[i] - s[i]);
        });
    }

    static BBox polygon_bbox(const Vec* poly, size_t count) {
        BBox bbox(poly[0]);
        for (size_t i = 1; i < count; ++i)
            bbox.extend(poly[i]);
        return bbox;
    }

    /// Expands a fragment box by the allowed conservative slack, without ever leaving the
    /// accumulated clipping cell.
    BBox make_tight_bbox(const BBox& poly_box, const BBox& cell) const {
        BBox out(
            Vec::generate([&] (size_t i) { return robust_max(poly_box.min[i] - slack_, cell.min[i]); }),
            Vec::generate([&] (size_t i) { return robust_min(poly_box.max[i] + slack_, cell.max[i]); }));
        return out;
    }

    static BBox compute_bbox(const std::vector<Ref>& refs) {
        auto bbox = BBox::make_empty();
        for (const auto& ref : refs)
            bbox.extend(ref.tight);
        return bbox;
    }

    /// Returns the extent of the fragment polygon along one axis.
    void polygon_extent(const Ref& ref, size_t axis, Scalar& lo, Scalar& hi) const {
        lo = hi = ref.poly[0][axis];
        for (size_t i = 1; i < ref.poly_count; ++i) {
            lo = robust_min(lo, ref.poly[i][axis]);
            hi = robust_max(hi, ref.poly[i][axis]);
        }
    }

    /// Classifies a reference against a splitting plane using its actual fragment polygon (not
    /// the slack-expanded box): -1 means entirely on the left, +1 entirely on the right, 0 means
    /// it genuinely straddles the plane. Vertices on the plane do not count as straddling.
    int classify(const Ref& ref, size_t axis, Scalar plane) const {
        Scalar lo, hi;
        polygon_extent(ref, axis, lo, hi);
        if (hi <= plane)
            return -1;
        if (lo >= plane)
            return +1;
        return 0;
    }

    /// Clips a fragment against one side of a plane and builds the resulting reference.
    Ref clip_ref(const Ref& ref, size_t axis, Scalar plane, bool keep_left) const {
        // Narrow the accumulated cell first. Since the cell only ever tightens one of its six
        // faces, at most six non-redundant half-spaces can ever be active, so a clipped triangle
        // has far fewer vertices than `MaxPolyVerts`; the capacity check stays as a guard.
        Ref child = ref;
        if (keep_left)
            child.cell.max[axis] = robust_min(ref.cell.max[axis], plane);
        else
            child.cell.min[axis] = robust_max(ref.cell.min[axis], plane);

        std::array<Vec, MaxPolyVerts + 1> out {};
        size_t count = clip_polygon(
            ref.poly.data(), ref.poly_count, axis, plane, keep_left, out.data());

        if (count == 0) [[unlikely]] {
            // The plane only touches the fragment without cutting an interior: keep it whole. Its
            // polygon is inside the narrowed cell, so coverage is preserved conservatively.
            child.tight = make_tight_bbox(
                polygon_bbox(ref.poly.data(), ref.poly_count), child.cell);
            child.center = child.tight.get_center();
            return child;
        }
        if (count > MaxPolyVerts) [[unlikely]] {
            // Defensive: never truncate. Keep the parent polygon (still inside the narrowed cell)
            // and recompute a valid tight box. This cannot be reached for valid inputs.
            child.tight = make_tight_bbox(
                polygon_bbox(ref.poly.data(), ref.poly_count), child.cell);
            child.center = child.tight.get_center();
            return child;
        }

        for (size_t i = 0; i < count; ++i)
            child.poly[i] = out[i];
        child.poly_count = static_cast<uint8_t>(count);
        child.tight = make_tight_bbox(polygon_bbox(child.poly.data(), count), child.cell);
        child.center = child.tight.get_center();
        return child;
    }

    /// Computes the (slack-expanded) box of a fragment clipped to one side of a plane for the
    /// purpose of evaluating a candidate spatial split.
    BBox clipped_side_bbox(
        const Ref& ref, size_t axis, Scalar plane, bool keep_left, const BBox& region) const
    {
        std::array<Vec, MaxPolyVerts + 1> out {};
        size_t count = clip_polygon(
            ref.poly.data(), ref.poly_count, axis, plane, keep_left, out.data());
        BBox box = polygon_bbox(out.data(), count);
        // Reuse the region as the clipping cell for the candidate cost.
        return make_tight_bbox(box, region);
    }

    // ------------------------------------------------------------------
    // Object split candidates: references are binned by the center of their tight box.
    // ------------------------------------------------------------------
    void find_object_candidates(
        const std::vector<Ref>& refs, const BBox& node_bbox, Split& best) const
    {
        const auto diag = node_bbox.get_diagonal();
        for (size_t axis = 0; axis < 3; ++axis) {
            if (!(diag[axis] > 0))
                continue;
            const Scalar scale = static_cast<Scalar>(BinCount) / diag[axis];
            std::array<Bin, BinCount> bins {};
            for (const auto& ref : refs) {
                Scalar pos = (ref.center[axis] - node_bbox.min[axis]) * scale;
                size_t id = static_cast<size_t>(robust_max(pos, Scalar(0)));
                id = std::min(id, BinCount - 1);
                bins[id].add(ref.tight);
            }

            std::array<Scalar, BinCount> right_costs {};
            std::array<size_t, BinCount> right_counts {};
            Bin right_accum;
            for (size_t i = BinCount; i-- > 1;) {
                if (bins[i].prim_count)
                    right_accum.add(bins[i].bbox);
                right_costs[i] = right_accum.get_cost(config_.sah);
                right_counts[i] = right_accum.prim_count;
            }

            Bin left_accum;
            for (size_t i = 0; i + 1 < BinCount; ++i) {
                if (bins[i].prim_count)
                    left_accum.add(bins[i].bbox);
                if (left_accum.prim_count == 0 || right_counts[i + 1] == 0)
                    continue;
                const Scalar cost =
                    left_accum.get_cost(config_.sah) + right_costs[i + 1];
                // Object candidates are evaluated first and only replaced on a strict improvement,
                // so a tie keeps the non-replicating object split.
                if (cost < best.cost) {
                    const Scalar plane =
                        node_bbox.min[axis] +
                        static_cast<Scalar>(i + 1) * diag[axis] / static_cast<Scalar>(BinCount);
                    best = Split {
                        SplitKind::Object, cost, axis, plane,
                        left_accum.prim_count, right_counts[i + 1], 0,
                        left_accum.bbox, BBox::make_empty() };
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Spatial split candidates: one interior plane per equal slice of the node range, on every
    // axis. Straddling references are duplicated and billed on both sides.
    // ------------------------------------------------------------------
    void find_spatial_candidates(
        const std::vector<Ref>& refs, const BBox& node_bbox,
        size_t budget_left, Split& best) const
    {
        const auto diag = node_bbox.get_diagonal();
        for (size_t axis = 0; axis < 3; ++axis) {
            if (!(diag[axis] > 0))
                continue;
            for (size_t k = 1; k < BinCount; ++k) {
                const Scalar plane =
                    node_bbox.min[axis] +
                    static_cast<Scalar>(k) * diag[axis] / static_cast<Scalar>(BinCount);

                BBox left_region = node_bbox;
                left_region.max[axis] = plane;
                BBox right_region = node_bbox;
                right_region.min[axis] = plane;

                BBox left_bbox = BBox::make_empty();
                BBox right_bbox = BBox::make_empty();
                size_t n_left = 0, n_right = 0, straddle = 0;

                for (const auto& ref : refs) {
                    const int side = classify(ref, axis, plane);
                    if (side < 0) {
                        left_bbox.extend(ref.tight);
                        ++n_left;
                    } else if (side > 0) {
                        right_bbox.extend(ref.tight);
                        ++n_right;
                    } else {
                        ++straddle;
                        left_bbox.extend(clipped_side_bbox(ref, axis, plane, true, left_region));
                        right_bbox.extend(clipped_side_bbox(ref, axis, plane, false, right_region));
                        ++n_left;
                        ++n_right;
                    }
                }

                if (n_left == 0 || n_right == 0)
                    continue;
                // Candidates that cannot be committed within the remaining shared budget are
                // infeasible. Evaluating them never consumed the budget.
                if (straddle > budget_left)
                    continue;

                const Scalar cost =
                    config_.sah.get_leaf_cost(0, n_left, left_bbox) +
                    config_.sah.get_leaf_cost(0, n_right, right_bbox);
                if (cost < best.cost) {
                    best = Split {
                        SplitKind::Spatial, cost, axis, plane,
                        n_left, n_right, straddle, left_bbox, right_bbox };
                }
            }
        }
    }

    /// Fallback object partition that always makes progress, used when the SAH does not justify a
    /// split but the node contains more than `max_leaf_size` references.
    Split fallback_object_split(std::vector<Ref>& refs, const BBox& node_bbox) const {
        const size_t axis = node_bbox.get_diagonal().get_largest_axis();
        const size_t mid = (refs.size() + 1) / 2;
        std::partial_sort(refs.begin(), refs.begin() + static_cast<ptrdiff_t>(mid), refs.end(),
            [&] (const Ref& a, const Ref& b) { return a.center[axis] < b.center[axis]; });
        Split split;
        split.kind = SplitKind::Object;
        split.axis = axis;
        split.plane = refs[mid].center[axis];
        split.left_count = mid;
        split.right_count = refs.size() - mid;
        split.left_bbox = BBox::make_empty();
        for (size_t i = 0; i < mid; ++i)
            split.left_bbox.extend(refs[i].tight);
        split.right_bbox = BBox::make_empty();
        for (size_t i = mid; i < refs.size(); ++i)
            split.right_bbox.extend(refs[i].tight);
        return split;
    }

    void commit_object_split(
        std::vector<Ref>& refs, const Split& split,
        std::vector<Ref>& left_refs, std::vector<Ref>& right_refs) const
    {
        const size_t axis = split.axis;
        left_refs.clear();
        right_refs.clear();
        // Object splits introduce no clipping half-space, so each reference keeps its accumulated
        // spatial cell; only the partition changes.
        for (auto& ref : refs) {
            if (ref.center[axis] < split.plane)
                left_refs.push_back(std::move(ref));
            else
                right_refs.push_back(std::move(ref));
        }
        if (left_refs.empty() || right_refs.empty()) {
            // Degenerate centers: recover a progressing partition.
            left_refs.clear();
            right_refs.clear();
            const size_t mid = (refs.size() + 1) / 2;
            for (size_t i = 0; i < refs.size(); ++i)
                (i < mid ? left_refs : right_refs).push_back(std::move(refs[i]));
        }
    }

    void commit_spatial_split(
        const std::vector<Ref>& refs, const Split& split,
        std::vector<Ref>& left_refs, std::vector<Ref>& right_refs) const
    {
        const size_t axis = split.axis;
        left_refs.clear();
        right_refs.clear();
        left_refs.reserve(split.left_count);
        right_refs.reserve(split.right_count);
        for (const auto& ref : refs) {
            const int side = classify(ref, axis, split.plane);
            if (side < 0) {
                Ref child = ref;
                child.cell.max[axis] = robust_min(ref.cell.max[axis], split.plane);
                left_refs.push_back(std::move(child));
            } else if (side > 0) {
                Ref child = ref;
                child.cell.min[axis] = robust_max(ref.cell.min[axis], split.plane);
                right_refs.push_back(std::move(child));
            } else {
                left_refs.push_back(clip_ref(ref, axis, split.plane, true));
                right_refs.push_back(clip_ref(ref, axis, split.plane, false));
            }
        }
    }

    Result build() {
        Result result;
        const size_t prim_count = tris_.size();
        if (prim_count == 0)
            return result;

        if (prim_count > static_cast<size_t>(Index::max_first_id))
            throw std::length_error("SpatialSahBuilder: too many input references");

        Scalar max_abs = 0;
        for (const auto& tri : tris_)
            for (const auto* p : { &tri.p0, &tri.p1, &tri.p2 })
                for (size_t i = 0; i < 3; ++i)
                    max_abs = robust_max(max_abs, std::abs((*p)[i]));
        slack_ = Scalar(32) * std::numeric_limits<Scalar>::epsilon() *
                 robust_max(Scalar(1), max_abs);

        std::vector<Ref> root_refs;
        root_refs.reserve(prim_count);
        for (size_t i = 0; i < prim_count; ++i) {
            const auto& tri = tris_[i];
            Ref ref;
            ref.prim_id = i;
            ref.poly[0] = tri.p0;
            ref.poly[1] = tri.p1;
            ref.poly[2] = tri.p2;
            ref.poly_count = 3;
            ref.cell = tri.get_bbox();
            ref.tight = make_tight_bbox(ref.cell, ref.cell);
            ref.center = ref.tight.get_center();
            root_refs.push_back(std::move(ref));
        }

        auto& bvh = result.bvh;
        bvh.nodes.emplace_back();
        bvh.nodes[0].set_bbox(compute_bbox(root_refs));

        size_t total_refs = prim_count;
        size_t budget_left = config_.max_extra_references;

        std::stack<WorkItem> stack;
        stack.push(WorkItem { 0, std::move(root_refs), bvh.nodes[0].get_bbox() });

        while (!stack.empty()) {
            WorkItem item = std::move(stack.top());
            stack.pop();
            auto& node = bvh.nodes[item.node_id];
            const size_t n = item.refs.size();

            const auto make_leaf = [&] {
                if (n > static_cast<size_t>(Index::max_prim_count))
                    throw std::length_error("SpatialSahBuilder: leaf does not fit in node index");
                const size_t first = bvh.prim_ids.size();
                if (first > static_cast<size_t>(Index::max_first_id))
                    throw std::length_error("SpatialSahBuilder: primitive index overflow");
                node.index = Index::make_leaf(first, n);
                for (const auto& ref : item.refs) {
                    bvh.prim_ids.push_back(ref.prim_id);
                    result.reference_bounds.push_back(ref.tight);
                    result.reference_cells.push_back(ref.cell);
                }
            };

            if (n <= config_.min_leaf_size) {
                make_leaf();
                continue;
            }

            // Object candidates first so that, on equal cost, the non-replicating split wins.
            Split best;
            find_object_candidates(item.refs, item.bbox, best);
            find_spatial_candidates(item.refs, item.bbox, budget_left, best);

            const Scalar non_split_cost =
                config_.sah.get_non_split_cost(0, n, item.bbox);

            bool use_split = best.kind != SplitKind::None && best.cost < non_split_cost;
            if (!use_split && n > config_.max_leaf_size) {
                best = fallback_object_split(item.refs, item.bbox);
                use_split = true;
            }
            if (!use_split) {
                make_leaf();
                continue;
            }

            std::vector<Ref> left_refs, right_refs;
            if (best.kind == SplitKind::Spatial) {
                if (best.straddle_count > budget_left)
                    throw std::length_error("SpatialSahBuilder: spatial split exceeds budget");
                const size_t new_total = total_refs + best.straddle_count;
                if (new_total > static_cast<size_t>(Index::max_first_id))
                    throw std::length_error("SpatialSahBuilder: reference count overflow");
                commit_spatial_split(item.refs, best, left_refs, right_refs);
                budget_left -= best.straddle_count;
                total_refs = new_total;
                ++result.spatial_split_count;
            } else {
                commit_object_split(item.refs, best, left_refs, right_refs);
            }

            if (left_refs.empty() || right_refs.empty())
                throw std::length_error("SpatialSahBuilder: split failed to make progress");

            BBox left_bbox = compute_bbox(left_refs);
            BBox right_bbox = compute_bbox(right_refs);

            // For any-hit queries, place the child with the largest area first.
            if (left_bbox.get_half_area() < right_bbox.get_half_area()) {
                std::swap(left_bbox, right_bbox);
                std::swap(left_refs, right_refs);
            }

            const size_t first_child = bvh.nodes.size();
            if (first_child + 1 > static_cast<size_t>(Index::max_first_id))
                throw std::length_error("SpatialSahBuilder: node index overflow");
            node.index = Index::make_inner(first_child);
            bvh.nodes.resize(first_child + 2);
            bvh.nodes[first_child + 0].set_bbox(left_bbox);
            bvh.nodes[first_child + 1].set_bbox(right_bbox);

            // Push the largest child last so it is processed first, keeping the stack shallow.
            WorkItem left_item { first_child + 0, std::move(left_refs), left_bbox };
            WorkItem right_item { first_child + 1, std::move(right_refs), right_bbox };
            if (left_item.refs.size() < right_item.refs.size())
                std::swap(left_item, right_item);
            stack.push(std::move(right_item));
            stack.push(std::move(left_item));
        }

        bvh.nodes.shrink_to_fit();
        return result;
    }
};

} // namespace bvh::v2

#endif
