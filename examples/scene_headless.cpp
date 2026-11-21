//
// scene_headless.cpp
//
// Minimal offline demo of VoxelScene: a two-triangle mesh built around a
// shared vertex is deformed and recolored incrementally.  The point clouds
// before and after the edit are written to <output dir>/before.xyz and
// <output dir>/after.xyz (one "x y z [r g b]" line per occupied cell).
//

#include "voxelizer.h"
#include "voxel_scene.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace voxel_scene;

namespace {

void writeCloud(std::string const& path, vx_point_cloud_t const* pc, bool colored)
{
    std::FILE* file = std::fopen(path.c_str(), "w");
    if (!file) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    for (std::size_t i = 0; i < pc->nvertices; ++i) {
        vx_vertex_t const& v = pc->vertices[i];
        if (colored) {
            vx_color_t const& c = pc->colors[i];
            std::fprintf(file, "%f %f %f %f %f %f\n",
                v.x, v.y, v.z, c.r, c.g, c.b);
        } else {
            std::fprintf(file, "%f %f %f\n", v.x, v.y, v.z);
        }
    }
    std::fclose(file);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output directory>\n", argv[0]);
        return 1;
    }
    std::string const outdir = argv[1];

    // Two triangles sharing vertex 0 and edge (0,2).
    const unsigned nvertices = 4;
    const unsigned nindices = 6;
    vx_mesh_t* mesh = vx_mesh_alloc((int) nvertices, (int) nindices);
    mesh->indices[0] = 0; mesh->indices[1] = 1; mesh->indices[2] = 2;
    mesh->indices[3] = 0; mesh->indices[4] = 2; mesh->indices[5] = 3;

    auto put = [&](unsigned i, float x, float y, float z,
                   float r, float g, float b) {
        mesh->vertices[i].x = x; mesh->vertices[i].y = y; mesh->vertices[i].z = z;
        mesh->colors[i].r = r; mesh->colors[i].g = g; mesh->colors[i].b = b;
    };
    put(0, 0.05f, 0.05f, 0.10f, 1.f, 0.f, 0.f);
    put(1, 0.45f, 0.05f, 0.10f, 1.f, 0.f, 0.f);
    put(2, 0.05f, 0.45f, 0.10f, 0.f, 1.f, 0.f);
    put(3, 0.45f, 0.45f, 0.10f, 0.f, 0.f, 1.f);

    const float step = 0.125f;
    VoxelScene scene(mesh, step, step, step, step * 0.1f);

    vx_point_cloud_t* before = scene.pointCloud();
    writeCloud(outdir + "/before.xyz", before, true);
    vx_point_cloud_free(before);

    // First batch: deform the shared vertex. Both incident triangles are
    // re-rasterized exactly once each.
    std::vector<VertexUpdate> deform;
    deform.push_back(VertexUpdate{ 0,
        vx_vertex_t{{{ 0.30f, 0.28f, 0.35f }}},
        mesh->colors[0] });
    VoxelPatch first = scene.apply(deform);
    std::printf("deform: %zu upsert, %zu remove, %zu geometry triangles, "
                "%llu candidate tests\n",
        first.upsert.size(), first.remove.size(),
        first.work.triangleIds.size(), first.work.candidateTests);

    // Second batch: recolor a non-shared vertex without moving anything.
    // Persistent weights are reused, so no triangle/box test runs.
    std::vector<VertexUpdate> recolor;
    recolor.push_back(VertexUpdate{ 1,
        mesh->vertices[1],
        vx_color_t{{{ 0.2f, 0.9f, 0.4f }}} });
    VoxelPatch second = scene.apply(recolor);
    std::printf("recolor: %zu upsert, %zu remove, %zu color triangles, "
                "%llu candidate tests\n",
        second.upsert.size(), second.remove.size(),
        second.work.colorTriangleIds.size(), second.work.candidateTests);

    vx_point_cloud_t* after = scene.pointCloud();
    writeCloud(outdir + "/after.xyz", after, true);
    vx_point_cloud_free(after);

    vx_mesh_free(mesh);
    return 0;
}
