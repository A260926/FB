#include "contract.hpp"

#include <poly2tri/poly2tri.h>

#include <cmath>
#include <cstddef>
#include <deque>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace {

using p2t::CDT;
using p2t::Point;
using p2t::Triangle;

/// Signed area of the triangle (a, b, c); positive for a counter-clockwise
/// triple.  Written here on purpose: the tests do not borrow p2t::Orient2d.
double Orient(const Point& a, const Point& b, const Point& c)
{
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

/// Determinant of the in-circle test for the counter-clockwise triple
/// (a, b, c): positive when d lies strictly inside the circumcircle.
double Incircle(const Point& a, const Point& b, const Point& c, const Point& d)
{
  const double adx = a.x - d.x;
  const double ady = a.y - d.y;
  const double bdx = b.x - d.x;
  const double bdy = b.y - d.y;
  const double cdx = c.x - d.x;
  const double cdy = c.y - d.y;
  const double alift = adx * adx + ady * ady;
  const double blift = bdx * bdx + bdy * bdy;
  const double clift = cdx * cdx + cdy * cdy;
  return alift * (bdx * cdy - cdx * bdy) - blift * (adx * cdy - cdx * ady) +
         clift * (adx * bdy - bdx * ady);
}

bool Flips(const Point& a, const Point& b, const Point& c, const Point& d)
{
  const double o1 = Orient(a, b, c);
  const double o2 = Orient(a, b, d);
  const double o3 = Orient(c, d, a);
  const double o4 = Orient(c, d, b);
  return o1 * o2 < 0.0 && o3 * o4 < 0.0;
}

struct EdgeKey {
  const Point* p;
  const Point* q;

  bool operator==(const EdgeKey& other) const { return p == other.p && q == other.q; }
  bool operator!=(const EdgeKey& other) const { return !(*this == other); }
};

struct EdgeLess {
  bool operator()(const EdgeKey& lhs, const EdgeKey& rhs) const
  {
    const std::less<const Point*> less;
    if (less(lhs.p, rhs.p))
      return true;
    if (less(rhs.p, lhs.p))
      return false;
    return less(lhs.q, rhs.q);
  }
};

using EdgeSet = std::set<EdgeKey, EdgeLess>;

EdgeKey MakeEdge(const Point* a, const Point* b)
{
  const std::less<const Point*> less;
  return less(a, b) ? EdgeKey{a, b} : EdgeKey{b, a};
}

struct Audit {
  size_t triangles = 0;
  double area = 0.0;
  size_t constrained_flags = 0;
  size_t bad_winding = 0;
  size_t non_reciprocal = 0;
  size_t flag_mismatch = 0;
  size_t incircle_violations = 0;
  size_t components = 0;
  std::set<const Point*> vertices;

  bool connected() const { return components == 1; }
};

Audit AuditMesh(const std::vector<Triangle*>& tris)
{
  Audit audit;
  audit.triangles = tris.size();
  const std::set<Triangle*> members(tris.begin(), tris.end());
  for (Triangle* t : tris) {
    const Point* a = t->GetPoint(0);
    const Point* b = t->GetPoint(1);
    const Point* c = t->GetPoint(2);
    if (Orient(*a, *b, *c) <= 0.0)
      ++audit.bad_winding;
    audit.area += std::abs(Orient(*a, *b, *c)) * 0.5;
    audit.vertices.insert(a);
    audit.vertices.insert(b);
    audit.vertices.insert(c);

    for (int i = 0; i < 3; ++i) {
      Point* p1 = t->GetPoint((i + 1) % 3);
      Point* p2 = t->GetPoint((i + 2) % 3);
      const Point* opposite_here = t->GetPoint(i);
      if (t->constrained_edge[i])
        ++audit.constrained_flags;
      Triangle* neighbor = t->GetNeighbor(i);
      if (!neighbor)
        continue;
      const int index = neighbor->EdgeIndex(p1, p2);
      if (index < 0) {
        ++audit.non_reciprocal;
        continue;
      }
      if (!members.count(neighbor))
        continue;
      if (neighbor->constrained_edge[index] != t->constrained_edge[i])
        ++audit.flag_mismatch;
      if (t->constrained_edge[i])
        continue;
      const Point* opposite = neighbor->GetPoint(index);
      if (!Flips(*p1, *p2, *opposite_here, *opposite))
        continue; // Not a flippable (convex) quad.
      const double det = Incircle(*p1, *p2, *opposite_here, *opposite);
      const double adx = p1->x - opposite->x;
      const double ady = p1->y - opposite->y;
      const double bdx = p2->x - opposite->x;
      const double bdy = p2->y - opposite->y;
      const double cdx = opposite_here->x - opposite->x;
      const double cdy = opposite_here->y - opposite->y;
      const double scale =
          adx * adx + ady * ady + bdx * bdx + bdy * bdy + cdx * cdx + cdy * cdy;
      if (det > 1e-10 * scale * scale)
        ++audit.incircle_violations;
    }
  }

  // The mesh must stay one edge-connected region: an edit may never isolate
  // a patch so that a mesh cleaner would drop one side.
  std::set<Triangle*> unvisited(tris.begin(), tris.end());
  while (!unvisited.empty()) {
    ++audit.components;
    std::vector<Triangle*> stack;
    stack.push_back(*unvisited.begin());
    unvisited.erase(unvisited.begin());
    while (!stack.empty()) {
      Triangle* t = stack.back();
      stack.pop_back();
      for (int i = 0; i < 3; ++i) {
        Triangle* neighbor = t->GetNeighbor(i);
        if (!neighbor || !members.count(neighbor))
          continue;
        const std::set<Triangle*>::iterator it = unvisited.find(neighbor);
        if (it == unvisited.end())
          continue;
        unvisited.erase(it);
        stack.push_back(neighbor);
      }
    }
  }
  return audit;
}

EdgeSet ConstrainedEdges(const std::vector<Triangle*>& tris)
{
  EdgeSet edges;
  for (Triangle* t : tris) {
    for (int i = 0; i < 3; ++i) {
      if (!t->constrained_edge[i])
        continue;
      edges.insert(MakeEdge(t->GetPoint((i + 1) % 3), t->GetPoint((i + 2) % 3)));
    }
  }
  return edges;
}

EdgeSet PolygonEdges(const std::vector<Point*>& polygon)
{
  EdgeSet edges;
  for (size_t i = 0; i < polygon.size(); ++i)
    edges.insert(MakeEdge(polygon[i], polygon[(i + 1) % polygon.size()]));
  return edges;
}

/// Number of triangles carrying edge (a, b) and whether every one of them
/// marks that edge as constrained.
size_t EdgeCarriers(const std::vector<Triangle*>& tris, const Point* a, const Point* b,
                    bool* all_constrained)
{
  size_t count = 0;
  bool all = true;
  for (Triangle* t : tris) {
    const int index = t->EdgeIndex(a, b);
    if (index < 0)
      continue;
    ++count;
    if (!t->constrained_edge[index])
      all = false;
  }
  if (all_constrained)
    *all_constrained = all;
  return count;
}

bool HasEdge(const std::vector<Triangle*>& tris, const Point* a, const Point* b)
{
  for (Triangle* t : tris) {
    if (t->EdgeIndex(a, b) >= 0)
      return true;
  }
  return false;
}

EdgeSet EdgesOf(const std::vector<Triangle*>& tris)
{
  EdgeSet edges;
  for (Triangle* t : tris) {
    for (int i = 0; i < 3; ++i)
      edges.insert(MakeEdge(t->GetPoint((i + 1) % 3), t->GetPoint((i + 2) % 3)));
  }
  return edges;
}

/// Full structural snapshot used to prove a rejected edit left everything
/// untouched: triangle pointers, vertex triples, adjacency and flags.
struct Snapshot {
  std::vector<Triangle*> triangles;
  std::vector<const Point*> points; // flattened triples
  std::vector<Triangle*> neighbors;
  std::vector<bool> constrained;
};

Snapshot Capture(const std::vector<Triangle*>& tris)
{
  Snapshot snapshot;
  snapshot.triangles = tris;
  for (Triangle* t : tris) {
    for (int i = 0; i < 3; ++i) {
      snapshot.points.push_back(t->GetPoint(i));
      snapshot.neighbors.push_back(t->GetNeighbor(i));
      snapshot.constrained.push_back(t->constrained_edge[i]);
    }
  }
  return snapshot;
}

bool SameSnapshot(const Snapshot& lhs, const Snapshot& rhs)
{
  return lhs.triangles == rhs.triangles && lhs.points == rhs.points &&
         lhs.neighbors == rhs.neighbors && lhs.constrained == rhs.constrained;
}

/// A surface patch in parameter space: unit grid square with a 4x4 interior
/// grid plus two off-grid points used by the diagonal geometry case.
struct GridFixture {
  std::deque<Point> store;
  std::vector<Point*> outer;
  Point* grid[4][4];
  Point* off_a;
  Point* off_b;
  std::unique_ptr<CDT> cdt;

  Point* Add(double x, double y)
  {
    store.emplace_back(x, y);
    return &store.back();
  }

  GridFixture()
  {
    outer.push_back(Add(0, 0));
    outer.push_back(Add(100, 0));
    outer.push_back(Add(100, 100));
    outer.push_back(Add(0, 100));
    for (int ix = 0; ix < 4; ++ix) {
      for (int iy = 0; iy < 4; ++iy)
        grid[ix][iy] = Add(20.0 + 20.0 * ix, 20.0 + 20.0 * iy);
    }
    off_a = Add(25, 70);
    off_b = Add(75, 30);

    cdt.reset(new CDT(outer));
    for (int ix = 0; ix < 4; ++ix)
      for (int iy = 0; iy < 4; ++iy)
        cdt->AddPoint(grid[ix][iy]);
    cdt->AddPoint(off_a);
    cdt->AddPoint(off_b);
    cdt->Triangulate();
  }

  std::vector<Triangle*> Tris() { return cdt->GetTriangles(); }
  Point* Grid(int ix, int iy) { return grid[ix][iy]; }
};

/// Square with a square hole and eight interior handles around it.
struct HoleFixture {
  std::deque<Point> store;
  std::vector<Point*> outer;
  std::vector<Point*> hole;
  std::vector<Point*> steiner;
  std::unique_ptr<CDT> cdt;

  Point* Add(double x, double y)
  {
    store.emplace_back(x, y);
    return &store.back();
  }

  HoleFixture()
  {
    outer.push_back(Add(0, 0));
    outer.push_back(Add(100, 0));
    outer.push_back(Add(100, 100));
    outer.push_back(Add(0, 100));
    hole.push_back(Add(35, 35));
    hole.push_back(Add(35, 65));
    hole.push_back(Add(65, 65));
    hole.push_back(Add(65, 35));
    steiner.push_back(Add(10, 50));
    steiner.push_back(Add(30, 50));
    steiner.push_back(Add(70, 50));
    steiner.push_back(Add(90, 50));
    steiner.push_back(Add(50, 10));
    steiner.push_back(Add(50, 30));
    steiner.push_back(Add(50, 70));
    steiner.push_back(Add(50, 90));

    cdt.reset(new CDT(outer));
    cdt->AddHole(hole);
    for (Point* p : steiner)
      cdt->AddPoint(p);
    cdt->Triangulate();
  }

  std::vector<Triangle*> Tris() { return cdt->GetTriangles(); }
};

/// Concave L-shaped outer boundary with a few interior handles.
struct ConcaveFixture {
  std::deque<Point> store;
  std::vector<Point*> outer;
  std::vector<Point*> steiner;
  std::unique_ptr<CDT> cdt;

  Point* Add(double x, double y)
  {
    store.emplace_back(x, y);
    return &store.back();
  }

  ConcaveFixture()
  {
    outer.push_back(Add(0, 0));
    outer.push_back(Add(60, 0));
    outer.push_back(Add(60, 20));
    outer.push_back(Add(20, 20));
    outer.push_back(Add(20, 60));
    outer.push_back(Add(0, 60));
    steiner.push_back(Add(10, 10));
    steiner.push_back(Add(40, 10));
    steiner.push_back(Add(10, 30));
    steiner.push_back(Add(10, 50));

    cdt.reset(new CDT(outer));
    for (Point* p : steiner)
      cdt->AddPoint(p);
    cdt->Triangulate();
  }

  std::vector<Triangle*> Tris() { return cdt->GetTriangles(); }
};

} // namespace

