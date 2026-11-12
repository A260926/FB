/*****************************************************************************
 *
 * disk_sst - A disk backed shared string table for constant_memory mode.
 *
 * The full string records are appended to one temporary file in stable
 * index order. A second temporary file holds a fixed size hash bucket
 * table and append-only spill chain records for the entries that have
 * been evicted from the bounded memory cache. Only a small number of file
 * handles are used regardless of how many unique strings are stored.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright 2014-2026, John McNamara, jmcnamara@cpan.org.
 *
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "xlsxwriter/disk_sst.h"
#include "xlsxwriter/xmlwriter.h"
#include "xlsxwriter/utility.h"

/* One byte kind tag plus a four byte little-endian length prefix. */
#define LXW_SST_DATA_HEADER_SIZE 5

/*****************************************************************************
 *
 * Private functions.
 *
 ****************************************************************************/

/*
 * Fixed width, byte oriented encoding so the index file does not depend
 * on the host struct padding or endianness.
 */
STATIC void
_put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t) (value & 0xFF);
    p[1] = (uint8_t) ((value >> 8) & 0xFF);
    p[2] = (uint8_t) ((value >> 16) & 0xFF);
    p[3] = (uint8_t) ((value >> 24) & 0xFF);
}

STATIC uint32_t
_get_u32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
           ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

STATIC void
_put_u64(uint8_t *p, uint64_t value)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (uint8_t) ((value >> (8 * i)) & 0xFF);
}

STATIC uint64_t
_get_u64(const uint8_t *p)
{
    uint64_t value = 0;
    int i;
    for (i = 0; i < 8; i++)
        value |= ((uint64_t) p[i]) << (8 * i);
    return value;
}

/*
 * FNV-1a hash over the raw string bytes, with the string kind mixed in so
 * that a plain string and a rich string never share an identity. The hash
 * is only used to select the bucket; every match is confirmed by comparing
 * the full bytes.
 */
uint32_t
lxw_disk_sst_hash(const char *string, uint8_t is_rich_string)
{
    uint32_t hash = 2166136261U;
    const unsigned char *p = (const unsigned char *) string;

    while (*p) {
        hash ^= *p++;
        hash *= 16777619U;
    }

    hash ^= is_rich_string ? 0x9E3779B9U : 0x84222325U;
    return hash;
}

/*
 * Record the first failure on the table. Once set, every later operation
 * fails fast, so a workbook cannot be assembled after a backend error.
 */
STATIC lxw_error
_fail(lxw_disk_sst *self, lxw_error error)
{
    if (self->error == LXW_NO_ERROR)
        self->error = error;
    return error;
}

/*
 * Checked write to a temporary file.
 */
STATIC lxw_error
_write_bytes(lxw_disk_sst *self, FILE *file, const void *buffer, size_t size)
{
    if (size && fwrite(buffer, size, 1, file) != 1)
        return _fail(self, LXW_ERROR_CREATING_TMPFILE);

    return LXW_NO_ERROR;
}

/*
 * Checked read from a temporary file.
 */
STATIC lxw_error
_read_bytes(lxw_disk_sst *self, FILE *file, void *buffer, size_t size)
{
    if (self->inject_read_failure)
        return _fail(self, LXW_ERROR_READING_TMPFILE);

    if (size && fread(buffer, size, 1, file) != 1)
        return _fail(self, LXW_ERROR_READING_TMPFILE);

    return LXW_NO_ERROR;
}

/*
 * Test seam for controllable backend write failures. A zero threshold
 * disables injection.
 */
STATIC lxw_error
_check_write_event(lxw_disk_sst *self)
{
    if (self->inject_fail_after &&
        self->write_ops >= self->inject_fail_after)
        return _fail(self, LXW_ERROR_CREATING_TMPFILE);

    return LXW_NO_ERROR;
}

/*
 * Append the 32 byte spill record describing an evicted node.
 */
