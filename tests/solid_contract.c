// Solid occupancy contract tests for vx_voxelize_solid.
//
// Groups:
//   winding   - signed integer winding: nested/reversed/coincident shells,
//               parity (odd/even) is shown to be insufficient
//   boundary  - exact surface contacts, shared edge/vertex ownership and
//               triangulation independence, narrow cavities
//   tiles     - one-shot vs tiled outputs, clipping outside the X range
//   ownership - validation, allocator injection, failure and cleanup
//
// Ground truth is derived independently from analytic box half-spaces and a
// tetrahedron half-space; whole output buffers are compared, never single
// points.

#define VOXELIZER_IMPLEMENTATION

// Inject a counting, fault-injecting allocator. The header guards VX_MALLOC,
// VX_CALLOC and VX_FREE with #ifndef, so definitions supplied before the
// include replace the libc defaults without touching the old entry points.
#include <stddef.h>
#include <stdlib.h>

static size_t alloc_live;
static size_t alloc_calls;
static size_t alloc_fail_at; /* 0 = never fail */

static void* test_malloc(size_t bytes)
{
    ++alloc_calls;
    if (alloc_fail_at && alloc_calls == alloc_fail_at) {
        return NULL;
    }
    void* p = malloc(bytes);
    if (p) {
        ++alloc_live;
    }
    return p;
}

static void* test_calloc(size_t bytes)
{
    ++alloc_calls;
    if (alloc_fail_at && alloc_calls == alloc_fail_at) {
        return NULL;
    }
    void* p = calloc(bytes, 1);
    if (p) {
        ++alloc_live;
    }
    return p;
}

static void test_free(void* p)
{
    if (p) {
        --alloc_live;
    }
    free(p);
}

#define VX_MALLOC(T, N) test_malloc((N) * sizeof(T))
#define VX_CALLOC(T, N) test_calloc((N) * sizeof(T))
#define VX_FREE(T) test_free(T)

#include "voxelizer.h"
#include "task_support/check.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --------------------------------------------------------------------------
// Mesh construction helpers
// --------------------------------------------------------------------------

static const unsigned int box_faces[12][3] = {
    {0,3,2},{0,2,1}, /* z = low  */
    {4,5,6},{4,6,7}, /* z = high */
    {0,1,5},{0,5,4}, /* y = low  */
    {2,3,7},{2,7,6}, /* y = high */
    {1,2,6},{1,6,5}, /* x = high */
    {0,4,7},{0,7,3}  /* x = low  */
};

static const float box_corners[8][3] = {
    {0,0,0},{1,0,0},{1,1,0},{0,1,0},
    {0,0,1},{1,0,1},{1,1,1},{0,1,1}
};

// One axis-aligned box shell, optionally flipped, placed in a shared mesh.
static void put_box(vx_mesh_t* m, unsigned int off,
                    double lo[3], double hi[3], int reversed)
{
    for (int i = 0; i < 8; ++i) {
        m->vertices[off + (unsigned int) i].x = (float) (lo[0] + box_corners[i][0] * (hi[0] - lo[0]));
        m->vertices[off + (unsigned int) i].y = (float) (lo[1] + box_corners[i][1] * (hi[1] - lo[1]));
        m->vertices[off + (unsigned int) i].z = (float) (lo[2] + box_corners[i][2] * (hi[2] - lo[2]));
    }
    unsigned int base = (off / 8u) * 36u;
    for (int t = 0; t < 12; ++t) {
        for (int k = 0; k < 3; ++k) {
            m->indices[base + (unsigned int) (3 * t + k)] =
                off + box_faces[t][reversed ? (2 - k) : k];
        }
    }
}

static vx_mesh_t* make_boxes(unsigned int nboxes,
                             const double lo[][3], const double hi[][3],
                             const int reversed[])
{
    vx_mesh_t* m = vx_mesh_alloc((int) (8 * nboxes), (int) (36 * nboxes));
    if (!m) {
        return NULL;
    }
    for (unsigned int b = 0; b < nboxes; ++b) {
        double l[3] = { lo[b][0], lo[b][1], lo[b][2] };
        double h[3] = { hi[b][0], hi[b][1], hi[b][2] };
        put_box(m, b * 8u, l, h, reversed[b]);
    }
    return m;
}

// Tetrahedron (0,0,0),(s,0,0),(0,s,0),(0,0,s), outward oriented.
static vx_mesh_t* make_tetra(double s, int reversed)
{
    vx_mesh_t* m = vx_mesh_alloc(4, 12);
    if (!m) {
        return NULL;
    }
    float p[4][3] = { {0,0,0}, {(float)s,0,0}, {0,(float)s,0}, {0,0,(float)s} };
    for (int i = 0; i < 4; ++i) {
        m->vertices[i].x = p[i][0];
        m->vertices[i].y = p[i][1];
        m->vertices[i].z = p[i][2];
    }
    unsigned int idx[12] = { 0,2,1, 0,1,3, 1,2,3, 0,3,2 };
    for (int i = 0; i < 12; ++i) {
        m->indices[i] = reversed ? idx[(i / 3) * 3 + (2 - i % 3)] : idx[i];
    }
    return m;
}

// --------------------------------------------------------------------------
// Independent analytic ground truth
// --------------------------------------------------------------------------

static int point_in_box(const double p[3], const double lo[3], const double hi[3])
{
    return p[0] >= lo[0] && p[0] <= hi[0] &&
           p[1] >= lo[1] && p[1] <= hi[1] &&
           p[2] >= lo[2] && p[2] <= hi[2];
}

// Generalized winding of a union of oriented box shells, evaluated directly
// from half-spaces (no ray casting), then reduced to non-zero occupancy.
static int ref_boxes(unsigned int nboxes,
                     const double lo[][3], const double hi[][3],
                     const int reversed[], const double p[3])
{
    int winding = 0;
    for (unsigned int b = 0; b < nboxes; ++b) {
        if (point_in_box(p, lo[b], hi[b])) {
            winding += reversed[b] ? -1 : 1;
        }
    }
    return winding != 0;
}

