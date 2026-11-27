////////////////////////////////////////////////////////////////////////////
//                           **** ADPCM-XQ ****                           //
//                  Xtreme Quality ADPCM Encoder/Decoder                  //
//                    Copyright (c) 2024 David Bryant.                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// Tests for the optional exact-search cache (adpcm-search-cache.c).
//
// This file contains an independent reference implementation of the encoder
// (the ref_* functions), which is a verbatim copy of the original adpcm-lib.c
// search logic from before the cache was introduced, instrumented with
// recursion-entry observation counters. Every test compares the production
// library against this reference path byte-for-byte, and the production
// "expanded" statistic is cross-checked against the reference node count so
// the statistics are provably measured at the real recursion entries.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "adpcm-lib.h"
#include "adpcm-search-cache.h"

/**************************************************************************/
/* Independent reference implementation (copy of the original logic)      */
/**************************************************************************/

typedef uint64_t rms_error_t;
#define REF_MAX_RMS_ERROR UINT64_MAX

#define REF_CLIP(data, min, max) \
if ((data) > (max)) data = max; \
else if ((data) < (min)) data = min;

#define REF_NIBBLE_TO_DELTA(b,n) ((n)<(1<<((b)-1))?(n)+1:(1<<((b)-1))-1-(n))

/* internal to adpcm-lib.c, mirrored here for the reference implementation */
#define NOISE_SHAPING_ENABLED   (NOISE_SHAPING_DYNAMIC | NOISE_SHAPING_STATIC)

static const uint16_t ref_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14,
    16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};

