// Regression tests for polymesh::normal_bake_session.
//
// Groups: projection, invalidation, locality.
// The test runner (task_support/driver.c) supplies main().

#include "task_support/check.h"

#include <polymesh/Mesh.hh>
#include <polymesh/algorithms/normal_baker.hh>
#include <polymesh/detail/segment_index.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace
{
// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

using pos4 = std::array<double, 4>;
using uv3 = std::array<double, 3>;
using d3 = std::array<double, 3>;
using d2 = std::array<double, 2>;

double const TOL_N = 1e-9;

bool nearly(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool nearly3(d3 const& a, d3 const& b, double tol)
{
    return nearly(a[0], b[0], tol) && nearly(a[1], b[1], tol) && nearly(a[2], b[2], tol);
}

pos4 make_p4(d3 const& p) { return pos4{{p[0], p[1], p[2], 1.0}}; }
uv3 make_uv(d2 const& t) { return uv3{{t[0], t[1], 0.0}}; }

pm::vertex_handle add_vertex(pm::Mesh& m, pm::vertex_attribute<pos4>& position, d3 const& p)
{
    auto v = m.vertices().add();
    position[v] = make_p4(p);
    return v;
}

void set_corner_uvs(pm::Mesh& m, pm::halfedge_attribute<uv3>& uv, pm::face_handle f, std::array<pm::vertex_handle, 3> const& vs,
                    std::array<d2, 3> const& corner_uvs)
{
    (void)m;
    for (auto h : f.halfedges())
    {
        auto const vi = h.vertex_to().idx.value;
        for (auto k = 0; k < 3; ++k)
            if (vi == vs[size_t(k)].idx.value)
                uv[h] = make_uv(corner_uvs[size_t(k)]);
    }
}

pm::face_handle add_low_tri(pm::Mesh& m, pm::vertex_attribute<pos4>& position, pm::halfedge_attribute<uv3>& uv, pm::vertex_handle a,
                            pm::vertex_handle b, pm::vertex_handle c, d2 const& ua, d2 const& ub, d2 const& uc)
{
    auto f = m.faces().add(a, b, c);
    set_corner_uvs(m, uv, f, {{a, b, c}}, {{ua, ub, uc}});
    (void)position;
    return f;
}

pm::face_handle add_high_tri(pm::Mesh& m, pm::vertex_attribute<pos4>& position, pm::vertex_handle a, pm::vertex_handle b, pm::vertex_handle c)
{
    (void)position;
    return m.faces().add(a, b, c);
}

/// low-poly unit square in [-1,1]^2 at z, two CCW triangles, UVs covering [0,1]^2.
/// `mirror_u` flips the u coordinate, producing mirrored (negative-area) UVs.
void make_low_square(pm::Mesh& m, pm::vertex_attribute<pos4>& position, pm::halfedge_attribute<uv3>& uv, double z, bool mirror_u = false)
{
    auto v0 = add_vertex(m, position, d3{{-1.0, -1.0, z}});
    auto v1 = add_vertex(m, position, d3{{1.0, -1.0, z}});
    auto v2 = add_vertex(m, position, d3{{1.0, 1.0, z}});
    auto v3 = add_vertex(m, position, d3{{-1.0, 1.0, z}});

    auto flip = [&](double u) { return mirror_u ? 1.0 - u : u; };
    add_low_tri(m, position, uv, v0, v1, v2, d2{{flip(0.0), 0.0}}, d2{{flip(1.0), 0.0}}, d2{{flip(1.0), 1.0}});
    add_low_tri(m, position, uv, v0, v2, v3, d2{{flip(0.0), 0.0}}, d2{{flip(1.0), 1.0}}, d2{{flip(0.0), 1.0}});
}

/// high-poly quad at constant z covering [-1,1]^2, two CCW triangles (normal +z)
void make_high_plane(pm::Mesh& m, pm::vertex_attribute<pos4>& position, double z)
{
    auto v0 = add_vertex(m, position, d3{{-1.0, -1.0, z}});
    auto v1 = add_vertex(m, position, d3{{1.0, -1.0, z}});
    auto v2 = add_vertex(m, position, d3{{1.0, 1.0, z}});
    auto v3 = add_vertex(m, position, d3{{-1.0, 1.0, z}});
    add_high_tri(m, position, v0, v1, v2);
    add_high_tri(m, position, v0, v2, v3);
}

using session_t = pm::normal_bake_session;
using texel_t = pm::normal_bake_texel;

/// Both results must agree. `tol_t` scales with max(1, r) at the call site.
bool compare_results(std::vector<texel_t> const& a, std::vector<texel_t> const& b, double tol_t, double tol_n)
{
    if (a.size() != b.size())
        return false;
    for (auto i = 0u; i < a.size(); ++i)
    {
        auto const& x = a[i];
        auto const& y = b[i];
        if (x.coverage != y.coverage || x.hit != y.hit)
            return false;
        if (x.low_face != y.low_face)
            return false;
        if (x.hit)
        {
            if (x.high_face != y.high_face)
                return false;
            if (!nearly(x.t, y.t, tol_t))
                return false;
            if (!nearly3(x.normal, y.normal, tol_n))
                return false;
        }
        else
        {
            if (x.high_face != y.high_face)
                return false;
            if (x.t != 0.0 || x.normal != d3{{0.0, 0.0, 0.0}})
                return false;
        }
    }
    return true;
}

bool same_results_exact(std::vector<texel_t> const& a, std::vector<texel_t> const& b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = 0u; i < a.size(); ++i)
    {
        if (a[i].coverage != b[i].coverage || a[i].hit != b[i].hit || a[i].low_face != b[i].low_face || a[i].high_face != b[i].high_face)
            return false;
        if (a[i].t != b[i].t || a[i].normal != b[i].normal)
            return false;
    }
    return true;
}

bool same_update(pm::normal_bake_update const& a, pm::normal_bake_update const& b)
{
    return a.faces == b.faces && a.texels == b.texels && a.nodes == b.nodes;
}

int count_covered(std::vector<texel_t> const& r)
{
    auto c = 0;
    for (auto const& t : r)
        if (t.coverage)
            ++c;
    return c;
}

int count_hits(std::vector<texel_t> const& r)
{
    auto c = 0;
    for (auto const& t : r)
        if (t.hit)
            ++c;
    return c;
}

// ---------------------------------------------------------------------------
// projection group
// ---------------------------------------------------------------------------

bool check_projection_basic()
{
    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    make_high_plane(high, hpos, 0.1);

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    make_low_square(low, lpos, luv, 0.0);

    auto const width = 8;
    auto const height = 8;
    auto const r = 0.5;
    session_t s(hpos, lpos, luv, width, height, r);

    auto const& res = s.result();
    if (int(res.size()) != width * height)
        return false;

    auto hits = 0;
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
        {
            auto const& t = res[size_t(y * width + x)];
            if (!t.coverage || !t.hit)
                return false;
            if (!nearly(t.t, 0.4, 1e-12))
                return false;
            if (!nearly3(t.normal, d3{{0.0, 0.0, 1.0}}, TOL_N))
                return false;
            if (t.low_face != 0 && t.low_face != 1)
                return false;
            ++hits;
        }
    if (hits != width * height)
        return false;

    // both low triangles must own some texels
    auto face0 = 0;
    auto face1 = 0;
    for (auto const& t : res)
        (t.low_face == 0 ? face0 : face1)++;
    if (face0 == 0 || face1 == 0)
        return false;
    return true;
}

bool check_projection_mirrored_uv()
{
    // Tilted high surface so the tangent frame affects the encoded normal.
    auto const build = [](pm::Mesh& high, pm::vertex_attribute<pos4>& hpos, pm::Mesh& low, pm::vertex_attribute<pos4>& lpos,
                          pm::halfedge_attribute<uv3>& luv, bool mirror) {
        auto v0 = add_vertex(high, hpos, d3{{-1.0, -1.0, 0.1}});
        auto v1 = add_vertex(high, hpos, d3{{1.0, -1.0, 0.2}});
        auto v2 = add_vertex(high, hpos, d3{{1.0, 1.0, 0.2}});
        auto v3 = add_vertex(high, hpos, d3{{-1.0, 1.0, 0.1}});
        add_high_tri(high, hpos, v0, v1, v2);
        add_high_tri(high, hpos, v0, v2, v3);
        make_low_square(low, lpos, luv, 0.0, mirror);
    };

    auto const expect_encoded = [](double tangent_x_sign) {
        // Nh = normalize(cross((2,0,0.1),(2,2,0.1))) = normalize((-0.2, 0, 4))
        auto const len = std::sqrt(0.2 * 0.2 + 4.0 * 4.0);
        d3 nh{{-0.2 / len, 0.0, 4.0 / len}};
        // T = (sign, 0, 0), B = (0, 1, 0), N = (0, 0, 1)
        return d3{{tangent_x_sign * nh[0], nh[1], nh[2]}};
    };

    {
        pm::Mesh high;
        pm::vertex_attribute<pos4> hpos(high);
        pm::Mesh low;
        pm::vertex_attribute<pos4> lpos(low);
        pm::halfedge_attribute<uv3> luv(low);
        build(high, hpos, low, lpos, luv, false);

        session_t s(hpos, lpos, luv, 8, 8, 0.5);
        auto const& res = s.result();
        if (count_hits(res) != 64)
            return false;
        auto const expected = expect_encoded(1.0);
        for (auto const& t : res)
            if (!t.hit || !nearly3(t.normal, expected, TOL_N))
                return false;
    }

    {
        pm::Mesh high;
        pm::vertex_attribute<pos4> hpos(high);
        pm::Mesh low;
        pm::vertex_attribute<pos4> lpos(low);
        pm::halfedge_attribute<uv3> luv(low);
        build(high, hpos, low, lpos, luv, true);

        session_t s(hpos, lpos, luv, 8, 8, 0.5);
        auto const& res = s.result();
        if (count_hits(res) != 64)
            return false;
        // mirrored UVs flip the tangent direction, hence the sign of the x term
        auto const expected = expect_encoded(-1.0);
        for (auto const& t : res)
            if (!t.hit || !nearly3(t.normal, expected, TOL_N))
                return false;
    }

    return true;
}

bool check_projection_shared_edge()
{
    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    make_high_plane(high, hpos, 0.1);

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    make_low_square(low, lpos, luv, 0.0);

    auto const width = 8;
    auto const height = 8;
    session_t s(hpos, lpos, luv, width, height, 0.5);
    auto const& res = s.result();

    // the shared diagonal must not create gaps or double coverage
    for (auto const& t : res)
        if (!t.coverage)
            return false;

    // texels whose centers lie exactly on the shared diagonal belong to the first triangle
    for (auto i = 0; i < width; ++i)
    {
        auto const& t = res[size_t(i * width + i)];
        if (t.low_face != 0)
            return false;
    }
    return true;
}

bool check_projection_backface()
{
    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    // reversed winding => geometric normal (0, 0, -1)
    auto v0 = add_vertex(high, hpos, d3{{-1.0, -1.0, -0.1}});
    auto v1 = add_vertex(high, hpos, d3{{1.0, 1.0, -0.1}});
    auto v2 = add_vertex(high, hpos, d3{{1.0, -1.0, -0.1}});
    auto v3 = add_vertex(high, hpos, d3{{-1.0, 1.0, -0.1}});
    add_high_tri(high, hpos, v0, v1, v2);
    add_high_tri(high, hpos, v0, v3, v1);

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    make_low_square(low, lpos, luv, 0.0);

    session_t s(hpos, lpos, luv, 6, 6, 0.5);
    auto const& res = s.result();
    for (auto const& t : res)
    {
        if (!t.hit)
            return false;
        if (!nearly(t.t, 0.6, 1e-12))
            return false;
        // must NOT be flipped to face the ray
        if (!nearly3(t.normal, d3{{0.0, 0.0, -1.0}}, TOL_N))
            return false;
    }
    return true;
}

bool check_projection_equal_distance_tie()
{
    // two coincident layers at the same distance; the lower original face index wins
    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);

    // faces 0, 1: reversed winding (normal -z)
    auto a0 = add_vertex(high, hpos, d3{{-1.0, -1.0, 0.1}});
    auto a1 = add_vertex(high, hpos, d3{{1.0, 1.0, 0.1}});
    auto a2 = add_vertex(high, hpos, d3{{1.0, -1.0, 0.1}});
    auto a3 = add_vertex(high, hpos, d3{{-1.0, 1.0, 0.1}});
    add_high_tri(high, hpos, a0, a1, a2);
    add_high_tri(high, hpos, a0, a3, a1);

    // faces 2, 3: normal +z
    auto b0 = add_vertex(high, hpos, d3{{-1.0, -1.0, 0.1}});
    auto b1 = add_vertex(high, hpos, d3{{1.0, -1.0, 0.1}});
    auto b2 = add_vertex(high, hpos, d3{{1.0, 1.0, 0.1}});
    auto b3 = add_vertex(high, hpos, d3{{-1.0, 1.0, 0.1}});
    add_high_tri(high, hpos, b0, b1, b2);
    add_high_tri(high, hpos, b0, b2, b3);

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    make_low_square(low, lpos, luv, 0.0);

    session_t s(hpos, lpos, luv, 8, 8, 0.5);
    auto const& res = s.result();
    for (auto const& t : res)
    {
        if (!t.hit)
            return false;
        if (t.high_face != 0 && t.high_face != 1)
            return false;
        if (!nearly3(t.normal, d3{{0.0, 0.0, -1.0}}, TOL_N))
            return false;
    }
    return true;
}

