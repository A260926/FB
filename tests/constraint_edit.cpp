// Dynamic constraint editing tests for p2t::CDT.
//
// The geometric predicates below are written here on purpose and do not call
// the library's own orientation/incircle routines: the tests must judge the
// produced mesh independently.  Every case is registered through the fixed
// support/contract.hpp driver and makes real content assertions.

#include "contract.hpp"

#include <poly2tri/poly2tri.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

namespace {

// ---- test-local, library-independent predicates ---------------------------

int orientation(const p2t::Point& a, const p2t::Point& b,
                const p2t::Point& c)
{
  const double det =
    (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  if (std::fpclassify(det) == FP_ZERO) {
    return 0;
  }
  return det < 0.0 ? -1 : 1;
}

// Positive when d is strictly inside the circumcircle of the triangle a,b,c.
double incircle(const p2t::Point& a, const p2t::Point& b,
                const p2t::Point& c, const p2t::Point& d)
{
  const p2t::Point* p1 = &a;
  const p2t::Point* p2 = &b;
  const p2t::Point* p3 = &c;
  if (orientation(*p1, *p2, *p3) < 0) {
    std::swap(p2, p3);
  }
  const double adx = p1->x - d.x, ady = p1->y - d.y;
  const double bdx = p2->x - d.x, bdy = p2->y - d.y;
  const double cdx = p3->x - d.x, cdy = p3->y - d.y;
  return (adx * adx + ady * ady) * (bdx * cdy - cdx * bdy) +
         (bdx * bdx + bdy * bdy) * (cdx * ady - adx * cdy) +
         (cdx * cdx + cdy * cdy) * (adx * bdy - bdx * ady);
}

double triangle_area(p2t::Triangle& t)
{
  const p2t::Point* a = t.GetPoint(0);
  const p2t::Point* b = t.GetPoint(1);
  const p2t::Point* c = t.GetPoint(2);
  return std::fabs((b->x - a->x) * (c->y - a->y) -
                   (b->y - a->y) * (c->x - a->x)) / 2.0;
}

double total_area(p2t::CDT& cdt)
{
  double sum = 0.0;
  for (p2t::Triangle* t : cdt.GetTriangles()) {
    sum += triangle_area(*t);
  }
  return sum;
}

bool has_edge(p2t::CDT& cdt, const p2t::Point* a, const p2t::Point* b)
{
  for (p2t::Triangle* t : cdt.GetTriangles()) {
    if (t->Contains(const_cast<p2t::Point*>(a),
                    const_cast<p2t::Point*>(b))) {
      return true;
    }
  }
  return false;
}

// The constrained flag must agree on both sides of every shared edge.
bool constrained_flags_consistent(p2t::CDT& cdt)
{
  for (p2t::Triangle* t : cdt.GetTriangles()) {
    for (int i = 0; i < 3; ++i) {
      if (!t->constrained_edge[i]) {
        continue;
      }
      p2t::Triangle* n = t->GetNeighbor(i);
      if (n == nullptr) {
        continue; // boundary edge has a single incident triangle
      }
      const p2t::Point* a = t->GetPoint((i + 1) % 3);
      const p2t::Point* b = t->GetPoint((i + 2) % 3);
      const int ni = n->EdgeIndex(const_cast<p2t::Point*>(a),
                                  const_cast<p2t::Point*>(b));
      if (ni < 0 || !n->constrained_edge[ni]) {
        return false;
      }
    }
  }
  return true;
}

bool edge_is_constrained(p2t::CDT& cdt, const p2t::Point* a,
                         const p2t::Point* b)
{
  int sides = 0;
  int marked = 0;
  for (p2t::Triangle* t : cdt.GetTriangles()) {
    const int i = t->EdgeIndex(const_cast<p2t::Point*>(a),
                               const_cast<p2t::Point*>(b));
    if (i >= 0) {
      ++sides;
      if (t->constrained_edge[i]) {
        ++marked;
      }
    }
  }
  return sides > 0 && sides == marked;
}

bool adjacency_reciprocal(p2t::CDT& cdt)
{
  for (p2t::Triangle* t : cdt.GetTriangles()) {
    for (int i = 0; i < 3; ++i) {
      p2t::Triangle* n = t->GetNeighbor(i);
      if (n == nullptr) {
        continue;
      }
      const p2t::Point* a = t->GetPoint((i + 1) % 3);
      const p2t::Point* b = t->GetPoint((i + 2) % 3);
      if (!n->Contains(const_cast<p2t::Point*>(a),
                       const_cast<p2t::Point*>(b))) {
        return false;
      }
      const int ni = n->EdgeIndex(const_cast<p2t::Point*>(a),
                                  const_cast<p2t::Point*>(b));
      if (ni < 0 || n->GetNeighbor(ni) != t) {
        return false;
      }
    }
  }
  return true;
}

// Every unconstrained shared edge satisfies the local empty circle
// condition.  Constrained edges are allowed to violate it.
bool local_delaunay(p2t::CDT& cdt)
{
  const double tol = 1e-9;
  for (p2t::Triangle* t : cdt.GetTriangles()) {
    for (int i = 0; i < 3; ++i) {
      if (t->constrained_edge[i]) {
        continue;
      }
      p2t::Triangle* n = t->GetNeighbor(i);
      if (n == nullptr) {
        continue;
      }
      const p2t::Point* a = t->GetPoint((i + 1) % 3);
      const p2t::Point* b = t->GetPoint((i + 2) % 3);
      const int ni = n->EdgeIndex(const_cast<p2t::Point*>(a),
                                  const_cast<p2t::Point*>(b));
      if (ni < 0 || n->constrained_edge[ni]) {
        continue;
      }
      const p2t::Point* opposite = n->GetPoint(ni);
      if (incircle(*t->GetPoint(0), *t->GetPoint(1),
                   *t->GetPoint(2), *opposite) > tol) {
        return false;
      }
    }
  }
  return true;
}

bool one_connected_component(p2t::CDT& cdt)
{
  std::vector<p2t::Triangle*> triangles = cdt.GetTriangles();
  if (triangles.empty()) {
    return false;
  }
  std::set<p2t::Triangle*> seen;
  std::vector<p2t::Triangle*> stack{triangles.front()};
  seen.insert(triangles.front());
  while (!stack.empty()) {
    p2t::Triangle* t = stack.back();
    stack.pop_back();
    for (int i = 0; i < 3; ++i) {
      p2t::Triangle* n = t->GetNeighbor(i);
      if (n != nullptr && seen.insert(n).second) {
        stack.push_back(n);
      }
    }
  }
  for (p2t::Triangle* t : triangles) {
    if (seen.count(t) == 0) {
      return false;
    }
  }
  return true;
}

// Content fingerprint: sorted triangle triples together with the constrained
// flag attached to each sorted edge.  Two equal fingerprints mean identical
// connectivity and constraint flags.
struct TriangleSignature {
  std::vector<const p2t::Point*> verts;
  std::vector<std::pair<std::pair<const p2t::Point*, const p2t::Point*>, bool>>
    edges;

  bool operator<(const TriangleSignature& other) const
  {
    if (verts != other.verts) {
      return verts < other.verts;
    }
    return edges < other.edges;
  }

  bool operator==(const TriangleSignature& other) const
  {
    return verts == other.verts && edges == other.edges;
  }
};

std::vector<TriangleSignature> fingerprint(p2t::CDT& cdt)
{
  std::vector<TriangleSignature> out;
  for (p2t::Triangle* t : cdt.GetTriangles()) {
    TriangleSignature sig;
    sig.verts = {t->GetPoint(0), t->GetPoint(1), t->GetPoint(2)};
    std::sort(sig.verts.begin(), sig.verts.end());
    for (int i = 0; i < 3; ++i) {
      const p2t::Point* u = t->GetPoint((i + 1) % 3);
      const p2t::Point* v = t->GetPoint((i + 2) % 3);
      const p2t::Point* lo = u < v ? u : v;
      const p2t::Point* hi = u < v ? v : u;
      sig.edges.push_back({{lo, hi}, t->constrained_edge[i]});
    }
    std::sort(sig.edges.begin(), sig.edges.end());
    out.push_back(sig);
  }
  std::sort(out.begin(), out.end());
  return out;
}

int count_constrained_flags(p2t::CDT& cdt)
{
  int count = 0;
  for (p2t::Triangle* t : cdt.GetTriangles()) {
    for (int i = 0; i < 3; ++i) {
      if (t->constrained_edge[i]) {
        ++count;
      }
    }
  }
  return count;
}

// A square patch with five interior Steiner points, several of which lie on
// the two main diagonals, so long crease lines split into real sub-edges.
struct DiagonalPatch {
  std::vector<p2t::Point> outer{
    {0, 0}, {10, 0}, {10, 10}, {0, 10}};
  std::vector<p2t::Point> steiner{
    {3, 3}, {7, 3}, {7, 7}, {3, 7}, {5, 5}};
  p2t::CDT cdt;

  DiagonalPatch() : cdt(std::vector<p2t::Point*>{
      &outer[0], &outer[1], &outer[2], &outer[3]})
  {
    for (p2t::Point& p : steiner) {
      cdt.AddPoint(&p);
    }
    cdt.Triangulate();
  }
};

} // namespace

// ============================ geometry group ===============================

UNIT_CASE(geometry, crease_forced_across_multiple_triangles)
{
  DiagonalPatch patch;
  const double area_before = total_area(patch.cdt);
  const std::size_t triangles_before = patch.cdt.GetTriangles().size();

  // (0,0)-(10,10) is not an existing edge and runs through (3,3),(5,5),(7,7).
  UNIT_CHECK(!has_edge(patch.cdt, &patch.outer[0], &patch.outer[2]));
  const p2t::CDT::ConstraintId id =
    patch.cdt.InsertConstraint(&patch.outer[0], &patch.outer[2]);
  UNIT_CHECK(id != 0);

  const std::vector<const p2t::Point*> chain{
    &patch.outer[0], &patch.steiner[0], &patch.steiner[4],
    &patch.steiner[2], &patch.outer[2]};
  for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
    UNIT_CHECK(has_edge(patch.cdt, chain[i], chain[i + 1]));
    UNIT_CHECK(edge_is_constrained(patch.cdt, chain[i], chain[i + 1]));
  }
  // No single long edge may skip an intermediate mesh vertex.
  UNIT_CHECK(!has_edge(patch.cdt, &patch.outer[0], &patch.outer[2]));

