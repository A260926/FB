/*
 * checks/mbcompress.c
 *
 * Verification for sp_mbcompress. The library module is checked against
 * an independent double-precision reference written from scratch here:
 *
 *   - the crossover filters use the analog Butterworth prototype
 *     H(s) = 1 / (s^2 + sqrt(2)s + 1) discretized with a prewarped
 *     bilinear transform written as k = tan(w_c) (a different algebraic
 *     layout than the module's c = 1/tan(w_c)), each difference equation
 *     coded directly;
 *   - the per-band compressor gain is an independent standalone scalar
 *     recursion following the specified dB target and attack/release
 *     smoothing.
 *
 * No Python is used. The same source is built against the float and
 * USE_DOUBLE static libraries; the active precision is detected at
 * runtime from sizeof(SPFLOAT).
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soundpipe.h"

#define MBC_ROOT2 1.4142135623730950488
#define MBC_FLOOR 1e-12
#define NBAND 3

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR 48000.0
#define F1 300.0
#define F2 3000.0
#define N 4096

static int g_failures = 0;
static int g_cases = 0;

/* ================================================================== */
/* Independent double-precision reference                              */
/* ================================================================== */

typedef struct {
    double b0, b1, b2, a1, a2;
    double x1, x2;
} rq;

static double rq_tick(rq *q, double in)
{
    double t = in - q->a1 * q->x1 - q->a2 * q->x2;
    double y = q->b0 * t + q->b1 * q->x1 + q->b2 * q->x2;
    q->x2 = q->x1;
    q->x1 = t;
    return y;
}

/* 2nd-order Butterworth lowpass, BLT with prewarped k = tan(w_c). */
static void rq_lp(rq *q, double k)
{
    double k2 = k * k;
    double d = k2 + MBC_ROOT2 * k + 1.0;
    q->b0 = k2 / d;
    q->b1 = 2.0 * k2 / d;
    q->b2 = k2 / d;
    q->a1 = 2.0 * (k2 - 1.0) / d;
    q->a2 = (k2 - MBC_ROOT2 * k + 1.0) / d;
    q->x1 = q->x2 = 0.0;
}

/* 2nd-order Butterworth highpass, same prewarping. */
static void rq_hp(rq *q, double k)
{
    double k2 = k * k;
    double d = k2 + MBC_ROOT2 * k + 1.0;
    q->b0 = 1.0 / d;
    q->b1 = -2.0 / d;
    q->b2 = 1.0 / d;
    q->a1 = 2.0 * (k2 - 1.0) / d;
    q->a2 = (k2 - MBC_ROOT2 * k + 1.0) / d;
    q->x1 = q->x2 = 0.0;
}

typedef struct {
    rq lp1[2], hp1[2], lp2[2], hp2[2];
    /* second-crossover all-pass A_f2 on the low path */
    rq llp2[2], lhp2[2];
    rq dlp1[2], dhp1[2], dlp2[2], dhp2[2];
} rch;

typedef struct {
    double sr, f1, f2, wet;
    double thr[NBAND], ratio[NBAND], atk[NBAND], rel[NBAND];
    double gainDb[NBAND];
    rch ch[2];
} refmbc;

static void rch_setup(rch *c, double sr, double f1, double f2)
{
    double k1 = tan(M_PI * f1 / sr);
    double k2 = tan(M_PI * f2 / sr);
    rq_lp(&c->lp1[0], k1); rq_lp(&c->lp1[1], k1);
    rq_hp(&c->hp1[0], k1); rq_hp(&c->hp1[1], k1);
    rq_lp(&c->lp2[0], k2); rq_lp(&c->lp2[1], k2);
    rq_hp(&c->hp2[0], k2); rq_hp(&c->hp2[1], k2);
    rq_lp(&c->llp2[0], k2); rq_lp(&c->llp2[1], k2);
    rq_hp(&c->lhp2[0], k2); rq_hp(&c->lhp2[1], k2);
    rq_lp(&c->dlp1[0], k1); rq_lp(&c->dlp1[1], k1);
    rq_hp(&c->dhp1[0], k1); rq_hp(&c->dhp1[1], k1);
    rq_lp(&c->dlp2[0], k2); rq_lp(&c->dlp2[1], k2);
    rq_hp(&c->dhp2[0], k2); rq_hp(&c->dhp2[1], k2);
}

static void ref_init(refmbc *r, double sr, double f1, double f2)
{
    int b;
    r->sr = sr;
    r->f1 = f1;
    r->f2 = f2;
    r->wet = 1.0;
    for (b = 0; b < NBAND; b++) {
        r->thr[b] = -12.0;
        r->ratio[b] = 1.0;
        r->atk[b] = 0.01;
        r->rel[b] = 0.1;
        r->gainDb[b] = 0.0;
    }
    rch_setup(&r->ch[0], sr, f1, f2);
    rch_setup(&r->ch[1], sr, f1, f2);
}

