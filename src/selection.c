/*
 * MyTinyGL - OpenGL 1.x Software Renderer
 * Copyright (c) 2025 zbufferoverflow (Eliezer Solinger)
 * https://github.com/zbufferoverflow/MyTinyGL
 * SPDX-License-Identifier: MIT
 *
 * selection.c - GL_SELECT selection mode: name stack and hit records
 *
 * Hit determination is purely geometric: vertices are transformed by the
 * current modelview/projection matrices (done by the regular pipeline) and
 * clipped against the six frustum planes in homogeneous clip space. A point
 * inside the frustum, a line with a non-zero clipped segment, or a triangle
 * with a positive-area clipped remnant counts as a hit - no color-key
 * rendering and no per-pixel depth readback. Selection never touches the
 * color, depth or stencil buffers.
 */

#include "selection.h"
#include "mytinygl.h"
#include "clipping.h"
#include "lists.h"
#include <math.h>

/* ------------------------------------------------------------------ */
/* Depth mapping                                                       */
/* ------------------------------------------------------------------ */

/* Map a clipped vertex's z/w through glDepthRange */
static double select_map_depth(const GLState *ctx, float z, float w)
{
    double zndc = (double)z / (double)w;
    return (zndc + 1.0) * 0.5 * (ctx->depth_far - ctx->depth_near) + ctx->depth_near;
}

/* Fold one clipped vertex into the pending hit record */
static void select_accumulate(GLState *ctx, float z, float w)
{
    double d = select_map_depth(ctx, z, w);
    if (!ctx->select_hit_pending) {
        ctx->select_hit_pending = 1;
        ctx->select_zmin = d;
        ctx->select_zmax = d;
    } else {
        if (d < ctx->select_zmin) ctx->select_zmin = d;
        if (d > ctx->select_zmax) ctx->select_zmax = d;
    }
}

/* Clamp to [0,1] and convert with floor(z * 2^32-1 + 0.5) */
static GLuint select_depth_word(double d)
{
    if (d < 0.0) d = 0.0;
    else if (d > 1.0) d = 1.0;
    return (GLuint)floor(d * 4294967295.0 + 0.5);
}

/* ------------------------------------------------------------------ */
/* Hit record settling                                                 */
/* ------------------------------------------------------------------ */

/* Write the pending hit record (if any) as
 * [name count, min depth, max depth, names bottom-to-top].
 * Records are written atomically: if the whole record does not fit, the
 * buffer is left untouched and locked until the next selection pass. */
static void select_settle_record(GLState *ctx)
{
    GLsizei need;
    GLuint *dst;
    GLint i;

    if (!ctx->select_hit_pending) return;
    ctx->select_hit_pending = 0;

    if (ctx->select_overflow) return;  /* buffer locked: no more writes */

    need = 3 + (GLsizei)ctx->name_stack_depth;
    if (!ctx->select_buffer || need > ctx->select_buffer_size - ctx->select_buffer_offset) {
        ctx->select_overflow = 1;
        return;
    }

    dst = ctx->select_buffer + ctx->select_buffer_offset;
    dst[0] = (GLuint)ctx->name_stack_depth;
    dst[1] = select_depth_word(ctx->select_zmin);
    dst[2] = select_depth_word(ctx->select_zmax);
    for (i = 0; i < ctx->name_stack_depth; i++) {
        dst[3 + i] = ctx->name_stack[i];
    }
    ctx->select_buffer_offset += need;
    ctx->select_hits++;
}

/* ------------------------------------------------------------------ */
/* Primitive hit testing (vertices are already in clip space)          */
/* ------------------------------------------------------------------ */

/* Point: hit when inside the view frustum */
static int select_point_inside(const vec4_t *p)
{
    return p->w > 0.0f &&
           p->x >= -p->w && p->x <= p->w &&
           p->y >= -p->w && p->y <= p->w &&
           p->z >= -p->w && p->z <= p->w;
}

/* Line: hit when clipping leaves a non-zero segment */
static void select_line(GLState *ctx, const vertex_t *v0, const vertex_t *v1)
{
    vertex_t a = *v0;
    vertex_t b = *v1;
    double ax, ay, az, bx, by, bz;

    if (!clip_line(&a, &b)) return;
    if (a.position.w <= 0.0f || b.position.w <= 0.0f) return;

    ax = (double)a.position.x / (double)a.position.w;
    ay = (double)a.position.y / (double)a.position.w;
    az = (double)a.position.z / (double)a.position.w;
    bx = (double)b.position.x / (double)b.position.w;
    by = (double)b.position.y / (double)b.position.w;
    bz = (double)b.position.z / (double)b.position.w;

    /* Degenerate remnant: zero-length segment does not count */
    if (ax == bx && ay == by && az == bz) return;

    select_accumulate(ctx, a.position.z, a.position.w);
    select_accumulate(ctx, b.position.z, b.position.w);
}

