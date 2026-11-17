#include "../task_support/check.h"

#include <polymesh/Mesh.hh>
#include <polymesh/formats/obj.hh>

#include <array>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace polymesh;

namespace
{
std::istringstream make_stream(std::string const& s)
{
    return std::istringstream(s);
}

/// walks the corners of a face and returns their 1-based source position
/// indices, rotated so the first output corner (smallest half-edge source
/// index of that face) comes first
std::vector<int> source_cycle(face_handle f, std::vector<int> const& vsrc, std::vector<int> const& hsrc)
{
    std::vector<halfedge_handle> ring;
    for (auto h : f.halfedges())
        ring.push_back(h);

    auto first = 0u;
    for (auto i = 1u; i < ring.size(); ++i)
        if (hsrc[int(ring[i])] < hsrc[int(ring[first])])
            first = i;

    std::vector<int> seq;
    for (auto i = 0u; i < ring.size(); ++i)
    {
        auto const h = ring[(first + i) % ring.size()];
        seq.push_back(vsrc[int(h.vertex_to())] + 1);
    }
    return seq;
}

/// half-edge of a face that carries the given source corner index, or invalid
halfedge_handle halfedge_for_corner(face_handle f, std::vector<int> const& hsrc, int corner)
{
    for (auto h : f.halfedges())
        if (hsrc[int(h)] == corner)
            return h;
    return {};
}

/// a repaired face must either keep its source cycle verbatim or reverse it
/// while keeping the original first corner first
bool matches_flip_rule(std::vector<int> const& seq, std::vector<int> const& expect)
{
    if (seq == expect)
        return true;
    if (seq.empty() || seq[0] != expect[0])
        return false;
    auto reversed = expect;
    std::reverse(reversed.begin(), reversed.end());
    // rotate reversed cycle so it starts at its first corner again
    while (reversed.front() != expect[0])
        std::rotate(reversed.begin(), reversed.begin() + 1, reversed.end());
    return seq == reversed;
}

bool throws_invalid_argument(std::string const& obj)
{
    Mesh m;
    try
    {
        auto stream = make_stream(obj);
        obj_reader<double> reader(stream, m, obj_read_mode::repair);
    }
    catch (std::invalid_argument const&)
    {
        return true;
    }
    return false;
}

// ======================== orientation ========================

int orientation_closed_scrambled()
{
    // the four distinct tetrahedron faces, written in shuffled order with
    // three of them authored with reversed (inward) winding; the anchor face
    // keeps its orientation and the rest are propagated to match
    std::string const obj = R"(
v 0 0 0
v 1 0 0
v 0 1 0
v 0 0 1
f 2 4 3
f 1 4 2
f 1 3 2
f 1 3 4
)";
    // source face table in written order
    std::vector<std::vector<int>> src = {{2, 4, 3}, {1, 4, 2}, {1, 3, 2}, {1, 3, 4}};

    Mesh m;
    auto stream = make_stream(obj);
    obj_reader<double> reader(stream, m, obj_read_mode::repair);
    m.assert_consistency();

    TASK_CHECK(reader.error_faces() == 0);
    TASK_CHECK(m.faces().size() == 4);
    TASK_CHECK(m.vertices().size() == 4);

    // closed: every edge has two adjacent faces
    for (auto e : m.edges())
    {
        TASK_CHECK(e.faceA().is_valid());
        TASK_CHECK(e.faceB().is_valid());
    }

    auto const& vsrc = reader.vertex_source();
    auto const& fsrc = reader.face_source();
    auto const& hsrc = reader.halfedge_source();
    TASK_CHECK(fsrc.size() == 4);

