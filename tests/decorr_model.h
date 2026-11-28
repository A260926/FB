////////////////////////////////////////////////////////////////////////////
//                       **** WAVPACK ****                                 //
//         Independent decorrelation reference model (test support)        //
//                                                                         //
//  This header is a deliberately independent reimplementation of the      //
//  WavPack lossless integer decorrelation stage, written in natural time  //
//  with 64-bit intermediates. It never calls the production              //
//  apply_weight / update_weight / decorrelation-pass / state-restore      //
//  functions and it does not copy their ring-buffer layout: history is    //
//  kept as a plain shift queue where index 0 is the most recent sample.   //
//                                                                         //
//  Only lossless 16/24-bit integer PCM in mono and stereo is modelled.    //
//  This is test support only; all functions are static so each test       //
//  translation unit gets its own copy and nothing is added to the library.//
////////////////////////////////////////////////////////////////////////////

#ifndef DECORR_MODEL_H
#define DECORR_MODEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>

#define DM_MAX_TERMS     16
#define DM_MAX_DELAY     8

// One decorrelation pass. History is natural time: hist_*[0] is the most
// recent reconstructed sample for that pass, hist_*[1] the one before.

typedef struct {
    int term;
    int delta;
    int64_t weight_A, weight_B;
    int64_t hist_A [DM_MAX_DELAY];
    int64_t hist_B [DM_MAX_DELAY];
} DmPass;

// A whole chain. "exec_order [k]" is the declaration index of the pass that
// is executed at decoder position k. WavPack metadata declares terms in one
// order and the decoder executes them in the reverse order; this models it.

typedef struct {
    int num_terms;
    DmPass pass [DM_MAX_TERMS];
    int exec_order [DM_MAX_TERMS];
    int mono;
    int64_t frames_seen;
    int overflow;                // set if any intermediate/reconstructed value left int32
    int64_t max_abs_source;      // largest |prediction source| observed in decode
} DmState;

// Plan recovered from metadata. All values are in declaration order, with
// weights already restored from signed-8-bit storage and history already
// restored from signed-16-bit base-2 logs. Omitted trailing entries are zero.

typedef struct {
    int num_terms;
    int mono;
    int term [DM_MAX_TERMS];
    int delta [DM_MAX_TERMS];
    int64_t weight_A [DM_MAX_TERMS], weight_B [DM_MAX_TERMS];
    int have_A [DM_MAX_TERMS], have_B [DM_MAX_TERMS];
    int64_t hist_A [DM_MAX_TERMS][DM_MAX_DELAY];   // [0] = oldest (metadata order)
    int64_t hist_B [DM_MAX_TERMS][DM_MAX_DELAY];
    int exec_order [DM_MAX_TERMS];
} DmPlan;

// A read-only view of one pass in the format's normalized representation:
//  - terms 1..8: the last "term" outputs, oldest first (slots 0..term-1)
//  - terms 17/18: {most recent, previous}
//  - negative terms: one saved value per channel in slot 0

typedef struct {
    int term, delta;
    int64_t weight_A, weight_B;
    int64_t hist_A [DM_MAX_DELAY], hist_B [DM_MAX_DELAY];
    int count_A, count_B;
} DmPassView;

/////////////////////////// format constant tables //////////////////////////

