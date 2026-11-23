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
#include "playout.h"
#include "diff608.h"
#include "eia608.h"
#include <stdlib.h>
#include <string.h>

// Builds a constrained screen: each non-null row string starts at column 0 and
// holds A-Z/0-9; null or empty rows are blank. The front snapshot is what the
// playout copies.
static void make_screen(caption_frame_t* frame, const char* rows[SCREEN_ROWS])
{
    caption_frame_init(frame);
    frame->write = &frame->front;

    for (int r = 0; r < SCREEN_ROWS; ++r) {
        if (!rows[r]) {
            continue;
        }

        for (int c = 0; rows[r][c]; ++c) {
            utf8_char_t ch[2] = { rows[r][c], 0 };
            caption_frame_write_char(frame, r, c, eia608_style_white, 0, ch);
        }
    }
}

static int front_cell(const caption_frame_t* dec, int r, int c, char want)
{
    const utf8_char_t* got = caption_frame_read_char((caption_frame_t*)dec, r, c, 0, 0);
    return want ? (got[0] == want && got[1] == 0) : (got[0] == 0);
}

// Every non-padding record must carry valid odd parity and consecutive,
// absolute frame numbers starting at first_frame.
static int records_well_formed(const cc_playout_record_t* rec, uint64_t n, uint64_t first_frame)
{
    for (uint64_t i = 0; i < n; ++i) {
        if (rec[i].frame != first_frame + i || !eia608_parity_varify(rec[i].cc_data)) {
            return 0;
        }
    }

    return 1;
}

static int count_nonpad(const cc_playout_record_t* rec, uint64_t n)
{
    int k = 0;

    for (uint64_t i = 0; i < n; ++i) {
        if (!eia608_is_padding(rec[i].cc_data)) {
            ++k;
        }
    }

    return k;
}

static int has_stripped_word(const cc_playout_record_t* rec, uint64_t n, uint16_t stripped)
{
    for (uint64_t i = 0; i < n; ++i) {
        if (eia608_parity_strip(rec[i].cc_data) == stripped) {
            return 1;
        }
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// screens group
////////////////////////////////////////////////////////////////////////////////

// Hand-written control sequence proving EOC really swaps the memories once and
// the duplicate EOC is suppressed.
static int test_eoc_swap_and_duplicate(void)
{
    caption_frame_t dec;
    caption_frame_init(&dec);

    uint16_t rcl = eia608_control_command(eia608_control_resume_caption_loading, 0);
    uint16_t pac = eia608_row_column_pramble(SCREEN_ROWS - 1, 0, 0, 0);
    uint16_t ab = eia608_parity(0x4142);
    uint16_t eoc = eia608_control_command(eia608_control_end_of_caption, 0);
    uint16_t seq[] = { rcl, rcl, pac, pac, ab, eoc, eoc };

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); ++i) {
        TASK_CHECK(eia608_parity_varify(seq[i]));
        TASK_CHECK(LIBCAPTION_ERROR != caption_frame_decode(&dec, seq[i], (double)i / 30));
    }

    // After both EOCs the caption stays displayed; the duplicate does not
    // swap it back away.
    TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, 0, 'A'));
    TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, 1, 'B'));
    TASK_CHECK(dec.back.cell[SCREEN_ROWS - 1][0].data[0] == 0); // old front retained hidden

    // Replay just the swap boundary on a fresh decoder for precise checks.
    caption_frame_t d2;
    caption_frame_init(&d2);
    TASK_CHECK(LIBCAPTION_ERROR != caption_frame_decode(&d2, rcl, 0));
    TASK_CHECK(LIBCAPTION_ERROR != caption_frame_decode(&d2, pac, 0));
    TASK_CHECK(LIBCAPTION_ERROR != caption_frame_decode(&d2, ab, 0));
    TASK_CHECK(front_cell(&d2, SCREEN_ROWS - 1, 0, 0)); // still hidden
    TASK_CHECK(LIBCAPTION_ERROR != caption_frame_decode(&d2, eoc, 0));
    TASK_CHECK(front_cell(&d2, SCREEN_ROWS - 1, 0, 'A')); // first EOC swapped
    TASK_CHECK(front_cell(&d2, SCREEN_ROWS - 1, 1, 'B'));
    TASK_CHECK(d2.back.cell[SCREEN_ROWS - 1][0].data[0] == 0); // old front retained hidden
    TASK_CHECK(LIBCAPTION_ERROR != caption_frame_decode(&d2, eoc, 0));
    TASK_CHECK(front_cell(&d2, SCREEN_ROWS - 1, 0, 'A')); // duplicate did not swap back
    TASK_CHECK(front_cell(&d2, SCREEN_ROWS - 1, 1, 'B'));
    TASK_CHECK(d2.back.cell[SCREEN_ROWS - 1][0].data[0] == 0);
    return 0;
}

