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
#include "task_support/check.h"
#include "caption/caption.h"
#include "caption/diff608.h"
#include "caption/eia608.h"
#include "caption/playout.h"
#include <stdlib.h>
#include <string.h>

#define TEST_MAX_RECORDS 256
#define PAD_WORD 0x8080

static uint16_t ctrl(eia608_control_t cmd) { return eia608_control_command(cmd, 0); }
static uint16_t pac0(int row) { return eia608_row_column_pramble(row, 0, 0, 0); }
static uint16_t chars2(char a, char b) { return eia608_parity((uint16_t)(((unsigned char)a << 8) | (unsigned char)b)); }

static int feed(caption_frame_t* rx, uint16_t word, uint64_t frame)
{
    TASK_CHECK(LIBCAPTION_ERROR != caption_frame_decode(rx, word, (double)frame / 30.0));
    TASK_CHECK(eia608_parity_varify(word));
    return 0;
}

// Initializes a frame whose writable target is front and places text on row.
static int frame_row(caption_frame_t* f, int row, const char* text)
{
    size_t i;
    caption_frame_init(f);
    f->write = &f->front;

    for (i = 0; text[i]; ++i) {
        char ch[2] = { text[i], 0 };
        TASK_CHECK(caption_frame_write_char(f, row, (int)i, eia608_style_white, 0, ch));
    }

    return 0;
}

// Adds another column-0 row to an already initialized snapshot.
static int frame_add_row(caption_frame_t* f, int row, const char* text)
{
    size_t i;

    for (i = 0; text[i]; ++i) {
        char ch[2] = { text[i], 0 };
        TASK_CHECK(caption_frame_write_char(f, row, (int)i, eia608_style_white, 0, ch));
    }

    return 0;
}

static int front_char(const caption_frame_t* f, int row, int col) { return f->front.cell[row][col].data[0]; }
static int back_char(const caption_frame_t* f, int row, int col) { return f->back.cell[row][col].data[0]; }

static int front_row_equals(const caption_frame_t* f, int row, const char* text)
{
    int col;

    for (col = 0; col < SCREEN_COLS; ++col) {
        int want = ((size_t)col < strlen(text)) ? text[col] : 0;
        TASK_CHECK(front_char(f, row, col) == want);
    }

    return 0;
}

static int screen_empty(const caption_frame_t* f)
{
    int row, col;

    for (row = 0; row < SCREEN_ROWS; ++row) {
        for (col = 0; col < SCREEN_COLS; ++col) {
            TASK_CHECK(0 == f->front.cell[row][col].data[0]);
        }
    }

    return 0;
}

static int records_same(const cc_playout_record_t* a, const cc_playout_record_t* b, size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        TASK_CHECK(a[i].frame == b[i].frame);
        TASK_CHECK(a[i].cc_data == b[i].cc_data);
    }

    return 0;
}

