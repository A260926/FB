/******************************************************************************
 *
 *  Copyright 2022 Google LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/**
 * Regression suite for the offline rate-distortion joint-stereo encoder.
 *
 * The expected decisions are produced by a fully independent reference
 * implemented in this file : its own MSB-first bit reader, its own
 * LOUDNESS / SNR bit allocation and its own fixed-point quantizer /
 * decoder reconstruction. Every legal joint mask is enumerated from the
 * raw QMF samples and the emitted SBC syntax (joint bits, scale factors,
 * quantization words, padding) is checked against that reference. The
 * production selector and the production cost function are never called to
 * compute an expected value.
 */

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sbc.h>

/* The raw analyzed sub-band samples come from the shared, single QMF
 * analysis (neither the selector nor a cost function). Everything else in
 * this test is an independent re-implementation. */
#include "sbc_internal.h"


/* ----------------------------------------------------------------------------
 *  Independent MSB-first bit reader
 * ------------------------------------------------------------------------- */

struct ref_reader {
    const uint8_t *p;
    unsigned nbytes;
    unsigned bitpos;
};

static void ref_reader_init(struct ref_reader *r, const uint8_t *p, unsigned n)
{
    r->p = p; r->nbytes = n; r->bitpos = 0;
}

static unsigned ref_get(struct ref_reader *r, unsigned n)
{
    unsigned v = 0;

    for (unsigned i = 0; i < n; i++) {
        unsigned byte = r->bitpos >> 3;
        if (byte >= r->nbytes) { r->bitpos += n - i; break; }
        unsigned bit = 7 - (r->bitpos & 7);
        v = (v << 1) | ((r->p[byte] >> bit) & 1);
        r->bitpos++;
    }
    return v;
}


/* ----------------------------------------------------------------------------
 *  Independent normative constants and fixed-point helpers
 * ------------------------------------------------------------------------- */

static const int ref_range_scale[16] = {
    0xFFFFFFF, 0x5555556, 0x2492492, 0x1111111,
    0x0842108, 0x0410410, 0x0204081, 0x0101010,
    0x0080402, 0x0040100, 0x0020040, 0x0010010,
    0x0008004, 0x0004001, 0x0002000, 0x0001000
};

static const int ref_loudness_4[SBC_NUM_FREQ][4] = {
    [SBC_FREQ_16K ] = { -1,  0,  0,  0 },
    [SBC_FREQ_32K ] = { -2,  0,  0,  1 },
    [SBC_FREQ_44K1] = { -2,  0,  0,  1 },
    [SBC_FREQ_48K ] = { -2,  0,  0,  1 },
};

static const int ref_loudness_8[SBC_NUM_FREQ][8] = {
    [SBC_FREQ_16K ] = { -2, 0, 0, 0, 0, 0, 0, 1 },
    [SBC_FREQ_32K ] = { -3, 0, 0, 0, 0, 0, 1, 2 },
    [SBC_FREQ_44K1] = { -4, 0, 0, 0, 0, 0, 1, 2 },
    [SBC_FREQ_48K ] = { -4, 0, 0, 0, 0, 0, 1, 2 },
};

static int ref_asr(int v, int n) { return n >= 0 ? v >> n : v << (-n); }

static int ref_clz(unsigned n)
{
    int k = 0;
    while (n >>= 1) k++;
    return 31 - k;
}


/* ----------------------------------------------------------------------------
 *  Independent scale factors / allocation / reconstruction
 * ------------------------------------------------------------------------- */

static void ref_scale_factors(const struct sbc_frame *fr,
    const int16_t sb[2][SBC_MAX_SAMPLES], unsigned mjoint,
    int scf[2][SBC_MAX_SUBBANDS])
{
    int nsb = fr->nsubbands;

    for (int isb = 0; isb < nsb; isb++) {
        unsigned m0 = 0, m1 = 0;

        for (int iblk = 0; iblk < fr->nblocks; iblk++) {
            int s0 = sb[0][iblk*nsb+isb];
            int s1 = sb[1][iblk*nsb+isb];
            int c0 = ((mjoint >> isb) & 1) ? s0 + s1 : s0;
            int c1 = ((mjoint >> isb) & 1) ? s0 - s1 : s1;

            m0 |= c0 < 0 ? ~c0 : c0;
            m1 |= c1 < 0 ? ~c1 : c1;
        }

        scf[0][isb] = m0 ? 31 - ref_clz(m0) : 0;
        scf[1][isb] = m1 ? 31 - ref_clz(m1) : 0;
    }
}

