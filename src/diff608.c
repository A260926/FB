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
#include <string.h>

#define DIFF608_CHANNEL 0

static int diff608_supported_char(char c)
{
    return ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9');
}

// Length of the run starting at column 0. Returns -1 when the row violates the
// playout input contract (gap, unsupported character, style or underline).
static int diff608_row_length_checked(const caption_frame_buffer_t* screen, int row)
{
    int col, len;

    for (col = 0; col < SCREEN_COLS; ++col) {
        const caption_frame_cell_t* cell = &screen->cell[row][col];

        if (0 == cell->data[0]) {
            break;
        }

        // single-byte A-Z/0-9 only, always white, never underlined
        if (0 != cell->data[1] || !diff608_supported_char(cell->data[0]) ||
            eia608_style_white != cell->sty || 0 != cell->uln) {
            return -1;
        }
    }

    len = col;

    // everything after the run must be an empty (zeroed) cell
    for (; col < SCREEN_COLS; ++col) {
        const caption_frame_cell_t* cell = &screen->cell[row][col];

        if (0 != cell->data[0] || 0 != cell->sty || 0 != cell->uln) {
            return -1;
        }
    }

    return len;
}

int diff608_screen_validate(const caption_frame_buffer_t* screen)
{
    int row;

    if (!screen) {
        return 0;
    }

    for (row = 0; row < SCREEN_ROWS; ++row) {
        if (0 > diff608_row_length_checked(screen, row)) {
            return 0;
        }
    }

    return 1;
}

static int diff608_row_equal(const caption_frame_buffer_t* a, const caption_frame_buffer_t* b, int row)
{
    return 0 == memcmp(&a->cell[row][0], &b->cell[row][0], sizeof(caption_frame_cell_t) * SCREEN_COLS);
}

static libcaption_stauts_t diff608_append(diff608_plan_t* plan, uint16_t word)
{
    if (DIFF608_MAX_WORDS <= plan->size) {
        return LIBCAPTION_ERROR;
    }

    plan->word[plan->size++] = eia608_parity(word);
    return LIBCAPTION_OK;
}

static libcaption_stauts_t diff608_append_twice(diff608_plan_t* plan, uint16_t word)
{
    if (LIBCAPTION_OK != diff608_append(plan, word)) {
        return LIBCAPTION_ERROR;
    }

    return diff608_append(plan, word);
}

size_t diff608_row_words(const caption_frame_buffer_t* hidden, const caption_frame_buffer_t* target)
{
    size_t words = 0;
    int row;

    for (row = 0; row < SCREEN_ROWS; ++row) {
        if (!diff608_row_equal(hidden, target, row)) {
            int len = diff608_row_length_checked(target, row);
            // PAC pair + DER pair + character pairs (odd run pads the low byte)
            words += 4 + (size_t)((len + 1) / 2);
        }
    }

    return words;
}

libcaption_stauts_t diff608_plan_append_rows(diff608_plan_t* plan, const caption_frame_buffer_t* hidden,
    const caption_frame_buffer_t* target)
{
    int row;

    for (row = 0; row < SCREEN_ROWS; ++row) {
        int col, len;

        if (diff608_row_equal(hidden, target, row)) {
            continue; // unchanged rows are never rewritten
        }

        len = diff608_row_length_checked(target, row);

        if (0 > len) {
            return LIBCAPTION_ERROR;
        }

        // locate the row at column 0, then clear the old tail before writing
        if (LIBCAPTION_OK != diff608_append_twice(plan, eia608_row_column_pramble(row, 0, DIFF608_CHANNEL, 0))) {
            return LIBCAPTION_ERROR;
        }

        if (LIBCAPTION_OK != diff608_append_twice(plan, eia608_control_command(eia608_control_delete_to_end_of_row, DIFF608_CHANNEL))) {
            return LIBCAPTION_ERROR;
        }

        // character pairs; an odd last character is paired with a null byte
        for (col = 0; col < len; col += 2) {
            uint16_t word = (uint16_t)((unsigned char)target->cell[row][col].data[0] << 8);

            if (col + 1 < len) {
                word |= (uint16_t)((unsigned char)target->cell[row][col + 1].data[0]);
            }

            if (LIBCAPTION_OK != diff608_append(plan, word)) {
                return LIBCAPTION_ERROR;
            }
        }
    }

    return LIBCAPTION_OK;
}

libcaption_stauts_t diff608_append_eoc(diff608_plan_t* plan)
{
    return diff608_append_twice(plan, eia608_control_command(eia608_control_end_of_caption, DIFF608_CHANNEL));
}

libcaption_stauts_t diff608_plan_build(diff608_plan_t* plan, const caption_frame_buffer_t* hidden,
    const caption_frame_buffer_t* target)
{
    if (!plan || !hidden || !target || !diff608_screen_validate(target)) {
        return LIBCAPTION_ERROR;
    }

    plan->size = 0;

    if (LIBCAPTION_OK != diff608_append_twice(plan, eia608_control_command(eia608_control_resume_caption_loading, DIFF608_CHANNEL))) {
        return LIBCAPTION_ERROR;
    }

    if (LIBCAPTION_OK != diff608_plan_append_rows(plan, hidden, target)) {
        return LIBCAPTION_ERROR;
    }

    if (LIBCAPTION_OK != diff608_append_twice(plan, eia608_control_command(eia608_control_end_of_caption, DIFF608_CHANNEL))) {
        return LIBCAPTION_ERROR;
    }

    return LIBCAPTION_OK;
}
