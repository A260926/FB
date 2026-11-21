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

#include <OpenVolumeMesh/Unstable/Topology/PlanePartition.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include <OpenVolumeMesh/Core/Properties/PropertyStorageT.hh>
#include <OpenVolumeMesh/Core/detail/internal_type_name.hh>

namespace OpenVolumeMesh {

namespace {

using Geometry::Vec3d;
using UV2 = std::array<double, 2>;

// ResourceManager::properties() is protected.  A pointer-to-member-function
// named through the derived class may be formed there and later called on
// any base-class instance (class.protected only restricts the naming
// context, not the object used for the call).
struct PropertyAccess : ResourceManager {
    template <class EntityTag>
    static auto all_properties_pmf() {
        return &PropertyAccess::properties<EntityTag>;
    }
};

template <class EntityTag>
auto all_properties(TopologyKernel& _mesh) {
    return (_mesh.*PropertyAccess::all_properties_pmf<EntityTag>())();
}

template <class EntityTag, class HandleT>
void copy_all_properties(TopologyKernel& _mesh, HandleT _src, HandleT _dst) {
    for (auto prop : all_properties<EntityTag>(_mesh)) {
        prop->copy(_src.uidx(), _dst.uidx());
    }
}

// Direct storage access for the specially handled hf:uv property.
std::vector<double>& uv_at(PropertyStorageT<std::vector<double>>* _prop,
                           HalfFaceHandle _hfh) {
    return _prop->data_vector()[_hfh.uidx()];
}

bool is_finite(const Vec3d& _v) {
    return std::isfinite(_v[0]) && std::isfinite(_v[1]) && std::isfinite(_v[2]);
}

constexpr double kPlaneEps = 1e-9;

// Plan of a single original edge crossed by the plane.
struct EdgeSplit {
    VertexHandle mid;
    VertexHandle orig_from;
    VertexHandle orig_to;
    Vec3d mid_position;
    double t = 0.0;                 // interpolation parameter along h0
    EdgeHandle positive_edge;
};

// Plan of a boundary face that is cut into two child faces.
struct FaceSplit {
    FaceHandle positive_face;
    EdgeHandle cap_edge;
};

} // anonymous namespace

std::vector<CellPartition>
partition_cells(GeometricPolyhedralMeshV3d& _mesh,
                std::vector<CellHandle> _cells,
                Vec3d _normal,
                double _offset) {

    // Duplicate selections are treated as a set; an empty selection is a
    // no-op that must not touch the mesh at all.
    std::set<CellHandle> selected_set;
    for (CellHandle ch : _cells) {
        selected_set.insert(ch);
    }
    if (selected_set.empty()) {
        return {};
    }

    if (!_mesh.has_full_bottom_up_incidences() ||
        !_mesh.deferred_deletion_enabled()) {
        throw std::logic_error(
            "partition_cells: full bottom-up incidences and deferred "
            "deletion must be enabled");
    }

    // -------------------------------------------------------------------
    // Validation: everything below must leave the mesh unchanged on error.
    // -------------------------------------------------------------------
    if (!std::isfinite(_offset) || !is_finite(_normal)) {
        throw std::invalid_argument(
            "partition_cells: plane contains non-finite coefficients");
    }
    const double normal_length = _normal.norm();
    if (!std::isfinite(normal_length) || normal_length == 0.0) {
        throw std::invalid_argument("partition_cells: plane normal is zero");
    }
    const Vec3d n = _normal / normal_length;
    const double off = _offset / normal_length;

    for (VertexHandle vh : _mesh.vertices()) {
        if (_mesh.is_deleted(vh)) {
            continue;
        }
        if (!is_finite(_mesh.vertex(vh))) {
            throw std::invalid_argument(
                "partition_cells: mesh contains non-finite vertex coordinates");
        }
    }

    for (CellHandle ch : selected_set) {
        if (!_mesh.is_valid(ch) || _mesh.is_deleted(ch)) {
            throw std::invalid_argument(
                "partition_cells: selection contains an invalid or deleted cell");
        }
    }

    // Locate and validate a potential hf:uv property.
    std::shared_ptr<PropertyStorageBase> uv_storage;
    for (auto prop : all_properties<Entity::HalfFace>(_mesh)) {
        if (prop->name() == "hf:uv") {
            uv_storage = prop;
            break;
        }
    }
    PropertyStorageT<std::vector<double>>* uv_prop = nullptr;
    if (uv_storage) {
        if (uv_storage->internal_type_name() !=
            detail::internal_type_name<std::vector<double>>()) {
            throw std::invalid_argument(
                "partition_cells: hf:uv must store std::vector<double>");
        }
        uv_prop = uv_storage->cast_to_StorageT<std::vector<double>>();
        for (HalfFaceHandle hfh : _mesh.halffaces()) {
            if (_mesh.is_deleted(hfh)) {
                continue;
            }
            const std::vector<double>& uv = uv_at(uv_prop, hfh);
            const size_t expected =
                2u * _mesh.face(hfh.face_handle()).halfedges().size();
            if (uv.size() != expected) {
                throw std::invalid_argument(
                    "partition_cells: hf:uv size does not match halfface valence");
            }
            for (double x : uv) {
                if (!std::isfinite(x)) {
                    throw std::invalid_argument(
                        "partition_cells: hf:uv contains a non-finite value");
                }
            }
        }
    }

    // Signed normalized distances of all live vertices.
    std::unordered_map<VertexHandle, double> dist;
    dist.reserve(_mesh.n_vertices());
    for (VertexHandle vh : _mesh.vertices()) {
        if (_mesh.is_deleted(vh)) {
            continue;
        }
        const double d = n.dot(_mesh.vertex(vh)) + off;
        if (std::abs(d) <= kPlaneEps) {
            throw std::invalid_argument(
                "partition_cells: plane passes through a live vertex; only "
                "proper transversal cuts are supported");
        }
        dist[vh] = d;
    }
    // Returns -1/+1 for original vertices, 0 for vertices created later.
    auto side_of = [&](VertexHandle _vh) -> int {
        auto it = dist.find(_vh);
        if (it == dist.end()) {
            return 0;
        }
        return it->second < 0.0 ? -1 : 1;
    };

    // -------------------------------------------------------------------
    // Planning phase (no mesh modification)
    // -------------------------------------------------------------------
    std::vector<CellHandle> selected(selected_set.begin(), selected_set.end());
    std::sort(selected.begin(), selected.end());

    std::vector<CellHandle> cut_cells;
    for (CellHandle ch : selected) {
        bool has_neg = false;
        bool has_pos = false;
        for (VertexHandle vh : _mesh.cell_vertices(ch)) {
            if (side_of(vh) < 0) {
                has_neg = true;
            } else {
                has_pos = true;
            }
        }
        if (has_neg && has_pos) {
            cut_cells.push_back(ch);
        }
    }
    if (cut_cells.empty()) {
        return {};
    }

    // Every original edge crossed by a selected cut cell is split once.
    std::map<EdgeHandle, EdgeSplit> edge_splits;
    for (CellHandle ch : cut_cells) {
        for (EdgeHandle eh : _mesh.cell_edges(ch)) {
            if (_mesh.is_deleted(eh) || edge_splits.count(eh)) {
                continue;
            }
            const auto endpoints = _mesh.edge_vertices(eh);
            if (side_of(endpoints[0]) != side_of(endpoints[1])) {
                const double d0 = dist[endpoints[0]];
                const double d1 = dist[endpoints[1]];
                const double t = -d0 / (d1 - d0);
                EdgeSplit plan;
                plan.orig_from = endpoints[0];
                plan.orig_to = endpoints[1];
                plan.t = t;
                plan.mid_position =
                    (1.0 - t) * _mesh.vertex(endpoints[0]) +
                    t * _mesh.vertex(endpoints[1]);
                edge_splits.emplace(eh, plan);
            }
        }
    }

    // Every face incident to a split edge gets at least a midpoint insertion.
    std::set<FaceHandle> affected_faces;
    for (const auto& entry : edge_splits) {
        for (FaceHandle fh : _mesh.edge_faces(entry.first)) {
            if (!_mesh.is_deleted(fh)) {
                affected_faces.insert(fh);
            }
        }
    }
    // Boundary faces of a cut selected cell are divided into two children.
    std::set<FaceHandle> divided_faces;
    for (CellHandle ch : cut_cells) {
        for (FaceHandle fh : _mesh.cell_faces(ch)) {
            if (affected_faces.count(fh)) {
                divided_faces.insert(fh);
            }
        }
    }

    // Snapshot original corner UVs per halfface, keyed by the corner vertex.
    // Also remember the original vertex/edge loop of every affected face,
    // because the edge split below destroys the original halfedges.
    std::map<HalfFaceHandle, std::map<VertexHandle, UV2>> uv_snapshot;
    std::map<FaceHandle, std::vector<VertexHandle>> face_old_vertices;
    std::map<FaceHandle, std::vector<EdgeHandle>> face_old_edges;
    for (FaceHandle fh : affected_faces) {
        const std::vector<HalfEdgeHandle> old_hes = _mesh.face(fh).halfedges();
        std::vector<VertexHandle> old_vs;
        std::vector<EdgeHandle> old_es;
        old_vs.reserve(old_hes.size());
        old_es.reserve(old_hes.size());
        for (HalfEdgeHandle heh : old_hes) {
            old_vs.push_back(_mesh.from_vertex_handle(heh));
            old_es.push_back(heh.edge_handle());
        }
        face_old_vertices[fh] = std::move(old_vs);
        face_old_edges[fh] = std::move(old_es);
        if (uv_prop) {
            for (unsigned char side = 0; side < 2; ++side) {
                HalfFaceHandle hfh = fh.halfface_handle(side);
                const std::vector<double>& uv = uv_at(uv_prop, hfh);
                const auto side_hes = _mesh.halfface(hfh).halfedges();
                auto& corner_map = uv_snapshot[hfh];
                // The two sides have reversed corner orders, so each side
                // must be indexed through its own halfedge loop.
                for (size_t i = 0; i < side_hes.size(); ++i) {
                    VertexHandle vh =
                        _mesh.from_vertex_handle(side_hes[i]);
                    corner_map[vh] = UV2{uv[2 * i], uv[2 * i + 1]};
                }
            }
        }
    }

    // Common world frame for both sides of every cap face.
    Vec3d axis(1.0, 0.0, 0.0);
    {
        double best = std::abs(n.dot(Vec3d(1.0, 0.0, 0.0)));
        const double cand_y = std::abs(n.dot(Vec3d(0.0, 1.0, 0.0)));
        const double cand_z = std::abs(n.dot(Vec3d(0.0, 0.0, 1.0)));
        if (cand_y < best) {
            best = cand_y;
            axis = Vec3d(0.0, 1.0, 0.0);
        }
        if (cand_z < best) {
            axis = Vec3d(0.0, 0.0, 1.0);
        }
    }
    const Vec3d U = (axis.cross(n)).normalized();
    const Vec3d V = n.cross(U);
    auto cap_uv_of = [&](VertexHandle _vh) -> UV2 {
        const Vec3d& p = _mesh.vertex(_vh);
        return UV2{p.dot(U), p.dot(V)};
    };

    // -------------------------------------------------------------------
    // Commit phase
    //
    // 1. Split edges.  The original handle keeps the negative-side
    //    sub-edge and its original direction; the positive sub-edge is
    //    a newly created edge.
    // -------------------------------------------------------------------
    std::unordered_map<VertexHandle, EdgeHandle> mid_to_edge;
    for (auto& entry : edge_splits) {
        EdgeHandle eh = entry.first;
        EdgeSplit& plan = entry.second;
        const VertexHandle v0 = plan.orig_from;
        const VertexHandle v1 = plan.orig_to;
        const VertexHandle mid = _mesh.add_vertex(plan.mid_position);
        plan.mid = mid;
        mid_to_edge[mid] = eh;

        EdgeHandle new_edge;
        if (side_of(v0) < 0) {
            // original h0: v0 -> v1, negative part is v0 -> mid
            _mesh.set_edge(eh, v0, mid);
            new_edge = _mesh.add_edge(mid, v1);
        } else {
            // negative part is mid -> v1, keeping the original direction
            _mesh.set_edge(eh, mid, v1);
            new_edge = _mesh.add_edge(v0, mid);
        }
        plan.positive_edge = new_edge;

        copy_all_properties<Entity::Edge>(_mesh, eh, new_edge);
        copy_all_properties<Entity::HalfEdge>(_mesh, eh.h0(), new_edge.h0());
        copy_all_properties<Entity::HalfEdge>(_mesh, eh.h1(), new_edge.h1());
    }

    auto oriented_halfedge = [&](VertexHandle _from, VertexHandle _to) {
        HalfEdgeHandle heh = _mesh.find_halfedge(_from, _to);
        if (!heh.is_valid()) {
            throw std::logic_error(
                "partition_cells: internal error, expected halfedge missing");
        }
        return heh;
    };

    // UV corner at the midpoint of a split edge as seen from one halfface.
    auto midpoint_uv = [&](HalfFaceHandle _origin_hfh, EdgeHandle _eh) {
        const EdgeSplit& plan = edge_splits.at(_eh);
        const auto& corner_map = uv_snapshot.at(_origin_hfh);
        const UV2& a = corner_map.at(plan.orig_from);
        const UV2& b = corner_map.at(plan.orig_to);
        UV2 result;
        for (int k = 0; k < 2; ++k) {
            result[k] = (1.0 - plan.t) * a[k] + plan.t * b[k];
        }
        return result;
    };

    // Write the UV vector of one halfface.  The corner order always
    // follows the halfface's own current halfedge sequence, so for the
    // opposite side we must not reuse the first side's loop (its corner
    // sequence is rotated/reversed).  When _world_frame is set (interior
    // cap faces), every corner is expressed in the common world frame;
    // otherwise split-edge midpoint corners are linearly interpolated
    // from the origin halfface.
    auto write_halfface_uv = [&](HalfFaceHandle _hfh,
                                 HalfFaceHandle _origin_hfh,
                                 bool _world_frame) {
        if (!uv_prop) {
            return;
        }
        const auto halfedges = _mesh.halfface(_hfh).halfedges();
        std::vector<double> uv;
        uv.reserve(2 * halfedges.size());
        for (HalfEdgeHandle heh : halfedges) {
            VertexHandle start = _mesh.from_vertex_handle(heh);
            UV2 corner;
            if (_world_frame) {
                corner = cap_uv_of(start);
            } else {
                auto mid_it = mid_to_edge.find(start);
                if (mid_it != mid_to_edge.end()) {
                    corner = midpoint_uv(_origin_hfh, mid_it->second);
                } else {
                    corner = uv_snapshot.at(_origin_hfh).at(start);
                }
            }
            uv.push_back(corner[0]);
            uv.push_back(corner[1]);
        }
        uv_at(uv_prop, _hfh) = std::move(uv);
    };

    // -------------------------------------------------------------------
    // 2. Rewrite faces: insert midpoint vertices, divide cut faces.
    // -------------------------------------------------------------------
    std::map<FaceHandle, FaceSplit> face_splits;

    for (FaceHandle fh : affected_faces) {
        // Original vertex loop, captured before the edges were split.
        const std::vector<VertexHandle>& old_vertices =
            face_old_vertices.at(fh);
        const std::vector<EdgeHandle>& old_edges = face_old_edges.at(fh);

        // Expanded boundary walk with the midpoint inserted on every split
        // edge, right after the edge's start vertex.
        std::vector<VertexHandle> walk;
        std::vector<bool> is_mid;
        walk.reserve(old_vertices.size() + 2);
        is_mid.reserve(old_vertices.size() + 2);
        for (size_t i = 0; i < old_vertices.size(); ++i) {
            VertexHandle va = old_vertices[i];
            walk.push_back(va);
            is_mid.push_back(false);
            EdgeHandle eh = old_edges[i];
            auto split_it = edge_splits.find(eh);
            if (split_it != edge_splits.end()) {
                walk.push_back(split_it->second.mid);
                is_mid.push_back(true);
            }
        }

        auto make_loop = [&](const std::vector<VertexHandle>& _chain) {
            std::vector<HalfEdgeHandle> loop;
            loop.reserve(_chain.size());
            for (size_t i = 0; i < _chain.size(); ++i) {
                loop.push_back(oriented_halfedge(
                    _chain[i], _chain[(i + 1) % _chain.size()]));
            }
            return loop;
        };

        if (!divided_faces.count(fh)) {
            // Midpoint insertion only, the face handle is retained.
            std::vector<HalfEdgeHandle> loop = make_loop(walk);
            _mesh.set_face(fh, loop);
            write_halfface_uv(fh.h0(), fh.h0(), false);
            write_halfface_uv(fh.h1(), fh.h1(), false);
            continue;
        }

        // A convex planar face has exactly two plane crossings.
        std::vector<VertexHandle> mids;
        for (size_t i = 0; i < walk.size(); ++i) {
            if (is_mid[i]) {
                mids.push_back(walk[i]);
            }
        }
        if (mids.size() != 2) {
            throw std::logic_error(
                "partition_cells: internal error, cut face has != 2 crossings");
        }
        const VertexHandle mA = mids[0];
        const VertexHandle mB = mids[1];
        // add_edge reuses an existing edge if this cut segment was already
        // created by a neighbouring divided face.
        EdgeHandle cap_edge = _mesh.add_edge(mA, mB);

        size_t idx_a = std::find(walk.begin(), walk.end(), mA) - walk.begin();
        size_t idx_b = std::find(walk.begin(), walk.end(), mB) - walk.begin();
        if (idx_a > idx_b) {
            std::swap(idx_a, idx_b);
        }
        std::vector<VertexHandle> chain_a(
            walk.begin() + static_cast<long>(idx_a),
            walk.begin() + static_cast<long>(idx_b) + 1);
        std::vector<VertexHandle> chain_b;
        chain_b.reserve(walk.size() - chain_a.size() + 2);
        for (size_t i = idx_b; i < walk.size(); ++i) {
            chain_b.push_back(walk[i]);
        }
        for (size_t i = 0; i <= idx_a; ++i) {
            chain_b.push_back(walk[i]);
        }

        std::vector<HalfEdgeHandle> loop_a = make_loop(chain_a);
        std::vector<HalfEdgeHandle> loop_b = make_loop(chain_b);

        auto loop_sign = [&](const std::vector<HalfEdgeHandle>& _loop) {
            for (HalfEdgeHandle heh : _loop) {
                int s = side_of(_mesh.from_vertex_handle(heh));
                if (s != 0) {
                    return s;
                }
            }
            return 0;
        };
        std::vector<HalfEdgeHandle> neg_loop;
        std::vector<HalfEdgeHandle> pos_loop;
        if (loop_sign(loop_a) < 0) {
            neg_loop = std::move(loop_a);
            pos_loop = std::move(loop_b);
        } else {
            pos_loop = std::move(loop_a);
            neg_loop = std::move(loop_b);
        }

        // Original face handle keeps the negative child.
        _mesh.set_face(fh, neg_loop);
        FaceHandle pos_fh = _mesh.add_face(pos_loop);
        copy_all_properties<Entity::Face>(_mesh, fh, pos_fh);
        copy_all_properties<Entity::HalfFace>(_mesh, fh.h0(), pos_fh.h0());
        copy_all_properties<Entity::HalfFace>(_mesh, fh.h1(), pos_fh.h1());

        face_splits[fh] = FaceSplit{pos_fh, cap_edge};

        // Boundary child faces interpolate every inserted midpoint corner.
        write_halfface_uv(fh.h0(), fh.h0(), false);
        write_halfface_uv(fh.h1(), fh.h1(), false);
        write_halfface_uv(pos_fh.h0(), fh.h0(), false);
        write_halfface_uv(pos_fh.h1(), fh.h1(), false);
    }

    // -------------------------------------------------------------------
    // 3. Rewrite the cut cells and create their interior cap faces.
    // -------------------------------------------------------------------
    std::vector<CellPartition> result;
    result.reserve(cut_cells.size());

    std::set<EdgeHandle> edges_to_reorder;
    auto remember_face_edges = [&](FaceHandle _fh) {
        for (HalfEdgeHandle heh : _mesh.face(_fh).halfedges()) {
            edges_to_reorder.insert(heh.edge_handle());
        }
    };
    for (FaceHandle fh : affected_faces) {
        remember_face_edges(fh);
        auto it = face_splits.find(fh);
        if (it != face_splits.end()) {
            remember_face_edges(it->second.positive_face);
            edges_to_reorder.insert(it->second.cap_edge);
        }
    }

    for (CellHandle ch : cut_cells) {
        const std::vector<HalfFaceHandle> old_hfs = _mesh.cell(ch).halffaces();

        std::vector<EdgeHandle> cap_edges;
        for (HalfFaceHandle hfh : old_hfs) {
            auto it = face_splits.find(hfh.face_handle());
            if (it != face_splits.end()) {
                cap_edges.push_back(it->second.cap_edge);
                edges_to_reorder.insert(it->second.cap_edge);
            }
        }

        // Order the cap edges into one polygon cycle.
        std::vector<VertexHandle> cap_cycle;
        {
            std::unordered_map<VertexHandle, std::vector<EdgeHandle>> at_vertex;
            for (EdgeHandle ce : cap_edges) {
                const auto eps = _mesh.edge_vertices(ce);
                at_vertex[eps[0]].push_back(ce);
                at_vertex[eps[1]].push_back(ce);
            }
            EdgeHandle first = cap_edges.front();
            const auto first_eps = _mesh.edge_vertices(first);
            VertexHandle start = first_eps[0];
            VertexHandle cur = first_eps[1];
            cap_cycle.push_back(start);
            std::set<EdgeHandle> used{first};
            while (cur != start) {
                cap_cycle.push_back(cur);
                EdgeHandle next = EdgeHandle(-1);
                for (EdgeHandle cand : at_vertex[cur]) {
                    if (!used.count(cand)) {
                        next = cand;
                        break;
                    }
                }
                if (!next.is_valid()) {
                    throw std::logic_error(
                        "partition_cells: internal error, cap cycle broken");
                }
                used.insert(next);
                const auto eps = _mesh.edge_vertices(next);
                cur = (eps[0] == cur) ? eps[1] : eps[0];
            }
        }

        auto loop_of = [&](const std::vector<VertexHandle>& _cycle) {
            std::vector<HalfEdgeHandle> loop;
            loop.reserve(_cycle.size());
            for (size_t i = 0; i < _cycle.size(); ++i) {
                loop.push_back(oriented_halfedge(
                    _cycle[i], _cycle[(i + 1) % _cycle.size()]));
            }
            return loop;
        };
        std::vector<HalfEdgeHandle> cap_loop = loop_of(cap_cycle);

        // Halfface 0 of the cap must wind towards the positive side (+n),
        // because the positive cell references it and halfface normals
        // point into the incident cell.
        const Vec3d winding =
            (_mesh.vertex(cap_cycle[1]) - _mesh.vertex(cap_cycle[0]))
                .cross(_mesh.vertex(cap_cycle[2]) - _mesh.vertex(cap_cycle[1]));
        if (winding.dot(n) < 0.0) {
            std::reverse(cap_cycle.begin(), cap_cycle.end());
            cap_loop = loop_of(cap_cycle);
        }

        FaceHandle cap_fh = _mesh.add_face(cap_loop);
        HalfFaceHandle cap_h0 = cap_fh.h0();
        HalfFaceHandle cap_h1 = cap_fh.h1();
        for (HalfEdgeHandle heh : cap_loop) {
            edges_to_reorder.insert(heh.edge_handle());
        }

        if (uv_prop) {
            write_halfface_uv(cap_h0, cap_h0, true);
            write_halfface_uv(cap_h1, cap_h1, true);
        }

        // Assemble the two cell shells, preserving the sub-index (and thus
        // the outward/inward orientation) of every inherited halfface.
        std::vector<HalfFaceHandle> neg_hfs;
        std::vector<HalfFaceHandle> pos_hfs;
        neg_hfs.reserve(old_hfs.size() + 1);
        pos_hfs.reserve(old_hfs.size() + 1);
        for (HalfFaceHandle hfh : old_hfs) {
            FaceHandle fh = hfh.face_handle();
            const unsigned char side = hfh.subidx();
            auto it = face_splits.find(fh);
            if (it == face_splits.end()) {
                // Uncut boundary face: assign it to the side it lies on.
                int s = 0;
                for (VertexHandle vh : _mesh.face_vertices(fh)) {
                    s = side_of(vh);
                    if (s != 0) {
                        break;
                    }
                }
                if (s < 0) {
                    neg_hfs.push_back(fh.halfface_handle(side));
                } else {
                    pos_hfs.push_back(fh.halfface_handle(side));
                }
            } else {
                neg_hfs.push_back(fh.halfface_handle(side));
                pos_hfs.push_back(
                    it->second.positive_face.halfface_handle(side));
            }
        }
        neg_hfs.push_back(cap_h1); // normal points into the negative cell
        pos_hfs.push_back(cap_h0); // normal points into the positive cell

        _mesh.set_cell(ch, neg_hfs);
        CellHandle pos_ch = _mesh.add_cell(pos_hfs);
        copy_all_properties<Entity::Cell>(_mesh, ch, pos_ch);

        CellPartition record;
        record.parent = ch;
        record.negative = ch;
        record.positive = pos_ch;
        record.cap = cap_fh;
        result.push_back(record);
    }

    // -------------------------------------------------------------------
    // 4. Unselected neighbouring cells keep their single body, but their
    //    halfface list must reference the two child faces of every
    //    divided face they were incident to.
    // -------------------------------------------------------------------
    std::set<CellHandle> cut_set(cut_cells.begin(), cut_cells.end());
    std::set<CellHandle> neighbours;
    for (const auto& entry : face_splits) {
        const auto cells = _mesh.face_cells(entry.first);
        for (CellHandle c : cells) {
            if (c.is_valid() && !_mesh.is_deleted(c) && !cut_set.count(c)) {
                neighbours.insert(c);
            }
        }
    }
    for (CellHandle ch : neighbours) {
        std::vector<HalfFaceHandle> new_hfs;
        new_hfs.reserve(_mesh.cell(ch).halffaces().size() + 2);
        for (HalfFaceHandle hfh : _mesh.cell(ch).halffaces()) {
            FaceHandle fh = hfh.face_handle();
            auto it = face_splits.find(fh);
            if (it == face_splits.end()) {
                new_hfs.push_back(hfh);
            } else {
                // The uncut neighbour keeps the entire boundary region,
                // which is now represented by both child faces.  The
                // sub-index preserves the side pointing into the cell.
                new_hfs.push_back(fh.halfface_handle(hfh.subidx()));
                new_hfs.push_back(
                    it->second.positive_face.halfface_handle(hfh.subidx()));
            }
        }
        _mesh.set_cell(ch, new_hfs);
    }

    // -------------------------------------------------------------------
    // 5. Rebuild the cyclic halfface ordering around every touched edge,
    //    now that all cell incidences are consistent again.
    // -------------------------------------------------------------------
    for (EdgeHandle eh : edges_to_reorder) {
        if (!_mesh.is_deleted(eh)) {
            _mesh.reorder_incident_halffaces(eh);
        }
    }

    std::sort(result.begin(), result.end(),
              [](const CellPartition& _a, const CellPartition& _b) {
                  return _a.parent < _b.parent;
              });
    return result;
}

} // namespace OpenVolumeMesh
