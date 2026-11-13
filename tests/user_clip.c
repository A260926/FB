/*
 * Regression tests for GL_CLIP_PLANE0 .. GL_CLIP_PLANE5.
 *
 * The tests are grouped into:
 *   plane_state - API state, coefficient freezing and query semantics
 *   geometry    - points, lines, multiple planes, list modes, matrix changes
 *   render      - analytic checks of filled/textured/zbuffered output
 *
 * All expected values are derived analytically from the transformed geometry,
 * never by comparing against a previous run of the same code.
 */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <TGL/gl.h>
#include "zbuffer.h"

#include "task_support/check.h"

#define FB_W 64
#define FB_H 64

static ZBuffer *zb;

static void setup(void)
{
    zb = ZB_open(FB_W, FB_H, ZB_MODE_RGBA, NULL);
    if (!zb)
        exit(2);
    glInit(zb);
    glViewport(0, 0, FB_W, FB_H);
    glClearColor(0, 0, 0, 0);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glShadeModel(GL_SMOOTH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void teardown(void)
{
    glClose();
    ZB_close(zb);
    zb = NULL;
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/* Window coordinate of a normalized device coordinate.  TinyGL's viewport
 * transform maps NDC [-1,1] to [0,width-1] (trans = (size-0.5)/2). */
static double win_x(double ndc_x)
{
    return ndc_x * ((FB_W - 0.5) / 2.0) + ((FB_W - 0.5) / 2.0);
}

static double win_y(double ndc_y)
{
    return ndc_y * (-(FB_H - 0.5) / 2.0) + ((FB_H - 0.5) / 2.0);
}

/* Reference rasterizer rule is integer truncation of the fixed point
 * coordinate, i.e. floor for positive values and ceil towards zero for
 * negative ones.  The clipped polygons in these tests stay well inside the
 * framebuffer, so truncation toward zero is enough. */
static int pixel_x(double ndc_x)
{
    double v = win_x(ndc_x);
    return v >= 0 ? (int) v : -(int) (-v);
}

static int pixel_y(double ndc_y)
{
    double v = win_y(ndc_y);
    return v >= 0 ? (int) v : -(int) (-v);
}

static PIXEL pixel_at(int x, int y)
{
    if (x < 0 || x >= FB_W || y < 0 || y >= FB_H)
        return 0;
    return zb->pbuf[y * zb->xsize + x];
}

static int pixel_is_filled(int x, int y)
{
    return pixel_at(x, y) != 0;
}

static int count_filled(void)
{
    int n = 0, i;
    for (i = 0; i < zb->xsize * zb->ysize; i++)
        if (zb->pbuf[i])
            n++;
    return n;
}

static int count_depth_written(void)
{
    int n = 0, i;
    for (i = 0; i < zb->xsize * zb->ysize; i++)
        if (zb->zbuf[i])
            n++;
    return n;
}

static void get_rgb(PIXEL p, int *r, int *g, int *b)
{
    *r = (int) GET_RED(p);
    *g = (int) GET_GREEN(p);
    *b = (int) GET_BLUE(p);
}

/* Slightly darker than the pure colour to account for the fixed point
 * conversion in the rasterizer. */
static int color_near(PIXEL p, int r, int g, int b)
{
    int pr, pg, pb;
    get_rgb(p, &pr, &pg, &pb);
    return abs(pr - r) <= 4 && abs(pg - g) <= 4 && abs(pb - b) <= 4;
}

/* Split the plane coefficients as they were frozen in eye space. */
static void get_plane(int i, GLdouble *eq)
{
    glGetClipPlane(GL_CLIP_PLANE0 + i, eq);
}

static int plane_enabled(int i)
{
    GLint v = 0;
    glGetIntegerv(GL_CLIP_PLANE0 + i, &v);
    return v != 0;
}

/* A triangle with a 1 unit "radius" covering roughly the middle of the
 * framebuffer.  Vertices are object coordinates z = 0. */
static void draw_big_triangle(void)
{
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.8f, -0.8f, 0.0f);
    glVertex3f(0.8f, -0.8f, 0.0f);
    glVertex3f(0.0f, 0.8f, 0.0f);
    glEnd();
}

/* ------------------------------------------------------------------ */
/* plane_state                                                         */
/* ------------------------------------------------------------------ */

static int test_defaults(void)
{
    int i;
    GLint max_planes = 0;
    GLdouble eq[4];

    setup();

    glGetIntegerv(GL_MAX_CLIP_PLANES, &max_planes);
    TASK_CHECK(max_planes == 6);

    for (i = 0; i < 6; i++) {
        TASK_CHECK(!plane_enabled(i));
        get_plane(i, eq);
        TASK_CHECK(eq[0] == 0.0 && eq[1] == 0.0 && eq[2] == 0.0 &&
                   eq[3] == 0.0);
    }

    teardown();
    return 0;
}

static int test_enable_state(void)
{
    int i;

    setup();

    for (i = 0; i < 6; i++) {
        glEnable(GL_CLIP_PLANE0 + i);
        TASK_CHECK(plane_enabled(i));
    }
    for (i = 0; i < 6; i++) {
        TASK_CHECK(plane_enabled(i));
        glDisable(GL_CLIP_PLANE0 + i);
        TASK_CHECK(!plane_enabled(i));
    }

    /* other caps must not disturb the plane state */
    glEnable(GL_CLIP_PLANE3);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    TASK_CHECK(plane_enabled(3));
    TASK_CHECK(!plane_enabled(2));

    teardown();
    return 0;
}

static int test_freeze_identity(void)
{
    GLdouble eq_in[4] = {1, 0, 0, -0.25};
    GLdouble eq_out[4];

    setup();

    glClipPlane(GL_CLIP_PLANE0, eq_in);
    get_plane(0, eq_out);
    TASK_CHECK(fabs(eq_out[0] - 1.0) < 1e-6);
    TASK_CHECK(fabs(eq_out[1]) < 1e-6);
    TASK_CHECK(fabs(eq_out[2]) < 1e-6);
    TASK_CHECK(fabs(eq_out[3] + 0.25) < 1e-6);

    teardown();
    return 0;
}

static int test_freeze_translation(void)
{
    /* object-space plane x >= 0 with MODELVIEW translate(+0.5): an object
     * point at x_obj maps to x_eye = x_obj + 0.5, so the plane becomes
     * x_eye - 0.5 >= 0. */
    GLdouble eq_in[4] = {1, 0, 0, 0};
    GLdouble eq[4];

    setup();

    glTranslatef(0.5f, 0, 0);
    glClipPlane(GL_CLIP_PLANE0, eq_in);
    get_plane(0, eq);
    TASK_CHECK(fabs(eq[0] - 1.0) < 1e-5);
    TASK_CHECK(fabs(eq[1]) < 1e-5);
    TASK_CHECK(fabs(eq[2]) < 1e-5);
    TASK_CHECK(fabs(eq[3] + 0.5) < 1e-5);

    teardown();
    return 0;
}

static int test_freeze_rotation(void)
{
    /* rotate 90 degrees around +Z: object point (1,0,0) goes to (0,1,0).
     * The object-space plane x >= 0 therefore becomes the eye-space plane
     * y >= 0 after the inverse transpose transform. */
    GLdouble eq_in[4] = {1, 0, 0, 0};
    GLdouble eq[4];

    setup();

    glRotatef(90.0f, 0, 0, 1);
    glClipPlane(GL_CLIP_PLANE0, eq_in);
    get_plane(0, eq);
    TASK_CHECK(fabs(eq[0]) < 1e-5);
    TASK_CHECK(fabs(eq[1] - 1.0) < 1e-5);
    TASK_CHECK(fabs(eq[2]) < 1e-5);
    TASK_CHECK(fabs(eq[3]) < 1e-5);

    teardown();
    return 0;
}

static int test_freeze_scale_proportional(void)
{
    /* Plane coefficients are defined up to a positive scale: doubling the
     * object-space coefficients doubles the frozen eye-space ones. */
    GLdouble eq_a[4] = {1, 2, 3, 4};
    GLdouble eq_b[4] = {2, 4, 6, 8};
    GLdouble a[4], b[4];
    int i;

    setup();

    glTranslatef(0.3f, -0.2f, 0.1f);
    glRotatef(35.0f, 1, 2, 3);
    glScalef(2.0f, 3.0f, 0.5f);

    glClipPlane(GL_CLIP_PLANE0, eq_a);
    glClipPlane(GL_CLIP_PLANE1, eq_b);
    get_plane(0, a);
    get_plane(1, b);

    for (i = 0; i < 4; i++)
        TASK_CHECK(fabs(b[i] - 2.0 * a[i]) < 1e-4);

    teardown();
    return 0;
}

static int test_freeze_until_redefine(void)
{
    GLdouble eq_in[4] = {1, 0, 0, 0};
    GLdouble eq[4];

    setup();

    glTranslatef(0.5f, 0, 0);
    glClipPlane(GL_CLIP_PLANE2, eq_in);
    get_plane(2, eq);
    TASK_CHECK(fabs(eq[3] + 0.5) < 1e-5);

    /* changing MODELVIEW moves geometry, not the frozen plane */
    glLoadIdentity();
    glTranslatef(-2.0f, 1.0f, 0);
    get_plane(2, eq);
    TASK_CHECK(fabs(eq[0] - 1.0) < 1e-5);
    TASK_CHECK(fabs(eq[1]) < 1e-5);
    TASK_CHECK(fabs(eq[3] + 0.5) < 1e-5);

    /* redefining re-captures the matrix in effect at that moment: the
     * current translation is -2, so the plane becomes x_eye + 2 >= 0 */
    glClipPlane(GL_CLIP_PLANE2, eq_in);
    get_plane(2, eq);
    TASK_CHECK(fabs(eq[3] - 2.0) < 1e-5);

    teardown();
    return 0;
}

static int test_clip_disabled_by_default_draws(void)
{
    int before, after;

    setup();
    before = count_filled();
    draw_big_triangle();
    after = count_filled();
    TASK_CHECK(before == 0);
    TASK_CHECK(after > 1000);

    /* a defined but disabled plane must not clip */
    {
        GLdouble eq[4] = {1, 0, 0, 0};
        glClipPlane(GL_CLIP_PLANE0, eq);
    }
    glClear(GL_COLOR_BUFFER_BIT);
    draw_big_triangle();
    TASK_CHECK(count_filled() == after);

    teardown();
    return 0;
}

/* ------------------------------------------------------------------ */
/* geometry                                                            */
/* ------------------------------------------------------------------ */

static int test_points(void)
{
    GLdouble eq[4] = {1, 0, 0, 0};

    setup();
    glClipPlane(GL_CLIP_PLANE0, eq);
    glEnable(GL_CLIP_PLANE0);

    glBegin(GL_POINTS);
    glVertex3f(-0.5f, 0.0f, 0); /* removed: x < 0 */
    glVertex3f(0.5f, 0.0f, 0);  /* kept */
    glVertex3f(0.0f, 0.5f, 0);  /* boundary: kept */
    glEnd();

    TASK_CHECK(!pixel_is_filled(pixel_x(-0.5), pixel_y(0.0)));
    TASK_CHECK(pixel_is_filled(pixel_x(0.5), pixel_y(0.0)));
    TASK_CHECK(pixel_is_filled(pixel_x(0.0), pixel_y(0.5)));

    teardown();
    return 0;
}

static int test_independent_lines(void)
{
    GLdouble eq[4] = {1, 0, 0, 0};

    setup();
    glClipPlane(GL_CLIP_PLANE0, eq);
    glEnable(GL_CLIP_PLANE0);

    /* horizontal line crossing the plane: left half is removed, the
     * intersection at x = 0 is kept and the line ends exactly on it */
    glColor3f(1, 1, 1);
    glBegin(GL_LINES);
    glVertex3f(-0.6f, 0.5f, 0);
    glVertex3f(0.6f, 0.5f, 0);
    glEnd();

    TASK_CHECK(!pixel_is_filled(pixel_x(-0.6), pixel_y(0.5)));
    TASK_CHECK(!pixel_is_filled(pixel_x(-0.2), pixel_y(0.5)));
    TASK_CHECK(pixel_is_filled(pixel_x(0.0), pixel_y(0.5)));
    TASK_CHECK(pixel_is_filled(pixel_x(0.2), pixel_y(0.5)));
    TASK_CHECK(pixel_is_filled(pixel_x(0.6), pixel_y(0.5)));

    /* a completely rejected line leaves nothing */
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_LINES);
    glVertex3f(-0.9f, -0.5f, 0);
    glVertex3f(-0.2f, -0.5f, 0);
    glEnd();
    TASK_CHECK(count_filled() == 0);

    teardown();
    return 0;
}

static int test_multiple_planes(void)
{
    /* Keep 0 <= x and y <= 0: a quadrant of a large triangle.  Vertices are
     * chosen so only the intersection polygon survives. */
    GLdouble px[4] = {1, 0, 0, 0};
    GLdouble ny[4] = {0, -1, 0, 0};

    setup();
    glClipPlane(GL_CLIP_PLANE0, px);
    glClipPlane(GL_CLIP_PLANE1, ny);
    glEnable(GL_CLIP_PLANE0);
    glEnable(GL_CLIP_PLANE1);

    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.9f, -0.9f, 0); /* both planes reject this point */
    glVertex3f(0.9f, -0.9f, 0);  /* y rejected */
    glVertex3f(-0.9f, 0.9f, 0);  /* x rejected */
    glEnd();

    /* Only the small triangle x>=0, y<=0, x+y<=0 survives.  Its corners are
     * (0,0), (0,-0.9) and (0.9,-0.9) in NDC. */
    TASK_CHECK(pixel_is_filled(pixel_x(0.1), pixel_y(-0.1)));
    TASK_CHECK(pixel_is_filled(pixel_x(0.5), pixel_y(-0.5)));
    TASK_CHECK(pixel_is_filled(pixel_x(0.3), pixel_y(-0.8)));
    TASK_CHECK(!pixel_is_filled(pixel_x(-0.2), pixel_y(-0.2)));
    TASK_CHECK(!pixel_is_filled(pixel_x(0.2), pixel_y(0.2)));
    TASK_CHECK(!pixel_is_filled(pixel_x(0.7), pixel_y(-0.1)));

    /* The edge generated at the x = 0 cut runs from (0,-0.9) to (0,0) and is
     * itself clipped by y <= 0: it must stop exactly at the origin.  With
     * line rendering the visible cut edge is the segment (0,-0.9)..(0,0). */
    glClear(GL_COLOR_BUFFER_BIT);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.9f, -0.9f, 0);
    glVertex3f(0.9f, -0.9f, 0);
    glVertex3f(-0.9f, 0.9f, 0);
    glEnd();
    {
        int x = pixel_x(0.0);
        int y_inside = pixel_y(-0.4);
        int y_outside = pixel_y(0.25);
        TASK_CHECK(pixel_is_filled(x, y_inside));
        TASK_CHECK(!pixel_is_filled(x, y_outside));
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.9f, -0.9f, 0);
    glVertex3f(0.9f, -0.9f, 0);
    glVertex3f(-0.9f, 0.9f, 0);
    glEnd();

    /* all six planes can be enabled at once; outside the region nothing is
     * drawn, inside it is */
    {
        GLdouble left[4] = {-1, 0, 0, -0.95};  /* x <= 0.95 */
        GLdouble top[4] = {0, 1, 0, -0.5};     /* y >= 0.5 */
        GLdouble near[4] = {0, 0, -1, 0.5};    /* z <= 0.5 */
        GLdouble far[4] = {0, 0, 1, 0.5};      /* z >= -0.5 */
        glDisable(GL_CLIP_PLANE0);
        glDisable(GL_CLIP_PLANE1);
        glClipPlane(GL_CLIP_PLANE2, left);
        glClipPlane(GL_CLIP_PLANE3, top);
        glClipPlane(GL_CLIP_PLANE4, near);
        glClipPlane(GL_CLIP_PLANE5, far);
        glEnable(GL_CLIP_PLANE2);
        glEnable(GL_CLIP_PLANE3);
        glEnable(GL_CLIP_PLANE4);
        glEnable(GL_CLIP_PLANE5);

        glClear(GL_COLOR_BUFFER_BIT);
        draw_big_triangle();
        /* above y = 0.5 the original triangle is empty */
        TASK_CHECK(count_filled() == 0);
    }

    teardown();
    return 0;
}

