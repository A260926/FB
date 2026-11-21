/*
 * MyTinyGL - GL_SELECT selection mode tests
 *
 * Registered with the task driver via task_cases/task_case_count.
 * Groups: geometry (hit rules), records (buffer/name-stack semantics),
 * replay (display list interaction).
 */

#include "task_support/check.h"
#include "src/mytinygl.h"
#include <math.h>

#define SENTINEL 0xDEADBEEFu

/* ------------------------------------------------------------ */
/* Helpers                                                       */
/* ------------------------------------------------------------ */

static GLState *new_ctx(int w, int h)
{
    GLState *c = gl_create_context(w, h);
    gl_make_current(c);
    return c;
}

static void free_ctx(GLState *c)
{
    gl_make_current(NULL);
    gl_destroy_context(c);
}

static void fill_sentinel(GLuint *buf, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) buf[i] = SENTINEL;
}

/* CCW (front-facing) triangle centered on the view at NDC depth z */
static void draw_tri(float z)
{
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, z);
    glVertex3f( 0.5f, -0.5f, z);
    glVertex3f( 0.0f,  0.5f, z);
    glEnd();
}

/* CW (back-facing) triangle at NDC depth z */
static void draw_tri_cw(float z)
{
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, z);
    glVertex3f( 0.0f,  0.5f, z);
    glVertex3f( 0.5f, -0.5f, z);
    glEnd();
}

/* floor(z * 2^32-1 + 0.5) with clamping, mirroring the implementation */
static GLuint word_of(double d)
{
    if (d < 0.0) d = 0.0;
    if (d > 1.0) d = 1.0;
    return (GLuint)floor(d * 4294967295.0 + 0.5);
}

/* Expected depth word for NDC z under the default glDepthRange(0,1) */
static GLuint depth_word(double zndc)
{
    return word_of((zndc + 1.0) * 0.5);
}

/* ------------------------------------------------------------ */
/* Group: geometry                                               */
/* ------------------------------------------------------------ */