/* Independent LOUDNESS / SNR allocation, re-derived from the SBC spec */
static void ref_allocate(const struct sbc_frame *fr,
    const int scf[2][SBC_MAX_SUBBANDS], int nbits[2][SBC_MAX_SUBBANDS])
{
    int nsb = fr->nsubbands;
    const int *off = nsb == 4 ? ref_loudness_4[fr->freq]
                              : ref_loudness_8[fr->freq];

    int bitneeds[2][SBC_MAX_SUBBANDS];
    int max_bitneed = 0;

    for (int ich = 0; ich < 2; ich++)
        for (int isb = 0; isb < nsb; isb++) {
            int bn;
            if (fr->bam == SBC_BAM_LOUDNESS) {
                bn = scf[ich][isb] ? scf[ich][isb] - off[isb] : -5;
                bn >>= (bn > 0);
            } else {
                bn = scf[ich][isb];
            }
            bitneeds[ich][isb] = bn;
            if (bn > max_bitneed) max_bitneed = bn;
        }

    int bitpool = fr->bitpool, bitcount = 0, bitslice = max_bitneed + 1;

    for (int bc = 0; bc < bitpool; ) {
        int bs = bitslice--;
        bitcount = bc;
        if (bitcount == bitpool) break;
        for (int ich = 0; ich < 2; ich++)
            for (int isb = 0; isb < nsb; isb++) {
                int bn = bitneeds[ich][isb];
                bc += (bn >= bs && bn < bs + 15) + (bn == bs);
            }
    }

    for (int ich = 0; ich < 2; ich++)
        for (int isb = 0; isb < nsb; isb++) {
            int n = bitneeds[ich][isb] - bitslice;
            nbits[ich][isb] = n < 2 ? 0 : n > 16 ? 16 : n;
        }

    for (int isb = 0; isb < nsb && bitcount < bitpool; isb++)
        for (int ich = 0; ich < 2 && bitcount < bitpool; ich++) {
            int n = nbits[ich][isb] && nbits[ich][isb] < 16 ? 1 :
                    bitneeds[ich][isb] == bitslice + 1 &&
                    bitpool > bitcount + 1 ? 2 : 0;
            nbits[ich][isb] += n;
            bitcount += n;
        }

    for (int isb = 0; isb < nsb && bitcount < bitpool; isb++)
        for (int ich = 0; ich < 2 && bitcount < bitpool; ich++) {
            int n = nbits[ich][isb] < 16;
            nbits[ich][isb] += n;
            bitcount += n;
        }
}

/* Independent greedy per-sub-band rule of the legacy encoder */
static unsigned ref_greedy_mask(const struct sbc_frame *fr,
    const int16_t sb[2][SBC_MAX_SAMPLES])
{
    int nsb = fr->nsubbands;
    unsigned mjoint = 0;

    for (int isb = 0; isb < nsb - 1; isb++) {
        unsigned m[2] = { }, mj[2] = { };

        for (int iblk = 0; iblk < fr->nblocks; iblk++) {
            int s0 = sb[0][iblk*nsb+isb];
            int s1 = sb[1][iblk*nsb+isb];
            m[0]  |= s0 < 0 ? ~s0 : s0;
            m[1]  |= s1 < 0 ? ~s1 : s1;
            mj[0] |= (s0+s1) < 0 ? ~(s0+s1) : (s0+s1);
            mj[1] |= (s0-s1) < 0 ? ~(s0-s1) : (s0-s1);
        }

        int scf0 = m[0]  ? 31 - ref_clz(m[0])  : 0;
        int scf1 = m[1]  ? 31 - ref_clz(m[1])  : 0;
        int js0  = mj[0] ? 31 - ref_clz(mj[0]) : 0;
        int js1  = mj[1] ? 31 - ref_clz(mj[1]) : 0;

        if (js0 + js1 < scf0 + scf1)
            mjoint |= 1u << isb;
    }
    return mjoint;
}

struct ref_candidate {
    double cost;
    int nbits[2][SBC_MAX_SUBBANDS];
    int qword[2][SBC_MAX_SAMPLES];
    int zero_bands;
};

