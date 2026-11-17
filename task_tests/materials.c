/*
 * materials.c - Per-vertex material ownership regression tests.
 *
 * Covers GL_TRIANGLES / GL_TRIANGLE_STRIP / GL_TRIANGLE_FAN in GL_FILL,
 * fed through immediate mode, glDrawArrays and display list replay.
 *
 * The reference below re-derives screen barycentrics, perspective weights,
 * clip-plane intersections and the fixed-function lighting equation on its
 * own; it never asks two renderer entry points to validate each other.
 *
 * Coordinate convention inside this file: framebuffer pixels use the
 * engine's top-left origin (sy grows downward), matching ndc_to_screen().
 * read_fb() converts to glReadPixels' bottom-left OpenGL origin.
 */

#include "../task_support/check.h"
#include "GL/gl.h"
#include "mytinygl.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small math helpers (independent of the engine's graphics.h)        */
/* ------------------------------------------------------------------ */

typedef struct { float x, y, z; } rvec3;

static float rdot(rvec3 a, rvec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static rvec3 rsub(rvec3 a, rvec3 b)
{
    return (rvec3){ a.x - b.x, a.y - b.y, a.z - b.z };
}

static rvec3 radd(rvec3 a, rvec3 b)
{
    return (rvec3){ a.x + b.x, a.y + b.y, a.z + b.z };
}

static rvec3 rscale(rvec3 a, float s)
{
    return (rvec3){ a.x * s, a.y * s, a.z * s };
}

static float rlen(rvec3 a)
{
    return sqrtf(rdot(a, a));
}

static rvec3 rnorm(rvec3 a)
{
    float l = rlen(a);
    if (l > 0.0f) return rscale(a, 1.0f / l);
    return a;
}

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* Reference material, mirroring the five tracked coefficients */
typedef struct {
    float amb[4], dif[4], spe[4], emi[4];
    float shiny;
} ref_mat;

/* Reference light (only LIGHT0 is used by these tests) */
typedef struct {
    float pos[4];      /* w = 0 directional, w = 1 positional (eye space) */
    float amb[4], dif[4], spe[4];
    int on;
} ref_light;

/* Independent re-implementation of the fixed-function lighting equation,
 * matching src/lighting.h step for step. Global light-model ambient is
 * forced to zero in every scene used here. */
static void ref_lighting(const ref_light *L, rvec3 eye, rvec3 n,
                         const ref_mat *m, float out[4])
{
    n = rnorm(n); /* the engine normalizes eye normals at emission */

    float r = m->emi[0], g = m->emi[1], b = m->emi[2];

    if (L->on) {
        rvec3 Lv;
        float atten = 1.0f;
        if (L->pos[3] == 0.0f) {
            Lv = rnorm((rvec3){ L->pos[0], L->pos[1], L->pos[2] });
        } else {
            rvec3 lp = { L->pos[0], L->pos[1], L->pos[2] };
            rvec3 to = rsub(lp, eye);
            float dist = rlen(to);
            if (dist < 1e-6f) dist = 1e-6f;
            Lv = rscale(to, 1.0f / dist);
            /* All tests use constant attenuation 1, linear/quadratic 0. */
            atten = 1.0f;
        }

        r += m->amb[0] * L->amb[0] * atten;
        g += m->amb[1] * L->amb[1] * atten;
        b += m->amb[2] * L->amb[2] * atten;

        float ndot = rdot(n, Lv);
        if (ndot > 0.0f) {
            r += m->dif[0] * L->dif[0] * atten * ndot;
            g += m->dif[1] * L->dif[1] * atten * ndot;
            b += m->dif[2] * L->dif[2] * atten * ndot;

            if (m->shiny > 0.0f) {
                /* Infinite viewer: V = (0, 0, 1) */
                rvec3 H = rnorm(radd(Lv, (rvec3){ 0.0f, 0.0f, 1.0f }));
                float nh = rdot(n, H);
                if (nh > 0.0f) {
                    float sp = powf(nh, m->shiny) * atten;
                    r += m->spe[0] * L->spe[0] * sp;
                    g += m->spe[1] * L->spe[1] * sp;
                    b += m->spe[2] * L->spe[2] * sp;
                }
            }
        }
    }

    out[0] = clamp01(r);
    out[1] = clamp01(g);
    out[2] = clamp01(b);
    out[3] = clamp01(m->dif[3]);
}

/* Screen-space barycentric coordinates using the engine's edge formula:
 * area   = edge(A,B,C), e0 = edge(B,C,P), e1 = edge(C,A,P), e2 = edge(A,B,P) */
static void screen_bary(float px, float py,
                        float ax, float ay, float bx, float by,
                        float cx, float cy, float *ba, float *bb, float *bc)
{
    float A  = (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
    float e0 = (px - bx) * (cy - by) - (py - by) * (cx - bx);
    float e1 = (px - cx) * (ay - cy) - (py - cy) * (ax - cx);
    float e2 = (px - ax) * (by - ay) - (py - ay) * (bx - ax);
    *ba = e0 / A;
    *bb = e1 / A;
    *bc = e2 / A;
}

/* Perspective-correct weights: screen barycentrics times 1/w, normalized. */
static void persp_weights(float ba, float bb, float bc,
                          float iw0, float iw1, float iw2,
                          float *k0, float *k1, float *k2)
{
    float s = ba * iw0 + bb * iw1 + bc * iw2;
    *k0 = ba * iw0 / s;
    *k1 = bb * iw1 / s;
    *k2 = bc * iw2 / s;
}

/* NDC -> top-origin framebuffer pixel coordinate (viewport 64x64).
 * The engine truncates vertex screen positions to integers before
 * rasterizing (see ndc_to_screen), so the reference does the same. */
static void ndc_pixel(float nx, float ny, float *sx, float *sy)
{
    *sx = (float)(int32_t)((nx + 1.0f) * 0.5f * 64.0f);
    *sy = (float)(int32_t)((1.0f - ny) * 0.5f * 64.0f);
}

static unsigned char to_byte(float v)
{
    return (unsigned char)(clamp01(v) * 255.0f);
}

/* ------------------------------------------------------------------ */
/* Engine driving helpers                                              */
/* ------------------------------------------------------------------ */

static const float COL_ZERO[4]  = { 0, 0, 0, 1 };
static const float COL_RED[4]   = { 1, 0, 0, 1 };
static const float COL_GREEN[4] = { 0, 1, 0, 1 };
static const float COL_BLUE[4]  = { 0, 0, 1, 1 };
static const float COL_WHITE[4] = { 1, 1, 1, 1 };

static GLState *make_ctx(void)
{
    GLState *c = gl_create_context(64, 64);
    gl_make_current(c);
    glViewport(0, 0, 64, 64);
    return c;
}

static void kill_ctx(GLState *c)
{
    gl_make_current(NULL);
    gl_destroy_context(c);
}

static void clear_fb(void)
{
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* Reset to a known lighting state: lighting on, no global ambient,
 * LIGHT0 present but switched off until a test enables it. */
static void base_lighting(void)
{
    glDisable(GL_LIGHT0);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, COL_ZERO);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_FALSE);
    glDisable(GL_COLOR_MATERIAL);
    glEnable(GL_LIGHTING);
    glShadeModel(GL_SMOOTH);
}

/* Set one face's material in the engine and mirror it into the reference. */
static void track_material(GLenum face, const float *amb, const float *dif,
                           const float *spe, const float *emi, float shiny,
                           ref_mat *out)
{
    glMaterialfv(face, GL_AMBIENT, amb);
    glMaterialfv(face, GL_DIFFUSE, dif);
    glMaterialfv(face, GL_SPECULAR, spe);
    glMaterialfv(face, GL_EMISSION, emi);
    glMaterialf(face, GL_SHININESS, shiny);
    if (out) {
        memcpy(out->amb, amb, sizeof(out->amb));
        memcpy(out->dif, dif, sizeof(out->dif));
        memcpy(out->spe, spe, sizeof(out->spe));
        memcpy(out->emi, emi, sizeof(out->emi));
        out->shiny = shiny;
    }
}

static void emissive_both(GLenum face, const float *emission, ref_mat *out)
{
    track_material(face, COL_ZERO, COL_ZERO, COL_ZERO, emission, 0.0f, out);
}

/* Read a framebuffer pixel using top-origin (sx, sy) coordinates. */
static void read_fb(int sx, int sy, unsigned char p[4])
{
    glReadPixels(sx, 63 - sy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
}

static int byte_near(int got, float expected)
{
    int e = (int)to_byte(expected);
    int d = got - e;
    return d >= -2 && d <= 2;
}

static int pixel_matches(const unsigned char p[4], const float ref[4])
{
    return byte_near(p[0], ref[0]) && byte_near(p[1], ref[1]) && byte_near(p[2], ref[2]);
}

/* Reference description of a submitted triangle vertex. */
typedef struct {
    rvec3 eye;            /* eye-space position */
    rvec3 n;              /* eye-space normal, unit length */
    ref_mat front, back;  /* materials bound at submission time */
    float w;              /* clip-space w */
} ref_vertex;

static void gouraud_vertex_colors(const ref_vertex *v, const ref_light *L,
                                  int two_side, float front[4], float back[4])
{
    ref_lighting(L, v->eye, v->n, &v->front, front);
    if (two_side) {
        rvec3 flipped = rscale(v->n, -1.0f);
        ref_lighting(L, v->eye, flipped, &v->back, back);
    } else {
        back[0] = front[0]; back[1] = front[1];
        back[2] = front[2]; back[3] = front[3];
    }
}

/* Screen position of a reference vertex (identity projection unless set). */
static void ref_screen(const ref_vertex *v, float *sx, float *sy)
{
    ndc_pixel(v->eye.x / v->w, v->eye.y / v->w, sx, sy);
}

/* Reference smooth (Gouraud) fragment for a non-clipped triangle.
 * back selects the back-face vertex colors; the color rule stays affine. */
static void smooth_ref_pixel(const ref_vertex v[3], const float fc[3][4],
                             const float bc[3][4], int back,
                             float px, float py, float out[4])
{
    float s[3][2];
    for (int i = 0; i < 3; i++) ref_screen(&v[i], &s[i][0], &s[i][1]);
    float ba, bb, bcc;
    screen_bary(px, py, s[0][0], s[0][1], s[1][0], s[1][1],
                s[2][0], s[2][1], &ba, &bb, &bcc);
    const float (*cols)[4] = back ? bc : fc;
    for (int ch = 0; ch < 4; ch++)
        out[ch] = ba * cols[0][ch] + bb * cols[1][ch] + bcc * cols[2][ch];
}

/* Reference Phong fragment: perspective-correct interpolation of eye
 * position, normal and the selected material, then the ordinary lighting. */
static void phong_ref_pixel(const ref_vertex v[3], const ref_light *L,
                            int two_side, int back,
                            float px, float py, float out[4])
{
    float s[3][2], iw[3];
    for (int i = 0; i < 3; i++) {
        ref_screen(&v[i], &s[i][0], &s[i][1]);
        iw[i] = 1.0f / v[i].w;
    }
    float ba, bb, bcc;
    screen_bary(px, py, s[0][0], s[0][1], s[1][0], s[1][1],
                s[2][0], s[2][1], &ba, &bb, &bcc);
    float kk[3];
    persp_weights(ba, bb, bcc, iw[0], iw[1], iw[2], &kk[0], &kk[1], &kk[2]);

    rvec3 eye = { 0, 0, 0 }, n = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) {
        eye = radd(eye, rscale(v[i].eye, kk[i]));
        n   = radd(n,   rscale(v[i].n,   kk[i]));
    }

    ref_mat interp;
    memset(&interp, 0, sizeof(interp));
    const ref_mat *mv[3];
    if (back && two_side) {
        n = rscale(n, -1.0f);
        for (int i = 0; i < 3; i++) mv[i] = &v[i].back;
    } else {
        for (int i = 0; i < 3; i++) mv[i] = &v[i].front;
    }
    for (int i = 0; i < 3; i++) {
        for (int ch = 0; ch < 4; ch++) {
            interp.amb[ch] += kk[i] * mv[i]->amb[ch];
            interp.dif[ch] += kk[i] * mv[i]->dif[ch];
            interp.spe[ch] += kk[i] * mv[i]->spe[ch];
            interp.emi[ch] += kk[i] * mv[i]->emi[ch];
        }
        interp.shiny += kk[i] * mv[i]->shiny;
    }
    ref_lighting(L, eye, n, &interp, out);
}

/* Build the reference light from engine state after scene configuration. */
static ref_light snapshot_light0(void)
{
    ref_light L;
    L.on = glIsEnabled(GL_LIGHT0) == GL_TRUE;
    glGetLightfv(GL_LIGHT0, GL_POSITION, L.pos);
    glGetLightfv(GL_LIGHT0, GL_AMBIENT, L.amb);
    glGetLightfv(GL_LIGHT0, GL_DIFFUSE, L.dif);
    glGetLightfv(GL_LIGHT0, GL_SPECULAR, L.spe);
    return L;
}

/* Integer framebuffer centroid of a reference triangle (strictly interior
 * for a non-degenerate triangle), plus its floating-point position. */
static void triangle_centroid(const ref_vertex v[3], int *cx, int *cy,
                              float *fx, float *fy)
{
    float s[3][2];
    for (int i = 0; i < 3; i++) ref_screen(&v[i], &s[i][0], &s[i][1]);
    float mx = (s[0][0] + s[1][0] + s[2][0]) / 3.0f;
    float my = (s[0][1] + s[1][1] + s[2][1]) / 3.0f;
    *cx = (int)mx; *cy = (int)my;
    *fx = (float)*cx; *fy = (float)*cy;
}

/* Build a ref_vertex triple from plain arrays plus per-vertex materials. */
static void make_ref_triangle(const float pos[3][3], const float norm[3][3],
                              const ref_mat fm[3], const ref_mat bm[3],
                              ref_vertex out[3])
{
    for (int i = 0; i < 3; i++) {
        out[i].eye = (rvec3){ pos[i][0], pos[i][1], pos[i][2] };
        out[i].n   = rnorm((rvec3){ norm[i][0], norm[i][1], norm[i][2] });
        out[i].front = fm[i];
        out[i].back  = bm[i];
        out[i].w = 1.0f;
    }
}

static const ref_mat BLACK_MAT = {
    {0,0,0,1}, {0,0,0,1}, {0,0,0,1}, {0,0,0,1}, 0.0f
};

/* Expected emission-only (no lights) color at a framebuffer pixel for a
 * triangle whose vertices carry the given emission colors. */
static void emission_ref_pixel(const float pos[3][3],
                                const float emi[3][4], int back,
                                float px, float py, float out[4])
{
    ref_mat fm[3], bm[3];
    float norm[3][3] = { {0,0,1},{0,0,1},{0,0,1} };
    for (int i = 0; i < 3; i++) {
        memcpy(fm[i].amb, COL_ZERO, 4 * sizeof(float));
        memcpy(fm[i].dif, COL_ZERO, 4 * sizeof(float));
        memcpy(fm[i].spe, COL_ZERO, 4 * sizeof(float));
        memcpy(fm[i].emi, back ? COL_ZERO : emi[i], 4 * sizeof(float));
        fm[i].shiny = 0.0f;
        memcpy(bm[i].amb, COL_ZERO, 4 * sizeof(float));
        memcpy(bm[i].dif, COL_ZERO, 4 * sizeof(float));
        memcpy(bm[i].spe, COL_ZERO, 4 * sizeof(float));
        memcpy(bm[i].emi, back ? emi[i] : COL_ZERO, 4 * sizeof(float));
        bm[i].shiny = 0.0f;
    }
    ref_vertex rv[3];
    make_ref_triangle(pos, norm, fm, bm, rv);
    ref_light L = { .on = 0 };
    float fcols[3][4], bcols[3][4];
    for (int i = 0; i < 3; i++)
        gouraud_vertex_colors(&rv[i], &L, 1, fcols[i], bcols[i]);
    smooth_ref_pixel(rv, fcols, bcols, back, px, py, out);
}

/* ================================================================== */
/* capture group                                                       */
/* ================================================================== */

/* The original bug report: emission-only triangle, one color per vertex,
 * checked on both windings with two-sided lighting enabled. */
static int emission_gradient_case(int back)
{
    GLState *c = make_ctx();
    base_lighting();
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    clear_fb();

    float pos[3][3]  = { {-0.8f,-0.8f,0}, {0.8f,-0.8f,0}, {0.0f,0.8f,0} };
    float norm[3][3] = { {0,0,1},{0,0,1},{0,0,1} };
    const float *ems[3] = { COL_RED, COL_GREEN, COL_BLUE };

    ref_mat fm[3], bm[3];
    glBegin(GL_TRIANGLES);
    if (!back) {
        /* Front winding A,B,C; front materials carry the gradient. */
        for (int i = 0; i < 3; i++) {
            emissive_both(GL_FRONT, ems[i], &fm[i]);
            emissive_both(GL_BACK, COL_ZERO, &bm[i]);
            glNormal3f(norm[i][0], norm[i][1], norm[i][2]);
            glVertex3f(pos[i][0], pos[i][1], pos[i][2]);
        }
    } else {
        /* Back winding A,C,B; back materials carry the gradient. */
        int order[3] = { 0, 2, 1 };
        for (int q = 0; q < 3; q++) {
            int i = order[q];
            emissive_both(GL_FRONT, COL_ZERO, &fm[q]);
            emissive_both(GL_BACK, ems[i], &bm[q]);
            glNormal3f(norm[i][0], norm[i][1], norm[i][2]);
            glVertex3f(pos[i][0], pos[i][1], pos[i][2]);
        }
    }
    glEnd();

    /* Reference triangle in actual submission order. */
    ref_vertex rv[3];
    if (!back) {
        make_ref_triangle(pos, norm, fm, bm, rv);
    } else {
        float p2[3][3], n2[3][3];
        int order[3] = { 0, 2, 1 };
        for (int q = 0; q < 3; q++)
            for (int k = 0; k < 3; k++) {
                p2[q][k] = pos[order[q]][k];
                n2[q][k] = norm[order[q]][k];
            }
        make_ref_triangle(p2, n2, fm, bm, rv);
    }

    ref_light L = { .on = 0 };
    float fcols[3][4], bcols[3][4];
    for (int i = 0; i < 3; i++)
        gouraud_vertex_colors(&rv[i], &L, 1, fcols[i], bcols[i]);

    /* Centroid plus two more interior probes, all evaluated independently. */
    int probes[3][2];
    int cx, cy; float fx, fy;
    triangle_centroid(rv, &cx, &cy, &fx, &fy);
    probes[0][0] = cx; probes[0][1] = cy;
    probes[1][0] = cx - 8; probes[1][1] = cy + 4;
    probes[2][0] = cx + 8; probes[2][1] = cy + 4;
    for (int q = 0; q < 3; q++) {
        float ref[4];
        smooth_ref_pixel(rv, fcols, bcols, back,
                         (float)probes[q][0], (float)probes[q][1], ref);
        unsigned char p[4];
        read_fb(probes[q][0], probes[q][1], p);
        TASK_CHECK(pixel_matches(p, ref));
        /* Every interior pixel mixes at least two emission channels. */
        TASK_CHECK((p[0] > 20) + (p[1] > 20) + (p[2] > 20) >= 2);
    }
    kill_ctx(c);
    return 0;
}

static int case_front_smooth(void) { return emission_gradient_case(0); }
static int case_back_smooth(void)  { return emission_gradient_case(1); }

/* GL_PHONG must interpolate the per-vertex materials, not read the last one. */
static int case_emission_phong(void)
{
    GLState *c = make_ctx();
    base_lighting();
    glShadeModel(GL_PHONG);
    clear_fb();

    float pos[3][3]  = { {-0.8f,-0.8f,0}, {0.8f,-0.8f,0}, {0.0f,0.8f,0} };
    float norm[3][3] = { {0,0,1},{0,0,1},{0,0,1} };
    const float *ems[3] = { COL_RED, COL_GREEN, COL_BLUE };
    ref_mat fm[3], bm[3];

    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        emissive_both(GL_FRONT, ems[i], &fm[i]);
        bm[i] = BLACK_MAT;
        glNormal3f(norm[i][0], norm[i][1], norm[i][2]);
        glVertex3f(pos[i][0], pos[i][1], pos[i][2]);
    }
    glEnd();

    ref_vertex rv[3];
    make_ref_triangle(pos, norm, fm, bm, rv);
    ref_light L = { .on = 0 };

    int cx, cy; float fx, fy;
    triangle_centroid(rv, &cx, &cy, &fx, &fy);
    int probes[3][2] = { {cx, cy}, {cx - 8, cy + 4}, {cx + 8, cy + 4} };
    for (int q = 0; q < 3; q++) {
        float ref[4];
        phong_ref_pixel(rv, &L, 0, 0,
                        (float)probes[q][0], (float)probes[q][1], ref);
        unsigned char p[4];
        read_fb(probes[q][0], probes[q][1], p);
        TASK_CHECK(pixel_matches(p, ref));
        TASK_CHECK((p[0] > 20) + (p[1] > 20) + (p[2] > 20) >= 2);
    }
    kill_ctx(c);
    return 0;
}

