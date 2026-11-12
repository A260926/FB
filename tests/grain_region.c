/*
 * Copyright © 2026, VideoLAN and dav1d authors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Verification of dav1d_apply_grain_region(): region grain output must be
 * bit-identical to the matching rectangle of a full-frame dav1d_apply_grain()
 * run, while the region call performs no scratch heap allocation and leaves
 * the input picture untouched. Decodes verification/fixtures/420-{8,10}.ivf.
 */

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <execinfo.h>

#include "dav1d/dav1d.h"

#include "src/ref.h"

#if defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define GRAIN_REGION_HAS_ASAN 1
# endif
#endif

/*
 * Allocation observation: the static dav1d library resolves malloc() and
 * posix_memalign() dynamically, so these definitions interpose on it. While
 * `grain_region_observing` is set, every heap allocation performed by the
 * library is counted; dav1d_apply_grain_region() must never allocate, which
 * proves it builds no full-frame (or region-sized) temporary pixel plane.
 * Disabled under ASan, which provides its own allocator.
 */
#ifndef GRAIN_REGION_HAS_ASAN
#include <dlfcn.h>
#include <pthread.h>

static int grain_region_observing;
static unsigned long grain_region_alloc_events;
static size_t grain_region_alloc_bytes;

static void *(*real_malloc_fn)(size_t);
static void (*real_free_fn)(void *);
static int (*real_posix_memalign_fn)(void **, size_t, size_t);

/* dlsym() may itself allocate before the real symbols are resolved. */
static char interpose_bootstrap[1 << 16];
static size_t interpose_bootstrap_off;
static int interpose_resolving;

static void *bootstrap_alloc(size_t sz) {
    sz = (sz + 63u) & ~(size_t) 63u;
    if (interpose_bootstrap_off + sz > sizeof(interpose_bootstrap))
        return NULL;
    void *p = interpose_bootstrap + interpose_bootstrap_off;
    interpose_bootstrap_off += sz;
    return p;
}

void *malloc(size_t size) {
    if (!real_malloc_fn) {
        if (interpose_resolving)
            return bootstrap_alloc(size);
        interpose_resolving = 1;
        real_malloc_fn = dlsym(RTLD_NEXT, "malloc");
        interpose_resolving = 0;
        if (!real_malloc_fn)
            return bootstrap_alloc(size);
    }
    if (grain_region_observing) {
        grain_region_alloc_events++;
        grain_region_alloc_bytes += size;
    }
    return real_malloc_fn(size);
}

void free(void *ptr) {
    if (!ptr)
        return;
    if ((char *) ptr >= interpose_bootstrap &&
        (char *) ptr < interpose_bootstrap + sizeof(interpose_bootstrap))
        return; /* bootstrap scratch is never individually freed */
    if (!real_free_fn) {
        if (interpose_resolving)
            return;
        interpose_resolving = 1;
        real_free_fn = dlsym(RTLD_NEXT, "free");
        interpose_resolving = 0;
        if (!real_free_fn)
            return;
    }
    real_free_fn(ptr);
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    if (!real_posix_memalign_fn) {
        if (interpose_resolving) {
            *memptr = bootstrap_alloc(size);
            return *memptr ? 0 : ENOMEM;
        }
        interpose_resolving = 1;
        real_posix_memalign_fn = dlsym(RTLD_NEXT, "posix_memalign");
        interpose_resolving = 0;
        if (!real_posix_memalign_fn) {
            *memptr = bootstrap_alloc(size);
            return *memptr ? 0 : ENOMEM;
        }
    }
    if (grain_region_observing) {
        grain_region_alloc_events++;
        grain_region_alloc_bytes += size;
    }
    return real_posix_memalign_fn(memptr, alignment, size);
}
#endif /* GRAIN_REGION_HAS_ASAN */

#ifndef GRAIN_REGION_HAS_ASAN
static void crash_handler(int sig) {
    void *bt[40];
    const int n = backtrace(bt, 40);
    char msg[64];
    const int len = snprintf(msg, sizeof(msg), "\ncaught signal %d\n", sig);
    if (write(2, msg, (size_t) len) == 0) { /* ignore */ }
    backtrace_symbols_fd(bt, n, 2);
    _exit(2);
}
#endif

static unsigned scenarios;
static unsigned long long pixels_compared;
static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
    } \
} while (0)