// Tetrahedron: x >= 0, y >= 0, z >= 0, x+y+z <= s (boundary included).
static int ref_tetra(double s, const double p[3])
{
    return p[0] >= 0.0 && p[1] >= 0.0 && p[2] >= 0.0 &&
           (p[0] + p[1] + p[2]) <= s;
}

static size_t idx3(const unsigned int size[3], unsigned int x, unsigned int y, unsigned int z)
{
    return ((size_t) z * (size_t) size[1] + (size_t) y) * (size_t) size[0] + (size_t) x;
}

static size_t count_mismatch(const unsigned char* a, const unsigned char* b, size_t n)
{
    size_t bad = 0;
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            ++bad;
        }
    }
    return bad;
}

static size_t volume_of(const unsigned int size[3])
{
    return (size_t) size[0] * (size_t) size[1] * (size_t) size[2];
}

// Fill a reference volume from an analytic predicate.
static void fill_ref_boxes(unsigned char* ref, const unsigned int size[3],
                           const double origin[3], const double step[3],
                           unsigned int nboxes, const double lo[][3],
                           const double hi[][3], const int reversed[])
{
    for (unsigned int z = 0; z < size[2]; ++z) {
        for (unsigned int y = 0; y < size[1]; ++y) {
            for (unsigned int x = 0; x < size[0]; ++x) {
                double p[3] = {
                    origin[0] + (double) x * step[0],
                    origin[1] + (double) y * step[1],
                    origin[2] + (double) z * step[2]
                };
                ref[idx3(size, x, y, z)] =
                    (unsigned char) ref_boxes(nboxes, lo, hi, reversed, p);
            }
        }
    }
}

static void fill_ref_tetra(unsigned char* ref, const unsigned int size[3],
                           const double origin[3], const double step[3], double s)
{
    for (unsigned int z = 0; z < size[2]; ++z) {
        for (unsigned int y = 0; y < size[1]; ++y) {
            for (unsigned int x = 0; x < size[0]; ++x) {
                double p[3] = {
                    origin[0] + (double) x * step[0],
                    origin[1] + (double) y * step[1],
                    origin[2] + (double) z * step[2]
                };
                ref[idx3(size, x, y, z)] = (unsigned char) ref_tetra(s, p);
            }
        }
    }
}

static int only_binary(const unsigned char* v, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (v[i] != 0 && v[i] != 1) {
            return 0;
        }
    }
    return 1;
}

// ==========================================================================
// WINDING GROUP
// ==========================================================================

static int case_tetra_halfspace(void)
{
    vx_mesh_t* m = make_tetra(4.0, 0);
    TASK_CHECK(m != NULL);
    const unsigned int size[3] = { 11, 11, 11 };
    const double origin[3] = { -1.0, -1.0, -1.0 };
    const double step[3] = { 0.5, 0.5, 0.5 };
    size_t n = volume_of(size);
    unsigned char* out = malloc(n);
    unsigned char* ref = malloc(n);
    TASK_CHECK(out && ref);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, size, out) == 1);
    fill_ref_tetra(ref, size, origin, step, 4.0);
    TASK_CHECK(only_binary(out, n));
    TASK_CHECK(count_mismatch(out, ref, n) == 0);
    free(out);
    free(ref);
    vx_mesh_free(m);
    return 0;
}

static int case_box_halfspace(void)
{
    double lo[1][3] = { { -2.0, -3.0, -1.5 } };
    double hi[1][3] = { {  2.0,  1.0,  2.5 } };
    int rev[1] = { 0 };
    vx_mesh_t* m = make_boxes(1, lo, hi, rev);
    TASK_CHECK(m != NULL);
    const unsigned int size[3] = { 9, 9, 9 };
    const double origin[3] = { -2.0, -3.0, -1.5 }; /* centers hit box faces */
    const double step[3] = { 0.5, 0.5, 0.5 };
    size_t n = volume_of(size);
    unsigned char* out = malloc(n);
    unsigned char* ref = malloc(n);
    TASK_CHECK(out && ref);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, size, out) == 1);
    fill_ref_boxes(ref, size, origin, step, 1, lo, hi, rev);
    TASK_CHECK(only_binary(out, n));
    TASK_CHECK(count_mismatch(out, ref, n) == 0);
    free(out);
    free(ref);
    vx_mesh_free(m);
    return 0;
}

static int case_nested_same_orientation(void)
{
    double lo[2][3] = { { 0,0,0 }, { 1,1,1 } };
    double hi[2][3] = { { 4,4,4 }, { 3,3,3 } };
    int rev[2] = { 0, 0 }; /* inner shell same orientation: stays solid */
    vx_mesh_t* m = make_boxes(2, lo, hi, rev);
    TASK_CHECK(m != NULL);
    const unsigned int size[3] = { 7, 7, 7 };
    const double origin[3] = { -0.5, -0.5, -0.5 };
    const double step[3] = { 1.0, 1.0, 1.0 };
    size_t n = volume_of(size);
    unsigned char* out = malloc(n);
    unsigned char* ref = malloc(n);
    TASK_CHECK(out && ref);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, size, out) == 1);
    fill_ref_boxes(ref, size, origin, step, 2, lo, hi, rev);
    TASK_CHECK(count_mismatch(out, ref, n) == 0);
    /* The inner region must be solid: winding 2 still reduces to occupancy 1. */
    TASK_CHECK(out[idx3(size, 2, 2, 2)] == 1);
    free(out);
    free(ref);
    vx_mesh_free(m);
    return 0;
}

