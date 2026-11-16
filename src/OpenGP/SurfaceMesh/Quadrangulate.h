#pragma once
#include <OpenGP/SurfaceMesh/SurfaceMesh.h>
#include <vector>
#include <stdexcept>

//=============================================================================
namespace OpenGP {
//=============================================================================

/// Merge triangle pairs inside \p region into editable quad faces.
///
/// The candidate graph has one node per selected triangle and one edge per
/// pair of adjacent triangles that pass every geometric and attribute test
/// below. A maximum-cardinality matching of this general (possibly
/// non-bipartite, e.g. containing odd cycles) graph is computed in
/// polynomial time with Edmonds' blossom algorithm, so the number of merges
/// is globally maximal; any one optimal matching is acceptable. Greedy,
/// edge-length-sorted, bipartite-only or enumeration-based schemes are not
/// used. The matching is fully determined before the mesh is touched, then
/// applied with SurfaceMesh::join_triangles(); unmatched triangles remain.
///
/// A pair of neighboring triangles is a candidate only when they share
/// exactly one edge and bound four distinct vertices forming a strictly
/// convex, planar quadrilateral. With L the longest distance among the four
/// points, the tests are:
/// - both triangle area-vector magnitudes exceed tolerance * L * L;
/// - the dot product of the two unit face normals is positive;
/// - the absolute mixed product of the four points is at most
///   tolerance * L * L * L (coplanarity);
/// - every outer-ring adjacent-edge cross product, projected onto the
///   normalized sum of the two unit normals, exceeds tolerance * L * L
///   (strict convexity, correct winding).
///
/// Attribute constraints, when the corresponding property exists:
/// - halfedge properties "h:texcoord" and "h:normal" (both Vec3) store the
///   value of the face corner at the halfedge's to-vertex. At each endpoint
///   of the shared diagonal the two incident corner values must be equal
///   component-wise and finite, otherwise the edge is rejected (a texture
///   seam or a hard normal break). Values are never averaged or recomputed;
/// - an integer face property "f:material" must be equal on the two faces.
/// Meshes without any of these properties work unchanged.
///
/// @param mesh oriented manifold triangle/polygon mesh
/// @param region triangle faces to consider; duplicate handles are ignored.
///        An empty region (with a valid tolerance) leaves the mesh untouched.
/// @param tolerance positive finite geometric tolerance normalized by L
/// @return the old Edge handles of the diagonals removed by this call,
///         sorted by ascending handle index; unmatched triangles stay put
///
/// @throws std::invalid_argument before any mesh modification when:
///   - tolerance is not a positive finite number;
///   - a region handle is invalid, out of range or deleted;
///   - a selected face is not a triangle;
///   - a selected triangle has non-finite vertex coordinates.
std::vector<SurfaceMesh::Edge>
quadrangulate(SurfaceMesh& mesh,
              const std::vector<SurfaceMesh::Face>& region,
              Scalar tolerance = Scalar(1e-5));

//=============================================================================
} // namespace OpenGP
//=============================================================================

// Header only support
#ifdef HEADERONLY
    #include "Quadrangulate.cpp"
#endif
