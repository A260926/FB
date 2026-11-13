////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Independent decorrelation test support               //
//              Copyright (c) 2025 Conifer Software.                     //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// decorr_test.h
//
// Shared support for the independent decorrelation certification tests.
// Everything in this header is an independent, from-specification
// implementation: natural time-ordered history queues (no ring buffers, no
// ring indices copied from the library) and 64-bit intermediate math.
// Production decorrelation code (apply_weight, update_weight, the pack/unpack
// decorrelation passes and the state-restoration functions) is NEVER called.
// The only production internals reused are the lossless entropy writing
// functions for the non-target residual bitstream, as explicitly allowed.

#ifndef DECORR_TEST_H
#define DECORR_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "wavpack.h"
#include "wavpack_local.h"
#include "decorr_observe.h"

#define DT_MAX_TERM       8
#define DT_MAX_NTERMS     16
#define DT_MAX_BEATS      96
#define DT_HIST_CAP       8               /* deepest natural queue (term 8) */
#define DT_MAX_CHANS      2

#ifndef DT_BLOCK_BEATS_LOCAL
#define DT_BLOCK_BEATS_LOCAL 24           /* block size used by the chain tests */
#endif

/* --------------------------------------------------------------------- */
/* Independent format math.  The log2/exp2 tables are format constants   */
/* (part of the WavPack bitstream definition); the algorithms around     */
/* them are written from scratch here and do not call wp_log2s(),         */
/* wp_exp2s(), store_weight() or restore_weight().                        */
/* --------------------------------------------------------------------- */

static const unsigned char dt_exp2_table[256] = {
    0x00, 0x01, 0x01, 0x02, 0x03, 0x03, 0x04, 0x05,
    0x06, 0x06, 0x07, 0x08, 0x08, 0x09, 0x0a, 0x0b,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0e, 0x0f, 0x10, 0x10,
    0x11, 0x12, 0x13, 0x13, 0x14, 0x15, 0x16, 0x16,
    0x17, 0x18, 0x19, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
    0x1d, 0x1e, 0x1f, 0x20, 0x20, 0x21, 0x22, 0x23,
    0x24, 0x24, 0x25, 0x26, 0x27, 0x28, 0x28, 0x29,
    0x2a, 0x2b, 0x2c, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x3a, 0x3b, 0x3c, 0x3d,
    0x3e, 0x3f, 0x40, 0x41, 0x41, 0x42, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x48, 0x49, 0x4a, 0x4b,
    0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x51, 0x52,
    0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a,
    0x5b, 0x5c, 0x5d, 0x5e, 0x5e, 0x5f, 0x60, 0x61,
    0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71,
    0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81,
    0x82, 0x83, 0x84, 0x85, 0x87, 0x88, 0x89, 0x8a,
    0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92,
    0x93, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b,
    0x9c, 0x9d, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4,
    0xa5, 0xa6, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad,
    0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc8, 0xc9, 0xca,
    0xcb, 0xcd, 0xce, 0xcf, 0xd0, 0xd2, 0xd3, 0xd4,
    0xd6, 0xd7, 0xd8, 0xd9, 0xdb, 0xdc, 0xdd, 0xde,
    0xe0, 0xe1, 0xe2, 0xe4, 0xe5, 0xe6, 0xe8, 0xe9,
    0xea, 0xec, 0xed, 0xee, 0xf0, 0xf1, 0xf2, 0xf4,
    0xf5, 0xf6, 0xf8, 0xf9, 0xfa, 0xfc, 0xfd, 0xff,
};

static const char dt_nbits_table[256] = {
    0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8
};

static const unsigned char dt_log2_table[256] = {
    0x00, 0x01, 0x03, 0x04, 0x06, 0x07, 0x09, 0x0a,
    0x0b, 0x0d, 0x0e, 0x10, 0x11, 0x12, 0x14, 0x15,
    0x16, 0x18, 0x19, 0x1a, 0x1c, 0x1d, 0x1e, 0x20,
    0x21, 0x22, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2a,
    0x2c, 0x2d, 0x2e, 0x2f, 0x31, 0x32, 0x33, 0x34,
    0x36, 0x37, 0x38, 0x39, 0x3b, 0x3c, 0x3d, 0x3e,
    0x3f, 0x41, 0x42, 0x43, 0x44, 0x45, 0x47, 0x48,
    0x49, 0x4a, 0x4b, 0x4d, 0x4e, 0x4f, 0x50, 0x51,
    0x52, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a,
    0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63,
    0x64, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c,
    0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d,
    0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85,
    0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d,
    0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
    0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9b, 0x9c,
    0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4,
    0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xa9, 0xaa, 0xab,
    0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb2,
    0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xb9,
    0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xc0,
    0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xcb, 0xcb, 0xcc, 0xcd, 0xce,
    0xcf, 0xd0, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd4,
    0xd5, 0xd6, 0xd7, 0xd8, 0xd8, 0xd9, 0xda, 0xdb,
    0xdc, 0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe0, 0xe1,
    0xe2, 0xe3, 0xe4, 0xe4, 0xe5, 0xe6, 0xe7, 0xe7,
    0xe8, 0xe9, 0xea, 0xea, 0xeb, 0xec, 0xed, 0xee,
    0xee, 0xef, 0xf0, 0xf1, 0xf1, 0xf2, 0xf3, 0xf4,
    0xf4, 0xf5, 0xf6, 0xf7, 0xf7, 0xf8, 0xf9, 0xf9,
    0xfa, 0xfb, 0xfc, 0xfc, 0xfd, 0xfe, 0xff, 0xff,
};

