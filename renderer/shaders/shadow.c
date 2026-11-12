#include <math.h>
#include "shadow.h"

/*
 * deterministic pcss, see
 * "Percentage-Closer Soft Shadows" (Fernando, 2005)
 *
 * the blocker search uses a 5x5 equidistant grid and the penumbra filter
 * a 7x7 equidistant grid, both inclusive at both ends. every tap reads
 * the nearest depth texel first and only then performs the comparison, so
 * depth values are never interpolated.
 */

#define SEARCH_GRID 5
#define FILTER_GRID 7

int pcss_config_is_soft(const pcss_config_t *config) {
    if (config == 0) {
        return 0;
    }
    if (config->mode != SHADOW_MODE_ORTHOGRAPHIC &&
        config->mode != SHADOW_MODE_PERSPECTIVE) {
        return 0;
    }
    if (config->near_plane <= 0 ||
        config->far_plane <= config->near_plane) {
        return 0;
    }
    if (config->width <= 0 || config->height <= 0) {
        return 0;
    }
    return config->emitter_half_size.x > 0 ||
           config->emitter_half_size.y > 0;
}

/* the legacy single comparison, kept as the hard shadow path */
float shadow_hard_visibility(vec4_t light_clip, texture_t *shadow_map,
                             float n_dot_l) {
    float u = (light_clip.x + 1) * 0.5f;
    float v = (light_clip.y + 1) * 0.5f;
    float d = (light_clip.z + 1) * 0.5f;
    float depth_bias = float_max(0.05f * (1 - n_dot_l), 0.005f);
    float current_depth = d - depth_bias;
    vec2_t texcoord = vec2_new(u, v);
    float closest_depth = texture_sample(shadow_map, texcoord).x;
    return current_depth > closest_depth ? 0.0f : 1.0f;
}

/* nearest texel lookup; taps outside the map are reported, not wrapped */
static float nearest_depth(texture_t *shadow_map, float u, float v,
                           int *outside) {
    int width = shadow_map->width;
    int height = shadow_map->height;
    int c, r, index;
    if (u < 0 || u > 1 || v < 0 || v > 1) {
        *outside = 1;
        return 1.0f;
    }
    *outside = 0;
    c = (int)((width - 1) * u);
    r = (int)((height - 1) * v);
    if (c < 0) {
        c = 0;
    } else if (c > width - 1) {
        c = width - 1;
    }
    if (r < 0) {
        r = 0;
    } else if (r > height - 1) {
        r = height - 1;
    }
    index = r * width + c;
    return shadow_map->buffer[index].x;
}

/*
 * stored [0, 1] depth back to a positive light-space distance.
 * orthographic depth is linear in distance, perspective depth is not.
 */
static float depth_to_distance(float depth01,
                               const pcss_config_t *config) {
    float near_plane = config->near_plane;
    float far_plane = config->far_plane;
    if (config->mode == SHADOW_MODE_ORTHOGRAPHIC) {
        return near_plane + depth01 * (far_plane - near_plane);
    } else {
        float denominator = far_plane - depth01 * (far_plane - near_plane);
        return near_plane * far_plane / denominator;
    }
}

/* world radii on the receiver plane -> uv radii of the depth map */
static void world_to_uv_radius(float world_x, float world_y,
                               float receiver,
                               const pcss_config_t *config,
                               float *u_radius, float *v_radius) {
    if (config->mode == SHADOW_MODE_ORTHOGRAPHIC) {
        *u_radius = world_x / config->width;
        *v_radius = world_y / config->height;
    } else {
        /* the projected frustum grows linearly with the distance */
        float projected_width = config->width * receiver / config->near_plane;
        float projected_height = config->height * receiver / config->near_plane;
        *u_radius = world_x / projected_width;
        *v_radius = world_y / projected_height;
    }
}

static float grid_offset(int index, int grid_size) {
    return (2.0f * (float)index / (float)(grid_size - 1)) - 1.0f;
}

