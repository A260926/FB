#ifndef BVH_V2_SPATIAL_SAH_BUILDER_H
#define BVH_V2_SPATIAL_SAH_BUILDER_H

#include "bvh/v2/bvh.h"
#include "bvh/v2/tri.h"
#include "bvh/v2/bbox.h"
#include "bvh/v2/vec.h"
#include "bvh/v2/top_down_sah_builder.h"

#include <array>
#include <vector>
#include <span>
#include <stack>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <type_traits>
#include <cstddef>

namespace bvh::v2 {

/// Top-down SAH builder for triangles that is allowed to split in *space*, meaning that a single
/// input triangle can be referenced by several tighter spatial references. The original triangles
/// are never modified: every reference still points to the caller's triangle index, and carries
/// the convex polygon obtained by intersecting the original triangle with the half-spaces of all
/// its ancestors. Traversal is unchanged and goes through the regular `Bvh<Node>` routines.
///
/// At every node, object splits (binned on reference centers), spatial splits (equally spaced
/// internal planes, clipping the current fragment polygons) and the option of not splitting are
/// all considered. A single, tree-wide budget limits how many extra references may be created.
///
/// Each node owns its own list of references. Because a spatial split can grow that list, the
/// references are passed by value between work items rather than addressed through absolute
/// indices into one shared array, which would otherwise be invalidated when a sibling grows.
template <typename Node, size_t BinCount = 16>
class SpatialSahBuilder {
    static_assert(Node::dimension == 3, "SpatialSahBuilder only supports 3D nodes");
    static_assert(
        std::is_same_v<typename Node::Scalar, float> ||
        std::is_same_v<typename Node::Scalar, double>,
        "SpatialSahBuilder only supports float or double nodes");

    using Scalar = typename Node::Scalar;
    using Vec    = bvh::v2::Vec<Scalar, 3>;
    using BBox   = bvh::v2::BBox<Scalar, 3>;
    using Tri    = bvh::v2::Tri<Scalar, 3>;
    using Index  = typename Node::Index;

public:
    struct Config : TopDownSahBuilder<Node>::Config {
        /// Tree-wide budget for additional references relative to the number of input triangles.
        /// A value of zero disables spatial splits entirely (only object splits are used).
        size_t max_extra_references = 0;
    };

    struct BuildResult {
        Bvh<Node> bvh;
        /// Tight, conservatively padded bounds of each fragment, in `bvh.prim_ids` slot order.
        std::vector<BBox> reference_bounds;
        /// Closed spatial region covered by each fragment (intersection of all ancestor
        /// half-spaces), in `bvh.prim_ids` slot order.
        std::vector<BBox> reference_cells;
        /// Number of spatial splits that were actually committed during the build.
        size_t spatial_split_count = 0;
    };

    [[nodiscard]] static BuildResult build(std::span<const Tri> tris, const Config& config = {}) {
        return SpatialSahBuilder(tris, config).run();
    }

private:
    /// A spatial reference: an original triangle index together with the polygon that represents
    /// the intersection of that triangle with all the clipping half-spaces applied so far.
    struct Reference {
        size_t prim_id;
        std::vector<Vec> polygon;
        BBox bounds; ///< Tight bounds of the polygon, with the conservative padding applied.
        BBox cell;   ///< Closed cumulative clipping region.
    };

    struct WorkItem {
        size_t node_id;
        std::vector<Reference> refs;
    };

    enum class SplitKind { None, Object, Spatial };

    struct Split {
        SplitKind kind = SplitKind::None;
        size_t axis = 0;
        Scalar plane = 0;
        Scalar cost = 0;
        size_t extra = 0; ///< Number of duplicated references for a spatial split.
    };

    const Config& config_;
    std::span<const Tri> tris_;
    size_t budget_remaining_;
    size_t spatial_split_count_ = 0;
    Scalar pad_delta_;

    SpatialSahBuilder(std::span<const Tri> tris, const Config& config)
        : config_(config)
        , tris_(tris)
        , budget_remaining_(config.max_extra_references)
    {
        if (config.min_leaf_size == 0 || config.min_leaf_size > config.max_leaf_size)
            throw std::length_error("invalid leaf size parameters");
        if (config.max_leaf_size > static_cast<size_t>(Index::max_prim_count))
            throw std::length_error("max_leaf_size cannot be represented by the node index");

        Scalar max_abs = 0;
        for (const auto& tri : tris)
            for (const auto* p : { &tri.p0, &tri.p1, &tri.p2 })
                for (size_t i = 0; i < 3; ++i)
                    max_abs = std::max(max_abs, std::abs((*p)[i]));
        pad_delta_ = static_cast<Scalar>(32) * std::numeric_limits<Scalar>::epsilon() *
            std::max(static_cast<Scalar>(1), max_abs);
    }

