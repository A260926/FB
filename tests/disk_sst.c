/*
 * Regression tests for the disk backed shared string table.
 *
 * The tests are grouped into identity, spill and errors. Every check goes
 * through CHECK_CHECK so a failure returns a negative result, and each
 * group returns the number of positive cases it exercised.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright 2014-2026, John McNamara, jmcnamara@cpan.org.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "xlsxwriter.h"
#include "xlsxwriter/disk_sst.h"

#include "cases.h"
#include "unzip.h"

#define TEST_BUDGET 4096

/*
 * Read one zip member into a freshly allocated NUL terminated buffer.
 */
static int
read_zip_member(const char *path, const char *member, char **out,
                size_t *out_len)
{
    unzFile uf;
    int rc = -1;

    *out = NULL;
    if (out_len)
        *out_len = 0;

    uf = unzOpen(path);
    if (!uf)
        return -1;

    if (unzLocateFile(uf, member, 0) != UNZ_OK)
        goto cleanup;

    if (unzOpenCurrentFile(uf) != UNZ_OK)
        goto cleanup;

    {
        unz_file_info64 info;
        char *buffer;
        size_t total = 0;
        int read_bytes;

        if (unzGetCurrentFileInfo64(uf, &info, NULL, 0, NULL, 0, NULL, 0)
            != UNZ_OK)
            goto close_member;

        buffer = malloc((size_t) info.uncompressed_size + 1);
        if (!buffer)
            goto close_member;

        while (total < (size_t) info.uncompressed_size) {
            read_bytes = unzReadCurrentFile(uf, buffer + total,
                                            (unsigned) ((size_t)
                                                        info.uncompressed_size
                                                        - total));
            if (read_bytes < 0) {
                free(buffer);
                goto close_member;
            }
            if (read_bytes == 0)
                break;
            total += (size_t) read_bytes;
        }

        buffer[total] = '\0';
        *out = buffer;
        if (out_len)
            *out_len = total;
        rc = 0;

close_member:
        unzCloseCurrentFile(uf);
    }

cleanup:
    unzClose(uf);
    return rc;
}

/*
 * Count non overlapping occurrences of needle in haystack.
 */
static int
count_occurrences(const char *haystack, const char *needle)
{
    int count = 0;
    size_t needle_len = strlen(needle);
    const char *position = haystack;

    while ((position = strstr(position, needle)) != NULL) {
        count++;
        position += needle_len;
    }

    return count;
}

/*
 * Tiny insertion ordered map of the indices the test itself expects.
 */
#define MAX_EXPECTED 512

struct expected_entry {
    char key[160];
    uint8_t is_rich_string;
    uint32_t index;
};

struct expected_map {
    struct expected_entry entries[MAX_EXPECTED];
    uint32_t count;
};

static void
expected_map_init(struct expected_map *map)
{
    map->count = 0;
}

static int
expected_map_find(struct expected_map *map, const char *key,
                  uint8_t is_rich_string, uint32_t *index)
{
    uint32_t i;

    for (i = 0; i < map->count; i++) {
        if (map->entries[i].is_rich_string == is_rich_string &&
            strcmp(map->entries[i].key, key) == 0) {
            *index = map->entries[i].index;
            return 1;
        }
    }

    return 0;
}

static void
expected_map_add(struct expected_map *map, const char *key,
                 uint8_t is_rich_string, uint32_t index)
{
    uint32_t unused_index;

    if (expected_map_find(map, key, is_rich_string, &unused_index))
        return;

    snprintf(map->entries[map->count].key,
             sizeof(map->entries[map->count].key), "%s", key);
    map->entries[map->count].is_rich_string = is_rich_string;
    map->entries[map->count].index = index;
    map->count++;
}

static void
test_path(char *buffer, size_t size, const char *basename)
{
    const char *tmpdir = getenv("TMPDIR");

    if (!tmpdir || !*tmpdir)
        tmpdir = "/tmp";

    snprintf(buffer, size, "%s/%s", tmpdir, basename);
}

static long
file_size(const char *path)
{
    FILE *file = fopen(path, "rb");
    long size;

    if (!file)
        return -1;

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fclose(file);
    return size;
}

/*****************************************************************************
 *
 * identity: stable numbering, kind separation, encoding and old behavior.
 *
 ****************************************************************************/