    // every face follows the flip rule, and the anchor (smallest source face
    // of the component) keeps its exact original cycle
    auto anchor_seen = false;
    for (auto f : m.faces())
    {
        auto const sf = fsrc[int(f)];
        auto const seq = source_cycle(f, vsrc, hsrc);
        TASK_CHECK(matches_flip_rule(seq, src[sf]));
        if (sf == 0)
        {
            anchor_seen = true;
            TASK_CHECK(seq == src[0]);
            // first half-edge still maps to the first source corner
            auto const h0 = halfedge_for_corner(f, hsrc, 0);
            TASK_CHECK(h0.is_valid());
        }
    }
    TASK_CHECK(anchor_seen);

    // geometric check: every face normal points to the same side of its
    // centroid relative to the tetra center, i.e. winding is globally
    // consistent (the anchor face decides which side)
    auto const& pos = reader.get_positions();
    std::array<double, 3> center = {0.25, 0.25, 0.25};
    auto expected_sign = 0;
    for (auto f : m.faces())
    {
        std::vector<std::array<double, 3>> p;
        for (auto v : f.vertices())
            p.push_back({pos[v][0], pos[v][1], pos[v][2]});
        std::array<double, 3> u = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
        std::array<double, 3> w = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
        std::array<double, 3> n = {u[1] * w[2] - u[2] * w[1], //
                                   u[2] * w[0] - u[0] * w[2], //
                                   u[0] * w[1] - u[1] * w[0]};
        std::array<double, 3> fc = {(p[0][0] + p[1][0] + p[2][0]) / 3.0, //
                                    (p[0][1] + p[1][1] + p[2][1]) / 3.0, //
                                    (p[0][2] + p[1][2] + p[2][2]) / 3.0};
        auto const dot = n[0] * (fc[0] - center[0]) + n[1] * (fc[1] - center[1]) + n[2] * (fc[2] - center[2]);
        TASK_CHECK(dot != 0.0);
        if (expected_sign == 0)
            expected_sign = dot > 0.0 ? 1 : -1;
        else
            TASK_CHECK((dot > 0.0 ? 1 : -1) == expected_sign);
    }
    return 0;
}

int orientation_tri_quad_patch()
{
    // quad plus triangle sharing edge (1,4); the triangle is authored flipped
    std::string const obj = R"(
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
v 0 2 0
f 1 2 3 4
f 4 1 5
)";
    std::vector<std::vector<int>> src = {{1, 2, 3, 4}, {4, 1, 5}};

    Mesh m;
    auto stream = make_stream(obj);
    obj_reader<double> reader(stream, m, obj_read_mode::repair);
    m.assert_consistency();

    TASK_CHECK(reader.error_faces() == 0);
    TASK_CHECK(m.faces().size() == 2);

    auto quad_seen = false;
    auto tri_seen = false;
    auto const& vsrc = reader.vertex_source();
    auto const& fsrc = reader.face_source();
    auto const& hsrc = reader.halfedge_source();
    for (auto f : m.faces())
    {
        auto const sf = fsrc[int(f)];
        TASK_CHECK(matches_flip_rule(source_cycle(f, vsrc, hsrc), src[sf]));
        if (f.halfedges().size() == 4)
            quad_seen = true;
        if (f.halfedges().size() == 3)
            tri_seen = true;
    }
    TASK_CHECK(quad_seen);
    TASK_CHECK(tri_seen);

    // the shared edge 1-4 stays a single edge adjacent to both faces
    // (a half-edge mesh then guarantees the faces traverse it oppositely)
    auto shared_found = false;
    for (auto e : m.edges())
    {
        auto a = vsrc[int(e.vertexA())] + 1;
        auto b = vsrc[int(e.vertexB())] + 1;
        if ((a == 1 && b == 4) || (a == 4 && b == 1))
        {
            shared_found = true;
            TASK_CHECK(e.faceA().is_valid() && e.faceB().is_valid());
            TASK_CHECK(e.faceA() != e.faceB());
            TASK_CHECK(e.halfedgeA().opposite() == e.halfedgeB());
        }
    }
    TASK_CHECK(shared_found);

    // boundary half-edges form complete next/prev loops
    auto boundary_count = 0;
    for (auto h : m.halfedges())
        if (h.is_boundary())
        {
            ++boundary_count;
            auto walk = h;
            do
            {
                TASK_CHECK(walk.is_boundary());
                TASK_CHECK(walk.next().prev() == walk);
                walk = walk.next();
            } while (walk != h);
        }
    TASK_CHECK(boundary_count == 5); // six distinct edges, one shared by both faces
    return 0;
}

