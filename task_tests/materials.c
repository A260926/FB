/*
 * materials.c - Per-vertex material ownership regression tests.
 *
 * Covers GL_FILL triangle primitives (GL_TRIANGLES, GL_TRIANGLE_STRIP,
 * GL_TRIANGLE_FAN) through immediate mode, glDrawArrays/glDrawElements and
 * display list playback. Every expected pixel is computed by an independent
 * reference implementation (own vector/lighting math, own barycentric and
 * perspective-correct weights); the two engine entry points are never
 * compared against each other alone.
 */

#include "mytinygl.h"
#include "task_support/check.h"
#include <math.h>
#include <string.h>

#define RW 64
#define RH 64

/* ------------------------------------------------------------------ */
/* Small independent math + reference lighting                        */
/* ------------------------------------------------------------------ */

typedef struct { float x, y, z; } rvec3;

static rvec3 rv3(float x, float y, float z) { return (rvec3){x, y, z}; }
static rvec3 rsub(rvec3 a, rvec3 b) { return rv3(a.x-b.x, a.y-b.y, a.z-b.z); }
static rvec3 radd(rvec3 a, rvec3 b) { return rv3(a.x+b.x, a.y+b.y, a.z+b.z); }
static rvec3 rscale(rvec3 a, float s) { return rv3(a.x*s, a.y*s, a.z*s); }
static float rdot(rvec3 a, rvec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static float rlen(rvec3 a) { return sqrtf(rdot(a, a)); }
static rvec3 rnorm(rvec3 a) { float l = rlen(a); return l > 0.0f ? rscale(a, 1.0f/l) : a; }

typedef struct {
    float amb[4], dif[4], spe[4], emi[4];
    float shininess;
} rmat;

static rmat rmat_black(float shininess)
{
    rmat m;
    memset(&m, 0, sizeof(m));
    m.amb[3] = m.dif[3] = m.spe[3] = m.emi[3] = 1.0f;
    m.shininess = shininess;
    return m;
}

/* One white positional point light at (0, 0, 2), attenuation 1, no spot.
 * The engine is configured with exactly the same parameters. */
static const rvec3 ref_light_pos = {0.0f, 0.0f, 2.0f};

/* Independent re-implementation of the fixed-function lighting math.
 * Global/light ambient are configured to zero in the lighting cases. */
static void ref_light(rvec3 eye_pos, rvec3 n, const rmat *m, int do_normalize,
                      float out[3])
{
    if (do_normalize) n = rnorm(n);
    out[0] = m->emi[0]; out[1] = m->emi[1]; out[2] = m->emi[2];

    rvec3 L = rnorm(rsub(ref_light_pos, eye_pos));
    float ndl = rdot(n, L);
    if (ndl > 0.0f) {
        out[0] += m->dif[0] * ndl;
        out[1] += m->dif[1] * ndl;
        out[2] += m->dif[2] * ndl;
        if (m->shininess > 0.0f) {
            rvec3 V = rv3(0.0f, 0.0f, 1.0f); /* non-local-viewer default */
            rvec3 halfv = rnorm(radd(L, V));
            float ndh = rdot(n, halfv);
            if (ndh > 0.0f) {
                float s = powf(ndh, m->shininess);
                out[0] += m->spe[0] * s;
                out[1] += m->spe[1] * s;
                out[2] += m->spe[2] * s;
            }
        }
    }
    for (int i = 0; i < 3; i++) {
        if (out[i] < 0.0f) out[i] = 0.0f;
        if (out[i] > 1.0f) out[i] = 1.0f;
    }
}

/* Reference vertex: object position, normalized eye normal, front/back
 * materials, pre-lit Gouraud colors, and clip-space w (identity modelview). */
typedef struct {
    rvec3 pos;          /* eye-space position */
    rvec3 normal;       /* eye-space normal, normalized */
    rmat front, back;
    float gouraud_front[3];
    float gouraud_back[3];
    float clip_w;
} rvertex;

static void rvertex_lit(rvertex *v, int two_side, int normalize)
{
    ref_light(v->pos, v->normal, &v->front, normalize, v->gouraud_front);
    if (two_side) {
        rvec3 flipped = rscale(v->normal, -1.0f);
        ref_light(v->pos, flipped, &v->back, normalize, v->gouraud_back);
    } else {
        memcpy(v->gouraud_back, v->gouraud_front, sizeof(v->gouraud_front));
    }
}

static float ref_edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

/* Screen affine barycentrics, matching the engine's edge function layout. */
static void ref_bary(const float s[6], float px, float py, float b[3])
{
    float area = ref_edge(s[0], s[1], s[2], s[3], s[4], s[5]);
    b[0] = ref_edge(s[2], s[3], s[4], s[5], px, py) / area;
    b[1] = ref_edge(s[4], s[5], s[0], s[1], px, py) / area;
    b[2] = ref_edge(s[0], s[1], s[2], s[3], px, py) / area;
}

/* Screen mapping matching the engine's ndc_to_screen, including its integer
 * truncation: barycentrics must be computed on the same integer vertex
 * coordinates the rasterizer uses. */
static void ref_project(float ndc_x, float ndc_y, float out[2])
{
    out[0] = (float)(int32_t)((ndc_x + 1.0f) * 0.5f * RW);
    out[1] = (float)(int32_t)((1.0f - ndc_y) * 0.5f * RH);
}

/* ------------------------------------------------------------------ */
/* Engine helpers                                                      */
/* ------------------------------------------------------------------ */

static GLState *new_ctx(void)
{
    GLState *c = gl_create_context(RW, RH);
    gl_make_current(c);
    glViewport(0, 0, RW, RH);
    return c;
}

static void end_ctx(GLState *c)
{
    gl_make_current(NULL);
    gl_destroy_context(c);
}

static void clear_bufs(void)
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* Read a fragment in framebuffer coordinates (the engine flips Y in
 * glReadPixels; the reference works in framebuffer rows directly). */
static void read_fb(int sx, int sy, unsigned char p[4])
{
    glReadPixels(sx, RH - 1 - sy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
}

static int channel_close(unsigned char got, float want, int tol)
{
    float w = want * 255.0f;
    return fabsf((float)got - w) <= (float)tol;
}

static int color_close(const unsigned char p[4], const float want[3], int tol)
{
    return channel_close(p[0], want[0], tol) &&
           channel_close(p[1], want[1], tol) &&
           channel_close(p[2], want[2], tol);
}

static void upload_mat(GLenum face, const rmat *m)
{
    glMaterialfv(face, GL_AMBIENT, m->amb);
    glMaterialfv(face, GL_DIFFUSE, m->dif);
    glMaterialfv(face, GL_SPECULAR, m->spe);
    glMaterialfv(face, GL_EMISSION, m->emi);
    glMaterialf(face, GL_SHININESS, m->shininess);
}

static void lighting_base(void)
{
    static const float zero[4] = {0, 0, 0, 1};
    static const float white[4] = {1, 1, 1, 1};
    static const float light_pos[4] = {0, 0, 2, 1};
    glEnable(GL_LIGHTING);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zero);
    glLightfv(GL_LIGHT0, GL_AMBIENT, zero);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, white);
    glLightfv(GL_LIGHT0, GL_SPECULAR, white);
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
    glEnable(GL_LIGHT0);
}

/* ------------------------------------------------------------------ */
/* capture group                                                       */
/* ------------------------------------------------------------------ */

