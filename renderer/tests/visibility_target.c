/*
 * This target test is built by task_support/run.sh in C99 mode.  The
 * production Viewer build compiles each tests source in C89 mode without
 * the task include path, so the body is compiled only for C99 compilers.
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L

#include "task_support/check.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "renderer/core/graphics.h"
#include "renderer/core/macro.h"
#include "renderer/core/maths.h"
#include "renderer/core/visibility.h"

/* -------------------------------------------------------------------------
 * Shared test shaders
 * ------------------------------------------------------------------------- */

typedef struct {
    vec4_t clip;
    float u;
} attribs_t;

typedef struct {
    float u;
} varyings_t;

typedef struct {
    vec4_t color;
    int masked;
    float threshold;
} uniforms_t;

typedef struct {
    uint32_t ids[4];
    vec4_t colors[4];
    int count;
} palette_t;

static unsigned vertex_calls;
static unsigned fragment_calls;
static unsigned coverage_calls;
static unsigned coverage_rejects;
static unsigned resolve_calls;

static void reset_counters(void) {
    vertex_calls = 0;
    fragment_calls = 0;
    coverage_calls = 0;
    coverage_rejects = 0;
    resolve_calls = 0;
}

static vec4_t vertex_main(void *attribs, void *varyings, void *uniforms) {
    attribs_t *a = (attribs_t*)attribs;
    varyings_t *v = (varyings_t*)varyings;
    (void)uniforms;
    ++vertex_calls;
    v->u = a->u;
    return a->clip;
}

static vec4_t fragment_main(void *varyings, void *uniforms,
                            int *discard, int backface) {
    varyings_t *v = (varyings_t*)varyings;
    uniforms_t *u = (uniforms_t*)uniforms;
    (void)backface;
    ++fragment_calls;
    if (u->masked && v->u > u->threshold) {
        *discard = 1;
        return u->color;
    }
    return u->color;
}

static int coverage_main(void *varyings, void *uniforms, int backface) {
    varyings_t *v = (varyings_t*)varyings;
    uniforms_t *u = (uniforms_t*)uniforms;
    (void)backface;
    ++coverage_calls;
    if (u->masked && v->u > u->threshold) {
        ++coverage_rejects;
        return 0;
    }
    return 1;
}

static vec4_t resolve_palette(const void *varyings, void *uniforms,
                              uint32_t primitive_id, int backface) {
    const palette_t *p = (const palette_t*)uniforms;
    int i;
    (void)varyings;
    (void)backface;
    ++resolve_calls;
    for (i = 0; i < p->count; i++) {
        if (p->ids[i] == primitive_id) {
            return p->colors[i];
        }
    }
    return vec4_new(1, 1, 0, 1);  /* unexpected id: bright yellow */
}

static vec4_t resolve_attribute(const void *varyings, void *uniforms,
                                uint32_t primitive_id, int backface) {
    const varyings_t *v = (const varyings_t*)varyings;
    (void)uniforms;
    (void)primitive_id;
    (void)backface;
    ++resolve_calls;
    return vec4_new(v->u, v->u, v->u, 1);
}

/* -------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static program_t *make_program(int double_sided, int blend) {
    return program_create(vertex_main, fragment_main,
                          sizeof(attribs_t), sizeof(varyings_t),
                          sizeof(uniforms_t), double_sided, blend);
}

static void set_vertex(program_t *program, int nth, vec4_t clip, float u) {
    attribs_t *a = (attribs_t*)program_get_attribs(program, nth);
    a->clip = clip;
    a->u = u;
}

static uniforms_t *set_uniforms(program_t *program, vec4_t color,
                                int masked, float threshold) {
    uniforms_t *u = (uniforms_t*)program_get_uniforms(program);
    u->color = color;
    u->masked = masked;
    u->threshold = threshold;
    return u;
}

static void set_palette(palette_t *p, uint32_t id0, vec4_t c0,
                        uint32_t id1, vec4_t c1) {
    p->ids[0] = id0;
    p->ids[1] = id1;
    p->colors[0] = c0;
    p->colors[1] = c1;
    p->count = 2;
}

static unsigned count_covered(const visibility_t *buffer, int width,
                              int height, uint32_t id) {
    unsigned count = 0;
    int x, y;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            visibility_sample_t sample;
            if (visibility_read(buffer, x, y, &sample) &&
                (id == UINT32_MAX || sample.primitive_id == id)) {
                ++count;
            }
        }
    }
    return count;
}

/*
 * Independent reconstruction: combine the caller's original clip
 * coordinates with the stored barycentric weights, then divide by the
 * combined w and map through the viewport transform.
 */
static void reconstruct_point(const visibility_sample_t *sample,
                              const vec4_t clip[3],
                              int width, int height,
                              float *x, float *y, float *depth) {
    float cx = sample->bary[0] * clip[0].x +
               sample->bary[1] * clip[1].x +
               sample->bary[2] * clip[2].x;
    float cy = sample->bary[0] * clip[0].y +
               sample->bary[1] * clip[1].y +
               sample->bary[2] * clip[2].y;
    float cz = sample->bary[0] * clip[0].z +
               sample->bary[1] * clip[1].z +
               sample->bary[2] * clip[2].z;
    float cw = sample->bary[0] * clip[0].w +
               sample->bary[1] * clip[1].w +
               sample->bary[2] * clip[2].w;
    float ndc_x = cx / cw;
    float ndc_y = cy / cw;
    float ndc_z = cz / cw;
    *x = (ndc_x + 1) * 0.5f * (float)width;
    *y = (ndc_y + 1) * 0.5f * (float)height;
    *depth = (ndc_z + 1) * 0.5f;
}

