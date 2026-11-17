#ifndef OBJECTLIST_H
#define OBJECTLIST_H

#include "util/Utilities.h"
#include "object/Object.h"
#include "bvh/BoundingBox.h"

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <memory>

// Forward declaration of Node
class Node;

// Work counters reported by ObjectList::refresh_visibility().
struct VisibilityRefreshStats {
    std::uint64_t object_reads = 0;    // Number of primitive visibility-mask reads performed
    std::uint64_t node_reductions = 0; // Number of node visibility-OR recomputations performed
};

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
      // Hidden from this ray's layers: treat the whole list as absent.
      if ((r.visibility_mask() & visibility_mask()) == 0) return false;
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

    // Re-apply the current visibility masks of the given objects to the BVH
    // caches. Callers mutate object masks first, then refresh every scene
    // that uses them, and only then issue queries. Only the leaf occurrences
    // of the dirty objects and their ancestors are touched; the tree
    // topology, root pointer, and bounding boxes are left unchanged.
    VisibilityRefreshStats refresh_visibility(const std::vector<const Object*>& dirty);

    BoundingBox bounding_box() const override;

    // print out all the hit counts of the objects in the list
    void log_hit_counts() const;

  private:
    // Index from an object pointer to every leaf node that references it.
    // Rebuilt by build(); used by refresh_visibility() to locate occurrences
    // without scanning the tree.
    std::unordered_multimap<const Object*, Node*> leaf_index_;

    void index_leaves(Node* node);
};

#endif