/* Emission-only triangle, red/green/blue vertices, two-sided lighting.
 * Every fragment must be the affine-interpolated per-vertex emissions. */
static int case_emission_gradient(void)
{
    GLState *c = new_ctx();
    TASK_CHECK(c != NULL);
    static const float r[4] = {1, 0, 0, 1}, g[4] = {0, 1, 0, 1}, b[4] = {0, 0, 1, 1};

    glEnable(GL_LIGHTING);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    static const float zero[4] = {0, 0, 0, 1};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zero);
    glShadeModel(GL_SMOOTH);
    clear_bufs();

    glBegin(GL_TRIANGLES);
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, r);
        glVertex3f(-0.8f, -0.8f, 0.0f);
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, g);
        glVertex3f(0.8f, -0.8f, 0.0f);
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, b);
        glVertex3f(0.0f, 0.8f, 0.0f);
    glEnd();

    /* Independent reference: identity projection, w = 1 everywhere. */
    float s[6];
    ref_project(-0.8f, -0.8f, &s[0]);
    ref_project(0.8f, -0.8f, &s[2]);
    ref_project(0.0f, 0.8f, &s[4]);
    float emi[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    int samples[][2] = {{32, 40}, {20, 36}, {44, 36}, {32, 20}};
    for (unsigned k = 0; k < sizeof(samples)/sizeof(samples[0]); k++) {
        float bb[3], want[3] = {0, 0, 0};
        ref_bary(s, (float)samples[k][0], (float)samples[k][1], bb);
        for (int ch = 0; ch < 3; ch++)
            want[ch] = bb[0]*emi[0][ch] + bb[1]*emi[1][ch] + bb[2]*emi[2][ch];
        unsigned char p[4];
        read_fb(samples[k][0], samples[k][1], p);
        TASK_CHECK(color_close(p, want, 3));
    }

    /* Center sample must contain all three channels (the original bug made
     * back/phong paths monochrome). */
    unsigned char center[4];
    read_fb(32, 32, center);
    TASK_CHECK(center[0] > 40 && center[1] > 40 && center[2] > 40);

    end_ctx(c);
    return 0;
}

/* Same gradient through immediate mode, glDrawArrays, glDrawElements and a
 * twice-replayed display list; all must match the independent reference. */
static int case_four_entries(void)
{
    GLState *c = new_ctx();
    TASK_CHECK(c != NULL);

    static const float verts[9] = {
        -0.8f, -0.8f, 0.0f,
         0.8f, -0.8f, 0.0f,
         0.0f,  0.8f, 0.0f
    };
    static const float colors[12] = {
        1, 0, 0, 1,
        0, 1, 0, 1,
        0, 0, 1, 1
    };
    static const GLubyte indices[3] = {0, 1, 2};

    glEnable(GL_LIGHTING);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    static const float zero[4] = {0, 0, 0, 1};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zero);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_EMISSION);
    glShadeModel(GL_SMOOTH);

    float s[6];
    ref_project(-0.8f, -0.8f, &s[0]);
    ref_project(0.8f, -0.8f, &s[2]);
    ref_project(0.0f, 0.8f, &s[4]);
    int samples[][2] = {{32, 40}, {22, 34}, {42, 34}, {32, 22}};
    float ref_colors[4][3];
    for (unsigned k = 0; k < 4; k++) {
        float bb[3];
        ref_bary(s, (float)samples[k][0], (float)samples[k][1], bb);
        for (int ch = 0; ch < 3; ch++)
            ref_colors[k][ch] = bb[0]*colors[ch] + bb[1]*colors[4+ch] + bb[2]*colors[8+ch];
    }

    unsigned char shots[4][4][4];

    /* 1. Immediate mode */
    clear_bufs();
    glBegin(GL_TRIANGLES);
        glColor4f(1, 0, 0, 1); glVertex3f(verts[0], verts[1], verts[2]);
        glColor4f(0, 1, 0, 1); glVertex3f(verts[3], verts[4], verts[5]);
        glColor4f(0, 0, 1, 1); glVertex3f(verts[6], verts[7], verts[8]);
    glEnd();
    for (unsigned k = 0; k < 4; k++) read_fb(samples[k][0], samples[k][1], shots[0][k]);

    /* 2. glDrawArrays with a color array */
    glVertexPointer(3, GL_FLOAT, 0, verts);
    glColorPointer(4, GL_FLOAT, 0, colors);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    clear_bufs();
    glDrawArrays(GL_TRIANGLES, 0, 3);
    for (unsigned k = 0; k < 4; k++) read_fb(samples[k][0], samples[k][1], shots[1][k]);

    /* 3. glDrawElements */
    clear_bufs();
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_BYTE, indices);
    for (unsigned k = 0; k < 4; k++) read_fb(samples[k][0], samples[k][1], shots[2][k]);

    /* 4. Display list, compiled then played back twice */
    GLuint list = glGenLists(1);
    TASK_CHECK(list != 0);
    glNewList(list, GL_COMPILE);
        glBegin(GL_TRIANGLES);
            glColor4f(1, 0, 0, 1); glVertex3f(verts[0], verts[1], verts[2]);
            glColor4f(0, 1, 0, 1); glVertex3f(verts[3], verts[4], verts[5]);
            glColor4f(0, 0, 1, 1); glVertex3f(verts[6], verts[7], verts[8]);
        glEnd();
    glEndList();
    clear_bufs();
    glCallList(list);
    for (unsigned k = 0; k < 4; k++) read_fb(samples[k][0], samples[k][1], shots[3][k]);
    /* Second playback must reproduce the first exactly. */
    clear_bufs();
    glCallList(list);
    for (unsigned k = 0; k < 4; k++) {
        unsigned char again[4];
        read_fb(samples[k][0], samples[k][1], again);
        TASK_CHECK(memcmp(again, shots[3][k], 4) == 0);
    }

    for (unsigned k = 0; k < 4; k++) {
        TASK_CHECK(color_close(shots[0][k], ref_colors[k], 3));
        TASK_CHECK(color_close(shots[1][k], ref_colors[k], 3));
        TASK_CHECK(color_close(shots[2][k], ref_colors[k], 3));
        TASK_CHECK(color_close(shots[3][k], ref_colors[k], 3));
        /* Entries must agree with each other as well. */
        TASK_CHECK(memcmp(shots[0][k], shots[1][k], 3) == 0);
        TASK_CHECK(memcmp(shots[0][k], shots[2][k], 3) == 0);
        TASK_CHECK(memcmp(shots[0][k], shots[3][k], 3) == 0);
    }

    end_ctx(c);
    return 0;
}

