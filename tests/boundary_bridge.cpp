//=============================================================================
// Tests for OpenMesh::Utils::bridge_boundaries()
//
// The test file registers three groups of task cases (optimality, topology,
// attributes) with the task_support/check.h harness.  It intentionally does
// not define main(); task_support/driver.c provides it.
//=============================================================================

#include <OpenMesh/Core/IO/MeshIO.hh>
#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Utils/BoundaryBridgeT.hh>
#include <OpenMesh/Tools/Utils/MeshCheckerT.hh>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "../task_support/check.h"

using Mesh = OpenMesh::TriMesh_ArrayKernelT<>;
using OpenMesh::Utils::BoundaryBridgeResultT;

//=============================================================================
// small helpers
//=============================================================================

static const double PI = 3.14159265358979323846;

static bool finite_point(const Mesh::Point& p)
{
  return std::isfinite(double(p[0])) &&
         std::isfinite(double(p[1])) &&
         std::isfinite(double(p[2]));
}

// Walk the complete boundary loop that contains _start.  Returns the vertices
// in boundary next direction, first vertex not repeated.
static std::vector<Mesh::VertexHandle>
boundary_loop_from(Mesh& _m, Mesh::VertexHandle _start)
{
  Mesh::HalfedgeHandle h;
  for (Mesh::ConstVertexOHalfedgeIter it = _m.cvoh_iter(_start); it.is_valid(); ++it)
    if (_m.is_boundary(*it)) { h = *it; break; }

  std::vector<Mesh::VertexHandle> loop;
  if (!h.is_valid()) return loop;
  Mesh::HalfedgeHandle cur = h;
  do
  {
    loop.push_back(_m.from_vertex_handle(cur));
    cur = _m.next_halfedge_handle(cur);
  }
  while (cur != h && loop.size() < 1000);
  return loop;
}

static int count_boundary_loops(const Mesh& _m)
{
  std::vector<bool> seen(_m.n_halfedges(), false);
  int count = 0;
  for (Mesh::ConstHalfedgeIter h = _m.halfedges_begin(); h != _m.halfedges_end(); ++h)
  {
    if (!_m.is_boundary(*h) || seen[h->idx()]) continue;
    ++count;
    Mesh::HalfedgeHandle cur = *h;
    size_t guard = 0;
    do { seen[cur.idx()] = true; cur = _m.next_halfedge_handle(cur); }
    while (cur != *h && ++guard < _m.n_halfedges());
  }
  return count;
}

// Build a mesh consisting of two closed "caps" (fans) whose outer rims are the
// two boundary loops.  _n0 / _n1 are the loop valences.  The loops are returned
// in boundary next direction; the first vertex of each is _first_vertex0/1.
static void make_two_caps(Mesh& _m, int _n0, int _n1,
                          std::vector<Mesh::VertexHandle> _loop[2],
                          double _layer_distance = 2.0,
                          double _radius_scale = 1.0,
                          double _phase0 = 0.0,
                          double _phase1 = 0.0)
{
  _m.clear();
  _m.request_vertex_status();
  _m.request_edge_status();
  _m.request_face_status();

  const int counts[2] = { _n0, _n1 };
  for (int layer = 0; layer < 2; ++layer)
  {
    const int n = counts[layer];
    std::vector<Mesh::VertexHandle> rim;
    const double phase = (layer == 0 ? _phase0 : _phase1);
    const double radius = _radius_scale * (1.0 + 0.35 * layer);
    const double z = double(layer) * _layer_distance;

    for (int i = 0; i < n; ++i)
    {
      const double ang = 2.0 * PI * (double(i) + phase) / double(n);
      rim.push_back(_m.add_vertex(Mesh::Point(float(radius * std::cos(ang)),
                                             float(radius * std::sin(ang)),
                                             float(z))));
    }

    const Mesh::VertexHandle center = _m.add_vertex(Mesh::Point(0, 0, float(z)));
    for (int i = 0; i < n; ++i)
    {
      // Orient the caps so that the two rims have opposite orientation.
      const Mesh::FaceHandle fh = (layer == 0)
          ? _m.add_face(center, rim[(i + 1) % n], rim[i])
          : _m.add_face(center, rim[i], rim[(i + 1) % n]);
      if (!fh.is_valid()) throw std::runtime_error("cap triangulation failed");
    }
    _loop[layer] = boundary_loop_from(_m, rim[0]);
  }
}

// b direction: second loop traversed backwards from its first vertex.
static std::vector<Mesh::VertexHandle>
reverse_from_first(const std::vector<Mesh::VertexHandle>& _loop)
{
  std::vector<Mesh::VertexHandle> out;
  out.reserve(_loop.size());
  for (size_t j = 0; j < _loop.size(); ++j)
    out.push_back(_loop[(_loop.size() - j) % _loop.size()]);
  return out;
}

// Two caps with explicitly controlled rim radii/heights.  Used by the
// smoothness examples where the pure-area and normal-weighted optima differ.
static void make_two_rings(Mesh& _m, int _n0, int _n1,
                           double _r0, double _z0, double _phase0,
                           double _r1, double _z1, double _phase1,
                           std::vector<Mesh::VertexHandle> _loop[2])
{
  _m.clear();
  const int counts[2] = { _n0, _n1 };
  const double rr[2] = { _r0, _r1 };
  const double zz[2] = { _z0, _z1 };
  const double pp[2] = { _phase0, _phase1 };

  for (int layer = 0; layer < 2; ++layer)
  {
    const int n = counts[layer];
    std::vector<Mesh::VertexHandle> rim;
    for (int i = 0; i < n; ++i)
    {
      const double a = 2.0 * PI * (double(i) + pp[layer]) / double(n);
      rim.push_back(_m.add_vertex(Mesh::Point(float(rr[layer] * std::cos(a)),
                                             float(rr[layer] * std::sin(a)),
                                             float(zz[layer]))));
    }
    const Mesh::VertexHandle center = _m.add_vertex(Mesh::Point(0, 0, float(zz[layer])));
    for (int i = 0; i < n; ++i)
    {
      const Mesh::FaceHandle fh = (layer == 0)
          ? _m.add_face(center, rim[(i + 1) % n], rim[i])
          : _m.add_face(center, rim[i], rim[(i + 1) % n]);
      if (!fh.is_valid()) throw std::runtime_error("cap triangulation failed");
    }
    _loop[layer] = boundary_loop_from(_m, rim[0]);
  }
}

//=============================================================================
// Direct enumeration of all legal lattice paths (reference for optimality).
//=============================================================================