static int bytes_close(unsigned char a, unsigned char b, int tolerance) {
    int d = (int)a - (int)b;
    return d >= -tolerance && d <= tolerance;
}

/* -------------------------------------------------------------------------
 * PPM helpers (windowless output)
 * ------------------------------------------------------------------------- */

static int write_ppm(const char *path, const framebuffer_t *fb) {
    FILE *file = fopen(path, "wb");
    int i;
    int count = fb->width * fb->height;
    if (!file) {
        return -1;
    }
    fprintf(file, "P6\n%d %d\n255\n", fb->width, fb->height);
    for (i = 0; i < count; i++) {
        fputc(fb->color_buffer[i * 4 + 0], file);
        fputc(fb->color_buffer[i * 4 + 1], file);
        fputc(fb->color_buffer[i * 4 + 2], file);
    }
    return fclose(file) == 0 ? 0 : -2;
}

static int read_ppm(const char *path, int width, int height,
                    unsigned char *rgb) {
    FILE *file = fopen(path, "rb");
    int w, h, max_value, count, i;
    char magic[3];
    if (!file) {
        return -1;
    }
    if (fscanf(file, "%2s %d %d %d", magic, &w, &h, &max_value) != 4) {
        fclose(file);
        return -2;
    }
    fgetc(file); /* consume the single whitespace after the header */
    if (strcmp(magic, "P6") || w != width || h != height || max_value != 255) {
        fclose(file);
        return -3;
    }
    count = width * height;
    for (i = 0; i < count; i++) {
        int r = fgetc(file);
        int g = fgetc(file);
        int b = fgetc(file);
        if (r == EOF || g == EOF || b == EOF) {
            fclose(file);
            return -4;
        }
        rgb[i * 3 + 0] = (unsigned char)r;
        rgb[i * 3 + 1] = (unsigned char)g;
        rgb[i * 3 + 2] = (unsigned char)b;
    }
    return fclose(file) == 0 ? 0 : -5;
}

/* -------------------------------------------------------------------------
 * capture group
 * ------------------------------------------------------------------------- */

/*
 * A triangle that crosses the near plane and a side plane, with non-equal w
 * on all three vertices.  Every covered sample is checked against an
 * independent reconstruction of the original clip coordinates, and against
 * an independently computed varying.
 */
static int case_capture_clipped_bary(void) {
    const int width = 32, height = 24;
    visibility_t *buffer;
    program_t *program;
    framebuffer_t *framebuffer;
    vec4_t clip[3];
    float u[3] = {0.15f, 0.72f, 0.31f};
    unsigned covered = 0;
    int x, y, i;

    reset_counters();
    buffer = visibility_create(width, height);
    program = make_program(1, 0);
    framebuffer = framebuffer_create(width, height);

    /* crosses the left x plane (x < -w) and the near plane (w < epsilon) */
    clip[0] = vec4_new(-1.95f, -0.42f, -0.20f, 1.00f);
    clip[1] = vec4_new( 1.05f, -0.28f,  0.18f, 0.90f);
    clip[2] = vec4_new( 0.06f,  0.62f, -0.12f, -0.45f);
    for (i = 0; i < 3; i++) {
        set_vertex(program, i, clip[i], u[i]);
    }
    set_uniforms(program, vec4_new(0, 0, 0, 1), 0, 0);

    TASK_CHECK(visibility_draw_triangle(buffer, program, 7, NULL) == 0);
    /* phased execution: three vertex calls, no fragment shader, no coverage */
    TASK_CHECK(vertex_calls == 3);
    TASK_CHECK(fragment_calls == 0);
    TASK_CHECK(coverage_calls == 0);

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            visibility_sample_t sample;
            if (!visibility_read(buffer, x, y, &sample)) {
                continue;
            }
            ++covered;
            TASK_CHECK(sample.primitive_id == 7);
            TASK_CHECK(fabsf(sample.bary[0] + sample.bary[1] +
                             sample.bary[2] - 1.0f) < 1e-3f);
            {
                float px, py, pd;
                reconstruct_point(&sample, clip, width, height, &px, &py, &pd);
                TASK_CHECK(fabsf(px - ((float)x + 0.5f)) < 5e-3f);
                TASK_CHECK(fabsf(py - ((float)y + 0.5f)) < 5e-3f);
                TASK_CHECK(fabsf(pd - sample.depth) < 5e-3f);
                TASK_CHECK(sample.depth >= 0.0f && sample.depth <= 1.0f);
            }
            /* independently combine the original varyings */
            TASK_CHECK(fabsf(sample.bary[0] * u[0] +
                             sample.bary[1] * u[1] +
                             sample.bary[2] * u[2]) <= 1.0f + 1e-4f);
        }
    }
    TASK_CHECK(covered > 0);

    /* resolve the stored varyings and compare against the same combination */
    framebuffer_clear_color(framebuffer, vec4_new(0.4f, 0.4f, 0.4f, 0.5f));
    framebuffer_clear_depth(framebuffer, 1.0f);
    TASK_CHECK(visibility_resolve(buffer, framebuffer, resolve_attribute,
                                  NULL) == 0);
    TASK_CHECK(resolve_calls == covered);
    TASK_CHECK(vertex_calls == 3); /* resolve never re-runs geometry */

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            visibility_sample_t sample;
            if (!visibility_read(buffer, x, y, &sample)) {
                int index = y * width + x;
                /* background pixels and alpha stay untouched */
                TASK_CHECK(framebuffer->color_buffer[index * 4 + 0] == 102);
                TASK_CHECK(framebuffer->color_buffer[index * 4 + 1] == 102);
                TASK_CHECK(framebuffer->color_buffer[index * 4 + 2] == 102);
                TASK_CHECK(framebuffer->color_buffer[index * 4 + 3] == 127);
                TASK_CHECK(framebuffer->depth_buffer[index] == 1.0f);
                continue;
            }
            {
                int index = y * width + x;
                float expected = sample.bary[0] * u[0] +
                                 sample.bary[1] * u[1] +
                                 sample.bary[2] * u[2];
                unsigned char want = float_to_uchar(
                    float_saturate(expected));
                TASK_CHECK(bytes_close(framebuffer->color_buffer[index * 4],
                                       want, 1));
                TASK_CHECK(bytes_close(framebuffer->color_buffer[index * 4 + 1],
                                       want, 1));
                TASK_CHECK(bytes_close(framebuffer->color_buffer[index * 4 + 2],
                                       want, 1));
                TASK_CHECK(framebuffer->color_buffer[index * 4 + 3] == 127);
                TASK_CHECK(fabsf(framebuffer->depth_buffer[index] -
                                 sample.depth) < 1e-6f);
            }
        }
    }

    visibility_release(buffer);
    program_release(program);
    framebuffer_release(framebuffer);
    return 0;
}

