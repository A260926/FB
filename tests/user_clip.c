/*
 * User clipping plane (GL_CLIP_PLANE0..GL_CLIP_PLANE5) regression tests.
 *
 * Three groups:
 *   plane_state - API state, freezing semantics and display list behaviour
 *   geometry    - analytical intersection checks on points, lines, triangles
 *   render      - framebuffer / depth buffer content and the PPM illustration
 *
 * Every rendered expectation is derived independently from the matrices
 * installed in the test rather than read back from the library.
 */
#include <TGL/gl.h>
#include "zbuffer.h"
#include "task_support/check.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

/* identity ortho projection maps object coords straight to NDC, so the
 * viewport transform is known up front */
static float sx_of(float x)
{
    return x * ((W - 0.5f) / 2.0f) + ((W - 0.5f) / 2.0f);
}
static float sy_of(float y)
{
    return ((H - 0.5f) / 2.0f) - y * ((H - 0.5f) / 2.0f);
}
static int px_of(float x)
{
    return (int) lrintf(sx_of(x));
}
static int py_of(float y)
{
    return (int) lrintf(sy_of(y));
}

/* independent replication of the engine depth encoding (gl_eval_viewport
 * plus the ZB_POINT_Z_FRAC_BITS shift used by the rasterizers) */
static GLushort ref_depth(float ndc_z)
{
    float zsize = (float) (1 << (ZB_Z_BITS + ZB_POINT_Z_FRAC_BITS));
    float zp = ndc_z * (-(zsize - 0.5f) / 2.0f) +
               ((zsize - 0.5f) / 2.0f + (1 << ZB_POINT_Z_FRAC_BITS) / 2);
    return (GLushort)((GLint) zp >> ZB_POINT_Z_FRAC_BITS);
}

static ZBuffer *open_ctx(void)
{
    ZBuffer *z = ZB_open(W, H, ZB_MODE_RGBA, NULL);
    if (!z)
        return NULL;
    glInit(z);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 0);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return z;
}

static void close_ctx(ZBuffer *z)
{
    glClose();
    ZB_close(z);
}

static GLuint pixel_at(ZBuffer *z, int x, int y)
{
    return z->pbuf[y * z->xsize + x];
}
static GLushort depth_at(ZBuffer *z, int x, int y)
{
    return z->zbuf[y * z->xsize + x];
}
static int is_lit(GLuint p)
{
    return p != 0;
}
static int near_lit(ZBuffer *z, float fsx, float fsy, int radius)
{
    int cx = (int) lrintf(fsx), cy = (int) lrintf(fsy);
    for (int dy = -radius; dy <= radius; dy++)
        for (int dx = -radius; dx <= radius; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x >= 0 && x < W && y >= 0 && y < H &&
                is_lit(pixel_at(z, x, y)))
                return 1;
        }
    return 0;
}
static unsigned count_lit(ZBuffer *z)
{
    unsigned n = 0;
    for (int i = 0; i < W * H; i++)
        if (is_lit(z->pbuf[i]))
            n++;
    return n;
}

static void full_quad(float z)
{
    glBegin(GL_QUADS);
    glVertex3f(-1, -1, z);
    glVertex3f(1, -1, z);
    glVertex3f(1, 1, z);
    glVertex3f(-1, 1, z);
    glEnd();
}

/* ------------------------------------------------------------------ */
/* plane_state group                                                   */
/* ------------------------------------------------------------------ */

static int case_defaults(void)
{
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    GLint v;
    GLdouble eq[4];

    glGetIntegerv(GL_MAX_CLIP_PLANES, &v);
    TASK_CHECK(v == 6);

    for (GLenum p = GL_CLIP_PLANE0; p <= GL_CLIP_PLANE5; p++) {
        glGetIntegerv(p, &v);
        TASK_CHECK(v == 0);
        glGetClipPlane(p, eq);
        for (int i = 0; i < 4; i++)
            TASK_CHECK(eq[i] == 0.0);
    }
    close_ctx(z);
    return 0;
}

