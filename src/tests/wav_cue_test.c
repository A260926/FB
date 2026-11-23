/*
	wav_cue_test.c: regression tests for WAV chapter marks (out123_wav_cue)

	The tests build WAV files through the public out123 API and inspect the
	generated RIFF bytes independently, checking chunk layout, PCM content,
	chapter positions and labels.
*/

#include "task_support/check.h"
#include "out123.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRIES 64

struct cue_entry
{
	uint32_t id;
	uint32_t pos;
	uint32_t chunkstart;
	uint32_t blockstart;
	uint32_t soff;
	char chunkid[5];
};

struct label_entry
{
	uint32_t id;
	char name[300];
};

struct wav
{
	unsigned char *bytes;
	size_t size;
	size_t data_off;
	size_t data_len;
	int has_data;
	int has_fact;
	uint32_t fact_frames;
	int has_cue;
	int has_list;
	uint32_t cue_count;
	struct cue_entry cues[MAX_ENTRIES];
	int ncues;
	struct label_entry labels[MAX_ENTRIES];
	int nlabels;
	char order[16][5];
	int norder;
};

static uint32_t get32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1]<<8
	|	(uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}

static void put16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v>>8);
}

/* Fill a buffer with a deterministic nonzero signed 16 bit pattern. */
static void pattern_s16(unsigned char *dst, size_t frames)
{
	size_t i;
	for(i=0; i<frames; ++i)
	{
		uint16_t v = (uint16_t)((i*137 + 11) & 0xffff);
		if(!v)
			v = 1; /* never zero, so the nonzero PCM check is meaningful */
		put16(dst+2*i, v);
	}
}

static void pattern_u8(unsigned char *dst, size_t frames)
{
	size_t i;
	for(i=0; i<frames; ++i)
		dst[i] = (unsigned char)((i*13 + 7) & 0xff) | 1;
}

static void pattern_f32(unsigned char *dst, size_t samples)
{
	size_t i;
	for(i=0; i<samples; ++i)
	{
		float f = (float)((int)i - 3)*0.25f;
		uint32_t u;
		memcpy(&u, &f, 4);
		dst[4*i+0] = (unsigned char)u;
		dst[4*i+1] = (unsigned char)(u>>8);
		dst[4*i+2] = (unsigned char)(u>>16);
		dst[4*i+3] = (unsigned char)(u>>24);
	}
}