/*
 * Identity bookkeeping and argument validation: background samples,
 * reserved ids, unique ids, and blend rejection before side effects.
 */
static int case_capture_identity(void) {
    const int width = 16, height = 12;
    visibility_t *buffer;
    program_t *program;
    program_t *blend_program;
    vec4_t clip[3];
    unsigned covered_before;
    int i;

    reset_counters();
    buffer = visibility_create(width, height);
    program = make_program(1, 0);

    /* a simple large front-facing triangle */
    clip[0] = vec4_new(-1.2f, -1.0f, -0.30f, 1.0f);
    clip[1] = vec4_new( 1.2f, -0.9f,  0.10f, 1.2f);
    clip[2] = vec4_new( 0.0f,  1.3f,  0.15f, 0.7f);
    for (i = 0; i < 3; i++) {
        set_vertex(program, i, clip[i], (float)i * 0.5f);
    }
    set_uniforms(program, vec4_new(1, 0, 0, 1), 0, 0);

    TASK_CHECK(visibility_draw_triangle(buffer, program, 1, NULL) == 0);
    TASK_CHECK(vertex_calls == 3);
    covered_before = count_covered(buffer, width, height, UINT32_MAX);

    /* background samples are empty with depth 1 */
    {
        visibility_sample_t sample;
        TASK_CHECK(visibility_read(buffer, 0, height - 1, &sample) == 0);
        TASK_CHECK(sample.primitive_id == UINT32_MAX);
        TASK_CHECK(sample.depth == 1.0f);
        TASK_CHECK(sample.bary[0] == 0.0f && sample.bary[1] == 0.0f &&
                   sample.bary[2] == 0.0f);
        TASK_CHECK(visibility_read(buffer, -1, 0, &sample) == 0);
        TASK_CHECK(visibility_read(buffer, width, height - 1, &sample) == 0);
    }

    /* reserved id is rejected before running vertex shaders */
    TASK_CHECK(visibility_draw_triangle(buffer, program, UINT32_MAX, NULL) != 0);
    TASK_CHECK(vertex_calls == 3);

    /* duplicate id within one clear interval is rejected */
    TASK_CHECK(visibility_draw_triangle(buffer, program, 1, NULL) != 0);
    TASK_CHECK(vertex_calls == 3);
    TASK_CHECK(count_covered(buffer, width, height, UINT32_MAX) == covered_before);

    /* blending programs are rejected before any callback or state change */
    blend_program = make_program(1, 1);
    for (i = 0; i < 3; i++) {
        set_vertex(blend_program, i, clip[i], 0.5f);
    }
    set_uniforms(blend_program, vec4_new(0, 0, 1, 0.5f), 0, 0);
    TASK_CHECK(visibility_draw_triangle(buffer, blend_program, 2,
                                        coverage_main) != 0);
    TASK_CHECK(vertex_calls == 3);
    TASK_CHECK(coverage_calls == 0);
    TASK_CHECK(fragment_calls == 0);
    TASK_CHECK(count_covered(buffer, width, height, 2) == 0);

    /* after clear the id becomes reusable and the buffer is empty */
    visibility_clear(buffer);
    TASK_CHECK(count_covered(buffer, width, height, UINT32_MAX) == 0);
    TASK_CHECK(visibility_draw_triangle(buffer, program, 1, NULL) == 0);

    program_release(blend_program);
    program_release(program);
    visibility_release(buffer);
    return 0;
}

/* -------------------------------------------------------------------------
 * replay group
 * ------------------------------------------------------------------------- */

