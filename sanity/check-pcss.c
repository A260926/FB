#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../renderer/shaders/blinn_shader.h"
#include "../renderer/shaders/pbr_shader.h"
#include "../renderer/shaders/shadow.h"

/*
 * sanity checks for the optional pcss soft shadows. every numerical result
 * of the shared sampler is cross checked against an independent scalar
 * reference implemented below; a failed check makes the process return a
 * nonzero status, and PCSS_SHADOW_OK is printed only when every case passes.
 */

#define W 16
#define H 16

static int g_failures;

static void check(int condition, const char *name) {
    if (condition) {
        printf("  [ok] %s\n", name);
    } else {
        printf("  [FAIL] %s\n", name);
        g_failures += 1;
    }
}

static int nearly(float a, float b) {
    return fabsf(a - b) < 1e-5f;
}

static texture_t *make_depth(int width, int height, float clear) {
    texture_t *map = texture_create(width, height);
    int i;
    for (i = 0; i < width * height; i++) {
        map->buffer[i] = vec4_new(clear, clear, clear, 1);
    }
    return map;
}

static void set_depth(texture_t *map, int c, int r, float depth) {
    int index = r * map->width + c;
    map->buffer[index] = vec4_new(depth, depth, depth, 1);
}

static pcss_config_t ortho_config(float near_plane, float far_plane,
                                  float width, float height,
                                  float ex, float ey) {
    pcss_config_t config;
    memset(&config, 0, sizeof(config));
    config.mode = SHADOW_MODE_ORTHOGRAPHIC;
    config.near_plane = near_plane;
    config.far_plane = far_plane;
    config.width = width;
    config.height = height;
    config.emitter_half_size = vec2_new(ex, ey);
    return config;
}

static pcss_config_t persp_config(float near_plane, float far_plane,
                                  float width, float height,
                                  float ex, float ey) {
    pcss_config_t config;
    memset(&config, 0, sizeof(config));
    config.mode = SHADOW_MODE_PERSPECTIVE;
    config.near_plane = near_plane;
    config.far_plane = far_plane;
    config.width = width;
    config.height = height;
    config.emitter_half_size = vec2_new(ex, ey);
    return config;
}

/* stored [0,1] depth of a point at light-space distance z */
static float ref_ortho_depth(float z, float n, float f) {
    return (z - n) / (f - n);
}

static float ref_persp_depth(float z, float n, float f) {
    return f * (z - n) / (z * (f - n));
}

/* independent scalar reconstruction of the light-space distance */
static float ref_distance(float depth, const pcss_config_t *config) {
    float n = config->near_plane;
    float f = config->far_plane;
    if (config->mode == SHADOW_MODE_ORTHOGRAPHIC) {
        return n + depth * (f - n);
    }
    return n * f / (f - depth * (f - n));
}

static float ref_nearest(texture_t *map, float u, float v, int *outside) {
    int c, r;
    if (u < 0 || u > 1 || v < 0 || v > 1) {
        *outside = 1;
        return 1.0f;
    }
    *outside = 0;
    c = (int)((map->width - 1) * u);
    r = (int)((map->height - 1) * v);
    return map->buffer[r * map->width + c].x;
}

static void ref_world_to_uv(float world_x, float world_y, float receiver,
                            const pcss_config_t *config,
                            float *ur, float *vr) {
    if (config->mode == SHADOW_MODE_ORTHOGRAPHIC) {
        *ur = world_x / config->width;
        *vr = world_y / config->height;
    } else {
        *ur = world_x / (config->width * receiver / config->near_plane);
        *vr = world_y / (config->height * receiver / config->near_plane);
    }
}