static int load_wav(const char *path, struct wav *w)
{
	FILE *f;
	long len;
	size_t off, sub;

	memset(w, 0, sizeof(*w));
	f = fopen(path, "rb");
	if(!f)
		return -1;
	if(fseek(f, 0, SEEK_END) || (len = ftell(f)) < 12 || fseek(f, 0, SEEK_SET))
	{
		fclose(f);
		return -1;
	}
	w->size = (size_t)len;
	w->bytes = malloc(w->size ? w->size : 1);
	if(!w->bytes)
	{
		fclose(f);
		return -1;
	}
	if(fread(w->bytes, 1, w->size, f) != w->size || fclose(f))
	{
		free(w->bytes);
		w->bytes = NULL;
		return -1;
	}
	if(memcmp(w->bytes, "RIFF", 4) || memcmp(w->bytes+8, "WAVE", 4))
		return -1;

	off = 12;
	while(off + 8 <= w->size)
	{
		uint32_t clen = get32(w->bytes+off+4);
		if((size_t)clen > w->size - off - 8)
			return -1;
		if(w->norder < 16)
		{
			memcpy(w->order[w->norder], w->bytes+off, 4);
			w->order[w->norder][4] = 0;
			++w->norder;
		}
		if(!memcmp(w->bytes+off, "data", 4))
		{
			if(w->has_data)
				return -1;
			w->has_data = 1;
			w->data_off = off+8;
			w->data_len = clen;
		}
		else if(!memcmp(w->bytes+off, "fact", 4))
		{
			if(clen < 4)
				return -1;
			w->has_fact = 1;
			w->fact_frames = get32(w->bytes+off+8);
		}
		else if(!memcmp(w->bytes+off, "cue ", 4))
		{
			size_t p;
			uint32_t n, c;
			if(w->has_cue || clen < 4)
				return -1;
			w->has_cue = 1;
			n = get32(w->bytes+off+8);
			w->cue_count = n;
			if(clen != 4 + n*24)
				return -1;
			p = off+12;
			for(c=0; c<n; ++c)
			{
				if(w->ncues >= MAX_ENTRIES)
					return -1;
				struct cue_entry *e = &w->cues[w->ncues++];
				e->id         = get32(w->bytes+p);
				e->pos        = get32(w->bytes+p+4);
				memcpy(e->chunkid, w->bytes+p+8, 4);
				e->chunkid[4] = 0;
				e->chunkstart = get32(w->bytes+p+12);
				e->blockstart = get32(w->bytes+p+16);
				e->soff       = get32(w->bytes+p+20);
				p += 24;
			}
		}
		else if(!memcmp(w->bytes+off, "LIST", 4))
		{
			if(w->has_list || clen < 4 || memcmp(w->bytes+off+8, "adtl", 4))
				return -1;
			w->has_list = 1;
			sub = off+12;
			while(sub + 8 <= off + 8 + clen)
			{
				uint32_t slen = get32(w->bytes+sub+4);
				if((size_t)slen > (size_t)(off + 8 + clen) - sub - 8)
					return -1;
				if(!memcmp(w->bytes+sub, "labl", 4))
				{
					size_t namelen;
					if(slen < 4 || w->nlabels >= MAX_ENTRIES)
						return -1;
					struct label_entry *l = &w->labels[w->nlabels++];
					l->id = get32(w->bytes+sub+8);
					namelen = slen - 4;
					if(namelen >= sizeof(l->name))
						namelen = sizeof(l->name)-1;
					memcpy(l->name, w->bytes+sub+12, namelen);
					l->name[namelen] = 0;
				}
				sub += 8 + slen + (slen & 1);
			}
			if(sub != off + 8 + clen)
				return -1;
		}
		off += 8 + clen + (clen & 1);
	}
	if(off != w->size)
		return -1;
	/* The RIFF length field must cover all chunks plus the 8 byte preamble. */
	if(get32(w->bytes+4) + 8 != w->size)
		return -1;
	return 0;
}

static void free_wav(struct wav *w)
{
	free(w->bytes);
	w->bytes = NULL;
}

static int cue_is(const struct wav *w, int index, uint32_t id, uint32_t pos)
{
	if(index < 0 || index >= w->ncues)
		return 0;
	const struct cue_entry *e = &w->cues[index];
	return e->id == id && e->pos == pos && e->soff == pos
	&& e->chunkstart == 0 && e->blockstart == 0
	&& !memcmp(e->chunkid, "data", 4);
}

static int label_is(const struct wav *w, uint32_t id, const char *name)
{
	int i;
	for(i=0; i<w->nlabels; ++i)
	{
		if(w->labels[i].id != id)
			continue;
		/* Stored name includes the terminating NUL. */
		size_t n = strlen(name)+1;
		return !memcmp(w->labels[i].name, name, n);
	}
	return 0;
}

static int has_label(const struct wav *w, uint32_t id)
{
	int i;
	for(i=0; i<w->nlabels; ++i)
		if(w->labels[i].id == id)
			return 1;
	return 0;
}

static int cue_index_by_id(const struct wav *w, uint32_t id)
{
	int i;
	for(i=0; i<w->ncues; ++i)
		if(w->cues[i].id == id)
			return i;
	return -1;
}

/* Common setup: open a WAV file and start mono signed 16 bit playback. */
static int start_s16(out123_handle *a, const char *path, int buffered)
{
	if(buffered && out123_set_buffer(a, 65536))
		return -1;
	if(out123_open(a, "wav", path))
		return -1;
	if(out123_start(a, 48000, 1, MPG123_ENC_SIGNED_16))
		return -1;
	return 0;
}

static int play_s16(out123_handle *a, const unsigned char *pcm, size_t bytes)
{
	return out123_play(a, (void*)pcm, bytes) == bytes;
}

/* ---------------------------------------------------------------- queue */