// Decodes a full record stream and returns the receiver state.
static int decode_stream(const cc_playout_record_t* rec, size_t count, caption_frame_t* rx)
{
    size_t i;
    caption_frame_init(rx);

    for (i = 0; i < count; ++i) {
        TASK_CHECK(rec[i].frame == i);
        TASK_CHECK(0 == feed(rx, rec[i].cc_data, i));
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// screens group
////////////////////////////////////////////////////////////////////////////////

// Hand-written control sequence: EOC really swaps the memories once and the
// mandatory duplicate EOC does not swap again.
static int case_handwritten_eoc(void)
{
    caption_frame_t f;
    caption_frame_init(&f);

    TASK_CHECK(0 == feed(&f, ctrl(eia608_control_resume_caption_loading), 0));
    TASK_CHECK(0 == feed(&f, pac0(SCREEN_ROWS - 1), 1));
    TASK_CHECK(0 == feed(&f, chars2('A', 'B'), 2));
    TASK_CHECK(0 == screen_empty(&f));                 // still hidden
    TASK_CHECK('A' == back_char(&f, SCREEN_ROWS - 1, 0));
    TASK_CHECK('B' == back_char(&f, SCREEN_ROWS - 1, 1));

    TASK_CHECK(0 == feed(&f, ctrl(eia608_control_end_of_caption), 3));
    TASK_CHECK(0 == front_row_equals(&f, SCREEN_ROWS - 1, "AB"));
    TASK_CHECK(0 == back_char(&f, SCREEN_ROWS - 1, 0)); // old front was empty

    TASK_CHECK(0 == feed(&f, ctrl(eia608_control_end_of_caption), 4));
    TASK_CHECK(0 == front_row_equals(&f, SCREEN_ROWS - 1, "AB")); // no second swap
    TASK_CHECK(0 == back_char(&f, SCREEN_ROWS - 1, 0));
    return 0;
}

// DER must really clear the rest of the current row on the write screen.
static int case_der_clears_hidden(void)
{
    caption_frame_t f;
    caption_frame_init(&f);

    TASK_CHECK(0 == feed(&f, ctrl(eia608_control_resume_caption_loading), 0));
    TASK_CHECK(0 == feed(&f, pac0(SCREEN_ROWS - 1), 1));
    TASK_CHECK(0 == feed(&f, chars2('A', 'B'), 2));
    TASK_CHECK(0 == feed(&f, chars2('C', 'D'), 3));
    TASK_CHECK(0 == feed(&f, pac0(SCREEN_ROWS - 1), 4));
    TASK_CHECK(0 == feed(&f, ctrl(eia608_control_delete_to_end_of_row), 5));

    for (int col = 0; col < SCREEN_COLS; ++col) {
        TASK_CHECK(0 == f.back.cell[SCREEN_ROWS - 1][col].data[0]);
        TASK_CHECK(0 == f.back.cell[SCREEN_ROWS - 1][col].sty);
        TASK_CHECK(0 == f.back.cell[SCREEN_ROWS - 1][col].uln);
    }

    return 0;
}

// Exact byte stream for SET first 10 AB, plus per-frame decoded visibility.
static int case_basic_ab_bytes(void)
{
    cc_playout_t* p = cc_playout_create(0);
    cc_playout_record_t rec[TEST_MAX_RECORDS];
    caption_frame_t snap, rx;
    const uint16_t expect[14] = {
        ctrl(eia608_control_resume_caption_loading),
        ctrl(eia608_control_resume_caption_loading),
        pac0(SCREEN_ROWS - 1),
        pac0(SCREEN_ROWS - 1),
        ctrl(eia608_control_delete_to_end_of_row),
        ctrl(eia608_control_delete_to_end_of_row),
        chars2('A', 'B'),
        PAD_WORD, PAD_WORD, PAD_WORD,
        ctrl(eia608_control_end_of_caption),
        ctrl(eia608_control_end_of_caption),
        PAD_WORD, PAD_WORD
    };

    TASK_CHECK(0 != p);
    TASK_CHECK(0 == frame_row(&snap, SCREEN_ROWS - 1, "AB"));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "first", 10, &snap));
    TASK_CHECK(14 == cc_playout_pull(p, 14, rec));
    caption_frame_init(&rx);

    for (int i = 0; i < 14; ++i) {
        TASK_CHECK(rec[i].frame == (uint64_t)i);
        TASK_CHECK(rec[i].cc_data == expect[i]);
        TASK_CHECK(0 == feed(&rx, rec[i].cc_data, (uint64_t)i));

        if (i < 10) {
            TASK_CHECK(0 == screen_empty(&rx));
        } else {
            TASK_CHECK(0 == front_row_equals(&rx, SCREEN_ROWS - 1, "AB"));
        }
    }

    cc_playout_free(p);
    return 0;
}