static int test_list_compile(void)
{
    GLuint list;
    GLdouble eq[4];
    int full, clipped;

    setup();

    full = 0;
    draw_big_triangle();
    full = count_filled();
    glClear(GL_COLOR_BUFFER_BIT);

    /* GL_COMPILE only records.  The list is compiled with a translated
     * modelview so that the captured matrix is distinguishable from the
     * current one at replay time. */
    list = glGenLists(1);
    glTranslatef(0.5f, 0, 0);
    glNewList(list, GL_COMPILE);
    {
        GLdouble p[4] = {1, 0, 0, 0};
        glClipPlane(GL_CLIP_PLANE0, p);
        glEnable(GL_CLIP_PLANE0);
    }
    glEndList();
    glLoadIdentity();

    /* the compile must not have changed state */
    TASK_CHECK(!plane_enabled(0));
    get_plane(0, eq);
    TASK_CHECK(eq[0] == 0 && eq[1] == 0 && eq[2] == 0 && eq[3] == 0);
    draw_big_triangle();
    TASK_CHECK(count_filled() == full);
    glClear(GL_COLOR_BUFFER_BIT);

    /* replay: the plane is captured with the matrix current at replay time.
     * The compile used translate(+0.5) (which would freeze d = -0.5); the
     * replay uses translate(-0.25), so d = +0.25 proves the replay-time
     * matrix was used. */
    glLoadIdentity();
    glTranslatef(-0.25f, 0, 0);
    glCallList(list);
    TASK_CHECK(plane_enabled(0));
    get_plane(0, eq);
    TASK_CHECK(fabs(eq[0] - 1.0) < 1e-5 && fabs(eq[3] - 0.25) < 1e-5);
    /* shift the geometry left; the frozen plane cuts off its left part */
    glTranslatef(-0.5f, 0, 0);

    draw_big_triangle();
    clipped = count_filled();
    TASK_CHECK(clipped > 0);
    TASK_CHECK(clipped < full);

    glDeleteLists(list, 1);
    teardown();
    return 0;
}

