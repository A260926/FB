/* Goxel 3D voxels editor
 *
 * Tests for the separated density / opacity volume samples and the
 * density driven cubic and marching cubes surface generation.
 */

#include "task_support/check.h"

#define BLOCK 16
#define BLOCK_VERTS (BLOCK * BLOCK * BLOCK * 6 * 4)

static void set_sample(volume_t *v, int x, int y, int z,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                       uint8_t density)
{
    const int pos[3] = {x, y, z};
    volume_sample_t s = {
        .rgba = {r, g, b, a},
        .density = density,
    };
    volume_set_sample_at(v, NULL, pos, &s);
}

static volume_sample_t get_sample(volume_t *v, int x, int y, int z)
{
    const int pos[3] = {x, y, z};
    volume_sample_t s;
    volume_get_sample_at(v, NULL, pos, &s);
    return s;
}

static int sample_block_index(int lx, int ly, int lz)
{
    // Local coordinates inside the (N + 2) block, 0 based.
    return lx + ly * (BLOCK + 2) + lz * (BLOCK + 2) * (BLOCK + 2);
}

// Compare everything but the RGBA color: topology must not depend on alpha.
static int same_topology(const voxel_vertex_t *a, int na,
                         const voxel_vertex_t *b, int nb)
{
    int i;
    if (na != nb) return 0;
    for (i = 0; i < na; i++) {
        if (memcmp(a[i].pos, b[i].pos, sizeof(a[i].pos))) return 0;
        if (memcmp(a[i].normal, b[i].normal, sizeof(a[i].normal))) return 0;
    }
    return 1;
}

static int all_alpha(const voxel_vertex_t *verts, int nb, int size,
                     uint8_t alpha)
{
    int i;
    for (i = 0; i < nb * size; i++)
        if (verts[i].color[3] != alpha) return 0;
    return 1;
}

/* ------------------------------ storage ------------------------------ */

static int run_sample_roundtrip(void)
{
    volume_t *v = volume_new();
    volume_accessor_t it = volume_get_accessor(v);
    const int pos[3] = {-7, -3, -9};
    volume_sample_t s = {
        .rgba = {10, 20, 30, 40},
        .density = 200,
    };
    volume_sample_t got;
    uint8_t raw[4];

    volume_set_sample_at(v, &it, pos, &s);
    volume_get_sample_at(v, &it, pos, &got);
    TASK_CHECK(got.density == 200);
    TASK_CHECK(!memcmp(got.rgba, s.rgba, 4));

    // The legacy RGBA interface keeps the original stored bytes.
    volume_get_at(v, NULL, pos, raw);
    TASK_CHECK(!memcmp(raw, s.rgba, 4));
    TASK_CHECK(raw[3] == 40);

    // Empty position, reached through a cached accessor.
    volume_get_sample_at(v, &it, (int[]){100, 100, 100}, &got);
    TASK_CHECK(got.density == 0);
    TASK_CHECK(got.rgba[0] == 0 && got.rgba[3] == 0);

    volume_delete(v);
    return 0;
}

static int run_legacy_interpretation(void)
{
    volume_t *v = volume_new();
    const int pos[3] = {4, 4, 4};
    uint8_t rgba[4] = {1, 2, 3, 64};
    volume_sample_t got;
    uint8_t raw[4];

    volume_set_at(v, NULL, pos, rgba);
    got = get_sample(v, 4, 4, 4);
    TASK_CHECK(got.density == 64);
    TASK_CHECK(got.rgba[3] == 255);
    TASK_CHECK(got.rgba[0] == 1 && got.rgba[1] == 2 && got.rgba[2] == 3);

    // Raw storage still exposes the original alpha.
    volume_get_at(v, NULL, pos, raw);
    TASK_CHECK(raw[3] == 64);

    rgba[3] = 0;
    volume_set_at(v, NULL, pos, rgba);
    got = get_sample(v, 4, 4, 4);
    TASK_CHECK(got.density == 0);
    TASK_CHECK(got.rgba[3] == 0);

    volume_delete(v);
    return 0;
}

