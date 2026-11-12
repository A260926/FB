/**********************************************************************************************/
/* The MIT License                                                                            */
/*                                                                                            */
/* Copyright 2016-2017 Twitch Interactive, Inc. or its affiliates. All Rights Reserved.       */
/*                                                                                            */
/* Permission is hereby granted, free of charge, to any person obtaining a copy               */
/* of this software and associated documentation files (the "Software"), to deal              */
/* in the Software without restriction, including without limitation the rights               */
/* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell                  */
/* copies of the Software, and to permit persons to whom the Software is                      */
/* furnished to do so, subject to the following conditions:                                   */
/*                                                                                            */
/* The above copyright notice and this permission notice shall be included in                 */
/* all copies or substantial portions of the Software.                                        */
/*                                                                                            */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR                 */
/* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,                   */
/* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE                */
/* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER                     */
/* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,              */
/* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN                  */
/* THE SOFTWARE.                                                                              */
/**********************************************************************************************/
// Demonstrates the dtvcc_decoder_t API: feed CEA-708 user data carrying native
// DTVCC packets and read back per-service window snapshots. The decoder covers
// transport and window/text state only, layout and rendering are left to the
// caller. See README.md for the supported command subset.

#include "caption/dtvcc.h"
#include <stdio.h>

static void dump_windows(dtvcc_decoder_t* dec)
{
    for (int service = 1; service < DTVCC_SERVICE_COUNT; ++service) {
        for (int window = 0; window < DTVCC_WINDOW_COUNT; ++window) {
            dtvcc_window_snapshot_t snap;

            if (!dtvcc_decoder_window_snapshot(dec, service, window, &snap)) {
                continue;
            }

            printf("service %d window %d: %s, priority %d, anchor (%d,%d) point %d, %d rows x %d cols, pen (%d,%d)\n",
                service, window, snap.visible ? "visible" : "hidden", snap.priority,
                snap.anchor_vertical, snap.anchor_horizontal, snap.anchor_point,
                snap.row_count, snap.col_count, snap.pen_row, snap.pen_col);

            for (int row = 0; row < snap.row_count; ++row) {
                printf("  [%2d] '%s'\n", row, snap.text[row]);
            }
        }
    }
}

int main(void)
{
    // A hand assembled DTVCC packet: service 1 defines window 0 (visible,
    // 2 rows x 8 cols), writes "HI" and displays it. Byte layout:
    //   2B                service 1, 11 byte block
    //   98 3C 14 01 07 00 DF0: visible, row/col lock, priority 4, anchor (1,4), 2 rows, 8 cols
    //   80                CW0
    //   48 49             "HI"
    //   89 01             DSW mask 0x01
    //   00                padding
    cea708_t cea708;
    cea708_init(&cea708, 1.0);
    cea708_add_cc_data(&cea708, 1, cc_type_dtvcc_packet_start, 0x072B); // seq 0, size 7
    cea708_add_cc_data(&cea708, 1, cc_type_dtvcc_packet_data, 0x983C);
    cea708_add_cc_data(&cea708, 1, cc_type_dtvcc_packet_data, 0x1401);
    cea708_add_cc_data(&cea708, 1, cc_type_dtvcc_packet_data, 0x0700);
    cea708_add_cc_data(&cea708, 1, cc_type_dtvcc_packet_data, 0x8048);
    cea708_add_cc_data(&cea708, 1, cc_type_dtvcc_packet_data, 0x4989);
    cea708_add_cc_data(&cea708, 1, cc_type_dtvcc_packet_data, 0x0100);

    // take the H.264 render/parse round trip, as it would arrive from a stream
    uint8_t wire[256];
    int size = cea708_render(&cea708, wire, sizeof(wire));
    cea708_t parsed;
    cea708_init(&parsed, 1.0);

    if (LIBCAPTION_OK != cea708_parse_h264(wire, size, &parsed)) {
        fprintf(stderr, "failed to parse cea708 user data\n");
        return 1;
    }

    dtvcc_decoder_t* dec = dtvcc_decoder_create();
    dtvcc_status_t status = dtvcc_decoder_decode(dec, &parsed);

    if (DTVCC_STATUS_OK != status) {
        fprintf(stderr, "dtvcc decode failed: %s\n", DTVCC_STATUS_UNSUPPORTED == status ? "unsupported command" : "format error");
        dtvcc_decoder_free(dec);
        return 1;
    }

    dump_windows(dec);
    dtvcc_decoder_free(dec);
    return 0;
}