/* glGetMaterial keeps reporting current state; vertex snapshots must not
 * mutate it, and a post-glEnd change stays queryable. */
static int case_material_state_query(void)
{
    GLState *c = make_ctx();
    base_lighting();
    clear_fb();

    glBegin(GL_TRIANGLES);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_RED);
    glVertex3f(-0.8f, -0.8f, 0.0f);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_GREEN);
    glVertex3f(0.8f, -0.8f, 0.0f);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_BLUE);
    glVertex3f(0.0f, 0.8f, 0.0f);
    glEnd();

    float got[4];
    glGetMaterialfv(GL_FRONT, GL_EMISSION, got);
    TASK_CHECK(got[0] == 0.0f && got[1] == 0.0f && got[2] == 1.0f && got[3] == 1.0f);

    glMaterialfv(GL_BACK, GL_EMISSION, COL_GREEN);
    glGetMaterialfv(GL_BACK, GL_EMISSION, got);
    TASK_CHECK(got[1] == 1.0f && got[0] == 0.0f && got[2] == 0.0f);

    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 17.0f);
    GLfloat sh = 0.0f;
    glGetMaterialfv(GL_FRONT, GL_SHININESS, &sh);
    TASK_CHECK(sh == 17.0f);
    glGetMaterialfv(GL_BACK, GL_SHININESS, &sh);
    TASK_CHECK(sh == 17.0f);
    kill_ctx(c);
    return 0;
}

