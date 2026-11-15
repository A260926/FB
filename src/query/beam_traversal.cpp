#include "beam_traversal.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace beam {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// Evaluate the plane n.p + c.
inline double EvalPlane(const std::array<double, 3> &n, double c,
                        const Vec3f &p) {
  return n[0] * static_cast<double>(p.x) +
         n[1] * static_cast<double>(p.y) +
         n[2] * static_cast<double>(p.z) + c;
}

// Conservative ray/AABB test used for single-pixel regions.  Unlike
// the shared IntersectsAABB it treats a component as parallel only
// when it is exactly zero, so a ray that is *almost* axis-parallel but
// enters a slab at a very large t is never wrongly rejected.
inline bool RayIntersectsBoxConservative(const Ray &ray, const AABB &box) {
  double tmin = -kInf;
  double tmax = kInf;
  const double o[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
  const double d[3] = {ray.dir.x, ray.dir.y, ray.dir.z};
  const double lo[3] = {box.min.x, box.min.y, box.min.z};
  const double hi[3] = {box.max.x, box.max.y, box.max.z};
  for (int i = 0; i < 3; ++i) {
    if (d[i] == 0.0) {
      if (o[i] < lo[i] || o[i] > hi[i]) return false;
      continue;
    }
    const double inv = 1.0 / d[i];
    double t1 = (lo[i] - o[i]) * inv;
    double t2 = (hi[i] - o[i]) * inv;
    if (t1 > t2) std::swap(t1, t2);
    tmin = std::max(tmin, t1);
    tmax = std::min(tmax, t2);
    if (tmin > tmax) return false;
  }
  return tmax > 0.0;
}

} // namespace

bool KnownBounds(const AABB &box) {
  return std::isfinite(box.min.x) && std::isfinite(box.min.y) &&
         std::isfinite(box.min.z) && std::isfinite(box.max.x) &&
         std::isfinite(box.max.y) && std::isfinite(box.max.z) &&
         box.min.x <= box.max.x && box.min.y <= box.max.y &&
         box.min.z <= box.max.z;
}

//---------------------------------------------------------------------
// Screen quadtree
//---------------------------------------------------------------------
void BeamTraversal::BuildScreen() {
  screen_nodes_.clear();
  screen_root_ = -1;
  if (width_ == 0 || height_ == 0) return;
  pixel_screen_node_.assign(width_ * height_, -1);
  screen_root_ = BuildScreenNode(0, 0, width_, height_, -1);
}

int BeamTraversal::BuildScreenNode(std::size_t col_begin,
                                   std::size_t row_begin,
                                   std::size_t col_end,
                                   std::size_t row_end,
                                   int parent) {
  const int idx = static_cast<int>(screen_nodes_.size());
  screen_nodes_.push_back(ScreenNode{});
  ScreenNode &node = screen_nodes_[idx];
  node.col_begin = col_begin;
  node.col_end = col_end;
  node.row_begin = row_begin;
  node.row_end = row_end;
  node.parent = parent;
  node.max_forward_depth = std::numeric_limits<float>::infinity();

  const std::size_t w = col_end - col_begin;
  const std::size_t h = row_end - row_begin;
  if (w <= 1 && h <= 1) {
    node.pixel_index = static_cast<int>(row_begin * width_ + col_begin);
    if (node.pixel_index >= 0 &&
        static_cast<std::size_t>(node.pixel_index) <
            pixel_screen_node_.size()) {
      pixel_screen_node_[static_cast<std::size_t>(node.pixel_index)] = idx;
    }
    return idx;
  }

  // Split into up to four quadrants; a 1-row / 1-column region gets
  // two children, and odd sizes leave a smaller final block.
  const std::size_t mid_c = col_begin + (w + 1) / 2;
  const std::size_t mid_r = row_begin + (h + 1) / 2;
  const bool split_c = w > 1;
  const bool split_r = h > 1;

  int child_idx = 0;
  for (int rpart = 0; rpart < (split_r ? 2 : 1); ++rpart) {
    for (int cpart = 0; cpart < (split_c ? 2 : 1); ++cpart) {
      const std::size_t c0 = cpart ? mid_c : col_begin;
      const std::size_t c1 = cpart ? col_end : mid_c;
      const std::size_t r0 = rpart ? mid_r : row_begin;
      const std::size_t r1 = rpart ? row_end : mid_r;
      if (c0 >= c1 || r0 >= r1) continue;
      const int child = BuildScreenNode(c0, r0, c1, r1, idx);
      if (child_idx < 4) {
        screen_nodes_[idx].children[child_idx++] = child;
      }
    }
  }
  screen_nodes_[idx].child_count = child_idx;
  return idx;
}