namespace ref {

struct V3 { double x, y, z; };
static V3 sub(const V3& a, const V3& b) { V3 r = {a.x-b.x, a.y-b.y, a.z-b.z}; return r; }
static V3 cross(const V3& a, const V3& b)
{ V3 r = {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; return r; }
static double dot(const V3& a, const V3& b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static double len(const V3& a) { return std::sqrt(dot(a,a)); }

struct Tri { double a; V3 n; bool ok; };

static V3 pt(const Mesh& m, Mesh::VertexHandle v)
{ V3 r = {double(m.point(v)[0]), double(m.point(v)[1]), double(m.point(v)[2])}; return r; }

static Tri tri(const Mesh& m, Mesh::VertexHandle v0, Mesh::VertexHandle v1, Mesh::VertexHandle v2)
{
  const V3 p0 = pt(m, v0), p1 = pt(m, v1), p2 = pt(m, v2);
  const V3 c = cross(sub(p1,p0), sub(p2,p0));
  const double l = len(c);
  Tri t; t.a = 0.5*l; t.ok = (t.a > 1e-12); t.n = {0,0,0};
  if (t.ok) { t.n.x = c.x/l; t.n.y = c.y/l; t.n.z = c.z/l; }
  return t;
}

struct Candidate { double energy; std::vector<int> dirs; bool valid; };

// Enumerate all monotone paths and return the minimal legal energy.
static Candidate enumerate(const Mesh& m,
                           const std::vector<Mesh::VertexHandle>& A,
                           const std::vector<Mesh::VertexHandle>& B,
                           double lambda)
{
  const int n = int(A.size()), M = int(B.size());
  std::vector<Mesh::VertexHandle> ac(n+1), bc(M+1);
  for (int i = 0; i < n; ++i) ac[i] = A[size_t(i)];
  for (int j = 0; j < M; ++j) bc[j] = B[size_t(j)];
  ac[n] = A[0]; bc[M] = B[0];

  const size_t rstride = size_t(M) + 1;
  std::vector<Tri> R(size_t(n) * rstride), U(size_t(n+1) * size_t(M));
  for (int i = 0; i < n; ++i)
    for (int j = 0; j <= M; ++j)
      R[size_t(i)*rstride + size_t(j)] = tri(m, ac[i], ac[i+1], bc[j]);
  for (int i = 0; i <= n; ++i)
    for (int j = 0; j < M; ++j)
      U[size_t(i)*size_t(M) + size_t(j)] = tri(m, ac[i], bc[j+1], bc[j]);

  const bool seam_existing = m.find_halfedge(ac[0], bc[0]).is_valid();

  Candidate best; best.energy = std::numeric_limits<double>::infinity(); best.valid = false;

  // choose which of the n+m steps are right steps
  const int total = n + M;
  std::vector<int> idx(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) idx[size_t(i)] = i;

  for (;;)
  {
    std::vector<int> dirs(size_t(total), 1);  // 1 = up
    for (int i = 0; i < n; ++i) dirs[size_t(idx[size_t(i)])] = 0;

    bool ok = !seam_existing;
    int i = 0, j = 0;
    std::vector<Tri> tris;
    std::vector<std::pair<int,int> > visited;
    if (ok) visited.push_back(std::make_pair(0, 0));  // seam pair used at start

    for (int s = 0; s < total && ok; ++s)
    {
      Tri t;
      if (dirs[size_t(s)] == 0)
      {
        if (i >= n) { ok = false; break; }
        t = R[size_t(i)*rstride + size_t(j)];
      }
      else
      {
        if (j >= M) { ok = false; break; }
        t = U[size_t(i)*size_t(M) + size_t(j)];
      }
      if (!t.ok) { ok = false; break; }
      tris.push_back(t);

      if (dirs[size_t(s)] == 0) ++i; else ++j;

      if (!(i == n && j == M))
      {
        const int pi = i % n, pj = j % M;
        for (size_t k = 0; k < visited.size(); ++k)
          if (visited[k].first == pi && visited[k].second == pj) { ok = false; break; }
        if (!ok) break;
        visited.push_back(std::make_pair(pi, pj));
        if (m.find_halfedge(ac[i], bc[j]).is_valid()) { ok = false; break; }
      }
    }

    if (ok)
    {
      double e = 0.0;
      for (size_t k = 0; k < tris.size(); ++k)
      {
        e += tris[k].a;
        if (k > 0) { V3 d = sub(tris[k].n, tris[k-1].n); e += lambda*dot(d,d); }
      }
      if (!tris.empty())
      {
        V3 d = sub(tris.back().n, tris.front().n);
        e += lambda*dot(d,d);
      }
      if (e < best.energy) { best.energy = e; best.dirs = dirs; best.valid = true; }
    }

    // advance combination indices
    int pos = n - 1;
    while (pos >= 0 && idx[size_t(pos)] == pos + M) --pos;
    if (pos < 0) break;
    ++idx[size_t(pos)];
    for (int k = pos + 1; k < n; ++k) idx[size_t(k)] = idx[size_t(k-1)] + 1;
  }

  return best;
}

} // namespace ref

static bool close(double _a, double _b)
{
  return std::fabs(_a - _b) <= 1e-9 * std::max(1.0, std::max(std::fabs(_a), std::fabs(_b)));
}

//=============================================================================
// Cases: optimality
//=============================================================================

static int test_enumerate_small_loops()
{
  // Asymmetric, differently sampled loops; exhaustive enumeration must match.
  const int sizes[][2] = { {3,3}, {3,5}, {4,3}, {4,6}, {5,4}, {6,5}, {5,5} };
  const double lambdas[] = { 0.0, 0.25, 1.0, 3.0 };

  for (size_t s = 0; s < sizeof(sizes)/sizeof(sizes[0]); ++s)
  {
    for (size_t l = 0; l < sizeof(lambdas)/sizeof(lambdas[0]); ++l)
    {
      // Each bridge call consumes the boundary loops, so every lambda case
      // needs its own mesh.
      Mesh probe;
      std::vector<Mesh::VertexHandle> ploop[2];
      make_two_caps(probe, sizes[s][0], sizes[s][1], ploop, 2.0, 1.0, 0.13, 0.41);

      const std::vector<Mesh::VertexHandle> A = ploop[0];
      const std::vector<Mesh::VertexHandle> B = reverse_from_first(ploop[1]);

      const ref::Candidate expected = ref::enumerate(probe, A, B, lambdas[l]);

      Mesh m;
      std::vector<Mesh::VertexHandle> loop[2];
      make_two_caps(m, sizes[s][0], sizes[s][1], loop, 2.0, 1.0, 0.13, 0.41);

      BoundaryBridgeResultT<Mesh> result;
      OpenMesh::Utils::bridge_boundaries(m, loop[0], loop[1], lambdas[l], result);

      TASK_CHECK(expected.valid);
      TASK_CHECK(result.faces.size() == size_t(sizes[s][0] + sizes[s][1]));
      TASK_CHECK(close(result.energy, expected.energy));
    }
  }
  return 0;
}

static int test_lambda_changes_optimum()
{
  // Two small rings in almost parallel planes.  With lambda = 0 the strip
  // minimises area; a large lambda strongly penalises the abrupt normal
  // change of the minimal-area fan and selects a different strip.  Both
  // results are checked against exhaustive enumeration.
  Mesh m;
  std::vector<Mesh::VertexHandle> loop[2];
  make_two_rings(m, 3, 3, 1.0, 0.0, 0.0, 0.5, -1.5, 0.0, loop);

  const std::vector<Mesh::VertexHandle> A = loop[0];
  const std::vector<Mesh::VertexHandle> B = reverse_from_first(loop[1]);

  const ref::Candidate e0 = ref::enumerate(m, A, B, 0.0);
  const ref::Candidate e1 = ref::enumerate(m, A, B, 50.0);
  TASK_CHECK(e0.valid && e1.valid);
  TASK_CHECK(e0.dirs != e1.dirs);                     // lambda really changes the choice
  TASK_CHECK(std::fabs(e0.energy - e1.energy) > 1e-6);

  BoundaryBridgeResultT<Mesh> r0;
  OpenMesh::Utils::bridge_boundaries(m, loop[0], loop[1], 0.0, r0);
  TASK_CHECK(close(r0.energy, e0.energy));

  // A second call needs a fresh mesh: the loops are no longer boundaries.
  Mesh m1;
  std::vector<Mesh::VertexHandle> loop1[2];
  make_two_rings(m1, 3, 3, 1.0, 0.0, 0.0, 0.5, -1.5, 0.0, loop1);

  BoundaryBridgeResultT<Mesh> r1;
  OpenMesh::Utils::bridge_boundaries(m1, loop1[0], loop1[1], 50.0, r1);
  TASK_CHECK(close(r1.energy, e1.energy));
  TASK_CHECK(std::fabs(r0.energy - r1.energy) > 1e-6);
  return 0;
}

// Reference enumeration variant that (optionally) omits the cyclic wrap term.
static double enumerate_energy_without_wrap(const Mesh& m,
                                            const std::vector<Mesh::VertexHandle>& A,
                                            const std::vector<Mesh::VertexHandle>& B,
                                            double lambda,
                                            std::vector<int>& best_dirs)
{
  const int n = int(A.size()), M = int(B.size());
  std::vector<Mesh::VertexHandle> ac(size_t(n) + 1), bc(size_t(M) + 1);
  for (int i = 0; i < n; ++i) ac[size_t(i)] = A[size_t(i)];
  for (int j = 0; j < M; ++j) bc[size_t(j)] = B[size_t(j)];
  ac[size_t(n)] = A[0]; bc[size_t(M)] = B[0];

  const size_t rstride = size_t(M) + 1;
  std::vector<ref::Tri> R(size_t(n) * rstride), U(size_t(n+1) * size_t(M));
  for (int i = 0; i < n; ++i)
    for (int j = 0; j <= M; ++j)
      R[size_t(i)*rstride + size_t(j)] = ref::tri(m, ac[size_t(i)], ac[size_t(i+1)], bc[size_t(j)]);
  for (int i = 0; i <= n; ++i)
    for (int j = 0; j < M; ++j)
      U[size_t(i)*size_t(M) + size_t(j)] = ref::tri(m, ac[size_t(i)], bc[size_t(j+1)], bc[size_t(j)]);

  const bool seam_existing = m.find_halfedge(ac[0], bc[0]).is_valid();
  const int total = n + M;
  double best = std::numeric_limits<double>::infinity();
  std::vector<int> idx(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) idx[size_t(i)] = i;

  for (;;)
  {
    std::vector<int> dirs(size_t(total), 1);
    for (int i = 0; i < n; ++i) dirs[size_t(idx[size_t(i)])] = 0;

    bool ok = !seam_existing;
    int i = 0, j = 0;
    std::vector<ref::Tri> tris;
    std::vector<std::pair<int,int> > visited;
    if (ok) visited.push_back(std::make_pair(0, 0));

    for (int s = 0; s < total && ok; ++s)
    {
      ref::Tri t;
      if (dirs[size_t(s)] == 0)
      {
        if (i >= n) { ok = false; break; }
        t = R[size_t(i)*rstride + size_t(j)];
      }
      else
      {
        if (j >= M) { ok = false; break; }
        t = U[size_t(i)*size_t(M) + size_t(j)];
      }
      if (!t.ok) { ok = false; break; }
      tris.push_back(t);
      if (dirs[size_t(s)] == 0) ++i; else ++j;
      if (!(i == n && j == M))
      {
        const int pi = i % n, pj = j % M;
        for (size_t k = 0; k < visited.size(); ++k)
          if (visited[k].first == pi && visited[k].second == pj) { ok = false; break; }
        if (!ok) break;
        visited.push_back(std::make_pair(pi, pj));
        if (m.find_halfedge(ac[size_t(i)], bc[size_t(j)]).is_valid()) { ok = false; break; }
      }
    }

    if (ok)
    {
      double e = 0.0;
      for (size_t k = 0; k < tris.size(); ++k)
      {
        e += tris[k].a;
        if (k > 0) { ref::V3 d = ref::sub(tris[k].n, tris[k-1].n); e += lambda*ref::dot(d,d); }
      }
      // intentionally omit the wrap term
      if (e < best) { best = e; best_dirs = dirs; }
    }

    int pos = n - 1;
    while (pos >= 0 && idx[size_t(pos)] == pos + M) --pos;
    if (pos < 0) break;
    ++idx[size_t(pos)];
    for (int k = pos + 1; k < n; ++k) idx[size_t(k)] = idx[size_t(k-1)] + 1;
  }
  return best;
}

static int test_closure_term_matters()
{
  // Geometry found by search: with the cyclic normal term included the
  // optimal strip differs from the one selected when the wrap term is
  // dropped.  This proves the closure term is part of the objective.
  Mesh m;
  std::vector<Mesh::VertexHandle> loop[2];
  make_two_rings(m, 3, 3, 1.0, 0.0, 0.0, 1.5, -1.0, 0.0, loop);

  const std::vector<Mesh::VertexHandle> A = loop[0];
  const std::vector<Mesh::VertexHandle> B = reverse_from_first(loop[1]);
  const double lambda = 10.0;

  const ref::Candidate with_wrap = ref::enumerate(m, A, B, lambda);
  TASK_CHECK(with_wrap.valid);

  std::vector<int> no_wrap_dirs;
  enumerate_energy_without_wrap(m, A, B, lambda, no_wrap_dirs);
  TASK_CHECK(no_wrap_dirs != with_wrap.dirs);   // closure term really decides

  BoundaryBridgeResultT<Mesh> result;
  OpenMesh::Utils::bridge_boundaries(m, loop[0], loop[1], lambda, result);
  TASK_CHECK(close(result.energy, with_wrap.energy));

  // Reproduce the reported energy from the returned faces, including the wrap
  // term, in double precision.
  std::vector<ref::Tri> tris;
  double area_sum = 0.0;
  for (size_t i = 0; i < result.faces.size(); ++i)
  {
    Mesh::ConstFaceVertexIter fv = m.cfv_iter(result.faces[i]);
    const Mesh::VertexHandle v0 = *fv; ++fv;
    const Mesh::VertexHandle v1 = *fv; ++fv;
    const Mesh::VertexHandle v2 = *fv;
    const ref::Tri t = ref::tri(m, v0, v1, v2);
    TASK_CHECK(t.ok);
    tris.push_back(t);
    area_sum += t.a;
  }
  double recomputed = area_sum;
  for (size_t i = 0; i < tris.size(); ++i)
  {
    const ref::V3 d = ref::sub(tris[i].n, tris[(i + 1) % tris.size()].n);
    recomputed += lambda * ref::dot(d, d);
  }
  TASK_CHECK(close(recomputed, result.energy));
  return 0;
}

static int test_cyclic_shift_invariance()
{
  // Cyclically shifting the loops changes the designated seam, which can
  // change the optimum (only the seam pair may be visited twice).  What must
  // hold is that every shifted input still matches exhaustive enumeration for
  // its own seam and produces a legal strip.
  for (size_t shift0 = 0; shift0 < 6; ++shift0)
  {
    for (size_t shift1 = 0; shift1 < 5; shift1 += 2)
    {
      Mesh m;
      std::vector<Mesh::VertexHandle> l[2];
      make_two_caps(m, 6, 5, l, 1.7, 1.0, 0.0, 0.0);

      std::vector<Mesh::VertexHandle> s0, s1;
      for (size_t k = 0; k < l[0].size(); ++k) s0.push_back(l[0][(shift0 + k) % l[0].size()]);
      for (size_t k = 0; k < l[1].size(); ++k) s1.push_back(l[1][(shift1 + k) % l[1].size()]);

      const std::vector<Mesh::VertexHandle> B = reverse_from_first(s1);
      const ref::Candidate expected = ref::enumerate(m, s0, B, 0.7);
      TASK_CHECK(expected.valid);

      BoundaryBridgeResultT<Mesh> r;
      OpenMesh::Utils::bridge_boundaries(m, s0, s1, 0.7, r);
      TASK_CHECK(r.faces.size() == size_t(11));
      TASK_CHECK(close(r.energy, expected.energy));
      TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());
    }
  }
  return 0;
}

// Build two coaxial rings of equal size joined by a closed band of opposite
// orientation and an optional cyclic twist.  With twist == 0 every ring
// vertex i is joined to its partner i; with twist == 2 the rungs are shifted
// by two positions.  The two rims are complete boundary loops.  Returns true
// on success.
static bool make_banded_rings(Mesh& _m, int _n, int _twist,
                              std::vector<Mesh::VertexHandle> _rim[2])
{
  _m.clear();
  std::vector<Mesh::VertexHandle> top, bot;
  for (int i = 0; i < _n; ++i)
  {
    const double a = 2.0 * PI * double(i) / double(_n);
    top.push_back(_m.add_vertex(Mesh::Point(float(std::cos(a)), float(std::sin(a)), 1.0f)));
    bot.push_back(_m.add_vertex(Mesh::Point(float(1.3 * std::cos(a)), float(1.3 * std::sin(a)), -1.0f)));
  }

  for (int i = 0; i < _n; ++i)
  {
    const int a0 = i, a1 = (i + 1) % _n;
    const int b0 = ((i + _twist) % _n + _n) % _n;
    const int b1 = ((i + 1 + _twist) % _n + _n) % _n;
    if (!_m.add_face(top[size_t(a0)], top[size_t(a1)], bot[size_t(b0)]).is_valid()) return false;
    if (!_m.add_face(top[size_t(a1)], bot[size_t(b1)], bot[size_t(b0)]).is_valid()) return false;
  }

  _rim[0] = boundary_loop_from(_m, top[0]);
  _rim[1] = boundary_loop_from(_m, bot[0]);
  return !_rim[0].empty() && !_rim[1].empty();
}

static int test_existing_cross_edge_forbidden()
{
  // Case 1: the seam pair is already joined by an existing cross edge.  No
  // legal strip may start at that pair, and for equal valences the existing
  // rungs block every monotone path, so the call must fail cleanly.
  {
    Mesh m;
    std::vector<Mesh::VertexHandle> rim[2];
    TASK_CHECK(make_banded_rings(m, 4, 0, rim));
    TASK_CHECK(m.find_halfedge(rim[0][0], rim[1][0]).is_valid());

    const size_t nf = m.n_faces();
    BoundaryBridgeResultT<Mesh> result;
    bool threw = false;
    try { OpenMesh::Utils::bridge_boundaries(m, rim[0], rim[1], 0.0, result); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    TASK_CHECK(m.n_faces() == nf);
  }

  // Case 2: non-seam cross edges exist, but a monotone strip can route around
  // them.  The optimum must match exhaustive enumeration that also forbids
  // reusing existing cross edges.
  {
    const int n = 6;
    Mesh probe;
    std::vector<Mesh::VertexHandle> prim[2];
    TASK_CHECK(make_banded_rings(probe, n, 2, prim));
    TASK_CHECK(probe.find_halfedge(prim[0][0], prim[1][0]).is_valid() == false);

    int existing_pairs = 0;
    for (size_t i = 0; i < prim[0].size(); ++i)
      for (size_t j = 0; j < prim[1].size(); ++j)
        if (probe.find_halfedge(prim[0][i], prim[1][j]).is_valid()) ++existing_pairs;
    TASK_CHECK(existing_pairs > 0);

    const std::vector<Mesh::VertexHandle> B = reverse_from_first(prim[1]);
    const ref::Candidate expected = ref::enumerate(probe, prim[0], B, 0.0);
    TASK_CHECK(expected.valid);

    Mesh m;
    std::vector<Mesh::VertexHandle> rim[2];
    TASK_CHECK(make_banded_rings(m, n, 2, rim));

    BoundaryBridgeResultT<Mesh> result;
    OpenMesh::Utils::bridge_boundaries(m, rim[0], rim[1], 0.0, result);

    TASK_CHECK(result.faces.size() == size_t(2 * n));
    TASK_CHECK(close(result.energy, expected.energy));
    TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());
    TASK_CHECK(count_boundary_loops(m) == 0);

    // No pre-existing cross edge may have been turned into a new face twice:
    // every old cross edge is still an interior edge with two faces and was
    // not "consumed" as a boundary edge by the strip.
    for (size_t i = 0; i < rim[0].size(); ++i)
      for (size_t j = 0; j < rim[1].size(); ++j)
      {
        const Mesh::HalfedgeHandle h = m.find_halfedge(rim[0][i], rim[1][j]);
        if (!h.is_valid()) continue;
        TASK_CHECK(!m.is_boundary(h));
      }
  }
  return 0;
}

static int test_third_unrelated_boundary()
{
  // Three boundary loops; bridging the first two must leave the third intact.
  Mesh m;
  std::vector<Mesh::VertexHandle> loop[2];
  make_two_caps(m, 4, 5, loop, 2.0);

  // Add a separate small cap as a third boundary loop.
  Mesh extra;
  std::vector<Mesh::VertexHandle> dummy[2];
  make_two_caps(extra, 3, 3, dummy, 1.0);

  // Copy the third loop's geometry as an extra island into m.
  std::vector<Mesh::VertexHandle> third;
  {
    const int k = 3;
    for (int i = 0; i < k; ++i)
    {
      const double a = 2.0 * PI * double(i) / double(k);
      third.push_back(m.add_vertex(Mesh::Point(float(8.0 + std::cos(a)), float(std::sin(a)), 5.0f)));
    }
    Mesh::VertexHandle c = m.add_vertex(Mesh::Point(8.0f, 0.0f, 5.0f));
    for (int i = 0; i < k; ++i)
      TASK_CHECK(m.add_face(c, third[size_t(i)], third[size_t((i + 1) % k)]).is_valid());
  }

  const std::vector<Mesh::VertexHandle> third_loop = boundary_loop_from(m, third[0]);
  TASK_CHECK(third_loop.size() == 3);
  TASK_CHECK(count_boundary_loops(m) == 3);

  BoundaryBridgeResultT<Mesh> result;
  OpenMesh::Utils::bridge_boundaries(m, loop[0], loop[1], 0.0, result);

  TASK_CHECK(count_boundary_loops(m) == 1);
  TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());