static void rch_split(rch *c, double in, double band[NBAND], double *dry)
{
    double lowpath = rq_tick(&c->lp1[1], rq_tick(&c->lp1[0], in));
    double upper = rq_tick(&c->hp1[1], rq_tick(&c->hp1[0], in));
    double mid = rq_tick(&c->lp2[1], rq_tick(&c->lp2[0], upper));
    double high = rq_tick(&c->hp2[1], rq_tick(&c->hp2[0], upper));
    /* low band passes through the second crossover all-pass too, so
     * the band sum factors into A_f1(z) * A_f2(z) */
    double low = rq_tick(&c->llp2[1], rq_tick(&c->llp2[0], lowpath))
               + rq_tick(&c->lhp2[1], rq_tick(&c->lhp2[0], lowpath));
    double ap1 = rq_tick(&c->dlp1[1], rq_tick(&c->dlp1[0], in))
               + rq_tick(&c->dhp1[1], rq_tick(&c->dhp1[0], in));
    *dry = rq_tick(&c->dlp2[1], rq_tick(&c->dlp2[0], ap1))
         + rq_tick(&c->dhp2[1], rq_tick(&c->dhp2[0], ap1));
    band[0] = low;
    band[1] = mid;
    band[2] = high;
}

/* Standalone scalar gain step, independent of the refmbc state. */
static double scalar_target(double thr, double levelDb, double ratio)
{
    double g = (thr - levelDb) * (1.0 - 1.0 / ratio);
    return g < 0.0 ? g : 0.0;
}

static void ref_process(refmbc *r, double inL, double inR,
                        double *outL, double *outR)
{
    double bL[NBAND], bR[NBAND], dryL, dryR, wetL = 0.0, wetR = 0.0;
    int b;

    rch_split(&r->ch[0], inL, bL, &dryL);
    rch_split(&r->ch[1], inR, bR, &dryR);

    for (b = 0; b < NBAND; b++) {
        double det = fabs(bL[b]);
        double level, target, coef, lin, t;
        if (fabs(bR[b]) > det) det = fabs(bR[b]);
        if (det < MBC_FLOOR) det = MBC_FLOOR;
        level = 20.0 * log10(det);
        target = scalar_target(r->thr[b], level, r->ratio[b]);
        t = (target < r->gainDb[b]) ? r->atk[b] : r->rel[b];
        coef = (t <= 0.0) ? 0.0 : exp(-1.0 / (r->sr * t));
        r->gainDb[b] = target + coef * (r->gainDb[b] - target);
        lin = pow(10.0, r->gainDb[b] / 20.0);
        wetL += bL[b] * lin;
        wetR += bR[b] * lin;
    }

    *outL = (1.0 - r->wet) * dryL + r->wet * wetL;
    *outR = (1.0 - r->wet) * dryR + r->wet * wetR;
}

/* Extract one analysis band of a whole buffer with fresh ref filters. */
static void filter_band(const double *in, long n, int band, double *out)
{
    rch c;
    long i;
    rch_setup(&c, SR, F1, F2);
    for (i = 0; i < n; i++) {
        double b[NBAND], dry;
        rch_split(&c, in[i], b, &dry);
        out[i] = b[band];
    }
}

/* The phase-aligned dry signal A_f1(z) * A_f2(z) of a whole buffer. */
static void filter_dry(const double *in, long n, double *out)
{
    rch c;
    long i;
    rch_setup(&c, SR, F1, F2);
    for (i = 0; i < n; i++) {
        double b[NBAND], dry;
        rch_split(&c, in[i], b, &dry);
        out[i] = dry;
    }
}

/* ================================================================== */
/* Test helpers                                                        */
/* ================================================================== */

static int is_double(void)
{
    return sizeof(SPFLOAT) == sizeof(double);
}

static int close_enough(double a, double b)
{
    double atol = is_double() ? 1e-10 : 1e-5;
    double rtol = is_double() ? 1e-8 : 1e-4;
    return fabs(a - b) <= atol + rtol * fabs(b);
}

static void case_begin(const char *name)
{
    g_cases++;
    printf("CASE %s ... ", name);
}

static void case_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    g_failures++;
}

static void case_pass(void)
{
    printf("PASS prec=%s\n", is_double() ? "double" : "float");
}

static int cmp_buffers(const char *label, const double *a, const double *b,
                       long n)
{
    long i;
    for (i = 0; i < n; i++) {
        if (!close_enough(a[i], b[i])) {
            case_fail("%s mismatch at sample %ld: got %.10g ref %.10g",
                      label, i, a[i], b[i]);
            return 0;
        }
    }
    return 1;
}

/* Amplitude of a steady pure sine measured over exactly one period
 * (period = sr/freq samples, so only integer-period frequencies are
 * accepted). RMS over a complete period is independent of the sample
 * phase, unlike a sample peak. */
static double sine_amp(const double *x, long n, double freq)
{
    double period = SR / freq;
    long p = (long)(period + 0.5);
    long from, i;
    double acc = 0.0;
    if (fabs(period - (double)p) > 1e-9 || p <= 0 || p > n) return -1.0;
    from = n - p;
    for (i = from; i < n; i++) acc += x[i] * x[i];
    return sqrt(2.0 * acc / (double)p);
}

static double sine(double amp, double freq, long n)
{
    return amp * sin(2.0 * M_PI * freq * (double)n / SR);
}