static int case_unbuffered_positions(void)
{
	const char *path = ".task-build/cue-queue-unbuffered.wav";
	out123_handle *a = out123_new();
	unsigned char pcm[2*16];
	struct wav w;
	remove(path);
	pattern_s16(pcm, 16);
	TASK_CHECK(a != NULL);
	TASK_CHECK(!start_s16(a, path, 0));
	TASK_CHECK(play_s16(a, pcm, 2*3));
	TASK_CHECK(out123_wav_cue(a, 1, "third") == 0);
	TASK_CHECK(play_s16(a, pcm+6, 2*5));
	TASK_CHECK(out123_wav_cue(a, 2, "eighth") == 0);
	TASK_CHECK(play_s16(a, pcm+16, 2*2));
	out123_stop(a);
	TASK_CHECK(out123_errcode(a) == 0);
	out123_del(a);

	TASK_CHECK(load_wav(path, &w) == 0);
	TASK_CHECK(w.has_data && w.data_len == 2*10);
	TASK_CHECK(!memcmp(w.bytes+w.data_off, pcm, 2*10));
	TASK_CHECK(w.ncues == 2);
	TASK_CHECK(cue_is(&w, 0, 1, 3));
	TASK_CHECK(cue_is(&w, 1, 2, 8));
	TASK_CHECK(label_is(&w, 1, "third"));
	TASK_CHECK(label_is(&w, 2, "eighth"));
	free_wav(&w);
	return 0;
}

static int case_buffered_positions(void)
{
	const char *path = ".task-build/cue-queue-buffered.wav";
	out123_handle *a = out123_new();
	unsigned char pcm[2*16];
	struct wav w;
	remove(path);
	pattern_s16(pcm, 16);
	TASK_CHECK(a != NULL);
	TASK_CHECK(!start_s16(a, path, 1));
	/* All of this is small enough to still sit in the shared ring buffer
	   when the marks are made; the positions must include it anyway. */
	TASK_CHECK(play_s16(a, pcm, 2*4));
	TASK_CHECK(out123_wav_cue(a, 10, "queued-a") == 0);
	TASK_CHECK(play_s16(a, pcm+8, 2*4));
	TASK_CHECK(out123_wav_cue(a, 11, "queued-b") == 0);
	out123_stop(a);
	TASK_CHECK(out123_errcode(a) == 0);
	out123_del(a);

	TASK_CHECK(load_wav(path, &w) == 0);
	TASK_CHECK(w.has_data && w.data_len == 2*8);
	TASK_CHECK(!memcmp(w.bytes+w.data_off, pcm, 2*8));
	TASK_CHECK(cue_is(&w, cue_index_by_id(&w, 10), 10, 4));
	TASK_CHECK(cue_is(&w, cue_index_by_id(&w, 11), 11, 8));
	TASK_CHECK(label_is(&w, 10, "queued-a"));
	TASK_CHECK(label_is(&w, 11, "queued-b"));
	free_wav(&w);
	return 0;
}

static int case_pause_accumulate(void)
{
	const char *path = ".task-build/cue-queue-pause.wav";
	out123_handle *a = out123_new();
	unsigned char pcm[2*16];
	struct wav w;
	remove(path);
	pattern_s16(pcm, 16);
	TASK_CHECK(a != NULL);
	TASK_CHECK(!start_s16(a, path, 1));
	TASK_CHECK(play_s16(a, pcm, 2*5));       /* frames 0..4 */
	out123_pause(a);
	TASK_CHECK(play_s16(a, pcm+10, 2*3));    /* auto-continues, frames 5..7 */
	TASK_CHECK(out123_wav_cue(a, 1, "paused") == 0); /* position 8 */
	out123_continue(a);
	TASK_CHECK(play_s16(a, pcm+16, 2*2));    /* frames 8..9 */
	out123_stop(a);
	out123_del(a);

	TASK_CHECK(load_wav(path, &w) == 0);
	TASK_CHECK(w.data_len == 2*10);
	TASK_CHECK(cue_is(&w, cue_index_by_id(&w, 1), 1, 8));
	TASK_CHECK(label_is(&w, 1, "paused"));
	free_wav(&w);
	return 0;
}