typedef struct {
    const char *name;
    int y_grain;
    int u_grain;
    int v_grain;
    int chroma_from_luma;
    int overlap;
    int restricted;
    int ar;
    unsigned seed;
} GrainVariant;

static const GrainVariant variants[] = {
    { "strong_correlated_overlap", 1, 1, 1, 0, 1, 0, 1, 12345 },
    { "overlap_off",                1, 1, 1, 0, 0, 0, 0, 777   },
    { "luma_driven_chroma",         1, 0, 0, 1, 1, 1, 1, 4242  },
    { "luma_plane_only",            1, 0, 0, 0, 1, 0, 0, 99    },
    { "chroma_planes_only",         0, 1, 1, 0, 1, 0, 1, 31337 },
    { "one_chroma_plane_only",      0, 1, 0, 0, 0, 0, 0, 555   },
    /* has_grain() is true only via clip_to_restricted_range &&
     * chroma_scaling_from_luma; chroma is still grained from luma scaling. */
    { "clip_csfl_no_points",        0, 0, 0, 1, 1, 1, 1, 808   },
    { "no_grain",                   0, 0, 0, 0, 0, 0, 0, 0     },
};
#define N_VARIANTS (int)(sizeof(variants) / sizeof(variants[0]))

typedef struct { int x, y, w, h; } Rect;

/* Region matrix: 32px block crossings, mid-block 1x1, odd sizes, corners. */
static const Rect regions[] = {
    { 16, 16, 48, 48 }, /* crosses luma block boundaries on both axes */
    { 34, 34,  1,  1 }, /* 1x1 starting in the middle of a block */
    {  2,  2,  1,  1 }, /* 1x1 near the origin */
    { 10, 10, 33, 31 }, /* odd width and height */
    { 94, 78,  2,  2 }, /* bottom-right picture corner */
    { 90, 74,  6,  6 }, /* bottom/right edges, even size */
    {  0,  0, 96, 80 }, /* the whole picture */
    {  0, 32, 96,  1 }, /* single odd-width row across block columns */
};
#define N_REGIONS (int)(sizeof(regions) / sizeof(regions[0]))

static int bpc, bps;

static void configure_grain(Dav1dFrameHeader *const h, const GrainVariant *const v) {
    Dav1dFilmGrainData *const d = &h->film_grain.data;
    memset(d, 0, sizeof(*d));
    h->film_grain.present = 1;
    h->film_grain.update = 1;
    d->seed = v->seed;
    d->scaling_shift = 8;
    d->ar_coeff_shift = 6;
    d->grain_scale_shift = 0;
    d->overlap_flag = v->overlap;
    d->clip_to_restricted_range = v->restricted;
    d->chroma_scaling_from_luma = v->chroma_from_luma;

    if (v->y_grain) {
        d->num_y_points = 2;
        d->y_points[0][0] = 0;   d->y_points[0][1] = 80;
        d->y_points[1][0] = 255; d->y_points[1][1] = 80;
    }
    for (int pl = 0; pl < 2; pl++) {
        const int on = pl == 0 ? v->u_grain : v->v_grain;
        if (on && !v->chroma_from_luma) {
            d->num_uv_points[pl] = 2;
            d->uv_points[pl][0][0] = 0;   d->uv_points[pl][0][1] = 40;
            d->uv_points[pl][1][0] = 255; d->uv_points[pl][1][1] = 40;
        }
        d->uv_mult[pl] = 64;
        d->uv_luma_mult[pl] = 64;
        d->uv_offset[pl] = 0;
    }

    if (v->ar) {
        d->ar_coeff_lag = 3;
        for (int i = 0; i < 24; i++)
            d->ar_coeffs_y[i] = 10;
        for (int pl = 0; pl < 2; pl++)
            for (int i = 0; i < 25; i++)
                d->ar_coeffs_uv[pl][i] = 10;
    }
}

/* One guarded output plane: sentinel-filled, plane pointer offset by a guard
 * row and a guard column so every written pixel has checked padding around. */
typedef struct {
    uint8_t *mem;
    size_t size;
    ptrdiff_t stride; /* bytes */
    uint8_t *plane;
    int pw, ph; /* valid pixels */
} GuardedPlane;

#define GUARD_ROWS 2
#define GUARD_COLS 22 /* bytes of right-hand per-row padding (kept even) */
#define SENTINEL 0xA5

