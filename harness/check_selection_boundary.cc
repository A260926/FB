// Driver for harness/check_selection_boundary.sh.
//
// Exercises OpenVolumeMesh::selection_boundary() on procedurally generated
// meshes: two tets sharing a face, two hexahedra sharing a quad, two
// disconnected shells, a selection with an inner cavity, general polyhedral
// cells (pentagonal prisms), and the edge-contact / vertex-contact
// configurations that must be rejected. Every case is re-run with each
// class of bottom-up incidence cache disabled, plus deferred-deletion and
// property-preservation checks.

#include <OpenVolumeMesh/Mesh/TetrahedralMesh.hh>
#include <OpenVolumeMesh/Mesh/HexahedralMesh.hh>
#include <OpenVolumeMesh/Mesh/PolyhedralMesh.hh>
#include <OpenVolumeMesh/Unstable/Topology/SelectionBoundary.hh>
#include <OpenVolumeMesh/Attribs/InterfaceAttrib.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace OpenVolumeMesh;

static int g_failures = 0;
static std::string g_context;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::cerr << "FAIL [" << g_context << "] line " << __LINE__    \
                      << ": " #cond << std::endl;                          \
        }                                                                  \
    } while (0)

// ---------------------------------------------------------------------------
// generic helpers (top-down incidences only, so they work with all caches off)
// ---------------------------------------------------------------------------

template <class MeshT>
static bool cell_owns_halfface(const MeshT& _m, CellHandle _ch, HalfFaceHandle _hfh) {
    for (const auto hfh : _m.cell(_ch).halffaces()) {
        if (hfh == _hfh) return true;
    }
    return false;
}

template <class MeshT>
static bool any_live_cell_owns(const MeshT& _m, HalfFaceHandle _hfh) {
    for (size_t ci = 0; ci < _m.n_cells(); ++ci) {
        const CellHandle ch(static_cast<int>(ci));
        if (_m.is_deleted(ch)) continue;
        if (cell_owns_halfface(_m, ch, _hfh)) return true;
    }
    return false;
}

static EdgeHandle find_edge(const TopologyKernel& _m, VertexHandle _u, VertexHandle _v) {
    for (size_t ei = 0; ei < _m.n_edges(); ++ei) {
        const EdgeHandle eh(static_cast<int>(ei));
        const auto ev = _m.edge_vertices(eh);
        if ((ev[0] == _u && ev[1] == _v) || (ev[0] == _v && ev[1] == _u)) return eh;
    }
    return EdgeHandle(-1);
}

static size_t face_valence(const TopologyKernel& _m, FaceHandle _fh) {
    return _m.face(_fh).halfedges().size();
}

// Call selection_boundary and verify that the query is side-effect free:
// no incidence caches get enabled, no garbage collection happens, and no
// handles are renumbered.
template <class MeshT>
static SelectionBoundary sb_call(const MeshT& _m, const std::vector<CellHandle>& _sel) {
    const bool v0 = _m.has_vertex_bottom_up_incidences();
    const bool e0 = _m.has_edge_bottom_up_incidences();
    const bool f0 = _m.has_face_bottom_up_incidences();
    const bool gc0 = _m.needs_garbage_collection();
    const size_t nv = _m.n_vertices(), ne = _m.n_edges();
    const size_t nf = _m.n_faces(), nc = _m.n_cells();
    SelectionBoundary sb = selection_boundary(_m, _sel);
    CHECK(_m.has_vertex_bottom_up_incidences() == v0);
    CHECK(_m.has_edge_bottom_up_incidences() == e0);
    CHECK(_m.has_face_bottom_up_incidences() == f0);
    CHECK(_m.needs_garbage_collection() == gc0);
    CHECK(_m.n_vertices() == nv && _m.n_edges() == ne);
    CHECK(_m.n_faces() == nf && _m.n_cells() == nc);
    return sb;
}

template <class F>
static bool throws_invalid_argument(F&& _f) {
    try { _f(); } catch (const std::invalid_argument&) { return true; } catch (...) {}
    return false;
}

template <class F>
static bool throws_domain_error(F&& _f, std::string& _msg) {
    try { _f(); } catch (const std::domain_error& e) { _msg = e.what(); return true; } catch (...) {}
    return false;
}

// ---------------------------------------------------------------------------
// generic result verification
// ---------------------------------------------------------------------------