static int case_split_play_and_wrap(void)
{
	const char *path = ".task-build/cue-queue-wrap.wav";
	out123_handle *a = out123_new();
	unsigned char pcm[2*1000];
	struct wav w;
	size_t i, total = 0;
	remove(path);
	pattern_s16(pcm, 1000);
	TASK_CHECK(a != NULL);
	TASK_CHECK(!start_s16(a, path, 1));
	/* Several thousand small plays, more than the 64 KiB ring can hold at
	   once, so the buffer wraps around many times. */
	for(i=0; i<40; ++i)
	{
		TASK_CHECK(play_s16(a, pcm, 2*1000));
		total += 1000;
		if(i == 19)
			TASK_CHECK(out123_wav_cue(a, 1, "mid") == 0);
	}
	TASK_CHECK(out123_wav_cue(a, 2, "end") == 0);
	out123_stop(a);
	TASK_CHECK(out123_errcode(a) == 0);
	out123_del(a);

	TASK_CHECK(load_wav(path, &w) == 0);
	TASK_CHECK(w.data_len == 2*total);
	TASK_CHECK(cue_is(&w, cue_index_by_id(&w, 1), 1, 20*1000));
	TASK_CHECK(cue_is(&w, cue_index_by_id(&w, 2), 2, 40*1000));
	free_wav(&w);
	return 0;
}

/* ------------------------------------------------------------ container */

static int case_same_position_and_empty_name(void)
{
	const char *path = ".task-build/cue-container-multi.wav";
	out123_handle *a = out123_new();
	unsigned char pcm[2*8];
	struct wav w;
	remove(path);
	pattern_s16(pcm, 8);
	TASK_CHECK(a != NULL);
	TASK_CHECK(!start_s16(a, path, 0));
	TASK_CHECK(play_s16(a, pcm, 2*4));
	TASK_CHECK(out123_wav_cue(a, 1, "one") == 0);
	TASK_CHECK(out123_wav_cue(a, 2, "") == 0);
	TASK_CHECK(out123_wav_cue(a, 3, "three") == 0);
	TASK_CHECK(play_s16(a, pcm+8, 2*4));
	TASK_CHECK(out123_wav_cue(a, 4, "four") == 0);
	out123_stop(a);
	out123_del(a);

	TASK_CHECK(load_wav(path, &w) == 0);
	TASK_CHECK(w.data_len == 2*8);
	TASK_CHECK(w.cue_count == 4 && w.ncues == 4);
	TASK_CHECK(cue_is(&w, 0, 1, 4));
	TASK_CHECK(cue_is(&w, 1, 2, 4));
	TASK_CHECK(cue_is(&w, 2, 3, 4));
	TASK_CHECK(cue_is(&w, 3, 4, 8));
	TASK_CHECK(label_is(&w, 1, "one"));
	TASK_CHECK(!has_label(&w, 2)); /* empty name: no labl subchunk */
	TASK_CHECK(label_is(&w, 3, "three"));
	TASK_CHECK(label_is(&w, 4, "four"));
	free_wav(&w);
	return 0;
}

static int case_odd_and_even_padding(void)
{
	const char *path = ".task-build/cue-container-odd.wav";
	out123_handle *a = out123_new();
	unsigned char pcm[16];
	struct wav w;
	size_t off;
	remove(path);
	pattern_u8(pcm, 16);
	TASK_CHECK(a != NULL);
	/* 8 bit mono: one byte per frame, so an odd frame count gives odd data. */
	TASK_CHECK(!out123_open(a, "wav", path));
	TASK_CHECK(!out123_start(a, 8000, 1, MPG123_ENC_UNSIGNED_8));
	TASK_CHECK(play_s16(a, pcm, 3));
	TASK_CHECK(out123_wav_cue(a, 1, "ab") == 0); /* labl payload 7, odd */
	TASK_CHECK(play_s16(a, pcm+3, 2));
	out123_stop(a);
	out123_del(a);

	TASK_CHECK(load_wav(path, &w) == 0);
	TASK_CHECK(w.data_len == 5);
	TASK_CHECK(!memcmp(w.bytes+w.data_off, pcm, 5));
	TASK_CHECK(cue_is(&w, cue_index_by_id(&w, 1), 1, 3));
	TASK_CHECK(label_is(&w, 1, "ab"));
	/* The data payload starts on an even offset; its odd length is followed
	   by one pad byte, so the next chunk also starts even. */
	TASK_CHECK(w.data_off % 2 == 0);
	TASK_CHECK(w.size > w.data_off + w.data_len); /* a cue chunk follows */
	/* Every chunk boundary in the parsed list must be even. */
	off = 12;
	while(off + 8 <= w.size)
	{
		uint32_t clen = get32(w.bytes+off+4);
		TASK_CHECK(off % 2 == 0);
		off += 8 + clen + (clen & 1);
	}
	TASK_CHECK(off == w.size);
	free_wav(&w);
	return 0;
}