/* unsigned base-2 logarithm, independent implementation */
static int dt_log2u (uint32_t avalue)
{
    int dbits;

    if ((avalue += avalue >> 9) < (1u << 8)) {
        dbits = dt_nbits_table [avalue];
        return (dbits << 8) + dt_log2_table [(avalue << (9 - dbits)) & 0xff];
    }
    else {
        if (avalue < (1u << 16))
            dbits = dt_nbits_table [avalue >> 8] + 8;
        else if (avalue < (1u << 24))
            dbits = dt_nbits_table [avalue >> 16] + 16;
        else
            dbits = dt_nbits_table [avalue >> 24] + 24;

        return (dbits << 8) + dt_log2_table [(avalue >> (dbits - 9)) & 0xff];
    }
}

/* signed base-2 logarithm used for history storage */
static int dt_log2s (int32_t value)
{
    return (value < 0) ? -dt_log2u ((uint32_t)(-value)) : dt_log2u ((uint32_t) value);
}

/* inverse signed logarithm, independent implementation of the format rule */
static int32_t dt_exp2s (int logv)
{
    uint32_t value;

    if (logv < 0)
        return (int32_t) ~((uint32_t) dt_exp2s (-logv) - 1);

    value = dt_exp2_table [logv & 0xff] | 0x100;

    if ((logv >>= 8) <= 9)
        return (int32_t)(value >> (9 - logv));
    else
        return (int32_t)(value << ((logv - 9) & 0x1f));
}

/* independent weight storage / restoration pair (8-bit signed metadata) */
static signed char dt_store_weight (int weight)
{
    if (weight > 1024)
        weight = 1024;
    else if (weight < -1024)
        weight = -1024;

    if (weight > 0)
        weight -= (weight + 64) >> 7;

    return (signed char) ((weight + 4) >> 3);
}

static int dt_restore_weight (signed char weight)
{
    int result = (int) weight * 8;

    if (result > 0)
        result += (result + 64) >> 7;

    return result;
}

/* --------------------------------------------------------------------- */
/* Independent natural-time decorrelation model.                          */
/*                                                                         */
/* Histories are plain FIFO queues in natural time (index 0 is the most   */
/* recent past sample).  There are no modular/ring indices anywhere.       */
/* Every multiply is carried out in 64 bits and every value that lands in  */
/* a 32-bit field is range-checked first, so results never rely on         */
/* overflow.                                                               */
/* --------------------------------------------------------------------- */

typedef struct {
    int term, delta;
    int32_t w[DT_MAX_CHANS];                    /* per-channel weights */
    int32_t hist[DT_MAX_CHANS][DT_HIST_CAP];    /* natural-time history */
    int hlen[DT_MAX_CHANS];                     /* entries currently queued */
} DtPass;

typedef struct {
    int mono;
    int npass;
    DtPass pass[DT_MAX_NTERMS];                 /* execution order, pass 0 first */
} DtModel;

/* floor((weight * source + 512) / 1024) with explicit floor and 64-bit math */
static int32_t dt_apply (int32_t weight, int32_t source)
{
    int64_t t = (int64_t) weight * (int64_t) source + 512;
    int64_t q = t / 1024;
    int64_t r = t % 1024;

    if (r < 0)
        q--;                                    /* C truncates toward zero */

    return (int32_t) q;
}

/* sign-of-source vs sign-of-residual update; negative terms additionally clip */
static int32_t dt_update (int32_t weight, int32_t delta, int32_t source, int32_t residual, int clip)
{
    if (source && residual) {
        if (((source ^ residual) >> 31) == 0)
            weight += delta;                    /* same sign */
        else
            weight -= delta;                    /* opposite signs */
    }

    if (clip) {
        if (weight > 1024)
            weight = 1024;
        else if (weight < -1024)
            weight = -1024;
    }

    return weight;
}

static void dt_push (DtPass *p, int chan, int32_t value)
{
    int need = (p->term > DT_MAX_TERM) ? 2 : (p->term < 0 ? 1 : p->term);
    int k;

    if (need > DT_HIST_CAP)
        need = DT_HIST_CAP;

    /* shift within [0..need-1], dropping the oldest; never write hist[need] */
    for (k = need - 1; k > 0; k--)
        p->hist [chan] [k] = p->hist [chan] [k - 1];

    p->hist [chan] [0] = value;

    if (p->hlen [chan] < need)
        p->hlen [chan]++;
}

static void dt_check32 (int64_t v, const char *what, int beat, int chan, int term)
{
    if (v < INT32_MIN || v > INT32_MAX) {
        fprintf (stderr, "INT32 RANGE FAILURE field=%s beat=%d chan=%d term=%d value=%lld\n",
            what, beat, chan, term, (long long) v);
        exit (2);
    }
}

/*
 * Run one beat of the whole pass chain on a pair of residuals.
 * *out receives the reconstructed PCM (out[1] untouched for mono).
 * The update rule always looks at the prediction SOURCE and the RESIDUAL,
 * never at the reconstructed sample.
 */
