#include "msghandling.h"
#include "zgl.h"
#include "ztriangle_variants.h"

/* fill triangle profile */
/* #define PROFILE */

#define CLIP_XMIN (1 << 0)
#define CLIP_XMAX (1 << 1)
#define CLIP_YMIN (1 << 2)
#define CLIP_YMAX (1 << 3)
#define CLIP_ZMIN (1 << 4)
#define CLIP_ZMAX (1 << 5)

#define CLIP_USER_ALL_MASK (((1 << MAX_CLIP_PLANES) - 1) << CLIP_USER_SHIFT)

/* Evaluate the enabled user planes against the eye coordinates of a
 * vertex. Plane i contributes bit (CLIP_USER_SHIFT + i) when the vertex
 * is strictly outside, i.e. the plane value is negative; the boundary
 * itself (value 0) is kept. */
GLint gl_user_clip_code(GLContext *c, GLVertex *v)
{
    GLint mask = c->clip_plane_mask;
    GLint code = 0;
    GLint bit = CLIP_USER_SHIFT;
    for (GLint i = 0; mask; i++, mask >>= 1, bit++) {
        if (mask & 1) {
            const V4 *p = &c->clip_plane_equation[i];
            GLfloat d = p->X * v->ec.X + p->Y * v->ec.Y + p->Z * v->ec.Z +
                        p->W * v->ec.W;
            if (d < 0.0f)
                code |= 1 << bit;
        }
    }
    return code;
}

/* glClipPlane: freeze the object-space half space ax+by+cz+d >= 0 into
 * eye space using the inverse transpose of the MODELVIEW matrix active
 * when the command is executed. Display lists replay through gl_add_op,
 * so the matrix is recaptured every time the opcode runs. */
void glopClipPlane(GLParam *p)
{
    GLContext *c = gl_get_context();
    GLint plane = p[1].i - GL_CLIP_PLANE0;
    GLfloat eq[4];
    M4 inv, inv_t;

    if (plane < 0 || plane >= MAX_CLIP_PLANES)
        return;

    for (GLint i = 0; i < 4; i++)
        eq[i] = p[2 + i].f;

    gl_M4_Inv(&inv, c->matrix_stack_ptr[0]);
    gl_M4_Transpose(&inv_t, &inv);

    /* eye[j] = sum_k invT[j][k] * object[k]; invT row j, column k
     * lives in m[j][k] */
    for (GLint j = 0; j < 4; j++) {
        GLfloat s = 0.0f;
        for (GLint k = 0; k < 4; k++)
            s += inv_t.m[j][k] * eq[k];
        c->clip_plane_equation[plane].v[j] = s;
    }
}

void glClipPlane(GLenum plane, const GLdouble *equation)
{
    GLParam p[6];
#include "error_check_no_context.h"
    if (!equation)
        return;
    for (GLint i = 0; i < 4; i++)
        if (!isfinite(equation[i]))
            return;
    p[0].op = OP_ClipPlane;
    p[1].i = plane;
    p[2].f = (GLfloat) equation[0];
    p[3].f = (GLfloat) equation[1];
    p[4].f = (GLfloat) equation[2];
    p[5].f = (GLfloat) equation[3];
    gl_add_op(p);
}

/* Queries are always executed immediately and are never compiled into a
 * display list. The returned equation is the eye-space value frozen at
 * the last glClipPlane call. */
void glGetClipPlane(GLenum plane, GLdouble *equation)
{
    GLContext *c;
    GLint idx;
#include "error_check_no_context.h"
    if (!equation)
        return;
    c = gl_get_context();
    idx = plane - GL_CLIP_PLANE0;
    if (idx < 0 || idx >= MAX_CLIP_PLANES)
        return;
    for (GLint i = 0; i < 4; i++)
        equation[i] = (GLdouble) c->clip_plane_equation[idx].v[i];
}

static GLfloat user_plane_distance(GLContext *c, GLint plane,
                                   const GLVertex *v)
{
    const V4 *p = &c->clip_plane_equation[plane];
    return p->X * v->ec.X + p->Y * v->ec.Y + p->Z * v->ec.Z +
           p->W * v->ec.W;
}


