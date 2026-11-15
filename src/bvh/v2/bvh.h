#ifndef BVH_V2_BVH_H
#define BVH_V2_BVH_H

#include "bvh/v2/node.h"

#include <cstddef>
#include <iterator>
#include <vector>
#include <stack>
#include <utility>
#include <tuple>
#include <algorithm>
#include <limits>

namespace bvh::v2 {

template <typename Node>
struct Bvh {
    using Index = typename Node::Index;
    using Scalar = typename Node::Scalar;
    using Ray = bvh::v2::Ray<Scalar, Node::dimension>;

    std::vector<Node> nodes;
    std::vector<size_t> prim_ids;

    /// Parent table of this BVH. It contains exactly one entry per node, in the same order as
    /// `nodes`, and stores the index of the parent of each node. The parent of the root node is
    /// 0, and an empty BVH has an empty parent table. The table is maintained by the builders and
    /// by the serialization/optimization/subtree-extraction routines; it only has to be rebuilt
    /// manually when `nodes` is modified directly (see `rebuild_parent_ids()`). Queries only read
    /// this table and never allocate, so any number of them may run concurrently on a BVH that is
    /// not being modified.
    std::vector<size_t> parent_ids;

    Bvh() = default;
    Bvh(Bvh&&) = default;

    Bvh& operator = (Bvh&&) = default;

    bool operator == (const Bvh& other) const = default;
    bool operator != (const Bvh& other) const = default;

    /// Returns whether the node located at the given index is the left child of its parent.
    static BVH_ALWAYS_INLINE bool is_left_sibling(size_t node_id) { return node_id % 2 == 1; }

    /// Returns the index of a sibling of a node.
    static BVH_ALWAYS_INLINE size_t get_sibling_id(size_t node_id) {
        return is_left_sibling(node_id) ? node_id + 1 : node_id - 1;
    }

    /// Returns the index of the left sibling of the node. This effectively returns the given index
    /// unchanged if the node is the left sibling, or the other sibling index otherwise.
    static BVH_ALWAYS_INLINE size_t get_left_sibling_id(size_t node_id) {
        return is_left_sibling(node_id) ? node_id : node_id - 1;
    }

    /// Returns the index of the right sibling of the node. This effectively returns the given index
    /// unchanged if the node is the right sibling, or the other sibling index otherwise.
    static BVH_ALWAYS_INLINE size_t get_right_sibling_id(size_t node_id) {
        return is_left_sibling(node_id) ? node_id + 1 : node_id;
    }

    /// Returns the root node of this BVH.
    BVH_ALWAYS_INLINE const Node& get_root() const { return nodes[0]; }

    /// Extracts the BVH rooted at the given node index.
    [[nodiscard]] inline Bvh extract_bvh(size_t root_id) const;

    /// Rebuilds `parent_ids` from the current contents of `nodes`. This has to be called after
    /// directly modifying the public `nodes` array. The parent table contains one entry per node,
    /// the parent of the root is 0, and an empty BVH produces an empty parent table.
    inline void rebuild_parent_ids();

    /// Stackless ray traversal. This behaves like `intersect()`, but never allocates memory, is
    /// not recursive, and does not use any stack (fixed size or dynamically grown). Instead, it
    /// relies on the read-only `parent_ids` table to walk back up the tree. Traversal starts at
    /// `root_node_id`, which does not have to be the actual root of the BVH; in that case only the
    /// subtree rooted at that node is visited. At every inner node, for closest-hit queries, the
    /// child with the smallest ray entry distance is visited first (the left child wins ties),
    /// and the far child is re-tested against the current `ray.tmax` before being entered, so
    /// that a tighter `tmax` set by a leaf callback can cull it. When `IsAnyHit` is true, the
    /// traversal ends as soon as the leaf callback returns true. The leaf callback receives the
    /// half-open range `[begin, end)` of entries in `prim_ids` that make up the leaf.
    template <bool IsAnyHit, bool IsRobust, typename LeafFn>
    inline void intersect_stackless(const Ray& ray, size_t root_node_id, LeafFn&& leaf_fn) const;

