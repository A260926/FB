/*===========================================================================*\
 *                                                                           *
 *                               OpenMesh                                    *
 *      Copyright (C) 2001-2013 by Computer Graphics Group, RWTH Aachen      *
 *                           www.openmesh.org                                *
 *                                                                           *
 *---------------------------------------------------------------------------*
 *  This file is part of OpenMesh.                                           *
 *                                                                           *
 *  OpenMesh is free software: you can redistribute it and/or modify         *
 *  it under the terms of the GNU Lesser General Public License as           *
 *  published by the Free Software Foundation, either version 3 of           *
 *  the License, or (at your option) any later version with the              *
 *  following exceptions:                                                    *
 *                                                                           *
 *  If other files instantiate templates or use macros                       *
 *  or inline functions from this file, or you compile this file and         *
 *  link it with other files to produce an executable, this file does        *
 *  not by itself cause the resulting executable to be covered by the        *
 *  GNU Lesser General Public License. This exception does not however       *
 *  invalidate any other reasons why the executable file might be            *
 *  covered by the GNU Lesser General Public License.                        *
 *                                                                           *
 *  OpenMesh is distributed in the hope that it will be useful,              *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of           *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
 *  GNU Lesser General Public License for more details.                     *
 *                                                                           *
 *  You should have received a copy of the GNU LesserGeneral Public          *
 *  License along with OpenMesh.  If not,                                    *
 *  see <http://www.gnu.org/licenses/>.                                      *
 *                                                                           *
\*===========================================================================*/

//=============================================================================
//
//  Intrinsic Delaunay triangulation on a fixed vertex set, driven by an
//  edge-based metric (edge length property) instead of the 3D positions.
//
//=============================================================================

#ifndef OPENMESH_UTILS_INTRINSICDELAUNAYT_HH
#define OPENMESH_UTILS_INTRINSICDELAUNAYT_HH


//== INCLUDES =================================================================

#include <OpenMesh/Core/System/config.h>
#include <OpenMesh/Core/Mesh/Handles.hh>
#include <OpenMesh/Core/Utils/Property.hh>

#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>


//== NAMESPACES ===============================================================

namespace OpenMesh {
namespace Utils {


//== CLASS DEFINITION =========================================================

/** Report returned by make_intrinsic_delaunay().
 */
struct IntrinsicDelaunayReport
{
  /// Why an edge that violates the intrinsic Delaunay criterion was kept.
  enum Reason
  {
    /// The edge is marked through the locked edge property.
    Locked        = 0,
    /// A topological flip is not allowed (boundary, non-manifold configuration
    /// or the opposite edge already exists).
    NotFlippable  = 1,
    /// The flipped edge would produce a degenerate or invalid triangle when
    /// the two incident triangles are unfolded.
    Degenerate    = 2
  };

  /// An edge that is still non-Delaunay when the run finishes.
  struct RemainingEdge
  {
    RemainingEdge(int _edge_idx, Reason _reason)
      : edge_idx(_edge_idx), reason(_reason) {}

    /// Index of the edge handle (EdgeHandle::idx()).
    int    edge_idx;
    /// Why it could not be flipped.
    Reason reason;
  };

  /// Number of edge flips that were performed.
  unsigned int flips;

  /// Edges that still violate the intrinsic Delaunay criterion after the run.
  /// These edges are deliberately not reported as Delaunay-satisfying.
  std::vector<RemainingEdge> remaining_non_delaunay;

