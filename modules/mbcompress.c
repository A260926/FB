/*
 * Mbcompress
 *
 * Stereo three-band compressor built from 4th-order Linkwitz-Riley
 * crossovers. Every branch is two cascaded 2nd-order Butterworth stages
 * (the same bilinear-transform lowpass/highpass formulas as sp_butlp and
 * sp_buthp), with independent delay state per channel, branch and stage.
 * See h/mbcompress.h for the signal diagram and units.
 *
 * Numerical note: each biquad keeps double-precision coefficients and
 * delay state even in the default float build. A six-stage-deep canonical
 * direct-form crossover accumulates enough single-precision roundoff at
 * low frequencies to miss a 1e-5 float accuracy target, so the recursion
 * is evaluated in double while all public signals stay SPFLOAT. No extra
 * library or device dependency is introduced, and no allocation happens
 * inside sp_mbcompress_compute.
 */

#include <math.h>
#include <stdlib.h>
#include "soundpipe.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MB_ROOT2 1.4142135623730950488
#define MB_NBANDS 3
#define MB_FLOOR 1e-12
/* stages per type per channel: 3 branches * 2 cascade stages */
#define MB_STAGES 6

/* One 2nd-order Butterworth stage. Coefficient formulas are identical to
 * sp_butlp/sp_buthp; the widened double state is what keeps the long
 * crossover chain accurate in the float build. */
struct mb_stage {
    double b0, b1, b2, a1, a2;
    double t1, t2;
    double freq;
    int ishp;
};

static void stage_setup(struct mb_stage *q, double sr, double freq,
                        int ishp)
{
    double c, g;
    q->freq = freq;
    q->ishp = ishp;
    if (ishp)
        c = tan(M_PI / sr * freq);
    else
        c = 1.0 / tan(M_PI / sr * freq);
    g = 1.0 / (1.0 + MB_ROOT2 * c + c * c);
    q->b0 = g;
    q->b1 = ishp ? -2.0 * g : 2.0 * g;
    q->b2 = g;
    q->a1 = (ishp ? 2.0 * (c * c - 1.0)
                  : 2.0 * (1.0 - c * c)) * g;
    q->a2 = (1.0 - MB_ROOT2 * c + c * c) * g;
    q->t1 = q->t2 = 0.0;
}

static double stage_step(struct mb_stage *q, double x)
{
    double t, y;
    t = x - q->a1 * q->t1 - q->a2 * q->t2;
    y = q->b0 * t + q->b1 * q->t1 + q->b2 * q->t2;
    q->t2 = q->t1;
    q->t1 = t;
    return y;
}

/* two identical stages in series: a 4th-order LR branch */
static double stage_pair(struct mb_stage *q, double x)
{
    return stage_step(&q[1], stage_step(&q[0], x));
}

static int valid_freqs(int sr, SPFLOAT f1, SPFLOAT f2)
{
    if (sr <= 0) return SP_NOT_OK;
    if (!isfinite(f1) || !isfinite(f2)) return SP_NOT_OK;
    if (!(f1 > 0.0)) return SP_NOT_OK;
    if (!(f1 < f2)) return SP_NOT_OK;
    if (!(f2 < (SPFLOAT)sr * 0.5)) return SP_NOT_OK;
    return SP_OK;
}

int sp_mbcompress_create(sp_mbcompress **p)
{
    sp_mbcompress *pp;
    int b;
    pp = malloc(sizeof(sp_mbcompress));
    if (pp == NULL) return SP_NOT_OK;
    pp->initok = 0;
    pp->wet = 1.0;
    for (b = 0; b < MB_NBANDS; b++) {
        pp->band[b].thresholdDb = -12.0;
        pp->band[b].ratio = 1.0;
        pp->band[b].attackSeconds = 0.01;
        pp->band[b].releaseSeconds = 0.1;
        pp->gainDb[b] = 0.0;
    }
    pp->lp = NULL;
    pp->hp = NULL;
    *p = pp;
    return SP_OK;
}

/* [channel][branch][stage] flattened into the per-type stage block */
#define MB_IDX(c, b, s) ((c) * MB_STAGES + (b) * 2 + (s))