static int run_mixed_writes(void)
{
    volume_t *v = volume_new();
    volume_sample_t got;

    // Explicit sample: solid shape, fully transparent surface.
    set_sample(v, 1, 1, 1, 255, 0, 0, 0, 255);
    got = get_sample(v, 1, 1, 1);
    TASK_CHECK(got.density == 255 && got.rgba[3] == 0);

    // Legacy voxel next to it in the same tile.
    uint8_t legacy[4] = {9, 8, 7, 200};
    volume_set_at(v, NULL, (int[]){2, 1, 1}, legacy);
    got = get_sample(v, 2, 1, 1);
    TASK_CHECK(got.density == 200 && got.rgba[3] == 255);

    // A legacy write over an explicit voxel reverts it to the legacy rule.
    uint8_t over[4] = {5, 6, 7, 100};
    volume_set_at(v, NULL, (int[]){1, 1, 1}, over);
    got = get_sample(v, 1, 1, 1);
    TASK_CHECK(got.density == 100);
    TASK_CHECK(got.rgba[3] == 255);
    TASK_CHECK(got.rgba[0] == 5);

    // And an explicit write can take it back.
    set_sample(v, 1, 1, 1, 5, 6, 7, 42, 255);
    got = get_sample(v, 1, 1, 1);
    TASK_CHECK(got.density == 255 && got.rgba[3] == 42);

    volume_delete(v);
    return 0;
}

static int run_snapshots(void)
{
    volume_t *v = volume_new();
    volume_t *copy, *assigned;
    uint64_t key, tile_id, copy_tile_id;
    volume_sample_t got;
    const int tile_pos[3] = {0, 0, 0};
    const int pos[3] = {3, 3, 3};

    set_sample(v, 3, 3, 3, 1, 2, 3, 64, 255);
    key = volume_get_key(v);
    TASK_CHECK(volume_get_tile_data(v, NULL, tile_pos, &tile_id) != NULL);

    copy = volume_copy(v);
    // volume_set adopts the same tile set, then COW keeps it independent.
    assigned = volume_new();
    volume_set(assigned, v);

    // Change only the density: the snapshots keep their own value and key.
    set_sample(v, 3, 3, 3, 1, 2, 3, 64, 100);
    TASK_CHECK(volume_get_key(v) != key);
    TASK_CHECK(volume_get_tile_data(v, NULL, tile_pos, NULL) != NULL);
    volume_get_tile_data(copy, NULL, tile_pos, &copy_tile_id);
    TASK_CHECK(copy_tile_id == tile_id);
    volume_get_sample_at(copy, NULL, pos, &got);
    TASK_CHECK(got.density == 255 && got.rgba[3] == 64);
    volume_get_sample_at(assigned, NULL, pos, &got);
    TASK_CHECK(got.density == 255 && got.rgba[3] == 64);

    // Change only the opacity: same isolation, and the key moves again.
    key = volume_get_key(v);
    set_sample(v, 3, 3, 3, 1, 2, 3, 10, 100);
    TASK_CHECK(volume_get_key(v) != key);
    volume_get_sample_at(copy, NULL, pos, &got);
    TASK_CHECK(got.density == 255 && got.rgba[3] == 64);
    volume_get_sample_at(v, NULL, pos, &got);
    TASK_CHECK(got.density == 100 && got.rgba[3] == 10);

    // Clearing one volume must not touch the snapshots.
    volume_clear(v);
    TASK_CHECK(volume_is_empty(v));
    volume_get_sample_at(copy, NULL, pos, &got);
    TASK_CHECK(got.density == 255 && got.rgba[3] == 64);
    volume_get_sample_at(assigned, NULL, pos, &got);
    TASK_CHECK(got.density == 255 && got.rgba[3] == 64);

    volume_delete(assigned);
    volume_delete(copy);
    volume_delete(v);
    return 0;
}

