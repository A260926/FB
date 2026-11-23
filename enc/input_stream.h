/*
Copyright (c) 2015, Cisco Systems
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#if !defined(_INPUT_STREAM_H_)
#define _INPUT_STREAM_H_

#include <stdio.h>

/*
 * This header is force-included into every translation unit (encoder and
 * decoder alike, including the high-bitdepth template re-instantiations) by
 * the task build, so it must stay free of includes that define SAMPLE or
 * encoder-only declarations.  Only named struct tags and forward
 * declarations are used here; the full definitions live in types.h
 * (yuv_frame_s/yuv_frame_t) and mainenc.h (enc_params_s/enc_params).
 */
struct enc_params_s;
struct yuv_frame_s;

/*
 * Streaming, non-seekable picture input.
 *
 * The stream is consumed strictly in display order through a single FILE
 * handle: configuration parsing never opens the input a second time, no
 * temporary files or whole-sequence buffers are used, and the producer is
 * never expected to signal end-of-stream before the requested number of
 * pictures has been encoded.
 *
 * Raw input is 8-bit I420 whose geometry and frame rate come from the
 * command line.  Streams beginning with the YUV4MPEG2 magic take their
 * width, height, frame rate and pixel format from the header.  Progressive
 * C420jpeg, C420 and little-endian C420p10 are accepted; X header tags are
 * ignored.  Per-frame markers are "FRAME" followed by an arbitrary,
 * newline-terminated tag.
 */

/* Parsing limits: header lines and frame marker lines. */
#define STREAM_PARSE_MAX_LINE 4096
/* Only the two line parsers below hold buffers of this size. */
#define STREAM_PARSE_BUF_SIZE 4096

typedef enum {
  STREAM_RAW_I420 = 0,
  STREAM_Y4M_420_8 = 1,
  STREAM_Y4M_420_10 = 2
} stream_input_format_t;

typedef struct stream_input_s stream_input_t;

/*
 * Open the input described by params->infilestr ("-" means stdin) and, for a
 * Y4M stream, finish parsing the header and overwrite the geometry, frame
 * rate, subsample and bitdepth fields of params before any picture storage is
 * allocated.  Returns NULL (after printing a diagnostic) on any malformed or
 * unsupported input.  The returned object owns the FILE handle.
 */
stream_input_t *stream_input_open(struct enc_params_s *params);

/*
 * Provide the next complete display-order picture into frame (which must have
 * been created with the geometry and bitdepth reported by the open call).
 * Handles arbitrarily fragmented arrivals, Y4M frame markers and -skip.
 *
 * Returns:
 *   1  a complete picture was delivered,
 *   0  a clean end-of-stream was hit on a complete picture boundary
 *      (or the -n limit was reached),
 *  -1  a truncated header/marker/picture plane was detected, or a read or
 *      allocation failed; a diagnostic is printed.
 *
 * Pictures skipped with -skip are consumed but never delivered.  The -n
 * limit counts delivered pictures only; once it is reached the producer is
 * not drained.
 */
int stream_input_next_picture(stream_input_t *st, struct yuv_frame_s *frame);

/* Number of complete display-order pictures consumed, including skipped ones. */
int stream_input_pictures_consumed(const stream_input_t *st);

/* Number of complete pictures delivered to the encoder (excluding -skip). */
int stream_input_pictures_delivered(const stream_input_t *st);

/* Nonzero once a clean producer EOF has been observed. */
int stream_input_at_eof(const stream_input_t *st);

/* Input geometry and pixel format resolved during open. */
int stream_input_width(const stream_input_t *st);
int stream_input_height(const stream_input_t *st);
int stream_input_input_bitdepth(const stream_input_t *st);
int stream_input_subsample(const stream_input_t *st);
stream_input_format_t stream_input_format(const stream_input_t *st);

/* Close the input and release every owned buffer. */
void stream_input_close(stream_input_t *st);

#endif /* _INPUT_STREAM_H_ */
