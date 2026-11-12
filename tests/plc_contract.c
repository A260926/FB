/*
 * G.722 fill-in contract tests.
 *
 * Every expectation here is computed by an independent model in this file:
 * real subband values come from a second, unmodified decoder running in ITU
 * test mode (one pair of subband reconstructions per codeword).  Pitch
 * correlation, period expansion, amplitude decay, recovery crossfade and the
 * receive QMF convolution are all reimplemented below; g722_fillin() is never
 * used to produce an expected value.
 *
 * A case returns exactly 1 after all assertions pass.  There are no skips.
 */

#include "checks/plc_contract.h"

/* Include the private state definition before the public headers so that
 * G722_DEC_CTX is the concrete structure type.  This is needed to inspect
 * band predictor state and QMF history, and to enable ITU test mode on the
 * reference decoder. */
#include "g722_private.h"
#include "g722.h"
#include "g722_encoder.h"
#include "g722_decoder.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXSUB 4096
#define MAXPCM 8192

#define PI 3.14159265358979323846

enum
{
    MAT_PERIODIC = 0,
    MAT_DUAL_TONE,
    MAT_SILENCE,
    MAT_NOISE
};

typedef struct
{
    int gap;
    int count;
} seg_t;

/* -------------------------------------------------------------------------
 * Independent reference model
 * ------------------------------------------------------------------------- */

typedef struct
{
    int narrow;
    int hist[320];
    int hpos;
    int hlen;
    int period[120];
    int pitch;
    int high_seed;
    int last_rhigh;
    int k;
    int gap;
    int transition;
    int recover;
    int x[24];
} model_t;

static const int model_qmf[12] =
{
    3, -11, 12, 32, -210, 951, 3876, -805, 362, -156, 53, -11
};

static int model_sat(int v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return v;
}

static int model_rdiv(int num, int den)
{
    if (num >= 0)
        return (num + den/2)/den;
    return -((-num + den/2)/den);
}

static void model_reset(model_t *m, int narrow)
{
    memset(m, 0, sizeof(*m));
    m->narrow = narrow;
}

static void model_push(model_t *m, int v)
{
    m->hist[m->hpos] = v;
    m->hpos = (m->hpos + 1) % 320;
    if (m->hlen < 320)
        m->hlen++;
}

static int model_get(const model_t *m, int idx)
{
    int oldest = (m->hlen == 320)  ?  m->hpos  :  0;
    return m->hist[(oldest + idx) % 320];
}

static void model_pick(model_t *m)
{
    double a[160];
    double sum_a;
    double mean_a;
    double var_a;
    double best_corr;
    int best_delay;
    int i;
    int d;

    m->pitch = 0;
    if (m->hlen < 280)
        return;
    for (i = 0;  i < 160;  i++)
        a[i] = model_get(m, m->hlen - 160 + i);
    sum_a = 0.0;
    for (i = 0;  i < 160;  i++)
        sum_a += a[i];
    mean_a = sum_a/160.0;
    var_a = 0.0;
    for (i = 0;  i < 160;  i++)
    {
        double t = a[i] - mean_a;
        var_a += t*t;
    }
    best_delay = 0;
    best_corr = -2.0;
    for (d = 40;  d <= 120;  d++)
    {
        int base = m->hlen - 160 - d;
        double sum_b = 0.0;
        double mean_b;
        double var_b = 0.0;
        double cross = 0.0;
        double denom;
        double corr;

        for (i = 0;  i < 160;  i++)
            sum_b += model_get(m, base + i);
        mean_b = sum_b/160.0;
        for (i = 0;  i < 160;  i++)
        {
            double bv = model_get(m, base + i) - mean_b;
            double av = a[i] - mean_a;
            var_b += bv*bv;
            cross += av*bv;
        }
        denom = sqrt(var_a*var_b);
        if (denom <= 0.0)
            continue;
        corr = cross/denom;
        if (corr > best_corr)
        {
            best_corr = corr;
            best_delay = d;
        }
    }
    if (best_delay == 0  ||  best_corr < 0.6)
        return;
    m->pitch = best_delay;
    for (i = 0;  i < best_delay;  i++)
        m->period[i] = model_get(m, m->hlen - best_delay + i);
}

static int model_low(const model_t *m, int k)
{
    int v;

    if (m->pitch <= 0)
        return 0;
    if (k >= 960)
        return 0;
    v = m->period[k % m->pitch];
    if (k <= 320)
        return v;
    return model_rdiv(v*(960 - k), 640);
}

static int model_high(const model_t *m, int k)
{
    if (k >= 16)
        return 0;
    return model_rdiv(m->high_seed*(16 - k), 16);
}

static int model_cross(int concealed, int real, int j)
{
    return model_rdiv(concealed*(39 - j) + real*(j + 1), 40);
}