// Independent geometric confirmation that every reported halfface points
// out of its selected cell: the polygon normal (Newell) must point away
// from an interior point of the cell.
template <class MeshT>
static void check_outward_geometry(const MeshT& _m, const SelectionBoundary& _sb) {
    for (const auto& f : _sb.faces) {
        std::vector<Geometry::Vec3d> pts;
        for (const auto heh : _m.halfface(f.halfface).halfedges()) {
            pts.push_back(_m.vertex(_m.from_vertex_handle(heh)));
        }
        CHECK(pts.size() >= 3);
        if (pts.size() < 3) continue;
        Geometry::Vec3d n{0., 0., 0.};
        Geometry::Vec3d fc{0., 0., 0.};
        for (size_t i = 0; i < pts.size(); ++i) {
            const auto& p = pts[i];
            const auto& q = pts[(i + 1) % pts.size()];
            n[0] += (p[1] - q[1]) * (p[2] + q[2]);
            n[1] += (p[2] - q[2]) * (p[0] + q[0]);
            n[2] += (p[0] - q[0]) * (p[1] + q[1]);
            fc += p;
        }
        fc = fc / static_cast<double>(pts.size());
        // interior point of the selected cell: average of its vertices
        std::set<int> cvs;
        for (const auto hfh : _m.cell(f.selected_cell).halffaces()) {
            for (const auto heh : _m.halfface(hfh).halfedges()) {
                cvs.insert(_m.to_vertex_handle(heh).idx());
            }
        }
        Geometry::Vec3d ci{0., 0., 0.};
        for (const int v : cvs) ci += _m.vertex(VertexHandle(v));
        ci = ci / static_cast<double>(cvs.size());
        CHECK((n | (fc - ci)) > 1e-12);
    }
}

