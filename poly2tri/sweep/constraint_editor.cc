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
#include <deque>
#include <set>

namespace p2t {

namespace {

double CrossValue(const Point* a, const Point* b, const Point* c)
{
  return (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
}

int SignOf(double value)
{
  return (value > 0.0) - (value < 0.0);
}

/// Proper crossing: no shared endpoint and each segment separates the
/// endpoints of the other.  Touching and collinear overlap are allowed.
bool ProperlyCross(const Point* a, const Point* b, const Point* c, const Point* d)
{
  const double c1 = CrossValue(a, b, c);
  const double c2 = CrossValue(a, b, d);
  if (c1 == 0.0 || c2 == 0.0 || SignOf(c1) == SignOf(c2))
    return false;
  const double c3 = CrossValue(c, d, a);
  const double c4 = CrossValue(c, d, b);
  if (c3 == 0.0 || c4 == 0.0 || SignOf(c3) == SignOf(c4))
    return false;
  return true;
}

bool OnClosedSegment(const Point* a, const Point* b, const Point* p, double tolerance)
{
  if (p == a || p == b)
    return true;
  if (std::abs(CrossValue(a, b, p)) > tolerance)
    return false;
  const double dot = (p->x - a->x) * (b->x - a->x) + (p->y - a->y) * (b->y - a->y);
  if (dot < -tolerance)
    return false;
  const double length2 = (b->x - a->x) * (b->x - a->x) + (b->y - a->y) * (b->y - a->y);
  return dot <= length2 + tolerance;
}

/// Positive when d lies inside the circle through the CCW triangle (a,b,c).
double InCircleValue(const Point* a, const Point* b, const Point* c, const Point* d)
{
  const double adx = a->x - d->x;
  const double ady = a->y - d->y;
  const double bdx = b->x - d->x;
  const double bdy = b->y - d->y;
  const double cdx = c->x - d->x;
  const double cdy = c->y - d->y;
  const double alift = adx * adx + ady * ady;
  const double blift = bdx * bdx + bdy * bdy;
  const double clift = cdx * cdx + cdy * cdy;
  return alift * (bdx * cdy - cdx * bdy) - blift * (adx * cdy - cdx * ady) +
         clift * (adx * bdy - bdx * ady);
}

} // namespace

ConstraintEditor::ConstraintEditor(SweepContext& context)
    : context_(context), ready_(false), initialized_(false), next_id_(1)
{
}

void ConstraintEditor::MarkTriangulated()
{
  ready_ = true;
  initialized_ = false;
}

ConstraintEditor::EdgeKey ConstraintEditor::Key(const Point* a, const Point* b) const
{
  return EdgeLess()(std::make_pair(a, b), std::make_pair(b, a)) ? std::make_pair(a, b)
                                                               : std::make_pair(b, a);
}

bool ConstraintEditor::IsProtected(const EdgeKey& edge) const
{
  const CountMap::const_iterator it = counts_.find(edge);
  return it != counts_.end() && it->second > 0;
}

void ConstraintEditor::EnsureInitialized()
{
  if (initialized_)
    return;
  initialized_ = true;

  triangles_.clear();
  interior_.clear();
  mesh_points_.clear();
  base_edges_.clear();
  counts_.clear();
  constraints_.clear();

  const std::vector<Triangle*>& mesh = context_.GetTriangles();
  triangles_.assign(mesh.begin(), mesh.end());
  for (Triangle* triangle : triangles_) {
    interior_.insert(triangle);
    for (int i = 0; i < 3; ++i) {
      mesh_points_.insert(triangle->points_[i]);
      if (triangle->constrained_edge[i]) {
        const int j = (i + 1) % 3;
        base_edges_.insert(Key(triangle->points_[j], triangle->points_[(i + 2) % 3]));
      }
    }
  }
  for (const EdgeKey& edge : base_edges_)
    counts_[edge] = 1;
}

ConstraintEditor::Snapshot ConstraintEditor::TakeSnapshot() const
{
  Snapshot snapshot;
  snapshot.counts = counts_;
  snapshot.triangles.reserve(triangles_.size());
  for (Triangle* triangle : triangles_) {
    TriangleState state;
    state.triangle = triangle;
    for (int i = 0; i < 3; ++i) {
      state.points[i] = triangle->points_[i];
      state.neighbors[i] = triangle->neighbors_[i];
      state.constrained[i] = triangle->constrained_edge[i];
      state.delaunay[i] = triangle->delaunay_edge[i];
    }
    snapshot.triangles.push_back(state);
  }
  return snapshot;
}

void ConstraintEditor::Restore(const Snapshot& snapshot)
{
  counts_ = snapshot.counts;
  for (const TriangleState& state : snapshot.triangles) {
    Triangle* triangle = state.triangle;
    for (int i = 0; i < 3; ++i) {
      triangle->points_[i] = state.points[i];
      triangle->neighbors_[i] = state.neighbors[i];
      triangle->constrained_edge[i] = state.constrained[i];
      triangle->delaunay_edge[i] = state.delaunay[i];
    }
  }
}

void ConstraintEditor::SetTriangleFlags(Triangle* triangle)
{
  for (int i = 0; i < 3; ++i) {
    const Point* a = triangle->points_[(i + 1) % 3];
    const Point* b = triangle->points_[(i + 2) % 3];
    triangle->constrained_edge[i] = IsProtected(Key(a, b));
  }
}

void ConstraintEditor::RefreshFlags()
{
  for (Triangle* triangle : triangles_)
    SetTriangleFlags(triangle);
}

std::vector<Point*> ConstraintEditor::BuildChain(Point* a, Point* b) const
{
  const double dx = b->x - a->x;
  const double dy = b->y - a->y;
  const double length2 = dx * dx + dy * dy;
  const double tolerance = 1e-12 * (length2 + 1.0);

  std::vector<std::pair<double, Point*>> on_line;
  on_line.push_back(std::make_pair(0.0, a));
  on_line.push_back(std::make_pair(1.0, b));
  for (const Point* point : mesh_points_) {
    if (point == a || point == b)
      continue;
    if (!OnClosedSegment(a, b, point, tolerance))
      continue;
    const double t = ((point->x - a->x) * dx + (point->y - a->y) * dy) / length2;
    on_line.push_back(std::make_pair(t, const_cast<Point*>(point)));
  }
  std::sort(on_line.begin(), on_line.end(),
            [](const std::pair<double, Point*>& lhs, const std::pair<double, Point*>& rhs) {
              return lhs.first < rhs.first;
            });
  std::vector<Point*> chain;
  chain.reserve(on_line.size());
  for (const auto& entry : on_line) {
    if (chain.empty() || chain.back() != entry.second)
      chain.push_back(entry.second);
  }
  return chain;
}

bool ConstraintEditor::EdgeExists(Point* a, Point* b) const
{
  for (Triangle* triangle : triangles_) {
    if (triangle->EdgeIndex(a, b) != -1)
      return true;
  }
  return false;
}

bool ConstraintEditor::FindEdgeTriangles(const EdgeKey& edge, Triangle*& t1, Triangle*& t2) const
{
  t1 = nullptr;
  t2 = nullptr;
  for (Triangle* triangle : triangles_) {
    if (triangle->EdgeIndex(edge.first, edge.second) == -1)
      continue;
    if (!t1) {
      t1 = triangle;
    } else if (triangle != t1) {
      t2 = triangle;
      break;
    }
  }
  if (t1 && !t2) {
    const int index = t1->EdgeIndex(edge.first, edge.second);
    Triangle* neighbor = t1->neighbors_[index];
    if (neighbor && interior_.count(neighbor) && neighbor->Contains(edge.first, edge.second))
      t2 = neighbor;
  }
  return t1 != nullptr;
}

bool ConstraintEditor::Walk(Point* a, Point* b, std::vector<EdgeKey>& crossed,
                            std::vector<Triangle*>& touched) const
{
  // The entry triangle is the one around `a` that the segment actually
  // enters: either it already contains `b`, or its opposite edge is properly
  // crossed by a->b.  Testing the opposite edge (instead of a fan angle)
  // stays correct when a->b is collinear with one of the spokes at `a`.
  Triangle* entry = nullptr;
  for (Triangle* triangle : triangles_) {
    if (!triangle->Contains(a))
      continue;
    if (triangle->Contains(b)) {
      entry = triangle;
      break;
    }
    const int index = triangle->Index(a);
    const Point* q1 = triangle->points_[(index + 1) % 3];
    const Point* q2 = triangle->points_[(index + 2) % 3];
    if (ProperlyCross(a, b, q1, q2)) {
      if (entry)
        return false; // Degenerate: the segment enters two triangles at once.
      entry = triangle;
    }
  }
  if (!entry)
    return false;

  crossed.clear();
  touched.clear();
  Triangle* current = entry;
  Point* last_first = nullptr;
  Point* last_second = nullptr;

  const size_t limit = 4 * triangles_.size() + 64;
  for (size_t step = 0; step < limit; ++step) {
    touched.push_back(current);
    if (current->Contains(b))
      return true;

    int chosen = -1;
    Point* chosen_a = nullptr;
    Point* chosen_b = nullptr;
    for (int i = 0; i < 3; ++i) {
      Point* p1 = current->points_[(i + 1) % 3];
      Point* p2 = current->points_[(i + 2) % 3];
      if (!ProperlyCross(a, b, p1, p2))
        continue;
      if (last_first && ((p1 == last_first && p2 == last_second) ||
                         (p1 == last_second && p2 == last_first)))
        continue;
      if (chosen != -1)
        return false; // Ambiguous exit edge; treat as an invalid walk.
      chosen = i;
      chosen_a = p1;
      chosen_b = p2;
    }
    if (chosen == -1)
      return false;

    crossed.push_back(Key(chosen_a, chosen_b));
    Triangle* next = current->neighbors_[chosen];
    if (!next || !interior_.count(next) || !next->Contains(chosen_a, chosen_b))
      return false;

    last_first = chosen_a;
    last_second = chosen_b;
    current = next;
  }
  return false;
}

bool ConstraintEditor::FlipEdge(Triangle* t1, Triangle* t2, Point* p, Point* q)
{
  int e1 = t1->EdgeIndex(p, q);
  int e2 = t2->EdgeIndex(p, q);
  if (e1 == -1 || e2 == -1)
    return false;

  // Orient so that r is left of p->q and s is right of it.
  if (CrossValue(p, q, t1->points_[e1]) < 0.0)
    std::swap(p, q);
  e1 = t1->EdgeIndex(p, q);
  e2 = t2->EdgeIndex(p, q);
  if (e1 == -1 || e2 == -1)
    return false;

  Point* r = t1->points_[e1];
  Point* s = t2->points_[e2];
  if (!r || !s || r == s)
    return false;

  // Require a strictly convex quad: p and q must straddle r->s, and r and s
  // must straddle p->q.
  if (!(CrossValue(p, q, r) > 0.0 && CrossValue(p, q, s) < 0.0 &&
        CrossValue(r, s, p) * CrossValue(r, s, q) < 0.0))
    return false;

  // Rotate the point arrays so t1 == [p, q, r] and t2 == [q, p, s].
  const int i1 = t1->Index(p);
  if (t1->points_[(i1 + 1) % 3] != q)
    return false;
  const int i2 = t2->Index(q);
  if (t2->points_[(i2 + 1) % 3] != p)
    return false;

  Triangle* const old_n1[3] = {t1->neighbors_[i1], t1->neighbors_[(i1 + 1) % 3],
                               t1->neighbors_[(i1 + 2) % 3]};
  Triangle* const old_n2[3] = {t2->neighbors_[i2], t2->neighbors_[(i2 + 1) % 3],
                               t2->neighbors_[(i2 + 2) % 3]};
  // old_n1 = {across(q,r), across(r,p), across(p,q)}
  // old_n2 = {across(p,s), across(s,q), across(q,p)}
  Triangle* const nb_qr = old_n1[0];
  Triangle* const nb_rp = old_n1[1];
  Triangle* const nb_ps = old_n2[0];
  Triangle* const nb_sq = old_n2[1];

  // New triangles: [p, s, r] and [q, r, s]; both must stay CCW.
  if (!(CrossValue(p, s, r) > 0.0 && CrossValue(q, r, s) > 0.0))
    return false;

  t1->ClearNeighbors();
  t2->ClearNeighbors();

  t1->points_[0] = p;
  t1->points_[1] = s;
  t1->points_[2] = r;
  t2->points_[0] = q;
  t2->points_[1] = r;
  t2->points_[2] = s;
  for (int i = 0; i < 3; ++i) {
    t1->delaunay_edge[i] = false;
    t2->delaunay_edge[i] = false;
  }

  // MarkNeighbor re-registers both sides, including the outer triangles.
  t1->MarkNeighbor(*t2);
  if (nb_rp)
    t1->MarkNeighbor(*nb_rp);
  if (nb_ps)
    t1->MarkNeighbor(*nb_ps);
  if (nb_sq)
    t2->MarkNeighbor(*nb_sq);
  if (nb_qr)
    t2->MarkNeighbor(*nb_qr);

  SetTriangleFlags(t1);
  SetTriangleFlags(t2);
  return true;
}

bool ConstraintEditor::ForceSegment(Point* a, Point* b, std::vector<Triangle*>& touched)
{
  std::vector<EdgeKey> crossings;
  std::vector<Triangle*> walk_touched;
  if (!Walk(a, b, crossings, walk_touched))
    return false;
  for (Triangle* triangle : walk_touched)
    touched.push_back(triangle);

  // Sloan's algorithm: flip crossing edges one at a time.  If the opposite
  // diagonal of a flipped quad still crosses the target, keep it in the work
  // list; otherwise it is gone for good.  Non-convex quads are retried later.
  const size_t limit = 32 * triangles_.size() + 1024;
  size_t retries = 0;
  for (size_t step = 0; step < limit && !crossings.empty(); ++step) {
    const EdgeKey edge = crossings.front();
    crossings.erase(crossings.begin());

    if (!ProperlyCross(a, b, edge.first, edge.second))
      continue; // Stale entry; the topology already moved this edge away.

    if (IsProtected(edge))
      return false;

    Triangle* t1 = nullptr;
    Triangle* t2 = nullptr;
    if (!FindEdgeTriangles(edge, t1, t2))
      continue; // Stale entry: no triangle carries this edge any more.
    if (!t1 || !t2 || !interior_.count(t1) || !interior_.count(t2))
      return false;

    const int i1 = t1->EdgeIndex(edge.first, edge.second);
    const int i2 = t2->EdgeIndex(edge.first, edge.second);
    Point* const r = t1->points_[i1];
    Point* const s = t2->points_[i2];

    if (!FlipEdge(t1, t2, const_cast<Point*>(edge.first),
                  const_cast<Point*>(edge.second))) {
      // Not flippable yet; try again after the other crossing edges.
      crossings.push_back(edge);
      if (++retries > 4 * triangles_.size() + 64)
        return false;
      continue;
    }
    retries = 0;
    touched.push_back(t1);
    touched.push_back(t2);

    if (ProperlyCross(a, b, r, s))
      crossings.push_back(Key(r, s));

    if (EdgeExists(a, b))
      return true;
  }
  return EdgeExists(a, b);
}

bool ConstraintEditor::InsertSubsegment(Point* a, Point* b, std::vector<Triangle*>& touched)
{
  if (!EdgeExists(a, b) && !ForceSegment(a, b, touched))
    return false;
  if (!EdgeExists(a, b))
    return false;
  ++counts_[Key(a, b)];
  return true;
}

void ConstraintEditor::LegalizeRegion(std::vector<Triangle*> seeds)
{
  std::deque<Triangle*> queue;
  std::set<Triangle*> queued;
  for (Triangle* seed : seeds) {
    if (!seed || !interior_.count(seed) || !queued.insert(seed).second)
      continue;
    queue.push_back(seed);
    for (int i = 0; i < 3; ++i) {
      Triangle* neighbor = seed->neighbors_[i];
      if (neighbor && interior_.count(neighbor) && queued.insert(neighbor).second)
        queue.push_back(neighbor);
    }
  }

  const size_t limit = 64 * triangles_.size() + 4096;
  size_t flips = 0;
  while (!queue.empty() && flips < limit) {
    Triangle* triangle = queue.front();
    queue.pop_front();
    queued.erase(triangle);
    if (!interior_.count(triangle))
      continue;

    for (int i = 0; i < 3; ++i) {
      const Point* p = triangle->points_[i];
      Point* p1 = triangle->points_[(i + 1) % 3];
      Point* p2 = triangle->points_[(i + 2) % 3];
      if (IsProtected(Key(p1, p2)))
        continue;
      Triangle* neighbor = triangle->neighbors_[i];
      if (!neighbor || !interior_.count(neighbor))
        continue;
      const int e2 = neighbor->EdgeIndex(p1, p2);
      if (e2 == -1)
        continue;
      Point* opposite = neighbor->points_[e2];
      if (!opposite || opposite == p)
        continue;

      // Only a strictly convex quad is flippable.
      if (!(CrossValue(p1, p2, p) * CrossValue(p1, p2, opposite) < 0.0 &&
            CrossValue(p, opposite, p1) * CrossValue(p, opposite, p2) < 0.0))
        continue;

      const double det = InCircleValue(p1, p2, p, opposite);
      const double adx = p1->x - opposite->x;
      const double ady = p1->y - opposite->y;
      const double bdx = p2->x - opposite->x;
      const double bdy = p2->y - opposite->y;
      const double cdx = p->x - opposite->x;
      const double cdy = p->y - opposite->y;
      const double scale = adx * adx + ady * ady + bdx * bdx + bdy * bdy + cdx * cdx + cdy * cdy;
      if (det <= 1e-12 * scale * scale)
        continue;

      if (!FlipEdge(triangle, neighbor, p1, p2))
        continue;
      ++flips;
      Triangle* affected[2] = {triangle, neighbor};
      for (Triangle* changed : affected) {
        if (!changed || !interior_.count(changed) || !queued.insert(changed).second)
          continue;
        queue.push_back(changed);
        for (int k = 0; k < 3; ++k) {
          Triangle* around = changed->neighbors_[k];
          if (around && interior_.count(around) && queued.insert(around).second)
            queue.push_back(around);
        }
      }
      break; // Restart the edge scan for this triangle.
    }
  }
}

ConstraintEditor::ConstraintId ConstraintEditor::Insert(Point* a, Point* b)
{
  if (!ready_ || !a || !b || a == b)
    return 0;
  EnsureInitialized();
  if (!mesh_points_.count(a) || !mesh_points_.count(b))
    return 0;

  const std::vector<Point*> chain = BuildChain(a, b);
  if (chain.size() < 2)
    return 0;

  // Reject before touching anything when any sub-segment properly crosses an
  // already protected edge (outer boundary, hole, or an existing constraint).
  for (size_t i = 0; i + 1 < chain.size(); ++i) {
    for (const auto& entry : counts_) {
      if (entry.second <= 0)
        continue;
      if (ProperlyCross(chain[i], chain[i + 1], entry.first.first, entry.first.second))
        return 0;
    }
  }

  const Snapshot snapshot = TakeSnapshot();
  std::vector<Triangle*> touched;
  std::vector<EdgeKey> segments;
  for (size_t i = 0; i + 1 < chain.size(); ++i) {
    if (!InsertSubsegment(chain[i], chain[i + 1], touched)) {
      Restore(snapshot);
      return 0;
    }
    segments.push_back(Key(chain[i], chain[i + 1]));
  }

  RefreshFlags();
  LegalizeRegion(touched);

  for (const EdgeKey& segment : segments) {
    if (!EdgeExists(const_cast<Point*>(segment.first), const_cast<Point*>(segment.second))) {
      Restore(snapshot);
      return 0;
    }
  }

  Constraint constraint;
  constraint.id = next_id_++;
  constraint.a = a;
  constraint.b = b;
  constraint.segments = segments;
  constraints_.push_back(constraint);
  return constraint.id;
}

bool ConstraintEditor::Remove(ConstraintId id)
{
  if (!ready_ || id == 0)
    return false;
  EnsureInitialized();

  size_t index = constraints_.size();
  for (size_t i = 0; i < constraints_.size(); ++i) {
    if (constraints_[i].id == id) {
      index = i;
      break;
    }
  }
  if (index == constraints_.size())
    return false;

  const Constraint constraint = constraints_[index];
  constraints_.erase(constraints_.begin() + static_cast<std::ptrdiff_t>(index));

  std::vector<Triangle*> seeds;
  for (const EdgeKey& segment : constraint.segments) {
    Triangle* t1 = nullptr;
    Triangle* t2 = nullptr;
    if (FindEdgeTriangles(segment, t1, t2)) {
      if (t1)
        seeds.push_back(t1);
      if (t2)
        seeds.push_back(t2);
    }
    CountMap::iterator it = counts_.find(segment);
    if (it == counts_.end())
      continue;
    if (--it->second <= 0)
      counts_.erase(it);
  }

  RefreshFlags();
  LegalizeRegion(seeds);
  RefreshFlags();
  return true;
}

} // namespace p2t
