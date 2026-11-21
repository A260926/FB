// Regression tests for OpenVolumeMesh::partition_cells.
//
// Registered through task_support/check.h in three groups:
//   closure  - shared topology, oriented closed shells, circulators, errors
//   geometry - positive volumes, conservation, cap placement/planarity
//   corners  - properties, hf:uv interpolation/world frame, IO and compression
//
// The file defines no main(); task_support/driver.c provides it.

#include "task_support/check.h"

#include <OpenVolumeMesh/Unstable/Topology/PlanePartition.hh>
#include <OpenVolumeMesh/Attribs/StatusAttrib.hh>
#include <OpenVolumeMesh/FileManager/FileManager.hh>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace OpenVolumeMesh;
using Vec = Geometry::Vec3d;

namespace {

constexpr double kEps = 1e-9;

// Face vertex loops of a unit hexahedron (winding is fixed up by
// inward_halfface geometrically).
const int kHexFaces[6][4] = {
    {3, 2, 1, 0}, {7, 6, 5, 4}, {1, 2, 6, 7},
    {4, 5, 3, 0}, {1, 7, 4, 0}, {2, 3, 5, 6},
};

HalfFaceHandle inward_halfface(GeometricPolyhedralMeshV3d& _mesh,
                               FaceHandle _fh, const Vec& _center) {
    const auto vs = _mesh.get_halfface_vertices(_fh.h0());
    Vec fc(0.0, 0.0, 0.0);
    for (VertexHandle vh : vs) {
        fc += _mesh.vertex(vh);
    }
    fc *= 1.0 / static_cast<double>(vs.size());
    const Vec winding =
        (_mesh.vertex(vs[1]) - _mesh.vertex(vs[0]))
            .cross(_mesh.vertex(vs[2]) - _mesh.vertex(vs[1]));
    return winding.dot(_center - fc) > 0.0 ? _fh.h0() : _fh.h1();
}

FaceHandle add_polygon_face(GeometricPolyhedralMeshV3d& _mesh,
                            const std::vector<VertexHandle>& _loop) {
    const HalfFaceHandle found = _mesh.find_halfface(_loop);
    if (found.is_valid()) {
        return found.face_handle();
    }
    return _mesh.add_face(_loop);
}

CellHandle add_hexahedron(GeometricPolyhedralMeshV3d& _mesh,
                          const std::vector<VertexHandle>& _v) {
    Vec center(0.0, 0.0, 0.0);
    for (VertexHandle vh : _v) {
        center += _mesh.vertex(vh);
    }
    center *= 0.125;

    std::vector<HalfFaceHandle> hfs;
    hfs.reserve(6);
    for (const auto& f : kHexFaces) {
        const std::vector<VertexHandle> loop{
            _v[f[0]], _v[f[1]], _v[f[2]], _v[f[3]]};
        const FaceHandle fh = add_polygon_face(_mesh, loop);
        hfs.push_back(inward_halfface(_mesh, fh, center));
    }
    return _mesh.add_cell(hfs);
}

// Oriented closed shell: every used halfedge appears with both
// orientations exactly once, and every halfface is incident to the cell.
bool check_closed_shell(GeometricPolyhedralMeshV3d& _mesh, CellHandle _ch) {
    std::map<HalfEdgeHandle, int> counts;
    size_t n_hfs = 0;
    for (HalfFaceHandle hfh : _mesh.cell_halffaces(_ch)) {
        if (_mesh.incident_cell(hfh) != _ch) {
            return false;
        }
        ++n_hfs;
        for (HalfEdgeHandle heh : _mesh.halfface(hfh).halfedges()) {
            ++counts[heh];
        }
    }
    if (n_hfs < 4) {
        return false;
    }
    for (const auto& entry : counts) {
        if (entry.second != 1) {
            return false;
        }
        const auto opp = entry.first.opposite_handle();
        const auto it = counts.find(opp);
        if (it == counts.end() || it->second != 1) {
            return false;
        }
    }
    return true;
}

double polyhedron_volume(GeometricPolyhedralMeshV3d& _mesh, CellHandle _ch) {
    double volume = 0.0;
    for (HalfFaceHandle hfh : _mesh.cell_halffaces(_ch)) {
        const auto vs = _mesh.get_halfface_vertices(hfh);
        for (size_t i = 1; i + 1 < vs.size(); ++i) {
            const Vec& a = _mesh.vertex(vs[0]);
            const Vec& b = _mesh.vertex(vs[i]);
            const Vec& c = _mesh.vertex(vs[i + 1]);
            volume -= a.dot(b.cross(c)) / 6.0; // inward winding
        }
    }
    return volume;
}

double total_volume(GeometricPolyhedralMeshV3d& _mesh) {
    double sum = 0.0;
    for (CellHandle ch : _mesh.cells()) {
        if (!_mesh.is_deleted(ch)) {
            const double v = polyhedron_volume(_mesh, ch);
            if (!(v > 0.0)) {
                return std::numeric_limits<double>::quiet_NaN();
            }
            sum += v;
        }
    }
    return sum;
}

bool manifold_face_incidences(GeometricPolyhedralMeshV3d& _mesh) {
    for (FaceHandle fh : _mesh.faces()) {
        if (_mesh.is_deleted(fh)) {
            continue;
        }
        const auto cells = _mesh.face_cells(fh);
        int n = 0;
        for (CellHandle ch : cells) {
            if (ch.is_valid()) {
                ++n;
            }
        }
        if (n > 2) {
            return false;
        }
    }
    return true;
}

// Every edge->halfface circulator must be usable, and the two directed
// halfedge incidence lists must be exact opposites of each other.
bool circulators_consistent(GeometricPolyhedralMeshV3d& _mesh) {
    for (EdgeHandle eh : _mesh.edges()) {
        if (_mesh.is_deleted(eh)) {
            continue;
        }
        std::set<HalfFaceHandle> l0;
        std::set<HalfFaceHandle> l1;
        for (HalfFaceHandle hfh : _mesh.halfedge_halffaces(eh.h0())) {
            l0.insert(hfh);
        }
        for (HalfFaceHandle hfh : _mesh.halfedge_halffaces(eh.h1())) {
            l1.insert(hfh);
        }
        if (l0.size() != l1.size()) {
            return false;
        }
        std::set<HalfFaceHandle> opp_l1;
        for (HalfFaceHandle hfh : l1) {
            opp_l1.insert(hfh.opposite_handle());
        }
        if (l0 != opp_l1) {
            return false;
        }
        // The union view exposed by edge_halffaces must contain exactly
        // the two sides of every halfface incident to the edge.
        std::set<HalfFaceHandle> edge_view;
        for (HalfFaceHandle hfh : _mesh.edge_halffaces(eh)) {
            edge_view.insert(hfh);
        }
        std::set<HalfFaceHandle> expected = l0;
        for (HalfFaceHandle hfh : l0) {
            expected.insert(hfh.opposite_handle());
        }
        if (edge_view != expected) {
            return false;
        }
    }
    // Within every cell, the adjacency walk along each halfface edge must
    // lead to another halfface of the same cell (this is what the cyclic
    // edge ordering is built from).
    for (CellHandle ch : _mesh.cells()) {
        if (_mesh.is_deleted(ch)) {
            continue;
        }
        std::set<HalfFaceHandle> cell_hfs;
        for (HalfFaceHandle hfh : _mesh.cell_halffaces(ch)) {
            cell_hfs.insert(hfh);
        }
        for (HalfFaceHandle hfh : cell_hfs) {
            for (HalfEdgeHandle heh : _mesh.halfface(hfh).halfedges()) {
                const HalfFaceHandle adjacent =
                    _mesh.adjacent_halfface_in_cell(hfh, heh);
                if (!adjacent.is_valid() || !cell_hfs.count(adjacent)) {
                    return false;
                }
            }
        }
    }
    return true;
}

// Two unit cubes [0,1]^3 and [1,2]x[0,1]x[0,1], sharing the x=1 quad.
struct DoubleCube {
    GeometricPolyhedralMeshV3d mesh;
    CellHandle left;
    CellHandle right;
    FaceHandle shared_face;
};

DoubleCube make_double_cube() {
    DoubleCube dc;
    auto& m = dc.mesh;
    m.enable_bottom_up_incidences(true);
    m.enable_deferred_deletion(true);

    for (int x = 0; x <= 2; ++x) {
        for (int y = 0; y <= 1; ++y) {
            for (int z = 0; z <= 1; ++z) {
                m.add_vertex(Vec(static_cast<double>(x),
                                 static_cast<double>(y),
                                 static_cast<double>(z)));
            }
        }
    }
    auto vidx = [](int x, int y, int z) {
        return VertexHandle(x * 4 + y * 2 + z);
    };
    std::vector<VertexHandle> lv(8), rv(8);
    for (int i = 0; i < 8; ++i) {
        const int x = (i == 1 || i == 2 || i == 6 || i == 7) ? 1 : 0;
        const int y = (i == 2 || i == 3 || i == 5 || i == 6) ? 1 : 0;
        const int z = (i >= 4) ? 1 : 0;
        lv[i] = vidx(x, y, z);
        rv[i] = vidx(x + 1, y, z);
    }
    dc.left = add_hexahedron(m, lv);
    dc.right = add_hexahedron(m, rv);

    dc.shared_face = FaceHandle(-1);
    for (FaceHandle fh : m.faces()) {
        const auto cells = m.face_cells(fh);
        if (cells[0].is_valid() && cells[1].is_valid()) {
            dc.shared_face = fh;
            break;
        }
    }
    return dc;
}

// A copyable custom property type exercising user-defined types.
struct CustomData {
    int id = 0;
    double weight = 0.0;
    std::vector<int> tags;
    bool operator==(const CustomData& _other) const {
        return id == _other.id && weight == _other.weight && tags == _other.tags;
    }
};

} // namespace