template <class MeshT>
static void verify_result(const MeshT& _m, const std::vector<CellHandle>& _selection,
                          const SelectionBoundary& _sb) {
    std::set<int> sel;
    for (const auto ch : _selection) sel.insert(ch.idx());

    // sorted by ascending FaceHandle, hence unique
    for (size_t i = 0; i < _sb.faces.size(); ++i) {
        CHECK(_sb.faces[i].halfface.is_valid());
        if (i > 0) {
            CHECK(_sb.faces[i - 1].halfface.face_handle() < _sb.faces[i].halfface.face_handle());
        }
    }

    // per-entry identity, sides and winding
    for (const auto& f : _sb.faces) {
        CHECK(f.selected_cell.is_valid());
        CHECK(sel.count(f.selected_cell.idx()) == 1);
        CHECK(!_m.is_deleted(f.selected_cell));
        // the selected cell owns the opposite (inward) halfface of this face
        HalfFaceHandle inner_hf;
        for (const auto hfh : _m.cell(f.selected_cell).halffaces()) {
            if (TopologyKernel::face_handle(hfh) == f.halfface.face_handle()) inner_hf = hfh;
        }
        CHECK(inner_hf.is_valid());
        CHECK(f.halfface == TopologyKernel::opposite_halfface_handle(inner_hf));
        if (f.other_cell.is_valid()) {
            CHECK(sel.count(f.other_cell.idx()) == 0);
            CHECK(!_m.is_deleted(f.other_cell));
            CHECK(cell_owns_halfface(_m, f.other_cell, f.halfface));
        } else {
            CHECK(!any_live_cell_owns(_m, f.halfface));
        }
        CHECK(f.component < _sb.component_count);
    }

    // completeness oracle: independently recompute the expected face set
    std::map<int, CellHandle> hf_owner;
    for (size_t ci = 0; ci < _m.n_cells(); ++ci) {
        const CellHandle ch(static_cast<int>(ci));
        if (_m.is_deleted(ch)) continue;
        for (const auto hfh : _m.cell(ch).halffaces()) hf_owner[hfh.idx()] = ch;
    }
    std::map<int, std::pair<CellHandle, CellHandle>> expected;
    for (size_t ci = 0; ci < _m.n_cells(); ++ci) {
        const CellHandle ch(static_cast<int>(ci));
        if (_m.is_deleted(ch) || !sel.count(static_cast<int>(ci))) continue;
        for (const auto hfh : _m.cell(ch).halffaces()) {
            const auto out = TopologyKernel::opposite_halfface_handle(hfh);
            CellHandle other;
            const auto it = hf_owner.find(out.idx());
            if (it != hf_owner.end()) other = it->second;
            if (other.is_valid() && sel.count(other.idx())) continue;
            expected[TopologyKernel::face_handle(out).idx()] = {ch, other};
        }
    }
    CHECK(_sb.faces.size() == expected.size());
    for (const auto& f : _sb.faces) {
        const auto it = expected.find(f.halfface.face_handle().idx());
        CHECK(it != expected.end());
        if (it != expected.end()) {
            CHECK(it->second.first == f.selected_cell);
            CHECK(it->second.second == f.other_cell);
        }
    }

    // closed-surface oracle: every edge of the reported surface is used
    // exactly twice, once in each direction
    std::map<int, std::array<int, 2>> edge_dirs;
    std::map<int, std::vector<size_t>> edge_users;
    for (size_t i = 0; i < _sb.faces.size(); ++i) {
        for (const auto heh : _m.halfface(_sb.faces[i].halfface).halfedges()) {
            const int e = TopologyKernel::edge_handle(heh).idx();
            edge_dirs[e][static_cast<size_t>(heh.idx() & 1)]++;
            edge_users[e].push_back(i);
        }
    }
    for (const auto& kv : edge_dirs) {
        CHECK(kv.second[0] == 1 && kv.second[1] == 1);
    }

    // component oracle: union faces over shared edges, ids ordered by
    // ascending smallest FaceHandle per component
    std::vector<size_t> parent(_sb.faces.size());
    std::iota(parent.begin(), parent.end(), 0);
    const std::function<size_t(size_t)> find = [&](size_t x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    for (const auto& kv : edge_users) {
        for (size_t k = 1; k < kv.second.size(); ++k) {
            const size_t a = find(kv.second[0]), b = find(kv.second[k]);
            if (a != b) parent[b] = a;
        }
    }
    std::map<size_t, int> min_face;
    for (size_t i = 0; i < _sb.faces.size(); ++i) {
        const size_t r = find(i);
        const int fi = _sb.faces[i].halfface.face_handle().idx();
        if (!min_face.count(r)) min_face[r] = fi;
        else min_face[r] = std::min(min_face[r], fi);
    }
    std::vector<int> mins;
    for (const auto& kv : min_face) mins.push_back(kv.second);
    std::sort(mins.begin(), mins.end());
    CHECK(_sb.component_count == mins.size());
    for (size_t i = 0; i < _sb.faces.size(); ++i) {
        const size_t expected_id = static_cast<size_t>(
            std::lower_bound(mins.begin(), mins.end(), min_face[find(i)]) - mins.begin());
        CHECK(_sb.faces[i].component == expected_id);
    }

    check_outward_geometry(_m, _sb);
}

// ---------------------------------------------------------------------------
// incidence cache configurations: bit 0 = vertex, bit 1 = edge, bit 2 = face
// ---------------------------------------------------------------------------

template <class MeshT>
static void apply_cfg(MeshT& _m, int _cfg) {
    if (_cfg & 1) _m.enable_vertex_bottom_up_incidences(false);
    if (_cfg & 2) _m.enable_edge_bottom_up_incidences(false);
    if (_cfg & 4) _m.enable_face_bottom_up_incidences(false);
}

// ---------------------------------------------------------------------------
// cases
// ---------------------------------------------------------------------------

static void case_two_tets(int _cfg) {
    GeometricTetrahedralMeshV3d m;
    const auto a = m.add_vertex({0, 0, 0}), b = m.add_vertex({1, 0, 0});
    const auto c = m.add_vertex({0, 1, 0}), d = m.add_vertex({0, 0, 1});
    const auto e = m.add_vertex({0, 0, -1});
    const auto x = m.add_cell(a, b, c, d, true);
    const auto y = m.add_cell(a, c, b, e, true);
    CHECK(x.is_valid() && y.is_valid());

    // the face shared by x and y
    FaceHandle shared;
    for (const auto hfh : m.cell(x).halffaces()) {
        if (cell_owns_halfface(m, y, TopologyKernel::opposite_halfface_handle(hfh))) {
            shared = TopologyKernel::face_handle(hfh);
        }
    }
    CHECK(shared.is_valid());

    apply_cfg(m, _cfg);

    {   // select only x: 4 faces, the shared one has y outside
        const auto sb = sb_call(m, {x});
        CHECK(sb.faces.size() == 4);
        CHECK(sb.component_count == 1);
        size_t n_shared = 0, n_open = 0;
        for (const auto& f : sb.faces) {
            CHECK(f.selected_cell == x);
            CHECK(f.component == 0);
            if (f.halfface.face_handle() == shared) { ++n_shared; CHECK(f.other_cell == y); }
            else { ++n_open; CHECK(!f.other_cell.is_valid()); }
        }
        CHECK(n_shared == 1 && n_open == 3);
        verify_result(m, {x}, sb);
    }
    {   // select only y: 4 faces, the shared one has x outside
        const auto sb = sb_call(m, {y});
        CHECK(sb.faces.size() == 4);
        CHECK(sb.component_count == 1);
        size_t n_shared = 0;
        for (const auto& f : sb.faces) {
            CHECK(f.selected_cell == y);
            if (f.halfface.face_handle() == shared) { ++n_shared; CHECK(f.other_cell == x); }
        }
        CHECK(n_shared == 1);
        verify_result(m, {y}, sb);
    }
    {   // select both: the 6 outer faces of the union, shared face drops out
        const auto sb = sb_call(m, {x, y});
        CHECK(sb.faces.size() == 6);
        CHECK(sb.component_count == 1);
        for (const auto& f : sb.faces) {
            CHECK(!f.other_cell.is_valid());
            CHECK(f.halfface.face_handle() != shared);
        }
        verify_result(m, {x, y}, sb);
    }
    {   // duplicate handles are treated as a set
        const auto sb = sb_call(m, {x, x, x});
        CHECK(sb.faces.size() == 4);
        CHECK(sb.component_count == 1);
    }
    {   // empty selection yields an empty result
        const auto sb = sb_call(m, {});
        CHECK(sb.faces.empty());
        CHECK(sb.component_count == 0);
    }
    {   // result does not depend on the selection order
        const auto s1 = sb_call(m, {x, y});
        const auto s2 = sb_call(m, {y, x});
        CHECK(s1.faces.size() == s2.faces.size());
        CHECK(s1.component_count == s2.component_count);
        for (size_t i = 0; i < s1.faces.size() && i < s2.faces.size(); ++i) {
            CHECK(s1.faces[i].halfface == s2.faces[i].halfface);
            CHECK(s1.faces[i].selected_cell == s2.faces[i].selected_cell);
            CHECK(s1.faces[i].other_cell == s2.faces[i].other_cell);
            CHECK(s1.faces[i].component == s2.faces[i].component);
        }
    }
}

// cube [i,i+1] x [j,j+1] x [k,k+1] on a vertex grid, OVM hex vertex order:
// 0-3 front face (bottom-left, bottom-right, top-right, top-left),
// 4-7 back face, so that all cell-owned halffaces point into the cell
template <class MeshT, class GridT>
static CellHandle add_hex(MeshT& _m, const GridT& _v, int _i, int _j, int _k) {
    return _m.add_cell({_v(_i,     _j,     _k),     _v(_i + 1, _j,     _k),
                        _v(_i + 1, _j,     _k + 1), _v(_i,     _j,     _k + 1),
                        _v(_i,     _j + 1, _k),     _v(_i,     _j + 1, _k + 1),
                        _v(_i + 1, _j + 1, _k + 1), _v(_i + 1, _j + 1, _k)}, true);
}

static void case_two_hexes(int _cfg) {
    GeometricHexahedralMeshV3d m;
    std::vector<VertexHandle> gv;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k)
                gv.push_back(m.add_vertex({static_cast<double>(i),
                                           static_cast<double>(j),
                                           static_cast<double>(k)}));
    const auto V = [&](int i, int j, int k) { return gv[static_cast<size_t>(i * 4 + j * 2 + k)]; };
    const auto h0 = add_hex(m, V, 0, 0, 0);
    const auto h1 = add_hex(m, V, 1, 0, 0);
    CHECK(h0.is_valid() && h1.is_valid());

    FaceHandle shared;
    for (const auto hfh : m.cell(h0).halffaces()) {
        if (cell_owns_halfface(m, h1, TopologyKernel::opposite_halfface_handle(hfh))) {
            shared = TopologyKernel::face_handle(hfh);
        }
    }
    CHECK(shared.is_valid());

    apply_cfg(m, _cfg);

    {   // select one hex: 6 quads, the shared one has the other hex outside
        const auto sb = sb_call(m, {h0});
        CHECK(sb.faces.size() == 6);
        CHECK(sb.component_count == 1);
        size_t n_shared = 0;
        for (const auto& f : sb.faces) {
            CHECK(f.selected_cell == h0);
            if (f.halfface.face_handle() == shared) { ++n_shared; CHECK(f.other_cell == h1); }
            else CHECK(!f.other_cell.is_valid());
        }
        CHECK(n_shared == 1);
        verify_result(m, {h0}, sb);
    }
    {   // select both: 10 quads, single component
        const auto sb = sb_call(m, {h0, h1});
        CHECK(sb.faces.size() == 10);
        CHECK(sb.component_count == 1);
        for (const auto& f : sb.faces) CHECK(!f.other_cell.is_valid());
        verify_result(m, {h0, h1}, sb);
    }
}

