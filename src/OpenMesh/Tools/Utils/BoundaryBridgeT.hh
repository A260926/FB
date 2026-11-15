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
 *  the License, or (at your option) any later version.                      *
 *                                                                           *
 *  OpenMesh is distributed in the hope that it will be useful,              *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of           *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
 *  GNU Lesser General Public License for more details.                      *
 *                                                                           *
 *  You should have received a copy of the GNU LesserGeneral Public          *
 *  License along with OpenMesh.  If not,                                    *
 *  see <http://www.gnu.org/licenses/>.                                      *
 *                                                                           *
\*===========================================================================*/

/*===========================================================================*\
 *                                                                           *
 *   $Revision$                                                         *
 *   $Date$                   *
 *                                                                           *
\*===========================================================================*/

//=============================================================================
//
//  Boundary bridge between two complete boundary loops
//
//=============================================================================

#ifndef OPENMESH_TOOLS_UTILS_BOUNDARYBRIDGET_HH
#define OPENMESH_TOOLS_UTILS_BOUNDARYBRIDGET_HH

//== INCLUDES =================================================================

#include <OpenMesh/Core/System/config.h>
#include <OpenMesh/Core/Geometry/VectorT.hh>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

//== NAMESPACES ===============================================================

namespace OpenMesh {
namespace Utils {


//== CLASS DEFINITION =========================================================

/** \brief Result of bridge_boundaries().
 *
 *  \c faces holds the handles of all newly created triangles in the order in
 *  which they were added (the order of the lattice path).  \c energy is the
 *  objective value of the selected strip: the sum of all new triangle areas
 *  plus lambda times the sum of squared differences of unit normals of
 *  neighbouring new triangles (including the first/last pair).
 */
template <class Mesh>
struct BoundaryBridgeResultT
{
  typedef typename Mesh::FaceHandle FaceHandle;

  std::vector<FaceHandle> faces;
  double                  energy;

  BoundaryBridgeResultT() : faces(), energy(0.0) {}
};


namespace detail {

//=============================================================================
// Small double precision 3D vector helpers.  The mesh point type is only used
// to read coordinates; all computations are carried out in double precision.
//=============================================================================

struct BridgeVec3
{
  double x, y, z;

  BridgeVec3() : x(0.0), y(0.0), z(0.0) {}
  BridgeVec3(double _x, double _y, double _z) : x(_x), y(_y), z(_z) {}
};

inline BridgeVec3 operator-(const BridgeVec3& _a, const BridgeVec3& _b)
{ return BridgeVec3(_a.x-_b.x, _a.y-_b.y, _a.z-_b.z); }

inline BridgeVec3 operator+(const BridgeVec3& _a, const BridgeVec3& _b)
{ return BridgeVec3(_a.x+_b.x, _a.y+_b.y, _a.z+_b.z); }

inline BridgeVec3 operator*(const BridgeVec3& _a, double _s)
{ return BridgeVec3(_a.x*_s, _a.y*_s, _a.z*_s); }

inline double bridge_dot(const BridgeVec3& _a, const BridgeVec3& _b)
{ return _a.x*_b.x + _a.y*_b.y + _a.z*_b.z; }

inline BridgeVec3 bridge_cross(const BridgeVec3& _a, const BridgeVec3& _b)
{
  return BridgeVec3(_a.y*_b.z - _a.z*_b.y,
                    _a.z*_b.x - _a.x*_b.z,
                    _a.x*_b.y - _a.y*_b.x);
}

inline double bridge_norm(const BridgeVec3& _a)
{ return std::sqrt(bridge_dot(_a, _a)); }

inline double bridge_sqr_distance(const BridgeVec3& _a, const BridgeVec3& _b)
{
  const BridgeVec3 d(_a - _b);
  return bridge_dot(d, d);
}

template <class Point>
inline BridgeVec3 bridge_point(const Point& _p)
{
  return BridgeVec3(double(_p[0]), double(_p[1]), double(_p[2]));
}

template <class Point>
inline bool bridge_finite_point(const Point& _p)
{
  return std::isfinite(double(_p[0])) &&
         std::isfinite(double(_p[1])) &&
         std::isfinite(double(_p[2]));
}

//=============================================================================
// Geometry of one prospective strip triangle.
//=============================================================================

struct BridgeTriangle
{
  double     area;
  BridgeVec3 normal;   // unit normal, oriented like the prospective face
  bool       valid;    // area > 1e-12

