#ifndef GRAPHICS_INTERNAL_H
#define GRAPHICS_INTERNAL_H

#include "graphics.h"
#include "maths.h"

#define GRAPHICS_MAX_VARYINGS 10

/*
 * Shared graphics pipeline internals.
 *
 * The clipping and rasterization rules live here so that the regular
 * framebuffer pipeline and the visibility capture pipeline cannot diverge.
 * This header is private to renderer/core; it is not part of the public API.
 */

struct program {
    vertex_shader_t *vertex_shader;
    fragment_shader_t *fragment_shader;
    int sizeof_attribs;
    int sizeof_varyings;
    int sizeof_uniforms;
    int double_sided;
    int enable_blend;
    /* for shaders */
    void *shader_attribs[3];
    void *shader_varyings;
    void *shader_uniforms;
    /* for clipping */
    vec4_t in_coords[GRAPHICS_MAX_VARYINGS];
    vec4_t out_coords[GRAPHICS_MAX_VARYINGS];
    void *in_varyings[GRAPHICS_MAX_VARYINGS];
    void *out_varyings[GRAPHICS_MAX_VARYINGS];
};

typedef struct {
    int backface;
    int index;
    float depth;
    vec3_t weights;
    float recip_w[3];
    vec4_t clip_coords[3];
    void *varyings[3];
} raster_fragment_t;

/*
 * Called for every fragment candidate that passed the shared coverage and
 * depth rules.  Return non-zero to commit the fragment; the caller then
 * writes the depth value.  Returning zero leaves the depth untouched.
 */
typedef int raster_fragment_fn(void *user, const raster_fragment_t *fragment);

/*
 * Clip a triangle against the homogeneous view frustum.  Barycentric
 * coordinates relative to the three original vertices may be supplied;
 * they are interpolated alongside the coordinates and varyings.  Pass NULL
 * for both bary arrays to skip that work.
 */
int graphics_clip_triangle(
    int sizeof_varyings,
    vec4_t in_coords[GRAPHICS_MAX_VARYINGS],
    void *in_varyings[GRAPHICS_MAX_VARYINGS],
    float in_bary[GRAPHICS_MAX_VARYINGS][3],
    vec4_t out_coords[GRAPHICS_MAX_VARYINGS],
    void *out_varyings[GRAPHICS_MAX_VARYINGS],
    float out_bary[GRAPHICS_MAX_VARYINGS][3]);

/*
 * Rasterize one triangle given in clip space.  Returns 1 if the triangle
 * was culled by the shared back-face rule, otherwise 0.
 */
int graphics_rasterize_triangle(
    int width, int height, float *depth_buffer,
    vec4_t clip_coords[3], void *varyings[3], int double_sided,
    raster_fragment_fn *fragment_fn, void *user);

/* Perspective-correct interpolation used by both pipelines. */
void graphics_interpolate_varyings(
    void *const src_varyings[3], void *dst_varyings, int sizeof_varyings,
    vec3_t weights, const float recip_w[3]);

#endif