static int test_list_compile_and_execute(void)
{
    GLuint list;
    GLdouble eq[4];

    setup();

    list = glGenLists(1);
    glTranslatef(0.5f, 0, 0);
    glNewList(list, GL_COMPILE_AND_EXECUTE);
    {
        GLdouble p[4] = {1, 0, 0, 0};
        glClipPlane(GL_CLIP_PLANE0, p);
        glEnable(GL_CLIP_PLANE0);
    }
    glEndList();

    TASK_CHECK(plane_enabled(0));
    get_plane(0, eq);
    TASK_CHECK(fabs(eq[3] + 0.5) < 1e-5);

    /* settle the plane so that a later re-capture is easy to distinguish */
    glLoadIdentity();
    glClipPlane(GL_CLIP_PLANE0, (GLdouble[4]) {1, 0, 0, 0});
    get_plane(0, eq);
    TASK_CHECK(fabs(eq[3]) < 1e-5);

    /* calling the list again re-captures with the matrix at replay time */
    glTranslatef(-0.25f, 0, 0);
    glCallList(list);
    get_plane(0, eq);
    TASK_CHECK(fabs(eq[3] - 0.25) < 1e-5);

    glDeleteLists(list, 1);
    teardown();
    return 0;
}

static int test_matrix_change_after_definition(void)
{
    GLdouble eq[4] = {1, 0, 0, 0};
    int c_before, c_after;

    setup();
    glClipPlane(GL_CLIP_PLANE0, eq);
    glEnable(GL_CLIP_PLANE0);

    /* define at identity: half the triangle is clipped */
    draw_big_triangle();
    c_before = count_filled();
    TASK_CHECK(c_before > 0);

    /* moving the modelview does not move the plane.  Shifting the object to
     * the left by 0.4 moves the eye-space geometry left, so the frozen plane
     * at x >= 0 removes more of it. */
    glClear(GL_COLOR_BUFFER_BIT);
    glTranslatef(-0.4f, 0, 0);
    draw_big_triangle();
    c_after = count_filled();
    TASK_CHECK(c_after < c_before);

    /* shifting right by 0.6 moves the whole eye-space geometry onto the
     * kept side, so nothing is clipped and the triangle covers more pixels */
    glClear(GL_COLOR_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.6f, 0, 0);
    draw_big_triangle();
    TASK_CHECK(count_filled() > c_before);

    teardown();
    return 0;
}