//=========================================================================
// closure group
//=========================================================================

// Two cubes sharing a face; only one side is selected.
static int closure_double_cube_one_side() {
    DoubleCube dc = make_double_cube();
    auto& m = dc.mesh;

    TASK_CHECK(dc.shared_face.is_valid());
    const size_t v_before = m.n_vertices();
    (void)v_before;

    const auto records =
        partition_cells(m, {dc.left, dc.left, dc.left}, Vec(1.0, 0.0, 0.0), -0.5);
    TASK_CHECK(records.size() == 1); // duplicate selection acts as a set
    const auto& r = records.front();
    TASK_CHECK(r.parent == dc.left);
    TASK_CHECK(r.negative == dc.left); // parent handle kept on negative side
    TASK_CHECK(r.positive != dc.left);
    TASK_CHECK(r.cap.is_valid());

    // The cap is the face truly shared by the two child cells.
    const auto cap_cells = m.face_cells(r.cap);
    TASK_CHECK((cap_cells[0] == r.negative && cap_cells[1] == r.positive) ||
               (cap_cells[1] == r.negative && cap_cells[0] == r.positive));
    TASK_CHECK(m.incident_cell(r.cap.h0()) !=
               m.incident_cell(r.cap.h1()));

    // All three cells are closed, oriented, manifold shells.
    for (CellHandle ch : m.cells()) {
        if (m.is_deleted(ch)) {
            continue;
        }
        TASK_CHECK(check_closed_shell(m, ch));
    }
    TASK_CHECK(manifold_face_incidences(m));
    TASK_CHECK(circulators_consistent(m));

    // The unselected neighbour remains a single cell whose halfface list
    // was rewritten to the two child faces of the shared boundary face.
    TASK_CHECK(!m.is_deleted(dc.right));
    TASK_CHECK(m.valence(dc.right) == 6);
    std::set<FaceHandle> neighbour_faces;
    for (HalfFaceHandle hfh : m.cell_halffaces(dc.right)) {
        neighbour_faces.insert(hfh.face_handle());
    }
    // Original shared face handle survives as the negative-side child and
    // is still referenced by the unselected neighbour.
    TASK_CHECK(neighbour_faces.count(dc.shared_face) == 1);
    // The neighbour also references the positive-side sibling face that
    // shares a boundary with one of the two child cells.
    bool sees_positive_child = false;
    for (CellHandle ch : {r.negative, r.positive}) {
        for (HalfFaceHandle hfh : m.cell_halffaces(ch)) {
            if (neighbour_faces.count(hfh.face_handle())) {
                if (ch == r.positive) {
                    sees_positive_child = true;
                }
            }
        }
    }
    TASK_CHECK(sees_positive_child);

    return 0;
}

