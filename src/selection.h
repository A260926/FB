/*
 * MyTinyGL - OpenGL 1.x Software Renderer
 * selection.h - GL_SELECT render mode: name stack and hit records
 *
 * Selection reuses the ordinary transform/clip pipeline but never touches
 * the color, depth or stencil buffers. A hit is decided geometrically on the
 * clipped primitive, not on pixel coverage or framebuffer readback.
 */

#ifndef MYTINYGL_SELECTION_H
#define MYTINYGL_SELECTION_H

#include "GL/gl.h"
#include "graphics.h"
#include <stddef.h>

/* Fixed capacity of the name stack (GL_MAX_NAME_STACK_DEPTH). */
#define MAX_NAME_STACK_DEPTH 64

/* Per-context selection state. Embedded directly in GLState. */
typedef struct {
    /* Render mode: GL_RENDER (default) or GL_SELECT. */
    GLenum render_mode;

    /* Bound selection buffer. The binding persists until replaced. */
    GLuint *buffer;
    GLsizei buffer_size;          /* Allowed to be zero; NULL until bound. */

    /* Name stack (bottom to top). */
    GLuint names[MAX_NAME_STACK_DEPTH];
    GLsizei name_count;

    /* Output bookkeeping, reset when entering GL_SELECT. */
    GLsizei result_count;         /* Words already written to the buffer. */
    GLsizei hit_count;            /* Fully written hit records. */
    GLboolean overflow;           /* Locked once a record cannot fit. */

    /* Pending record for the geometry drawn under the current name stack.
     * glEnd does not flush it; the record is emitted when the stack changes
     * or when selection mode is left. */
    GLboolean pending;            /* A hit is waiting to be written. */
    double pending_min;           /* Mapped-window depth extrema. */
    double pending_max;
    GLuint pending_names[MAX_NAME_STACK_DEPTH];
    GLsizei pending_name_count;
} selection_t;

/* Lifecycle */
void selection_init(selection_t *sel);

/* API entry points (implemented in selection.c) */
void glSelectBuffer(GLsizei size, GLuint *buffer);
GLint glRenderMode(GLenum mode);
void glInitNames(void);
void glPushName(GLuint name);
void glPopName(void);
void glLoadName(GLuint name);

/* Display-list recording hooks (1 = execution is suppressed by the list). */
int list_record_init_names(void);
int list_record_push_name(GLuint name);
int list_record_pop_name(void);
int list_record_load_name(GLuint name);

/* Validate a primitive mode for selection. Unsupported primitives raise
 * GL_INVALID_ENUM and leave the mode and existing results untouched. */
int selection_primitive_supported(GLenum mode);

/* Feed one fully transformed primitive. Inputs are clip-space vertices from
 * the ordinary vertex buffer; the same six-plane clipping is applied here.
 * Culling for triangles is handled internally. */
void selection_observe_points(const vertex_t *verts, size_t count);
void selection_observe_lines(const vertex_t *verts, size_t count, GLenum mode);
void selection_observe_triangle(const vertex_t *triangle);

#endif /* MYTINYGL_SELECTION_H */
