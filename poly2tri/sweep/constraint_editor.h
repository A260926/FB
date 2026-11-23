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
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace p2t {

class SweepContext;

/**
 * Local, in-place editor for the constrained edges of an already finished
 * Constrained Delaunay Triangulation.
 *
 * The editor never moves or adds points, never rebuilds the triangulation and
 * reuses every Triangle object that is not touched by a local flip.  A
 * logical crease line is split at every mesh vertex it passes through; the
 * resulting sub-segments are forced into the mesh by flipping the
 * (unconstrained) diagonals crossed by the line.  Every sub-segment carries a
 * reference count, so several constraint ids may share an edge and the
 * original boundary edges stay protected for the whole life of the CDT.
 */
class ConstraintEditor {
public:

  using ConstraintId = std::uint64_t;

  explicit ConstraintEditor(SweepContext& context);

  /**
   * Force the segment a-b to become a chain of constrained mesh edges.
   *
   * Returns 0 when the endpoints are not two distinct Point objects used by
   * this triangulation, or when the line leaves the filled region, crosses a
   * hole, or properly crosses an already protected edge at a location that is
   * not an existing vertex.  On failure the mesh is left completely
   * untouched.  On success a non-zero id, never reused during the lifetime of
   * this editor, is returned; repeated or reversed calls for the same line
   * get independent ids.
   */
  ConstraintId InsertConstraint(Point* a, Point* b);

  /**
   * Release the contribution of a single id.  Unknown, zero and already
   * removed ids return false.  Sub-edges shared with another live id stay
   * constrained; edges that become free rejoin the local Delaunay
   * legalization.
   */
  bool RemoveConstraint(ConstraintId id);

  bool IsManagedPoint(const Point* point) const;

private:

  struct EdgeKey {
    Point* a;
    Point* b;

    EdgeKey(Point* x, Point* y)
    {
      a = x < y ? x : y;
      b = x < y ? y : x;
    }

    bool operator==(const EdgeKey& other) const
    {
      return a == other.a && b == other.b;
    }
  };

  struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey& key) const
    {
      const std::size_t ha = std::hash<const Point*>{}(key.a);
      const std::size_t hb = std::hash<const Point*>{}(key.b);
      return ha ^ (hb << 1) ^ (hb >> (sizeof(std::size_t) * 8 - 1));
    }
  };

  struct Plan {
    /// Mesh vertices found on the logical line, from the first endpoint to
    /// the last one.  Consecutive entries are the real sub-edges to enforce.
    std::vector<Point*> chain;
  };

  SweepContext& context_;

  /// All Point objects actually used by the finished mesh.
  std::unordered_set<Point*> managed_points_;
  /// Outer contour and hole edges, protected forever.
  std::unordered_set<EdgeKey, EdgeKeyHash> permanent_edges_;
  /// Number of live user ids currently sharing a non-permanent sub-edge.
  std::unordered_map<EdgeKey, unsigned, EdgeKeyHash> edge_refs_;
  /// Sub-edges owned by every live id.
  std::unordered_map<ConstraintId, std::vector<EdgeKey>> live_constraints_;

  ConstraintId next_id_;

  /// Point -> triangles incident to it, kept current across every flip.
  std::unordered_map<Point*, std::vector<Triangle*>> vertex_triangles_;

  bool BuildPlan(Point* a, Point* b, Plan& plan);
  Triangle* SelectFanTriangle(Point* vertex, const Point* target,
                              Triangle* avoid);
  bool IsFlippable(Triangle& triangle, int edge_index) const;
  void ApplyPlan(const Plan& plan);
  void ForceEdge(Point* a, Point* b);
  void FlipEdge(Triangle& triangle, int edge_index);
  void LegalizeRegion();

  bool HasMeshEdge(Point* a, Point* b) const;
  void MarkConstrained(Point* a, Point* b);
  void ClearConstrained(Point* a, Point* b);

};

} // namespace p2t
