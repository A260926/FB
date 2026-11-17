/********************************************************************************
*  This file is part of CinoLib                                                 *
*  Copyright(C) 2024: CinoLib contributors                                      *
*                                                                               *
*  The MIT License                                                              *
*                                                                               *
*  Permission is hereby granted, free of charge, to any person obtaining a      *
*  copy of this software and associated documentation files (the "Software"),   *
*  to deal in the Software without restriction, including without limitation    *
*  the rights to use, copy, modify, merge, publish, distribute, sublicense,     *
*  and/or sell copies of the Software, and to permit persons to whom the        *
*  Software is furnished to do so, subject to the following conditions:         *
*                                                                               *
*  The above copyright notice and this permission notice shall be included in   *
*  all copies or substantial portions of the Software.                          *
*                                                                               *
*  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR   *
*  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,     *
*  FITNESS FOR A PARTICULAR PURPOSE AND NON INFRINGEMENT. IN NO EVENT SHALL THE *
*  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER       *
*  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING      *
*  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS *
*  IN THE SOFTWARE.                                                             *
*********************************************************************************/
#ifndef CINO_SURFACE_CORRESPONDENCE_H
#define CINO_SURFACE_CORRESPONDENCE_H

#include <array>
#include <vector>
#include <cinolib/cino_inline.h>
#include <cinolib/geometry/vec_mat.h>
#include <cinolib/meshes/trimesh.h>

namespace cinolib
{

/* A single piece of the piecewise affine bijection between the source surface
 * (the mesh given at construction time) and the current surface (the working
 * Trimesh copy that gets coarsened by edge collapses).
 *
 * A patch lives on one source triangle and one current triangle. Both sides
 * have strictly positive area. The three source-side barycentric triples are
 * the images, in sourceFace, of the three vertices of currentFace (in the
 * vertex order of currentFace). The three current-side triples are the
 * images, in currentFace, of the three vertices of sourceFace (in the vertex
 * order of sourceFace). Both triples of points form an inner, positively
 * oriented triangle, so the restriction of the correspondence to the patch is
 * affine and invertible in both directions.
 *
 * The patches form an exact, overlap/gap free partition of every source face
 * and of every current face. Shared patch edges agree geometrically, which is
 * what makes the correspondence well defined on mesh edges and vertices.
*/
struct CorrespondencePatch
{
    uint sourceFace = 0;
    uint currentFace = 0;

    // sourceBary[i] = barycentric coords (w.r.t. sourceFace) of currentFace vertex i
    std::array<std::array<double,3>,3> sourceBary{};
    // currentBary[i] = barycentric coords (w.r.t. currentFace) of sourceFace vertex i
    std::array<std::array<double,3>,3> currentBary{};
};

// Result of a point query: the dense id of a face on the target surface plus
// the three barycentric weights ordered as that face's vertices.
struct SurfaceQuery
{
    uint face = 0;
    std::array<double,3> bary{0.0, 0.0, 0.0};
};

// Local rewrite report emitted by one successful edge collapse. Ids refer to
// the working mesh, before (old*) and after (new*) the rewrite. The session
// consumes these reports to keep both index directions and the barycentric
// vertex orders in sync without rescanning the mesh.
struct CollapseRewriteReport
{
    // edge that was contracted
    uint edge = 0;
    // endpoint kept (moved to edge_sample_at(edge, lambda)) and endpoint removed
    uint keptVert = 0;
    uint removedVert = 0;

    // faces incident to the contracted edge: they disappear
    std::vector<uint> removedFaces;
    // every other face incident to removedVert: old face id -> freshly created face id
    std::vector<std::pair<uint,uint>> rebornFaces;
    // dense slot compaction: old face id -> new face id, for faces moved by
    // swap-with-last removal (identity rewrites are not listed)
    std::vector<std::pair<uint,uint>> faceIdSwaps;
    // dense slot compaction: old vertex id -> new vertex id
    std::vector<std::pair<uint,uint>> vertIdSwaps;

    bool empty() const { return removedFaces.empty() && rebornFaces.empty() &&
                                faceIdSwaps.empty() && vertIdSwaps.empty(); }
};

/* SurfaceCorrespondence owns an independent Trimesh copy of the source
 * surface and maintains a full surface-to-surface correspondence across a
 * sequence of legal edge collapses. The source surface is never resampled:
 * every correspondence patch always points back to the construction-time
 * mesh, whose face numbering stays fixed for the whole session.
 *
 * Input assumptions (violating them makes the construction throw
 * std::invalid_argument): finite coordinates, oriented, non degenerate,
 * manifold triangle surface, possibly with boundary and multiple connected
 * components. Edges incident to a boundary vertex are never collapsed.
*/
class SurfaceCorrespondence
{
    public:

        explicit SurfaceCorrespondence(const Trimesh<> & source);
        ~SurfaceCorrespondence();

        // the current (possibly coarsened) working mesh, read only
        const Trimesh<> & mesh() const;

        // number of source faces (fixed at construction time) and current faces
        uint num_source_faces() const;
        uint num_current_faces() const;

        // attempt an edge collapse on the working mesh. Returns false (and
        // leaves the mesh, the patches and all queries unchanged) when the
        // edge is illegal, the link condition / geometric admissibility fails
        // or the local correspondence cannot be built without flips.
        bool collapse(uint eid, double lambda);

        // read only snapshot of the correspondence patches
        const std::vector<CorrespondencePatch> & patches() const;

        // point queries. Barycentric weights must be finite, non negative and
        // sum to 1 within 1e-12. Points on edges/vertices are accepted; when a
        // point belongs to several target faces the one with the smallest id
        // is returned. Illegal arguments throw std::invalid_argument.
        SurfaceQuery toCurrent(uint sourceFace, const std::array<double,3> & bary) const;
        SurfaceQuery toSource (uint currentFace, const std::array<double,3> & bary) const;

        // 3D position of a point on the source surface
        vec3d source_point(uint sourceFace, const std::array<double,3> & bary) const;

        // rewrite report of the last successful collapse (undefined before any)
        const CollapseRewriteReport & last_report() const;

    private:

        struct Impl;
        Impl * impl_;

        // non copyable: it owns a working session
        SurfaceCorrespondence(const SurfaceCorrespondence &) = delete;
        SurfaceCorrespondence & operator=(const SurfaceCorrespondence &) = delete;
};

}

#endif // CINO_SURFACE_CORRESPONDENCE_H