UNIT_CASE(geometry, crease_crosses_many_triangles)
{
  GridFixture fixture;
  const double expected_area = 10000.0;
  auto before = fixture.Tris();
  const Audit initial = AuditMesh(before);
  UNIT_CHECK(initial.triangles > 20);
  UNIT_CHECK(initial.bad_winding == 0);
  UNIT_CHECK(initial.non_reciprocal == 0);
  UNIT_CHECK(initial.flag_mismatch == 0);
  UNIT_CHECK(initial.incircle_violations == 0);
  UNIT_CHECK(initial.connected());
  UNIT_CHECK(std::abs(initial.area - expected_area) < 1e-9);
  UNIT_CHECK(initial.vertices.size() == 22);

  const Point* a = fixture.off_a;
  const Point* b = fixture.off_b;
  UNIT_CHECK(!HasEdge(before, a, b));

  // A corner triangle is far from the requested diagonal; it must survive
  // every local rewrite untouched.
  Triangle* corner = nullptr;
  for (Triangle* t : before) {
    if (t->Contains(fixture.Grid(0, 0)))
      corner = t;
  }
  UNIT_CHECK(corner != nullptr);
  Point* corner_points[3] = {corner->GetPoint(0), corner->GetPoint(1), corner->GetPoint(2)};

  // Triangles are mutated in place, so capture the vertex triples (not just
  // the pointers) before editing.
  std::vector<std::vector<const Point*>> triples_before;
  for (Triangle* t : before) {
    std::vector<const Point*> triple;
    for (int i = 0; i < 3; ++i)
      triple.push_back(t->GetPoint(i));
    triples_before.push_back(triple);
  }

  const std::set<Triangle*> pointers(before.begin(), before.end());
  const CDT::ConstraintId id = fixture.cdt->InsertConstraint(
      const_cast<Point*>(a), const_cast<Point*>(b));
  UNIT_CHECK(id != 0);

  auto after = fixture.Tris();
  UNIT_CHECK(after.size() == before.size());
  const std::set<Triangle*> after_pointers(after.begin(), after.end());
  UNIT_CHECK(after_pointers == pointers);
  const Audit audit = AuditMesh(after);
  UNIT_CHECK(audit.triangles == initial.triangles);
  UNIT_CHECK(audit.bad_winding == 0);
  UNIT_CHECK(audit.non_reciprocal == 0);
  UNIT_CHECK(audit.flag_mismatch == 0);
  UNIT_CHECK(audit.incircle_violations == 0);
  UNIT_CHECK(audit.connected());
  UNIT_CHECK(audit.vertices == initial.vertices);
  UNIT_CHECK(std::abs(audit.area - expected_area) < 1e-9);

  bool both = false;
  const size_t carriers = EdgeCarriers(after, a, b, &both);
  UNIT_CHECK(carriers == 2);
  UNIT_CHECK(both);

  // The crease really re-triangulated the interior: triangle objects keep
  // their identity but some of their vertex triples changed.
  size_t changed = 0;
  for (size_t i = 0; i < before.size(); ++i) {
    bool same = true;
    for (int k = 0; k < 3; ++k) {
      if (triples_before[i][k] != after[i]->GetPoint(k))
        same = false;
    }
    if (!same)
      ++changed;
  }
  UNIT_CHECK(changed > 0);
  UNIT_CHECK(corner_points[0] == corner->GetPoint(0) && corner_points[1] == corner->GetPoint(1) &&
             corner_points[2] == corner->GetPoint(2));
}