float shadow_pcss_visibility(vec4_t light_clip, texture_t *shadow_map,
                             const pcss_config_t *config, float n_dot_l) {
    float near_plane = config->near_plane;
    float far_plane = config->far_plane;
    float half_w = config->width * 0.5f;
    float half_h = config->height * 0.5f;
    float emitter_x = config->emitter_half_size.x;
    float emitter_y = config->emitter_half_size.y;
    float depth_bias = float_max(0.05f * (1 - n_dot_l), 0.005f);
    float w = light_clip.w;
    float ndc_x, ndc_y, ndc_z, depth01, current_depth;
    float receiver, u_center, v_center;
    float search_x, search_y, search_ur, search_vr;
    float penumbra_x, penumbra_y, filter_ur, filter_vr;
    float blocker_sum, average_blocker, visibility;
    int blocker_count, lit_count, i, j;

    if (w <= 0) {
        return 1.0f;
    }
    ndc_x = light_clip.x / w;
    ndc_y = light_clip.y / w;
    ndc_z = light_clip.z / w;
    if (ndc_x < -1 || ndc_x > 1 || ndc_y < -1 || ndc_y > 1 ||
        ndc_z < -1 || ndc_z > 1) {
        return 1.0f;
    }

    depth01 = ndc_z * 0.5f + 0.5f;
    receiver = depth_to_distance(depth01, config);
    if (receiver < near_plane || receiver > far_plane) {
        return 1.0f;
    }
    current_depth = depth01 - depth_bias;
    u_center = ndc_x * 0.5f + 0.5f;
    v_center = ndc_y * 0.5f + 0.5f;

    /*
     * blocker search: per axis radii that grow with the distance from near.
     * orthographic radii use the angular tangent, perspective radii follow
     * the near plane frustum relation.
     */
    if (config->mode == SHADOW_MODE_ORTHOGRAPHIC) {
        search_x = (receiver - near_plane) * emitter_x;
        search_y = (receiver - near_plane) * emitter_y;
    } else {
        search_x = half_w * (receiver - near_plane) / near_plane;
        search_y = half_h * (receiver - near_plane) / near_plane;
    }
    world_to_uv_radius(search_x, search_y, receiver, config,
                       &search_ur, &search_vr);

    blocker_sum = 0;
    blocker_count = 0;
    for (i = 0; i < SEARCH_GRID; i++) {
        float u = u_center + grid_offset(i, SEARCH_GRID) * search_ur;
        for (j = 0; j < SEARCH_GRID; j++) {
            int outside = 0;
            float v = v_center + grid_offset(j, SEARCH_GRID) * search_vr;
            float closest_depth = nearest_depth(shadow_map, u, v, &outside);
            if (!outside && closest_depth < current_depth) {
                blocker_sum += depth_to_distance(closest_depth, config);
                blocker_count += 1;
            }
        }
    }
    if (blocker_count == 0) {
        return 1.0f;
    }
    average_blocker = blocker_sum / (float)blocker_count;

    /*
     * penumbra filter from the per-axis similar-triangles relation,
     * evaluated with light-space distances rather than nonlinear depth
     * differences.
     */
    if (config->mode == SHADOW_MODE_ORTHOGRAPHIC) {
        penumbra_x = (receiver - average_blocker) * emitter_x;
        penumbra_y = (receiver - average_blocker) * emitter_y;
    } else {
        penumbra_x = emitter_x * (receiver - average_blocker) /
                     average_blocker;
        penumbra_y = emitter_y * (receiver - average_blocker) /
                     average_blocker;
    }
    world_to_uv_radius(penumbra_x, penumbra_y, receiver, config,
                       &filter_ur, &filter_vr);

    lit_count = 0;
    for (i = 0; i < FILTER_GRID; i++) {
        float u = u_center + grid_offset(i, FILTER_GRID) * filter_ur;
        for (j = 0; j < FILTER_GRID; j++) {
            int outside = 0;
            float v = v_center + grid_offset(j, FILTER_GRID) * filter_vr;
            float closest_depth = nearest_depth(shadow_map, u, v, &outside);
            if (outside || closest_depth >= current_depth) {
                lit_count += 1;
            }
        }
    }
    visibility = (float)lit_count / (float)(FILTER_GRID * FILTER_GRID);
    return float_saturate(visibility);
}