/* glColorMaterial drives only the selected face/parameter, per vertex. */
static int case_color_material_tracking(void)
{
    GLState *c = make_ctx();
    base_lighting();
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT, GL_EMISSION);
    clear_fb();

    float pos[3][3]  = { {-0.8f,-0.8f,0}, {0.8f,-0.8f,0}, {0.0f,0.8f,0} };
    float cols[3][4] = { {1,0,0,1},{0,1,0,1},{0,0,1,1} };

    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        glColor4f(cols[i][0], cols[i][1], cols[i][2], cols[i][3]);
        glVertex3f(pos[i][0], pos[i][1], pos[i][2]);
    }
    glEnd();

    /* The tracked emission ends at the last vertex color. */
    float got[4];
    glGetMaterialfv(GL_FRONT, GL_EMISSION, got);
    TASK_CHECK(got[2] == 1.0f && got[0] == 0.0f && got[1] == 0.0f);
    /* The untracked back material keeps its default black emission. */
    glGetMaterialfv(GL_BACK, GL_EMISSION, got);
    TASK_CHECK(got[0] == 0.0f && got[1] == 0.0f && got[2] == 0.0f);

    /* The rendered triangle shows the per-vertex color gradient used as
     * emission, evaluated against the independent affine reference. */
    float emi[3][4];
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 4; k++) emi[i][k] = cols[i][k];
    int grad_pts[2][2] = { {14, 44}, {50, 44} };
    for (int q = 0; q < 2; q++) {
        float ref[4];
        emission_ref_pixel(pos, emi, 0, (float)grad_pts[q][0], (float)grad_pts[q][1], ref);
        unsigned char p[4];
        read_fb(grad_pts[q][0], grad_pts[q][1], p);
        TASK_CHECK(pixel_matches(p, ref));
        TASK_CHECK((p[0] > 20) + (p[1] > 20) + (p[2] > 20) >= 2);
    }

    /* Switching the tracked parameter to diffuse freezes emission.
     * Tracking takes effect at the next submitted vertex, so emit one
     * (off-screen, so it cannot touch the framebuffer assertions). */
    glColorMaterial(GL_FRONT, GL_DIFFUSE);
    glColor3f(1.0f, 1.0f, 0.0f);
    glBegin(GL_POINTS);
    glVertex3f(-2.0f, -2.0f, 0.0f);
    glEnd();
    glGetMaterialfv(GL_FRONT, GL_EMISSION, got);
    TASK_CHECK(got[2] == 1.0f);
    glGetMaterialfv(GL_FRONT, GL_DIFFUSE, got);
    TASK_CHECK(got[0] == 1.0f && got[1] == 1.0f && got[2] == 0.0f);
    kill_ctx(c);
    return 0;
}