/* ------------------------------------------------------------------ */
/* render                                                              */
/* ------------------------------------------------------------------ */

/* Analytic reference for the filled triangle above eye-space half plane
 * x >= 0.  Returns 1 if the NDC point (x,y) is inside the visible part of the
 * triangle whose corners are given in NDC. */
static int inside_triangle(double px, double py,
                           const double *xs, const double *ys)
{
    double d0 = (xs[1] - xs[0]) * (py - ys[0]) - (ys[1] - ys[0]) * (px - xs[0]);
    double d1 = (xs[2] - xs[1]) * (py - ys[1]) - (ys[2] - ys[1]) * (px - xs[1]);
    double d2 = (xs[0] - xs[2]) * (py - ys[2]) - (ys[0] - ys[2]) * (px - xs[2]);
    int neg = (d0 < 0) || (d1 < 0) || (d2 < 0);
    int pos = (d0 > 0) || (d1 > 0) || (d2 > 0);
    return !(neg && pos);
}

/* Triangle (-0.8,-0.8) (0.8,-0.8) (0,0.8) clipped by x >= 0.
 * Edge (-0.8,-0.8)->(0,0.8) reaches x = 0 only at its endpoint, so the
 * surviving polygon is the triangle (0,-0.8), (0.8,-0.8), (0,0.8). */