static void plane_alloc_stride(GuardedPlane *const g, const int pw,
                               const int ph, const int extra_col_bytes) {
    g->pw = pw;
    g->ph = ph;
    g->stride = (ptrdiff_t) pw * bps + extra_col_bytes;
    g->size = (size_t)(ph + 2 * GUARD_ROWS) * (size_t) g->stride;
    g->mem = malloc(g->size);
    CHECK(g->mem != NULL, "out of memory");
    memset(g->mem, SENTINEL, g->size);
    g->plane = g->mem + (size_t) GUARD_ROWS * (size_t) g->stride;
}

static void plane_alloc(GuardedPlane *const g, const int pw, const int ph) {
    plane_alloc_stride(g, pw, ph, GUARD_COLS);
}

/* Allocate with a guaranteed odd byte stride (8 bpc only). */
static void plane_alloc_odd_stride(GuardedPlane *const g, const int pw,
                                   const int ph) {
    const int base = pw * bps;
    const int extra = GUARD_COLS + ((base + GUARD_COLS) & 1 ? 0 : 1);
    plane_alloc_stride(g, pw, ph, extra);
    CHECK((g->stride & 1), "failed to build an odd stride");
}

static void plane_free(GuardedPlane *const g) {
    free(g->mem);
    g->mem = g->plane = NULL;
}

static int plane_guards_intact(const GuardedPlane *const g) {
    const uint8_t *const m = g->mem;
    /* top and bottom guard rows */
    for (int y = 0; y < GUARD_ROWS; y++)
        for (ptrdiff_t k = 0; k < g->stride; k++)
            if (m[(size_t) y * (size_t) g->stride + k] != SENTINEL)
                return 0;
    const size_t bot = (size_t)(GUARD_ROWS + g->ph) * (size_t) g->stride;
    for (size_t k = 0; k < (size_t) GUARD_ROWS * (size_t) g->stride; k++)
        if (m[bot + k] != SENTINEL)
            return 0;
    /* per-row right guard band */
    for (int y = 0; y < g->ph; y++) {
        const uint8_t *row = g->plane + (size_t) y * (size_t) g->stride;
        for (int k = (int) g->pw * bps; k < g->pw * bps + GUARD_COLS; k++)
            if (row[k] != SENTINEL)
                return 0;
    }
    return 1;
}

static int sample_equal(const uint8_t *const a, const uint8_t *const b) {
    if (bps == 1)
        return *a == *b;
    uint16_t va, vb;
    memcpy(&va, a, 2);
    memcpy(&vb, b, 2);
    return va == vb;
}

/* Compare a region output (local coordinates) against the full-frame
 * reference at the picture position (x0, y0). */
static void compare_region(const char *const tag,
                           const GuardedPlane planes[3],
                           const Dav1dPicture *const ref,
                           const Rect *const r)
{
    const int cw = (r->w + 1) >> 1, ch = (r->h + 1) >> 1;
    const int dims[3][2] = {
        { r->w, r->h }, { cw, ch }, { cw, ch },
    };
    const int off[3][2] = {
        { r->x, r->y }, { r->x >> 1, r->y >> 1 }, { r->x >> 1, r->y >> 1 },
    };
    for (int pl = 0; pl < 3; pl++) {
        const uint8_t *rp = (const uint8_t *) ref->data[pl];
        const ptrdiff_t rstride = ref->stride[pl > 0];
        const GuardedPlane *const g = &planes[pl];
        for (int y = 0; y < dims[pl][1]; y++) {
            const uint8_t *src = g->plane + (size_t) y * (size_t) g->stride;
            const uint8_t *gold =
                rp + (size_t)(off[pl][1] + y) * (size_t) rstride
                   + (size_t) off[pl][0] * bps;
            for (int x = 0; x < dims[pl][0]; x++) {
                if (!sample_equal(src + (size_t) x * bps,
                                  gold + (size_t) x * bps)) {
                    CHECK(0, "%s plane %d mismatch at (%d,%d) region=(%d,%d %dx%d)",
                          tag, pl, off[pl][0] + x, off[pl][1] + y,
                          r->x, r->y, r->w, r->h);
                    return;
                }
                pixels_compared++;
            }
        }
    }
}

static int call_region(Dav1dContext *const c, const Dav1dPicture *const p,
                       GuardedPlane planes[3], const Rect *const r)
{
    Dav1dGrainRegion region = {
        .x = r->x, .y = r->y, .w = r->w, .h = r->h,
        .data = { planes[0].plane, planes[1].plane, planes[2].plane },
        .stride = { planes[0].stride, planes[1].stride },
    };
    return dav1d_apply_grain_region(c, p, &region);
}