/*
 * Resolve twice with different palettes after releasing the program.  Depth,
 * identity and barycentrics are unchanged; geometry and coverage callbacks
 * do not run again.
 */
static int case_replay_two_recolors(void) {
    const int width = 20, height = 16;
    visibility_t *buffer;
    program_t *program;
    framebuffer_t *framebuffer;
    vec4_t clip[3];
    palette_t palette;
    unsigned covered;
    int i, pass;

    reset_counters();
    buffer = visibility_create(width, height);
    program = make_program(1, 0);
    framebuffer = framebuffer_create(width, height);

    clip[0] = vec4_new(-1.1f, -1.0f, -0.32f, 1.0f);
    clip[1] = vec4_new( 1.1f, -0.8f,  0.20f, 1.3f);
    clip[2] = vec4_new( 0.1f,  1.2f,  0.05f, 0.8f);
    for (i = 0; i < 3; i++) {
        set_vertex(program, i, clip[i], 0.2f + 0.3f * (float)i);
    }
    set_uniforms(program, vec4_new(1, 0, 0, 1), 0, 0);

    TASK_CHECK(visibility_draw_triangle(buffer, program, 11,
                                        coverage_main) == 0);
    TASK_CHECK(vertex_calls == 3);
    TASK_CHECK(fragment_calls == 0);
    TASK_CHECK(coverage_calls > 0);

    covered = count_covered(buffer, width, height, UINT32_MAX);
    TASK_CHECK(covered > 0);

    /* caller may now clobber the program's inputs and release it entirely */
    memset(program_get_attribs(program, 0), 0x5a, sizeof(attribs_t));
    memset(program_get_uniforms(program), 0xa5, sizeof(uniforms_t));
    program_release(program);
    program = NULL;

    set_palette(&palette, 10, vec4_new(0.9f, 0.1f, 0.1f, 1),
                11, vec4_new(0.1f, 0.2f, 0.9f, 1));
    /* palette for primitive 11 is at slot 1 */
    palette.ids[0] = 11;
    palette.colors[0] = vec4_new(0.95f, 0.25f, 0.10f, 1);
    palette.count = 1;

    framebuffer_clear_color(framebuffer, vec4_new(0, 0, 0, 1));
    framebuffer_clear_depth(framebuffer, 1.0f);
    TASK_CHECK(visibility_resolve(buffer, framebuffer, resolve_palette,
                                  &palette) == 0);
    TASK_CHECK(resolve_calls == covered);
    TASK_CHECK(vertex_calls == 3);
    TASK_CHECK(coverage_calls > 0);
    for (pass = 0; pass < width * height; pass++) {
        if (framebuffer->color_buffer[pass * 4 + 2] == 0) {
            continue;
        }
        TASK_CHECK(framebuffer->color_buffer[pass * 4 + 0] == 242);
    }

    /* second coloring: only uniforms change */
    palette.colors[0] = vec4_new(0.15f, 0.85f, 0.35f, 1);
    TASK_CHECK(visibility_resolve(buffer, framebuffer, resolve_palette,
                                  &palette) == 0);
    TASK_CHECK(resolve_calls == 2 * covered);

    for (pass = 0; pass < width * height; pass++) {
        int index = pass;
        visibility_sample_t sample;
        if (visibility_read(buffer, index % width, index / width, &sample)) {
            TASK_CHECK(framebuffer->color_buffer[index * 4 + 0] == 38);
            TASK_CHECK(framebuffer->color_buffer[index * 4 + 1] == 216);
            TASK_CHECK(framebuffer->color_buffer[index * 4 + 2] == 89);
            TASK_CHECK(fabsf(framebuffer->depth_buffer[index] -
                             sample.depth) < 1e-6f);
            TASK_CHECK(sample.primitive_id == 11);
        } else {
            TASK_CHECK(framebuffer->color_buffer[index * 4 + 0] == 0);
            TASK_CHECK(framebuffer->color_buffer[index * 4 + 2] == 0);
            TASK_CHECK(framebuffer->depth_buffer[index] == 1.0f);
        }
    }

    visibility_release(buffer);
    framebuffer_release(framebuffer);
    return 0;
}

/*
 * Two buffers used interleaved, one cleared while the other stays intact,
 * plus a target size mismatch check.
 */
