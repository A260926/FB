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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "FLAC/stream_splice.h"
#include "FLAC/stream_decoder.h"
#include "FLAC/stream_encoder.h"
#include "FLAC/format.h"
#include "private/bitwriter.h"
#include "private/crc.h"
#include "private/md5.h"
#include "private/stream_encoder_framing.h"
#include "share/alloc.h"
#include "share/compat.h"
#include "share/private.h"

#define SPLICE_MIN_BLOCK 16u
#define SPLICE_MAX_SOURCE_BLOCK 4096u
#define SPLICE_MAX_CHANNELS 2u
#define SPLICE_MAX_HEADER_BYTES 16u


/***********************************************************************
 *
 * Small helpers
 *
 ***********************************************************************/

typedef struct {
	FLAC__byte *data;
	size_t bytes, capacity;
} SpliceBuffer;

static void splice_buffer_clear(SpliceBuffer *b)
{
	if(b->data)
		free(b->data);
	b->data = 0;
	b->bytes = b->capacity = 0;
}

static FLAC__bool splice_buffer_reserve(SpliceBuffer *b, size_t extra)
{
	size_t needed, capacity;
	FLAC__byte *p;

	if(extra > SIZE_MAX - b->bytes)
		return false;
	needed = b->bytes + extra;
	if(needed <= b->capacity)
		return true;
	capacity = b->capacity? b->capacity : 1024;
	while(capacity < needed) {
		if(capacity > SIZE_MAX / 2) {
			capacity = needed;
			break;
		}
		capacity *= 2;
	}
	p = (FLAC__byte*)realloc(b->data, capacity);
	if(!p)
		return false;
	b->data = p;
	b->capacity = capacity;
	return true;
}

static FLAC__bool splice_buffer_append(SpliceBuffer *b, const void *data, size_t bytes)
{
	if(!splice_buffer_reserve(b, bytes))
		return false;
	if(bytes)
		memcpy(b->data + b->bytes, data, bytes);
	b->bytes += bytes;
	return true;
}

static FLAC__uint64 splice_get_bits(const FLAC__byte *data, uint32_t bitpos, uint32_t bits)
{
	FLAC__uint64 value = 0;
	uint32_t i;
	for(i = 0; i < bits; i++) {
		uint32_t pos = bitpos + i;
		value = (value << 1) | ((data[pos >> 3] >> (7 - (pos & 7))) & 1);
	}
	return value;
}

/* Read a FLAC UTF-8 coded number starting at *pos.  Returns false on an
 * invalid or truncated sequence.  The encoding accepted here mirrors the
 * one accepted by the stream decoder. */
static FLAC__bool splice_read_utf8(const FLAC__byte *data, size_t avail, size_t *pos, FLAC__uint64 *value)
{
	FLAC__uint64 v = 0;
	FLAC__uint32 x;
	uint32_t i, count;

	if(*pos >= avail)
		return false;
	x = data[(*pos)++];
	if(!(x & 0x80)) {
		v = x;
		count = 0;
	}
	else if((x & 0xE0) == 0xC0) {
		v = x & 0x1F;
		count = 1;
	}
	else if((x & 0xF0) == 0xE0) {
		v = x & 0x0F;
		count = 2;
	}
	else if((x & 0xF8) == 0xF0) {
		v = x & 0x07;
		count = 3;
	}
	else if((x & 0xFC) == 0xF8) {
		v = x & 0x03;
		count = 4;
	}
	else if((x & 0xFE) == 0xFC) {
		v = x & 0x01;
		count = 5;
	}
	else if(x == 0xFE) {
		v = 0;
		count = 6;
	}
	else
		return false;
	for(i = 0; i < count; i++) {
		if(*pos >= avail)
			return false;
		x = data[(*pos)++];
		if(!(x & 0x80) || (x & 0x40))
			return false;
		v = (v << 6) | (x & 0x3F);
	}
	*value = v;
	return true;
}

/***********************************************************************
 *
 * Frame header parsing
 *
 ***********************************************************************/

typedef struct {
	uint32_t blocksize;
	uint32_t sample_rate; /* 0 if taken from STREAMINFO */
	uint32_t channels;
	uint32_t bits_per_sample; /* 0 if taken from STREAMINFO */
	FLAC__ChannelAssignment channel_assignment;
	FLAC__bool variable_blocking;
	FLAC__uint64 number;
	uint32_t header_bytes;
} SpliceHeaderInfo;

