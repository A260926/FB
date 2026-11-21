#pragma once
/*===========================================================================*\
 *                                                                           *
 *                            OpenVolumeMesh                                 *
 *        Copyright (C) 2011 by Computer Graphics Group, RWTH Aachen         *
 *                        www.openvolumemesh.org                             *
 *                                                                           *
 *---------------------------------------------------------------------------*
 *  This file is part of OpenVolumeMesh.                                     *
 *                                                                           *
 *  OpenVolumeMesh is free software: you can redistribute it and/or modify   *
 *  it under the terms of the GNU Lesser General Public License as           *
 *  published by the Free Software Foundation, either version 3 of the       *
 *  License, or (at your option) any later version with the                  *
 *  following exceptions:                                                    *
 *                                                                           *
 *  If other files instantiate templates or use macros                       *
 *  or inline functions from this file, or you compile this file and         *
 *  link it with other files to produce an executable, this file does        *
 *  not by itself cause the resulting executable to be covered by the        *
 *  GNU Lesser General Public License. This exception does not however       *
 *  invalidate any other reasons why the executable file might be            *
 *  covered by the GNU Lesser General Public License.                        *
 *                                                                           *
 *  OpenVolumeMesh is distributed in the hope that it will be useful,        *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of           *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
 *  GNU Lesser General Public License.  For more details see                 *
 *  <http://www.gnu.org/licenses/>.                                          *
 *                                                                           *
\*===========================================================================*/

#include <vector>

#include <OpenVolumeMesh/Config/Export.hh>
#include <OpenVolumeMesh/Core/Handles.hh>
#include <OpenVolumeMesh/Geometry/VectorT.hh>
#include <OpenVolumeMesh/Mesh/PolyhedralMesh.hh>

namespace OpenVolumeMesh {

/// One record per selected cell that is actually intersected by the plane,
/// sorted by the original CellHandle.
struct OVM_EXPORT CellPartition {
    /// Handle of the original cell (now the negative-side cell).
    CellHandle parent;
    /// Negative-side cell: keeps the parent cell handle.
    CellHandle negative;
    /// Newly created positive-side cell.
    CellHandle positive;
    /// Interior face shared by both resulting cells.
    FaceHandle cap;
};

/// \brief Split a subset of the cells of a polyhedral volume mesh by a plane.
///
/// The plane is defined by normal . p + offset = 0; the normal need not be
/// normalized. Only the given cells are split into a negative and a positive
/// part. Neighbouring unselected cells are not split, but their half-face
/// lists are updated wherever an incident face gets divided. Every original
/// edge crossed by the plane is split exactly once, and the intersection
/// vertex is inserted into every face incident to that edge.
///
/// The mesh is modified in place without rebuilding or garbage collection.
/// Ordinary entity properties are inherited by the new entities (edges,
/// halfedges, faces, halffaces and cells). A halfface property named
/// "hf:uv" is treated specially: corner values are interpolated along split
/// edges and regenerated for the cap faces in a common world frame.
///
/// Requires full bottom-up incidences and deferred deletion to be enabled,
/// otherwise std::logic_error is thrown. Invalid input, non-finite
/// coordinates/plane coefficients or a plane passing through a live vertex
/// result in std::invalid_argument without modifying the mesh.
OVM_EXPORT std::vector<CellPartition>
partition_cells(GeometricPolyhedralMeshV3d& mesh,
                std::vector<CellHandle> cells,
                Geometry::Vec3d normal,
                double offset);

} // namespace OpenVolumeMesh
