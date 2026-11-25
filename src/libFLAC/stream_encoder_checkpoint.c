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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include "FLAC/format.h"
#include "FLAC/metadata.h"
#include "FLAC/stream_encoder.h"
#include "FLAC/stream_encoder_checkpoint.h"
#include "private/bitwriter.h"
#include "private/md5.h"
#include "private/stream_encoder_private.h"

/*
 * On-disk layout.  All integers are unsigned little endian, fixed width.
 * The format is documented in doc/encoder-checkpoint.md.  Any change here
 * must be reflected there and must bump FLAC__CHECKPOINT_VERSION.
 */
#define FLAC__CHECKPOINT_MAGIC           "FLACCKPT"
#define FLAC__CHECKPOINT_MAGIC_LEN       8
#define FLAC__CHECKPOINT_VERSION         1u
#define FLAC__CHECKPOINT_MAX_SEEKPOINTS  64u
#define FLAC__CHECKPOINT_FLAG_MD5        0x1u
#define FLAC__CHECKPOINT_MD5_BLOCK       64u

/***********************************************************************
 *
 * Bounded reader/writer helpers
 *
 ***********************************************************************/

typedef struct {
	FLAC__byte *buf;
	size_t capacity;
	size_t pos;
	FLAC__bool overflow;
} checkpoint_writer;

typedef struct {
	const FLAC__byte *buf;
	size_t size;
	size_t pos;
	FLAC__bool underflow;
} checkpoint_reader;

static void writer_put(checkpoint_writer *w, const void *src, size_t n)
{
	if(w->overflow || n > w->capacity - w->pos) {
		w->overflow = true;
		return;
	}
	if(n)
		memcpy(w->buf + w->pos, src, n);
	w->pos += n;
}

static void writer_u32(checkpoint_writer *w, FLAC__uint32 v)
{
	FLAC__byte b[4];
	b[0] = (FLAC__byte)(v & 0xff);
	b[1] = (FLAC__byte)((v >> 8) & 0xff);
	b[2] = (FLAC__byte)((v >> 16) & 0xff);
	b[3] = (FLAC__byte)((v >> 24) & 0xff);
	writer_put(w, b, 4);
}

static void writer_u64(checkpoint_writer *w, FLAC__uint64 v)
{
	FLAC__byte b[8];
	unsigned i;
	for(i = 0; i < 8; i++)
		b[i] = (FLAC__byte)((v >> (8*i)) & 0xff);
	writer_put(w, b, 8);
}

static FLAC__bool reader_take(checkpoint_reader *r, void *dst, size_t n)
{
	if(r->underflow || n > r->size - r->pos) {
		r->underflow = true;
		return false;
	}
	if(dst && n)
		memcpy(dst, r->buf + r->pos, n);
	r->pos += n;
	return true;
}

static FLAC__uint32 reader_u32(checkpoint_reader *r)
{
	FLAC__byte b[4] = {0, 0, 0, 0};
	(void)reader_take(r, b, 4);
	return (FLAC__uint32)b[0] | ((FLAC__uint32)b[1] << 8) |
	       ((FLAC__uint32)b[2] << 16) | ((FLAC__uint32)b[3] << 24);
}

static FLAC__uint64 reader_u64(checkpoint_reader *r)
{
	FLAC__byte b[8];
	FLAC__uint64 v = 0;
	unsigned i;
	if(!reader_take(r, b, 8))
		return 0;
	for(i = 0; i < 8; i++)
		v |= ((FLAC__uint64)b[i]) << (8*i);
	return v;
}

/* CRC-32 (IEEE 802.3, reflected).  Used only to reject corrupted or
 * accidentally mismatched checkpoint buffers. */
static FLAC__uint32 checkpoint_crc32_(const FLAC__byte *data, size_t len)
{
	FLAC__uint32 crc = 0xffffffffu;
	size_t i;
	unsigned bit;
	for(i = 0; i < len; i++) {
		crc ^= data[i];
		for(bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320u & (FLAC__uint32)(-(FLAC__int32)(crc & 1u)));
	}
	return ~crc;
}

