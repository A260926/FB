// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

// Task driver tests for per-corner normals, smoothing groups and the
// corner-preserving edge split. This translation unit intentionally has no
// main(): task_support/driver.c provides the C entry point.

#include "pmp/algorithms/corner_edit.h"
#include "pmp/algorithms/normals.h"
#include "pmp/io/read_obj.h"
#include "pmp/io/write_obj.h"
#include "pmp/surface_mesh.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "task_support/check.h"

using namespace pmp;

namespace {

constexpr Scalar kEps = Scalar(1e-5);
constexpr Scalar kPi = Scalar(3.14159265358979323846);

bool close3(const Point& a, const Point& b, Scalar eps = kEps)
{
    return norm(a - b) <= eps;
}

bool close2(const TexCoord& a, const TexCoord& b, Scalar eps = kEps)
{
    return norm(a - b) <= eps;
}

// halfedge of face f targeting vertex v (the corner halfedge at v)
Halfedge corner_halfedge(const SurfaceMesh& m, Face f, Vertex v)
{
    for (auto h : m.halfedges(f))
        if (m.to_vertex(h) == v)
            return h;
    return {};
}

// --- independent geometry helpers (do not call the production normals) ----

Point ref_face_normal(const SurfaceMesh& m, Face f)
{
    std::vector<Point> p;
    for (auto v : m.vertices(f))
        p.push_back(m.position(v));
    Point n(0, 0, 0);
    for (size_t i = 0; i < p.size(); ++i)
    {
        const Point& a = p[i];
        const Point& b = p[(i + 1) % p.size()];
        n += cross(a, b);
    }
    const Scalar len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    return len > Scalar(1e-12) ? n / len : Point(0, 0, 0);
}

Scalar ref_corner_angle(const SurfaceMesh& m, Face f, Vertex v)
{
    std::vector<Vertex> verts(m.vertices(f).begin(), m.vertices(f).end());
    size_t k = 0;
    while (k < verts.size() && verts[k] != v)
        ++k;
    const Vertex va = verts[(k + verts.size() - 1) % verts.size()];
    const Vertex vb = verts[(k + 1) % verts.size()];
    Point ea = m.position(va) - m.position(v);
    Point eb = m.position(vb) - m.position(v);
    const Scalar la = norm(ea), lb = norm(eb);
    if (la <= Scalar(1e-12) || lb <= Scalar(1e-12))
        return Scalar(0);
    Scalar c = dot(ea, eb) / (la * lb);
    c = std::max(Scalar(-1), std::min(Scalar(1), c));
    return std::acos(c);
}

// Independent re-implementation of the smoothing fan rule, used to check the
// production implementation. Returns the angle-weighted fan normal at the
// corner identified by the halfedge \p h targeting its corner vertex.
Normal ref_corner_normal(const SurfaceMesh& m,
                         const FaceProperty<int>& smoothing, Halfedge h)
{
    const Vertex v = m.to_vertex(h);
    const Face seed = m.face(h);
    const int group = smoothing[seed];

    std::vector<Face> incident;
    std::vector<Halfedge> corners; // halfedges targeting v, one per face
    for (auto oh : m.halfedges(v))
    {
        if (!m.is_boundary(oh))
        {
            incident.push_back(m.face(oh));
            corners.push_back(m.prev_halfedge(oh));
        }
    }

    std::vector<bool> visited(incident.size(), false);
    std::vector<size_t> stack;
    for (size_t i = 0; i < incident.size(); ++i)
        if (incident[i] == seed)
        {
            visited[i] = true;
            stack.push_back(i);
        }

    std::vector<size_t> fan;
    while (!stack.empty())
    {
        const size_t cur = stack.back();
        stack.pop_back();
        fan.push_back(cur);
        if (group <= 0)
            continue; // group 0 never shares a fan

        // the two edges touching the corner vertex
        const Halfedge ch = corners[cur];
        const Halfedge across[2] = {
            m.opposite_halfedge(ch),
            m.opposite_halfedge(m.next_halfedge(ch))};
        for (Halfedge nh : across)
        {
            if (m.is_boundary(nh))
                continue;
            const Face nf = m.face(nh);
            for (size_t j = 0; j < incident.size(); ++j)
            {
                if (!visited[j] && incident[j] == nf &&
                    smoothing[nf] == group)
                {
                    visited[j] = true;
                    stack.push_back(j);
                }
            }
        }
    }

    Normal sum(0, 0, 0);
    for (size_t idx : fan)
    {
        const Face f = incident[idx];
        sum += ref_face_normal(m, f) * ref_corner_angle(m, f, v);
    }
    const Scalar len = norm(sum);
    return len > Scalar(1e-12) ? sum / len : Normal(0, 0, 0);
}

// Check every automatic corner of the mesh against the reference fan rule.
int check_all_automatic(const SurfaceMesh& m)
{
    auto smoothing = m.get_face_property<int>("f:smoothing_group");
    auto hnormal = m.get_halfedge_property<Normal>("h:normal");
    auto hnormal_explicit =
        m.get_halfedge_property<bool>("h:normal_explicit");
    TASK_CHECK(bool(smoothing) && bool(hnormal) && bool(hnormal_explicit));

    for (auto f : m.faces())
        for (auto h : m.halfedges(f))
        {
            if (hnormal_explicit[h])
                continue;
            const Normal expected = ref_corner_normal(m, smoothing, h);
            TASK_CHECK(close3(hnormal[h], expected));
        }
    return 0;
}

// Attach the corner property set used by the task to a freshly built mesh.
void init_corner_properties(SurfaceMesh& m)
{
    m.halfedge_property<TexCoord>("h:tex", TexCoord(0, 0));
    m.halfedge_property<Normal>("h:normal", Normal(0, 0, 0));
    m.halfedge_property<bool>("h:normal_explicit", false);
    m.face_property<int>("f:smoothing_group", 0);
}

// Independent OBJ token inspector. It only uses the STL and records the three
// index domains separately so the test never relies on the production writer.
struct ObjTokens
{
    int nv = 0, nvt = 0, nvn = 0;
    std::vector<int> smoothing; // one entry per face, in face order
    struct Corner
    {
        int v;
        int t = 0;       // 0 == omitted
        int n = 0;       // 0 == omitted
        bool has_t = false;
        bool has_n = false;
    };
    std::vector<std::vector<Corner>> faces;
};

ObjTokens parse_obj(const std::string& path)
{
    ObjTokens obj;
    std::ifstream in(path);
    if (!in.good())
        return obj;
    std::string line;
    int current_group = 0;
    while (std::getline(in, line))
    {
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "v")
            ++obj.nv;
        else if (tag == "vt")
            ++obj.nvt;
        else if (tag == "vn")
            ++obj.nvn;
        else if (tag == "s")
        {
            std::string tok;
            ss >> tok;
            current_group = (tok == "off" || tok == "0") ? 0 : std::stoi(tok);
        }
        else if (tag == "f")
        {
            std::vector<ObjTokens::Corner> corners;
            std::string spec;
            while (ss >> spec)
            {
                ObjTokens::Corner c;
                const size_t p1 = spec.find('/');
                if (p1 == std::string::npos)
                {
                    c.v = std::stoi(spec);
                }
                else
                {
                    c.v = std::stoi(spec.substr(0, p1));
                    const size_t p2 = spec.find('/', p1 + 1);
                    if (p2 == std::string::npos)
                    {
                        const std::string mid = spec.substr(p1 + 1);
                        if (!mid.empty())
                        {
                            c.t = std::stoi(mid);
                            c.has_t = true;
                        }
                    }
                    else
                    {
                        const std::string mid = spec.substr(p1 + 1, p2 - p1 - 1);
                        if (!mid.empty())
                        {
                            c.t = std::stoi(mid);
                            c.has_t = true;
                        }
                        const std::string tail = spec.substr(p2 + 1);
                        if (!tail.empty())
                        {
                            c.n = std::stoi(tail);
                            c.has_n = true;
                        }
                    }
                }
                corners.push_back(c);
            }
            obj.faces.push_back(corners);
            obj.smoothing.push_back(current_group);
        }
    }
    return obj;
}

