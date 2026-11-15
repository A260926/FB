#ifndef NODE_H
#define NODE_H
#include "util/Utilities.h"

#include "bvh/BoundingBox.h"
#include "object/ObjectList.h"

#include <algorithm>

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
    }

    // Different splitting strategies
    void random_splits(std::vector<shared_ptr<Object>>& objects, size_t start, size_t end);
    void la_splits(std::vector<shared_ptr<Object>>& objects, size_t start, size_t end);
    void sah_splits(std::vector<shared_ptr<Object>>& objects, size_t start, size_t end);

    bool intersect(const Ray& r, Interval ray_t, IntersectionRecord& rec) const override {
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

  private:
    BoundingBox bbox;
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