static void make_region_planes(GuardedPlane planes[3], const Rect *const r) {
    const int cw = (r->w + 1) >> 1, ch = (r->h + 1) >> 1;
    plane_alloc(&planes[0], r->w, r->h);
    plane_alloc(&planes[1], cw, ch);
    plane_alloc(&planes[2], cw, ch);
}

static void free_region_planes(GuardedPlane planes[3]) {
    for (int i = 0; i < 3; i++)
        plane_free(&planes[i]);
}

/* Full picture byte snapshots (valid area + allocator padding). The default
 * allocator pads to a 128-pixel multiple, I420 chroma to half of that. */
typedef struct { uint8_t *data[3]; size_t size[3]; } PicSnapshot;

static void snapshot_take(const Dav1dPicture *const p, PicSnapshot *const s) {
    const int rows[3] = { 128, 64, 64 };
    for (int pl = 0; pl < 3; pl++) {
        s->size[pl] = (size_t) rows[pl] * (size_t) p->stride[pl > 0];
        s->data[pl] = malloc(s->size[pl]);
        CHECK(s->data[pl] != NULL, "out of memory");
        memcpy(s->data[pl], p->data[pl], s->size[pl]);
    }
}

static int snapshot_equal(const Dav1dPicture *const p, const PicSnapshot *const s) {
    const int rows[3] = { 128, 64, 64 };
    for (int pl = 0; pl < 3; pl++)
        if (memcmp(s->data[pl], p->data[pl],
                   (size_t) rows[pl] * (size_t) p->stride[pl > 0]))
            return 0;
    return 1;
}

static void snapshot_free(PicSnapshot *const s) {
    for (int pl = 0; pl < 3; pl++) {
        free(s->data[pl]);
        s->data[pl] = NULL;
    }
}

/* Split the picture into an uneven, block-misaligned tile grid, process the
 * tiles in shuffled order into separate buffers, then stitch them back. The
 * stitched picture must equal the full-frame reference everywhere. */
static void mosaic_test(Dav1dContext *const c, const Dav1dPicture *const p,
                        const Dav1dPicture *const ref, const char *const tag)
{
    static const int xs[] = { 0, 30, 64, 96 };
    static const int ys[] = { 0, 26, 52, 80 };
    enum { NX = 3, NY = 3, NT = NX * NY };

    uint8_t *stitched[3];
    const int pw = p->p.w, ph = p->p.h;
    const int cwp = (pw + 1) >> 1, chp = (ph + 1) >> 1;
    const int plane_w[3] = { pw, cwp, cwp };
    const int plane_h[3] = { ph, chp, chp };
    for (int pl = 0; pl < 3; pl++) {
        stitched[pl] = malloc((size_t) plane_w[pl] * plane_h[pl] * bps);
        CHECK(stitched[pl] != NULL, "out of memory");
        memset(stitched[pl], SENTINEL,
               (size_t) plane_w[pl] * plane_h[pl] * bps);
    }

    /* deterministic shuffle */
    int order[NT];
    for (int i = 0; i < NT; i++)
        order[i] = i;
    unsigned lcg = 0x12345678u;
    for (int i = NT - 1; i > 0; i--) {
        lcg = lcg * 1103515245u + 12345u;
        const int j = (int) ((lcg >> 8) % (unsigned)(i + 1));
        const int tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }

    for (int k = 0; k < NT; k++) {
        const int t = order[k];
        const int tx = t % NX, ty = t / NX;
        Rect r = {
            xs[tx], ys[ty],
            xs[tx + 1] - xs[tx], ys[ty + 1] - ys[ty],
        };
        GuardedPlane planes[3];
        make_region_planes(planes, &r);
        const int res = call_region(c, p, planes, &r);
        CHECK(res == 0, "%s mosaic tile (%d,%d) failed: %d", tag, tx, ty, res);
        const int cw = (r.w + 1) >> 1, ch = (r.h + 1) >> 1;
        const int dims[3][2] = { { r.w, r.h }, { cw, ch }, { cw, ch } };
        const int off[3][2] = {
            { r.x, r.y }, { r.x >> 1, r.y >> 1 }, { r.x >> 1, r.y >> 1 },
        };
        for (int pl = 0; pl < 3; pl++) {
            for (int y = 0; y < dims[pl][1]; y++) {
                const uint8_t *src =
                    planes[pl].plane + (size_t) y * (size_t) planes[pl].stride;
                uint8_t *dst = stitched[pl]
                    + ((size_t)(off[pl][1] + y) * (size_t) plane_w[pl]
                       + (size_t) off[pl][0]) * bps;
                memcpy(dst, src, (size_t) dims[pl][0] * bps);
            }
        }
        free_region_planes(planes);
    }

    for (int pl = 0; pl < 3; pl++) {
        const uint8_t *rp = (const uint8_t *) ref->data[pl];
        const ptrdiff_t rs = ref->stride[pl > 0];
        for (int y = 0; y < plane_h[pl]; y++)
            for (int x = 0; x < plane_w[pl]; x++) {
                const uint8_t *gold = rp + (size_t) y * (size_t) rs
                                      + (size_t) x * bps;
                const uint8_t *got = stitched[pl]
                    + ((size_t) y * (size_t) plane_w[pl] + (size_t) x) * bps;
                if (!sample_equal(gold, got)) {
                    CHECK(0, "%s mosaic mismatch plane %d at (%d,%d)",
                          tag, pl, x, y);
                    goto done;
                }
                pixels_compared++;
            }
    }
done:
    for (int pl = 0; pl < 3; pl++)
        free(stitched[pl]);
}