static void gl_transform_to_viewport_clip_c(GLContext *c, GLVertex *v)
{ /* MARK: NOT_INLINED_IN_OG*/
    /* coordinates */
    {
        GLfloat winv = 1.0f / v->pc.W;
        v->zp.x = clamp_viewport_coord(v->pc.X * winv * c->viewport.scale.X +
                                       c->viewport.trans.X);
        v->zp.y = clamp_viewport_coord(v->pc.Y * winv * c->viewport.scale.Y +
                                       c->viewport.trans.Y);
        v->zp.z = clamp_viewport_coord(v->pc.Z * winv * c->viewport.scale.Z +
                                       c->viewport.trans.Z);
    }
    /* color */
    v->zp.r =
        (GLint) (v->color.v[0] * COLOR_CORRECTED_MULT_MASK + COLOR_MIN_MULT) &
        COLOR_MASK;
    v->zp.g =
        (GLint) (v->color.v[1] * COLOR_CORRECTED_MULT_MASK + COLOR_MIN_MULT) &
        COLOR_MASK;
    v->zp.b =
        (GLint) (v->color.v[2] * COLOR_CORRECTED_MULT_MASK + COLOR_MIN_MULT) &
        COLOR_MASK;

    /* texture */

    if (c->texture_2d_enabled) {
        v->zp.s = (GLint) (v->tex_coord.X * (ZB_POINT_S_MAX - ZB_POINT_S_MIN) +
                           ZB_POINT_S_MIN);
        v->zp.t = (GLint) (v->tex_coord.Y * (ZB_POINT_T_MAX - ZB_POINT_T_MIN) +
                           ZB_POINT_T_MIN);
    }
}