  UNIT_CHECK(patch.cdt.GetTriangles().size() == triangles_before);
  UNIT_CHECK(std::fabs(total_area(patch.cdt) - area_before) < 1e-9);
  UNIT_CHECK(adjacency_reciprocal(patch.cdt));
  UNIT_CHECK(constrained_flags_consistent(patch.cdt));
  UNIT_CHECK(one_connected_component(patch.cdt));
  UNIT_CHECK(local_delaunay(patch.cdt));
}

UNIT_CASE(geometry, long_line_splits_at_existing_vertices)
{
  DiagonalPatch patch;
  const double area_before = total_area(patch.cdt);

  const p2t::CDT::ConstraintId id =
    patch.cdt.InsertConstraint(&patch.outer[3], &patch.outer[1]);
  UNIT_CHECK(id != 0);

  // Anti-diagonal passes through (3,7),(5,5),(7,3).
  const std::vector<const p2t::Point*> chain{
    &patch.outer[3], &patch.steiner[3], &patch.steiner[4],
    &patch.steiner[1], &patch.outer[1]};
  for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
    UNIT_CHECK(has_edge(patch.cdt, chain[i], chain[i + 1]));
    UNIT_CHECK(edge_is_constrained(patch.cdt, chain[i], chain[i + 1]));
  }
  UNIT_CHECK(!has_edge(patch.cdt, &patch.outer[3], &patch.outer[1]));
  UNIT_CHECK(std::fabs(total_area(patch.cdt) - area_before) < 1e-9);
  UNIT_CHECK(adjacency_reciprocal(patch.cdt));
  UNIT_CHECK(local_delaunay(patch.cdt));
  UNIT_CHECK(one_connected_component(patch.cdt));
}

