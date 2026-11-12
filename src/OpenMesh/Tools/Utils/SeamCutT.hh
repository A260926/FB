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
 *  covered by the GNU Lesser General Public License. This exception does    *
 *  not however invalidate any other reasons why the executable file might   *
 *  be covered by the GNU Lesser General Public License.                     *
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
 *   $Revision$                                                              *
 *   $Date$                                                                  *
 *                                                                           *
\*===========================================================================*/


#ifndef OPENMESH_SEAMCUT_HH
#define OPENMESH_SEAMCUT_HH


//== INCLUDES =================================================================


#include <OpenMesh/Core/System/config.h>
#include <vector>
#include <stdexcept>


//== NAMESPACES ===============================================================


namespace OpenMesh {
namespace Utils {


//== CLASS DEFINITION =========================================================


/** \brief Element correspondence produced by cut_seams().
 *
 *  After a call to cut_seams(), each vector maps the idx() of an element of
 *  the result mesh to the idx() of the element of the same kind in the
 *  source mesh it originates from:
 *
 *  - vertex_source[v.idx()]   -> idx() of the source vertex of v
 *  - face_source[f.idx()]     -> idx() of the source face of f
 *  - halfedge_source[h.idx()] -> idx() of the source halfedge with the same
 *                                direction h was derived from.  For a
 *                                boundary halfedge of the result this is the
 *                                source halfedge along the same direction
 *                                (the opposite of the source halfedge that
 *                                keeps its face).
 *
 *  The result mesh is free to renumber its elements; always go through this
 *  map to relate result elements to source elements.  In particular the map
 *  is sufficient to rebuild per-face custom data (e.g. material indices via
 *  face_source) and per-corner data (via halfedge_source) on the result.
 */
struct SeamCutMap
{
  std::vector<int> vertex_source;
  std::vector<int> face_source;
  std::vector<int> halfedge_source;
};


namespace SeamCutDetail {

/// find with path halving for the corner union-find used by cut_seams()
inline int uf_find(std::vector<int>& _parent, int _x)
{
  while (_parent[_x] != _x)
  {
    _parent[_x] = _parent[_parent[_x]];
    _x = _parent[_x];
  }
  return _x;
}

} // namespace SeamCutDetail


//== FUNCTION DEFINITION ======================================================


/** \brief Cut a triangle mesh along a set of seam edges.
 *
 *  Builds a new mesh \p result from \p source in which every edge listed in
 *  \p seams is turned into two genuine boundary edges.  Face corners that
 *  meet across a seam edge no longer share a vertex; corners that meet
 *  across any other edge keep sharing one, so face adjacency is preserved
 *  everywhere except across the seams.  The result stays a connected
 *  halfedge mesh (no triangle soup): every result vertex is incident to a
 *  single connected fan of faces, boundary loops can be walked with
 *  next/prev halfedge navigation, and tools such as MeshCheckerT and the
 *  standard circulators keep working on it.
 *
 *  Each source face appears exactly once in the result, with the same
 *  winding and the same corner order.  Vertices are only split where a seam
 *  passes through them; vertices are never merged, even if their positions
 *  coincide.  An empty seam set yields an equivalent copy of the source
 *  (isolated vertices are kept, once each).
 *
 *  The seam set is undirected: duplicate entries are ignored and the order
 *  of the handles does not matter.  Legal seam configurations are simple
 *  closed loops that do not touch the mesh boundary, and simple chains that
 *  start and end on the mesh boundary without touching it in between.
 *  Loops and chains must not share vertices.  Concretely: every selected
 *  edge must be an interior edge, every interior vertex incident to
 *  selected edges must have seam-degree two, and every boundary vertex
 *  incident to selected edges must have seam-degree one (a chain end).
 *  Out-of-range handles, selected boundary edges, branches, and chains
 *  with a dangling interior end point are rejected with
 *  std::invalid_argument.
 *
 *  The input must be an oriented, 2-manifold triangle mesh without deleted
 *  (garbage-collectable) elements.  \p source is only read.  \p result and
 *  \p source must not be the same object.  On failure \p result and \p map
 *  are left untouched.
 *
 *  Properties: if \p source has vertex normals or 2D halfedge texture
 *  coordinates requested, they are requested on \p result as well and
 *  copied element-wise through the correspondence.  A halfedge texture
 *  coordinate is understood as the value of the face corner at the vertex
 *  the halfedge points to; the two sides of an uncut edge keep their
 *  distinct values, and a new boundary halfedge receives the value of the
 *  same-direction source halfedge (boundary halfedges do not take part in
 *  per-corner rendering, the value is provided for completeness only).
 *  Any other (custom) properties are NOT transferred; use \p map to copy
 *  them yourself.
 *
 *  \param source  mesh to cut (read-only)
 *  \param seams   undirected set of edges to cut open
 *  \param result  receives the cut mesh (previous content is discarded)
 *  \param map     receives the result -> source correspondence
 */
template <class Mesh>
void cut_seams(const Mesh&                              source,
               const std::vector<typename Mesh::EdgeHandle>& seams,
               Mesh&                                    result,
               SeamCutMap&                              map)
{
  typedef typename Mesh::VertexHandle   VertexHandle;
  typedef typename Mesh::HalfedgeHandle HalfedgeHandle;
  typedef typename Mesh::EdgeHandle     EdgeHandle;
  typedef typename Mesh::FaceHandle     FaceHandle;

  if (static_cast<const void*>(&source) == static_cast<const void*>(&result))
    throw std::invalid_argument("cut_seams: source and result must not be the same object");

  const unsigned int nV = source.n_vertices();
  const unsigned int nE = source.n_edges();
  const unsigned int nH = source.n_halfedges();
  const unsigned int nF = source.n_faces();

  //---------------------------------------------------------------------------
  // validate the seam set (nothing below this point may throw on bad input)
  //---------------------------------------------------------------------------

  std::vector<char> is_seam(nE, 0);
  std::vector<int>  seam_degree(nV, 0);

  for (typename std::vector<EdgeHandle>::const_iterator it = seams.begin();
       it != seams.end(); ++it)
  {
    if (!it->is_valid() || it->idx() < 0 ||
        static_cast<unsigned int>(it->idx()) >= nE)
      throw std::invalid_argument("cut_seams: seam edge handle out of range");

    const unsigned int ei = static_cast<unsigned int>(it->idx());
    if (is_seam[ei])
      continue; // duplicates are ignored

    const EdgeHandle eh(ei);
    if (source.is_boundary(eh))
      throw std::invalid_argument("cut_seams: seam edge must be an interior edge");

    is_seam[ei] = 1;
    const HalfedgeHandle h = source.halfedge_handle(eh, 0);
    ++seam_degree[source.from_vertex_handle(h).idx()];
    ++seam_degree[source.to_vertex_handle(h).idx()];
  }

  for (unsigned int vi = 0; vi < nV; ++vi)
  {
    if (seam_degree[vi] == 0)
      continue;
    if (source.is_boundary(VertexHandle(vi)))
    {
      if (seam_degree[vi] != 1)
        throw std::invalid_argument(
          "cut_seams: seam touches a boundary vertex that is not a simple chain end");
    }
    else if (seam_degree[vi] != 2)
    {
      throw std::invalid_argument(
        "cut_seams: interior seam vertices must have degree 2 (no branches, no dangling ends)");
    }
  }

  //---------------------------------------------------------------------------
  // group face corners (halfedges) around each source vertex: two corners
  // belong to the same group iff they are connected through uncut interior
  // edges.  Each group becomes one vertex of the result.
  //---------------------------------------------------------------------------

  std::vector<int> parent(nH);
  for (unsigned int i = 0; i < nH; ++i)
    parent[i] = static_cast<int>(i);

  for (unsigned int ei = 0; ei < nE; ++ei)
  {
    if (is_seam[ei])
      continue;
    const EdgeHandle eh(ei);
    if (source.is_boundary(eh))
      continue;
    const HalfedgeHandle h0 = source.halfedge_handle(eh, 0);
    const HalfedgeHandle h1 = source.halfedge_handle(eh, 1);
    // corners at both endpoints of the uncut interior edge stay together
    const int c00 = h0.idx();
    const int c01 = source.prev_halfedge_handle(h1).idx();
    const int c10 = h1.idx();
    const int c11 = source.prev_halfedge_handle(h0).idx();
    parent[SeamCutDetail::uf_find(parent, c00)] = SeamCutDetail::uf_find(parent, c01);
    parent[SeamCutDetail::uf_find(parent, c10)] = SeamCutDetail::uf_find(parent, c11);
  }

  //---------------------------------------------------------------------------
  // build the cut mesh in a temporary object so that result and map stay
  // untouched if anything goes wrong
  //---------------------------------------------------------------------------

  const bool copy_normals = source.has_vertex_normals();
  const bool copy_tex     = source.has_halfedge_texcoords2D();

  Mesh tmp;
  if (copy_normals) tmp.request_vertex_normals();
  if (copy_tex)     tmp.request_halfedge_texcoords2D();

  SeamCutMap out;

  // one result vertex per corner group
  std::vector<int>  corner_vertex(nH, -1); // indexed by union-find root
  std::vector<char> touched(nV, 0);
  for (unsigned int hi = 0; hi < nH; ++hi)
  {
    const HalfedgeHandle hh(hi);
    if (!source.face_handle(hh).is_valid())
      continue;
    const VertexHandle sv = source.to_vertex_handle(hh);
    touched[sv.idx()] = 1;
    const int root = SeamCutDetail::uf_find(parent, static_cast<int>(hi));
    if (corner_vertex[root] < 0)
    {
      const VertexHandle nv = tmp.add_vertex(source.point(sv));
      corner_vertex[root] = nv.idx();
      out.vertex_source.push_back(sv.idx());
    }
  }

  // isolated source vertices are kept, once each
  for (unsigned int vi = 0; vi < nV; ++vi)
    if (!touched[vi])
    {
      tmp.add_vertex(source.point(VertexHandle(vi)));
      out.vertex_source.push_back(static_cast<int>(vi));
    }

  // one result face per source face, same corner order and winding
  for (unsigned int fi = 0; fi < nF; ++fi)
  {
    const FaceHandle fh(fi);
    const HalfedgeHandle a = source.halfedge_handle(fh);
    const HalfedgeHandle b = source.next_halfedge_handle(a);
    const HalfedgeHandle c = source.next_halfedge_handle(b);
    const VertexHandle va(corner_vertex[SeamCutDetail::uf_find(parent, a.idx())]);
    const VertexHandle vb(corner_vertex[SeamCutDetail::uf_find(parent, b.idx())]);
    const VertexHandle vc(corner_vertex[SeamCutDetail::uf_find(parent, c.idx())]);
    const FaceHandle nf = tmp.add_face(va, vb, vc);
    if (!nf.is_valid())
      throw std::runtime_error("cut_seams: failed to add face (input not 2-manifold?)");
    out.face_source.push_back(static_cast<int>(fi));
  }

  // halfedge correspondence: interior halfedges via the faces they belong to
  out.halfedge_source.assign(tmp.n_halfedges(), -1);
  for (unsigned int fi = 0; fi < nF; ++fi)
  {
    const FaceHandle fh(fi);
    HalfedgeHandle h = source.halfedge_handle(fh);
    for (int k = 0; k < 3; ++k)
    {
      const HalfedgeHandle p = source.prev_halfedge_handle(h);
      const VertexHandle from_v(corner_vertex[SeamCutDetail::uf_find(parent, p.idx())]);
      const VertexHandle to_v(corner_vertex[SeamCutDetail::uf_find(parent, h.idx())]);
      const HalfedgeHandle rh = tmp.find_halfedge(from_v, to_v);
      out.halfedge_source[rh.idx()] = h.idx();
      h = source.next_halfedge_handle(h);
    }
  }
  // boundary halfedges map to the same-direction source halfedge
  for (unsigned int hi = 0; hi < tmp.n_halfedges(); ++hi)
  {
    const HalfedgeHandle rh(hi);
    if (tmp.face_handle(rh).is_valid())
      continue;
    const HalfedgeHandle opp = tmp.opposite_halfedge_handle(rh);
    const HalfedgeHandle sh(out.halfedge_source[opp.idx()]);
    out.halfedge_source[hi] = source.opposite_halfedge_handle(sh).idx();
  }

  //---------------------------------------------------------------------------
  // copy the standard properties that carry per-element meaning
  //---------------------------------------------------------------------------

  if (copy_normals)
    for (unsigned int vi = 0; vi < tmp.n_vertices(); ++vi)
      tmp.set_normal(VertexHandle(vi),
                     source.normal(VertexHandle(out.vertex_source[vi])));

  if (copy_tex)
    for (unsigned int hi = 0; hi < tmp.n_halfedges(); ++hi)
      tmp.set_texcoord2D(HalfedgeHandle(hi),
                         source.texcoord2D(HalfedgeHandle(out.halfedge_source[hi])));

  //---------------------------------------------------------------------------
  // commit: move the temporary mesh into result (handles stay identical,
  // the temporary mesh contains no deleted elements)
  //---------------------------------------------------------------------------

  result.clear();
  if (copy_normals) result.request_vertex_normals();
  if (copy_tex)     result.request_halfedge_texcoords2D();

  for (unsigned int vi = 0; vi < tmp.n_vertices(); ++vi)
  {
    const VertexHandle vh(vi);
    const VertexHandle nv = result.add_vertex(tmp.point(vh));
    if (copy_normals) result.set_normal(nv, tmp.normal(vh));
  }
  for (unsigned int fi = 0; fi < nF; ++fi)
  {
    // exactly the add_face() calls used for tmp, in the same order, so the
    // kernel assigns identical edge and halfedge indices
    const FaceHandle fh(fi);
    const HalfedgeHandle a = source.halfedge_handle(fh);
    const HalfedgeHandle b = source.next_halfedge_handle(a);
    const HalfedgeHandle c = source.next_halfedge_handle(b);
    result.add_face(VertexHandle(corner_vertex[SeamCutDetail::uf_find(parent, a.idx())]),
                    VertexHandle(corner_vertex[SeamCutDetail::uf_find(parent, b.idx())]),
                    VertexHandle(corner_vertex[SeamCutDetail::uf_find(parent, c.idx())]));
  }
  if (copy_tex)
    for (unsigned int hi = 0; hi < tmp.n_halfedges(); ++hi)
      result.set_texcoord2D(HalfedgeHandle(hi), tmp.texcoord2D(HalfedgeHandle(hi)));

  map.vertex_source.swap(out.vertex_source);
  map.face_source.swap(out.face_source);
  map.halfedge_source.swap(out.halfedge_source);
}


//=============================================================================
} // namespace Utils
} // namespace OpenMesh
//=============================================================================
#endif // OPENMESH_SEAMCUT_HH defined
//=============================================================================