/***********************************************************************
 *
 * What can be checkpointed
 *
 ***********************************************************************/

static FLAC__bool checkpoint_supported_(const FLAC__StreamEncoder *encoder)
{
	const FLAC__StreamEncoderProtected *p = encoder->protected_;
	const FLAC__StreamEncoderPrivate *q = encoder->private_;
	uint32_t i;

	if(p == 0 || q == 0)
		return false;
	if(p->state != FLAC__STREAM_ENCODER_OK)
		return false;
	if(q->is_ogg)
		return false;
	/* A resumed encoder may be checkpointed again: its pending PCM and
	 * counters carry over and the file it appends to is unchanged. */
	if(p->num_threads != 1)
		return false;
	if(p->verify || !p->do_md5 || !p->streamable_subset)
		return false;
	/* Compression level 5 with its default machinery and no overrides. */
	if(p->max_lpc_order != 8)
		return false;
#ifndef FLAC__INTEGER_ONLY_LIBRARY
	if(p->num_apodizations != 1 || p->apodizations[0].type != FLAC__APODIZATION_TUKEY ||
	   p->apodizations[0].parameters.tukey.p != 0.5f)
		return false;
#endif
	if(p->do_qlp_coeff_prec_search || p->do_escape_coding || p->do_exhaustive_model_search)
		return false;
	if(p->min_residual_partition_order != 0 || p->max_residual_partition_order != 5)
		return false;
	if(p->rice_parameter_search_dist != 0 || p->limit_min_bitrate)
		return false;
	if(q->disable_mmx || q->disable_sse2 || q->disable_ssse3 || q->disable_sse41 ||
	   q->disable_sse42 || q->disable_avx2 || q->disable_fma)
		return false;
	if(q->disable_constant_subframes || q->disable_fixed_subframes || q->disable_verbatim_subframes)
		return false;
	/* Channel layout. */
	if(p->channels < 1 || p->channels > 2)
		return false;
	if(p->do_mid_side_stereo != (p->channels == 2))
		return false;
	if(p->loose_mid_side_stereo)
		return false;
	if(p->bits_per_sample != 16 && p->bits_per_sample != 24)
		return false;
	if(p->sample_rate != 44100 && p->sample_rate != 48000)
		return false;
	if(p->blocksize != 256 && p->blocksize != 1024 && p->blocksize != 4096)
		return false;
	/* Metadata: nothing, or exactly one SEEKTABLE with at most 64 points. */
	if(p->num_metadata_blocks > 1)
		return false;
	if(p->num_metadata_blocks == 1) {
		const FLAC__StreamMetadata *m = p->metadata ? p->metadata[0] : 0;
		if(m == 0 || m->type != FLAC__METADATA_TYPE_SEEKTABLE)
			return false;
		if(m->data.seek_table.num_points > FLAC__CHECKPOINT_MAX_SEEKPOINTS)
			return false;
	}
	/* The seek table the encoder records into must be the one we serialize. */
	if(q->seek_table) {
		FLAC__bool found = false;
		uint32_t k;
		for(k = 0; k < p->num_metadata_blocks && !found; k++)
			if(p->metadata[k] && &p->metadata[k]->data.seek_table == q->seek_table)
				found = true;
		if(!found)
			return false;
	}
	/* No partially encoded frame may be sitting in the bitwriter. */
	if(q->threadtask[0] == 0 || q->threadtask[0]->frame == 0)
		return false;
	if(!FLAC__bitwriter_is_byte_aligned(q->threadtask[0]->frame) ||
	   FLAC__bitwriter_get_input_bits_unconsumed(q->threadtask[0]->frame) != 0)
		return false;
	if(q->write_callback == 0 || q->tell_callback == 0)
		return false;
	if(q->current_sample_number > p->blocksize)
		return false;
	for(i = 0; i < p->channels; i++)
		if(q->threadtask[0]->integer_signal[i] == 0)
			return false;
	return true;
}

static uint32_t checkpoint_seekpoint_count_(const FLAC__StreamEncoder *encoder)
{
	const FLAC__StreamEncoderPrivate *q = encoder->private_;
	return q->seek_table ? q->seek_table->num_points : 0;
}

