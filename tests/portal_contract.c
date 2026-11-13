#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "contract.h"
#include "renderer/core/api.h"
#include "renderer/core/portal.h"
#include "renderer/shaders/blinn_shader.h"
#include "renderer/shaders/pbr_shader.h"
#include "renderer/shaders/cache_helper.h"
#include "renderer/tests/test_helper.h"

/* ================================================================== */
/* shared test infrastructure                                          */
/* ================================================================== */

#define WIDTH 64
#define HEIGHT 64
#define CENTER (HEIGHT / 2 * WIDTH + WIDTH / 2)
#ifndef INFINITY
#define INFINITY (1.0f / 0.0f)
#endif

typedef int (*portal_check_fn)(PortalCheck *);

static int color_close(const unsigned char *px, int r, int g, int b,
                       int tolerance) {
    return abs((int)px[0] - r) <= tolerance &&
           abs((int)px[1] - g) <= tolerance &&
           abs((int)px[2] - b) <= tolerance;
}

static perframe_t make_frame(vec3_t eye, mat4_t proj) {
    perframe_t p;
    memset(&p, 0, sizeof p);
    p.camera_view_matrix = mat4_translate(-eye.x, -eye.y, -eye.z);
    p.camera_proj_matrix = proj;
    p.light_view_matrix = mat4_identity();
    p.light_proj_matrix = mat4_identity();
    p.light_dir = vec3_new(0, 0, -1);
    p.camera_pos = eye;
    p.ambient_intensity = 1;
    p.punctual_intensity = 1;
    p.shadow_map = NULL;
    p.layer_view = -1;
    return p;
}

static mat4_t symmetric_proj(void) {
    return mat4_perspective(1.5707963f, 1, 0.5f, 30);
}

/* asymmetric frustum: shifted optical axis, still ordinary perspective */
static mat4_t asymmetric_proj(void) {
    return mat4_frustum(-0.6f, 1.1f, -0.8f, 0.9f, 0.5f, 30);
}

static model_t *blinn_triangle(mat4_t transform, vec4_t color,
                               float alpha_cutoff, int blend) {
    blinn_material_t m;
    memset(&m, 0, sizeof m);
    m.basecolor = color;
    m.shininess = 32;
    m.double_sided = 1;
    m.enable_blend = blend;
    m.alpha_cutoff = alpha_cutoff;
    return blinn_create_model("checks/triangle.obj", transform, NULL, -1, &m);
}

static model_t *pbrm_triangle(mat4_t transform, vec4_t color) {
    pbrm_material_t m;
    memset(&m, 0, sizeof m);
    m.basecolor_factor = color;
    m.metalness_factor = 0;
    m.roughness_factor = 0.6f;
    m.double_sided = 1;
    return pbrm_create_model("checks/triangle.obj", transform, NULL, -1, &m,
                             NULL);
}

static model_t *pbrs_triangle(mat4_t transform, vec3_t diffuse,
                              vec3_t specular, float glossiness) {
    pbrs_material_t m;
    memset(&m, 0, sizeof m);
    m.diffuse_factor = vec4_from_vec3(diffuse, 1);
    m.specular_factor = specular;
    m.glossiness_factor = glossiness;
    m.double_sided = 1;
    return pbrs_create_model("checks/triangle.obj", transform, NULL, -1, &m,
                             NULL);
}

static scene_t *make_scene(model_t **models) {
    return scene_create(vec3_new(0.04f, 0.04f, 0.06f), NULL, models, 1, 1,
                        0, 0);
}

static portal_t make_portal(mat4_t local_to_world, float hx, float hy,
                            int linked, vec4_t fallback) {
    portal_t portal;
    portal.local_to_world = local_to_world;
    portal.half_size = vec2_new(hx, hy);
    portal.linked = linked;
    portal.fallback = fallback;
    return portal;
}

static portal_t facing_door(vec3_t position) {
    mat4_t l2w = mat4_mul_mat4(
        mat4_translate(position.x, position.y, position.z),
        mat4_rotate_y(PI));
    return make_portal(l2w, 1.2f, 1.2f, -1, vec4_new(0, 0, 0, 1));
}

/* ------------------------------------------------------------------ */
/* independent ray / plane math used to verify the renderer            */
/* ------------------------------------------------------------------ */

static vec4_t project_world(mat4_t view_proj, vec3_t world) {
    return mat4_mul_vec4(view_proj, vec4_from_vec3(world, 1));
}

static void world_to_pixel(mat4_t view_proj, vec3_t world, int width,
                           int height, float *pixel_x, float *pixel_y,
                           float *depth) {
    vec4_t clip = project_world(view_proj, world);
    float ndc_x = clip.x / clip.w;
    float ndc_y = clip.y / clip.w;
    float ndc_z = clip.z / clip.w;
    *pixel_x = (ndc_x * 0.5f + 0.5f) * (float)width;
    *pixel_y = (1 - (ndc_y * 0.5f + 0.5f)) * (float)height;
    *depth = (ndc_z + 1) * 0.5f;
}

static vec3_t unproject(mat4_t view_proj, float ndc_x, float ndc_y,
                        float ndc_z) {
    mat4_t inverse = mat4_inverse(view_proj);
    vec4_t clip = vec4_new(ndc_x, ndc_y, ndc_z, 1);
    vec4_t world = mat4_mul_vec4(inverse, clip);
    return vec3_new(world.x / world.w, world.y / world.w, world.z / world.w);
}

static vec3_t pixel_ray(const perframe_t *frame, int width, int height,
                        int px, int py) {
    float ndc_x = ((float)px + 0.5f) / (float)width * 2 - 1;
    float ndc_y = 1 - (((float)py + 0.5f) / (float)height * 2);
    vec3_t near_point = unproject(
        mat4_mul_mat4(frame->camera_proj_matrix, frame->camera_view_matrix),
        ndc_x, ndc_y, 0);
    return vec3_normalize(vec3_sub(near_point, frame->camera_pos));
}

static vec3_t ray_plane_hit(vec3_t eye, vec3_t dir, vec3_t normal,
                            vec3_t point) {
    float t = vec3_dot(vec3_sub(point, eye), normal) /
              vec3_dot(dir, normal);
    return vec3_add(eye, vec3_mul(dir, t));
}

static vec3_t world_to_local(mat4_t local_to_world, vec3_t world) {
    vec4_t local = mat4_mul_vec4(mat4_inverse(local_to_world),
                                 vec4_from_vec3(world, 1));
    return vec3_from_vec4(local);
}

/*
 * analytic depth buffer value at a pixel center for a world plane,
 * derived from the pixel ray / plane intersection rather than the
 * rasterizer, so it can verify the renderer independently.
 */
static float plane_pixel_depth(const perframe_t *frame, vec3_t normal,
                               vec3_t point, int px, int py) {
    float ndc_x = ((float)px + 0.5f) / (float)WIDTH * 2 - 1;
    float ndc_y = 1 - (((float)py + 0.5f) / (float)HEIGHT * 2);
    mat4_t vp = mat4_mul_mat4(frame->camera_proj_matrix,
                              frame->camera_view_matrix);
    vec3_t near_point = unproject(vp, ndc_x, ndc_y, 0);
    vec3_t dir = vec3_normalize(vec3_sub(near_point, frame->camera_pos));
    vec3_t hit = ray_plane_hit(frame->camera_pos, dir, normal, point);
    vec4_t clip = project_world(vp, hit);
    return (clip.z / clip.w + 1) * 0.5f;
}