/* Per-vertex materials on triangle strip and fan primitives. */
static int case_strip_fan(void)
{
    GLState *c = new_ctx();
    TASK_CHECK(c != NULL);

    static const float emi[4][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}};
    static const float strip_v[12] = {
        -0.9f, -0.6f, 0,
        -0.3f, -0.6f, 0,
         0.3f,  0.6f, 0,
         0.9f,  0.6f, 0
    };
    glEnable(GL_LIGHTING);
    static const float zero[4] = {0, 0, 0, 1};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zero);
    glShadeModel(GL_SMOOTH);
    clear_bufs();

    glBegin(GL_TRIANGLE_STRIP);
    for (int i = 0; i < 4; i++) {
        float e[4] = {emi[i][0], emi[i][1], emi[i][2], 1};
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, e);
        glVertex3f(strip_v[i*3], strip_v[i*3+1], strip_v[i*3+2]);
    }
    glEnd();

    /* Reference for both strip sub-triangles. */
    float scr[4][2];
    for (int i = 0; i < 4; i++) ref_project(strip_v[i*3], strip_v[i*3+1], scr[i]);
    /* First strip triangle: vertices 0,1,2 (CCW order). */
    float s0[6] = {scr[0][0], scr[0][1], scr[1][0], scr[1][1], scr[2][0], scr[2][1]};
    float cent0[2] = {(s0[0]+s0[2]+s0[4])/3.0f, (s0[1]+s0[3]+s0[5])/3.0f};
    int sx0 = (int)cent0[0], sy0 = (int)cent0[1];
    float b0[3], want0[3] = {0, 0, 0};
    ref_bary(s0, (float)sx0, (float)sy0, b0);
    for (int ch = 0; ch < 3; ch++)
        want0[ch] = b0[0]*emi[0][ch] + b0[1]*emi[1][ch] + b0[2]*emi[2][ch];
    unsigned char p0[4];
    read_fb(sx0, sy0, p0);
    TASK_CHECK(color_close(p0, want0, 3));

    /* Second strip triangle covers submitted vertices 1, 2, 3. Its screen
     * geometry is independent of the even/odd winding swap, so check its
     * material gradient against those three vertices directly. */
    float s1[6] = {scr[1][0], scr[1][1], scr[2][0], scr[2][1], scr[3][0], scr[3][1]};
    float cent1[2] = {(s1[0]+s1[2]+s1[4])/3.0f, (s1[1]+s1[3]+s1[5])/3.0f};
    int sx1 = (int)cent1[0], sy1 = (int)cent1[1];
    float b1[3], want1[3] = {0, 0, 0};
    ref_bary(s1, (float)sx1, (float)sy1, b1);
    for (int ch = 0; ch < 3; ch++)
        want1[ch] = b1[0]*emi[1][ch] + b1[1]*emi[2][ch] + b1[2]*emi[3][ch];
    unsigned char p1s[4];
    read_fb(sx1, sy1, p1s);
    TASK_CHECK(color_close(p1s, want1, 3));

    /* Fan primitive with the same kind of gradient. */
    static const float fan_v[12] = {
        0.0f, 0.0f, 0,
        -0.9f, -0.7f, 0,
        -0.4f, -0.9f, 0,
         0.4f, -0.9f, 0
    };
    clear_bufs();
    glBegin(GL_TRIANGLE_FAN);
    for (int i = 0; i < 4; i++) {
        float e[4] = {emi[i][0], emi[i][1], emi[i][2], 1};
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, e);
        glVertex3f(fan_v[i*3], fan_v[i*3+1], fan_v[i*3+2]);
    }
    glEnd();

    float fscr[4][2];
    for (int i = 0; i < 4; i++) ref_project(fan_v[i*3], fan_v[i*3+1], fscr[i]);
    /* First fan triangle: 0,1,2. */
    float fs[6] = {fscr[0][0], fscr[0][1], fscr[1][0], fscr[1][1], fscr[2][0], fscr[2][1]};
    float fcent[2] = {(fs[0]+fs[2]+fs[4])/3.0f, (fs[1]+fs[3]+fs[5])/3.0f};
    int fsx = (int)fcent[0], fsy = (int)fcent[1];
    float fb[3], fwant[3] = {0, 0, 0};
    ref_bary(fs, (float)fsx, (float)fsy, fb);
    for (int ch = 0; ch < 3; ch++)
        fwant[ch] = fb[0]*emi[0][ch] + fb[1]*emi[1][ch] + fb[2]*emi[2][ch];
    unsigned char fp[4];
    read_fb(fsx, fsy, fp);
    TASK_CHECK(color_close(fp, fwant, 3));

    end_ctx(c);
    return 0;
}

/* Material changes made after the last glVertex but before glEnd must not
 * touch the submitted batch. The old back/phong paths read the final
 * material at glEnd time. */
static int case_material_after_last_vertex(void)
{
    GLState *c = new_ctx();
    TASK_CHECK(c != NULL);
    static const float r[4] = {1, 0, 0, 1}, g[4] = {0, 1, 0, 1}, b[4] = {0, 0, 1, 1};
    static const float yellow[4] = {1, 1, 0, 1};

    glEnable(GL_LIGHTING);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    static const float zero[4] = {0, 0, 0, 1};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zero);

    /* Per-vertex emission in submission order (red, green, blue). */
    float emi[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    GLenum modes[3] = {GL_SMOOTH, GL_SMOOTH, GL_PHONG};
    int back_winding[3] = {0, 1, 0};
    for (int k = 0; k < 3; k++) {
        /* Positions follow the actual submission order of each pass. */
        float px[3], py[3];
        px[0] = -0.8f; py[0] = -0.8f;
        if (back_winding[k]) {
            px[1] = 0.0f; py[1] = 0.8f;
            px[2] = 0.8f; py[2] = -0.8f;
        } else {
            px[1] = 0.8f; py[1] = -0.8f;
            px[2] = 0.0f; py[2] = 0.8f;
        }
        float s[6];
        ref_project(px[0], py[0], &s[0]);
        ref_project(px[1], py[1], &s[2]);
        ref_project(px[2], py[2], &s[4]);

        glShadeModel(modes[k]);
        clear_bufs();
        glBegin(GL_TRIANGLES);
            glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, r);
            glVertex3f(px[0], py[0], 0.0f);
            glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, g);
            glVertex3f(px[1], py[1], 0.0f);
            glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, b);
            glVertex3f(px[2], py[2], 0.0f);
            /* Too late: no vertex follows this change. */
            glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, yellow);
        glEnd();

        float bb[3], want[3] = {0, 0, 0};
        ref_bary(s, 32.0f, 32.0f, bb);
        for (int ch = 0; ch < 3; ch++)
            want[ch] = bb[0]*emi[0][ch] + bb[1]*emi[1][ch] + bb[2]*emi[2][ch];
        unsigned char p[4];
        read_fb(32, 32, p);
        TASK_CHECK(color_close(p, want, 3));
        TASK_CHECK(!(p[0] > 200 && p[1] > 200 && p[2] < 60)); /* not yellow */
    }

    end_ctx(c);
    return 0;
}

/* glGetMaterial must keep reporting the current tracked state, including
 * GL_COLOR_MATERIAL updates, with correct face selection. */
