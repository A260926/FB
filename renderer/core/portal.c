#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "darray.h"
#include "graphics.h"
#include "macro.h"
#include "maths.h"
#include "portal.h"
#include "scene.h"
#include "texture.h"

#define PORTAL_MAX_DEPTH 4
#define RIGID_TOLERANCE 1e-4f
#define FLOAT_ONE_TOLERANCE 1e-5f

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int is_finitef(float value) {
    return value == value && value >= -FLT_MAX && value <= FLT_MAX;
}

static int is_finite_vec4(vec4_t value) {
    return is_finitef(value.x) && is_finitef(value.y) &&
           is_finitef(value.z) && is_finitef(value.w);
}

static int is_finite_mat4(mat4_t value) {
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            if (!is_finitef(value.m[i][j])) {
                return 0;
            }
        }
    }
    return 1;
}

/* Ry(pi): flips local x and z, keeps y. this is a 180 degree rotation,
 * not a mirror reflection. */
static mat4_t rotate_y_pi(void) {
    mat4_t m = mat4_identity();
    m.m[0][0] = -1;
    m.m[2][2] = -1;
    return m;
}

static vec3_t matrix_origin(mat4_t m) {
    return vec3_new(m.m[0][3], m.m[1][3], m.m[2][3]);
}

static vec3_t matrix_column(mat4_t m, int column) {
    return vec3_new(m.m[0][column], m.m[1][column], m.m[2][column]);
}

/* ------------------------------------------------------------------ */
/* validation                                                          */
/* ------------------------------------------------------------------ */

static int is_rigid_matrix(mat4_t m) {
    vec3_t columns[3];
    int i, j;
    if (m.m[3][0] != 0 || m.m[3][1] != 0 ||
        m.m[3][2] != 0 || m.m[3][3] != 1) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        columns[i] = matrix_column(m, i);
        if (fabsf(vec3_length(columns[i]) - 1) > RIGID_TOLERANCE) {
            return 0;
        }
    }
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            float expected = i == j ? 1.0f : 0.0f;
            if (fabsf(vec3_dot(columns[i], columns[j]) - expected) >
                RIGID_TOLERANCE) {
                return 0;
            }
        }
    }
    /* reject reflections: a rigid frame is right handed */
    if (vec3_dot(vec3_cross(columns[0], columns[1]), columns[2]) <
        1 - RIGID_TOLERANCE) {
        return 0;
    }
    return 1;
}

/* ordinary perspective projection only: right handed -Z, w = -view.z,
 * NDC z in [-1, 1], asymmetric frusta are allowed. */
static int is_perspective_projection(mat4_t p) {
    if (!is_finite_mat4(p)) {
        return 0;
    }
    if (p.m[3][0] != 0 || p.m[3][1] != 0 ||
        p.m[3][2] != -1 || p.m[3][3] != 0) {
        return 0;
    }
    /* finite positive near/far give m22 = -(f+n)/(f-n) < -1 and a
     * negative z translation; an infinite-far projection collapses to
     * m22 == -1, which is rejected here */
    if (!(p.m[2][3] < 0) || p.m[2][2] >= -1.0f ||
        p.m[0][0] == 0 || p.m[1][1] == 0) {
        return 0;
    }
    return 1;
}

static int validate_frame(const perframe_t *frame) {
    if (frame == NULL || !is_finite_mat4(frame->camera_view_matrix) ||
        !is_rigid_matrix(frame->camera_view_matrix) ||
        !is_perspective_projection(frame->camera_proj_matrix)) {
        return 0;
    }
    if (!is_finite_mat4(frame->light_view_matrix) ||
        !is_finite_mat4(frame->light_proj_matrix)) {
        return 0;
    }
    if (!is_finitef(frame->camera_pos.x) ||
        !is_finitef(frame->camera_pos.y) ||
        !is_finitef(frame->camera_pos.z)) {
        return 0;
    }
    if (!is_finitef(frame->light_dir.x) ||
        !is_finitef(frame->light_dir.y) ||
        !is_finitef(frame->light_dir.z)) {
        return 0;
    }
    if (!is_finitef(frame->frame_time) ||
        !is_finitef(frame->delta_time) ||
        !is_finitef(frame->ambient_intensity) ||
        !is_finitef(frame->punctual_intensity)) {
        return 0;
    }
    return 1;
}

static int validate_scene(scene_t *scene) {
    int num_models, i;
    if (scene == NULL || scene->skybox != NULL ||
        scene->shadow_buffer != NULL || scene->shadow_map != NULL) {
        return 0;
    }
    num_models = darray_size(scene->models);
    for (i = 0; i < num_models; i++) {
        model_t *model = scene->models[i];
        if (model == NULL || model->mesh == NULL || model->program == NULL ||
            model->update == NULL || model->draw == NULL ||
            model->release == NULL || !model->opaque) {
            return 0;
        }
        if (!is_finite_mat4(model->transform)) {
            return 0;
        }
    }
    return 1;
}