//---------------------------------------------------------------------
// Object BVH
//---------------------------------------------------------------------
int BeamTraversal::BuildObjectNode(int begin, int end) {
  const int idx = static_cast<int>(object_nodes_.size());
  object_nodes_.push_back(ObjectNode{});
  if (end - begin <= 1) {
    ObjectNode &node = object_nodes_[idx];
    node.begin = begin;
    node.end = end;
    node.left = node.right = -1;
    return idx;
  }

  AABB centroid_bounds;
  for (int i = begin; i < end; ++i) {
    centroid_bounds.Expand(objects_[static_cast<std::size_t>(
        object_slots_[static_cast<std::size_t>(i)])]->center);
  }
  const float ext[3] = {
      centroid_bounds.max.x - centroid_bounds.min.x,
      centroid_bounds.max.y - centroid_bounds.min.y,
      centroid_bounds.max.z - centroid_bounds.min.z};
  int axis = 0;
  if (ext[1] > ext[axis]) axis = 1;
  if (ext[2] > ext[axis]) axis = 2;
  const bool spread = std::isfinite(ext[axis]) && ext[axis] > 0.0f;

  const int mid = begin + (end - begin) / 2;
  if (spread) {
    auto cmp = [this, axis](int a, int b) {
      return objects_[static_cast<std::size_t>(a)]->center[axis] <
             objects_[static_cast<std::size_t>(b)]->center[axis];
    };
    std::nth_element(object_slots_.begin() + begin,
                     object_slots_.begin() + mid,
                     object_slots_.begin() + end, cmp);
  }

  const int left = BuildObjectNode(begin, mid);
  const int right = BuildObjectNode(mid, end);
  ObjectNode &node = object_nodes_[idx];
  node.begin = begin;
  node.end = end;
  node.left = left;
  node.right = right;
  return idx;
}

void BeamTraversal::BuildObjectBvh() {
  object_nodes_.clear();
  object_root_ = -1;
  if (objects_.empty()) {
    object_slots_.clear();
    ++topology_generation_;
    return;
  }
  object_slots_.resize(objects_.size());
  for (std::size_t i = 0; i < object_slots_.size(); ++i)
    object_slots_[i] = static_cast<int>(i);
  object_root_ = BuildObjectNode(0, static_cast<int>(objects_.size()));
  RefitObjectBvh();
  ++topology_generation_;
}

void BeamTraversal::RefitObjectBvh() {
  if (object_root_ >= 0) RefitObjectNode(object_root_);
}