/* A compiled list replays the same per-vertex materials on every call. */
static int case_display_list_replay(void)
{
    GLState *c = make_ctx();
    base_lighting();

    GLuint list = glGenLists(1);
    TASK_CHECK(list != 0);
    glNewList(list, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    glMaterialfv(GL_FRONT, GL_AMBIENT, COL_ZERO);
    glMaterialfv(GL_FRONT, GL_DIFFUSE, COL_ZERO);
    glMaterialfv(GL_FRONT, GL_SPECULAR, COL_ZERO);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_RED);
    glVertex3f(-0.8f, -0.8f, 0.0f);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_GREEN);
    glVertex3f(0.8f, -0.8f, 0.0f);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_BLUE);
    glVertex3f(0.0f, 0.8f, 0.0f);
    glEnd();
    glEndList();

    static unsigned char first[64 * 64 * 4], second[64 * 64 * 4];
    clear_fb();
    glCallList(list);
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, first);

    /* Change current material after the first play: replay must ignore it. */
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_WHITE);
    clear_fb();
    glCallList(list);
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, second);
    TASK_CHECK(memcmp(first, second, sizeof(first)) == 0);

    /* The replayed frame still contains the three-channel gradient,
     * checked against an independently computed reference. */
    float dl_pos[3][3]  = { {-0.8f,-0.8f,0}, {0.8f,-0.8f,0}, {0.0f,0.8f,0} };
    float dl_emi[3][4]  = { {1,0,0,1}, {0,1,0,1}, {0,0,1,1} };
    int dl_pts[2][2] = { {14, 44}, {50, 44} };
    for (int q = 0; q < 2; q++) {
        float ref[4];
        emission_ref_pixel(dl_pos, dl_emi, 0,
                           (float)dl_pts[q][0], (float)dl_pts[q][1], ref);
        unsigned char p[4];
        read_fb(dl_pts[q][0], dl_pts[q][1], p);
        TASK_CHECK(pixel_matches(p, ref));
    }

    /* Replay leaves current state at the list's last material command. */
    float got[4];
    glGetMaterialfv(GL_FRONT, GL_EMISSION, got);
    TASK_CHECK(got[2] == 1.0f && got[0] == 0.0f);
    glDeleteLists(list, 1);
    kill_ctx(c);
    return 0;
}

/* ================================================================== */
/* lighting group                                                      */
/* ================================================================== */

/* Positional point light, white front material, red back material,
 * distinct normals; both one-sided and two-sided back faces are checked
 * against an independently evaluated reference. */
static int case_point_light_diffuse(void)
{
    GLState *c = make_ctx();
    base_lighting();
    glEnable(GL_NORMALIZE);
    glEnable(GL_LIGHT0);
    float lpos[4] = { 0.4f, 0.4f, 1.0f, 1.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, lpos);
    glLightfv(GL_LIGHT0, GL_AMBIENT, COL_ZERO);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, COL_WHITE);
    glLightfv(GL_LIGHT0, GL_SPECULAR, COL_ZERO);

    float pos[3][3]  = { {-0.8f,-0.8f,0}, {0.8f,-0.8f,0}, {0.0f,0.8f,0} };
    float norm[3][3] = { {-0.6f,0.0f,0.8f}, {0.6f,0.0f,0.8f}, {0.0f,0.6f,0.8f} };

    for (int pass = 0; pass < 2; pass++) {
        int back = pass;
        glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, back ? GL_TRUE : GL_FALSE);
        clear_fb();

        ref_mat fm[3], bm[3];
        glBegin(GL_TRIANGLES);
        if (!back) {
            for (int i = 0; i < 3; i++) {
                track_material(GL_FRONT, COL_ZERO, COL_WHITE, COL_ZERO, COL_ZERO, 0.0f, &fm[i]);
                bm[i] = BLACK_MAT;
                glNormal3f(norm[i][0], norm[i][1], norm[i][2]);
                glVertex3f(pos[i][0], pos[i][1], pos[i][2]);
            }
        } else {
            int order[3] = { 0, 2, 1 };
            for (int q = 0; q < 3; q++) {
                int i = order[q];
                fm[q] = BLACK_MAT;
                track_material(GL_BACK, COL_ZERO, COL_RED, COL_ZERO, COL_ZERO, 0.0f, &bm[q]);
                glNormal3f(norm[i][0], norm[i][1], norm[i][2]);
                glVertex3f(pos[i][0], pos[i][1], pos[i][2]);
            }
        }
        glEnd();

        ref_vertex rv[3];
        if (!back) {
            make_ref_triangle(pos, norm, fm, bm, rv);
        } else {
            float p2[3][3], n2[3][3];
            int order[3] = { 0, 2, 1 };
            for (int q = 0; q < 3; q++)
                for (int k = 0; k < 3; k++) {
                    p2[q][k] = pos[order[q]][k];
                    n2[q][k] = norm[order[q]][k];
                }
            make_ref_triangle(p2, n2, fm, bm, rv);
        }
        ref_light L = snapshot_light0();
        float fcols[3][4], bcols[3][4];
        for (int i = 0; i < 3; i++)
            gouraud_vertex_colors(&rv[i], &L, back, fcols[i], bcols[i]);

        int cx, cy; float fx, fy;
        triangle_centroid(rv, &cx, &cy, &fx, &fy);
        int probes[2][2] = { {cx, cy}, {cx - 6, cy + 5} };
        for (int q = 0; q < 2; q++) {
            float ref[4];
            smooth_ref_pixel(rv, fcols, bcols, back,
                             (float)probes[q][0], (float)probes[q][1], ref);
            unsigned char p[4];
            read_fb(probes[q][0], probes[q][1], p);
            TASK_CHECK(pixel_matches(p, ref));
        }
    }
    kill_ctx(c);
    return 0;
}

/* Specular highlight: Phong (interpolate normal, then shade) differs from
 * Gouraud (shade, then interpolate); each matches its own reference. */