STATIC lxw_error
_write_spill_record(lxw_disk_sst *self, lxw_disk_sst_node *node,
                    uint64_t next_offset, uint64_t *record_offset)
{
    uint8_t record[LXW_SST_SPILL_RECORD_SIZE] = { 0 };
    lxw_error err;

    if (fseek(self->index_file, 0, SEEK_END) != 0)
        return _fail(self, LXW_ERROR_CREATING_TMPFILE);

    *record_offset = (uint64_t) ftell(self->index_file);

    _put_u32(record + 0, node->index);
    _put_u32(record + 4, node->hash);
    _put_u32(record + 8, node->data_len);
    record[12] = node->is_rich_string;
    record[13] = node->bucket;
    _put_u64(record + 16, node->data_offset);
    _put_u64(record + 24, next_offset);

    err = _write_bytes(self, self->index_file, record, sizeof(record));
    if (err)
        return err;

    self->write_ops++;
    return LXW_NO_ERROR;
}

/*
 * Persist the new head offset of a bucket spill chain.
 */
STATIC lxw_error
_update_bucket_head(lxw_disk_sst *self, uint8_t bucket, uint64_t offset)
{
    uint8_t encoded[8];
    lxw_error err;

    _put_u64(encoded, offset);

    if (fseek(self->index_file,
              LXW_SST_INDEX_HEADER_SIZE + ((long) bucket * 8),
              SEEK_SET) != 0)
        return _fail(self, LXW_ERROR_CREATING_TMPFILE);

    err = _write_bytes(self, self->index_file, encoded, sizeof(encoded));
    if (err)
        return err;

    if (fseek(self->index_file, 0, SEEK_END) != 0)
        return _fail(self, LXW_ERROR_CREATING_TMPFILE);

    return LXW_NO_ERROR;
}

/*
 * Remove a node from its resident bucket chain.
 */
STATIC void
_unlink_bucket_node(lxw_disk_sst *self, lxw_disk_sst_node *node)
{
    lxw_disk_sst_node *current = self->buckets[node->bucket];
    lxw_disk_sst_node *previous = NULL;

    while (current) {
        if (current == node) {
            if (previous)
                previous->next = current->next;
            else
                self->buckets[node->bucket] = current->next;
            return;
        }
        previous = current;
        current = current->next;
    }
}

/*
 * Adjust the retained memory counters and their peaks.
 */
STATIC void
_account_add(lxw_disk_sst *self, lxw_disk_sst_node *node)
{
    self->stats.index_bytes += sizeof(lxw_disk_sst_node);
    self->stats.cache_bytes += node->key_len + 1;
    self->stats.cached_entries++;

    if (self->stats.index_bytes > self->stats.index_bytes_peak)
        self->stats.index_bytes_peak = self->stats.index_bytes;
    if (self->stats.cache_bytes > self->stats.cache_bytes_peak)
        self->stats.cache_bytes_peak = self->stats.cache_bytes;
}

STATIC void
_account_remove(lxw_disk_sst *self, lxw_disk_sst_node *node)
{
    self->stats.index_bytes -= sizeof(lxw_disk_sst_node);
    self->stats.cache_bytes -= node->key_len + 1;
    self->stats.cached_entries--;
}

STATIC size_t
_retained_bytes(lxw_disk_sst *self)
{
    return self->stats.index_bytes + self->stats.cache_bytes;
}

/*
 * Evict least recently used nodes until the cache can hold the incoming
 * entry within the budget. Every evicted node is appended to its on-disk
 * spill chain before its resident key is freed.
 */
STATIC lxw_error
_make_room(lxw_disk_sst *self, size_t incoming_bytes)
{
    lxw_disk_sst_node *victim;
    uint64_t record_offset;
    lxw_error err;

    while (_retained_bytes(self) + incoming_bytes > self->cache_budget &&
           !TAILQ_EMPTY(&self->lru)) {

        victim = TAILQ_FIRST(&self->lru);

        err = _check_write_event(self);
        if (err)
            return err;

        err = _write_spill_record(self, victim,
                                  self->head_offsets[victim->bucket],
                                  &record_offset);
        if (err)
            return err;

        err = _update_bucket_head(self, victim->bucket, record_offset);
        if (err)
            return err;

        self->head_offsets[victim->bucket] = record_offset;

        _unlink_bucket_node(self, victim);
        TAILQ_REMOVE(&self->lru, victim, cache_pointers);
        _account_remove(self, victim);

        free(victim->key);
        free(victim);
        self->stats.evictions++;
    }

    return LXW_NO_ERROR;
}

/*
 * Read one fixed width spill record.
 */