static void case_two_shells(int _cfg) {
    GeometricTetrahedralMeshV3d m;
    const auto a = m.add_vertex({0, 0, 0}), b = m.add_vertex({1, 0, 0});
    const auto c = m.add_vertex({0, 1, 0}), d = m.add_vertex({0, 0, 1});
    const auto t1 = m.add_cell(a, b, c, d, true);
    const auto e = m.add_vertex({3, 0, 0}), f = m.add_vertex({4, 0, 0});
    const auto g = m.add_vertex({3, 1, 0}), h = m.add_vertex({3, 0, 1});
    const auto t2 = m.add_cell(e, f, g, h, true);
    CHECK(t1.is_valid() && t2.is_valid());

    apply_cfg(m, _cfg);

    // two disconnected shells: 8 faces, 2 components of 4 faces each
    const auto sb = sb_call(m, {t1, t2});
    CHECK(sb.faces.size() == 8);
    CHECK(sb.component_count == 2);
    std::map<int, size_t> comp_of_cell;
    for (const auto& fe : sb.faces) {
        comp_of_cell[fe.selected_cell.idx()] = fe.component;
        CHECK(!fe.other_cell.is_valid());
    }
    CHECK(comp_of_cell.count(t1.idx()) == 1 && comp_of_cell.count(t2.idx()) == 1);
    CHECK(comp_of_cell[t1.idx()] != comp_of_cell[t2.idx()]);
    // component ids follow the smallest FaceHandle of each shell
    int min_f1 = -1, min_f2 = -1;
    for (const auto hfh : m.cell(t1).halffaces()) {
        const int fi = TopologyKernel::face_handle(hfh).idx();
        min_f1 = min_f1 < 0 ? fi : std::min(min_f1, fi);
    }
    for (const auto hfh : m.cell(t2).halffaces()) {
        const int fi = TopologyKernel::face_handle(hfh).idx();
        min_f2 = min_f2 < 0 ? fi : std::min(min_f2, fi);
    }
    CHECK(comp_of_cell[t1.idx()] == (min_f1 < min_f2 ? 0u : 1u));
    CHECK(comp_of_cell[t2.idx()] == (min_f2 < min_f1 ? 0u : 1u));
    verify_result(m, {t1, t2}, sb);
}