int orientation_contradiction_throws()
{
    // five quads forming a Mobius strip: the seam forces an odd flip parity
    std::string const obj = R"(
v 1 0 0
v 2 0 0
v 3 0 0
v 4 0 0
v 5 0 0
v 6 0 0
v 7 0 0
v 8 0 0
v 9 0 0
v 10 0 0
f 1 6 7 2
f 2 7 8 3
f 3 8 9 4
f 4 9 10 5
f 5 10 1 6
)";
    TASK_CHECK(throws_invalid_argument(obj));
    return 0;
}

// ======================== fans ========================

int fans_butterfly_split()
{
    // two triangles touching only at source vertex 1: the butterfly vertex
    // must be split into two output vertices
    std::string const obj = R"(
v 0 0 0
v 1 0 0
v 0 1 0
v 0 0 1
v 1 0 1
f 1 2 3
f 1 4 5
)";
    Mesh m;
    auto stream = make_stream(obj);
    obj_reader<double> reader(stream, m, obj_read_mode::repair);
    m.assert_consistency();

    TASK_CHECK(reader.error_faces() == 0);
    TASK_CHECK(m.faces().size() == 2);
    TASK_CHECK(m.vertices().size() == 6); // 5 source positions + one split copy

    auto const& vsrc = reader.vertex_source();
    auto copies_of_first = 0;
    for (auto v : m.vertices())
    {
        if (vsrc[int(v)] == 0)
        {
            ++copies_of_first;
            TASK_CHECK(v.faces().count() == 1); // one connected fan per output vertex
        }
    }
    TASK_CHECK(copies_of_first == 2);

    // the two components do not share an edge
    for (auto e : m.edges())
        TASK_CHECK(!(e.faceA().is_valid() && e.faceB().is_valid()));

    // the result must still support editing: split an edge, remove a face
    auto const edges_before = m.edges().size();
    auto const verts_before = m.vertices().size();
    auto e = *m.edges().begin();
    m.edges().split(e);
    m.assert_consistency();
    TASK_CHECK(m.edges().size() == edges_before + 1);
    TASK_CHECK(m.vertices().size() == verts_before + 1);

    m.faces().remove(*m.faces().begin());
    m.compactify();
    m.assert_consistency();
    TASK_CHECK(m.faces().size() == 1);
    return 0;
}

int fans_three_fans_and_isolated()
{
    // three disconnected triangles all reuse source vertex 1, plus an
    // unreferenced isolated position
    std::string const obj = R"(
v 0 0 0
v 1 0 0
v 0 1 0
v 1 1 0
v 2 0 0
v 2 1 0
v 3 0 0
v 9 9 9
f 1 2 3
f 1 4 5
f 1 6 7
)";
    Mesh m;
    auto stream = make_stream(obj);
    obj_reader<double> reader(stream, m, obj_read_mode::repair);
    m.assert_consistency();

    TASK_CHECK(m.faces().size() == 3);
    TASK_CHECK(m.vertices().size() == 10); // 8 source positions + 2 split copies

    auto const& vsrc = reader.vertex_source();
    auto fan_copies = 0;
    auto isolated = 0;
    for (auto v : m.vertices())
    {
        if (vsrc[int(v)] == 0)
        {
            ++fan_copies;
            TASK_CHECK(v.faces().count() == 1);
        }
        if (v.is_isolated())
        {
            ++isolated;
            TASK_CHECK(vsrc[int(v)] == 7);
        }
    }
    TASK_CHECK(fan_copies == 3);
    TASK_CHECK(isolated == 1);
    return 0;
}