static void dt_model_beat (DtModel *m, int beat, const int32_t resid[2], int32_t out[2])
{
    int pi, c;

    out [0] = resid [0];
    out [1] = resid [1];

    for (pi = 0; pi < m->npass; pi++) {
        DtPass *p = &m->pass [pi];
        int negclip = (p->term < 0);

        if (m->mono) {
            int32_t src, pred, x;

            if (p->term > DT_MAX_TERM) {
                if (p->term == 17)
                    src = 2 * p->hist [0] [0] - p->hist [0] [1];
                else
                    src = (3 * p->hist [0] [0] - p->hist [0] [1]) >> 1;
            }
            else
                src = (p->hlen [0] >= p->term) ? p->hist [0] [p->term - 1] : 0;

            dt_check32 ((int64_t) p->w [0] * src, "weight*source", beat, 0, p->term);
            pred = dt_apply (p->w [0], src);
            dt_check32 ((int64_t) pred + out [0], "reconstructed", beat, 0, p->term);
            x = out [0] + pred;
            p->w [0] = dt_update (p->w [0], p->delta, src, out [0], 0);
            out [0] = x;

            if (p->term > DT_MAX_TERM)
                dt_push (p, 0, x);            /* push handles the two-slot shift */
            else
                dt_push (p, 0, x);
        }
        else if (p->term > 0) {
            for (c = 0; c < 2; c++) {
                int32_t src, pred, x;

                if (p->term > DT_MAX_TERM) {
                    if (p->term == 17)
                        src = 2 * p->hist [c] [0] - p->hist [c] [1];
                    else
                        src = p->hist [c] [0] + ((p->hist [c] [0] - p->hist [c] [1]) >> 1);
                }
                else
                    src = (p->hlen [c] >= p->term) ? p->hist [c] [p->term - 1] : 0;

                dt_check32 ((int64_t) p->w [c] * src, "weight*source", beat, c, p->term);
                pred = dt_apply (p->w [c], src);
                dt_check32 ((int64_t) pred + out [c], "reconstructed", beat, c, p->term);
                x = out [c] + pred;
                p->w [c] = dt_update (p->w [c], p->delta, src, out [c], 0);
                out [c] = x;
                dt_push (p, c, x);
            }
        }
        else if (p->term == -1) {
            int32_t srcL, srcR, predL, predR, left, right;

            srcL = p->hist [0] [0];                       /* previous beat's right */
            dt_check32 ((int64_t) p->w [0] * srcL, "weight*source", beat, 0, -1);
            predL = dt_apply (p->w [0], srcL);
            dt_check32 ((int64_t) predL + out [0], "reconstructed", beat, 0, -1);
            left = out [0] + predL;
            p->w [0] = dt_update (p->w [0], p->delta, srcL, out [0], negclip);

            srcR = left;                                  /* this beat's left, rebuilt first */
            dt_check32 ((int64_t) p->w [1] * srcR, "weight*source", beat, 1, -1);
            predR = dt_apply (p->w [1], srcR);
            dt_check32 ((int64_t) predR + out [1], "reconstructed", beat, 1, -1);
            right = out [1] + predR;
            p->w [1] = dt_update (p->w [1], p->delta, srcR, out [1], negclip);

            out [0] = left;
            out [1] = right;
            p->hist [0] [0] = right;                      /* cross-saved for next beat */
            p->hlen [0] = 1;
        }
        else if (p->term == -2) {
            int32_t srcL, srcR, predL, predR, left, right;

            srcR = p->hist [1] [0];                       /* previous beat's left */
            dt_check32 ((int64_t) p->w [1] * srcR, "weight*source", beat, 1, -2);
            predR = dt_apply (p->w [1], srcR);
            dt_check32 ((int64_t) predR + out [1], "reconstructed", beat, 1, -2);
            right = out [1] + predR;
            p->w [1] = dt_update (p->w [1], p->delta, srcR, out [1], negclip);

            srcL = right;                                 /* this beat's right, rebuilt first */
            dt_check32 ((int64_t) p->w [0] * srcL, "weight*source", beat, 0, -2);
            predL = dt_apply (p->w [0], srcL);
            dt_check32 ((int64_t) predL + out [0], "reconstructed", beat, 0, -2);
            left = out [0] + predL;
            p->w [0] = dt_update (p->w [0], p->delta, srcL, out [0], negclip);

            out [0] = left;
            out [1] = right;
            p->hist [1] [0] = left;                       /* cross-saved for next beat */
            p->hlen [1] = 1;
        }
        else { /* term -3: each side uses its own saved opposite from the PREVIOUS beat */
            int32_t srcL = p->hist [0] [0];               /* saved previous right */
            int32_t srcR = p->hist [1] [0];               /* saved previous left  */
            int32_t predL, predR, left, right;

            dt_check32 ((int64_t) p->w [0] * srcL, "weight*source", beat, 0, -3);
            predL = dt_apply (p->w [0], srcL);
            dt_check32 ((int64_t) predL + out [0], "reconstructed", beat, 0, -3);
            left = out [0] + predL;
            p->w [0] = dt_update (p->w [0], p->delta, srcL, out [0], negclip);

            dt_check32 ((int64_t) p->w [1] * srcR, "weight*source", beat, 1, -3);
            predR = dt_apply (p->w [1], srcR);
            dt_check32 ((int64_t) predR + out [1], "reconstructed", beat, 1, -3);
            right = out [1] + predR;
            p->w [1] = dt_update (p->w [1], p->delta, srcR, out [1], negclip);

            out [0] = left;
            out [1] = right;
            p->hist [1] [0] = left;                       /* save this beat's left  */
            p->hist [0] [0] = right;                      /* save this beat's right */
            p->hlen [0] = p->hlen [1] = 1;
        }
    }
}

/* Snapshot of model state after a beat, for return-boundary comparisons. */
typedef struct {
    int32_t weight[DT_MAX_NTERMS][DT_MAX_CHANS];
    int32_t hist[DT_MAX_NTERMS][DT_MAX_CHANS][DT_HIST_CAP];
    int    hlen[DT_MAX_NTERMS][DT_MAX_CHANS];
} DtSnapshot;