static int
run_identity(void)
{
    int cases = 0;
    char path[1024];
    lxw_workbook *workbook;
    lxw_worksheet *sheet1;
    lxw_worksheet *sheet2;
    lxw_workbook_options options = { 0 };
    struct expected_map map;
    lxw_disk_sst_stats stats;
    lxw_error err;
    uint32_t index;
    uint32_t i;
    uint32_t final_unique;
    uint32_t final_refs;
    char string[128];
    char *shared_xml = NULL;
    char *sheet_xml = NULL;
    char cell_ref[64];

    const char *categories[] = { "sensor", "gateway", "router", "camera",
                                 "actuator" };
    const char *devices[] = { "dev-alpha", "dev-bravo", "dev-charlie",
                              "dev-delta", "dev-echo", "dev-foxtrot",
                              "dev-golf", "dev-hotel" };

    test_path(path, sizeof(path), "disk_sst_identity.xlsx");

    options.constant_memory = LXW_TRUE;
    options.disk_shared_strings = LXW_TRUE;
    options.sst_cache_size = TEST_BUDGET;

    workbook = workbook_new_opt(path, &options);
    CHECK_CHECK(workbook != NULL);
    CHECK_CHECK(workbook->sst->disk_mode == LXW_TRUE);
    CHECK_CHECK(workbook->sst->disk != NULL);
    cases++;

    sheet1 = workbook_add_worksheet(workbook, "one");
    sheet2 = workbook_add_worksheet(workbook, "two");
    expected_map_init(&map);

    /* Generate repeated values across two sheets, recording the index
     * assigned to every new key without adding extra references. */
    for (i = 0; i < 200; i++) {
        const char *name = devices[i % 8];
        const char *category = categories[i % 5];

        snprintf(string, sizeof(string), "%s/%s-%03d", category, name,
                 i % 200);

        err = worksheet_write_string(sheet1, i, 0, string, NULL);
        CHECK_CHECK(err == LXW_NO_ERROR);

        if (!expected_map_find(&map, string, LXW_FALSE, &index)) {
            /* First registration: the newest unique index is count-1. */
            expected_map_add(&map, string, LXW_FALSE,
                             workbook->sst->unique_count - 1);
        }

        /* Repeated plain value on a second sheet reuses the numbering. */
        err = worksheet_write_string(sheet2, i, 0, name, NULL);
        CHECK_CHECK(err == LXW_NO_ERROR);
        if (!expected_map_find(&map, name, LXW_FALSE, &index)) {
            expected_map_add(&map, name, LXW_FALSE,
                             workbook->sst->unique_count - 1);
        }
    }

    /* 200 generated strings plus the 8 device names = 208 uniques. */
    CHECK_CHECK(workbook->sst->unique_count == 208);
    CHECK_CHECK(workbook->sst->unique_count == map.count);
    CHECK_CHECK(workbook->sst->string_count == 400);

    /* The small budget must have forced real spill and disk lookups. */
    lxw_disk_sst_get_stats(workbook->sst->disk, &stats);
    CHECK_CHECK(stats.cached_entries > 0);
    CHECK_CHECK(stats.evictions > 0);
    CHECK_CHECK(stats.cache_hits > 0);
    CHECK_CHECK(stats.disk_lookups > 0);
    CHECK_CHECK(stats.index_bytes_peak > 0);
    CHECK_CHECK(stats.cache_bytes_peak > 0);
    CHECK_CHECK(stats.index_bytes_peak + stats.cache_bytes_peak
                <= TEST_BUDGET + 256);
    cases++;

    /* Whitespace, Unicode and control characters keep their handling. */
    err = worksheet_write_string(sheet1, 200, 0, "  padded  ", NULL);
    CHECK_CHECK(err == LXW_NO_ERROR);
    err = worksheet_write_string(sheet1, 201, 0, "caf\xC3\xA9", NULL);
    CHECK_CHECK(err == LXW_NO_ERROR);
    err = worksheet_write_string(sheet1, 202, 0,
                                 "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",
                                 NULL);
    CHECK_CHECK(err == LXW_NO_ERROR);
    err = worksheet_write_string(sheet1, 203, 0,
                                 "a\x01" "b\x02" "c", NULL);
    CHECK_CHECK(err == LXW_NO_ERROR);

    /* Empty strings keep their original blank/ignore convention. */
    err = worksheet_write_string(sheet1, 204, 0, "", NULL);
    CHECK_CHECK(err == LXW_NO_ERROR);
    err = worksheet_write_string(sheet1, 205, 0, "",
                                 workbook_add_format(workbook));
    CHECK_CHECK(err == LXW_NO_ERROR);

    /* Overlong strings are still rejected. */
    {
        char *long_string = malloc(32769);
        CHECK_CHECK(long_string != NULL);
        memset(long_string, 'x', 32768);
        long_string[32768] = '\0';
        err = worksheet_write_string(sheet1, 206, 0, long_string, NULL);
        CHECK_CHECK(err == LXW_ERROR_MAX_STRING_LENGTH_EXCEEDED);
        free(long_string);
    }

    /* Writing to an already flushed row keeps the old error convention. */
    err = worksheet_write_string(sheet1, 199, 0, "late row", NULL);
    CHECK_CHECK(err == LXW_ERROR_WORKSHEET_INDEX_OUT_OF_RANGE);

    /* Numbering stays stable after eviction: re-writing an early value adds
     * a reference but never a new unique index. */
    {
        uint32_t unique_before = workbook->sst->unique_count;
        uint32_t refs_before = workbook->sst->string_count;
        snprintf(string, sizeof(string), "%s/%s-%03d", categories[0],
                 devices[0], 0);
        CHECK_CHECK(expected_map_find(&map, string, LXW_FALSE, &index));
        err = worksheet_write_string(sheet1, 208, 0, string, NULL);
        CHECK_CHECK(err == LXW_NO_ERROR);
        CHECK_CHECK(workbook->sst->unique_count == unique_before);
        CHECK_CHECK(workbook->sst->string_count == refs_before + 1);
    }
    cases++;

    /* Plain text and rich text are distinct kinds and identities. */
    {
        lxw_format *bold = workbook_add_format(workbook);
        lxw_format *italic = workbook_add_format(workbook);
        lxw_rich_string_tuple f1 = { .format = NULL, .string = "kind" };
        lxw_rich_string_tuple f2 = { .format = bold, .string = "text" };
        lxw_rich_string_tuple *rich_bold[] = { &f1, &f2, NULL };

        lxw_rich_string_tuple g1 = { .format = NULL, .string = "kind" };
        lxw_rich_string_tuple g2 = { .format = italic, .string = "text" };
        lxw_rich_string_tuple *rich_italic[] = { &g1, &g2, NULL };

        uint32_t before = workbook->sst->unique_count;

        format_set_bold(bold);
        format_set_italic(italic);

        /* Same display text, plain kind first. */
        err = worksheet_write_string(sheet1, 210, 0, "kindtext", NULL);
        CHECK_CHECK(err == LXW_NO_ERROR);
        CHECK_CHECK(workbook->sst->unique_count == before + 1);

        /* The rich form is a different identity, not a merge. */
        err = worksheet_write_rich_string(sheet1, 211, 0, rich_bold, NULL);
        CHECK_CHECK(err == LXW_NO_ERROR);
        CHECK_CHECK(workbook->sst->unique_count == before + 2);

        /* Different fragment formatting is again a distinct rich value. */
        err = worksheet_write_rich_string(sheet1, 212, 0, rich_italic,
                                          NULL);
        CHECK_CHECK(err == LXW_NO_ERROR);
        CHECK_CHECK(workbook->sst->unique_count == before + 3);

        /* Identical rich bytes reuse the existing index and number. */
        err = worksheet_write_rich_string(sheet1, 213, 0, rich_bold, NULL);
        CHECK_CHECK(err == LXW_NO_ERROR);
        CHECK_CHECK(workbook->sst->unique_count == before + 3);
        cases++;
    }

    /* Snapshot the table counters before close frees the workbook. */
    final_refs = workbook->sst->string_count;
    final_unique = workbook->sst->unique_count;
    CHECK_CHECK(final_refs == 409);
    CHECK_CHECK(final_unique == 215);

    err = workbook_close(workbook);
    CHECK_CHECK(err == LXW_NO_ERROR);
    cases++;

    /* The package must contain a real sharedStrings.xml, not inline str. */
    CHECK_CHECK(read_zip_member(path, "xl/sharedStrings.xml", &shared_xml,
                                NULL) == 0);
    CHECK_CHECK(read_zip_member(path, "xl/worksheets/sheet1.xml",
                                &sheet_xml, NULL) == 0);
    CHECK_CHECK(shared_xml != NULL && sheet_xml != NULL);

    /* 208 loop uniques (400 refs), one early repeat, 4 special strings, then
     * the plain plus 2 rich kind identities (4 refs, 3 uniques) give 215
     * uniques and 409 references. Empty strings never enter the table. */
    {
        char expected_count[32];
        char expected_unique[32];
        snprintf(expected_count, sizeof(expected_count), "count=\"%u\"",
                 final_refs);
        snprintf(expected_unique, sizeof(expected_unique),
                 "uniqueCount=\"%u\"", final_unique);
        CHECK_CHECK(strstr(shared_xml, expected_count) != NULL);
        CHECK_CHECK(strstr(shared_xml, expected_unique) != NULL);
        CHECK_CHECK(count_occurrences(shared_xml, "<si>")
                    == (int) final_unique);
    }

    /* Cells use t="s" with the shared index; inlineStr must not appear. */
    CHECK_CHECK(strstr(sheet_xml, "t=\"s\"") != NULL);
    CHECK_CHECK(strstr(sheet_xml, "inlineStr") == NULL);
    CHECK_CHECK(count_occurrences(sheet_xml, "t=\"s\"") >= 200);

    /* Whitespace preservation, control escaping and Unicode survive. */
    CHECK_CHECK(strstr(shared_xml, "xml:space=\"preserve\"") != NULL);
    CHECK_CHECK(strstr(shared_xml, "_x0001_") != NULL);
    CHECK_CHECK(strstr(shared_xml, "caf\xC3\xA9") != NULL);
    CHECK_CHECK(strstr(shared_xml,
                       "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E") != NULL);

    /* The same display text exists once as plain and once as rich runs. */
    CHECK_CHECK(strstr(shared_xml, "<t>kindtext</t>") != NULL);
    CHECK_CHECK(count_occurrences(shared_xml, "<t>kind</t>") >= 1);
    CHECK_CHECK(count_occurrences(shared_xml, "<t>text</t>") >= 2);
    CHECK_CHECK(strstr(shared_xml, "<rPr>") != NULL);

    /* A known cell references its expected shared index. */
    snprintf(string, sizeof(string), "%s/%s-%03d", categories[0],
             devices[0], 0);
    CHECK_CHECK(expected_map_find(&map, string, LXW_FALSE, &index));
    snprintf(cell_ref, sizeof(cell_ref),
             "<c r=\"A1\" t=\"s\"><v>%u</v></c>", index);
    CHECK_CHECK(strstr(sheet_xml, cell_ref) != NULL);

    free(shared_xml);
    free(sheet_xml);
    cases++;

    return cases;
}

