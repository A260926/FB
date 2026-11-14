#include <stdlib.h>
#include <string.h>
#include "visibility.h"
#include "graphics_internal.h"
#include "macro.h"
#include "maths.h"

/*
 * Storage model
 * -------------
 * Each covered pixel stores only its final visible sample: submission index,
 * original-triangle barycentric weights, depth and facing.  The three
 * original (post-vertex-shader) varying blocks of every submission are kept
 * in an arena owned by the visibility buffer.  No per-pixel varying, color
 * or superseded candidate is kept, and resolve never re-runs geometry.
 */

struct submission {
    uint32_t primitive_id;
    int sizeof_varyings;
    size_t arena_offset;
};

struct visibility {
    int width;
    int height;
    int num_pixels;

    /* one final sample per pixel */
    uint32_t *sample_submission;   /* UINT32_MAX means background */
    float *sample_bary;            /* 3 weights per pixel */
    float *sample_depth;
    unsigned char *sample_backface;

    /* submission snapshots */
    struct submission *submissions;
    int num_submissions;
    int capacity_submissions;
    unsigned char *varying_arena;
    size_t arena_used;
    size_t arena_capacity;

    /* pipeline scratch */
    float *scratch_in[GRAPHICS_MAX_VARYINGS];
    float *scratch_out[GRAPHICS_MAX_VARYINGS];
    float *scratch_fragment_varyings;
    int scratch_stride;
};

/* create / release / clear */

visibility_t *visibility_create(int width, int height) {
    visibility_t *buffer;
    int num_pixels;

    if (width <= 0 || height <= 0) {
        return NULL;
    }
    num_pixels = width * height;
    if (num_pixels <= 0) {
        return NULL;
    }

    buffer = (visibility_t*)malloc(sizeof(visibility_t));
    if (!buffer) {
        return NULL;
    }
    memset(buffer, 0, sizeof(visibility_t));
    buffer->width = width;
    buffer->height = height;
    buffer->num_pixels = num_pixels;

    buffer->sample_submission = (uint32_t*)malloc(
        sizeof(uint32_t) * (size_t)num_pixels);
    buffer->sample_bary = (float*)malloc(
        sizeof(float) * 3 * (size_t)num_pixels);
    buffer->sample_depth = (float*)malloc(
        sizeof(float) * (size_t)num_pixels);
    buffer->sample_backface = (unsigned char*)malloc(
        sizeof(unsigned char) * (size_t)num_pixels);

    if (!buffer->sample_submission || !buffer->sample_bary ||
        !buffer->sample_depth || !buffer->sample_backface) {
        visibility_release(buffer);
        return NULL;
    }

    visibility_clear(buffer);
    return buffer;
}

void visibility_release(visibility_t *buffer) {
    int i;
    if (!buffer) {
        return;
    }
    free(buffer->sample_submission);
    free(buffer->sample_bary);
    free(buffer->sample_depth);
    free(buffer->sample_backface);
    free(buffer->submissions);
    free(buffer->varying_arena);
    for (i = 0; i < GRAPHICS_MAX_VARYINGS; i++) {
        free(buffer->scratch_in[i]);
        free(buffer->scratch_out[i]);
    }
    free(buffer->scratch_fragment_varyings);
    free(buffer);
}

void visibility_clear(visibility_t *buffer) {
    int i;
    if (!buffer) {
        return;
    }
    for (i = 0; i < buffer->num_pixels; i++) {
        buffer->sample_submission[i] = UINT32_MAX;
        buffer->sample_depth[i] = 1.0f;
        buffer->sample_bary[i * 3 + 0] = 0.0f;
        buffer->sample_bary[i * 3 + 1] = 0.0f;
        buffer->sample_bary[i * 3 + 2] = 0.0f;
        buffer->sample_backface[i] = 0;
    }
    buffer->num_submissions = 0;
    buffer->arena_used = 0;
}

/* scratch management */