void BeamTraversal::RefitObjectNode(int node_idx) {
  ObjectNode &node = object_nodes_[static_cast<std::size_t>(node_idx)];
  if (node.IsLeaf()) {
    AABB box;
    bool known = node.end > node.begin;
    for (int i = node.begin; i < node.end; ++i) {
      const AABB &ob = objects_[static_cast<std::size_t>(
          object_slots_[static_cast<std::size_t>(i)])]->bounds;
      if (!KnownBounds(ob)) {
        // an unknown box must never be culled, and no finite union
        // can stand in for it
        known = false;
        continue;
      }
      box.Expand(ob.min);
      box.Expand(ob.max);
    }
    node.bounds_known = known;
    node.bounds = known ? box : AABB{};
    return;
  }
  if (node.left >= 0) RefitObjectNode(node.left);
  if (node.right >= 0) RefitObjectNode(node.right);
  AABB box;
  bool known = true;
  for (int child : {node.left, node.right}) {
    if (child < 0) continue;
    const ObjectNode &cn = object_nodes_[static_cast<std::size_t>(child)];
    if (!cn.bounds_known) {
      known = false;
      continue;
    }
    box.Expand(cn.bounds.min);
    box.Expand(cn.bounds.max);
  }
  node.bounds_known = known;
  node.bounds = known ? box : AABB{};
}

bool BeamTraversal::SyncObjects(const std::vector<Object *> &objects) {
  const bool same = synced_ && built_pointers_ == objects;
  objects_ = objects;
  last_sync_rebuilt_ = !same;
  if (!same) {
    BuildObjectBvh();
  } else {
    RefitObjectBvh();
  }
  built_pointers_ = objects;
  synced_ = true;
  return last_sync_rebuilt_;
}

//---------------------------------------------------------------------
// Camera / cones
//---------------------------------------------------------------------
void BeamTraversal::PrepareCamera(const Camera &camera) {
  eye_ = camera.center();
  const auto corners = camera.CornersWorld();
  world_tl_ = corners[0];
  span_h_ = corners[1] - corners[0];
  span_v_ = corners[2] - corners[0];
  const Vec3f fwd = camera.Unproject(0, 0) - eye_;
  cam_forward_ = fwd.Unit();
}

Vec3f BeamTraversal::PixelWorld(std::size_t row, std::size_t col) const {
  // exactly the sampling rule of RayTracer::Trace
  const float u = width_ > 1
      ? static_cast<float>(col) / static_cast<float>(width_ - 1) : 0.0f;
  const float v = height_ > 1
      ? static_cast<float>(row) / static_cast<float>(height_ - 1) : 0.0f;
  return world_tl_ + span_h_ * u + span_v_ * v;
}

