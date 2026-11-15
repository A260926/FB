// Copyright 2011-2020 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#pragma once

#include "pmp/surface_mesh.h"

namespace pmp {

//! \brief Compute vertex normals for the whole \p mesh.
//! \details Calls vertex_normal() for each vertex and adds a new
//! vertex property of type Normal named "v:normal".
//! \note This algorithm works on general polygon meshes.
//! \ingroup algorithms
void vertex_normals(SurfaceMesh& mesh);

//! \brief Compute face normals for the whole \p mesh.
//! \details Calls face_normal() for each face and adds a new face
//! property of type Normal named "f:normal".
//! \note This algorithm works on general polygon meshes.
//! \ingroup algorithms
void face_normals(SurfaceMesh& mesh);

//! \brief Compute the normal vector of vertex \p v.
//! \note This algorithm works on general polygon meshes.
//! \ingroup algorithms
Normal vertex_normal(const SurfaceMesh& mesh, Vertex v);

//! \brief Compute the normal vector of face \p f.
//! \details Normal is computed as (normalized) sum of per-corner
//! cross products of the two incident edges. This corresponds to
//! the normalized vector area in \cite alexa_2011_laplace
//! \note This algorithm works on general polygon meshes.
//! \ingroup algorithms
Normal face_normal(const SurfaceMesh& mesh, Face f);

//! \brief Compute the normal vector of the polygon corner specified by the
//! target vertex of halfedge \p h.
//! \details Averages incident corner normals if they are within crease_angle
//! of the face normal. \p crease_angle is in radians, not degrees.
//! \note This algorithm works on general polygon meshes.
//! \ingroup algorithms
Normal corner_normal(const SurfaceMesh& mesh, Halfedge h, Scalar crease_angle);

//! \brief Recompute the automatic per-corner normals of \p mesh.
//! \details Works on the halfedge properties \c "h:normal" (type Normal) and
//! \c "h:normal_explicit" (type bool). Corners whose explicit flag is set keep
//! their stored normal. Every other corner is assigned the normal of the
//! smoothing fan it belongs to.
//!
//! A smoothing fan at a vertex consists of the incident faces that are
//! connected through shared edges and share the same positive smoothing group
//! (face property \c "f:smoothing_group"). Faces in group 0 are never averaged
//! with any other face. A fan normal is the normalized, interior-angle
//! weighted sum of the unit geometric face normals; if the sum vanishes the
//! corner normal is the zero vector. UV seams do not split a fan, and a fan
//! never reaches across a boundary edge or a face of a different smoothing
//! group.
//!
//! If the halfedge normal properties do not exist yet they are created, so the
//! function can be called on meshes read without normals. The existing
//! vertex_normal() and corner_normal() conventions are left untouched.
//! \ingroup algorithms
void update_corner_normals(SurfaceMesh& mesh);

} // namespace pmp
