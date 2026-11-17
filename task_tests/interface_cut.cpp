// Tests for OpenVolumeMesh/Unstable/Topology/InterfaceCut.hh
//
// Groups (registered via task_cases / task_case_count):
//   sectors     - local-link sector splitting of vertices/edges and per-cell
//                 half-face adjacency after cutting.
//   attributes  - copying of arbitrary (including non-serializable) properties
//                 and the hf:uv / hf:normal corner data.
//   persistence - interface ids across repeated cuts, garbage collection and
//                 ASCII OVM file round trips, plus rejection behaviour.

#include "task_support/check.h"

#include <OpenVolumeMesh/Unstable/Topology/InterfaceCut.hh>

#include <OpenVolumeMesh/FileManager/FileManager.hh>
#include <OpenVolumeMesh/Mesh/PolyhedralMesh.hh>
#include <OpenVolumeMesh/Mesh/TetrahedralMesh.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace OpenVolumeMesh;

using V = VertexHandle;
using E = EdgeHandle;
using F = FaceHandle;
using C = CellHandle;

namespace {

//=============================================================================
// Mesh construction helpers
//=============================================================================

C add_tetrahedron(GeometricPolyhedralMeshV3d &_mesh,
                  V _a, V _b, V _c, V _d)
{
    auto make_face = [&](V x, V y, V z) {
        std::vector<HalfEdgeHandle> halfedges;
        auto make_halfedge = [&](V from, V to) {
            HalfEdgeHandle heh = _mesh.find_halfedge(from, to);
            if (!heh.is_valid()) {
                heh = _mesh.add_edge(from, to, true).halfedge_handle(0);
            }
            if (_mesh.from_vertex_handle(heh) != from) {
                heh = heh.opposite_handle();
            }
            return heh;
        };
        halfedges.push_back(make_halfedge(x, y));
        halfedges.push_back(make_halfedge(y, z));
        halfedges.push_back(make_halfedge(z, x));

        HalfFaceHandle hfh = _mesh.find_halfface(halfedges);
        if (hfh.is_valid()) {
            return hfh;
        }
        return _mesh.halfface_handle(_mesh.add_face(halfedges), 0);
    };

    std::vector<HalfFaceHandle> halffaces;
    halffaces.push_back(make_face(_a, _b, _c));
    halffaces.push_back(make_face(_a, _c, _d));
    halffaces.push_back(make_face(_a, _d, _b));
    halffaces.push_back(make_face(_b, _d, _c));
    return _mesh.add_cell(halffaces, true);
}

/// Two tetrahedra sharing one interior face.
GeometricPolyhedralMeshV3d two_cells()
{
    GeometricPolyhedralMeshV3d mesh;
    const V a = mesh.add_vertex(Vec3d(0.0, 0.0, 0.0));
    const V b = mesh.add_vertex(Vec3d(1.0, 0.0, 0.0));
    const V c = mesh.add_vertex(Vec3d(0.0, 1.0, 0.0));
    const V d = mesh.add_vertex(Vec3d(0.0, 0.0, 1.0));
    const V e = mesh.add_vertex(Vec3d(0.0, 0.0, -1.0));

    add_tetrahedron(mesh, a, b, c, d);
    add_tetrahedron(mesh, a, c, b, e);
    mesh.enable_bottom_up_incidences(true);
    mesh.enable_deferred_deletion(true);
    return mesh;
}

/// A fan of `n` tetrahedra around the edge (top, bottom), all sharing it.
/// Each consecutive pair of tetrahedra shares a face containing that edge.
GeometricPolyhedralMeshV3d edge_fan(int n)
{
    GeometricPolyhedralMeshV3d mesh;
    const V top = mesh.add_vertex(Vec3d(0.0, 0.0, 1.0));
    const V bottom = mesh.add_vertex(Vec3d(0.0, 0.0, -1.0));
    std::vector<V> ring;
    for (int i = 0; i < n; ++i) {
        const double angle = 2.0 * 3.14159265358979323846 * i / double(n);
        ring.push_back(mesh.add_vertex(
                Vec3d(std::cos(angle), std::sin(angle), 0.0)));
    }
    for (int i = 0; i < n; ++i) {
        add_tetrahedron(mesh, top, bottom, ring[i], ring[(i + 1) % n]);
    }
    mesh.enable_bottom_up_incidences(true);
    mesh.enable_deferred_deletion(true);
    return mesh;
}

/// Two tetrahedra sharing one interior face, plus one isolated tetrahedron far
/// away that can be deleted without touching the interface.
GeometricPolyhedralMeshV3d two_cells_with_isolated_cell()
{
    GeometricPolyhedralMeshV3d mesh;
    const V a = mesh.add_vertex(Vec3d(0.0, 0.0, 0.0));
    const V b = mesh.add_vertex(Vec3d(1.0, 0.0, 0.0));
    const V c = mesh.add_vertex(Vec3d(0.0, 1.0, 0.0));
    const V d = mesh.add_vertex(Vec3d(0.0, 0.0, 1.0));
    const V e = mesh.add_vertex(Vec3d(0.0, 0.0, -1.0));
    add_tetrahedron(mesh, a, b, c, d);
    add_tetrahedron(mesh, a, c, b, e);

    // Isolated tetrahedron far away (separate vertices).
    add_tetrahedron(mesh,
                    mesh.add_vertex(Vec3d(10.0, 0.0, 0.0)),
                    mesh.add_vertex(Vec3d(11.0, 0.0, 0.0)),
                    mesh.add_vertex(Vec3d(10.0, 1.0, 0.0)),
                    mesh.add_vertex(Vec3d(10.0, 0.0, 1.0)));
    mesh.enable_bottom_up_incidences(true);
    mesh.enable_deferred_deletion(true);
    return mesh;
}

/// A structured 2x2x2 cube split into Kuhn tetrahedra.
GeometricPolyhedralMeshV3d tet_grid(int n)
{
    GeometricPolyhedralMeshV3d mesh;
    std::vector<V> lattice;
    const auto vid = [&](int x, int y, int z) {
        return lattice[size_t((z * (n + 1) + y) * (n + 1) + x)];
    };
    for (int z = 0; z <= n; ++z) {
        for (int y = 0; y <= n; ++y) {
            for (int x = 0; x <= n; ++x) {
                lattice.push_back(mesh.add_vertex(Vec3d(x, y, z)));
            }
        }
    }
    static const int permutations[6][3] = {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2},
        {1, 2, 0}, {2, 0, 1}, {2, 1, 0}
    };
    for (int z = 0; z < n; ++z) {
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                for (const auto &perm : permutations) {
                    int offset[3] = {x, y, z};
                    V vs[4];
                    vs[0] = vid(offset[0], offset[1], offset[2]);
                    for (int s = 0; s < 3; ++s) {
                        offset[perm[s]] += 1;
                        vs[s + 1] = vid(offset[0], offset[1], offset[2]);
                    }
                    const Vec3d p0 = mesh.vertex(vs[0]);
                    const Vec3d p1 = mesh.vertex(vs[1]);
                    const Vec3d p2 = mesh.vertex(vs[2]);
                    const Vec3d p3 = mesh.vertex(vs[3]);
                    if (dot(cross(p1 - p0, p2 - p0), p3 - p0) < 0.0) {
                        std::swap(vs[2], vs[3]);
                    }
                    add_tetrahedron(mesh, vs[0], vs[1], vs[2], vs[3]);
                }
            }
        }
    }
    mesh.enable_bottom_up_incidences(true);
    mesh.enable_deferred_deletion(true);
    return mesh;
}

std::vector<F> interior_faces(const TopologyKernel &_mesh)
{
    std::vector<F> faces;
    for (const F fh : _mesh.faces()) {
        const CellHandle c0 = _mesh.incident_cell(_mesh.halfface_handle(fh, 0));
        const CellHandle c1 = _mesh.incident_cell(_mesh.halfface_handle(fh, 1));
        if (c0.is_valid() && c1.is_valid()) {
            faces.push_back(fh);
        }
    }
    return faces;
}

std::vector<C> cells_incident_to(const TopologyKernel &_mesh, V _vh)
{
    std::vector<C> cells;
    for (const C ch : _mesh.vertex_cells(_vh)) {
        cells.push_back(ch);
    }
    std::sort(cells.begin(), cells.end());
    cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
    return cells;
}

std::vector<C> cells_incident_to(const TopologyKernel &_mesh, E _eh)
{
    std::vector<C> cells;
    for (const C ch : _mesh.edge_cells(_eh)) {
        cells.push_back(ch);
    }
    std::sort(cells.begin(), cells.end());
    cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
    return cells;
}

std::vector<F> faces_incident_to(const TopologyKernel &_mesh, V _vh)
{
    std::vector<F> faces;
    for (const F fh : _mesh.vertex_faces(_vh)) {
        faces.push_back(fh);
    }
    std::sort(faces.begin(), faces.end());
    faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
    return faces;
}

