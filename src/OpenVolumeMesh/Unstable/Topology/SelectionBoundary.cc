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

#include <OpenVolumeMesh/Unstable/Topology/SelectionBoundary.hh>

#include <algorithm>
#include <array>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>

#include <OpenVolumeMesh/Core/BaseEntities.hh>
#include <OpenVolumeMesh/Core/TopologyKernel.hh>

namespace OpenVolumeMesh {

namespace {

/// Minimal union-find used for surface components and vertex fans.
class DisjointSets {
public:
    explicit DisjointSets(size_t _n) : parent_(_n) {
        std::iota(parent_.begin(), parent_.end(), 0);
    }
    size_t find(size_t _x) {
        while (parent_[_x] != _x) {
            parent_[_x] = parent_[parent_[_x]];
            _x = parent_[_x];
        }
        return _x;
    }
    void unite(size_t _a, size_t _b) {
        _a = find(_a);
        _b = find(_b);
        if (_a != _b) parent_[_b] = _a;
    }
private:
    std::vector<size_t> parent_;
};

} // anonymous namespace

SelectionBoundary selection_boundary(const TopologyKernel& _mesh,
                                     const std::vector<CellHandle>& _selected_cells)
{
    // Validate the input handles; duplicates are treated as a set.
    std::vector<char> is_selected(_mesh.n_cells(), 0);
    for (const CellHandle ch : _selected_cells) {
        if (!_mesh.is_valid(ch) || _mesh.is_deleted(ch)) {
            throw std::invalid_argument(
                "selection_boundary: cell handle " + std::to_string(ch.idx()) +
                " is invalid, out of range, or deleted");
        }
        is_selected[ch.uidx()] = 1;
    }

    SelectionBoundary result;
    if (_selected_cells.empty()) {
        return result;
    }

    // Per-query index: halfface -> incident live cell, built from the
    // top-down (cell -> halfface) relation only. This works regardless of
    // which bottom-up incidence caches are enabled and never touches them.
    // Deferred-deleted cells are treated as absent.
    std::vector<CellHandle> incident_cell(_mesh.n_halffaces(), TopologyKernel::InvalidCellHandle);
    for (size_t ci = 0; ci < _mesh.n_cells(); ++ci) {
        const CellHandle ch(static_cast<int>(ci));
        if (_mesh.is_deleted(ch)) continue;
        for (const HalfFaceHandle hfh : _mesh.cell(ch).halffaces()) {
            incident_cell[hfh.uidx()] = ch;
        }
    }

    // Collect the boundary faces: exactly one side selected.
    struct Entry {
        FaceHandle face;
        HalfFaceHandle halfface; // oriented out of the selection
        CellHandle selected_cell;
        CellHandle other_cell;
    };
    std::vector<Entry> entries;
    for (size_t ci = 0; ci < is_selected.size(); ++ci) {
        if (!is_selected[ci]) continue;
        const CellHandle ch(static_cast<int>(ci));
        for (const HalfFaceHandle hfh : _mesh.cell(ch).halffaces()) {
            // The cell-owned halfface points into the cell; report its
            // opposite, which points out of the selection.
            const HalfFaceHandle out = TopologyKernel::opposite_halfface_handle(hfh);
            const CellHandle other = incident_cell[out.uidx()];
            if (other.is_valid() && is_selected[other.uidx()]) {
                continue; // selected on both sides: interior to the selection
            }
            entries.push_back({TopologyKernel::face_handle(out), out, ch, other});
        }
    }

    // Output is sorted by ascending FaceHandle.
    std::sort(entries.begin(), entries.end(),
              [](const Entry& _a, const Entry& _b) { return _a.face < _b.face; });

    const size_t n_entries = entries.size();

    // Pass 1: count directed edge usages of the reported surface.
    std::vector<std::array<int, 2>> edge_dir_count(_mesh.n_edges(), {{0, 0}});
    for (const Entry& e : entries) {
        for (const HalfEdgeHandle heh : _mesh.halfface(e.halfface).halfedges()) {
            const EdgeHandle eh = TopologyKernel::edge_handle(heh);
            edge_dir_count[eh.uidx()][static_cast<size_t>(heh.idx() & 1)]++;
        }
    }
    // Closed 2-manifold, edge part: every used edge appears exactly once in
    // each direction. This rejects selections whose parts touch along an
    // edge (the shared edge would be used more than twice).
    for (size_t ei = 0; ei < edge_dir_count.size(); ++ei) {
        const auto& c = edge_dir_count[ei];
        if (c[0] == 0 && c[1] == 0) continue;
        if (c[0] != 1 || c[1] != 1) {
            throw std::domain_error(
                "selection_boundary: selection boundary is not a closed "
                "2-manifold at edge " + std::to_string(ei));
        }
    }

    // Pass 2: edge -> the (exactly two) reported faces using it, and
    // vertex -> (face, edge) incidences for the fan check.
    std::vector<std::array<size_t, 2>> edge_faces(_mesh.n_edges(),
                                                  {{n_entries, n_entries}});
    std::vector<std::vector<std::pair<size_t, size_t>>> vertex_uses(_mesh.n_vertices());
    for (size_t i = 0; i < n_entries; ++i) {
        for (const HalfEdgeHandle heh : _mesh.halfface(entries[i].halfface).halfedges()) {
            const EdgeHandle eh = TopologyKernel::edge_handle(heh);
            auto& ef = edge_faces[eh.uidx()];
            if (ef[0] == n_entries) ef[0] = i; else ef[1] = i;
            vertex_uses[_mesh.from_vertex_handle(heh).uidx()].emplace_back(i, eh.uidx());
            vertex_uses[_mesh.to_vertex_handle(heh).uidx()].emplace_back(i, eh.uidx());
        }
    }

    // Edge-connected surface components.
    DisjointSets components(n_entries);
    for (const auto& ef : edge_faces) {
        if (ef[0] != n_entries && ef[1] != n_entries) {
            components.unite(ef[0], ef[1]);
        }
    }

    // Closed 2-manifold, vertex part: the faces around each vertex must
    // form a single connected fan (linked through the edges at that
    // vertex). This rejects selections whose parts touch only at a vertex.
    for (size_t vi = 0; vi < vertex_uses.size(); ++vi) {
        const auto& uses = vertex_uses[vi];
        if (uses.empty()) continue;
        // Collect the reported faces incident to this vertex.
        std::vector<size_t> local;
        local.reserve(uses.size());
        for (const auto& u : uses) local.push_back(u.first);
        std::sort(local.begin(), local.end());
        local.erase(std::unique(local.begin(), local.end()), local.end());
        if (local.size() <= 1) continue;

        DisjointSets fan(local.size());
        auto local_id = [&](size_t _entry) {
            return static_cast<size_t>(
                std::lower_bound(local.begin(), local.end(), _entry) - local.begin());
        };
        for (const auto& u : uses) {
            const auto& ef = edge_faces[u.second];
            const size_t other = (ef[0] == u.first) ? ef[1] : ef[0];
            fan.unite(local_id(u.first), local_id(other));
        }
        const size_t root = fan.find(0);
        for (size_t k = 1; k < local.size(); ++k) {
            if (fan.find(k) != root) {
                throw std::domain_error(
                    "selection_boundary: selection boundary does not form a "
                    "single face fan at vertex " + std::to_string(vi));
            }
        }
    }

    // Assign component indices in ascending order of each component's
    // smallest FaceHandle, independent of the input selection order.
    std::vector<size_t> comp_root(n_entries);
    std::vector<std::pair<int, size_t>> comp_order; // (min face idx, root)
    {
        std::vector<char> seen(n_entries, 0);
        std::vector<int> min_face(n_entries, -1);
        for (size_t i = 0; i < n_entries; ++i) {
            const size_t r = components.find(i);
            comp_root[i] = r;
            if (!seen[r]) {
                seen[r] = 1;
                min_face[r] = entries[i].face.idx();
            } else {
                min_face[r] = std::min(min_face[r], entries[i].face.idx());
            }
        }
        for (size_t i = 0; i < n_entries; ++i) {
            if (seen[i]) comp_order.emplace_back(min_face[i], i);
        }
        std::sort(comp_order.begin(), comp_order.end());
    }
    std::vector<size_t> comp_id(n_entries, 0);
    for (size_t k = 0; k < comp_order.size(); ++k) {
        comp_id[comp_order[k].second] = k;
    }

    result.faces.reserve(n_entries);
    for (size_t i = 0; i < n_entries; ++i) {
        result.faces.push_back({entries[i].halfface,
                                entries[i].selected_cell,
                                entries[i].other_cell,
                                comp_id[comp_root[i]]});
    }
    result.component_count = comp_order.size();
    return result;
}

} // namespace OpenVolumeMesh