static unsigned long lcg = 987654321UL;
static double noise(void)
{
    lcg = lcg * 6364136223846793005UL + 1442695040888963407UL;
    return ((double)((long)(lcg >> 33) + (1L << 31)) / (double)(1UL << 32))
           * 2.0 - 1.0;
}

/* runtime-generated non-finite values, so no precision-specific literal
 * warning can fire in the float build */
static SPFLOAT test_nan(void)
{
    volatile SPFLOAT z = 0;
    return z / z;
}

static SPFLOAT test_inf(void)
{
    volatile SPFLOAT z = 0;
    return (SPFLOAT)1 / z;
}

static sp_mbcompress *make_lib(sp_data *sp, double f1, double f2)
{
    sp_mbcompress *p = NULL;
    if (sp_mbcompress_create(&p) != SP_OK) return NULL;
    if (sp_mbcompress_init(sp, p, (SPFLOAT)f1, (SPFLOAT)f2) != SP_OK) {
        sp_mbcompress_destroy(&p);
        return NULL;
    }
    return p;
}

/* Run the library over whole buffers, converting the double test
 * signals to the build's SPFLOAT the way a real caller would. */
static void run_lib(sp_mbcompress *p, const double *inL, const double *inR,
                    double *outL, double *outR, long n, int inplace)
{
    long i;
    for (i = 0; i < n; i++) {
        SPFLOAT l = (SPFLOAT)inL[i], r = (SPFLOAT)inR[i];
        if (inplace) {
            sp_mbcompress_compute(NULL, p, &l, &r, &l, &r);
            outL[i] = l;
            outR[i] = r;
        } else {
            SPFLOAT ol = 0.0, or_ = 0.0;
            sp_mbcompress_compute(NULL, p, &l, &r, &ol, &or_);
            outL[i] = ol;
            outR[i] = or_;
        }
    }
}

static void run_ref(refmbc *r, const double *inL, const double *inR,
                    double *outL, double *outR, long n)
{
    long i;
    for (i = 0; i < n; i++)
        ref_process(r, inL[i], inR[i], &outL[i], &outR[i]);
}

/* Like run_lib, but records the module's per-sample per-band gainDb. */
static void run_lib_gains(sp_mbcompress *p,
                          const double *inL, const double *inR,
                          double *outL, double *outR, long n,
                          double gains[][NBAND])
{
    long i;
    int b;
    for (i = 0; i < n; i++) {
        SPFLOAT l = (SPFLOAT)inL[i], r = (SPFLOAT)inR[i];
        SPFLOAT ol = 0.0, or_ = 0.0;
        sp_mbcompress_compute(NULL, p, &l, &r, &ol, &or_);
        outL[i] = ol;
        outR[i] = or_;
        for (b = 0; b < NBAND; b++) gains[i][b] = p->gainDb[b];
    }
}

/* ================================================================== */
/* Named test cases                                                    */
/* ================================================================== */

/* 1. Unit-gain all-pass: with every band at ratio 1, the wet output
 * must equal the independently computed A_f1(z) * A_f2(z) response
 * sample for sample, and its steady-state magnitude is unity at every
 * probe frequency. In-place and out-of-place calls must agree. */
static void t_unity_allpass(sp_data *sp)
{
    double inL[N], inR[N], ol[N], or_[N], pl[N], pr[N], rl[N], rr[N];
    sp_mbcompress *p = make_lib(sp, F1, F2);
    sp_mbcompress *pp = make_lib(sp, F1, F2);
    refmbc r;
    long i;
    int ok = 1;
    /* all probe frequencies have an integer sample period at SR */
    double freqs[] = {30, 100, F1, 1000, F2, 8000, 12000};
    unsigned k;

    case_begin(__func__);
    ref_init(&r, SR, F1, F2);

    lcg = 11;
    for (i = 0; i < N; i++) {
        double t = (double)i / SR;
        inL[i] = 0.4 * sin(2 * M_PI * 55 * t)
               + 0.25 * sin(2 * M_PI * 1200 * t)
               + 0.2 * sin(2 * M_PI * 9000 * t) + 0.05 * noise();
        inR[i] = 0.7 * inL[i] + 0.05 * noise();
    }
    inL[77] += 0.9;   /* impulses exercise the full transient response */
    inR[203] -= 0.8;

    run_lib(p, inL, inR, ol, or_, N, 0);
    run_lib(pp, inL, inR, pl, pr, N, 1);
    ok &= cmp_buffers("in-place L", ol, pl, N);
    ok &= cmp_buffers("in-place R", or_, pr, N);

    run_ref(&r, inL, inR, rl, rr, N);
    ok &= cmp_buffers("allpass L", ol, rl, N);
    ok &= cmp_buffers("allpass R", or_, rr, N);

    for (k = 0; k < sizeof(freqs) / sizeof(freqs[0]); k++) {
        sp_mbcompress *q = make_lib(sp, F1, F2);
        double bin[N], bout[N], dummy[N], amp_in, amp_out;
        for (i = 0; i < N; i++) bin[i] = sine(0.5, freqs[k], i);
        run_lib(q, bin, bin, bout, dummy, N, 0);
        amp_in = sine_amp(bin, N, freqs[k]);
        amp_out = sine_amp(bout, N, freqs[k]);
        sp_mbcompress_destroy(&q);
        if (!close_enough(amp_in, amp_out)) {
            case_fail("unity magnitude at %.0f Hz: in %.6f out %.6f",
                      freqs[k], amp_in, amp_out);
            ok = 0;
        }
    }

    sp_mbcompress_destroy(&p);
    sp_mbcompress_destroy(&pp);
    if (ok) case_pass();
}

