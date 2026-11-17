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

#include <OpenVolumeMesh/Unstable/Topology/InterfaceCut.hh>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <OpenVolumeMesh/Core/TopologyKernel.hh>
#include <OpenVolumeMesh/Mesh/PolyhedralMesh.hh>

namespace OpenVolumeMesh {

namespace {

using Mesh = GeometricPolyhedralMeshV3d;

constexpr const char *kFaceInterfaceId = "f:interface_id";
constexpr const char *kHalfFaceInterfaceSide = "hf:interface_side";
constexpr const char *kNextInterfaceId = "m:next_interface_id";
constexpr const char *kHalfFaceUV = "hf:uv";
constexpr const char *kHalfFaceNormal = "hf:normal";

//-----------------------------------------------------------------------------

class DisjointSet
{
public:
    explicit DisjointSet(size_t _n)
        : parent_(_n)
        , rank_(_n, 0)
    {
        std::iota(parent_.begin(), parent_.end(), 0);
    }

    int find(int _x) {
        while (parent_[_x] != _x) {
            parent_[_x] = parent_[parent_[_x]];
            _x = parent_[_x];
        }
        return _x;
    }

    void unite(int _a, int _b) {
        _a = find(_a);
        _b = find(_b);
        if (_a == _b) {
            return;
        }
        if (rank_[_a] < rank_[_b]) {
            std::swap(_a, _b);
        }
        parent_[_b] = _a;
        if (rank_[_a] == rank_[_b]) {
            ++rank_[_a];
        }
    }

private:
    std::vector<int> parent_;
    std::vector<int> rank_;
};

//-----------------------------------------------------------------------------

/// Splitting of a vertex or edge into connected sectors of its local link.
struct SplitInfo
{
    /// Cell index -> sector index.
    std::unordered_map<int, int> sector_of_cell;
    /// Smallest incident cell index per sector.
    std::vector<int> sector_min_cell;
    /// Sector containing the globally smallest incident cell.
    int representative_sector = -1;

