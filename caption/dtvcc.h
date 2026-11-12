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
#ifndef LIBCAPTION_DTVCC_H
#define LIBCAPTION_DTVCC_H
#ifdef __cplusplus
extern "C" {
#endif

#include "cea708.h"
#include "utf8.h"

// DTVCC services are numbered 1 to 63. Service 0 is reserved for padding.
#define DTVCC_SERVICE_COUNT 64
// Each service owns 8 independent windows.
#define DTVCC_WINDOW_COUNT 8
// CEA-708 window dimensions: row count is a 4 bit field (1-16), column count 6 bit (1-64).
#define DTVCC_MAX_ROWS 16
#define DTVCC_MAX_COLS 64

typedef enum {
    DTVCC_STATUS_OK = 0, //< input consumed
    DTVCC_STATUS_ERROR = 1, //< malformed input (truncated command, bad service block, ...)
    DTVCC_STATUS_UNSUPPORTED = 2, //< well formed input using commands outside the supported set
} dtvcc_status_t;

// Self contained copy of the state of a single service window.
// Only transport and text state is exposed. Fonts, colors and pixel rendering
// are intentionally out of scope, layout is left to the caller.
typedef struct {
    int service; //< 1 to 63
    int window; //< 0 to 7
    int defined; //< window is defined
    int visible;
    int priority;
    int row_lock;
    int col_lock;
    int relative_pos;
    int anchor_vertical;
    int anchor_horizontal;
    int anchor_point;
    int row_count; //< 1 to DTVCC_MAX_ROWS when defined
    int col_count; //< 1 to DTVCC_MAX_COLS when defined
    int pen_row;
    int pen_col;
    int window_style;
    int pen_style;
    double timestamp; //< timestamp of the most recent input consumed by the decoder
    utf8_char_t text[DTVCC_MAX_ROWS][DTVCC_MAX_COLS * 4 + 1]; //< per row text, trailing blanks trimmed
} dtvcc_window_snapshot_t;

typedef struct dtvcc_decoder_t dtvcc_decoder_t;

/*! \brief Creates a new DTVCC decoder. Free it with dtvcc_decoder_free.
*/
dtvcc_decoder_t* dtvcc_decoder_create(void);
/*! \brief Frees a DTVCC decoder created with dtvcc_decoder_create.
*/
void dtvcc_decoder_free(dtvcc_decoder_t* decoder);
/*! \brief Clears all services, windows and any partially assembled packet.
*/
void dtvcc_decoder_reset(dtvcc_decoder_t* decoder);
/*! \brief Consumes one cea708_t.

    Only when user_identifier is GA94 and process_cc_data_flag is set are the native
    DTVCC entries (cc_type_dtvcc_packet_start / cc_type_dtvcc_packet_data) consumed,
    using the cc_valid / cc_type / cc_data fields and cea708->timestamp. 608 entries
    (cc_type 0/1) are ignored here, decode them with caption_frame_decode instead.

    DTVCC packets are assembled across calls and interpreted only once complete.
    Detected packet loss (a start interrupting an unfinished packet, or a sequence
    number jump) clears all service windows until the next complete packet arrives.
    A packet that fails to decode is rejected as a whole, previously received
    packets are not affected.

    Returns DTVCC_STATUS_OK, DTVCC_STATUS_ERROR (malformed input) or
    DTVCC_STATUS_UNSUPPORTED (commands outside the supported set).
*/
dtvcc_status_t dtvcc_decoder_decode(dtvcc_decoder_t* decoder, const cea708_t* cea708);
/*! \brief Copies the current state of the given service window into snapshot.

    The snapshot is caller owned and fully self contained, later decoder input
    never modifies it. Returns 1 if the window is defined (snapshot holds valid
    data), 0 otherwise (snapshot is zeroed except for the service and window
    numbers).
*/
int dtvcc_decoder_window_snapshot(const dtvcc_decoder_t* decoder, int service, int window, dtvcc_window_snapshot_t* snapshot);

#ifdef __cplusplus
}
#endif
#endif