static int case_specular_phong_vs_gouraud(void)
{
    GLState *c = make_ctx();

    float pos[3][3]  = { {-0.8f,-0.8f,0}, {0.8f,-0.8f,0}, {0.0f,0.8f,0} };
    /* The average of these normals points straight at the light, while no
     * individual vertex normal does: the highlight only exists per-fragment. */
    float norm[3][3] = {
        { -0.6f, -0.3f, 0.7416f },
        {  0.6f, -0.3f, 0.7416f },
        {  0.0f,  0.6f, 0.8f    }
    };
    float lpos[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

    unsigned char gouraud_px[4], phong_px[4];
    float g_ref[4], p_ref[4];
    int sx, sy; float fx, fy;

    for (int pass = 0; pass < 2; pass++) {
        base_lighting();
        glEnable(GL_NORMALIZE);
        glEnable(GL_LIGHT0);
        glLightfv(GL_LIGHT0, GL_POSITION, lpos);
        glLightfv(GL_LIGHT0, GL_AMBIENT, COL_ZERO);
        glLightfv(GL_LIGHT0, GL_DIFFUSE, COL_ZERO);
        glLightfv(GL_LIGHT0, GL_SPECULAR, COL_WHITE);
        glShadeModel(pass == 0 ? GL_SMOOTH : GL_PHONG);
        clear_fb();

        ref_mat fm[3], bm[3];
        glBegin(GL_TRIANGLES);
        for (int i = 0; i < 3; i++) {
            track_material(GL_FRONT, COL_ZERO, COL_ZERO, COL_WHITE, COL_ZERO, 32.0f, &fm[i]);
            bm[i] = BLACK_MAT;
            glNormal3f(norm[i][0], norm[i][1], norm[i][2]);
            glVertex3f(pos[i][0], pos[i][1], pos[i][2]);
        }
        glEnd();

        ref_vertex rv[3];
        make_ref_triangle(pos, norm, fm, bm, rv);
        ref_light L = snapshot_light0();
        triangle_centroid(rv, &sx, &sy, &fx, &fy);

        if (pass == 0) {
            read_fb(sx, sy, gouraud_px);
            float fcols[3][4], bcols[3][4];
            for (int i = 0; i < 3; i++)
                gouraud_vertex_colors(&rv[i], &L, 0, fcols[i], bcols[i]);
            smooth_ref_pixel(rv, fcols, bcols, 0, fx, fy, g_ref);
            TASK_CHECK(pixel_matches(gouraud_px, g_ref));
        } else {
            read_fb(sx, sy, phong_px);
            phong_ref_pixel(rv, &L, 0, 0, fx, fy, p_ref);
            TASK_CHECK(pixel_matches(phong_px, p_ref));
        }
    }

    /* The two shading models genuinely disagree at this fragment. */
    TASK_CHECK(abs((int)phong_px[0] - (int)gouraud_px[0]) > 12);
    TASK_CHECK(p_ref[0] > g_ref[0] + 0.05f);
    kill_ctx(c);
    return 0;
}

/* GL_FLAT always uses the original provoking (last) vertex, both faces. */
static int case_flat_provoking_vertex(void)
{
    GLState *c = make_ctx();
    base_lighting();
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    glShadeModel(GL_FLAT);
    clear_fb();

    glBegin(GL_TRIANGLES);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_RED);
    glMaterialfv(GL_BACK, GL_EMISSION, COL_GREEN);
    glVertex3f(-0.8f, -0.8f, 0.0f);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_GREEN);
    glMaterialfv(GL_BACK, GL_EMISSION, COL_RED);
    glVertex3f(0.8f, -0.8f, 0.0f);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_BLUE);
    glMaterialfv(GL_BACK, GL_EMISSION, COL_BLUE);
    glVertex3f(0.0f, 0.8f, 0.0f);
    glEnd();

    /* Front face: solid blue from the provoking front emission. */
    int pts[4][2] = { {14,44},{32,40},{50,44},{32,26} };
    for (int i = 0; i < 4; i++) {
        unsigned char p[4];
        read_fb(pts[i][0], pts[i][1], p);
        TASK_CHECK(p[2] >= 250 && p[0] <= 4 && p[1] <= 4);
    }
    kill_ctx(c);

    /* Reverse winding: back face must be solid blue too, since the last
     * submitted back emission is blue. */
    c = make_ctx();
    base_lighting();
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    glShadeModel(GL_FLAT);
    clear_fb();
    glBegin(GL_TRIANGLES);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_RED);
    glMaterialfv(GL_BACK, GL_EMISSION, COL_GREEN);
    glVertex3f(-0.8f, -0.8f, 0.0f);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_BLUE);
    glMaterialfv(GL_BACK, GL_EMISSION, COL_RED);
    glVertex3f(0.0f, 0.8f, 0.0f);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_GREEN);
    glMaterialfv(GL_BACK, GL_EMISSION, COL_BLUE);
    glVertex3f(0.8f, -0.8f, 0.0f);
    glEnd();
    for (int i = 0; i < 4; i++) {
        unsigned char p[4];
        read_fb(pts[i][0], pts[i][1], p);
        TASK_CHECK(p[2] >= 250 && p[0] <= 4 && p[1] <= 4);
    }
    kill_ctx(c);
    return 0;
}

/* Two-sided: back faces flip the normal and use the back material;
 * with two-sided lighting off, back faces use the front material. */
static int case_two_side_materials(void)
{
    GLState *c = make_ctx();
    base_lighting();
    glEnable(GL_NORMALIZE);
    glEnable(GL_LIGHT0);
    float lpos[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, lpos);
    glLightfv(GL_LIGHT0, GL_AMBIENT, COL_ZERO);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, COL_WHITE);
    glLightfv(GL_LIGHT0, GL_SPECULAR, COL_ZERO);

    /* This winding is back-facing; its normals point away from the viewer,
     * so only the flipped back normal faces the light. */
    float pos[3][3]  = { {-0.8f,-0.8f,0}, {0.0f,0.8f,0}, {0.8f,-0.8f,0} };
    float norm[3][3] = { {0,0,-1},{0,0,-1},{0,0,-1} };

    glMaterialfv(GL_FRONT, GL_AMBIENT, COL_ZERO);
    glMaterialfv(GL_FRONT, GL_DIFFUSE, COL_WHITE);
    glMaterialfv(GL_FRONT, GL_SPECULAR, COL_ZERO);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_ZERO);
    glMaterialfv(GL_BACK, GL_AMBIENT, COL_ZERO);
    glMaterialfv(GL_BACK, GL_DIFFUSE, COL_RED);
    glMaterialfv(GL_BACK, GL_SPECULAR, COL_ZERO);
    glMaterialfv(GL_BACK, GL_EMISSION, COL_ZERO);

    ref_mat fm[3], bm[3];
    for (int i = 0; i < 3; i++) {
        track_material(GL_FRONT, COL_ZERO, COL_WHITE, COL_ZERO, COL_ZERO, 0.0f, &fm[i]);
        track_material(GL_BACK,  COL_ZERO, COL_RED,   COL_ZERO, COL_ZERO, 0.0f, &bm[i]);
    }

    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    clear_fb();
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        glNormal3f(norm[i][0], norm[i][1], norm[i][2]);
        glVertex3f(pos[i][0], pos[i][1], pos[i][2]);
    }
    glEnd();

    ref_vertex rv[3];
    make_ref_triangle(pos, norm, fm, bm, rv);
    ref_light L = snapshot_light0();
    float fcols[3][4], bcols[3][4];
    for (int i = 0; i < 3; i++)
        gouraud_vertex_colors(&rv[i], &L, 1, fcols[i], bcols[i]);
    int cx, cy; float fx, fy;
    triangle_centroid(rv, &cx, &cy, &fx, &fy);
    float ref[4];
    smooth_ref_pixel(rv, fcols, bcols, 1, fx, fy, ref);
    unsigned char p[4];
    read_fb(cx, cy, p);
    TASK_CHECK(pixel_matches(p, ref));
    /* Lit back material is red-dominant (Gouraud averages the per-vertex
     * diffuse terms, so the exact level follows the reference above). */
    TASK_CHECK(p[0] > 150 && p[1] <= 6 && p[2] <= 6 && p[0] > 8 * (p[1] + 1));

    /* One-sided: unflipped normal faces away from the light -> black. */
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_FALSE);
    clear_fb();
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        glNormal3f(norm[i][0], norm[i][1], norm[i][2]);
        glVertex3f(pos[i][0], pos[i][1], pos[i][2]);
    }
    glEnd();
    read_fb(cx, cy, p);
    TASK_CHECK(p[0] <= 4 && p[1] <= 4 && p[2] <= 4);
    kill_ctx(c);
    return 0;
}

/* glFrontFace swaps which winding counts as the back face. */
static int case_front_face_switch(void)
{
    GLState *c = make_ctx();
    base_lighting();
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    clear_fb();

    glMaterialfv(GL_FRONT, GL_EMISSION, COL_RED);
    glMaterialfv(GL_BACK, GL_EMISSION, COL_GREEN);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.8f, -0.8f, 0.0f);
    glVertex3f(0.8f, -0.8f, 0.0f);
    glVertex3f(0.0f, 0.8f, 0.0f);
    glEnd();

    unsigned char p[4];
    read_fb(32, 36, p);
    TASK_CHECK(p[0] >= 250 && p[1] <= 4); /* CCW winding is front -> red */

    glFrontFace(GL_CW);
    clear_fb();
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.8f, -0.8f, 0.0f);
    glVertex3f(0.8f, -0.8f, 0.0f);
    glVertex3f(0.0f, 0.8f, 0.0f);
    glEnd();
    read_fb(32, 36, p);
    TASK_CHECK(p[1] >= 250 && p[0] <= 4); /* same winding now back -> green */
    kill_ctx(c);
    return 0;
}

