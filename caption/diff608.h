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
#ifndef LIBCAPTION_DIFF608_H
#define LIBCAPTION_DIFF608_H
#ifdef __cplusplus
extern "C" {
#endif

#include "caption.h"

// Largest word sequence diff608_encode_rows() can append for one screen:
// 15 rows * (2 PAC + 2 DER + ceil(32/2) character words) = 300 words.
#define DIFF608_MAX_ROW_WORDS (SCREEN_ROWS * (4 + (SCREEN_COLS + 1) / 2))

typedef enum {
    DIFF608_OK = 0,
    DIFF608_INVALID = -1, //< unsupported screen content
    DIFF608_NO_ROOM = -2 //< output array too small
} diff608_status_t;

/*!
    \brief Validates the constrained pop-on screen format used by the playout.
    Every row must be empty or hold a single run of A-Z / 0-9 characters
    starting at column 0; all other cells are empty, white and not underlined.
    An entirely empty screen is valid.
    \param screen frame whose front buffer is validated (may not be null)
    \return 1 when valid, 0 otherwise
*/
int diff608_screen_valid(const caption_frame_t* screen);

/*!
    \brief Encodes the row-level difference between two screen memories.
    Rows are compared in ascending row order. Each changed row emits a column-0
    PAC twice, DER twice (clearing the old row tail, including a full-row
    rewrite), then the row text as paired basic-NA characters; an odd final
    character is paired with a space in the low byte. Rows that compare equal
    are not touched, and no whole-screen erase is ever emitted. The caller
    owns the surrounding RCL and EOC pairs.
    All emitted words carry correct odd parity.
    \param out destination array
    \param cap capacity of out in words
    \param from hidden memory the receiver currently holds
    \param target desired hidden memory after the sequence is applied
    \return number of words appended (>= 0), or a negative diff608_status_t
*/
int diff608_encode_rows(uint16_t* out, int cap, const caption_frame_buffer_t* from, const caption_frame_buffer_t* target);

#ifdef __cplusplus
}
#endif
#endif
