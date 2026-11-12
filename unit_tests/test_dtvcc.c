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
#include "caption/dtvcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_cases = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

#define RUN_CASE(fn)                            \
    do {                                        \
        printf("DTVCC_CASE %s RUN\n", #fn);     \
        fn();                                   \
        g_cases += 1;                           \
        printf("DTVCC_CASE %s PASS\n", #fn);    \
    } while (0)

////////////////////////////////////////////////////////////////////////////////
// test scaffolding: a minimal DTVCC packet encoder, only used to drive the
// decoder under test. test_hand_written_packet does NOT use it, it feeds raw
// hand assembled bytes instead.
////////////////////////////////////////////////////////////////////////////////

typedef struct {
    int valid;
    int type;
    uint16_t data;
} test_cc_t;

// encodes a packet payload into cc_data entries, returns the entry count
static int test_packet_cc(const uint8_t* payload, int len, int seq, test_cc_t* out)
{
    int size_code = len / 2 + 1; // the payload capacity of a packet is always odd: 2 * size - 1
    int capacity = size_code * 2 - 1;
    uint8_t padded[128];
    CHECK(capacity <= 127);
    memcpy(padded, payload, len);

    for (int i = len; i < capacity; ++i) {
        padded[i] = 0x00; // null service block padding
    }

    int n = 0;
    out[n].valid = 1;
    out[n].type = cc_type_dtvcc_packet_start;
    out[n].data = (uint16_t)(((seq & 0x03) << 14) | (size_code << 8) | padded[0]);
    ++n;

    for (int i = 1; i < capacity; i += 2) {
        out[n].valid = 1;
        out[n].type = cc_type_dtvcc_packet_data;
        out[n].data = (uint16_t)((padded[i] << 8) | padded[i + 1]);
        ++n;
    }

    return n;
}

// feeds n cc entries to the decoder in chunks of chunk_size entries per cea708_t.
// with roundtrip != 0 each cea708_t takes the H.264 render/parse round trip first.
static dtvcc_status_t feed_cc(dtvcc_decoder_t* dec, const test_cc_t* cc, int n, int chunk_size, int roundtrip, double timestamp)
{
    int i = 0;

    while (i < n) {
        cea708_t cea708;
        cea708_init(&cea708, timestamp);
        int chunk = 0;

        for (; i < n && chunk < chunk_size; ++i, ++chunk) {
            cea708_add_cc_data(&cea708, cc[i].valid, (cea708_cc_type_t)cc[i].type, cc[i].data);
        }

        if (roundtrip) {
            uint8_t wire[512];
            cea708_t parsed;
            int size = cea708_render(&cea708, wire, sizeof(wire));
            cea708_init(&parsed, timestamp);
            CHECK(LIBCAPTION_OK == cea708_parse_h264(wire, size, &parsed));
            dtvcc_status_t status = dtvcc_decoder_decode(dec, &parsed);

            if (DTVCC_STATUS_OK != status) {
                return status;
            }
        } else {
            dtvcc_status_t status = dtvcc_decoder_decode(dec, &cea708);

            if (DTVCC_STATUS_OK != status) {
                return status;
            }
        }
    }

    return DTVCC_STATUS_OK;
}

// feeds one packet in a single cea708_t
static dtvcc_status_t feed_packet(dtvcc_decoder_t* dec, const uint8_t* payload, int len, int seq)
{
    test_cc_t cc[64];
    int n = test_packet_cc(payload, len, seq, cc);
    return feed_cc(dec, cc, n, 31, 0, 1.0);
}

////////////////////////////////////////////////////////////////////////////////
// service block / packet payload builders
////////////////////////////////////////////////////////////////////////////////

typedef struct {
    uint8_t data[64];
    int len;
} block_t;

static void b_df(block_t* b, int window, int visible, int rows, int cols)
{
    b->data[b->len++] = 0x98 + window; // DF0-DF7
    b->data[b->len++] = (visible ? 0x20 : 0x00) | 0x10 | 0x08 | 0x04; // visible, row lock, col lock, priority 4
    b->data[b->len++] = 0x14; // anchor vertical 1, anchor horizontal 4
    b->data[b->len++] = (rows - 1) & 0x0F; // anchor point 0
    b->data[b->len++] = (cols - 1) & 0x3F;
    b->data[b->len++] = 0x00; // window style 0, pen style 0
}

static void b_cw(block_t* b, int window) { b->data[b->len++] = 0x80 + window; }
static void b_cmd(block_t* b, uint8_t cmd) { b->data[b->len++] = cmd; }
static void b_mask(block_t* b, uint8_t cmd, int mask) { b->data[b->len++] = cmd; b->data[b->len++] = mask; }
static void b_spl(block_t* b, int row, int col) { b->data[b->len++] = 0x92; b->data[b->len++] = row; b->data[b->len++] = col; }
static void b_text(block_t* b, const char* text) { while (*text) { b->data[b->len++] = (uint8_t)(*text++); } }

typedef struct {
    uint8_t data[128];
    int len;
} payload_t;

static void p_block(payload_t* p, int service, const block_t* b)
{
    CHECK(b->len <= 31);

    if (service < 7) {
        p->data[p->len++] = (service << 5) | b->len;
    } else {
        p->data[p->len++] = (7 << 5) | b->len;
        p->data[p->len++] = service & 0x3F; // extended service number
    }

    memcpy(&p->data[p->len], b->data, b->len);
    p->len += b->len;
}

static void get_window(dtvcc_decoder_t* dec, int service, int window, dtvcc_window_snapshot_t* s)
{
    CHECK(1 == dtvcc_decoder_window_snapshot(dec, service, window, s));
    CHECK(s->defined);
    CHECK(s->service == service);
    CHECK(s->window == window);
}

////////////////////////////////////////////////////////////////////////////////
// cases
////////////////////////////////////////////////////////////////////////////////

// Independent hand assembled packet bytes (not produced by the test encoder):
// one packet, sequence 0, packet size 7 (13 payload bytes).
//   2B                service 1, 11 byte block
//   98 3C 14 01 07 00 DF0: visible, row/col lock, priority 4, anchor (1,4), point 0, 2 rows, 8 cols
//   80                CW0
//   48 49             "HI"
//   89 01             DSW mask 0x01
//   00                padding (null service block)
static void test_hand_written_packet(void)
{
    static const test_cc_t stream[] = {
        { 1, cc_type_dtvcc_packet_start, 0x072B }, // seq 0, size 7, payload[0]
        { 1, cc_type_dtvcc_packet_data, 0x983C },
        { 1, cc_type_dtvcc_packet_data, 0x1401 },
        { 1, cc_type_dtvcc_packet_data, 0x0700 },
        { 1, cc_type_dtvcc_packet_data, 0x8048 },
        { 1, cc_type_dtvcc_packet_data, 0x4989 },
        { 1, cc_type_dtvcc_packet_data, 0x0100 },
    };

    dtvcc_decoder_t* dec = dtvcc_decoder_create();
    CHECK(DTVCC_STATUS_OK == feed_cc(dec, stream, 7, 31, 0, 1.0));

    dtvcc_window_snapshot_t s;
    get_window(dec, 1, 0, &s);
    CHECK(s.visible && s.priority == 4 && s.row_lock && s.col_lock);
    CHECK(s.anchor_vertical == 1 && s.anchor_horizontal == 4 && s.anchor_point == 0);
    CHECK(s.row_count == 2 && s.col_count == 8);
    CHECK(s.pen_row == 0 && s.pen_col == 2);
    CHECK(0 == strcmp(s.text[0], "HI"));
    CHECK(0 == strcmp(s.text[1], ""));

    CHECK(0 == dtvcc_decoder_window_snapshot(dec, 1, 1, &s)); // window 1 undefined
    CHECK(0 == dtvcc_decoder_window_snapshot(dec, 0, 0, &s)); // service 0 is padding only
    dtvcc_decoder_free(dec);
}

static void test_split_across_inputs(void)
{
    block_t b = { { 0 }, 0 };
    b_df(&b, 0, 1, 2, 10);
    b_cw(&b, 0);
    b_text(&b, "SPLIT");
    b_mask(&b, 0x89, 0x01); // DSW

    payload_t p = { { 0 }, 0 };
    p_block(&p, 1, &b);

    test_cc_t cc[64];
    int n = test_packet_cc(p.data, p.len, 0, cc);
    CHECK(n > 4);

    dtvcc_decoder_t* dec = dtvcc_decoder_create();
    // first part of the packet, window state must not appear yet
    CHECK(DTVCC_STATUS_OK == feed_cc(dec, cc, 3, 31, 0, 1.0));
    dtvcc_window_snapshot_t s;
    CHECK(0 == dtvcc_decoder_window_snapshot(dec, 1, 0, &s));
    // the rest completes the packet
    CHECK(DTVCC_STATUS_OK == feed_cc(dec, cc + 3, n - 3, 31, 0, 2.0));
    get_window(dec, 1, 0, &s);
    CHECK(s.row_count == 2 && s.col_count == 10);
    CHECK(0 == strcmp(s.text[0], "SPLIT"));
    dtvcc_decoder_free(dec);
}

static void test_sequence_wrap_3_to_0(void)
{
    dtvcc_decoder_t* dec = dtvcc_decoder_create();

    block_t b1 = { { 0 }, 0 };
    b_df(&b1, 0, 1, 1, 10);
    b_cw(&b1, 0);
    b_text(&b1, "ABC");
    payload_t p1 = { { 0 }, 0 };
    p_block(&p1, 1, &b1);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p1.data, p1.len, 3)); // first packet may have any sequence

    block_t b2 = { { 0 }, 0 };
    b_cw(&b2, 0);
    b_text(&b2, "DEF");
    payload_t p2 = { { 0 }, 0 };
    p_block(&p2, 1, &b2);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p2.data, p2.len, 0)); // 3 wraps to 0, not a loss

    dtvcc_window_snapshot_t s;
    get_window(dec, 1, 0, &s);
    CHECK(0 == strcmp(s.text[0], "ABCDEF")); // state survived the wrap
    dtvcc_decoder_free(dec);
}