static void model_emit(model_t *m, int rlow, int rhigh, int16_t *out, int *n)
{
    int i;
    int xout1;
    int xout2;

    if (m->narrow)
    {
        out[(*n)++] = (int16_t) (rlow << 1);
        return;
    }
    for (i = 0;  i < 22;  i++)
        m->x[i] = m->x[i + 2];
    m->x[22] = rlow + rhigh;
    m->x[23] = rlow - rhigh;
    xout1 = 0;
    xout2 = 0;
    for (i = 0;  i < 12;  i++)
    {
        xout2 += m->x[2*i]*model_qmf[i];
        xout1 += m->x[2*i + 1]*model_qmf[11 - i];
    }
    out[(*n)++] = (int16_t) model_sat(xout1 >> 11);
    out[(*n)++] = (int16_t) model_sat(xout2 >> 11);
}

static void model_real(model_t *m, int real_low, int real_high, int16_t *out, int *n)
{
    int em_low;
    int em_high;

    if (m->gap)
    {
        m->transition = 1;
        m->recover = 0;
    }
    if (m->transition)
    {
        em_low = model_cross(model_low(m, m->k), real_low, m->recover);
        em_high = model_cross(model_high(m, m->k), real_high, m->recover);
    }
    else
    {
        em_low = real_low;
        em_high = real_high;
    }
    model_emit(m, em_low, em_high, out, n);
    model_push(m, real_low);
    m->last_rhigh = real_high;
    if (m->transition)
    {
        m->k++;
        if (++m->recover >= 40)
            m->transition = 0;
    }
    m->gap = 0;
}

static void model_gap_start(model_t *m)
{
    if (m->gap)
        return;
    model_pick(m);
    m->high_seed = m->last_rhigh;
    m->k = 0;
    m->recover = 0;
    m->transition = 0;
    m->gap = 1;
}

static void model_gap_step(model_t *m, int16_t *out, int *n)
{
    int rlow = model_low(m, m->k);
    int rhigh = m->narrow  ?  0  :  model_high(m, m->k);

    model_emit(m, rlow, rhigh, out, n);
    m->k++;
}

/* Run a schedule of real codeword runs and missing intervals.  When
 * snapshot_gap is non-NULL it receives the model state immediately after the
 * last missing instant of the schedule's first gap interval. */
static void model_run(const int *low, const int *high, int nsub, int narrow,
                      const seg_t *segs, int nseg,
                      int16_t *out, int *nout, model_t *final, int *rend,
                      model_t *snapshot_gap, int *snapshot_n)
{
    model_t m;
    int n;
    int ri;
    int s;
    int c;
    int first_gap = -1;

    (void) nsub;
    model_reset(&m, narrow);
    n = 0;
    ri = 0;
    for (s = 0;  s < nseg;  s++)
    {
        if (segs[s].gap)
        {
            if (first_gap < 0)
                first_gap = s;
            model_gap_start(&m);
            for (c = 0;  c < segs[s].count;  c++)
                model_gap_step(&m, out, &n);
            if (snapshot_gap  &&  s == first_gap)
            {
                *snapshot_gap = m;
                *snapshot_n = n;
            }
        }
        else
        {
            for (c = 0;  c < segs[s].count;  c++)
            {
                model_real(&m, low[ri], high[ri], out, &n);
                ri++;
            }
        }
    }
    *nout = n;
    if (final)
        *final = m;
    if (rend)
        *rend = ri;
}

/* -------------------------------------------------------------------------
 * Material generation and encoding
 * ------------------------------------------------------------------------- */

static int16_t gen_pcm[MAXPCM];
static uint8_t gen_bytes[MAXSUB];
static int ref_low[MAXSUB];
static int ref_high[MAXSUB];
static int16_t expect_pcm[MAXPCM];
static int16_t actual_pcm[MAXPCM];

static void gen_material(int kind, int fs, int16_t *pcm, int len)
{
    unsigned int rng;
    int i;

    rng = 0x12345678u;
    for (i = 0;  i < len;  i++)
    {
        double t = (double) i/(double) fs;
        switch (kind)
        {
        case MAT_PERIODIC:
            pcm[i] = (int16_t) (3000.0*sin(2*PI*100.0*t)
                                + 1400.0*sin(2*PI*200.0*t)
                                + 700.0*sin(2*PI*300.0*t));
            break;
        case MAT_DUAL_TONE:
            pcm[i] = (int16_t) (2500.0*sin(2*PI*700.0*t)
                                + 2500.0*sin(2*PI*1700.0*t));
            break;
        case MAT_SILENCE:
            pcm[i] = 0;
            break;
        default:
            rng = rng*1103515245u + 12345u;
            pcm[i] = (int16_t) (((int) ((rng >> 16) & 0xFFFF) % 12001) - 6000);
            break;
        }
    }
}

/* Encode exactly ncodewords codewords worth of material. */
static int encode_material(int rate, int narrow, int kind, int ncodewords)
{
    G722_ENC_CTX *enc;
    int len;
    int bytes;

    len = narrow  ?  ncodewords  :  2*ncodewords;
    if (len > MAXPCM)
        return -1;
    gen_material(kind, narrow  ?  8000  :  16000, gen_pcm, len);
    enc = g722_encoder_new(rate, narrow  ?  G722_SAMPLE_RATE_8000  :  G722_DEFAULT);
    if (!enc)
        return -1;
    bytes = g722_encode(enc, gen_pcm, len, gen_bytes);
    g722_encoder_destroy(enc);
    return bytes;
}

