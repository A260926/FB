/*
 * MyTinyGL - OpenGL 1.x Software Renderer
 * selection.c - GL_SELECT render mode implementation
 *
 * Hit testing runs on the transformed and clipped geometry only: it never
 * consults the color, depth or stencil buffers and never writes to them.
 * Points must survive the frustum, lines must leave a non-zero segment and
 * triangles must leave a positive-area fragment (still honoring face
 * culling), even when no pixel center is covered in the viewport.
 */

#include "GL/gl.h"
#include "graphics.h"
#include "mytinygl.h"
#include "clipping.h"
#include "selection.h"
#include "lists.h"
#include <math.h>
#include <string.h>

/* Largest window-depth value representable in a GLuint hit record. */
#define DEPTH_ONE 4294967295.0

void selection_init(selection_t *sel)
{
    sel->render_mode = GL_RENDER;
    sel->buffer = NULL;
    sel->buffer_size = 0;
    sel->name_count = 0;
    sel->result_count = 0;
    sel->hit_count = 0;
    sel->overflow = GL_FALSE;
    sel->pending = GL_FALSE;
    sel->pending_min = 0.0;
    sel->pending_max = 0.0;
    sel->pending_name_count = 0;
}

int selection_primitive_supported(GLenum mode)
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

/* ---- depth mapping ----------------------------------------------------- */

/* NDC z (post-clip, z/w) mapped through glDepthRange and clamped to [0,1]. */
static double window_depth(GLState *c, float clip_z, float clip_w)
{
    double z = (double)clip_z / (double)clip_w;
    double d = (z + 1.0) * 0.5 *
                   ((double)c->depth_far - (double)c->depth_near)
               + (double)c->depth_near;
    if (d < 0.0) d = 0.0;
    if (d > 1.0) d = 1.0;
    return d;
}

static GLuint depth_to_uint(double d)
{
    double q = floor(d * DEPTH_ONE + 0.5);
    if (q < 0.0) q = 0.0;
    if (q > DEPTH_ONE) q = DEPTH_ONE;
    return (GLuint)q;
}

/* ---- hit aggregation and record output -------------------------------- */

static void note_hit(GLState *c, double dmin, double dmax)
{
    selection_t *s = &c->selection;
    if (s->render_mode != GL_SELECT) return;

    if (!s->pending) {
        s->pending = GL_TRUE;
        s->pending_min = dmin;
        s->pending_max = dmax;
        s->pending_name_count = s->name_count;
        memcpy(s->pending_names, s->names,
               (size_t)s->name_count * sizeof(GLuint));
    } else {
        if (dmin < s->pending_min) s->pending_min = dmin;
        if (dmax > s->pending_max) s->pending_max = dmax;
    }
}

/* Write the pending record. Records are always written whole; once the
 * buffer cannot hold a record it locks and no further words are written. */
static void flush_pending(GLState *c)
{
    selection_t *s = &c->selection;
    if (!s->pending) return;
    s->pending = GL_FALSE;

    if (s->overflow) return;

    GLsizei length = 3 + s->pending_name_count;
    if (s->buffer == NULL ||
        s->result_count + length > s->buffer_size) {
        s->overflow = GL_TRUE;
        return;
    }

    GLuint *out = s->buffer + s->result_count;
    out[0] = (GLuint)s->pending_name_count;
    out[1] = depth_to_uint(s->pending_min);
    out[2] = depth_to_uint(s->pending_max);
    for (GLsizei i = 0; i < s->pending_name_count; i++) {
        out[3 + i] = s->pending_names[i];
    }

    s->result_count += length;
    s->hit_count++;
}

/* ---- primitive observation -------------------------------------------- */

void selection_observe_points(const vertex_t *verts, size_t count)
{
    GLState *c = gl_get_current_context();
    if (!c || c->selection.render_mode != GL_SELECT) return;

    int hit = 0;
    double dmin = 1.0, dmax = 0.0;

    for (size_t i = 0; i < count; i++) {
        const vec4_t *p = &verts[i].position;
        /* Same six-plane homogeneous test as the point draw chain. */
        if (p->w <= 0.0f) continue;
        if (p->x < -p->w || p->x > p->w ||
            p->y < -p->w || p->y > p->w ||
            p->z < -p->w || p->z > p->w) {
            continue;
        }
        double d = window_depth(c, p->z, p->w);
        if (!hit) { dmin = dmax = d; hit = 1; }
        else { if (d < dmin) dmin = d; if (d > dmax) dmax = d; }
    }

    if (hit) note_hit(c, dmin, dmax);
}