UNIT_CASE(geometry, hole_blocks_line_but_accepts_valid_crease)
{
  std::vector<p2t::Point> outer{
    {0, 0}, {12, 0}, {12, 10}, {0, 10}};
  std::vector<p2t::Point> hole{
    {5, 4}, {5, 6}, {7, 6}, {7, 4}};
  std::vector<p2t::Point> handles{
    {2, 2}, {4, 2}, {8, 2}, {10, 2}, {2, 8}, {10, 8}};
  std::vector<p2t::Point*> outline;
  std::vector<p2t::Point*> holeline;
  std::set<p2t::Point*> expected;
  for (p2t::Point& p : outer) {
    outline.push_back(&p);
    expected.insert(&p);
  }
  for (p2t::Point& p : hole) {
    holeline.push_back(&p);
    expected.insert(&p);
  }
  for (p2t::Point& p : handles) {
    expected.insert(&p);
  }

  p2t::CDT cdt(outline);
  cdt.AddHole(holeline);
  for (p2t::Point& p : handles) {
    cdt.AddPoint(&p);
  }
  cdt.Triangulate();
  const double area_before = total_area(cdt);
  const auto print_before = fingerprint(cdt);

  // The main diagonal cuts straight through the rectangular hole.
  UNIT_CHECK(cdt.InsertConstraint(&outer[0], &outer[2]) == 0);
  // Rejection must leave connectivity and flags untouched.
  UNIT_CHECK(fingerprint(cdt) == print_before);
  UNIT_CHECK(std::fabs(total_area(cdt) - area_before) < 1e-9);
  UNIT_CHECK(one_connected_component(cdt));

  // A crease entirely in the lower strip is legal.
  const p2t::CDT::ConstraintId id =
    cdt.InsertConstraint(&handles[0], &handles[3]);
  UNIT_CHECK(id != 0);
  UNIT_CHECK(has_edge(cdt, &handles[0], &handles[3]) ||
             edge_is_constrained(cdt, &handles[0], &handles[1]));
  UNIT_CHECK(std::fabs(total_area(cdt) - area_before) < 1e-9);
  UNIT_CHECK(adjacency_reciprocal(cdt));
  UNIT_CHECK(constrained_flags_consistent(cdt));
  UNIT_CHECK(local_delaunay(cdt));

  // No vertex was introduced and every face uses the original points.
  for (p2t::Triangle* t : cdt.GetTriangles()) {
    for (int i = 0; i < 3; ++i) {
      UNIT_CHECK(expected.count(t->GetPoint(i)) == 1);
    }
  }
}