static void test_interleaved_services(void)
{
    dtvcc_decoder_t* dec = dtvcc_decoder_create();

    block_t b1 = { { 0 }, 0 };
    b_df(&b1, 0, 1, 1, 16);
    b_cw(&b1, 0);
    b_text(&b1, "ONE");
    block_t b9 = { { 0 }, 0 };
    b_df(&b9, 0, 1, 1, 16);
    b_cw(&b9, 0);
    b_text(&b9, "NINE");
    payload_t p1 = { { 0 }, 0 };
    p_block(&p1, 1, &b1); // normal service
    p_block(&p1, 9, &b9); // extended service in the same packet

    block_t b1b = { { 0 }, 0 };
    b_cw(&b1b, 0);
    b_text(&b1b, "!");
    block_t b9b = { { 0 }, 0 };
    b_cw(&b9b, 0);
    b_text(&b9b, "!");
    payload_t p2 = { { 0 }, 0 };
    p_block(&p2, 9, &b9b); // extended first this time
    p_block(&p2, 1, &b1b);

    // both packets in a single input
    test_cc_t cc[64];
    int n1 = test_packet_cc(p1.data, p1.len, 0, cc);
    int n2 = test_packet_cc(p2.data, p2.len, 1, cc + n1);
    CHECK(DTVCC_STATUS_OK == feed_cc(dec, cc, n1 + n2, 31, 0, 1.0));

    dtvcc_window_snapshot_t s;
    get_window(dec, 1, 0, &s);
    CHECK(0 == strcmp(s.text[0], "ONE!"));
    get_window(dec, 9, 0, &s);
    CHECK(0 == strcmp(s.text[0], "NINE!"));
    CHECK(0 == dtvcc_decoder_window_snapshot(dec, 2, 0, &s)); // untouched service
    dtvcc_decoder_free(dec);
}