static int case_nested_reversed_cavity(void)
{
    double lo[2][3] = { { 0,0,0 }, { 1,1,1 } };
    double hi[2][3] = { { 4,4,4 }, { 3,3,3 } };
    int rev[2] = { 0, 1 }; /* inner shell flipped: carves a cavity */
    vx_mesh_t* m = make_boxes(2, lo, hi, rev);
    TASK_CHECK(m != NULL);
    const unsigned int size[3] = { 7, 7, 7 };
    const double origin[3] = { -0.5, -0.5, -0.5 };
    const double step[3] = { 1.0, 1.0, 1.0 };
    size_t n = volume_of(size);
    unsigned char* out = malloc(n);
    unsigned char* ref = malloc(n);
    TASK_CHECK(out && ref);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, size, out) == 1);
    fill_ref_boxes(ref, size, origin, step, 2, lo, hi, rev);
    TASK_CHECK(count_mismatch(out, ref, n) == 0);
    TASK_CHECK(out[idx3(size, 2, 2, 2)] == 0); /* cavity center */
    TASK_CHECK(out[idx3(size, 1, 1, 1)] == 1); /* shell region */
    free(out);
    free(ref);
    vx_mesh_free(m);
    return 0;
}

static int case_coincident_cancel(void)
{
    double lo[2][3] = { { 0,0,0 }, { 0,0,0 } };
    double hi[2][3] = { { 2,2,2 }, { 2,2,2 } };
    int rev[2] = { 0, 1 }; /* identical geometry, opposite orientation */
    vx_mesh_t* m = make_boxes(2, lo, hi, rev);
    TASK_CHECK(m != NULL);
    /* Generic centers: none lies exactly on a face, so cancellation gives 0. */
    const unsigned int size[3] = { 5, 5, 5 };
    const double origin[3] = { 0.13, 0.27, 0.41 };
    const double step[3] = { 0.37, 0.43, 0.51 };
    size_t n = volume_of(size);
    unsigned char* out = malloc(n);
    unsigned char* ref = malloc(n);
    TASK_CHECK(out && ref);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, size, out) == 1);
    fill_ref_boxes(ref, size, origin, step, 2, lo, hi, rev);
    TASK_CHECK(count_mismatch(out, ref, n) == 0);
    size_t ones = 0;
    for (size_t i = 0; i < n; ++i) {
        ones += out[i];
    }
    TASK_CHECK(ones == 0);
    free(out);
    free(ref);
    vx_mesh_free(m);
    return 0;
}

static int case_overlapping_shells(void)
{
    double lo[2][3] = { { 0,0,0 }, { 2,2,2 } };
    double hi[2][3] = { { 4,4,4 }, { 6,6,6 } };
    int rev[2] = { 0, 0 }; /* overlapping, same orientation */
    vx_mesh_t* m = make_boxes(2, lo, hi, rev);
    TASK_CHECK(m != NULL);
    const unsigned int size[3] = { 8, 8, 8 };
    const double origin[3] = { -0.5, -0.5, -0.5 };
    const double step[3] = { 1.0, 1.0, 1.0 };
    size_t n = volume_of(size);
    unsigned char* out = malloc(n);
    unsigned char* ref = malloc(n);
    TASK_CHECK(out && ref);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, size, out) == 1);
    fill_ref_boxes(ref, size, origin, step, 2, lo, hi, rev);
    TASK_CHECK(count_mismatch(out, ref, n) == 0);
    free(out);
    free(ref);
    vx_mesh_free(m);
    return 0;
}

static int case_reversed_whole_mesh(void)
{
    double lo[1][3] = { { 0,0,0 } };
    double hi[1][3] = { { 3,3,3 } };
    int fwd[1] = { 0 }, back[1] = { 1 };
    vx_mesh_t* a = make_boxes(1, lo, hi, fwd);
    vx_mesh_t* b = make_boxes(1, lo, hi, back);
    TASK_CHECK(a && b);
    const unsigned int size[3] = { 6, 6, 6 };
    const double origin[3] = { -0.7, -0.3, -1.1 };
    const double step[3] = { 0.9, 0.8, 0.7 };
    size_t n = volume_of(size);
    unsigned char* oa = malloc(n);
    unsigned char* ob = malloc(n);
    TASK_CHECK(oa && ob);
    TASK_CHECK(vx_voxelize_solid(a, origin, step, size, oa) == 1);
    TASK_CHECK(vx_voxelize_solid(b, origin, step, size, ob) == 1);
    TASK_CHECK(count_mismatch(oa, ob, n) == 0);
    free(oa);
    free(ob);
    vx_mesh_free(a);
    vx_mesh_free(b);
    return 0;
}

static int case_zero_area_faces(void)
{
    double lo[1][3] = { { 0,0,0 } };
    double hi[1][3] = { { 2,2,2 } };
    int rev[1] = { 0 };
    vx_mesh_t* plain = make_boxes(1, lo, hi, rev);
    vx_mesh_t* withdeg = vx_mesh_alloc(8, 42);
    TASK_CHECK(plain && withdeg);
    for (int i = 0; i < 8; ++i) {
        withdeg->vertices[i] = plain->vertices[i];
    }
    memcpy(withdeg->indices, plain->indices, 36 * sizeof(unsigned int));
    withdeg->indices[36] = 2; withdeg->indices[37] = 2; withdeg->indices[38] = 5;
    withdeg->indices[39] = 0; withdeg->indices[40] = 0; withdeg->indices[41] = 0;
    const unsigned int size[3] = { 5, 5, 5 };
    const double origin[3] = { -0.5, -0.5, -0.5 };
    const double step[3] = { 1.0, 1.0, 1.0 };
    size_t n = volume_of(size);
    unsigned char* oa = malloc(n);
    unsigned char* ob = malloc(n);
    TASK_CHECK(oa && ob);
    TASK_CHECK(vx_voxelize_solid(plain, origin, step, size, oa) == 1);
    TASK_CHECK(vx_voxelize_solid(withdeg, origin, step, size, ob) == 1);
    TASK_CHECK(count_mismatch(oa, ob, n) == 0);
    free(oa);
    free(ob);
    vx_mesh_free(plain);
    vx_mesh_free(withdeg);
    return 0;
}

