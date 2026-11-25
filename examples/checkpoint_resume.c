/* checkpoint_resume.c - example of persisting and resuming a FLAC encoder.
 *
 * Usage:
 *   checkpoint-example save   <workdir>
 *   checkpoint-example resume <workdir>
 *   checkpoint-example verify <workdir>
 *
 * save    encodes a deterministic signal, stops mid-stream, and writes
 *         checkpoint.bin (encoder state), stream.flac (the unfinished FLAC
 *         stream up to the save point) and suffix.pcm (the remaining raw
 *         samples).  It then exits without finishing the stream.
 * resume  runs in a fresh process: it reads only checkpoint.bin and
 *         suffix.pcm, reopens stream.flac at the saved byte position and
 *         finishes appending the remaining audio.
 * verify  independently encodes the same signal from scratch to
 *         reference.flac, compares the two FLAC files byte for byte, then
 *         decodes stream.flac with the real decoder and checks MD5 and the
 *         sample values.  Only on success does it write verified.txt.
 *
 * The example never reads the source PCM before the save point during
 * resume; it only continues from the persisted state.
 */

#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#include <FLAC/stream_encoder_checkpoint.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixed demonstration configuration (a subset supported by checkpoints). */
enum {
	TOTAL_FRAMES = 4099,
	CHANNELS = 2,
	BITS = 16,
	SAMPLE_RATE = 48000,
	BLOCK_SIZE = 256,
	SPLIT_FRAME = 300,      /* save point: inside the second block */
	SEEKPOINTS = 8
};

#define CHECKPOINT_FILE "checkpoint.bin"
#define STREAM_FILE     "stream.flac"
#define SUFFIX_FILE     "suffix.pcm"
#define REFERENCE_FILE  "reference.flac"
#define VERIFIED_FILE   "verified.txt"

static int32_t sample_value(unsigned frame, unsigned channel)
{
	return (int32_t)(((frame * 37u + (frame % 19u) * 193u + channel * 7001u) % 60001u)) - 30000;
}

static void path_join(char *out, size_t cap, const char *dir, const char *name)
{
	snprintf(out, cap, "%s/%s", dir, name);
}

static FILE *open_file(const char *dir, const char *name, const char *mode, char *path, size_t cap)
{
	path_join(path, cap, dir, name);
	return fopen(path, mode);
}

/* Remove artifacts this example produced on a previous run.  What is removed
 * depends on the subcommand: resume and verify must keep the files save
 * produced. */
static void cleanup_artifacts(const char *dir, const char *mode)
{
	char path[4096];
	if(strcmp(mode, "save") == 0) {
		path_join(path, sizeof(path), dir, CHECKPOINT_FILE); remove(path);
		path_join(path, sizeof(path), dir, STREAM_FILE); remove(path);
		path_join(path, sizeof(path), dir, SUFFIX_FILE); remove(path);
	}
	/* verify regenerates these, and any success marker is stale. */
	path_join(path, sizeof(path), dir, REFERENCE_FILE); remove(path);
	path_join(path, sizeof(path), dir, VERIFIED_FILE); remove(path);
}

/* ------------------------------------------------------------------ */
/* Raw suffix PCM: interleaved native 16-bit little endian.            */

static int write_suffix(const char *dir, unsigned start, unsigned count)
{
	char path[4096];
	FILE *f = open_file(dir, SUFFIX_FILE, "wb", path, sizeof(path));
	unsigned i, ch;
	if(f == NULL)
		return 0;
	for(i = 0; i < count; i++) {
		for(ch = 0; ch < CHANNELS; ch++) {
			int32_t v = sample_value(start + i, ch);
			unsigned char b[2];
			b[0] = (unsigned char)(v & 0xff);
			b[1] = (unsigned char)((v >> 8) & 0xff);
			if(fwrite(b, 1, 2, f) != 2) {
				fclose(f);
				return 0;
			}
		}
	}
	return fclose(f) == 0;
}

/* ------------------------------------------------------------------ */
/* Encoder callbacks backed by a stdio file.                           */

typedef struct {
	FILE *file;
	unsigned writes;
	unsigned seeks;
} file_sink;

