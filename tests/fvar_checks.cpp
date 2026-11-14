// Checks for the optional face-varying (per face corner) texture coordinate
// refinement of OpenMesh::Subdivider::Uniform::CatmullClarkT and LoopT.
//
// A halfedge texcoord denotes the texture coordinate of the halfedge's end
// vertex inside the face of that halfedge.  Two face corners of one
// geometric edge are continuous iff both endpoint coordinates agree exactly
// on both sides.  Corners around a geometric vertex that are connected by
// continuous edges form a texture face fan.  Every fan is subdivided as an
// independent control mesh.

#include "task_support/check.h"

#include <OpenMesh/Core/Mesh/PolyMesh_ArrayKernelT.hh>
#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Subdivider/Uniform/CatmullClarkT.hh>
#include <OpenMesh/Tools/Subdivider/Uniform/LoopT.hh>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace fvar_checks {

// ------------------------------------------------------------------- types

typedef OpenMesh::PolyMesh_ArrayKernelT<> PolyMesh;
typedef OpenMesh::TriMesh_ArrayKernelT<>  TriMesh;

typedef OpenMesh::Subdivider::Uniform::CatmullClarkT<PolyMesh> Catmull;
typedef OpenMesh::Subdivider::Uniform::LoopT<TriMesh>          Loop;

static const float kPi = 3.14159265358979323846f;

// ---------------------------------------------------------------- utilities

inline bool near(float _a, float _b)
{
  return std::fabs(_a - _b) <= 1e-4f;
}

template <class Mesh>
bool near(const typename Mesh::TexCoord2D& _a,
          const typename Mesh::TexCoord2D& _b)
{
  return near(_a[0], _b[0]) && near(_a[1], _b[1]);
}

template <class Mesh>
typename Mesh::TexCoord2D tc(float _u, float _v)
{
  return typename Mesh::TexCoord2D(_u, _v);
}

// All face-corner texcoords at a vertex (duplicates removed).
template <class Mesh>
std::vector<typename Mesh::TexCoord2D>
uvs_at_vertex(Mesh& _m, typename Mesh::VertexHandle _vh)
{
  typedef typename Mesh::TexCoord2D TexCoord;
  std::vector<TexCoord> result;

  for (typename Mesh::HalfedgeIter h = _m.halfedges_begin();
       h != _m.halfedges_end(); ++h)
  {
    if (!_m.face_handle(*h).is_valid())
      continue;
    if (_m.to_vertex_handle(*h) != _vh)
      continue;

    const TexCoord uv = _m.texcoord2D(*h);
    bool duplicate = false;
    for (size_t i = 0; i < result.size(); ++i)
      if (near<Mesh>(result[i], uv))
        duplicate = true;
    if (!duplicate)
      result.push_back(uv);
  }
  return result;
}

template <class Mesh>
bool has_uv_at_vertex(Mesh& _m, typename Mesh::VertexHandle _vh,
                      const typename Mesh::TexCoord2D& _uv)
{
  std::vector<typename Mesh::TexCoord2D> values = uvs_at_vertex(_m, _vh);
  for (size_t i = 0; i < values.size(); ++i)
    if (near<Mesh>(values[i], _uv))
      return true;
  return false;
}

// Is the geometric edge of _hh continuous in texture space?
template <class Mesh>
bool continuous(Mesh& _m, typename Mesh::HalfedgeHandle _hh)
{
  const typename Mesh::HalfedgeHandle h0 = _hh;
  const typename Mesh::HalfedgeHandle h1 = _m.opposite_halfedge_handle(h0);

  if (!_m.face_handle(h0).is_valid() || !_m.face_handle(h1).is_valid())
    return false;

  return _m.texcoord2D(h0) == _m.texcoord2D(_m.prev_halfedge_handle(h1)) &&
         _m.texcoord2D(_m.prev_halfedge_handle(h0)) == _m.texcoord2D(h1);
}

template <class Mesh>
size_t discontinuous_interior_edges(Mesh& _m)
{
  size_t n = 0;
  for (typename Mesh::EdgeIter e = _m.edges_begin(); e != _m.edges_end(); ++e)
  {
    const typename Mesh::HalfedgeHandle h0 = _m.halfedge_handle(*e, 0);
    const typename Mesh::HalfedgeHandle h1 = _m.opposite_halfedge_handle(h0);

    if (!_m.face_handle(h0).is_valid() || !_m.face_handle(h1).is_valid())
      continue;

    if (!continuous(_m, h0))
      ++n;
  }
  return n;
}

// Every face corner must carry a finite coordinate.
template <class Mesh>
bool all_corners_are_finite(Mesh& _m)
{
  for (typename Mesh::HalfedgeIter h = _m.halfedges_begin();
       h != _m.halfedges_end(); ++h)
  {
    if (!_m.face_handle(*h).is_valid())
      continue;

    const typename Mesh::TexCoord2D uv = _m.texcoord2D(*h);
    if (!(uv[0] == uv[0]) || !(uv[1] == uv[1]))
      return false;
  }
  return true;
}

// Exact topology and geometry comparison.
template <class MeshA, class MeshB>
bool same_mesh(MeshA& _a, MeshB& _b)
{
  if (_a.n_vertices() != _b.n_vertices() ||
      _a.n_faces()    != _b.n_faces()    ||
      _a.n_edges()    != _b.n_edges()    ||
      _a.n_halfedges()!= _b.n_halfedges())
    return false;

  for (size_t i = 0; i < _a.n_halfedges(); ++i)
  {
    typename MeshA::HalfedgeHandle ha(static_cast<int>(i));
    typename MeshB::HalfedgeHandle hb(static_cast<int>(i));

    if (_a.to_vertex_handle(ha).idx() != _b.to_vertex_handle(hb).idx()) return false;
    if (_a.from_vertex_handle(ha).idx() != _b.from_vertex_handle(hb).idx()) return false;
    if (_a.next_halfedge_handle(ha).idx() != _b.next_halfedge_handle(hb).idx()) return false;
    if (_a.face_handle(ha).idx() != _b.face_handle(hb).idx()) return false;
  }

  for (size_t i = 0; i < _a.n_vertices(); ++i)
  {
    typename MeshA::VertexHandle va(static_cast<int>(i));
    typename MeshB::VertexHandle vb(static_cast<int>(i));
    if ((_a.point(va) - _b.point(vb)).norm() > 1e-5f)
      return false;
  }
  return true;
}

// All face-corner coordinates, in halfedge order (for round comparisons).
template <class Mesh>
std::vector<typename Mesh::TexCoord2D> corner_texcoords(Mesh& _m)
{
  std::vector<typename Mesh::TexCoord2D> result;
  for (typename Mesh::HalfedgeIter h = _m.halfedges_begin();
       h != _m.halfedges_end(); ++h)
  {
    if (_m.face_handle(*h).is_valid())
      result.push_back(_m.texcoord2D(*h));
  }
  return result;
}

template <class Mesh>
bool same_corner_texcoords(Mesh& _a, Mesh& _b)
{
  if (_a.n_halfedges() != _b.n_halfedges())
    return false;

  std::vector<typename Mesh::TexCoord2D> ta = corner_texcoords(_a);
  std::vector<typename Mesh::TexCoord2D> tb = corner_texcoords(_b);
  if (ta.size() != tb.size())
    return false;

  for (size_t i = 0; i < ta.size(); ++i)
    if (ta[i] != tb[i])
      return false;
  return true;
}

// -------------------------------------------------------------- UV helpers

// Continuous chart: uv = affine(position).  Used to verify the smooth rules
// exactly (linearity preserved by Catmull-Clark and Loop vertex rules).
template <class Mesh>
void assign_affine_uv(Mesh& _m)
{
  const float a = 1.25f, b = -0.5f, c = 2.0f;
  const float d = 0.75f, e =  1.5f, f = -1.0f;

  for (typename Mesh::FaceIter fi = _m.faces_begin(); fi != _m.faces_end(); ++fi)
    for (typename Mesh::FaceHalfedgeIter h = _m.fh_begin(*fi);
         h != _m.fh_end(*fi); ++h)
    {
      const typename Mesh::Point p = _m.point(_m.to_vertex_handle(*h));
      _m.set_texcoord2D(*h, typename Mesh::TexCoord2D(
          a * p[0] + b * p[1] + c,
          d * p[0] + e * p[1] + f));
    }
}

template <class Mesh>
float max_affine_error(Mesh& _m)
{
  const float a = 1.25f, b = -0.5f, c = 2.0f;
  const float d = 0.75f, e =  1.5f, f = -1.0f;

  float err = 0.0f;
  for (typename Mesh::FaceIter fi = _m.faces_begin(); fi != _m.faces_end(); ++fi)
    for (typename Mesh::FaceHalfedgeIter h = _m.fh_begin(*fi);
         h != _m.fh_end(*fi); ++h)
    {
      const typename Mesh::Point p = _m.point(_m.to_vertex_handle(*h));
      const typename Mesh::TexCoord2D uv = _m.texcoord2D(*h);

      err = std::max(err, std::fabs(uv[0] - (a * p[0] + b * p[1] + c)));
      err = std::max(err, std::fabs(uv[1] - (d * p[0] + e * p[1] + f)));
    }
  return err;
}