static int run_tile_boundary_reads(void)
{
    volume_t *v = volume_new();
    static volume_sample_t block[(BLOCK + 2) * (BLOCK + 2) * (BLOCK + 2)];
    static uint8_t raw_block[(BLOCK + 2) * (BLOCK + 2) * (BLOCK + 2) * 4];
    const int size[3] = {BLOCK + 2, BLOCK + 2, BLOCK + 2};
    volume_sample_t got;

    // Voxels on both sides of a tile boundary, including negatives.
    set_sample(v, -1, -1, -1, 1, 0, 0, 64, 255);
    set_sample(v, 15, 15, 15, 2, 0, 0, 64, 255);
    set_sample(v, 16, 16, 16, 3, 0, 0, 64, 255);

    // Aligned block around tile (0, 0, 0).
    volume_read_samples(v, (int[]){-1, -1, -1}, size, block);
    got = block[sample_block_index(0, 0, 0)];
    TASK_CHECK(got.density == 255 && got.rgba[0] == 1 && got.rgba[3] == 64);
    got = block[sample_block_index(16, 16, 16)];
    TASK_CHECK(got.density == 255 && got.rgba[0] == 2);

    // Aligned block around tile (16, 16, 16).
    memset(block, 0, sizeof(block));
    volume_read_samples(v, (int[]){15, 15, 15}, size, block);
    got = block[sample_block_index(1, 1, 1)];
    TASK_CHECK(got.density == 255 && got.rgba[0] == 3);

    // The legacy 4 byte read keeps the original authored alpha.
    volume_read(v, (int[]){-1, -1, -1}, size, raw_block);
    TASK_CHECK(raw_block[(sample_block_index(0, 0, 0)) * 4 + 3] == 64);
    TASK_CHECK(raw_block[(sample_block_index(16, 16, 16)) * 4 + 3] == 64);

    volume_delete(v);
    return 0;
}

static int run_empty_tiles_and_bbox(void)
{
    volume_t *v = volume_new();
    int bbox[2][3];
    volume_sample_t got;

    // A zero density voxel keeps a color but must not count as geometry.
    set_sample(v, 10, 10, 10, 9, 9, 9, 255, 0);
    set_sample(v, 2, 2, 2, 1, 1, 1, 0, 255);
    TASK_CHECK(volume_get_bbox(v, bbox, true));
    TASK_CHECK(bbox[0][0] == 2 && bbox[0][1] == 2 && bbox[0][2] == 2);
    TASK_CHECK(bbox[1][0] == 3 && bbox[1][1] == 3 && bbox[1][2] == 3);

    // Remove the only dense voxel: the whole tile is dropped, including
    // the color stored on the zero density voxel.
    set_sample(v, 2, 2, 2, 1, 1, 1, 0, 0);
    volume_remove_empty_tiles(v, false);
    TASK_CHECK(volume_get_tiles_count(v) == 0);
    got = get_sample(v, 10, 10, 10);
    TASK_CHECK(got.density == 0 && got.rgba[3] == 0);
    TASK_CHECK(!volume_get_bbox(v, bbox, true));

    volume_delete(v);
    return 0;
}

static int run_copy_tile(void)
{
    volume_t *src = volume_new();
    volume_t *dst = volume_new();
    volume_sample_t got;

    set_sample(src, 3, 3, 3, 12, 34, 56, 40, 200);
    volume_copy_tile(src, (int[]){0, 0, 0}, dst, (int[]){16, 16, 16});
    got = get_sample(dst, 19, 19, 19);
    TASK_CHECK(got.density == 200);
    TASK_CHECK(got.rgba[0] == 12 && got.rgba[3] == 40);

    // The source tile is untouched by writing through the destination.
    set_sample(dst, 19, 19, 19, 0, 0, 0, 0, 0);
    got = get_sample(src, 3, 3, 3);
    TASK_CHECK(got.density == 200 && got.rgba[3] == 40);

    volume_delete(src);
    volume_delete(dst);
    return 0;
}

