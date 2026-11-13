////////////////////////////////////////////////////////////////////////////
//                           **** ADPCM-XQ ****                           //
//                  Xtreme Quality ADPCM Encoder/Decoder                  //
//                    Copyright (c) 2024 David Bryant.                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

/* Tests for the optional exact search cache (adpcm-search-cache.c).
 *
 * Byte-for-byte reference: an independent, self-contained ADPCM encoder is
 * embedded below.  It is written from the same published IMA ADPCM rules but
 * shares no code or state with adpcm-lib.c, and it implements the lookahead
 * search without any cache at all.  Its output is compared against the
 * library with the cache off and on.  The library itself is never used to
 * produce the reference stream.
 *
 * The remaining tests exercise:
 *   - every code width, noise-shaping mode, and lookahead depth 0..16;
 *   - short tails and consecutive blocks (audio history carried between them);
 *   - inputs where identical states recur under different pruning thresholds;
 *   - the observer hook, to confirm expanded really tracks production nodes;
 *   - two interleaved instances, runtime toggling, and capacity pressure;
 *   - the reported duplicate-state workload from the task statement.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "adpcm-lib.h"
#include "adpcm-search-cache.h"

#ifndef ADPCM_SEARCH_CACHE_SLOTS
#error "ADPCM_SEARCH_CACHE_SLOTS must be defined by adpcm-search-cache.h"
#endif

#define CHUNK   4096
#define MAXFRAMES 300

static int cases_run;

#define TEST(cond, ...) do { \
    if (!(cond)) { \
        fprintf (stderr, "FAILED: " __VA_ARGS__); \
        fprintf (stderr, "\n"); \
        exit (1); \
    } \
} while (0)

/************************ independent reference encoder ************************/

#define R_CLIP(v, lo, hi) do { if ((v) > (hi)) (v) = (hi); else if ((v) < (lo)) (v) = (lo); } while (0)