UNIT_CASE(geometry, long_line_through_existing_vertices)
{
  GridFixture fixture;
  auto before = fixture.Tris();
  const Point* a = fixture.Grid(0, 2); // (20, 40)
  const Point* b = fixture.Grid(3, 2); // (80, 40)
  const Point* mid0 = fixture.Grid(1, 2); // (40, 40)
  const Point* mid1 = fixture.Grid(2, 2); // (60, 40)

  UNIT_CHECK(!HasEdge(before, a, b));
  const CDT::ConstraintId id =
      fixture.cdt->InsertConstraint(const_cast<Point*>(a), const_cast<Point*>(b));
  UNIT_CHECK(id != 0);

  auto after = fixture.Tris();
  const Audit audit = AuditMesh(after);
  UNIT_CHECK(after.size() == before.size());
  UNIT_CHECK(audit.incircle_violations == 0);
  UNIT_CHECK(audit.non_reciprocal == 0);
  UNIT_CHECK(std::abs(audit.area - 10000.0) < 1e-9);

  // The long edge must not exist as one straight triangle side; every
  // sub-edge between consecutive on-line vertices must exist and be marked
  // constrained from both sides.
  UNIT_CHECK(!HasEdge(after, a, b));
  const Point* chain[4] = {a, mid0, mid1, b};
  for (size_t i = 0; i + 1 < 4; ++i) {
    bool both = false;
    const size_t carriers = EdgeCarriers(after, chain[i], chain[i + 1], &both);
    UNIT_CHECK(carriers >= 1);
    UNIT_CHECK(carriers == 2);
    UNIT_CHECK(both);
  }
  // No edge may skip an intermediate vertex either.
  UNIT_CHECK(!HasEdge(after, a, mid1));
  UNIT_CHECK(!HasEdge(after, mid0, b));
}