static vec3_t portal_normal_of(const portal_t *portal) {
    return vec3_new(portal->local_to_world.m[0][2],
                    portal->local_to_world.m[1][2],
                    portal->local_to_world.m[2][2]);
}

static vec3_t portal_origin_of(const portal_t *portal) {
    return vec3_new(portal->local_to_world.m[0][3],
                    portal->local_to_world.m[1][3],
                    portal->local_to_world.m[2][3]);
}

static framebuffer_t *new_buffer(void) {
    return framebuffer_create(WIDTH, HEIGHT);
}

static int buffers_match(const framebuffer_t *a, const framebuffer_t *b) {
    int pixels = a->width * a->height;
    return memcmp(a->color_buffer, b->color_buffer, (size_t)pixels * 4) == 0 &&
           memcmp(a->depth_buffer, b->depth_buffer,
                  sizeof(float) * (size_t)pixels) == 0;
}

/* render with a given clip bias and count the green triangle fragments
 * that survive the exit-plane clip inside the doorway */
static int count_green_pixels(scene_t *scene, framebuffer_t *fb,
                              const perframe_t *frame,
                              const portal_t portals[2], float clip_bias) {
    int green = 0, i;
    framebuffer_clear_color(fb, scene->background);
    framebuffer_clear_depth(fb, 1);
    if (scene_draw_portals(scene, fb, frame, portals, 2, 1, clip_bias) != 0) {
        return -1;
    }
    for (i = 0; i < WIDTH * HEIGHT; i++) {
        unsigned char *px = &fb->color_buffer[i * 4];
        if (px[1] > 150 && px[0] < 120 && px[2] < 120) {
            green++;
        }
    }
    return green;
}

static framebuffer_t *clone_buffer(const framebuffer_t *src) {
    framebuffer_t *copy = framebuffer_create(src->width, src->height);
    int pixels = src->width * src->height;
    memcpy(copy->color_buffer, src->color_buffer, (size_t)pixels * 4);
    memcpy(copy->depth_buffer, src->depth_buffer,
           sizeof(float) * (size_t)pixels);
    return copy;
}

/* ================================================================== */
/* group 1: view mapping                                               */
/* ================================================================== */

/*
 * the color seen through the door must match a direct render from the
 * transformed virtual camera, and the parent depth is the door depth.
 */