static void test_multi_window_mask_ops(void)
{
    dtvcc_decoder_t* dec = dtvcc_decoder_create();

    // a single packet defines two windows, writes to both and operates masks
    block_t b = { { 0 }, 0 };
    b_df(&b, 0, 0, 1, 8); // hidden
    b_df(&b, 1, 0, 1, 8); // hidden
    b_cw(&b, 0);
    b_text(&b, "W0");
    b_cw(&b, 1);
    b_text(&b, "W1");
    b_mask(&b, 0x89, 0x01); // DSW: show window 0
    payload_t p = { { 0 }, 0 };
    p_block(&p, 1, &b);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p.data, p.len, 0));

    dtvcc_window_snapshot_t s;
    get_window(dec, 1, 0, &s);
    CHECK(s.visible && 0 == strcmp(s.text[0], "W0"));
    get_window(dec, 1, 1, &s);
    CHECK(!s.visible && 0 == strcmp(s.text[0], "W1")); // hidden but text present

    block_t b2 = { { 0 }, 0 };
    b_mask(&b2, 0x8A, 0x01); // HDW window 0
    payload_t p2 = { { 0 }, 0 };
    p_block(&p2, 1, &b2);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p2.data, p2.len, 1));
    get_window(dec, 1, 0, &s);
    CHECK(!s.visible && 0 == strcmp(s.text[0], "W0"));

    block_t b3 = { { 0 }, 0 };
    b_mask(&b3, 0x8B, 0x03); // TGW both
    payload_t p3 = { { 0 }, 0 };
    p_block(&p3, 1, &b3);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p3.data, p3.len, 2));
    get_window(dec, 1, 0, &s);
    CHECK(s.visible);
    get_window(dec, 1, 1, &s);
    CHECK(s.visible);
    dtvcc_decoder_free(dec);
}