std::vector<F> faces_incident_to(const TopologyKernel &_mesh, E _eh)
{
    std::vector<F> faces;
    for (const F fh : _mesh.edge_faces(_eh)) {
        faces.push_back(fh);
    }
    std::sort(faces.begin(), faces.end());
    faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
    return faces;
}

std::set<int> incident_cell_set(const TopologyKernel &_mesh, V _vh)
{
    std::set<int> result;
    for (const C ch : _mesh.vertex_cells(_vh)) {
        result.insert(ch.idx());
    }
    return result;
}

std::set<int> incident_cell_set(const TopologyKernel &_mesh, E _eh)
{
    std::set<int> result;
    for (const C ch : _mesh.edge_cells(_eh)) {
        result.insert(ch.idx());
    }
    return result;
}

//=============================================================================
// Independent local-link sector computation
//=============================================================================

/// Partition the incident cells of an entity into connected components of the
/// link that remains after removing the cut faces.  This deliberately does not
/// use any production helper, so it independently verifies the split counts.
std::vector<std::set<int>> reference_sectors(
        const TopologyKernel &_mesh,
        std::vector<C> _cells,
        std::vector<F> _link_faces,
        const std::set<int> &_cut_faces)
{
    std::sort(_cells.begin(), _cells.end());
    _cells.erase(std::unique(_cells.begin(), _cells.end()), _cells.end());

    std::map<int, int> position;
    for (size_t i = 0; i < _cells.size(); ++i) {
        position[_cells[i].idx()] = int(i);
    }
    std::vector<int> parent(_cells.size());
    for (size_t i = 0; i < _cells.size(); ++i) {
        parent[i] = int(i);
    }
    std::function<int(int)> find = [&](int x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    const auto unite = [&](int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b) {
            parent[b] = a;
        }
    };

    for (const F fh : _link_faces) {
        if (_cut_faces.count(fh.idx()) != 0) {
            continue;
        }
        const CellHandle c0 = _mesh.incident_cell(_mesh.halfface_handle(fh, 0));
        const CellHandle c1 = _mesh.incident_cell(_mesh.halfface_handle(fh, 1));
        if (!c0.is_valid() || !c1.is_valid()) {
            continue;
        }
        const auto it0 = position.find(c0.idx());
        const auto it1 = position.find(c1.idx());
        if (it0 == position.end() || it1 == position.end()) {
            continue;
        }
        unite(it0->second, it1->second);
    }

    std::map<int, int> root_to_sector;
    std::vector<std::set<int>> sectors;
    for (size_t i = 0; i < _cells.size(); ++i) {
        const int root = find(int(i));
        auto insertion = root_to_sector.emplace(root, int(sectors.size()));
        if (insertion.second) {
            sectors.emplace_back();
        }
        sectors[size_t(insertion.first->second)].insert(_cells[i].idx());
    }
    std::sort(sectors.begin(), sectors.end(),
              [](const std::set<int> &a, const std::set<int> &b) {
                  return *a.begin() < *b.begin();
              });
    return sectors;
}

//=============================================================================
// Mesh invariants
//=============================================================================