bool check_projection_closed_interval()
{
    auto const r = 0.5;

    // t == 0 (high plane exactly at P + r*N)
    {
        pm::Mesh high;
        pm::vertex_attribute<pos4> hpos(high);
        make_high_plane(high, hpos, r);
        pm::Mesh low;
        pm::vertex_attribute<pos4> lpos(low);
        pm::halfedge_attribute<uv3> luv(low);
        make_low_square(low, lpos, luv, 0.0);
        session_t s(hpos, lpos, luv, 4, 4, r);
        for (auto const& t : s.result())
            if (!t.hit || !nearly(t.t, 0.0, 1e-12))
                return false;
    }

    // t == 2r (high plane exactly at P - r*N)
    {
        pm::Mesh high;
        pm::vertex_attribute<pos4> hpos(high);
        make_high_plane(high, hpos, -r);
        pm::Mesh low;
        pm::vertex_attribute<pos4> lpos(low);
        pm::halfedge_attribute<uv3> luv(low);
        make_low_square(low, lpos, luv, 0.0);
        session_t s(hpos, lpos, luv, 4, 4, r);
        for (auto const& t : s.result())
            if (!t.hit || !nearly(t.t, 2.0 * r, 1e-12))
                return false;
    }

    // just beyond 2r: miss
    {
        pm::Mesh high;
        pm::vertex_attribute<pos4> hpos(high);
        make_high_plane(high, hpos, -r - 1e-6);
        pm::Mesh low;
        pm::vertex_attribute<pos4> lpos(low);
        pm::halfedge_attribute<uv3> luv(low);
        make_low_square(low, lpos, luv, 0.0);
        session_t s(hpos, lpos, luv, 4, 4, r);
        for (auto const& t : s.result())
        {
            if (!t.coverage || t.hit)
                return false;
            if (t.high_face != -1 || t.t != 0.0 || t.normal != d3{{0.0, 0.0, 0.0}})
                return false;
        }
    }
    return true;
}

