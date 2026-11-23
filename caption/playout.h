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
/* The above copyright notice and its permission notice shall be included in                  */
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
#ifndef LIBCAPTION_PLAYOUT_H
#define LIBCAPTION_PLAYOUT_H
#ifdef __cplusplus
extern "C" {
#endif

#include "caption.h"
#include <stdint.h>

// RCL pair + largest possible row diff + the first (timed) EOC.
#define CC_PLAYOUT_MAX_PLAN_WORDS (2 + DIFF608_MAX_ROW_WORDS + 1)
// Longest supported event id, including the terminating null byte.
#define CC_PLAYOUT_MAX_ID 256

typedef struct cc_playout cc_playout_t;

// One 608 word for one absolute transmission frame. cc_data always carries
// correct odd parity; an idle frame holds the padding word 0x8080.
typedef struct {
    uint64_t frame;
    uint16_t cc_data;
} cc_playout_record_t;

typedef enum {
    CC_PLAYOUT_OK = 0,
    CC_PLAYOUT_INVALID = -1, //< null argument, empty id or unsupported screen
    CC_PLAYOUT_NOT_FOUND = -2, //< cancel of an unknown id
    CC_PLAYOUT_CONFLICT = -3, //< another id already owns the display frame
    CC_PLAYOUT_LATE = -4, //< queue cannot be preloaded in time; nothing changed
    CC_PLAYOUT_COMMITTED = -5, //< first EOC already sent; update/cancel refused
    CC_PLAYOUT_OVERFLOW = -6 //< frame arithmetic or capacity would overflow
} cc_playout_status_t;

/*!
    \brief Creates a CC1 pop-on playout starting at frame start_frame.
    The modeled receiver starts with both memories empty. No wall clock,
    threads or network are used.
    \param start_frame non-negative absolute frame number of the first pull
    \return heap allocated playout, or null on allocation failure
*/
cc_playout_t* cc_playout_create(uint64_t start_frame);

/*!
    \brief Frees a playout and every deep-copied event screen it holds.
    \param playout may be null
*/
void cc_playout_free(cc_playout_t* playout);

/*!
    \brief Absolute frame number of the next record pull will produce.
*/
uint64_t cc_playout_position(const cc_playout_t* playout);

/*!
    \brief Adds or replaces a future display event, identified by id.
    The screen is deep copied: later changes to the caller's frame never affect
    the playout. The screen must satisfy diff608_screen_valid(); unsupported
    characters are rejected, never dropped. display_frame must be reachable:
    the whole uncommitted queue (including later events) is replayed against
    the real on-the-wire state, and the call fails unless every event can keep
    its exact display frame. A failed call changes nothing already transmitted,
    queued or pulled. Replacing an event whose first EOC was already sent fails
    with CC_PLAYOUT_COMMITTED; two ids may never share a display frame.
    \return CC_PLAYOUT_OK or a negative cc_playout_status_t
*/
int cc_playout_set(cc_playout_t* playout, const char* id, uint64_t display_frame, const caption_frame_t* screen);

/*!
    \brief Removes a not-yet-committed event.
    Words already transmitted stay on the wire and in the modeled hidden
    memory; the receiver is never reset. The remaining queue is replayed to
    prove every later deadline is still reachable. On failure the event stays
    queued and the state is untouched.
    \return CC_PLAYOUT_OK or a negative cc_playout_status_t
*/
int cc_playout_cancel(cc_playout_t* playout, const char* id);

/*!
    \brief Produces exactly count records starting at the current position.
    Each transmission frame holds one odd-parity 16-bit word; idle frames are
    padding. The first EOC of an event is emitted exactly on its display frame
    and the duplicate EOC on the following frame; preloading starts as early as
    possible but never early enough to display prematurely. Fragmenting pulls
    never changes the per-frame output. out must hold count records.
    \return count on success, or a negative cc_playout_status_t
*/
int64_t cc_playout_pull(cc_playout_t* playout, uint64_t count, cc_playout_record_t* out);

#ifdef __cplusplus
}
#endif
#endif
