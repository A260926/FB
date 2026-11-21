// Normal baker example.
//
// Reads a small embedded high-poly / low-poly OBJ pair, builds a bake session,
// performs one real position update, and writes the tangent-space normal map
// as an ASCII PPM (P3) file.
//
// Usage: normal_baker <output.ppm>

#include <polymesh/Mesh.hh>
#include <polymesh/algorithms/normal_baker.hh>
#include <polymesh/formats/obj.hh>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
std::string const low_poly_obj = R"(# low-poly: two triangles forming a unit square in the z = 0 plane
v -1 -1 0 1
v  1 -1 0 1
v  1  1 0 1
v -1  1 0 1
vt 0 0
vt 1 0
vt 1 1
vt 0 1
f 1/1 2/2 3/3
f 1/1 3/3 4/4
)";

std::string const high_poly_obj = R"(# high-poly: a bumpy grid above the low-poly square
v -1 -1 0.2 1
v  0 -1 0.6 1
v  1 -1 0.2 1
v -1  0 0.6 1
v  0  0 1.0 1
v  1  0 0.6 1
v -1  1 0.2 1
v  0  1 0.6 1
v  1  1 0.2 1
f 1 2 5
f 1 5 4
f 2 3 6
f 2 6 5
f 4 5 8
f 4 8 7
f 5 6 9
f 5 9 8
)";

int channel(double v)
{
    auto const c = int(std::lround(255.0 * v));
    return std::max(0, std::min(255, c));
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: normal_baker <output.ppm>\n";
        return 1;
    }

    auto width = 64;
    auto height = 64;
    auto const r = 0.5;

    // load the high-poly mesh
    pm::Mesh high_mesh;
    std::istringstream high_in(high_poly_obj);
    pm::obj_reader<double> high_reader(high_in, high_mesh);
    if (high_reader.error_faces() != 0 || high_mesh.faces().size() != 8)
    {
        std::cerr << "failed to read the embedded high-poly mesh\n";
        return 2;
    }

    // load the low-poly mesh
    pm::Mesh low_mesh;
    std::istringstream low_in(low_poly_obj);
    pm::obj_reader<double> low_reader(low_in, low_mesh);
    if (low_reader.error_faces() != 0 || low_mesh.faces().size() != 2)
    {
        std::cerr << "failed to read the embedded low-poly mesh\n";
        return 3;
    }

    auto const& high_pos = high_reader.get_positions();
    auto const& low_pos = low_reader.get_positions();
    auto const& low_uv = low_reader.get_tex_coords();

    pm::normal_bake_session session(high_pos, low_pos, low_uv, width, height, r);

    // perform one real edit: lift the central high-poly vertex
    std::vector<std::pair<int, std::array<double, 3>>> updates;
    updates.emplace_back(4, std::array<double, 3>{{0.0, 0.0, 1.2}});
    session.update_positions(updates);

    auto const& result = session.result();
    if (int(result.size()) != width * height)
    {
        std::cerr << "unexpected result size\n";
        return 4;
    }

    // encode: hit -> 0.5 * (normal + 1), miss -> black
    std::ofstream out(argv[1]);
    if (!out.good())
    {
        std::cerr << "cannot open output file\n";
        return 5;
    }
    out << "P3\n" << width << " " << height << "\n255\n";
    for (auto y = 0; y < height; ++y)
    {
        for (auto x = 0; x < width; ++x)
        {
            auto const& texel = result[size_t(y * width + x)];
            if (texel.hit)
            {
                auto const& n = texel.normal;
                out << channel(0.5 * (n[0] + 1.0)) << " " << channel(0.5 * (n[1] + 1.0)) << " " << channel(0.5 * (n[2] + 1.0));
            }
            else
            {
                out << "0 0 0";
            }
            out << (((x + 1) % 16 == 0) ? "\n" : " ");
        }
    }
    out.flush();
    if (!out.good())
    {
        std::cerr << "failed to write output\n";
        return 6;
    }

    std::cout << "normal_baker wrote " << width << "x" << height << " to " << argv[1] << "\n";
    return 0;
}
