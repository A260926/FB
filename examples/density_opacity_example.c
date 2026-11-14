/*
 * Minimal example: separate density and surface opacity.
 *
 * A voxel can now be physically solid (density 255) while its surface is
 * fully transparent (alpha 0).  The shape is driven by the density, so the
 * transparent voxel still produces geometry, and changing only the alpha
 * never changes the generated topology.
 *
 * Build (headless, from the repository root).  First build the production
 * library with task_support/build.sh.  The bundled meshoptimizer is C++, so
 * the final link step uses the C++ compiler:
 *   bash task_support/build.sh
 *   cc -std=gnu11 -I. -Isrc -Iext_src/uthash \
 *      -include task_support/headless.h -c \
 *      examples/density_opacity_example.c -o example.o
 *   c++ example.o .task-build/production.a -lm -o density_opacity_example
 */

#include <stdio.h>

int main(void)
{
    volume_t *volume = volume_new();
    const int pos[3] = {4, 4, 4};

    // Solid and fully transparent: geometry exists, alpha is 0.
    volume_sample_t transparent_solid = {
        .rgba = {255, 0, 0, 0},
        .density = 255,
    };
    volume_set_sample_at(volume, NULL, pos, &transparent_solid);

    volume_sample_t got;
    volume_get_sample_at(volume, NULL, pos, &got);
    printf("density=%u alpha=%u\n", got.density, got.rgba[3]);

    // Cubic faces: six quads even though the surface alpha is zero.
    voxel_vertex_t verts[16 * 16 * 16 * 6 * 4];
    int size, subdivide;
    const int block[3] = {0, 0, 0};
    int faces = volume_generate_vertices(volume, block, 0, verts,
                                         &size, &subdivide);
    printf("cubic faces=%d vertex_alpha=%u\n", faces, verts[0].color[3]);

    // Make it half transparent without touching the density: same shape.
    transparent_solid.rgba[3] = 128;
    volume_set_sample_at(volume, NULL, pos, &transparent_solid);

    // Whole-volume mesh export keeps the linear alpha (palette NULL).
    volume_mesh_t *mesh = volume_generate_mesh(volume, 0, NULL, 0);
    printf("mesh indices=%d linear_alpha=%.3f\n",
           mesh->indices_count, mesh->vertices[0].color[3]);
    volume_mesh_free(mesh);

    // A zero density voxel is empty no matter what color it carries.
    volume_sample_t colored_empty = {
        .rgba = {0, 255, 0, 255},
        .density = 0,
    };
    volume_set_sample_at(volume, NULL, pos, &colored_empty);
    // Zero density drops the tile during the usual empty tile cleanup.
    volume_remove_empty_tiles(volume, false);
    printf("empty volume? %s\n", volume_is_empty(volume) ? "yes" : "no");

    volume_delete(volume);
    return 0;
}