    BVH_ALWAYS_INLINE BBox pad_bbox(const BBox& bbox) const {
        return BBox(bbox.min - Vec(pad_delta_), bbox.max + Vec(pad_delta_));
    }

    static BBox tight_bbox(const std::vector<Vec>& polygon) {
        auto bbox = BBox::make_empty();
        for (const auto& p : polygon)
            bbox.extend(p);
        return bbox;
    }

    static bool any_different(const Vec& a, const Vec& b) {
        for (size_t i = 0; i < 3; ++i)
            if (a[i] != b[i])
                return true;
        return false;
    }

    /// Clips a convex polygon against the axis-aligned plane `x[axis] = plane`. When `keep_left`
    /// is true the closed negative half-space is kept, otherwise the closed positive half-space
    /// is kept. Vertices lying on the plane are kept by either side so that adjacent fragments
    /// share their boundary.
    std::vector<Vec> clip_polygon(
        const std::vector<Vec>& polygon,
        size_t axis,
        Scalar plane,
        bool keep_left) const
    {
        std::vector<Vec> out;
        out.reserve(polygon.size() + 1);
        const auto inside = [&] (Scalar d) { return keep_left ? d <= 0 : d >= 0; };

        for (size_t i = 0; i < polygon.size(); ++i) {
            const auto& a = polygon[i];
            const auto& b = polygon[(i + 1) % polygon.size()];
            const Scalar da = a[axis] - plane;
            const Scalar db = b[axis] - plane;
            const bool a_in = inside(da);
            const bool b_in = inside(db);

            if (a_in != b_in) {
                // The edge crosses the plane: insert the intersection point. The classification
                // guarantees the endpoints are on opposite sides, so the denominator is non-zero.
                const Scalar t = da / (da - db);
                Vec q = Vec::generate([&] (size_t k) {
                    return k == axis ? plane : a[k] + t * (b[k] - a[k]);
                });
                out.push_back(q);
            }
            if (b_in) {
                // Drop consecutive duplicates, which are common when a vertex is on the plane.
                if (out.empty() || any_different(out.back(), b))
                    out.push_back(b);
            }
        }
        return out;
    }

    struct Classification {
        bool has_left;   ///< At least one vertex strictly on the negative side.
        bool has_right;  ///< At least one vertex strictly on the positive side.
    };

    static Classification classify(const std::vector<Vec>& polygon, size_t axis, Scalar plane) {
        Classification c { false, false };
        for (const auto& p : polygon) {
            const Scalar d = p[axis] - plane;
            if (d < 0) c.has_left = true;
            else if (d > 0) c.has_right = true;
            // A vertex exactly on the plane belongs to both closed half-spaces; the clipping
            // routines keep it on either side, so nothing special has to be done here.
        }
        return c;
    }

    /// Clamps a cumulative clipping cell to one closed side of the plane.
    static BBox clamp_cell(const BBox& cell, size_t axis, Scalar plane, bool keep_left) {
        auto out = cell;
        if (keep_left)
            out.max[axis] = std::min(out.max[axis], plane);
        else
            out.min[axis] = std::max(out.min[axis], plane);
        return out;
    }

    static BBox union_bounds(const std::vector<Reference>& refs) {
        auto bbox = BBox::make_empty();
        for (const auto& ref : refs)
            bbox.extend(ref.bounds);
        return bbox;
    }