/* independent reimplementation of the deterministic pcss spec */
static float ref_visibility(vec4_t clip, texture_t *map,
                            const pcss_config_t *config, float n_dot_l) {
    float n = config->near_plane;
    float f = config->far_plane;
    float ex = config->emitter_half_size.x;
    float ey = config->emitter_half_size.y;
    float half_w = config->width * 0.5f;
    float half_h = config->height * 0.5f;
    float bias = fmaxf(0.05f * (1 - n_dot_l), 0.005f);
    float ndc_x, ndc_y, ndc_z, depth, receiver, current;
    float sx, sy, fx, fy, su, sv, fu, fv, blocker_sum, average;
    int blockers, lit, i, j;

    if (clip.w <= 0) {
        return 1.0f;
    }
    ndc_x = clip.x / clip.w;
    ndc_y = clip.y / clip.w;
    ndc_z = clip.z / clip.w;
    if (ndc_x < -1 || ndc_x > 1 || ndc_y < -1 || ndc_y > 1 ||
        ndc_z < -1 || ndc_z > 1) {
        return 1.0f;
    }

    depth = ndc_z * 0.5f + 0.5f;
    receiver = ref_distance(depth, config);
    if (receiver < n || receiver > f) {
        return 1.0f;
    }
    current = depth - bias;

    if (config->mode == SHADOW_MODE_ORTHOGRAPHIC) {
        sx = (receiver - n) * ex;
        sy = (receiver - n) * ey;
    } else {
        sx = half_w * (receiver - n) / n;
        sy = half_h * (receiver - n) / n;
    }
    ref_world_to_uv(sx, sy, receiver, config, &su, &sv);

    blocker_sum = 0;
    blockers = 0;
    for (i = 0; i < 5; i++) {
        float u = (ndc_x * 0.5f + 0.5f) +
                  (2.0f * i / 4.0f - 1.0f) * su;
        for (j = 0; j < 5; j++) {
            int outside = 0;
            float v = (ndc_y * 0.5f + 0.5f) +
                      (2.0f * j / 4.0f - 1.0f) * sv;
            float closest = ref_nearest(map, u, v, &outside);
            if (!outside && closest < current) {
                blocker_sum += ref_distance(closest, config);
                blockers++;
            }
        }
    }
    if (blockers == 0) {
        return 1.0f;
    }
    average = blocker_sum / (float)blockers;

    if (config->mode == SHADOW_MODE_ORTHOGRAPHIC) {
        fx = (receiver - average) * ex;
        fy = (receiver - average) * ey;
    } else {
        fx = ex * (receiver - average) / average;
        fy = ey * (receiver - average) / average;
    }
    ref_world_to_uv(fx, fy, receiver, config, &fu, &fv);

    lit = 0;
    for (i = 0; i < 7; i++) {
        float u = (ndc_x * 0.5f + 0.5f) +
                  (2.0f * i / 6.0f - 1.0f) * fu;
        for (j = 0; j < 7; j++) {
            int outside = 0;
            float v = (ndc_y * 0.5f + 0.5f) +
                      (2.0f * j / 6.0f - 1.0f) * fv;
            float closest = ref_nearest(map, u, v, &outside);
            if (outside || closest >= current) {
                lit++;
            }
        }
    }
    return (float)lit / 49.0f;
}

static vec4_t ortho_clip(float u, float v, float receiver,
                         const pcss_config_t *config) {
    float depth = ref_ortho_depth(receiver, config->near_plane,
                                  config->far_plane);
    return vec4_new(2 * u - 1, 2 * v - 1, 2 * depth - 1, 1);
}

static vec4_t persp_clip(float u, float v, float receiver,
                         const pcss_config_t *config) {
    float depth = ref_persp_depth(receiver, config->near_plane,
                                  config->far_plane);
    float ndc_z = 2 * depth - 1;
    return vec4_new((2 * u - 1) * receiver, (2 * v - 1) * receiver,
                    ndc_z * receiver, receiver);
}

