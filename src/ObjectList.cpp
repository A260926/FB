#include "object/ObjectList.h"
#include "bvh/Node.h"
#include "object/Sphere.h"
#include "object/Triangle.h"
#include "object/Cylinder.h"

#include <queue>
#include <unordered_set>
#include <utility>

ObjectList::ObjectList() {}

ObjectList::ObjectList(std::shared_ptr<Object> object) {
    add(object);
}

void ObjectList::clear() {
    objects.clear();
    excluded_objects.clear();
    leaf_index_.clear();
    root.reset();
}

void ObjectList::add(std::shared_ptr<Object> object) {
    objects.push_back(object);
}

void ObjectList::build() {
    // A rebuild replaces the previous excluded list and leaf index entirely.
    excluded_objects.clear();
    leaf_index_.clear();
    root.reset();

    // Build the BVH
    std::vector<std::shared_ptr<Object>> bvh_objects;

    for (const auto& obj : objects) {
        if (obj->exclude_from_bvh()) {
            excluded_objects.push_back(obj);
        } else {
            bvh_objects.push_back(obj);
        }
    }
    if (!bvh_objects.empty()) {
        root = std::make_shared<Node>(bvh_objects, 0, bvh_objects.size());
        index_leaves(root.get());
    }
}

void ObjectList::index_leaves(Node* node) {
    if (!node) return;
    if (node->left) {
        if (Node* left_node = dynamic_cast<Node*>(node->left.get())) {
            index_leaves(left_node);
        } else {
            // Leaf slot holding a primitive: record the occurrence.
            leaf_index_.emplace(node->left.get(), node);
        }
    }
    if (node->right) {
        if (Node* right_node = dynamic_cast<Node*>(node->right.get())) {
            index_leaves(right_node);
        } else {
            leaf_index_.emplace(node->right.get(), node);
        }
    }
}

VisibilityRefreshStats ObjectList::refresh_visibility(const std::vector<const Object*>& dirty) {
    VisibilityRefreshStats stats;
    if (dirty.empty()) return stats;

    // De-duplicate the dirty list: each distinct object is processed once.
    std::unordered_set<const Object*> seen;
    std::vector<const Object*> unique_dirty;
    unique_dirty.reserve(dirty.size());
    for (const Object* obj : dirty) {
        if (obj && seen.insert(obj).second) {
            unique_dirty.push_back(obj);
        }
    }

    // Ancestors waiting for a recomputation, ordered deepest-first so that
    // children are always reduced before their parents and each affected
    // node is reduced at most once.
    std::priority_queue<std::pair<int, Node*>> pending;
    std::unordered_set<Node*> queued;
    auto queue_node = [&](Node* node) {
        if (node && queued.insert(node).second) {
            pending.emplace(node->depth, node);
        }
    };

    // Locate every leaf occurrence of each dirty object and refresh its cache.
    for (const Object* obj : unique_dirty) {
        auto range = leaf_index_.equal_range(obj);
        if (range.first == range.second) {
            continue;  // Not part of this tree (e.g. excluded from the BVH)
        }
        ++stats.object_reads;
        const std::uint32_t mask = obj->visibility_mask();
        for (auto it = range.first; it != range.second; ++it) {
            Node* leaf = it->second;
            ++stats.node_reductions;  // Recompute the leaf's cached OR
            if (leaf->visibility_or == mask) {
                continue;  // Unchanged: nothing to propagate
            }
            leaf->visibility_or = mask;
            queue_node(leaf->parent);
        }
    }

    // Propagate the changes towards the root.
    while (!pending.empty()) {
        Node* node = pending.top().second;
        pending.pop();
        std::uint32_t mask = 0;
        for (const shared_ptr<Object>* child : {&node->left, &node->right}) {
            if (!*child) continue;
            if (const Node* child_node = dynamic_cast<const Node*>(child->get())) {
                mask |= child_node->visibility_or;  // Internal cache: not an object read
            } else {
                ++stats.object_reads;
                mask |= (*child)->visibility_mask();
            }
        }
        ++stats.node_reductions;
        if (mask == node->visibility_or) {
            continue;  // OR unchanged: ancestors cannot change either, stop early
        }
        node->visibility_or = mask;
        queue_node(node->parent);
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