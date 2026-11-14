/*
** FAAD2 - Freeware Advanced Audio (AAC) Decoder including SBR decoding
**
** Numeric regression tests for AAC-LC temporal noise shaping (TNS).
**
** The reference implementation in this file is independent of the decoder:
** reflection coefficients are derived from the transmitted integers with the
** closed-form sine mapping, expanded into LPC coefficients with a separate
** double-precision step-up recursion, and applied with a direct difference
** equation that starts from zero history for every filter region.  No table
** from libfaad/tns.c is used.
**
** Groups:
**   coefficients - transmission parameters -> reflection -> LPC -> spectrum
**   ranges       - real sample-rate band tables, clipping, multi-region and
**                  per-window boundaries
**   stream       - legal AAC-LC raw_data_blocks through the public decoder,
**                  compared against spectra rebuilt from the generator's own
**                  quantised values and scale factors, then handed to an
**                  independently held filter bank
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "common.h"
#include "structs.h"
#include "syntax.h"
#include "tns.h"
#include "filtbank.h"
#include "neaacdec.h"

#include "cases.h"

/* Observation callback installed by the test binary.  The symbol only exists
 * when the library was configured with FAAD_TNS_TEST_OBSERVE. */
extern void (*faad_tns_observe)(uint8_t channel, uint16_t frame_len,
                                const real_t *spectrum);

#define TNS_PI 3.14159265358979323846
#define SPEC_LEN 1024
#define NSHORT 128
#define TNS_ORD_MAX 20
#define COEF_TOL 2e-5
#define STREAM_SPEC_TOL 2e-5
#define PCM_TOL 2e-3

static const float FLOAT_SCALE = 1.0f / 32768.0f;

/* ------------------------------------------------------------------ */
/* Independent reference: transmission parameters to LPC              */
/* ------------------------------------------------------------------ */

/* Reflection coefficient index for one transmitted integer.  With
 * coef_compress the wire carries one bit less; the upper half of the wire
 * range folds back onto high negative indices. */
static int ref_coef_index(int wire, int res, int compress)
{
    int half, idx;

    if (wire < 0 || wire >= (1 << res))
        return 0;
    if (!compress)
        return wire;

    half = 1 << (res - 1);
    idx = wire;
    if (wire >= (half >> 1))
        idx = (1 << res) - (half - wire);
    if (idx < 0 || idx >= (1 << res))
        return 0;
    return idx;
}

/* Closed-form value of the AAC TNS reflection coefficient table. */
static double ref_coef_value(int idx, int res)
{
    int half = 1 << (res - 1);

    if (idx < 0 || idx >= (1 << res))
        return 0.0;
    if (idx < half)
        return sin(TNS_PI * (double)idx / (double)((1 << res) - 1));
    return -sin(TNS_PI * (double)((1 << res) - idx) / (double)((1 << res) + 1));
}

/* Step-up recursion, performed entirely in double precision. */
static void ref_lpc_from_wire(int order, int res, int compress,
                              const int *wire, double *a)
{
    double refl[TNS_ORD_MAX], b[TNS_ORD_MAX + 1];
    int m, i;

    for (i = 0; i < order; i++)
        refl[i] = ref_coef_value(ref_coef_index(wire[i], res, compress), res);

    a[0] = 1.0;
    for (m = 1; m <= order; m++)
    {
        a[m] = refl[m - 1];
        for (i = 1; i < m; i++)
            b[i] = a[i] + a[m] * a[m - i];
        for (i = 1; i < m; i++)
            a[i] = b[i];
    }
}

/* Direct all-pole difference equation with zero initial state. */
static void ref_ar_filter(double *spectrum, int size, int inc,
                          const double *a, int order)
{
    double state[2 * TNS_ORD_MAX];
    int state_index = 0;
    int n, j;

    for (j = 0; j < 2 * TNS_ORD_MAX; j++)
        state[j] = 0.0;

    for (n = 0; n < size; n++)
    {
        double y = 0.0;
        for (j = 0; j < order; j++)
            y += state[state_index + j] * a[j + 1];
        y = *spectrum - y;

        state_index--;
        if (state_index < 0)
            state_index = order - 1;
        state[state_index] = state[state_index + order] = y;

        *spectrum = y;
        spectrum += inc;
    }
}

/* ------------------------------------------------------------------ */
/* Layout description shared by the synthetic and bitstream cases     */
/* ------------------------------------------------------------------ */

typedef struct
{
    int sr_index;
    int window_sequence;
    int is_short;
    int num_windows;
    int num_window_groups;
    int window_group_length[8];
    int num_swb;
    int max_sfb;
    uint16_t swb_offset[52];
    uint16_t swb_offset_max;

    int tns_present;
    int n_filt[8];
    int coef_res[8];
    int length[8][4];
    int order[8][4];
    int direction[8][4];
    int coef_compress[8][4];
    int coef[8][4][32];
} tns_layout;

/* max TNS scalefactor band per sample rate: LC long, LC short, SSR long,
 * SSR short.  This is the band limit stated by the coding specification. */
static int ref_max_tns_sfb(int sr_index, int is_short)
{
    static const uint8_t limit[12][4] =
    {
        {31,  9, 28, 7}, {31,  9, 28, 7}, {34, 10, 27, 7},
        {40, 14, 26, 6}, {42, 14, 26, 6}, {51, 14, 26, 6},
        {46, 14, 29, 7}, {46, 14, 29, 7}, {42, 14, 23, 8},
        {42, 14, 23, 8}, {42, 14, 23, 8}, {39, 14, 19, 7}
    };

    if (sr_index < 0 || sr_index >= 12)
        return 0;
    return limit[sr_index][is_short ? 1 : 0];
}

/* Independent implementation of the TNS region walk.  Every filter starts
 * from a fresh zero history; a whole-array pass is never substituted for the
 * per-region start/end/direction decision.  When spec is non-NULL the filter
 * runs, and when covered is non-NULL the touched coefficients are marked. */
static void ref_walk(const tns_layout *L, double *spec, uint8_t *covered,
                     int frame_len)
{
    int win_inc = frame_len / 8;
    int w, f;

    if (!L->tns_present)
        return;

    for (w = 0; w < L->num_windows; w++)
    {
        int bottom = L->num_swb;
        int limit = ref_max_tns_sfb(L->sr_index, L->is_short);

        for (f = 0; f < L->n_filt[w]; f++)
        {
            int top = bottom;
            int order = L->order[w][f];
            int start, end, size, inc;
            double a[TNS_ORD_MAX + 1];

            bottom = top - L->length[w][f];
            if (bottom < 0)
                bottom = 0;

            if (order > TNS_ORD_MAX)
                order = TNS_ORD_MAX;
            if (order <= 0)
                continue;

            start = bottom < limit ? bottom : limit;
            if (start > L->max_sfb)
                start = L->max_sfb;
            start = L->swb_offset[start];
            if (start > L->swb_offset_max)
                start = L->swb_offset_max;

            end = top < limit ? top : limit;
            if (end > L->max_sfb)
                end = L->max_sfb;
            end = L->swb_offset[end];
            if (end > L->swb_offset_max)
                end = L->swb_offset_max;

            size = end - start;
            if (size <= 0)
                continue;

            if (L->direction[w][f])
            {
                inc = -1;
                start = end - 1;
            }
            else
            {
                inc = 1;
            }

            if (spec != NULL)
            {
                ref_lpc_from_wire(order, L->coef_res[w] + 3,
                                  L->coef_compress[w][f], L->coef[w][f], a);
                ref_ar_filter(&spec[w * win_inc + start], size, inc, a, order);
            }
            if (covered != NULL)
            {
                int k;
                int first = (inc > 0) ? start : start - (size - 1);
                for (k = 0; k < size; k++)
                    covered[w * win_inc + first + k] = 1;
            }
        }
    }
}