STATIC lxw_error
_read_spill_record(lxw_disk_sst *self, uint64_t offset, uint32_t *index,
                   uint32_t *hash, uint32_t *data_len, uint8_t *tag,
                   uint64_t *data_offset, uint64_t *next_offset)
{
    uint8_t record[LXW_SST_SPILL_RECORD_SIZE];
    lxw_error err;

    if (fseek(self->index_file, (long) offset, SEEK_SET) != 0)
        return _fail(self, LXW_ERROR_READING_TMPFILE);

    err = _read_bytes(self, self->index_file, record, sizeof(record));
    if (err)
        return err;

    *index = _get_u32(record + 0);
    *hash = _get_u32(record + 4);
    *data_len = _get_u32(record + 8);
    *tag = record[12];
    *data_offset = _get_u64(record + 16);
    *next_offset = _get_u64(record + 24);

    return LXW_NO_ERROR;
}

/*
 * Walk the on-disk spill chain for a bucket looking for an exact match.
 */
STATIC lxw_error
_find_on_disk(lxw_disk_sst *self, uint8_t bucket, uint32_t hash,
              const char *string, size_t string_len, uint8_t is_rich_string,
              uint32_t *index, uint8_t *found)
{
    uint64_t offset = self->head_offsets[bucket];
    uint64_t data_offset;
    uint64_t next_offset;
    uint32_t rec_index;
    uint32_t rec_hash;
    uint32_t rec_len;
    uint8_t rec_tag;
    char *buffer;
    lxw_error err;

    *found = LXW_FALSE;
    if (!offset)
        return LXW_NO_ERROR;

    self->stats.disk_lookups++;

    while (offset) {
        err = _read_spill_record(self, offset, &rec_index, &rec_hash,
                                 &rec_len, &rec_tag, &data_offset,
                                 &next_offset);
        if (err)
            return err;

        if (rec_hash == hash && rec_tag == is_rich_string &&
            rec_len == string_len) {

            /* This buffer holds only the single record being compared. The
             * string follows the one byte tag and four byte length. */
            buffer = malloc(rec_len + 1);
            if (!buffer)
                return _fail(self, LXW_ERROR_MEMORY_MALLOC_FAILED);

            if (fseek(self->data_file,
                      (long) (data_offset + LXW_SST_DATA_HEADER_SIZE),
                      SEEK_SET) != 0) {
                free(buffer);
                return _fail(self, LXW_ERROR_READING_TMPFILE);
            }

            err = _read_bytes(self, self->data_file, buffer, rec_len);
            if (err) {
                free(buffer);
                return err;
            }
            buffer[rec_len] = '\0';

            if (memcmp(buffer, string, rec_len) == 0) {
                *index = rec_index;
                *found = LXW_TRUE;
                free(buffer);
                return LXW_NO_ERROR;
            }
            free(buffer);
        }

        offset = next_offset;
    }

    return LXW_NO_ERROR;
}

/*
 * Write the <si> element for a plain string, preserving the existing
 * control character escaping and whitespace handling.
 */
STATIC lxw_error
_write_plain_si(lxw_disk_sst *self, FILE *outfile, char *string)
{
    struct xml_attribute_list attributes;
    struct xml_attribute *attribute;
    char *escaped_string = string;
    uint8_t was_escaped = LXW_FALSE;

    LXW_INIT_ATTRIBUTES();

    if (isspace((unsigned char) string[0]) ||
        isspace((unsigned char) string[strlen(string) - 1]))
        LXW_PUSH_ATTRIBUTES_STR("xml:space", "preserve");

    if (lxw_has_control_characters(string)) {
        escaped_string = lxw_escape_control_characters(string);
        was_escaped = LXW_TRUE;
        if (!escaped_string)
            return _fail(self, LXW_ERROR_MEMORY_MALLOC_FAILED);
    }

    lxw_xml_start_tag(outfile, "si", NULL);
    lxw_xml_data_element(outfile, "t", escaped_string, &attributes);
    lxw_xml_end_tag(outfile, "si");

    LXW_FREE_ATTRIBUTES();

    if (was_escaped)
        free(escaped_string);

    if (ferror(outfile))
        return _fail(self, LXW_ERROR_CREATING_TMPFILE);

    return LXW_NO_ERROR;
}

/*****************************************************************************
 *
 * Public functions.
 *
 ****************************************************************************/

/*
 * Create the disk backed table and its two temporary files.
 */