// Deliberately degraded reference: parity (odd/even) toggling instead of
// signed accumulation, written directly from box half-spaces. It compiles and
// runs the same numeric comparisons; nested same-oriented shells expose it.
static void fill_parity_boxes(unsigned char* dst, const unsigned int size[3],
                              const double origin[3], const double step[3],
                              unsigned int nboxes, const double lo[][3],
                              const double hi[][3])
{
    for (unsigned int z = 0; z < size[2]; ++z) {
        for (unsigned int y = 0; y < size[1]; ++y) {
            for (unsigned int x = 0; x < size[0]; ++x) {
                double p[3] = {
                    origin[0] + (double) x * step[0],
                    origin[1] + (double) y * step[1],
                    origin[2] + (double) z * step[2]
                };
                int parity = 0;
                for (unsigned int b = 0; b < nboxes; ++b) {
                    if (point_in_box(p, lo[b], hi[b])) {
                        parity ^= 1; /* ignores the sign of the shell */
                    }
                }
                dst[idx3(size, x, y, z)] = (unsigned char) parity;
            }
        }
    }
}

static int case_parity_degenerate_is_caught(void)
{
    double lo[2][3] = { { 0,0,0 }, { 1,1,1 } };
    double hi[2][3] = { { 4,4,4 }, { 3,3,3 } };
    int rev[2] = { 0, 0 };
    vx_mesh_t* m = make_boxes(2, lo, hi, rev);
    TASK_CHECK(m != NULL);
    const unsigned int size[3] = { 7, 7, 7 };
    const double origin[3] = { -0.5, -0.5, -0.5 };
    const double step[3] = { 1.0, 1.0, 1.0 };
    size_t n = volume_of(size);
    unsigned char* out = malloc(n);
    unsigned char* parity = malloc(n);
    unsigned char* ref = malloc(n);
    TASK_CHECK(out && parity && ref);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, size, out) == 1);
    fill_parity_boxes(parity, size, origin, step, 2, lo, hi);
    fill_ref_boxes(ref, size, origin, step, 2, lo, hi, rev);
    /* The implementation agrees with signed truth, and parity provably does
       not: the inner box region disagrees on the whole volume. */
    TASK_CHECK(count_mismatch(out, ref, n) == 0);
    TASK_CHECK(count_mismatch(parity, ref, n) > 0);
    TASK_CHECK(parity[idx3(size, 2, 2, 2)] == 0); /* parity wrongly carves it */
    TASK_CHECK(out[idx3(size, 2, 2, 2)] == 1);
    free(out);
    free(parity);
    free(ref);
    vx_mesh_free(m);
    return 0;
}

// ==========================================================================
// BOUNDARY GROUP
// ==========================================================================

static int case_cube_edge_vertex_centers(void)
{
    double lo[1][3] = { { 0,0,0 } };
    double hi[1][3] = { { 2,2,2 } };
    int rev[1] = { 0 };
    vx_mesh_t* m = make_boxes(1, lo, hi, rev);
    TASK_CHECK(m != NULL);
    /* Every center lands on a face, edge or vertex of the cube. */
    const unsigned int size[3] = { 3, 3, 3 };
    const double origin[3] = { 0.0, 0.0, 0.0 };
    const double step[3] = { 1.0, 1.0, 1.0 };
    size_t n = volume_of(size);
    unsigned char* out = malloc(n);
    unsigned char* ref = malloc(n);
    TASK_CHECK(out && ref);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, size, out) == 1);
    fill_ref_boxes(ref, size, origin, step, 1, lo, hi, rev);
    TASK_CHECK(count_mismatch(out, ref, n) == 0);
    size_t ones = 0;
    for (size_t i = 0; i < n; ++i) {
        ones += out[i];
    }
    TASK_CHECK(ones == n);
    free(out);
    free(ref);
    vx_mesh_free(m);
    return 0;
}

static int case_diagonal_swap_invariant(void)
{
    double lo[1][3] = { { 0,0,0 } };
    double hi[1][3] = { { 2,2,2 } };
    int rev[1] = { 0 };
    vx_mesh_t* a = make_boxes(1, lo, hi, rev);
    vx_mesh_t* b = vx_mesh_alloc(8, 36);
    TASK_CHECK(a && b);
    for (int i = 0; i < 8; ++i) {
        b->vertices[i] = a->vertices[i];
    }
    memcpy(b->indices, a->indices, 36 * sizeof(unsigned int));
    /* Re-diagonalize the z=high face (faces index 2 and 3). */
    b->indices[6] = 4;  b->indices[7] = 5;  b->indices[8] = 7;
    b->indices[9] = 5;  b->indices[10] = 6; b->indices[11] = 7;
    const unsigned int size[3] = { 4, 4, 4 };
    const double origin[3] = { -0.25, -0.25, -0.25 };
    const double step[3] = { 0.75, 0.75, 0.75 };
    size_t n = volume_of(size);
    unsigned char* oa = malloc(n);
    unsigned char* ob = malloc(n);
    TASK_CHECK(oa && ob);
    TASK_CHECK(vx_voxelize_solid(a, origin, step, size, oa) == 1);
    TASK_CHECK(vx_voxelize_solid(b, origin, step, size, ob) == 1);
    TASK_CHECK(count_mismatch(oa, ob, n) == 0);
    free(oa);
    free(ob);
    vx_mesh_free(a);
    vx_mesh_free(b);
    return 0;
}

