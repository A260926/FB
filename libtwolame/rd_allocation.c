/*
 *  TwoLAME: an optimized MPEG Audio Layer Two encoder
 *
 *  Copyright (C) 2001-2004 Michael Cheng
 *  Copyright (C) 2004-2018 The TwoLAME Project
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/*
 * Intra-frame rate-distortion optimization.
 *
 * For every active subband/channel unit the four SCFSI scalefactor
 * sharing patterns and every legal non-zero allocation level are
 * evaluated with one shared quantization rule. The whole frame is
 * then solved as a multiple-choice knapsack problem - minimize the
 * squared reconstruction error within the fixed no-padding bit budget
 * - using an exact bounded depth-first search with branch pruning and
 * backtracking. No per-subband independent truncation or greedy
 * decision is used for the final choice.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "twolame.h"
#include "common.h"
#include "bitbuffer.h"
#include "encode.h"
#include "bitbuffer_inline.h"


/* Number of scalefactors actually transmitted for each SCFSI code */
static const int rd_sfs_per_scfsi[4] = { 3, 2, 1, 2 };

/* Maximum number of allocation levels (nbal is at most 4) times the
   four SCFSI patterns, plus the zero allocation candidate. */
#define RD_MAX_CANDS ((1 << 4) * 4 + 1)


typedef struct {
    int ba;                     /* bit allocation index, 0 means silent */
    int scfsi;                  /* SCFSI code (0..3), unused when ba == 0 */
    int nbits;                  /* bits used beyond the unit's nbal field */
    double dist;                /* squared reconstruction error, 36 samples */
    unsigned int sf[3];         /* scalefactor indices applied per group */
    unsigned int code[3][SCALE_BLOCK]; /* quantized sample codes */
} rd_candidate;

typedef struct {
    int ch;
    int sb;
    int max_alloc;
    int ncand;                  /* candidates after dominance filtering */
    double zero_dist;           /* error of the zero allocation choice */
    rd_candidate cand[RD_MAX_CANDS];
} rd_unit;


/* Quantize one sample with the single RD quantization rule.
   q = clamp(floor((x/s + 1) * L/2), 0, L-1) */
static int rd_quantize(FLOAT x, FLOAT s, int L)
{
    double r = floor(((double) x / (double) s + 1.0) * (double) L * 0.5);
    if (r < 0.0)
        r = 0.0;
    if (r > (double) (L - 1))
        r = (double) (L - 1);
    return (int) r;
}


/* Distortion of one unit for a given set of three transmitted
   scalefactor indices; also fills in the quantized sample codes. */
static double rd_unit_distortion(twolame_options * glopts, int ch, int sb,
                                 const unsigned int sf[3], int L,
                                 unsigned int code[3][SCALE_BLOCK])
{
    int gr, j;
    double dist = 0.0;

    for (gr = 0; gr < 3; gr++) {
        FLOAT s = twolame_rd_scalefactor((int) sf[gr]);
        for (j = 0; j < SCALE_BLOCK; j++) {
            FLOAT x = (*glopts->sb_sample)[ch][gr][j][sb];
            int q = rd_quantize(x, s, L);
            FLOAT y = s * (FLOAT) (2 * q + 1 - L) / (FLOAT) L;
            FLOAT d = x - y;
            code[gr][j] = (unsigned int) q;
            dist += (double) d * (double) d;
        }
    }
    return dist;
}


/* Build the SCFSI-specific scalefactor triple from the raw indices.
   Smaller index means a larger scalefactor. */
static void rd_pattern_sf(const unsigned int m[3], int scfsi, unsigned int sf[3])
{
    unsigned int u, v;

    switch (scfsi) {
    case 0:
        sf[0] = m[0];
        sf[1] = m[1];
        sf[2] = m[2];
        break;
    case 1:
        u = MIN(m[0], m[1]);
        sf[0] = sf[1] = u;
        sf[2] = m[2];
        break;
    case 2:
        u = MIN(m[0], MIN(m[1], m[2]));
        sf[0] = sf[1] = sf[2] = u;
        break;
    default:                    /* case 3 */
        v = MIN(m[1], m[2]);
        sf[0] = m[0];
        sf[1] = sf[2] = v;
        break;
    }
}