static void test_hide_reshow(void)
{
    dtvcc_decoder_t* dec = dtvcc_decoder_create();

    block_t b1 = { { 0 }, 0 };
    b_df(&b1, 0, 1, 1, 8);
    b_cw(&b1, 0);
    b_text(&b1, "AB");
    payload_t p1 = { { 0 }, 0 };
    p_block(&p1, 1, &b1);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p1.data, p1.len, 0));

    block_t b2 = { { 0 }, 0 };
    b_mask(&b2, 0x8A, 0x01); // HDW
    b_cw(&b2, 0);
    b_text(&b2, "CD"); // written while hidden
    payload_t p2 = { { 0 }, 0 };
    p_block(&p2, 1, &b2);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p2.data, p2.len, 1));

    dtvcc_window_snapshot_t s;
    get_window(dec, 1, 0, &s);
    CHECK(!s.visible);
    CHECK(0 == strcmp(s.text[0], "ABCD")); // hidden text is there

    block_t b3 = { { 0 }, 0 };
    b_mask(&b3, 0x89, 0x01); // DSW
    payload_t p3 = { { 0 }, 0 };
    p_block(&p3, 1, &b3);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p3.data, p3.len, 2));
    get_window(dec, 1, 0, &s);
    CHECK(s.visible);
    CHECK(0 == strcmp(s.text[0], "ABCD")); // re-shown with the hidden writes
    dtvcc_decoder_free(dec);
}

static void test_window_resize(void)
{
    dtvcc_decoder_t* dec = dtvcc_decoder_create();

    block_t b1 = { { 0 }, 0 };
    b_df(&b1, 0, 1, 2, 5);
    b_cw(&b1, 0);
    b_text(&b1, "HELLO");
    b_cmd(&b1, 0x0D); // CR
    b_text(&b1, "WORLD");
    payload_t p1 = { { 0 }, 0 };
    p_block(&p1, 1, &b1);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p1.data, p1.len, 0));

    dtvcc_window_snapshot_t s;
    get_window(dec, 1, 0, &s);
    CHECK(0 == strcmp(s.text[0], "HELLO"));
    CHECK(0 == strcmp(s.text[1], "WORLD"));

    // shrink to 1 row: row 0 kept, row 1 clipped
    block_t b2 = { { 0 }, 0 };
    b_df(&b2, 0, 1, 1, 5);
    payload_t p2 = { { 0 }, 0 };
    p_block(&p2, 1, &b2);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p2.data, p2.len, 1));
    get_window(dec, 1, 0, &s);
    CHECK(s.row_count == 1 && s.col_count == 5);
    CHECK(0 == strcmp(s.text[0], "HELLO"));

    // same size redefinition keeps the text
    block_t b3 = { { 0 }, 0 };
    b_df(&b3, 0, 1, 1, 5);
    payload_t p3 = { { 0 }, 0 };
    p_block(&p3, 1, &b3);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p3.data, p3.len, 2));
    get_window(dec, 1, 0, &s);
    CHECK(0 == strcmp(s.text[0], "HELLO"));

    // grow to 3x8: text kept
    block_t b4 = { { 0 }, 0 };
    b_df(&b4, 0, 1, 3, 8);
    payload_t p4 = { { 0 }, 0 };
    p_block(&p4, 1, &b4);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p4.data, p4.len, 3));
    get_window(dec, 1, 0, &s);
    CHECK(s.row_count == 3 && s.col_count == 8);
    CHECK(0 == strcmp(s.text[0], "HELLO"));
    CHECK(0 == strcmp(s.text[1], ""));
    dtvcc_decoder_free(dec);
}

static void test_cr_scroll_and_pen_controls(void)
{
    dtvcc_decoder_t* dec = dtvcc_decoder_create();

    block_t b1 = { { 0 }, 0 };
    b_df(&b1, 0, 1, 2, 4);
    b_cw(&b1, 0);
    b_text(&b1, "AB");
    b_cmd(&b1, 0x0D); // CR: next row
    b_text(&b1, "CD");
    b_cmd(&b1, 0x0D); // CR at the bottom: scroll this window
    b_text(&b1, "EF");
    payload_t p1 = { { 0 }, 0 };
    p_block(&p1, 1, &b1);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p1.data, p1.len, 0));

    dtvcc_window_snapshot_t s;
    get_window(dec, 1, 0, &s);
    CHECK(0 == strcmp(s.text[0], "CD")); // scrolled up
    CHECK(0 == strcmp(s.text[1], "EF"));
    CHECK(s.pen_row == 1 && s.pen_col == 2);

    // SPL sets the pen, BS steps back
    block_t b2 = { { 0 }, 0 };
    b_cw(&b2, 0);
    b_spl(&b2, 0, 3);
    b_text(&b2, "G");
    b_spl(&b2, 1, 1);
    b_cmd(&b2, 0x08); // BS
    b_text(&b2, "H");
    payload_t p2 = { { 0 }, 0 };
    p_block(&p2, 1, &b2);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p2.data, p2.len, 1));
    get_window(dec, 1, 0, &s);
    CHECK(0 == strcmp(s.text[0], "CD G"));
    CHECK(0 == strcmp(s.text[1], "HF"));

    // FF clears only the current window
    block_t b3 = { { 0 }, 0 };
    b_df(&b3, 1, 1, 1, 4);
    b_cw(&b3, 1);
    b_text(&b3, "W1");
    b_cw(&b3, 0);
    b_cmd(&b3, 0x0C); // FF
    payload_t p3 = { { 0 }, 0 };
    p_block(&p3, 1, &b3);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p3.data, p3.len, 2));
    get_window(dec, 1, 0, &s);
    CHECK(s.defined && s.visible);
    CHECK(0 == strcmp(s.text[0], "") && 0 == strcmp(s.text[1], ""));
    CHECK(s.pen_row == 0 && s.pen_col == 0);
    get_window(dec, 1, 1, &s);
    CHECK(0 == strcmp(s.text[0], "W1")); // untouched

    // writes past the locked bounds are clipped, never reach the adjacent window
    block_t b4 = { { 0 }, 0 };
    b_cw(&b4, 0);
    b_text(&b4, "WXYZQ"); // 5 characters into 4 columns
    payload_t p4 = { { 0 }, 0 };
    p_block(&p4, 1, &b4);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p4.data, p4.len, 3));
    get_window(dec, 1, 0, &s);
    CHECK(0 == strcmp(s.text[0], "WXYZ"));
    get_window(dec, 1, 1, &s);
    CHECK(0 == strcmp(s.text[0], "W1"));
    dtvcc_decoder_free(dec);
}