// A then B then A: the third event only needs RCL plus the timed EOC pair,
// because the swap leaves the previous A screen in hidden memory.
static int test_aba_hidden_reuse(void)
{
    const char* a_rows[SCREEN_ROWS] = { 0 };
    const char* b_rows[SCREEN_ROWS] = { 0 };
    a_rows[SCREEN_ROWS - 1] = "A";
    b_rows[SCREEN_ROWS - 1] = "B";
    caption_frame_t A, B, A2;
    make_screen(&A, a_rows);
    make_screen(&B, b_rows);
    make_screen(&A2, a_rows);

    cc_playout_t* p = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "a", 10, &A));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "b", 20, &B));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "c", 30, &A2));

    cc_playout_record_t rec[48];
    TASK_CHECK(32 == cc_playout_pull(p, 32, rec));
    TASK_CHECK(records_well_formed(rec, 32, 0));

    // Third event window (frames 22..31): only RCL pair and EOC pair.
    int third = 0;

    for (int i = 22; i < 32; ++i) {
        if (!eia608_is_padding(rec[i].cc_data)) {
            ++third;
        }
    }

    TASK_CHECK(4 == third);

    // Decode the whole stream and check the visible screen at each deadline.
    caption_frame_t dec;
    caption_frame_init(&dec);

    for (int i = 0; i < 32; ++i) {
        TASK_CHECK(LIBCAPTION_ERROR != caption_frame_decode(&dec, rec[i].cc_data, (double)i / 30));

        if (i < 10) {
            TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, 0, 0));
        } else if (i < 20) {
            TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, 0, 'A'));
        } else if (i < 30) {
            TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, 0, 'B'));
        } else {
            TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, 0, 'A'));
        }
    }

    cc_playout_free(p);
    return 0;
}

// A full 32-cell row and an odd-length row: correct word count, null-byte pad
// on the odd tail, and every cell decoded as expected.
static int test_odd_and_32_cells(void)
{
    char full[SCREEN_COLS + 1];

    for (int c = 0; c < SCREEN_COLS; ++c) {
        full[c] = (char)('0' + (c % 10));
    }

    full[SCREEN_COLS] = 0;
    const char* full_rows[SCREEN_ROWS] = { 0 };
    full_rows[SCREEN_ROWS - 1] = full;
    caption_frame_t full_screen;
    make_screen(&full_screen, full_rows);
    TASK_CHECK(diff608_screen_valid(&full_screen));

    cc_playout_t* p = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "full", 40, &full_screen));
    cc_playout_record_t rec[48];
    TASK_CHECK(42 == cc_playout_pull(p, 42, rec));
    // RCL2 + PAC2 + DER2 + 16 character words + EOC2 = 24 non-padding words.
    TASK_CHECK(24 == count_nonpad(rec, 42));

    caption_frame_t dec;
    caption_frame_init(&dec);

    for (int i = 0; i < 42; ++i) {
        caption_frame_decode(&dec, rec[i].cc_data, (double)i / 30);
    }

    for (int c = 0; c < SCREEN_COLS; ++c) {
        TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, c, full[c]));
    }

    cc_playout_free(p);

    const char* odd_rows[SCREEN_ROWS] = { 0 };
    odd_rows[SCREEN_ROWS - 1] = "ABC";
    caption_frame_t odd_screen;
    make_screen(&odd_screen, odd_rows);
    cc_playout_t* q = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(q, "odd", 12, &odd_screen));
    cc_playout_record_t orec[16];
    TASK_CHECK(14 == cc_playout_pull(q, 14, orec));
    // The final odd character is paired with a null low byte (wire 0x80).
    TASK_CHECK(has_stripped_word(orec, 14, 0x4300));
    caption_frame_t odec;
    caption_frame_init(&dec);
    caption_frame_init(&odec);

    for (int i = 0; i < 14; ++i) {
        caption_frame_decode(&odec, orec[i].cc_data, (double)i / 30);
    }

    TASK_CHECK(front_cell(&odec, SCREEN_ROWS - 1, 0, 'A'));
    TASK_CHECK(front_cell(&odec, SCREEN_ROWS - 1, 1, 'B'));
    TASK_CHECK(front_cell(&odec, SCREEN_ROWS - 1, 2, 'C'));
    TASK_CHECK(front_cell(&odec, SCREEN_ROWS - 1, 3, 0)); // no trailing space
    cc_playout_free(q);
    return 0;
}