/* Full candidate evaluation: own quantizer words, own decoder-style
 * reconstruction back to the input sub-band scale, total squared error. */
static struct ref_candidate ref_evaluate(const struct sbc_frame *fr,
    const int16_t sb[2][SBC_MAX_SAMPLES], unsigned mjoint,
    const int scf[2][SBC_MAX_SUBBANDS])
{
    int nsb = fr->nsubbands, nblk = fr->nblocks;
    struct ref_candidate c;
    memset(&c, 0, sizeof c);

    ref_allocate(fr, scf, c.nbits);

    int sb_scale[2];
    for (int ich = 0; ich < 2; ich++) {
        int max_scf = 0;
        for (int isb = 0; isb < nsb; isb++) {
            int v = scf[ich][isb] + ((mjoint >> isb) & 1);
            if (v > max_scf) max_scf = v;
        }
        sb_scale[ich] = (15 - max_scf) - 1;
    }
    if (sb_scale[0] > sb_scale[1]) sb_scale[0] = sb_scale[1];
    else                           sb_scale[1] = sb_scale[0];

    int16_t rec[2][SBC_MAX_SAMPLES];

    for (int ich = 0; ich < 2; ich++)
        for (int iblk = 0; iblk < nblk; iblk++)
            for (int isb = 0; isb < nsb; isb++) {
                int idx = iblk*nsb + isb;
                int s0 = sb[0][idx], s1 = sb[1][idx];
                int s = ich == 0 ? s0 : s1;
                if ((mjoint >> isb) & 1)
                    s = (int16_t)(ich == 0 ? (s0+s1) >> 1
                                           : (s0-s1) >> 1);

                int nbit = c.nbits[ich][isb], sc = scf[ich][isb];
                int q = 0, value = 0;

                if (!nbit) {
                    c.zero_bands++;
                } else {
                    int range = ~(UINT_MAX << nbit);
                    q = (((s * range) >> (sc + 1)) + range) >> 1;
                    int built = ((q << 1) | 1) * ref_range_scale[nbit-1];
                    value = ref_asr(built - (1 << 28),
                                    28 - ((sc + 1) + sb_scale[ich]));
                }

                c.qword[ich][idx] = q;
                rec[ich][idx] = (int16_t)value;
            }

    for (int isb = 0; isb < nsb; isb++) {
        if (!((mjoint >> isb) & 1)) continue;
        for (int iblk = 0; iblk < nblk; iblk++) {
            int idx = iblk*nsb + isb;
            int16_t a = rec[0][idx], b = rec[1][idx];
            rec[0][idx] = (int16_t)(a + b);
            rec[1][idx] = (int16_t)(a - b);
        }
    }

    /* 2^(-sb_scale), computed without libm */
    double gain = 1.0;
    if (sb_scale[0] >= 0) gain /= (double)(1L << sb_scale[0]);
    else                  gain *= (double)(1L << -sb_scale[0]);

    for (int ich = 0; ich < 2; ich++)
        for (int i = 0; i < nblk*nsb; i++) {
            double e = (double)rec[ich][i] * gain - (double)sb[ich][i];
            c.cost += e * e;
        }

    return c;
}

struct ref_decision {
    unsigned mask;
    double   cost;
    double   greedy_cost;
    unsigned greedy_mask;
    int      scf[2][SBC_MAX_SUBBANDS];
    struct ref_candidate cand;
};

/* Enumerate every legal mask (highest sub-band always uncoupled) and apply
 * the 1e-12 tie rule, breaking ties toward the smallest numeric mask. */
