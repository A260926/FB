/*
 * gray_contract.c -- numerical contract tests for Leptonica's grayscale
 * reconstruction chain:
 *
 *     pixSeedfillGray(), pixSeedfillGraySimple(),
 *     pixHDome(), pixSeedfillGrayBasin()
 *
 * Every expected value is computed by the independent reference model
 * in gray_ref.h (threshold-set definition, own arrays and graph
 * traversal); no library output is ever used as ground truth.
 * See checks/gray_contract.md for the rationale and coverage notes.
 *
 * Cases are registered through contract_cases() in three groups:
 *   thresholds - core reconstruction semantics of the two seedfill impls
 *   storage    - PIX row padding / tail-byte behavior
 *   derived    - pixHDome and pixSeedfillGrayBasin
 *
 * On any numeric deviation a case prints GRAY_MISMATCH with the first
 * bad coordinate and the expected/actual values, plus a GRAY_REPLAY
 * dump of the raw input arrays (directly re-runnable), and returns 1.
 * On success it returns 0; no case is ever skipped.
 *
 * All material is generated in C; no external images or codecs are used.
 * Random generators use a fixed seed and an explicit small size cap
 * (GRAY_RAND_MAX_DIM).
 */

#include "allheaders.h"
#include "arrayaccess.h"
#include "contract.h"
#include "gray_ref.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Explicit small-size cap for every randomly generated image. */
#define GRAY_RAND_MAX_DIM 33

/* ------------------------------------------------------------------ *
 *  Deterministic PRNG (xorshift32); each random case reseeds itself  *
 * ------------------------------------------------------------------ */
static uint32_t gray_rng_state;

static void
gray_rng_seed(uint32_t seed)
{
    gray_rng_state = seed ? seed : 0x1234567u;
}

static uint32_t
gray_rng_next(void)
{
    uint32_t x = gray_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    gray_rng_state = x;
    return x;
}

static uint32_t
gray_rng_below(uint32_t n)
{
    return n ? gray_rng_next() % n : 0;
}

/* ------------------------------------------------------------------ *
 *  Failure reporting                                                  *
 * ------------------------------------------------------------------ */
typedef struct {
    const char    *cname;   /* case name                        */
    const char    *func;    /* library function under test      */
    const char    *what;    /* quantity that deviated           */
    int            conn;    /* connectivity, 0 if n/a           */
    int            w, h;    /* image size                       */
    int            height;  /* pixHDome parameter, -1 if n/a    */
    int            delta;   /* basin parameter, -1 if n/a       */
    const uint8_t *seed;    /* replay arrays, NULL when absent  */
    const uint8_t *mask;
    const uint8_t *bin;
} gray_ctx;

static void
gray_dump_array(const char *tag, const uint8_t *a, int n)
{
    int i;
    fprintf(stderr, "GRAY_REPLAY %s[%d]={", tag, n);
    for (i = 0; i < n; i++)
        fprintf(stderr, "%s%u", i ? "," : "", (unsigned)a[i]);
    fprintf(stderr, "};\n");
}

static int
gray_mismatch(const gray_ctx *cx, int x, int y, int expected, int actual)
{
    fprintf(stderr,
            "GRAY_MISMATCH case=%s func=%s what=%s conn=%d x=%d y=%d "
            "expected=%d actual=%d\n",
            cx->cname, cx->func, cx->what, cx->conn, x, y, expected, actual);
    fprintf(stderr, "GRAY_REPLAY w=%d h=%d conn=%d height=%d delta=%d\n",
            cx->w, cx->h, cx->conn, cx->height, cx->delta);
    if (cx->seed)
        gray_dump_array("seed", cx->seed, cx->w * cx->h);
    if (cx->mask)
        gray_dump_array("mask", cx->mask, cx->w * cx->h);
    if (cx->bin)
        gray_dump_array("bin", cx->bin, cx->w * cx->h);
    return 1;
}

/* ------------------------------------------------------------------ *
 *  PIX helpers                                                       *
 * ------------------------------------------------------------------ */
static PIX *
gray_pix_from_array(int w, int h, int depth, const uint8_t *a)
{
    PIX *pix = pixCreate(w, h, depth);
    int x, y;
    if (!pix)
        return NULL;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            pixSetPixel(pix, x, y, (l_uint32)a[y * w + x]);
    return pix;
}

static void
gray_pix_to_array(PIX *pix, uint8_t *out)
{
    l_int32 w, h;
    int x, y;
    l_uint32 v;
    pixGetDimensions(pix, &w, &h, NULL);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            pixGetPixel(pix, x, y, &v);
            out[y * w + x] = (uint8_t)v;
        }
}

static size_t
gray_pix_data_size(PIX *pix)
{
    return (size_t)pixGetWpl(pix) * 4u * (size_t)pixGetHeight(pix);
}

/* Write a salt-dependent pattern into the invalid tail bytes of every
 * row (byte indexes [w, 4*wpl)).  Different salts for seed and mask. */
static void
gray_scribble_padding(PIX *pix, uint32_t salt)
{
    l_int32 w, h, wpl;
    l_uint32 *data, *line;
    int i, j, pad;

    pixGetDimensions(pix, &w, &h, NULL);
    wpl = pixGetWpl(pix);
    data = pixGetData(pix);
    for (i = 0; i < h; i++) {
        line = data + i * wpl;
        for (j = w; j < 4 * wpl; j++) {
            pad = (int)(salt ^ (uint32_t)(i * 31 + j * 17 + 5)) & 0xff;
            SET_DATA_BYTE(line, j, pad);
        }
    }
}