    /// Object split candidate: references are binned based on the center of their current bounds.
    Split find_object_split(const BBox& node_bbox, const std::vector<Reference>& refs,
                            Scalar leaf_cost) const {
        struct Bin {
            BBox bbox = BBox::make_empty();
            size_t count = 0;
        };
        Split best;
        best.kind = SplitKind::None;
        best.cost = leaf_cost;

        for (size_t axis = 0; axis < 3; ++axis) {
            const Scalar diag = node_bbox.get_diagonal()[axis];
            if (!(diag > 0))
                continue; // A flat node on this axis cannot be partitioned along it.
            const Scalar scale = static_cast<Scalar>(BinCount) / diag;
            const Scalar offset = -node_bbox.min[axis] * scale;

            std::array<Bin, BinCount> bins {};
            for (const auto& ref : refs) {
                Scalar pos = fast_mul_add(ref.bounds.get_center()[axis], scale, offset);
                size_t id = static_cast<size_t>(robust_max(pos, Scalar(0)));
                id = std::min(BinCount - 1, id);
                bins[id].bbox.extend(ref.bounds);
                bins[id].count += 1;
            }

            std::array<Scalar, BinCount> right_costs {};
            std::array<size_t, BinCount> right_counts {};
            Bin right_accum;
            for (size_t i = BinCount - 1; i > 0; --i) {
                right_accum.bbox.extend(bins[i].bbox);
                right_accum.count += bins[i].count;
                right_costs[i] = config_.sah.get_leaf_cost(0, right_accum.count, right_accum.bbox);
                right_counts[i] = right_accum.count;
            }

            Bin left_accum;
            for (size_t i = 0; i < BinCount - 1; ++i) {
                left_accum.bbox.extend(bins[i].bbox);
                left_accum.count += bins[i].count;
                if (left_accum.count == 0 || right_counts[i + 1] == 0)
                    continue;
                const Scalar cost =
                    config_.sah.get_leaf_cost(0, left_accum.count, left_accum.bbox) +
                    right_costs[i + 1];
                // Object splits are registered first; spatial candidates must beat them strictly,
                // which implements the tie-breaking rule in favor of non-duplicating splits.
                if (cost < best.cost) {
                    best.kind = SplitKind::Object;
                    best.axis = axis;
                    best.plane = node_bbox.min[axis] +
                        diag * (static_cast<Scalar>(i + 1) / static_cast<Scalar>(BinCount));
                    best.cost = cost;
                    best.extra = 0;
                }
            }
        }
        return best;
    }

    /// Evaluates all spatial planes (BinCount equally spaced internal planes on each axis) without
    /// consuming any budget. Trial clipping is used to compute the bounds and reference counts on
    /// both sides, including duplicated references.
    Split find_spatial_split(const BBox& node_bbox, const std::vector<Reference>& refs,
                             Scalar leaf_cost) const {
        Split best;
        best.kind = SplitKind::None;
        best.cost = leaf_cost;

        for (size_t axis = 0; axis < 3; ++axis) {
            const Scalar diag = node_bbox.get_diagonal()[axis];
            if (!(diag > 0))
                continue;
            for (size_t plane_id = 1; plane_id < BinCount; ++plane_id) {
                const Scalar plane = node_bbox.min[axis] +
                    diag * (static_cast<Scalar>(plane_id) / static_cast<Scalar>(BinCount));

                BBox left_bbox = BBox::make_empty();
                BBox right_bbox = BBox::make_empty();
                size_t left_count = 0, right_count = 0;
                bool strict_left = false, strict_right = false;

                for (const auto& ref : refs) {
                    const auto c = classify(ref.polygon, axis, plane);
                    strict_left |= c.has_left;
                    strict_right |= c.has_right;

                    if (c.has_left && c.has_right) {
                        // The fragment straddles the plane: clip the current polygon on both
                        // sides, and bill both resulting references.
                        auto left_poly = clip_polygon(ref.polygon, axis, plane, true);
                        auto right_poly = clip_polygon(ref.polygon, axis, plane, false);
                        left_bbox.extend(pad_bbox(tight_bbox(left_poly)));
                        right_bbox.extend(pad_bbox(tight_bbox(right_poly)));
                        ++left_count;
                        ++right_count;
                    } else if (c.has_right) {
                        // Entirely on the positive side (vertices on the plane stay covered by
                        // the padded bounds regardless).
                        right_bbox.extend(ref.bounds);
                        ++right_count;
                    } else if (c.has_left) {
                        left_bbox.extend(ref.bounds);
                        ++left_count;
                    } else {
                        // Zero-thickness fragment lying exactly on the plane: keep it on both
                        // sides so boundary rays cannot slip between the children.
                        left_bbox.extend(ref.bounds);
                        right_bbox.extend(ref.bounds);
                        ++left_count;
                        ++right_count;
                    }
                }

                if (!strict_left || !strict_right)
                    continue; // The plane must genuinely cut through the node to make progress.
                if (left_count == 0 || right_count == 0)
                    continue;

                const size_t extra = (left_count + right_count) - refs.size();
                if (extra > budget_remaining_)
                    continue; // Trials never consume the shared budget.

                const Scalar cost =
                    config_.sah.get_leaf_cost(0, left_count, left_bbox) +
                    config_.sah.get_leaf_cost(0, right_count, right_bbox);
                if (cost < best.cost) {
                    best.kind = SplitKind::Spatial;
                    best.axis = axis;
                    best.plane = plane;
                    best.cost = cost;
                    best.extra = extra;
                }
            }
        }
        return best;
    }