UNIT_CASE(geometry, hole_blocks_middle_of_segment)
{
  HoleFixture fixture;
  const double expected_area = 10000.0 - 30.0 * 30.0;
  auto triangles = fixture.Tris();
  const Audit initial = AuditMesh(triangles);
  UNIT_CHECK(initial.bad_winding == 0);
  UNIT_CHECK(initial.non_reciprocal == 0);
  UNIT_CHECK(initial.incircle_violations == 0);
  UNIT_CHECK(std::abs(initial.area - expected_area) < 1e-9);

  const EdgeSet boundary = PolygonEdges(fixture.outer);
  const EdgeSet hole_edges = PolygonEdges(fixture.hole);
  EdgeSet all_boundary = boundary;
  all_boundary.insert(hole_edges.begin(), hole_edges.end());
  UNIT_CHECK(ConstrainedEdges(triangles) == all_boundary);

  const Snapshot snapshot = Capture(triangles);
  const EdgeSet edges = EdgesOf(triangles);

  // Both endpoints are legal handles, but the middle would run through the
  // hole: the whole line must be rejected.
  UNIT_CHECK(fixture.cdt->InsertConstraint(fixture.steiner[0], fixture.steiner[3]) == 0);
  UNIT_CHECK(fixture.cdt->InsertConstraint(fixture.steiner[4], fixture.steiner[7]) == 0);
  auto after_rejects = fixture.Tris();
  UNIT_CHECK(SameSnapshot(snapshot, Capture(after_rejects)));
  UNIT_CHECK(EdgesOf(after_rejects) == edges);
  UNIT_CHECK(ConstrainedEdges(after_rejects) == all_boundary);

  // A line that stays on one side of the hole is accepted.
  const CDT::ConstraintId id =
      fixture.cdt->InsertConstraint(fixture.steiner[0], fixture.steiner[1]);
  UNIT_CHECK(id != 0);
  auto after = fixture.Tris();
  const Audit audit = AuditMesh(after);
  UNIT_CHECK(audit.incircle_violations == 0);
  UNIT_CHECK(audit.non_reciprocal == 0);
  UNIT_CHECK(audit.flag_mismatch == 0);
  UNIT_CHECK(std::abs(audit.area - expected_area) < 1e-9);
  const EdgeSet after_constrained = ConstrainedEdges(after);
  for (const EdgeKey& edge : all_boundary)
    UNIT_CHECK(after_constrained.count(edge) == 1);
  bool both = false;
  UNIT_CHECK(EdgeCarriers(after, fixture.steiner[0], fixture.steiner[1], &both) == 2);
  UNIT_CHECK(both);
}