static int case_getmaterial_state(void)
{
    GLState *c = new_ctx();
    TASK_CHECK(c != NULL);

    static const float m[4] = {0.1f, 0.2f, 0.3f, 1};
    glMaterialfv(GL_FRONT, GL_DIFFUSE, m);
    float got[4] = {0};
    glGetMaterialfv(GL_FRONT, GL_DIFFUSE, got);
    TASK_CHECK(fabsf(got[0]-0.1f) < 1e-6f && fabsf(got[1]-0.2f) < 1e-6f &&
               fabsf(got[2]-0.3f) < 1e-6f && fabsf(got[3]-1.0f) < 1e-6f);

    static const float zero[4] = {0, 0, 0, 1};
    glEnable(GL_LIGHTING);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zero);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_BACK, GL_EMISSION);

    static const float front_emi[4] = {0.4f, 0.5f, 0.6f, 1};
    glMaterialfv(GL_FRONT, GL_EMISSION, front_emi);

    glBegin(GL_TRIANGLES);
        glColor4f(0.2f, 0.4f, 0.8f, 1.0f);
        glVertex3f(-0.5f, -0.5f, 0);
        glColor4f(0.9f, 0.1f, 0.2f, 1.0f);
        glVertex3f(0.5f, -0.5f, 0);
        glColor4f(0.3f, 0.7f, 0.3f, 1.0f);
        glVertex3f(0.0f, 0.5f, 0);
    glEnd();

    /* Back emission now tracks the last glColor; front emission is untouched. */
    glGetMaterialfv(GL_BACK, GL_EMISSION, got);
    TASK_CHECK(fabsf(got[0]-0.3f) < 1e-6f && fabsf(got[1]-0.7f) < 1e-6f &&
               fabsf(got[2]-0.3f) < 1e-6f);
    glGetMaterialfv(GL_FRONT, GL_EMISSION, got);
    TASK_CHECK(fabsf(got[0]-0.4f) < 1e-6f && fabsf(got[1]-0.5f) < 1e-6f &&
               fabsf(got[2]-0.6f) < 1e-6f);

    /* Face-specific selection must not leak to the other face: switch the
     * color material to front diffuse and verify back emission survives. */
    glColorMaterial(GL_FRONT, GL_DIFFUSE);
    glBegin(GL_TRIANGLES);
        glColor4f(0.11f, 0.22f, 0.33f, 1.0f);
        glVertex3f(-0.5f, -0.5f, 0);
        glVertex3f(0.5f, -0.5f, 0);
        glVertex3f(0.0f, 0.5f, 0);
    glEnd();
    glGetMaterialfv(GL_FRONT, GL_DIFFUSE, got);
    TASK_CHECK(fabsf(got[0]-0.11f) < 1e-6f && fabsf(got[1]-0.22f) < 1e-6f &&
               fabsf(got[2]-0.33f) < 1e-6f);
    glGetMaterialfv(GL_BACK, GL_EMISSION, got);
    TASK_CHECK(fabsf(got[0]-0.3f) < 1e-6f && fabsf(got[1]-0.7f) < 1e-6f &&
               fabsf(got[2]-0.3f) < 1e-6f);

    end_ctx(c);
    return 0;
}

/* ------------------------------------------------------------------ */
/* lighting group                                                      */
/* ------------------------------------------------------------------ */

static rvertex make_lit_vertex(float x, float y, float z,
                               float nx, float ny, float nz,
                               rmat front, rmat back,
                               int two_side, int normalize)
{
    rvertex v;
    v.pos = rv3(x, y, z);
    v.normal = rnorm(rv3(nx, ny, nz));
    v.front = front;
    v.back = back;
    v.clip_w = 1.0f;
    rvertex_lit(&v, two_side, normalize);
    return v;
}

static void project_rvertex(const rvertex *v, float out[2])
{
    /* Identity projection in the non-clipping lighting cases. */
    ref_project(v->pos.x, v->pos.y, out);
}

/* Point light with differing normals and per-vertex materials: Gouraud and
 * Phong must each match an independently computed reference, and must be
 * distinguishable (pre- vs post-interpolation lighting). */
static int case_point_light_modes(void)
{
    GLState *c = new_ctx();
    TASK_CHECK(c != NULL);
    lighting_base();
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_FALSE);

    rmat mats[3];
    mats[0] = rmat_black(0.0f); mats[0].dif[0] = 1.0f;            /* red   */
    mats[1] = rmat_black(0.0f); mats[1].dif[1] = 1.0f;            /* green */
    mats[2] = rmat_black(0.0f); mats[2].dif[2] = 1.0f;            /* blue  */
    float coords[9] = {-0.8f, -0.8f, 0.0f,
                        0.8f, -0.8f, 0.0f,
                        0.0f,  0.8f, 0.0f};
    float norms[9] = {0.0f, 0.0f, 1.0f,
                      1.0f, 0.0f, 0.0f,
                      0.0f, 1.0f, 0.0f};

    rvertex rv[3];
    for (int i = 0; i < 3; i++)
        rv[i] = make_lit_vertex(coords[i*3], coords[i*3+1], coords[i*3+2],
                                norms[i*3], norms[i*3+1], norms[i*3+2],
                                mats[i], mats[i], 0, 0);

    float s[6];
    for (int i = 0; i < 3; i++) project_rvertex(&rv[i], &s[i*2]);

    int samples[][2] = {{32, 40}, {24, 34}, {40, 30}};
    unsigned char gou[3][4], pho[3][4];
    float ref_gou[3][3], ref_pho[3][3];

    for (unsigned k = 0; k < 3; k++) {
        float b[3];
        ref_bary(s, (float)samples[k][0], (float)samples[k][1], b);

        /* Gouraud reference: light at vertices, affine interpolate colors. */
        for (int ch = 0; ch < 3; ch++)
            ref_gou[k][ch] = b[0]*rv[0].gouraud_front[ch]
                           + b[1]*rv[1].gouraud_front[ch]
                           + b[2]*rv[2].gouraud_front[ch];

        /* Phong reference: perspective weights (w=1 here, so equal to the
         * normalized barycentrics), interpolate inputs, then light. */
        rvec3 P = rv3(b[0]*rv[0].pos.x + b[1]*rv[1].pos.x + b[2]*rv[2].pos.x,
                      b[0]*rv[0].pos.y + b[1]*rv[1].pos.y + b[2]*rv[2].pos.y,
                      0.0f);
        rvec3 N = rv3(b[0]*rv[0].normal.x + b[1]*rv[1].normal.x + b[2]*rv[2].normal.x,
                      b[0]*rv[0].normal.y + b[1]*rv[1].normal.y + b[2]*rv[2].normal.y,
                      b[0]*rv[0].normal.z + b[1]*rv[1].normal.z + b[2]*rv[2].normal.z);
        rmat m;
        m = rmat_black(0.0f);
        for (int ch = 0; ch < 3; ch++) {
            m.dif[ch] = b[0]*rv[0].front.dif[ch] + b[1]*rv[1].front.dif[ch]
                      + b[2]*rv[2].front.dif[ch];
        }
        ref_light(P, N, &m, 0, ref_pho[k]);
    }

    /* Engine: Gouraud */
    glShadeModel(GL_SMOOTH);
    clear_bufs();
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        upload_mat(GL_FRONT_AND_BACK, &mats[i]);
        glNormal3f(norms[i*3], norms[i*3+1], norms[i*3+2]);
        glVertex3f(coords[i*3], coords[i*3+1], coords[i*3+2]);
    }
    glEnd();
    for (unsigned k = 0; k < 3; k++) read_fb(samples[k][0], samples[k][1], gou[k]);

    /* Engine: Phong */
    glShadeModel(GL_PHONG);
    clear_bufs();
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        upload_mat(GL_FRONT_AND_BACK, &mats[i]);
        glNormal3f(norms[i*3], norms[i*3+1], norms[i*3+2]);
        glVertex3f(coords[i*3], coords[i*3+1], coords[i*3+2]);
    }
    glEnd();
    for (unsigned k = 0; k < 3; k++) read_fb(samples[k][0], samples[k][1], pho[k]);

    int distinguishable = 0;
    for (unsigned k = 0; k < 3; k++) {
        TASK_CHECK(color_close(gou[k], ref_gou[k], 4));
        TASK_CHECK(color_close(pho[k], ref_pho[k], 4));
        float d = fabsf((float)gou[k][0]-pho[k][0]) +
                  fabsf((float)gou[k][1]-pho[k][1]) +
                  fabsf((float)gou[k][2]-pho[k][2]);
        if (d > 6.0f) distinguishable = 1;
    }
    TASK_CHECK(distinguishable);

    end_ctx(c);
    return 0;
}