UNIT_CASE(geometry, concave_contour_rejects_notch_cutting_line)
{
  // L-shaped simple polygon (area 76).
  std::vector<p2t::Point> outline_points{
    {0, 0}, {10, 0}, {10, 4}, {6, 4}, {6, 10}, {0, 10}};
  p2t::Point steiner(3, 2);
  std::vector<p2t::Point*> outline;
  for (p2t::Point& p : outline_points) {
    outline.push_back(&p);
  }
  p2t::CDT cdt(outline);
  cdt.AddPoint(&steiner);
  cdt.Triangulate();
  const double area_before = total_area(cdt);
  const auto print_before = fingerprint(cdt);

  // (0,10)-(10,4) leaves the polygon through the missing upper-right notch.
  UNIT_CHECK(cdt.InsertConstraint(&outline_points[5],
                                  &outline_points[2]) == 0);
  UNIT_CHECK(fingerprint(cdt) == print_before);
  UNIT_CHECK(std::fabs(total_area(cdt) - area_before) < 1e-9);

  // (0,0) to the reentrant corner (6,4) stays inside and splits at (3,2).
  const p2t::CDT::ConstraintId id =
    cdt.InsertConstraint(&outline_points[0], &outline_points[3]);
  UNIT_CHECK(id != 0);
  UNIT_CHECK(has_edge(cdt, &outline_points[0], &steiner));
  UNIT_CHECK(has_edge(cdt, &steiner, &outline_points[3]));
  UNIT_CHECK(edge_is_constrained(cdt, &outline_points[0], &steiner));
  UNIT_CHECK(edge_is_constrained(cdt, &steiner, &outline_points[3]));
  UNIT_CHECK(!has_edge(cdt, &outline_points[0], &outline_points[3]));
  UNIT_CHECK(adjacency_reciprocal(cdt));
  UNIT_CHECK(local_delaunay(cdt));
  UNIT_CHECK(one_connected_component(cdt));
}