static FLAC__StreamEncoderWriteStatus file_write(const FLAC__StreamEncoder *encoder, const FLAC__byte buffer[], size_t bytes, uint32_t samples, uint32_t current_frame, void *client_data)
{
	file_sink *s = (file_sink *)client_data;
	(void)encoder; (void)samples; (void)current_frame;
	if(fwrite(buffer, 1, bytes, s->file) != bytes)
		return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
	s->writes++;
	return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

static FLAC__StreamEncoderSeekStatus file_seek(const FLAC__StreamEncoder *encoder, FLAC__uint64 offset, void *client_data)
{
	file_sink *s = (file_sink *)client_data;
	(void)encoder;
	if(fseeko(s->file, (off_t)offset, SEEK_SET) != 0)
		return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
	s->seeks++;
	return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}

static FLAC__StreamEncoderTellStatus file_tell(const FLAC__StreamEncoder *encoder, FLAC__uint64 *offset, void *client_data)
{
	file_sink *s = (file_sink *)client_data;
	off_t p;
	(void)encoder;
	p = ftello(s->file);
	if(p < 0)
		return FLAC__STREAM_ENCODER_TELL_STATUS_ERROR;
	*offset = (FLAC__uint64)p;
	return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

static FLAC__StreamEncoder *build_encoder(file_sink *sink, FLAC__StreamMetadata **seektable)
{
	FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
	FLAC__StreamMetadata *list[1];
	unsigned i;
	if(e == NULL)
		return NULL;
	if(!FLAC__stream_encoder_set_compression_level(e, 5) ||
	   !FLAC__stream_encoder_set_channels(e, CHANNELS) ||
	   !FLAC__stream_encoder_set_bits_per_sample(e, BITS) ||
	   !FLAC__stream_encoder_set_sample_rate(e, SAMPLE_RATE) ||
	   !FLAC__stream_encoder_set_blocksize(e, BLOCK_SIZE) ||
	   !FLAC__stream_encoder_set_verify(e, false)) {
		FLAC__stream_encoder_delete(e);
		return NULL;
	}
	*seektable = FLAC__metadata_object_new(FLAC__METADATA_TYPE_SEEKTABLE);
	if(*seektable == NULL ||
	   !FLAC__metadata_object_seektable_resize_points(*seektable, SEEKPOINTS)) {
		if(*seektable)
			FLAC__metadata_object_delete(*seektable);
		FLAC__stream_encoder_delete(e);
		*seektable = NULL;
		return NULL;
	}
	for(i = 0; i < SEEKPOINTS; i++) {
		(*seektable)->data.seek_table.points[i].sample_number = (FLAC__uint64)i * BLOCK_SIZE * 4u;
		(*seektable)->data.seek_table.points[i].stream_offset = 0;
		(*seektable)->data.seek_table.points[i].frame_samples = 0;
	}
	list[0] = *seektable;
	if(!FLAC__stream_encoder_set_metadata(e, list, 1) ||
	   FLAC__stream_encoder_init_stream(e, file_write, file_seek, file_tell, NULL, sink) !=
	   FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
		FLAC__metadata_object_delete(*seektable);
		*seektable = NULL;
		FLAC__stream_encoder_delete(e);
		return NULL;
	}
	return e;
}

static int feed(FLAC__StreamEncoder *e, unsigned start, unsigned count)
{
	FLAC__int32 *pcm;
	unsigned i, ch;
	if(count == 0)
		return 1;
	pcm = (FLAC__int32 *)malloc(sizeof(FLAC__int32) * count * CHANNELS);
	if(pcm == NULL)
		return 0;
	for(i = 0; i < count; i++)
		for(ch = 0; ch < CHANNELS; ch++)
			pcm[i * CHANNELS + ch] = sample_value(start + i, ch);
	i = FLAC__stream_encoder_process_interleaved(e, pcm, count);
	free(pcm);
	return (int)i;
}

/* ------------------------------------------------------------------ */
/* save                                                               */

static int do_save(const char *dir)
{
	char stream_path[4096], checkpoint_path[4096];
	file_sink sink;
	FLAC__StreamMetadata *seektable = NULL;
	FLAC__StreamEncoder *e = NULL;
	FLAC__byte *state = NULL;
	size_t state_size;
	FILE *checkpoint;
	int ok = 0;

	memset(&sink, 0, sizeof(sink));
	sink.file = open_file(dir, STREAM_FILE, "w+b", stream_path, sizeof(stream_path));
	if(sink.file == NULL)
		return 0;
	e = build_encoder(&sink, &seektable);
	if(e == NULL)
		goto done;
	if(!feed(e, 0, SPLIT_FRAME))
		goto done;

	state_size = FLAC__stream_encoder_checkpoint_size(e);
	if(state_size == 0)
		goto done;
	state = (FLAC__byte *)malloc(state_size);
	if(state == NULL)
		goto done;
	if(!FLAC__stream_encoder_checkpoint_write(e, state, state_size))
		goto done;

	checkpoint = open_file(dir, CHECKPOINT_FILE, "wb", checkpoint_path, sizeof(checkpoint_path));
	if(checkpoint == NULL)
		goto done;
	if(fwrite(state, 1, state_size, checkpoint) != state_size || fclose(checkpoint) != 0)
		goto done;

	if(fflush(sink.file) != 0)
		goto done;
	if(!write_suffix(dir, SPLIT_FRAME, TOTAL_FRAMES - SPLIT_FRAME))
		goto done;

	ok = 1;

done:
	/* The stream must stay unfinished: never call finish() or delete() on the
	 * encoder here, since delete() finishes and would append the trailer.
	 * Just close the file and exit; the OS reclaims the encoder. */
	if(sink.file)
		fclose(sink.file);
	free(state);
	if(seektable)
		FLAC__metadata_object_delete(seektable);
	return ok;
}

/* ------------------------------------------------------------------ */
/* resume                                                             */

static int do_resume(const char *dir)
{
	char stream_path[4096], checkpoint_path[4096];
	file_sink sink;
	FLAC__StreamMetadata *seektable = NULL;
	FLAC__StreamEncoder *e = NULL;
	FLAC__byte *state = NULL;
	size_t state_size = 0, capacity = 0, got;
	FLAC__uint64 expected;
	FILE *checkpoint;
	int ok = 0;
	off_t file_size;

	/* Read the checkpoint. */
	checkpoint = open_file(dir, CHECKPOINT_FILE, "rb", checkpoint_path, sizeof(checkpoint_path));
	if(checkpoint == NULL)
		return 0;
	if(fseeko(checkpoint, 0, SEEK_END) != 0) {
		fclose(checkpoint);
		return 0;
	}
	file_size = ftello(checkpoint);
	if(file_size <= 0 || fseeko(checkpoint, 0, SEEK_SET) != 0) {
		fclose(checkpoint);
		return 0;
	}
	capacity = (size_t)file_size;
	state = (FLAC__byte *)malloc(capacity);
	if(state == NULL) {
		fclose(checkpoint);
		return 0;
	}
	got = fread(state, 1, capacity, checkpoint);
	fclose(checkpoint);
	if(got != capacity)
		goto done;
	state_size = capacity;

	/* Open the unfinished stream and position it at the saved end. */
	memset(&sink, 0, sizeof(sink));
	sink.file = open_file(dir, STREAM_FILE, "r+b", stream_path, sizeof(stream_path));
	if(sink.file == NULL)
		goto done;
	if(fseeko(sink.file, 0, SEEK_END) != 0)
		goto done;
	expected = (FLAC__uint64)ftello(sink.file);

	e = FLAC__stream_encoder_checkpoint_resume(state, state_size, expected,
	                                           file_write, file_seek, file_tell, NULL, &sink);
	if(e == NULL)
		goto done;

	/* Feed the remaining PCM from suffix.pcm.  This is the only audio input
	 * this process reads; the samples before the save point are not available
	 * to it. */
	{
		char path[4096];
		FILE *suffix = open_file(dir, SUFFIX_FILE, "rb", path, sizeof(path));
		unsigned long long frames_fed = 0;
		if(suffix == NULL)
			goto done;
		for(;;) {
			unsigned char raw[2 * 2 * 256];
			size_t bytes = fread(raw, 1, sizeof(raw), suffix);
			size_t frames;
			FLAC__int32 pcm[2 * 256];
			unsigned i, ch;
			if(bytes % (2 * CHANNELS) != 0) {
				fclose(suffix);
				goto done;
			}
			frames = bytes / (2 * CHANNELS);
			if(frames == 0) {
				fclose(suffix);
				break;
			}
			for(i = 0; i < frames; i++) {
				for(ch = 0; ch < CHANNELS; ch++) {
					const unsigned char *b = raw + (i * CHANNELS + ch) * 2;
					int32_t v = (int32_t)((uint16_t)(b[0] | (b[1] << 8)));
					if(v >= 0x8000)
						v -= 0x10000;
					pcm[i * CHANNELS + ch] = v;
				}
			}
			if(!FLAC__stream_encoder_process_interleaved(e, pcm, (uint32_t)frames)) {
				fclose(suffix);
				goto done;
			}
			frames_fed += frames;
		}
		if(frames_fed != (unsigned long long)(TOTAL_FRAMES - SPLIT_FRAME))
			goto done;
	}

	if(!FLAC__stream_encoder_finish(e))
		goto done;
	FLAC__stream_encoder_delete(e);
	e = NULL;
	ok = 1;

done:
	if(e)
		FLAC__stream_encoder_delete(e);
	if(sink.file)
		fclose(sink.file);
	free(state);
	(void)seektable;
	return ok;
}

/* ------------------------------------------------------------------ */
/* verify                                                             */

static int read_file(const char *path, FLAC__byte **data, size_t *size)
{
	FILE *f = fopen(path, "rb");
	long length;
	FLAC__byte *buffer;
	if(f == NULL)
		return 0;
	if(fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return 0;
	}
	length = ftell(f);
	if(length < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return 0;
	}
	buffer = (FLAC__byte *)malloc((size_t)length ? (size_t)length : 1);
	if(buffer == NULL) {
		fclose(f);
		return 0;
	}
	if(fread(buffer, 1, (size_t)length, f) != (size_t)length) {
		free(buffer);
		fclose(f);
		return 0;
	}
	fclose(f);
	*data = buffer;
	*size = (size_t)length;
	return 1;
}

static int make_reference(const char *dir)
{
	char path[4096];
	file_sink sink;
	FLAC__StreamMetadata *seektable = NULL;
	FLAC__StreamEncoder *e = NULL;
	int ok = 0;

	memset(&sink, 0, sizeof(sink));
	sink.file = open_file(dir, REFERENCE_FILE, "w+b", path, sizeof(path));
	if(sink.file == NULL)
		return 0;
	e = build_encoder(&sink, &seektable);
	if(e == NULL)
		goto done;
	if(!feed(e, 0, TOTAL_FRAMES))
		goto done;
	if(!FLAC__stream_encoder_finish(e))
		goto done;
	FLAC__stream_encoder_delete(e);
	e = NULL;
	ok = 1;

done:
	if(e)
		FLAC__stream_encoder_delete(e);
	if(sink.file)
		fclose(sink.file);
	if(seektable)
		FLAC__metadata_object_delete(seektable);
	return ok;
}

/* Real decode with MD5 checking and sample comparison against the generator. */
typedef struct {
	const FLAC__byte *data;
	size_t size, pos;
	unsigned frames;
	unsigned bad;
	unsigned errors;
	unsigned md5_nonzero;
} verify_state;

static FLAC__StreamDecoderReadStatus v_read(const FLAC__StreamDecoder *d, FLAC__byte out[], size_t *n, void *u)
{
	verify_state *s = (verify_state *)u;
	size_t left = s->size - s->pos;
	(void)d;
	if(*n > left)
		*n = left;
	if(*n == 0)
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	memcpy(out, s->data + s->pos, *n);
	s->pos += *n;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus v_write(const FLAC__StreamDecoder *d, const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *u)
{
	verify_state *s = (verify_state *)u;
	unsigned i, ch;
	(void)d;
	if(frame->header.channels != CHANNELS)
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	for(i = 0; i < frame->header.blocksize; i++)
		for(ch = 0; ch < CHANNELS; ch++)
			if(s->frames + i >= TOTAL_FRAMES ||
			   buffer[ch][i] != sample_value(s->frames + i, ch))
				s->bad++;
	s->frames += frame->header.blocksize;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void v_metadata(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *u)
{
	verify_state *s = (verify_state *)u;
	unsigned i;
	(void)d;
	if(m->type == FLAC__METADATA_TYPE_STREAMINFO) {
		if(m->data.stream_info.total_samples != TOTAL_FRAMES)
			s->bad++;
		for(i = 0; i < 16; i++)
			s->md5_nonzero |= m->data.stream_info.md5sum[i];
	}
}

static void v_error(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus st, void *u)
{
	(void)d; (void)st;
	((verify_state *)u)->errors++;
}

static int decode_and_check(const FLAC__byte *data, size_t size)
{
	verify_state s;
	FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
	int ok;
	if(d == NULL)
		return 0;
	memset(&s, 0, sizeof(s));
	s.data = data;
	s.size = size;
	if(!FLAC__stream_decoder_set_md5_checking(d, 1) ||
	   FLAC__stream_decoder_init_stream(d, v_read, NULL, NULL, NULL, NULL,
	                                    v_write, v_metadata, v_error, &s) !=
	   FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		FLAC__stream_decoder_delete(d);
		return 0;
	}
	ok = FLAC__stream_decoder_process_until_end_of_stream(d) &&
	     FLAC__stream_decoder_finish(d) &&
	     s.frames == TOTAL_FRAMES && s.bad == 0 && s.errors == 0 &&
	     s.md5_nonzero != 0;
	FLAC__stream_decoder_delete(d);
	return ok;
}

static int do_verify(const char *dir)
{
	char stream_path[4096], reference_path[4096], verified_path[4096];
	FLAC__byte *stream = NULL, *reference = NULL;
	size_t stream_size = 0, reference_size = 0;
	int ok = 0;

	if(!make_reference(dir))
		return 0;
	path_join(stream_path, sizeof(stream_path), dir, STREAM_FILE);
	path_join(reference_path, sizeof(reference_path), dir, REFERENCE_FILE);
	if(!read_file(stream_path, &stream, &stream_size))
		goto done;
	if(!read_file(reference_path, &reference, &reference_size))
		goto done;
	if(stream_size == 0 || stream_size != reference_size)
		goto done;
	if(memcmp(stream, reference, stream_size) != 0)
		goto done;
	if(!decode_and_check(stream, stream_size))
		goto done;

	{
		FILE *f;
		path_join(verified_path, sizeof(verified_path), dir, VERIFIED_FILE);
		f = fopen(verified_path, "wb");
		if(f == NULL)
			goto done;
		if(fprintf(f, "verified bytes=%zu frames=%u sha-equal=1 md5=ok\n",
		           stream_size, (unsigned)TOTAL_FRAMES) <= 0 || fclose(f) != 0)
			goto done;
	}
	ok = 1;

done:
	free(stream);
	free(reference);
	return ok;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	if(argc != 3) {
		fprintf(stderr, "usage: %s save|resume|verify <workdir>\n", argv[0]);
		return 2;
	}
	cleanup_artifacts(argv[2], argv[1]);
	if(strcmp(argv[1], "save") == 0) {
		if(!do_save(argv[2])) { fprintf(stderr, "save failed\n"); return 1; }
		printf("SAVE ok split=%u\n", (unsigned)SPLIT_FRAME);
		return 0;
	}
	if(strcmp(argv[1], "resume") == 0) {
		if(!do_resume(argv[2])) { fprintf(stderr, "resume failed\n"); return 1; }
		printf("RESUME ok\n");
		return 0;
	}
	if(strcmp(argv[1], "verify") == 0) {
		if(!do_verify(argv[2])) { fprintf(stderr, "verify failed\n"); return 1; }
		printf("VERIFY ok\n");
		return 0;
	}
	fprintf(stderr, "unknown subcommand: %s\n", argv[1]);
	return 2;
}
