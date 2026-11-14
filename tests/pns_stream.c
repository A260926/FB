/*
 * End-to-end PNS content regression.
 *
 * The test constructs AAC-LC raw_data_block packets in C and drives them
 * through the public API (NeAACDecInit2 / NeAACDecDecode). Test-only hooks
 * (PNS_TEST_HOOK) capture the spectra immediately after PNS and after M/S,
 * plus the frame-end RNG register state, without adding public interfaces.
 *
 * Expectations are built independently from the generator's band plan and
 * the standalone RNG model in pns_reference.c: production output is never
 * turned into an expectation. The synthesis filter bank is reused only for
 * an isolated continuous-PCM cross-check with separate overlap history.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "neaacdec.h"
#include "common.h"
#include "pns_reference.h"

/* Internal headers are needed for the test-only hook signatures. */
#include "structs.h"
#include "syntax.h"
#include "filtbank.h"

#ifndef PNS_TEST_HOOK
#error "pns_stream.c must be built with -DPNS_TEST_HOOK"
#endif

#define MAX_FRAMES 16
#define FRAME      1024
#define TOL(expect) (3e-5 * (1.0 + fabs(expect)))

enum { CB_ZERO = 0, CB_ONE = 1, CB_NOISE = 13 };

typedef struct
{
    int cbL;
    int cbR;
    unsigned char ms_used; /* evaluated when ms_mask_present == 1 */
} sfb_plan_t;

typedef struct
{
    int seq;                       /* window_sequence */
    int ms_mask;                   /* 0, 1 or 2 */
    int max_sfb;
    int grouping;                  /* scale_factor_grouping (short only) */
    int num_groups;
    int group_len[8];
    sfb_plan_t sfb[8];
    int nrgL[8][8];                /* [group][sfb], -1 when not PNS */
    int nrgR[8][8];
} frame_plan_t;

/* ------------------------------------------------------------------ */
/* HCB_SF codewords, extracted from libfaad/codebook/hcb_sf.h.        */
/* ------------------------------------------------------------------ */
static const struct pns_sf_entry { unsigned char len; const char *bits; }
    pns_sf_code[121] = {
#include "pns_sf_table.inc"
    };

/* ------------------------------------------------------------------ */
/* Bitstream writer.                                                  */
/* ------------------------------------------------------------------ */
typedef struct
{
    unsigned char buf[2048];
    unsigned pos;
} bitbuf_t;

static void bb_put(bitbuf_t *bb, unsigned value, unsigned n)
{
    while (n--)
    {
        unsigned b = (value >> n) & 1u;
        bb->buf[bb->pos >> 3] |= (unsigned char)(b << (7 - (bb->pos & 7)));
        bb->pos++;
    }
}

static void bb_put_sf(bitbuf_t *bb, unsigned symbol)
{
    const char *p;
    if (symbol > 120) { fprintf(stderr, "sf symbol out of range\n"); exit(2); }
    for (p = pns_sf_code[symbol].bits; *p; p++)
        bb_put(bb, (unsigned)(*p - '0'), 1);
}

static unsigned bb_byte_size(bitbuf_t *bb)
{
    return (bb->pos + 7u) / 8u;
}

/* ------------------------------------------------------------------ */
/* Hook capture. One slot per decoded packet.                         */
/* ------------------------------------------------------------------ */
typedef struct
{
    float pns_l[FRAME];
    float pns_r[FRAME];
    float ms_l[FRAME];
    float ms_r[FRAME];
    uint32_t r1;
    uint32_t r2;
    int have_pns;
    int have_ms;
} capture_t;

static capture_t cap[MAX_FRAMES];
static unsigned cap_index;

void pns_test_hook_pns(const ic_stream *ics_left, const ic_stream *ics_right,
                       const real_t *spec_left, const real_t *spec_right,
                       uint16_t frame_len, uint8_t channel_pair,
                       uint32_t r1, uint32_t r2)
{
    capture_t *c;
    (void)ics_left; (void)ics_right; (void)frame_len; (void)channel_pair;
    if (cap_index >= MAX_FRAMES) { fprintf(stderr, "hook overflow\n"); exit(2); }
    c = &cap[cap_index];
    memcpy(c->pns_l, spec_left, sizeof(c->pns_l));
    if (spec_right)
        memcpy(c->pns_r, spec_right, sizeof(c->pns_r));
    else
        memset(c->pns_r, 0, sizeof(c->pns_r));
    c->r1 = r1;
    c->r2 = r2;
    c->have_pns = 1;
}