/* 2. Crossover alignment: at f1 the low band and the upper branch each
 * carry half the amplitude (-6 dB) in phase; at f2 the same holds for
 * the mid/high branches. Their in-phase sum is the all-pass, and the
 * library output matches the reference. */
static void t_crossover_alignment(sp_data *sp)
{
    long i;
    int ok = 1;

    case_begin(__func__);

    /* f1: low branch vs the rest of the spectrum, both at -6 dB */
    {
        double lo[N], up[N];
        {
            rch c;
            rch_setup(&c, SR, F1, F2);
            for (i = 0; i < N; i++) {
                double in = sine(0.6, F1, i), b[NBAND], dry;
                rch_split(&c, in, b, &dry);
                lo[i] = b[0];
                up[i] = b[1] + b[2];
                if (i >= N - 1024 && !close_enough(lo[i] + up[i], dry)) {
                    case_fail("f1 branches out of phase at sample %ld", i);
                    ok = 0;
                    break;
                }
            }
        }
        if (ok && (!close_enough(sine_amp(lo, N, F1), 0.3) ||
                   !close_enough(sine_amp(up, N, F1), 0.3))) {
            case_fail("f1 -6 dB branches: low %.6f upper %.6f want 0.3",
                      sine_amp(lo, N, F1), sine_amp(up, N, F1));
            ok = 0;
        }
    }

    /* f2: mid vs high branches. f2 is not the first crossover's cutoff,
     * so the -6 dB expectation is stated relative to the signal arriving
     * from the first highpass branch (measured through the reference). */
    if (ok) {
        rch c;
        double mid[N], hi[N], upperbuf[N];
        rch_setup(&c, SR, F1, F2);
        for (i = 0; i < N; i++) {
            double in = sine(0.6, F2, i), b[NBAND], dry;
            rch_split(&c, in, b, &dry);
            mid[i] = b[1];
            hi[i] = b[2];
            /* at the cutoff the two LR4 branches are in phase and
             * equal in amplitude, so they match sample for sample */
            if (i >= N - 1024 && !close_enough(mid[i], hi[i])) {
                case_fail("f2 branches out of phase at sample %ld", i);
                ok = 0;
                break;
            }
        }
        /* the first crossover's high branch at f2, measured directly */
        rch_setup(&c, SR, F1, F2);
        for (i = 0; i < N; i++) {
            double in = sine(0.6, F2, i);
            upperbuf[i] = rq_tick(&c.hp1[1], rq_tick(&c.hp1[0], in));
        }
        if (ok) {
            double upper_amp = sine_amp(upperbuf, N, F2);
            double mid_amp = sine_amp(mid, N, F2);
            double hi_amp = sine_amp(hi, N, F2);
            if (!close_enough(mid_amp, 0.5 * upper_amp) ||
                !close_enough(hi_amp, 0.5 * upper_amp) ||
                !close_enough(mid_amp, hi_amp)) {
                case_fail("f2 -6 dB branches: mid %.6f high %.6f want %.6f",
                          mid_amp, hi_amp, 0.5 * upper_amp);
                ok = 0;
            }
        }
    }

    /* library vs reference on mixed crossover tones */
    if (ok) {
        sp_mbcompress *p = make_lib(sp, F1, F2);
        refmbc r;
        double inL[N], ol[N], or_[N], rl[N], rr[N];
        ref_init(&r, SR, F1, F2);
        for (i = 0; i < N; i++) {
            inL[i] = sine(0.6, F1, i);
            if (i > N / 2) inL[i] += sine(0.4, F2, i);
        }
        run_lib(p, inL, inL, ol, or_, N, 0);
        run_ref(&r, inL, inL, rl, rr, N);
        ok = cmp_buffers("crossover library output", ol, rl, N);
        sp_mbcompress_destroy(&p);
    }

    if (ok) case_pass();
}

/* 3. Two-tone band selectivity: a two-tone signal only develops gain
 * reduction in the band whose compressor is enabled; the other bands'
 * gain states stay at 0 dB. The active band's gainDb must match an
 * independent scalar recursion, and the whole output must match the
 * full reference implementation. */
