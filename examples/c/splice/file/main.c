/* example_c_splice - Lossless FLAC stream splicing with libFLAC
 * Copyright (C) 2025  Xiph.Org Foundation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * This example shows how to use FLAC__stream_splice() to assemble a new
 * native FLAC file from retained ranges of two existing FLAC files.
 *
 * The segment table describes what to take and in which order.  Each
 * FLAC__SpliceSegment refers to one complete input FLAC stream held in
 * memory and selects the half-open range [begin, end) of per-channel
 * sample frames from it.  The inputs are not modified and the caller
 * keeps ownership of their buffers; the new file is returned as one
 * allocated buffer which the caller releases with
 * FLAC__stream_splice_free().
 *
 * Only whole frames inside a range are copied compressed.  The frames
 * cut by a range edge are re-encoded, so the result is lossless without
 * requiring a full decode/encode cycle.
 *
 * Usage: example_c_splice first.flac second.flac out.flac
 *
 * The example reads both inputs into memory and takes the whole first
 * stream followed by the second half of the second stream.  Real
 * applications that already hold their archives in memory can pass
 * those buffers directly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FLAC/stream_decoder.h"
#include "FLAC/stream_splice.h"

static FLAC__byte *read_file(const char *filename, size_t *bytes)
{
	FILE *file = fopen(filename, "rb");
	FLAC__byte *data;
	long length;

	if(!file) {
		fprintf(stderr, "ERROR: opening %s\n", filename);
		return 0;
	}
	if(fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
		fprintf(stderr, "ERROR: sizing %s\n", filename);
		fclose(file);
		return 0;
	}
	data = (FLAC__byte*)malloc((size_t)length);
	if(!data) {
		fprintf(stderr, "ERROR: allocating %ld bytes for %s\n", length, filename);
		fclose(file);
		return 0;
	}
	if(fread(data, 1, (size_t)length, file) != (size_t)length) {
		fprintf(stderr, "ERROR: reading %s\n", filename);
		free(data);
		fclose(file);
		return 0;
	}
	fclose(file);
	*bytes = (size_t)length;
	return data;
}

/* Decoder callbacks used only to reach the STREAMINFO metadata. */
typedef struct {
	const FLAC__byte *data;
	size_t bytes, pos;
	FLAC__StreamMetadata_StreamInfo info;
	FLAC__bool got_info;
} InfoState;

static FLAC__StreamDecoderReadStatus info_read(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
	InfoState *state = (InfoState*)client_data;
	size_t remaining;
	(void)decoder;
	remaining = state->bytes - state->pos;
	if(*bytes == 0 || remaining == 0) {
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	if(*bytes > remaining)
		*bytes = remaining;
	memcpy(buffer, state->data + state->pos, *bytes);
	state->pos += *bytes;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static void info_metadata(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
	InfoState *state = (InfoState*)client_data;
	(void)decoder;
	if(metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		state->info = metadata->data.stream_info;
		state->got_info = true;
	}
}

static void info_error(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
	(void)decoder; (void)status; (void)client_data;
}

/* A write callback is required by the decoder even though this example
 * only needs the metadata; audio frames are simply discarded. */
static FLAC__StreamDecoderWriteStatus info_write(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data)
{
	(void)decoder; (void)frame; (void)buffer; (void)client_data;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static FLAC__bool stream_info(const FLAC__byte *data, size_t bytes, FLAC__StreamMetadata_StreamInfo *info)
{
	InfoState state;
	FLAC__StreamDecoder *decoder;
	FLAC__bool ok;

	memset(&state, 0, sizeof(state));
	state.data = data;
	state.bytes = bytes;
	decoder = FLAC__stream_decoder_new();
	if(!decoder)
		return false;
	if(FLAC__stream_decoder_init_stream(decoder, info_read, 0, 0, 0, 0, info_write, info_metadata, info_error, &state) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		FLAC__stream_decoder_delete(decoder);
		return false;
	}
	ok = FLAC__stream_decoder_process_until_end_of_metadata(decoder);
	(void)FLAC__stream_decoder_finish(decoder);
	FLAC__stream_decoder_delete(decoder);
	if(!ok || !state.got_info)
		return false;
	*info = state.info;
	return true;
}

int main(int argc, char *argv[])
{
	FLAC__byte *first_data = 0, *second_data = 0;
	size_t first_bytes = 0, second_bytes = 0;
	FLAC__StreamMetadata_StreamInfo first_info, second_info;
	FLAC__SpliceSegment segments[2];
	FLAC__byte *output = 0;
	size_t output_bytes = 0;
	FILE *file;
	FLAC__bool ok;

	if(argc != 4) {
		fprintf(stderr, "usage: %s first.flac second.flac out.flac\n", argv[0]);
		return 1;
	}

	first_data = read_file(argv[1], &first_bytes);
	second_data = read_file(argv[2], &second_bytes);
	if(!first_data || !second_data) {
		free(first_data);
		free(second_data);
		return 1;
	}
	if(!stream_info(first_data, first_bytes, &first_info) ||
	   !stream_info(second_data, second_bytes, &second_info)) {
		fprintf(stderr, "ERROR: could not read STREAMINFO from an input\n");
		free(first_data);
		free(second_data);
		return 1;
	}

	/*
	 * The ranges are given in sample frames and must lie within each
	 * input's accurate total sample count.  A single input may be
	 * referenced by several segments, and segments may appear in any
	 * order.  Here the whole first stream is followed by the second
	 * half of the second stream.
	 */
	segments[0].data = first_data;
	segments[0].bytes = first_bytes;
	segments[0].begin = 0;
	segments[0].end = first_info.total_samples;

	segments[1].data = second_data;
	segments[1].bytes = second_bytes;
	segments[1].begin = second_info.total_samples / 2;
	segments[1].end = second_info.total_samples;

	ok = FLAC__stream_splice(segments, 2, &output, &output_bytes);
	if(!ok) {
		fprintf(stderr, "ERROR: splicing failed; the inputs must share the same\n"
		                "       44100/48000 Hz, 1 or 2 channel, 16/24-bit format.\n");
		free(first_data);
		free(second_data);
		return 1;
	}

	file = fopen(argv[3], "wb");
	if(!file) {
		fprintf(stderr, "ERROR: opening %s for output\n", argv[3]);
		FLAC__stream_splice_free(output);
		free(first_data);
		free(second_data);
		return 1;
	}
	if(fwrite(output, 1, output_bytes, file) != output_bytes) {
		fprintf(stderr, "ERROR: writing %s\n", argv[3]);
		fclose(file);
		FLAC__stream_splice_free(output);
		free(first_data);
		free(second_data);
		return 1;
	}
	fclose(file);

	fprintf(stderr, "spliced %zu bytes into %s\n", output_bytes, argv[3]);

	/* The spliced buffer belongs to the caller once splicing succeeds. */
	FLAC__stream_splice_free(output);
	/* The input buffers remain the caller's responsibility. */
	free(first_data);
	free(second_data);
	return 0;
}