// Four cubes around one central edge; only two of them are selected.
static int closure_shared_edge_partial_selection() {
    GeometricPolyhedralMeshV3d m;
    m.enable_bottom_up_incidences(true);
    m.enable_deferred_deletion(true);

    for (int x = 0; x <= 2; ++x) {
        for (int y = 0; y <= 2; ++y) {
            for (int z = 0; z <= 1; ++z) {
                m.add_vertex(Vec(static_cast<double>(x),
                                 static_cast<double>(y),
                                 static_cast<double>(z)));
            }
        }
    }
    auto vidx = [](int x, int y, int z) {
        return VertexHandle(x * 6 + y * 2 + z);
    };
    auto hex_verts = [&](int ox, int oy) {
        std::vector<VertexHandle> v(8);
        for (int i = 0; i < 8; ++i) {
            const int dx = (i == 1 || i == 2 || i == 6 || i == 7) ? 1 : 0;
            const int dy = (i == 2 || i == 3 || i == 5 || i == 6) ? 1 : 0;
            const int dz = (i >= 4) ? 1 : 0;
            v[i] = vidx(ox + dx, oy + dy, dz);
        }
        return v;
    };
    // The central vertical edge x=1,y=1 is shared by all four cubes.
    const CellHandle c00 = add_hexahedron(m, hex_verts(0, 0));
    const CellHandle c10 = add_hexahedron(m, hex_verts(1, 0));
    const CellHandle c11 = add_hexahedron(m, hex_verts(1, 1));
    const CellHandle c01 = add_hexahedron(m, hex_verts(0, 1));

    const VertexHandle central_low = vidx(1, 1, 0);
    const VertexHandle central_high = vidx(1, 1, 1);
    const HalfEdgeHandle central_he =
        m.find_halfedge(central_low, central_high);
    TASK_CHECK(central_he.is_valid());
    const EdgeHandle central_edge = central_he.edge_handle();

    // Plane x = 1.5 cuts the two x-positive cubes only (diagonal selection
    // relative to the central edge fan).
    const auto records = partition_cells(
        m, {c10, c11}, Vec(1.0, 0.0, 0.0), -1.5);
    TASK_CHECK(records.size() == 2);

    // The central edge is not crossed by the plane, so it stays intact,
    // but every face around it remains usable through circulators.
    TASK_CHECK(circulators_consistent(m));
    TASK_CHECK(manifold_face_incidences(m));
    for (CellHandle ch : m.cells()) {
        if (!m.is_deleted(ch)) {
            TASK_CHECK(check_closed_shell(m, ch));
        }
    }

    // Unselected cells keep their handles and stay single cells.
    TASK_CHECK(!m.is_deleted(c00) && !m.is_deleted(c01));
    TASK_CHECK(m.valence(c00) == 6 && m.valence(c01) == 6);

    // Both selected cells were split and share no cap face with each other.
    TASK_CHECK(records[0].cap != records[1].cap);
    TASK_CHECK(records[0].parent < records[1].parent); // sorted output

    // The untouched central edge still has its original endpoint direction.
    const auto endpoints = m.edge_vertices(central_edge);
    TASK_CHECK(endpoints[0] == central_low && endpoints[1] == central_high);
    return 0;
}

// Both adjacent cells are selected and the plane cuts straight through
// their shared interface: every crossed original edge still produces one
// intersection vertex, and the two interior cap faces share edges.
static int closure_adjacent_cells_share_cut_face() {
    DoubleCube dc = make_double_cube();
    auto& m = dc.mesh;
    const FaceHandle shared = dc.shared_face;

    const size_t vertices_before = m.n_vertices();
    const size_t edges_before = m.n_edges();

    const auto records = partition_cells(
        m, {dc.left, dc.right}, Vec(0.0, 1.0, 0.0), -0.5);
    TASK_CHECK(records.size() == 2);

    // Each cube has four y-directed edges; the two at the x = 1 interface
    // are shared, so six distinct original edges are crossed, each exactly
    // once: six new intersection vertices and six new sub-edges.  The two
    // rectangular cap faces (four edges each) meet along the single cut
    // segment at x = 1, giving seven distinct new cap edges.
    TASK_CHECK(m.n_vertices() == vertices_before + 6);
    TASK_CHECK(m.n_edges() == edges_before + 6 + 7);

    // The shared interface was divided exactly once: its handle survives
    // as one child and exactly one sibling face of the same size exists.
    const auto shared_vs = m.get_halfface_vertices(shared.h0());
    TASK_CHECK(shared_vs.size() == 4);
    bool sibling_found = false;
    for (FaceHandle fh : m.faces()) {
        if (fh == shared || m.is_deleted(fh)) {
            continue;
        }
        const auto vs = m.get_halfface_vertices(fh.h0());
        if (vs.size() != shared_vs.size()) {
            continue;
        }
        bool on_interface = true;
        for (VertexHandle vh : vs) {
            if (std::abs(m.vertex(vh)[0] - 1.0) > 1e-12) {
                on_interface = false;
            }
        }
        if (on_interface) {
            sibling_found = true;
        }
    }
    TASK_CHECK(sibling_found);

    // The two cap faces share at least one edge (the cut segment inside
    // the common interface face).
    std::set<EdgeHandle> cap0_edges;
    for (HalfEdgeHandle heh : m.face(records[0].cap).halfedges()) {
        cap0_edges.insert(heh.edge_handle());
    }
    bool shared_cap_edge = false;
    for (HalfEdgeHandle heh : m.face(records[1].cap).halfedges()) {
        if (cap0_edges.count(heh.edge_handle())) {
            shared_cap_edge = true;
        }
    }
    TASK_CHECK(shared_cap_edge);

    for (CellHandle ch : m.cells()) {
        if (!m.is_deleted(ch)) {
            TASK_CHECK(check_closed_shell(m, ch));
        }
    }
    TASK_CHECK(manifold_face_incidences(m));
    TASK_CHECK(circulators_consistent(m));
    TASK_CHECK(std::abs(total_volume(m) - 2.0) < 1e-10);

    // Edge splitting rule: every crossed y-directed edge yields one
    // midpoint vertex at y = 0.5, and the two sub-edges chain from the
    // original low-y vertex through the midpoint to the high-y vertex.
    const double plane_y = 0.5;
    size_t split_edges_checked = 0;
    for (CellHandle ch : {dc.left, dc.right}) {
        for (VertexHandle vh : m.cell_vertices(ch)) {
            if (std::abs(m.vertex(vh)[1] - plane_y) > 1e-12) {
                continue;
            }
            // This is an intersection vertex.  It must have one sub-edge
            // going to a low-y vertex and one to a high-y vertex, and the
            // low-y sub-edge must keep the direction towards the midpoint.
            bool has_low = false;
            bool has_high = false;
            for (VertexHandle neighbour : m.vertex_vertices(vh)) {
                const double ny = m.vertex(neighbour)[1];
                if (ny < plane_y) {
                    has_low = true;
                    // The halfedge from the low vertex towards the midpoint
                    // must exist with the inherited direction.
                    const HalfEdgeHandle up =
                        m.find_halfedge(neighbour, vh);
                    TASK_CHECK(up.is_valid());
                    TASK_CHECK(m.to_vertex_handle(up) == vh);
                } else if (ny > plane_y) {
                    has_high = true;
                }
            }
            TASK_CHECK(has_low && has_high);
            ++split_edges_checked;
        }
    }
    TASK_CHECK(split_edges_checked >= 6);
    return 0;
}

