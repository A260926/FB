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
 *  or inline functions from this file, or you compile this file and link it *
 *  with other files to produce an executable, this file does not by itself  *
 *  cause the resulting executable to be covered by the GNU Lesser General   *
 *  Public License. This exception does not however invalidate any other     *
 *  reasons why the executable file might be covered by the GNU Lesser       *
 *  General Public License.                                                  *
 *                                                                           *
\*===========================================================================*/

/** \file Tools/Utils/SurfaceImprintT.hh
    Constraint-based surface remeshing: line segments drawn on the triangles
    of a manifold triangle mesh are imprinted as real mesh edges.

    Every input segment is given per source face as a pair of barycentric
    coordinates with respect to that face's cfv_iter corner order.  The
    segments are never flattened and welded, never merged by 3D distance and
    never stored as a display-only line list.  Inside each source face they
    are treated as a planar arrangement: all proper intersections,
    endpoints-on-segment events and collinear overlaps become vertices, the
    constrained edges (segment pieces and the original triangle sides) are
    kept, and the remaining regions are triangulated.  The triangulator is a
    self-contained constrained working triangulation (incremental point
    insertion plus constrained edge insertion by edge flips); no external
    triangulation library is used and no Steiner points are introduced:
    output vertices are exclusively original vertices, segment endpoints,
    segment intersections and original edge split points shared with the
    neighbouring face.

    Points inserted on a shared original edge are created exactly once: one
    edge parameterization is shared by both incident faces, so even a face
    carrying no segment itself is split at the same positions, and segments
    drawn along the edge from either side reference the same sub-edges.

    The output mesh preserves every original vertex (same index and
    position); every output face lies in a unique source face, inherits its
    winding and all output faces together cover the source surface exactly
    once.  The output is a compact oriented manifold with genuinely
    reciprocal halfedge adjacency, so MeshCheckerT and the circulators keep
    working.  Identities are original vertex/edge/face based throughout;
    spatially overlapping but disconnected shells are never welded.

    If the source mesh requests halfedge texcoords2D, the result requests the
    same property.  Each new face corner is interpolated from the three
    corner UVs of its source face through the corner barycentric
    coordinates.  A halfedge UV belongs to the face corner at its target
    vertex, so UV seams are preserved: no opposite-halfedge lookup and no
    global averaging.  New boundary halfedges get zero UV.  No other source
    attributes are copied; callers reconstruct them from
    SurfaceImprintMap::face_source and SurfaceImprintMap::corner_bary.
*/

//=============================================================================
//
//  Surface imprint (constrained remeshing of triangle surfaces)
//
//=============================================================================

#ifndef OPENMESH_TOOLS_SURFACE_IMPRINT_T_HH
#define OPENMESH_TOOLS_SURFACE_IMPRINT_T_HH

#include <OpenMesh/Core/System/config.h>
#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace OpenMesh {
namespace Utils {

//=============================================================================

/** \brief One straight segment drawn inside a single source face.

    The two endpoints are barycentric coordinates with respect to the face's
    cfv_iter corner order.  A stroke crossing several faces is split by the
    caller into one SurfaceSegment per face.  The segment's identity is its
    index in the vector handed to imprint_surface(); coincident segments,
    including reversed duplicates, keep separate identities.
*/
struct SurfaceSegment
{
  int face;                       ///< Source face index.
  std::array<double, 3> a;        ///< Start point, barycentric coordinates.
  std::array<double, 3> b;        ///< End point, barycentric coordinates.
};

/** \brief Provenance information returned by imprint_surface().

    The first two vectors are indexed by output element indices and have
    lengths exactly equal to the corresponding output element counts.
*/
struct SurfaceImprintMap
{
  /// Source face index for every output face.
  std::vector<int> face_source;

  /// Barycentric coordinates (in the face given by face_source) of the
  /// target vertex of every output halfedge.  Zero for boundary halfedges
  /// of the output mesh.
  std::vector<std::array<double, 3> > corner_bary;

  /// For every input segment (indexed by its input position): the ordered
  /// output vertex indices from its a endpoint to its b endpoint, including
  /// all split points along the segment.  Consecutive vertices are connected
  /// by a real output edge and never repeat.
  std::vector<std::vector<int> > segment_chains;

  /// For every output edge: the sorted, deduplicated input segment indices
  /// covering (part of) that edge.  Empty when no segment covers the edge.
  /// Fully or partially overlapping segments all appear; identities never
  /// overwrite each other.
  std::vector<std::vector<size_t> > edge_segments;
};

//=============================================================================
//  Implementation details
//=============================================================================

namespace SurfaceImprintDetail {

// All planar construction works in doubles on barycentric coordinates,
// independently of the mesh Point scalar type: barycentric coordinates are
// affine, so every constructed point stays on the source plane exactly.
struct Bary
{
  double w[3];

  Bary() { w[0] = w[1] = w[2] = 0.0; }
  explicit Bary(const std::array<double, 3>& v)
  { w[0] = v[0]; w[1] = v[1]; w[2] = v[2]; }

  double  operator[](int i) const { return w[i]; }
  double& operator[](int i)       { return w[i]; }

  Bary operator+(const Bary& o) const
  { Bary r; for (int i = 0; i < 3; ++i) r.w[i] = w[i] + o.w[i]; return r; }
  Bary operator-(const Bary& o) const
  { Bary r; for (int i = 0; i < 3; ++i) r.w[i] = w[i] - o.w[i]; return r; }