static const int ref_index_table[] = {
    /* adpcm data size is 4 */
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int ref_index_table_3bit[] = {
    /* adpcm data size is 3 */
    -1, -1, 1, 2
};

static const int ref_index_table_5bit[] = {
    /* adpcm data size is 5 */
    -1, -1, -1, -1, -1, -1, -1, -1, 1, 2, 4, 6, 8, 10, 13, 16
};

typedef struct {
    int32_t pcmdata;                        // current PCM value
    int32_t shaping_weight, error;          // for noise shaping
    int8_t index;                           // current index into step size table
} ref_channel;

typedef struct {
    ref_channel channels [2];
    int num_channels, sample_rate, config_flags;
    int16_t *dynamic_shaping_array, last_shaping_weight;
    int static_shaping_weight;
} ref_context;

/* observation counters for the reference recursion */

static uint64_t ref_expanded;                   // recursion entries, total
static uint64_t ref_expanded_phase [2];         // ... split by phase (0 = initial index scan, 1 = encode)
static int ref_phase = 1;

/* re-arrival tracking: records the first max_error seen for each distinct
 * node identity so that "same state reached again with a different threshold"
 * can be counted independently of the production cache
 */

static int ref_track;
static uint64_t ref_revisited [2];              // same-state re-arrivals per phase
static uint64_t ref_revisited_threshold [2];    // ...of which with a different max_error than the first visit
static uint64_t ref_revisited_prev [2];         // ...of which with a different max_error than the previous visit
static uint64_t ref_revisited_leaf0 [2];        // re-arrivals at remaining depth 0
static uint64_t ref_rev_gt [2], ref_rev_lt [2]; // re-arrivals with threshold above/below the first visit
static uint64_t ref_distinct;                   // distinct states visited more than once
static uint64_t ref_distinct_varied;            // distinct states whose threshold varied
static const int16_t *ref_track_base;           // block base for position-keyed tracking
static int ref_track_pos;                       // include the sample position in the identity

typedef struct {
    adpcm_search_key key;
    uint64_t max_error;         // threshold of the first visit
    uint64_t last_max_error;    // threshold of the most recent visit
    int32_t pos;                // sample position (only when ref_track_pos is set)
    uint32_t visits;
    uint8_t used;
    uint8_t varied;             // a re-arrival had a different threshold
} seen_slot;

#define SEEN_SLOTS  (1 << 16)
#define SEEN_MASK   (SEEN_SLOTS - 1)

static seen_slot *seen_table;

static uint64_t test_hash_key (const adpcm_search_key *key)
{
    const uint8_t *bp = (const uint8_t *) key;
    uint64_t hash = 14695981039346656037ULL;
    size_t i;

    for (i = 0; i < sizeof (*key); ++i) {
        hash ^= bp [i];
        hash *= 1099511628211ULL;
    }

    return hash;
}

static void ref_track_node (int bps, const ref_channel *pchan, int nch, int32_t csample, const int16_t *psample, int flags, uint64_t max_error)
{
    adpcm_search_key key;
    seen_slot *slot;
    uint64_t hash;
    int32_t pos = ref_track_pos ? (int32_t) (psample - ref_track_base) : 0;
    int depth = flags & LOOKAHEAD_DEPTH, i;

    memset (&key, 0, sizeof (key));
    key.pcmdata = pchan->pcmdata;
    key.csample = csample;
    key.error = pchan->error;
    key.shaping_weight = pchan->shaping_weight;
    key.flags = flags & (NOISE_SHAPING_ENABLED | LOOKAHEAD_EXHAUSTIVE | LOOKAHEAD_NO_BRANCHING);
    key.index = pchan->index;
    key.depth = depth;
    key.bps = bps;
    key.nch = nch;

    for (i = 0; i < depth; ++i)
        key.samples [i] = psample [(i + 1) * nch];

    hash = test_hash_key (&key) ^ (uint32_t) pos;

    while (1) {
        slot = seen_table + (hash & SEEN_MASK);

        if (!slot->used || (!memcmp (&slot->key, &key, sizeof (key)) && slot->pos == pos))
            break;

        hash++;
    }

    if (!slot->used) {
        slot->used = 1;
        slot->key = key;
        slot->pos = pos;
        slot->max_error = max_error;
        slot->last_max_error = max_error;
        slot->visits = 1;
        slot->varied = 0;
    }
    else {
        if (slot->visits == 1)
            ref_distinct++;

        ref_revisited [ref_phase]++;

        if (!(flags & LOOKAHEAD_DEPTH))
            ref_revisited_leaf0 [ref_phase]++;

        if (slot->max_error != max_error) {
            ref_revisited_threshold [ref_phase]++;

            if (!slot->varied) {
                slot->varied = 1;
                ref_distinct_varied++;
            }
        }

        if (slot->last_max_error != max_error)
            ref_revisited_prev [ref_phase]++;

        if (max_error > slot->max_error)
            ref_rev_gt [ref_phase]++;
        else if (max_error < slot->max_error)
            ref_rev_lt [ref_phase]++;

        slot->last_max_error = max_error;
        slot->visits++;
    }
}

static void ref_reset_tracking (void)
{
    if (!seen_table)
        seen_table = calloc (SEEN_SLOTS, sizeof (seen_slot));
    else
        memset (seen_table, 0, SEEN_SLOTS * sizeof (seen_slot));

    ref_expanded = 0;
    ref_expanded_phase [0] = ref_expanded_phase [1] = 0;
    ref_revisited [0] = ref_revisited [1] = 0;
    ref_revisited_threshold [0] = ref_revisited_threshold [1] = 0;
    ref_revisited_prev [0] = ref_revisited_prev [1] = 0;
    ref_revisited_leaf0 [0] = ref_revisited_leaf0 [1] = 0;
    ref_rev_gt [0] = ref_rev_gt [1] = ref_rev_lt [0] = ref_rev_lt [1] = 0;
    ref_distinct = ref_distinct_varied = 0;
}

static ref_context *ref_create_context (int num_channels, int sample_rate, int lookahead, int noise_shaping)
{
    ref_context *pcnxt = malloc (sizeof (ref_context));
    int ch;

    memset (pcnxt, 0, sizeof (ref_context));
    pcnxt->config_flags = noise_shaping | lookahead;
    pcnxt->static_shaping_weight = 1024;
    pcnxt->num_channels = num_channels;
    pcnxt->sample_rate = sample_rate;

    for (ch = 0; ch < num_channels; ++ch)
        pcnxt->channels [ch].index = -1;

    return pcnxt;
}

static void ref_free_context (ref_context *pcnxt)
{
    free (pcnxt);
}

static void ref_set_shaping_weight (ref_context *pcnxt, double shaping_weight)
{
    pcnxt->static_shaping_weight = (int) floor (shaping_weight * 1024.0 + 0.5);

    if (pcnxt->static_shaping_weight > 1024) pcnxt->static_shaping_weight = 1024;
    if (pcnxt->static_shaping_weight < -1023) pcnxt->static_shaping_weight = -1023;
}

static int32_t ref_noise_shape (ref_channel *pchan, int32_t sample)
{
    int32_t temp = -((pchan->shaping_weight * pchan->error + 512) >> 10);

    if (pchan->shaping_weight < 0 && temp) {
        if (temp == pchan->error)
            temp = (temp < 0) ? temp + 1 : temp - 1;

        pchan->error = -sample;
        sample += temp;
    }
    else
        pchan->error = -(sample += temp);

    return sample;
}

static rms_error_t ref_min_error_4bit (const ref_channel *pchan, int nch, int32_t csample, const int16_t *psample, int flags, int *best_nibble, rms_error_t max_error)
{
    int32_t delta = csample - pchan->pcmdata, csample2;
    ref_channel chan = *pchan;
    uint16_t step = ref_step_table[chan.index];
    uint16_t trial_delta = (step >> 3);
    int nibble, testnbl;
    rms_error_t min_error;

    ref_expanded++;
    ref_expanded_phase [ref_phase]++;
    if (ref_track) ref_track_node (4, pchan, nch, csample, psample, flags, max_error);

    if (delta < 0) {
        int mag = ((-delta << 2) + (step & 3) + ((step & 1) << 1)) / step;
        nibble = 0x8 | (mag > 7 ? 7 : mag);
    }
    else {
        int mag = ((delta << 2) + (step & 3) + ((step & 1) << 1)) / step;
        nibble = mag > 7 ? 7 : mag;
    }

    if (nibble & 1) trial_delta += (step >> 2);
    if (nibble & 2) trial_delta += (step >> 1);
    if (nibble & 4) trial_delta += step;

    if (nibble & 8)
        chan.pcmdata -= trial_delta;
    else
        chan.pcmdata += trial_delta;

    REF_CLIP(chan.pcmdata, -32768, 32767);
    if (best_nibble) *best_nibble = nibble;
    min_error = (rms_error_t) (chan.pcmdata - csample) * (chan.pcmdata - csample);

    if (!(flags & LOOKAHEAD_DEPTH) || min_error >= max_error)
        return min_error;

    chan.index += ref_index_table[nibble & 0x07];
    REF_CLIP(chan.index, 0, 88);

    if (flags & NOISE_SHAPING_ENABLED) {
        chan.error += chan.pcmdata;
        csample2 = ref_noise_shape (&chan, psample [nch]);
    }
    else
        csample2 = psample [nch];

    min_error += ref_min_error_4bit (&chan, nch, csample2, psample + nch, flags - 1, NULL, max_error - min_error);

    if (flags & LOOKAHEAD_NO_BRANCHING)
        return min_error;

    for (testnbl = 0; testnbl <= 0xF; ++testnbl) {
        rms_error_t error, threshold;

        if (testnbl == nibble)
            continue;

        if (flags & LOOKAHEAD_EXHAUSTIVE || !(~testnbl & 0x7) || abs (REF_NIBBLE_TO_DELTA (4,nibble) - REF_NIBBLE_TO_DELTA (4,testnbl)) <= 3) {
            trial_delta = (step >> 3);
            chan = *pchan;

            if (testnbl & 1) trial_delta += (step >> 2);
            if (testnbl & 2) trial_delta += (step >> 1);
            if (testnbl & 4) trial_delta += step;

            if (testnbl & 8)
                chan.pcmdata -= trial_delta;
            else
                chan.pcmdata += trial_delta;

            REF_CLIP(chan.pcmdata, -32768, 32767);

            error = (rms_error_t) (chan.pcmdata - csample) * (chan.pcmdata - csample);
            threshold = max_error < min_error ? max_error : min_error;

            if (error < threshold) {
                chan.index += ref_index_table[testnbl & 0x07];
                REF_CLIP(chan.index, 0, 88);

                if (flags & NOISE_SHAPING_ENABLED) {
                    chan.error += chan.pcmdata;
                    csample2 = ref_noise_shape (&chan, psample [nch]);
                }
                else
                    csample2 = psample [nch];

                error += ref_min_error_4bit (&chan, nch, csample2, psample + nch, flags - 1, NULL, threshold - error);

                if (error < min_error) {
                    if (best_nibble) *best_nibble = testnbl;
                    min_error = error;
                }
            }
        }
    }

    return min_error;
}

static rms_error_t ref_min_error_2bit (const ref_channel *pchan, int nch, int32_t csample, const int16_t *psample, int flags, int *best_nibble, rms_error_t max_error)
{
    int32_t delta = csample - pchan->pcmdata, csample2;
    ref_channel chan = *pchan;
    uint16_t step = ref_step_table[chan.index];
    int nibble, testnbl;
    rms_error_t min_error;

    ref_expanded++;
    ref_expanded_phase [ref_phase]++;
    if (ref_track) ref_track_node (2, pchan, nch, csample, psample, flags, max_error);

    if (delta < 0) {
        if (-delta >= step) {
            chan.pcmdata -= step + (step >> 1);
            nibble = 3;
        }
        else {
            chan.pcmdata -= step >> 1;
            nibble = 2;
        }
    }
    else
        chan.pcmdata += step * ((nibble = delta >= step)) + (step >> 1);

    REF_CLIP(chan.pcmdata, -32768, 32767);
    if (best_nibble) *best_nibble = nibble;
    min_error = (rms_error_t) (chan.pcmdata - csample) * (chan.pcmdata - csample);

    if (!(flags & LOOKAHEAD_DEPTH) || min_error >= max_error)
        return min_error;

    chan.index += (nibble & 1) * 3 - 1;
    REF_CLIP(chan.index, 0, 88);

    if (flags & NOISE_SHAPING_ENABLED) {
        chan.error += chan.pcmdata;
        csample2 = ref_noise_shape (&chan, psample [nch]);
    }
    else
        csample2 = psample [nch];

    min_error += ref_min_error_2bit (&chan, nch, csample2, psample + nch, flags - 1, NULL, max_error - min_error);

    if (flags & LOOKAHEAD_NO_BRANCHING)
        return min_error;

    for (testnbl = 0; testnbl <= 0x3; ++testnbl) {
        rms_error_t error, threshold;

        if (testnbl == nibble)
            continue;

        chan = *pchan;

        if (testnbl & 2)
            chan.pcmdata -= step * (testnbl & 1) + (step >> 1);
        else
            chan.pcmdata += step * (testnbl & 1) + (step >> 1);

        REF_CLIP(chan.pcmdata, -32768, 32767);

        error = (rms_error_t) (chan.pcmdata - csample) * (chan.pcmdata - csample);
        threshold = max_error < min_error ? max_error : min_error;

        if (error < threshold) {
            chan.index += (testnbl & 1) * 3 - 1;
            REF_CLIP(chan.index, 0, 88);

            if (flags & NOISE_SHAPING_ENABLED) {
                chan.error += chan.pcmdata;
                csample2 = ref_noise_shape (&chan, psample [nch]);
            }
            else
                csample2 = psample [nch];

            error += ref_min_error_2bit (&chan, nch, csample2, psample + nch, flags - 1, NULL, threshold - error);

            if (error < min_error) {
                if (best_nibble) *best_nibble = testnbl;
                min_error = error;
            }
        }
    }

    return min_error;
}

static rms_error_t ref_min_error_3bit (const ref_channel *pchan, int nch, int32_t csample, const int16_t *psample, int flags, int *best_nibble, rms_error_t max_error)
{
    int32_t delta = csample - pchan->pcmdata, csample2;
    ref_channel chan = *pchan;
    uint16_t step = ref_step_table[chan.index];
    uint16_t trial_delta = (step >> 2);
    int nibble, testnbl;
    rms_error_t min_error;

    ref_expanded++;
    ref_expanded_phase [ref_phase]++;
    if (ref_track) ref_track_node (3, pchan, nch, csample, psample, flags, max_error);

    if (delta < 0) {
        int mag = ((-delta << 1) + (step & 1)) / step;
        nibble = 0x4 | (mag > 3 ? 3 : mag);
    }
    else {
        int mag = ((delta << 1) + (step & 1)) / step;
        nibble = mag > 3 ? 3 : mag;
    }

    if (nibble & 1) trial_delta += (step >> 1);
    if (nibble & 2) trial_delta += step;

    if (nibble & 4)
        chan.pcmdata -= trial_delta;
    else
        chan.pcmdata += trial_delta;

    REF_CLIP(chan.pcmdata, -32768, 32767);
    if (best_nibble) *best_nibble = nibble;
    min_error = (rms_error_t) (chan.pcmdata - csample) * (chan.pcmdata - csample);

    if (!(flags & LOOKAHEAD_DEPTH) || min_error >= max_error)
        return min_error;

    chan.index += ref_index_table_3bit[nibble & 0x03];
    REF_CLIP(chan.index, 0, 88);

    if (flags & NOISE_SHAPING_ENABLED) {
        chan.error += chan.pcmdata;
        csample2 = ref_noise_shape (&chan, psample [nch]);
    }
    else
        csample2 = psample [nch];

    min_error += ref_min_error_3bit (&chan, nch, csample2, psample + nch, flags - 1, NULL, max_error - min_error);

    if (flags & LOOKAHEAD_NO_BRANCHING)
        return min_error;

    for (testnbl = 0; testnbl <= 0x7; ++testnbl) {
        rms_error_t error, threshold;

        if (testnbl == nibble)
            continue;

        if (flags & LOOKAHEAD_EXHAUSTIVE || !(~testnbl & 0x3) || abs (REF_NIBBLE_TO_DELTA (3,nibble) - REF_NIBBLE_TO_DELTA (3,testnbl)) <= 2) {
            trial_delta = (step >> 2);
            chan = *pchan;

            if (testnbl & 1) trial_delta += (step >> 1);
            if (testnbl & 2) trial_delta += step;

            if (testnbl & 4)
                chan.pcmdata -= trial_delta;
            else
                chan.pcmdata += trial_delta;

            REF_CLIP(chan.pcmdata, -32768, 32767);
            error = (rms_error_t) (chan.pcmdata - csample) * (chan.pcmdata - csample);
            threshold = max_error < min_error ? max_error : min_error;

            if (error < threshold) {
                chan.index += ref_index_table_3bit[testnbl & 0x03];
                REF_CLIP(chan.index, 0, 88);

                if (flags & NOISE_SHAPING_ENABLED) {
                    chan.error += chan.pcmdata;
                    csample2 = ref_noise_shape (&chan, psample [nch]);
                }
                else
                    csample2 = psample [nch];

                error += ref_min_error_3bit (&chan, nch, csample2, psample + nch, flags - 1, NULL, threshold - error);

                if (error < min_error) {
                    if (best_nibble) *best_nibble = testnbl;
                    min_error = error;
                }
            }
        }
    }

    return min_error;
}

static rms_error_t ref_min_error_5bit (const ref_channel *pchan, int nch, int32_t csample, const int16_t *psample, int flags, int *best_nibble, rms_error_t max_error)
{
    static char comp_table [16] = { 0, 0, 0, 5, 0, 6, 4, 10, 0, 7, 6, 10, 4, 11, 11, 13 };
    int32_t delta = csample - pchan->pcmdata, csample2;
    ref_channel chan = *pchan;
    uint16_t step = ref_step_table[chan.index];
    uint16_t trial_delta = (step >> 4);
    int nibble, testnbl;
    rms_error_t min_error;

    ref_expanded++;
    ref_expanded_phase [ref_phase]++;
    if (ref_track) ref_track_node (5, pchan, nch, csample, psample, flags, max_error);

    if (delta < 0) {
        int mag = ((-delta << 3) + comp_table [step & 0xf]) / step;
        nibble = 0x10 | (mag > 0xf ? 0xf : mag);
    }
    else {
        int mag = ((delta << 3) + comp_table [step & 0xf]) / step;
        nibble = mag > 0xf ? 0xf : mag;
    }

    if (nibble & 1) trial_delta += (step >> 3);
    if (nibble & 2) trial_delta += (step >> 2);
    if (nibble & 4) trial_delta += (step >> 1);
    if (nibble & 8) trial_delta += step;

    if (nibble & 0x10)
        chan.pcmdata -= trial_delta;
    else
        chan.pcmdata += trial_delta;

    REF_CLIP(chan.pcmdata, -32768, 32767);
    if (best_nibble) *best_nibble = nibble;
    min_error = (rms_error_t) (chan.pcmdata - csample) * (chan.pcmdata - csample);

    if (!(flags & LOOKAHEAD_DEPTH) || min_error >= max_error)
        return min_error;

    chan.index += ref_index_table_5bit[nibble & 0x0f];
    REF_CLIP(chan.index, 0, 88);

    if (flags & NOISE_SHAPING_ENABLED) {
        chan.error += chan.pcmdata;
        csample2 = ref_noise_shape (&chan, psample [nch]);
    }
    else
        csample2 = psample [nch];

    min_error += ref_min_error_5bit (&chan, nch, csample2, psample + nch, flags - 1, NULL, max_error - min_error);

    if (flags & LOOKAHEAD_NO_BRANCHING)
        return min_error;

    for (testnbl = 0; testnbl <= 0x1F; ++testnbl) {
        rms_error_t error, threshold;

        if (testnbl == nibble)
            continue;

        if (flags & LOOKAHEAD_EXHAUSTIVE || (testnbl | 3) == (nibble | 0xf) || abs (REF_NIBBLE_TO_DELTA (5,nibble) - REF_NIBBLE_TO_DELTA (5,testnbl)) <= 3) {
            trial_delta = (step >> 4);
            chan = *pchan;

            if (testnbl & 1) trial_delta += (step >> 3);
            if (testnbl & 2) trial_delta += (step >> 2);
            if (testnbl & 4) trial_delta += (step >> 1);
            if (testnbl & 8) trial_delta += step;

            if (testnbl & 0x10)
                chan.pcmdata -= trial_delta;
            else
                chan.pcmdata += trial_delta;

            REF_CLIP(chan.pcmdata, -32768, 32767);

            error = (rms_error_t) (chan.pcmdata - csample) * (chan.pcmdata - csample);
            threshold = max_error < min_error ? max_error : min_error;

            if (error < threshold) {
                chan.index += ref_index_table_5bit [testnbl & 0x0f];
                REF_CLIP(chan.index, 0, 88);

                if (flags & NOISE_SHAPING_ENABLED) {
                    chan.error += chan.pcmdata;
                    csample2 = ref_noise_shape (&chan, psample [nch]);
                }
                else
                    csample2 = psample [nch];

                error += ref_min_error_5bit (&chan, nch, csample2, psample + nch, flags - 1, NULL, threshold - error);

                if (error < min_error) {
                    if (best_nibble) *best_nibble = testnbl;
                    min_error = error;
                }
            }
        }
    }

    return min_error;
}

static uint8_t ref_encode_sample (ref_context *pcnxt, int ch, int bps, const int16_t *psample, int num_samples)
{
    ref_channel *pchan = pcnxt->channels + ch;
    uint16_t step = ref_step_table[pchan->index];
    int flags = pcnxt->config_flags, nibble;
    int32_t csample = *psample;
    uint16_t trial_delta;

    if (flags & NOISE_SHAPING_ENABLED)
        csample = ref_noise_shape (pchan, csample);

    if ((flags & LOOKAHEAD_DEPTH) > num_samples - 1)
        flags = (flags & ~LOOKAHEAD_DEPTH) + num_samples - 1;

    if (bps == 2) {
        ref_min_error_2bit (pchan, pcnxt->num_channels, csample, psample, flags, &nibble, REF_MAX_RMS_ERROR);

        if (nibble & 2)
            pchan->pcmdata -= step * (nibble & 1) + (step >> 1);
        else
            pchan->pcmdata += step * (nibble & 1) + (step >> 1);

        pchan->index += (nibble & 1) * 3 - 1;
    }
    else if (bps == 3) {
        ref_min_error_3bit (pchan, pcnxt->num_channels, csample, psample, flags, &nibble, REF_MAX_RMS_ERROR);
        trial_delta = (step >> 2);
        if (nibble & 1) trial_delta += (step >> 1);
        if (nibble & 2) trial_delta += step;

        if (nibble & 4)
            pchan->pcmdata -= trial_delta;
        else
            pchan->pcmdata += trial_delta;

        pchan->index += ref_index_table_3bit[nibble & 0x03];
    }
    else if (bps == 4) {
        ref_min_error_4bit (pchan, pcnxt->num_channels, csample, psample, flags, &nibble, REF_MAX_RMS_ERROR);
        trial_delta = (step >> 3);
        if (nibble & 1) trial_delta += (step >> 2);
        if (nibble & 2) trial_delta += (step >> 1);
        if (nibble & 4) trial_delta += step;

        if (nibble & 8)
            pchan->pcmdata -= trial_delta;
        else
            pchan->pcmdata += trial_delta;

        pchan->index += ref_index_table[nibble & 0x07];
    }
    else {  // bps == 5
        ref_min_error_5bit (pchan, pcnxt->num_channels, csample, psample, flags, &nibble, REF_MAX_RMS_ERROR);
        trial_delta = (step >> 4);
        if (nibble & 1) trial_delta += (step >> 3);
        if (nibble & 2) trial_delta += (step >> 2);
        if (nibble & 4) trial_delta += (step >> 1);
        if (nibble & 8) trial_delta += step;

        if (nibble & 0x10)
            pchan->pcmdata -= trial_delta;
        else
            pchan->pcmdata += trial_delta;

        pchan->index += ref_index_table_5bit[nibble & 0x0f];
    }

    REF_CLIP(pchan->index, 0, 88);
    REF_CLIP(pchan->pcmdata, -32768, 32767);

    if (flags & NOISE_SHAPING_ENABLED)
        pchan->error += pchan->pcmdata;

    return nibble;
}

static void ref_encode_chunks (ref_context *pcnxt, uint8_t *outbuf, size_t *outbufsize, const int16_t *inbuf, int inbufcount, int bps)
{
    const int16_t *pcmbuf;
    int ch;

    for (ch = 0; ch < pcnxt->num_channels; ++ch) {
        int shiftbits = 0, numbits = 0, i, j;

        if (pcnxt->config_flags & NOISE_SHAPING_STATIC)
            pcnxt->channels [ch].shaping_weight = pcnxt->static_shaping_weight;

        pcmbuf = inbuf + ch;

        for (j = i = 0; i < inbufcount; ++i) {
            if (pcnxt->config_flags & NOISE_SHAPING_DYNAMIC)
                pcnxt->channels [ch].shaping_weight = pcnxt->dynamic_shaping_array [i];

            shiftbits |= ref_encode_sample (pcnxt, ch, bps, pcmbuf, inbufcount - i) << numbits;
            pcmbuf += pcnxt->num_channels;

            if ((numbits += bps) >= 8) {
                outbuf [(j & ~3) * pcnxt->num_channels + (ch * 4) + (j & 3)] = shiftbits;
                shiftbits >>= 8;
                numbits -= 8;
                j++;
            }
        }

        if (numbits)
            outbuf [(j & ~3) * pcnxt->num_channels + (ch * 4) + (j & 3)] = shiftbits;
    }

    *outbufsize += (inbufcount * bps + 31) / 32 * pcnxt->num_channels * 4;
}

static int ref_encode_block_ex (ref_context *pcnxt, uint8_t *outbuf, size_t *outbufsize, const int16_t *inbuf, int inbufcount, int bps)
{
    int ch;

    *outbufsize = 0;

    if (bps < 2 || bps > 5)
        return 0;

    if (!inbufcount)
        return 1;

    for (ch = 0; ch < pcnxt->num_channels; ch++)
        pcnxt->channels[ch].pcmdata = *inbuf++;

    inbufcount--;

    if (inbufcount && (pcnxt->channels [0].index < 0 || (pcnxt->config_flags & LOOKAHEAD_DEPTH) >= 3)) {
        int flags = 16 | LOOKAHEAD_NO_BRANCHING;

        if ((flags & LOOKAHEAD_DEPTH) > inbufcount - 1)
            flags = (flags & ~LOOKAHEAD_DEPTH) + inbufcount - 1;

        ref_phase = 0;

        for (ch = 0; ch < pcnxt->num_channels; ch++) {
            rms_error_t min_error = REF_MAX_RMS_ERROR;
            rms_error_t error_per_index [89];
            int best_index = 0, tindex;

            for (tindex = 0; tindex <= 88; tindex++) {
                ref_channel chan = pcnxt->channels [ch];

                chan.index = tindex;
                chan.shaping_weight = 0;

                if (bps == 2)
                    error_per_index [tindex] = ref_min_error_2bit (&chan, pcnxt->num_channels, inbuf [ch], inbuf + ch, flags, NULL, REF_MAX_RMS_ERROR);
                else if (bps == 3)
                    error_per_index [tindex] = ref_min_error_3bit (&chan, pcnxt->num_channels, inbuf [ch], inbuf + ch, flags, NULL, REF_MAX_RMS_ERROR);
                else if (bps == 5)
                    error_per_index [tindex] = ref_min_error_5bit (&chan, pcnxt->num_channels, inbuf [ch], inbuf + ch, flags, NULL, REF_MAX_RMS_ERROR);
                else
                    error_per_index [tindex] = ref_min_error_4bit (&chan, pcnxt->num_channels, inbuf [ch], inbuf + ch, flags, NULL, REF_MAX_RMS_ERROR);
            }

            for (tindex = 0; tindex <= 87; tindex++) {
                rms_error_t terror = error_per_index [tindex];

                if (tindex)
                    terror = (error_per_index [tindex - 1] + terror + error_per_index [tindex + 1]) / 3;

                if (terror < min_error) {
                    best_index = tindex;
                    min_error = terror;
                }
            }

            pcnxt->channels [ch].index = best_index;
        }

        ref_phase = 1;
    }

    for (ch = 0; ch < pcnxt->num_channels; ch++) {
        outbuf[0] = pcnxt->channels[ch].pcmdata;
        outbuf[1] = pcnxt->channels[ch].pcmdata >> 8;
        outbuf[2] = pcnxt->channels[ch].index;
        outbuf[3] = 0;

        outbuf += 4;
        *outbufsize += 4;
    }

    if (inbufcount && (pcnxt->config_flags & NOISE_SHAPING_DYNAMIC)) {
        pcnxt->dynamic_shaping_array = malloc (inbufcount * sizeof (int16_t));
        generate_dns_values (inbuf, inbufcount, pcnxt->num_channels, pcnxt->sample_rate, pcnxt->dynamic_shaping_array, -512, pcnxt->last_shaping_weight);
        pcnxt->last_shaping_weight = pcnxt->dynamic_shaping_array [inbufcount - 1];
    }

    if (inbufcount)
        ref_encode_chunks (pcnxt, outbuf, outbufsize, inbuf, inbufcount, bps);

    if (pcnxt->dynamic_shaping_array && (pcnxt->config_flags & NOISE_SHAPING_DYNAMIC)) {
        free (pcnxt->dynamic_shaping_array);
        pcnxt->dynamic_shaping_array = NULL;
    }

    return 1;
}

/**************************************************************************/
/* test harness                                                           */
/**************************************************************************/

static int failures = 0;
static int cases = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf ("FAIL line %d: ", __LINE__); \
        printf (__VA_ARGS__); \
        printf ("\n"); \
        failures++; \
    } \
} while (0)