void BeamTraversal::BuildCones() {
  cones_.assign(screen_nodes_.size(), RegionCone{});

  // Plane through the eye and the world-space line a..b.  The normal
  // is normalize((a-eye) x (b-eye)); the plane passes through the
  // eye, so offset = -normal.eye.
  auto make_plane = [&](const Vec3f &a, const Vec3f &b, Plane &out) {
    std::array<double, 3> da{{static_cast<double>(a.x) - eye_.x,
                              static_cast<double>(a.y) - eye_.y,
                              static_cast<double>(a.z) - eye_.z}};
    std::array<double, 3> db{{static_cast<double>(b.x) - eye_.x,
                              static_cast<double>(b.y) - eye_.y,
                              static_cast<double>(b.z) - eye_.z}};
    std::array<double, 3> cr{{da[1] * db[2] - da[2] * db[1],
                              da[2] * db[0] - da[0] * db[2],
                              da[0] * db[1] - da[1] * db[0]}};
    const double len = std::sqrt(cr[0] * cr[0] + cr[1] * cr[1] +
                                 cr[2] * cr[2]);
    if (!(len > 1e-12) || !std::isfinite(len)) return false;
    out.normal = {{cr[0] / len, cr[1] / len, cr[2] / len}};
    out.offset = -(out.normal[0] * static_cast<double>(eye_.x) +
                   out.normal[1] * static_cast<double>(eye_.y) +
                   out.normal[2] * static_cast<double>(eye_.z));
    return true;
  };

  for (std::size_t i = 0; i < screen_nodes_.size(); ++i) {
    const ScreenNode &node = screen_nodes_[i];
    RegionCone &cone = cones_[i];
    const bool one_col = node.col_end - node.col_begin <= 1;
    const bool one_row = node.row_end - node.row_begin <= 1;

    // Real sample points of the region's extreme pixels (bit-for-bit
    // the float positions the rays use).
    const Vec3f tl = PixelWorld(node.row_begin, node.col_begin);
    const Vec3f tr = PixelWorld(node.row_begin, node.col_end - 1);
    const Vec3f bl = PixelWorld(node.row_end - 1, node.col_begin);
    const Vec3f br = PixelWorld(node.row_end - 1, node.col_end - 1);

    // Full rectangles are bounded by four planes through the eye and
    // the rectangle edges.  A 1-row / 1-column region has a single
    // degenerate edge, handled by the thin plane below.
    Plane p;
    int n = 0;
    if (!one_col) {
      if (make_plane(tl, bl, p)) cone.boundary[n++] = p; // left
      if (make_plane(tr, br, p)) cone.boundary[n++] = p; // right
    }
    if (!one_row) {
      if (make_plane(tl, tr, p)) cone.boundary[n++] = p; // top
      if (make_plane(bl, br, p)) cone.boundary[n++] = p; // bottom
    }
    cone.boundary_valid = {{n > 0, n > 1, n > 2, n > 3}};

    // Orient the boundaries towards the region centre.
    const Vec3f centre = (tl + tr + bl + br) * 0.25f;
    for (int k = 0; k < 4; ++k) {
      if (!cone.boundary_valid[k]) continue;
      if (EvalPlane(cone.boundary[k].normal, cone.boundary[k].offset,
                    centre) < 0.0) {
        for (double &v : cone.boundary[k].normal) v = -v;
        cone.boundary[k].offset = -cone.boundary[k].offset;
      }
    }

    // A one-row region is a horizontal segment (tl==bl, tr==br); a
    // one-column region is a vertical segment (tl==tr, bl==br).  Its
    // rays all live in one plane through the eye and that segment.
    // (A single pixel spans no segment; it falls back to the exact
    // ray/box test.)
    if (one_row && !one_col) {
      if (make_plane(tl, tr, p)) {
        cone.thin = p;
        cone.thin_valid = true;
      }
    } else if (one_col && !one_row) {
      if (make_plane(tl, bl, p)) {
        cone.thin = p;
        cone.thin_valid = true;
      }
    }
  }
}

bool BeamTraversal::ConeIntersectsBox(int screen_node,
                                      const AABB &box) const {
  const ScreenNode &sn =
      screen_nodes_[static_cast<std::size_t>(screen_node)];
  const RegionCone &cone =
      cones_[static_cast<std::size_t>(screen_node)];

  // Relative tolerance covering the float rounding difference
  // between the float sample points and the planes built from them.
  // It may only make us keep more boxes.
  double scale = std::max(1.0, static_cast<double>(eye_.Norm()));
  for (int k = 0; k < 8; ++k) {
    const Vec3f c{(k & 1) ? box.max.x : box.min.x,
                  (k & 2) ? box.max.y : box.min.y,
                  (k & 4) ? box.max.z : box.min.z};
    scale = std::max(scale, static_cast<double>(std::abs(c.x)));
    scale = std::max(scale, static_cast<double>(std::abs(c.y)));
    scale = std::max(scale, static_cast<double>(std::abs(c.z)));
  }
  const double tol = 1e-5 * scale;

  auto plane_range = [&](const Plane &p, double &mn, double &mx) {
    mn = kInf;
    mx = -kInf;
    for (int k = 0; k < 8; ++k) {
      const Vec3f c{(k & 1) ? box.max.x : box.min.x,
                    (k & 2) ? box.max.y : box.min.y,
                    (k & 4) ? box.max.z : box.min.z};
      const double s = EvalPlane(p.normal, p.offset, c);
      mn = std::min(mn, s);
      mx = std::max(mx, s);
    }
  };

  double mn, mx;
  for (std::size_t k = 0; k < 4; ++k) {
    if (!cone.boundary_valid[k]) continue;
    plane_range(cone.boundary[k], mn, mx);
    if (mx < -tol) return false; // entirely outside one boundary
  }
  if (cone.thin_valid) {
    plane_range(cone.thin, mn, mx);
    if (mn > tol || mx < -tol) return false; // strictly off the plane
  }

  // A single-pixel region has no boundary plane; its cone is the ray
  // itself, so the exact conservative ray/box slab test applies.
  const bool degenerate =
      !cone.boundary_valid[0] && !cone.thin_valid;
  if (degenerate) {
    Ray ray(eye_, PixelWorld(sn.row_begin, sn.col_begin));
    if (!RayIntersectsBoxConservative(ray, box)) return false;
  }
  // Boxes containing the eye pass every plane test and are kept, as
  // they must be.
  return true;
}

