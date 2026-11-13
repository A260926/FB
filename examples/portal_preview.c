#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "renderer/core/api.h"
#include "renderer/core/portal.h"
#include "renderer/shaders/blinn_shader.h"
#include "renderer/shaders/cache_helper.h"

/*
 * offline portal preview.
 *
 * usage: portal_preview <output-directory>
 *
 * renders one fixed camera looking down a corridor of linked portal
 * pairs at three recursion depths and writes depth0.ppm, depth1.ppm and
 * depth2.ppm (binary P6) into the output directory. every pixel comes
 * from a real scene_draw_portals() draw; only the recursion budget
 * changes between the three images.
 *
 * coordinate convention:
 *   - the camera looks down -Z from the origin (right handed, -Z forward)
 *   - a portal rectangle lives in its local z=0 plane, spanning
 *     +/- half_size in x and y; +Z is its front face
 *   - paired doors face each other, so an exit door is a translation
 *     followed by a 180 degree rotation about Y
 *
 * lifecycle: each framebuffer is created per draw and released, the
 * scene and its models are released once, and the global resource cache
 * is cleaned up before exit.
 *
 * limitations: only opaque and alpha-cutout models are supported; there
 * is no skybox, no blended/transparent geometry and no shadow pass.
 */

#define IMAGE_WIDTH 128
#define IMAGE_HEIGHT 128

static perframe_t build_frame(void) {
    perframe_t frame;
    memset(&frame, 0, sizeof frame);
    frame.camera_view_matrix = mat4_identity();
    frame.camera_proj_matrix = mat4_perspective(1.2f, 1.0f, 0.5f, 40.0f);
    frame.light_view_matrix = mat4_identity();
    frame.light_proj_matrix = mat4_identity();
    frame.light_dir = vec3_new(0, 0, -1);
    frame.camera_pos = vec3_new(0, 0, 0);
    frame.ambient_intensity = 1;
    frame.punctual_intensity = 1;
    frame.shadow_map = NULL;
    frame.layer_view = -1;
    return frame;
}

static model_t *colored_triangle(mat4_t transform, vec4_t color) {
    blinn_material_t material;
    memset(&material, 0, sizeof material);
    material.basecolor = color;
    material.shininess = 16;
    material.double_sided = 1;
    material.enable_blend = 0;
    material.alpha_cutoff = 0;
    return blinn_create_model("checks/triangle.obj", transform, NULL, -1,
                              &material);
}

static void write_ppm(const char *path, const framebuffer_t *framebuffer) {
    FILE *file = fopen(path, "wb");
    int i;
    if (file == NULL) {
        fprintf(stderr, "cannot open %s for writing\n", path);
        exit(2);
    }
    fprintf(file, "P6\n%d %d\n255\n", framebuffer->width, framebuffer->height);
    for (i = 0; i < framebuffer->width * framebuffer->height; i++) {
        unsigned char rgb[3];
        rgb[0] = framebuffer->color_buffer[i * 4 + 0];
        rgb[1] = framebuffer->color_buffer[i * 4 + 1];
        rgb[2] = framebuffer->color_buffer[i * 4 + 2];
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}

static portal_t make_door(mat4_t local_to_world, int linked,
                          vec4_t fallback) {
    portal_t door;
    door.local_to_world = local_to_world;
    door.half_size = vec2_new(1.3f, 1.3f);
    door.linked = linked;
    door.fallback = fallback;
    return door;
}

int main(int argc, char **argv) {
    const char *output_dir;
    model_t **models = NULL;
    scene_t *scene;
    perframe_t frame;
    portal_t portals[4];
    int depth;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
        return 2;
    }
    output_dir = argv[1];

    /* geometry visible at the far end of the corridor */
    darray_push(models, colored_triangle(
        mat4_translate(0, 0, -12), vec4_new(0.9f, 0.15f, 0.1f, 1)));
    darray_push(models, colored_triangle(
        mat4_mul_mat4(mat4_translate(0, 0, -10),
                      mat4_scale(0.5f, 0.5f, 0.5f)),
        vec4_new(0.1f, 0.4f, 0.95f, 1)));
    scene = scene_create(vec3_new(0.05f, 0.05f, 0.08f), NULL, models, 1, 1,
                         0, 0);
    frame = build_frame();

    /* first linked pair: entry at z=-2, exit at z=-4 facing back */
    portals[0] = make_door(mat4_translate(0, 0, -2), 1,
                           vec4_new(0.85f, 0.1f, 0.7f, 1));
    portals[1] = make_door(
        mat4_mul_mat4(mat4_translate(0, 0, -4), mat4_rotate_y(PI)), 0,
        vec4_new(0.1f, 0.75f, 0.8f, 1));
    /* second linked pair deeper down the corridor */
    portals[2] = make_door(mat4_translate(0, 0, -7), 3,
                           vec4_new(0.95f, 0.8f, 0.1f, 1));
    portals[3] = make_door(
        mat4_mul_mat4(mat4_translate(0, 0, -9), mat4_rotate_y(PI)), 2,
        vec4_new(0.2f, 0.8f, 0.35f, 1));

    for (depth = 0; depth <= 2; depth++) {
        framebuffer_t *framebuffer = framebuffer_create(IMAGE_WIDTH,
                                                        IMAGE_HEIGHT);
        char path[512];
        int written;

        framebuffer_clear_color(framebuffer, scene->background);
        framebuffer_clear_depth(framebuffer, 1);
        if (scene_draw_portals(scene, framebuffer, &frame, portals, 4, depth,
                               0.02f) != 0) {
            fprintf(stderr, "scene_draw_portals failed at depth %d\n", depth);
            framebuffer_release(framebuffer);
            scene_release(scene);
            cache_cleanup();
            return 3;
        }

        written = snprintf(path, sizeof path, "%s/depth%d.ppm",
                           output_dir, depth);
        if (written <= 0 || (size_t)written >= sizeof path) {
            fprintf(stderr, "output path too long\n");
            framebuffer_release(framebuffer);
            scene_release(scene);
            cache_cleanup();
            return 4;
        }
        write_ppm(path, framebuffer);
        printf("wrote %s\n", path);
        framebuffer_release(framebuffer);
    }

    scene_release(scene);
    cache_cleanup();
    return 0;
}