// A, then B, then A again: the third event only needs RCL pair + EOC pair.
static int case_aba_hidden_reuse(void)
{
    cc_playout_t* p = cc_playout_create(0);
    cc_playout_record_t rec[TEST_MAX_RECORDS];
    caption_frame_t a, b, rx;
    size_t i, extra = 0;

    TASK_CHECK(0 == frame_row(&a, SCREEN_ROWS - 1, "A"));
    TASK_CHECK(0 == frame_row(&b, SCREEN_ROWS - 1, "B"));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "a", 20, &a));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "b", 40, &b));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "a2", 60, &a));
    TASK_CHECK(62 == cc_playout_pull(p, 62, rec));
    TASK_CHECK(rec[20].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(rec[21].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(rec[40].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(rec[41].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(rec[60].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(rec[61].cc_data == ctrl(eia608_control_end_of_caption));

    // Only four non-padding words after B's second EOC: RCL, RCL, EOC, EOC.
    for (i = 42; i < 62; ++i) {
        if (PAD_WORD != rec[i].cc_data) {
            TASK_CHECK(extra < 4);
            if (0 == extra) {
                TASK_CHECK(rec[i].cc_data == ctrl(eia608_control_resume_caption_loading));
            } else if (1 == extra) {
                TASK_CHECK(rec[i].cc_data == ctrl(eia608_control_resume_caption_loading));
            } else {
                TASK_CHECK(rec[i].cc_data == ctrl(eia608_control_end_of_caption));
            }

            ++extra;
        }
    }

    TASK_CHECK(4 == extra);
    TASK_CHECK(0 == decode_stream(rec, 62, &rx));
    TASK_CHECK(0 == front_row_equals(&rx, SCREEN_ROWS - 1, "A"));
    cc_playout_free(p);
    return 0;
}

// Full 32-cell row and an odd-length row: 16 pairs each, null low byte last.
static int case_32cols_and_odd(void)
{
    static const char long_text[SCREEN_COLS + 1] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    cc_playout_t* p = cc_playout_create(0);
    cc_playout_record_t rec[TEST_MAX_RECORDS];
    caption_frame_t snap, rx;

    TASK_CHECK(32 == strlen(long_text));
    TASK_CHECK(0 == frame_row(&snap, SCREEN_ROWS - 1, long_text));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "full", 23, &snap)); // 2 RCL + 2 PAC + 2 DER + 16 pairs + 2 EOC
    TASK_CHECK(25 == cc_playout_pull(p, 25, rec));
    TASK_CHECK(rec[23].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(0 == decode_stream(rec, 25, &rx));
    TASK_CHECK(0 == front_row_equals(&rx, SCREEN_ROWS - 1, long_text));
    cc_playout_free(p);

    p = cc_playout_create(0);
    TASK_CHECK(0 == frame_row(&snap, SCREEN_ROWS - 1, "ABC"));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "odd", 20, &snap));
    TASK_CHECK(22 == cc_playout_pull(p, 22, rec));

    // indices 6 and 7 hold "AB" then "C" paired with a null low byte
    TASK_CHECK(rec[6].cc_data == chars2('A', 'B'));
    TASK_CHECK(rec[7].cc_data == eia608_parity((uint16_t)('C' << 8)));
    TASK_CHECK(0 == decode_stream(rec, 22, &rx));
    TASK_CHECK(0 == front_row_equals(&rx, SCREEN_ROWS - 1, "ABC"));
    cc_playout_free(p);
    return 0;
}

// Removing a whole row emits PAC + DER pairs only, never a screen erase.
// Three events are needed: after the middle event the first event's two-row
// screen is left in hidden memory, so the last event only deletes row 10 and
// reuses the unchanged row 14.
static int case_delete_whole_row(void)
{
    cc_playout_t* p = cc_playout_create(0);
    cc_playout_record_t rec[TEST_MAX_RECORDS];
    caption_frame_t two_rows, one_row, rx;

    TASK_CHECK(0 == frame_row(&two_rows, 10, "HELLO"));
    TASK_CHECK(0 == frame_add_row(&two_rows, SCREEN_ROWS - 1, "WORLD"));
    TASK_CHECK(0 == frame_row(&one_row, SCREEN_ROWS - 1, "WORLD"));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "two", 30, &two_rows));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "mid", 70, &one_row));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "last", 110, &one_row));
    TASK_CHECK(112 == cc_playout_pull(p, 112, rec));
    TASK_CHECK(0 == decode_stream(rec, 112, &rx));
    TASK_CHECK(0 == front_row_equals(&rx, 10, ""));
    TASK_CHECK(0 == front_row_equals(&rx, SCREEN_ROWS - 1, "WORLD"));
    TASK_CHECK(rec[110].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(rec[111].cc_data == ctrl(eia608_control_end_of_caption));

    // the last event's body may contain only PAC/DER (row 14 is reused)
    int seen_rcl = 0, body = 0;

    for (int i = 72; i < 110; ++i) {
        if (rec[i].cc_data == ctrl(eia608_control_resume_caption_loading)) {
            TASK_CHECK(!body);
            ++seen_rcl;
        } else if (PAD_WORD != rec[i].cc_data) {
            body = 1;
            TASK_CHECK(rec[i].cc_data == pac0(10) ||
                       rec[i].cc_data == ctrl(eia608_control_delete_to_end_of_row));
        }
    }

    TASK_CHECK(2 == seen_rcl);
    TASK_CHECK(body);
    cc_playout_free(p);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// deadlines group
////////////////////////////////////////////////////////////////////////////////

// Exact capacity succeeds; one frame fewer fails and leaves the queue empty.
static int case_exact_capacity_and_short(void)
{
    static const char long_text[SCREEN_COLS + 1] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    cc_playout_t* p = cc_playout_create(0);
    cc_playout_record_t rec[TEST_MAX_RECORDS];
    caption_frame_t snap;

    TASK_CHECK(0 == frame_row(&snap, SCREEN_ROWS - 1, long_text));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "full", 22, &snap)); // 24 words: preload fills 0..21, EOC1 exactly at 22
    TASK_CHECK(24 == cc_playout_pull(p, 24, rec));
    TASK_CHECK(rec[22].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(rec[23].cc_data == ctrl(eia608_control_end_of_caption));
    cc_playout_free(p);

    p = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_ERROR_DEADLINE == cc_playout_set(p, "full", 21, &snap)); // one frame short
    TASK_CHECK(1 == cc_playout_pull(p, 1, rec));
    TASK_CHECK(PAD_WORD == rec[0].cc_data); // nothing was queued
    cc_playout_free(p);
    return 0;
}