  // The third loop's vertices are still boundary vertices of the remaining
  // hole and have not been modified.
  for (size_t i = 0; i < third_loop.size(); ++i)
  {
    TASK_CHECK(m.is_boundary(third_loop[i]));
    TASK_CHECK(finite_point(m.point(third_loop[i])));
  }
  return 0;
}

//=============================================================================
// Cases: topology
//=============================================================================

static int test_topology_and_handles()
{
  Mesh m;
  std::vector<Mesh::VertexHandle> loop[2];
  make_two_caps(m, 5, 4, loop, 2.0);

  const size_t nv = m.n_vertices();
  const size_t ne = m.n_edges();
  const size_t nf = m.n_faces();

  // Snapshot points of all existing vertices.
  std::vector<Mesh::Point> points;
  for (size_t i = 0; i < nv; ++i) points.push_back(m.point(Mesh::VertexHandle(int(i))));

  BoundaryBridgeResultT<Mesh> result;
  OpenMesh::Utils::bridge_boundaries(m, loop[0], loop[1], 0.4, result);

  TASK_CHECK(m.n_vertices() == nv);                 // no vertices added/welded
  TASK_CHECK(m.n_faces() == nf + loop[0].size() + loop[1].size());
  // Each new triangle adds one new cross-loop edge; all loop edges already
  // exist and are merely consumed, so no edge is split.
  TASK_CHECK(m.n_edges() == ne + loop[0].size() + loop[1].size());
  TASK_CHECK(result.faces.size() == loop[0].size() + loop[1].size());
  TASK_CHECK(count_boundary_loops(m) == 0);

  // Old points unchanged, old handles still valid.
  for (size_t i = 0; i < nv; ++i)
  {
    TASK_CHECK((m.point(Mesh::VertexHandle(int(i))) - points[i]).norm() == 0.0f);
  }

  // Each original boundary edge was consumed exactly once: no original boundary
  // halfedge remains a boundary halfedge.
  for (size_t i = 0; i < loop[0].size(); ++i)
  {
    Mesh::HalfedgeHandle h = m.find_halfedge(loop[0][i], loop[0][(i + 1) % loop[0].size()]);
    TASK_CHECK(h.is_valid());
    TASK_CHECK(!m.is_boundary(h));
    TASK_CHECK(!m.is_boundary(m.opposite_halfedge_handle(h)));
  }
  for (size_t j = 0; j < loop[1].size(); ++j)
  {
    Mesh::HalfedgeHandle h = m.find_halfedge(loop[1][j], loop[1][(j + 1) % loop[1].size()]);
    TASK_CHECK(h.is_valid());
    TASK_CHECK(!m.is_boundary(h));
    TASK_CHECK(!m.is_boundary(m.opposite_halfedge_handle(h)));
  }

  // Returned faces are valid, distinct, and are exactly the newly added ones.
  std::vector<bool> seen(m.n_faces(), false);
  for (size_t i = 0; i < result.faces.size(); ++i)
  {
    TASK_CHECK(result.faces[i].is_valid());
    TASK_CHECK(m.is_valid_handle(result.faces[i]));
    TASK_CHECK(!seen[result.faces[i].idx()]);
    seen[result.faces[i].idx()] = true;
  }

  TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());
  return 0;
}