/* Recover the genuine subband reconstructions with a second decoder in ITU
 * test mode.  The high band of an 8 kHz stream is not meaningful and is
 * ignored by narrowband expectations. */
static int make_reference(int rate, const uint8_t *bytes, int nbytes)
{
    G722_DEC_CTX *ref;
    int16_t buf[MAXPCM];
    int outn;
    int i;

    ref = g722_decoder_new(rate, G722_DEFAULT);
    if (!ref)
        return 0;
    ref->itu_test_mode = 1;
    outn = g722_decode(ref, bytes, nbytes, buf);
    g722_decoder_destroy(ref);
    if (outn != 2*nbytes)
        return 0;
    for (i = 0;  i < nbytes;  i++)
    {
        ref_low[i] = buf[2*i]/2;
        ref_high[i] = buf[2*i + 1]/2;
    }
    return 1;
}

static int run_actual(int rate, int narrow, const uint8_t *bytes, int nbytes,
                      const seg_t *segs, int nseg,
                      int16_t *out, int *fill_returns, int *nfill)
{
    G722_DEC_CTX *dec;
    int n;
    int off;
    int s;

    (void) nbytes;
    dec = g722_decoder_new(rate, narrow  ?  G722_SAMPLE_RATE_8000  :  G722_DEFAULT);
    if (!dec)
        return -1;
    n = 0;
    off = 0;
    if (nfill)
        *nfill = 0;
    for (s = 0;  s < nseg;  s++)
    {
        if (segs[s].gap)
        {
            int r = g722_fillin(dec, out + n, segs[s].count);

            if (fill_returns  &&  nfill)
                fill_returns[(*nfill)++] = r;
            if (r < 0)
            {
                g722_decoder_destroy(dec);
                return -1;
            }
            n += r;
        }
        else
        {
            int r = g722_decode(dec, bytes + off, segs[s].count, out + n);
            if (r != segs[s].count*(narrow  ?  1  :  2))
            {
                g722_decoder_destroy(dec);
                return -1;
            }
            off += segs[s].count;
            n += r;
        }
    }
    g722_decoder_destroy(dec);
    return n;
}

static int pcm_close(const int16_t *a, const int16_t *b, int n)
{
    int i;

    for (i = 0;  i < n;  i++)
    {
        int d = (int) a[i] - (int) b[i];
        if (d < 0)
            d = -d;
        if (d > 1)
        {
            fprintf(stderr, "PCM mismatch at %d: actual=%d expected=%d\n",
                    i, a[i], b[i]);
            return 0;
        }
    }
    return 1;
}

/* Compare one complete scripted decode against the independent model. */
static int check_schedule(int rate, int narrow, int kind, int ncodewords,
                          const seg_t *segs, int nseg, int *fill_returns)
{
    int nbytes;
    int na;
    int ne;
    int nfill;
    int rend;

    nbytes = encode_material(rate, narrow, kind, ncodewords);
    if (nbytes <= 0  ||  nbytes > MAXSUB)
        return 0;
    if (!make_reference(rate, gen_bytes, nbytes))
        return 0;
    na = run_actual(rate, narrow, gen_bytes, nbytes, segs, nseg,
                    actual_pcm, fill_returns, &nfill);
    if (na < 0)
        return 0;
    model_run(ref_low, ref_high, nbytes, narrow, segs, nseg,
              expect_pcm, &ne, NULL, &rend, NULL, NULL);
    if (rend != nbytes  ||  ne != na)
        return 0;
    return pcm_close(actual_pcm, expect_pcm, na);
}

/* Compute the best normalised correlation over the 40..120 delay range for
 * the final window of the supplied real low-band history. */
static int best_correlation(const int *low, int nsub, double *best_out)
{
    double a[160];
    double sum_a;
    double mean_a;
    double var_a;
    double best;
    int i;
    int d;

    if (nsub < 280)
        return 0;
    for (i = 0;  i < 160;  i++)
        a[i] = low[nsub - 160 + i];
    sum_a = 0.0;
    for (i = 0;  i < 160;  i++)
        sum_a += a[i];
    mean_a = sum_a/160.0;
    var_a = 0.0;
    for (i = 0;  i < 160;  i++)
    {
        double t = a[i] - mean_a;
        var_a += t*t;
    }
    best = -2.0;
    for (d = 40;  d <= 120;  d++)
    {
        double sum_b = 0.0;
        double mean_b;
        double var_b = 0.0;
        double cross = 0.0;
        double denom;
        double corr;
        int base = nsub - 160 - d;

        for (i = 0;  i < 160;  i++)
            sum_b += low[base + i];
        mean_b = sum_b/160.0;
        for (i = 0;  i < 160;  i++)
        {
            double bv = low[base + i] - mean_b;
            double av = a[i] - mean_a;
            var_b += bv*bv;
            cross += av*bv;
        }
        denom = sqrt(var_a*var_b);
        if (denom <= 0.0)
            continue;
        corr = cross/denom;
        if (corr > best)
            best = corr;
    }
    *best_out = best;
    return 1;
}