// Deleting a whole row: once the row sits in hidden memory (by re-displaying
// the same screen, which the swap leaves hidden), a later blank screen must
// clear it with DER rather than rewriting characters or erasing the screen.
static int test_delete_whole_row(void)
{
    const char* on_rows[SCREEN_ROWS] = { 0 };
    const char* off_rows[SCREEN_ROWS] = { 0 };
    on_rows[SCREEN_ROWS - 1] = "HELLO";
    caption_frame_t on, off;
    make_screen(&on, on_rows);
    make_screen(&off, off_rows); // entirely blank

    cc_playout_t* p = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "first", 10, &on));
    // Re-display the identical screen; the swap leaves HELLO in hidden memory.
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "again", 22, &on));
    // The blank screen now diffs against hidden memory that still has the row.
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "blank", 34, &off));
    cc_playout_record_t rec[40];
    TASK_CHECK(37 == cc_playout_pull(p, 37, rec));

    // Third event window (frames 24..36): DER present, whole-screen EDM absent.
    TASK_CHECK(has_stripped_word(rec + 24, 13, 0x1424));
    TASK_CHECK(!has_stripped_word(rec + 24, 13, 0x142C));

    caption_frame_t dec;
    caption_frame_init(&dec);

    for (int i = 0; i < 37; ++i) {
        caption_frame_decode(&dec, rec[i].cc_data, (double)i / 30);
    }

    for (int c = 0; c < SCREEN_COLS; ++c) {
        TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, c, 0));
    }

    cc_playout_free(p);
    return 0;
}

static int test_invalid_screens_rejected(void)
{
    caption_frame_t bad;
    caption_frame_init(&bad);
    bad.write = &bad.front;
    // Lowercase letters are outside the supported alphabet.
    caption_frame_write_char(&bad, 0, 0, eia608_style_white, 0, "a");
    TASK_CHECK(!diff608_screen_valid(&bad));
    cc_playout_t* p = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_INVALID == cc_playout_set(p, "x", 10, &bad));

    caption_frame_t gap;
    caption_frame_init(&gap);
    gap.write = &gap.front;
    // Text must begin at column 0.
    caption_frame_write_char(&gap, 0, 1, eia608_style_white, 0, "A");
    TASK_CHECK(!diff608_screen_valid(&gap));
    TASK_CHECK(CC_PLAYOUT_INVALID == cc_playout_set(p, "y", 10, &gap));
    TASK_CHECK(CC_PLAYOUT_INVALID == cc_playout_set(p, "", 10, &gap));
    TASK_CHECK(CC_PLAYOUT_INVALID == cc_playout_set(0, "z", 10, &gap));
    cc_playout_free(p);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// deadlines group
////////////////////////////////////////////////////////////////////////////////