static int validate_portals(const portal_t *portals, int count) {
    int i;
    if (count < 0) {
        return 0;
    }
    if (count == 0) {
        return 1;
    }
    if (portals == NULL) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        const portal_t *portal = &portals[i];
        if (!is_finitef(portal->half_size.x) ||
            !is_finitef(portal->half_size.y) ||
            portal->half_size.x <= 0 || portal->half_size.y <= 0) {
            return 0;
        }
        if (!is_finite_mat4(portal->local_to_world) ||
            !is_rigid_matrix(portal->local_to_world)) {
            return 0;
        }
        if (!is_finite_vec4(portal->fallback) ||
            fabsf(portal->fallback.w - 1) > FLOAT_ONE_TOLERANCE) {
            return 0;
        }
        if (portal->linked < 0 || portal->linked >= count ||
            portal->linked == i) {
            return 0;
        }
    }
    for (i = 0; i < count; i++) {
        const portal_t *portal = &portals[i];
        const portal_t *partner = &portals[portal->linked];
        if (partner->linked != i) {
            return 0;
        }
        if (fabsf(partner->half_size.x - portal->half_size.x) >
                RIGID_TOLERANCE ||
            fabsf(partner->half_size.y - portal->half_size.y) >
                RIGID_TOLERANCE) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* framebuffer snapshot, used to keep dst untouched on failure         */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char *color;
    float *depth;
    int pixels;
} snapshot_t;

static int snapshot_capture(framebuffer_t *framebuffer, snapshot_t *snapshot) {
    int pixels = framebuffer->width * framebuffer->height;
    snapshot->pixels = pixels;
    snapshot->color = (unsigned char*)malloc((size_t)pixels * 4);
    snapshot->depth = (float*)malloc(sizeof(float) * (size_t)pixels);
    if (snapshot->color == NULL || snapshot->depth == NULL) {
        free(snapshot->color);
        free(snapshot->depth);
        return 0;
    }
    memcpy(snapshot->color, framebuffer->color_buffer, (size_t)pixels * 4);
    memcpy(snapshot->depth, framebuffer->depth_buffer,
           sizeof(float) * (size_t)pixels);
    return 1;
}

static void snapshot_restore(framebuffer_t *framebuffer,
                             const snapshot_t *snapshot) {
    memcpy(framebuffer->color_buffer, snapshot->color,
           (size_t)snapshot->pixels * 4);
    memcpy(framebuffer->depth_buffer, snapshot->depth,
           sizeof(float) * (size_t)snapshot->pixels);
}

/* ------------------------------------------------------------------ */
/* portal geometry                                                     */
/* ------------------------------------------------------------------ */

static vec3_t portal_normal(const portal_t *portal) {
    return matrix_column(portal->local_to_world, 2);
}

static vec3_t portal_origin(const portal_t *portal) {
    return matrix_origin(portal->local_to_world);
}

/* signed distance of a point to the portal plane along its front normal */
static float portal_distance(const portal_t *portal, vec3_t point) {
    vec3_t normal = portal_normal(portal);
    return vec3_dot(normal, vec3_sub(point, portal_origin(portal)));
}

static int portal_is_visible(const portal_t *portal, vec3_t camera_pos,
                             float clip_bias) {
    return portal_distance(portal, camera_pos) > clip_bias;
}

/*
 * the camera world matrix for the child view when looking through
 * entry portal a towards its exit portal b:
 *
 *     C' = B.local_to_world * Ry(pi) * inverse(A.local_to_world) * C
 */
static mat4_t child_camera_matrix(mat4_t parent_camera,
                                  const portal_t *entry,
                                  const portal_t *exit_portal) {
    mat4_t flip = rotate_y_pi();
    mat4_t relative = mat4_mul_mat4(
        exit_portal->local_to_world,
        mat4_mul_mat4(flip, mat4_inverse(entry->local_to_world)));
    return mat4_mul_mat4(relative, parent_camera);
}

/*
 * move the exit portal's world plane into the current clip space.
 * planes transform with the inverse transpose of the point matrix.
 * the kept side satisfies n.x + d >= clip_bias in world space, so the
 * homogeneous plane handed to the programs is (n, d - clip_bias).
 */
static vec4_t exit_clip_plane(const perframe_t *frame, mat4_t view_matrix,
                              const portal_t *exit_portal,
                              float clip_bias) {
    vec3_t normal = portal_normal(exit_portal);
    float distance = -vec3_dot(normal, portal_origin(exit_portal));
    vec4_t world_plane = vec4_new(normal.x, normal.y, normal.z,
                                  distance - clip_bias);
    mat4_t vp = mat4_mul_mat4(frame->camera_proj_matrix, view_matrix);
    return mat4_mul_vec4(mat4_inverse_transpose(vp), world_plane);
}