static int case_enable_query(void)
{
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    GLint v;

    glEnable(GL_CLIP_PLANE2);
    glGetIntegerv(GL_CLIP_PLANE2, &v);
    TASK_CHECK(v == 1);
    /* enabling one plane must not touch the others */
    glGetIntegerv(GL_CLIP_PLANE0, &v);
    TASK_CHECK(v == 0);
    glGetIntegerv(GL_CLIP_PLANE5, &v);
    TASK_CHECK(v == 0);

    glEnable(GL_CLIP_PLANE5);
    glDisable(GL_CLIP_PLANE2);
    glGetIntegerv(GL_CLIP_PLANE2, &v);
    TASK_CHECK(v == 0);
    glGetIntegerv(GL_CLIP_PLANE5, &v);
    TASK_CHECK(v == 1);
    close_ctx(z);
    return 0;
}

static int case_freeze_identity(void)
{
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    GLdouble eq_in[4] = {0.5, -1.0, 2.0, -3.25};
    GLdouble eq_out[4];

    glClipPlane(GL_CLIP_PLANE1, eq_in);
    glGetClipPlane(GL_CLIP_PLANE1, eq_out);
    for (int i = 0; i < 4; i++)
        TASK_CHECK(fabs(eq_out[i] - eq_in[i]) < 1e-9);

    /* positive scaling of the equation describes the same half space and
     * must freeze to a proportional eye-space equation */
    {
        GLdouble big[4] = {1.5, -3.0, 6.0, -9.75};
        GLdouble out2[4];
        glClipPlane(GL_CLIP_PLANE2, big);
        glGetClipPlane(GL_CLIP_PLANE2, out2);
        for (int i = 0; i < 4; i++)
            TASK_CHECK(fabs(out2[i] - 3.0 * eq_in[i]) < 1e-6);
    }
    close_ctx(z);
    return 0;
}

static int case_define_then_change_matrix(void)
{
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    GLdouble eq[4] = {1, 0, 0, 0};
    GLdouble out[4];

    glClipPlane(GL_CLIP_PLANE0, eq);
    glGetClipPlane(GL_CLIP_PLANE0, out);
    TASK_CHECK(fabs(out[0] - 1.0) < 1e-9 && out[1] == 0.0 && out[2] == 0.0 &&
               out[3] == 0.0);

    /* moving MODELVIEW after the definition must not move the frozen
     * eye-space plane */
    glTranslatef(5.0f, 0.0f, 0.0f);
    glRotatef(37.0f, 0.0f, 1.0f, 0.0f);
    glGetClipPlane(GL_CLIP_PLANE0, out);
    TASK_CHECK(fabs(out[0] - 1.0) < 1e-9 && out[1] == 0.0 && out[2] == 0.0 &&
               out[3] == 0.0);

    /* redefining under the new matrix recaptures it */
    glLoadIdentity();
    glTranslatef(1.0f, 0.0f, 0.0f);
    glClipPlane(GL_CLIP_PLANE0, eq); /* object x >= 0, eye x' = x + 1 */
    glGetClipPlane(GL_CLIP_PLANE0, out);
    TASK_CHECK(fabs(out[0] - 1.0) < 1e-6);
    TASK_CHECK(fabs(out[3] - (-1.0)) < 1e-6); /* eye: x' - 1 >= 0 */
    close_ctx(z);
    return 0;
}

static int case_list_compile(void)
{
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    GLuint list = glGenLists(1);
    GLint enabled;
    GLdouble eq[4] = {1, 0, 0, 0};
    GLdouble out[4];

    glNewList(list, GL_COMPILE);
    glClipPlane(GL_CLIP_PLANE0, eq);
    glEnable(GL_CLIP_PLANE0);
    /* queries stay immediate even while a list is open */
    glGetIntegerv(GL_CLIP_PLANE0, &enabled);
    TASK_CHECK(enabled == 0);
    glEndList();

    /* nothing executed yet */
    glGetIntegerv(GL_CLIP_PLANE0, &enabled);
    TASK_CHECK(enabled == 0);
    glGetClipPlane(GL_CLIP_PLANE0, out);
    TASK_CHECK(out[0] == 0.0 && out[3] == 0.0);

    glCallList(list);
    glGetIntegerv(GL_CLIP_PLANE0, &enabled);
    TASK_CHECK(enabled == 1);
    glGetClipPlane(GL_CLIP_PLANE0, out);
    TASK_CHECK(fabs(out[0] - 1.0) < 1e-9);
    glDeleteLists(list, 1);
    close_ctx(z);
    return 0;
}

