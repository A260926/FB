#ifndef BEAM_TRAVERSAL_HPP_
#define BEAM_TRAVERSAL_HPP_

// CPU primary-ray beam traversal.
//
// The scene is organised as a BVH over objects and the image as a
// quadtree of pixel rectangles.  A traversal state is a pair
// (screen region, object node).  For every pair a cone that contains
// all real sample directions of the region is tested against the
// node's bounding box; only when the two overlap do we split either
// the screen region or the object node.  Exact Object::Intersects
// calls happen exclusively at (single pixel, object leaf) pairs.
//
// Adjacent pixels therefore share the expensive cone/box decisions,
// while the per-pixel nearest hit is still computed with the original
// object intersection routines.

#include "camera.hpp"
#include "common.hpp"
#include "objects.hpp"
#include "ray.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace beam {

// Counts of work actually performed by the primary-ray dual
// (screen quadtree / object BVH) traversal.  They are reset for every
// frame.
struct WorkStats {
  // a real cone-vs-box test was evaluated
  std::uint64_t beam_box_tests{0};
  // a whole (region, node) pair was discarded because the node's
  // conservative minimum forward depth exceeded the region maximum
  std::uint64_t depth_rejects{0};
  // Object::Intersects was invoked for a primary ray
  std::uint64_t object_tests{0};
};

// Nearest primary hit of one pixel.
struct Hit {
  // original object from the AddObject slot
  const Object *object{nullptr};
  // AddObject input slot, max() when nothing was hit
  std::size_t slot{std::numeric_limits<std::size_t>::max()};
  // exact record returned by Object::Intersects
  HitRecord record{};
  // camera-forward depth of record.where (infinity on a miss)
  float forward_depth{std::numeric_limits<float>::infinity()};

  bool IsHit() const { return object != nullptr; }
};

// One rectangle of the screen quadtree.  Public so callers can verify
// the hierarchy, its children and the reduced depth values.
struct ScreenNode {
  // columns [col_begin, col_end), rows [row_begin, row_end)
  std::size_t col_begin{0};
  std::size_t col_end{0};
  std::size_t row_begin{0};
  std::size_t row_end{0};
  // child node indices, -1 when absent
  std::array<int, 4> children{{-1, -1, -1, -1}};
  int child_count{0};
  int parent{-1};
  // row * width + col for a 1x1 leaf, -1 for an internal node
  int pixel_index{-1};
  // max over the current nearest-hit forward depths of all real
  // pixels in this rectangle; infinity while any pixel is a miss
  float max_forward_depth{std::numeric_limits<float>::infinity()};

  bool IsLeaf() const { return child_count == 0; }
  std::size_t Width() const { return col_end - col_begin; }
  std::size_t Height() const { return row_end - row_begin; }
};

// One node of the object BVH.  `begin`/`end` index into
// BeamTraversal::object_slots(), which stores the original AddObject
// slot of every leaf entry.
struct ObjectNode {
  AABB bounds{};
  // false for an empty / non-finite box: no culling is allowed then
  bool bounds_known{false};
  int left{-1};
  int right{-1};
  int begin{0};
  int end{0};

  bool IsLeaf() const { return left < 0 && right < 0; }
};

class BeamTraversal {
public:
  BeamTraversal() = default;

  // Synchronise the object hierarchy with the objects, given in
  // AddObject slot order.  The topology is rebuilt only when the
  // number or the identity/order of the pointers changed; otherwise
  // the existing topology is refitted from the objects' current
  // bounds.  Returns true when a rebuild happened.
  bool SyncObjects(const std::vector<Object *> &objects);

  // Traverse the current camera's sampling rays.  Per-frame state
  // (pixel hits, reduced screen depths and WorkStats) is cleared
  // first.  The camera projection itself is not modified.
  void Run(const Camera &camera);

  // Clear per-frame screen state without changing the topologies.
  void ResetFrame();

  std::size_t width() const { return width_; }
  std::size_t height() const { return height_; }