static void t_band_selectivity(sp_data *sp)
{
    long i;
    int ok = 1;

    case_begin(__func__);

    /* low-band compressor only: 60 Hz compressed, 9 kHz left alone */
    {
        sp_mbcompress *p = make_lib(sp, F1, F2);
        refmbc r;
        double in[N], out[N], dummy[N], rl[N], rr[N];
        double gains[N][NBAND], band0[N];
        double thr = -20.0, ratio = 4.0, g = 0.0;
        sp_mbcompress_threshold(p, 0, thr);
        sp_mbcompress_ratio(p, 0, ratio);
        sp_mbcompress_attack(p, 0, 0.0);
        sp_mbcompress_release(p, 0, 0.0);

        for (i = 0; i < N; i++)
            in[i] = 0.7 * sine(1.0, 60.0, i) + 0.4 * sine(1.0, 9000.0, i);
        run_lib_gains(p, in, in, out, dummy, N, gains);

        /* inactive bands never leave 0 dB, at any sample */
        for (i = 0; i < N; i++) {
            if (gains[i][1] != 0.0 || gains[i][2] != 0.0) {
                case_fail("inactive bands gained reduction at sample %ld", i);
                ok = 0;
                break;
            }
        }

        /* active band matches the standalone scalar recursion per sample */
        if (ok) {
            filter_band(in, N, 0, band0);
            for (i = 0; i < N; i++) {
                double det = fabs(band0[i]);
                double level, target;
                if (det < MBC_FLOOR) det = MBC_FLOOR;
                level = 20.0 * log10(det);
                target = scalar_target(thr, level, ratio);
                g = target;  /* instant time constants */
                if (!close_enough(gains[i][0], g)) {
                    case_fail("low band gainDb at %ld: %.8g scalar %.8g",
                              i, gains[i][0], g);
                    ok = 0;
                    break;
                }
            }
        }

        ref_init(&r, SR, F1, F2);
        r.thr[0] = thr;
        r.ratio[0] = ratio;
        r.atk[0] = r.rel[0] = 0.0;
        run_ref(&r, in, in, rl, rr, N);
        ok &= cmp_buffers("selective low compression", out, rl, N);
        sp_mbcompress_destroy(&p);
    }

    /* mid-band compressor only: 1.2 kHz compressed, outer bands stay */
    if (ok) {
        sp_mbcompress *p = make_lib(sp, F1, F2);
        refmbc r;
        double in[N], out[N], dummy[N], rl[N], rr[N];
        double gains[N][NBAND], band1[N];
        double thr = -24.0, ratio = 8.0, g = 0.0;
        sp_mbcompress_threshold(p, 1, thr);
        sp_mbcompress_ratio(p, 1, ratio);
        sp_mbcompress_attack(p, 1, 0.0);
        sp_mbcompress_release(p, 1, 0.0);
        for (i = 0; i < N; i++)
            in[i] = 0.5 * sine(1.0, 60.0, i)
                  + 0.5 * sine(1.0, 1200.0, i)
                  + 0.5 * sine(1.0, 9000.0, i);
        run_lib_gains(p, in, in, out, dummy, N, gains);

        for (i = 0; i < N; i++) {
            if (gains[i][0] != 0.0 || gains[i][2] != 0.0) {
                case_fail("outer bands gained reduction at sample %ld", i);
                ok = 0;
                break;
            }
        }
        if (ok) {
            filter_band(in, N, 1, band1);
            for (i = 0; i < N; i++) {
                double det = fabs(band1[i]);
                double level, target;
                if (det < MBC_FLOOR) det = MBC_FLOOR;
                level = 20.0 * log10(det);
                target = scalar_target(thr, level, ratio);
                g = target;
                if (!close_enough(gains[i][1], g)) {
                    case_fail("mid band gainDb at %ld: %.8g scalar %.8g",
                              i, gains[i][1], g);
                    ok = 0;
                    break;
                }
            }
        }

        ref_init(&r, SR, F1, F2);
        r.thr[1] = thr;
        r.ratio[1] = ratio;
        r.atk[1] = r.rel[1] = 0.0;
        run_ref(&r, in, in, rl, rr, N);
        ok &= cmp_buffers("selective mid compression", out, rl, N);
        sp_mbcompress_destroy(&p);
    }

    if (ok) case_pass();
}

/* 4. Stereo-linked gain: the same linear gain lands on both channels
 * of a band. A constant-ratio image and an inverse-polarity image must
 * keep their relationship, and the gain must match a standalone scalar
 * recursion driven by the larger channel magnitude (never the sum). */
