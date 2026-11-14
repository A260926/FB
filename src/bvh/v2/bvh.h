#ifndef BVH_V2_BVH_H
#define BVH_V2_BVH_H

#include "bvh/v2/node.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>
#include <stack>
#include <utility>
#include <tuple>
#include <algorithm>

namespace bvh::v2 {

/// Work counters for the most recent edit performed on a BVH in editable mode.
struct EditStats {
    size_t node_visits = 0;
    size_t primitive_visits = 0;
};

template <typename Node>
struct Bvh {
    using Index = typename Node::Index;
    using Scalar = typename Node::Scalar;
    using Vec = bvh::v2::Vec<Scalar, Node::dimension>;
    using BBox = bvh::v2::BBox<Scalar, Node::dimension>;
    using Ray = bvh::v2::Ray<Scalar, Node::dimension>;

    std::vector<Node> nodes;
    std::vector<size_t> prim_ids;

    /// State attached to a BVH when editable mode has been enabled. The primitive boxes are stored
    /// by external primitive ID, which makes IDs sparse and stable across topology changes.
    struct EditState {
        std::map<size_t, BBox> boxes;
        std::unordered_map<size_t, size_t> id_to_leaf;
        std::vector<size_t> parents;

        /// Ranges of slot indices in `prim_ids` that are not referenced by any leaf. Keeping holes
        /// is what makes it possible to insert and erase primitives without shifting the ranges of
        /// unrelated leaves. The allocator reuses these ranges so that storage does not grow with
        /// the number of edits.
        std::map<size_t, size_t> free_runs;

        EditStats stats;
    };

    std::optional<EditState> edit_state;

    Bvh() = default;
    Bvh(Bvh&&) = default;

    Bvh& operator = (Bvh&&) = default;

    bool operator == (const Bvh& other) const {
        return nodes == other.nodes && prim_ids == other.prim_ids;
    }
    bool operator != (const Bvh& other) const { return !(*this == other); }

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

    /// Returns whether this BVH has no node (in particular after the last primitive was erased).
    BVH_ALWAYS_INLINE bool is_empty() const { return nodes.empty(); }

    /// Extracts the BVH rooted at the given node index.
    [[nodiscard]] inline Bvh extract_bvh(size_t root_id) const;

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

    /// Enables editable mode on this BVH. `boxes` contains the bounding box of every primitive,
    /// indexed by its external primitive ID (from 0 to `boxes.size() - 1`). The topology of the
    /// BVH is left untouched; only the auxiliary index used by the edit operations is built.
    /// Returns false if the number of boxes does not match the number of primitives referenced by
    /// the leaves, or if editable mode is already enabled.
    bool enable_edits(std::span<const BBox> boxes);

    /// Inserts the primitive with the given external ID, using the given bounding box. Returns
    /// false if editable mode is not enabled, if the ID is already present, or if the ID is out of
    /// the supported range.
    bool insert_primitive(size_t id, const BBox& box);

    /// Erases the primitive with the given external ID. Returns false if editable mode is not
    /// enabled or if the ID is not present.
    bool erase_primitive(size_t id);

    /// Returns the work counters for the most recent edit.
    EditStats last_edit_stats() const {
        return edit_state ? edit_state->stats : EditStats {};
    }

private:
    static constexpr size_t max_primitive_id = static_cast<size_t>(1) << 31;

    static constexpr uint32_t edit_magic = 0x45485642u; // "BVHE"
    static constexpr uint32_t edit_version = 1;
    static constexpr uint32_t edit_flag_enabled = 1u;

    BVH_ALWAYS_INLINE void count_node_visit(size_t count = 1) {
        if (edit_state)
            edit_state->stats.node_visits += count;
    }

    BVH_ALWAYS_INLINE void count_primitive_visit(size_t count = 1) {
        if (edit_state)
            edit_state->stats.primitive_visits += count;
    }

    /// Recomputes the bounding boxes of the ancestors of `node_id`, all the way to the root.
    void refit_ancestors(size_t node_id);

