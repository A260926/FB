/*
 * selection.c - Task tests for the GL_SELECT implementation.
 *
 * Registered through task_support/check.h in three groups:
 *   geometry - hit testing, clipping, depth, culling, validation
 *   records  - aggregation, buffer capacity, overflow, name stack rules
 *   replay   - display list timing and context isolation
 *
 * The file has no main(): task_support/driver.c drives the cases.
 */

#include "task_support/check.h"
#include "GL/gl.h"
#include "src/mytinygl.h"
#include <math.h>
#include <string.h>

#define DEPTH_ONE 4294967295.0

/* Opaque black is stored with the alpha channel set. */
#define CLEAR_PIXEL 0xFF000000u

static GLState *new_context(int w, int h)
{
    GLState *c = gl_create_context(w, h);
    task_check(c != NULL, "gl_create_context", __FILE__, __LINE__);
    if (c) {
        gl_make_current(c);
        /* Establish deterministic framebuffer contents. */
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                GL_STENCIL_BUFFER_BIT);
    }
    return c;
}

static void use_ortho(void)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    /* Object (x,y,z) maps straight to NDC (x,y,-z), w = 1. */
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static GLuint depth_word(double window_depth)
{
    double q = floor(window_depth * DEPTH_ONE + 0.5);
    if (q < 0.0) q = 0.0;
    if (q > DEPTH_ONE) q = DEPTH_ONE;
    return (GLuint)q;
}

/* Window depth for an object-space z under the ortho setup above. */
static double ortho_window_depth(double object_z)
{
    double ndc_z = -object_z;
    double d = (ndc_z + 1.0) * 0.5;
    if (d < 0.0) d = 0.0;
    if (d > 1.0) d = 1.0;
    return d;
}

static void draw_triangle(float z)
{
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, z);
    glVertex3f( 0.5f, -0.5f, z);
    glVertex3f( 0.0f,  0.5f, z);
    glEnd();
}

static size_t colored_pixels(GLState *c)
{
    size_t n = 0;
    size_t total = (size_t)c->framebuffer.width * c->framebuffer.height;
    for (size_t i = 0; i < total; i++) {
        if (c->framebuffer.color[i] != CLEAR_PIXEL) n++;
    }
    return n;
}

/* ============================ geometry ============================ */

