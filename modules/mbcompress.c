/*
 * Mbcompress
 *
 * Stereo three-band compressor using fourth-order Linkwitz-Riley
 * crossovers. Each LR branch is two cascaded second-order Butterworth
 * stages (the same bilinear-transform response as sp_butlp / sp_buthp),
 * and every stage keeps its own delay state per channel and per branch.
 *
 * An LR low/high pair tuned to the same frequency recombines to an
 * all-pass A_f(z) = LP4_f(z) + HP4_f(z), so the full three-band sum has
 * transfer function A_f1(z) * A_f2(z): unit magnitude at every
 * frequency. The low band is routed through the second crossover's
 * all-pass A_f2 as well, so it cannot skip the second crossover's phase
 * response. The dry path runs through that same combined all-pass chain,
 * which keeps every dry/wet mixture phase aligned with the wet bands.
 *
 * The filter histories and the per-band gain recursion are kept in
 * double precision in both builds; audio samples cross the public
 * SPFLOAT boundary only at the inputs and outputs. With the long
 * per-channel cascade this keeps the float build numerically aligned
 * with the unit-gain all-pass instead of accumulating roundoff across
 * the deep notches, while the USE_DOUBLE build simply passes through.
 * No memory is allocated or freed during compute.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SP_MB_ROOT2 1.4142135623730950488
#define SP_MB_FLOOR 1e-12

#include "soundpipe.h"

/* One biquad section in transposed direct form II, held in double. */
typedef struct {
    double b0, b1, b2, a1, a2;
    double s1, s2;
} mbc_biquad;

/* All filter state for one channel. The band branches and the dry
 * all-pass chain are independent sections with independent histories. */
typedef struct {
    /* first crossover, f1 */
    mbc_biquad lp1a, lp1b;   /* low path: two cascaded lowpass stages   */
    mbc_biquad hp1a, hp1b;   /* upper path: two cascaded highpass       */
    /* second crossover, f2, fed by the upper path */
    mbc_biquad lp2a, lp2b;   /* mid band: two cascaded lowpass stages   */
    mbc_biquad hp2a, hp2b;   /* high band: two cascaded highpass        */
    /* second-crossover all-pass A_f2 on the low path, so the low band
     * carries the same phase reference as the mid/high bands */
    mbc_biquad llp2a, llp2b, lhp2a, lhp2b;
    /* dry all-pass reference A_f1(z) * A_f2(z) */
    mbc_biquad dlp1a, dlp1b, dhp1a, dhp1b;
    mbc_biquad dlp2a, dlp2b, dhp2a, dhp2b;
} mbc_channel;

typedef struct {
    mbc_channel ch[2];
    double gainDb[3];   /* authoritative per-band gain, shared by L/R */
} mbc_filters;

static double mbc_tick(mbc_biquad *q, double in)
{
    double out = q->b0 * in + q->s1;
    q->s1 = q->b1 * in - q->a1 * out + q->s2;
    q->s2 = q->b2 * in - q->a2 * out;
    return out;
}

/* 2nd-order Butterworth BLT coefficients in k = tan(w_c) form,
 * algebraically identical to the sp_butlp / sp_buthp constants. */
static void mbc_lp_coeffs(mbc_biquad *q, double k)
{
    double k2 = k * k;
    double d = 1.0 / (k2 + SP_MB_ROOT2 * k + 1.0);
    q->b0 = k2 * d;
    q->b1 = 2.0 * k2 * d;
    q->b2 = k2 * d;
    q->a1 = 2.0 * (k2 - 1.0) * d;
    q->a2 = (k2 - SP_MB_ROOT2 * k + 1.0) * d;
}

static void mbc_hp_coeffs(mbc_biquad *q, double k)
{
    double k2 = k * k;
    double d = 1.0 / (k2 + SP_MB_ROOT2 * k + 1.0);
    q->b0 = d;
    q->b1 = -2.0 * d;
    q->b2 = d;
    q->a1 = 2.0 * (k2 - 1.0) * d;
    q->a2 = (k2 - SP_MB_ROOT2 * k + 1.0) * d;
}

static void mbc_zero(mbc_biquad *q)
{
    q->s1 = 0.0;
    q->s2 = 0.0;
}

/* Configure a fourth-order LR pair (two identical cascaded biquads) at
 * the given cutoff. hp == 0 builds the lowpass branch, hp != 0 the
 * highpass branch. */
static void mbc_lr4_setup(mbc_biquad *qa, mbc_biquad *qb, int hp,
                          double sr, double freq)
{
    double k = tan(M_PI * freq / sr);
    if (hp) {
        mbc_hp_coeffs(qa, k);
        mbc_hp_coeffs(qb, k);
    } else {
        mbc_lp_coeffs(qa, k);
        mbc_lp_coeffs(qb, k);
    }
    mbc_zero(qa);
    mbc_zero(qb);
}

