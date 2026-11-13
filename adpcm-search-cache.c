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

/* This file implements the optional exact search-result cache.  The recursive
 * encoder in adpcm-lib.c determines candidate ranges and visit order just as
 * before; this module only remembers exact results for states that have
 * already been fully evaluated.
 *
 * Storage is a fixed open-addressed table with a bounded probe window.  Keys
 * are compared in full, so a hash collision can only cause a miss (and thus a
 * recomputation), never an aliased hit.  When the probe window is full a slot
 * is overwritten; that likewise only causes later recomputation for the
 * displaced state.  There is no generation/epoch shortcut which could make a
 * stale entry look valid.
 */

#define CACHE_SLOTS     ((uint32_t) ADPCM_SEARCH_CACHE_SLOTS)
#define CACHE_MASK      (CACHE_SLOTS - 1)
#define CACHE_PROBE     32

struct adpcm_search_cache_entry {
    struct adpcm_search_key key;
    uint64_t value;
    uint8_t valid;
};

struct adpcm_search_cache {
    struct adpcm_search_cache_entry *entries;
};

adpcm_search_observe_fn adpcm_search_observe_hook = NULL;

static uint32_t key_hash (const struct adpcm_search_key *key)
{
    const unsigned char *p = (const unsigned char *) key;
    uint32_t hash = 2166136261u;    // FNV-1a
    size_t i;

    for (i = 0; i < sizeof (*key); i++) {
        hash ^= p [i];
        hash *= 16777619u;
    }

    return hash;
}

struct adpcm_search_cache *adpcm_search_cache_create (void)
{
    struct adpcm_search_cache *cache = malloc (sizeof (*cache));

    if (!cache)
        return NULL;

    cache->entries = calloc (CACHE_SLOTS, sizeof (*cache->entries));

    if (!cache->entries) {
        free (cache);
        return NULL;
    }

    return cache;
}

void adpcm_search_cache_destroy (struct adpcm_search_cache *cache)
{
    if (cache) {
        free (cache->entries);
        free (cache);
    }
}

void adpcm_search_cache_clear (struct adpcm_search_cache *cache)
{
    if (cache)
        memset (cache->entries, 0, CACHE_SLOTS * sizeof (*cache->entries));
}

int adpcm_search_cache_lookup (struct adpcm_search_cache *cache,
    const struct adpcm_search_key *key, uint64_t max_error, uint64_t *value)
{
    uint32_t pos, i;

    if (!cache || !key || !value)
        return 0;

    pos = key_hash (key) & CACHE_MASK;

    for (i = 0; i < CACHE_PROBE; i++) {
        struct adpcm_search_cache_entry *entry = cache->entries + ((pos + i) & CACHE_MASK);

        if (!entry->valid)
            return 0;

        if (!memcmp (&entry->key, key, sizeof (*key))) {
            if (entry->value < max_error) {
                *value = entry->value;
                return 1;
            }

            return 0;
        }
    }

    return 0;
}

void adpcm_search_cache_store (struct adpcm_search_cache *cache,
    const struct adpcm_search_key *key, uint64_t value, uint64_t max_error)
{
    uint32_t pos, i;

    // A result that reached (or exceeded) the caller's pruning limit may have
    // stopped early and is not necessarily the complete optimum, so it must
    // never be treated as an exact cached result.

    if (!cache || !key || value >= max_error)
        return;

    pos = key_hash (key) & CACHE_MASK;

    for (i = 0; i < CACHE_PROBE; i++) {
        struct adpcm_search_cache_entry *entry = cache->entries + ((pos + i) & CACHE_MASK);

        if (!entry->valid || !memcmp (&entry->key, key, sizeof (*key))) {
            entry->key = *key;
            entry->value = value;
            entry->valid = 1;
            return;
        }
    }

    // Probe window full: overwrite the home slot.  The new key remains findable
    // from its home; the displaced key simply misses and is recomputed later.

    cache->entries [pos].key = *key;
    cache->entries [pos].value = value;
    cache->entries [pos].valid = 1;
}
