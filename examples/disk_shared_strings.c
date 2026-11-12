/*
 * Example of writing a large workbook in constant_memory mode while keeping
 * a shared string table backed by temporary files. Repeated device names and
 * category values are stored once and referenced by index, but the unique
 * strings and their lookup index spill to disk instead of accumulating in
 * memory.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright 2014-2026, John McNamara, jmcnamara@cpan.org.
 *
 */

#include "xlsxwriter.h"

int
main(void)
{
    lxw_row_t row;
    lxw_row_t max_row = 10000;

    const char *categories[] = { "sensor", "gateway", "router", "camera",
                                 "actuator" };
    const char *devices[] = { "dev-alpha", "dev-bravo", "dev-charlie",
                              "dev-delta", "dev-echo" };

    /* Set the worksheet options. The disk SST requires constant_memory and
     * bounds its resident index/key cache to the given byte budget. */
    lxw_workbook_options options = {
        .constant_memory = LXW_TRUE,
        .tmpdir = NULL,
        .use_zip64 = LXW_FALSE,
        .output_buffer = NULL,
        .output_buffer_size = NULL,
        .disk_shared_strings = LXW_TRUE,
        .sst_cache_size = 64 * 1024,
    };

    /* Create a new workbook with options. */
    lxw_workbook *workbook =
        workbook_new_opt("disk_shared_strings.xlsx", &options);
    lxw_worksheet *worksheet = workbook_add_worksheet(workbook, NULL);

    for (row = 0; row < max_row; row++) {
        char identifier[64];

        /* Repeated category and device values share their string indices. */
        worksheet_write_string(worksheet, row, 0,
                               categories[row % 5], NULL);
        worksheet_write_string(worksheet, row, 1,
                               devices[row % 5], NULL);

        /* A mostly unique identifier still only costs one cache entry. */
        lxw_snprintf(identifier, sizeof(identifier), "unit-%05lu",
                     (unsigned long) row);
        worksheet_write_string(worksheet, row, 2, identifier, NULL);

        worksheet_write_number(worksheet, row, 3, (double) row, NULL);
    }

    return workbook_close(workbook);
}