/*****************************************************************************
 *
 * spill: budget far below the unique count, repeats, collisions and size.
 *
 ****************************************************************************/
static int
run_spill(void)
{
    int cases = 0;
    char sst_path[1024];
    char inline_path[1024];
    lxw_workbook *workbook;
    lxw_worksheet *sheet;
    lxw_workbook_options options = { 0 };
    lxw_disk_sst_stats stats;
    lxw_error err;
    uint32_t i;
    uint32_t index;
    char unique[64];
    char repeat[64];
    char cell_ref[64];
    char *shared_xml = NULL;
    char *sheet_xml = NULL;
    size_t sst_shared_len = 0;
    size_t sst_sheet_len = 0;
    uint32_t spill_final_refs = 0;
    uint32_t spill_final_uniques = 0;
    const uint32_t num_unique = 500;

    test_path(sst_path, sizeof(sst_path), "disk_sst_spill.xlsx");
    test_path(inline_path, sizeof(inline_path), "disk_sst_inline.xlsx");

    options.constant_memory = LXW_TRUE;
    options.disk_shared_strings = LXW_TRUE;
    options.sst_cache_size = TEST_BUDGET;

    workbook = workbook_new_opt(sst_path, &options);
    CHECK_CHECK(workbook != NULL);
    sheet = workbook_add_worksheet(workbook, NULL);

    /* Many more unique values than the cache can hold, plus early repeats. */
    for (i = 0; i < num_unique; i++) {
        snprintf(unique, sizeof(unique), "device-%04u", i);
        snprintf(repeat, sizeof(repeat), "device-%04u", i % 10);

        err = worksheet_write_string(sheet, i, 0, unique, NULL);
        CHECK_CHECK(err == LXW_NO_ERROR);
        err = worksheet_write_string(sheet, i, 1, repeat, NULL);
        CHECK_CHECK(err == LXW_NO_ERROR);
        err = worksheet_write_string(sheet, i, 2, "device-0000", NULL);
        CHECK_CHECK(err == LXW_NO_ERROR);

        /* Early values keep their index long after being evicted. */
        if (i == 400) {
            err = lxw_sst_lookup_index(workbook->sst, "device-0003",
                                       LXW_FALSE, &index);
            CHECK_CHECK(err == LXW_NO_ERROR);
            CHECK_CHECK(index == 3);
            err = lxw_sst_lookup_index(workbook->sst, "device-0000",
                                       LXW_FALSE, &index);
            CHECK_CHECK(err == LXW_NO_ERROR);
            CHECK_CHECK(index == 0);
        }
    }

    CHECK_CHECK(workbook->sst->unique_count == num_unique);
    /* The two explicit lookups at i == 400 each add one reference. */
    CHECK_CHECK(workbook->sst->string_count == num_unique * 3 + 2);

    lxw_disk_sst_get_stats(workbook->sst->disk, &stats);
    CHECK_CHECK(stats.evictions > 100);
    CHECK_CHECK(stats.disk_lookups > 0);
    CHECK_CHECK(stats.cache_hits > 0);
    CHECK_CHECK(stats.index_bytes + stats.cache_bytes <= TEST_BUDGET);
    CHECK_CHECK(stats.index_bytes_peak + stats.cache_bytes_peak
                <= TEST_BUDGET + 256);
    cases++;

    /* Genuine hash bucket collision: distinct keys, distinct hashes,
     * same bucket, both evicted and still resolved correctly. */
    {
        char first[64];
        char second[64];
        uint32_t h1;
        uint32_t h2;
        uint32_t idx1;
        uint32_t idx2;
        int found = 0;

        snprintf(first, sizeof(first), "coll-%04u", 0);
        h1 = lxw_disk_sst_hash(first, LXW_FALSE);

        for (i = 1; i < 5000 && !found; i++) {
            snprintf(second, sizeof(second), "coll-%04u", i);
            h2 = lxw_disk_sst_hash(second, LXW_FALSE);
            if ((h1 % LXW_SST_BUCKETS) == (h2 % LXW_SST_BUCKETS) &&
                h1 != h2)
                found = 1;
        }
        CHECK_CHECK(found);

        err = worksheet_write_string(sheet, num_unique + 10, 0, first,
                                     NULL);
        CHECK_CHECK(err == LXW_NO_ERROR);
        err = worksheet_write_string(sheet, num_unique + 11, 0, second,
                                     NULL);
        CHECK_CHECK(err == LXW_NO_ERROR);

        err = lxw_sst_lookup_index(workbook->sst, first, LXW_FALSE,
                                   &idx1);
        CHECK_CHECK(err == LXW_NO_ERROR);
        err = lxw_sst_lookup_index(workbook->sst, second, LXW_FALSE,
                                   &idx2);
        CHECK_CHECK(err == LXW_NO_ERROR);
        CHECK_CHECK(idx1 != idx2);

        /* Force both out of the cache and resolve through the disk chain. */
        for (i = 0; i < 100; i++) {
            snprintf(unique, sizeof(unique), "flush-%04u", i);
            err = worksheet_write_string(sheet, num_unique + 20 + i, 0,
                                         unique, NULL);
            CHECK_CHECK(err == LXW_NO_ERROR);
        }

        err = lxw_sst_lookup_index(workbook->sst, first, LXW_FALSE,
                                   &index);
        CHECK_CHECK(err == LXW_NO_ERROR);
        CHECK_CHECK(index == idx1);
        err = lxw_sst_lookup_index(workbook->sst, second, LXW_FALSE,
                                   &index);
        CHECK_CHECK(err == LXW_NO_ERROR);
        CHECK_CHECK(index == idx2);
        cases++;
    }

    /* Snapshot the counters before close frees the workbook. Base loop:
     * 1500 refs / 500 uniques plus the two i == 400 lookups; collision
     * block: 2 writes + 2 lookups + 100 flush writes + 2 verification
     * lookups = 108 more refs and 102 more uniques. */
    {
        uint32_t final_refs = workbook->sst->string_count;
        uint32_t final_uniques = workbook->sst->unique_count;
        CHECK_CHECK(final_uniques == num_unique + 102);
        CHECK_CHECK(final_refs == num_unique * 3 + 108);
        spill_final_refs = final_refs;
        spill_final_uniques = final_uniques;
    }

    err = workbook_close(workbook);
    CHECK_CHECK(err == LXW_NO_ERROR);
    cases++;

    CHECK_CHECK(read_zip_member(sst_path, "xl/sharedStrings.xml",
                                &shared_xml, &sst_shared_len) == 0);
    CHECK_CHECK(read_zip_member(sst_path, "xl/worksheets/sheet1.xml",
                                &sheet_xml, &sst_sheet_len) == 0);

    {
        char expected_count[64];
        char expected_unique[64];
        const char *first_si;

        snprintf(expected_count, sizeof(expected_count), "count=\"%u\"",
                 spill_final_refs);
        snprintf(expected_unique, sizeof(expected_unique),
                 "uniqueCount=\"%u\"", spill_final_uniques);

        CHECK_CHECK(strstr(shared_xml, expected_count) != NULL);
        CHECK_CHECK(strstr(shared_xml, expected_unique) != NULL);
        CHECK_CHECK(count_occurrences(shared_xml, "<si>")
                    == (int) spill_final_uniques);

        /* The table is emitted in stable index order. */
        first_si = strstr(shared_xml, "<si>");
        CHECK_CHECK(first_si && strstr(first_si, "device-0000") != NULL);
        CHECK_CHECK(strstr(shared_xml, "device-0499") != NULL);
        CHECK_CHECK(strstr(shared_xml, "device-0000")
                    < strstr(shared_xml, "device-0499"));
    }

    /* The repeat cell references the original shared index. */
    snprintf(cell_ref, sizeof(cell_ref),
             "<c r=\"A401\" t=\"s\"><v>400</v></c>");
    CHECK_CHECK(strstr(sheet_xml, cell_ref) != NULL);
    snprintf(cell_ref, sizeof(cell_ref),
             "<c r=\"C401\" t=\"s\"><v>0</v></c>");
    CHECK_CHECK(strstr(sheet_xml, cell_ref) != NULL);
    CHECK_CHECK(strstr(sheet_xml, "inlineStr") == NULL);

    free(shared_xml);
    free(sheet_xml);
    cases++;

    /* Same data through the default inlineStr path. Deflate hides much of
     * the difference at the zip level, so compare the uncompressed XML
     * payloads (the actual file volume the repeated text would occupy). */
    {
        lxw_workbook *inline_workbook;
        lxw_worksheet *inline_sheet;
        lxw_workbook_options inline_options = { 0 };
        char *inline_sheet_xml = NULL;
        size_t inline_sheet_len = 0;

        inline_options.constant_memory = LXW_TRUE;
        inline_workbook = workbook_new_opt(inline_path, &inline_options);
        CHECK_CHECK(inline_workbook != NULL);
        inline_sheet = workbook_add_worksheet(inline_workbook, NULL);

        for (i = 0; i < num_unique; i++) {
            snprintf(unique, sizeof(unique), "device-%04u", i);
            snprintf(repeat, sizeof(repeat), "device-%04u", i % 10);
            CHECK_CHECK(worksheet_write_string(inline_sheet, i, 0, unique,
                                               NULL) == LXW_NO_ERROR);
            CHECK_CHECK(worksheet_write_string(inline_sheet, i, 1, repeat,
                                               NULL) == LXW_NO_ERROR);
            CHECK_CHECK(worksheet_write_string(inline_sheet, i, 2,
                                               "device-0000", NULL)
                        == LXW_NO_ERROR);
        }

        CHECK_CHECK(workbook_close(inline_workbook) == LXW_NO_ERROR);
        CHECK_CHECK(file_size(sst_path) > 0);
        CHECK_CHECK(file_size(inline_path) > 0);
        CHECK_CHECK(read_zip_member(inline_path,
                                    "xl/worksheets/sheet1.xml",
                                    &inline_sheet_xml, &inline_sheet_len)
                    == 0);
        CHECK_CHECK(inline_sheet_xml != NULL);
        CHECK_CHECK(strstr(inline_sheet_xml, "inlineStr") != NULL);

        /* The shared mode worksheet plus its string table is still smaller
         * than the single inline worksheet carrying every repeated copy. */
        CHECK_CHECK(inline_sheet_len
                    > sst_sheet_len + sst_shared_len);
        CHECK_CHECK(inline_sheet_len > sst_sheet_len);

        free(inline_sheet_xml);
        cases++;
    }

    return cases;
}