/* -------------------------------------------------------------------------
 * Cases
 * ------------------------------------------------------------------------- */

static int case_zero_length_call(void)
{
    static const seg_t pre[] = { { 0, 400 } };
    int nbytes;
    G722_DEC_CTX *dec;
    struct g722_decode_state snap;
    int16_t guard[4];
    int i;
    int r;

    nbytes = encode_material(64000, 0, MAT_PERIODIC, 900);
    if (nbytes <= 0)
        return 0;
    dec = g722_decoder_new(64000, G722_DEFAULT);
    if (!dec)
        return 0;
    if (g722_decode(dec, gen_bytes, pre[0].count, actual_pcm)
        != pre[0].count*2)
        return 0;

    snap = *dec;
    for (i = 0;  i < 4;  i++)
        guard[i] = (int16_t) (12345 + i);
    r = g722_fillin(dec, guard, 0);
    if (r != 0)
        return 0;
    if (memcmp(dec, &snap, sizeof(snap)) != 0)
        return 0;
    for (i = 0;  i < 4;  i++)
    {
        if (guard[i] != (int16_t) (12345 + i))
            return 0;
    }

    /* An empty decode call must also leave the recovery progress untouched. */
    if (g722_decode(dec, NULL, 0, guard) != 0)
        return 0;
    if (memcmp(dec, &snap, sizeof(snap)) != 0)
        return 0;
    g722_decoder_destroy(dec);

    /* Zero fill on a brand new instance: nothing to conceal. */
    dec = g722_decoder_new(48000, G722_SAMPLE_RATE_8000);
    if (!dec)
        return 0;
    snap = *dec;
    r = g722_fillin(dec, guard, 0);
    g722_decoder_destroy(dec);
    return r == 0;
}

static int case_invalid_arguments(void)
{
    G722_DEC_CTX *dec;
    struct g722_decode_state snap;
    int16_t guard[8];
    int i;
    int r;

    /* Use a context with real history so that a state change would show. */
    if (encode_material(64000, 0, MAT_PERIODIC, 600) <= 0)
        return 0;
    dec = g722_decoder_new(64000, G722_DEFAULT);
    if (!dec)
        return 0;
    if (g722_decode(dec, gen_bytes, 400, actual_pcm) != 800)
        return 0;

    for (i = 0;  i < 8;  i++)
        guard[i] = (int16_t) (777 + i);
    snap = *dec;
    r = g722_fillin(dec, guard, -1);
    if (r != -1  ||  memcmp(dec, &snap, sizeof(snap)) != 0)
        return 0;
    r = g722_fillin(dec, guard, 8001);
    if (r != -1  ||  memcmp(dec, &snap, sizeof(snap)) != 0)
        return 0;
    for (i = 0;  i < 8;  i++)
    {
        if (guard[i] != (int16_t) (777 + i))
            return 0;
    }
    g722_decoder_destroy(dec);

    /* Packed 48 kbps and 56 kbps contexts are unsupported. */
    dec = g722_decoder_new(48000, G722_PACKED);
    if (!dec  ||  !dec->packed)
        return 0;
    snap = *dec;
    r = g722_fillin(dec, guard, 10);
    i = (r == -1  &&  memcmp(dec, &snap, sizeof(snap)) == 0);
    g722_decoder_destroy(dec);
    if (!i)
        return 0;

    dec = g722_decoder_new(56000, G722_PACKED);
    if (!dec  ||  !dec->packed)
        return 0;
    snap = *dec;
    r = g722_fillin(dec, guard, 10);
    i = (r == -1  &&  memcmp(dec, &snap, sizeof(snap)) == 0);
    g722_decoder_destroy(dec);
    if (!i)
        return 0;

    /* At 64 kbps the PACKED option is ignored, so the context is supported. */
    dec = g722_decoder_new(64000, G722_PACKED);
    if (!dec  ||  dec->packed)
        return 0;
    r = g722_fillin(dec, guard, 4);
    g722_decoder_destroy(dec);
    return r == 8;
}

static int case_six_rate_layouts(void)
{
    seg_t segs[3];
    int rates[3] = { 48000, 56000, 64000 };
    int r;
    int narrow;
    int ncodewords;
    int fills[1];

    ncodewords = 1400;
    segs[0].gap = 0;
    segs[0].count = 400;
    segs[1].gap = 1;
    segs[1].count = 20;
    segs[2].gap = 0;
    segs[2].count = ncodewords - 400;

    for (r = 0;  r < 3;  r++)
    {
        for (narrow = 0;  narrow < 2;  narrow++)
        {
            if (!check_schedule(rates[r], narrow, MAT_PERIODIC, ncodewords,
                                segs, 3, fills))
                return 0;
            if (fills[0] != 20*(narrow  ?  1  :  2))
                return 0;
        }
    }
    return 1;
}

