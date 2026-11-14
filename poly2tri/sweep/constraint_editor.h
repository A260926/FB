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

#pragma once

#include "../common/shapes.h"

#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace p2t {

class SweepContext;

/**
 * Post-triangulation constraint editing.
 *
 * The editor never allocates or deletes triangles and never revisits the
 * sweep.  A constraint is forced into the existing mesh by flipping the
 * diagonals that cross it (Sloan's incremental algorithm), followed by a
 * local Lawson legalisation of the touched region.  Because only the point
 * arrays and neighbour slots of existing Triangle objects are rewritten,
 * triangle identity, GetTriangles()/GetMap() membership and the vertex
 * objects all stay stable across edits.
 */
class ConstraintEditor {
public:
  using ConstraintId = std::uint64_t;

  explicit ConstraintEditor(SweepContext& context);

  /// Force segment a-b into the mesh.  Returns 0 when the request is invalid.
  ConstraintId Insert(Point* a, Point* b);

  /// Drop the contribution of one constraint id.  Returns false if unknown.
  bool Remove(ConstraintId id);

  /// True once the owning CDT has produced a mesh that can be edited.
  bool ready() const
  {
    return ready_;
  }

  /// Remember that the sweep has completed and an editable mesh now exists.
  void MarkTriangulated();

private:
  using EdgeKey = std::pair<const Point*, const Point*>;
  struct EdgeLess {
    bool operator()(const EdgeKey& a, const EdgeKey& b) const
    {
      const std::less<const Point*> less;
      if (less(a.first, b.first))
        return true;
      if (less(b.first, a.first))
        return false;
      return less(a.second, b.second);
    }
  };
  using CountMap = std::map<EdgeKey, int, EdgeLess>;

  struct Constraint {
    ConstraintId id;
    Point* a;
    Point* b;
    std::vector<EdgeKey> segments;
  };

  /// Complete copy of the mutable triangle state, used to roll a failed
  /// insertion back without leaving a partially edited mesh behind.
  struct TriangleState {
    Triangle* triangle;
    Point* points[3];
    Triangle* neighbors[3];
    bool constrained[3];
    bool delaunay[3];
  };
  struct Snapshot {
    std::vector<TriangleState> triangles;
    CountMap counts;
  };

  SweepContext& context_;
  bool ready_;
  bool initialized_;
  std::uint64_t next_id_;

  std::vector<Triangle*> triangles_;
  std::set<Triangle*> interior_;
  std::set<const Point*> mesh_points_;
  std::set<EdgeKey, EdgeLess> base_edges_;
  CountMap counts_;
  std::vector<Constraint> constraints_;

  void EnsureInitialized();
  void RefreshFlags();
  Snapshot TakeSnapshot() const;
  void Restore(const Snapshot& snapshot);

  EdgeKey Key(const Point* a, const Point* b) const;
  bool IsProtected(const EdgeKey& edge) const;
  bool IsProtected(const Point* a, const Point* b) const
  {
    return IsProtected(Key(a, b));
  }
  void SetTriangleFlags(Triangle* triangle);

  /// All mesh vertices lying on the closed segment a-b, ordered from a to b.
  std::vector<Point*> BuildChain(Point* a, Point* b) const;

  bool EdgeExists(Point* a, Point* b) const;
  bool FindEdgeTriangles(const EdgeKey& edge, Triangle*& t1, Triangle*& t2) const;

  /// Walk the current mesh along a-b, collecting the edges it properly crosses.
  bool Walk(Point* a, Point* b, std::vector<EdgeKey>& crossed,
            std::vector<Triangle*>& touched) const;

  /// Drive the crossing edges out of the segment by flipping.
  bool ForceSegment(Point* a, Point* b, std::vector<Triangle*>& touched);

  bool InsertSubsegment(Point* a, Point* b, std::vector<Triangle*>& touched);

  /// Flip the diagonal shared by t1/t2 to the opposite diagonal.
  bool FlipEdge(Triangle* t1, Triangle* t2, Point* p, Point* q);

  /// Restore the empty-circle property in the neighbourhood of the seeds.
  void LegalizeRegion(std::vector<Triangle*> seeds);
};

} // namespace p2t
