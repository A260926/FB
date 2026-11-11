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

#include <cstddef>
#include <vector>

#include <OpenVolumeMesh/Config/Export.hh>
#include <OpenVolumeMesh/Core/Handles.hh>

namespace OpenVolumeMesh {

class TopologyKernel;

/**
 * @brief One oriented face of the boundary of a cell selection.
 *
 * The boundary of a set of selected cells consists of all mesh faces that
 * separate a selected cell from an unselected (or no) cell. Each such face
 * is reported exactly once, with the following orientation convention:
 *
 * Following the OpenVolumeMesh convention, the halfface owned by a cell has
 * its normal pointing *into* the cell. The #halfface stored here is the
 * opposite one: its winding is oriented such that its normal points out of
 * the selection ("from the inside of the selection towards the outside").
 */
struct SelectionBoundaryFace {
    /// Halfface of the boundary face, oriented out of the selection, i.e.
    /// the opposite of the halfface owned by #selected_cell.
    HalfFaceHandle halfface;

    /// The selected (live) cell on the inside of the boundary face.
    CellHandle selected_cell;

    /// The unselected live cell on the outside of the boundary face, or
    /// InvalidCellHandle if there is no live cell on the outside.
    CellHandle other_cell;

    /// Index of the edge-connected surface component this face belongs to,
    /// in [0, SelectionBoundary::component_count).
    size_t component = 0;
};

/**
 * @brief Result of selection_boundary(): the oriented skin of a cell
 * selection, grouped into edge-connected surface components.
 */
struct SelectionBoundary {
    /// Boundary faces, sorted by ascending FaceHandle.
    std::vector<SelectionBoundaryFace> faces;

    /// Number of distinct edge-connected surface components.
    size_t component_count = 0;
};

/**
 * @brief Compute the oriented boundary ("skin") of a set of selected cells.
 *
 * For every face of the mesh that has a live selected cell on exactly one
 * side, the result contains exactly one entry. Faces with selected cells on
 * both sides (interior to the selection) and faces without any selected
 * incident cell (e.g. unselected shells, faces without any incident cell,
 * or isolated faces) are not reported.
 *
 * Supported input meshes are topologically consistent, oriented meshes with
 * tetrahedral, hexahedral, or general polyhedral cells.
 *
 * Orientation: for each reported face, #SelectionBoundaryFace::halfface is
 * oriented such that its normal points out of the selection. Since the
 * halfface owned by a cell has its normal pointing into the cell (the
 * OpenVolumeMesh convention), the reported halfface is the opposite of the
 * halfface stored in #SelectionBoundaryFace::selected_cell.
 *
 * Components: the reported faces are grouped into surface components, where
 * two faces belong to the same component iff they are connected through
 * shared edges. Separate closed shells (e.g. the outer skin and the inner
 * skin around a cavity of the same selection) form distinct components.
 * Component indices are assigned in ascending order of the smallest
 * FaceHandle of each component and do not depend on the order of
 * `_selected_cells`.
 *
 * Validity: every reported surface component is required to be a closed
 * 2-manifold: every edge of the reported surface must be used exactly once
 * in each direction, and the faces around every vertex must form a single
 * connected fan. If two selected regions touch only along an edge or only
 * at a vertex, this is violated and a std::domain_error is thrown whose
 * message contains the index of the conflicting EdgeHandle or VertexHandle.
 *
 * Robustness: this function only uses the top-down (cell -> halfface ->
 * halfedge -> vertex) incidences and builds its own per-query index. It
 * therefore works unchanged when any or all of the bottom-up incidence
 * caches (vertex, edge, face) are disabled, and when deferred deletion is
 * enabled and garbage collection is pending. It never enables incidences,
 * never triggers garbage collection, and never changes handle numbering or
 * any mesh data, properties, or attributes. Deferred-deleted cells are
 * treated as absent: they are not valid input and never appear as
 * #SelectionBoundaryFace::other_cell.
 *
 * @param _mesh The mesh to query. It is not modified in any way.
 * @param _selected_cells Handles of the selected cells. Duplicate handles
 *        are treated as a set. An empty selection yields an empty result.
 *
 * @return The selection boundary; faces are sorted by ascending FaceHandle.
 *
 * @throw std::invalid_argument if `_selected_cells` contains a handle that
 *        is invalid, out of range, or refers to a (deferred-)deleted cell.
 * @throw std::domain_error if the selection boundary is not a closed
 *        2-manifold; the message names the conflicting EdgeHandle or
 *        VertexHandle index.
 */
OVM_EXPORT
SelectionBoundary selection_boundary(const TopologyKernel& _mesh,
                                     const std::vector<CellHandle>& _selected_cells);

} // namespace OpenVolumeMesh