static struct ref_decision ref_select(const struct sbc_frame *fr,
    const int16_t sb[2][SBC_MAX_SAMPLES])
{
    int nsb = fr->nsubbands, nmasks = 1 << (nsb - 1);
    double costs[1 << (SBC_MAX_SUBBANDS - 1)];
    int all_scf[1 << (SBC_MAX_SUBBANDS - 1)][2][SBC_MAX_SUBBANDS];
    struct ref_candidate all_cand[1 << (SBC_MAX_SUBBANDS - 1)];

    double min_cost = 0;
    for (int m = 0; m < nmasks; m++) {
        ref_scale_factors(fr, sb, (unsigned)m, all_scf[m]);
        all_cand[m] = ref_evaluate(fr, sb, (unsigned)m, all_scf[m]);
        costs[m] = all_cand[m].cost;
        if (m == 0 || costs[m] < min_cost) min_cost = costs[m];
    }

    double tol = 1e-12 * (min_cost > 1.0 ? min_cost : 1.0);
    unsigned chosen = 0;
    for (int m = 0; m < nmasks; m++)
        if (costs[m] <= min_cost + tol) { chosen = (unsigned)m; break; }

    struct ref_decision d;
    d.mask = chosen;
    d.cost = costs[chosen];
    d.greedy_mask = ref_greedy_mask(fr, sb);
    d.greedy_cost = costs[d.greedy_mask];
    memcpy(d.scf, all_scf[chosen], sizeof d.scf);
    d.cand = all_cand[chosen];
    return d;
}


/* ----------------------------------------------------------------------------
 *  Independent parsing of the emitted stream
 * ------------------------------------------------------------------------- */

static unsigned ref_read_joint(struct ref_reader *r, int nsb)
{
    if (nsb == 4) {
        unsigned v = ref_get(r, 4);
        return ((v & 0x02) << 1) | ((v & 0x04) >> 1) | ((v & 0x08) >> 3);
    }
    unsigned v = ref_get(r, 8);
    return ((v & 0x02) << 5) | ((v & 0x04) << 3) | ((v & 0x08) << 1) |
           ((v & 0x10) >> 1) | ((v & 0x20) >> 3) | ((v & 0x40) >> 5) |
           ((v & 0x80) >> 7);
}

/* Verify every syntax element of the emitted frame against the reference */
static int verify_stream(const struct sbc_frame *fr,
    const uint8_t *data, unsigned fsize, const struct ref_decision *d)
{
    int nsb = fr->nsubbands, nblk = fr->nblocks;
    struct ref_reader r;
    ref_reader_init(&r, data, fsize);

    /* Header */
    if (ref_get(&r, 8) != 0x9c) return 0;
    ref_get(&r, 2); /* freq */
    ref_get(&r, 2); /* blocks */
    if (ref_get(&r, 2) != SBC_MODE_JOINT_STEREO) return 0;
    ref_get(&r, 1); /* bam */
    ref_get(&r, 1); /* subbands */
    if (ref_get(&r, 8) != (unsigned)fr->bitpool) return 0;
    ref_get(&r, 8); /* crc */

    if (ref_read_joint(&r, nsb) != d->mask) return 0;

    int scf[2][SBC_MAX_SUBBANDS];
    for (int ich = 0; ich < 2; ich++)
        for (int isb = 0; isb < nsb; isb++) {
            scf[ich][isb] = (int)ref_get(&r, 4);
            if (scf[ich][isb] != d->scf[ich][isb]) return 0;
        }

    int nbits[2][SBC_MAX_SUBBANDS];
    ref_allocate(fr, scf, nbits);
    for (int ich = 0; ich < 2; ich++)
        for (int isb = 0; isb < nsb; isb++)
            if (nbits[ich][isb] != d->cand.nbits[ich][isb]) return 0;

    for (int iblk = 0; iblk < nblk; iblk++)
        for (int ich = 0; ich < 2; ich++)
            for (int isb = 0; isb < nsb; isb++) {
                int nbit = nbits[ich][isb];
                if (!nbit) continue;
                unsigned w = ref_get(&r, (unsigned)nbit);
                if ((int)w != d->cand.qword[ich][iblk*nsb+isb]) return 0;
            }

    /* Padding lands exactly on the frame byte boundary */
    unsigned used = r.bitpos;
    if (used > 8*fsize || 8*fsize - used >= 8) return 0;
    while (r.bitpos < 8*fsize)
        if (ref_get(&r, 1) != 0) return 0;

    return 1;
}


/* ----------------------------------------------------------------------------
 *  Test harness
 * ------------------------------------------------------------------------- */

static int g_failures;
static int g_checks;

