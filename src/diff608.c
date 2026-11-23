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
#include "diff608.h"
#include "eia608.h"

static int valid_char(unsigned char c)
{
    return ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9');
}

// A valid row is empty, or a single run of A-Z/0-9 starting at column 0,
// white and not underlined, with every remaining cell blank.
static int row_valid(const caption_frame_cell_t row[SCREEN_COLS])
{
    int c = 0;

    for (; c < SCREEN_COLS && row[c].data[0]; ++c) {
        if (!valid_char((unsigned char)row[c].data[0]) || row[c].data[1] || eia608_style_white != row[c].sty || row[c].uln) {
            return 0;
        }
    }

    for (; c < SCREEN_COLS; ++c) {
        if (row[c].data[0] || eia608_style_white != row[c].sty || row[c].uln) {
            return 0;
        }
    }

    return 1;
}

int diff608_screen_valid(const caption_frame_t* screen)
{
    if (!screen) {
        return 0;
    }

    for (int r = 0; r < SCREEN_ROWS; ++r) {
        if (!row_valid(screen->front.cell[r])) {
            return 0;
        }
    }

    return 1;
}

static int row_equal(const caption_frame_cell_t a[SCREEN_COLS], const caption_frame_cell_t b[SCREEN_COLS])
{
    for (int c = 0; c < SCREEN_COLS; ++c) {
        if (a[c].data[0] != b[c].data[0]) {
            return 0;
        }
    }

    return 1;
}

static int append_word(uint16_t* out, int cap, int* used, uint16_t word)
{
    if (*used >= cap) {
        return DIFF608_NO_ROOM;
    }

    out[(*used)++] = word;
    return DIFF608_OK;
}

static int append_twice(uint16_t* out, int cap, int* used, uint16_t word)
{
    int status = append_word(out, cap, used, word);

    if (DIFF608_OK == status) {
        status = append_word(out, cap, used, word);
    }

    return status;
}

int diff608_encode_rows(uint16_t* out, int cap, const caption_frame_buffer_t* from, const caption_frame_buffer_t* target)
{
    int used = 0;

    if (!out || cap < 0 || !from || !target) {
        return DIFF608_INVALID;
    }

    for (int r = 0; r < SCREEN_ROWS; ++r) {
        if (row_equal(from->cell[r], target->cell[r])) {
            continue; // unchanged rows are rewritten never
        }

        // Locate column 0, erase the old row tail (also covers full rewrites),
        // then stream the replacement text.
        int status = append_twice(out, cap, &used, eia608_row_column_pramble(r, 0, 0, 0));

        if (DIFF608_OK != status) {
            return status;
        }

        status = append_twice(out, cap, &used, eia608_control_command(eia608_control_delete_to_end_of_row, 0));

        if (DIFF608_OK != status) {
            return status;
        }

        int len = 0;

        while (len < SCREEN_COLS && target->cell[r][len].data[0]) {
            ++len;
        }

        for (int c = 0; c < len; c += 2) {
            uint16_t hi = (uint16_t)(unsigned char)target->cell[r][c].data[0];
            // An odd final character is paired with the 608 null byte. Parity
            // turns 0x00 into wire 0x80, which the decoder consumes without
            // writing a trailing space.
            uint16_t lo = (c + 1 < len) ? (uint16_t)(unsigned char)target->cell[r][c + 1].data[0] : 0x00;
            status = append_word(out, cap, &used, eia608_parity((uint16_t)((hi << 8) | lo)));

            if (DIFF608_OK != status) {
                return status;
            }
        }
    }

    return used;
}
