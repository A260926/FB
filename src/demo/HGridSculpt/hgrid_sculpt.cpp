//
//  hgrid_sculpt.cpp
//
//  Windowless example: sculpt a solid block in a sparse HGrid with
//  HGrid::fill_box (plus a few single voxel edits) and turn the sparse data
//  directly into a renderable surface with the HGrid overload of
//  HMesh::volume_polygonize - without ever densifying the grid.
//
//  Usage: hgrid_sculpt [output.obj]
//
//  If an output path is given, the resulting mesh is saved as OBJ;
//  otherwise only statistics are printed.

#include <cstdio>
#include <iostream>

#include <GEL/CGLA/Vec3i.h>
#include <GEL/Geometry/HGrid.h>
#include <GEL/Geometry/XForm.h>
#include <GEL/HMesh/Manifold.h>
#include <GEL/HMesh/polygonize.h>
#include <GEL/HMesh/obj_save.h>

using namespace CGLA;
using namespace Geometry;
using namespace HMesh;

int main(int argc, char** argv)
{
    // A 96^3 voxel domain. Everything outside the sculpted region keeps the
    // default value -1 (exterior), stored as one value per 8^3 brick.
    const Vec3i dims(96, 96, 96);
    HGrid<float> grid(dims, -1.0f);

    // Region edits: fill a solid block, carve a smaller box out of it and
    // poke a single voxel hole. The bricks covered by each box are merged
    // into single values, so the edits stay cheap in memory.
    grid.fill_box(Vec3i(16, 16, 16), Vec3i(80, 80, 80), 1.0f);   // solid block
    grid.fill_box(Vec3i(40, 40, 16), Vec3i(56, 56, 80), -1.0f);  // carve a shaft
    grid.store(Vec3i(20, 50, 50), -1.0f);                        // single voxel edit

    // Map voxel indices to object coordinates (here: unit cube spacing with
    // a uniform scale of 0.02 and no offset).
    XForm xform(dims, 0.02);

    // Polygonize the sparse representation directly. tau=0 separates the
    // filled (1) region from the default (-1) region; stats reports how much
    // work the sparse traversal actually did.
    SparsePolygonizeStats stats;
    Manifold mesh = volume_polygonize(xform, grid, 0.0f, true, true, false, &stats);

    std::printf("sculpted surface: %zu vertices, %zu faces\n",
                mesh.no_vertices(), mesh.no_faces());
    std::printf("sparse traversal: %zu voxels visited, %zu scalar samples read\n",
                stats.visited_voxels, stats.sample_reads);
    std::printf("(a dense 96^3 scan would visit %d voxels)\n", 96*96*96);

    if (argc > 1) {
        if (!obj_save(argv[1], mesh)) {
            std::cerr << "failed to save " << argv[1] << "\n";
            return 1;
        }
        std::printf("saved %s\n", argv[1]);
    }
    return 0;
}