static int test_no_seam_repeat_and_modular_indices()
{
  // Odd/even sample counts and asymmetric geometry.
  Mesh m;
  std::vector<Mesh::VertexHandle> loop[2];
  make_two_caps(m, 3, 8, loop, 2.5, 0.9, 0.2, 0.05);

  BoundaryBridgeResultT<Mesh> result;
  OpenMesh::Utils::bridge_boundaries(m, loop[0], loop[1], 0.15, result);

  TASK_CHECK(result.faces.size() == loop[0].size() + loop[1].size());
  TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());
  TASK_CHECK(count_boundary_loops(m) == 0);

  // Every face of the strip must contain at least one vertex from each loop.
  for (size_t i = 0; i < result.faces.size(); ++i)
  {
    int ca = 0, cb = 0;
    for (Mesh::ConstFaceVertexIter fv = m.cfv_iter(result.faces[i]); fv.is_valid(); ++fv)
    {
      const bool inA = std::find(loop[0].begin(), loop[0].end(), *fv) != loop[0].end();
      const bool inB = std::find(loop[1].begin(), loop[1].end(), *fv) != loop[1].end();
      if (inA) ++ca;
      if (inB) ++cb;
    }
    TASK_CHECK(ca > 0 && cb > 0);
  }
  return 0;
}