static const unsigned char dm_log2_table [256] = {
    0x00,0x01,0x03,0x04,0x06,0x07,0x09,0x0a,0x0b,0x0d,0x0e,0x10,0x11,0x12,0x14,0x15,
    0x16,0x18,0x19,0x1a,0x1c,0x1d,0x1e,0x20,0x21,0x22,0x24,0x25,0x26,0x28,0x29,0x2a,
    0x2c,0x2d,0x2e,0x2f,0x31,0x32,0x33,0x34,0x36,0x37,0x38,0x39,0x3b,0x3c,0x3d,0x3e,
    0x3f,0x41,0x42,0x43,0x44,0x45,0x47,0x48,0x49,0x4a,0x4b,0x4d,0x4e,0x4f,0x50,0x51,
    0x52,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x5c,0x5d,0x5e,0x5f,0x60,0x61,0x62,0x63,
    0x64,0x66,0x67,0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,0x70,0x71,0x72,0x74,0x75,
    0x76,0x77,0x78,0x79,0x7a,0x7b,0x7c,0x7d,0x7e,0x7f,0x80,0x81,0x82,0x83,0x84,0x85,
    0x86,0x87,0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,0x90,0x91,0x92,0x93,0x94,0x95,
    0x96,0x97,0x98,0x99,0x9a,0x9b,0x9b,0x9c,0x9d,0x9e,0x9f,0xa0,0xa1,0xa2,0xa3,0xa4,
    0xa5,0xa6,0xa7,0xa8,0xa9,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf,0xb0,0xb1,0xb2,0xb2,
    0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xb9,0xba,0xbb,0xbc,0xbd,0xbe,0xbf,0xc0,0xc0,
    0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc6,0xc7,0xc8,0xc9,0xca,0xcb,0xcb,0xcc,0xcd,0xce,
    0xcf,0xd0,0xd0,0xd1,0xd2,0xd3,0xd4,0xd4,0xd5,0xd6,0xd7,0xd8,0xd8,0xd9,0xda,0xdb,
    0xdc,0xdc,0xdd,0xde,0xdf,0xe0,0xe0,0xe1,0xe2,0xe3,0xe4,0xe4,0xe5,0xe6,0xe7,0xe7,
    0xe8,0xe9,0xea,0xea,0xeb,0xec,0xed,0xee,0xee,0xef,0xf0,0xf1,0xf1,0xf2,0xf3,0xf4,
    0xf4,0xf5,0xf6,0xf7,0xf7,0xf8,0xf9,0xf9,0xfa,0xfb,0xfc,0xfc,0xfd,0xfe,0xff,0xff
};

static const unsigned char dm_exp2_table [256] = {
    0x00,0x01,0x01,0x02,0x03,0x03,0x04,0x05,0x06,0x06,0x07,0x08,0x08,0x09,0x0a,0x0b,
    0x0b,0x0c,0x0d,0x0e,0x0e,0x0f,0x10,0x10,0x11,0x12,0x13,0x13,0x14,0x15,0x16,0x16,
    0x17,0x18,0x19,0x19,0x1a,0x1b,0x1c,0x1d,0x1d,0x1e,0x1f,0x20,0x20,0x21,0x22,0x23,
    0x24,0x24,0x25,0x26,0x27,0x28,0x28,0x29,0x2a,0x2b,0x2c,0x2c,0x2d,0x2e,0x2f,0x30,
    0x30,0x31,0x32,0x33,0x34,0x35,0x35,0x36,0x37,0x38,0x39,0x3a,0x3a,0x3b,0x3c,0x3d,
    0x3e,0x3f,0x40,0x41,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x48,0x49,0x4a,0x4b,
    0x4c,0x4d,0x4e,0x4f,0x50,0x51,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,
    0x5b,0x5c,0x5d,0x5e,0x5e,0x5f,0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,
    0x7a,0x7b,0x7c,0x7d,0x7e,0x7f,0x80,0x81,0x82,0x83,0x84,0x85,0x87,0x88,0x89,0x8a,
    0x8b,0x8c,0x8d,0x8e,0x8f,0x90,0x91,0x92,0x93,0x95,0x96,0x97,0x98,0x99,0x9a,0x9b,
    0x9c,0x9d,0x9f,0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa8,0xa9,0xaa,0xab,0xac,0xad,
    0xaf,0xb0,0xb1,0xb2,0xb3,0xb4,0xb6,0xb7,0xb8,0xb9,0xba,0xbc,0xbd,0xbe,0xbf,0xc0,
    0xc2,0xc3,0xc4,0xc5,0xc6,0xc8,0xc9,0xca,0xcb,0xcd,0xce,0xcf,0xd0,0xd2,0xd3,0xd4,
    0xd6,0xd7,0xd8,0xd9,0xdb,0xdc,0xdd,0xde,0xe0,0xe1,0xe2,0xe4,0xe5,0xe6,0xe8,0xe9,
    0xea,0xec,0xed,0xee,0xf0,0xf1,0xf2,0xf4,0xf5,0xf6,0xf8,0xf9,0xfa,0xfc,0xfd,0xff
};