static int case_replay_interleave_clear(void) {
    const int aw = 18, ah = 14;
    const int bw = 14, bh = 18;
    visibility_t *a;
    visibility_t *b;
    program_t *program;
    framebuffer_t *target;
    vec4_t clip[3];
    unsigned covered_a, covered_b;
    visibility_sample_t sample;
    int i;

    reset_counters();
    a = visibility_create(aw, ah);
    b = visibility_create(bw, bh);
    program = make_program(1, 0);
    target = framebuffer_create(aw, ah);

    clip[0] = vec4_new(-1.2f, -1.1f, -0.4f, 1.0f);
    clip[1] = vec4_new( 1.2f, -0.9f,  0.3f, 1.4f);
    clip[2] = vec4_new( 0.0f,  1.3f,  0.0f, 0.9f);

    for (i = 0; i < 3; i++) {
        set_vertex(program, i, clip[i], 0.4f);
    }
    set_uniforms(program, vec4_new(0, 0, 1, 1), 0, 0);
    TASK_CHECK(visibility_draw_triangle(a, program, 20, NULL) == 0);

    for (i = 0; i < 3; i++) {
        set_vertex(program, i, clip[i], 0.6f);
    }
    TASK_CHECK(visibility_draw_triangle(b, program, 21, NULL) == 0);

    covered_a = count_covered(a, aw, ah, UINT32_MAX);
    covered_b = count_covered(b, bw, bh, UINT32_MAX);
    TASK_CHECK(covered_a > 0 && covered_b > 0);

    /* interleaved reads: each buffer keeps its own identity */
    {
        int x, y, seen_a = 0, seen_b = 0;
        for (y = 0; y < ah; y++) {
            for (x = 0; x < aw; x++) {
                if (visibility_read(a, x, y, &sample) &&
                    sample.primitive_id == 20) {
                    ++seen_a;
                }
            }
        }
        for (y = 0; y < bh; y++) {
            for (x = 0; x < bw; x++) {
                if (visibility_read(b, x, y, &sample) &&
                    sample.primitive_id == 21) {
                    ++seen_b;
                }
            }
        }
        TASK_CHECK((unsigned)seen_a == covered_a);
        TASK_CHECK((unsigned)seen_b == covered_b);
    }

    /* clear one buffer; samples empty and depth 1, the other untouched */
    visibility_clear(a);
    TASK_CHECK(count_covered(a, aw, ah, UINT32_MAX) == 0);
    TASK_CHECK(visibility_read(a, 0, 0, &sample) == 0);
    TASK_CHECK(sample.depth == 1.0f);
    TASK_CHECK(count_covered(b, bw, bh, UINT32_MAX) == covered_b);

    /* size mismatch is rejected before any write */
    framebuffer_clear_color(target, vec4_new(0.2f, 0.2f, 0.2f, 1));
    TASK_CHECK(visibility_resolve(b, target, resolve_palette, NULL) != 0);
    TASK_CHECK(target->color_buffer[0] == float_to_uchar(0.2f));

    /* cleared buffer resolves to untouched background */
    {
        palette_t palette;
        set_palette(&palette, 20, vec4_new(1, 0, 0, 1),
                    21, vec4_new(0, 1, 0, 1));
        TASK_CHECK(visibility_resolve(a, target, resolve_palette, &palette) == 0);
        TASK_CHECK(resolve_calls == 0);
        TASK_CHECK(target->color_buffer[0] == 51);
        TASK_CHECK(target->color_buffer[3] == 255);
    }

    /* same id may be reused after a clear */
    for (i = 0; i < 3; i++) {
        set_vertex(program, i, clip[i], 0.4f);
    }
    TASK_CHECK(visibility_draw_triangle(a, program, 20, NULL) == 0);
    TASK_CHECK(count_covered(a, aw, ah, 20) > 0);

    program_release(program);
    visibility_release(a);
    visibility_release(b);
    framebuffer_release(target);
    return 0;
}

/*
 * Programmatically generated intersecting triangles, resolved with two
 * palettes and written as windowless PPM files.  Pixels are verified by
 * reading the files back.
 */