static int case_list_compile_and_execute(void)
{
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    GLuint list = glGenLists(1);
    GLdouble eq[4] = {1, 0, 0, 0};
    GLdouble out[4];

    glNewList(list, GL_COMPILE_AND_EXECUTE);
    glClipPlane(GL_CLIP_PLANE0, eq);
    glEnable(GL_CLIP_PLANE0);
    glEndList();

    glGetClipPlane(GL_CLIP_PLANE0, out);
    TASK_CHECK(fabs(out[0] - 1.0) < 1e-9 && fabs(out[3]) < 1e-9);

    /* replay under another matrix: the recorded definition re-captures the
     * matrix active at replay time */
    glLoadIdentity();
    glTranslatef(1.0f, 0.0f, 0.0f);
    glCallList(list);
    glGetClipPlane(GL_CLIP_PLANE0, out);
    TASK_CHECK(fabs(out[0] - 1.0) < 1e-6);
    TASK_CHECK(fabs(out[3] + 1.0) < 1e-6);

    glDeleteLists(list, 1);
    close_ctx(z);
    return 0;
}

/* ------------------------------------------------------------------ */
/* geometry group                                                      */
/* ------------------------------------------------------------------ */

static int case_points_and_lines(void)
{
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    GLdouble plane[4] = {1, 0, 0, 0}; /* keep x >= 0 */

    glColor3f(1, 1, 1);
    glClipPlane(GL_CLIP_PLANE0, plane);
    glEnable(GL_CLIP_PLANE0);

    glBegin(GL_POINTS);
    glVertex3f(-0.5f, 0.6f, 0); /* outside */
    glVertex3f(0.0f, 0.6f, 0);  /* on the boundary: kept */
    glVertex3f(0.5f, 0.6f, 0);  /* inside */
    glEnd();

    TASK_CHECK(!near_lit(z, sx_of(-0.5f), sy_of(0.6f), 1));
    TASK_CHECK(near_lit(z, sx_of(0.0f), sy_of(0.6f), 1));
    TASK_CHECK(near_lit(z, sx_of(0.5f), sy_of(0.6f), 1));

    /* independent line segment: [-0.8, 0.8] on y = -0.4, cut at x = 0 */
    glBegin(GL_LINES);
    glVertex3f(-0.8f, -0.4f, 0);
    glVertex3f(0.8f, -0.4f, 0);
    glEnd();

    TASK_CHECK(!near_lit(z, sx_of(-0.5f), sy_of(-0.4f), 1)); /* removed */
    TASK_CHECK(near_lit(z, sx_of(0.5f), sy_of(-0.4f), 1));   /* kept */
    TASK_CHECK(near_lit(z, sx_of(0.05f), sy_of(-0.4f), 1));  /* new endpoint */
    close_ctx(z);
    return 0;
}

static int case_multiple_planes(void)
{
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    GLdouble px[4] = {1, 0, 0, 0}; /* x >= 0 */
    GLdouble py[4] = {0, 1, 0, 0}; /* y >= 0 */

    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(1, 1, 1);
    glClipPlane(GL_CLIP_PLANE0, px);
    glClipPlane(GL_CLIP_PLANE1, py);
    glEnable(GL_CLIP_PLANE0);
    glEnable(GL_CLIP_PLANE1);
    full_quad(-0.8f);

    /* only the top-right quadrant (x >= 0, y >= 0) survives */
    TASK_CHECK(is_lit(pixel_at(z, px_of(0.5f), py_of(0.5f))));
    TASK_CHECK(!is_lit(pixel_at(z, px_of(-0.5f), py_of(0.5f))));
    TASK_CHECK(!is_lit(pixel_at(z, px_of(0.5f), py_of(-0.5f))));
    TASK_CHECK(!is_lit(pixel_at(z, px_of(-0.5f), py_of(-0.5f))));

    /* depth is only generated where geometry survived */
    TASK_CHECK(depth_at(z, px_of(0.5f), py_of(0.5f)) != 0);
    TASK_CHECK(depth_at(z, px_of(-0.5f), py_of(0.5f)) == 0);
    close_ctx(z);
    return 0;
}