int fans_no_weld_coincident()
{
    // vertex 6 has the same coordinates as vertex 1 but a different index:
    // coincident positions must never be welded
    std::string const obj = R"(
v 0 0 0
v 1 0 0
v 0 1 0
v 0 0 1
v 1 0 1
v 0 0 0
f 1 2 3
f 6 4 5
)";
    Mesh m;
    auto stream = make_stream(obj);
    obj_reader<double> reader(stream, m, obj_read_mode::repair);
    m.assert_consistency();

    TASK_CHECK(m.vertices().size() == 6); // no split, no weld
    auto const& pos = reader.get_positions();
    auto const& vsrc = reader.vertex_source();

    auto slot_of_source = [&](int source) {
        for (auto i = 0u; i < vsrc.size(); ++i)
            if (vsrc[i] == source)
                return int(i);
        return -1;
    };
    auto const s0 = slot_of_source(0);
    auto const s5 = slot_of_source(5);
    TASK_CHECK(s0 >= 0 && s5 >= 0 && s0 != s5);
    for (auto i = 0; i < 4; ++i)
        TASK_CHECK(pos[m.vertices()[s0]][i] == pos[m.vertices()[s5]][i]);
    return 0;
}

int fans_topology_errors_throw()
{
    TASK_CHECK(throws_invalid_argument("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n"));
    TASK_CHECK(throws_invalid_argument("v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nf 1 2 5\n"));
    TASK_CHECK(throws_invalid_argument("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 1\n"));
    TASK_CHECK(throws_invalid_argument("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\nf 2 1 3\n"));
    TASK_CHECK(throws_invalid_argument("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\nf 2 3 1\n"));
    TASK_CHECK(throws_invalid_argument(
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nv 0 2 0\nf 1 2 3\nf 1 2 4\nf 1 2 5\n"));
    return 0;
}

// ======================== corner_data ========================

int corner_uv_seam_and_sources()
{
    // two triangles sharing edge (2,3), each corner carries its own vt
    std::string const obj = R"(
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
vt 1 0
vt 2 0
vt 3 0
vt 4 0
vt 5 0
vt 6 0
f 1/1 2/2 3/3
f 2/4 4/5 3/6
)";
    Mesh m;
    auto stream = make_stream(obj);
    obj_reader<double> reader(stream, m, obj_read_mode::repair);
    m.assert_consistency();

    auto const& vsrc = reader.vertex_source();
    auto const& fsrc = reader.face_source();
    auto const& hsrc = reader.halfedge_source();
    auto const& tex = reader.get_tex_coords();
    auto const& nrm = reader.get_normals();

    TASK_CHECK(vsrc.size() == 4);
    TASK_CHECK((fsrc == std::vector<int>{0, 1}));

    auto face_halfedges = 0;
    auto boundary_halfedges = 0;
    for (auto h : m.halfedges())
    {
        if (h.is_boundary())
        {
            ++boundary_halfedges;
            TASK_CHECK(hsrc[int(h)] == -1);
        }
        else
        {
            ++face_halfedges;
            auto const src = hsrc[int(h)];
            TASK_CHECK(src >= 0 && src < 6);
            // tex coords follow the corner that owns the half-edge
            TASK_CHECK(tex[h][0] == double(src + 1));
            // no normals authored: default value stays zero
            TASK_CHECK(nrm[h][0] == 0.0 && nrm[h][1] == 0.0 && nrm[h][2] == 0.0);
        }
    }
    TASK_CHECK(face_halfedges == 6);
    TASK_CHECK(boundary_halfedges == 4);

    // the two sides of the UV seam keep their own tex data
    for (auto e : m.edges())
    {
        auto a = vsrc[int(e.vertexA())] + 1;
        auto b = vsrc[int(e.vertexB())] + 1;
        if ((a == 2 && b == 3) || (a == 3 && b == 2))
        {
            TASK_CHECK(e.faceA().is_valid() && e.faceB().is_valid());
            TASK_CHECK(tex[e.halfedgeA()][0] != tex[e.halfedgeB()][0]);
        }
    }
    return 0;
}

int corner_flip_keeps_explicit_normals()
{
    // second triangle is authored with reversed winding, forcing a flip;
    // explicit normals travel with the corners and are never negated
    std::string const obj = R"(
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
vn 101 0 0
vn 102 0 0
vn 103 0 0
vn 104 0 0
vn 105 0 0
vn 106 0 0
f 1//1 2//2 3//3
f 2//4 3//6 4//5
)";
    Mesh m;
    auto stream = make_stream(obj);
    obj_reader<double> reader(stream, m, obj_read_mode::repair);
    m.assert_consistency();

    auto const& hsrc = reader.halfedge_source();
    auto const& fsrc = reader.face_source();
    auto const& nrm = reader.get_normals();
    auto const& tex = reader.get_tex_coords();

    // source corner -> authored normal index (face 1 is written 2//4 3//6 4//5)
    int const expected_normal[6] = {1, 2, 3, 4, 6, 5};

    for (auto f : m.faces())
        for (auto h : f.halfedges())
        {
            auto const src = hsrc[int(h)];
            TASK_CHECK(src >= 0 && src < 6);
            // verbatim copy: normal x (authored as 100 + normal index) equals
            // the authored normal's value, never negated despite the flip
            TASK_CHECK(nrm[h][0] == double(100 + expected_normal[src]));
            TASK_CHECK(tex[h][0] == 0.0);
        }

    // the flipped face still starts at its original first corner (v2, normal 4)
    for (auto f : m.faces())
        if (fsrc[int(f)] == 1)
        {
            auto const h0 = halfedge_for_corner(f, hsrc, 3); // global corner 3
            TASK_CHECK(h0.is_valid());
            TASK_CHECK(nrm[h0][0] == 104.0);
        }
    return 0;
}