UNIT_CASE(geometry, concave_boundary_rejects_outside_line)
{
  ConcaveFixture fixture;
  const double expected_area = 60.0 * 20.0 + 20.0 * 40.0;
  auto triangles = fixture.Tris();
  const Audit initial = AuditMesh(triangles);
  UNIT_CHECK(initial.bad_winding == 0);
  UNIT_CHECK(initial.non_reciprocal == 0);
  UNIT_CHECK(std::abs(initial.area - expected_area) < 1e-9);

  const Snapshot snapshot = Capture(triangles);
  // A straight line between two legal handles that leaves the filled region
  // through the concave notch.
  UNIT_CHECK(fixture.cdt->InsertConstraint(fixture.steiner[1], fixture.steiner[3]) == 0);
  auto after_reject = fixture.Tris();
  UNIT_CHECK(SameSnapshot(snapshot, Capture(after_reject)));
  UNIT_CHECK(std::abs(AuditMesh(after_reject).area - initial.area) < 1e-9);

  // Walking along the existing outer boundary is acceptable: the edge is
  // already protected, so the id only adds a reference count.
  const CDT::ConstraintId boundary_id =
      fixture.cdt->InsertConstraint(fixture.outer[0], fixture.outer[5]);
  UNIT_CHECK(boundary_id != 0);
  const EdgeSet boundary = PolygonEdges(fixture.outer);
  UNIT_CHECK(ConstrainedEdges(after_reject) == boundary);

  // A crease through an interior handle: it must split into real sub-edges.
  const CDT::ConstraintId id =
      fixture.cdt->InsertConstraint(fixture.steiner[0], fixture.steiner[3]);
  UNIT_CHECK(id != 0);
  auto after = fixture.Tris();
  const Audit audit = AuditMesh(after);
  UNIT_CHECK(audit.bad_winding == 0);
  UNIT_CHECK(audit.non_reciprocal == 0);
  UNIT_CHECK(audit.incircle_violations == 0);
  UNIT_CHECK(std::abs(audit.area - expected_area) < 1e-9);
  const EdgeSet after_constrained = ConstrainedEdges(after);
  for (const EdgeKey& edge : boundary)
    UNIT_CHECK(after_constrained.count(edge) == 1);
  const Point* chain[3] = {fixture.steiner[0], fixture.steiner[2], fixture.steiner[3]};
  for (size_t i = 0; i + 1 < 3; ++i) {
    bool both = false;
    UNIT_CHECK(EdgeCarriers(after, chain[i], chain[i + 1], &both) >= 1);
    UNIT_CHECK(both);
  }
  UNIT_CHECK(!HasEdge(after, fixture.steiner[0], fixture.steiner[3]));
  UNIT_CHECK(fixture.cdt->RemoveConstraint(boundary_id));
  UNIT_CHECK(fixture.cdt->RemoveConstraint(id));
  const Audit final_audit = AuditMesh(fixture.Tris());
  UNIT_CHECK(final_audit.incircle_violations == 0);
  UNIT_CHECK(ConstrainedEdges(fixture.Tris()) == boundary);
}