bool check_projection_ignores_zero_uv_area()
{
    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    make_high_plane(high, hpos, 0.1);

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    make_low_square(low, lpos, luv, 0.0);

    // an extra triangle far away with a collapsed UV triangle (zero UV area)
    auto v0 = add_vertex(low, lpos, d3{{10.0, 10.0, 0.0}});
    auto v1 = add_vertex(low, lpos, d3{{12.0, 10.0, 0.0}});
    auto v2 = add_vertex(low, lpos, d3{{10.0, 12.0, 0.0}});
    add_low_tri(low, lpos, luv, v0, v1, v2, d2{{0.5, 0.5}}, d2{{0.5, 0.5}}, d2{{0.5, 0.5}});

    session_t s(hpos, lpos, luv, 8, 8, 0.5);
    auto const& res = s.result();
    if (count_covered(res) != 64)
        return false;
    for (auto const& t : res)
        if (t.low_face != 0 && t.low_face != 1)
            return false;
    return true;
}

bool check_projection_v_up()
{
    // high-poly patch only over the lower half (world y in [-1, 0], i.e. v < 0.5)
    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    auto v0 = add_vertex(high, hpos, d3{{-1.0, -1.0, 0.1}});
    auto v1 = add_vertex(high, hpos, d3{{1.0, -1.0, 0.1}});
    auto v2 = add_vertex(high, hpos, d3{{1.0, 0.0, 0.1}});
    auto v3 = add_vertex(high, hpos, d3{{-1.0, 0.0, 0.1}});
    add_high_tri(high, hpos, v0, v1, v2);
    add_high_tri(high, hpos, v0, v2, v3);

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    make_low_square(low, lpos, luv, 0.0);

    auto const width = 8;
    auto const height = 8;
    session_t s(hpos, lpos, luv, width, height, 0.5);
    auto const& res = s.result();

    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
        {
            auto const& t = res[size_t(y * width + x)];
            if (!t.coverage)
                return false;
            if (y < height / 2)
            {
                if (!t.hit || !nearly(t.t, 0.4, 1e-12))
                    return false;
            }
            else
            {
                // v >= 0.5 lies above the high patch
                if (t.hit || t.high_face != -1 || t.t != 0.0)
                    return false;
                if (t.low_face < 0)
                    return false;
            }
        }
    return true;
}

// ---------------------------------------------------------------------------
// invalidation group
// ---------------------------------------------------------------------------

bool check_projection_constructor_validation()
{
    auto expect_invalid = [](int width, int height, double r, char mode) {
        pm::Mesh high;
        pm::vertex_attribute<pos4> hpos(high);
        make_high_plane(high, hpos, 0.1);
        pm::Mesh low;
        pm::vertex_attribute<pos4> lpos(low);
        pm::halfedge_attribute<uv3> luv(low);
        make_low_square(low, lpos, luv, 0.0);

        if (mode == 'w')
            hpos.data()[0] = pos4{{0.0, 0.0, 0.1, 2.0}};
        else if (mode == 'f')
            hpos.data()[0] = pos4{{std::numeric_limits<double>::infinity(), 0.0, 0.1, 1.0}};
        else if (mode == 'd')
        {
            // collapse a high face onto a line
            // make both high-poly triangles collapse onto a line
            hpos.data()[0] = make_p4(d3{{-1.0, -1.0, 0.1}});
            hpos.data()[1] = make_p4(d3{{1.0, -1.0, 0.1}});
            hpos.data()[2] = make_p4(d3{{0.0, -1.0, 0.1}});
            hpos.data()[3] = make_p4(d3{{-1.0, 1.0, 0.1}});
        }
        else if (mode == 'q')
        {
            // build a quad in the low mesh (non-triangular)
            auto a = add_vertex(low, lpos, d3{{-1.0, -1.0, 0.0}});
            auto b = add_vertex(low, lpos, d3{{1.0, -1.0, 0.0}});
            auto c = add_vertex(low, lpos, d3{{1.0, 1.0, 0.0}});
            auto d = add_vertex(low, lpos, d3{{-1.0, 1.0, 0.0}});
            auto f = low.faces().add(a, b, c, d);
            for (auto h : f.halfedges())
                luv[h] = uv3{{0.0, 0.0, 0.0}};
        }
        else if (mode == 'o')
        {
            // overlapping UV coverage: duplicate the low square on top of itself
            auto a = add_vertex(low, lpos, d3{{-1.0, -1.0, 0.0}});
            auto b = add_vertex(low, lpos, d3{{1.0, -1.0, 0.0}});
            auto c = add_vertex(low, lpos, d3{{1.0, 1.0, 0.0}});
            auto f = low.faces().add(a, b, c);
            for (auto h : f.halfedges())
                luv[h] = uv3{{0.5, 0.5, 0.0}};
            // a second triangle covering the same texels
            auto a2 = add_vertex(low, lpos, d3{{-1.0, -1.0, 0.0}});
            auto b2 = add_vertex(low, lpos, d3{{1.0, -1.0, 0.0}});
            auto c2 = add_vertex(low, lpos, d3{{1.0, 1.0, 0.0}});
            auto f2 = low.faces().add(a2, b2, c2);
            std::vector<pm::halfedge_handle> hs;
            for (auto h : f2.halfedges())
                hs.push_back(h);
            luv[hs[0]] = uv3{{0.0, 0.0, 0.0}};
            luv[hs[1]] = uv3{{1.0, 0.0, 0.0}};
            luv[hs[2]] = uv3{{0.0, 1.0, 0.0}};
        }

        auto threw = false;
        try
        {
            session_t s(hpos, lpos, luv, width, height, r);
        }
        catch (std::invalid_argument const&)
        {
            threw = true;
        }
        return threw;
    };

    // invalid dimensions / radius
    if (!expect_invalid(0, 8, 0.5, 'n'))
        return false;
    if (!expect_invalid(8, -1, 0.5, 'n'))
        return false;
    if (!expect_invalid(8, 8, 0.0, 'n'))
        return false;
    if (!expect_invalid(8, 8, -1.0, 'n'))
        return false;
    if (!expect_invalid(8, 8, std::numeric_limits<double>::quiet_NaN(), 'n'))
        return false;

    // w != 1
    if (!expect_invalid(8, 8, 0.5, 'w'))
        return false;
    // non-finite position
    if (!expect_invalid(8, 8, 0.5, 'f'))
        return false;
    // world-degenerate high-poly faces
    if (!expect_invalid(8, 8, 0.5, 'd'))
        return false;
    // non-triangular low-poly face
    if (!expect_invalid(8, 8, 0.5, 'q'))
        return false;
    // overlapping UV coverage
    if (!expect_invalid(8, 8, 0.5, 'o'))
        return false;

    // valid inputs still succeed
    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    make_high_plane(high, hpos, 0.1);
    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    make_low_square(low, lpos, luv, 0.0);
    session_t s(hpos, lpos, luv, 8, 8, 0.5);
    return count_hits(s.result()) == 64;
}

