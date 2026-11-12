/*
 * mbcompress.c - verification for sp_mbcompress.
 *
 * The module is checked against an independent double-precision
 * difference-equation reference (2nd-order Butterworth biquads built
 * from the bilinear-transform coefficients, cascaded into 4th-order
 * Linkwitz-Riley branches) and an independent scalar dB-gain recursion.
 * No Python is used anywhere.
 */

#include "soundpipe.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR 48000.0
#define N 4800
#define F1 250.0
#define F2 2500.0
#define FLOOR 1e-12
#define ROOT2 1.4142135623730950488
#define NB 3

#ifdef USE_DOUBLE
#define IS_DOUBLE 1
#else
#define IS_DOUBLE 0
#endif

static int g_fail = 0;
static int g_checks = 0;

static double atol(void) { return IS_DOUBLE ? 1e-10 : 1e-5; }
static double rtol(void) { return IS_DOUBLE ? 1e-8 : 1e-4; }

static int close_enough(double got, double ref)
{
    return fabs(got - ref) <= atol() + rtol() * fabs(ref);
}

static void check(int cond, const char *what)
{
    g_checks++;
    if (!cond) {
        g_fail = 1;
        printf("  FAIL: %s\n", what);
    }
}

/* wrapped absolute phase difference in [0, pi] */
static double phase_diff(double a, double b)
{
    double d = fmod(a - b + M_PI, 2.0 * M_PI);
    if (d < 0.0) d += 2.0 * M_PI;
    return fabs(d - M_PI);
}

/* ---------------- independent double reference ---------------- */

typedef struct {
    double a[5]; /* b0 b1 b2 a1 a2 */
    double t1, t2;
} Biq;

static void biq_lp(Biq *q, double f)
{
    double c = 1.0 / tan(M_PI / SR * f);
    double g = 1.0 / (1.0 + ROOT2 * c + c * c);
    q->a[0] = g;
    q->a[1] = 2.0 * g;
    q->a[2] = g;
    q->a[3] = 2.0 * (1.0 - c * c) * g;
    q->a[4] = (1.0 - ROOT2 * c + c * c) * g;
    q->t1 = q->t2 = 0.0;
}

static void biq_hp(Biq *q, double f)
{
    double c = tan(M_PI / SR * f);
    double g = 1.0 / (1.0 + ROOT2 * c + c * c);
    q->a[0] = g;
    q->a[1] = -2.0 * g;
    q->a[2] = g;
    q->a[3] = 2.0 * (c * c - 1.0) * g;
    q->a[4] = (1.0 - ROOT2 * c + c * c) * g;
    q->t1 = q->t2 = 0.0;
}

static double biq_step(Biq *q, double x)
{
    double t = x - q->a[3] * q->t1 - q->a[4] * q->t2;
    double y = q->a[0] * t + q->a[1] * q->t1 + q->a[2] * q->t2;
    q->t2 = q->t1;
    q->t1 = t;
    return y;
}

typedef struct {
    double thresh, ratio, atk, rel;
} RefCfg;

typedef struct {
    /* per channel: 2 LP(f1), 2 HP(f1), and for the f2 split of each
       f1 branch an independent LP/HP cascade (low path and high path) */
    Biq lp1[2][2], hp1[2][2];
    Biq lp2lo[2][2], hp2lo[2][2], lp2hi[2][2], hp2hi[2][2];
    /* alternative all-pass cascade A_f1 then A_f2 */
    Biq ap1_lp[2][2], ap1_hp[2][2], ap2_lp[2][2], ap2_hp[2][2];
    double gaindb[NB];
    RefCfg cfg[NB];
    double wet;
    double last_gain[NB];
    double gainhist[NB][N];
    double band[2][NB];   /* last step's pre-gain band signals */
    int hist_pos;
} Ref;

static void ref_init(Ref *r)
{
    int c, s;
    memset(r, 0, sizeof(*r));
    for (c = 0; c < 2; c++) {
        for (s = 0; s < 2; s++) {
            biq_lp(&r->lp1[c][s], F1);
            biq_hp(&r->hp1[c][s], F1);
            biq_lp(&r->lp2lo[c][s], F2);
            biq_hp(&r->hp2lo[c][s], F2);
            biq_lp(&r->lp2hi[c][s], F2);
            biq_hp(&r->hp2hi[c][s], F2);
            biq_lp(&r->ap1_lp[c][s], F1);
            biq_hp(&r->ap1_hp[c][s], F1);
            biq_lp(&r->ap2_lp[c][s], F2);
            biq_hp(&r->ap2_hp[c][s], F2);
        }
    }
    for (int b = 0; b < NB; b++) {
        r->cfg[b].thresh = -12.0;
        r->cfg[b].ratio = 1.0;
        r->cfg[b].atk = 0.01;
        r->cfg[b].rel = 0.1;
        r->gaindb[b] = 0.0;
    }
    r->wet = 1.0;
    r->hist_pos = 0;
}