    /// Removes a leaf that no longer contains any primitive, merging its parent with its sibling
    /// and compacting the node array.
    void remove_empty_leaf(size_t leaf_id);

    /// Rebuilds the auxiliary edit index from the current topology and primitive list.
    void rebuild_edit_index(EditState& state) const;

    /// Allocates a contiguous range of at least `length` unreferenced slots in `prim_ids`.
    size_t allocate_run(EditState& state, size_t length);

    /// Releases a contiguous range of slots so that it can be reused by later edits.
    void release_run(EditState& state, size_t start, size_t length);

    /// Truncates free runs that end at the end of the slot storage, keeping memory bounded.
    static void trim_free_tail(EditState& state, std::vector<size_t>& prim_ids);

    static BVH_ALWAYS_INLINE BBox merge_bbox(const BBox& a, const BBox& b) {
        return BBox(a.min, a.max).extend(b);
    }
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
    return bvh;
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
            // An empty tree has no root node; the caller may still pass `Index{0}` as the start
            // index, in which case there is nothing to traverse.
            if (top.first_id() >= nodes.size())
                return;
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
    if (!edit_state) {
        // Keep the original byte layout for BVHs that do not use editable mode.
        stream.write(static_cast<IndexType>(nodes.size()));
        stream.write(static_cast<IndexType>(prim_ids.size()));
        for (auto&& node : nodes)
            node.serialize(stream);
        for (auto&& prim_id : prim_ids)
            stream.write(static_cast<IndexType>(prim_id));
        return;
    }

    // Editable BVHs use an extended format introduced by a sentinel that cannot be a node count,
    // followed by a format marker and flags, and finally the active primitive boxes. The original
    // format starts directly with the node count, so legacy files keep being readable.
    stream.write(static_cast<IndexType>(-1));
    stream.write(edit_magic);
    stream.write(edit_version);
    stream.write(static_cast<uint32_t>(edit_flag_enabled));

    stream.write(static_cast<IndexType>(nodes.size()));
    stream.write(static_cast<IndexType>(prim_ids.size()));
    for (auto&& node : nodes)
        node.serialize(stream);
    for (auto&& prim_id : prim_ids)
        stream.write(static_cast<IndexType>(prim_id));