bool check_projection_boundary_coverage()
{
    // A low triangle whose UVs touch the [0,1] boundary exactly. Coverage is
    // decided at texel centers with the top-left rule, so every texel center
    // strictly inside the UV domain must be covered exactly once.
    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    make_high_plane(high, hpos, 0.1);

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    auto v0 = add_vertex(low, lpos, d3{{-1.0, -1.0, 0.0}});
    auto v1 = add_vertex(low, lpos, d3{{1.0, -1.0, 0.0}});
    auto v2 = add_vertex(low, lpos, d3{{1.0, 1.0, 0.0}});
    auto v3 = add_vertex(low, lpos, d3{{-1.0, 1.0, 0.0}});
    add_low_tri(low, lpos, luv, v0, v1, v2, d2{{0.0, 0.0}}, d2{{1.0, 0.0}}, d2{{1.0, 1.0}});
    add_low_tri(low, lpos, luv, v0, v2, v3, d2{{0.0, 0.0}}, d2{{1.0, 1.0}}, d2{{0.0, 1.0}});

    auto const width = 8;
    auto const height = 8;
    session_t s(hpos, lpos, luv, width, height, 0.5);
    for (auto const& t : s.result())
        if (!t.coverage || !t.hit)
            return false;
    return count_covered(s.result()) == width * height;
}

bool check_projection_analytic_precision()
{
    // A large tilted high-poly plane z = a*x + b*y + c above the low-poly
    // square. For a texel whose world point is P = (x, y, 0), N = (0, 0, 1),
    // the ray is (x, y, r) - t*(0, 0, 1) and hits the plane at
    // t = r - (a*x + b*y + c). The encoded normal is the unit plane normal
    // because T = (1, 0, 0) and B = (0, 1, 0).
    double const a = 0.13;
    double const b = -0.07;
    double const c = 0.21;

    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    auto plane = [&](double x, double y) { return d3{{x, y, a * x + b * y + c}}; };
    auto p0 = plane(-3.0, -3.0);
    auto p1 = plane(3.0, -3.0);
    auto p2 = plane(3.0, 3.0);
    auto p3 = plane(-3.0, 3.0);
    auto v0 = add_vertex(high, hpos, p0);
    auto v1 = add_vertex(high, hpos, p1);
    auto v2 = add_vertex(high, hpos, p2);
    auto v3 = add_vertex(high, hpos, p3);
    add_high_tri(high, hpos, v0, v1, v2);
    add_high_tri(high, hpos, v0, v2, v3);

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    make_low_square(low, lpos, luv, 0.0);

    auto const width = 16;
    auto const height = 16;
    auto const r = 0.5;
    session_t s(hpos, lpos, luv, width, height, r);
    auto const& res = s.result();

    d3 plane_n{{-a, -b, 1.0}};
    auto const len = std::sqrt(plane_n[0] * plane_n[0] + plane_n[1] * plane_n[1] + plane_n[2] * plane_n[2]);
    for (auto& x : plane_n)
        x /= len;

    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
        {
            auto const& t = res[size_t(y * width + x)];
            if (!t.hit)
                return false;
            auto const u = (double(x) + 0.5) / double(width);
            auto const v = (double(y) + 0.5) / double(height);
            auto const wx = -1.0 + 2.0 * u;
            auto const wy = -1.0 + 2.0 * v;
            auto const expected_t = r - (a * wx + b * wy + c);
            if (!nearly(t.t, expected_t, 1e-12 * std::max(1.0, r)))
                return false;
            if (!nearly3(t.normal, plane_n, TOL_N))
                return false;
        }
    return true;
}

// ---------------------------------------------------------------------------
// invalidation group
// ---------------------------------------------------------------------------

bool check_invalidation_old_miss_becomes_hit()
{
    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    make_high_plane(high, hpos, -1.0); // outside [0, 2r]

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    make_low_square(low, lpos, luv, 0.0);

    auto const width = 8;
    auto const height = 8;
    auto const r = 0.5;
    session_t s(hpos, lpos, luv, width, height, r);

    if (count_hits(s.result()) != 0 || count_covered(s.result()) != width * height)
        return false;

    std::vector<std::pair<int, d3>> updates;
    for (auto i = 0; i < 4; ++i)
    {
        d3 p = d3{{hpos.data()[i][0], hpos.data()[i][1], 0.1}};
        updates.emplace_back(i, p);
        hpos.data()[i] = make_p4(p);
    }
    s.update_positions(updates);

    auto const& res = s.result();
    if (count_hits(res) != width * height)
        return false;
    for (auto const& t : res)
        if (!t.hit || !nearly(t.t, 0.4, 1e-12) || !nearly3(t.normal, d3{{0.0, 0.0, 1.0}}, TOL_N))
            return false;

    auto const& u = s.last_update();
    if (u.faces != std::vector<int>({0, 1}))
        return false;
    if (int(u.texels.size()) != width * height)
        return false;
    if (u.nodes.empty())
        return false;
    if (!std::is_sorted(u.nodes.begin(), u.nodes.end()) || std::adjacent_find(u.nodes.begin(), u.nodes.end()) != u.nodes.end())
        return false;

    // must match a session baked from the final geometry
    session_t fresh(hpos, lpos, luv, width, height, r);
    return compare_results(res, fresh.result(), 1e-9 * std::max(1.0, r), TOL_N);
}