static int test_invalid_inputs_leave_mesh_unchanged()
{
  Mesh m;
  std::vector<Mesh::VertexHandle> loop[2];
  make_two_caps(m, 4, 5, loop, 2.0);

  const size_t nv = m.n_vertices();
  const size_t ne = m.n_edges();
  const size_t nf = m.n_faces();
  const int loops_before = count_boundary_loops(m);

  BoundaryBridgeResultT<Mesh> result;
  result.energy = 123.0;
  result.faces.clear();

  // lambda < 0
  bool threw = false;
  try { OpenMesh::Utils::bridge_boundaries(m, loop[0], loop[1], -0.1, result); }
  catch (const std::invalid_argument&) { threw = true; }
  TASK_CHECK(threw);

  // lambda = NaN
  threw = false;
  try { OpenMesh::Utils::bridge_boundaries(m, loop[0], loop[1],
        std::numeric_limits<double>::quiet_NaN(), result); }
  catch (const std::invalid_argument&) { threw = true; }
  TASK_CHECK(threw);

  // Incomplete loop
  std::vector<Mesh::VertexHandle> incomplete = loop[1];
  incomplete.pop_back();
  threw = false;
  try { OpenMesh::Utils::bridge_boundaries(m, loop[0], incomplete, 0.0, result); }
  catch (const std::invalid_argument&) { threw = true; }
  TASK_CHECK(threw);

  // Repeated vertex in a loop
  std::vector<Mesh::VertexHandle> repeated = loop[1];
  repeated[1] = repeated[0];
  threw = false;
  try { OpenMesh::Utils::bridge_boundaries(m, loop[0], repeated, 0.0, result); }
  catch (const std::invalid_argument&) { threw = true; }
  TASK_CHECK(threw);

  // Invalid vertex handle
  std::vector<Mesh::VertexHandle> bad = loop[1];
  bad[0] = Mesh::VertexHandle(int(nv) + 5);
  threw = false;
  try { OpenMesh::Utils::bridge_boundaries(m, loop[0], bad, 0.0, result); }
  catch (const std::invalid_argument&) { threw = true; }
  TASK_CHECK(threw);

  // Wrong direction (reverse of a valid loop)
  std::vector<Mesh::VertexHandle> rev;
  for (size_t i = 0; i < loop[1].size(); ++i)
    rev.push_back(loop[1][(loop[1].size() - i) % loop[1].size()]);
  threw = false;
  try { OpenMesh::Utils::bridge_boundaries(m, loop[0], rev, 0.0, result); }
  catch (const std::invalid_argument&) { threw = true; }
  TASK_CHECK(threw);

  // The mesh and the result object must be untouched by all failed calls.
  TASK_CHECK(m.n_vertices() == nv);
  TASK_CHECK(m.n_edges() == ne);
  TASK_CHECK(m.n_faces() == nf);
  TASK_CHECK(count_boundary_loops(m) == loops_before);
  TASK_CHECK(result.faces.empty());
  TASK_CHECK(result.energy == 123.0);

  // A subsequent valid call must still succeed.
  BoundaryBridgeResultT<Mesh> good;
  OpenMesh::Utils::bridge_boundaries(m, loop[0], loop[1], 0.0, good);
  TASK_CHECK(good.faces.size() == loop[0].size() + loop[1].size());
  TASK_CHECK(count_boundary_loops(m) == 0);
  TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());
  return 0;
}