static double ref_band_gain(Ref *r, int b, double sL, double sR)
{
    double det, level, target, coeff, old, t;
    det = fabs(sL);
    if (fabs(sR) > det) det = fabs(sR);
    if (det < FLOOR) det = FLOOR;
    level = 20.0 * log10(det);
    target = (r->cfg[b].thresh - level) *
             (1.0 - 1.0 / r->cfg[b].ratio);
    if (target > 0.0) target = 0.0;
    old = r->gaindb[b];
    t = (target < old) ? r->cfg[b].atk : r->cfg[b].rel;
    if (t <= 0.0) {
        r->gaindb[b] = target;
    } else {
        coeff = exp(-1.0 / (SR * t));
        r->gaindb[b] = coeff * old + (1.0 - coeff) * target;
    }
    r->last_gain[b] = pow(10.0, r->gaindb[b] / 20.0);
    r->gainhist[b][r->hist_pos % N] = r->last_gain[b];
    return r->last_gain[b];
}

/* full reference step; dry and all-pass-cascade outputs exposed */
static void ref_step(Ref *r, double xL, double xR,
                     double *oL, double *oR,
                     double *dryL, double *dryR, double *apL, double *apR)
{
    double low[2], hi[2], b0[2], b1[2], b2[2];
    double wet[2] = {0.0, 0.0}, dry[2], ap[2];
    int c;
    double xv[2] = {xL, xR};
    for (c = 0; c < 2; c++) {
        double a1, h1, a2, mlo, mhi;
        low[c] = biq_step(&r->lp1[c][0], xv[c]);
        low[c] = biq_step(&r->lp1[c][1], low[c]);
        hi[c] = biq_step(&r->hp1[c][0], xv[c]);
        hi[c] = biq_step(&r->hp1[c][1], hi[c]);

        /* f2 split of the low f1 branch */
        b0[c] = biq_step(&r->lp2lo[c][0], low[c]);
        b0[c] = biq_step(&r->lp2lo[c][1], b0[c]);
        mlo = biq_step(&r->hp2lo[c][0], low[c]);
        mlo = biq_step(&r->hp2lo[c][1], mlo);
        /* f2 split of the high f1 branch */
        mhi = biq_step(&r->lp2hi[c][0], hi[c]);
        mhi = biq_step(&r->lp2hi[c][1], mhi);
        b2[c] = biq_step(&r->hp2hi[c][0], hi[c]);
        b2[c] = biq_step(&r->hp2hi[c][1], b2[c]);
        /* mid band is the sum of the two middle branches */
        b1[c] = mlo + mhi;
        dry[c] = b0[c] + b1[c] + b2[c];

        /* alternative construction: A_f1 = LP1^2 + HP1^2,
           then A_f2 = LP2^2 + HP2^2, cascaded */
        a1 = biq_step(&r->ap1_lp[c][0], xv[c]);
        a1 = biq_step(&r->ap1_lp[c][1], a1);
        h1 = biq_step(&r->ap1_hp[c][0], xv[c]);
        h1 = biq_step(&r->ap1_hp[c][1], h1);
        a1 += h1;
        a2 = biq_step(&r->ap2_lp[c][0], a1);
        a2 = biq_step(&r->ap2_lp[c][1], a2);
        h1 = biq_step(&r->ap2_hp[c][0], a1);
        h1 = biq_step(&r->ap2_hp[c][1], h1);
        ap[c] = a2 + h1;
    }

    double g0 = ref_band_gain(r, 0, b0[0], b0[1]);
    double g1 = ref_band_gain(r, 1, b1[0], b1[1]);
    double g2 = ref_band_gain(r, 2, b2[0], b2[1]);
    r->band[0][0] = b0[0]; r->band[0][1] = b1[0]; r->band[0][2] = b2[0];
    r->band[1][0] = b0[1]; r->band[1][1] = b1[1]; r->band[1][2] = b2[1];
    for (c = 0; c < 2; c++) {
        wet[c] = g0 * b0[c] + g1 * b1[c] + g2 * b2[c];
    }
    *oL = r->wet * wet[0] + (1.0 - r->wet) * dry[0];
    *oR = r->wet * wet[1] + (1.0 - r->wet) * dry[1];
    *dryL = dry[0]; *dryR = dry[1];
    *apL = ap[0]; *apR = ap[1];
    r->hist_pos++;
}

/* ---------------- signal generators ---------------- */

