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
 *  published by the Free Software Foundation, either version 3 of           *
 *  the License, or (at your option) any later version with the              *
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
 *  GNU Lesser General Public License for more details.                      *
 *                                                                           *
 *  You should have received a copy of the GNU LesserGeneral Public          *
 *  License along with OpenVolumeMesh.  If not,                              *
 *  see <http://www.gnu.org/licenses/>.                                      *
 *                                                                           *
\*===========================================================================*/

#include <initializer_list>
#include <set>
#include <vector>

#include <OpenVolumeMesh/Config/Export.hh>
#include <OpenVolumeMesh/Core/Handles.hh>
#include <OpenVolumeMesh/Mesh/PolyhedralMesh.hh>

namespace OpenVolumeMesh {

/**
 * One cut interface, as returned by interface_pairs().
 *
 * Both half-faces belong to one of the two face copies created by
 * cut_interfaces().  `side0` is the half-face of the copy that belonged to
 * the incident cell with the smaller CellHandle before the cut.
 */
struct InterfacePair {
    unsigned long id = 0;
    HalfFaceHandle side0;
    HalfFaceHandle side1;
};

/**
 * \brief Cut a polyhedral mesh open along the given interior faces.
 *
 * Each face in \p faces is duplicated, such that both sides become boundary
 * faces of their respective original cells.  Vertices and edges are
 * duplicated according to the connected sectors of the local link of their
 * incident cells after removing the selected faces.  Cells, vertex
 * positions, cell order and the winding of the cells are preserved.
 *
 * The mesh must have all bottom-up incidences and deferred deletion enabled.
 * The input faces must exist, must not be deleted, must be interior and must
 * not have been cut already; otherwise an std::invalid_argument is thrown
 * before the mesh is modified.  An empty face list leaves the mesh untouched.
 * Neither bottom-up incidences nor deferred deletion are changed and no
 * garbage collection is performed.
 *
 * The following persistent properties are maintained:
 *  - f:interface_id (unsigned long): 0 for ordinary faces, the interface id
 *    for the two copies of a cut face.
 *  - hf:interface_side (int): -1 for ordinary half-faces, 0/1 for the two
 *    cell-incident half-faces of the interface copies.
 *  - m:next_interface_id (unsigned long): the next unused interface id.
 *
 * The function guarantees no rollback on allocation failure; a failed
 * allocation may leave the mesh partially modified.
 *
 * \throws std::logic_error if the mesh state is not supported.
 * \throws std::invalid_argument if the face list is invalid.
 */
OVM_EXPORT
void cut_interfaces(GeometricPolyhedralMeshV3d& mesh,
                    const std::vector<FaceHandle>& faces);

/// Convenience overload for a set of faces.
inline void cut_interfaces(GeometricPolyhedralMeshV3d& mesh,
                           const std::set<FaceHandle>& faces)
{
    cut_interfaces(mesh, std::vector<FaceHandle>(faces.begin(), faces.end()));
}

/// Convenience overload for an initializer list.
inline void cut_interfaces(GeometricPolyhedralMeshV3d& mesh,
                           std::initializer_list<FaceHandle> faces)
{
    cut_interfaces(mesh, std::vector<FaceHandle>(faces.begin(), faces.end()));
}

/**
 * \brief Query the cut interfaces of a mesh.
 *
 * The result is sorted by interface id.  Each id appears exactly once; the
 * two returned half-faces are the cell-incident half-faces of the two face
 * copies, with side0 belonging to the cell with the smaller pre-cut handle.
 */
OVM_EXPORT
std::vector<InterfacePair> interface_pairs(const GeometricPolyhedralMeshV3d& mesh);

} // namespace OpenVolumeMesh