#define MAX_BLOCK_SAMPLES   4096
#define OUTBUF_SIZE         32768
#define PCM_SIZE            (300000)

static int16_t pcm [PCM_SIZE];
static uint8_t prod_buf [OUTBUF_SIZE], ref_buf [OUTBUF_SIZE];

static uint32_t rnd_state = 0x12345678;

static uint32_t next_random (void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

/* deterministic mixes of sines, noise and steps, different per channel */

static void gen_audio (int16_t *dst, int frames, int nch, int flavor)
{
    int i, ch;

    for (i = 0; i < frames; ++i)
        for (ch = 0; ch < nch; ++ch) {
            double v = 0.0;

            switch (flavor) {
                case 0:     // smooth sines
                    v = 9000.0 * sin (0.05 * i + ch * 1.3) + 5000.0 * sin (0.013 * i + ch * 0.7) + 2000.0 * sin (0.31 * i);
                    break;

                case 1:     // sines plus noise
                    v = 7000.0 * sin (0.11 * i + ch) + ((int) (next_random () & 0x3fff) - 8192);
                    break;

                case 2:     // full-scale noise
                    v = (double) (int16_t) (next_random () & 0xffff) * 0.5;
                    break;

                case 3:     // steps with ringing (exercises predictor clipping)
                    v = ((i / 37 + ch) & 1) ? 28000.0 : -28000.0;
                    v += 3000.0 * sin (0.9 * i);
                    break;
            }

            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            dst [i * nch + ch] = (int16_t) v;
        }
}

typedef struct {
    int num_channels, sample_rate, lookahead, noise_shaping, bps;
} tcfg;

/* Encode num_blocks blocks with both the production library and the
 * independent reference, comparing every output byte and block size. When
 * the cache is disabled the production "expanded" statistic must exactly
 * equal the reference recursion-entry count; when enabled it can only be
 * smaller (cache hits skip nodes). Returns nonzero on success.
 */

static int check_stream (const tcfg *cfg, const int16_t *pcmdata, int block_samples, int num_blocks, int cache_enabled, const char *tag)
{
    void *prod = adpcm_create_context (cfg->num_channels, cfg->sample_rate, cfg->lookahead, cfg->noise_shaping);
    ref_context *ref = ref_create_context (cfg->num_channels, cfg->sample_rate, cfg->lookahead, cfg->noise_shaping);
    int ok = 1, b;

    if (cache_enabled && adpcm_set_search_cache (prod, 1) != 1) {
        printf ("FAIL: adpcm_set_search_cache() failed for %s\n", tag);
        adpcm_free_context (prod);
        ref_free_context (ref);
        return 0;
    }

    for (b = 0; b < num_blocks && ok; ++b) {
        const int16_t *in = pcmdata + (size_t) b * block_samples * cfg->num_channels;
        uint64_t ref_before = ref_expanded, ref_nodes;
        adpcm_search_stats st;
        size_t psz = 0, rsz = 0;

        if (adpcm_encode_block_ex (prod, prod_buf, &psz, in, block_samples, cfg->bps) != 1) {
            printf ("FAIL: production encode failed for %s block %d\n", tag, b);
            ok = 0;
            break;
        }

        if (ref_encode_block_ex (ref, ref_buf, &rsz, in, block_samples, cfg->bps) != 1) {
            printf ("FAIL: reference encode failed for %s block %d\n", tag, b);
            ok = 0;
            break;
        }

        ref_nodes = ref_expanded - ref_before;

        if (psz != rsz || memcmp (prod_buf, ref_buf, psz)) {
            printf ("FAIL: byte mismatch for %s block %d (prod %zu bytes, ref %zu bytes)\n", tag, b, psz, rsz);
            ok = 0;
        }

        if (adpcm_get_search_stats (prod, &st) != 1) {
            printf ("FAIL: adpcm_get_search_stats() failed for %s\n", tag);
            ok = 0;
        }
        else if (!cache_enabled) {
            if (st.expanded != ref_nodes || st.hits != 0) {
                printf ("FAIL: stats mismatch for %s block %d: expanded %llu (ref %llu), hits %llu\n",
                    tag, b, (unsigned long long) st.expanded, (unsigned long long) ref_nodes, (unsigned long long) st.hits);
                ok = 0;
            }
        }
        else if (st.expanded > ref_nodes) {
            printf ("FAIL: cache expanded more nodes than reference for %s block %d: %llu > %llu\n",
                tag, b, (unsigned long long) st.expanded, (unsigned long long) ref_nodes);
            ok = 0;
        }
    }

    adpcm_free_context (prod);
    ref_free_context (ref);
    return ok;
}

/**************************************************************************/
/* individual tests                                                       */
/**************************************************************************/

/* The documented repeated-computation case: mono 16 kHz, 65 frames of
 * alternating full-scale samples, lookahead 5 | LOOKAHEAD_EXHAUSTIVE, no
 * noise shaping. The original search enters 1888 nodes and the same states
 * are reached again with different pruning thresholds.
 */

static void test_repro_case (void)
{
    int16_t alt [130];
    size_t rsz1 = 0, rsz2 = 0, psz = 0;
    uint8_t ref1 [256], ref2 [256], prod1 [256], prod2 [256];
    adpcm_search_stats st;
    uint64_t expanded_off, pos_revisited, pos_threshold;
    ref_context *ref;
    void *prod;
    int i;

    for (i = 0; i < 130; ++i)
        alt [i] = (i & 1) ? 32767 : -32768;

    // reference pass with re-arrival tracking (independent observation point)

    ref_reset_tracking ();
    ref_track = 1;
    ref = ref_create_context (1, 16000, 5 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_OFF);
    ref_encode_block_ex (ref, ref1, &rsz1, alt, 65, 4);
    ref_track = 0;

    printf ("repro: reference expanded=%llu (init %llu + encode %llu), revisited=%llu with new threshold=%llu\n",
        (unsigned long long) ref_expanded,
        (unsigned long long) ref_expanded_phase [0], (unsigned long long) ref_expanded_phase [1],
        (unsigned long long) (ref_revisited [0] + ref_revisited [1]),
        (unsigned long long) (ref_revisited_threshold [0] + ref_revisited_threshold [1]));

    printf ("repro detail: revisited init=%llu encode=%llu, vs-first init=%llu encode=%llu, vs-prev init=%llu encode=%llu, gt=%llu lt=%llu, leaf0=%llu, distinct=%llu varied=%llu\n",
        (unsigned long long) ref_revisited [0], (unsigned long long) ref_revisited [1],
        (unsigned long long) ref_revisited_threshold [0], (unsigned long long) ref_revisited_threshold [1],
        (unsigned long long) ref_revisited_prev [0], (unsigned long long) ref_revisited_prev [1],
        (unsigned long long) (ref_rev_gt [0] + ref_rev_gt [1]), (unsigned long long) (ref_rev_lt [0] + ref_rev_lt [1]),
        (unsigned long long) (ref_revisited_leaf0 [0] + ref_revisited_leaf0 [1]),
        (unsigned long long) ref_distinct, (unsigned long long) ref_distinct_varied);

    // same measurement with the sample position included in the identity;
    // this reproduces the documented 403 re-arrivals, all with a changed
    // pruning threshold

    ref_reset_tracking ();
    ref_track = 1;
    ref_track_pos = 1;
    ref_track_base = alt;
    {
        ref_context *refp = ref_create_context (1, 16000, 5 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_OFF);
        size_t psz2 = 0;
        uint8_t tmp [256];

        ref_encode_block_ex (refp, tmp, &psz2, alt, 65, 4);
        ref_free_context (refp);
    }
    ref_track = 0;
    ref_track_pos = 0;

    pos_revisited = ref_revisited [0] + ref_revisited [1];
    pos_threshold = ref_revisited_threshold [0] + ref_revisited_threshold [1];

    printf ("repro pos-keyed: revisited=%llu with new threshold=%llu (init %llu encode %llu)\n",
        (unsigned long long) pos_revisited, (unsigned long long) pos_threshold,
        (unsigned long long) ref_revisited_threshold [0], (unsigned long long) ref_revisited_threshold [1]);

    CHECK (pos_revisited == 403, "position-keyed re-arrivals %llu != 403", (unsigned long long) pos_revisited);
    CHECK (pos_threshold == 403, "position-keyed threshold-changed re-arrivals %llu != 403",
        (unsigned long long) pos_threshold);

    CHECK (ref_expanded == 1888, "reference node count %llu != 1888", (unsigned long long) ref_expanded);
    CHECK (ref_revisited [0] + ref_revisited [1] > 0, "no same-state re-arrivals observed");
    CHECK (ref_revisited_threshold [0] + ref_revisited_threshold [1] > 0, "no threshold-changed re-arrivals observed");

    // production, cache disabled: must match the reference exactly

    prod = adpcm_create_context (1, 16000, 5 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_OFF);
    CHECK (adpcm_encode_block (prod, prod1, &psz, alt, 65) == 1, "production encode failed");
    CHECK (psz == rsz1 && !memcmp (prod1, ref1, psz), "cache-off bytes differ from reference");
    CHECK (adpcm_get_search_stats (prod, &st) == 1, "get_search_stats failed");
    CHECK (st.expanded == 1888 && st.hits == 0, "cache-off stats %llu/%llu != 1888/0",
        (unsigned long long) st.expanded, (unsigned long long) st.hits);
    expanded_off = st.expanded;

    // production, cache enabled on the same context (toggle between blocks):
    // identical bytes, at least 10% fewer expanded nodes, and real hits

    CHECK (adpcm_set_search_cache (prod, 1) == 1, "set_search_cache(1) failed");
    CHECK (adpcm_encode_block (prod, prod2, &psz, alt, 65) == 1, "cached encode failed");
    CHECK (psz == rsz1 && !memcmp (prod2, ref1, psz), "cache-on bytes differ from reference");
    CHECK (adpcm_get_search_stats (prod, &st) == 1, "get_search_stats failed");

    printf ("repro: cache cut expanded from %llu to %llu with %llu hits\n",
        (unsigned long long) expanded_off, (unsigned long long) st.expanded, (unsigned long long) st.hits);

    CHECK (st.hits > 0, "no cache hits in repro case");
    CHECK (st.expanded * 10 <= expanded_off * 9, "expanded dropped less than 10%%: %llu -> %llu",
        (unsigned long long) expanded_off, (unsigned long long) st.expanded);

    // a second, consecutive block on both encoders verifies that the encoding
    // history carried across blocks is identical with the cache on

    CHECK (adpcm_encode_block (prod, prod2, &psz, alt + 65, 65) == 1, "second cached encode failed");
    ref_encode_block_ex (ref, ref2, &rsz2, alt + 65, 65, 4);
    CHECK (psz == rsz2 && !memcmp (prod2, ref2, psz), "second-block bytes (carried history) differ");

    adpcm_free_context (prod);
    ref_free_context (ref);
    cases++;
}

/* matrix of code widths, noise shaping modes and depths, two blocks each */

static void test_matrix (void)
{
    static const int depths [] = { 0, 1, 3, 5 | LOOKAHEAD_EXHAUSTIVE, 16 };
    static const int shapings [] = { NOISE_SHAPING_OFF, NOISE_SHAPING_STATIC, NOISE_SHAPING_DYNAMIC };
    int bps, di, si;

    gen_audio (pcm, 2 * 257, 2, 1);

    for (bps = 2; bps <= 5; ++bps)
        for (di = 0; di < 5; ++di)
            for (si = 0; si < 3; ++si) {
                tcfg cfg = { 2, 22050, depths [di], shapings [si], bps };
                char tag [80];

                // depth 16 on a non-exhaustive search is the slowest; keep it short
                int samples = (depths [di] == 16) ? 129 : 257;

                snprintf (tag, sizeof (tag), "matrix bps=%d depth=0x%x shaping=0x%x", bps, depths [di], shapings [si]);

                if (check_stream (&cfg, pcm, samples, 2, 0, tag))
                    cases++;

                if (check_stream (&cfg, pcm, samples, 2, 1, tag))
                    cases++;
            }
}

/* custom (non-default) static shaping weights, including negative */

static void test_shaping_weights (void)
{
    static const double weights [] = { 0.5, -0.5, 1.0 };
    int wi;

    gen_audio (pcm, 2 * 257, 1, 3);

    for (wi = 0; wi < 3; ++wi) {
        tcfg cfg = { 1, 16000, 5 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_STATIC, 4 };
        void *prod;
        ref_context *ref;
        size_t psz = 0, rsz = 0;
        int b, ok = 1;

        prod = adpcm_create_context (cfg.num_channels, cfg.sample_rate, cfg.lookahead, cfg.noise_shaping);
        ref = ref_create_context (cfg.num_channels, cfg.sample_rate, cfg.lookahead, cfg.noise_shaping);
        adpcm_set_shaping_weight (prod, weights [wi]);
        ref_set_shaping_weight (ref, weights [wi]);
        adpcm_set_search_cache (prod, 1);

        for (b = 0; b < 2 && ok; ++b) {
            const int16_t *in = pcm + b * 257;

            adpcm_encode_block_ex (prod, prod_buf, &psz, in, 257, cfg.bps);
            ref_encode_block_ex (ref, ref_buf, &rsz, in, 257, cfg.bps);

            if (psz != rsz || memcmp (prod_buf, ref_buf, psz)) {
                printf ("FAIL: shaping weight %f block %d bytes differ\n", weights [wi], b);
                ok = 0;
            }
        }

        adpcm_free_context (prod);
        ref_free_context (ref);

        if (ok)
            cases++;
    }
}

/* very short blocks ("short tail") where the lookahead is clamped, plus the
 * empty block and invalid arguments
 */

static void test_short_tail (void)
{
    static const int counts [] = { 1, 2, 3, 9, 33 };
    static const int depths [] = { 16, 5 | LOOKAHEAD_EXHAUSTIVE };
    static const int shapings [] = { NOISE_SHAPING_OFF, NOISE_SHAPING_DYNAMIC };
    int ci, di, si;

    gen_audio (pcm, 64, 1, 0);

    for (ci = 0; ci < 5; ++ci)
        for (di = 0; di < 2; ++di)
            for (si = 0; si < 2; ++si) {
                tcfg cfg = { 1, 8000, depths [di], shapings [si], 4 };
                char tag [80];

                snprintf (tag, sizeof (tag), "short n=%d depth=0x%x shaping=0x%x", counts [ci], depths [di], shapings [si]);

                if (check_stream (&cfg, pcm, counts [ci], 1, 0, tag))
                    cases++;

                if (check_stream (&cfg, pcm, counts [ci], 1, 1, tag))
                    cases++;
            }

    // empty block: succeeds, writes nothing, statistics are reset

    {
        void *prod = adpcm_create_context (1, 8000, 5, NOISE_SHAPING_OFF);
        adpcm_search_stats st;
        size_t sz = 123;

        adpcm_set_search_cache (prod, 1);
        CHECK (adpcm_encode_block_ex (prod, prod_buf, &sz, pcm, 0, 4) == 1, "empty block failed");
        CHECK (sz == 0, "empty block wrote %zu bytes", sz);
        CHECK (adpcm_get_search_stats (prod, &st) == 1 && st.expanded == 0 && st.hits == 0,
            "empty block stats not zero");
        CHECK (adpcm_encode_block_ex (prod, prod_buf, &sz, pcm, 16, 1) == 0, "bps=1 accepted");
        CHECK (adpcm_encode_block_ex (prod, prod_buf, &sz, pcm, 16, 6) == 0, "bps=6 accepted");
        adpcm_free_context (prod);
        cases++;
    }
}

/* two encoder instances working interleaved must not share search results */

static void test_interleaved_instances (void)
{
    tcfg cfg1 = { 1, 16000, 5 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_OFF, 4 };
    tcfg cfg2 = { 2, 44100, 3, NOISE_SHAPING_DYNAMIC, 3 };
    void *p1 = adpcm_create_context (cfg1.num_channels, cfg1.sample_rate, cfg1.lookahead, cfg1.noise_shaping);
    void *p2 = adpcm_create_context (cfg2.num_channels, cfg2.sample_rate, cfg2.lookahead, cfg2.noise_shaping);
    ref_context *r1 = ref_create_context (cfg1.num_channels, cfg1.sample_rate, cfg1.lookahead, cfg1.noise_shaping);
    ref_context *r2 = ref_create_context (cfg2.num_channels, cfg2.sample_rate, cfg2.lookahead, cfg2.noise_shaping);
    int ok = 1, b;

    adpcm_set_search_cache (p1, 1);     // p1 cached, p2 not: statistics must stay separate
    gen_audio (pcm, 8 * 200 * 2, 2, 1);

    for (b = 0; b < 8 && ok; ++b) {
        const int16_t *in1 = pcm + b * 200 * 2;         // use the left channel for p1
        int16_t mono [200];
        adpcm_search_stats st1, st2;
        uint64_t ref1_before, ref2_before;
        size_t pz1 = 0, pz2 = 0, rz1 = 0, rz2 = 0;
        int i;

        for (i = 0; i < 200; ++i)
            mono [i] = in1 [i * 2];

        ref1_before = ref_expanded;
        adpcm_encode_block_ex (p1, prod_buf, &pz1, mono, 200, cfg1.bps);
        ref_encode_block_ex (r1, ref_buf, &rz1, mono, 200, cfg1.bps);

        if (pz1 != rz1 || memcmp (prod_buf, ref_buf, pz1)) {
            printf ("FAIL: interleaved p1 block %d differs\n", b);
            ok = 0;
        }

        ref2_before = ref_expanded;
        adpcm_encode_block_ex (p2, prod_buf, &pz2, in1, 200, cfg2.bps);
        ref_encode_block_ex (r2, ref_buf, &rz2, in1, 200, cfg2.bps);

        if (pz2 != rz2 || memcmp (prod_buf, ref_buf, pz2)) {
            printf ("FAIL: interleaved p2 block %d differs\n", b);
            ok = 0;
        }

        // p2 has no cache: its counters must exactly track its own recursion,
        // unaffected by p1's cached searches in between; p1 (cached) can
        // never expand more nodes than its own reference pass

        adpcm_get_search_stats (p1, &st1);
        adpcm_get_search_stats (p2, &st2);

        if (st2.hits != 0 || st2.expanded != ref_expanded - ref2_before) {
            printf ("FAIL: interleaved p2 stats wrong at block %d: %llu/%llu (ref %llu)\n", b,
                (unsigned long long) st2.expanded, (unsigned long long) st2.hits,
                (unsigned long long) (ref_expanded - ref2_before));
            ok = 0;
        }

        if (st1.expanded > ref2_before - ref1_before) {
            printf ("FAIL: interleaved p1 expanded too many nodes at block %d: %llu > %llu\n", b,
                (unsigned long long) st1.expanded, (unsigned long long) (ref2_before - ref1_before));
            ok = 0;
        }
    }

    adpcm_free_context (p1);
    adpcm_free_context (p2);
    ref_free_context (r1);
    ref_free_context (r2);

    if (ok)
        cases++;
}

/* toggling the cache between block encodes must not disturb the audio
 * history or the output, and the enable/disable API validates its arguments
 */

static void test_toggle (void)
{
    static const int pattern [] = { 0, 1, 1, 0, 1, 0 };
    tcfg cfg = { 1, 16000, 5 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_STATIC, 4 };
    void *prod = adpcm_create_context (cfg.num_channels, cfg.sample_rate, cfg.lookahead, cfg.noise_shaping);
    ref_context *ref = ref_create_context (cfg.num_channels, cfg.sample_rate, cfg.lookahead, cfg.noise_shaping);
    adpcm_search_stats st;
    int ok = 1, b;

    CHECK (adpcm_set_search_cache (NULL, 1) == 0, "NULL context accepted");
    CHECK (adpcm_set_search_cache (prod, 2) == 0, "enabled=2 accepted");
    CHECK (adpcm_set_search_cache (prod, -1) == 0, "enabled=-1 accepted");
    CHECK (adpcm_set_search_cache (prod, 0) == 1, "disable failed");
    CHECK (adpcm_set_search_cache (prod, 1) == 1, "enable failed");
    CHECK (adpcm_set_search_cache (prod, 1) == 1, "re-enable failed");
    CHECK (adpcm_set_search_cache (prod, 0) == 1, "re-disable failed");
    CHECK (adpcm_get_search_stats (NULL, &st) == 0, "NULL context stats accepted");
    CHECK (adpcm_get_search_stats (prod, NULL) == 0, "NULL stats accepted");

    gen_audio (pcm, 6 * 200, 1, 3);

    for (b = 0; b < 6 && ok; ++b) {
        size_t psz = 0, rsz = 0;

        CHECK (adpcm_set_search_cache (prod, pattern [b]) == 1, "toggle failed at block %d", b);
        adpcm_encode_block_ex (prod, prod_buf, &psz, pcm + b * 200, 200, cfg.bps);
        ref_encode_block_ex (ref, ref_buf, &rsz, pcm + b * 200, 200, cfg.bps);

        if (psz != rsz || memcmp (prod_buf, ref_buf, psz)) {
            printf ("FAIL: toggle block %d (cache %d) differs\n", b, pattern [b]);
            ok = 0;
        }

        adpcm_get_search_stats (prod, &st);

        if (!pattern [b] && st.hits != 0) {
            printf ("FAIL: hits reported with cache disabled at block %d\n", b);
            ok = 0;
        }
    }

    adpcm_free_context (prod);
    ref_free_context (ref);

    if (ok)
        cases++;
}

/* statistics are reset on every block encode; a repeated identical block
 * with a warm cache may hit more, but the counters never accumulate
 */

static void test_stats_reset (void)
{
    void *prod = adpcm_create_context (1, 16000, 5 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_OFF);
    adpcm_search_stats s1, s2;

    gen_audio (pcm, 200, 1, 1);

    adpcm_encode_block_ex (prod, prod_buf, &(size_t) { 0 }, pcm, 200, 4);
    adpcm_get_search_stats (prod, &s1);
    adpcm_encode_block_ex (prod, prod_buf, &(size_t) { 0 }, pcm, 200, 4);
    adpcm_get_search_stats (prod, &s2);

    CHECK (s1.expanded > 0 && s1.expanded == s2.expanded && s2.hits == 0,
        "uncached stats not repeatable: %llu vs %llu",
        (unsigned long long) s1.expanded, (unsigned long long) s2.expanded);

    adpcm_set_search_cache (prod, 1);
    adpcm_encode_block_ex (prod, prod_buf, &(size_t) { 0 }, pcm, 200, 4);
    adpcm_get_search_stats (prod, &s1);
    adpcm_encode_block_ex (prod, prod_buf, &(size_t) { 0 }, pcm, 200, 4);
    adpcm_get_search_stats (prod, &s2);

    CHECK (s2.expanded <= s1.expanded, "cached stats accumulated: %llu then %llu",
        (unsigned long long) s1.expanded, (unsigned long long) s2.expanded);
    CHECK (s2.hits >= s1.hits, "warm cache did not hit at least as much");

    adpcm_free_context (prod);
    cases++;
}

/* a long noisy stream forces the fixed-size cache table through collisions
 * and evictions; output must stay byte-identical throughout
 */

static void test_capacity_stress (void)
{
    tcfg cfg1 = { 2, 44100, 5 | LOOKAHEAD_EXHAUSTIVE, NOISE_SHAPING_DYNAMIC, 4 };
    tcfg cfg2 = { 1, 8000, 3, NOISE_SHAPING_OFF, 2 };
    int blocks1 = 48, samples1 = 2048;
    int blocks2 = 24, samples2 = 2048;

    gen_audio (pcm, blocks1 * samples1, 2, 2);

    if (check_stream (&cfg1, pcm, samples1, blocks1, 0, "stress stereo uncached"))
        cases++;

    if (check_stream (&cfg1, pcm, samples1, blocks1, 1, "stress stereo cached"))
        cases++;

    gen_audio (pcm, blocks2 * samples2, 1, 2);

    if (check_stream (&cfg2, pcm, samples2, blocks2, 1, "stress mono 2-bit cached"))
        cases++;
}

int main (void)
{
    ref_reset_tracking ();

    test_repro_case ();
    test_matrix ();
    test_shaping_weights ();
    test_short_tail ();
    test_interleaved_instances ();
    test_toggle ();
    test_stats_reset ();
    test_capacity_stress ();

    free (seen_table);

    if (failures) {
        printf ("ADPCM_SEARCH_CACHE_FAILED failures=%d\n", failures);
        return 1;
    }

    printf ("ADPCM_SEARCH_CACHE_OK cases=%d\n", cases);
    return 0;
}