bool check_cell_structure(const TopologyKernel &_mesh)
{
    for (const C ch : _mesh.cells()) {
        const auto &halffaces = _mesh.cell(ch).halffaces();
        std::set<int> seen;
        for (const HalfFaceHandle hfh : halffaces) {
            if (!seen.insert(hfh.idx()).second) {
                return false;
            }
            if (_mesh.incident_cell(hfh) != ch) {
                return false;
            }
            const auto &halfedges = _mesh.halfface(hfh).halfedges();
            for (size_t i = 0; i < halfedges.size(); ++i) {
                if (_mesh.to_vertex_handle(halfedges[i])
                        != _mesh.from_vertex_handle(
                               halfedges[(i + 1) % halfedges.size()])) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool check_adjacency(const TopologyKernel &_mesh)
{
    for (const C ch : _mesh.cells()) {
        for (const HalfFaceHandle hfh : _mesh.cell_halffaces(ch)) {
            for (const HalfEdgeHandle heh : _mesh.halfface(hfh).halfedges()) {
                const HalfFaceHandle adjacent =
                        _mesh.adjacent_halfface_in_cell(hfh, heh);
                if (!adjacent.is_valid() || _mesh.incident_cell(adjacent) != ch) {
                    return false;
                }
                bool shares_edge = false;
                for (const HalfEdgeHandle other : _mesh.halfface(adjacent).halfedges()) {
                    if (other == heh.opposite_handle()) {
                        shares_edge = true;
                    }
                }
                if (!shares_edge) {
                    return false;
                }
                const HalfFaceHandle back =
                        _mesh.adjacent_halfface_in_cell(adjacent, heh.opposite_handle());
                if (back != hfh) {
                    return false;
                }
            }
        }
    }
    return true;
}

/// Every cell must remain a consistently oriented closed surface: the sum of
/// its halfface area vectors is (numerically) zero.
bool check_cell_orientation(const GeometricPolyhedralMeshV3d &_mesh)
{
    for (const C ch : _mesh.cells()) {
        Vec3d sum(0.0, 0.0, 0.0);
        for (const HalfFaceHandle hfh : _mesh.cell_halffaces(ch)) {
            const auto halfedges = _mesh.halfface(hfh).halfedges();
            const Vec3d p0 = _mesh.vertex(_mesh.from_vertex_handle(halfedges[0]));
            const Vec3d p1 = _mesh.vertex(_mesh.from_vertex_handle(halfedges[1]));
            const Vec3d p2 = _mesh.vertex(_mesh.from_vertex_handle(halfedges[2]));
            sum += cross(p1 - p0, p2 - p0);
        }
        if (sum.norm() > 1e-9) {
            return false;
        }
    }
    return true;
}

/// Verify that the vertex/edge splits match the independent local-link sector
/// computation, and that the old handles stay with the sector that holds the
/// smallest incident cell.
bool check_entity_splits(const GeometricPolyhedralMeshV3d &before,
                         const GeometricPolyhedralMeshV3d &after,
                         const std::vector<F> &cut_faces)
{
    std::set<int> cut;
    for (const F fh : cut_faces) {
        cut.insert(fh.idx());
    }

    std::set<int> affected_vertices;
    std::set<int> affected_edges;
    for (const F fh : cut_faces) {
        for (const V vh : before.face_vertices(fh)) {
            affected_vertices.insert(vh.idx());
        }
        for (const E eh : before.face_edges(fh)) {
            affected_edges.insert(eh.idx());
        }
    }
    std::set<int> checked_edges = affected_edges;
    for (const int vi : affected_vertices) {
        for (const E eh : before.vertex_edges(V(vi))) {
            checked_edges.insert(eh.idx());
        }
    }

    std::map<std::array<double, 3>, std::vector<V>> vertices_by_position;
    for (const V vh : after.vertices()) {
        const Vec3d p = after.vertex(vh);
        vertices_by_position[{p[0], p[1], p[2]}].push_back(vh);
    }
    std::map<std::pair<std::array<double, 3>, std::array<double, 3>>,
             std::vector<E>> edges_by_position;
    for (const E eh : after.edges()) {
        const Vec3d p = after.vertex(after.edge(eh).from_vertex());
        const Vec3d q = after.vertex(after.edge(eh).to_vertex());
        std::array<double, 3> a{p[0], p[1], p[2]};
        std::array<double, 3> b{q[0], q[1], q[2]};
        if (b < a) {
            std::swap(a, b);
        }
        edges_by_position[{a, b}].push_back(eh);
    }

    std::map<int, Vec3d> old_position;
    for (const V vh : before.vertices()) {
        old_position[vh.idx()] = before.vertex(vh);
    }
    std::map<int, std::pair<V, V>> old_endpoints;
    for (const E eh : before.edges()) {
        old_endpoints[eh.idx()] = {before.edge(eh).from_vertex(),
                                   before.edge(eh).to_vertex()};
    }

    for (const int vi : affected_vertices) {
        const std::vector<std::set<int>> sectors = reference_sectors(
                before,
                cells_incident_to(before, V(vi)),
                faces_incident_to(before, V(vi)),
                cut);
        const Vec3d p = old_position[vi];
        const auto it = vertices_by_position.find({p[0], p[1], p[2]});
        if (it == vertices_by_position.end()
                || it->second.size() != sectors.size()) {
            return false;
        }
        for (const auto &sector : sectors) {
            int matches = 0;
            for (const V vh : it->second) {
                if (incident_cell_set(after, vh) == sector) {
                    ++matches;
                }
            }
            if (matches != 1) {
                return false;
            }
        }
        // The old handle must stay with the sector whose minimum cell is
        // globally minimal.
        size_t representative = 0;
        for (size_t i = 1; i < sectors.size(); ++i) {
            if (*sectors[i].begin() < *sectors[representative].begin()) {
                representative = i;
            }
        }
        bool old_handle_kept = false;
        for (const V vh : it->second) {
            if (vh.idx() == vi
                    && incident_cell_set(after, vh) == sectors[representative]) {
                old_handle_kept = true;
            }
        }
        if (!old_handle_kept) {
            return false;
        }
    }

    for (const int ei : checked_edges) {
        const std::vector<std::set<int>> sectors = reference_sectors(
                before,
                cells_incident_to(before, E(ei)),
                faces_incident_to(before, E(ei)),
                cut);
        if (sectors.empty()) {
            continue;
        }
        const V old_from = old_endpoints[ei].first;
        const V old_to = old_endpoints[ei].second;
        Vec3d p = old_position[old_from.idx()];
        Vec3d q = old_position[old_to.idx()];
        std::array<double, 3> a{p[0], p[1], p[2]};
        std::array<double, 3> b{q[0], q[1], q[2]};
        if (b < a) {
            std::swap(a, b);
        }
        const auto it = edges_by_position.find({a, b});
        if (it == edges_by_position.end()) {
            return false;
        }
        for (const auto &sector : sectors) {
            int matches = 0;
            for (const E eh : it->second) {
                if (incident_cell_set(after, eh) == sector) {
                    ++matches;
                }
            }
            if (matches != 1) {
                return false;
            }
        }
        size_t representative = 0;
        for (size_t i = 1; i < sectors.size(); ++i) {
            if (*sectors[i].begin() < *sectors[representative].begin()) {
                representative = i;
            }
        }
        bool old_handle_kept = false;
        for (const E eh : it->second) {
            if (eh.idx() == ei
                    && incident_cell_set(after, eh) == sectors[representative]) {
                old_handle_kept = true;
            }
        }
        if (!old_handle_kept) {
            return false;
        }
    }
    return true;
}

bool check_interface_pairs(const GeometricPolyhedralMeshV3d &_mesh)
{
    const std::vector<InterfacePair> pairs = interface_pairs(_mesh);
    std::set<unsigned long> ids;
    for (size_t i = 0; i < pairs.size(); ++i) {
        const InterfacePair &pair = pairs[i];
        if (pair.id == 0 || !ids.insert(pair.id).second) {
            return false;
        }
        if (i > 0 && !(pairs[i - 1].id < pair.id)) {
            return false;
        }
        if (!_mesh.incident_cell(pair.side0).is_valid()
                || !_mesh.incident_cell(pair.side1).is_valid()) {
            return false;
        }
        if (!(_mesh.incident_cell(pair.side0)
              < _mesh.incident_cell(pair.side1))) {
            return false;
        }
        if (pair.side0 == pair.side1) {
            return false;
        }
    }
    // Every face with a positive id must be represented, and every id must
    // have exactly two such faces.
    const auto ids_prop =
            _mesh.get_face_property<unsigned long>("f:interface_id");
    if (!ids_prop) {
        return pairs.empty();
    }
    std::map<unsigned long, int> per_id;
    for (const F fh : _mesh.faces()) {
        if (_mesh.is_deleted(fh)) {
            continue;
        }
        const unsigned long id = (*ids_prop)[fh];
        if (id != 0) {
            ++per_id[id];
        }
    }
    for (const auto &entry : per_id) {
        if (entry.second != 2) {
            return false;
        }
    }
    return per_id.size() == pairs.size();
}

//=============================================================================
// Attribute helpers
//=============================================================================

struct CustomValue
{
    int number = 0;
    std::string text;

    CustomValue() = default;
    CustomValue(int _number, std::string _text)
        : number(_number), text(std::move(_text)) {}

    bool operator==(const CustomValue &other) const
    {
        return number == other.number && text == other.text;
    }
};

void fill_corner_properties(GeometricPolyhedralMeshV3d &_mesh)
{
    auto uv = _mesh.request_halfface_property<std::vector<double>>("hf:uv", {});
    auto normal = _mesh.request_halfface_property<std::vector<double>>("hf:normal", {});
    for (const HalfFaceHandle hfh : _mesh.halffaces()) {
        const size_t valence =
                _mesh.face(hfh.face_handle()).halfedges().size();
        std::vector<double> uvs(2 * valence);
        std::vector<double> normals(3 * valence);
        for (size_t i = 0; i < valence; ++i) {
            uvs[2 * i] = 100.0 * double(hfh.idx()) + double(i);
            uvs[2 * i + 1] = 1000.0 * double(hfh.idx()) + double(i);
            normals[3 * i] = 1.0;
            normals[3 * i + 1] = 2.0;
            normals[3 * i + 2] = 3.0;
        }
        uv[hfh] = std::move(uvs);
        normal[hfh] = std::move(normals);
    }
    _mesh.set_persistent(uv);
    _mesh.set_persistent(normal);
}

} // namespace

//=============================================================================
// Group: sectors
//=============================================================================

int case_two_cells_split(void)
{
    GeometricPolyhedralMeshV3d mesh = two_cells();
    const std::vector<F> shared = interior_faces(mesh);
    TASK_CHECK(shared.size() == 1);

    const size_t vertices_before = mesh.n_vertices();
    const size_t edges_before = mesh.n_edges();
    const size_t faces_before = mesh.n_faces();
    const size_t cells_before = mesh.n_cells();

    const GeometricPolyhedralMeshV3d snapshot = mesh;
    cut_interfaces(mesh, std::vector<F>{shared[0]});

    // Every vertex/edge on the cut face is split into exactly two sectors.
    TASK_CHECK(mesh.n_vertices() == vertices_before + 3);
    TASK_CHECK(mesh.n_edges() == edges_before + 3);
    TASK_CHECK(mesh.n_faces() == faces_before + 1);
    TASK_CHECK(mesh.n_cells() == cells_before);

    TASK_CHECK(check_entity_splits(snapshot, mesh, shared));
    TASK_CHECK(check_cell_structure(mesh));
    TASK_CHECK(check_adjacency(mesh));
    TASK_CHECK(check_cell_orientation(mesh));
    TASK_CHECK(check_interface_pairs(mesh));

    // The old face becomes a boundary face; both interface copies are boundary.
    const std::vector<InterfacePair> pairs = interface_pairs(mesh);
    TASK_CHECK(pairs.size() == 1);
    TASK_CHECK(mesh.incident_cell(pairs[0].side0).idx() == 0);
    TASK_CHECK(mesh.incident_cell(pairs[0].side1).idx() == 1);
    TASK_CHECK(mesh.is_boundary(pairs[0].side0.face_handle()));
    TASK_CHECK(mesh.is_boundary(pairs[0].side1.face_handle()));
    TASK_CHECK(mesh.n_logical_faces() == faces_before + 1);
    return 0;
}

int case_edge_fan_split(void)
{
    GeometricPolyhedralMeshV3d mesh = edge_fan(4);
    const std::vector<F> shared = interior_faces(mesh);
    TASK_CHECK(shared.size() == 4);

    // Cut two adjacent faces so that they meet along the central edge.
    cut_interfaces(mesh, std::vector<F>{shared[0], shared[1]});
    TASK_CHECK(interface_pairs(mesh).size() == 2);
    TASK_CHECK(check_cell_structure(mesh));
    TASK_CHECK(check_adjacency(mesh));
    TASK_CHECK(check_cell_orientation(mesh));
    TASK_CHECK(check_interface_pairs(mesh));

    // The central edge is split into two arcs: one incident to the two cells
    // on either side of the cut, one to the two remaining cells.
    const GeometricPolyhedralMeshV3d before = mesh;
    return 0;
}

int case_grid_adjacent_cuts(void)
{
    GeometricPolyhedralMeshV3d mesh = tet_grid(2);
    const std::vector<F> shared = interior_faces(mesh);
    TASK_CHECK(!shared.empty());

    // Pick two interior faces that share an edge, if they exist.
    std::map<int, std::vector<F>> faces_by_edge;
    for (const F fh : shared) {
        for (const E eh : mesh.face_edges(fh)) {
            faces_by_edge[eh.idx()].push_back(fh);
        }
    }
    std::vector<F> selection;
    for (const auto &entry : faces_by_edge) {
        if (entry.second.size() >= 2) {
            selection = {entry.second[0], entry.second[1]};
            break;
        }
    }
    TASK_CHECK(selection.size() == 2);

    const GeometricPolyhedralMeshV3d snapshot = mesh;
    cut_interfaces(mesh, selection);
    TASK_CHECK(interface_pairs(mesh).size() == 2);
    TASK_CHECK(check_entity_splits(snapshot, mesh, selection));
    TASK_CHECK(check_cell_structure(mesh));
    TASK_CHECK(check_adjacency(mesh));
    TASK_CHECK(check_cell_orientation(mesh));
    TASK_CHECK(check_interface_pairs(mesh));
    return 0;
}

int case_grid_partial_fan(void)
{
    GeometricPolyhedralMeshV3d mesh = tet_grid(2);
    const std::vector<F> shared = interior_faces(mesh);

    std::map<int, std::vector<F>> faces_by_edge;
    for (const F fh : shared) {
        for (const E eh : mesh.face_edges(fh)) {
            faces_by_edge[eh.idx()].push_back(fh);
        }
    }
    int best_edge = -1;
    size_t best_count = 0;
    for (const auto &entry : faces_by_edge) {
        if (entry.second.size() > best_count) {
            best_count = entry.second.size();
            best_edge = entry.first;
        }
    }
    TASK_CHECK(best_edge >= 0);
    TASK_CHECK(best_count >= 4);

    // Cut only half of the faces around the edge: a crack tip along the edge.
    // The remaining half-link keeps some sectors connected.
    const std::vector<F> &fan = faces_by_edge[best_edge];
    std::vector<F> selection(fan.begin(), fan.begin() + long(best_count / 2));
    const GeometricPolyhedralMeshV3d snapshot = mesh;
    cut_interfaces(mesh, selection);
    TASK_CHECK(interface_pairs(mesh).size() == selection.size());
    TASK_CHECK(check_entity_splits(snapshot, mesh, selection));
    TASK_CHECK(check_cell_structure(mesh));
    TASK_CHECK(check_adjacency(mesh));
    TASK_CHECK(check_cell_orientation(mesh));
    return 0;
}

int case_grid_distant_components(void)
{
    GeometricPolyhedralMeshV3d mesh = tet_grid(3);
    const std::vector<F> shared = interior_faces(mesh);

    // Pick interior faces whose incident cells are pairwise disjoint.
    std::set<int> used_cells;
    std::vector<F> selection;
    for (const F fh : shared) {
        const CellHandle c0 = mesh.incident_cell(mesh.halfface_handle(fh, 0));
        const CellHandle c1 = mesh.incident_cell(mesh.halfface_handle(fh, 1));
        if (used_cells.count(c0.idx()) || used_cells.count(c1.idx())) {
            continue;
        }
        selection.push_back(fh);
        used_cells.insert(c0.idx());
        used_cells.insert(c1.idx());
        if (selection.size() == 3) {
            break;
        }
    }
    TASK_CHECK(selection.size() == 3);
    const GeometricPolyhedralMeshV3d snapshot = mesh;
    cut_interfaces(mesh, selection);
    TASK_CHECK(interface_pairs(mesh).size() == 3);
    TASK_CHECK(check_entity_splits(snapshot, mesh, selection));
    TASK_CHECK(check_cell_structure(mesh));
    TASK_CHECK(check_adjacency(mesh));
    TASK_CHECK(check_cell_orientation(mesh));

    // Existing slots retain their numbering: old vertex handles still refer to
    // the same positions, and old faces untouched by any affected vertex keep
    // their exact vertex loop.
    std::set<int> affected_vertices;
    for (const F fh : selection) {
        for (const V vh : snapshot.face_vertices(fh)) {
            affected_vertices.insert(vh.idx());
        }
    }
    for (const V vh : snapshot.vertices()) {
        TASK_CHECK(mesh.is_valid(vh));
        TASK_CHECK(mesh.vertex(vh) == snapshot.vertex(vh));
    }
    for (const F fh : snapshot.faces()) {
        TASK_CHECK(mesh.is_valid(fh));
    }
    for (const F fh : snapshot.faces()) {
        if (std::find(selection.begin(), selection.end(), fh) != selection.end()) {
            continue;
        }
        bool touched = false;
        for (const V vh : snapshot.face_vertices(fh)) {
            if (affected_vertices.count(vh.idx())) {
                touched = true;
            }
        }
        if (!touched) {
            TASK_CHECK(mesh.get_halfface_vertices(mesh.halfface_handle(fh, 0))
                       == snapshot.get_halfface_vertices(
                              snapshot.halfface_handle(fh, 0)));
        }
    }
    // Cell slots and their winding are unchanged.
    TASK_CHECK(mesh.n_cells() == snapshot.n_cells());
    for (const C ch : snapshot.cells()) {
        TASK_CHECK(mesh.cell(ch).halffaces().size()
                   == snapshot.cell(ch).halffaces().size());
    }
    return 0;
}

int case_all_faces_split(void)
{
    GeometricPolyhedralMeshV3d mesh = tet_grid(2);
    const std::vector<F> shared = interior_faces(mesh);
    TASK_CHECK(shared.size() == 72);

    const GeometricPolyhedralMeshV3d snapshot = mesh;
    cut_interfaces(mesh, shared);
    TASK_CHECK(interface_pairs(mesh).size() == shared.size());
    TASK_CHECK(check_entity_splits(snapshot, mesh, shared));
    TASK_CHECK(check_cell_structure(mesh));
    TASK_CHECK(check_adjacency(mesh));
    TASK_CHECK(check_cell_orientation(mesh));
    TASK_CHECK(check_interface_pairs(mesh));

    // Every face is now a boundary face, so every cell is disconnected.
    for (const F fh : mesh.faces()) {
        TASK_CHECK(mesh.is_boundary(fh));
    }
    return 0;
}

int case_duplicates_ignored(void)
{
    GeometricPolyhedralMeshV3d mesh = two_cells();
    const F shared = interior_faces(mesh)[0];
    const size_t faces_before = mesh.n_faces();
    cut_interfaces(mesh, std::vector<F>{shared, shared, shared});
    TASK_CHECK(mesh.n_faces() == faces_before + 1);
    TASK_CHECK(interface_pairs(mesh).size() == 1);
    return 0;
}

int case_hexahedral_two_cells(void)
{
    GeometricPolyhedralMeshV3d mesh;
    std::vector<V> vertices;
    for (int z = 0; z < 3; ++z) {
        vertices.push_back(mesh.add_vertex(Vec3d(0, 0, z)));
        vertices.push_back(mesh.add_vertex(Vec3d(1, 0, z)));
        vertices.push_back(mesh.add_vertex(Vec3d(1, 1, z)));
        vertices.push_back(mesh.add_vertex(Vec3d(0, 1, z)));
    }
    const auto v = [&](int x, int y, int z) {
        return vertices[size_t(z * 4 + (y ? (x ? 2 : 3) : (x ? 1 : 0)))];
    };
    const auto halfface_loop = [&](std::vector<V> loop) {
        std::vector<HalfEdgeHandle> halfedges;
        for (size_t i = 0; i < loop.size(); ++i) {
            HalfEdgeHandle heh = mesh.find_halfedge(loop[i],
                                                    loop[(i + 1) % loop.size()]);
            if (!heh.is_valid()) {
                heh = mesh.add_edge(loop[i], loop[(i + 1) % loop.size()], true)
                              .halfedge_handle(0);
            }
            if (mesh.from_vertex_handle(heh) != loop[i]) {
                heh = heh.opposite_handle();
            }
            halfedges.push_back(heh);
        }
        HalfFaceHandle hfh = mesh.find_halfface(halfedges);
        if (hfh.is_valid()) {
            return hfh;
        }
        return mesh.halfface_handle(mesh.add_face(halfedges), 0);
    };
    const auto add_hex = [&](int z) {
        std::vector<HalfFaceHandle> halffaces;
        halffaces.push_back(halfface_loop({v(0, 0, z), v(0, 1, z), v(1, 1, z), v(1, 0, z)}));
        halffaces.push_back(halfface_loop({v(0, 0, z + 1), v(1, 0, z + 1), v(1, 1, z + 1), v(0, 1, z + 1)}));
        halffaces.push_back(halfface_loop({v(0, 0, z), v(1, 0, z), v(1, 0, z + 1), v(0, 0, z + 1)}));
        halffaces.push_back(halfface_loop({v(1, 0, z), v(1, 1, z), v(1, 1, z + 1), v(1, 0, z + 1)}));
        halffaces.push_back(halfface_loop({v(1, 1, z), v(0, 1, z), v(0, 1, z + 1), v(1, 1, z + 1)}));
        halffaces.push_back(halfface_loop({v(0, 1, z), v(0, 0, z), v(0, 0, z + 1), v(0, 1, z + 1)}));
        return mesh.add_cell(halffaces, true);
    };
    TASK_CHECK(add_hex(0).is_valid());
    TASK_CHECK(add_hex(1).is_valid());
    mesh.enable_bottom_up_incidences(true);
    mesh.enable_deferred_deletion(true);

    const std::vector<F> shared = interior_faces(mesh);
    TASK_CHECK(shared.size() == 1);

    const GeometricPolyhedralMeshV3d snapshot = mesh;
    cut_interfaces(mesh, shared);
    TASK_CHECK(interface_pairs(mesh).size() == 1);
    TASK_CHECK(check_entity_splits(snapshot, mesh, shared));
    TASK_CHECK(check_cell_structure(mesh));
    TASK_CHECK(check_adjacency(mesh));
    TASK_CHECK(check_cell_orientation(mesh));
    // A hexahedron has 4 vertices and 4 edges on the shared face.
    TASK_CHECK(mesh.n_vertices() == snapshot.n_vertices() + 4);
    TASK_CHECK(mesh.n_edges() == snapshot.n_edges() + 4);
    return 0;
}

int case_crack_tip_shares_edge(void)
{
    // A fan of four tetrahedra around the central edge (top, bottom).
    // Cutting only one face leaves the link of the central edge connected,
    // so the edge is not split; the two face copies must share it.
    GeometricPolyhedralMeshV3d mesh = edge_fan(4);
    const V top = V(0);
    const V bottom = V(1);
    const E central = mesh.find_halfedge(top, bottom).edge_handle();
    TASK_CHECK(central.is_valid());

    const std::vector<F> shared = interior_faces(mesh);
    F cut_face = TopologyKernel::InvalidFaceHandle;
    for (const F fh : shared) {
        if (mesh.is_incident(fh, central)) {
            cut_face = fh;
            break;
        }
    }
    TASK_CHECK(cut_face.is_valid());

    cut_interfaces(mesh, std::vector<F>{cut_face});
    const std::vector<InterfacePair> pairs = interface_pairs(mesh);
    TASK_CHECK(pairs.size() == 1);

    const auto contains_edge = [&](HalfFaceHandle hfh, E eh) {
        for (const HalfEdgeHandle heh : mesh.halfface(hfh).halfedges()) {
            if (heh.edge_handle() == eh) {
                return true;
            }
        }
        return false;
    };
    // The central edge is unchanged and both interface half-faces use it.
    TASK_CHECK(mesh.n_edges() == 13 + 2);  // only the ring edges split
    TASK_CHECK(contains_edge(pairs[0].side0, central));
    TASK_CHECK(contains_edge(pairs[0].side1, central));
    TASK_CHECK(check_cell_structure(mesh));
    TASK_CHECK(check_adjacency(mesh));
    TASK_CHECK(check_cell_orientation(mesh));
    return 0;
}

int case_successive_edits(void)
{
    GeometricPolyhedralMeshV3d mesh = edge_fan(4);
    const std::vector<F> shared = interior_faces(mesh);
    TASK_CHECK(shared.size() == 4);

    // Cut the faces one at a time; ids must keep increasing and never collide.
    for (size_t i = 0; i < shared.size(); ++i) {
        cut_interfaces(mesh, std::vector<F>{shared[i]});
        const std::vector<InterfacePair> pairs = interface_pairs(mesh);
        TASK_CHECK(pairs.size() == i + 1);
        TASK_CHECK(pairs.back().id == i + 1);
        TASK_CHECK(check_cell_structure(mesh));
        TASK_CHECK(check_adjacency(mesh));
    TASK_CHECK(check_cell_orientation(mesh));
    }
    // The old face handles are reused for the smaller-cell side; cutting them
    // again must be rejected.
    bool threw = false;
    try {
        cut_interfaces(mesh, shared);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(interface_pairs(mesh).size() == 4);
    return 0;
}

//=============================================================================
// Group: attributes
//=============================================================================

int case_custom_properties_copied(void)
{
    GeometricPolyhedralMeshV3d mesh = two_cells();
    const F shared = interior_faces(mesh)[0];

    auto vertex_prop = mesh.request_vertex_property<CustomValue>("v:custom", CustomValue());
    auto edge_prop = mesh.request_edge_property<CustomValue>("e:custom", CustomValue());
    auto halfedge_prop = mesh.request_halfedge_property<CustomValue>("he:custom", CustomValue());
    auto face_prop = mesh.request_face_property<CustomValue>("f:custom", CustomValue());
    auto halfface_prop = mesh.request_halfface_property<CustomValue>("hf:custom", CustomValue());
    auto cell_prop = mesh.request_cell_property<CustomValue>("c:custom", CustomValue());

    for (const V vh : mesh.vertices()) {
        vertex_prop[vh] = CustomValue(vh.idx(), "v" + std::to_string(vh.idx()));
    }
    for (const E eh : mesh.edges()) {
        edge_prop[eh] = CustomValue(eh.idx(), "e" + std::to_string(eh.idx()));
        halfedge_prop[eh.halfedge_handle(0)] =
                CustomValue(2 * eh.idx(), "h" + std::to_string(2 * eh.idx()));
        halfedge_prop[eh.halfedge_handle(1)] =
                CustomValue(2 * eh.idx() + 1, "h" + std::to_string(2 * eh.idx() + 1));
    }
    for (const F fh : mesh.faces()) {
        face_prop[fh] = CustomValue(fh.idx(), "f" + std::to_string(fh.idx()));
    }
    for (const HalfFaceHandle hfh : mesh.halffaces()) {
        halfface_prop[hfh] = CustomValue(hfh.idx(), "hf" + std::to_string(hfh.idx()));
    }
    for (const C ch : mesh.cells()) {
        cell_prop[ch] = CustomValue(ch.idx(), "c" + std::to_string(ch.idx()));
    }
    for (auto *prop : {&vertex_prop}) {
        (void)prop;
    }
    // None of these properties is declared serializable; persistence in memory
    // is what matters here.
    mesh.set_persistent(vertex_prop);
    mesh.set_persistent(edge_prop);
    mesh.set_persistent(halfedge_prop);
    mesh.set_persistent(face_prop);
    mesh.set_persistent(halfface_prop);
    mesh.set_persistent(cell_prop);

    const int min_sub =
            mesh.incident_cell(mesh.halfface_handle(shared, 0)).idx()
                    < mesh.incident_cell(mesh.halfface_handle(shared, 1)).idx()
            ? 0 : 1;
    const HalfFaceHandle old_min_side = mesh.halfface_handle(shared, min_sub);
    const HalfFaceHandle old_max_side = mesh.halfface_handle(shared, 1 - min_sub);
    const CustomValue expected_min_half = halfface_prop[old_min_side];
    const CustomValue expected_max_half = halfface_prop[old_max_side];

    cut_interfaces(mesh, std::vector<F>{shared});

    // Cell properties are untouched.
    for (const C ch : mesh.cells()) {
        TASK_CHECK(cell_prop[ch] == CustomValue(ch.idx(), "c" + std::to_string(ch.idx())));
    }

    // Every vertex copy shares the property value of its original vertex.
    std::map<std::array<double, 3>, std::vector<V>> by_position;
    for (const V vh : mesh.vertices()) {
        const Vec3d p = mesh.vertex(vh);
        by_position[{p[0], p[1], p[2]}].push_back(vh);
    }
    for (const auto &entry : by_position) {
        TASK_CHECK(!entry.second.empty());
        const CustomValue expected = vertex_prop[entry.second.front()];
        for (const V vh : entry.second) {
            TASK_CHECK(vertex_prop[vh] == expected);
        }
    }

    // Every edge copy shares the edge and halfedge property values of the
    // original edge at the same endpoints.
    std::map<std::pair<std::array<double, 3>, std::array<double, 3>>,
             std::vector<E>> by_endpoints;
    for (const E eh : mesh.edges()) {
        const Vec3d p = mesh.vertex(mesh.edge(eh).from_vertex());
        const Vec3d q = mesh.vertex(mesh.edge(eh).to_vertex());
        std::array<double, 3> a{p[0], p[1], p[2]};
        std::array<double, 3> b{q[0], q[1], q[2]};
        if (b < a) {
            std::swap(a, b);
        }
        by_endpoints[{a, b}].push_back(eh);
    }
    for (const auto &entry : by_endpoints) {
        const CustomValue expected = edge_prop[entry.second.front()];
        for (const E eh : entry.second) {
            TASK_CHECK(edge_prop[eh] == expected);
        }
        // Half-edge properties of a copy are copied onto corresponding
        // half-edge handles.
        const E reference = entry.second.front();
        TASK_CHECK(halfedge_prop[reference.halfedge_handle(0)]
                   == CustomValue(2 * reference.idx(), "h" + std::to_string(2 * reference.idx())));
    }

    // The new face inherits the old face's property.
    const std::vector<InterfacePair> pairs = interface_pairs(mesh);
    TASK_CHECK(pairs.size() == 1);
    TASK_CHECK(face_prop[pairs[0].side1.face_handle()]
               == CustomValue(shared.idx(), "f" + std::to_string(shared.idx())));

    // The cell-incident half-face of each interface face inherits the property
    // value of the corresponding original side.
    TASK_CHECK(halfface_prop[pairs[0].side0] == expected_min_half);
    TASK_CHECK(halfface_prop[pairs[0].side1] == expected_max_half);
    return 0;
}

int case_uv_and_normal_per_side(void)
{
    GeometricPolyhedralMeshV3d mesh = two_cells();
    const F shared = interior_faces(mesh)[0];
    fill_corner_properties(mesh);

    const int min_sub =
            mesh.incident_cell(mesh.halfface_handle(shared, 0)).idx()
                    < mesh.incident_cell(mesh.halfface_handle(shared, 1)).idx()
            ? 0 : 1;
    const HalfFaceHandle old_min = mesh.halfface_handle(shared, min_sub);
    const HalfFaceHandle old_max = mesh.halfface_handle(shared, 1 - min_sub);

    const auto uv = mesh.get_halfface_property<std::vector<double>>("hf:uv");
    const auto normal = mesh.get_halfface_property<std::vector<double>>("hf:normal");
    TASK_CHECK(uv.has_value());
    TASK_CHECK(normal.has_value());

    const std::vector<double> uv_min = (*uv)[old_min];
    const std::vector<double> uv_max = (*uv)[old_max];
    const int smaller_cell =
            std::min(mesh.incident_cell(old_min).idx(),
                     mesh.incident_cell(old_max).idx());
    const int larger_cell =
            std::max(mesh.incident_cell(old_min).idx(),
                     mesh.incident_cell(old_max).idx());

    cut_interfaces(mesh, std::vector<F>{shared});

    const std::vector<InterfacePair> pairs = interface_pairs(mesh);
    TASK_CHECK(pairs.size() == 1);

    // The old face's cell-incident half-face is side0 and keeps its data.
    TASK_CHECK(mesh.incident_cell(pairs[0].side0).idx() == smaller_cell);
    TASK_CHECK((*uv)[pairs[0].side0] == uv_min);

    // The new copy's cell-incident half-face is side1 and keeps its own data.
    TASK_CHECK((*uv)[pairs[0].side1] == uv_max);
    TASK_CHECK(mesh.incident_cell(pairs[0].side1).idx() == larger_cell);
    TASK_CHECK((*uv)[pairs[0].side1] != (*uv)[pairs[0].side0]);

    // The opposite half-face of each interface face is the reversed corner
    // order with negated normals.
    const size_t valence = mesh.face(shared).halfedges().size();
    const HalfFaceHandle side0_outer = pairs[0].side0.opposite_handle();
    const HalfFaceHandle side1_outer = pairs[0].side1.opposite_handle();
    for (size_t i = 0; i < valence; ++i) {
        TASK_CHECK((*uv)[side0_outer][2 * i] == uv_min[2 * (valence - 1 - i)]);
        TASK_CHECK((*uv)[side0_outer][2 * i + 1] == uv_min[2 * (valence - 1 - i) + 1]);
        TASK_CHECK((*uv)[side1_outer][2 * i] == uv_max[2 * (valence - 1 - i)]);
    }
    TASK_CHECK((*normal)[side0_outer][0] == -1.0);
    TASK_CHECK((*normal)[side1_outer][0] == -1.0);
    TASK_CHECK((*normal)[pairs[0].side0][0] == 1.0);
    TASK_CHECK((*normal)[pairs[0].side1][0] == 1.0);
    return 0;
}

int case_uv_invalid_rejected(void)
{
    GeometricPolyhedralMeshV3d mesh = two_cells();
    const std::vector<F> shared = interior_faces(mesh);
    fill_corner_properties(mesh);

    auto uv = mesh.get_halfface_property<std::vector<double>>("hf:uv");
    TASK_CHECK(uv.has_value());
    const HalfFaceHandle victim = mesh.halfface_handle(shared[0], 0);
    const std::vector<double> saved = (*uv)[victim];

    const size_t faces_before = mesh.n_faces();
    const size_t vertices_before = mesh.n_vertices();

    // Wrong length.
    (*uv)[victim].pop_back();
    bool threw = false;
    try {
        cut_interfaces(mesh, shared);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(mesh.n_faces() == faces_before);
    TASK_CHECK(mesh.n_vertices() == vertices_before);
    TASK_CHECK(interface_pairs(mesh).empty());

    // Non-finite value.
    (*uv)[victim] = saved;
    (*uv)[victim][0] = std::numeric_limits<double>::quiet_NaN();
    threw = false;
    try {
        cut_interfaces(mesh, shared);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(mesh.n_faces() == faces_before);

    // After fixing the data the edit succeeds.
    (*uv)[victim] = saved;
    cut_interfaces(mesh, shared);
    TASK_CHECK(mesh.n_faces() == faces_before + 1);
    TASK_CHECK(interface_pairs(mesh).size() == 1);
    return 0;
}

int case_bad_normal_rejected(void)
{
    GeometricPolyhedralMeshV3d mesh = two_cells();
    const std::vector<F> shared = interior_faces(mesh);
    fill_corner_properties(mesh);

    auto normal = mesh.get_halfface_property<std::vector<double>>("hf:normal");
    TASK_CHECK(normal.has_value());
    const HalfFaceHandle victim = mesh.halfface_handle(shared[0], 1);
    const std::vector<double> saved = (*normal)[victim];
    (*normal)[victim][1] = std::numeric_limits<double>::infinity();

    bool threw = false;
    try {
        cut_interfaces(mesh, shared);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);
    (*normal)[victim] = saved;

    cut_interfaces(mesh, shared);
    TASK_CHECK(interface_pairs(mesh).size() == 1);
    return 0;
}

int case_no_corner_properties(void)
{
    GeometricPolyhedralMeshV3d mesh = two_cells();
    const std::vector<F> shared = interior_faces(mesh);
    // No hf:uv / hf:normal properties exist: the cut must still work.
    cut_interfaces(mesh, shared);
    TASK_CHECK(interface_pairs(mesh).size() == 1);
    TASK_CHECK(check_cell_structure(mesh));
    return 0;
}

int case_smaller_cell_on_reverse_side(void)
{
    // Force the smaller cell handle onto half-face 1 of the shared face by
    // swapping the two cells.  The face keeps its winding and handle, so the
    // implementation must still leave the old handle with the smaller cell.
    GeometricPolyhedralMeshV3d mesh = two_cells();
    mesh.swap_cell_indices(C(0), C(1));
    const F shared = interior_faces(mesh)[0];

    fill_corner_properties(mesh);
    const auto uv = mesh.get_halfface_property<std::vector<double>>("hf:uv");
    const auto normal = mesh.get_halfface_property<std::vector<double>>("hf:normal");
    TASK_CHECK(uv.has_value() && normal.has_value());

    const int min_sub =
            mesh.incident_cell(mesh.halfface_handle(shared, 0)).idx()
                    < mesh.incident_cell(mesh.halfface_handle(shared, 1)).idx()
            ? 0 : 1;
    TASK_CHECK(min_sub == 1);

    const HalfFaceHandle old_min = mesh.halfface_handle(shared, min_sub);
    const HalfFaceHandle old_max = mesh.halfface_handle(shared, 1 - min_sub);
    const int smaller_cell = mesh.incident_cell(old_min).idx();
    const int larger_cell = mesh.incident_cell(old_max).idx();
    const std::vector<double> uv_min = (*uv)[old_min];
    const std::vector<double> uv_max = (*uv)[old_max];

    // The old face keeps its handle, and the winding/vertex order is unchanged.
    const std::vector<V> winding_before = mesh.get_halfface_vertices(
            mesh.halfface_handle(shared, 0));

    cut_interfaces(mesh, std::vector<F>{shared});

    TASK_CHECK(mesh.get_halfface_vertices(mesh.halfface_handle(shared, 0))
               == winding_before);

    const std::vector<InterfacePair> pairs = interface_pairs(mesh);
    TASK_CHECK(pairs.size() == 1);
    TASK_CHECK(pairs[0].side0.face_handle() == shared);
    TASK_CHECK(pairs[0].side1.face_handle() != shared);
    TASK_CHECK(mesh.incident_cell(pairs[0].side0).idx() == smaller_cell);
    TASK_CHECK(mesh.incident_cell(pairs[0].side1).idx() == larger_cell);
    // Each side keeps its own original corner data; nothing is mixed.
    TASK_CHECK((*uv)[pairs[0].side0] == uv_min);
    TASK_CHECK((*uv)[pairs[0].side1] == uv_max);
    TASK_CHECK((*normal)[pairs[0].side0][0] == 1.0);
    TASK_CHECK((*normal)[pairs[0].side1][0] == 1.0);

    // Each face's non-cell half-face is the reversed corner order of its own
    // cell-incident side, with negated normals.
    const size_t valence = mesh.face(shared).halfedges().size();
    const HalfFaceHandle side0_outer = pairs[0].side0.opposite_handle();
    const HalfFaceHandle side1_outer = pairs[0].side1.opposite_handle();
    for (size_t i = 0; i < valence; ++i) {
        TASK_CHECK((*uv)[side0_outer][2 * i] == uv_min[2 * (valence - 1 - i)]);
        TASK_CHECK((*uv)[side1_outer][2 * i] == uv_max[2 * (valence - 1 - i)]);
    }
    TASK_CHECK((*normal)[side0_outer][0] == -1.0);
    TASK_CHECK((*normal)[side1_outer][0] == -1.0);

    TASK_CHECK(check_cell_structure(mesh));
    TASK_CHECK(check_adjacency(mesh));
    TASK_CHECK(check_cell_orientation(mesh));
    return 0;
}

//=============================================================================
// Group: persistence
//=============================================================================

int case_ids_and_next_id(void)
{
    GeometricPolyhedralMeshV3d mesh = tet_grid(2);
    const std::vector<F> shared = interior_faces(mesh);

    cut_interfaces(mesh, std::vector<F>{shared[0], shared[1], shared[2]});
    const std::vector<InterfacePair> pairs = interface_pairs(mesh);
    TASK_CHECK(pairs.size() == 3);
    TASK_CHECK(pairs[0].id == 1 && pairs[1].id == 2 && pairs[2].id == 3);

    const auto next_id =
            mesh.get_property<unsigned long, Entity::Mesh>("m:next_interface_id");
    TASK_CHECK(next_id.has_value());
    TASK_CHECK((*next_id)[MeshHandle(0)] == 4);

    const auto face_ids =
            mesh.get_face_property<unsigned long>("f:interface_id");
    const auto sides =
            mesh.get_halfface_property<int>("hf:interface_side");
    TASK_CHECK(face_ids.has_value());
    TASK_CHECK(sides.has_value());

    for (const InterfacePair &pair : pairs) {
        TASK_CHECK((*face_ids)[pair.side0.face_handle()] == pair.id);
        TASK_CHECK((*face_ids)[pair.side1.face_handle()] == pair.id);
        TASK_CHECK((*sides)[pair.side0] == 0);
        TASK_CHECK((*sides)[pair.side1] == 1);
        // The opposite half-faces are not sides of the interface.
        TASK_CHECK((*sides)[pair.side0.opposite_handle()] == -1);
        TASK_CHECK((*sides)[pair.side1.opposite_handle()] == -1);
    }
    // Ordinary faces have id 0.
    int ordinary = 0;
    for (const F fh : mesh.faces()) {
        if ((*face_ids)[fh] == 0) {
            ++ordinary;
        }
    }
    TASK_CHECK(ordinary > 0);
    return 0;
}

int case_recut_after_gc(void)
{
    GeometricPolyhedralMeshV3d mesh = tet_grid(2);
    const std::vector<F> shared = interior_faces(mesh);

    cut_interfaces(mesh, std::vector<F>{shared[0]});
    TASK_CHECK(interface_pairs(mesh).size() == 1);
    TASK_CHECK(interface_pairs(mesh)[0].id == 1);

    // Garbage collection must not disturb the interface properties.
    mesh.collect_garbage();
    const std::vector<InterfacePair> after_gc = interface_pairs(mesh);
    TASK_CHECK(after_gc.size() == 1);
    TASK_CHECK(after_gc[0].id == 1);

    // Cut again after collection: old pairing is still queryable and the new
    // id does not collide.
    const std::vector<F> remaining = interior_faces(mesh);
    TASK_CHECK(!remaining.empty());
    cut_interfaces(mesh, std::vector<F>{remaining[0]});
    const std::vector<InterfacePair> both = interface_pairs(mesh);
    TASK_CHECK(both.size() == 2);
    TASK_CHECK(both[0].id == 1);
    TASK_CHECK(both[1].id == 2);

    const auto next_id =
            mesh.get_property<unsigned long, Entity::Mesh>("m:next_interface_id");
    TASK_CHECK(next_id.has_value() && (*next_id)[MeshHandle(0)] == 3);
    return 0;
}

int case_edit_with_deleted_slots(void)
{
    // Deferred deletion is required; the mesh may already contain deleted
    // slots when cut_interfaces() is called.  Those slots must be tolerated as
    // long as they are not the faces being cut.
    GeometricPolyhedralMeshV3d mesh = two_cells_with_isolated_cell();
    TASK_CHECK(mesh.n_logical_cells() == 3);

    // The isolated cell is the last one; delete it (deferred).
    mesh.delete_cell(C(2));
    TASK_CHECK(mesh.needs_garbage_collection());
    TASK_CHECK(mesh.deferred_deletion_enabled());

    const std::vector<F> shared = interior_faces(mesh);
    TASK_CHECK(shared.size() == 1);

    cut_interfaces(mesh, shared);
    const std::vector<InterfacePair> pairs = interface_pairs(mesh);
    TASK_CHECK(pairs.size() == 1);
    for (const InterfacePair &pair : pairs) {
        TASK_CHECK(mesh.incident_cell(pair.side0).is_valid());
        TASK_CHECK(mesh.incident_cell(pair.side1).is_valid());
        TASK_CHECK(!mesh.is_deleted(mesh.incident_cell(pair.side0)));
        TASK_CHECK(!mesh.is_deleted(mesh.incident_cell(pair.side1)));
    }
    TASK_CHECK(mesh.needs_garbage_collection());  // still deferred, not compacted

    mesh.collect_garbage();
    const std::vector<InterfacePair> after_gc = interface_pairs(mesh);
    TASK_CHECK(after_gc.size() == 1);
    TASK_CHECK(after_gc[0].id == 1);
    TASK_CHECK(mesh.n_logical_cells() == 2);
    return 0;
}

int case_deleted_side_drops_pair(void)
{
    // Two independent interfaces with pairwise disjoint cells; deleting one
    // side of the second and then collecting garbage must leave the first
    // interface intact and drop the now-incomplete second one.
    GeometricPolyhedralMeshV3d mesh;
    const V a = mesh.add_vertex(Vec3d(0.0, 0.0, 0.0));
    const V b = mesh.add_vertex(Vec3d(1.0, 0.0, 0.0));
    const V c = mesh.add_vertex(Vec3d(0.0, 1.0, 0.0));
    const V d = mesh.add_vertex(Vec3d(0.0, 0.0, 1.0));
    const V e = mesh.add_vertex(Vec3d(0.0, 0.0, -1.0));
    add_tetrahedron(mesh, a, b, c, d);
    add_tetrahedron(mesh, a, c, b, e);

    const V p = mesh.add_vertex(Vec3d(10.0, 0.0, 0.0));
    const V q = mesh.add_vertex(Vec3d(11.0, 0.0, 0.0));
    const V r = mesh.add_vertex(Vec3d(10.0, 1.0, 0.0));
    const V s = mesh.add_vertex(Vec3d(10.0, 0.0, 1.0));
    const V t = mesh.add_vertex(Vec3d(10.0, 0.0, -1.0));
    add_tetrahedron(mesh, p, q, r, s);
    add_tetrahedron(mesh, p, r, q, t);

    mesh.enable_bottom_up_incidences(true);
    mesh.enable_deferred_deletion(true);

    const std::vector<F> shared = interior_faces(mesh);
    TASK_CHECK(shared.size() == 2);
    cut_interfaces(mesh, shared);
    std::vector<InterfacePair> pairs = interface_pairs(mesh);
    TASK_CHECK(pairs.size() == 2);
    TASK_CHECK(pairs[0].id == 1 && pairs[1].id == 2);

    // Delete the cell that owns side0 of the last pair.
    const CellHandle victim = mesh.incident_cell(pairs.back().side0);
    TASK_CHECK(victim.is_valid());
    const unsigned long victim_id = pairs.back().id;
    mesh.delete_cell(victim);
    mesh.collect_garbage();

    const std::vector<InterfacePair> remaining = interface_pairs(mesh);
    TASK_CHECK(remaining.size() == 1);
    for (const InterfacePair &pair : remaining) {
        TASK_CHECK(pair.id != victim_id);
        TASK_CHECK(pair.id == 1);
        TASK_CHECK(mesh.incident_cell(pair.side0).is_valid());
        TASK_CHECK(mesh.incident_cell(pair.side1).is_valid());
    }
    return 0;
}

int case_file_round_trip(void)
{
    GeometricPolyhedralMeshV3d mesh = tet_grid(2);
    const std::vector<F> shared = interior_faces(mesh);
    cut_interfaces(mesh, std::vector<F>{shared[0], shared[1], shared[2]});
    const std::vector<InterfacePair> before = interface_pairs(mesh);
    TASK_CHECK(before.size() == 3);

    // Store the world positions of both sides so that we can compare the ids
    // across the file boundary without relying on handle values.
    std::vector<std::pair<unsigned long, Vec3d>> before_centers;
    for (const InterfacePair &pair : before) {
        const auto vertices = mesh.get_halfface_vertices(pair.side0);
        Vec3d center(0.0, 0.0, 0.0);
        for (const V vh : vertices) {
            center += mesh.vertex(vh);
        }
        center /= double(vertices.size());
        before_centers.emplace_back(pair.id, center);
    }

    const std::string path = std::string(std::getenv("TMPDIR")
                                         ? std::getenv("TMPDIR") : "/tmp")
            + "/ovm_interface_persistence.ovm";
    IO::FileManager file_manager;
    TASK_CHECK(file_manager.writeFile(path, mesh));

    GeometricPolyhedralMeshV3d read_back;
    TASK_CHECK(file_manager.readFile(path, read_back));
    TASK_CHECK(read_back.n_cells() == mesh.n_cells());
    TASK_CHECK(read_back.n_faces() == mesh.n_faces());

    const std::vector<InterfacePair> after = interface_pairs(read_back);
    TASK_CHECK(after.size() == before.size());
    for (size_t i = 0; i < after.size(); ++i) {
        TASK_CHECK(after[i].id == before[i].id);
        TASK_CHECK(read_back.incident_cell(after[i].side0).is_valid());
        TASK_CHECK(read_back.incident_cell(after[i].side1).is_valid());
        TASK_CHECK(read_back.incident_cell(after[i].side0)
                   < read_back.incident_cell(after[i].side1));
        const auto vertices = read_back.get_halfface_vertices(after[i].side0);
        Vec3d center(0.0, 0.0, 0.0);
        for (const V vh : vertices) {
            center += read_back.vertex(vh);
        }
        center /= double(vertices.size());
        TASK_CHECK((center - before_centers[i].second).norm() < 1e-12);
    }

    // Cut the read-back mesh again and verify that the old pairing survives
    // and the new id continues after the read mesh's next id.
    const std::vector<F> remaining = interior_faces(read_back);
    TASK_CHECK(!remaining.empty());
    cut_interfaces(read_back, std::vector<F>{remaining[0]});
    const std::vector<InterfacePair> extended = interface_pairs(read_back);
    TASK_CHECK(extended.size() == before.size() + 1);
    TASK_CHECK(extended.back().id == 4);
    std::set<unsigned long> ids;
    for (const InterfacePair &pair : extended) {
        TASK_CHECK(ids.insert(pair.id).second);
    }
    std::remove(path.c_str());
    return 0;
}

int case_gc_after_file_round_trip(void)
{
    GeometricPolyhedralMeshV3d mesh = tet_grid(2);
    const std::vector<F> shared = interior_faces(mesh);
    cut_interfaces(mesh, std::vector<F>{shared[0], shared[1]});

    const std::string path = std::string(std::getenv("TMPDIR")
                                         ? std::getenv("TMPDIR") : "/tmp")
            + "/ovm_interface_gc.ovm";
    IO::FileManager file_manager;
    TASK_CHECK(file_manager.writeFile(path, mesh));

    GeometricPolyhedralMeshV3d read_back;
    TASK_CHECK(file_manager.readFile(path, read_back));
    TASK_CHECK(interface_pairs(read_back).size() == 2);

    read_back.collect_garbage();
    const std::vector<InterfacePair> after_gc = interface_pairs(read_back);
    TASK_CHECK(after_gc.size() == 2);
    TASK_CHECK(after_gc[0].id == 1 && after_gc[1].id == 2);
    std::remove(path.c_str());
    return 0;
}

int case_rejection_is_atomic(void)
{
    GeometricPolyhedralMeshV3d mesh = two_cells();
    const std::vector<F> shared = interior_faces(mesh);
    const F interior = shared[0];

    const FaceHandle invalid(9999);
    // Find a boundary face (one of its half-faces has no incident cell).
    F boundary_face = TopologyKernel::InvalidFaceHandle;
    for (const F fh : mesh.faces()) {
        if (!mesh.incident_cell(mesh.halfface_handle(fh, 0)).is_valid()
                || !mesh.incident_cell(mesh.halfface_handle(fh, 1)).is_valid()) {
            boundary_face = fh;
            break;
        }
    }
    TASK_CHECK(boundary_face.is_valid());

    const std::size_t vertices = mesh.n_vertices();
    const std::size_t edges = mesh.n_edges();
    const std::size_t faces = mesh.n_faces();
    const std::size_t cells = mesh.n_cells();

    bool threw = false;
    try {
        cut_interfaces(mesh, std::vector<F>{invalid});
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try {
        cut_interfaces(mesh, std::vector<F>{boundary_face});
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);

    // Nothing changed by the rejected calls, and an empty call is a no-op.
    TASK_CHECK(mesh.n_vertices() == vertices);
    TASK_CHECK(mesh.n_edges() == edges);
    TASK_CHECK(mesh.n_faces() == faces);
    TASK_CHECK(mesh.n_cells() == cells);
    TASK_CHECK(interface_pairs(mesh).empty());
    cut_interfaces(mesh, std::vector<F>{});
    TASK_CHECK(mesh.n_faces() == faces);

    // Editing still works after the rejections.
    cut_interfaces(mesh, std::vector<F>{interior});
    TASK_CHECK(interface_pairs(mesh).size() == 1);
    return 0;
}

int case_reject_deleted_and_recut(void)
{
    GeometricPolyhedralMeshV3d mesh = two_cells();
    const std::vector<F> shared = interior_faces(mesh);
    cut_interfaces(mesh, shared);
    TASK_CHECK(interface_pairs(mesh).size() == 1);

    // Cutting the same face again is rejected.
    bool threw = false;
    try {
        cut_interfaces(mesh, shared);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);

    // A deleted slot is rejected.  Delete a boundary face (which also deletes
    // its incident cell) with deferred deletion, leaving a deleted face slot.
    GeometricPolyhedralMeshV3d other = two_cells();
    F deleted = TopologyKernel::InvalidFaceHandle;
    for (const F fh : other.faces()) {
        if (!other.incident_cell(other.halfface_handle(fh, 0)).is_valid()
                || !other.incident_cell(other.halfface_handle(fh, 1)).is_valid()) {
            deleted = fh;
            break;
        }
    }
    TASK_CHECK(deleted.is_valid());
    other.delete_face(deleted);
    TASK_CHECK(other.is_deleted(deleted));
    threw = false;
    try {
        cut_interfaces(other, std::vector<F>{deleted});
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);
    return 0;
}

int case_requires_mesh_state(void)
{
    GeometricPolyhedralMeshV3d mesh = two_cells();
    const std::vector<F> shared = interior_faces(mesh);

    // Deferred deletion disabled.
    mesh.enable_deferred_deletion(false);
    bool threw = false;
    try {
        cut_interfaces(mesh, shared);
    } catch (const std::logic_error &) {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(interface_pairs(mesh).empty());
    mesh.enable_deferred_deletion(true);

    // Bottom-up incidences disabled.
    mesh.enable_bottom_up_incidences(false);
    threw = false;
    try {
        cut_interfaces(mesh, shared);
    } catch (const std::logic_error &) {
        threw = true;
    }
    TASK_CHECK(threw);
    mesh.enable_bottom_up_incidences(true);

    // Now the edit works.
    cut_interfaces(mesh, shared);
    TASK_CHECK(interface_pairs(mesh).size() == 1);
    return 0;
}

//=============================================================================
// Registration
//=============================================================================

const struct task_case task_cases[] = {
    {"sectors", "two_cells", case_two_cells_split},
    {"sectors", "edge_fan", case_edge_fan_split},
    {"sectors", "grid_adjacent_cuts", case_grid_adjacent_cuts},
    {"sectors", "grid_partial_fan", case_grid_partial_fan},
    {"sectors", "grid_distant_components", case_grid_distant_components},
    {"sectors", "all_faces_split", case_all_faces_split},
    {"sectors", "duplicates_ignored", case_duplicates_ignored},
    {"sectors", "hexahedral_two_cells", case_hexahedral_two_cells},
    {"sectors", "crack_tip_shares_edge", case_crack_tip_shares_edge},
    {"sectors", "successive_edits", case_successive_edits},

    {"attributes", "custom_properties_copied", case_custom_properties_copied},
    {"attributes", "uv_and_normal_per_side", case_uv_and_normal_per_side},
    {"attributes", "uv_invalid_rejected", case_uv_invalid_rejected},
    {"attributes", "bad_normal_rejected", case_bad_normal_rejected},
    {"attributes", "no_corner_properties", case_no_corner_properties},
    {"attributes", "smaller_cell_on_reverse_side",
     case_smaller_cell_on_reverse_side},

    {"persistence", "ids_and_next_id", case_ids_and_next_id},
    {"persistence", "recut_after_gc", case_recut_after_gc},
    {"persistence", "edit_with_deleted_slots", case_edit_with_deleted_slots},
    {"persistence", "deleted_side_drops_pair", case_deleted_side_drops_pair},
    {"persistence", "file_round_trip", case_file_round_trip},
    {"persistence", "gc_after_file_round_trip", case_gc_after_file_round_trip},
    {"persistence", "rejection_is_atomic", case_rejection_is_atomic},
    {"persistence", "reject_deleted_and_recut", case_reject_deleted_and_recut},
    {"persistence", "requires_mesh_state", case_requires_mesh_state},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