/////////////////////////// format conversions //////////////////////////////

static int dm_nbits (uint32_t v)
{
    int n = 0;
    while (v) { n++; v >>= 1; }
    return n;
}

static int dm_wp_log2 (uint32_t avalue)
{
    int dbits;

    if ((avalue += avalue >> 9) < (1u << 8)) {
        dbits = dm_nbits (avalue);
        return (dbits << 8) + dm_log2_table [(avalue << (9 - dbits)) & 0xff];
    }
    else {
        if (avalue < (1u << 16))
            dbits = dm_nbits (avalue >> 8) + 8;
        else if (avalue < (1u << 24))
            dbits = dm_nbits (avalue >> 16) + 16;
        else
            dbits = dm_nbits (avalue >> 24) + 24;

        return (dbits << 8) + dm_log2_table [(avalue >> (dbits - 9)) & 0xff];
    }
}

// signed 16-bit logarithm of a signed 32-bit value (format rule)
static int16_t dm_log2s (int32_t value)
{
    int r = (value < 0) ? -dm_wp_log2 ((uint32_t) -value) : dm_wp_log2 ((uint32_t) value);
    return (int16_t) r;
}

// inverse of dm_log2s (format rule)
static int32_t dm_exp2s (int16_t log)
{
    uint32_t value;

    if (log < 0)
        return -dm_exp2s ((int16_t) -log);

    value = dm_exp2_table [log & 0xff] | 0x100;

    if ((log >>= 8) <= 9)
        return (int32_t) (value >> (9 - log));
    else
        return (int32_t) (value << ((log - 9) & 0x1f));
}

// signed 8-bit weight storage / restoration (format rule)
static int dm_store_weight (int weight)
{
    if (weight > 1024)
        weight = 1024;
    else if (weight < -1024)
        weight = -1024;

    if (weight > 0)
        weight -= (weight + 64) >> 7;

    return (signed char) ((weight + 4) >> 3);
}

static int dm_restore_weight (int stored)
{
    int result = (int) (signed char) stored * 8;

    if (result > 0)
        result += (result + 64) >> 7;

    return result;
}

/////////////////////////// prediction arithmetic ///////////////////////////

// floor ((weight * source + 512) / 1024), using a 64-bit product and an
// explicit floor correction so that negative half-steps round downward.
static int64_t dm_apply_weight (int64_t weight, int64_t source)
{
    int64_t num = weight * source + 512;
    int64_t q = num / 1024;

    if (num % 1024 < 0)
        q--;

    return q;
}

// Weight adaptation looks at the sign of the prediction SOURCE and of the
// current RESIDUAL (the value entering this pass), never at the reconstructed
// sample. The clipped variant is only used by negative terms and limits the
// weight to +/-1024 on both ends.

static int64_t dm_update_weight (int64_t weight, int delta, int64_t source, int64_t residual)
{
    if (source && residual) {
        if ((source ^ residual) < 0)
            weight -= delta;
        else
            weight += delta;
    }

    return weight;
}

static int64_t dm_update_weight_clip (int64_t weight, int delta, int64_t source, int64_t residual)
{
    if (source && residual) {
        if ((source ^ residual) < 0) {
            weight -= delta;

            if (weight < -1024)
                weight = -1024;
        }
        else {
            weight += delta;

            if (weight > 1024)
                weight = 1024;
        }
    }

    return weight;
}

//////////////////////////// metadata -> plan //////////////////////////////

static int32_t dm_rd16 (const unsigned char *p)
{
    return (int32_t) (int16_t) (unsigned short) (p [0] | (p [1] << 8));
}

// Build a plan from the raw ID_DECORR_TERMS / _WEIGHTS / _SAMPLES payloads, in
// on-disk declaration order, applying the format's rule that omitted trailing
// weights and histories read as zero. Returns 0 on success, -1 on malformed
// input. This never consults the decoder's restored state.