bool check_invalidation_hit_becomes_miss()
{
    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    make_high_plane(high, hpos, 0.1);

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    make_low_square(low, lpos, luv, 0.0);

    auto const width = 8;
    auto const height = 8;
    session_t s(hpos, lpos, luv, width, height, 0.5);
    if (count_hits(s.result()) != width * height)
        return false;

    std::vector<std::pair<int, d3>> updates;
    for (auto i = 0; i < 4; ++i)
    {
        d3 p = d3{{hpos.data()[i][0], hpos.data()[i][1], -1.0}};
        updates.emplace_back(i, p);
    }
    s.update_positions(updates);

    auto const& res = s.result();
    if (count_hits(res) != 0 || count_covered(res) != width * height)
        return false;
    for (auto const& t : res)
    {
        if (!t.coverage || t.hit || t.high_face != -1 || t.t != 0.0 || t.normal != d3{{0.0, 0.0, 0.0}})
            return false;
        if (t.low_face != 0 && t.low_face != 1)
            return false;
    }

    auto const& u = s.last_update();
    if (u.faces != std::vector<int>({0, 1}))
        return false;
    if (int(u.texels.size()) != width * height)
        return false;
    return true;
}

bool check_invalidation_multiple_edits()
{
    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    make_high_plane(high, hpos, 0.1);

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    make_low_square(low, lpos, luv, 0.0);

    auto const width = 10;
    auto const height = 10;
    auto const r = 0.5;
    session_t s(hpos, lpos, luv, width, height, r);

    std::array<d3, 3> const edits = {{{0.5, 0.0, 0.0}, {0.45, 0.0, 0.0}, {0.35, 0.0, 0.0}}}; // z offsets for slots 0, 1, 2
    for (auto k = 0; k < 3; ++k)
    {
        auto const slot = k;
        auto p = d3{{hpos.data()[slot][0], hpos.data()[slot][1], edits[size_t(k)][0]}};
        std::vector<std::pair<int, d3>> updates;
        updates.emplace_back(slot, p);
        s.update_positions(updates);
        hpos.data()[slot] = make_p4(p);

        session_t fresh(hpos, lpos, luv, width, height, r);
        if (!compare_results(s.result(), fresh.result(), 1e-9 * std::max(1.0, r), TOL_N))
            return false;
    }
    return true;
}

bool check_invalidation_release_inputs()
{
    std::unique_ptr<session_t> session;
    std::vector<texel_t> initial;
    {
        pm::Mesh high;
        pm::vertex_attribute<pos4> hpos(high);
        make_high_plane(high, hpos, 0.1);

        pm::Mesh low;
        pm::vertex_attribute<pos4> lpos(low);
        pm::halfedge_attribute<uv3> luv(low);
        make_low_square(low, lpos, luv, 0.0);

        session = std::make_unique<session_t>(hpos, lpos, luv, 8, 8, 0.5);
        initial = session->result();
    } // meshes and attributes are gone

    if (count_hits(initial) != 64)
        return false;

    std::vector<std::pair<int, d3>> updates;
    for (auto i = 0; i < 4; ++i)
        updates.emplace_back(i, d3{{-1.0 + 2.0 * (i == 1 || i == 2), -1.0 + 2.0 * (i >= 2), 0.2}});
    session->update_positions(updates);

    auto const& res = session->result();
    if (count_hits(res) != 64)
        return false;
    for (auto const& t : res)
    {
        if (!t.hit || !nearly(t.t, 0.3, 1e-12))
            return false;
        if (!nearly3(t.normal, d3{{0.0, 0.0, 1.0}}, 1e-12))
            return false;
    }
    if (session->last_update().faces != std::vector<int>({0, 1}))
        return false;
    return true;
}

bool check_invalidation_failure_keeps_state()
{
    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    make_high_plane(high, hpos, 0.1);

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    make_low_square(low, lpos, luv, 0.0);

    session_t s(hpos, lpos, luv, 8, 8, 0.5);

    // establish a non-empty last_update so we can prove it is preserved
    s.update_positions({{0, d3{{-1.0, -1.0, 0.2}}}});
    auto const snapshot_result = s.result();
    auto const snapshot_update = s.last_update();
    auto const snapshot_high = hpos.to_vector();

    auto expect_invalid = [&](std::vector<std::pair<int, d3>> const& updates) {
        auto threw = false;
        try
        {
            s.update_positions(updates);
        }
        catch (std::invalid_argument const&)
        {
            threw = true;
        }
        if (!threw)
            return false;
        if (!same_results_exact(s.result(), snapshot_result))
            return false;
        if (!same_update(s.last_update(), snapshot_update))
            return false;
        return true;
    };

    // invalid slot
    if (!expect_invalid({{-1, d3{{0.0, 0.0, 0.0}}}}))
        return false;
    if (!expect_invalid({{100000, d3{{0.0, 0.0, 0.0}}}}))
        return false;

    // non-finite value
    if (!expect_invalid({{0, d3{{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}}}}))
        return false;

    // update that degenerates an adjacent face (v1 collapses onto v0)
    if (!expect_invalid({{1, d3{{-1.0, -1.0, 0.2}}}}))
        return false;

    // session snapshot is untouched as well: a legal update still behaves normally
    s.update_positions({{0, d3{{-1.0, -1.0, 0.1}}}});
    session_t fresh(hpos, lpos, luv, 8, 8, 0.5);
    if (!compare_results(s.result(), fresh.result(), 1e-9, TOL_N))
        return false;
    (void)snapshot_high;
    return true;
}

bool check_invalidation_empty_and_duplicate()
{
    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    make_high_plane(high, hpos, 0.1);

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    make_low_square(low, lpos, luv, 0.0);

    session_t s(hpos, lpos, luv, 8, 8, 0.5);
    auto const before = s.result();

    s.update_positions({});
    if (!same_results_exact(s.result(), before))
        return false;
    if (!s.last_update().empty())
        return false;

    // duplicate slots: the last value wins
    std::vector<std::pair<int, d3>> duplicate;
    duplicate.emplace_back(0, d3{{-1.0, -1.0, 0.5}});
    duplicate.emplace_back(0, d3{{-1.0, -1.0, -0.2}});
    s.update_positions(duplicate);
    if (s.last_update().faces != std::vector<int>({0, 1}))
        return false;

    hpos.data()[0] = make_p4(d3{{-1.0, -1.0, -0.2}});
    session_t fresh(hpos, lpos, luv, 8, 8, 0.5);
    return compare_results(s.result(), fresh.result(), 1e-9, TOL_N);
}

