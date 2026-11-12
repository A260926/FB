/*
 * SpanDSP - a series of DSP components for telephony
 *
 * g722_decode.c - The ITU G.722 codec, decode part.
 *
 * Written by Steve Underwood <steveu@coppice.org>
 *
 * Copyright (C) 2005 Steve Underwood
 *
 *  Despite my general liking of the GPL, I place my own contributions 
 *  to this code in the public domain for the benefit of all mankind -
 *  even the slimy ones who might try to proprietize my work and use it
 *  to my detriment.
 *
 * Based in part on a single channel G.722 codec which is:
 *
 * Copyright (c) CMU 1993
 * Computer Science, Speech Group
 * Chengxiang Lu and Alex Hauptmann
 * 
 * The Carnegie Mellon ADPCM program is Copyright (c) 1993 by Carnegie Mellon
 * University. Use of this program, for any research or commercial purpose, is
 * completely unrestricted. If you make use of or redistribute this material,
 * we would appreciate acknowlegement of its origin.
 */

/*! \file */

#include <stdio.h>
#include <inttypes.h>
#include <memory.h>
#include <stdlib.h>
#include <math.h>

#include "g722_private.h"
#include "g722_common.h"
#include "g722.h"
#include "g722_decoder.h"

/* -------------------------------------------------------------------------
 * Packet loss concealment ("fill-in") helpers.
 *
 * These operate purely on the real, limited low-band reconstruction history
 * (the values produced by Block 6L LIMIT) and on the most recent real
 * high-band reconstruction.  The ADPCM predictor and quantiser adaptation
 * state is never touched here.
 * ------------------------------------------------------------------------- */

/*! Generate one 16 kHz PCM pair (or one 8 kHz sample) from a pair of subband
    reconstructions, advancing the receive QMF history exactly like the
    regular decoder. */
static void g722_emit_subbands(G722_DEC_CTX *s, int rlow, int rhigh, int16_t amp[], int *outlen)
{
    static const int qmf_coeffs[12] =
    {
           3,  -11,   12,   32, -210,  951, 3876, -805,  362, -156,   53,  -11,
    };
    int i;
    int xout1;
    int xout2;

    if (s->itu_test_mode)
    {
        amp[(*outlen)++] = (int16_t) (rlow << 1);
        amp[(*outlen)++] = (int16_t) (rhigh << 1);
    }
    else if (s->eight_k)
    {
        amp[(*outlen)++] = (int16_t) (rlow << 1);
    }
    else
    {
        /* Apply the receive QMF */
        for (i = 0;  i < 22;  i++)
            s->x[i] = s->x[i + 2];
        s->x[22] = rlow + rhigh;
        s->x[23] = rlow - rhigh;

        xout1 = 0;
        xout2 = 0;
        for (i = 0;  i < 12;  i++)
        {
            xout2 += s->x[2*i]*qmf_coeffs[i];
            xout1 += s->x[2*i + 1]*qmf_coeffs[11 - i];
        }
        amp[(*outlen)++] = saturate(xout1 >> 11);
        amp[(*outlen)++] = saturate(xout2 >> 11);
    }
}
/*- End of function --------------------------------------------------------*/

/*! Round num/den to the nearest integer, with a halfway value going away
    from zero.  den must be positive. */
static int g722_round_div(int num, int den)
{
    if (num >= 0)
        return (num + den/2)/den;
    return -((-num + den/2)/den);
}
/*- End of function --------------------------------------------------------*/

/*! Append the latest real limited low-band reconstruction to the 320-entry
    history ring. */
static void g722_plc_push_low(G722_DEC_CTX *s, int value)
{
    s->plc_hist[s->plc_hist_pos] = value;
    s->plc_hist_pos = (s->plc_hist_pos + 1) % 320;
    if (s->plc_hist_len < 320)
        s->plc_hist_len++;
}
/*- End of function --------------------------------------------------------*/

/*! Read history by position from oldest (0) to newest (plc_hist_len-1). */
static int g722_plc_hist_get(const G722_DEC_CTX *s, int idx)
{
    int oldest;

    oldest = (s->plc_hist_len == 320)  ?  s->plc_hist_pos  :  0;
    return s->plc_hist[(oldest + idx) % 320];
}
/*- End of function --------------------------------------------------------*/