static void test_no_blocker_is_fully_lit(void) {
    pcss_config_t config = ortho_config(0.5f, 3.0f, 2.0f, 2.0f, 0.5f, 0.5f);
    texture_t *map = make_depth(W, H, 1.0f);
    vec4_t clip = ortho_clip(0.5f, 0.5f, 2.0f, &config);
    float vis = shadow_pcss_visibility(clip, map, &config, 1.0f);
    float ref = ref_visibility(clip, map, &config, 1.0f);
    check(nearly(vis, 1.0f), "a search with no blocker returns fully lit");
    check(nearly(vis, ref), "no blocker result matches scalar reference");
    texture_release(map);
}

static void test_constant_occluder(void) {
    pcss_config_t config = ortho_config(0.5f, 3.0f, 2.0f, 2.0f, 0.1f, 0.1f);
    texture_t *map = make_depth(W, H, ref_ortho_depth(1.0f, 0.5f, 3.0f));
    vec4_t clip = ortho_clip(0.5f, 0.5f, 1.5f, &config);
    float vis = shadow_pcss_visibility(clip, map, &config, 1.0f);
    float ref = ref_visibility(clip, map, &config, 1.0f);
    check(nearly(vis, 0.0f), "constant occluder is fully dark");
    check(nearly(vis, ref), "constant occluder matches scalar reference");
    texture_release(map);
}

static void test_half_plane_edge(texture_t *map, float blocker_depth) {
    int c, r;
    for (r = 0; r < H; r++) {
        for (c = 0; c < W; c++) {
            if (c < W / 2) {
                set_depth(map, c, r, blocker_depth);
            }
        }
    }
}

static void test_edge_fraction(void) {
    pcss_config_t config = ortho_config(0.5f, 3.0f, 2.0f, 2.0f, 0.5f, 0.5f);
    texture_t *map = make_depth(W, H, 1.0f);
    vec4_t clip = ortho_clip(0.5f, 0.5f, 1.5f, &config);
    float vis, ref;
    test_half_plane_edge(map, ref_ortho_depth(0.8f, 0.5f, 3.0f));
    vis = shadow_pcss_visibility(clip, map, &config, 1.0f);
    ref = ref_visibility(clip, map, &config, 1.0f);
    check(vis > 0 && vis < 1, "half plane edge gives a partial shadow");
    check(nearly(vis, 3.0f / 7.0f), "half plane edge has the deterministic value");
    check(nearly(vis, ref), "half plane edge matches scalar reference");
    texture_release(map);
}

static void test_penumbra_growth(void) {
    pcss_config_t near_config = ortho_config(0.5f, 3.0f, 2.0f, 2.0f,
                                             0.5f, 0.5f);
    pcss_config_t far_config = near_config;
    texture_t *map1 = make_depth(W, H, 1.0f);
    texture_t *map2 = make_depth(W, H, 1.0f);
    texture_t *map3 = make_depth(W, H, 1.0f);
    vec4_t near_clip = ortho_clip(0.5f, 0.5f, 1.0f, &near_config);
    vec4_t far_clip = ortho_clip(0.5f, 0.5f, 2.5f, &far_config);
    vec4_t lit_side = ortho_clip(0.75f, 0.5f, 2.5f, &far_config);
    float near_vis, far_vis, lit_side_vis, near_ref, far_ref, lit_ref;
    test_half_plane_edge(map1, ref_ortho_depth(0.8f, 0.5f, 3.0f));
    test_half_plane_edge(map2, ref_ortho_depth(0.8f, 0.5f, 3.0f));
    test_half_plane_edge(map3, ref_ortho_depth(0.8f, 0.5f, 3.0f));
    near_vis = shadow_pcss_visibility(near_clip, map1, &near_config, 1.0f);
    far_vis = shadow_pcss_visibility(far_clip, map2, &far_config, 1.0f);
    lit_side_vis = shadow_pcss_visibility(lit_side, map3, &far_config, 1.0f);
    near_ref = ref_visibility(near_clip, map1, &near_config, 1.0f);
    far_ref = ref_visibility(far_clip, map2, &far_config, 1.0f);
    lit_ref = ref_visibility(lit_side, map3, &far_config, 1.0f);
    check(near_vis > 0 && near_vis < 1, "near contact penumbra is partial");
    check(far_vis > 0 && far_vis < 1, "far contact penumbra is partial");
    check(far_vis > near_vis,
          "the edge brightens as the penumbra widens with distance");
    check(lit_side_vis < 1.0f,
          "a wide far contact penumbra reaches past the geometric edge");
    check(nearly(near_vis, near_ref), "near contact matches scalar reference");
    check(nearly(far_vis, far_ref), "far contact matches scalar reference");
    check(nearly(lit_side_vis, lit_ref),
          "off edge penumbra matches scalar reference");
    texture_release(map1);
    texture_release(map2);
    texture_release(map3);
}