// ---------------------------------------------------------------- fans -----

int case_fan_separation()
{
    SurfaceMesh m;
    init_corner_properties(m);
    auto smoothing = m.get_face_property<int>("f:smoothing_group");

    const Vertex o = m.add_vertex(Point(0, 0, 0));
    std::vector<Vertex> ring;
    const int k = 6;
    for (int i = 0; i < k; ++i)
    {
        const Scalar a = Scalar(2) * kPi * i / k;
        ring.push_back(m.add_vertex(
            Point(std::cos(a), std::sin(a), Scalar(0.3) * std::sin(2 * a))));
    }
    std::vector<Face> faces;
    for (int i = 0; i < k; ++i)
        faces.push_back(m.add_triangle(o, ring[i], ring[(i + 1) % k]));

    // group 1 faces are split into two fans by the group-2 faces in between
    const int groups[6] = {1, 2, 1, 1, 2, 1};
    for (int i = 0; i < k; ++i)
        smoothing[faces[i]] = groups[i];

    update_corner_normals(m);
    TASK_CHECK(check_all_automatic(m) == 0);

    auto hnormal = m.get_halfedge_property<Normal>("h:normal");

    // fan A = {f0, f5}, fan B = {f2, f3}: same group, different fans
    const Normal fanA = ref_corner_normal(m, smoothing,
                                          corner_halfedge(m, faces[0], o));
    const Normal fanB = ref_corner_normal(m, smoothing,
                                          corner_halfedge(m, faces[2], o));
    TASK_CHECK(close3(hnormal[corner_halfedge(m, faces[0], o)], fanA));
    TASK_CHECK(close3(hnormal[corner_halfedge(m, faces[5], o)], fanA));
    TASK_CHECK(close3(hnormal[corner_halfedge(m, faces[2], o)], fanB));
    TASK_CHECK(close3(hnormal[corner_halfedge(m, faces[3], o)], fanB));
    TASK_CHECK(norm(fanA - fanB) > Scalar(1e-3));

    // isolated group-2 faces keep their own geometric normal
    TASK_CHECK(close3(hnormal[corner_halfedge(m, faces[1], o)],
                      ref_face_normal(m, faces[1])));
    TASK_CHECK(close3(hnormal[corner_halfedge(m, faces[4], o)],
                      ref_face_normal(m, faces[4])));

    // make one corner explicit: it must survive a recomputation untouched
    auto hnormal_explicit =
        m.get_halfedge_property<bool>("h:normal_explicit");
    const Halfedge hc = corner_halfedge(m, faces[2], o);
    const Normal custom(1, 0, 0);
    hnormal_explicit[hc] = true;
    hnormal[hc] = custom;
    update_corner_normals(m);
    TASK_CHECK(hnormal_explicit[hc]);
    TASK_CHECK(close3(hnormal[hc], custom));
    // the other corner of the same fan is still the automatic fan normal
    TASK_CHECK(close3(hnormal[corner_halfedge(m, faces[3], o)], fanB));
    TASK_CHECK(check_all_automatic(m) == 0);
    return 0;
}