// Duplicate deadlines, past frame numbers and a surviving original event.
static int case_duplicate_and_past(void)
{
    cc_playout_t* p = cc_playout_create(0);
    cc_playout_record_t rec[TEST_MAX_RECORDS];
    caption_frame_t snap;

    TASK_CHECK(0 == frame_row(&snap, SCREEN_ROWS - 1, "AB"));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "a", 15, &snap));
    TASK_CHECK(CC_PLAYOUT_ERROR_DEADLINE == cc_playout_set(p, "b", 15, &snap));
    TASK_CHECK(5 == cc_playout_pull(p, 5, rec));
    TASK_CHECK(CC_PLAYOUT_ERROR_DEADLINE == cc_playout_set(p, "c", 3, &snap));  // past
    TASK_CHECK(CC_PLAYOUT_ERROR_DEADLINE == cc_playout_set(p, "c", 15, &snap)); // taken
    TASK_CHECK(CC_PLAYOUT_ERROR_DEADLINE == cc_playout_set(p, "c", 16, &snap)); // blocked by a's duplicate EOC
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "c", 30, &snap));
    TASK_CHECK(12 == cc_playout_pull(p, 12, rec));
    TASK_CHECK(rec[10].cc_data == ctrl(eia608_control_end_of_caption)); // event a at 15
    TASK_CHECK(rec[11].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(16 == cc_playout_pull(p, 16, rec)); // frames 17..32
    TASK_CHECK(rec[13].cc_data == ctrl(eia608_control_end_of_caption)); // event c at 30
    TASK_CHECK(rec[14].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(PAD_WORD == rec[15].cc_data);
    cc_playout_free(p);
    return 0;
}

// An infeasible replacement leaves the emitted prefix and schedule untouched.
static int case_infeasible_edit_keeps_playout(void)
{
    static const char long_text[SCREEN_COLS + 1] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    cc_playout_t* edited = cc_playout_create(0);
    cc_playout_t* mirror = cc_playout_create(0);
    cc_playout_record_t r1[TEST_MAX_RECORDS], r2[TEST_MAX_RECORDS];
    caption_frame_t snap, short_snap;

    TASK_CHECK(0 == frame_row(&snap, SCREEN_ROWS - 1, long_text));
    TASK_CHECK(0 == frame_row(&short_snap, SCREEN_ROWS - 1, "AB"));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(edited, "x", 23, &snap));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(mirror, "x", 23, &snap));
    TASK_CHECK(10 == cc_playout_pull(edited, 10, r1));
    TASK_CHECK(10 == cc_playout_pull(mirror, 10, r2));
    TASK_CHECK(0 == records_same(r1, r2, 10));

    TASK_CHECK(CC_PLAYOUT_ERROR_DEADLINE == cc_playout_set(edited, "x", 12, &short_snap));
    TASK_CHECK(20 == cc_playout_pull(edited, 20, r1));
    TASK_CHECK(20 == cc_playout_pull(mirror, 20, r2));
    TASK_CHECK(0 == records_same(r1, r2, 20));
    cc_playout_free(edited);
    cc_playout_free(mirror);
    return 0;
}