    bool empty() const { return sector_min_cell.empty(); }
};

/// Partition the incident cells of an entity into sectors that are connected
/// through non-cut faces of the entity's local link.
SplitInfo compute_split(const TopologyKernel &_kernel,
                        const std::vector<CellHandle> &_cells_in,
                        const std::vector<FaceHandle> &_faces_in,
                        const std::set<int> &_cut_faces)
{
    SplitInfo info;

    std::vector<CellHandle> cells;
    cells.reserve(_cells_in.size());
    for (const CellHandle ch : _cells_in) {
        if (ch.is_valid() && !_kernel.is_deleted(ch)) {
            cells.push_back(ch);
        }
    }
    std::sort(cells.begin(), cells.end());
    cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
    if (cells.empty()) {
        return info;
    }

    std::unordered_map<int, int> position_of_cell;
    position_of_cell.reserve(cells.size() * 2);
    for (size_t i = 0; i < cells.size(); ++i) {
        position_of_cell.emplace(cells[i].idx(), int(i));
    }

    DisjointSet dsu(cells.size());

    for (const FaceHandle fh : _faces_in) {
        if (!fh.is_valid() || _kernel.is_deleted(fh)) {
            continue;
        }
        if (static_cast<size_t>(fh.idx()) >= _kernel.n_faces()) {
            continue;
        }
        if (_cut_faces.count(fh.idx()) != 0) {
            // The cut face is removed from the local link.
            continue;
        }

        const CellHandle c0 = _kernel.incident_cell(_kernel.halfface_handle(fh, 0));
        const CellHandle c1 = _kernel.incident_cell(_kernel.halfface_handle(fh, 1));
        if (!c0.is_valid() || !c1.is_valid()) {
            continue;
        }
        if (_kernel.is_deleted(c0) || _kernel.is_deleted(c1)) {
            continue;
        }

        const auto it0 = position_of_cell.find(c0.idx());
        const auto it1 = position_of_cell.find(c1.idx());
        if (it0 == position_of_cell.end() || it1 == position_of_cell.end()) {
            // Not a local-link face of this entity.
            continue;
        }
        dsu.unite(it0->second, it1->second);
    }

    // Cells are sorted, so the first cell seen for a root is the smallest
    // incident cell of the sector.  This makes the numbering deterministic.
    std::unordered_map<int, int> sector_of_root;
    for (size_t i = 0; i < cells.size(); ++i) {
        const int root = dsu.find(int(i));
        const auto ins = sector_of_root.emplace(root, int(info.sector_min_cell.size()));
        const int sector = ins.first->second;
        if (ins.second) {
            info.sector_min_cell.push_back(cells[i].idx());
        }
        info.sector_of_cell.emplace(cells[i].idx(), sector);
    }

    info.representative_sector = info.sector_of_cell.at(cells.front().idx());
    return info;
}

std::vector<CellHandle> incident_cells(const TopologyKernel &_kernel, VertexHandle _vh)
{
    std::vector<CellHandle> cells;
    for (const CellHandle ch : _kernel.vertex_cells(_vh)) {
        cells.push_back(ch);
    }
    return cells;
}

std::vector<CellHandle> incident_cells(const TopologyKernel &_kernel, EdgeHandle _eh)
{
    std::vector<CellHandle> cells;
    for (const CellHandle ch : _kernel.edge_cells(_eh)) {
        cells.push_back(ch);
    }
    return cells;
}

std::vector<FaceHandle> incident_faces(const TopologyKernel &_kernel, VertexHandle _vh)
{
    std::vector<FaceHandle> faces;
    for (const FaceHandle fh : _kernel.vertex_faces(_vh)) {
        faces.push_back(fh);
    }
    return faces;
}

std::vector<FaceHandle> incident_faces(const TopologyKernel &_kernel, EdgeHandle _eh)
{
    std::vector<FaceHandle> faces;
    for (const FaceHandle fh : _kernel.edge_faces(_eh)) {
        faces.push_back(fh);
    }
    return faces;
}

SplitInfo compute_vertex_split(const TopologyKernel &_kernel,
                               VertexHandle _vh,
                               const std::set<int> &_cut_faces)
{
    return compute_split(_kernel,
                         incident_cells(_kernel, _vh),
                         incident_faces(_kernel, _vh),
                         _cut_faces);
}

SplitInfo compute_edge_split(const TopologyKernel &_kernel,
                             EdgeHandle _eh,
                             const std::set<int> &_cut_faces)
{
    return compute_split(_kernel,
                         incident_cells(_kernel, _eh),
                         incident_faces(_kernel, _eh),
                         _cut_faces);
}

//-----------------------------------------------------------------------------

/// Validate the corner-valued half-face attributes.
bool validate_half_face_corner_properties(const Mesh &_mesh)
{
    const auto uv_prop = _mesh.get_halfface_property<std::vector<double>>(kHalfFaceUV);
    const auto normal_prop = _mesh.get_halfface_property<std::vector<double>>(kHalfFaceNormal);
    if (!uv_prop && !normal_prop) {
        return true;
    }

    for (const HalfFaceHandle hfh : _mesh.halffaces()) {
        const FaceHandle fh = hfh.face_handle();
        if (_mesh.is_deleted(fh)) {
            continue;
        }
        const size_t valence = _mesh.face(fh).halfedges().size();

        if (uv_prop) {
            const auto &values = (*uv_prop)[hfh];
            if (values.size() != 2 * valence) {
                return false;
            }
            for (const double value : values) {
                if (!std::isfinite(value)) {
                    return false;
                }
            }
        }
        if (normal_prop) {
            const auto &values = (*normal_prop)[hfh];
            if (values.size() != 3 * valence) {
                return false;
            }
            for (const double value : values) {
                if (!std::isfinite(value)) {
                    return false;
                }
            }
        }
    }
    return true;
}

/// Copy all half-face properties from `_src` to `_dst`, then re-express the
/// corner-valued geometry attributes for the oppositely oriented half-face:
/// the corner order is reversed and the normals are negated.
void derive_opposite_halfface(Mesh &_mesh, HalfFaceHandle _src, HalfFaceHandle _dst)
{
    if (_src == _dst) {
        return;
    }
    _mesh.copy_entity_properties(_src, _dst);

    const size_t valence = _mesh.face(_src.face_handle()).halfedges().size();

    auto uv_prop = _mesh.get_halfface_property<std::vector<double>>(kHalfFaceUV);
    if (uv_prop) {
        const std::vector<double> src_values = (*uv_prop)[_src];
        std::vector<double> dst_values(src_values.size());
        for (size_t corner = 0; corner < valence; ++corner) {
            const size_t from = valence - 1 - corner;
            dst_values[2 * corner + 0] = src_values[2 * from + 0];
            dst_values[2 * corner + 1] = src_values[2 * from + 1];
        }
        (*uv_prop)[_dst] = std::move(dst_values);
    }

    auto normal_prop = _mesh.get_halfface_property<std::vector<double>>(kHalfFaceNormal);
    if (normal_prop) {
        const std::vector<double> src_values = (*normal_prop)[_src];
        std::vector<double> dst_values(src_values.size());
        for (size_t corner = 0; corner < valence; ++corner) {
            const size_t from = valence - 1 - corner;
            for (size_t component = 0; component < 3; ++component) {
                dst_values[3 * corner + component] =
                        -src_values[3 * from + component];
            }
        }
        (*normal_prop)[_dst] = std::move(dst_values);
    }
}

bool replace_halfface(std::vector<HalfFaceHandle> &_halffaces,
                      HalfFaceHandle _old,
                      HalfFaceHandle _new)
{
    const auto it = std::find(_halffaces.begin(), _halffaces.end(), _old);
    if (it == _halffaces.end()) {
        return false;
    }
    *it = _new;
    return true;
}

std::vector<HalfEdgeHandle> map_halfface_to_cell_edges(
        const std::vector<HalfEdgeHandle> &_halfedges,
        CellHandle _cell,
        const std::function<EdgeHandle(EdgeHandle, CellHandle)> &_edge_for)
{
    std::vector<HalfEdgeHandle> result;
    result.reserve(_halfedges.size());
    for (const HalfEdgeHandle heh : _halfedges) {
        const EdgeHandle target = _edge_for(heh.edge_handle(), _cell);
        result.push_back(target.halfedge_handle(heh.subidx()));
    }
    return result;
}

} // namespace

//=============================================================================

void cut_interfaces(Mesh &_mesh, const std::vector<FaceHandle> &_faces)
{
    TopologyKernel &kernel = _mesh;

    //-------------------------------------------------------------------------
    // Validation.  Nothing below this block may modify the mesh.
    //-------------------------------------------------------------------------

    if (!kernel.has_full_bottom_up_incidences()
            || !kernel.deferred_deletion_enabled()) {
        throw std::logic_error(
                "cut_interfaces(): full bottom-up incidences and deferred "
                "deletion must be enabled.");
    }

    // Duplicates are ignored.
    std::set<int> cut_faces;
    for (const FaceHandle fh : _faces) {
        if (!fh.is_valid()
                || static_cast<size_t>(fh.idx()) >= kernel.n_faces()) {
            throw std::invalid_argument(
                    "cut_interfaces(): face handle is out of range.");
        }
        cut_faces.insert(fh.idx());
    }
    if (cut_faces.empty()) {
        // An empty set of faces does not modify the mesh.
        return;
    }

    const auto existing_face_ids =
            kernel.get_face_property<unsigned long>(kFaceInterfaceId);

    for (const int face_idx : cut_faces) {
        const FaceHandle fh(face_idx);
        if (kernel.is_deleted(fh)) {
            throw std::invalid_argument(
                    "cut_interfaces(): face handle refers to a deleted slot.");
        }
        if (existing_face_ids && (*existing_face_ids)[fh] != 0) {
            throw std::invalid_argument(
                    "cut_interfaces(): face has already been cut.");
        }

        const CellHandle c0 =
                kernel.incident_cell(kernel.halfface_handle(fh, 0));
        const CellHandle c1 =
                kernel.incident_cell(kernel.halfface_handle(fh, 1));
        if (!c0.is_valid() || !c1.is_valid()
                || kernel.is_deleted(c0) || kernel.is_deleted(c1)) {
            throw std::invalid_argument(
                    "cut_interfaces(): face is not an interior face.");
        }
    }

    if (!validate_half_face_corner_properties(_mesh)) {
        throw std::invalid_argument(
                "cut_interfaces(): hf:uv / hf:normal must have the correct "
                "length and contain only finite values.");
    }

    //-------------------------------------------------------------------------
    // Collect the entities whose local link is affected.
    //-------------------------------------------------------------------------

    std::set<int> affected_vertices;
    std::set<int> split_edges;
    for (const int face_idx : cut_faces) {
        const FaceHandle fh(face_idx);
        for (const VertexHandle vh : kernel.face_vertices(fh)) {
            affected_vertices.insert(vh.idx());
        }
        for (const EdgeHandle eh : kernel.face_edges(fh)) {
            split_edges.insert(eh.idx());
        }
    }

    // Edges incident to a split vertex may need to be re-attached to the
    // correct vertex copy even if the edge itself does not split.
    std::set<int> affected_edges = split_edges;
    for (const int vertex_idx : affected_vertices) {
        for (const EdgeHandle eh : kernel.vertex_edges(VertexHandle(vertex_idx))) {
            affected_edges.insert(eh.idx());
        }
    }

    //-------------------------------------------------------------------------
    // Compute the local-link sectors before mutating anything.
    //-------------------------------------------------------------------------

    std::vector<SplitInfo> vertex_split(kernel.n_vertices());
    std::vector<std::vector<VertexHandle>> vertex_copies(kernel.n_vertices());
    for (const int vertex_idx : affected_vertices) {
        const SplitInfo info = compute_vertex_split(
                kernel, VertexHandle(vertex_idx), cut_faces);
        vertex_split[vertex_idx] = info;
        if (info.empty()) {
            continue;
        }
        auto &copies = vertex_copies[vertex_idx];
        copies.resize(info.sector_min_cell.size(), TopologyKernel::InvalidVertexHandle);
        copies[info.representative_sector] = VertexHandle(vertex_idx);
    }

    std::vector<SplitInfo> edge_split(kernel.n_edges());
    std::vector<std::vector<EdgeHandle>> edge_copies(kernel.n_edges());
    for (const int edge_idx : affected_edges) {
        const EdgeHandle eh(edge_idx);
        const SplitInfo info = compute_edge_split(kernel, eh, cut_faces);
        edge_split[edge_idx] = info;
        if (info.empty()) {
            continue;
        }
        edge_copies[edge_idx].resize(info.sector_min_cell.size(),
                                    TopologyKernel::InvalidEdgeHandle);
    }

    // Snapshot the original endpoints of all affected edges.  Endpoint
    // handles do not change during the whole operation.
    std::map<int, std::pair<VertexHandle, VertexHandle>> edge_vertices;
    for (const int edge_idx : affected_edges) {
        const EdgeHandle eh(edge_idx);
        edge_vertices.emplace(edge_idx,
                              std::make_pair(kernel.edge(eh).from_vertex(),
                                             kernel.edge(eh).to_vertex()));
    }

    //-------------------------------------------------------------------------
    // Snapshot the cell incidence of all faces and cells we touch.
    //-------------------------------------------------------------------------

    struct CutPlan
    {
        FaceHandle face;
        HalfFaceHandle old_min_side;
        HalfFaceHandle old_max_side;
        CellHandle cell_min;
        CellHandle cell_max;
        std::vector<HalfEdgeHandle> halfedges_max;
        FaceHandle copy_face;
        HalfFaceHandle copy_inner;
    };

    std::vector<CutPlan> cut_plans;
    cut_plans.reserve(cut_faces.size());

    for (const int face_idx : cut_faces) {
        const FaceHandle fh(face_idx);
        const HalfFaceHandle h0 = kernel.halfface_handle(fh, 0);
        const HalfFaceHandle h1 = kernel.halfface_handle(fh, 1);
        const CellHandle c0 = kernel.incident_cell(h0);
        const CellHandle c1 = kernel.incident_cell(h1);

        CutPlan plan;
        plan.face = fh;
        if (c0.idx() <= c1.idx()) {
            plan.cell_min = c0;
            plan.cell_max = c1;
            plan.old_min_side = h0;
            plan.old_max_side = h1;
        } else {
            plan.cell_min = c1;
            plan.cell_max = c0;
            plan.old_min_side = h1;
            plan.old_max_side = h0;
        }
        cut_plans.push_back(std::move(plan));
    }

    std::map<int, std::vector<HalfFaceHandle>> cell_halffaces;
    for (const CutPlan &plan : cut_plans) {
        cell_halffaces.emplace(plan.cell_min.idx(),
                               kernel.cell(plan.cell_min).halffaces());
        cell_halffaces.emplace(plan.cell_max.idx(),
                               kernel.cell(plan.cell_max).halffaces());
    }

    // Non-cut faces that may need a different half-edge copy after the split.
    std::set<int> dirty_faces;
    for (const int edge_idx : split_edges) {
        for (const FaceHandle fh : kernel.edge_faces(EdgeHandle(edge_idx))) {
            if (!kernel.is_deleted(fh) && cut_faces.count(fh.idx()) == 0) {
                dirty_faces.insert(fh.idx());
            }
        }
    }
    std::map<int, CellHandle> face_reference_cell;
    for (const int face_idx : dirty_faces) {
        const FaceHandle fh(face_idx);
        const CellHandle c0 =
                kernel.incident_cell(kernel.halfface_handle(fh, 0));
        const CellHandle c1 =
                kernel.incident_cell(kernel.halfface_handle(fh, 1));
        if (c0.is_valid() && !kernel.is_deleted(c0)) {
            face_reference_cell.emplace(face_idx, c0);
        } else if (c1.is_valid() && !kernel.is_deleted(c1)) {
            face_reference_cell.emplace(face_idx, c1);
        }
    }

    //-------------------------------------------------------------------------
    // Mutation begins here.
    //-------------------------------------------------------------------------

    auto face_interface_id =
            kernel.request_face_property<unsigned long>(kFaceInterfaceId, 0ul);
    kernel.set_persistent(face_interface_id);
    auto halfface_interface_side =
            kernel.request_halfface_property<int>(kHalfFaceInterfaceSide, -1);
    kernel.set_persistent(halfface_interface_side);
    auto next_interface_id =
            kernel.request_mesh_property<unsigned long>(kNextInterfaceId, 1ul);
    kernel.set_persistent(next_interface_id);

    unsigned long next_id =
            std::max<unsigned long>(1ul, next_interface_id[MeshHandle(0)]);
    for (const FaceHandle fh : kernel.faces()) {
        next_id = std::max<unsigned long>(next_id,
                                          face_interface_id[fh] + 1ul);
    }

    //-------------------------------------------------------------------------
    // Duplicate vertices.
    //-------------------------------------------------------------------------

    for (const int vertex_idx : affected_vertices) {
        const VertexHandle vh(vertex_idx);
        const SplitInfo &info = vertex_split[vertex_idx];
        if (info.empty()) {
            continue;
        }
        auto &copies = vertex_copies[vertex_idx];
        for (size_t sector = 0; sector < copies.size(); ++sector) {
            if (int(sector) == info.representative_sector) {
                continue;
            }
            const VertexHandle copy = kernel.add_vertex();
            kernel.copy_entity_properties(vh, copy);
            copies[sector] = copy;
        }
    }

    auto vertex_for = [&](VertexHandle _vh, CellHandle _ch) -> VertexHandle {
        const SplitInfo &info = vertex_split[_vh.idx()];
        if (info.empty()) {
            return _vh;
        }
        const auto it = info.sector_of_cell.find(_ch.idx());
        if (it == info.sector_of_cell.end()) {
            return _vh;
        }
        return vertex_copies[_vh.idx()][size_t(it->second)];
    };

    //-------------------------------------------------------------------------
    // Duplicate edges, re-attaching all affected edges to the right vertices.
    //-------------------------------------------------------------------------

    for (const int edge_idx : affected_edges) {
        const EdgeHandle eh(edge_idx);
        const SplitInfo &info = edge_split[edge_idx];
        if (info.empty()) {
            continue;
        }
        const VertexHandle old_from = edge_vertices.at(edge_idx).first;
        const VertexHandle old_to = edge_vertices.at(edge_idx).second;
        auto &copies = edge_copies[edge_idx];

        for (size_t sector = 0; sector < copies.size(); ++sector) {
            const CellHandle reference_cell(info.sector_min_cell[sector]);
            const VertexHandle from = vertex_for(old_from, reference_cell);
            const VertexHandle to = vertex_for(old_to, reference_cell);

            if (int(sector) == info.representative_sector) {
                // Keep the old edge handle, but attach it to the correct
                // vertex copies.  The orientation is retained.
                kernel.set_edge(eh, from, to);
                copies[sector] = eh;
            } else {
                const EdgeHandle copy = kernel.add_edge(from, to, true);
                kernel.copy_entity_properties(eh, copy);
                kernel.copy_entity_properties(eh.halfedge_handle(0),
                                              copy.halfedge_handle(0));
                kernel.copy_entity_properties(eh.halfedge_handle(1),
                                              copy.halfedge_handle(1));
                copies[sector] = copy;
            }
        }
    }

    auto edge_for = [&](EdgeHandle _eh, CellHandle _ch) -> EdgeHandle {
        const SplitInfo &info = edge_split[_eh.idx()];
        if (info.empty()) {
            return _eh;
        }
        const auto it = info.sector_of_cell.find(_ch.idx());
        if (it == info.sector_of_cell.end()) {
            return _eh;
        }
        return edge_copies[_eh.idx()][size_t(it->second)];
    };

    //-------------------------------------------------------------------------
    // Create the duplicate faces.
    //-------------------------------------------------------------------------

    for (CutPlan &plan : cut_plans) {
        plan.halfedges_max = map_halfface_to_cell_edges(
                kernel.halfface(plan.old_max_side).halfedges(),
                plan.cell_max, edge_for);

        // The new face is stored in the orientation seen by the larger cell,
        // so its first half-face is the cell-incident one.
        plan.copy_face = kernel.add_face(plan.halfedges_max, false);
        plan.copy_inner = kernel.halfface_handle(plan.copy_face, 0);
        const HalfFaceHandle copy_outer =
                kernel.halfface_handle(plan.copy_face, 1);

        kernel.copy_entity_properties(plan.face, plan.copy_face);
        kernel.copy_entity_properties(plan.old_max_side, plan.copy_inner);
        derive_opposite_halfface(_mesh, plan.copy_inner, copy_outer);
    }

    //-------------------------------------------------------------------------
    // Transfer the original faces to the smaller cells and commit cells.
    //-------------------------------------------------------------------------

    for (CutPlan &plan : cut_plans) {
        // Re-attach the old face to the edge copies of the smaller cell.
        const std::vector<HalfEdgeHandle> old_halfedges =
                kernel.face(plan.face).halfedges();
        const std::vector<HalfEdgeHandle> new_halfedges =
                map_halfface_to_cell_edges(old_halfedges,
                                           plan.cell_min, edge_for);
        if (new_halfedges != old_halfedges) {
            kernel.set_face(plan.face, new_halfedges);
        }

        // The old face keeps the corner data of the smaller cell's side.  Its
        // opposite half-face is the other side of the same surface, so it is
        // re-derived (never mixing the two original sides' data).
        derive_opposite_halfface(_mesh, plan.old_min_side, plan.old_max_side);

        const unsigned long id = next_id++;
        face_interface_id[plan.face] = id;
        face_interface_id[plan.copy_face] = id;

        // The old face keeps its orientation and its old handle; its
        // cell-incident half-face belongs to the smaller cell.
        halfface_interface_side[plan.old_min_side] = 0;
        halfface_interface_side[plan.old_max_side] = -1;
        halfface_interface_side[plan.copy_inner] = 1;
        halfface_interface_side[kernel.halfface_handle(plan.copy_face, 1)] = -1;

        auto &max_cell_halffaces = cell_halffaces.at(plan.cell_max.idx());
        if (!replace_halfface(max_cell_halffaces,
                              plan.old_max_side, plan.copy_inner)) {
            throw std::logic_error(
                    "cut_interfaces(): inconsistent cell incidence.");
        }
    }

    //-------------------------------------------------------------------------
    // Update half-edge handles of the remaining faces.
    //-------------------------------------------------------------------------

    for (const auto &entry : face_reference_cell) {
        const FaceHandle fh(entry.first);
        const std::vector<HalfEdgeHandle> old_halfedges =
                kernel.face(fh).halfedges();
        const std::vector<HalfEdgeHandle> new_halfedges =
                map_halfface_to_cell_edges(old_halfedges,
                                           entry.second, edge_for);
        if (new_halfedges != old_halfedges) {
            kernel.set_face(fh, new_halfedges);
        }
    }

    for (auto &entry : cell_halffaces) {
        kernel.set_cell(CellHandle(entry.first), entry.second);
    }

    //-------------------------------------------------------------------------
    // Restore the cyclic order of half-faces around all touched edges.
    //-------------------------------------------------------------------------

    std::set<int> dirty_edges;
    auto collect_edges = [&](FaceHandle _fh) {
        for (const EdgeHandle eh : kernel.face_edges(_fh)) {
            dirty_edges.insert(eh.idx());
        }
    };
    for (const CutPlan &plan : cut_plans) {
        collect_edges(plan.face);
        collect_edges(plan.copy_face);
    }
    for (const auto &entry : face_reference_cell) {
        collect_edges(FaceHandle(entry.first));
    }
    for (const int edge_idx : affected_edges) {
        dirty_edges.insert(edge_idx);
        for (const EdgeHandle eh : edge_copies[edge_idx]) {
            if (eh.is_valid()) {
                dirty_edges.insert(eh.idx());
            }
        }
    }
    for (const int edge_idx : dirty_edges) {
        kernel.reorder_incident_halffaces(EdgeHandle(edge_idx));
    }

    next_interface_id[MeshHandle(0)] = next_id;
}

//=============================================================================

std::vector<InterfacePair> interface_pairs(const Mesh &_mesh)
{
    const auto face_ids = _mesh.get_face_property<unsigned long>(kFaceInterfaceId);
    const auto sides = _mesh.get_halfface_property<int>(kHalfFaceInterfaceSide);
    if (!face_ids || !sides) {
        return {};
    }

    const bool can_check_cells = _mesh.has_face_bottom_up_incidences();

    struct Entry
    {
        HalfFaceHandle halffaces[2];
        bool present[2] = {false, false};
    };
    std::map<unsigned long, Entry> by_id;

    for (const FaceHandle fh : _mesh.faces()) {
        if (_mesh.is_deleted(fh)) {
            continue;
        }
        const unsigned long id = (*face_ids)[fh];
        if (id == 0) {
            continue;
        }
        Entry &entry = by_id[id];
        for (int sub = 0; sub < 2; ++sub) {
            const HalfFaceHandle hfh = _mesh.halfface_handle(fh, sub);
            const int side = (*sides)[hfh];
            if (side == 0 || side == 1) {
                entry.halffaces[side] = hfh;
                entry.present[side] = true;
            }
        }
    }

    std::vector<InterfacePair> result;
    result.reserve(by_id.size());
    for (const auto &item : by_id) {
        if (!item.second.present[0] || !item.second.present[1]) {
            continue;
        }
        const HalfFaceHandle side0 = item.second.halffaces[0];
        const HalfFaceHandle side1 = item.second.halffaces[1];
        if (can_check_cells) {
            // Only half-faces that still have an owning cell belong to a
            // usable interface.  This filters out interfaces whose cell was
            // deleted (and possibly garbage collected) after the cut.
            const CellHandle cell0 = _mesh.incident_cell(side0);
            const CellHandle cell1 = _mesh.incident_cell(side1);
            if (!cell0.is_valid() || !cell1.is_valid()
                    || _mesh.is_deleted(cell0) || _mesh.is_deleted(cell1)) {
                continue;
            }
        }
        InterfacePair pair;
        pair.id = item.first;
        pair.side0 = side0;
        pair.side1 = side1;
        result.push_back(pair);
    }
    return result;
}

} // namespace OpenVolumeMesh
