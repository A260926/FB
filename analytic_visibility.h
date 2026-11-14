//
// Analytic rectangular-light visibility for NanoRT BVH scenes.
//
// The fraction of a rectangle light's area that is visible from a
// receiver point is computed exactly (up to floating point error):
// every double-sided, opaque triangle is projected from the receiver
// onto the light plane, clipped to the open receiver->light segment
// range and to the rectangle, and the union of the resulting convex
// polygons is measured. No rasterization, fixed resolution or random
// shadow rays are used.
//
// Geometric conventions (see analytic_visibility.cpp):
//   - The rectangle is origin + s*edge_u + t*edge_v, 0 <= s,t <= 1,
//     with orthogonal, non-zero edges.
//   - A triangle blocks a light point only when the open segment from
//     the receiver to that light point intersects the triangle.
//     Geometry behind the light plane or past the receiver cannot cast
//     back into the frustum.
//   - Repeated, reversed, edge-sharing or overlapping triangles, and any
//     re-triangulation of the same surface, contribute one union only.
//   - Coordinates are finite, the receiver is neither on the light
//     plane nor on any occluder triangle plane, and triangles are
//     non-degenerate. For ordinary-scale, non-pathological inputs the
//     absolute error of `visible` is at most 1e-7 and the value is in
//     [0, 1].
//
// Cache conventions:
//   - VisibilityCache is default-constructible and owns all of its
//     data (projected polygons are held by value).
//   - Geometry changes are published only through successful Build() /
//     Refit() calls on the same BVH, which must outlive the cache.
//   - Identical (BVH, versions, receiver, light) queries reuse the
//     stored result with both test counters at 0.
//   - A single Refit revision forward re-projects only the primitives
//     listed by BVHAccel::GetLastRefitPrimitives(); projections of
//     unchanged primitives are reused, including removals of faces that
//     left the frustum and insertions of faces that entered it.
//   - Skipping two or more refits, switching BVH, a Build generation
//     change, or any receiver/light component change invalidates the
//     cache completely.
//   - Two independent caches may be queried interleaved; each tracks its
//     own versions.
//   - Counters are reset per query: node_tests counts BVH bounding boxes
//     actually tested against the frustum, triangle_tests counts faces
//     actually entering clip/projection. Reused projections count as
//     neither.
//
#ifndef ANALYTIC_VISIBILITY_H_
#define ANALYTIC_VISIBILITY_H_

#include <cstddef>
#include <vector>

namespace nanort {
template <typename T>
class BVHAccel;
}

namespace area_visibility {

struct Vec3 {
  double x, y, z;
  Vec3() : x(0.0), y(0.0), z(0.0) {}
  Vec3(double xx, double yy, double zz) : x(xx), y(yy), z(zz) {}
};

struct Rectangle {
  Vec3 origin;
  Vec3 edge_u;
  Vec3 edge_v;
  Rectangle() {}
  Rectangle(const Vec3 &o, const Vec3 &u, const Vec3 &v)
      : origin(o), edge_u(u), edge_v(v) {}
};

struct VisibilityResult {
  double visible;       // visible fraction of the rectangle, in [0, 1]
  std::size_t node_tests;
  std::size_t triangle_tests;
  VisibilityResult()
      : visible(1.0), node_tests(0), triangle_tests(0) {}
  VisibilityResult(double v, std::size_t n, std::size_t t)
      : visible(v), node_tests(n), triangle_tests(t) {}
};

// One stored projection: original primitive id plus its clipped convex
// polygon in the light's (s, t) coordinates. Held by value inside the
// cache; the cache owns no references to caller temporaries.
struct CachedPolygon {
  unsigned int id;
  std::vector<Vec3> polygon;
};

class VisibilityCache {
 public:
  VisibilityCache()
      : bvh_(0),
        generation_(0),
        revision_(0),
        visible_(1.0),
        node_tests_(0),
        triangle_tests_(0),
        valid_(false) {}

 private:
  friend VisibilityResult query(const nanort::BVHAccel<double> &bvh,
                                const std::vector<double> &vertices,
                                const std::vector<unsigned int> &faces,
                                const Vec3 &receiver, const Rectangle &light,
                                VisibilityCache &cache);

  const nanort::BVHAccel<double> *bvh_;
  unsigned long long generation_;
  unsigned long long revision_;
  Vec3 receiver_;
  Rectangle light_;
  std::vector<CachedPolygon> polys_;
  double visible_;
  std::size_t node_tests_;
  std::size_t triangle_tests_;
  bool valid_;
};

// One-shot query. An empty occluder mesh returns visible = 1.
VisibilityResult query(const nanort::BVHAccel<double> &bvh,
                       const std::vector<double> &vertices,
                       const std::vector<unsigned int> &faces,
                       const Vec3 &receiver, const Rectangle &light);

// Cached query with the same parameters and return value.
VisibilityResult query(const nanort::BVHAccel<double> &bvh,
                       const std::vector<double> &vertices,
                       const std::vector<unsigned int> &faces,
                       const Vec3 &receiver, const Rectangle &light,
                       VisibilityCache &cache);

}  // namespace area_visibility

#endif  // ANALYTIC_VISIBILITY_H_