/* Triangle: hit when clipping leaves a positive-area remnant.
 * Honors glFrontFace/glCullFace; a triangle clipped into a fan still
 * counts as a single hit. */
static void select_triangle(GLState *ctx, const vertex_t *v0, const vertex_t *v1, const vertex_t *v2)
{
    vertex_t triangle[3];
    vertex_t clipped[MAX_CLIP_VERTS];
    double nx[MAX_CLIP_VERTS];
    double ny[MAX_CLIP_VERTS];
    double area2 = 0.0;
    int n, i, j, is_front;

    triangle[0] = *v0;
    triangle[1] = *v1;
    triangle[2] = *v2;

    n = clip_triangle(triangle, clipped);
    if (n < 3) return;

    /* Perspective divide of the complete clipped geometry (w > 0) */
    for (i = 0; i < n; i++) {
        if (clipped[i].position.w <= 0.0f) return;
        nx[i] = (double)clipped[i].position.x / (double)clipped[i].position.w;
        ny[i] = (double)clipped[i].position.y / (double)clipped[i].position.w;
    }

    /* Twice the signed area of the clipped polygon (shoelace) */
    for (i = 0; i < n; i++) {
        j = (i + 1) % n;
        area2 += nx[i] * ny[j] - nx[j] * ny[i];
    }
    if (area2 == 0.0) return;  /* degenerate: no positive-area remnant */

    /* Face orientation in NDC: positive area = CCW */
    is_front = (ctx->front_face == GL_CCW) ? (area2 > 0.0) : (area2 < 0.0);
    if (ctx->flags & FLAG_CULL_FACE) {
        if (ctx->cull_face_mode == GL_FRONT_AND_BACK) return;
        if (ctx->cull_face_mode == GL_FRONT && is_front) return;
        if (ctx->cull_face_mode == GL_BACK && !is_front) return;
    }

    /* Depth extremes come from the complete clipped geometry */
    for (i = 0; i < n; i++) {
        select_accumulate(ctx, clipped[i].position.z, clipped[i].position.w);
    }
}

/* ------------------------------------------------------------------ */
/* Pipeline hooks (called from gl_api.c)                               */
/* ------------------------------------------------------------------ */

int mtgl_select_mode_supported(GLenum mode)
{
    switch (mode) {
        case GL_POINTS:
        case GL_LINES:
        case GL_LINE_STRIP:
        case GL_LINE_LOOP:
        case GL_TRIANGLES:
        case GL_TRIANGLE_STRIP:
        case GL_TRIANGLE_FAN:
            return 1;
        default:
            return 0;
    }
}