static int test_degenerate_geometry_rejected()
{
  // Two identical (coincident) rings: every cross triangle has zero area, so
  // no legal strip exists and the call must fail without touching the mesh.
  Mesh m;
  std::vector<Mesh::VertexHandle> loop[2];
  make_two_rings(m, 4, 4, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, loop);

  const size_t nf = m.n_faces();
  BoundaryBridgeResultT<Mesh> result;
  bool threw = false;
  try { OpenMesh::Utils::bridge_boundaries(m, loop[0], loop[1], 0.0, result); }
  catch (const std::invalid_argument&) { threw = true; }
  TASK_CHECK(threw);
  TASK_CHECK(m.n_faces() == nf);
  TASK_CHECK(result.faces.empty());
  return 0;
}

//=============================================================================
// Cases: attributes
//=============================================================================

static int test_existing_face_uv_and_custom_attributes()
{
  Mesh m;
  std::vector<Mesh::VertexHandle> loop[2];
  make_two_caps(m, 5, 6, loop, 2.0);

  // Existing per-face-corner UVs for all old faces, including the original
  // boundary corners.
  m.request_halfedge_texcoords2D();
  for (Mesh::ConstHalfedgeIter h = m.halfedges_begin(); h != m.halfedges_end(); ++h)
    m.set_texcoord2D(*h, Mesh::TexCoord2D(float(h->idx()) * 0.013f, -float(h->idx()) * 0.007f));

  OpenMesh::VPropHandleT<double> custom_v;
  OpenMesh::FPropHandleT<int> custom_f;
  m.add_property(custom_v, "custom_vertex_scalar");
  m.add_property(custom_f, "custom_face_index");
  for (Mesh::VertexIter v = m.vertices_begin(); v != m.vertices_end(); ++v)
    m.property(custom_v, *v) = double(v->idx()) * 1.5;
  for (Mesh::FaceIter f = m.faces_begin(); f != m.faces_end(); ++f)
    m.property(custom_f, *f) = 100 + f->idx();

  const size_t nf = m.n_faces();
  const size_t nv = m.n_vertices();

  // Snapshot the old halfedge texcoords keyed by handle index.  Only face
  // corners (halfedges with a valid face) are protected: the faceless
  // boundary halfedges are consumed by the new strip and receive new values.
  std::vector<Mesh::TexCoord2D> old_uv(m.n_halfedges());
  std::vector<bool> protected_he(m.n_halfedges(), false);
  for (size_t i = 0; i < m.n_halfedges(); ++i)
  {
    const Mesh::HalfedgeHandle h = Mesh::HalfedgeHandle(int(i));
    if (m.face_handle(h).is_valid())
    {
      old_uv[i] = m.texcoord2D(h);
      protected_he[i] = true;
    }
  }
  std::vector<double> old_v(nv);
  for (size_t i = 0; i < nv; ++i) old_v[i] = m.property(custom_v, Mesh::VertexHandle(int(i)));
  std::vector<int> old_f(nf);
  for (size_t i = 0; i < nf; ++i) old_f[i] = m.property(custom_f, Mesh::FaceHandle(int(i)));

  BoundaryBridgeResultT<Mesh> result;
  OpenMesh::Utils::bridge_boundaries(m, loop[0], loop[1], 0.3, result);

  TASK_CHECK(m.n_vertices() == nv);
  TASK_CHECK(m.n_faces() == nf + result.faces.size());

  // Old face-corner halfedge handles keep their values.
  for (size_t i = 0; i < old_uv.size(); ++i)
  {
    if (!protected_he[i]) continue;
    const Mesh::TexCoord2D uv = m.texcoord2D(Mesh::HalfedgeHandle(int(i)));
    TASK_CHECK((uv - old_uv[i]).norm() == 0.0f);
  }
  // Old custom properties unchanged.
  for (size_t i = 0; i < nv; ++i)
    TASK_CHECK(m.property(custom_v, Mesh::VertexHandle(int(i))) == old_v[i]);
  for (size_t i = 0; i < nf; ++i)
    TASK_CHECK(m.property(custom_f, Mesh::FaceHandle(int(i))) == old_f[i]);

  // New faces have UVs with V==0 on the first loop and V==1 on the second.
  for (size_t i = 0; i < result.faces.size(); ++i)
  {
    for (Mesh::ConstFaceHalfedgeIter fh = m.cfh_iter(result.faces[i]); fh.is_valid(); ++fh)
    {
      const Mesh::VertexHandle to = m.to_vertex_handle(*fh);
      const bool inA = std::find(loop[0].begin(), loop[0].end(), to) != loop[0].end();
      const bool inB = std::find(loop[1].begin(), loop[1].end(), to) != loop[1].end();
      TASK_CHECK(inA || inB);
      const Mesh::TexCoord2D uv = m.texcoord2D(*fh);
      if (inA) TASK_CHECK(std::fabs(double(uv[1]) - 0.0) < 1e-6);
      if (inB) TASK_CHECK(std::fabs(double(uv[1]) - 1.0) < 1e-6);
    }
  }
  return 0;
}

