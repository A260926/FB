/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------

Copyright (c) 2006-2026, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the
following conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
  contributors may be used to endorse or promote products
  derived from this software without specific prior
  written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

----------------------------------------------------------------------
*/

/** @file ResolveTJunctionsProcess.h
 *  @brief Reusable production logic that resolves T-junctions inside a
 *    single aiMesh before it is triangulated.
 *
 *  The step finds vertices whose position projects strictly inside an
 *  indexed edge (projection parameter in (0,1), perpendicular distance
 *  within epsilon) and inserts such points into every face edge that
 *  references the same pair of vertex indices, keeping the resulting
 *  boundary sub-segments as real edges of the output triangles.
 *
 *  @note Only base-shape edge chains are repaired. Edges that become
 *  collinear only after an arbitrary skinning transformation are not
 *  promised to be detected.
 *  @note Processing is strictly per aiMesh: vertices are never paired
 *  across meshes or through node transformations.
 */
#ifndef AI_RESOLVETJUNCTIONSPROCESS_H_INC
#define AI_RESOLVETJUNCTIONSPROCESS_H_INC

#include <assimp/mesh.h>
#include <assimp/vector3.h>

#include <map>
#include <vector>

namespace Assimp {

// ---------------------------------------------------------------------------
/** Shared, mesh-local logic for T-junction resolution.
 *
 *  The class is deliberately independent of the post-processing pipeline:
 *  it neither registers as a post step nor reads importer properties. The
 *  TriangulateProcess owns the configuration and drives the conversion.
 */
class ASSIMP_API ResolveTJunctionsProcess {
public:
    /// A logical split point on an indexed edge. Verbose-format meshes may
    /// contain several vertex indices at the same physical position; they
    /// are merged into one split point and listed in #equivalents.
    struct SplitPoint {
        unsigned int vertex; ///< primary candidate index
        ai_real parameter; ///< projection parameter in (0,1)
        std::vector<unsigned int> equivalents; ///< all indices at this point
    };

    /// The ordered split points (plus the two endpoint indices) collected
    /// for one undirected indexed edge.
    struct EdgeSplit {
        unsigned int endpointA;
        unsigned int endpointB;
        std::vector<SplitPoint> points; // sorted by parameter, ascending
    };

    ResolveTJunctionsProcess() = default;
    ~ResolveTJunctionsProcess() = default;

    // -------------------------------------------------------------------
    /** Validate the epsilon configuration.
     *  @return true if eps is finite and strictly positive.
     */
    static bool IsValidEpsilon(ai_real eps);

    // -------------------------------------------------------------------
    /** Collect the T-junction topology of a mesh.
     *
     *  Only faces with at least three indices participate. Candidate
     *  points are taken from the vertices referenced by those faces.
     *  The mesh is not modified.
     *
     *  @param mesh  Mesh to inspect.
     *  @param eps   Maximum perpendicular distance to an edge.
     *  @return Map from the canonical (low,high) vertex index pair to
     *    the ordered list of split points on that edge. Edges without
     *    split points are not stored.
     */
    static std::map<std::pair<unsigned int, unsigned int>, EdgeSplit>
    CollectEdgeSplits(const aiMesh *mesh, ai_real eps);

    // -------------------------------------------------------------------
    /** Rewrite a mesh so that all collected T-junction points become
     *  real boundary vertices, and triangulate every affected face.
     *
     *  Faces with fewer than three indices are preserved untouched. The
     *  original vertex order, existing attribute values, face materials
     *  and node references stay valid; existing vertices are neither
     *  merged nor moved. Running the method on an already resolved mesh
     *  leaves it unchanged.
     *
     *  @param mesh   Mesh to rewrite in place.
     *  @param splits Topology as produced by CollectEdgeSplits().
     *  @return true if the mesh was modified.
     */
    static bool ApplyToMesh(aiMesh *mesh,
            const std::map<std::pair<unsigned int, unsigned int>, EdgeSplit> &splits);

    // -------------------------------------------------------------------
    /** One-shot convenience: collect and apply.
     *  @return true if the mesh was modified.
     */
    static bool ResolveMesh(aiMesh *mesh, ai_real eps);

private:
    // Per-edge table used while rewriting one mesh.
    struct EdgeInfo {
        unsigned int low;
        unsigned int high;
        // Logical interior split points ordered from low to high.
        std::vector<SplitPoint> splits;
        // Final vertex index for every node of the split edge, ordered
        // from `low` (parameter 0) to `high` (parameter 1). Interior
        // slots start as the primary candidate id and are resolved to a
        // reused or freshly appended vertex index.
        std::vector<unsigned int> nodesLowToHigh;
    };

    // Build the new vertex for an edge split and extend every stream.
    static unsigned int AppendInterpolatedVertex(aiMesh *mesh, const EdgeInfo &edge,
            ai_real t);

    // Interpolation helpers for the individual attribute streams.
    static aiVector3D LerpNormalized(const aiVector3D &a, const aiVector3D &b, ai_real t);
    static void InterpolateBoneWeights(aiMesh *mesh, unsigned int newVertex,
            unsigned int vertexA, unsigned int vertexB, ai_real t);
    static void InterpolateAnimMesh(aiAnimMesh *anim, unsigned int newVertex,
            unsigned int vertexA, unsigned int vertexB, ai_real t);

    // Triangulation of one simple planar polygon given as an ordered ring
    // of vertex indices; output triangles keep the input winding.
    static std::vector<aiFace> TriangulateRing(const aiMesh *mesh,
            const std::vector<unsigned int> &ring);
};

} // end of namespace Assimp

#endif // AI_RESOLVETJUNCTIONSPROCESS_H_INC