/* Specular highlight: Phong interpolates the normal and keeps the highlight;
 * Gouraud can lose it between vertices. Both match their own reference. */
static int case_specular_highlight(void)
{
    GLState *c = new_ctx();
    TASK_CHECK(c != NULL);
    lighting_base();
    glEnable(GL_NORMALIZE);

    rmat m = rmat_black(32.0f);
    m.spe[0] = m.spe[1] = m.spe[2] = 1.0f;
    m.dif[0] = m.dif[1] = m.dif[2] = 0.1f;

    float coords[9] = {-0.8f, -0.8f, 0.0f,
                        0.8f, -0.8f, 0.0f,
                        0.0f,  0.8f, 0.0f};
    /* Symmetric tilted normals: each misses the half-vector at its vertex,
     * but their centroid average points straight at the light (+z). */
    float norms[9] = { 0.7071f,  0.0f,    0.7071f,
                      -0.3536f,  0.6124f, 0.7071f,
                      -0.3536f, -0.6124f, 0.7071f};

    rvertex rv[3];
    for (int i = 0; i < 3; i++)
        rv[i] = make_lit_vertex(coords[i*3], coords[i*3+1], coords[i*3+2],
                                norms[i*3], norms[i*3+1], norms[i*3+2],
                                m, m, 0, 1);
    float s[6];
    for (int i = 0; i < 3; i++) project_rvertex(&rv[i], &s[i*2]);

    int sx = 32, sy = 32;
    float b[3];
    ref_bary(s, (float)sx, (float)sy, b);
    float ref_gou[3];
    for (int ch = 0; ch < 3; ch++)
        ref_gou[ch] = b[0]*rv[0].gouraud_front[ch]
                    + b[1]*rv[1].gouraud_front[ch]
                    + b[2]*rv[2].gouraud_front[ch];

    rvec3 P = rv3(b[0]*rv[0].pos.x + b[1]*rv[1].pos.x + b[2]*rv[2].pos.x,
                  b[0]*rv[0].pos.y + b[1]*rv[1].pos.y + b[2]*rv[2].pos.y, 0.0f);
    rvec3 N = rv3(b[0]*rv[0].normal.x + b[1]*rv[1].normal.x + b[2]*rv[2].normal.x,
                  b[0]*rv[0].normal.y + b[1]*rv[1].normal.y + b[2]*rv[2].normal.y,
                  b[0]*rv[0].normal.z + b[1]*rv[1].normal.z + b[2]*rv[2].normal.z);
    float ref_pho[3];
    ref_light(P, N, &m, 1, ref_pho);
    /* Phong keeps a strong highlight at the centroid while Gouraud loses it. */
    TASK_CHECK(ref_pho[0] > 0.45f);
    TASK_CHECK(ref_gou[0] < 0.20f);

    unsigned char gou[4], pho[4];
    glShadeModel(GL_SMOOTH);
    clear_bufs();
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        upload_mat(GL_FRONT_AND_BACK, &m);
        glNormal3f(norms[i*3], norms[i*3+1], norms[i*3+2]);
        glVertex3f(coords[i*3], coords[i*3+1], coords[i*3+2]);
    }
    glEnd();
    read_fb(sx, sy, gou);
    TASK_CHECK(color_close(gou, ref_gou, 4));

    glShadeModel(GL_PHONG);
    clear_bufs();
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        upload_mat(GL_FRONT_AND_BACK, &m);
        glNormal3f(norms[i*3], norms[i*3+1], norms[i*3+2]);
        glVertex3f(coords[i*3], coords[i*3+1], coords[i*3+2]);
    }
    glEnd();
    read_fb(sx, sy, pho);
    TASK_CHECK(color_close(pho, ref_pho, 4));
    TASK_CHECK(pho[0] > gou[0] + 60);

    end_ctx(c);
    return 0;
}

/* GL_FLAT: the whole primitive takes the provoking (last submitted) vertex's
 * face-specific lit color, including strip sub-triangles. */
static int case_flat_provoking(void)
{
    GLState *c = new_ctx();
    TASK_CHECK(c != NULL);
    lighting_base();
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    glShadeModel(GL_FLAT);

    rmat mats[4];
    float emi[4][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}};
    for (int i = 0; i < 4; i++) {
        mats[i] = rmat_black(0.0f);
        mats[i].emi[0] = emi[i][0]; mats[i].emi[1] = emi[i][1]; mats[i].emi[2] = emi[i][2];
    }

    /* Single triangle: provoking vertex is index 2 (blue). */
    clear_bufs();
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        upload_mat(GL_FRONT_AND_BACK, &mats[i]);
        glNormal3f(0, 0, 1);
        float x = (i == 2) ? 0.0f : (i == 0 ? -0.8f : 0.8f);
        float y = (i == 2) ? 0.8f : -0.8f;
        glVertex3f(x, y, 0);
    }
    glEnd();
    int pts[][2] = {{32, 40}, {24, 36}, {40, 36}, {32, 24}};
    for (unsigned k = 0; k < 4; k++) {
        unsigned char p[4];
        read_fb(pts[k][0], pts[k][1], p);
        float blue[3] = {0, 0, 1};
        TASK_CHECK(color_close(p, blue, 3));
    }

    /* Strip: triangle 0 provoking = vertex 2 (blue),
     * triangle 1 provoking = vertex 3 (yellow). */
    static const float sv[12] = {
        -0.9f, -0.6f, 0,
        -0.3f, -0.6f, 0,
         0.3f,  0.6f, 0,
         0.9f,  0.6f, 0
    };
    clear_bufs();
    glBegin(GL_TRIANGLE_STRIP);
    for (int i = 0; i < 4; i++) {
        upload_mat(GL_FRONT_AND_BACK, &mats[i]);
        glNormal3f(0, 0, 1);
        glVertex3f(sv[i*3], sv[i*3+1], sv[i*3+2]);
    }
    glEnd();

    float scr[4][2];
    for (int i = 0; i < 4; i++) ref_project(sv[i*3], sv[i*3+1], scr[i]);
    float t0[6] = {scr[0][0], scr[0][1], scr[1][0], scr[1][1], scr[2][0], scr[2][1]};
    float c0f[2] = {(t0[0]+t0[2]+t0[4])/3.0f, (t0[1]+t0[3]+t0[5])/3.0f};
    int c0x = (int)c0f[0], c0y = (int)c0f[1];
    /* Second strip triangle assembled as (1, 0, 2)... vertices 1,2,3 form it;
     * its provoking submitted vertex is 3 regardless of winding swap. */
    float t1[6] = {scr[1][0], scr[1][1], scr[2][0], scr[2][1], scr[3][0], scr[3][1]};
    float c1f[2] = {(t1[0]+t1[2]+t1[4])/3.0f, (t1[1]+t1[3]+t1[5])/3.0f};
    int c1x = (int)c1f[0], c1y = (int)c1f[1];

    unsigned char p0[4], p1[4];
    read_fb(c0x, c0y, p0);
    read_fb(c1x, c1y, p1);
    TASK_CHECK(color_close(p0, emi[2], 3));   /* blue */
    TASK_CHECK(color_close(p1, emi[3], 3));   /* yellow */

    /* Back-facing flat triangle with distinct front/back materials: the flat
     * source is the provoking vertex's face-specific color. */
    rmat fmat[3], bmat[3];
    float front_emi[3][3] = {{1,0,0},{0,1,0},{0,0,1}};   /* vertex 2: blue   */
    float back_emi[3][3]  = {{0,1,1},{1,0,1},{1,1,0}};   /* vertex 2: yellow */
    for (int i = 0; i < 3; i++) {
        fmat[i] = rmat_black(0.0f);
        bmat[i] = rmat_black(0.0f);
        for (int ch = 0; ch < 3; ch++) {
            fmat[i].emi[ch] = front_emi[i][ch];
            bmat[i].emi[ch] = back_emi[i][ch];
        }
    }
    clear_bufs();
    glBegin(GL_TRIANGLES);   /* clockwise winding => back facing under GL_CCW */
    for (int i = 0; i < 3; i++) {
        upload_mat(GL_FRONT, &fmat[i]);
        upload_mat(GL_BACK, &bmat[i]);
        glNormal3f(0, 0, -1);
        float x = (i == 1) ? 0.0f : (i == 0 ? -0.8f : 0.8f);
        float y = (i == 1) ? 0.8f : -0.8f;
        glVertex3f(x, y, 0);
    }
    glEnd();
    unsigned char bp[4];
    read_fb(32, 36, bp);
    TASK_CHECK(color_close(bp, back_emi[2], 3));   /* provoking back color */

    glFrontFace(GL_CW);                            /* same winding now front */
    clear_bufs();
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        upload_mat(GL_FRONT, &fmat[i]);
        upload_mat(GL_BACK, &bmat[i]);
        glNormal3f(0, 0, -1);
        float x = (i == 1) ? 0.0f : (i == 0 ? -0.8f : 0.8f);
        float y = (i == 1) ? 0.8f : -0.8f;
        glVertex3f(x, y, 0);
    }
    glEnd();
    unsigned char fp[4];
    read_fb(32, 36, fp);
    TASK_CHECK(color_close(fp, front_emi[2], 3));  /* provoking front color */
    glFrontFace(GL_CCW);

    end_ctx(c);
    return 0;
}