static void mbc_channel_setup(mbc_channel *c, double sr,
                              double f1, double f2)
{
    mbc_lr4_setup(&c->lp1a, &c->lp1b, 0, sr, f1);
    mbc_lr4_setup(&c->hp1a, &c->hp1b, 1, sr, f1);
    mbc_lr4_setup(&c->lp2a, &c->lp2b, 0, sr, f2);
    mbc_lr4_setup(&c->hp2a, &c->hp2b, 1, sr, f2);
    mbc_lr4_setup(&c->llp2a, &c->llp2b, 0, sr, f2);
    mbc_lr4_setup(&c->lhp2a, &c->lhp2b, 1, sr, f2);
    mbc_lr4_setup(&c->dlp1a, &c->dlp1b, 0, sr, f1);
    mbc_lr4_setup(&c->dhp1a, &c->dhp1b, 1, sr, f1);
    mbc_lr4_setup(&c->dlp2a, &c->dlp2b, 0, sr, f2);
    mbc_lr4_setup(&c->dhp2a, &c->dhp2b, 1, sr, f2);
}

static int mbc_finite(SPFLOAT v)
{
    return isfinite((double)v);
}

int sp_mbcompress_create(sp_mbcompress **p)
{
    *p = malloc(sizeof(sp_mbcompress));
    if (*p == NULL) return SP_NOT_OK;
    memset(*p, 0, sizeof(sp_mbcompress));
    return SP_OK;
}

int sp_mbcompress_destroy(sp_mbcompress **p)
{
    sp_mbcompress *pp = *p;
    mbc_filters *f = (mbc_filters *)pp->ud;
    free(f);
    free(pp);
    *p = NULL;
    return SP_OK;
}

int sp_mbcompress_init(sp_data *sp, sp_mbcompress *p, SPFLOAT f1, SPFLOAT f2)
{
    double sr = (double)sp->sr;
    double cf1 = (double)f1, cf2 = (double)f2;
    mbc_filters *f;
    int b;

    if (!mbc_finite(f1) || !mbc_finite(f2)) return SP_NOT_OK;
    if (!(cf1 > 0.0 && cf2 > cf1 && cf2 < sr * 0.5)) return SP_NOT_OK;

    f = malloc(sizeof(mbc_filters));
    if (f == NULL) return SP_NOT_OK;
    memset(f, 0, sizeof(mbc_filters));

    p->f1 = f1;
    p->f2 = f2;
    p->sr = (SPFLOAT)sr;

    for (b = 0; b < 3; b++) {
        p->thresholdDb[b] = (SPFLOAT)-12.0;
        p->ratio[b] = (SPFLOAT)1.0;
        p->attackSeconds[b] = (SPFLOAT)0.01;
        p->releaseSeconds[b] = (SPFLOAT)0.1;
        p->gainDb[b] = (SPFLOAT)0.0;
        f->gainDb[b] = 0.0;
    }
    p->wet = (SPFLOAT)1.0;

    mbc_channel_setup(&f->ch[0], sr, cf1, cf2);
    mbc_channel_setup(&f->ch[1], sr, cf1, cf2);
    p->ud = f;
    return SP_OK;
}

/* One sample of the three analysis bands plus the phase-aligned dry
 * signal for a single channel.
 *
 * The low band is A_f2(LP_f1^2(x)) and the mid/high bands are
 * HP_f1^2(x) split at f2, so their sum factors exactly into
 * A_f1(z) * A_f2(z): the low band is not allowed to skip the second
 * crossover's phase response. */
static void mbc_channel_split(mbc_channel *c, double in,
                              double band[3], double *dry)
{
    double lowpath = mbc_tick(&c->lp1b, mbc_tick(&c->lp1a, in));
    double upper = mbc_tick(&c->hp1b, mbc_tick(&c->hp1a, in));
    double mid = mbc_tick(&c->lp2b, mbc_tick(&c->lp2a, upper));
    double high = mbc_tick(&c->hp2b, mbc_tick(&c->hp2a, upper));
    double low = mbc_tick(&c->llp2b, mbc_tick(&c->llp2a, lowpath))
               + mbc_tick(&c->lhp2b, mbc_tick(&c->lhp2a, lowpath));

    /* A_f1(z): the low and high branches of the first LR crossover. */
    double ap1 = mbc_tick(&c->dlp1b, mbc_tick(&c->dlp1a, in))
               + mbc_tick(&c->dhp1b, mbc_tick(&c->dhp1a, in));
    /* A_f1(z) * A_f2(z): same all-pass treatment at the second cutoff. */
    *dry = mbc_tick(&c->dlp2b, mbc_tick(&c->dlp2a, ap1))
         + mbc_tick(&c->dhp2b, mbc_tick(&c->dhp2a, ap1));

    band[0] = low;
    band[1] = mid;
    band[2] = high;
}