/* Points inside the frustum hit; outside or behind do not */
static int geo_points(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[8];
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 8);
    glSelectBuffer(8, buf);
    TASK_CHECK(glRenderMode(GL_SELECT) == 0);
    glInitNames();
    glPushName(7);
    glBegin(GL_POINTS);
    glVertex3f( 0.0f,  0.0f,  0.0f);  /* inside */
    glVertex3f( 2.0f,  0.0f,  0.0f);  /* outside: x > 1 */
    glVertex3f( 0.0f,  0.0f,  2.0f);  /* outside: beyond far plane */
    glVertex3f( 0.0f,  0.0f, -2.0f);  /* outside: beyond near plane */
    glVertex3f( 0.5f, -0.5f,  0.5f);  /* inside */
    glVertex3f( 1.0f,  1.0f,  1.0f);  /* on the far corner: inside */
    glVertex3f(-1.0f, -1.0f, -1.0f);  /* on the near corner: inside */
    glEnd();
    glPopName();
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);
    TASK_CHECK(buf[0] == 1);
    TASK_CHECK(buf[1] == depth_word(-1.0));
    TASK_CHECK(buf[2] == depth_word(1.0));
    TASK_CHECK(buf[3] == 7);
    TASK_CHECK(buf[4] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* Lines hit when a non-zero segment survives clipping */
static int geo_lines(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[16];
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 16);
    glSelectBuffer(16, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(3);
    glBegin(GL_LINES);
    /* crosses the left plane: clipped to x=-1 */
    glVertex3f(-2.0f, 0.0f, 0.0f);
    glVertex3f( 0.5f, 0.0f, 0.5f);
    /* fully outside (both endpoints x > 1): no hit */
    glVertex3f( 2.0f, 0.0f, 0.0f);
    glVertex3f( 3.0f, 0.0f, 0.0f);
    /* degenerate segment inside the frustum: zero length, no hit */
    glVertex3f( 0.2f, 0.2f, 0.2f);
    glVertex3f( 0.2f, 0.2f, 0.2f);
    /* crosses the near plane: clipped to z=-1 */
    glVertex3f( 0.0f, 0.5f,  0.5f);
    glVertex3f( 0.0f, 0.5f, -2.0f);
    glEnd();
    glLoadName(4);
    /* degenerate-only draw under a new name: still no record */
    glBegin(GL_LINES);
    glVertex3f(0.1f, 0.1f, 0.1f);
    glVertex3f(0.1f, 0.1f, 0.1f);
    glEnd();
    glLoadName(5);
    /* closed loop, all segments inside */
    glBegin(GL_LINE_LOOP);
    glVertex3f(-0.5f, -0.5f, 0.25f);
    glVertex3f( 0.5f, -0.5f, 0.25f);
    glVertex3f( 0.0f,  0.5f, 0.25f);
    glEnd();
    TASK_CHECK(glRenderMode(GL_RENDER) == 2);
    TASK_CHECK(buf[0] == 1);
    TASK_CHECK(buf[1] == depth_word(-1.0));  /* clipped at the near plane */
    TASK_CHECK(buf[2] == depth_word(0.5));
    TASK_CHECK(buf[3] == 3);
    TASK_CHECK(buf[4] == 1);
    TASK_CHECK(buf[5] == depth_word(0.25));
    TASK_CHECK(buf[6] == depth_word(0.25));
    TASK_CHECK(buf[7] == 5);
    TASK_CHECK(buf[8] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* Line strips and loops go through the same clipping chain */
static int geo_line_strip_loop(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[8];
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 8);
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(9);
    /* strip: first segment crosses the left plane */
    glBegin(GL_LINE_STRIP);
    glVertex3f(-2.0f, 0.0f, 0.0f);
    glVertex3f( 0.0f, 0.0f, 0.0f);
    glVertex3f( 0.0f, 0.5f, 0.0f);
    glEnd();
    glLoadName(10);
    /* loop fully outside (x > 1): no hit */
    glBegin(GL_LINE_LOOP);
    glVertex3f(2.0f, 0.0f, 0.0f);
    glVertex3f(3.0f, 0.0f, 0.0f);
    glVertex3f(2.5f, 0.5f, 0.0f);
    glEnd();
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);
    TASK_CHECK(buf[0] == 1);
    TASK_CHECK(buf[1] == depth_word(0.0));
    TASK_CHECK(buf[2] == depth_word(0.0));
    TASK_CHECK(buf[3] == 9);
    TASK_CHECK(buf[4] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* Triangles clipped by the near/side planes hit; depth extremes come
 * from the clipped geometry, not the original vertices */
static int geo_triangles_clip(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[16];
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 16);
    glSelectBuffer(16, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(1);
    /* crosses the near plane (z=-1): clipped remnant reaches depth 0 */
    glBegin(GL_TRIANGLES);
    glVertex3f( 0.0f,  0.5f, -2.0f);
    glVertex3f(-0.5f, -0.5f,  0.0f);
    glVertex3f( 0.5f, -0.5f,  0.0f);
    glEnd();
    glLoadName(2);
    /* crosses the left plane (x=-1): still hits, depths from z=0 */
    glBegin(GL_TRIANGLES);
    glVertex3f(-2.0f,  0.0f, 0.0f);
    glVertex3f( 0.5f, -0.5f, 0.0f);
    glVertex3f( 0.5f,  0.5f, 0.0f);
    glEnd();
    TASK_CHECK(glRenderMode(GL_RENDER) == 2);
    TASK_CHECK(buf[0] == 1);
    TASK_CHECK(buf[1] == depth_word(-1.0));  /* clipped z, not the original z=-2 */
    TASK_CHECK(buf[2] == depth_word(0.0));
    TASK_CHECK(buf[3] == 1);
    TASK_CHECK(buf[4] == 1);
    TASK_CHECK(buf[5] == depth_word(0.0));
    TASK_CHECK(buf[6] == depth_word(0.0));
    TASK_CHECK(buf[7] == 2);
    TASK_CHECK(buf[8] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* A sub-pixel triangle hits even though it covers no pixel center;
 * selection never touches the color/depth/stencil buffers */
static int geo_triangle_subpixel(void)
{
    GLState *c = new_ctx(64, 64);
    GLuint buf[8];
    pixel_t clear_px;
    size_t i;
    int mismatch = 0;
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 8);
    glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
    glClearDepth(0.75);
    glClearStencil(3);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    clear_px = c->framebuffer.color[0];
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(11);
    glBegin(GL_TRIANGLES);
    glVertex3f(0.3100f, 0.3100f, 0.3f);
    glVertex3f(0.3105f, 0.3100f, 0.3f);
    glVertex3f(0.3100f, 0.3105f, 0.3f);
    glEnd();
    glPopName();
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);
    TASK_CHECK(buf[0] == 1);
    TASK_CHECK(buf[1] == depth_word(0.3f));
    TASK_CHECK(buf[2] == depth_word(0.3f));
    TASK_CHECK(buf[3] == 11);
    for (i = 0; i < 64 * 64; i++) {
        if (c->framebuffer.color[i] != clear_px) mismatch = 1;
        if (c->framebuffer.depth[i] != 0.75f) mismatch = 1;
        if (c->framebuffer.stencil[i] != 3) mismatch = 1;
    }
    TASK_CHECK(!mismatch);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* Depth testing does not affect hits: an occluded triangle still hits */
static int geo_triangle_occluded(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[16];
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 16);
    glEnable(GL_DEPTH_TEST);
    glSelectBuffer(16, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(1);
    draw_tri(0.2f);   /* far */
    glLoadName(2);
    draw_tri(-0.2f);  /* near: would occlude in the framebuffer */
    TASK_CHECK(glRenderMode(GL_RENDER) == 2);
    TASK_CHECK(buf[0] == 1 && buf[1] == depth_word(0.2f) &&
               buf[2] == depth_word(0.2f) && buf[3] == 1);
    TASK_CHECK(buf[4] == 1 && buf[5] == depth_word(-0.2f) &&
               buf[6] == depth_word(-0.2f) && buf[7] == 2);
    TASK_CHECK(buf[8] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* Perspective projection: vertices with unequal clip w */
static int geo_unequal_w(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[8];
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 8);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 3.0);
    glMatrixMode(GL_MODELVIEW);
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(12);
    /* eye-space z = -1, -2, -3 -> clip w = 1, 2, 3 */
    glBegin(GL_TRIANGLES);
    glVertex3f( 0.5f, 0.0f, -1.0f);  /* ndc z = -1 */
    glVertex3f( 0.0f, 0.5f, -2.0f);  /* ndc z = 0.5 */
    glVertex3f(-0.5f, 0.0f, -3.0f);  /* ndc z = 1 */
    glEnd();
    glPopName();
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);
    TASK_CHECK(buf[0] == 1);
    TASK_CHECK(buf[1] == depth_word(-1.0));
    TASK_CHECK(buf[2] == depth_word(1.0));
    TASK_CHECK(buf[3] == 12);
    TASK_CHECK(buf[4] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* Reversed glDepthRange: extremes are taken over the mapped values */
static int geo_depth_range_reverse(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[8];
    double m0, m1, m2, lo, hi;
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 8);
    glDepthRange(0.8, 0.2);  /* reversed */
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(21);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, -1.0f);  /* maps to 0.8 */
    glVertex3f( 0.5f, -0.5f,  1.0f);  /* maps to 0.2 */
    glVertex3f( 0.0f,  0.5f,  0.0f);  /* maps to ~0.5 */
    glEnd();
    glPopName();
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);
    m0 = (-1.0 + 1.0) * 0.5 * (0.2 - 0.8) + 0.8;
    m1 = ( 1.0 + 1.0) * 0.5 * (0.2 - 0.8) + 0.8;
    m2 = ( 0.0 + 1.0) * 0.5 * (0.2 - 0.8) + 0.8;
    lo = m0; if (m1 < lo) lo = m1; if (m2 < lo) lo = m2;
    hi = m0; if (m1 > hi) hi = m1; if (m2 > hi) hi = m2;
    TASK_CHECK(buf[0] == 1);
    TASK_CHECK(buf[1] == word_of(lo));
    TASK_CHECK(buf[2] == word_of(hi));
    TASK_CHECK(buf[3] == 21);
    TASK_CHECK(buf[4] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* Triangles honor glFrontFace/glCullFace */
static int geo_cull_face(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[20];
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 20);
    glEnable(GL_CULL_FACE);
    glSelectBuffer(20, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(1);
    draw_tri(0.0f);        /* CCW front: kept (culling BACK) */
    glLoadName(2);
    draw_tri_cw(0.0f);     /* CW back: culled */
    glLoadName(3);
    glCullFace(GL_FRONT);
    draw_tri(0.0f);        /* CCW front: culled */
    glLoadName(4);
    draw_tri_cw(0.0f);     /* CW back: kept */
    glCullFace(GL_BACK);
    glFrontFace(GL_CW);
    glLoadName(5);
    draw_tri_cw(0.0f);     /* CW is now front: kept */
    glLoadName(6);
    draw_tri(0.0f);        /* CCW is now back: culled */
    TASK_CHECK(glRenderMode(GL_RENDER) == 3);
    TASK_CHECK(buf[0] == 1 && buf[1] == depth_word(0.0) &&
               buf[2] == depth_word(0.0) && buf[3] == 1);
    TASK_CHECK(buf[4] == 1 && buf[5] == depth_word(0.0) &&
               buf[6] == depth_word(0.0) && buf[7] == 4);
    TASK_CHECK(buf[8] == 1 && buf[9] == depth_word(0.0) &&
               buf[10] == depth_word(0.0) && buf[11] == 5);
    TASK_CHECK(buf[12] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* Depth/stencil/alpha/scissor/blend/color-mask state must not affect
 * hits, and the buffers stay untouched */
static int geo_state_independent(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[8];
    pixel_t clear_px;
    size_t i;
    int mismatch = 0;
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 8);
    glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    clear_px = c->framebuffer.color[0];
    /* every per-fragment kill switch on: none may affect selection */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_NEVER);
    glDepthMask(GL_FALSE);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_NEVER, 0, 0xFF);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_NEVER, 0.0f);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 1, 1);
    glEnable(GL_BLEND);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(31);
    draw_tri(0.0f);
    glPopName();
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);
    TASK_CHECK(buf[0] == 1 && buf[1] == depth_word(0.0) &&
               buf[2] == depth_word(0.0) && buf[3] == 31);
    for (i = 0; i < 32 * 32; i++) {
        if (c->framebuffer.color[i] != clear_px) mismatch = 1;
        if (c->framebuffer.depth[i] != 1.0f) mismatch = 1;
        if (c->framebuffer.stencil[i] != 0) mismatch = 1;
    }
    TASK_CHECK(!mismatch);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* Strips and fans hit; a fan clipped by the near plane stays one hit */
static int geo_strip_fan(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[16];
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 16);
    glSelectBuffer(16, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(41);
    /* triangle strip: two triangles, fully inside */
    glBegin(GL_TRIANGLE_STRIP);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f( 0.5f, -0.5f, 0.0f);
    glVertex3f(-0.5f,  0.5f, 0.0f);
    glVertex3f( 0.5f,  0.5f, 0.0f);
    glEnd();
    glLoadName(42);
    /* fan with center beyond the near plane: every triangle is clipped
     * into a fan and still counts as part of the same (merged) hit */
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f( 0.0f,  0.0f, -2.0f);  /* center: outside */
    glVertex3f(-0.5f, -0.5f,  0.0f);
    glVertex3f( 0.5f, -0.5f,  0.0f);
    glVertex3f( 0.5f,  0.5f,  0.0f);
    glVertex3f(-0.5f,  0.5f,  0.0f);
    glEnd();
    TASK_CHECK(glRenderMode(GL_RENDER) == 2);
    TASK_CHECK(buf[0] == 1 && buf[1] == depth_word(0.0) &&
               buf[2] == depth_word(0.0) && buf[3] == 41);
    TASK_CHECK(buf[4] == 1 && buf[5] == depth_word(-1.0) &&
               buf[6] == depth_word(0.0) && buf[7] == 42);
    TASK_CHECK(buf[8] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* ------------------------------------------------------------ */
/* Group: records                                                */
/* ------------------------------------------------------------ */

/* Records settle when the name stack changes and on mode exit */
static int rec_settle_sequence(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[24];
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 24);
    glSelectBuffer(24, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(10);
    draw_tri(0.0f);
    glPushName(20);      /* settles [10] */
    draw_tri(0.5f);
    glLoadName(30);      /* settles [10,20] */
    draw_tri(-0.5f);
    glPopName();         /* settles [10,30] */
    draw_tri(1.0f);
    TASK_CHECK(glRenderMode(GL_RENDER) == 4);  /* settles [10] */
    TASK_CHECK(buf[0] == 1 && buf[1] == depth_word(0.0) &&
               buf[2] == depth_word(0.0) && buf[3] == 10);
    TASK_CHECK(buf[4] == 2 && buf[5] == depth_word(0.5) &&
               buf[6] == depth_word(0.5) && buf[7] == 10 && buf[8] == 20);
    TASK_CHECK(buf[9] == 2 && buf[10] == depth_word(-0.5) &&
               buf[11] == depth_word(-0.5) && buf[12] == 10 && buf[13] == 30);
    TASK_CHECK(buf[14] == 1 && buf[15] == depth_word(1.0) &&
               buf[16] == depth_word(1.0) && buf[17] == 10);
    TASK_CHECK(buf[18] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* Immediate mode, glDrawArrays and glDrawElements merge into one
 * record while the name stack is unchanged */
static int rec_aggregate(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[8];
    static const GLfloat va[9] = { -0.5f, -0.5f, 0.5f,
                                    0.5f, -0.5f, 0.5f,
                                    0.0f,  0.5f, 0.5f };
    static const GLfloat vb[9] = { -0.5f, -0.5f, -0.5f,
                                    0.5f, -0.5f, -0.5f,
                                    0.0f,  0.5f, -0.5f };
    static const GLushort ib[3] = { 0, 1, 2 };
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 8);
    glEnableClientState(GL_VERTEX_ARRAY);
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(5);
    draw_tri(0.0f);                                    /* immediate mode */
    glVertexPointer(3, GL_FLOAT, 0, va);
    glDrawArrays(GL_TRIANGLES, 0, 3);                  /* vertex arrays */
    glVertexPointer(3, GL_FLOAT, 0, vb);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, ib);
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);          /* one merged record */
    TASK_CHECK(buf[0] == 1);
    TASK_CHECK(buf[1] == depth_word(-0.5));
    TASK_CHECK(buf[2] == depth_word(0.5));
    TASK_CHECK(buf[3] == 5);
    TASK_CHECK(buf[4] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* An empty name stack still produces a record */
static int rec_empty_stack(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[8];
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 8);
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    draw_tri(0.0f);
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);
    TASK_CHECK(buf[0] == 0);
    TASK_CHECK(buf[1] == depth_word(0.0));
    TASK_CHECK(buf[2] == depth_word(0.0));
    TASK_CHECK(buf[3] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* A buffer filled to exactly its capacity is legal */
static int rec_exact_capacity(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[8];
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 8);
    glSelectBuffer(8, buf);   /* exactly two 4-word records */
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(7);
    draw_tri(0.0f);
    glLoadName(8);
    draw_tri(0.0f);
    TASK_CHECK(glRenderMode(GL_RENDER) == 2);
    TASK_CHECK(buf[0] == 1 && buf[1] == depth_word(0.0) &&
               buf[2] == depth_word(0.0) && buf[3] == 7);
    TASK_CHECK(buf[4] == 1 && buf[5] == depth_word(0.0) &&
               buf[6] == depth_word(0.0) && buf[7] == 8);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* One word short: the record is not written at all, the buffer locks,
 * the exit reports -1, and nothing beyond the first record changes */
static int rec_one_word_short(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[16];
    int i;
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 16);
    glSelectBuffer(7, buf);   /* one word short of two 4-word records */
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(7);
    draw_tri(0.0f);
    glLoadName(8);
    draw_tri(0.0f);
    TASK_CHECK(glRenderMode(GL_RENDER) == -1);
    TASK_CHECK(buf[0] == 1 && buf[1] == depth_word(0.0) &&
               buf[2] == depth_word(0.0) && buf[3] == 7);
    for (i = 4; i < 16; i++) {
        TASK_CHECK(buf[i] == SENTINEL);
    }
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* Overflow locks the buffer and reports -1; re-entering selection
 * starts from empty results; zero capacity overflows on first write */
static int rec_overflow_recover(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[16];
    GLuint buf2[8];
    int i;
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 16);
    fill_sentinel(buf2, 8);
    glSelectBuffer(4, buf);   /* room for exactly one record */
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(1);
    draw_tri(0.0f);
    glLoadName(2);            /* settles record 1: buffer exactly full */
    draw_tri(0.0f);
    glLoadName(3);            /* record 2 does not fit: buffer locks */
    draw_tri(0.5f);
    glPushName(4);            /* locked: still no writes */
    draw_tri(0.0f);
    TASK_CHECK(glRenderMode(GL_RENDER) == -1);
    TASK_CHECK(buf[0] == 1 && buf[1] == depth_word(0.0) &&
               buf[2] == depth_word(0.0) && buf[3] == 1);
    for (i = 4; i < 16; i++) {
        TASK_CHECK(buf[i] == SENTINEL);
    }
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    /* recovery: rebind (legal again in GL_RENDER) and re-enter */
    glSelectBuffer(8, buf2);
    TASK_CHECK(glRenderMode(GL_SELECT) == 0);
    glInitNames();
    glPushName(9);
    draw_tri(0.0f);
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);
    TASK_CHECK(buf2[0] == 1 && buf2[1] == depth_word(0.0) &&
               buf2[2] == depth_word(0.0) && buf2[3] == 9);
    TASK_CHECK(buf2[4] == SENTINEL);
    /* zero capacity is a legal binding that overflows on the first record */
    glSelectBuffer(0, NULL);
    TASK_CHECK(glRenderMode(GL_SELECT) == 0);
    glInitNames();
    glPushName(1);
    draw_tri(0.0f);
    TASK_CHECK(glRenderMode(GL_RENDER) == -1);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* Name stack limits: 64 entries, overflow/underflow/invalid-op failures
 * neither settle records nor change the stack */
static int rec_stack_limits(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[260];
    GLuint buf2[8];
    GLint depth;
    int i;
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 260);
    glSelectBuffer(260, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPopName();
    TASK_CHECK(glGetError() == GL_STACK_UNDERFLOW);
    glLoadName(5);
    TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
    for (i = 1; i <= 64; i++) {
        glPushName((GLuint)i);
    }
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    glGetIntegerv(GL_NAME_STACK_DEPTH, &depth);
    TASK_CHECK(depth == 64);
    glPushName(65);
    TASK_CHECK(glGetError() == GL_STACK_OVERFLOW);
    glGetIntegerv(GL_NAME_STACK_DEPTH, &depth);
    TASK_CHECK(depth == 64);  /* failed push changed nothing */
    draw_tri(0.0f);
    glInitNames();            /* settles one record with all 64 names */
    draw_tri(0.5f);
    TASK_CHECK(glRenderMode(GL_RENDER) == 2);
    TASK_CHECK(buf[0] == 64);
    TASK_CHECK(buf[1] == depth_word(0.0) && buf[2] == depth_word(0.0));
    for (i = 0; i < 64; i++) {
        TASK_CHECK(buf[3 + i] == (GLuint)(i + 1));
    }
    TASK_CHECK(buf[67] == 0);  /* second record: empty stack */
    TASK_CHECK(buf[68] == depth_word(0.5) && buf[69] == depth_word(0.5));
    TASK_CHECK(buf[70] == SENTINEL);
    /* name ops outside selection mode are no-ops (but recordable) */
    glPushName(9);
    glPopName();
    glLoadName(3);
    glInitNames();
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    glGetIntegerv(GL_NAME_STACK_DEPTH, &depth);
    TASK_CHECK(depth == 0);
    fill_sentinel(buf2, 8);
    glSelectBuffer(8, buf2);
    TASK_CHECK(glRenderMode(GL_SELECT) == 0);
    draw_tri(0.0f);
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);
    TASK_CHECK(buf2[0] == 0);  /* render-mode pushes never happened */
    TASK_CHECK(buf2[3] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* API misuse: bad arguments and glBegin/glEnd-window calls report
 * errors without changing existing state */
static int rec_api_misuse(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[8];
    GLuint other[8];
    GLint mode;
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 8);
    fill_sentinel(other, 8);
    glSelectBuffer(-1, buf);
    TASK_CHECK(glGetError() == GL_INVALID_VALUE);
    glSelectBuffer(4, NULL);
    TASK_CHECK(glGetError() == GL_INVALID_VALUE);
    glSelectBuffer(0, NULL);   /* zero capacity is allowed */
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    glRenderMode(GL_FEEDBACK); /* unsupported mode */
    TASK_CHECK(glGetError() == GL_INVALID_ENUM);
    glRenderMode(GL_TRIANGLES);
    TASK_CHECK(glGetError() == GL_INVALID_ENUM);
    /* inside glBegin/glEnd every selection entry point is invalid */
    glSelectBuffer(8, buf);
    TASK_CHECK(glRenderMode(GL_SELECT) == 0);
    glBegin(GL_TRIANGLES);
    glPushName(1);
    TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
    glPopName();
    TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
    glLoadName(1);
    TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
    glInitNames();
    TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
    glSelectBuffer(8, other);
    TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
    TASK_CHECK(glRenderMode(GL_RENDER) == 0);
    TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f( 0.5f, -0.5f, 0.0f);
    glVertex3f( 0.0f,  0.5f, 0.0f);
    glEnd();
    /* the failed calls changed nothing: still selecting into buf */
    glGetIntegerv(GL_RENDER_MODE, &mode);
    TASK_CHECK(mode == GL_SELECT);
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);
    TASK_CHECK(buf[0] == 0 && buf[1] == depth_word(0.0) &&
               buf[2] == depth_word(0.0));
    TASK_CHECK(buf[3] == SENTINEL);
    TASK_CHECK(other[0] == SENTINEL);  /* never rebound */
    /* rebinding the buffer during selection is invalid, binding kept */
    TASK_CHECK(glRenderMode(GL_SELECT) == 0);
    glSelectBuffer(8, other);
    TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
    glInitNames();
    glPushName(2);
    draw_tri(0.0f);
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);
    TASK_CHECK(buf[0] == 1 && buf[1] == depth_word(0.0) &&
               buf[2] == depth_word(0.0) && buf[3] == 2);
    TASK_CHECK(other[0] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* Unsupported primitives in selection mode: GL_INVALID_ENUM, and the
 * render mode plus accumulated results are preserved */
static int rec_invalid_primitive(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[8];
    GLint mode;
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 8);
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(1);
    draw_tri(0.0f);
    glBegin(GL_QUADS);
    TASK_CHECK(glGetError() == GL_INVALID_ENUM);
    glBegin(GL_QUAD_STRIP);
    TASK_CHECK(glGetError() == GL_INVALID_ENUM);
    glBegin(GL_POLYGON);
    TASK_CHECK(glGetError() == GL_INVALID_ENUM);
    glGetIntegerv(GL_RENDER_MODE, &mode);
    TASK_CHECK(mode == GL_SELECT);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.5f);
    glVertex3f( 0.5f, -0.5f, 0.5f);
    glVertex3f( 0.0f,  0.5f, 0.5f);
    glEnd();
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);
    TASK_CHECK(buf[0] == 1 && buf[1] == depth_word(0.0) &&
               buf[2] == depth_word(0.5) && buf[3] == 1);
    TASK_CHECK(buf[4] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* Two interleaved contexts keep fully independent selection state */
static int rec_two_contexts(void)
{
    GLState *a = gl_create_context(32, 32);
    GLState *b = gl_create_context(32, 32);
    GLuint bufa[8];
    GLuint bufb[8];
    GLint ha, hb;
    TASK_CHECK(a != NULL && b != NULL);
    fill_sentinel(bufa, 8);
    fill_sentinel(bufb, 8);
    gl_make_current(a);
    glSelectBuffer(8, bufa);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(1);
    draw_tri(0.0f);
    gl_make_current(b);
    glSelectBuffer(8, bufb);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(2);
    draw_tri(0.5f);
    gl_make_current(a);
    draw_tri(0.25f);  /* more geometry under a's name */
    ha = glRenderMode(GL_RENDER);
    gl_make_current(b);
    hb = glRenderMode(GL_RENDER);
    TASK_CHECK(ha == 1 && hb == 1);
    TASK_CHECK(bufa[0] == 1 && bufa[1] == depth_word(0.0) &&
               bufa[2] == depth_word(0.25f) && bufa[3] == 1);
    TASK_CHECK(bufa[4] == SENTINEL);
    TASK_CHECK(bufb[0] == 1 && bufb[1] == depth_word(0.5f) &&
               bufb[2] == depth_word(0.5f) && bufb[3] == 2);
    TASK_CHECK(bufb[4] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    gl_make_current(a);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    gl_make_current(NULL);
    gl_destroy_context(a);
    gl_destroy_context(b);
    return 0;
}

/* Normal drawing after a selection pass works and leaves the
 * selection results untouched */
static int rec_resume_render(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[8];
    size_t i;
    size_t colored = 0;
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 8);
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(1);
    draw_tri(0.0f);
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);
    TASK_CHECK(buf[0] == 1 && buf[3] == 1);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(1.0f, 0.0f, 0.0f);
    draw_tri(0.0f);
    for (i = 0; i < 32 * 32; i++) {
        if (c->framebuffer.color[i] != 0xFF000000u) colored++;
    }
    TASK_CHECK(colored > 0);
    TASK_CHECK(buf[0] == 1 && buf[1] == depth_word(0.0) &&
               buf[2] == depth_word(0.0) && buf[3] == 1);
    TASK_CHECK(buf[4] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* ------------------------------------------------------------ */
/* Group: replay                                                 */
/* ------------------------------------------------------------ */

/* GL_COMPILE records name ops without executing them; replaying the
 * list in selection mode executes them, repeatedly */
static int rep_compile_only(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[16];
    GLuint list;
    GLint depth;
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 16);
    list = glGenLists(1);
    TASK_CHECK(list != 0);
    glNewList(list, GL_COMPILE);
    glPushName(42);
    draw_tri(0.0f);
    glPopName();
    glEndList();
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    glGetIntegerv(GL_NAME_STACK_DEPTH, &depth);
    TASK_CHECK(depth == 0);  /* compile did not execute the name ops */
    glSelectBuffer(16, buf);
    TASK_CHECK(glRenderMode(GL_SELECT) == 0);
    glInitNames();
    glCallList(list);
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);
    TASK_CHECK(buf[0] == 1 && buf[1] == depth_word(0.0) &&
               buf[2] == depth_word(0.0) && buf[3] == 42);
    TASK_CHECK(buf[4] == SENTINEL);
    /* repeated playback produces the record again */
    fill_sentinel(buf, 16);
    TASK_CHECK(glRenderMode(GL_SELECT) == 0);
    glCallList(list);
    glCallList(list);
    TASK_CHECK(glRenderMode(GL_RENDER) == 2);
    TASK_CHECK(buf[0] == 1 && buf[3] == 42);
    TASK_CHECK(buf[4] == 1 && buf[7] == 42);
    TASK_CHECK(buf[8] == SENTINEL);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* GL_COMPILE_AND_EXECUTE records and executes; replay reproduces the
 * records in selection mode and draws pixels in render mode */
static int rep_compile_and_execute(void)
{
    GLState *c = new_ctx(32, 32);
    GLuint buf[16];
    GLuint list;
    GLint depth;
    size_t i;
    size_t colored = 0;
    TASK_CHECK(c != NULL);
    fill_sentinel(buf, 16);
    glSelectBuffer(16, buf);
    TASK_CHECK(glRenderMode(GL_SELECT) == 0);
    glInitNames();
    list = glGenLists(1);
    TASK_CHECK(list != 0);
    glNewList(list, GL_COMPILE_AND_EXECUTE);
    glPushName(7);
    draw_tri(0.0f);
    glLoadName(8);
    draw_tri(0.5f);
    glPopName();
    glEndList();
    TASK_CHECK(glRenderMode(GL_RENDER) == 2);
    TASK_CHECK(buf[0] == 1 && buf[1] == depth_word(0.0) &&
               buf[2] == depth_word(0.0) && buf[3] == 7);
    TASK_CHECK(buf[4] == 1 && buf[5] == depth_word(0.5) &&
               buf[6] == depth_word(0.5) && buf[7] == 8);
    TASK_CHECK(buf[8] == SENTINEL);
    /* replaying in selection mode reproduces both records */
    fill_sentinel(buf, 16);
    TASK_CHECK(glRenderMode(GL_SELECT) == 0);
    glCallList(list);
    TASK_CHECK(glRenderMode(GL_RENDER) == 2);
    TASK_CHECK(buf[0] == 1 && buf[3] == 7);
    TASK_CHECK(buf[4] == 1 && buf[7] == 8);
    TASK_CHECK(buf[8] == SENTINEL);
    /* replaying in render mode draws pixels, name stack untouched */
    glGetIntegerv(GL_NAME_STACK_DEPTH, &depth);
    TASK_CHECK(depth == 0);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glCallList(list);
    glGetIntegerv(GL_NAME_STACK_DEPTH, &depth);
    TASK_CHECK(depth == 0);
    for (i = 0; i < 32 * 32; i++) {
        if (c->framebuffer.color[i] != 0xFF000000u) colored++;
    }
    TASK_CHECK(colored > 0);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    free_ctx(c);
    return 0;
}

/* ------------------------------------------------------------ */
/* Registration                                                  */
/* ------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "geometry", "points", geo_points },
    { "geometry", "lines", geo_lines },
    { "geometry", "line_strip_loop", geo_line_strip_loop },
    { "geometry", "triangles_clip", geo_triangles_clip },
    { "geometry", "triangle_subpixel", geo_triangle_subpixel },
    { "geometry", "triangle_occluded", geo_triangle_occluded },
    { "geometry", "unequal_w", geo_unequal_w },
    { "geometry", "depth_range_reverse", geo_depth_range_reverse },
    { "geometry", "cull_face", geo_cull_face },
    { "geometry", "state_independent", geo_state_independent },
    { "geometry", "strip_fan", geo_strip_fan },
    { "records", "settle_sequence", rec_settle_sequence },
    { "records", "aggregate", rec_aggregate },
    { "records", "empty_stack", rec_empty_stack },
    { "records", "exact_capacity", rec_exact_capacity },
    { "records", "one_word_short", rec_one_word_short },
    { "records", "overflow_recover", rec_overflow_recover },
    { "records", "stack_limits", rec_stack_limits },
    { "records", "api_misuse", rec_api_misuse },
    { "records", "invalid_primitive", rec_invalid_primitive },
    { "records", "two_contexts", rec_two_contexts },
    { "records", "resume_render", rec_resume_render },
    { "replay", "compile_only", rep_compile_only },
    { "replay", "compile_and_execute", rep_compile_and_execute },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