static uint32_t rng_state = 0x12345678u;
static double white(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (double)(int32_t)rng_state / 2147483648.0;
}

static double sine(int n, int cycles)
{
    return sin(2.0 * M_PI * cycles * n / N);
}

/* single-bin amplitude over the last M samples (integer cycles there) */
static void tone_amp(const double *x, int M, int cycles,
                     double *amp, double *phase)
{
    double re = 0.0, im = 0.0;
    int n;
    for (n = N - M; n < N; n++) {
        double w = 2.0 * M_PI * cycles * n / N;
        re += x[n] * cos(w);
        im -= x[n] * sin(w);
    }
    re = 2.0 * re / M;
    im = 2.0 * im / M;
    *amp = sqrt(re * re + im * im);
    *phase = atan2(im, re);
}

/* ---------------- module helpers ---------------- */

static sp_mbcompress *new_mod(sp_data *sp, double f1, double f2)
{
    sp_mbcompress *m = NULL;
    sp_mbcompress_create(&m);
    if (sp_mbcompress_init(sp, m, (SPFLOAT)f1, (SPFLOAT)f2) != SP_OK) {
        sp_mbcompress_destroy(&m);
        return NULL;
    }
    return m;
}

static void sync_cfg(Ref *r, sp_mbcompress *m)
{
    int b;
    for (b = 0; b < NB; b++) {
        sp_mbcompress_set_threshold(m, b, (SPFLOAT)r->cfg[b].thresh);
        sp_mbcompress_set_ratio(m, b, (SPFLOAT)r->cfg[b].ratio);
        sp_mbcompress_set_attack(m, b, (SPFLOAT)r->cfg[b].atk);
        sp_mbcompress_set_release(m, b, (SPFLOAT)r->cfg[b].rel);
    }
    sp_mbcompress_set_wet(m, (SPFLOAT)r->wet);
}

/* ---------------- named cases ---------------- */

/* 1: unity-gain reconstruction equals the all-pass cascade reference */
static void case_unity_allpass(sp_data *sp)
{
    sp_mbcompress *m = new_mod(sp, F1, F2);
    sp_mbcompress *ma = new_mod(sp, F1, F2); /* in-place instance */
    Ref r;
    double xL[N], xR[N], aL[N], aR[N];
    int n, bad = 0;
    ref_init(&r);
    for (n = 0; n < N; n++) {
        xL[n] = 0.4 * sine(n, 17) + 0.25 * sine(n, 120) + 0.1 * white();
        xR[n] = 0.35 * sine(n, 30) - 0.3 * sine(n, 300) + 0.1 * white();
    }
    for (n = 0; n < N; n++) {
        SPFLOAT il = (SPFLOAT)xL[n], ir = (SPFLOAT)xR[n];
        SPFLOAT ol, orr, al, ar;
        double rl, rr, dl, dr, pl, pr;
        sp_mbcompress_compute(sp, m, &il, &ir, &ol, &orr);
        /* aliased in/out pointers */
        al = il; ar = ir;
        sp_mbcompress_compute(sp, ma, &al, &ar, &al, &ar);
        aL[n] = al; aR[n] = ar;
        ref_step(&r, xL[n], xR[n], &rl, &rr, &dl, &dr, &pl, &pr);
        if (!close_enough(ol, rl) || !close_enough(orr, rr) ||
            !close_enough(ol, dl) || !close_enough(orr, dr) ||
            !close_enough(dl, pl) || !close_enough(dr, pr) ||
            !close_enough(al, rl) || !close_enough(ar, rr))
            bad++;
    }
    check(bad == 0, "unity: module == band sum == A_f1*A_f2, in-place");
    sp_mbcompress_destroy(&m);
    sp_mbcompress_destroy(&ma);
}

/* steady-state analysis window holding an integer number of cycles, so
 * the single-bin measurement is not biased by the startup transient */
static int window_for(int cycles)
{
    if (cycles % 2 == 0) return N / 2;
    return N / 2 + N / (2 * cycles);
}

