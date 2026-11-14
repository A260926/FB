/*
 * Regression tests for per-slot output interpolation on GL_TRIANGLE_LIST.
 *
 * Everything here is checked against an independent double precision
 * reference: the triangle (and its frustum-clipped polygon) is clipped
 * and projected in double, and values are evaluated with screen space
 * barycentric coordinates at pixel centers.
 */

#include "task_support/check.h"
#include "gl.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define W 64
#define H 64
#define ATTR 7
#define NPIX (W * H)

/********************************************************************
 *                          capture state                           *
 ********************************************************************/

struct capture {
    double attr[ATTR][NPIX];
    double depth[NPIX];
    unsigned char got[NPIX];
    unsigned calls;
};

static unsigned alt_vs_calls;

static void
vs_copy(float *out, float *in, void *uniform)
{
    (void)uniform;
    for (int i = 0; i < ATTR; i++)
        out[i] = in[i];
}

static void
vs_alt(float *out, float *in, void *uniform)
{
    (void)uniform;
    ++alt_vs_calls;
    for (int i = 0; i < ATTR; i++)
        out[i] = in[i];
}

static void
fs_capture(uint32_t *out, float *in, void *uniform)
{
    struct capture *c = (struct capture *)uniform;
    int x = (int)floorf(in[0]);
    int y = (int)floorf(in[1]);

    *out = 0x11223344u;

    if (x < 0 || x >= W || y < 0 || y >= H)
        return;

    int idx = y * W + x;
    for (int i = 0; i < ATTR; i++)
        c->attr[i][idx] = in[i];
    c->depth[idx] = in[2];
    c->got[idx] = 1;
    ++c->calls;
}

static void
buffers_clear(uint32_t *colors, float *depths, struct capture *cap)
{
    memset(cap, 0, sizeof(*cap));
    for (int i = 0; i < NPIX; i++) {
        colors[i] = 0;
        depths[i] = INFINITY;
    }
}

/********************************************************************
 *                        double reference                          *
 ********************************************************************/

struct mesh {
    int n_pts;
    double pts[8][ATTR];
    int n_idx;
    int idx[16];
};

struct ref_face {
    int n_poly;
    double poly[16][ATTR];       /* clipped, in original index order */
    double orig[3][ATTR];        /* original primitive vertices      */
    double flat_first[ATTR];
    double flat_last[ATTR];
};

static void
project(const double *v, double *sx, double *sy)
{
    *sx = (W / 2.0) * (v[0] / v[3] + 1.0);
    *sy = (H / 2.0) * (1.0 - v[1] / v[3]);
}

/* screen space barycentric coordinates of (px,py) in a triangle */

static int
screen_bary(const double *ax, const double *ay,
            double px, double py, double b[3])
{
    double det = (ay[1] - ay[2]) * (ax[0] - ax[2]) +
                 (ax[2] - ax[1]) * (ay[0] - ay[2]);

    if (fabs(det) < 1e-12)
        return 0;

    b[0] = ((ay[1] - ay[2]) * (px - ax[2]) +
            (ax[2] - ax[1]) * (py - ay[2])) / det;
    b[1] = ((ay[2] - ay[0]) * (px - ax[2]) +
            (ax[0] - ax[2]) * (py - ay[2])) / det;
    b[2] = 1.0 - b[0] - b[1];
    return 1;
}

/* Sutherland-Hodgman against the same five planes as clip.c */

static int
clip_ref(const double in[8][ATTR], int n, double out[16][ATTR])
{
    static const int axis[5] = {0, 1, 2, 0, 1};
    static const int sign[5] = {-1, -1, -1, 1, 1};
    double a[16][ATTR];
    double b[16][ATTR];

    if (n > 16)
        n = 16;
    memcpy(a, in, (size_t)n * ATTR * sizeof(double));

    for (int plane = 0; plane < 5; plane++) {
        int m = 0;

        for (int i = 0; i < n; i++) {
            const double *prev = a[(i + n - 1) % n];
            const double *cur = a[i];
            int pin = sign[plane] * prev[axis[plane]] <= prev[3];
            int cin = sign[plane] * cur[axis[plane]] <= cur[3];

            if (cin != pin) {
                double dp = prev[3] - sign[plane] * prev[axis[plane]];
                double dc = cur[3] - sign[plane] * cur[axis[plane]];
                double t = dp / (dp - dc);
                for (int k = 0; k < ATTR; k++)
                    b[m][k] = prev[k] + (cur[k] - prev[k]) * t;
                ++m;
            }
            if (cin) {
                memcpy(b[m], cur, ATTR * sizeof(double));
                ++m;
            }
        }

        n = m;
        if (n < 3)
            return 0;
        memcpy(a, b, (size_t)n * ATTR * sizeof(double));
    }

    memcpy(out, a, (size_t)n * ATTR * sizeof(double));
    return n;
}