int case_group_zero()
{
    SurfaceMesh m;
    init_corner_properties(m);
    const Vertex v0 = m.add_vertex(Point(0, 0, 0));
    const Vertex v1 = m.add_vertex(Point(1, 0, 0));
    const Vertex v2 = m.add_vertex(Point(0, 1, 0));
    const Vertex v3 = m.add_vertex(Point(1, 1, 1));
    const Face f0 = m.add_triangle(v0, v1, v2);
    const Face f1 = m.add_triangle(v1, v3, v2);
    // both faces stay in group 0 (default): every corner is on its own

    update_corner_normals(m);
    auto hnormal = m.get_halfedge_property<Normal>("h:normal");
    for (auto f : {f0, f1})
        for (auto h : m.halfedges(f))
            TASK_CHECK(close3(hnormal[h], ref_face_normal(m, f)));
    return 0;
}

int case_uv_seam_smooth()
{
    SurfaceMesh m;
    init_corner_properties(m);
    auto smoothing = m.get_face_property<int>("f:smoothing_group");
    auto tex = m.get_halfedge_property<TexCoord>("h:tex");

    const Vertex v0 = m.add_vertex(Point(0, 0, 0));
    const Vertex v1 = m.add_vertex(Point(1, 0, 0));
    const Vertex v2 = m.add_vertex(Point(0, 1, 0));
    const Vertex v3 = m.add_vertex(Point(1, 1, Scalar(0.2)));
    const Face f0 = m.add_triangle(v0, v1, v2);
    const Face f1 = m.add_triangle(v1, v3, v2);
    smoothing[f0] = 5;
    smoothing[f1] = 5; // same group -> one smooth fan

    // different UVs at the shared vertex corners: a UV seam
    const Halfedge c2a = corner_halfedge(m, f0, v2);
    const Halfedge c2b = corner_halfedge(m, f1, v2);
    tex[c2a] = TexCoord(0, 0);
    tex[c2b] = TexCoord(7, 9);
    TASK_CHECK(!close2(tex[c2a], tex[c2b]));

    update_corner_normals(m);
    auto hnormal = m.get_halfedge_property<Normal>("h:normal");
    // the seam must not turn into a hard normal boundary
    TASK_CHECK(close3(hnormal[c2a], hnormal[c2b]));
    const Normal expected = ref_corner_normal(m, smoothing, c2a);
    TASK_CHECK(close3(hnormal[c2a], expected));
    TASK_CHECK(close3(hnormal[c2b], expected));
    return 0;
}