UNIT_CASE(ownership, overlapping_ids_release_only_their_own)
{
  GridFixture fixture;
  auto triangles = fixture.Tris();
  const EdgeSet boundary = PolygonEdges(fixture.outer);
  UNIT_CHECK(ConstrainedEdges(triangles) == boundary);

  // Row segment through existing vertices: (20,60)-(80,60).
  const CDT::ConstraintId long_id =
      fixture.cdt->InsertConstraint(fixture.Grid(0, 3), fixture.Grid(3, 3));
  UNIT_CHECK(long_id != 0);
  // A partially overlapping line sharing the middle sub-edge.
  const CDT::ConstraintId part_id =
      fixture.cdt->InsertConstraint(fixture.Grid(0, 3), fixture.Grid(2, 3));
  UNIT_CHECK(part_id != 0);
  // A reverse duplicate of the short line: its own id, same sub-edges.
  const CDT::ConstraintId reverse_id =
      fixture.cdt->InsertConstraint(fixture.Grid(2, 3), fixture.Grid(0, 3));
  UNIT_CHECK(reverse_id != 0);
  UNIT_CHECK(long_id != part_id);
  UNIT_CHECK(long_id != reverse_id);
  UNIT_CHECK(part_id != reverse_id);

  const Point* chain[4] = {fixture.Grid(0, 3), fixture.Grid(1, 3), fixture.Grid(2, 3),
                           fixture.Grid(3, 3)};
  auto check_all_chain = [&](const std::vector<Triangle*>& tris) {
    for (size_t i = 0; i + 1 < 4; ++i) {
      bool both = false;
      UNIT_CHECK(EdgeCarriers(tris, chain[i], chain[i + 1], &both) == 2);
      UNIT_CHECK(both);
    }
  };

  auto with_all = fixture.Tris();
  check_all_chain(with_all);
  UNIT_CHECK(ConstrainedEdges(with_all).size() >= boundary.size());

  // Long line owns all three sub-edges; drop only the short overlap first.
  UNIT_CHECK(fixture.cdt->RemoveConstraint(part_id));
  auto after_part = fixture.Tris();
  check_all_chain(after_part);

  // Drop the long line: the reverse duplicate still protects the first two
  // sub-edges, the last one is released.
  UNIT_CHECK(fixture.cdt->RemoveConstraint(long_id));
  auto after_long = fixture.Tris();
  for (size_t i = 0; i < 2; ++i) {
    bool both = false;
    UNIT_CHECK(EdgeCarriers(after_long, chain[i], chain[i + 1], &both) == 2);
    UNIT_CHECK(both);
  }
  {
    bool both = false;
    const size_t carriers = EdgeCarriers(after_long, chain[2], chain[3], &both);
    UNIT_CHECK(carriers >= 1);
    UNIT_CHECK(!both);
  }
  const EdgeSet after_long_constrained = ConstrainedEdges(after_long);
  for (const EdgeKey& edge : boundary)
    UNIT_CHECK(after_long_constrained.count(edge) == 1);

  // Removing it again must fail; the id is retired.
  UNIT_CHECK(!fixture.cdt->RemoveConstraint(long_id));
  UNIT_CHECK(!fixture.cdt->RemoveConstraint(part_id));
  UNIT_CHECK(fixture.cdt->RemoveConstraint(reverse_id));
  const Audit final_audit = AuditMesh(fixture.Tris());
  UNIT_CHECK(final_audit.incircle_violations == 0);
  UNIT_CHECK(final_audit.non_reciprocal == 0);
  UNIT_CHECK(std::abs(final_audit.area - 10000.0) < 1e-9);
  UNIT_CHECK(ConstrainedEdges(fixture.Tris()) == boundary);
}

UNIT_CASE(ownership, original_boundary_survives_every_order)
{
  HoleFixture fixture;
  const EdgeSet boundary = [&fixture] {
    EdgeSet set = PolygonEdges(fixture.outer);
    const EdgeSet hole = PolygonEdges(fixture.hole);
    set.insert(hole.begin(), hole.end());
    return set;
  }();
  UNIT_CHECK(ConstrainedEdges(fixture.Tris()) == boundary);

  // Each constraint id is retired independently, in an interleaved order.
  const CDT::ConstraintId first =
      fixture.cdt->InsertConstraint(fixture.steiner[0], fixture.steiner[1]);
  const CDT::ConstraintId second =
      fixture.cdt->InsertConstraint(fixture.steiner[2], fixture.steiner[3]);
  const CDT::ConstraintId third =
      fixture.cdt->InsertConstraint(fixture.steiner[4], fixture.steiner[5]);
  UNIT_CHECK(first != 0);
  UNIT_CHECK(second != 0);
  UNIT_CHECK(third != 0);
  UNIT_CHECK(first != second && second != third && first != third);
  UNIT_CHECK(ConstrainedEdges(fixture.Tris()).size() >= boundary.size());

  UNIT_CHECK(fixture.cdt->RemoveConstraint(second));
  UNIT_CHECK(fixture.cdt->RemoveConstraint(third));
  UNIT_CHECK(!fixture.cdt->RemoveConstraint(second));
  UNIT_CHECK(fixture.cdt->RemoveConstraint(first));
  UNIT_CHECK(!fixture.cdt->RemoveConstraint(first));
  UNIT_CHECK(!fixture.cdt->RemoveConstraint(0));
  UNIT_CHECK(!fixture.cdt->RemoveConstraint(static_cast<CDT::ConstraintId>(1) << 62));

  auto tris = fixture.Tris();
  const Audit audit = AuditMesh(tris);
  UNIT_CHECK(audit.incircle_violations == 0);
  UNIT_CHECK(audit.non_reciprocal == 0);
  UNIT_CHECK(audit.bad_winding == 0);
  UNIT_CHECK(std::abs(audit.area - (10000.0 - 900.0)) < 1e-9);
  UNIT_CHECK(ConstrainedEdges(tris) == boundary);
  // The outer ring and the hole edges are still present and still flagged
  // constrained.  They border the unbounded exterior, so one side is enough.
  for (const EdgeKey& edge : boundary) {
    bool both = false;
    const size_t carriers = EdgeCarriers(tris, edge.p, edge.q, &both);
    UNIT_CHECK(carriers >= 1);
    UNIT_CHECK(both);
  }
}