lxw_disk_sst *
lxw_disk_sst_new(const char *tmpdir, size_t cache_size)
{
    lxw_disk_sst *self;
    uint8_t header[LXW_SST_INDEX_HEADER_SIZE] = { 0 };
    uint8_t zeros[LXW_SST_BUCKETS * 8];
    lxw_error err = LXW_NO_ERROR;

    if (cache_size == 0)
        cache_size = LXW_SST_DEFAULT_CACHE_SIZE;
    else if (cache_size < LXW_SST_MIN_CACHE_SIZE)
        return NULL;

    self = calloc(1, sizeof(lxw_disk_sst));
    if (!self)
        return NULL;

    self->cache_budget = cache_size;
    self->tmpdir = tmpdir;
    TAILQ_INIT(&self->lru);

    self->data_file = lxw_tmpfile(tmpdir);
    if (!self->data_file)
        goto mem_error;

    self->index_file = lxw_tmpfile(tmpdir);
    if (!self->index_file)
        goto mem_error;

    /* Write the fixed size index header and the zeroed bucket table. */
    memcpy(header, LXW_SST_INDEX_MAGIC, sizeof(LXW_SST_INDEX_MAGIC) - 1);
    _put_u32(header + 8, 1);
    _put_u32(header + 12, LXW_SST_BUCKETS);

    err = _write_bytes(self, self->index_file, header, sizeof(header));
    if (err)
        goto mem_error;

    memset(zeros, 0, sizeof(zeros));
    err = _write_bytes(self, self->index_file, zeros, sizeof(zeros));
    if (err)
        goto mem_error;

    if (fflush(self->index_file) != 0)
        goto mem_error;

    return self;

mem_error:
    lxw_disk_sst_free(self);
    return NULL;
}

/*
 * Free the table, its temporary files and every resident cache entry.
 */
void
lxw_disk_sst_free(lxw_disk_sst *self)
{
    lxw_disk_sst_node *node;

    if (!self)
        return;

    while (!TAILQ_EMPTY(&self->lru)) {
        node = TAILQ_FIRST(&self->lru);
        TAILQ_REMOVE(&self->lru, node, cache_pointers);
        free(node->key);
        free(node);
    }

    if (self->data_file)
        fclose(self->data_file);

    if (self->index_file)
        fclose(self->index_file);

    free(self);
}

/*
 * Add or find a string and return its stable index. A new index is only
 * consumed after every required write has succeeded.
 */
lxw_error
lxw_disk_sst_get_index(lxw_disk_sst *self, const char *string,
                       uint8_t is_rich_string, uint32_t *index,
                       uint8_t *existed)
{
    lxw_disk_sst_node *node;
    lxw_disk_sst_node *current;
    uint8_t tag = is_rich_string ? LXW_SST_TAG_RICH : LXW_SST_TAG_PLAIN;
    uint8_t bucket;
    uint32_t hash;
    uint64_t data_offset;
    size_t string_len;
    uint8_t encoded_len[4];
    lxw_error err;

    if (existed)
        *existed = LXW_FALSE;

    if (self->error)
        return self->error;

    if (!string)
        return _fail(self, LXW_ERROR_NULL_PARAMETER_IGNORED);

    string_len = strlen(string);
    hash = lxw_disk_sst_hash(string, is_rich_string);
    bucket = (uint8_t) (hash % LXW_SST_BUCKETS);

    /* Search the resident cache first. */
    current = self->buckets[bucket];
    while (current) {
        if (current->hash == hash &&
            current->is_rich_string == is_rich_string &&
            current->key_len == string_len &&
            memcmp(current->key, string, string_len) == 0) {

            TAILQ_REMOVE(&self->lru, current, cache_pointers);
            TAILQ_INSERT_TAIL(&self->lru, current, cache_pointers);
            self->stats.cache_hits++;
            self->string_count++;
            *index = current->index;
            if (existed)
                *existed = LXW_TRUE;
            return LXW_NO_ERROR;
        }
        current = current->next;
    }

    /* Search the spilled portion of the chain on disk. */
    {
        uint8_t found = LXW_FALSE;
        err = _find_on_disk(self, bucket, hash, string, string_len,
                            is_rich_string, index, &found);
        if (err)
            return err;

        if (found) {
            self->string_count++;
            if (existed)
                *existed = LXW_TRUE;
            return LXW_NO_ERROR;
        }
    }

    /* New unique value. Persist the record before consuming an index. */
    err = _check_write_event(self);
    if (err)
        return err;

    if (fseek(self->data_file, 0, SEEK_END) != 0)
        return _fail(self, LXW_ERROR_CREATING_TMPFILE);

    data_offset = (uint64_t) ftell(self->data_file);

    err = _write_bytes(self, self->data_file, &tag, sizeof(tag));
    if (err)
        return err;

    _put_u32(encoded_len, (uint32_t) string_len);
    err = _write_bytes(self, self->data_file, encoded_len,
                       sizeof(encoded_len));
    if (err)
        return err;

    err = _write_bytes(self, self->data_file, string, string_len);
    if (err)
        return err;

    self->write_ops++;

    node = calloc(1, sizeof(lxw_disk_sst_node));
    if (!node)
        return _fail(self, LXW_ERROR_MEMORY_MALLOC_FAILED);

    node->key = lxw_strdup(string);
    if (!node->key) {
        free(node);
        return _fail(self, LXW_ERROR_MEMORY_MALLOC_FAILED);
    }

    /* Make room before the new index becomes visible. */
    err = _make_room(self, sizeof(lxw_disk_sst_node) + string_len + 1);
    if (err) {
        free(node->key);
        free(node);
        return err;
    }

    node->index = self->unique_count;
    node->hash = hash;
    node->data_offset = data_offset;
    node->data_len = (uint32_t) string_len;
    node->is_rich_string = is_rich_string;
    node->bucket = bucket;
    node->key_len = string_len;

    node->next = self->buckets[bucket];
    self->buckets[bucket] = node;
    TAILQ_INSERT_TAIL(&self->lru, node, cache_pointers);
    _account_add(self, node);

    self->unique_count++;
    self->string_count++;
    *index = node->index;

    return LXW_NO_ERROR;
}