static void case_cavity(int _cfg) {
    GeometricHexahedralMeshV3d m;
    std::vector<VertexHandle> gv;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                gv.push_back(m.add_vertex({static_cast<double>(i),
                                           static_cast<double>(j),
                                           static_cast<double>(k)}));
    const auto V = [&](int i, int j, int k) { return gv[static_cast<size_t>(i * 16 + j * 4 + k)]; };
    std::vector<CellHandle> cells(27);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                cells[static_cast<size_t>(i * 9 + j * 3 + k)] = add_hex(m, V, i, j, k);
    const auto C = [&](int i, int j, int k) { return cells[static_cast<size_t>(i * 9 + j * 3 + k)]; };
    const CellHandle center = C(1, 1, 1);
    CHECK(center.is_valid());

    apply_cfg(m, _cfg);

    std::vector<CellHandle> all_but_center;
    for (const auto ch : cells) if (ch != center) all_but_center.push_back(ch);

    {   // shell around a cavity: outer skin (54 quads) + inner skin (6 quads)
        const auto sb = sb_call(m, all_but_center);
        CHECK(sb.faces.size() == 60);
        CHECK(sb.component_count == 2);
        size_t n_inner = 0, n_outer = 0;
        std::set<size_t> inner_comps, outer_comps;
        std::set<int> inner_selected;
        for (const auto& f : sb.faces) {
            if (f.other_cell == center) {
                ++n_inner;
                inner_comps.insert(f.component);
                inner_selected.insert(f.selected_cell.idx());
            } else {
                ++n_outer;
                outer_comps.insert(f.component);
                CHECK(!f.other_cell.is_valid());
            }
        }
        CHECK(n_inner == 6 && n_outer == 54);
        CHECK(inner_comps.size() == 1 && outer_comps.size() == 1);
        CHECK(*inner_comps.begin() != *outer_comps.begin());
        // the inner skin faces the 6 face-neighbors of the cavity
        CHECK(inner_selected.count(C(0, 1, 1).idx()) == 1);
        CHECK(inner_selected.count(C(2, 1, 1).idx()) == 1);
        CHECK(inner_selected.count(C(1, 0, 1).idx()) == 1);
        CHECK(inner_selected.count(C(1, 2, 1).idx()) == 1);
        CHECK(inner_selected.count(C(1, 1, 0).idx()) == 1);
        CHECK(inner_selected.count(C(1, 1, 2).idx()) == 1);
        verify_result(m, all_but_center, sb);
    }
    {   // only the center: 6 quads facing its 6 neighbors
        const auto sb = sb_call(m, {center});
        CHECK(sb.faces.size() == 6);
        CHECK(sb.component_count == 1);
        std::set<int> others;
        for (const auto& f : sb.faces) {
            CHECK(f.selected_cell == center);
            others.insert(f.other_cell.idx());
        }
        CHECK(others.count(C(0, 1, 1).idx()) == 1);
        CHECK(others.count(C(2, 1, 1).idx()) == 1);
        CHECK(others.count(C(1, 0, 1).idx()) == 1);
        CHECK(others.count(C(1, 2, 1).idx()) == 1);
        CHECK(others.count(C(1, 1, 0).idx()) == 1);
        CHECK(others.count(C(1, 1, 2).idx()) == 1);
        verify_result(m, {center}, sb);
    }
    {   // full grid: outer skin only, one component
        const auto sb = sb_call(m, cells);
        CHECK(sb.faces.size() == 54);
        CHECK(sb.component_count == 1);
        for (const auto& f : sb.faces) CHECK(!f.other_cell.is_valid());
        verify_result(m, cells, sb);
    }
}