UNIT_CASE(session, failures_do_not_poison_later_edits)
{
  HoleFixture fixture;
  auto triangles = fixture.Tris();
  const Snapshot snapshot = Capture(triangles);
  const EdgeSet boundary = ConstrainedEdges(triangles);

  // Rejected requests first.
  UNIT_CHECK(fixture.cdt->InsertConstraint(nullptr, fixture.steiner[0]) == 0);
  UNIT_CHECK(fixture.cdt->InsertConstraint(fixture.steiner[0], nullptr) == 0);
  UNIT_CHECK(fixture.cdt->InsertConstraint(nullptr, nullptr) == 0);
  UNIT_CHECK(fixture.cdt->InsertConstraint(fixture.steiner[0], fixture.steiner[0]) == 0);

  Point foreign_a(10, 10);
  Point foreign_b(90, 90);
  UNIT_CHECK(fixture.cdt->InsertConstraint(&foreign_a, fixture.steiner[0]) == 0);
  UNIT_CHECK(fixture.cdt->InsertConstraint(fixture.steiner[0], &foreign_b) == 0);

  // A line whose middle goes through the hole.
  UNIT_CHECK(fixture.cdt->InsertConstraint(fixture.steiner[0], fixture.steiner[3]) == 0);
  UNIT_CHECK(SameSnapshot(snapshot, Capture(fixture.Tris())));

  // A legal edit after failures must work and leave the mesh valid.
  const CDT::ConstraintId good =
      fixture.cdt->InsertConstraint(fixture.steiner[4], fixture.steiner[5]);
  UNIT_CHECK(good != 0);
  auto after_good = fixture.Tris();
  const Audit audit = AuditMesh(after_good);
  UNIT_CHECK(audit.bad_winding == 0);
  UNIT_CHECK(audit.non_reciprocal == 0);
  UNIT_CHECK(audit.incircle_violations == 0);
  UNIT_CHECK(std::abs(audit.area - (10000.0 - 900.0)) < 1e-9);
  UNIT_CHECK(ConstrainedEdges(after_good).size() >= boundary.size());

  // Crossing an active constraint is rejected and does not disturb the mesh.
  const Snapshot before_cross = Capture(after_good);
  UNIT_CHECK(fixture.cdt->InsertConstraint(fixture.steiner[0], fixture.steiner[3]) == 0);
  UNIT_CHECK(SameSnapshot(before_cross, Capture(fixture.Tris())));

  // Failed removal requests are harmless.
  UNIT_CHECK(!fixture.cdt->RemoveConstraint(good + 1000));
  UNIT_CHECK(!fixture.cdt->RemoveConstraint(0));
  UNIT_CHECK(SameSnapshot(before_cross, Capture(fixture.Tris())));

  UNIT_CHECK(fixture.cdt->RemoveConstraint(good));
  const Audit final_audit = AuditMesh(fixture.Tris());
  UNIT_CHECK(final_audit.incircle_violations == 0);
  UNIT_CHECK(std::abs(final_audit.area - (10000.0 - 900.0)) < 1e-9);
}

UNIT_CASE(session, consecutive_neighbouring_edits_stay_consistent)
{
  GridFixture fixture;
  const std::vector<Triangle*> before = fixture.Tris();
  const std::set<Triangle*> identity(before.begin(), before.end());
  const double expected_area = 10000.0;
  std::vector<CDT::ConstraintId> ids;

  // A fan of creases sharing the (20,20) corner and neighbouring grid points.
  ids.push_back(fixture.cdt->InsertConstraint(fixture.Grid(0, 0), fixture.Grid(2, 2)));
  ids.push_back(fixture.cdt->InsertConstraint(fixture.Grid(2, 2), fixture.Grid(3, 2)));
  ids.push_back(fixture.cdt->InsertConstraint(fixture.Grid(3, 2), fixture.Grid(3, 3)));
  ids.push_back(fixture.cdt->InsertConstraint(fixture.Grid(2, 2), fixture.Grid(2, 3)));
  ids.push_back(fixture.cdt->InsertConstraint(fixture.Grid(0, 0), fixture.Grid(3, 3)));
  for (CDT::ConstraintId id : ids)
    UNIT_CHECK(id != 0);

  auto tris = fixture.Tris();
  UNIT_CHECK(tris.size() == before.size());
  const std::set<Triangle*> after_identity(tris.begin(), tris.end());
  UNIT_CHECK(after_identity == identity);
  const Audit audit = AuditMesh(tris);
  UNIT_CHECK(audit.bad_winding == 0);
  UNIT_CHECK(audit.non_reciprocal == 0);
  UNIT_CHECK(audit.flag_mismatch == 0);
  UNIT_CHECK(audit.incircle_violations == 0);
  UNIT_CHECK(audit.vertices.size() == 22);
  UNIT_CHECK(std::abs(audit.area - expected_area) < 1e-9);

  // Removing the creases must not only clear flags: the diagonals that were
  // kept non-Delaunay while the crease existed have to flip back.  Capture the
  // edge set before any edit and require it to come back exactly.
  const EdgeSet pristine = EdgesOf(before);

  // Undo in reverse order; the mesh stays structurally sound after each step.
  for (size_t i = ids.size(); i-- > 0;) {
    UNIT_CHECK(fixture.cdt->RemoveConstraint(ids[i]));
    const Audit step = AuditMesh(fixture.Tris());
    UNIT_CHECK(step.bad_winding == 0);
    UNIT_CHECK(step.non_reciprocal == 0);
    UNIT_CHECK(step.incircle_violations == 0);
    UNIT_CHECK(std::abs(step.area - expected_area) < 1e-9);
  }
  UNIT_CHECK(EdgesOf(fixture.Tris()) == pristine);
  UNIT_CHECK(ConstrainedEdges(fixture.Tris()) == PolygonEdges(fixture.outer));
}