static void error_suite(Dav1dContext *const c, const Dav1dPicture *const p,
                        const PicSnapshot *const snap)
{
    scenarios++;
    const Rect good = { 4, 4, 16, 16 };
    GuardedPlane planes[3];
    make_region_planes(planes, &good);
    uint8_t *const scratch[3] = { planes[0].plane, planes[1].plane,
                                  planes[2].plane };
    const ptrdiff_t good_stride[2] = { planes[0].stride, planes[1].stride };

    Dav1dGrainRegion base = {
        .x = good.x, .y = good.y, .w = good.w, .h = good.h,
        .data = { scratch[0], scratch[1], scratch[2] },
        .stride = { good_stride[0], good_stride[1] },
    };

    struct { const char *name; Dav1dGrainRegion r; } bad[] = {
        { "zero width",       { .x = 4, .y = 4, .w = 0,  .h = 16, .data = { scratch[0], scratch[1], scratch[2] }, .stride = { good_stride[0], good_stride[1] } } },
        { "zero height",      { .x = 4, .y = 4, .w = 16, .h = 0,  .data = { scratch[0], scratch[1], scratch[2] }, .stride = { good_stride[0], good_stride[1] } } },
        { "negative width",   { .x = 4, .y = 4, .w = -1, .h = 16, .data = { scratch[0], scratch[1], scratch[2] }, .stride = { good_stride[0], good_stride[1] } } },
        { "odd x",            { .x = 5, .y = 4, .w = 16, .h = 16, .data = { scratch[0], scratch[1], scratch[2] }, .stride = { good_stride[0], good_stride[1] } } },
        { "odd y",            { .x = 4, .y = 5, .w = 16, .h = 16, .data = { scratch[0], scratch[1], scratch[2] }, .stride = { good_stride[0], good_stride[1] } } },
        { "negative x",       { .x = -2,.y = 4, .w = 16, .h = 16, .data = { scratch[0], scratch[1], scratch[2] }, .stride = { good_stride[0], good_stride[1] } } },
        { "x out of picture", { .x = 96,.y = 0, .w = 1,  .h = 16, .data = { scratch[0], scratch[1], scratch[2] }, .stride = { good_stride[0], good_stride[1] } } },
        { "y out of picture", { .x = 0, .y = 80,.w = 16, .h = 1,  .data = { scratch[0], scratch[1], scratch[2] }, .stride = { good_stride[0], good_stride[1] } } },
        { "overflow add",     { .x = INT_MAX - 1, .y = 0, .w = 16, .h = 16, .data = { scratch[0], scratch[1], scratch[2] }, .stride = { good_stride[0], good_stride[1] } } },
        { "null y plane",     { .x = 4, .y = 4, .w = 16, .h = 16, .data = { NULL, scratch[1], scratch[2] }, .stride = { good_stride[0], good_stride[1] } } },
        { "null u plane",     { .x = 4, .y = 4, .w = 16, .h = 16, .data = { scratch[0], NULL, scratch[2] }, .stride = { good_stride[0], good_stride[1] } } },
        { "null v plane",     { .x = 4, .y = 4, .w = 16, .h = 16, .data = { scratch[0], scratch[1], NULL }, .stride = { good_stride[0], good_stride[1] } } },
        { "zero luma stride", { .x = 4, .y = 4, .w = 16, .h = 16, .data = { scratch[0], scratch[1], scratch[2] }, .stride = { 0, good_stride[1] } } },
        { "neg chroma stride",{ .x = 4, .y = 4, .w = 16, .h = 16, .data = { scratch[0], scratch[1], scratch[2] }, .stride = { good_stride[0], -4 } } },
        { "small luma stride",{ .x = 4, .y = 4, .w = 16, .h = 16, .data = { scratch[0], scratch[1], scratch[2] }, .stride = { 16 * bps - 1, good_stride[1] } } },
        { "small uv stride",  { .x = 4, .y = 4, .w = 16, .h = 16, .data = { scratch[0], scratch[1], scratch[2] }, .stride = { good_stride[0], 8 * bps - 1 } } },
        { "odd luma stride",  { .x = 4, .y = 4, .w = 16, .h = 16, .data = { scratch[0], scratch[1], scratch[2] }, .stride = { 16 * bps + 1, good_stride[1] } } },
        { "odd uv stride",    { .x = 4, .y = 4, .w = 16, .h = 16, .data = { scratch[0], scratch[1], scratch[2] }, .stride = { good_stride[0], 8 * bps + 1 } } },
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        /* Even stride is only mandatory for the uint16_t 10 bpc storage. */
        if (bpc == 8 &&
            (!strcmp(bad[i].name, "odd luma stride") ||
             !strcmp(bad[i].name, "odd uv stride")))
            continue;
        const int res = dav1d_apply_grain_region(c, p, &bad[i].r);
        CHECK(res == DAV1D_ERR(EINVAL), "case %s: expected EINVAL, got %d",
              bad[i].name, res);
        for (int pl = 0; pl < 3; pl++)
            CHECK(plane_guards_intact(&planes[pl]),
                  "case %s wrote plane %d before rejection", bad[i].name, pl);
        CHECK(snapshot_equal(p, snap),
              "case %s modified the input picture", bad[i].name);
    }

    /* NULL pointers */
    CHECK(dav1d_apply_grain_region(NULL, p, &base) == DAV1D_ERR(EINVAL),
          "null context not rejected");
    CHECK(dav1d_apply_grain_region(c, NULL, &base) == DAV1D_ERR(EINVAL),
          "null picture not rejected");
    CHECK(dav1d_apply_grain_region(c, p, NULL) == DAV1D_ERR(EINVAL),
          "null region not rejected");

    /* Unsupported layout / bit depth: craft minimal pictures that fail the
     * capability check before any pixel access. */
    Dav1dPicture fake = *p;
    Dav1dFrameHeader fh = *p->frame_hdr;
    fake.frame_hdr = &fh;
    fake.p.layout = DAV1D_PIXEL_LAYOUT_I444;
    CHECK(dav1d_apply_grain_region(c, &fake, &base) == DAV1D_ERR(ENOSYS),
          "I444 not rejected with ENOSYS");
    fake = *p;
    fake.frame_hdr = &fh;
    fake.p.bpc = 12;
    CHECK(dav1d_apply_grain_region(c, &fake, &base) == DAV1D_ERR(ENOSYS),
          "12 bpc not rejected with ENOSYS");

    free_region_planes(planes);
}