/* Same scene through glDrawArrays with a color array driving emission. */
static int case_draw_arrays_entry(void)
{
    GLState *c = make_ctx();
    base_lighting();
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_EMISSION);
    clear_fb();

    static const float verts[9] = {
        -0.8f,-0.8f,0.0f,  0.8f,-0.8f,0.0f,  0.0f,0.8f,0.0f
    };
    static const float colors[12] = {
        1,0,0,1,  0,1,0,1,  0,0,1,1
    };
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, verts);
    glColorPointer(4, GL_FLOAT, 0, colors);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    /* Every fragment is checked against the independent emission reference,
     * exercising the same chain as immediate mode. */
    float da_pos[3][3]  = { {-0.8f,-0.8f,0}, {0.8f,-0.8f,0}, {0.0f,0.8f,0} };
    float da_emi[3][4]  = { {1,0,0,1}, {0,1,0,1}, {0,0,1,1} };
    int da_pts[3][2] = { {14, 44}, {50, 44}, {32, 24} };
    for (int q = 0; q < 3; q++) {
        float ref[4];
        emission_ref_pixel(da_pos, da_emi, 0,
                           (float)da_pts[q][0], (float)da_pts[q][1], ref);
        unsigned char p[4];
        read_fb(da_pts[q][0], da_pts[q][1], p);
        TASK_CHECK(pixel_matches(p, ref));
        TASK_CHECK((p[0] > 20) + (p[1] > 20) + (p[2] > 20) >= 2);
    }
    kill_ctx(c);
    return 0;
}

/* Two-sided GL_PHONG on a back face: the interpolated normal is flipped
 * and the interpolated back material is used, while the front material
 * (black here) must never appear. A positional light plus a per-vertex
 * back emission gradient exercises both paths in one fragment. */
static int case_phong_two_side_back(void)
{
    GLState *c = make_ctx();
    base_lighting();
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    glShadeModel(GL_PHONG);
    glEnable(GL_NORMALIZE);
    glEnable(GL_LIGHT0);
    float lpos[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, lpos);
    glLightfv(GL_LIGHT0, GL_AMBIENT, COL_ZERO);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, COL_WHITE);
    glLightfv(GL_LIGHT0, GL_SPECULAR, COL_ZERO);
    clear_fb();

    /* CW winding: the triangle is back-facing. Submission order A,C,B. */
    float pos[3][3]  = { {-0.8f,-0.8f,0}, {0.0f,0.8f,0}, {0.8f,-0.8f,0} };
    float norm[3][3] = { {0,0,-1},{0,0,-1},{0,0,-1} };
    const float *ems[3] = { COL_RED, COL_GREEN, COL_BLUE };
    ref_mat fm[3], bm[3];

    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        fm[i] = BLACK_MAT;
        track_material(GL_BACK, COL_ZERO, COL_RED, COL_ZERO, ems[i], 0.0f, &bm[i]);
        glNormal3f(norm[i][0], norm[i][1], norm[i][2]);
        glVertex3f(pos[i][0], pos[i][1], pos[i][2]);
    }
    glEnd();

    ref_vertex rv[3];
    make_ref_triangle(pos, norm, fm, bm, rv);
    ref_light L = snapshot_light0();

    int cx, cy; float fx, fy;
    triangle_centroid(rv, &cx, &cy, &fx, &fy);
    int probes[3][2] = { {cx, cy}, {cx - 7, cy + 4}, {cx + 7, cy + 4} };
    for (int q = 0; q < 3; q++) {
        float ref[4];
        phong_ref_pixel(rv, &L, 1, 1,
                        (float)probes[q][0], (float)probes[q][1], ref);
        unsigned char p[4];
        read_fb(probes[q][0], probes[q][1], p);
        TASK_CHECK(pixel_matches(p, ref));
        /* The flipped normal faces the light: red back diffuse lights up,
         * which the unflipped (away-facing) normal could not produce. */
        TASK_CHECK(p[0] > 180);
    }
    kill_ctx(c);
    return 0;
}

/* Same material chain reached through glDrawElements, with the indices
 * reordering the vertices: per-vertex color/material association must
 * follow the index, not the array slot order. */
static int case_draw_elements_entry(void)
{
    GLState *c = make_ctx();
    base_lighting();
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_EMISSION);
    clear_fb();

    static const float verts[9] = {
        -0.8f,-0.8f,0.0f,  0.8f,-0.8f,0.0f,  0.0f,0.8f,0.0f
    };
    static const float colors[12] = {
        1,0,0,1,  0,1,0,1,  0,0,1,1
    };
    /* Submission order becomes vertex 2 (blue), 0 (red), 1 (green). */
    static const GLubyte indices[3] = { 2, 0, 1 };

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, verts);
    glColorPointer(4, GL_FLOAT, 0, colors);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_BYTE, indices);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    /* Colors stay attached to their positions regardless of index order,
     * so the same position-based emission reference applies. */
    float de_pos[3][3]  = { {-0.8f,-0.8f,0}, {0.8f,-0.8f,0}, {0.0f,0.8f,0} };
    float de_emi[3][4]  = { {1,0,0,1}, {0,1,0,1}, {0,0,1,1} };
    int de_pts[3][2] = { {14, 44}, {50, 44}, {32, 24} };
    for (int q = 0; q < 3; q++) {
        float ref[4];
        emission_ref_pixel(de_pos, de_emi, 0,
                           (float)de_pts[q][0], (float)de_pts[q][1], ref);
        unsigned char p[4];
        read_fb(de_pts[q][0], de_pts[q][1], p);
        TASK_CHECK(pixel_matches(p, ref));
        TASK_CHECK((p[0] > 20) + (p[1] > 20) + (p[2] > 20) >= 2);
    }
    kill_ctx(c);
    return 0;
}

/* Strip and fan share the same per-vertex material chain. Reference
 * triangulation mirrors flush_triangle_strip / flush_triangle_fan. */