static void clipped_triangle_ndc(double *xs, double *ys)
{
    xs[0] = 0.0;
    ys[0] = -0.8;
    xs[1] = 0.8;
    ys[1] = -0.8;
    xs[2] = 0.0;
    ys[2] = 0.8;
}

/* Signed distance from (px,py) to the line through a and b, in NDC units. */
static double line_dist(double px, double py,
                        double ax, double ay, double bx, double by)
{
    double dx = bx - ax, dy = by - ay;
    double len = sqrt(dx * dx + dy * dy);
    if (len == 0)
        return 0;
    return ((dx) * (py - ay) - (dy) * (px - ax)) / len;
}

static int test_fill_analytic(void)
{
    double xs[3], ys[3];
    int x, y, deep_mismatch = 0, band_mismatch = 0, deep_inside = 0;
    int filled;
    double area_expected;
    GLdouble eq[4] = {1, 0, 0, 0};

    setup();
    glClipPlane(GL_CLIP_PLANE0, eq);
    glEnable(GL_CLIP_PLANE0);
    glColor3f(1, 0, 0);
    draw_big_triangle();
    clipped_triangle_ndc(xs, ys);

    for (y = 0; y < FB_H; y++) {
        for (x = 0; x < FB_W; x++) {
            double nx = ((double) x - (FB_W - 0.5) / 2.0) /
                        ((FB_W - 0.5) / 2.0);
            double ny = ((double) y - (FB_H - 0.5) / 2.0) /
                        (-(FB_H - 0.5) / 2.0);
            int want = inside_triangle(nx, ny, xs, ys) && nx >= 0.0;
            int got = pixel_is_filled(x, y);
            double d_edge;

            /* distance to the three analytic edges, converted to pixels */
            d_edge = fabs(line_dist(nx, ny, xs[0], ys[0], xs[1], ys[1]));
            if (fabs(line_dist(nx, ny, xs[1], ys[1], xs[2], ys[2])) < d_edge)
                d_edge =
                    fabs(line_dist(nx, ny, xs[1], ys[1], xs[2], ys[2]));
            if (fabs(line_dist(nx, ny, xs[2], ys[2], xs[0], ys[0])) < d_edge)
                d_edge =
                    fabs(line_dist(nx, ny, xs[2], ys[2], xs[0], ys[0]));
            d_edge *= (FB_W - 0.5) / 2.0;
            if (fabs(nx - 0.0) * (FB_W - 0.5) / 2.0 < d_edge)
                d_edge = fabs(nx) * (FB_W - 0.5) / 2.0;

            if (d_edge < 2.0) {
                if (want != got)
                    band_mismatch++;
                continue;
            }
            if (want)
                deep_inside++;
            if (want != got)
                deep_mismatch++;
            if (got)
                TASK_CHECK(color_near(pixel_at(x, y), 255, 0, 0));
        }
    }

    filled = count_filled();

    /* analytic area of the visible polygon in NDC, converted to pixels.
     * NDC area is 0.5 * base * height = 0.5 * 0.8 * 1.6 = 0.64. */
    area_expected = 0.64 * ((FB_W - 0.5) / 2.0) * ((FB_H - 0.5) / 2.0);


    /* Everything more than two pixels inside the analytic polygon is drawn
     * and nothing outside it is; the boundary band is left to the scan
     * converter's fixed point rule. */
    TASK_CHECK(deep_inside > 400);
    TASK_CHECK(deep_mismatch == 0);
    TASK_CHECK(band_mismatch < 100);
    /* filled area agrees with the analytic polygon area to within 10% */
    TASK_CHECK(fabs((double) filled - area_expected) < area_expected * 0.10);

    /* analytic interior/exterior probes, well away from the cut */
    TASK_CHECK(pixel_is_filled(pixel_x(0.2), pixel_y(-0.5)));
    TASK_CHECK(pixel_is_filled(pixel_x(0.1), pixel_y(0.2)));
    TASK_CHECK(!pixel_is_filled(pixel_x(-0.3), pixel_y(0.4)));
    TASK_CHECK(!pixel_is_filled(pixel_x(0.7), pixel_y(0.5)));

    teardown();
    return 0;
}

/* Colour of a point inside the clipped triangle, using the barycentric
 * weights of the clip-space vertices.  The clipped triangle is
 *   A' = (0,-0.6)  colour (0.5,0.5,0)   [midpoint of red and green]
 *   B  = (0.6,-0.6) colour (0,1,0)
 *   C  = (0,0.6)   colour (0,0,1) */
