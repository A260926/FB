//
// Implementation of analytic rectangular-light visibility.
//
// See analytic_visibility.h for the geometric and caching conventions.
//
#include "analytic_visibility.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "nanort.h"

namespace area_visibility {

namespace {

const double kEps = 1e-12;

// ---------------------------------------------------------------------------
// Small double-precision vector helpers.
// ---------------------------------------------------------------------------

Vec3 vadd(const Vec3 &a, const Vec3 &b) {
  return Vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}
Vec3 vsub(const Vec3 &a, const Vec3 &b) {
  return Vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}
Vec3 vscale(const Vec3 &a, double s) {
  return Vec3(a.x * s, a.y * s, a.z * s);
}
double vdot(const Vec3 &a, const Vec3 &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 vcross(const Vec3 &a, const Vec3 &b) {
  return Vec3(a.y * b.z - a.z * b.y,
              a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x);
}

// A convex polygon of (s, t) coordinates on the light plane.
typedef std::vector<Vec3> Polygon;

// ---------------------------------------------------------------------------
// Convex polygon clipping in the (s, t) plane (Sutherland-Hodgman).
// Each half-plane is  a*s + b*t + c >= 0 ; (a, b) is normalized to unit
// length so the tolerance is measured in (s, t) units.
// ---------------------------------------------------------------------------

struct HalfPlane {
  double a, b, c;
  HalfPlane(double aa, double bb, double cc) : a(aa), b(bb), c(cc) {
    const double len = std::sqrt(a * a + b * b);
    if (len > 0.0) {
      a /= len;
      b /= len;
      c /= len;
    }
  }
  double eval(const Vec3 &p) const { return a * p.x + b * p.y + c; }
};

Polygon ClipConvex(const Polygon &poly, const std::vector<HalfPlane> &planes) {
  Polygon in = poly;
  for (size_t h = 0; h < planes.size(); ++h) {
    Polygon out;
    if (in.empty()) break;
    for (size_t i = 0; i < in.size(); ++i) {
      const Vec3 &cur = in[i];
      const Vec3 &prev = in[(i + in.size() - 1) % in.size()];
      const double dcur = planes[h].eval(cur);
      const double dprev = planes[h].eval(prev);
      const bool cur_in = dcur >= -kEps;
      const bool prev_in = dprev >= -kEps;
      if (cur_in) {
        if (!prev_in) {
          const double r = dprev / (dprev - dcur);
          out.push_back(Vec3(prev.x + (cur.x - prev.x) * r,
                             prev.y + (cur.y - prev.y) * r, 0.0));
        }
        out.push_back(cur);
      } else if (prev_in) {
        const double r = dprev / (dprev - dcur);
        out.push_back(Vec3(prev.x + (cur.x - prev.x) * r,
                           prev.y + (cur.y - prev.y) * r, 0.0));
      }
    }
    in.swap(out);
  }
  return in;
}

// Clip a convex polygon to the rectangle 0 <= s,t <= 1.
Polygon ClipToRectangle(Polygon poly) {
  std::vector<HalfPlane> planes;
  planes.push_back(HalfPlane(1.0, 0.0, 0.0));    // s >= 0
  planes.push_back(HalfPlane(-1.0, 0.0, 1.0));   // s <= 1
  planes.push_back(HalfPlane(0.0, 1.0, 0.0));    // t >= 0
  planes.push_back(HalfPlane(0.0, -1.0, 1.0));   // t <= 1
  return ClipConvex(poly, planes);
}

// ---------------------------------------------------------------------------
// 3D convex polygon clipping against two planes parallel to the light.
// Keeps points with  w_near <= dot(X - R, N) <= w_far.
// ---------------------------------------------------------------------------

std::vector<Vec3> ClipDepth(std::vector<Vec3> poly, const Vec3 &r,
                            const Vec3 &n, double w_near, double w_far) {
  // Each pass keeps points whose signed value relative to a threshold is
  // >= 0, so the standard r = vprev / (vprev - vcur) intersection applies.
  for (int pass = 0; pass < 2; ++pass) {
    const bool near_pass = (pass == 0);
    std::vector<Vec3> out;
    if (poly.empty()) break;
    for (size_t i = 0; i < poly.size(); ++i) {
      const Vec3 &cur = poly[i];
      const Vec3 &prev = poly[(i + poly.size() - 1) % poly.size()];
      const double dcur = vdot(vsub(cur, r), n);
      const double dprev = vdot(vsub(prev, r), n);
      // Signed distance to the threshold; interior is value >= 0.
      const double vcur = near_pass ? (dcur - w_near) : (w_far - dcur);
      const double vprev = near_pass ? (dprev - w_near) : (w_far - dprev);
      const bool cur_in = vcur >= 0.0;
      const bool prev_in = vprev >= 0.0;
      if (cur_in) {
        if (!prev_in) {
          const double rr = vprev / (vprev - vcur);
          out.push_back(vadd(prev, vscale(vsub(cur, prev), rr)));
        }
        out.push_back(cur);
      } else if (prev_in) {
        const double rr = vprev / (vprev - vcur);
        out.push_back(vadd(prev, vscale(vsub(cur, prev), rr)));
      }
    }
    poly.swap(out);
  }
  return poly;
}

// ---------------------------------------------------------------------------
// Frustum: receiver point + rectangle define a finite pyramid. The four
// side planes pass through the receiver and a light edge; the two depth
// planes are parallel to the light through the receiver and the light.
// Conservative AABB tests use the p-vertex (n-vertex) criterion.
// ---------------------------------------------------------------------------

struct Plane {
  Vec3 n;
  double d;  // interior: dot(n, X) + d >= 0
};

struct Frustum {
  Vec3 receiver;
  Vec3 light_origin;
  Vec3 eu, ev, n;       // n oriented so the light is on the positive side
  double light_dist;
  double near_dist;
  std::vector<Plane> planes;
};

Plane MakePlane(const Vec3 &normal, const Vec3 &point) {
  Plane pl;
  pl.n = normal;
  pl.d = -vdot(normal, point);
  return pl;
}

Frustum MakeFrustum(const Vec3 &receiver, const Rectangle &light) {
  Frustum f;
  f.receiver = receiver;
  f.light_origin = light.origin;
  f.eu = light.edge_u;
  f.ev = light.edge_v;

  f.n = vcross(f.eu, f.ev);
  double ldist = vdot(vsub(light.origin, receiver), f.n);
  if (ldist < 0.0) {
    f.n = vscale(f.n, -1.0);
    ldist = -ldist;
  }
  f.light_dist = ldist;
  // Open receiver->light segment. A tiny positive near threshold keeps the
  // clipping intersection in front of the receiver (finite projection)
  // while excluding the receiver itself; its projected sliver lies far
  // outside the rectangle in ordinary scenes.
  f.near_dist = kEps * std::max(1.0, ldist);

  const Vec3 c =
      vadd(light.origin, vscale(vadd(f.eu, f.ev), 0.5));  // rectangle center

  // Four side planes, each through the receiver and a light edge.
  const Vec3 corners[4] = {
      light.origin, vadd(light.origin, f.eu),
      vadd(vadd(light.origin, f.eu), f.ev),
      vadd(light.origin, f.ev)};
  const Vec3 edges[4] = {f.eu, f.ev, vscale(f.eu, -1.0),
                         vscale(f.ev, -1.0)};
  for (int i = 0; i < 4; ++i) {
    Vec3 normal = vcross(edges[i], vsub(receiver, corners[i]));
    // Orient so the rectangle center is on the interior side.
    if (vdot(normal, c) - vdot(normal, corners[i]) < 0.0) {
      normal = vscale(normal, -1.0);
    }
    f.planes.push_back(MakePlane(normal, receiver));
  }
  // Near depth plane (through the receiver, parallel to the light).
  f.planes.push_back(MakePlane(f.n, receiver));
  // Far depth plane (through the light, normal flipped to face receiver).
  f.planes.push_back(MakePlane(vscale(f.n, -1.0), light.origin));
  return f;
}

// Conservative frustum vs AABB test: the box can intersect only if its
// p-vertex is inside (or on) every frustum plane.
bool BoxInFrustum(const Frustum &f, const double bmin[3],
                  const double bmax[3]) {
  for (size_t k = 0; k < f.planes.size(); ++k) {
    const Vec3 &n = f.planes[k].n;
    Vec3 p(n.x >= 0.0 ? bmax[0] : bmin[0],
           n.y >= 0.0 ? bmax[1] : bmin[1],
           n.z >= 0.0 ? bmax[2] : bmin[2]);
    if (vdot(n, p) + f.planes[k].d < -kEps) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Project one triangle onto the light plane and clip it to the rectangle.
// Returns an empty polygon when the open receiver->light segment range is
// never crossed.
// ---------------------------------------------------------------------------

Polygon ProjectTriangle(const Frustum &f, const Vec3 &p0, const Vec3 &p1,
                        const Vec3 &p2) {
  std::vector<Vec3> clipped;
  clipped.push_back(p0);
  clipped.push_back(p1);
  clipped.push_back(p2);
  clipped = ClipDepth(clipped, f.receiver, f.n, f.near_dist, f.light_dist);
  if (clipped.empty()) return Polygon();

  const double eu2 = vdot(f.eu, f.eu);
  const double ev2 = vdot(f.ev, f.ev);

  Polygon st;
  for (size_t i = 0; i < clipped.size(); ++i) {
    const Vec3 &x = clipped[i];
    const double w = vdot(vsub(x, f.receiver), f.n);
    if (w <= 0.0) return Polygon();  // defensive: depth clip guarantees w > 0
    const Vec3 q =
        vadd(f.receiver, vscale(vsub(x, f.receiver), f.light_dist / w));
    const Vec3 rel = vsub(q, f.light_origin);
    st.push_back(Vec3(vdot(rel, f.eu) / eu2, vdot(rel, f.ev) / ev2, 0.0));
  }
  return ClipToRectangle(st);
}

// ---------------------------------------------------------------------------
// Exact union area of convex polygons in the (s, t) plane.
//
// Vertical decomposition at every polygon vertex s-coordinate: inside each
// slab the lower/upper coverage boundaries are linear, so the union area is
// the merged vertical coverage interval at the slab midpoint times the slab
// width. This is adaptive (vertex-driven, not a fixed grid), merges
// overlapping/duplicate polygons and is independent of triangulation.
// ---------------------------------------------------------------------------

double PolygonArea(const Polygon &poly) {
  double a = 0.0;
  for (size_t i = 0; i < poly.size(); ++i) {
    const Vec3 &p = poly[i];
    const Vec3 &q = poly[(i + 1) % poly.size()];
    a += p.x * q.y - q.x * p.y;
  }
  return 0.5 * std::fabs(a);
}

void VerticalInterval(const Polygon &poly, double x, double *lo,
                      double *hi) {
  std::vector<double> ts;
  for (size_t i = 0; i < poly.size(); ++i) {
    const Vec3 &p = poly[i];
    const Vec3 &q = poly[(i + 1) % poly.size()];
    const double xmin = std::min(p.x, q.x);
    const double xmax = std::max(p.x, q.x);
    if (x < xmin - kEps || x > xmax + kEps) continue;
    if (std::fabs(q.x - p.x) <= kEps) {
      // Vertical edge: its t range is picked up by the adjacent crossing
      // edges; x is a slab midpoint so this only happens for a zero-width
      // slab, which the caller skips.
      continue;
    }
    const double r = (x - p.x) / (q.x - p.x);
    ts.push_back(p.y + (q.y - p.y) * r);
  }
  if (ts.empty()) {
    *lo = 1.0;
    *hi = 0.0;
    return;
  }
  *lo = *std::min_element(ts.begin(), ts.end());
  *hi = *std::max_element(ts.begin(), ts.end());
}

double UnionArea(const std::vector<Polygon> &polys) {
  std::vector<double> xs;
  for (size_t i = 0; i < polys.size(); ++i) {
    for (size_t k = 0; k < polys[i].size(); ++k) xs.push_back(polys[i][k].x);
  }
  std::sort(xs.begin(), xs.end());
  xs.erase(std::unique(xs.begin(), xs.end(),
                       [](double a, double b) {
                         return std::fabs(a - b) <= kEps;
                       }),
           xs.end());

  double area = 0.0;
  for (size_t i = 0; i + 1 < xs.size(); ++i) {
    const double x0 = xs[i];
    const double x1 = xs[i + 1];
    if (x1 - x0 <= kEps) continue;
    const double xm = 0.5 * (x0 + x1);

    std::vector<std::pair<double, double> > intervals;
    for (size_t k = 0; k < polys.size(); ++k) {
      double lo, hi;
      VerticalInterval(polys[k], xm, &lo, &hi);
      if (hi > lo) intervals.push_back(std::make_pair(lo, hi));
    }
    if (intervals.empty()) continue;
    std::sort(intervals.begin(), intervals.end());

    double covered = 0.0;
    double cur_lo = intervals[0].first;
    double cur_hi = intervals[0].second;
    for (size_t k = 1; k < intervals.size(); ++k) {
      if (intervals[k].first <= cur_hi + kEps) {
        cur_hi = std::max(cur_hi, intervals[k].second);
      } else {
        covered += cur_hi - cur_lo;
        cur_lo = intervals[k].first;
        cur_hi = intervals[k].second;
      }
    }
    covered += cur_hi - cur_lo;
    area += covered * (x1 - x0);
  }
  return area;
}

// ---------------------------------------------------------------------------
// BVH traversal shared by full and incremental queries.
//
// When allowed is non-null only primitive ids in that sorted list enter
// projection; node tests still reflect every bounding box traversed.
// ---------------------------------------------------------------------------

struct QueryCounters {
  std::size_t node_tests;
  std::size_t triangle_tests;
  QueryCounters() : node_tests(0), triangle_tests(0) {}
};

bool IdAllowed(const std::vector<unsigned int> *allowed, unsigned int id) {
  if (!allowed) return true;
  return std::binary_search(allowed->begin(), allowed->end(), id);
}

void TraversePolygons(const nanort::BVHAccel<double> &bvh,
                      const std::vector<double> &vertices,
                      const std::vector<unsigned int> &faces,
                      const Frustum &frustum,
                      const std::vector<unsigned int> *allowed,
                      std::vector<CachedPolygon> *out, QueryCounters *cnt) {
  const std::vector<nanort::BVHNode<double> > &nodes = bvh.GetNodes();
  const std::vector<unsigned int> &indices = bvh.GetIndices();

  std::vector<unsigned int> stack;
  stack.push_back(0);
  while (!stack.empty()) {
    const unsigned int ni = stack.back();
    stack.pop_back();
    const nanort::BVHNode<double> &node = nodes[ni];
    ++cnt->node_tests;

    if (!BoxInFrustum(frustum, node.bmin, node.bmax)) continue;

    if (node.flag == 0) {
      stack.push_back(node.data[1]);
      stack.push_back(node.data[0]);
      continue;
    }

    const unsigned int count = node.data[0];
    const unsigned int offset = node.data[1];
    for (unsigned int k = 0; k < count; ++k) {
      const unsigned int id = indices[offset + k];
      if (!IdAllowed(allowed, id)) continue;
      ++cnt->triangle_tests;

      const unsigned int i0 = faces[3 * id + 0];
      const unsigned int i1 = faces[3 * id + 1];
      const unsigned int i2 = faces[3 * id + 2];
      const Vec3 p0(vertices[3 * i0 + 0], vertices[3 * i0 + 1],
                    vertices[3 * i0 + 2]);
      const Vec3 p1(vertices[3 * i1 + 0], vertices[3 * i1 + 1],
                    vertices[3 * i1 + 2]);
      const Vec3 p2(vertices[3 * i2 + 0], vertices[3 * i2 + 1],
                    vertices[3 * i2 + 2]);

      Polygon poly = ProjectTriangle(frustum, p0, p1, p2);
      if (!poly.empty() && PolygonArea(poly) > 0.0) {
        CachedPolygon np;
        np.id = id;
        np.polygon = poly;
        out->push_back(np);
      }
    }
  }
}

VisibilityResult Finalize(const std::vector<CachedPolygon> &named,
                          const QueryCounters &cnt) {
  std::vector<Polygon> polys;
  polys.reserve(named.size());
  for (size_t i = 0; i < named.size(); ++i) polys.push_back(named[i].polygon);
  double blocked = UnionArea(polys);
  double visible = 1.0 - blocked;
  if (visible < 0.0) visible = 0.0;
  if (visible > 1.0) visible = 1.0;
  return VisibilityResult(visible, cnt.node_tests, cnt.triangle_tests);
}

bool SameVec(const Vec3 &a, const Vec3 &b) {
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

}  // namespace

// ---------------------------------------------------------------------------
// Query entry points. The cache type is defined in the header and owns all
// of its projected polygons by value.
// ---------------------------------------------------------------------------

VisibilityResult query(const nanort::BVHAccel<double> &bvh,
                       const std::vector<double> &vertices,
                       const std::vector<unsigned int> &faces,
                       const Vec3 &receiver, const Rectangle &light) {
  if (faces.empty() || vertices.empty() || !bvh.IsValid()) {
    return VisibilityResult(1.0, 0, 0);
  }

  const Frustum frustum = MakeFrustum(receiver, light);
  QueryCounters cnt;
  std::vector<CachedPolygon> named;
  TraversePolygons(bvh, vertices, faces, frustum, 0, &named, &cnt);
  return Finalize(named, cnt);
}

VisibilityResult query(const nanort::BVHAccel<double> &bvh,
                       const std::vector<double> &vertices,
                       const std::vector<unsigned int> &faces,
                       const Vec3 &receiver, const Rectangle &light,
                       VisibilityCache &cache) {
  if (faces.empty() || vertices.empty() || !bvh.IsValid()) {
    return VisibilityResult(1.0, 0, 0);
  }

  const unsigned long long gen = bvh.GetBuildGeneration();
  const unsigned long long rev = bvh.GetRefitRevision();
  const bool same_view =
      cache.valid_ && SameVec(cache.receiver_, receiver) &&
      SameVec(cache.light_.origin, light.origin) &&
      SameVec(cache.light_.edge_u, light.edge_u) &&
      SameVec(cache.light_.edge_v, light.edge_v);

  // Exact repeat: reuse without any bounding box or triangle work.
  if (cache.valid_ && cache.bvh_ == &bvh && cache.generation_ == gen &&
      cache.revision_ == rev && same_view) {
    return VisibilityResult(cache.visible_, 0, 0);
  }

  const bool single_refit =
      cache.valid_ && cache.bvh_ == &bvh && cache.generation_ == gen &&
      cache.revision_ + 1 == rev && same_view;

  if (single_refit) {
    // Only the primitives named by the one missed refit re-enter
    // clip/projection; every other stored polygon is reused.
    std::vector<unsigned int> changed = bvh.GetLastRefitPrimitives();
    std::sort(changed.begin(), changed.end());

    std::vector<CachedPolygon> kept;
    kept.reserve(cache.polys_.size());
    for (size_t i = 0; i < cache.polys_.size(); ++i) {
      if (!std::binary_search(changed.begin(), changed.end(),
                              cache.polys_[i].id)) {
        kept.push_back(cache.polys_[i]);
      }
    }

    const Frustum frustum = MakeFrustum(receiver, light);
    QueryCounters cnt;
    TraversePolygons(bvh, vertices, faces, frustum, &changed, &kept, &cnt);

    const VisibilityResult result = Finalize(kept, cnt);
    cache.polys_.swap(kept);
    cache.visible_ = result.visible;
    cache.node_tests_ = result.node_tests;
    cache.triangle_tests_ = result.triangle_tests;
    cache.revision_ = rev;
    return result;
  }

  // Full invalidation: skipped refits, another BVH, new generation or a
  // changed receiver/light.
  const Frustum frustum = MakeFrustum(receiver, light);
  QueryCounters cnt;
  std::vector<CachedPolygon> named;
  TraversePolygons(bvh, vertices, faces, frustum, 0, &named, &cnt);
  const VisibilityResult result = Finalize(named, cnt);

  cache.bvh_ = &bvh;
  cache.generation_ = gen;
  cache.revision_ = rev;
  cache.receiver_ = receiver;
  cache.light_ = light;
  cache.polys_.swap(named);
  cache.visible_ = result.visible;
  cache.node_tests_ = result.node_tests;
  cache.triangle_tests_ = result.triangle_tests;
  cache.valid_ = true;
  return result;
}

}  // namespace area_visibility