/* -------------------------------- cubic ------------------------------- */

static voxel_vertex_t *new_verts(void)
{
    return calloc(BLOCK_VERTS, sizeof(voxel_vertex_t));
}

static int run_cubic_alpha_independent(void)
{
    volume_t *v = volume_new();
    voxel_vertex_t *a = new_verts();
    voxel_vertex_t *b = new_verts();
    int size, sub;
    const int origin[3] = {0, 0, 0};

    // Density 255 but alpha 0 still produces the full six faces.
    set_sample(v, 4, 4, 4, 255, 0, 0, 0, 255);
    int nb0 = volume_generate_vertices(v, origin, 0, a, &size, &sub);
    TASK_CHECK(nb0 == 6 && size == 4 && sub == 1);
    TASK_CHECK(all_alpha(a, nb0, size, 0));

    // Changing only the opacity must leave the geometry untouched.
    set_sample(v, 4, 4, 4, 255, 0, 0, 128, 255);
    int nb1 = volume_generate_vertices(v, origin, 0, b, &size, &sub);
    TASK_CHECK(nb1 == nb0);
    TASK_CHECK(same_topology(a, nb0 * size, b, nb1 * size));
    TASK_CHECK(all_alpha(b, nb1, size, 128));

    free(a);
    free(b);
    volume_delete(v);
    return 0;
}

static int run_cubic_density_threshold(void)
{
    volume_t *v = volume_new();
    voxel_vertex_t *verts = new_verts();
    int size, sub;
    const int origin[3] = {0, 0, 0};

    set_sample(v, 4, 4, 4, 1, 2, 3, 255, 0);
    TASK_CHECK(volume_generate_vertices(v, origin, 0, verts,
                                        &size, &sub) == 0);

    set_sample(v, 4, 4, 4, 1, 2, 3, 255, 126);
    TASK_CHECK(volume_generate_vertices(v, origin, 0, verts,
                                        &size, &sub) == 0);

    set_sample(v, 4, 4, 4, 1, 2, 3, 255, 127);
    TASK_CHECK(volume_generate_vertices(v, origin, 0, verts,
                                        &size, &sub) == 6);

    // Visible color but zero density never creates geometry.
    set_sample(v, 4, 4, 4, 1, 2, 3, 255, 0);
    TASK_CHECK(volume_generate_vertices(v, origin, 0, verts,
                                        &size, &sub) == 0);

    free(verts);
    volume_delete(v);
    return 0;
}

static int run_cubic_neighbor_culling(void)
{
    volume_t *v = volume_new();
    voxel_vertex_t *verts = new_verts();
    int size, sub;
    const int origin[3] = {0, 0, 0};

    // Two adjacent solid voxels: the touching faces are culled using
    // density only, even though both surfaces are fully transparent.
    set_sample(v, 4, 4, 4, 255, 0, 0, 0, 255);
    set_sample(v, 5, 4, 4, 0, 0, 255, 0, 255);
    int nb = volume_generate_vertices(v, origin, 0, verts, &size, &sub);
    TASK_CHECK(nb == 10);

    // Dropping the neighbor density brings the hidden face back, no
    // matter what its opacity says.
    set_sample(v, 5, 4, 4, 0, 0, 255, 255, 0);
    nb = volume_generate_vertices(v, origin, 0, verts, &size, &sub);
    TASK_CHECK(nb == 6);

    free(verts);
    volume_delete(v);
    return 0;
}