/* Fixed portion of the format, in bytes, excluding the CRC, the seekpoint
 * array and the pending PCM.  Keep in sync with checkpoint_write_. */
#define CHECKPOINT_FIXED_BYTES ( \
	FLAC__CHECKPOINT_MAGIC_LEN + /* magic */ \
	4 +  /* version */ \
	4 +  /* total_size */ \
	4 +  /* flags */ \
	4 +  /* channels */ \
	4 +  /* bits_per_sample */ \
	4 +  /* sample_rate */ \
	4 +  /* blocksize */ \
	4 +  /* streaminfo min_blocksize */ \
	4 +  /* streaminfo max_blocksize */ \
	4 +  /* streaminfo min_framesize */ \
	4 +  /* streaminfo max_framesize */ \
	8 +  /* total_samples_estimate */ \
	8 +  /* bytes_written */ \
	8 +  /* samples_written */ \
	4 +  /* frames_written */ \
	4 +  /* first_seekpoint_to_check */ \
	4 +  /* current_sample_number */ \
	4 +  /* current_frame_number */ \
	8 +  /* streaminfo_offset */ \
	8 +  /* seektable_offset */ \
	8 +  /* audio_offset */ \
	8 +  /* streaminfo total_samples */ \
	4 +  /* streaminfo min_framesize */ \
	4 +  /* streaminfo max_framesize */ \
	16 + /* streaminfo md5 */ \
	4 +  /* md5 bytes[0] */ \
	4 +  /* md5 bytes[1] */ \
	4 +  /* md5 block size */ \
	4 +  /* md5 reserved */ \
	16 + /* md5 buf[4] */ \
	64 + /* md5 in[16] raw bytes */ \
	4 +  /* num_seekpoints */ \
	8 +  /* pending_bytes */ \
	8 +  /* pending per channel 0 */ \
	8 +  /* pending per channel 1 */ \
	8 +  /* pending md5 bytes */ \
	4 +  /* bitwriter pending bytes */ \
	4 +  /* reserved */ \
	4    /* crc32 (covers all preceding bytes, placed last) */ \
)

static size_t checkpoint_serialized_size_(const FLAC__StreamEncoder *encoder)
{
	const FLAC__StreamEncoderProtected *p = encoder->protected_;
	uint32_t seekpoints = checkpoint_seekpoint_count_(encoder);
	size_t pending = (size_t)encoder->private_->current_sample_number * p->channels * 4u;
	return (size_t)CHECKPOINT_FIXED_BYTES + (size_t)seekpoints * 20u + pending;
}

FLAC_API size_t FLAC__stream_encoder_checkpoint_size(const FLAC__StreamEncoder *encoder)
{
	if(encoder == 0 || encoder->protected_ == 0 || encoder->private_ == 0)
		return 0;
	if(!checkpoint_supported_(encoder))
		return 0;
	return checkpoint_serialized_size_(encoder);
}