static void tricolor_expected(double px, double py,
                              double *r, double *g, double *b)
{
    double ax = 0.0, ay = -0.6;
    double bx = 0.6, by = -0.6;
    double cx = 0.0, cy = 0.6;
    double det = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    double u = ((px - ax) * (cy - ay) - (py - ay) * (cx - ax)) / det;
    double v = ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) / det;
    double w = 1.0 - u - v;

    *r = (w * 0.5) * 255.0;
    *g = (w * 0.5 + u * 1.0) * 255.0;
    *b = (v * 1.0) * 255.0;
}

static int test_tricolor_interpolation(void)
{
    GLdouble eq[4] = {1, 0, 0, 0};
    static const double probes[3][2] = {
        {0.05, 0.0},
        {0.45, -0.5},
        {0.05, 0.4},
    };
    int i;

    setup();
    glClipPlane(GL_CLIP_PLANE0, eq);
    glEnable(GL_CLIP_PLANE0);

    /* red left, green bottom-right, blue top */
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0);
    glVertex3f(-0.6f, -0.6f, 0);
    glColor3f(0, 1, 0);
    glVertex3f(0.6f, -0.6f, 0);
    glColor3f(0, 0, 1);
    glVertex3f(0.0f, 0.6f, 0);
    glEnd();

    for (i = 0; i < 3; i++) {
        int px = pixel_x(probes[i][0]);
        int py = pixel_y(probes[i][1]);
        double ndc_x = ((double) px - (FB_W - 0.5) / 2.0) /
                       ((FB_W - 0.5) / 2.0);
        double ndc_y = ((double) py - (FB_H - 0.5) / 2.0) /
                       (-(FB_H - 0.5) / 2.0);
        double er, eg, eb;
        PIXEL p = pixel_at(px, py);
        int r, g, b;

        TASK_CHECK(p != 0);
        tricolor_expected(ndc_x, ndc_y, &er, &eg, &eb);
        get_rgb(p, &r, &g, &b);
        /* the software rasterizer truncates the integer vertex positions, so
         * allow roughly one pixel worth of parameter error */
        TASK_CHECK(fabs(r - er) < 25);
        TASK_CHECK(fabs(g - eg) < 25);
        TASK_CHECK(fabs(b - eb) < 25);
    }

    /* the removed left vertex region stays black */
    TASK_CHECK(!pixel_is_filled(pixel_x(-0.4), pixel_y(-0.4)));

    teardown();
    return 0;
}

static int test_zbuffer(void)
{
    GLdouble eq[4] = {1, 0, 0, 0};
    int px, py;
    GLushort depth_far, depth_near;

    setup();
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glClipPlane(GL_CLIP_PLANE0, eq);
    glEnable(GL_CLIP_PLANE0);

    /* far (green) triangle first, then the near (white) one; both cover the
     * same clipped region x >= 0 with the same shape */
    glBegin(GL_TRIANGLES);
    glColor3f(0, 1, 0);
    glVertex3f(0.0f, -0.8f, 0.5f);
    glVertex3f(0.8f, -0.8f, 0.5f);
    glVertex3f(0.4f, 0.4f, 0.5f);
    glEnd();

    glBegin(GL_TRIANGLES);
    glColor3f(1, 1, 1);
    glVertex3f(0.0f, -0.8f, -0.5f);
    glVertex3f(0.8f, -0.8f, -0.5f);
    glVertex3f(0.4f, 0.4f, -0.5f);
    glEnd();

    /* clipped-away pixels must not write colour or depth.  (-0.4,0) is
     * outside the kept half plane. */
    px = pixel_x(-0.4);
    py = pixel_y(0.0);
    TASK_CHECK(zb->zbuf[py * zb->xsize + px] == 0);
    TASK_CHECK(!pixel_is_filled(px, py));

    /* inside the kept region the near triangle wins the depth test */
    px = pixel_x(0.4);
    py = pixel_y(-0.5);
    depth_far = zb->zbuf[py * zb->xsize + px];
    TASK_CHECK(depth_far != 0);
    TASK_CHECK(color_near(pixel_at(px, py), 255, 255, 255));

    /* the depth value equals that of the near plane: z = -0.5 in eye space
     * maps to ndc z = -0.5, and the viewport maps -1..1 onto
     * 0 .. 2^(ZB_Z_BITS+ZB_POINT_Z_FRAC_BITS)-1. */
    depth_near = (GLushort) (((-0.5 + 1.0) * 0.5) *
                            ((1 << (ZB_Z_BITS + ZB_POINT_Z_FRAC_BITS)) - 1));
    TASK_CHECK(abs((int) depth_far - (int) depth_near) < 64);

    teardown();
    return 0;
}