static int case_triangle_oblique_cut(void)
{
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    /* keep x + y + 0.2 >= 0: vertex A is outside, B and C inside */
    GLdouble plane[4] = {1, 1, 0, 0.2};

    glClipPlane(GL_CLIP_PLANE0, plane);
    glEnable(GL_CLIP_PLANE0);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0);
    glVertex3f(-0.8f, -0.8f, 0); /* A */
    glColor3f(0, 1, 0);
    glVertex3f(0.8f, -0.8f, 0);  /* B */
    glColor3f(0, 0, 1);
    glVertex3f(0.0f, 0.8f, 0);   /* C */
    glEnd();

    /* analytical intersections:
     *   AB: t = 1.4/1.6 = 0.875      -> (0.6, -0.8)
     *   AC: t = 1.4/2.4 = 0.58333... -> (-0.3333, 0.1333)            */
    TASK_CHECK(near_lit(z, sx_of(0.6f), sy_of(-0.8f), 2));
    TASK_CHECK(near_lit(z, sx_of(-1.0f / 3.0f), sy_of(2.0f / 15.0f), 2));
    /* the removed corner around A stays empty */
    TASK_CHECK(!near_lit(z, sx_of(-0.75f), sy_of(-0.75f), 2));
    /* interior of the retained quad is filled in FILL mode */
    TASK_CHECK(is_lit(pixel_at(z, px_of(0.2f), py_of(0.0f))));
    close_ctx(z);
    return 0;
}

static int case_oblique_and_near_perspective(void)
{
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    GLdouble eye_plane[4] = {1, 0, 0, 0.2}; /* eye: x + 0.2 >= 0 */

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1, 1, -1, 1, 1, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    /* plane frozen while MODELVIEW is identity -> fixed in eye space */
    glClipPlane(GL_CLIP_PLANE0, eye_plane);
    glEnable(GL_CLIP_PLANE0);
    glTranslatef(0, 0, -3.0f);

    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    /* P0 lands behind the near plane AND outside the user plane;
     * P1/P2 are inside both */
    glVertex3f(-1.5f, -0.5f, 2.5f);
    glVertex3f(1.5f, -0.5f, 1.0f);
    glVertex3f(0.0f, 0.5f, 1.0f);
    glEnd();

    /* The near-plane intersection of edge P0P1 would be eye (-0.5,-0.5,-1)
     * -> NDC (-0.5,-0.5), but it fails the user plane (x+0.2 = -0.3), so
     * the later user-plane stage must remove it: intersection points keep
     * participating in the remaining clipping. */
    TASK_CHECK(!near_lit(z, sx_of(-0.5f), sy_of(-0.5f), 1));
    /* the surviving user-plane cut vertex: eye (-0.2,-0.5,-1.15),
     * NDC (-0.2/1.15, -0.5/1.15) */
    {
        float ndc_x = -0.2f / 1.15f;
        TASK_CHECK(near_lit(z, sx_of(ndc_x), sy_of(-0.5f / 1.15f), 2));
    }
    /* removed region: eye x well below -0.2 */
    TASK_CHECK(!near_lit(z, sx_of(-0.9f), sy_of(0.2f), 1));

    /* looser plane keeps the near-plane intersection instead, proving the
     * near-plane vertex is generated and then retained by the user stage */
    glDisable(GL_CLIP_PLANE0);
    {
        GLdouble loose[4] = {1, 0, 0, 1.0}; /* eye: x + 1 >= 0 */
        glClipPlane(GL_CLIP_PLANE0, loose);
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_CLIP_PLANE0);
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.5f, -0.5f, 2.5f);
    glVertex3f(1.5f, -0.5f, 1.0f);
    glVertex3f(0.0f, 0.5f, 1.0f);
    glEnd();
    /* near-plane vertex eye (-0.5,-0.5,-1): now x+1 = 0.5 >= 0, kept */
    TASK_CHECK(near_lit(z, sx_of(-0.5f), sy_of(-0.5f), 2));
    close_ctx(z);
    return 0;
}