static void case_prisms(int _cfg) {
    GeometricPolyhedralMeshV3d m;
    const int n = 5;
    std::vector<VertexHandle> p[3]; // pentagon layers at z = 0, 1, 2
    for (int layer = 0; layer < 3; ++layer) {
        for (int k = 0; k < n; ++k) {
            const double ang = 2.0 * 3.14159265358979323846 * static_cast<double>(k) / n;
            p[layer].push_back(m.add_vertex({std::cos(ang), std::sin(ang),
                                             static_cast<double>(layer)}));
        }
    }
    // prism 1 (z in [0,1]): faces wound so the normal points into the cell
    const auto f_bot = m.add_face({p[0][0], p[0][1], p[0][2], p[0][3], p[0][4]});
    const auto f_mid = m.add_face({p[1][4], p[1][3], p[1][2], p[1][1], p[1][0]});
    std::vector<FaceHandle> side1;
    for (int k = 0; k < n; ++k) {
        const int k1 = (k + 1) % n;
        side1.push_back(m.add_face({p[1][k], p[1][k1], p[0][k1], p[0][k]}));
    }
    std::vector<HalfFaceHandle> hfs1;
    hfs1.push_back(TopologyKernel::halfface_handle(f_bot, 0));
    hfs1.push_back(TopologyKernel::halfface_handle(f_mid, 0));
    for (const auto fh : side1) hfs1.push_back(TopologyKernel::halfface_handle(fh, 0));
    const auto prism1 = m.add_cell(hfs1, true);
    CHECK(prism1.is_valid());

    // prism 2 (z in [1,2]): shares f_mid with the opposite orientation
    const auto f_top = m.add_face({p[2][4], p[2][3], p[2][2], p[2][1], p[2][0]});
    std::vector<FaceHandle> side2;
    for (int k = 0; k < n; ++k) {
        const int k1 = (k + 1) % n;
        side2.push_back(m.add_face({p[2][k], p[2][k1], p[1][k1], p[1][k]}));
    }
    std::vector<HalfFaceHandle> hfs2;
    hfs2.push_back(TopologyKernel::halfface_handle(f_mid, 1));
    hfs2.push_back(TopologyKernel::halfface_handle(f_top, 0));
    for (const auto fh : side2) hfs2.push_back(TopologyKernel::halfface_handle(fh, 0));
    const auto prism2 = m.add_cell(hfs2, true);
    CHECK(prism2.is_valid());

    apply_cfg(m, _cfg);

    {   // lower prism: 7 faces (2 pentagons + 5 quads)
        const auto sb = sb_call(m, {prism1});
        CHECK(sb.faces.size() == 7);
        CHECK(sb.component_count == 1);
        size_t n_shared = 0, n_pent = 0;
        for (const auto& f : sb.faces) {
            CHECK(f.selected_cell == prism1);
            if (f.halfface.face_handle() == f_mid) { ++n_shared; CHECK(f.other_cell == prism2); }
            else CHECK(!f.other_cell.is_valid());
            if (face_valence(m, f.halfface.face_handle()) == 5) ++n_pent;
        }
        CHECK(n_shared == 1);
        CHECK(n_pent == 2);
        verify_result(m, {prism1}, sb);
    }
    {   // both prisms: 12 faces, shared pentagon drops out
        const auto sb = sb_call(m, {prism1, prism2});
        CHECK(sb.faces.size() == 12);
        CHECK(sb.component_count == 1);
        for (const auto& f : sb.faces) {
            CHECK(!f.other_cell.is_valid());
            CHECK(f.halfface.face_handle() != f_mid);
        }
        verify_result(m, {prism1, prism2}, sb);
    }
}