int case_zero_sum()
{
    SurfaceMesh m;
    init_corner_properties(m);
    auto smoothing = m.get_face_property<int>("f:smoothing_group");

    // two manifold triangles sharing edge (v,b), wound oppositely so their
    // unit normals cancel; both have a 90 degree interior angle at v, hence
    // the angle-weighted normal sum at v is exactly zero
    const Vertex v = m.add_vertex(Point(0, 0, 0));
    const Vertex a = m.add_vertex(Point(0, 1, 0));
    const Vertex b = m.add_vertex(Point(1, 0, 0));
    const Vertex c = m.add_vertex(Point(0, 1, 0)); // distinct vertex, same spot
    const Face f0 = m.add_triangle(v, a, b);
    const Face f1 = m.add_triangle(v, b, c);
    smoothing[f0] = 3;
    smoothing[f1] = 3;
    TASK_CHECK(close3(ref_face_normal(m, f0),
                      -ref_face_normal(m, f1), Scalar(1e-4)));
    TASK_CHECK(std::abs(ref_corner_angle(m, f0, v) -
                        ref_corner_angle(m, f1, v)) <= Scalar(1e-4));

    update_corner_normals(m);
    auto hnormal = m.get_halfedge_property<Normal>("h:normal");
    TASK_CHECK(norm(hnormal[corner_halfedge(m, f0, v)]) <= Scalar(1e-6));
    TASK_CHECK(norm(hnormal[corner_halfedge(m, f1, v)]) <= Scalar(1e-6));
    return 0;
}

// --------------------------------------------------------- split_transfer --

// Two triangles sharing edge (v1,v2); side A opposite vertex v0, side B v3.
struct SplitFixture
{
    SurfaceMesh m;
    Vertex v0, v1, v2, v3;
    Face fa, fb;
    Halfedge edge_h;
};

void build_split_fixture(SplitFixture& fx)
{
    auto& m = fx.m;
    init_corner_properties(m);
    fx.v0 = m.add_vertex(Point(0, 0, 0));
    fx.v1 = m.add_vertex(Point(1, 0, 0));
    fx.v2 = m.add_vertex(Point(0, 1, 0));
    fx.v3 = m.add_vertex(Point(1, 1, 1));
    fx.fa = m.add_triangle(fx.v0, fx.v1, fx.v2);
    fx.fb = m.add_triangle(fx.v1, fx.v3, fx.v2);
    fx.edge_h = m.find_halfedge(fx.v1, fx.v2);

    auto tex = m.get_halfedge_property<TexCoord>("h:tex");
    for (auto h : m.halfedges())
        tex[h] = TexCoord(Scalar(h.idx()) * Scalar(0.1),
                          Scalar(h.idx()) * Scalar(0.05));

    auto smoothing = m.get_face_property<int>("f:smoothing_group");
    smoothing[fx.fa] = 3;
    smoothing[fx.fb] = 4;
}