// A 32-character row needs 22 preload words; display frame 22 is exactly
// feasible, frame 21 is one frame short.
static int test_exact_capacity_and_one_short(void)
{
    char full[SCREEN_COLS + 1];

    for (int c = 0; c < SCREEN_COLS; ++c) {
        full[c] = 'A';
    }

    full[SCREEN_COLS] = 0;
    const char* rows[SCREEN_ROWS] = { 0 };
    rows[SCREEN_ROWS - 1] = full;
    caption_frame_t screen;
    make_screen(&screen, rows);

    cc_playout_t* exact = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(exact, "e", 22, &screen));
    cc_playout_record_t rec[24];
    TASK_CHECK(24 == cc_playout_pull(exact, 24, rec));
    TASK_CHECK(eia608_parity_strip(rec[22].cc_data) == 0x142F); // first EOC on time
    TASK_CHECK(eia608_parity_strip(rec[23].cc_data) == 0x142F); // duplicate next frame
    cc_playout_free(exact);

    cc_playout_t* short_one = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_LATE == cc_playout_set(short_one, "e", 21, &screen));
    cc_playout_free(short_one);
    return 0;
}

static int test_duplicate_deadline_rejected(void)
{
    const char* rows[SCREEN_ROWS] = { 0 };
    rows[SCREEN_ROWS - 1] = "A";
    caption_frame_t screen;
    make_screen(&screen, rows);
    cc_playout_t* p = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "a", 10, &screen));
    TASK_CHECK(CC_PLAYOUT_CONFLICT == cc_playout_set(p, "b", 10, &screen));
    cc_playout_free(p);
    return 0;
}

// The same schedule pulled in one block and in arbitrary fragments must produce
// byte-identical per-frame output, including fragments straddling the EOC pair.
static int test_chunking_across_eoc_boundary(void)
{
    const char* rows1[SCREEN_ROWS] = { 0 };
    const char* rows2[SCREEN_ROWS] = { 0 };
    rows1[SCREEN_ROWS - 1] = "HI";
    rows2[SCREEN_ROWS - 1] = "WORLD9";
    caption_frame_t s1, s2;
    make_screen(&s1, rows1);
    make_screen(&s2, rows2);

    cc_playout_t* whole = cc_playout_create(0);
    cc_playout_t* chunked = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(whole, "1", 7, &s1));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(whole, "2", 18, &s2));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(chunked, "1", 7, &s1));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(chunked, "2", 18, &s2));

    enum { N = 24 };
    cc_playout_record_t w[N], c[N];
    TASK_CHECK(N == cc_playout_pull(whole, N, w));
    int chunks[] = { 1, 1, 6, 1, 1, 2, 5, 7 }; // splits land on frames 7/8 and 18/19
    int off = 0;

    for (size_t k = 0; k < sizeof(chunks) / sizeof(chunks[0]); ++k) {
        TASK_CHECK(chunks[k] == cc_playout_pull(chunked, (uint64_t)chunks[k], c + off));
        off += chunks[k];
    }

    TASK_CHECK(off == N);

    for (int i = 0; i < N; ++i) {
        TASK_CHECK(w[i].frame == c[i].frame);
        TASK_CHECK(w[i].cc_data == c[i].cc_data);
    }

    TASK_CHECK(eia608_parity_strip(w[7].cc_data) == 0x142F);
    TASK_CHECK(eia608_parity_strip(w[8].cc_data) == 0x142F);
    TASK_CHECK(eia608_parity_strip(w[18].cc_data) == 0x142F);
    TASK_CHECK(eia608_parity_strip(w[19].cc_data) == 0x142F);
    cc_playout_free(whole);
    cc_playout_free(chunked);
    return 0;
}

