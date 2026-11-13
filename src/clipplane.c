/*
 * User-defined clipping planes (GL_CLIP_PLANE0 .. GL_CLIP_PLANE5).
 *
 * Planes are specified in object coordinates as the half space
 *
 *     a*x + b*y + c*z + d >= 0
 *
 * At the moment glClipPlane() is executed, the coefficients are transformed
 * into eye coordinates using the inverse transpose of the current MODELVIEW
 * matrix and stored.  Later MODELVIEW changes therefore move the geometry but
 * not the already-defined plane; only a new glClipPlane() call re-captures the
 * matrix.
 *
 * Clipping itself is performed on the eye coordinates of the vertices inside
 * the normal geometry/projection/raster pipeline (see clip.c).
 */

#include <math.h>
#include <string.h>

#include "msghandling.h"
#include "zgl.h"

/* Invert a 4x4 matrix in double precision.  Returns 0 on success, 1 if the
 * matrix is singular.  The matrix uses the same row-major layout as M4:
 * result[i][j] is the coefficient of input row j in output row i. */
static GLint clip_invert4(const GLfloat m[4][4], GLdouble inv[4][4])
{
    GLdouble a[4][8];
    GLint i, j, k;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            a[i][j] = (GLdouble) m[i][j];
            a[i][j + 4] = (i == j) ? 1.0 : 0.0;
        }
    }

    for (j = 0; j < 4; j++) {
        GLint pivot = j;
        GLdouble best = fabs(a[j][j]);

        for (i = j + 1; i < 4; i++) {
            GLdouble v = fabs(a[i][j]);
            if (v > best) {
                best = v;
                pivot = i;
            }
        }
        if (best == 0.0)
            return 1;

        if (pivot != j) {
            for (k = 0; k < 8; k++) {
                GLdouble t = a[j][k];
                a[j][k] = a[pivot][k];
                a[pivot][k] = t;
            }
        }

        {
            GLdouble d = a[j][j];
            for (k = 0; k < 8; k++)
                a[j][k] /= d;
        }

        for (i = 0; i < 4; i++) {
            GLdouble f;
            if (i == j)
                continue;
            f = a[i][j];
            if (f == 0.0)
                continue;
            for (k = 0; k < 8; k++)
                a[i][k] -= f * a[j][k];
        }
    }

    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            inv[i][j] = a[i][j + 4];

    return 0;
}

/* Recompute the frozen eye-space coefficients of "plane" from the object-space
 * equation using the MODELVIEW matrix that is current right now. */
static void clip_plane_set(GLContext *c, GLint plane, const GLdouble eq[4])
{
    M4 *mv = c->matrix_stack_ptr[0];
    GLdouble inv[4][4];
    GLdouble eye[4];
    GLint i;

    if (clip_invert4((const GLfloat (*)[4]) mv->m, inv) != 0)
        return; /* singular MODELVIEW: out of the supported range */

    /* p_eye = inverse(MODELVIEW)^T * p_obj, i.e.
     * p_eye[i] = sum_j inv[j][i] * p_obj[j]. */
    for (i = 0; i < 4; i++)
        eye[i] = eq[0] * inv[0][i] + eq[1] * inv[1][i] +
                 eq[2] * inv[2][i] + eq[3] * inv[3][i];

    for (i = 0; i < 4; i++)
        c->clip_plane_eye[plane][i] = eye[i];
}

/* Evaluate the frozen eye-space plane at an eye-space vertex. */
GLdouble gl_clip_plane_value(GLContext *c, GLint plane, const V4 *ec)
{
    const GLdouble *p = c->clip_plane_eye[plane];
    return p[0] * (GLdouble) ec->X + p[1] * (GLdouble) ec->Y +
           p[2] * (GLdouble) ec->Z + p[3] * (GLdouble) ec->W;
}

/* Clip-code bits for every enabled user plane.  Boundary points (value == 0)
 * are kept. */
GLint gl_user_clip_active(GLContext *c)
{
    GLint i;
    for (i = 0; i < MAX_CLIP_PLANES; i++)
        if (c->clip_plane_enabled[i])
            return 1;
    return 0;
}

GLint gl_user_clip_code(GLContext *c, const V4 *ec)
{
    GLint code = 0;
    GLint i;

    for (i = 0; i < MAX_CLIP_PLANES; i++) {
        if (c->clip_plane_enabled[i] &&
            gl_clip_plane_value(c, i, ec) < 0.0)
            code |= 1 << (6 + i);
    }
    return code;
}

/* Parameter t of the intersection of segment [a,b] (eye coordinates) with the
 * frozen plane.  The caller guarantees the two endpoints are on opposite
 * sides. */
GLfloat gl_user_clip_plane_t(GLContext *c, GLint plane, const V4 *a,
                             const V4 *b)
{
    GLdouble fa = gl_clip_plane_value(c, plane, a);
    GLdouble fb = gl_clip_plane_value(c, plane, b);
    GLdouble den = fb - fa;

    if (den == 0.0)
        return 0.0f;
    return (GLfloat) (-fa / den);
}

/* Pack/unpack doubles in GLParam slots (used so the display list keeps the
 * exact object-space equation supplied by the application). */
static void clip_pack_double(GLParam *p, GLdouble d)
{
    GLuint lo, hi;
    memcpy(&lo, ((const unsigned char *) &d) + 0, sizeof(GLuint));
    memcpy(&hi, ((const unsigned char *) &d) + 4, sizeof(GLuint));
    p[0].ui = lo;
    p[1].ui = hi;
}

static GLdouble clip_unpack_double(const GLParam *p)
{
    GLdouble d;
    memcpy(((unsigned char *) &d) + 0, &p[0].ui, sizeof(GLuint));
    memcpy(((unsigned char *) &d) + 4, &p[1].ui, sizeof(GLuint));
    return d;
}

void glClipPlane(GLenum plane, const GLdouble *equation)
{
    GLParam p[10];
    GLint i;

    if (plane < GL_CLIP_PLANE0 || plane >= GL_CLIP_PLANE0 + MAX_CLIP_PLANES)
        return;
    if (!equation)
        return;

    for (i = 0; i < 4; i++) {
        if (!isfinite(equation[i]))
            return;
    }

    p[0].op = OP_ClipPlane;
    p[1].i = (GLint) (plane - GL_CLIP_PLANE0);
    for (i = 0; i < 4; i++)
        clip_pack_double(&p[2 + i * 2], equation[i]);

    gl_add_op(p);
}

void glopClipPlane(GLParam *p)
{
    GLContext *c = gl_get_context();
    GLint plane = p[1].i;
    GLdouble eq[4];
    GLint i;

    if (plane < 0 || plane >= MAX_CLIP_PLANES)
        return;

    for (i = 0; i < 4; i++)
        eq[i] = clip_unpack_double(&p[2 + i * 2]);

    clip_plane_set(c, plane, eq);
}

void glGetClipPlane(GLenum plane, GLdouble *equation)
{
    GLContext *c = gl_get_context();
    GLint i;

    if (!equation)
        return;
    if (plane < GL_CLIP_PLANE0 || plane >= GL_CLIP_PLANE0 + MAX_CLIP_PLANES)
        return;

    for (i = 0; i < 4; i++)
        equation[i] = c->clip_plane_eye[plane - GL_CLIP_PLANE0][i];
}
