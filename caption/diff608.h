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
#include <stddef.h>
#include <stdint.h>

/*!
    \file diff608.h
    \brief Stateful pop-on EIA-608 screen differencing for CC1.

    A diff transforms one hidden (non-displayed) 608 screen into a target
    screen without erasing the whole display. Every emitted 16-bit word
    already carries correct odd parity on both bytes.

    Frame budget (one word per transmission frame, worst case, 15 rows):
      - 2x RCL (resume caption loading)
      - per changed row: 2x PAC (column 0 preamble), 2x DER (delete to end
        of row) and ceil(len/2) character pairs, at most 16, so 20 words
      - 2x EOC (end of caption)
    DIFF608_MAX_WORDS is sized above that worst case.

    Ownership: the differ never copies its inputs; callers keep ownership of
    both screens and of the plan object. Plans are plain value structs.
*/
#define DIFF608_MAX_WORDS 384

typedef struct {
    size_t size;                       //< number of valid words, 0 when empty
    uint16_t word[DIFF608_MAX_WORDS]; //< parity-corrected 608 words, in send order
} diff608_plan_t;

/*!
    \brief Validates a playout input screen.
    \param screen screen to validate, must not be NULL
    \return 1 when every row is empty or holds A-Z/0-9 characters starting at
            column 0 with no gaps, white style and no underline; 0 otherwise
            (unsupported characters, leading gaps, styles or underline all fail)
*/
int diff608_screen_validate(const caption_frame_buffer_t* screen);

/*!
    \brief Number of 608 words required to turn hidden into target.
    \param hidden current content of the receiver hidden memory
    \param target desired hidden memory before the EOC swap
    \return word count excluding RCL/EOC framing; 0 when the screens match
*/
size_t diff608_row_words(const caption_frame_buffer_t* hidden, const caption_frame_buffer_t* target);

/*!
    \brief Builds a complete pop-on diff plan: RCL pair, changed rows in
           ascending row order (PAC pair, DER pair, character pairs), EOC pair.
    \param plan destination, reset on entry
    \param hidden current content of the receiver hidden memory
    \param target desired screen
    \return LIBCAPTION_OK on success, LIBCAPTION_ERROR on invalid input or
            overflow of DIFF608_MAX_WORDS
*/
libcaption_stauts_t diff608_plan_build(diff608_plan_t* plan, const caption_frame_buffer_t* hidden,
    const caption_frame_buffer_t* target);

/*!
    \brief Appends only the changed-row portion of a diff (no RCL, no EOC).
    Used to re-plan the unsent suffix of an event whose text changed after
    some of its words were already transmitted; the receiver is never reset
    and the visible screen is never touched.
    \param plan plan to append to
    \param hidden content of the receiver hidden memory including every word
                  already transmitted for this event
    \param target new desired screen
    \return LIBCAPTION_OK on success, LIBCAPTION_ERROR on overflow
*/
libcaption_stauts_t diff608_plan_append_rows(diff608_plan_t* plan, const caption_frame_buffer_t* hidden,
    const caption_frame_buffer_t* target);

/*!
    \brief Appends the duplicated end-of-caption pair to a plan.
    \return LIBCAPTION_OK on success, LIBCAPTION_ERROR on overflow
*/
libcaption_stauts_t diff608_append_eoc(diff608_plan_t* plan);

#ifdef __cplusplus
}
#endif
#endif