    stream.write(static_cast<IndexType>(edit_state->boxes.size()));
    for (const auto& entry : edit_state->boxes) {
        stream.write(static_cast<IndexType>(entry.first));
        for (size_t axis = 0; axis < Node::dimension; ++axis) {
            stream.write(entry.second.min[axis]);
            stream.write(entry.second.max[axis]);
        }
    }
}

template <typename Node>
template <typename IndexType>
Bvh<Node> Bvh<Node>::deserialize(InputStream& stream) {
    Bvh bvh;
    const auto first = stream.read<IndexType>();
    if (first != static_cast<IndexType>(-1)) {
        // Original format.
        bvh.nodes.resize(first);
        bvh.prim_ids.resize(stream.read<IndexType>());
        for (auto& node : bvh.nodes)
            node = Node::deserialize(stream);
        for (auto& prim_id : bvh.prim_ids)
            prim_id = stream.read<IndexType>();
        return bvh;
    }

    const auto magic = stream.read<uint32_t>();
    const auto version = stream.read<uint32_t>();
    const auto flags = stream.read<uint32_t>();
    if (magic != edit_magic || version != edit_version)
        return bvh;

    bvh.nodes.resize(stream.read<IndexType>());
    bvh.prim_ids.resize(stream.read<IndexType>());
    for (auto& node : bvh.nodes)
        node = Node::deserialize(stream);
    for (auto& prim_id : bvh.prim_ids)
        prim_id = stream.read<IndexType>();

    if (flags & edit_flag_enabled) {
        EditState state;
        const auto box_count = stream.read<IndexType>();
        for (IndexType i = 0; i < box_count; ++i) {
            const auto id = stream.read<IndexType>();
            BBox box;
            for (size_t axis = 0; axis < Node::dimension; ++axis) {
                box.min[axis] = stream.read<Scalar>();
                box.max[axis] = stream.read<Scalar>();
            }
            state.boxes.emplace(static_cast<size_t>(id), box);
        }
        bvh.rebuild_edit_index(state);
        bvh.edit_state = std::move(state);
    }
    return bvh;
}

template <typename Node>
void Bvh<Node>::refit_ancestors(size_t node_id) {
    auto& state = *edit_state;
    size_t current = node_id;
    while (current != 0) {
        const size_t parent = state.parents[current];
        Node& node = nodes[parent];
        ++state.stats.node_visits;
        if (!node.is_leaf()) {
            state.stats.node_visits += 2;
            node.set_bbox(merge_bbox(
                nodes[node.index.first_id()].get_bbox(),
                nodes[node.index.first_id() + 1].get_bbox()));
        }
        current = parent;
    }
}

template <typename Node>
void Bvh<Node>::rebuild_edit_index(EditState& state) const {
    state.parents.assign(nodes.size(), 0);
    state.id_to_leaf.clear();
    state.free_runs.clear();
    state.stats = {};

    std::vector<bool> used(prim_ids.size(), false);
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (!node.is_leaf()) {
            const size_t first = node.index.first_id();
            // Corrupt input must not write out of bounds; the corresponding edits will simply
            // fail, which is preferable to undefined behavior while loading a file.
            if (first + 1 < nodes.size()) {
                state.parents[first + 0] = i;
                state.parents[first + 1] = i;
            }
        } else {
            const size_t first = node.index.first_id();
            const size_t count = static_cast<size_t>(node.index.prim_count());
            for (size_t j = first; j < first + count; ++j) {
                state.id_to_leaf[prim_ids[j]] = i;
                used[j] = true;
            }
        }
    }
    for (size_t i = 0; i < used.size();) {
        if (used[i]) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < used.size() && !used[i])
            ++i;
        state.free_runs.emplace(start, i - start);
    }
}

template <typename Node>
bool Bvh<Node>::enable_edits(std::span<const BBox> boxes) {
    if (edit_state)
        return false;

    const size_t prim_count = boxes.size();
    if (prim_count > max_primitive_id)
        return false;
    for (const auto& box : boxes) {
        for (size_t axis = 0; axis < Node::dimension; ++axis) {
            if (!std::isfinite(static_cast<double>(box.min[axis])) ||
                !std::isfinite(static_cast<double>(box.max[axis])) ||
                !(box.min[axis] <= box.max[axis]))
                return false;
        }
    }

    if (prim_count == 0) {
        // An empty BVH may still contain the degenerate root produced by a builder called with
        // zero primitives. Normalize it to the canonical empty representation so that later
        // insertions can start from scratch.
        if (!prim_ids.empty())
            return false;
        if (nodes.size() > 1)
            return false;
        if (nodes.size() == 1 && nodes[0].is_leaf())
            return false;
        nodes.clear();
        prim_ids.clear();
    } else {
        // The leaves must reference every given box exactly once, in any order.
        if (nodes.empty() || prim_ids.size() != prim_count)
            return false;
        size_t referenced_count = 0;
        for (const auto& node : nodes) {
            if (node.is_leaf()) {
                const size_t first = node.index.first_id();
                const size_t count = static_cast<size_t>(node.index.prim_count());
                if (first + count > prim_ids.size())
                    return false;
                referenced_count += count;
            } else if (node.index.first_id() + 1 >= nodes.size()) {
                return false;
            }
        }
        if (referenced_count != prim_count)
            return false;
        std::vector<bool> seen(prim_count, false);
        for (auto prim_id : prim_ids) {
            if (prim_id >= prim_count || seen[prim_id])
                return false;
            seen[prim_id] = true;
        }
    }

    EditState state;
    for (size_t i = 0; i < prim_count; ++i)
        state.boxes.emplace(i, boxes[i]);
    rebuild_edit_index(state);
    edit_state = std::move(state);
    return true;
}