static int case_replay_ppm(void) {
    const int width = 32, height = 24;
    const char *path_a = ".task-build/visibility_palette_a.ppm";
    const char *path_b = ".task-build/visibility_palette_b.ppm";
    const float pi = 3.14159265f;
    visibility_t *buffer;
    program_t *program;
    framebuffer_t *target;
    palette_t palette_a, palette_b;
    unsigned char *read_a;
    unsigned char *read_b;
    unsigned covered = 0, differing = 0, background_checked = 0;
    int k, j, x, y;

    reset_counters();
    buffer = visibility_create(width, height);
    program = make_program(1, 0);
    target = framebuffer_create(width, height);

    /* generated overlapping triangles */
    for (k = 0; k < 2; k++) {
        for (j = 0; j < 3; j++) {
            float theta = 0.75f * (float)k +
                          2.0f * pi * (float)j / 3.0f + 0.35f;
            float radius = 0.80f + 0.12f * (float)k;
            float ndc_x = cosf(theta) * radius * 1.15f;
            float ndc_y = sinf(theta) * radius * 0.90f +
                          (k == 0 ? -0.12f : 0.12f);
            float ndc_z = 0.10f;
            float w = 1.0f + 0.20f * (float)j;
            set_vertex(program, j,
                       vec4_new(ndc_x * w, ndc_y * w, ndc_z * w, w),
                       0.25f * (float)j);
        }
        set_uniforms(program, vec4_new(0.5f, 0.5f, 0.5f, 1), 0, 0);
        TASK_CHECK(visibility_draw_triangle(
            buffer, program, (uint32_t)(100 + k), coverage_main) == 0);
    }
    TASK_CHECK(vertex_calls == 6);
    TASK_CHECK(fragment_calls == 0);

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            visibility_sample_t sample;
            if (visibility_read(buffer, x, y, &sample)) {
                ++covered;
            }
        }
    }
    TASK_CHECK(covered > 0);
    TASK_CHECK(count_covered(buffer, width, height, 100) > 0);
    TASK_CHECK(count_covered(buffer, width, height, 101) > 0);

    set_palette(&palette_a, 100, vec4_new(0.90f, 0.20f, 0.10f, 1),
                101, vec4_new(0.10f, 0.30f, 0.90f, 1));
    set_palette(&palette_b, 100, vec4_new(0.10f, 0.80f, 0.20f, 1),
                101, vec4_new(0.90f, 0.70f, 0.10f, 1));

    framebuffer_clear_color(target, vec4_new(0, 0, 0, 1));
    framebuffer_clear_depth(target, 1.0f);
    TASK_CHECK(visibility_resolve(buffer, target, resolve_palette,
                                  &palette_a) == 0);
    TASK_CHECK(write_ppm(path_a, target) == 0);

    framebuffer_clear_color(target, vec4_new(0, 0, 0, 1));
    TASK_CHECK(visibility_resolve(buffer, target, resolve_palette,
                                  &palette_b) == 0);
    TASK_CHECK(write_ppm(path_b, target) == 0);
    TASK_CHECK(resolve_calls == 2 * covered);

    read_a = (unsigned char*)malloc((size_t)width * height * 3);
    read_b = (unsigned char*)malloc((size_t)width * height * 3);
    TASK_CHECK(read_a && read_b);
    TASK_CHECK(read_ppm(path_a, width, height, read_a) == 0);
    TASK_CHECK(read_ppm(path_b, width, height, read_b) == 0);

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            visibility_sample_t sample;
            int index = y * width + x;
            vec4_t color_a, color_b;
            if (visibility_read(buffer, x, y, &sample)) {
                if (sample.primitive_id == 100) {
                    color_a = palette_a.colors[0];
                    color_b = palette_b.colors[0];
                } else {
                    TASK_CHECK(sample.primitive_id == 101);
                    color_a = palette_a.colors[1];
                    color_b = palette_b.colors[1];
                }
                TASK_CHECK(bytes_close(read_a[index * 3 + 0],
                                       float_to_uchar(color_a.x), 0));
                TASK_CHECK(bytes_close(read_a[index * 3 + 1],
                                       float_to_uchar(color_a.y), 0));
                TASK_CHECK(bytes_close(read_a[index * 3 + 2],
                                       float_to_uchar(color_a.z), 0));
                TASK_CHECK(bytes_close(read_b[index * 3 + 0],
                                       float_to_uchar(color_b.x), 0));
                TASK_CHECK(bytes_close(read_b[index * 3 + 1],
                                       float_to_uchar(color_b.y), 0));
                TASK_CHECK(bytes_close(read_b[index * 3 + 2],
                                       float_to_uchar(color_b.z), 0));
                if (read_a[index * 3 + 0] != read_b[index * 3 + 0] ||
                    read_a[index * 3 + 1] != read_b[index * 3 + 1] ||
                    read_a[index * 3 + 2] != read_b[index * 3 + 2]) {
                    ++differing;
                }
            } else {
                TASK_CHECK(read_a[index * 3 + 0] == 0);
                TASK_CHECK(read_a[index * 3 + 1] == 0);
                TASK_CHECK(read_a[index * 3 + 2] == 0);
                TASK_CHECK(read_b[index * 3 + 0] == 0);
                TASK_CHECK(read_b[index * 3 + 1] == 0);
                TASK_CHECK(read_b[index * 3 + 2] == 0);
                ++background_checked;
            }
        }
    }
    TASK_CHECK(differing == covered);
    TASK_CHECK(background_checked > 0);

    free(read_a);
    free(read_b);
    program_release(program);
    framebuffer_release(target);
    visibility_release(buffer);
    return 0;
}

/* -------------------------------------------------------------------------
 * compat group
 * ------------------------------------------------------------------------- */

static void draw_reference(framebuffer_t *fb, program_t *program,
                           vec4_t color, int masked, float threshold,
                           const vec4_t clip[3], const float u[3]) {
    int i;
    for (i = 0; i < 3; i++) {
        set_vertex(program, i, clip[i], u[i]);
    }
    set_uniforms(program, color, masked, threshold);
    graphics_draw_triangle(fb, program);
}

static int compare_images(const framebuffer_t *reference,
                          const framebuffer_t *resolved,
                          int check_depth) {
    int count = reference->width * reference->height;
    int i;
    for (i = 0; i < count; i++) {
        TASK_CHECK(reference->color_buffer[i * 4 + 0] ==
                   resolved->color_buffer[i * 4 + 0]);
        TASK_CHECK(reference->color_buffer[i * 4 + 1] ==
                   resolved->color_buffer[i * 4 + 1]);
        TASK_CHECK(reference->color_buffer[i * 4 + 2] ==
                   resolved->color_buffer[i * 4 + 2]);
        TASK_CHECK(reference->color_buffer[i * 4 + 3] ==
                   resolved->color_buffer[i * 4 + 3]);
        if (check_depth) {
            TASK_CHECK(fabsf(reference->depth_buffer[i] -
                             resolved->depth_buffer[i]) < 1e-6f);
        }
    }
    return 0;
}

/*
 * Front/back ordering, non-equal w, and the <= equal-depth rule where a
 * later submission replaces an earlier one.  The resolved image must match
 * the production pipeline pixel for pixel.
 */