void mtgl_select_flush(GLState *ctx, GLenum mode)
{
    vertex_t *verts = vertex_buffer_data(ctx);
    size_t count = vertex_buffer_count(ctx);
    size_t i;

    switch (mode) {
        case GL_POINTS:
            for (i = 0; i < count; i++) {
                if (select_point_inside(&verts[i].position)) {
                    select_accumulate(ctx, verts[i].position.z, verts[i].position.w);
                }
            }
            break;
        case GL_LINES:
            for (i = 0; i + 1 < count; i += 2) {
                select_line(ctx, &verts[i], &verts[i + 1]);
            }
            break;
        case GL_LINE_STRIP:
            for (i = 0; i + 1 < count; i++) {
                select_line(ctx, &verts[i], &verts[i + 1]);
            }
            break;
        case GL_LINE_LOOP:
            for (i = 0; i + 1 < count; i++) {
                select_line(ctx, &verts[i], &verts[i + 1]);
            }
            if (count >= 2) {
                select_line(ctx, &verts[count - 1], &verts[0]);
            }
            break;
        case GL_TRIANGLES:
            for (i = 0; i + 2 < count; i += 3) {
                select_triangle(ctx, &verts[i], &verts[i + 1], &verts[i + 2]);
            }
            break;
        case GL_TRIANGLE_STRIP:
            for (i = 0; i + 2 < count; i++) {
                /* Alternate winding order, mirroring the rasterizer */
                if (i % 2 == 0) {
                    select_triangle(ctx, &verts[i], &verts[i + 1], &verts[i + 2]);
                } else {
                    select_triangle(ctx, &verts[i + 1], &verts[i], &verts[i + 2]);
                }
            }
            break;
        case GL_TRIANGLE_FAN:
            for (i = 1; i + 1 < count; i++) {
                select_triangle(ctx, &verts[0], &verts[i], &verts[i + 1]);
            }
            break;
        default:
            /* Unsupported modes are rejected in glBegin */
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void glSelectBuffer(GLsizei size, GLuint *buffer)
{
    GLState *ctx = gl_get_current_context();
    if (!ctx) return;

    if (ctx->flags & FLAG_INSIDE_BEGIN_END) {
        gl_set_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (size < 0) {
        gl_set_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (size > 0 && !buffer) {
        gl_set_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (ctx->render_mode == GL_SELECT) {
        /* Buffer binding may not be replaced during selection */
        gl_set_error(ctx, GL_INVALID_OPERATION);
        return;
    }

    /* Binding is immediate and persists until explicitly replaced;
     * it is never recorded into display lists. */
    ctx->select_buffer = buffer;
    ctx->select_buffer_size = size;
}

GLint glRenderMode(GLenum mode)
{
    GLState *ctx = gl_get_current_context();
    if (!ctx) return 0;

    if (ctx->flags & FLAG_INSIDE_BEGIN_END) {
        gl_set_error(ctx, GL_INVALID_OPERATION);
        return 0;
    }
    if (mode != GL_RENDER && mode != GL_SELECT) {
        gl_set_error(ctx, GL_INVALID_ENUM);
        return 0;
    }
    if (ctx->render_mode == mode) {
        return 0;  /* No transition */
    }

    if (mode == GL_SELECT) {
        /* Enter selection: results start empty, buffer binding kept */
        ctx->render_mode = GL_SELECT;
        ctx->select_buffer_offset = 0;
        ctx->select_hits = 0;
        ctx->select_overflow = 0;
        ctx->select_hit_pending = 0;
        return 0;
    }

    /* Leave selection: settle the pending record, then report */
    select_settle_record(ctx);
    ctx->render_mode = GL_RENDER;
    if (ctx->select_overflow) {
        return -1;
    }
    return (GLint)ctx->select_hits;
}

void glInitNames(void)
{
    GLState *ctx = gl_get_current_context();
    if (!ctx) return;

    if (ctx->flags & FLAG_INSIDE_BEGIN_END) {
        gl_set_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (list_record_init_names()) return;
    if (ctx->render_mode != GL_SELECT) return;

    select_settle_record(ctx);  /* Settle before the name stack changes */
    ctx->name_stack_depth = 0;
}

void glPushName(GLuint name)
{
    GLState *ctx = gl_get_current_context();
    if (!ctx) return;

    if (ctx->flags & FLAG_INSIDE_BEGIN_END) {
        gl_set_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (list_record_push_name(name)) return;
    if (ctx->render_mode != GL_SELECT) return;

    if (ctx->name_stack_depth >= MAX_NAME_STACK_DEPTH) {
        /* Failure: no settling, no stack change */
        gl_set_error(ctx, GL_STACK_OVERFLOW);
        return;
    }
    select_settle_record(ctx);  /* Settle before the name stack changes */
    ctx->name_stack[ctx->name_stack_depth++] = name;
}

void glPopName(void)
{
    GLState *ctx = gl_get_current_context();
    if (!ctx) return;

    if (ctx->flags & FLAG_INSIDE_BEGIN_END) {
        gl_set_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (list_record_pop_name()) return;
    if (ctx->render_mode != GL_SELECT) return;

    if (ctx->name_stack_depth <= 0) {
        /* Failure: no settling, no stack change */
        gl_set_error(ctx, GL_STACK_UNDERFLOW);
        return;
    }
    select_settle_record(ctx);  /* Settle before the name stack changes */
    ctx->name_stack_depth--;
}

void glLoadName(GLuint name)
{
    GLState *ctx = gl_get_current_context();
    if (!ctx) return;

    if (ctx->flags & FLAG_INSIDE_BEGIN_END) {
        gl_set_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (list_record_load_name(name)) return;
    if (ctx->render_mode != GL_SELECT) return;

    if (ctx->name_stack_depth <= 0) {
        /* Failure: no settling, no stack change */
        gl_set_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    select_settle_record(ctx);  /* Settle before the name stack changes */
    ctx->name_stack[ctx->name_stack_depth - 1] = name;
}