static void setup_all_stages(sp_mbcompress *p)
{
    int c, s;
    double sr = (double)p->sr;
    for (c = 0; c < 2; c++) {
        for (s = 0; s < 2; s++) {
            /* branch 0: first crossover at f1 */
            stage_setup(&p->lp[MB_IDX(c, 0, s)], sr, (double)p->f1, 0);
            stage_setup(&p->hp[MB_IDX(c, 0, s)], sr, (double)p->f1, 1);
            /* branch 1: f2 split of the low f1 path */
            stage_setup(&p->lp[MB_IDX(c, 1, s)], sr, (double)p->f2, 0);
            stage_setup(&p->hp[MB_IDX(c, 1, s)], sr, (double)p->f2, 1);
            /* branch 2: independent f2 split of the high f1 path */
            stage_setup(&p->lp[MB_IDX(c, 2, s)], sr, (double)p->f2, 0);
            stage_setup(&p->hp[MB_IDX(c, 2, s)], sr, (double)p->f2, 1);
        }
    }
}

int sp_mbcompress_destroy(sp_mbcompress **p)
{
    sp_mbcompress *pp = *p;
    if (pp->initok) {
        free(pp->lp);
        free(pp->hp);
    }
    free(pp);
    *p = NULL;
    return SP_OK;
}

int sp_mbcompress_init(sp_data *sp, sp_mbcompress *p,
                       SPFLOAT f1, SPFLOAT f2)
{
    if (valid_freqs(sp->sr, f1, f2) != SP_OK) return SP_NOT_OK;

    if (p->initok) {
        free(p->lp);
        free(p->hp);
        p->initok = 0;
    }

    p->sr = sp->sr;
    p->f1 = f1;
    p->f2 = f2;
    p->lp = malloc((size_t)(2 * MB_STAGES) * sizeof(struct mb_stage));
    p->hp = malloc((size_t)(2 * MB_STAGES) * sizeof(struct mb_stage));
    if (p->lp == NULL || p->hp == NULL) {
        free(p->lp);
        free(p->hp);
        p->lp = p->hp = NULL;
        return SP_NOT_OK;
    }
    setup_all_stages(p);

    p->wet = 1.0;
    for (int b = 0; b < MB_NBANDS; b++) {
        p->band[b].thresholdDb = -12.0;
        p->band[b].ratio = 1.0;
        p->band[b].attackSeconds = 0.01;
        p->band[b].releaseSeconds = 0.1;
        p->gainDb[b] = 0.0;
    }
    p->initok = 1;
    return SP_OK;
}

int sp_mbcompress_reset(sp_data *sp, sp_mbcompress *p)
{
    int k;
    (void)sp;
    if (!p->initok) return SP_NOT_OK;
    /* recomputing coefficients clears every delay line and keeps the
     * fixed crossover configuration intact */
    setup_all_stages(p);
    for (k = 0; k < MB_NBANDS; k++) p->gainDb[k] = 0.0;
    return SP_OK;
}

int sp_mbcompress_set_threshold(sp_mbcompress *p, int band,
                                SPFLOAT thresholdDb)
{
    if (band < 0 || band >= MB_NBANDS) return SP_NOT_OK;
    if (!isfinite(thresholdDb)) return SP_NOT_OK;
    p->band[band].thresholdDb = thresholdDb;
    return SP_OK;
}

int sp_mbcompress_set_ratio(sp_mbcompress *p, int band, SPFLOAT ratio)
{
    if (band < 0 || band >= MB_NBANDS) return SP_NOT_OK;
    if (!isfinite(ratio) || ratio < 1.0) return SP_NOT_OK;
    p->band[band].ratio = ratio;
    return SP_OK;
}

int sp_mbcompress_set_attack(sp_mbcompress *p, int band,
                             SPFLOAT attackSeconds)
{
    if (band < 0 || band >= MB_NBANDS) return SP_NOT_OK;
    if (!isfinite(attackSeconds) || attackSeconds < 0.0) return SP_NOT_OK;
    p->band[band].attackSeconds = attackSeconds;
    return SP_OK;
}

int sp_mbcompress_set_release(sp_mbcompress *p, int band,
                              SPFLOAT releaseSeconds)
{
    if (band < 0 || band >= MB_NBANDS) return SP_NOT_OK;
    if (!isfinite(releaseSeconds) || releaseSeconds < 0.0)
        return SP_NOT_OK;
    p->band[band].releaseSeconds = releaseSeconds;
    return SP_OK;
}

int sp_mbcompress_set_wet(sp_mbcompress *p, SPFLOAT wet)
{
    if (!isfinite(wet) || wet < 0.0 || wet > 1.0) return SP_NOT_OK;
    p->wet = wet;
    return SP_OK;
}