static int case_float_fact(void)
{
	const char *path = ".task-build/cue-container-float.wav";
	out123_handle *a = out123_new();
	unsigned char fpcm[4*2*8];
	struct wav w;
	remove(path);
	pattern_f32(fpcm, 8);
	TASK_CHECK(a != NULL);
	TASK_CHECK(!out123_open(a, "wav", path));
	TASK_CHECK(!out123_start(a, 44100, 2, MPG123_ENC_FLOAT_32));
	TASK_CHECK(out123_play(a, fpcm, 4*8) == 4*8); /* 4 frames */
	TASK_CHECK(out123_wav_cue(a, 7, "float") == 0);
	TASK_CHECK(out123_play(a, fpcm+32, 4*8) == 4*8); /* 4 more frames */
	out123_stop(a);
	out123_del(a);

	TASK_CHECK(load_wav(path, &w) == 0);
	TASK_CHECK(w.has_fact && w.fact_frames == 8);
	TASK_CHECK(w.has_data && w.data_len == 4*2*8);
	TASK_CHECK(!memcmp(w.bytes+w.data_off, fpcm, w.data_len));
	TASK_CHECK(cue_is(&w, cue_index_by_id(&w, 7), 7, 4));
	TASK_CHECK(label_is(&w, 7, "float"));
	free_wav(&w);
	return 0;
}

static int case_no_marks_no_chunks(void)
{
	const char *path = ".task-build/cue-container-none.wav";
	out123_handle *a = out123_new();
	unsigned char pcm[2*4];
	struct wav w;
	remove(path);
	pattern_s16(pcm, 4);
	TASK_CHECK(a != NULL);
	TASK_CHECK(!start_s16(a, path, 0));
	TASK_CHECK(play_s16(a, pcm, 2*4));
	out123_stop(a);
	out123_del(a);

	TASK_CHECK(load_wav(path, &w) == 0);
	TASK_CHECK(w.data_len == 2*4);
	TASK_CHECK(!w.has_cue && !w.has_list);
	TASK_CHECK(!memcmp(w.bytes+w.data_off, pcm, w.data_len));
	/* Nothing appended after an even-sized data chunk. */
	TASK_CHECK(w.data_off + w.data_len == w.size);
	TASK_CHECK(w.norder == 2); /* fmt, data */
	free_wav(&w);
	return 0;
}

/* ------------------------------------------------------------ lifecycle */