  static double dot(const Bary& x, const Bary& y)
  { return x.w[0]*y.w[0] + x.w[1]*y.w[1] + x.w[2]*y.w[2]; }
};

inline bool finite_bary(const Bary& b)
{
  return std::isfinite(b.w[0]) && std::isfinite(b.w[1]) &&
         std::isfinite(b.w[2]);
}

// The third barycentric component is redundant (components sum to one), so
// (w0, w1) is a faithful 2D coordinate system for the face plane.
typedef std::array<double, 2> P2;

inline P2 to2(const Bary& b) { return P2{{b.w[0], b.w[1]}}; }

inline P2 sub2(const P2& a, const P2& b)
{ return P2{{a[0] - b[0], a[1] - b[1]}}; }

inline double cross2(const P2& a, const P2& b)
{ return a[0]*b[1] - a[1]*b[0]; }

inline double orient2(const P2& a, const P2& b, const P2& c)
{ return cross2(sub2(b, a), sub2(c, a)); }

// Event tolerances.  Distinct real events are at least 1e-7 apart in any
// relevant barycentric system; shared-edge events may differ by 1e-12 and
// are merged at 1e-10.  Nothing else is ever merged or moved.
static const double kEventEps  = 1.0e-10;
static const double kOnLineEps = 2.0e-10;

// An event on one input segment: parameter t in [0,1] from a to b, together
// with the planar point at which it happens.
struct SegEvent
{
  double t;
  P2     p;
};

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

inline Bary normalize_bary(const std::array<double, 3>& v, bool& ok)
{
  Bary b(v);
  ok = false;
  if (!finite_bary(b)) return b;
  for (int i = 0; i < 3; ++i)
    if (b[i] < -1.0e-12 || b[i] > 1.0 + 1.0e-12) return b;
  const double s = b[0] + b[1] + b[2];
  if (s == 0.0 || std::fabs(s - 1.0) > 1.0e-12) return b;
  for (int i = 0; i < 3; ++i)
    b[i] = std::max(0.0, b[i] / s);
  // Restore an exact unit sum on the largest component.
  int big = 0;
  for (int i = 1; i < 3; ++i) if (b[i] > b[big]) big = i;
  b[big] = 1.0 - (b[(big + 1) % 3] + b[(big + 2) % 3]);
  ok = true;
  return b;
}

// Merge sorted parameters that agree within kEventEps.
inline std::vector<double> merge_parameters(std::vector<double> v)
{
  std::sort(v.begin(), v.end());
  std::vector<double> out;
  for (double x : v)
  {
    if (out.empty() || std::fabs(out.back() - x) > kEventEps)
      out.push_back(x);
    else
      out.back() = 0.5 * (out.back() + x);
  }
  return out;
}

// Side parameter of a barycentric point on side o.  Sides follow the cfv
// walk corner o -> corner (o+1)%3.
//   side 0: w2 == 0, parameter w1
//   side 1: w0 == 0, parameter w2
//   side 2: w1 == 0, parameter w0
inline double side_parameter(int side, const Bary& b)
{
  if (side == 0) return b[1];
  if (side == 1) return b[2];
  return b[0];
}

// Barycentric point of parameter t on side o.
inline Bary side_point(int side, double t)
{
  Bary b;
  if (side == 0)      { b[0] = 1.0 - t; b[1] = t;       b[2] = 0.0; }
  else if (side == 1) { b[0] = 0.0;     b[1] = 1.0 - t; b[2] = t; }
  else                { b[0] = t;       b[1] = 0.0;     b[2] = 1.0 - t; }
  return b;
}

// Which side (if any) a planar point lies on.  Returns -1 for an interior
// point.  Exact corners are tested first; computed intersections are
// recognized as side points within kOnLineEps.
inline int which_side(const P2& p)
{
  const double w0 = p[0], w1 = p[1], w2 = 1.0 - p[0] - p[1];
  // Exact / near-exact corners.
  if (w1 <= kOnLineEps && w2 <= kOnLineEps) return 2; // corner 0
  if (w0 <= kOnLineEps && w2 <= kOnLineEps) return 0; // corner 1
  if (w0 <= kOnLineEps && w1 <= kOnLineEps) return 1; // corner 2
  if (w2 <= kOnLineEps && w2 >= -kOnLineEps &&
      w0 >= -kOnLineEps && w1 >= -kOnLineEps)
    return 0;
  if (w0 <= kOnLineEps && w0 >= -kOnLineEps &&
      w1 >= -kOnLineEps && w2 >= -kOnLineEps)
    return 1;
  if (w1 <= kOnLineEps && w1 >= -kOnLineEps &&
      w0 >= -kOnLineEps && w2 >= -kOnLineEps)
    return 2;
  return -1;
}

// ---------------------------------------------------------------------------
// Pairwise arrangement of the segments of one face.
//
// Complexity is quadratic in the segment count of this face only; segments
// of different faces are never compared against each other.
// ---------------------------------------------------------------------------

inline bool arrange_segments(
  const std::vector<std::array<Bary, 2> >& ends,
  std::vector<std::vector<SegEvent> >&     events)
{
  const size_t n = ends.size();
  events.assign(n, std::vector<SegEvent>());
  for (size_t i = 0; i < n; ++i)
  {
    events[i].push_back(SegEvent{0.0, to2(ends[i][0])});
    events[i].push_back(SegEvent{1.0, to2(ends[i][1])});
  }

  for (size_t i = 0; i < n; ++i)
  {
    const P2 a = to2(ends[i][0]);
    const P2 b = to2(ends[i][1]);
    const P2 d1 = sub2(b, a);
    for (size_t j = i + 1; j < n; ++j)
    {
      const P2 c = to2(ends[j][0]);
      const P2 d = to2(ends[j][1]);
      const P2 d2 = sub2(d, c);
      const double den = cross2(d1, d2);

      if (std::fabs(den) > 1.0e-12)
      {
        // Proper intersection or T-junction (an endpoint on the other line).
        const P2 ca = sub2(c, a);
        double t = cross2(ca, d2) / den;
        double u = cross2(ca, d1) / den;
        if (t >= -kOnLineEps && t <= 1.0 + kOnLineEps &&
            u >= -kOnLineEps && u <= 1.0 + kOnLineEps)
        {
          t = std::min(1.0, std::max(0.0, t));
          u = std::min(1.0, std::max(0.0, u));
          P2 p{{a[0] + d1[0]*t, a[1] + d1[1]*t}};
          events[i].push_back(SegEvent{t, p});
          events[j].push_back(SegEvent{u, p});
        }
      }
      else
      {
        // Parallel: collinear only if c sits on segment i's supporting line.
        if (std::fabs(cross2(sub2(c, a), d1)) > kOnLineEps) continue;
        const double len1 = d1[0]*d1[0] + d1[1]*d1[1];
        const double len2 = d2[0]*d2[0] + d2[1]*d2[1];
        if (!(len1 > 0.0 && len2 > 0.0)) return false;
        auto param = [&](const P2& q)
        {
          const P2 qa = sub2(q, a);
          return (qa[0]*d1[0] + qa[1]*d1[1]) / len1;
        };
        double s0 = param(c), s1 = param(d);
        if (s0 > s1) std::swap(s0, s1);
        const double lo = std::min(1.0, std::max(0.0, s0));
        const double hi = std::min(1.0, std::max(0.0, s1));
        if (lo > hi + kOnLineEps) continue;       // disjoint collinear pieces
        for (int q = 0; q < 2; ++q)
        {
          const double t = (q == 0 ? lo : hi);
          P2 p{{a[0] + d1[0]*t, a[1] + d1[1]*t}};
          events[i].push_back(SegEvent{t, p});
          const P2 qd = sub2(p, c);
          double uj = (qd[0]*d2[0] + qd[1]*d2[1]) / len2;
          uj = std::min(1.0, std::max(0.0, uj));
          events[j].push_back(SegEvent{uj, p});
        }
      }
    }
  }

  // Sort and merge coincident parameters per segment.
  for (size_t i = 0; i < n; ++i)
  {
    std::sort(events[i].begin(), events[i].end(),
              [](const SegEvent& x, const SegEvent& y)
              { return x.t < y.t; });
    std::vector<SegEvent> uniq;
    for (const SegEvent& e : events[i])
    {
      if (!uniq.empty() && std::fabs(uniq.back().t - e.t) <= kEventEps)
      {
        // Same physical event reached through different segment pairs:
        // average the point to stay symmetric.
        uniq.back().p[0] = 0.5 * (uniq.back().p[0] + e.p[0]);
        uniq.back().p[1] = 0.5 * (uniq.back().p[1] + e.p[1]);
      }
      else uniq.push_back(e);
    }
    events[i].swap(uniq);
    if (events[i].size() < 2 ||
        events[i].front().t > kEventEps ||
        events[i].back().t < 1.0 - kEventEps)
      return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Constrained working triangulation of one source face.
//
// Invariants:
//   * every triangle is counter-clockwise in the (w0, w1) plane;
//   * every undirected triangle edge has exactly one edge record;
//   * an edge record carries the global input segment ids covering it;
//   * splitting an edge propagates its segment ids to both sub-edges.
//
// A missing constrained edge is realized with the classical incremental
// algorithm: all constraint vertices already exist, so the segment only
// crosses triangulation edges, which are flipped until the edge (u,v)
// appears.  Constraint interiors never cross (all intersections were turned
// into vertices beforehand), hence a tagged edge is never flipped.
// ---------------------------------------------------------------------------

class FaceTriangulation
{
public:
  struct Tri { int v[3]; };

  std::vector<P2>                          points;
  std::vector<Tri>                         tris;
  std::vector<std::array<int, 2> >         edges;
  std::vector<std::vector<size_t> >        edge_tags;
  std::map<std::pair<int, int>, int>       edge_id;

  FaceTriangulation()
  {
    // Nodes 0,1,2 are the source corners, counter-clockwise in (w0,w1).
    points.push_back(P2{{1.0, 0.0}});
    points.push_back(P2{{0.0, 1.0}});
    points.push_back(P2{{0.0, 0.0}});
    tris.push_back(Tri{{0, 1, 2}});
    (void)get_edge(0, 1);
    (void)get_edge(1, 2);
    (void)get_edge(2, 0);
  }

  static std::pair<int, int> key(int u, int v)
  { if (u > v) std::swap(u, v); return std::make_pair(u, v); }

  int get_edge(int u, int v)
  {
    std::pair<int, int> k = key(u, v);
    auto it = edge_id.find(k);
    if (it != edge_id.end()) return it->second;
    int id = static_cast<int>(edges.size());
    edges.push_back(std::array<int, 2>{{k.first, k.second}});
    edge_tags.push_back(std::vector<size_t>());
    edge_id[k] = id;
    return id;
  }

  void tag_edge(int u, int v, size_t seg)
  {
    int id = get_edge(u, v);
    std::vector<size_t>& tags = edge_tags[id];
    if (std::find(tags.begin(), tags.end(), seg) == tags.end())
      tags.push_back(seg);
  }

  // Triangles incident to edge (u,v).  The flag records whether the triangle
  // stores the directed edge (u,v) (false) or (v,u) (true).
  std::vector<std::pair<int, bool> > edge_triangles(int u, int v) const
  {
    std::vector<std::pair<int, bool> > out;
    for (size_t k = 0; k < tris.size(); ++k)
    {
      const Tri& t = tris[k];
      for (int q = 0; q < 3; ++q)
      {
        if (t.v[q] == u && t.v[(q + 1) % 3] == v)
        { out.push_back(std::make_pair((int)k, false)); break; }
        if (t.v[q] == v && t.v[(q + 1) % 3] == u)
        { out.push_back(std::make_pair((int)k, true)); break; }
      }
    }
    return out;
  }

  // First triangle containing point p (boundary included).
  int locate_triangle(const P2& p) const
  {
    for (size_t k = 0; k < tris.size(); ++k)
    {
      const Tri& t = tris[k];
      if (orient2(points[t.v[0]], points[t.v[1]], p) >= -kOnLineEps &&
          orient2(points[t.v[1]], points[t.v[2]], p) >= -kOnLineEps &&
          orient2(points[t.v[2]], points[t.v[0]], p) >= -kOnLineEps)
        return static_cast<int>(k);
    }
    return -1;
  }

  // Inserts an already-created point node.  A point on an edge splits that
  // edge (every incident triangle); an interior point splits one triangle
  // into three.
  bool insert_point(int node)
  {
    const P2& p = points[node];
    int tk = locate_triangle(p);
    if (tk < 0) return false;
    const Tri t = tris[tk];

    for (int q = 0; q < 3; ++q)
    {
      int a = t.v[q], b = t.v[(q + 1) % 3];
      if (std::fabs(orient2(points[a], points[b], p)) <= kOnLineEps &&
          on_segment(points[a], points[b], p))
      {
        split_edge(a, b, node);
        return true;
      }
    }

    // Interior insertion: (a,b,c) -> (node,b,c),(a,node,c),(a,b,node).
    tris[tk] = Tri{{node, t.v[1], t.v[2]}};
    tris.push_back(Tri{{t.v[0], node, t.v[2]}});
    tris.push_back(Tri{{t.v[0], t.v[1], node}});
    (void)get_edge(node, t.v[1]);
    (void)get_edge(node, t.v[2]);
    (void)get_edge(t.v[0], node);
    return true;
  }

  // Splits edge (x,y) at an existing point node.  Every incident triangle
  // is split and covering segment ids are inherited by both sub-edges.
  void split_edge(int x, int y, int node)
  {
    const int eid = get_edge(x, y);
    const std::vector<size_t> inherited = edge_tags[eid];
    const auto incident = edge_triangles(x, y);

    for (const auto& hit : incident)
    {
      Tri t = tris[hit.first];
      if (!hit.second)
      {
        // (x,y,c) ccw -> (node,y,c) and (x,node,c).
        int c = third_vertex(t, x, y);
        tris[hit.first] = Tri{{node, y, c}};
        tris.push_back(Tri{{x, node, c}});
      }
      else
      {
        // (y,x,c) ccw -> (node,x,c) and (y,node,c).
        int c = third_vertex(t, y, x);
        tris[hit.first] = Tri{{node, x, c}};
        tris.push_back(Tri{{y, node, c}});
      }
    }

    remove_edge_record(x, y);
    const int e1 = get_edge(x, node);
    const int e2 = get_edge(node, y);
    for (size_t s : inherited)
    {
      if (std::find(edge_tags[e1].begin(), edge_tags[e1].end(), s) ==
          edge_tags[e1].end()) edge_tags[e1].push_back(s);
      if (std::find(edge_tags[e2].begin(), edge_tags[e2].end(), s) ==
          edge_tags[e2].end()) edge_tags[e2].push_back(s);
    }
    // Re-register every edge used by the current triangles.
    for (const Tri& tr : tris)
      for (int q = 0; q < 3; ++q)
        (void)get_edge(tr.v[q], tr.v[(q + 1) % 3]);
  }

  // Inserts constrained edge (u,v) and tags it with seg.  All constraint
  // intersections are already vertices, so the segment u->v crosses only
  // untagged triangulation edges.  Any existing node lying on the open
  // segment splits it first; every resulting sub-piece then has a
  // node-free interior, which keeps its extraction "pipe" a simple
  // two-chain polygon.  Each pipe is found by half-plane interval clipping
  // per triangle (no adjacency walk, so a ray through a vertex cannot
  // oscillate) and re-triangulated by ear clipping, yielding real edges.
  bool insert_constraint(int u, int v, size_t seg)
  {
    if (edge_id.count(key(u, v)))
    {
      tag_edge(u, v, seg);
      return true;
    }

    const P2& pu = points[u];
    const P2& pv = points[v];

    // All nodes on the open segment, ordered from u to v.  Splitting here
    // also covers segments that coincide with already subdivided sides.
    const P2 d = sub2(pv, pu);
    const double dlen2 = d[0]*d[0] + d[1]*d[1];
    if (!(dlen2 > 0.0)) return false;
    std::vector<std::pair<double, int> > on(1, std::make_pair(0.0, u));
    for (size_t k = 0; k < points.size(); ++k)
    {
      if ((int)k == u || (int)k == v) continue;
      const P2 r = sub2(points[k], pu);
      if (std::fabs(cross2(d, r)) > kOnLineEps) continue;
      const double t = (d[0]*r[0] + d[1]*r[1]) / dlen2;
      if (t > kOnLineEps && t < 1.0 - kOnLineEps)
        on.push_back(std::make_pair(t, (int)k));
    }
    on.push_back(std::make_pair(1.0, v));
    std::sort(on.begin(), on.end());
    {
      std::vector<std::pair<double, int> > uniq;
      for (const auto& q : on)
        if (uniq.empty() || uniq.back().second != q.second) uniq.push_back(q);
      on.swap(uniq);
    }

    // If the whole collinear chain already exists, just tag its edges.
    bool chain_exists = true;
    for (size_t k = 1; k < on.size(); ++k)
      if (!edge_id.count(key(on[k - 1].second, on[k].second)))
      { chain_exists = false; break; }
    if (chain_exists)
    {
      for (size_t k = 1; k < on.size(); ++k)
        tag_edge(on[k - 1].second, on[k].second, seg);
      return true;
    }

    // Insert every node-free sub-piece through its pipe.
    for (size_t piece = 0; piece + 1 < on.size(); ++piece)
      if (!insert_pipe_edge(on[piece].second, on[piece + 1].second, seg))
        return false;
    return true;
  }

  // Realizes a single constraint sub-piece whose open segment contains no
  // other node, then tags the resulting edge with seg.
  bool insert_pipe_edge(int u, int v, size_t seg)
  {
    if (edge_id.count(key(u, v)))
    {
      tag_edge(u, v, seg);
      return true;
    }

    const P2& pu = points[u];
    const P2& pv = points[v];
    const double clip_eps = 1.0e-12;

    // ---- Clip the ray against every triangle ------------------------------
    // A triangle is a genuine pipe member only if the ray segment passes
    // through its interior with positive length, or one of u/v is one of
    // its vertices.  A grazing contact (the ray merely touches a vertex or
    // runs along an edge) yields a near-zero interval and is rejected.
    struct Hit { double lo, hi; int tri; };
    std::vector<Hit> hits;
    for (size_t k = 0; k < tris.size(); ++k)
    {
      const Tri& t = tris[k];
      double lo = 0.0, hi = 1.0;
      bool empty = false;
      for (int q = 0; q < 3; ++q)
      {
        const P2& pa = points[t.v[q]];
        const P2& pb = points[t.v[(q + 1) % 3]];
        const double f = orient2(pa, pb, pu);
        const double g = orient2(pa, pb, pv) - f;
        if (std::fabs(g) <= 1.0e-14)
        {
          if (f < -clip_eps) { empty = true; break; }
        }
        else if (g > 0.0)
          lo = std::max(lo, -f / g);
        else
          hi = std::min(hi, -f / g);
      }
      if (empty) continue;
      lo = std::max(lo, 0.0);
      hi = std::min(hi, 1.0);
      if (hi <= lo + clip_eps) continue;
      hits.push_back(Hit{lo, hi, (int)k});
    }
    if (hits.empty()) return false;
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.lo < b.lo; });

    // Build the pipe from triangles whose intervals form a contiguous cover
    // of [0,1].
    std::set<int> pipe_tris;
    double frontier = 0.0;
    size_t scan = 0;
    while (frontier < 1.0 - clip_eps)
    {
      double advanced = frontier;
      size_t mark = scan;
      for (size_t k = scan; k < hits.size(); ++k)
      {
        if (hits[k].lo > frontier + clip_eps) break;
        if (hits[k].hi > advanced) advanced = hits[k].hi;
        pipe_tris.insert(hits[k].tri);
        mark = k + 1;
      }
      if (advanced <= frontier + clip_eps) return false;
      frontier = advanced;
      scan = mark;
    }

    // ---- Single triangle strip: subdivide directly -------------------------
    if (pipe_tris.size() == 1)
      return subdivide_one_triangle(*pipe_tris.begin(), u, v, seg);

    // ---- Classify every edge against the pipe ------------------------------
    std::vector<std::pair<int, int> > interior_edges;
    std::vector<std::pair<int, int> > boundary_edges;
    struct XEdge { double t; int a, b; };
    std::vector<XEdge> crossed;
    for (size_t e = 0; e < edges.size(); ++e)
    {
      const auto nb = edge_triangles(edges[e][0], edges[e][1]);
      int in = 0;
      for (const auto& hit : nb)
        if (pipe_tris.count(hit.first)) ++in;
      if (in == 2)
      {
        if (!edge_tags[e].empty()) return false;   // constraints must not cross
        interior_edges.push_back(
          std::make_pair(edges[e][0], edges[e][1]));
        const P2 d1 = sub2(pv, pu);
        const P2 d2 = sub2(points[edges[e][1]], points[edges[e][0]]);
        const double den = cross2(d1, d2);
        if (std::fabs(den) > 1.0e-14)
        {
          const P2 rp = sub2(points[edges[e][0]], pu);
          const double tt = cross2(rp, d2) / den;
          const double ww = cross2(rp, d1) / den;
          if (tt > clip_eps && tt < 1.0 - clip_eps &&
              ww > clip_eps && ww < 1.0 - clip_eps)
            crossed.push_back(XEdge{tt, edges[e][0], edges[e][1]});
        }
      }
      else if (in == 1)
        boundary_edges.push_back(
          std::make_pair(edges[e][0], edges[e][1]));
    }
    std::sort(crossed.begin(), crossed.end(),
              [](const XEdge& x, const XEdge& y) { return x.t < y.t; });

    // Boundary graph.  Its nodes have degree 2 except u and v (the pipe is a
    // topological disk strip); at a vertex pinch both sides touch the same
    // node and the monotonic walk still advances.
    std::map<int, std::vector<int> > badj;
    for (const auto& be : boundary_edges)
    {
      badj[be.first].push_back(be.second);
      badj[be.second].push_back(be.first);
    }
    const P2 rayd = sub2(pv, pu);
    const double raylen2 = rayd[0] * rayd[0] + rayd[1] * rayd[1];
    auto ray_t = [&](int n)
    {
      const P2 r = sub2(points[n], pu);
      return (r[0] * rayd[0] + r[1] * rayd[1]) / raylen2;
    };
    auto walk = [&](int first) -> std::vector<int>
    {
      std::vector<int> chain(1, u);
      int prev = -1, cur = u, next = first;
      size_t guard = 0;
      while (cur != v)
      {
        if (guard++ > points.size() + 16) return std::vector<int>();
        chain.push_back(next);
        prev = cur;
        cur = next;
        if (cur == v) break;
        // Follow the forward neighbour with smallest ray progress so a
        // branching (pinch) node cannot divert into a side pocket.
        int follow = -1;
        double best = 1.0e300;
        const double tcur = ray_t(cur);
        for (int cand : badj[cur])
        {
          if (cand == prev) continue;
          double tc = ray_t(cand);
          if (tc + kOnLineEps < tcur) continue;
          if (tc < best) { best = tc; follow = cand; }
        }
        if (follow < 0)
          for (int cand : badj[cur])
            if (cand != prev &&
                std::find(chain.begin(), chain.end(), cand) == chain.end())
            { follow = cand; break; }
        if (follow < 0) return std::vector<int>();
        next = follow;
      }
      return chain;
    };

    std::vector<int> left_chain, right_chain;
    if (badj.count(u) && badj[u].size() == 2 && badj.count(v))
    {
      std::vector<int> c1 = walk(badj[u][0]);
      std::vector<int> c2 = walk(badj[u][1]);
      if (c1.size() < 2 || c2.size() < 2 ||
          c1.back() != v || c2.back() != v)
        return false;
      auto side = [&](const std::vector<int>& ch)
      {
        for (int n : ch)
        {
          if (n == u || n == v) continue;
          double s = orient2(pu, pv, points[n]);
          if (std::fabs(s) > kOnLineEps) return s > 0.0 ? 1 : -1;
        }
        return 0;
      };
      int s1 = side(c1), s2 = side(c2);
      if (s1 == 0 || s2 == 0 || s1 == s2) return false;
      left_chain  = (s1 > 0 ? c1 : c2);
      right_chain = (s1 > 0 ? c2 : c1);
    }
    else
    {
      // Interior event endpoints: derive chains from strict crossings, with
      // a boundary-node fallback for short strips without a strict crossing.
      std::vector<int> left, right;
      if (crossed.empty())
      {
        std::set<int> ls, rs;
        for (const auto& be : boundary_edges)
          for (int n : {be.first, be.second})
          {
            double s = orient2(pu, pv, points[n]);
            if (s > kOnLineEps) ls.insert(n);
            else if (s < -kOnLineEps) rs.insert(n);
          }
        std::vector<int> lv(ls.begin(), ls.end()), rv(rs.begin(), rs.end());
        std::sort(lv.begin(), lv.end(),
                  [&](int x, int y) { return ray_t(x) < ray_t(y); });
        std::sort(rv.begin(), rv.end(),
                  [&](int x, int y) { return ray_t(x) < ray_t(y); });
        left = lv; right = rv;
      }
      else
        for (const XEdge& xe : crossed)
        {
          double s0 = orient2(pu, pv, points[xe.a]);
          double s1 = orient2(pu, pv, points[xe.b]);
          if (std::fabs(s0) <= kOnLineEps) s0 = kOnLineEps;
          if (std::fabs(s1) <= kOnLineEps) s1 = kOnLineEps;
          if (s0 > 0.0 && s1 < 0.0)
          { left.push_back(xe.a); right.push_back(xe.b); }
          else if (s0 < 0.0 && s1 > 0.0)
          { left.push_back(xe.b); right.push_back(xe.a); }
          else return false;
        }
      auto dedupe = [](std::vector<int>& c)
      {
        std::vector<int> out;
        for (int n : c)
          if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
        c.swap(out);
      };
      dedupe(left); dedupe(right);
      left_chain.push_back(u);
      for (int n : left) left_chain.push_back(n);
      left_chain.push_back(v);
      right_chain.push_back(u);
      for (int n : right) right_chain.push_back(n);
      right_chain.push_back(v);
      dedupe(left_chain); dedupe(right_chain);
      if (left_chain.size() < 2 || right_chain.size() < 2 ||
          left_chain.front() != u || left_chain.back() != v ||
          right_chain.front() != u || right_chain.back() != v)
        return false;
    }

    // Polygons, both counter-clockwise.  Left ccw order is
    // (u, v, Lk..L1); right ccw order is (u, R1..Rk, v).
    std::vector<int> lpoly(1, u);
    lpoly.push_back(v);
    for (auto it = left_chain.rbegin(); it != left_chain.rend(); ++it)
      if (*it != v && *it != u) lpoly.push_back(*it);
    std::vector<int> rpoly;
    for (int n : right_chain) rpoly.push_back(n);

    std::vector<Tri> new_tris;
    if (!ear_polygon(lpoly, new_tris) || !ear_polygon(rpoly, new_tris))
      return false;

    // ---- Replace the pipe triangulation -----------------------------------
    std::vector<Tri> kept;
    kept.reserve(tris.size() - pipe_tris.size() + new_tris.size());
    for (size_t k = 0; k < tris.size(); ++k)
      if (!pipe_tris.count((int)k)) kept.push_back(tris[k]);
    for (const Tri& t : new_tris) kept.push_back(t);
    tris.swap(kept);

    for (const auto& ie : interior_edges)
      remove_edge_record(ie.first, ie.second);
    for (const Tri& tr : tris)
      for (int q = 0; q < 3; ++q)
        (void)get_edge(tr.v[q], tr.v[(q + 1) % 3]);

    // Interior work nodes (e.g. T-junctions) must remain vertices of the
    // triangulation after the coarse polygon triangulation.
    if (!conformize()) return false;

    if (!edge_id.count(key(u, v))) return false;
    tag_edge(u, v, seg);
    return true;
  }

  // Subdivides one triangle by the interior segment (u,v).  Because the
  // whole segment lies in this single triangle, u and v are two of its
  // vertices and the third vertex w is strictly off the segment; adding
  // edge (u,v) replaces the triangle by the two triangles on either side.
  bool subdivide_one_triangle(int tk, int u, int v, size_t seg)
  {
    int w = -1;
    for (int q = 0; q < 3; ++q)
    {
      int x = tris[tk].v[q];
      if (x != u && x != v)
      {
        if (w >= 0) return false;            // u or v not a triangle vertex
        w = x;
      }
    }
    if (w < 0) return false;
    auto ccw_tri = [&](int a, int b, int c) -> Tri
    {
      if (orient2(points[a], points[b], points[c]) > 0.0)
        return Tri{{a, b, c}};
      return Tri{{a, c, b}};
    };
    tris[tk] = ccw_tri(u, v, w);
    tris.push_back(ccw_tri(u, w, v));
    for (const Tri& tr : {tris[tk], tris.back()})
      for (int q = 0; q < 3; ++q)
        (void)get_edge(tr.v[q], tr.v[(q + 1) % 3]);
    if (!conformize()) return false;
    if (!edge_id.count(key(u, v))) return false;
    tag_edge(u, v, seg);
    return true;
  }

  // Ear-clips a simple ccw polygon into triangles.  The ear test considers
  // every work-mesh point, not only polygon vertices, so a diagonal can never
  // pass through an existing node; interior points are re-inserted by
  // conformize() once the polygon is triangulated.
  bool ear_polygon(const std::vector<int>& poly, std::vector<Tri>& out)
  {
    std::vector<int> p = poly;
    size_t guard = 0;
    while (p.size() > 3)
    {
      bool clipped = false;
      for (size_t i = 0; i < p.size(); ++i)
      {
        size_t ip = (i + p.size() - 1) % p.size();
        size_t in = (i + 1) % p.size();
        int a = p[ip], b = p[i], c = p[in];
        if (orient2(points[a], points[b], points[c]) <= kOnLineEps)
          continue;
        bool empty = true;
        // Polygon vertices other than the ear tips.
        for (size_t k = 0; k < p.size() && empty; ++k)
        {
          if (k == i || k == ip || k == in) continue;
          if (point_in_tri(points[p[k]], points[a], points[b], points[c]))
            empty = false;
        }
        // Every other work point must stay outside the ear as well.
        for (size_t k = 0; k < points.size() && empty; ++k)
        {
          int n = (int)k;
          if (n == a || n == b || n == c) continue;
          if (std::find(p.begin(), p.end(), n) != p.end()) continue;
          if (point_in_tri(points[n], points[a], points[b], points[c]))
            empty = false;
        }
        if (!empty) continue;
        out.push_back(Tri{{a, b, c}});
        p.erase(p.begin() + (long)i);
        clipped = true;
        break;
      }
      if (!clipped || guard++ > poly.size() * poly.size() + 16) return false;
    }
    if (p.size() != 3 ||
        orient2(points[p[0]], points[p[1]], points[p[2]]) <= kOnLineEps)
      return false;
    out.push_back(Tri{{p[0], p[1], p[2]}});
    return true;
  }

  // Restores a conforming triangulation: every work-mesh point must be a
  // triangle vertex.  Points found inside a triangle or on one of its edges
  // are inserted with the standard splitter, which keeps the triangulation
  // valid and preserves every existing edge (the freshly created
  // constraint edge in particular, because the caller guarantees no other
  // node lies on it).
  bool conformize()
  {
    size_t guard = 0;
    for (;;)
    {
      if (guard++ > points.size() + 16) return false;
      std::vector<char> used(points.size(), 0);
      for (const Tri& t : tris)
        for (int q = 0; q < 3; ++q) used[t.v[q]] = 1;
      int missing = -1;
      for (size_t k = 0; k < points.size(); ++k)
        if (!used[k]) { missing = (int)k; break; }
      if (missing < 0) break;
      if (!insert_point(missing)) return false;
    }
    return true;
  }

  // Resolves a planar point to an existing node within kEventEps; creates a
  // new node otherwise.
  int find_or_add_point(const P2& p)
  {
    for (size_t k = 0; k < points.size(); ++k)
      if (std::fabs(points[k][0] - p[0]) <= kEventEps &&
          std::fabs(points[k][1] - p[1]) <= kEventEps)
        return static_cast<int>(k);
    points.push_back(p);
    return static_cast<int>(points.size()) - 1;
  }

private:
  static int third_vertex(const Tri& t, int x, int y)
  {
    for (int q = 0; q < 3; ++q)
      if (t.v[q] != x && t.v[q] != y) return t.v[q];
    return -1;
  }

  static bool on_segment(const P2& a, const P2& b, const P2& p)
  {
    return p[0] >= std::min(a[0], b[0]) - kOnLineEps &&
           p[0] <= std::max(a[0], b[0]) + kOnLineEps &&
           p[1] >= std::min(a[1], b[1]) - kOnLineEps &&
           p[1] <= std::max(a[1], b[1]) + kOnLineEps;
  }

  // Strict containment of p in ccw triangle (a,b,c), boundary included.
  static bool point_in_tri(const P2& p, const P2& a, const P2& b,
                           const P2& c)
  {
    return orient2(a, b, p) >= -kOnLineEps &&
           orient2(b, c, p) >= -kOnLineEps &&
           orient2(c, a, p) >= -kOnLineEps;
  }

  void remove_edge_record(int u, int v)
  {
    auto it = edge_id.find(key(u, v));
    if (it == edge_id.end()) return;
    const int id = it->second;
    const int last = static_cast<int>(edges.size()) - 1;
    if (id != last)
    {
      edges[id] = edges[last];
      edge_tags[id] = edge_tags[last];
      edge_id[key(edges[id][0], edges[id][1])] = id;
    }
    edges.pop_back();
    edge_tags.pop_back();
    edge_id.erase(it);
  }
};

// ---------------------------------------------------------------------------
// Everything the driver needs per source face.
// ---------------------------------------------------------------------------

struct FacePlan
{
  std::vector<size_t>                 global_seg;
  std::vector<std::array<Bary, 2> >   ends;
  std::vector<std::vector<SegEvent> > events;
  // Side subdivision: planar points and corresponding work-mesh node ids,
  // ordered corner o -> corner (o+1)%3, including both corner nodes.
  std::array<std::vector<P2>, 3>  side_points;
  std::array<std::vector<int>, 3> side_nodes;
};

// Resolves an arrangement event point to a work-mesh node.  Corners and
// shared side subdivision points are matched first; interior events are
// matched (or created) through the triangulation's point table.
inline int locate_event_node(FacePlan& plan, FaceTriangulation& ft,
                             const P2& p, bool create_interior)
{
  // Corners.
  for (int c = 0; c < 3; ++c)
    if (std::fabs(ft.points[c][0] - p[0]) <= kEventEps &&
        std::fabs(ft.points[c][1] - p[1]) <= kEventEps)
      return c;

  // Side subdivision points (any of the three sides).
  Bary pb; pb[0] = p[0]; pb[1] = p[1]; pb[2] = 1.0 - p[0] - p[1];
  for (int o = 0; o < 3; ++o)
  {
    for (size_t k = 0; k < plan.side_points[o].size(); ++k)
    {
      const P2& sp = plan.side_points[o][k];
      if (std::fabs(sp[0] - p[0]) <= kEventEps &&
          std::fabs(sp[1] - p[1]) <= kEventEps)
        return plan.side_nodes[o][k];
    }
  }

  // Interior node.
  for (size_t k = 3; k < ft.points.size(); ++k)
    if (std::fabs(ft.points[k][0] - p[0]) <= kEventEps &&
        std::fabs(ft.points[k][1] - p[1]) <= kEventEps)
      return static_cast<int>(k);
  if (create_interior) return ft.find_or_add_point(p);
  return -1;
}

} // namespace SurfaceImprintDetail

//=============================================================================
//  Main entry point
//=============================================================================

/** \brief Imprints drawn segments as real mesh edges.

    @param _source   Compact oriented manifold triangle mesh (no isolated
                     vertices, no deleted slots, finite coordinates, positive
                     area).  Never modified.
    @param _segments In-face segments; barycentric coordinates follow each
                     face's cfv_iter corner order.  A segment's identity is
                     its vector index; reversed or repeated coincident
                     segments keep their own identity.
    @param _result   Receives the remeshed surface.  Must not alias _source.
    @param _map      Receives provenance / segment-to-edge information.

    @throws std::invalid_argument if _result aliases _source, a face index is
            invalid, a barycentric coordinate is non-finite or outside
            [0,1], component sums deviate from 1 by more than 1e-12, or a
            segment's endpoints coincide after normalization.  On any
            validation or construction failure _result and _map are left
            untouched.
*/
template <class Mesh>
void imprint_surface(const Mesh&                         _source,
                     const std::vector<SurfaceSegment>&  _segments,
                     Mesh&                               _result,
                     SurfaceImprintMap&                  _map)
{
  using namespace SurfaceImprintDetail;

  if (static_cast<const void*>(&_source) ==
      static_cast<const void*>(&_result))
    throw std::invalid_argument(
      "imprint_surface: result must not alias source");

  const size_t nfaces = _source.n_faces();
  const size_t nverts = _source.n_vertices();
  const size_t nedges = _source.n_edges();

  // ---- Validate all input before touching any output ----------------------
  std::vector<std::vector<size_t> > face_segs(nfaces);
  std::vector<std::array<Bary, 2> > seg_ends(_segments.size());
  for (size_t s = 0; s < _segments.size(); ++s)
  {
    const SurfaceSegment& seg = _segments[s];
    if (seg.face < 0 || static_cast<size_t>(seg.face) >= nfaces)
      throw std::invalid_argument(
        "imprint_surface: segment references invalid face");
    bool ok0 = false, ok1 = false;
    Bary ba = normalize_bary(seg.a, ok0);
    Bary bb = normalize_bary(seg.b, ok1);
    if (!ok0 || !ok1)
      throw std::invalid_argument(
        "imprint_surface: invalid barycentric coordinates");
    if (Bary::dot(ba - bb, ba - bb) <= 4.0e-20)
      throw std::invalid_argument(
        "imprint_surface: degenerate segment (coincident endpoints)");
    seg_ends[s][0] = ba;
    seg_ends[s][1] = bb;
    face_segs[seg.face].push_back(s);
  }

  // ---- Source topology tables ---------------------------------------------
  struct SideRef { int edge; bool same; };
  std::vector<std::array<SideRef, 3> > face_sides(nfaces);
  std::vector<std::array<int, 3> >     face_corners(nfaces);
  for (size_t fi = 0; fi < nfaces; ++fi)
  {
    typename Mesh::FaceHandle fh(static_cast<int>(fi));
    auto fv = _source.cfv_iter(fh);
    for (int o = 0; o < 3; ++o, ++fv)
      face_corners[fi][o] = fv->idx();
    // cfh_iter need not start at the same corner as cfv_iter, so the
    // halfedge of side o (corner o -> corner o+1) is looked up explicitly.
    for (int o = 0; o < 3; ++o)
    {
      typename Mesh::VertexHandle v0(face_corners[fi][o]);
      typename Mesh::VertexHandle v1(face_corners[fi][(o + 1) % 3]);
      typename Mesh::HalfedgeHandle he = _source.find_halfedge(v0, v1);
      if (!he.is_valid())
        throw std::invalid_argument(
          "imprint_surface: source face halfedge missing");
      const int eidx = _source.edge_handle(he).idx();
      typename Mesh::HalfedgeHandle h0 =
        _source.halfedge_handle(typename Mesh::EdgeHandle(eidx), 0);
      const bool same = _source.from_vertex_handle(h0) ==
                        _source.from_vertex_handle(he);
      face_sides[fi][o] = SideRef{eidx, same};
    }
  }

  // ---- Pass A: per-face pairwise segment arrangement ----------------------
  std::vector<FacePlan> plans(nfaces);
  std::vector<std::vector<double> > edge_params(nedges);
  for (size_t fi = 0; fi < nfaces; ++fi)
  {
    FacePlan& plan = plans[fi];
    plan.global_seg = face_segs[fi];
    plan.ends.reserve(plan.global_seg.size());
    for (size_t s : plan.global_seg) plan.ends.push_back(seg_ends[s]);
    if (!arrange_segments(plan.ends, plan.events))
      throw std::invalid_argument(
        "imprint_surface: segment arrangement could not be constructed");

    // Harvest side events into the shared edge parameter sets.
    for (const auto& evs : plan.events)
      for (const SegEvent& e : evs)
      {
        const int side = which_side(e.p);
        if (side < 0) continue;
        Bary b;
        b[0] = e.p[0]; b[1] = e.p[1]; b[2] = 1.0 - e.p[0] - e.p[1];
        const double tface = side_parameter(side, b);
        if (tface <= kEventEps || tface >= 1.0 - kEventEps) continue;
        const int eidx = face_sides[fi][side].edge;
        const bool same = face_sides[fi][side].same;
        edge_params[eidx].push_back(same ? tface : 1.0 - tface);
      }
  }
  for (size_t e = 0; e < nedges; ++e)
    edge_params[e] = merge_parameters(edge_params[e]);

  // ---- Pass B: constrained working triangulation per face -----------------
  struct SideKey
  {
    int edge;
    double t;
    bool operator<(const SideKey& o) const
    { return edge != o.edge ? edge < o.edge : t < o.t; }
  };
  std::map<SideKey, int> side_vertex;

  // The output mesh is built locally so the caller's data survives any
  // failure; it is committed only after everything succeeds.
  Mesh out;
  const bool want_uv = _source.has_halfedge_texcoords2D();
  if (want_uv) out.request_halfedge_texcoords2D();

  for (size_t vi = 0; vi < nverts; ++vi)
  {
    typename Mesh::VertexHandle vh = out.add_vertex(
      _source.point(typename Mesh::VertexHandle(static_cast<int>(vi))));
    if (static_cast<size_t>(vh.idx()) != vi)
      throw std::invalid_argument(
        "imprint_surface: compact source mesh required");
  }

  std::vector<FaceTriangulation> work(nfaces);
  std::vector<std::vector<int> > node_vertex(nfaces);

  auto resolve_side_vertex = [&](int eidx, double te) -> int
  {
    auto it = side_vertex.lower_bound(SideKey{eidx, te - kEventEps});
    if (it != side_vertex.end() && it->first.edge == eidx &&
        std::fabs(it->first.t - te) <= kEventEps)
      return it->second;
    typename Mesh::HalfedgeHandle h0 = _source.halfedge_handle(typename Mesh::EdgeHandle(eidx), 0);
    typename Mesh::VertexHandle v0 = _source.from_vertex_handle(h0);
    typename Mesh::VertexHandle v1 = _source.to_vertex_handle(h0);
    typename Mesh::Point pos =
      _source.point(v0) * (1.0 - te) + _source.point(v1) * te;
    const int vidx = out.add_vertex(pos).idx();
    side_vertex.insert(std::make_pair(SideKey{eidx, te}, vidx));
    return vidx;
  };

  for (size_t fi = 0; fi < nfaces; ++fi)
  {
    FaceTriangulation& ft = work[fi];
    FacePlan& plan = plans[fi];

    // Side subdivisions from the shared edge parameters.
    for (int o = 0; o < 3; ++o)
    {
      const int eidx = face_sides[fi][o].edge;
      const bool same = face_sides[fi][o].same;
      plan.side_nodes[o].clear();
      plan.side_points[o].clear();
      std::vector<double> ts;
      ts.push_back(0.0);
      for (double te : edge_params[eidx])
      {
        const double tface = same ? te : 1.0 - te;
        if (tface > kEventEps && tface < 1.0 - kEventEps)
          ts.push_back(tface);
      }
      ts.push_back(1.0);
      ts = merge_parameters(ts);
      for (double tface : ts)
      {
        const Bary b = side_point(o, tface);
        const P2 p = to2(b);
        plan.side_points[o].push_back(p);
        int node;
        if (tface <= kEventEps) node = o;
        else if (tface >= 1.0 - kEventEps) node = (o + 1) % 3;
        else
        {
          node = ft.find_or_add_point(p);
          if (static_cast<size_t>(node) >= node_vertex[fi].size())
            node_vertex[fi].resize(node + 1, -1);
          const double te = same ? tface : 1.0 - tface;
          node_vertex[fi][node] = resolve_side_vertex(eidx, te);
        }
        plan.side_nodes[o].push_back(node);
      }
    }

    // Insert the side subdivision points into the working triangulation.
    for (int o = 0; o < 3; ++o)
      for (size_t k = 1; k + 1 < plan.side_nodes[o].size(); ++k)
        if (!ft.insert_point(plan.side_nodes[o][k]))
          throw std::invalid_argument(
            "imprint_surface: edge split point insertion failed");

    // Interior event nodes: identical physical events (multi-segment
    // concurrence) merge within kEventEps; distinct events stay separate.
    std::set<int> interior_nodes;
    for (const auto& evs : plan.events)
      for (const SegEvent& e : evs)
        if (which_side(e.p) < 0)
          interior_nodes.insert(locate_event_node(plan, ft, e.p, true));
    for (int node : interior_nodes)
      if (!ft.insert_point(node))
        throw std::invalid_argument(
          "imprint_surface: segment intersection insertion failed");

    // Resolve output vertices for every work-mesh node.
    typename Mesh::FaceHandle sfh(static_cast<int>(fi));
    typename Mesh::Point corner[3];
    {
      auto fv = _source.cfv_iter(sfh);
      for (int o = 0; o < 3; ++o, ++fv) corner[o] = _source.point(*fv);
    }
    if (node_vertex[fi].size() < ft.points.size())
      node_vertex[fi].resize(ft.points.size(), -1);
    node_vertex[fi][0] = face_corners[fi][0];
    node_vertex[fi][1] = face_corners[fi][1];
    node_vertex[fi][2] = face_corners[fi][2];
    for (size_t node = 3; node < ft.points.size(); ++node)
    {
      if (node_vertex[fi][node] >= 0) continue;       // side node
      const P2& p = ft.points[node];
      const Bary b = Bary(std::array<double, 3>{{
        p[0], p[1], 1.0 - p[0] - p[1]}});
      typename Mesh::Point pos =
        corner[0] * b[0] + corner[1] * b[1] + corner[2] * b[2];
      node_vertex[fi][node] = out.add_vertex(pos).idx();
    }

    // Insert the segment pieces as constrained edges.
    for (size_t li = 0; li < plan.global_seg.size(); ++li)
    {
      const size_t gseg = plan.global_seg[li];
      const std::vector<SegEvent>& evs = plan.events[li];
      std::vector<int> nodes;
      for (const SegEvent& e : evs)
      {
        const int node = locate_event_node(plan, ft, e.p, false);
        if (node < 0)
          throw std::invalid_argument(
            "imprint_surface: segment event node missing");
        if (nodes.empty() || nodes.back() != node) nodes.push_back(node);
      }
      for (size_t k = 1; k < nodes.size(); ++k)
        if (!ft.insert_constraint(nodes[k - 1], nodes[k], gseg))
          throw std::invalid_argument(
            "imprint_surface: constrained edge insertion failed");
    }
  }

  // ---- Emit output faces ---------------------------------------------------
  std::vector<int> out_face_source;
  for (size_t fi = 0; fi < nfaces; ++fi)
  {
    FaceTriangulation& ft = work[fi];
    for (const FaceTriangulation::Tri& t : ft.tris)
    {
      typename Mesh::VertexHandle vh[3];
      for (int k = 0; k < 3; ++k)
        vh[k] = typename Mesh::VertexHandle(node_vertex[fi][t.v[k]]);
      typename Mesh::FaceHandle fh = out.add_face(vh[0], vh[1], vh[2]);
      if (!fh.is_valid())
        throw std::invalid_argument(
          "imprint_surface: output face construction failed");
      out_face_source.push_back(static_cast<int>(fi));
    }
  }

  // corner_bary is indexed by output halfedge index; boundary halfedges stay
  // zero as required.
  std::vector<std::array<double, 3> > out_corner_bary(out.n_halfedges());
  for (size_t fi = 0; fi < nfaces; ++fi)
  {
    FaceTriangulation& ft = work[fi];
    for (const FaceTriangulation::Tri& t : ft.tris)
    {
      Bary bb[3];
      for (int k = 0; k < 3; ++k)
      {
        const P2& p = ft.points[t.v[k]];
        bb[k][0] = p[0]; bb[k][1] = p[1];
        bb[k][2] = 1.0 - p[0] - p[1];
      }
      typename Mesh::VertexHandle vh[3];
      for (int k = 0; k < 3; ++k)
        vh[k] = typename Mesh::VertexHandle(node_vertex[fi][t.v[k]]);
      typename Mesh::HalfedgeHandle he = out.find_halfedge(vh[0], vh[1]);
      if (!he.is_valid())
        throw std::invalid_argument(
          "imprint_surface: output halfedge lookup failed");
      // Walk the three halfedges of this face; each one targets the next
      // corner in the work triangle.
      for (int k = 0; k < 3; ++k)
      {
        const int target = (k + 1) % 3;
        out_corner_bary[he.idx()] =
          std::array<double, 3>{{bb[target][0], bb[target][1],
                                 bb[target][2]}};
        he = out.next_halfedge_handle(he);
      }
    }
  }

  // ---- Edge segment ownership ----------------------------------------------
  std::vector<std::vector<size_t> > out_edge_segments(out.n_edges());
  for (size_t fi = 0; fi < nfaces; ++fi)
  {
    FaceTriangulation& ft = work[fi];
    for (size_t e = 0; e < ft.edges.size(); ++e)
    {
      if (ft.edge_tags[e].empty()) continue;
      const int va = node_vertex[fi][ft.edges[e][0]];
      const int vb = node_vertex[fi][ft.edges[e][1]];
      typename Mesh::HalfedgeHandle he = out.find_halfedge(
        typename Mesh::VertexHandle(va),
        typename Mesh::VertexHandle(vb));
      if (!he.is_valid())
        throw std::invalid_argument(
          "imprint_surface: missing output edge for segment piece");
      const int eidx = out.edge_handle(he).idx();
      for (size_t s : ft.edge_tags[e])
        if (std::find(out_edge_segments[eidx].begin(),
                      out_edge_segments[eidx].end(), s) ==
            out_edge_segments[eidx].end())
          out_edge_segments[eidx].push_back(s);
    }
  }
  for (auto& v : out_edge_segments) std::sort(v.begin(), v.end());

  // ---- Segment chains ------------------------------------------------------
  std::vector<std::vector<int> > chains(_segments.size());
  for (size_t fi = 0; fi < nfaces; ++fi)
  {
    FacePlan& plan = plans[fi];
    FaceTriangulation& ft = work[fi];
    for (size_t li = 0; li < plan.global_seg.size(); ++li)
    {
      const size_t gseg = plan.global_seg[li];
      std::vector<int> chain;
      for (const SegEvent& ev : plan.events[li])
      {
        const int node = locate_event_node(plan, ft, ev.p, false);
        if (node < 0)
          throw std::invalid_argument(
            "imprint_surface: segment chain event missing");
        const int vidx = node_vertex[fi][node];
        if (chain.empty() || chain.back() != vidx) chain.push_back(vidx);
      }
      if (chain.size() < 2)
        throw std::invalid_argument(
          "imprint_surface: segment chain has no edges");
      chains[gseg] = chain;
    }
  }

  // ---- Halfedge texcoords --------------------------------------------------
  if (want_uv)
  {
    for (typename Mesh::FaceIter fit = out.faces_begin();
         fit != out.faces_end(); ++fit)
    {
      const int sfi = out_face_source[fit->idx()];
      typename Mesh::TexCoord2D suv[3];
      // Halfedge k targets corner k+1, so the face corner at corner k is
      // the halfedge from corner k-1 to corner k; look both up explicitly
      // because cfh_iter need not start at the cfv corner.
      for (int k = 0; k < 3; ++k)
      {
        typename Mesh::VertexHandle from(face_corners[sfi][(k + 2) % 3]);
        typename Mesh::VertexHandle to(face_corners[sfi][k]);
        suv[k] = _source.texcoord2D(_source.find_halfedge(from, to));
      }
      for (typename Mesh::FaceHalfedgeIter fhe = out.cfh_iter(*fit);
           fhe.is_valid(); ++fhe)
      {
        const std::array<double, 3>& bc = out_corner_bary[fhe->idx()];
        typename Mesh::TexCoord2D uv;
        for (int comp = 0; comp < 2; ++comp)
        {
          const double v = suv[0][comp] * bc[0] +
                           suv[1][comp] * bc[1] +
                           suv[2][comp] * bc[2];
          uv[comp] =
            static_cast<typename Mesh::TexCoord2D::value_type>(v);
        }
        out.set_texcoord2D(*fhe, uv);
      }
    }
    // Boundary halfedges are not reached by face circulators.
    for (typename Mesh::HalfedgeIter hit = out.halfedges_begin();
         hit != out.halfedges_end(); ++hit)
      if (out.is_boundary(*hit))
        out.set_texcoord2D(*hit,
          typename Mesh::TexCoord2D(0.0, 0.0));
  }

  // ---- Commit --------------------------------------------------------------
  _result = out;
  SurfaceImprintMap delivered;
  delivered.face_source.swap(out_face_source);
  delivered.corner_bary.swap(out_corner_bary);
  delivered.segment_chains.swap(chains);
  delivered.edge_segments.swap(out_edge_segments);
  _map = std::move(delivered);
}

//=============================================================================
} // namespace Utils
} // namespace OpenMesh

//=============================================================================
#endif // OPENMESH_TOOLS_SURFACE_IMPRINT_T_HH
//=============================================================================