static FLAC__bool splice_parse_frame_header(const FLAC__byte *data, size_t avail, SpliceHeaderInfo *info)
{
	uint32_t blocksize_code, sample_rate_code, channel_code, bps_code;
	FLAC__uint64 number = 0;
	size_t pos;

	memset(info, 0, sizeof(*info));
	if(avail < 6)
		return false;
	if(data[0] != 0xFF || (data[1] & 0xFE) != 0xF8 || (data[1] & 0x02))
		return false;
	info->variable_blocking = (data[1] & 0x01)? true : false;
	blocksize_code = data[2] >> 4;
	sample_rate_code = data[2] & 0x0F;
	channel_code = data[3] >> 4;
	bps_code = (data[3] >> 1) & 0x07;
	if(data[3] & 0x01)
		return false;
	if(blocksize_code == 0 || sample_rate_code == 15 || channel_code > 10 || bps_code == 3)
		return false;

	pos = 4;
	if(!splice_read_utf8(data, avail, &pos, &number))
		return false;
	info->number = number;

	switch(blocksize_code) {
		case 1: info->blocksize = 192; break;
		case 2: info->blocksize = 576; break;
		case 3: info->blocksize = 1152; break;
		case 4: info->blocksize = 2304; break;
		case 5: info->blocksize = 4608; break;
		case 6:
			if(pos + 1 > avail) return false;
			info->blocksize = data[pos] + 1;
			pos += 1;
			break;
		case 7:
			if(pos + 2 > avail) return false;
			info->blocksize = (((uint32_t)data[pos] << 8) | data[pos+1]) + 1;
			pos += 2;
			break;
		case 8: info->blocksize = 256; break;
		case 9: info->blocksize = 512; break;
		case 10: info->blocksize = 1024; break;
		case 11: info->blocksize = 2048; break;
		case 12: info->blocksize = 4096; break;
		case 13: info->blocksize = 8192; break;
		case 14: info->blocksize = 16384; break;
		case 15: info->blocksize = 32768; break;
		default: return false;
	}

	switch(sample_rate_code) {
		case 0: info->sample_rate = 0; break;
		case 1: info->sample_rate = 88200; break;
		case 2: info->sample_rate = 176400; break;
		case 3: info->sample_rate = 192000; break;
		case 4: info->sample_rate = 8000; break;
		case 5: info->sample_rate = 16000; break;
		case 6: info->sample_rate = 22050; break;
		case 7: info->sample_rate = 24000; break;
		case 8: info->sample_rate = 32000; break;
		case 9: info->sample_rate = 44100; break;
		case 10: info->sample_rate = 48000; break;
		case 11: info->sample_rate = 96000; break;
		case 12:
			if(pos + 1 > avail) return false;
			info->sample_rate = (uint32_t)data[pos] * 1000;
			pos += 1;
			break;
		case 13:
			if(pos + 2 > avail) return false;
			info->sample_rate = ((uint32_t)data[pos] << 8) | data[pos+1];
			pos += 2;
			break;
		case 14:
			if(pos + 2 > avail) return false;
			info->sample_rate = (((uint32_t)data[pos] << 8) | data[pos+1]) * 10;
			pos += 2;
			break;
		default: return false;
	}

	switch(bps_code) {
		case 0: info->bits_per_sample = 0; break;
		case 1: info->bits_per_sample = 8; break;
		case 2: info->bits_per_sample = 12; break;
		case 4: info->bits_per_sample = 16; break;
		case 5: info->bits_per_sample = 20; break;
		case 6: info->bits_per_sample = 24; break;
		case 7: info->bits_per_sample = 32; break;
		default: return false;
	}

	if(channel_code < 8) {
		info->channels = channel_code + 1;
		info->channel_assignment = FLAC__CHANNEL_ASSIGNMENT_INDEPENDENT;
	}
	else {
		info->channels = 2;
		switch(channel_code) {
			case 8: info->channel_assignment = FLAC__CHANNEL_ASSIGNMENT_LEFT_SIDE; break;
			case 9: info->channel_assignment = FLAC__CHANNEL_ASSIGNMENT_RIGHT_SIDE; break;
			case 10: info->channel_assignment = FLAC__CHANNEL_ASSIGNMENT_MID_SIDE; break;
			default: return false;
		}
	}

	if(pos >= avail)
		return false;
	if(FLAC__crc8(data, (uint32_t)pos) != data[pos])
		return false;
	info->header_bytes = (uint32_t)(pos + 1);

	if(info->channels != 2 && info->channel_assignment != FLAC__CHANNEL_ASSIGNMENT_INDEPENDENT)
		return false;
	return true;
}

/***********************************************************************
 *
 * Source stream representation
 *
 ***********************************************************************/

typedef struct {
	size_t start;       /* offset of the frame header */
	size_t header_end;  /* offset just after the header CRC-8 */
	size_t end;         /* offset just after the frame CRC-16 */
	uint32_t blocksize;
	FLAC__uint64 sample;
	uint32_t channels;
	uint32_t bits_per_sample;
	uint32_t sample_rate;
	FLAC__ChannelAssignment channel_assignment;
} SpliceFrame;

typedef struct {
	const FLAC__byte *data;
	size_t bytes;
	FLAC__StreamMetadata_StreamInfo info;
	uint32_t fixed_blocksize;
	size_t audio_offset;
	SpliceFrame *frames;
	size_t frame_count, frame_capacity;
} SpliceStream;

static void splice_stream_clear(SpliceStream *stream)
{
	if(stream->frames)
		free(stream->frames);
	stream->frames = 0;
	stream->frame_count = stream->frame_capacity = 0;
}

typedef struct {
	const FLAC__byte *data;
	size_t bytes;
	size_t pos;
} SpliceMemInput;