void selection_observe_lines(const vertex_t *verts, size_t count, GLenum mode)
{
    GLState *c = gl_get_current_context();
    if (!c || c->selection.render_mode != GL_SELECT) return;

    int hit = 0;
    double dmin = 1.0, dmax = 0.0;

    /* Enumerate the segments of the primitive as (a, b) index pairs. */
    size_t segments = 0;
    if (mode == GL_LINES) {
        segments = count / 2;
    } else if (count >= 2) {
        segments = count - 1;                 /* strip / loop chain */
        if (mode == GL_LINE_LOOP) segments++; /* closing segment */
    }

    for (size_t seg = 0; seg < segments; seg++) {
        size_t ia, ib;
        if (mode == GL_LINES) {
            ia = seg * 2;
            ib = ia + 1;
        } else if (seg < count - 1) {
            ia = seg;
            ib = seg + 1;
        } else {
            ia = count - 1;                  /* loop closure */
            ib = 0;
        }

        vertex_t v0 = verts[ia];
        vertex_t v1 = verts[ib];

        if (!clip_line(&v0, &v1)) continue;

        /* Require a genuinely non-zero surviving segment. */
        if (v0.position.x == v1.position.x &&
            v0.position.y == v1.position.y &&
            v0.position.z == v1.position.z &&
            v0.position.w == v1.position.w) {
            continue;
        }

        double d0 = window_depth(c, v0.position.z, v0.position.w);
        double d1 = window_depth(c, v1.position.z, v1.position.w);
        double lo = d0 < d1 ? d0 : d1;
        double hi = d0 > d1 ? d0 : d1;
        if (!hit) { dmin = lo; dmax = hi; hit = 1; }
        else { if (lo < dmin) dmin = lo; if (hi > dmax) dmax = hi; }
    }

    if (hit) note_hit(c, dmin, dmax);
}

void selection_observe_triangle(const vertex_t *triangle_src)
{
    GLState *c = gl_get_current_context();
    if (!c || c->selection.render_mode != GL_SELECT) return;

    vertex_t triangle[3];
    vertex_t clipped[MAX_CLIP_VERTS];
    for (int i = 0; i < 3; i++) {
        triangle[i] = triangle_src[i];
    }

    int clip_count = clip_triangle(triangle, clipped);
    if (clip_count < 3) return;

    /* Perspective divide; the six-plane clip guarantees w > 0. */
    float ndc_x[MAX_CLIP_VERTS], ndc_y[MAX_CLIP_VERTS];
    for (int i = 0; i < clip_count; i++) {
        float inv_w = 1.0f / clipped[i].position.w;
        ndc_x[i] = clipped[i].position.x * inv_w;
        ndc_y[i] = clipped[i].position.y * inv_w;
    }

    /* The clipped polygon is triangulated as a fan; decide culling on the
     * geometric (NDC) winding so sub-pixel triangles still register. */
    int surviving = 0;
    int cull_enabled = c->flags & FLAG_CULL_FACE;
    for (int j = 1; j + 1 < clip_count; j++) {
        float area = (ndc_x[j] - ndc_x[0]) * (ndc_y[j + 1] - ndc_y[0])
                   - (ndc_x[j + 1] - ndc_x[0]) * (ndc_y[j] - ndc_y[0]);
        if (area == 0.0f) continue; /* degenerate fan piece */

        if (cull_enabled) {
            int is_front;
            if (c->front_face == GL_CCW) {
                is_front = (area > 0.0f);  /* NDC keeps y pointing up */
            } else {
                is_front = (area < 0.0f);
            }
            if (c->cull_face_mode == GL_FRONT_AND_BACK) return;
            if (c->cull_face_mode == GL_FRONT && is_front) continue;
            if (c->cull_face_mode == GL_BACK && !is_front) continue;
        }
        surviving++;
    }
    if (!surviving) return;

    /* Depth extrema over the whole clipped geometry. */
    double dmin = 1.0, dmax = 0.0;
    for (int i = 0; i < clip_count; i++) {
        double d = window_depth(c, clipped[i].position.z, clipped[i].position.w);
        if (i == 0 || d < dmin) dmin = d;
        if (i == 0 || d > dmax) dmax = d;
    }

    note_hit(c, dmin, dmax);
}

/* ---- API entry points -------------------------------------------------- */

static int inside_begin_end(GLState *c)
{
    return (c->flags & FLAG_INSIDE_BEGIN_END) != 0;
}

