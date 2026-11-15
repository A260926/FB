#pragma once

#include <libnurbs/Core/Typedefs.hpp>
#include <array>
#include <vector>

namespace libnurbs
{
    class Surface;

    /**
     * @brief Triangulated surface mesh in the surface's original parameter space.
     *
     * - positions: unique mesh vertices in 3D.
     * - triangles: triples of indices into `positions`, counter-clockwise in UV.
     * - corner_uv: original UV coordinate for every triangle corner, stored three
     *   entries per triangle (triangle i -> entries 3i, 3i+1, 3i+2).
     * - corner_normals: analytic unit normal on the interior side of the knot
     *   cell each corner belongs to, three entries per triangle.
     *
     * A corner shared between two triangles retains the same position but keeps
     * its own normal, so a C0 crease has distinct normals on each side.
     */
    struct SurfaceMesh
    {
        std::vector<Vec3> positions;
        std::vector<std::array<int, 3>> triangles;
        std::vector<Vec2> corner_uv;
        std::vector<Vec3> corner_normals;
    };

    /**
     * @brief Build a triangulated mesh of `surface` over the supplied original-domain sites.
     *
     * @param surface Surface to tessellate.
     * @param us Strictly increasing U sites, including both domain endpoints.
     *            All distinct interior U knots are merged into the grid.
     * @param vs Strictly increasing V sites, including both domain endpoints.
     *            All distinct interior V knots are merged into the grid.
     *
     * @throws std::invalid_argument for non-increasing or out-of-domain sites.
     * @throws std::runtime_error when an analytic corner normal is undefined.
     */
    SurfaceMesh BuildSurfaceMesh(const Surface& surface,
                                 const std::vector<double>& us,
                                 const std::vector<double>& vs);
}