static void
face_build(const struct mesh *mesh, const int *tri, struct ref_face *f)
{
    double in[8][ATTR];

    for (int i = 0; i < 3; i++) {
        memcpy(f->orig[i], mesh->pts[tri[i]], ATTR * sizeof(double));
        memcpy(in[i], mesh->pts[tri[i]], ATTR * sizeof(double));
    }
    memcpy(f->flat_first, f->orig[0], ATTR * sizeof(double));
    memcpy(f->flat_last, f->orig[2], ATTR * sizeof(double));
    f->n_poly = clip_ref(in, 3, f->poly);
}

/*
 * Finds the fan triangle of the clipped polygon containing the pixel
 * with the requested barycentric margin.  Fan triangles are the same
 * ones draw_prim walks: (0, i, i + 1).
 */

static int
face_bary(const struct ref_face *f, double px, double py, double margin,
          double b[3], int *fan)
{
    for (int i = 0; i + 2 < f->n_poly; i++) {
        int vi[3] = {0, i + 1, i + 2};
        double ax[3], ay[3], bb[3];

        for (int k = 0; k < 3; k++)
            project(f->poly[vi[k]], &ax[k], &ay[k]);

        if (!screen_bary(ax, ay, px, py, bb))
            continue;

        if (bb[0] > margin && bb[1] > margin && bb[2] > margin) {
            b[0] = bb[0];
            b[1] = bb[1];
            b[2] = bb[2];
            if (fan)
                *fan = i;
            return 1;
        }
    }
    return 0;
}

static void
orig_bary(const struct ref_face *f, double px, double py, double b[3])
{
    double ax[3], ay[3];

    for (int k = 0; k < 3; k++)
        project(f->orig[k], &ax[k], &ay[k]);
    screen_bary(ax, ay, px, py, b);
}

static double
ref_value(const struct ref_face *f, int slot,
          enum gl_interpolation_mode mode, enum gl_provoking_vertex prov,
          double px, double py)
{
    double b[3];

    if (mode == GL_INTERP_FLAT)
        return prov == GL_PROVOKE_LAST
            ? f->flat_last[slot]
            : f->flat_first[slot];

    orig_bary(f, px, py, b);

    if (mode == GL_INTERP_NOPERSPECTIVE) {
        /* screen affine */
        return b[0] * f->orig[0][slot] +
               b[1] * f->orig[1][slot] +
               b[2] * f->orig[2][slot];
    }

    /* perspective correct */
    double num = b[0] * f->orig[0][slot] / f->orig[0][3] +
                 b[1] * f->orig[1][slot] / f->orig[1][3] +
                 b[2] * f->orig[2][slot] / f->orig[2][3];
    double den = b[0] / f->orig[0][3] +
                 b[1] / f->orig[1][3] +
                 b[2] / f->orig[2][3];

    return num / den;
}

static double
ref_depth(const struct ref_face *f, int fan, const double b[3])
{
    int vi[3] = {0, fan + 1, fan + 2};
    double Z = 0;

    for (int k = 0; k < 3; k++)
        Z += b[k] / f->poly[vi[k]][3];

    return 1.0 / Z;
}

/********************************************************************
 *                         render helpers                           *
 ********************************************************************/

static float
mesh_float(const struct mesh *mesh, int pt, int attr)
{
    return (float)mesh->pts[pt][attr];
}

/* winding that makes the rasterizer accept the reference polygon */

static int
ref_winding(const struct ref_face *f)
{
    double ax[3], ay[3];

    if (f->n_poly < 3)
        return GL_WINDING_ORDER_CCW;

    for (int i = 0; i < 3; i++)
        project(f->poly[i], &ax[i], &ay[i]);

    double s = (ax[1] - ax[0]) * (ay[1] + ay[0]) +
               (ax[2] - ax[1]) * (ay[2] + ay[1]) +
               (ax[0] - ax[2]) * (ay[0] + ay[2]);

    return s > 0 ? GL_WINDING_ORDER_CCW : GL_WINDING_ORDER_CW;
}

static void
render_explicit(const struct mesh *mesh,
                const enum gl_interpolation_mode interp[GL_MAX_ATTRIBUTE_COUNT],
                enum gl_provoking_vertex prov, int zero_defaults,
                int winding, uint32_t *colors, float *depths,
                struct capture *cap, enum gl_primitive prim)
{
    float pts[8 * ATTR];
    struct gl_framebuffer fbuf = {colors, depths, W, H};
    struct gl_pipeline pipe;

    for (int p = 0; p < mesh->n_pts; p++)
        for (int a = 0; a < ATTR; a++)
            pts[p * ATTR + a] = mesh_float(mesh, p, a);

    memset(&pipe, 0, sizeof(pipe));
    pipe.fbuf = &fbuf;
    pipe.uniform = cap;
    pipe.vs = vs_copy;
    pipe.fs = fs_capture;
    pipe.pts_in = pts;
    pipe.n_pts = mesh->n_pts;
    pipe.n_attr_in = ATTR;
    pipe.n_attr_out = ATTR;
    pipe.winding = winding;