static int dm_plan_from_metadata (DmPlan *plan, int mono,
    const unsigned char *terms, int terms_len,
    const unsigned char *weights, int weights_len,
    const unsigned char *samples, int samples_len)
{
    int i, n, tcnt, doff = 0;

    if (terms_len < 1 || terms_len > DM_MAX_TERMS)
        return -1;

    n = plan->num_terms = terms_len;
    plan->mono = mono;

    for (i = 0; i < n; i++) {
        int b = terms [i];

        plan->term [i] = (b & 0x1f) - 5;
        plan->delta [i] = (b >> 5) & 0x7;
        plan->weight_A [i] = plan->weight_B [i] = 0;
        plan->have_A [i] = plan->have_B [i] = 0;

        for (int k = 0; k < DM_MAX_DELAY; k++)
            plan->hist_A [i][k] = plan->hist_B [i][k] = 0;

        if (plan->term [i] == 0 || plan->term [i] < -3 ||
            (plan->term [i] > 8 && plan->term [i] < 17) || plan->term [i] > 18)
                return -1;

        if (mono && plan->term [i] < 0)
            return -1;

        // decoder execution order is the reverse of metadata declaration order
        plan->exec_order [i] = n - 1 - i;
    }

    tcnt = mono ? weights_len : weights_len / 2;

    if (tcnt > n)
        return -1;

    for (i = 0; i < tcnt; i++) {
        plan->weight_A [i] = dm_restore_weight (weights [mono ? i : i * 2]);

        if (!mono)
            plan->weight_B [i] = dm_restore_weight (weights [i * 2 + 1]);
    }

    for (i = 0; i < n && doff < samples_len; i++) {
        int term = plan->term [i];

        if (term > 8) {                        // 17 / 18: two samples per channel
            if (doff + (mono ? 4 : 8) > samples_len)
                return -1;

            plan->hist_A [i][0] = dm_exp2s ((int16_t) dm_rd16 (samples + doff)); doff += 2;
            plan->hist_A [i][1] = dm_exp2s ((int16_t) dm_rd16 (samples + doff)); doff += 2;
            plan->have_A [i] = 2;

            if (!mono) {
                plan->hist_B [i][0] = dm_exp2s ((int16_t) dm_rd16 (samples + doff)); doff += 2;
                plan->hist_B [i][1] = dm_exp2s ((int16_t) dm_rd16 (samples + doff)); doff += 2;
                plan->have_B [i] = 2;
            }
        }
        else if (term < 0) {                   // one saved value per channel
            if (doff + 4 > samples_len)
                return -1;

            plan->hist_A [i][0] = dm_exp2s ((int16_t) dm_rd16 (samples + doff)); doff += 2;
            plan->hist_B [i][0] = dm_exp2s ((int16_t) dm_rd16 (samples + doff)); doff += 2;
            plan->have_A [i] = plan->have_B [i] = 1;
        }
        else {                                 // 1..8 samples per channel, oldest first
            for (int m = 0; m < term; m++) {
                if (doff + (mono ? 2 : 4) > samples_len)
                    return -1;

                plan->hist_A [i][m] = dm_exp2s ((int16_t) dm_rd16 (samples + doff)); doff += 2;

                if (!mono) {
                    plan->hist_B [i][m] = dm_exp2s ((int16_t) dm_rd16 (samples + doff)); doff += 2;
                    plan->have_B [i] = term;
                }

                plan->have_A [i] = term;
            }
        }
    }

    if (doff != samples_len)
        return -1;

    return 0;
}