void glSelectBuffer(GLsizei size, GLuint *buffer)
{
    GLState *c = gl_get_current_context();
    if (!c) return;

    if (inside_begin_end(c)) {
        gl_set_error(c, GL_INVALID_OPERATION);
        return;
    }
    if (size < 0 || (size > 0 && buffer == NULL)) {
        gl_set_error(c, GL_INVALID_VALUE);
        return;
    }
    if (c->selection.render_mode == GL_SELECT) {
        gl_set_error(c, GL_INVALID_OPERATION);
        return;
    }

    c->selection.buffer = buffer;
    c->selection.buffer_size = size;
}

GLint glRenderMode(GLenum mode)
{
    GLState *c = gl_get_current_context();
    if (!c) return GL_RENDER;

    selection_t *s = &c->selection;

    if (inside_begin_end(c)) {
        gl_set_error(c, GL_INVALID_OPERATION);
        return (GLint)s->render_mode;
    }
    if (mode != GL_RENDER && mode != GL_SELECT) {
        gl_set_error(c, GL_INVALID_ENUM);
        return (GLint)s->render_mode;
    }

    GLenum previous = s->render_mode;
    if (mode == previous) return (GLint)previous;

    if (mode == GL_SELECT) {
        /* Every selection pass starts from an empty result set. */
        s->result_count = 0;
        s->hit_count = 0;
        s->overflow = GL_FALSE;
        s->pending = GL_FALSE;
        s->render_mode = GL_SELECT;
        return (GLint)previous;
    }

    /* Leaving selection: flush the last record and report the pass. */
    flush_pending(c);
    s->render_mode = GL_RENDER;
    if (s->overflow) return -1;
    return (GLint)s->hit_count;
}

void glInitNames(void)
{
    GLState *c = gl_get_current_context();
    if (!c) return;
    if (inside_begin_end(c)) {
        gl_set_error(c, GL_INVALID_OPERATION);
        return;
    }
    if (list_record_init_names()) return;
    if (c->selection.render_mode != GL_SELECT) return;

    flush_pending(c);
    c->selection.name_count = 0;
}

void glPushName(GLuint name)
{
    GLState *c = gl_get_current_context();
    if (!c) return;
    if (inside_begin_end(c)) {
        gl_set_error(c, GL_INVALID_OPERATION);
        return;
    }
    if (list_record_push_name(name)) return;

    selection_t *s = &c->selection;
    if (s->render_mode != GL_SELECT) return;

    if (s->name_count >= MAX_NAME_STACK_DEPTH) {
        gl_set_error(c, GL_STACK_OVERFLOW);
        return;
    }
    flush_pending(c);
    s->names[s->name_count++] = name;
}

void glPopName(void)
{
    GLState *c = gl_get_current_context();
    if (!c) return;
    if (inside_begin_end(c)) {
        gl_set_error(c, GL_INVALID_OPERATION);
        return;
    }
    if (list_record_pop_name()) return;

    selection_t *s = &c->selection;
    if (s->render_mode != GL_SELECT) return;

    if (s->name_count <= 0) {
        gl_set_error(c, GL_STACK_UNDERFLOW);
        return;
    }
    flush_pending(c);
    s->name_count--;
}

void glLoadName(GLuint name)
{
    GLState *c = gl_get_current_context();
    if (!c) return;
    if (inside_begin_end(c)) {
        gl_set_error(c, GL_INVALID_OPERATION);
        return;
    }
    if (list_record_load_name(name)) return;

    selection_t *s = &c->selection;
    if (s->render_mode != GL_SELECT) return;

    if (s->name_count <= 0) {
        gl_set_error(c, GL_INVALID_OPERATION);
        return;
    }
    flush_pending(c);
    s->names[s->name_count - 1] = name;
}

/* ---- display list recording ------------------------------------------- */

static int record_name_op(list_opcode_t opcode, GLuint name)
{
    GLState *c = gl_get_current_context();
    if (!c || c->list_index == 0) return 0;

    display_list_t *list = list_get(&c->lists, c->list_index);
    if (list) {
        list_command_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = opcode;
        cmd.data.name_op.name = name;
        list_add_command(list, &cmd);
    }
    return (c->list_mode == GL_COMPILE);
}

int list_record_init_names(void)
{
    return record_name_op(CMD_INIT_NAMES, 0);
}

int list_record_push_name(GLuint name)
{
    return record_name_op(CMD_PUSH_NAME, name);
}

int list_record_pop_name(void)
{
    return record_name_op(CMD_POP_NAME, 0);
}

int list_record_load_name(GLuint name)
{
    return record_name_op(CMD_LOAD_NAME, name);
}