static void check(int cond, const char *fmt, ...)
{
    g_checks++;
    if (cond) return;
    g_failures++;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "  FAIL: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void analyze_input(sbc_t *sbc, const struct sbc_frame *fr,
    const int16_t *l, const int16_t *r,
    int16_t sb[2][SBC_MAX_SAMPLES])
{
    sbc_analyze_frame(sbc, fr, l, 1, r, 1, sb);
}
static double decode_energy(const uint8_t *data, unsigned fsize,
    const struct sbc_frame *fr)
{
    sbc_t dec; sbc_reset(&dec);
    struct sbc_frame parsed;
    int16_t pl[SBC_MAX_SAMPLES], pr[SBC_MAX_SAMPLES];
    int rc = sbc_decode(&dec, data, fsize, &parsed, pl, 1, pr, 1);
    check(rc == 0, "public sbc_decode rejected the RD stream");
    double e = 0;
    int n = fr->nblocks * fr->nsubbands;
    for (int i = 0; i < n; i++)
        e += (double)pl[i]*pl[i] + (double)pr[i]*pr[i];
    return e;
}

static void run_case(const char *name,
    const struct sbc_frame *fr, const int16_t *l, const int16_t *r,
    int expect_zero_bands, double expected_min_cost,
    unsigned expected_mask)
{
    printf("case %s\n", name);

    /* A dedicated probe context supplies the single analysis read by the
     * independent reference; the encoder context stays untouched until the
     * actual sbc_encode_rd() call runs its own one-shot analysis. */
    sbc_t probe; sbc_reset(&probe);
    int16_t sb[2][SBC_MAX_SAMPLES];
    analyze_input(&probe, fr, l, r, sb);

    struct ref_decision d = ref_select(fr, sb);

    if (expected_min_cost >= 0)
        check(fabs(d.cost - expected_min_cost) <=
                  1e-9 * (1.0 + expected_min_cost),
              "selected cost %.6f != expected %.6f", d.cost, expected_min_cost);
    if (expected_mask != UINT32_MAX)
        check(d.mask == expected_mask,
              "selected mask 0x%x != expected 0x%x", d.mask, expected_mask);
    check(d.cost <= d.greedy_cost +
              1e-12 * (1.0 + fmax(d.cost, d.greedy_cost)),
          "RD cost %.6f exceeds greedy mask 0x%x cost %.6f",
          d.cost, d.greedy_mask, d.greedy_cost);
    if (expect_zero_bands)
        check(d.cand.zero_bands >= expect_zero_bands,
              "expected >= %d zero-bit bands, got %d",
              expect_zero_bands, d.cand.zero_bands);
    printf("  zero-bit bands=%d selected mask=0x%x cost=%.6f\n",
           d.cand.zero_bands, d.mask, d.cost);

    unsigned fsize = sbc_get_frame_size(fr);
    check(fsize != 0, "invalid frame description");
    uint8_t data[256];
    sbc_t enc; sbc_reset(&enc);
    int rc = sbc_encode_rd(&enc, l, 1, r, 1, fr, data, sizeof data);
    check(rc == 0, "sbc_encode_rd failed");
    check(verify_stream(fr, data, fsize, &d),
          "emitted syntax does not match the reference candidate");

    struct sbc_frame probed;
    check(sbc_probe(data, &probed) == 0 && probed.bitpool == fr->bitpool,
          "probe / bitpool mismatch");

    double energy = decode_energy(data, fsize, fr);
    printf("  decoded PCM energy=%.1f\n", energy);
    if (expected_min_cost == 0.0)
        check(energy == 0.0, "silent frame decoded with sound");
    else
        check(energy > 0.0, "decoded frame has no non-zero sound (e=%.1f)",
              energy);
}

/* Deterministic PRNG so cases are fully reproducible */
static uint32_t g_state;
static uint32_t prng(void) { g_state = g_state*1103515245u + 12345u; return g_state; }
static int16_t rsamp(int amp)
{ return (int16_t)((((int32_t)(prng() & 0xffff) - 0x8000) / 32768.0) * amp); }