int case_interior_split()
{
    SplitFixture fx;
    build_split_fixture(fx);
    auto& m = fx.m;
    auto tex = m.get_halfedge_property<TexCoord>("h:tex");
    auto hnormal = m.get_halfedge_property<Normal>("h:normal");
    auto hnormal_explicit =
        m.get_halfedge_property<bool>("h:normal_explicit");

    // side A: both edge endpoints explicit -> the new corner is explicit
    const Normal n1a = normalize(Normal(0, 0, 1));
    const Normal n2a = normalize(Normal(0, 1, 1));
    hnormal_explicit[corner_halfedge(m, fx.fa, fx.v1)] = true;
    hnormal[corner_halfedge(m, fx.fa, fx.v1)] = n1a;
    hnormal_explicit[corner_halfedge(m, fx.fa, fx.v2)] = true;
    hnormal[corner_halfedge(m, fx.fa, fx.v2)] = n2a;
    // side B: only one endpoint explicit -> the new corner stays automatic
    hnormal_explicit[corner_halfedge(m, fx.fb, fx.v1)] = true;
    hnormal[corner_halfedge(m, fx.fb, fx.v1)] = Normal(1, 0, 0);

    const TexCoord t1 = tex[corner_halfedge(m, fx.fa, fx.v1)];
    const TexCoord t2 = tex[corner_halfedge(m, fx.fa, fx.v2)];
    const TexCoord u1 = tex[corner_halfedge(m, fx.fb, fx.v1)];
    const TexCoord u2 = tex[corner_halfedge(m, fx.fb, fx.v2)];

    const Scalar t = Scalar(0.3);
    const size_t nv_before = m.n_vertices();
    const size_t nf_before = m.n_faces();
    const Vertex vnew = split_edge_with_corners(m, fx.edge_h, t);

    TASK_CHECK(m.n_vertices() == nv_before + 1);
    TASK_CHECK(m.n_faces() == nf_before + 2);
    TASK_CHECK(close3(m.position(vnew),
                      m.position(fx.v1) * (1 - t) + m.position(fx.v2) * t));

    auto smoothing = m.get_face_property<int>("f:smoothing_group");

    // inspect the four child faces, grouped by the side they belong to
    int side_a_faces = 0, side_b_faces = 0;
    for (auto f : m.faces(vnew))
    {
        bool side_a = false, side_b = false;
        for (auto v : m.vertices(f))
        {
            if (v == fx.v0)
                side_a = true;
            if (v == fx.v3)
                side_b = true;
        }
        TASK_CHECK(side_a != side_b);

        const Halfedge hnew = corner_halfedge(m, f, vnew);
        if (side_a)
        {
            ++side_a_faces;
            TASK_CHECK(smoothing[f] == 3);
            TASK_CHECK(close2(tex[hnew], t1 * (1 - t) + t2 * t));
            TASK_CHECK(hnormal_explicit[hnew]);
            const Normal expected_n =
                normalize(n1a * (1 - t) + n2a * t);
            TASK_CHECK(close3(hnormal[hnew], expected_n));
        }
        else
        {
            ++side_b_faces;
            TASK_CHECK(smoothing[f] == 4);
            TASK_CHECK(close2(tex[hnew], u1 * (1 - t) + u2 * t));
            TASK_CHECK(!hnormal_explicit[hnew]);
        }
    }
    TASK_CHECK(side_a_faces == 2 && side_b_faces == 2);

    // original corners keep their UV / explicit flag / normal per side
    for (auto f : m.faces())
        for (auto v : {fx.v0, fx.v1, fx.v2, fx.v3})
        {
            bool in = false;
            for (auto vv : m.vertices(f))
                in |= (vv == v);
            if (in)
            {
                const Halfedge h = corner_halfedge(m, f, v);
                TASK_CHECK(std::isfinite(tex[h][0]) &&
                           std::isfinite(tex[h][1]));
            }
        }
    // the preserved explicit side-A corners are still explicit with the
    // original normal. Each child face keeps only one of the two endpoints.
    for (auto f : m.faces())
    {
        bool side_a = false;
        for (auto vv : m.vertices(f))
            side_a |= (vv == fx.v0);
        if (!side_a)
            continue;
        const Halfedge h1 = corner_halfedge(m, f, fx.v1);
        const Halfedge h2 = corner_halfedge(m, f, fx.v2);
        if (h1.is_valid())
        {
            TASK_CHECK(hnormal_explicit[h1] && close3(hnormal[h1], n1a));
            TASK_CHECK(close2(tex[h1], t1));
        }
        if (h2.is_valid())
        {
            TASK_CHECK(hnormal_explicit[h2] && close3(hnormal[h2], n2a));
            TASK_CHECK(close2(tex[h2], t2));
        }
        // the opposite corner is always a preserved original
        const Halfedge h0 = corner_halfedge(m, f, fx.v0);
        TASK_CHECK(h0.is_valid() && close2(tex[h0],
            tex[corner_halfedge(m, fx.fa, fx.v0)]));
    }

    TASK_CHECK(check_all_automatic(m) == 0);
    return 0;
}

int case_boundary_split()
{
    SurfaceMesh m;
    init_corner_properties(m);
    const Vertex a0 = m.add_vertex(Point(0, 0, 0));
    const Vertex a1 = m.add_vertex(Point(1, 0, 0));
    const Vertex a2 = m.add_vertex(Point(0, 1, 0));
    const Face f = m.add_triangle(a0, a1, a2);
    auto smoothing = m.get_face_property<int>("f:smoothing_group");
    smoothing[f] = 6;
    auto tex = m.get_halfedge_property<TexCoord>("h:tex");
    const TexCoord ta = tex[corner_halfedge(m, f, a1)];
    const TexCoord tb = tex[corner_halfedge(m, f, a2)];

    const Halfedge h = m.find_halfedge(a1, a2);
    TASK_CHECK(m.is_boundary(m.opposite_halfedge(h)));
    const Scalar t = Scalar(0.7);
    const size_t nf_before = m.n_faces();
    const Vertex vnew = split_edge_with_corners(m, h, t);

    TASK_CHECK(m.n_faces() == nf_before + 1); // only one side subdivided
    int child_count = 0;
    for (auto cf : m.faces(vnew))
    {
        ++child_count;
        TASK_CHECK(smoothing[cf] == 6);
        const Halfedge hnew = corner_halfedge(m, cf, vnew);
        TASK_CHECK(close2(tex[hnew], ta * (1 - t) + tb * t));
    }
    TASK_CHECK(child_count == 2);
    TASK_CHECK(close3(m.position(vnew),
                      m.position(a1) * (1 - t) + m.position(a2) * t));
    TASK_CHECK(check_all_automatic(m) == 0);
    return 0;
}

