#ifndef NODE_H
#define NODE_H
#include "util/Utilities.h"

#include "bvh/BoundingBox.h"
#include "object/ObjectList.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

class Node : public Object {
  public:
    shared_ptr<Object> left;
    shared_ptr<Object> right;


    Node(ObjectList list) : Node(list.objects, 0, list.objects.size()) {
    }

    Node(std::vector<shared_ptr<Object>>& objects, size_t start, size_t end) {
        // Calculate the bounding box of all objects in the list

        // random_splits(objects, start, end);
        // la_splits(objects, start, end);
        sah_splits(objects, start, end);
        visibilityMask = compute_visibility_mask();
    }

    // Different splitting strategies
    void random_splits(std::vector<shared_ptr<Object>>& objects, size_t start, size_t end);
    void la_splits(std::vector<shared_ptr<Object>>& objects, size_t start, size_t end);
    void sah_splits(std::vector<shared_ptr<Object>>& objects, size_t start, size_t end);

    bool intersect(const Ray& r, Interval ray_t, IntersectionRecord& rec) const override {
        // No object below this node carries a layer the ray queries.
        if (!visibility_matches(visibilityMask, r.visibility_mask())) {
            return false;
        }

        // Check if the ray intersects the bounding box of this node
        if (!bbox.intersect(r, ray_t)) {
            return false;
        }

        // Traverse child nodes
        bool hit_left = left && left->intersect(r, ray_t, rec);
        bool hit_right = right && right->intersect(r, Interval(ray_t.min, hit_left ? rec.t : ray_t.max), rec);

        // If a hit was found, terminate traversal early
        return hit_left || hit_right;
    }

    BoundingBox bounding_box() const override { return bbox; }

    // Aggregate OR of every primitive mask beneath this node.
    std::uint32_t visibility_mask() const override { return visibilityMask; }

    std::uint32_t cached_visibility_mask() const { return visibilityMask; }

    // Build the Object* -> node index map over the whole subtree. Each object
    // occurrence in the tree gets an entry, so shared or duplicated primitives
    // resolve to every leaf that references them. Parent links are recorded in
    // preorder alongside the flat node list.
    void refresh_index(std::unordered_map<const Object*, std::vector<size_t>>& occurrences,
                       std::vector<Node*>& nodes,
                       std::vector<size_t>& parents,
                       size_t parent_index) const;

    // Re-read the primitive masks of this leaf and recompute its cached OR.
    // Counts one primitive read per object occurrence and one reduction.
    void refresh_reduce(std::uint64_t& object_reads, std::uint64_t& node_reductions);

    // Recompute the cached OR from the two children (one reduction). Returns
    // true when the cached value changed, so propagation can stop early.
    bool refresh_propagate(std::uint64_t& node_reductions);

  private:
    BoundingBox bbox;

    // OR of the masks of every object beneath this node.
    std::uint32_t compute_visibility_mask() const;
};



inline void print_bvh_structure(const std::shared_ptr<Node>& node, int depth = 0) {
    if (!node) return;
    // Print current node's bounding box
    std::cout << std::string(depth * 2, ' ') << "Node at depth " << depth << ": "
              << "Bounding Box [X: (" << node->bounding_box().x.min << ", " << node->bounding_box().x.max
              << "), Y: (" << node->bounding_box().y.min << ", " << node->bounding_box().y.max
              << "), Z: (" << node->bounding_box().z.min << ", " << node->bounding_box().z.max << ")]\n";
    if (!node->left && !node->right) {
        std::cout << std::string(depth * 2, ' ') << "  Leaf Node\n";
        return;
    }
    // Recursively print children
    if (node->left) {
        std::cout << std::string(depth * 2, ' ') << "  Left Child:\n";
        print_bvh_structure(std::dynamic_pointer_cast<Node>(node->left), depth + 1);
    }
    if (node->right) {
        std::cout << std::string(depth * 2, ' ') << "  Right Child:\n";
        print_bvh_structure(std::dynamic_pointer_cast<Node>(node->right), depth + 1);
    }
}

#endif