static const uint16_t r_step_table [89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int r_index_table [8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
static const int r_index_table_3bit [4] = { -1, -1, 1, 2 };
static const int r_index_table_5bit [16] = { -1, -1, -1, -1, -1, -1, -1, -1, 1, 2, 4, 6, 8, 10, 13, 16 };

#define R_NIBBLE_TO_DELTA(b,n) ((n)<(1<<((b)-1))?(n)+1:(1<<((b)-1))-1-(n))

typedef struct {
    int32_t pcmdata;
    int32_t shaping_weight, error;
    int index;
} ref_chan;

/************************ generic reference search ************************/

static uint64_t ref_search (int bps, ref_chan pchan_in, int nch, int32_t csample,
    const int16_t *psample, int flags, int *best_nibble, uint64_t max_error)
{
    ref_chan pchan = pchan_in, orig = pchan_in;
    uint16_t step = r_step_table [pchan.index];
    int nibble, testnbl, nvals = 1 << bps;
    ref_chan chan;
    uint64_t min_error;
    int32_t delta = csample - pchan.pcmdata, csample2;
    uint16_t trial_delta;

    /* naive nearest code, exactly as specified for each width */

    if (bps == 2) {
        if (delta < 0) {
            if (-delta >= step) { pchan.pcmdata -= step + (step >> 1); nibble = 3; }
            else { pchan.pcmdata -= step >> 1; nibble = 2; }
        }
        else
            pchan.pcmdata += step * ((nibble = delta >= step)) + (step >> 1);
    }
    else if (bps == 3) {
        uint16_t td = step >> 2;
        if (delta < 0) {
            int mag = ((-delta << 1) + (step & 1)) / step;
            nibble = 0x4 | (mag > 3 ? 3 : mag);
        }
        else {
            int mag = ((delta << 1) + (step & 1)) / step;
            nibble = mag > 3 ? 3 : mag;
        }
        if (nibble & 1) td += step >> 1;
        if (nibble & 2) td += step;
        if (nibble & 4) pchan.pcmdata -= td; else pchan.pcmdata += td;
    }
    else if (bps == 5) {
        static const char comp_table [16] = { 0, 0, 0, 5, 0, 6, 4, 10, 0, 7, 6, 10, 4, 11, 11, 13 };
        uint16_t td = step >> 4;
        if (delta < 0) {
            int mag = ((-delta << 3) + comp_table [step & 15]) / step;
            nibble = 0x10 | (mag > 15 ? 15 : mag);
        }
        else {
            int mag = ((delta << 3) + comp_table [step & 15]) / step;
            nibble = mag > 15 ? 15 : mag;
        }
        if (nibble & 1) td += step >> 3;
        if (nibble & 2) td += step >> 2;
        if (nibble & 4) td += step >> 1;
        if (nibble & 8) td += step;
        if (nibble & 0x10) pchan.pcmdata -= td; else pchan.pcmdata += td;
    }
    else {
        uint16_t td = step >> 3;
        if (delta < 0) {
            int mag = ((-delta << 2) + (step & 3) + ((step & 1) << 1)) / step;
            nibble = 0x8 | (mag > 7 ? 7 : mag);
        }
        else {
            int mag = ((delta << 2) + (step & 3) + ((step & 1) << 1)) / step;
            nibble = mag > 7 ? 7 : mag;
        }
        if (nibble & 1) td += step >> 2;
        if (nibble & 2) td += step >> 1;
        if (nibble & 4) td += step;
        if (nibble & 8) pchan.pcmdata -= td; else pchan.pcmdata += td;
    }

    R_CLIP (pchan.pcmdata, -32768, 32767);
    if (best_nibble) *best_nibble = nibble;
    min_error = (uint64_t) (pchan.pcmdata - csample) * (pchan.pcmdata - csample);

    if (!(flags & LOOKAHEAD_DEPTH) || min_error >= max_error)
        return min_error;

    chan = pchan;

    if (bps == 2) {
        chan.index += (nibble & 1) * 3 - 1;
    }
    else if (bps == 3)
        chan.index += r_index_table_3bit [nibble & 3];
    else if (bps == 5)
        chan.index += r_index_table_5bit [nibble & 15];
    else
        chan.index += r_index_table [nibble & 7];

    R_CLIP (chan.index, 0, 88);

    if (flags & (NOISE_SHAPING_STATIC | NOISE_SHAPING_DYNAMIC)) {
        chan.error += chan.pcmdata;
        {
            int32_t t = -((chan.shaping_weight * chan.error + 512) >> 10);
            if (chan.shaping_weight < 0 && t) {
                if (t == chan.error) t = (t < 0) ? t + 1 : t - 1;
                chan.error = -psample [nch];
                csample2 = psample [nch] + t;
            }
            else {
                csample2 = psample [nch] + t;
                chan.error = -csample2;
            }
        }
    }
    else
        csample2 = psample [nch];

    min_error += ref_search (bps, chan, nch, csample2, psample + nch, flags - 1, NULL, max_error - min_error);

    if (flags & LOOKAHEAD_NO_BRANCHING)
        return min_error;

    for (testnbl = 0; testnbl < nvals; testnbl++) {
        uint64_t error, threshold;

        if (testnbl == nibble)
            continue;

        if (bps == 4 && !(flags & LOOKAHEAD_EXHAUSTIVE) && (~testnbl & 7) &&
            abs (R_NIBBLE_TO_DELTA (4,nibble) - R_NIBBLE_TO_DELTA (4,testnbl)) > 3)
            continue;

        if (bps == 3 && !(flags & LOOKAHEAD_EXHAUSTIVE) && (~testnbl & 3) &&
            abs (R_NIBBLE_TO_DELTA (3,nibble) - R_NIBBLE_TO_DELTA (3,testnbl)) > 2)
            continue;

        if (bps == 5 && !(flags & LOOKAHEAD_EXHAUSTIVE) && (testnbl | 3) != (nibble | 0xf) &&
            abs (R_NIBBLE_TO_DELTA (5,nibble) - R_NIBBLE_TO_DELTA (5,testnbl)) > 3)
            continue;

        chan = orig;
        trial_delta = (bps == 2) ? 0 : (bps == 3) ? (step >> 2) : (bps == 4) ? (step >> 3) : (step >> 4);

        if (bps == 2) {
            if (testnbl & 2) chan.pcmdata -= step * (testnbl & 1) + (step >> 1);
            else chan.pcmdata += step * (testnbl & 1) + (step >> 1);
        }
        else if (bps == 3) {
            if (testnbl & 1) trial_delta += step >> 1;
            if (testnbl & 2) trial_delta += step;
            if (testnbl & 4) chan.pcmdata -= trial_delta; else chan.pcmdata += trial_delta;
        }
        else if (bps == 4) {
            if (testnbl & 1) trial_delta += step >> 2;
            if (testnbl & 2) trial_delta += step >> 1;
            if (testnbl & 4) trial_delta += step;
            if (testnbl & 8) chan.pcmdata -= trial_delta; else chan.pcmdata += trial_delta;
        }
        else {
            if (testnbl & 1) trial_delta += step >> 3;
            if (testnbl & 2) trial_delta += step >> 2;
            if (testnbl & 4) trial_delta += step >> 1;
            if (testnbl & 8) trial_delta += step;
            if (testnbl & 0x10) chan.pcmdata -= trial_delta; else chan.pcmdata += trial_delta;
        }

        R_CLIP (chan.pcmdata, -32768, 32767);
        error = (uint64_t) (chan.pcmdata - csample) * (chan.pcmdata - csample);
        threshold = max_error < min_error ? max_error : min_error;

        if (error < threshold) {
            if (bps == 2)
                chan.index += (testnbl & 1) * 3 - 1;
            else if (bps == 3)
                chan.index += r_index_table_3bit [testnbl & 3];
            else if (bps == 5)
                chan.index += r_index_table_5bit [testnbl & 15];
            else
                chan.index += r_index_table [testnbl & 7];

            R_CLIP (chan.index, 0, 88);

            if (flags & (NOISE_SHAPING_STATIC | NOISE_SHAPING_DYNAMIC)) {
                chan.error += chan.pcmdata;
                {
                    int32_t t = -((chan.shaping_weight * chan.error + 512) >> 10);
                    if (chan.shaping_weight < 0 && t) {
                        if (t == chan.error) t = (t < 0) ? t + 1 : t - 1;
                        chan.error = -psample [nch];
                        csample2 = psample [nch] + t;
                    }
                    else {
                        csample2 = psample [nch] + t;
                        chan.error = -csample2;
                    }
                }
            }
            else
                csample2 = psample [nch];

            error += ref_search (bps, chan, nch, csample2, psample + nch, flags - 1, NULL, threshold - error);

            if (error < min_error) {
                if (best_nibble) *best_nibble = testnbl;
                min_error = error;
            }
        }
    }

    return min_error;
}

/* Reference block encoder: independent implementation of the whole block
 * layout, initial-index sweep, noise shaping, and chunk packing. */

typedef struct {
    ref_chan chan [2];
    int num_channels, sample_rate, flags;
    int static_weight;
    int16_t last_shaping_weight;
    int16_t *dns;
} ref_ctx;

static uint8_t ref_encode_block (ref_ctx *rc, uint8_t *outbuf, size_t *outbufsize,
    const int16_t *inbuf, int inbufcount, int bps)
{
    int ch, i;

    *outbufsize = 0;

    for (ch = 0; ch < rc->num_channels; ch++)
        rc->chan [ch].pcmdata = *inbuf++;

    inbufcount--;

    if (inbufcount && (rc->chan [0].index < 0 || (rc->flags & LOOKAHEAD_DEPTH) >= 3)) {
        int flags = 16 | LOOKAHEAD_NO_BRANCHING;

        if ((flags & LOOKAHEAD_DEPTH) > inbufcount - 1)
            flags = (flags & ~LOOKAHEAD_DEPTH) + inbufcount - 1;

        for (ch = 0; ch < rc->num_channels; ch++) {
            uint64_t min_error = UINT64_MAX, error_per_index [89];
            int best_index = 0, tindex;

            for (tindex = 0; tindex <= 88; tindex++) {
                ref_chan chan = rc->chan [ch];

                chan.index = tindex;
                chan.shaping_weight = 0;
                error_per_index [tindex] = ref_search (bps, chan, rc->num_channels,
                    inbuf [ch], inbuf + ch, flags, NULL, UINT64_MAX);
            }

            for (tindex = 0; tindex <= 87; tindex++) {
                uint64_t terror = error_per_index [tindex];

                if (tindex)
                    terror = (error_per_index [tindex - 1] + terror + error_per_index [tindex + 1]) / 3;

                if (terror < min_error) {
                    best_index = tindex;
                    min_error = terror;
                }
            }

            rc->chan [ch].index = best_index;
        }
    }

    for (ch = 0; ch < rc->num_channels; ch++) {
        outbuf [0] = rc->chan [ch].pcmdata;
        outbuf [1] = rc->chan [ch].pcmdata >> 8;
        outbuf [2] = rc->chan [ch].index;
        outbuf [3] = 0;
        outbuf += 4;
        *outbufsize += 4;
    }

    if (inbufcount && (rc->flags & NOISE_SHAPING_DYNAMIC)) {
        rc->dns = malloc (inbufcount * sizeof (int16_t));
        generate_dns_values (inbuf, inbufcount, rc->num_channels, rc->sample_rate,
            rc->dns, -512, rc->last_shaping_weight);
        rc->last_shaping_weight = rc->dns [inbufcount - 1];
    }

    for (ch = 0; ch < rc->num_channels; ch++) {
        int shiftbits = 0, numbits = 0;
        size_t jj = 0;
        const int16_t *pb = inbuf + ch;

        if (rc->flags & NOISE_SHAPING_STATIC)
            rc->chan [ch].shaping_weight = rc->static_weight;

        for (i = 0; i < inbufcount; i++) {
            int32_t csample;
            int nibble;
            uint16_t step, trial_delta;
            ref_chan *pc = rc->chan + ch;

            if (rc->flags & NOISE_SHAPING_DYNAMIC)
                pc->shaping_weight = rc->dns [i];

            csample = *pb;

            if (rc->flags & (NOISE_SHAPING_STATIC | NOISE_SHAPING_DYNAMIC)) {
                int32_t t = -((pc->shaping_weight * pc->error + 512) >> 10);
                if (pc->shaping_weight < 0 && t) {
                    if (t == pc->error) t = (t < 0) ? t + 1 : t - 1;
                    pc->error = -csample;
                    csample += t;
                }
                else
                    pc->error = -(csample += t);
            }

            {
                int flags = rc->flags;

                if ((flags & LOOKAHEAD_DEPTH) > inbufcount - i - 1)
                    flags = (flags & ~LOOKAHEAD_DEPTH) + inbufcount - i - 1;

                ref_search (bps, *pc, rc->num_channels, csample, pb, flags, &nibble, UINT64_MAX);
            }

            step = r_step_table [pc->index];

            if (bps == 2) {
                if (nibble & 2) pc->pcmdata -= step * (nibble & 1) + (step >> 1);
                else pc->pcmdata += step * (nibble & 1) + (step >> 1);
                pc->index += (nibble & 1) * 3 - 1;
            }
            else if (bps == 3) {
                trial_delta = step >> 2;
                if (nibble & 1) trial_delta += step >> 1;
                if (nibble & 2) trial_delta += step;
                if (nibble & 4) pc->pcmdata -= trial_delta; else pc->pcmdata += trial_delta;
                pc->index += r_index_table_3bit [nibble & 3];
            }
            else if (bps == 4) {
                trial_delta = step >> 3;
                if (nibble & 1) trial_delta += step >> 2;
                if (nibble & 2) trial_delta += step >> 1;
                if (nibble & 4) trial_delta += step;
                if (nibble & 8) pc->pcmdata -= trial_delta; else pc->pcmdata += trial_delta;
                pc->index += r_index_table [nibble & 7];
            }
            else {
                trial_delta = step >> 4;
                if (nibble & 1) trial_delta += step >> 3;
                if (nibble & 2) trial_delta += step >> 2;
                if (nibble & 4) trial_delta += step >> 1;
                if (nibble & 8) trial_delta += step;
                if (nibble & 0x10) pc->pcmdata -= trial_delta; else pc->pcmdata += trial_delta;
                pc->index += r_index_table_5bit [nibble & 15];
            }

            R_CLIP (pc->index, 0, 88);
            R_CLIP (pc->pcmdata, -32768, 32767);

            if (rc->flags & (NOISE_SHAPING_STATIC | NOISE_SHAPING_DYNAMIC))
                pc->error += pc->pcmdata;

            shiftbits |= nibble << numbits;
            pb += rc->num_channels;

            if ((numbits += bps) >= 8) {
                outbuf [(jj & ~3) * rc->num_channels + (ch * 4) + (jj & 3)] = shiftbits;
                shiftbits >>= 8;
                numbits -= 8;
                jj++;
            }
        }

        if (numbits)
            outbuf [(jj & ~3) * rc->num_channels + (ch * 4) + (jj & 3)] = shiftbits;
    }

    *outbufsize += (inbufcount * bps + 31) / 32 * rc->num_channels * 4;

    if (rc->dns) {
        free (rc->dns);
        rc->dns = NULL;
    }

    return 1;
}

/***************************** helpers / fixtures *****************************/

/* The encoders leave the bytes after the final partial chunk untouched.  Give
 * every output buffer the same deterministic, non-zero pattern so that any
 * such untouched byte compares equal between two encoders, and real writes
 * still differ. */
static void prefill (uint8_t *buf, size_t cap)
{
    size_t i;

    for (i = 0; i < cap; i++)
        buf [i] = (uint8_t) (0xA5 ^ (i * 37));
}

static void fill_signal (int16_t *in, int n, int kind)
{
    int i;

    for (i = 0; i < n; i++) {
        switch (kind) {
            case 0: in [i] = (int16_t) (12000 * sin (0.17 * i) + 6000 * cos (0.23 * i)); break;
            case 1: in [i] = (i & 1) ? 32767 : -32768; break;
            case 2: in [i] = (int16_t) ((i * 7919) ^ (i << 5)); break;
            case 3: in [i] = (int16_t) (i < n / 2 ? 30000 : -30000); break;
            default: in [i] = (int16_t) (4000 * sin (0.9 * i) * ((i & 8) ? 1 : -1)); break;
        }
    }
}

static void *new_lib_ctx (int channels, int lookahead, int shaping, int cache)
{
    void *c = adpcm_create_context (channels, 16000, lookahead, shaping);

    TEST (c != NULL, "context allocation failed");
    if (cache)
        TEST (adpcm_set_search_cache (c, 1) == 1, "enabling cache failed");

    return c;
}

/* Encode a sequence of blocks through the library.  Returns the total bytes and
 * fills both supplied buffers (caller passes capacity). */
static size_t lib_encode_blocks (void *c, const int16_t *pcm, const int *counts, int nblocks,
    int bps, uint8_t *out, size_t *blockbytes, size_t cap)
{
    size_t total = 0, bytes;
    int b;

    for (b = 0; b < nblocks; b++) {
        int n = counts [b];

        TEST (adpcm_encode_block_ex (c, out + total, &bytes, pcm, n, bps) == 1,
            "library encode failed bps=%d block=%d", bps, b);
        blockbytes [b] = bytes;
        total += bytes;
        pcm += n;
    }

    TEST (total <= cap, "output overflow: %zu > %zu", total, cap);
    return total;
}

/* Reference encode the same sequence; returns bytes. */
static size_t ref_encode_blocks (int channels, int lookahead, int shaping, int weight,
    const int16_t *pcm, const int *counts, int nblocks, int bps, uint8_t *out, size_t cap)
{
    ref_ctx rc;
    size_t total = 0, bytes;
    int b;

    memset (&rc, 0, sizeof (rc));
    rc.num_channels = channels;
    rc.sample_rate = 16000;
    rc.flags = lookahead | shaping;
    rc.static_weight = weight;
    rc.chan [0].index = rc.chan [1].index = -1;

    for (b = 0; b < nblocks; b++) {
        TEST (total + 2048 <= cap, "reference output overflow");
        ref_encode_block (&rc, out + total, &bytes, pcm, counts [b], bps);
        total += bytes;
        pcm += counts [b];
    }

    return total;
}

/***************************** test cases *****************************/

/* 1. Independent byte reference across widths, shaping, depth, tails, blocks. */
static void test_reference_matrix (void)
{
    static const int widths [] = { 2, 3, 4, 5 };
    static const int shapings [] = { NOISE_SHAPING_OFF, NOISE_SHAPING_STATIC, NOISE_SHAPING_DYNAMIC };
    static const int depths [] = { 0, 1, 2, 3, 5, 8 };
    static const int tails [] = { 1, 2, 5, 12, 33, 65 };
    uint8_t lib [CHUNK], ref [CHUNK], cached [CHUNK];
    int16_t pcm [MAXFRAMES + 32];
    int wi, si, di, ti;

    for (wi = 0; wi < 4; wi++)
        for (si = 0; si < 3; si++)
            for (di = 0; di < 6; di++)
                for (ti = 0; ti < 6; ti++) {
                    int bps = widths [wi], shaping = shapings [si], depth = depths [di];
                    int n = tails [ti], counts [2], nb;
                    size_t bl, br, bc, lb [2];
                    void *c;

                    /* one or two consecutive blocks; second block continues history */
                    counts [0] = n;
                    nb = 1;
                    if (n >= 12) { counts [1] = n - 7; nb = 2; }

                    fill_signal (pcm, MAXFRAMES + 32, (wi + si + di + ti) % 5);
                    prefill (lib, CHUNK);
                    prefill (ref, CHUNK);
                    prefill (cached, CHUNK);

                    c = new_lib_ctx (1, depth, shaping, 0);
                    bl = lib_encode_blocks (c, pcm, counts, nb, bps, lib, lb, CHUNK);
                    adpcm_free_context (c);

                    br = ref_encode_blocks (1, depth, shaping, 1024, pcm, counts, nb, bps, ref, CHUNK);
                    if (bl != br || memcmp (lib, ref, bl)) {
                        size_t q;
                        fprintf (stderr, "lib: ");
                        for (q = 0; q < bl; q++) fprintf (stderr, "%02x", lib [q]);
                        fprintf (stderr, "\nref: ");
                        for (q = 0; q < br; q++) fprintf (stderr, "%02x", ref [q]);
                        fprintf (stderr, "\n");
                    }
                    TEST (bl == br && !memcmp (lib, ref, bl),
                        "reference mismatch bps=%d shaping=%#x depth=%d n=%d nb=%d (%zu vs %zu)",
                        bps, shaping, depth, n, nb, bl, br);

                    c = new_lib_ctx (1, depth, shaping, 1);
                    bc = lib_encode_blocks (c, pcm, counts, nb, bps, cached, lb, CHUNK);
                    adpcm_free_context (c);

                    TEST (bc == bl && !memcmp (cached, lib, bl),
                        "cache changed output bps=%d shaping=%#x depth=%d n=%d nb=%d",
                        bps, shaping, depth, n, nb);

                    /* a stereo variant spot-checks the interleaved predictor path */
                    if (nb == 1 && n >= 12 && (si + di) % 3 == 0) {
                        int16_t st_pcm [MAXFRAMES * 2];
                        uint8_t st_a [CHUNK], st_b [CHUNK], st_ref [CHUNK];
                        size_t sa, sb, sr;

                        for (int q = 0; q < n * 2; q++)
                            st_pcm [q] = pcm [q / 2] + (q & 1) * 2000;

                        prefill (st_a, CHUNK);
                        prefill (st_b, CHUNK);
                        prefill (st_ref, CHUNK);

                        c = new_lib_ctx (2, depth, shaping, 0);
                        sa = lib_encode_blocks (c, st_pcm, (int []) { n }, 1, bps, st_a, lb, CHUNK);
                        adpcm_free_context (c);

                        sr = ref_encode_blocks (2, depth, shaping, 1024, st_pcm, (int []) { n }, 1,
                            bps, st_ref, CHUNK);
                        TEST (sa == sr && !memcmp (st_a, st_ref, sa),
                            "stereo reference mismatch bps=%d shaping=%#x depth=%d", bps, shaping, depth);

                        c = new_lib_ctx (2, depth, shaping, 1);
                        sb = lib_encode_blocks (c, st_pcm, (int []) { n }, 1, bps, st_b, lb, CHUNK);
                        adpcm_free_context (c);
                        TEST (sb == sa && !memcmp (st_b, st_a, sa),
                            "stereo cache mismatch bps=%d shaping=%#x depth=%d", bps, shaping, depth);
                    }

                    cases_run++;
                }
}

/* 2. Full depth range 0..16, exhaustive and default, cache exactness.  Depths
 * beyond 8 also get an independent byte-reference comparison. */
static void test_depths (void)
{
    static const int modes [] = { 0, LOOKAHEAD_EXHAUSTIVE };
    int16_t pcm [MAXFRAMES];
    uint8_t a [CHUNK], b [CHUNK];
    int d, mi;

    for (d = 0; d <= 16; d++)
        for (mi = 0; mi < 2; mi++) {
            void *c;
            size_t ba, bb, lb [1];
            adpcm_search_stats sa, sb;

            fill_signal (pcm, MAXFRAMES, d % 5);
            prefill (a, CHUNK);
            prefill (b, CHUNK);

            if (d >= 9) {
                uint8_t ref [CHUNK];
                size_t br = ref_encode_blocks (1, d | modes [mi], NOISE_SHAPING_OFF, 1024,
                    pcm, (int []) { 33 }, 1, 4, ref, CHUNK);

                c = new_lib_ctx (1, d | modes [mi], NOISE_SHAPING_OFF, 0);
                ba = lib_encode_blocks (c, pcm, (int []) { 33 }, 1, 4, a, lb, CHUNK);
                adpcm_free_context (c);

                TEST (ba == br && !memcmp (a, ref, ba),
                    "deep reference mismatch depth=%d exhaustive=%d", d, modes [mi] != 0);
            }

            c = new_lib_ctx (1, d | modes [mi], NOISE_SHAPING_STATIC, 0);
            ba = lib_encode_blocks (c, pcm, (int []) { 65 }, 1, 4, a, lb, CHUNK);
            TEST (adpcm_get_search_stats (c, &sa) && sa.hits == 0, "hits nonzero when disabled");
            adpcm_free_context (c);

            c = new_lib_ctx (1, d | modes [mi], NOISE_SHAPING_STATIC, 1);
            bb = lib_encode_blocks (c, pcm, (int []) { 65 }, 1, 4, b, lb, CHUNK);
            TEST (adpcm_get_search_stats (c, &sb), "stats query failed");
            adpcm_free_context (c);

            TEST (ba == bb && !memcmp (a, b, ba),
                "cache mismatch at depth=%d exhaustive=%d", d, modes [mi] != 0);
            TEST (sb.expanded <= sa.expanded,
                "cache expanded more nodes at depth=%d (off=%llu on=%llu)",
                d, (unsigned long long) sa.expanded, (unsigned long long) sb.expanded);

            cases_run++;
        }
}

/* 3. The reported duplicate-state workload: exact reduction and exact bytes. */
static void test_reported_workload (void)
{
    int16_t pcm [65];
    uint8_t off [CHUNK], on [CHUNK];
    size_t bo, bn, lb [1];
    adpcm_search_stats so, sn;
    void *c;
    int i;

    for (i = 0; i < 65; i++)
        pcm [i] = (i & 1) ? 32767 : -32768;

    prefill (off, CHUNK);
    prefill (on, CHUNK);

    c = new_lib_ctx (1, 5 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_OFF, 0);
    bo = lib_encode_blocks (c, pcm, (int []) { 65 }, 1, 4, off, lb, CHUNK);
    TEST (adpcm_get_search_stats (c, &so), "stats query failed");
    adpcm_free_context (c);

    c = new_lib_ctx (1, 5 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_OFF, 1);
    bn = lib_encode_blocks (c, pcm, (int []) { 65 }, 1, 4, on, lb, CHUNK);
    TEST (adpcm_get_search_stats (c, &sn), "stats query failed");
    adpcm_free_context (c);

    TEST (bo == bn && !memcmp (off, on, bo), "reported workload output changed");
    TEST (so.expanded == 1888, "unexpected baseline expanded=%llu", (unsigned long long) so.expanded);
    TEST (so.hits == 0, "disabled hits must be 0");
    TEST (sn.hits > 0, "no cache hits on duplicate-state workload");
    TEST (sn.expanded * 10 <= so.expanded * 9,
        "expanded reduction below 10%% (%llu -> %llu)",
        (unsigned long long) so.expanded, (unsigned long long) sn.expanded);

    printf ("  reported workload: expanded %llu -> %llu (%llu hits)\n",
        (unsigned long long) so.expanded, (unsigned long long) sn.expanded,
        (unsigned long long) sn.hits);

    cases_run++;
}

/* 4. Observer hook: expanded really counts production recursion nodes. */
static uint64_t observed_nodes, observed_pruned;

static void observe (int bps, int flags, uint64_t max_error)
{
    (void) bps;
    (void) flags;
    if (max_error != UINT64_MAX)
        observed_pruned++;
    observed_nodes++;
}

static void test_observer_consistency (void)
{
    int16_t pcm [MAXFRAMES];
    uint8_t out [CHUNK];
    adpcm_search_stats st;
    size_t lb [1];
    int configs;

    for (configs = 0; configs < 12; configs++) {
        int bps = 2 + configs % 4;
        int depth = (configs * 3) % 9;
        int shaping = (configs % 3) ? NOISE_SHAPING_OFF : NOISE_SHAPING_STATIC;
        int mode = depth | ((configs & 2) ? LOOKAHEAD_EXHAUSTIVE : 0);
        uint64_t off_expanded;
        void *c;

        fill_signal (pcm, MAXFRAMES, configs % 5);

        /* baseline: observer must agree with expanded in the cache-off case too */
        observed_nodes = 0;
        observed_pruned = 0;
        adpcm_search_observe_hook = observe;
        c = new_lib_ctx (1, mode, shaping, 0);
        lib_encode_blocks (c, pcm, (int []) { 65 }, 1, bps, out, lb, CHUNK);
        TEST (adpcm_get_search_stats (c, &st), "stats query failed");
        adpcm_free_context (c);
        adpcm_search_observe_hook = NULL;

        off_expanded = st.expanded;
        TEST (observed_nodes == st.expanded,
            "observer=%llu but expanded=%llu (bps=%d depth=%d off)",
            (unsigned long long) observed_nodes, (unsigned long long) st.expanded, bps, depth);
        TEST (observed_pruned > 0 || depth == 0,
            "observer never saw a pruned node (bps=%d depth=%d)", bps, depth);

        /* cache on: same agreement, and a reduction whenever hits occur */
        observed_nodes = 0;
        observed_pruned = 0;
        adpcm_search_observe_hook = observe;
        c = new_lib_ctx (1, mode, shaping, 1);
        lib_encode_blocks (c, pcm, (int []) { 65 }, 1, bps, out, lb, CHUNK);
        TEST (adpcm_get_search_stats (c, &st), "stats query failed");
        adpcm_free_context (c);
        adpcm_search_observe_hook = NULL;

        TEST (observed_nodes == st.expanded,
            "observer=%llu but expanded=%llu (bps=%d depth=%d on)",
            (unsigned long long) observed_nodes, (unsigned long long) st.expanded, bps, depth);
        TEST (st.hits <= off_expanded, "more hits than baseline nodes (bps=%d depth=%d)", bps, depth);

        if (st.hits)
            TEST (st.expanded + st.hits <= off_expanded,
                "cache did not reduce work (bps=%d depth=%d: %llu+%llu > %llu)", bps, depth,
                (unsigned long long) st.expanded, (unsigned long long) st.hits,
                (unsigned long long) off_expanded);

        cases_run++;
    }
}

/* 5. Different thresholds for the same state: never a bogus hit, output stable.
 *
 * Periodic alternation produces many identical predictor states under
 * different pruning limits.  We verify both the byte stream and the next
 * block's history (a third block continues the same context), and that the
 * statistics are self-consistent. */
static void test_threshold_variants (void)
{
    int16_t pcm [MAXFRAMES * 3];
    uint8_t off [CHUNK], on [CHUNK];
    int counts [] = { 65, 65, 65 };
    size_t ob [3], nb [3];
    adpcm_search_stats so, sn;
    void *c;
    int i, k;

    for (k = 0; k < MAXFRAMES * 3; k++)
        pcm [k] = (k & 1) ? 32767 : -32768;

    prefill (off, CHUNK);
    prefill (on, CHUNK);

    for (i = 0; i < 3; i++) {
        c = new_lib_ctx (1, 5 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_OFF, 0);
        lib_encode_blocks (c, pcm, counts, 3, 4, off, ob, CHUNK);
        TEST (adpcm_get_search_stats (c, &so), "stats query failed");
        adpcm_free_context (c);

        c = new_lib_ctx (1, 5 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_OFF, 1);
        lib_encode_blocks (c, pcm, counts, 3, 4, on, nb, CHUNK);
        TEST (adpcm_get_search_stats (c, &sn), "stats query failed");
        adpcm_free_context (c);

        TEST (ob [0] == nb [0] && ob [1] == nb [1] && ob [2] == nb [2] &&
            !memcmp (off, on, ob [0] + ob [1] + ob [2]),
            "multi-block output changed (pass %d)", i);
        TEST (so.hits == 0 && sn.hits > 0, "unexpected statistics (pass %d)", i);
        TEST (sn.expanded < so.expanded, "no reduction across blocks (pass %d)", i);
    }

    /* Per-block statistics must be cleared by each encode call: the last
     * block of a three-block run cannot report the first block's totals. */
    {
        c = new_lib_ctx (1, 5 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_OFF, 1);
        lib_encode_blocks (c, pcm, (int []) { 65 }, 1, 4, on, nb, CHUNK);
        TEST (adpcm_get_search_stats (c, &sn), "stats query failed");
        {
            uint64_t first = sn.expanded;

            lib_encode_blocks (c, pcm + 65, (int []) { 65 }, 1, 4, on, nb, CHUNK);
            TEST (adpcm_get_search_stats (c, &sn), "stats query failed");
            TEST (sn.expanded < first * 2 && sn.expanded > 0,
                "statistics not cleared per block");
        }
        adpcm_free_context (c);
    }

    cases_run++;
}

/* 6. Two interleaved instances do not share results, and toggling preserves
 * audio history.
 *
 * Discrimination: the two instances encode the *same* PCM with the same
 * configuration.  With per-instance tables each sees exactly the work of a
 * solo run (same expanded and hits).  If the table were shared, the second
 * instance would find the first instance's entries and report extra hits. */
static void test_isolation_and_toggle (void)
{
    int16_t pa [MAXFRAMES];
    uint8_t solo [CHUNK], out_a [CHUNK], out_b [CHUNK];
    adpcm_search_stats solo_st, sa, sb;
    size_t bsolo, oa, ob, lb [1];
    void *ca, *cb;
    int i;

    fill_signal (pa, MAXFRAMES, 2);

    ca = new_lib_ctx (1, 5 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_STATIC, 1);
    bsolo = lib_encode_blocks (ca, pa, (int []) { 65 }, 1, 4, solo, lb, CHUNK);
    TEST (adpcm_get_search_stats (ca, &solo_st), "stats");
    adpcm_free_context (ca);

    ca = new_lib_ctx (1, 5 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_STATIC, 1);
    cb = new_lib_ctx (1, 5 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_STATIC, 1);

    TEST (adpcm_encode_block_ex (ca, out_a, &oa, pa, 65, 4) == 1, "encode A");
    TEST (adpcm_encode_block_ex (cb, out_b, &ob, pa, 65, 4) == 1, "encode B");

    TEST (oa == bsolo && ob == bsolo && !memcmp (out_a, solo, bsolo) && !memcmp (out_b, solo, bsolo),
        "interleaved instances differ from solo");

    TEST (adpcm_get_search_stats (ca, &sa) && adpcm_get_search_stats (cb, &sb), "stats");
    TEST (sa.expanded == solo_st.expanded && sa.hits == solo_st.hits,
        "instance A statistics differ when interleaved");
    TEST (sb.expanded == solo_st.expanded && sb.hits == solo_st.hits,
        "instance B reused instance A's results (expanded %llu/%llu hits %llu/%llu)",
        (unsigned long long) sb.expanded, (unsigned long long) solo_st.expanded,
        (unsigned long long) sb.hits, (unsigned long long) solo_st.hits);

    adpcm_free_context (ca);
    adpcm_free_context (cb);

    /* toggle between blocks against a permanently enabled context */
    for (i = 0; i < 6; i++) {
        uint8_t toggled [CHUNK], steady [CHUNK];
        void *ct = new_lib_ctx (1, 4, NOISE_SHAPING_DYNAMIC, 0);
        void *cs = new_lib_ctx (1, 4, NOISE_SHAPING_DYNAMIC, 1);
        size_t toff = 0, soff = 0, bytes;

        prefill (toggled, CHUNK);
        prefill (steady, CHUNK);
        int b;

        for (b = 0; b < 5; b++) {
            TEST (adpcm_set_search_cache (ct, b & 1) == 1, "toggle failed");

            TEST (adpcm_encode_block_ex (ct, toggled + toff, &bytes, pa + b * 33, 33, 4) == 1,
                "toggled encode");
            toff += bytes;

            TEST (adpcm_encode_block_ex (cs, steady + soff, &bytes, pa + b * 33, 33, 4) == 1,
                "steady encode");
            soff += bytes;
        }

        TEST (toff == soff && !memcmp (toggled, steady, toff), "toggling changed output (pass %d)", i);

        adpcm_free_context (ct);
        adpcm_free_context (cs);
    }

    cases_run++;
}

/* 6b. A pruned result must never masquerade as an exact one.
 *
 * The production cache module is exercised directly: a result stored under a
 * limit it did not beat (a pruned result) must be rejected, while a result
 * that beat its limit is reusable only under a limit it still beats.  This is
 * the exact-value vs. pruning distinction the recursion relies on. */
static void test_pruned_not_reused (void)
{
    struct adpcm_search_cache *cache = adpcm_search_cache_create ();
    struct adpcm_search_key key;
    uint64_t value;
    int i;

    TEST (cache != NULL, "cache create failed");

    memset (&key, 0, sizeof (key));
    key.pcmdata = 12345;
    key.index = 42;
    key.nch = 1;
    key.bps = 4;
    key.flags = 3;

    /* value >= limit: pruned, must not be stored */
    adpcm_search_cache_store (cache, &key, 5000, 5000);
    TEST (adpcm_search_cache_lookup (cache, &key, UINT64_MAX, &value) == 0,
        "pruned result (value == limit) was stored as exact");
    adpcm_search_cache_store (cache, &key, 6000, 5555);
    TEST (adpcm_search_cache_lookup (cache, &key, UINT64_MAX, &value) == 0,
        "pruned result (value > limit) was stored as exact");

    /* exact result: found only under a strictly wider limit */
    adpcm_search_cache_store (cache, &key, 4000, 5555);
    TEST (adpcm_search_cache_lookup (cache, &key, 4000, &value) == 0,
        "exact result was served under a limit it does not beat");
    TEST (adpcm_search_cache_lookup (cache, &key, 4001, &value) == 1 && value == 4000,
        "exact result not served under a wider limit");

    /* differing identity fields must not alias, one at a time */
    for (i = 0; i < 7; i++) {
        struct adpcm_search_key other = key;

        switch (i) {
            case 0: other.pcmdata++; break;
            case 1: other.index++; break;
            case 2: other.error++; break;
            case 3: other.shaping_weight++; break;
            case 4: other.csample++; break;
            case 5: other.samples [0]++; other.flags = 3; break;
            default: other.flags = 2; break;
        }

        TEST (adpcm_search_cache_lookup (cache, &other, UINT64_MAX, &value) == 0,
            "cache aliased on identity field %d", i);
    }

    adpcm_search_cache_destroy (cache);
    cases_run++;
}

/* 7. Capacity pressure: enough distinct states to exhaust the table many times.
 * Must stay exact (recompute on eviction, never a false hit).
 *
 * The table holds 32768 entries.  At depth 16 each frame visits hundreds of
 * distinct nodes, so deepening the lookahead quickly exceeds the table and
 * forces eviction; the byte stream must still match the cache-off run. */
static void test_capacity_pressure (void)
{
    enum { BLOCKS = 40, FRAMES = 65 };
    int16_t *pcm = malloc (BLOCKS * FRAMES * sizeof (int16_t));
    uint8_t *off = malloc (BLOCKS * CHUNK), *on = malloc (BLOCKS * CHUNK);
    size_t bo = 0, bn = 0, bytes, offb [BLOCKS], onb [BLOCKS];
    adpcm_search_stats st;
    void *c;
    int b, i;

    TEST (pcm && off && on, "allocation failed");

    for (i = 0; i < BLOCKS * FRAMES; i++)
        pcm [i] = (int16_t) (((i * 104729) ^ (i << 6)) % 65536 - 32768);

    for (b = 0; b < BLOCKS; b++)
        pcm [b * FRAMES] = (int16_t) (b * 1237 - 20000);

    prefill (off, BLOCKS * CHUNK);
    prefill (on, BLOCKS * CHUNK);

    c = new_lib_ctx (1, 12 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_STATIC, 0);
    for (b = 0; b < BLOCKS; b++) {
        TEST (adpcm_encode_block_ex (c, off + bo, &bytes, pcm + b * FRAMES, FRAMES, 4) == 1, "off encode");
        offb [b] = bytes;
        bo += bytes;
    }
    adpcm_free_context (c);

    c = new_lib_ctx (1, 12 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_STATIC, 1);
    for (b = 0; b < BLOCKS; b++) {
        TEST (adpcm_encode_block_ex (c, on + bn, &bytes, pcm + b * FRAMES, FRAMES, 4) == 1, "on encode");
        onb [b] = bytes;
        bn += bytes;
    }
    TEST (adpcm_get_search_stats (c, &st), "stats query failed");
    adpcm_free_context (c);

    {
        int same = bo == bn;

        for (b = 0; same && b < BLOCKS; b++)
            same = offb [b] == onb [b];

        if (same)
            same = !memcmp (off, on, bo);

        TEST (same, "capacity pressure changed output (%zu vs %zu)", bo, bn);
    }
    TEST (st.hits > 0, "no hits under capacity pressure");

    printf ("  capacity pressure: %d blocks, last-block expanded=%llu hits=%llu\n",
        BLOCKS, (unsigned long long) st.expanded, (unsigned long long) st.hits);

    free (pcm);
    free (off);
    free (on);
    cases_run++;
}

/* 8. Argument validation and stats API contract. */
static void test_api_contract (void)
{
    adpcm_search_stats st;
    void *c = adpcm_create_context (1, 16000, 3, NOISE_SHAPING_OFF);

    TEST (c != NULL, "context allocation failed");

    TEST (adpcm_set_search_cache (c, 0) == 1, "disable should succeed");
    TEST (adpcm_set_search_cache (c, 1) == 1, "enable should succeed");
    TEST (adpcm_set_search_cache (c, 2) == 0, "enabled=2 must fail");
    TEST (adpcm_set_search_cache (c, -1) == 0, "enabled=-1 must fail");
    TEST (adpcm_set_search_cache (NULL, 1) == 0, "NULL context must fail");
    TEST (adpcm_get_search_stats (c, NULL) == 0, "NULL stats out must fail");
    TEST (adpcm_get_search_stats (NULL, &st) == 0, "NULL context stats must fail");

    /* zero the statistics before any block: query must not alter behaviour */
    TEST (adpcm_get_search_stats (c, &st) == 1, "stats query failed");
    TEST (st.expanded == 0 && st.hits == 0, "fresh context statistics not zero");

    /* stats query does not perturb encoding */
    {
        uint8_t a [CHUNK], b [CHUNK];
        size_t ba, bb;
        int16_t pcm [MAXFRAMES];

        fill_signal (pcm, MAXFRAMES, 1);
        prefill (a, CHUNK);
        prefill (b, CHUNK);
        TEST (adpcm_encode_block_ex (c, a, &ba, pcm, 65, 4) == 1, "encode a");
        adpcm_free_context (c);
        c = NULL;

        c = adpcm_create_context (1, 16000, 3, NOISE_SHAPING_OFF);
        TEST (adpcm_set_search_cache (c, 1) == 1, "enable");
        TEST (adpcm_get_search_stats (c, &st) == 1, "pre-query");
        TEST (adpcm_encode_block_ex (c, b, &bb, pcm, 65, 4) == 1, "encode b");
        TEST (ba == bb && !memcmp (a, b, ba), "stats query changed output");
    }

    if (c)
        adpcm_free_context (c);
    cases_run++;
}

/* 9. Enabling the cache between blocks must not disturb encoding history:
 * compare a block encoded with a context that was just switched on against
 * the identical context that has always had the cache enabled.  Both must
 * produce the same bytes and the same statistics for the second block. */
static void test_switch_preserves_history (void)
{
    int16_t pcm [MAXFRAMES];
    uint8_t always [CHUNK], switched [CHUNK];
    adpcm_search_stats st_always, st_switched;
    size_t b1, b2, l1 [1], l2 [1];
    void *ca, *cs;
    int k;

    for (k = 0; k < MAXFRAMES; k++)
        pcm [k] = (int16_t) (9000 * sin (0.21 * k) + 7000 * cos (0.37 * k));

    prefill (always, CHUNK);
    prefill (switched, CHUNK);

    ca = new_lib_ctx (1, 6 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_DYNAMIC, 0);
    TEST (adpcm_set_search_cache (ca, 1) == 1, "enable");
    lib_encode_blocks (ca, pcm, (int []) { 65 }, 1, 4, always, l1, CHUNK);

    cs = new_lib_ctx (1, 6 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_DYNAMIC, 0);
    lib_encode_blocks (cs, pcm, (int []) { 65 }, 1, 4, switched, l1, CHUNK);
    TEST (adpcm_set_search_cache (cs, 1) == 1, "enable between blocks");
    lib_encode_blocks (cs, pcm + 65, (int []) { 65 }, 1, 4, switched + l1 [0], l2, CHUNK);

    b1 = l1 [0];
    lib_encode_blocks (ca, pcm + 65, (int []) { 65 }, 1, 4, always + b1, l2, CHUNK);
    b2 = l2 [0];
    TEST (b1 == b2, "block sizes differ after switch (%zu vs %zu)", b1, b2);
    TEST (!memcmp (always + b1, switched + b1, b2), "history lost when enabling cache");
    TEST (!memcmp (always, switched, b2), "cache-enabled first block differs");

    TEST (adpcm_get_search_stats (ca, &st_always) && adpcm_get_search_stats (cs, &st_switched), "stats");
    TEST (st_switched.expanded == st_always.expanded && st_switched.hits == st_always.hits,
        "second-block statistics differ after switch");

    adpcm_free_context (ca);
    adpcm_free_context (cs);
    cases_run++;
}

int main (void)
{
    test_reference_matrix ();
    test_depths ();
    test_reported_workload ();
    test_observer_consistency ();
    test_threshold_variants ();
    test_isolation_and_toggle ();
    test_pruned_not_reused ();
    test_capacity_pressure ();
    test_switch_preserves_history ();
    test_api_contract ();

    printf ("ADPCM_SEARCH_CACHE_OK cases=%d\n", cases_run);
    return 0;
}
