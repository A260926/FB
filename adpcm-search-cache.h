////////////////////////////////////////////////////////////////////////////
//                           **** ADPCM-XQ ****                           //
//                  Xtreme Quality ADPCM Encoder/Decoder                  //
//                    Copyright (c) 2024 David Bryant.                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

#ifndef ADPCM_SEARCH_CACHE_H_
#define ADPCM_SEARCH_CACHE_H_

#include "adpcm-lib.h"

/* Optional, fixed-size, exact-value cache for the recursive lookahead search.
 *
 * The cache is disabled by default and is only allocated when enabled with
 * adpcm_set_search_cache().  It never grows with the length of the recording:
 * one table of ADPCM_SEARCH_CACHE_SLOTS entries is allocated per instance and
 * reused for every block.  Its footprint is well under the 4 MiB per-instance
 * limit (see README for the exact number).
 *
 * An identity ("key") is the complete state that the four min_error_n*()
 * routines read: the predictor (pcmdata and step index), the noise-shaping
 * state (error and shaping weight), the already noise-shaped current sample,
 * the raw lookahead samples that this recursion depth actually consumes,
 * the channel stride, the code width, and the complete search flags (depth,
 * EXHAUSTIVE/NO_BRANCHING and noise-shaping mode).  max_error is deliberately
 * not part of the key: it is a caller pruning limit, not part of the state.
 */

#define ADPCM_SEARCH_MAX_DEPTH  16
#define ADPCM_SEARCH_CACHE_SLOTS 32768u

struct adpcm_search_key {
    int32_t pcmdata;                            // predictor output
    int32_t error;                              // noise-shaping error term
    int32_t shaping_weight;                     // current shaping weight
    int32_t csample;                            // current (possibly shaped) sample
    int16_t samples [ADPCM_SEARCH_MAX_DEPTH];   // raw samples consumed ahead
    int8_t index;                               // step table index
    uint8_t nch;                                // channel stride
    uint8_t bps;                                // code width in bits
    int16_t flags;                              // depth + search/shaping flags
};

struct adpcm_search_cache;

struct adpcm_search_cache *adpcm_search_cache_create (void);
void adpcm_search_cache_destroy (struct adpcm_search_cache *cache);
void adpcm_search_cache_clear (struct adpcm_search_cache *cache);

/* Returns 1 only when the key is present with an exact result that is still
 * usable for the supplied max_error (i.e., stored value < max_error).  A
 * collision, a full table, or any other mismatch simply returns 0. */
int adpcm_search_cache_lookup (struct adpcm_search_cache *cache,
    const struct adpcm_search_key *key, uint64_t max_error, uint64_t *value);

/* Only exact results (value < max_error) are stored.  Calls that were cut
 * short by the caller's pruning threshold are never recorded as reusable. */
void adpcm_search_cache_store (struct adpcm_search_cache *cache,
    const struct adpcm_search_key *key, uint64_t value, uint64_t max_error);

/* Optional, test/benchmark observation point.  It is called exactly once per
 * recursive node that actually enters the production candidate computation,
 * after any cache hit has been served.  Therefore the number of callbacks is
 * equal to adpcm_search_stats.expanded for that block.  It is NULL by default
 * and never affects encoding. */
typedef void (*adpcm_search_observe_fn) (int bps, int flags, uint64_t max_error);
extern adpcm_search_observe_fn adpcm_search_observe_hook;

#endif /* ADPCM_SEARCH_CACHE_H_ */