/*****************************************************************************
 *
 * errors: option validation, controlled disk failures and independence.
 *
 ****************************************************************************/
static int
run_errors(void)
{
    int cases = 0;
    char path[1024];
    lxw_workbook_options options = { 0 };
    lxw_workbook *workbook;
    lxw_worksheet *sheet;
    lxw_error err;
    uint32_t index;
    uint32_t before;
    uint32_t i;
    char string[64];
    lxw_disk_sst_stats stats;

    /* disk_shared_strings requires constant_memory. */
    memset(&options, 0, sizeof(options));
    options.constant_memory = LXW_FALSE;
    options.disk_shared_strings = LXW_TRUE;
    test_path(path, sizeof(path), "disk_sst_bad_mode.xlsx");
    CHECK_CHECK(workbook_new_opt(path, &options) == NULL);

    /* Sub-4KiB non-zero budgets are rejected, including boundary values. */
    memset(&options, 0, sizeof(options));
    options.constant_memory = LXW_TRUE;
    options.disk_shared_strings = LXW_TRUE;
    options.sst_cache_size = 100;
    CHECK_CHECK(workbook_new_opt(path, &options) == NULL);
    options.sst_cache_size = LXW_SST_MIN_CACHE_SIZE - 1;
    CHECK_CHECK(workbook_new_opt(path, &options) == NULL);

    /* Zero selects the documented default and 4096 is accepted. */
    options.sst_cache_size = 0;
    workbook = workbook_new_opt(path, &options);
    CHECK_CHECK(workbook != NULL);
    CHECK_CHECK(workbook->sst->disk->cache_budget
                == LXW_SST_DEFAULT_CACHE_SIZE);
    CHECK_CHECK(workbook_close(workbook) == LXW_NO_ERROR);

    options.sst_cache_size = LXW_SST_MIN_CACHE_SIZE;
    workbook = workbook_new_opt(path, &options);
    CHECK_CHECK(workbook != NULL);
    CHECK_CHECK(workbook->sst->disk->cache_budget == LXW_SST_MIN_CACHE_SIZE);
    CHECK_CHECK(workbook_close(workbook) == LXW_NO_ERROR);

    /* The backend constructor rejects the same budget directly. */
    {
        lxw_disk_sst *tmp;
        CHECK_CHECK(lxw_disk_sst_new(NULL, 1024) == NULL);
        tmp = lxw_disk_sst_new(NULL, 0);
        CHECK_CHECK(tmp != NULL);
        lxw_disk_sst_free(tmp);
    }
    cases++;

    /* Enabling disk mode twice or on a used table is rejected. */
    {
        lxw_sst *sst = lxw_sst_new();
        CHECK_CHECK(sst != NULL);
        CHECK_CHECK(lxw_sst_enable_disk_mode(sst, NULL, TEST_BUDGET)
                    == LXW_NO_ERROR);
        CHECK_CHECK(lxw_sst_enable_disk_mode(sst, NULL, TEST_BUDGET)
                    == LXW_ERROR_PARAMETER_VALIDATION);
        lxw_sst_free(sst);
        cases++;
    }

    /* Controlled backend write failure: no index is consumed by failure. */
    {
        lxw_disk_sst *disk = lxw_disk_sst_new(NULL, TEST_BUDGET);
        CHECK_CHECK(disk != NULL);

        err = lxw_disk_sst_get_index(disk, "one", LXW_FALSE, &index, NULL);
        CHECK_CHECK(err == LXW_NO_ERROR);
        CHECK_CHECK(index == 0);

        /* Fail before the second unique value performs any backend write. */
        lxw_disk_sst_inject_write_failure(disk, 1);

        err = lxw_disk_sst_get_index(disk, "two", LXW_FALSE, &index, NULL);
        CHECK_CHECK(err == LXW_ERROR_CREATING_TMPFILE);
        CHECK_CHECK(disk->unique_count == 1);

        /* The error is sticky and the failed entry never took a number. */
        err = lxw_disk_sst_get_index(disk, "three", LXW_FALSE, &index,
                                     NULL);
        CHECK_CHECK(err == LXW_ERROR_CREATING_TMPFILE);
        CHECK_CHECK(disk->unique_count == 1);

        lxw_disk_sst_free(disk);

        /* A fresh table restarts numbering: no shared global state. */
        disk = lxw_disk_sst_new(NULL, TEST_BUDGET);
        CHECK_CHECK(disk != NULL);
        err = lxw_disk_sst_get_index(disk, "one", LXW_FALSE, &index, NULL);
        CHECK_CHECK(err == LXW_NO_ERROR);
        CHECK_CHECK(index == 0);
        lxw_disk_sst_free(disk);
        cases++;
    }

    /* The failure propagates through the worksheet and workbook close. */
    test_path(path, sizeof(path), "disk_sst_failed.xlsx");
    memset(&options, 0, sizeof(options));
    options.constant_memory = LXW_TRUE;
    options.disk_shared_strings = LXW_TRUE;
    options.sst_cache_size = TEST_BUDGET;

    workbook = workbook_new_opt(path, &options);
    CHECK_CHECK(workbook != NULL);
    sheet = workbook_add_worksheet(workbook, NULL);

    CHECK_CHECK(worksheet_write_string(sheet, 0, 0, "one", NULL)
                == LXW_NO_ERROR);
    before = workbook->sst->unique_count;

    lxw_disk_sst_inject_write_failure(workbook->sst->disk, 1);

    err = worksheet_write_string(sheet, 1, 0, "two", NULL);
    CHECK_CHECK(err == LXW_ERROR_CREATING_TMPFILE);
    CHECK_CHECK(workbook->sst->unique_count == before);

    /* Close must report this workbook's failure, not a fake success. */
    err = workbook_close(workbook);
    CHECK_CHECK(err != LXW_NO_ERROR);
    cases++;

    /* A failure during eviction writes is reported the same way. */
    test_path(path, sizeof(path), "disk_sst_evict_fail.xlsx");
    workbook = workbook_new_opt(path, &options);
    CHECK_CHECK(workbook != NULL);
    sheet = workbook_add_worksheet(workbook, NULL);
    lxw_disk_sst_inject_write_failure(workbook->sst->disk, 60);

    {
        int saw_failure = 0;
        for (i = 0; i < 200; i++) {
            snprintf(string, sizeof(string), "bulk-%04u", i);
            err = worksheet_write_string(sheet, i, 0, string, NULL);
            if (err != LXW_NO_ERROR) {
                CHECK_CHECK(err == LXW_ERROR_CREATING_TMPFILE);
                saw_failure = 1;
                break;
            }
        }
        CHECK_CHECK(saw_failure);
        CHECK_CHECK(workbook_close(workbook) != LXW_NO_ERROR);
        cases++;
    }

    /* Read failures while walking a spilled chain are propagated too. */
    {
        lxw_disk_sst *disk;
        uint32_t idx;

        disk = lxw_disk_sst_new(NULL, TEST_BUDGET);
        CHECK_CHECK(disk != NULL);

        /* Fill past the cache so later keys spill to the index file. */
        for (i = 0; i < 200; i++) {
            snprintf(string, sizeof(string), "readfail-%04u", i);
            CHECK_CHECK(lxw_disk_sst_get_index(disk, string, LXW_FALSE,
                                               &idx, NULL)
                        == LXW_NO_ERROR);
        }

        lxw_disk_sst_get_stats(disk, &stats);
        CHECK_CHECK(stats.evictions > 0);

        /* A spilled key now requires a read; force that read to fail. */
        lxw_disk_sst_inject_read_failure(disk, LXW_TRUE);
        snprintf(string, sizeof(string), "readfail-%04u", 0);
        err = lxw_disk_sst_get_index(disk, string, LXW_FALSE, &idx, NULL);
        CHECK_CHECK(err == LXW_ERROR_READING_TMPFILE);

        /* The read failure is sticky on this table. */
        {
            FILE *sink = tmpfile();
            CHECK_CHECK(sink != NULL);
            err = lxw_disk_sst_write_si(disk, sink);
            CHECK_CHECK(err == LXW_ERROR_READING_TMPFILE);
            fclose(sink);
        }
        lxw_disk_sst_inject_read_failure(disk, LXW_FALSE);
        lxw_disk_sst_free(disk);
        cases++;
    }

    /* Two workbooks interleave independently, including under failure. */
    {
        char path_a[1024];
        char path_b[1024];
        lxw_workbook *wb_a;
        lxw_workbook *wb_b;
        lxw_worksheet *ws_a;
        lxw_worksheet *ws_b;
        uint32_t idx_a1;
        uint32_t idx_a2;
        uint32_t idx_b1;
        uint32_t idx_b2;

        test_path(path_a, sizeof(path_a), "disk_sst_indep_a.xlsx");
        test_path(path_b, sizeof(path_b), "disk_sst_indep_b.xlsx");

        wb_a = workbook_new_opt(path_a, &options);
        wb_b = workbook_new_opt(path_b, &options);
        CHECK_CHECK(wb_a != NULL && wb_b != NULL);
        ws_a = workbook_add_worksheet(wb_a, NULL);
        ws_b = workbook_add_worksheet(wb_b, NULL);

        CHECK_CHECK(worksheet_write_string(ws_a, 0, 0, "alpha", NULL)
                    == LXW_NO_ERROR);
        CHECK_CHECK(worksheet_write_string(ws_b, 0, 0, "beta", NULL)
                    == LXW_NO_ERROR);
        CHECK_CHECK(worksheet_write_string(ws_a, 1, 0, "beta", NULL)
                    == LXW_NO_ERROR);
        CHECK_CHECK(worksheet_write_string(ws_b, 1, 0, "alpha", NULL)
                    == LXW_NO_ERROR);

        CHECK_CHECK(lxw_sst_lookup_index(wb_a->sst, "alpha", LXW_FALSE,
                                         &idx_a1) == LXW_NO_ERROR);
        CHECK_CHECK(lxw_sst_lookup_index(wb_a->sst, "beta", LXW_FALSE,
                                         &idx_a2) == LXW_NO_ERROR);
        CHECK_CHECK(lxw_sst_lookup_index(wb_b->sst, "beta", LXW_FALSE,
                                         &idx_b1) == LXW_NO_ERROR);
        CHECK_CHECK(lxw_sst_lookup_index(wb_b->sst, "alpha", LXW_FALSE,
                                         &idx_b2) == LXW_NO_ERROR);
        CHECK_CHECK(idx_a1 == 0 && idx_a2 == 1);
        CHECK_CHECK(idx_b1 == 0 && idx_b2 == 1);

        /* Fail A; B must keep working and close cleanly. A has completed
         * two unique writes, so fail on its third backend write. */
        lxw_disk_sst_inject_write_failure(wb_a->sst->disk, 2);
        CHECK_CHECK(worksheet_write_string(ws_a, 2, 0, "gamma", NULL)
                    == LXW_ERROR_CREATING_TMPFILE);
        CHECK_CHECK(worksheet_write_string(ws_b, 2, 0, "gamma", NULL)
                    == LXW_NO_ERROR);

        CHECK_CHECK(workbook_close(wb_a) != LXW_NO_ERROR);
        CHECK_CHECK(workbook_close(wb_b) == LXW_NO_ERROR);
        cases++;
    }

    return cases;
}

int
sst_case(const char *group)
{
    if (strcmp(group, "identity") == 0)
        return run_identity();

    if (strcmp(group, "spill") == 0)
        return run_spill();

    if (strcmp(group, "errors") == 0)
        return run_errors();

    return -1;
}