  IntrinsicDelaunayReport() : flips(0) {}
};


//== IMPLEMENTATION ===========================================================

/** \name Intrinsic Delaunay triangulation
 *
 *  Run an intrinsic Delaunay flip algorithm on a triangle mesh.
 *
 *  The mesh is treated as a metric space described by the per-edge length
 *  property, not by the vertex positions (which are never moved, read or
 *  modified).  Vertices, faces and handle numbering are preserved; only edge
 *  connectivity changes through the ordinary TriConnectivity::flip().
 *
 *  For every unlocked interior edge whose two opposite angles sum to more
 *  than pi + 1e-10, the two incident triangles are unfolded into a common
 *  plane and the edge is replaced by the other diagonal.  The new diagonal
 *  length is obtained from the unfolded metric (law of cosines), never from
 *  the straight-line distance of the 3D endpoints.  Neighbouring edges are
 *  re-examined after every flip, so cascades of flips are handled.
 *
 *  After the flip loop the cotangent stiffness weights of the current
 *  intrinsic triangulation are written to \p _weights:
 *  - interior edge: half the sum of the cotangents of the two opposite
 *    angles,
 *  - boundary edge: half the cotangent of the single opposite angle.
 *  Negative weights are kept (they occur on locked illegal edges); they are
 *  never clamped to zero.
 *
 *  Scope and limitations:
 *  - The input must be an oriented manifold triangle mesh, possibly with
 *    boundary and multiple components, without lazily deleted elements.
 *  - Every length must be finite and strictly positive, and every triangle
 *    must strictly satisfy the triangle inequality.  Otherwise
 *    std::invalid_argument is thrown before the topology is touched.
 *  - Output is written into existing edge properties; no separate mesh is
 *    created and all other vertex/face properties keep their values.
 *  - Per-corner attributes such as UV coordinates are not transferred to the
 *    new triangle connectivity: a flip rewires halfedges of the two affected
 *    faces, and corner data stored against the old corners is not remapped.
 *    Do not rely on per-corner UVs after the call; the 3D points remain
 *    valid for display and for locating external per-vertex attributes.
 *
 *  @param _mesh     Manifold triangle mesh.
 *  @param _lengths  Edge property with the intrinsic edge lengths (read and
 *                   updated in place; flipped edges receive the unfolded
 *                   diagonal length).
 *  @param _locked   Edge property marking edges that must never be flipped.
 *  @param _weights  Edge property receiving the cotangent stiffness weights.
 *  @return          Flip count and the edges that remain non-Delaunay.
 */
template <class Mesh>
IntrinsicDelaunayReport
make_intrinsic_delaunay(Mesh&                          _mesh,
                        EPropHandleT<double>           _lengths,
                        EPropHandleT<bool>             _locked,
                        EPropHandleT<double>           _weights)
{
  typedef typename Mesh::EdgeHandle      EdgeHandle;
  typedef typename Mesh::HalfedgeHandle  HalfedgeHandle;

  const double pi      = 3.14159265358979323846;
  const double ang_eps = 1e-10;

  // ---- small metric helpers ------------------------------------------------

  // Angle opposite side c in a triangle with side lengths (a, b, c).
  // Valid input guarantees |cos| <= 1, clamp only against round-off.
  struct Metric
  {
    static double angle(double _a, double _b, double _c)
    {
      double cs = (_a*_a + _b*_b - _c*_c) / (2.0 * _a * _b);
      if (cs < -1.0) cs = -1.0;
      if (cs >  1.0) cs =  1.0;
      return std::acos(cs);
    }

    // Twice the triangle area via Heron's formula (non-negative).
    static double double_area(double _a, double _b, double _c)
    {
      double s  = 0.5 * (_a + _b + _c);
      double v  = s * (s - _a) * (s - _b) * (s - _c);
      if (v < 0.0) v = 0.0;
      return 2.0 * std::sqrt(v);
    }

    // Cotangent of the angle opposite side c (adjacent sides a, b).
    static double cotan(double _a, double _b, double _c)
    {
      double two_area = double_area(_a, _b, _c);
      if (two_area == 0.0)
      {
        // Degenerate triangle: fall back to the cosine sign.
        double cs = (_a*_a + _b*_b - _c*_c) / (2.0 * _a * _b);
        return cs >= 0.0 ? std::numeric_limits<double>::infinity()
                         : -std::numeric_limits<double>::infinity();
      }
      double cos_a = (_a*_a + _b*_b - _c*_c) / (2.0 * _a * _b);
      double sin_a = two_area / (_a * _b);
      return cos_a / sin_a;
    }

    // Strict triangle inequality, with a tiny numerical guard.
    static bool valid_triangle(double _a, double _b, double _c)
    {
      if (!(_a > 0.0) || !(_b > 0.0) || !(_c > 0.0))
        return false;
      double tol = 1e-12 * std::max(_a, std::max(_b, _c));
      return (_a + _b > _c + tol) &&
             (_a + _c > _b + tol) &&
             (_b + _c > _a + tol);
    }
  };

  // ---- validate the input before touching any topology ---------------------

  for (typename Mesh::ConstEdgeIter eit = _mesh.edges_begin();
       eit != _mesh.edges_end(); ++eit)
  {
    double L = _mesh.property(_lengths, *eit);
    if (!std::isfinite(L) || L <= 0.0)
      throw std::invalid_argument(
        "make_intrinsic_delaunay: edge length must be finite and positive");
  }

  for (typename Mesh::ConstFaceIter fit = _mesh.faces_begin();
       fit != _mesh.faces_end(); ++fit)  {
    HalfedgeHandle hh = _mesh.halfedge_handle(*fit);
    double l0 = _mesh.property(_lengths, _mesh.edge_handle(hh));
    double l1 = _mesh.property(_lengths,
                 _mesh.edge_handle(_mesh.next_halfedge_handle(hh)));
    double l2 = _mesh.property(_lengths,
                 _mesh.edge_handle(_mesh.next_halfedge_handle(
                                     _mesh.next_halfedge_handle(hh))));
    if (!Metric::valid_triangle(l0, l1, l2))
      throw std::invalid_argument(
        "make_intrinsic_delaunay: triangle does not satisfy the strict "
        "triangle inequality");
  }

  IntrinsicDelaunayReport report;

  // Sum of the two opposite angles across an interior edge.
  // Returns a value > pi iff the edge is intrinsically non-Delaunay.
  struct Probe
  {
    static double opposite_angle_sum(const Mesh& _m,
                                     EPropHandleT<double> _len,
                                     EdgeHandle _e)
    {
      HalfedgeHandle h0 = _m.halfedge_handle(_e, 0);
      HalfedgeHandle h1 = _m.halfedge_handle(_e, 1);
      double le = _m.property(_len, _e);

      // Face at h0: angle at to(next(h0)), adjacent sides are the other two
      // edges of that face.
      HalfedgeHandle n0 = _m.next_halfedge_handle(h0);
      HalfedgeHandle n0n = _m.next_halfedge_handle(n0);
      double a0 = _m.property(_len, _m.edge_handle(n0));
      double b0 = _m.property(_len, _m.edge_handle(n0n));
      double ang0 = Metric::angle(a0, b0, le);

      HalfedgeHandle n1 = _m.next_halfedge_handle(h1);
      HalfedgeHandle n1n = _m.next_halfedge_handle(n1);
      double a1 = _m.property(_len, _m.edge_handle(n1));
      double b1 = _m.property(_len, _m.edge_handle(n1n));
      double ang1 = Metric::angle(a1, b1, le);

      return ang0 + ang1;
    }
  };

  // ---- worklist driven flip loop -------------------------------------------
  //
  // No elements are added or removed, so edge indices stay valid throughout
  // and a dense flag vector is sufficient.

  const size_t n_edges = _mesh.n_edges();
  std::vector<char> in_queue(n_edges, 0);
  std::vector<int>  queue;
  queue.reserve(n_edges);
  for (size_t i = 0; i < n_edges; ++i)
  {
    queue.push_back(static_cast<int>(i));
    in_queue[i] = 1;
  }

  while (!queue.empty())
  {
    int eidx = queue.back();
    queue.pop_back();
    in_queue[eidx] = 0;

    EdgeHandle e(eidx);

    if (_mesh.is_boundary(e))
      continue;
    if (_mesh.property(_locked, e))
      continue;

    if (Probe::opposite_angle_sum(_mesh, _lengths, e) <= pi + ang_eps)
      continue;

    if (!_mesh.is_flip_ok(e))
      continue;

    // Gather the local configuration before the flip (see TriConnectivity
    // flip() for the halfedge naming).
    HalfedgeHandle a0 = _mesh.halfedge_handle(e, 0);
    HalfedgeHandle b0 = _mesh.halfedge_handle(e, 1);
    HalfedgeHandle a1 = _mesh.next_halfedge_handle(a0);
    HalfedgeHandle a2 = _mesh.next_halfedge_handle(a1);
    HalfedgeHandle b1 = _mesh.next_halfedge_handle(b0);
    HalfedgeHandle b2 = _mesh.next_halfedge_handle(b1);

    double le  = _mesh.property(_lengths, e);
    double la1 = _mesh.property(_lengths, _mesh.edge_handle(a1));
    double la2 = _mesh.property(_lengths, _mesh.edge_handle(a2));
    double lb1 = _mesh.property(_lengths, _mesh.edge_handle(b1));
    double lb2 = _mesh.property(_lengths, _mesh.edge_handle(b2));

    // Unfold the two triangles into a plane and measure the other diagonal
    // with the law of cosines.  Around hinge vertex to(a0) (= v1), the
    // interior angle of the unfolded quadrilateral is the sum of the two
    // face angles opposite the old diagonal.
    double hinge_angle =
      Metric::angle(la1, le, la2) + Metric::angle(lb2, le, lb1);

    double new_len_sq =
      la1*la1 + lb2*lb2 - 2.0*la1*lb2*std::cos(hinge_angle);
    if (!(new_len_sq > 0.0) || !std::isfinite(new_len_sq))
      continue;
    double new_len = std::sqrt(new_len_sq);

    // The two triangles created by the new diagonal must be valid.
    if (!Metric::valid_triangle(la2, lb1, new_len) ||
        !Metric::valid_triangle(la1, lb2, new_len))
      continue;

    _mesh.flip(e);
    _mesh.property(_lengths, e) = new_len;
    ++report.flips;

    // Re-examine the flipped edge and the four surviving edges; any of them
    // may have become illegal through the new diagonal length.
    int neighbour[5] =
    {
      e.idx(),
      _mesh.edge_handle(a1).idx(),
      _mesh.edge_handle(a2).idx(),
      _mesh.edge_handle(b1).idx(),
      _mesh.edge_handle(b2).idx()
    };

    for (int k = 0; k < 5; ++k)
    {
      if (!in_queue[neighbour[k]])
      {
        in_queue[neighbour[k]] = 1;
        queue.push_back(neighbour[k]);
      }
    }
  }

  // ---- report the edges that are still illegal -----------------------------

  for (size_t i = 0; i < n_edges; ++i)
  {
    EdgeHandle e(static_cast<int>(i));
    if (_mesh.is_boundary(e))
      continue;
    if (Probe::opposite_angle_sum(_mesh, _lengths, e) <= pi + ang_eps)
      continue;

    IntrinsicDelaunayReport::Reason reason;
    if (_mesh.property(_locked, e))
      reason = IntrinsicDelaunayReport::Locked;
    else if (!_mesh.is_flip_ok(e))
      reason = IntrinsicDelaunayReport::NotFlippable;
    else
      reason = IntrinsicDelaunayReport::Degenerate;

    report.remaining_non_delaunay.push_back(
      IntrinsicDelaunayReport::RemainingEdge(e.idx(), reason));
  }

  // ---- cotangent stiffness weights of the intrinsic triangulation ---------

  for (size_t i = 0; i < n_edges; ++i)
  {
    EdgeHandle e(static_cast<int>(i));
    HalfedgeHandle h0 = _mesh.halfedge_handle(e, 0);
    HalfedgeHandle h1 = _mesh.halfedge_handle(e, 1);
    double le = _mesh.property(_lengths, e);
    double w  = 0.0;

    if (!_mesh.is_boundary(h0))
    {
      HalfedgeHandle n0  = _mesh.next_halfedge_handle(h0);
      HalfedgeHandle n0n = _mesh.next_halfedge_handle(n0);
      w += Metric::cotan(_mesh.property(_lengths, _mesh.edge_handle(n0)),
                         _mesh.property(_lengths, _mesh.edge_handle(n0n)),
                         le);
    }
    if (!_mesh.is_boundary(h1))
    {
      HalfedgeHandle n1  = _mesh.next_halfedge_handle(h1);
      HalfedgeHandle n1n = _mesh.next_halfedge_handle(n1);
      w += Metric::cotan(_mesh.property(_lengths, _mesh.edge_handle(n1)),
                         _mesh.property(_lengths, _mesh.edge_handle(n1n)),
                         le);
    }
    _mesh.property(_weights, e) = 0.5 * w;
  }

  return report;
}


//=============================================================================
} // namespace Utils
} // namespace OpenMesh
//=============================================================================
#endif // OPENMESH_UTILS_INTRINSICDELAUNAYT_HH defined
//=============================================================================