static int test_texture_and_fill(void)
{
    /* 256x256 checkerboard with 128 texel quadrants.  The quad maps texture
     * s,t in [0,1] onto object [-0.8,0.8]^2, so the visible half x >= 0
     * contains all four texel quadrants.  Each expected colour is derived
     * from the texel quadrant that the sampled point maps to. */
    static unsigned char tex[256 * 256 * 3];
    GLuint t;
    int x, y;
    int checked = 0;
    GLdouble eq[4] = {1, 0, 0, 0};

    for (y = 0; y < 256; y++)
        for (x = 0; x < 256; x++) {
            int cell = ((x >> 7) ^ (y >> 7)) & 1;
            unsigned char *p = &tex[(y * 256 + x) * 3];
            p[0] = cell ? 255 : 0;
            p[1] = cell ? 0 : 255;
            p[2] = 0;
        }

    setup();
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE,
                 tex);
    glEnable(GL_TEXTURE_2D);
    glClipPlane(GL_CLIP_PLANE0, eq);
    glEnable(GL_CLIP_PLANE0);

    glColor3f(1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex3f(-0.8f, -0.8f, 0);
    glTexCoord2f(1, 0);
    glVertex3f(0.8f, -0.8f, 0);
    glTexCoord2f(1, 1);
    glVertex3f(0.8f, 0.8f, 0);
    glTexCoord2f(0, 1);
    glVertex3f(-0.8f, 0.8f, 0);
    glEnd();

    /* sample only points well inside a texel quadrant (away from the
     * quadrant boundaries and from the polygon edges) */
    for (y = 0; y < FB_H; y++)
        for (x = 0; x < FB_W; x++) {
            double obj_x = ((double) x - (FB_W - 0.5) / 2.0) /
                           ((FB_W - 0.5) / 2.0);
            double obj_y = ((double) y - (FB_H - 0.5) / 2.0) /
                           (-(FB_H - 0.5) / 2.0);
            double s = (obj_x + 0.8) / 1.6;
            double t = (obj_y + 0.8) / 1.6;
            int near_border_x, near_border_y;
            int quad_x, quad_y, cell;
            PIXEL p;
            int r, g, b;

            if (obj_x < 0.02 || obj_x > 0.78 || obj_y < -0.78 || obj_y > 0.78)
                continue;
            /* stay 2 pixels away from every texel-quadrant border */
            near_border_x = fabs(s - 0.5) * 1.6 * ((FB_W - 0.5) / 2.0) < 3.0;
            near_border_y = fabs(t - 0.5) * 1.6 * ((FB_H - 0.5) / 2.0) < 3.0;
            if (near_border_x || near_border_y)
                continue;
            /* stay away from the polygon's own outline */
            if (fabs(obj_x - 0.8) * ((FB_W - 0.5) / 2.0) < 3.0 ||
                fabs(obj_y - 0.8) * ((FB_H - 0.5) / 2.0) < 3.0 ||
                fabs(obj_y + 0.8) * ((FB_H - 0.5) / 2.0) < 3.0)
                continue;

            quad_x = s >= 0.5;
            quad_y = t >= 0.5;
            cell = quad_x ^ quad_y;
            p = pixel_at(x, y);
            TASK_CHECK(p != 0);
            get_rgb(p, &r, &g, &b);
            checked++;
            if (cell)
                TASK_CHECK(color_near(p, 255, 0, 0));
            else
                TASK_CHECK(color_near(p, 0, 255, 0));
        }

    TASK_CHECK(checked > 200);

    /* the clipped half is untouched */
    TASK_CHECK(!pixel_is_filled(pixel_x(-0.6), pixel_y(0.0)));
    TASK_CHECK(!pixel_is_filled(pixel_x(-0.5), pixel_y(0.5)));
    TASK_CHECK(pixel_is_filled(pixel_x(0.5), pixel_y(0.5)));

    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &t);
    teardown();
    return 0;
}

static int test_wireframe_cut(void)
{
    GLdouble eq[4] = {1, 0, 0, 0};
    int boundary_count, diagonal_count;

    setup();
    glClipPlane(GL_CLIP_PLANE0, eq);
    glEnable(GL_CLIP_PLANE0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glColor3f(1, 1, 1);
    draw_big_triangle();

    /* The clipped polygon has three edges: the bottom edge, the right/upper
     * edge and the new cut edge x = 0, which coincides with NDC column 0
     * (the framebuffer spans NDC [-1,1] over [0,width-1]). */
    boundary_count = 0;
    {
        int y;
        int x = pixel_x(0.0);
        for (y = 0; y < FB_H; y++)
            if (pixel_is_filled(x, y))
                boundary_count++;
    }
    TASK_CHECK(boundary_count >= 40);

    /* The frame is only the outline: the interior of the clipped polygon must
     * not contain the extra diagonal that a fan triangulation without hidden
     * edges would produce. */
    diagonal_count = 0;
    {
        int y;
        int x = pixel_x(0.05); /* just inside the cut, away from the other
                                  two edges in this row range */
        for (y = pixel_y(0.2); y <= pixel_y(-0.5); y++)
            if (pixel_is_filled(x, y))
                diagonal_count++;
    }
    TASK_CHECK(diagonal_count == 0);

    /* nothing at all to the left of the cut */
    {
        int x, y, left = 0;
        for (y = 0; y < FB_H; y++)
            for (x = 0; x < pixel_x(0.0); x++)
                if (pixel_is_filled(x, y))
                    left++;
        TASK_CHECK(left == 0);
    }

    teardown();
    return 0;
}

static int test_no_full_model_then_erase(void)
{
    /* If the geometry were drawn first and erased afterwards, the depth
     * buffer would still contain values for the removed region. */
    GLdouble eq[4] = {1, 0, 0, 0};

    setup();
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glClipPlane(GL_CLIP_PLANE0, eq);
    glEnable(GL_CLIP_PLANE0);
    glColor3f(1, 1, 1);
    draw_big_triangle();

    TASK_CHECK(count_depth_written() > 0);
    TASK_CHECK(zb->zbuf[pixel_y(0.0) * zb->xsize + pixel_x(-0.5)] == 0);
    TASK_CHECK(zb->zbuf[pixel_y(-0.5) * zb->xsize + pixel_x(-0.6)] == 0);

    teardown();
    return 0;
}

static int test_lit_and_unlit_paths(void)
{
    GLdouble eq[4] = {1, 0, 0, 0};
    int unlit, lit;

    setup();
    glClipPlane(GL_CLIP_PLANE0, eq);
    glEnable(GL_CLIP_PLANE0);

    glColor3f(1, 1, 1);
    draw_big_triangle();
    unlit = count_filled();
    TASK_CHECK(unlit > 0);

    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    {
        GLfloat pos[4] = {0, 0, 1, 0};
        GLfloat diff[4] = {1, 1, 1, 1};
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
        glLightfv(GL_LIGHT0, GL_DIFFUSE, diff);
    }
    glNormal3f(0, 0, 1);
    draw_big_triangle();
    lit = count_filled();
    glDisable(GL_LIGHTING);

    /* the lighting path produces geometry too, with the same clipped
     * silhouette (pixel count may differ slightly because of shading only) */
    TASK_CHECK(lit > 0);
    TASK_CHECK(lit == unlit);

    teardown();
    return 0;
}

static int test_perspective_projection(void)
{
    GLdouble eq[4] = {1, 0, 0, 0};

    setup();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1, 1, -1, 1, 1, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, 0, -3);

    glClipPlane(GL_CLIP_PLANE0, eq);
    glEnable(GL_CLIP_PLANE0);

    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex3f(-2.0f, -1.0f, 0);
    glVertex3f(2.0f, -1.0f, 0);
    glVertex3f(0.0f, 1.0f, 0);
    glEnd();

    TASK_CHECK(count_filled() > 0);
    /* object x = +/-2 at eye z = -3 projects to ndc x = +/-0.667; the cut at
     * eye x = 0 is the centre column, so a point just right of it is kept
     * and its mirror is clipped */
    TASK_CHECK(!pixel_is_filled(pixel_x(-0.2), pixel_y(-0.1)));
    TASK_CHECK(pixel_is_filled(pixel_x(0.2), pixel_y(-0.1)));

    teardown();
    return 0;
}