// Arbitrary chunk pulls, including a split on the EOC double-send boundary,
// match one large pull frame for frame.
static int case_chunk_equivalence(void)
{
    static const char long_text[SCREEN_COLS + 1] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    static const size_t chunks[] = { 1, 6, 23, 1, 1, 40 };
    cc_playout_t* one = cc_playout_create(0);
    cc_playout_t* split = cc_playout_create(0);
    cc_playout_record_t full[TEST_MAX_RECORDS], part[TEST_MAX_RECORDS];
    caption_frame_t long_snap, ab_snap;
    size_t i, offset = 0;

    TASK_CHECK(0 == frame_row(&long_snap, SCREEN_ROWS - 1, long_text));
    TASK_CHECK(0 == frame_row(&ab_snap, SCREEN_ROWS - 1, "AB"));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(one, "a", 30, &long_snap));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(one, "b", 70, &ab_snap));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(split, "a", 30, &long_snap));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(split, "b", 70, &ab_snap));
    TASK_CHECK(72 == cc_playout_pull(one, 72, full));

    for (i = 0; i < sizeof(chunks) / sizeof(chunks[0]); ++i) {
        TASK_CHECK(chunks[i] == cc_playout_pull(split, chunks[i], part));
        TASK_CHECK(0 == records_same(full + offset, part, chunks[i]));
        offset += chunks[i];
    }

    TASK_CHECK(72 == offset);
    TASK_CHECK(full[30].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(full[31].cc_data == ctrl(eia608_control_end_of_caption));
    cc_playout_free(one);
    cc_playout_free(split);
    return 0;
}

// Idle frames are parity-correct padding with the right frame numbers.
static int case_idle_padding(void)
{
    cc_playout_t* p = cc_playout_create(7);
    cc_playout_record_t rec[8];

    TASK_CHECK(8 == cc_playout_pull(p, 8, rec));

    for (int i = 0; i < 8; ++i) {
        TASK_CHECK(rec[i].frame == (uint64_t)(7 + i));
        TASK_CHECK(PAD_WORD == rec[i].cc_data);
        TASK_CHECK(eia608_parity_varify(rec[i].cc_data));
    }

    TASK_CHECK(15 == cc_playout_position(p));
    TASK_CHECK(0 == cc_playout_pull(p, 0, 0));
    cc_playout_free(p);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// editing group
////////////////////////////////////////////////////////////////////////////////

// A long row already partly transmitted is replaced by a short one: no reset,
// the old tail must not remain when the caption displays.
static int case_inflight_shrink(void)
{
    static const char long_text[SCREEN_COLS + 1] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    cc_playout_t* edited = cc_playout_create(0);
    cc_playout_t* mirror = cc_playout_create(0);
    cc_playout_record_t rec[TEST_MAX_RECORDS], mirror_rec[TEST_MAX_RECORDS];
    caption_frame_t long_snap, short_snap, rx;

    TASK_CHECK(0 == frame_row(&long_snap, SCREEN_ROWS - 1, long_text));
    TASK_CHECK(0 == frame_row(&short_snap, SCREEN_ROWS - 1, "AB"));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(edited, "x", 30, &long_snap));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(mirror, "x", 30, &long_snap));
    TASK_CHECK(12 == cc_playout_pull(edited, 12, rec));
    TASK_CHECK(12 == cc_playout_pull(mirror, 12, mirror_rec));
    TASK_CHECK(0 == records_same(rec, mirror_rec, 12)); // prefix immutable

    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(edited, "x", 30, &short_snap));
    TASK_CHECK(20 == cc_playout_pull(edited, 20, rec + 12));

    // the suffix must not restart loading: no RCL after the shared prefix
    for (int i = 12; i < 30; ++i) {
        TASK_CHECK(rec[i].cc_data != ctrl(eia608_control_resume_caption_loading));
    }

    TASK_CHECK(rec[30].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(rec[31].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(0 == decode_stream(rec, 32, &rx));
    TASK_CHECK(0 == front_row_equals(&rx, SCREEN_ROWS - 1, "AB"));
    cc_playout_free(edited);
    cc_playout_free(mirror);
    return 0;
}

// Cancelling a half-preloaded event leaves its residue in hidden memory; the
// next event diffs against that residue and still displays a clean row.
static int case_inflight_cancel_residue(void)
{
    static const char long_text[SCREEN_COLS + 1] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    cc_playout_t* p = cc_playout_create(0);
    cc_playout_record_t rec[TEST_MAX_RECORDS];
    caption_frame_t long_snap, z_snap, rx;

    TASK_CHECK(0 == frame_row(&long_snap, SCREEN_ROWS - 1, long_text));
    TASK_CHECK(0 == frame_row(&z_snap, SCREEN_ROWS - 1, "Z"));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "x", 30, &long_snap));
    TASK_CHECK(12 == cc_playout_pull(p, 12, rec)); // six char pairs are hidden now
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_cancel(p, "x"));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "y", 40, &z_snap));
    TASK_CHECK(30 == cc_playout_pull(p, 30, rec + 12));
    TASK_CHECK(0 == decode_stream(rec, 42, &rx));
    TASK_CHECK(0 == front_row_equals(&rx, SCREEN_ROWS - 1, "Z")); // tail erased by DER
    cc_playout_free(p);
    return 0;
}

