#ifndef OBJECTLIST_H
#define OBJECTLIST_H

#include "util/Utilities.h"
#include "object/Object.h"
#include "bvh/BoundingBox.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <memory>

// Forward declaration of Node
class Node;

// Work performed by a single ObjectList::refresh_visibility() call.
struct VisibilityRefreshStats {
    // Number of times a primitive's visibility mask was actually read
    // (including the reads done while enumerating dirty occurrences).
    std::uint64_t object_reads = 0;
    // Number of node OR reductions performed (leaf and internal).
    std::uint64_t node_reductions = 0;
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

    // Re-read the masks of the given primitives and update the cached node ORs
    // without rebuilding the tree. Call after changing the primitives' masks.
    // Duplicate entries and objects absent from this list are ignored; an empty
    // dirty set does nothing. Objects outside the BVH (excluded or BVH
    // disabled) need no cached state, so they cost no work here.
    VisibilityRefreshStats refresh_visibility(const std::vector<const Object*>& dirty);

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

    // Flat, address-stable view of the tree and the map from a primitive to
    // every node index where it occurs. Rebuilt by build() together with the
    // BVH; exposed so callers (and the layer tests) can inspect locality.
    std::vector<Node*> tree_nodes;
    std::vector<std::size_t> tree_parents;  // Parent node index; size_t(-1) for the root.
    std::unordered_map<const Object*, std::vector<std::size_t>> object_occurrences;

  private:
    void build_refresh_index();

    // Persistent "already scheduled" stamp for refresh_visibility(): a node is
    // scheduled in the current refresh when its generation matches. Avoids an
    // O(node count) clear on every incremental refresh.
    std::vector<std::uint64_t> node_schedule_generation;
    std::uint64_t refresh_generation = 0;
};

#endif