/* 2: phase alignment at the crossover points, unity magnitude, no comb */
static void case_crossover_points(sp_data *sp)
{
    static const double freqs[] = {60, 120, 260, 700, 1500, F2, 4000,
                                   9000, 15000};
    static const int cycles[] = {6, 12, 26, 70, 150, 250, 400, 900, 1500};
    int k, n, bad = 0;
    for (k = 0; k < 9; k++) {
        sp_mbcompress *m = new_mod(sp, F1, F2);
        Ref r;
        double out[N], dry[N], ap[N], in[N];
        double oa, op, da, dp, aa, ap_ph, ia, ip;
        int M = window_for(cycles[k]);
        ref_init(&r);
        for (n = 0; n < N; n++) {
            SPFLOAT il, ir, ol, orr;
            double rl, rr, dl, dr, pl, pr;
            in[n] = sine(n, cycles[k]);
            il = (SPFLOAT)in[n]; ir = 0.0;
            sp_mbcompress_compute(sp, m, &il, &ir, &ol, &orr);
            out[n] = ol;
            ref_step(&r, in[n], 0.0, &rl, &rr, &dl, &dr, &pl, &pr);
            dry[n] = dl; ap[n] = pl;
        }
        tone_amp(in, M, cycles[k], &ia, &ip);
        tone_amp(out, M, cycles[k], &oa, &op);
        tone_amp(dry, M, cycles[k], &da, &dp);
        tone_amp(ap, M, cycles[k], &aa, &ap_ph);
        /* all-pass: unit magnitude at every probe frequency */
        if (!close_enough(oa, ia) || !close_enough(da, ia) ||
            !close_enough(aa, ia))
            bad++;
        /* output, band-sum dry and A_f1*A_f2 share one phase */
        if (phase_diff(op, dp) > 5e-2 ||
            phase_diff(dp, ap_ph) > 5e-2)
            bad++;
        sp_mbcompress_destroy(&m);
    }

    /* inter-band phase at the two crossover frequencies. Bands are:
       b0 = LP2^2(LP1^2 x), b1 = HP2^2(LP1^2 x) + LP2^2(HP1^2 x),
       b2 = HP2^2(HP1^2 x). */
    {
        Biq lp1[2], hp1[2], lp2lo[2], hp2lo[2], lp2hi[2], hp2hi[2];
        double b0a, b0p, b1a, b1p, b2a, b2p;
        int t;
        (void)freqs;

        for (t = 0; t < 2; t++) {
            biq_lp(&lp1[t], F1); biq_hp(&hp1[t], F1);
            biq_lp(&lp2lo[t], F2); biq_hp(&hp2lo[t], F2);
            biq_lp(&lp2hi[t], F2); biq_hp(&hp2hi[t], F2);
        }
        {
            double pb0[N], pb1[N], pb2[N], x;
            for (n = 0; n < N; n++) {
                double lo, hi, mlo, mhi;
                x = sine(n, 25); /* exactly f1 */
                lo = biq_step(&lp1[0], x); lo = biq_step(&lp1[1], lo);
                hi = biq_step(&hp1[0], x); hi = biq_step(&hp1[1], hi);
                pb0[n] = biq_step(&lp2lo[0], lo);
                pb0[n] = biq_step(&lp2lo[1], pb0[n]);
                mlo = biq_step(&hp2lo[0], lo);
                mlo = biq_step(&hp2lo[1], mlo);
                mhi = biq_step(&lp2hi[0], hi);
                mhi = biq_step(&lp2hi[1], mhi);
                pb1[n] = mlo + mhi;
                pb2[n] = biq_step(&hp2hi[0], hi);
                pb2[n] = biq_step(&hp2hi[1], pb2[n]);
            }
            tone_amp(pb0, N, 25, &b0a, &b0p);
            tone_amp(pb1, N, 25, &b1a, &b1p);
            tone_amp(pb2, N, 25, &b2a, &b2p);
        }
        /* at f1 the low and mid bands carry equal -6 dB branches in
           phase; the high band is silent */
        check(b2a < 1e-4, "f1 crossover: high band silent");
        check(fabs(b0a - b1a) < 1e-3, "f1 crossover: -6 dB branch match");
        check(phase_diff(b0p, b1p) < 5e-2,
              "f1 crossover: low/mid bands in phase");

        for (t = 0; t < 2; t++) {
            biq_lp(&lp1[t], F1); biq_lp(&lp2lo[t], F2);
            biq_hp(&hp2lo[t], F2); biq_lp(&lp2hi[t], F2);
            biq_hp(&hp1[t], F1); biq_hp(&hp2hi[t], F2);
        }
        {
            double pb0[N], pb1[N], pb2[N], x;
            for (n = 0; n < N; n++) {
                double lo, hi, mlo, mhi;
                x = sine(n, 250); /* exactly f2 */
                lo = biq_step(&lp1[0], x); lo = biq_step(&lp1[1], lo);
                hi = biq_step(&hp1[0], x); hi = biq_step(&hp1[1], hi);
                pb0[n] = biq_step(&lp2lo[0], lo);
                pb0[n] = biq_step(&lp2lo[1], pb0[n]);
                mlo = biq_step(&hp2lo[0], lo);
                mlo = biq_step(&hp2lo[1], mlo);
                mhi = biq_step(&lp2hi[0], hi);
                mhi = biq_step(&lp2hi[1], mhi);
                pb1[n] = mlo + mhi;
                pb2[n] = biq_step(&hp2hi[0], hi);
                pb2[n] = biq_step(&hp2hi[1], pb2[n]);
            }
            tone_amp(pb0, N, 250, &b0a, &b0p);
            tone_amp(pb1, N, 250, &b1a, &b1p);
            tone_amp(pb2, N, 250, &b2a, &b2p);
        }
        /* at f2 the low band is silent; the mid/high split carries equal
           -6 dB branches in phase */
        check(b0a < 1e-3, "f2 crossover: low band silent");
        check(fabs(b2a - b1a) < 1e-3, "f2 crossover: -6 dB branch match");
        check(phase_diff(b2p, b1p) < 5e-2,
              "f2 crossover: mid/high bands in phase");
    }
    check(bad == 0, "crossover: unity magnitude and shared phase");
}