static int case_periodic_short_loss(void)
{
    seg_t segs[3];
    int ncodewords = 2400;
    int fills[1];

    segs[0].gap = 0;
    segs[0].count = 420;
    segs[1].gap = 1;
    segs[1].count = 13;
    segs[2].gap = 0;
    segs[2].count = ncodewords - 420;
    if (!check_schedule(64000, 0, MAT_PERIODIC, ncodewords, segs, 3, fills))
        return 0;
    return fills[0] == 26;
}

static int case_loss_during_recovery(void)
{
    seg_t segs[5];
    int ncodewords = 2000;

    segs[0].gap = 0;
    segs[0].count = 400;
    segs[1].gap = 1;
    segs[1].count = 100;
    segs[2].gap = 0;
    segs[2].count = 17;
    segs[3].gap = 1;
    segs[3].count = 90;
    segs[4].gap = 0;
    segs[4].count = ncodewords - 417;
    return check_schedule(64000, 0, MAT_PERIODIC, ncodewords, segs, 5, NULL);
}

static int case_long_120ms_gap(void)
{
    seg_t segs[3];
    int ncodewords = 2600;
    int fills[1];
    int nbytes;
    int na;
    int ne;
    int rend;
    int16_t expected[MAXPCM];
    model_t final;
    model_t gap_end;
    int gap_end_n;
    int i;

    segs[0].gap = 0;
    segs[0].count = 400;
    segs[1].gap = 1;
    segs[1].count = 1050;   /* 120 ms of subband instants is 960. */
    segs[2].gap = 0;
    segs[2].count = ncodewords - 400;
    if (!check_schedule(64000, 0, MAT_PERIODIC, ncodewords, segs, 3, fills))
        return 0;
    if (fills[0] != 2100)
        return 0;

    /* Re-run the model and snapshot the state at the end of the gap.  A real
     * pitch must have been selected, yet every extension from instant 960 on
     * must be zero and the produced PCM must have decayed to the QMF tail. */
    nbytes = encode_material(64000, 0, MAT_PERIODIC, ncodewords);
    if (nbytes <= 0  ||  !make_reference(64000, gen_bytes, nbytes))
        return 0;
    model_run(ref_low, ref_high, nbytes, 0, segs, 3, expected, &ne, &final,
              &rend, &gap_end, &gap_end_n);
    if (rend != ncodewords  ||  ne != 2100 + 800 + 2*(ncodewords-400))
        return 0;
    if (gap_end_n != 800 + 2100  ||  gap_end.k != 1050)
        return 0;
    if (gap_end.pitch < 40  ||  gap_end.pitch > 120)
        return 0;
    for (na = 960;  na < 1050;  na++)
    {
        if (model_low(&gap_end, na) != 0  ||  model_high(&gap_end, na) != 0)
            return 0;
    }
    /* The QMF inputs are exactly zero from instant 960 on (the extension is
     * silent and no real data has resumed).  A 12-tap receive QMF can only
     * carry the last six non-zero instants (954..959); from instant 967 on
     * the output must be indistinguishable from silence. */
    for (na = 967;  na < 1050;  na++)
    {
        for (i = 0;  i < 2;  i++)
        {
            int d = (int) expected[(800 + 2*na) + i];

            if (d < 0)
                d = -d;
            if (d > 2)
                return 0;
        }
    }
    return 1;
}

static int case_silence_gap(void)
{
    seg_t segs[3];
    int ncodewords = 1400;
    int nbytes;
    int na;
    int nfill;
    int fills[1];
    int i;

    nbytes = encode_material(64000, 0, MAT_SILENCE, ncodewords);
    if (nbytes <= 0  ||  !make_reference(64000, gen_bytes, nbytes))
        return 0;
    segs[0].gap = 0;
    segs[0].count = 400;
    segs[1].gap = 1;
    segs[1].count = 113;
    segs[2].gap = 0;
    segs[2].count = ncodewords - 400;
    na = run_actual(64000, 0, gen_bytes, nbytes, segs, 3, actual_pcm,
                    fills, &nfill);
    if (na < 0  ||  nfill != 1  ||  fills[0] != 226)
        return 0;
    for (i = 800;  i < 800 + 226;  i++)
    {
        if (actual_pcm[i] < -2  ||  actual_pcm[i] > 2)
            return 0;
    }
    return check_schedule(64000, 0, MAT_SILENCE, ncodewords, segs, 3, NULL);
}

static int case_nonperiodic_zero_fill(void)
{
    seg_t segs[3];
    int ncodewords = 1600;
    double corr;

    segs[0].gap = 0;
    segs[0].count = 400;
    segs[1].gap = 1;
    segs[1].count = 113;
    segs[2].gap = 0;
    segs[2].count = ncodewords - 400;
    if (!check_schedule(48000, 0, MAT_NOISE, ncodewords, segs, 3, NULL))
        return 0;
    if (encode_material(48000, 0, MAT_NOISE, ncodewords) <= 0)
        return 0;
    if (!make_reference(48000, gen_bytes, ncodewords))
        return 0;
    if (!best_correlation(ref_low, 400, &corr))
        return 0;
    return corr < 0.6;
}