    /// Fallback object partition that always produces two non-empty groups, used when the node is
    /// over `max_leaf_size` but no SAH-improving split exists.
    static Split fallback_split(const BBox& node_bbox, std::vector<Reference>& refs) {
        Split split;
        split.kind = SplitKind::Object;
        split.axis = node_bbox.get_diagonal().get_largest_axis();
        const size_t mid = refs.size() / 2;
        std::nth_element(
            refs.begin(), refs.begin() + mid, refs.end(),
            [&] (const Reference& a, const Reference& b) {
                return a.bounds.get_center()[split.axis] <
                       b.bounds.get_center()[split.axis];
            });
        split.plane = refs[mid].bounds.get_center()[split.axis];
        split.cost = 0;
        split.extra = 0;
        return split;
    }

    std::vector<Reference> apply_object_split(const Split& split, std::vector<Reference> refs,
                                              std::vector<Reference>& right) const {
        const auto axis = split.axis;
        auto it = std::partition(refs.begin(), refs.end(),
            [&] (const Reference& ref) {
                return ref.bounds.get_center()[axis] < split.plane;
            });
        if (it == refs.begin() || it == refs.end()) {
            // Degenerate centers: fall back to a stable median to guarantee progress.
            const size_t mid = refs.size() / 2;
            std::nth_element(
                refs.begin(), refs.begin() + mid, refs.end(),
                [&] (const Reference& a, const Reference& b) {
                    return a.prim_id < b.prim_id ||
                           (a.prim_id == b.prim_id && a.cell.min[axis] < b.cell.min[axis]);
                });
            it = refs.begin() + mid;
        }
        right.reserve(refs.end() - it);
        right.insert(right.end(), std::make_move_iterator(it), std::make_move_iterator(refs.end()));
        refs.erase(it, refs.end());
        return refs;
    }

    /// Commits a spatial split: clips the current fragment polygons, deducts the duplicated
    /// references from the shared budget and returns the two child reference lists.
    std::vector<Reference> apply_spatial_split(
        const Split& split, const std::vector<Reference>& refs,
        std::vector<Reference>& right)
    {
        const size_t axis = split.axis;
        const Scalar plane = split.plane;

        std::vector<Reference> left;
        left.reserve(refs.size() + split.extra + 1);
        right.reserve(refs.size() / 2 + 1);

        for (const auto& ref : refs) {
            const auto c = classify(ref.polygon, axis, plane);

            if (c.has_left && c.has_right) {
                Reference l = ref;
                Reference r = ref;
                l.polygon = clip_polygon(ref.polygon, axis, plane, true);
                r.polygon = clip_polygon(ref.polygon, axis, plane, false);
                l.bounds = pad_bbox(tight_bbox(l.polygon));
                r.bounds = pad_bbox(tight_bbox(r.polygon));
                l.cell = clamp_cell(ref.cell, axis, plane, true);
                r.cell = clamp_cell(ref.cell, axis, plane, false);
                left.push_back(std::move(l));
                right.push_back(std::move(r));
            } else if (c.has_right) {
                Reference r = ref;
                r.cell = clamp_cell(ref.cell, axis, plane, false);
                right.push_back(std::move(r));
            } else if (c.has_left) {
                Reference l = ref;
                l.cell = clamp_cell(ref.cell, axis, plane, true);
                left.push_back(std::move(l));
            } else {
                // Zero-thickness fragment on the plane: keep it on both closed sides.
                Reference l = ref;
                Reference r = ref;
                l.cell = clamp_cell(ref.cell, axis, plane, true);
                r.cell = clamp_cell(ref.cell, axis, plane, false);
                left.push_back(std::move(l));
                right.push_back(std::move(r));
            }
        }

        const size_t new_extra = (left.size() + right.size()) - refs.size();
        if (new_extra > budget_remaining_)
            throw std::length_error("spatial split exceeds the reference budget");
        budget_remaining_ -= new_extra;
        ++spatial_split_count_;
        return left;
    }

    void check_node_capacity(size_t node_count) const {
        if (node_count > static_cast<size_t>(Index::max_first_id))
            throw std::length_error("too many BVH nodes for the node index");
    }