/* Populate the decoder's own structures from a layout. */
static void layout_to_decoder(const tns_layout *L, ic_stream *ics,
                              tns_info *tns)
{
    int i, w, f, k;

    memset(ics, 0, sizeof(*ics));
    memset(tns, 0, sizeof(*tns));

    ics->window_sequence = (uint8_t)L->window_sequence;
    ics->num_windows = (uint8_t)L->num_windows;
    ics->num_window_groups = (uint8_t)L->num_window_groups;
    for (w = 0; w < L->num_window_groups; w++)
        ics->window_group_length[w] = (uint8_t)L->window_group_length[w];
    ics->num_swb = (uint8_t)L->num_swb;
    ics->max_sfb = (uint8_t)L->max_sfb;
    ics->swb_offset_max = L->swb_offset_max;
    for (i = 0; i <= L->num_swb; i++)
        ics->swb_offset[i] = L->swb_offset[i];

    ics->tns_data_present = (uint8_t)L->tns_present;
    for (w = 0; w < L->num_windows; w++)
    {
        tns->n_filt[w] = (uint8_t)L->n_filt[w];
        tns->coef_res[w] = (uint8_t)L->coef_res[w];
        for (f = 0; f < L->n_filt[w]; f++)
        {
            tns->length[w][f] = (uint8_t)L->length[w][f];
            tns->order[w][f] = (uint8_t)L->order[w][f];
            tns->direction[w][f] = (uint8_t)L->direction[w][f];
            tns->coef_compress[w][f] = (uint8_t)L->coef_compress[w][f];
            for (k = 0; k < L->order[w][f] && k < 32; k++)
                tns->coef[w][f][k] = (uint8_t)L->coef[w][f][k];
        }
    }
}

/* ------------------------------------------------------------------ */
/* Comparison helpers                                                 */
/* ------------------------------------------------------------------ */

static int spec_matches(const double *ref, const real_t *got, int n,
                        double rel, double *worst_out)
{
    double maxabs = 0.0, maxdiff = 0.0;
    int i;

    for (i = 0; i < n; i++)
    {
        double a = fabs(ref[i]);
        double d = fabs(ref[i] - (double)got[i]);
        if (a > maxabs)
            maxabs = a;
        if (d > maxdiff)
            maxdiff = d;
    }
    if (worst_out)
        *worst_out = maxdiff;
    return maxdiff <= rel * (1.0 + maxabs);
}

static int pcm_matches(const float *got, const real_t *ref_time, int n,
                       double *worst_out)
{
    double maxabs = 0.0, maxdiff = 0.0;
    int i;

    for (i = 0; i < n; i++)
    {
        double r = (double)ref_time[i] * (double)FLOAT_SCALE;
        double a = fabs(r);
        double d = fabs(r - (double)got[i]);
        if (a > maxabs)
            maxabs = a;
        if (d > maxdiff)
            maxdiff = d;
    }
    if (worst_out)
        *worst_out = maxdiff;
    return maxdiff <= PCM_TOL * (1.0 + maxabs);
}

/* Values used as probe spectra: multiples of 1/64 plus a few integer
 * impulses.  A cast to float is exact, so only filter arithmetic differs. */
static double probe_value(int i, int variant)
{
    double v = (double)((i * 37 + variant * 11 + 19) % 129 - 64) / 64.0 * 0.25;

    if (i == 3)
        v += 3.0;
    if (i == 17)
        v -= 1.5;
    if (i == 61)
        v += 2.25;
    if ((i % 101) == 43)
        v += 2.0;
    if ((i % 89) == 7)
        v -= 0.75;
    if ((i % 47) == 5)
        v += 0.5;
    return v;
}

static void fill_probe(double *ref, real_t *prod, int n, int variant)
{
    int i;

    for (i = 0; i < n; i++)
    {
        ref[i] = probe_value(i, variant);
        prod[i] = (real_t)ref[i];
    }
}

/* ------------------------------------------------------------------ */
/* Group: coefficients                                                */
/* ------------------------------------------------------------------ */

/* One full-frame filter, so range handling cannot mask a coefficient error. */
static void make_simple_layout(tns_layout *L, int res, int compress,
                               int order, int direction, const int *wire)
{
    int i;

    memset(L, 0, sizeof(*L));
    L->sr_index = 4;
    L->window_sequence = ONLY_LONG_SEQUENCE;
    L->is_short = 0;
    L->num_windows = 1;
    L->num_window_groups = 1;
    L->window_group_length[0] = 1;
    L->num_swb = 1;
    L->max_sfb = 1;
    L->swb_offset[0] = 0;
    L->swb_offset[1] = SPEC_LEN;
    L->swb_offset_max = SPEC_LEN;

    L->tns_present = 1;
    L->n_filt[0] = 1;
    L->coef_res[0] = res - 3;
    L->length[0][0] = 1;
    L->order[0][0] = order;
    L->direction[0][0] = direction;
    L->coef_compress[0][0] = compress;
    for (i = 0; i < order; i++)
        L->coef[0][0][i] = wire[i];
}

/* Run the decoder and the independent reference on one coefficient set. */
static void run_coef_case(int res, int compress, int order, int direction,
                          const int *wire, int variant)
{
    tns_layout L;
    ic_stream ics;
    tns_info tns;
    double in_ref[SPEC_LEN], ref[SPEC_LEN];
    real_t prod[SPEC_LEN];
    double worst;
    int i, changed = 0, ref_changed = 0;

    make_simple_layout(&L, res, compress, order, direction, wire);
    layout_to_decoder(&L, &ics, &tns);

    fill_probe(in_ref, prod, SPEC_LEN, variant);
    memcpy(ref, in_ref, sizeof(ref));

    tns_decode_frame(&ics, &tns, (uint8_t)L.sr_index, LC, prod, SPEC_LEN);
    ref_walk(&L, ref, NULL, SPEC_LEN);

    TASK_CHECK(spec_matches(ref, prod, SPEC_LEN, COEF_TOL, &worst));
    TASK_CHECK(isfinite(worst));

    /* The decoder must agree with the reference about whether the filter
     * altered the input: a zero reflection coefficient is a legal no-op. */
    for (i = 0; i < SPEC_LEN; i++)
    {
        if (fabs((double)prod[i] - in_ref[i]) > 1e-6)
            changed = 1;
        if (fabs(ref[i] - in_ref[i]) > 1e-6)
            ref_changed = 1;
    }
    TASK_CHECK(changed == ref_changed);
}