template <typename Node>
void Bvh<Node>::trim_free_tail(EditState& state, std::vector<size_t>& prim_ids) {
    while (!state.free_runs.empty()) {
        auto it = std::prev(state.free_runs.end());
        if (it->first + it->second != prim_ids.size())
            break;
        prim_ids.resize(it->first);
        state.free_runs.erase(it);
    }
}

template <typename Node>
bool Bvh<Node>::insert_primitive(size_t id, const BBox& box) {
    if (!edit_state)
        return false;
    if (id >= max_primitive_id)
        return false;
    if (edit_state->boxes.find(id) != edit_state->boxes.end())
        return false;
    for (size_t axis = 0; axis < Node::dimension; ++axis) {
        if (!std::isfinite(static_cast<double>(box.min[axis])) ||
            !std::isfinite(static_cast<double>(box.max[axis])) ||
            !(box.min[axis] <= box.max[axis]))
            return false;
    }

    auto& state = *edit_state;
    state.stats = {};

    if (nodes.empty()) {
        // The empty state must have no other live primitive, otherwise the bookkeeping is
        // inconsistent and the edit is rejected without changing anything.
        if (!state.boxes.empty() || !state.id_to_leaf.empty())
            return false;
        // Recover from the empty state by creating a root leaf holding the new primitive.
        prim_ids.clear();
        state.free_runs.clear();
        state.parents.clear();
        const size_t slot = allocate_run(state, 1);
        prim_ids[slot] = id;
        nodes.emplace_back();
        state.parents.emplace_back(0);
        nodes[0].index = Index::make_leaf(slot, 1);
        nodes[0].set_bbox(box);
        state.stats.node_visits += 1;
        state.boxes.emplace(id, box);
        state.stats.primitive_visits += 1;
        state.id_to_leaf[id] = 0;
        return true;
    }

    // Descend along a single root-to-leaf path, always picking the child whose box grows the least.
    size_t node_id = 0;
    size_t leaf_id = 0;
    while (true) {
        Node& node = nodes[node_id];
        ++state.stats.node_visits;
        if (node.is_leaf()) {
            leaf_id = node_id;
            break;
        }
        const size_t left_id = node.index.first_id();
        const size_t right_id = left_id + 1;
        const auto& left = nodes[left_id];
        const auto& right = nodes[right_id];
        state.stats.node_visits += 2;
        const auto left_growth =
            merge_bbox(left.get_bbox(), box).get_half_area() - left.get_bbox().get_half_area();
        const auto right_growth =
            merge_bbox(right.get_bbox(), box).get_half_area() - right.get_bbox().get_half_area();
        node_id = left_growth <= right_growth ? left_id : right_id;
    }

    const size_t old_first = nodes[leaf_id].index.first_id();
    const size_t old_count = static_cast<size_t>(nodes[leaf_id].index.prim_count());
    const BBox old_bbox = nodes[leaf_id].get_bbox();
    const size_t capacity = static_cast<size_t>(Index::max_prim_count);

    if (old_count < capacity) {
        // Grow the leaf. The slots are moved to a run of the new size so that blocks of every size
        // can be recycled by the allocator.
        const size_t new_first = allocate_run(state, old_count + 1);
        for (size_t i = 0; i < old_count; ++i)
            prim_ids[new_first + i] = prim_ids[old_first + i];
        prim_ids[new_first + old_count] = id;
        release_run(state, old_first, old_count);

        Node& leaf = nodes[leaf_id];
        ++state.stats.node_visits;
        leaf.index = Index::make_leaf(new_first, old_count + 1);
        leaf.set_bbox(merge_bbox(old_bbox, box));
        state.boxes.emplace(id, box);
        state.stats.primitive_visits += 1;
        state.id_to_leaf[id] = leaf_id;
        refit_ancestors(leaf_id);
        return true;
    }

    // Split a full leaf: the leaf node becomes an inner node whose children are a copy of the
    // former leaf and a new leaf holding the inserted primitive.
    const size_t pair = nodes.size();
    nodes.resize(pair + 2);
    state.parents.resize(pair + 2, 0);
    const size_t slot = allocate_run(state, 1);
    prim_ids[slot] = id;

    const Node former_leaf = nodes[leaf_id];
    Node& new_inner = nodes[leaf_id];
    state.stats.node_visits += 4;
    new_inner.index = Index::make_inner(pair);
    new_inner.set_bbox(merge_bbox(old_bbox, box));
    nodes[pair] = former_leaf;
    nodes[pair + 1] = Node {};
    nodes[pair + 1].index = Index::make_leaf(slot, 1);
    nodes[pair + 1].set_bbox(box);
    state.parents[pair] = leaf_id;
    state.parents[pair + 1] = leaf_id;

    for (size_t i = 0; i < old_count; ++i)
        state.id_to_leaf[prim_ids[old_first + i]] = pair;
    state.boxes.emplace(id, box);
    state.stats.primitive_visits += 1;
    state.id_to_leaf[id] = pair + 1;
    refit_ancestors(leaf_id);
    return true;
}