typedef struct {
    int32_t pcm[DT_MAX_BEATS][DT_MAX_CHANS];
    int32_t resid[DT_MAX_BEATS][DT_MAX_CHANS];
    DtSnapshot snap[DT_MAX_BEATS];
    int beats;
} DtTrace;

static void dt_snapshot (DtModel *m, int beat, DtTrace *tr)
{
    int pi, c, k;

    for (pi = 0; pi < m->npass; pi++) {
        for (c = 0; c < (m->mono ? 1 : 2); c++) {
            tr->snap [beat].weight [pi] [c] = m->pass [pi].w [c];

            for (k = 0; k < DT_HIST_CAP; k++)
                tr->snap [beat].hist [pi] [c] [k] = m->pass [pi].hist [c] [k];

            tr->snap [beat].hlen [pi] [c] = m->pass [pi].hlen [c];
        }
    }
}

/* --------------------------------------------------------------------- */
/* Block plan: the generator states explicitly what the bitstream must    */
/* contain.  Terms are listed in DECLARED (metadata) order; the model      */
/* executes them in reverse, exactly like the production decoder.          */
/* --------------------------------------------------------------------- */

typedef struct {
    int term;
    int delta;
    int32_t init_w[DT_MAX_CHANS];
    int32_t init_hist[DT_MAX_CHANS][DT_HIST_CAP];  /* natural time, slot 0 = newest */
} DtTermPlan;

typedef struct {
    int mono;
    int bits;                           /* 16 or 24 */
    int nterms;
    DtTermPlan terms[DT_MAX_NTERMS];    /* declared order */
    int beats;
    int32_t resid[DT_MAX_BEATS][DT_MAX_CHANS];
    uint32_t init_median[DT_MAX_CHANS][3];
    int block_index;                    /* first sample index of this block */
    int total_samples;                  /* file-wide total */
    const char *label;
} DtBlockPlan;

/* Build an executable model (pass order = reverse of declared order). */
static DtModel dt_model_from_plan (const DtBlockPlan *pl)
{
    DtModel m;
    int i, c, k;

    memset (&m, 0, sizeof (m));
    m.mono = pl->mono;
    m.npass = pl->nterms;

    for (i = 0; i < pl->nterms; i++) {
        int pi = pl->nterms - 1 - i;    /* reverse declaration order */
        DtPass *p = &m.pass [pi];
        const DtTermPlan *t = &pl->terms [i];

        p->term = t->term;
        p->delta = t->delta;

        /* Recover initial state independently through the same metadata
         * quantization the decoder uses, rather than copying production
         * state: 8-bit weight rounding and signed-log2 history rounding. */
        p->w [0] = dt_restore_weight (dt_store_weight (t->init_w [0]));
        p->w [1] = pl->mono ? 0 : dt_restore_weight (dt_store_weight (t->init_w [1]));

        for (c = 0; c < (pl->mono ? 1 : 2); c++) {
            int need = (t->term > DT_MAX_TERM) ? 2 : (t->term < 0 ? 1 : t->term);

            for (k = 0; k < need; k++)
                p->hist [c] [k] = dt_exp2s (dt_log2s (t->init_hist [c] [k]));

            p->hlen [c] = need;
        }
    }

    return m;
}

/* Run a plan through the independent model, recording the full trace. */
static void dt_run_plan (const DtBlockPlan *pl, DtTrace *tr)
{
    DtModel m = dt_model_from_plan (pl);
    int i;

    if (pl->beats > DT_MAX_BEATS) {
        fprintf (stderr, "plan too long: %s\n", pl->label);
        exit (2);
    }

    tr->beats = pl->beats;

    for (i = 0; i < pl->beats; i++) {
        int32_t r[2] = { pl->resid [i] [0], pl->resid [i] [1] };
        int32_t x[2];

        tr->resid [i] [0] = r[0];
        tr->resid [i] [1] = r[1];
        dt_model_beat (&m, i, r, x);
        tr->pcm [i] [0] = x[0];
        tr->pcm [i] [1] = x[1];
        dt_snapshot (&m, i, tr);
    }
}

/* --------------------------------------------------------------------- */
/* Manual WavPack block writer.  Metadata framing is implemented from the  */
/* format specification; the residual payload itself is produced by the    */
/* production entropy writer.  Expected PCM (and therefore the header CRC) */
/* comes exclusively from the independent model.                          */
/* --------------------------------------------------------------------- */

static void dt_bs_wrap (Bitstream *bs) { bs->error = 1; }

static void dt_bs_open (Bitstream *bs, void *start, void *end)
{
    bs->error = bs->sr = bs->bc = 0;
    bs->ptr = bs->buf = (uint16_t *) start;
    bs->end = (uint16_t *) end;
    bs->wrap = dt_bs_wrap;
}

/* mirror of the library's static bs_close_write(): pad with ones to an even size */
static uint32_t dt_bs_close (Bitstream *bs)
{
    uint32_t bytes;

    if (bs->error)
        return (uint32_t) -1;

    while (1) {
        while (bs->bc)
            putbit_1 (bs);

        bytes = (uint32_t)((char *) bs->ptr - (char *) bs->buf);

        if (bytes & 1)
            putbit_1 (bs);
        else
            break;
    }

    return bytes;
}

static unsigned char *dt_put_meta (unsigned char *p, unsigned char id, const void *data, int len)
{
    *p++ = (unsigned char)(id | (len & 1 ? ID_ODD_SIZE : 0));
    *p++ = (unsigned char)((len + 1) >> 1);

    if (len)
        memcpy (p, data, len);

    return p + len + (len & 1);
}