UNIT_CASE(session, untouched_legacy_workflow_still_works)
{
  // The original one-shot workflow must behave exactly as before: same
  // triangle count, area and boundary flags, and no editing calls needed.
  std::deque<Point> store;
  auto add = [&store](double x, double y) {
    store.emplace_back(x, y);
    return &store.back();
  };
  std::vector<Point*> outer{add(0, 0), add(12, 0), add(12, 10), add(0, 10)};
  std::vector<Point*> hole{add(5, 4), add(5, 6), add(7, 6), add(7, 4)};
  std::vector<Point*> handles{add(2, 2), add(4, 2), add(8, 2), add(10, 2), add(2, 8), add(10, 8)};

  CDT cdt(outer);
  cdt.AddHole(hole);
  for (Point* p : handles)
    cdt.AddPoint(p);
  cdt.Triangulate();

  auto tris = cdt.GetTriangles();
  UNIT_CHECK(tris.size() == 20);
  const Audit audit = AuditMesh(tris);
  UNIT_CHECK(audit.bad_winding == 0);
  UNIT_CHECK(audit.non_reciprocal == 0);
  UNIT_CHECK(audit.constrained_flags == 8);
  UNIT_CHECK(audit.vertices.size() == 14);
  UNIT_CHECK(std::abs(audit.area - 116.0) < 1e-9);
  UNIT_CHECK(!cdt.GetMap().empty());
  const EdgeSet boundary = ConstrainedEdges(tris);
  UNIT_CHECK(boundary.size() == 8);

  // Editing is still available on the same object, but only through the new
  // entry points; GetTriangles/GetMap stay consistent afterwards.
  const CDT::ConstraintId id = cdt.InsertConstraint(handles[0], handles[4]);
  UNIT_CHECK(id != 0);
  UNIT_CHECK(cdt.GetTriangles().size() == 20);
  UNIT_CHECK(!cdt.GetMap().empty());
  UNIT_CHECK(AuditMesh(cdt.GetTriangles()).incircle_violations == 0);
  UNIT_CHECK(cdt.RemoveConstraint(id));
  UNIT_CHECK(ConstrainedEdges(cdt.GetTriangles()) == boundary);
  UNIT_CHECK(std::abs(AuditMesh(cdt.GetTriangles()).area - 116.0) < 1e-9);
}

UNIT_CASE(session, edits_require_a_completed_triangulation)
{
  std::deque<Point> store;
  auto add = [&store](double x, double y) {
    store.emplace_back(x, y);
    return &store.back();
  };
  std::vector<Point*> outer{add(0, 0), add(10, 0), add(10, 10), add(0, 10)};

  CDT cdt(outer);
  // Before Triangulate the mesh does not exist yet: every request is refused
  // and leaves the object usable.
  UNIT_CHECK(cdt.InsertConstraint(outer[0], outer[2]) == 0);
  UNIT_CHECK(cdt.InsertConstraint(outer[0], outer[0]) == 0);
  UNIT_CHECK(cdt.InsertConstraint(nullptr, outer[2]) == 0);
  UNIT_CHECK(!cdt.RemoveConstraint(1));
  UNIT_CHECK(!cdt.RemoveConstraint(0));
  UNIT_CHECK(cdt.GetTriangles().empty());

  cdt.Triangulate();
  UNIT_CHECK(!cdt.GetTriangles().empty());

  // After Triangulate, points that were never added are foreign objects.
  Point foreign(5, 5);
  UNIT_CHECK(cdt.InsertConstraint(&foreign, outer[0]) == 0);

  const CDT::ConstraintId id = cdt.InsertConstraint(outer[0], outer[2]);
  UNIT_CHECK(id != 0);
  bool both = false;
  UNIT_CHECK(EdgeCarriers(cdt.GetTriangles(), outer[0], outer[2], &both) == 2);
  UNIT_CHECK(both);
  UNIT_CHECK(cdt.GetTriangles().size() == 2);
  UNIT_CHECK(std::abs(AuditMesh(cdt.GetTriangles()).area - 100.0) < 1e-9);
  UNIT_CHECK(cdt.RemoveConstraint(id));
  UNIT_CHECK(cdt.GetTriangles().size() == 2);
  UNIT_CHECK(AuditMesh(cdt.GetTriangles()).incircle_violations == 0);
}