static void test_clear_delete_reset(void)
{
    dtvcc_decoder_t* dec = dtvcc_decoder_create();

    block_t b1 = { { 0 }, 0 };
    b_df(&b1, 0, 1, 1, 6);
    b_cw(&b1, 0);
    b_text(&b1, "KEEP");
    payload_t p1 = { { 0 }, 0 };
    p_block(&p1, 1, &b1);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p1.data, p1.len, 0));

    // CLW clears the text but keeps definition and visibility
    block_t b2 = { { 0 }, 0 };
    b_mask(&b2, 0x88, 0x01); // CLW
    payload_t p2 = { { 0 }, 0 };
    p_block(&p2, 1, &b2);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p2.data, p2.len, 1));
    dtvcc_window_snapshot_t s;
    get_window(dec, 1, 0, &s);
    CHECK(s.defined && s.visible);
    CHECK(0 == strcmp(s.text[0], ""));

    block_t b3 = { { 0 }, 0 };
    b_cw(&b3, 0);
    b_text(&b3, "NEW");
    payload_t p3 = { { 0 }, 0 };
    p_block(&p3, 1, &b3);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p3.data, p3.len, 2));
    get_window(dec, 1, 0, &s);
    CHECK(0 == strcmp(s.text[0], "NEW"));

    // DLW revokes the definition
    block_t b4 = { { 0 }, 0 };
    b_mask(&b4, 0x8C, 0x01); // DLW
    payload_t p4 = { { 0 }, 0 };
    p_block(&p4, 1, &b4);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p4.data, p4.len, 3));
    CHECK(0 == dtvcc_decoder_window_snapshot(dec, 1, 0, &s));

    // RST deletes all windows of the service
    block_t b5 = { { 0 }, 0 };
    b_df(&b5, 0, 1, 1, 6);
    b_cw(&b5, 0);
    b_text(&b5, "ZZ");
    b_df(&b5, 1, 1, 1, 6);
    b_cw(&b5, 1);
    b_text(&b5, "YY");
    payload_t p5 = { { 0 }, 0 };
    p_block(&p5, 1, &b5);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p5.data, p5.len, 0)); // sequence wrapped

    block_t b6 = { { 0 }, 0 };
    b_cmd(&b6, 0x8F); // RST
    payload_t p6 = { { 0 }, 0 };
    p_block(&p6, 1, &b6);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p6.data, p6.len, 1));
    CHECK(0 == dtvcc_decoder_window_snapshot(dec, 1, 0, &s));
    CHECK(0 == dtvcc_decoder_window_snapshot(dec, 1, 1, &s));
    dtvcc_decoder_free(dec);
}