// Convert a plan to model state, translating metadata history order into the
// model's newest-first queue.
static void dm_init (DmState *st, const DmPlan *plan)
{
    st->num_terms = plan->num_terms;
    st->mono = plan->mono;
    st->frames_seen = 0;
    st->overflow = 0;
    st->max_abs_source = 0;

    for (int i = 0; i < plan->num_terms; i++) {
        DmPass *p = st->pass + i;
        int term = plan->term [i];

        p->term = term;
        p->delta = plan->delta [i];
        p->weight_A = plan->weight_A [i];
        p->weight_B = plan->weight_B [i];

        for (int k = 0; k < DM_MAX_DELAY; k++)
            p->hist_A [k] = p->hist_B [k] = 0;

        if (term > 8) {
            p->hist_A [0] = plan->hist_A [i][0];
            p->hist_A [1] = plan->hist_A [i][1];
            p->hist_B [0] = plan->hist_B [i][0];
            p->hist_B [1] = plan->hist_B [i][1];
        }
        else if (term < 0) {
            p->hist_A [0] = plan->hist_A [i][0];
            p->hist_B [0] = plan->hist_B [i][0];
        }
        else {
            for (int k = 0; k < term; k++) {
                p->hist_A [k] = plan->hist_A [i][term - 1 - k];
                p->hist_B [k] = plan->hist_B [i][term - 1 - k];
            }
        }

        st->exec_order [i] = plan->exec_order [i];
    }
}

/////////////////////////////// decode /////////////////////////////////////