    if (!zero_defaults) {
        for (int i = 0; i < GL_MAX_ATTRIBUTE_COUNT; i++)
            pipe.interp[i] = interp[i];
        pipe.provoking = prov;
    }

    gl_render(&pipe, (int *)mesh->idx, mesh->n_idx, prim);
}

static void
modes_smooth(enum gl_interpolation_mode interp[GL_MAX_ATTRIBUTE_COUNT])
{
    for (int i = 0; i < GL_MAX_ATTRIBUTE_COUNT; i++)
        interp[i] = GL_INTERP_SMOOTH;
}

static void
gp_bindings_reset(void)
{
    for (int slot = 4; slot < GL_MAX_ATTRIBUTE_COUNT; slot++)
        gl_bind_interpolation(slot, GL_INTERP_SMOOTH);
    gl_bind_provoking_vertex(GL_PROVOKE_FIRST);
}

static void
gp_setup(const struct mesh *mesh, uint32_t *colors, float *depths,
         struct capture *cap)
{
    static float pts[8 * ATTR];

    for (int p = 0; p < mesh->n_pts; p++)
        for (int a = 0; a < ATTR; a++)
            pts[p * ATTR + a] = mesh_float(mesh, p, a);

    gl_bind_framebuffer(W, H, colors, depths);
    gl_bind_vertices(pts, mesh->n_pts, ATTR);
    gl_bind_uniform(cap);
    gl_bind_custom_vs(vs_copy, ATTR);
    gl_bind_custom_fs(fs_capture);
}

/********************************************************************
 *                        reference checking                        *
 ********************************************************************/

#define CLOSE(actual, expected, tol) \
    TASK_CHECK(fabs((double)(actual) - (double)(expected)) <= (tol))

/*
 * Validates one face over its strongly covered pixels.  The same
 * geometry is rendered twice by the caller: once with zero-initialized
 * interpolation (legacy) and once with the modes under test.  Coverage
 * and depth must be bit-identical between the two, while colors follow
 * the requested modes.
 */

static int
check_face(const struct capture *cap, const struct ref_face *f,
           const enum gl_interpolation_mode interp[GL_MAX_ATTRIBUTE_COUNT],
           enum gl_provoking_vertex prov, unsigned *compared)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            double px = x + 0.5;
            double py = y + 0.5;
            double b[3];
            int fan;

            if (!face_bary(f, px, py, 0.05, b, &fan))
                continue;

            int idx = y * W + x;
            TASK_CHECK(cap->got[idx]);

            TASK_CHECK(fabs(cap->depth[idx] - ref_depth(f, fan, b)) <= 1e-4);

            for (int slot = 4; slot < ATTR; slot++) {
                double expected = ref_value(f, slot, interp[slot], prov, px, py);
                double tol = (interp[slot] == GL_INTERP_FLAT) ? 1e-5 : 2e-3;
                CLOSE(cap->attr[slot][idx], expected, tol);
            }
            ++*compared;
        }
    }
    return 0;
}

/* coverage and depth must not change when interpolation modes change */

static int
check_same_coverage(const struct capture *a, const struct capture *b)
{
    TASK_CHECK(a->calls == b->calls);

    for (int i = 0; i < NPIX; i++) {
        TASK_CHECK(a->got[i] == b->got[i]);
        if (a->got[i])
            TASK_CHECK(fabs(a->depth[i] - b->depth[i]) <= 1e-6);
    }
    return 0;
}

/********************************************************************
 *                           test meshes                            *
 ********************************************************************/

/* unclipped triangle with three different w values */

static void
mesh_modes(struct mesh *m)
{
    static const double pts[3][ATTR] = {
        /* x      y      z     w    smooth  nopersp  flat */
        {-0.60,  0.60,  0.20, 1.00,  0.10,   2.00,   11.0},
        { 0.75, -0.70,  0.40, 2.00,  0.90,   7.00,   22.0},
        {-0.10,  0.85,  0.10, 3.50,  0.35,   4.50,   33.0},
    };
    static const int idx[3] = {0, 1, 2};

    memset(m, 0, sizeof(*m));
    m->n_pts = 3;
    memcpy(m->pts, pts, sizeof(pts));
    m->n_idx = 3;
    memcpy(m->idx, idx, sizeof(idx));
}

/*
 * Triangle crossing the left and near planes at the same time.
 * Vertex 0 is outside the left plane, vertex 1 outside the near plane.
 */

static void
mesh_clip(struct mesh *m, int reversed)
{
    static const double pts[3][ATTR] = {
        {-2.00,  0.50,  0.10, 1.00,  0.20,   1.00,   101.0},
        { 0.40, -0.90, -2.40, 2.00,  0.85,   5.00,   202.0},
        {-0.20,  0.30,  0.40, 3.00,  0.50,   3.00,   303.0},
    };
    static const int fwd[3] = {0, 1, 2};
    static const int rev[3] = {2, 1, 0};

    memset(m, 0, sizeof(*m));
    m->n_pts = 3;
    memcpy(m->pts, pts, sizeof(pts));
    m->n_idx = 3;
    memcpy(m->idx, reversed ? rev : fwd, 3 * sizeof(int));
}