// =========================== ownership group ===============================

UNIT_CASE(ownership, overlapping_and_reversed_lines_keep_independent_ids)
{
  std::vector<p2t::Point> square{
    {0, 0}, {8, 0}, {8, 8}, {0, 8}};
  std::vector<p2t::Point> steiner{
    {4, 2}, {4, 6}, {2, 4}, {6, 4}};
  std::vector<p2t::Point*> outline;
  for (p2t::Point& p : square) {
    outline.push_back(&p);
  }
  p2t::CDT cdt(outline);
  for (p2t::Point& p : steiner) {
    cdt.AddPoint(&p);
  }
  cdt.Triangulate();

  const p2t::CDT::ConstraintId forward =
    cdt.InsertConstraint(&steiner[0], &steiner[1]); // (4,2)-(4,6)
  const p2t::CDT::ConstraintId reversed =
    cdt.InsertConstraint(&steiner[1], &steiner[0]); // same line, reversed
  UNIT_CHECK(forward != 0 && reversed != 0);
  UNIT_CHECK(forward != reversed);

  UNIT_CHECK(edge_is_constrained(cdt, &steiner[0], &steiner[1]));
  UNIT_CHECK(cdt.RemoveConstraint(forward));
  // The reversed id still holds the shared sub-edge.
  UNIT_CHECK(edge_is_constrained(cdt, &steiner[0], &steiner[1]));
  UNIT_CHECK(cdt.RemoveConstraint(reversed));
  // With both ids gone the crease flag is released.
  UNIT_CHECK(!edge_is_constrained(cdt, &steiner[0], &steiner[1]));
  UNIT_CHECK(adjacency_reciprocal(cdt));
  UNIT_CHECK(local_delaunay(cdt));
}

UNIT_CASE(ownership, shared_subedge_survives_until_last_id_removed)
{
  DiagonalPatch patch;
  const p2t::CDT::ConstraintId first =
    patch.cdt.InsertConstraint(&patch.outer[0], &patch.outer[2]);
  const p2t::CDT::ConstraintId second =
    patch.cdt.InsertConstraint(&patch.outer[0], &patch.outer[2]);
  UNIT_CHECK(first != 0 && second != 0 && first != second);

  // The shared sub-edge (3,3)-(5,5) is referenced by both ids.
  UNIT_CHECK(patch.cdt.RemoveConstraint(first));
  UNIT_CHECK(edge_is_constrained(patch.cdt,
                                 &patch.steiner[0], &patch.steiner[4]));
  UNIT_CHECK(patch.cdt.RemoveConstraint(second));
  UNIT_CHECK(!edge_is_constrained(patch.cdt,
                                  &patch.steiner[0], &patch.steiner[4]));
  UNIT_CHECK(adjacency_reciprocal(patch.cdt));
  UNIT_CHECK(local_delaunay(patch.cdt));
  UNIT_CHECK(one_connected_component(patch.cdt));
}