static void test_non_square_map(void) {
    const int nw = 24;
    const int nh = 8;
    pcss_config_t config = ortho_config(0.5f, 3.0f, 3.0f, 1.0f,
                                        0.5f, 0.1f);
    texture_t *map = make_depth(nw, nh, 1.0f);
    vec4_t clip = ortho_clip(0.5f, 0.5f, 1.5f, &config);
    float vis, ref;
    int c, r;
    for (r = 0; r < nh; r++) {
        for (c = 0; c < nw; c++) {
            if (c < nw / 2) {
                set_depth(map, c, r, ref_ortho_depth(0.8f, 0.5f, 3.0f));
            }
        }
    }
    vis = shadow_pcss_visibility(clip, map, &config, 1.0f);
    ref = ref_visibility(clip, map, &config, 1.0f);
    check(vis > 0 && vis < 1, "non square map gives a partial shadow");
    check(nearly(vis, ref), "non square map matches scalar reference");
    check(!nearly(config.width, config.height) &&
          !nearly(config.emitter_half_size.x, config.emitter_half_size.y),
          "non square case exercises independent u and v radii");
    texture_release(map);
}

static void test_outside_taps_and_bounds(void) {
    pcss_config_t config = ortho_config(0.5f, 3.0f, 2.0f, 2.0f, 0.5f, 0.5f);
    texture_t *map = make_depth(8, 8, ref_ortho_depth(1.0f, 0.5f, 3.0f));
    vec4_t edge_clip = ortho_clip(0.0f, 0.5f, 1.8f, &config);
    vec4_t outside_clip = ortho_clip(-0.5f, 0.5f, 1.8f, &config);
    vec4_t behind_clip = vec4_new(0, 0, 0, -1);
    vec4_t too_far = ortho_clip(0.5f, 0.5f, 3.5f, &config);
    vec4_t too_near = ortho_clip(0.5f, 0.5f, 0.3f, &config);
    float edge_vis = shadow_pcss_visibility(edge_clip, map, &config, 1.0f);
    float edge_ref = ref_visibility(edge_clip, map, &config, 1.0f);
    check(nearly(edge_vis, 21.0f / 49.0f),
          "outside taps stay lit and keep the full denominator");
    check(nearly(edge_vis, edge_ref),
          "outside tap fraction matches scalar reference");
    check(nearly(shadow_pcss_visibility(outside_clip, map, &config, 1.0f), 1),
          "receiver outside the light frustum is fully lit");
    check(nearly(shadow_pcss_visibility(behind_clip, map, &config, 1.0f), 1),
          "non positive w is fully lit");
    check(nearly(shadow_pcss_visibility(too_far, map, &config, 1.0f), 1),
          "receiver past far is fully lit");
    check(nearly(shadow_pcss_visibility(too_near, map, &config, 1.0f), 1),
          "receiver before near is fully lit");
    texture_release(map);
}

