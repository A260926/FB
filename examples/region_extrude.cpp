// Minimal example: extrude an interior region of a quad grid and write the result to OBJ.
//
// Usage: region_extrude <output.obj>

#include <polymesh/Mesh.hh>
#include <polymesh/algorithms/region_extrude.hh>
#include <polymesh/formats/obj.hh>

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

using namespace polymesh;

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: region_extrude <output.obj>\n";
        return 2;
    }

    Mesh m;
    auto pos = m.vertices().make_attribute<std::array<double, 4>>();
    auto uv = m.halfedges().make_attribute<std::array<double, 3>>();
    auto normal = m.halfedges().make_attribute<std::array<double, 3>>();

    // 5x5 vertex grid -> 4x4 quads
    auto const N = 5;
    std::vector<vertex_handle> vh(size_t(N) * size_t(N));
    for (auto y = 0; y < N; ++y)
        for (auto x = 0; x < N; ++x)
        {
            auto v = m.vertices().add();
            vh[size_t(y) * size_t(N) + size_t(x)] = v;
            pos[v] = {double(x), double(y), 0.0, 1.0};
        }

    auto const F = N - 1;
    std::vector<face_handle> faces;
    for (auto y = 0; y < F; ++y)
        for (auto x = 0; x < F; ++x)
            faces.push_back(m.faces().add(vh[size_t(y) * size_t(N) + size_t(x)], //
                                          vh[size_t(y) * size_t(N) + size_t(x + 1)], //
                                          vh[size_t(y + 1) * size_t(N) + size_t(x + 1)], //
                                          vh[size_t(y + 1) * size_t(N) + size_t(x)]));

    // initial per-corner UVs (planar) and normals
    for (auto h : m.halfedges())
    {
        auto p = pos[h.vertex_to()];
        uv[h] = {p[0] / double(F), p[1] / double(F), 0.0};
        normal[h] = {0.0, 0.0, 1.0};
    }

    // extrude a centered 2x2 patch upward
    std::vector<face_handle> region = {
        faces[size_t(1 * F + 1)], faces[size_t(1 * F + 2)], //
        faces[size_t(2 * F + 1)], faces[size_t(2 * F + 2)], //
    };

    auto caps = extrude_region(m, pos, uv, normal, region, {0.0, 0.0, 1.5});
    m.assert_consistency();

    m.compactify();
    m.assert_consistency();

    {
        obj_writer<double> writer(argv[1]);
        writer.write_object_name("region_extrude");
        writer.write_mesh(pos, &uv, &normal);
    }

    std::cout << "extruded " << caps.size() << " face(s): " << m.vertices().size() << " vertices, " << m.edges().size()
              << " edges, " << m.faces().size() << " faces -> " << argv[1] << "\n";
    return 0;
}