// Decode "frames" frames of interleaved residuals into interleaved PCM,
// applying each decoder pass to the whole chunk in decoder execution order.
// The model tracks natural-time history, so arbitrary chunk sizes compose.
static void dm_decode_block (DmState *st, const int32_t *residual, int32_t *pcm, int frames){
    int nchan = st->mono ? 1 : 2;
    int64_t *buf = (int64_t *) malloc ((size_t) frames * nchan * sizeof (int64_t));
    int k;

    if (!buf)
        return;

    for (int i = 0; i < frames * nchan; i++)
        buf [i] = residual [i];

    for (k = 0; k < st->num_terms; k++) {
        DmPass *p = st->pass + st->exec_order [k];
        int term = p->term;

        if (term > 0 && term <= 8) {
            for (int f = 0; f < frames; f++) {
                int64_t srcA = p->hist_A [term - 1];
                int64_t inA = buf [f * nchan];
                int64_t outA = inA + dm_apply_weight (p->weight_A, srcA);

                if (srcA > st->max_abs_source) st->max_abs_source = srcA;
                if (-srcA > st->max_abs_source) st->max_abs_source = -srcA;

                p->weight_A = dm_update_weight (p->weight_A, p->delta, srcA, inA);

                for (int j = term - 1; j > 0; j--)
                    p->hist_A [j] = p->hist_A [j - 1];

                p->hist_A [0] = outA;
                buf [f * nchan] = outA;

                if (nchan == 2) {
                    int64_t srcB = p->hist_B [term - 1];
                    int64_t inB = buf [f * nchan + 1];
                    int64_t outB = inB + dm_apply_weight (p->weight_B, srcB);

                    p->weight_B = dm_update_weight (p->weight_B, p->delta, srcB, inB);

                    for (int j = term - 1; j > 0; j--)
                        p->hist_B [j] = p->hist_B [j - 1];

                    p->hist_B [0] = outB;
                    buf [f * nchan + 1] = outB;
                }
            }
        }
        else if (term == 17 || term == 18) {
            for (int f = 0; f < frames; f++) {
                int64_t srcA, inA = buf [f * nchan], outA;

                if (term == 17)
                    srcA = 2 * p->hist_A [0] - p->hist_A [1];
                else {
                    int64_t d = 3 * p->hist_A [0] - p->hist_A [1];
                    srcA = d / 2;
                    if (d % 2 < 0)
                        srcA--;
                }

                if (srcA > st->max_abs_source) st->max_abs_source = srcA;
                if (-srcA > st->max_abs_source) st->max_abs_source = -srcA;

                outA = inA + dm_apply_weight (p->weight_A, srcA);
                p->weight_A = dm_update_weight (p->weight_A, p->delta, srcA, inA);
                p->hist_A [1] = p->hist_A [0];
                p->hist_A [0] = outA;
                buf [f * nchan] = outA;

                if (nchan == 2) {
                    int64_t srcB, inB = buf [f * nchan + 1], outB;

                    if (term == 17)
                        srcB = 2 * p->hist_B [0] - p->hist_B [1];
                    else {
                        int64_t d = 3 * p->hist_B [0] - p->hist_B [1];
                        srcB = d / 2;
                        if (d % 2 < 0)
                            srcB--;
                    }

                    outB = inB + dm_apply_weight (p->weight_B, srcB);
                    p->weight_B = dm_update_weight (p->weight_B, p->delta, srcB, inB);
                    p->hist_B [1] = p->hist_B [0];
                    p->hist_B [0] = outB;
                    buf [f * nchan + 1] = outB;
                }
            }
        }
        else if (term == -1) {
            for (int f = 0; f < frames; f++) {
                int64_t inA = buf [f * nchan], inB = buf [f * nchan + 1];
                int64_t srcA = p->hist_A [0];
                int64_t outA = inA + dm_apply_weight (p->weight_A, srcA);

                p->weight_A = dm_update_weight_clip (p->weight_A, p->delta, srcA, inA);
                buf [f * nchan] = outA;

                {
                    int64_t outB = inB + dm_apply_weight (p->weight_B, outA);

                    p->weight_B = dm_update_weight_clip (p->weight_B, p->delta, outA, inB);
                    p->hist_A [0] = outB;
                    buf [f * nchan + 1] = outB;
                }
            }
        }
        else if (term == -2) {
            for (int f = 0; f < frames; f++) {
                int64_t inA = buf [f * nchan], inB = buf [f * nchan + 1];
                int64_t srcB = p->hist_B [0];
                int64_t outB = inB + dm_apply_weight (p->weight_B, srcB);

                p->weight_B = dm_update_weight_clip (p->weight_B, p->delta, srcB, inB);
                buf [f * nchan + 1] = outB;

                {
                    int64_t outA = inA + dm_apply_weight (p->weight_A, outB);

                    p->weight_A = dm_update_weight_clip (p->weight_A, p->delta, outB, inA);
                    p->hist_B [0] = outA;
                    buf [f * nchan] = outA;
                }
            }
        }
        else if (term == -3) {
            for (int f = 0; f < frames; f++) {
                int64_t inA = buf [f * nchan], inB = buf [f * nchan + 1];
                int64_t srcA = p->hist_A [0], srcB = p->hist_B [0];
                int64_t outA = inA + dm_apply_weight (p->weight_A, srcA);
                int64_t outB = inB + dm_apply_weight (p->weight_B, srcB);

                p->weight_A = dm_update_weight_clip (p->weight_A, p->delta, srcA, inA);
                p->weight_B = dm_update_weight_clip (p->weight_B, p->delta, srcB, inB);
                buf [f * nchan] = outA;
                buf [f * nchan + 1] = outB;
                p->hist_B [0] = outA;      // next tick's right uses this tick's left
                p->hist_A [0] = outB;      // next tick's left uses this tick's right
            }
        }
    }

    for (int i = 0; i < frames * nchan; i++) {
        if (buf [i] < INT32_MIN || buf [i] > INT32_MAX) {
            fprintf (stderr, "model: intermediate outside int32 (frame %d ch %d)\n",
                i / nchan, i % nchan);
            st->overflow = 1;
            pcm [i] = 0;
        }
        else
            pcm [i] = (int32_t) buf [i];
    }

    st->frames_seen += frames;
    free (buf);
}