static void test_bad_block_and_truncated_command(void)
{
    dtvcc_decoder_t* dec = dtvcc_decoder_create();

    block_t b1 = { { 0 }, 0 };
    b_df(&b1, 0, 1, 1, 8);
    b_cw(&b1, 0);
    b_text(&b1, "SAFE");
    payload_t p1 = { { 0 }, 0 };
    p_block(&p1, 1, &b1);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p1.data, p1.len, 0));

    // block size reaches past the end of the packet
    uint8_t bad_block[] = { 0x26, 0x98, 0x3C, 0x14, 0x01 }; // claims 6 block bytes, 4 present
    CHECK(DTVCC_STATUS_ERROR == feed_packet(dec, bad_block, sizeof(bad_block), 1));

    // command parameters truncated at the block boundary
    uint8_t truncated[] = { 0x21, 0x88 }; // CLW without its mask byte
    CHECK(DTVCC_STATUS_ERROR == feed_packet(dec, truncated, sizeof(truncated), 2));

    // DF with too few parameter bytes
    uint8_t short_df[] = { 0x22, 0x98, 0x3C };
    CHECK(DTVCC_STATUS_ERROR == feed_packet(dec, short_df, sizeof(short_df), 3));

    // extended service numbers below 7 are invalid
    uint8_t bad_extended[] = { 0xE2, 0x05, 0x80 };
    CHECK(DTVCC_STATUS_ERROR == feed_packet(dec, bad_extended, sizeof(bad_extended), 0));

    // none of the bad packets touched the state, the decoder is still usable
    dtvcc_window_snapshot_t s;
    get_window(dec, 1, 0, &s);
    CHECK(0 == strcmp(s.text[0], "SAFE"));

    block_t b2 = { { 0 }, 0 };
    b_cw(&b2, 0);
    b_text(&b2, "!");
    payload_t p2 = { { 0 }, 0 };
    p_block(&p2, 1, &b2);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p2.data, p2.len, 1));
    get_window(dec, 1, 0, &s);
    CHECK(0 == strcmp(s.text[0], "SAFE!"));
    dtvcc_decoder_free(dec);
}

static void test_unsupported_command_rollback(void)
{
    dtvcc_decoder_t* dec = dtvcc_decoder_create();

    block_t b1 = { { 0 }, 0 };
    b_df(&b1, 0, 1, 1, 8);
    b_cw(&b1, 0);
    b_text(&b1, "KEEP");
    payload_t p1 = { { 0 }, 0 };
    p_block(&p1, 1, &b1);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p1.data, p1.len, 0));

    // SPA is not supported; the whole packet is rejected, including the
    // window and text it carried before the unsupported command
    block_t b2 = { { 0 }, 0 };
    b_df(&b2, 1, 1, 1, 8);
    b_cw(&b2, 1);
    b_text(&b2, "NO");
    b_cmd(&b2, 0x90); // SPA
    b_cmd(&b2, 0x00);
    b_cmd(&b2, 0x00);
    payload_t p2 = { { 0 }, 0 };
    p_block(&p2, 1, &b2);
    CHECK(DTVCC_STATUS_UNSUPPORTED == feed_packet(dec, p2.data, p2.len, 1));

    dtvcc_window_snapshot_t s;
    CHECK(0 == dtvcc_decoder_window_snapshot(dec, 1, 1, &s)); // rolled back
    get_window(dec, 1, 0, &s);
    CHECK(0 == strcmp(s.text[0], "KEEP")); // earlier packet untouched

    // G1 characters are not supported either
    block_t b3 = { { 0 }, 0 };
    b_cw(&b3, 0);
    b_cmd(&b3, 0xA0);
    payload_t p3 = { { 0 }, 0 };
    p_block(&p3, 1, &b3);
    CHECK(DTVCC_STATUS_UNSUPPORTED == feed_packet(dec, p3.data, p3.len, 2));

    // decoder still works
    block_t b4 = { { 0 }, 0 };
    b_cw(&b4, 0);
    b_text(&b4, "!");
    payload_t p4 = { { 0 }, 0 };
    p_block(&p4, 1, &b4);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p4.data, p4.len, 3));
    get_window(dec, 1, 0, &s);
    CHECK(0 == strcmp(s.text[0], "KEEP!"));
    dtvcc_decoder_free(dec);
}