static int run_cubic_negative_and_mixed(void)
{
    volume_t *v = volume_new();
    voxel_vertex_t *verts = new_verts();
    int size, sub;
    const int block_neg[3] = {-16, -16, -16};

    // Geometry at negative coordinates.
    set_sample(v, -1, -1, -1, 10, 20, 30, 77, 255);
    int nb = volume_generate_vertices(v, block_neg, 0, verts,
                                      &size, &sub);
    TASK_CHECK(nb == 6);
    TASK_CHECK(all_alpha(verts, nb, size, 77));
    free(verts);
    volume_delete(v);

    // A mesh spanning a tile boundary with mixed legacy and explicit
    // writes: two adjacent voxels give ten faces with mixed alphas.
    volume_t *w = volume_new();
    uint8_t legacy[4] = {5, 5, 5, 255};
    volume_set_at(w, NULL, (int[]){15, 0, 0}, legacy);
    set_sample(w, 16, 0, 0, 6, 6, 6, 0, 255);
    volume_mesh_t *mesh = volume_generate_mesh(w, 0, NULL, 0);
    TASK_CHECK(mesh->indices_count == 60); // 10 quads.
    int saw_zero = 0, saw_opaque = 0, i;
    for (i = 0; i < mesh->vertices_count; i++) {
        if (mesh->vertices[i].color[3] == 0.0f) saw_zero = 1;
        if (mesh->vertices[i].color[3] == 1.0f) saw_opaque = 1;
    }
    TASK_CHECK(saw_zero && saw_opaque);
    volume_mesh_free(mesh);

    volume_delete(w);
    return 0;
}

static int run_cubic_mesh_linear_alpha_and_snapshot(void)
{
    volume_t *v = volume_new();
    volume_t *copy;
    volume_mesh_t *m1, *m2;

    set_sample(v, 4, 4, 4, 200, 100, 50, 128, 255);
    copy = volume_copy(v);

    m1 = volume_generate_mesh(v, 0, NULL, 0);
    TASK_CHECK(m1->indices_count == 36);
    for (int i = 0; i < m1->vertices_count; i++)
        TASK_CHECK(fabsf(m1->vertices[i].color[3] - 128 / 255.f) < 1e-6f);

    // Alpha only edit on the original: snapshot mesh keeps its alpha.
    set_sample(v, 4, 4, 4, 200, 100, 50, 16, 255);
    m2 = volume_generate_mesh(copy, 0, NULL, 0);
    TASK_CHECK(m2->indices_count == m1->indices_count);
    TASK_CHECK(m2->vertices_count == m1->vertices_count);
    for (int i = 0; i < m2->vertices_count; i++)
        TASK_CHECK(fabsf(m2->vertices[i].color[3] - 128 / 255.f) < 1e-6f);

    volume_mesh_free(m1);
    volume_mesh_free(m2);
    volume_delete(copy);
    volume_delete(v);
    return 0;
}

/* ----------------------------- marching cubes ------------------------- */

static int run_mc_alpha_invariant(void)
{
    volume_t *v = volume_new();
    voxel_vertex_t *a = new_verts();
    voxel_vertex_t *b = new_verts();
    int size, sub, x, y, z;
    const int origin[3] = {0, 0, 0};

    for (z = 0; z < 2; z++)
    for (y = 0; y < 2; y++)
    for (x = 0; x < 2; x++)
        set_sample(v, x, y, z, 10, 20, 30, 0, 255);

    int effects = EFFECT_MARCHING_CUBES | EFFECT_MC_SMOOTH;
    int nb0 = volume_generate_vertices(v, origin, effects, a, &size, &sub);
    TASK_CHECK(nb0 > 0 && size == 3);
    TASK_CHECK(all_alpha(a, nb0, size, 0));

    // Alpha only change, smooth mode: identical topology.
    for (z = 0; z < 2; z++)
    for (y = 0; y < 2; y++)
    for (x = 0; x < 2; x++)
        set_sample(v, x, y, z, 10, 20, 30, 99, 255);
    int nb1 = volume_generate_vertices(v, origin, effects, b, &size, &sub);
    TASK_CHECK(nb1 == nb0);
    TASK_CHECK(same_topology(a, nb0 * size, b, nb1 * size));
    TASK_CHECK(all_alpha(b, nb1, size, 99));

    // Same check in flat mode (triangle splitting must not react either).
    int nb2 = volume_generate_vertices(v, origin, EFFECT_MARCHING_CUBES,
                                       a, &size, &sub);
    for (z = 0; z < 2; z++)
    for (y = 0; y < 2; y++)
    for (x = 0; x < 2; x++)
        set_sample(v, x, y, z, 10, 20, 30, 255, 255);
    int nb3 = volume_generate_vertices(v, origin, EFFECT_MARCHING_CUBES,
                                       b, &size, &sub);
    TASK_CHECK(nb2 == nb3);
    TASK_CHECK(same_topology(a, nb2 * size, b, nb3 * size));
    TASK_CHECK(all_alpha(b, nb3, size, 255));

    free(a);
    free(b);
    volume_delete(v);
    return 0;
}