UNIT_CASE(ownership, boundary_edges_are_never_released)
{
  DiagonalPatch patch;
  const int boundary_before = count_constrained_flags(patch.cdt);
  UNIT_CHECK(boundary_before == 4); // four outer contour sides

  const p2t::CDT::ConstraintId id =
    patch.cdt.InsertConstraint(&patch.outer[0], &patch.outer[1]);
  UNIT_CHECK(id != 0);
  UNIT_CHECK(edge_is_constrained(patch.cdt,
                                 &patch.outer[0], &patch.outer[1]));
  UNIT_CHECK(patch.cdt.RemoveConstraint(id));
  // Removing a user id must not strip the permanent boundary protection.
  UNIT_CHECK(edge_is_constrained(patch.cdt,
                                 &patch.outer[0], &patch.outer[1]));
  UNIT_CHECK(count_constrained_flags(patch.cdt) >= boundary_before);

  // Unknown, zero and double-removed ids are rejected without side effects.
  UNIT_CHECK(!patch.cdt.RemoveConstraint(0));
  UNIT_CHECK(!patch.cdt.RemoveConstraint(999999));
  UNIT_CHECK(!patch.cdt.RemoveConstraint(id));
  UNIT_CHECK(adjacency_reciprocal(patch.cdt));
}

// ============================= session group ===============================

UNIT_CASE(session, rejected_line_can_be_followed_by_valid_edits)
{
  std::vector<p2t::Point> square{
    {0, 0}, {8, 0}, {8, 8}, {0, 8}};
  std::vector<p2t::Point> steiner{
    {4, 2}, {4, 6}, {2, 4}, {6, 4}};
  std::vector<p2t::Point*> outline;
  for (p2t::Point& p : square) {
    outline.push_back(&p);
  }
  p2t::CDT cdt(outline);
  for (p2t::Point& p : steiner) {
    cdt.AddPoint(&p);
  }
  cdt.Triangulate();

  const p2t::CDT::ConstraintId vertical =
    cdt.InsertConstraint(&steiner[0], &steiner[1]);
  UNIT_CHECK(vertical != 0);
  const auto print_before = fingerprint(cdt);

  // Horizontal crease crosses the protected vertical one away from a vertex.
  UNIT_CHECK(cdt.InsertConstraint(&steiner[2], &steiner[3]) == 0);
  UNIT_CHECK(fingerprint(cdt) == print_before);

  // Editing continues normally after the rejection.
  const p2t::CDT::ConstraintId next =
    cdt.InsertConstraint(&square[0], &steiner[0]);
  UNIT_CHECK(next != 0);
  UNIT_CHECK(edge_is_constrained(cdt, &square[0], &steiner[0]) ||
             has_edge(cdt, &square[0], &steiner[0]));
  UNIT_CHECK(adjacency_reciprocal(cdt));
  UNIT_CHECK(local_delaunay(cdt));
  UNIT_CHECK(one_connected_component(cdt));
}

