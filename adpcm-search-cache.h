////////////////////////////////////////////////////////////////////////////
//                           **** ADPCM-XQ ****                           //
//                  Xtreme Quality ADPCM Encoder/Decoder                  //
//                    Copyright (c) 2024 David Bryant.                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

#ifndef ADPCM_SEARCH_CACHE_H
#define ADPCM_SEARCH_CACHE_H

#include "adpcm-lib.h"

/* Optional exact-search cache for the lookahead recursion in adpcm-lib.c.
 *
 * The cache is a fixed-size, direct-mapped table keyed by the complete
 * identity of a recursive search node: everything that determines the node's
 * result EXCLUDING the caller's pruning threshold (max_error). That identity
 * is the predictor value, the step table index, the noise shaping error term
 * and coefficient, the current (possibly shaped) sample, the future source
 * samples the node can still see, the remaining depth, the search control
 * flags, the code width and the channel stride.
 *
 * The cache never changes the encoder output. The recursive search functions
 * are heuristics whose return value depends on max_error in a complicated
 * way: a call that is cut short by the threshold returns only a partial cost,
 * and even a completed call can be inflated by pruning deeper in the subtree.
 * Only two kinds of results are therefore reused across thresholds, both
 * provably exact (see README.md):
 *
 *  - "E0" results, where the node returned its immediate naive error e0.
 *    A leaf (depth 0) returns e0 for ANY threshold, and a node whose naive
 *    error already reaches the threshold returns e0 for any threshold up to
 *    e0, so an E0 entry may answer any query with max_error <= e0 (or any
 *    query at all for a leaf).
 *
 *  - "FULL" results, which are reused only for the identical max_error they
 *    were computed with, which is trivially exact.
 *
 * Collisions, a full table or a query that matches neither rule simply cause
 * the node to be recomputed; they can never produce a "hit" with a wrong
 * value.
 */

#define ADPCM_SEARCH_MAX_DEPTH  16      // matches the LOOKAHEAD_DEPTH limit

typedef struct {
    int32_t pcmdata;                            // current predictor output
    int32_t csample;                            // current sample (after any noise shaping)
    int32_t error;                              // noise shaping error term
    int32_t shaping_weight;                     // noise shaping coefficient
    int16_t samples [ADPCM_SEARCH_MAX_DEPTH];   // future source samples (first "depth" valid)
    uint16_t flags;                             // shaping mode, EXHAUSTIVE and NO_BRANCHING bits
    int8_t index;                               // step table index
    uint8_t depth;                              // remaining lookahead depth
    uint8_t bps;                                // ADPCM code width in bits (2-5)
    uint8_t nch;                                // channel count (source sample stride)
} adpcm_search_key;

typedef struct adpcm_search_cache adpcm_search_cache;

adpcm_search_cache *adpcm_search_cache_create (void);
void adpcm_search_cache_destroy (adpcm_search_cache *cache);

/* Look up a node. Returns 1 and stores the reusable result in *value on a
 * hit, or 0 on a miss (a miss is also reported when an entry exists but its
 * reuse rule does not cover this max_error).
 */

int adpcm_search_cache_lookup (adpcm_search_cache *cache, const adpcm_search_key *key,
    uint64_t max_error, uint64_t *value);

/* Store a freshly computed node result. "e0" must be nonzero only when the
 * returned value is exactly the node's immediate naive error (a leaf or a
 * threshold early-stop); "budget" is the max_error the result was computed
 * with and is only meaningful for non-e0 results.
 */

void adpcm_search_cache_store (adpcm_search_cache *cache, const adpcm_search_key *key,
    uint64_t value, uint64_t budget, int e0);

#endif /* ADPCM_SEARCH_CACHE_H */
