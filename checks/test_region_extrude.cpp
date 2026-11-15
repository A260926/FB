// Regression tests for pm::extrude_region including its attribute slot-copy support.
//
// Groups: topology, attributes, persistence

#include <polymesh/Mesh.hh>
#include <polymesh/algorithms/region_extrude.hh>
#include <polymesh/ext/attribute_collection.hh>
#include <polymesh/formats/obj.hh>
#include <polymesh/properties.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "task_support/check.h"

using namespace polymesh;

namespace
{
using pos_t = std::array<double, 4>;
using corner_t = std::array<double, 3>;

struct fixture
{
    Mesh m;
    vertex_attribute<pos_t> pos;
    halfedge_attribute<corner_t> uv;
    halfedge_attribute<corner_t> nrm;

    explicit fixture(int grid_n) : pos(m), uv(m), nrm(m)
    {
        // grid_n x grid_n vertices -> (grid_n-1)^2 quad faces,
        // face index = y*(grid_n-1)+x
        std::vector<vertex_handle> vh(size_t(grid_n) * size_t(grid_n));
        for (auto y = 0; y < grid_n; ++y)
            for (auto x = 0; x < grid_n; ++x)
            {
                auto v = m.vertices().add();
                vh[size_t(y) * size_t(grid_n) + size_t(x)] = v;
                pos[v] = {double(x), double(y), 0.0, 1.0};
            }

        for (auto y = 0; y < grid_n - 1; ++y)
            for (auto x = 0; x < grid_n - 1; ++x)
                m.faces().add(vh[size_t(y) * size_t(grid_n) + size_t(x)], //
                              vh[size_t(y) * size_t(grid_n) + size_t(x + 1)], //
                              vh[size_t(y + 1) * size_t(grid_n) + size_t(x + 1)], //
                              vh[size_t(y + 1) * size_t(grid_n) + size_t(x)]);

        // nonzero defaults so "untouched" checks are meaningful
        for (auto h : m.halfedges())
        {
            uv[h] = {0.125, 0.25, 0.0};
            nrm[h] = {0.0, 0.0, 1.0};
        }
    }

    face_handle face_at(int x, int y, int n) const { return m.handle_of(face_index(y * (n - 1) + x)); }
};

struct snapshot
{
    int total[4];
    std::vector<pos_t> pos;
    std::vector<corner_t> uv, nrm;

    explicit snapshot(fixture& f)
    {
        total[0] = f.m.all_vertices().size();
        total[1] = f.m.all_edges().size();
        total[2] = f.m.all_faces().size();
        total[3] = f.m.all_halfedges().size();
        for (auto v : f.m.all_vertices())
            pos.push_back(f.pos[v]);
        for (auto h : f.m.all_halfedges())
        {
            uv.push_back(f.uv[h]);
            nrm.push_back(f.nrm[h]);
        }
    }

