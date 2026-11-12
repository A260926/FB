/*
 * libxlsxwriter
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright 2014-2026, John McNamara, jmcnamara@cpan.org.
 *
 * disk_sst - A disk backed shared string table for workbooks written in
 *            constant_memory mode.
 *
 * The unique strings and the hash index that locates them both live in
 * temporary files, so the resident memory used by the shared string table
 * is bounded by the caller supplied cache budget instead of growing with
 * the number of unique strings.
 *
 */
#ifndef __LXW_DISK_SST_H__
#define __LXW_DISK_SST_H__

#include <stdio.h>
#include <stdint.h>

#include "common.h"

/* Default cache budget when sst_cache_size is left at zero. */
#define LXW_SST_DEFAULT_CACHE_SIZE 65536

/* Minimum accepted non-zero cache budget. */
#define LXW_SST_MIN_CACHE_SIZE 4096

/* Number of hash buckets in the on-disk primary index. */
#define LXW_SST_BUCKETS 256

/* On disk record tags, stored as a single byte. */
#define LXW_SST_TAG_PLAIN 0
#define LXW_SST_TAG_RICH  1

/* Fixed size header and bucket table of the index file. */
#define LXW_SST_INDEX_MAGIC "LXWSST1"
#define LXW_SST_INDEX_HEADER_SIZE 64
#define LXW_SST_SPILL_RECORD_SIZE 32

/*
 * A single hash chain entry held in the bounded memory cache.
 */
typedef struct lxw_disk_sst_node {
    uint32_t index;
    uint32_t hash;
    uint64_t data_offset;
    uint32_t data_len;
    uint8_t is_rich_string;
    uint8_t bucket;

    /* Resident copy of the key, freed when the node is spilled to disk. */
    char *key;
    size_t key_len;

    /* LRU list pointers; least recently used at the head, most at tail. */
    TAILQ_ENTRY (lxw_disk_sst_node) cache_pointers;

    /* Resident hash bucket chain pointer. */
    struct lxw_disk_sst_node *next;
} lxw_disk_sst_node;

TAILQ_HEAD(lxw_disk_sst_lru, lxw_disk_sst_node);

/*
 * Statistics describing the real memory retained by the disk SST backend.
 * These count bytes owned by the backend only; caller input, the string
 * currently being registered and XML output buffers are excluded.
 */
typedef struct lxw_disk_sst_stats {
    /* Current bytes used by resident index node structs. */
    size_t index_bytes;

    /* Current bytes used by resident cached key copies. */
    size_t cache_bytes;

    /* Highest values observed for the fields above. */
    size_t index_bytes_peak;
    size_t cache_bytes_peak;

    /* Number of entries currently resident in the memory cache. */
    uint32_t cached_entries;

    /* Number of cache entries evicted to the on-disk spill chains. */
    uint32_t evictions;

    /* Number of lookups served from the memory cache. */
    uint32_t cache_hits;

    /* Number of lookups that had to read the on-disk spill chain. */
    uint32_t disk_lookups;
} lxw_disk_sst_stats;

/*
 * The disk backed shared string table.
 */
typedef struct lxw_disk_sst {
    /* Append-only file holding the full string records, in index order. */
    FILE *data_file;

    /* Fixed size bucket heads followed by append-only spill chain nodes. */
    FILE *index_file;

    const char *tmpdir;
    size_t cache_budget;

    /* Stable numbering, starting at zero, in first registration order. */
    uint32_t string_count;
    uint32_t unique_count;

    /* Resident hash bucket heads. */
    lxw_disk_sst_node *buckets[LXW_SST_BUCKETS];

    /* Mirrors the persisted bucket head offsets (one spill chain each). */
    uint64_t head_offsets[LXW_SST_BUCKETS];

    /* LRU list of every resident node. */
    struct lxw_disk_sst_lru lru;

    /* Retained memory accounting. */
    lxw_disk_sst_stats stats;

    /* First error recorded by the backend, sticky for the workbook life. */
    lxw_error error;

    /* Test seam: once write_ops reaches inject_fail_after, writes fail. */
    uint32_t write_ops;
    uint32_t inject_fail_after;

    /* Test seam: when non-zero every backend read fails. */
    uint8_t inject_read_failure;
} lxw_disk_sst;

/* *INDENT-OFF* */
#ifdef __cplusplus
extern "C" {
#endif
/* *INDENT-ON* */

/*
 * FNV-1a hash over the raw string bytes with the string kind mixed in.
 * Exposed so tests can construct genuine bucket collisions.
 */
uint32_t lxw_disk_sst_hash(const char *string, uint8_t is_rich_string);

/*
 * Create and destroy the disk backed table. The cache_size is the memory
 * budget for the resident index/key cache; zero selects the default.
 */
lxw_disk_sst *lxw_disk_sst_new(const char *tmpdir, size_t cache_size);
void lxw_disk_sst_free(lxw_disk_sst *self);

/*
 * Add a string, or find an identical one (same bytes and same kind), and
 * return its stable index. New indices are only consumed on success.
 * The optional existed flag is set to 1 for a duplicate and 0 for a new
 * unique value.
 */
lxw_error lxw_disk_sst_get_index(lxw_disk_sst *self, const char *string,
                                 uint8_t is_rich_string, uint32_t *index,
                                 uint8_t *existed);

/*
 * Stream every stored <si> in index order to outfile, reusing a single
 * record sized read buffer so the full table is never loaded at once.
 */
lxw_error lxw_disk_sst_write_si(lxw_disk_sst *self, FILE *outfile);

/* Return a copy of the internal accounting statistics. */
void lxw_disk_sst_get_stats(lxw_disk_sst *self, lxw_disk_sst_stats *stats);

/*
 * Test seam: make the after-th backend write fail. Pass zero to disable.
 */
void lxw_disk_sst_inject_write_failure(lxw_disk_sst *self, uint32_t after);

/*
 * Test seam: when enabled, every backend read (spill chain and string
 * record) fails. Pass zero to disable.
 */
void lxw_disk_sst_inject_read_failure(lxw_disk_sst *self, uint8_t enable);

/* *INDENT-OFF* */
#ifdef __cplusplus
}
#endif
/* *INDENT-ON* */

#endif /* __LXW_DISK_SST_H__ */