/* two adjacent faces sharing the edge A-C */

static void
mesh_shared(struct mesh *m)
{
    static const double pts[4][ATTR] = {
        /* A */ {-0.80,  0.80, 0.10, 1.00, 0.15, 1.50,  10.0},
        /* B */ { 0.60,  0.70, 0.20, 2.00, 0.65, 4.00,  20.0},
        /* C */ { 0.70, -0.60, 0.15, 1.50, 0.95, 2.50,  30.0},
        /* D */ {-0.70, -0.70, 0.05, 3.00, 0.40, 6.00,  40.0},
    };
    /* face 1: A B C  (first A, last C)
     * face 2: C D A  (first C, last A) -- cyclic, same orientation */
    static const int idx[6] = {0, 1, 2, 2, 3, 0};

    memset(m, 0, sizeof(*m));
    m->n_pts = 4;
    memcpy(m->pts, pts, sizeof(pts));
    m->n_idx = 6;
    memcpy(m->idx, idx, sizeof(idx));
}

/********************************************************************
 *                          modes group                             *
 ********************************************************************/

static int
case_mixed_modes(enum gl_provoking_vertex prov)
{
    struct mesh m;
    struct ref_face f;
    enum gl_interpolation_mode mixed[GL_MAX_ATTRIBUTE_COUNT];
    int tri[3] = {0, 1, 2};
    uint32_t colors[NPIX];
    float depths[NPIX];
    struct capture legacy, tested;
    unsigned compared = 0;

    mesh_modes(&m);
    face_build(&m, tri, &f);
    TASK_CHECK(f.n_poly == 3);

    modes_smooth(mixed);
    mixed[4] = GL_INTERP_SMOOTH;
    mixed[5] = GL_INTERP_NOPERSPECTIVE;
    mixed[6] = GL_INTERP_FLAT;

    int winding = ref_winding(&f);

    buffers_clear(colors, depths, &legacy);
    render_explicit(&m, mixed, prov, 1, winding, colors, depths, &legacy,
                    GL_TRIANGLE_LIST);

    buffers_clear(colors, depths, &tested);
    render_explicit(&m, mixed, prov, 0, winding, colors, depths, &tested,
                    GL_TRIANGLE_LIST);

    TASK_CHECK(legacy.calls > 0);

    /* zero initialized pipeline really is smooth everywhere */
    enum gl_interpolation_mode all_smooth[GL_MAX_ATTRIBUTE_COUNT];
    modes_smooth(all_smooth);
    check_face(&legacy, &f, all_smooth, prov, &compared);
    TASK_CHECK(compared > 0);
    TASK_CHECK(check_same_coverage(&legacy, &tested) == 0);

    compared = 0;
    enum gl_interpolation_mode real_modes[GL_MAX_ATTRIBUTE_COUNT];
    modes_smooth(real_modes);
    real_modes[4] = GL_INTERP_SMOOTH;
    real_modes[5] = GL_INTERP_NOPERSPECTIVE;
    real_modes[6] = GL_INTERP_FLAT;
    check_face(&tested, &f, real_modes, prov, &compared);
    TASK_CHECK(compared > 0);

    /* at least one pixel must actually differ between the three modes */
    {
        int found = 0;
        for (int i = 0; i < NPIX && !found; i++) {
            if (!tested.got[i])
                continue;
            double px = (i % W) + 0.5;
            double py = (i / W) + 0.5;
            double s = ref_value(&f, 4, GL_INTERP_SMOOTH, prov, px, py);
            double n = ref_value(&f, 5, GL_INTERP_NOPERSPECTIVE, prov, px, py);
            if (fabs(s - n) > 1e-3)
                found = 1;
        }
        TASK_CHECK(found);
    }

    /* the mixed run kept slot 4 smooth and slot 6 constant */
    for (int i = 0; i < NPIX; i++) {
        if (!tested.got[i])
            continue;
        TASK_CHECK(fabs(tested.attr[6][i] -
                        (prov == GL_PROVOKE_LAST ? 33.0 : 11.0)) <= 1e-5);
    }

    return 0;
}

static int case_modes_first(void) { return case_mixed_modes(GL_PROVOKE_FIRST); }
static int case_modes_last(void)  { return case_mixed_modes(GL_PROVOKE_LAST); }