TASK_CASE("coefficients", coefficients_res3_uncompressed)
{
    static const int p[] = {1, 2};        /* positive and negative, order 2 */
    static const int q[] = {3, 4};        /* two negative, order 2 */
    static const int r[] = {2, 6, 1};     /* mixed, order 3 */
    static const int s[] = {0, 1, 0, 1, 0, 1, 0, 1};  /* order 8 */
    int d;

    for (d = 0; d < 2; d++)
    {
        run_coef_case(3, 0, 2, d, p, 1 + d);
        run_coef_case(3, 0, 2, d, q, 3 + d);
        run_coef_case(3, 0, 3, d, r, 5 + d);
        run_coef_case(3, 0, 8, d, s, 7 + d);
    }
}

TASK_CASE("coefficients", coefficients_res3_compressed)
{
    static const int p[] = {1, 2};        /* wire 2 is a negative index */
    static const int q[] = {3, 2, 1};     /* mixed, order 3 */
    static const int s[] = {1, 3, 1, 3, 1, 3, 1, 3};
    int d;

    for (d = 0; d < 2; d++)
    {
        run_coef_case(3, 1, 2, d, p, 11 + d);
        run_coef_case(3, 1, 3, d, q, 13 + d);
        run_coef_case(3, 1, 8, d, s, 15 + d);
    }
}

TASK_CASE("coefficients", coefficients_res4_uncompressed)
{
    static const int p[] = {1, 13};       /* positive and negative, order 2 */
    static const int q[] = {2, 9, 14};    /* mixed, order 3 */
    static const int s[] = {1, 3, 1, 3, 1, 3, 1, 3};
    int d;

    for (d = 0; d < 2; d++)
    {
        run_coef_case(4, 0, 2, d, p, 21 + d);
        run_coef_case(4, 0, 3, d, q, 23 + d);
        run_coef_case(4, 0, 8, d, s, 25 + d);
    }
}

TASK_CASE("coefficients", coefficients_res4_compressed)
{
    static const int p[] = {1, 6};        /* wire 6 is a negative index */
    static const int q[] = {7, 2, 5};     /* mixed, order 3 */
    static const int s[] = {7, 5, 3, 1, 7, 5, 3, 1};
    int d;

    for (d = 0; d < 2; d++)
    {
        run_coef_case(4, 1, 2, d, p, 31 + d);
        run_coef_case(4, 1, 3, d, q, 33 + d);
        run_coef_case(4, 1, 8, d, s, 35 + d);
    }
}

TASK_CASE("coefficients", coefficients_zero_order)
{
    static const int none[1] = {0};
    int d;

    for (d = 0; d < 2; d++)
    {
        run_coef_case(3, 0, 0, d, none, 41 + d);
        run_coef_case(4, 0, 0, d, none, 43 + d);
        run_coef_case(4, 1, 0, d, none, 45 + d);
    }
}

