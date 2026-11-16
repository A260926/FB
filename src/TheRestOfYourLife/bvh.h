#ifndef BVH_H
#define BVH_H
//==============================================================================================
// Originally written in 2016 by Peter Shirley <ptrshrl@gmail.com>
//
// To the extent possible under law, the author(s) have dedicated all copyright and related and
// neighboring rights to this software to the public domain worldwide. This software is
// distributed without any warranty.
//
// You should have received a copy (see file COPYING.txt) of the CC0 Public Domain Dedication
// along with this software. If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.
//==============================================================================================

#include "aabb.h"
#include "hittable.h"
#include "hittable_list.h"

#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>


class bvh_node : public hittable {
  public:
    bvh_node(hittable_list list) : bvh_node(list.objects, 0, list.objects.size()) {
        // There's a C++ subtlety here. This constructor (without span indices) creates an
        // implicit copy of the hittable list, which we will modify. The lifetime of the copied
        // list only extends until this constructor exits. That's OK, because we only need to
        // persist the resulting bounding volume hierarchy.
    }

    bvh_node(std::vector<shared_ptr<hittable>>& objects, size_t start, size_t end) {
        // Build the bounding box of the span of source objects.
        bbox = aabb::empty;
        for (size_t object_index=start; object_index < end; object_index++)
            bbox = aabb(bbox, objects[object_index]->bounding_box());

        int axis = bbox.longest_axis();

        auto comparator = (axis == 0) ? box_x_compare
                        : (axis == 1) ? box_y_compare
                                      : box_z_compare;

        size_t object_span = end - start;

        if (object_span == 1) {
            left = right = objects[start];
        } else if (object_span == 2) {
            left = objects[start];
            right = objects[start+1];
        } else {
            std::sort(std::begin(objects) + start, std::begin(objects) + end, comparator);

            auto mid = start + object_span/2;
            left = make_shared<bvh_node>(objects, start, mid);
            right = make_shared<bvh_node>(objects, mid, end);
        }
    }

    // Build a tree whose leaves remember the stable input slot (the object's
    // index in `objects`). The hierarchy is built exactly like the legacy
    // constructor; only the slot tag travels with each object while sorting.
    static shared_ptr<bvh_node> build_slotted(std::vector<shared_ptr<hittable>> objects) {
        std::vector<slot_entry> entries;
        entries.reserve(objects.size());
        for (size_t i = 0; i < objects.size(); i++)
            entries.push_back(slot_entry{i, objects[i]});
        return make_shared<bvh_node>(entries, 0, entries.size());
    }

    // Persistent leaf replacement. `replacements` maps stable leaf slots to new
    // leaf objects. Only the replaced leaves and the ancestor paths leading to
    // them are allocated; every unaffected subtree keeps its original node and
    // is shared with the old tree. No node reachable from `root` is modified and
    // no re-sort or full rebuild happens. An empty replacement map returns the
    // very same root pointer. `root` must be a tree produced by build_slotted.
    static shared_ptr<hittable> replace_leaves(
        const shared_ptr<hittable>& root,
        const std::vector<std::pair<size_t, shared_ptr<hittable>>>& replacements
    ) {
        if (replacements.empty())
            return root;

        std::unordered_map<size_t, shared_ptr<hittable>> by_slot;
        by_slot.reserve(replacements.size());
        for (const auto& entry : replacements)
            by_slot.insert(entry);

        return update_rec(root, by_slot);
    }

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
        if (!bbox.hit(r, ray_t))
            return false;

        bool hit_left = left->hit(r, ray_t, rec);
        bool hit_right = right->hit(r, interval(ray_t.min, hit_left ? rec.t : ray_t.max), rec);