static int
case_legacy_default(void)
{
    struct mesh m;
    struct ref_face f;
    enum gl_interpolation_mode mixed[GL_MAX_ATTRIBUTE_COUNT];
    enum gl_interpolation_mode smooth[GL_MAX_ATTRIBUTE_COUNT];
    int tri[3] = {0, 1, 2};
    uint32_t colors[NPIX];
    float depths[NPIX];
    struct capture legacy, tested;
    unsigned compared = 0;

    mesh_modes(&m);
    face_build(&m, tri, &f);

    modes_smooth(smooth);
    modes_smooth(mixed);
    mixed[4] = GL_INTERP_NOPERSPECTIVE;
    mixed[5] = GL_INTERP_FLAT;
    mixed[6] = GL_INTERP_FLAT;

    int winding = ref_winding(&f);

    buffers_clear(colors, depths, &legacy);
    render_explicit(&m, smooth, GL_PROVOKE_FIRST, 1, winding, colors, depths,
                    &legacy, GL_TRIANGLE_LIST);

    buffers_clear(colors, depths, &tested);
    render_explicit(&m, mixed, GL_PROVOKE_LAST, 0, winding, colors, depths,
                    &tested, GL_TRIANGLE_LIST);

    check_face(&legacy, &f, smooth, GL_PROVOKE_FIRST, &compared);
    TASK_CHECK(compared > 0);
    TASK_CHECK(check_same_coverage(&legacy, &tested) == 0);

    /* explicit default colors */
    for (int i = 0; i < NPIX; i++) {
        if (!legacy.got[i])
            continue;
        double px = (i % W) + 0.5;
        double py = (i / W) + 0.5;
        CLOSE(legacy.attr[4][i],
              ref_value(&f, 4, GL_INTERP_SMOOTH, GL_PROVOKE_FIRST, px, py),
              2e-3);
        CLOSE(legacy.attr[5][i],
              ref_value(&f, 5, GL_INTERP_SMOOTH, GL_PROVOKE_FIRST, px, py),
              2e-3);
        CLOSE(legacy.attr[6][i],
              ref_value(&f, 6, GL_INTERP_SMOOTH, GL_PROVOKE_FIRST, px, py),
              2e-3);
    }
    return 0;
}

/* points, lines and strips keep their previous behavior */

static int
case_other_primitives(void)
{
    struct mesh m;
    enum gl_interpolation_mode mixed[GL_MAX_ATTRIBUTE_COUNT];
    uint32_t colors[NPIX];
    float depths[NPIX];
    struct capture cap;

    /* three isolated points inside the viewport */
    memset(&m, 0, sizeof(m));
    m.n_pts = 3;
    m.n_idx = 3;
    m.idx[0] = 0;
    m.idx[1] = 1;
    m.idx[2] = 2;
    m.pts[0][0] = -0.671875; m.pts[0][1] =  0.359375; m.pts[0][3] = 1;
    m.pts[1][0] =  0.10;     m.pts[1][1] = -0.20;     m.pts[1][3] = 1;
    m.pts[2][0] =  0.55;     m.pts[2][1] =  0.70;     m.pts[2][3] = 1;
    m.pts[0][4] = 0.25; m.pts[1][4] = 0.50; m.pts[2][4] = 0.75;

    modes_smooth(mixed);
    mixed[4] = GL_INTERP_NOPERSPECTIVE;
    mixed[5] = GL_INTERP_FLAT;
    mixed[6] = GL_INTERP_FLAT;

    buffers_clear(colors, depths, &cap);
    render_explicit(&m, mixed, GL_PROVOKE_LAST, 0, GL_WINDING_ORDER_CCW,
                    colors, depths, &cap, GL_POINT_LIST);
    TASK_CHECK(cap.calls == 3);
    /* point shading still receives the raw vertex attributes */
    {
        int idx = 20 * W + 10;
        TASK_CHECK(cap.got[idx]);
        CLOSE(cap.attr[4][idx], 0.25, 1e-6);
        CLOSE(cap.attr[5][idx], 0.0, 1e-6);
    }

    /* line list still draws nothing (unchanged stub) */
    buffers_clear(colors, depths, &cap);
    render_explicit(&m, mixed, GL_PROVOKE_LAST, 0, GL_WINDING_ORDER_CCW,
                    colors, depths, &cap, GL_LINE_LIST);
    TASK_CHECK(cap.calls == 0);

    /* a strip must stay fully smooth regardless of slot modes */
    {
        struct mesh tri;
        struct ref_face f;
        enum gl_interpolation_mode smooth[GL_MAX_ATTRIBUTE_COUNT];
        struct capture strip;
        int t[3] = {0, 1, 2};
        unsigned compared = 0;

        mesh_modes(&tri);
        face_build(&tri, t, &f);
        modes_smooth(smooth);

        buffers_clear(colors, depths, &strip);
        render_explicit(&tri, mixed, GL_PROVOKE_LAST, 0, ref_winding(&f),
                        colors, depths, &strip, GL_TRIANGLE_STRIP);
        TASK_CHECK(strip.calls > 0);
        check_face(&strip, &f, smooth, GL_PROVOKE_LAST, &compared);
        TASK_CHECK(compared > 0);
    }

    return 0;
}

/********************************************************************
 *                        clipping group                            *
 ********************************************************************/