/* Compare the invalid tail bytes of pix against a raw snapshot. */
static int
gray_padding_differs(PIX *pix, const uint8_t *snap, int *pi, int *pj,
                     int *expected, int *actual)
{
    l_int32 w, h, wpl;
    l_uint32 *data, *line;
    const l_uint32 *sline;
    int i, j, e, a;

    pixGetDimensions(pix, &w, &h, NULL);
    wpl = pixGetWpl(pix);
    data = pixGetData(pix);
    for (i = 0; i < h; i++) {
        line = data + i * wpl;
        sline = (const l_uint32 *)(const void *)(snap + (size_t)i * wpl * 4u);
        for (j = w; j < 4 * wpl; j++) {
            e = GET_DATA_BYTE(sline, j);
            a = GET_DATA_BYTE(line, j);
            if (e != a) {
                *pi = i;
                *pj = j;
                *expected = e;
                *actual = a;
                return 1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Core check: run both fill implementations on (seed, mask, conn)   *
 *  and compare every valid pixel with the reference model.  Also     *
 *  verifies that the mask (valid + padding bytes) is not rewritten   *
 *  and that garbage in the seed padding does not leak into the       *
 *  valid region and is itself left alone.                            *
 * ------------------------------------------------------------------ */
static int
gray_run_fill(const char *cname, int w, int h, const uint8_t *seed,
              const uint8_t *mask, int conn, int scribble)
{
    static const char *const fname[2] = {
        "pixSeedfillGray", "pixSeedfillGraySimple"
    };
    const int n = w * h;
    int fn, ret = 0;
    uint8_t *ref = NULL, *actual = NULL, *msnap = NULL, *ssnap = NULL;
    gray_ctx cx;

    if (!gray_ref_legal(seed, mask, n)) {
        fprintf(stderr, "GRAY_TEST_BUG case=%s illegal seed>mask\n", cname);
        return 1;
    }
    ref = (uint8_t *)malloc((size_t)n);
    actual = (uint8_t *)malloc((size_t)n);
    gray_ref_reconstruct(seed, mask, w, h, conn, ref);

    for (fn = 0; fn < 2 && !ret; fn++) {
        PIX *ps = gray_pix_from_array(w, h, 8, seed);
        PIX *pm = gray_pix_from_array(w, h, 8, mask);
        size_t dsz;
        int x, y, bad = 0;

        if (!ps || !pm) {
            fprintf(stderr, "GRAY_TEST_BUG case=%s pixCreate failed\n", cname);
            pixDestroy(&ps);
            pixDestroy(&pm);
            ret = 1;
            break;
        }
        if (scribble) {
            gray_scribble_padding(ps, 0xa5u);   /* seed padding pattern */
            gray_scribble_padding(pm, 0x3cu);   /* mask padding pattern */
        }
        dsz = gray_pix_data_size(pm);
        msnap = (uint8_t *)malloc(dsz);
        ssnap = (uint8_t *)malloc(dsz);
        memcpy(msnap, pixGetData(pm), dsz);
        memcpy(ssnap, pixGetData(ps), dsz);

        if (fn == 0)
            pixSeedfillGray(ps, pm, conn);
        else
            pixSeedfillGraySimple(ps, pm, conn);

        gray_pix_to_array(ps, actual);

        cx.cname = cname;
        cx.func = fname[fn];
        cx.what = "pixels";
        cx.conn = conn;
        cx.w = w;
        cx.h = h;
        cx.height = -1;
        cx.delta = -1;
        cx.seed = seed;
        cx.mask = mask;
        cx.bin = NULL;

        for (y = 0; y < h && !bad; y++)
            for (x = 0; x < w && !bad; x++)
                if (actual[y * w + x] != ref[y * w + x])
                    bad = gray_mismatch(&cx, x, y, ref[y * w + x],
                                        actual[y * w + x]);

            /* The mask must be byte-identical afterwards, padding
             * included: it is a read-only input of the operation. */
        if (!bad && memcmp(msnap, pixGetData(pm), dsz) != 0) {
            const uint8_t *now = (const uint8_t *)pixGetData(pm);
            const int rowbytes = 4 * pixGetWpl(pm);
            size_t k;
            for (k = 0; k < dsz && msnap[k] == now[k]; k++)
                ;
            cx.what = "mask-data";
            bad = gray_mismatch(&cx, (int)(k % (size_t)rowbytes),
                                (int)(k / (size_t)rowbytes),
                                msnap[k], now[k]);
        }

            /* Garbage written into the seed padding must stay there:
             * it must neither influence the valid region (checked
             * above) nor be rewritten by the fill. */
        if (!bad && scribble) {
            int pi, pj, pe, pa;
            if (gray_padding_differs(ps, ssnap, &pi, &pj, &pe, &pa)) {
                cx.what = "seed-padding";
                bad = gray_mismatch(&cx, pj, pi, pe, pa);
            }
        }

        free(msnap);
        free(ssnap);
        msnap = ssnap = NULL;
        pixDestroy(&ps);
        pixDestroy(&pm);
        if (bad)
            ret = 1;
    }

    free(ref);
    free(actual);
    return ret;
}

/* ------------------------------------------------------------------ *
 *  Derived checks: pixHDome / pixSeedfillGrayBasin vs the model      *
 * ------------------------------------------------------------------ */
static int
gray_run_hdome(const char *cname, int w, int h, const uint8_t *mask,
               int height, int conn)
{
    const int n = w * h;
    int ret = 0, x, y;
    uint8_t *model = (uint8_t *)malloc((size_t)n);
    uint8_t *actual = (uint8_t *)malloc((size_t)n);
    uint8_t *msnap;
    PIX *pm, *out;
    gray_ctx cx;

    gray_ref_hdome(mask, w, h, height, conn, model);
    pm = gray_pix_from_array(w, h, 8, mask);
    msnap = (uint8_t *)malloc(gray_pix_data_size(pm));
    memcpy(msnap, pixGetData(pm), gray_pix_data_size(pm));

    out = pixHDome(pm, height, conn);
    if (!out) {
        fprintf(stderr, "GRAY_MISMATCH case=%s func=pixHDome what=null-return "
                "conn=%d x=-1 y=-1 expected=0 actual=-1\n", cname, conn);
        free(model);
        free(actual);
        free(msnap);
        pixDestroy(&pm);
        return 1;
    }
    gray_pix_to_array(out, actual);

    cx.cname = cname;
    cx.func = "pixHDome";
    cx.what = "pixels";
    cx.conn = conn;
    cx.w = w;
    cx.h = h;
    cx.height = height;
    cx.delta = -1;
    cx.seed = NULL;
    cx.mask = mask;
    cx.bin = NULL;

    for (y = 0; y < h && !ret; y++)
        for (x = 0; x < w && !ret; x++)
            if (actual[y * w + x] != model[y * w + x])
                ret = gray_mismatch(&cx, x, y, model[y * w + x],
                                    actual[y * w + x]);

        /* The caller's filling mask must be left exactly as it was. */
    if (!ret && memcmp(msnap, pixGetData(pm), gray_pix_data_size(pm)) != 0) {
        const uint8_t *now = (const uint8_t *)pixGetData(pm);
        const int rowbytes = 4 * pixGetWpl(pm);
        size_t k, dsz = gray_pix_data_size(pm);
        for (k = 0; k < dsz && msnap[k] == now[k]; k++)
            ;
        cx.what = "input-mask";
        ret = gray_mismatch(&cx, (int)(k % (size_t)rowbytes),
                            (int)(k / (size_t)rowbytes), msnap[k], now[k]);
    }

    free(model);
    free(actual);
    free(msnap);
    pixDestroy(&out);
    pixDestroy(&pm);
    return ret;
}

static int
gray_run_basin(const char *cname, int w, int h, const uint8_t *bin,
               const uint8_t *mask, int delta, int conn)
{
    const int n = w * h;
    int ret = 0, x, y;
    uint8_t *model = (uint8_t *)malloc((size_t)n);
    uint8_t *actual = (uint8_t *)malloc((size_t)n);
    uint8_t *msnap, *bsnap;
    PIX *pm, *pb, *out;
    gray_ctx cx;

    gray_ref_basin(bin, mask, w, h, delta, conn, model);
    pm = gray_pix_from_array(w, h, 8, mask);
    pb = gray_pix_from_array(w, h, 1, bin);
    msnap = (uint8_t *)malloc(gray_pix_data_size(pm));
    bsnap = (uint8_t *)malloc(gray_pix_data_size(pb));
    memcpy(msnap, pixGetData(pm), gray_pix_data_size(pm));
    memcpy(bsnap, pixGetData(pb), gray_pix_data_size(pb));

    out = pixSeedfillGrayBasin(pb, pm, delta, conn);
    if (!out) {
        fprintf(stderr, "GRAY_MISMATCH case=%s func=pixSeedfillGrayBasin "
                "what=null-return conn=%d x=-1 y=-1 expected=0 actual=-1\n",
                cname, conn);
        free(model);
        free(actual);
        free(msnap);
        free(bsnap);
        pixDestroy(&pm);
        pixDestroy(&pb);
        return 1;
    }
    gray_pix_to_array(out, actual);

    cx.cname = cname;
    cx.func = "pixSeedfillGrayBasin";
    cx.what = "pixels";
    cx.conn = conn;
    cx.w = w;
    cx.h = h;
    cx.height = -1;
    cx.delta = delta;
    cx.seed = NULL;
    cx.mask = mask;
    cx.bin = bin;

    for (y = 0; y < h && !ret; y++)
        for (x = 0; x < w && !ret; x++)
            if (actual[y * w + x] != model[y * w + x])
                ret = gray_mismatch(&cx, x, y, model[y * w + x],
                                    actual[y * w + x]);

        /* Both caller inputs (binary seed locations and gray mask)
         * must be preserved byte for byte. */
    if (!ret && memcmp(msnap, pixGetData(pm), gray_pix_data_size(pm)) != 0) {
        const uint8_t *now = (const uint8_t *)pixGetData(pm);
        const int rowbytes = 4 * pixGetWpl(pm);
        size_t k, dsz = gray_pix_data_size(pm);
        for (k = 0; k < dsz && msnap[k] == now[k]; k++)
            ;
        cx.what = "input-mask";
        ret = gray_mismatch(&cx, (int)(k % (size_t)rowbytes),
                            (int)(k / (size_t)rowbytes), msnap[k], now[k]);
    }
    if (!ret && memcmp(bsnap, pixGetData(pb), gray_pix_data_size(pb)) != 0) {
        const uint8_t *now = (const uint8_t *)pixGetData(pb);
        const int rowbytes = 4 * pixGetWpl(pb);
        size_t k, dsz = gray_pix_data_size(pb);
        for (k = 0; k < dsz && bsnap[k] == now[k]; k++)
            ;
        cx.what = "input-bin";
        ret = gray_mismatch(&cx, (int)(k % (size_t)rowbytes),
                            (int)(k / (size_t)rowbytes), bsnap[k], now[k]);
    }

    free(model);
    free(actual);
    free(msnap);
    free(bsnap);
    pixDestroy(&out);
    pixDestroy(&pm);
    pixDestroy(&pb);
    return ret;
}

/* ------------------------------------------------------------------ *
 *  Random image generation (fixed seed, size cap)                    *
 * ------------------------------------------------------------------ */
static void
gray_rand_mask(uint8_t *mask, int w, int h, int style)
{
    const int n = w * h;
    static const uint8_t extremes[7] = { 0, 1, 2, 127, 253, 254, 255 };
    static const uint8_t levels[5] = { 0, 64, 128, 192, 255 };
    int p, r;

    switch (style) {
    case 0:  /* uniform noise */
        for (p = 0; p < n; p++)
            mask[p] = (uint8_t)gray_rng_below(256);
        break;
    case 1:  /* extremes near 0/1/254/255 */
        for (p = 0; p < n; p++)
            mask[p] = extremes[gray_rng_below(7)];
        break;
    case 2:  /* plateaus: flat level plus stamped rectangles */
        for (p = 0; p < n; p++)
            mask[p] = levels[gray_rng_below(5)];
        for (r = 0; r < 6; r++) {
            int rw = 1 + (int)gray_rng_below((uint32_t)(w > 8 ? 8 : w));
            int rh = 1 + (int)gray_rng_below((uint32_t)(h > 8 ? 8 : h));
            int rx = (int)gray_rng_below((uint32_t)(w - rw + 1));
            int ry = (int)gray_rng_below((uint32_t)(h - rh + 1));
            uint8_t v = (uint8_t)gray_rng_below(256);
            int x, y;
            for (y = ry; y < ry + rh; y++)
                for (x = rx; x < rx + rw; x++)
                    mask[y * w + x] = v;
        }
        break;
    default:  /* smooth surface plus sparse spikes */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                mask[y * w + x] =
                    (uint8_t)((x * 5 + y * 7 + (int)gray_rng_below(9)) & 0xff);
        for (r = 0; r < 4; r++)
            mask[gray_rng_below((uint32_t)n)] =
                (uint8_t)(240 + gray_rng_below(16));
        break;
    }
}

static void
gray_rand_seed(uint8_t *seed, const uint8_t *mask, int n, int sparse)
{
    int p;
    if (sparse) {
        memset(seed, 0, (size_t)n);
        for (p = 0; p < n; p++)
            if (gray_rng_below(16) == 0)
                seed[p] = (uint8_t)gray_rng_below((uint32_t)mask[p] + 1u);
    } else {
        for (p = 0; p < n; p++)
            seed[p] = (uint8_t)gray_rng_below((uint32_t)mask[p] + 1u);
    }
}

/* ------------------------------------------------------------------ *
 *  thresholds group                                                  *
 * ------------------------------------------------------------------ */

/* Single-pixel-wide maze with several turns.  The fill cannot finish
 * after the raster/anti-raster scan pair: the last leg must be reached
 * by the propagation phase, and the final turn lies on the left image
 * border (a downward step at column 0). */
static int
case_maze_left_edge(void)
{
    static const uint8_t mask[5 * 6] = {
        200, 200, 200, 200, 200, 200,
          0,   0,   0,   0,   0, 200,
        200, 200, 200, 200, 200, 200,
        200,   0,   0,   0,   0,   0,
        200, 200, 200, 200, 200, 200
    };
    uint8_t seed[5 * 6];
    memset(seed, 0, sizeof(seed));
    seed[0] = 150;  /* (x=0, y=0) */
    if (gray_run_fill("maze_left_edge", 6, 5, seed, mask, 4, 0))
        return 1;
    if (gray_run_fill("maze_left_edge", 6, 5, seed, mask, 8, 0))
        return 1;
    return 0;
}

/* Same maze mirrored: the final turn lies on the right image border. */
static int
case_maze_right_edge(void)
{
    static const uint8_t mask[5 * 6] = {
        200, 200, 200, 200, 200, 200,
        200,   0,   0,   0,   0,   0,
        200, 200, 200, 200, 200, 200,
          0,   0,   0,   0,   0, 200,
        200, 200, 200, 200, 200, 200
    };
    uint8_t seed[5 * 6];
    memset(seed, 0, sizeof(seed));
    seed[5] = 150;  /* (x=5, y=0) */
    if (gray_run_fill("maze_right_edge", 6, 5, seed, mask, 4, 0))
        return 1;
    if (gray_run_fill("maze_right_edge", 6, 5, seed, mask, 8, 0))
        return 1;
    return 0;
}

/* Two plateaus joined only by a diagonal touch: the unique passage
 * exists for 8-connectivity but not for 4-connectivity, so the two
 * connectivities must give different results. */
static int
case_diagonal_passage(void)
{
    static const uint8_t mask[5 * 5] = {
        200, 200,   0,   0,   0,
        200, 200,   0,   0,   0,
          0,   0, 200, 200, 200,
          0,   0, 200, 200, 200,
          0,   0, 200, 200, 200
    };
    uint8_t seed[5 * 5];
    uint8_t r4[5 * 5], r8[5 * 5];
    int p, differ = 0;

    memset(seed, 0, sizeof(seed));
    seed[0] = 150;
    if (gray_run_fill("diagonal_passage", 5, 5, seed, mask, 4, 0))
        return 1;
    if (gray_run_fill("diagonal_passage", 5, 5, seed, mask, 8, 0))
        return 1;
    gray_ref_reconstruct(seed, mask, 5, 5, 4, r4);
    gray_ref_reconstruct(seed, mask, 5, 5, 8, r8);
    for (p = 0; p < 25; p++)
        if (r4[p] != r8[p])
            differ = 1;
    if (!differ) {
        fprintf(stderr, "GRAY_MISMATCH case=diagonal_passage "
                "func=model what=conn-difference conn=0 x=-1 y=-1 "
                "expected=1 actual=0\n");
        return 1;
    }
    return 0;
}

/* Wide flat platform carrying several seeds of different heights,
 * surrounded by a lower ring that clips the fill. */
static int
case_platform_multi_seed(void)
{
    uint8_t mask[5 * 9], seed[5 * 9];
    int x, y;

    for (y = 0; y < 5; y++)
        for (x = 0; x < 9; x++)
            mask[y * 9 + x] =
                (x == 0 || y == 0 || x == 8 || y == 4) ? 45 : 180;
    memset(seed, 0, sizeof(seed));
    seed[2 * 9 + 2] = 120;  /* (2,2) */
    seed[3 * 9 + 6] = 60;   /* (6,3) */
    seed[1 * 9 + 4] = 90;   /* (4,1) */
    if (gray_run_fill("platform_multi_seed", 9, 5, seed, mask, 4, 0))
        return 1;
    if (gray_run_fill("platform_multi_seed", 9, 5, seed, mask, 8, 0))
        return 1;
    return 0;
}

/* Sombrero rims with saddles at different heights: the fill leaks out
 * through the highest rim point, so the outside level is set by the
 * highest saddle, not by the lowest or the average one. */
static int
case_saddles(void)
{
    uint8_t mask[9 * 11], seed[9 * 11];
    int x, y, on_rim;

    for (y = 0; y < 9; y++)
        for (x = 0; x < 11; x++)
            mask[y * 11 + x] = 220;             /* outside, high */
    for (y = 3; y <= 5; y++)
        for (x = 4; x <= 6; x++)
            mask[y * 11 + x] = 240;             /* inside */
    for (y = 2; y <= 6; y++)
        for (x = 3; x <= 7; x++) {
            on_rim = (x == 3 || x == 7 || y == 2 || y == 6);
            if (on_rim)
                mask[y * 11 + x] = 80;          /* rim base level */
        }
    mask[2 * 11 + 3] = 130;                     /* saddle, mid height */
    mask[2 * 11 + 4] = 130;
    mask[2 * 11 + 5] = 130;
    mask[6 * 11 + 7] = 160;                     /* highest saddle */
    memset(seed, 0, sizeof(seed));
    seed[4 * 11 + 5] = 200;                     /* seed at the center */

    if (gray_run_fill("saddles", 11, 9, seed, mask, 4, 0))
        return 1;
    if (gray_run_fill("saddles", 11, 9, seed, mask, 8, 0))
        return 1;

    /* Second configuration: same topology, different saddle heights. */
    for (y = 0; y < 9; y++)
        for (x = 0; x < 11; x++)
            if (mask[y * 11 + x] == 80)
                mask[y * 11 + x] = 60;
            else if (mask[y * 11 + x] == 130)
                mask[y * 11 + x] = 95;
            else if (mask[y * 11 + x] == 160)
                mask[y * 11 + x] = 110;
    if (gray_run_fill("saddles", 11, 9, seed, mask, 4, 0))
        return 1;
    if (gray_run_fill("saddles", 11, 9, seed, mask, 8, 0))
        return 1;
    return 0;
}

/* A zero band isolates regions completely; values around 1, 254 and
 * 255 are exercised on both sides of the band. */
static int
case_zero_band_extremes(void)
{
    uint8_t mask[7 * 11], seed[7 * 11];
    int x, y;

    memset(mask, 0, sizeof(mask));
    memset(seed, 0, sizeof(seed));
    for (y = 0; y <= 3; y++)
        for (x = 0; x <= 4; x++)
            mask[y * 11 + x] = 255;             /* top-left 255 block */
    for (y = 4; y <= 6; y++)
        for (x = 0; x <= 4; x++)
            mask[y * 11 + x] = 254;             /* bottom-left 254 */
    mask[6 * 11 + 0] = 1;                       /* isolated low cell */
    for (y = 0; y <= 6; y++)
        for (x = 6; x <= 10; x++)
            mask[y * 11 + x] = 200;             /* right block */
    mask[2 * 11 + 8] = 2;                       /* low cell, right */
    /* column x == 5 stays 0: the isolation band */

    seed[1 * 11 + 1] = 255;                     /* seeds at extremes */
    seed[5 * 11 + 2] = 254;
    seed[6 * 11 + 0] = 1;
    /* right block intentionally has no seed: it must stay 0 */

    if (gray_run_fill("zero_band_extremes", 11, 7, seed, mask, 4, 0))
        return 1;
    if (gray_run_fill("zero_band_extremes", 11, 7, seed, mask, 8, 0))
        return 1;
    return 0;
}

/* A 255 plateau with a 255 seed must reconstruct to 255, not saturate
 * one level below.  This is the counterexample for implementations
 * that clip mask==255 down to 254. */
static int
case_extremal_plateau_255(void)
{
    uint8_t mask[5 * 7], seed[5 * 7];
    int x, y;

    memset(mask, 255, sizeof(mask));
    memset(seed, 0, sizeof(seed));
    seed[2 * 7 + 3] = 255;
    if (gray_run_fill("extremal_plateau_255", 7, 5, seed, mask, 4, 0))
        return 1;
    if (gray_run_fill("extremal_plateau_255", 7, 5, seed, mask, 8, 0))
        return 1;

    /* 255 plateau embedded in a 250 surround. */
    for (y = 0; y < 5; y++)
        for (x = 0; x < 7; x++)
            mask[y * 7 + x] = 250;
    for (y = 1; y <= 3; y++)
        for (x = 2; x <= 4; x++)
            mask[y * 7 + x] = 255;
    memset(seed, 0, sizeof(seed));
    seed[2 * 7 + 3] = 255;
    if (gray_run_fill("extremal_plateau_255", 7, 5, seed, mask, 4, 0))
        return 1;
    if (gray_run_fill("extremal_plateau_255", 7, 5, seed, mask, 8, 0))
        return 1;
    return 0;
}

/* Reconstruction must commute with rotations, mirrors and transpose:
 * transform the inputs, fill, map the coordinates back, and the
 * result must equal the reference of the original image. */
static int
case_transform_invariance(void)
{
    static const int kinds[6] = { 0, 1, 2, 3, 4, 5 };
    const int w = 11, h = 7, n = w * h;
    uint8_t mask[11 * 7], seed[11 * 7];
    uint8_t ref[11 * 7];
    uint8_t tm[11 * 7], ts[11 * 7], back[11 * 7];
    int ki, conn, x, y, p, ret = 0;
    gray_ctx cx;

    /* Asymmetric mask: ramp plus a block plus extremes. */
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            mask[y * w + x] = (uint8_t)((40 + x * 9 + y * 13) % 256);
    for (y = 2; y <= 4; y++)
        for (x = 5; x <= 8; x++)
            mask[y * w + x] = 230;
    mask[0] = 1;
    mask[w - 1] = 255;
    mask[(h - 1) * w] = 0;
    for (p = 0; p < n; p++)
        seed[p] = 0;
    seed[3 * w + 6] = 210;
    seed[5 * w + 1] = 77;
    seed[1 * w + 9] = 255;

    for (conn = 4; conn <= 8 && !ret; conn += 4) {
        gray_ref_reconstruct(seed, mask, w, h, conn, ref);
        for (ki = 0; ki < 6 && !ret; ki++) {
            int tw, th, bw, bh;
            PIX *ps, *pm;
            gray_ref_transform(mask, w, h, kinds[ki], tm, &tw, &th);
            gray_ref_transform(seed, w, h, kinds[ki], ts, &bw, &bh);
            pm = gray_pix_from_array(tw, th, 8, tm);
            ps = gray_pix_from_array(tw, th, 8, ts);
            pixSeedfillGray(ps, pm, conn);
            gray_pix_to_array(ps, tm);  /* reuse tm as actual buffer */
            pixDestroy(&ps);
            pixDestroy(&pm);
            /* map the filled image back to the original frame */
            gray_ref_transform(tm, tw, th,
                               gray_ref_transform_inverse(kinds[ki]),
                               back, &bw, &bh);
            cx.cname = "transform_invariance";
            cx.func = "pixSeedfillGray";
            cx.what = "transform";
            cx.conn = conn;
            cx.w = w;
            cx.h = h;
            cx.height = -1;
            cx.delta = kinds[ki];
            cx.seed = seed;
            cx.mask = mask;
            cx.bin = NULL;
            for (p = 0; p < n && !ret; p++)
                if (back[p] != ref[p])
                    ret = gray_mismatch(&cx, p % w, p / w, ref[p], back[p]);
        }
    }
    return ret;
}

/* Monotonicity in seed and mask, and idempotence of a second fill,
 * each checked together with the per-pixel reference. */
static int
case_monotone_idempotent(void)
{
    const int w = 13, h = 9, n = w * h;
    uint8_t mask[13 * 9], seed[13 * 9];
    uint8_t seed2[13 * 9], mask2[13 * 9];
    uint8_t base[13 * 9], raised[13 * 9], again[13 * 9];
    int img, conn, p, ret = 0;
    gray_ctx cx;

    gray_rng_seed(0x5eed1234u);
    for (img = 0; img < 3 && !ret; img++) {
        gray_rand_mask(mask, w, h, img % 4);
        gray_rand_seed(seed, mask, n, img == 2);
        for (conn = 4; conn <= 8 && !ret; conn += 4) {
            cx.cname = "monotone_idempotent";
            cx.func = "pixSeedfillGray";
            cx.what = "pixels";
            cx.conn = conn;
            cx.w = w;
            cx.h = h;
            cx.height = -1;
            cx.delta = -1;
            cx.seed = seed;
            cx.mask = mask;
            cx.bin = NULL;

            /* base result from the library, checked against the model */
            {
                PIX *ps = gray_pix_from_array(w, h, 8, seed);
                PIX *pm = gray_pix_from_array(w, h, 8, mask);
                pixSeedfillGray(ps, pm, conn);
                gray_pix_to_array(ps, base);
                pixDestroy(&ps);
                pixDestroy(&pm);
            }
            gray_ref_reconstruct(seed, mask, w, h, conn, raised);
            for (p = 0; p < n && !ret; p++)
                if (base[p] != raised[p])
                    ret = gray_mismatch(&cx, p % w, p / w, raised[p], base[p]);

            /* raised legal seed: result must dominate the base one */
            for (p = 0; p < n; p++) {
                int v = seed[p] + 17;
                seed2[p] = (uint8_t)(v > mask[p] ? mask[p] : v);
            }
            if (!ret && gray_run_fill("monotone_idempotent", w, h,
                                      seed2, mask, conn, 0))
                ret = 1;
            if (!ret) {
                PIX *ps = gray_pix_from_array(w, h, 8, seed2);
                PIX *pm = gray_pix_from_array(w, h, 8, mask);
                pixSeedfillGray(ps, pm, conn);
                gray_pix_to_array(ps, raised);
                pixDestroy(&ps);
                pixDestroy(&pm);
                cx.what = "monotone-seed";
                for (p = 0; p < n && !ret; p++)
                    if (raised[p] < base[p])
                        ret = gray_mismatch(&cx, p % w, p / w,
                                            base[p], raised[p]);
            }

            /* raised mask: result must dominate the base one */
            for (p = 0; p < n; p++) {
                int v = mask[p] + 23;
                mask2[p] = (uint8_t)(v > 255 ? 255 : v);
            }
            if (!ret && gray_run_fill("monotone_idempotent", w, h,
                                      seed, mask2, conn, 0))
                ret = 1;
            if (!ret) {
                PIX *ps = gray_pix_from_array(w, h, 8, seed);
                PIX *pm = gray_pix_from_array(w, h, 8, mask2);
                pixSeedfillGray(ps, pm, conn);
                gray_pix_to_array(ps, raised);
                pixDestroy(&ps);
                pixDestroy(&pm);
                cx.what = "monotone-mask";
                for (p = 0; p < n && !ret; p++)
                    if (raised[p] < base[p])
                        ret = gray_mismatch(&cx, p % w, p / w,
                                            base[p], raised[p]);
            }

            /* idempotence: filling the result again changes nothing,
             * for the library and for the reference model alike */
            if (!ret) {
                PIX *ps = gray_pix_from_array(w, h, 8, base);
                PIX *pm = gray_pix_from_array(w, h, 8, mask);
                pixSeedfillGray(ps, pm, conn);
                gray_pix_to_array(ps, again);
                pixDestroy(&ps);
                pixDestroy(&pm);
                cx.what = "idempotent";
                for (p = 0; p < n && !ret; p++)
                    if (again[p] != base[p])
                        ret = gray_mismatch(&cx, p % w, p / w,
                                            base[p], again[p]);
                gray_ref_reconstruct(base, mask, w, h, conn, again);
                cx.what = "idempotent-model";
                for (p = 0; p < n && !ret; p++)
                    if (again[p] != base[p])
                        ret = gray_mismatch(&cx, p % w, p / w,
                                            base[p], again[p]);
            }
        }
    }
    return ret;
}

/* Random small images: fixed seed, explicit size cap, both
 * connectivities, both implementations, per-pixel reference. */
static int
case_random_small(void)
{
    uint8_t mask[GRAY_RAND_MAX_DIM * GRAY_RAND_MAX_DIM];
    uint8_t seed[GRAY_RAND_MAX_DIM * GRAY_RAND_MAX_DIM];
    int img, conn, w, h;

    gray_rng_seed(0x9e3779b9u);
    for (img = 0; img < 60; img++) {
        w = 1 + (int)gray_rng_below(GRAY_RAND_MAX_DIM);
        h = 1 + (int)gray_rng_below(GRAY_RAND_MAX_DIM);
        gray_rand_mask(mask, w, h, img % 4);
        gray_rand_seed(seed, mask, w * h, (img % 3) == 2);
        for (conn = 4; conn <= 8; conn += 4)
            if (gray_run_fill("random_small", w, h, seed, mask, conn, 0))
                return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 *  storage group                                                     *
 * ------------------------------------------------------------------ */

/* Widths 1, 3, 5, 31, 33 give real PIX rows with 1..3 padding bytes.
 * The invalid tail bytes of seed and mask are filled with different
 * patterns; the valid-region result must match the reference, the
 * mask must be byte-identical afterwards, and the seed padding must
 * be left alone. */
static int
case_tail_padding(void)
{
    static const int widths[5] = { 1, 3, 5, 31, 33 };
    static const int heights[3] = { 1, 2, 5 };
    uint8_t mask[GRAY_RAND_MAX_DIM * 5];
    uint8_t seed[GRAY_RAND_MAX_DIM * 5];
    int wi, hi, conn, w, h;

    gray_rng_seed(0x0ad5eedu);
    for (wi = 0; wi < 5; wi++) {
        for (hi = 0; hi < 3; hi++) {
            w = widths[wi];
            h = heights[hi];
            gray_rand_mask(mask, w, h, (wi + hi) % 4);
            gray_rand_seed(seed, mask, w * h, (wi % 2) == 1);
            for (conn = 4; conn <= 8; conn += 4)
                if (gray_run_fill("tail_padding", w, h, seed, mask, conn, 1))
                    return 1;
        }
    }
    return 0;
}

/* Degenerate and narrow geometries (single pixel, single row/column,
 * wide-short) densely covered with extreme values. */
static int
case_edge_sizes(void)
{
    static const int sizes[7][2] = {
        { 1, 1 }, { 1, 7 }, { 7, 1 }, { 3, 2 }, { 5, 4 }, { 31, 2 }, { 33, 3 }
    };
    static const uint8_t vals[7] = { 0, 1, 2, 254, 255, 128, 64 };
    uint8_t mask[33 * 4], seed[33 * 4];
    int si, conn, w, h, p, n;

    for (si = 0; si < 7; si++) {
        w = sizes[si][0];
        h = sizes[si][1];
        n = w * h;
        for (p = 0; p < n; p++)
            mask[p] = vals[(p + si) % 7];
        for (p = 0; p < n; p++)
            seed[p] = 0;
        seed[0] = mask[0];                       /* extreme seeds */
        seed[n - 1] = mask[n - 1];
        if (n > 2)
            seed[n / 2] = (uint8_t)(mask[n / 2] / 2);
        for (conn = 4; conn <= 8; conn += 4)
            if (gray_run_fill("edge_sizes", w, h, seed, mask, conn, 1))
                return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 *  derived group                                                     *
 * ------------------------------------------------------------------ */

/* H-dome over several disconnected peaks of different heights,
 * including the zero and saturating ends of the height range. */
static int
case_hdome_plateaus(void)
{
    const int w = 13, h = 9;
    uint8_t mask[13 * 9];
    static const int heights[5] = { 0, 1, 40, 70, 255 };
    int x, y, hi, conn;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            mask[y * w + x] = 60;                /* background */
    for (y = 1; y <= 3; y++)
        for (x = 1; x <= 3; x++)
            mask[y * w + x] = 200;               /* tall plateau */
    for (y = 5; y <= 6; y++)
        for (x = 2; x <= 3; x++)
            mask[y * w + x] = 120;               /* low plateau */
    mask[4 * w + 7] = 255;                       /* single-pixel peak */
    for (y = 4; y <= 7; y++)
        for (x = 9; x <= 11; x++)
            mask[y * w + x] = 160;               /* third plateau */

    for (hi = 0; hi < 5; hi++)
        for (conn = 4; conn <= 8; conn += 4)
            if (gray_run_hdome("hdome_plateaus", w, h, mask,
                               heights[hi], conn))
                return 1;
    /* height beyond the value range: seed saturates to 0 everywhere */
    for (conn = 4; conn <= 8; conn += 4)
        if (gray_run_hdome("hdome_plateaus", w, h, mask, 300, conn))
            return 1;
    return 0;
}

/* Two high plateaus touching only at a diagonal corner: the leak
 * path exists for 8-connectivity only, so the dome over the lower
 * plateau differs between 4- and 8-connectivity. */
static int
case_hdome_connectivity(void)
{
    static const uint8_t mask[5 * 5] = {
        250, 250,  50,  50,  50,
        250, 250,  50,  50,  50,
         50,  50, 200, 200, 200,
         50,  50, 200, 200, 200,
         50,  50, 200, 200, 200
    };
    uint8_t m4[5 * 5], m8[5 * 5];
    int p, differ = 0;

    if (gray_run_hdome("hdome_connectivity", 5, 5, mask, 100, 4))
        return 1;
    if (gray_run_hdome("hdome_connectivity", 5, 5, mask, 100, 8))
        return 1;
    gray_ref_hdome(mask, 5, 5, 100, 4, m4);
    gray_ref_hdome(mask, 5, 5, 100, 8, m8);
    for (p = 0; p < 25; p++)
        if (m4[p] != m8[p])
            differ = 1;
    if (!differ) {
        fprintf(stderr, "GRAY_MISMATCH case=hdome_connectivity "
                "func=model what=conn-difference conn=0 x=-1 y=-1 "
                "expected=1 actual=0\n");
        return 1;
    }
    return 0;
}

/* Several disconnected basins, the zero-delta copy path, saturation
 * of mask+delta at 255, and an empty seed image. */
static int
case_basin_basins(void)
{
    const int w = 10, h = 8, n = w * h;
    uint8_t mask[10 * 8], bin[10 * 8];
    int x, y, conn;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            mask[y * w + x] = 180;               /* background/rim */
    for (y = 1; y <= 3; y++)
        for (x = 1; x <= 2; x++)
            mask[y * w + x] = 30;                /* basin 1 */
    for (y = 4; y <= 6; y++)
        for (x = 6; x <= 8; x++)
            mask[y * w + x] = 50;                /* basin 2 */
    for (y = 5; y <= 6; y++)
        for (x = 4; x <= 5; x++)
            mask[y * w + x] = 60;                /* unseeded basin 3 */
    memset(bin, 0, (size_t)n);
    bin[1 * w + 1] = 1;                          /* seed in basin 1 */
    bin[5 * w + 7] = 1;                          /* seed in basin 2 */

    for (conn = 4; conn <= 8; conn += 4) {
        if (gray_run_basin("basin_basins", w, h, bin, mask, 40, conn))
            return 1;
        if (gray_run_basin("basin_basins", w, h, bin, mask, 0, conn))
            return 1;   /* delta == 0: exact copy of the mask */
        if (gray_run_basin("basin_basins", w, h, bin, mask, 255, conn))
            return 1;
    }

    /* Saturation: mask + delta clips at 255, which makes the inverted
     * seed vanish; contrast delta 20 (seed 250) with delta 40 (255). */
    {
        const int w2 = 6, h2 = 4, n2 = w2 * h2;
        uint8_t m2[6 * 4], b2[6 * 4];
        for (y = 0; y < h2; y++)
            for (x = 0; x < w2; x++)
                m2[y * w2 + x] = 250;            /* high walls */
        for (y = 1; y <= 2; y++)
            for (x = 2; x <= 3; x++)
                m2[y * w2 + x] = 230;            /* deep basin */
        memset(b2, 0, (size_t)n2);
        b2[1 * w2 + 2] = 1;
        for (conn = 4; conn <= 8; conn += 4) {
            if (gray_run_basin("basin_basins", w2, h2, b2, m2, 20, conn))
                return 1;
            if (gray_run_basin("basin_basins", w2, h2, b2, m2, 40, conn))
                return 1;
        }
    }

    /* Empty seed image: every pixel is 255 in the seed. */
    memset(bin, 0, (size_t)n);
    for (conn = 4; conn <= 8; conn += 4)
        if (gray_run_basin("basin_basins", w, h, bin, mask, 40, conn))
            return 1;
    return 0;
}

/* Two low basins touching only diagonally: the spill from the seeded
 * basin reaches the other one only under 8-connectivity. */
static int
case_basin_connectivity(void)
{
    static const uint8_t mask[5 * 5] = {
         20,  20, 200, 200, 200,
         20,  20, 200, 200, 200,
        200, 200,  40,  40,  40,
        200, 200,  40,  40,  40,
        200, 200,  40,  40,  40
    };
    uint8_t bin[5 * 5];
    uint8_t m4[5 * 5], m8[5 * 5];
    int p, differ = 0;

    memset(bin, 0, sizeof(bin));
    bin[0] = 1;  /* seed inside the top-left basin only */
    if (gray_run_basin("basin_connectivity", 5, 5, bin, mask, 60, 4))
        return 1;
    if (gray_run_basin("basin_connectivity", 5, 5, bin, mask, 60, 8))
        return 1;
    gray_ref_basin(bin, mask, 5, 5, 60, 4, m4);
    gray_ref_basin(bin, mask, 5, 5, 60, 8, m8);
    for (p = 0; p < 25; p++)
        if (m4[p] != m8[p])
            differ = 1;
    if (!differ) {
        fprintf(stderr, "GRAY_MISMATCH case=basin_connectivity "
                "func=model what=conn-difference conn=0 x=-1 y=-1 "
                "expected=1 actual=0\n");
        return 1;
    }
    return 0;
}

/* Random derived-operation checks: h-dome and basin on random masks,
 * with heights/deltas at the zero, normal and saturating ends. */
static int
case_derived_random(void)
{
    uint8_t mask[GRAY_RAND_MAX_DIM * GRAY_RAND_MAX_DIM];
    uint8_t bin[GRAY_RAND_MAX_DIM * GRAY_RAND_MAX_DIM];
    static const int heights[4] = { 0, 1, 64, 255 };
    static const int deltas[4] = { 0, 1, 30, 255 };
    int img, conn, w, h, n, p, k;

    gray_rng_seed(0xba511u);
    for (img = 0; img < 8; img++) {
        w = 1 + (int)gray_rng_below(GRAY_RAND_MAX_DIM);
        h = 1 + (int)gray_rng_below(GRAY_RAND_MAX_DIM);
        n = w * h;
        gray_rand_mask(mask, w, h, img % 4);
        for (k = 0; k < 4; k++)
            for (conn = 4; conn <= 8; conn += 4)
                if (gray_run_hdome("derived_random", w, h, mask,
                                   heights[k], conn))
                    return 1;
        for (p = 0; p < n; p++)
            bin[p] = (gray_rng_below(12) == 0) ? 1 : 0;
        for (k = 0; k < 4; k++)
            for (conn = 4; conn <= 8; conn += 4)
                if (gray_run_basin("derived_random", w, h, bin, mask,
                                   deltas[k], conn))
                    return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Registration                                                      *
 * ------------------------------------------------------------------ */
static const struct contract_case gray_cases[] = {
    { "thresholds", "maze_left_edge", case_maze_left_edge },
    { "thresholds", "maze_right_edge", case_maze_right_edge },
    { "thresholds", "diagonal_passage", case_diagonal_passage },
    { "thresholds", "platform_multi_seed", case_platform_multi_seed },
    { "thresholds", "saddles", case_saddles },
    { "thresholds", "zero_band_extremes", case_zero_band_extremes },
    { "thresholds", "extremal_plateau_255", case_extremal_plateau_255 },
    { "thresholds", "transform_invariance", case_transform_invariance },
    { "thresholds", "monotone_idempotent", case_monotone_idempotent },
    { "thresholds", "random_small", case_random_small },
    { "storage", "tail_padding", case_tail_padding },
    { "storage", "edge_sizes", case_edge_sizes },
    { "derived", "hdome_plateaus", case_hdome_plateaus },
    { "derived", "hdome_connectivity", case_hdome_connectivity },
    { "derived", "basin_basins", case_basin_basins },
    { "derived", "basin_connectivity", case_basin_connectivity },
    { "derived", "derived_random", case_derived_random }
};

const struct contract_case *
contract_cases(size_t *count)
{
    *count = sizeof(gray_cases) / sizeof(gray_cases[0]);
    return gray_cases;
}