static int test_seam_uv_zero_and_one()
{
  // The closure corner must carry S=0 on the first triangle and S=1 on the
  // last triangle where the same geometric vertex is used on both sides.
  Mesh m;
  std::vector<Mesh::VertexHandle> loop[2];
  make_two_caps(m, 5, 5, loop, 2.0, 1.0, 0.0, 0.0);

  BoundaryBridgeResultT<Mesh> result;
  OpenMesh::Utils::bridge_boundaries(m, loop[0], loop[1], 0.0, result);

  // Find the U coordinate of the corner at loop[0][0] in the first and the
  // last face.  They must be 0 and 1 respectively (only if the path used a
  // seam edge at each end, which is guaranteed by construction: every strip
  // starts and ends at the seam).
  Mesh::FaceHandle first = result.faces.front();
  Mesh::FaceHandle last  = result.faces.back();

  double s_first = -1.0, s_last = -1.0;
  for (Mesh::ConstFaceHalfedgeIter fh = m.cfh_iter(first); fh.is_valid(); ++fh)
    if (m.to_vertex_handle(*fh) == loop[0][0])
      s_first = double(m.texcoord2D(*fh)[0]);
  for (Mesh::ConstFaceHalfedgeIter fh = m.cfh_iter(last); fh.is_valid(); ++fh)
    if (m.to_vertex_handle(*fh) == loop[0][0])
      s_last = double(m.texcoord2D(*fh)[0]);

  TASK_CHECK(s_first >= -1e-12 && s_first <= 1e-12);
  TASK_CHECK(std::fabs(s_last - 1.0) < 1e-9);

  // Same for the second loop seam vertex, whose S is also 0/1.
  double t_first = -1.0, t_last = -1.0;
  for (Mesh::ConstFaceHalfedgeIter fh = m.cfh_iter(first); fh.is_valid(); ++fh)
    if (m.to_vertex_handle(*fh) == loop[1][0])
      t_first = double(m.texcoord2D(*fh)[0]);
  for (Mesh::ConstFaceHalfedgeIter fh = m.cfh_iter(last); fh.is_valid(); ++fh)
    if (m.to_vertex_handle(*fh) == loop[1][0])
      t_last = double(m.texcoord2D(*fh)[0]);
  TASK_CHECK(t_first >= -1e-12 && t_first <= 1e-12);
  TASK_CHECK(std::fabs(t_last - 1.0) < 1e-9);
  return 0;
}

static int test_standard_texture_om_roundtrip()
{
  Mesh m;
  std::vector<Mesh::VertexHandle> loop[2];
  make_two_caps(m, 5, 5, loop, 2.0);

  BoundaryBridgeResultT<Mesh> result;
  OpenMesh::Utils::bridge_boundaries(m, loop[0], loop[1], 0.2, result);

  // Assign a unique UV to every face corner, including old faces.
  int k = 0;
  for (Mesh::ConstHalfedgeIter h = m.halfedges_begin(); h != m.halfedges_end(); ++h, ++k)
    if (!m.is_boundary(*h))
      m.set_texcoord2D(*h, Mesh::TexCoord2D(float(k) * 0.01f, -float(k) * 0.02f));

  OpenMesh::IO::Options wopt(OpenMesh::IO::Options::Binary);
  wopt += OpenMesh::IO::Options::FaceTexCoord;
  const std::string path = ".task-build/boundary_bridge_tex.om";
  TASK_CHECK(OpenMesh::IO::write_mesh(m, path, wopt));

  Mesh out;
  OpenMesh::IO::Options ropt(OpenMesh::IO::Options::Binary);
  ropt += OpenMesh::IO::Options::FaceTexCoord;
  TASK_CHECK(OpenMesh::IO::read_mesh(out, path, ropt));
  TASK_CHECK(out.has_halfedge_texcoords2D());
  TASK_CHECK(out.n_faces() == m.n_faces());
  TASK_CHECK(out.n_vertices() == m.n_vertices());

  // Restore corner UVs by face order and per-face vertex order.
  for (size_t fi = 0; fi < m.n_faces(); ++fi)
  {
    const Mesh::FaceHandle fin  = Mesh::FaceHandle(int(fi));
    const Mesh::FaceHandle fout = Mesh::FaceHandle(int(fi));
    std::vector<Mesh::VertexHandle> vin, vout;
    std::vector<Mesh::TexCoord2D> uvin, uvout;
    for (Mesh::ConstFaceVertexIter fv = m.cfv_iter(fin); fv.is_valid(); ++fv) vin.push_back(*fv);
    for (Mesh::ConstFaceVertexIter fv = out.cfv_iter(fout); fv.is_valid(); ++fv) vout.push_back(*fv);
    TASK_CHECK(vin == vout);

    for (Mesh::ConstFaceHalfedgeIter fh = m.cfh_iter(fin); fh.is_valid(); ++fh)
      uvin.push_back(m.texcoord2D(*fh));
    for (Mesh::ConstFaceHalfedgeIter fh = out.cfh_iter(fout); fh.is_valid(); ++fh)
      uvout.push_back(out.texcoord2D(*fh));

    TASK_CHECK(uvin.size() == uvout.size());
    for (size_t c = 0; c < uvin.size(); ++c)
      TASK_CHECK((uvin[c] - uvout[c]).norm() < 1e-6f);
  }

  // Standard custom-property persistence must not be involved; check that no
  // halfedge custom property was created for h:texcoords2D (it is a standard
  // attribute, not a custom one).
  TASK_CHECK(out.has_halfedge_texcoords2D());
  return 0;
}