static int case_wireframe_cut_edges(void)
{
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    GLdouble plane[4] = {1, 1, 0, 0.2};

    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glClipPlane(GL_CLIP_PLANE0, plane);
    glEnable(GL_CLIP_PLANE0);
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.8f, -0.8f, 0);
    glVertex3f(0.8f, -0.8f, 0);
    glVertex3f(0.0f, 0.8f, 0);
    glEnd();
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* the new cut edge (0.6,-0.8)-(-0.333,0.133) must be drawn */
    TASK_CHECK(near_lit(z, sx_of(0.1333f), sy_of(-0.3333f), 2));
    /* centroid of the retained quad: interior, must stay background in
     * wireframe - no diagonal may cross the clipping fan */
    {
        float cx = (0.8f + 0.0f + 0.6f - 0.3333f) / 4.0f;
        float cy = (-0.8f + 0.8f - 0.8f + 0.1333f) / 4.0f;
        TASK_CHECK(!near_lit(z, sx_of(cx), sy_of(cy), 1));
    }
    /* the same interior point is filled when the cut runs in FILL mode,
     * proving geometry (not a pixel erase) produced the wireframe result */
    close_ctx(z);
    z = open_ctx();
    TASK_CHECK(z != NULL);
    glClipPlane(GL_CLIP_PLANE0, plane);
    glEnable(GL_CLIP_PLANE0);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 1, 1);
    glVertex3f(-0.8f, -0.8f, 0);
    glVertex3f(0.8f, -0.8f, 0);
    glVertex3f(0.0f, 0.8f, 0);
    glEnd();
    {
        float cx = (0.8f + 0.0f + 0.6f - 0.3333f) / 4.0f;
        float cy = (-0.8f + 0.8f - 0.8f + 0.1333f) / 4.0f;
        TASK_CHECK(is_lit(pixel_at(z, px_of(cx), py_of(cy))));
    }
    close_ctx(z);
    return 0;
}

static void make_checker(unsigned char *rgb, int dim)
{
    const int cell = dim / 8;
    for (int y = 0; y < dim; y++)
        for (int x = 0; x < dim; x++) {
            int white = ((x / cell) + (y / cell)) & 1;
            unsigned char v = white ? 255 : 0;
            int o = 3 * (y * dim + x);
            rgb[o] = rgb[o + 1] = rgb[o + 2] = v;
        }
}

static int case_textured_checker(void)
{
    enum { DIM = 256 };
    static unsigned char rgb[DIM * DIM * 3];
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    GLdouble plane[4] = {1, 0, 0, 0}; /* keep x >= 0 */

    make_checker(rgb, DIM);
    {
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, 3, DIM, DIM, 0, GL_RGB,
                     GL_UNSIGNED_BYTE, rgb);
    }
    glEnable(GL_TEXTURE_2D);
    glColor3f(1, 1, 1);
    glClipPlane(GL_CLIP_PLANE0, plane);
    glEnable(GL_CLIP_PLANE0);

    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex3f(-1, -1, 0);
    glTexCoord2f(1, 0);
    glVertex3f(1, -1, 0);
    glTexCoord2f(1, 1);
    glVertex3f(1, 1, 0);
    glTexCoord2f(0, 1);
    glVertex3f(-1, 1, 0);
    glEnd();

    /* removed half is empty, kept half textured */
    TASK_CHECK(!is_lit(pixel_at(z, px_of(-0.5f), py_of(0.0f))));
    TASK_CHECK(is_lit(pixel_at(z, px_of(0.5f), py_of(0.0f))));
    /* both checker cell colors must survive the cut: texture coordinates
     * are interpolated, not flattened, across the clipped region */
    {
        int black = 0, white = 0;
        for (int y = 0; y < H; y++)
            for (int x = px_of(0.0f); x < W; x++) {
                GLuint p = pixel_at(z, x, y);
                if (p == 0)
                    black++;
                else
                    white++;
            }
        TASK_CHECK(black > 0 && white > 0);
    }
    /* nothing at all may be written on the removed (left) half; stay a
     * couple of pixels off the cut edge where the fill rules may touch the
     * boundary cell */
    for (int y = 0; y < H; y++)
        for (int x = 0; x <= px_of(0.0f) - 2; x++)
            TASK_CHECK(pixel_at(z, x, y) == 0);
    close_ctx(z);
    return 0;
}