static int run_mc_density_surface(void)
{
    volume_t *v = volume_new();
    voxel_vertex_t *verts = new_verts();
    int size, sub;
    const int origin[3] = {0, 0, 0};

    // Solid but invisible voxel still generates marching cube geometry.
    set_sample(v, 1, 1, 1, 1, 2, 3, 0, 255);
    int solid = volume_generate_vertices(v, origin, EFFECT_MARCHING_CUBES,
                                         verts, &size, &sub);
    TASK_CHECK(solid > 0 && size == 3);
    TASK_CHECK(all_alpha(verts, solid, size, 0));

    // Keep the color, remove the density: no surface left.
    set_sample(v, 1, 1, 1, 1, 2, 3, 255, 0);
    int empty = volume_generate_vertices(v, origin, EFFECT_MARCHING_CUBES,
                                         verts, &size, &sub);
    TASK_CHECK(empty == 0);

    // Sign threshold stays at 127.
    set_sample(v, 1, 1, 1, 1, 2, 3, 255, 126);
    TASK_CHECK(volume_generate_vertices(v, origin, EFFECT_MARCHING_CUBES,
                                        verts, &size, &sub) == 0);
    set_sample(v, 1, 1, 1, 1, 2, 3, 255, 127);
    TASK_CHECK(volume_generate_vertices(v, origin, EFFECT_MARCHING_CUBES,
                                        verts, &size, &sub) > 0);

    free(verts);
    volume_delete(v);
    return 0;
}

static int run_mc_interpolation(void)
{
    volume_t *v = volume_new();
    voxel_vertex_t *lo = new_verts();
    voxel_vertex_t *hi = new_verts();
    voxel_vertex_t *alpha_only = new_verts();
    int size, sub;
    const int origin[3] = {0, 0, 0};
    int effects = EFFECT_MARCHING_CUBES | EFFECT_MC_SMOOTH;

    // Two adjacent voxels, one fully dense and one below the threshold.
    set_sample(v, 0, 0, 0, 255, 255, 255, 255, 255);
    set_sample(v, 1, 0, 0, 255, 255, 255, 255, 50);
    int nb_lo = volume_generate_vertices(v, origin, effects, lo,
                                         &size, &sub);
    TASK_CHECK(nb_lo > 0);

    // A different sub-threshold density moves the interpolated edges.
    set_sample(v, 1, 0, 0, 255, 255, 255, 255, 110);
    int nb_hi = volume_generate_vertices(v, origin, effects, hi,
                                         &size, &sub);
    TASK_CHECK(nb_hi == nb_lo);
    TASK_CHECK(!same_topology(lo, nb_lo * size, hi, nb_hi * size));

    // An alpha only change must not move anything.
    set_sample(v, 1, 0, 0, 255, 255, 255, 17, 110);
    int nb_a = volume_generate_vertices(v, origin, effects, alpha_only,
                                        &size, &sub);
    TASK_CHECK(nb_a == nb_hi);
    TASK_CHECK(same_topology(hi, nb_hi * size, alpha_only, nb_a * size));

    free(lo);
    free(hi);
    free(alpha_only);
    volume_delete(v);
    return 0;
}