static int case_coplanar_split_invariant(void)
{
    double lo[1][3] = { { 0,0,0 } };
    double hi[1][3] = { { 2,2,2 } };
    int rev[1] = { 0 };
    vx_mesh_t* a = make_boxes(1, lo, hi, rev);
    vx_mesh_t* b = vx_mesh_alloc(9, 42);
    TASK_CHECK(a && b);
    for (int i = 0; i < 8; ++i) {
        b->vertices[i] = a->vertices[i];
    }
    b->vertices[8].x = 1.0f; b->vertices[8].y = 1.0f; b->vertices[8].z = 2.0f;
    memcpy(b->indices, a->indices, 6 * sizeof(unsigned int));
    unsigned int top[12] = { 4,5,8, 5,6,8, 6,7,8, 7,4,8 };
    for (int i = 0; i < 12; ++i) {
        b->indices[6 + i] = top[i];
    }
    for (int i = 12; i < 36; ++i) {
        b->indices[6 + i] = a->indices[i];
    }
    const unsigned int size[3] = { 4, 4, 4 };
    const double origin[3] = { -0.25, -0.25, -0.25 };
    const double step[3] = { 0.75, 0.75, 0.75 };
    size_t n = volume_of(size);
    unsigned char* oa = malloc(n);
    unsigned char* ob = malloc(n);
    TASK_CHECK(oa && ob);
    TASK_CHECK(vx_voxelize_solid(a, origin, step, size, oa) == 1);
    TASK_CHECK(vx_voxelize_solid(b, origin, step, size, ob) == 1);
    TASK_CHECK(count_mismatch(oa, ob, n) == 0);
    free(oa);
    free(ob);
    vx_mesh_free(a);
    vx_mesh_free(b);
    return 0;
}

static int case_face_parallel_to_ray(void)
{
    double lo[1][3] = { { 0,0,0 } };
    double hi[1][3] = { { 2,2,2 } };
    int rev[1] = { 0 };
    vx_mesh_t* m = make_boxes(1, lo, hi, rev);
    TASK_CHECK(m != NULL);
    /* Single center sitting exactly on the y=0 face, which is parallel to X. */
    const unsigned int size[3] = { 1, 1, 1 };
    const double origin[3] = { 1.0, 0.0, 1.0 };
    const double step[3] = { 1.0, 1.0, 1.0 };
    unsigned char out[1] = { 0 };
    TASK_CHECK(vx_voxelize_solid(m, origin, step, size, out) == 1);
    TASK_CHECK(out[0] == 1);
    /* And on the y=high face. */
    const double origin2[3] = { 1.0, 2.0, 1.0 };
    out[0] = 0;
    TASK_CHECK(vx_voxelize_solid(m, origin2, step, size, out) == 1);
    TASK_CHECK(out[0] == 1);
    vx_mesh_free(m);
    return 0;
}

static int case_tetra_slanted_face_contact(void)
{
    vx_mesh_t* m = make_tetra(4.0, 0);
    TASK_CHECK(m != NULL);
    /* Centers (1,1,2) is exactly on x+y+z=4; (2,2,2) is outside. */
    const unsigned int size[3] = { 2, 2, 2 };
    const double origin[3] = { 1.0, 1.0, 2.0 };
    const double step[3] = { 1.0, 1.0, 1.0 };
    size_t n = volume_of(size);
    unsigned char* out = malloc(n);
    unsigned char* ref = malloc(n);
    TASK_CHECK(out && ref);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, size, out) == 1);
    fill_ref_tetra(ref, size, origin, step, 4.0);
    TASK_CHECK(count_mismatch(out, ref, n) == 0);
    TASK_CHECK(out[idx3(size, 0, 0, 0)] == 1); /* on the slanted face */
    TASK_CHECK(out[idx3(size, 1, 1, 0)] == 0); /* 2+2+2 = 6 > 4 */
    free(out);
    free(ref);
    vx_mesh_free(m);
    return 0;
}

static int case_narrow_cavity_preserved(void)
{
    /* Bounds are stated through floats, so the analytic truth must use the
       same float-rounded values rather than the decimal literals. */
    double flo = (double) 0.05f;
    double fhi = (double) 3.95f;
    double lo[2][3] = { { 0,0,0 }, { flo, flo, flo } };
    double hi[2][3] = { { 4,4,4 }, { fhi, fhi, fhi } };
    int rev[2] = { 0, 1 };
    vx_mesh_t* m = make_boxes(2, lo, hi, rev);
    TASK_CHECK(m != NULL);
    const unsigned int size[3] = { 201, 3, 3 };
    const double origin[3] = { 0.0, 0.99, 0.99 };
    const double step[3] = { 0.02, 0.5, 0.5 };
    size_t n = volume_of(size);
    unsigned char* out = malloc(n);
    unsigned char* ref = malloc(n);
    TASK_CHECK(out && ref);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, size, out) == 1);
    fill_ref_boxes(ref, size, origin, step, 2, lo, hi, rev);
    TASK_CHECK(count_mismatch(out, ref, n) == 0);
    free(out);
    free(ref);
    vx_mesh_free(m);
    return 0;
}

// ==========================================================================
// TILES GROUP
// ==========================================================================