static Dav1dPicture decode_fixture(const char *const path,
                                   Dav1dContext **const c_out)
{
    FILE *const f = fopen(path, "rb");
    CHECK(f != NULL, "cannot open %s", path);
    unsigned char head[44];
    if (fread(head, 1, 44, f) != 44)
        CHECK(0, "short ivf header");
    const unsigned n = head[32] | head[33] << 8 | head[34] << 16 |
                       (unsigned) head[35] << 24;
    Dav1dData data = { 0 };
    uint8_t *const buf = dav1d_data_create(&data, n);
    CHECK(buf != NULL, "dav1d_data_create failed");
    if (fread(buf, 1, n, f) != n)
        CHECK(0, "short ivf payload");
    fclose(f);

    Dav1dSettings s;
    dav1d_default_settings(&s);
    s.n_threads = 1;
    s.apply_grain = 0;
    Dav1dContext *c = NULL;
    CHECK(dav1d_open(&c, &s) == 0, "dav1d_open failed");
    CHECK(dav1d_send_data(c, &data) == 0, "dav1d_send_data failed");
    Dav1dPicture p = { 0 };
    CHECK(dav1d_get_picture(c, &p) == 0, "dav1d_get_picture failed");
    CHECK(p.p.w == 96 && p.p.h == 80, "unexpected fixture dimensions");
    CHECK(p.p.layout == DAV1D_PIXEL_LAYOUT_I420, "fixture is not I420");
    *c_out = c;
    return p;
}