bool check_invalidation_front_back_competition()
{
    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);

    // back layer at z = 0.0 (faces 0, 1)
    auto b0 = add_vertex(high, hpos, d3{{-1.0, -1.0, 0.0}});
    auto b1 = add_vertex(high, hpos, d3{{1.0, -1.0, 0.0}});
    auto b2 = add_vertex(high, hpos, d3{{1.0, 1.0, 0.0}});
    auto b3 = add_vertex(high, hpos, d3{{-1.0, 1.0, 0.0}});
    add_high_tri(high, hpos, b0, b1, b2);
    add_high_tri(high, hpos, b0, b2, b3);

    // front layer at z = 0.3 (faces 2, 3)
    auto f0 = add_vertex(high, hpos, d3{{-1.0, -1.0, 0.3}});
    auto f1 = add_vertex(high, hpos, d3{{1.0, -1.0, 0.3}});
    auto f2 = add_vertex(high, hpos, d3{{1.0, 1.0, 0.3}});
    auto f3 = add_vertex(high, hpos, d3{{-1.0, 1.0, 0.3}});
    add_high_tri(high, hpos, f0, f1, f2);
    add_high_tri(high, hpos, f0, f2, f3);

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    make_low_square(low, lpos, luv, 0.0);

    auto const width = 8;
    auto const height = 8;
    auto const r = 0.5;
    session_t s(hpos, lpos, luv, width, height, r);

    for (auto const& t : s.result())
    {
        if (!t.hit || !nearly(t.t, 0.2, 1e-12))
            return false;
        if (t.high_face != 2 && t.high_face != 3)
            return false;
    }

    // move the front layer out of range: the back layer must be found
    std::vector<std::pair<int, d3>> updates;
    for (auto i = 4; i < 8; ++i)
    {
        d3 p = d3{{hpos.data()[i][0], hpos.data()[i][1], 2.0}};
        updates.emplace_back(i, p);
        hpos.data()[i] = make_p4(p);
    }
    s.update_positions(updates);

    for (auto const& t : s.result())
    {
        if (!t.hit || !nearly(t.t, 0.5, 1e-12))
            return false;
        if (t.high_face != 0 && t.high_face != 1)
            return false;
    }
    if (s.last_update().faces != std::vector<int>({2, 3}))
        return false;

    session_t fresh(hpos, lpos, luv, width, height, r);
    return compare_results(s.result(), fresh.result(), 1e-9 * std::max(1.0, r), TOL_N);
}

bool check_invalidation_edit_degenerate()
{
    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    make_high_plane(high, hpos, 0.1);

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    make_low_square(low, lpos, luv, 0.0);

    session_t s(hpos, lpos, luv, 8, 8, 0.5);
    auto const before = s.result();
    auto const before_update = s.last_update();

    auto threw = false;
    try
    {
        // v2 collapses onto v0, degenerating both incident faces
        s.update_positions({{2, d3{{-1.0, -1.0, 0.1}}}});
    }
    catch (std::invalid_argument const&)
    {
        threw = true;
    }
    if (!threw)
        return false;
    if (!same_results_exact(s.result(), before))
        return false;
    return same_update(s.last_update(), before_update);
}

// ---------------------------------------------------------------------------
// locality group
// ---------------------------------------------------------------------------

int const ISLAND_COLS = 16;
int const ISLAND_COUNT = ISLAND_COLS * ISLAND_COLS; // 256

/// Builds a grid of ISLAND_COUNT disjoint high-poly quads.
/// Island k (k = j * cols + i) stores its vertices at slots 4k .. 4k+3 and its
/// faces at indices 2k, 2k+1. Island 0's vertex 0 gets z = island0_z.
void build_island_high(pm::Mesh& m, pm::vertex_attribute<pos4>& position, double island0_z)
{
    for (auto j = 0; j < ISLAND_COLS; ++j)
        for (auto i = 0; i < ISLAND_COLS; ++i)
        {
            auto const k = j * ISLAND_COLS + i;
            double const x0 = i * 2.0 + 0.2;
            double const x1 = i * 2.0 + 0.8;
            double const y0 = j * 2.0 + 0.2;
            double const y1 = j * 2.0 + 0.8;
            double const z = 0.1;

            auto v0 = add_vertex(m, position, d3{{x0, y0, k == 0 ? island0_z : z}});
            auto v1 = add_vertex(m, position, d3{{x1, y0, z}});
            auto v2 = add_vertex(m, position, d3{{x1, y1, z}});
            auto v3 = add_vertex(m, position, d3{{x0, y1, z}});
            add_high_tri(m, position, v0, v1, v2);
            add_high_tri(m, position, v0, v2, v3);
        }
}

/// Builds the matching low-poly quads with separate UV islands.
void build_island_low(pm::Mesh& m, pm::vertex_attribute<pos4>& position, pm::halfedge_attribute<uv3>& uv)
{
    double const cell = 1.0 / double(ISLAND_COLS);
    for (auto j = 0; j < ISLAND_COLS; ++j)
        for (auto i = 0; i < ISLAND_COLS; ++i)
        {
            double const x0 = i * 2.0 + 0.2;
            double const x1 = i * 2.0 + 0.8;
            double const y0 = j * 2.0 + 0.2;
            double const y1 = j * 2.0 + 0.8;

            double const u0 = i * cell + 0.2 * cell;
            double const u1 = (i + 1) * cell - 0.2 * cell;
            double const v0 = j * cell + 0.2 * cell;
            double const v1 = (j + 1) * cell - 0.2 * cell;

            auto a = add_vertex(m, position, d3{{x0, y0, 0.0}});
            auto b = add_vertex(m, position, d3{{x1, y0, 0.0}});
            auto c = add_vertex(m, position, d3{{x1, y1, 0.0}});
            auto d = add_vertex(m, position, d3{{x0, y1, 0.0}});

            add_low_tri(m, position, uv, a, b, c, d2{{u0, v0}}, d2{{u1, v0}}, d2{{u1, v1}});
            add_low_tri(m, position, uv, a, c, d, d2{{u0, v0}}, d2{{u1, v1}}, d2{{u0, v1}});
        }
}

bool check_locality_grid()
{
    auto const width = 256;
    auto const height = 256;
    auto const r = 0.5;

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    build_island_low(low, lpos, luv);

    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    build_island_high(high, hpos, 0.1);

    session_t s(hpos, lpos, luv, width, height, r);
    auto const before = s.result();

    auto const total_covered = count_covered(before);
    if (total_covered < ISLAND_COUNT * 25)
        return false;

    // move the first island's anchor vertex, tilting both of its faces
    s.update_positions({{0, d3{{0.2, 0.2, 0.3}}}});

    auto const& res = s.result();
    auto const& u = s.last_update();

    // only the two faces of island 0 may have been recomputed
    if (u.faces != std::vector<int>({0, 1}))
        return false;

    // the re-projection must stay confined to island 0's texels
    if (u.texels.empty() || int(u.texels.size()) > 150)
        return false;
    if (int(u.texels.size()) * 50 > total_covered)
        return false;

    double const cell = 1.0 / double(ISLAND_COLS);
    auto const u0 = 0.2 * cell;
    auto const u1 = 1.0 * cell - 0.2 * cell;
    auto const v0 = 0.2 * cell;
    auto const v1 = 1.0 * cell - 0.2 * cell;
    auto const x_lo = int(std::floor(u0 * width)) - 1;
    auto const x_hi = int(std::ceil(u1 * width)) + 1;
    auto const y_lo = int(std::floor(v0 * height)) - 1;
    auto const y_hi = int(std::ceil(v1 * height)) + 1;
    for (auto const texel : u.texels)
    {
        auto const x = texel % width;
        auto const y = texel / width;
        if (x < x_lo || x > x_hi || y < y_lo || y > y_hi)
            return false;
    }

    // nodes must be a small ancestor chain, not the whole tree
    if (u.nodes.empty() || int(u.nodes.size()) > 48)
        return false;
    if (u.nodes.size() * 4 > s.bvh_nodes().size())
        return false;
    if (!std::is_sorted(u.nodes.begin(), u.nodes.end()) || std::adjacent_find(u.nodes.begin(), u.nodes.end()) != u.nodes.end())
        return false;

    // every texel that was not re-projected keeps its previous value
    std::vector<char> touched(res.size(), 0);
    for (auto const texel : u.texels)
        touched[size_t(texel)] = 1;
    for (auto i = 0u; i < res.size(); ++i)
    {
        if (touched[i])
            continue;
        auto const& a = before[i];
        auto const& b = res[i];
        if (a.coverage != b.coverage || a.hit != b.hit || a.low_face != b.low_face || a.high_face != b.high_face || a.t != b.t || a.normal != b.normal)
            return false;
    }

    // and the updated result matches a fresh bake of the final geometry
    pm::Mesh high2;
    pm::vertex_attribute<pos4> hpos2(high2);
    build_island_high(high2, hpos2, 0.3);

    session_t fresh(hpos2, lpos, luv, width, height, r);
    return compare_results(res, fresh.result(), 1e-9 * std::max(1.0, r), TOL_N);
}