static int case_error_codes(void)
{
	const char *path = ".task-build/cue-life-errors.wav";
	out123_handle *a = out123_new();
	char longname[300];
	remove(path);
	memset(longname, 'x', 256);
	longname[256] = 0;
	TASK_CHECK(a != NULL);
	/* Not opened at all. */
	TASK_CHECK(out123_wav_cue(a, 1, "x") == -1);
	TASK_CHECK(out123_errcode(a) == OUT123_NOT_LIVE);
	TASK_CHECK(!out123_open(a, "wav", path));
	/* Opened but not started. */
	TASK_CHECK(out123_wav_cue(a, 1, "x") == -1);
	TASK_CHECK(out123_errcode(a) == OUT123_NOT_LIVE);
	TASK_CHECK(!out123_start(a, 48000, 1, MPG123_ENC_SIGNED_16));
	/* Invalid IDs. */
	TASK_CHECK(out123_wav_cue(a, 0, "x") == -1);
	TASK_CHECK(out123_errcode(a) == OUT123_ARG_ERROR);
	TASK_CHECK(out123_wav_cue(a, 0x100000000UL, "x") == -1);
	TASK_CHECK(out123_errcode(a) == OUT123_ARG_ERROR);
	/* Oversized label. */
	TASK_CHECK(out123_wav_cue(a, 1, longname) == -1);
	TASK_CHECK(out123_errcode(a) == OUT123_ARG_ERROR);
	/* Valid ID, then duplicate. */
	TASK_CHECK(out123_wav_cue(a, 42, "ok") == 0);
	TASK_CHECK(out123_wav_cue(a, 42, "again") == -1);
	TASK_CHECK(out123_errcode(a) == OUT123_ARG_ERROR);
	/* 0xffffffff is a valid ID. */
	TASK_CHECK(out123_wav_cue(a, 0xffffffffUL, "max") == 0);
	out123_stop(a);
	/* Stopped output is no longer live. */
	TASK_CHECK(out123_wav_cue(a, 5, "late") == -1);
	TASK_CHECK(out123_errcode(a) == OUT123_NOT_LIVE);
	out123_del(a);

	/* Non-WAV output cannot carry marks. */
	a = out123_new();
	TASK_CHECK(a != NULL);
	TASK_CHECK(!out123_open(a, "raw", path));
	TASK_CHECK(!out123_start(a, 48000, 1, MPG123_ENC_SIGNED_16));
	TASK_CHECK(out123_wav_cue(a, 1, "x") == -1);
	TASK_CHECK(out123_errcode(a) == OUT123_NOT_SUPPORTED);
	out123_del(a);
	return 0;
}

static int case_failed_mark_does_not_consume(void)
{
	const char *path = ".task-build/cue-life-noconsume.wav";
	out123_handle *a = out123_new();
	unsigned char pcm[2*6];
	struct wav w;
	remove(path);
	pattern_s16(pcm, 6);
	TASK_CHECK(a != NULL);
	TASK_CHECK(!start_s16(a, path, 0));
	/* A rejected registration must not swallow audio or leave a mark. */
	TASK_CHECK(out123_wav_cue(a, 0, "bad") == -1);
	TASK_CHECK(play_s16(a, pcm, 2*6));
	TASK_CHECK(out123_wav_cue(a, 1, "good") == 0);
	out123_stop(a);
	out123_del(a);

	TASK_CHECK(load_wav(path, &w) == 0);
	TASK_CHECK(w.data_len == 2*6);
	TASK_CHECK(!memcmp(w.bytes+w.data_off, pcm, 2*6));
	TASK_CHECK(w.ncues == 1);
	TASK_CHECK(cue_is(&w, 0, 1, 6));
	free_wav(&w);
	return 0;
}

static int case_drop_cancels_and_reuses_id(void)
{
	const char *path = ".task-build/cue-life-drop.wav";
	out123_handle *a = out123_new();
	unsigned char pcm[2*20];
	struct wav w;
	remove(path);
	pattern_s16(pcm, 20);
	TASK_CHECK(a != NULL);
	TASK_CHECK(!start_s16(a, path, 1));
	/* Small amounts all stay queued in the ring while preloading, so they
	   are still pending when dropped. */
	TASK_CHECK(play_s16(a, pcm, 2*10));
	TASK_CHECK(out123_wav_cue(a, 5, "doomed-a") == 0);
	TASK_CHECK(play_s16(a, pcm+20, 2*5));
	TASK_CHECK(out123_wav_cue(a, 6, "doomed-b") == 0);
	out123_drop(a);
	/* Cancelled IDs are free again, and the position starts at the real tail. */
	TASK_CHECK(out123_wav_cue(a, 5, "reused") == 0);
	TASK_CHECK(play_s16(a, pcm+30, 2*3));
	TASK_CHECK(out123_wav_cue(a, 7, "survivor") == 0);
	out123_stop(a);
	TASK_CHECK(out123_errcode(a) == 0);
	out123_del(a);

	TASK_CHECK(load_wav(path, &w) == 0);
	TASK_CHECK(w.data_len == 2*3); /* only the audio after the drop remains */
	TASK_CHECK(!memcmp(w.bytes+w.data_off, pcm+30, 2*3));
	TASK_CHECK(w.ncues == 2);
	TASK_CHECK(cue_is(&w, cue_index_by_id(&w, 5), 5, 0));
	TASK_CHECK(cue_index_by_id(&w, 6) < 0);
	TASK_CHECK(cue_is(&w, cue_index_by_id(&w, 7), 7, 3));
	TASK_CHECK(label_is(&w, 5, "reused"));
	free_wav(&w);
	return 0;
}