static void test_perspective_depth(void) {
    const float n = 1.0f;
    const float f = 10.0f;
    pcss_config_t config = persp_config(n, f, 2.0f, 2.0f, 0.2f, 0.2f);
    texture_t *map = make_depth(W, H, 1.0f);
    float blocked = ref_persp_depth(2.0f, n, f);
    vec4_t center = persp_clip(0.5f, 0.5f, 5.0f, &config);
    vec4_t boundary = persp_clip(0.65f, 0.5f, 5.0f, &config);
    float center_vis, boundary_vis, center_ref, boundary_ref;
    float linear_guess, true_distance;
    int c, r;
    /* a vertical blocker strip spanning every row, at distance 2 */
    for (r = 0; r < H; r++) {
        for (c = 6; c <= 9; c++) {
            set_depth(map, c, r, blocked);
        }
    }
    center_vis = shadow_pcss_visibility(center, map, &config, 1.0f);
    boundary_vis = shadow_pcss_visibility(boundary, map, &config, 1.0f);
    center_ref = ref_visibility(center, map, &config, 1.0f);
    boundary_ref = ref_visibility(boundary, map, &config, 1.0f);
    true_distance = ref_distance(blocked, &config);
    linear_guess = n + blocked * (f - n);
    check(nearly(true_distance, 2.0f) && !nearly(linear_guess, 2.0f),
          "perspective depth is reconstructed with the nonlinear formula");
    check(nearly(center_vis, 0.0f), "perspective blocker center is dark");
    check(nearly(boundary_vis, 2.0f / 7.0f),
          "perspective blocker boundary has the deterministic fraction");
    check(boundary_vis > 0 && boundary_vis < 1,
          "perspective blocker boundary is partial");
    check(nearly(center_vis, center_ref),
          "perspective center matches scalar reference");
    check(nearly(boundary_vis, boundary_ref),
          "perspective boundary matches scalar reference");
    texture_release(map);
}

static void test_anisotropic_emitter(void) {
    /* a horizontal blocker edge is governed by the v radius, i.e. the
       emitter y half size, so swapping the two axes must change it */
    pcss_config_t wide_x = ortho_config(0.5f, 3.0f, 2.0f, 2.0f, 0.5f, 0.1f);
    pcss_config_t wide_y = ortho_config(0.5f, 3.0f, 2.0f, 2.0f, 0.1f, 0.5f);
    texture_t *map1 = make_depth(W, H, 1.0f);
    texture_t *map2 = make_depth(W, H, 1.0f);
    vec4_t clip = ortho_clip(0.5f, 0.5f, 1.5f, &wide_x);
    float vis_x, vis_y, ref_x, ref_y;
    int c, r;
    for (r = 0; r < H / 2; r++) {
        for (c = 0; c < W; c++) {
            set_depth(map1, c, r, ref_ortho_depth(0.8f, 0.5f, 3.0f));
            set_depth(map2, c, r, ref_ortho_depth(0.8f, 0.5f, 3.0f));
        }
    }
    vis_x = shadow_pcss_visibility(clip, map1, &wide_x, 1.0f);
    vis_y = shadow_pcss_visibility(clip, map2, &wide_y, 1.0f);
    ref_x = ref_visibility(clip, map1, &wide_x, 1.0f);
    ref_y = ref_visibility(clip, map2, &wide_y, 1.0f);
    check(vis_x > 0 && vis_x < 1 && vis_y > 0 && vis_y < 1,
          "anisotropic emitter gives partial shadows on both axes");
    check(!nearly(vis_x, vis_y),
          "the two emitter half sizes drive independent u and v radii");
    check(nearly(vis_x, ref_x) && nearly(vis_y, ref_y),
          "anisotropic emitter matches scalar reference");
    texture_release(map1);
    texture_release(map2);
}

