/* ----------------------------------------------------------------------- *
 * This file is part of GEL, http://www.imm.dtu.dk/GEL
 * Copyright (C) the authors and DTU Informatics
 * For license and list of authors, see ../../doc/intro.pdf
 * ----------------------------------------------------------------------- */

/**
 * @file AttributedManifold.h
 * @brief A Manifold paired with per-halfedge ("per face corner") texture
 * coordinates, explicit normals and validity masks.
 *
 * A face corner is the angle of the halfedge that points to a vertex while
 * belonging to a face. Halfedges without a face (boundary hole halfedges) are
 * not rendering corners: their attribute slots are never read when writing or
 * comparing corner data and their mask bytes stay zero.
 */

#ifndef HMESH_ATTRIBUTED_MANIFOLD_H
#define HMESH_ATTRIBUTED_MANIFOLD_H

#include <GEL/CGLA/Vec.h>
#include <GEL/HMesh/AttributeVector.h>
#include <GEL/HMesh/Manifold.h>

namespace HMesh
{
    /// Mesh together with halfedge indexed texture coordinates and normals.
    /// The mask vectors mark, per halfedge, whether the corresponding
    /// attribute is defined. Only halfedges belonging to a face are corners.
    class AttributedManifold
    {
    public:
        /// The connectivity and geometry.
        Manifold mesh;

        /// Per-halfedge texture coordinates (only meaningful together with has_texcoord).
        HalfEdgeAttributeVector<CGLA::Vec2d> texcoords;

        /// Per-halfedge explicit normals (only meaningful together with has_normal).
        HalfEdgeAttributeVector<CGLA::Vec3d> normals;

        /// Per-halfedge mask: nonzero when texcoords holds a defined value.
        HalfEdgeAttributeVector<unsigned char> has_texcoord;

        /// Per-halfedge mask: nonzero when normals holds a defined value.
        HalfEdgeAttributeVector<unsigned char> has_normal;

        /// Remove unused slots from the mesh and remap all four external
        /// attribute vectors through the actual IDRemap produced by the
        /// connectivity kernel cleanup.
        void cleanup();

        /// Remove every entity and attribute.
        void clear();
    };
}

#endif // HMESH_ATTRIBUTED_MANIFOLD_H