    BuildResult run() {
        BuildResult result;
        if (tris_.empty())
            return result;

        const size_t max_total_refs = tris_.size() + config_.max_extra_references;
        if (max_total_refs > static_cast<size_t>(Index::max_first_id) ||
            max_total_refs < tris_.size())
            throw std::length_error("total reference count cannot be represented by the node index");

        std::vector<Reference> root_refs;
        root_refs.reserve(max_total_refs);
        for (size_t i = 0; i < tris_.size(); ++i) {
            Reference ref;
            ref.prim_id = i;
            ref.polygon = { tris_[i].p0, tris_[i].p1, tris_[i].p2 };
            ref.cell = tris_[i].get_bbox();
            ref.bounds = pad_bbox(tight_bbox(ref.polygon));
            root_refs.push_back(std::move(ref));
        }

        Bvh<Node> bvh;
        bvh.nodes.reserve(std::max(size_t(1), 2 * tris_.size()));
        bvh.nodes.emplace_back();
        bvh.nodes.back().set_bbox(union_bounds(root_refs));

        std::stack<WorkItem> stack;
        stack.push(WorkItem { 0, std::move(root_refs) });

        while (!stack.empty()) {
            auto item = std::move(stack.top());
            stack.pop();
            auto& node = bvh.nodes[item.node_id];
            auto& refs = item.refs;
            const auto node_bbox = node.get_bbox();

            if (refs.size() <= config_.min_leaf_size) {
                make_leaf(bvh, result, node, refs);
                continue;
            }

            const Scalar leaf_cost = config_.sah.get_non_split_cost(0, refs.size(), node_bbox);

            // Object candidates first so that equal-cost spatial candidates cannot replace them.
            Split object_split = find_object_split(node_bbox, refs, leaf_cost);
            Split spatial_split = find_spatial_split(node_bbox, refs, leaf_cost);

            Split chosen;
            if (spatial_split.kind == SplitKind::Spatial &&
                (object_split.kind != SplitKind::Object ||
                 spatial_split.cost < object_split.cost))
                chosen = spatial_split;
            else if (object_split.kind == SplitKind::Object)
                chosen = object_split;

            std::vector<Reference> left, right;
            if (chosen.kind == SplitKind::Spatial) {
                left = apply_spatial_split(chosen, refs, right);
            } else if (chosen.kind == SplitKind::Object) {
                left = apply_object_split(chosen, std::move(refs), right);
            } else if (refs.size() <= config_.max_leaf_size) {
                make_leaf(bvh, result, node, refs);
                continue;
            } else {
                auto fallback = fallback_split(node_bbox, refs);
                left = apply_object_split(fallback, std::move(refs), right);
            }

            if (left.empty() || right.empty())
                throw std::length_error("spatial builder split failed to make progress");

            check_node_capacity(bvh.nodes.size() + 2);
            const size_t first_child = bvh.nodes.size();
            node.index = Index::make_inner(first_child);
            bvh.nodes.resize(first_child + 2);

            // Pair each child's node id with its bounds and references so the three stay together.
            WorkItem left_item { first_child + 0, std::move(left) };
            WorkItem right_item { first_child + 1, std::move(right) };
            auto left_bbox = union_bounds(left_item.refs);
            auto right_bbox = union_bounds(right_item.refs);

            // For "any-hit" queries, the child with the largest area is visited first.
            if (left_bbox.get_half_area() < right_bbox.get_half_area()) {
                std::swap(left_bbox, right_bbox);
                std::swap(left_item, right_item);
            }
            bvh.nodes[left_item.node_id].set_bbox(left_bbox);
            bvh.nodes[right_item.node_id].set_bbox(right_bbox);

            // Push the smaller child first so the larger one is processed first (smaller stack).
            if (left_item.refs.size() < right_item.refs.size())
                std::swap(left_item, right_item);
            stack.push(std::move(left_item));
            stack.push(std::move(right_item));
        }

        result.spatial_split_count = spatial_split_count_;
        bvh.nodes.shrink_to_fit();
        result.bvh = std::move(bvh);
        return result;
    }

    void make_leaf(Bvh<Node>& bvh, BuildResult& result, Node& node,
                   const std::vector<Reference>& refs)
    {
        const size_t first = result.reference_bounds.size();
        if (first > static_cast<size_t>(Index::max_first_id))
            throw std::length_error("too many spatial references for the node index");
        node.index = Index::make_leaf(first, refs.size());
        for (const auto& ref : refs) {
            bvh.prim_ids.push_back(ref.prim_id);
            result.reference_bounds.push_back(ref.bounds);
            result.reference_cells.push_back(ref.cell);
        }
    }
};

} // namespace bvh::v2

#endif