/*! Pick the pitch period for a new gap.  Candidates are integer delays
    40..120 subband samples.  The most recent 160 samples are correlated with
    the 160 samples one delay earlier, after removing the mean, using the
    normalised correlation.  Candidates with a zero denominator are invalid;
    the largest correlation wins, and equal correlations prefer the smaller
    delay.  Fewer than 280 real samples, or a best correlation below 0.6,
    means zero fill (plc_pitch == 0). */
static void g722_plc_pick_period(G722_DEC_CTX *s)
{
    int a[160];
    int i;
    int d;
    int best_delay;
    double best_corr;
    double sum_a;
    double mean_a;
    double var_a;

    s->plc_pitch = 0;
    if (s->plc_hist_len < 280)
        return;

    for (i = 0;  i < 160;  i++)
        a[i] = g722_plc_hist_get(s, s->plc_hist_len - 160 + i);
    sum_a = 0.0;
    for (i = 0;  i < 160;  i++)
        sum_a += a[i];
    mean_a = sum_a/160.0;
    var_a = 0.0;
    for (i = 0;  i < 160;  i++)
    {
        double d1 = a[i] - mean_a;
        var_a += d1*d1;
    }

    best_delay = 0;
    best_corr = -2.0;
    for (d = 40;  d <= 120;  d++)
    {
        int base;
        double sum_b;
        double mean_b;
        double var_b;
        double cross;
        double denom;
        double corr;

        base = s->plc_hist_len - 160 - d;
        sum_b = 0.0;
        for (i = 0;  i < 160;  i++)
            sum_b += g722_plc_hist_get(s, base + i);
        mean_b = sum_b/160.0;
        var_b = 0.0;
        cross = 0.0;
        for (i = 0;  i < 160;  i++)
        {
            double bv = g722_plc_hist_get(s, base + i) - mean_b;
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

    /* Repeat the last complete period, starting at its beginning. */
    s->plc_pitch = best_delay;
    for (i = 0;  i < best_delay;  i++)
        s->plc_period[i] = g722_plc_hist_get(s, s->plc_hist_len - best_delay + i);
}
/*- End of function --------------------------------------------------------*/

/*! Concealed low-band value for subband instant k of the current gap.
    Holds for 40 ms, then decays to silence at 120 ms. */
static int g722_plc_low_sample(const G722_DEC_CTX *s, int k)
{
    int v;

    if (s->plc_pitch <= 0)
        return 0;
    if (k >= 960)
        return 0;
    v = s->plc_period[k % s->plc_pitch];
    if (k <= 320)
        return v;
    return g722_round_div(v*(960 - k), 640);
}
/*- End of function --------------------------------------------------------*/

/*! Concealed high-band value for subband instant k of the current gap: the
    last real high-band value decays to zero over 16 subband instants. */
static int g722_plc_high_sample(const G722_DEC_CTX *s, int k)
{
    if (k >= 16)
        return 0;
    return g722_round_div(s->plc_high_seed*(16 - k), 16);
}
/*- End of function --------------------------------------------------------*/

/*! Linear crossfade from the concealed extension to the real reconstruction.
    At recovery instant j (0..39) the real data weight is (j+1)/40. */
static int g722_plc_crossfade(int concealed, int real, int j)
{
    return g722_round_div(concealed*(39 - j) + real*(j + 1), 40);
}
/*- End of function --------------------------------------------------------*/

G722_DEC_CTX *g722_decoder_new(int rate, int options)
{
    G722_DEC_CTX *s;

    if ((s = (G722_DEC_CTX *) malloc(sizeof(*s))) == NULL)
        return NULL;
    memset(s, 0, sizeof(*s));
    if (rate == 48000)
        s->bits_per_sample = 6;
    else if (rate == 56000)
        s->bits_per_sample = 7;
    else
        s->bits_per_sample = 8;
    if ((options & G722_SAMPLE_RATE_8000))
        s->eight_k = TRUE;
    if ((options & G722_PACKED)  &&  s->bits_per_sample != 8)
        s->packed = TRUE;
    else
        s->packed = FALSE;
    s->band[0].det = 32;
    s->band[1].det = 8;
    return s;
}
/*- End of function --------------------------------------------------------*/

int g722_decoder_destroy(G722_DEC_CTX *s)
{
    free(s);
    return 0;
}
/*- End of function --------------------------------------------------------*/

int g722_decode(G722_DEC_CTX *s, const uint8_t g722_data[], int len, int16_t amp[])
{
    static const int wl[8] = {-60, -30, 58, 172, 334, 538, 1198, 3042 };
    static const int rl42[16] = {0, 7, 6, 5, 4, 3, 2, 1, 7, 6, 5, 4, 3,  2, 1, 0 };
    static const int ilb[32] =
    {
        2048, 2093, 2139, 2186, 2233, 2282, 2332,
        2383, 2435, 2489, 2543, 2599, 2656, 2714,
        2774, 2834, 2896, 2960, 3025, 3091, 3158,
        3228, 3298, 3371, 3444, 3520, 3597, 3676,
        3756, 3838, 3922, 4008
    };
    static const int wh[3] = {0, -214, 798};
    static const int rh2[4] = {2, 1, 2, 1};
    static const int qm2[4] = {-7408, -1616,  7408,   1616};
    static const int qm4[16] = 
    {
              0, -20456, -12896,  -8968, 
          -6288,  -4240,  -2584,  -1200,
          20456,  12896,   8968,   6288,
           4240,   2584,   1200,      0
    };
    static const int qm5[32] =
    {
           -280,   -280, -23352, -17560,
         -14120, -11664,  -9752,  -8184,
          -6864,  -5712,  -4696,  -3784,
          -2960,  -2208,  -1520,   -880,
          23352,  17560,  14120,  11664,
           9752,   8184,   6864,   5712,
           4696,   3784,   2960,   2208,
           1520,    880,    280,   -280
    };
    static const int qm6[64] =
    {
           -136,   -136,   -136,   -136,
         -24808, -21904, -19008, -16704,
         -14984, -13512, -12280, -11192,
         -10232,  -9360,  -8576,  -7856,
          -7192,  -6576,  -6000,  -5456,
          -4944,  -4464,  -4008,  -3576,
          -3168,  -2776,  -2400,  -2032,
          -1688,  -1360,  -1040,   -728,
          24808,  21904,  19008,  16704,
          14984,  13512,  12280,  11192,
          10232,   9360,   8576,   7856,
           7192,   6576,   6000,   5456,
           4944,   4464,   4008,   3576,
           3168,   2776,   2400,   2032,
           1688,   1360,   1040,    728,
            432,    136,   -432,   -136
    };
    int dlowt;
    int rlow;
    int ihigh;
    int dhigh;
    int rhigh;
    int real_low;
    int real_high;
    int wd1;
    int wd2;
    int wd3;
    int code;
    int outlen;
    int j;

    outlen = 0;
    rhigh = 0;
    for (j = 0;  j < len;  )
    {
        if (s->packed)
        {
            /* Unpack the code bits */
            if (s->in_bits < s->bits_per_sample)
            {
                s->in_buffer |= (g722_data[j++] << s->in_bits);
                s->in_bits += 8;
            }
            code = s->in_buffer & ((1 << s->bits_per_sample) - 1);
            s->in_buffer >>= s->bits_per_sample;
            s->in_bits -= s->bits_per_sample;
        }
        else
        {
            code = g722_data[j++];
        }

        switch (s->bits_per_sample)
        {
        default:
        case 8:
            wd1 = code & 0x3F;
            ihigh = (code >> 6) & 0x03;
            wd2 = qm6[wd1];
            wd1 >>= 2;
            break;
        case 7:
            wd1 = code & 0x1F;
            ihigh = (code >> 5) & 0x03;
            wd2 = qm5[wd1];
            wd1 >>= 1;
            break;
        case 6:
            wd1 = code & 0x0F;
            ihigh = (code >> 4) & 0x03;
            wd2 = qm4[wd1];
            break;
        }
        /* Block 5L, LOW BAND INVQBL */
        wd2 = (s->band[0].det*wd2) >> 15;
        /* Block 5L, RECONS */
        rlow = s->band[0].s + wd2;
        /* Block 6L, LIMIT */
        if (rlow > 16383)
            rlow = 16383;
        else if (rlow < -16384)
            rlow = -16384;

        /* Block 2L, INVQAL */
        wd2 = qm4[wd1];
        dlowt = (s->band[0].det*wd2) >> 15;

        /* Block 3L, LOGSCL */
        wd2 = rl42[wd1];
        wd1 = (s->band[0].nb*127) >> 7;
        wd1 += wl[wd2];
        if (wd1 < 0)
            wd1 = 0;
        else if (wd1 > 18432)
            wd1 = 18432;
        s->band[0].nb = wd1;
            
        /* Block 3L, SCALEL */
        wd1 = (s->band[0].nb >> 6) & 31;
        wd2 = 8 - (s->band[0].nb >> 11);
        wd3 = (wd2 < 0)  ?  (ilb[wd1] << -wd2)  :  (ilb[wd1] >> wd2);
        s->band[0].det = wd3 << 2;

        block4(&s->band[0], dlowt);
        
        if (!s->eight_k)
        {
            /* Block 2H, INVQAH */
            wd2 = qm2[ihigh];
            dhigh = (s->band[1].det*wd2) >> 15;
            /* Block 5H, RECONS */
            rhigh = dhigh + s->band[1].s;
            /* Block 6H, LIMIT */
            if (rhigh > 16383)
                rhigh = 16383;
            else if (rhigh < -16384)
                rhigh = -16384;

            /* Block 2H, INVQAH */
            wd2 = rh2[ihigh];
            wd1 = (s->band[1].nb*127) >> 7;
            wd1 += wh[wd2];
            if (wd1 < 0)
                wd1 = 0;
            else if (wd1 > 22528)
                wd1 = 22528;
            s->band[1].nb = wd1;
            
            /* Block 3H, SCALEH */
            wd1 = (s->band[1].nb >> 6) & 31;
            wd2 = 10 - (s->band[1].nb >> 11);
            wd3 = (wd2 < 0)  ?  (ilb[wd1] << -wd2)  :  (ilb[wd1] >> wd2);
            s->band[1].det = wd3 << 2;

            block4(&s->band[1], dhigh);
        }

        if (s->plc_gap)
        {
            /* First real codeword after a missing interval: the gap ends here
               and the concealed extension is crossfaded into the real
               reconstruction over the next 40 subband instants. */
            s->plc_transition = TRUE;
            s->plc_recover = 0;
        }
        if (s->plc_transition)
        {
            /* Crossfade the concealed extension into the real reconstruction
               across the first 40 subband instants after real data resumes.
               This happens before the receive QMF, on the subband values. */
            int concealed_low;
            int concealed_high;

            real_low = rlow;
            real_high = rhigh;
            concealed_low = g722_plc_low_sample(s, s->plc_k);
            concealed_high = g722_plc_high_sample(s, s->plc_k);
            rlow = g722_plc_crossfade(concealed_low, rlow, s->plc_recover);
            rhigh = g722_plc_crossfade(concealed_high, rhigh, s->plc_recover);
        }
        else
        {
            real_low = rlow;
            real_high = rhigh;
        }

        g722_emit_subbands(s, rlow, rhigh, amp, &outlen);

        /* The real ADPCM state has already been advanced above; now record
           the genuine (pre-crossfade) subband history and advance the
           recovery progress. */
        g722_plc_push_low(s, real_low);
        s->plc_last_rhigh = real_high;
        if (s->plc_transition)
        {
            s->plc_k++;
            if (++s->plc_recover >= 40)
                s->plc_transition = FALSE;
        }
        s->plc_gap = FALSE;
    }
    return outlen;
}
/*- End of function --------------------------------------------------------*/

int g722_fillin(G722_DEC_CTX *s, int16_t out[], int missing_pairs)
{
    int k;
    int outlen;
    int rlow;
    int rhigh;

    if (missing_pairs == 0)
        return 0;
    if (missing_pairs < 0  ||  missing_pairs > 8000  ||  s->packed)
        return -1;

    if (!s->plc_gap)
    {
        /* A new missing interval starts here.  Anchor the concealment on the
           real history captured so far and on the last real high-band value. */
        g722_plc_pick_period(s);
        s->plc_high_seed = s->plc_last_rhigh;
        s->plc_k = 0;
        s->plc_recover = 0;
        s->plc_transition = FALSE;
        s->plc_gap = TRUE;
    }

    outlen = 0;
    for (k = 0;  k < missing_pairs;  k++)
    {
        rlow = g722_plc_low_sample(s, s->plc_k);
        rhigh = 0;
        if (!s->eight_k)
            rhigh = g722_plc_high_sample(s, s->plc_k);
        /* Advance the receive QMF history for every concealed subband instant.
           No fake codewords are built and the ADPCM predictor/quantiser
           adaptation state is left untouched. */
        g722_emit_subbands(s, rlow, rhigh, out, &outlen);
        s->plc_k++;
    }
    return outlen;
}
/*- End of function --------------------------------------------------------*/
/*- End of file ------------------------------------------------------------*/