FLAC_API FLAC__bool FLAC__stream_encoder_checkpoint_write(const FLAC__StreamEncoder *encoder, FLAC__byte *buffer, size_t capacity)
{
	const FLAC__StreamEncoderProtected *p;
	const FLAC__StreamEncoderPrivate *q;
	checkpoint_writer w;
	FLAC__uint64 pos = 0;
	FLAC__StreamEncoderTellStatus tell_status;
	uint32_t seekpoints, pending, seekpoint;
	uint32_t buffered, channel, i;
	size_t expected;
	FLAC__uint32 crc;

	if(encoder == 0 || buffer == 0)
		return false;
	if(!checkpoint_supported_(encoder))
		return false;
	if(encoder->private_->tell_callback == 0)
		return false;

	p = encoder->protected_;
	q = encoder->private_;
	expected = checkpoint_serialized_size_(encoder);
	if(capacity < expected)
		return false;

	/* Saving must not call write/seek and must not advance the encoder.
	 * tell is allowed so the caller can learn the current position. */
	tell_status = q->tell_callback(encoder, &pos, q->client_data);
	if(tell_status != FLAC__STREAM_ENCODER_TELL_STATUS_OK)
		return false;

	seekpoints = checkpoint_seekpoint_count_(encoder);
	pending = q->current_sample_number;

	w.buf = buffer;
	w.capacity = capacity;
	w.pos = 0;
	w.overflow = false;

	writer_put(&w, FLAC__CHECKPOINT_MAGIC, FLAC__CHECKPOINT_MAGIC_LEN);
	writer_u32(&w, FLAC__CHECKPOINT_VERSION);
	writer_u32(&w, (FLAC__uint32)expected);
	writer_u32(&w, FLAC__CHECKPOINT_FLAG_MD5);
	writer_u32(&w, p->channels);
	writer_u32(&w, p->bits_per_sample);
	writer_u32(&w, p->sample_rate);
	writer_u32(&w, p->blocksize);
	writer_u32(&w, q->streaminfo.data.stream_info.min_blocksize);
	writer_u32(&w, q->streaminfo.data.stream_info.max_blocksize);
	writer_u32(&w, q->streaminfo.data.stream_info.min_framesize);
	writer_u32(&w, q->streaminfo.data.stream_info.max_framesize);
	writer_u64(&w, p->total_samples_estimate);
	writer_u64(&w, q->bytes_written);
	writer_u64(&w, q->samples_written);
	writer_u32(&w, q->frames_written);
	writer_u32(&w, q->first_seekpoint_to_check);
	writer_u32(&w, q->current_sample_number);
	writer_u32(&w, q->current_frame_number);
	writer_u64(&w, p->streaminfo_offset);
	writer_u64(&w, p->seektable_offset);
	writer_u64(&w, p->audio_offset);
	writer_u64(&w, q->streaminfo.data.stream_info.total_samples);
	writer_u32(&w, q->streaminfo.data.stream_info.min_framesize);
	writer_u32(&w, q->streaminfo.data.stream_info.max_framesize);
	writer_put(&w, q->streaminfo.data.stream_info.md5sum, 16);
	writer_u32(&w, q->md5context.bytes[0]);
	writer_u32(&w, q->md5context.bytes[1]);
	writer_u32(&w, FLAC__CHECKPOINT_MD5_BLOCK);
	writer_u32(&w, 0);
	for(i = 0; i < 4; i++)
		writer_u32(&w, q->md5context.buf[i]);
	{
		/* Serialize the MD5 input buffer as raw bytes.  Only the first
		 * (bytes[0] & 63) bytes are live; canonicalize the rest to zero so
		 * repeated saves are byte-identical. */
		FLAC__byte in[FLAC__CHECKPOINT_MD5_BLOCK];
		buffered = q->md5context.bytes[0] & 0x3fu;
		memset(in, 0, sizeof(in));
		memcpy(in, (const FLAC__byte *)q->md5context.in, buffered);
		writer_put(&w, in, sizeof(in));
	}
	writer_u32(&w, seekpoints);
	for(seekpoint = 0; seekpoint < seekpoints; seekpoint++) {
		const FLAC__StreamMetadata_SeekPoint *pt = &q->seek_table->points[seekpoint];
		writer_u64(&w, pt->sample_number);
		writer_u64(&w, pt->stream_offset);
		writer_u32(&w, pt->frame_samples);
	}
	writer_u64(&w, (FLAC__uint64)pending * p->channels * 4u);
	writer_u64(&w, pending);
	writer_u64(&w, p->channels == 2 ? pending : 0);
	writer_u64(&w, (FLAC__uint64)pending * p->channels * ((p->bits_per_sample + 7u) / 8u));
	writer_u32(&w, 0); /* bitwriter pending bytes */
	writer_u32(&w, 0); /* reserved */

	/* Uncommitted PCM exactly as buffered by the encoder: channel-major,
	 * one native 32-bit sample per entry. */
	for(channel = 0; channel < p->channels; channel++) {
		for(i = 0; i < pending; i++)
			writer_u32(&w, (FLAC__uint32)q->threadtask[0]->integer_signal[channel][i]);
	}

	if(w.overflow || w.pos != expected - 4)
		return false;

	/* The CRC-32 sits at the very end and covers everything before it. */
	crc = checkpoint_crc32_(buffer, w.pos);
	writer_u32(&w, crc);

	if(w.overflow || w.pos != expected)
		return false;

	return true;
}