int case_consecutive_splits()
{
    SplitFixture fx;
    build_split_fixture(fx);
    auto& m = fx.m;

    const Vertex w1 = split_edge_with_corners(m, fx.edge_h, Scalar(0.25));
    // split another, newly created edge (w1 -> v2)
    const Halfedge h2 = m.find_halfedge(w1, fx.v2);
    TASK_CHECK(h2.is_valid());
    const Vertex w2 = split_edge_with_corners(m, h2, Scalar(0.4));

    TASK_CHECK(m.n_vertices() == 6);
    TASK_CHECK(m.n_faces() == 6);
    TASK_CHECK(w1 != w2);

    // full connectivity sanity and corner consistency
    for (auto hh : m.halfedges())
    {
        TASK_CHECK(m.opposite_halfedge(m.opposite_halfedge(hh)) == hh);
        TASK_CHECK(m.next_halfedge(m.prev_halfedge(hh)) == hh);
    }
    for (auto f : m.faces())
        TASK_CHECK(m.valence(f) == 3);

    auto tex = m.get_halfedge_property<TexCoord>("h:tex");
    for (auto hh : m.halfedges())
        if (!m.is_boundary(hh))
            TASK_CHECK(std::isfinite(tex[hh][0]) &&
                       std::isfinite(tex[hh][1]));
    TASK_CHECK(check_all_automatic(m) == 0);
    return 0;
}

// --------------------------------------------------------------- roundtrip --

const char* kObjPath = ".task-build/corner_edit_roundtrip.obj";

int case_negative_obj_roundtrip()
{
    // mixed index domains, negative indices, smoothing groups and partially
    // explicit normals
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
        "vn 0 0 1\nvn 0 0 -1\n"
        "s 1\nf 1/1/1 2/2/1 3/3/2\n"
        "s off\nf 2//2 4 -2/4/1\n";
    {
        std::ofstream out(kObjPath);
        out << obj;
    }

    SurfaceMesh m;
    read_obj(m, kObjPath);
    TASK_CHECK(m.n_vertices() == 4 && m.n_faces() == 2);
    auto smoothing = m.get_face_property<int>("f:smoothing_group");
    auto hnormal = m.get_halfedge_property<Normal>("h:normal");
    auto hnormal_explicit =
        m.get_halfedge_property<bool>("h:normal_explicit");
    auto tex = m.get_halfedge_property<TexCoord>("h:tex");
    TASK_CHECK(bool(smoothing) && bool(hnormal) &&
               bool(hnormal_explicit) && bool(tex));

    // face 0: all three corners explicit; face 1: v2 and v3 explicit, v4 auto
    const std::vector<Face> faces(m.faces().begin(), m.faces().end());
    TASK_CHECK(smoothing[faces[0]] == 1);
    TASK_CHECK(smoothing[faces[1]] == 0);
    int explicit_count = 0;
    for (auto f : m.faces())
        for (auto h : m.halfedges(f))
            if (hnormal_explicit[h])
                ++explicit_count;
    TASK_CHECK(explicit_count == 5);

    update_corner_normals(m);
    TASK_CHECK(check_all_automatic(m) == 0);

    // export in per-corner mode
    IOFlags flags;
    flags.use_halfedge_normals = true;
    flags.use_halfedge_texcoords = true;
    write_obj(m, kObjPath, flags);

    // independent token inspection of the three index domains
    const ObjTokens tok = parse_obj(kObjPath);
    TASK_CHECK(tok.nv == 4);
    TASK_CHECK(tok.nvt == int(m.n_halfedges()));
    TASK_CHECK(tok.nvn == explicit_count);
    TASK_CHECK(tok.faces.size() == 2);
    TASK_CHECK(tok.smoothing[0] == 1 && tok.smoothing[1] == 0);
    int token_normals = 0;
    for (const auto& fc : tok.faces)
    {
        for (const auto& c : fc)
        {
            TASK_CHECK(c.v >= 1 && c.v <= tok.nv);
            TASK_CHECK(c.has_t && c.t >= 1 && c.t <= tok.nvt);
            if (c.has_n)
            {
                ++token_normals;
                TASK_CHECK(c.n >= 1 && c.n <= tok.nvn);
            }
        }
    }
    TASK_CHECK(token_normals == explicit_count);

    // re-read: explicit mask, groups and recomputed automatic normals survive
    SurfaceMesh back;
    read_obj(back, kObjPath);
    auto back_smoothing = back.get_face_property<int>("f:smoothing_group");
    auto back_explicit =
        back.get_halfedge_property<bool>("h:normal_explicit");
    auto back_tex = back.get_halfedge_property<TexCoord>("h:tex");
    TASK_CHECK(bool(back_smoothing) && bool(back_explicit) &&
               bool(back_tex));
    update_corner_normals(back);
    TASK_CHECK(check_all_automatic(back) == 0);

    for (auto f : m.faces())
        for (auto h : m.halfedges(f))
        {
            const Vertex from = m.from_vertex(h);
            const Vertex to = m.to_vertex(h);
            const Halfedge oh = back.find_halfedge(from, to);
            TASK_CHECK(oh.is_valid());
            TASK_CHECK(back_explicit[oh] == hnormal_explicit[h]);
            TASK_CHECK(close2(back_tex[oh], tex[h]));
            if (hnormal_explicit[h])
                TASK_CHECK(close3(back.get_halfedge_property<Normal>(
                                     "h:normal")[oh],
                                 hnormal[h]));
            TASK_CHECK(back_smoothing[back.face(oh)] == smoothing[f]);
        }

    // keep editing the re-read mesh
    const Vertex v1 = Vertex(1), v2 = Vertex(2);
    const Halfedge eh = back.find_halfedge(v1, v2);
    TASK_CHECK(eh.is_valid());
    const Vertex vnew = split_edge_with_corners(back, eh, Scalar(0.35));
    TASK_CHECK(back.n_vertices() == 5 && back.n_faces() == 4);
    TASK_CHECK(close3(back.position(vnew),
                      back.position(v1) * Scalar(0.65) +
                          back.position(v2) * Scalar(0.35)));
    TASK_CHECK(check_all_automatic(back) == 0);
    return 0;
}