/* per-band gain in dB; one gain shared by the left and right channels */
static SPFLOAT band_gain(sp_mbcompress *p, int b, double sigL,
                         double sigR)
{
    double detect, levelDb, target, coeff, old, t;
    sp_mbcompress_band *cfg = &p->band[b];

    detect = fabs(sigL);
    if (fabs(sigR) > detect) detect = fabs(sigR);
    if (detect < MB_FLOOR) detect = MB_FLOOR;
    levelDb = 20.0 * log10(detect);

    target = ((double)cfg->thresholdDb - levelDb) *
             (1.0 - 1.0 / (double)cfg->ratio);
    if (target > 0.0) target = 0.0;

    old = (double)p->gainDb[b];
    /* moving toward a more negative gain is the attack direction */
    t = (target < old) ? (double)cfg->attackSeconds
                       : (double)cfg->releaseSeconds;

    if (t <= 0.0) {
        p->gainDb[b] = (SPFLOAT)target;
    } else {
        coeff = exp(-1.0 / ((double)p->sr * t));
        p->gainDb[b] = (SPFLOAT)(coeff * old + (1.0 - coeff) * target);
    }
    return (SPFLOAT)pow(10.0, (double)p->gainDb[b] / 20.0);
}

int sp_mbcompress_compute(sp_data *sp, sp_mbcompress *p,
                          SPFLOAT *inL, SPFLOAT *inR,
                          SPFLOAT *outL, SPFLOAT *outR)
{
    double xL, xR;
    double lowL, lowR, hiL, hiR;     /* after the f1 crossover */
    double b0L, b0R;                 /* low band */
    double mloL, mloR, mhiL, mhiR;   /* mid band's two branches */
    double b1L, b1R, b2L, b2R;       /* mid and high bands */
    double dryL, dryR, wetL, wetR, g;
    (void)sp;

    if (!p->initok) return SP_NOT_OK;

    /* widen the inputs once; sampling up front allows in-place pointers */
    xL = (double)*inL;
    xR = (double)*inR;

    /* first crossover at f1: independent state per channel */
    lowL = stage_pair(&p->lp[MB_IDX(0, 0, 0)], xL);
    lowR = stage_pair(&p->lp[MB_IDX(1, 0, 0)], xR);
    hiL  = stage_pair(&p->hp[MB_IDX(0, 0, 0)], xL);
    hiR  = stage_pair(&p->hp[MB_IDX(1, 0, 0)], xR);

    /* second crossover at f2 on both f1 branches, again with independent
     * state; splitting the low path again keeps phase alignment there. */
    b0L = stage_pair(&p->lp[MB_IDX(0, 1, 0)], lowL);
    b0R = stage_pair(&p->lp[MB_IDX(1, 1, 0)], lowR);
    mloL = stage_pair(&p->hp[MB_IDX(0, 1, 0)], lowL);
    mloR = stage_pair(&p->hp[MB_IDX(1, 1, 0)], lowR);
    mhiL = stage_pair(&p->lp[MB_IDX(0, 2, 0)], hiL);
    mhiR = stage_pair(&p->lp[MB_IDX(1, 2, 0)], hiR);
    b2L = stage_pair(&p->hp[MB_IDX(0, 2, 0)], hiL);
    b2R = stage_pair(&p->hp[MB_IDX(1, 2, 0)], hiR);

    /* Mid band = sum of the two middle branches, so the unity-gain sum
     * is exactly A_f1(z) * A_f2(z): the four products of
     * (LP1^2 + HP1^2)(LP2^2 + HP2^2), not an approximation. */
    b1L = mloL + mhiL;
    b1R = mloR + mhiR;

    /* Phase-aligned dry reference: bands reconstructed at unity gain,
     * i.e. the same all-pass response as the wet sum. The raw input is
     * never mixed back in, so dry/wet cannot carve a comb notch. */
    dryL = b0L + b1L + b2L;
    dryR = b0R + b1R + b2R;

    /* detection on max(|L|, |R|), then one linear gain for both channels */
    wetL = 0.0;
    wetR = 0.0;
    g = (double)band_gain(p, 0, b0L, b0R);
    wetL += g * b0L;
    wetR += g * b0R;
    g = (double)band_gain(p, 1, b1L, b1R);
    wetL += g * b1L;
    wetR += g * b1R;
    g = (double)band_gain(p, 2, b2L, b2R);
    wetL += g * b2L;
    wetR += g * b2R;

    *outL = (SPFLOAT)((double)p->wet * wetL +
                      (1.0 - (double)p->wet) * dryL);
    *outR = (SPFLOAT)((double)p->wet * wetR +
                      (1.0 - (double)p->wet) * dryR);
    return SP_OK;
}