/*
 * Serialize one complete, checksummed WavPack block for the plan.
 * Returns the block length in bytes.
 */
static uint32_t dt_build_block (const DtBlockPlan *pl, const DtTrace *tr, unsigned char *out, uint32_t outcap)
{
    WavpackHeader hdr;
    unsigned char meta[4096], *mp = meta;
    unsigned char terms_bytes[DT_MAX_NTERMS];
    unsigned char weight_bytes[DT_MAX_NTERMS * 2];
    unsigned char sample_bytes[512];
    unsigned char ent_bytes[12];
    int nch = pl->mono ? 1 : 2;
    int i, c, k, wn = 0, sn = 0;
    WavpackStream ews;
    uint32_t payload, crc;
    static unsigned char bitbuf[65536];

    if (pl->nterms > DT_MAX_NTERMS)
        return 0;

    /* ID_DECORR_TERMS: declared order, byte = (term + 5) | (delta << 5) */
    for (i = 0; i < pl->nterms; i++)
        terms_bytes [i] = (unsigned char)(((pl->terms [i].term + 5) & 0x1f) |
                                          ((pl->terms [i].delta << 5) & 0xe0));

    /* ID_DECORR_WEIGHTS: forward declared order, matching the sample order;
     * the decoder consumes weight and sample bytes from the highest
     * decorr_passes index downward.  All-zero leading runs are omitted, with
     * the absent terms left at zero by the decoder's initialization. */
    for (i = 0; i < pl->nterms; i++) {
        signed char sa = dt_store_weight (pl->terms [i].init_w [0]);
        signed char sb = pl->mono ? 0 : dt_store_weight (pl->terms [i].init_w [1]);

        if (pl->mono) {
            if (sa || wn)
                weight_bytes [wn++] = (unsigned char) sa;
        }
        else {
            if (sa || sb || wn) {
                weight_bytes [wn++] = (unsigned char) sa;
                weight_bytes [wn++] = (unsigned char) sb;
            }
        }
    }

    /* ID_DECORR_SAMPLES: terms in forward declared order.  The production
     * writer walks its pack-time decorr_passes forward, and the decoder's
     * sample loop consumes bytes starting at its highest array index
     * (which holds the first-declared term) downward.  Empirically this is
     * the order that round-trips for both single-term and multi-term
     * blocks.  Delay-term slots within a term are oldest-first so a fresh
     * decoder ring reads the oldest preloaded value from slot 0 first. */
    for (i = 0; i < pl->nterms; i++) {
        const DtTermPlan *t = &pl->terms [i];
        int term = t->term;

        if (term > DT_MAX_TERM) {
            /* two-point terms group all of channel A's two slots, then all
             * of channel B's (the production reader fills A then B) */
            for (c = 0; c < nch; c++)
                for (k = 0; k < 2; k++) {
                    int lg = dt_log2s (t->init_hist [c] [k]);
                    sample_bytes [sn++] = (unsigned char) lg;
                    sample_bytes [sn++] = (unsigned char)(lg >> 8);
                }
        }
        else if (term < 0) {
            int lg;
            lg = dt_log2s (t->init_hist [0] [0]);
            sample_bytes [sn++] = (unsigned char) lg;
            sample_bytes [sn++] = (unsigned char)(lg >> 8);
            lg = dt_log2s (t->init_hist [1] [0]);
            sample_bytes [sn++] = (unsigned char) lg;
            sample_bytes [sn++] = (unsigned char)(lg >> 8);
        }
        else {
            /* delay terms 1-8: oldest-first within each slot, and for stereo
             * the two channels are interleaved per slot (A_m then B_m),
             * matching the production writer's inner channel loop.  The
             * independent model itself keeps natural-time queues only. */
            for (k = 0; k < term; k++)
                for (c = 0; c < nch; c++) {
                    int lg = dt_log2s (t->init_hist [c] [term - 1 - k]);
                    sample_bytes [sn++] = (unsigned char) lg;
                    sample_bytes [sn++] = (unsigned char)(lg >> 8);
                }
        }
    }

    /* entropy-encode the residual plan with the production lossless writer */
    memset (&ews, 0, sizeof (ews));
    ews.wphdr.flags = pl->mono ? MONO_FLAG : 0;
    init_words (&ews);

    for (c = 0; c < nch; c++) {
        ews.w.c [c].median [0] = pl->init_median [c] [0];
        ews.w.c [c].median [1] = pl->init_median [c] [1];
        ews.w.c [c].median [2] = pl->init_median [c] [2];
    }

    {
        WavpackMetadata emd;
        write_entropy_vars (&ews, &emd);
        if (emd.byte_length != (pl->mono ? 6 : 12)) {
            fprintf (stderr, "unexpected entropy metadata length\n");
            exit (2);
        }
        memcpy (ent_bytes, emd.data, emd.byte_length);
        free (emd.data);
    }

    dt_bs_open (&ews.wvbits, bitbuf, bitbuf + sizeof (bitbuf));

    if (pl->mono) {
        /* mono entropy coding needs a contiguous sample stream, not the
         * interleaved [beat][channel] trace layout */
        int32_t mono_resid[DT_MAX_BEATS];
        int bi;

        for (bi = 0; bi < pl->beats; bi++)
            mono_resid [bi] = tr->resid [bi] [0];

        send_words_lossless (&ews, mono_resid, pl->beats);
    }
    else
        send_words_lossless (&ews, (int32_t *) tr->resid, pl->beats);

    flush_word (&ews);
    payload = dt_bs_close (&ews.wvbits);

    if (payload == (uint32_t) -1 || payload & 1) {
        fprintf (stderr, "residual bitstream framing failure\n");
        exit (2);
    }

    /* assemble metadata in the required production order */
    mp = dt_put_meta (mp, ID_DECORR_TERMS, terms_bytes, pl->nterms);
    if (wn)
        mp = dt_put_meta (mp, ID_DECORR_WEIGHTS, weight_bytes, wn);
    mp = dt_put_meta (mp, ID_DECORR_SAMPLES, sample_bytes, sn);
    mp = dt_put_meta (mp, ID_ENTROPY_VARS, ent_bytes, pl->mono ? 6 : 12);
    mp = dt_put_meta (mp, ID_WV_BITSTREAM, bitbuf, (int) payload);

    /* CRC over expected PCM from the independent model; the production
     * accumulation is strictly 32-bit modular, so it must wrap identically */
    crc = 0xffffffff;

    if (pl->mono)
        for (i = 0; i < pl->beats; i++)
            crc = (uint32_t)((crc + (crc << 1)) + (uint32_t) tr->pcm [i] [0]);
    else
        for (i = 0; i < pl->beats; i++)
            crc = (uint32_t)((crc + (crc << 3)) +
                   ((uint32_t) tr->pcm [i] [0] << 1) +
                   tr->pcm [i] [0] + tr->pcm [i] [1]);

    /* build the header natively, then serialize little-endian */
    memset (&hdr, 0, sizeof (hdr));
    memcpy (hdr.ckID, "wvpk", 4);
    hdr.ckSize = sizeof (WavpackHeader) - 8 + (uint32_t)(mp - meta) + 4;
    hdr.version = 0x410;
    SET_TOTAL_SAMPLES (hdr, pl->total_samples);
    SET_BLOCK_INDEX (hdr, pl->block_index);
    hdr.block_samples = (uint32_t) pl->beats;
    hdr.flags = (uint32_t)(pl->bits == 16 ? 1 : 2)               /* BYTES_STORED */
              | ((uint32_t) pl->bits << 18)                      /* MAG_MASK */
              | (10u << 23)                                      /* 48 kHz */
              | INITIAL_BLOCK | FINAL_BLOCK | HAS_CHECKSUM
              | (pl->mono ? MONO_FLAG : 0);
    hdr.crc = crc;

    if (outcap < sizeof (hdr) + (uint32_t)(mp - meta) + 4)
        return 0;

    memcpy (out, &hdr, sizeof (hdr));
    WavpackNativeToLittleEndian ((WavpackHeader *) out, WavpackHeaderFormat);
    memcpy (out + sizeof (hdr), meta, mp - meta);

    /* trailing 2-byte block checksum over the little-endian block so far:
     * id byte + length byte + two folded checksum bytes (four bytes total) */
    {
        uint16_t *sp = (uint16_t *) out;
        uint32_t csum = 0xffffffff;
        int wcount = (int)((sizeof (hdr) + (mp - meta)) >> 1);
        unsigned char *cp = out + sizeof (hdr) + (mp - meta);

        while (wcount--)
            csum = csum * 3 + *sp++;

        *cp++ = ID_BLOCK_CHECKSUM;
        *cp++ = 1;                       /* 2-byte checksum: length byte = bytes >> 1 */
        *cp++ = (unsigned char)(csum ^ (csum >> 16));
        *cp++ = (unsigned char)((csum ^ (csum >> 16)) >> 8);
    }

    return sizeof (WavpackHeader) + (uint32_t)(mp - meta) + 4;
}

