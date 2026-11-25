/* libFLAC - Free Lossless Audio Codec library
 * Copyright (C) 2000-2025  Xiph.Org Foundation
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

#ifndef FLAC__STREAM_ENCODER_CHECKPOINT_H
#define FLAC__STREAM_ENCODER_CHECKPOINT_H

#include "export.h"
#include "stream_encoder.h"

#ifdef __cplusplus
extern "C" {
#endif


/** \file include/FLAC/stream_encoder_checkpoint.h
 *
 *  \brief
 *  This module contains functions to persist the exact mid-stream state
 *  of a native FLAC stream encoder and to resume encoding into the same
 *  file from that state.
 *
 *  See \link flac_stream_encoder_checkpoint the checkpoint interface \endlink
 *  and the file doc/encoder-checkpoint.md for the on-disk format.
 */

/** \defgroup flac_stream_encoder_checkpoint FLAC/stream_encoder_checkpoint.h: checkpoint interface
 *  \ingroup flac_encoder
 *
 *  \brief
 *  Persist a partially encoded native FLAC stream and resume it later.
 *
 *  A checkpoint captures the encoder state after any successful
 *  FLAC__stream_encoder_process()/..._process_interleaved() call and before
 *  FLAC__stream_encoder_finish().  Saving does not advance the encoder, does
 *  not invoke the write/seek callbacks and does not flush the final partial
 *  block, so the same encoder object can keep going normally afterwards.
 *
 *  Resuming produces a brand new encoder that appends to the already written
 *  stream.  The caller must have positioned the underlying output at the
 *  byte offset recorded in the checkpoint before calling
 *  FLAC__stream_encoder_checkpoint_resume(); the resume call may invoke the
 *  tell callback, but never the write or seek callbacks.  Once the remaining
 *  samples have been fed and the new encoder finished, the resulting file is
 *  byte-for-byte identical to an uninterrupted encoding of the same input.
 *
 *  \{
 */

/** Return the number of bytes needed to store a checkpoint of \a encoder.
 *
 *  This is 0 if the encoder is not in a state that can be checkpointed
 *  (not initialized, already finished, unsupported configuration, or an
 *  internal error).
 */
FLAC_API size_t FLAC__stream_encoder_checkpoint_size(const FLAC__StreamEncoder *encoder);

/** Serialize a checkpoint into \a buffer of \a capacity bytes.
 *
 *  The bytes are written only when the function returns \c true.  It returns
 *  \c false on invalid arguments, an unsupported encoder state, or when
 *  \a capacity is smaller than FLAC__stream_encoder_checkpoint_size(); in
 *  that case \a buffer is left untouched.  Saving is a pure read of the
 *  encoder: the encoder is not advanced and no callbacks are invoked.
 *
 *  Saving the same live encoder twice produces identical bytes.
 */
FLAC_API FLAC__bool FLAC__stream_encoder_checkpoint_write(const FLAC__StreamEncoder *encoder, FLAC__byte *buffer, size_t capacity);

/** Create a new encoder that continues the stream described by the
 *  checkpoint in \a buffer.
 *
 *  \a expected_offset must match both the byte offset stored in the
 *  checkpoint and the current position reported by \a tell_callback through
 *  the caller's output.  The caller must have opened the previous output at
 *  that position before calling this function.
 *
 *  Returns a new encoder on success, or NULL if the checkpoint is invalid,
 *  truncated, of an unknown version, internally inconsistent, or if the
 *  offsets do not agree.  On failure nothing is written to the output.
 *  On success the caller owns the returned encoder and must delete it when
 *  done; the encoder is independent of both the source encoder and
 *  \a buffer.
 */
FLAC_API FLAC__StreamEncoder *FLAC__stream_encoder_checkpoint_resume(
	const FLAC__byte *buffer,
	size_t size,
	FLAC__uint64 expected_offset,
	FLAC__StreamEncoderWriteCallback write_callback,
	FLAC__StreamEncoderSeekCallback seek_callback,
	FLAC__StreamEncoderTellCallback tell_callback,
	FLAC__StreamEncoderMetadataCallback metadata_callback,
	void *client_data
);

/** \} */

#ifdef __cplusplus
}
#endif

#endif /* FLAC__STREAM_ENCODER_CHECKPOINT_H */
