// Windowless C++14 geometry regression for the Poly2Tri CDT -> interior mesh path.
//
// The checks below are deliberately independent of Poly2Tri's own predicates:
// all orientation / incircle / point-in-polygon / segment work is implemented
// here in long double, from the original input rings and a Point identity table.
//
// The same driver is compiled against the normal sources and against three
// mutated source copies by devcheck/check-cdt.sh.

#include <poly2tri/poly2tri.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using p2t::Point;
using p2t::Triangle;

// ---------------------------------------------------------------------------
// Basic geometry.  Everything is long double; only storage coordinates are
// double (the library API is double).
// ---------------------------------------------------------------------------

struct V2 {
  long double x = 0.0L;
  long double y = 0.0L;
};

static V2 V(double x, double y)
{
  return V2{static_cast<long double>(x), static_cast<long double>(y)};
}

static V2 V(const Point* p)
{
  return V(p->x, p->y);
}

static long double ldabs(long double v)
{
  return v < 0.0L ? -v : v;
}

static long double orient(const V2& a, const V2& b, const V2& c)
{
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static long double dot(const V2& a, const V2& b)
{
  return a.x * b.x + a.y * b.y;
}

static long double dist2(const V2& a, const V2& b)
{
  const long double dx = a.x - b.x;
  const long double dy = a.y - b.y;
  return dx * dx + dy * dy;
}

static long double ring_area_abs(const std::vector<V2>& ring)
{
  long double s = 0.0L;
  const size_t n = ring.size();
  for (size_t i = 0; i < n; ++i) {
    const V2& a = ring[i];
    const V2& b = ring[(i + 1) % n];
    s += a.x * b.y - a.y * b.x;
  }
  return (s < 0.0L ? -s : s) * 0.5L;
}

// Per-variant tolerances.  Each quantity gets the dimension appropriate to it:
// length, area / squared length, and fourth-order incircle determinant.
struct Tol {
  long double len;      // 1e-12 * L
  long double len2;     // 1e-12 * L^2  (orientation, area floor)
  long double len4;     // 1e-12 * L^4  (incircle determinant)
};

static Tol make_tol(long double L)
{
  const long double eps = 1.0e-12L;
  return Tol{eps * L, eps * L * L, eps * L * L * L * L};
}

// Distance from p to segment ab (long double), using the cross product.
static long double point_seg_distance(const V2& p, const V2& a, const V2& b)
{
  const long double l2 = dist2(a, b);
  if (l2 <= 0.0L) {
    return std::sqrt(dist2(p, a));
  }
  const long double c = orient(a, b, p);
  return (c < 0.0L ? -c : c) / std::sqrt(l2);
}

static bool point_on_segment(const V2& p, const V2& a, const V2& b, const Tol& tol)
{
  if (point_seg_distance(p, a, b) > tol.len)
    return false;
  const long double l2 = dist2(a, b);
  if (l2 <= 0.0L)
    return dist2(p, a) <= tol.len * tol.len;
  const long double t = dot(V2{p.x - a.x, p.y - a.y}, V2{b.x - a.x, b.y - a.y});
  return t >= -tol.len * std::sqrt(l2) && t <= l2 + tol.len * std::sqrt(l2);
}

static bool point_strictly_on_segment(const V2& p, const V2& a, const V2& b, const Tol& tol)
{
  if (!point_on_segment(p, a, b, tol))
    return false;
  const long double l2 = dist2(a, b);
  if (l2 <= 0.0L)
    return false;
  const long double t = dot(V2{p.x - a.x, p.y - a.y}, V2{b.x - a.x, b.y - a.y});
  return t > tol.len * std::sqrt(l2) && t < l2 - tol.len * std::sqrt(l2);
}

static bool same_segment(const V2& a, const V2& b, const V2& c, const V2& d)
{
  return (dist2(a, c) == 0.0L && dist2(b, d) == 0.0L) ||
         (dist2(a, d) == 0.0L && dist2(b, c) == 0.0L);
}

// True when the *interiors* of the two segments cross or overlap, or when one
// endpoint lies strictly inside the other segment.  A shared endpoint is not a
// conflict.
static bool segments_conflict(const V2& a, const V2& b, const V2& c, const V2& d, const Tol& tol)
{
  if (same_segment(a, b, c, d))
    return false;

  const long double o1 = orient(a, b, c);
  const long double o2 = orient(a, b, d);
  const long double o3 = orient(c, d, a);
  const long double o4 = orient(c, d, b);
  const long double t2 = tol.len2;

  const bool c_left = o1 > t2;
  const bool c_right = o1 < -t2;
  const bool d_left = o2 > t2;
  const bool d_right = o2 < -t2;
  const bool a_left = o3 > t2;
  const bool a_right = o3 < -t2;
  const bool b_left = o4 > t2;
  const bool b_right = o4 < -t2;

  if (((c_left && d_right) || (c_right && d_left)) &&
      ((a_left && b_right) || (a_right && b_left))) {
    return true;
  }

  // Collinear overlap.  Project to the dominant axis and test interval overlap.
  if ((ldabs(o1) <= t2) && (ldabs(o2) <= t2) &&
      (ldabs(o3) <= t2) && (ldabs(o4) <= t2)) {
    const long double dx = b.x - a.x;
    const long double dy = b.y - a.y;
    auto proj = [dx, dy](const V2& p) {
      return (ldabs(dx) >= ldabs(dy)) ? p.x : p.y;
    };
    long double lo1 = std::min(proj(a), proj(b));
    long double hi1 = std::max(proj(a), proj(b));
    long double lo2 = std::min(proj(c), proj(d));
    long double hi2 = std::max(proj(c), proj(d));
    const long double lo = std::max(lo1, lo2);
    const long double hi = std::min(hi1, hi2);
    if (hi - lo > tol.len)
      return true;
    // Touching at one shared endpoint is fine.
    return false;
  }

  if (point_strictly_on_segment(c, a, b, tol) || point_strictly_on_segment(d, a, b, tol) ||
      point_strictly_on_segment(a, c, d, tol) || point_strictly_on_segment(b, c, d, tol)) {
    return true;
  }
  return false;
}

enum class Loc { Outside, Inside, Boundary };

static Loc locate_in_ring(const V2& p, const std::vector<V2>& ring, const Tol& tol)
{
  const size_t n = ring.size();
  if (n < 3)
    return Loc::Outside;

  // Boundary first (strict point-in-polygon must not count boundary points).
  for (size_t i = 0; i < n; ++i) {
    if (point_on_segment(p, ring[i], ring[(i + 1) % n], tol))
      return Loc::Boundary;
  }

  bool inside = false;
  for (size_t i = 0; i < n; ++i) {
    const V2& a = ring[i];
    const V2& b = ring[(i + 1) % n];
    if ((a.y > p.y) != (b.y > p.y)) {
      const long double xint = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
      if (p.x < xint)
        inside = !inside;
    }
  }
  return inside ? Loc::Inside : Loc::Outside;
}

// Strictly inside a CCW triangle (points on an edge do not count).
static bool point_strict_in_tri(const V2& p, const V2& a, const V2& b, const V2& c, const Tol& tol)
{
  const long double o0 = orient(a, b, p);
  const long double o1 = orient(b, c, p);
  const long double o2 = orient(c, a, p);
  return o0 > tol.len2 && o1 > tol.len2 && o2 > tol.len2;
}

// Incircle determinant for CCW triangle (a,b,c); positive means d is strictly
// inside the circumcircle.  This is the test's own predicate, not Poly2Tri's.
static long double incircle_det(const V2& a, const V2& b, const V2& c, const V2& d)
{
  const long double adx = a.x - d.x, ady = a.y - d.y;
  const long double bdx = b.x - d.x, bdy = b.y - d.y;
  const long double cdx = c.x - d.x, cdy = c.y - d.y;
  const long double alift = adx * adx + ady * ady;
  const long double blift = bdx * bdx + bdy * bdy;
  const long double clift = cdx * cdx + cdy * cdy;
  const long double bdxcdy = bdx * cdy;
  const long double cdxbdy = cdx * bdy;
  const long double cdxady = cdx * ady;
  const long double adxcdy = adx * cdy;
  const long double adxbdy = adx * bdy;
  const long double bdxady = bdx * ady;
  return alift * (bdxcdy - cdxbdy) + blift * (cdxady - adxcdy) + clift * (adxbdy - bdxady);
}

// ---------------------------------------------------------------------------
// Input specification.
// ---------------------------------------------------------------------------

struct VecD {
  double x;
  double y;
};

struct ShapeSpec {
  std::string name;
  std::vector<VecD> outer;
  std::vector<std::vector<VecD>> holes;
  std::vector<VecD> steiner;
};

static ShapeSpec shape_concave_two_holes()
{
  ShapeSpec s;
  s.name = "concave-two-holes";
  s.outer = {{0, 0}, {16, 0}, {16, 12}, {9, 12}, {9, 6}, {7, 6}, {7, 12}, {0, 12}};
  // Holes are clockwise (opposite to the CCW outer ring).
  s.holes.push_back({{2, 2}, {2, 5}, {5, 5}, {5, 2}});
  s.holes.push_back({{10, 2}, {10, 8}, {14, 8}, {14, 2}});
  s.steiner = {{8, 3}, {11, 10}, {1.5, 10.5}, {6, 8.5}};
  return s;
}

static ShapeSpec shape_thin_corner()
{
  ShapeSpec s;
  s.name = "thin-corner";
  // U-shaped outline: a 2-unit-wide bottom bar with two arms.  The hole sits
  // 0.4 units below the horizontal re-entrant edge at y=2 and 0.5 units from
  // the outer right edge, leaving thin slivers right at the inner corner.
  s.outer = {{0, 0}, {14, 0}, {14, 10}, {12, 10}, {12, 2}, {2, 2}, {2, 10}, {0, 10}};
  s.holes.push_back({{10.5, 0.4}, {10.5, 1.6}, {13.5, 1.6}, {13.5, 0.4}});
  s.steiner = {{12, 0.2}, {12, 1.8}, {11, 1.8}, {1, 5}, {13, 5}, {13.8, 1.0}};
  return s;
}

static ShapeSpec shape_scatter_diagonals()
{
  ShapeSpec s;
  s.name = "scatter-diagonals";
  s.outer = {{0, 0}, {18, 0}, {18, 10}, {10, 10}, {10, 4}, {8, 4}, {8, 10}, {0, 10}};
  s.steiner = {{3, 3}, {7, 3}, {12, 3}, {15, 3}, {4, 7}, {14, 7}, {6, 8.5}, {16, 8}, {9, 2.5}};
  return s;
}

static ShapeSpec shape_cocircular_rectangle()
{
  ShapeSpec s;
  s.name = "cocircular-rectangle";
  // The four corners of an ordinary rectangle are cocircular, so both
  // diagonals are legal Delaunay choices.
  s.outer = {{0, 0}, {12, 0}, {12, 8}, {0, 8}};
  return s;
}

// ---------------------------------------------------------------------------
// Variants: rotate the ring start, reverse windings, reverse Steiner order,
// and apply uniform scale + translation.  Every variant gets fresh Point
// storage with an empty edge_list.
// ---------------------------------------------------------------------------

struct Variant {
  std::string name;
  std::size_t outer_shift;
  std::size_t hole_shift;
  bool reverse_outer;
  bool reverse_holes;
  bool reverse_steiner;
  double scale;
  double tx;
  double ty;
};

static std::vector<Variant> variants_for(const ShapeSpec& s)
{
  std::vector<Variant> v;
  v.push_back(Variant{"base", 0, 0, false, false, false, 1.0, 0.0, 0.0});
  v.push_back(Variant{"rotate-start", s.outer.size() / 3, 1, false, false, false, 1.0, 0.0, 0.0});
  v.push_back(Variant{"reverse-both", 0, 0, true, true, false, 1.0, 0.0, 0.0});
  v.push_back(Variant{"reverse-outer", 0, 0, true, false, false, 1.0, 0.0, 0.0});
  if (!s.holes.empty())
    v.push_back(Variant{"reverse-holes", 0, 0, false, true, false, 1.0, 0.0, 0.0});
  if (!s.steiner.empty())
    v.push_back(Variant{"rotate-reverse-steiner", s.outer.size() / 2, 2, true, true, true, 1.0, 0.0, 0.0});
  v.push_back(Variant{"scale-1/8+translate", 0, 0, false, false, false, 0.125, 3.25, 5.5});
  v.push_back(Variant{"scale-1+translate", 0, 0, false, false, false, 1.0, -7.5, 11.25});
  v.push_back(Variant{"scale-16+translate", 0, 0, false, false, false, 16.0, 12.5, -9.75});
  v.push_back(Variant{"combined-reverse-rotate-scale16", s.outer.size() / 4, 3, true, true, !s.steiner.empty(),
                    16.0, -30.5, 42.25});
  return v;
}

struct FinalInput {
  std::vector<V2> outer;
  std::vector<std::vector<V2>> holes;
  std::vector<V2> steiner;
};

static std::vector<V2> transform_ring(const std::vector<VecD>& ring, const Variant& v, bool reverse,
                                      std::size_t shift)
{
  std::vector<V2> out;
  out.reserve(ring.size());
  for (const VecD& p : ring) {
    out.push_back(V(static_cast<double>(v.scale) * p.x + v.tx,
                   static_cast<double>(v.scale) * p.y + v.ty));
  }
  if (reverse)
    std::reverse(out.begin(), out.end());
  if (!out.empty() && shift % out.size() != 0)
    std::rotate(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(shift % out.size()), out.end());
  return out;
}

static FinalInput build_final_input(const ShapeSpec& s, const Variant& v)
{
  FinalInput f;
  f.outer = transform_ring(s.outer, v, v.reverse_outer, v.outer_shift);
  for (std::size_t h = 0; h < s.holes.size(); ++h)
    f.holes.push_back(transform_ring(s.holes[h], v, v.reverse_holes, v.hole_shift + h));
  f.steiner = transform_ring(s.steiner, v, v.reverse_steiner, 0);
  return f;
}

// ---------------------------------------------------------------------------
// Input validation, independent of the library.
// ---------------------------------------------------------------------------

struct RingProblem {
  bool ok = true;
  std::string why;
};

static void ring_simple(const std::vector<V2>& ring, const Tol& tol, RingProblem& rp)
{
  const std::size_t n = ring.size();
  if (n < 3) {
    rp.ok = false;
    rp.why = "ring has fewer than 3 points";
    return;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (dist2(ring[i], ring[(i + 1) % n]) <= tol.len * tol.len) {
      rp.ok = false;
      rp.why = "zero-length ring edge";
      return;
    }
  }
  for (std::size_t i = 0; i < n; ++i) {
    const V2& a = ring[i];
    const V2& b = ring[(i + 1) % n];
    for (std::size_t j = i + 1; j < n; ++j) {
      if (j == i + 1 || (i == 0 && j == n - 1))
        continue; // adjacent edges share an endpoint by construction
      const V2& c = ring[j];
      const V2& d = ring[(j + 1) % n];
      if (segments_conflict(a, b, c, d, tol)) {
        rp.ok = false;
        rp.why = "self-intersecting ring";
        return;
      }
    }
  }
}

static bool validate_input(const FinalInput& f, const Tol& tol, std::string& why)
{
  RingProblem rp;
  ring_simple(f.outer, tol, rp);
  if (!rp.ok) {
    why = "outer: " + rp.why;
    return false;
  }
  for (std::size_t h = 0; h < f.holes.size(); ++h) {
    rp = RingProblem();
    ring_simple(f.holes[h], tol, rp);
    if (!rp.ok) {
      why = "hole " + std::to_string(h) + ": " + rp.why;
      return false;
    }
  }

  // Every hole vertex strictly inside the outer ring; no crossing of outer.
  for (std::size_t h = 0; h < f.holes.size(); ++h) {
    for (const V2& p : f.holes[h]) {
      if (locate_in_ring(p, f.outer, tol) != Loc::Inside) {
        why = "hole vertex is not strictly inside outer ring";
        return false;
      }
    }
    for (std::size_t i = 0; i < f.holes[h].size(); ++i) {
      const V2& a = f.holes[h][i];
      const V2& b = f.holes[h][(i + 1) % f.holes[h].size()];
      for (std::size_t k = 0; k < f.outer.size(); ++k) {
        const V2& c = f.outer[k];
        const V2& d = f.outer[(k + 1) % f.outer.size()];
        if (segments_conflict(a, b, c, d, tol)) {
          why = "hole edge crosses outer ring";
          return false;
        }
      }
    }
  }

  // Holes mutually disjoint (and strictly separated).
  for (std::size_t h1 = 0; h1 < f.holes.size(); ++h1) {
    for (std::size_t h2 = h1 + 1; h2 < f.holes.size(); ++h2) {
      for (const V2& p : f.holes[h1]) {
        if (locate_in_ring(p, f.holes[h2], tol) != Loc::Outside) {
          why = "hole vertices are inside/touching another hole";
          return false;
        }
      }
      for (std::size_t i = 0; i < f.holes[h1].size(); ++i) {
        const V2& a = f.holes[h1][i];
        const V2& b = f.holes[h1][(i + 1) % f.holes[h1].size()];
        for (std::size_t j = 0; j < f.holes[h2].size(); ++j) {
          const V2& c = f.holes[h2][j];
          const V2& d = f.holes[h2][(j + 1) % f.holes[h2].size()];
          if (segments_conflict(a, b, c, d, tol)) {
            why = "holes intersect";
            return false;
          }
        }
      }
    }
  }

  // Steiner points: strictly inside fill area, not on any input edge.
  for (const V2& p : f.steiner) {
    if (locate_in_ring(p, f.outer, tol) != Loc::Inside) {
      why = "Steiner point is not strictly inside outer ring";
      return false;
    }
    for (const auto& hole : f.holes) {
      if (locate_in_ring(p, hole, tol) != Loc::Outside) {
        why = "Steiner point is in/touching a hole";
        return false;
      }
    }
    for (std::size_t i = 0; i < f.outer.size(); ++i) {
      if (point_on_segment(p, f.outer[i], f.outer[(i + 1) % f.outer.size()], tol)) {
        why = "Steiner point lies on an outer edge";
        return false;
      }
    }
    for (const auto& hole : f.holes) {
      for (std::size_t i = 0; i < hole.size(); ++i) {
        if (point_on_segment(p, hole[i], hole[(i + 1) % hole.size()], tol)) {
          why = "Steiner point lies on a hole edge";
          return false;
        }
      }
    }
  }

  // All coordinates distinct (no duplicate input points).
  std::vector<V2> all = f.outer;
  for (const auto& h : f.holes)
    all.insert(all.end(), h.begin(), h.end());
  all.insert(all.end(), f.steiner.begin(), f.steiner.end());
  for (std::size_t i = 0; i < all.size(); ++i) {
    for (std::size_t j = i + 1; j < all.size(); ++j) {
      if (dist2(all[i], all[j]) <= tol.len * tol.len) {
        why = "duplicate input coordinates";
        return false;
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Input identity table and per-point drawing attributes.
// ---------------------------------------------------------------------------

enum class Kind { Outer = 0, Hole = 1, Steiner = 2 };

struct InputPoint {
  Point* ptr = nullptr;
  int id = -1;
  Kind kind = Kind::Outer;
  int ring = -1;
  int index = -1;
  V2 original;
};

struct Attr {
  int id = -1;
  double u = 0.0;
  double v = 0.0;
  std::uint32_t color = 0;
};

static Attr make_attr(int id, int n)
{
  Attr a;
  a.id = id;
  a.u = static_cast<double>(id + 1) / static_cast<double>(n + 1);
  a.v = 1.0 - a.u;
  a.color = 0xFF000000u | (static_cast<std::uint32_t>(id) & 0x00FFFFFFu);
  return a;
}

// ---------------------------------------------------------------------------
// Failure accounting / reporting.
// ---------------------------------------------------------------------------

struct Failures {
  int point_coverage = 0;
  int hole_coverage = 0;
  int coverage = 0;
  int topology = 0;
  int delaunay = 0;
  int identity = 0;
  std::map<std::string, int> printed;
};

static void report(Failures& f, const std::string& category, const std::string& variant,
                   const std::string& detail)
{
  if (category == "point-coverage")
    ++f.point_coverage;
  else if (category == "hole-coverage")
    ++f.hole_coverage;
  else if (category == "coverage")
    ++f.coverage;
  else if (category == "topology")
    ++f.topology;
  else if (category == "delaunay")
    ++f.delaunay;
  else if (category == "identity")
    ++f.identity;

  const int n = f.printed[category];
  if (n < 12) {
    std::cout << "ASSERT_FAIL " << category << " variant=" << variant << " detail=" << detail << "\n";
  } else if (n == 12) {
    std::cout << "ASSERT_FAIL " << category << " variant=" << variant
              << " detail=(further failures suppressed)\n";
  }
  f.printed[category] = n + 1;
}

// ---------------------------------------------------------------------------
// Per-triangulation mesh checks.
// ---------------------------------------------------------------------------

struct RingEdge {
  int id1 = -1;
  int id2 = -1;
  int ring = -1;
  bool hole = false;
};

struct Occurrence {
  Triangle* t = nullptr;
  int edge = -1; // edge index, opposite GetPoint(edge)
  int opp_id = -1;
  int a_id = -1;
  int b_id = -1;
};

using EdgeKey = std::pair<int, int>;

static EdgeKey key_of(int a, int b)
{
  return a < b ? EdgeKey{a, b} : EdgeKey{b, a};
}

static std::string ldstr(long double v, int prec = 18)
{
  std::ostringstream s;
  s << std::setprecision(prec) << static_cast<double>(v);
  return s.str();
}

static void render_svg(const ShapeSpec& shape, const FinalInput& in,
                       const std::vector<Triangle*>& triangles, const std::string& dir);

static void check_variant(const ShapeSpec& shape, const Variant& variant, const FinalInput& in,
                          const Tol& tol, const std::string& out_dir, Failures& failures)
{
  const std::string vname = shape.name + "/" + variant.name;

  // Fresh Point storage for every variant.  The storage (and all p2t::Point
  // objects) outlives the CDT; it is not shared with any other variant.
  std::vector<std::unique_ptr<Point>> store;
  std::vector<InputPoint> inputs;
  std::vector<Point*> outer_ptrs;
  std::vector<std::vector<Point*>> hole_ptrs(in.holes.size());
  std::vector<Point*> steiner_ptrs;

  store.reserve(in.outer.size() + in.steiner.size() +
                [&in]() { std::size_t n = 0; for (const auto& h : in.holes) n += h.size(); return n; }());
  int next_id = 0;
  auto add_point = [&](const V2& p, Kind kind, int ring, int index) -> Point* {
    std::unique_ptr<Point> owned(new Point(static_cast<double>(p.x), static_cast<double>(p.y)));
    Point* raw = owned.get();
    store.push_back(std::move(owned));
    if (!raw->edge_list.empty()) {
      report(failures, "identity", vname,
             "fresh Point already has a non-empty edge_list (reused storage)");
    }
    inputs.push_back(InputPoint{raw, next_id++, kind, ring, index, p});
    return raw;
  };

  for (std::size_t i = 0; i < in.outer.size(); ++i)
    outer_ptrs.push_back(add_point(in.outer[i], Kind::Outer, -1, static_cast<int>(i)));
  for (std::size_t h = 0; h < in.holes.size(); ++h)
    for (std::size_t i = 0; i < in.holes[h].size(); ++i)
      hole_ptrs[h].push_back(add_point(in.holes[h][i], Kind::Hole, static_cast<int>(h),
                                       static_cast<int>(i)));
  for (std::size_t i = 0; i < in.steiner.size(); ++i)
    steiner_ptrs.push_back(add_point(in.steiner[i], Kind::Steiner, -1, static_cast<int>(i)));

  std::unordered_map<const Point*, const InputPoint*> by_ptr;
  for (const InputPoint& ip : inputs)
    by_ptr[ip.ptr] = &ip;
  const int n_points = next_id;
  std::vector<Attr> attrs(n_points);
  for (int i = 0; i < n_points; ++i)
    attrs[i] = make_attr(i, n_points);

  // Map input ring edges to identity keys.
  std::vector<RingEdge> ring_edges;
  std::set<EdgeKey> boundary_keys;
  for (std::size_t i = 0; i < in.outer.size(); ++i) {
    const int id1 = by_ptr.at(outer_ptrs[i])->id;
    const int id2 = by_ptr.at(outer_ptrs[(i + 1) % outer_ptrs.size()])->id;
    ring_edges.push_back(RingEdge{id1, id2, -1, false});
    boundary_keys.insert(key_of(id1, id2));
  }
  for (std::size_t h = 0; h < in.holes.size(); ++h) {
    for (std::size_t i = 0; i < hole_ptrs[h].size(); ++i) {
      const int id1 = by_ptr.at(hole_ptrs[h][i])->id;
      const int id2 = by_ptr.at(hole_ptrs[h][(i + 1) % hole_ptrs[h].size()])->id;
      ring_edges.push_back(RingEdge{id1, id2, static_cast<int>(h), true});
      boundary_keys.insert(key_of(id1, id2));
    }
  }

  std::vector<Triangle*> triangles;
  bool triangulated = false;
  std::string runtime_error;

  // The CDT is destroyed before the Point storage (unique_ptr storage was
  // declared first and is still alive when cdt goes out of scope below).
  {
    p2t::CDT cdt(outer_ptrs);
    for (std::size_t h = 0; h < hole_ptrs.size(); ++h)
      cdt.AddHole(hole_ptrs[h]);
    for (Point* p : steiner_ptrs)
      cdt.AddPoint(p);
    try {
      cdt.Triangulate();
      triangles = cdt.GetTriangles();
      triangulated = true;
    } catch (const std::exception& e) {
      runtime_error = e.what();
    } catch (...) {
      runtime_error = "unknown exception";
    }

    if (!triangulated) {
      report(failures, "coverage", vname, "triangulation threw: " + runtime_error);
    } else {
      // Observation data is printed before assertions so mutants always show
      // what they actually produced.
      long double tri_area = 0.0L;
      std::set<int> used_ids;
      int constrained_count = 0;
      bool all_known = true;
      for (Triangle* t : triangles) {
        const V2 a = V(t->GetPoint(0)), b = V(t->GetPoint(1)), c = V(t->GetPoint(2));
        tri_area += orient(a, b, c) * 0.5L;
        for (int i = 0; i < 3; ++i) {
          Point* p = t->GetPoint(i);
          auto it = by_ptr.find(p);
          if (it == by_ptr.end()) {
            all_known = false;
          } else {
            used_ids.insert(it->second->id);
          }
          if (t->constrained_edge[i])
            ++constrained_count;
        }
      }
      const long double outer_area = ring_area_abs(in.outer);
      long double holes_area = 0.0L;
      for (const auto& h : in.holes)
        holes_area += ring_area_abs(h);

      std::cout << "OBS variant=" << vname
                << " triangles=" << triangles.size()
                << " area=" << ldstr(tri_area)
                << " outer_area=" << ldstr(outer_area)
                << " holes_area=" << ldstr(holes_area)
                << " used_points=" << used_ids.size() << "/" << n_points
                << " steiner_used=" << [&]() {
                     int n = 0;
                     for (const InputPoint& ip : inputs)
                       if (ip.kind == Kind::Steiner && used_ids.count(ip.id)) ++n;
                     return n;
                   }()
                << "/" << in.steiner.size()
                << " constrained_bits=" << constrained_count
                << " all_output_points_known=" << (all_known ? "yes" : "no")
                << "\n";

      if (!all_known) {
        report(failures, "point-coverage", vname,
               "output triangle references a pointer not present in the input table");
        report(failures, "identity", vname,
               "output triangle contains a non-input Point identity");
      }

      // ---- Point coverage -------------------------------------------------
      for (const InputPoint& ip : inputs) {
        if (used_ids.find(ip.id) == used_ids.end()) {
          std::string what = (ip.kind == Kind::Outer) ? "outer boundary"
                            : (ip.kind == Kind::Hole) ? "hole boundary"
                                                     : "Steiner";
          report(failures, "point-coverage", vname,
                 "input " + what + " point id=" + std::to_string(ip.id) + " is unused");
        }
      }

      // ---- Identity / drawing attributes ---------------------------------
      bool identity_ok = all_known;
      // Coordinates must be the very same values that were supplied.
      for (const InputPoint& ip : inputs) {
        if (static_cast<long double>(ip.ptr->x) != ip.original.x ||
            static_cast<long double>(ip.ptr->y) != ip.original.y) {
          identity_ok = false;
          report(failures, "identity", vname,
                 "input point id=" + std::to_string(ip.id) + " coordinates changed");
        }
      }

      // Reconstruct per-corner UV/colour arrays from Point identity.
      std::vector<Attr> per_corner;
      std::vector<int> per_corner_id;
      std::vector<V2> per_corner_pos;
      bool attrs_unique = true;
      {
        std::set<std::uint32_t> distinct_colors;
        for (int id = 0; id < n_points; ++id)
          distinct_colors.insert(attrs[id].color);
        attrs_unique = static_cast<int>(distinct_colors.size()) == n_points;
      }
      for (Triangle* t : triangles) {
        for (int i = 0; i < 3; ++i) {
          Point* p = t->GetPoint(i);
          auto it = by_ptr.find(p);
          if (it == by_ptr.end()) {
            identity_ok = false;
            continue;
          }
          const int id = it->second->id;
          const Attr& a = attrs[id];
          per_corner.push_back(a);
          per_corner_id.push_back(id);
          per_corner_pos.push_back(V(p));
        }
      }
      if (identity_ok) {
        for (std::size_t i = 0; i < per_corner.size(); ++i) {
          const int id = per_corner_id[i];
          if (per_corner[i].id != id || per_corner[i].u != attrs[id].u ||
              per_corner[i].v != attrs[id].v || per_corner[i].color != attrs[id].color) {
            identity_ok = false;
            report(failures, "identity", vname,
                   "per-corner attribute does not match Point identity id=" + std::to_string(id));
            break;
          }
        }

        // Simulate a renderer that sorts corners by coordinates.  The
        // attributes must still belong to the identity that owns the
        // coordinate, not to a coordinate-ordered slot.
        std::map<std::pair<long double, long double>, int> coord_to_id;
        bool coords_unique = true;
        for (const InputPoint& ip : inputs) {
          auto ins = coord_to_id.insert({{ip.original.x, ip.original.y}, ip.id});
          if (!ins.second) {
            coords_unique = false;
            report(failures, "identity", vname,
                   "two Point identities share the same coordinate id=" + std::to_string(ip.id));
          }
        }
        std::vector<std::size_t> order(per_corner.size());
        for (std::size_t i = 0; i < order.size(); ++i)
          order[i] = i;
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
          if (per_corner_pos[a].x != per_corner_pos[b].x)
            return per_corner_pos[a].x < per_corner_pos[b].x;
          return per_corner_pos[a].y < per_corner_pos[b].y;
        });
        for (std::size_t idx : order) {
          const int id = per_corner_id[idx];
          const V2& p = per_corner_pos[idx];
          auto it = coord_to_id.find({p.x, p.y});
          if (it == coord_to_id.end() || it->second != id) {
            identity_ok = false;
            report(failures, "identity", vname,
                   "coordinate-sorted corner lost its Point identity id=" + std::to_string(id));
            break;
          }
        }
        if (!coords_unique)
          identity_ok = false;
        if (!attrs_unique) {
          identity_ok = false;
          report(failures, "identity", vname, "duplicate UV/colour attribute");
        }
      }
      if (!identity_ok) {
        // Individual failures above carry the details.
      }

      // ---- Positive area ---------------------------------------------------
      bool orient_ok = true;
      for (std::size_t ti = 0; ti < triangles.size(); ++ti) {
        Triangle* t = triangles[ti];
        const V2 a = V(t->GetPoint(0)), b = V(t->GetPoint(1)), c = V(t->GetPoint(2));
        const long double twice = orient(a, b, c);
        if (twice <= tol.len2) {
          orient_ok = false;
          report(failures, "coverage", vname,
                 "triangle " + std::to_string(ti) + " has non-positive area twice=" +
                 ldstr(twice));
        }
      }

      // ---- Edge incidence / adjacency / constrained bits -------------------
      std::map<EdgeKey, std::vector<Occurrence>> edges;
      bool topology_ok = true;
      for (std::size_t ti = 0; ti < triangles.size(); ++ti) {
        Triangle* t = triangles[ti];
        for (int i = 0; i < 3; ++i) {
          Point* p1 = t->GetPoint((i + 1) % 3);
          Point* p2 = t->GetPoint((i + 2) % 3);
          auto i1 = by_ptr.find(p1);
          auto i2 = by_ptr.find(p2);
          if (i1 == by_ptr.end() || i2 == by_ptr.end())
            continue;
          // The edge index is the index of its opposite point.  Do not assume
          // GetPoint(i)->GetPoint(i+1) corresponds to constrained_edge[i].
          if (t->EdgeIndex(p1, p2) != i) {
            topology_ok = false;
            report(failures, "topology", vname,
                   "edge index/opposite-point convention mismatch in triangle " + std::to_string(ti));
          }
          Occurrence occ;
          occ.t = t;
          occ.edge = i;
          occ.a_id = i1->second->id;
          occ.b_id = i2->second->id;
          auto ip = by_ptr.find(t->GetPoint(i));
          if (ip == by_ptr.end())
            continue;
          occ.opp_id = ip->second->id;
          edges[key_of(occ.a_id, occ.b_id)].push_back(occ);
        }
      }

      // Every constrained bit in the output must correspond to an original
      // ring edge; every original ring edge must be one constrained output
      // edge.  This includes all hole edges (the "hole coverage" assertion).
      bool hole_edges_ok = true;
      for (Triangle* t : triangles) {
        for (int i = 0; i < 3; ++i) {
          if (!t->constrained_edge[i])
            continue;
          Point* p1 = t->GetPoint((i + 1) % 3);
          Point* p2 = t->GetPoint((i + 2) % 3);
          auto i1 = by_ptr.find(p1);
          auto i2 = by_ptr.find(p2);
          if (i1 == by_ptr.end() || i2 == by_ptr.end())
            continue;
          if (boundary_keys.find(key_of(i1->second->id, i2->second->id)) == boundary_keys.end()) {
            topology_ok = false;
            report(failures, "topology", vname,
                   "constrained_edge bit is set on a non-boundary edge");
          }
        }
      }

      if (!orient_ok)
        topology_ok = false;

      for (const auto& kv : edges) {
        const std::vector<Occurrence>& occ = kv.second;
        if (occ.size() > 2) {
          topology_ok = false;
          report(failures, "topology", vname,
                 "edge has " + std::to_string(occ.size()) + " output incidences");
          continue;
        }

        if (occ.size() == 1) {
          const Occurrence& o = occ.front();
          if (!o.t->constrained_edge[o.edge]) {
            // A lone incidence is only admissible for a constrained boundary
            // edge.  A non-constrained edge must have two interior triangles.
            topology_ok = false;
            report(failures, "topology", vname,
                   "non-constrained edge has only one output triangle");
          }
          continue;
        }

        // Two output incidences.
        const Occurrence& o1 = occ[0];
        const Occurrence& o2 = occ[1];
        if (o1.t->constrained_edge[o1.edge] || o2.t->constrained_edge[o2.edge]) {
          topology_ok = false;
          report(failures, "topology", vname,
                 "constrained edge is incident to two output triangles");
        }
        // Opposite direction: one a->b and one b->a.
        if (!((o1.a_id == o2.b_id && o1.b_id == o2.a_id))) {
          topology_ok = false;
          report(failures, "topology", vname,
                 "shared edge has the same direction in both triangles");
        }
        // Neighbour pointers must point at each other and at the same edge.
        Triangle* n1 = o1.t->GetNeighbor(o1.edge);
        Triangle* n2 = o2.t->GetNeighbor(o2.edge);
        if (n1 != o2.t || n2 != o1.t) {
          topology_ok = false;
          report(failures, "topology", vname,
                 "neighbour pointers across a shared edge do not point at each other");
        } else {
          Point* pa = o1.t->GetPoint((o1.edge + 1) % 3);
          Point* pb = o1.t->GetPoint((o1.edge + 2) % 3);
          if (n1->EdgeIndex(pa, pb) != o2.edge || n2->EdgeIndex(pa, pb) != o1.edge) {
            topology_ok = false;
            report(failures, "topology", vname,
                   "neighbour triangles are not attached to the same shared edge");
          }
        }
      }

      // Original ring edge coverage (exactly one constrained incidence).
      for (const RingEdge& re : ring_edges) {
        const auto it = edges.find(key_of(re.id1, re.id2));
        bool found = false;
        if (it != edges.end() && it->second.size() == 1) {
          const Occurrence& o = it->second.front();
          found = o.t->constrained_edge[o.edge];
        }
        if (!found) {
          if (re.hole) {
            hole_edges_ok = false;
            report(failures, "hole-coverage", vname,
                   "hole ring edge ids=(" + std::to_string(re.id1) + "," +
                   std::to_string(re.id2) + ") is not exactly one constrained output edge");
          } else {
            topology_ok = false;
            report(failures, "topology", vname,
                   "outer ring edge ids=(" + std::to_string(re.id1) + "," +
                   std::to_string(re.id2) + ") is not exactly one constrained output edge");
          }
        }
      }

      // ---- Area / coverage -------------------------------------------------
      long double mesh_area = 0.0L;
      for (Triangle* t : triangles) {
        const V2 a = V(t->GetPoint(0)), b = V(t->GetPoint(1)), c = V(t->GetPoint(2));
        mesh_area += orient(a, b, c) * 0.5L;
      }
      const long double expected_area = outer_area - holes_area;
      if (ldabs(mesh_area - expected_area) > tol.len2) {
        report(failures, "coverage", vname,
               "area mismatch mesh=" + ldstr(mesh_area) + " expected=" + ldstr(expected_area) +
               " tol=" + ldstr(tol.len2));
        if (!in.holes.empty())
          report(failures, "hole-coverage", vname,
                 "mesh area " + ldstr(mesh_area) + " differs from outer-minus-holes " +
                 ldstr(expected_area));
      }

      // Strict point-in-polygon samples: the centroid and three interior
      // barycentric samples of every triangle must be in the filled region.
      const long double bw[4][3] = {
          {1.0L / 3.0L, 1.0L / 3.0L, 1.0L / 3.0L},
          {0.6L, 0.2L, 0.2L},
          {0.2L, 0.6L, 0.2L},
          {0.2L, 0.2L, 0.6L},
      };
      bool locations_ok = true;
      for (std::size_t ti = 0; ti < triangles.size() && locations_ok; ++ti) {
        Triangle* t = triangles[ti];
        const V2 a = V(t->GetPoint(0)), b = V(t->GetPoint(1)), c = V(t->GetPoint(2));
        for (int s = 0; s < 4; ++s) {
          const V2 p{bw[s][0] * a.x + bw[s][1] * b.x + bw[s][2] * c.x,
                     bw[s][0] * a.y + bw[s][1] * b.y + bw[s][2] * c.y};
          if (locate_in_ring(p, in.outer, tol) != Loc::Inside) {
            locations_ok = false;
            report(failures, "coverage", vname,
                   "triangle " + std::to_string(ti) + " sample lies outside the outer ring");
            break;
          }
          for (std::size_t h = 0; h < in.holes.size(); ++h) {
            const Loc loc = locate_in_ring(p, in.holes[h], tol);
            if (loc == Loc::Inside) {
              locations_ok = false;
              report(failures, "hole-coverage", vname,
                     "triangle " + std::to_string(ti) + " sample lies inside hole " +
                     std::to_string(h));
              break;
            }
            if (loc == Loc::Boundary) {
              // A triangle interior sample exactly on a hole edge means the
              // mesh merely touches the hole from the wrong side.
              locations_ok = false;
              report(failures, "hole-coverage", vname,
                     "triangle " + std::to_string(ti) + " sample lies on hole " +
                     std::to_string(h) + " boundary");
              break;
            }
          }
          if (!locations_ok)
            break;
        }
      }

      // Original boundary segments must not be crossed by any triangle edge
      // (except the boundary edge itself, which is shared with the mesh).
      std::vector<V2> boundary_segments;
      auto add_ring_segments = [&](const std::vector<V2>& ring) {
        for (std::size_t i = 0; i < ring.size(); ++i) {
          boundary_segments.push_back(ring[i]);
          boundary_segments.push_back(ring[(i + 1) % ring.size()]);
        }
      };
      add_ring_segments(in.outer);
      for (const auto& h : in.holes)
        add_ring_segments(h);

      bool crossings_ok = true;
      for (std::size_t ti = 0; ti < triangles.size() && crossings_ok; ++ti) {
        Triangle* t = triangles[ti];
        for (int i = 0; i < 3; ++i) {
          const V2 a = V(t->GetPoint((i + 1) % 3)), b = V(t->GetPoint((i + 2) % 3));
          for (std::size_t k = 0; k < boundary_segments.size(); k += 2) {
            const V2& c = boundary_segments[k];
            const V2& d = boundary_segments[k + 1];
            if (same_segment(a, b, c, d))
              continue;
            if (segments_conflict(a, b, c, d, tol)) {
              crossings_ok = false;
              report(failures, "coverage", vname,
                     "triangle edge crosses an input boundary segment (triangle " +
                     std::to_string(ti) + ", segment " + std::to_string(k / 2) + ")");
              break;
            }
          }
          if (!crossings_ok)
            break;
        }
      }

      // Triangle-vs-triangle geometry: no edge crossings, no T-junctions, no
      // vertex of one triangle strictly inside another.
      bool triangles_ok = true;
      for (std::size_t i = 0; i < triangles.size() && triangles_ok; ++i) {
        Triangle* ta = triangles[i];
        const V2 a0 = V(ta->GetPoint(0)), a1 = V(ta->GetPoint(1)), a2 = V(ta->GetPoint(2));
        for (std::size_t j = i + 1; j < triangles.size() && triangles_ok; ++j) {
          Triangle* tb = triangles[j];
          const V2 b0 = V(tb->GetPoint(0)), b1 = V(tb->GetPoint(1)), b2 = V(tb->GetPoint(2));
          const V2 av[3] = {a0, a1, a2};
          const V2 bv[3] = {b0, b1, b2};
          for (int e1 = 0; e1 < 3 && triangles_ok; ++e1) {
            const V2& p = av[e1];
            const V2& q = av[(e1 + 1) % 3];
            for (int e2 = 0; e2 < 3; ++e2) {
              const V2& r = bv[e2];
              const V2& s = bv[(e2 + 1) % 3];
              if (same_segment(p, q, r, s))
                continue;
              if (segments_conflict(p, q, r, s, tol)) {
                triangles_ok = false;
                report(failures, "coverage", vname,
                       "triangle edges intersect (triangles " + std::to_string(i) + "," +
                       std::to_string(j) + ")");
                break;
              }
            }
          }
          if (!triangles_ok)
            break;
          for (int k = 0; k < 3; ++k) {
            if (point_strict_in_tri(av[k], b0, b1, b2, tol) ||
                point_strict_in_tri(bv[k], a0, a1, a2, tol)) {
              triangles_ok = false;
              report(failures, "coverage", vname,
                     "a triangle vertex lies strictly inside another triangle (" +
                     std::to_string(i) + "," + std::to_string(j) + ")");
              break;
            }
          }
        }
      }

      // ---- Local Delaunay condition ---------------------------------------
      bool delaunay_ok = true;
      int nonconstrained_shared = 0;
      int convex_quads = 0;
      long double max_incircle_det = -1e100L;
      for (const auto& kv : edges) {
        const std::vector<Occurrence>& occ = kv.second;
        if (occ.size() != 2)
          continue;
        const Occurrence& o1 = occ[0];
        const Occurrence& o2 = occ[1];
        if (o1.t->constrained_edge[o1.edge] || o2.t->constrained_edge[o2.edge])
          continue; // constrained edges are exempt
        ++nonconstrained_shared;

        const V2 a = V(o1.t->GetPoint((o1.edge + 1) % 3));
        const V2 b = V(o1.t->GetPoint((o1.edge + 2) % 3));
        const V2 r = V(o1.t->GetPoint(o1.edge)); // T1 opposite
        const V2 s = V(o2.t->GetPoint(o2.edge)); // T2 opposite

        if (orient(a, b, r) <= tol.len2) {
          // T1 should be CCW; if not, coverage already reported it.
          continue;
        }

        // The union is convex only when the derived boundary cycle
        // a -> s -> b -> r -> a turns strictly left at every corner.
        const long double c0 = orient(a, s, b);
        const long double c1 = orient(s, b, r);
        const long double c2 = orient(b, r, a);
        const long double c3 = orient(r, a, s);
        const bool convex = c0 > tol.len2 && c1 > tol.len2 && c2 > tol.len2 && c3 > tol.len2;
        if (!convex)
          continue;
        ++convex_quads;

        // T1's actual vertex order is CCW.  Positive determinant means the
        // opposite vertex s is strictly inside T1's circumcircle, which is a
        // local Delaunay violation.  Cocircular (det ~ 0) is acceptable.
        const V2 t0 = V(o1.t->GetPoint(0));
        const V2 t1 = V(o1.t->GetPoint(1));
        const V2 t2 = V(o1.t->GetPoint(2));
        const long double det = incircle_det(t0, t1, t2, s);
        if (det > max_incircle_det)
          max_incircle_det = det;
        if (det > tol.len4) {
          delaunay_ok = false;
          report(failures, "delaunay", vname,
                 "local Delaunay violation on edge ids=(" + std::to_string(o1.a_id) + "," +
                 std::to_string(o1.b_id) + ") det=" + ldstr(det) + " tol=" + ldstr(tol.len4));
        }
      }

      std::cout << "DELAUNAY variant=" << vname
                << " nonconstrained_shared_edges=" << nonconstrained_shared
                << " convex_quads=" << convex_quads
                << " max_incircle_det=" << ldstr(max_incircle_det)
                << " tol=" << ldstr(tol.len4) << "\n";

      if (!locations_ok || !crossings_ok || !triangles_ok)
        topology_ok = false; // geometric coverage failure also invalidates mesh

      // ---- SVG (base variant only) ---------------------------------------
      if (variant.name == "base") {
        render_svg(shape, in, triangles, out_dir);
      }

      // Category aggregation is done by the caller, using the individual
      // reports above.
      (void)topology_ok;
      (void)hole_edges_ok;
      (void)delaunay_ok;
    }
  } // CDT and its triangles are destroyed here; Point storage is still alive.

  // Touch the surviving Point storage after CDT destruction, proving the
  // points outlived the triangulation.
  for (const auto& owned : store) {
    volatile double x = owned->x;
    volatile double y = owned->y;
    (void)x;
    (void)y;
  }
}

// Render one SVG per base shape with boundaries and triangle edges.
static void render_svg(const ShapeSpec& shape, const FinalInput& in,
                       const std::vector<Triangle*>& triangles, const std::string& dir)
{
  long double minx = 1e100L, maxx = -1e100L, miny = 1e100L, maxy = -1e100L;
  auto extend = [&](const V2& p) {
    minx = std::min(minx, p.x);
    maxx = std::max(maxx, p.x);
    miny = std::min(miny, p.y);
    maxy = std::max(maxy, p.y);
  };
  for (const V2& p : in.outer)
    extend(p);
  for (const auto& h : in.holes)
    for (const V2& p : h)
      extend(p);

  const long double w = std::max<long double>(1.0L, maxx - minx);
  const long double h = std::max<long double>(1.0L, maxy - miny);
  const long double W = 720.0L, H = 520.0L, M = 24.0L;
  const long double sc = std::min((W - 2 * M) / w, (H - 2 * M) / h);
  auto sx = [&](long double x) { return M + (x - minx) * sc; };
  auto sy = [&](long double y) { return H - M - (y - miny) * sc; };

  std::ofstream out(dir + "/mesh-" + shape.name + ".svg");
  if (!out)
    return;
  out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << static_cast<int>(W)
      << "\" height=\"" << static_cast<int>(H) << "\" viewBox=\"0 0 " << static_cast<int>(W)
      << " " << static_cast<int>(H) << "\">\n"
      << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n"
      << "<g stroke=\"#b0b7c3\" stroke-width=\"0.7\" fill=\"none\">\n";
  for (Triangle* t : triangles) {
    for (int i = 0; i < 3; ++i) {
      const Point* p = t->GetPoint(i);
      const Point* q = t->GetPoint((i + 1) % 3);
      out << "<line x1=\"" << static_cast<double>(sx(static_cast<long double>(p->x)))
          << "\" y1=\"" << static_cast<double>(sy(static_cast<long double>(p->y)))
          << "\" x2=\"" << static_cast<double>(sx(static_cast<long double>(q->x)))
          << "\" y2=\"" << static_cast<double>(sy(static_cast<long double>(q->y))) << "\"/>\n";
    }
  }
  out << "</g>\n<g fill=\"none\" stroke=\"#1f4fa8\" stroke-width=\"2\">\n";
  auto poly = [&](const std::vector<V2>& ring) {
    out << "<polygon points=\"";
    for (const V2& p : ring)
      out << static_cast<double>(sx(p.x)) << "," << static_cast<double>(sy(p.y)) << " ";
    out << "\"/>\n";
  };
  poly(in.outer);
  for (const auto& hh : in.holes)
    poly(hh);
  out << "</g>\n<g fill=\"#c0392b\" r=\"2\">\n";
  for (const V2& p : in.steiner)
    out << "<circle cx=\"" << static_cast<double>(sx(p.x)) << "\" cy=\""
        << static_cast<double>(sy(p.y)) << "\" r=\"2\"/>\n";
  out << "</g>\n<title>Poly2Tri CDT mesh: " << shape.name << "</title>\n</svg>\n";
}

// ---------------------------------------------------------------------------
// Shape declarations and driver main.
// ---------------------------------------------------------------------------

static std::vector<ShapeSpec> all_shapes()
{
  return {shape_concave_two_holes(), shape_thin_corner(), shape_scatter_diagonals(),
          shape_cocircular_rectangle()};
}

static long double geometry_scale(const FinalInput& in)
{
  long double minx = 1e100L, maxx = -1e100L, miny = 1e100L, maxy = -1e100L;
  auto extend = [&](const V2& p) {
    minx = std::min(minx, p.x);
    maxx = std::max(maxx, p.x);
    miny = std::min(miny, p.y);
    maxy = std::max(maxy, p.y);
  };
  for (const V2& p : in.outer)
    extend(p);
  for (const auto& h : in.holes)
    for (const V2& p : h)
      extend(p);
  for (const V2& p : in.steiner)
    extend(p);
  const long double w = maxx - minx, h = maxy - miny;
  const long double L = std::max(w, h);
  return L > 0.0L ? L : 1.0L;
}

} // namespace

int main(int argc, char** argv)
{
  if (argc < 2) {
    std::cout << "usage: cdt-mesh <svg-output-dir>\n";
    return 2;
  }
  const std::string out_dir = argv[1];

  Failures failures;
  int variants_run = 0;
  int variants_expected = 0;

  for (const ShapeSpec& shape : all_shapes()) {
    for (const Variant& variant : variants_for(shape)) {
      ++variants_expected;
      const FinalInput in = build_final_input(shape, variant);
      const long double L = geometry_scale(in);
      const Tol tol = make_tol(L);

      std::string why;
      if (!validate_input(in, tol, why)) {
        report(failures, "coverage", shape.name + "/" + variant.name,
               "invalid test input: " + why);
        continue;
      }
      check_variant(shape, variant, in, tol, out_dir, failures);
      ++variants_run;
    }
  }

  const bool point_ok = failures.point_coverage == 0;
  const bool hole_ok = failures.hole_coverage == 0;
  const bool coverage_ok = failures.coverage == 0;
  const bool topology_ok = failures.topology == 0;
  const bool delaunay_ok = failures.delaunay == 0;
  const bool identity_ok = failures.identity == 0;

  std::cout << "SUMMARY variants=" << variants_run << "/" << variants_expected
            << " point-coverage-failures=" << failures.point_coverage
            << " hole-coverage-failures=" << failures.hole_coverage
            << " coverage-failures=" << failures.coverage
            << " topology-failures=" << failures.topology
            << " delaunay-failures=" << failures.delaunay
            << " identity-failures=" << failures.identity << "\n";

  if (point_ok && hole_ok && coverage_ok) {
    std::cout << "PASS coverage\n";
  } else {
    std::cout << "FAIL coverage point=" << failures.point_coverage
              << " hole=" << failures.hole_coverage
              << " geometric=" << failures.coverage << "\n";
  }
  if (topology_ok)
    std::cout << "PASS topology\n";
  else
    std::cout << "FAIL topology\n";
  if (delaunay_ok)
    std::cout << "PASS delaunay\n";
  else
    std::cout << "FAIL delaunay\n";
  if (identity_ok)
    std::cout << "PASS identity\n";
  else
    std::cout << "FAIL identity\n";

  const bool all_ok = point_ok && hole_ok && coverage_ok && topology_ok && delaunay_ok && identity_ok;
  std::cout << (all_ok ? "CDT_MESH_DRIVER_OK" : "CDT_MESH_DRIVER_FAIL") << "\n";
  return all_ok ? 0 : 1;
}