int sp_mbcompress_compute(sp_data *sp, sp_mbcompress *p,
                          SPFLOAT *inL, SPFLOAT *inR,
                          SPFLOAT *outL, SPFLOAT *outR)
{
    mbc_filters *f = (mbc_filters *)p->ud;
    double bandL[3], bandR[3], dryL, dryR, wetL, wetR;
    double xL = (double)*inL, xR = (double)*inR;
    double sr = (double)p->sr;
    int b;

    (void)sp;

    mbc_channel_split(&f->ch[0], xL, bandL, &dryL);
    mbc_channel_split(&f->ch[1], xR, bandR, &dryR);

    wetL = 0.0;
    wetR = 0.0;

    for (b = 0; b < 3; b++) {
        double thr = (double)p->thresholdDb[b];
        double ratio = (double)p->ratio[b];
        double detect, levelDb, target, coef, lin, time;

        /* stereo-linked detection: the larger channel magnitude, never
         * the sum, so equal or opposite stereo images gain equally. */
        detect = fabs(bandL[b]);
        if (fabs(bandR[b]) > detect) detect = fabs(bandR[b]);
        if (detect < SP_MB_FLOOR) detect = SP_MB_FLOOR;

        levelDb = 20.0 * log10(detect);
        target = (thr - levelDb) * (1.0 - 1.0 / ratio);
        if (target > 0.0) target = 0.0;

        /* moving toward a more negative gain is an attack, else release */
        if (target < f->gainDb[b]) {
            time = (double)p->attackSeconds[b];
        } else {
            time = (double)p->releaseSeconds[b];
        }
        if (time <= 0.0) {
            coef = 0.0; /* time of zero reaches the target immediately */
        } else {
            coef = exp(-1.0 / (sr * time));
        }
        f->gainDb[b] = target + coef * (f->gainDb[b] - target);
        lin = pow(10.0, f->gainDb[b] / 20.0);
        p->gainDb[b] = (SPFLOAT)f->gainDb[b];

        wetL += bandL[b] * lin;
        wetR += bandR[b] * lin;
    }

    double wet = (double)p->wet;
    *outL = (SPFLOAT)((1.0 - wet) * dryL + wet * wetL);
    *outR = (SPFLOAT)((1.0 - wet) * dryR + wet * wetR);
    return SP_OK;
}

int sp_mbcompress_threshold(sp_mbcompress *p, int band, SPFLOAT thresholdDb)
{
    if (band < 0 || band > 2 || !mbc_finite(thresholdDb)) return SP_NOT_OK;
    p->thresholdDb[band] = thresholdDb;
    return SP_OK;
}

int sp_mbcompress_ratio(sp_mbcompress *p, int band, SPFLOAT ratio)
{
    if (band < 0 || band > 2 || !mbc_finite(ratio) || ratio < 1.0)
        return SP_NOT_OK;
    p->ratio[band] = ratio;
    return SP_OK;
}

int sp_mbcompress_attack(sp_mbcompress *p, int band, SPFLOAT attackSeconds)
{
    if (band < 0 || band > 2 || !mbc_finite(attackSeconds) ||
        attackSeconds < 0.0)
        return SP_NOT_OK;
    p->attackSeconds[band] = attackSeconds;
    return SP_OK;
}

int sp_mbcompress_release(sp_mbcompress *p, int band, SPFLOAT releaseSeconds)
{
    if (band < 0 || band > 2 || !mbc_finite(releaseSeconds) ||
        releaseSeconds < 0.0)
        return SP_NOT_OK;
    p->releaseSeconds[band] = releaseSeconds;
    return SP_OK;
}

int sp_mbcompress_wet(sp_mbcompress *p, SPFLOAT wet)
{
    if (!mbc_finite(wet) || wet < 0.0 || wet > 1.0) return SP_NOT_OK;
    p->wet = wet;
    return SP_OK;
}

int sp_mbcompress_reset(sp_mbcompress *p)
{
    mbc_filters *f = (mbc_filters *)p->ud;
    int ch, b;

    for (ch = 0; ch < 2; ch++) {
        mbc_channel *c = &f->ch[ch];
        mbc_zero(&c->lp1a); mbc_zero(&c->lp1b);
        mbc_zero(&c->hp1a); mbc_zero(&c->hp1b);
        mbc_zero(&c->lp2a); mbc_zero(&c->lp2b);
        mbc_zero(&c->hp2a); mbc_zero(&c->hp2b);
        mbc_zero(&c->llp2a); mbc_zero(&c->llp2b);
        mbc_zero(&c->lhp2a); mbc_zero(&c->lhp2b);
        mbc_zero(&c->dlp1a); mbc_zero(&c->dlp1b);
        mbc_zero(&c->dhp1a); mbc_zero(&c->dhp1b);
        mbc_zero(&c->dlp2a); mbc_zero(&c->dlp2b);
        mbc_zero(&c->dhp2a); mbc_zero(&c->dhp2b);
    }
    for (b = 0; b < 3; b++) {
        f->gainDb[b] = 0.0;
        p->gainDb[b] = (SPFLOAT)0.0;
    }
    return SP_OK;
}