/*
 * Stream every stored <si> in index order. A single reusable read buffer
 * holds the current record, so the full string table is never loaded.
 */
lxw_error
lxw_disk_sst_write_si(lxw_disk_sst *self, FILE *outfile)
{
    char *buffer = NULL;
    size_t buffer_cap = 0;
    uint32_t i;
    uint8_t tag;
    uint8_t encoded_len[4];
    uint32_t record_len;
    lxw_error err;

    if (self->error)
        return self->error;

    rewind(self->data_file);

    for (i = 0; i < self->unique_count; i++) {
        err = _read_bytes(self, self->data_file, &tag, sizeof(tag));
        if (err)
            goto cleanup;

        err = _read_bytes(self, self->data_file, encoded_len,
                          sizeof(encoded_len));
        if (err)
            goto cleanup;

        record_len = _get_u32(encoded_len);

        if (record_len + 1 > buffer_cap) {
            char *new_buffer = realloc(buffer, record_len + 1);
            if (!new_buffer) {
                err = _fail(self, LXW_ERROR_MEMORY_MALLOC_FAILED);
                goto cleanup;
            }
            buffer = new_buffer;
            buffer_cap = record_len + 1;
        }

        err = _read_bytes(self, self->data_file, buffer, record_len);
        if (err)
            goto cleanup;
        buffer[record_len] = '\0';

        if (tag == LXW_SST_TAG_RICH) {
            lxw_xml_rich_si_element(outfile, buffer);
            if (ferror(outfile)) {
                err = _fail(self, LXW_ERROR_CREATING_TMPFILE);
                goto cleanup;
            }
        }
        else {
            err = _write_plain_si(self, outfile, buffer);
            if (err)
                goto cleanup;
        }
    }

    if (fflush(outfile) != 0)
        err = _fail(self, LXW_ERROR_CREATING_TMPFILE);

cleanup:
    free(buffer);
    return err;
}

/*
 * Return a copy of the internal accounting statistics.
 */
void
lxw_disk_sst_get_stats(lxw_disk_sst *self, lxw_disk_sst_stats *stats)
{
    if (stats)
        *stats = self->stats;
}

/*
 * Test seam: fail the next backend write after the given number of
 * successful write events.
 */
void
lxw_disk_sst_inject_write_failure(lxw_disk_sst *self, uint32_t after)
{
    self->inject_fail_after = after;
}

/*
 * Test seam: make every backend read fail.
 */
void
lxw_disk_sst_inject_read_failure(lxw_disk_sst *self, uint8_t enable)
{
    self->inject_read_failure = enable;
}
