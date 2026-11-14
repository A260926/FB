/* Goxel 3D voxels editor
 *
 * copyright (c) 2017 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "goxel.h"
#include "utils/color.h"

#include "../ext_src/meshoptimizer/meshoptimizer.h"

static const int N = BLOCK_SIZE;

// Return the next power of 2 larger or equal to x.
static int next_pow2(int x)
{
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x++;
    return x;
}


// Implemented in marchingcube.c
int volume_generate_vertices_mc(const volume_t *volume, const int block_pos[3],
                              int effects, voxel_vertex_t *out,
                              int *size, int *pos_scale);

// Implemented in volume_samples.c: density driven cubic faces with
// separate appearance RGBA (including linear alpha).
int volume_generate_vertices_samples(const volume_t *volume,
                              const int block_pos[3], int effects,
                              voxel_vertex_t *out, int *size,
                              int *pos_scale);

int volume_generate_vertices(const volume_t *volume, const int block_pos[3],
                           int effects, voxel_vertex_t *out,
                           int *size, int *subdivide)
{
    if (effects & EFFECT_MARCHING_CUBES)
        return volume_generate_vertices_mc(volume, block_pos, effects, out,
                                         size, subdivide);

    return volume_generate_vertices_samples(volume, block_pos, effects, out,
                                            size, subdivide);
}

static void fill_mesh(volume_mesh_t *mesh,
                      const voxel_vertex_t *verts, int nb, int size,
                      int subdivide, const int bpos[3],
                      const palette_t *palette)
{
    int i, c, idx, s = 0;
    float normal[3];
    float color[4];

    // The palette texture size.
    if (palette != NULL)
        s = max(next_pow2(ceil(log2(palette->size))), 16);

    // Fill up the vertices.
    mesh->vertices = realloc(
            mesh->vertices, (mesh->vertices_count + nb * size) *
            sizeof(*mesh->vertices));
    memset(mesh->vertices + mesh->vertices_count, 0,
           nb * size * sizeof(*mesh->vertices));
    for (i = 0; i < nb * size; i++) {
        idx = mesh->vertices_count + i;
        normal[0] = verts[i].normal[0];
        normal[1] = verts[i].normal[1];
        normal[2] = verts[i].normal[2];
        vec3_normalize(normal, normal);
        mesh->vertices[idx] = (typeof(*mesh->vertices)) {
            .pos = {
                (float)verts[i].pos[0] / subdivide + bpos[0],
                (float)verts[i].pos[1] / subdivide + bpos[1],
                (float)verts[i].pos[2] / subdivide + bpos[2]},
            .normal = {normal[0], normal[1], normal[2]},
        };
        if (palette == NULL) {
            srgba8_to_rgba(verts[i].color, color);
            mesh->vertices[idx].color[0] = color[0];
            mesh->vertices[idx].color[1] = color[1];
            mesh->vertices[idx].color[2] = color[2];
            mesh->vertices[idx].color[3] = color[3];
        } else {
            c = palette_search(palette, verts[i].color, true);
            assert(c != -1);
            mesh->vertices[idx].texcoord[0] = (c % s + 0.5) / s;
            mesh->vertices[idx].texcoord[1] = (c / s + 0.5) / s;
        }
    }

    // Add the indices.
    if (size == 4) {
        mesh->indices = realloc(
                mesh->indices, (mesh->indices_count + 6 * nb) *
                sizeof(*mesh->indices));
        for (i = 0; i < nb * 6; i++) {
            mesh->indices[mesh->indices_count + i] = mesh->vertices_count +
                (i / 6) * 4 + ((int[]){0, 1, 2, 2, 3, 0})[i % 6];
        }
        mesh->indices_count += nb * 6;
    } else {
        mesh->indices = realloc(
                mesh->indices, (mesh->indices_count + 3 * nb) *
                sizeof(*mesh->indices));
        for (i = 0; i < nb * 3; i++) {
            mesh->indices[mesh->indices_count + i] = mesh->vertices_count + i;
        }
        mesh->indices_count += 3 * nb;
    }

    mesh->vertices_count += nb * size;
}

static void optimize_mesh(volume_mesh_t *mesh, float simplify)
{
    unsigned int *remap;
    unsigned int *tmp_indices;
    typeof(*mesh->vertices) *tmp_vertices;
    size_t vertices_count;
    size_t indices_count;
    int target_index_count;
	float target_error = 1e-2f;

    // Merge duplicated vertices.
    remap = calloc(mesh->vertices_count, sizeof(unsigned int));
    vertices_count = meshopt_generateVertexRemap(
            remap, mesh->indices, mesh->indices_count, mesh->vertices,
            mesh->vertices_count, sizeof(*mesh->vertices));

    tmp_indices = malloc(mesh->indices_count * sizeof(*tmp_indices));
    meshopt_remapIndexBuffer(
            tmp_indices, mesh->indices, mesh->indices_count, remap);

    tmp_vertices = malloc(vertices_count * sizeof(*mesh->vertices));
    meshopt_remapVertexBuffer(
            tmp_vertices, mesh->vertices, mesh->vertices_count,
            sizeof(*mesh->vertices), remap);

    free(remap);
    SWAP(mesh->vertices, tmp_vertices);
    SWAP(mesh->indices, tmp_indices);
    mesh->vertices_count = vertices_count;

    // Also simplify the mesh if required.
	target_index_count = (int)(mesh->indices_count * (1 - simplify));
    target_index_count = fmax(target_index_count, 1);
    if (target_index_count < mesh->indices_count) {
        indices_count = meshopt_simplify(
                tmp_indices, mesh->indices, mesh->indices_count,
                (const float*)mesh->vertices, mesh->vertices_count,
                sizeof(*mesh->vertices), target_index_count, target_error,
                0, NULL);
        vertices_count = meshopt_optimizeVertexFetch(
                tmp_vertices, tmp_indices, indices_count,
                mesh->vertices, mesh->vertices_count, sizeof(*mesh->vertices));
        SWAP(mesh->vertices, tmp_vertices);
        SWAP(mesh->indices, tmp_indices);
        mesh->vertices_count = vertices_count;
        mesh->indices_count = indices_count;
    }

    free(tmp_vertices);
    free(tmp_indices);
}

volume_mesh_t *volume_generate_mesh(
        const volume_t *volume, int effects, const palette_t *palette,
        float simplify)
{
    volume_iterator_t iter;
    int bpos[3];
    voxel_vertex_t *verts;
    int i, nb, size, subdivide;
    volume_mesh_t *mesh = calloc(1, sizeof(*mesh));

    verts = calloc(N * N * N * 6 * 4, sizeof(*verts));
    iter = volume_get_iterator(volume,
            VOLUME_ITER_TILES | VOLUME_ITER_INCLUDES_NEIGHBORS);
    while (volume_iter(&iter, bpos)) {
        nb = volume_generate_vertices(volume, bpos, effects, verts,
                                      &size, &subdivide);
        if (nb == 0) continue;
        fill_mesh(mesh, verts, nb, size, subdivide, bpos, palette);
    }
    free(verts);

    optimize_mesh(mesh, simplify);

    mesh->pos_min[0] = +FLT_MAX;
    mesh->pos_min[1] = +FLT_MAX;
    mesh->pos_min[2] = +FLT_MAX;
    mesh->pos_max[0] = -FLT_MAX;
    mesh->pos_max[1] = -FLT_MAX;
    mesh->pos_max[2] = -FLT_MAX;
    for (i = 0; i < mesh->vertices_count; i++) {
        mesh->pos_min[0] = min(mesh->vertices[i].pos[0], mesh->pos_min[0]);
        mesh->pos_min[1] = min(mesh->vertices[i].pos[1], mesh->pos_min[1]);
        mesh->pos_min[2] = min(mesh->vertices[i].pos[2], mesh->pos_min[2]);
        mesh->pos_max[0] = max(mesh->vertices[i].pos[0], mesh->pos_max[0]);
        mesh->pos_max[1] = max(mesh->vertices[i].pos[1], mesh->pos_max[1]);
        mesh->pos_max[2] = max(mesh->vertices[i].pos[2], mesh->pos_max[2]);
    }

    return mesh;
}

void volume_mesh_free(volume_mesh_t *mesh)
{
    free(mesh->vertices);
    free(mesh->indices);
    free(mesh);
}
