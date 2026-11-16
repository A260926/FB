/* ----------------------------------------------------------------------- *
 * This file is part of GEL, http://www.imm.dtu.dk/GEL
 * Copyright (C) the authors and DTU Informatics
 * For license and list of authors, see ../../doc/intro.pdf
 * ----------------------------------------------------------------------- */

/**
 * @file HolePatch.h
 * @brief Fair filling of a simple convex boundary hole.
 *
 * The hole is first capped with close_hole and the cap polygon is refined
 * with repeated barycentric triangle splits (split_face_by_vertex). The
 * interior vertex positions are then chosen as the minimizer of the
 * discrete Dirichlet energy
 *
 *   E = sum_i || p_i - (1/deg(i)) * sum_{j adj i} p_j ||^2
 *
 * where the rows cover both the new interior vertices and the original
 * boundary vertices. The boundary rows use the real adjacency of the
 * patched mesh, so the geometry outside the hole participates in the
 * solution; only interior positions are free variables.
 */

#ifndef HMESH_HOLE_PATCH_H
#define HMESH_HOLE_PATCH_H

#include <vector>

#include <GEL/HMesh/AttributedManifold.h>
#include <GEL/HMesh/ItemID.h>

namespace HMesh
{
    /// Outcome of fill_fair_hole: the surviving elements introduced by the
    /// operation together with the achieved energy and gradient residual.
    struct HolePatchResult
    {
        /// New vertices that survive in the mesh, in creation order.
        std::vector<VertexID> vertices;

        /// New faces that survive in the mesh (all triangles), in the order
        /// produced by the final refinement round.
        std::vector<FaceID> faces;

        /// The actual minimized energy E computed on the patched adjacency.
        double energy = 0.0;

        /// Infinity norm of the true energy gradient with respect to the
        /// coordinates of all new vertices.
        double residual = 0.0;
    };

    /** Fill the boundary hole containing the face-less halfedge @p boundary
        with a fairly smoothed triangle patch.

        The boundary must be a complete simple loop of at least three
        face-less halfedges on an oriented two-manifold triangle mesh.
        Projected onto the plane defined by the boundary Newell normal the
        hole must be strictly convex, all coordinates must be finite, and
        original face areas and edge lengths must be positive.
        @p refinement_rounds must be between 1 and 3; the finished patch
        contains n * 3^(rounds-1) triangles for a hole of n edges.

        Old vertex positions, surviving old element IDs and existing corner
        data are preserved. Invalid input throws std::invalid_argument; a
        numerical failure (non-convergence, unfolding or a degenerate
        triangle) throws std::runtime_error. In either error case the entire
        AttributedManifold, including the external attributes, is restored
        to its state before the call. */
    HolePatchResult fill_fair_hole(AttributedManifold& am, HalfEdgeID boundary,
                                   unsigned refinement_rounds = 2);
}

#endif // HMESH_HOLE_PATCH_H
