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

/*
 * Tests for FLAC__stream_splice().  All material is produced in-process
 * with the existing stream encoder; no external files or programs are
 * involved.  Cases are registered for the pcm, payload and metadata groups.
 *
 * The payload group deliberately re-parses the produced streams on its
 * own: frame boundaries come from the stock decoder's reported byte
 * positions, while the frame header length and both CRCs are computed
 * here with local implementations, not libFLAC's.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "task_support/check.h"
#include "FLAC/format.h"
#include "FLAC/ordinals.h"
#include "FLAC/stream_decoder.h"
#include "FLAC/stream_encoder.h"
#include "FLAC/stream_splice.h"

/***********************************************************************
 *
 * Independent CRC-8 / CRC-16
 *
 ***********************************************************************/

static uint8_t local_crc8_table[256];
static uint16_t local_crc16_table[256];
static int local_crc_ready;

static void local_crc_init(void)
{
	unsigned i, j;
	if(local_crc_ready)
		return;
	for(i = 0; i < 256; i++) {
		uint8_t c8 = (uint8_t)i;
		uint16_t c16 = (uint16_t)(i << 8);
		for(j = 0; j < 8; j++) {
			c8 = (uint8_t)((c8 & 0x80)? (uint8_t)((c8 << 1) ^ 0x07) : (uint8_t)(c8 << 1));
			c16 = (uint16_t)((c16 & 0x8000)? (uint16_t)((c16 << 1) ^ 0x8005) : (uint16_t)(c16 << 1));
		}
		local_crc8_table[i] = c8;
		local_crc16_table[i] = c16;
	}
	local_crc_ready = 1;
}

static uint8_t local_crc8(const uint8_t *data, size_t bytes)
{
	size_t i;
	uint8_t crc = 0;
	local_crc_init();
	for(i = 0; i < bytes; i++)
		crc = local_crc8_table[crc ^ data[i]];
	return crc;
}

static uint16_t local_crc16(const uint8_t *data, size_t bytes)
{
	size_t i;
	uint16_t crc = 0;
	local_crc_init();
	for(i = 0; i < bytes; i++)
		crc = (uint16_t)((crc << 8) ^ local_crc16_table[((crc >> 8) ^ data[i]) & 0xFF]);
	return crc;
}

/***********************************************************************
 *
 * Byte buffers
 *
 ***********************************************************************/

typedef struct {
	FLAC__byte *data;
	size_t bytes, capacity;
} ByteBuffer;

static void buffer_clear(ByteBuffer *b)
{
	if(b->data)
		free(b->data);
	b->data = 0;
	b->bytes = b->capacity = 0;
}