static int case_point_frustum_and_depth(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    GLuint buf[16] = {0};
    glSelectBuffer(16, buf);
    TASK_CHECK(glRenderMode(GL_SELECT) == GL_RENDER);
    glInitNames();
    glPushName(1);

    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);     /* inside, depth 0.5 */
    glVertex3f(0.25f, 0.25f, 0.5f);  /* inside, nearer */
    glVertex3f(5.0f, 0.0f, 0.0f);    /* outside any plane */
    glEnd();

    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 1);
    TASK_CHECK(buf[0] == 1);
    TASK_CHECK(buf[3] == 1);
    TASK_CHECK(buf[1] == depth_word(ortho_window_depth(0.5)));
    TASK_CHECK(buf[2] == depth_word(ortho_window_depth(0.0)));

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_line_near_and_side_clip(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    GLuint buf[32] = {0};
    glSelectBuffer(32, buf);
    glRenderMode(GL_SELECT);
    glInitNames();

    /* Line punched through the near plane (object z = +1 is the near
     * plane here); the surviving part is still a non-zero segment. */
    glPushName(1);
    glBegin(GL_LINES);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glVertex3f(0.0f, 0.0f, 2.0f);
    glEnd();
    glPopName();

    /* Line crossing the left side plane. */
    glPushName(2);
    glBegin(GL_LINE_STRIP);
    glVertex3f(-2.0f, 0.0f, 0.0f);
    glVertex3f( 0.0f, 0.0f, 0.0f);
    glEnd();
    glPopName();

    /* Fully outside: no hit, no record for name 3. */
    glPushName(3);
    glBegin(GL_LINE_LOOP);
    glVertex3f(-2.0f, -2.0f, 0.0f);
    glVertex3f(-1.5f, -2.0f, 0.0f);
    glVertex3f(-1.75f, -1.5f, 0.0f);
    glEnd();
    glPopName();

    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 2);
    TASK_CHECK(buf[0] == 1 && buf[3] == 1);
    size_t off = 4;
    TASK_CHECK(buf[off] == 1 && buf[off + 3] == 2);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_degenerate_line_no_hit(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    GLuint buf[8] = {0};
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(1);

    /* Zero-length segment inside the frustum: no non-zero part survives. */
    glBegin(GL_LINES);
    glVertex3f(0.1f, 0.1f, 0.0f);
    glVertex3f(0.1f, 0.1f, 0.0f);
    glEnd();

    TASK_CHECK(glRenderMode(GL_RENDER) == 0);
    TASK_CHECK(buf[0] == 0);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_unequal_w_perspective(void)
{
    GLState *c = new_context(32, 32);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 10.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    GLuint buf[16] = {0};
    glSelectBuffer(16, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(9);

    /* Eye z = -2 gives w = 2 and NDC z = 1/9; eye z = -5 gives w = 5
     * and NDC z = 7/9. The vertices deliberately have unequal w. */
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.5f, -1.5f, -2.0f);
    glVertex3f( 1.5f, -1.5f, -2.0f);
    glVertex3f( 0.0f,  1.5f, -5.0f);
    glEnd();

    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 1);
    double d_near = ((1.0 / 9.0) + 1.0) * 0.5;  /* 5/9 */
    double d_far  = ((7.0 / 9.0) + 1.0) * 0.5;  /* 8/9 */
    /* The pipeline transforms in float32, whose ULP near these magnitudes
     * is 256-512 GLuint units; tolerate two ULPs while keeping the order. */
    GLuint expect_near = depth_word(d_near);
    GLuint expect_far = depth_word(d_far);
    GLuint diff_near = buf[1] > expect_near ? buf[1] - expect_near
                                            : expect_near - buf[1];
    GLuint diff_far = buf[2] > expect_far ? buf[2] - expect_far
                                          : expect_far - buf[2];
    TASK_CHECK(diff_near <= 1024u);
    TASK_CHECK(diff_far <= 1024u);
    TASK_CHECK(buf[1] < buf[2]);
    TASK_CHECK(buf[3] == 9);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_subpixel_triangle_hits(void)
{
    GLState *c = new_context(64, 64);
    use_ortho();

    /* A triangle far smaller than a pixel: the normal rasterizer covers
     * no pixel center, but selection must still register a geometric hit. */
    const float tri[6] = {
        0.0010f, 0.0010f,
        0.0025f, 0.0010f,
        0.0010f, 0.0025f
    };

    GLuint buf[8] = {0};
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(4);
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) glVertex3f(tri[2 * i], tri[2 * i + 1], 0.0f);
    glEnd();
    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 1);
    TASK_CHECK(buf[0] == 1 && buf[3] == 4);

    /* Selection must leave every framebuffer byte untouched. */
    TASK_CHECK(colored_pixels(c) == 0);

    /* The same triangle in render mode rasterizes nothing, proving the
     * selection hit really is geometry-based rather than pixel-based. */
    glColor3f(1.0f, 1.0f, 1.0f);
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) glVertex3f(tri[2 * i], tri[2 * i + 1], 0.0f);
    glEnd();
    TASK_CHECK(colored_pixels(c) == 0);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_occluded_face_still_hits(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();
    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    GLuint buf[32] = {0};
    glSelectBuffer(32, buf);
    glRenderMode(GL_SELECT);
    glInitNames();

    /* Nearer face first; the second face loses the depth test in ordinary
     * rendering yet must still produce its own selection hit. */
    glPushName(10);
    draw_triangle(0.2f);
    glPopName();
    glPushName(11);
    draw_triangle(-0.3f);
    glPopName();

    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 2);
    TASK_CHECK(buf[3] == 10);
    TASK_CHECK(buf[7] == 11);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_reverse_depth_range(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();
    glDepthRange(1.0, 0.0);  /* reversed: nearer geometry maps higher */

    GLuint buf[8] = {0};
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(5);

    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);   /* base 0.5, reversed -> 0.5 */
    glVertex3f(0.0f, 0.0f, 0.5f);   /* base 0.25, reversed -> 0.75 */
    glEnd();

    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 1);
    /* Reversed range: nearer geometry carries the larger mapped value,
     * and extrema are still taken on the mapped values. */
    TASK_CHECK(buf[1] == depth_word(0.5));
    TASK_CHECK(buf[2] == depth_word(0.75));

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_clipped_depth_extrema(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    GLuint buf[8] = {0};
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(6);

    /* Spans both near and far; after clipping the mapped depth extrema
     * are exactly 0 and 1. */
    glBegin(GL_TRIANGLES);
    glVertex3f(-2.0f, -2.0f,  2.0f);
    glVertex3f( 2.0f, -2.0f,  2.0f);
    glVertex3f( 0.0f,  2.0f, -2.0f);
    glEnd();

    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 1);
    TASK_CHECK(buf[1] == 0);
    TASK_CHECK(buf[2] == 4294967295u);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_front_face_culling(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();
    glEnable(GL_CULL_FACE);  /* default: cull GL_BACK, front GL_CCW */

    GLuint buf[16] = {0};
    glSelectBuffer(16, buf);
    glRenderMode(GL_SELECT);
    glInitNames();

    /* Counter-clockwise in NDC: front facing, survives. */
    glPushName(1);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f( 0.5f, -0.5f, 0.0f);
    glVertex3f( 0.0f,  0.5f, 0.0f);
    glEnd();
    glPopName();

    /* Clockwise: back facing, culled, no record. */
    glPushName(2);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f( 0.0f,  0.5f, 0.0f);
    glVertex3f( 0.5f, -0.5f, 0.0f);
    glEnd();
    glPopName();

    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 1);
    TASK_CHECK(buf[0] == 1 && buf[3] == 1);

    /* Flip front-face convention: the formerly culled winding now hits. */
    glFrontFace(GL_CW);
    memset(buf, 0, sizeof(buf));
    glSelectBuffer(16, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(3);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f( 0.0f,  0.5f, 0.0f);
    glVertex3f( 0.5f, -0.5f, 0.0f);
    glEnd();
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);
    TASK_CHECK(buf[3] == 3);

    /* Culling both faces removes everything. */
    glCullFace(GL_FRONT_AND_BACK);
    memset(buf, 0, sizeof(buf));
    glSelectBuffer(16, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(4);
    draw_triangle(0.0f);
    TASK_CHECK(glRenderMode(GL_RENDER) == 0);
    TASK_CHECK(buf[0] == 0);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_degenerate_triangle_no_hit(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    GLuint buf[8] = {0};
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(1);

    /* Three collinear points: the clipped polygon has no positive area. */
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, 0.0f, 0.0f);
    glVertex3f( 0.0f, 0.0f, 0.0f);
    glVertex3f( 0.5f, 0.0f, 0.0f);
    glEnd();

    TASK_CHECK(glRenderMode(GL_RENDER) == 0);
    TASK_CHECK(buf[0] == 0);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_unsupported_primitive_error(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    GLuint buf[8] = {0};
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(7);
    draw_triangle(0.0f);

    /* GL_QUADS is not selectable: error, mode and results preserved. */
    glBegin(GL_QUADS);
    TASK_CHECK(glGetError() == GL_INVALID_ENUM);
    GLint mode;
    glGetIntegerv(GL_RENDER_MODE, &mode);
    TASK_CHECK(mode == GL_SELECT);

    glEnd(); /* no begin was started, reports its own error */
    TASK_CHECK(glGetError() == GL_INVALID_OPERATION);

    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 1);
    TASK_CHECK(buf[3] == 7);

    /* The same rejection applies on the vertex-array entry points, and
     * must happen before any vertex is consumed. */
    static const GLfloat quad_verts[] = {
        -0.5f, -0.5f, 0.0f,  0.5f, -0.5f, 0.0f,
         0.5f,  0.5f, 0.0f, -0.5f,  0.5f, 0.0f
    };
    glVertexPointer(3, GL_FLOAT, 0, quad_verts);
    glEnableClientState(GL_VERTEX_ARRAY);

    GLuint buf2[8] = {0};
    glSelectBuffer(8, buf2);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(8);
    glDrawArrays(GL_QUADS, 0, 4);
    TASK_CHECK(glGetError() == GL_INVALID_ENUM);
    static const GLubyte qidx[] = { 0, 1, 2, 3 };
    glDrawElements(GL_POLYGON, 4, GL_UNSIGNED_BYTE, qidx);
    TASK_CHECK(glGetError() == GL_INVALID_ENUM);
    TASK_CHECK(glRenderMode(GL_RENDER) == 0);
    TASK_CHECK(buf2[0] == 0);

    /* The buffer binding persists across mode switches until replaced. */
    GLint bound_size = -1;
    glGetIntegerv(GL_SELECTION_BUFFER_SIZE, &bound_size);
    TASK_CHECK(bound_size == 8);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_draw_arrays_and_elements_chain(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    static const GLfloat verts[] = {
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
         0.0f,  0.5f, 0.0f,
         0.0f,  0.0f, 0.0f
    };
    static const GLubyte indices[] = { 0, 1, 2, 3 };

    glVertexPointer(3, GL_FLOAT, 0, verts);
    glEnableClientState(GL_VERTEX_ARRAY);

    GLuint buf[16] = {0};
    glSelectBuffer(16, buf);
    glRenderMode(GL_SELECT);
    glInitNames();

    glPushName(20);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glPopName();
    glPushName(21);
    glDrawElements(GL_POINTS, 1, GL_UNSIGNED_BYTE, indices + 3);
    glPopName();

    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 2);
    TASK_CHECK(buf[3] == 20);
    TASK_CHECK(buf[7] == 21);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

/* ============================= records ============================ */

static int case_aggregate_across_draws(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    GLuint buf[16] = {0};
    glSelectBuffer(16, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(1);

    /* Several draws under one name, including glEnd calls in between:
     * they must merge into a single record with merged depth extrema. */
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);    /* depth 0.5 */
    glEnd();
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.5f);   /* depth 0.25 */
    glEnd();
    draw_triangle(-0.5f);            /* depth 0.75 */

    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 1);
    TASK_CHECK(buf[0] == 1);
    TASK_CHECK(buf[1] == depth_word(ortho_window_depth(0.5)));
    TASK_CHECK(buf[2] == depth_word(ortho_window_depth(-0.5)));
    TASK_CHECK(buf[3] == 1);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_empty_name_stack_record(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    GLuint buf[8] = {0};
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    /* Deliberately no glInitNames/push: empty stack still records. */
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();

    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 1);
    TASK_CHECK(buf[0] == 0);
    TASK_CHECK(buf[1] == depth_word(0.5));
    TASK_CHECK(buf[2] == depth_word(0.5));

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_exact_capacity(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    /* One record with one name needs exactly 4 words. */
    GLuint buf[4] = {0};
    glSelectBuffer(4, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(42);
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();

    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 1);
    TASK_CHECK(buf[0] == 1 && buf[1] == depth_word(0.5) &&
               buf[2] == depth_word(0.5) && buf[3] == 42);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_one_word_short_overflows(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    enum { SENTINEL = 0xDEADBEEFu };
    GLuint buf[4] = { SENTINEL, SENTINEL, SENTINEL, SENTINEL };
    glSelectBuffer(3, buf);  /* one word short of the 4-word record */
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(42);
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();

    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == -1);
    for (int i = 0; i < 4; i++) TASK_CHECK(buf[i] == SENTINEL);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_overflow_lock_and_recovery(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    /* Zero capacity is legal; the first record that needs writing overflows. */
    glSelectBuffer(0, NULL);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(1);
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();
    /* Even after the first overflow, more draws keep the pass locked. */
    glPushName(2);
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();
    TASK_CHECK(glRenderMode(GL_RENDER) == -1);

    /* Rebind a usable buffer in render mode, re-enter: results restart
     * from empty and the pass succeeds. */
    GLuint fresh[16] = {0};
    glSelectBuffer(16, fresh);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(3);
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();
    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 1);
    TASK_CHECK(fresh[0] == 1 && fresh[3] == 3);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_name_changes_split_records(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    GLuint buf[32] = {0};
    glSelectBuffer(32, buf);
    glRenderMode(GL_SELECT);
    glInitNames();

    glPushName(100);
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();
    glLoadName(200);  /* replaces top, splits the record */
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();
    glPushName(201);  /* deeper stack, another split */
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();
    glPopName();

    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 3);
    TASK_CHECK(buf[0] == 1 && buf[3] == 100);
    TASK_CHECK(buf[4] == 1 && buf[7] == 200);
    TASK_CHECK(buf[8] == 2 && buf[11] == 200 && buf[12] == 201);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_name_stack_and_entry_validation(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    /* glSelectBuffer argument validation, state must remain unchanged. */
    GLuint buf[16] = {0};
    glSelectBuffer(-1, buf);
    TASK_CHECK(glGetError() == GL_INVALID_VALUE);
    glSelectBuffer(4, NULL);
    TASK_CHECK(glGetError() == GL_INVALID_VALUE);
    glSelectBuffer(16, buf);

    /* Bad render mode. */
    TASK_CHECK(glRenderMode(0x1234) == GL_RENDER);
    TASK_CHECK(glGetError() == GL_INVALID_ENUM);

    TASK_CHECK(glRenderMode(GL_SELECT) == GL_RENDER);
    /* Re-binding while selecting is rejected. */
    GLuint other[4] = {0};
    glSelectBuffer(4, other);
    TASK_CHECK(glGetError() == GL_INVALID_OPERATION);

    /* Name operations between glBegin/glEnd are rejected without effect. */
    glInitNames();
    glBegin(GL_POINTS);
    glInitNames();
    TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
    glPushName(1);
    TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
    glPopName();
    TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
    glLoadName(1);
    TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
    glEnd();
    GLint depth = -1;
    glGetIntegerv(GL_NAME_STACK_DEPTH, &depth);
    TASK_CHECK(depth == 0);

    /* Load on an empty stack. */
    glLoadName(9);
    TASK_CHECK(glGetError() == GL_INVALID_OPERATION);

    /* Fill to capacity 64, then overflow. */
    for (int i = 0; i < 64; i++) glPushName((GLuint)i);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    glGetIntegerv(GL_NAME_STACK_DEPTH, &depth);
    TASK_CHECK(depth == 64);
    glPushName(64);
    TASK_CHECK(glGetError() == GL_STACK_OVERFLOW);
    glGetIntegerv(GL_NAME_STACK_DEPTH, &depth);
    TASK_CHECK(depth == 64);

    /* Drain, then underflow. */
    for (int i = 0; i < 64; i++) glPopName();
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    glPopName();
    TASK_CHECK(glGetError() == GL_STACK_UNDERFLOW);

    TASK_CHECK(glRenderMode(GL_RENDER) == 0);

    /* In render mode name ops never touch the stack, but the binding and
     * mode stay usable for the next selection pass. */
    glPushName(77);
    TASK_CHECK(glGetError() == GL_NO_ERROR);
    glGetIntegerv(GL_NAME_STACK_DEPTH, &depth);
    TASK_CHECK(depth == 0);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

/* ============================== replay ============================ */

static int case_compile_list_played_in_select(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    GLuint list = glGenLists(1);
    TASK_CHECK(list != 0);

    /* GL_COMPILE: name ops and geometry are only recorded. */
    glNewList(list, GL_COMPILE);
    glInitNames();
    glPushName(300);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f( 0.5f, -0.5f, 0.0f);
    glVertex3f( 0.0f,  0.5f, 0.0f);
    glEnd();
    glPopName();
    glEndList();

    GLint depth = -1;
    glGetIntegerv(GL_NAME_STACK_DEPTH, &depth);
    TASK_CHECK(depth == 0);  /* compile did not execute */

    GLuint buf[16] = {0};
    glSelectBuffer(16, buf);
    glRenderMode(GL_SELECT);
    glCallList(list);  /* replay executes against the current (select) mode */
    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 1);
    TASK_CHECK(buf[0] == 1 && buf[3] == 300);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_compile_and_execute_then_replay(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    GLuint list = glGenLists(1);
    TASK_CHECK(list != 0);

    /* Building in render mode: recorded and executed, but name ops are
     * no-ops on the name stack outside GL_SELECT. */
    glNewList(list, GL_COMPILE_AND_EXECUTE);
    glInitNames();
    glPushName(301);
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();
    glPopName();
    glEndList();

    GLint depth = -1;
    glGetIntegerv(GL_NAME_STACK_DEPTH, &depth);
    TASK_CHECK(depth == 0);

    GLuint buf[16] = {0};
    glSelectBuffer(16, buf);
    glRenderMode(GL_SELECT);
    glCallList(list);
    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 1);
    TASK_CHECK(buf[3] == 301);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_repeated_playback_aggregates(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    /* The list only draws; the name stack is held outside, so two
     * playbacks share one stack state and merge into a single record. */
    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();
    glEndList();

    GLuint buf[16] = {0};
    glSelectBuffer(16, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(400);
    glCallList(list);
    glCallList(list);
    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 1);
    TASK_CHECK(buf[0] == 1 && buf[3] == 400);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_playback_with_name_ops_flushes_per_pass(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();

    /* A list that changes the name stack itself settles a record each
     * time it is replayed, so repeated calls stay independent. */
    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glInitNames();
    glPushName(401);
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();
    glPopName();
    glEndList();

    GLuint buf[16] = {0};
    glSelectBuffer(16, buf);
    glRenderMode(GL_SELECT);
    glCallList(list);
    glCallList(list);
    GLint hits = glRenderMode(GL_RENDER);
    TASK_CHECK(hits == 2);
    TASK_CHECK(buf[0] == 1 && buf[3] == 401);
    TASK_CHECK(buf[4] == 1 && buf[7] == 401);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

static int case_two_contexts_stay_independent(void)
{
    GLState *a = gl_create_context(32, 32);
    GLState *b = gl_create_context(32, 32);
    TASK_CHECK(a != NULL && b != NULL);

    GLuint bufa[16] = {0};
    GLuint bufb[16] = {0};

    gl_make_current(a);
    use_ortho();
    glSelectBuffer(16, bufa);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(11);
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();

    /* Interleave a second context mid-pass. */
    gl_make_current(b);
    use_ortho();
    glSelectBuffer(16, bufb);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(22);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f( 0.5f, -0.5f, 0.0f);
    glVertex3f( 0.0f,  0.5f, 0.0f);
    glEnd();
    GLint hits_b = glRenderMode(GL_RENDER);
    TASK_CHECK(hits_b == 1);
    TASK_CHECK(bufb[3] == 22);

    /* Back to the first context: its pass is exactly where it was left. */
    gl_make_current(a);
    GLint hits_a = glRenderMode(GL_RENDER);
    TASK_CHECK(hits_a == 1);
    TASK_CHECK(bufa[3] == 11);

    gl_make_current(NULL);
    gl_destroy_context(a);
    gl_destroy_context(b);
    return 0;
}

static int case_render_after_select_and_no_buffer_writes(void)
{
    GLState *c = new_context(32, 32);
    use_ortho();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    size_t pixels_before = colored_pixels(c);
    TASK_CHECK(pixels_before == 0);

    GLuint buf[8] = {0};
    glSelectBuffer(8, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(50);
    glColor3f(1.0f, 1.0f, 1.0f);
    draw_triangle(0.0f);
    TASK_CHECK(glRenderMode(GL_RENDER) == 1);

    /* The selection pass changed no color, depth or stencil storage. */
    TASK_CHECK(colored_pixels(c) == 0);
    size_t total = (size_t)c->framebuffer.width * c->framebuffer.height;
    for (size_t i = 0; i < total; i++) {
        TASK_CHECK(c->framebuffer.depth[i] == 1.0f);
        TASK_CHECK(c->framebuffer.stencil[i] == 0);
    }

    /* Ordinary drawing still works after leaving selection. */
    glColor3f(1.0f, 0.0f, 0.0f);
    draw_triangle(0.0f);
    TASK_CHECK(colored_pixels(c) > 50);

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}

/* ========================= registration =========================== */

const struct task_case task_cases[] = {
    /* geometry */
    { "geometry", "point_frustum_and_depth",     case_point_frustum_and_depth },
    { "geometry", "line_near_and_side_clip",     case_line_near_and_side_clip },
    { "geometry", "degenerate_line_no_hit",      case_degenerate_line_no_hit },
    { "geometry", "unequal_w_perspective",       case_unequal_w_perspective },
    { "geometry", "subpixel_triangle_hits",      case_subpixel_triangle_hits },
    { "geometry", "degenerate_triangle_no_hit",  case_degenerate_triangle_no_hit },
    { "geometry", "occluded_face_still_hits",    case_occluded_face_still_hits },
    { "geometry", "reverse_depth_range",         case_reverse_depth_range },
    { "geometry", "clipped_depth_extrema",       case_clipped_depth_extrema },
    { "geometry", "front_face_culling",          case_front_face_culling },
    { "geometry", "unsupported_primitive_error", case_unsupported_primitive_error },
    { "geometry", "draw_arrays_elements_chain",  case_draw_arrays_and_elements_chain },

    /* records */
    { "records", "aggregate_across_draws",       case_aggregate_across_draws },
    { "records", "empty_name_stack_record",      case_empty_name_stack_record },
    { "records", "exact_capacity",               case_exact_capacity },
    { "records", "one_word_short_overflows",     case_one_word_short_overflows },
    { "records", "overflow_lock_and_recovery",   case_overflow_lock_and_recovery },
    { "records", "name_changes_split_records",   case_name_changes_split_records },
    { "records", "name_stack_validation",        case_name_stack_and_entry_validation },

    /* replay */
    { "replay", "compile_list_played_in_select",     case_compile_list_played_in_select },
    { "replay", "compile_and_execute_then_replay",   case_compile_and_execute_then_replay },
    { "replay", "repeated_playback_aggregates",      case_repeated_playback_aggregates },
    { "replay", "playback_with_name_ops_flushes",    case_playback_with_name_ops_flushes_per_pass },
    { "replay", "two_contexts_stay_independent",     case_two_contexts_stay_independent },
    { "replay", "render_after_select_no_writes",     case_render_after_select_and_no_buffer_writes },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
