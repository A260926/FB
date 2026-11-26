#include <polymesh/Mesh.hh>
#include <polymesh/formats/obj.hh>
#include <task_support/check.h>

#include <algorithm>
#include <array>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using pm::Mesh;
using pm::obj_read_mode;
using pm::obj_reader;

namespace
{
template <class ScalarT>
struct loaded
{
    std::unique_ptr<Mesh> mesh;
    std::unique_ptr<obj_reader<ScalarT>> reader;
};

template <class ScalarT>
loaded<ScalarT> read_repair(std::string const& text)
{
    loaded<ScalarT> result;
    result.mesh.reset(new Mesh());
    std::istringstream stream(text);
    result.reader.reset(new obj_reader<ScalarT>(stream, *result.mesh, obj_read_mode::repair));
    return result;
}

template <class ScalarT>
bool throws_invalid(std::string const& text)
{
    try
    {
        auto mesh = std::make_unique<Mesh>();
        std::istringstream stream(text);
        obj_reader<ScalarT> reader(stream, *mesh, obj_read_mode::repair);
    }
    catch (std::invalid_argument const&)
    {
        return true;
    }
    return false;
}

// global source corner ids around an output face (cyclic, arbitrary start)
template <class ScalarT>
std::vector<int> source_corner_ring(obj_reader<ScalarT> const& reader, pm::face_handle f)
{
    std::vector<int> ring;
    for (auto h : f.halfedges())
        ring.push_back(reader.halfedge_source()[(int)h]);
    return ring;
}

std::vector<int> rotate_to(std::vector<int> ring, int first_value)
{
    for (auto i = 0u; i < ring.size(); ++i)
        if (ring[i] == first_value)
        {
            std::rotate(ring.begin(), ring.begin() + i, ring.end());
            return ring;
        }
    return ring;
}

double signed_volume(Mesh const& mesh, obj_reader<double> const& reader)
{
    auto const& pos = reader.get_positions();
    auto volume = 0.0;
    for (auto f : mesh.faces())
    {
        std::vector<pm::vertex_handle> verts;
        for (auto v : f.vertices())
            verts.push_back(v);
        for (auto i = 1u; i + 1 < verts.size(); ++i)
        {
            auto const& a = pos[verts[0]];
            auto const& b = pos[verts[i]];
            auto const& c = pos[verts[i + 1]];
            volume += a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
        }
    }
    return volume / 6.0;
}

// ===== orientation group =====

int orientation_closed_tetra()
{
    // tetrahedron, faces deliberately shuffled; faces 1 and 3 are stored with
    // reversed winding and must be repaired
    std::string const obj = "v 0 0 0\n"
                            "v 1 0 0\n"
                            "v 0 1 0\n"
                            "v 0 0 1\n"
                            "f 1 3 2\n"
                            "f 1 4 2\n"
                            "f 2 3 4\n"
                            "f 4 1 3\n";

    auto loaded = read_repair<double>(obj);
    auto& mesh = *loaded.mesh;
    auto& reader = *loaded.reader;
    mesh.assert_consistency();

    TASK_CHECK(reader.error_faces() == 0);
    TASK_CHECK(mesh.faces().size() == 4);
    TASK_CHECK(mesh.vertices().size() == 4);

    // closed surface: every edge borders two faces
    for (auto e : mesh.edges())
    {
        TASK_CHECK(e.faceA().is_valid());
        TASK_CHECK(e.faceB().is_valid());
    }

    // anchor: source face 0 keeps its original winding 1 -> 3 -> 2
    auto anchor_found = false;
    for (auto f : mesh.faces())
    {
        if (reader.face_source()[(int)f] != 0)
            continue;
        anchor_found = true;
        TASK_CHECK(rotate_to(source_corner_ring(reader, f), 0) == std::vector<int>({0, 1, 2}));
    }
    TASK_CHECK(anchor_found);

    // flipped faces still start at their original first corner
    auto const expected = std::vector<std::vector<int>>{
        {0, 1, 2},      // face 0 kept
        {3, 5, 4},      // face 1 flipped: starts at corner 3
        {6, 7, 8},      // face 2 kept
        {9, 11, 10}};   // face 3 flipped: starts at corner 9
    for (auto f : mesh.faces())
    {
        auto const src = reader.face_source()[(int)f];
        auto const ring = rotate_to(source_corner_ring(reader, f), src * 3);
        TASK_CHECK(ring == expected[(size_t)src]);
    }

    // consistent outward winding gives a positive volume (tetra volume = 1/6)
    TASK_CHECK(signed_volume(mesh, reader) > 0.1);

    // the mesh stays editable: split an edge and re-check
    auto const vertices_before = mesh.vertices().size();
    mesh.edges().split(*mesh.edges().begin());
    mesh.assert_consistency();
    TASK_CHECK(mesh.vertices().size() == vertices_before + 1);
    return 0;
}

int orientation_mixed_polygons()
{
    // square pyramid: one quad base and four triangles, shuffled and partly flipped
    std::string const obj = "v -1 -1 0\n"
                            "v 1 -1 0\n"
                            "v 1 1 0\n"
                            "v -1 1 0\n"
                            "v 0 0 1\n"
                            "f 1 2 5\n"
                            "f 4 3 2 1\n"
                            "f 5 3 2\n"
                            "f 3 4 5\n"
                            "f 5 1 4\n";

    auto loaded = read_repair<float>(obj);
    auto& mesh = *loaded.mesh;
    auto& reader = *loaded.reader;
    mesh.assert_consistency();

    TASK_CHECK(reader.error_faces() == 0);
    TASK_CHECK(mesh.faces().size() == 5);
    TASK_CHECK(mesh.vertices().size() == 5);

    auto triangles = 0;
    auto quads = 0;
    for (auto f : mesh.faces())
    {
        auto const degree = (int)f.halfedges().size();
        TASK_CHECK(degree == 3 || degree == 4);
        if (degree == 3)
            ++triangles;
        else
            ++quads;
    }
    TASK_CHECK(triangles == 4);
    TASK_CHECK(quads == 1);

    // closed pyramid: 8 edges, each with two faces
    TASK_CHECK(mesh.edges().size() == 8);
    for (auto e : mesh.edges())
        TASK_CHECK(e.faceA().is_valid() && e.faceB().is_valid());

    // flipped source faces (2 and 4) start at their original first corner
    for (auto f : mesh.faces())
    {
        auto const src = reader.face_source()[(int)f];
        auto ring = source_corner_ring(reader, f);
        if (src == 2)
            TASK_CHECK(rotate_to(ring, 7) == std::vector<int>({7, 9, 8}));
        if (src == 4)
            TASK_CHECK(rotate_to(ring, 13) == std::vector<int>({13, 15, 14}));
    }
    return 0;
}

int orientation_two_components()
{
    // two edge-disconnected components (vertices 1..4 and 5..8); in each one
    // the smallest-index face must keep its winding and the other one flips
    std::string const obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\n"
                            "v 3 0 0\nv 4 0 0\nv 3 1 0\nv 4 1 0\n"
                            "f 1 2 3\n"
                            "f 2 3 4\n" // shares edge 2-3 in the same direction -> flips
                            "f 5 6 7\n"
                            "f 6 7 8\n"; // second component, same pattern

    auto loaded = read_repair<double>(obj);
    auto& mesh = *loaded.mesh;
    auto& reader = *loaded.reader;
    mesh.assert_consistency();

    TASK_CHECK(mesh.faces().size() == 4);
    TASK_CHECK(mesh.vertices().size() == 8); // no connection between the two patches

    auto const expected = std::vector<std::vector<int>>{
        {0, 1, 2},      // component A anchor, kept
        {3, 5, 4},      // component A, flipped starting at corner 3
        {6, 7, 8},      // component B anchor (smallest face of its component), kept
        {9, 11, 10}};   // component B, flipped starting at corner 9

    for (auto f : mesh.faces())
    {
        auto const src = reader.face_source()[(int)f];
        auto const ring = rotate_to(source_corner_ring(reader, f), src * 3);
        TASK_CHECK(ring == expected[(size_t)src]);
    }
    return 0;
}

int orientation_reject_topology()
{
    // three faces on one undirected edge (1-2), each with a different apex
    TASK_CHECK(throws_invalid<double>("v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nv 2 2 0\n"
                                      "f 1 2 3\nf 2 1 4\nf 1 2 5\n"));

    // duplicate face up to cyclic shift
    TASK_CHECK(throws_invalid<double>("v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                      "f 1 2 3\nf 2 3 1\n"));

    // duplicate face up to reversal
    TASK_CHECK(throws_invalid<float>("v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                     "f 1 2 3\nf 1 3 2\n"));

    // contradictory winding constraints through two shared edges
    TASK_CHECK(throws_invalid<double>("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                                      "f 1 2 3 4\nf 1 2 4 3\n"));
    return 0;
}

// ===== fans group =====

int fans_butterfly_and_isolated()
{
    // two triangles meet only at source vertex 1 (butterfly), plus an unused
    // isolated source position
    std::string const obj = "v 0 0 0\n"
                            "v 1 0 0\n"
                            "v 0 1 0\n"
                            "v 2 0 0\n"
                            "v 2 1 0\n"
                            "v 9 9 9\n"
                            "f 1 2 3\n"
                            "f 1 4 5\n";

    auto loaded = read_repair<double>(obj);
    auto& mesh = *loaded.mesh;
    auto& reader = *loaded.reader;
    mesh.assert_consistency();

    TASK_CHECK(mesh.faces().size() == 2);
    // 5 used source positions, vertex 1 duplicated into two fans, plus isolated
    TASK_CHECK(mesh.vertices().size() == 7);

    auto shared_copies = 0;
    auto isolated_copies = 0;
    for (auto i = 0; i < (int)reader.vertex_source().size(); ++i)
    {
        if (reader.vertex_source()[(size_t)i] == 0)
            ++shared_copies;
        if (reader.vertex_source()[(size_t)i] == 5)
            ++isolated_copies;
    }
    TASK_CHECK(shared_copies == 2);
    TASK_CHECK(isolated_copies == 1);

    // every non-isolated output vertex carries a single connected face fan
    for (auto v : mesh.vertices())
    {
        if (v.is_isolated())
            continue;
        TASK_CHECK(v.faces().size() >= 1);
        for (auto f : v.faces())
            TASK_CHECK(f.is_valid());
    }

    // the isolated output vertex really is isolated in the half-edge mesh
    auto isolated_found = false;
    for (auto v : mesh.vertices())
        if (v.is_isolated())
        {
            isolated_found = true;
            TASK_CHECK(v.edges().size() == 0);
        }
    TASK_CHECK(isolated_found);

    // both patches are open: every edge has exactly one face
    for (auto e : mesh.edges())
    {
        auto const faces = (e.faceA().is_valid() ? 1 : 0) + (e.faceB().is_valid() ? 1 : 0);
        TASK_CHECK(faces == 1);
    }

    // boundary halfedges form closed next/prev loops: walking next from any
    // boundary halfedge returns to it after traversing the whole fan boundary
    for (auto e : mesh.edges())
    {
        auto h = e.halfedgeA().is_boundary() ? e.halfedgeA() : e.halfedgeB();
        TASK_CHECK(h.is_boundary());
        auto const start = h;
        auto steps = 0;
        do
        {
            TASK_CHECK(h.is_boundary());
            TASK_CHECK(h.next().prev() == h);
            h = h.next();
            ++steps;
            TASK_CHECK(steps <= 6);
        } while (h != start);
    }

    // boundary halfedges carry source -1, face halfedges carry real corners
    auto boundary = 0;
    for (auto i = 0; i < (int)reader.halfedge_source().size(); ++i)
        if (reader.halfedge_source()[(size_t)i] == -1)
            ++boundary;
        else
            TASK_CHECK(reader.halfedge_source()[(size_t)i] >= 0);
    TASK_CHECK(boundary == 6); // six boundary edges around the two triangles

    // really edit the repaired mesh
    auto const vertices_before = mesh.vertices().size();
    mesh.edges().split(*mesh.edges().begin());
    mesh.assert_consistency();
    TASK_CHECK(mesh.vertices().size() == vertices_before + 1);
    TASK_CHECK(mesh.faces().size() == 2);
    return 0;
}

int fans_no_weld_coincident()
{
    // source vertices 1 and 2 share identical coordinates but have different
    // indices; two disconnected patches use one each and must not be welded
    std::string const obj = "v 0 0 0\n"
                            "v 0 0 0\n"
                            "v 1 0 0\n"
                            "v 0 1 0\n"
                            "v 2 0 0\n"
                            "v 2 1 0\n"
                            "f 1 3 4\n"
                            "f 2 5 6\n";

    auto loaded = read_repair<double>(obj);
    auto& mesh = *loaded.mesh;
    auto& reader = *loaded.reader;
    mesh.assert_consistency();

    // no welding: six distinct source positions remain six output vertices
    TASK_CHECK(mesh.vertices().size() == 6);
    TASK_CHECK(mesh.faces().size() == 2);

    auto has_source = [&](int src) {
        for (auto s : reader.vertex_source())
            if (s == src)
                return true;
        return false;
    };
    TASK_CHECK(has_source(0));
    TASK_CHECK(has_source(1));

    // the two coincident positions are distinct slots carrying equal data
    auto slots = std::vector<int>();
    for (auto i = 0; i < (int)reader.vertex_source().size(); ++i)
        if (reader.vertex_source()[(size_t)i] <= 1)
            slots.push_back(i);
    TASK_CHECK(slots.size() == 2);
    auto const& pos = reader.get_positions();
    TASK_CHECK(pos[pm::vertex_index(slots[0])] == pos[pm::vertex_index(slots[1])]);
    return 0;
}

int fans_reject_indices()
{
    // zero index
    TASK_CHECK(throws_invalid<double>("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n"));
    // out of range
    TASK_CHECK(throws_invalid<double>("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 9\n"));
    // repeated vertex within a face
    TASK_CHECK(throws_invalid<float>("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 1\n"));
    // out of range texcoord
    TASK_CHECK(throws_invalid<double>("v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                      "vt 0 0\nvt 1 0\nvt 0 1\n"
                                      "f 1/1 2/2 3/9\n"));
    // out of range normal
    TASK_CHECK(throws_invalid<float>("v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                     "vn 0 0 1\nvn 0 1 0\nvn 1 0 0\n"
                                     "f 1//1 2//2 3//9\n"));
    return 0;
}

// ===== corner_data group =====

int corner_data_uv_seam()
{
    std::string const obj = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                            "vt 0.1 0.1\nvt 0.2 0.2\nvt 0.3 0.3\nvt 0.4 0.4\nvt 0.5 0.5\nvt 0.6 0.6\n"
                            "vn 1 0 0\nvn 0 1 0\nvn 0 0 1\nvn 0 0 -1\nvn -1 0 0\nvn 0 -1 0\n"
                            "f 1/1/1 2/2/2 3/3/3\n"
                            "f 1/4/4 3/5/5 4/6/6\n";

    auto loaded = read_repair<float>(obj);
    auto& mesh = *loaded.mesh;
    auto& reader = *loaded.reader;
    mesh.assert_consistency();

    // the shared edge connects the fans, so no vertex duplication is needed
    TASK_CHECK(mesh.vertices().size() == 4);
    TASK_CHECK(reader.vertex_source() == std::vector<int>({0, 1, 2, 3}));

    auto const& uv = reader.get_tex_coords();
    auto const& nrm = reader.get_normals();

    // expected per global source corner (face 0: corners 0..2, face 1: 3..5)
    auto const expected_uv = std::vector<std::array<float, 3>>{
        {0.1f, 0.1f, 1.f}, {0.2f, 0.2f, 1.f}, {0.3f, 0.3f, 1.f},
        {0.4f, 0.4f, 1.f}, {0.5f, 0.5f, 1.f}, {0.6f, 0.6f, 1.f}};
    auto const expected_n = std::vector<std::array<float, 3>>{
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}, {-1, 0, 0}, {0, -1, 0}};

    auto seen_corners = std::vector<int>();
    for (auto f : mesh.faces())
        for (auto h : f.halfedges())
        {
            auto const src = reader.halfedge_source()[(int)h];
            TASK_CHECK(src >= 0);
            seen_corners.push_back(src);
            TASK_CHECK(uv[h] == expected_uv[(size_t)src]);
            TASK_CHECK(nrm[h] == expected_n[(size_t)src]);
        }
    std::sort(seen_corners.begin(), seen_corners.end());
    TASK_CHECK(seen_corners == std::vector<int>({0, 1, 2, 3, 4, 5}));

    // UV seam: the two halfedges on the shared edge keep their own corner data
    auto seam_found = false;
    for (auto e : mesh.edges())
        if (e.faceA().is_valid() && e.faceB().is_valid())
        {
            seam_found = true;
            TASK_CHECK(uv[e.halfedgeA()] != uv[e.halfedgeB()]);
            TASK_CHECK(nrm[e.halfedgeA()] != nrm[e.halfedgeB()]);
        }
    TASK_CHECK(seam_found);

    // four boundary halfedges keep source -1
    auto boundary = 0;
    for (auto i = 0; i < (int)reader.halfedge_source().size(); ++i)
        if (reader.halfedge_source()[(size_t)i] == -1)
            ++boundary;
    TASK_CHECK(boundary == 4);

    // positions keep all four components
    for (auto v : mesh.vertices())
        TASK_CHECK(reader.get_positions()[v][3] == 1.0f);

    mesh.edges().split(*mesh.edges().begin());
    mesh.assert_consistency();
    return 0;
}

int corner_data_flip_keeps_normals_and_defaults()
{
    // second face is stored backwards; its explicit normals must survive the
    // flip verbatim (never negated), and missing uv stays at the default value
    std::string const obj = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                            "vn 0 0 1\nvn 0 0 2\nvn 0 0 3\nvn 0 0 4\nvn 0 0 5\nvn 0 0 6\n"
                            "f 1//1 2//2 3//3\n"
                            "f 1//4 4//5 3//6\n";

    auto loaded = read_repair<double>(obj);
    auto& mesh = *loaded.mesh;
    auto& reader = *loaded.reader;
    mesh.assert_consistency();

    TASK_CHECK(mesh.faces().size() == 2);

    auto const& nrm = reader.get_normals();
    auto const& uv = reader.get_tex_coords();
    std::array<double, 3> const zero_uv{};
    for (auto f : mesh.faces())
        for (auto h : f.halfedges())
        {
            auto const src = reader.halfedge_source()[(int)h];
            TASK_CHECK(src >= 0);
            // normals copied from the original corner, z encodes the corner id
            TASK_CHECK(nrm[h][2] == double(src + 1));
            // no vt given anywhere: legacy default (zero) remains
            TASK_CHECK(uv[h] == zero_uv);
        }

    // face 1 had to flip; its first output corner is still source corner 3
    auto flipped_ring = std::vector<int>();
    for (auto f : mesh.faces())
        if (reader.face_source()[(int)f] == 1)
            flipped_ring = source_corner_ring(reader, f);
    flipped_ring = rotate_to(flipped_ring, 3);
    TASK_CHECK(flipped_ring == std::vector<int>({3, 5, 4}));
    return 0;
}
} // namespace

#define TASK_CASE(group, name) \
    {                          \
        group, #name, name    \
    }

const struct task_case task_cases[] = {
    TASK_CASE("orientation", orientation_closed_tetra),
    TASK_CASE("orientation", orientation_mixed_polygons),
    TASK_CASE("orientation", orientation_two_components),
    TASK_CASE("orientation", orientation_reject_topology),
    TASK_CASE("fans", fans_butterfly_and_isolated),
    TASK_CASE("fans", fans_no_weld_coincident),
    TASK_CASE("fans", fans_reject_indices),
    TASK_CASE("corner_data", corner_data_uv_seam),
    TASK_CASE("corner_data", corner_data_flip_keeps_normals_and_defaults),
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
