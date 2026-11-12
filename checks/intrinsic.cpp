// Tests for OpenMesh::Utils::make_intrinsic_delaunay().
//
// All expected values are derived independently from triangle unfolding and
// the law of cosines; nothing here reuses the implementation under test.

#include <OpenMesh/Core/Geometry/VectorT.hh>
#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Utils/IntrinsicDelaunayT.hh>
#include <OpenMesh/Tools/Utils/MeshCheckerT.hh>

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

const double PI  = 3.14159265358979323846;
const double EPS = 1e-9;

typedef OpenMesh::TriMesh_ArrayKernelT<>                 Mesh;
typedef OpenMesh::EPropHandleT<double>                   LengthProp;
typedef OpenMesh::EPropHandleT<bool>                     LockProp;
typedef OpenMesh::EPropHandleT<double>                   WeightProp;

// A mesh traits different from the default ones (double precision points).
struct DTraits : OpenMesh::DefaultTraits
{
  typedef OpenMesh::Vec3d Point;
  typedef OpenMesh::Vec3d Normal;
};
typedef OpenMesh::TriMesh_ArrayKernelT<DTraits>          DMesh;

// ---------------------------------------------------------------------------
// Independent metric toolkit (law of cosines / Heron, no mesh code).
// ---------------------------------------------------------------------------

double angle_opposite(double a, double b, double c)
{
  double cs = (a*a + b*b - c*c) / (2.0 * a * b);
  if (cs < -1.0) cs = -1.0;
  if (cs >  1.0) cs =  1.0;
  return std::acos(cs);
}

double cotan_angle(double a, double b, double c)
{
  double ang = angle_opposite(a, b, c);
  return 1.0 / std::tan(ang);
}

double face_area(double a, double b, double c)
{
  double s = 0.5 * (a + b + c);
  double v = s * (s - a) * (s - b) * (s - c);
  if (v < 0.0) v = 0.0;
  return std::sqrt(v);
}

template <class M>
OpenMesh::EdgeHandle edge_of(M& m,
                             OpenMesh::VertexHandle u,
                             OpenMesh::VertexHandle v)
{
  OpenMesh::HalfedgeHandle h = m.find_halfedge(u, v);
  assert(h.is_valid());
  return m.edge_handle(h);
}

template <class M>
void init_lengths_from_points(M& m, LengthProp len)
{
  for (typename M::EdgeIter it = m.edges_begin(); it != m.edges_end(); ++it)
  {
    OpenMesh::HalfedgeHandle h = m.halfedge_handle(*it, 0);
    m.property(len, *it) =
      (m.point(m.to_vertex_handle(h)) -
       m.point(m.from_vertex_handle(h))).norm();
  }
}

// Intrinsic invariants that a flip sequence must preserve.
struct Invariants
{
  std::vector<double> vertex_angle_sum;
  double              total_area;
  double              boundary_length;
};

template <class M>
Invariants capture_invariants(const M& m, LengthProp len)
{
  Invariants inv;
  inv.vertex_angle_sum.assign(m.n_vertices(), 0.0);
  inv.total_area      = 0.0;
  inv.boundary_length = 0.0;

  for (typename M::ConstFaceIter f = m.faces_begin(); f != m.faces_end(); ++f)
  {
    OpenMesh::HalfedgeHandle h  = m.halfedge_handle(*f);
    OpenMesh::HalfedgeHandle n  = m.next_halfedge_handle(h);
    OpenMesh::HalfedgeHandle nn = m.next_halfedge_handle(n);

    double l_h  = m.property(len, m.edge_handle(h));
    double l_n  = m.property(len, m.edge_handle(n));
    double l_nn = m.property(len, m.edge_handle(nn));

    // Corner at to(h): adjacent sides l_h, l_n; opposite side l_nn.
    inv.vertex_angle_sum[m.to_vertex_handle(h).idx()] +=
      angle_opposite(l_h, l_n, l_nn);
    inv.vertex_angle_sum[m.to_vertex_handle(n).idx()] +=
      angle_opposite(l_n, l_nn, l_h);
    inv.vertex_angle_sum[m.to_vertex_handle(nn).idx()] +=
      angle_opposite(l_nn, l_h, l_n);

    inv.total_area += face_area(l_h, l_n, l_nn);
  }

  for (typename M::ConstEdgeIter e = m.edges_begin(); e != m.edges_end(); ++e)
    if (m.is_boundary(*e))
      inv.boundary_length += m.property(len, *e);

  return inv;
}