static int case_tiled_reconstruction(void)
{
    double lo[1][3] = { { -1.3, -1.7, -2.1 } };
    double hi[1][3] = { {  2.7,  2.3,  1.9 } };
    int rev[1] = { 0 };
    vx_mesh_t* m = make_boxes(1, lo, hi, rev);
    TASK_CHECK(m != NULL);
    const unsigned int size[3] = { 9, 9, 9 }; /* divisible into 3-voxel tiles */
    const double origin[3] = { -2.0, -2.0, -2.0 };
    const double step[3] = { 0.5, 0.6, 0.7 }; /* negative world, non-uniform */
    size_t n = volume_of(size);
    unsigned char* full = malloc(n);
    unsigned char* stitched = malloc(n);
    TASK_CHECK(full && stitched);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, size, full) == 1);
    memset(stitched, 0x55, n);
    for (unsigned int tz = 0; tz < 3; ++tz) {
        for (unsigned int ty = 0; ty < 3; ++ty) {
            for (unsigned int tx = 0; tx < 3; ++tx) {
                unsigned int ts[3] = { 3, 3, 3 };
                double to[3] = {
                    origin[0] + (double) (tx * 3) * step[0],
                    origin[1] + (double) (ty * 3) * step[1],
                    origin[2] + (double) (tz * 3) * step[2]
                };
                unsigned char tile[27];
                TASK_CHECK(vx_voxelize_solid(m, to, step, ts, tile) == 1);
                for (unsigned int z = 0; z < 3; ++z) {
                    for (unsigned int y = 0; y < 3; ++y) {
                        for (unsigned int x = 0; x < 3; ++x) {
                            stitched[idx3(size, tx * 3 + x, ty * 3 + y, tz * 3 + z)] =
                                tile[idx3(ts, x, y, z)];
                        }
                    }
                }
            }
        }
    }
    TASK_CHECK(count_mismatch(full, stitched, n) == 0);
    free(full);
    free(stitched);
    vx_mesh_free(m);
    return 0;
}

static int case_xclip_inside_and_outside(void)
{
    double lo[1][3] = { { -10, -1, -1 } };
    double hi[1][3] = { {  10,  1,  1 } };
    int rev[1] = { 0 };
    vx_mesh_t* m = make_boxes(1, lo, hi, rev);
    TASK_CHECK(m != NULL);
    const unsigned int size[3] = { 3, 3, 3 };
    const double step[3] = { 1.0, 1.0, 1.0 };
    size_t n = volume_of(size);
    unsigned char* out = malloc(n);
    unsigned char* ref = malloc(n);
    TASK_CHECK(out && ref);
    /* Tile fully inside the mesh, far from any in-tile X crossing: the initial
       winding must come from crossings left of the tile. Y/Z centers run from
       one box face to the other, including exact surface contacts. */
    const double inside_origin[3] = { 4.0, -1.0, -1.0 };
    TASK_CHECK(vx_voxelize_solid(m, inside_origin, step, size, out) == 1);
    fill_ref_boxes(ref, size, inside_origin, step, 1, lo, hi, rev);
    TASK_CHECK(count_mismatch(out, ref, n) == 0);
    size_t ones = 0;
    for (size_t i = 0; i < n; ++i) {
        ones += out[i];
    }
    TASK_CHECK(ones == n);
    /* Tile fully to the right of the mesh: winding must be back to zero. */
    const double right_origin[3] = { 11.0, 0.0, 0.0 };
    TASK_CHECK(vx_voxelize_solid(m, right_origin, step, size, out) == 1);
    fill_ref_boxes(ref, size, right_origin, step, 1, lo, hi, rev);
    TASK_CHECK(count_mismatch(out, ref, n) == 0);
    for (size_t i = 0; i < n; ++i) {
        TASK_CHECK(out[i] == 0);
    }
    free(out);
    free(ref);
    vx_mesh_free(m);
    return 0;
}

// Deliberately degraded: pretend no crossings exist left of the tile, i.e.
// start the winding at zero instead of accumulating the clipped-away events.
// Compiled and executed against real outputs; the inside tile exposes it.
static int degraded_no_initial(const vx_mesh_t* m,
                               const double origin[3], const double step[3],
                               const unsigned int size[3], unsigned char* out)
{
    unsigned int nx = size[0], ny = size[1], nz = size[2];
    for (unsigned int z = 0; z < nz; ++z) {
        for (unsigned int y = 0; y < ny; ++y) {
            for (unsigned int x = 0; x < nx; ++x) {
                double p[3] = {
                    origin[0] + (double) x * step[0],
                    origin[1] + (double) y * step[1],
                    origin[2] + (double) z * step[2]
                };
                int winding = 0;
                for (size_t f = 0; f + 2 < m->nindices; f += 3) {
                    const vx_vertex_t* a = &m->vertices[m->indices[f]];
                    const vx_vertex_t* b = &m->vertices[m->indices[f + 1]];
                    const vx_vertex_t* c = &m->vertices[m->indices[f + 2]];
                    double proj = ((double) b->y - a->y) * ((double) c->z - a->z) -
                                  ((double) b->z - a->z) * ((double) c->y - a->y);
                    if (proj == 0.0) {
                        continue;
                    }
                    double lo = fmin(fmin(a->y, b->y), c->y);
                    double hi = fmax(fmax(a->y, b->y), c->y);
                    double loz = fmin(fmin(a->z, b->z), c->z);
                    double hiz = fmax(fmax(a->z, b->z), c->z);
                    if (p[1] < lo || p[1] > hi || p[2] < loz || p[2] > hiz) {
                        continue;
                    }
                    double denom = proj;
                    double dy = p[1] - (double) a->y;
                    double dz = p[2] - (double) a->z;
                    double u = (dy * ((double) c->z - a->z) -
                                dz * ((double) c->y - a->y)) / denom;
                    double v = (((double) b->y - a->y) * dz -
                                ((double) b->z - a->z) * dy) / denom;
                    double w = 1.0 - u - v;
                    if (u < 0.0 || v < 0.0 || w < 0.0) {
                        continue;
                    }
                    double hx = w * (double) a->x + u * (double) b->x + v * (double) c->x;
                    if (hx < origin[0]) {
                        continue; /* the bug: clipped-away crossings ignored */
                    }
                    if (hx <= p[0]) {
                        winding += (proj > 0.0) ? -1 : 1;
                    }
                }
                out[idx3(size, x, y, z)] = (unsigned char) (winding != 0);
            }
        }
    }
    return 1;
}