static int case_dual_tone_short_gap(void)
{
    seg_t segs[3];
    int ncodewords = 1600;

    segs[0].gap = 0;
    segs[0].count = 360;
    segs[1].gap = 1;
    segs[1].count = 9;
    segs[2].gap = 0;
    segs[2].count = ncodewords - 360;
    return check_schedule(64000, 0, MAT_DUAL_TONE, ncodewords, segs, 3, NULL);
}

static int case_dual_instances(void)
{
    static const seg_t a_segs[5] =
    {
        { 0, 400 }, { 1, 40 }, { 0, 300 }, { 1, 10 }, { 0, 850 }
    };
    static const seg_t b_segs[5] =
    {
        { 0, 400 }, { 1, 25 }, { 0, 300 }, { 1, 10 }, { 0, 1650 }
    };
    G722_DEC_CTX *da;
    G722_DEC_CTX *db;
    int16_t outa[MAXPCM];
    int16_t outb[MAXPCM];
    int16_t exa[MAXPCM];
    int16_t exb[MAXPCM];
    static int low_b[MAXSUB];
    static int high_b[MAXSUB];
    static uint8_t bytes_a[MAXSUB];
    static uint8_t bytes_b[MAXSUB];
    int na;
    int nb;
    int nba;
    int nbb;
    int pa;
    int pb;
    int ia;
    int ib;
    int s;

    if (encode_material(64000, 0, MAT_PERIODIC, 1600) <= 0)
        return 0;
    nba = 1600;
    if (!make_reference(64000, gen_bytes, nba))
        return 0;
    memcpy(bytes_a, gen_bytes, nba);
    {
        static int low_a[MAXSUB];
        static int high_a[MAXSUB];
        memcpy(low_a, ref_low, nba*sizeof(int));
        memcpy(high_a, ref_high, nba*sizeof(int));

        if (encode_material(48000, 1, MAT_PERIODIC, 2400) <= 0)
            return 0;
        nbb = 2400;
        if (!make_reference(48000, gen_bytes, nbb))
            return 0;
        memcpy(bytes_b, gen_bytes, nbb);
        memcpy(low_b, ref_low, nbb*sizeof(int));
        memcpy(high_b, ref_high, nbb*sizeof(int));

        da = g722_decoder_new(64000, G722_DEFAULT);
        db = g722_decoder_new(48000, G722_SAMPLE_RATE_8000);
        if (!da  ||  !db)
            return 0;
        na = 0;
        nb = 0;
        pa = 0;
        pb = 0;
        /* Interleave the two instances step by step, preserving each
         * instance's own schedule order. */
        for (s = 0;  s < 5;  s++)
        {
            if (a_segs[s].gap)
            {
                int r = g722_fillin(da, outa + na, a_segs[s].count);

                if (r != a_segs[s].count*2)
                    return 0;
                na += r;
            }
            else
            {
                int r = g722_decode(da, bytes_a + pa, a_segs[s].count, outa + na);

                if (r != a_segs[s].count*2)
                    return 0;
                pa += a_segs[s].count;
                na += r;
            }
            if (b_segs[s].gap)
            {
                int r = g722_fillin(db, outb + nb, b_segs[s].count);

                if (r != b_segs[s].count)
                    return 0;
                nb += r;
            }
            else
            {
                int r = g722_decode(db, bytes_b + pb, b_segs[s].count, outb + nb);

                if (r != b_segs[s].count)
                    return 0;
                pb += b_segs[s].count;
                nb += r;
            }
        }
        g722_decoder_destroy(da);
        g722_decoder_destroy(db);
        model_run(low_a, high_a, nba, 0, a_segs, 5, exa, &ia, NULL, NULL,
                  NULL, NULL);
        if (ia != na  ||  !pcm_close(outa, exa, na))
            return 0;
    }
    model_run(low_b, high_b, nbb, 1, b_segs, 5, exb, &ib, NULL, NULL,
              NULL, NULL);
    return ib == nb  &&  pcm_close(outb, exb, nb);
}

