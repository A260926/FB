// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

// Console example: optimize an existing annular (ring-shaped) UV chart
// without moving either boundary loop. The mesh and its initial UV layout
// are generated procedurally, written to an OBJ file, optimized with
// pmp::optimize_uv(), and written again so the two charts can be compared.
//
// Usage: uv_optimization [output-directory]
//
// The two output files are ring_uv_before.obj and ring_uv_after.obj.
// Both contain per-vertex texture coordinates (vt lines).

#include <pmp/algorithms/uv_optimization.h>
#include <pmp/surface_mesh.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using namespace pmp;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Write an OBJ file carrying the vertex property "v:tex" as per-vertex
// texture coordinates. The example meshes are compact (no garbage
// collection holes), so iterator order matches the 1-based OBJ indices.
bool write_obj_with_uv(const SurfaceMesh& mesh,
                       const std::filesystem::path& file)
{
    auto points = mesh.get_vertex_property<Point>("v:point");
    auto tex = mesh.get_vertex_property<TexCoord>("v:tex");
    if (!points || !tex)
        return false;

    std::ofstream out(file);
    if (!out)
        return false;

    out.precision(10);
    for (auto v : mesh.vertices())
    {
        const Point& p = points[v];
        out << "v " << p[0] << ' ' << p[1] << ' ' << p[2] << '\n';
    }
    for (auto v : mesh.vertices())
    {
        const TexCoord& t = tex[v];
        out << "vt " << t[0] << ' ' << t[1] << '\n';
    }
    for (auto f : mesh.faces())
    {
        out << "f";
        for (auto v : mesh.vertices(f))
        {
            const auto idx = static_cast<unsigned long>(v.idx() + 1);
            out << ' ' << idx << '/' << idx;
        }
        out << '\n';
    }
    return static_cast<bool>(out);
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path out_dir =
        (argc > 1) ? std::filesystem::path(argv[1])
                   : std::filesystem::current_path();
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    // Build a planar annular triangle grid: three concentric loops of
    // sixteen vertices. Both the outer loop (ring 0) and the inner hole
    // loop (ring 2) are boundary vertices and therefore stay fixed.
    constexpr int kRings = 3;
    constexpr int kSegments = 16;
    SurfaceMesh mesh;
    std::vector<std::vector<Vertex>> v(
        kRings, std::vector<Vertex>(kSegments));
    for (int r = 0; r < kRings; ++r)
    {
        for (int i = 0; i < kSegments; ++i)
        {
            const double angle = 2.0 * kPi * i / kSegments;
            const double radius = 3.0 - static_cast<double>(r);
            v[r][i] = mesh.add_vertex(
                Point(radius * std::cos(angle), radius * std::sin(angle),
                      0.0));
        }
    }
    for (int r = 0; r + 1 < kRings; ++r)
    {
        for (int i = 0; i < kSegments; ++i)
        {
            const int j = (i + 1) % kSegments;
            mesh.add_triangle(v[r][i], v[r][j], v[r + 1][j]);
            mesh.add_triangle(v[r][i], v[r + 1][j], v[r + 1][i]);
        }
    }

    // Start from an existing, deliberately distorted but still positive
    // chart. Boundary loops keep their exact XY positions; only the free
    // inner ring is jittered.
    auto tex = mesh.vertex_property<TexCoord>("v:tex");
    for (int r = 0; r < kRings; ++r)
    {
        for (int i = 0; i < kSegments; ++i)
        {
            const double angle = 2.0 * kPi * i / kSegments;
            const double radius = 3.0 - static_cast<double>(r);
            const double jitter =
                (r == 1) ? 0.35 * std::sin(3.0 * angle) : 0.0;
            tex[v[r][i]] = TexCoord(radius * std::cos(angle) + jitter,
                                    radius * std::sin(angle));
        }
    }

    const auto before = out_dir / "ring_uv_before.obj";
    const auto after = out_dir / "ring_uv_after.obj";
    if (!write_obj_with_uv(mesh, before))
    {
        std::cerr << "Failed to write " << before << '\n';
        return 1;
    }

    UvOptimizationResult result;
    try
    {
        result = optimize_uv(mesh, 100, 1e-9);
    }
    catch (const std::exception& e)
    {
        std::cerr << "UV optimization failed: " << e.what() << '\n';
        return 1;
    }

    if (!write_obj_with_uv(mesh, after))
    {
        std::cerr << "Failed to write " << after << '\n';
        return 1;
    }

    std::cout.precision(17);
    std::cout << "accepted steps : " << result.accepted_steps << '\n'
              << "converged      : " << (result.converged ? "yes" : "no")
              << '\n'
              << "gradient norm  : " << result.gradient_norm << '\n'
              << "energy before  : " << result.energies.front() << '\n'
              << "energy after   : " << result.energies.back() << '\n'
              << "wrote " << before << '\n'
              << "wrote " << after << '\n';
    return 0;
}
