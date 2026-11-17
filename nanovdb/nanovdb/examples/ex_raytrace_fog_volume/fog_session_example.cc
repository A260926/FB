// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/// @file fog_session_example.cc
///
/// @brief Windowless CPU example of compositing several sparse NanoVDB fog
///        volumes through nanovdb::tools::FogSession.
///
/// The example builds three independently transformed fog volumes, fires a
/// small bundle of rays through them, and advances every session in bounded
/// slices to mimic a renderer that must yield the thread regularly.  It has no
/// window, no CUDA and no OpenVDB dependency; the produced pixels are printed
/// to stdout.
///
/// Build (from a checkout that has the nanovdb/ include directory on the path):
/// @code
///     c++ -std=c++17 -I nanovdb fog_session_example.cc \
///         nanovdb/nanovdb/tools/FogSession.cc -o fog_session_example
///     ./fog_session_example
/// @endcode

#include <nanovdb/HostBuffer.h>
#include <nanovdb/NanoVDB.h>
#include <nanovdb/math/Ray.h>
#include <nanovdb/tools/CreateNanoGrid.h>
#include <nanovdb/tools/CreatePrimitives.h>
#include <nanovdb/tools/FogSession.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using Handle = nanovdb::GridHandle<nanovdb::HostBuffer>;
using Vec3   = nanovdb::Vec3d;

/// @brief Build a constant-density cube out of active 8^3 constant tiles.
///
/// Every tile is created inactive first so that the builder does not seed the
/// surrounding hierarchy with the tile value; only the requested regions are
/// then activated.  Each tile is a single constant hierarchy span, so the
/// whole box is traversed with a handful of span steps instead of one step per
/// voxel.  Together with a positive uniform scale and a translation this is
/// exactly the class of mappings FogSession supports.
Handle makeBox(const Vec3& center, double halfWidth, double voxelSize, float density,
               const Vec3& translation)
{
    using BuildGrid = nanovdb::tools::build::FloatGrid;
    BuildGrid grid(0.0f);
    const int lo[3] = {int(std::floor((center[0] - halfWidth) / voxelSize)),
                       int(std::floor((center[1] - halfWidth) / voxelSize)),
                       int(std::floor((center[2] - halfWidth) / voxelSize))};
    const int hi[3] = {int(std::ceil((center[0] + halfWidth) / voxelSize)),
                       int(std::ceil((center[1] + halfWidth) / voxelSize)),
                       int(std::ceil((center[2] + halfWidth) / voxelSize))};
    // 8^3 voxel tiles, aligned to the leaf-level grid.
    for (int x = lo[0]; x < hi[0]; x += 8)
        for (int y = lo[1]; y < hi[1]; y += 8)
            for (int z = lo[2]; z < hi[2]; z += 8) {
                const nanovdb::Coord origin(x & ~7, y & ~7, z & ~7);
                grid.tree().root().addTile<1>(origin, 0.0f, false);
                grid.tree().root().addTile<1>(origin, density, true);
            }
    grid.setTransform(voxelSize, translation);
    return nanovdb::tools::createNanoGrid(grid,
                                          nanovdb::tools::StatsMode::BBox,
                                          nanovdb::CheckMode::Disable);
}

} // namespace

int main()
{
    // Three overlapping volumes with different mappings, extinctions and
    // emission colours.  Every grid must stay alive and unmodified while a
    // session refers to it.
    Handle red   = makeBox(Vec3(0.0, 0.0, 0.0), 4.0, 1.0, 0.8f, Vec3(0.0));
    Handle green = makeBox(Vec3(2.0, 1.0, 0.0), 3.5, 1.0, 0.6f, Vec3(1.0, -1.0, 0.0));
    Handle blue  = makeBox(Vec3(1.0, -1.0, 1.0), 3.0, 0.5, 0.7f, Vec3(-1.0, 0.5, 1.5));

    std::vector<nanovdb::tools::FogVolume> volumes{
        {red.grid<float>(), 0.35, Vec3(0.9, 0.1, 0.05)},
        {green.grid<float>(), 0.25, Vec3(0.05, 0.8, 0.1)},
        {blue.grid<float>(), 0.45, Vec3(0.05, 0.1, 0.9)},
    };

    const Vec3 background(0.02, 0.02, 0.03);
    const int  width = 4;
    const int  height = 3;
    const double budget = 2; // hierarchy span steps per yielded slice

    std::printf("ray grid (radiance rgb, transmittance)\n");
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            // Rays fan across the overlap region of all three volumes.  The
            // three grids use different scales and translations, so the same
            // world-space ray sees different index-space paths in each.
            const double y = -4.0 + 4.0 * double(row);
            const double z = -4.0 + 2.5 * double(col);
            const Vec3 eye(-16.0, y, z);
            // Non-unit direction: FogSession must still measure the true
            // world-space path length.
            const Vec3 dir(8.0, -0.05 * double(row), 0.08 * double(col - 1));
            const nanovdb::math::Ray<double> ray(eye, dir, 0.0, 6.0);

            nanovdb::tools::FogSession session(volumes, ray, background);
            // Yield after a bounded amount of work, as a render thread would.
            while (!session.advance(std::size_t(budget))) {
            }
            const nanovdb::tools::FogResult result = session.result();
            std::printf("(%d,%d) rgb=(%.4f %.4f %.4f) T=%.4f spans=%zu\n",
                        col, row, result.radiance[0], result.radiance[1], result.radiance[2],
                        result.transmittance, session.spanSteps());
        }
    }
    return 0;
}