    bool matches(fixture& f) const
    {
        if (total[0] != f.m.all_vertices().size() || total[1] != f.m.all_edges().size()
            || total[2] != f.m.all_faces().size() || total[3] != f.m.all_halfedges().size())
            return false;

        auto i = 0u;
        for (auto v : f.m.all_vertices())
        {
            if (pos[i] != f.pos[v])
                return false;
            ++i;
        }
        i = 0u;
        for (auto h : f.m.all_halfedges())
        {
            if (uv[i] != f.uv[h] || nrm[i] != f.nrm[h])
                return false;
            ++i;
        }
        return true;
    }
};

bool expect_throw(fixture& f, std::vector<face_handle> const& region, std::array<double, 3> delta)
{
    snapshot before(f);
    bool threw = false;
    try
    {
        extrude_region(f.m, f.pos, f.uv, f.nrm, region, delta);
    }
    catch (std::invalid_argument const&)
    {
        threw = true;
    }
    return threw && before.matches(f);
}

bool almost(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

std::array<double, 3> sub3(pos_t const& a, pos_t const& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
std::array<double, 3> cross3(std::array<double, 3> const& a, std::array<double, 3> const& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double len3(std::array<double, 3> const& a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }

/// Newell normal of the face (unit length, zero if degenerate)
std::array<double, 3> face_normal_impl(face_handle fc, vertex_attribute<pos_t> const& position)
{
    auto hs = fc.halfedges().to_vector();
    std::array<double, 3> n = {0, 0, 0};
    for (auto i = 0u; i < hs.size(); ++i)
    {
        auto a = position[hs[i].vertex_to()];
        auto b = position[hs[(i + 1) % hs.size()].vertex_to()];
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    auto l = len3(n);
    if (l > 0)
        n = {n[0] / l, n[1] / l, n[2] / l};
    return n;
}

/// average of face corner positions (adequate for our planar quads)
pos_t face_centroid_impl(face_handle fc, vertex_attribute<pos_t> const& position)
{
    pos_t c = {0, 0, 0, 0};
    auto cnt = 0;
    for (auto v : fc.vertices())
    {
        auto p = position[v];
        c[0] += p[0];
        c[1] += p[1];
        c[2] += p[2];
        c[3] += p[3];
        ++cnt;
    }
    for (auto i = 0; i < 4; ++i)
        c[i] /= double(cnt);
    return c;
}

double face_area_impl(face_handle fc, vertex_attribute<pos_t> const& position)
{
    auto hs = fc.halfedges().to_vector();
    pos_t p0 = position[hs[0].vertex_to()];
    std::array<double, 3> acc = {0, 0, 0};
    for (auto i = 1u; i + 1 < hs.size(); ++i)
    {
        auto a = sub3(position[hs[i].vertex_to()], p0);
        auto b = sub3(position[hs[i + 1].vertex_to()], p0);
        auto c = cross3(a, b);
        acc[0] += c[0];
        acc[1] += c[1];
        acc[2] += c[2];
    }
    return 0.5 * len3(acc);
}
} // namespace

// ================= TOPOLOGY =================

static int test_empty_region()
{
    fixture f(4);
    snapshot before(f);
    auto caps = extrude_region(f.m, f.pos, f.uv, f.nrm, {}, {0, 0, 1});
    TASK_CHECK(caps.empty());
    TASK_CHECK(before.matches(f));
    f.m.assert_consistency();
    return 0;
}

static int test_single_interior_face()
{
    fixture f(4); // 3x3 faces
    auto const f0 = f.m.faces().size();
    auto const v0 = f.m.vertices().size();
    auto const e0 = f.m.edges().size();

    auto region = std::vector<face_handle>{f.face_at(1, 1, 4)};
    auto caps = extrude_region(f.m, f.pos, f.uv, f.nrm, region, {0, 0, 2});

    TASK_CHECK(caps.size() == 1);
    TASK_CHECK(caps[0].idx == region[0].idx); // face handle reused
    TASK_CHECK(f.m.faces().size() == f0 + 4);      // 4 walls
    TASK_CHECK(f.m.vertices().size() == v0 + 4);   // 4 cap vertices
    TASK_CHECK(f.m.edges().size() == e0 + 8);      // 4 top + 4 vertical
    f.m.assert_consistency();

    TASK_CHECK(caps[0].vertices().size() == 4); // cap stays a quad
    auto n = face_normal_impl(caps[0], f.pos);
    TASK_CHECK(almost(n[2], 1.0));
    TASK_CHECK(almost(face_area_impl(caps[0], f.pos), 1.0));

    auto z0 = 0, z2 = 0;
    for (auto v : f.m.vertices())
    {
        if (f.pos[v][2] == 0.0)
            z0++;
        if (f.pos[v][2] == 2.0)
            z2++;
    }
    TASK_CHECK(z0 == 16);
    TASK_CHECK(z2 == 4);
    return 0;
}

static int test_multi_face_shared_inside()
{
    fixture f(5); // 4x4 faces
    auto const f0 = f.m.faces().size();
    auto const v0 = f.m.vertices().size();

    std::vector<face_handle> region = {f.face_at(1, 1, 5), f.face_at(2, 1, 5), f.face_at(1, 2, 5), f.face_at(2, 2, 5)};
    auto caps = extrude_region(f.m, f.pos, f.uv, f.nrm, region, {0, 0, 1});

    TASK_CHECK(caps.size() == 4);
    for (auto i = 0u; i < caps.size(); ++i)
        TASK_CHECK(caps[i].idx == region[i].idx);

    // 8 boundary vertices duplicated, 1 interior vertex translated
    TASK_CHECK(f.m.vertices().size() == v0 + 8);
    // boundary of the 2x2 patch has 8 edges -> 8 walls
    TASK_CHECK(f.m.faces().size() == f0 + 8);

    for (auto c : caps)
        TASK_CHECK(c.vertices().size() == 4);
    f.m.assert_consistency();

    auto moved = 0;
    for (auto v : f.m.vertices())
        if (f.pos[v][2] == 1.0)
            moved++;
    TASK_CHECK(moved == 9); // 8 duplicates + 1 interior

    // the 4 internal edges between caps are shared by exactly two cap faces
    auto shared = 0;
    for (auto e : f.m.edges())
    {
        auto fa = e.faceA();
        auto fb = e.faceB();
        if (!fa.is_valid() || !fb.is_valid())
            continue;
        bool ca = false, cb = false;
        for (auto c : caps)
        {
            ca |= (c == fa);
            cb |= (c == fb);
        }
        if (ca && cb)
            shared++;
    }
    TASK_CHECK(shared == 4);
    return 0;
}

static int test_hole_region()
{
    fixture f(7); // 6x6 faces
    std::vector<face_handle> ring;
    for (auto y = 1; y <= 4; ++y)
        for (auto x = 1; x <= 4; ++x)
            if (!(x >= 2 && x <= 3 && y >= 2 && y <= 3))
                ring.push_back(f.face_at(x, y, 7));

    TASK_CHECK(ring.size() == 12);

    auto const f0 = f.m.faces().size();
    auto caps = extrude_region(f.m, f.pos, f.uv, f.nrm, ring, {0, 0, 1});
    TASK_CHECK(caps.size() == 12);
    f.m.assert_consistency();

    // outer loop 16 edges + hole loop 8 edges -> 24 walls
    TASK_CHECK(f.m.faces().size() == f0 + 24);

    // recognize walls as the non-caps with 4 vertices (the surrounding grid is also made of quads)
    auto wall_cnt = 0;
    for (auto fc : f.m.faces())
    {
        if (fc.vertices().size() != 4)
            continue;
        bool is_cap = false;
        for (auto c : caps)
            is_cap |= (c == fc);
        if (is_cap)
            continue;
        // walls are quads that have exactly one edge at z == 0 and one at z == 1
        auto z0 = false, z1 = false;
        for (auto v : fc.vertices())
        {
            z0 |= almost(f.pos[v][2], 0.0);
            z1 |= almost(f.pos[v][2], 1.0);
        }
        if (z0 && z1)
            ++wall_cnt;
    }
    TASK_CHECK(wall_cnt == 24);

    // hole walls must face into the hole: normal points from the wall toward (3,3)
    auto hole_center = std::array<double, 2>{3.0, 3.0};
    auto hole_walls = 0;
    for (auto fc : f.m.faces())
    {
        if (fc.vertices().size() != 4)
            continue;
        bool is_cap = false;
        for (auto c : caps)
            is_cap |= (c == fc);
        if (is_cap)
            continue;

        // hole wall: has a bottom edge (z=0) and a top edge (z=1), and lies at the hole's rim
        auto z0 = false, z1 = false;
        for (auto v : fc.vertices())
        {
            z0 |= almost(f.pos[v][2], 0.0);
            z1 |= almost(f.pos[v][2], 1.0);
        }
        if (!z0 || !z1)
            continue;
        auto inside = true;
        for (auto v : fc.vertices())
            inside &= (f.pos[v][0] >= 1.5 && f.pos[v][0] <= 4.5 && f.pos[v][1] >= 1.5 && f.pos[v][1] <= 4.5);
        if (!inside)
            continue;

        auto cen = face_centroid_impl(fc, f.pos);
        auto n = face_normal_impl(fc, f.pos);
        // horizontal
        TASK_CHECK(almost(n[2], 0.0));
        // points into the hole
        TASK_CHECK(n[0] * (hole_center[0] - cen[0]) + n[1] * (hole_center[1] - cen[1]) > 0.0);
        ++hole_walls;
    }
    TASK_CHECK(hole_walls == 8);
    return 0;
}

static int test_disconnected_components()
{
    fixture f(7); // 6x6 faces
    std::vector<face_handle> region = {f.face_at(1, 1, 7), f.face_at(4, 4, 7)};
    auto const f0 = f.m.faces().size();
    auto caps = extrude_region(f.m, f.pos, f.uv, f.nrm, region, {0, 0, 1});
    TASK_CHECK(caps.size() == 2);
    f.m.assert_consistency();
    TASK_CHECK(f.m.faces().size() == f0 + 8); // 4 walls each
    return 0;
}

static int test_compactify_and_second_extrude()
{
    fixture f(5);
    std::vector<face_handle> region = {f.face_at(1, 1, 5), f.face_at(2, 1, 5), f.face_at(1, 2, 5), f.face_at(2, 2, 5)};
    auto caps = extrude_region(f.m, f.pos, f.uv, f.nrm, region, {0, 0, 1});
    f.m.assert_consistency();

    f.m.compactify();
    f.m.assert_consistency();
    TASK_CHECK(f.m.is_compact());

    auto cap = f.m.handle_of(face_index(caps[1].idx.value));
    TASK_CHECK(!cap.is_removed());

    auto caps2 = extrude_region(f.m, f.pos, f.uv, f.nrm, {cap}, {0, 0, 1});
    TASK_CHECK(caps2.size() == 1);
    f.m.assert_consistency();

    auto e = f.m.edges().first();
    auto nv = f.m.edges().split(e);
    TASK_CHECK(nv.is_valid());
    f.m.assert_consistency();
    return 0;
}

static int test_reject_bad_handles()
{
    fixture f(4);
    fixture other(3);

    TASK_CHECK(expect_throw(f, {other.face_at(0, 0, 3)}, {0, 0, 1})); // foreign mesh
    TASK_CHECK(expect_throw(f, {face_handle{}}, {0, 0, 1}));          // invalid
    TASK_CHECK(expect_throw(f, {f.m.handle_of(face_index(999))}, {0, 0, 1})); // out of range

    auto f0 = f.face_at(1, 1, 4);
    TASK_CHECK(expect_throw(f, {f0, f0}, {0, 0, 1})); // duplicate

    f.m.faces().remove(f0);
    TASK_CHECK(expect_throw(f, {f0}, {0, 0, 1})); // removed
    return 0;
}

static int test_reject_bad_geometry()
{
    fixture f(4);
    auto region = std::vector<face_handle>{f.face_at(1, 1, 4)};

    TASK_CHECK(expect_throw(f, region, {0, 0, 0}));            // zero
    TASK_CHECK(expect_throw(f, region, {std::nan(""), 0, 1})); // non-finite
    TASK_CHECK(expect_throw(f, region, {0, INFINITY, 1}));
    TASK_CHECK(expect_throw(f, region, {1, 0, 0})); // parallel to a boundary edge
    TASK_CHECK(expect_throw(f, region, {0, -1, 0}));

    // region on the mesh boundary
    {
        fixture g(4);
        TASK_CHECK(expect_throw(g, {g.face_at(0, 0, 4)}, {0, 0, 1}));
    }

    // split face fan at a shared interior vertex
    {
        fixture g(7); // 6x6 faces
        std::vector<face_handle> split_fan = {g.face_at(1, 1, 7), g.face_at(2, 2, 7)};
        TASK_CHECK(expect_throw(g, split_fan, {0, 0, 1}));
    }

    // touching loops: a C-shaped region whose two arms meet at a single vertex
    {
        fixture g(7);
        // faces (1,1) and (1,3) share no vertex; use (1,1),(2,2),(1,3) where (1,1)&(2,2) touch at (2,2)
        // and (2,2)&(1,3) touch at (2,3) -> boundary is a single self-touching loop
        std::vector<face_handle> touching = {g.face_at(1, 1, 7), g.face_at(2, 2, 7), g.face_at(1, 3, 7)};
        TASK_CHECK(expect_throw(g, touching, {0, 0, 1}));
    }

    // non-parallel displacement is accepted
    snapshot before(f);
    auto caps = extrude_region(f.m, f.pos, f.uv, f.nrm, region, {1, 1, 1});
    TASK_CHECK(caps.size() == 1);
    TASK_CHECK(!before.matches(f));
    return 0;
}

static int test_unselected_handles_stable()
{
    fixture f(5); // 4x4 faces
    auto region = std::vector<face_handle>{f.face_at(1, 1, 5)};

    // capture handles and adjacency of everything that is not selected
    auto const region_idx = region[0].idx;
    std::vector<face_handle> unselected_faces;
    for (auto fc : f.m.faces())
        if (fc.idx != region_idx)
            unselected_faces.push_back(fc);

    std::vector<edge_handle> unselected_edges;
    for (auto e : f.m.edges())
    {
        auto a = e.faceA();
        auto b = e.faceB();
        if ((a.is_valid() && a.idx == region_idx) || (b.is_valid() && b.idx == region_idx))
            continue;
        unselected_edges.push_back(e);
    }

    auto face_ids = std::vector<int>();
    for (auto fc : unselected_faces)
        face_ids.push_back(int(fc));
    auto edge_ids = std::vector<std::pair<int, int>>();
    for (auto e : unselected_edges)
        edge_ids.emplace_back(int(e.vertexA()), int(e.vertexB()));

    std::vector<corner_t> face_uv_before;
    for (auto fc : unselected_faces)
        for (auto h : fc.halfedges())
            face_uv_before.push_back(f.uv[h]);

    extrude_region(f.m, f.pos, f.uv, f.nrm, region, {0, 0, 1});
    f.m.assert_consistency();

    // face handles still valid, same count, same indices
    for (auto i = 0u; i < unselected_faces.size(); ++i)
    {
        TASK_CHECK(!unselected_faces[i].is_removed());
        TASK_CHECK(int(unselected_faces[i]) == face_ids[i]);
        TASK_CHECK(unselected_faces[i].idx.value < f.m.all_faces().size());
    }

    // edge handles still valid between the same vertex pairs
    for (auto i = 0u; i < unselected_edges.size(); ++i)
    {
        TASK_CHECK(!unselected_edges[i].is_removed());
        TASK_CHECK(int(unselected_edges[i].vertexA()) == edge_ids[i].first);
        TASK_CHECK(int(unselected_edges[i].vertexB()) == edge_ids[i].second);
    }

    // corner data of unselected faces at the same slot is untouched
    auto k = 0u;
    for (auto fc : unselected_faces)
        for (auto h : fc.halfedges())
        {
            TASK_CHECK(f.uv[h] == face_uv_before[k]);
            ++k;
        }
    return 0;
}

static int test_triangle_mesh()
{
    // 4x4 vertex grid triangulated into 2 triangles per quad -> interior triangles exist
    Mesh m;
    auto pos = m.vertices().make_attribute<pos_t>();
    auto uv = m.halfedges().make_attribute<corner_t>();
    auto nr = m.halfedges().make_attribute<corner_t>();

    auto const N = 4;
    std::vector<vertex_handle> vh(size_t(N) * size_t(N));
    for (auto y = 0; y < N; ++y)
        for (auto x = 0; x < N; ++x)
        {
            auto v = m.vertices().add();
            vh[size_t(y) * size_t(N) + size_t(x)] = v;
            pos[v] = {double(x), double(y), 0, 1};
        }

    auto vert = [&](int x, int y) { return vh[size_t(y) * size_t(N) + size_t(x)]; };
    std::vector<face_handle> tris;
    for (auto y = 0; y < N - 1; ++y)
        for (auto x = 0; x < N - 1; ++x)
        {
            tris.push_back(m.faces().add(vert(x, y), vert(x + 1, y), vert(x + 1, y + 1)));
            tris.push_back(m.faces().add(vert(x, y), vert(x + 1, y + 1), vert(x, y + 1)));
        }
    m.assert_consistency();

    // the quad at (1,1) consists of tris [ (1*3+1)*2 + 0 / +1 ]
    auto const f0 = m.faces().size();
    auto face_index_of = [](int x, int y) { return (y * 3 + x) * 2; };
    std::vector<face_handle> region = {
        m.handle_of(face_index(face_index_of(1, 1) + 0)),
        m.handle_of(face_index(face_index_of(1, 1) + 1)),
    };

    auto caps = extrude_region(m, pos, uv, nr, region, {0, 0, 1});
    TASK_CHECK(caps.size() == 2);
    m.assert_consistency();

    // region boundary = the 4 edges of the quad -> 4 walls
    TASK_CHECK(m.faces().size() == f0 + 4);
    // the shared diagonal vertex pair is re-created as caps; 4 boundary vertices duplicated
    TASK_CHECK(m.vertices().size() == size_t(N * N) + 4);
    for (auto c : caps)
        TASK_CHECK(c.vertices().size() == 3);
    return 0;
}

static int test_reflex_region()
{
    // L-shaped region (reflex corner) in a 5x5 grid of quads -> interior, boundary is a simple loop
    fixture f(6); // 5x5 faces
    std::vector<face_handle> region = {
        f.face_at(1, 1, 6), f.face_at(2, 1, 6), //
        f.face_at(1, 2, 6), f.face_at(1, 3, 6), //
    };

    // count boundary edges of the region before extrusion
    std::vector<char> selected(size_t(f.m.all_faces().size()), 0);
    for (auto fc : region)
        selected[size_t(fc.idx.value)] = 1;
    auto boundary_edges = 0;
    for (auto fc : region)
        for (auto h : fc.halfedges())
        {
            auto opp = h.opposite_face();
            TASK_CHECK(opp.is_valid());
            if (!selected[size_t(opp.idx.value)])
                ++boundary_edges;
        }
    TASK_CHECK(boundary_edges == 10); // L-shape with a reflex corner

    auto const f0 = f.m.faces().size();
    auto caps = extrude_region(f.m, f.pos, f.uv, f.nrm, region, {0, 0, 1});
    TASK_CHECK(caps.size() == 4);
    f.m.assert_consistency();
    TASK_CHECK(f.m.faces().size() == f0 + boundary_edges);
    return 0;
}

static int test_closed_mesh_cube()
{
    // closed manifold (no mesh boundary at all): region vertices are fully surrounded by faces
    Mesh m;
    auto pos = m.vertices().make_attribute<pos_t>();
    auto uv = m.halfedges().make_attribute<corner_t>();
    auto nr = m.halfedges().make_attribute<corner_t>();

    std::vector<vertex_handle> v(8);
    for (auto i = 0; i < 8; ++i)
    {
        v[size_t(i)] = m.vertices().add();
        pos[v[size_t(i)]] = {double((i & 1) != 0), double((i & 2) != 0), double((i & 4) != 0), 1.0};
    }

    // 6 cube quads, CCW outward
    m.faces().add(v[0], v[2], v[6], v[4]); // x = 0
    m.faces().add(v[1], v[5], v[7], v[3]); // x = 1
    m.faces().add(v[0], v[1], v[3], v[2]); // y = 0
    m.faces().add(v[4], v[6], v[7], v[5]); // y = 1
    m.faces().add(v[0], v[4], v[5], v[1]); // z = 0
    m.faces().add(v[2], v[3], v[7], v[6]); // z = 1
    m.assert_consistency();
    TASK_CHECK(!m.halfedges().any([](halfedge_handle h) { return h.is_boundary(); }));

    auto top = m.handle_of(face_index(5));
    auto const f0 = m.faces().size();
    auto const v0 = m.vertices().size();
    // the top face is axis aligned -> use a tilted displacement
    auto caps = extrude_region(m, pos, uv, nr, {top}, {0.5, 0.25, 1});
    TASK_CHECK(caps.size() == 1);
    m.assert_consistency();

    // 4 walls, 4 new cap vertices, 4 new top edges + 4 vertical edges
    TASK_CHECK(m.faces().size() == f0 + 4);
    TASK_CHECK(m.vertices().size() == v0 + 4);
    return 0;
}

static int test_closed_mesh_all_faces()
{
    // selecting every face of a closed mesh: no region boundary at all -> plain translation
    Mesh m;
    auto pos = m.vertices().make_attribute<pos_t>();
    auto uv = m.halfedges().make_attribute<corner_t>();
    auto nr = m.halfedges().make_attribute<corner_t>();

    std::vector<vertex_handle> v(8);
    for (auto i = 0; i < 8; ++i)
    {
        v[size_t(i)] = m.vertices().add();
        pos[v[size_t(i)]] = {double((i & 1) != 0), double((i & 2) != 0), double((i & 4) != 0), 1.0};
    }
    m.faces().add(v[0], v[2], v[6], v[4]);
    m.faces().add(v[1], v[5], v[7], v[3]);
    m.faces().add(v[0], v[1], v[3], v[2]);
    m.faces().add(v[4], v[6], v[7], v[5]);
    m.faces().add(v[0], v[4], v[5], v[1]);
    m.faces().add(v[2], v[3], v[7], v[6]);

    std::vector<face_handle> region;
    for (auto fc : m.faces())
        region.push_back(fc);

    // remember the z coordinates (cube corners are at z = 0 or 1)
    std::vector<double> before_z;
    for (auto vv : m.all_vertices())
        before_z.push_back(pos[vv][2]);

    auto caps = extrude_region(m, pos, uv, nr, region, {0, 0, 1});
    TASK_CHECK(caps.size() == 6);
    m.assert_consistency();

    // all vertices moved, no walls
    TASK_CHECK(m.faces().size() == 6);
    auto i = 0u;
    for (auto vv : m.all_vertices())
        TASK_CHECK(almost(pos[vv][2], before_z[i++] + 1.0));
    return 0;
}

// ================= ATTRIBUTES =================

static int test_cap_inherits_all_four_domains()
{
    fixture f(4);

    // non-trivial attributes, owned directly (not via attribute_collection)
    auto v_str = f.m.vertices().make_attribute<std::string>("v-default");
    auto h_str = f.m.halfedges().make_attribute<std::string>("h-default");
    auto e_str = f.m.edges().make_attribute<std::string>("e-default");
    auto f_str = f.m.faces().make_attribute<std::string>("f-default");

    for (auto v : f.m.vertices())
        v_str[v] = "V" + std::to_string(int(v));
    for (auto h : f.m.halfedges())
        h_str[h] = "H" + std::to_string(int(h));
    for (auto e : f.m.edges())
        e_str[e] = "E" + std::to_string(int(e));
    for (auto fc : f.m.faces())
        f_str[fc] = "F" + std::to_string(int(fc));

    auto region_h = f.face_at(1, 1, 4);
    std::vector<vertex_handle> orig_verts = region_h.vertices().to_vector();
    std::vector<std::string> orig_v_str;
    for (auto v : orig_verts)
        orig_v_str.push_back(v_str[v]);

    auto caps = extrude_region(f.m, f.pos, f.uv, f.nrm, {region_h}, {0, 0, 1});
    f.m.assert_consistency();

    // cap vertices: inherited vertex slot, not the default
    auto cap_verts = caps[0].vertices().to_vector();
    TASK_CHECK(cap_verts.size() == 4);
    for (auto cv : cap_verts)
    {
        TASK_CHECK(v_str[cv].rfind("V", 0) == 0); // a real copied value
        bool found = false;
        for (auto i = 0u; i < orig_verts.size(); ++i)
        {
            auto p = f.pos[orig_verts[i]];
            p[2] += 1.0;
            if (f.pos[cv] == p)
            {
                TASK_CHECK(v_str[cv] == orig_v_str[i]);
                found = true;
            }
        }
        TASK_CHECK(found);
    }

    // cap corners inherit the original corner slots (keyed by vertex_to, ring start may rotate)
    {
        auto cap_h_str = caps[0].halfedges().to_vector();
        TASK_CHECK(cap_h_str.size() == 4);
        for (auto ch : caps[0].halfedges())
        {
            bool found = false;
            for (auto oh : region_h.halfedges())
                if (oh.vertex_to() == ch.vertex_to())
                {
                    TASK_CHECK(h_str[ch] == h_str[oh]);
                    found = true;
                }
            TASK_CHECK(found);
        }
        // and no cap corner kept the old selected-face default either
        for (auto ch : cap_h_str)
            TASK_CHECK(h_str[ch] != "h-default");
    }

    // wall faces inherit the adjacent original selected face
    auto inherited_walls = 0;
    for (auto fc : f.m.faces())
        if (f_str[fc] == "F" + std::to_string(int(region_h)))
            inherited_walls++;
    TASK_CHECK(inherited_walls == 5); // cap + 4 walls

    // cap edges inherit the original boundary edges
    auto inherited_edges = 0;
    for (auto e : caps[0].edges())
        if (e_str[e] != "e-default")
            inherited_edges++;
    TASK_CHECK(inherited_edges == 4);

    // vertical edges keep their default value
    auto vertical_defaults = 0;
    for (auto e : f.m.edges())
        if (e_str[e] == "e-default")
            vertical_defaults++;
    TASK_CHECK(vertical_defaults == 4);
    return 0;
}

static int test_unselected_slots_untouched()
{
    fixture f(5);
    auto region = std::vector<face_handle>{f.face_at(1, 1, 5)};

    std::vector<char> was_unselected(size_t(f.m.all_faces().size()), 0);
    for (auto fc : f.m.all_faces())
        if (fc.idx != region[0].idx)
            was_unselected[size_t(fc.idx.value)] = 1;
    auto const original_face_count = f.m.all_faces().size();

    std::vector<corner_t> before_uv;
    std::vector<corner_t> before_nrm;
    for (auto h : f.m.all_halfedges())
    {
        before_uv.push_back(f.uv[h]);
        before_nrm.push_back(f.nrm[h]);
    }

    extrude_region(f.m, f.pos, f.uv, f.nrm, region, {0, 0, 1});

    for (auto h : f.m.all_halfedges())
    {
        if (h.is_removed())
            continue;
        auto fc = h.face();
        if (!fc.is_valid() || fc.idx.value >= original_face_count)
            continue; // walls are new faces, not "unselected" ones
        if (!was_unselected[size_t(fc.idx.value)])
            continue;

        // untouched face corners keep the original value at the same slot
        TASK_CHECK(f.uv[h] == before_uv[size_t(h.idx.value)]);
        TASK_CHECK(f.nrm[h] == before_nrm[size_t(h.idx.value)]);
    }
    return 0;
}

static int test_wall_uv_and_normals()
{
    fixture f(5); // 4x4 faces
    auto region = std::vector<face_handle>{f.face_at(1, 1, 5)};

    // expected u starts at the smallest boundary halfedge of the loop
    std::vector<halfedge_index> boundary_hs;
    std::vector<double> boundary_len;
    for (auto h : region[0].halfedges())
    {
        boundary_hs.push_back(h.idx);
        auto d = sub3(f.pos[h.vertex_to()], f.pos[h.vertex_from()]);
        boundary_len.push_back(len3(d));
    }
    auto first = 0u;
    for (auto i = 1u; i < boundary_hs.size(); ++i)
        if (boundary_hs[i].value < boundary_hs[first].value)
            first = i;

    auto const n = int(boundary_hs.size());
    std::vector<double> expected_u(size_t(n) + 1, 1e30);
    expected_u[0] = 0.0;
    for (auto i = 0; i < n; ++i)
        expected_u[size_t(i) + 1] = expected_u[size_t(i)] + boundary_len[size_t((first + i) % n)];

    extrude_region(f.m, f.pos, f.uv, f.nrm, region, {0, 0, 3});
    f.m.assert_consistency();

    // walls are the new faces (all faces beyond the original 16)
    auto walls = std::vector<face_handle>();
    for (auto fc : f.m.faces())
        if (fc.idx.value >= 16)
            walls.push_back(fc);
    TASK_CHECK(walls.size() == 4);

    auto bottom_corners = 0, top_corners = 0;
    auto min_u = 1e30, max_u = -1e30;
    for (auto w : walls)
    {
        auto wn = face_normal_impl(w, f.pos);
        TASK_CHECK(almost(std::sqrt(wn[0] * wn[0] + wn[1] * wn[1]), 1.0));
        TASK_CHECK(almost(wn[2], 0.0));

        for (auto h : w.halfedges())
        {
            TASK_CHECK(almost(f.uv[h][2], 0.0));
            TASK_CHECK(almost(f.nrm[h][0], wn[0]));
            TASK_CHECK(almost(f.nrm[h][1], wn[1]));
            TASK_CHECK(almost(f.nrm[h][2], wn[2]));

            min_u = std::min(min_u, f.uv[h][0]);
            max_u = std::max(max_u, f.uv[h][0]);

            if (almost(f.uv[h][1], 0.0))
                bottom_corners++;
            if (almost(f.uv[h][1], 3.0))
                top_corners++;
        }
    }
    // 2 bottom + 2 top corners per wall
    TASK_CHECK(bottom_corners == 8);
    TASK_CHECK(top_corners == 8);
    TASK_CHECK(almost(min_u, 0.0));
    TASK_CHECK(almost(max_u, 4.0)); // unit quad perimeter
    TASK_CHECK(almost(expected_u[size_t(n)], 4.0));
    return 0;
}

static int test_hole_uv_separate_perimeter()
{
    fixture f(7);
    std::vector<face_handle> ring;
    for (auto y = 1; y <= 4; ++y)
        for (auto x = 1; x <= 4; ++x)
            if (!(x >= 2 && x <= 3 && y >= 2 && y <= 3))
                ring.push_back(f.face_at(x, y, 7));

    extrude_region(f.m, f.pos, f.uv, f.nrm, ring, {0, 0, 1});
    f.m.assert_consistency();

    // each loop starts at u=0 independently; the hole perimeter is 8, the outer one 16
    auto has_u0 = false, has_u8 = false, has_u16 = false;
    for (auto h : f.m.halfedges())
    {
        if (!almost(f.uv[h][1], 1.0)) // top rim
            continue;
        has_u0 |= almost(f.uv[h][0], 0.0);
        has_u8 |= almost(f.uv[h][0], 8.0);
        has_u16 |= almost(f.uv[h][0], 16.0);
    }
    TASK_CHECK(has_u0);
    TASK_CHECK(has_u8);
    TASK_CHECK(has_u16);
    return 0;
}

static int test_shared_corner_uv_closure()
{
    fixture f(4);
    auto region = std::vector<face_handle>{f.face_at(1, 1, 4)};
    extrude_region(f.m, f.pos, f.uv, f.nrm, region, {0, 0, 2});
    f.m.assert_consistency();

    // at the ring closure, the same top point appears once with u=0 and once with u=perimeter
    auto z0 = 0, z4 = 0;
    for (auto h : f.m.halfedges())
    {
        if (!almost(f.uv[h][1], 2.0))
            continue;
        if (almost(f.uv[h][0], 0.0))
            z0++;
        if (almost(f.uv[h][0], 4.0))
            z4++;
    }
    TASK_CHECK(z0 == 1);
    TASK_CHECK(z4 == 1);
    return 0;
}

static int test_nontrivial_struct_attributes()
{
    // A user type that is definitely not trivially copyable and whose game is to notice
    // a shallow byte copy (v[1] would alias v[0] if copied via memcpy)
    struct tagged
    {
        std::vector<int> values;
        std::string label;
        int id = -1;

        tagged() = default;
        explicit tagged(int i) : values{i, i * 2, i * 3}, label("L" + std::to_string(i)), id(i) {}

        bool intact() const { return int(values.size()) == 3 && values[0] == id && values[1] == 2 * id && values[2] == 3 * id && label == "L" + std::to_string(id); }
    };

    fixture f(5);
    auto vt = f.m.vertices().make_attribute<tagged>(tagged(-1));
    auto ht = f.m.halfedges().make_attribute<tagged>(tagged(-1));
    auto et = f.m.edges().make_attribute<tagged>(tagged(-1));
    auto ft = f.m.faces().make_attribute<tagged>(tagged(-1));

    for (auto v : f.m.vertices())
        vt[v] = tagged(int(v) + 1000);
    for (auto h : f.m.halfedges())
        ht[h] = tagged(int(h) + 2000);
    for (auto e : f.m.edges())
        et[e] = tagged(int(e) + 3000);
    for (auto fc : f.m.faces())
        ft[fc] = tagged(int(fc) + 4000);

    std::vector<face_handle> region = {f.face_at(1, 1, 5), f.face_at(2, 1, 5)};
    auto region_face_ids = std::vector<int>();
    for (auto fc : region)
        region_face_ids.push_back(int(fc));
    auto orig_vertex_ids = std::vector<int>();
    for (auto fc : region)
        for (auto v : fc.vertices())
            orig_vertex_ids.push_back(vt[v].id);

    auto caps = extrude_region(f.m, f.pos, f.uv, f.nrm, region, {0, 0, 1});
    f.m.assert_consistency();

    // everything still intact (no aliased shallow copies, no leaks of the wrong slot)
    for (auto v : f.m.all_vertices())
        if (!v.is_removed())
            TASK_CHECK(vt[v].intact());
    for (auto h : f.m.all_halfedges())
        if (!h.is_removed())
            TASK_CHECK(ht[h].intact());
    for (auto e : f.m.all_edges())
        if (!e.is_removed())
            TASK_CHECK(et[e].intact());
    for (auto fc : f.m.all_faces())
        if (!fc.is_removed())
            TASK_CHECK(ft[fc].intact());

    // cap vertices inherited real values (not the default)
    for (auto c : caps)
        for (auto cv : c.vertices())
        {
            TASK_CHECK(vt[cv].id != -1);
            bool found = false;
            for (auto id : orig_vertex_ids)
                found |= (vt[cv].id == id);
            TASK_CHECK(found);
        }

    // wall faces inherited the selected face's value
    auto wall_count = 0;
    for (auto fc : f.m.faces())
    {
        bool is_cap = false;
        for (auto c : caps)
            is_cap |= (c == fc);
        if (is_cap)
            continue;
        if (fc.idx.value < region_face_ids.back())
            continue;
        if (ft[fc].id == region_face_ids[0] + 4000 || ft[fc].id == region_face_ids[1] + 4000)
            wall_count++;
    }
    TASK_CHECK(wall_count == 6); // 3 boundary edges per face in the 1x2 patch, 6 total

    // vertical edges still hold the default
    auto defaults = 0;
    for (auto e : f.m.edges())
        if (et[e].id == -1)
            defaults++;
    TASK_CHECK(defaults == 6);
    return 0;
}

// ================= PERSISTENCE =================

static void assign_unique_corners(fixture& f)
{
    auto i = 0;
    for (auto h : f.m.halfedges())
    {
        f.uv[h] = {0.01 * (i + 1), 0.02 * (i + 1), 0.03 * (i + 1)};
        f.nrm[h] = {0.1 * (i + 1), 0.2 * (i + 1), 0.3 * (i + 1)};
        ++i;
    }
}

bool corner_close(corner_t const& a, corner_t const& b, double eps = 1e-4)
{
    // OBJ is written with the stream's default precision, so compare with a small tolerance
    return almost(a[0], b[0], eps) && almost(a[1], b[1], eps) && almost(a[2], b[2], eps);
}

// returns an error code (0 on success) since TASK_CHECK returns from the enclosing function
static int check_obj_roundtrip(fixture& f, std::vector<face_handle> const& caps, char const* what)
{
    std::ostringstream out;
    {
        obj_writer<double> w(out);
        w.write_mesh(f.pos, &f.uv, &f.nrm);
    }
    TASK_CHECK(!out.str().empty());

    Mesh m2;
    std::istringstream in(out.str());
    obj_reader<double> r(in, m2);
    TASK_CHECK(r.error_faces() == 0);
    TASK_CHECK(m2.faces().size() == f.m.faces().size());
    TASK_CHECK(m2.vertices().size() == f.m.vertices().size());
    m2.assert_consistency();

    auto uv2 = r.get_tex_coords();
    auto nrm2 = r.get_normals();

    // every face corner must round-trip (keyed by vertex_to, as the ring start may rotate)
    for (auto fc : f.m.faces())
    {
        auto fc2 = m2.handle_of(face_index(fc.idx.value));
        TASK_CHECK(fc.vertices().size() == fc2.vertices().size());

        auto matched = 0;
        for (auto h : fc.halfedges())
            for (auto h2 : fc2.halfedges())
                if (int(h.vertex_to()) == int(h2.vertex_to()))
                {
                    TASK_CHECK(corner_close(f.uv[h], uv2[h2]));
                    TASK_CHECK(corner_close(f.nrm[h], nrm2[h2]));
                    ++matched;
                }
        TASK_CHECK(matched == fc.halfedges().size());
    }

    // the caps in particular
    for (auto c : caps)
        TASK_CHECK(m2.handle_of(face_index(c.idx.value)).vertices().size() == c.vertices().size());
    (void)what;
    return 0;
}

static int test_obj_roundtrip_preserves_caps()
{
    fixture f(5);
    assign_unique_corners(f);
    std::vector<face_handle> region = {f.face_at(1, 1, 5), f.face_at(2, 1, 5), f.face_at(1, 2, 5), f.face_at(2, 2, 5)};
    auto caps = extrude_region(f.m, f.pos, f.uv, f.nrm, region, {0, 0, 1});
    f.m.assert_consistency();
    f.m.compactify();
    f.m.assert_consistency();

    return check_obj_roundtrip(f, caps, "caps");
    return 0;
}

static int test_obj_roundtrip_with_hole()
{
    fixture f(7);
    assign_unique_corners(f);
    std::vector<face_handle> ring;
    for (auto y = 1; y <= 4; ++y)
        for (auto x = 1; x <= 4; ++x)
            if (!(x >= 2 && x <= 3 && y >= 2 && y <= 3))
                ring.push_back(f.face_at(x, y, 7));

    auto caps = extrude_region(f.m, f.pos, f.uv, f.nrm, ring, {0, 0, 1});
    f.m.assert_consistency();

    return check_obj_roundtrip(f, caps, "hole");
    return 0;
}

static int test_compact_edit_then_second_extrude_and_split()
{
    fixture f(6);
    assign_unique_corners(f);

    std::vector<face_handle> region = {f.face_at(1, 1, 6), f.face_at(2, 1, 6), f.face_at(1, 2, 6), f.face_at(2, 2, 6)};
    auto caps = extrude_region(f.m, f.pos, f.uv, f.nrm, region, {0, 0, 1});
    f.m.assert_consistency();
    f.m.compactify();
    f.m.assert_consistency();

    // compactified, continuous edit: second extrusion on a re-resolved cap
    auto cap = f.m.handle_of(face_index(caps[0].idx.value));
    auto caps2 = extrude_region(f.m, f.pos, f.uv, f.nrm, {cap}, {0, 0, 1});
    TASK_CHECK(caps2.size() == 1);
    f.m.assert_consistency();

    // still compactible, and edge splitting still works
    f.m.compactify();
    TASK_CHECK(f.m.is_compact());
    f.m.edges().split(f.m.edges().first());
    f.m.assert_consistency();
    return 0;
}

static int test_attribute_collection_survives()
{
    fixture f(4);
    auto ac = attribute_collection();
    auto vtag = f.m.vertices().make_attribute<int>(7);
    for (auto v : f.m.vertices())
        vtag[v] = int(v) + 100;
    ac["tag"] = vtag;

    auto region = std::vector<face_handle>{f.face_at(1, 1, 4)};
    extrude_region(f.m, f.pos, f.uv, f.nrm, region, {0, 0, 1});
    f.m.assert_consistency();

    // both the standalone attribute and the collection copy were updated by the slot copy
    auto& stored = ac["tag"].vertex<int>();
    for (auto v : f.m.all_vertices())
    {
        TASK_CHECK(vtag[v] == stored[v]);
        TASK_CHECK(stored[v] >= 100);
    }

    // cap vertex slots are real inherited values, not the default
    auto caps = extrude_region(f.m, f.pos, f.uv, f.nrm, {f.m.handle_of(face_index(region[0].idx.value))}, {0, 0, 1});
    TASK_CHECK(caps.size() == 1);
    for (auto v : caps[0].vertices())
        TASK_CHECK(stored[v] != 7);
    return 0;
}

// ================= REGISTRATION =================

const struct task_case task_cases[] = {
    {"topology", "empty_region", test_empty_region},
    {"topology", "single_interior_face", test_single_interior_face},
    {"topology", "multi_face_shared_inside", test_multi_face_shared_inside},
    {"topology", "hole_region", test_hole_region},
    {"topology", "disconnected_components", test_disconnected_components},
    {"topology", "compactify_and_second_extrude", test_compactify_and_second_extrude},
    {"topology", "reject_bad_handles", test_reject_bad_handles},
    {"topology", "reject_bad_geometry", test_reject_bad_geometry},
    {"topology", "unselected_handles_stable", test_unselected_handles_stable},
    {"topology", "triangle_mesh", test_triangle_mesh},
    {"topology", "reflex_region", test_reflex_region},
    {"topology", "closed_mesh_cube", test_closed_mesh_cube},
    {"topology", "closed_mesh_all_faces", test_closed_mesh_all_faces},

    {"attributes", "cap_inherits_all_four_domains", test_cap_inherits_all_four_domains},
    {"attributes", "unselected_slots_untouched", test_unselected_slots_untouched},
    {"attributes", "wall_uv_and_normals", test_wall_uv_and_normals},
    {"attributes", "hole_uv_separate_perimeter", test_hole_uv_separate_perimeter},
    {"attributes", "shared_corner_uv_closure", test_shared_corner_uv_closure},
    {"attributes", "nontrivial_struct_attributes", test_nontrivial_struct_attributes},

    {"persistence", "obj_roundtrip_preserves_caps", test_obj_roundtrip_preserves_caps},
    {"persistence", "obj_roundtrip_with_hole", test_obj_roundtrip_with_hole},
    {"persistence", "compact_edit_then_second_extrude_and_split", test_compact_edit_then_second_extrude_and_split},
    {"persistence", "attribute_collection_survives", test_attribute_collection_survives},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