/* 3: compressing one band leaves the other tones untouched */
static void case_band_selectivity(sp_data *sp)
{
    /* 100 Hz (low), 1000 Hz (mid), 8000 Hz (high); even cycle counts so
       the 2400-sample analysis window always holds integer periods */
    static const int cyc[NB] = {10, 100, 800};
    static const double amp = 0.4;
    int b, n, bad = 0, j, k;
    double in[N], baseA[NB], baseP[NB];

    /* unity-run reference amplitudes and the band isolation matrix */
    {
        sp_mbcompress *m = new_mod(sp, F1, F2);
        Ref r;
        double bs[NB][N], out[N];
        double iso[NB][NB];
        ref_init(&r);
        for (n = 0; n < N; n++)
            in[n] = amp * (sine(n, cyc[0]) + sine(n, cyc[1]) +
                           sine(n, cyc[2]));
        for (n = 0; n < N; n++) {
            SPFLOAT il = (SPFLOAT)in[n], ir = 0.0, ol, orr;
            double rl, rr, dl, dr, pl, pr;
            sp_mbcompress_compute(sp, m, &il, &ir, &ol, &orr);
            ref_step(&r, in[n], 0.0, &rl, &rr, &dl, &dr, &pl, &pr);
            out[n] = ol;
            for (j = 0; j < NB; j++) bs[j][n] = r.band[0][j];
        }
        for (k = 0; k < NB; k++)
            tone_amp(out, 2400, cyc[k], &baseA[k], &baseP[k]);
        /* every tone must live overwhelmingly in its own band */
        for (j = 0; j < NB; j++)
            for (k = 0; k < NB; k++)
                tone_amp(bs[j], 2400, cyc[k], &iso[j][k], &(double){0});
        for (k = 0; k < NB; k++) {
            for (j = 0; j < NB; j++) {
                if (j == k)
                    check(iso[j][k] > 0.3, "isolation: band holds its tone");
                else
                    check(iso[j][k] < 0.04 * iso[k][k],
                          "isolation: off-band leakage below -28 dB");
            }
        }
        sp_mbcompress_destroy(&m);
    }

    /* two-tone tests: compress one band, the other tone must pass */
    for (b = 0; b < NB; b++) {
        int other = (b + 1) % NB;
        sp_mbcompress *m = new_mod(sp, F1, F2);
        Ref r;
        double comp[N], ca, cp, oa, op, meang = 0.0;
        ref_init(&r);
        for (k = 0; k < NB; k++) {
            r.cfg[k].thresh = -20.0;
            r.cfg[k].ratio = (k == b) ? 8.0 : 1.0;
            r.cfg[k].atk = 0.005;
            r.cfg[k].rel = 0.05;
        }
        sync_cfg(&r, m);
        for (n = 0; n < N; n++) {
            SPFLOAT il, ir, ol, orr;
            double rl, rr, dl, dr, pl, pr;
            in[n] = amp * (sine(n, cyc[b]) + sine(n, cyc[other]));
            il = (SPFLOAT)in[n];
            ir = (SPFLOAT)(0.5 * in[n]);
            sp_mbcompress_compute(sp, m, &il, &ir, &ol, &orr);
            ref_step(&r, in[n], 0.5 * in[n], &rl, &rr, &dl, &dr,
                     &pl, &pr);
            if (!close_enough(ol, rl) || !close_enough(orr, rr))
                bad++;
            comp[n] = ol;
        }
        /* ratio 1 means the other two band gains never leave unity */
        for (j = 0; j < NB; j++) {
            double worst = 0.0;
            if (j == b) continue;
            for (n = N - 2400; n < N; n++)
                if (fabs(r.gainhist[j][n] - 1.0) > worst)
                    worst = fabs(r.gainhist[j][n] - 1.0);
            if (worst > (IS_DOUBLE ? 1e-12 : 1e-6)) bad++;
        }
        for (n = N - 2400; n < N; n++)
            meang += r.gainhist[b][n];
        meang /= 2400.0;
        if (!(meang < 0.95)) bad++; /* the selected band really gains down */

        tone_amp(comp, 2400, cyc[b], &ca, &cp);
        tone_amp(comp, 2400, cyc[other], &oa, &op);
        /* selected tone is pulled down, the other tone keeps its level */
        if (!(ca < 0.9 * baseA[b])) bad++;
        if (fabs(oa / baseA[other] - 1.0) > 0.05) bad++;
        if (phase_diff(op, baseP[other]) > 5e-2)
            bad++;
        sp_mbcompress_destroy(&m);
    }
    check(bad == 0, "band selectivity: only the driven band is reduced");
}