/* Two-sided lighting: back faces flip the normal and use back materials;
 * glFrontFace keeps controlling which side is which. */
static int case_two_sided(void)
{
    GLState *c = new_ctx();
    TASK_CHECK(c != NULL);
    lighting_base();
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    glShadeModel(GL_SMOOTH);

    rmat front = rmat_black(0.0f);
    front.emi[0] = 0.2f; front.dif[0] = 1.0f;       /* red   */
    rmat back = rmat_black(0.0f);
    back.emi[2] = 0.2f; back.dif[2] = 1.0f;         /* blue  */

    /* Clockwise wound triangle with geometric normal pointing -z, so after
     * the back-face flip it points toward the light at +z. */
    float coords[9] = {-0.8f, -0.8f, 0.0f,
                        0.0f,  0.8f, 0.0f,
                        0.8f, -0.8f, 0.0f};

    rvertex rv[3];
    for (int i = 0; i < 3; i++)
        rv[i] = make_lit_vertex(coords[i*3], coords[i*3+1], coords[i*3+2],
                                0, 0, -1, front, back, 1, 0);
    float s[6];
    for (int i = 0; i < 3; i++) project_rvertex(&rv[i], &s[i*2]);
    float b[3], want_back[3], want_front[3];
    ref_bary(s, 32.0f, 34.0f, b);
    for (int ch = 0; ch < 3; ch++) {
        want_back[ch]  = b[0]*rv[0].gouraud_back[ch]
                       + b[1]*rv[1].gouraud_back[ch]
                       + b[2]*rv[2].gouraud_back[ch];
        want_front[ch] = b[0]*rv[0].gouraud_front[ch]
                       + b[1]*rv[1].gouraud_front[ch]
                       + b[2]*rv[2].gouraud_front[ch];
    }

    clear_bufs();
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        upload_mat(GL_FRONT, &front);
        upload_mat(GL_BACK, &back);
        glNormal3f(0, 0, -1);
        glVertex3f(coords[i*3], coords[i*3+1], coords[i*3+2]);
    }
    glEnd();
    unsigned char pb[4];
    read_fb(32, 34, pb);
    TASK_CHECK(color_close(pb, want_back, 4));
    TASK_CHECK(pb[2] > pb[0] + 40);                 /* blue-dominated */

    /* Reverse the front-face convention: the same winding is now front. */
    glFrontFace(GL_CW);
    clear_bufs();
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        upload_mat(GL_FRONT, &front);
        upload_mat(GL_BACK, &back);
        glNormal3f(0, 0, -1);
        glVertex3f(coords[i*3], coords[i*3+1], coords[i*3+2]);
    }
    glEnd();
    unsigned char pf[4];
    read_fb(32, 34, pf);
    TASK_CHECK(color_close(pf, want_front, 4));
    TASK_CHECK(pf[0] >= pf[2]);                     /* red-dominated */

    end_ctx(c);
    return 0;
}

/* GL_COLOR_MATERIAL drives the selected face/property through rendering. */
static int case_color_material_render(void)
{
    GLState *c = new_ctx();
    TASK_CHECK(c != NULL);
    lighting_base();
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT, GL_DIFFUSE);
    glShadeModel(GL_SMOOTH);

    rmat base = rmat_black(0.0f);   /* ambient/specular/emission stay zero */

    float cols[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float coords[9] = {-0.8f, -0.8f, 0,
                        0.8f, -0.8f, 0,
                        0.0f,  0.8f, 0};
    float nrm[3] = {0, 0, 1};

    rvertex rv[3];
    for (int i = 0; i < 3; i++) {
        rmat m = base;
        m.dif[0] = cols[i][0]; m.dif[1] = cols[i][1]; m.dif[2] = cols[i][2];
        rv[i] = make_lit_vertex(coords[i*3], coords[i*3+1], coords[i*3+2],
                                nrm[0], nrm[1], nrm[2], m, m, 0, 0);
    }
    float s[6];
    for (int i = 0; i < 3; i++) project_rvertex(&rv[i], &s[i*2]);

    clear_bufs();
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        upload_mat(GL_FRONT_AND_BACK, &base);
        glColor3f(cols[i][0], cols[i][1], cols[i][2]);
        glNormal3f(0, 0, 1);
        glVertex3f(coords[i*3], coords[i*3+1], coords[i*3+2]);
    }
    glEnd();

    int samples[][2] = {{32, 40}, {26, 34}};
    for (unsigned k = 0; k < 2; k++) {
        float bb[3], want[3] = {0, 0, 0};
        ref_bary(s, (float)samples[k][0], (float)samples[k][1], bb);
        for (int ch = 0; ch < 3; ch++)
            want[ch] = bb[0]*rv[0].gouraud_front[ch]
                     + bb[1]*rv[1].gouraud_front[ch]
                     + bb[2]*rv[2].gouraud_front[ch];
        unsigned char p[4];
        read_fb(samples[k][0], samples[k][1], p);
        TASK_CHECK(color_close(p, want, 4));
    }

    end_ctx(c);
    return 0;
}

/* GL_NORMALIZE still decides whether interpolated Phong normals are
 * renormalized per fragment. */