void pns_test_hook_ms(const ic_stream *ics, const ic_stream *icsr,
                      const real_t *l_spec, const real_t *r_spec,
                      uint16_t frame_len)
{
    capture_t *c;
    (void)ics; (void)icsr; (void)frame_len;
    if (cap_index >= MAX_FRAMES) { fprintf(stderr, "hook overflow\n"); exit(2); }
    c = &cap[cap_index];
    memcpy(c->ms_l, l_spec, sizeof(c->ms_l));
    memcpy(c->ms_r, r_spec, sizeof(c->ms_r));
    c->have_ms = 1;
    cap_index++;
}

/* ------------------------------------------------------------------ */
/* Packet construction.                                               */
/* ------------------------------------------------------------------ */
static int is_short(const frame_plan_t *p)
{
    return p->seq == EIGHT_SHORT_SEQUENCE;
}

static void encode_ics(bitbuf_t *bb, const frame_plan_t *p)
{
    bb_put(bb, 0, 1);                 /* ics_reserved_bit */
    bb_put(bb, (unsigned)p->seq, 2); /* window_sequence */
    bb_put(bb, 0, 1);                 /* window_shape */
    if (is_short(p))
    {
        bb_put(bb, (unsigned)p->max_sfb, 4);
        bb_put(bb, (unsigned)p->grouping, 7);
    } else {
        bb_put(bb, (unsigned)p->max_sfb, 6);
        bb_put(bb, 0, 1);             /* predictor_data_present */
    }
}

/* Run-length section encoding for one channel. */
static void encode_sections(bitbuf_t *bb, const frame_plan_t *p,
                            const int *cb_per_sfb)
{
    int g;
    unsigned sect_bits = is_short(p) ? 3u : 5u;

    for (g = 0; g < p->num_groups; g++)
    {
        int sfb = 0;
        while (sfb < p->max_sfb)
        {
            int cb = cb_per_sfb[sfb];
            int run = 1;
            while (sfb + run < p->max_sfb && cb_per_sfb[sfb + run] == cb)
                run++;
            bb_put(bb, (unsigned)cb, 4); /* sect_cb is always four bits */
            bb_put(bb, (unsigned)run, sect_bits); /* runs stay below escape */
            sfb += run;
        }
    }
}

/* Scale factor / noise energy data for one channel. */
static void encode_scalefactors(bitbuf_t *bb, const frame_plan_t *p,
                                const int *cb_per_sfb, const int (*nrg)[8],
                                int global_gain)
{
    int g, sfb;
    int noise_pcm_flag = 1;
    int noise_energy = global_gain - 90;

    for (g = 0; g < p->num_groups; g++)
    {
        for (sfb = 0; sfb < p->max_sfb; sfb++)
        {
            int cb = cb_per_sfb[sfb];
            if (cb == CB_ZERO)
                continue;
            if (cb == CB_NOISE)
            {
                int energy = nrg[g][sfb];
                if (noise_pcm_flag)
                {
                    int word = 256 + (energy - (global_gain - 90));
                    if (word < 0 || word > 511)
                    { fprintf(stderr, "bad first noise word\n"); exit(2); }
                    bb_put(bb, (unsigned)word, 9);
                    noise_pcm_flag = 0;
                } else {
                    int delta = energy - noise_energy;
                    int symbol = 60 + delta;
                    if (symbol < 0 || symbol > 120)
                    { fprintf(stderr, "noise delta out of range\n"); exit(2); }
                    bb_put_sf(bb, (unsigned)symbol);
                }
                noise_energy = energy;
            } else {
                /* Spectral band: plain HCB_SF delta of zero (symbol 60). */
                bb_put_sf(bb, 60);
            }
        }
    }
}