int case_garbage_collection()
{
    SplitFixture fx;
    build_split_fixture(fx);
    auto& m = fx.m;

    split_edge_with_corners(m, fx.edge_h, Scalar(0.4));
    const Vertex junk = m.add_vertex(Point(7, 8, 9));
    m.delete_vertex(junk);
    m.garbage_collection();

    // splitting and round-tripping must still work after compaction
    const Halfedge live = m.find_halfedge(fx.v0, fx.v1);
    TASK_CHECK(live.is_valid());
    const Vertex vnew = split_edge_with_corners(m, live, Scalar(0.6));
    TASK_CHECK(vnew.is_valid());
    TASK_CHECK(check_all_automatic(m) == 0);

    IOFlags flags;
    flags.use_halfedge_normals = true;
    flags.use_halfedge_texcoords = true;
    write_obj(m, ".task-build/corner_edit_gc.obj", flags);
    SurfaceMesh back;
    read_obj(back, ".task-build/corner_edit_gc.obj");
    TASK_CHECK(back.n_vertices() == m.n_vertices());
    TASK_CHECK(back.n_faces() == m.n_faces());
    update_corner_normals(back);
    TASK_CHECK(check_all_automatic(back) == 0);
    return 0;
}

int case_failures_then_success()
{
    SplitFixture fx;
    build_split_fixture(fx);
    auto& m = fx.m;
    const size_t nv = m.n_vertices(), nf = m.n_faces(), ne = m.n_edges();

    auto expect_fail = [&](Halfedge h, Scalar t) -> bool {
        bool threw = false;
        try
        {
            split_edge_with_corners(m, h, t);
        }
        catch (const InvalidInputException&)
        {
            threw = true;
        }
        return threw && m.n_vertices() == nv && m.n_faces() == nf &&
               m.n_edges() == ne;
    };

    TASK_CHECK(expect_fail(Halfedge(), Scalar(0.5)));   // invalid handle
    TASK_CHECK(expect_fail(fx.edge_h, Scalar(0)));      // t out of range
    TASK_CHECK(expect_fail(fx.edge_h, Scalar(1)));
    TASK_CHECK(expect_fail(fx.edge_h, Scalar(-0.1)));
    TASK_CHECK(expect_fail(fx.edge_h, Scalar(1.1)));

    // deleted halfedge (dedicated mesh so the shared fixture stays intact)
    {
        SurfaceMesh d;
        init_corner_properties(d);
        const Vertex d0 = d.add_vertex(Point(0, 0, 0));
        const Vertex d1 = d.add_vertex(Point(1, 0, 0));
        const Vertex d2 = d.add_vertex(Point(0, 1, 0));
        d.add_triangle(d0, d1, d2);
        const Halfedge dh = d.find_halfedge(d0, d1);
        const Edge de = d.edge(dh);
        d.delete_edge(de);
        bool threw = false;
        try
        {
            split_edge_with_corners(d, dh, Scalar(0.5));
        }
        catch (const InvalidInputException&)
        {
            threw = true;
        }
        TASK_CHECK(threw);
    }

    // non-triangle neighbor
    SurfaceMesh q;
    init_corner_properties(q);
    const Vertex q0 = q.add_vertex(Point(0, 0, 0));
    const Vertex q1 = q.add_vertex(Point(1, 0, 0));
    const Vertex q2 = q.add_vertex(Point(1, 1, 0));
    const Vertex q3 = q.add_vertex(Point(0, 1, 0));
    const std::vector<Vertex> quad = {q0, q1, q2, q3};
    const Face qf = q.add_face(quad);
    TASK_CHECK(q.valence(qf) == 4);
    bool threw = false;
    try
    {
        split_edge_with_corners(q, q.find_halfedge(q0, q1), Scalar(0.5));
    }
    catch (const InvalidInputException&)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(q.n_faces() == 1);

    // zero interpolated explicit normal
    SurfaceMesh z;
    init_corner_properties(z);
    const Vertex z0 = z.add_vertex(Point(0, 0, 0));
    const Vertex z1 = z.add_vertex(Point(1, 0, 0));
    const Vertex z2 = z.add_vertex(Point(0, 1, 0));
    const Vertex z3 = z.add_vertex(Point(1, 1, 1));
    const Face zfa = z.add_triangle(z0, z1, z2);
    z.add_triangle(z1, z3, z2);
    auto zexplicit = z.get_halfedge_property<bool>("h:normal_explicit");
    auto znormals = z.get_halfedge_property<Normal>("h:normal");
    zexplicit[corner_halfedge(z, zfa, z1)] = true;
    znormals[corner_halfedge(z, zfa, z1)] = Normal(0, 0, 1);
    zexplicit[corner_halfedge(z, zfa, z2)] = true;
    znormals[corner_halfedge(z, zfa, z2)] = Normal(0, 0, -1);
    const size_t znv = z.n_vertices();
    threw = false;
    try
    {
        split_edge_with_corners(z, z.find_halfedge(z1, z2), Scalar(0.5));
    }
    catch (const InvalidInputException&)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(z.n_vertices() == znv);

    // after all the failures, a valid split still succeeds
    const Vertex ok =
        split_edge_with_corners(z, z.find_halfedge(z0, z1), Scalar(0.5));
    TASK_CHECK(ok.is_valid());
    TASK_CHECK(check_all_automatic(z) == 0);

    // a rejected call on a mesh without corner properties must not add any
    {
        SurfaceMesh plain;
        const Vertex p0 = plain.add_vertex(Point(0, 0, 0));
        const Vertex p1 = plain.add_vertex(Point(1, 0, 0));
        const Vertex p2 = plain.add_vertex(Point(0, 1, 0));
        plain.add_triangle(p0, p1, p2);
        TASK_CHECK(!plain.has_halfedge_property("h:normal"));
        TASK_CHECK(!plain.has_halfedge_property("h:normal_explicit"));
        bool threw = false;
        try
        {
            split_edge_with_corners(plain, plain.find_halfedge(p0, p1),
                                    Scalar(2.0));
        }
        catch (const InvalidInputException&)
        {
            threw = true;
        }
        TASK_CHECK(threw);
        TASK_CHECK(!plain.has_halfedge_property("h:normal"));
        TASK_CHECK(!plain.has_halfedge_property("h:normal_explicit"));
        TASK_CHECK(plain.n_vertices() == 3 && plain.n_faces() == 1);
    }
    return 0;
}

