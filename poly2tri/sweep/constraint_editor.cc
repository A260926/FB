/*
 * Poly2Tri Copyright (c) 2009-2024, Poly2Tri Contributors
 * https://github.com/jhasse/poly2tri
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * * Neither the name of Poly2Tri nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without specific
 *   prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "constraint_editor.h"
#include "sweep_context.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace p2t {

namespace {

double DirectionCross(const Point& origin, const Point& a, const Point& b)
{
  return (a.x - origin.x) * (b.y - origin.y) -
         (a.y - origin.y) * (b.x - origin.x);
}

int ExactSign(double value)
{
  if (std::fpclassify(value) == FP_ZERO) {
    return 0;
  }
  return value < 0.0 ? -1 : 1;
}

/// Strict incircle predicate.  Returns a positive value when d is inside the
/// circumcircle of the CCW triangle a,b,c, zero on the circle.
double IncircleDeterminant(const Point& a, const Point& b,
                           const Point& c, const Point& d)
{
  const Point* p1 = &a;
  const Point* p2 = &b;
  const Point* p3 = &c;
  if (DirectionCross(*p1, *p2, *p3) < 0.0) {
    std::swap(p2, p3);
  }

  const double adx = p1->x - d.x;
  const double ady = p1->y - d.y;
  const double bdx = p2->x - d.x;
  const double bdy = p2->y - d.y;
  const double cdx = p3->x - d.x;
  const double cdy = p3->y - d.y;

  const double adxbdy = adx * bdy;
  const double bdxady = bdx * ady;
  const double oabd = adxbdy - bdxady;

  const double cdxady = cdx * ady;
  const double adxcdy = adx * cdy;
  const double ocad = cdxady - adxcdy;

  const double bdxcdy = bdx * cdy;
  const double cdxbdy = cdx * bdy;

  const double alift = adx * adx + ady * ady;
  const double blift = bdx * bdx + bdy * bdy;
  const double clift = cdx * cdx + cdy * cdy;

  return alift * (bdxcdy - cdxbdy) + blift * ocad + clift * oabd;
}

/// Description of where the ray anchor -> b leaves a triangle.
struct Step {
  enum Kind { NONE, INTERIOR, VERTEX, COLLINEAR };
  Kind kind = NONE;
  int edge_index = -1;
  Point* point = nullptr;
  double t = 0.0;
};

} // namespace

ConstraintEditor::ConstraintEditor(SweepContext& context)
  : context_(context), next_id_(1)
{
  for (Triangle* triangle : context_.GetTriangles()) {
    for (int i = 0; i < 3; ++i) {
      Point* p = triangle->GetPoint(i);
      managed_points_.insert(p);
      vertex_triangles_[p].push_back(triangle);
      if (triangle->constrained_edge[i]) {
        permanent_edges_.insert(
          EdgeKey(triangle->GetPoint((i + 1) % 3),
                  triangle->GetPoint((i + 2) % 3)));
      }
    }
  }
}

bool ConstraintEditor::IsManagedPoint(const Point* point) const
{
  return point != nullptr && managed_points_.count(const_cast<Point*>(point)) != 0;
}

bool ConstraintEditor::HasMeshEdge(Point* a, Point* b) const
{
  auto it = vertex_triangles_.find(a);
  if (it == vertex_triangles_.end()) {
    return false;
  }
  for (Triangle* triangle : it->second) {
    if (triangle->Contains(a, b)) {
      return true;
    }
  }
  return false;
}

/// Parametric segment traversal used by the pre-flip validation walk, where
/// the anchor point need not be a vertex of the current triangle.  Finds the
/// first edge met by the ray anchor -> b, ignoring the edge leading back to
/// `entry`.
static Step FindStep(Triangle& triangle, const Point& anchor,
                     const Point& b, const Triangle* entry)
{
  const double dx = b.x - anchor.x;
  const double dy = b.y - anchor.y;
  const double dd = dx * dx + dy * dy;
  const double tolerance = 1e-12 * std::max(1.0, std::sqrt(dd));

  Step best;

  for (int i = 0; i < 3; ++i) {
    if (entry != nullptr && triangle.GetNeighbor(i) == entry) {
      continue;
    }
    Point* u = triangle.GetPoint((i + 1) % 3);
    Point* v = triangle.GetPoint((i + 2) % 3);

    const double ex = v->x - u->x;
    const double ey = v->y - u->y;
    const double denom = dx * ey - dy * ex;

    if (ExactSign(denom) != 0) {
      const double ux = u->x - anchor.x;
      const double uy = u->y - anchor.y;
      const double t = (ux * ey - uy * ex) / denom;
      const double s = (ux * dy - uy * dx) / denom;
      if (t <= tolerance || t > 1.0 + tolerance) {
        continue;
      }
      if (s < -tolerance || s > 1.0 + tolerance) {
        continue;
      }

      Step step;
      step.edge_index = i;
      step.t = t;
      if (std::fabs(s) <= tolerance) {
        step.kind = Step::VERTEX;
        step.point = u;
      } else if (std::fabs(s - 1.0) <= tolerance) {
        step.kind = Step::VERTEX;
        step.point = v;
      } else {
        step.kind = Step::INTERIOR;
      }

      if (best.kind == Step::NONE || step.t < best.t) {
        best = step;
      }
    } else if (ExactSign(DirectionCross(anchor, *u, b)) == 0) {
      // Parallel and collinear: follow the edge endpoint ahead of the anchor.
      const double tu = ((u->x - anchor.x) * dx + (u->y - anchor.y) * dy) / dd;
      const double tv = ((v->x - anchor.x) * dx + (v->y - anchor.y) * dy) / dd;
      double forward = -1.0;
      Point* w = nullptr;
      if (tu > tolerance && tu <= 1.0 + tolerance &&
          (w == nullptr || tu < forward)) {
        forward = tu;
        w = u;
      }
      if (tv > tolerance && tv <= 1.0 + tolerance &&
          (w == nullptr || tv < forward)) {
        forward = tv;
        w = v;
      }
      if (w == nullptr) {
        continue;
      }

      Step step;
      step.kind = Step::COLLINEAR;
      step.edge_index = i;
      step.point = w;
      step.t = forward;
      if (best.kind == Step::NONE || step.t < best.t) {
        best = step;
      }
    }
  }

  return best;
}

/// Where the ray vertex -> target leaves a triangle that has `vertex` as a
/// vertex.  Uses only orientation signs, so the answer is exact: the ray
/// either hits the opposite edge in its interior or passes through one of
/// its endpoints.
static Step RayExit(Triangle& triangle, const Point& vertex,
                    const Point& target)
{
  const int k = triangle.Index(const_cast<Point*>(&vertex));
  Point* x = triangle.GetPoint((k + 1) % 3);
  Point* y = triangle.GetPoint((k + 2) % 3);

  // The interior cone at vertex is bounded by the rays vertex->x and
  // vertex->y.  s_x / s_y are the signs of cross(ray, edge ray).
  const int s_x = ExactSign(DirectionCross(vertex, target, *x));
  const int s_y = ExactSign(DirectionCross(vertex, target, *y));
  const int oriented =
    ExactSign(DirectionCross(vertex, *x, *y));
  const double ray_x = (target.x - vertex.x) * (x->x - vertex.x) +
                       (target.y - vertex.y) * (x->y - vertex.y);
  const double ray_y = (target.x - vertex.x) * (y->x - vertex.x) +
                       (target.y - vertex.y) * (y->y - vertex.y);
  const bool forward_x = ray_x > 0.0;
  const bool forward_y = ray_y > 0.0;

  Step step;
  if (oriented > 0) {
    // CCW triangle: cone is cross(ray,x) <= 0 and cross(ray,y) >= 0.
    if (s_x > 0 || s_y < 0) {
      return step; // not inside this cone
    }
    if (s_x == 0 && forward_x) {
      step.kind = Step::VERTEX;
      step.point = x;
    } else if (s_y == 0 && forward_y) {
      step.kind = Step::VERTEX;
      step.point = y;
    } else {
      step.kind = Step::INTERIOR;
    }
  } else {
    // CW triangle: mirrored signs.
    if (s_x < 0 || s_y > 0) {
      return step;
    }
    if (s_x == 0 && forward_x) {
      step.kind = Step::VERTEX;
      step.point = x;
    } else if (s_y == 0 && forward_y) {
      step.kind = Step::VERTEX;
      step.point = y;
    } else {
      step.kind = Step::INTERIOR;
    }
  }

  if (step.kind == Step::INTERIOR) {
    // The edge opposite vertex; locate its index in the triangle's own
    // numbering so callers can fetch the neighbor.
    for (int i = 0; i < 3; ++i) {
      if (triangle.GetPoint(i) == const_cast<Point*>(&vertex)) {
        step.edge_index = i;
        break;
      }
    }
  }
  return step;
}

Triangle* ConstraintEditor::SelectFanTriangle(Point* vertex,
                                              const Point* target,
                                              Triangle* avoid)
{
  auto fan = vertex_triangles_.find(vertex);
  if (fan == vertex_triangles_.end()) {
    return nullptr;
  }
  Triangle* fallback = nullptr;
  for (Triangle* candidate : fan->second) {
    if (candidate == avoid) {
      continue;
    }
    const Step step = RayExit(*candidate, *vertex, *target);
    if (step.kind == Step::INTERIOR) {
      return candidate;
    }
    if (step.kind == Step::VERTEX && fallback == nullptr) {
      fallback = candidate;
    }
  }
  return fallback;
}

bool ConstraintEditor::BuildPlan(Point* a, Point* b, Plan& plan)
{
  if (vertex_triangles_.count(a) == 0) {
    return false;
  }

  plan.chain.clear();

  // Build the collinear chain by scanning every managed point: any point
  // exactly on the open segment a-b becomes an intermediate chain vertex.
  // The resulting sub-segments therefore contain no mesh vertex in their
  // relative interior, which is what makes every later edge flip convex.
  const double dx = b->x - a->x;
  const double dy = b->y - a->y;
  const double length2 = dx * dx + dy * dy;
  std::vector<std::pair<double, Point*>> on_line;
  on_line.push_back({0.0, a});
  on_line.push_back({1.0, b});
  for (Point* p : managed_points_) {
    if (p == a || p == b) {
      continue;
    }
    if (ExactSign(DirectionCross(*a, *b, *p)) != 0) {
      continue;
    }
    const double dot = (p->x - a->x) * dx + (p->y - a->y) * dy;
    if (dot > 0.0 && dot < length2) {
      on_line.push_back({dot / length2, p});
    }
  }
  std::sort(on_line.begin(), on_line.end(),
            [](const std::pair<double, Point*>& x,
               const std::pair<double, Point*>& y) {
              return x.first < y.first;
            });
  for (const auto& entry : on_line) {
    if (plan.chain.empty() || plan.chain.back() != entry.second) {
      plan.chain.push_back(entry.second);
    }
  }

  // Validate each vertex-free sub-segment by walking the current mesh with
  // the parametric (anchor-independent) traversal.  The sub-segment contains
  // no mesh vertex, so the walk anchor stays fixed at `from`.  It must stay
  // inside the filled region and may only cross unprotected, flippable
  // interior edges.  Nothing is mutated here.
  for (std::size_t k = 0; k + 1 < plan.chain.size(); ++k) {
    Point* from = plan.chain[k];
    Point* to = plan.chain[k + 1];

    Triangle* current = SelectFanTriangle(from, to, nullptr);
    if (current == nullptr) {
      return false;
    }
    Triangle* back = nullptr;
    std::unordered_set<Triangle*> seen;
    seen.insert(current);

    while (true) {
      const Step step = FindStep(*current, *from, *to, back);
      if (step.kind == Step::NONE) {
        return false;
      }
      if (step.kind == Step::INTERIOR) {
        if (current->constrained_edge[step.edge_index] ||
            !IsFlippable(*current, step.edge_index)) {
          return false;
        }
        Triangle* next = current->GetNeighbor(step.edge_index);
        if (next == nullptr) {
          return false;
        }
        back = current;
        current = next;
        if (!seen.insert(current).second) {
          return false;
        }
        continue;
      }
      // A vertex-free sub-segment can only finish at its own endpoint.
      if (step.point != to) {
        return false;
      }
      break;
    }
  }

  return true;
}

bool ConstraintEditor::IsFlippable(Triangle& triangle, int edge_index) const
{
  Triangle* other = triangle.GetNeighbor(edge_index);
  if (other == nullptr) {
    return false;
  }
  Point* e1 = triangle.GetPoint((edge_index + 1) % 3);
  Point* e2 = triangle.GetPoint((edge_index + 2) % 3);
  const int other_index = other->EdgeIndex(e1, e2);
  if (other_index < 0) {
    return false;
  }
  Point* p = triangle.GetPoint(edge_index);
  Point* op = other->GetPoint(other_index);
  // Legal flip of shared edge e1-e2 into diagonal p-op requires:
  //  1. the two triangles lie on opposite sides of e1-e2 (convex union);
  //  2. the new diagonal p-op does not pass through e1 or e2, which would
  //     produce a degenerate zero-area face.
  // In a valid planar triangulation no other mesh vertex can lie inside the
  // two-triangle convex quad (it would have to be a vertex of those faces),
  // so checking the four corners is sufficient.
  const int edge_side_p = ExactSign(DirectionCross(*e1, *e2, *p));
  const int edge_side_op = ExactSign(DirectionCross(*e1, *e2, *op));
  const int diag_side_1 = ExactSign(DirectionCross(*p, *op, *e1));
  const int diag_side_2 = ExactSign(DirectionCross(*p, *op, *e2));
  return edge_side_p != 0 && edge_side_op != 0 &&
         edge_side_p != edge_side_op &&
         diag_side_1 != 0 && diag_side_2 != 0 &&
         diag_side_1 != diag_side_2;
}

void ConstraintEditor::FlipEdge(Triangle& triangle, int edge_index)
{
  Point* p = triangle.GetPoint(edge_index);
  Point* e1 = triangle.GetPoint((edge_index + 1) % 3);
  Point* e2 = triangle.GetPoint((edge_index + 2) % 3);

  Triangle* other = triangle.GetNeighbor(edge_index);
  if (other == nullptr) {
    throw std::runtime_error("ConstraintEditor: cannot flip boundary edge");
  }
  const int other_index = other->EdgeIndex(e1, e2);
  if (other_index < 0) {
    throw std::runtime_error("ConstraintEditor: inconsistent triangle adjacency");
  }
  Point* op = other->GetPoint(other_index);

  Triangle* n1 = triangle.NeighborCCW(*p);
  Triangle* n2 = triangle.NeighborCW(*p);
  Triangle* n3 = other->NeighborCCW(*op);
  Triangle* n4 = other->NeighborCW(*op);

  const bool ce1 = triangle.GetConstrainedEdgeCCW(*p);
  const bool ce2 = triangle.GetConstrainedEdgeCW(*p);
  const bool ce3 = other->GetConstrainedEdgeCCW(*op);
  const bool ce4 = other->GetConstrainedEdgeCW(*op);

  const bool de1 = triangle.GetDelunayEdgeCCW(*p);
  const bool de2 = triangle.GetDelunayEdgeCW(*p);
  const bool de3 = other->GetDelunayEdgeCCW(*op);
  const bool de4 = other->GetDelunayEdgeCW(*op);

  triangle.Legalize(*p, *op);
  other->Legalize(*op, *p);

  other->SetDelunayEdgeCCW(*p, de1);
  triangle.SetDelunayEdgeCW(*p, de2);
  triangle.SetDelunayEdgeCCW(*op, de3);
  other->SetDelunayEdgeCW(*op, de4);

  other->SetConstrainedEdgeCCW(*p, ce1);
  triangle.SetConstrainedEdgeCW(*p, ce2);
  triangle.SetConstrainedEdgeCCW(*op, ce3);
  other->SetConstrainedEdgeCW(*op, ce4);

  triangle.ClearNeighbors();
  other->ClearNeighbors();
  if (n1 != nullptr) {
    other->MarkNeighbor(*n1);
  }
  if (n2 != nullptr) {
    triangle.MarkNeighbor(*n2);
  }
  if (n3 != nullptr) {
    triangle.MarkNeighbor(*n3);
  }
  if (n4 != nullptr) {
    other->MarkNeighbor(*n4);
  }
  triangle.MarkNeighbor(*other);

  // Keep the point -> incident triangle buckets current.
  for (Point* v : {p, op, e1, e2}) {
    auto& bucket = vertex_triangles_[v];
    bucket.erase(std::remove(bucket.begin(), bucket.end(), &triangle),
                 bucket.end());
    bucket.erase(std::remove(bucket.begin(), bucket.end(), other),
                 bucket.end());
    if (triangle.Contains(v)) {
      bucket.push_back(&triangle);
    }
    if (other->Contains(v)) {
      bucket.push_back(other);
    }
  }
}

void ConstraintEditor::ForceEdge(Point* a, Point* b)
{
  if (HasMeshEdge(a, b)) {
    return;
  }

  // a-b is one piece of a split chain, hence no mesh vertex lies in its
  // relative interior.  Use exactly the same parametric traversal as the
  // non-mutating BuildPlan.  After a flip the new diagonal is incident to a,
  // which the segment a-b can never cross, so restart the walk from a: each
  // flip removes one crossed edge and adds an a-incident edge, reducing the
  // number of crossings until a-b exists.  BuildPlan already proved every
  // crossed edge unprotected and flippable, and flips only ever replace a
  // crossing with another flippable one, so no exception is expected here.
  const std::size_t guard = context_.GetTriangles().size() * 4 + 16;
  std::size_t iterations = 0;

  while (!HasMeshEdge(a, b)) {
    if (++iterations > guard) {
      throw std::runtime_error(
        "ConstraintEditor: constraint insertion did not converge");
    }

    Triangle* current = SelectFanTriangle(a, b, nullptr);
    if (current == nullptr) {
      throw std::runtime_error(
        "ConstraintEditor: no incident triangle at constraint endpoint");
    }
    Triangle* back = nullptr;

    while (true) {
      const Step step = FindStep(*current, *a, *b, back);
      if (step.kind == Step::NONE) {
        throw std::runtime_error(
          "ConstraintEditor: lost the constraint line while inserting");
      }
      if (step.kind == Step::INTERIOR) {
        if (current->constrained_edge[step.edge_index] ||
            !IsFlippable(*current, step.edge_index)) {
          throw std::runtime_error(
            "ConstraintEditor: validated line hits a non-flippable edge");
        }
        Triangle* other = current->GetNeighbor(step.edge_index);
        if (other == nullptr) {
          throw std::runtime_error(
            "ConstraintEditor: validated line leaves the mesh");
        }
        FlipEdge(*current, step.edge_index);
        break;
      }
      // A vertex-free sub-segment can only finish at its own endpoint b.
      if (step.point != b) {
        throw std::runtime_error(
          "ConstraintEditor: unexpected vertex inside a chain sub-segment");
      }
      return;
    }
  }
}

void ConstraintEditor::MarkConstrained(Point* a, Point* b)
{
  for (Triangle* triangle : vertex_triangles_[a]) {
    if (triangle->Contains(a, b)) {
      triangle->MarkConstrainedEdge(a, b);
    }
  }
}

void ConstraintEditor::ClearConstrained(Point* a, Point* b)
{
  for (Triangle* triangle : vertex_triangles_[a]) {
    const int index = triangle->EdgeIndex(a, b);
    if (index >= 0) {
      triangle->constrained_edge[index] = false;
    }
  }
}

void ConstraintEditor::LegalizeRegion()
{
  // Fixed-point edge-flip relaxation over the whole mesh.  Forcing a crease
  // can leave an illegal edge a few adjacency hops away, across edges that
  // are themselves already legal and therefore never flipped in a local
  // pass.  Each pass scans every interior, unconstrained edge and flips the
  // illegal ones; the mesh starts Delaunay, so triangles outside the
  // constraint's cavity pass the empty-circle test untouched and keep their
  // vertex triples.  A strict incircle flip strictly reduces the total
  // squared edge length, so the number of flips is finite and the process
  // converges on the constrained Delaunay triangulation; co-circular quads
  // (determinant == 0) keep either legal diagonal.  Constrained edges are
  // never flipped, preserving every active crease and boundary.
  const std::size_t triangle_count = context_.GetTriangles().size();
  std::size_t flip_budget =
    triangle_count * triangle_count * triangle_count + 1024;

  for (std::size_t pass = 0; pass < triangle_count + 4; ++pass) {
    bool flipped_any = false;
    for (Triangle* triangle : context_.GetTriangles()) {
      for (int i = 0; i < 3; ++i) {
        if (triangle->constrained_edge[i]) {
          continue;
        }
        Triangle* other = triangle->GetNeighbor(i);
        if (other == nullptr) {
          continue;
        }
        Point* e1 = triangle->GetPoint((i + 1) % 3);
        Point* e2 = triangle->GetPoint((i + 2) % 3);
        const int other_index = other->EdgeIndex(e1, e2);
        if (other_index < 0 || other->constrained_edge[other_index]) {
          continue;
        }
        if (!IsFlippable(*triangle, i) || flip_budget == 0) {
          continue;
        }

        Point* op = other->GetPoint(other_index);
        const int side_a = ExactSign(IncircleDeterminant(
          *triangle->GetPoint(0), *triangle->GetPoint(1),
          *triangle->GetPoint(2), *op));
        if (side_a > 0) {
          FlipEdge(*triangle, i);
          --flip_budget;
          flipped_any = true;
        }
      }
    }
    if (!flipped_any) {
      break;
    }
  }
}

void ConstraintEditor::ApplyPlan(const Plan& plan)
{
  for (std::size_t i = 0; i + 1 < plan.chain.size(); ++i) {
    ForceEdge(plan.chain[i], plan.chain[i + 1]);
  }
}

ConstraintEditor::ConstraintId ConstraintEditor::InsertConstraint(Point* a,
                                                                  Point* b)
{
  if (!IsManagedPoint(a) || !IsManagedPoint(b) || a == b) {
    return 0;
  }

  Plan plan;
  if (!BuildPlan(a, b, plan) || plan.chain.size() < 2) {
    return 0;
  }

  try {
    ApplyPlan(plan);
  } catch (const std::runtime_error&) {
    // BuildPlan validates every crossing up front, so this is only reachable
    // on configurations the input contract does not promise.  Every flip
    // already applied was individually valid (convex, area and adjacency
    // preserving), so restore a fully legal state and report rejection.
    LegalizeRegion();
    return 0;
  }

  const ConstraintId id = next_id_++;
  auto& owned = live_constraints_[id];

  for (std::size_t i = 0; i + 1 < plan.chain.size(); ++i) {
    Point* u = plan.chain[i];
    Point* v = plan.chain[i + 1];
    const EdgeKey key(u, v);
    owned.push_back(key);
    if (permanent_edges_.count(key) == 0) {
      ++edge_refs_[key];
      MarkConstrained(u, v);
    }
  }

  LegalizeRegion();
  return id;
}

bool ConstraintEditor::RemoveConstraint(ConstraintId id)
{
  auto it = live_constraints_.find(id);
  if (id == 0 || it == live_constraints_.end()) {
    return false;
  }

  for (const EdgeKey& key : it->second) {
    auto ref = edge_refs_.find(key);
    if (ref == edge_refs_.end()) {
      // Permanent boundary edge: it stays protected.
      continue;
    }
    if (--ref->second == 0) {
      ClearConstrained(key.a, key.b);
      edge_refs_.erase(ref);
    }
  }

  live_constraints_.erase(it);
  // Re-legalize the whole mesh: a freed crease can make edges illegal some
  // adjacency hops away, and the fixed-point pass leaves every other
  // (already legal) triangle's vertex triple untouched.
  LegalizeRegion();
  return true;
}

} // namespace p2t