/* ------------------------------------------------------------------ */
/* portal face program                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    vec3_t local_position;
} face_attribs_t;

typedef struct {
    vec2_t screen_uv;
} face_varyings_t;

typedef struct {
    mat4_t view_proj;
    vec4_t fallback;
    texture_t *source;
    vec2_t viewport;
} face_uniforms_t;

static vec4_t face_vertex_shader(void *attribs_, void *varyings_,
                                 void *uniforms_) {
    face_attribs_t *attribs = (face_attribs_t*)attribs_;
    face_varyings_t *varyings = (face_varyings_t*)varyings_;
    face_uniforms_t *uniforms = (face_uniforms_t*)uniforms_;
    vec4_t local = vec4_from_vec3(attribs->local_position, 1);
    /* the face is drawn in the parent portal frame, which the caller
     * bakes into view_proj together with the portal transform */
    vec4_t clip = mat4_mul_vec4(uniforms->view_proj, local);
    float ndc_x = clip.x / clip.w;
    float ndc_y = clip.y / clip.w;
    float u = ndc_x * 0.5f + 0.5f;
    float v = ndc_y * 0.5f + 0.5f;
    /* map pixel centers onto texel centers so the child view is read
     * back at the exact screen pixel it was rendered for */
    if (uniforms->source != NULL && uniforms->viewport.x > 1 &&
        uniforms->viewport.y > 1) {
        u = (u * uniforms->viewport.x - 0.5f) /
            (uniforms->viewport.x - 1);
        v = (v * uniforms->viewport.y - 0.5f) /
            (uniforms->viewport.y - 1);
    }
    varyings->screen_uv = vec2_new(u, v);
    return clip;
}

static vec4_t face_fragment_shader(void *varyings_, void *uniforms_,
                                   int *discard, int backface) {
    face_varyings_t *varyings = (face_varyings_t*)varyings_;
    face_uniforms_t *uniforms = (face_uniforms_t*)uniforms_;
    UNUSED_VAR(discard);
    UNUSED_VAR(backface);
    if (uniforms->source != NULL) {
        return texture_clamp_sample(uniforms->source, varyings->screen_uv);
    }
    return uniforms->fallback;
}

/* ------------------------------------------------------------------ */
/* per-view rendering state                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    scene_t *scene;
    const perframe_t *frame;
    const portal_t *portals;
    int count;
    int max_depth;
    float clip_bias;
    program_t *face_program;
    texture_t *scratch_texture;
} portal_ctx_t;

static void render_models(portal_ctx_t *ctx, framebuffer_t *framebuffer,
                          mat4_t view_matrix, vec3_t camera_pos,
                          const vec4_t *clip_plane) {
    int num_models = darray_size(ctx->scene->models);
    int i;
    for (i = 0; i < num_models; i++) {
        model_t *model = ctx->scene->models[i];
        perframe_t perframe = *ctx->frame;
        perframe.camera_view_matrix = view_matrix;
        perframe.camera_pos = camera_pos;
        model->update(model, &perframe);
        program_set_clip_plane(model->program, clip_plane);
        model->draw(model, framebuffer, 0);
    }
}

static void draw_portal_face(portal_ctx_t *ctx, framebuffer_t *framebuffer,
                             const portal_t *portal, mat4_t view_matrix,
                             const vec4_t *clip_plane, texture_t *source) {
    static const int indices[6] = {0, 1, 2, 0, 2, 3};
    face_uniforms_t *uniforms =
        (face_uniforms_t*)program_get_uniforms(ctx->face_program);
    float half_x = portal->half_size.x;
    float half_y = portal->half_size.y;
    vec3_t corners[4];
    mat4_t view_proj;
    int i;

    corners[0] = vec3_new(-half_x, -half_y, 0);
    corners[1] = vec3_new(+half_x, -half_y, 0);
    corners[2] = vec3_new(+half_x, +half_y, 0);
    corners[3] = vec3_new(-half_x, +half_y, 0);

    view_proj = mat4_mul_mat4(
        mat4_mul_mat4(ctx->frame->camera_proj_matrix, view_matrix),
        portal->local_to_world);

    uniforms->view_proj = view_proj;
    uniforms->fallback = portal->fallback;
    uniforms->source = source;
    uniforms->viewport = vec2_new((float)framebuffer->width,
                                  (float)framebuffer->height);
    program_set_clip_plane(ctx->face_program, clip_plane);

    for (i = 0; i < 6; i += 3) {
        int j;
        for (j = 0; j < 3; j++) {
            face_attribs_t *attribs =
                (face_attribs_t*)program_get_attribs(ctx->face_program, j);
            attribs->local_position = corners[indices[i + j]];
        }
        graphics_draw_triangle(framebuffer, ctx->face_program);
    }
}

/* ------------------------------------------------------------------ */
/* recursive view                                                      */
/* ------------------------------------------------------------------ */

