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
 *  GNU Lesser General Public License for more details.                      *
 *                                                                           *
 *  You should have received a copy of the GNU LesserGeneral Public          *
 *  License along with OpenMesh.  If not,                                    *
 *  see <http://www.gnu.org/licenses/>.                                      *
 *                                                                           *
\*===========================================================================*/

/** \file FaceVaryingTexcoordsT.hh

    Helper computing the face-varying (per face corner) texture coordinates
    for one round of uniform Catmull-Clark or Loop subdivision.

    The helper never changes the geometry or the topology of the mesh.  It
    only precomputes texture coordinate values that the subdivider then
    writes to the halfedges created while splitting the mesh.
 */

#ifndef OPENMESH_SUBDIVIDER_UNIFORM_FACEVARYINGTEXCOORDST_HH
#define OPENMESH_SUBDIVIDER_UNIFORM_FACEVARYINGTEXCOORDST_HH

//== INCLUDES =================================================================

#include <OpenMesh/Core/System/config.hh>

#include <map>
#include <set>
#include <utility>
#include <vector>
#include <cmath>

//== NAMESPACES ===============================================================

namespace OpenMesh   {
namespace Subdivider {
namespace Uniform    {

//== CLASS DEFINITION =========================================================

/** Precompute per-round face-varying texture coordinates.
 *
 *  A texture chart is a maximal set of faces glued along edges whose
 *  texture coordinates agree exactly on both sides.  Around a geometric
 *  vertex, the face corners connected through such continuous edges form
 *  a texture face fan.  Each fan is treated as an independent control
 *  mesh vertex.
 */
template <typename MeshType, typename RealType>
class FaceVaryingTexcoordsT
{
public:

  typedef MeshType                                      mesh_t;
  typedef typename MeshType::TexCoord2D                 TexCoord;
  typedef typename TexCoord::value_type                 tex_scalar_t;

  typedef typename MeshType::FaceHandle                 FaceHandle;
  typedef typename MeshType::EdgeHandle                 EdgeHandle;
  typedef typename MeshType::HalfedgeHandle             HalfedgeHandle;
  typedef typename MeshType::VertexHandle               VertexHandle;

  typedef typename MeshType::FaceIter                   FaceIter;
  typedef typename MeshType::EdgeIter                   EdgeIter;
  typedef typename MeshType::HalfedgeIter               HalfedgeIter;

  typedef std::pair<RealType, RealType>                 weight_t;
  typedef std::vector<weight_t>                         weights_t;

  enum Scheme { CATMULL_CLARK, LOOP };

public:

  explicit FaceVaryingTexcoordsT(Scheme _scheme)
    : scheme_(_scheme), weights_(0)
  {}

  void set_scheme(Scheme _scheme) { scheme_ = _scheme; }

  /// Optionally supply the Loop smooth weights used by LoopT.
  void set_weights(const weights_t* _weights) { weights_ = _weights; }

public:

  /// Read all corner coordinates and precompute this round's values.
  void prepare(mesh_t& _m)
  {
    const size_t nh = _m.n_halfedges();
    const size_t nf = _m.n_faces();

    uv_.assign(nh, TexCoord(tex_scalar_t(0), tex_scalar_t(0)));
    corner_new_.assign(nh, TexCoord(tex_scalar_t(0), tex_scalar_t(0)));
    edge_point_.assign(nh, TexCoord(tex_scalar_t(0), tex_scalar_t(0)));
    face_point_.assign(nf, TexCoord(tex_scalar_t(0), tex_scalar_t(0)));

    std::vector<int> parent(nh);
    std::vector<char> is_corner(nh, 0);

    for (size_t i = 0; i < nh; ++i)
      parent[i] = int(i);

    // Read the old face corner coordinates.
    for (HalfedgeIter h_it = _m.halfedges_begin();
         h_it != _m.halfedges_end(); ++h_it)
    {
      const HalfedgeHandle hh = *h_it;
      if (!_m.face_handle(hh).is_valid())
        continue;
      is_corner[hh.idx()] = 1;
      uv_[hh.idx()] = _m.texcoord2D(hh);
    }

    // Chart face points: centroid of the corner coordinates of the face.
    for (FaceIter f_it = _m.faces_begin(); f_it != _m.faces_end(); ++f_it)
    {
      const FaceHandle fh = *f_it;
      TexCoord sum(tex_scalar_t(0), tex_scalar_t(0));
      size_t n = 0;

      const HalfedgeHandle start = _m.halfedge_handle(fh);
      HalfedgeHandle hh = start;
      do
      {
        sum += uv_[hh.idx()];
        ++n;
        hh = _m.next_halfedge_handle(hh);
      }
      while (hh != start);

      if (n)
        face_point_[fh.idx()] = sum / tex_scalar_t(n);
    }

    // Classify edges and glue corners belonging to the same fan.
    std::vector<char> edge_cont(_m.n_edges(), 0);

    for (EdgeIter e_it = _m.edges_begin(); e_it != _m.edges_end(); ++e_it)
    {
      const EdgeHandle eh = *e_it;
      const HalfedgeHandle h0 = _m.halfedge_handle(eh, 0);
      const HalfedgeHandle h1 = _m.halfedge_handle(eh, 1);

      const bool f0 = _m.face_handle(h0).is_valid();
      const bool f1 = _m.face_handle(h1).is_valid();

      bool cont = false;
      if (f0 && f1)
      {
        const HalfedgeHandle p0 = _m.prev_halfedge_handle(h0);
        const HalfedgeHandle p1 = _m.prev_halfedge_handle(h1);
        cont = (uv_[h0.idx()] == uv_[p1.idx()]) &&
               (uv_[p0.idx()] == uv_[h1.idx()]);

        if (cont)
        {
          uf_unite(parent, h0.idx(), p1.idx());
          uf_unite(parent, p0.idx(), h1.idx());
        }
      }
      edge_cont[eh.idx()] = char(cont ? 1 : 0);

      if (cont)
      {
        TexCoord ep;
        if (scheme_ == CATMULL_CLARK)
        {
          ep  = uv_[h0.idx()];
          ep += uv_[h1.idx()];
          ep += face_point_[_m.face_handle(h0).idx()];
          ep += face_point_[_m.face_handle(h1).idx()];
          ep *= tex_scalar_t(0.25);
        }
        else
        {
          ep  = (uv_[h0.idx()] + uv_[h1.idx()]) * tex_scalar_t(3.0);
          ep += uv_[_m.next_halfedge_handle(h0).idx()];
          ep += uv_[_m.next_halfedge_handle(h1).idx()];
          ep *= tex_scalar_t(0.125);
        }
        edge_point_[h0.idx()] = ep;
        edge_point_[h1.idx()] = ep;
      }
      else
      {
        // Texture boundary edge: average of the two end points, taken
        // independently on each side.
        if (f0)
          edge_point_[h0.idx()] =
              (uv_[h0.idx()] + uv_[_m.prev_halfedge_handle(h0).idx()])
              * tex_scalar_t(0.5);
        if (f1)
          edge_point_[h1.idx()] =
              (uv_[h1.idx()] + uv_[_m.prev_halfedge_handle(h1).idx()])
              * tex_scalar_t(0.5);
      }
    }

    // Fan ids per corner.
    std::vector<int> fan(nh, -1);
    std::map<int, TexCoord> fan_pos;

    for (HalfedgeIter h_it = _m.halfedges_begin();
         h_it != _m.halfedges_end(); ++h_it)
    {
      const HalfedgeHandle hh = *h_it;
      if (!is_corner[hh.idx()])
        continue;
      const int id = uf_find(parent, hh.idx());
      fan[hh.idx()] = id;
      if (fan_pos.find(id) == fan_pos.end())
        fan_pos[id] = uv_[hh.idx()];
    }

    // Chart edges per fan.  A fan's valence counts geometric edges, so the
    // two sides of one continuous edge do not count twice; a multi-edge
    // still contributes each of its geometric edges separately.
    std::map<int, std::map<int, int> > fan_edge_other;   // fan -> (edge -> other fan)
    std::map<int, std::set<int> >      fan_boundary_edges;
    std::map<int, std::set<int> >      fan_faces;

    for (HalfedgeIter h_it = _m.halfedges_begin();
         h_it != _m.halfedges_end(); ++h_it)
    {
      const HalfedgeHandle hh = *h_it;
      if (!is_corner[hh.idx()])
        continue;

      const HalfedgeHandle ph = _m.prev_halfedge_handle(hh);
      if (!is_corner[ph.idx()])
        continue;

      const int a = fan[hh.idx()];
      const int b = fan[ph.idx()];
      if (a < 0 || b < 0)
        continue;

      const int eidx = _m.edge_handle(hh).idx();
      const bool boundary = !edge_cont[eidx];

      fan_edge_other[a][eidx] = b;
      fan_edge_other[b][eidx] = a;

      if (boundary)
      {
        fan_boundary_edges[a].insert(eidx);
        fan_boundary_edges[b].insert(eidx);
      }

      const int fidx = _m.face_handle(hh).idx();
      fan_faces[a].insert(fidx);
      fan_faces[b].insert(fidx);
    }

    // New position of every fan (control mesh vertex).
    std::map<int, TexCoord> fan_new;

    for (typename std::map<int, TexCoord>::const_iterator fit =
             fan_pos.begin(); fit != fan_pos.end(); ++fit)
    {
      const int id = fit->first;
      const TexCoord P = fit->second;
      const std::map<int, int>& edges = fan_edge_other[id];
      const std::set<int>& boundary_edges = fan_boundary_edges[id];

      TexCoord S(tex_scalar_t(0), tex_scalar_t(0));
      size_t n = edges.size();
      std::vector<int> boundary_neighbors;

      for (typename std::map<int, int>::const_iterator eit = edges.begin();
           eit != edges.end(); ++eit)
      {
        const int other = eit->second;
        if (other != id)
          S += fan_pos[other];
      }

      for (typename std::set<int>::const_iterator eit = boundary_edges.begin();
           eit != boundary_edges.end(); ++eit)
        boundary_neighbors.push_back(fan_edge_other[id].find(*eit)->second);

      TexCoord value;

      if (!boundary_edges.empty())
      {
        if (boundary_edges.size() == 2)
        {
          // 3/4, 1/8, 1/8 boundary rule.
          value  = P * tex_scalar_t(0.75);
          value += (fan_pos[boundary_neighbors[0]] +
                    fan_pos[boundary_neighbors[1]]) * tex_scalar_t(0.125);
        }
        else
        {
          // Other boundary branches keep their old value.
          value = P;
        }
      }
      else if (n == 0)
      {
        value = P;
      }
      else if (scheme_ == CATMULL_CLARK)
      {
        TexCoord F(tex_scalar_t(0), tex_scalar_t(0));
        const std::set<int>& faces = fan_faces[id];
        for (typename std::set<int>::const_iterator it = faces.begin();
             it != faces.end(); ++it)
          F += face_point_[*it];

        const tex_scalar_t inv = tex_scalar_t(1.0) / tex_scalar_t(n);
        value  = S * (inv * inv);
        value += P * (tex_scalar_t(n - 2) * inv);
        value += F * (inv * inv);
      }
      else
      {
        const weight_t w = loop_weight(n);
        value  = P * tex_scalar_t(w.first);
        value += S * tex_scalar_t(w.second);
      }

      fan_new[id] = value;
    }

    for (HalfedgeIter h_it = _m.halfedges_begin();
         h_it != _m.halfedges_end(); ++h_it)
    {
      const HalfedgeHandle hh = *h_it;
      if (!is_corner[hh.idx()])
        continue;
      const int id = fan[hh.idx()];
      if (id >= 0)
        corner_new_[hh.idx()] = fan_new[id];
    }
  }

public:

  /// New coordinate of the face corner represented by halfedge @p _hh.
  const TexCoord& corner_new(HalfedgeHandle _hh) const
  { return corner_new_[_hh.idx()]; }

  /// New coordinate of the edge point of edge(@p _hh), on its face side.
  const TexCoord& edge_point(HalfedgeHandle _hh) const
  { return edge_point_[_hh.idx()]; }

  /// New coordinate of the face point of @p _fh.
  const TexCoord& face_point(FaceHandle _fh) const
  { return face_point_[_fh.idx()]; }

  /** Write the coordinates of the halfedges touched by an edge split.
   *
   *  On entry @p _heh points from the first old vertex to the midpoint
   *  (and still belongs to its old face), @p _opp still points to the
   *  second old vertex, @p _new_heh points from the midpoint to the second
   *  old vertex and @p _opp_new_heh points to the midpoint.
   */
  void write_split_edge(mesh_t& _m,
                        HalfedgeHandle _heh,
                        HalfedgeHandle _opp,
                        HalfedgeHandle _new_heh,
                        HalfedgeHandle _opp_new_heh) const
  {
    if (_m.face_handle(_heh).is_valid())
    {
      _m.set_texcoord2D(_heh, edge_point_[_heh.idx()]);
      _m.set_texcoord2D(_new_heh, corner_new_[_heh.idx()]);
    }
    if (_m.face_handle(_opp).is_valid())
    {
      _m.set_texcoord2D(_opp_new_heh, edge_point_[_opp.idx()]);
      _m.set_texcoord2D(_opp, corner_new_[_opp.idx()]);
    }
  }

private:

  static int uf_find(std::vector<int>& _p, int _x)
  {
    while (_p[_x] != _x)
    {
      _p[_x] = _p[_p[_x]];
      _x = _p[_x];
    }
    return _x;
  }

  static void uf_unite(std::vector<int>& _p, int _a, int _b)
  {
    _a = uf_find(_p, _a);
    _b = uf_find(_p, _b);
    if (_a != _b)
      _p[_a] = _b;
  }

  weight_t loop_weight(size_t _n) const
  {
    if (weights_ && _n < weights_->size())
      return (*weights_)[_n];

    // Same formula as LoopT::compute_weight.
    if (_n)
    {
      const double inv_v = 1.0 / double(_n);
      const double t = 3.0 + 2.0 * std::cos(2.0 * M_PI * inv_v);
      const double alpha = (40.0 - t * t) / 64.0;
      return weight_t(RealType(1.0 - alpha), RealType(inv_v * alpha));
    }
    return weight_t(RealType(0), RealType(0));
  }

private:

  Scheme                scheme_;
  const weights_t*      weights_;

  std::vector<TexCoord> uv_;
  std::vector<TexCoord> corner_new_;
  std::vector<TexCoord> edge_point_;
  std::vector<TexCoord> face_point_;
};

//=============================================================================
} // END_NS_UNIFORM
} // END_NS_SUBDIVIDER
} // END_NS_OPENMESH
//=============================================================================
#endif // OPENMESH_SUBDIVIDER_UNIFORM_FACEVARYINGTEXCOORDST_HH defined
//=============================================================================