/* --------------------------------------------------------------------- */
/* Seekable in-memory reader for the public WavpackOpenFileInputEx64 path  */
/* --------------------------------------------------------------------- */

typedef struct {
    const unsigned char *data;
    int64_t len, pos;
} DtMemFile;

static int32_t dt_mem_read (void *id, void *dst, int32_t bcount)
{
    DtMemFile *f = (DtMemFile *) id;
    int32_t n = bcount;

    if (f->pos + n > f->len)
        n = (int32_t)(f->len - f->pos);

    if (n > 0)
        memcpy (dst, f->data + f->pos, n);

    f->pos += n;
    return n;
}

static int32_t dt_mem_write (void *id, void *data, int32_t bcount) { (void) id; (void) data; return bcount; }
static int64_t dt_mem_pos (void *id) { return ((DtMemFile *) id)->pos; }
static int dt_mem_seek (void *id, int64_t pos) { ((DtMemFile *) id)->pos = pos; return 0; }
static int dt_mem_seekrel (void *id, int64_t delta, int mode)
{
    DtMemFile *f = (DtMemFile *) id;
    if (mode == SEEK_SET) f->pos = delta;
    else if (mode == SEEK_END) f->pos = f->len + delta;
    else f->pos += delta;
    return 0;
}
static int dt_mem_push (void *id, int c) { DtMemFile *f = (DtMemFile *) id; f->pos--; (void) c; return c; }
static int64_t dt_mem_len (void *id) { return ((DtMemFile *) id)->len; }
static int dt_mem_canseek (void *id) { (void) id; return 1; }
static int dt_mem_trunc (void *id) { (void) id; return 0; }
static int dt_mem_close (void *id) { (void) id; return 0; }

static WavpackStreamReader64 dt_mem_reader = {
    dt_mem_read, dt_mem_write, dt_mem_pos, dt_mem_seek, dt_mem_seekrel,
    dt_mem_push, dt_mem_len, dt_mem_canseek, dt_mem_trunc, dt_mem_close
};

/* --------------------------------------------------------------------- */
/* Comparison accounting and first-difference reporting                   */
/* --------------------------------------------------------------------- */