/* Remove candidates which can never be part of an optimal solution:
   a candidate dominated by another candidate using no more bits and
   producing no more error. Transitivity makes it safe to compare
   against candidates that are themselves going to be removed. */
static void rd_filter_dominated(rd_unit * u)
{
    int i, kept = 0;

    /* cand[0] is the zero allocation candidate and always stays. */
    for (i = 1; i < u->ncand; i++) {
        int j;
        int dominated = 0;
        for (j = 0; j < i; j++) {
            if (u->cand[j].nbits <= u->cand[i].nbits &&
                u->cand[j].dist <= u->cand[i].dist) {
                dominated = 1;
                break;
            }
        }
        if (dominated)
            continue;
        kept++;
        if (kept != i)
            u->cand[kept] = u->cand[i];
    }
    u->ncand = kept + 1;
}


/* Build all candidates of one unit. */
static void rd_build_unit(twolame_options * glopts, int ch, int sb, rd_unit * u)
{
    unsigned int m[3];
    int ba, scfsi;

    u->ch = ch;
    u->sb = sb;
    u->max_alloc = twolame_rd_max_alloc_for(glopts, sb);
    u->ncand = 0;

    m[0] = glopts->scalar[ch][0][sb];
    m[1] = glopts->scalar[ch][1][sb];
    m[2] = glopts->scalar[ch][2][sb];

    /* Zero allocation: no SCFSI, no scalefactors, 36 zero samples. */
    {
        rd_candidate *c = &u->cand[u->ncand];
        int gr, j;
        double dist = 0.0;
        memset(c, 0, sizeof(*c));
        c->ba = 0;
        c->scfsi = 0;
        c->nbits = 0;
        c->sf[0] = m[0];
        c->sf[1] = m[1];
        c->sf[2] = m[2];
        for (gr = 0; gr < 3; gr++)
            for (j = 0; j < SCALE_BLOCK; j++) {
                FLOAT x = (*glopts->sb_sample)[ch][gr][j][sb];
                dist += (double) x * (double) x;
            }
        c->dist = dist;
        u->zero_dist = dist;
        u->ncand++;
    }

    /* Every legal non-zero allocation level, all four SCFSI patterns. */
    for (ba = 1; ba <= u->max_alloc; ba++) {
        int stepidx = twolame_rd_step_index_for(glopts, sb, ba);
        int L = twolame_rd_steps(stepidx);
        int sample_bits = SCALE_BLOCK * twolame_rd_group(stepidx) * twolame_rd_bits(stepidx);

        for (scfsi = 0; scfsi < 4; scfsi++) {
            rd_candidate *c = &u->cand[u->ncand];
            memset(c, 0, sizeof(*c));
            c->ba = ba;
            c->scfsi = scfsi;
            rd_pattern_sf(m, scfsi, c->sf);
            c->nbits = 2 + 6 * rd_sfs_per_scfsi[scfsi] + sample_bits;
            c->dist = rd_unit_distortion(glopts, ch, sb, c->sf, L, c->code);
            u->ncand++;
        }
    }

    rd_filter_dominated(u);
}


#define RD_INF 1e300

/* Exact multiple-choice knapsack over the integer bit budget.
   dp holds, for every exact number of spent bits, the minimum total
   distortion of the units processed so far. The choice table records
   which candidate produced each state so the optimum can be
   backtracked. The final answer is the minimum distortion at or below
   the budget; exact cost ties therefore resolve to the fewest bits.
   The work is bounded by units * budget * candidates - there is no
   enumeration of unit combinations. */