static void case_edge_contact(int _cfg) {
    GeometricTetrahedralMeshV3d m;
    const auto a = m.add_vertex({0, 0, 0}), b = m.add_vertex({1, 0, 0});
    const auto c = m.add_vertex({0, 1, 0}), d = m.add_vertex({0, 0, 1});
    const auto e = m.add_vertex({0, -1, 0}), f = m.add_vertex({0, 0, -1});
    const auto t1 = m.add_cell(a, b, c, d, true);
    const auto t2 = m.add_cell(a, b, e, f, true);
    CHECK(t1.is_valid() && t2.is_valid());
    // the two tets touch exactly along edge a-b
    const EdgeHandle shared_edge = find_edge(m, a, b);
    CHECK(shared_edge.is_valid());

    apply_cfg(m, _cfg);

    // each tet alone is fine
    CHECK(sb_call(m, {t1}).faces.size() == 4);
    CHECK(sb_call(m, {t2}).faces.size() == 4);

    // together their boundary is not edge-manifold: must be rejected and
    // name the conflicting edge
    std::string msg;
    CHECK(throws_domain_error([&] { selection_boundary(m, {t1, t2}); }, msg));
    CHECK(msg.find("edge " + std::to_string(shared_edge.idx())) != std::string::npos);
}

static void case_vertex_contact(int _cfg) {
    GeometricTetrahedralMeshV3d m;
    const auto a = m.add_vertex({0, 0, 0});
    const auto b = m.add_vertex({1, 0, 0}), c = m.add_vertex({0, 1, 0});
    const auto d = m.add_vertex({0, 0, 1});
    const auto e = m.add_vertex({-1, 0, 0}), f = m.add_vertex({0, -1, 0});
    const auto g = m.add_vertex({0, 0, -1});
    const auto t1 = m.add_cell(a, b, c, d, true);
    const auto t2 = m.add_cell(a, e, f, g, true);
    CHECK(t1.is_valid() && t2.is_valid());

    apply_cfg(m, _cfg);

    // each tet alone is fine
    CHECK(sb_call(m, {t1}).faces.size() == 4);
    CHECK(sb_call(m, {t2}).faces.size() == 4);

    // together their boundary has a butterfly vertex: must be rejected and
    // name the conflicting vertex
    std::string msg;
    CHECK(throws_domain_error([&] { selection_boundary(m, {t1, t2}); }, msg));
    CHECK(msg.find("vertex " + std::to_string(a.idx())) != std::string::npos);
}

static void case_deferred_deletion(int _cfg) {
    GeometricTetrahedralMeshV3d m;
    const auto a = m.add_vertex({0, 0, 0}), b = m.add_vertex({1, 0, 0});
    const auto c = m.add_vertex({0, 1, 0}), d = m.add_vertex({0, 0, 1});
    const auto e = m.add_vertex({0, 0, -1});
    const auto x = m.add_cell(a, b, c, d, true);
    const auto y = m.add_cell(a, c, b, e, true);
    CHECK(x.is_valid() && y.is_valid());

    m.enable_deferred_deletion(true);
    m.delete_cell(y);
    CHECK(m.deferred_deletion_enabled());
    CHECK(m.is_deleted(y));
    CHECK(m.needs_garbage_collection());
    CHECK(m.n_cells() == 2); // no renumbering

    apply_cfg(m, _cfg);

    {   // the deleted cell counts as absent: the cut face now has no
        // outside cell, and no garbage collection is triggered
        const auto sb = sb_call(m, {x});
        CHECK(sb.faces.size() == 4);
        CHECK(sb.component_count == 1);
        for (const auto& f : sb.faces) {
            CHECK(f.selected_cell == x);
            CHECK(!f.other_cell.is_valid());
        }
        verify_result(m, {x}, sb);
        CHECK(m.needs_garbage_collection());
        CHECK(m.is_deleted(y));
        CHECK(m.n_cells() == 2);
    }
    // deleted, out-of-range and invalid handles are rejected
    CHECK(throws_invalid_argument([&] { selection_boundary(m, {y}); }));
    CHECK(throws_invalid_argument([&] { selection_boundary(m, {x, y}); }));
    CHECK(throws_invalid_argument([&] { selection_boundary(m, {CellHandle(100)}); }));
    CHECK(throws_invalid_argument([&] { selection_boundary(m, {CellHandle(-1)}); }));
}

