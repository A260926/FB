// Windowless C++ example: voxel sculpting with Geometry::HGrid<float> and
// sparse isosurface extraction with HMesh::volume_polygonize.
//
// Build (from the repository root, after building the GEL static library):
//
//   c++ -std=c++20 -I src examples/hgrid_surface.cpp path/to/libgel.a -o hgrid_surface
//
// Run:
//
//   ./hgrid_surface [output.obj]
//
// The program needs no window system, graphics library or display. It fills a
// solid box with HGrid::fill_box, carves a smaller box and a couple of single
// voxels with HGrid::store, extracts the zero isosurface through the sparse
// overload of volume_polygonize, and optionally writes the result as OBJ.

#include <cmath>
#include <cstdio>
#include <string>

#include <GEL/CGLA/Vec.h>
#include <GEL/Geometry/HGrid.h>
#include <GEL/Geometry/XForm.h>
#include <GEL/HMesh/Manifold.h>
#include <GEL/HMesh/obj_save.h>
#include <GEL/HMesh/polygonize.h>

int main(int argc, char** argv)
{
    using namespace CGLA;
    using namespace Geometry;
    using namespace HMesh;

    // A 40 x 40 x 40 voxel volume. The last brick on each axis is partial,
    // since 40 is not a multiple of the 8-voxel brick size.
    const Vec3i dims(40, 40, 40);

    // The grid starts out uniformly "outside" (negative values). The default
    // value is what clear() restores, and it is used for every untouched
    // voxel and every merged brick.
    HGrid<float> volume(dims, -1.0f);

    // Sculpt a solid block: a half-open voxel box [lo, hi), clipped to the
    // grid. Fully covered bricks are merged immediately instead of being
    // expanded to individual voxels.
    volume.fill_box(Vec3i(8, 8, 8), Vec3i(32, 32, 32), 1.0f);

    // Carve a smaller cavity back to "outside". Only bricks straddling the
    // cavity boundary are split and written voxel by voxel; afterwards each
    // affected brick is merged again if it became uniform.
    volume.fill_box(Vec3i(18, 18, 18), Vec3i(22, 22, 22), -1.0f);

    // Single point edits continue to work on top of region edits.
    volume.store(Vec3i(10, 20, 20), 1.0f);
    volume.store(Vec3i(31, 31, 31), -1.0f);

    // The isocontour is taken at value tau = 0 with high values inside.
    const float tau = 0.0f;

    // Maps voxel indices to a world-space box of roughly [-1, 1]^3. Pass
    // XForm(dims) instead for identity voxel coordinates.
    const XForm xform(Vec3d(-1.0), Vec3d(1.0), dims, 0.0);

    SparsePolygonizeStats stats;
    Manifold surface = volume_polygonize(xform, volume,
                                         tau,
                                         /*make_triangles=*/true,
                                         /*high_is_inside=*/true,
                                         /*dual_connectivity=*/false,
                                         &stats);

    std::printf("vertices: %zu\n", surface.no_vertices());
    std::printf("faces:    %zu\n", surface.no_faces());
    std::printf("valid:    %s\n", valid(surface) ? "yes" : "no");
    std::printf("visited voxels: %zu\n", stats.visited_voxels);
    std::printf("sample reads:   %zu\n", stats.sample_reads);

    if (argc > 1) {
        const std::string path = argv[1];
        if (obj_save(path, surface))
            std::printf("wrote %s\n", path.c_str());
        else {
            std::fprintf(stderr, "could not write %s\n", path.c_str());
            return 1;
        }
    }

    return 0;
}