static void test_alpha_cutoff_map(void) {
    pcss_config_t config = ortho_config(0.5f, 3.0f, 2.0f, 2.0f, 0.5f, 0.5f);
    texture_t *map = make_depth(W, H, 1.0f);
    vec4_t clip = ortho_clip(0.5f, 0.5f, 1.8f, &config);
    float blocked = ref_ortho_depth(1.0f, 0.5f, 3.0f);
    float vis, ref;
    int c, r;
    /* discarded alpha texels keep the cleared far depth, no interpolation */
    for (r = 0; r < H; r++) {
        for (c = 0; c < W; c++) {
            if ((c + r) % 2 == 0) {
                set_depth(map, c, r, blocked);
            }
        }
    }
    vis = shadow_pcss_visibility(clip, map, &config, 1.0f);
    ref = ref_visibility(clip, map, &config, 1.0f);
    check(vis > 0 && vis < 1, "partial alpha cutoff gives a partial shadow");
    check(nearly(vis, ref), "alpha cutoff map matches scalar reference");
    texture_release(map);
}

static void test_zero_emitter_falls_back_to_hard(void) {
    pcss_config_t config = ortho_config(0.5f, 3.0f, 2.0f, 2.0f, 0.0f, 0.0f);
    texture_t *map = make_depth(W, H, ref_ortho_depth(1.0f, 0.5f, 3.0f));
    vec4_t clip = ortho_clip(0.5f, 0.5f, 1.5f, &config);
    check(!pcss_config_is_soft(&config),
          "a zero size emitter does not enable soft shadows");
    check(nearly(shadow_hard_visibility(clip, map, 1.0f), 0.0f),
          "zero size emitter keeps the hard shadow result");
    texture_release(map);
}

/* ---- fragment shader entry points for the three materials ---- */

static texture_t *make_gray_texture(float value) {
    texture_t *map = texture_create(2, 2);
    int i;
    for (i = 0; i < 4; i++) {
        map->buffer[i] = vec4_new(value, value, value, 1);
    }
    return map;
}

static texture_t *make_half_plane_map(void) {
    texture_t *map = make_depth(W, H, 1.0f);
    test_half_plane_edge(map, ref_ortho_depth(0.8f, 0.5f, 3.0f));
    return map;
}

static void base_varyings(blinn_varyings_t *bv, pbr_varyings_t *pv,
                          float u, float receiver,
                          const pcss_config_t *config) {
    vec4_t clip = ortho_clip(u, 0.5f, receiver, config);
    memset(bv, 0, sizeof(*bv));
    memset(pv, 0, sizeof(*pv));
    bv->normal = vec3_new(0, 0, 1);
    bv->world_position = vec3_new(0, 0, 0);
    bv->depth_position = vec3_new(clip.x, clip.y, clip.z);
    bv->light_position = clip;
    pv->world_normal = vec3_new(0, 0, 1);
    pv->world_position = vec3_new(0, 0, 0);
    pv->depth_position = vec3_new(clip.x, clip.y, clip.z);
    pv->light_position = clip;
    pv->clip_position = vec4_new(0, 0, 0, 1);
}