  // World position of the real sample of pixel (row, col) using the
  // same col/(width-1), row/(height-1) rule as RayTracer::Trace.
  Vec3f PixelWorld(std::size_t row, std::size_t col) const;

  const std::vector<Hit> &hits() const { return hits_; }
  const Hit &hit(std::size_t row, std::size_t col) const {
    return hits_[row * width_ + col];
  }
  const WorkStats &stats() const { return stats_; }

  // ---- inspection -------------------------------------------------
  int screen_root() const { return screen_root_; }
  int object_root() const { return object_root_; }
  const std::vector<ScreenNode> &screen_nodes() const {
    return screen_nodes_;
  }
  const std::vector<ObjectNode> &object_nodes() const {
    return object_nodes_;
  }
  // Leaf order -> AddObject slot.  A permutation of 0..N-1.
  const std::vector<int> &object_slots() const { return object_slots_; }
  // Objects in AddObject slot order (the vector given to
  // SyncObjects); objects_view()[slot] matches Hit::slot.
  const std::vector<Object *> &objects_view() const { return objects_; }
  // Changes only when the BVH topology is rebuilt.
  std::uint64_t topology_generation() const { return topology_generation_; }
  bool last_sync_rebuilt() const { return last_sync_rebuilt_; }

private:
  // Conservative direction bound for one screen region.  For a full
  // rectangle the four inward-facing planes through the eye and the
  // rectangle edges bound a (double) pyramid that contains every ray
  // of the region.  For one-row / one-column regions the region lies
  // on a line through the eye; the plane through that line is then
  // used from both sides.
  struct Plane {
    std::array<double, 3> normal{{0.0, 0.0, 0.0}};
    double offset{0}; // n.p + offset = 0 on the plane
  };
  struct RegionCone {
    // inward half-space boundaries of a full rectangular region
    std::array<Plane, 4> boundary{};
    std::array<bool, 4> boundary_valid{{false, false, false, false}};
    // For a one-row / one-column region all rays lie in one plane
    // through the eye; a box strictly off that plane cannot be hit.
    Plane thin{};
    bool thin_valid{false};
  };

  void BuildScreen();
  int BuildScreenNode(std::size_t col_begin, std::size_t row_begin,
                      std::size_t col_end, std::size_t row_end,
                      int parent);

  void BuildObjectBvh();
  int BuildObjectNode(int begin, int end);
  void RefitObjectBvh();
  void RefitObjectNode(int node);

  void PrepareCamera(const Camera &camera);
  void BuildCones();
  void Traverse(int screen_node, int object_node);
  void TestLeafPair(int screen_node, int object_node);

  bool ConeIntersectsBox(int screen_node, const AABB &box) const;
  double MinForwardDepth(const AABB &box) const;
  float ForwardDepth(const Vec3f &point) const;

  void UpdatePixel(int pixel_index, const Object *object,
                   std::size_t slot, const HitRecord &record,
                   float forward_depth);
  void PropagateDepth(int node);

  std::vector<Object *> objects_;
  std::vector<Object *> built_pointers_;
  bool synced_{false};

  std::vector<int> object_slots_;
  std::vector<ObjectNode> object_nodes_;
  int object_root_{-1};
  std::uint64_t topology_generation_{0};
  bool last_sync_rebuilt_{false};

  std::size_t width_{0};
  std::size_t height_{0};
  std::size_t screen_width_{0};
  std::size_t screen_height_{0};
  std::vector<ScreenNode> screen_nodes_;
  int screen_root_{-1};
  // pixel index -> screen leaf node index
  std::vector<int> pixel_screen_node_;

  std::vector<Hit> hits_;
  std::vector<RegionCone> cones_;
  WorkStats stats_;

  // camera data captured once per Run()
  Vec3f eye_{};
  Vec3f world_tl_{};
  Vec3f span_h_{};
  Vec3f span_v_{};
  Vec3f cam_forward_{};
};

// True when all six coordinates are finite and min <= max.
bool KnownBounds(const AABB &box);

} // namespace beam

#endif // BEAM_TRAVERSAL_HPP_
