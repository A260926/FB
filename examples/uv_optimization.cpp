// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

// Example: reduce the interior stretch of an existing annular UV chart
// without moving either boundary loop. The mesh and its UVs are written
// to OBJ before and after the optimization.

#include <pmp/surface_mesh.h>
#include <pmp/algorithms/uv_optimization.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>

using namespace pmp;

namespace {

// Minimal OBJ writer that embeds the per-vertex "v:tex" coordinates as
// "vt" lines and references them from every face corner.
void write_obj_with_uv(const SurfaceMesh& mesh,
                       const std::filesystem::path& file)
{
    std::ofstream out(file);
    if (!out)
        throw IOException("Failed to open file: " + file.string());

    auto points = mesh.get_vertex_property<Point>("v:point");
    auto tex = mesh.get_vertex_property<TexCoord>("v:tex");

    for (auto v : mesh.vertices())
    {
        const Point& p = points[v];
        out << "v " << p[0] << ' ' << p[1] << ' ' << p[2] << '\n';
    }
    for (auto v : mesh.vertices())
        out << "vt " << tex[v][0] << ' ' << tex[v][1] << '\n';

    for (auto f : mesh.faces())
    {
        out << 'f';
        for (auto v : mesh.vertices(f))
        {
            const auto index = static_cast<unsigned long>(v.idx() + 1);
            out << ' ' << index << '/' << index;
        }
        out << '\n';
    }
}

} // namespace

int main()
{
    // Three concentric loops (two boundary loops, one free interior
    // loop) lifted out of the plane so the metric is genuinely 3D.
    constexpr int loops = 3;
    constexpr int segments = 24;
    SurfaceMesh mesh;
    std::vector<std::vector<Vertex>> v(
        loops, std::vector<Vertex>(segments));

    for (int r = 0; r < loops; ++r)
        for (int i = 0; i < segments; ++i)
        {
            const double angle = 2.0 * std::numbers::pi * i / segments;
            const double radius = 3.0 - r;
            const double z = 0.4 * std::sin(2.0 * angle) + 0.2 * r;
            v[r][i] = mesh.add_vertex(Point(radius * std::cos(angle),
                                             radius * std::sin(angle), z));
        }

    for (int r = 0; r < loops - 1; ++r)
        for (int i = 0; i < segments; ++i)
        {
            const int j = (i + 1) % segments;
            mesh.add_triangle(v[r][i], v[r][j], v[r + 1][j]);
            mesh.add_triangle(v[r][i], v[r + 1][j], v[r + 1][i]);
        }

    auto tex = mesh.vertex_property<TexCoord>("v:tex");

    // Existing, deliberately stretched chart. A radial map with the
    // interior ring squashed toward the inner boundary stays orientation
    // preserving but stretches the middle band of triangles.
    constexpr double chart_radius[loops] = {3.0, 1.25, 1.0};
    for (int r = 0; r < loops; ++r)
        for (int i = 0; i < segments; ++i)
        {
            const double angle = 2.0 * std::numbers::pi * i / segments;
            tex[v[r][i]] = TexCoord(chart_radius[r] * std::cos(angle),
                                    chart_radius[r] * std::sin(angle));
        }

    write_obj_with_uv(mesh, "uv_before.obj");
    std::cout << "wrote uv_before.obj\n";

    const UvOptimizationResult result = optimize_uv(mesh, 200, 1e-8);

    write_obj_with_uv(mesh, "uv_after.obj");
    std::cout << "wrote uv_after.obj\n";

    std::cout << "accepted steps: " << result.accepted_steps
              << "\nconverged: " << (result.converged ? "yes" : "no")
              << "\nenergy before: " << result.energies.front()
              << "\nenergy after:  " << result.energies.back()
              << "\ngradient norm: " << result.gradient_norm << '\n';
    return 0;
}