static int case_no_initial_degenerate_is_caught(void)
{
    double lo[1][3] = { { -10, -1, -1 } };
    double hi[1][3] = { {  10,  1,  1 } };
    int rev[1] = { 0 };
    vx_mesh_t* m = make_boxes(1, lo, hi, rev);
    TASK_CHECK(m != NULL);
    const unsigned int size[3] = { 3, 3, 3 };
    const double origin[3] = { 4.0, -1.0, -1.0 };
    const double step[3] = { 1.0, 1.0, 1.0 };
    size_t n = volume_of(size);
    unsigned char* good = malloc(n);
    unsigned char* bad = malloc(n);
    TASK_CHECK(good && bad);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, size, good) == 1);
    TASK_CHECK(degraded_no_initial(m, origin, step, size, bad) == 1);
    TASK_CHECK(count_mismatch(good, bad, n) > 0);
    size_t badones = 0;
    for (size_t i = 0; i < n; ++i) {
        badones += bad[i];
    }
    TASK_CHECK(badones == 0); /* degraded output wrongly empties the tile */
    size_t goodones = 0;
    for (size_t i = 0; i < n; ++i) {
        goodones += good[i];
    }
    TASK_CHECK(goodones == n);
    free(good);
    free(bad);
    vx_mesh_free(m);
    return 0;
}

static int case_xsplit_stitching(void)
{
    double lo[1][3] = { { 0,0,0 } };
    double hi[1][3] = { { 5,3,3 } };
    int rev[1] = { 0 };
    vx_mesh_t* m = make_boxes(1, lo, hi, rev);
    TASK_CHECK(m != NULL);
    const unsigned int size[3] = { 8, 4, 4 };
    const double origin[3] = { -0.5, -0.5, -0.5 };
    const double step[3] = { 1.0, 1.0, 1.0 };
    size_t n = volume_of(size);
    unsigned char* full = malloc(n);
    unsigned char* stitched = malloc(n);
    TASK_CHECK(full && stitched);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, size, full) == 1);
    unsigned int half[3] = { 4, 4, 4 };
    unsigned char left[64], right[64];
    double ol[3] = { origin[0], origin[1], origin[2] };
    double orr[3] = { origin[0] + 4.0 * step[0], origin[1], origin[2] };
    TASK_CHECK(vx_voxelize_solid(m, ol, step, half, left) == 1);
    TASK_CHECK(vx_voxelize_solid(m, orr, step, half, right) == 1);
    for (unsigned int z = 0; z < 4; ++z) {
        for (unsigned int y = 0; y < 4; ++y) {
            for (unsigned int x = 0; x < 8; ++x) {
                stitched[idx3(size, x, y, z)] =
                    x < 4 ? left[idx3(half, x, y, z)]
                          : right[idx3(half, x - 4, y, z)];
            }
        }
    }
    TASK_CHECK(count_mismatch(full, stitched, n) == 0);
    free(full);
    free(stitched);
    vx_mesh_free(m);
    return 0;
}

// ==========================================================================
// OWNERSHIP GROUP
// ==========================================================================

static int case_invalid_arguments_untouched(void)
{
    double lo[1][3] = { { 0,0,0 } };
    double hi[1][3] = { { 2,2,2 } };
    int rev[1] = { 0 };
    vx_mesh_t* m = make_boxes(1, lo, hi, rev);
    TASK_CHECK(m != NULL);
    const unsigned int size[3] = { 4, 4, 4 };
    const double origin[3] = { 0,0,0 };
    const double step[3] = { 1,1,1 };
    size_t n = volume_of(size);
    unsigned char* out = malloc(n);
    TASK_CHECK(out != NULL);
    memset(out, 0x7E, n);

    TASK_CHECK(vx_voxelize_solid(NULL, origin, step, size, out) == 0);
    TASK_CHECK(vx_voxelize_solid(m, NULL, step, size, out) == 0);
    TASK_CHECK(vx_voxelize_solid(m, origin, NULL, size, out) == 0);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, NULL, out) == 0);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, size, NULL) == 0);

    const unsigned int zsize[3] = { 0, 4, 4 };
    TASK_CHECK(vx_voxelize_solid(m, origin, step, zsize, out) == 0);

    const double zerostep[3] = { 1.0, 0.0, 1.0 };
    const double negstep[3] = { 1.0, -1.0, 1.0 };
    const double nanstep[3] = { 1.0, (double) NAN, 1.0 };
    const double infstep[3] = { (double) INFINITY, 1.0, 1.0 };
    TASK_CHECK(vx_voxelize_solid(m, origin, zerostep, size, out) == 0);
    TASK_CHECK(vx_voxelize_solid(m, origin, negstep, size, out) == 0);
    TASK_CHECK(vx_voxelize_solid(m, origin, nanstep, size, out) == 0);
    TASK_CHECK(vx_voxelize_solid(m, origin, infstep, size, out) == 0);

    /* Arithmetic size_t overflow, detected before any allocation. */
    const unsigned int huge1[3] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 2u };
    const unsigned int huge2[3] = { 0xFFFFFFFFu, 2u, 0xFFFFFFFFu };
    TASK_CHECK(vx_voxelize_solid(m, origin, step, huge1, out) == 0);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, huge2, out) == 0);

    for (size_t i = 0; i < n; ++i) {
        TASK_CHECK(out[i] == 0x7E);
    }
    free(out);
    vx_mesh_free(m);
    return 0;
}

