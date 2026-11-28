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
#ifndef LIBCAPTION_PLAYOUT_H
#define LIBCAPTION_PLAYOUT_H
#ifdef __cplusplus
extern "C" {
#endif

#include "caption.h"
#include <stddef.h>
#include <stdint.h>

/*!
    \file playout.h
    \brief Stateful CC1 pop-on playout engine.

    The engine schedules pop-on EIA-608 caption events onto a stream where
    every transmission frame carries exactly one parity-corrected 16-bit
    word. It owns no threads, clocks, sockets or third-party state: time is
    the integer frame position advanced only by cc_playout_pull(), and idle
    frames are filled with padding (0x8080).

    \section Ownership
    cc_playout_create() allocates an engine; cc_playout_free() releases it and
    every event, id string and screen snapshot held inside. Screens passed to
    cc_playout_set() are deep copied, so callers may reuse or free their
    caption_frame_t immediately. Two engines never share memory.

    \section Budget
    Each event emits 2x RCL, per changed row 2x PAC, 2x DER and ceil(len/2)
    character pairs, then 2x EOC. The first EOC is emitted exactly at the
    event display frame (the second one frame later); words are preloaded as
    early as possible and the gap before the EOC is padded. An event whose
    words cannot finish before its display frame is rejected.

    \section Errors
    All mutating calls fail atomically: on error the emitted prefix, the
    queued events and the current frame position are left completely
    unchanged. Invalid screens are rejected rather than truncated. An event
    is committed once its first EOC has been pulled; committed events can no
    longer be replaced or cancelled.
*/
#define CC_PLAYOUT_MAX_ID 255

typedef enum {
    CC_PLAYOUT_OK = 0,        //< success
    CC_PLAYOUT_ERROR_ARG = 1, //< NULL/empty argument or screen violating the input contract
    CC_PLAYOUT_ERROR_NOMEM = 2, //< allocation failure
    CC_PLAYOUT_ERROR_STATE = 3, //< unknown id, or id already committed
    CC_PLAYOUT_ERROR_DEADLINE = 4, //< duplicate display frame, past frame number, or budget infeasible
} cc_playout_status_t;

typedef struct cc_playout cc_playout_t;

typedef struct {
    uint64_t frame;   //< absolute frame number, starting at the create() start frame
    uint16_t cc_data; //< one odd-parity 608 word; 0x8080 padding when idle
} cc_playout_record_t;

/*!
    \brief Creates a playout engine.
    \param start_frame nonnegative absolute frame number of the first pull
    \return engine, or NULL on allocation failure. The modeled receiver starts
            with both display and hidden screens empty.
*/
cc_playout_t* cc_playout_create(uint64_t start_frame);

/*!
    \brief Frees an engine and everything it owns. NULL is ignored.
*/
void cc_playout_free(cc_playout_t* playout);

/*!
    \brief Adds or replaces a future display event.
    \param playout engine
    \param id NUL-terminated unique event id, at most CC_PLAYOUT_MAX_ID bytes
    \param display_frame nonnegative absolute frame of the first EOC
    \param snapshot caption_frame_t whose front buffer is deep copied. Every
           row must be empty or a column-0 run of A-Z/0-9, white, no underline
    \return CC_PLAYOUT_OK or an error code. Replacing an event whose first EOC
            was already pulled, reusing another id's display frame, or a plan
            the remaining frame budget cannot satisfy all fail without changing
            engine state.
*/
cc_playout_status_t cc_playout_set(cc_playout_t* playout, const char* id, uint64_t display_frame,
    const caption_frame_t* snapshot);

/*!
    \brief Cancels a not-yet-committed event.
    \return CC_PLAYOUT_OK, CC_PLAYOUT_ERROR_STATE for an unknown or committed
            id, CC_PLAYOUT_ERROR_DEADLINE when removing it makes a later event
            miss its deadline (in that case nothing changes). Words already
            transmitted for the event stay on the wire; their residue remains
            in receiver hidden memory and later diffs build on top of it.
*/
cc_playout_status_t cc_playout_cancel(cc_playout_t* playout, const char* id);

/*!
    \brief Pulls the next count frame records, padding idle frames.
    \param playout engine
    \param count number of frames to advance, may be zero
    \param out destination for exactly count records, may be NULL only when
           count is zero
    \return number of records written, always count on valid arguments; zero
            only on bad arguments. Repeated pulls and arbitrary chunk sizes
            yield the same per-frame words as one large pull.
*/
size_t cc_playout_pull(cc_playout_t* playout, size_t count, cc_playout_record_t* out);

/*!
    \brief Absolute frame number of the next record that would be pulled.
*/
uint64_t cc_playout_position(const cc_playout_t* playout);

#ifdef __cplusplus
}
#endif
#endif
