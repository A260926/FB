/* libFLAC - Free Lossless Audio Codec library
 * Copyright (C) 2025  Xiph.Org Foundation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * - Neither the name of the Xiph.org Foundation nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef FLAC__STREAM_SPLICE_H
#define FLAC__STREAM_SPLICE_H

#include <stddef.h>

#include "export.h"
#include "ordinals.h"

#ifdef __cplusplus
extern "C" {
#endif

/** \file include/FLAC/stream_splice.h
 *
 *  \brief
 *  This module contains functions for losslessly splicing a number of
 *  existing native FLAC streams into a new native FLAC stream.
 *
 *  \ingroup flac
 *
 *  A splice is described by an array of FLAC__SpliceSegment structures.
 *  Each segment refers to one complete input stream (the \a data pointer
 *  and \a bytes length) and selects the half-open range of per-channel
 *  sample frames [begin, end) that should be taken from it.  Segments are
 *  concatenated in the order in which they appear in the array.  A single
 *  input stream may be referenced by more than one segment, and segments
 *  may refer to inputs in any order.
 *
 *  Frames that are completely contained in a segment are copied without
 *  re-encoding: everything between the source frame header CRC-8 and the
 *  frame footer CRC-16 is preserved byte for byte, while the frame header
 *  (which carries the new absolute sample number and the variable blocking
 *  strategy bit) and the frame CRC-16 are written anew.  Only the partial
 *  frames at the edges of a segment are re-encoded, using the library's
 *  existing prediction and channel assignment code.
 *
 *  The input streams must be native FLAC without an ID3 tag, must use
 *  fixed-size blocks, a sample rate of 44100 or 48000 Hz, one or two
 *  channels, and 16 or 24 bits per sample.  All inputs used in a single
 *  call must share the same format.  Every input must have an accurate,
 *  nonzero total sample count; the original streams may use any block size
 *  between 16 and 4096 samples.  A segment must be at least 16 samples
 *  long, and every generated output block is at least 16 samples long.
 *
 *  The output contains exactly one fLaC stream marker and one STREAMINFO
 *  metadata block; no other input metadata (for example a SEEKTABLE) is
 *  carried over.  Its STREAMINFO block records the true total sample
 *  count, the actual minimum and maximum block and frame sizes, and the
 *  MD5 signatures of the spliced PCM.
 *
 * \{
 */

/** Structure describing one segment of a splice.
 *
 *  \a data points to a complete native FLAC stream of \a bytes bytes.
 *  The caller retains ownership of the input buffer; it is not modified
 *  and must remain valid for the duration of FLAC__stream_splice().
 *
 *  \a begin and \a end select the half-open interval [begin, end) of
 *  per-channel samples to take from the stream.  They are counted in
 *  sample frames, are relative to the beginning of the input stream, and
 *  must both lie within its accurate total sample count.  The interval
 *  must be at least 16 sample frames long.
 */
typedef struct {
	const FLAC__byte *data;
	size_t bytes;
	FLAC__uint64 begin;
	FLAC__uint64 end;
} FLAC__SpliceSegment;

/** Splicing function.
 *
 *  \param  segments      An array of \a count segment descriptions.
 *  \param  count         The number of segments in \a segments.  Must be
 *                        at least 1.
 *  \param  output        Pointer to a variable that will receive the
 *                        allocated output buffer on success.
 *  \param  output_bytes  Pointer to a variable that will receive the size
 *                        of the output buffer in bytes on success.
 *  \assert
 *    \code segments != NULL \endcode
 *    \code output != NULL \endcode
 *    \code output_bytes != NULL \endcode
 *  \retval FLAC__bool
 *    \c true on success.  The caller takes ownership of the buffer in
 *    \a *output and must release it with FLAC__stream_splice_free().
 *    On failure, \c false is returned and \a *output and \a *output_bytes
 *    are set to \c NULL and \c 0 respectively; the input buffers are
 *    never modified.
 */
FLAC_API FLAC__bool FLAC__stream_splice(const FLAC__SpliceSegment *segments, size_t count, FLAC__byte **output, size_t *output_bytes);

/** Release a buffer returned by FLAC__stream_splice().
 *
 *  \param  buffer  The buffer to release.  A \c NULL pointer is ignored.
 */
FLAC_API void FLAC__stream_splice_free(FLAC__byte *buffer);

/* \} */

#ifdef __cplusplus
}
#endif

#endif