static int rd_solve(rd_unit * units, int nunits, int budget, int *best_choice)
{
    double *dp_prev, *dp_cur;
    int *take;                  /* candidate chosen at (unit, bits) */
    int width = budget + 1;
    int reachable = 0;          /* max bits reachable with processed units */
    int i, b, k;

    dp_prev = (double *) malloc((size_t) width * sizeof(double));
    dp_cur = (double *) malloc((size_t) width * sizeof(double));
    take = (int *) malloc((size_t) nunits * (size_t) width * sizeof(int));
    if (dp_prev == NULL || dp_cur == NULL || take == NULL) {
        free(dp_prev);
        free(dp_cur);
        free(take);
        return -1;
    }

    dp_prev[0] = 0.0;
    for (b = 1; b < width; b++)
        dp_prev[b] = RD_INF;

    for (i = 0; i < nunits; i++) {
        rd_unit *u = &units[i];
        int max_spend = reachable;
        int *row = take + (size_t) i * (size_t) width;

        for (b = 0; b < width; b++)
            dp_cur[b] = RD_INF;

        for (b = 0; b <= reachable; b++) {
            if (dp_prev[b] >= RD_INF)
                continue;
            for (k = 0; k < u->ncand; k++) {
                int w = u->cand[k].nbits;
                int nb = b + w;
                double val;
                if (nb > budget)
                    continue;
                val = dp_prev[b] + u->cand[k].dist;
                if (nb > max_spend)
                    max_spend = nb;
                /* Keep the first candidate on an exact tie so the
                   resolution order stays stable and deterministic. */
                if (val < dp_cur[nb]) {
                    dp_cur[nb] = val;
                    row[nb] = k;
                }
            }
        }
        reachable = max_spend;
        {
            double *tmp = dp_prev;
            dp_prev = dp_cur;
            dp_cur = tmp;
        }
    }

    /* Minimum distortion over every feasible bit count, fewest bits
       first so an exact tie prefers the cheaper solution. */
    {
        int best_b = 0;
        for (b = 1; b <= reachable; b++) {
            if (dp_prev[b] < dp_prev[best_b])
                best_b = b;
        }
        b = best_b;
        for (i = nunits - 1; i >= 0; i--) {
            int k = take[(size_t) i * (size_t) width + b];
            best_choice[i] = k;
            b -= units[i].cand[k].nbits;
        }
    }

    free(dp_prev);
    free(dp_cur);
    free(take);
    return 0;
}


/* Write the sample codewords chosen by the solver, mirroring the
   framing of twolame_write_samples() but using the RD quantization
   rule's own codes instead of the legacy quantizer. */
static void rd_write_samples(twolame_options * glopts, bit_stream * bs)
{
    int nch = glopts->num_channels_out;
    int sblimit = glopts->sblimit;
    int sb, ch, gr, j;

    for (gr = 0; gr < 3; gr++)
        for (j = 0; j < SCALE_BLOCK; j += 3)
            for (sb = 0; sb < sblimit; sb++)
                for (ch = 0; ch < nch; ch++) {
                    int ba = (int) glopts->bit_alloc[ch][sb];
                    int stepidx, L;
                    if (ba == 0)
                        continue;
                    stepidx = twolame_rd_step_index_for(glopts, sb, ba);
                    if (twolame_rd_group(stepidx) == 3) {
                        /* One codeword per sample */
                        int x;
                        for (x = 0; x < 3; x++)
                            buffer_putbits(bs,
                                           (*glopts->subband)[ch][gr][j + x][sb],
                                           twolame_rd_bits(stepidx));
                    } else {
                        /* Grouped 3/5/9 levels: one codeword per triplet,
                           V = q0 + L*q1 + L^2*q2 */
                        L = twolame_rd_steps(stepidx);
                        unsigned int temp =
                            (*glopts->subband)[ch][gr][j][sb]
                            + (*glopts->subband)[ch][gr][j + 1][sb] * (unsigned int) L
                            + (*glopts->subband)[ch][gr][j + 2][sb]
                            * (unsigned int) L * (unsigned int) L;
                        buffer_putbits(bs, temp, twolame_rd_bits(stepidx));
                    }
                }
}