double BeamTraversal::MinForwardDepth(const AABB &box) const {
  if (!KnownBounds(box)) return -kInf;
  double m = kInf;
  for (int k = 0; k < 8; ++k) {
    const Vec3f corner{
        (k & 1) ? box.max.x : box.min.x,
        (k & 2) ? box.max.y : box.min.y,
        (k & 4) ? box.max.z : box.min.z};
    m = std::min(m, static_cast<double>((corner - eye_).Dot(cam_forward_)));
  }
  return m;
}

float BeamTraversal::ForwardDepth(const Vec3f &point) const {
  return (point - eye_).Dot(cam_forward_);
}

//---------------------------------------------------------------------
// Depth reduction
//---------------------------------------------------------------------
void BeamTraversal::PropagateDepth(int node) {
  // Reduce the updated pixel depth up the screen ancestors.  A value
  // that did not change cannot affect any ancestor, so stop early.
  while (node >= 0) {
    ScreenNode &n = screen_nodes_[static_cast<std::size_t>(node)];
    float m = -std::numeric_limits<float>::infinity();
    for (int ci = 0; ci < n.child_count; ++ci) {
      const int child = n.children[ci];
      if (child < 0) continue;
      m = std::max(m, screen_nodes_[static_cast<std::size_t>(child)]
                          .max_forward_depth);
    }
    if (m == n.max_forward_depth) break;
    n.max_forward_depth = m;
    node = n.parent;
  }
}

void BeamTraversal::UpdatePixel(int pixel_index, const Object *object,
                                std::size_t slot,
                                const HitRecord &record,
                                float forward_depth) {
  Hit &hit = hits_[static_cast<std::size_t>(pixel_index)];
  hit.object = object;
  hit.slot = slot;
  hit.record = record;
  hit.forward_depth = forward_depth;

  const int leaf = pixel_screen_node_[static_cast<std::size_t>(pixel_index)];
  if (leaf < 0) return;
  ScreenNode &node = screen_nodes_[static_cast<std::size_t>(leaf)];
  if (node.max_forward_depth == forward_depth) return;
  node.max_forward_depth = forward_depth;
  PropagateDepth(node.parent);
}

//---------------------------------------------------------------------
// Traversal
//---------------------------------------------------------------------
void BeamTraversal::TestLeafPair(int screen_node, int object_node) {
  const ScreenNode &sn = screen_nodes_[static_cast<std::size_t>(screen_node)];
  const ObjectNode &on = object_nodes_[static_cast<std::size_t>(object_node)];
  const int pixel = sn.pixel_index;
  if (pixel < 0) return;
  const std::size_t row = static_cast<std::size_t>(pixel) / width_;
  const std::size_t col = static_cast<std::size_t>(pixel) % width_;
  Ray ray(eye_, PixelWorld(row, col));

  for (int i = on.begin; i < on.end; ++i) {
    const int slot = object_slots_[static_cast<std::size_t>(i)];
    const Object *obj = objects_[static_cast<std::size_t>(slot)];
    ++stats_.object_tests;
    const HitRecord hit = obj->Intersects(ray);
    if (!hit.is_hit || !(hit.t > 0.0f) || !std::isfinite(hit.t)) continue;
    const Hit &cur = hits_[static_cast<std::size_t>(pixel)];
    // nearest finite positive t; on an exact tie the smaller original
    // AddObject slot wins
    if (!cur.IsHit() || hit.t < cur.record.t ||
        (hit.t == cur.record.t &&
         static_cast<std::size_t>(slot) < cur.slot)) {
      UpdatePixel(pixel, obj, static_cast<std::size_t>(slot), hit,
                  ForwardDepth(hit.where));
    }
  }
}