static void test_sequence_gap_clears_windows(void)
{
    // sequence number jump
    {
        dtvcc_decoder_t* dec = dtvcc_decoder_create();
        block_t b1 = { { 0 }, 0 };
        b_df(&b1, 0, 1, 1, 8);
        b_cw(&b1, 0);
        b_text(&b1, "OLD");
        payload_t p1 = { { 0 }, 0 };
        p_block(&p1, 1, &b1);
        CHECK(DTVCC_STATUS_OK == feed_packet(dec, p1.data, p1.len, 0));

        block_t b2 = { { 0 }, 0 };
        b_df(&b2, 1, 1, 1, 8);
        b_cw(&b2, 1);
        b_text(&b2, "NEW");
        payload_t p2 = { { 0 }, 0 };
        p_block(&p2, 1, &b2);
        CHECK(DTVCC_STATUS_OK == feed_packet(dec, p2.data, p2.len, 2)); // 1 was lost

        dtvcc_window_snapshot_t s;
        CHECK(0 == dtvcc_decoder_window_snapshot(dec, 1, 0, &s)); // cleared by the loss
        get_window(dec, 1, 1, &s);
        CHECK(0 == strcmp(s.text[0], "NEW")); // the new complete packet establishes state
        dtvcc_decoder_free(dec);
    }

    // a start that interrupts an unfinished packet
    {
        dtvcc_decoder_t* dec = dtvcc_decoder_create();
        block_t b1 = { { 0 }, 0 };
        b_df(&b1, 0, 1, 1, 8);
        b_cw(&b1, 0);
        b_text(&b1, "STALE");
        payload_t p1 = { { 0 }, 0 };
        p_block(&p1, 1, &b1);
        CHECK(DTVCC_STATUS_OK == feed_packet(dec, p1.data, p1.len, 0));

        // unfinished packet: only its first entries arrive
        block_t b2 = { { 0 }, 0 };
        b_df(&b2, 0, 1, 1, 8);
        b_cw(&b2, 0);
        b_text(&b2, "PARTIAL");
        payload_t p2 = { { 0 }, 0 };
        p_block(&p2, 1, &b2);
        test_cc_t cc[64];
        int n = test_packet_cc(p2.data, p2.len, 1, cc);
        CHECK(n > 2);
        CHECK(DTVCC_STATUS_OK == feed_cc(dec, cc, 2, 31, 0, 1.0)); // start + 1 continuation

        // a new start interrupts the unfinished packet: loss, stale windows cleared
        block_t b3 = { { 0 }, 0 };
        b_df(&b3, 1, 1, 1, 5);
        b_cw(&b3, 1);
        b_text(&b3, "FRESH");
        payload_t p3 = { { 0 }, 0 };
        p_block(&p3, 1, &b3);
        CHECK(DTVCC_STATUS_OK == feed_packet(dec, p3.data, p3.len, 2));

        dtvcc_window_snapshot_t s;
        CHECK(0 == dtvcc_decoder_window_snapshot(dec, 1, 0, &s)); // STALE cleared
        get_window(dec, 1, 1, &s);
        CHECK(0 == strcmp(s.text[0], "FRESH"));

        // leftover continuations of the interrupted packet are ignored
        CHECK(DTVCC_STATUS_OK == feed_cc(dec, cc + 2, n - 2, 31, 0, 1.0));
        get_window(dec, 1, 1, &s);
        CHECK(0 == strcmp(s.text[0], "FRESH"));
        dtvcc_decoder_free(dec);
    }
}

static void test_h264_roundtrip(void)
{
    dtvcc_decoder_t* dec = dtvcc_decoder_create();

    block_t b = { { 0 }, 0 };
    b_df(&b, 0, 1, 2, 10);
    b_cw(&b, 0);
    b_text(&b, "ROUND");
    b_cmd(&b, 0x0D);
    b_text(&b, "TRIP");
    payload_t p = { { 0 }, 0 };
    p_block(&p, 1, &b);

    // split across inputs, each taking the H.264 render/parse round trip
    test_cc_t cc[64];
    int n = test_packet_cc(p.data, p.len, 0, cc);
    CHECK(n > 4);
    CHECK(DTVCC_STATUS_OK == feed_cc(dec, cc, n, 4, 1, 9.5));

    dtvcc_window_snapshot_t s;
    get_window(dec, 1, 0, &s);
    CHECK(s.row_count == 2 && s.col_count == 10);
    CHECK(0 == strcmp(s.text[0], "ROUND"));
    CHECK(0 == strcmp(s.text[1], "TRIP"));
    CHECK(s.timestamp == 9.5);
    dtvcc_decoder_free(dec);
}

static void test_ignores_non_dtvcc(void)
{
    dtvcc_decoder_t* dec = dtvcc_decoder_create();
    dtvcc_window_snapshot_t s;
    cea708_t c;

    // 608 data is left to the old interface
    cea708_init(&c, 1.0);
    cea708_add_cc_data(&c, 1, cc_type_ntsc_cc_field_1, 0x4142);
    CHECK(DTVCC_STATUS_OK == dtvcc_decoder_decode(dec, &c));
    CHECK(0 == dtvcc_decoder_window_snapshot(dec, 1, 0, &s));

    // process_cc_data_flag clear: nothing is consumed
    cea708_init(&c, 1.0);
    cea708_add_cc_data(&c, 1, cc_type_dtvcc_packet_start, 0x072B);
    c.user_data.process_cc_data_flag = 0;
    CHECK(DTVCC_STATUS_OK == dtvcc_decoder_decode(dec, &c));
    CHECK(0 == dtvcc_decoder_window_snapshot(dec, 1, 0, &s));

    // non GA94 user data: nothing is consumed
    cea708_init(&c, 1.0);
    cea708_add_cc_data(&c, 1, cc_type_dtvcc_packet_start, 0x072B);
    c.user_identifier = DTG1;
    CHECK(DTVCC_STATUS_OK == dtvcc_decoder_decode(dec, &c));
    CHECK(0 == dtvcc_decoder_window_snapshot(dec, 1, 0, &s));

    // invalid entries are skipped
    cea708_init(&c, 1.0);
    cea708_add_cc_data(&c, 0, cc_type_dtvcc_packet_start, 0x072B);
    CHECK(DTVCC_STATUS_OK == dtvcc_decoder_decode(dec, &c));
    CHECK(0 == dtvcc_decoder_window_snapshot(dec, 1, 0, &s));

    // the decoder is still fully functional
    block_t b = { { 0 }, 0 };
    b_df(&b, 0, 1, 1, 4);
    b_cw(&b, 0);
    b_text(&b, "OK");
    payload_t p = { { 0 }, 0 };
    p_block(&p, 1, &b);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p.data, p.len, 0));
    get_window(dec, 1, 0, &s);
    CHECK(0 == strcmp(s.text[0], "OK"));
    dtvcc_decoder_free(dec);
}