/* HCB1 spectral payload: one (1,0,0,0) quad then zero quads per group. */
static void encode_spectral(bitbuf_t *bb, const frame_plan_t *p,
                            const int *cb_per_sfb)
{
    int g;
    for (g = 0; g < p->num_groups; g++)
    {
        int sfb;
        for (sfb = 0; sfb < p->max_sfb; sfb++)
        {
            if (cb_per_sfb[sfb] != CB_ONE)
                continue;
            /* width is four coefficients per window; one quad per window. */
            int w;
            for (w = 0; w < p->group_len[g]; w++)
            {
                if (w == 0)
                    bb_put(bb, 0x10, 5); /* 10000 -> (1,0,0,0), sign positive */
                else
                    bb_put(bb, 0, 1);    /* zero quad */
            }
        }
    }
}

static void encode_channel(bitbuf_t *bb, const frame_plan_t *p,
                           const int *cb_per_sfb, const int (*nrg)[8],
                           int global_gain)
{
    bb_put(bb, (unsigned)global_gain, 8);
    encode_sections(bb, p, cb_per_sfb);
    encode_scalefactors(bb, p, cb_per_sfb, nrg, global_gain);
    bb_put(bb, 0, 1); /* pulse_data_present */
    bb_put(bb, 0, 1); /* tns_data_present */
    bb_put(bb, 0, 1); /* gain_control_data_present */
    encode_spectral(bb, p, cb_per_sfb);
}

static unsigned build_packet(const frame_plan_t *p, unsigned char *out)
{
    bitbuf_t bb;
    int cbL[8], cbR[8], sfb;
    memset(&bb, 0, sizeof(bb));

    bb_put(&bb, ID_CPE, 3);  /* channel_pair_element */
    bb_put(&bb, 0, 4);       /* element_instance_tag */
    bb_put(&bb, 1, 1);       /* common_window */
    encode_ics(&bb, p);

    bb_put(&bb, (unsigned)p->ms_mask, 2);
    if (p->ms_mask == 1)
    {
        int g;
        for (g = 0; g < p->num_groups; g++)
            for (sfb = 0; sfb < p->max_sfb; sfb++)
                bb_put(&bb, p->sfb[sfb].ms_used, 1);
    }

    for (sfb = 0; sfb < p->max_sfb; sfb++)
    {
        cbL[sfb] = p->sfb[sfb].cbL;
        cbR[sfb] = p->sfb[sfb].cbR;
    }
    encode_channel(&bb, p, cbL, p->nrgL, 100);
    encode_channel(&bb, p, cbR, p->nrgR, 100);

    bb_put(&bb, ID_END, 3);
    memcpy(out, bb.buf, bb_byte_size(&bb));
    return bb_byte_size(&bb);
}

/* ------------------------------------------------------------------ */
/* Independent reference expectation for one frame.                  */
/* ------------------------------------------------------------------ */
typedef struct
{
    double pns_l[FRAME];
    double pns_r[FRAME];
    double ms_l[FRAME];
    double ms_r[FRAME];
    pns_rng_t rng;
} expect_t;

static const uint16_t *offsets_for(const frame_plan_t *p)
{
    return is_short(p) ? pns_short_offsets : pns_long_offsets;
}

static int correlated_band(const frame_plan_t *p, int sfb)
{
    if (p->sfb[sfb].cbL != CB_NOISE || p->sfb[sfb].cbR != CB_NOISE)
        return 0;
    if (p->ms_mask == 2)
        return 1;
    if (p->ms_mask == 1)
        return p->sfb[sfb].ms_used != 0;
    return 0;
}