    /// Traverses the BVH from the given index in `start` using the provided stack. Every leaf
    /// encountered on the way is processed using the given `LeafFn` function, and every pair of
    /// nodes is processed with the function in `InnerFn`, which returns a triplet of booleans
    /// indicating whether the first child should be processed, whether the second child should be
    /// processed, and whether to traverse the second child first instead of the other way around.
    template <bool IsAnyHit, typename Stack, typename LeafFn, typename InnerFn>
    inline void traverse_top_down(Index start, Stack&, LeafFn&&, InnerFn&&) const;

    /// Intersects the BVH with a single ray, using the given function to intersect the contents
    /// of a leaf. The algorithm starts at the node index `start` and uses the given stack object.
    /// When `IsAnyHit` is true, the function stops at the first intersection (useful for shadow
    /// rays), otherwise it finds the closest intersection. When `IsRobust` is true, a slower but
    /// numerically robust ray-box test is used, otherwise a fast, but less precise test is used.
    template <bool IsAnyHit, bool IsRobust, typename Stack, typename LeafFn, typename InnerFn = IgnoreArgs>
    inline void intersect(const Ray& ray, Index start, Stack&, LeafFn&&, InnerFn&& = {}) const;

    /// Traverses this BVH from the bottom to the top, using the given function objects to process
    /// leaves and inner nodes.
    template <typename LeafFn = IgnoreArgs, typename InnerFn = IgnoreArgs>
    inline void traverse_bottom_up(LeafFn&& = {}, InnerFn&& = {});

    /// Refits the BVH, using the given function object to recompute the bounding box of the leaves.
    template <typename LeafFn = IgnoreArgs>
    inline void refit(LeafFn&& = {});

    template <typename IndexType = typename Index::Type>
    inline void serialize(OutputStream&) const;

    template <typename IndexType = typename Index::Type>
    [[nodiscard]] static inline Bvh deserialize(InputStream&);
};

template <typename Node>
Bvh<Node> Bvh<Node>::extract_bvh(size_t root_id) const {
    assert(root_id != 0);

    Bvh bvh;
    bvh.nodes.emplace_back();

    std::stack<std::pair<size_t, size_t>> stack;
    stack.emplace(root_id, 0);
    while (!stack.empty()) {
        auto [src_id, dst_id] = stack.top();
        stack.pop();
        const auto& src_node = nodes[src_id];
        auto& dst_node = bvh.nodes[dst_id];
        dst_node = src_node;
        if (src_node.is_leaf()) {
            dst_node.index.set_first_id(bvh.prim_ids.size());
            std::copy_n(
                prim_ids.begin() + src_node.index.first_id(),
                src_node.index.prim_count(),
                std::back_inserter(bvh.prim_ids));
        } else {
            dst_node.index.set_first_id(bvh.nodes.size());
            stack.emplace(src_node.index.first_id() + 0, bvh.nodes.size() + 0);
            stack.emplace(src_node.index.first_id() + 1, bvh.nodes.size() + 1);
            // Note: This may invalidate `dst_node` so has to happen after any access to it.
            bvh.nodes.emplace_back();
            bvh.nodes.emplace_back();
        }
    }
    bvh.rebuild_parent_ids();
    return bvh;
}

template <typename Node>
void Bvh<Node>::rebuild_parent_ids() {
    parent_ids.assign(nodes.size(), 0);
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].is_leaf())
            continue;
        parent_ids[nodes[i].index.first_id() + 0] = i;
        parent_ids[nodes[i].index.first_id() + 1] = i;
    }
}

template <typename Node>
template <bool IsAnyHit, typename Stack, typename LeafFn, typename InnerFn>
void Bvh<Node>::traverse_top_down(Index start, Stack& stack, LeafFn&& leaf_fn, InnerFn&& inner_fn) const
{
    stack.push(start);
restart:
    while (!stack.is_empty()) {
        auto top = stack.pop();
        while (top.prim_count() == 0) {
            auto& left  = nodes[top.first_id()];
            auto& right = nodes[top.first_id() + 1];
            auto [hit_left, hit_right, should_swap] = inner_fn(left, right);

            if (hit_left) {
                auto near_index = left.index;
                if (hit_right) {
                    auto far_index = right.index;
                    if (should_swap)
                        std::swap(near_index, far_index);
                    stack.push(far_index);
                }
                top = near_index;
            } else if (hit_right)
                top = right.index;
            else [[unlikely]]
                goto restart;
        }

        [[maybe_unused]] auto was_hit = leaf_fn(top.first_id(), top.first_id() + top.prim_count());
        if constexpr (IsAnyHit) {
            if (was_hit) return;
        }
    }
}