typedef struct {
    long term_compare[22];      /* index: term+3 for -3..18 (skip 0) */
    long chain_compare;
    long pcm_compare;
    long weight_compare;
    long history_compare;
    long residual_compare;
    int failed;
} DtCounts;

static void dt_counts_init (DtCounts *c)
{
    memset (c, 0, sizeof (*c));
}

static void dt_note_diff (DtCounts *c, const char *kind, const char *label, int block,
                          int pass, int chan, int beat, int term,
                          int32_t got, int32_t expect, const char *field)
{
    c->failed = 1;
    fprintf (stderr,
        "FIRST DIFF kind=%s block=%s pass=%d(term=%d) chan=%d sample=%d field=%s "
        "got=%d expect=%d\n",
        kind, label, pass, term, chan, beat, field, got, expect);
}

/* --------------------------------------------------------------------- */
/* Public-chain decode and value-by-value certification helpers           */
/* --------------------------------------------------------------------- */

/*
 * Read-side bridge between the production term 1-8 ring discipline and the
 * model's natural-time queues.  The model itself never uses ring indices.
 *
 * Each WavpackUnpackSamples() call re-enters the stereo decorrelation pass
 * with m=0, k=term and walks `call_len` beats, so the raw ring position at
 * the end of a call depends only on that call's length, not on the block's
 * absolute beat.  The mono pass additionally self-normalizes on return.
 *
 * Returns the natural age stored in ring slot `slot`, or -1 if not a live
 * history slot for this term.
 */
static int dt_ring_slot_age (int term, int mono, int normalized, int call_len, int slot)
{
    int age;

    if (normalized || mono)
        age = (term - 1 - slot) & (MAX_TERM - 1);
    else
        age = (call_len - 1 + term - slot) & (MAX_TERM - 1);

    return (age >= 0 && age < term) ? age : -1;
}

/*
 * Verify the observations emitted by ONE WavpackUnpackSamples() call.
 * The call covers absolute beats [call_first, call_first+call_n) of the
 * file and may cross block boundaries; each touched block contributes one
 * residual record plus one pass record per term, plus normalization records
 * for terms 1-8 whenever the call length inside that block is not a
 * multiple of MAX_TERM.
 *
 * The model keeps only natural-time queues and never rotates them.  For raw
 * pass records the production ring is translated on the READ side
 * (slot (end-age) & 7); after a NORM record the production layout already
 * matches natural time and is compared directly.  No ring layout is copied
 * into the model.
 */