// Adding a second event that cannot be preloaded before its deadline must fail
// and leave the already accepted prefix untouched.
static int test_future_event_budget_rejected(void)
{
    char full[SCREEN_COLS + 1];

    for (int c = 0; c < SCREEN_COLS; ++c) {
        full[c] = 'Z';
    }

    full[SCREEN_COLS] = 0;
    const char* rows[SCREEN_ROWS] = { 0 };
    rows[SCREEN_ROWS - 1] = full;
    caption_frame_t big;
    make_screen(&big, rows);
    const char* small_rows[SCREEN_ROWS] = { 0 };
    small_rows[SCREEN_ROWS - 1] = "Q";
    caption_frame_t small;
    make_screen(&small, small_rows);

    cc_playout_t* p = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "big", 22, &big));
    // big occupies preload 0..21, EOC pair 22..23; small at 24 has no room.
    TASK_CHECK(CC_PLAYOUT_LATE == cc_playout_set(p, "small", 24, &small));
    // The first event still plays exactly as planned.
    cc_playout_record_t rec[26];
    TASK_CHECK(26 == cc_playout_pull(p, 26, rec));
    TASK_CHECK(eia608_parity_strip(rec[22].cc_data) == 0x142F);
    cc_playout_free(p);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// editing group
////////////////////////////////////////////////////////////////////////////////

// A long row partially preloaded, then shortened before its EOC: the visible
// screen must show only the short text with no leftover tail.
static int test_inflight_shorten_no_tail(void)
{
    const char* long_rows[SCREEN_ROWS] = { 0 };
    const char* short_rows[SCREEN_ROWS] = { 0 };
    long_rows[SCREEN_ROWS - 1] = "ABCDE";
    short_rows[SCREEN_ROWS - 1] = "X";
    caption_frame_t long_screen, short_screen;
    make_screen(&long_screen, long_rows);
    make_screen(&short_screen, short_rows);

    cc_playout_t* p = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "e", 20, &long_screen));
    cc_playout_record_t prefix[8];
    TASK_CHECK(8 == cc_playout_pull(p, 8, prefix));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "e", 20, &short_screen));

    cc_playout_record_t rest[16];
    TASK_CHECK(14 == cc_playout_pull(p, 14, rest));

    caption_frame_t dec;
    caption_frame_init(&dec);

    for (int i = 0; i < 8; ++i) {
        caption_frame_decode(&dec, prefix[i].cc_data, 0);
    }

    for (int i = 0; i < 14; ++i) {
        caption_frame_decode(&dec, rest[i].cc_data, 0);
    }

    TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, 0, 'X'));

    for (int c = 1; c < 6; ++c) {
        TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, c, 0));
    }

    cc_playout_free(p);
    return 0;
}

// Cancelling an event after part of its preload is on the wire keeps the
// residual hidden memory; a later event diffs against it without any reset.
static int test_inflight_cancel_keeps_residual(void)
{
    const char* drop_rows[SCREEN_ROWS] = { 0 };
    const char* keep_rows[SCREEN_ROWS] = { 0 };
    drop_rows[SCREEN_ROWS - 1] = "ABCDE";
    keep_rows[SCREEN_ROWS - 1] = "ABXY";
    caption_frame_t drop_screen, keep_screen;
    make_screen(&drop_screen, drop_rows);
    make_screen(&keep_screen, keep_rows);

    cc_playout_t* p = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "drop", 20, &drop_screen));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "keep", 40, &keep_screen));
    cc_playout_record_t prefix[8];
    TASK_CHECK(8 == cc_playout_pull(p, 8, prefix)); // RCL, PAC, DER pairs and AB/CD sent
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_cancel(p, "drop"));
    TASK_CHECK(CC_PLAYOUT_NOT_FOUND == cc_playout_cancel(p, "ghost"));

    cc_playout_record_t rest[40];
    TASK_CHECK(34 == cc_playout_pull(p, 34, rest));

    caption_frame_t dec;
    caption_frame_init(&dec);

    for (int i = 0; i < 8; ++i) {
        caption_frame_decode(&dec, prefix[i].cc_data, 0);
    }

    for (int i = 0; i < 34; ++i) {
        caption_frame_decode(&dec, rest[i].cc_data, 0);
    }

    TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, 0, 'A'));
    TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, 1, 'B'));
    TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, 2, 'X'));
    TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, 3, 'Y'));
    TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, 4, 0));
    cc_playout_free(p);
    return 0;
}