/* 4: stereo gain linking, constant ratios, anti-phase input */
static void case_stereo_link(sp_data *sp)
{
    sp_mbcompress *m = new_mod(sp, F1, F2);
    Ref r;
    int n, bad = 0, compressed = 0, k;
    double x, ming = 1.0;
    ref_init(&r);
    r.cfg[0].thresh = r.cfg[1].thresh = r.cfg[2].thresh = -24.0;
    r.cfg[0].ratio = r.cfg[1].ratio = r.cfg[2].ratio = 4.0;
    sync_cfg(&r, m);

    for (n = 0; n < N; n++) {
        SPFLOAT il, ir, ol, orr;
        double rl, rr, dl, dr, pl, pr;
        x = 0.6 * sine(n, 40) + 0.5 * sine(n, 200) + 0.2 * white();
        /* anti-phase pair: a sum-based detector would see silence */
        il = (SPFLOAT)x;
        ir = (SPFLOAT)(-x);
        sp_mbcompress_compute(sp, m, &il, &ir, &ol, &orr);
        ref_step(&r, x, -x, &rl, &rr, &dl, &dr, &pl, &pr);
        if (!close_enough(ol, rl) || !close_enough(orr, rr)) bad++;
        if (!close_enough(orr, -ol)) bad++;
        for (k = 0; k < NB; k++)
            if (r.last_gain[k] < ming) ming = r.last_gain[k];
    }
    if (ming < 0.9) compressed = 1;
    check(compressed, "anti-phase: max(|L|,|R|) detector still compresses");
    check(bad == 0, "anti-phase: R == -L and matches scalar reference");
    sp_mbcompress_destroy(&m);

    /* constant level ratio between channels must survive compression */
    m = new_mod(sp, F1, F2);
    ref_init(&r);
    r.cfg[0].thresh = r.cfg[1].thresh = r.cfg[2].thresh = -24.0;
    r.cfg[0].ratio = r.cfg[1].ratio = r.cfg[2].ratio = 4.0;
    sync_cfg(&r, m);
    bad = 0;
    for (n = 0; n < N; n++) {
        SPFLOAT il, ir, ol, orr;
        double rl, rr, dl, dr, pl, pr;
        x = 0.5 * sine(n, 33) + 0.4 * sine(n, 177);
        il = (SPFLOAT)x;
        ir = (SPFLOAT)(0.7 * x);
        sp_mbcompress_compute(sp, m, &il, &ir, &ol, &orr);
        ref_step(&r, x, 0.7 * x, &rl, &rr, &dl, &dr, &pl, &pr);
        if (!close_enough(ol, rl) || !close_enough(orr, rr)) bad++;
        if (fabs(ol) > 1e-6 && !close_enough(orr, 0.7 * ol)) bad++;
    }
    check(bad == 0, "stereo: fixed L/R level ratio is preserved");
}