// Error handling: rejected calls leave the mesh editable and unchanged.
static int closure_rejections_keep_mesh_editable() {
    DoubleCube dc = make_double_cube();
    auto& m = dc.mesh;

    // Empty selection is a no-op without any validation side effects.
    const auto empty = partition_cells(m, {}, Vec(1.0, 0.0, 0.0), 0.0);
    TASK_CHECK(empty.empty());
    TASK_CHECK(m.n_cells() == 2);

    const size_t nv = m.n_vertices();
    const size_t ne = m.n_edges();
    const size_t nf = m.n_faces();
    const size_t nc = m.n_cells();

    auto expect_invalid = [&](std::vector<CellHandle> sel, Vec normal,
                              double offset) -> int {
        bool threw = false;
        try {
            partition_cells(m, std::move(sel), normal, offset);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        TASK_CHECK(threw);
        return 0;
    };
    expect_invalid({dc.left}, Vec(0.0, 0.0, 0.0), 0.0);          // zero normal
    expect_invalid({dc.left}, Vec(1.0, 0.0, 0.0),
                   std::numeric_limits<double>::quiet_NaN());    // non-finite
    expect_invalid({CellHandle(9999)}, Vec(1.0, 0.0, 0.0), -0.5); // bad handle
    expect_invalid({dc.left}, Vec(1.0, 0.0, 0.0), 0.0);          // through vertex
    expect_invalid({dc.left}, Vec(1.0, 0.0, 0.0), -1.0);         // coplanar face

    // A plane through an edge (but no vertex) is rejected as well.
    expect_invalid({dc.left}, Vec(0.0, 1.0, 0.0), 0.0);
    expect_invalid({dc.left}, Vec(0.0, 0.0, 1.0), -1.0);

    // Malformed hf:uv properties are rejected before any modification.
    {
        auto bad_uv =
            m.request_halfface_property<std::vector<double>>("hf:uv", {});
        bad_uv[HalfFaceHandle(0)] = {1.0, 2.0}; // valence 4 expects 8 values
        bool threw = false;
        try {
            partition_cells(m, {dc.left}, Vec(1.0, 0.0, 0.0), -0.5);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        TASK_CHECK(threw);
        bad_uv[HalfFaceHandle(0)] = {
            std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0,
            0.0, 0.0, 0.0, 0.0};
        threw = false;
        try {
            partition_cells(m, {dc.left}, Vec(1.0, 0.0, 0.0), -0.5);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        TASK_CHECK(threw);
    }

    TASK_CHECK(m.n_vertices() == nv);
    TASK_CHECK(m.n_edges() == ne);
    TASK_CHECK(m.n_faces() == nf);
    TASK_CHECK(m.n_cells() == nc);
    TASK_CHECK(!m.needs_garbage_collection());

    // The mesh must still be editable after the rejected calls.
    const auto records =
        partition_cells(m, {dc.left}, Vec(1.0, 0.0, 0.0), -0.5);
    TASK_CHECK(records.size() == 1);
    TASK_CHECK(check_closed_shell(m, records.front().negative));
    TASK_CHECK(check_closed_shell(m, records.front().positive));
    TASK_CHECK(circulators_consistent(m));
    return 0;
}

//=========================================================================
// geometry group
//=========================================================================

static int geometry_double_cube_volume_conservation() {
    DoubleCube dc = make_double_cube();
    auto& m = dc.mesh;
    const double before = total_volume(m);
    TASK_CHECK(std::abs(before - 2.0) < 1e-12);

    const auto records =
        partition_cells(m, {dc.left}, Vec(2.0, 0.0, 0.0), -1.0); // x = 0.5
    TASK_CHECK(records.size() == 1);
    const auto& r = records.front();

    const double vn = polyhedron_volume(m, r.negative);
    const double vp = polyhedron_volume(m, r.positive);
    TASK_CHECK(vn > 0.0 && vp > 0.0);
    TASK_CHECK(std::abs(vn - 0.5) < 1e-10);
    TASK_CHECK(std::abs(vp - 0.5) < 1e-10);
    TASK_CHECK(std::abs(total_volume(m) - before) < 1e-10);

    // Every cap vertex lies on the plane and the cap winding faces +n.
    const Vec n = Vec(1.0, 0.0, 0.0);
    for (VertexHandle vh : m.face_vertices(r.cap)) {
        TASK_CHECK(std::abs(n.dot(m.vertex(vh)) - 0.5) < 1e-10);
    }
    const auto cap_vs = m.get_halfface_vertices(r.cap.h0());
    const Vec cap_winding =
        (m.vertex(cap_vs[1]) - m.vertex(cap_vs[0]))
            .cross(m.vertex(cap_vs[2]) - m.vertex(cap_vs[1]));
    TASK_CHECK(cap_winding.dot(n) > 0.0);

    // The unselected right cube keeps its full unit volume.
    TASK_CHECK(std::abs(polyhedron_volume(m, dc.right) - 1.0) < 1e-10);
    return 0;
}

// Mixed cell types: a hexahedron with a triangulated side face and a
// tetrahedron glued to one of the triangles, cut by an oblique plane.
static int geometry_oblique_mixed_cells() {
    GeometricPolyhedralMeshV3d m;
    m.enable_bottom_up_incidences(true);
    m.enable_deferred_deletion(true);

    std::vector<VertexHandle> h(8);
    for (int i = 0; i < 8; ++i) {
        const double x = (i == 1 || i == 2 || i == 6 || i == 7) ? 1.0 : 0.0;
        const double y = (i == 2 || i == 3 || i == 5 || i == 6) ? 1.0 : 0.0;
        const double z = (i >= 4) ? 1.0 : 0.0;
        h[i] = m.add_vertex(Vec(x, y, z));
    }
    const VertexHandle apex = m.add_vertex(Vec(-1.0, 0.5, 0.5));

    // Hexahedron with the x=0 quad replaced by two triangles.
    Vec hex_center(0.5, 0.5, 0.5);
    std::vector<HalfFaceHandle> hex_hfs;
    auto take = [&](FaceHandle fh) {
        hex_hfs.push_back(inward_halfface(m, fh, hex_center));
    };
    take(add_polygon_face(m, {h[0], h[3], h[5]}));
    take(add_polygon_face(m, {h[0], h[5], h[4]}));
    take(add_polygon_face(m, {h[1], h[7], h[6], h[2]}));
    take(add_polygon_face(m, {h[0], h[4], h[7], h[1]}));
    take(add_polygon_face(m, {h[3], h[2], h[6], h[5]}));
    take(add_polygon_face(m, {h[0], h[1], h[2], h[3]}));
    take(add_polygon_face(m, {h[4], h[5], h[6], h[7]}));
    const CellHandle hex_cell = m.add_cell(hex_hfs);

    // Tetrahedron sharing triangle (0,3,5), apex on the negative x side.
    const FaceHandle shared_tri = add_polygon_face(m, {h[0], h[3], h[5]});
    const Vec tet_center = (m.vertex(apex) + m.vertex(h[0]) +
                            m.vertex(h[3]) + m.vertex(h[5])) * 0.25;
    std::vector<HalfFaceHandle> tet_hfs;
    // Shared triangle: halfface pointing into the tet.
    {
        const HalfFaceHandle h0 = shared_tri.h0();
        const HalfFaceHandle in =
            m.normal(h0).dot(tet_center - m.barycenter(shared_tri)) > 0.0
                ? h0 : h0.opposite_handle();
        tet_hfs.push_back(in);
    }
    for (auto loop : std::vector<std::vector<VertexHandle>>{
             {apex, h[0], h[3]},
             {apex, h[3], h[5]},
             {apex, h[5], h[0]}}) {
        const FaceHandle fh = add_polygon_face(m, loop);
        tet_hfs.push_back(inward_halfface(m, fh, tet_center));
    }
    const CellHandle tet_cell = m.add_cell(tet_hfs);

    TASK_CHECK(check_closed_shell(m, hex_cell));
    TASK_CHECK(check_closed_shell(m, tet_cell));
    const double before = total_volume(m);

    // Oblique, non-unit normal: plane 2(x+y+z) - 3 = 0.
    const auto records = partition_cells(
        m, {hex_cell, tet_cell}, Vec(2.0, 2.0, 2.0), -3.0);
    TASK_CHECK(records.size() == 2);
    for (const auto& r : records) {
        TASK_CHECK(polyhedron_volume(m, r.negative) > 0.0);
        TASK_CHECK(polyhedron_volume(m, r.positive) > 0.0);
        TASK_CHECK(check_closed_shell(m, r.negative));
        TASK_CHECK(check_closed_shell(m, r.positive));
        // Cap planarity in the normalized plane.
        for (VertexHandle vh : m.face_vertices(r.cap)) {
            const double d = m.vertex(vh)[0] + m.vertex(vh)[1] +
                             m.vertex(vh)[2] - 1.5;
            TASK_CHECK(std::abs(d) < 1e-9);
        }
    }
    TASK_CHECK(std::abs(total_volume(m) - before) < 1e-9);
    TASK_CHECK(circulators_consistent(m));
    return 0;
}

// A disconnected far component stays untouched; two successive partitions
// with different planes keep the volume conserved.
static int geometry_far_component_and_double_partition() {
    GeometricPolyhedralMeshV3d m;
    m.enable_bottom_up_incidences(true);
    m.enable_deferred_deletion(true);

    auto make_cube = [&](const Vec& _origin) {
        std::vector<VertexHandle> v(8);
        for (int i = 0; i < 8; ++i) {
            const double dx = (i == 1 || i == 2 || i == 6 || i == 7) ? 1.0 : 0.0;
            const double dy = (i == 2 || i == 3 || i == 5 || i == 6) ? 1.0 : 0.0;
            const double dz = (i >= 4) ? 1.0 : 0.0;
            v[i] = m.add_vertex(_origin + Vec(dx, dy, dz));
        }
        return add_hexahedron(m, v);
    };
    const CellHandle near_cell = make_cube(Vec(0.0, 0.0, 0.0));
    const CellHandle far_cell = make_cube(Vec(10.0, 0.0, 0.0));

    const std::set<VertexHandle> far_verts{[&] {
        std::set<VertexHandle> s;
        for (VertexHandle vh : m.cell_vertices(far_cell)) {
            s.insert(vh);
        }
        return s;
    }()};

    const double before = total_volume(m);
    const auto first = partition_cells(
        m, {near_cell}, Vec(1.0, 0.0, 0.0), -0.5);
    TASK_CHECK(first.size() == 1);

    // The far component is completely unchanged in handles and geometry.
    TASK_CHECK(!m.is_deleted(far_cell));
    TASK_CHECK(m.valence(far_cell) == 6);
    for (VertexHandle vh : far_verts) {
        TASK_CHECK(m.vertex(vh)[0] >= 10.0 - kEps);
    }
    TASK_CHECK(std::abs(total_volume(m) - before) < 1e-10);

    // Second, different plane (y = 0.5) cuts the positive child again and
    // also the far cell when selected.
    const CellHandle positive_child = first.front().positive;
    const auto second = partition_cells(
        m, {positive_child, far_cell}, Vec(0.0, 1.0, 0.0), -0.5);
    TASK_CHECK(second.size() == 2);
    for (const auto& r : second) {
        TASK_CHECK(polyhedron_volume(m, r.negative) > 0.0);
        TASK_CHECK(polyhedron_volume(m, r.positive) > 0.0);
        TASK_CHECK(check_closed_shell(m, r.negative));
        TASK_CHECK(check_closed_shell(m, r.positive));
    }
    TASK_CHECK(std::abs(total_volume(m) - before) < 1e-10);
    TASK_CHECK(circulators_consistent(m));
    TASK_CHECK(manifold_face_incidences(m));
    return 0;
}

//=========================================================================
// corners group
//=========================================================================

static int corners_uv_interpolation_and_distinct_sides() {
    DoubleCube dc = make_double_cube();
    auto& m = dc.mesh;

    auto uv = m.request_halfface_property<std::vector<double>>("hf:uv", {});
    // Deliberately different values on the two sides of every face.
    for (HalfFaceHandle hfh : m.halffaces()) {
        const size_t k = m.face(hfh.face_handle()).halfedges().size();
        std::vector<double> values;
        values.reserve(2 * k);
        for (size_t i = 0; i < k; ++i) {
            values.push_back(1000.0 + static_cast<double>(hfh.idx()));
            values.push_back(static_cast<double>(i) + 0.25 * hfh.idx());
        }
        uv[hfh] = values;
    }

    // Full pre-partition corner snapshot keyed by (halfface, vertex), so
    // the post-partition corner-to-vertex ordering can be pinned exactly.
    auto corner_at = [&](HalfFaceHandle hfh, VertexHandle vh) {
        const auto hes = m.halfface(hfh).halfedges();
        for (size_t i = 0; i < hes.size(); ++i) {
            if (m.from_vertex_handle(hes[i]) == vh) {
                return std::array<double, 2>{uv[hfh][2 * i], uv[hfh][2 * i + 1]};
            }
        }
        return std::array<double, 2>{0.0, 0.0};
    };
    // The far x = 2 quad belongs to the unselected neighbour: the cut
    // inserts midpoint vertices into it without dividing it, so its handle
    // survives and its original corner ordering can be pinned on both
    // halffaces after the midpoint insertions.
    FaceHandle far_face = FaceHandle(-1);
    for (FaceHandle fh : m.faces()) {
        const auto vs = m.get_halfface_vertices(fh.h0());
        bool all_x2 = vs.size() == 4;
        for (VertexHandle vh : vs) {
            if (std::abs(m.vertex(vh)[0] - 2.0) > 1e-12) {
                all_x2 = false;
            }
        }
        if (all_x2) {
            far_face = fh;
            break;
        }
    }
    TASK_CHECK(far_face.is_valid());
    // Top face of the unselected right cube (z = 1): two of its y-edges
    // coincide with split edges of the selected cell, so midpoint vertices
    // are inserted but the face itself is not divided.
    FaceHandle top_face = FaceHandle(-1);
    for (FaceHandle fh : m.faces()) {
        const auto vs = m.get_halfface_vertices(fh.h0());
        bool right_top = vs.size() == 4;
        for (VertexHandle vh : vs) {
            const Vec& p = m.vertex(vh);
            if (std::abs(p[2] - 1.0) > 1e-12 || p[0] < 1.0 - 1e-12) {
                right_top = false;
            }
        }
        if (right_top) {
            top_face = fh;
            break;
        }
    }
    TASK_CHECK(top_face.is_valid());
    std::map<std::pair<int, int>, std::array<double, 2>> before_snapshot;
    for (FaceHandle probe : {far_face, top_face}) {
        for (unsigned char side = 0; side < 2; ++side) {
            const HalfFaceHandle hfh = probe.halfface_handle(side);
            for (VertexHandle vh : m.get_halfface_vertices(hfh)) {
                before_snapshot[{hfh.idx(), vh.idx()}] = corner_at(hfh, vh);
            }
        }
    }

    // Compute the expected midpoint UVs on the shared face directly from
    // its endpoint corners.  The cut uses the y = 0.5 plane so that the
    // shared x = 1 quad is crossed along its two y-directed edges.
    const auto h0_uv = [&](HalfFaceHandle hfh, VertexHandle vh) {
        const auto hes = m.halfface(hfh).halfedges();
        for (size_t i = 0; i < hes.size(); ++i) {
            if (m.from_vertex_handle(hes[i]) == vh) {
                return std::array<double, 2>{uv[hfh][2 * i], uv[hfh][2 * i + 1]};
            }
        }
        return std::array<double, 2>{0.0, 0.0};
    };
    // Expected midpoint UVs per (x, z) column of the shared quad,
    // separately for the two halfface sides (no cross-side mixing).
    const FaceHandle shared = dc.shared_face;
    std::map<std::pair<int, int>, std::array<std::array<double, 2>, 2>>
        expected_mid;
    for (HalfEdgeHandle heh : m.face(shared).halfedges()) {
        const EdgeHandle eh = heh.edge_handle();
        const auto eps = m.edge_vertices(eh);
        if (std::abs(m.vertex(eps[0])[1] - m.vertex(eps[1])[1]) < 1e-12) {
            continue; // only y-directed edges are crossed
        }
        const VertexHandle neg_v = m.vertex(eps[0])[1] < m.vertex(eps[1])[1]
                                       ? eps[0] : eps[1];
        const VertexHandle pos_v = neg_v == eps[0] ? eps[1] : eps[0];
        const auto key = std::make_pair(
            static_cast<int>(std::lround(m.vertex(neg_v)[0])),
            static_cast<int>(std::lround(m.vertex(neg_v)[2])));
        for (unsigned char side = 0; side < 2; ++side) {
            const HalfFaceHandle hfh = shared.halfface_handle(side);
            const auto a = h0_uv(hfh, neg_v);
            const auto b = h0_uv(hfh, pos_v);
            expected_mid[key][side] = {0.5 * (a[0] + b[0]),
                                       0.5 * (a[1] + b[1])};
        }
    }
    TASK_CHECK(expected_mid.size() == 2);

    const auto records =
        partition_cells(m, {dc.left}, Vec(0.0, 1.0, 0.0), -0.5);
    TASK_CHECK(records.size() == 1);

    // All UV vectors keep matching their halfface valence and stay finite.
    for (HalfFaceHandle hfh : m.halffaces()) {
        if (m.is_deleted(hfh)) {
            continue;
        }
        const size_t k = m.face(hfh.face_handle()).halfedges().size();
        TASK_CHECK(uv[hfh].size() == 2 * k);
        for (double x : uv[hfh]) {
            TASK_CHECK(std::isfinite(x));
        }
    }

    // The far x = 2 face is untouched by the cut (none of its edges touch
    // the selected cell): it keeps its handle, valence and exact per-vertex
    // corner UVs on both halffaces, which pins the h1 corner ordering.
    TASK_CHECK(m.face(far_face).halfedges().size() == 4);
    for (unsigned char side = 0; side < 2; ++side) {
        const HalfFaceHandle hfh = far_face.halfface_handle(side);
        const auto hes = m.halfface(hfh).halfedges();
        TASK_CHECK(hes.size() == 4);
        for (size_t i = 0; i < hes.size(); ++i) {
            const VertexHandle vh = m.from_vertex_handle(hes[i]);
            const auto want = before_snapshot.at({hfh.idx(), vh.idx()});
            TASK_CHECK(std::abs(uv[hfh][2 * i] - want[0]) < 1e-12);
            TASK_CHECK(std::abs(uv[hfh][2 * i + 1] - want[1]) < 1e-12);
        }
    }

    // The neighbour's top face was only point-inserted (one midpoint on
    // its shared x = 1 edge) and kept its single handle; original corners
    // keep their UVs on both halffaces and the midpoint equals the
    // endpoint average.
    TASK_CHECK(m.face(top_face).halfedges().size() == 5);
    for (unsigned char side = 0; side < 2; ++side) {
        const HalfFaceHandle hfh = top_face.halfface_handle(side);
        const auto hes = m.halfface(hfh).halfedges();
        size_t midpoints = 0;
        for (size_t i = 0; i < hes.size(); ++i) {
            const VertexHandle vh = m.from_vertex_handle(hes[i]);
            auto it = before_snapshot.find({hfh.idx(), vh.idx()});
            if (it != before_snapshot.end()) {
                TASK_CHECK(std::abs(uv[hfh][2 * i] - it->second[0]) < 1e-12);
                TASK_CHECK(std::abs(uv[hfh][2 * i + 1] - it->second[1]) < 1e-12);
            } else {
                // Midpoint: its two neighbours along the loop are original
                // corners and its UV must be their linear average.
                TASK_CHECK(std::abs(m.vertex(vh)[1] - 0.5) < 1e-12);
                const auto prev_he =
                    (i == 0) ? hes.back() : hes[i - 1];
                const auto next_he = hes[(i + 1) % hes.size()];
                const VertexHandle pv = m.from_vertex_handle(prev_he);
                const VertexHandle nv = m.from_vertex_handle(next_he);
                const auto a = before_snapshot.at({hfh.idx(), pv.idx()});
                const auto b = before_snapshot.at({hfh.idx(), nv.idx()});
                TASK_CHECK(std::abs(uv[hfh][2 * i] - 0.5 * (a[0] + b[0])) < 1e-12);
                TASK_CHECK(std::abs(uv[hfh][2 * i + 1] - 0.5 * (a[1] + b[1])) < 1e-12);
                ++midpoints;
            }
        }
        TASK_CHECK(midpoints == 1);
    }

    // The unselected neighbour spans both sides of the y-plane, so its
    // halfface list on the shared interface contains both child faces,
    // and every inserted midpoint corner is interpolated from the
    // neighbour's own side of the original halfface.
    bool checked_interpolation = false;
    for (HalfFaceHandle hfh : m.cell_halffaces(dc.right)) {
        // The shared face and its positive sibling both lie on x = 1 and
        // are now both referenced by the unselected neighbour.
        bool derived_from_shared = false;
        {
            const auto vs = m.get_halfface_vertices(hfh);
            if (vs.size() >= 3) {
                derived_from_shared = true;
                for (VertexHandle vh : vs) {
                    if (std::abs(m.vertex(vh)[0] - 1.0) > 1e-12) {
                        derived_from_shared = false;
                        break;
                    }
                }
            }
        }
        if (!derived_from_shared) {
            continue;
        }
        const unsigned char side = hfh.subidx();
        const auto hes = m.halfface(hfh).halfedges();
        for (size_t i = 0; i < hes.size(); ++i) {
            const VertexHandle start = m.from_vertex_handle(hes[i]);
            if (std::abs(m.vertex(start)[1] - 0.5) > 1e-12) {
                continue;
            }
            const auto key = std::make_pair(
                static_cast<int>(std::lround(m.vertex(start)[0])),
                static_cast<int>(std::lround(m.vertex(start)[2])));
            const auto& want = expected_mid.at(key)[side];
            TASK_CHECK(std::abs(uv[hfh][2 * i] - want[0]) < 1e-12);
            TASK_CHECK(std::abs(uv[hfh][2 * i + 1] - want[1]) < 1e-12);
            checked_interpolation = true;
        }
    }
    TASK_CHECK(checked_interpolation);

    // The two sides of any face keep independent UVs (no cross leakage).
    for (FaceHandle fh : m.faces()) {
        if (m.is_deleted(fh) || m.valence(fh) < 3) {
            continue;
        }
        TASK_CHECK(uv[fh.h0()] != uv[fh.h1()]);
    }

    // Cap UVs use the common world frame.  With n = +y the smallest
    // |axis . n| axis is X, so U = z and V = x, and each corner's UV is
    // (p . U, p . V) = (p.z, p.x) on both cap halffaces.
    const FaceHandle cap = records.front().cap;
    for (HalfFaceHandle hfh : {cap.h0(), cap.h1()}) {
        const auto hes = m.halfface(hfh).halfedges();
        for (size_t i = 0; i < hes.size(); ++i) {
            const VertexHandle vh = m.from_vertex_handle(hes[i]);
            const Vec& p = m.vertex(vh);
            TASK_CHECK(std::abs(uv[hfh][2 * i] - p[2]) < 1e-12);
            TASK_CHECK(std::abs(uv[hfh][2 * i + 1] - p[0]) < 1e-12);
        }
    }
    return 0;
}

static int corners_property_inheritance_and_handles() {
    DoubleCube dc = make_double_cube();
    auto& m = dc.mesh;

    auto ep = m.request_edge_property<int>("e:tag", -1);
    auto hep = m.request_halfedge_property<int>("he:tag", -1);
    auto fp = m.request_face_property<int>("f:tag", -1);
    auto hfp = m.request_halfface_property<int>("hf:tag", -1);
    auto cp = m.request_cell_property<CustomData>("c:custom", CustomData{});
    for (EdgeHandle eh : m.edges()) {
        ep[eh] = 100 + eh.idx();
        hep[eh.h0()] = 1000 + eh.idx();
        hep[eh.h1()] = 2000 + eh.idx();
    }
    for (FaceHandle fh : m.faces()) {
        fp[fh] = 300 + fh.idx();
        hfp[fh.h0()] = 400 + fh.idx();
        hfp[fh.h1()] = 500 + fh.idx();
    }
    cp[dc.left] = CustomData{7, 1.5, {1, 2, 3}};
    cp[dc.right] = CustomData{9, 2.5, {4, 5}};

    // Snapshot original geometry; old vertex coordinates must be preserved.
    std::map<VertexHandle, Vec> original_positions;
    for (VertexHandle vh : m.vertices()) {
        original_positions[vh] = m.vertex(vh);
    }

    const auto records =
        partition_cells(m, {dc.left}, Vec(1.0, 0.0, 0.0), -0.5);
    TASK_CHECK(records.size() == 1);
    const auto& r = records.front();

    for (const auto& entry : original_positions) {
        TASK_CHECK((m.vertex(entry.first) - entry.second).norm() < 1e-15);
    }

    // Both children inherit the parent's custom cell property.
    const CustomData want_left{7, 1.5, {1, 2, 3}};
    const CustomData want_right{9, 2.5, {4, 5}};
    TASK_CHECK((cp[r.negative] == want_left));
    TASK_CHECK((cp[r.positive] == want_left));
    TASK_CHECK((cp[dc.right] == want_right));

    // The cap face's own edges are brand new and must carry domain
    // defaults; every other edge used by the positive child is a sub-edge
    // of a split original edge and must have copied properties.
    std::set<EdgeHandle> cap_edges;
    for (HalfEdgeHandle heh : m.face(r.cap).halfedges()) {
        cap_edges.insert(heh.edge_handle());
    }
    bool checked_sub_edge = false;
    bool checked_cap_edge = false;
    for (HalfFaceHandle hfh : m.cell_halffaces(r.positive)) {
        for (HalfEdgeHandle heh : m.halfface(hfh).halfedges()) {
            const EdgeHandle eh = heh.edge_handle();
            if (cap_edges.count(eh)) {
                TASK_CHECK(ep[eh] == -1);
                TASK_CHECK(hep[heh] == -1);
                checked_cap_edge = true;
            } else {
                TASK_CHECK(ep[eh] != -1);
                TASK_CHECK(hep[heh] != -1);
                checked_sub_edge = true;
            }
        }
    }
    TASK_CHECK(checked_sub_edge);
    TASK_CHECK(checked_cap_edge);

    // The negative child face keeps the original face handle and property.
    TASK_CHECK(fp[dc.shared_face] == 300 + dc.shared_face.idx());
    TASK_CHECK(hfp[dc.shared_face.h0()] == 400 + dc.shared_face.idx());

    // Original cell and unselected cell handles are preserved.
    TASK_CHECK(r.negative == dc.left);
    TASK_CHECK(!m.is_deleted(dc.right));
    TASK_CHECK(check_closed_shell(m, r.negative));
    TASK_CHECK(check_closed_shell(m, r.positive));
    return 0;
}

// Compression (garbage collection) and an OVM file round trip must leave
// the mesh partitionable again with persistent properties intact.
static int corners_compression_and_file_roundtrip() {
    GeometricPolyhedralMeshV3d m;
    m.enable_bottom_up_incidences(true);
    m.enable_deferred_deletion(true);

    auto make_cube = [&](const Vec& _origin) {
        std::vector<VertexHandle> v(8);
        for (int i = 0; i < 8; ++i) {
            const double dx = (i == 1 || i == 2 || i == 6 || i == 7) ? 1.0 : 0.0;
            const double dy = (i == 2 || i == 3 || i == 5 || i == 6) ? 1.0 : 0.0;
            const double dz = (i >= 4) ? 1.0 : 0.0;
            v[i] = m.add_vertex(_origin + Vec(dx, dy, dz));
        }
        return add_hexahedron(m, v);
    };
    const CellHandle a = make_cube(Vec(0.0, 0.0, 0.0));
    const CellHandle b = make_cube(Vec(10.0, 0.0, 0.0));

    auto material = m.request_cell_property<int>("c:material", 0);
    material[a] = 7;
    material[b] = 9;
    m.set_persistent(material);
    auto uv = m.request_halfface_property<std::vector<double>>("hf:uv", {});
    for (HalfFaceHandle hfh : m.halffaces()) {
        const size_t k = m.face(hfh.face_handle()).halfedges().size();
        std::vector<double> values;
        for (size_t i = 0; i < k; ++i) {
            values.push_back(static_cast<double>(hfh.idx()));
            values.push_back(static_cast<double>(i));
        }
        uv[hfh] = values;
    }
    m.set_persistent(uv);

    // Delete and compress the far component to force handle compaction.
    m.delete_cell(b);
    {
        StatusAttrib status(m);
        m.collect_garbage();
    }
    TASK_CHECK(!m.needs_garbage_collection());

    // First partition after compression.
    const auto first = partition_cells(m, {a}, Vec(1.0, 0.0, 0.0), -0.5);
    TASK_CHECK(first.size() == 1);
    TASK_CHECK(material[first.front().negative] == 7);
    TASK_CHECK(material[first.front().positive] == 7);

    // Round trip through an OVM file.
    const std::string path =
        std::string(getenv("TMPDIR")) + "/plane_partition_roundtrip.ovm";
    IO::FileManager io;
    TASK_CHECK(io.writeFile(path, m));
    GeometricPolyhedralMeshV3d reread;
    TASK_CHECK(io.readFile(path, reread));

    auto material2 = reread.request_cell_property<int>("c:material", 0);
    auto uv2 = reread.request_halfface_property<std::vector<double>>("hf:uv", {});
    TASK_CHECK(reread.n_logical_cells() == 2);
    bool material_seen = false;
    for (CellHandle ch : reread.cells()) {
        if (material2[ch] == 7) {
            material_seen = true;
        }
    }
    TASK_CHECK(material_seen);
    TASK_CHECK(!uv2[HalfFaceHandle(0)].empty());

    // Partition again on the re-read mesh with a different plane.
    std::vector<CellHandle> all;
    for (CellHandle ch : reread.cells()) {
        all.push_back(ch);
    }
    const double before = total_volume(reread);
    const auto second = partition_cells(
        reread, all, Vec(0.0, 1.0, 0.0), -0.5);
    TASK_CHECK(second.size() == 2);
    TASK_CHECK(std::abs(total_volume(reread) - before) < 1e-9);
    for (const auto& r : second) {
        TASK_CHECK(check_closed_shell(reread, r.negative));
        TASK_CHECK(check_closed_shell(reread, r.positive));
    }
    TASK_CHECK(circulators_consistent(reread));
    return 0;
}

//=========================================================================
// Registration
//=========================================================================

extern const struct task_case task_cases[];
const struct task_case task_cases[] = {
    {"closure", "double_cube_one_side", &closure_double_cube_one_side},
    {"closure", "shared_edge_partial_selection",
     &closure_shared_edge_partial_selection},
    {"closure", "adjacent_cells_share_cut_face",
     &closure_adjacent_cells_share_cut_face},
    {"closure", "rejections_keep_mesh_editable",
     &closure_rejections_keep_mesh_editable},
    {"geometry", "double_cube_volume_conservation",
     &geometry_double_cube_volume_conservation},
    {"geometry", "oblique_mixed_cells", &geometry_oblique_mixed_cells},
    {"geometry", "far_component_and_double_partition",
     &geometry_far_component_and_double_partition},
    {"corners", "uv_interpolation_and_distinct_sides",
     &corners_uv_interpolation_and_distinct_sides},
    {"corners", "property_inheritance_and_handles",
     &corners_property_inheritance_and_handles},
    {"corners", "compression_and_file_roundtrip",
     &corners_compression_and_file_roundtrip},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