/// Verifies the exposed high-poly BVH connectivity and its bounds against the
/// final geometry.
bool verify_bvh(session_t const& s, pm::Mesh const& high, std::vector<d3> const& positions)
{
    auto const& nodes = s.bvh_nodes();
    auto const& order = s.bvh_face_order();
    if (nodes.empty())
        return order.empty();

    // collect valid face vertex indices
    std::vector<std::array<int, 3>> face_vertices(high.all_faces().size(), std::array<int, 3>{{-1, -1, -1}});
    auto valid_faces = 0;
    for (auto f : high.faces())
    {
        auto vs = std::vector<int>();
        for (auto v : f.vertices())
            vs.push_back(v.idx.value);
        face_vertices[size_t(f.idx.value)] = std::array<int, 3>{{vs[0], vs[1], vs[2]}};
        ++valid_faces;
    }

    // the face order is a permutation of all valid faces
    if (int(order.size()) != valid_faces)
        return false;
    std::vector<int> sorted_order = order;
    std::sort(sorted_order.begin(), sorted_order.end());
    if (std::adjacent_find(sorted_order.begin(), sorted_order.end()) != sorted_order.end())
        return false;
    for (auto fi : sorted_order)
        if (fi < 0 || fi >= int(face_vertices.size()) || face_vertices[size_t(fi)][0] < 0)
            return false;

    if (nodes[0].parent != -1)
        return false;

    auto face_bounds = [&](int fi, d3& mn, d3& mx) {
        auto const& f = face_vertices[size_t(fi)];
        for (auto k = 0; k < 3; ++k)
        {
            mn[k] = std::min(positions[size_t(f[0])][k], std::min(positions[size_t(f[1])][k], positions[size_t(f[2])][k]));
            mx[k] = std::max(positions[size_t(f[0])][k], std::max(positions[size_t(f[1])][k], positions[size_t(f[2])][k]));
        }
    };

    std::function<bool(int, d3&, d3&)> verify_node = [&](int n, d3& mn, d3& mx) -> bool {
        auto const& node = nodes[size_t(n)];
        if (node.is_leaf())
        {
            if (node.first < 0 || node.first + node.count > int(order.size()))
                return false;
            auto first = true;
            for (auto i = node.first; i < node.first + node.count; ++i)
            {
                d3 fmn, fmx;
                face_bounds(order[size_t(i)], fmn, fmx);
                if (first)
                {
                    mn = fmn;
                    mx = fmx;
                    first = false;
                }
                else
                    for (auto k = 0; k < 3; ++k)
                    {
                        mn[k] = std::min(mn[k], fmn[k]);
                        mx[k] = std::max(mx[k], fmx[k]);
                    }
            }
        }
        else
        {
            if (node.left < 0 || node.right < 0 || node.left <= n || node.right <= n)
                return false;
            if (nodes[size_t(node.left)].parent != n || nodes[size_t(node.right)].parent != n)
                return false;
            d3 lmn, lmx, rmn, rmx;
            if (!verify_node(node.left, lmn, lmx) || !verify_node(node.right, rmn, rmx))
                return false;
            for (auto k = 0; k < 3; ++k)
            {
                mn[k] = std::min(lmn[k], rmn[k]);
                mx[k] = std::max(lmx[k], rmx[k]);
            }
        }
        for (auto k = 0; k < 3; ++k)
            if (node.min[k] != mn[k] || node.max[k] != mx[k])
                return false;
        return true;
    };

    d3 mn, mx;
    return verify_node(0, mn, mx);
}

/// Finds the leaf node that stores a given face.
int find_leaf(session_t const& s, int face)
{
    auto const& nodes = s.bvh_nodes();
    for (auto n = 0; n < int(nodes.size()); ++n)
    {
        auto const& node = nodes[size_t(n)];
        if (!node.is_leaf())
            continue;
        for (auto i = node.first; i < node.first + node.count; ++i)
            if (s.bvh_face_order()[size_t(i)] == face)
                return n;
    }
    return -1;
}

bool check_locality_bvh_consistency()
{
    auto const width = 128;
    auto const height = 128;
    auto const r = 0.5;

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    build_island_low(low, lpos, luv);

    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    build_island_high(high, hpos, 0.1);

    session_t s(hpos, lpos, luv, width, height, r);

    // expected positions: initial geometry, then with slot 0 lifted from 0.1 to 0.3
    std::vector<d3> initial_positions;
    std::vector<d3> final_positions;
    initial_positions.reserve(size_t(hpos.size()));
    final_positions.reserve(size_t(hpos.size()));
    for (auto i = 0; i < int(hpos.size()); ++i)
    {
        auto const vi = size_t(i);
        initial_positions.push_back(d3{{hpos.data()[i][0], hpos.data()[i][1], hpos.data()[i][2]}});
        if (vi == 0)
            final_positions.push_back(d3{{hpos.data()[i][0], hpos.data()[i][1], 0.3}});
        else
            final_positions.push_back(d3{{hpos.data()[i][0], hpos.data()[i][1], hpos.data()[i][2]}});
    }

    if (!verify_bvh(s, high, initial_positions))
        return false;

    s.update_positions({{0, d3{{0.2, 0.2, 0.3}}}});

    if (!verify_bvh(s, high, final_positions))
        return false;

    // the reported nodes must be exactly the touched leaves and their ancestors
    std::vector<int> expected_nodes;
    for (auto const face : {0, 1})
    {
        auto n = find_leaf(s, face);
        while (n >= 0)
        {
            expected_nodes.push_back(n);
            n = s.bvh_nodes()[size_t(n)].parent;
        }
    }
    std::sort(expected_nodes.begin(), expected_nodes.end());
    expected_nodes.erase(std::unique(expected_nodes.begin(), expected_nodes.end()), expected_nodes.end());

    return s.last_update().nodes == expected_nodes;
}