        return hit_left || hit_right;
    }

    aabb bounding_box() const override { return bbox; }

    // --- Read-only diagnostics for verifying persistence and sharing ---

    bool is_leaf() const { return node_leaf_slot != invalid_instance_id; }

    // Stable input slot of a slotted leaf; invalid_instance_id for internal
    // nodes and for trees built through the legacy constructors.
    size_t leaf_slot() const { return node_leaf_slot; }

    const void* node_id() const { return static_cast<const void*>(this); }
    const void* left_id() const { return left.get(); }
    const void* right_id() const { return right.get(); }
    const shared_ptr<hittable>& left_child() const { return left; }
    const shared_ptr<hittable>& right_child() const { return right; }
    const aabb& node_bounds() const { return bbox; }

    // --- Persistent-tree construction (used by build_slotted/replace_leaves) ---

    struct slot_entry {
        size_t slot;
        shared_ptr<hittable> object;
    };

    // Slotted leaf node: a single wrapped object tagged with its stable slot.
    bvh_node(size_t slot, const shared_ptr<hittable>& object)
        : left(object), right(object), bbox(object->bounding_box()), node_leaf_slot(slot) {}

    // Slotted internal node: recomputes the box from its two children.
    bvh_node(const shared_ptr<hittable>& left_child,
             const shared_ptr<hittable>& right_child)
        : left(left_child), right(right_child),
          bbox(left_child->bounding_box(), right_child->bounding_box()) {}

    bvh_node(std::vector<slot_entry>& entries, size_t start, size_t end) {
        bbox = aabb::empty;
        for (size_t i = start; i < end; i++)
            bbox = aabb(bbox, entries[i].object->bounding_box());

        int axis = bbox.longest_axis();

        auto comparator = (axis == 0) ? slot_box_x_compare
                        : (axis == 1) ? slot_box_y_compare
                                      : slot_box_z_compare;

        size_t object_span = end - start;

        if (object_span == 1) {
            node_leaf_slot = entries[start].slot;
            left = right = entries[start].object;
        } else if (object_span == 2) {
            // Keep the same shape as the legacy builder: two distinct leaf nodes.
            left = make_shared<bvh_node>(entries[start].slot, entries[start].object);
            right = make_shared<bvh_node>(entries[start+1].slot, entries[start+1].object);
        } else {
            std::sort(std::begin(entries) + start, std::begin(entries) + end, comparator);

            auto mid = start + object_span/2;
            left = make_shared<bvh_node>(entries, start, mid);
            right = make_shared<bvh_node>(entries, mid, end);
        }
    }

  private:
    shared_ptr<hittable> left;
    shared_ptr<hittable> right;
    aabb bbox;
    size_t node_leaf_slot = invalid_instance_id;

    static shared_ptr<hittable> update_rec(
        const shared_ptr<hittable>& node,
        const std::unordered_map<size_t, shared_ptr<hittable>>& by_slot
    ) {
        const auto* current = dynamic_cast<const bvh_node*>(node.get());

        if (current->is_leaf()) {
            auto found = by_slot.find(current->node_leaf_slot);
            if (found == by_slot.end())
                return node;  // Unchanged leaf: share the original node.
            return make_shared<bvh_node>(current->node_leaf_slot, found->second);
        }

        auto new_left = update_rec(current->left, by_slot);
        auto new_right = update_rec(current->right, by_slot);

        if (new_left == current->left && new_right == current->right)
            return node;  // Neither subtree changed: share the original node.

        // A shared ancestor of several replaced leaves is cloned exactly once,
        // here on the single unwind of the recursion.
        return make_shared<bvh_node>(new_left, new_right);
    }

    static bool slot_box_compare(const slot_entry& a, const slot_entry& b, int axis_index) {
        auto a_axis_interval = a.object->bounding_box().axis_interval(axis_index);
        auto b_axis_interval = b.object->bounding_box().axis_interval(axis_index);
        return a_axis_interval.min < b_axis_interval.min;
    }

    static bool slot_box_x_compare(const slot_entry& a, const slot_entry& b) {
        return slot_box_compare(a, b, 0);
    }

    static bool slot_box_y_compare(const slot_entry& a, const slot_entry& b) {
        return slot_box_compare(a, b, 1);
    }

    static bool slot_box_z_compare(const slot_entry& a, const slot_entry& b) {
        return slot_box_compare(a, b, 2);
    }

    static bool box_compare(
        const shared_ptr<hittable> a, const shared_ptr<hittable> b, int axis_index
    ) {
        auto a_axis_interval = a->bounding_box().axis_interval(axis_index);
        auto b_axis_interval = b->bounding_box().axis_interval(axis_index);
        return a_axis_interval.min < b_axis_interval.min;
    }

    static bool box_x_compare (const shared_ptr<hittable> a, const shared_ptr<hittable> b) {
        return box_compare(a, b, 0);
    }

    static bool box_y_compare (const shared_ptr<hittable> a, const shared_ptr<hittable> b) {
        return box_compare(a, b, 1);
    }

    static bool box_z_compare (const shared_ptr<hittable> a, const shared_ptr<hittable> b) {
        return box_compare(a, b, 2);
    }
};


#endif