  BridgeTriangle() : area(0.0), valid(false) {}
};

template <class Mesh>
BridgeTriangle bridge_triangle(const Mesh& _mesh,
                               typename Mesh::VertexHandle _v0,
                               typename Mesh::VertexHandle _v1,
                               typename Mesh::VertexHandle _v2)
{
  BridgeTriangle t;
  const BridgeVec3 p0(bridge_point(_mesh.point(_v0)));
  const BridgeVec3 p1(bridge_point(_mesh.point(_v1)));
  const BridgeVec3 p2(bridge_point(_mesh.point(_v2)));

  const BridgeVec3 c(bridge_cross(p1 - p0, p2 - p0));
  const double len(bridge_norm(c));
  t.area = 0.5 * len;

  if (!(t.area > 1e-12))     // also rejects NaN
  {
    t.valid = false;
    return t;
  }

  t.normal = c * (1.0 / len);
  t.valid  = true;
  return t;
}

//=============================================================================
// One step of a monotone lattice path.
//   dir == 0 : "right" step  (a_i, a_{i+1}, b_j)
//   dir == 1 : "up"    step  (a_i, b_{j+1}, b_j)
//=============================================================================

struct BridgeStep
{
  int dir;
  int i;
  int j;

  BridgeStep() : dir(0), i(0), j(0) {}
  BridgeStep(int _dir, int _i, int _j) : dir(_dir), i(_i), j(_j) {}
};

} // namespace detail


//== IMPLEMENTATION ===========================================================

/** \brief Connect two complete boundary loops of a triangle mesh with an
 *         optimally selected triangle strip.
 *
 *  Both input vectors must list one complete boundary loop each, in the
 *  direction of the \c next pointers of the loop's boundary (faceless)
 *  halfedges.  The first vertex is not repeated at the end.  The edge between
 *  \c first[0] and \c second[0] is the designated seam.
 *
 *  Writing <tt>a = first</tt> and <tt>b</tt> for \c second traversed in the
 *  opposite direction starting at \c second[0], the allowed strips are the
 *  monotone lattice paths from (0,0) to (n,m).  A right step at (i,j) adds
 *  the triangle <tt>(a_i, a_{i+1}, b_j)</tt>, an up step adds
 *  <tt>(a_i, b_{j+1}, b_j)</tt> (indices modulo the loop lengths).  A pair of
 *  cross loop vertices may only be used once, with the single exception of
 *  the seam pair closing the strip between the first and the last triangle.
 *  Every new triangle must have an area greater than 1e-12 and pre-existing
 *  cross loop edges may not be reused.
 *
 *  The selected strip minimises
 *  \f[
 *      \sum_{\text{new }f} \mathrm{area}(f)
 *      + \lambda \sum_{(f,g)\text{ adjacent}} \|n_f - n_g\|^2 ,
 *  \f]
 *  where the adjacency is cyclic (the first and the last triangle are
 *  neighbours as well).
 *
 *  On success, all new faces are inserted into the mesh, their halfedge
 *  texture coordinates (standard attribute <tt>h:texcoords2D</tt>) are
 *  assigned, and already requested face/vertex normals are recomputed via
 *  OpenMesh's normal update path.  Existing vertices, edges, faces, their
 *  handles and positions are left untouched; no edge is split and no vertex
 *  is welded.  Other boundary loops are unaffected.
 *
 *  \param _mesh   The mesh to modify (typically a TriMesh_ArrayKernelT).
 *  \param _first  Complete first boundary loop, in boundary \c next direction.
 *  \param _second Complete second boundary loop, in boundary \c next direction.
 *  \param _lambda Non-negative finite weight of the normal smoothness term.
 *  \param _result Receives the new face handles and the minimal energy.
 *
 *  \throws std::invalid_argument if a loop is incomplete, repeated or shares
 *          vertices with the other loop, if a handle/parameter is invalid, or
 *          if no legal strip exists.  In that case the mesh and \p _result are
 *          left unchanged.
 */
template <class Mesh>
void bridge_boundaries(
    Mesh& _mesh,
    const std::vector<typename Mesh::VertexHandle>& _first,
    const std::vector<typename Mesh::VertexHandle>& _second,
    double _lambda,
    BoundaryBridgeResultT<Mesh>& _result)
{
  typedef typename Mesh::VertexHandle  VertexHandle;
  typedef typename Mesh::HalfedgeHandle HalfedgeHandle;
  typedef typename Mesh::FaceHandle    FaceHandle;
  typedef detail::BridgeVec3           Vec3;
  typedef detail::BridgeTriangle       Tri;
  typedef detail::BridgeStep           Step;

  //---------------------------------------------------------------- input checks
  if (!std::isfinite(_lambda) || _lambda < 0.0)
    throw std::invalid_argument("bridge_boundaries: lambda must be finite and non-negative");

  const std::size_t n = _first.size();
  const std::size_t m = _second.size();

  if (n < 3 || n > 32 || m < 3 || m > 32)
    throw std::invalid_argument("bridge_boundaries: boundary loops must contain 3 to 32 vertices");

  // Validate every handle and the coordinates of the corresponding points.
  for (std::size_t i = 0; i < n; ++i)
  {
    const VertexHandle vh = _first[i];
    if (!_mesh.is_valid_handle(vh))
      throw std::invalid_argument("bridge_boundaries: invalid vertex handle in first loop");
    if (_mesh.has_vertex_status() && _mesh.status(vh).deleted())
      throw std::invalid_argument("bridge_boundaries: deleted vertex in first loop");
    if (!detail::bridge_finite_point(_mesh.point(vh)))
      throw std::invalid_argument("bridge_boundaries: non-finite vertex coordinate");
  }
  for (std::size_t j = 0; j < m; ++j)
  {
    const VertexHandle vh = _second[j];
    if (!_mesh.is_valid_handle(vh))
      throw std::invalid_argument("bridge_boundaries: invalid vertex handle in second loop");
    if (_mesh.has_vertex_status() && _mesh.status(vh).deleted())
      throw std::invalid_argument("bridge_boundaries: deleted vertex in second loop");
    if (!detail::bridge_finite_point(_mesh.point(vh)))
      throw std::invalid_argument("bridge_boundaries: non-finite vertex coordinate");
  }

  // Vertices inside a loop must be distinct, and the two loops must not share
  // any vertex.
  for (std::size_t i = 0; i < n; ++i)
    for (std::size_t k = i + 1; k < n; ++k)
      if (_first[i] == _first[k])
        throw std::invalid_argument("bridge_boundaries: repeated vertex in first loop");

  for (std::size_t j = 0; j < m; ++j)
    for (std::size_t k = j + 1; k < m; ++k)
      if (_second[j] == _second[k])
        throw std::invalid_argument("bridge_boundaries: repeated vertex in second loop");

  for (std::size_t i = 0; i < n; ++i)
    for (std::size_t j = 0; j < m; ++j)
      if (_first[i] == _second[j])
        throw std::invalid_argument("bridge_boundaries: boundary loops share a vertex");

  // Verify that each vector is a complete boundary loop traversed along the
  // next pointers of its faceless halfedges.
  std::vector<HalfedgeHandle> first_h(n), second_h(m);

  for (std::size_t i = 0; i < n; ++i)
  {
    const HalfedgeHandle h = _mesh.find_halfedge(_first[i], _first[(i + 1) % n]);
    if (!h.is_valid() || !_mesh.is_boundary(h))
      throw std::invalid_argument("bridge_boundaries: first loop is not a boundary next-direction loop");
    first_h[i] = h;
  }
  for (std::size_t i = 0; i < n; ++i)
  {
    if (_mesh.next_halfedge_handle(first_h[i]) != first_h[(i + 1) % n])
      throw std::invalid_argument("bridge_boundaries: first loop is incomplete");
  }

  for (std::size_t j = 0; j < m; ++j)
  {
    const HalfedgeHandle h = _mesh.find_halfedge(_second[j], _second[(j + 1) % m]);
    if (!h.is_valid() || !_mesh.is_boundary(h))
      throw std::invalid_argument("bridge_boundaries: second loop is not a boundary next-direction loop");
    second_h[j] = h;
  }
  for (std::size_t j = 0; j < m; ++j)
  {
    if (_mesh.next_halfedge_handle(second_h[j]) != second_h[(j + 1) % m])
      throw std::invalid_argument("bridge_boundaries: second loop is incomplete");
  }

  //---------------------------------------------------------------- loops a / b
  // a_i = first[i]
  // b_j = second[(m - j) % m]   (second traversed backwards from its first vertex)
  std::vector<VertexHandle> a(n);
  std::vector<VertexHandle> b(m);
  for (std::size_t i = 0; i < n; ++i)
    a[i] = _first[i];
  for (std::size_t j = 0; j < m; ++j)
    b[j] = _second[(m - j) % m];

  // Closed versions of the arrays including the seam copy (index n / m).
  std::vector<VertexHandle> a_closed(n + 1), b_closed(m + 1);
  for (std::size_t i = 0; i < n; ++i) a_closed[i] = a[i];
  for (std::size_t j = 0; j < m; ++j) b_closed[j] = b[j];
  a_closed[n] = a[0];
  b_closed[m] = b[0];

  //---------------------------------------------------------------- normalized S
  std::vector<double> s(n + 1, 0.0), t(m + 1, 0.0);
  double perimeter_a = 0.0, perimeter_b = 0.0;

  for (std::size_t i = 0; i < n; ++i)
  {
    const Vec3 p0(detail::bridge_point(_mesh.point(a_closed[i])));
    const Vec3 p1(detail::bridge_point(_mesh.point(a_closed[i + 1])));
    perimeter_a += detail::bridge_norm(p1 - p0);
    s[i + 1] = perimeter_a;
  }
  for (std::size_t j = 0; j < m; ++j)
  {
    const Vec3 p0(detail::bridge_point(_mesh.point(b_closed[j])));
    const Vec3 p1(detail::bridge_point(_mesh.point(b_closed[j + 1])));
    perimeter_b += detail::bridge_norm(p1 - p0);
    t[j + 1] = perimeter_b;
  }

  if (!(perimeter_a > 0.0) || !(perimeter_b > 0.0))
    throw std::invalid_argument("bridge_boundaries: degenerate boundary loop geometry");

  for (std::size_t i = 0; i <= n; ++i) s[i] /= perimeter_a;
  for (std::size_t j = 0; j <= m; ++j) t[j] /= perimeter_b;

  //----------------------------------------------------- triangle precompute
  // R(i,j): right step at (i,j), valid for i in [0,n), j in [0,m]
  // U(i,j): up    step at (i,j), valid for i in [0,n], j in [0,m)
  const std::size_t ni = n + 1;
  const std::size_t mj = m + 1;

  std::vector<Tri> R(n * mj), U(ni * m);
#define BB_RIDX(ii, jj) (std::size_t(ii) * mj + std::size_t(jj))
#define BB_UIDX(ii, jj) (std::size_t(ii) * m   + std::size_t(jj))

  for (std::size_t i = 0; i < n; ++i)
    for (std::size_t j = 0; j < mj; ++j)
      R[BB_RIDX(i, j)] = detail::bridge_triangle(_mesh, a_closed[i], a_closed[i + 1], b_closed[j]);

  for (std::size_t i = 0; i < ni; ++i)
    for (std::size_t j = 0; j < m; ++j)
      U[BB_UIDX(i, j)] = detail::bridge_triangle(_mesh, a_closed[i], b_closed[j + 1], b_closed[j]);

  //----------------------------------------- pre-existing cross loop edges
  // existing(i,j) tells whether the mesh already contains an edge between
  // a_i and b_j.  Such a vertex of the lattice may not be visited by a path.
  std::vector<unsigned char> existing(ni * mj, 0);
  for (std::size_t i = 0; i < ni; ++i)
    for (std::size_t j = 0; j < mj; ++j)
      existing[std::size_t(i) * mj + j] =
          _mesh.find_halfedge(a_closed[i], b_closed[j]).is_valid() ? 1 : 0;

#define BB_EIDX(ii, jj) (std::size_t(ii) * mj + std::size_t(jj))

  //============================================================ DP machinery
  // A state is (i,j,dir) where dir is the direction of the step that arrived
  // at (i,j).  dir == 0 (right) means the previous triangle is R(i-1,j),
  // dir == 1 (up) means it is U(i,j-1).
  struct Entry
  {
    double cost;
    int    pi, pj, pdir;   // predecessor; -1/-1/-1 for the initial state
    Entry() : cost(std::numeric_limits<double>::infinity()), pi(-1), pj(-1), pdir(-1) {}
  };

  double best_energy = std::numeric_limits<double>::infinity();
  std::vector<Step> best_path;

  // Iterate over the two families of paths.  Every monotone path begins with a
  // (non-empty) maximal run in its first direction and is uniquely identified
  // by that run length.  Family 0 covers paths whose first step is a right
  // step: initial right run I in [1,n-1] followed by one forced up step.
  // Family 1 covers paths whose first step is an up step: initial up run J in
  // [1,m-1] followed by one forced right step.
  //
  // The run length is capped at n-1 / m-1 because a full run would reach the
  // lattice corner (n,0) resp. (0,m); those corners share the vertex pair
  // (a_0,b_0) with the seam and would use the seam pair a second time.  The
  // arrival constraints inside the DP prevent the remaining aliasing: a
  // vertex pair may be visited at most once, except for the seam pair that is
  // revisited only at the closing corner (n,m).
  for (int family = 0; family < 2; ++family)
  {
    const int max_run = (family == 0) ? int(n) - 1 : int(m) - 1;
    for (int run = 1; run <= max_run; ++run)
    {
      std::vector<Entry> dp(2 * ni * mj);
#define BB_DIDX(ii, jj, dd) ((std::size_t(ii) * mj + std::size_t(jj)) * 2 + std::size_t(dd))

      std::vector<Step> initial_steps;
      int start_i = 0, start_j = 0, start_dir = 0;
      double initial_cost = 0.0;
      bool case_ok = true;

      if (family == 0) // initial right run
      {
        const int I = run;
        start_i = I;
        start_j = 1;
        start_dir = 1; // the forced step is an up step; it arrived here

        // All lattice vertices on the initial run and the forced step target.
        for (int k = 0; k <= I && case_ok; ++k)
          if (existing[BB_EIDX(k, 0)]) case_ok = false;
        if (existing[BB_EIDX(I, 1)]) case_ok = false;

        for (int k = 0; k < I && case_ok; ++k)
          if (!R[BB_RIDX(k, 0)].valid) case_ok = false;
        if (case_ok && !U[BB_UIDX(I, 0)].valid) case_ok = false;

        if (case_ok)
        {
          for (int k = 0; k < I; ++k)
          {
            initial_cost += R[BB_RIDX(k, 0)].area;
            if (k > 0)
              initial_cost += _lambda *
                  detail::bridge_sqr_distance(R[BB_RIDX(k - 1, 0)].normal,
                                              R[BB_RIDX(k, 0)].normal);
            initial_steps.push_back(Step(0, k, 0));
          }
          initial_cost += U[BB_UIDX(I, 0)].area;
          initial_cost += _lambda *
              detail::bridge_sqr_distance(R[BB_RIDX(I - 1, 0)].normal,
                                          U[BB_UIDX(I, 0)].normal);
          initial_steps.push_back(Step(1, I, 0));
        }
      }
      else // initial up run
      {
        const int J = run;
        start_i = 1;
        start_j = J;
        start_dir = 0; // forced right step arrives here

        for (int k = 0; k <= J && case_ok; ++k)
          if (existing[BB_EIDX(0, k)]) case_ok = false;
        if (existing[BB_EIDX(1, J)]) case_ok = false;

        for (int k = 0; k < J && case_ok; ++k)
          if (!U[BB_UIDX(0, k)].valid) case_ok = false;
        if (case_ok && !R[BB_RIDX(0, J)].valid) case_ok = false;

        if (case_ok)
        {
          for (int k = 0; k < J; ++k)
          {
            initial_cost += U[BB_UIDX(0, k)].area;
            if (k > 0)
              initial_cost += _lambda *
                  detail::bridge_sqr_distance(U[BB_UIDX(0, k - 1)].normal,
                                              U[BB_UIDX(0, k)].normal);
            initial_steps.push_back(Step(1, 0, k));
          }
          initial_cost += R[BB_RIDX(0, J)].area;
          initial_cost += _lambda *
              detail::bridge_sqr_distance(U[BB_UIDX(0, J - 1)].normal,
                                          R[BB_RIDX(0, J)].normal);
          initial_steps.push_back(Step(0, 0, J));
        }
      }

      if (!case_ok)
        continue;

      Entry& start = dp[BB_DIDX(start_i, start_j, start_dir)];
      start.cost = initial_cost;
      start.pi = -1;

      // Topological order: increasing i, then increasing j.
      for (std::size_t i = 0; i < ni; ++i)
      {
        for (std::size_t j = 0; j < mj; ++j)
        {
          for (int dir = 0; dir < 2; ++dir)
          {
            const std::size_t sidx = BB_DIDX(i, j, dir);
            if (!std::isfinite(dp[sidx].cost))
              continue;

            Vec3 prev_normal;
            if (dir == 0)
              prev_normal = R[BB_RIDX(i - 1, j)].normal;
            else
              prev_normal = U[BB_UIDX(i, j - 1)].normal;

            // right step
            if (i < n && R[BB_RIDX(i, j)].valid)
            {
              const std::size_t t_i = i + 1, t_j = j;
              bool ok = !existing[BB_EIDX(t_i, t_j)];

              // A cross-loop vertex pair may only be used once.  Lattice
              // points (0,j) and (n,j) share the same pair (a_0,b_j).  In
              // family 1 the initial up run visited (0,1)..(0,J), so the
              // first arrival at i == n must not happen at height <= J
              // (arriving at (n,m) is the allowed closure).
              if (ok && family == 1 && t_i == n && int(t_j) <= run)
                ok = false;

              if (ok)
              {
                const double cost = dp[sidx].cost + R[BB_RIDX(i, j)].area +
                    _lambda * detail::bridge_sqr_distance(prev_normal, R[BB_RIDX(i, j)].normal);
                Entry& dst = dp[BB_DIDX(t_i, t_j, 0)];
                if (cost < dst.cost)
                {
                  dst.cost = cost;
                  dst.pi = int(i); dst.pj = int(j); dst.pdir = dir;
                }
              }
            }

            // up step
            if (j < m && U[BB_UIDX(i, j)].valid)
            {
              const std::size_t t_i = i, t_j = j + 1;
              bool ok = !existing[BB_EIDX(t_i, t_j)];

              // Symmetric to the right step: points (i,0) and (i,m) share
              // the pair (a_i,b_0).  In family 0 the initial right run
              // visited (1,0)..(I,0), so the first arrival at j == m must
              // not happen at width <= I (the corner (n,m) is the closure).
              if (ok && family == 0 && t_j == m && int(t_i) <= run)
                ok = false;

              if (ok)
              {
                const double cost = dp[sidx].cost + U[BB_UIDX(i, j)].area +
                    _lambda * detail::bridge_sqr_distance(prev_normal, U[BB_UIDX(i, j)].normal);
                Entry& dst = dp[BB_DIDX(t_i, t_j, 1)];
                if (cost < dst.cost)
                {
                  dst.cost = cost;
                  dst.pi = int(i); dst.pj = int(j); dst.pdir = dir;
                }
              }
            }
          }
        }
      }

      // Finish: close the cyclic adjacency with the first triangle.
      const Vec3 first_normal = (family == 0) ? R[BB_RIDX(0, 0)].normal
                                             : U[BB_UIDX(0, 0)].normal;

      for (int dir = 0; dir < 2; ++dir)
      {
        const std::size_t sidx = BB_DIDX(n, m, dir);
        if (!std::isfinite(dp[sidx].cost))
          continue;

        const Vec3 last_normal = (dir == 0) ? R[BB_RIDX(n - 1, m)].normal
                                           : U[BB_UIDX(n, m - 1)].normal;
        const double energy = dp[sidx].cost +
            _lambda * detail::bridge_sqr_distance(last_normal, first_normal);

        if (energy < best_energy)
        {
          // Reconstruct the DP part backwards, then reverse it.
          std::vector<Step> tail;
          int ci = int(n), cj = int(m), cd = dir;
          while (dp[BB_DIDX(ci, cj, cd)].pi >= 0)
          {
            const Entry& e = dp[BB_DIDX(ci, cj, cd)];
            if (cd == 0)
              tail.push_back(Step(0, ci - 1, cj));
            else
              tail.push_back(Step(1, ci, cj - 1));
            ci = e.pi; cj = e.pj; cd = e.pdir;
          }
          std::reverse(tail.begin(), tail.end());

          best_path.clear();
          best_path.reserve(initial_steps.size() + tail.size());
          best_path.insert(best_path.end(), initial_steps.begin(), initial_steps.end());
          best_path.insert(best_path.end(), tail.begin(), tail.end());
          best_energy = energy;
        }
      }
    }
  }

#undef BB_RIDX
#undef BB_UIDX
#undef BB_EIDX
#undef BB_DIDX

  if (best_path.empty() || !std::isfinite(best_energy))
    throw std::invalid_argument("bridge_boundaries: no legal triangle strip between the loops");

  if (best_path.size() != n + m)
    throw std::invalid_argument("bridge_boundaries: internal path construction failed");

  //----------------------------------------------------------- mesh mutation
  // Only from here on the mesh is modified.  All faces are inserted first;
  // texture coordinates are written in a second pass, so a failure during
  // face insertion cannot leave a partially populated texture attribute.
  std::vector<FaceHandle> new_faces;
  new_faces.reserve(best_path.size());

  for (std::size_t k = 0; k < best_path.size(); ++k)
  {
    const Step& step = best_path[k];

    VertexHandle v0, v1, v2;
    if (step.dir == 0)
    {
      // (a_i, a_{i+1}, b_j)
      v0 = a_closed[std::size_t(step.i)];
      v1 = a_closed[std::size_t(step.i) + 1];
      v2 = b_closed[std::size_t(step.j)];
    }
    else
    {
      // (a_i, b_{j+1}, b_j)
      v0 = a_closed[std::size_t(step.i)];
      v1 = b_closed[std::size_t(step.j) + 1];
      v2 = b_closed[std::size_t(step.j)];
    }

    const FaceHandle fh = _mesh.add_face(v0, v1, v2);
    if (!fh.is_valid())
      throw std::invalid_argument("bridge_boundaries: face insertion failed");

    new_faces.push_back(fh);
  }

  //------------------------------------------------------- texture coordinates
  if (!_mesh.has_halfedge_texcoords2D())
    _mesh.request_halfedge_texcoords2D();

  for (std::size_t k = 0; k < best_path.size(); ++k)
  {
    const Step& step = best_path[k];
    const FaceHandle fh = new_faces[k];

    VertexHandle v0, v1, v2;
    double       u0, w0, u1, w1, u2, w2;

    if (step.dir == 0)
    {
      // (a_i, a_{i+1}, b_j)
      const std::size_t i = std::size_t(step.i);
      const std::size_t j = std::size_t(step.j);
      v0 = a_closed[i];     u0 = s[i];     w0 = 0.0;
      v1 = a_closed[i + 1]; u1 = s[i + 1]; w1 = 0.0;
      v2 = b_closed[j];     u2 = t[j];     w2 = 1.0;
    }
    else
    {
      // (a_i, b_{j+1}, b_j)
      const std::size_t i = std::size_t(step.i);
      const std::size_t j = std::size_t(step.j);
      v0 = a_closed[i];     u0 = s[i];     w0 = 0.0;
      v1 = b_closed[j + 1]; u1 = t[j + 1]; w1 = 1.0;
      v2 = b_closed[j];     u2 = t[j];     w2 = 1.0;
    }

    // A halfedge texture coordinate belongs to the corner at its to-vertex.
    const VertexHandle corner_v[3] = { v0, v1, v2 };
    const double       corner_u[3] = { u0, u1, u2 };
    const double       corner_w[3] = { w0, w1, w2 };

    for (int c = 0; c < 3; ++c)
    {
      for (typename Mesh::FaceHalfedgeIter fh_it = _mesh.fh_iter(fh);
           fh_it.is_valid(); ++fh_it)
      {
        if (_mesh.to_vertex_handle(*fh_it) == corner_v[c])
        {
          _mesh.set_texcoord2D(*fh_it,
              typename Mesh::TexCoord2D(
                  typename Mesh::TexCoord2D::value_type(corner_u[c]),
                  typename Mesh::TexCoord2D::value_type(corner_w[c])));
          break;
        }
      }
    }
  }

  //---------------------------------------------------------- recompute normals
  // Use OpenMesh's own normal update path when possible.  update_normals()
  // recomputes face normals first and then vertex/halfedge normals from them,
  // so it is the canonical way to refresh already requested normal attributes.
  if (_mesh.has_face_normals())
  {
    _mesh.update_normals();
  }
  else if (_mesh.has_vertex_normals())
  {
    // Vertex normals without face normals: average the geometric face normals
    // directly, mirroring calc_vertex_normal().
    for (typename Mesh::VertexIter v_it = _mesh.vertices_begin();
         v_it != _mesh.vertices_end(); ++v_it)
    {
      typename Mesh::Normal n(0.0, 0.0, 0.0);
      for (typename Mesh::ConstVertexFaceIter vf_it = _mesh.cvf_iter(*v_it);
           vf_it.is_valid(); ++vf_it)
        n += _mesh.calc_face_normal(*vf_it);

      const typename Mesh::Normal::value_type len = n.length();
      if (len != typename Mesh::Normal::value_type(0))
        n *= typename Mesh::Normal::value_type(1) / len;
      _mesh.set_normal(*v_it, n);
    }
  }

  //---------------------------------------------------------------- result
  _result.faces.swap(new_faces);
  _result.energy = best_energy;
}


//=============================================================================
} // namespace Utils
} // namespace OpenMesh
//=============================================================================

#endif // OPENMESH_TOOLS_UTILS_BOUNDARYBRIDGET_HH
//=============================================================================