static int ensure_scratch(visibility_t *buffer, int sizeof_varyings) {
    int i;
    int stride_in_floats;

    if (sizeof_varyings <= buffer->scratch_stride) {
        return 0;
    }
    stride_in_floats = sizeof_varyings / (int)sizeof(float);
    for (i = 0; i < GRAPHICS_MAX_VARYINGS; i++) {
        float *in_data = (float*)realloc(
            buffer->scratch_in[i], sizeof(float) * (size_t)stride_in_floats);
        float *out_data;
        if (!in_data) {
            return -1;
        }
        buffer->scratch_in[i] = in_data;
        out_data = (float*)realloc(
            buffer->scratch_out[i], sizeof(float) * (size_t)stride_in_floats);
        if (!out_data) {
            return -1;
        }
        buffer->scratch_out[i] = out_data;
    }
    {
        float *fragment_data = (float*)realloc(
            buffer->scratch_fragment_varyings,
            sizeof(float) * (size_t)stride_in_floats);
        if (!fragment_data) {
            return -1;
        }
        buffer->scratch_fragment_varyings = fragment_data;
    }
    buffer->scratch_stride = sizeof_varyings;
    return 0;
}

static int reserve_submission(visibility_t *buffer, size_t snapshot_size) {
    if (buffer->num_submissions >= buffer->capacity_submissions) {
        int new_capacity = buffer->capacity_submissions
            ? buffer->capacity_submissions * 2 : 16;
        struct submission *grown = (struct submission*)realloc(
            buffer->submissions,
            sizeof(struct submission) * (size_t)new_capacity);
        if (!grown) {
            return -1;
        }
        buffer->submissions = grown;
        buffer->capacity_submissions = new_capacity;
    }
    if (buffer->arena_used + snapshot_size > buffer->arena_capacity) {
        size_t new_capacity = buffer->arena_capacity
            ? buffer->arena_capacity : 4096;
        unsigned char *grown;
        while (new_capacity < buffer->arena_used + snapshot_size) {
            new_capacity *= 2;
        }
        grown = (unsigned char*)realloc(buffer->varying_arena, new_capacity);
        if (!grown) {
            return -1;
        }
        buffer->varying_arena = grown;
        buffer->arena_capacity = new_capacity;
    }
    return 0;
}

/* draw */

typedef struct {
    visibility_t *buffer;
    uint32_t submission_index;
    void *bary_varyings[3];
    int sizeof_varyings;
    visibility_coverage_t *coverage;
    void *uniforms;
    float *fragment_varyings;
} capture_context_t;

static int capture_fragment(void *user, const raster_fragment_t *fragment) {
    capture_context_t *context = (capture_context_t*)user;
    visibility_t *buffer = context->buffer;
    float bary[3];
    int index = fragment->index;

    /* the coverage callback sees perspective-correct varyings */
    graphics_interpolate_varyings(
        fragment->varyings, context->fragment_varyings,
        context->sizeof_varyings, fragment->weights, fragment->recip_w);

    if (context->coverage &&
        !context->coverage(context->fragment_varyings, context->uniforms,
                           fragment->backface)) {
        /* a rejected candidate does not occupy depth */
        return 0;
    }

    /* recover weights of the three original vertices */
    graphics_interpolate_varyings(
        context->bary_varyings, bary,
        3 * (int)sizeof(float), fragment->weights, fragment->recip_w);

    buffer->sample_submission[index] = context->submission_index;
    buffer->sample_bary[index * 3 + 0] = bary[0];
    buffer->sample_bary[index * 3 + 1] = bary[1];
    buffer->sample_bary[index * 3 + 2] = bary[2];
    buffer->sample_depth[index] = fragment->depth;
    buffer->sample_backface[index] = fragment->backface ? 1 : 0;

    /* commit: the shared rasterizer writes the depth value */
    return 1;
}