// An edit that cannot meet the deadline fails and the original schedule keeps
// playing unchanged.
static int test_infeasible_edit_keeps_original(void)
{
    char full[SCREEN_COLS + 1];

    for (int c = 0; c < SCREEN_COLS; ++c) {
        full[c] = 'Z';
    }

    full[SCREEN_COLS] = 0;
    const char* rows[SCREEN_ROWS] = { 0 };
    rows[SCREEN_ROWS - 1] = full;
    caption_frame_t big;
    make_screen(&big, rows);
    const char* small_rows[SCREEN_ROWS] = { 0 };
    small_rows[SCREEN_ROWS - 1] = "Q";
    caption_frame_t small;
    make_screen(&small, small_rows);

    cc_playout_t* p = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "x", 22, &big));
    TASK_CHECK(CC_PLAYOUT_LATE == cc_playout_set(p, "x", 5, &small));

    cc_playout_record_t rec[24];
    TASK_CHECK(24 == cc_playout_pull(p, 24, rec));
    caption_frame_t dec;
    caption_frame_init(&dec);

    for (int i = 0; i < 24; ++i) {
        caption_frame_decode(&dec, rec[i].cc_data, 0);
    }

    TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, 0, 'Z'));
    TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, SCREEN_COLS - 1, 'Z'));
    cc_playout_free(p);
    return 0;
}

// Once the first EOC is sent, update and cancel are rejected, while the
// mandatory duplicate EOC is still transmitted.
static int test_committed_event_rejects_edits(void)
{
    const char* rows[SCREEN_ROWS] = { 0 };
    rows[SCREEN_ROWS - 1] = "HI";
    caption_frame_t screen;
    make_screen(&screen, rows);
    const char* other_rows[SCREEN_ROWS] = { 0 };
    other_rows[SCREEN_ROWS - 1] = "Q";
    caption_frame_t other;
    make_screen(&other, other_rows);

    cc_playout_t* p = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "e", 7, &screen));
    cc_playout_record_t rec[10];
    TASK_CHECK(9 == cc_playout_pull(p, 9, rec));
    TASK_CHECK(eia608_parity_strip(rec[7].cc_data) == 0x142F);
    TASK_CHECK(eia608_parity_strip(rec[8].cc_data) == 0x142F);
    TASK_CHECK(CC_PLAYOUT_COMMITTED == cc_playout_set(p, "e", 11, &other));
    TASK_CHECK(CC_PLAYOUT_COMMITTED == cc_playout_cancel(p, "e"));
    cc_playout_free(p);
    return 0;
}

// A drained playout accepts new future events, and two playouts never share
// state because every input screen is deep copied.
static int test_object_reuse_and_isolation(void)
{
    const char* rows[SCREEN_ROWS] = { 0 };
    rows[SCREEN_ROWS - 1] = "AB";
    caption_frame_t screen;
    make_screen(&screen, rows);

    cc_playout_t* reuse = cc_playout_create(0);
    // One row costs 7 preload words, so display 10 leaves comfortable room.
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(reuse, "first", 10, &screen));
    cc_playout_record_t rec[32];
    TASK_CHECK(12 == cc_playout_pull(reuse, 12, rec)); // through the duplicate EOC
    TASK_CHECK(cc_playout_position(reuse) == 12);
    // Reuse the same object for a later event on the advanced timeline.
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(reuse, "second", 22, &screen));
    TASK_CHECK(12 == cc_playout_pull(reuse, 12, rec)); // frames 12..23
    TASK_CHECK(eia608_parity_strip(rec[10].cc_data) == 0x142F); // first EOC at frame 22
    TASK_CHECK(eia608_parity_strip(rec[11].cc_data) == 0x142F); // duplicate at frame 23
    cc_playout_free(reuse);

    cc_playout_t* a = cc_playout_create(0);
    cc_playout_t* b = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(a, "id", 10, &screen));
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(b, "id", 12, &screen));
    // Mutating the caller's screen after set must not change either playout.
    caption_frame_write_char(&screen, SCREEN_ROWS - 1, 0, eia608_style_white, 0, "Z");
    cc_playout_record_t ra[16], rb[16];
    TASK_CHECK(14 == cc_playout_pull(a, 14, ra));
    TASK_CHECK(14 == cc_playout_pull(b, 14, rb));
    TASK_CHECK(eia608_parity_strip(ra[10].cc_data) == 0x142F);
    TASK_CHECK(eia608_parity_strip(rb[12].cc_data) == 0x142F);
    cc_playout_free(a);
    cc_playout_free(b);
    return 0;
}