int case_legacy_writer()
{
    SplitFixture fx;
    build_split_fixture(fx);
    auto& m = fx.m;
    update_corner_normals(m);

    // new option off: the writer must keep the legacy output shape
    write_obj(m, ".task-build/corner_edit_legacy.obj", IOFlags());
    {
        std::ifstream in(".task-build/corner_edit_legacy.obj");
        std::string line;
        while (std::getline(in, line))
        {
            TASK_CHECK(line.rfind("s ", 0) != 0);
            if (line.rfind("f ", 0) == 0)
            {
                // plain "f i j k" face records, no normal domain
                std::istringstream ss(line);
                std::string tag;
                ss >> tag;
                std::string spec;
                while (ss >> spec)
                    TASK_CHECK(spec.find('/') == std::string::npos);
            }
        }
    }

    // legacy vertex-normal mode still wins when the new flag is off
    vertex_normals(m);
    IOFlags flags;
    flags.use_vertex_normals = true;
    write_obj(m, ".task-build/corner_edit_vn.obj", flags);
    const ObjTokens tok = parse_obj(".task-build/corner_edit_vn.obj");
    TASK_CHECK(tok.nvn == int(m.n_vertices()));
    for (const auto& fc : tok.faces)
        for (const auto& c : fc)
        {
            TASK_CHECK(c.has_n && !c.has_t);
            TASK_CHECK(c.n == c.v); // vertex normal indices track vertex indices
        }
    return 0;
}

} // namespace

const task_case task_cases[] = {
    {"fans", "fan_separation", case_fan_separation},
    {"fans", "group_zero", case_group_zero},
    {"fans", "uv_seam_smooth", case_uv_seam_smooth},
    {"fans", "zero_sum", case_zero_sum},
    {"split_transfer", "interior_split", case_interior_split},
    {"split_transfer", "boundary_split", case_boundary_split},
    {"split_transfer", "consecutive_splits", case_consecutive_splits},
    {"roundtrip", "negative_obj_roundtrip", case_negative_obj_roundtrip},
    {"roundtrip", "garbage_collection", case_garbage_collection},
    {"roundtrip", "failures_then_success", case_failures_then_success},
    {"roundtrip", "legacy_writer", case_legacy_writer},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