static uint32_t read_be24(const FLAC__byte *p)
{
	return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static uint16_t read_be16(const FLAC__byte *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/***********************************************************************
 *
 * In-memory stream encoder
 *
 ***********************************************************************/

typedef struct {
	ByteBuffer out;
	size_t pos; /* write position, advanced by writes and seeks */
} EncoderSink;

static FLAC__StreamEncoderWriteStatus sink_write(const FLAC__StreamEncoder *encoder, const FLAC__byte buffer[], size_t bytes, uint32_t samples, uint32_t current_frame, void *client_data)
{
	EncoderSink *sink = (EncoderSink*)client_data;
	(void)encoder; (void)samples; (void)current_frame;
	/* Writes normally append; the encoder also rewrites the STREAMINFO
	 * block at the end through the seek callback, so honour pos. */
	if(sink->pos > sink->out.bytes)
		return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
	if(sink->pos + bytes > sink->out.bytes) {
		size_t target = sink->pos + bytes;
		if(target > sink->out.capacity) {
			size_t capacity = sink->out.capacity? sink->out.capacity : 4096;
			FLAC__byte *p;
			while(capacity < target)
				capacity *= 2;
			p = (FLAC__byte*)realloc(sink->out.data, capacity);
			if(!p)
				return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
			sink->out.data = p;
			sink->out.capacity = capacity;
		}
	}
	if(bytes)
		memcpy(sink->out.data + sink->pos, buffer, bytes);
	sink->pos += bytes;
	if(sink->pos > sink->out.bytes)
		sink->out.bytes = sink->pos;
	return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

static FLAC__StreamEncoderSeekStatus sink_seek(const FLAC__StreamEncoder *encoder, FLAC__uint64 absolute_byte_offset, void *client_data)
{
	EncoderSink *sink = (EncoderSink*)client_data;
	(void)encoder;
	if(absolute_byte_offset > (FLAC__uint64)SIZE_MAX)
		return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
	sink->pos = (size_t)absolute_byte_offset;
	return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}

static FLAC__StreamEncoderTellStatus sink_tell(const FLAC__StreamEncoder *encoder, FLAC__uint64 *absolute_byte_offset, void *client_data)
{
	EncoderSink *sink = (EncoderSink*)client_data;
	(void)encoder;
	*absolute_byte_offset = (FLAC__uint64)sink->pos;
	return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

static FLAC__bool encode_stream(const FLAC__int32 *pcm, uint32_t samples, uint32_t channels, uint32_t bits_per_sample, uint32_t sample_rate, uint32_t blocksize, ByteBuffer *out)
{
	FLAC__StreamEncoder *encoder;
	EncoderSink sink;
	FLAC__bool ok;

	memset(&sink, 0, sizeof(sink));
	memset(out, 0, sizeof(*out));

	encoder = FLAC__stream_encoder_new();
	if(!encoder)
		return false;
	ok = FLAC__stream_encoder_set_channels(encoder, channels) &&
	     FLAC__stream_encoder_set_bits_per_sample(encoder, bits_per_sample) &&
	     FLAC__stream_encoder_set_sample_rate(encoder, sample_rate) &&
	     FLAC__stream_encoder_set_blocksize(encoder, blocksize) &&
	     FLAC__stream_encoder_set_compression_level(encoder, 5) &&
	     FLAC__stream_encoder_set_verify(encoder, true) &&
	     FLAC__stream_encoder_set_total_samples_estimate(encoder, samples) &&
	     FLAC__stream_encoder_init_stream(encoder, sink_write, sink_seek, sink_tell, 0, &sink) == FLAC__STREAM_ENCODER_INIT_STATUS_OK &&
	     FLAC__stream_encoder_process_interleaved(encoder, pcm, samples) &&
	     FLAC__stream_encoder_finish(encoder);
	FLAC__stream_encoder_delete(encoder);
	if(!ok) {
		buffer_clear(&sink.out);
		return false;
	}
	*out = sink.out;
	return true;
}

/***********************************************************************
 *
 * In-memory stream decoder
 *
 ***********************************************************************/

typedef struct FrameInfo {
	size_t start, header_end, end;
	uint32_t blocksize;
	FLAC__uint64 sample;
	FLAC__bool variable;
	FLAC__bool crc_header_ok;
	FLAC__bool crc_frame_ok;
	uint32_t channels;
	uint32_t bits_per_sample;
} FrameInfo;

typedef struct {
	const FLAC__byte *data;
	size_t bytes, pos;
	FLAC__int32 *pcm;
	uint32_t samples, capacity, channels;
	FLAC__bool error;
	FLAC__StreamMetadata_StreamInfo info;
	FLAC__bool got_info;
	/* optional frame boundary capture */
	FLAC__bool capture;
	FrameInfo *frames;
	size_t frame_count, frame_capacity;
	size_t audio_offset;
} DecoderState;

static FLAC__StreamDecoderReadStatus mem_read(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
	DecoderState *state = (DecoderState*)client_data;
	size_t remaining;
	(void)decoder;
	if(*bytes == 0)
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	remaining = state->bytes - state->pos;
	if(remaining == 0) {
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	if(*bytes > remaining)
		*bytes = remaining;
	memcpy(buffer, state->data + state->pos, *bytes);
	state->pos += *bytes;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderSeekStatus mem_seek(const FLAC__StreamDecoder *decoder, FLAC__uint64 absolute_byte_offset, void *client_data)
{
	DecoderState *state = (DecoderState*)client_data;
	(void)decoder;
	if(absolute_byte_offset > state->bytes)
		return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
	state->pos = (size_t)absolute_byte_offset;
	return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

static FLAC__StreamDecoderTellStatus mem_tell(const FLAC__StreamDecoder *decoder, FLAC__uint64 *absolute_byte_offset, void *client_data)
{
	DecoderState *state = (DecoderState*)client_data;
	(void)decoder;
	*absolute_byte_offset = (FLAC__uint64)state->pos;
	return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus mem_length(const FLAC__StreamDecoder *decoder, FLAC__uint64 *stream_length, void *client_data)
{
	DecoderState *state = (DecoderState*)client_data;
	(void)decoder;
	*stream_length = (FLAC__uint64)state->bytes;
	return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool mem_eof(const FLAC__StreamDecoder *decoder, void *client_data)
{
	DecoderState *state = (DecoderState*)client_data;
	(void)decoder;
	return state->pos >= state->bytes;
}

/* Parse a FLAC UTF-8 coded number.  Returns false on malformed input. */
static FLAC__bool read_utf8_number(const FLAC__byte *data, size_t avail, size_t *pos, FLAC__uint64 *value)
{
	FLAC__uint64 v;
	uint32_t x, count, i;
	if(*pos >= avail)
		return false;
	x = data[(*pos)++];
	if(!(x & 0x80)) { v = x; count = 0; }
	else if((x & 0xE0) == 0xC0) { v = x & 0x1F; count = 1; }
	else if((x & 0xF0) == 0xE0) { v = x & 0x0F; count = 2; }
	else if((x & 0xF8) == 0xF0) { v = x & 0x07; count = 3; }
	else if((x & 0xFC) == 0xF8) { v = x & 0x03; count = 4; }
	else if((x & 0xFE) == 0xFC) { v = x & 0x01; count = 5; }
	else if(x == 0xFE) { v = 0; count = 6; }
	else return false;
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

/* Parse the frame header located at offset.  Fills in the frame's
 * blocksize, absolute sample number, variable-blocking flag and the
 * independently computed header CRC check. */
static FLAC__bool parse_frame_header(const FLAC__byte *data, size_t bytes, size_t offset, FrameInfo *info)
{
	uint32_t blocksize_code, sample_rate_code, channel_code, bps_code;
	FLAC__uint64 number;
	size_t pos;
	const FLAC__byte *h = data + offset;

	memset(info, 0, sizeof(*info));
	if(offset + 6 > bytes)
		return false;
	if(h[0] != 0xFF || (h[1] & 0xFE) != 0xF8 || (h[1] & 0x02))
		return false;
	info->variable = (h[1] & 1)? true : false;
	blocksize_code = h[2] >> 4;
	sample_rate_code = h[2] & 0x0F;
	channel_code = h[3] >> 4;
	bps_code = (h[3] >> 1) & 0x07;
	if(blocksize_code == 0 || sample_rate_code == 15 || channel_code > 10 || bps_code == 3 || (h[3] & 1))
		return false;
	pos = offset + 4;
	if(!read_utf8_number(data, bytes, &pos, &number))
		return false;
	info->sample = number;

	switch(blocksize_code) {
		case 1: info->blocksize = 192; break;
		case 2: info->blocksize = 576; break;
		case 3: info->blocksize = 1152; break;
		case 4: info->blocksize = 2304; break;
		case 5: info->blocksize = 4608; break;
		case 6:
			if(pos + 1 > bytes) return false;
			info->blocksize = data[pos] + 1;
			pos += 1;
			break;
		case 7:
			if(pos + 2 > bytes) return false;
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
		case 12:
			if(pos + 1 > bytes) return false;
			pos += 1;
			break;
		case 13:
		case 14:
			if(pos + 2 > bytes) return false;
			pos += 2;
			break;
		default:
			break;
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

	info->channels = (channel_code < 8)? channel_code + 1 : 2;
	if(pos >= bytes)
		return false;
	info->start = offset;
	info->header_end = pos + 1;
	info->crc_header_ok = (local_crc8(data + offset, pos - offset) == data[pos]);
	return true;
}

/* Append the frame whose tail is at decode_position end, starting where
 * the previous frame ended.  The header CRC and the frame CRC are
 * checked independently of libFLAC. */
static FLAC__bool capture_frame(DecoderState *state, FLAC__uint64 end, const FLAC__Frame *frame)
{
	FrameInfo info;
	size_t start = state->frame_count? state->frames[state->frame_count - 1].end : state->audio_offset;
	FrameInfo *p;

	if((size_t)end <= start || (size_t)end > state->bytes)
		return false;
	if(!parse_frame_header(state->data, state->bytes, start, &info))
		return false;
	if(info.header_end >= (size_t)end)
		return false;
	{
		uint16_t stored = read_be16(state->data + (size_t)end - 2);
		uint16_t computed = local_crc16(state->data + start, (size_t)end - 2 - start);
		info.crc_frame_ok = (stored == computed);
	}
	info.end = (size_t)end;
	if(info.blocksize != frame->header.blocksize)
		return false;
	if(info.channels != frame->header.channels)
		return false;
	/* Fixed-blocking streams store a frame number in the header; the
	 * absolute sample number is derived from the fixed block size. */
	if(!info.variable) {
		if(!state->got_info || state->info.min_blocksize != state->info.max_blocksize)
			return false;
		info.sample = info.sample * state->info.min_blocksize;
	}
	if(frame->header.number_type != FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER ||
	   info.sample != frame->header.number.sample_number)
		return false;
	if(state->frame_count == state->frame_capacity) {
		size_t capacity = state->frame_capacity? state->frame_capacity * 2 : 32;
		p = (FrameInfo*)realloc(state->frames, capacity * sizeof(FrameInfo));
		if(!p)
			return false;
		state->frames = p;
		state->frame_capacity = capacity;
	}
	state->frames[state->frame_count++] = info;
	return true;
}

static FLAC__StreamDecoderWriteStatus mem_write(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data)
{
	DecoderState *state = (DecoderState*)client_data;
	uint32_t i, c, needed;

	state->channels = frame->header.channels;
	needed = state->samples + frame->header.blocksize;
	if(needed > state->capacity) {
		FLAC__int32 *p = (FLAC__int32*)realloc(state->pcm, sizeof(FLAC__int32) * needed * state->channels);
		if(!p) {
			state->error = true;
			return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
		}
		state->pcm = p;
		state->capacity = needed;
	}
	for(i = 0; i < frame->header.blocksize; i++)
		for(c = 0; c < frame->header.channels; c++)
			state->pcm[(state->samples + i) * state->channels + c] = buffer[c][i];
	state->samples = needed;

	if(state->capture) {
		FLAC__uint64 end = 0;
		if(!FLAC__stream_decoder_get_decode_position(decoder, &end) || !capture_frame(state, end, frame)) {
			state->error = true;
			return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
		}
	}
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void mem_metadata(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
	DecoderState *state = (DecoderState*)client_data;
	(void)decoder;
	if(metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		state->info = metadata->data.stream_info;
		state->got_info = true;
	}
}

static void mem_error(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
	DecoderState *state = (DecoderState*)client_data;
	(void)decoder; (void)status;
	state->error = true;
}

static FLAC__StreamDecoder *decoder_new(const FLAC__byte *data, size_t bytes, DecoderState *state, FLAC__bool md5, FLAC__bool capture)
{
	FLAC__StreamDecoder *d;
	size_t audio_offset = 0;
	FLAC__bool is_last = false;
	size_t pos = 4;

	memset(state, 0, sizeof(*state));
	state->data = data;
	state->bytes = bytes;
	state->capture = capture;

	/* Compute the audio offset independently. */
	if(bytes < 8 || memcmp(data, "fLaC", 4))
		return 0;
	while(!is_last) {
		uint32_t length;
		if(pos + 4 > bytes)
			return 0;
		is_last = (data[pos] & 0x80)? true : false;
		length = read_be24(data + pos + 1);
		pos += 4;
		if(length > bytes - pos)
			return 0;
		pos += length;
	}
	audio_offset = pos;
	state->audio_offset = audio_offset;

	d = FLAC__stream_decoder_new();
	if(!d)
		return 0;
	FLAC__stream_decoder_set_md5_checking(d, md5);
	if(FLAC__stream_decoder_init_stream(d, mem_read, mem_seek, mem_tell, mem_length, mem_eof, mem_write, mem_metadata, mem_error, state) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		FLAC__stream_decoder_delete(d);
		return 0;
	}
	return d;
}

static void state_clear(DecoderState *state)
{
	if(state->pcm)
		free(state->pcm);
	state->pcm = 0;
	if(state->frames)
		free(state->frames);
	state->frames = 0;
	state->frame_count = state->frame_capacity = 0;
}

static FLAC__bool decode_all(const FLAC__byte *data, size_t bytes, DecoderState *state, FLAC__bool *md5_ok)
{
	FLAC__StreamDecoder *d = decoder_new(data, bytes, state, true, false);
	FLAC__bool ok;
	if(!d)
		return false;
	ok = FLAC__stream_decoder_process_until_end_of_stream(d) &&
	     FLAC__stream_decoder_get_state(d) != FLAC__STREAM_DECODER_ABORTED &&
	     FLAC__stream_decoder_get_state(d) != FLAC__STREAM_DECODER_MEMORY_ALLOCATION_ERROR &&
	     FLAC__stream_decoder_get_state(d) != FLAC__STREAM_DECODER_SEEK_ERROR;
	if(md5_ok)
		*md5_ok = FLAC__stream_decoder_finish(d);
	else
		(void)FLAC__stream_decoder_finish(d);
	FLAC__stream_decoder_delete(d);
	return ok && !state->error;
}

/* Decode fully while recording independently parsed frame boundaries. */
static FLAC__bool index_stream(const FLAC__byte *data, size_t bytes, DecoderState *state)
{
	FLAC__StreamDecoder *d = decoder_new(data, bytes, state, false, true);
	FLAC__bool ok;
	if(!d)
		return false;
	ok = FLAC__stream_decoder_process_until_end_of_stream(d) &&
	     FLAC__stream_decoder_get_state(d) != FLAC__STREAM_DECODER_ABORTED &&
	     FLAC__stream_decoder_get_state(d) != FLAC__STREAM_DECODER_MEMORY_ALLOCATION_ERROR &&
	     FLAC__stream_decoder_get_state(d) != FLAC__STREAM_DECODER_SEEK_ERROR;
	(void)FLAC__stream_decoder_finish(d);
	FLAC__stream_decoder_delete(d);
	return ok && !state->error && state->frame_count > 0;
}

/***********************************************************************
 *
 * Source material
 *
 ***********************************************************************/

typedef struct {
	FLAC__int32 *pcm;
	uint32_t samples, channels, bits_per_sample, sample_rate;
	ByteBuffer flac;
} Source;

static FLAC__int32 sample_value(uint32_t index, uint32_t channel, uint32_t seed, uint32_t bits_per_sample, uint32_t kind)
{
	FLAC__int32 limit = (bits_per_sample >= 24)? 8388607 : 32767;
	uint32_t rng = seed * 2654435761u + index * 40503u + channel * 6971u;
	int32_t value;
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	switch((kind + channel) % 3) {
		case 0:
			value = (int32_t)((index * 37 + channel * 11) % 2048) - 1024 + (int32_t)(rng & 7);
			break;
		case 1:
			value = (int32_t)((rng & 0xFFFF) ^ ((rng >> 16) & 0xFFFF)) - 32768;
			break;
		default:
			value = (int32_t)(((index / 13) * 29 + channel * 5) % 4096) - 2048;
			break;
	}
	value = (int32_t)((int64_t)value * limit / 32768);
	if(value > limit)
		value = limit;
	if(value < -limit - 1)
		value = -limit - 1;
	return value;
}

static FLAC__bool make_source(Source *source, uint32_t samples, uint32_t channels, uint32_t bits_per_sample, uint32_t sample_rate, uint32_t blocksize, uint32_t seed, uint32_t kind)
{
	uint32_t i, c;
	memset(source, 0, sizeof(*source));
	if(samples == 0)
		return false;
	source->pcm = (FLAC__int32*)malloc(sizeof(FLAC__int32) * samples * channels);
	if(!source->pcm)
		return false;
	for(i = 0; i < samples; i++)
		for(c = 0; c < channels; c++)
			source->pcm[i * channels + c] = sample_value(i, c, seed, bits_per_sample, kind);
	source->samples = samples;
	source->channels = channels;
	source->bits_per_sample = bits_per_sample;
	source->sample_rate = sample_rate;
	if(!encode_stream(source->pcm, samples, channels, bits_per_sample, sample_rate, blocksize, &source->flac)) {
		free(source->pcm);
		source->pcm = 0;
		return false;
	}
	return true;
}

static void source_clear(Source *source)
{
	if(source->pcm)
		free(source->pcm);
	source->pcm = 0;
	buffer_clear(&source->flac);
}

static void append_range(FLAC__int32 *dest, size_t *pos, const Source *source, uint64_t begin, uint64_t end)
{
	uint64_t i;
	uint32_t c;
	for(i = begin; i < end; i++)
		for(c = 0; c < source->channels; c++)
			dest[(*pos)++] = source->pcm[i * source->channels + c];
}

/***********************************************************************
 *
 * Splice call helpers
 *
 ***********************************************************************/

typedef struct {
	FLAC__SpliceSegment segments[8];
	size_t count;
} SegmentList;

static void segments_reset(SegmentList *list)
{
	memset(list, 0, sizeof(*list));
}

static void segments_add(SegmentList *list, const Source *source, uint64_t begin, uint64_t end)
{
	FLAC__SpliceSegment *s = &list->segments[list->count++];
	s->data = source->flac.data;
	s->bytes = source->flac.bytes;
	s->begin = begin;
	s->end = end;
}

/* Run a splice, storing the result in out, and check the container
 * shape: one fLaC marker and one STREAMINFO block. */
static FLAC__bool splice_and_check(const SegmentList *list, ByteBuffer *out)
{
	FLAC__byte *result = 0;
	size_t bytes = 0;
	FLAC__bool ok = FLAC__stream_splice(list->segments, list->count, &result, &bytes);
	TASK_CHECK(ok);
	TASK_CHECK(result != 0);
	TASK_CHECK(bytes > 8);
	TASK_CHECK(memcmp(result, "fLaC", 4) == 0);
	TASK_CHECK((result[4] & 0x7F) == FLAC__METADATA_TYPE_STREAMINFO);
	TASK_CHECK(read_be24(result + 5) == FLAC__STREAM_METADATA_STREAMINFO_LENGTH);
	/* Only one metadata block: the STREAMINFO must be the last. */
	TASK_CHECK(result[4] & 0x80);
	out->data = result;
	out->bytes = bytes;
	out->capacity = bytes;
	return true;
}

/* Compare the decoded PCM against the expected interleaved signal. */
static FLAC__bool expect_pcm(const DecoderState *state, const FLAC__int32 *expected, size_t samples, uint32_t channels)
{
	size_t i;
	TASK_CHECK(state->samples == samples);
	TASK_CHECK(state->got_info && state->info.total_samples == (FLAC__uint64)samples);
	if(state->samples != samples)
		return false;
	for(i = 0; i < samples * channels; i++)
		TASK_CHECK(state->pcm[i] == expected[i]);
	return true;
}

/***********************************************************************
 *
 * pcm group
 *
 ***********************************************************************/

static int case_pcm_whole_and_trim(void)
{
	static const struct {
		uint32_t channels, bits, blocksize, samples, begin, end;
	} spec[] = {
		{ 1, 16, 1024, 6000, 0, 6000 },
		{ 2, 16, 1024, 6000, 137, 5881 },
		{ 1, 24, 512, 5000, 0, 5000 },
		{ 2, 24, 4096, 9000, 4001, 8503 }
	};
	size_t k;
	for(k = 0; k < sizeof(spec)/sizeof(spec[0]); k++) {
		Source source;
		SegmentList list;
		ByteBuffer out;
		DecoderState state;
		FLAC__int32 *expected;
		size_t count, pos = 0;

		TASK_CHECK(make_source(&source, spec[k].samples, spec[k].channels, spec[k].bits, (k & 1)? 44100 : 48000, spec[k].blocksize, (uint32_t)(201 + k), (uint32_t)k));
		count = (size_t)(spec[k].end - spec[k].begin);
		expected = (FLAC__int32*)malloc(sizeof(FLAC__int32) * count * spec[k].channels);
		TASK_CHECK(expected != 0);
		append_range(expected, &pos, &source, spec[k].begin, spec[k].end);

		segments_reset(&list);
		segments_add(&list, &source, spec[k].begin, spec[k].end);
		TASK_CHECK(splice_and_check(&list, &out));
		TASK_CHECK(decode_all(out.data, out.bytes, &state, 0));
		TASK_CHECK(expect_pcm(&state, expected, count, spec[k].channels));
		TASK_CHECK(state.info.channels == spec[k].channels);
		TASK_CHECK(state.info.bits_per_sample == spec[k].bits);

		state_clear(&state);
		free(expected);
		buffer_clear(&out);
		source_clear(&source);
	}
	return 0;
}

static int case_pcm_sources_order_and_reuse(void)
{
	/* Sources referenced out of order, one of them twice, with edges
	 * unaligned in all of them. */
	Source a, b, c;
	SegmentList list;
	ByteBuffer out;
	DecoderState state;
	FLAC__int32 *expected;
	size_t count, pos = 0;

	TASK_CHECK(make_source(&a, 3000, 1, 24, 44100, 512, 211, 0));
	TASK_CHECK(make_source(&b, 2500, 1, 24, 44100, 1024, 212, 1));
	TASK_CHECK(make_source(&c, 4000, 1, 24, 44100, 2048, 213, 2));

	count = (4000 - 701) + (1500 - 901) + (1000 - 51) + (605 - 6);
	expected = (FLAC__int32*)malloc(sizeof(FLAC__int32) * count);
	TASK_CHECK(expected != 0);

	segments_reset(&list);
	segments_add(&list, &c, 701, 4000);
	segments_add(&list, &a, 901, 1500);
	segments_add(&list, &b, 51, 1000);
	segments_add(&list, &c, 6, 605);
	append_range(expected, &pos, &c, 701, 4000);
	append_range(expected, &pos, &a, 901, 1500);
	append_range(expected, &pos, &b, 51, 1000);
	append_range(expected, &pos, &c, 6, 605);

	TASK_CHECK(splice_and_check(&list, &out));
	TASK_CHECK(decode_all(out.data, out.bytes, &state, 0));
	TASK_CHECK(expect_pcm(&state, expected, count, 1));
	TASK_CHECK(state.info.channels == 1 && state.info.bits_per_sample == 24);

	state_clear(&state);
	free(expected);
	buffer_clear(&out);
	source_clear(&a);
	source_clear(&b);
	source_clear(&c);
	return 0;
}

static int case_pcm_stereo_unaligned(void)
{
	/* Stereo sources whose cut points fall well inside both edge frames,
	 * so the re-encoded blocks differ in length from the originals. */
	Source a, b;
	SegmentList list;
	ByteBuffer out;
	DecoderState state;
	FLAC__int32 *expected;
	size_t count, pos = 0;

	TASK_CHECK(make_source(&a, 7000, 2, 16, 48000, 1024, 221, 1));
	TASK_CHECK(make_source(&b, 5200, 2, 16, 48000, 4096, 222, 2));
	count = (3000 - 88) + (4900 - 2051) + (7000 - 6100);
	expected = (FLAC__int32*)malloc(sizeof(FLAC__int32) * count * 2);
	TASK_CHECK(expected != 0);

	segments_reset(&list);
	segments_add(&list, &a, 88, 3000);
	segments_add(&list, &b, 2051, 4900);
	segments_add(&list, &a, 6100, 7000);
	append_range(expected, &pos, &a, 88, 3000);
	append_range(expected, &pos, &b, 2051, 4900);
	append_range(expected, &pos, &a, 6100, 7000);

	TASK_CHECK(splice_and_check(&list, &out));
	TASK_CHECK(decode_all(out.data, out.bytes, &state, 0));
	TASK_CHECK(expect_pcm(&state, expected, count, 2));

	state_clear(&state);
	free(expected);
	buffer_clear(&out);
	source_clear(&a);
	source_clear(&b);
	return 0;
}

static int case_pcm_short_edges_merge(void)
{
	/* A retained range that leaves fewer than 16 samples at each edge.
	 * The short pieces must be merged with their same-segment neighbours,
	 * and no output block may end up shorter than 16 samples. */
	static const uint64_t begins[] = { 1021, 1013, 4093 };
	static const uint64_t ends[]   = { 6149, 6133, 8201 };
	size_t k;
	for(k = 0; k < sizeof(begins)/sizeof(begins[0]); k++) {
		Source source;
		SegmentList list;
		ByteBuffer out;
		DecoderState state;
		FLAC__int32 *expected;
		size_t count, pos = 0, f;

		TASK_CHECK(make_source(&source, 10000, (k == 1)? 2 : 1, 16, 44100, 1024, (uint32_t)(231 + k), (uint32_t)k));
		TASK_CHECK(ends[k] <= source.samples);
		count = (size_t)(ends[k] - begins[k]);
		expected = (FLAC__int32*)malloc(sizeof(FLAC__int32) * count * source.channels);
		TASK_CHECK(expected != 0);
		append_range(expected, &pos, &source, begins[k], ends[k]);

		segments_reset(&list);
		segments_add(&list, &source, begins[k], ends[k]);
		TASK_CHECK(splice_and_check(&list, &out));
		TASK_CHECK(decode_all(out.data, out.bytes, &state, 0));
		TASK_CHECK(expect_pcm(&state, expected, count, source.channels));

		/* Independently re-index the output frames. */
		state_clear(&state);
		TASK_CHECK(index_stream(out.data, out.bytes, &state));
		TASK_CHECK(state.frame_count >= 1);
		for(f = 0; f < state.frame_count; f++) {
			TASK_CHECK(state.frames[f].blocksize >= 16);
			TASK_CHECK(state.frames[f].crc_header_ok);
			TASK_CHECK(state.frames[f].crc_frame_ok);
		}
		state_clear(&state);
		free(expected);
		buffer_clear(&out);
		source_clear(&source);
	}
	return 0;
}

static int case_pcm_rejections(void)
{
	Source a, other;
	SegmentList list;
	FLAC__byte *result;
	size_t bytes;

	TASK_CHECK(make_source(&a, 4000, 2, 16, 48000, 1024, 241, 0));
	TASK_CHECK(make_source(&other, 4000, 2, 24, 48000, 1024, 242, 0));

	/* Mixed formats in one call. */
	segments_reset(&list);
	segments_add(&list, &a, 0, 4000);
	segments_add(&list, &other, 0, 4000);
	result = (FLAC__byte*)1; bytes = 77;
	TASK_CHECK(!FLAC__stream_splice(list.segments, list.count, &result, &bytes));
	TASK_CHECK(result == 0 && bytes == 0);

	/* Range beyond the declared total samples. */
	segments_reset(&list);
	segments_add(&list, &a, 10, 4001);
	result = (FLAC__byte*)1; bytes = 77;
	TASK_CHECK(!FLAC__stream_splice(list.segments, list.count, &result, &bytes));
	TASK_CHECK(result == 0 && bytes == 0);

	/* Segment shorter than the 16-sample minimum. */
	segments_reset(&list);
	segments_add(&list, &a, 100, 115);
	result = (FLAC__byte*)1; bytes = 77;
	TASK_CHECK(!FLAC__stream_splice(list.segments, list.count, &result, &bytes));
	TASK_CHECK(result == 0 && bytes == 0);

	/* Inverted range. */
	segments_reset(&list);
	segments_add(&list, &a, 500, 400);
	result = (FLAC__byte*)1; bytes = 77;
	TASK_CHECK(!FLAC__stream_splice(list.segments, list.count, &result, &bytes));
	TASK_CHECK(result == 0 && bytes == 0);

	/* No segments at all. */
	result = (FLAC__byte*)1; bytes = 77;
	TASK_CHECK(!FLAC__stream_splice(list.segments, 0, &result, &bytes));
	TASK_CHECK(result == 0 && bytes == 0);

	/* A NULL output pointer must not crash and must fail. */
	result = 0; bytes = 77;
	TASK_CHECK(!FLAC__stream_splice(list.segments, 1, 0, &bytes));
	TASK_CHECK(bytes == 0);

	source_clear(&a);
	source_clear(&other);
	return 0;
}

/***********************************************************************
 *
 * payload group
 *
 ***********************************************************************/

static int case_payload_frames_preserved(void)
{
	Source source;
	SegmentList list;
	ByteBuffer out;
	DecoderState source_index, out_index;
	size_t i, j;

	TASK_CHECK(make_source(&source, 8000, 2, 16, 48000, 1024, 251, 1));
	TASK_CHECK(index_stream(source.flac.data, source.flac.bytes, &source_index));
	TASK_CHECK(source_index.frame_count >= 5);

	segments_reset(&list);
	segments_add(&list, &source,
	              source_index.frames[1].sample + source_index.frames[1].blocksize / 2,
	              source_index.frames[4].sample + 500);
	TASK_CHECK(splice_and_check(&list, &out));
	TASK_CHECK(index_stream(out.data, out.bytes, &out_index));
	TASK_CHECK(out_index.frame_count >= 3);

	/* All output frames are variable-blocking with consecutive absolute
	 * sample numbers, and both CRCs are valid. */
	{
		FLAC__uint64 sample = 0;
		for(i = 0; i < out_index.frame_count; i++) {
			TASK_CHECK(out_index.frames[i].variable);
			TASK_CHECK(out_index.frames[i].sample == sample);
			TASK_CHECK(out_index.frames[i].crc_header_ok);
			TASK_CHECK(out_index.frames[i].crc_frame_ok);
			sample += out_index.frames[i].blocksize;
		}
	}

	/* Interior frames are copied one for one.  Output sample numbers are
	 * cumulative from zero, so map each output frame back to the source
	 * via its position relative to the start of the retained range. */
	{
		FLAC__uint64 begin = source_index.frames[1].sample + source_index.frames[1].blocksize / 2;
		size_t checked_full = 0;
		size_t checked_edges = 0;
		FLAC__uint64 out_pos = 0;
		for(i = 0; i < out_index.frame_count; i++) {
			const FrameInfo *of = &out_index.frames[i];
			FLAC__uint64 source_sample = begin + out_pos;
			out_pos += of->blocksize;
			/* Find the source frame containing source_sample. */
			for(j = 0; j < source_index.frame_count; j++) {
				const FrameInfo *sf = &source_index.frames[j];
				if(source_sample >= sf->sample &&
				   source_sample + of->blocksize <= sf->sample + sf->blocksize &&
				   of->blocksize == sf->blocksize &&
				   source_sample == sf->sample) {
					size_t out_payload = of->end - 2 - of->header_end;
					size_t src_payload = sf->end - 2 - sf->header_end;
					TASK_CHECK(out_payload == src_payload);
					TASK_CHECK(memcmp(out.data + of->header_end,
					                  source.flac.data + sf->header_end,
					                  src_payload) == 0);
					checked_full++;
					break;
				}
			}
			(void)checked_edges;
		}
		/* Frames 2 and 3 of the source are entirely inside the retained
		 * range, so at least those two must have been copied verbatim. */
		TASK_CHECK(checked_full >= 2);
	}

	state_clear(&source_index);
	state_clear(&out_index);
	buffer_clear(&out);
	source_clear(&source);
	return 0;
}

static int case_payload_sample_number_boundaries(void)
{
	Source source;
	SegmentList list;
	ByteBuffer out;
	DecoderState state;
	FLAC__bool crossed_127 = false, crossed_2047 = false;
	FLAC__uint64 sample = 0;
	size_t i;

	/* 16-sample blocks keep the frame count high; the absolute sample
	 * numbers must pass both the 127 and 2047 variable-length thresholds.
	 * A multiple of 16 avoids a final source frame below the minimum. */
	TASK_CHECK(make_source(&source, 5008, 1, 16, 44100, 16, 261, 0));
	segments_reset(&list);
	segments_add(&list, &source, 0, 5008);
	TASK_CHECK(splice_and_check(&list, &out));
	TASK_CHECK(index_stream(out.data, out.bytes, &state));
	TASK_CHECK(state.frame_count >= 300);
	for(i = 0; i < state.frame_count; i++) {
		TASK_CHECK(state.frames[i].variable);
		TASK_CHECK(state.frames[i].sample == sample);
		if(state.frames[i].sample >= 128 && state.frames[i].sample <= 2047)
			crossed_127 = true;
		if(state.frames[i].sample >= 2048)
			crossed_2047 = true;
		sample += state.frames[i].blocksize;
	}
	TASK_CHECK(crossed_127);
	TASK_CHECK(crossed_2047);
	TASK_CHECK(sample == 5008);
	/* Every header and footer CRC must be correct, including for headers
	 * whose sample number occupies more than one UTF-8 byte. */
	for(i = 0; i < state.frame_count; i++) {
		TASK_CHECK(state.frames[i].crc_header_ok);
		TASK_CHECK(state.frames[i].crc_frame_ok);
	}
	state_clear(&state);
	buffer_clear(&out);
	source_clear(&source);
	return 0;
}

/***********************************************************************
 *
 * metadata group
 *
 ***********************************************************************/

static int case_metadata_md5_matches_reference(void)
{
	Source a, b;
	SegmentList list;
	ByteBuffer out, reference;
	FLAC__int32 *expected;
	size_t count, pos = 0, i;
	uint32_t channels = 1;
	DecoderState state;
	FLAC__bool md5_ok = false;

	TASK_CHECK(make_source(&a, 4200, 1, 16, 48000, 1024, 271, 0));
	TASK_CHECK(make_source(&b, 3300, 1, 16, 48000, 2048, 272, 2));
	count = (4200 - 333) + (900 - 51) + (1200 - 7);
	expected = (FLAC__int32*)malloc(sizeof(FLAC__int32) * count * channels);
	TASK_CHECK(expected != 0);

	segments_reset(&list);
	segments_add(&list, &a, 333, 4200);
	segments_add(&list, &b, 51, 900);
	segments_add(&list, &a, 7, 1200);
	append_range(expected, &pos, &a, 333, 4200);
	append_range(expected, &pos, &b, 51, 900);
	append_range(expected, &pos, &a, 7, 1200);

	TASK_CHECK(splice_and_check(&list, &out));
	TASK_CHECK(decode_all(out.data, out.bytes, &state, &md5_ok));
	TASK_CHECK(md5_ok);
	TASK_CHECK(expect_pcm(&state, expected, count, channels));

	/* The stock encoder over the same PCM must produce the same digest. */
	TASK_CHECK(encode_stream(expected, (uint32_t)count, channels, 16, 48000, 1024, &reference));
	{
		DecoderState ref_state;
		TASK_CHECK(decode_all(reference.data, reference.bytes, &ref_state, 0));
		TASK_CHECK(ref_state.info.total_samples == (FLAC__uint64)count);
		TASK_CHECK(memcmp(ref_state.info.md5sum, state.info.md5sum, 16) == 0);
		for(i = 0; i < count * channels; i++)
			TASK_CHECK(ref_state.pcm[i] == expected[i]);
		state_clear(&ref_state);
	}

	state_clear(&state);
	buffer_clear(&reference);
	free(expected);
	buffer_clear(&out);
	source_clear(&a);
	source_clear(&b);
	return 0;
}

static int case_metadata_seek_across_seams(void)
{
	Source a, b;
	SegmentList list;
	ByteBuffer out;
	FLAC__int32 *expected;
	size_t count, pos = 0;
	uint32_t channel_count = 2;
	DecoderState state;
	FLAC__bool md5_ok = false;
	uint32_t seam_positions[4], seams = 0;

	TASK_CHECK(make_source(&a, 5000, 2, 24, 44100, 512, 281, 1));
	TASK_CHECK(make_source(&b, 3800, 2, 24, 44100, 2048, 282, 0));
	count = 2500 + (1990 - 90) + (4200 - 100);
	expected = (FLAC__int32*)malloc(sizeof(FLAC__int32) * count * channel_count);
	TASK_CHECK(expected != 0);

	segments_reset(&list);
	segments_add(&list, &a, 0, 2500);
	segments_add(&list, &b, 90, 1990);
	segments_add(&list, &a, 100, 4200);
	append_range(expected, &pos, &a, 0, 2500);
	append_range(expected, &pos, &b, 90, 1990);
	append_range(expected, &pos, &a, 100, 4200);
	seam_positions[seams++] = 2500;
	seam_positions[seams++] = 2500 + (1990 - 90);
	seam_positions[seams++] = 2499;
	seam_positions[seams++] = 2500 + (1990 - 90) - 1;

	TASK_CHECK(splice_and_check(&list, &out));
	TASK_CHECK(decode_all(out.data, out.bytes, &state, &md5_ok));
	TASK_CHECK(md5_ok);
	TASK_CHECK(expect_pcm(&state, expected, count, channel_count));
	TASK_CHECK(state.info.channels == 2 && state.info.bits_per_sample == 24);
	state_clear(&state);

	/* Seek to the seam neighbourhoods and to random positions, confirming
	 * that the decoded samples match the expected PCM from that absolute
	 * position onward. */
	{
		uint32_t targets[12];
		uint32_t target_count = 0, t;
		uint32_t rng = 0x12345678u;
		for(t = 0; t < seams; t++)
			targets[target_count++] = seam_positions[t];
		while(target_count < sizeof(targets)/sizeof(targets[0])) {
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			targets[target_count++] = rng % (uint32_t)count;
		}
		for(t = 0; t < target_count; t++) {
			FLAC__StreamDecoder *d;
			DecoderState seek_state;
			uint32_t target = targets[t];
			uint32_t compare;

			d = decoder_new(out.data, out.bytes, &seek_state, false, false);
			TASK_CHECK(d != 0);
			TASK_CHECK(FLAC__stream_decoder_seek_absolute(d, target));
			TASK_CHECK(seek_state.samples > 0);
			compare = seek_state.samples;
			if(compare > (uint32_t)count - target)
				compare = (uint32_t)count - target;
			{
				uint32_t s, c;
				for(s = 0; s < compare; s++)
					for(c = 0; c < channel_count; c++)
						TASK_CHECK(seek_state.pcm[s * channel_count + c] ==
						            expected[((size_t)target + s) * channel_count + c]);
			}
			state_clear(&seek_state);
			FLAC__stream_decoder_delete(d);
		}
	}

	free(expected);
	buffer_clear(&out);
	source_clear(&a);
	source_clear(&b);
	return 0;
}

static int case_metadata_rejections(void)
{
	Source a, b;
	SegmentList list;
	FLAC__byte *result;
	size_t bytes;
	ByteBuffer broken;
	size_t streaminfo_body = 8;

	TASK_CHECK(make_source(&a, 4000, 1, 16, 48000, 1024, 291, 0));
	TASK_CHECK(make_source(&b, 4000, 1, 16, 44100, 1024, 292, 0));

	/* Corrupted frame CRC: flip a byte in the middle of the audio. */
	broken.data = (FLAC__byte*)malloc(a.flac.bytes);
	TASK_CHECK(broken.data != 0);
	memcpy(broken.data, a.flac.data, a.flac.bytes);
	broken.bytes = broken.capacity = a.flac.bytes;
	broken.data[broken.bytes / 2] ^= 0x40;
	segments_reset(&list);
	segments_add(&list, &a, 0, 4000);
	list.segments[0].data = broken.data;
	list.segments[0].bytes = broken.bytes;
	result = (FLAC__byte*)1; bytes = 77;
	TASK_CHECK(!FLAC__stream_splice(list.segments, list.count, &result, &bytes));
	TASK_CHECK(result == 0 && bytes == 0);
	buffer_clear(&broken);

	/* Corrupted source MD5 signature. */
	broken.data = (FLAC__byte*)malloc(a.flac.bytes);
	TASK_CHECK(broken.data != 0);
	memcpy(broken.data, a.flac.data, a.flac.bytes);
	broken.bytes = broken.capacity = a.flac.bytes;
	broken.data[streaminfo_body + 18] ^= 0x01;
	segments_reset(&list);
	segments_add(&list, &a, 0, 4000);
	list.segments[0].data = broken.data;
	list.segments[0].bytes = broken.bytes;
	result = (FLAC__byte*)1; bytes = 77;
	TASK_CHECK(!FLAC__stream_splice(list.segments, list.count, &result, &bytes));
	TASK_CHECK(result == 0 && bytes == 0);
	buffer_clear(&broken);

	/* Truncated input. */
	broken.data = (FLAC__byte*)malloc(a.flac.bytes);
	TASK_CHECK(broken.data != 0);
	memcpy(broken.data, a.flac.data, a.flac.bytes);
	broken.bytes = broken.capacity = a.flac.bytes - 8;
	segments_reset(&list);
	segments_add(&list, &a, 0, 4000);
	list.segments[0].data = broken.data;
	list.segments[0].bytes = broken.bytes;
	result = (FLAC__byte*)1; bytes = 77;
	TASK_CHECK(!FLAC__stream_splice(list.segments, list.count, &result, &bytes));
	TASK_CHECK(result == 0 && bytes == 0);
	buffer_clear(&broken);

	/* Inputs with different sample rates. */
	segments_reset(&list);
	segments_add(&list, &a, 0, 4000);
	segments_add(&list, &b, 0, 4000);
	result = (FLAC__byte*)1; bytes = 77;
	TASK_CHECK(!FLAC__stream_splice(list.segments, list.count, &result, &bytes));
	TASK_CHECK(result == 0 && bytes == 0);

	source_clear(&a);
	source_clear(&b);
	return 0;
}

/***********************************************************************
 *
 * Registration
 *
 ***********************************************************************/

const struct task_case task_cases[] = {
	{ "pcm", "whole_and_trim", case_pcm_whole_and_trim },
	{ "pcm", "sources_order_and_reuse", case_pcm_sources_order_and_reuse },
	{ "pcm", "stereo_unaligned", case_pcm_stereo_unaligned },
	{ "pcm", "short_edges_merge", case_pcm_short_edges_merge },
	{ "pcm", "rejections", case_pcm_rejections },
	{ "payload", "frames_preserved", case_payload_frames_preserved },
	{ "payload", "sample_number_boundaries", case_payload_sample_number_boundaries },
	{ "metadata", "md5_matches_reference", case_metadata_md5_matches_reference },
	{ "metadata", "seek_across_seams", case_metadata_seek_across_seams },
	{ "metadata", "rejections", case_metadata_rejections }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