template <typename Node>
void Bvh<Node>::remove_empty_leaf(size_t leaf_id) {
    auto& state = *edit_state;

    if (leaf_id == 0) {
        // The root itself was a leaf and its last primitive was removed.
        nodes.clear();
        prim_ids.clear();
        state.parents.clear();
        state.id_to_leaf.clear();
        state.free_runs.clear();
        return;
    }

    const size_t parent_id = state.parents[leaf_id];
    const size_t sibling_id = get_sibling_id(leaf_id);
    const size_t pair = get_left_sibling_id(leaf_id);

    // Replace the parent with the sibling subtree, so that the sibling takes the parent's place.
    const Node sibling = nodes[sibling_id];
    ++state.stats.node_visits;
    nodes[parent_id] = sibling;
    ++state.stats.node_visits;
    if (sibling.is_leaf()) {
        const size_t first = sibling.index.first_id();
        const size_t count = static_cast<size_t>(sibling.index.prim_count());
        for (size_t i = first; i < first + count; ++i)
            state.id_to_leaf[prim_ids[i]] = parent_id;
    } else {
        state.parents[sibling.index.first_id() + 0] = parent_id;
        state.parents[sibling.index.first_id() + 1] = parent_id;
    }

    // Compact the node array by moving the last sibling pair into the freed pair. This keeps every
    // node reachable from the root and every pair contiguous.
    const size_t last_pair = nodes.size() - 2;
    size_t refit_from = parent_id;
    if (pair != last_pair) {
        const Node a = nodes[last_pair + 0];
        const Node b = nodes[last_pair + 1];
        const size_t moved_parent = state.parents[last_pair];
        state.stats.node_visits += 4;

        // The moved pair may be a child of the sibling subtree, which was copied into the parent
        // slot; its new parent is then the copy at `parent_id`. Otherwise the parent is unchanged.
        const size_t new_parent = moved_parent == sibling_id ? parent_id : moved_parent;

        // The parent itself may be part of the last pair; in that case the hoisted sibling (which
        // now occupies a slot of that pair) moves to the freed pair, and so does the parent.
        const size_t moved_offset = parent_id >= last_pair ? parent_id - last_pair : SIZE_MAX;
        if (moved_offset < 2)
            refit_from = pair + moved_offset;

        nodes[pair + 0] = a;
        nodes[pair + 1] = b;
        state.parents[pair + 0] = new_parent;
        state.parents[pair + 1] = new_parent;
        Node& new_parent_node = nodes[new_parent];
        if (!new_parent_node.is_leaf())
            new_parent_node.index.set_first_id(pair);
        for (size_t i = 0; i < 2; ++i) {
            const size_t child = pair + i;
            Node& moved = nodes[child];
            if (moved.is_leaf()) {
                const size_t first = moved.index.first_id();
                const size_t count = static_cast<size_t>(moved.index.prim_count());
                for (size_t j = first; j < first + count; ++j)
                    state.id_to_leaf[prim_ids[j]] = child;
            } else {
                state.parents[moved.index.first_id() + 0] = child;
                state.parents[moved.index.first_id() + 1] = child;
            }
        }
    }

    nodes.resize(last_pair);
    state.parents.resize(last_pair);

    if (nodes.empty()) {
        prim_ids.clear();
        state.parents.clear();
        state.id_to_leaf.clear();
        state.free_runs.clear();
        return;
    }
    refit_ancestors(refit_from);
}