static FLAC__StreamDecoderReadStatus splice_mem_read(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
	SpliceMemInput *in = (SpliceMemInput*)client_data;
	size_t remaining;
	(void)decoder;

	if(*bytes == 0)
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	remaining = in->bytes - in->pos;
	if(remaining == 0) {
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	if(*bytes > remaining)
		*bytes = remaining;
	memcpy(buffer, in->data + in->pos, *bytes);
	in->pos += *bytes;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderSeekStatus splice_mem_seek(const FLAC__StreamDecoder *decoder, FLAC__uint64 absolute_byte_offset, void *client_data)
{
	SpliceMemInput *in = (SpliceMemInput*)client_data;
	(void)decoder;
	if(absolute_byte_offset > in->bytes)
		return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
	in->pos = (size_t)absolute_byte_offset;
	return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

static FLAC__StreamDecoderTellStatus splice_mem_tell(const FLAC__StreamDecoder *decoder, FLAC__uint64 *absolute_byte_offset, void *client_data)
{
	SpliceMemInput *in = (SpliceMemInput*)client_data;
	(void)decoder;
	*absolute_byte_offset = (FLAC__uint64)in->pos;
	return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus splice_mem_length(const FLAC__StreamDecoder *decoder, FLAC__uint64 *stream_length, void *client_data)
{
	SpliceMemInput *in = (SpliceMemInput*)client_data;
	(void)decoder;
	*stream_length = (FLAC__uint64)in->bytes;
	return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool splice_mem_eof(const FLAC__StreamDecoder *decoder, void *client_data)
{
	SpliceMemInput *in = (SpliceMemInput*)client_data;
	(void)decoder;
	return in->pos >= in->bytes;
}

static FLAC__bool splice_stream_append_frame(SpliceStream *s, const SpliceFrame *frame)
{
	SpliceFrame *p;
	size_t capacity;

	if(s->frame_count == s->frame_capacity) {
		capacity = s->frame_capacity? s->frame_capacity * 2 : 32;
		if(capacity < s->frame_capacity)
			return false;
		if(capacity > SIZE_MAX / sizeof(SpliceFrame))
			return false;
		p = (SpliceFrame*)realloc(s->frames, capacity * sizeof(SpliceFrame));
		if(!p)
			return false;
		s->frames = p;
		s->frame_capacity = capacity;
	}
	s->frames[s->frame_count++] = *frame;
	return true;
}

typedef struct {
	SpliceMemInput in; /* must be first: the read callbacks receive this */
	SpliceStream *stream;
	size_t next_pos;
	FLAC__uint64 next_sample;
	FLAC__bool error;
} SpliceValidateContext;

static FLAC__StreamDecoderWriteStatus splice_validate_write(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data)
{
	SpliceValidateContext *ctx = (SpliceValidateContext*)client_data;
	FLAC__uint64 pos = 0;
	SpliceHeaderInfo hi;
	SpliceFrame f;
	size_t payload_end;
	FLAC__uint16 stored_crc, computed_crc;
	(void)buffer;

	if(ctx->error)
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	if(!FLAC__stream_decoder_get_decode_position(decoder, &pos)) {
		ctx->error = true;
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}
	if(pos <= ctx->next_pos || pos > ctx->stream->bytes) {
		ctx->error = true;
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}
	if(!splice_parse_frame_header(ctx->stream->data + ctx->next_pos, (size_t)(pos - ctx->next_pos), &hi)) {
		ctx->error = true;
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}
	if(hi.variable_blocking) {
		ctx->error = true;
	}
	else if(hi.blocksize != frame->header.blocksize ||
	        hi.channels != frame->header.channels ||
	        (hi.bits_per_sample != 0 && hi.bits_per_sample != frame->header.bits_per_sample) ||
	        (hi.sample_rate != 0 && hi.sample_rate != frame->header.sample_rate)) {
		ctx->error = true;
	}
	else if(frame->header.number_type != FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER ||
	        frame->header.number.sample_number != ctx->next_sample) {
		ctx->error = true;
	}
	else if(frame->header.sample_rate != ctx->stream->info.sample_rate ||
	        frame->header.channels != ctx->stream->info.channels ||
	        frame->header.bits_per_sample != ctx->stream->info.bits_per_sample) {
		ctx->error = true;
	}
	else if(frame->header.blocksize < SPLICE_MIN_BLOCK ||
	        frame->header.blocksize > ctx->stream->fixed_blocksize) {
		ctx->error = true;
	}

	if(!ctx->error) {
		payload_end = (size_t)pos - 2;
		if(payload_end <= ctx->next_pos + hi.header_bytes) {
			ctx->error = true;
		}
		else {
			stored_crc = (FLAC__uint16)(((FLAC__uint16)ctx->stream->data[payload_end] << 8) | ctx->stream->data[payload_end + 1]);
			computed_crc = FLAC__crc16(ctx->stream->data + ctx->next_pos, (uint32_t)(payload_end - ctx->next_pos));
			if(stored_crc != computed_crc)
				ctx->error = true;
		}
	}
	if(ctx->error)
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;

	f.start = ctx->next_pos;
	f.header_end = ctx->next_pos + hi.header_bytes;
	f.end = (size_t)pos;
	f.blocksize = frame->header.blocksize;
	f.sample = frame->header.number.sample_number;
	f.channels = frame->header.channels;
	f.bits_per_sample = frame->header.bits_per_sample;
	f.sample_rate = frame->header.sample_rate;
	f.channel_assignment = frame->header.channel_assignment;

	if(!splice_stream_append_frame(ctx->stream, &f)) {
		ctx->error = true;
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}

	/* Enforce the constraints of this interface as the frames are seen. */
	if(f.blocksize > SPLICE_MAX_SOURCE_BLOCK || f.channels > SPLICE_MAX_CHANNELS) {
		ctx->error = true;
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}

	ctx->next_pos = (size_t)pos;
	ctx->next_sample += f.blocksize;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void splice_validate_error(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
	SpliceValidateContext *ctx = (SpliceValidateContext*)client_data;
	(void)decoder;
	(void)status;
	ctx->error = true;
}

static void splice_validate_metadata(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
	(void)decoder;
	(void)metadata;
	(void)client_data;
}

static void splice_parse_stream_info(const FLAC__byte *body, FLAC__StreamMetadata_StreamInfo *info)
{
	memset(info, 0, sizeof(*info));
	info->min_blocksize = (uint32_t)splice_get_bits(body, 0, 16);
	info->max_blocksize = (uint32_t)splice_get_bits(body, 16, 16);
	info->min_framesize = (uint32_t)splice_get_bits(body, 32, 24);
	info->max_framesize = (uint32_t)splice_get_bits(body, 56, 24);
	info->sample_rate = (uint32_t)splice_get_bits(body, 80, 20);
	info->channels = (uint32_t)splice_get_bits(body, 100, 3) + 1;
	info->bits_per_sample = (uint32_t)splice_get_bits(body, 103, 5) + 1;
	info->total_samples = splice_get_bits(body, 108, 36);
	memcpy(info->md5sum, body + 18, 16);
}

static FLAC__bool splice_parse_stream(const FLAC__byte *data, size_t bytes, SpliceStream *stream)
{
	size_t pos, i;
	FLAC__bool is_first = true, have_streaminfo = false, is_last = false;
	FLAC__StreamDecoder *decoder = 0;
	SpliceValidateContext ctx;
	FLAC__bool ok = false;

	memset(stream, 0, sizeof(*stream));
	stream->data = data;
	stream->bytes = bytes;

	if(bytes < 8 || memcmp(data, "fLaC", 4))
		goto cleanup;

	/* Walk the metadata blocks.  Exactly one STREAMINFO must come first;
	 * any other leading metadata is tolerated but never carried over. */
	pos = 4;
	while(!is_last) {
		uint32_t length, type;
		if(pos + 4 > bytes)
			goto cleanup;
		is_last = (data[pos] & 0x80)? true : false;
		type = data[pos] & 0x7F;
		length = ((uint32_t)data[pos+1] << 16) | ((uint32_t)data[pos+2] << 8) | data[pos+3];
		pos += 4;
		if(type == 127 || length > bytes - pos)
			goto cleanup;
		if(is_first) {
			if(type != FLAC__METADATA_TYPE_STREAMINFO || length != FLAC__STREAM_METADATA_STREAMINFO_LENGTH)
				goto cleanup;
			splice_parse_stream_info(data + pos, &stream->info);
			have_streaminfo = true;
			is_first = false;
		}
		else if(type == FLAC__METADATA_TYPE_STREAMINFO) {
			goto cleanup;
		}
		pos += length;
	}
	if(!have_streaminfo || pos >= bytes)
		goto cleanup;
	stream->audio_offset = pos;

	/* Only the supported subset of the format is accepted. */
	if(stream->info.sample_rate != 44100 && stream->info.sample_rate != 48000)
		goto cleanup;
	if(stream->info.channels < 1 || stream->info.channels > SPLICE_MAX_CHANNELS)
		goto cleanup;
	if(stream->info.bits_per_sample != 16 && stream->info.bits_per_sample != 24)
		goto cleanup;
	if(stream->info.total_samples == 0)
		goto cleanup;
	if(stream->info.min_blocksize < SPLICE_MIN_BLOCK ||
	   stream->info.max_blocksize > SPLICE_MAX_SOURCE_BLOCK ||
	   stream->info.min_blocksize != stream->info.max_blocksize)
		goto cleanup;
	stream->fixed_blocksize = stream->info.min_blocksize;

	/* Walk the whole stream once.  This validates every frame header and
	 * both CRCs, verifies the source MD5 signature, and builds a byte
	 * index of all frames for later use. */
	memset(&ctx, 0, sizeof(ctx));
	ctx.stream = stream;
	ctx.next_pos = stream->audio_offset;
	ctx.next_sample = 0;
	ctx.in.data = data;
	ctx.in.bytes = bytes;
	ctx.in.pos = 0;

	decoder = FLAC__stream_decoder_new();
	if(!decoder)
		goto cleanup;
	if(!FLAC__stream_decoder_set_md5_checking(decoder, true))
		goto cleanup;
	if(FLAC__stream_decoder_init_stream(decoder, splice_mem_read, splice_mem_seek, splice_mem_tell, splice_mem_length, splice_mem_eof, splice_validate_write, splice_validate_metadata, splice_validate_error, &ctx) != FLAC__STREAM_DECODER_INIT_STATUS_OK)
		goto cleanup;
	if(!FLAC__stream_decoder_process_until_end_of_stream(decoder))
		goto cleanup;
	if(ctx.error || FLAC__stream_decoder_get_state(decoder) != FLAC__STREAM_DECODER_END_OF_STREAM)
		goto cleanup;
	/* A false return means the source MD5 signature did not match.  A
	 * zero signature is allowed; the decoder never checks it. */
	if(!FLAC__stream_decoder_finish(decoder))
		goto cleanup;

	if(ctx.next_pos != bytes)
		goto cleanup;
	if(stream->frame_count == 0)
		goto cleanup;
	if(ctx.next_sample != stream->info.total_samples)
		goto cleanup;

	for(i = 0; i < stream->frame_count; i++) {
		if(i + 1 < stream->frame_count) {
			if(stream->frames[i].blocksize != stream->fixed_blocksize)
				goto cleanup;
		}
		else if(stream->frames[i].blocksize > stream->fixed_blocksize) {
			goto cleanup;
		}
		if(stream->frames[i].blocksize < SPLICE_MIN_BLOCK)
			goto cleanup;
		if(stream->frames[i].end > bytes || stream->frames[i].start >= stream->frames[i].end)
			goto cleanup;
		if(i == 0) {
			if(stream->frames[i].sample != 0)
				goto cleanup;
		}
		else if(stream->frames[i].sample != stream->frames[i-1].sample + stream->frames[i-1].blocksize) {
			goto cleanup;
		}
	}
	ok = true;

cleanup:
	if(decoder) {
		(void)FLAC__stream_decoder_finish(decoder);
		FLAC__stream_decoder_delete(decoder);
	}
	if(!ok)
		splice_stream_clear(stream);
	return ok;
}

/***********************************************************************
 *
 * Output planning
 *
 ***********************************************************************/

typedef struct {
	FLAC__bool encode;
	size_t stream;
	size_t frame;             /* copy only */
	FLAC__uint64 lo, hi;      /* encode only, absolute source samples */
	FLAC__uint64 out_sample;  /* absolute output sample */
	uint32_t blocksize;       /* copy only */
} SpliceUnit;

typedef struct {
	SpliceUnit *units;
	size_t count, capacity;
} SpliceUnitList;

static void splice_units_clear(SpliceUnitList *list)
{
	if(list->units)
		free(list->units);
	list->units = 0;
	list->count = list->capacity = 0;
}

static FLAC__bool splice_units_append(SpliceUnitList *list, const SpliceUnit *unit)
{
	SpliceUnit *p;
	size_t capacity;
	if(list->count == list->capacity) {
		capacity = list->capacity? list->capacity * 2 : 64;
		if(capacity < list->capacity || capacity > SIZE_MAX / sizeof(SpliceUnit))
			return false;
		p = (SpliceUnit*)realloc(list->units, capacity * sizeof(SpliceUnit));
		if(!p)
			return false;
		list->units = p;
		list->capacity = capacity;
	}
	list->units[list->count++] = *unit;
	return true;
}

static size_t splice_find_frame(const SpliceStream *stream, FLAC__uint64 sample)
{
	size_t low = 0, high = stream->frame_count;
	while(low < high) {
		size_t mid = low + (high - low) / 2;
		if(stream->frames[mid].sample <= sample)
			low = mid + 1;
		else
			high = mid;
	}
	return low? low - 1 : 0;
}

typedef struct {
	FLAC__uint64 lo, hi;
	FLAC__bool full;
	size_t frame;
} SplicePiece;

static FLAC__bool splice_plan_segment(SpliceUnitList *units, const SpliceStream *stream, size_t stream_index, FLAC__uint64 begin, FLAC__uint64 end, FLAC__uint64 *out_sample)
{
	SplicePiece *pieces;
	size_t first, last, count, i;
	FLAC__bool ok = true;

	first = splice_find_frame(stream, begin);
	last = splice_find_frame(stream, end - 1);
	if(first > last)
		return false;
	if(begin < stream->frames[first].sample ||
	   end > stream->frames[last].sample + stream->frames[last].blocksize)
		return false;

	count = last - first + 1;
	if(count > SIZE_MAX / sizeof(SplicePiece))
		return false;
	pieces = (SplicePiece*)malloc(count * sizeof(SplicePiece));
	if(!pieces)
		return false;

	for(i = 0; i < count; i++) {
		const SpliceFrame *f = &stream->frames[first + i];
		FLAC__uint64 lo = begin > f->sample? begin : f->sample;
		FLAC__uint64 hi = end < f->sample + f->blocksize? end : f->sample + f->blocksize;
		pieces[i].lo = lo;
		pieces[i].hi = hi;
		pieces[i].full = (lo == f->sample && hi == f->sample + f->blocksize)? true : false;
		pieces[i].frame = first + i;
	}

	/* A partial edge piece shorter than the minimum block size may not
	 * stand alone.  Merge it with its only neighbour inside this
	 * segment.  Every segment boundary touches at most one frame at each
	 * end, so this never produces an interior short piece. */
	for(;;) {
		size_t short_index = SIZE_MAX, other;
		for(i = 0; i < count; i++) {
			if(!pieces[i].full && pieces[i].hi - pieces[i].lo < SPLICE_MIN_BLOCK) {
				short_index = i;
				break;
			}
		}
		if(short_index == SIZE_MAX)
			break;
		if(count < 2) {
			/* The whole segment lies in one frame.  Its length is at
			 * least the minimum segment length, so it cannot be short. */
			ok = false;
			goto done;
		}
		if(short_index == 0)
			other = 1;
		else if(short_index == count - 1)
			other = count - 2;
		else {
			ok = false;
			goto done;
		}
		if(pieces[other].lo < pieces[short_index].lo)
			pieces[short_index].lo = pieces[other].lo;
		if(pieces[other].hi > pieces[short_index].hi)
			pieces[short_index].hi = pieces[other].hi;
		pieces[short_index].full = false;
		for(i = other; i + 1 < count; i++)
			pieces[i] = pieces[i + 1];
		count--;
	}

	for(i = 0; i < count; i++) {
		SpliceUnit unit;
		memset(&unit, 0, sizeof(unit));
		unit.stream = stream_index;
		unit.out_sample = *out_sample;
		if(pieces[i].full) {
			const SpliceFrame *f = &stream->frames[pieces[i].frame];
			unit.encode = false;
			unit.frame = pieces[i].frame;
			unit.blocksize = f->blocksize;
			if(pieces[i].hi - pieces[i].lo != f->blocksize)
				ok = false;
		}
		else {
			if(pieces[i].hi - pieces[i].lo < SPLICE_MIN_BLOCK ||
			   pieces[i].hi - pieces[i].lo > FLAC__MAX_BLOCK_SIZE) {
				ok = false;
			}
			unit.encode = true;
			unit.lo = pieces[i].lo;
			unit.hi = pieces[i].hi;
		}
		if(!ok || !splice_units_append(units, &unit)) {
			ok = false;
			goto done;
		}
		*out_sample += pieces[i].hi - pieces[i].lo;
	}

done:
	free(pieces);
	return ok;
}

/***********************************************************************
 *
 * Source frame decoding for payload retrieval and MD5
 *
 ***********************************************************************/

typedef struct {
	SpliceMemInput in; /* must be first: the read callbacks receive this */
	const SpliceStream *stream;
	FLAC__StreamDecoder *decoder;
	FLAC__int32 *pcm[SPLICE_MAX_CHANNELS];
	uint32_t capacity;
	FLAC__bool got_frame;
	FLAC__bool error;
	FLAC__bool pending;
	FLAC__uint64 frame_sample;
	uint32_t frame_blocksize;
	uint32_t frame_channels;
	uint32_t frame_bits;
	uint32_t frame_sample_rate;
} SpliceReader;

static FLAC__StreamDecoderWriteStatus splice_reader_write(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data)
{
	SpliceReader *reader = (SpliceReader*)client_data;
	uint32_t channel;
	(void)decoder;

	if(reader->error)
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	if(frame->header.channels != reader->stream->info.channels ||
	   frame->header.bits_per_sample != reader->stream->info.bits_per_sample ||
	   frame->header.sample_rate != reader->stream->info.sample_rate ||
	   frame->header.blocksize > reader->capacity) {
		reader->error = true;
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}
	for(channel = 0; channel < frame->header.channels; channel++) {
		memcpy(reader->pcm[channel], buffer[channel], sizeof(FLAC__int32) * frame->header.blocksize);
	}
	reader->frame_sample = frame->header.number.sample_number;
	reader->frame_blocksize = frame->header.blocksize;
	reader->frame_channels = frame->header.channels;
	reader->frame_bits = frame->header.bits_per_sample;
	reader->frame_sample_rate = frame->header.sample_rate;
	reader->got_frame = true;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void splice_reader_error_cb(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
	SpliceReader *reader = (SpliceReader*)client_data;
	(void)decoder;
	(void)status;
	reader->error = true;
}

static void splice_reader_metadata_cb(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
	(void)decoder;
	(void)metadata;
	(void)client_data;
}

static void splice_reader_close(SpliceReader *reader)
{
	uint32_t channel;
	if(reader->decoder) {
		(void)FLAC__stream_decoder_finish(reader->decoder);
		FLAC__stream_decoder_delete(reader->decoder);
		reader->decoder = 0;
	}
	for(channel = 0; channel < SPLICE_MAX_CHANNELS; channel++) {
		if(reader->pcm[channel]) {
			free(reader->pcm[channel]);
			reader->pcm[channel] = 0;
		}
	}
	reader->capacity = 0;
}

static FLAC__bool splice_reader_open(SpliceReader *reader, const SpliceStream *stream, FLAC__uint64 first_sample)
{
	uint32_t channel;
	size_t bytes;

	memset(reader, 0, sizeof(*reader));
	reader->stream = stream;
	reader->in.data = stream->data;
	reader->in.bytes = stream->bytes;
	reader->in.pos = 0;
	reader->capacity = stream->info.max_blocksize;
	if(reader->capacity < SPLICE_MIN_BLOCK)
		reader->capacity = SPLICE_MIN_BLOCK;
	if((size_t)reader->capacity > SIZE_MAX / sizeof(FLAC__int32))
		return false;
	bytes = (size_t)reader->capacity * sizeof(FLAC__int32);
	for(channel = 0; channel < SPLICE_MAX_CHANNELS; channel++) {
		reader->pcm[channel] = (FLAC__int32*)malloc(bytes);
		if(!reader->pcm[channel]) {
			splice_reader_close(reader);
			return false;
		}
	}

	reader->decoder = FLAC__stream_decoder_new();
	if(!reader->decoder) {
		splice_reader_close(reader);
		return false;
	}
	if(FLAC__stream_decoder_init_stream(reader->decoder, splice_mem_read, splice_mem_seek, splice_mem_tell, splice_mem_length, splice_mem_eof, splice_reader_write, splice_reader_metadata_cb, splice_reader_error_cb, reader) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		splice_reader_close(reader);
		return false;
	}
	if(!FLAC__stream_decoder_seek_absolute(reader->decoder, first_sample)) {
		splice_reader_close(reader);
		return false;
	}
	if(reader->error || !reader->got_frame || reader->frame_sample != first_sample) {
		splice_reader_close(reader);
		return false;
	}
	reader->pending = true;
	return true;
}

/* Advance to the next frame.  Returns false at the end of the stream or
 * on any decoding error.  A successful call leaves the samples of the
 * frame in reader->pcm and its metadata in the frame_* fields. */
static FLAC__bool splice_reader_next(SpliceReader *reader)
{
	if(reader->pending) {
		reader->pending = false;
		return true;
	}
	reader->got_frame = false;
	if(reader->error)
		return false;
	if(!FLAC__stream_decoder_process_single(reader->decoder))
		return false;
	if(reader->error || !reader->got_frame)
		return false;
	return true;
}

/***********************************************************************
 *
 * Output frame generation
 *
 ***********************************************************************/

typedef struct {
	SpliceBuffer out;
	FLAC__uint32 sample_rate;
	FLAC__uint32 channels;
	FLAC__uint32 bits_per_sample;
	uint32_t bytes_per_sample;
	FLAC__MD5Context md5;
	FLAC__bool md5_initialized;
	FLAC__uint32 min_blocksize, max_blocksize;
	FLAC__uint32 min_framesize, max_framesize;
	/* One decoder is kept open to serve sequential reads from the stream
	 * currently being processed.  It is repositioned only when the next
	 * requested frame is not the one that follows the previous read. */
	SpliceReader reader;
	FLAC__bool reader_open;
	const SpliceStream *reader_stream;
	FLAC__uint64 reader_next_sample;
} SpliceBuildContext;

static void splice_build_release_reader(SpliceBuildContext *ctx)
{
	if(ctx->reader_open) {
		splice_reader_close(&ctx->reader);
		ctx->reader_open = false;
	}
	ctx->reader_stream = 0;
}

/* Make sure the cached reader is positioned so that the next
 * splice_reader_next() returns the source frame that begins at
 * first_sample. */
static FLAC__bool splice_build_position_reader(SpliceBuildContext *ctx, const SpliceStream *stream, FLAC__uint64 first_sample)
{
	if(ctx->reader_open && ctx->reader_stream == stream && ctx->reader_next_sample == first_sample)
		return true;
	splice_build_release_reader(ctx);
	if(!splice_reader_open(&ctx->reader, stream, first_sample))
		return false;
	ctx->reader_open = true;
	ctx->reader_stream = stream;
	ctx->reader_next_sample = first_sample;
	return true;
}

/* Emit one output frame: a freshly written variable-blocking header with
 * the given absolute sample number, the already encoded payload, and a
 * recomputed frame CRC-16 over the resulting bytes. */
static FLAC__bool splice_append_output_frame(SpliceBuildContext *ctx, FLAC__ChannelAssignment channel_assignment, uint32_t blocksize, FLAC__uint64 sample_number, const FLAC__byte *payload, size_t payload_bytes)
{
	FLAC__BitWriter *bw = 0;
	FLAC__FrameHeader header;
	const FLAC__byte *header_bytes = 0;
	size_t header_length = 0, frame_start, frame_bytes;
	FLAC__uint16 crc;
	FLAC__byte crc_bytes[2];
	FLAC__bool ok = false;

	if(payload_bytes > UINT32_MAX)
		return false;

	memset(&header, 0, sizeof(header));
	header.blocksize = blocksize;
	header.sample_rate = ctx->sample_rate;
	header.channels = ctx->channels;
	header.bits_per_sample = ctx->bits_per_sample;
	header.channel_assignment = channel_assignment;
	header.number_type = FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER;
	header.number.sample_number = sample_number;

	bw = FLAC__bitwriter_new();
	if(!bw)
		return false;
	if(!FLAC__bitwriter_init(bw))
		goto done;
	if(!FLAC__frame_add_header(&header, bw))
		goto done;
	if(!FLAC__bitwriter_is_byte_aligned(bw))
		goto done;
	if(!FLAC__bitwriter_get_buffer(bw, &header_bytes, &header_length))
		goto done;
	if(header_length == 0 || header_length > SPLICE_MAX_HEADER_BYTES)
		goto done;

	frame_start = ctx->out.bytes;
	if(!splice_buffer_append(&ctx->out, header_bytes, header_length))
		goto done;
	FLAC__bitwriter_release_buffer(bw);
	header_bytes = 0;
	if(!splice_buffer_append(&ctx->out, payload, payload_bytes))
		goto done;

	{
		size_t body_bytes = ctx->out.bytes - frame_start;
		if(body_bytes > UINT32_MAX)
			goto done;
		crc = FLAC__crc16(ctx->out.data + frame_start, (uint32_t)body_bytes);
	}
	crc_bytes[0] = (FLAC__byte)(crc >> 8);
	crc_bytes[1] = (FLAC__byte)(crc & 0xFF);
	if(!splice_buffer_append(&ctx->out, crc_bytes, 2))
		goto done;

	frame_bytes = ctx->out.bytes - frame_start;
	if(ctx->min_blocksize == 0 || blocksize < ctx->min_blocksize)
		ctx->min_blocksize = blocksize;
	if(blocksize > ctx->max_blocksize)
		ctx->max_blocksize = blocksize;
	if(ctx->min_framesize == 0 || frame_bytes < ctx->min_framesize)
		ctx->min_framesize = (FLAC__uint32)frame_bytes;
	if(frame_bytes > ctx->max_framesize)
		ctx->max_framesize = (FLAC__uint32)frame_bytes;
	ok = true;

done:
	if(bw) {
		if(header_bytes)
			FLAC__bitwriter_release_buffer(bw);
		FLAC__bitwriter_delete(bw);
	}
	return ok;
}

static FLAC__bool splice_md5_feed(SpliceBuildContext *ctx, const SpliceReader *reader, uint32_t offset, uint32_t samples)
{
	const FLAC__int32 *signal[SPLICE_MAX_CHANNELS];
	uint32_t channel;
	if(samples == 0)
		return true;
	for(channel = 0; channel < ctx->channels; channel++)
		signal[channel] = reader->pcm[channel] + offset;
	return FLAC__MD5Accumulate(&ctx->md5, signal, ctx->channels, samples, ctx->bytes_per_sample);
}

static FLAC__bool splice_process_copy(SpliceBuildContext *ctx, const SpliceStream *stream, const SpliceUnit *unit)
{
	const SpliceFrame *frame = &stream->frames[unit->frame];
	SpliceReader *reader = &ctx->reader;
	size_t payload_start, payload_end;

	if(!splice_build_position_reader(ctx, stream, frame->sample))
		return false;
	if(!splice_reader_next(reader))
		return false;
	ctx->reader_next_sample = reader->frame_sample + reader->frame_blocksize;
	if(reader->frame_sample != frame->sample || reader->frame_blocksize != frame->blocksize ||
	   reader->frame_channels != ctx->channels || reader->frame_bits != ctx->bits_per_sample ||
	   reader->frame_sample_rate != ctx->sample_rate)
		return false;
	if(!splice_md5_feed(ctx, reader, 0, frame->blocksize))
		return false;

	/* Everything between the source header CRC-8 and the frame CRC-16 is
	 * copied verbatim. */
	payload_start = frame->header_end;
	payload_end = frame->end - 2;
	if(payload_start >= payload_end || payload_end > stream->bytes)
		return false;
	return splice_append_output_frame(ctx, frame->channel_assignment, frame->blocksize, unit->out_sample, stream->data + payload_start, payload_end - payload_start);
}

typedef struct {
	SpliceBuffer frame;
	uint32_t frame_count;
	FLAC__uint64 samples;
	FLAC__bool error;
} SpliceEncoderCollector;

static FLAC__StreamEncoderWriteStatus splice_encoder_write(const FLAC__StreamEncoder *encoder, const FLAC__byte buffer[], size_t bytes, uint32_t samples, uint32_t current_frame, void *client_data)
{
	SpliceEncoderCollector *collector = (SpliceEncoderCollector*)client_data;
	(void)encoder;
	(void)current_frame;
	if(samples == 0)
		return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
	if(collector->error)
		return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
	if(!splice_buffer_append(&collector->frame, buffer, bytes)) {
		collector->error = true;
		return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
	}
	collector->frame_count++;
	collector->samples += samples;
	return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

/* Re-encode the retained part of the frames that a segment edge cuts
 * through.  The existing stream encoder does the prediction and channel
 * selection; only a single output block is produced here. */
static FLAC__bool splice_process_encode(SpliceBuildContext *ctx, const SpliceStream *stream, const SpliceUnit *unit)
{
	SpliceReader reader;
	FLAC__StreamEncoder *encoder = 0;
	SpliceEncoderCollector collector;
	SpliceHeaderInfo enc_header;
	const SpliceFrame *first_frame, *last_frame;
	FLAC__uint64 total = unit->hi - unit->lo;
	FLAC__uint64 fed = 0, frame_end;
	uint32_t blocksize;
	size_t first, last, i, payload_start, payload_end;
	FLAC__uint16 stored_crc, computed_crc;
	FLAC__bool ok = false;

	if(total < SPLICE_MIN_BLOCK || total > FLAC__MAX_BLOCK_SIZE)
		return false;
	blocksize = (uint32_t)total;

	first = splice_find_frame(stream, unit->lo);
	last = splice_find_frame(stream, unit->hi - 1);
	if(first > last)
		return false;
	first_frame = &stream->frames[first];
	last_frame = &stream->frames[last];
	frame_end = last_frame->sample + last_frame->blocksize;
	if(unit->lo < first_frame->sample || unit->hi > frame_end)
		return false;

	memset(&collector, 0, sizeof(collector));
	memset(&reader, 0, sizeof(reader));

	encoder = FLAC__stream_encoder_new();
	if(!encoder)
		return false;
	if(!FLAC__stream_encoder_set_channels(encoder, ctx->channels) ||
	   !FLAC__stream_encoder_set_bits_per_sample(encoder, ctx->bits_per_sample) ||
	   !FLAC__stream_encoder_set_sample_rate(encoder, ctx->sample_rate) ||
	   !FLAC__stream_encoder_set_compression_level(encoder, 5) ||
	   !FLAC__stream_encoder_set_blocksize(encoder, blocksize) ||
	   !FLAC__stream_encoder_set_total_samples_estimate(encoder, total) ||
	   !FLAC__stream_encoder_set_do_md5(encoder, false) ||
	   !FLAC__stream_encoder_set_verify(encoder, false))
		goto done;
	if(FLAC__stream_encoder_init_stream(encoder, splice_encoder_write, 0, 0, 0, &collector) != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
		goto done;

	if(!splice_reader_open(&reader, stream, first_frame->sample))
		goto done;

	for(i = first; i <= last; i++) {
		const SpliceFrame *f = &stream->frames[i];
		FLAC__uint64 lo, hi;
		uint32_t offset, count;
		const FLAC__int32 *signal[SPLICE_MAX_CHANNELS];
		uint32_t channel;

		if(!splice_reader_next(&reader))
			goto done;
		if(reader.frame_sample != f->sample || reader.frame_blocksize != f->blocksize ||
		   reader.frame_channels != ctx->channels || reader.frame_bits != ctx->bits_per_sample ||
		   reader.frame_sample_rate != ctx->sample_rate)
			goto done;
		lo = unit->lo > f->sample? unit->lo : f->sample;
		hi = unit->hi < f->sample + f->blocksize? unit->hi : f->sample + f->blocksize;
		if(hi <= lo)
			goto done;
		offset = (uint32_t)(lo - f->sample);
		count = (uint32_t)(hi - lo);
		if(offset + count > f->blocksize)
			goto done;
		if(!splice_md5_feed(ctx, &reader, offset, count))
			goto done;
		for(channel = 0; channel < ctx->channels; channel++)
			signal[channel] = reader.pcm[channel] + offset;
		if(!FLAC__stream_encoder_process(encoder, signal, count))
			goto done;
		fed += count;
	}
	if(fed != total)
		goto done;
	if(!FLAC__stream_encoder_finish(encoder))
		goto done;
	FLAC__stream_encoder_delete(encoder);
	encoder = 0;
	if(collector.error || collector.frame_count != 1 || collector.samples != total)
		goto done;
	if(collector.frame.bytes < 8)
		goto done;
	if(!splice_parse_frame_header(collector.frame.data, collector.frame.bytes, &enc_header))
		goto done;
	if(enc_header.blocksize != blocksize || enc_header.channels != ctx->channels ||
	   enc_header.bits_per_sample != ctx->bits_per_sample ||
	   (enc_header.sample_rate != 0 && enc_header.sample_rate != ctx->sample_rate))
		goto done;
	stored_crc = (FLAC__uint16)(((FLAC__uint16)collector.frame.data[collector.frame.bytes - 2] << 8) | collector.frame.data[collector.frame.bytes - 1]);
	computed_crc = FLAC__crc16(collector.frame.data, (uint32_t)(collector.frame.bytes - 2));
	if(stored_crc != computed_crc)
		goto done;
	payload_start = enc_header.header_bytes;
	payload_end = collector.frame.bytes - 2;
	if(payload_start >= payload_end)
		goto done;
	if(!splice_append_output_frame(ctx, enc_header.channel_assignment, blocksize, unit->out_sample, collector.frame.data + payload_start, payload_end - payload_start))
		goto done;
	ok = true;

done:
	if(encoder)
		FLAC__stream_encoder_delete(encoder);
	splice_reader_close(&reader);
	splice_buffer_clear(&collector.frame);
	return ok;
}

/***********************************************************************
 *
 * Public API
 *
 ***********************************************************************/

FLAC_API void FLAC__stream_splice_free(FLAC__byte *buffer)
{
	if(buffer)
		free(buffer);
}

FLAC_API FLAC__bool FLAC__stream_splice(const FLAC__SpliceSegment *segments, size_t count, FLAC__byte **output, size_t *output_bytes)
{
	SpliceStream *streams = 0;
	size_t stream_count = 0;
	SpliceBuildContext ctx;
	SpliceUnitList units;
	FLAC__bool ok = false;
	size_t i;
	FLAC__uint64 out_sample = 0;
	FLAC__byte digest[16];
	FLAC__BitWriter *metadata_bw = 0;
	const FLAC__byte *metadata_bytes = 0;
	size_t metadata_length = 0;
	FLAC__StreamMetadata metadata;

	if(output)
		*output = 0;
	if(output_bytes)
		*output_bytes = 0;
	if(!segments || !output || !output_bytes || count == 0)
		return false;
	if(count > SIZE_MAX / sizeof(SpliceStream))
		return false;

	memset(&ctx, 0, sizeof(ctx));
	memset(&units, 0, sizeof(units));

	/* Attach a parsed source stream to each segment.  The same buffer is
	 * parsed only once even when several segments refer to it. */
	streams = (SpliceStream*)calloc(count, sizeof(SpliceStream));
	if(!streams)
		goto cleanup;

	for(i = 0; i < count; i++) {
		const FLAC__SpliceSegment *segment = &segments[i];
		size_t s;

		if(!segment->data || segment->bytes == 0)
			goto cleanup;
		if(segment->begin >= segment->end || segment->end - segment->begin < SPLICE_MIN_BLOCK)
			goto cleanup;

		for(s = 0; s < stream_count; s++) {
			if(streams[s].data == segment->data && streams[s].bytes == segment->bytes)
				break;
		}
		if(s == stream_count) {
			if(!splice_parse_stream(segment->data, segment->bytes, &streams[stream_count]))
				goto cleanup;
			stream_count++;
		}
		if(segment->end > streams[s].info.total_samples)
			goto cleanup;
	}

	/* All inputs in one call must share the same format. */
	ctx.sample_rate = streams[0].info.sample_rate;
	ctx.channels = streams[0].info.channels;
	ctx.bits_per_sample = streams[0].info.bits_per_sample;
	ctx.bytes_per_sample = (ctx.bits_per_sample + 7) / 8;
	for(i = 1; i < stream_count; i++) {
		if(streams[i].info.sample_rate != ctx.sample_rate ||
		   streams[i].info.channels != ctx.channels ||
		   streams[i].info.bits_per_sample != ctx.bits_per_sample)
			goto cleanup;
	}

	/* Plan the output, in segment order. */
	for(i = 0; i < count; i++) {
		const FLAC__SpliceSegment *segment = &segments[i];
		size_t s;
		for(s = 0; s < stream_count; s++) {
			if(streams[s].data == segment->data && streams[s].bytes == segment->bytes)
				break;
		}
		if(s == stream_count)
			goto cleanup;
		if(!splice_plan_segment(&units, &streams[s], s, segment->begin, segment->end, &out_sample))
			goto cleanup;
	}
	if(units.count == 0 || out_sample == 0)
		goto cleanup;
	if(out_sample >= (FLAC__U64L(1) << FLAC__STREAM_METADATA_STREAMINFO_TOTAL_SAMPLES_LEN))
		goto cleanup;

	/* Reserve the fLaC marker and a placeholder for the single
	 * STREAMINFO block; it is filled in once the final statistics and
	 * MD5 signature are known. */
	if(!splice_buffer_append(&ctx.out, "fLaC", 4))
		goto cleanup;
	{
		FLAC__byte zeros[FLAC__STREAM_METADATA_HEADER_LENGTH + FLAC__STREAM_METADATA_STREAMINFO_LENGTH];
		memset(zeros, 0, sizeof(zeros));
		if(!splice_buffer_append(&ctx.out, zeros, sizeof(zeros)))
			goto cleanup;
	}

	FLAC__MD5Init(&ctx.md5);
	ctx.md5_initialized = true;

	for(i = 0; i < units.count; i++) {
		const SpliceUnit *unit = &units.units[i];
		if(unit->stream >= stream_count)
			goto cleanup;
		if(unit->encode) {
			if(!splice_process_encode(&ctx, &streams[unit->stream], unit))
				goto cleanup;
		}
		else {
			if(!splice_process_copy(&ctx, &streams[unit->stream], unit))
				goto cleanup;
		}
	}

	FLAC__MD5Final(digest, &ctx.md5);
	ctx.md5_initialized = false;

	if(ctx.min_blocksize == 0 || ctx.max_blocksize == 0 ||
	   ctx.min_framesize == 0 || ctx.max_framesize == 0)
		goto cleanup;
	if(ctx.max_framesize >= (1u << FLAC__STREAM_METADATA_STREAMINFO_MAX_FRAME_SIZE_LEN))
		goto cleanup;

	/* Build and install the STREAMINFO block. */
	memset(&metadata, 0, sizeof(metadata));
	metadata.type = FLAC__METADATA_TYPE_STREAMINFO;
	metadata.is_last = true;
	metadata.length = FLAC__STREAM_METADATA_STREAMINFO_LENGTH;
	metadata.data.stream_info.min_blocksize = ctx.min_blocksize;
	metadata.data.stream_info.max_blocksize = ctx.max_blocksize;
	metadata.data.stream_info.min_framesize = ctx.min_framesize;
	metadata.data.stream_info.max_framesize = ctx.max_framesize;
	metadata.data.stream_info.sample_rate = ctx.sample_rate;
	metadata.data.stream_info.channels = ctx.channels;
	metadata.data.stream_info.bits_per_sample = ctx.bits_per_sample;
	metadata.data.stream_info.total_samples = out_sample;
	memcpy(metadata.data.stream_info.md5sum, digest, 16);

	metadata_bw = FLAC__bitwriter_new();
	if(!metadata_bw)
		goto cleanup;
	if(!FLAC__bitwriter_init(metadata_bw))
		goto cleanup;
	if(!FLAC__add_metadata_block(&metadata, metadata_bw, false))
		goto cleanup;
	if(!FLAC__bitwriter_is_byte_aligned(metadata_bw))
		goto cleanup;
	if(!FLAC__bitwriter_get_buffer(metadata_bw, &metadata_bytes, &metadata_length))
		goto cleanup;
	if(metadata_length != FLAC__STREAM_METADATA_HEADER_LENGTH + FLAC__STREAM_METADATA_STREAMINFO_LENGTH)
		goto cleanup;
	memcpy(ctx.out.data + 4, metadata_bytes, metadata_length);

	*output = ctx.out.data;
	*output_bytes = ctx.out.bytes;
	ctx.out.data = 0;
	ctx.out.bytes = ctx.out.capacity = 0;
	ok = true;

cleanup:
	if(ctx.md5_initialized) {
		FLAC__MD5Final(digest, &ctx.md5);
		ctx.md5_initialized = false;
	}
	if(metadata_bw) {
		if(metadata_bytes)
			FLAC__bitwriter_release_buffer(metadata_bw);
		FLAC__bitwriter_delete(metadata_bw);
	}
	splice_build_release_reader(&ctx);
	splice_buffer_clear(&ctx.out);
	splice_units_clear(&units);
	for(i = 0; i < stream_count; i++)
		splice_stream_clear(&streams[i]);
	if(streams)
		free(streams);
	return ok;
}