// Read the unique coordinate at a vertex of a continuous chart.
template <class Mesh>
bool unique_uv_at_vertex(Mesh& _m, typename Mesh::VertexHandle _vh,
                         typename Mesh::TexCoord2D& _out)
{
  std::vector<typename Mesh::TexCoord2D> values = uvs_at_vertex(_m, _vh);
  if (values.size() != 1)
    return false;
  _out = values[0];
  return true;
}

// ------------------------------------------------------------- mesh makers

inline PolyMesh make_poly_grid(int _nx, int _ny, int _hole_x, int _hole_y)
{
  PolyMesh m;
  m.request_halfedge_texcoords2D();

  std::vector<PolyMesh::VertexHandle> vs;
  for (int y = 0; y <= _ny; ++y)
    for (int x = 0; x <= _nx; ++x)
      vs.push_back(m.add_vertex(PolyMesh::Point(float(x), float(y), 0.0f)));

  for (int y = 0; y < _ny; ++y)
    for (int x = 0; x < _nx; ++x)
    {
      if (x == _hole_x && y == _hole_y)
        continue;

      std::vector<PolyMesh::VertexHandle> f;
      f.push_back(vs[ y      * (_nx + 1) + x     ]);
      f.push_back(vs[ y      * (_nx + 1) + x + 1 ]);
      f.push_back(vs[(y + 1) * (_nx + 1) + x + 1 ]);
      f.push_back(vs[(y + 1) * (_nx + 1) + x     ]);
      m.add_face(f);
    }
  return m;
}

inline TriMesh make_tri_grid(int _n, bool _hole)
{
  TriMesh m;
  m.request_halfedge_texcoords2D();

  std::vector<TriMesh::VertexHandle> vs;
  for (int y = 0; y <= _n; ++y)
    for (int x = 0; x <= _n; ++x)
      vs.push_back(m.add_vertex(TriMesh::Point(float(x), float(y), 0.0f)));

  for (int y = 0; y < _n; ++y)
    for (int x = 0; x < _n; ++x)
    {
      if (_hole && x == _n / 2 && y == _n / 2)
        continue;

      TriMesh::VertexHandle p = vs[ y      * (_n + 1) + x     ];
      TriMesh::VertexHandle q = vs[ y      * (_n + 1) + x + 1 ];
      TriMesh::VertexHandle r = vs[(y + 1) * (_n + 1) + x     ];
      TriMesh::VertexHandle s = vs[(y + 1) * (_n + 1) + x + 1 ];

      std::vector<TriMesh::VertexHandle> f;
      f.push_back(p); f.push_back(q); f.push_back(s);
      m.add_face(f);
      f.clear();
      f.push_back(p); f.push_back(s); f.push_back(r);
      m.add_face(f);
    }
  return m;
}

// Single n-gon.  Catmull-Clark splits it into n quads; the chart stays
// affine, which checks the general face loop (non-quad valence handling).
template <class Mesh>
Mesh make_ngon(int _n)
{
  Mesh m;
  m.request_halfedge_texcoords2D();

  std::vector<typename Mesh::VertexHandle> vs;
  for (int i = 0; i < _n; ++i)
  {
    const float a = 2.0f * kPi * float(i) / float(_n);
    vs.push_back(m.add_vertex(typename Mesh::Point(
        std::cos(a), std::sin(a), 0.0f)));
  }
  m.add_face(vs);
  return m;
}

// Closed mesh with valence-3 vertices (tetrahedron).
inline PolyMesh make_poly_tetra()
{
  PolyMesh m;
  m.request_halfedge_texcoords2D();

  PolyMesh::VertexHandle v[4];
  v[0] = m.add_vertex(PolyMesh::Point(0, 0, 0));
  v[1] = m.add_vertex(PolyMesh::Point(1, 0, 0));
  v[2] = m.add_vertex(PolyMesh::Point(0, 1, 0));
  v[3] = m.add_vertex(PolyMesh::Point(0, 0, 1));

  const int F[4][3] = {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {0, 3, 2}};
  for (int i = 0; i < 4; ++i)
  {
    std::vector<PolyMesh::VertexHandle> f;
    for (int k = 0; k < 3; ++k)
      f.push_back(v[F[i][k]]);
    m.add_face(f);
  }
  return m;
}

// Closed triangle mesh with two valence-5 vertices (pentagonal bipyramid).
template <class Mesh>
Mesh make_bipyramid()
{
  Mesh m;
  m.request_halfedge_texcoords2D();

  std::vector<typename Mesh::VertexHandle> ring;
  for (int i = 0; i < 5; ++i)
  {
    const float a = 2.0f * kPi * float(i) / 5.0f;
    ring.push_back(m.add_vertex(typename Mesh::Point(
        std::cos(a), std::sin(a), 0.0f)));
  }
  typename Mesh::VertexHandle top = m.add_vertex(typename Mesh::Point(0, 0,  1));
  typename Mesh::VertexHandle bot = m.add_vertex(typename Mesh::Point(0, 0, -1));

  for (int i = 0; i < 5; ++i)
  {
    const int j = (i + 1) % 5;

    std::vector<typename Mesh::VertexHandle> f;
    f.push_back(ring[i]); f.push_back(ring[j]); f.push_back(top);
    m.add_face(f);
    f.clear();
    f.push_back(ring[j]); f.push_back(ring[i]); f.push_back(bot);
    m.add_face(f);
  }
  return m;
}