static int case_strip_and_fan(void)
{
    GLState *c = make_ctx();
    base_lighting();
    clear_fb();

    float pos[4][3] = {
        {-0.9f,-0.7f,0}, {-0.3f,-0.7f,0}, {-0.9f,0.7f,0}, {-0.3f,0.7f,0}
    };
    const float *ems[4] = { COL_RED, COL_GREEN, COL_BLUE, COL_GREEN };
    ref_mat fm[4];
    glBegin(GL_TRIANGLE_STRIP);
    for (int i = 0; i < 4; i++) {
        emissive_both(GL_FRONT, ems[i], &fm[i]);
        glVertex3f(pos[i][0], pos[i][1], pos[i][2]);
    }
    glEnd();

    /* Engine strip order: tri0 = (0,1,2), tri1 = (1,0,2). */
    int strip_tris[2][3] = { {0,1,2}, {1,0,2} };
    for (int t = 0; t < 2; t++) {
        ref_vertex rv[3];
        float tp[3][3], tn[3][3] = { {0,0,1},{0,0,1},{0,0,1} };
        ref_mat tf[3], tb[3];
        for (int q = 0; q < 3; q++) {
            int i = strip_tris[t][q];
            tp[q][0] = pos[i][0]; tp[q][1] = pos[i][1]; tp[q][2] = pos[i][2];
            tf[q] = fm[i]; tb[q] = BLACK_MAT;
        }
        make_ref_triangle(tp, tn, tf, tb, rv);
        ref_light L = { .on = 0 };
        float fcols[3][4], bcols[3][4];
        for (int i = 0; i < 3; i++)
            gouraud_vertex_colors(&rv[i], &L, 0, fcols[i], bcols[i]);
        int cx, cy; float fx, fy;
        triangle_centroid(rv, &cx, &cy, &fx, &fy);
        float ref[4];
        smooth_ref_pixel(rv, fcols, bcols, 0, fx, fy, ref);
        unsigned char p[4];
        read_fb(cx, cy, p);
        TASK_CHECK(pixel_matches(p, ref));
    }

    /* Fan: center red, rim green, blue, green; engine order (0,1,2),(0,2,3). */
    clear_fb();
    float fpos[4][3] = {
        {0.2f,0.0f,0}, {0.4f,-0.7f,0}, {0.9f,0.0f,0}, {0.4f,0.7f,0}
    };
    ref_mat ff[4];
    glBegin(GL_TRIANGLE_FAN);
    emissive_both(GL_FRONT, COL_RED, &ff[0]);
    glVertex3f(fpos[0][0], fpos[0][1], fpos[0][2]);
    emissive_both(GL_FRONT, COL_GREEN, &ff[1]);
    glVertex3f(fpos[1][0], fpos[1][1], fpos[1][2]);
    emissive_both(GL_FRONT, COL_BLUE, &ff[2]);
    glVertex3f(fpos[2][0], fpos[2][1], fpos[2][2]);
    emissive_both(GL_FRONT, COL_GREEN, &ff[3]);
    glVertex3f(fpos[3][0], fpos[3][1], fpos[3][2]);
    glEnd();

    int fan_tris[2][3] = { {0,1,2}, {0,2,3} };
    for (int t = 0; t < 2; t++) {
        ref_vertex rv[3];
        float tp[3][3], tn[3][3] = { {0,0,1},{0,0,1},{0,0,1} };
        ref_mat tf[3], tb[3];
        for (int q = 0; q < 3; q++) {
            int i = fan_tris[t][q];
            tp[q][0] = fpos[i][0]; tp[q][1] = fpos[i][1]; tp[q][2] = fpos[i][2];
            tf[q] = ff[i]; tb[q] = BLACK_MAT;
        }
        make_ref_triangle(tp, tn, tf, tb, rv);
        ref_light L = { .on = 0 };
        float fcols[3][4], bcols[3][4];
        for (int i = 0; i < 3; i++)
            gouraud_vertex_colors(&rv[i], &L, 0, fcols[i], bcols[i]);
        int cx, cy; float fx, fy;
        triangle_centroid(rv, &cx, &cy, &fx, &fy);
        float ref[4];
        smooth_ref_pixel(rv, fcols, bcols, 0, fx, fy, ref);
        unsigned char p[4];
        read_fb(cx, cy, p);
        TASK_CHECK(pixel_matches(p, ref));
    }
    kill_ctx(c);
    return 0;
}

/* ================================================================== */
/* clipping group                                                      */
/* ================================================================== */

/* Reference near-plane (z >= -w, w = 1) Sutherland-Hodgman step.
 * Boundary vertices inherit interpolated attributes by clip parameter. */
typedef struct {
    float pos[3];
    float emi[3];
} clip_ref_vert;

static int near_clip_ref(const clip_ref_vert *in, int n, clip_ref_vert *out)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const clip_ref_vert *cur = &in[i];
        const clip_ref_vert *prv = &in[(i + n - 1) % n];
        float dcur = cur->pos[2] + 1.0f;   /* z + w, w = 1 */
        float dprv = prv->pos[2] + 1.0f;
        if (dprv >= 0.0f && dcur >= 0.0f) {
            out[m++] = *cur;
        } else if (dprv >= 0.0f && dcur < 0.0f) {
            float t = dprv / (dprv - dcur);
            clip_ref_vert q;
            for (int k = 0; k < 3; k++) {
                q.pos[k] = prv->pos[k] + t * (cur->pos[k] - prv->pos[k]);
                q.emi[k] = prv->emi[k] + t * (cur->emi[k] - prv->emi[k]);
            }
            out[m++] = q;
        } else if (dprv < 0.0f && dcur >= 0.0f) {
            float t = dprv / (dprv - dcur);
            clip_ref_vert q;
            for (int k = 0; k < 3; k++) {
                q.pos[k] = prv->pos[k] + t * (cur->pos[k] - prv->pos[k]);
                q.emi[k] = prv->emi[k] + t * (cur->emi[k] - prv->emi[k]);
            }
            out[m++] = q;
            out[m++] = *cur;
        }
    }
    return m;
}

static void clip_ref_screen(const clip_ref_vert *v, float *sx, float *sy)
{
    ndc_pixel(v->pos[0], v->pos[1], sx, sy);
}

/* GL_FLAT where the provoking vertex is clipped away: every surviving
 * fragment must still carry its original blue emission. */
static int case_flat_provoking_clipped(void)
{
    GLState *c = make_ctx();
    base_lighting();
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    glShadeModel(GL_FLAT);
    clear_fb();

    /* The third vertex is beyond the near plane (z < -w) with positive w. */
    glBegin(GL_TRIANGLES);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_RED);
    glMaterialfv(GL_BACK, GL_EMISSION, COL_GREEN);
    glVertex3f(-0.8f, -0.8f, 0.0f);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_GREEN);
    glMaterialfv(GL_BACK, GL_EMISSION, COL_RED);
    glVertex3f(0.8f, -0.8f, 0.0f);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_BLUE);
    glMaterialfv(GL_BACK, GL_EMISSION, COL_BLUE);
    glVertex3f(0.0f, 0.2f, -1.5f);
    glEnd();

    /* Independently clip the same triangle and test every fan centroid. */
    clip_ref_vert in[3], clipped[8];
    float pin[3][3] = { {-0.8f,-0.8f,0.0f}, {0.8f,-0.8f,0.0f}, {0.0f,0.2f,-1.5f} };
    float emi[3][3] = { {1,0,0},{0,1,0},{0,0,1} };
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) { in[i].pos[k] = pin[i][k]; in[i].emi[k] = emi[i][k]; }
    }
    int m = near_clip_ref(in, 3, clipped);
    TASK_CHECK(m >= 3);

    for (int j = 1; j + 1 < m; j++) {
        float s0[2], s1[2], s2[2];
        clip_ref_screen(&clipped[0], &s0[0], &s0[1]);
        clip_ref_screen(&clipped[j], &s1[0], &s1[1]);
        clip_ref_screen(&clipped[j+1], &s2[0], &s2[1]);
        int cx = (int)((s0[0] + s1[0] + s2[0]) / 3.0f);
        int cy = (int)((s0[1] + s1[1] + s2[1]) / 3.0f);
        unsigned char p[4];
        read_fb(cx, cy, p);
        TASK_CHECK(p[2] >= 250 && p[0] <= 4 && p[1] <= 4);
    }
    kill_ctx(c);
    return 0;
}

/* Smooth shading through the near clip plane: boundary vertices carry
 * interpolated emission and the surviving region keeps its gradient. */
static int case_smooth_clipped_gradient(void)
{
    GLState *c = make_ctx();
    base_lighting();
    glShadeModel(GL_SMOOTH);
    clear_fb();

    float pos[3][3] = { {-0.8f,-0.8f,0.0f}, {0.8f,-0.8f,0.0f}, {0.0f,0.2f,-1.5f} };
    float emi[3][3] = { {1,0,0},{0,1,0},{0,0,1} };

    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        float e[4] = { emi[i][0], emi[i][1], emi[i][2], 1.0f };
        glMaterialfv(GL_FRONT, GL_AMBIENT, COL_ZERO);
        glMaterialfv(GL_FRONT, GL_DIFFUSE, COL_ZERO);
        glMaterialfv(GL_FRONT, GL_SPECULAR, COL_ZERO);
        glMaterialfv(GL_FRONT, GL_EMISSION, e);
        glVertex3f(pos[i][0], pos[i][1], pos[i][2]);
    }
    glEnd();

    clip_ref_vert in[3], clipped[8];
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) { in[i].pos[k] = pos[i][k]; in[i].emi[k] = emi[i][k]; }
    }
    int m = near_clip_ref(in, 3, clipped);
    TASK_CHECK(m >= 3);

    float first_ref[3] = { -1, -1, -1 };
    for (int j = 1; j + 1 < m; j++) {
        float s0[2], s1[2], s2[2];
        clip_ref_screen(&clipped[0], &s0[0], &s0[1]);
        clip_ref_screen(&clipped[j], &s1[0], &s1[1]);
        clip_ref_screen(&clipped[j+1], &s2[0], &s2[1]);
        /* The engine shades integer pixel centers, so truncate before
         * evaluating the barycentric reference. */
        int fx_i = (int)((s0[0] + s1[0] + s2[0]) / 3.0f);
        int fy_i = (int)((s0[1] + s1[1] + s2[1]) / 3.0f);
        float fx = (float)fx_i, fy = (float)fy_i;
        float ba, bb, bc;
        screen_bary(fx, fy, s0[0], s0[1], s1[0], s1[1], s2[0], s2[1], &ba, &bb, &bc);
        float ref[3];
        for (int ch = 0; ch < 3; ch++)
            ref[ch] = ba * clipped[0].emi[ch] + bb * clipped[j].emi[ch]
                    + bc * clipped[j+1].emi[ch];
        unsigned char p[4];
        read_fb(fx_i, fy_i, p);
        TASK_CHECK(byte_near(p[0], ref[0]) && byte_near(p[1], ref[1]) && byte_near(p[2], ref[2]));
        if (first_ref[0] < -0.5f) { first_ref[0] = ref[0]; first_ref[1] = ref[1]; first_ref[2] = ref[2]; }
    }

    /* Two separated covered samples must still show different colors
     * (red-dominant vs green-dominant sides of the surviving quad). */
    unsigned char pa[4], pb[4];
    read_fb(20, 50, pa);
    read_fb(44, 50, pb);
    TASK_CHECK(abs((int)pa[0] - (int)pb[0]) + abs((int)pa[1] - (int)pb[1])
               + abs((int)pa[2] - (int)pb[2]) > 60);
    TASK_CHECK(pa[0] > pa[1] && pb[1] > pb[0]);
    kill_ctx(c);
    return 0;
}