static int run_mc_endpoint_color(void)
{
    volume_t *v = volume_new();
    voxel_vertex_t *verts = new_verts();
    int size, sub, i, nb;
    const int origin[3] = {0, 0, 0};

    // Dense red endpoint with a low alpha, lighter blue neighbor.
    set_sample(v, 0, 0, 0, 255, 0, 0, 32, 255);
    set_sample(v, 1, 0, 0, 0, 0, 255, 255, 110);
    nb = volume_generate_vertices(v, origin,
                                  EFFECT_MARCHING_CUBES | EFFECT_MC_SMOOTH,
                                  verts, &size, &sub);
    TASK_CHECK(nb > 0);

    int saw_red_linear = 0;
    for (i = 0; i < nb * size; i++) {
        if (verts[i].color[0] == 255 && verts[i].color[1] == 0 &&
            verts[i].color[2] == 0 && verts[i].color[3] == 32)
            saw_red_linear = 1;
    }
    TASK_CHECK(saw_red_linear);

    free(verts);
    volume_delete(v);
    return 0;
}

static int run_mc_legacy_and_mesh(void)
{
    volume_t *v = volume_new();
    voxel_vertex_t *verts = new_verts();
    int size, sub;
    const int origin[3] = {0, 0, 0};

    // Legacy data keeps its opaque geometry and alpha 255 output.
    uint8_t opaque[4] = {1, 2, 3, 255};
    volume_set_at(v, NULL, (int[]){1, 1, 1}, opaque);
    int nb = volume_generate_vertices(v, origin, EFFECT_MARCHING_CUBES,
                                      verts, &size, &sub);
    TASK_CHECK(nb > 0);
    TASK_CHECK(all_alpha(verts, nb, size, 255));

    // A legacy low alpha voxel is below the density threshold, as before.
    uint8_t faint[4] = {1, 2, 3, 64};
    volume_set_at(v, NULL, (int[]){1, 1, 1}, faint);
    TASK_CHECK(volume_generate_vertices(v, origin, EFFECT_MARCHING_CUBES,
                                        verts, &size, &sub) == 0);

    // Mesh export keeps the linear alpha with palette NULL, simplify 0.
    set_sample(v, 1, 1, 1, 10, 20, 40, 64, 255);
    volume_mesh_t *mesh = volume_generate_mesh(v, EFFECT_MARCHING_CUBES,
                                               NULL, 0);
    TASK_CHECK(mesh->indices_count > 0);
    for (int i = 0; i < mesh->vertices_count; i++)
        TASK_CHECK(fabsf(mesh->vertices[i].color[3] - 64 / 255.f) < 1e-6f);
    volume_mesh_free(mesh);

    free(verts);
    volume_delete(v);
    return 0;
}

const struct task_case task_cases[] = {
    {"storage", "sample_roundtrip", run_sample_roundtrip},
    {"storage", "legacy_interpretation", run_legacy_interpretation},
    {"storage", "mixed_writes", run_mixed_writes},
    {"storage", "snapshots", run_snapshots},
    {"storage", "tile_boundary_reads", run_tile_boundary_reads},
    {"storage", "empty_tiles_and_bbox", run_empty_tiles_and_bbox},
    {"storage", "copy_tile", run_copy_tile},

    {"cubic", "alpha_independent", run_cubic_alpha_independent},
    {"cubic", "density_threshold", run_cubic_density_threshold},
    {"cubic", "neighbor_culling", run_cubic_neighbor_culling},
    {"cubic", "negative_and_mixed_mesh", run_cubic_negative_and_mixed},
    {"cubic", "mesh_linear_alpha_snapshot",
     run_cubic_mesh_linear_alpha_and_snapshot},

    {"marching", "alpha_invariant", run_mc_alpha_invariant},
    {"marching", "density_surface", run_mc_density_surface},
    {"marching", "interpolation", run_mc_interpolation},
    {"marching", "endpoint_color", run_mc_endpoint_color},
    {"marching", "legacy_and_mesh", run_mc_legacy_and_mesh},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