int visibility_draw_triangle(
        visibility_t *buffer, program_t *program,
        uint32_t primitive_id, visibility_coverage_t *coverage) {
    vec4_t in_coords[GRAPHICS_MAX_VARYINGS];
    vec4_t out_coords[GRAPHICS_MAX_VARYINGS];
    float in_bary[GRAPHICS_MAX_VARYINGS][3];
    float out_bary[GRAPHICS_MAX_VARYINGS][3];
    void *in_varyings[GRAPHICS_MAX_VARYINGS];
    void *out_varyings[GRAPHICS_MAX_VARYINGS];
    int num_vertices;
    int sizeof_varyings;
    int num_floats;
    size_t snapshot_size;
    size_t offset;
    int submission_index;
    int i, j;

    if (!buffer || !program) {
        return 1;
    }

    /*
     * Reject unsupported programs before running any callback or touching
     * any buffer state.
     */
    if (program->enable_blend) {
        return 2;
    }
    if (primitive_id == UINT32_MAX) {
        return 3;
    }
    for (i = 0; i < buffer->num_submissions; i++) {
        if (buffer->submissions[i].primitive_id == primitive_id) {
            return 4;
        }
    }

    sizeof_varyings = program->sizeof_varyings;
    num_floats = sizeof_varyings / (int)sizeof(float);
    snapshot_size = (size_t)sizeof_varyings * 3;

    if (ensure_scratch(buffer, sizeof_varyings) != 0) {
        return 5;
    }
    if (reserve_submission(buffer, snapshot_size) != 0) {
        return 6;
    }

    submission_index = buffer->num_submissions;
    offset = buffer->arena_used;
    buffer->submissions[submission_index].primitive_id = primitive_id;
    buffer->submissions[submission_index].sizeof_varyings = sizeof_varyings;
    buffer->submissions[submission_index].arena_offset = offset;

    /* execute the three vertex shaders, snapshotting original varyings */
    for (i = 0; i < 3; i++) {
        float *dst = (float*)(buffer->varying_arena + offset) + i * num_floats;
        vec4_t clip_coord = program->vertex_shader(
            program->shader_attribs[i], dst, program->shader_uniforms);
        in_coords[i] = clip_coord;
        in_varyings[i] = dst;
    }
    buffer->num_submissions += 1;
    buffer->arena_used += snapshot_size;

    /* original-vertex barycentric basis, carried through clipping */
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            in_bary[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }
    for (i = 0; i < GRAPHICS_MAX_VARYINGS; i++) {
        out_varyings[i] = buffer->scratch_out[i];
        in_varyings[i] = buffer->scratch_in[i];
    }
    /*
     * Clipping ping-pongs between the in and out buffers, so the retained
     * original snapshots must be copied into writable scratch first.
     */
    for (i = 0; i < 3; i++) {
        memcpy(in_varyings[i], buffer->varying_arena + offset +
               (size_t)i * (size_t)sizeof_varyings, (size_t)sizeof_varyings);
    }

    num_vertices = graphics_clip_triangle(
        sizeof_varyings, in_coords, in_varyings, in_bary,
        out_coords, out_varyings, out_bary);

    /* triangle assembly and rasterization, one identity per submission */
    for (i = 0; i < num_vertices - 2; i++) {
        int index0 = 0;
        int index1 = i + 1;
        int index2 = i + 2;
        vec4_t clip_coords[3];
        void *varyings[3];
        capture_context_t context;
        int is_culled;

        clip_coords[0] = out_coords[index0];
        clip_coords[1] = out_coords[index1];
        clip_coords[2] = out_coords[index2];
        varyings[0] = out_varyings[index0];
        varyings[1] = out_varyings[index1];
        varyings[2] = out_varyings[index2];

        context.buffer = buffer;
        context.submission_index = (uint32_t)submission_index;
        context.sizeof_varyings = sizeof_varyings;
        context.coverage = coverage;
        context.uniforms = program->shader_uniforms;
        context.fragment_varyings = buffer->scratch_fragment_varyings;
        context.bary_varyings[0] = out_bary[index0];
        context.bary_varyings[1] = out_bary[index1];
        context.bary_varyings[2] = out_bary[index2];

        is_culled = graphics_rasterize_triangle(
            buffer->width, buffer->height, buffer->sample_depth,
            clip_coords, varyings, program->double_sided,
            capture_fragment, &context);
        if (is_culled) {
            break;
        }
    }

    return 0;
}