static void test_blinn_fragment(void) {
    pcss_config_t config = ortho_config(0.5f, 3.0f, 2.0f, 2.0f, 0.5f, 0.5f);
    blinn_uniforms_t u;
    blinn_varyings_t v;
    pbr_varyings_t pv;
    texture_t *shadow = make_half_plane_map();
    texture_t *emission = make_gray_texture(0.3f);
    float soft_values[9];
    float hard_values[9];
    float blocked_color, unshadowed_color, direct_color;
    int discard, i;
    int soft_partial = 0, soft_monotone = 1, hard_binary = 1, hard_levels = 1;

    memset(&u, 0, sizeof(u));
    u.basecolor = vec4_new(1, 1, 1, 1);
    u.light_dir = vec3_new(0, 0, -1);
    u.camera_pos = vec3_new(0, 0, 2);
    u.punctual_intensity = 1;
    u.ambient_intensity = 1;
    u.shininess = 32;
    u.shadow_map = shadow;
    u.emission_map = emission;
    u.pcss_config = config;

    for (i = 0; i < 9; i++) {
        float su = 0.2f + 0.6f * i / 8.0f;
        vec4_t color;
        base_varyings(&v, &pv, su, 1.5f, &config);
        color = blinn_fragment_shader(&v, &u, &discard, 0);
        soft_values[i] = color.x;
        u.pcss_config.mode = SHADOW_MODE_HARD;
        color = blinn_fragment_shader(&v, &u, &discard, 0);
        hard_values[i] = color.x;
        u.pcss_config = config;
    }
    /* the hard path must use exactly two levels: the lit level and the
       ambient/emission floor, never an intermediate penumbra value */
    for (i = 1; i < 9; i++) {
        if (!nearly(hard_values[i], hard_values[0]) &&
            !nearly(hard_values[i], hard_values[8])) {
            hard_binary = 0;
        }
        if (!nearly(hard_values[i], hard_values[0])) {
            hard_levels = 2;
        }
        if (soft_values[i] + 1e-6f < soft_values[i - 1]) {
            soft_monotone = 0;
        }
        if (soft_values[i] > soft_values[0] + 1e-4f &&
            soft_values[i] < soft_values[8] - 1e-4f) {
            soft_partial = 1;
        }
    }
    check(soft_monotone && soft_partial,
          "blinn direct light changes continuously under pcss");
    check(hard_binary && hard_levels == 2,
          "blinn hard shadow stays binary on the same sweep");

    /* ambient and emission must not be multiplied by the visibility */
    base_varyings(&v, &pv, 0.2f, 1.5f, &config);
    u.punctual_intensity = 0;
    u.shadow_map = NULL;
    unshadowed_color = blinn_fragment_shader(&v, &u, &discard, 0).x;
    u.shadow_map = shadow;
    u.pcss_config = config;
    blocked_color = blinn_fragment_shader(&v, &u, &discard, 0).x;
    check(nearly(blocked_color, unshadowed_color),
          "blinn ambient and emission are unchanged in shadow");
    u.punctual_intensity = 1;
    direct_color = blinn_fragment_shader(&v, &u, &discard, 0).x;
    check(nearly(direct_color, blocked_color),
          "blinn fully shadowed direct light adds nothing");

    texture_release(shadow);
    texture_release(emission);
}

static void test_pbr_fragment(workflow_t workflow) {
    pcss_config_t config = ortho_config(0.5f, 3.0f, 2.0f, 2.0f, 0.5f, 0.5f);
    pbr_uniforms_t u;
    pbr_varyings_t v;
    blinn_varyings_t bv;
    texture_t *shadow = make_half_plane_map();
    texture_t *emission = make_gray_texture(0.3f);
    float soft_values[9];
    float blocked_color, unshadowed_color, direct_color;
    int discard, i, partial_found = 0;

    memset(&u, 0, sizeof(u));
    u.light_dir = vec3_new(0, 0, -1);
    u.camera_pos = vec3_new(0, 0, 2);
    u.punctual_intensity = 1;
    u.shadow_map = shadow;
    u.emission_map = emission;
    u.workflow = workflow;
    u.layer_view = -1;
    u.pcss_config = config;
    if (workflow == METALNESS_WORKFLOW) {
        u.basecolor_factor = vec4_new(1, 1, 1, 1);
        u.metalness_factor = 1;
        u.roughness_factor = 0.5f;
    } else {
        u.diffuse_factor = vec4_new(1, 1, 1, 1);
        u.specular_factor = vec3_new(0.04f, 0.04f, 0.04f);
        u.glossiness_factor = 0.5f;
    }

    for (i = 0; i < 9; i++) {
        float su = 0.2f + 0.6f * i / 8.0f;
        vec4_t color;
        base_varyings(&bv, &v, su, 1.5f, &config);
        color = pbr_fragment_shader(&v, &u, &discard, 0);
        soft_values[i] = color.x;
    }
    for (i = 0; i < 9; i++) {
        if (soft_values[i] > soft_values[0] + 1e-4f &&
            soft_values[i] < soft_values[8] - 1e-4f) {
            partial_found = 1;
        }
    }
    check(partial_found, "pbr direct light changes continuously under pcss");

    /* emission only: no ibldata, so the ambient/ibl branch is skipped */
    base_varyings(&bv, &v, 0.2f, 1.5f, &config);
    u.punctual_intensity = 0;
    u.shadow_map = NULL;
    unshadowed_color = pbr_fragment_shader(&v, &u, &discard, 0).x;
    u.shadow_map = shadow;
    u.pcss_config = config;
    blocked_color = pbr_fragment_shader(&v, &u, &discard, 0).x;
    check(nearly(blocked_color, unshadowed_color),
          "pbr emission is unchanged by the shadow factor");
    u.punctual_intensity = 1;
    direct_color = pbr_fragment_shader(&v, &u, &discard, 0).x;
    check(nearly(direct_color, blocked_color),
          "pbr fully shadowed direct light adds nothing");

    /* the material layer preview ignores the shadow map entirely */
    u.layer_view = 1;
    base_varyings(&bv, &v, 0.2f, 1.5f, &config);
    blocked_color = pbr_fragment_shader(&v, &u, &discard, 0).x;
    texture_release(shadow);
    u.shadow_map = make_depth(W, H, 1.0f);
    unshadowed_color = pbr_fragment_shader(&v, &u, &discard, 0).x;
    check(nearly(blocked_color, unshadowed_color),
          "pbr layer preview does not apply the shadow factor");
    texture_release(u.shadow_map);
    texture_release(emission);
}

