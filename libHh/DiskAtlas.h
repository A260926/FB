// -*- C++ -*-  Copyright (c) Microsoft Corporation; see LICENSE.
#ifndef MESH_PROCESSING_LIBHH_DISKATLAS_H_
#define MESH_PROCESSING_LIBHH_DISKATLAS_H_

#include <utility>
#include <vector>

#include "libHh/GMesh.h"

namespace hh {

// Cut a closed orientable triangle surface (genus >= 1) into a single topological disk, and lay out that disk as a
// texture atlas with a unit-circle boundary and harmonic (uniform-weight) interior.
//
// The cut graph is derived from the actual connectivity of `source`:
//   * T  "primal_tree_edges": a shortest-path tree of the primal vertex graph, rooted at `root_vertex_id`, using the
//        three-dimensional edge lengths.
//   * D  "dual_tree_edges":   a maximum-total-length spanning tree of the face-dual graph restricted to the dual
//        edges that do not correspond to edges of T.  Each entry is the primal edge dual to one tree edge.
//   * L  "generator_edges":   the remaining edges, E \ (T union D); there are exactly 2 * genus of them.
//   * "cut_edges":            the 2-core obtained by repeatedly deleting degree-one edges from T union L.
// All four lists contain pairs of original vertex ids in ascending order; each list is sorted and free of
// duplicates.  The edges are undirected and are expressed through their endpoints so that callers can trace each
// cut in the returned mesh.
//
// The result is a real GMesh: every source face appears exactly once with its original vertex rotation and point
// positions, every non-cut edge is shared by its two faces, every cut edge yields two boundary edges, and a cut-graph
// branch vertex is duplicated once per incident face sector.  The result has one face-connected component, one simple
// boundary loop, and V - E + F == 1.  No faces are added (no cap is created), deleted, or moved by projection.
//
// Element provenance is recorded in the GMesh strings:
//   * each vertex has "source_vertex=<original vertex id>";
//   * each face has "source_face=<original face id>";
//   * each edge has "source_edge=(<a> <b>)" with ascending original vertex ids.
// Original flags and strings (colors, explicit corner normals, other keys) are carried over; any "uv" key in the
// source is dropped.  A vertex-level normal/color fallback is copied to every split copy, and corner-level strings
// stay attached to their own original face corner.
//
// Boundary texture coordinates are placed on the unit circle at angles proportional to cumulative 3D boundary edge
// length, starting at the boundary copy whose source_vertex id is smallest (ties are broken arbitrarily).  Interior
// coordinates are the average of all distinct neighboring vertex coordinates, solved with a sparse iterative method
// (no dense matrix is formed).  The reported `residual` is the largest absolute coordinate residual of those averaging
// equations over the final stored (float) coordinates, and is 0 when the disk has no interior vertex.  The disk's two
// possible boundary orientations are both allowed; the one making every face's signed UV area positive is chosen.
//
// Throws std::invalid_argument if the source is not a single connected closed orientable triangle 2-manifold of
// genus >= 1 with finite points and positive face areas / edge lengths, if root_vertex_id is not a valid vertex id, or
// if `source` and `result` are the same object.  Throws std::runtime_error if the cut/UV solve fails (for example, if
// the dual spanning tree cannot be built or the texture layout would fold over).  In both failure cases, `result`
// retains its previous contents.  `source` is never modified.
struct DiskAtlasInfo {
  std::vector<std::pair<int, int>> primal_tree_edges;
  std::vector<std::pair<int, int>> dual_tree_edges;
  std::vector<std::pair<int, int>> generator_edges;
  std::vector<std::pair<int, int>> cut_edges;
  double residual{0};
};

[[nodiscard]] DiskAtlasInfo make_disk_atlas(const GMesh& source, int root_vertex_id, GMesh& result);

}  // namespace hh

#endif  // MESH_PROCESSING_LIBHH_DISKATLAS_H_
