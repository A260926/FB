#pragma once

#include <array>
#include <vector>
#include <libnurbs/Core/Typedefs.hpp>

namespace libnurbs
{
    class Surface;

    /**
     * @brief A triangle mesh sampled from a Surface on a grid of original-domain
     *        parameter stations.
     *
     * positions are shared by parameter identity: all triangles touching the same
     * (u, v) station reference the same position, even across C0 creases.
     * triangles, corner_uv and corner_normals are per-triangle: entry t describes
     * triangle t, and corner_uv[t][k] / corner_normals[t][k] belong to the corner
     * referencing positions[triangles[t][k]]. corner_uv stores the original-domain
     * (u, v) of the corner; corner_normals stores the analytic unit normal taken
     * from the inside of the knot cell the triangle was sampled from, so the two
     * sides of a C0 crease keep their own one-sided normals.
     */
    struct SurfaceMesh
    {
        using UV = Eigen::Vector<Numeric, 2>;

        std::vector<Vec3> positions{};
        std::vector<std::array<int, 3>> triangles{};
        std::vector<std::array<UV, 3>> corner_uv{};
        std::vector<std::array<Vec3, 3>> corner_normals{};
    };

    /**
     * @brief Build a triangle mesh of a surface over a station grid.
     * @param surface The surface to sample (evaluated in its original parameter domain).
     * @param us Strictly increasing original-domain u stations; must contain both
     *           domain endpoints. All distinct internal u knots are merged in.
     * @param vs Strictly increasing original-domain v stations; must contain both
     *           domain endpoints. All distinct internal v knots are merged in.
     * @throws std::invalid_argument if the stations are illegal (surface and
     *         stations are left unchanged).
     * @throws std::runtime_error if a one-sided normal is degenerate (surface and
     *         stations are left unchanged).
     */
    SurfaceMesh BuildSurfaceMesh(const Surface& surface,
                                 const std::vector<double>& us,
                                 const std::vector<double>& vs);
}