/* 5: dry/wet at 0, 0.5 and 1 against the phase aligned reference */
static void case_dry_wet(sp_data *sp)
{
    static const double wets[3] = {0.0, 0.5, 1.0};
    int w, n, bad = 0;
    double in[N];
    sp_mbcompress *base = new_mod(sp, F1, F2);
    double baseout[N];

    for (n = 0; n < N; n++)
        in[n] = 0.5 * sine(n, 22) + 0.45 * sine(n, 160) +
                0.4 * sine(n, 420);
    for (n = 0; n < N; n++) {
        SPFLOAT il = (SPFLOAT)in[n], ir = (SPFLOAT)(-0.6 * in[n]);
        SPFLOAT ol, orr;
        sp_mbcompress_compute(sp, base, &il, &ir, &ol, &orr);
        baseout[n] = ol;
    }
    sp_mbcompress_destroy(&base);

    for (w = 0; w < 3; w++) {
        sp_mbcompress *m = new_mod(sp, F1, F2);
        Ref r;
        int k;
        ref_init(&r);
        for (k = 0; k < NB; k++) {
            r.cfg[k].thresh = -24.0;
            r.cfg[k].ratio = 4.0;
            r.cfg[k].atk = 0.005;
            r.cfg[k].rel = 0.05;
        }
        r.wet = wets[w];
        sync_cfg(&r, m);
        for (n = 0; n < N; n++) {
            SPFLOAT il = (SPFLOAT)in[n], ir = (SPFLOAT)(-0.6 * in[n]);
            SPFLOAT ol, orr;
            double rl, rr, dl, dr, pl, pr;
            sp_mbcompress_compute(sp, m, &il, &ir, &ol, &orr);
            ref_step(&r, in[n], -0.6 * in[n], &rl, &rr, &dl, &dr,
                     &pl, &pr);
            if (!close_enough(ol, rl) || !close_enough(orr, rr)) bad++;
            if (w == 0 && !close_enough(ol, baseout[n])) bad++;
        }
        sp_mbcompress_destroy(&m);
    }
    check(bad == 0, "dry/wet: 0, 0.5 and 1 match the aligned reference");
}