/* read */

int visibility_read(
        const visibility_t *buffer, int x, int y, visibility_sample_t *out) {
    int index;
    uint32_t submission_index;

    if (out) {
        out->primitive_id = UINT32_MAX;
        out->bary[0] = 0.0f;
        out->bary[1] = 0.0f;
        out->bary[2] = 0.0f;
        out->depth = 1.0f;
        out->backface = 0;
    }
    if (!buffer || !out || x < 0 || y < 0 ||
        x >= buffer->width || y >= buffer->height) {
        return 0;
    }

    index = y * buffer->width + x;
    submission_index = buffer->sample_submission[index];
    if (submission_index == UINT32_MAX) {
        return 0;
    }

    out->primitive_id = buffer->submissions[submission_index].primitive_id;
    out->bary[0] = buffer->sample_bary[index * 3 + 0];
    out->bary[1] = buffer->sample_bary[index * 3 + 1];
    out->bary[2] = buffer->sample_bary[index * 3 + 2];
    out->depth = buffer->sample_depth[index];
    out->backface = buffer->sample_backface[index];
    return 1;
}

/* resolve */

int visibility_resolve(
        const visibility_t *buffer, framebuffer_t *framebuffer,
        visibility_shader_t *shader, void *uniforms) {
    float *varyings;
    int max_sizeof_varyings;
    int i, j;

    if (!buffer || !framebuffer || !shader) {
        return 1;
    }
    if (framebuffer->width != buffer->width ||
        framebuffer->height != buffer->height) {
        return 2;
    }

    max_sizeof_varyings = 0;
    for (i = 0; i < buffer->num_submissions; i++) {
        if (buffer->submissions[i].sizeof_varyings > max_sizeof_varyings) {
            max_sizeof_varyings = buffer->submissions[i].sizeof_varyings;
        }
    }
    varyings = (float*)malloc(sizeof(float) *
                              (size_t)(max_sizeof_varyings > 0
                                       ? max_sizeof_varyings : 1));
    if (!varyings) {
        return 3;
    }

    for (i = 0; i < buffer->num_pixels; i++) {
        uint32_t submission_index = buffer->sample_submission[i];
        const struct submission *submission;
        const float *src;
        const float *bary;
        int num_floats;
        vec4_t color;

        if (submission_index == UINT32_MAX) {
            continue;
        }
        submission = &buffer->submissions[submission_index];
        src = (const float*)(buffer->varying_arena + submission->arena_offset);
        bary = buffer->sample_bary + i * 3;
        num_floats = submission->sizeof_varyings / (int)sizeof(float);

        for (j = 0; j < num_floats; j++) {
            varyings[j] = src[j] * bary[0] +
                          src[num_floats + j] * bary[1] +
                          src[2 * num_floats + j] * bary[2];
        }

        color = shader(varyings, uniforms,
                       submission->primitive_id,
                       buffer->sample_backface[i]);

        /* same saturation and byte conversion as the framebuffer pipeline */
        color = vec4_saturate(color);
        framebuffer->color_buffer[i * 4 + 0] = float_to_uchar(color.x);
        framebuffer->color_buffer[i * 4 + 1] = float_to_uchar(color.y);
        framebuffer->color_buffer[i * 4 + 2] = float_to_uchar(color.z);
        /* alpha byte is preserved */
        framebuffer->depth_buffer[i] = buffer->sample_depth[i];
    }

    free(varyings);
    return 0;
}
