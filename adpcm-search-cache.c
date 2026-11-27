////////////////////////////////////////////////////////////////////////////
//                           **** ADPCM-XQ ****                           //
//                  Xtreme Quality ADPCM Encoder/Decoder                  //
//                    Copyright (c) 2024 David Bryant.                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

#include <stdlib.h>
#include <string.h>

#include "adpcm-search-cache.h"

/* The table is direct-mapped and fixed in size: 32768 slots of 80 bytes is
 * 2.5 MiB per encoder instance, comfortably under the 4 MiB limit. It is
 * allocated only when the cache is first enabled and never grows or shrinks
 * while encoding, so no memory is allocated or freed during the recursion.
 * A slot collision simply evicts the previous occupant, which can only cause
 * an extra recomputation later, never an incorrect result.
 */

#define NUM_SLOTS   32768

#define SLOT_EMPTY  0       // slot not in use
#define SLOT_E0     1       // value is the node's immediate naive error e0
#define SLOT_FULL   2       // value is the heuristic cost for "budget" only

typedef struct {
    adpcm_search_key key;
    uint64_t value;         // the cached result
    uint64_t budget;        // max_error a FULL result was computed with
    uint8_t state;
} cache_slot;

struct adpcm_search_cache {
    cache_slot slots [NUM_SLOTS];
};

adpcm_search_cache *adpcm_search_cache_create (void)
{
    return calloc (1, sizeof (adpcm_search_cache));
}

void adpcm_search_cache_destroy (adpcm_search_cache *cache)
{
    free (cache);
}

/* FNV-1a over the entire key; callers zero the key (including padding and any
 * unused sample slots) before filling it, so every byte is well defined.
 */

static uint64_t hash_key (const adpcm_search_key *key)
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

int adpcm_search_cache_lookup (adpcm_search_cache *cache, const adpcm_search_key *key, uint64_t max_error, uint64_t *value)
{
    cache_slot *slot = cache->slots + (hash_key (key) & (NUM_SLOTS - 1));

    if (slot->state == SLOT_EMPTY || memcmp (&slot->key, key, sizeof (*key)))
        return 0;

    // An E0 result is the node's immediate naive error: a leaf returns it for
    // any threshold, and any other node returns it for any threshold up to e0
    // (the node stops early there by definition). Anything else must be
    // recomputed because the heuristic return is not threshold-independent.

    if (slot->state == SLOT_E0) {
        if (key->depth == 0 || max_error <= slot->value) {
            *value = slot->value;
            return 1;
        }

        return 0;
    }

    // A FULL result is reused only for the identical threshold it was
    // computed with, which is trivially exact.

    if (max_error == slot->budget) {
        *value = slot->value;
        return 1;
    }

    return 0;
}

void adpcm_search_cache_store (adpcm_search_cache *cache, const adpcm_search_key *key, uint64_t value, uint64_t budget, int e0)
{
    cache_slot *slot = cache->slots + (hash_key (key) & (NUM_SLOTS - 1));

    slot->key = *key;
    slot->value = value;
    slot->budget = budget;
    slot->state = e0 ? SLOT_E0 : SLOT_FULL;
}
