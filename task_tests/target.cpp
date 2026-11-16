// ===========================================================================
// Task tests for OpenMesh::Utils::imprint_surface.
//
// Three groups:
//   arrangement : in-face X crossing, T junction, reversed partial overlap,
//                 closed interior ring and dangling short segment.
//   topology    : folded two-sided surface with several shared-edge splits
//                 induced from one side only, and spatially overlapping but
//                 topologically separate shells that must not be welded.
//   attributes  : halfedge texcoord interpolation (with a UV seam), empty
//                 input, failure rollback with sentinel output, repeated
//                 calls, and both double-Point traits configurations.
//
// Every expectation is computed independently of the produced map: coverage
// uses barycentric integration, intersections use hand-coded segment
// intersection, and chain/edge ownership is checked against the inputs.
// ===========================================================================

#include "registry.hpp"

#include <OpenMesh/Tools/Utils/SurfaceImprintT.hh>
#include <OpenMesh/Tools/Utils/MeshCheckerT.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <vector>

namespace {

using OpenMesh::Utils::SurfaceSegment;
using OpenMesh::Utils::SurfaceImprintMap;
using OpenMesh::Utils::imprint_surface;

typedef std::array<double, 3> Bary;

Bary make_bary(double w0, double w1, double w2)
{ return Bary{{w0, w1, w2}}; }

// Two traits with double Points (the second keeps double texture coords).
struct DoubleTraits : OpenMesh::DefaultTraits
{
  typedef OpenMesh::Vec3d Point;
  typedef OpenMesh::Vec3d Normal;
};

struct DoubleTexTraits : OpenMesh::DefaultTraits
{
  typedef OpenMesh::Vec3d Point;
  typedef OpenMesh::Vec3d Normal;
  typedef OpenMesh::Vec2d TexCoord2D;
};

// ---------------------------------------------------------------------------
// Independent reference geometry (never derived from the output map).
// ---------------------------------------------------------------------------

struct P2Ref { double x, y; };

P2Ref sub_ref(P2Ref a, P2Ref b) { return {a.x - b.x, a.y - b.y}; }
double cross_ref(P2Ref a, P2Ref b) { return a.x * b.y - a.y * b.x; }

// Independent intersection of two 2D segments (w0,w1 plane); returns whether
// they meet, with parameters t, u.
bool intersect_ref(P2Ref a, P2Ref b, P2Ref c, P2Ref d,
                   double& t, double& u)
{
  P2Ref d1 = sub_ref(b, a);
  P2Ref d2 = sub_ref(d, c);
  double den = cross_ref(d1, d2);
  if (std::fabs(den) <= 1.0e-12) return false;
  P2Ref ca = sub_ref(c, a);
  t = cross_ref(ca, d2) / den;
  u = cross_ref(ca, d1) / den;
  return t >= 0.0 && t <= 1.0 && u >= 0.0 && u <= 1.0;
}

P2Ref bary_to_xy(const Bary& w) { return {w[0], w[1]}; }

// Independent point-on-segment test in barycentric 3D.
bool point_on_segment_ref(const Bary& p, const Bary& a, const Bary& b,
                          double& t)
{
  double dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
  double rx = p[0] - a[0], ry = p[1] - a[1], rz = p[2] - a[2];
  double c1 = dy * rz - dz * ry;
  double c2 = dz * rx - dx * rz;
  double c3 = dx * ry - dy * rx;
  if (std::fabs(c1) + std::fabs(c2) + std::fabs(c3) > 1.0e-9) return false;
  double len = dx * dx + dy * dy + dz * dz;
  t = (dx * rx + dy * ry + dz * rz) / len;
  return t >= -1.0e-9 && t <= 1.0 + 1.0e-9;
}

// ---------------------------------------------------------------------------
// Shared mesh builders.
// ---------------------------------------------------------------------------

template <class Mesh>
OpenMesh::VertexHandle add_v(Mesh& m, double x, double y, double z)
{ return m.add_vertex(typename Mesh::Point(x, y, z)); }

// Two triangles sharing edge (a,b): face 0 = (a,b,c), face 1 = (b,a,d).
template <class Mesh>
void build_two_faces(Mesh& m,
                     OpenMesh::VertexHandle& a,
                     OpenMesh::VertexHandle& b,
                     OpenMesh::VertexHandle& c,
                     OpenMesh::VertexHandle& d)
{
  a = add_v<Mesh>(m, 0, 0, 0);
  b = add_v<Mesh>(m, 1, 0, 0);
  c = add_v<Mesh>(m, 0, 1, 0);
  d = add_v<Mesh>(m, 0, 0, 1);
  m.add_face(a, b, c);
  m.add_face(b, a, d);
}

// ---------------------------------------------------------------------------
// Generic, independent verification of an imprint result.
// ---------------------------------------------------------------------------

template <class Mesh>
void verify_imprint(Case& q, const Mesh& src,
                    const std::vector<SurfaceSegment>& segs,
                    const Mesh& out, const SurfaceImprintMap& map)
{
  // MeshChecker plus basic map length contracts.
  q.check(OpenMesh::Utils::MeshCheckerT<Mesh>(out).check(),
          "output fails MeshChecker");
  q.check((int)map.face_source.size() == (int)out.n_faces(),
          "face_source length");
  q.check((int)map.corner_bary.size() == (int)out.n_halfedges(),
          "corner_bary length");
  q.check(map.segment_chains.size() == segs.size(), "segment_chains length");
  q.check(map.edge_segments.size() == out.n_edges(),
          "edge_segments length");

  // Original vertices keep index and position.
  for (size_t v = 0; v < src.n_vertices(); ++v)
    q.check(out.point(typename Mesh::VertexHandle((int)v)) ==
            src.point(typename Mesh::VertexHandle((int)v)),
            "original vertex preserved");

  // Every output face belongs to a valid source face; bary coordinates are
  // valid; signed planar coverage per source face equals the unit triangle.
  std::vector<double> area(src.n_faces(), 0.0);
  for (auto fit = out.faces_begin(); fit != out.faces_end(); ++fit)
  {
    const int f = fit->idx();
    const int s = map.face_source[f];
    q.check(s >= 0 && (size_t)s < src.n_faces(), "valid face source");
    double p[3][2];
    int k = 0;
    for (auto he = out.cfh_iter(*fit); he.is_valid(); ++he, ++k)
    {
      const Bary& bc = map.corner_bary[he->idx()];
      double sum = bc[0] + bc[1] + bc[2];
      q.check(std::fabs(sum - 1.0) <= 1.0e-9 &&
              bc[0] >= -1.0e-12 && bc[1] >= -1.0e-12 && bc[2] >= -1.0e-12,
              "valid corner bary");
      p[k][0] = bc[0];
      p[k][1] = bc[1];
    }
    area[s] += 0.5 * ((p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) -
                      (p[1][1] - p[0][1]) * (p[2][0] - p[0][0]));
  }
  for (size_t f = 0; f < src.n_faces(); ++f)
    q.check(std::fabs(area[f] - 0.5) <= 1.0e-9, "exact face coverage");

  // Manifold reciprocity and closed face loops.
  for (auto hit = out.halfedges_begin(); hit != out.halfedges_end(); ++hit)
  {
    auto op = out.opposite_halfedge_handle(*hit);
    q.check(out.opposite_halfedge_handle(op) == *hit,
            "reciprocal opposite halfedge");
    if (!out.is_boundary(*hit))
    {
      auto n1 = out.next_halfedge_handle(*hit);
      auto n2 = out.next_halfedge_handle(n1);
      auto n3 = out.next_halfedge_handle(n2);
      q.check(n3 == *hit, "closed face halfedge loop");
    }
  }

  // Boundary halfedges carry zero bary.
  for (auto hit = out.halfedges_begin(); hit != out.halfedges_end(); ++hit)
    if (out.is_boundary(*hit))
    {
      const Bary& bc = map.corner_bary[hit->idx()];
      q.check(bc[0] == 0.0 && bc[1] == 0.0 && bc[2] == 0.0,
              "zero boundary bary");
    }

  // Chains: ordered, distinct vertices, every consecutive pair is a real
  // edge, and the edge lists that segment (forward consistency).
  for (size_t s = 0; s < segs.size(); ++s)
  {
    const std::vector<int>& ch = map.segment_chains[s];
    q.check(ch.size() >= 2, "chain has edges");
    std::set<int> uniq(ch.begin(), ch.end());
    q.check(uniq.size() == ch.size(), "chain vertices distinct");
    for (size_t k = 1; k < ch.size(); ++k)
    {
      auto he = out.find_halfedge(
        typename Mesh::VertexHandle(ch[k - 1]),
        typename Mesh::VertexHandle(ch[k]));
      q.check(he.is_valid(), "chain edge exists");
      if (he.is_valid())
      {
        int e = out.edge_handle(he).idx();
        const std::vector<size_t>& es = map.edge_segments[e];
        q.check(std::find(es.begin(), es.end(), s) != es.end(),
                "chain segment owns edge");
      }
    }
  }

  // edge_segments: sorted, deduplicated; every listed segment truly covers
  // the edge (independent point-on-segment test), and the segment's chain
  // traverses that edge (reverse consistency).
  for (size_t e = 0; e < out.n_edges(); ++e)
  {
    const std::vector<size_t>& es = map.edge_segments[e];
    for (size_t k = 1; k < es.size(); ++k)
      q.check(es[k] > es[k - 1], "edge_segments sorted unique");
    for (size_t s : es)
    {
      auto eh = typename Mesh::EdgeHandle((int)e);
      auto he = out.halfedge_handle(eh, 0);
      if (out.is_boundary(he)) he = out.opposite_halfedge_handle(he);
      auto fh = out.face_handle(he);
      q.check(fh.is_valid(), "edge has a face");
      Bary ba = make_bary(0, 0, 0), bb = make_bary(0, 0, 0);
      int from = out.from_vertex_handle(he).idx();
      int to   = out.to_vertex_handle(he).idx();
      for (auto fhe = out.cfh_iter(fh); fhe.is_valid(); ++fhe)
      {
        int tv = out.to_vertex_handle(*fhe).idx();
        if (tv == from) ba = map.corner_bary[fhe->idx()];
        if (tv == to)   bb = map.corner_bary[fhe->idx()];
      }
      Bary mid = make_bary(0.5 * (ba[0] + bb[0]),
                           0.5 * (ba[1] + bb[1]),
                           0.5 * (ba[2] + bb[2]));
      double t = 0.0;
      q.check(segs[s].face == map.face_source[fh.idx()] &&
              point_on_segment_ref(mid, segs[s].a, segs[s].b, t),
              "edge geometrically covered by listed segment");
      const std::vector<int>& ch = map.segment_chains[s];
      bool has = false;
      for (size_t k = 1; k < ch.size(); ++k)
        if ((ch[k - 1] == from && ch[k] == to) ||
            (ch[k - 1] == to && ch[k] == from))
        { has = true; break; }
      q.check(has, "edge appears in segment chain");
    }
  }
}

// ===========================================================================
// arrangement group
// ===========================================================================

// Hand-built X crossing in one face, with the intersection computed by the
// independent reference routine, not by the implementation.
int arrangement_x(Case& q)
{
  typedef OpenMesh::TriMesh_ArrayKernelT<DoubleTraits> Mesh;
  Mesh src;
  OpenMesh::VertexHandle a, b, c, d;
  build_two_faces(src, a, b, c, d);

  std::vector<SurfaceSegment> segs;
  segs.push_back({0, make_bary(0.50, 0.50, 0.00),
                     make_bary(0.00, 0.25, 0.75)});
  segs.push_back({0, make_bary(0.25, 0.00, 0.75),
                     make_bary(0.00, 0.75, 0.25)});

  double t = 0.0, u = 0.0;
  bool hit = intersect_ref(bary_to_xy(segs[0].a), bary_to_xy(segs[0].b),
                           bary_to_xy(segs[1].a), bary_to_xy(segs[1].b),
                           t, u);
  q.check(hit && t > 0.0 && t < 1.0 && u > 0.0 && u < 1.0,
          "reference X intersection in interiors");

  Mesh out; SurfaceImprintMap map;
  imprint_surface(src, segs, out, map);
  verify_imprint(q, src, segs, out, map);

  // The intersection must be one vertex lying on both chains.
  P2Ref xp{segs[0].a[0] + (segs[0].b[0] - segs[0].a[0]) * t,
           segs[0].a[1] + (segs[0].b[1] - segs[0].a[1]) * t};
  int common = -1;
  const std::vector<int>& c0 = map.segment_chains[0];
  const std::vector<int>& c1 = map.segment_chains[1];
  for (int v : c0)
    if (std::find(c1.begin(), c1.end(), v) != c1.end()) common = v;
  q.check(common >= 0, "X intersection shared by both chains");
  if (common >= 0)
  {
    const typename Mesh::Point& p =
      out.point(typename Mesh::VertexHandle(common));
    typename Mesh::Point expect =
      src.point(a) * xp.x +
      src.point(b) * xp.y +
      src.point(c) * (1.0 - xp.x - xp.y);
    q.check((p - expect).norm() <= 1.0e-9, "X intersection position");
  }
  return 0;
}

// T junction: one segment ends in the interior of another.
int arrangement_t(Case& q)
{
  typedef OpenMesh::TriMesh_ArrayKernelT<DoubleTraits> Mesh;
  Mesh src;
  OpenMesh::VertexHandle a, b, c, d;
  build_two_faces(src, a, b, c, d);

  std::vector<SurfaceSegment> segs;
  segs.push_back({0, make_bary(0.60, 0.10, 0.30),
                     make_bary(0.10, 0.60, 0.30)});
  Bary tip = make_bary(0.35, 0.35, 0.30);   // lies on segment 0's line
  segs.push_back({0, make_bary(0.70, 0.10, 0.20), tip});

  double tt = 0.0;
  q.check(point_on_segment_ref(tip, segs[0].a, segs[0].b, tt) &&
          tt > 0.0 && tt < 1.0, "reference endpoint lies on segment 0");

  Mesh out; SurfaceImprintMap map;
  imprint_surface(src, segs, out, map);
  verify_imprint(q, src, segs, out, map);

  const std::vector<int>& c0 = map.segment_chains[0];
  const std::vector<int>& c1 = map.segment_chains[1];
  int tipv = c1.back();   // segment 1's b endpoint is the tip on segment 0
  auto where = std::find(c0.begin(), c0.end(), tipv);
  q.check(where != c0.end() && where != c0.begin() &&
          where != c0.end() - 1,
          "T tip splits the other segment internally");
  q.check(c1.back() == tipv, "T tip is a chain endpoint");
  return 0;
}

// Reversed, partially overlapping collinear segments keep distinct
// identities over the shared sub-edges.
int arrangement_reverse_overlap(Case& q)
{
  typedef OpenMesh::TriMesh_ArrayKernelT<DoubleTraits> Mesh;
  Mesh src;
  OpenMesh::VertexHandle a, b, c, d;
  build_two_faces(src, a, b, c, d);

  std::vector<SurfaceSegment> segs;
  segs.push_back({0, make_bary(0.20, 0.20, 0.60),
                     make_bary(0.70, 0.20, 0.10)});
  segs.push_back({0, make_bary(0.60, 0.20, 0.20),
                     make_bary(0.30, 0.20, 0.50)});   // reverse sub-range

  Mesh out; SurfaceImprintMap map;
  imprint_surface(src, segs, out, map);
  verify_imprint(q, src, segs, out, map);

  bool shared = false;
  for (const auto& es : map.edge_segments)
  {
    std::set<size_t> ids(es.begin(), es.end());
    if (ids.count(0) && ids.count(1)) shared = true;
  }
  q.check(shared, "overlap edge keeps both identities");

  const std::vector<int>& c0 = map.segment_chains[0];
  const std::vector<int>& c1 = map.segment_chains[1];
  // The vertices shared by the two collinear chains must appear in opposite
  // orders (the second segment runs in the reverse direction).
  std::vector<int> shared_order;
  for (int v : c0)
    if (std::find(c1.begin(), c1.end(), v) != c1.end())
      shared_order.push_back(v);
  q.check(shared_order.size() >= 2, "reversed overlap shares chain vertices");
  bool reversed = true;
  for (size_t k = 0; k < shared_order.size(); ++k)
    if (std::find(c1.begin(), c1.end(),
                  shared_order[shared_order.size() - 1 - k]) == c1.end())
      reversed = false;
  q.check(reversed, "reversed segment chain order");
  return 0;
}

// Closed interior ring plus an isolated dangling segment.  The ring is not
// a hole: a face exists at its centroid.
int arrangement_ring_and_dangle(Case& q)
{
  typedef OpenMesh::TriMesh_ArrayKernelT<DoubleTraits> Mesh;
  Mesh src;
  OpenMesh::VertexHandle a, b, c, d;
  build_two_faces(src, a, b, c, d);

  Bary r0 = make_bary(0.20, 0.20, 0.60);
  Bary r1 = make_bary(0.30, 0.20, 0.50);
  Bary r2 = make_bary(0.25, 0.30, 0.45);
  std::vector<SurfaceSegment> segs;
  segs.push_back({0, r0, r1});
  segs.push_back({0, r1, r2});
  segs.push_back({0, r2, r0});
  segs.push_back({0, make_bary(0.60, 0.30, 0.10),
                     make_bary(0.55, 0.35, 0.10)});   // dangling

  Mesh out; SurfaceImprintMap map;
  imprint_surface(src, segs, out, map);
  verify_imprint(q, src, segs, out, map);

  Bary centroid = make_bary((r0[0] + r1[0] + r2[0]) / 3.0,
                            (r0[1] + r1[1] + r2[1]) / 3.0, 0.0);
  centroid[2] = 1.0 - centroid[0] - centroid[1];
  bool covered = false;
  for (auto fit = out.faces_begin(); fit != out.faces_end(); ++fit)
  {
    if (map.face_source[fit->idx()] != 0) continue;
    Bary tri[3]; int k = 0;
    for (auto he = out.cfh_iter(*fit); he.is_valid(); ++he, ++k)
      tri[k] = map.corner_bary[he->idx()];
    double d0 = (tri[1][1] - tri[0][1]) * (centroid[0] - tri[0][0]) -
                (tri[1][0] - tri[0][0]) * (centroid[1] - tri[0][1]);
    double d1 = (tri[2][1] - tri[1][1]) * (centroid[0] - tri[1][0]) -
                (tri[2][0] - tri[1][0]) * (centroid[1] - tri[1][1]);
    double d2 = (tri[0][1] - tri[2][1]) * (centroid[0] - tri[2][0]) -
                (tri[0][0] - tri[2][0]) * (centroid[1] - tri[2][1]);
    if ((d0 >= -1e-9 && d1 >= -1e-9 && d2 >= -1e-9) ||
        (d0 <= 1e-9 && d1 <= 1e-9 && d2 <= 1e-9))
    { covered = true; break; }
  }
  q.check(covered, "ring interior is triangulated (not a hole)");
  q.check(map.segment_chains[3].size() >= 2,
          "dangling segment realized as edge chain");
  return 0;
}

// ===========================================================================
// topology group
// ===========================================================================

// Folded two-sided surface; several split points on the shared edge are all
// induced from face 0 only.  Face 1 carries no segment but must be split at
// every shared position, reusing the same vertices.
int topology_shared_edge_splits(Case& q)
{
  typedef OpenMesh::TriMesh_ArrayKernelT<DoubleTraits> Mesh;
  Mesh src;
  OpenMesh::VertexHandle a, b, c, d;
  build_two_faces(src, a, b, c, d);

  std::vector<SurfaceSegment> segs;
  // Endpoints on the shared edge (w2 == 0) at t = 0.2, 0.5, 0.7 along a->b.
  segs.push_back({0, make_bary(0.05, 0.05, 0.90),
                     make_bary(0.80, 0.20, 0.00)});
  segs.push_back({0, make_bary(0.20, 0.30, 0.50),
                     make_bary(0.50, 0.50, 0.00)});
  segs.push_back({0, make_bary(0.10, 0.20, 0.70),
                     make_bary(0.30, 0.70, 0.00)});

  Mesh out; SurfaceImprintMap map;
  imprint_surface(src, segs, out, map);
  verify_imprint(q, src, segs, out, map);

  // The segment-free neighbour's triangle is subdivided by the three shared
  // split points into four triangles.
  int nf1 = 0;
  for (auto fit = out.faces_begin(); fit != out.faces_end(); ++fit)
    if (map.face_source[fit->idx()] == 1) ++nf1;
  q.check(nf1 == 4, "segment-free neighbour split at shared positions");

  // Exactly three new vertices lie on the original shared edge line.
  typename Mesh::Point pa = src.point(a), pb = src.point(b);
  typename Mesh::Point e = pb - pa;
  double elen2 = e.norm() * e.norm();
  int shared_new = 0;
  for (size_t v = src.n_vertices(); v < out.n_vertices(); ++v)
  {
    typename Mesh::Point x = out.point(typename Mesh::VertexHandle((int)v));
    double t = ((x - pa) | e) / elen2;
    typename Mesh::Point perp = x - (pa + e * t);
    if (t > 1.0e-9 && t < 1.0 - 1.0e-9 && perp.norm() <= 1.0e-9)
      ++shared_new;
  }
  q.check(shared_new == 3, "exactly three shared-edge split vertices");
  return 0;
}

// Two disconnected components occupying identical 3D space keep separate
// identities and never weld.
int topology_overlapping_shells(Case& q)
{
  typedef OpenMesh::TriMesh_ArrayKernelT<DoubleTraits> Mesh;
  Mesh src;
  auto a0 = add_v<Mesh>(src, 0, 0, 0);
  auto b0 = add_v<Mesh>(src, 1, 0, 0);
  auto c0 = add_v<Mesh>(src, 0, 1, 0);
  auto a1 = add_v<Mesh>(src, 0, 0, 0);
  auto b1 = add_v<Mesh>(src, 1, 0, 0);
  auto c1 = add_v<Mesh>(src, 0, 1, 0);
  src.add_face(a0, b0, c0);
  src.add_face(a1, b1, c1);

  std::vector<SurfaceSegment> segs;
  segs.push_back({0, make_bary(0.20, 0.30, 0.50),
                     make_bary(0.50, 0.20, 0.30)});
  segs.push_back({1, make_bary(0.20, 0.30, 0.50),
                     make_bary(0.50, 0.20, 0.30)});

  Mesh out; SurfaceImprintMap map;
  imprint_surface(src, segs, out, map);
  verify_imprint(q, src, segs, out, map);

  q.check(out.n_vertices() == 10, "overlapping shells not welded");

  std::set<int> f0v, f1v;
  for (auto fit = out.faces_begin(); fit != out.faces_end(); ++fit)
    for (auto fv = out.cfv_iter(*fit); fv.is_valid(); ++fv)
    {
      if (map.face_source[fit->idx()] == 0) f0v.insert(fv->idx());
      else                                   f1v.insert(fv->idx());
    }
  bool disconnected = true;
  for (auto eit = out.edges_begin(); eit != out.edges_end(); ++eit)
  {
    auto he = out.halfedge_handle(*eit, 0);
    int u = out.from_vertex_handle(he).idx();
    int v = out.to_vertex_handle(he).idx();
    if ((f0v.count(u) && f1v.count(v)) || (f1v.count(u) && f0v.count(v)))
      disconnected = false;
  }
  q.check(disconnected, "shells remain separate components");
  return 0;
}

// ===========================================================================
// attributes group
// ===========================================================================

// Halfedge texcoords are interpolated per face; a UV seam across the shared
// edge is preserved (no opposite-halfedge lookup, no averaging).
int attributes_texcoord_interpolation(Case& q)
{
  typedef OpenMesh::TriMesh_ArrayKernelT<DoubleTexTraits> Mesh;
  Mesh src;
  src.request_halfedge_texcoords2D();
  OpenMesh::VertexHandle a, b, c, d;
  build_two_faces(src, a, b, c, d);

  auto put_uv = [&](OpenMesh::VertexHandle from,
                    OpenMesh::VertexHandle to, double u, double v)
  {
    src.set_texcoord2D(src.find_halfedge(from, to),
                       Mesh::TexCoord2D(u, v));
  };
  put_uv(a, b, 1.0, 2.0);
  put_uv(b, c, 3.0, 5.0);
  put_uv(c, a, 7.0, 11.0);
  put_uv(b, a, 20.0, 21.0);   // deliberately different across the seam
  put_uv(a, d, 22.0, 23.0);
  put_uv(d, b, 24.0, 25.0);

  std::vector<SurfaceSegment> segs;
  segs.push_back({0, make_bary(0.20, 0.20, 0.60),
                     make_bary(0.60, 0.40, 0.00)});

  Mesh out; SurfaceImprintMap map;
  imprint_surface(src, segs, out, map);
  verify_imprint(q, src, segs, out, map);
  q.check(out.has_halfedge_texcoords2D(), "output requests halfedge UVs");

  double maxerr = 0.0;
  for (auto fit = out.faces_begin(); fit != out.faces_end(); ++fit)
  {
    const int s = map.face_source[fit->idx()];
    int corner[3];
    auto sfv = src.cfv_iter(typename Mesh::FaceHandle(s));
    for (int k = 0; k < 3; ++k, ++sfv) corner[k] = sfv->idx();
    Mesh::TexCoord2D suv[3];
    for (int k = 0; k < 3; ++k)
      suv[k] = src.texcoord2D(src.find_halfedge(
        typename Mesh::VertexHandle(corner[(k + 2) % 3]),
        typename Mesh::VertexHandle(corner[k])));
    for (auto fhe = out.cfh_iter(*fit); fhe.is_valid(); ++fhe)
    {
      const Bary& bc = map.corner_bary[fhe->idx()];
      for (int comp = 0; comp < 2; ++comp)
      {
        double expect = suv[0][comp] * bc[0] +
                        suv[1][comp] * bc[1] +
                        suv[2][comp] * bc[2];
        maxerr = std::max(maxerr,
          std::fabs(double(out.texcoord2D(*fhe)[comp]) - expect));
      }
    }
  }
  q.check(maxerr <= 1.0e-5, "interpolated UV accuracy");

  bool seam = false;
  for (auto eit = out.edges_begin(); eit != out.edges_end(); ++eit)
  {
    auto h0 = out.halfedge_handle(*eit, 0);
    auto h1 = out.halfedge_handle(*eit, 1);
    if (out.is_boundary(h0) || out.is_boundary(h1)) continue;
    auto u0 = out.texcoord2D(h0), u1 = out.texcoord2D(h1);
    if (std::fabs(double(u0[0]) - double(u1[0])) +
        std::fabs(double(u0[1]) - double(u1[1])) > 1.0)
      seam = true;
  }
  q.check(seam, "UV seam preserved across shared edge");

  for (auto hit = out.halfedges_begin(); hit != out.halfedges_end(); ++hit)
    if (out.is_boundary(*hit))
    {
      auto uv = out.texcoord2D(*hit);
      q.check(uv[0] == 0.0 && uv[1] == 0.0, "zero boundary UV");
    }
  return 0;
}

// Empty segment list gives an equivalent copy; a failed call leaves the
// caller's result and map untouched; repeated calls are independent.
int attributes_empty_rollback_repeat(Case& q)
{
  typedef OpenMesh::TriMesh_ArrayKernelT<DoubleTraits> Mesh;
  Mesh src;
  OpenMesh::VertexHandle a, b, c, d;
  build_two_faces(src, a, b, c, d);

  Mesh out; SurfaceImprintMap map;
  imprint_surface(src, std::vector<SurfaceSegment>(), out, map);
  q.check(out.n_faces() == src.n_faces() &&
          out.n_vertices() == src.n_vertices(), "empty input equivalent copy");
  verify_imprint(q, src, std::vector<SurfaceSegment>(), out, map);

  Mesh guarded;
  guarded.add_vertex(typename Mesh::Point(9, 9, 9));
  SurfaceImprintMap sentinel;
  sentinel.face_source.push_back(-777);
  sentinel.corner_bary.push_back(make_bary(0.25, 0.25, 0.50));
  size_t v_before = guarded.n_vertices();

  bool threw = false;
  try
  {
    std::vector<SurfaceSegment> bad;
    bad.push_back({99, make_bary(0.2, 0.2, 0.6),
                       make_bary(0.3, 0.3, 0.4)});   // invalid face index
    imprint_surface(src, bad, guarded, sentinel);
  }
  catch (const std::invalid_argument&) { threw = true; }
  q.check(threw, "invalid face index throws");
  q.check(guarded.n_vertices() == v_before &&
          sentinel.face_source.size() == 1 &&
          sentinel.face_source[0] == -777,
          "result and map preserved on failure");

  threw = false;
  try
  {
    std::vector<SurfaceSegment> bad;
    bad.push_back({0, make_bary(0.5, 0.5, 0.5),
                       make_bary(0.2, 0.2, 0.6)});    // sums to 1.5
    imprint_surface(src, bad, guarded, sentinel);
  }
  catch (const std::invalid_argument&) { threw = true; }
  q.check(threw, "invalid barycentric coordinates throw");
  q.check(sentinel.face_source.size() == 1 &&
          sentinel.face_source[0] == -777, "map still preserved");

  Mesh o1, o2;
  SurfaceImprintMap m1, m2;
  std::vector<SurfaceSegment> segs;
  segs.push_back({0, make_bary(0.2, 0.2, 0.6),
                     make_bary(0.6, 0.4, 0.0)});
  imprint_surface(src, segs, o1, m1);
  imprint_surface(src, segs, o2, m2);
  q.check(o1.n_faces() == o2.n_faces() &&
          o1.n_vertices() == o2.n_vertices() &&
          o1.n_edges() == o2.n_edges(), "repeated calls consistent");
  q.check(OpenMesh::Utils::MeshCheckerT<Mesh>(o2).check(),
          "repeated call valid mesh");

  bool alias_throw = false;
  try { imprint_surface(src, segs, src, m1); }
  catch (const std::invalid_argument&) { alias_throw = true; }
  q.check(alias_throw, "aliased result rejected");
  return 0;
}

// The default (float Point) kernel and the double-Point/double-UV kernel both
// exercise the same template path.
int attributes_two_double_traits(Case& q)
{
  {
    typedef OpenMesh::TriMesh_ArrayKernelT<> Mesh;
    Mesh src;
    auto a = add_v<Mesh>(src, 0, 0, 0);
    auto b = add_v<Mesh>(src, 1, 0, 0);
    auto c = add_v<Mesh>(src, 0, 1, 0);
    src.add_face(a, b, c);
    std::vector<SurfaceSegment> segs;
    segs.push_back({0, make_bary(0.2, 0.3, 0.5),
                       make_bary(0.6, 0.2, 0.2)});
    Mesh out; SurfaceImprintMap map;
    imprint_surface(src, segs, out, map);
    verify_imprint(q, src, segs, out, map);
  }
  {
    typedef OpenMesh::TriMesh_ArrayKernelT<DoubleTexTraits> Mesh;
    Mesh src;
    src.request_halfedge_texcoords2D();
    auto a = add_v<Mesh>(src, 0, 0, 0);
    auto b = add_v<Mesh>(src, 1, 0, 0);
    auto c = add_v<Mesh>(src, 0, 1, 0);
    src.add_face(a, b, c);
    src.set_texcoord2D(src.find_halfedge(a, b), Mesh::TexCoord2D(0.1, 0.2));
    src.set_texcoord2D(src.find_halfedge(b, c), Mesh::TexCoord2D(0.3, 0.4));
    src.set_texcoord2D(src.find_halfedge(c, a), Mesh::TexCoord2D(0.5, 0.6));
    std::vector<SurfaceSegment> segs;
    segs.push_back({0, make_bary(0.1, 0.1, 0.8),
                       make_bary(0.4, 0.4, 0.2)});
    Mesh out; SurfaceImprintMap map;
    imprint_surface(src, segs, out, map);
    verify_imprint(q, src, segs, out, map);
    q.check(out.has_halfedge_texcoords2D(), "double UV property present");
  }
  return 0;
}

} // namespace

void register_task_tests(Registry& r)
{
  r.add("arrangement", "x_crossing", arrangement_x);
  r.add("arrangement", "t_junction", arrangement_t);
  r.add("arrangement", "reverse_partial_overlap",
        arrangement_reverse_overlap);
  r.add("arrangement", "ring_and_dangling", arrangement_ring_and_dangle);

  r.add("topology", "shared_edge_splits_one_sided",
        topology_shared_edge_splits);
  r.add("topology", "overlapping_disconnected_shells",
        topology_overlapping_shells);

  r.add("attributes", "halfedge_texcoord_interpolation",
        attributes_texcoord_interpolation);
  r.add("attributes", "empty_rollback_repeat",
        attributes_empty_rollback_repeat);
  r.add("attributes", "double_point_traits",
        attributes_two_double_traits);
}