// Scheduling a new event between the first and second EOC of a committed
// event must account for the still-pending duplicate and keep every deadline.
static int test_schedule_while_duplicate_pending(void)
{
    const char* rows[SCREEN_ROWS] = { 0 };
    const char* next_rows[SCREEN_ROWS] = { 0 };
    rows[SCREEN_ROWS - 1] = "HI";
    next_rows[SCREEN_ROWS - 1] = "OK";
    caption_frame_t screen, next_screen;
    make_screen(&screen, rows);
    make_screen(&next_screen, next_rows);

    cc_playout_t* p = cc_playout_create(0);
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "e", 7, &screen));
    cc_playout_record_t rec[8];
    TASK_CHECK(8 == cc_playout_pull(p, 8, rec)); // frames 0..7; first EOC at 7
    TASK_CHECK(eia608_parity_strip(rec[7].cc_data) == 0x142F);
    // The duplicate EOC at frame 8 is still owed when this is accepted.
    TASK_CHECK(CC_PLAYOUT_OK == cc_playout_set(p, "n", 16, &next_screen));
    cc_playout_record_t more[12];
    TASK_CHECK(12 == cc_playout_pull(p, 12, more)); // frames 8..19
    TASK_CHECK(eia608_parity_strip(more[0].cc_data) == 0x142F); // pending duplicate
    TASK_CHECK(eia608_parity_strip(more[8].cc_data) == 0x142F); // new event EOC at 16
    TASK_CHECK(eia608_parity_strip(more[9].cc_data) == 0x142F); // its duplicate at 17

    caption_frame_t dec;
    caption_frame_init(&dec);

    for (int i = 0; i < 8; ++i) {
        caption_frame_decode(&dec, rec[i].cc_data, 0);
    }

    for (int i = 0; i < 12; ++i) {
        caption_frame_decode(&dec, more[i].cc_data, 0);
    }

    TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, 0, 'O'));
    TASK_CHECK(front_cell(&dec, SCREEN_ROWS - 1, 1, 'K'));
    cc_playout_free(p);
    return 0;
}

const struct task_case task_cases[] = {
    { "screens", "eoc_swap_and_duplicate", test_eoc_swap_and_duplicate },
    { "screens", "aba_hidden_reuse", test_aba_hidden_reuse },
    { "screens", "odd_and_32_cells", test_odd_and_32_cells },
    { "screens", "delete_whole_row", test_delete_whole_row },
    { "screens", "invalid_screens_rejected", test_invalid_screens_rejected },
    { "deadlines", "exact_capacity_and_one_short", test_exact_capacity_and_one_short },
    { "deadlines", "duplicate_deadline_rejected", test_duplicate_deadline_rejected },
    { "deadlines", "chunking_across_eoc_boundary", test_chunking_across_eoc_boundary },
    { "deadlines", "future_event_budget_rejected", test_future_event_budget_rejected },
    { "deadlines", "schedule_while_duplicate_pending", test_schedule_while_duplicate_pending },
    { "editing", "inflight_shorten_no_tail", test_inflight_shorten_no_tail },
    { "editing", "inflight_cancel_keeps_residual", test_inflight_cancel_keeps_residual },
    { "editing", "infeasible_edit_keeps_original", test_infeasible_edit_keeps_original },
    { "editing", "committed_event_rejects_edits", test_committed_event_rejects_edits },
    { "editing", "object_reuse_and_isolation", test_object_reuse_and_isolation },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