/***********************************************************************
 *
 * Deserialization / resume
 *
 ***********************************************************************/

typedef struct {
	FLAC__uint32 channels;
	FLAC__uint32 bits_per_sample;
	FLAC__uint32 sample_rate;
	FLAC__uint32 blocksize;
	FLAC__uint32 streaminfo_min_blocksize;
	FLAC__uint32 streaminfo_max_blocksize;
	FLAC__uint32 streaminfo_min_framesize_u64_dummy; /* unused, keeps layout explicit */
	FLAC__uint32 streaminfo_max_framesize_u64_dummy;
	FLAC__uint64 total_samples_estimate;
	FLAC__uint64 bytes_written;
	FLAC__uint64 samples_written;
	FLAC__uint32 frames_written;
	FLAC__uint32 first_seekpoint_to_check;
	FLAC__uint32 current_sample_number;
	FLAC__uint32 current_frame_number;
	FLAC__uint64 streaminfo_offset;
	FLAC__uint64 seektable_offset;
	FLAC__uint64 audio_offset;
	FLAC__uint64 streaminfo_total_samples;
	FLAC__uint32 streaminfo_min_framesize;
	FLAC__uint32 streaminfo_max_framesize;
	FLAC__byte streaminfo_md5[16];
	FLAC__uint32 md5_bytes[2];
	FLAC__uint32 md5_buf[4];
	FLAC__byte md5_in[FLAC__CHECKPOINT_MD5_BLOCK];
	FLAC__uint32 num_seekpoints;
	FLAC__uint64 pending_bytes;
	FLAC__uint64 pending_channel0;
	FLAC__uint64 pending_channel1;
	FLAC__uint64 pending_md5_bytes;
	FLAC__uint32 bitwriter_bytes;
	size_t seekpoints_offset;
	size_t pcm_offset;
} checkpoint_fields;