static int case_normalize_switch(void)
{
    GLState *c = new_ctx();
    TASK_CHECK(c != NULL);
    lighting_base();
    glShadeModel(GL_PHONG);

    rmat m = rmat_black(8.0f);
    m.dif[0] = m.dif[1] = m.dif[2] = 0.5f;

    float coords[9] = {-0.8f, -0.8f, 0,
                        0.8f, -0.8f, 0,
                        0.0f,  0.8f, 0};
    float norms[9] = {-0.6f, 0.0f, 0.8f,
                       0.6f, 0.0f, 0.8f,
                       0.0f, 0.6f, 0.8f};

    glShadeModel(GL_PHONG);
    unsigned char off[4], on[4];
    float want_off[3], want_on[3];
    int sx = 32, sy = 32;

    for (int pass = 0; pass < 2; pass++) {
        int normalize = pass;
        if (normalize) glEnable(GL_NORMALIZE); else glDisable(GL_NORMALIZE);
        clear_bufs();
        glBegin(GL_TRIANGLES);
        for (int i = 0; i < 3; i++) {
            upload_mat(GL_FRONT_AND_BACK, &m);
            glNormal3f(norms[i*3], norms[i*3+1], norms[i*3+2]);
            glVertex3f(coords[i*3], coords[i*3+1], coords[i*3+2]);
        }
        glEnd();
        if (normalize) read_fb(sx, sy, on); else read_fb(sx, sy, off);

        /* Reference: barycentric weights (w=1), interpolate the normalized
         * eye normals; only renormalize when the flag is on. */
        float s[6];
        for (int i = 0; i < 3; i++) {
            rvertex tmp = make_lit_vertex(coords[i*3], coords[i*3+1], coords[i*3+2],
                                          norms[i*3], norms[i*3+1], norms[i*3+2],
                                          m, m, 0, 0);
            ref_project(tmp.pos.x, tmp.pos.y, &s[i*2]);
        }
        float bb[3];
        ref_bary(s, (float)sx, (float)sy, bb);
        rvec3 N = {0};
        for (int i = 0; i < 3; i++) {
            rvec3 nn = rnorm(rv3(norms[i*3], norms[i*3+1], norms[i*3+2]));
            N = radd(N, rscale(nn, bb[i]));
        }
        rvec3 P = rv3(0, 0, 0);
        if (normalize) ref_light(P, N, &m, 1, pass ? want_on : want_off);
        else           ref_light(P, N, &m, 0, pass ? want_on : want_off);
    }

    TASK_CHECK(color_close(off, want_off, 4));
    TASK_CHECK(color_close(on, want_on, 4));
    TASK_CHECK(memcmp(off, on, 3) != 0);

    end_ctx(c);
    return 0;
}

/* ------------------------------------------------------------------ */
/* clipping group                                                      */
/* ------------------------------------------------------------------ */

static void frustum_projection(void)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 10.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

/* Reference clip-space transform matching glFrustum(-1,1,-1,1,1,10). */
static void ref_clip(const float eye[3], float clip[4])
{
    mat4_t proj = mat4_frustum(-1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 10.0f);
    vec4_t v = mat4_mul_vec4(proj, vec4(eye[0], eye[1], eye[2], 1.0f));
    clip[0] = v.x; clip[1] = v.y; clip[2] = v.z; clip[3] = v.w;
}

/* GL_FLAT with the provoking (third) vertex clipped away: the surviving
 * fragments must still show its lit color, not a replacement source. */
static int case_flat_provoking_clipped(void)
{
    GLState *c = new_ctx();
    TASK_CHECK(c != NULL);
    frustum_projection();
    lighting_base();
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    glShadeModel(GL_FLAT);

    rmat m0 = rmat_black(0), m1 = rmat_black(0), m2 = rmat_black(0);
    m0.emi[0] = 1.0f;                  /* red    */
    m1.emi[1] = 1.0f;                  /* green  */
    m2.emi[2] = 1.0f;                  /* blue, provoking, will be clipped */

    /* v2 at eye z=-0.5 is in front of the near plane (z=-1). */
    float eye[9] = {-1.5f, -1.5f, -3.0f,
                     1.5f, -1.5f, -3.0f,
                     0.0f,  1.5f, -0.5f};
    /* Sanity: confirm v2 really is outside the near plane. */
    float clip2[4];
    ref_clip(&eye[6], clip2);
    TASK_CHECK(clip2[2] < -clip2[3]);
    float clip0[4], clip1[4];
    ref_clip(&eye[0], clip0);
    ref_clip(&eye[3], clip1);
    TASK_CHECK(clip0[2] >= -clip0[3] && clip1[2] >= -clip1[3]);

    clear_bufs();
    glBegin(GL_TRIANGLES);
        upload_mat(GL_FRONT_AND_BACK, &m0);
        glNormal3f(0, 0, 1);
        glVertex3f(eye[0], eye[1], eye[2]);
        upload_mat(GL_FRONT_AND_BACK, &m1);
        glVertex3f(eye[3], eye[4], eye[5]);
        upload_mat(GL_FRONT_AND_BACK, &m2);
        glVertex3f(eye[6], eye[7], eye[8]);
    glEnd();

    int samples[][2] = {{32, 36}, {28, 40}, {36, 40}, {32, 44}};
    for (unsigned k = 0; k < 4; k++) {
        unsigned char p[4];
        read_fb(samples[k][0], samples[k][1], p);
        /* Flat source remains the provoking blue vertex despite clipping. */
        float blue[3] = {0, 0, 1};
        TASK_CHECK(color_close(p, blue, 3));
        TASK_CHECK(!(p[1] > p[2]));    /* not green from a surviving vertex */
    }

    end_ctx(c);
    return 0;
}

/* Smooth shading after a near-plane clip that splits the polygon: replicate
 * the Sutherland-Hodgman near-plane clip and fan triangulation in the
 * reference, then check affine-interpolated Gouraud colors. */
static int case_smooth_clipped_reference(void)
{
    GLState *c = new_ctx();
    TASK_CHECK(c != NULL);
    frustum_projection();

    glEnable(GL_LIGHTING);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    static const float zero[4] = {0, 0, 0, 1};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zero);
    glShadeModel(GL_SMOOTH);

    float eye[9] = {-1.5f, -1.5f, -3.0f,
                     1.5f, -1.5f, -3.0f,
                     0.0f,  1.5f, -0.5f};      /* clipped by near plane */
    float emi[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    clear_bufs();
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        float e[4] = {emi[i][0], emi[i][1], emi[i][2], 1};
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, e);
        glNormal3f(0, 0, 1);
        glVertex3f(eye[i*3], eye[i*3+1], eye[i*3+2]);
    }
    glEnd();

    /* Reference near-plane clip in eye space: plane z = -1. */
    typedef struct { float p[3]; float em[3]; } rp;
    rp in[3];
    for (int i = 0; i < 3; i++) {
        memcpy(in[i].p, &eye[i*3], 3*sizeof(float));
        memcpy(in[i].em, emi[i], 3*sizeof(float));
    }
    rp poly[6];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        rp *A = &in[(i + 2) % 3], *B = &in[i];
        float da = -1.0f - A->p[2];          /* inside when z <= -1 => d >= 0 */
        float db = -1.0f - B->p[2];
        if (db >= 0.0f) {
            if (da < 0.0f) {
                float t = da / (da - db);
                rp I;
                for (int k = 0; k < 3; k++) I.p[k] = A->p[k] + t*(B->p[k]-A->p[k]);
                for (int k = 0; k < 3; k++) I.em[k] = A->em[k] + t*(B->em[k]-A->em[k]);
                poly[n++] = I;
            }
            poly[n++] = *B;
        } else if (da >= 0.0f) {
            float t = da / (da - db);
            rp I;
            for (int k = 0; k < 3; k++) I.p[k] = A->p[k] + t*(B->p[k]-A->p[k]);
            for (int k = 0; k < 3; k++) I.em[k] = A->em[k] + t*(B->em[k]-A->em[k]);
            poly[n++] = I;
        }
    }
    TASK_CHECK(n >= 3);

    /* Project the clipped polygon and fan-triangulate from poly[0]. */
    float ps[6][2];
    for (int i = 0; i < n; i++) {
        float clip[4];
        ref_clip(poly[i].p, clip);
        ref_project(clip[0]/clip[3], clip[1]/clip[3], ps[i]);
    }

    for (int j = 1; j + 1 < n; j++) {
        float s[6] = {ps[0][0], ps[0][1], ps[j][0], ps[j][1], ps[j+1][0], ps[j+1][1]};
        float cent[2] = {(s[0]+s[2]+s[4])/3.0f, (s[1]+s[3]+s[5])/3.0f};
        int sx = (int)cent[0], sy = (int)cent[1];
        float bb[3], want[3] = {0, 0, 0};
        ref_bary(s, (float)sx, (float)sy, bb);
        for (int ch = 0; ch < 3; ch++)
            want[ch] = bb[0]*poly[0].em[ch] + bb[1]*poly[j].em[ch]
                     + bb[2]*poly[j+1].em[ch];
        unsigned char p[4];
        read_fb(sx, sy, p);
        TASK_CHECK(color_close(p, want, 4));
    }

    end_ctx(c);
    return 0;
}