void check_invariants(const Invariants& before, const Invariants& after)
{
  assert(before.vertex_angle_sum.size() == after.vertex_angle_sum.size());
  for (size_t i = 0; i < before.vertex_angle_sum.size(); ++i)
    assert(std::fabs(before.vertex_angle_sum[i] -
                     after.vertex_angle_sum[i]) < EPS);
  assert(std::fabs(before.total_area - after.total_area) < EPS);
  assert(std::fabs(before.boundary_length - after.boundary_length) < EPS);
}

// Independently recompute every cotangent weight from the edge lengths and
// compare against the property written by the implementation.
template <class M>
void check_weights_independently(const M& m, LengthProp len, WeightProp w)
{
  for (typename M::ConstEdgeIter e = m.edges_begin(); e != m.edges_end(); ++e)
  {
    double expected = 0.0;
    OpenMesh::HalfedgeHandle h0 = m.halfedge_handle(*e, 0);
    OpenMesh::HalfedgeHandle h1 = m.halfedge_handle(*e, 1);
    double le = m.property(len, *e);

    if (!m.is_boundary(h0))
    {
      OpenMesh::HalfedgeHandle n  = m.next_halfedge_handle(h0);
      OpenMesh::HalfedgeHandle nn = m.next_halfedge_handle(n);
      expected += 0.5 * cotan_angle(m.property(len, m.edge_handle(n)),
                                    m.property(len, m.edge_handle(nn)), le);
    }
    if (!m.is_boundary(h1))
    {
      OpenMesh::HalfedgeHandle n  = m.next_halfedge_handle(h1);
      OpenMesh::HalfedgeHandle nn = m.next_halfedge_handle(n);
      expected += 0.5 * cotan_angle(m.property(len, m.edge_handle(n)),
                                    m.property(len, m.edge_handle(nn)), le);
    }
    assert(std::fabs(expected - m.property(w, *e)) < EPS);
  }
}

// Every unlocked interior edge must satisfy the local Delaunay criterion.
template <class M>
void check_final_delaunay(const M& m, LengthProp len, LockProp lk)
{
  for (typename M::ConstEdgeIter e = m.edges_begin(); e != m.edges_end(); ++e)
  {
    if (m.is_boundary(*e) || m.property(lk, *e))
      continue;
    OpenMesh::HalfedgeHandle h0 = m.halfedge_handle(*e, 0);
    OpenMesh::HalfedgeHandle h1 = m.halfedge_handle(*e, 1);
    double le = m.property(len, *e);

    OpenMesh::HalfedgeHandle n0  = m.next_halfedge_handle(h0);
    OpenMesh::HalfedgeHandle n0n = m.next_halfedge_handle(n0);
    OpenMesh::HalfedgeHandle n1  = m.next_halfedge_handle(h1);
    OpenMesh::HalfedgeHandle n1n = m.next_halfedge_handle(n1);

    double sum = angle_opposite(m.property(len, m.edge_handle(n0)),
                                m.property(len, m.edge_handle(n0n)), le)
               + angle_opposite(m.property(len, m.edge_handle(n1)),
                                m.property(len, m.edge_handle(n1n)), le);
    assert(sum <= PI + EPS);
  }
}