static FLAC__bool checkpoint_parse_(const FLAC__byte *buffer, size_t size, checkpoint_fields *f)
{
	checkpoint_reader r;
	FLAC__byte magic[FLAC__CHECKPOINT_MAGIC_LEN];
	FLAC__uint32 version, total_size, flags, md5_block, md5_reserved, i;
	FLAC__uint32 stored_crc, computed_crc, reserved;

	r.buf = buffer;
	r.size = size;
	r.pos = 0;
	r.underflow = false;

	if(size < FLAC__CHECKPOINT_MAGIC_LEN + 12)
		return false;
	(void)reader_take(&r, magic, FLAC__CHECKPOINT_MAGIC_LEN);
	if(memcmp(magic, FLAC__CHECKPOINT_MAGIC, FLAC__CHECKPOINT_MAGIC_LEN) != 0)
		return false;
	version = reader_u32(&r);
	if(version != FLAC__CHECKPOINT_VERSION)
		return false;
	total_size = reader_u32(&r);
	if(total_size != size)
		return false;
	flags = reader_u32(&r);
	f->channels = reader_u32(&r);
	f->bits_per_sample = reader_u32(&r);
	f->sample_rate = reader_u32(&r);
	f->blocksize = reader_u32(&r);
	f->streaminfo_min_blocksize = reader_u32(&r);
	f->streaminfo_max_blocksize = reader_u32(&r);
	f->streaminfo_min_framesize_u64_dummy = reader_u32(&r);
	f->streaminfo_max_framesize_u64_dummy = reader_u32(&r);
	f->total_samples_estimate = reader_u64(&r);
	f->bytes_written = reader_u64(&r);
	f->samples_written = reader_u64(&r);
	f->frames_written = reader_u32(&r);
	f->first_seekpoint_to_check = reader_u32(&r);
	f->current_sample_number = reader_u32(&r);
	f->current_frame_number = reader_u32(&r);
	f->streaminfo_offset = reader_u64(&r);
	f->seektable_offset = reader_u64(&r);
	f->audio_offset = reader_u64(&r);
	f->streaminfo_total_samples = reader_u64(&r);
	f->streaminfo_min_framesize = reader_u32(&r);
	f->streaminfo_max_framesize = reader_u32(&r);
	(void)reader_take(&r, f->streaminfo_md5, 16);
	f->md5_bytes[0] = reader_u32(&r);
	f->md5_bytes[1] = reader_u32(&r);
	md5_block = reader_u32(&r);
	md5_reserved = reader_u32(&r);
	for(i = 0; i < 4; i++)
		f->md5_buf[i] = reader_u32(&r);
	(void)reader_take(&r, f->md5_in, FLAC__CHECKPOINT_MD5_BLOCK);
	f->num_seekpoints = reader_u32(&r);
	f->seekpoints_offset = r.pos;
	if(r.underflow)
		return false;
	if(f->num_seekpoints > FLAC__CHECKPOINT_MAX_SEEKPOINTS)
		return false;
	if(!reader_take(&r, 0, (size_t)f->num_seekpoints * 20u))
		return false;
	f->pending_bytes = reader_u64(&r);
	f->pending_channel0 = reader_u64(&r);
	f->pending_channel1 = reader_u64(&r);
	f->pending_md5_bytes = reader_u64(&r);
	f->bitwriter_bytes = reader_u32(&r);
	reserved = reader_u32(&r);
	if(r.underflow)
		return false;

	/* Reject unknown flags and self-contradictory fixed fields. */
	if(flags != FLAC__CHECKPOINT_FLAG_MD5)
		return false;
	if(md5_block != FLAC__CHECKPOINT_MD5_BLOCK || md5_reserved != 0 || reserved != 0)
		return false;
	if(f->channels < 1 || f->channels > 2)
		return false;
	if(f->bits_per_sample != 16 && f->bits_per_sample != 24)
		return false;
	if(f->sample_rate != 44100 && f->sample_rate != 48000)
		return false;
	if(f->blocksize != 256 && f->blocksize != 1024 && f->blocksize != 4096)
		return false;
	if(f->streaminfo_min_blocksize != f->blocksize || f->streaminfo_max_blocksize != f->blocksize)
		return false;
	if(f->streaminfo_min_framesize_u64_dummy != f->streaminfo_min_framesize ||
	   f->streaminfo_max_framesize_u64_dummy != f->streaminfo_max_framesize)
		return false;
	if(f->streaminfo_min_framesize > 0xffffffu || f->streaminfo_max_framesize > 0xffffffu)
		return false;
	if(f->current_sample_number > f->blocksize)
		return false;
	if(f->bitwriter_bytes != 0)
		return false;
	if(f->pending_channel0 != f->current_sample_number)
		return false;
	if(f->channels == 1) {
		if(f->pending_channel1 != 0)
			return false;
	}
	else if(f->pending_channel1 != f->current_sample_number) {
		return false;
	}
	if(f->pending_bytes != (FLAC__uint64)f->current_sample_number * f->channels * 4u)
		return false;
	if(f->pending_md5_bytes != (FLAC__uint64)f->current_sample_number * f->channels * ((f->bits_per_sample + 7u) / 8u))
		return false;
	if(f->samples_written != (FLAC__uint64)f->current_frame_number * f->blocksize)
		return false;
	if(f->streaminfo_total_samples != f->samples_written)
		return false;
	if(f->frames_written < f->current_frame_number)
		return false;
	if(f->first_seekpoint_to_check > f->num_seekpoints)
		return false;
	/* Seek points must be legal in their own right. */
	if(f->num_seekpoints > 0) {
		checkpoint_reader sr;
		FLAC__uint64 prev = 0;
		FLAC__bool got_prev = false;
		sr.buf = buffer;
		sr.size = size;
		sr.pos = f->seekpoints_offset;
		sr.underflow = false;
		for(i = 0; i < f->num_seekpoints; i++) {
			FLAC__uint64 sample_number = reader_u64(&sr);
			(void)reader_u64(&sr); /* stream_offset */
			(void)reader_u32(&sr); /* frame_samples */
			if(sr.underflow)
				return false;
			if(got_prev && sample_number != FLAC__STREAM_METADATA_SEEKPOINT_PLACEHOLDER &&
			   sample_number <= prev)
				return false;
			prev = sample_number;
			got_prev = true;
		}
	}
	/* All bytes must be accounted for: fixed tail + seekpoints + PCM + CRC. */
	f->pcm_offset = r.pos;
	if((FLAC__uint64)f->pcm_offset + f->pending_bytes + 4u != (FLAC__uint64)size)
		return false;
	/* The CRC-32 covers everything, including the PCM. */
	stored_crc = (FLAC__uint32)buffer[size - 4] |
	             ((FLAC__uint32)buffer[size - 3] << 8) |
	             ((FLAC__uint32)buffer[size - 2] << 16) |
	             ((FLAC__uint32)buffer[size - 1] << 24);
	computed_crc = checkpoint_crc32_(buffer, size - 4);
	if(stored_crc != computed_crc)
		return false;
	return true;
}