/* true if the vertex lies outside at least one frustum plane */

static int
vertex_clipped(const double v[ATTR])
{
    return (v[3] + v[0] < 0) || (v[3] + v[1] < 0) || (v[3] + v[2] < 0) ||
           (v[3] - v[0] < 0) || (v[3] - v[1] < 0);
}

static int
case_clip_trimmed(int reversed, enum gl_provoking_vertex prov)
{
    struct mesh m;
    struct ref_face f;
    enum gl_interpolation_mode mixed[GL_MAX_ATTRIBUTE_COUNT];
    uint32_t colors[NPIX];
    float depths[NPIX];
    struct capture legacy, tested;
    unsigned compared = 0;

    mesh_clip(&m, reversed);
    face_build(&m, m.idx, &f);

    /* both clipping planes must really have produced intersections */
    TASK_CHECK(f.n_poly > 3);

    modes_smooth(mixed);
    mixed[4] = GL_INTERP_SMOOTH;
    mixed[5] = GL_INTERP_NOPERSPECTIVE;
    mixed[6] = GL_INTERP_FLAT;

    int winding = ref_winding(&f);

    buffers_clear(colors, depths, &legacy);
    render_explicit(&m, mixed, prov, 1, winding, colors, depths, &legacy,
                    GL_TRIANGLE_LIST);

    buffers_clear(colors, depths, &tested);
    render_explicit(&m, mixed, prov, 0, winding, colors, depths, &tested,
                    GL_TRIANGLE_LIST);

    TASK_CHECK(legacy.calls > 0);
    TASK_CHECK(check_same_coverage(&legacy, &tested) == 0);

    enum gl_interpolation_mode all_smooth[GL_MAX_ATTRIBUTE_COUNT];
    modes_smooth(all_smooth);
    check_face(&legacy, &f, all_smooth, prov, &compared);
    TASK_CHECK(compared > 0);

    compared = 0;
    check_face(&tested, &f, mixed, prov, &compared);
    TASK_CHECK(compared > 0);

    /*
     * The flat value comes from the original provoking vertex even when
     * that vertex was cut away by clipping: never from an intersection.
     */
    int provoking_index = (prov == GL_PROVOKE_LAST) ? 2 : 0;
    double flat_expected =
        (prov == GL_PROVOKE_LAST) ? f.flat_last[6] : f.flat_first[6];

    /* this geometry really does trim the provoking vertex away */
    TASK_CHECK(vertex_clipped(f.orig[provoking_index]));

    {
        int found = 0;
        for (int i = 0; i < NPIX; i++) {
            if (!tested.got[i])
                continue;
            CLOSE(tested.attr[6][i], flat_expected, 1e-5);
            found = 1;
        }
        TASK_CHECK(found);
    }

    /* the smoothed slot still follows the perspective reference */
    return 0;
}

static int
case_clip_first_trimmed(void)
{
    /* index order 0,1,2: vertex 0 (first) lies outside the left plane */
    return case_clip_trimmed(0, GL_PROVOKE_FIRST);
}

static int
case_clip_last_trimmed(void)
{
    /*
     * index order 2,1,0: vertex 0 (now the last provoking vertex for
     * GL_PROVOKE_LAST) is still the one outside the left plane.
     */
    return case_clip_trimmed(1, GL_PROVOKE_LAST);
}