// Assemble the discrete cotangent Laplacian L from the intrinsic weights and
// apply it to the constant vector of ones: row sums must vanish.
template <class M>
void check_laplace_kills_constants(const M& m, WeightProp w)
{
  size_t n = m.n_vertices();
  std::vector<double> diagonal(n, 0.0);
  std::vector<double> off_sum(n, 0.0);

  for (typename M::ConstEdgeIter e = m.edges_begin(); e != m.edges_end(); ++e)
  {
    OpenMesh::HalfedgeHandle h = m.halfedge_handle(*e, 0);
    int u = m.from_vertex_handle(h).idx();
    int v = m.to_vertex_handle(h).idx();
    double weight = m.property(w, *e);
    diagonal[u] += weight;
    diagonal[v] += weight;
    off_sum[u]  += weight;
    off_sum[v]  += weight;
  }
  for (size_t i = 0; i < n; ++i)
    assert(std::fabs(diagonal[i] - off_sum[i]) < 1e-12);
}

// ---------------------------------------------------------------------------
// Folded quadrilateral: the canonical intrinsic-metric case.
// ---------------------------------------------------------------------------

void test_folded_quad()
{
  Mesh m;
  Mesh::VertexHandle a = m.add_vertex(Mesh::Point(-1, 0, 0));
  Mesh::VertexHandle b = m.add_vertex(Mesh::Point( 0, 0.2f, 0));
  Mesh::VertexHandle c = m.add_vertex(Mesh::Point( 1, 0, 0));
  Mesh::VertexHandle d = m.add_vertex(Mesh::Point( 0, 0, 2));
  assert(m.add_face(a, b, c).is_valid());
  assert(m.add_face(a, c, d).is_valid());

  LengthProp len; WeightProp w; LockProp lk;
  m.add_property(len); m.add_property(w); m.add_property(lk);
  init_lengths_from_points(m, len);
  for (Mesh::EdgeIter e = m.edges_begin(); e != m.edges_end(); ++e)
    m.property(lk, *e) = false;

  // Other vertex/face properties must survive the flips untouched.
  OpenMesh::VPropHandleT<double> vmark;
  OpenMesh::FPropHandleT<int>    fmark;
  m.add_property(vmark); m.add_property(fmark);
  for (Mesh::VertexIter vv = m.vertices_begin();
       vv != m.vertices_end(); ++vv)
    m.property(vmark, *vv) = 3.0 * vv->idx() + 0.5;
  for (Mesh::FaceIter ff = m.faces_begin(); ff != m.faces_end(); ++ff)
    m.property(fmark, *ff) = 100 + ff->idx();

  OpenMesh::EdgeHandle ac = edge_of(m, a, c);

  // Independently: the two angles opposite ac already sum to more than pi.
  {
    double lab = m.property(len, edge_of(m, a, b));
    double lbc = m.property(len, edge_of(m, b, c));
    double lac = m.property(len, ac);
    double lad = m.property(len, edge_of(m, a, d));
    double lcd = m.property(len, edge_of(m, c, d));
    double sum = angle_opposite(lab, lbc, lac)
               + angle_opposite(lad, lcd, lac);
    assert(sum > PI + 1e-10);
  }

  Invariants before = capture_invariants(m, len);

  OpenMesh::Utils::IntrinsicDelaunayReport rep =
    OpenMesh::Utils::make_intrinsic_delaunay(m, len, lk, w);

  assert(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());
  assert(rep.flips == 1);
  assert(rep.remaining_non_delaunay.empty());
  assert(m.n_vertices() == 4 && m.n_edges() == 5 && m.n_faces() == 2);

  // The rewired edge slot now connects b and d.
  OpenMesh::HalfedgeHandle hbd = m.find_halfedge(b, d);
  if (!hbd.is_valid()) hbd = m.find_halfedge(d, b);
  assert(hbd.is_valid());
  OpenMesh::EdgeHandle bd = m.edge_handle(hbd);
  assert(bd.idx() == ac.idx());

  // Unfolded diagonal length is 2.2, not the 3D straight-line distance.
  // Tolerance accounts for the float point coordinates the metric came from.
  double new_len = m.property(len, bd);
  assert(std::fabs(new_len - 2.2) < 1e-6);
  assert(std::fabs(new_len - std::sqrt(4.04)) > 1e-3);

  // Both new triangles are valid and every surviving edge is Delaunay.
  check_final_delaunay(m, len, lk);
  check_invariants(before, capture_invariants(m, len));
  check_weights_independently(m, len, w);
  check_laplace_kills_constants(m, w);

  // Vertex and face properties are untouched and handle numbering is stable.
  for (Mesh::VertexIter vv = m.vertices_begin();
       vv != m.vertices_end(); ++vv)
    assert(m.property(vmark, *vv) == 3.0 * vv->idx() + 0.5);
  for (Mesh::FaceIter ff = m.faces_begin(); ff != m.faces_end(); ++ff)
    assert(m.property(fmark, *ff) == 100 + ff->idx());

  // Boundary edges keep a single-sided weight.
  for (Mesh::EdgeIter e = m.edges_begin(); e != m.edges_end(); ++e)
    if (m.is_boundary(*e))
      assert(m.property(w, *e) >= -EPS);

  // A second run on the same mesh performs no further flips.
  OpenMesh::Utils::IntrinsicDelaunayReport rep2 =
    OpenMesh::Utils::make_intrinsic_delaunay(m, len, lk, w);
  assert(rep2.flips == 0);
  assert(rep2.remaining_non_delaunay.empty());
  assert(std::fabs(m.property(len, bd) - 2.2) < 1e-6);

  // Moving the display points while keeping the lengths must not change the
  // intrinsic result: feed the same *initial* metric to a mesh with wildly
  // different points and expect the identical flip sequence and results.
  std::vector<double> initial_metric;
  {
    // Reconstruct the original metric of the folded quad independently.
    double lab = std::sqrt(1.04), lbc = std::sqrt(1.04), lac = 2.0;
    double lad = std::sqrt(5.0),  lcd = std::sqrt(5.0);
    initial_metric.push_back(lab);   // edge index 0: ab
    initial_metric.push_back(lbc);   // edge index 1: bc
    initial_metric.push_back(lac);   // edge index 2: ac
    initial_metric.push_back(lad);   // edge index 3: ad
    initial_metric.push_back(lcd);   // edge index 4: cd
  }
  Mesh m2;
  Mesh::VertexHandle a2 = m2.add_vertex(Mesh::Point( 40, -7,  3));
  Mesh::VertexHandle b2 = m2.add_vertex(Mesh::Point(-9,  2, 18));
  Mesh::VertexHandle c2 = m2.add_vertex(Mesh::Point( 7, 33, -5));
  Mesh::VertexHandle d2 = m2.add_vertex(Mesh::Point( 0,  0,  9));
  assert(m2.add_face(a2, b2, c2).is_valid());
  assert(m2.add_face(a2, c2, d2).is_valid());
  LengthProp len2; WeightProp w2; LockProp lk2;
  m2.add_property(len2); m2.add_property(w2); m2.add_property(lk2);
  for (Mesh::EdgeIter e2 = m2.edges_begin(); e2 != m2.edges_end(); ++e2)
  {
    m2.property(len2, *e2) = initial_metric[e2->idx()];
    m2.property(lk2,  *e2) = false;
  }
  OpenMesh::Utils::IntrinsicDelaunayReport rep3 =
    OpenMesh::Utils::make_intrinsic_delaunay(m2, len2, lk2, w2);
  assert(rep3.flips == 1);  // same metric, points are irrelevant
  for (Mesh::EdgeIter e1 = m.edges_begin(), e2 = m2.edges_begin();
       e1 != m.edges_end(); ++e1, ++e2)
  {
    // The two metrics differ only by the float precision of the original
    // point coordinates, so tolerances follow that level.
    assert(std::fabs(m.property(len, *e1) - m2.property(len2, *e2)) < 1e-6);
    assert(std::fabs(m.property(w,   *e1) - m2.property(w2,   *e2)) < 1e-5);
  }

  std::cout << "PASS intrinsic-metric\n";
}