static void t_stereo_link(sp_data *sp)
{
    double inL[N], inR[N], ol[N], or_[N], rl[N], rr[N];
    int pass, ok = 1;
    long i;

    case_begin(__func__);
    lcg = 27;

    for (pass = 0; pass < 2; pass++) {
        sp_mbcompress *p = make_lib(sp, F1, F2);
        refmbc r;
        double k = (pass == 0) ? 0.6 : -1.0;

        for (i = 0; i < N; i++) {
            double t = (double)i / SR;
            inL[i] = 0.5 * sin(2 * M_PI * 220 * t)
                   + 0.25 * sin(2 * M_PI * 4200 * t) + 0.08 * noise();
            inR[i] = k * inL[i];
        }

        if (pass == 1) {
            int b;
            for (b = 0; b < NBAND; b++) {
                sp_mbcompress_threshold(p, b, -18.0);
                sp_mbcompress_ratio(p, b, 5.0);
                sp_mbcompress_attack(p, b, 0.0);
                sp_mbcompress_release(p, b, 0.0);
            }
        }
        ref_init(&r, SR, F1, F2);
        if (pass == 1) {
            int b;
            for (b = 0; b < NBAND; b++) {
                r.thr[b] = -18.0;
                r.ratio[b] = 5.0;
                r.atk[b] = r.rel[b] = 0.0;
            }
        }

        run_lib(p, inL, inR, ol, or_, N, 0);
        run_ref(&r, inL, inR, rl, rr, N);
        ok &= cmp_buffers(pass == 0 ? "ratio image L" : "polarity image L",
                          ol, rl, N);
        ok &= cmp_buffers(pass == 0 ? "ratio image R" : "polarity image R",
                          or_, rr, N);

        for (i = N - 2048; i < N; i++) {
            if (!close_enough(or_[i], k * ol[i])) {
                case_fail("stereo relationship broken at sample %ld "
                          "(pass %d): %.8g vs %.8g", i, pass, or_[i],
                          k * ol[i]);
                ok = 0;
                break;
            }
        }
        sp_mbcompress_destroy(&p);
    }

    /* explicit scalar recursion on the max-magnitude detector: the
     * recorded low-band gainDb must reproduce a standalone scalar
     * recursion driven by the larger channel magnitude (never the sum),
     * and one identical linear gain applies to both channels. */
    if (ok) {
        sp_mbcompress *p = make_lib(sp, F1, F2);
        double l_in[N], r_in[N], ol[N], or_[N], dummy[N];
        double gains[N][NBAND], bl[N], br[N];
        double g = 0.0, thr = -16.0, ratio = 3.0;
        sp_mbcompress_threshold(p, 0, thr);
        sp_mbcompress_ratio(p, 0, ratio);
        sp_mbcompress_attack(p, 0, 0.005);
        sp_mbcompress_release(p, 0, 0.07);
        for (i = 0; i < N; i++) {
            l_in[i] = 0.8 * sine(1.0, 90.0, i);   /* louder channel */
            r_in[i] = 0.3 * sine(1.0, 90.0, i);
        }
        run_lib_gains(p, l_in, r_in, ol, or_, N, gains);
        filter_band(l_in, N, 0, bl);
        filter_band(r_in, N, 0, br);

        for (i = 0; i < N; i++) {
            double det = fabs(bl[i]);
            double level, target, coef;
            if (fabs(br[i]) > det) det = fabs(br[i]);
            if (det < MBC_FLOOR) det = MBC_FLOOR;
            level = 20.0 * log10(det);
            target = scalar_target(thr, level, ratio);
            coef = (target < g) ? exp(-1.0 / (SR * 0.005))
                                : exp(-1.0 / (SR * 0.07));
            g = target + coef * (g - target);
            if (!close_enough(gains[i][0], g)) {
                case_fail("shared scalar gain mismatch at sample %ld: "
                          "%.8g vs %.8g", i, gains[i][0], g);
                ok = 0;
                break;
            }
        }
        sp_mbcompress_destroy(&p);
        (void)dummy;
    }

    if (ok) case_pass();
}

/* 5. dry/wet at 0, 0.5 and 1: wet=0 must be the all-pass-aligned dry
 * signal (not the raw input), wet=1 the compressed wet signal, and the
 * middle value exactly the linear blend of the two endpoints. */
static void t_dry_wet(sp_data *sp)
{
    double in[N], w0[N], w1[N], wh[N], dummy[N], rl[N], rr[N], dryref[N];
    sp_mbcompress *p0 = make_lib(sp, F1, F2);
    sp_mbcompress *p1 = make_lib(sp, F1, F2);
    sp_mbcompress *ph = make_lib(sp, F1, F2);
    refmbc r;
    long i;
    int ok = 1;
    double raw_gap;

    case_begin(__func__);
    for (i = 0; i < N; i++)
        in[i] = sine(0.5, F1, i) + 0.3 * sine(1.0, 120.0, i);

    sp_mbcompress_wet(p0, 0.0);
    sp_mbcompress_wet(p1, 1.0);
    sp_mbcompress_wet(ph, 0.5);
    sp_mbcompress_threshold(p1, 0, -20.0);
    sp_mbcompress_ratio(p1, 0, 4.0);
    sp_mbcompress_threshold(ph, 0, -20.0);
    sp_mbcompress_ratio(ph, 0, 4.0);

    run_lib(p0, in, in, w0, dummy, N, 0);
    run_lib(p1, in, in, w1, dummy, N, 0);
    run_lib(ph, in, in, wh, dummy, N, 0);

    /* wet=0 is the phase-aligned dry reference, not the raw input */
    filter_dry(in, N, dryref);
    ok &= cmp_buffers("wet=0 aligned dry", w0, dryref, N);
    raw_gap = 0.0;
    for (i = N - 1024; i < N; i++)
        if (fabs(w0[i] - in[i]) > raw_gap) raw_gap = fabs(w0[i] - in[i]);
    if (raw_gap < 0.1) {
        case_fail("wet=0 looks like the unprocessed input (gap %.4f)",
                  raw_gap);
        ok = 0;
    }

    /* wet=1 against the full independent reference */
    ref_init(&r, SR, F1, F2);
    r.thr[0] = -20.0;
    r.ratio[0] = 4.0;
    run_ref(&r, in, in, rl, rr, N);
    ok &= cmp_buffers("wet=1 path", w1, rl, N);

    /* wet=0.5 is exactly the linear blend of the endpoints */
    for (i = 0; i < N; i++) {
        double blend = 0.5 * (w0[i] + w1[i]);
        if (!close_enough(wh[i], blend)) {
            case_fail("wet=0.5 not linear at sample %ld: %.8g vs %.8g",
                      i, wh[i], blend);
            ok = 0;
            break;
        }
    }

    sp_mbcompress_destroy(&p0);
    sp_mbcompress_destroy(&p1);
    sp_mbcompress_destroy(&ph);
    if (ok) case_pass();
}