// Encode "frames" frames of interleaved PCM into residuals using the same
// natural-time model in reverse. This mirrors the encoder's residue
// computation: for each decoder pass in turn, subtract that pass's prediction
// (using the *current* chunk's reconstructed values as the source) from the
// data flowing through. It exists so the tests can produce a residual plan
// that is guaranteed to decode back to a bounded, chosen PCM signal without
// relying on any production code. Returns 0 on success, -1 if an intermediate
// left the int32 range.
static int dm_encode_block (DmState *st, const int32_t *pcm, int32_t *residual, int frames)
{
    int nchan = st->mono ? 1 : 2;
    int64_t *buf = (int64_t *) malloc ((size_t) frames * nchan * sizeof (int64_t));
    int k, ok = 1;

    if (!buf)
        return -1;

    for (int i = 0; i < frames * nchan; i++)
        buf [i] = pcm [i];

    // Invert the decoder: decoding runs passes exec_order[0..n-1], so encoding
    // must undo them in the opposite order (exec_order[n-1] down to [0]), which
    // is exactly declaration order 0..n-1.
    for (k = st->num_terms - 1; k >= 0; k--) {
        DmPass *p = st->pass + st->exec_order [k];
        int term = p->term;

        if (term > 0 && term <= 8) {
            for (int f = 0; f < frames; f++) {
                int64_t srcA = p->hist_A [term - 1];
                int64_t srcB = p->hist_B [term - 1];
                int64_t inA = buf [f * nchan];
                int64_t predA = dm_apply_weight (p->weight_A, srcA);
                int64_t outA = inA - predA;

                p->weight_A = dm_update_weight (p->weight_A, p->delta, srcA, outA);

                for (int j = term - 1; j > 0; j--)
                    p->hist_A [j] = p->hist_A [j - 1];

                p->hist_A [0] = inA;
                buf [f * nchan] = outA;

                if (nchan == 2) {
                    int64_t inB = buf [f * nchan + 1];
                    int64_t predB = dm_apply_weight (p->weight_B, srcB);
                    int64_t outB = inB - predB;

                    p->weight_B = dm_update_weight (p->weight_B, p->delta, srcB, outB);

                    for (int j = term - 1; j > 0; j--)
                        p->hist_B [j] = p->hist_B [j - 1];

                    p->hist_B [0] = inB;
                    buf [f * nchan + 1] = outB;
                }
            }
        }
        else if (term == 17 || term == 18) {
            for (int f = 0; f < frames; f++) {
                int64_t srcA, inA = buf [f * nchan], outA;

                if (term == 17)
                    srcA = 2 * p->hist_A [0] - p->hist_A [1];
                else {
                    int64_t d = 3 * p->hist_A [0] - p->hist_A [1];
                    srcA = d / 2;
                    if (d % 2 < 0)
                        srcA--;
                }

                outA = inA - dm_apply_weight (p->weight_A, srcA);
                p->weight_A = dm_update_weight (p->weight_A, p->delta, srcA, outA);
                p->hist_A [1] = p->hist_A [0];
                p->hist_A [0] = inA;
                buf [f * nchan] = outA;

                if (nchan == 2) {
                    int64_t srcB, inB = buf [f * nchan + 1], outB;

                    if (term == 17)
                        srcB = 2 * p->hist_B [0] - p->hist_B [1];
                    else {
                        int64_t d = 3 * p->hist_B [0] - p->hist_B [1];
                        srcB = d / 2;
                        if (d % 2 < 0)
                            srcB--;
                    }

                    outB = inB - dm_apply_weight (p->weight_B, srcB);
                    p->weight_B = dm_update_weight (p->weight_B, p->delta, srcB, outB);
                    p->hist_B [1] = p->hist_B [0];
                    p->hist_B [0] = inB;
                    buf [f * nchan + 1] = outB;
                }
            }
        }
        else if (term == -1) {
            for (int f = 0; f < frames; f++) {
                int64_t inA = buf [f * nchan], inB = buf [f * nchan + 1];
                int64_t srcA = p->hist_A [0];
                int64_t outA = inA - dm_apply_weight (p->weight_A, srcA);

                p->weight_A = dm_update_weight_clip (p->weight_A, p->delta, srcA, outA);
                buf [f * nchan] = outA;

                {
                    int64_t predB = dm_apply_weight (p->weight_B, inA);
                    int64_t outB = inB - predB;

                    p->weight_B = dm_update_weight_clip (p->weight_B, p->delta, inA, outB);
                    p->hist_A [0] = inB;
                    buf [f * nchan + 1] = outB;
                }
            }
        }
        else if (term == -2) {
            for (int f = 0; f < frames; f++) {
                int64_t inA = buf [f * nchan], inB = buf [f * nchan + 1];
                int64_t srcB = p->hist_B [0];
                int64_t outB = inB - dm_apply_weight (p->weight_B, srcB);

                p->weight_B = dm_update_weight_clip (p->weight_B, p->delta, srcB, outB);
                buf [f * nchan + 1] = outB;

                {
                    int64_t predA = dm_apply_weight (p->weight_A, inB);
                    int64_t outA = inA - predA;

                    p->weight_A = dm_update_weight_clip (p->weight_A, p->delta, inB, outA);
                    p->hist_B [0] = inA;
                    buf [f * nchan] = outA;
                }
            }
        }
        else if (term == -3) {
            for (int f = 0; f < frames; f++) {
                int64_t inA = buf [f * nchan], inB = buf [f * nchan + 1];
                int64_t srcA = p->hist_A [0], srcB = p->hist_B [0];
                int64_t outA = inA - dm_apply_weight (p->weight_A, srcA);
                int64_t outB = inB - dm_apply_weight (p->weight_B, srcB);

                p->weight_A = dm_update_weight_clip (p->weight_A, p->delta, srcA, outA);
                p->weight_B = dm_update_weight_clip (p->weight_B, p->delta, srcB, outB);
                buf [f * nchan] = outA;
                buf [f * nchan + 1] = outB;
                p->hist_B [0] = inA;
                p->hist_A [0] = inB;
            }
        }
    }

    for (int i = 0; i < frames * nchan; i++) {
        if (buf [i] < INT32_MIN || buf [i] > INT32_MAX) {
            st->overflow = 1;
            ok = 0;
            residual [i] = 0;
        }
        else
            residual [i] = (int32_t) buf [i];
    }

    st->frames_seen += frames;
    free (buf);
    return ok ? 0 : -1;
}