static int case_bad_vertices_and_indices(void)
{
    const unsigned int size[3] = { 3, 3, 3 };
    const double origin[3] = { 0,0,0 };
    const double step[3] = { 1,1,1 };
    size_t n = volume_of(size);
    unsigned char* out = malloc(n);
    TASK_CHECK(out != NULL);
    memset(out, 0x33, n);

    vx_mesh_t* badidx = vx_mesh_alloc(4, 3);
    TASK_CHECK(badidx != NULL);
    badidx->indices[0] = 0;
    badidx->indices[1] = 1;
    badidx->indices[2] = 999; /* out of range */
    TASK_CHECK(vx_voxelize_solid(badidx, origin, step, size, out) == 0);
    vx_mesh_free(badidx);

    vx_mesh_t* nanmesh = vx_mesh_alloc(3, 3);
    TASK_CHECK(nanmesh != NULL);
    nanmesh->vertices[0].x = (float) NAN;
    nanmesh->indices[0] = 0;
    nanmesh->indices[1] = 1;
    nanmesh->indices[2] = 2;
    TASK_CHECK(vx_voxelize_solid(nanmesh, origin, step, size, out) == 0);
    vx_mesh_free(nanmesh);

    for (size_t i = 0; i < n; ++i) {
        TASK_CHECK(out[i] == 0x33);
    }
    free(out);
    return 0;
}

static int case_empty_mesh(void)
{
    vx_mesh_t* m = vx_mesh_alloc(0, 0);
    TASK_CHECK(m != NULL);
    const unsigned int size[3] = { 4, 3, 2 };
    const double origin[3] = { 0,0,0 };
    const double step[3] = { 1,1,1 };
    size_t n = volume_of(size);
    unsigned char* out = malloc(n);
    TASK_CHECK(out != NULL);
    memset(out, 0x91, n);
    TASK_CHECK(vx_voxelize_solid(m, origin, step, size, out) == 1);
    for (size_t i = 0; i < n; ++i) {
        TASK_CHECK(out[i] == 0);
    }
    free(out);
    vx_mesh_free(m);
    return 0;
}

// Injectable counting allocator (VX_MALLOC/VX_CALLOC/VX_FREE are #ifndef
// guarded in the header); see the definitions above the include.
static int case_allocator_failure_and_cleanup(void)
{
    double lo[1][3] = { { 0,0,0 } };
    double hi[1][3] = { { 4,4,4 } };
    int rev[1] = { 0 };
    alloc_fail_at = 0;
    vx_mesh_t* m = make_boxes(1, lo, hi, rev);
    TASK_CHECK(m != NULL);

    const unsigned int size[3] = { 8, 8, 8 };
    const double origin[3] = { -0.5, -0.5, -0.5 };
    const double step[3] = { 1.0, 1.0, 1.0 };
    size_t n = volume_of(size);
    unsigned char* out = malloc(n);
    unsigned char* ref = malloc(n);
    TASK_CHECK(out && ref);
    fill_ref_boxes(ref, size, origin, step, 1, lo, hi, rev);

    // The mesh and its buffers are live for the whole case.
    size_t baseline = alloc_live;
    TASK_CHECK(baseline > 0);

    // Inject failures at every voxelizer allocation point in turn. Every
    // failed call returns 0, leaves the output untouched, and releases all
    // temporary space; successful calls still produce the reference volume.
    for (size_t fail = 1; fail <= 64; ++fail) {
        alloc_fail_at = fail;
        alloc_calls = 0;
        memset(out, 0x6C, n);
        int rc = vx_voxelize_solid(m, origin, step, size, out);
        if (rc == 0) {
            for (size_t i = 0; i < n; ++i) {
                TASK_CHECK(out[i] == 0x6C);
            }
        } else {
            TASK_CHECK(count_mismatch(out, ref, n) == 0);
        }
        TASK_CHECK(alloc_live == baseline);
    }

    // Repeated successful calls must not accumulate temporary allocations.
    alloc_fail_at = 0;
    for (int rep = 0; rep < 5; ++rep) {
        TASK_CHECK(vx_voxelize_solid(m, origin, step, size, out) == 1);
        TASK_CHECK(alloc_live == baseline);
    }
    TASK_CHECK(count_mismatch(out, ref, n) == 0);

    free(out);
    free(ref);
    vx_mesh_free(m);
    TASK_CHECK(alloc_live == 0);
    return 0;
}

// --------------------------------------------------------------------------
// Registration
// --------------------------------------------------------------------------

const struct task_case task_cases[] = {
    { "winding", "tetra_halfspace", case_tetra_halfspace },
    { "winding", "box_halfspace", case_box_halfspace },
    { "winding", "nested_same_orientation", case_nested_same_orientation },
    { "winding", "nested_reversed_cavity", case_nested_reversed_cavity },
    { "winding", "coincident_cancel", case_coincident_cancel },
    { "winding", "overlapping_shells", case_overlapping_shells },
    { "winding", "reversed_whole_mesh", case_reversed_whole_mesh },
    { "winding", "zero_area_faces", case_zero_area_faces },
    { "winding", "parity_degenerate_caught", case_parity_degenerate_is_caught },

    { "boundary", "cube_edge_vertex_centers", case_cube_edge_vertex_centers },
    { "boundary", "diagonal_swap_invariant", case_diagonal_swap_invariant },
    { "boundary", "coplanar_split_invariant", case_coplanar_split_invariant },
    { "boundary", "face_parallel_to_ray", case_face_parallel_to_ray },
    { "boundary", "tetra_slanted_face_contact", case_tetra_slanted_face_contact },
    { "boundary", "narrow_cavity_preserved", case_narrow_cavity_preserved },

    { "tiles", "tiled_reconstruction", case_tiled_reconstruction },
    { "tiles", "xclip_inside_outside", case_xclip_inside_and_outside },
    { "tiles", "xsplit_stitching", case_xsplit_stitching },
    { "tiles", "no_initial_degenerate_caught", case_no_initial_degenerate_is_caught },

    { "ownership", "invalid_arguments_untouched", case_invalid_arguments_untouched },
    { "ownership", "bad_vertices_and_indices", case_bad_vertices_and_indices },
    { "ownership", "empty_mesh", case_empty_mesh },
    { "ownership", "allocator_failure_cleanup", case_allocator_failure_and_cleanup },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
