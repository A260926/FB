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
#ifndef LIBCAPTION_FLV_SPLITTER_H
#define LIBCAPTION_FLV_SPLITTER_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/*
 * Streaming FLV/AVC caption channel splitter.
 *
 * A single FLV byte stream carrying one AVC stream and CEA-608/708 captions
 * is de-multiplexed into four independent FLV byte streams, one per caption
 * service: 1 = CC1, 2 = CC2, 3 = CC3, 4 = CC4.  Audio, script and non-AVC
 * video tags, the AVC configuration tag, composition times and 32 bit
 * timestamps are forwarded without re-encoding; only the cc_data entries of
 * ATSC/GA94 user data SEI messages (user_data_type_code 3,
 * process_cc_data_flag set) are projected per service.
 *
 * Each emitted byte buffer is owned by the splitter and remains valid until
 * the callback accepts it (returns 0), rejects it for now (returns 1) or a
 * later call fails.  The callback must copy anything it wishes to keep; it
 * must not retain or free the pointer.  Emission is strictly ordered from
 * service 1 through service 4.  A paused service is retried with exactly the
 * same bytes before any higher numbered service is called; accepted services
 * are never replayed.  Returning -1 aborts the split permanently, after which
 * no further callback is made.
 *
 * The splitter buffers at most one input FLV header/tag plus the four output
 * copies of it; it never buffers the whole stream.
 */
typedef struct caption_flv_splitter_t caption_flv_splitter_t;

/*
 * Called with one complete FLV header or tag destined for channel
 * `service` (1..4).  Returns 0 to accept, 1 if this buffer is not accepted
 * yet (it will be offered again later, unchanged) and -1 to abort.
 */
typedef int (*caption_flv_emit_fn)(unsigned service, const uint8_t* bytes, size_t size, void* opaque);

/*
 * Create a splitter.  `max_tag` bounds the total size of a single complete
 * FLV tag, including its 11 byte header, its body and the trailing 4 byte
 * PreviousTagSize field; it must be at least 13.  A compiled-in FLV tag or
 * one produced by the splitter is rejected if it would exceed this bound.
 * `emit` must not be NULL.  Returns NULL for invalid arguments or when out
 * of memory.
 */
caption_flv_splitter_t* caption_flv_splitter_new(size_t max_tag, caption_flv_emit_fn emit, void* opaque);

/*
 * Feed the next `size` bytes of the input stream.  Input may be split at any
 * byte boundary.  `consumed` receives the number of bytes actually taken from
 * `data` during this call; it is always <= size and may be 0 when output from
 * a previous call is still pending.  Feeding zero bytes is allowed and can be
 * used to drive pending output.
 *
 * Returns 0 when the supplied data was processed (or simply buffered), 1 when
 * emission is paused because a callback has not accepted its buffer yet, and
 * -1 on a format error, truncation or callback abort.  Once -1 has been
 * returned the splitter stays failed and every later call returns -1.
 *
 * `consumed` may be NULL, in which case the byte count is not reported.
 */
int caption_flv_splitter_feed(caption_flv_splitter_t* ctx, const uint8_t* data, size_t size, size_t* consumed);

/*
 * Flush any pending output and finish the stream.  It returns 0 on success,
 * 1 when output is still pending (call again, optionally after feeding zero
 * bytes) and -1 on truncation or a callback abort.  On success later calls
 * keep returning 0.  Feeding non-empty data after a successful finish is an
 * error.  Freeing the splitter does not emit anything.
 */
int caption_flv_splitter_finish(caption_flv_splitter_t* ctx);

/*
 * Release the splitter and any buffered input/output.  Pending output is
 * discarded, not delivered.
 */
void caption_flv_splitter_free(caption_flv_splitter_t* ctx);

#ifdef __cplusplus
}
#endif
#endif