int main(int argc, char **argv) {
#ifndef GRAIN_REGION_HAS_ASAN
    signal(SIGSEGV, crash_handler);
    signal(SIGTRAP, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGBUS, crash_handler);
#endif
    if (argc != 3) {
        fprintf(stderr, "usage: %s <ivf> <bpc>\n", argv[0]);
        return 2;
    }
    bpc = atoi(argv[2]);
    bps = bpc > 8 ? 2 : 1;
    if (bpc != 8 && bpc != 10) {
        fprintf(stderr, "unsupported bpc %d\n", bpc);
        return 2;
    }

    Dav1dContext *c = NULL;
    Dav1dPicture p = decode_fixture(argv[1], &c);
    if (failures)
        goto out;
    CHECK(p.p.bpc == bpc, "fixture bpc %d != expected %d", p.p.bpc, bpc);

#ifndef GRAIN_REGION_HAS_ASAN
    /* Prove the interposer really sees library allocations: opening a fresh
     * decoder unconditionally allocates. */
    {
        Dav1dContext *c2 = NULL;
        Dav1dSettings s2;
        dav1d_default_settings(&s2);
        s2.n_threads = 1;
        grain_region_alloc_events = 0;
        grain_region_alloc_bytes = 0;
        grain_region_observing = 1;
        const int o = dav1d_open(&c2, &s2);
        grain_region_observing = 0;
        CHECK(o == 0, "interposer self-check dav1d_open failed: %d", o);
        CHECK(grain_region_alloc_events > 0,
              "allocation interposer observed no library allocations");
        if (c2)
            dav1d_close(&c2);
    }
#endif

    Dav1dFrameHeader *const orig_hdr = p.frame_hdr;
    Dav1dFrameHeader hdr_copy = *orig_hdr;
    PicSnapshot snap;
    snapshot_take(&p, &snap);

    /* The decoder's reference pool may already hold multiple references to
     * the frame, so the absolute count is not 1; the region API must simply
     * leave every reference count exactly as it found it. */
    const int ref_before = atomic_load(&p.ref->ref_cnt);
    const int fh_ref_before =
        p.frame_hdr_ref ? atomic_load(&p.frame_hdr_ref->ref_cnt) : 0;
    const int sh_ref_before =
        p.seq_hdr_ref ? atomic_load(&p.seq_hdr_ref->ref_cnt) : 0;

    for (int vi = 0; vi < N_VARIANTS; vi++) {
        const GrainVariant *const v = &variants[vi];
        Dav1dFrameHeader h = hdr_copy;
        configure_grain(&h, v);
        if (!v->y_grain && !v->u_grain && !v->v_grain &&
            !v->chroma_from_luma)
            h.film_grain.present = 0;
        p.frame_hdr = &h;

        /* Full-frame reference through the legacy entry point. */
        Dav1dPicture ref = { 0 };
        const int gr = dav1d_apply_grain(c, &ref, &p);
        CHECK(gr == 0, "full-frame grain failed for %s: %d", v->name, gr);
        if (gr)
            continue;

        for (int ri = 0; ri < N_REGIONS; ri++) {
            const Rect *const r = &regions[ri];
            GuardedPlane planes[3];
            make_region_planes(planes, r);

#ifndef GRAIN_REGION_HAS_ASAN
            grain_region_alloc_events = 0;
            grain_region_alloc_bytes = 0;
            grain_region_observing = 1;
#endif
            const int res = call_region(c, &p, planes, r);
#ifndef GRAIN_REGION_HAS_ASAN
            grain_region_observing = 0;
#endif
            CHECK(res == 0, "region call failed for %s: %d", v->name, res);

            scenarios++;
            char tag[128];
            snprintf(tag, sizeof(tag), "%s/r%dx%d-%dx%d", v->name,
                     r->x, r->y, r->w, r->h);
            compare_region(tag, planes, &ref, r);

            /* Allocation observation: the region path may only use fixed-size
             * stack tables plus the caller's output buffers, never a
             * picture-sized (or region-sized) temporary pixel plane, so it
             * must perform zero heap allocations. */
#ifndef GRAIN_REGION_HAS_ASAN
            CHECK(grain_region_alloc_events == 0,
                  "%s performed %lu unexpected scratch allocations", tag,
                  grain_region_alloc_events);
            CHECK(grain_region_alloc_bytes == 0,
                  "%s allocated %zu scratch bytes", tag,
                  grain_region_alloc_bytes);
#endif

            for (int pl = 0; pl < 3; pl++)
                CHECK(plane_guards_intact(&planes[pl]),
                      "%s clobbered padding of plane %d", tag, pl);
            CHECK(snapshot_equal(&p, &snap),
                  "%s modified the input picture", tag);

            free_region_planes(planes);
        }

        scenarios++;
        mosaic_test(c, &p, &ref, v->name);
        CHECK(snapshot_equal(&p, &snap),
              "%s mosaic modified the input picture", v->name);

        /* Odd byte strides are legal for the byte-packed 8 bpc storage; make
         * sure a deliberately odd stride still matches the reference. */
        if (bpc == 8) {
            const Rect r = { 12, 12, 37, 33 };
            GuardedPlane odd[3];
            const int cw = (r.w + 1) >> 1, ch = (r.h + 1) >> 1;
            plane_alloc_odd_stride(&odd[0], r.w, r.h);
            plane_alloc_odd_stride(&odd[1], cw, ch);
            plane_alloc_odd_stride(&odd[2], cw, ch);
            Dav1dGrainRegion gr = {
                .x = r.x, .y = r.y, .w = r.w, .h = r.h,
                .data = { odd[0].plane, odd[1].plane, odd[2].plane },
                .stride = { odd[0].stride, odd[1].stride },
            };
            const int res = dav1d_apply_grain_region(c, &p, &gr);
            CHECK(res == 0, "odd-stride call failed for %s: %d", v->name, res);
            scenarios++;
            char otag[128];
            snprintf(otag, sizeof(otag), "%s/odd-stride", v->name);
            compare_region(otag, odd, &ref, &r);
            for (int pl = 0; pl < 3; pl++)
                CHECK(plane_guards_intact(&odd[pl]),
                      "%s clobbered padding of plane %d", otag, pl);
            free_region_planes(odd);
        }

        dav1d_picture_unref(&ref);
    }

    /* Error handling uses the first (grain-present) configuration. */
    Dav1dFrameHeader eh = hdr_copy;
    configure_grain(&eh, &variants[0]);
    p.frame_hdr = &eh;
    error_suite(c, &p, &snap);

    /* The input header content and the reference counts are untouched. */
    CHECK(memcmp(&hdr_copy, orig_hdr, sizeof(hdr_copy)) == 0,
          "input frame header was modified");
    CHECK(atomic_load(&p.ref->ref_cnt) == ref_before,
          "input picture refcount changed: %d -> %d",
          ref_before, atomic_load(&p.ref->ref_cnt));
    if (p.frame_hdr_ref)
        CHECK(atomic_load(&p.frame_hdr_ref->ref_cnt) == fh_ref_before,
              "frame header refcount changed: %d -> %d",
              fh_ref_before, atomic_load(&p.frame_hdr_ref->ref_cnt));
    if (p.seq_hdr_ref)
        CHECK(atomic_load(&p.seq_hdr_ref->ref_cnt) == sh_ref_before,
              "sequence header refcount changed: %d -> %d",
              sh_ref_before, atomic_load(&p.seq_hdr_ref->ref_cnt));
    CHECK(snapshot_equal(&p, &snap), "input picture changed by error suite");

    snapshot_free(&snap);
    p.frame_hdr = orig_hdr;

out:
    dav1d_picture_unref(&p);
    if (c)
        dav1d_close(&c);

    if (failures) {
        fprintf(stderr, "GRAIN_REGION_FAIL failures=%d scenarios=%u "
                "pixels=%llu\n", failures, scenarios, pixels_compared);
        return 1;
    }
    if (!scenarios || !pixels_compared) {
        fprintf(stderr, "GRAIN_REGION_FAIL empty test run\n");
        return 1;
    }
    printf("grain region: scenarios=%u pixels_compared=%llu "
           "GRAIN_REGION_PASS\n", scenarios, pixels_compared);
    return 0;
}