static int test_orthographic_projection(void)
{
    GLdouble eq[4] = {1, 0, 0, 0};

    setup();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-2, 2, -2, 2, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glClipPlane(GL_CLIP_PLANE0, eq);
    glEnable(GL_CLIP_PLANE0);

    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.5f, -1.0f, 0);
    glVertex3f(1.5f, -1.0f, 0);
    glVertex3f(0.0f, 1.0f, 0);
    glEnd();

    TASK_CHECK(count_filled() > 0);
    /* with ortho(-2,2) the NDC x is object_x/2; the cut at object x=0 is the
     * framebuffer centre */
    TASK_CHECK(!pixel_is_filled(pixel_x(-0.3), pixel_y(-0.5)));
    TASK_CHECK(pixel_is_filled(pixel_x(0.3), pixel_y(-0.5)));

    teardown();
    return 0;
}

static int test_all_planes_off_preserves_render(void)
{
    PIXEL before[FB_W * FB_H];
    GLushort depth_before[FB_W * FB_H];
    int i;

    setup();
    glEnable(GL_DEPTH_TEST);
    glColor3f(1, 0.5f, 0.25f);
    draw_big_triangle();

    memcpy(before, zb->pbuf, sizeof(before));
    memcpy(depth_before, zb->zbuf, sizeof(depth_before));

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    {
        GLdouble eq[4] = {1, 0, 0, 0};
        glClipPlane(GL_CLIP_PLANE0, eq);
        glClipPlane(GL_CLIP_PLANE1, eq);
    }
    glDisable(GL_CLIP_PLANE0);
    glDisable(GL_CLIP_PLANE1);
    glColor3f(1, 0.5f, 0.25f);
    draw_big_triangle();

    for (i = 0; i < FB_W * FB_H; i++) {
        TASK_CHECK(before[i] == zb->pbuf[i]);
        TASK_CHECK(depth_before[i] == zb->zbuf[i]);
    }

    teardown();
    return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"plane_state", "defaults", test_defaults},
    {"plane_state", "enable_state", test_enable_state},
    {"plane_state", "freeze_identity", test_freeze_identity},
    {"plane_state", "freeze_translation", test_freeze_translation},
    {"plane_state", "freeze_rotation", test_freeze_rotation},
    {"plane_state", "freeze_scale_proportional",
     test_freeze_scale_proportional},
    {"plane_state", "freeze_until_redefine", test_freeze_until_redefine},
    {"plane_state", "disabled_draws", test_clip_disabled_by_default_draws},

    {"geometry", "points", test_points},
    {"geometry", "independent_lines", test_independent_lines},
    {"geometry", "multiple_planes", test_multiple_planes},
    {"geometry", "list_compile", test_list_compile},
    {"geometry", "list_compile_and_execute", test_list_compile_and_execute},
    {"geometry", "matrix_change_after_definition",
     test_matrix_change_after_definition},

    {"render", "fill_analytic", test_fill_analytic},
    {"render", "tricolor_interpolation", test_tricolor_interpolation},
    {"render", "zbuffer", test_zbuffer},
    {"render", "texture_and_fill", test_texture_and_fill},
    {"render", "wireframe_cut", test_wireframe_cut},
    {"render", "no_full_model_then_erase", test_no_full_model_then_erase},
    {"render", "lit_and_unlit_paths", test_lit_and_unlit_paths},
    {"render", "perspective_projection", test_perspective_projection},
    {"render", "orthographic_projection", test_orthographic_projection},
    {"render", "all_planes_off_preserves_render",
     test_all_planes_off_preserves_render},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