// ---------------------------------------------------------------------------
// Non-flat mesh with a cascade of adjacent flips, plus a cocircular case.
// Uses non-default (double precision point) traits.
// ---------------------------------------------------------------------------

void test_multiple_flips_and_boundary()
{
  DMesh m;
  // Six points on the parabola y = x^2, lifted out of the plane by a
  // z-perturbation: polygon order v0..v5 is convex in the xy-plane.
  std::vector<DMesh::VertexHandle> v;
  const int n = 6;
  const double xs[6] = {-2.5, -1.5, -0.5, 0.5, 1.5, 2.5};
  for (int i = 0; i < n; ++i)
    v.push_back(m.add_vertex(DMesh::Point(xs[i], xs[i]*xs[i],
                                          0.7 * std::sin(double(i)))));
  // Fan triangulation from v0, which is not intrinsic Delaunay.
  for (int i = 1; i < n - 1; ++i)
    assert(m.add_face(v[0], v[i], v[i + 1]).is_valid());

  LengthProp len; WeightProp w; LockProp lk;
  m.add_property(len); m.add_property(w); m.add_property(lk);
  // Metric = planar parabola distances, deliberately not the 3D distances.
  for (DMesh::EdgeIter e = m.edges_begin(); e != m.edges_end(); ++e)
  {
    DMesh::HalfedgeHandle h = m.halfedge_handle(*e, 0);
    DMesh::Point p = m.point(m.from_vertex_handle(h));
    DMesh::Point q = m.point(m.to_vertex_handle(h));
    double dx = q[0] - p[0];
    double dy = q[1] - p[1];
    m.property(len, *e) = std::sqrt(dx*dx + dy*dy);
    m.property(lk,  *e) = false;
  }

  Invariants before = capture_invariants(m, len);
  size_t boundary_before = 0;
  for (DMesh::EdgeIter e = m.edges_begin(); e != m.edges_end(); ++e)
    if (m.is_boundary(*e)) ++boundary_before;

  OpenMesh::Utils::IntrinsicDelaunayReport rep =
    OpenMesh::Utils::make_intrinsic_delaunay(m, len, lk, w);

  assert(OpenMesh::Utils::MeshCheckerT<DMesh>(m).check());
  assert(rep.flips >= 2);                      // cascade of adjacent flips
  assert(rep.remaining_non_delaunay.empty());
  check_final_delaunay(m, len, lk);
  check_invariants(before, capture_invariants(m, len));
  check_weights_independently(m, len, w);
  check_laplace_kills_constants(m, w);

  size_t boundary_after = 0;
  for (DMesh::EdgeIter e = m.edges_begin(); e != m.edges_end(); ++e)
    if (m.is_boundary(*e)) ++boundary_after;
  assert(boundary_before == boundary_after);   // boundary edges never flip

  // Cocircular quadrilateral: the diagonal of a rectangle must be kept,
  // because the opposite angles sum to exactly pi.  Exact double lengths
  // are assigned so the metric is genuinely cocircular.
  Mesh r;
  Mesh::VertexHandle p0 = r.add_vertex(Mesh::Point(0, 0, 0));
  Mesh::VertexHandle p1 = r.add_vertex(Mesh::Point(2, 0, 0));
  Mesh::VertexHandle p2 = r.add_vertex(Mesh::Point(2, 1, 0));
  Mesh::VertexHandle p3 = r.add_vertex(Mesh::Point(0, 1, 0));
  assert(r.add_face(p0, p1, p2).is_valid());
  assert(r.add_face(p0, p2, p3).is_valid());
  LengthProp rlen; WeightProp rw; LockProp rlk;
  r.add_property(rlen); r.add_property(rw); r.add_property(rlk);
  init_lengths_from_points(r, rlen);
  for (Mesh::EdgeIter e = r.edges_begin(); e != r.edges_end(); ++e)
    r.property(rlk, *e) = false;
  r.property(rlen, edge_of(r, p0, p1)) = 2.0;
  r.property(rlen, edge_of(r, p1, p2)) = 1.0;
  r.property(rlen, edge_of(r, p0, p3)) = 1.0;
  r.property(rlen, edge_of(r, p2, p3)) = 2.0;
  r.property(rlen, edge_of(r, p0, p2)) = std::sqrt(5.0);

  OpenMesh::EdgeHandle diag = edge_of(r, p0, p2);
  {
    double l01 = r.property(rlen, edge_of(r, p0, p1));
    double l12 = r.property(rlen, edge_of(r, p1, p2));
    double l03 = r.property(rlen, edge_of(r, p0, p3));
    double l23 = r.property(rlen, edge_of(r, p2, p3));
    double ld  = r.property(rlen, diag);
    double sum = angle_opposite(l01, l12, ld) + angle_opposite(l03, l23, ld);
    assert(std::fabs(sum - PI) < 1e-12);
  }
  OpenMesh::Utils::IntrinsicDelaunayReport rrep =
    OpenMesh::Utils::make_intrinsic_delaunay(r, rlen, rlk, rw);
  assert(rrep.flips == 0);
  assert(rrep.remaining_non_delaunay.empty());
  assert(r.find_halfedge(p0, p2).is_valid() ||
         r.find_halfedge(p2, p0).is_valid());
  assert(OpenMesh::Utils::MeshCheckerT<Mesh>(r).check());

  // Two disconnected folded-quad components in one mesh: both must flip.
  Mesh mc;
  Mesh::VertexHandle q[8];
  double qcoords[8][3] = {
    {-1, 0, 0}, {0, 0.2, 0}, {1, 0, 0}, {0, 0, 2},
    {-1, 0, 0}, {0, 0.2, 0}, {1, 0, 0}, {0, 0, 2}
  };
  for (int i = 0; i < 8; ++i)
    q[i] = mc.add_vertex(Mesh::Point(qcoords[i][0],
                                     static_cast<float>(qcoords[i][1]),
                                     qcoords[i][2]));
  assert(mc.add_face(q[0], q[1], q[2]).is_valid());
  assert(mc.add_face(q[0], q[2], q[3]).is_valid());
  assert(mc.add_face(q[4], q[5], q[6]).is_valid());
  assert(mc.add_face(q[4], q[6], q[7]).is_valid());
  LengthProp mlen; WeightProp mw; LockProp mlk;
  mc.add_property(mlen); mc.add_property(mw); mc.add_property(mlk);
  init_lengths_from_points(mc, mlen);
  for (Mesh::EdgeIter e = mc.edges_begin(); e != mc.edges_end(); ++e)
    mc.property(mlk, *e) = false;
  OpenMesh::Utils::IntrinsicDelaunayReport mrep =
    OpenMesh::Utils::make_intrinsic_delaunay(mc, mlen, mlk, mw);
  assert(OpenMesh::Utils::MeshCheckerT<Mesh>(mc).check());
  assert(mrep.flips == 2);
  assert(mrep.remaining_non_delaunay.empty());
  check_final_delaunay(mc, mlen, mlk);
  check_weights_independently(mc, mlen, mw);

  std::cout << "PASS local-delaunay\n";
}