static int case_chunked_equals_single(void)
{
    static const seg_t chunks[13] =
    {
        { 0, 400 }, { 1, 1 }, { 1, 7 }, { 1, 7 }, { 1, 7 }, { 1, 7 },
        { 1, 7 }, { 1, 7 }, { 1, 7 }, { 1, 7 }, { 1, 7 }, { 1, 7 }, { 1, 42 }
    };
    int ncodewords = 1000;
    int narrow;
    int rate;
    int b0;
    int b1;
    G722_DEC_CTX *da;
    G722_DEC_CTX *db;
    int16_t fa[MAXPCM];
    int16_t fb[MAXPCM];
    int16_t ra[MAXPCM];
    int16_t rb[MAXPCM];
    struct g722_decode_state snap;

    for (rate = 48000;  rate <= 64000;  rate += 16000)
    {
        for (narrow = 0;  narrow < 2;  narrow++)
        {
            int bytes = encode_material(rate, narrow, MAT_PERIODIC, ncodewords);

            if (bytes != ncodewords)
                return 0;
            if (!make_reference(rate, gen_bytes, bytes))
                return 0;
            da = g722_decoder_new(rate, narrow  ?  G722_SAMPLE_RATE_8000  :  G722_DEFAULT);
            db = g722_decoder_new(rate, narrow  ?  G722_SAMPLE_RATE_8000  :  G722_DEFAULT);
            if (!da  ||  !db)
                return 0;
            if (g722_decode(da, gen_bytes, 400, ra) != 400*(narrow ? 1 : 2))
                return 0;
            if (g722_decode(db, gen_bytes, 400, rb) != 400*(narrow ? 1 : 2))
                return 0;
            b0 = g722_fillin(da, fa, 113);
            b1 = 0;
            {
                int i;

                for (i = 1;  i < 13;  i++)
                {
                    int r = g722_fillin(db, fb + b1, chunks[i].count);

                    if (r < 0)
                        return 0;
                    b1 += r;
                }
            }
            if (b0 != 113*(narrow ? 1 : 2)  ||  b1 != b0)
                return 0;
            if (memcmp(fa, fb, (size_t) b0*sizeof(int16_t)) != 0)
                return 0;
            snap = *db;
            if (g722_decode(db, NULL, 0, fb) != 0)
                return 0;
            if (memcmp(db, &snap, sizeof(snap)) != 0)
                return 0;
            /* Recovery must land on exactly the same state as the single call. */
            if (memcmp(fa, fb, (size_t) b0*sizeof(int16_t)) != 0)
                return 0;
            {
                int rest = ncodewords - 400;
                int oa = g722_decode(da, gen_bytes + 400, rest, fa + b0);
                int ob = g722_decode(db, gen_bytes + 400, rest, fb + b0);

                if (oa != ob  ||  oa != rest*(narrow ? 1 : 2))
                    return 0;
                if (memcmp(fa, fb, (size_t) (b0 + oa)*sizeof(int16_t)) != 0)
                    return 0;
            }
            g722_decoder_destroy(da);
            g722_decoder_destroy(db);
        }
    }
    return 1;
}

static int case_predictor_and_qmf_state(void)
{
    seg_t segs[3];
    int ncodewords = 1800;
    int nbytes;
    int na;
    int ne;
    int i;
    G722_DEC_CTX *dec;
    G722_DEC_CTX *ref;
    struct g722_decode_state snap;
    struct g722_band bands_before[2];
    model_t m;
    int16_t model_out[MAXPCM];
    int mn;
    int16_t ref_out[MAXPCM];

    nbytes = encode_material(64000, 0, MAT_PERIODIC, ncodewords);
    if (nbytes <= 0  ||  !make_reference(64000, gen_bytes, nbytes))
        return 0;
    dec = g722_decoder_new(64000, G722_DEFAULT);
    if (!dec)
        return 0;

    segs[0].gap = 0;
    segs[0].count = 400;
    segs[1].gap = 1;
    segs[1].count = 200;
    segs[2].gap = 0;
    segs[2].count = ncodewords - 400;

    if (g722_decode(dec, gen_bytes, 400, actual_pcm) != 800)
        return 0;

    /* Snapshot: the predictor and quantiser adaptation state must survive the
     * whole concealed interval without a single update. */
    snap = *dec;
    memcpy(bands_before, dec->band, sizeof(bands_before));

    if (g722_fillin(dec, actual_pcm + 800, 200) != 400)
        return 0;
    if (memcmp(dec->band, bands_before, sizeof(bands_before)) != 0)
        return 0;
    if (dec->in_buffer != snap.in_buffer  ||  dec->in_bits != snap.in_bits)
        return 0;

    /* The receive QMF must have advanced through every concealed instant. */
    model_run(ref_low, ref_high, nbytes, 0, segs, 2, model_out, &mn, &m, NULL,
              NULL, NULL);
    if (mn != 800 + 400)
        return 0;
    if (memcmp(dec->x, m.x, sizeof(m.x)) != 0)
        return 0;

    /* Finish the decode and check that the real predictor state is exactly
     * the one an unbroken decode of all codewords reaches. */
    na = 800 + 400 + g722_decode(dec, gen_bytes + 400,
                                 ncodewords - 400, actual_pcm + 1200);
    ref = g722_decoder_new(64000, G722_DEFAULT);
    if (!ref)
        return 0;
    if (g722_decode(ref, gen_bytes, nbytes, ref_out) != 2*ncodewords)
        return 0;
    if (memcmp(dec->band, ref->band, sizeof(ref->band)) != 0)
        return 0;
    g722_decoder_destroy(ref);
    g722_decoder_destroy(dec);

    model_run(ref_low, ref_high, nbytes, 0, segs, 3, expect_pcm, &ne, &m, NULL,
              NULL, NULL);
    if (ne != na)
        return 0;
    for (i = 0;  i < ne;  i++)
    {
        int d = (int) actual_pcm[i] - (int) expect_pcm[i];

        if (d < 0)
            d = -d;
        if (d > 1)
            return 0;
    }
    return 1;
}