// Closed triangle mesh with valence-4 vertices (octahedron).
inline TriMesh make_tri_octa()
{
  TriMesh m;
  m.request_halfedge_texcoords2D();

  TriMesh::VertexHandle v[6];
  v[0] = m.add_vertex(TriMesh::Point( 1, 0, 0));
  v[1] = m.add_vertex(TriMesh::Point(-1, 0, 0));
  v[2] = m.add_vertex(TriMesh::Point(0,  1, 0));
  v[3] = m.add_vertex(TriMesh::Point(0, -1, 0));
  v[4] = m.add_vertex(TriMesh::Point(0, 0,  1));
  v[5] = m.add_vertex(TriMesh::Point(0, 0, -1));

  const int F[8][3] = {
    {0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
    {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}
  };
  for (int i = 0; i < 8; ++i)
  {
    std::vector<TriMesh::VertexHandle> f;
    for (int k = 0; k < 3; ++k)
      f.push_back(v[F[i][k]]);
    m.add_face(f);
  }
  return m;
}

// Two quads sharing one edge; the shared edge carries a seam.
//   left  chart: uv(position)
//   right chart: uv(position) + (10, 0)
struct TwoQuads
{
  PolyMesh mesh;
  PolyMesh::VertexHandle v[6];
};

inline TwoQuads make_two_quads_with_seam()
{
  TwoQuads q;
  PolyMesh& m = q.mesh;
  m.request_halfedge_texcoords2D();

  q.v[0] = m.add_vertex(PolyMesh::Point(0, 0, 0));
  q.v[1] = m.add_vertex(PolyMesh::Point(1, 0, 0)); // shared
  q.v[2] = m.add_vertex(PolyMesh::Point(1, 1, 0)); // shared
  q.v[3] = m.add_vertex(PolyMesh::Point(0, 1, 0));
  q.v[4] = m.add_vertex(PolyMesh::Point(2, 0, 0));
  q.v[5] = m.add_vertex(PolyMesh::Point(2, 1, 0));

  std::vector<PolyMesh::VertexHandle> f0;
  f0.push_back(q.v[0]); f0.push_back(q.v[1]);
  f0.push_back(q.v[2]); f0.push_back(q.v[3]);
  m.add_face(f0);

  std::vector<PolyMesh::VertexHandle> f1;
  f1.push_back(q.v[1]); f1.push_back(q.v[4]);
  f1.push_back(q.v[5]); f1.push_back(q.v[2]);
  m.add_face(f1);

  for (PolyMesh::FaceIter fi = m.faces_begin(); fi != m.faces_end(); ++fi)
  {
    bool shifted = false;
    for (PolyMesh::FaceHalfedgeIter h = m.fh_begin(*fi);
         h != m.fh_end(*fi); ++h)
      if (m.to_vertex_handle(*h) == q.v[4] ||
          m.to_vertex_handle(*h) == q.v[5])
        shifted = true;

    for (PolyMesh::FaceHalfedgeIter h = m.fh_begin(*fi);
         h != m.fh_end(*fi); ++h)
    {
      const PolyMesh::Point p = m.point(m.to_vertex_handle(*h));
      m.set_texcoord2D(*h, PolyMesh::TexCoord2D(
          p[0] + (shifted ? 10.0f : 0.0f), p[1]));
    }
  }
  return q;
}

// A 2x2 quad grid around a center vertex.  Two opposite interior edges are
// texture seams, splitting the four faces into two two-quad texture charts.
// Each chart gets its own affine coordinate system.
inline PolyMesh make_center_fans()
{
  PolyMesh m = make_poly_grid(2, 2, -1, -1);

  for (PolyMesh::FaceIter fi = m.faces_begin(); fi != m.faces_end(); ++fi)
  {
    PolyMesh::Point centroid(0, 0, 0);
    for (PolyMesh::FaceHalfedgeIter h = m.fh_begin(*fi);
         h != m.fh_end(*fi); ++h)
      centroid += m.point(m.to_vertex_handle(*h));
    centroid /= 4.0f;

    // chart A = left column {Q0, Q3}, chart B = right column {Q1, Q2};
    // the shared vertical line is the texture seam.
    const bool chart_b = centroid[0] > 1.0f;

    for (PolyMesh::FaceHalfedgeIter h = m.fh_begin(*fi);
         h != m.fh_end(*fi); ++h)
    {
      const PolyMesh::Point p = m.point(m.to_vertex_handle(*h));
      const float u = p[0] * 2.0f + (chart_b ? 50.0f : 1.0f);
      const float v = p[1] * 3.0f + (chart_b ? 60.0f : 2.0f);
      m.set_texcoord2D(*h, PolyMesh::TexCoord2D(u, v));
    }
  }
  return m;
}

// Two quads sharing an edge.  The seam edge is discontinuous only because of
// its second endpoint: the coordinate at the shared first vertex is equal on
// both sides, yet the fans must not be merged.
struct EqualVertexSeam
{
  PolyMesh mesh;
  PolyMesh::VertexHandle shared;
};

inline EqualVertexSeam make_equal_vertex_seam()
{
  EqualVertexSeam q;
  PolyMesh& m = q.mesh;
  m.request_halfedge_texcoords2D();

  PolyMesh::VertexHandle v1 = m.add_vertex(PolyMesh::Point(1, 0, 0)); // shared
  PolyMesh::VertexHandle v2 = m.add_vertex(PolyMesh::Point(1, 1, 0)); // shared
  PolyMesh::VertexHandle l0 = m.add_vertex(PolyMesh::Point(0, 0, 0));
  PolyMesh::VertexHandle l3 = m.add_vertex(PolyMesh::Point(0, 1, 0));
  PolyMesh::VertexHandle r4 = m.add_vertex(PolyMesh::Point(2, 0, 0));
  PolyMesh::VertexHandle r5 = m.add_vertex(PolyMesh::Point(2, 1, 0));
  q.shared = v1;

  std::vector<PolyMesh::VertexHandle> left;
  left.push_back(l0); left.push_back(v1); left.push_back(v2); left.push_back(l3);
  PolyMesh::FaceHandle lf = m.add_face(left);

  std::vector<PolyMesh::VertexHandle> right;
  right.push_back(v1); right.push_back(r4); right.push_back(r5); right.push_back(v2);
  PolyMesh::FaceHandle rf = m.add_face(right);

  const PolyMesh::TexCoord2D left_uv[4]  = {
    tc<PolyMesh>(0, 0), tc<PolyMesh>(5, 5), tc<PolyMesh>(9, 1), tc<PolyMesh>(0, 9) };
  const PolyMesh::VertexHandle left_v[4] = { l0, v1, v2, l3 };

  const PolyMesh::TexCoord2D right_uv[4]  = {
    tc<PolyMesh>(5, 5), tc<PolyMesh>(12, 2), tc<PolyMesh>(13, 3), tc<PolyMesh>(7, 7) };
  const PolyMesh::VertexHandle right_v[4] = { v1, r4, r5, v2 };

  for (int k = 0; k < 4; ++k)
    for (PolyMesh::FaceHalfedgeIter h = m.fh_begin(lf);
         h != m.fh_end(lf); ++h)
      if (m.to_vertex_handle(*h) == left_v[k])
        m.set_texcoord2D(*h, left_uv[k]);

  for (int k = 0; k < 4; ++k)
    for (PolyMesh::FaceHalfedgeIter h = m.fh_begin(rf);
         h != m.fh_end(rf); ++h)
      if (m.to_vertex_handle(*h) == right_v[k])
        m.set_texcoord2D(*h, right_uv[k]);

  return q;
}

// Two triangles sharing an edge, with a seam on the shared edge.
struct TwoTriangles
{
  TriMesh mesh;
  TriMesh::VertexHandle v[4];
};

inline TwoTriangles make_two_triangles_with_seam()
{
  TwoTriangles q;
  TriMesh& m = q.mesh;
  m.request_halfedge_texcoords2D();

  q.v[0] = m.add_vertex(TriMesh::Point(0, 0, 0));
  q.v[1] = m.add_vertex(TriMesh::Point(1, 0, 0));
  q.v[2] = m.add_vertex(TriMesh::Point(1, 1, 0));
  q.v[3] = m.add_vertex(TriMesh::Point(2, 0, 0));

  std::vector<TriMesh::VertexHandle> f0;
  f0.push_back(q.v[0]); f0.push_back(q.v[1]); f0.push_back(q.v[2]);
  m.add_face(f0);

  std::vector<TriMesh::VertexHandle> f1;
  f1.push_back(q.v[1]); f1.push_back(q.v[3]); f1.push_back(q.v[2]);
  m.add_face(f1);

  for (TriMesh::FaceIter fi = m.faces_begin(); fi != m.faces_end(); ++fi)
  {
    bool shifted = false;
    for (TriMesh::FaceHalfedgeIter h = m.fh_begin(*fi);
         h != m.fh_end(*fi); ++h)
      if (m.to_vertex_handle(*h) == q.v[3])
        shifted = true;

    for (TriMesh::FaceHalfedgeIter h = m.fh_begin(*fi);
         h != m.fh_end(*fi); ++h)
    {
      const TriMesh::Point p = m.point(m.to_vertex_handle(*h));
      m.set_texcoord2D(*h, TriMesh::TexCoord2D(
          p[0] + (shifted ? 10.0f : 0.0f), p[1] + (shifted ? 20.0f : 0.0f)));
    }
  }
  return q;
}

// ------------------------------------------------- boundary rule prediction

// For a continuous chart on a mesh with geometric boundary: the new texture
// coordinate of an old boundary vertex is 3/4 P + 1/8 (N1 + N2), where N1, N2
// are the chart coordinates of the two neighbours along the boundary.
template <class Mesh>
bool predict_boundary_values(
    Mesh& _m,
    std::vector<typename Mesh::TexCoord2D>& _expected,
    std::vector<char>& _has_expected)
{
  _expected.assign(_m.n_vertices(), typename Mesh::TexCoord2D());
  _has_expected.assign(_m.n_vertices(), 0);

  for (typename Mesh::VertexIter vi = _m.vertices_begin();
       vi != _m.vertices_end(); ++vi)
  {
    const typename Mesh::VertexHandle vh = *vi;
    if (!_m.is_boundary(vh))
      continue;

    typename Mesh::TexCoord2D P, N[2];
    if (!unique_uv_at_vertex(_m, vh, P))
      return false;

    int count = 0;
    for (typename Mesh::VertexEdgeIter ve = _m.ve_iter(vh);
         ve != _m.ve_end(vh); ++ve)
    {
      if (!_m.is_boundary(*ve))
        continue;
      if (count >= 2)
        return false;

      const typename Mesh::HalfedgeHandle h0 = _m.halfedge_handle(*ve, 0);
      const typename Mesh::VertexHandle other =
          (_m.to_vertex_handle(h0) == vh)
              ? _m.from_vertex_handle(h0)
              : _m.to_vertex_handle(h0);

      if (!unique_uv_at_vertex(_m, other, N[count]))
        return false;
      ++count;
    }

    if (count != 2)
      continue; // other configurations keep the old value

    _expected[vh.idx()] = P * 0.75f + (N[0] + N[1]) * 0.125f;
    _has_expected[vh.idx()] = 1;
  }
  return true;
}

template <class Mesh>
bool check_predicted_boundaries(
    Mesh& _m,
    const std::vector<typename Mesh::TexCoord2D>& _expected,
    const std::vector<char>& _has_expected)
{
  for (size_t i = 0; i < _expected.size(); ++i)
  {
    if (!_has_expected[i])
      continue;
    typename Mesh::VertexHandle vh(static_cast<int>(i));
    if (!has_uv_at_vertex(_m, vh, _expected[i]))
      return false;
  }
  return true;
}

// =========================================================== CATMULL GROUP

// One quad, explicit per-corner coordinates: Catmull-Clark face, edge and
// boundary vertex masks must produce the expected chart values.
int catmull_single_quad_explicit()
{
  PolyMesh m;
  m.request_halfedge_texcoords2D();

  PolyMesh::VertexHandle v[4];
  v[0] = m.add_vertex(PolyMesh::Point(0, 0, 0));
  v[1] = m.add_vertex(PolyMesh::Point(1, 0, 0));
  v[2] = m.add_vertex(PolyMesh::Point(1, 1, 0));
  v[3] = m.add_vertex(PolyMesh::Point(0, 1, 0));

  std::vector<PolyMesh::VertexHandle> f;
  f.push_back(v[0]); f.push_back(v[1]); f.push_back(v[2]); f.push_back(v[3]);
  PolyMesh::FaceHandle fh = m.add_face(f);
  TASK_CHECK(fh.is_valid());

  // Coordinates distinct in both components so mixes are identifiable.
  const PolyMesh::TexCoord2D uv[4] = {
    tc<PolyMesh>(0, 100), tc<PolyMesh>(1, 101),
    tc<PolyMesh>(2, 102), tc<PolyMesh>(3, 103)
  };
  for (int k = 0; k < 4; ++k)
    for (PolyMesh::FaceHalfedgeIter h = m.fh_begin(fh); h != m.fh_end(fh); ++h)
      if (m.to_vertex_handle(*h) == v[k])
        m.set_texcoord2D(*h, uv[k]);

  // Reference geometry without texcoord refinement.  Same vertex order as
  // above so that topology and positions can be compared index by index.
  PolyMesh ref;
  {
    PolyMesh::VertexHandle r[4];
    r[0] = ref.add_vertex(PolyMesh::Point(0, 0, 0));
    r[1] = ref.add_vertex(PolyMesh::Point(1, 0, 0));
    r[2] = ref.add_vertex(PolyMesh::Point(1, 1, 0));
    r[3] = ref.add_vertex(PolyMesh::Point(0, 1, 0));
    std::vector<PolyMesh::VertexHandle> rf;
    rf.push_back(r[0]); rf.push_back(r[1]);
    rf.push_back(r[2]); rf.push_back(r[3]);
    ref.add_face(rf);
  }
  {
    Catmull s; // refine disabled
    TASK_CHECK(s.attach(ref) && s(1)); s.detach();
  }

  Catmull s;
  s.set_refine_texcoords(true);
  TASK_CHECK(s.refine_texcoords());
  TASK_CHECK(s.attach(m) && s(1));
  s.detach();

  TASK_CHECK(same_mesh(m, ref)); // topology/geometry unchanged
  TASK_CHECK(m.n_faces() == 4 && m.n_vertices() == 9);
  TASK_CHECK(all_corners_are_finite(m));

  const PolyMesh::TexCoord2D face_point =
      (uv[0] + uv[1] + uv[2] + uv[3]) * 0.25f;

  // New boundary vertex value: 3/4 P + 1/8 (two neighbours).
  const PolyMesh::TexCoord2D corner0 = uv[0] * 0.75f + (uv[1] + uv[3]) * 0.125f;
  const PolyMesh::TexCoord2D corner1 = uv[1] * 0.75f + (uv[0] + uv[2]) * 0.125f;
  const PolyMesh::TexCoord2D corner2 = uv[2] * 0.75f + (uv[1] + uv[3]) * 0.125f;
  const PolyMesh::TexCoord2D corner3 = uv[3] * 0.75f + (uv[0] + uv[2]) * 0.125f;

  TASK_CHECK(has_uv_at_vertex(m, PolyMesh::VertexHandle(0), corner0));
  TASK_CHECK(has_uv_at_vertex(m, PolyMesh::VertexHandle(1), corner1));
  TASK_CHECK(has_uv_at_vertex(m, PolyMesh::VertexHandle(2), corner2));
  TASK_CHECK(has_uv_at_vertex(m, PolyMesh::VertexHandle(3), corner3));

  // Edge midpoints: averages of the two endpoint coordinates.
  const PolyMesh::TexCoord2D mid01 = (uv[0] + uv[1]) * 0.5f;
  const PolyMesh::TexCoord2D mid12 = (uv[1] + uv[2]) * 0.5f;
  const PolyMesh::TexCoord2D mid23 = (uv[2] + uv[3]) * 0.5f;
  const PolyMesh::TexCoord2D mid30 = (uv[3] + uv[0]) * 0.5f;

  // Locate the midpoint vertices by their geometric position.
  bool found01 = false, found12 = false, found23 = false, found30 = false;
  bool found_fp = false;

  for (PolyMesh::VertexIter vi = m.vertices_begin(); vi != m.vertices_end(); ++vi)
  {
    const PolyMesh::Point p = m.point(*vi);
    if (near(p[0], 0.5f) && near(p[1], 0.0f))
    { found01 = has_uv_at_vertex(m, *vi, mid01); }
    else if (near(p[0], 1.0f) && near(p[1], 0.5f))
    { found12 = has_uv_at_vertex(m, *vi, mid12); }
    else if (near(p[0], 0.5f) && near(p[1], 1.0f))
    { found23 = has_uv_at_vertex(m, *vi, mid23); }
    else if (near(p[0], 0.0f) && near(p[1], 0.5f))
    { found30 = has_uv_at_vertex(m, *vi, mid30); }
    else if (near(p[0], 0.5f) && near(p[1], 0.5f))
    { found_fp = has_uv_at_vertex(m, *vi, face_point); }
  }

  TASK_CHECK(found01 && found12 && found23 && found30);
  TASK_CHECK(found_fp);

  // Single chart: the four corners of every inner quad repeat their
  // neighbours' chart values -> no texture seam at all.
  TASK_CHECK(discontinuous_interior_edges(m) == 0);
  return 0;
}

// Non-regular valence: the smooth Catmull-Clark rules are checked with
// explicit values on a closed triangle mesh (valence 3) and with the
// closed-form vertex/edge mask on a valence-5 vertex.
int catmull_irregular_valence_affine()
{
  // ---- explicit regular-valence vertex on a closed 3x3 triangle-free mesh:
  //      the 2x2 closed quad torus is overkill; use the tetrahedron for the
  //      edge rule and a valence-5 bipyramid for the vertex rule.
  // (Kept simple: exact formulas are evaluated in the checks below.)

  // Tetrahedron: every vertex has valence 3 and every face has 3 corners.
  PolyMesh tetra = make_poly_tetra();
  assign_affine_uv(tetra);

  // Old chart values at each vertex (unique because the chart is continuous).
  PolyMesh::TexCoord2D p_old[4];
  PolyMesh::TexCoord2D fp[4];
  {
    for (PolyMesh::VertexIter vi = tetra.vertices_begin();
         vi != tetra.vertices_end(); ++vi)
    {
      TASK_CHECK(unique_uv_at_vertex(tetra, *vi, p_old[vi->idx()]));
    }

    for (PolyMesh::FaceIter fi = tetra.faces_begin();
         fi != tetra.faces_end(); ++fi)
    {
      PolyMesh::TexCoord2D s(0, 0);
      size_t n = 0;
      for (PolyMesh::FaceHalfedgeIter h = tetra.fh_begin(*fi);
           h != tetra.fh_end(*fi); ++h)
      { s += tetra.texcoord2D(*h); ++n; }
      fp[fi->idx()] = s / float(n);
    }
  }

  // Smooth Catmull-Clark vertex rule for valence n:
  //    p' = sum(neighbours)/n^2 + (n-2)/n * P + sum(face points)/n^2.
  // For n = 3 this is  sum(neighbours)/9 + P/3 + sum(face points)/9.
  std::vector<PolyMesh::TexCoord2D> expected(tetra.n_vertices());
  std::vector<char> has_expected(tetra.n_vertices(), 0);

  for (PolyMesh::VertexIter vi = tetra.vertices_begin();
       vi != tetra.vertices_end(); ++vi)
  {
    const PolyMesh::VertexHandle vh = *vi;
    PolyMesh::TexCoord2D neighbour_sum(0, 0);
    size_t n = 0;
    PolyMesh::TexCoord2D face_sum(0, 0);

    for (PolyMesh::VertexVertexIter vv = tetra.vv_iter(vh);
         vv != tetra.vv_end(vh); ++vv)
    {
      neighbour_sum += p_old[vv->idx()];
      ++n;
    }

    for (PolyMesh::VertexFaceIter vf = tetra.vf_iter(vh);
         vf != tetra.vf_end(vh); ++vf)
      face_sum += fp[vf->idx()];

    TASK_CHECK(n == 3);

    const float inv = 1.0f / float(n);
    expected[vh.idx()] =
        neighbour_sum * (inv * inv) +
        p_old[vh.idx()] * (float(n - 2) * inv) +
        face_sum * (inv * inv);
    has_expected[vh.idx()] = 1;
  }

  Catmull s;
  s.set_refine_texcoords(true);
  TASK_CHECK(s.attach(tetra) && s(1));
  s.detach();

  TASK_CHECK(check_predicted_boundaries(tetra, expected, has_expected));
  TASK_CHECK(all_corners_are_finite(tetra));

  // Reference geometry without refinement must be identical.
  PolyMesh ref = make_poly_tetra();
  {
    Catmull r;
    TASK_CHECK(r.attach(ref) && r(1)); r.detach();
  }
  TASK_CHECK(same_mesh(tetra, ref));

  // Affine invariance over a second round and on a valence-5 mesh.
  PolyMesh tetra2 = make_poly_tetra();
  assign_affine_uv(tetra2);
  Catmull s2;
  s2.set_refine_texcoords(true);
  TASK_CHECK(s2.attach(tetra2) && s2(2));
  s2.detach();
  TASK_CHECK(max_affine_error(tetra2) <= 1e-4f);
  TASK_CHECK(tetra2.n_vertices() == 50); // 14 after one round, 50 after two

  PolyMesh penta = make_bipyramid<PolyMesh>();
  assign_affine_uv(penta);

  Catmull sp;
  sp.set_refine_texcoords(true);
  TASK_CHECK(sp.attach(penta) && sp(1));
  sp.detach();
  TASK_CHECK(max_affine_error(penta) <= 1e-4f);
  return 0;
}

// A pentagon (non-quad face) with explicit corner coordinates: the face loop
// must split it into five quads and produce the chart face point, edge
// averages and the 3/4-1/8-1/8 boundary values.
int catmull_ngon_explicit()
{
  PolyMesh m = make_ngon<PolyMesh>(5);

  // Corner coordinate k, injected at the corner with that vertex index.
  const PolyMesh::TexCoord2D uv[5] = {
    tc<PolyMesh>(1, 100), tc<PolyMesh>(3, 101), tc<PolyMesh>(5, 102),
    tc<PolyMesh>(7, 103), tc<PolyMesh>(9, 104)
  };
  PolyMesh::FaceHandle fh = *m.faces_begin();
  for (PolyMesh::FaceHalfedgeIter h = m.fh_begin(fh); h != m.fh_end(fh); ++h)
    m.set_texcoord2D(*h, uv[m.to_vertex_handle(*h).idx()]);

  // Remember the corner positions before the topology is refined.
  PolyMesh::Point corners[5];
  for (int k = 0; k < 5; ++k)
    corners[k] = m.point(PolyMesh::VertexHandle(k));

  Catmull s;
  s.set_refine_texcoords(true);
  TASK_CHECK(s.attach(m) && s(1));
  s.detach();

  TASK_CHECK(m.n_faces() == 5);
  TASK_CHECK(all_corners_are_finite(m));

  // Face point: centroid of the five corners.
  PolyMesh::TexCoord2D face_point(0, 0);
  for (int k = 0; k < 5; ++k)
    face_point += uv[k];
  face_point *= 0.2f;

  // Boundary midpoints: averages of the two endpoint coordinates.
  bool face_point_found = false;
  for (PolyMesh::VertexIter vi = m.vertices_begin(); vi != m.vertices_end(); ++vi)
  {
    // The face point is the only new vertex strictly inside the pentagon,
    // recognizable by its geometric position being the centroid.
    PolyMesh::Point c(0, 0, 0);
    for (int k = 0; k < 5; ++k)
      c += corners[k];
    c *= 0.2f;
    if ((m.point(*vi) - c).norm() < 1e-5f)
      face_point_found = has_uv_at_vertex(m, *vi, face_point);
  }
  TASK_CHECK(face_point_found);

  for (int k = 0; k < 5; ++k)
  {
    const int j = (k + 1) % 5;
    const PolyMesh::TexCoord2D mid = (uv[k] + uv[j]) * 0.5f;
    bool found = false;
    for (PolyMesh::VertexIter vi = m.vertices_begin();
         vi != m.vertices_end(); ++vi)
    {
      const PolyMesh::Point p = m.point(*vi);
      const PolyMesh::Point a = corners[k];
      const PolyMesh::Point b = corners[j];
      if ((p - (a + b) * 0.5f).norm() < 1e-5f)
        found = has_uv_at_vertex(m, *vi, mid);
    }
    TASK_CHECK(found);
  }

  // Boundary vertex value: 3/4 P + 1/8 (neighbours along the boundary).
  for (int k = 0; k < 5; ++k)
  {
    const int prev = (k + 4) % 5;
    const int next = (k + 1) % 5;
    const PolyMesh::TexCoord2D expected =
        uv[k] * 0.75f + (uv[prev] + uv[next]) * 0.125f;
    TASK_CHECK(has_uv_at_vertex(m, PolyMesh::VertexHandle(k), expected));
  }

  TASK_CHECK(discontinuous_interior_edges(m) == 0);
  return 0;
}

// Hole boundary: old boundary chart vertices use 3/4, 1/8, 1/8.
int catmull_hole_boundary()
{
  PolyMesh m = make_poly_grid(4, 3, 1, 1); // one quad removed
  assign_affine_uv(m);

  TASK_CHECK(m.n_faces() == 11);

  std::vector<PolyMesh::TexCoord2D> expected;
  std::vector<char> has_expected;
  TASK_CHECK(predict_boundary_values(m, expected, has_expected));

  size_t count = 0;
  for (size_t i = 0; i < has_expected.size(); ++i)
    if (has_expected[i])
      ++count;
  TASK_CHECK(count > 0);

  Catmull s;
  s.set_refine_texcoords(true);
  TASK_CHECK(s.attach(m) && s(1));
  s.detach();

  TASK_CHECK(check_predicted_boundaries(m, expected, has_expected));
  TASK_CHECK(all_corners_are_finite(m));

  // No thresholds or holes were invented: the hole stays open (its boundary
  // is still a boundary in the subdivided mesh).
  size_t boundary_loops = 0;
  for (PolyMesh::VertexIter vi = m.vertices_begin(); vi != m.vertices_end(); ++vi)
    if (m.is_boundary(*vi))
      ++boundary_loops;
  TASK_CHECK(boundary_loops > 0);
  return 0;
}

// Two texture charts sharing an interior edge: both charts stay independent.
int catmull_two_chart_seam()
{
  TwoQuads q = make_two_quads_with_seam();
  PolyMesh& m = q.mesh;

  Catmull s;
  s.set_refine_texcoords(true);
  TASK_CHECK(s.attach(m) && s(1));
  s.detach();

  TASK_CHECK(m.n_faces() == 8);
  TASK_CHECK(all_corners_are_finite(m));

  // The original shared edge is split in two; nothing else is a texture seam.
  TASK_CHECK(discontinuous_interior_edges(m) == 2);

  // Left chart corner values (only one face per chart).
  TASK_CHECK(has_uv_at_vertex(m, q.v[0], tc<PolyMesh>(0.125f, 0.125f)));
  TASK_CHECK(has_uv_at_vertex(m, q.v[1], tc<PolyMesh>(0.875f, 0.125f)));
  TASK_CHECK(has_uv_at_vertex(m, q.v[2], tc<PolyMesh>(0.875f, 0.875f)));
  TASK_CHECK(has_uv_at_vertex(m, q.v[3], tc<PolyMesh>(0.125f, 0.875f)));

  // Right chart corner values.
  TASK_CHECK(has_uv_at_vertex(m, q.v[1], tc<PolyMesh>(11.125f, 0.125f)));
  TASK_CHECK(has_uv_at_vertex(m, q.v[2], tc<PolyMesh>(11.125f, 0.875f)));
  TASK_CHECK(has_uv_at_vertex(m, q.v[4], tc<PolyMesh>(11.875f, 0.125f)));
  TASK_CHECK(has_uv_at_vertex(m, q.v[5], tc<PolyMesh>(11.875f, 0.875f)));

  // Face points: the two charts get their own centroid coordinates.
  bool left_face_point = false, right_face_point = false;
  for (PolyMesh::VertexIter vi = m.vertices_begin(); vi != m.vertices_end(); ++vi)
  {
    const PolyMesh::Point p = m.point(*vi);
    if (near(p[0], 0.5f) && near(p[1], 0.5f))
      left_face_point = has_uv_at_vertex(m, *vi, tc<PolyMesh>(0.5f, 0.5f));
    if (near(p[0], 1.5f) && near(p[1], 0.5f))
      right_face_point = has_uv_at_vertex(m, *vi, tc<PolyMesh>(11.5f, 0.5f));
  }
  TASK_CHECK(left_face_point);
  TASK_CHECK(right_face_point);
  return 0;
}

// ============================================================== LOOP GROUP

// One triangle, explicit coordinates: 3/4-1/8-1/8 boundary rule, 3/8-1/8
// edge rule and exact chart propagation.
int loop_single_triangle_explicit()
{
  TriMesh m;
  m.request_halfedge_texcoords2D();

  TriMesh::VertexHandle v[3];
  v[0] = m.add_vertex(TriMesh::Point(0, 0, 0));
  v[1] = m.add_vertex(TriMesh::Point(1, 0, 0));
  v[2] = m.add_vertex(TriMesh::Point(0, 1, 0));

  std::vector<TriMesh::VertexHandle> f;
  f.push_back(v[0]); f.push_back(v[1]); f.push_back(v[2]);
  TriMesh::FaceHandle fh = m.add_face(f);
  TASK_CHECK(fh.is_valid());

  const TriMesh::TexCoord2D uv[3] = {
    tc<TriMesh>(0, 100), tc<TriMesh>(1, 101), tc<TriMesh>(2, 102)
  };
  for (int k = 0; k < 3; ++k)
    for (TriMesh::FaceHalfedgeIter h = m.fh_begin(fh); h != m.fh_end(fh); ++h)
      if (m.to_vertex_handle(*h) == v[k])
        m.set_texcoord2D(*h, uv[k]);

  // Reference geometry without refinement.
  TriMesh ref = make_tri_grid(1, false);
  // make_tri_grid(1,false) has two triangles; build an equivalent single
  // triangle reference instead.
  ref.clear();
  ref.request_halfedge_texcoords2D();
  {
    TriMesh::VertexHandle rv[3];
    rv[0] = ref.add_vertex(TriMesh::Point(0, 0, 0));
    rv[1] = ref.add_vertex(TriMesh::Point(1, 0, 0));
    rv[2] = ref.add_vertex(TriMesh::Point(0, 1, 0));
    std::vector<TriMesh::VertexHandle> rf;
    rf.push_back(rv[0]); rf.push_back(rv[1]); rf.push_back(rv[2]);
    ref.add_face(rf);
  }
  {
    Loop s;
    TASK_CHECK(s.attach(ref) && s(1)); s.detach();
  }

  Loop s;
  s.set_refine_texcoords(true);
  TASK_CHECK(s.attach(m) && s(1));
  s.detach();

  TASK_CHECK(same_mesh(m, ref));
  TASK_CHECK(m.n_faces() == 4 && m.n_vertices() == 6);
  TASK_CHECK(all_corners_are_finite(m));

  // Boundary corner values.
  TASK_CHECK(has_uv_at_vertex(m, v[0], uv[0] * 0.75f + (uv[1] + uv[2]) * 0.125f));
  TASK_CHECK(has_uv_at_vertex(m, v[1], uv[1] * 0.75f + (uv[0] + uv[2]) * 0.125f));
  TASK_CHECK(has_uv_at_vertex(m, v[2], uv[2] * 0.75f + (uv[0] + uv[1]) * 0.125f));

  // Edge midpoints (geometric positions identify them).
  TASK_CHECK(has_uv_at_vertex(m, TriMesh::VertexHandle(3),
                              (uv[0] + uv[1]) * 0.5f)); // (0.5, 0)
  TASK_CHECK(has_uv_at_vertex(m, TriMesh::VertexHandle(4),
                              (uv[1] + uv[2]) * 0.5f)); // (0.5, 0.5)
  TASK_CHECK(has_uv_at_vertex(m, TriMesh::VertexHandle(5),
                              (uv[0] + uv[2]) * 0.5f)); // (0, 0.5)

  TASK_CHECK(discontinuous_interior_edges(m) == 0);
  return 0;
}

// Interior Loop rules on explicit numbers, using the octahedron (valence 4,
// closed):
//   interior edge point = 3/8 (P0 + P1) + 1/8 (P2 + P3) where P2, P3 are
//   the opposite tetrahedron vertices of the two incident triangles;
//   interior vertex     = (1 - alpha(n)) P + alpha(n)/n * sum(neighbours),
//   alpha(n) = (40 - (3 + 2 cos(2 pi / n))^2) / 64.
int loop_irregular_valence_affine()
{
  TriMesh octa = make_tri_octa();
  assign_affine_uv(octa);

  // Old chart values and geometric edge endpoints before refinement.
  std::vector<TriMesh::TexCoord2D> p_old(octa.n_vertices());
  for (TriMesh::VertexIter vi = octa.vertices_begin(); vi != octa.vertices_end(); ++vi)
    TASK_CHECK(unique_uv_at_vertex(octa, *vi, p_old[vi->idx()]));

  struct EdgeInfo
  {
    TriMesh::VertexHandle a, b, c0, c1;
    TriMesh::TexCoord2D expected;
  };
  std::vector<EdgeInfo> edges;

  for (TriMesh::EdgeIter e = octa.edges_begin(); e != octa.edges_end(); ++e)
  {
    const TriMesh::HalfedgeHandle h0 = octa.halfedge_handle(*e, 0);
    const TriMesh::HalfedgeHandle h1 = octa.halfedge_handle(*e, 1);
    TASK_CHECK(octa.face_handle(h0).is_valid() && octa.face_handle(h1).is_valid());

    EdgeInfo info;
    info.a = octa.from_vertex_handle(h0);
    info.b = octa.to_vertex_handle(h0);
    info.c0 = octa.to_vertex_handle(octa.next_halfedge_handle(h0));
    info.c1 = octa.to_vertex_handle(octa.next_halfedge_handle(h1));

    info.expected =
        (p_old[info.a.idx()] + p_old[info.b.idx()]) * 0.375f +
        (p_old[info.c0.idx()] + p_old[info.c1.idx()]) * 0.125f;
    edges.push_back(info);
  }

  // Interior vertex rule for valence 4.
  const float n = 4.0f;
  const float alpha =
      (40.0f - std::pow(3.0f + 2.0f * std::cos(2.0f * kPi / n), 2.0f)) / 64.0f;

  std::vector<TriMesh::TexCoord2D> v_expected(octa.n_vertices());
  for (TriMesh::VertexIter vi = octa.vertices_begin(); vi != octa.vertices_end(); ++vi)
  {
    TriMesh::TexCoord2D sum(0, 0);
    for (TriMesh::VertexVertexIter vv = octa.vv_iter(*vi);
         vv != octa.vv_end(*vi); ++vv)
      sum += p_old[vv->idx()];

    v_expected[vi->idx()] =
        p_old[vi->idx()] * (1.0f - alpha) + sum * (alpha / n);
  }

  TriMesh ref = make_tri_octa();
  {
    Loop r;
    TASK_CHECK(r.attach(ref) && r(1)); r.detach();
  }

  Loop s;
  s.set_refine_texcoords(true);
  TASK_CHECK(s.attach(octa) && s(1));
  s.detach();

  TASK_CHECK(same_mesh(octa, ref));
  TASK_CHECK(all_corners_are_finite(octa));

  // Old vertices keep their indices through the split.
  size_t vertex_checked = 0;
  for (TriMesh::VertexIter vi = octa.vertices_begin(); vi != octa.vertices_end(); ++vi)
  {
    if (vi->idx() >= static_cast<int>(p_old.size()))
      continue;
    TASK_CHECK(has_uv_at_vertex(octa, *vi, v_expected[vi->idx()]));
    ++vertex_checked;
  }
  TASK_CHECK(vertex_checked == p_old.size());

  // Edge points: match each expected value against the coordinates present in
  // the refined mesh.
  size_t edge_checked = 0;
  for (size_t k = 0; k < edges.size(); ++k)
  {
    bool found = false;
    for (TriMesh::VertexIter vi = octa.vertices_begin();
         vi != octa.vertices_end(); ++vi)
      if (has_uv_at_vertex(octa, *vi, edges[k].expected))
        found = true;
    TASK_CHECK(found);
    ++edge_checked;
  }
  TASK_CHECK(edge_checked == edges.size());

  // Valence-5 vertices, affine invariance over two rounds.
  TriMesh bipyramid = make_bipyramid<TriMesh>();
  assign_affine_uv(bipyramid);

  Loop sb;
  sb.set_refine_texcoords(true);
  TASK_CHECK(sb.attach(bipyramid) && sb(2));
  sb.detach();
  TASK_CHECK(max_affine_error(bipyramid) <= 1e-4f);
  TASK_CHECK(all_corners_are_finite(bipyramid));
  return 0;
}

// Hole boundary on a triangle mesh.
int loop_hole_boundary()
{
  TriMesh m = make_tri_grid(3, true);
  assign_affine_uv(m);

  std::vector<TriMesh::TexCoord2D> expected;
  std::vector<char> has_expected;
  TASK_CHECK(predict_boundary_values(m, expected, has_expected));

  size_t count = 0;
  for (size_t i = 0; i < has_expected.size(); ++i)
    if (has_expected[i])
      ++count;
  TASK_CHECK(count > 0);

  Loop s;
  s.set_refine_texcoords(true);
  TASK_CHECK(s.attach(m) && s(1));
  s.detach();

  TASK_CHECK(check_predicted_boundaries(m, expected, has_expected));
  TASK_CHECK(all_corners_are_finite(m));
  TASK_CHECK(discontinuous_interior_edges(m) == 0); // no seam introduced
  return 0;
}

// ============================================================= SEAMS GROUP

// Four quads around one vertex, separated into two charts: each chart is a
// separate control mesh; values do not merge across the seam.
int seams_center_fans_catmull()
{
  PolyMesh m = make_center_fans();

  TASK_CHECK(discontinuous_interior_edges(m) == 2);

  Catmull s;
  s.set_refine_texcoords(true);
  TASK_CHECK(s.attach(m) && s(1));
  s.detach();

  TASK_CHECK(all_corners_are_finite(m));

  // Each of the two seams splits into two discontinuous edges.
  TASK_CHECK(discontinuous_interior_edges(m) == 4);

  // Center vertex (index 4 in the 3x3 grid).
  const PolyMesh::VertexHandle c(4);

  // Chart A: P=(3,5), boundary neighbours (3,2) and (3,8).
  const PolyMesh::TexCoord2D center_a =
      tc<PolyMesh>(3, 5) * 0.75f +
      (tc<PolyMesh>(3, 2) + tc<PolyMesh>(3, 8)) * 0.125f;

  // Chart B: P=(52,63), boundary neighbours (52,60) and (52,66).
  const PolyMesh::TexCoord2D center_b =
      tc<PolyMesh>(52, 63) * 0.75f +
      (tc<PolyMesh>(52, 60) + tc<PolyMesh>(52, 66)) * 0.125f;

  TASK_CHECK(has_uv_at_vertex(m, c, center_a));
  TASK_CHECK(has_uv_at_vertex(m, c, center_b));
  TASK_CHECK(!near<PolyMesh>(center_a, center_b));

  // Coordinates stay within their own chart, i.e. the two fans are not
  // glued through the seam.
  for (PolyMesh::FaceIter fi = m.faces_begin(); fi != m.faces_end(); ++fi)
  {
    PolyMesh::Point centroid(0, 0, 0);
    for (PolyMesh::FaceHalfedgeIter h = m.fh_begin(*fi);
         h != m.fh_end(*fi); ++h)
      centroid += m.point(m.to_vertex_handle(*h));
    centroid /= 4.0f;

    const bool chart_b = centroid[0] > 1.0f;

    for (PolyMesh::FaceHalfedgeIter h = m.fh_begin(*fi);
         h != m.fh_end(*fi); ++h)
    {
      const PolyMesh::TexCoord2D uv = m.texcoord2D(*h);
      if (chart_b)
        TASK_CHECK(uv[0] > 40.0f);
      else
        TASK_CHECK(uv[0] < 20.0f);
    }
  }
  return 0;
}

// Same fan separation for Loop on a triangle mesh.
int seams_center_fans_loop()
{
  TriMesh m = make_tri_grid(2, false);

  // Craft a seam on two opposite edges around the center vertex.  The center
  // of a 2x2 triangle grid has index 4 (3x3 vertices).
  // Simpler and more explicit: build the same two-chart configuration with
  // four triangles around the center.
  m.clear();
  m.request_halfedge_texcoords2D();

  std::vector<TriMesh::VertexHandle> vs;
  for (int y = 0; y <= 2; ++y)
    for (int x = 0; x <= 2; ++x)
      vs.push_back(m.add_vertex(TriMesh::Point(float(x), float(y), 0.0f)));

  // Faces: Q0 lower-left, Q1 lower-right, Q2 upper-right, Q3 upper-left,
  // each split into two triangles.
  const int quads[4][4] = {
    {0, 1, 4, 3}, {1, 2, 5, 4}, {4, 5, 8, 7}, {3, 4, 7, 6}
  };
  for (int i = 0; i < 4; ++i)
  {
    TriMesh::VertexHandle a = vs[quads[i][0]];
    TriMesh::VertexHandle b = vs[quads[i][1]];
    TriMesh::VertexHandle c = vs[quads[i][2]];
    TriMesh::VertexHandle d = vs[quads[i][3]];

    std::vector<TriMesh::VertexHandle> f;
    f.push_back(a); f.push_back(b); f.push_back(c); m.add_face(f);
    f.clear();
    f.push_back(a); f.push_back(c); f.push_back(d); m.add_face(f);
  }

  // Charts: A = {Q0,Q3}, B = {Q1,Q2}, as in the Catmull-Clark case.
  for (TriMesh::FaceIter fi = m.faces_begin(); fi != m.faces_end(); ++fi)
  {
    TriMesh::Point centroid(0, 0, 0);
    size_t n = 0;
    for (TriMesh::FaceHalfedgeIter h = m.fh_begin(*fi);
         h != m.fh_end(*fi); ++h)
    { centroid += m.point(m.to_vertex_handle(*h)); ++n; }
    centroid /= float(n);

    // Same two-chart split as the Catmull-Clark case: the left column of
    // quads forms chart A, the right column chart B.
    const bool chart_b = centroid[0] > 1.0f;

    for (TriMesh::FaceHalfedgeIter h = m.fh_begin(*fi);
         h != m.fh_end(*fi); ++h)
    {
      const TriMesh::Point p = m.point(m.to_vertex_handle(*h));
      const float u = p[0] * 2.0f + (chart_b ? 50.0f : 1.0f);
      const float v = p[1] * 3.0f + (chart_b ? 60.0f : 2.0f);
      m.set_texcoord2D(*h, TriMesh::TexCoord2D(u, v));
    }
  }

  TASK_CHECK(discontinuous_interior_edges(m) == 2);

  Loop s;
  s.set_refine_texcoords(true);
  TASK_CHECK(s.attach(m) && s(1));
  s.detach();

  TASK_CHECK(all_corners_are_finite(m));
  TASK_CHECK(discontinuous_interior_edges(m) == 4);

  const TriMesh::VertexHandle c(4);
  const TriMesh::TexCoord2D center_a =
      tc<TriMesh>(3, 5) * 0.75f +
      (tc<TriMesh>(3, 2) + tc<TriMesh>(3, 8)) * 0.125f;
  const TriMesh::TexCoord2D center_b =
      tc<TriMesh>(52, 63) * 0.75f +
      (tc<TriMesh>(52, 60) + tc<TriMesh>(52, 66)) * 0.125f;

  TASK_CHECK(has_uv_at_vertex(m, c, center_a));
  TASK_CHECK(has_uv_at_vertex(m, c, center_b));
  TASK_CHECK(!near<TriMesh>(center_a, center_b));
  return 0;
}

// Equal coordinates at one vertex on both sides of a discontinuous edge must
// not merge the two texture fans.
int seams_equal_value_not_merged()
{
  EqualVertexSeam q = make_equal_vertex_seam();
  PolyMesh& m = q.mesh;

  TASK_CHECK(discontinuous_interior_edges(m) == 1);

  Catmull s;
  s.set_refine_texcoords(true);
  TASK_CHECK(s.attach(m) && s(1));
  s.detach();

  // Left chart: P=(5,5), boundary neighbours (0,0) and (9,1).
  const PolyMesh::TexCoord2D left =
      tc<PolyMesh>(5, 5) * 0.75f + (tc<PolyMesh>(0, 0) + tc<PolyMesh>(9, 1)) * 0.125f;
  // Right chart: P=(5,5) (numerically equal!), neighbours (12,2) and (7,7).
  const PolyMesh::TexCoord2D right =
      tc<PolyMesh>(5, 5) * 0.75f + (tc<PolyMesh>(12, 2) + tc<PolyMesh>(7, 7)) * 0.125f;

  TASK_CHECK(!near<PolyMesh>(left, right));
  TASK_CHECK(has_uv_at_vertex(m, q.shared, left));
  TASK_CHECK(has_uv_at_vertex(m, q.shared, right));
  return 0;
}

// Triangle charts around a seam for Loop.
int seams_triangle_pair_loop()
{
  TwoTriangles q = make_two_triangles_with_seam();
  TriMesh& m = q.mesh;

  Loop s;
  s.set_refine_texcoords(true);
  TASK_CHECK(s.attach(m) && s(1));
  s.detach();

  TASK_CHECK(all_corners_are_finite(m));

  // Two triangles become eight; the seam edge splits into two.
  TASK_CHECK(m.n_faces() == 8);
  TASK_CHECK(discontinuous_interior_edges(m) == 2);

  // Left triangle: P0=(0,0), P1=(1,0), P2=(1,1); each vertex has two
  // boundary neighbours.
  TASK_CHECK(has_uv_at_vertex(m, q.v[0], tc<TriMesh>(0, 0) * 0.75f +
                              (tc<TriMesh>(1, 0) + tc<TriMesh>(1, 1)) * 0.125f));
  TASK_CHECK(has_uv_at_vertex(m, q.v[1], tc<TriMesh>(1, 0) * 0.75f +
                              (tc<TriMesh>(0, 0) + tc<TriMesh>(1, 1)) * 0.125f));
  TASK_CHECK(has_uv_at_vertex(m, q.v[2], tc<TriMesh>(1, 1) * 0.75f +
                              (tc<TriMesh>(0, 0) + tc<TriMesh>(1, 0)) * 0.125f));

  // Right triangle: shifted chart, P1=(11,20), P3=(12,20), P2=(11,21).
  const TriMesh::TexCoord2D r0 = tc<TriMesh>(11, 20);
  const TriMesh::TexCoord2D r1 = tc<TriMesh>(12, 20);
  const TriMesh::TexCoord2D r2 = tc<TriMesh>(11, 21);
  TASK_CHECK(has_uv_at_vertex(m, q.v[1], r0 * 0.75f + (r1 + r2) * 0.125f));
  TASK_CHECK(has_uv_at_vertex(m, q.v[3], r1 * 0.75f + (r0 + r2) * 0.125f));
  TASK_CHECK(has_uv_at_vertex(m, q.v[2], r2 * 0.75f + (r0 + r1) * 0.125f));
  return 0;
}

// ========================================================= LIFECYCLE GROUP

// set_refine_texcoords is opt-in.
int lifecycle_option_default_off()
{
  Catmull c;
  Loop l;
  TASK_CHECK(!c.refine_texcoords());
  TASK_CHECK(!l.refine_texcoords());

  c.set_refine_texcoords(true);
  l.set_refine_texcoords(true);
  TASK_CHECK(c.refine_texcoords());
  TASK_CHECK(l.refine_texcoords());

  c.set_refine_texcoords(false);
  l.set_refine_texcoords(false);
  TASK_CHECK(!c.refine_texcoords());
  TASK_CHECK(!l.refine_texcoords());
  return 0;
}

// With the option on but no halfedge texcoords requested, the geometric path
// runs unchanged and no texture property is created or released.
int lifecycle_no_texcoords_requested()
{
  PolyMesh plain = make_poly_grid(3, 2, -1, -1);
  plain.release_halfedge_texcoords2D();

  PolyMesh ref = make_poly_grid(3, 2, -1, -1);
  ref.release_halfedge_texcoords2D();

  TASK_CHECK(!plain.has_halfedge_texcoords2D());
  TASK_CHECK(!ref.has_halfedge_texcoords2D());

  Catmull s;
  s.set_refine_texcoords(true);
  TASK_CHECK(s.attach(plain) && s(2));
  s.detach();

  Catmull sref;
  TASK_CHECK(sref.attach(ref) && sref(2));
  sref.detach();

  TASK_CHECK(same_mesh(plain, ref));
  TASK_CHECK(!plain.has_halfedge_texcoords2D());

  // Same for Loop.
  TriMesh tplain = make_tri_grid(2, false);
  tplain.release_halfedge_texcoords2D();
  TriMesh tref = make_tri_grid(2, false);
  tref.release_halfedge_texcoords2D();

  TASK_CHECK(!tplain.has_halfedge_texcoords2D());

  Loop l;
  l.set_refine_texcoords(true);
  TASK_CHECK(l.attach(tplain) && l(1));
  l.detach();

  Loop lref;
  TASK_CHECK(lref.attach(tref) && lref(1));
  lref.detach();

  TASK_CHECK(same_mesh(tplain, tref));
  TASK_CHECK(!tplain.has_halfedge_texcoords2D());
  return 0;
}

// Repeated attach/detach must not disturb the caller's texture property.
int lifecycle_repeated_attach_detach()
{
  PolyMesh m = make_poly_grid(3, 2, -1, -1);
  assign_affine_uv(m);

  TASK_CHECK(m.has_halfedge_texcoords2D());
  const size_t halfedges = m.n_halfedges();

  Catmull s;
  s.set_refine_texcoords(true);

  TASK_CHECK(s.attach(m));
  s.detach();
  s.detach(); // second detach is harmless

  TASK_CHECK(m.has_halfedge_texcoords2D());
  TASK_CHECK(m.n_halfedges() == halfedges);
  TASK_CHECK(max_affine_error(m) <= 1e-4f);

  TASK_CHECK(s.attach(m) && s(1));
  s.detach();
  TASK_CHECK(m.has_halfedge_texcoords2D());
  TASK_CHECK(all_corners_are_finite(m));

  TASK_CHECK(s.attach(m) && s(1));
  s.detach();
  TASK_CHECK(m.has_halfedge_texcoords2D());

  // The caller still owns the property and may release it.
  m.release_halfedge_texcoords2D();
  TASK_CHECK(!m.has_halfedge_texcoords2D());
  return 0;
}

// Two rounds in one call and two single-round calls give the same result.
int lifecycle_round_equivalence()
{
  PolyMesh a = make_poly_grid(3, 2, -1, -1);
  PolyMesh b = make_poly_grid(3, 2, -1, -1);
  assign_affine_uv(a);
  assign_affine_uv(b);

  Catmull s2;
  s2.set_refine_texcoords(true);
  TASK_CHECK(s2.attach(a) && s2(2));
  s2.detach();

  Catmull s1;
  s1.set_refine_texcoords(true);
  TASK_CHECK(s1.attach(b) && s1(1) && s1(1));
  s1.detach();

  TASK_CHECK(same_mesh(a, b));
  TASK_CHECK(same_corner_texcoords(a, b));

  TriMesh ta = make_tri_grid(2, false);
  TriMesh tb = make_tri_grid(2, false);
  assign_affine_uv(ta);
  assign_affine_uv(tb);

  Loop l2;
  l2.set_refine_texcoords(true);
  TASK_CHECK(l2.attach(ta) && l2(2));
  l2.detach();

  Loop l1;
  l1.set_refine_texcoords(true);
  TASK_CHECK(l1.attach(tb) && l1(1) && l1(1));
  l1.detach();

  TASK_CHECK(same_mesh(ta, tb));
  TASK_CHECK(same_corner_texcoords(ta, tb));
  return 0;
}

// Turning the option off leaves the existing corner coordinates untouched and
// produces a mesh identical to a plain geometric run.
int lifecycle_option_off_matches_plain()
{
  PolyMesh with = make_poly_grid(3, 2, -1, -1);
  PolyMesh without = make_poly_grid(3, 2, -1, -1);
  assign_affine_uv(with);
  assign_affine_uv(without);

  const std::vector<PolyMesh::TexCoord2D> old_uv = corner_texcoords(with);
  const size_t old_halfedges = with.n_halfedges();

  Catmull s0;
  s0.set_refine_texcoords(false);
  TASK_CHECK(s0.attach(with) && s0(2));
  s0.detach();

  Catmull s1;
  TASK_CHECK(s1.attach(without) && s1(2));
  s1.detach();

  TASK_CHECK(same_mesh(with, without));

  // Old halfedge indices are preserved by the edge split; their texcoords
  // must be exactly the values the caller stored, since refinement is off.
  size_t old_corners = 0;
  for (size_t i = 0; i < old_halfedges; ++i)
  {
    PolyMesh::HalfedgeHandle h(static_cast<int>(i));
    if (!with.face_handle(h).is_valid())
      continue;
    TASK_CHECK(with.texcoord2D(h) == old_uv[old_corners]);
    ++old_corners;
  }
  return 0;
}

} // namespace fvar_checks