// ---------------------------------------------------------------------------
// Locked illegal edge and topologically unflippable closed tetrahedron.
// ---------------------------------------------------------------------------

void test_locked_and_unflippable()
{
  // Locked bad edge: same folded quad, but ac is forbidden to flip.
  Mesh m;
  Mesh::VertexHandle a = m.add_vertex(Mesh::Point(-1, 0, 0));
  Mesh::VertexHandle b = m.add_vertex(Mesh::Point( 0, 0.2f, 0));
  Mesh::VertexHandle c = m.add_vertex(Mesh::Point( 1, 0, 0));
  Mesh::VertexHandle d = m.add_vertex(Mesh::Point( 0, 0, 2));
  assert(m.add_face(a, b, c).is_valid());
  assert(m.add_face(a, c, d).is_valid());

  LengthProp len; WeightProp w; LockProp lk;
  m.add_property(len); m.add_property(w); m.add_property(lk);
  init_lengths_from_points(m, len);
  for (Mesh::EdgeIter e = m.edges_begin(); e != m.edges_end(); ++e)
    m.property(lk, *e) = false;

  OpenMesh::EdgeHandle ac = edge_of(m, a, c);
  m.property(lk, ac) = true;

  double lab = m.property(len, edge_of(m, a, b));
  double lbc = m.property(len, edge_of(m, b, c));
  double lad = m.property(len, edge_of(m, a, d));
  double lcd = m.property(len, edge_of(m, c, d));
  double lac = m.property(len, ac);
  double expected_locked_weight =
    0.5 * (cotan_angle(lab, lbc, lac) + cotan_angle(lad, lcd, lac));
  assert(expected_locked_weight < 0.0);   // illegal edge => negative weight

  OpenMesh::Utils::IntrinsicDelaunayReport rep =
    OpenMesh::Utils::make_intrinsic_delaunay(m, len, lk, w);
  assert(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());
  assert(rep.flips == 0);
  assert(rep.remaining_non_delaunay.size() == 1);
  assert(rep.remaining_non_delaunay[0].edge_idx == ac.idx());
  assert(rep.remaining_non_delaunay[0].reason ==
         OpenMesh::Utils::IntrinsicDelaunayReport::Locked);
  // The negative weight is reported, never clamped to zero.
  assert(std::fabs(m.property(w, ac) - expected_locked_weight) < EPS);
  assert(m.property(w, ac) < 0.0);
  // Topology is untouched.
  assert(m.find_halfedge(a, c).is_valid() || m.find_halfedge(c, a).is_valid());
  assert(m.n_edges() == 5 && m.n_faces() == 2);

  // Closed tetrahedron: every edge's opposite vertices are already joined,
  // so no edge is topologically flippable, even with an illegal metric.
  Mesh t;
  Mesh::VertexHandle t0 = t.add_vertex(Mesh::Point(0, 0, 0));
  Mesh::VertexHandle t1 = t.add_vertex(Mesh::Point(1, 0, 0));
  Mesh::VertexHandle t2 = t.add_vertex(Mesh::Point(0, 1, 0));
  Mesh::VertexHandle t3 = t.add_vertex(Mesh::Point(0, 0, 1));
  assert(t.add_face(t0, t1, t2).is_valid());
  assert(t.add_face(t0, t2, t3).is_valid());
  assert(t.add_face(t0, t3, t1).is_valid());
  assert(t.add_face(t1, t3, t2).is_valid());

  LengthProp tlen; WeightProp tw; LockProp tlk;
  t.add_property(tlen); t.add_property(tw); t.add_property(tlk);
  for (Mesh::EdgeIter e = t.edges_begin(); e != t.edges_end(); ++e)
  {
    t.property(tlen, *e) = 1.0;       // default: unit edges
    t.property(tlk,  *e) = false;
  }
  OpenMesh::EdgeHandle e02 = edge_of(t, t0, t2);
  t.property(tlen, e02) = 1.9;        // long hinge, strict inequality holds
  assert(!t.is_flip_ok(e02));         // opposite vertices t1,t3 are adjacent

  OpenMesh::Utils::IntrinsicDelaunayReport trep =
    OpenMesh::Utils::make_intrinsic_delaunay(t, tlen, tlk, tw);
  assert(OpenMesh::Utils::MeshCheckerT<Mesh>(t).check());
  assert(trep.flips == 0);
  bool reported = false;
  for (size_t i = 0; i < trep.remaining_non_delaunay.size(); ++i)
    if (trep.remaining_non_delaunay[i].edge_idx == e02.idx())
    {
      assert(trep.remaining_non_delaunay[i].reason ==
             OpenMesh::Utils::IntrinsicDelaunayReport::NotFlippable);
      reported = true;
    }
  assert(reported);

  std::cout << "PASS locks-and-repeat\n";
}

