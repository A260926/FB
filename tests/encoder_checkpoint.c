/* Tests for the persistent stream-encoder checkpoint interface.
 *
 * Each case builds a "virtual file" in memory, encodes a deterministic
 * signal, optionally saves/reloads a checkpoint, and verifies the result:
 *  - byte-for-byte equality with an uninterrupted encode,
 *  - a real libFLAC decode (MD5 checking on) matching the source PCM,
 *  - seeks to both sides of the save point compared against the PCM.
 *
 * Cases are registered in task_cases[] under the groups "buffered",
 * "container" and "process"; task_support/driver.c runs them.  TASK_CHECK
 * from task_support/check.h records content comparisons.
 */
#include "task_support/check.h"

#include <FLAC/format.h>
#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#include <FLAC/stream_encoder_checkpoint.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 4099
#define CAPACITY (1u << 20)

/* ------------------------------------------------------------------ */
/* Deterministic PCM and an in-memory output sink.                    */

typedef struct {
	FLAC__byte *data;
	size_t pos;
	size_t size;
	unsigned writes;
	unsigned seeks;
} sink;

static int32_t sample_value(unsigned frame, unsigned channel)
{
	return (int32_t)(((frame * 37u + (frame % 19u) * 193u + channel * 7001u) % 60001u)) - 30000;
}