/* Unequal w under perspective: Phong material interpolation must follow
 * the 1/w-weighted perspective rule, not affine screen weights. */
static int case_unequal_w_phong(void)
{
    GLState *c = make_ctx();
    base_lighting();
    glShadeModel(GL_PHONG);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 20.0);
    glMatrixMode(GL_MODELVIEW);
    clear_fb();

    float pos[3][3]  = { {-1.2f,-0.9f,-2.0f}, {1.2f,-0.9f,-2.0f}, {0.0f,0.9f,-8.0f} };
    float norm[3][3] = { {0,0,1},{0,0,1},{0,0,1} };
    const float *ems[3] = { COL_RED, COL_GREEN, COL_BLUE };
    ref_mat fm[3], bm[3];

    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        emissive_both(GL_FRONT, ems[i], &fm[i]);
        bm[i] = BLACK_MAT;
        glNormal3f(norm[i][0], norm[i][1], norm[i][2]);
        glVertex3f(pos[i][0], pos[i][1], pos[i][2]);
    }
    glEnd();

    /* Reference NDC for the symmetric glFrustum above:
     * clip.x = eye.x * near/(-eye.z), clip.w = -eye.z, so
     * ndc = clip / w = eye.xy * near / (-eye.z)^2... equivalently
     * ndc.xy = eye.xy / clip.w directly. */
    ref_vertex rv[3];
    float ndc[3][2];
    for (int i = 0; i < 3; i++) {
        rv[i].eye = (rvec3){ pos[i][0], pos[i][1], pos[i][2] };
        rv[i].n   = (rvec3){ 0, 0, 1 };
        rv[i].front = fm[i];
        rv[i].back  = bm[i];
        rv[i].w = -pos[i][2];
        ndc[i][0] = pos[i][0] / rv[i].w;
        ndc[i][1] = pos[i][1] / rv[i].w;
    }

    float s[3][2], iw[3];
    for (int i = 0; i < 3; i++) {
        ndc_pixel(ndc[i][0], ndc[i][1], &s[i][0], &s[i][1]);
        iw[i] = 1.0f / rv[i].w;
    }

    int probes[3][2] = { {32, 40}, {24, 42}, {40, 42} };
    for (int q = 0; q < 3; q++) {
        float px = (float)probes[q][0], py = (float)probes[q][1];
        float ba, bb, bc, k0, k1, k2;
        screen_bary(px, py, s[0][0], s[0][1], s[1][0], s[1][1],
                    s[2][0], s[2][1], &ba, &bb, &bc);
        persp_weights(ba, bb, bc, iw[0], iw[1], iw[2], &k0, &k1, &k2);

        float ref[3] = {0, 0, 0};
        float kk[3] = { k0, k1, k2 };
        for (int i = 0; i < 3; i++)
            for (int ch = 0; ch < 3; ch++)
                ref[ch] += kk[i] * fm[i].emi[ch];

        unsigned char p[4];
        read_fb(probes[q][0], probes[q][1], p);
        TASK_CHECK(byte_near(p[0], ref[0]) && byte_near(p[1], ref[1]) && byte_near(p[2], ref[2]));

        /* The affine answer must be visibly wrong at the same fragment. */
        float aff[3] = {
            ba * 1.0f, bb * 1.0f, bc * 1.0f
        };
        int diff = 0;
        diff += abs((int)to_byte(aff[0]) - (int)p[0]);
        diff += abs((int)to_byte(aff[1]) - (int)p[1]);
        diff += abs((int)to_byte(aff[2]) - (int)p[2]);
        TASK_CHECK(diff > 8);
    }
    kill_ctx(c);
    return 0;
}

/* Display list replay combined with a clipped flat triangle: the source
 * provoking color survives both clipping and a second list execution. */
static int case_flat_clipped_list_replay(void)
{
    GLState *c = make_ctx();
    base_lighting();
    glShadeModel(GL_FLAT);

    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    glMaterialfv(GL_FRONT, GL_AMBIENT, COL_ZERO);
    glMaterialfv(GL_FRONT, GL_DIFFUSE, COL_ZERO);
    glMaterialfv(GL_FRONT, GL_SPECULAR, COL_ZERO);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_RED);
    glVertex3f(-0.8f, -0.8f, 0.0f);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_GREEN);
    glVertex3f(0.8f, -0.8f, 0.0f);
    glMaterialfv(GL_FRONT, GL_EMISSION, COL_BLUE);
    glVertex3f(0.0f, 0.2f, -1.5f);
    glEnd();
    glEndList();

    unsigned char run1[4], run2[4];
    clear_fb();
    glCallList(list);
    read_fb(32, 42, run1);
    TASK_CHECK(run1[2] >= 250 && run1[0] <= 4 && run1[1] <= 4);

    glMaterialfv(GL_FRONT, GL_EMISSION, COL_WHITE);
    clear_fb();
    glCallList(list);
    read_fb(32, 42, run2);
    TASK_CHECK(memcmp(run1, run2, 4) == 0);
    glDeleteLists(list, 1);
    kill_ctx(c);
    return 0;
}

/* ================================================================== */
/* Registration                                                        */
/* ================================================================== */

const struct task_case task_cases[] = {
    { "capture",  "emission_front_smooth",            case_front_smooth },
    { "capture",  "emission_back_smooth",             case_back_smooth },
    { "capture",  "emission_phong_material_interp",   case_emission_phong },
    { "capture",  "material_state_query",             case_material_state_query },
    { "capture",  "color_material_tracking",          case_color_material_tracking },
    { "capture",  "display_list_replay",              case_display_list_replay },

    { "lighting", "point_light_diffuse_two_side",     case_point_light_diffuse },
    { "lighting", "specular_phong_vs_gouraud",        case_specular_phong_vs_gouraud },
    { "lighting", "flat_provoking_vertex",            case_flat_provoking_vertex },
    { "lighting", "two_side_back_material",           case_two_side_materials },
    { "lighting", "phong_two_side_back_material",     case_phong_two_side_back },
    { "lighting", "front_face_switch",                case_front_face_switch },
    { "lighting", "draw_arrays_color_material",       case_draw_arrays_entry },
    { "lighting", "draw_elements_color_material",     case_draw_elements_entry },
    { "lighting", "triangle_strip_fan_materials",     case_strip_and_fan },

    { "clipping", "flat_provoking_vertex_clipped",    case_flat_provoking_clipped },
    { "clipping", "smooth_near_clip_gradient",        case_smooth_clipped_gradient },
    { "clipping", "unequal_w_phong_perspective",      case_unequal_w_phong },
    { "clipping", "flat_clipped_display_list_replay", case_flat_clipped_list_replay },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