static int render_view(portal_ctx_t *ctx, framebuffer_t *framebuffer,
                       mat4_t view_matrix, vec3_t camera_pos,
                       const portal_t *exit_portal, int depth) {
    vec4_t clip_plane;
    const vec4_t *active_plane = NULL;
    int i;

    if (exit_portal != NULL) {
        clip_plane = exit_clip_plane(ctx->frame, view_matrix, exit_portal,
                                     ctx->clip_bias);
        active_plane = &clip_plane;
    }

    render_models(ctx, framebuffer, view_matrix, camera_pos, active_plane);

    for (i = 0; i < ctx->count; i++) {
        const portal_t *portal = &ctx->portals[i];
        if (!portal_is_visible(portal, camera_pos, ctx->clip_bias)) {
            continue;
        }
        if (depth >= ctx->max_depth) {
            draw_portal_face(ctx, framebuffer, portal, view_matrix,
                             active_plane, NULL);
        } else {
            const portal_t *linked = &ctx->portals[portal->linked];
            mat4_t parent_camera = mat4_inverse(view_matrix);
            mat4_t child_camera = child_camera_matrix(parent_camera,
                                                      portal, linked);
            framebuffer_t *temp;
            int status;

            temp = framebuffer_create(framebuffer->width,
                                      framebuffer->height);
            if (temp == NULL) {
                return -1;
            }
            framebuffer_clear_color(temp, ctx->scene->background);
            framebuffer_clear_depth(temp, 1);

            status = render_view(ctx, temp, child_camera,
                                 matrix_origin(child_camera), linked,
                                 depth + 1);
            if (status != 0) {
                framebuffer_release(temp);
                return status;
            }

            texture_from_colorbuffer(ctx->scratch_texture, temp);
            draw_portal_face(ctx, framebuffer, portal, view_matrix,
                             active_plane, ctx->scratch_texture);
            framebuffer_release(temp);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* public entry point                                                  */
/* ------------------------------------------------------------------ */

int scene_draw_portals(scene_t *scene, framebuffer_t *dst,
                       const perframe_t *frame,
                       const portal_t *portals, int count,
                       int max_depth, float clip_bias) {
    portal_ctx_t ctx;
    snapshot_t snapshot;
    int num_models, i, status;

    if (dst == NULL || dst->width <= 0 || dst->height <= 0) {
        return -1;
    }
    if (!validate_frame(frame) || !validate_scene(scene) ||
        !validate_portals(portals, count)) {
        return -1;
    }
    if (max_depth < 0 || max_depth > PORTAL_MAX_DEPTH ||
        !is_finitef(clip_bias) || clip_bias < 0) {
        return -1;
    }

    if (!snapshot_capture(dst, &snapshot)) {
        return -1;
    }

    ctx.scene = scene;
    ctx.frame = frame;
    ctx.portals = portals;
    ctx.count = count;
    ctx.max_depth = max_depth;
    ctx.clip_bias = clip_bias;
    ctx.face_program = program_create(
        face_vertex_shader, face_fragment_shader,
        sizeof(face_attribs_t), sizeof(face_varyings_t),
        sizeof(face_uniforms_t), 0, 0);
    ctx.scratch_texture = texture_create(dst->width, dst->height);
    if (ctx.face_program == NULL || ctx.scratch_texture == NULL) {
        if (ctx.face_program != NULL) {
            program_release(ctx.face_program);
        }
        if (ctx.scratch_texture != NULL) {
            texture_release(ctx.scratch_texture);
        }
        snapshot_restore(dst, &snapshot);
        free(snapshot.color);
        free(snapshot.depth);
        return -1;
    }

    framebuffer_clear_color(dst, scene->background);
    framebuffer_clear_depth(dst, 1);

    status = render_view(&ctx, dst, frame->camera_view_matrix,
                         frame->camera_pos, NULL, 0);

    /* close the temporary clipping and restore every model program to
     * the uniforms matching the frame passed in by the caller */
    num_models = darray_size(scene->models);
    for (i = 0; i < num_models; i++) {
        model_t *model = scene->models[i];
        perframe_t perframe = *frame;
        model->update(model, &perframe);
        program_set_clip_plane(model->program, NULL);
    }
    program_set_clip_plane(ctx.face_program, NULL);

    program_release(ctx.face_program);
    texture_release(ctx.scratch_texture);

    if (status != 0) {
        snapshot_restore(dst, &snapshot);
    }
    free(snapshot.color);
    free(snapshot.depth);
    return status == 0 ? 0 : -1;
}