static int
case_shared_index(void)
{
    struct mesh m;
    struct ref_face f1, f2;
    int t1[3] = {0, 1, 2};
    int t2[3] = {2, 3, 0};
    enum gl_interpolation_mode mixed[GL_MAX_ATTRIBUTE_COUNT];
    uint32_t colors[NPIX];
    float depths[NPIX];
    struct capture tested;
    unsigned compared = 0;

    mesh_shared(&m);
    face_build(&m, t1, &f1);
    face_build(&m, t2, &f2);

    TASK_CHECK(ref_winding(&f1) == ref_winding(&f2));

    modes_smooth(mixed);
    mixed[6] = GL_INTERP_FLAT;
    mixed[5] = GL_INTERP_NOPERSPECTIVE;

    buffers_clear(colors, depths, &tested);
    render_explicit(&m, mixed, GL_PROVOKE_FIRST, 0, ref_winding(&f1),
                    colors, depths, &tested, GL_TRIANGLE_LIST);
    TASK_CHECK(tested.calls > 0);

    /* each adjacent face keeps its own provoking vertex */
    check_face(&tested, &f1, mixed, GL_PROVOKE_FIRST, &compared);
    TASK_CHECK(compared > 0);
    compared = 0;
    check_face(&tested, &f2, mixed, GL_PROVOKE_FIRST, &compared);
    TASK_CHECK(compared > 0);

    /* face 1 flat = A (10), face 2 flat = C (30), C is shared */
    {
        int seen_first = 0, seen_second = 0;
        for (int i = 0; i < NPIX; i++) {
            if (!tested.got[i])
                continue;
            double px = (i % W) + 0.5;
            double py = (i / W) + 0.5;
            double b[3];
            if (face_bary(&f1, px, py, 0.2, b, NULL))
                { CLOSE(tested.attr[6][i], 10.0, 1e-5); seen_first = 1; }
            if (face_bary(&f2, px, py, 0.2, b, NULL))
                { CLOSE(tested.attr[6][i], 30.0, 1e-5); seen_second = 1; }
        }
        TASK_CHECK(seen_first);
        TASK_CHECK(seen_second);
    }

    /* same geometry with the last provoking vertex */
    buffers_clear(colors, depths, &tested);
    render_explicit(&m, mixed, GL_PROVOKE_LAST, 0, ref_winding(&f1),
                    colors, depths, &tested, GL_TRIANGLE_LIST);
    TASK_CHECK(tested.calls > 0);

    compared = 0;
    check_face(&tested, &f1, mixed, GL_PROVOKE_LAST, &compared);
    TASK_CHECK(compared > 0);
    compared = 0;
    check_face(&tested, &f2, mixed, GL_PROVOKE_LAST, &compared);
    TASK_CHECK(compared > 0);

    {
        int seen_first = 0, seen_second = 0;
        for (int i = 0; i < NPIX; i++) {
            if (!tested.got[i])
                continue;
            double px = (i % W) + 0.5;
            double py = (i / W) + 0.5;
            double b[3];
            if (face_bary(&f1, px, py, 0.2, b, NULL))
                { CLOSE(tested.attr[6][i], 30.0, 1e-5); seen_first = 1; }
            if (face_bary(&f2, px, py, 0.2, b, NULL))
                { CLOSE(tested.attr[6][i], 10.0, 1e-5); seen_second = 1; }
        }
        TASK_CHECK(seen_first);
        TASK_CHECK(seen_second);
    }

    return 0;
}

/********************************************************************
 *                        bindings group                            *
 ********************************************************************/

static int
case_argument_validation(void)
{
    struct mesh m;
    struct ref_face f;
    enum gl_interpolation_mode expected[GL_MAX_ATTRIBUTE_COUNT];
    uint32_t colors[NPIX];
    float depths[NPIX];
    struct capture cap;
    unsigned compared = 0;

    mesh_modes(&m);
    face_build(&m, m.idx, &f);

    gp_bindings_reset();

    TASK_CHECK(gl_bind_interpolation(4, GL_INTERP_NOPERSPECTIVE) == 1);
    TASK_CHECK(gl_bind_interpolation(5, GL_INTERP_FLAT) == 1);

    /* invalid slots and modes are rejected and leave the state alone */
    TASK_CHECK(gl_bind_interpolation(0, GL_INTERP_SMOOTH) == 0);
    TASK_CHECK(gl_bind_interpolation(3, GL_INTERP_FLAT) == 0);
    TASK_CHECK(gl_bind_interpolation(-1, GL_INTERP_SMOOTH) == 0);
    TASK_CHECK(gl_bind_interpolation(GL_MAX_ATTRIBUTE_COUNT, GL_INTERP_SMOOTH) == 0);
    TASK_CHECK(gl_bind_interpolation(4, (enum gl_interpolation_mode)3) == 0);
    TASK_CHECK(gl_bind_interpolation(4, (enum gl_interpolation_mode)-1) == 0);
    TASK_CHECK(gl_bind_interpolation(5, (enum gl_interpolation_mode)99) == 0);

    TASK_CHECK(gl_bind_provoking_vertex(GL_PROVOKE_LAST) == 1);
    TASK_CHECK(gl_bind_provoking_vertex((enum gl_provoking_vertex)2) == 0);
    TASK_CHECK(gl_bind_provoking_vertex((enum gl_provoking_vertex)-1) == 0);

    modes_smooth(expected);
    expected[4] = GL_INTERP_NOPERSPECTIVE;
    expected[5] = GL_INTERP_FLAT;
    expected[6] = GL_INTERP_SMOOTH;

    gp_setup(&m, colors, depths, &cap);
    buffers_clear(colors, depths, &cap);
    gl_renderl((int *)m.idx, m.n_idx, GL_TRIANGLE_LIST);
    TASK_CHECK(cap.calls > 0);
    check_face(&cap, &f, expected, GL_PROVOKE_LAST, &compared);
    TASK_CHECK(compared > 0);

    return 0;
}

static int
case_slot_range(void)
{
    gp_bindings_reset();

    for (int slot = 0; slot < GL_MAX_ATTRIBUTE_COUNT; slot++) {
        int expected = slot >= 4 ? 1 : 0;
        TASK_CHECK(gl_bind_interpolation(slot, GL_INTERP_SMOOTH) == expected);
        TASK_CHECK(gl_bind_interpolation(slot, GL_INTERP_NOPERSPECTIVE) == expected);
        TASK_CHECK(gl_bind_interpolation(slot, GL_INTERP_FLAT) == expected);
    }
    TASK_CHECK(gl_bind_provoking_vertex(GL_PROVOKE_FIRST) == 1);
    TASK_CHECK(gl_bind_provoking_vertex(GL_PROVOKE_LAST) == 1);

    gp_bindings_reset();
    return 0;
}

