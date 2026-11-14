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
#ifndef LIBCAPTION_SCC_TIMELINE_H
#define LIBCAPTION_SCC_TIMELINE_H
#ifdef __cplusplus
extern "C" {
#endif

#include "utf8.h"
#include "vtt.h"

////////////////////////////////////////////////////////////////////////////////
// scc_timeline - frame-accurate Scenarist SCC to WebVTT import
//
// Frame basis
// -----------
// All frame numbers are integer transmission frames at 30000/1001 frames per
// second (~29.97 fps). Transmission frame n covers the program time interval
// [n*1001/30000, (n+1)*1001/30000) seconds. The first control word of an SCC
// line occupies the frame named by the line's timecode; every following word
// on the line, including 0x8080 padding and repeated control words, occupies
// the next frame. Timecodes use ':' for non-drop and ';' for drop-frame mode
// and convert to the same absolute transmission-frame timeline:
//   non-drop: frame = (hh*3600 + mm*60 + ss) * 30 + ff
//   drop:     frame -= 2 * (total_minutes - total_minutes / 10)
// with hh = 0..23, mm/ss = 0..59, ff = 0..29. In drop-frame mode the labels
// ff = 00 and 01 do not exist at second 0 of minutes not divisible by ten and
// are rejected; ten-minute marks skip nothing. Mixing modes in one file is an
// error. Lines may abut but must not overlap or move backwards; no midnight
// wraparound is assumed.
//
// Interval rules
// --------------
// The file is replayed from the first line through caption_frame_decode(),
// including the lines before the window, so pop-on text staged in the back
// buffer and the decoder's repeated-control-word suppression are honored.
// After each word the visible (front) screen is compared cell by cell
// (character, position, style, underline); a boundary is emitted only when
// the screen changes. The maximal constant-screen intervals are intersected
// with the half-open window [begin_frame, end_frame); empty intersections and
// screens that project to no text are dropped. Cue times remain absolute
// program time (begin_frame is NOT subtracted): each endpoint is converted to
// seconds with frames*1001/30000 and the duration is the difference of the
// two converted endpoints. The last interval is closed at end_frame, so a
// caption still on screen when the file ends - or when the window contains no
// control words at all - is exported through end_frame. Cue text is extracted
// with caption_frame_to_text(); a style, underline or position change splits
// cues even when the projected text is identical.
//
// Errors
// ------
// On success a newly allocated vtt_t is returned (the caller frees it with
// vtt_free()) and *error/*error_line, when non-NULL, are set to
// SCC_TIMELINE_OK/0. On failure NULL is returned, no partial vtt_t is
// produced, and *error/*error_line, when non-NULL, receive the failure code
// and the 1-based input line number (0 when no line is implicated).
////////////////////////////////////////////////////////////////////////////////

typedef enum {
    SCC_TIMELINE_OK = 0, //< no error
    SCC_TIMELINE_ERROR_ARGUMENT, //< NULL data or begin_frame >= end_frame (error_line is 0)
    SCC_TIMELINE_ERROR_HEADER, //< first line is not exactly "Scenarist_SCC V1.0"
    SCC_TIMELINE_ERROR_TIMECODE, //< malformed timecode, component out of range, nonexistent
                                 //< drop-frame label, or mixed ':'/';' modes in one file
    SCC_TIMELINE_ERROR_ORDER, //< line starts before the previous line's words end
    SCC_TIMELINE_ERROR_WORD, //< control word is not four hex digits, bad separator, or truncated line
    SCC_TIMELINE_ERROR_PARITY, //< control word fails EIA-608 odd parity
    SCC_TIMELINE_ERROR_OVERFLOW, //< word count or frame arithmetic overflow
    SCC_TIMELINE_ERROR_MEMORY //< allocation failure
} scc_timeline_error_t;

/*! \brief Imports the SCC text in [data, data+size) into a new vtt_t covering
    the half-open window [begin_frame, end_frame) of integer transmission
    frames. data does not need to be NUL terminated and is never read past
    size bytes. See the file-level comment above for frame, interval and
    error semantics.
    \param data SCC text, need not be NUL terminated
    \param size number of bytes at data
    \param begin_frame first transmission frame of the window (inclusive)
    \param end_frame last transmission frame of the window (exclusive)
    \param error optional out parameter for the scc_timeline_error_t result
    \param error_line optional out parameter for the 1-based error line
*/
vtt_t* scc_timeline_to_vtt(const utf8_char_t* data, size_t size, uint32_t begin_frame, uint32_t end_frame, scc_timeline_error_t* error, size_t* error_line);

#ifdef __cplusplus
}
#endif
#endif