static int case_lighting_path(void)
{
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    GLdouble plane[4] = {1, 0, 0, 0};
    static GLfloat pos[4] = {0, 0, 1, 0};
    static GLfloat diff[4] = {1, 1, 1, 1};

    glClipPlane(GL_CLIP_PLANE0, plane);
    glEnable(GL_CLIP_PLANE0);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, diff);
    full_quad(0);

    TASK_CHECK(!is_lit(pixel_at(z, px_of(-0.5f), py_of(0.0f))));
    TASK_CHECK(is_lit(pixel_at(z, px_of(0.5f), py_of(0.0f))));
    close_ctx(z);
    return 0;
}

static int case_scaling_invariance(void)
{
    ZBuffer *a = open_ctx();
    TASK_CHECK(a != NULL);
    GLdouble p1[4] = {1, 0, 0, 0};
    GLdouble p2[4] = {3.5f, 0, 0, 0};
    size_t n = (size_t) W * H * sizeof(GLuint);
    size_t nz = (size_t) W * H * sizeof(GLushort);
    void *img1 = malloc(n), *dep1 = malloc(nz);
    void *img2, *dep2;
    ZBuffer *b;

    TASK_CHECK(img1 && dep1);
    glColor3f(0.7f, 0.3f, 0.9f);
    glClipPlane(GL_CLIP_PLANE0, p1);
    glEnable(GL_CLIP_PLANE0);
    full_quad(-0.3f);
    memcpy(img1, a->pbuf, n);
    memcpy(dep1, a->zbuf, nz);
    close_ctx(a);

    b = open_ctx();
    TASK_CHECK(b != NULL);
    img2 = malloc(n);
    dep2 = malloc(nz);
    TASK_CHECK(img2 && dep2);
    glColor3f(0.7f, 0.3f, 0.9f);
    glClipPlane(GL_CLIP_PLANE0, p2); /* same half space, x 3.5 */
    glEnable(GL_CLIP_PLANE0);
    full_quad(-0.3f);
    memcpy(img2, b->pbuf, n);
    memcpy(dep2, b->zbuf, nz);

    TASK_CHECK(memcmp(img1, img2, n) == 0);
    TASK_CHECK(memcmp(dep1, dep2, nz) == 0);
    free(img1);
    free(dep1);
    free(img2);
    free(dep2);
    close_ctx(b);
    return 0;
}

/* ------------------------------------------------------------------ */
/* render group                                                        */
/* ------------------------------------------------------------------ */

static int case_fill_color_and_depth(void)
{
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    GLdouble plane[4] = {1, 0, 0, 0}; /* keep x >= 0 */

    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClipPlane(GL_CLIP_PLANE0, plane);
    glEnable(GL_CLIP_PLANE0);

    /* constant-depth full quad: every kept cell has an analytic color
     * and depth value to compare against */
    glColor3f(1, 0.4f, 0.2f);
    full_quad(-0.8f);

    {
        int kx = px_of(0.5f), ky = py_of(0.0f);
        GLuint p = pixel_at(z, kx, ky);
        TASK_CHECK(GET_RED(p) > 200);
        TASK_CHECK(abs((int) GET_GREEN(p) - (int) (0.4f * 255)) <= 2);
        TASK_CHECK(abs((int) GET_BLUE(p) - (int) (0.2f * 255)) <= 2);
        /* depth matches the independent analytic encoding */
        TASK_CHECK(abs((int) depth_at(z, kx, ky) -
                       (int) ref_depth(-0.8f)) <= 2);
        /* removed half: neither color nor depth was written */
        TASK_CHECK(pixel_at(z, px_of(-0.5f), py_of(0.0f)) == 0);
        TASK_CHECK(depth_at(z, px_of(-0.5f), py_of(0.0f)) == 0);
    }
    close_ctx(z);
    return 0;
}

static int case_depth_occlusion(void)
{
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    GLdouble plane[4] = {1, 0, 0, 0};

    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClipPlane(GL_CLIP_PLANE0, plane);
    glEnable(GL_CLIP_PLANE0);

    /* far red quad first, then a nearer green quad covering the kept
     * right half; depth test must hide the far surface */
    glColor3f(1, 0, 0);
    full_quad(0.8f);
    glColor3f(0, 1, 0);
    full_quad(-0.8f);

    {
        GLuint p = pixel_at(z, px_of(0.5f), py_of(0.0f));
        TASK_CHECK(GET_GREEN(p) > 200 && GET_RED(p) < 40);
        TASK_CHECK(abs((int) depth_at(z, px_of(0.5f), py_of(0.0f)) -
                       (int) ref_depth(-0.8f)) <= 2);
        TASK_CHECK(pixel_at(z, px_of(-0.5f), py_of(0.0f)) == 0);
    }
    close_ctx(z);
    return 0;
}

