// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#pragma once

#include "pmp/surface_mesh.h"

namespace pmp {

//! \addtogroup algorithms
//! @{

//! \brief Split the edge containing halfedge \p h at parameter \p t.
//! \details Inserts a single new vertex at the linear interpolation
//! \f$(1-t)\,\mathbf{v}_0 + t\,\mathbf{v}_1\f$ between the start and the end
//! vertex of \p h, where \f$0 < t < 1\f$. The real topology change is delegated
//! to SurfaceMesh::split(Edge, Vertex): an interior edge is subdivided on both
//! sides, a boundary edge only on its incident side. The faces adjacent to the
//! edge have to be triangles.
//!
//! The child faces inherit the smoothing group of their parent. Existing
//! corners keep their texture coordinates, explicit flags and normals. The two
//! new corners at the inserted vertex are built independently per side:
//!
//!  * the texture coordinate is the interpolation at the same parameter \p t
//!    between the two corner texture coordinates of that side (the two sides
//!    are never mixed);
//!  * an explicit normal results only when both end corners of the side are
//!    explicit; their interpolated normal is normalized, otherwise the corner
//!    is automatic;
//!  * after the split all automatic corner normals are refreshed with
//!    update_corner_normals().
//!
//! Other custom properties follow the default SurfaceMesh::split() behavior.
//!
//! \throw InvalidInputException if \p h is invalid or deleted, if \p t is not
//! strictly between 0 and 1, if an adjacent face is not a triangle, or if an
//! interpolated explicit normal would be the zero vector. In any failure case
//! the mesh is left unchanged.
//! \return the newly inserted vertex.
Vertex split_edge_with_corners(SurfaceMesh& mesh, Halfedge h, Scalar t);

//! @}

} // namespace pmp
