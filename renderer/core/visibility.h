#ifndef VISIBILITY_H
#define VISIBILITY_H

#include <stdint.h>
#include "graphics.h"

/*
 * Re-colorable visibility buffer.
 *
 * The visibility stage runs the regular vertex/clipping/rasterization rules
 * once and records, for every finally visible pixel, the identity of the
 * original triangle, its barycentric position in that triangle, its depth
 * and its facing.  Shading can then be re-run with a different program
 * (uniforms) as many times as desired without touching geometry again.
 *
 * A visibility buffer owns a copy of the three original-vertex varying
 * blocks for each submitted triangle, so the caller may release or reuse
 * the program that produced them.
 */

typedef struct visibility visibility_t;

typedef struct {
    uint32_t primitive_id;  /* UINT32_MAX for background pixels */
    float bary[3];          /* weights of the three original vertices */
    float depth;
    int backface;
} visibility_sample_t;

/*
 * Coverage callback.  Receives the perspective-correct interpolated
 * varyings and the current uniforms.  A non-zero return keeps the
 * candidate; a zero return culls it without occupying depth.
 */
typedef int visibility_coverage_t(
    void *varyings, void *uniforms, int backface);

/*
 * Recoloring shader for visibility_resolve().  There is no discard.
 */
typedef vec4_t visibility_shader_t(
    const void *varyings, void *uniforms,
    uint32_t primitive_id, int backface);

/* create / release / clear */
visibility_t *visibility_create(int width, int height);
void visibility_release(visibility_t *buffer);
void visibility_clear(visibility_t *buffer);

/*
 * Submit one triangle from the program's current attribs and uniforms.
 * Returns 0 on success, non-zero on failure.  primitive_id must be unique
 * until the next clear() and must not be UINT32_MAX.  Programs with
 * blending enabled are rejected before any callback runs or any buffer
 * state changes.  The program's fragment shader is never called.
 */
int visibility_draw_triangle(
    visibility_t *buffer, program_t *program,
    uint32_t primitive_id, visibility_coverage_t *coverage);

/*
 * Query a pixel.  Returns 1 if the pixel is covered, 0 for background.
 * Background samples report primitive_id UINT32_MAX.
 */
int visibility_read(
    const visibility_t *buffer, int x, int y, visibility_sample_t *out);

/*
 * Shade every covered pixel exactly once with the stored primitive_id,
 * backface and original-triangle barycentric weights.  Retained depth is
 * written to the framebuffer, RGB is written from the shader color, and
 * the alpha byte and all background pixels are left untouched.  The
 * framebuffer dimensions must match the visibility buffer.
 */
int visibility_resolve(
    const visibility_t *buffer, framebuffer_t *framebuffer,
    visibility_shader_t *shader, void *uniforms);

#endif