static int
case_interleaved(void)
{
    struct mesh m;
    struct ref_face f;
    enum gl_interpolation_mode smooth[GL_MAX_ATTRIBUTE_COUNT];
    enum gl_interpolation_mode flat[GL_MAX_ATTRIBUTE_COUNT];
    uint32_t colors[NPIX];
    float depths[NPIX];
    struct capture bound, explicit_, again;
    unsigned compared = 0;

    mesh_modes(&m);
    face_build(&m, m.idx, &f);
    modes_smooth(smooth);
    modes_smooth(flat);
    flat[4] = GL_INTERP_FLAT;

    int winding = ref_winding(&f);

    /* binding based entry */
    gp_bindings_reset();
    TASK_CHECK(gl_bind_interpolation(4, GL_INTERP_NOPERSPECTIVE) == 1);
    gp_setup(&m, colors, depths, &bound);
    buffers_clear(colors, depths, &bound);
    gl_renderl((int *)m.idx, m.n_idx, GL_TRIANGLE_LIST);
    TASK_CHECK(bound.calls > 0);

    /* explicit pipeline entry: its own smooth configuration */
    buffers_clear(colors, depths, &explicit_);
    render_explicit(&m, smooth, GL_PROVOKE_FIRST, 0, winding, colors, depths,
                    &explicit_, GL_TRIANGLE_LIST);

    /* the binding entry must not have leaked into the passed pipeline */
    compared = 0;
    check_face(&explicit_, &f, smooth, GL_PROVOKE_FIRST, &compared);
    TASK_CHECK(compared > 0);

    /* ...and the explicit draw must not have changed the bound state */
    compared = 0;
    {
        enum gl_interpolation_mode np[GL_MAX_ATTRIBUTE_COUNT];
        modes_smooth(np);
        np[4] = GL_INTERP_NOPERSPECTIVE;
        check_face(&bound, &f, np, GL_PROVOKE_FIRST, &compared);
        TASK_CHECK(compared > 0);
    }

    /* changing the bound vertex callback keeps the configuration */
    alt_vs_calls = 0;
    gl_bind_custom_vs(vs_alt, ATTR);
    buffers_clear(colors, depths, &again);
    gl_bind_uniform(&again);
    gl_renderl((int *)m.idx, m.n_idx, GL_TRIANGLE_LIST);
    TASK_CHECK(alt_vs_calls > 0);
    TASK_CHECK(again.calls > 0);
    compared = 0;
    {
        enum gl_interpolation_mode np[GL_MAX_ATTRIBUTE_COUNT];
        modes_smooth(np);
        np[4] = GL_INTERP_NOPERSPECTIVE;
        check_face(&again, &f, np, GL_PROVOKE_FIRST, &compared);
        TASK_CHECK(compared > 0);
    }

    /* switch the binding to flat; explicit smooth draws stay smooth */
    TASK_CHECK(gl_bind_interpolation(4, GL_INTERP_FLAT) == 1);
    buffers_clear(colors, depths, &again);
    gl_bind_uniform(&again);
    gl_renderl((int *)m.idx, m.n_idx, GL_TRIANGLE_LIST);
    compared = 0;
    {
        enum gl_interpolation_mode fl[GL_MAX_ATTRIBUTE_COUNT];
        modes_smooth(fl);
        fl[4] = GL_INTERP_FLAT;
        check_face(&again, &f, fl, GL_PROVOKE_FIRST, &compared);
        TASK_CHECK(compared > 0);
    }

    buffers_clear(colors, depths, &explicit_);
    render_explicit(&m, smooth, GL_PROVOKE_FIRST, 0, winding, colors, depths,
                    &explicit_, GL_TRIANGLE_LIST);
    compared = 0;
    check_face(&explicit_, &f, smooth, GL_PROVOKE_FIRST, &compared);
    TASK_CHECK(compared > 0);

    gp_bindings_reset();
    return 0;
}

/********************************************************************
 *                          registration                            *
 ********************************************************************/

const struct task_case task_cases[] = {
    {"modes", "mixed_modes_first", case_modes_first},
    {"modes", "mixed_modes_last", case_modes_last},
    {"modes", "legacy_defaults", case_legacy_default},
    {"modes", "other_primitives", case_other_primitives},
    {"clipping", "clip_first_trimmed", case_clip_first_trimmed},
    {"clipping", "clip_last_trimmed", case_clip_last_trimmed},
    {"clipping", "shared_index_flat", case_shared_index},
    {"bindings", "argument_validation", case_argument_validation},
    {"bindings", "slot_range", case_slot_range},
    {"bindings", "interleaved_entries", case_interleaved},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