template <typename Node>
template <bool IsAnyHit, bool IsRobust, typename Stack, typename LeafFn, typename InnerFn>
void Bvh<Node>::intersect(const Ray& ray, Index start, Stack& stack, LeafFn&& leaf_fn, InnerFn&& inner_fn) const {
    auto inv_dir = ray.template get_inv_dir<!IsRobust>();
    auto inv_org = -inv_dir * ray.org;
    auto inv_dir_pad = ray.pad_inv_dir(inv_dir);
    auto octant = ray.get_octant();

    traverse_top_down<IsAnyHit>(start, stack, leaf_fn, [&] (const Node& left, const Node& right) {
        inner_fn(left, right);
        std::pair<Scalar, Scalar> intr_left, intr_right;
        if constexpr (IsRobust) {
            intr_left  = left .intersect_robust(ray, inv_dir, inv_dir_pad, octant);
            intr_right = right.intersect_robust(ray, inv_dir, inv_dir_pad, octant);
        } else {
            intr_left  = left .intersect_fast(ray, inv_dir, inv_org, octant);
            intr_right = right.intersect_fast(ray, inv_dir, inv_org, octant);
        }
        return std::make_tuple(
            intr_left.first <= intr_left.second,
            intr_right.first <= intr_right.second,
            !IsAnyHit && intr_left.first > intr_right.first);
    });
}

template <typename Node>
template <bool IsAnyHit, bool IsRobust, typename LeafFn>
void Bvh<Node>::intersect_stackless(const Ray& ray, size_t root_node_id, LeafFn&& leaf_fn) const {
    if (nodes.empty() || root_node_id >= nodes.size())
        return;

    // Everything in this function is scalar state: there is no recursion, no stack and no
    // allocation, so the per-ray footprint is independent of the tree depth and of its number of
    // nodes. Only the read-only `parent_ids` table is used to walk back up, and never to jump
    // back to the global root to relocate a node. Starting from a non-root node therefore only
    // visits that subtree.
    auto inv_dir = ray.template get_inv_dir<!IsRobust>();
    auto inv_org = -inv_dir * ray.org;
    auto inv_dir_pad = ray.pad_inv_dir(inv_dir);
    auto octant = ray.get_octant();

    auto intersect_node = [&] (const Node& node) {
        if constexpr (IsRobust)
            return node.intersect_robust(ray, inv_dir, inv_dir_pad, octant);
        else
            return node.intersect_fast(ray, inv_dir, inv_org, octant);
    };

    // Computes the near and far children of `node`: in closest-hit mode the child with the
    // smallest entry distance comes first (the left child wins ties), in any-hit mode the static
    // left-to-right order is kept. Children whose box is missed with the current tmax are
    // reported through the booleans.
    auto order_children = [&] (const Node& node, size_t& near_id, size_t& far_id,
                               bool& near_hit, bool& far_hit)
    {
        const auto first_child = static_cast<size_t>(node.index.first_id());
        const Node& left  = nodes[first_child + 0];
        const Node& right = nodes[first_child + 1];
        auto intr_left  = intersect_node(left);
        auto intr_right = intersect_node(right);
        bool hit_left  = intr_left.first  <= intr_left.second;
        bool hit_right = intr_right.first <= intr_right.second;

        bool swap_children = false;
        if constexpr (!IsAnyHit)
            swap_children = hit_left && hit_right && intr_left.first > intr_right.first;

        near_id = first_child + (swap_children ? 1 : 0);
        far_id  = first_child + (swap_children ? 0 : 1);
        near_hit = swap_children ? hit_right : hit_left;
        far_hit  = swap_children ? hit_left  : hit_right;
    };

    // `previous` is the node visited just before `current`. It distinguishes a descent (previous
    // is the parent) from a return (previous is a child), which is what makes the parent-link
    // walk a continuation rather than a brand new descent: a subtree that has already been fully
    // processed is never entered a second time.
    constexpr size_t invalid = std::numeric_limits<size_t>::max();
    size_t current = root_node_id;
    size_t previous = invalid;
    bool first_entry = true;

    while (true) {
        const Node& node = nodes[current];
        bool arriving_from_above = first_entry || previous == parent_ids[current];
        first_entry = false;

        if (node.is_leaf()) {
            if (leaf_fn(node.index.first_id(), node.index.first_id() + node.index.prim_count())) {
                if constexpr (IsAnyHit)
                    return;
            }
        } else if (arriving_from_above) {
            size_t near_id, far_id;
            bool near_hit, far_hit;
            order_children(node, near_id, far_id, near_hit, far_hit);
            if (near_hit) {
                previous = current;
                current = near_id;
                continue;
            }
            if (far_hit) {
                previous = current;
                current = far_id;
                continue;
            }
            // Neither child is intersected with the current tmax: climb out of this subtree.
        } else {
            // Returning from the fully processed child `previous`. Re-deriving the near/far
            // ordering here is only an identity check and not a fresh descent: the completed
            // child is never entered again, so near leaves are not replayed. Box entry distances
            // do not depend on `ray.tmax`, so this ordering matches the one used on the way down.
            size_t near_id, far_id;
            bool near_hit, far_hit;
            order_children(node, near_id, far_id, near_hit, far_hit);
            if (previous == near_id) {
                // The far child has not been visited yet. Re-test it with the latest tmax right
                // before entering, so a tighter tmax installed by a near leaf can cull it and no
                // far leaf is missed.
                auto intr = intersect_node(nodes[far_id]);
                if (intr.first <= intr.second) {
                    previous = current;
                    current = far_id;
                    continue;
                }
            }
            // Otherwise `previous` is the far child (both children are done), or the far child
            // was just culled: climb to the parent.
        }

        if (current == root_node_id)
            return;
        previous = current;
        current = parent_ids[current];
    }
}