static FLAC__StreamEncoderWriteStatus sink_write(const FLAC__StreamEncoder *encoder, const FLAC__byte buffer[], size_t bytes, uint32_t samples, uint32_t current_frame, void *client_data)
{
	sink *s = (sink *)client_data;
	(void)encoder; (void)samples; (void)current_frame;
	if(bytes > CAPACITY - s->pos)
		return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
	memcpy(s->data + s->pos, buffer, bytes);
	s->pos += bytes;
	if(s->pos > s->size)
		s->size = s->pos;
	s->writes++;
	return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

static FLAC__StreamEncoderSeekStatus sink_seek(const FLAC__StreamEncoder *encoder, FLAC__uint64 offset, void *client_data)
{
	sink *s = (sink *)client_data;
	(void)encoder;
	if(offset > s->size)
		return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
	s->pos = (size_t)offset;
	s->seeks++;
	return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}

static FLAC__StreamEncoderTellStatus sink_tell(const FLAC__StreamEncoder *encoder, FLAC__uint64 *offset, void *client_data)
{
	(void)encoder;
	*offset = (FLAC__uint64)((sink *)client_data)->pos;
	return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Encoder construction.                                              */

typedef struct {
	unsigned blocksize;
	unsigned channels;
	unsigned bits;
	unsigned rate;
	unsigned seekpoints; /* 0 = no SEEKTABLE */
} config;

static FLAC__StreamMetadata *make_seektable(const config *c)
{
	FLAC__StreamMetadata *table = FLAC__metadata_object_new(FLAC__METADATA_TYPE_SEEKTABLE);
	unsigned i;
	if(table == NULL)
		return NULL;
	if(!FLAC__metadata_object_seektable_resize_points(table, c->seekpoints)) {
		FLAC__metadata_object_delete(table);
		return NULL;
	}
	for(i = 0; i < c->seekpoints; i++) {
		table->data.seek_table.points[i].sample_number = (FLAC__uint64)i * c->blocksize * 4u;
		table->data.seek_table.points[i].stream_offset = 0;
		table->data.seek_table.points[i].frame_samples = 0;
	}
	return table;
}

static FLAC__StreamEncoder *make_encoder(const config *c, sink *s, FLAC__StreamMetadata **seektable)
{
	FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
	FLAC__StreamMetadata *list[1];
	*seektable = NULL;
	if(e == NULL)
		return NULL;
	if(!FLAC__stream_encoder_set_compression_level(e, 5) ||
	   !FLAC__stream_encoder_set_channels(e, c->channels) ||
	   !FLAC__stream_encoder_set_bits_per_sample(e, c->bits) ||
	   !FLAC__stream_encoder_set_sample_rate(e, c->rate) ||
	   !FLAC__stream_encoder_set_blocksize(e, c->blocksize) ||
	   !FLAC__stream_encoder_set_verify(e, false)) {
		FLAC__stream_encoder_delete(e);
		return NULL;
	}
	if(c->seekpoints) {
		*seektable = make_seektable(c);
		if(*seektable == NULL) {
			FLAC__stream_encoder_delete(e);
			return NULL;
		}
		list[0] = *seektable;
		if(!FLAC__stream_encoder_set_metadata(e, list, 1)) {
			FLAC__metadata_object_delete(*seektable);
			*seektable = NULL;
			FLAC__stream_encoder_delete(e);
			return NULL;
		}
	}
	if(FLAC__stream_encoder_init_stream(e, sink_write, sink_seek, sink_tell, NULL, s) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
		if(*seektable)
			FLAC__metadata_object_delete(*seektable);
		*seektable = NULL;
		FLAC__stream_encoder_delete(e);
		return NULL;
	}
	return e;
}

/* Feed [start, start+count) of the deterministic signal. */
static int feed(FLAC__StreamEncoder *e, unsigned start, unsigned count, const config *c, int planar)
{
	FLAC__int32 *interleaved;
	unsigned i, ch;
	int ok;
	if(count == 0)
		return 1;
	interleaved = (FLAC__int32 *)malloc(sizeof(FLAC__int32) * count * c->channels);
	if(interleaved == NULL)
		return 0;
	for(i = 0; i < count; i++)
		for(ch = 0; ch < c->channels; ch++)
			interleaved[i * c->channels + ch] = sample_value(start + i, ch);
	if(planar) {
		FLAC__int32 *planes[2] = { NULL, NULL };
		const FLAC__int32 *buffers[2];
		for(ch = 0; ch < c->channels; ch++) {
			planes[ch] = (FLAC__int32 *)malloc(sizeof(FLAC__int32) * count);
			if(planes[ch] == NULL) {
				for(i = 0; i < ch; i++)
					free(planes[i]);
				free(interleaved);
				return 0;
			}
			for(i = 0; i < count; i++)
				planes[ch][i] = interleaved[i * c->channels + ch];
			buffers[ch] = planes[ch];
		}
		ok = (int)FLAC__stream_encoder_process(e, buffers, count);
		for(ch = 0; ch < c->channels; ch++)
			free(planes[ch]);
	}
	else {
		ok = (int)FLAC__stream_encoder_process_interleaved(e, interleaved, count);
	}
	free(interleaved);
	return ok;
}

/* ------------------------------------------------------------------ */
/* Reference encode.                                                  */

static FLAC__byte *reference_encode(const config *c, size_t *out_size)
{
	sink s;
	FLAC__StreamMetadata *seektable;
	FLAC__StreamEncoder *e;
	FLAC__byte *copy;
	memset(&s, 0, sizeof(s));
	s.data = (FLAC__byte *)calloc(1, CAPACITY);
	if(s.data == NULL)
		return NULL;
	e = make_encoder(c, &s, &seektable);
	if(e == NULL) {
		free(s.data);
		return NULL;
	}
	if(!feed(e, 0, MAX_FRAMES, c, 0) || !FLAC__stream_encoder_finish(e)) {
		FLAC__stream_encoder_delete(e);
		if(seektable)
			FLAC__metadata_object_delete(seektable);
		free(s.data);
		return NULL;
	}
	FLAC__stream_encoder_delete(e);
	if(seektable)
		FLAC__metadata_object_delete(seektable);
	copy = (FLAC__byte *)malloc(s.size ? s.size : 1);
	if(copy)
		memcpy(copy, s.data, s.size);
	*out_size = s.size;
	free(s.data);
	return copy;
}

/* ------------------------------------------------------------------ */
/* Decoding and seeking.                                              */

typedef struct {
	const FLAC__byte *data;
	size_t size;
	size_t pos;
	unsigned channels;
	unsigned frames;
	unsigned base;
	unsigned bad;
	unsigned errors;
	unsigned md5_nonzero;
	unsigned long long total_samples;
	FLAC__uint64 last_position;
} decoder_state;

static FLAC__StreamDecoderReadStatus dec_read(const FLAC__StreamDecoder *d, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
	decoder_state *s = (decoder_state *)client_data;
	size_t left = s->size - s->pos;
	(void)d;
	if(*bytes > left)
		*bytes = left;
	if(*bytes == 0)
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	memcpy(buffer, s->data + s->pos, *bytes);
	s->pos += *bytes;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderSeekStatus dec_seek(const FLAC__StreamDecoder *d, FLAC__uint64 offset, void *client_data)
{
	decoder_state *s = (decoder_state *)client_data;
	(void)d;
	if(offset > s->size)
		return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
	s->pos = (size_t)offset;
	return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

static FLAC__StreamDecoderTellStatus dec_tell(const FLAC__StreamDecoder *d, FLAC__uint64 *offset, void *client_data)
{
	(void)d;
	*offset = (FLAC__uint64)((decoder_state *)client_data)->pos;
	return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus dec_length(const FLAC__StreamDecoder *d, FLAC__uint64 *length, void *client_data)
{
	(void)d;
	*length = (FLAC__uint64)((decoder_state *)client_data)->size;
	return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool dec_eof(const FLAC__StreamDecoder *d, void *client_data)
{
	(void)d;
	return ((decoder_state *)client_data)->pos >= ((decoder_state *)client_data)->size;
}

static FLAC__StreamDecoderWriteStatus dec_write(const FLAC__StreamDecoder *d, const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *client_data)
{
	decoder_state *s = (decoder_state *)client_data;
	unsigned i, ch;
	(void)d;
	if(frame->header.channels != s->channels)
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	for(i = 0; i < frame->header.blocksize; i++) {
		for(ch = 0; ch < s->channels; ch++) {
			unsigned index = s->base + s->frames + i;
			if(index >= MAX_FRAMES || buffer[ch][i] != sample_value(index, ch))
				s->bad++;
		}
	}
	s->frames += frame->header.blocksize;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void dec_metadata(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *metadata, void *client_data)
{
	decoder_state *s = (decoder_state *)client_data;
	unsigned i;
	(void)d;
	if(metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		s->total_samples = metadata->data.stream_info.total_samples;
		for(i = 0; i < 16; i++)
			s->md5_nonzero |= metadata->data.stream_info.md5sum[i];
	}
}

static void dec_error(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
	(void)d; (void)status;
	((decoder_state *)client_data)->errors++;
}

static FLAC__StreamDecoder *decoder_new(decoder_state *s, const config *c, unsigned base)
{
	FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
	if(d == NULL)
		return NULL;
	memset(s, 0, sizeof(*s));
	s->data = NULL; /* filled by caller */
	s->channels = c->channels;
	s->base = base;
	if(!FLAC__stream_decoder_set_md5_checking(d, 1) ||
	   FLAC__stream_decoder_init_stream(d, dec_read, dec_seek, dec_tell, dec_length, dec_eof,
	                                    dec_write, dec_metadata, dec_error, s) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		FLAC__stream_decoder_delete(d);
		return NULL;
	}
	return d;
}

static FLAC__StreamDecoder *decoder_start(decoder_state *s, const config *c, const FLAC__byte *data, size_t size, unsigned base)
{
	FLAC__StreamDecoder *d = decoder_new(s, c, base);
	if(d == NULL)
		return NULL;
	s->data = data;
	s->size = size;
	s->pos = 0;
	return d;
}

/* Full decode: PCM identity and STREAMINFO total. */
static int verify_stream(const FLAC__byte *data, size_t size, const config *c, unsigned expected_total)
{
	decoder_state s;
	FLAC__StreamDecoder *d;
	int ok;
	d = decoder_start(&s, c, data, size, 0);
	if(d == NULL)
		return 0;
	ok = FLAC__stream_decoder_process_until_end_of_stream(d) &&
	     FLAC__stream_decoder_finish(d) &&
	     s.frames == expected_total &&
	     s.total_samples == expected_total &&
	     s.bad == 0 && s.errors == 0 && s.md5_nonzero != 0;
	FLAC__stream_decoder_delete(d);
	return ok;
}

/* Seek to a sample and decode the frame containing it. */
static int verify_seek(const FLAC__byte *data, size_t size, const config *c, unsigned target)
{
	decoder_state s;
	FLAC__StreamDecoder *d;
	int ok;
	d = decoder_start(&s, c, data, size, target);
	if(d == NULL)
		return 0;
	ok = FLAC__stream_decoder_seek_absolute(d, target) &&
	     FLAC__stream_decoder_get_decode_position(d, &s.last_position) &&
	     FLAC__stream_decoder_process_single(d) &&
	     FLAC__stream_decoder_finish(d) &&
	     s.frames > 0 && s.bad == 0 && s.errors == 0 && s.last_position <= size;
	FLAC__stream_decoder_delete(d);
	return ok;
}

/* ------------------------------------------------------------------ */
/* Checkpoint helpers.                                                */

/* CRC-32 (IEEE) used only by the test to make mutated checkpoints otherwise
 * well-formed, so the resume path's field-consistency checks are exercised
 * rather than just its CRC check. */
static FLAC__uint32 test_crc32(const FLAC__byte *data, size_t len)
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

static void test_refresh_crc(FLAC__byte *buffer, size_t size)
{
	FLAC__uint32 crc = test_crc32(buffer, size - 4);
	buffer[size - 4] = (FLAC__byte)(crc & 0xff);
	buffer[size - 3] = (FLAC__byte)((crc >> 8) & 0xff);
	buffer[size - 2] = (FLAC__byte)((crc >> 16) & 0xff);
	buffer[size - 1] = (FLAC__byte)((crc >> 24) & 0xff);
}

static FLAC__byte *save_checkpoint(FLAC__StreamEncoder *e, size_t *size){
	size_t needed = FLAC__stream_encoder_checkpoint_size(e);
	FLAC__byte *buffer;
	if(needed == 0)
		return NULL;
	buffer = (FLAC__byte *)malloc(needed);
	if(buffer == NULL)
		return NULL;
	if(!FLAC__stream_encoder_checkpoint_write(e, buffer, needed)) {
		free(buffer);
		return NULL;
	}
	*size = needed;
	return buffer;
}

static FLAC__byte *capture_prefix(const sink *s, size_t *size)
{
	FLAC__byte *copy = (FLAC__byte *)malloc(s->size ? s->size : 1);
	if(copy == NULL)
		return NULL;
	memcpy(copy, s->data, s->size);
	*size = s->size;
	return copy;
}

static FLAC__StreamEncoder *resume(sink *s, const FLAC__byte *state, size_t size)
{
	return FLAC__stream_encoder_checkpoint_resume(state, size, (FLAC__uint64)s->pos,
	                                              sink_write, sink_seek, sink_tell, NULL, s);
}

/* ------------------------------------------------------------------ */
/* Split-encode driver.                                               */

typedef struct {
	config cfg;
	unsigned split;      /* samples fed before the first save */
	unsigned split2;     /* additional samples before the second save */
	int planar_first;    /* feed the pre-save chunk planarly */
	int chain;           /* save/resume twice */
	unsigned consume;    /* samples to feed in the first post-resume chunk */
} scenario;

/* Returns 0 on success, 1 on failure; TASK_CHECK is used for the content
 * assertions so the driver counts them. */
static int run_scenario(const scenario *sc)
{
	size_t ref_size = 0, state_size = 0, state2_size = 0, prefix_size = 0;
	FLAC__byte *ref = reference_encode(&sc->cfg, &ref_size);
	FLAC__byte *state = NULL, *state2 = NULL, *prefix = NULL, *prefix2 = NULL;
	FLAC__StreamMetadata *seektable = NULL;
	FLAC__StreamEncoder *e = NULL, *r = NULL;
	sink s;
	int ok = 0;
	size_t position;

	if(ref == NULL)
		return 1;

	/* Branch A: encode the prefix, save, destroy the encoder. */
	memset(&s, 0, sizeof(s));
	s.data = (FLAC__byte *)calloc(1, CAPACITY);
	if(s.data == NULL)
		goto done;
	e = make_encoder(&sc->cfg, &s, &seektable);
	if(e == NULL)
		goto done;
	if(!feed(e, 0, sc->split, &sc->cfg, sc->planar_first))
		goto done;
	state = save_checkpoint(e, &state_size);
	if(state == NULL)
		goto done;
	/* Saving twice is byte identical, does not advance and calls no I/O. */
	{
		unsigned writes = s.writes, seeks = s.seeks;
		size_t again_size = 0;
		FLAC__byte *again = save_checkpoint(e, &again_size);
		if(again == NULL || again_size != state_size || memcmp(again, state, state_size) != 0) {
			free(again);
			goto done;
		}
		free(again);
		if(s.writes != writes || s.seeks != seeks)
			goto done;
	}
	if(FLAC__stream_encoder_checkpoint_size(e) != state_size)
		goto done;
	position = s.pos;
	prefix = capture_prefix(&s, &prefix_size);
	FLAC__stream_encoder_delete(e);
	e = NULL;
	if(prefix == NULL || prefix_size != position)
		goto done;

	/* Branch B (independent): resume from only the prefix. */
	memset(&s, 0, sizeof(s));
	s.data = (FLAC__byte *)calloc(1, CAPACITY);
	if(s.data == NULL)
		goto done;
	memcpy(s.data, prefix, prefix_size);
	s.pos = s.size = prefix_size;
	r = resume(&s, state, state_size);
	if(r == NULL)
		goto done;
	/* Resume itself must not have written anything. */
	if(s.pos != prefix_size || s.writes != 0 || s.seeks != 0)
		goto done;

	if(sc->chain) {
		if(!feed(r, sc->split, sc->split2, &sc->cfg, 0))
			goto done;
		state2 = save_checkpoint(r, &state2_size);
		if(state2 == NULL)
			goto done;
		prefix2 = capture_prefix(&s, &prefix_size);
		FLAC__stream_encoder_delete(r);
		r = NULL;
		if(prefix2 == NULL)
			goto done;
		memset(&s, 0, sizeof(s));
		s.data = (FLAC__byte *)calloc(1, CAPACITY);
		if(s.data == NULL)
			goto done;
		memcpy(s.data, prefix2, prefix_size);
		s.pos = s.size = prefix_size;
		free(prefix2);
		prefix2 = NULL;
		r = resume(&s, state2, state2_size);
		if(r == NULL)
			goto done;
		if(!feed(r, sc->split + sc->split2, sc->consume, &sc->cfg, 0))
			goto done;
		if(sc->split + sc->split2 + sc->consume < MAX_FRAMES &&
		   !feed(r, sc->split + sc->split2 + sc->consume,
		         MAX_FRAMES - (sc->split + sc->split2 + sc->consume), &sc->cfg, 0))
			goto done;
	}
	else {
		if(!feed(r, sc->split, sc->consume, &sc->cfg, 0))
			goto done;
		if(sc->split + sc->consume < MAX_FRAMES &&
		   !feed(r, sc->split + sc->consume, MAX_FRAMES - (sc->split + sc->consume), &sc->cfg, 0))
			goto done;
	}

	if(!FLAC__stream_encoder_finish(r))
		goto done;
	FLAC__stream_encoder_delete(r);
	r = NULL;

	/* The result must be byte-for-byte the uninterrupted encode. */
	TASK_CHECK(s.size == ref_size);
	TASK_CHECK(s.size > 0 && memcmp(s.data, ref, ref_size) == 0);
	/* Real decode with MD5 checking, and STREAMINFO total. */
	TASK_CHECK(verify_stream(s.data, s.size, &sc->cfg, MAX_FRAMES));
	/* Seeks around the save point, compared to the source PCM. */
	{
		unsigned point = sc->split;
		unsigned nearby[4];
		unsigned i;
		nearby[0] = point > 0 ? point - 1 : 0;
		nearby[1] = point;
		nearby[2] = point + 1 < MAX_FRAMES ? point + 1 : MAX_FRAMES - 1;
		nearby[3] = point + sc->cfg.blocksize < MAX_FRAMES ? point + sc->cfg.blocksize : MAX_FRAMES - 2;
		for(i = 0; i < 4; i++)
			TASK_CHECK(verify_seek(s.data, s.size, &sc->cfg, nearby[i]));
	}
	ok = 1;

done:
	if(e)
		FLAC__stream_encoder_delete(e);
	if(r)
		FLAC__stream_encoder_delete(r);
	if(seektable)
		FLAC__metadata_object_delete(seektable);
	free(ref);
	free(state);
	free(state2);
	free(prefix);
	free(prefix2);
	free(s.data);
	return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Rejection tests.                                                   */

static int test_rejections(void)
{
	config c = { 256, 2, 16, 48000, 0 };
	sink s;
	FLAC__StreamMetadata *seektable = NULL;
	FLAC__StreamEncoder *e = NULL;
	FLAC__byte *state = NULL, *mut = NULL, *ref = NULL, *prefix = NULL;
	size_t state_size = 0, ref_size = 0, prefix_size = 0;
	int ok = 0, i;

	memset(&s, 0, sizeof(s));
	s.data = (FLAC__byte *)calloc(1, CAPACITY);
	if(s.data == NULL)
		return 1;
	ref = reference_encode(&c, &ref_size);
	if(ref == NULL)
		goto done;
	e = make_encoder(&c, &s, &seektable);
	if(e == NULL)
		goto done;
	if(!feed(e, 0, 300, &c, 0))
		goto done;
	state = save_checkpoint(e, &state_size);
	if(state == NULL)
		goto done;
	mut = (FLAC__byte *)malloc(state_size);
	if(mut == NULL)
		goto done;
	/* Keep the genuine prefix for the "original still works" check. */
	prefix = capture_prefix(&s, &prefix_size);
	if(prefix == NULL)
		goto done;

	for(i = 0; i < 9; i++) {
		FLAC__StreamEncoder *bad;
		sink t;
		FLAC__uint64 position = (FLAC__uint64)s.pos;
		memset(&t, 0, sizeof(t));
		t.data = (FLAC__byte *)calloc(1, CAPACITY); /* isolated buffer */
		if(t.data == NULL)
			goto done;
		t.pos = t.size = prefix_size;
		memcpy(t.data, prefix, prefix_size);
		memcpy(mut, state, state_size);
		switch(i) {
			case 0: bad = FLAC__stream_encoder_checkpoint_resume(state, state_size, position + 1,
			            sink_write, sink_seek, sink_tell, NULL, &t); break;   /* wrong offset */
			case 1: bad = FLAC__stream_encoder_checkpoint_resume(state, state_size - 1, position,
			            sink_write, sink_seek, sink_tell, NULL, &t); break;   /* truncated */
			case 2: mut[8] = (FLAC__byte)99;                                  /* bad version */
			        bad = FLAC__stream_encoder_checkpoint_resume(mut, state_size, position,
			            sink_write, sink_seek, sink_tell, NULL, &t); break;
			case 3: mut[0] ^= 0xff;                                           /* bad magic */
			        bad = FLAC__stream_encoder_checkpoint_resume(mut, state_size, position,
			            sink_write, sink_seek, sink_tell, NULL, &t); break;
			case 4: mut[state_size / 2] ^= 0x01;                               /* CRC mismatch */
			        bad = FLAC__stream_encoder_checkpoint_resume(mut, state_size, position,
			            sink_write, sink_seek, sink_tell, NULL, &t); break;
			case 5: /* contradictory total size, CRC made valid */
			        mut[12] = (FLAC__byte)((state_size + 4) & 0xff);
			        test_refresh_crc(mut, state_size);
			        bad = FLAC__stream_encoder_checkpoint_resume(mut, state_size, position,
			            sink_write, sink_seek, sink_tell, NULL, &t); break;
			case 6: /* unsupported bits-per-sample with a valid CRC */
			        mut[24] = 8;
			        test_refresh_crc(mut, state_size);
			        bad = FLAC__stream_encoder_checkpoint_resume(mut, state_size, position,
			            sink_write, sink_seek, sink_tell, NULL, &t); break;
			case 7: /* buffered count contradicts pending byte count, valid CRC */
			        mut[84] = (FLAC__byte)((mut[84] + 1) & 0xff);
			        test_refresh_crc(mut, state_size);
			        bad = FLAC__stream_encoder_checkpoint_resume(mut, state_size, position,
			            sink_write, sink_seek, sink_tell, NULL, &t); break;
			default: /* flags set an unknown bit, valid CRC */
			        mut[16] = 0x03;
			        test_refresh_crc(mut, state_size);
			        bad = FLAC__stream_encoder_checkpoint_resume(mut, state_size, position,
			            sink_write, sink_seek, sink_tell, NULL, &t); break;
		}
		TASK_CHECK(bad == NULL);        /* every corrupt checkpoint is refused */
		TASK_CHECK(t.writes == 0);      /* and nothing is written */
		TASK_CHECK(t.pos == position);  /* nor is the position moved */
		if(bad)
			FLAC__stream_encoder_delete(bad);
		free(t.data);
	}

	/* The original encoder is unharmed: it can still finish the stream. */
	TASK_CHECK(feed(e, 300, MAX_FRAMES - 300, &c, 0));
	TASK_CHECK(FLAC__stream_encoder_finish(e));
	FLAC__stream_encoder_delete(e);
	e = NULL;
	TASK_CHECK(s.size == ref_size);
	TASK_CHECK(memcmp(s.data, ref, ref_size) == 0);
	TASK_CHECK(verify_stream(s.data, s.size, &c, MAX_FRAMES));
	ok = 1;

done:
	if(e)
		FLAC__stream_encoder_delete(e);
	if(seektable)
		FLAC__metadata_object_delete(seektable);
	free(state);
	free(mut);
	free(ref);
	free(prefix);
	free(s.data);
	return ok ? 0 : 1;
}

/* Two independent encoders resumed from the same bytes, interleaved, freed in
 * opposite orders.  Both must produce the reference stream. */
static int test_two_instances(void)
{
	config c = { 1024, 2, 24, 44100, 0 };
	sink a, b;
	FLAC__StreamMetadata *sa = NULL, *sb = NULL;
	FLAC__StreamEncoder *ea = NULL, *ra = NULL, *rb = NULL;
	FLAC__byte *state = NULL, *prefix = NULL, *ref = NULL;
	size_t state_size = 0, prefix_size = 0, ref_size = 0;
	int ok = 0;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	ref = reference_encode(&c, &ref_size);
	if(ref == NULL)
		goto done;

	a.data = (FLAC__byte *)calloc(1, CAPACITY);
	if(a.data == NULL)
		goto done;
	ea = make_encoder(&c, &a, &sa);
	if(ea == NULL)
		goto done;
	if(!feed(ea, 0, 1500, &c, 0))
		goto done;
	state = save_checkpoint(ea, &state_size);
	if(state == NULL)
		goto done;
	prefix = capture_prefix(&a, &prefix_size);
	FLAC__stream_encoder_delete(ea);
	ea = NULL;
	if(prefix == NULL)
		goto done;

	free(a.data);
	a.data = (FLAC__byte *)calloc(1, CAPACITY);
	b.data = (FLAC__byte *)calloc(1, CAPACITY);
	if(a.data == NULL || b.data == NULL)
		goto done;
	memcpy(a.data, prefix, prefix_size);
	memcpy(b.data, prefix, prefix_size);
	a.pos = a.size = b.pos = b.size = prefix_size;
	ra = resume(&a, state, state_size);
	rb = resume(&b, state, state_size);
	if(ra == NULL || rb == NULL)
		goto done;

	/* Interleave feeding in different chunk sizes; each encoder sees the
	 * whole suffix, just delivered in its own chunk pattern. */
	{
		unsigned ca = 1500, cb = 1500;
		while(ca < MAX_FRAMES || cb < MAX_FRAMES) {
			if(ca < MAX_FRAMES) {
				unsigned n = MAX_FRAMES - ca;
				if(n > 37) n = 37;
				if(!feed(ra, ca, n, &c, 0))
					goto done;
				ca += n;
			}
			if(cb < MAX_FRAMES) {
				unsigned n = MAX_FRAMES - cb;
				if(n > 53) n = 53;
				if(!feed(rb, cb, n, &c, 0))
					goto done;
				cb += n;
			}
		}
	}
	if(!FLAC__stream_encoder_finish(ra) || !FLAC__stream_encoder_finish(rb))
		goto done;
	/* Free in opposite order to the creation order. */
	FLAC__stream_encoder_delete(ra);
	ra = NULL;
	FLAC__stream_encoder_delete(rb);
	rb = NULL;

	TASK_CHECK(a.size == ref_size);
	TASK_CHECK(b.size == ref_size);
	TASK_CHECK(a.size > 0 && memcmp(a.data, ref, ref_size) == 0);
	TASK_CHECK(b.size > 0 && memcmp(b.data, ref, ref_size) == 0);
	TASK_CHECK(a.size == b.size && memcmp(a.data, b.data, a.size) == 0);
	TASK_CHECK(verify_stream(a.data, a.size, &c, MAX_FRAMES));
	ok = 1;

done:
	if(ea)
		FLAC__stream_encoder_delete(ea);
	if(ra)
		FLAC__stream_encoder_delete(ra);
	if(rb)
		FLAC__stream_encoder_delete(rb);
	if(sa)
		FLAC__metadata_object_delete(sa);
	if(sb)
		FLAC__metadata_object_delete(sb);
	free(state);
	free(prefix);
	free(ref);
	free(a.data);
	free(b.data);
	return ok ? 0 : 1;
}

/* An unsupported configuration reports size 0, refuses to write, yet still
 * encodes normally through the classic interface. */
static int test_unsupported(void)
{
	config c = { 1152, 2, 16, 44100, 0 }; /* blocksize 1152 is not supported */
	sink s;
	FLAC__StreamMetadata *seektable = NULL;
	FLAC__StreamEncoder *e = NULL;
	FLAC__byte dummy[16];
	int ok = 0;

	memset(&s, 0, sizeof(s));
	s.data = (FLAC__byte *)calloc(1, CAPACITY);
	if(s.data == NULL)
		return 1;
	e = make_encoder(&c, &s, &seektable);
	if(e == NULL)
		goto done;
	if(!feed(e, 0, 200, &c, 0))
		goto done;
	TASK_CHECK(FLAC__stream_encoder_checkpoint_size(e) == 0);
	TASK_CHECK(!FLAC__stream_encoder_checkpoint_write(e, dummy, sizeof(dummy)));
	/* A blocksize of 1152 is fine through the classic interface. */
	if(!feed(e, 200, MAX_FRAMES - 200, &c, 0))
		goto done;
	TASK_CHECK(FLAC__stream_encoder_finish(e));
	FLAC__stream_encoder_delete(e);
	e = NULL;
	TASK_CHECK(verify_stream(s.data, s.size, &c, MAX_FRAMES));
	ok = 1;

done:
	if(e)
		FLAC__stream_encoder_delete(e);
	if(seektable)
		FLAC__metadata_object_delete(seektable);
	free(s.data);
	return ok ? 0 : 1;
}

/* State size depends only on configuration, buffered samples and seek points,
 * not on how much audio has already been encoded. */
static int test_size_independence(void)
{
	config c = { 1024, 2, 16, 48000, 8 };
	sink s;
	FLAC__StreamMetadata *seektable = NULL;
	FLAC__StreamEncoder *e = NULL;
	size_t first, later, plus_one;
	int ok = 0;

	memset(&s, 0, sizeof(s));
	s.data = (FLAC__byte *)calloc(1, CAPACITY);
	if(s.data == NULL)
		return 1;
	e = make_encoder(&c, &s, &seektable);
	if(e == NULL)
		goto done;
	if(!feed(e, 0, 100, &c, 0))
		goto done;
	first = FLAC__stream_encoder_checkpoint_size(e);
	if(!feed(e, 100, 2048, &c, 0))
		goto done;
	later = FLAC__stream_encoder_checkpoint_size(e);
	/* 100 and 2148 both leave 100 buffered samples and the same seek table. */
	TASK_CHECK(first > 0);
	TASK_CHECK(first == later);
	/* One more buffered sample grows the state by one sample per channel. */
	if(!feed(e, 2148, 1, &c, 0))
		goto done;
	plus_one = FLAC__stream_encoder_checkpoint_size(e);
	TASK_CHECK(plus_one == first + c.channels * 4u);
	ok = 1;

done:
	if(e)
		FLAC__stream_encoder_delete(e);
	if(seektable)
		FLAC__metadata_object_delete(seektable);
	free(s.data);
	return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Registered cases.                                                  */

/* The three save positions: before a full frame, exactly one full block, and
 * one full block plus the internal look-ahead sample. */

static int buffered_partial_block(void)
{
	scenario sc;
	memset(&sc, 0, sizeof(sc));
	sc.cfg.blocksize = 1024;
	sc.cfg.channels = 2;
	sc.cfg.bits = 16;
	sc.cfg.rate = 44100;
	sc.split = 300;
	sc.planar_first = 1;
	sc.consume = MAX_FRAMES - 300;
	return run_scenario(&sc);
}

static int buffered_exact_full_block(void)
{
	scenario sc;
	memset(&sc, 0, sizeof(sc));
	sc.cfg.blocksize = 256;
	sc.cfg.channels = 1;
	sc.cfg.bits = 16;
	sc.cfg.rate = 48000;
	sc.split = 1024;
	sc.consume = MAX_FRAMES - 1024;
	return run_scenario(&sc);
}

static int buffered_lookahead_sample(void)
{
	scenario sc;
	memset(&sc, 0, sizeof(sc));
	sc.cfg.blocksize = 256;
	sc.cfg.channels = 2;
	sc.cfg.bits = 24;
	sc.cfg.rate = 48000;
	sc.split = 1025;
	sc.planar_first = 1;
	sc.consume = MAX_FRAMES - 1025;
	return run_scenario(&sc);
}

static int container_seektable(void)
{
	scenario sc;
	memset(&sc, 0, sizeof(sc));
	sc.cfg.blocksize = 4096;
	sc.cfg.channels = 2;
	sc.cfg.bits = 16;
	sc.cfg.rate = 44100;
	sc.cfg.seekpoints = 8;
	sc.split = 2048;
	sc.consume = MAX_FRAMES - 2048;
	return run_scenario(&sc);
}

static int container_seektable_max(void)
{
	scenario sc;
	memset(&sc, 0, sizeof(sc));
	sc.cfg.blocksize = 256;
	sc.cfg.channels = 2;
	sc.cfg.bits = 16;
	sc.cfg.rate = 48000;
	sc.cfg.seekpoints = 64;
	sc.split = 257;
	sc.consume = MAX_FRAMES - 257;
	return run_scenario(&sc);
}

static int container_stereo_24bit(void)
{
	scenario sc;
	memset(&sc, 0, sizeof(sc));
	sc.cfg.blocksize = 1024;
	sc.cfg.channels = 2;
	sc.cfg.bits = 24;
	sc.cfg.rate = 48000;
	sc.cfg.seekpoints = 16;
	sc.split = 4096;
	sc.planar_first = 1;
	sc.consume = MAX_FRAMES - 4096;
	return run_scenario(&sc);
}

static int container_mono_24bit(void)
{
	scenario sc;
	memset(&sc, 0, sizeof(sc));
	sc.cfg.blocksize = 256;
	sc.cfg.channels = 1;
	sc.cfg.bits = 24;
	sc.cfg.rate = 44100;
	sc.cfg.seekpoints = 4;
	sc.split = 257;
	sc.consume = MAX_FRAMES - 257;
	return run_scenario(&sc);
}

static int process_mixed_feeding(void)
{
	scenario sc;
	memset(&sc, 0, sizeof(sc));
	sc.cfg.blocksize = 256;
	sc.cfg.channels = 2;
	sc.cfg.bits = 16;
	sc.cfg.rate = 48000;
	sc.split = 300;
	sc.planar_first = 1;
	sc.consume = 1000;
	return run_scenario(&sc);
}

static int process_repeated_save(void)
{
	scenario sc;
	memset(&sc, 0, sizeof(sc));
	sc.cfg.blocksize = 1024;
	sc.cfg.channels = 2;
	sc.cfg.bits = 16;
	sc.cfg.rate = 48000;
	sc.split = 300;
	sc.split2 = 700;
	sc.chain = 1;
	sc.consume = MAX_FRAMES - 1000;
	return run_scenario(&sc);
}

const struct task_case task_cases[] = {
	{ "buffered",  "partial_block",    buffered_partial_block },
	{ "buffered",  "exact_full_block", buffered_exact_full_block },
	{ "buffered",  "lookahead_sample", buffered_lookahead_sample },
	{ "container", "seektable",        container_seektable },
	{ "container", "seektable_max",    container_seektable_max },
	{ "container", "stereo_24bit",     container_stereo_24bit },
	{ "container", "mono_24bit",       container_mono_24bit },
	{ "process",   "mixed_feeding",    process_mixed_feeding },
	{ "process",   "repeated_save",    process_repeated_save },
	{ "process",   "rejections",       test_rejections },
	{ "process",   "two_instances",    test_two_instances },
	{ "process",   "unsupported",      test_unsupported },
	{ "process",   "size_independence",test_size_independence }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

/* task_support/driver.c provides main(). */