// After the first EOC the id is committed: edits and cancels fail, but the
// duplicate EOC is still transmitted.
static int case_committed_frozen(void)
{
    cc_playout_t* p = cc_playout_create(0);
    cc_playout_record_t rec[16];
    caption_frame_t snap, other;

    TASK_CHECK(0 == frame_row(&snap, SCREEN_ROWS - 1, "AB"));
    TASK_CHECK(0 == frame_row(&other, SCREEN_ROWS - 1, "CD"));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "x", 10, &snap));
    TASK_CHECK(11 == cc_playout_pull(p, 11, rec)); // first EOC now transmitted
    TASK_CHECK(rec[10].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(CC_PLAYOUT_ERROR_STATE == cc_playout_set(p, "x", 20, &other));
    TASK_CHECK(CC_PLAYOUT_ERROR_STATE == cc_playout_cancel(p, "x"));
    TASK_CHECK(1 == cc_playout_pull(p, 1, rec));
    TASK_CHECK(rec[0].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(2 == cc_playout_pull(p, 2, rec));
    TASK_CHECK(PAD_WORD == rec[0].cc_data);
    TASK_CHECK(PAD_WORD == rec[1].cc_data);
    cc_playout_free(p);
    return 0;
}

// Deep-copied snapshots, independent engines and free/create reuse.
static int case_ownership_and_reuse(void)
{
    cc_playout_t* p1 = cc_playout_create(0);
    cc_playout_t* p2 = cc_playout_create(0);
    cc_playout_record_t r1[16], r2[16], r3[16];
    caption_frame_t snap;

    TASK_CHECK(0 == frame_row(&snap, SCREEN_ROWS - 1, "AB"));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p1, "x", 10, &snap));
    snap.front.cell[SCREEN_ROWS - 1][0].data[0] = 'Z'; // mutate after deep copy
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p2, "x", 10, &snap));
    TASK_CHECK(12 == cc_playout_pull(p1, 12, r1));
    TASK_CHECK(12 == cc_playout_pull(p2, 12, r2));
    TASK_CHECK(r1[6].cc_data == chars2('A', 'B'));
    TASK_CHECK(r2[6].cc_data == chars2('Z', 'B'));

    for (int i = 0; i < 12; ++i) {
        TASK_CHECK(r1[i].frame == r2[i].frame);
    }

    cc_playout_free(p1);
    cc_playout_free(p2);

    p1 = cc_playout_create(0);
    TASK_CHECK(0 == frame_row(&snap, SCREEN_ROWS - 1, "AB"));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p1, "x", 10, &snap));
    TASK_CHECK(12 == cc_playout_pull(p1, 12, r3));
    TASK_CHECK(0 == records_same(r1, r3, 12));
    cc_playout_free(p1);
    cc_playout_free(0); // must be a safe no-op
    return 0;
}

