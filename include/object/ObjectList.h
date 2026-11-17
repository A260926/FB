#ifndef OBJECTLIST_H
#define OBJECTLIST_H

#include "util/Utilities.h"
#include "object/Object.h"
#include "bvh/BoundingBox.h"

#include <vector>
#include <memory>

// Forward declaration of Node
class Node;

// Child class of Object
// Represents a list of objects in the scene
class ObjectList : public Object {
  public:
    std::vector<shared_ptr<Object>> objects;  // List of objects
    std::shared_ptr<Node> root;  // BVH root node
    std::vector<shared_ptr<Object>> excluded_objects;

    UseBVH use_bvh = UseBVH::ENABLE;  // Use BVH or not

    ObjectList();
    ObjectList(std::shared_ptr<Object> object);

    void clear();
    void add(std::shared_ptr<Object> object);
    void build();

    bool intersect(const Ray& r, Interval ray_t, IntersectionRecord& rec) const override {
      // Check if the ray intersects any object in the list
      bool hit_anything = false;
      switch (use_bvh) {
          case UseBVH::ENABLE:
              hit_anything = intersect_bvh_enabled(r, ray_t, rec);
              break;
          case UseBVH::DISABLE:
              hit_anything = intersect_bvh_disabled(r, ray_t, rec);
              break;
      }
      return hit_anything;
    };

    // Intersection test with and without BVH
    bool intersect_bvh_enabled(const Ray& r, Interval ray_t, IntersectionRecord& rec) const;
    bool intersect_bvh_disabled(const Ray& r, Interval ray_t, IntersectionRecord& rec) const;

    BoundingBox bounding_box() const override;

    // print out all the hit counts of the objects in the list
    void log_hit_counts() const;
};

#endif