template <typename Node>
bool Bvh<Node>::erase_primitive(size_t id) {
    if (!edit_state)
        return false;
    auto& state = *edit_state;
    const auto it = state.id_to_leaf.find(id);
    if (it == state.id_to_leaf.end())
        return false;

    state.stats = {};
    const size_t leaf_id = it->second;
    state.id_to_leaf.erase(it);
    state.boxes.erase(id);
    state.stats.primitive_visits += 1;

    const size_t first = nodes[leaf_id].index.first_id();
    const size_t count = static_cast<size_t>(nodes[leaf_id].index.prim_count());
    ++state.stats.node_visits;

    if (count == 1) {
        release_run(state, first, count);
        remove_empty_leaf(leaf_id);
        return true;
    }

    size_t position = first;
    while (prim_ids[position] != id)
        ++position;

    // Relocate the leaf to a run of the smaller size, dropping the erased slot.
    const size_t new_first = allocate_run(state, count - 1);
    for (size_t i = 0, j = 0; i < count; ++i) {
        if (first + i == position)
            continue;
        prim_ids[new_first + j++] = prim_ids[first + i];
    }
    release_run(state, first, count);

    Node& leaf_node = nodes[leaf_id];
    ++state.stats.node_visits;
    BBox box = BBox::make_empty();
    for (size_t i = 0; i < count - 1; ++i) {
        box.extend(state.boxes.at(prim_ids[new_first + i]));
        ++state.stats.primitive_visits;
    }
    leaf_node.index = Index::make_leaf(new_first, count - 1);
    leaf_node.set_bbox(box);
    refit_ancestors(leaf_id);
    return true;
}

template <typename Node>
size_t Bvh<Node>::allocate_run(EditState& state, size_t length) {
    // Best fit: reusing the smallest free run that fits limits fragmentation, which keeps the
    // storage bounded when a small set of primitives is repeatedly inserted and erased.
    auto best = state.free_runs.end();
    auto it = state.free_runs.begin();
    for (; it != state.free_runs.end(); ++it) {
        if (it->second >= length && (best == state.free_runs.end() || it->second < best->second))
            best = it;
        if (it->second == length) {
            best = it;
            break;
        }
    }
    if (best != state.free_runs.end()) {
        const size_t start = best->first;
        const size_t available = best->second;
        state.free_runs.erase(best);
        if (available > length)
            state.free_runs.emplace(start + length, available - length);
        return start;
    }
    const size_t start = prim_ids.size();
    prim_ids.resize(start + length);
    return start;
}

template <typename Node>
void Bvh<Node>::release_run(EditState& state, size_t start, size_t length) {
    if (length == 0)
        return;
    auto right = state.free_runs.find(start + length);
    if (right != state.free_runs.end()) {
        length += right->second;
        state.free_runs.erase(right);
    }
    auto left = state.free_runs.lower_bound(start);
    if (left != state.free_runs.begin()) {
        --left;
        if (left->first + left->second == start) {
            start = left->first;
            length += left->second;
            state.free_runs.erase(left);
        }
    }
    state.free_runs.emplace(start, length);
    trim_free_tail(state, prim_ids);
}

} // namespace bvh::v2

#endif