static int case_compat_depth_order(void) {
    const int width = 28, height = 22;
    visibility_t *buffer;
    program_t *reference_program;
    program_t *capture_program;
    framebuffer_t *reference;
    framebuffer_t *resolved;
    palette_t palette;
    vec4_t front[3], back[3], later[3];
    float u[3] = {0.2f, 0.6f, 0.4f};
    unsigned red, green, blue, backface_samples;
    int x, y, i;

    reset_counters();
    buffer = visibility_create(width, height);
    reference_program = make_program(1, 0);
    capture_program = make_program(1, 0);
    reference = framebuffer_create(width, height);
    resolved = framebuffer_create(width, height);

    /* front-facing, constant depth 0.4, non-equal w */
    front[0] = vec4_new(-1.30f * 1.00f, -0.90f * 1.00f, -0.20f * 1.00f, 1.00f);
    front[1] = vec4_new( 0.40f * 1.20f, -0.90f * 1.20f, -0.20f * 1.20f, 1.20f);
    front[2] = vec4_new(-0.30f * 0.80f,  0.80f * 0.80f, -0.20f * 0.80f, 0.80f);

    /* back-facing (reversed winding), constant depth 0.65, drawn double-sided */
    back[0] = vec4_new( 1.40f * 1.10f, -0.90f * 1.10f,  0.30f * 1.10f, 1.10f);
    back[1] = vec4_new( 0.00f * 1.30f,  0.50f * 1.30f,  0.30f * 1.30f, 1.30f);
    back[2] = vec4_new( 0.70f * 0.80f,  0.90f * 0.80f,  0.30f * 0.80f, 0.80f);

    /* later submission at the same depth 0.4, shifted right: equal-depth wins */
    later[0] = vec4_new(-0.85f * 1.00f, -0.90f * 1.00f, -0.20f * 1.00f, 1.00f);
    later[1] = vec4_new( 0.85f * 1.20f, -0.90f * 1.20f, -0.20f * 1.20f, 1.20f);
    later[2] = vec4_new( 0.15f * 0.80f,  0.80f * 0.80f, -0.20f * 0.80f, 0.80f);

    /* reference pipeline */
    draw_reference(reference, reference_program, vec4_new(1, 0, 0, 1), 0, 0,
                   front, u);
    draw_reference(reference, reference_program, vec4_new(0, 1, 0, 1), 0, 0,
                   back, u);
    draw_reference(reference, reference_program, vec4_new(0, 0, 1, 1), 0, 0,
                   later, u);
    TASK_CHECK(vertex_calls == 9);
    TASK_CHECK(fragment_calls > 0);

    /* capture side: same geometry, no fragment shader runs */
    reset_counters();
    for (i = 0; i < 3; i++) {
        set_vertex(capture_program, i, front[i], u[i]);
    }
    set_uniforms(capture_program, vec4_new(1, 0, 0, 1), 0, 0);
    TASK_CHECK(visibility_draw_triangle(buffer, capture_program, 30, NULL) == 0);

    for (i = 0; i < 3; i++) {
        set_vertex(capture_program, i, back[i], u[i]);
    }
    set_uniforms(capture_program, vec4_new(0, 1, 0, 1), 0, 0);
    TASK_CHECK(visibility_draw_triangle(buffer, capture_program, 31, NULL) == 0);

    for (i = 0; i < 3; i++) {
        set_vertex(capture_program, i, later[i], u[i]);
    }
    set_uniforms(capture_program, vec4_new(0, 0, 1, 1), 0, 0);
    TASK_CHECK(visibility_draw_triangle(buffer, capture_program, 32, NULL) == 0);
    TASK_CHECK(vertex_calls == 9);
    TASK_CHECK(fragment_calls == 0);

    set_palette(&palette, 30, vec4_new(1, 0, 0, 1),
                31, vec4_new(0, 1, 0, 1));
    palette.ids[2] = 32;
    palette.colors[2] = vec4_new(0, 0, 1, 1);
    palette.count = 3;

    framebuffer_clear_color(resolved, vec4_new(0, 0, 0, 1));
    framebuffer_clear_depth(resolved, 1.0f);
    TASK_CHECK(visibility_resolve(buffer, resolved, resolve_palette,
                                  &palette) == 0);

    TASK_CHECK(compare_images(reference, resolved, 1) == 0);

    red = count_covered(buffer, width, height, 30);
    green = count_covered(buffer, width, height, 31);
    blue = count_covered(buffer, width, height, 32);
    TASK_CHECK(red > 0);
    TASK_CHECK(green > 0);
    TASK_CHECK(blue > 0);
    /* later submission replaced part of the earlier equal-depth triangle */
    TASK_CHECK(blue > 0 && red + blue <= count_covered(buffer, width, height,
                                                       UINT32_MAX));

    backface_samples = 0;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            visibility_sample_t sample;
            if (visibility_read(buffer, x, y, &sample)) {
                if (sample.primitive_id == 31) {
                    TASK_CHECK(sample.backface == 1);
                    ++backface_samples;
                } else {
                    TASK_CHECK(sample.backface == 0);
                }
                TASK_CHECK(sample.depth >= 0 && sample.depth <= 1);
            }
        }
    }
    TASK_CHECK(backface_samples == green);

    /* back-face culling: single-sided program discards the reversed winding */
    {
        visibility_t *culled = visibility_create(width, height);
        program_t *single = make_program(0, 0);
        for (i = 0; i < 3; i++) {
            set_vertex(single, i, back[i], u[i]);
        }
        set_uniforms(single, vec4_new(0, 1, 0, 1), 0, 0);
        TASK_CHECK(visibility_draw_triangle(culled, single, 33, NULL) == 0);
        TASK_CHECK(count_covered(culled, width, height, UINT32_MAX) == 0);
        TASK_CHECK(count_covered(culled, width, height, 33) == 0);
        program_release(single);
        visibility_release(culled);
    }

    program_release(reference_program);
    program_release(capture_program);
    framebuffer_release(reference);
    framebuffer_release(resolved);
    visibility_release(buffer);
    return 0;
}