TASK_CASE("coefficients", coefficients_every_wire_value)
{
    /* Walk every transmitted integer of both resolutions and both
     * compression modes, forward and reverse, at order one. */
    int res, comp, wire, d;

    for (res = 3; res <= 4; res++)
    {
        for (comp = 0; comp <= 1; comp++)
        {
            int maxwire = 1 << (res - comp);
            for (wire = 0; wire < maxwire; wire++)
            {
                int w[1];
                w[0] = wire;
                for (d = 0; d < 2; d++)
                    run_coef_case(res, comp, 1, d, w,
                                  50 + (res - 3) * 40 + comp * 20 + wire * 2 + d);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Real band tables used by both ranges and stream groups             */
/* ------------------------------------------------------------------ */

/* 1024-window offset tables, indexed by the AAC sampling frequency index.
 * Same values the decoder derives from the specification. */
static const uint16_t swb1024_48[] =
{
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48,
    56, 64, 72, 80, 88, 96, 108, 120, 132, 144, 160, 176,
    196, 216, 240, 264, 292, 320, 352, 384, 416, 448, 480, 512,
    544, 576, 608, 640, 672, 704, 736, 768, 800, 832, 864, 896,
    928, 1024
};
#define NUM_SWB_48 49

static const uint16_t swb128_48[] =
{
    0, 4, 8, 12, 16, 20, 28, 36, 44, 56, 68, 80,
    96, 112, 128
};
#define NUM_SWB_128_48 14

static const uint16_t swb1024_32[] =
{
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48,
    56, 64, 72, 80, 88, 96, 108, 120, 132, 144, 160, 176,
    196, 216, 240, 264, 292, 320, 352, 384, 416, 448, 480, 512,
    544, 576, 608, 640, 672, 704, 736, 768, 800, 832, 864, 896,
    928, 960, 992, 1024
};
#define NUM_SWB_32 51

static const uint16_t swb1024_96[] =
{
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44,
    48, 52, 56, 64, 72, 80, 88, 96, 108, 120, 132, 144,
    156, 172, 188, 212, 240, 276, 320, 384, 448, 512, 576, 640,
    704, 768, 832, 896, 960, 1024
};
#define NUM_SWB_96 41

static const uint16_t swb1024_8[] =
{
    0, 12, 24, 36, 48, 60, 72, 84, 96, 108, 120, 132,
    144, 156, 172, 188, 204, 220, 236, 252, 268, 288, 308, 328,
    348, 372, 396, 420, 448, 476, 508, 544, 580, 620, 664, 712,
    764, 820, 880, 944, 1024
};
#define NUM_SWB_8 40

static const uint16_t swb128_8[] =
{
    0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 60,
    72, 88, 108, 128
};
#define NUM_SWB_128_8 15

static uint16_t num_swb_for(int sr_index, int is_short)
{
    static const uint8_t long_n[12] = {41, 41, 47, 49, 49, 51, 47, 47, 43, 43, 43, 40};
    static const uint8_t short_n[12] = {12, 12, 12, 14, 14, 14, 15, 15, 15, 15, 15, 15};

    if (sr_index < 0 || sr_index >= 12)
        return 0;
    return is_short ? short_n[sr_index] : long_n[sr_index];
}

static const uint16_t *swb_for(int sr_index, int is_short)
{
    if (is_short)
    {
        return sr_index == 11 ? swb128_8 : swb128_48;
    }
    switch (sr_index)
    {
    case 0:
    case 1:
        return swb1024_96;
    case 2:
    case 3:
    case 4:
        return swb1024_48;
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
        return swb1024_32;
    default:
        return swb1024_8;
    }
}

/* ------------------------------------------------------------------ */
/* Group: ranges                                                      */
/* ------------------------------------------------------------------ */

static void range_setup(tns_layout *L, int sr_index, int short_win,
                        int max_sfb)
{
    int n = num_swb_for(sr_index, short_win);
    const uint16_t *table = swb_for(sr_index, short_win);
    int i;

    memset(L, 0, sizeof(*L));
    L->sr_index = sr_index;
    L->is_short = short_win;
    L->window_sequence = short_win ? EIGHT_SHORT_SEQUENCE : ONLY_LONG_SEQUENCE;
    L->num_windows = short_win ? 8 : 1;
    L->num_window_groups = 1;
    L->window_group_length[0] = L->num_windows;
    L->num_swb = n;
    L->max_sfb = max_sfb;
    for (i = 0; i <= n; i++)
        L->swb_offset[i] = table[i];
    L->swb_offset_max = short_win ? NSHORT : SPEC_LEN;
    L->tns_present = 1;
}

static void range_filter(tns_layout *L, int w, int f, int len, int order,
                         int direction, int compress, int res,
                         const int *wire)
{
    int i;

    if (w >= L->num_windows || f >= 4)
        return;
    L->n_filt[w] = f + 1;
    if (f == 0)
        L->coef_res[w] = res - 3;
    L->length[w][f] = len;
    L->order[w][f] = order;
    L->direction[w][f] = direction;
    L->coef_compress[w][f] = compress;
    for (i = 0; i < order; i++)
        L->coef[w][f][i] = wire[i];
}

/* Clamp one end of a TNS region to the permitted band limit, max_sfb and the
 * per-window offset cap, exactly as the specification orders those steps. */
static int region_endpoint(const tns_layout *L, int band)
{
    int limit = ref_max_tns_sfb(L->sr_index, L->is_short);
    int v = band < limit ? band : limit;

    if (v > L->max_sfb)
        v = L->max_sfb;
    v = L->swb_offset[v];
    if (v > L->swb_offset_max)
        v = L->swb_offset_max;
    return v;
}

/* Absolute first/last coefficient of one declared filter region, or 0 when
 * the region is empty.  Used to assert agreement region by region. */
static int region_span(const tns_layout *L, int w, int f, int *lo, int *hi)
{
    int bottom = L->num_swb;
    int top, start, end, i;

    for (i = 0; i <= f; i++)
    {
        top = bottom;
        bottom = top - L->length[w][i];
        if (bottom < 0)
            bottom = 0;
    }
    if (L->order[w][f] <= 0)
        return 0;

    start = region_endpoint(L, bottom);
    end = region_endpoint(L, top);
    if (end - start <= 0)
        return 0;

    /* The region occupies the same span whichever way it is filtered. */
    *lo = w * (L->is_short ? NSHORT : SPEC_LEN) + start;
    *hi = w * (L->is_short ? NSHORT : SPEC_LEN) + end - 1;
    return 1;
}

static void run_range_case(tns_layout *L, int variant)
{
    ic_stream ics;
    tns_info tns;
    double in[SPEC_LEN], ref[SPEC_LEN];
    real_t prod[SPEC_LEN];
    uint8_t covered[SPEC_LEN];
    double worst;
    int i, outside_ok = 1, inside_changed = 0;
    static const int guard_pos[4] = {1, 2, 1022, 1023};

    layout_to_decoder(L, &ics, &tns);

    fill_probe(in, prod, SPEC_LEN, variant);
    /* Distinct guard values, in case a region boundary is off by one. */
    for (i = 0; i < 4; i++)
    {
        prod[guard_pos[i]] = (real_t)(guard_pos[i] == 1 ? 777.25 : -555.5);
        in[guard_pos[i]] = (double)prod[guard_pos[i]];
    }
    memcpy(ref, in, sizeof(ref));
    memset(covered, 0, sizeof(covered));

    tns_decode_frame(&ics, &tns, (uint8_t)L->sr_index, LC, prod, SPEC_LEN);
    ref_walk(L, ref, covered, SPEC_LEN);

    TASK_CHECK(spec_matches(ref, prod, SPEC_LEN, COEF_TOL, &worst));
    TASK_CHECK(isfinite(worst));

    for (i = 0; i < SPEC_LEN; i++)
    {
        if (!covered[i] && (double)prod[i] != in[i])
            outside_ok = 0;
        if (covered[i] && fabs((double)prod[i] - in[i]) > 1e-6)
            inside_changed = 1;
    }
    TASK_CHECK(outside_ok);

    /* Every declared region with a nonzero order must have actually run.
     * An all-zero-order declaration legitimately leaves the input alone. */
    {
        int active = 0;
        int w, f;
        for (w = 0; w < L->num_windows; w++)
            for (f = 0; f < L->n_filt[w]; f++)
                if (L->order[w][f] > 0)
                    active = 1;
        if (active)
            TASK_CHECK(inside_changed);
    }

    /* Per-region and per-window agreement on the exact span that the region
     * covers, so a single swapped direction or boundary cannot hide inside
     * an otherwise matching frame. */
    {
        int w, f;
        for (w = 0; w < L->num_windows; w++)
            for (f = 0; f < L->n_filt[w]; f++)
            {
                int lo, hi, k, touched = 0;
                if (!region_span(L, w, f, &lo, &hi))
                    continue;
                for (k = lo; k <= hi; k++)
                {
                    double d = fabs(ref[k] - (double)prod[k]);
                    if (d > COEF_TOL * (1.0 + fabs(ref[k])))
                        touched = 1;
                }
                TASK_CHECK(!touched);
            }
    }
}

TASK_CASE("ranges", ranges_adjacent_long_regions)
{
    tns_layout L;
    static const int a[] = {2, 6};
    static const int b[] = {1, 2};
    static const int c[] = {3, 4};

    range_setup(&L, 4, 0, 42);            /* 44100, long, 49 bands */
    range_filter(&L, 0, 0, 8, 2, 0, 0, 3, a);   /* bands 41..42 */
    range_filter(&L, 0, 1, 4, 2, 1, 1, 4, b);   /* bands 37..41, reverse */
    range_filter(&L, 0, 2, 6, 2, 0, 0, 3, c);   /* bands 31..37 */
    run_range_case(&L, 101);

    range_setup(&L, 3, 0, 40);            /* 48000, long, 49 bands */
    range_filter(&L, 0, 0, 7, 3, 1, 0, 4, c);
    range_filter(&L, 0, 1, 5, 2, 0, 1, 3, a);
    run_range_case(&L, 102);
}

TASK_CASE("ranges", ranges_zero_length_and_order)
{
    tns_layout L;
    static const int a[] = {1, 2};
    static const int b[] = {3, 4};

    range_setup(&L, 4, 0, 42);
    range_filter(&L, 0, 0, 12, 2, 0, 0, 3, a);  /* bands 37..42 */
    range_filter(&L, 0, 1, 0, 2, 0, 0, 3, b);   /* zero length: no change */
    range_filter(&L, 0, 2, 5, 2, 1, 1, 4, a);   /* bands 32..37 */
    run_range_case(&L, 111);

    /* A zero-order declaration between two active ones must not disturb
     * the neighbours' zero-history start either. */
    range_setup(&L, 3, 0, 40);
    range_filter(&L, 0, 0, 6, 2, 0, 0, 3, a);
    range_filter(&L, 0, 1, 4, 0, 0, 0, 3, a);   /* order 0 */
    range_filter(&L, 0, 2, 5, 3, 1, 0, 4, b);
    run_range_case(&L, 112);
}

TASK_CASE("ranges", ranges_band_limit_and_overlong_declaration)
{
    tns_layout L;
    static const int a[] = {1, 2};
    static const int b[] = {3, 2, 1};

    /* 96000: the permitted TNS band limit is 31 even though the long-window
     * table and max_sfb go to 40.  Bands 31..40 must stay untouched. */
    range_setup(&L, 0, 0, 40);
    range_filter(&L, 0, 0, 12, 2, 0, 0, 3, a);  /* bottom 29 -> bands 29..31 */
    range_filter(&L, 0, 1, 63, 3, 1, 1, 4, b);  /* clamps down to band 0 */
    run_range_case(&L, 121);

    /* 44100: max_sfb limits a declaration that would otherwise be legal. */
    range_setup(&L, 4, 0, 20);
    range_filter(&L, 0, 0, 40, 2, 0, 0, 3, a);  /* bands 9..20 */
    range_filter(&L, 0, 1, 60, 2, 1, 1, 4, a);  /* clamps to 0 */
    run_range_case(&L, 122);

    /* 8000: the coarsest long-window table has 40 bands and the TNS limit
     * is 39; a declaration that would reach past max_sfb is clamped. */
    range_setup(&L, 11, 0, 30);
    range_filter(&L, 0, 0, 6, 2, 0, 0, 3, a);   /* bands 24..30 */
    range_filter(&L, 0, 1, 50, 2, 1, 1, 4, b);  /* clamps to 0 */
    run_range_case(&L, 123);
}

TASK_CASE("ranges", ranges_short_windows_nonuniform_grouping)
{
    tns_layout L;
    static const int a[] = {1, 2};
    static const int b[] = {1, 6};
    static const int c[] = {3, 4};
    static const int d[] = {1, 6, 3};

    /* Eight short windows, non-uniform grouping 2+3+3.  Each window has its
     * own TNS parameters and therefore its own zero-history filters. */
    range_setup(&L, 4, 1, 12);            /* 44100, short, 14 bands */
    L.num_window_groups = 3;
    L.window_group_length[0] = 2;
    L.window_group_length[1] = 3;
    L.window_group_length[2] = 3;

    range_filter(&L, 0, 0, 4, 2, 0, 0, 3, a);
    /* window 1: TNS off */
    range_filter(&L, 2, 0, 4, 2, 1, 1, 4, b);
    range_filter(&L, 3, 0, 5, 1, 0, 1, 3, c);
    /* window 4: TNS off */
    range_filter(&L, 5, 0, 4, 1, 1, 0, 4, c);
    /* window 6: TNS off */
    range_filter(&L, 7, 0, 4, 2, 0, 0, 3, c);
    range_filter(&L, 7, 1, 4, 3, 1, 0, 4, d);
    run_range_case(&L, 131);

    /* A short frame with a zero-length first region and a second region that
     * starts from zero history rather than from the first region's tail. */
    range_setup(&L, 3, 1, 12);
    range_filter(&L, 0, 0, 0, 2, 0, 0, 3, a);
    range_filter(&L, 0, 1, 4, 2, 0, 0, 3, a);
    range_filter(&L, 1, 0, 4, 2, 1, 1, 4, b);
    run_range_case(&L, 132);
}

/* ------------------------------------------------------------------ */
/* Group: stream                                                      */
/* ------------------------------------------------------------------ */

/* --- bit writer --- */
#define BW_CAP 20000
typedef struct
{
    unsigned char buf[BW_CAP];
    int pos;
} bit_writer;

static void bw_init(bit_writer *b)
{
    memset(b->buf, 0, sizeof(b->buf));
    b->pos = 0;
}

static void bw_put(bit_writer *b, unsigned v, int n)
{
    while (n-- > 0)
    {
        if ((v >> n) & 1u)
            b->buf[b->pos >> 3] |= (unsigned char)(1u << (7 - (b->pos & 7)));
        b->pos++;
    }
}

static void bw_code(bit_writer *b, const char *code)
{
    while (*code)
        bw_put(b, (unsigned)(*code++ == '1'), 1);
}

static void bw_align(bit_writer *b)
{
    while (b->pos & 7)
        bw_put(b, 0, 1);
}

/* --- codebook-1 patterns and their codewords --- */
static const int cb1_pattern[8][4] =
{
    {0, 0, 0, 0},
    {1, 0, 0, 0},
    {-1, 0, 0, 0},
    {0, 0, 0, -1},
    {0, 1, 0, 0},
    {0, 0, 0, 1},
    {0, 0, -1, 0},
    {0, 0, 1, 0}
};
static const char *cb1_code[8] =
{
    "0", "10000", "10001", "10010", "10011", "10100", "10101", "10110"
};

static int cb1_pattern_index(int seed, int g, int sfb, int win, int bin)
{
    uint32_t h = (uint32_t)seed * 131u + (uint32_t)g * 977u
               + (uint32_t)sfb * 53u + (uint32_t)win * 29u + (uint32_t)bin * 7u;
    h = h * 2654435761u;
    h ^= h >> 15;
    return (int)(h % 8u);
}

/* --- frame description --- */
typedef struct
{
    int sr_index;
    int window_sequence;
    int window_shape;
    int max_sfb;
    int grouping;              /* 7-bit scale_factor_grouping, short only */
    const uint16_t *swb;
    int num_swb;
    int active_lo, active_hi;  /* scalefactor bands coded with book 1 */
    int seed;

    int tns_present;
    int n_filt[8];
    int tns_res[8];
    int len[8][4], order[8][4], dir[8][4], comp[8][4];
    int coef[8][4][8];
} stream_desc;

typedef struct
{
    unsigned char bytes[BW_CAP / 8];
    int len;
    int window_sequence;
    int window_shape;
    real_t ref_spec[SPEC_LEN];
} built_frame;

static int desc_is_short(const stream_desc *d)
{
    return d->window_sequence == EIGHT_SHORT_SEQUENCE;
}

static int desc_windows(const stream_desc *d)
{
    return desc_is_short(d) ? 8 : 1;
}

/* Derive the window grouping exactly as the bitstream groups field does. */
static int desc_groups(const stream_desc *d, int *len, int *start)
{
    int nw = desc_windows(d);
    int g = 0, w;

    len[0] = 1;
    start[0] = 0;
    for (w = 1; w < nw; w++)
    {
        if ((d->grouping & (1 << (7 - w))) != 0)
        {
            len[g]++;
        }
        else
        {
            g++;
            len[g] = 1;
            start[g] = w;
        }
    }
    return g + 1;
}

    /* Reference value of the inverse quantiser: sign(q)*|q|^(4/3). */
static double ref_iquant(int q)
{
    double a = fabs((double)q);
    double m = pow(a, 4.0 / 3.0);
    return q < 0 ? -m : m;
}

/* The decoder evaluates 2^((sf-100)/4) as a table of powers of two times an
 * interpolation factor for the remainder; reference the closed form. */
static double ref_scf(int sf)
{
    return pow(2.0, (double)(sf - 100) / 4.0);
}

static void build_frame(const stream_desc *d, built_frame *out)
{
    bit_writer bw;
    int glen[8], gstart[8];
    int ng = desc_groups(d, glen, gstart);
    int nw = desc_windows(d);
    int is_short = desc_is_short(d);
    int win_inc = is_short ? NSHORT : SPEC_LEN;
    int w, g, f, sfb;
    int i;

    bw_init(&bw);

    /* raw_data_block: one SCE */
    bw_put(&bw, ID_SCE, 3);
    bw_put(&bw, 0, 4);                 /* element_instance_tag */
    bw_put(&bw, 100, 8);               /* global_gain */

    /* ics_info */
    bw_put(&bw, 0, 1);                 /* ics_reserved_bit */
    bw_put(&bw, (unsigned)d->window_sequence, 2);
    bw_put(&bw, (unsigned)d->window_shape, 1);
    if (is_short)
    {
        bw_put(&bw, (unsigned)d->max_sfb, 4);
        bw_put(&bw, (unsigned)d->grouping, 7);
    }
    else
    {
        bw_put(&bw, (unsigned)d->max_sfb, 6);
        bw_put(&bw, 0, 1);             /* predictor_data_present */
    }

    /* section_data: one sfb per section.  Inactive bands are coded with the
     * zero codebook, so their scale factors are not transmitted either. */
    for (g = 0; g < ng; g++)
    {
        for (sfb = 0; sfb < d->max_sfb; sfb++)
        {
            int cb = (sfb >= d->active_lo && sfb < d->active_hi) ? 1 : 0;
            bw_put(&bw, (unsigned)cb, 4);
            bw_put(&bw, 1, is_short ? 3 : 5);
        }
    }

    /* scale_factor_data: delta zero from the global gain for every coded
     * scalefactor band, so every active band has scale factor 100. */
    for (g = 0; g < ng; g++)
        for (sfb = 0; sfb < d->max_sfb; sfb++)
            if (sfb >= d->active_lo && sfb < d->active_hi)
                bw_code(&bw, "0");     /* huffman symbol 60 */

    bw_put(&bw, 0, 1);                 /* pulse_data_present */

    /* tns_data */
    bw_put(&bw, d->tns_present ? 1u : 0u, 1);
    if (d->tns_present)
    {
        for (w = 0; w < nw; w++)
        {
            bw_put(&bw, (unsigned)d->n_filt[w], is_short ? 1 : 2);
            if (d->n_filt[w])
                bw_put(&bw, (unsigned)(d->tns_res[w] - 3), 1);
            for (f = 0; f < d->n_filt[w]; f++)
            {
                int order = d->order[w][f];
                bw_put(&bw, (unsigned)d->len[w][f], is_short ? 4 : 6);
                bw_put(&bw, (unsigned)order, is_short ? 3 : 5);
                if (order)
                {
                    int bits;
                    bw_put(&bw, (unsigned)d->dir[w][f], 1);
                    bw_put(&bw, (unsigned)d->comp[w][f], 1);
                    bits = d->tns_res[w] - d->comp[w][f];
                    for (i = 0; i < order; i++)
                    {
                        /* A coefficient that does not fit the transmitted
                         * field would silently alias; reject it instead. */
                        TASK_CHECK(d->coef[w][f][i] >= 0 &&
                                   d->coef[w][f][i] < (1 << bits));
                        bw_put(&bw, (unsigned)d->coef[w][f][i] & ((1 << bits) - 1),
                               bits);
                    }
                }
            }
        }
    }

    bw_put(&bw, 0, 1);                 /* gain_control_data_present */

    /* spectral_data, in the bitstream's group/sfb/window/bin order.  Every
     * band inside max_sfb consumes codebook content; bands outside the
     * active window use the zero codebook and consume nothing. */
    for (g = 0; g < ng; g++)
    {
        for (sfb = 0; sfb < d->max_sfb; sfb++)
        {
            int width;
            if (sfb < d->active_lo || sfb >= d->active_hi)
                continue;
            width = d->swb[sfb + 1] - d->swb[sfb];
            for (w = 0; w < glen[g]; w++)
            {
                int bin;
                for (bin = 0; bin < width; bin += 4)
                {
                    int p = cb1_pattern_index(d->seed, g, sfb, w, bin / 4);
                    bw_code(&bw, cb1_code[p]);
                }
            }
        }
    }

    bw_put(&bw, ID_END, 3);
    bw_align(&bw);

    out->len = bw.pos / 8;
    memcpy(out->bytes, bw.buf, (size_t)out->len);
    out->window_sequence = d->window_sequence;
    out->window_shape = d->window_shape;

    /* Independent reference reconstruction of the pre-TNS spectrum from the
     * generator's own quantised values and scale factors. */
    memset(out->ref_spec, 0, sizeof(out->ref_spec));
    {
        double pre[SPEC_LEN];
        uint8_t covered[SPEC_LEN];
        tns_layout L;

        memset(pre, 0, sizeof(pre));
        memset(covered, 0, sizeof(covered));

        for (g = 0; g < ng; g++)
        {
            for (sfb = 0; sfb < d->max_sfb; sfb++)
            {
                int width;
                if (sfb < d->active_lo || sfb >= d->active_hi)
                    continue;
                width = d->swb[sfb + 1] - d->swb[sfb];
                for (w = 0; w < glen[g]; w++)
                {
                    int bin;
                    for (bin = 0; bin < width; bin += 4)
                    {
                        int p = cb1_pattern_index(d->seed, g, sfb, w, bin / 4);
                        double scf = ref_scf(100);   /* global_gain 100 */
                        int k;
                        for (k = 0; k < 4; k++)
                        {
                            int pos = (gstart[g] + w) * win_inc
                                    + d->swb[sfb] + bin + k;
                            pre[pos] = ref_iquant(cb1_pattern[p][k]) * scf;
                        }
                    }
                }
            }
        }

        /* TNS parameters in the reference layout. */
        memset(&L, 0, sizeof(L));
        L.sr_index = d->sr_index;
        L.is_short = is_short;
        L.window_sequence = d->window_sequence;
        L.num_windows = nw;
        L.num_window_groups = ng;
        for (g = 0; g < ng; g++)
            L.window_group_length[g] = glen[g];
        L.num_swb = d->num_swb;
        L.max_sfb = d->max_sfb;
        for (i = 0; i <= d->num_swb; i++)
            L.swb_offset[i] = d->swb[i];
        L.swb_offset_max = win_inc;
        L.tns_present = d->tns_present;
        for (w = 0; w < nw; w++)
        {
            L.n_filt[w] = d->n_filt[w];
            L.coef_res[w] = d->tns_res[w] - 3;
            for (f = 0; f < d->n_filt[w]; f++)
            {
                L.length[w][f] = d->len[w][f];
                L.order[w][f] = d->order[w][f];
                L.direction[w][f] = d->dir[w][f];
                L.coef_compress[w][f] = d->comp[w][f];
                for (i = 0; i < d->order[w][f]; i++)
                    L.coef[w][f][i] = d->coef[w][f][i];
            }
        }
        ref_walk(&L, pre, NULL, SPEC_LEN);
        for (i = 0; i < SPEC_LEN; i++)
            out->ref_spec[i] = (real_t)pre[i];
    }
}

/* --- observation hook --- */
static const real_t *g_pending_ref;
static int g_pending_active;
static real_t g_observed[SPEC_LEN];
static int g_observed_count;

static void observe_cb(uint8_t channel, uint16_t frame_len,
                       const real_t *spectrum)
{
    if (!g_pending_active)
        return;
    if (channel != 0 || frame_len != SPEC_LEN)
        return;
    memcpy(g_observed, spectrum, sizeof(g_observed));
    g_observed_count++;
}

typedef struct
{
    NeAACDecHandle dec;
    unsigned long rate;
    unsigned char channels;
    fb_info *fb;
    real_t overlap[SPEC_LEN];
    uint8_t shape_prev;
} stream_instance;

static int stream_open(stream_instance *si, int sr_index)
{
    unsigned char asc[2];
    unsigned bits = 0;
    unsigned long rate = 0;
    unsigned char channels = 0;
    NeAACDecConfigurationPtr cfg;

    /* AudioSpecificConfig: LC, sample rate index, mono, no small frames. */
    bits = (2u << 11) | ((unsigned)sr_index << 7) | (1u << 3);
    asc[0] = (unsigned char)(bits >> 8);
    asc[1] = (unsigned char)(bits & 0xFF);

    si->dec = NeAACDecOpen();
    if (!si->dec)
        return 0;
    cfg = NeAACDecGetCurrentConfiguration(si->dec);
    cfg->outputFormat = FAAD_FMT_FLOAT;
    NeAACDecSetConfiguration(si->dec, cfg);

    if (NeAACDecInit2(si->dec, asc, sizeof(asc), &rate, &channels) != 0)
        return 0;
    si->rate = rate;
    si->channels = channels;
    si->fb = filter_bank_init(SPEC_LEN);
    memset(si->overlap, 0, sizeof(si->overlap));
    si->shape_prev = 0;
    return si->fb != NULL;
}

static void stream_close(stream_instance *si)
{
    if (si->fb)
        filter_bank_end(si->fb);
    if (si->dec)
        NeAACDecClose(si->dec);
    si->fb = NULL;
    si->dec = NULL;
}

static void stream_decode_frame(stream_instance *si, const built_frame *bf,
                                int frame_no)
{
    NeAACDecFrameInfo info;
    float *pcm;
    real_t fb_in[SPEC_LEN], fb_out[SPEC_LEN];
    double worst;
    unsigned long i;
    double energy = 0.0;
    unsigned long expect_samples = (frame_no == 0) ? 0 : SPEC_LEN;

    g_pending_ref = bf->ref_spec;
    g_pending_active = 1;
    g_observed_count = 0;

    pcm = (float *)NeAACDecDecode(si->dec, &info, (unsigned char *)bf->bytes,
                                  (unsigned long)bf->len);

    TASK_CHECK(pcm != NULL);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.channels == 1);
    TASK_CHECK(info.samples == expect_samples);

    TASK_CHECK(g_observed_count == 1);
    {
        double maxabs = 0.0, maxdiff = 0.0;
        for (i = 0; i < SPEC_LEN; i++)
        {
            double r = (double)bf->ref_spec[i];
            double a = fabs(r);
            double d = fabs(r - (double)g_observed[i]);
            if (a > maxabs) maxabs = a;
            if (d > maxdiff) maxdiff = d;
        }
        worst = maxdiff;
        TASK_CHECK(maxdiff <= STREAM_SPEC_TOL * (1.0 + maxabs));
        TASK_CHECK(maxabs > 0.0);
    }

    for (i = 0; i < SPEC_LEN; i++)
        energy += (double)pcm[i] * (double)pcm[i];
    if (frame_no > 0)
        TASK_CHECK(energy > 0.0);

    /* The independently rebuilt spectrum goes through a filter bank that
     * holds its own overlap history. */
    memcpy(fb_in, bf->ref_spec, sizeof(fb_in));
    ifilter_bank(si->fb, (uint8_t)bf->window_sequence,
                 (uint8_t)bf->window_shape, si->shape_prev,
                 fb_in, fb_out, si->overlap, LC, SPEC_LEN);
    si->shape_prev = (uint8_t)bf->window_shape;

    TASK_CHECK(pcm_matches(pcm, fb_out, SPEC_LEN, &worst));
    TASK_CHECK(isfinite(worst));

    g_pending_active = 0;
}

/* --- concrete streams --- */

static void desc_init(stream_desc *d, int sr_index, int window_sequence,
                      int max_sfb, int grouping, int active_lo, int active_hi,
                      int seed)
{
    memset(d, 0, sizeof(*d));
    d->sr_index = sr_index;
    d->window_sequence = window_sequence;
    d->window_shape = 0;
    d->max_sfb = max_sfb;
    d->grouping = grouping;
    d->swb = swb_for(sr_index, window_sequence == EIGHT_SHORT_SEQUENCE);
    d->num_swb = num_swb_for(sr_index, window_sequence == EIGHT_SHORT_SEQUENCE);
    d->active_lo = active_lo;
    d->active_hi = active_hi;
    d->seed = seed;
}

static void desc_tns(stream_desc *d, int w, int nf, int res, int len0,
                     int order0, int dir0, int comp0, const int *c0)
{
    int i;
    d->tns_present = 1;
    d->n_filt[w] = nf;
    d->tns_res[w] = res;
    d->len[w][0] = len0;
    d->order[w][0] = order0;
    d->dir[w][0] = dir0;
    d->comp[w][0] = comp0;
    for (i = 0; i < order0; i++)
        d->coef[w][0][i] = c0[i];
}

static void desc_tns2(stream_desc *d, int w, int nf, int res, int len0,
                      int order0, int dir0, int comp0, const int *c0,
                      int len1, int order1, int dir1, int comp1,
                      const int *c1)
{
    int i;
    desc_tns(d, w, nf, res, len0, order0, dir0, comp0, c0);
    d->len[w][1] = len1;
    d->order[w][1] = order1;
    d->dir[w][1] = dir1;
    d->comp[w][1] = comp1;
    for (i = 0; i < order1; i++)
        d->coef[w][1][i] = c1[i];
}

/* Instance A: 44100 Hz, long -> long start -> eight short -> long stop ->
 * long.  Adjacent frames change TNS parameters; one frame has TNS off. */
static int build_instance_a(built_frame frames[5])
{
    stream_desc d;
    static const int c12[] = {1, 2};
    static const int c26[] = {2, 6};
    static const int c153_comp[] = {1, 5, 3};   /* fits res 4 compressed */
    static const int c16[] = {1, 6};
    static const int c34[] = {3, 4};
    static const int c163[] = {1, 6, 3};

    /* frame 0: ONLY_LONG, two adjacent filters, forward and reverse */
    desc_init(&d, 4, ONLY_LONG_SEQUENCE, 42, 0, 36, 42, 2001);
    desc_tns2(&d, 0, 2, 3, 8, 2, 0, 0, c12, 4, 2, 1, 1, c12);
    build_frame(&d, &frames[0]);

    /* frame 1: LONG_START, different resolution/order/direction */
    desc_init(&d, 4, LONG_START_SEQUENCE, 42, 0, 29, 42, 2002);
    desc_tns(&d, 0, 1, 4, 20, 3, 1, 1, c153_comp);
    build_frame(&d, &frames[1]);

    /* frame 2: EIGHT_SHORT, non-uniform grouping, per-window parameters */
    desc_init(&d, 4, EIGHT_SHORT_SEQUENCE, 12, 0x5B, 8, 12, 2003);
    desc_tns(&d, 0, 1, 3, 4, 2, 0, 0, c12);
    d.n_filt[1] = 0;
    desc_tns(&d, 2, 1, 4, 4, 2, 1, 1, c16);
    desc_tns(&d, 3, 1, 3, 5, 1, 0, 1, c12);
    d.n_filt[4] = 0;
    desc_tns(&d, 5, 1, 4, 4, 1, 1, 0, c34);
    d.n_filt[6] = 0;
    /* Short windows carry only one filter per window. */
    desc_tns(&d, 7, 1, 3, 4, 3, 1, 0, c163);
    build_frame(&d, &frames[2]);

    /* frame 3: LONG_STOP, TNS off */
    desc_init(&d, 4, LONG_STOP_SEQUENCE, 42, 0, 34, 42, 2004);
    d.tns_present = 0;
    build_frame(&d, &frames[3]);

    /* frame 4: ONLY_LONG, parameters change again */
    desc_init(&d, 4, ONLY_LONG_SEQUENCE, 42, 0, 36, 42, 2005);
    desc_tns(&d, 0, 1, 4, 12, 2, 0, 0, c26);
    build_frame(&d, &frames[4]);

    return 5;
}

/* Instance B: 32000 Hz, three long frames, interleaved with instance A. */
static int build_instance_b(built_frame frames[3])
{
    stream_desc d;
    static const int c12[] = {1, 2};
    static const int c26[] = {2, 6};
    static const int c163[] = {1, 6, 3};

    desc_init(&d, 5, ONLY_LONG_SEQUENCE, 51, 0, 45, 51, 3001);
    desc_tns(&d, 0, 1, 3, 4, 2, 0, 1, c12);
    build_frame(&d, &frames[0]);

    desc_init(&d, 5, ONLY_LONG_SEQUENCE, 51, 0, 45, 51, 3002);
    d.tns_present = 0;
    build_frame(&d, &frames[1]);

    desc_init(&d, 5, ONLY_LONG_SEQUENCE, 51, 0, 44, 51, 3003);
    desc_tns2(&d, 0, 2, 4, 6, 3, 1, 0, c163, 5, 2, 0, 0, c26);
    build_frame(&d, &frames[2]);

    return 3;
}

TASK_CASE("stream", stream_long_short_long_path)
{
    stream_instance si;
    built_frame frames[5];
    unsigned long i;
    int n, f;
    double energy = 0.0;

    faad_tns_observe = observe_cb;
    n = build_instance_a(frames);
    TASK_CHECK(n == 5);
    for (i = 0; i < (unsigned long)n; i++)
    {
        TASK_CHECK(frames[i].len > 0);
        TASK_CHECK(frames[i].len * 8 <= BW_CAP);
    }

    TASK_CHECK(stream_open(&si, 4));
    TASK_CHECK(si.rate == 44100);
    TASK_CHECK(si.channels == 1);

    for (f = 0; f < n; f++)
        stream_decode_frame(&si, &frames[f], f);

    /* The legal long -> short -> long sequence must have been exercised. */
    TASK_CHECK(frames[0].window_sequence == ONLY_LONG_SEQUENCE);
    TASK_CHECK(frames[1].window_sequence == LONG_START_SEQUENCE);
    TASK_CHECK(frames[2].window_sequence == EIGHT_SHORT_SEQUENCE);
    TASK_CHECK(frames[3].window_sequence == LONG_STOP_SEQUENCE);
    TASK_CHECK(frames[4].window_sequence == ONLY_LONG_SEQUENCE);

    /* TNS off in frame 3: its reference and observation still come from the
     * same path, and the spectrum retains the generated content. */
    for (i = 0; i < SPEC_LEN; i++)
        energy += (double)frames[3].ref_spec[i] * (double)frames[3].ref_spec[i];
    TASK_CHECK(energy > 0.0);

    stream_close(&si);
    g_pending_active = 0;
}

TASK_CASE("stream", stream_parameter_change_across_frames)
{
    /* Same two long frames rendered with different TNS declarations must
     * produce different final spectra; identical ones would hide a decoder
     * that ignores an adjacent-frame parameter update. */
    stream_desc d1, d2;
    built_frame f1, f2;
    static const int c12[] = {1, 2};
    static const int c34[] = {3, 4};
    double diff = 0.0;
    int i;

    desc_init(&d1, 4, ONLY_LONG_SEQUENCE, 42, 0, 36, 42, 4101);
    desc_tns(&d1, 0, 1, 3, 8, 2, 0, 0, c12);
    build_frame(&d1, &f1);

    desc_init(&d2, 4, ONLY_LONG_SEQUENCE, 42, 0, 36, 42, 4101);
    desc_tns(&d2, 0, 1, 4, 8, 2, 1, 1, c34);
    build_frame(&d2, &f2);

    for (i = 0; i < SPEC_LEN; i++)
        diff += fabs((double)f1.ref_spec[i] - (double)f2.ref_spec[i]);
    TASK_CHECK(diff > 1e-3);
    TASK_CHECK(f1.len > 0 && f2.len > 0);
}

TASK_CASE("stream", stream_two_instances_interleaved)
{
    stream_instance a, b;
    built_frame fa[5], fb[3];
    int na, nb, k, f;

    faad_tns_observe = observe_cb;
    na = build_instance_a(fa);
    nb = build_instance_b(fb);
    TASK_CHECK(na == 5 && nb == 3);

    TASK_CHECK(stream_open(&a, 4));
    TASK_CHECK(stream_open(&b, 5));
    TASK_CHECK(a.rate == 44100);
    TASK_CHECK(b.rate == 32000);
    TASK_CHECK(a.channels == 1 && b.channels == 1);
    TASK_CHECK(a.fb != b.fb);

    for (k = 0; k < na; k++)
    {
        stream_decode_frame(&a, &fa[k], k);
        if (k < nb)
            stream_decode_frame(&b, &fb[k], k);
    }
    /* Alternate once more in the other order to rule out ordering state. */
    for (f = 0; f < nb; f++)
    {
        stream_decode_frame(&b, &fb[f], nb + f);
    }

    stream_close(&a);
    stream_close(&b);
    g_pending_active = 0;
}