static int case_wireframe_vs_mask(void)
{
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    GLdouble plane[4] = {1, 0, 0, 0};

    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glClipPlane(GL_CLIP_PLANE0, plane);
    glEnable(GL_CLIP_PLANE0);
    glColor3f(1, 1, 1);
    full_quad(-0.8f);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* cut edge runs vertically at x = 0: it must be present */
    TASK_CHECK(near_lit(z, sx_of(0.0f), sy_of(0.0f), 1));
    /* interior of the kept half is empty in wireframe */
    TASK_CHECK(!near_lit(z, sx_of(0.6f), sy_of(0.0f), 1));
    /* but depth exists only along generated edges; a post-hoc pixel mask
     * could not explain the matching depth pattern. The cut vertex lands
     * at fractional screen x 31.75 which the rasterizer truncates */
    {
        int edge_x = (int) sx_of(0.0f);
        int edge_y = (int) sy_of(0.0f);
        TASK_CHECK(depth_at(z, edge_x, edge_y) != 0);
        TASK_CHECK(depth_at(z, px_of(0.6f), py_of(0.0f)) == 0);
    }
    close_ctx(z);
    return 0;
}

static int case_ppm_illustration(void)
{
    ZBuffer *z = open_ctx();
    TASK_CHECK(z != NULL);
    GLdouble oblique[4] = {1, 1, 0, 0.2};
    const char *path = "clipplanes.ppm";
    FILE *f;
    char header[32];
    int got;

    glClipPlane(GL_CLIP_PLANE0, oblique);
    glEnable(GL_CLIP_PLANE0);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0);
    glVertex3f(-0.8f, -0.8f, 0);
    glColor3f(0, 1, 0);
    glVertex3f(0.8f, -0.8f, 0);
    glColor3f(0, 0, 1);
    glVertex3f(0.0f, 0.8f, 0);
    glEnd();

    TASK_CHECK(count_lit(z) > 50);
    TASK_CHECK(!near_lit(z, sx_of(-0.75f), sy_of(-0.75f), 2));

    f = fopen(path, "wb");
    TASK_CHECK(f != NULL);
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned char rgb[3] = {
            (unsigned char) GET_RED(z->pbuf[i]),
            (unsigned char) GET_GREEN(z->pbuf[i]),
            (unsigned char) GET_BLUE(z->pbuf[i]),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);

    f = fopen(path, "rb");
    TASK_CHECK(f != NULL);
    got = fread(header, 1, sizeof(header) - 1, f);
    header[got] = 0;
    TASK_CHECK(strncmp(header, "P6", 2) == 0);
    fclose(f);
    close_ctx(z);
    return 0;
}

const struct task_case task_cases[] = {
    /* plane_state */
    {"plane_state", "defaults", case_defaults},
    {"plane_state", "enable_query", case_enable_query},
    {"plane_state", "freeze_identity", case_freeze_identity},
    {"plane_state", "define_then_change_matrix", case_define_then_change_matrix},
    {"plane_state", "list_compile", case_list_compile},
    {"plane_state", "list_compile_and_execute", case_list_compile_and_execute},
    /* geometry */
    {"geometry", "points_and_lines", case_points_and_lines},
    {"geometry", "multiple_planes", case_multiple_planes},
    {"geometry", "triangle_oblique_cut", case_triangle_oblique_cut},
    {"geometry", "oblique_and_near_perspective",
     case_oblique_and_near_perspective},
    {"geometry", "wireframe_cut_edges", case_wireframe_cut_edges},
    {"geometry", "textured_checker", case_textured_checker},
    {"geometry", "lighting_path", case_lighting_path},
    {"geometry", "scaling_invariance", case_scaling_invariance},
    /* render */
    {"render", "fill_color_and_depth", case_fill_color_and_depth},
    {"render", "depth_occlusion", case_depth_occlusion},
    {"render", "wireframe_vs_mask", case_wireframe_vs_mask},
    {"render", "ppm_illustration", case_ppm_illustration},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