int corner_missing_attributes_default()
{
    std::string const obj = R"(
v 0 0 0
v 1 0 0
v 0 1 0
# positions only, no vt/vn
f 1 2 3
)";
    Mesh m;
    auto stream = make_stream(obj);
    obj_reader<double> reader(stream, m, obj_read_mode::repair);
    m.assert_consistency();

    TASK_CHECK(reader.error_faces() == 0);
    auto const& hsrc = reader.halfedge_source();
    auto const& tex = reader.get_tex_coords();
    auto const& nrm = reader.get_normals();

    auto face_corners = 0;
    for (auto h : m.halfedges())
    {
        if (h.is_boundary())
        {
            TASK_CHECK(hsrc[int(h)] == -1);
            continue;
        }
        ++face_corners;
        TASK_CHECK(hsrc[int(h)] >= 0);
        for (auto i = 0; i < 3; ++i)
        {
            TASK_CHECK(tex[h][i] == 0.0);
            TASK_CHECK(nrm[h][i] == 0.0);
        }
    }
    TASK_CHECK(face_corners == 3);
    return 0;
}
} // namespace

const task_case task_cases[] = {
    {"orientation", "closed_scrambled", &orientation_closed_scrambled},
    {"orientation", "tri_quad_patch", &orientation_tri_quad_patch},
    {"orientation", "contradiction_throws", &orientation_contradiction_throws},
    {"fans", "butterfly_split", &fans_butterfly_split},
    {"fans", "three_fans_and_isolated", &fans_three_fans_and_isolated},
    {"fans", "no_weld_coincident", &fans_no_weld_coincident},
    {"fans", "topology_errors_throw", &fans_topology_errors_throw},
    {"corner_data", "uv_seam_and_sources", &corner_uv_seam_and_sources},
    {"corner_data", "flip_keeps_normals", &corner_flip_keeps_explicit_normals},
    {"corner_data", "missing_attributes_default", &corner_missing_attributes_default},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