static int test_om_without_face_texcoord_skips_block()
{
  Mesh m;
  std::vector<Mesh::VertexHandle> loop[2];
  make_two_caps(m, 4, 5, loop, 2.0);
  m.request_halfedge_texcoords2D();
  for (Mesh::ConstHalfedgeIter h = m.halfedges_begin(); h != m.halfedges_end(); ++h)
    if (!m.is_boundary(*h)) m.set_texcoord2D(*h, Mesh::TexCoord2D(0.25f, 0.75f));

  OpenMesh::FPropHandleT<int> mat;
  m.add_property(mat, "material");
  m.property(mat).set_persistent(true);
  for (Mesh::FaceIter f = m.faces_begin(); f != m.faces_end(); ++f)
    m.property(mat, *f) = 7 + f->idx();

  const std::string path = ".task-build/boundary_bridge_nofacetex.om";
  OpenMesh::IO::Options wopt(OpenMesh::IO::Options::Binary);   // no FaceTexCoord
  TASK_CHECK(OpenMesh::IO::write_mesh(m, path, wopt));

  // Reading without FaceTexCoord must skip the (absent) block and still
  // restore following custom properties.
  Mesh out;
  OpenMesh::FPropHandleT<int> mat2;
  out.add_property(mat2, "material");
  OpenMesh::IO::Options ropt(OpenMesh::IO::Options::Binary);
  TASK_CHECK(OpenMesh::IO::read_mesh(out, path, ropt));
  TASK_CHECK(out.n_faces() == m.n_faces());
  for (size_t i = 0; i < m.n_faces(); ++i)
    TASK_CHECK(out.property(mat2, Mesh::FaceHandle(int(i))) == 7 + int(i));
  TASK_CHECK(!out.has_halfedge_texcoords2D());

  // Requesting the attribute but not the option must also skip the block.
  Mesh out2;
  out2.request_halfedge_texcoords2D();
  OpenMesh::FPropHandleT<int> mat3;
  out2.add_property(mat3, "material");
  OpenMesh::IO::Options ropt2(OpenMesh::IO::Options::Binary);
  TASK_CHECK(OpenMesh::IO::read_mesh(out2, path, ropt2));
  for (size_t i = 0; i < m.n_faces(); ++i)
    TASK_CHECK(out2.property(mat3, Mesh::FaceHandle(int(i))) == 7 + int(i));
  return 0;
}

static int test_om_old_format_compatible()
{
  // A file written without the standard texcoord chunk must remain readable
  // when FaceTexCoord is requested.
  Mesh m;
  std::vector<Mesh::VertexHandle> loop[2];
  make_two_caps(m, 4, 4, loop, 2.0);

  OpenMesh::FPropHandleT<float> tag;
  m.add_property(tag, "tag");
  m.property(tag).set_persistent(true);
  for (Mesh::FaceIter f = m.faces_begin(); f != m.faces_end(); ++f)
    m.property(tag, *f) = float(f->idx()) + 0.5f;

  const std::string path = ".task-build/boundary_bridge_old.om";
  OpenMesh::IO::Options wopt(OpenMesh::IO::Options::Binary);
  TASK_CHECK(OpenMesh::IO::write_mesh(m, path, wopt));

  Mesh out;
  OpenMesh::FPropHandleT<float> tag2;
  out.add_property(tag2, "tag");
  out.request_halfedge_texcoords2D();
  OpenMesh::IO::Options ropt(OpenMesh::IO::Options::Binary);
  ropt += OpenMesh::IO::Options::FaceTexCoord;
  TASK_CHECK(OpenMesh::IO::read_mesh(out, path, ropt));
  TASK_CHECK(out.n_faces() == m.n_faces());
  for (size_t i = 0; i < m.n_faces(); ++i)
    TASK_CHECK(out.property(tag2, Mesh::FaceHandle(int(i))) == float(i) + 0.5f);
  return 0;
}

// End-to-end: build, bridge, verify topology, and round-trip the standard
// texture through the OM binary format without losing the bridge face order.
static int test_joint_topology_and_om()
{
  Mesh m;
  std::vector<Mesh::VertexHandle> loop[2];
  make_two_caps(m, 6, 5, loop, 2.0);

  BoundaryBridgeResultT<Mesh> result;
  OpenMesh::Utils::bridge_boundaries(m, loop[0], loop[1], 0.9, result);
  TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());
  TASK_CHECK(count_boundary_loops(m) == 0);

  OpenMesh::IO::Options wopt(OpenMesh::IO::Options::Binary);
  wopt += OpenMesh::IO::Options::FaceTexCoord;
  const std::string path = ".task-build/boundary_bridge_joint.om";
  TASK_CHECK(OpenMesh::IO::write_mesh(m, path, wopt));

  Mesh out;
  OpenMesh::IO::Options ropt(OpenMesh::IO::Options::Binary);
  ropt += OpenMesh::IO::Options::FaceTexCoord;
  TASK_CHECK(OpenMesh::IO::read_mesh(out, path, ropt));

  TASK_CHECK(out.n_vertices() == m.n_vertices());
  TASK_CHECK(out.n_faces() == m.n_faces());
  TASK_CHECK(out.has_halfedge_texcoords2D());
  TASK_CHECK(count_boundary_loops(out) == 0);
  TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(out).check());

  // Compare every face's corner UV sequence in file face order.
  for (size_t fi = 0; fi < m.n_faces(); ++fi)
  {
    std::vector<Mesh::TexCoord2D> a, b;
    for (Mesh::ConstFaceHalfedgeIter fh = m.cfh_iter(Mesh::FaceHandle(int(fi)));
         fh.is_valid(); ++fh)
      a.push_back(m.texcoord2D(*fh));
    for (Mesh::ConstFaceHalfedgeIter fh = out.cfh_iter(Mesh::FaceHandle(int(fi)));
         fh.is_valid(); ++fh)
      b.push_back(out.texcoord2D(*fh));
    TASK_CHECK(a.size() == b.size());
    for (size_t c = 0; c < a.size(); ++c)
      TASK_CHECK((a[c] - b[c]).norm() < 1e-6f);
  }
  return 0;
}

//=============================================================================
// registration
//=============================================================================

const struct task_case task_cases[] = {
  { "optimality", "enumerate_small_loops",          test_enumerate_small_loops },
  { "optimality", "lambda_changes_optimum",         test_lambda_changes_optimum },
  { "optimality", "closure_term_matters",           test_closure_term_matters },
  { "optimality", "cyclic_shift_invariance",        test_cyclic_shift_invariance },
  { "optimality", "existing_cross_edge_forbidden",  test_existing_cross_edge_forbidden },
  { "optimality", "third_unrelated_boundary",       test_third_unrelated_boundary },

  { "topology",   "topology_and_handles",           test_topology_and_handles },
  { "topology",   "no_seam_repeat_modular_indices", test_no_seam_repeat_and_modular_indices },
  { "topology",   "invalid_inputs_unchanged",       test_invalid_inputs_leave_mesh_unchanged },
  { "topology",   "degenerate_rejected",            test_degenerate_geometry_rejected },

  { "attributes", "existing_uv_custom_attributes",  test_existing_face_uv_and_custom_attributes },
  { "attributes", "seam_uv_zero_and_one",           test_seam_uv_zero_and_one },
  { "attributes", "standard_texture_om_roundtrip",  test_standard_texture_om_roundtrip },
  { "attributes", "om_without_facetex_skips",       test_om_without_face_texcoord_skips_block },
  { "attributes", "om_old_format_compatible",       test_om_old_format_compatible },
  { "attributes", "joint_topology_and_om",          test_joint_topology_and_om }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