/* Phong with unequal w: perspective-correct weights are screen barycentrics
 * times 1/w, renormalized. The affine result must visibly differ. */
static int case_phong_unequal_w(void)
{
    GLState *c = new_ctx();
    TASK_CHECK(c != NULL);
    frustum_projection();
    lighting_base();
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_FALSE);
    glShadeModel(GL_PHONG);

    rmat mats[3];
    mats[0] = rmat_black(0.0f); mats[0].dif[0] = 1.0f;
    mats[1] = rmat_black(0.0f); mats[1].dif[1] = 1.0f;
    mats[2] = rmat_black(0.0f); mats[2].dif[2] = 1.0f;

    float eye[9] = {-1.2f, -1.2f, -2.0f,
                     1.2f, -1.2f, -8.0f,
                     0.0f,  1.2f, -3.0f};
    float nrm[9] = {0, 0, 1, 0, 0, 1, 0, 0, 1};

    clear_bufs();
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        upload_mat(GL_FRONT_AND_BACK, &mats[i]);
        glNormal3f(nrm[i*3], nrm[i*3+1], nrm[i*3+2]);
        glVertex3f(eye[i*3], eye[i*3+1], eye[i*3+2]);
    }
    glEnd();

    /* Reference: clip/w -> NDC -> screen, barycentrics, then q weights. */
    float clip[3][4], scr[3][2], winv[3];
    rvec3 epos[3], enrm[3];
    for (int i = 0; i < 3; i++) {
        ref_clip(&eye[i*3], clip[i]);
        winv[i] = 1.0f / clip[i][3];
        ref_project(clip[i][0]/clip[i][3], clip[i][1]/clip[i][3], scr[i]);
        epos[i] = rv3(eye[i*3], eye[i*3+1], eye[i*3+2]);
        enrm[i] = rnorm(rv3(nrm[i*3], nrm[i*3+1], nrm[i*3+2]));
    }
    TASK_CHECK(fabsf(winv[0] - winv[1]) > 0.05f);     /* genuinely unequal w */

    int samples[][2] = {{27, 35}, {30, 32}, {24, 40}};
    for (unsigned k = 0; k < 3; k++) {
        float s[6] = {scr[0][0], scr[0][1], scr[1][0], scr[1][1], scr[2][0], scr[2][1]};
        float b[3];
        ref_bary(s, (float)samples[k][0], (float)samples[k][1], b);
        float q[3] = {b[0]*winv[0], b[1]*winv[1], b[2]*winv[2]};
        float qs = q[0]+q[1]+q[2];
        for (int i = 0; i < 3; i++) q[i] /= qs;

        rvec3 P = rv3(q[0]*epos[0].x + q[1]*epos[1].x + q[2]*epos[2].x,
                      q[0]*epos[0].y + q[1]*epos[1].y + q[2]*epos[2].y,
                      q[0]*epos[0].z + q[1]*epos[1].z + q[2]*epos[2].z);
        rvec3 N = rv3(q[0]*enrm[0].x + q[1]*enrm[1].x + q[2]*enrm[2].x,
                      q[0]*enrm[0].y + q[1]*enrm[1].y + q[2]*enrm[2].y,
                      q[0]*enrm[0].z + q[1]*enrm[1].z + q[2]*enrm[2].z);
        rmat m = rmat_black(0.0f);
        for (int ch = 0; ch < 3; ch++)
            m.dif[ch] = q[0]*mats[0].dif[ch] + q[1]*mats[1].dif[ch] + q[2]*mats[2].dif[ch];
        float want[3];
        ref_light(P, N, &m, 0, want);

        /* Affine-only reference for contrast. */
        rvec3 Pa = rv3(b[0]*epos[0].x + b[1]*epos[1].x + b[2]*epos[2].x,
                       b[0]*epos[0].y + b[1]*epos[1].y + b[2]*epos[2].y,
                       b[0]*epos[0].z + b[1]*epos[1].z + b[2]*epos[2].z);
        rmat ma = rmat_black(0.0f);
        for (int ch = 0; ch < 3; ch++)
            ma.dif[ch] = b[0]*mats[0].dif[ch] + b[1]*mats[1].dif[ch] + b[2]*mats[2].dif[ch];
        float want_affine[3];
        rvec3 Na = rv3(b[0]*enrm[0].x + b[1]*enrm[1].x + b[2]*enrm[2].x,
                       b[0]*enrm[0].y + b[1]*enrm[1].y + b[2]*enrm[2].y,
                       b[0]*enrm[0].z + b[1]*enrm[1].z + b[2]*enrm[2].z);
        ref_light(Pa, Na, &ma, 0, want_affine);

        unsigned char p[4];
        read_fb(samples[k][0], samples[k][1], p);
        TASK_CHECK(color_close(p, want, 5));
        float da = fabsf(want[0]-want_affine[0]) + fabsf(want[1]-want_affine[1])
                 + fabsf(want[2]-want_affine[2]);
        TASK_CHECK(da > 0.02f);
    }

    end_ctx(c);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"capture",  "emission_gradient",          case_emission_gradient},
    {"capture",  "four_entries",               case_four_entries},
    {"capture",  "strip_fan",                  case_strip_fan},
    {"capture",  "material_after_last_vertex", case_material_after_last_vertex},
    {"capture",  "getmaterial_state",          case_getmaterial_state},
    {"lighting", "point_light_modes",          case_point_light_modes},
    {"lighting", "specular_highlight",         case_specular_highlight},
    {"lighting", "flat_provoking",             case_flat_provoking},
    {"lighting", "two_sided",                  case_two_sided},
    {"lighting", "color_material_render",      case_color_material_render},
    {"lighting", "normalize_switch",           case_normalize_switch},
    {"clipping", "flat_provoking_clipped",     case_flat_provoking_clipped},
    {"clipping", "smooth_clipped_reference",   case_smooth_clipped_reference},
    {"clipping", "phong_unequal_w",            case_phong_unequal_w},
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