template <typename Node>
template <typename LeafFn, typename InnerFn>
void Bvh<Node>::traverse_bottom_up(LeafFn&& leaf_fn, InnerFn&& inner_fn) {
    std::vector<size_t> parents(nodes.size(), 0);
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].is_leaf())
            continue;
        parents[nodes[i].index.first_id()] = i;
        parents[nodes[i].index.first_id() + 1] = i;
    }
    std::vector<bool> seen(nodes.size(), false);
    for (size_t i = nodes.size(); i-- > 0;) {
        if (!nodes[i].is_leaf())
            continue;
        leaf_fn(nodes[i]);
        seen[i] = true;
        for (size_t j = parents[i];; j = parents[j]) {
            auto& node = nodes[j];
            if (seen[j] || !seen[node.index.first_id()] || !seen[node.index.first_id() + 1])
                break;
            inner_fn(nodes[j]);
            seen[j] = true;
        }
    }
}

template <typename Node>
template <typename LeafFn>
void Bvh<Node>::refit(LeafFn&& leaf_fn) {
    traverse_bottom_up(leaf_fn, [&] (Node& node) {
        const auto& left  = nodes[node.index.first_id()];
        const auto& right = nodes[node.index.first_id() + 1];
        node.set_bbox(left.get_bbox().extend(right.get_bbox()));
    });
}

template <typename Node>
template <typename IndexType>
void Bvh<Node>::serialize(OutputStream& stream) const {
    stream.write(static_cast<IndexType>(nodes.size()));
    stream.write(static_cast<IndexType>(prim_ids.size()));
    for (auto&& node : nodes)
        node.serialize(stream);
    for (auto&& prim_id : prim_ids)
        stream.write(static_cast<IndexType>(prim_id));
}

template <typename Node>
template <typename IndexType>
Bvh<Node> Bvh<Node>::deserialize(InputStream& stream) {
    Bvh bvh;
    bvh.nodes.resize(stream.read<IndexType>());
    bvh.prim_ids.resize(stream.read<IndexType>());
    for (auto& node : bvh.nodes)
        node = Node::deserialize(stream);
    for (auto& prim_id : bvh.prim_ids)
        prim_id = stream.read<IndexType>();
    // The parent table is not part of the archive format; it is reconstructed on load instead,
    // keeping the serialized byte layout unchanged.
    bvh.rebuild_parent_ids();
    return bvh;
}

} // namespace bvh::v2

#endif