/* 6: zero attack/release, runtime parameter updates, rejection, reset */
static void case_timing_params_reset(sp_data *sp)
{
    sp_mbcompress *m = new_mod(sp, F1, F2);
    Ref r;
    int n, bad = 0;
    double in[N];
    ref_init(&r);
    sync_cfg(&r, m);

    /* zero attack/release: gain snaps to target every sample */
    for (n = 0; n < NB; n++) {
        r.cfg[n].atk = r.cfg[n].rel = 0.0;
        r.cfg[n].thresh = -20.0;
        r.cfg[n].ratio = 6.0;
    }
    sync_cfg(&r, m);
    for (n = 0; n < N; n++) {
        SPFLOAT il, ir, ol, orr;
        double rl, rr, dl, dr, pl, pr, env;
        env = (n / 1200) % 2 ? 1e-7 : 0.8;
        in[n] = env * (sine(n, 30) + 0.7 * sine(n, 220));
        il = (SPFLOAT)in[n]; ir = (SPFLOAT)(0.9 * in[n]);
        sp_mbcompress_compute(sp, m, &il, &ir, &ol, &orr);
        ref_step(&r, in[n], 0.9 * in[n], &rl, &rr, &dl, &dr, &pl, &pr);
        if (!close_enough(ol, rl) || !close_enough(orr, rr)) bad++;
    }
    check(bad == 0, "zero attack/release: instantaneous target tracking");

    /* legal runtime updates take effect at the next sample, history kept.
     * Reference and module are changed at the same sample so the shared
     * filter and gain history stays comparable. */
    for (n = 0; n < N; n++) {
        SPFLOAT il, ir, ol, orr;
        double rl, rr, dl, dr, pl, pr;
        if (n == 1000) {
            r.cfg[1].ratio = 8.0;
            r.cfg[1].thresh = -30.0;
            r.cfg[1].atk = 0.002;
            r.cfg[1].rel = 0.2;
            r.wet = 0.7;
            sync_cfg(&r, m);
        }
        in[n] = 0.5 * sine(n, 150) + 0.3 * white();
        il = (SPFLOAT)in[n]; ir = (SPFLOAT)(-in[n]);
        sp_mbcompress_compute(sp, m, &il, &ir, &ol, &orr);
        ref_step(&r, in[n], -in[n], &rl, &rr, &dl, &dr, &pl, &pr);
        if (!close_enough(ol, rl) || !close_enough(orr, rr)) bad++;
    }
    check(bad == 0, "runtime parameter updates apply next sample");

    /* illegal values are rejected, existing configuration keeps working */
    check(sp_mbcompress_set_ratio(m, 0, 0.5) == SP_NOT_OK,
          "reject ratio < 1");
    check(sp_mbcompress_set_ratio(m, 3, 2.0) == SP_NOT_OK,
          "reject out of range band index");
    check(sp_mbcompress_set_attack(m, 0, -0.01) == SP_NOT_OK,
          "reject negative attack");
    check(sp_mbcompress_set_release(m, 0, -1.0) == SP_NOT_OK,
          "reject negative release");
    check(sp_mbcompress_set_wet(m, -0.1) == SP_NOT_OK,
          "reject wet < 0");
    check(sp_mbcompress_set_wet(m, 1.1) == SP_NOT_OK,
          "reject wet > 1");
    check(sp_mbcompress_set_threshold(m, 0, INFINITY) == SP_NOT_OK,
          "reject non-finite threshold");
    check(sp_mbcompress_set_ratio(m, 0, NAN) == SP_NOT_OK,
          "reject non-finite ratio");
    check(sp_mbcompress_set_wet(m, NAN) == SP_NOT_OK,
          "reject non-finite wet");
    {
        sp_mbcompress *badm = NULL;
        sp_mbcompress_create(&badm);
        check(sp_mbcompress_init(sp, badm, 0.0, F2) == SP_NOT_OK,
              "reject f1 <= 0");
        check(sp_mbcompress_init(sp, badm, F2, F2) == SP_NOT_OK,
              "reject f1 >= f2");
        check(sp_mbcompress_init(sp, badm, F1, SR * 0.5) == SP_NOT_OK,
              "reject f2 >= sr/2");
        check(sp_mbcompress_init(sp, badm, NAN, F2) == SP_NOT_OK,
              "reject non-finite frequency");
        check(sp_mbcompress_init(sp, badm, F1, F2) == SP_OK,
              "valid frequencies accepted");
        sp_mbcompress_destroy(&badm);
    }
    /* after the rejected calls the module still matches the reference */
    for (n = 0; n < 512; n++) {
        SPFLOAT il, ir, ol, orr;
        double rl, rr, dl, dr, pl, pr;
        in[n] = 0.4 * sine(n, 61) + 0.2 * white();
        il = (SPFLOAT)in[n]; ir = (SPFLOAT)(0.3 * in[n]);
        sp_mbcompress_compute(sp, m, &il, &ir, &ol, &orr);
        ref_step(&r, in[n], 0.3 * in[n], &rl, &rr, &dl, &dr, &pl, &pr);
        if (!close_enough(ol, rl) || !close_enough(orr, rr)) bad++;
    }
    check(bad == 0, "rejected parameters leave the configuration intact");
    sp_mbcompress_destroy(&m);

    /* reset: warmed-up module matches a freshly initialized one */
    {
        sp_mbcompress *fresh = new_mod(sp, F1, F2);
        sp_mbcompress *warm = new_mod(sp, F1, F2);
        Ref rf, rw;
        int j;
        ref_init(&rf);
        ref_init(&rw);
        for (j = 0; j < NB; j++) {
            rw.cfg[j].thresh = -18.0;
            rw.cfg[j].ratio = 5.0;
        }
        sync_cfg(&rw, warm);
        for (n = 0; n < 2000; n++) {
            SPFLOAT il = (SPFLOAT)(0.7 * sine(n, 27)), ir = 0.0, ol, orr;
            sp_mbcompress_compute(sp, warm, &il, &ir, &ol, &orr);
        }
        check(sp_mbcompress_reset(sp, warm) == SP_OK, "reset returns OK");
        /* bring both modules to the same retained configuration */
        rf.cfg[0].thresh = rf.cfg[1].thresh = rf.cfg[2].thresh = -18.0;
        rf.cfg[0].ratio = rf.cfg[1].ratio = rf.cfg[2].ratio = 5.0;
        sync_cfg(&rf, fresh);
        sync_cfg(&rf, warm);
        bad = 0;
        for (n = 0; n < N; n++) {
            SPFLOAT fL, fR, wL, wR, fo1, fo2, wo1, wo2;
            in[n] = 0.5 * sine(n, 44) + 0.3 * sine(n, 260);
            fL = wL = (SPFLOAT)in[n];
            fR = wR = (SPFLOAT)(-0.8 * in[n]);
            sp_mbcompress_compute(sp, fresh, &fL, &fR, &fo1, &fo2);
            sp_mbcompress_compute(sp, warm, &wL, &wR, &wo1, &wo2);
            if (!close_enough(fo1, wo1) || !close_enough(fo2, wo2))
                bad++;
        }
        check(bad == 0, "reset: cleared history and unity gains match fresh");
        sp_mbcompress_destroy(&fresh);
        sp_mbcompress_destroy(&warm);
    }
}

int main(void)
{
    sp_data *sp;
    sp_create(&sp);
    sp->sr = (int)SR;

    printf("mbcompress precision=%s\n", IS_DOUBLE ? "double" : "float");
    case_unity_allpass(sp);
    printf("  case unity_allpass\n");
    case_crossover_points(sp);
    printf("  case crossover_points\n");
    case_band_selectivity(sp);
    printf("  case band_selectivity\n");
    case_stereo_link(sp);
    printf("  case stereo_link\n");
    case_dry_wet(sp);
    printf("  case dry_wet\n");
    case_timing_params_reset(sp);
    printf("  case timing_params_reset\n");

    sp_destroy(&sp);
    if (g_fail || g_checks < 20) {
        printf("MB_COMPRESS_FAIL checks=%d\n", g_checks);
        return 1;
    }
    printf("MB_COMPRESS_CASES_DONE count=6 checks=%d\n", g_checks);
    return 0;
}