void BeamTraversal::Traverse(int screen_node, int object_node) {
  if (screen_node < 0 || object_node < 0) return;
  const ScreenNode &sn = screen_nodes_[static_cast<std::size_t>(screen_node)];
  const ObjectNode &on =
      object_nodes_[static_cast<std::size_t>(object_node)];

  // (1) conservative cone / box test
  if (on.bounds_known) {
    ++stats_.beam_box_tests;
    if (!ConeIntersectsBox(screen_node, on.bounds)) return;
  }

  // (2) depth pruning: only when the box is strictly behind every
  // nearest hit currently known in the region.  Equality must still
  // reach the objects so the slot tie-break can be resolved, hence the
  // small tolerance below (it can only make us keep more pairs).
  if (on.bounds_known && std::isfinite(sn.max_forward_depth)) {
    const double region_max = static_cast<double>(sn.max_forward_depth);
    const double box_min = MinForwardDepth(on.bounds);
    const double tol = 1e-6 * std::max(1.0, std::abs(region_max));
    if (box_min > region_max + tol) {
      ++stats_.depth_rejects;
      return;
    }
  }

  if (sn.IsLeaf() && on.IsLeaf()) {
    TestLeafPair(screen_node, object_node);
    return;
  }

  // Split the object hierarchy first.  A leaf of the object tree is a
  // single AddObject slot; descending to it with the whole region and
  // only then splitting the screen lets the first (nearest) occluder
  // establish hits for all pixels of the region.  Everything behind
  // it is then rejected at a coarse screen node in one depth test per
  // object subtree instead of once per pixel.
  if (!on.IsLeaf()) {
    const double left_min = on.left >= 0
        ? MinForwardDepth(
              object_nodes_[static_cast<std::size_t>(on.left)].bounds)
        : kInf;
    const double right_min = on.right >= 0
        ? MinForwardDepth(
              object_nodes_[static_cast<std::size_t>(on.right)].bounds)
        : kInf;
    if (left_min <= right_min) {
      Traverse(screen_node, on.left);
      Traverse(screen_node, on.right);
    } else {
      Traverse(screen_node, on.right);
      Traverse(screen_node, on.left);
    }
    return;
  }

  // Single object for this region: refine the screen so its cone gets
  // tighter, until the object is tested once per real pixel.
  for (int ci = 0; ci < sn.child_count; ++ci) {
    const int child = sn.children[ci];
    if (child >= 0) Traverse(child, object_node);
  }
}

void BeamTraversal::ResetFrame() {
  hits_.assign(width_ * height_, Hit{});
  stats_ = WorkStats{};
  for (ScreenNode &node : screen_nodes_) {
    node.max_forward_depth = std::numeric_limits<float>::infinity();
  }
}

void BeamTraversal::Run(const Camera &camera) {
  width_ = camera.width();
  height_ = camera.height();
  if (width_ != screen_width_ || height_ != screen_height_) {
    screen_width_ = width_;
    screen_height_ = height_;
    BuildScreen();
  }
  ResetFrame();
  if (width_ == 0 || height_ == 0 || screen_root_ < 0) return;
  PrepareCamera(camera);
  BuildCones();
  if (object_root_ >= 0) Traverse(screen_root_, object_root_);
}

} // namespace beam