static void case_properties_preserved(int _cfg) {
    GeometricHexahedralMeshV3d m;
    std::vector<VertexHandle> gv;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k)
                gv.push_back(m.add_vertex({static_cast<double>(i),
                                           static_cast<double>(j),
                                           static_cast<double>(k)}));
    const auto V = [&](int i, int j, int k) { return gv[static_cast<size_t>(i * 4 + j * 2 + k)]; };
    const auto h0 = add_hex(m, V, 0, 0, 0);
    const auto h1 = add_hex(m, V, 1, 0, 0);
    CHECK(h0.is_valid() && h1.is_valid());

    // a cell "selection" property, an InterfaceAttrib and vertex positions
    // must all survive the query untouched
    auto sel_prop = m.request_cell_property<int>("selection_state", 0);
    for (size_t ci = 0; ci < m.n_cells(); ++ci) {
        sel_prop[CellHandle(static_cast<int>(ci))] = 42 + static_cast<int>(ci);
    }
    InterfaceAttrib iface(m);
    iface[VertexHandle(0)] = true;
    iface[EdgeHandle(1)] = true;
    iface[FaceHandle(2)] = true;

    std::vector<Geometry::Vec3d> pos;
    for (size_t vi = 0; vi < m.n_vertices(); ++vi) {
        pos.push_back(m.vertex(VertexHandle(static_cast<int>(vi))));
    }
    std::vector<int> prop_vals;
    for (size_t ci = 0; ci < m.n_cells(); ++ci) {
        prop_vals.push_back(sel_prop[CellHandle(static_cast<int>(ci))]);
    }
    std::vector<bool> iv, ie, ifa;
    for (size_t vi = 0; vi < m.n_vertices(); ++vi) iv.push_back(iface[VertexHandle(static_cast<int>(vi))]);
    for (size_t ei = 0; ei < m.n_edges(); ++ei) ie.push_back(iface[EdgeHandle(static_cast<int>(ei))]);
    for (size_t fi = 0; fi < m.n_faces(); ++fi) ifa.push_back(iface[FaceHandle(static_cast<int>(fi))]);

    apply_cfg(m, _cfg);

    const auto sb = sb_call(m, {h0});
    CHECK(sb.faces.size() == 6);

    for (size_t vi = 0; vi < m.n_vertices(); ++vi) {
        const VertexHandle vh(static_cast<int>(vi));
        CHECK(m.vertex(vh) == pos[vi]);
        CHECK(iface[vh] == iv[vi]);
    }
    for (size_t ei = 0; ei < m.n_edges(); ++ei) {
        CHECK(iface[EdgeHandle(static_cast<int>(ei))] == ie[ei]);
    }
    for (size_t fi = 0; fi < m.n_faces(); ++fi) {
        CHECK(iface[FaceHandle(static_cast<int>(fi))] == ifa[fi]);
    }
    for (size_t ci = 0; ci < m.n_cells(); ++ci) {
        CHECK(sel_prop[CellHandle(static_cast<int>(ci))] == prop_vals[ci]);
    }
}

int main() {
    const int cfgs[] = {0, 1, 2, 4, 7}; // all on; v/e/f off individually; all off
    for (const int cfg : cfgs) {
        g_context = "cfg=" + std::to_string(cfg);
        case_two_tets(cfg);
        case_two_hexes(cfg);
        case_two_shells(cfg);
        case_cavity(cfg);
        case_prisms(cfg);
        case_edge_contact(cfg);
        case_vertex_contact(cfg);
        case_deferred_deletion(cfg);
        case_properties_preserved(cfg);
    }
    if (g_failures == 0) {
        std::cout << "selected volume skin PASS" << std::endl;
        return 0;
    }
    std::cerr << g_failures << " check(s) failed" << std::endl;
    return 1;
}