int twolame_rd_bit_allocation(twolame_options * glopts, int *adb)
{
    int nch, sblimit, sb, ch, i;
    int nunits;
    rd_unit *units;
    int *best_choice;
    int nbal_total = 0;
    int syntax;
    int budget;
    long used = 0;

    if (glopts == NULL || adb == NULL || glopts->sb_sample == NULL ||
        glopts->subband == NULL)
        return -1;

    nch = glopts->num_channels_out;
    sblimit = glopts->sblimit;
    nunits = nch * sblimit;

    units = (rd_unit *) calloc((size_t) nunits, sizeof(rd_unit));
    best_choice = (int *) calloc((size_t) nunits, sizeof(int));
    if (units == NULL || best_choice == NULL) {
        free(units);
        free(best_choice);
        return -1;
    }

    /* Build the units in canonical (subband, channel) order. Keeping a
       fixed order makes the double accumulation order deterministic, so
       exact-cost ties resolve identically across independent searches
       and the fewer-bit tie rule is observable. */
    i = 0;
    for (sb = 0; sb < sblimit; sb++)
        for (ch = 0; ch < nch; ch++) {
            rd_build_unit(glopts, ch, sb, &units[i]);
            nbal_total += twolame_rd_nbal_for(glopts, sb);
            i++;
        }

    /* Fixed budget: no-padding frame bits, ancillary already removed by
       the caller, minus header, optional CRC and every nbal field. */
    syntax = 32 + (glopts->error_protection ? 16 : 0) + nbal_total;
    budget = *adb - syntax;
    if (budget < 0) {
        free(units);
        free(best_choice);
        return -1;
    }

    /* Exact bounded integer-budget solve with backtracking. */
    if (rd_solve(units, nunits, budget, best_choice) != 0) {
        free(units);
        free(best_choice);
        return -1;
    }

    /* Commit the winning selection onto the encoder state. */
    memset((char *) glopts->bit_alloc, 0, sizeof(glopts->bit_alloc));
    memset((char *) glopts->scfsi, 0, sizeof(glopts->scfsi));
    memset((char *) glopts->subband, 0, sizeof(subband_t));
    for (i = 0; i < nunits; i++) {
        rd_unit *u = &units[i];
        rd_candidate *c = &u->cand[best_choice[i]];
        int gr, j;

        glopts->bit_alloc[u->ch][u->sb] = (unsigned int) c->ba;
        glopts->scalar[u->ch][0][u->sb] = c->sf[0];
        glopts->scalar[u->ch][1][u->sb] = c->sf[1];
        glopts->scalar[u->ch][2][u->sb] = c->sf[2];
        if (c->ba) {
            glopts->scfsi[u->ch][u->sb] = (unsigned int) c->scfsi;
            for (gr = 0; gr < 3; gr++)
                for (j = 0; j < SCALE_BLOCK; j++)
                    (*glopts->subband)[u->ch][gr][j][u->sb] =
                        (unsigned int) c->code[gr][j];
        }
        used += (long) c->nbits;
    }

    /* Remaining bits are written as zero padding by the caller. */
    *adb = budget - (int) used;

    free(units);
    free(best_choice);
    return 0;
}


/* Encode the RD sample codes onto the bitstream. Called from
   encode_frame() instead of the legacy quantization/writing pair. */
void twolame_rd_write_samples(twolame_options * glopts, bit_stream * bs)
{
    rd_write_samples(glopts, bs);
}


#ifdef TWOLAME_RD_TESTING
/* Read-only observation points for the target tests. */
sb_sample_t *twolame_rd_test_get_samples(twolame_options * glopts)
{
    return glopts->sb_sample;
}

subband_t *twolame_rd_test_get_subband(twolame_options * glopts)
{
    return glopts->subband;
}

unsigned int (*twolame_rd_test_get_scalar(twolame_options * glopts))[3][SBLIMIT]
{
    return glopts->scalar;
}

unsigned int (*twolame_rd_test_get_bit_alloc(twolame_options * glopts))[SBLIMIT]
{
    return glopts->bit_alloc;
}

unsigned int (*twolame_rd_test_get_scfsi(twolame_options * glopts))[SBLIMIT]
{
    return glopts->scfsi;
}
#endif
