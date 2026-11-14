#ifndef RELIEF_SHADER_H
#define RELIEF_SHADER_H

#include "../core/api.h"

/* high-level api */

typedef struct {
    /* height grid: width * height values, row major, each in [0, 1] */
    int width, height;
    const float *heights;
    /* recess depth, the surface is z = -scale * h(u, v) */
    float scale;
    /* constant diffuse color */
    vec3_t basecolor;
    /* perspective camera depth range */
    float near_plane;
    float far_plane;
} relief_material_t;

/*
 * build a model around the local [0, 1]^2, z = 0 base quad; the height data
 * is copied, so the caller may release its grid afterwards. invalid sizes,
 * non-finite or out-of-range heights and non-finite parameters are rejected.
 */
model_t *relief_create_model(const relief_material_t *material,
                             mat4_t transform);

/* low-level test entry */

typedef struct {
    vec3_t position;   /* hit point in local coordinates */
    vec2_t uv;         /* surface coordinates in [0, 1]^2 */
    vec3_t normal;     /* local surface normal */
    int hit;
} relief_hit_t;

/*
 * intersect the ray (origin, unit direction) with the recessed bilinear
 * surface; the ray is only traced inside the local column
 * [0, 1]^2 x [-scale, 0], leaving through a side counts as a miss.
 * returns 1 on a hit, 0 when the ray misses or is discarded.
 */
int relief_intersect(const relief_material_t *material,
                     vec3_t origin, vec3_t direction,
                     relief_hit_t *hit);

#endif
