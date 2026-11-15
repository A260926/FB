/**
 * Indexed triangle mesh generation from a successful PatchEditor snapshot.
 *
 * Use of this source code is governed by a BSD-style license that can be found in
 * the LICENSE file.
 */

#ifndef TINYNURBS_PATCH_MESH_H
#define TINYNURBS_PATCH_MESH_H

#include "glm/glm.hpp"
#include "patch_editor.h"
#include <cstddef>
#include <string>
#include <vector>

namespace tinynurbs
{

/**
 * One triangle corner.
 *
 * Several corners of different patches may reference the same entry of
 * PatchMesh::positions (a welded seam position), while each corner keeps its
 * own patch index, original parameter pair and analytic normal.
 */
struct PatchMeshCorner
{
    int position = 0;        ///< Index into PatchMesh::positions
    int patch = 0;           ///< Source patch index
    double u = 0.0;          ///< Parameter in the source patch's original domain
    double v = 0.0;          ///< Parameter in the source patch's original domain
    glm::dvec3 normal = glm::dvec3(0.0); ///< Analytic unit normal at (u, v)
};

/** One triangle of the output mesh. */
struct PatchMeshTriangle
{
    PatchMeshCorner corners[3];
};

/**
 * Indexed triangle mesh.
 *
 * `positions` contains each welded position once.  Triangles store corner
 * attributes separately so a welded seam position can carry the original UV
 * and analytic normal of each adjacent patch.
 */
struct PatchMesh
{
    std::vector<glm::dvec3> positions;
    std::vector<PatchMeshTriangle> triangles;

    std::size_t vertexCount() const { return positions.size(); }
    std::size_t triangleCount() const { return triangles.size(); }
};

// Convenience aliases.
using IndexedPatchMesh = PatchMesh;
using MeshTriangle = PatchMeshTriangle;
using MeshCorner = PatchMeshCorner;

/**
 * Build an indexed triangle mesh from a successful editor snapshot.
 *
 * Every original knot cell of every patch is subdivided into a fixed 4x4 grid.
 * Along declared seams the sample stations of both sides are merged and both
 * patches are sampled at the merged stations; the resulting positions are
 * welded (shared index).  Undeclared coincident patches are never welded.
 *
 * If the snapshot carries the original (pre-refinement) knot vectors they are
 * used to define the cells; otherwise the knot vectors of `snapshot.patches`
 * are used.
 */
PatchMesh buildPatchMesh(const PatchEditorSnapshot &snapshot);

/**
 * Write a mesh to a Wavefront OBJ file.
 * @return true on success, false if the file could not be created.
 */
bool writePatchMeshOBJ(const PatchMesh &mesh, const std::string &path);

/**
 * Same as buildPatchMesh(snapshot), followed by writePatchMeshOBJ().
 */
bool writePatchMeshOBJ(const PatchEditorSnapshot &snapshot, const std::string &path);

} // namespace tinynurbs

#endif // TINYNURBS_PATCH_MESH_H