/*
 * Coverage-culled candidates: holes must match the production alpha-test
 * pipeline exactly, and a rejected nearer candidate must not occupy depth,
 * so the farther triangle behind it stays visible.
 */
static int case_compat_hole_depth(void) {
    const int width = 24, height = 20;
    visibility_t *buffer;
    program_t *reference_program;
    program_t *capture_program;
    framebuffer_t *reference;
    framebuffer_t *resolved;
    palette_t palette;
    vec4_t front[3], behind[3];
    float u_front[3] = {0.05f, 0.30f, 1.00f};
    float u_behind[3] = {0.4f, 0.4f, 0.4f};
    unsigned red, green, rejected_depth_ok;
    int x, y, i;

    reset_counters();
    buffer = visibility_create(width, height);
    reference_program = make_program(1, 0);
    capture_program = make_program(1, 0);
    reference = framebuffer_create(width, height);
    resolved = framebuffer_create(width, height);

    /* constant window depth 0.30: ndc z = -0.4, so clip z = -0.4 * w */
    front[0] = vec4_new(-1.25f, -1.05f, -0.40f * 1.00f, 1.00f);
    front[1] = vec4_new( 1.20f, -0.95f, -0.40f * 1.10f, 1.10f);
    front[2] = vec4_new( 0.05f,  1.25f, -0.40f * 0.90f, 0.90f);
    behind[0] = vec4_new(-1.20f, -1.00f,  0.40f * 1.00f, 1.00f);
    behind[1] = vec4_new( 1.15f, -0.90f,  0.40f * 1.10f, 1.10f);
    behind[2] = vec4_new( 0.00f,  1.20f,  0.40f * 0.90f, 0.90f);

    /* reference: alpha test on the nearer triangle only */
    draw_reference(reference, reference_program, vec4_new(1, 0, 0, 1),
                   1, 0.5f, front, u_front);
    TASK_CHECK(fragment_calls > 0);
    draw_reference(reference, reference_program, vec4_new(0, 1, 0, 1),
                   0, 0, behind, u_behind);
    TASK_CHECK(vertex_calls == 6);

    reset_counters();
    for (i = 0; i < 3; i++) {
        set_vertex(capture_program, i, front[i], u_front[i]);
    }
    set_uniforms(capture_program, vec4_new(1, 0, 0, 1), 1, 0.5f);
    TASK_CHECK(visibility_draw_triangle(buffer, capture_program, 40,
                                        coverage_main) == 0);
    TASK_CHECK(vertex_calls == 3);
    TASK_CHECK(fragment_calls == 0);
    TASK_CHECK(coverage_calls > 0);
    TASK_CHECK(coverage_rejects > 0);

    for (i = 0; i < 3; i++) {
        set_vertex(capture_program, i, behind[i], u_behind[i]);
    }
    set_uniforms(capture_program, vec4_new(0, 1, 0, 1), 0, 0);
    TASK_CHECK(visibility_draw_triangle(buffer, capture_program, 41,
                                        coverage_main) == 0);
    TASK_CHECK(vertex_calls == 6);
    TASK_CHECK(fragment_calls == 0);
    TASK_CHECK(coverage_rejects > 0);

    set_palette(&palette, 40, vec4_new(1, 0, 0, 1),
                41, vec4_new(0, 1, 0, 1));
    framebuffer_clear_color(resolved, vec4_new(0, 0, 0, 1));
    framebuffer_clear_depth(resolved, 1.0f);
    TASK_CHECK(visibility_resolve(buffer, resolved, resolve_palette,
                                  &palette) == 0);
    TASK_CHECK(compare_images(reference, resolved, 1) == 0);

    red = count_covered(buffer, width, height, 40);
    green = count_covered(buffer, width, height, 41);
    TASK_CHECK(red > 0);
    TASK_CHECK(green > 0);

    /* a rejected candidate left the depth at the farther triangle's value */
    rejected_depth_ok = 0;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            visibility_sample_t sample;
            if (visibility_read(buffer, x, y, &sample) &&
                sample.primitive_id == 41) {
                TASK_CHECK(fabsf(sample.depth - 0.7f) < 1e-3f);
                ++rejected_depth_ok;
            }
        }
    }
    TASK_CHECK(rejected_depth_ok == green);

    program_release(reference_program);
    program_release(capture_program);
    framebuffer_release(reference);
    framebuffer_release(resolved);
    visibility_release(buffer);
    return 0;
}

/* -------------------------------------------------------------------------
 * registration
 * ------------------------------------------------------------------------- */

const struct task_case task_cases[] = {
    {"capture", "clipped_bary", case_capture_clipped_bary},
    {"capture", "identity_background", case_capture_identity},
    {"replay", "two_recolors", case_replay_two_recolors},
    {"replay", "interleave_clear", case_replay_interleave_clear},
    {"replay", "intersecting_ppm", case_replay_ppm},
    {"compat", "depth_order", case_compat_depth_order},
    {"compat", "hole_depth", case_compat_hole_depth},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

#else

/* C89 fallback: the production build links this globbed test source. */
typedef int visibility_target_unused_t;

#endif /* C99 or newer */