static void build_expect(const frame_plan_t *p, expect_t *e, pns_rng_t start)
{
    const uint16_t *off = offsets_for(p);
    int g, b, sfb;
    unsigned window_index = 0;

    memset(e, 0, sizeof(*e));
    e->rng = start;

    for (g = 0; g < p->num_groups; g++)
    {
        for (b = 0; b < p->group_len[g]; b++)
        {
            unsigned base = window_index * PNS_NSHORT;
            window_index++;
            for (sfb = 0; sfb < p->max_sfb; sfb++)
            {
                unsigned begin = base + off[sfb];
                unsigned end = base + off[sfb + 1];
                unsigned size = end - begin;

                if (p->sfb[sfb].cbL == CB_NOISE)
                {
                    pns_rng_t snapshot = e->rng;
                    pns_gen_vector(&e->pns_l[begin], p->nrgL[g][sfb],
                        size, &e->rng);
                    if (p->sfb[sfb].cbR == CB_NOISE &&
                        correlated_band(p, sfb))
                    {
                        /* Same random shape, right channel's own amplitude;
                         * the global RNG must not be advanced. */
                        pns_gen_vector(&e->pns_r[begin], p->nrgR[g][sfb],
                            size, &snapshot);
                    }
                }

                if (p->sfb[sfb].cbR == CB_NOISE &&
                    !(p->sfb[sfb].cbL == CB_NOISE && correlated_band(p, sfb)))
                {
                    pns_gen_vector(&e->pns_r[begin], p->nrgR[g][sfb],
                        size, &e->rng);
                }

                if (p->sfb[sfb].cbL == CB_ONE || p->sfb[sfb].cbR == CB_ONE)
                {
                    /* The encoded payload carries one HCB1 (1,0,0,0) quad for
                     * the first window of each group and zero quads for the
                     * remaining grouped windows. */
                    if (b == 0)
                    {
                        /* HCB1 (1,0,0,0), scale factor delta zero -> sf 100. */
                        double v = pns_iquant(1, 100);
                        if (p->sfb[sfb].cbL == CB_ONE) e->pns_l[begin] = v;
                        if (p->sfb[sfb].cbR == CB_ONE) e->pns_r[begin] = v;
                    }
                }
            }
        }
    }

    memcpy(e->ms_l, e->pns_l, sizeof(e->ms_l));
    memcpy(e->ms_r, e->pns_r, sizeof(e->ms_r));

    /* Reference M/S, mirroring ms_decode's band guards. */
    window_index = 0;
    for (g = 0; g < p->num_groups; g++)
    {
        for (b = 0; b < p->group_len[g]; b++)
        {
            unsigned base = window_index * PNS_NSHORT;
            window_index++;
            for (sfb = 0; sfb < p->max_sfb; sfb++)
            {
                int ms_active = (p->ms_mask == 2) ||
                    (p->ms_mask == 1 && p->sfb[sfb].ms_used);
                int noise_either = p->sfb[sfb].cbL == CB_NOISE ||
                    p->sfb[sfb].cbR == CB_NOISE;
                if (ms_active && !noise_either)
                    pns_apply_ms(e->ms_l, e->ms_r,
                        base + off[sfb], base + off[sfb + 1]);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Comparison with reporting.                                         */
/* ------------------------------------------------------------------ */
static long g_compared;
static int g_failures;

static void report_loc(const char *stage, unsigned frame, int group,
                       int window, int sfb, int bin,
                       double expect, double actual)
{
    printf("MISMATCH %s packet=%u group=%d window=%d sfb=%d bin=%d "
           "expect=%.9g actual=%.9g tol=%.3g\n",
           stage, frame, group, window, sfb, bin, expect, actual,
           TOL(expect));
}

static void compare_spectrum(const char *stage, unsigned frame,
                             const frame_plan_t *p,
                             const double *expect, const float *actual)
{
    const uint16_t *off = offsets_for(p);
    int g, b, sfb;
    unsigned window_index = 0;

    for (g = 0; g < p->num_groups; g++)
    {
        for (b = 0; b < p->group_len[g]; b++)
        {
            unsigned base = window_index * PNS_NSHORT;
            window_index++;
            for (sfb = 0; sfb < p->max_sfb; sfb++)
            {
                unsigned k;
                for (k = off[sfb]; k < off[sfb + 1]; k++)
                {
                    double ev = expect[base + k];
                    double av = (double)actual[base + k];
                    g_compared++;
                    if (fabs(av - ev) > TOL(ev))
                    {
                        if (g_failures < 20)
                            report_loc(stage, frame, g, window_index - 1,
                                sfb, (int)k, ev, av);
                        g_failures++;
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Filter-bank PCM cross-check with independent overlap history.      */
/* ------------------------------------------------------------------ */
typedef struct
{
    fb_info *fb;
    float overlap[FRAME];
    int shape_prev;
} synth_t;

static void synth_run(synth_t *s, int seq, const float *spec, float *pcm)
{
    ifilter_bank(s->fb, (uint8_t)seq, 0, (uint8_t)s->shape_prev,
        (real_t *)(uintptr_t)spec, pcm, s->overlap, LC, FRAME);
    s->shape_prev = 0;
}

/* ------------------------------------------------------------------ */
/* Frame plans.                                                       */
/* ------------------------------------------------------------------ */
static frame_plan_t plans[MAX_FRAMES];
static unsigned num_plans;

static sfb_plan_t S(int l, int r, int ms)
{
    sfb_plan_t s = { l, r, (unsigned char)ms };
    return s;
}

static void build_plans(void)
{
    frame_plan_t *q;
    int g, sfb;

    /* F0: independent dual noise (mask 0), zero band and trailing noise. */
    q = &plans[num_plans++];
    q->seq = ONLY_LONG_SEQUENCE; q->ms_mask = 0; q->max_sfb = 4;
    q->num_groups = 1; q->group_len[0] = 1;
    q->sfb[0] = S(CB_NOISE, CB_NOISE, 0);
    q->sfb[1] = S(CB_NOISE, CB_NOISE, 0);
    q->sfb[2] = S(CB_ZERO, CB_ZERO, 0);
    q->sfb[3] = S(CB_NOISE, CB_NOISE, 0);
    q->nrgL[0][0]=10; q->nrgL[0][1]=12; q->nrgL[0][3]=13;
    q->nrgR[0][0]=14; q->nrgR[0][1]=17; q->nrgR[0][3]=16;

    /* F1: correlated dual noise via per-band ms_used (mask 1). */
    q = &plans[num_plans++];
    q->seq = ONLY_LONG_SEQUENCE; q->ms_mask = 1; q->max_sfb = 4;
    q->num_groups = 1; q->group_len[0] = 1;
    q->sfb[0] = S(CB_NOISE, CB_NOISE, 1);
    q->sfb[1] = S(CB_NOISE, CB_NOISE, 0);
    q->sfb[2] = S(CB_NOISE, CB_NOISE, 1);
    q->sfb[3] = S(CB_NOISE, CB_NOISE, 0);
    q->nrgL[0][0]=10; q->nrgL[0][1]=12; q->nrgL[0][2]=11; q->nrgL[0][3]=13;
    q->nrgR[0][0]=14; q->nrgR[0][1]=14; q->nrgR[0][2]=16; q->nrgR[0][3]=15;

    /* F2: all-band correlation (mask 2), with an uncorrelated left-only
     * noise band trailing the correlated ones. */
    q = &plans[num_plans++];
    q->seq = ONLY_LONG_SEQUENCE; q->ms_mask = 2; q->max_sfb = 4;
    q->num_groups = 1; q->group_len[0] = 1;
    q->sfb[0] = S(CB_NOISE, CB_NOISE, 0);
    q->sfb[1] = S(CB_NOISE, CB_NOISE, 0);
    q->sfb[2] = S(CB_NOISE, CB_NOISE, 0);
    q->sfb[3] = S(CB_NOISE, CB_ZERO, 0);
    q->nrgL[0][0]=10; q->nrgL[0][1]=12; q->nrgL[0][2]=11; q->nrgL[0][3]=13;
    q->nrgR[0][0]=14; q->nrgR[0][1]=16; q->nrgR[0][2]=15;

    /* F3: single-side noise on alternating channels under mask 2 (the
     * witness regression scenario), followed by an uncorrelated band. */
    q = &plans[num_plans++];
    q->seq = ONLY_LONG_SEQUENCE; q->ms_mask = 2; q->max_sfb = 2;
    q->num_groups = 1; q->group_len[0] = 1;
    q->sfb[0] = S(CB_ZERO, CB_NOISE, 0);
    q->sfb[1] = S(CB_NOISE, CB_ZERO, 0);
    q->nrgL[0][1]=10;
    q->nrgR[0][0]=14;

    /* F4: left-only noise (mask 0), then an independent trailing band. */
    q = &plans[num_plans++];
    q->seq = ONLY_LONG_SEQUENCE; q->ms_mask = 0; q->max_sfb = 2;
    q->num_groups = 1; q->group_len[0] = 1;
    q->sfb[0] = S(CB_NOISE, CB_ZERO, 0);
    q->sfb[1] = S(CB_NOISE, CB_NOISE, 0);
    q->nrgL[0][0]=10; q->nrgL[0][1]=12;
    q->nrgR[0][1]=14;

    /* F5: legal long-start transition. */
    q = &plans[num_plans++];
    q->seq = LONG_START_SEQUENCE; q->ms_mask = 0; q->max_sfb = 3;
    q->num_groups = 1; q->group_len[0] = 1;
    q->sfb[0] = S(CB_NOISE, CB_NOISE, 0);
    q->sfb[1] = S(CB_ZERO, CB_ZERO, 0);
    q->sfb[2] = S(CB_NOISE, CB_NOISE, 0);
    q->nrgL[0][0]=10; q->nrgL[0][2]=13;
    q->nrgR[0][0]=14; q->nrgR[0][2]=15;

    /* F6: eight short windows, three unequal groups (lengths 3,1,4). */
    q = &plans[num_plans++];
    q->seq = EIGHT_SHORT_SEQUENCE; q->ms_mask = 1; q->max_sfb = 4;
    q->grouping = 0x67; /* bits 1100111 -> groups of lengths 3, 1, 4 */
    q->num_groups = 3;
    q->group_len[0] = 3; q->group_len[1] = 1; q->group_len[2] = 4;
    q->sfb[0] = S(CB_ZERO, CB_ZERO, 0);
    q->sfb[1] = S(CB_ONE, CB_ONE, 1);  /* ordinary spectral band, real M/S */
    q->sfb[2] = S(CB_NOISE, CB_NOISE, 1); /* correlated noise */
    q->sfb[3] = S(CB_NOISE, CB_NOISE, 0); /* independent trailing noise */
    for (g = 0; g < 3; g++)
        for (sfb = 0; sfb < 4; sfb++)
        {
            q->nrgL[g][sfb] = -1;
            q->nrgR[g][sfb] = -1;
        }
    q->nrgL[0][2]=10; q->nrgL[0][3]=14;
    q->nrgL[1][2]=12; q->nrgL[1][3]=13;
    q->nrgL[2][2]=11; q->nrgL[2][3]=15;
    q->nrgR[0][2]=14; q->nrgR[0][3]=12;
    q->nrgR[1][2]=16; q->nrgR[1][3]=13;
    q->nrgR[2][2]=15; q->nrgR[2][3]=14;

    /* F7: long-stop transition, per-band flags again. */
    q = &plans[num_plans++];
    q->seq = LONG_STOP_SEQUENCE; q->ms_mask = 1; q->max_sfb = 3;
    q->num_groups = 1; q->group_len[0] = 1;
    q->sfb[0] = S(CB_NOISE, CB_NOISE, 1);
    q->sfb[1] = S(CB_NOISE, CB_NOISE, 0);
    q->sfb[2] = S(CB_ZERO, CB_ZERO, 0);
    q->nrgL[0][0]=10; q->nrgL[0][1]=12;
    q->nrgR[0][0]=14; q->nrgR[0][1]=13;

    /* F8: back to only-long, state must still line up. */
    q = &plans[num_plans++];
    q->seq = ONLY_LONG_SEQUENCE; q->ms_mask = 0; q->max_sfb = 2;
    q->num_groups = 1; q->group_len[0] = 1;
    q->sfb[0] = S(CB_NOISE, CB_NOISE, 0);
    q->sfb[1] = S(CB_NOISE, CB_NOISE, 0);
    q->nrgL[0][0]=10; q->nrgL[0][1]=11;
    q->nrgR[0][0]=14; q->nrgR[0][1]=16;
}

/* Exact RNG cross-check against the production implementation. */
static int rng_exactness_check(void)
{
    pns_rng_t ref = { PNS_R1_INIT, PNS_R2_INIT };
    uint32_t p1 = PNS_R1_INIT, p2 = PNS_R2_INIT;
    int i;
    for (i = 0; i < 50000; i++)
    {
        uint32_t a = pns_rng_next(&ref);
        uint32_t b = ne_rng(&p1, &p2);
        if (a != b || ref.r1 != p1 || ref.r2 != p2)
        {
            printf("RNG MISMATCH step=%d ref=%08x prod=%08x\n", i, a, b);
            return 1;
        }
    }
    return 0;
}

/* The first noise energy of each channel is a 9-bit PCM word; later ones
 * are HCB_SF deltas. Verify the helper interprets them strictly in
 * transmission order, including negative deltas. */
static int noise_energy_order_check(void)
{
    const int gg = 100;
    const int first9 = 280;                 /* word 280 -> t = +24 */
    const int deltas[3] = { 2, -5, 3 };
    int e0 = pns_noise_energy(gg, first9, deltas, 0);
    int e1 = pns_noise_energy(gg, first9, deltas, 1);
    int e2 = pns_noise_energy(gg, first9, deltas, 2);
    int e3 = pns_noise_energy(gg, first9, deltas, 3);

    if (e0 != gg - 90 + 24) return 1;
    if (e1 != e0 + 2) return 1;
    if (e2 != e0 + 2 - 5) return 1;
    if (e3 != e0 + 2 - 5 + 3) return 1;
    return 0;
}

int main(void)
{
    static unsigned char packet[2048];
    static expect_t expect[MAX_FRAMES];
    NeAACDecHandle dec;
    NeAACDecConfigurationPtr cfg;
    NeAACDecFrameInfo fi;
    unsigned char asc[] = { 0x12, 0x10 };
    unsigned long rate;
    unsigned char channels;
    pns_rng_t global_rng;
    synth_t actualL, actualR, expectL, expectR;
    float pcm_actualL[FRAME], pcm_actualR[FRAME];
    float pcm_expectL[FRAME], pcm_expectR[FRAME];
    float specf[FRAME];
    unsigned f;
    long total_samples = 0;

    build_plans();
    if (rng_exactness_check())
        return 1;
    if (noise_energy_order_check())
    {
        printf("noise energy ordering check failed\n");
        return 1;
    }

    dec = NeAACDecOpen();
    cfg = NeAACDecGetCurrentConfiguration(dec);
    cfg->outputFormat = FAAD_FMT_FLOAT;
    NeAACDecSetConfiguration(dec, cfg);
    if (NeAACDecInit2(dec, asc, sizeof(asc), &rate, &channels))
    {
        printf("NeAACDecInit2 failed\n");
        return 1;
    }
    if (rate != 44100 || channels != 2)
    {
        printf("unexpected format rate=%lu ch=%u\n", rate, channels);
        return 1;
    }

    memset(&actualL, 0, sizeof(actualL));
    memset(&actualR, 0, sizeof(actualR));
    memset(&expectL, 0, sizeof(expectL));
    memset(&expectR, 0, sizeof(expectR));
    actualL.fb = filter_bank_init(FRAME);
    actualR.fb = filter_bank_init(FRAME);
    expectL.fb = filter_bank_init(FRAME);
    expectR.fb = filter_bank_init(FRAME);

    pns_rng_init(&global_rng);

    for (f = 0; f < num_plans; f++)
    {
        unsigned len = build_packet(&plans[f], packet);
        float *out;

        /* Independent expectation from band plan + RNG model. */
        build_expect(&plans[f], &expect[f], global_rng);
        global_rng = expect[f].rng;

        out = NeAACDecDecode(dec, &fi, packet, len);
        if (fi.error)
        {
            printf("packet=%u decode error %u: %s\n", f, fi.error,
                NeAACDecGetErrorMessage(fi.error));
            return 1;
        }
        if (fi.bytesconsumed != len)
        {
            printf("packet=%u consumed %lu != %u\n", f,
                (unsigned long)fi.bytesconsumed, len);
            return 1;
        }
        /* The decoder reports zero samples for the priming frame (its frame
         * counter is still <= 1); spectra and the PCM buffer are produced
         * regardless, so all numeric checks still apply to packet 0. */
        {
            unsigned long want_samples = (f == 0) ? 0u : (2u * FRAME);
            if (fi.samples != want_samples || !out)
            {
                printf("packet=%u bad sample count %lu (want %lu)\n", f,
                    (unsigned long)fi.samples, want_samples);
                return 1;
            }
        }
        total_samples += fi.samples;

        if (!cap[f].have_pns || !cap[f].have_ms)
        {
            printf("packet=%u missing hook capture\n", f);
            return 1;
        }

        /* Exact frame-end register state. */
        if (cap[f].r1 != global_rng.r1 || cap[f].r2 != global_rng.r2)
        {
            printf("RNG STATE MISMATCH packet=%u got=%08x:%08x "
                   "want=%08x:%08x\n", f, cap[f].r1, cap[f].r2,
                   global_rng.r1, global_rng.r2);
            g_failures++;
        }

        compare_spectrum("post-PNS-L", f, &plans[f], expect[f].pns_l,
            cap[f].pns_l);
        compare_spectrum("post-PNS-R", f, &plans[f], expect[f].pns_r,
            cap[f].pns_r);
        compare_spectrum("post-MS-L", f, &plans[f], expect[f].ms_l,
            cap[f].ms_l);
        compare_spectrum("post-MS-R", f, &plans[f], expect[f].ms_r,
            cap[f].ms_r);

        /* Continuous PCM via separately held filter-bank overlap state. */
        synth_run(&actualL, plans[f].seq, cap[f].ms_l, pcm_actualL);
        synth_run(&actualR, plans[f].seq, cap[f].ms_r, pcm_actualR);
        {
            int k;
            for (k = 0; k < FRAME; k++)
                specf[k] = (float)expect[f].ms_l[k];
        }
        synth_run(&expectL, plans[f].seq, specf, pcm_expectL);
        {
            int k;
            for (k = 0; k < FRAME; k++)
                specf[k] = (float)expect[f].ms_r[k];
        }
        synth_run(&expectR, plans[f].seq, specf, pcm_expectR);

        {
            int k;
            for (k = 0; k < FRAME; k++)
            {
                double ev = (double)pcm_expectL[k];
                g_compared++;
                if (fabs((double)pcm_actualL[k] - ev) > TOL(ev))
                {
                    if (g_failures < 20)
                        printf("PCM MISMATCH packet=%u ch=L bin=%d\n", f, k);
                    g_failures++;
                }
                ev = (double)pcm_expectR[k];
                g_compared++;
                if (fabs((double)pcm_actualR[k] - ev) > TOL(ev))
                {
                    if (g_failures < 20)
                        printf("PCM MISMATCH packet=%u ch=R bin=%d\n", f, k);
                    g_failures++;
                }
            }
        }

        /* Public output keeps L/R interleave order and float scaling. */
        {
            int k;
            for (k = 0; k < FRAME; k++)
            {
                float wantL = pcm_actualL[k] * (1.0f / 32768.0f);
                float wantR = pcm_actualR[k] * (1.0f / 32768.0f);
                g_compared += 2;
                if (fabsf(out[2 * k] - wantL) > 1e-6f * (1.0f + fabsf(wantL)))
                {
                    if (g_failures < 20)
                        printf("OUTPUT ORDER packet=%u bin=%d ch=L\n", f, k);
                    g_failures++;
                }
                if (fabsf(out[2 * k + 1] - wantR) >
                    1e-6f * (1.0f + fabsf(wantR)))
                {
                    if (g_failures < 20)
                        printf("OUTPUT ORDER packet=%u bin=%d ch=R\n", f, k);
                    g_failures++;
                }
                if (!isfinite(out[2 * k]) || !isfinite(out[2 * k + 1]))
                {
                    printf("NON-FINITE OUTPUT packet=%u bin=%d\n", f, k);
                    return 1;
                }
            }
        }
    }

    filter_bank_end(actualL.fb);
    filter_bank_end(actualR.fb);
    filter_bank_end(expectL.fb);
    filter_bank_end(expectR.fb);
    NeAACDecClose(dec);

    if (g_failures)
    {
        printf("FAILED packets=%u compared=%ld failures=%d\n",
            num_plans, g_compared, g_failures);
        return 1;
    }
    printf("PNS_STREAM_OK packets=%u samples=%ld compared=%ld "
           "rng_steps_exact=50000\n",
        num_plans, total_samples, g_compared);
    return 0;
}