// Snapshot one pass (declaration index) in the format-normalized view.
static void dm_pass_view (const DmState *st, int decl_index, DmPassView *view)
{
    const DmPass *p = st->pass + decl_index;
    int term = p->term;

    view->term = term;
    view->delta = p->delta;
    view->weight_A = p->weight_A;
    view->weight_B = p->weight_B;

    for (int k = 0; k < DM_MAX_DELAY; k++)
        view->hist_A [k] = view->hist_B [k] = 0;

    if (term > 0 && term <= 8) {
        for (int k = 0; k < term; k++) {
            view->hist_A [k] = p->hist_A [term - 1 - k];
            view->hist_B [k] = p->hist_B [term - 1 - k];
        }

        view->count_A = view->count_B = term;
    }
    else if (term == 17 || term == 18) {
        view->hist_A [0] = p->hist_A [0]; view->hist_A [1] = p->hist_A [1];
        view->hist_B [0] = p->hist_B [0]; view->hist_B [1] = p->hist_B [1];
        view->count_A = view->count_B = 2;
    }
    else {
        view->hist_A [0] = p->hist_A [0];
        view->hist_B [0] = p->hist_B [0];
        view->count_A = view->count_B = 1;
    }
}

// Map an execution position (0 = first decoder pass) to a declaration index.
static int dm_exec_to_decl (const DmState *st, int exec_index)
{
    return st->exec_order [exec_index];
}

// Fold a model state back into a plan in declaration order, using the format's
// normalized history layout. Used to regenerate a block's metadata from the
// model state the previous block ended with, so each block genuinely re-derives
// its initial state from metadata (and the model is re-seeded from a fresh
// parse of those same bytes).
static void dm_plan_from_state (DmPlan *plan, const DmState *st)
{
    plan->num_terms = st->num_terms;
    plan->mono = st->mono;

    for (int i = 0; i < st->num_terms; i++) {
        DmPassView v;

        dm_pass_view (st, i, &v);
        plan->term [i] = v.term;
        plan->delta [i] = v.delta;
        plan->weight_A [i] = v.weight_A;
        plan->weight_B [i] = v.weight_B;
        plan->have_A [i] = v.count_A;
        plan->have_B [i] = v.count_B;

        for (int k = 0; k < DM_MAX_DELAY; k++) {
            plan->hist_A [i][k] = v.hist_A [k];
            plan->hist_B [i][k] = v.hist_B [k];
        }

        plan->exec_order [i] = st->exec_order [i];
    }
}

#endif // DECORR_MODEL_H