/* 6. Zero attack/release, mid-stream parameter updates and reset:
 *  - zero time constants snap the gain to the scalar target in one
 *    sample;
 *  - valid updates apply from the next sample without clearing any
 *    filter or gain history, while invalid arguments are rejected and
 *    leave the configuration intact;
 *  - reset clears all histories and restores unity gains but keeps the
 *    configuration, matching a freshly created and configured instance. */
static void t_envelope_params_reset(sp_data *sp)
{
    long i;
    int ok = 1, b;

    case_begin(__func__);

    /* zero attack/release: per-sample gain snaps to the scalar target */
    {
        sp_mbcompress *p = make_lib(sp, F1, F2);
        double in[N], out[N], dummy[N], b_in[N], gains[N][NBAND];
        double thr = -18.0, ratio = 6.0, g = 0.0;
        sp_mbcompress_threshold(p, 1, thr);
        sp_mbcompress_ratio(p, 1, ratio);
        sp_mbcompress_attack(p, 1, 0.0);
        sp_mbcompress_release(p, 1, 0.0);
        for (i = 0; i < N; i++)
            in[i] = (i < 512) ? 0.1 * sine(1.0, 1000.0, i)
                              : 0.8 * sine(1.0, 1000.0, i);
        run_lib_gains(p, in, in, out, dummy, N, gains);
        filter_band(in, N, 1, b_in);

        for (i = 0; i < N; i++) {
            double det = fabs(b_in[i]);
            double level, target;
            if (det < MBC_FLOOR) det = MBC_FLOOR;
            level = 20.0 * log10(det);
            target = scalar_target(thr, level, ratio);
            g = target;  /* coef = 0: instant reach */
            if (!close_enough(gains[i][1], g)) {
                case_fail("instant gainDb wrong at sample %ld: %.8g "
                          "scalar %.8g", i, gains[i][1], g);
                ok = 0;
                break;
            }
        }
        sp_mbcompress_destroy(&p);
    }

    /* mid-stream parameter updates take effect on the next sample */
    if (ok) {
        sp_mbcompress *p = make_lib(sp, F1, F2);
        refmbc r;
        double inL[N], inR[N], ol[N], or_[N], rl[N], rr[N];
        lcg = 71;
        for (i = 0; i < N; i++) {
            double t = (double)i / SR;
            inL[i] = 0.6 * sin(2 * M_PI * 90 * t)
                   + 0.4 * sin(2 * M_PI * 1500 * t) + 0.05 * noise();
            inR[i] = 0.8 * inL[i] + 0.05 * noise();
        }
        ref_init(&r, SR, F1, F2);
        run_lib(p, inL, inR, ol, or_, 1000, 0);
        for (i = 0; i < 1000; i++)
            ref_process(&r, inL[i], inR[i], &rl[i], &rr[i]);

        sp_mbcompress_threshold(p, 1, -30.0);
        sp_mbcompress_ratio(p, 1, 3.0);
        sp_mbcompress_attack(p, 1, 0.003);
        sp_mbcompress_release(p, 1, 0.05);
        sp_mbcompress_threshold(p, 0, -22.0);
        sp_mbcompress_wet(p, 0.7);
        r.thr[1] = -30.0;
        r.ratio[1] = 3.0;
        r.atk[1] = 0.003;
        r.rel[1] = 0.05;
        r.thr[0] = -22.0;
        r.wet = 0.7;
        for (i = 1000; i < N; i++) {
            SPFLOAT l = (SPFLOAT)inL[i], rr_ = (SPFLOAT)inR[i];
            SPFLOAT a = 0.0, c = 0.0;
            sp_mbcompress_compute(NULL, p, &l, &rr_, &a, &c);
            ol[i] = a; or_[i] = c;
            ref_process(&r, inL[i], inR[i], &rl[i], &rr[i]);
        }
        ok &= cmp_buffers("post-update L", ol, rl, N);
        ok &= cmp_buffers("post-update R", or_, rr, N);

        /* invalid setters are rejected and leave the config intact */
        if (sp_mbcompress_ratio(p, 1, 0.5) != SP_NOT_OK ||
            sp_mbcompress_attack(p, 2, -0.01) != SP_NOT_OK ||
            sp_mbcompress_release(p, 0, -1.0) != SP_NOT_OK ||
            sp_mbcompress_wet(p, 1.1) != SP_NOT_OK ||
            sp_mbcompress_wet(p, -0.1) != SP_NOT_OK ||
            sp_mbcompress_threshold(p, 3, -10.0) != SP_NOT_OK ||
            sp_mbcompress_ratio(p, -1, 2.0) != SP_NOT_OK ||
            sp_mbcompress_wet(p, test_nan()) != SP_NOT_OK ||
            sp_mbcompress_threshold(p, 0, test_inf()) != SP_NOT_OK) {
            case_fail("an invalid parameter was accepted");
            ok = 0;
        }
        if (p->ratio[1] != (SPFLOAT)3.0 || p->wet != (SPFLOAT)0.7 ||
            p->thresholdDb[0] != (SPFLOAT)(-22.0)) {
            case_fail("a rejected update modified the configuration");
            ok = 0;
        }
        sp_mbcompress_destroy(&p);
    }

    /* illegal init frequencies are rejected */
    if (ok) {
        sp_mbcompress *bad = NULL;
        sp_mbcompress_create(&bad);
        if (sp_mbcompress_init(sp, bad, 0.0, F2) == SP_OK ||
            sp_mbcompress_init(sp, bad, -1.0, F2) == SP_OK ||
            sp_mbcompress_init(sp, bad, F2, F1) == SP_OK ||
            sp_mbcompress_init(sp, bad, F1, SR / 2.0) == SP_OK ||
            sp_mbcompress_init(sp, bad, F1, SR) == SP_OK ||
            sp_mbcompress_init(sp, bad, test_nan(), F2) == SP_OK ||
            sp_mbcompress_init(sp, bad, F1, test_inf()) == SP_OK) {
            case_fail("an illegal crossover frequency was accepted");
            ok = 0;
        }
        sp_mbcompress_destroy(&bad);
    }

    /* reset: histories cleared, gains back to 1, configuration kept */
    if (ok) {
        sp_mbcompress *p = make_lib(sp, F1, F2);
        sp_mbcompress *fresh;
        double inL[N], inR[N], ol[N], or_[N], fl[N], fr[N];
        lcg = 123;
        for (i = 0; i < N; i++) {
            double t = (double)i / SR;
            inL[i] = 0.7 * sin(2 * M_PI * 70 * t)
                   + 0.3 * sin(2 * M_PI * 2400 * t) + 0.05 * noise();
            inR[i] = -0.6 * inL[i];
        }
        for (b = 0; b < NBAND; b++) {
            sp_mbcompress_threshold(p, b, -16.0 - 2.0 * b);
            sp_mbcompress_ratio(p, b, 2.0 + b);
            sp_mbcompress_attack(p, b, 0.004 + 0.001 * b);
            sp_mbcompress_release(p, b, 0.06 + 0.01 * b);
        }
        sp_mbcompress_wet(p, 0.6);
        run_lib(p, inL, inR, ol, or_, N, 0);  /* build up histories */

        if (sp_mbcompress_reset(p) != SP_OK) {
            case_fail("reset returned an error");
            ok = 0;
        }
        for (b = 0; b < NBAND; b++) {
            if (p->gainDb[b] != 0.0) {
                case_fail("band %d gain not restored to 1 after reset", b);
                ok = 0;
            }
        }
        if (p->wet != (SPFLOAT)0.6 || p->ratio[2] != (SPFLOAT)4.0 ||
            p->thresholdDb[1] != (SPFLOAT)(-18.0) ||
            p->attackSeconds[0] != (SPFLOAT)0.004 ||
            p->releaseSeconds[2] != (SPFLOAT)0.08) {
            case_fail("reset changed the stored configuration");
            ok = 0;
        }

        fresh = make_lib(sp, F1, F2);
        for (b = 0; b < NBAND; b++) {
            sp_mbcompress_threshold(fresh, b, -16.0 - 2.0 * b);
            sp_mbcompress_ratio(fresh, b, 2.0 + b);
            sp_mbcompress_attack(fresh, b, 0.004 + 0.001 * b);
            sp_mbcompress_release(fresh, b, 0.06 + 0.01 * b);
        }
        sp_mbcompress_wet(fresh, 0.6);
        run_lib(p, inL, inR, ol, or_, N, 0);
        run_lib(fresh, inL, inR, fl, fr, N, 0);
        ok &= cmp_buffers("reset vs fresh L", ol, fl, N);
        ok &= cmp_buffers("reset vs fresh R", or_, fr, N);

        sp_mbcompress_destroy(&p);
        sp_mbcompress_destroy(&fresh);
    }

    if (ok) case_pass();
}

int main(void)
{
    sp_data *sp;

    sp_create(&sp);
    sp->sr = (int)SR;

    printf("mbcompress checks, SPFLOAT size = %d bytes (%s)\n",
           (int)sizeof(SPFLOAT), is_double() ? "double" : "float");

    t_unity_allpass(sp);
    t_crossover_alignment(sp);
    t_band_selectivity(sp);
    t_stereo_link(sp);
    t_dry_wet(sp);
    t_envelope_params_reset(sp);

    sp_destroy(&sp);

    printf("ran %d named cases, %d failure(s)\n", g_cases, g_failures);
    return g_failures == 0 ? 0 : 1;
}