#define clip_funcdef(name, sign, dir, dir1, dir2) \
    static GLfloat name(V4 *c, V4 *a, V4 *b)      \
    {                                             \
        GLfloat t, dX, dY, dZ, dW, den;           \
        dX = (b->X - a->X);                       \
        dY = (b->Y - a->Y);                       \
        dZ = (b->Z - a->Z);                       \
        dW = (b->W - a->W);                       \
        den = -(sign d##dir) + dW;                \
        if (den == 0)                             \
            t = 0;                                \
        else                                      \
            t = (sign a->dir - a->W) / den;       \
        c->dir1 = a->dir1 + t * d##dir1;          \
        c->dir2 = a->dir2 + t * d##dir2;          \
        c->W = a->W + t * dW;                     \
        c->dir = sign c->W;                       \
        return t;                                 \
    }

/* clang-format off */
clip_funcdef(clip_xmin, -, X, Y, Z)
clip_funcdef(clip_xmax, +, X, Y, Z)
clip_funcdef(clip_ymin, -, Y, X, Z)
clip_funcdef(clip_ymax, +, Y, X, Z)
clip_funcdef(clip_zmin, -, Z, X, Y)
clip_funcdef(clip_zmax, +, Z, X, Y)

static GLfloat (*clip_proc[6])(V4 *, V4 *, V4 *) = {
    clip_xmin, clip_xmax, clip_ymin, clip_ymax, clip_zmin, clip_zmax,
};
/* clang-format on */

/* point */
#if TGL_HAS(ALT_RENDERMODES)
static void gl_add_select1(GLint z1, GLint z2, GLint z3)
{
    GLint min, max;
    min = max = z1;
    if (z2 < min)
        min = z2;
    if (z3 < min)
        min = z3;
    if (z2 > max)
        max = z2;
    if (z3 > max)
        max = z3;

    gl_add_select(0xffffffff - min, 0xffffffff - max);
}
#else
#define gl_add_select1(a, b, c) /*a comment*/
#endif

/* full vertex interpolation along q = p0 + t * (p1 - p0). All attributes
 * used by the rasterizer are interpolated so user plane intersection
 * vertices carry correct color, depth and texture coordinates. */
static void gl_interpolate_vertex(GLVertex *q,
                                  const GLVertex *p0,
                                  const GLVertex *p1,
                                  GLfloat t)
{
    GLint i;
    q->pc.X = p0->pc.X + (p1->pc.X - p0->pc.X) * t;
    q->pc.Y = p0->pc.Y + (p1->pc.Y - p0->pc.Y) * t;
    q->pc.Z = p0->pc.Z + (p1->pc.Z - p0->pc.Z) * t;
    q->pc.W = p0->pc.W + (p1->pc.W - p0->pc.W) * t;
    for (i = 0; i < 4; i++) {
        q->color.v[i] = p0->color.v[i] + (p1->color.v[i] - p0->color.v[i]) * t;
        q->tex_coord.v[i] =
            p0->tex_coord.v[i] + (p1->tex_coord.v[i] - p0->tex_coord.v[i]) * t;
        q->ec.v[i] = p0->ec.v[i] + (p1->ec.v[i] - p0->ec.v[i]) * t;
    }
    q->normal.X = p0->normal.X + (p1->normal.X - p0->normal.X) * t;
    q->normal.Y = p0->normal.Y + (p1->normal.Y - p0->normal.Y) * t;
    q->normal.Z = p0->normal.Z + (p1->normal.Z - p0->normal.Z) * t;
}

void gl_draw_point(GLVertex *p0)
{
    GLContext *c = gl_get_context();
    if (p0->clip_code == 0) {
#if TGL_HAS(ALT_RENDERMODES)
        if (c->render_mode == GL_SELECT) {
            gl_add_select(p0->zp.z, p0->zp.z);

        } else if (c->render_mode == GL_FEEDBACK) {
            gl_add_feedback(GL_POINT_TOKEN, p0, NULL, NULL, 0);
        } else
#endif
        {
            ZB_plot(c->zb, &p0->zp);
        }
    }
}

/* line */

/*
 * Line Clipping
 */

/* Line Clipping algorithm from 'Computer Graphics', Principles and
   Practice */
static GLint ClipLine1(GLfloat denom, GLfloat num, GLfloat *tmin, GLfloat *tmax)
{
    GLfloat t;

    if (denom > 0) {
        t = num / denom;
        if (t > *tmax)
            return 0;
        if (t > *tmin)
            *tmin = t;
    } else if (denom < 0) {
        t = num / denom;
        if (t < *tmin)
            return 0;
        if (t < *tmax)
            *tmax = t;
    } else if (num > 0)
        return 0;
    return 1;
}

static void gl_draw_line_render(GLContext *c, GLVertex *p1, GLVertex *p2)
{
#if TGL_HAS(ALT_RENDERMODES)
    if (c->render_mode == GL_SELECT) {
        gl_add_select1(p1->zp.z, p2->zp.z, p2->zp.z);
    } else if (c->render_mode == GL_FEEDBACK) {
        gl_add_feedback(GL_LINE_TOKEN, p1, p2, NULL, 0);
    } else
#endif
    {
        if (c->zb->depth_test)
            ZB_line_z(c->zb, &p1->zp, &p2->zp);
        else
            ZB_line(c->zb, &p1->zp, &p2->zp);
    }
}

void gl_draw_line(GLVertex *p1, GLVertex *p2)
{
    GLContext *c = gl_get_context();
    GLVertex q1, q2;
    GLint cc1, cc2;

    cc1 = p1->clip_code;
    cc2 = p2->clip_code;

    /* trivial accept / reject when no user plane is involved */
    if ((cc1 | cc2) == 0) {
        gl_draw_line_render(c, p1, p2);
        return;
    }
    if ((cc1 & cc2) != 0)
        return;

    if (c->clip_plane_mask == 0 ||
        ((cc1 | cc2) & CLIP_USER_ALL_MASK) == 0) {
        /* only frustum clipping is needed: keep the optimized path */
        GLfloat dx, dy, dz, dw, x1, y1, z1, w1;
        GLfloat tmin = 0;
        GLfloat tmax = 1;
        dx = p2->pc.X - p1->pc.X;
        dy = p2->pc.Y - p1->pc.Y;
        dz = p2->pc.Z - p1->pc.Z;
        dw = p2->pc.W - p1->pc.W;
        x1 = p1->pc.X;
        y1 = p1->pc.Y;
        z1 = p1->pc.Z;
        w1 = p1->pc.W;

        if (ClipLine1(dx + dw, -x1 - w1, &tmin, &tmax) &&
            ClipLine1(-dx + dw, x1 - w1, &tmin, &tmax) &&
            ClipLine1(dy + dw, -y1 - w1, &tmin, &tmax) &&
            ClipLine1(-dy + dw, y1 - w1, &tmin, &tmax) &&
            ClipLine1(dz + dw, -z1 - w1, &tmin, &tmax) &&
            ClipLine1(-dz + dw, z1 - w1, &tmin, &tmax)) {
            gl_interpolate_vertex(&q1, p1, p2, tmin);
            gl_interpolate_vertex(&q2, p1, p2, tmax);
            gl_transform_to_viewport_clip_c(c, &q1);
            gl_transform_to_viewport_clip_c(c, &q2);
            gl_draw_line_render(c, &q1, &q2);
        }
        return;
    }

    /* general case: clip the segment against every frustum plane and
     * every enabled user plane with the Liang-Barsky interval test */
    {
        GLfloat tmin = 0.0f;
        GLfloat tmax = 1.0f;
        GLfloat dx = p2->pc.X - p1->pc.X;
        GLfloat dy = p2->pc.Y - p1->pc.Y;
        GLfloat dz = p2->pc.Z - p1->pc.Z;
        GLfloat dw = p2->pc.W - p1->pc.W;
        GLfloat x1 = p1->pc.X;
        GLfloat y1 = p1->pc.Y;
        GLfloat z1 = p1->pc.Z;
        GLfloat w1 = p1->pc.W;

        if (!ClipLine1(dx + dw, -x1 - w1, &tmin, &tmax) ||
            !ClipLine1(-dx + dw, x1 - w1, &tmin, &tmax) ||
            !ClipLine1(dy + dw, -y1 - w1, &tmin, &tmax) ||
            !ClipLine1(-dy + dw, y1 - w1, &tmin, &tmax) ||
            !ClipLine1(dz + dw, -z1 - w1, &tmin, &tmax) ||
            !ClipLine1(-dz + dw, z1 - w1, &tmin, &tmax))
            return;

        for (GLint i = 0; i < MAX_CLIP_PLANES; i++) {
            if (!(c->clip_plane_mask & (1 << i)))
                continue;
            GLfloat d0 = user_plane_distance(c, i, p1);
            GLfloat d1 = user_plane_distance(c, i, p2);
            /* half space d >= 0: inside, boundary d == 0 kept. ClipLine1
             * takes the negated inside value as its numerator */
            if (!ClipLine1(d1 - d0, -d0, &tmin, &tmax))
                return;
        }

        gl_interpolate_vertex(&q1, p1, p2, tmin);
        gl_interpolate_vertex(&q2, p1, p2, tmax);
        gl_transform_to_viewport_clip_c(c, &q1);
        gl_transform_to_viewport_clip_c(c, &q2);
        gl_draw_line_render(c, &q1, &q2);
    }
}

/*Triangles*/

static void updateTmp(GLContext *c,
                      GLVertex *q,
                      GLVertex *p0,
                      GLVertex *p1,
                      GLfloat t,
                      GLint clip_bit)
{
    GLint i;

    {
        q->color.v[0] = p0->color.v[0] + (p1->color.v[0] - p0->color.v[0]) * t;
        q->color.v[1] = p0->color.v[1] + (p1->color.v[1] - p0->color.v[1]) * t;
        q->color.v[2] = p0->color.v[2] + (p1->color.v[2] - p0->color.v[2]) * t;
    }

#if TGL_OPTIMIZATION_HINT_BRANCH_COST < 1
    if (c->texture_2d_enabled)
#endif
    {
        q->tex_coord.X =
            p0->tex_coord.X + (p1->tex_coord.X - p0->tex_coord.X) * t;
        q->tex_coord.Y =
            p0->tex_coord.Y + (p1->tex_coord.Y - p0->tex_coord.Y) * t;
    }

    /* eye coordinates of the intersection: needed so the vertex can be
     * tested against the remaining user clipping planes. Clip-space
     * clipping interpolates pc linearly in t; unprojection is linear too,
     * so the matching eye point is the same linear blend. Its homogeneous
     * W need not be 1, but every surviving point has W > 0 so the sign of
     * the plane distance still gives the correct inside/outside test. */
    for (i = 0; i < 4; i++)
        q->ec.v[i] = p0->ec.v[i] + (p1->ec.v[i] - p0->ec.v[i]) * t;

    q->clip_code = gl_clipcode(q->pc.X, q->pc.Y, q->pc.Z, q->pc.W);
    if (c->clip_plane_mask)
        q->clip_code |= gl_user_clip_code(c, q);
    /* the new vertex lies on the plane just processed; clear its bit so
     * floating point rounding cannot make a later recursion reject it */
    q->clip_code &= ~(1 << clip_bit);
    if (q->clip_code == 0)
        gl_transform_to_viewport_clip_c(c, q);
}

/* Parameter t (and clipped pc) where the edge a->b crosses plane
 * clip_bit. Frustum planes use the dedicated proc; user planes are
 * solved from the frozen eye-space plane distances. */
static GLfloat triangle_clip_param(GLContext *c,
                                   GLint clip_bit,
                                   GLVertex *out,
                                   GLVertex *a,
                                   GLVertex *b)
{
    GLfloat t;
    if (clip_bit < 6)
        return clip_proc[clip_bit](&out->pc, &a->pc, &b->pc);

    {
        GLint plane = clip_bit - CLIP_USER_SHIFT;
        GLfloat d0 = user_plane_distance(c, plane, a);
        GLfloat d1 = user_plane_distance(c, plane, b);
        GLfloat den = d0 - d1;
        if (den == 0.0f)
            t = 0.0f;
        else
            t = d0 / den; /* d0 + t (d1 - d0) = 0 */
    }
    out->pc.X = a->pc.X + (b->pc.X - a->pc.X) * t;
    out->pc.Y = a->pc.Y + (b->pc.Y - a->pc.Y) * t;
    out->pc.Z = a->pc.Z + (b->pc.Z - a->pc.Z) * t;
    out->pc.W = a->pc.W + (b->pc.W - a->pc.W) * t;
    return t;
}

static void gl_draw_triangle_clip(GLContext *c,
                                  GLVertex *p0,
                                  GLVertex *p1,
                                  GLVertex *p2,
                                  GLint clip_bit);

/* Internal helper that takes context - avoids redundant gl_get_context() in hot
 * paths */
static void gl_draw_triangle_c(GLContext *c,
                               GLVertex *p0,
                               GLVertex *p1,
                               GLVertex *p2)
{
    GLint co, cc[3], front;

    cc[0] = p0->clip_code;
    cc[1] = p1->clip_code;
    cc[2] = p2->clip_code;

    co = cc[0] | cc[1] | cc[2];

    /* we handle the non clipped case here to go faster */
    if (co == 0) {
        GLfloat norm;
        norm =
            (GLfloat) (p1->zp.x - p0->zp.x) * (GLfloat) (p2->zp.y - p0->zp.y) -
            (GLfloat) (p2->zp.x - p0->zp.x) * (GLfloat) (p1->zp.y - p0->zp.y);

        if (norm == 0)
            return;

        front = norm < 0.0;
        front = front ^ c->current_front_face;

        /* back face culling */
        if (c->cull_face_enabled) {
            /* most used case first */
            if (c->current_cull_face == GL_BACK) {
                if (front == 0)
                    return;
                c->draw_triangle_front(p0, p1, p2);
            } else if (c->current_cull_face == GL_FRONT) {
                if (front != 0)
                    return;
                c->draw_triangle_back(p0, p1, p2);
            } else {
                return;
            }
        } else {
            /* no culling */
            if (front) {
                c->draw_triangle_front(p0, p1, p2);
            } else {
                c->draw_triangle_back(p0, p1, p2);
            }
        }
    } else {
        /* GLint c_and = cc[0] & cc[1] & cc[2];*/
        if ((cc[0] & cc[1] & cc[2]) ==
            0) { /* Don't draw a triangle with no points*/
            gl_draw_triangle_clip(c, p0, p1, p2, 0);
        }
    }
}
void gl_draw_triangle(GLVertex *p0, GLVertex *p1, GLVertex *p2)
{
    gl_draw_triangle_c(gl_get_context(), p0, p1, p2);
}

static void gl_draw_triangle_clip(GLContext *c,
                                  GLVertex *p0,
                                  GLVertex *p1,
                                  GLVertex *p2,
                                  GLint clip_bit)
{
    GLint co, c_and, co1, cc[3], edge_flag_tmp, clip_mask;

    GLVertex *q[3];

    cc[0] = p0->clip_code;
    cc[1] = p1->clip_code;
    cc[2] = p2->clip_code;

    co = cc[0] | cc[1] | cc[2];
    if (co == 0) {
        gl_draw_triangle_c(c, p0, p1, p2);
    } else {
        c_and = cc[0] & cc[1] & cc[2];
        /* the triangle is completely outside */
        if (c_and != 0)
            return;

        /* find the next plane to clip against: the six frustum planes
         * first, then the enabled user clipping planes */
        while (clip_bit < 6 + MAX_CLIP_PLANES) {
            if (clip_bit >= 6 &&
                !(c->clip_plane_mask & (1 << (clip_bit - CLIP_USER_SHIFT)))) {
                clip_bit++;
                continue;
            }
            if (co & (1 << clip_bit))
                break;
            clip_bit++;
        }

        /* this test can be true only in case of rounding errors */
        if (clip_bit == 6 + MAX_CLIP_PLANES) {
            return;
        }

        clip_mask = 1 << clip_bit;
        co1 = (cc[0] ^ cc[1] ^ cc[2]) & clip_mask;

        if (co1) {
            /* one point outside */

            if (cc[0] & clip_mask) {
                q[0] = p0;
                q[1] = p1;
                q[2] = p2;
            } else if (cc[1] & clip_mask) {
                q[0] = p1;
                q[1] = p2;
                q[2] = p0;
            } else {
                q[0] = p2;
                q[1] = p0;
                q[2] = p1;
            }
            {
                GLVertex tmp1, tmp2;
                GLfloat tt;
                tt = triangle_clip_param(c, clip_bit, &tmp1, q[0], q[1]);
                updateTmp(c, &tmp1, q[0], q[1], tt, clip_bit);

                tt = triangle_clip_param(c, clip_bit, &tmp2, q[0], q[2]);
                updateTmp(c, &tmp2, q[0], q[2], tt, clip_bit);

                tmp1.edge_flag = q[0]->edge_flag;
                edge_flag_tmp = q[2]->edge_flag;
                q[2]->edge_flag = 0;
                gl_draw_triangle_clip(c, &tmp1, q[1], q[2], clip_bit + 1);

                tmp2.edge_flag = 1;
                tmp1.edge_flag = 0;
                q[2]->edge_flag = edge_flag_tmp;
                gl_draw_triangle_clip(c, &tmp2, &tmp1, q[2], clip_bit + 1);
            }
        } else {
            /* two points outside */

            if ((cc[0] & clip_mask) == 0) {
                q[0] = p0;
                q[1] = p1;
                q[2] = p2;
            } else if ((cc[1] & clip_mask) == 0) {
                q[0] = p1;
                q[1] = p2;
                q[2] = p0;
            } else {
                q[0] = p2;
                q[1] = p0;
                q[2] = p1;
            }
            {
                GLVertex tmp1, tmp2;
                GLfloat tt;
                tt = triangle_clip_param(c, clip_bit, &tmp1, q[0], q[1]);
                updateTmp(c, &tmp1, q[0], q[1], tt, clip_bit);

                tt = triangle_clip_param(c, clip_bit, &tmp2, q[0], q[2]);
                updateTmp(c, &tmp2, q[0], q[2], tt, clip_bit);

                tmp1.edge_flag = 1;
                tmp2.edge_flag = q[2]->edge_flag;
                gl_draw_triangle_clip(c, q[0], &tmp1, &tmp2, clip_bit + 1);
            }
        }
    }
}

/* see vertex.c to see how the draw functions are assigned.*/
void gl_draw_triangle_select(GLVertex *p0, GLVertex *p1, GLVertex *p2)
{
    gl_add_select1(p0->zp.z, p1->zp.z, p2->zp.z);
}
void gl_draw_triangle_feedback(GLVertex *p0, GLVertex *p1, GLVertex *p2)
{
    gl_add_feedback(GL_POLYGON_TOKEN, p0, p1, p2, 0);
}

#ifdef PROFILE
int count_triangles, count_triangles_textured, count_pixels;
#warning "Compile with PROFILE slows down everything"
#endif

/* see vertex.c to see how the draw functions are assigned.*/
void gl_draw_triangle_fill(GLVertex *p0, GLVertex *p1, GLVertex *p2)
{
    GLContext *c = gl_get_context();
    ZBuffer *zb = c->zb;
    GLint dt = zb->depth_test;
    GLint dw = zb->depth_write;
    ZB_fillTriangleFunc func;

    if (c->texture_2d_enabled) {
#if TGL_HAS(LIT_TEXTURES)
        if (c->current_shade_model != GL_SMOOTH) {
            p1->zp.r = p2->zp.r;
            p1->zp.g = p2->zp.g;
            p1->zp.b = p2->zp.b;

            p0->zp.r = p2->zp.r;
            p0->zp.g = p2->zp.g;
            p0->zp.b = p2->zp.b;
        }
#endif
        ZB_setTexture(zb, c->current_texture->images[0].pixmap);
#if TGL_HAS(BLEND)
        if (zb->enable_blend)
            func = ZB_getTriangleFunc(zb_triangle_dispatch.textured, dt, dw);
        else
            func = ZB_getTriangleFunc(zb_triangle_dispatch.textured_noblend, dt,
                                      dw);
#else
        func =
            ZB_getTriangleFunc(zb_triangle_dispatch.textured_noblend, dt, dw);
#endif
    } else if (c->current_shade_model == GL_SMOOTH) {
#if TGL_HAS(BLEND)
        if (zb->enable_blend)
            func = ZB_getTriangleFunc(zb_triangle_dispatch.smooth, dt, dw);
        else
            func =
                ZB_getTriangleFunc(zb_triangle_dispatch.smooth_noblend, dt, dw);
#else
        func = ZB_getTriangleFunc(zb_triangle_dispatch.smooth_noblend, dt, dw);
#endif
    } else {
#if TGL_HAS(BLEND)
        if (zb->enable_blend)
            func = ZB_getTriangleFunc(zb_triangle_dispatch.flat, dt, dw);
        else
            func =
                ZB_getTriangleFunc(zb_triangle_dispatch.flat_noblend, dt, dw);
#else
        func = ZB_getTriangleFunc(zb_triangle_dispatch.flat_noblend, dt, dw);
#endif
    }

    func(zb, &p0->zp, &p1->zp, &p2->zp);
}

/* Render a clipped triangle in line mode */

void gl_draw_triangle_line(GLVertex *p0, GLVertex *p1, GLVertex *p2)
{
    GLContext *c = gl_get_context();
    if (c->zb->depth_test) {
        if (p0->edge_flag)
            ZB_line_z(c->zb, &p0->zp, &p1->zp);
        if (p1->edge_flag)
            ZB_line_z(c->zb, &p1->zp, &p2->zp);
        if (p2->edge_flag)
            ZB_line_z(c->zb, &p2->zp, &p0->zp);
    } else {
        if (p0->edge_flag)
            ZB_line(c->zb, &p0->zp, &p1->zp);
        if (p1->edge_flag)
            ZB_line(c->zb, &p1->zp, &p2->zp);
        if (p2->edge_flag)
            ZB_line(c->zb, &p2->zp, &p0->zp);
    }
}

/* Render a clipped triangle in point mode */
void gl_draw_triangle_point(GLVertex *p0, GLVertex *p1, GLVertex *p2)
{
    GLContext *c = gl_get_context();
    if (p0->edge_flag)
        ZB_plot(c->zb, &p0->zp);
    if (p1->edge_flag)
        ZB_plot(c->zb, &p1->zp);
    if (p2->edge_flag)
        ZB_plot(c->zb, &p2->zp);
}