static int case_drop_keeps_reached_boundary(void)
{
	const char *path = ".task-build/cue-life-boundary.wav";
	out123_handle *a = out123_new();
	unsigned char pcm[2*16];
	struct wav w;
	remove(path);
	pattern_s16(pcm, 16);
	TASK_CHECK(a != NULL);
	TASK_CHECK(!start_s16(a, path, 1));
	/* First batch: force it out so it forms the real file tail. */
	TASK_CHECK(play_s16(a, pcm, 2*16));
	out123_drain(a);
	/* Mark at the reached boundary. */
	TASK_CHECK(out123_wav_cue(a, 1, "kept") == 0);
	/* Queue a small amount that stays pending, plus a mark beyond the tail. */
	TASK_CHECK(play_s16(a, pcm, 2*4));
	TASK_CHECK(out123_wav_cue(a, 2, "dropped") == 0);
	out123_drop(a);
	out123_stop(a);
	out123_del(a);

	TASK_CHECK(load_wav(path, &w) == 0);
	TASK_CHECK(w.data_len == 2*16);
	TASK_CHECK(w.ncues == 1);
	TASK_CHECK(cue_is(&w, cue_index_by_id(&w, 1), 1, 16));
	TASK_CHECK(cue_index_by_id(&w, 2) < 0);
	free_wav(&w);
	return 0;
}

static int case_reopen_does_not_inherit(void)
{
	const char *path1 = ".task-build/cue-life-reopen1.wav";
	const char *path2 = ".task-build/cue-life-reopen2.wav";
	out123_handle *a = out123_new();
	unsigned char pcm[2*8];
	struct wav w;
	remove(path1);
	remove(path2);
	pattern_s16(pcm, 8);
	TASK_CHECK(a != NULL);
	TASK_CHECK(!start_s16(a, path1, 0));
	TASK_CHECK(play_s16(a, pcm, 2*2));
	TASK_CHECK(out123_wav_cue(a, 1, "old") == 0);
	TASK_CHECK(play_s16(a, pcm+4, 2*2));
	out123_stop(a);
	/* Same handle, new file: no old marks may leak into it. */
	TASK_CHECK(!out123_open(a, "wav", path2));
	TASK_CHECK(!out123_start(a, 48000, 1, MPG123_ENC_SIGNED_16));
	TASK_CHECK(play_s16(a, pcm, 2*3));
	TASK_CHECK(out123_wav_cue(a, 1, "new") == 0);
	out123_stop(a);
	out123_del(a);

	TASK_CHECK(load_wav(path1, &w) == 0);
	TASK_CHECK(w.ncues == 1 && cue_is(&w, 0, 1, 2));
	TASK_CHECK(label_is(&w, 1, "old"));
	free_wav(&w);
	TASK_CHECK(load_wav(path2, &w) == 0);
	TASK_CHECK(w.ncues == 1 && cue_is(&w, 0, 1, 3));
	TASK_CHECK(label_is(&w, 1, "new"));
	free_wav(&w);
	return 0;
}