// Everything outside the input contract is rejected, not truncated.
static int case_invalid_inputs(void)
{
    cc_playout_t* p = cc_playout_create(0);
    caption_frame_t f;
    cc_playout_record_t rec[4];

    caption_frame_init(&f);
    f.write = &f.front;
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "empty", 10, &f)); // blank screen is legal
    TASK_CHECK(CC_PLAYOUT_ERROR_STATE == cc_playout_cancel(p, "missing"));

    TASK_CHECK(caption_frame_write_char(&f, SCREEN_ROWS - 1, 0, eia608_style_white, 0, "a"));
    TASK_CHECK(CC_PLAYOUT_ERROR_ARG == cc_playout_set(p, "lower", 11, &f));

    caption_frame_init(&f);
    f.write = &f.front;
    TASK_CHECK(caption_frame_write_char(&f, SCREEN_ROWS - 1, 1, eia608_style_white, 0, "A"));
    TASK_CHECK(CC_PLAYOUT_ERROR_ARG == cc_playout_set(p, "gap", 11, &f));

    caption_frame_init(&f);
    f.write = &f.front;
    TASK_CHECK(caption_frame_write_char(&f, SCREEN_ROWS - 1, 0, eia608_style_white, 1, "A"));
    TASK_CHECK(CC_PLAYOUT_ERROR_ARG == cc_playout_set(p, "uln", 11, &f));

    caption_frame_init(&f);
    f.write = &f.front;
    TASK_CHECK(caption_frame_write_char(&f, SCREEN_ROWS - 1, 0, eia608_style_green, 0, "A"));
    TASK_CHECK(CC_PLAYOUT_ERROR_ARG == cc_playout_set(p, "green", 11, &f));

    TASK_CHECK(CC_PLAYOUT_ERROR_ARG == cc_playout_set(p, "", 11, &f));
    TASK_CHECK(CC_PLAYOUT_ERROR_ARG == cc_playout_set(0, "x", 11, &f));
    TASK_CHECK(4 == cc_playout_pull(p, 4, rec)); // valid pulls still work
    TASK_CHECK(0 == frame_row(&f, SCREEN_ROWS - 1, "AB"));
    TASK_CHECK(CC_PLAYOUT_ERROR_DEADLINE == cc_playout_set(p, "late", 2, &f));
    cc_playout_free(p);
    return 0;
}

// Cancelling a fully preloaded head right before its EOC would leave a full
// hidden screen the next event cannot erase in time: the cancel is rejected
// and the original schedule (EOC at the original frame) stays intact.
static int case_cancel_failure_keeps_state(void)
{
    cc_playout_t* p = cc_playout_create(0);
    cc_playout_record_t rec[512];
    caption_frame_t big, blank;

    caption_frame_init(&big);
    big.write = &big.front;

    for (int r = 0; r < SCREEN_ROWS; ++r) {
        for (int c = 0; c < 8; ++c) {
            char ch[2] = { (char)('A' + c), 0 };
            TASK_CHECK(caption_frame_write_char(&big, r, c, eia608_style_white, 0, ch));
        }
    }

    caption_frame_init(&blank);
    blank.write = &blank.front;
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "x", 400, &big));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "y", 404, &blank));
    TASK_CHECK(399 == cc_playout_pull(p, 399, rec));
    TASK_CHECK(CC_PLAYOUT_ERROR_DEADLINE == cc_playout_cancel(p, "x"));
    TASK_CHECK(3 == cc_playout_pull(p, 3, rec)); // frames 399..401
    TASK_CHECK(PAD_WORD == rec[0].cc_data);
    TASK_CHECK(rec[1].cc_data == ctrl(eia608_control_end_of_caption)); // still at 400
    TASK_CHECK(rec[2].cc_data == ctrl(eia608_control_end_of_caption));
    cc_playout_free(p);
    return 0;
}