UNIT_CASE(session, consecutive_adjacent_edits_preserve_all_invariants)
{
  std::vector<p2t::Point> square{
    {0, 0}, {8, 0}, {8, 8}, {0, 8}};
  std::vector<p2t::Point> steiner{
    {4, 2}, {4, 6}, {2, 4}, {6, 4}};
  std::vector<p2t::Point*> outline;
  for (p2t::Point& p : square) {
    outline.push_back(&p);
  }
  p2t::CDT cdt(outline);
  for (p2t::Point& p : steiner) {
    cdt.AddPoint(&p);
  }
  cdt.Triangulate();
  const double area_before = total_area(cdt);

  // Lines meet only at existing vertices, so every insertion is accepted.
  std::vector<p2t::CDT::ConstraintId> ids;
  ids.push_back(cdt.InsertConstraint(&steiner[0], &steiner[1]));
  ids.push_back(cdt.InsertConstraint(&square[0], &steiner[0]));
  ids.push_back(cdt.InsertConstraint(&steiner[1], &square[3]));
  for (p2t::CDT::ConstraintId id : ids) {
    UNIT_CHECK(id != 0);
  }
  UNIT_CHECK(adjacency_reciprocal(cdt));
  UNIT_CHECK(constrained_flags_consistent(cdt));
  UNIT_CHECK(local_delaunay(cdt));
  UNIT_CHECK(one_connected_component(cdt));
  UNIT_CHECK(std::fabs(total_area(cdt) - area_before) < 1e-9);

  for (p2t::CDT::ConstraintId id : ids) {
    UNIT_CHECK(cdt.RemoveConstraint(id));
  }
  UNIT_CHECK(adjacency_reciprocal(cdt));
  UNIT_CHECK(local_delaunay(cdt));
  UNIT_CHECK(one_connected_component(cdt));
  UNIT_CHECK(std::fabs(total_area(cdt) - area_before) < 1e-9);
}

UNIT_CASE(session, triangulation_without_edits_keeps_legacy_behavior)
{
  // No constraint-editing call at all: the original one-shot workflow must
  // behave exactly as before, including ownership of every input point.
  std::vector<p2t::Point> outer{
    {0, 0}, {12, 0}, {12, 10}, {0, 10}};
  std::vector<p2t::Point> hole{
    {5, 4}, {5, 6}, {7, 6}, {7, 4}};
  std::vector<p2t::Point> handles{
    {2, 2}, {4, 2}, {8, 2}, {10, 2}, {2, 8}, {10, 8}};
  std::vector<p2t::Point*> outline;
  std::vector<p2t::Point*> holeline;
  std::set<p2t::Point*> expected;
  for (p2t::Point& p : outer) {
    outline.push_back(&p);
    expected.insert(&p);
  }
  for (p2t::Point& p : hole) {
    holeline.push_back(&p);
    expected.insert(&p);
  }
  for (p2t::Point& p : handles) {
    expected.insert(&p);
  }

  p2t::CDT cdt(outline);
  cdt.AddHole(holeline);
  for (p2t::Point& p : handles) {
    cdt.AddPoint(&p);
  }
  cdt.Triangulate();

  UNIT_CHECK(cdt.GetTriangles().size() == 20);
  UNIT_CHECK(std::fabs(total_area(cdt) - 116.0) < 1e-9);
  UNIT_CHECK(count_constrained_flags(cdt) == 8);
  UNIT_CHECK(!cdt.GetMap().empty());
  UNIT_CHECK(adjacency_reciprocal(cdt));
  UNIT_CHECK(local_delaunay(cdt));
  UNIT_CHECK(one_connected_component(cdt));

  std::set<p2t::Point*> used;
  for (p2t::Triangle* t : cdt.GetTriangles()) {
    for (int i = 0; i < 3; ++i) {
      used.insert(t->GetPoint(i));
    }
  }
  UNIT_CHECK(used == expected);

  // Editing before Triangulate, with null/foreign/equal endpoints, is
  // rejected and cannot disturb the finished mesh.
  p2t::Point foreign(1, 1);
  UNIT_CHECK(cdt.InsertConstraint(nullptr, &outer[0]) == 0);
  UNIT_CHECK(cdt.InsertConstraint(&foreign, &outer[0]) == 0);
  UNIT_CHECK(cdt.InsertConstraint(&outer[0], &outer[0]) == 0);
  UNIT_CHECK(!cdt.RemoveConstraint(1));
}