static int check_virtual_camera_mapping(PortalCheck *c) {
    framebuffer_t *through = new_buffer();
    framebuffer_t *direct = new_buffer();
    model_t **models = NULL;
    scene_t *scene;
    perframe_t frame = make_frame(vec3_new(0, 0, 0), symmetric_proj());
    portal_t portals[2];
    mat4_t vp;
    float sx, sy, door_depth;
    vec3_t ray, normal, origin, hit, local;

    darray_push(models, pbrs_triangle(
        mat4_translate(0, 0, -7),
        vec3_new(0.05f, 0.05f, 0.2f), vec3_new(1, 1, 1), 0.9f));
    scene = make_scene(models);

    portals[0] = make_portal(mat4_translate(0, 0, -3), 1.2f, 1.2f, 1,
                             vec4_new(1, 0, 1, 1));
    portals[1] = facing_door(vec3_new(0, 0, -5));
    portals[1].linked = 0;
    portals[1].fallback = vec4_new(0, 1, 1, 1);

    PORTAL_CHECK(c, scene_draw_portals(scene, through, &frame, portals, 2, 1,
                                       0.01f) == 0);

    /* direct render from the virtual camera: B * Ry(pi) * A^-1 * C */
    {
        mat4_t flip = mat4_rotate_y(PI);
        mat4_t virtual_camera = mat4_mul_mat4(
            portals[1].local_to_world,
            mat4_mul_mat4(flip,
                          mat4_inverse(portals[0].local_to_world)));
        perframe_t virtual_frame = make_frame(
            vec3_new(virtual_camera.m[0][3], virtual_camera.m[1][3],
                     virtual_camera.m[2][3]),
            symmetric_proj());
        PORTAL_CHECK(c, scene_draw_portals(scene, direct, &virtual_frame,
                                           NULL, 0, 0, 0.01f) == 0);
    }

    /* glossy view-dependent shading must agree pixel for pixel */
    PORTAL_CHECK(c, color_close(&through->color_buffer[CENTER * 4],
                                direct->color_buffer[CENTER * 4],
                                direct->color_buffer[CENTER * 4 + 1],
                                direct->color_buffer[CENTER * 4 + 2], 6));

    /* ray / plane verification of the door center and parent depth */
    vp = mat4_mul_mat4(frame.camera_proj_matrix, frame.camera_view_matrix);
    world_to_pixel(vp, portal_origin_of(&portals[0]), WIDTH, HEIGHT,
                   &sx, &sy, &door_depth);
    PORTAL_CHECK(c, fabsf(sx - WIDTH / 2) < 1.5f);
    PORTAL_CHECK(c, fabsf(sy - HEIGHT / 2) < 1.5f);
    normal = portal_normal_of(&portals[0]);
    origin = portal_origin_of(&portals[0]);
    door_depth = plane_pixel_depth(&frame, normal, origin,
                                   WIDTH / 2, HEIGHT / 2);
    PORTAL_CHECK(c, fabsf(through->depth_buffer[CENTER] - door_depth) < 2e-4f);

    ray = pixel_ray(&frame, WIDTH, HEIGHT, WIDTH / 2, HEIGHT / 2);
    hit = ray_plane_hit(frame.camera_pos, ray, normal, origin);
    local = world_to_local(portals[0].local_to_world, hit);
    PORTAL_CHECK(c, fabsf(local.x) < 0.05f && fabsf(local.y) < 0.05f);
    PORTAL_CHECK(c, fabsf(local.z) < 0.05f);

    framebuffer_release(through);
    framebuffer_release(direct);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* a rotated, off-center door under an asymmetric projection */
static int check_rotated_door_asymmetric_frustum(PortalCheck *c) {
    framebuffer_t *fb = new_buffer();
    model_t **models = NULL;
    scene_t *scene;
    perframe_t frame = make_frame(vec3_new(0, 0, 0), asymmetric_proj());
    portal_t portals[2];
    mat4_t a_world, b_world, vp;
    vec3_t normal, origin, ray, hit, local;
    float sx, sy, depth;
    int center_pixel;

    darray_push(models, blinn_triangle(
        mat4_translate(0, 0, -8), vec4_new(0.8f, 0.1f, 0.1f, 1), 0, 0));
    scene = make_scene(models);

    a_world = mat4_mul_mat4(mat4_translate(-0.7f, 0.3f, -3),
                            mat4_rotate_y(TO_RADIANS(30)));
    b_world = mat4_mul_mat4(mat4_translate(-0.7f, 0.3f, -5),
                            mat4_rotate_y(TO_RADIANS(210)));
    portals[0] = make_portal(a_world, 1.1f, 1.4f, 1, vec4_new(1, 0, 1, 1));
    portals[1] = make_portal(b_world, 1.1f, 1.4f, 0, vec4_new(0, 1, 1, 1));

    PORTAL_CHECK(c, scene_draw_portals(scene, fb, &frame, portals, 2, 0,
                                       0.02f) == 0);

    vp = mat4_mul_mat4(frame.camera_proj_matrix, frame.camera_view_matrix);
    world_to_pixel(vp, portal_origin_of(&portals[0]), WIDTH, HEIGHT,
                   &sx, &sy, &depth);
    center_pixel = (int)sy * WIDTH + (int)sx;
    PORTAL_CHECK(c, sx >= 0 && sx < WIDTH && sy >= 0 && sy < HEIGHT);

    /* depth 0 shows the fallback, with the analytic door-plane depth */
    PORTAL_CHECK(c, color_close(&fb->color_buffer[center_pixel * 4],
                                255, 0, 255, 4));
    normal = portal_normal_of(&portals[0]);
    origin = portal_origin_of(&portals[0]);
    depth = plane_pixel_depth(&frame, normal, origin, (int)sx, (int)sy);
    PORTAL_CHECK(c, fabsf(fb->depth_buffer[center_pixel] - depth) < 2e-4f);

    /* independent ray cast must land inside the rotated rectangle */
    ray = pixel_ray(&frame, WIDTH, HEIGHT, (int)sx, (int)sy);
    hit = ray_plane_hit(frame.camera_pos, ray, normal, origin);
    local = world_to_local(a_world, hit);
    PORTAL_CHECK(c, fabsf(local.x) <= 1.1f + 0.05f);
    PORTAL_CHECK(c, fabsf(local.y) <= 1.4f + 0.05f);
    PORTAL_CHECK(c, fabsf(local.z) < 0.05f);

    /* a pixel whose ray misses the rectangle keeps the background */
    {
        int miss_x = WIDTH - 2;
        int miss_y = 2;
        vec3_t miss_ray = pixel_ray(&frame, WIDTH, HEIGHT, miss_x, miss_y);
        vec3_t miss_hit = ray_plane_hit(frame.camera_pos, miss_ray, normal,
                                        origin);
        vec3_t miss_local = world_to_local(a_world, miss_hit);
        int outside = fabsf(miss_local.x) > 1.1f ||
                      fabsf(miss_local.y) > 1.4f;
        PORTAL_CHECK(c, outside);
        PORTAL_CHECK(c, color_close(
            &fb->color_buffer[(miss_y * WIDTH + miss_x) * 4],
            10, 10, 15, 8));
    }

    framebuffer_release(fb);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* the back face of a door is neither drawn nor occluding the scene */
static int check_backface_invisible(PortalCheck *c) {
    framebuffer_t *fb = new_buffer();
    model_t **models = NULL;
    scene_t *scene;
    perframe_t frame = make_frame(vec3_new(0, 0, -6), symmetric_proj());
    portal_t portals[2];

    darray_push(models, blinn_triangle(
        mat4_translate(0, 0, -8), vec4_new(0.9f, 0.1f, 0.1f, 1), 0, 0));
    scene = make_scene(models);

    /* camera sits behind the door (its front normal points +z) */
    portals[0] = make_portal(mat4_translate(0, 0, -3), 1.2f, 1.2f, 1,
                             vec4_new(1, 0, 1, 1));
    portals[1] = facing_door(vec3_new(0, 0, -1));
    portals[1].linked = 0;
    portals[1].fallback = vec4_new(0, 1, 1, 1);

    PORTAL_CHECK(c, scene_draw_portals(scene, fb, &frame, portals, 2, 1,
                                       0.01f) == 0);
    /* the red triangle behind the back face stays directly visible */
    PORTAL_CHECK(c, color_close(&fb->color_buffer[CENTER * 4],
                                229, 26, 26, 30));
    /* no portal depth was written */
    {
        float sx, sy, door_depth;
        mat4_t vp = mat4_mul_mat4(frame.camera_proj_matrix,
                                  frame.camera_view_matrix);
        world_to_pixel(vp, portal_origin_of(&portals[0]), WIDTH, HEIGHT,
                       &sx, &sy, &door_depth);
        PORTAL_CHECK(c, fabsf(fb->depth_buffer[CENTER] - door_depth) > 0.02f);
    }

    framebuffer_release(fb);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* ================================================================== */
/* group 2: clipping and occlusion                                     */
/* ================================================================== */

/* geometry in front of the exit plane must not leak into the doorway */
static int check_exit_plane_clipping(PortalCheck *c) {
    framebuffer_t *fb = new_buffer();
    model_t **models = NULL;
    scene_t *scene;
    perframe_t frame = make_frame(vec3_new(0, 0, 0), symmetric_proj());
    portal_t portals[2];

    /* green triangle between virtual camera and the exit plane */
    darray_push(models, blinn_triangle(
        mat4_translate(0, 0, -4.4f), vec4_new(0.1f, 0.9f, 0.1f, 1), 0, 0));
    /* red triangle safely beyond the exit plane */
    darray_push(models, blinn_triangle(
        mat4_translate(0, 0, -7.5f), vec4_new(0.9f, 0.1f, 0.1f, 1), 0, 0));
    scene = make_scene(models);

    portals[0] = make_portal(mat4_translate(0, 0, -3), 1.2f, 1.2f, 1,
                             vec4_new(1, 0, 1, 1));
    portals[1] = facing_door(vec3_new(0, 0, -5));
    portals[1].linked = 0;
    portals[1].fallback = vec4_new(0, 1, 1, 1);

    PORTAL_CHECK(c, scene_draw_portals(scene, fb, &frame, portals, 2, 1,
                                       0.05f) == 0);
    /* red survives the exit clip, green is removed inside the doorway */
    PORTAL_CHECK(c, color_close(&fb->color_buffer[CENTER * 4],
                                229, 26, 26, 35));
    PORTAL_CHECK(c, fb->color_buffer[CENTER * 4 + 1] < 120);

    framebuffer_release(fb);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* a triangle crossing the exit plane is partially kept, not wholesale */
static int check_crossing_triangle_clip(PortalCheck *c) {
    framebuffer_t *fb = new_buffer();
    model_t **models = NULL;
    scene_t *scene;
    perframe_t frame = make_frame(vec3_new(0, 0, 0), symmetric_proj());
    portal_t portals[2];
    int kept_small, kept_middle, kept_large;

    /* rotate about Y so the triangle spans z while still facing the
     * camera, then center it on the exit plane at z=-5. about half of
     * its area is on each side of the plane. */
    darray_push(models, blinn_triangle(
        mat4_mul_mat4(mat4_translate(0, 0, -5),
                      mat4_mul_mat4(mat4_rotate_y(TO_RADIANS(45)),
                                    mat4_scale(1.1f, 1.1f, 1.1f))),
        vec4_new(0.1f, 0.9f, 0.1f, 1), 0, 0));
    scene = make_scene(models);

    portals[0] = make_portal(mat4_translate(0, 0, -3), 1.2f, 1.2f, 1,
                             vec4_new(1, 0, 1, 1));
    portals[1] = facing_door(vec3_new(0, 0, -5));
    portals[1].linked = 0;
    portals[1].fallback = vec4_new(0, 1, 1, 1);

    kept_small = count_green_pixels(scene, fb, &frame, portals, 0.02f);
    kept_middle = count_green_pixels(scene, fb, &frame, portals, 0.45f);
    kept_large = count_green_pixels(scene, fb, &frame, portals, 0.95f);

    /* a crossing triangle is partially kept, never wholly kept or
     * wholly dropped, and pushing the plane in removes strictly more */
    PORTAL_CHECK(c, kept_small > 3);
    PORTAL_CHECK(c, kept_middle > 0);
    PORTAL_CHECK(c, kept_middle < kept_small);
    PORTAL_CHECK(c, kept_large == 0);

    framebuffer_release(fb);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* an occluder in front of the door wins over the portal content */
static int check_near_occlusion(PortalCheck *c) {
    framebuffer_t *fb = new_buffer();
    model_t **models = NULL;
    scene_t *scene;
    perframe_t frame = make_frame(vec3_new(0, 0, 0), symmetric_proj());
    portal_t portals[2];
    mat4_t vp;
    float door_depth, occluder_depth;

    darray_push(models, blinn_triangle(
        mat4_mul_mat4(mat4_translate(0, 0, -1.6f),
                      mat4_scale(0.35f, 0.35f, 0.35f)),
        vec4_new(1, 1, 0, 1), 0, 0));
    darray_push(models, blinn_triangle(
        mat4_translate(0, 0, -7), vec4_new(0.9f, 0.1f, 0.1f, 1), 0, 0));
    scene = make_scene(models);

    portals[0] = make_portal(mat4_translate(0, 0, -3), 1.2f, 1.2f, 1,
                             vec4_new(1, 0, 1, 1));
    portals[1] = facing_door(vec3_new(0, 0, -5));
    portals[1].linked = 0;
    portals[1].fallback = vec4_new(0, 1, 1, 1);

    PORTAL_CHECK(c, scene_draw_portals(scene, fb, &frame, portals, 2, 1,
                                       0.01f) == 0);

    /* center is covered by the yellow occluder, at its own nearer depth */
    PORTAL_CHECK(c, color_close(&fb->color_buffer[CENTER * 4],
                                255, 255, 0, 8));
    vp = mat4_mul_mat4(frame.camera_proj_matrix, frame.camera_view_matrix);
    world_to_pixel(vp, vec3_new(0, 0, -1.6f), WIDTH, HEIGHT,
                   &(float){0}, &(float){0}, &occluder_depth);
    world_to_pixel(vp, portal_origin_of(&portals[0]), WIDTH, HEIGHT,
                   &(float){0}, &(float){0}, &door_depth);
    PORTAL_CHECK(c, fabsf(fb->depth_buffer[CENTER] - occluder_depth) < 2e-4f);
    PORTAL_CHECK(c, occluder_depth < door_depth);

    framebuffer_release(fb);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* a model behind the door plane cannot cover the portal interior */
static int check_behind_door_loses(PortalCheck *c) {
    framebuffer_t *fb = new_buffer();
    model_t **models = NULL;
    scene_t *scene;
    perframe_t frame = make_frame(vec3_new(0, 0, 0), symmetric_proj());
    portal_t portals[2];

    /* big yellow triangle just behind the door, projecting over it */
    darray_push(models, blinn_triangle(
        mat4_mul_mat4(mat4_translate(0, 0, -3.4f),
                      mat4_scale(2.5f, 2.5f, 2.5f)),
        vec4_new(1, 1, 0, 1), 0, 0));
    scene = make_scene(models);

    portals[0] = make_portal(mat4_translate(0, 0, -3), 1.2f, 1.2f, 1,
                             vec4_new(1, 0, 1, 1));
    portals[1] = facing_door(vec3_new(0, 0, -5));
    portals[1].linked = 0;
    portals[1].fallback = vec4_new(0, 1, 1, 1);

    PORTAL_CHECK(c, scene_draw_portals(scene, fb, &frame, portals, 2, 0,
                                       0.01f) == 0);
    /* the magenta door face wins over the farther yellow triangle */
    PORTAL_CHECK(c, color_close(&fb->color_buffer[CENTER * 4],
                                255, 0, 255, 6));

    framebuffer_release(fb);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* a door partly outside the frustum only writes inside its silhouette */
static int check_door_crosses_screen_edge(PortalCheck *c) {
    framebuffer_t *fb = new_buffer();
    model_t **models = NULL;
    scene_t *scene;
    perframe_t frame = make_frame(vec3_new(0, 0, 0), symmetric_proj());
    portal_t portals[2];
    int covered = 0, untouched = 0, i;

    scene = make_scene(models);
    portals[0] = make_portal(mat4_translate(-2.0f, 0, -3), 1.0f, 1.0f, 1,
                             vec4_new(1, 0, 1, 1));
    portals[1] = make_portal(
        mat4_mul_mat4(mat4_translate(-2.0f, 0, -5), mat4_rotate_y(PI)),
        1.0f, 1.0f, 0, vec4_new(0, 1, 1, 1));

    PORTAL_CHECK(c, scene_draw_portals(scene, fb, &frame, portals, 2, 0,
                                       0.01f) == 0);

    for (i = 0; i < WIDTH * HEIGHT; i++) {
        unsigned char *px = &fb->color_buffer[i * 4];
        if (color_close(px, 255, 0, 255, 6)) {
            covered++;
        }
        if (color_close(px, 10, 10, 15, 8)) {
            untouched++;
        }
    }
    PORTAL_CHECK(c, covered > 20);
    PORTAL_CHECK(c, untouched > WIDTH * HEIGHT / 2);

    /* nothing was written outside the projected silhouette on the right */
    PORTAL_CHECK(c, color_close(
        &fb->color_buffer[(HEIGHT / 2 * WIDTH + WIDTH - 3) * 4],
        10, 10, 15, 8));

    framebuffer_release(fb);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* alpha-cutout fragments are discarded by the normal model path and
 * never write color or depth into the portal interior */
static int check_alpha_cutoff(PortalCheck *c) {
    model_t **cut_models = NULL, **opaque_models = NULL;
    scene_t *cut_scene, *opaque_scene;
    perframe_t frame = make_frame(vec3_new(0, 0, 0), symmetric_proj());
    portal_t portals[2];
    framebuffer_t *cut_fb = new_buffer();
    framebuffer_t *opaque_fb = new_buffer();
    float entry_depth;
    vec3_t normal, origin;

    darray_push(cut_models, blinn_triangle(
        mat4_translate(0, 0, -7), vec4_new(0.9f, 0.1f, 0.1f, 0),
        0.5f, 0));
    darray_push(opaque_models, blinn_triangle(
        mat4_translate(0, 0, -7), vec4_new(0.9f, 0.1f, 0.1f, 1),
        0, 0));
    cut_scene = make_scene(cut_models);
    opaque_scene = make_scene(opaque_models);

    portals[0] = make_portal(mat4_translate(0, 0, -3), 1.2f, 1.2f, 1,
                             vec4_new(1, 0, 1, 1));
    portals[1] = facing_door(vec3_new(0, 0, -5));
    portals[1].linked = 0;
    portals[1].fallback = vec4_new(0, 1, 1, 1);

    PORTAL_CHECK(c, scene_draw_portals(cut_scene, cut_fb, &frame, portals, 2,
                                       1, 0.01f) == 0);
    PORTAL_CHECK(c, scene_draw_portals(opaque_scene, opaque_fb, &frame,
                                       portals, 2, 1, 0.01f) == 0);

    /* the opaque twin is visible through the door, proving the geometry
     * itself is in view; the cutout twin contributes nothing */
    PORTAL_CHECK(c, color_close(&opaque_fb->color_buffer[CENTER * 4],
                                229, 26, 26, 40));
    PORTAL_CHECK(c, !color_close(&cut_fb->color_buffer[CENTER * 4],
                                 229, 26, 26, 60));
    /* discarded fragments leave the scene background, not red */
    PORTAL_CHECK(c, color_close(&cut_fb->color_buffer[CENTER * 4],
                                10, 10, 15, 10));

    /* both buffers carry the entry door depth at the parent; the
     * cutout model's own depth never leaks into the doorway */
    normal = portal_normal_of(&portals[0]);
    origin = portal_origin_of(&portals[0]);
    entry_depth = plane_pixel_depth(&frame, normal, origin,
                                    WIDTH / 2, HEIGHT / 2);
    PORTAL_CHECK(c, fabsf(cut_fb->depth_buffer[CENTER] - entry_depth) < 2e-4f);
    PORTAL_CHECK(c, fabsf(opaque_fb->depth_buffer[CENTER] - entry_depth) <
                  2e-4f);

    framebuffer_release(cut_fb);
    framebuffer_release(opaque_fb);
    scene_release(cut_scene);
    scene_release(opaque_scene);
    cache_cleanup();
    return 0;
}

/* overlapping doors resolve by their real parent-view plane depth; on
 * equal depth the existing <= rule lets the later array door win, never
 * the array order on its own */
static int check_overlapping_door_order(PortalCheck *c) {
    framebuffer_t *fb = new_buffer();
    scene_t *scene = make_scene(NULL);
    perframe_t frame = make_frame(vec3_new(0, 0, 0),
                                  mat4_perspective(1.2f, 1, 0.5f, 40));
    portal_t portals[4];

    /* two independent pairs whose entry doors overlap on screen */
    portals[0] = make_portal(mat4_translate(0, 0, -3), 1.2f, 1.2f, 2,
                             vec4_new(1, 0, 1, 1));                 /* magenta */
    portals[1] = make_portal(mat4_translate(0, 0, -3), 1.2f, 1.2f, 3,
                             vec4_new(0, 1, 1, 1));                 /* cyan */
    portals[2] = make_portal(
        mat4_mul_mat4(mat4_translate(0, 0, -9), mat4_rotate_y(PI)),
        1.2f, 1.2f, 0, vec4_new(0, 1, 0, 1));
    portals[3] = make_portal(
        mat4_mul_mat4(mat4_translate(0, 0, -11), mat4_rotate_y(PI)),
        1.2f, 1.2f, 1, vec4_new(1, 0.5f, 0, 1));

    /* coplanar: later array door (index 1, cyan) wins the depth tie */
    PORTAL_CHECK(c, scene_draw_portals(scene, fb, &frame, portals, 4, 0,
                                       0.01f) == 0);
    PORTAL_CHECK(c, color_close(&fb->color_buffer[CENTER * 4],
                                0, 255, 255, 8));

    /* move the earlier door nearer: real depth must override array order */
    framebuffer_clear_color(fb, scene->background);
    framebuffer_clear_depth(fb, 1);
    portals[0].local_to_world = mat4_translate(0, 0, -2.5f);
    PORTAL_CHECK(c, scene_draw_portals(scene, fb, &frame, portals, 4, 0,
                                       0.01f) == 0);
    PORTAL_CHECK(c, color_close(&fb->color_buffer[CENTER * 4],
                                255, 0, 255, 8));

    framebuffer_release(fb);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* ================================================================== */
/* group 3: recursion and state                                        */
/* ================================================================== */

/*
 * a corridor of four facing pairs. only the entry door of each pair is
 * visible through the previous opening (the exit doors face away from
 * the virtual camera), so each recursion level exposes one more entry
 * door and its fallback color.
 */
static void build_chain(portal_t portals[8]) {
    portals[0] = make_portal(mat4_translate(0, 0, -2), 1.2f, 1.2f, 1,
                             vec4_new(1, 0, 1, 1));            /* magenta */
    portals[1] = make_portal(
        mat4_mul_mat4(mat4_translate(0, 0, -4), mat4_rotate_y(PI)),
        1.2f, 1.2f, 0, vec4_new(0, 1, 1, 1));
    portals[2] = make_portal(mat4_translate(0, 0, -7), 1.2f, 1.2f, 3,
                             vec4_new(1, 1, 0, 1));            /* yellow */
    portals[3] = make_portal(
        mat4_mul_mat4(mat4_translate(0, 0, -9), mat4_rotate_y(PI)),
        1.2f, 1.2f, 2, vec4_new(0, 1, 0, 1));
    portals[4] = make_portal(mat4_translate(0, 0, -12), 1.2f, 1.2f, 5,
                             vec4_new(1, 0.5f, 0, 1));         /* orange */
    portals[5] = make_portal(
        mat4_mul_mat4(mat4_translate(0, 0, -14), mat4_rotate_y(PI)),
        1.2f, 1.2f, 4, vec4_new(0.5f, 0, 0.5f, 1));
    portals[6] = make_portal(mat4_translate(0, 0, -17), 1.2f, 1.2f, 7,
                             vec4_new(0, 0, 1, 1));            /* blue */
    portals[7] = make_portal(
        mat4_mul_mat4(mat4_translate(0, 0, -19), mat4_rotate_y(PI)),
        1.2f, 1.2f, 6, vec4_new(0, 0.5f, 0.5f, 1));
}

/* max_depth 0..3 each expose one more layer; the parent depth never moves */
static int check_recursion_depth_limits(PortalCheck *c) {
    static const unsigned char expected[4][3] = {
        {255, 0, 255}, {255, 255, 0}, {255, 127, 0}, {0, 0, 255}
    };
    framebuffer_t *fb = new_buffer();
    model_t **models = NULL;
    scene_t *scene;
    perframe_t frame = make_frame(vec3_new(0, 0, 0),
                                  mat4_perspective(1.5707963f, 1, 0.5f, 40));
    portal_t portals[8];
    vec3_t normal, origin;
    float root_door_depth;
    int depth;

    darray_push(models, blinn_triangle(
        mat4_translate(0, 0, -23), vec4_new(0.9f, 0.1f, 0.1f, 1), 0, 0));
    scene = make_scene(models);
    build_chain(portals);

    normal = portal_normal_of(&portals[0]);
    origin = portal_origin_of(&portals[0]);
    root_door_depth = plane_pixel_depth(&frame, normal, origin,
                                        WIDTH / 2, HEIGHT / 2);

    for (depth = 0; depth <= 3; depth++) {
        framebuffer_clear_color(fb, scene->background);
        framebuffer_clear_depth(fb, 1);
        PORTAL_CHECK(c, scene_draw_portals(scene, fb, &frame, portals, 8,
                                           depth, 0.01f) == 0);
        PORTAL_CHECK(c, color_close(
            &fb->color_buffer[CENTER * 4],
            expected[depth][0], expected[depth][1], expected[depth][2], 8));
        /* the parent always keeps the root entry door's depth */
        PORTAL_CHECK(c, fabsf(fb->depth_buffer[CENTER] -
                              root_door_depth) < 2e-4f);
    }

    /* the cap itself (4) is accepted, terminates, and keeps the parent
     * depth at the root door rather than pulling in a remote depth */
    framebuffer_clear_color(fb, scene->background);
    framebuffer_clear_depth(fb, 1);
    PORTAL_CHECK(c, scene_draw_portals(scene, fb, &frame, portals, 8, 4,
                                       0.01f) == 0);
    PORTAL_CHECK(c, fabsf(fb->depth_buffer[CENTER] -
                          root_door_depth) < 2e-4f);

    framebuffer_release(fb);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* a mutually visible pair must terminate by the depth bound rather than
 * a global visited set, and the stabilized result must not depend on how
 * much spare recursion budget remains */
static int check_cycle_termination(PortalCheck *c) {
    framebuffer_t *fb = new_buffer();
    framebuffer_t *prev = new_buffer();
    model_t **models = NULL;
    scene_t *scene;
    perframe_t frame = make_frame(vec3_new(0, 0, 0),
                                  mat4_perspective(1.5707963f, 1, 0.5f, 40));
    portal_t portals[2];
    float root_depth;
    int depth;

    darray_push(models, blinn_triangle(
        mat4_translate(0, 0, -8), vec4_new(0.9f, 0.1f, 0.1f, 1), 0, 0));
    scene = make_scene(models);
    portals[0] = make_portal(mat4_translate(0, 0, -2), 1.2f, 1.2f, 1,
                             vec4_new(1, 0, 1, 1));
    portals[1] = make_portal(
        mat4_mul_mat4(mat4_translate(0, 0, -4), mat4_rotate_y(PI)),
        1.2f, 1.2f, 0, vec4_new(0, 1, 1, 1));

    root_depth = plane_pixel_depth(&frame, portal_normal_of(&portals[0]),
                                   portal_origin_of(&portals[0]),
                                   WIDTH / 2, HEIGHT / 2);

    /* depth 0 stops at the first fallback */
    framebuffer_clear_color(fb, scene->background);
    framebuffer_clear_depth(fb, 1);
    PORTAL_CHECK(c, scene_draw_portals(scene, fb, &frame, portals, 2, 0,
                                       0.01f) == 0);
    PORTAL_CHECK(c, color_close(&fb->color_buffer[CENTER * 4],
                                255, 0, 255, 8));

    /* every deeper budget converges to the same image: pure depth bound,
     * no visited-set suppression of a legitimately re-visible door */
    for (depth = 1; depth <= 4; depth++) {
        framebuffer_clear_color(fb, scene->background);
        framebuffer_clear_depth(fb, 1);
        PORTAL_CHECK(c, scene_draw_portals(scene, fb, &frame, portals, 2,
                                           depth, 0.01f) == 0);
        PORTAL_CHECK(c, fabsf(fb->depth_buffer[CENTER] - root_depth) < 2e-4f);
        if (depth == 1) {
            framebuffer_t *tmp = prev;
            prev = clone_buffer(fb);
            framebuffer_release(tmp);
        } else {
            PORTAL_CHECK(c, buffers_match(fb, prev));
        }
    }

    framebuffer_release(fb);
    framebuffer_release(prev);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* two sibling pairs keep independent child views: the left opening only
 * ever shows left-side geometry and the right opening only right-side
 * geometry, with no cross-contamination between the two sub-views */
static int check_sibling_isolation(PortalCheck *c) {
    framebuffer_t *fb = new_buffer();
    model_t **models = NULL;
    scene_t *scene;
    perframe_t frame = make_frame(vec3_new(0, 0, 0), symmetric_proj());
    portal_t portals[4];
    int left_green = 0, left_blue = 0, right_green = 0, right_blue = 0;
    int i;

    darray_push(models, blinn_triangle(
        mat4_translate(-1.7f, 0, -7), vec4_new(0.1f, 0.8f, 0.1f, 1), 0, 0));
    darray_push(models, blinn_triangle(
        mat4_translate(1.7f, 0, -7), vec4_new(0.1f, 0.2f, 0.9f, 1), 0, 0));
    scene = make_scene(models);

    portals[0] = make_portal(mat4_translate(-1.7f, 0, -3), 0.8f, 1.0f, 1,
                             vec4_new(1, 0, 1, 1));
    portals[1] = make_portal(
        mat4_mul_mat4(mat4_translate(-1.7f, 0, -5), mat4_rotate_y(PI)),
        0.8f, 1.0f, 0, vec4_new(0, 1, 1, 1));
    portals[2] = make_portal(mat4_translate(1.7f, 0, -3), 0.8f, 1.0f, 3,
                             vec4_new(1, 1, 0, 1));
    portals[3] = make_portal(
        mat4_mul_mat4(mat4_translate(1.7f, 0, -5), mat4_rotate_y(PI)),
        0.8f, 1.0f, 2, vec4_new(1, 0.5f, 0, 1));

    PORTAL_CHECK(c, scene_draw_portals(scene, fb, &frame, portals, 4, 1,
                                       0.01f) == 0);

    for (i = 0; i < WIDTH * HEIGHT; i++) {
        unsigned char *px = &fb->color_buffer[i * 4];
        int is_green = px[1] > 120 && px[0] < 120 && px[2] < 120;
        int is_blue = px[2] > 120 && px[0] < 120 && px[1] < 120;
        if (i % WIDTH < WIDTH / 2) {
            left_green += is_green;
            left_blue += is_blue;
        } else {
            right_green += is_green;
            right_blue += is_blue;
        }
    }
    /* each sibling view contains its own model and never the other's */
    PORTAL_CHECK(c, left_green > 10);
    PORTAL_CHECK(c, right_blue > 10);
    PORTAL_CHECK(c, left_blue == 0);
    PORTAL_CHECK(c, right_green == 0);

    framebuffer_release(fb);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* all three material kinds are exercised inside a portal view */
static int check_all_materials_used(PortalCheck *c) {
    framebuffer_t *fb = new_buffer();
    model_t **models = NULL;
    scene_t *scene;
    perframe_t frame = make_frame(vec3_new(0, 0, 0), symmetric_proj());
    portal_t portals[2];
    int red = 0, green = 0, blue = 0, i;

    darray_push(models, blinn_triangle(
        mat4_mul_mat4(mat4_translate(-1.4f, 0, -7),
                      mat4_scale(0.7f, 0.7f, 0.7f)),
        vec4_new(0.9f, 0.05f, 0.05f, 1), 0, 0));
    darray_push(models, pbrm_triangle(
        mat4_translate(0, 0, -7), vec4_new(0.05f, 0.8f, 0.05f, 1)));
    darray_push(models, pbrs_triangle(
        mat4_mul_mat4(mat4_translate(1.4f, 0, -7),
                      mat4_scale(0.7f, 0.7f, 0.7f)),
        vec3_new(0.05f, 0.1f, 0.8f), vec3_new(0.2f, 0.2f, 0.2f), 0.3f));
    scene = make_scene(models);

    portals[0] = make_portal(
        mat4_mul_mat4(mat4_translate(0, 0, -3), mat4_scale(1, 1, 1)),
        2.6f, 1.6f, 1, vec4_new(1, 0, 1, 1));
    portals[1] = facing_door(vec3_new(0, 0, -5));
    portals[1].half_size = vec2_new(2.6f, 1.6f);
    portals[1].linked = 0;
    portals[1].fallback = vec4_new(0, 1, 1, 1);

    PORTAL_CHECK(c, scene_draw_portals(scene, fb, &frame, portals, 2, 1,
                                       0.01f) == 0);
    for (i = 0; i < WIDTH * HEIGHT; i++) {
        unsigned char *px = &fb->color_buffer[i * 4];
        if (px[0] > 120 && px[1] < 90 && px[2] < 90) red++;
        if (px[1] > 120 && px[0] < 120 && px[2] < 120) green++;
        if (px[2] > 120 && px[0] < 120 && px[1] < 120) blue++;
    }
    PORTAL_CHECK(c, red > 5);
    PORTAL_CHECK(c, green > 5);
    PORTAL_CHECK(c, blue > 5);

    framebuffer_release(fb);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* clip planes are closed afterwards and caller state is restored */
static int check_state_restore_and_old_path(PortalCheck *c) {
    framebuffer_t *fb = new_buffer();
    framebuffer_t *ref = new_buffer();
    model_t **models = NULL;
    scene_t *scene, *plain_scene;
    model_t **plain_models = NULL;
    perframe_t frame = make_frame(vec3_new(0, 0, 0), symmetric_proj());
    perframe_t frame_copy;
    portal_t portals[2], portal_copy[2];
    model_t *model_pointers[8];
    int num_models, i;

    darray_push(models, blinn_triangle(
        mat4_translate(0, 0, -7), vec4_new(0.9f, 0.1f, 0.1f, 1), 0, 0));
    scene = make_scene(models);
    portals[0] = make_portal(mat4_translate(0, 0, -3), 1.2f, 1.2f, 1,
                             vec4_new(1, 0, 1, 1));
    portals[1] = facing_door(vec3_new(0, 0, -5));
    portals[1].linked = 0;
    portals[1].fallback = vec4_new(0, 1, 1, 1);

    frame_copy = frame;
    memcpy(portal_copy, portals, sizeof portals);
    num_models = darray_size(scene->models);
    PORTAL_CHECK(c, num_models <= 8);
    for (i = 0; i < num_models; i++) {
        model_pointers[i] = scene->models[i];
    }

    PORTAL_CHECK(c, scene_draw_portals(scene, fb, &frame, portals, 2, 1,
                                       0.01f) == 0);

    /* caller data is untouched */
    PORTAL_CHECK(c, memcmp(&frame, &frame_copy, sizeof frame) == 0);
    PORTAL_CHECK(c, memcmp(portals, portal_copy, sizeof portals) == 0);
    PORTAL_CHECK(c, darray_size(scene->models) == num_models);
    for (i = 0; i < num_models; i++) {
        PORTAL_CHECK(c, scene->models[i] == model_pointers[i]);
    }

    /* the old scene path still works, proving clipping was disabled */
    darray_push(plain_models, blinn_triangle(
        mat4_translate(0, 0, -2), vec4_new(0.1f, 0.8f, 0.1f, 1), 0, 0));
    plain_scene = make_scene(plain_models);
    test_draw_scene(plain_scene, ref, &frame);
    PORTAL_CHECK(c, color_close(&ref->color_buffer[CENTER * 4],
                                51, 255, 51, 8));
    scene_release(plain_scene);

    framebuffer_release(fb);
    framebuffer_release(ref);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* every invalid configuration fails before drawing and keeps dst */
static int check_invalid_arguments(PortalCheck *c) {
    framebuffer_t *fb = new_buffer();
    framebuffer_t *snapshot;
    model_t **models = NULL;
    scene_t *scene;
    perframe_t frame = make_frame(vec3_new(0, 0, 0), symmetric_proj());
    portal_t portals[2];
    int rc;

    darray_push(models, blinn_triangle(
        mat4_translate(0, 0, -7), vec4_new(0.9f, 0.1f, 0.1f, 1), 0, 0));
    scene = make_scene(models);
    portals[0] = make_portal(mat4_translate(0, 0, -3), 1.2f, 1.2f, 1,
                             vec4_new(1, 0, 1, 1));
    portals[1] = facing_door(vec3_new(0, 0, -5));
    portals[1].linked = 0;
    portals[1].fallback = vec4_new(0, 1, 1, 1);

    framebuffer_clear_color(fb, vec4_new(0.3f, 0.4f, 0.5f, 1));
    framebuffer_clear_depth(fb, 0.42f);

    #define EXPECT_FAIL(...)                                                 \
        do {                                                                 \
            snapshot = clone_buffer(fb);                                     \
            rc = (__VA_ARGS__);                                              \
            PORTAL_CHECK(c, rc != 0);                                        \
            PORTAL_CHECK(c, buffers_match(fb, snapshot));                    \
            framebuffer_release(snapshot);                                   \
        } while (0)

    EXPECT_FAIL(scene_draw_portals(scene, fb, &frame, portals, 2, 5, 0.01f));
    EXPECT_FAIL(scene_draw_portals(scene, fb, &frame, portals, 2, 1, -0.1f));
    EXPECT_FAIL(scene_draw_portals(scene, fb, &frame, portals, 2, 1,
                                   INFINITY));
    EXPECT_FAIL(scene_draw_portals(scene, fb, NULL, portals, 2, 1, 0.01f));

    {
        portal_t bad[2];
        memcpy(bad, portals, sizeof bad);
        bad[0].linked = 0;  /* self reference */
        EXPECT_FAIL(scene_draw_portals(scene, fb, &frame, bad, 2, 1, 0.01f));
    }
    {
        portal_t bad[2];
        memcpy(bad, portals, sizeof bad);
        bad[1].half_size = vec2_new(2.0f, 1.2f);  /* pair size mismatch */
        EXPECT_FAIL(scene_draw_portals(scene, fb, &frame, bad, 2, 1, 0.01f));
    }
    {
        portal_t bad[2];
        float nan = 0.0f / 0.0f;
        memcpy(bad, portals, sizeof bad);
        bad[0].fallback = vec4_new(nan, 0, 1, 1);
        EXPECT_FAIL(scene_draw_portals(scene, fb, &frame, bad, 2, 1, 0.01f));
    }
    {
        portal_t bad[2];
        memcpy(bad, portals, sizeof bad);
        bad[0].local_to_world = mat4_scale(2, 1, 1);  /* non-rigid frame */
        EXPECT_FAIL(scene_draw_portals(scene, fb, &frame, bad, 2, 1, 0.01f));
    }
    {
        portal_t bad[2];
        mat4_t mirror = mat4_identity();
        memcpy(bad, portals, sizeof bad);
        mirror.m[0][0] = -1;  /* reflection */
        bad[0].local_to_world = mirror;
        EXPECT_FAIL(scene_draw_portals(scene, fb, &frame, bad, 2, 1, 0.01f));
    }
    {
        perframe_t bad = frame;
        bad.camera_view_matrix = mat4_scale(1, 2, 1);  /* non-rigid view */
        EXPECT_FAIL(scene_draw_portals(scene, fb, &bad, portals, 2, 1,
                                       0.01f));
    }
    {
        perframe_t bad = frame;
        bad.camera_proj_matrix = mat4_orthographic(1, 1, 0.5f, 30);
        EXPECT_FAIL(scene_draw_portals(scene, fb, &bad, portals, 2, 1,
                                       0.01f));
    }
    {
        /* skybox / shadow / blend are unsupported */
        scene_t *sky_scene = make_scene(NULL);
        model_t dummy;
        memset(&dummy, 0, sizeof dummy);
        sky_scene->skybox = &dummy;
        EXPECT_FAIL(scene_draw_portals(sky_scene, fb, &frame, portals, 2, 1,
                                       0.01f));
        free(sky_scene);
    }
    {
        model_t **shadow_models = NULL;
        scene_t *shadow_scene = scene_create(vec3_new(0, 0, 0), NULL,
                                             shadow_models, 1, 1, 16, 16);
        EXPECT_FAIL(scene_draw_portals(shadow_scene, fb, &frame, portals, 2,
                                       1, 0.01f));
        scene_release(shadow_scene);
    }
    {
        model_t **blend_models = NULL;
        scene_t *blend_scene;
        darray_push(blend_models, blinn_triangle(
            mat4_translate(0, 0, -7), vec4_new(0.9f, 0.1f, 0.1f, 0.4f),
            0, 1));
        blend_scene = make_scene(blend_models);
        EXPECT_FAIL(scene_draw_portals(blend_scene, fb, &frame, portals, 2,
                                       1, 0.01f));
        scene_release(blend_scene);
    }

    #undef EXPECT_FAIL

    framebuffer_release(fb);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* two frames and two scenes can be interleaved without state leakage */
static int check_reuse_across_frames_and_scenes(PortalCheck *c) {
    framebuffer_t *fb1 = new_buffer();
    framebuffer_t *fb2 = new_buffer();
    model_t **models_a = NULL, **models_b = NULL;
    scene_t *scene_a, *scene_b;
    perframe_t frame_a = make_frame(vec3_new(0, 0, 0), symmetric_proj());
    perframe_t frame_b = make_frame(vec3_new(0.6f, 0, 0), symmetric_proj());
    portal_t portals_a[2], portals_b[2];

    darray_push(models_a, blinn_triangle(
        mat4_translate(0, 0, -7), vec4_new(0.9f, 0.1f, 0.1f, 1), 0, 0));
    darray_push(models_b, blinn_triangle(
        mat4_translate(0, 0, -7), vec4_new(0.1f, 0.2f, 0.9f, 1), 0, 0));
    scene_a = make_scene(models_a);
    scene_b = make_scene(models_b);

    portals_a[0] = make_portal(mat4_translate(0, 0, -3), 1.2f, 1.2f, 1,
                               vec4_new(1, 0, 1, 1));
    portals_a[1] = facing_door(vec3_new(0, 0, -5));
    portals_a[1].linked = 0;
    portals_a[1].fallback = vec4_new(0, 1, 1, 1);
    portals_b[0] = make_portal(mat4_translate(0, 0, -3), 1.2f, 1.2f, 1,
                               vec4_new(1, 1, 0, 1));
    portals_b[1] = facing_door(vec3_new(0, 0, -5));
    portals_b[1].linked = 0;
    portals_b[1].fallback = vec4_new(0, 1, 0, 1);

    PORTAL_CHECK(c, scene_draw_portals(scene_a, fb1, &frame_a, portals_a, 2,
                                       1, 0.01f) == 0);
    PORTAL_CHECK(c, scene_draw_portals(scene_b, fb2, &frame_b, portals_b, 2,
                                       1, 0.01f) == 0);
    /* interleave again: results must be reproducible and scene-specific */
    {
        framebuffer_t *repeat = new_buffer();
        PORTAL_CHECK(c, scene_draw_portals(scene_a, repeat, &frame_a,
                                           portals_a, 2, 1, 0.01f) == 0);
        PORTAL_CHECK(c, buffers_match(fb1, repeat));
        framebuffer_release(repeat);
    }
    /* frame a centers the red model through its door */
    PORTAL_CHECK(c, color_close(&fb1->color_buffer[CENTER * 4],
                                229, 26, 26, 35));
    /* the shifted-camera frame still renders only scene b's blue model,
     * with no red leaking in from the previously drawn scene */
    {
        int blue = 0, red = 0, i;
        for (i = 0; i < WIDTH * HEIGHT; i++) {
            unsigned char *px = &fb2->color_buffer[i * 4];
            if (px[2] > 120 && px[0] < 120 && px[1] < 120) blue++;
            if (px[0] > 120 && px[1] < 90 && px[2] < 90) red++;
        }
        PORTAL_CHECK(c, blue > 10);
        PORTAL_CHECK(c, red == 0);
    }

    framebuffer_release(fb1);
    framebuffer_release(fb2);
    scene_release(scene_a);
    scene_release(scene_b);
    cache_cleanup();
    return 0;
}

/* count == 0 is a legal plain scene draw */
static int check_zero_portals(PortalCheck *c) {
    framebuffer_t *fb = new_buffer();
    framebuffer_t *ref = new_buffer();
    model_t **models = NULL;
    scene_t *scene;
    perframe_t frame = make_frame(vec3_new(0, 0, 0), symmetric_proj());

    darray_push(models, blinn_triangle(
        mat4_translate(0, 0, -2), vec4_new(0.9f, 0.1f, 0.1f, 1), 0, 0));
    scene = make_scene(models);

    PORTAL_CHECK(c, scene_draw_portals(scene, fb, &frame, NULL, 0, 0,
                                       0.01f) == 0);
    test_draw_scene(scene, ref, &frame);
    PORTAL_CHECK(c, buffers_match(fb, ref));

    framebuffer_release(fb);
    framebuffer_release(ref);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* ================================================================== */
/* registry                                                            */
/* ================================================================== */

const PortalCase *portal_cases(unsigned *count) {
    static const PortalCase cases[] = {
        {"view-mapping", "virtual-camera-mapping",
         check_virtual_camera_mapping},
        {"view-mapping", "rotated-door-asymmetric-frustum",
         check_rotated_door_asymmetric_frustum},
        {"view-mapping", "backface-invisible", check_backface_invisible},

        {"clip-and-occlusion", "exit-plane-clipping",
         check_exit_plane_clipping},
        {"clip-and-occlusion", "crossing-triangle-clip",
         check_crossing_triangle_clip},
        {"clip-and-occlusion", "near-occlusion", check_near_occlusion},
        {"clip-and-occlusion", "behind-door-loses", check_behind_door_loses},
        {"clip-and-occlusion", "door-crosses-screen-edge",
         check_door_crosses_screen_edge},
        {"clip-and-occlusion", "alpha-cutoff", check_alpha_cutoff},
        {"clip-and-occlusion", "overlapping-door-order",
         check_overlapping_door_order},

        {"recursion-and-state", "recursion-depth-limits",
         check_recursion_depth_limits},
        {"recursion-and-state", "cycle-termination",
         check_cycle_termination},
        {"recursion-and-state", "sibling-isolation",
         check_sibling_isolation},
        {"recursion-and-state", "all-materials-used",
         check_all_materials_used},
        {"recursion-and-state", "state-restore-old-path",
         check_state_restore_and_old_path},
        {"recursion-and-state", "invalid-arguments",
         check_invalid_arguments},
        {"recursion-and-state", "reuse-frames-scenes",
         check_reuse_across_frames_and_scenes},
        {"recursion-and-state", "zero-portals", check_zero_portals},
    };
    *count = (unsigned)(sizeof(cases) / sizeof(cases[0]));
    return cases;
}