// ---------------------------------------------------------------------------
// Invalid input metrics: exceptions are thrown before any topology change.
// ---------------------------------------------------------------------------

void test_invalid_inputs()
{
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double bad_values[3] = {inf, nan, 0.0};

  for (int k = 0; k < 3; ++k)
  {
    Mesh m;
    Mesh::VertexHandle a = m.add_vertex(Mesh::Point(-1, 0, 0));
    Mesh::VertexHandle b = m.add_vertex(Mesh::Point( 0, 1, 0));
    Mesh::VertexHandle c = m.add_vertex(Mesh::Point( 1, 0, 0));
    Mesh::VertexHandle d = m.add_vertex(Mesh::Point( 0, 0, 2));
    assert(m.add_face(a, b, c).is_valid());
    assert(m.add_face(a, c, d).is_valid());
    LengthProp len; WeightProp w; LockProp lk;
    m.add_property(len); m.add_property(w); m.add_property(lk);
    init_lengths_from_points(m, len);
    for (Mesh::EdgeIter e = m.edges_begin(); e != m.edges_end(); ++e)
      m.property(lk, *e) = false;
    m.property(len, edge_of(m, a, b)) = bad_values[k];

    bool threw = false;
    try {
      OpenMesh::Utils::make_intrinsic_delaunay(m, len, lk, w);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    assert(threw);
    assert(m.n_vertices() == 4 && m.n_edges() == 5 && m.n_faces() == 2);
    assert(m.find_halfedge(a, c).is_valid());
  }

  // Strict triangle inequality violation.
  Mesh m;
  Mesh::VertexHandle a = m.add_vertex(Mesh::Point(-1, 0, 0));
  Mesh::VertexHandle b = m.add_vertex(Mesh::Point( 0, 1, 0));
  Mesh::VertexHandle c = m.add_vertex(Mesh::Point( 1, 0, 0));
  Mesh::VertexHandle d = m.add_vertex(Mesh::Point( 0, 0, 2));
  assert(m.add_face(a, b, c).is_valid());
  assert(m.add_face(a, c, d).is_valid());
  LengthProp len; WeightProp w; LockProp lk;
  m.add_property(len); m.add_property(w); m.add_property(lk);
  init_lengths_from_points(m, len);
  for (Mesh::EdgeIter e = m.edges_begin(); e != m.edges_end(); ++e)
    m.property(lk, *e) = false;
  m.property(len, edge_of(m, a, c)) = 5.0;   // longer than ab + bc

  bool threw = false;
  try {
    OpenMesh::Utils::make_intrinsic_delaunay(m, len, lk, w);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  assert(threw);
  assert(m.n_faces() == 2);
  assert(m.find_halfedge(a, c).is_valid());

  // Negative length.
  m.property(len, edge_of(m, a, c)) = -1.0;
  threw = false;
  try {
    OpenMesh::Utils::make_intrinsic_delaunay(m, len, lk, w);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  assert(threw);
}

} // namespace

int main()
{
  test_folded_quad();
  test_multiple_flips_and_boundary();
  test_locked_and_unflippable();
  test_invalid_inputs();
  std::cout << "PASS cotangent\n";
  std::cout << "all intrinsic Delaunay tests done\n";
  return 0;
}