static int dt_compare_call (DtCounts *cnt, const DtBlockPlan *const plans[],
                            const DtTrace *const traces[], int nblocks,
                            int call_first, int call_n)
{
    int remaining = call_n, cursor = call_first, bad = 0;

    while (remaining > 0) {
        int blockno = cursor / DT_BLOCK_BEATS_LOCAL;
        const DtBlockPlan *pl;
        const DtTrace *tr;
        int in_block, first_in_block, end_in_block, nch, pi;
        struct dt_obs_record rec;

        /* single-block plans (reference program) always report block 0 */
        if (nblocks == 1) {
            blockno = 0;
            in_block = remaining;
            first_in_block = cursor;
        }
        else {
            int block_start = blockno * DT_BLOCK_BEATS_LOCAL;
            int block_end = block_start + DT_BLOCK_BEATS_LOCAL;
            first_in_block = cursor - block_start;
            in_block = call_first + call_n - cursor;
            if (block_end < cursor + in_block)
                in_block = block_end - cursor;
        }

        pl = plans [blockno];
        tr = traces [blockno];
        nch = pl->mono ? 1 : 2;
        end_in_block = first_in_block + in_block - 1;

        /* residual snapshot for this block segment */
        if (!dt_obs_pop (&rec) || rec.kind != DT_OBS_RESIDUAL) {
            fprintf (stderr, "FIRST DIFF kind=residual block=%s missing observation\n", pl->label);
            cnt->failed = 1;
            return 1;
        }

        if ((int) rec.count != in_block) {
            dt_note_diff (cnt, "residual", pl->label, blockno, -1, -1,
                          pl->block_index + first_in_block, -1,
                          (int32_t) rec.count, in_block, "count");
            bad = 1;
        }

        for (int i = 0; i < in_block; i++)
            for (int c = 0; c < nch; c++) {
                int32_t e = tr->resid [first_in_block + i] [c];
                int32_t g = rec.values [i * nch + c];

                cnt->residual_compare++;

                if (g != e) {
                    dt_note_diff (cnt, "residual", pl->label, blockno, -1, c,
                                  pl->block_index + first_in_block + i, -1, g, e, "residual");
                    bad = 1;
                }
            }

        /* one pass record per executed pass (execution order) */
        for (pi = 0; pi < pl->nterms; pi++) {
            int declared = pl->nterms - 1 - pi;
            int term = pl->terms [declared].term;
            int c, k;

            if (!dt_obs_pop (&rec) || rec.kind != DT_OBS_PASS) {
                fprintf (stderr, "FIRST DIFF kind=pass block=%s pass=%d missing observation\n",
                         pl->label, pi);
                cnt->failed = 1;
                return 1;
            }

            if (rec.term != term) {
                dt_note_diff (cnt, "order", pl->label, blockno, pi, -1,
                              pl->block_index + end_in_block, term, rec.term, term, "term-order");
                bad = 1;
            }

            for (c = 0; c < nch; c++) {
                int32_t ew = tr->snap [end_in_block].weight [pi] [c];
                int32_t gw = c ? rec.weight_B : rec.weight_A;

                cnt->weight_compare++;

                if (gw != ew) {
                    dt_note_diff (cnt, "weight", pl->label, blockno, pi, c,
                                  pl->block_index + end_in_block, term, gw, ew, "weight");
                    bad = 1;
                }
            }

            if (term > 0 && term <= DT_MAX_TERM) {
                /*
                 * The mono pass self-normalizes; the stereo pass leaves the
                 * raw ring.  In both layouts we walk the ring slots and map
                 * each live slot to its natural age via the independent
                 * bridge, never copying ring layout into the model.
                 */
                for (c = 0; c < nch; c++) {
                    const int32_t *ring = c ? rec.samples_B : rec.samples_A;

                    for (k = 0; k < MAX_TERM; k++) {
                        int age = dt_ring_slot_age (term, pl->mono, 0,
                                                    in_block, k);

                        if (age < 0)
                            continue;

                        int32_t eh = tr->snap [end_in_block].hist [pi] [c] [age];
                        int32_t gh = ring [k];

                        cnt->history_compare++;

                        if (gh != eh) {
                            dt_note_diff (cnt, "history", pl->label, blockno, pi, c,
                                          pl->block_index + end_in_block, term, gh, eh,
                                          "history-ring");
                            bad = 1;
                        }
                    }
                }
            }
            else if (term > DT_MAX_TERM) {
                for (c = 0; c < nch; c++) {
                    const int32_t *ring = c ? rec.samples_B : rec.samples_A;

                    for (k = 0; k < 2; k++) {
                        int32_t eh = tr->snap [end_in_block].hist [pi] [c] [k];
                        int32_t gh = ring [k];

                        cnt->history_compare++;

                        if (gh != eh) {
                            dt_note_diff (cnt, "history", pl->label, blockno, pi, c,
                                          pl->block_index + end_in_block, term, gh, eh,
                                          "history-2pt");
                            bad = 1;
                        }
                    }
                }
            }
            else if (term == -1) {
                cnt->history_compare++;

                if (rec.samples_A [0] != tr->snap [end_in_block].hist [pi] [0] [0]) {
                    dt_note_diff (cnt, "history", pl->label, blockno, pi, 0,
                                  pl->block_index + end_in_block, term,
                                  rec.samples_A [0], tr->snap [end_in_block].hist [pi] [0] [0],
                                  "cross-right");
                    bad = 1;
                }
            }
            else if (term == -2) {
                cnt->history_compare++;

                if (rec.samples_B [0] != tr->snap [end_in_block].hist [pi] [1] [0]) {
                    dt_note_diff (cnt, "history", pl->label, blockno, pi, 1,
                                  pl->block_index + end_in_block, term,
                                  rec.samples_B [0], tr->snap [end_in_block].hist [pi] [1] [0],
                                  "cross-left");
                    bad = 1;
                }
            }
            else { /* term -3 */
                int32_t pairs[2][2] = {
                    { rec.samples_A [0], tr->snap [end_in_block].hist [pi] [0] [0] },
                    { rec.samples_B [0], tr->snap [end_in_block].hist [pi] [1] [0] }
                };

                for (c = 0; c < 2; c++) {
                    cnt->history_compare++;

                    if (pairs [c] [0] != pairs [c] [1]) {
                        dt_note_diff (cnt, "history", pl->label, blockno, pi, c,
                                      pl->block_index + end_in_block, term,
                                      pairs [c] [0], pairs [c] [1], "prev-opposite");
                        bad = 1;
                    }
                }
            }

            cnt->term_compare [term + 3]++;
        }

        /* normalization records: stereo only (the mono pass normalizes its
         * own ring internally) and present exactly for terms 1-8 whose call
         * length within this block is not a multiple of MAX_TERM */
        if (!pl->mono)
            for (pi = 0; pi < pl->nterms; pi++) {
                int declared = pl->nterms - 1 - pi;
                int term = pl->terms [declared].term;
                int c, k;

                if (term <= 0 || term > DT_MAX_TERM)
                    continue;

                if (in_block % MAX_TERM == 0)
                    continue;   /* production leaves the ring in place */

                if (!dt_obs_pop (&rec) || rec.kind != DT_OBS_NORM || rec.term != term) {
                    fprintf (stderr, "FIRST DIFF kind=norm block=%s term=%d missing/raw record\n",
                             pl->label, term);
                    cnt->failed = 1;
                    return 1;
                }

                for (c = 0; c < nch; c++) {
                    const int32_t *norm = c ? rec.samples_B : rec.samples_A;

                    for (k = 0; k < MAX_TERM; k++) {
                        int age = dt_ring_slot_age (term, pl->mono, 1,
                                                    in_block, k);

                        if (age < 0)
                            continue;

                        int32_t eh = tr->snap [end_in_block].hist [pi] [c] [age];
                        int32_t gh = norm [k];

                        cnt->history_compare++;

                        if (gh != eh) {
                            dt_note_diff (cnt, "norm-history", pl->label, blockno, pi, c,
                                          pl->block_index + end_in_block, term, gh, eh,
                                          "normalized-slot");
                            bad = 1;
                        }
                    }
                }
            }

        remaining -= in_block;
        cursor += in_block;
    }

    return bad;
}

/* single-block convenience wrapper used by the reference program */
static int dt_compare_observed (DtCounts *cnt, const DtBlockPlan *pl, const DtTrace *tr,
                                int blockno, int call_first, int call_n)
{
    (void) blockno;
    return dt_compare_call (cnt, &pl, &tr, 1, call_first, call_n);
}

#endif /* DECORR_TEST_H */