// ------------------------------------------------------------------ runner

using namespace fvar_checks;

const struct task_case task_cases[] = {
  { "catmull",   "single_quad_explicit",     catmull_single_quad_explicit },
  { "catmull",   "irregular_valence_affine", catmull_irregular_valence_affine },
  { "catmull",   "ngon_explicit",            catmull_ngon_explicit },
  { "catmull",   "hole_boundary",            catmull_hole_boundary },
  { "catmull",   "two_chart_seam",           catmull_two_chart_seam },

  { "loop",      "single_triangle_explicit", loop_single_triangle_explicit },
  { "loop",      "irregular_valence_affine", loop_irregular_valence_affine },
  { "loop",      "hole_boundary",            loop_hole_boundary },

  { "seams",     "center_fans_catmull",      seams_center_fans_catmull },
  { "seams",     "center_fans_loop",         seams_center_fans_loop },
  { "seams",     "equal_value_not_merged",   seams_equal_value_not_merged },
  { "seams",     "triangle_pair_loop",       seams_triangle_pair_loop },

  { "lifecycle", "option_default_off",       lifecycle_option_default_off },
  { "lifecycle", "no_texcoords_requested",   lifecycle_no_texcoords_requested },
  { "lifecycle", "repeated_attach_detach",   lifecycle_repeated_attach_detach },
  { "lifecycle", "round_equivalence",        lifecycle_round_equivalence },
  { "lifecycle", "option_off_matches_plain", lifecycle_option_off_matches_plain }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