bool check_locality_far_untouched()
{
    auto const width = 64;
    auto const height = 64;
    auto const r = 0.5;

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    build_island_low(low, lpos, luv);

    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    build_island_high(high, hpos, 0.1);

    session_t s(hpos, lpos, luv, width, height, r);
    auto const before = s.result();

    // edit an island on the far side of the grid
    auto const island = ISLAND_COUNT - 1;
    auto const slot = island * 4;
    auto const x = hpos.data()[slot][0];
    auto const y = hpos.data()[slot][1];
    s.update_positions({{slot, d3{{x, y, 0.35}}}});

    auto const& u = s.last_update();
    // only the two faces of that island
    if (u.faces != std::vector<int>({2 * island, 2 * island + 1}))
        return false;

    // and only texels around that island were reprojected
    if (u.texels.empty() || int(u.texels.size()) > 150)
        return false;

    std::vector<char> touched(s.result().size(), 0);
    for (auto const texel : u.texels)
        touched[size_t(texel)] = 1;
    for (auto i = 0u; i < before.size(); ++i)
        if (!touched[i] && (before[i].coverage != s.result()[i].coverage || before[i].t != s.result()[i].t || before[i].high_face != s.result()[i].high_face))
            return false;

    return true;
}

bool check_locality_segment_index()
{
    // the exposed finite-segment index must cover exactly the covered texels
    pm::Mesh high;
    pm::vertex_attribute<pos4> hpos(high);
    make_high_plane(high, hpos, 0.1);

    pm::Mesh low;
    pm::vertex_attribute<pos4> lpos(low);
    pm::halfedge_attribute<uv3> luv(low);
    make_low_square(low, lpos, luv, 0.0);

    session_t s(hpos, lpos, luv, 16, 16, 0.5);

    auto const& index = s.projection_segment_index();
    if (index.size() != count_covered(s.result()))
        return false;
    if (index.nodes().empty() || index.leaf_order().size() != size_t(index.size()))
        return false;

    // every segment id maps to a covered texel
    std::vector<char> seen(s.result().size(), 0);
    for (auto const& seg : index.segments())
    {
        if (seg.id < 0 || seg.id >= int(s.result().size()))
            return false;
        if (!s.result()[size_t(seg.id)].coverage)
            return false;
        if (!seen[size_t(seg.id)])
        {
            seen[size_t(seg.id)] = 1;
        }
        else
            return false;
    }
    return true;
}

} // namespace

// TASK_CHECK wrappers: every registered case performs at least one checked assertion.
namespace
{
int case_projection_basic()
{
    TASK_CHECK(check_projection_basic());
    return 0;
}
int case_projection_mirrored_uv()
{
    TASK_CHECK(check_projection_mirrored_uv());
    return 0;
}
int case_projection_shared_edge()
{
    TASK_CHECK(check_projection_shared_edge());
    return 0;
}
int case_projection_backface()
{
    TASK_CHECK(check_projection_backface());
    return 0;
}
int case_projection_equal_distance_tie()
{
    TASK_CHECK(check_projection_equal_distance_tie());
    return 0;
}
int case_projection_closed_interval()
{
    TASK_CHECK(check_projection_closed_interval());
    return 0;
}
int case_projection_ignores_zero_uv_area()
{
    TASK_CHECK(check_projection_ignores_zero_uv_area());
    return 0;
}
int case_projection_v_up()
{
    TASK_CHECK(check_projection_v_up());
    return 0;
}
int case_projection_constructor_validation()
{
    TASK_CHECK(check_projection_constructor_validation());
    return 0;
}
int case_projection_boundary_coverage()
{
    TASK_CHECK(check_projection_boundary_coverage());
    return 0;
}
int case_projection_analytic_precision()
{
    TASK_CHECK(check_projection_analytic_precision());
    return 0;
}
int case_invalidation_old_miss_becomes_hit()
{
    TASK_CHECK(check_invalidation_old_miss_becomes_hit());
    return 0;
}
int case_invalidation_hit_becomes_miss()
{
    TASK_CHECK(check_invalidation_hit_becomes_miss());
    return 0;
}
int case_invalidation_multiple_edits()
{
    TASK_CHECK(check_invalidation_multiple_edits());
    return 0;
}
int case_invalidation_release_inputs()
{
    TASK_CHECK(check_invalidation_release_inputs());
    return 0;
}
int case_invalidation_failure_keeps_state()
{
    TASK_CHECK(check_invalidation_failure_keeps_state());
    return 0;
}
int case_invalidation_empty_and_duplicate()
{
    TASK_CHECK(check_invalidation_empty_and_duplicate());
    return 0;
}
int case_invalidation_front_back_competition()
{
    TASK_CHECK(check_invalidation_front_back_competition());
    return 0;
}
int case_invalidation_edit_degenerate()
{
    TASK_CHECK(check_invalidation_edit_degenerate());
    return 0;
}
int case_locality_grid()
{
    TASK_CHECK(check_locality_grid());
    return 0;
}
int case_locality_bvh_consistency()
{
    TASK_CHECK(check_locality_bvh_consistency());
    return 0;
}
int case_locality_far_untouched()
{
    TASK_CHECK(check_locality_far_untouched());
    return 0;
}
int case_locality_segment_index()
{
    TASK_CHECK(check_locality_segment_index());
    return 0;
}
} // namespace


extern const struct task_case task_cases[] = {
    {"projection", "basic", case_projection_basic},
    {"projection", "mirrored_uv", case_projection_mirrored_uv},
    {"projection", "shared_edge", case_projection_shared_edge},
    {"projection", "backface", case_projection_backface},
    {"projection", "equal_distance_tie", case_projection_equal_distance_tie},
    {"projection", "closed_interval", case_projection_closed_interval},
    {"projection", "zero_uv_area", case_projection_ignores_zero_uv_area},
    {"projection", "v_up", case_projection_v_up},
    {"projection", "constructor_validation", case_projection_constructor_validation},
    {"projection", "boundary_coverage", case_projection_boundary_coverage},
    {"projection", "analytic_precision", case_projection_analytic_precision},
    {"invalidation", "old_miss_becomes_hit", case_invalidation_old_miss_becomes_hit},
    {"invalidation", "hit_becomes_miss", case_invalidation_hit_becomes_miss},
    {"invalidation", "multiple_edits", case_invalidation_multiple_edits},
    {"invalidation", "release_inputs", case_invalidation_release_inputs},
    {"invalidation", "failure_keeps_state", case_invalidation_failure_keeps_state},
    {"invalidation", "empty_and_duplicate", case_invalidation_empty_and_duplicate},
    {"invalidation", "front_back_competition", case_invalidation_front_back_competition},
    {"invalidation", "edit_degenerate", case_invalidation_edit_degenerate},
    {"locality", "grid_256", case_locality_grid},
    {"locality", "bvh_consistency", case_locality_bvh_consistency},
    {"locality", "far_untouched", case_locality_far_untouched},
    {"locality", "segment_index", case_locality_segment_index},
};
extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