static int case_two_handles_independent(void)
{
	const char *pa = ".task-build/cue-life-handle-a.wav";
	const char *pb = ".task-build/cue-life-handle-b.wav";
	out123_handle *a = out123_new();
	out123_handle *b = out123_new();
	unsigned char pcm[2*8];
	struct wav wa, wb;
	remove(pa);
	remove(pb);
	pattern_s16(pcm, 8);
	TASK_CHECK(a != NULL && b != NULL);
	TASK_CHECK(!start_s16(a, pa, 1));
	TASK_CHECK(!start_s16(b, pb, 1));
	TASK_CHECK(play_s16(a, pcm, 2*2));
	TASK_CHECK(play_s16(b, pcm, 2*3));
	TASK_CHECK(out123_wav_cue(a, 1, "a-one") == 0); /* pos 2 */
	TASK_CHECK(out123_wav_cue(b, 2, "b-one") == 0); /* pos 3 */
	TASK_CHECK(play_s16(a, pcm+4, 2*2));
	TASK_CHECK(play_s16(b, pcm+6, 2*1));
	TASK_CHECK(out123_wav_cue(a, 3, "a-two") == 0); /* pos 4 */
	TASK_CHECK(out123_wav_cue(b, 4, "b-two") == 0); /* pos 4 */
	out123_stop(a);
	out123_stop(b);
	out123_del(a);
	out123_del(b);

	TASK_CHECK(load_wav(pa, &wa) == 0);
	TASK_CHECK(wa.data_len == 2*4);
	TASK_CHECK(wa.ncues == 2);
	TASK_CHECK(cue_is(&wa, cue_index_by_id(&wa, 1), 1, 2));
	TASK_CHECK(cue_is(&wa, cue_index_by_id(&wa, 3), 3, 4));
	free_wav(&wa);
	TASK_CHECK(load_wav(pb, &wb) == 0);
	TASK_CHECK(wb.data_len == 2*4);
	TASK_CHECK(wb.ncues == 2);
	TASK_CHECK(cue_is(&wb, cue_index_by_id(&wb, 2), 2, 3));
	TASK_CHECK(cue_is(&wb, cue_index_by_id(&wb, 4), 4, 4));
	free_wav(&wb);
	return 0;
}

static int case_drain_and_pause_keep_positions(void)
{
	const char *path = ".task-build/cue-life-drain.wav";
	out123_handle *a = out123_new();
	unsigned char pcm[2*16];
	struct wav w;
	remove(path);
	pattern_s16(pcm, 16);
	TASK_CHECK(a != NULL);
	TASK_CHECK(!start_s16(a, path, 1));
	TASK_CHECK(play_s16(a, pcm, 2*5));      /* frames 0..4 */
	TASK_CHECK(out123_wav_cue(a, 1, "before") == 0); /* pos 5 */
	out123_pause(a);
	out123_continue(a);
	out123_drain(a);
	TASK_CHECK(play_s16(a, pcm+10, 2*3));   /* frames 5..7 */
	TASK_CHECK(out123_wav_cue(a, 2, "after") == 0); /* pos 8 */
	out123_ndrain(a, 2);
	TASK_CHECK(play_s16(a, pcm+16, 2*2));   /* frames 8..9 */
	out123_stop(a);
	out123_del(a);

	TASK_CHECK(load_wav(path, &w) == 0);
	TASK_CHECK(w.data_len == 2*10);
	TASK_CHECK(cue_is(&w, cue_index_by_id(&w, 1), 1, 5));
	TASK_CHECK(cue_is(&w, cue_index_by_id(&w, 2), 2, 8));
	TASK_CHECK(w.norder == 4); /* fmt, data, cue , LIST */
	TASK_CHECK(!strcmp(w.order[2], "cue ") && !strcmp(w.order[3], "LIST"));
	free_wav(&w);
	return 0;
}

const struct task_case task_cases[] =
{
	{ "queue", "unbuffered_positions", case_unbuffered_positions },
	{ "queue", "buffered_positions", case_buffered_positions },
	{ "queue", "pause_accumulate", case_pause_accumulate },
	{ "queue", "split_play_and_wrap", case_split_play_and_wrap },
	{ "container", "same_position_and_empty_name", case_same_position_and_empty_name },
	{ "container", "odd_and_even_padding", case_odd_and_even_padding },
	{ "container", "float_fact", case_float_fact },
	{ "container", "no_marks_no_chunks", case_no_marks_no_chunks },
	{ "lifecycle", "error_codes", case_error_codes },
	{ "lifecycle", "failed_mark_does_not_consume", case_failed_mark_does_not_consume },
	{ "lifecycle", "drop_cancels_and_reuses_id", case_drop_cancels_and_reuses_id },
	{ "lifecycle", "drop_keeps_reached_boundary", case_drop_keeps_reached_boundary },
	{ "lifecycle", "reopen_does_not_inherit", case_reopen_does_not_inherit },
	{ "lifecycle", "two_handles_independent", case_two_handles_independent },
	{ "lifecycle", "drain_and_pause_keep_positions", case_drain_and_pause_keep_positions }
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
