#include "object/ObjectList.h"
#include "bvh/Node.h"
#include "object/Sphere.h"
#include "object/Triangle.h"
#include "object/Cylinder.h"

#include <algorithm>
#include <unordered_set>

ObjectList::ObjectList() {}

ObjectList::ObjectList(std::shared_ptr<Object> object) {
    add(object);
}

void ObjectList::clear() {
    // Drop the objects together with every derived index so no stale
    // primitive or node reference survives.
    objects.clear();
    excluded_objects.clear();
    root.reset();
    tree_nodes.clear();
    tree_parents.clear();
    object_occurrences.clear();
    node_schedule_generation.clear();
    refresh_generation = 0;
}

void ObjectList::add(std::shared_ptr<Object> object) {
    objects.push_back(object);
}

void ObjectList::build() {
    // Build the BVH
    std::vector<std::shared_ptr<Object>> bvh_objects;

    // A rebuild replaces the old excluded list and index outright.
    excluded_objects.clear();
    tree_nodes.clear();
    tree_parents.clear();
    object_occurrences.clear();
    node_schedule_generation.clear();
    refresh_generation = 0;
    root.reset();

    for (const auto& obj : objects) {
        if (obj->exclude_from_bvh()) {
            excluded_objects.push_back(obj);
        } else {
            bvh_objects.push_back(obj);
        }
    }
    if (!bvh_objects.empty()) {
        root = std::make_shared<Node>(bvh_objects, 0, bvh_objects.size());
        build_refresh_index();
        node_schedule_generation.assign(tree_nodes.size(), 0);
        refresh_generation = 0;
    }
}

void ObjectList::build_refresh_index() {
    if (!root) return;
    constexpr size_t no_parent = static_cast<size_t>(-1);
    root->refresh_index(object_occurrences, tree_nodes, tree_parents, no_parent);
}

VisibilityRefreshStats ObjectList::refresh_visibility(const std::vector<const Object*>& dirty) {
    VisibilityRefreshStats stats;
    if (dirty.empty() || !root) {
        return stats;
    }

    // De-duplicate the dirty pointers and keep only primitives that actually
    // occur in this tree. This enumeration reads each unique primitive mask
    // once and is charged as a primitive read.
    std::unordered_set<const Object*> unique_dirty;
    unique_dirty.reserve(dirty.size());
    for (const Object* obj : dirty) {
        if (!obj || object_occurrences.find(obj) == object_occurrences.end()) {
            continue;
        }
        unique_dirty.insert(obj);
    }
    if (unique_dirty.empty()) {
        return stats;
    }

    if (unique_dirty.empty()) {
        return stats;
    }
    ++refresh_generation;
    const std::uint64_t generation = refresh_generation;

    std::vector<size_t> pending;  // Internal nodes awaiting reduction.

    auto is_scheduled = [&](size_t index) {
        return node_schedule_generation[index] == generation;
    };
    auto mark_scheduled = [&](size_t index) {
        node_schedule_generation[index] = generation;
    };

    // Phase 1: reduce every dirty leaf occurrence (each at most once). Only
    // leaves whose cached OR actually changed need to propagate upward.
    for (const Object* obj : unique_dirty) {
        ++stats.object_reads;  // Enumeration read of the primitive mask.

        for (size_t leaf_index : object_occurrences[obj]) {
            Node* leaf = tree_nodes[leaf_index];
            const std::uint32_t old_mask = leaf->cached_visibility_mask();
            leaf->refresh_reduce(stats.object_reads, stats.node_reductions);

            if (leaf->cached_visibility_mask() == old_mask) {
                continue;  // Nothing above this leaf can change through it.
            }
            const size_t parent = tree_parents[leaf_index];
            if (parent != static_cast<size_t>(-1) && !is_scheduled(parent)) {
                mark_scheduled(parent);
                pending.push_back(parent);
            }
        }
    }

    // Phase 2: bottom-up propagation. The flat tree is in preorder, so a parent
    // always has a smaller index than its descendants: picking the largest
    // pending index finalizes every changed child before its parent. Each
    // affected internal node is reduced at most once; an unchanged OR stops the
    // propagation along that subtree. Scratch stays within the affected union.
    while (!pending.empty()) {
        size_t chosen = 0;
        for (size_t i = 1; i < pending.size(); ++i) {
            if (pending[i] > pending[chosen]) {
                chosen = i;
            }
        }
        const size_t current = pending[chosen];
        pending[chosen] = pending.back();
        pending.pop_back();

        Node* node = tree_nodes[current];
        const bool changed = node->refresh_propagate(stats.node_reductions);

        if (changed) {
            const size_t parent = tree_parents[current];
            if (parent != static_cast<size_t>(-1) && !is_scheduled(parent)) {
                mark_scheduled(parent);
                pending.push_back(parent);
            }
        }
    }

    return stats;
}

bool ObjectList::intersect_bvh_enabled(const Ray& r, Interval ray_t, IntersectionRecord& rec) const {
    // Check if the ray intersects the BVH
    IntersectionRecord temp_rec;
    bool intersected = false;
    auto nearest_t = ray_t.max;

    // Test BVH
    if (root && root->intersect(r, ray_t, temp_rec)) {
        intersected = true;
        nearest_t = temp_rec.t;
        rec = temp_rec;
    }

    // Test excluded objects
    for (const auto& object : excluded_objects) {
        if (object->intersect(r, Interval(ray_t.min, nearest_t), temp_rec)) {
            intersected = true;
            nearest_t = temp_rec.t;
            rec = temp_rec;
        }
    }

    return intersected;
}

// NOT USING BVH
bool ObjectList::intersect_bvh_disabled(const Ray& r, Interval ray_t, IntersectionRecord& rec) const {
    // Check if the ray intersects any object in the list
    IntersectionRecord temp_rec;
    bool intersected = false;
    auto nearest_t = ray_t.max;

    for (const auto& object : objects) {
        if (object->intersect(r, Interval(ray_t.min, nearest_t), temp_rec)) {
            intersected = true;
            nearest_t = temp_rec.t;
            rec = temp_rec;
        }
    }

    return intersected;
}


BoundingBox ObjectList::bounding_box() const {
    return root ? root->bounding_box() : BoundingBox();
}

void ObjectList::log_hit_counts() const {
    for (const auto& object : objects) {
        // Check if the object is a sphere
        if (auto sphere_ptr = std::dynamic_pointer_cast<Sphere>(object)) {
            std::cout << "Sphere hit_call_count: " << sphere_ptr->hit_call_count << std::endl;
        }
        // Check if the object is a triangle
        else if (auto triangle_ptr = std::dynamic_pointer_cast<Triangle>(object)) {
            std::cout << "Triangle hit_call_count: " << triangle_ptr->hit_call_count << std::endl;
        }
        // Check if the object is a cylinder
        else if (auto cylinder_ptr = std::dynamic_pointer_cast<Cylinder>(object)) {
            std::cout << "Cylinder hit_call_count: " << cylinder_ptr->hit_call_count << std::endl;
        }
        // Handle other hittable objects
        else {
            std::cout << "Unknown object type, skipping." << std::endl;
        }
    }
}