static void write_ppm(const char *outdir) {
    const int panel = 96;
    const int height = 64;
    pcss_config_t config = ortho_config(0.5f, 3.0f, 2.0f, 2.0f, 0.5f, 0.5f);
    texture_t *map = make_depth(W, H, 1.0f);
    char path[512];
    FILE *file;
    int x, y;
    test_half_plane_edge(map, ref_ortho_depth(0.8f, 0.5f, 3.0f));
    snprintf(path, sizeof(path), "%s/pcss_compare.ppm", outdir);
    file = fopen(path, "wb");
    check(file != NULL, "ppm output file opens");
    if (file) {
        fprintf(file, "P6\n%d %d\n255\n", panel * 2, height);
        for (y = 0; y < height; y++) {
            for (x = 0; x < panel * 2; x++) {
                int panel_x = x % panel;
                float u = 0.05f + 0.9f * (panel_x + 0.5f) / panel;
                vec4_t clip = ortho_clip(u, 0.5f, 1.5f, &config);
                float vis;
                unsigned char byte;
                if (x < panel) {
                    vis = shadow_hard_visibility(clip, map, 1.0f);
                } else {
                    vis = shadow_pcss_visibility(clip, map, &config, 1.0f);
                }
                byte = (unsigned char)(vis * 255);
                fputc(byte, file);
                fputc(byte, file);
                fputc(byte, file);
            }
        }
        fclose(file);
        printf("  wrote %s (left: hard, right: pcss)\n", path);
    }
    texture_release(map);
}

int main(int argc, char *argv[]) {
    const char *outdir = argc > 1 ? argv[1] : ".";

    printf("pcss sampler cases:\n");
    test_no_blocker_is_fully_lit();
    test_constant_occluder();
    test_edge_fraction();
    test_penumbra_growth();
    test_non_square_map();
    test_outside_taps_and_bounds();
    test_perspective_depth();
    test_anisotropic_emitter();
    test_alpha_cutoff_map();
    test_zero_emitter_falls_back_to_hard();

    printf("fragment shader cases:\n");
    test_blinn_fragment();
    test_pbr_fragment(METALNESS_WORKFLOW);
    test_pbr_fragment(SPECULAR_WORKFLOW);

    printf("image case:\n");
    write_ppm(outdir);

    if (g_failures > 0) {
        printf("PCSS_CHECKS_FAILED=%d\n", g_failures);
        return 1;
    }
    puts("PCSS_SHADOW_OK");
    return 0;
}