static void test_snapshot_independence(void)
{
    dtvcc_decoder_t* dec = dtvcc_decoder_create();

    block_t b1 = { { 0 }, 0 };
    b_df(&b1, 0, 1, 1, 8);
    b_cw(&b1, 0);
    b_text(&b1, "FIRST");
    payload_t p1 = { { 0 }, 0 };
    p_block(&p1, 1, &b1);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p1.data, p1.len, 0));

    dtvcc_window_snapshot_t s1;
    get_window(dec, 1, 0, &s1);
    CHECK(0 == strcmp(s1.text[0], "FIRST"));

    block_t b2 = { { 0 }, 0 };
    b_cw(&b2, 0);
    b_spl(&b2, 0, 0);
    b_text(&b2, "SECOND");
    payload_t p2 = { { 0 }, 0 };
    p_block(&p2, 1, &b2);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p2.data, p2.len, 1));

    // the snapshot taken before the new input is unchanged
    CHECK(0 == strcmp(s1.text[0], "FIRST"));
    CHECK(s1.pen_col == 5);

    dtvcc_window_snapshot_t s2;
    get_window(dec, 1, 0, &s2);
    CHECK(0 == strcmp(s2.text[0], "SECOND"));
    dtvcc_decoder_free(dec);
}

static void test_reset_discards_partial(void)
{
    dtvcc_decoder_t* dec = dtvcc_decoder_create();

    block_t b1 = { { 0 }, 0 };
    b_df(&b1, 0, 1, 1, 8);
    b_cw(&b1, 0);
    b_text(&b1, "RESET");
    payload_t p1 = { { 0 }, 0 };
    p_block(&p1, 1, &b1);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p1.data, p1.len, 0));

    // an unfinished packet
    block_t b2 = { { 0 }, 0 };
    b_df(&b2, 1, 1, 1, 8);
    b_cw(&b2, 1);
    b_text(&b2, "PARTIAL");
    payload_t p2 = { { 0 }, 0 };
    p_block(&p2, 1, &b2);
    test_cc_t cc[64];
    int n = test_packet_cc(p2.data, p2.len, 1, cc);
    CHECK(DTVCC_STATUS_OK == feed_cc(dec, cc, 2, 31, 0, 1.0));

    dtvcc_decoder_reset(dec);

    // leftover bytes of the unfinished packet cannot be assembled
    CHECK(DTVCC_STATUS_OK == feed_cc(dec, cc + 2, n - 2, 31, 0, 1.0));
    dtvcc_window_snapshot_t s;
    CHECK(0 == dtvcc_decoder_window_snapshot(dec, 1, 0, &s)); // cleared by reset
    CHECK(0 == dtvcc_decoder_window_snapshot(dec, 1, 1, &s));

    // any sequence is accepted after a reset
    block_t b3 = { { 0 }, 0 };
    b_df(&b3, 0, 1, 1, 4);
    b_cw(&b3, 0);
    b_text(&b3, "NEW");
    payload_t p3 = { { 0 }, 0 };
    p_block(&p3, 1, &b3);
    CHECK(DTVCC_STATUS_OK == feed_packet(dec, p3.data, p3.len, 2));
    get_window(dec, 1, 0, &s);
    CHECK(0 == strcmp(s.text[0], "NEW"));
    dtvcc_decoder_free(dec);
}

int main(void)
{
    RUN_CASE(test_hand_written_packet);
    RUN_CASE(test_split_across_inputs);
    RUN_CASE(test_sequence_wrap_3_to_0);
    RUN_CASE(test_interleaved_services);
    RUN_CASE(test_multi_window_mask_ops);
    RUN_CASE(test_hide_reshow);
    RUN_CASE(test_window_resize);
    RUN_CASE(test_cr_scroll_and_pen_controls);
    RUN_CASE(test_clear_delete_reset);
    RUN_CASE(test_bad_block_and_truncated_command);
    RUN_CASE(test_unsupported_command_rollback);
    RUN_CASE(test_sequence_gap_clears_windows);
    RUN_CASE(test_h264_roundtrip);
    RUN_CASE(test_ignores_non_dtvcc);
    RUN_CASE(test_snapshot_independence);
    RUN_CASE(test_reset_discards_partial);
    printf("DTVCC_CASES_RUN %d\n", g_cases);
    return 0;
}