int main(void)
{
    const int n4 = 16, n8 = 64;
    int16_t l[64], r[64];

    /* --- Case 1: fixed short input where the greedy mask loses -------- */
    {
        static const int16_t sl[16] = {
            -16000,16000,-16000,16000,-16000,16000,-16000,16000,
            -16000,16000,-16000,16000,-16000,16000,-16000,16000 };
        static const int16_t sr[16] = {
            -12000,-12000,12000,12000,-12000,-12000,12000,12000,
            -12000,-12000,12000,12000,-12000,-12000,12000,12000 };
        struct sbc_frame fr = { .freq = SBC_FREQ_48K,
            .mode = SBC_MODE_JOINT_STEREO, .bam = SBC_BAM_LOUDNESS,
            .nblocks = 4, .nsubbands = 4, .bitpool = 8 };
        run_case("showcase_greedy_suboptimal", &fr, sl, sr, 0,
                 4066778.109375, 0x1);

        /* Explicit greedy-vs-global printout required by the spec */
        sbc_t show_probe; sbc_reset(&show_probe);
        int16_t sb[2][SBC_MAX_SAMPLES];
        analyze_input(&show_probe, &fr, sl, sr, sb);
        struct ref_decision d = ref_select(&fr, sb);
        printf("  greedy mask=0x%x cost=%.3f | global mask=0x%x cost=%.3f\n",
               d.greedy_mask, d.greedy_cost, d.mask, d.cost);
        check(d.greedy_mask == 0x0, "showcase greedy mask changed");
        check(d.greedy_cost > d.cost, "showcase no longer demonstrates a gain");
    }

    /* --- Case 2: 8 sub-bands, SNR, uncorrelated, low bitpool ---------- */
    {
        g_state = 0xA11CE;
        for (int i = 0; i < n8; i++) { l[i] = rsamp(20000); r[i] = rsamp(20000); }
        struct sbc_frame fr = { .freq = SBC_FREQ_44K1,
            .mode = SBC_MODE_JOINT_STEREO, .bam = SBC_BAM_SNR,
            .nblocks = 8, .nsubbands = 8, .bitpool = 6 };
        run_case("nsb8_snr_uncorrelated_zerobit", &fr, l, r, 8, -1, UINT32_MAX);
    }

    /* --- Case 3: 8 sub-bands, LOUDNESS, strongly correlated ------------ */
    {
        g_state = 0xB0B;
        for (int i = 0; i < n8; i++) {
            int16_t base = rsamp(18000);
            l[i] = base;
            r[i] = (int16_t)(base + rsamp(1500));
        }
        struct sbc_frame fr = { .freq = SBC_FREQ_32K,
            .mode = SBC_MODE_JOINT_STEREO, .bam = SBC_BAM_LOUDNESS,
            .nblocks = 8, .nsubbands = 8, .bitpool = 18 };
        run_case("nsb8_loudness_correlated", &fr, l, r, 0, -1, UINT32_MAX);
    }

    /* --- Case 4: anti-phase channels with odd negative samples --------- */
    {
        g_state = 0xD0D0;
        for (int i = 0; i < n4; i++) {
            l[i] = (int16_t)(rsamp(15000) | 1);      /* keep odd values */
            r[i] = (int16_t)(-(l[i] + (l[i] & 1)));  /* odd negatives too */
        }
        struct sbc_frame fr = { .freq = SBC_FREQ_48K,
            .mode = SBC_MODE_JOINT_STEREO, .bam = SBC_BAM_SNR,
            .nblocks = 4, .nsubbands = 4, .bitpool = 10 };
        run_case("antiphase_odd_negative", &fr, l, r, 0, -1, UINT32_MAX);
    }

    /* --- Case 5: silence, all masks tie, smallest mask wins ------------ */
    {
        for (int i = 0; i < n8; i++) l[i] = r[i] = 0;
        struct sbc_frame fr8 = { .freq = SBC_FREQ_48K,
            .mode = SBC_MODE_JOINT_STEREO, .bam = SBC_BAM_LOUDNESS,
            .nblocks = 8, .nsubbands = 8, .bitpool = 12 };
        run_case("silence_tie_smallest_mask", &fr8, l, r, 80, 0.0, 0x0);

        struct sbc_frame fr4 = { .freq = SBC_FREQ_16K,
            .mode = SBC_MODE_JOINT_STEREO, .bam = SBC_BAM_SNR,
            .nblocks = 4, .nsubbands = 4, .bitpool = 4 };
        run_case("silence_tie_smallest_mask_4sb", &fr4, l, r, 24, 0.0, 0x0);
    }

    /* --- Case 6: continuity (no history pollution) and rejection ------- */
    {
        printf("case continuity_and_rejection\n");
        struct sbc_frame fr = { .freq = SBC_FREQ_48K,
            .mode = SBC_MODE_JOINT_STEREO, .bam = SBC_BAM_LOUDNESS,
            .nblocks = 8, .nsubbands = 8, .bitpool = 20 };
        int n = 64;
        g_state = 0x1234;
        int16_t pcm[3][2][64];
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < n; i++) {
                pcm[k][0][i] = rsamp(16000);
                pcm[k][1][i] = rsamp(16000);
            }

        sbc_t rd_ctx, leg_ctx; sbc_reset(&rd_ctx); sbc_reset(&leg_ctx);
        uint8_t a[3][256], b[3][256];

        /* First frame: RD on one context, legacy on the other. Both run the
         * same single QMF analysis, so histories must converge. */
        check(sbc_encode_rd(&rd_ctx, pcm[0][0], 1, pcm[0][1], 1,
                            &fr, a[0], sizeof a[0]) == 0, "rd frame0");
        check(sbc_encode(&leg_ctx, pcm[0][0], 1, pcm[0][1], 1,
                         &fr, b[0], sizeof b[0]) == 0, "legacy frame0");

        for (int k = 1; k < 3; k++) {
            check(sbc_encode(&rd_ctx, pcm[k][0], 1, pcm[k][1], 1,
                             &fr, a[k], sizeof a[k]) == 0, "rd-then-legacy");
            check(sbc_encode(&leg_ctx, pcm[k][0], 1, pcm[k][1], 1,
                             &fr, b[k], sizeof b[k]) == 0, "legacy-only");
            check(memcmp(a[k], b[k], sbc_get_frame_size(&fr)) == 0,
                  "legacy frame %d polluted after RD encoding", k);
        }

        /* Invalid configurations and undersized buffers must fail before
         * advancing analysis history or touching the output buffer. */
        uint8_t sentinel[256];
        memset(sentinel, 0xA5, sizeof sentinel);
        uint8_t out[256];
        memcpy(out, sentinel, sizeof out);

        struct sbc_frame bad_msbc = fr; bad_msbc.msbc = true;
        struct sbc_frame bad_mono = fr; bad_mono.mode = SBC_MODE_MONO;
        struct sbc_frame bad_stereo = fr; bad_stereo.mode = SBC_MODE_STEREO;
        struct sbc_frame bad_dual = fr; bad_dual.mode = SBC_MODE_DUAL_CHANNEL;

        check(sbc_encode_rd(&rd_ctx, pcm[0][0], 1, pcm[0][1], 1,
                            &bad_msbc, out, sizeof out) == -1, "mSBC accepted");
        check(sbc_encode_rd(&rd_ctx, pcm[0][0], 1, pcm[0][1], 1,
                            &bad_mono, out, sizeof out) == -1, "mono accepted");
        check(sbc_encode_rd(&rd_ctx, pcm[0][0], 1, pcm[0][1], 1,
                            &bad_stereo, out, sizeof out) == -1, "stereo accepted");
        check(sbc_encode_rd(&rd_ctx, pcm[0][0], 1, pcm[0][1], 1,
                            &bad_dual, out, sizeof out) == -1, "dual accepted");
        check(sbc_encode_rd(&rd_ctx, pcm[0][0], 1, pcm[0][1], 1,
                            &fr, out, sbc_get_frame_size(&fr) - 1) == -1,
              "undersized buffer accepted");
        check(memcmp(out, sentinel, sizeof out) == 0,
              "failed call wrote into the output buffer");

        /* A following valid frame must match the untouched context */
        uint8_t after[256], expect[256];
        check(sbc_encode(&rd_ctx, pcm[0][0], 1, pcm[0][1], 1,
                         &fr, after, sizeof after) == 0, "encode after rejects");
        check(sbc_encode(&leg_ctx, pcm[0][0], 1, pcm[0][1], 1,
                         &fr, expect, sizeof expect) == 0, "ref encode");
        check(memcmp(after, expect, sbc_get_frame_size(&fr)) == 0,
              "rejected calls advanced the analysis history");
    }

    if (g_failures) {
        fprintf(stderr, "RD_REGRESSION_FAIL checks=%d failures=%d\n",
                g_checks, g_failures);
        return 1;
    }
    printf("checks=%d\n", g_checks);
    printf("SBC_RD_OK cases=6\n");
    return 0;
}