// Replacing a half-loaded event with a blank screen must erase every hidden
// cell, so the display goes fully empty with no residue.
static int case_inflight_blank(void)
{
    static const char long_text[SCREEN_COLS + 1] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    cc_playout_t* p = cc_playout_create(0);
    cc_playout_record_t rec[TEST_MAX_RECORDS];
    caption_frame_t long_snap, blank, rx;

    TASK_CHECK(0 == frame_row(&long_snap, SCREEN_ROWS - 1, long_text));
    caption_frame_init(&blank);
    blank.write = &blank.front;
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "x", 30, &long_snap));
    TASK_CHECK(12 == cc_playout_pull(p, 12, rec));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "x", 30, &blank));
    TASK_CHECK(22 == cc_playout_pull(p, 22, rec + 12));
    TASK_CHECK(rec[30].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(0 == decode_stream(rec, 32, &rx));
    TASK_CHECK(0 == screen_empty(&rx));
    cc_playout_free(p);
    return 0;
}

// Moving a half-loaded event to a later frame keeps the transmitted prefix and
// only delays the EOC; no RCL is re-sent.
static int case_inflight_move_keeps_prefix(void)
{
    static const char long_text[SCREEN_COLS + 1] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    cc_playout_t* p = cc_playout_create(0);
    cc_playout_t* mirror = cc_playout_create(0);
    cc_playout_record_t rec[TEST_MAX_RECORDS], mirror_rec[16];
    caption_frame_t long_snap, short_snap, rx;

    TASK_CHECK(0 == frame_row(&long_snap, SCREEN_ROWS - 1, long_text));
    TASK_CHECK(0 == frame_row(&short_snap, SCREEN_ROWS - 1, "AB"));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "x", 30, &long_snap));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(mirror, "x", 30, &long_snap));
    TASK_CHECK(12 == cc_playout_pull(p, 12, rec));
    TASK_CHECK(12 == cc_playout_pull(mirror, 12, mirror_rec));
    TASK_CHECK(0 == records_same(rec, mirror_rec, 12));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "x", 40, &short_snap));
    TASK_CHECK(30 == cc_playout_pull(p, 30, rec + 12)); // up to frame 41
    TASK_CHECK(rec[40].cc_data == ctrl(eia608_control_end_of_caption));
    TASK_CHECK(rec[41].cc_data == ctrl(eia608_control_end_of_caption));

    for (int i = 12; i < 40; ++i) {
        TASK_CHECK(rec[i].cc_data != ctrl(eia608_control_resume_caption_loading));
    }

    TASK_CHECK(0 == decode_stream(rec, 42, &rx));
    TASK_CHECK(0 == front_row_equals(&rx, SCREEN_ROWS - 1, "AB"));
    cc_playout_free(p);
    cc_playout_free(mirror);
    return 0;
}

const struct task_case task_cases[] = {
    { "screens", "handwritten_eoc", case_handwritten_eoc },
    { "screens", "der_clears_hidden", case_der_clears_hidden },
    { "screens", "basic_ab_bytes", case_basic_ab_bytes },
    { "screens", "aba_hidden_reuse", case_aba_hidden_reuse },
    { "screens", "cols32_and_odd", case_32cols_and_odd },
    { "screens", "delete_whole_row", case_delete_whole_row },
    { "deadlines", "exact_capacity_and_short", case_exact_capacity_and_short },
    { "deadlines", "duplicate_and_past", case_duplicate_and_past },
    { "deadlines", "infeasible_edit_keeps_playout", case_infeasible_edit_keeps_playout },
    { "deadlines", "chunk_equivalence", case_chunk_equivalence },
    { "deadlines", "idle_padding", case_idle_padding },
    { "deadlines", "cancel_failure_keeps_state", case_cancel_failure_keeps_state },
    { "editing", "inflight_shrink", case_inflight_shrink },
    { "editing", "inflight_blank", case_inflight_blank },
    { "editing", "inflight_move_keeps_prefix", case_inflight_move_keeps_prefix },
    { "editing", "inflight_cancel_residue", case_inflight_cancel_residue },
    { "editing", "committed_frozen", case_committed_frozen },
    { "editing", "ownership_and_reuse", case_ownership_and_reuse },
    { "editing", "invalid_inputs", case_invalid_inputs },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