static int case_insufficient_history(void)
{
    seg_t segs[3];
    int ncodewords = 1400;
    int short_run;
    int16_t out[MAXPCM];
    int n;
    int fills[2];
    int nfill;
    int i;
    G722_DEC_CTX *dec;

    /* Right after construction there is no real history at all: the whole
     * concealed run must be silent. */
    segs[0].gap = 1;
    segs[0].count = 100;
    segs[1].gap = 0;
    segs[1].count = ncodewords;
    if (!check_schedule(64000, 0, MAT_PERIODIC, ncodewords, segs, 2, NULL))
        return 0;
    n = encode_material(64000, 0, MAT_PERIODIC, ncodewords);
    if (n <= 0  ||  !make_reference(64000, gen_bytes, n))
        return 0;
    n = run_actual(64000, 0, gen_bytes, n, segs, 2, out, fills, &nfill);
    if (n < 0  ||  nfill != 1  ||  fills[0] != 200)
        return 0;
    for (i = 0;  i < 200;  i++)
    {
        if (out[i] != 0)
            return 0;
    }
    /* The QMF history must be all zero too, since the extension was silent. */
    dec = g722_decoder_new(64000, G722_DEFAULT);
    if (!dec)
        return 0;
    if (g722_fillin(dec, out, 100) != 200)
        return 0;
    for (i = 0;  i < 24;  i++)
    {
        if (dec->x[i] != 0)
        {
            g722_decoder_destroy(dec);
            return 0;
        }
    }
    g722_decoder_destroy(dec);

    /* 279 real values is still below the 280 threshold: zero fill. */
    short_run = 279;
    segs[0].gap = 0;
    segs[0].count = short_run;
    segs[1].gap = 1;
    segs[1].count = 60;
    segs[2].gap = 0;
    segs[2].count = ncodewords - short_run;
    if (!check_schedule(64000, 0, MAT_PERIODIC, ncodewords, segs, 3, NULL))
        return 0;
    n = encode_material(64000, 0, MAT_PERIODIC, ncodewords);
    if (n <= 0  ||  !make_reference(64000, gen_bytes, n))
        return 0;
    dec = g722_decoder_new(64000, G722_DEFAULT);
    if (!dec)
        return 0;
    if (g722_decode(dec, gen_bytes, short_run, out) != 2*short_run)
        return 0;
    if (g722_fillin(dec, out, 60) != 120)
        return 0;
    /* No pitch was selected: the low band is zero fill.  The high band still
     * decays from the last real value for 16 instants, so only inspect the
     * output after the high-band decay and the QMF tail have passed. */
    if (dec->plc_pitch != 0  ||  dec->plc_hist_len != 279)
    {
        g722_decoder_destroy(dec);
        return 0;
    }
    for (i = 2*(16 + 12);  i < 120;  i++)
    {
        int d = out[i];

        if (d < 0)
            d = -d;
        if (d > 2)
        {
            g722_decoder_destroy(dec);
            return 0;
        }
    }
    g722_decoder_destroy(dec);

    /* 280 real values is enough for a real period. */
    short_run = 280;
    segs[0].count = short_run;
    segs[1].count = 60;
    segs[2].count = ncodewords - short_run;
    if (!check_schedule(64000, 0, MAT_PERIODIC, ncodewords, segs, 3, NULL))
        return 0;
    dec = g722_decoder_new(64000, G722_DEFAULT);
    if (!dec)
        return 0;
    if (g722_decode(dec, gen_bytes, short_run, out) != 2*short_run)
        return 0;
    if (g722_fillin(dec, out, 60) != 120)
        return 0;
    if (dec->plc_pitch < 40  ||  dec->plc_pitch > 120)
    {
        g722_decoder_destroy(dec);
        return 0;
    }
    g722_decoder_destroy(dec);
    return 1;
}

const plc_test_case plc_cases[] =
{
    { "zero_length_call_is_noop",            case_zero_length_call },
    { "invalid_arguments_preserve_state",    case_invalid_arguments },
    { "six_rate_layout_configs",             case_six_rate_layouts },
    { "insufficient_history_zero_fills",     case_insufficient_history },
    { "periodic_short_loss_matches_model",   case_periodic_short_loss },
    { "loss_during_recovery_matches_model",  case_loss_during_recovery },
    { "long_120ms_gap_decay",                case_long_120ms_gap },
    { "silence_gap_is_silence",              case_silence_gap },
    { "nonperiodic_gap_zero_fills",          case_nonperiodic_zero_fill },
    { "dual_tone_short_gap_16k",             case_dual_tone_short_gap },
    { "dual_instances_interleaved",          case_dual_instances },
    { "chunked_fillin_matches_single_call",  case_chunked_equals_single },
    { "predictor_held_and_qmf_advanced",     case_predictor_and_qmf_state }
};

const size_t plc_case_count = sizeof(plc_cases)/sizeof(plc_cases[0]);
