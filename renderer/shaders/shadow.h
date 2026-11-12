#ifndef SHADOW_H
#define SHADOW_H

#include "../core/maths.h"
#include "../core/texture.h"

/*
 * optional percentage-closer soft shadows (pcss).
 *
 * the legacy hard shadow path is the default and stays untouched: scenes
 * without a pcss config, or configs whose emitter has zero size, keep the
 * original single depth comparison.
 *
 * only one projection convention is supported, matching the matrices in
 * maths.c: right handed, non-reversed depth, ndc z in the [-1, 1] range.
 */

typedef enum {
    SHADOW_MODE_HARD = 0,
    SHADOW_MODE_ORTHOGRAPHIC = 1,
    SHADOW_MODE_PERSPECTIVE = 2
} shadow_mode_t;

typedef struct {
    /* hard / orthographic directional light / perspective spot light */
    shadow_mode_t mode;
    /* positive light-space distances of the depth clipping planes */
    float near_plane;
    float far_plane;
    /*
     * full world-space width and height of the receiver plane projected
     * into the depth map: the orthographic coverage rectangle, or the
     * perspective near clipping plane.
     */
    float width;
    float height;
    /*
     * two dimensional emitter half size.
     * orthographic: tangent of the light angular half extent per axis.
     * perspective: world length of the half emitter on the light plane.
     * when both components are zero the hard shadow path is used.
     */
    vec2_t emitter_half_size;
} pcss_config_t;

/* returns 1 only when a config requests an actual soft shadow */
int pcss_config_is_soft(const pcss_config_t *config);

/*
 * shared sampling entry. light_clip is the full homogeneous light-space
 * position (light view-projection output), w included. n_dot_l keeps the
 * existing slope dependent bias, which is still applied in stored depth
 * domain. returns a visibility factor in the [0, 1] range.
 */
float shadow_hard_visibility(vec4_t light_clip, texture_t *shadow_map,
                             float n_dot_l);
float shadow_pcss_visibility(vec4_t light_clip, texture_t *shadow_map,
                             const pcss_config_t *config, float n_dot_l);

#endif