static void checkpoint_restore_md5_(FLAC__MD5Context *ctx, const checkpoint_fields *f)
{
	uint32_t i;
	ctx->bytes[0] = f->md5_bytes[0];
	ctx->bytes[1] = f->md5_bytes[1];
	for(i = 0; i < 4; i++)
		ctx->buf[i] = f->md5_buf[i];
	memcpy((FLAC__byte *)ctx->in, f->md5_in, FLAC__CHECKPOINT_MD5_BLOCK);
	ctx->internal_buf.p8 = 0;
	ctx->capacity = 0;
}

FLAC_API FLAC__StreamEncoder *FLAC__stream_encoder_checkpoint_resume(
	const FLAC__byte *buffer,
	size_t size,
	FLAC__uint64 expected_offset,
	FLAC__StreamEncoderWriteCallback write_callback,
	FLAC__StreamEncoderSeekCallback seek_callback,
	FLAC__StreamEncoderTellCallback tell_callback,
	FLAC__StreamEncoderMetadataCallback metadata_callback,
	void *client_data
)
{
	checkpoint_fields f;
	FLAC__StreamEncoder *encoder = 0;
	FLAC__StreamEncoderCheckpointResume resume;
	FLAC__StreamMetadata *seektable = 0;
	FLAC__uint64 tell_pos = 0;
	FLAC__StreamEncoderTellStatus tell_status;
	uint32_t channel, i;
	size_t count;
	memset(&f, 0, sizeof(f));
	if(buffer == 0 || write_callback == 0 || tell_callback == 0)
		return 0;
	if(!checkpoint_parse_(buffer, size, &f))
		return 0;

	/* The caller's stated position must match both the checkpoint and the
	 * observed tell position. */
	if(expected_offset != f.bytes_written)
		return 0;

	encoder = FLAC__stream_encoder_new();
	if(encoder == 0)
		return 0;

	if(!FLAC__stream_encoder_set_channels(encoder, f.channels) ||
	   !FLAC__stream_encoder_set_bits_per_sample(encoder, f.bits_per_sample) ||
	   !FLAC__stream_encoder_set_sample_rate(encoder, f.sample_rate) ||
	   !FLAC__stream_encoder_set_blocksize(encoder, f.blocksize) ||
	   !FLAC__stream_encoder_set_verify(encoder, false) ||
	   !FLAC__stream_encoder_set_streamable_subset(encoder, true) ||
	   !FLAC__stream_encoder_set_compression_level(encoder, 5)) {
		FLAC__stream_encoder_delete(encoder);
		return 0;
	}
	encoder->protected_->do_md5 = true;
	encoder->protected_->total_samples_estimate = f.total_samples_estimate;

	/* Rebuild the SEEKTABLE the previous encoder had been filling in. */
	if(f.num_seekpoints > 0) {
		FLAC__StreamMetadata **list;
		checkpoint_reader sr;

		seektable = FLAC__metadata_object_new(FLAC__METADATA_TYPE_SEEKTABLE);
		if(seektable == 0) {
			FLAC__stream_encoder_delete(encoder);
			return 0;
		}
		if(!FLAC__metadata_object_seektable_resize_points(seektable, f.num_seekpoints)) {
			FLAC__metadata_object_delete(seektable);
			FLAC__stream_encoder_delete(encoder);
			return 0;
		}
		sr.buf = buffer;
		sr.size = size;
		sr.pos = f.seekpoints_offset;
		sr.underflow = false;
		for(i = 0; i < f.num_seekpoints; i++) {
			seektable->data.seek_table.points[i].sample_number = reader_u64(&sr);
			seektable->data.seek_table.points[i].stream_offset = reader_u64(&sr);
			seektable->data.seek_table.points[i].frame_samples = reader_u32(&sr);
		}
		if(sr.underflow || !FLAC__format_seektable_is_legal(&seektable->data.seek_table)) {
			FLAC__metadata_object_delete(seektable);
			FLAC__stream_encoder_delete(encoder);
			return 0;
		}
		list = malloc(sizeof(list[0]));
		if(list == 0) {
			FLAC__metadata_object_delete(seektable);
			FLAC__stream_encoder_delete(encoder);
			return 0;
		}
		list[0] = seektable;
		if(!FLAC__stream_encoder_set_metadata(encoder, list, 1)) {
			free(list);
			FLAC__metadata_object_delete(seektable);
			FLAC__stream_encoder_delete(encoder);
			return 0;
		}
		free(list);
	}

	/* Validate the position with tell before anything is written. */
	encoder->private_->tell_callback = tell_callback;
	encoder->private_->client_data = client_data;
	tell_status = tell_callback(encoder, &tell_pos, client_data);
	encoder->private_->tell_callback = 0;
	encoder->private_->client_data = 0;
	if(tell_status != FLAC__STREAM_ENCODER_TELL_STATUS_OK || tell_pos != expected_offset) {
		if(seektable)
			FLAC__metadata_object_delete(seektable);
		FLAC__stream_encoder_delete(encoder);
		return 0;
	}

	resume.streaminfo_offset = f.streaminfo_offset;
	resume.seektable_offset = f.seektable_offset;
	resume.audio_offset = f.audio_offset;

	if(FLAC__stream_encoder_init_stream_checkpoint_(encoder, write_callback, seek_callback,
	                                                 tell_callback, metadata_callback,
	                                                 client_data, &resume) !=
	   FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
		if(seektable)
			FLAC__metadata_object_delete(seektable);
		FLAC__stream_encoder_delete(encoder);
		return 0;
	}

	/* Restore running counters and the STREAMINFO scratchpad. */
	encoder->private_->bytes_written = f.bytes_written;
	encoder->private_->samples_written = f.samples_written;
	encoder->private_->frames_written = f.frames_written;
	encoder->private_->current_frame_number = f.current_frame_number;
	encoder->private_->current_sample_number = f.current_sample_number;
	encoder->private_->first_seekpoint_to_check = f.first_seekpoint_to_check;
	encoder->private_->streaminfo.data.stream_info.total_samples = f.streaminfo_total_samples;
	encoder->private_->streaminfo.data.stream_info.min_framesize = f.streaminfo_min_framesize;
	encoder->private_->streaminfo.data.stream_info.max_framesize = f.streaminfo_max_framesize;
	memcpy(encoder->private_->streaminfo.data.stream_info.md5sum, f.streaminfo_md5, 16);
	checkpoint_restore_md5_(&encoder->private_->md5context, &f);

	if(seektable)
		encoder->private_->checkpoint_owned_seektable = seektable;

	/* Restore the uncommitted PCM. */
	{
		checkpoint_reader pr;
		pr.buf = buffer;
		pr.size = size;
		pr.pos = f.pcm_offset;
		pr.underflow = false;
		count = f.current_sample_number;
		for(channel = 0; channel < f.channels; channel++) {
			for(i = 0; i < count; i++) {
				FLAC__uint32 s = reader_u32(&pr);
				encoder->private_->threadtask[0]->integer_signal[channel][i] = (FLAC__int32)s;
			}
		}
		if(pr.underflow) {
			FLAC__stream_encoder_delete(encoder);
			return 0;
		}
	}

	return encoder;
}
