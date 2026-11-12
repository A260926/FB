/*
** Copyright (C) 2026 Erik de Castro Lopo <erikd@mega-nerd.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU Lesser General Public License as published by
** the Free Software Foundation; either version 2.1 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/*
** Tests for named cue points ('labl' labels inside LIST/adtl chunks) in
** little endian RIFF/WAVE files.
**
** The read direction uses hand crafted RIFF byte streams. The write
** direction is verified by walking the RIFF bytes of the written file
** directly, not only by reading the file back with libsndfile itself.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <sndfile.h>

#define TEST_FILE	"wav_cue_labels_test.wav"

/* The audio signal used by every scenario : 4 frames of stereo 16 bit PCM. */
#define TEST_FRAMES	4
#define TEST_CHANNELS	2
static const short test_pcm [TEST_FRAMES * TEST_CHANNELS] =
{	100, -100, 200, -200, 300, -300, 400, -400
} ;

static int failures = 0 ;

#define CHECK(cond, msg) \
	do {	if (! (cond)) \
			{	printf ("FAIL (line %d) : %s\n", __LINE__, msg) ; \
				failures ++ ; \
				} ; \
		} while (0)

/*------------------------------------------------------------------------------
** Little endian byte buffer for hand crafted files.
*/

typedef struct
{	uint8_t *data ;
	size_t	len ;
	size_t	cap ;
} BUF ;

static void
buf_put (BUF *b, const void *src, size_t n)
{	if (b->len + n > b->cap)
	{	b->cap = 2 * (b->len + n) + 256 ;
		if ((b->data = realloc (b->data, b->cap)) == NULL)
		{	fprintf (stderr, "Out of memory.\n") ;
			exit (1) ;
			} ;
		} ;
	memcpy (b->data + b->len, src, n) ;
	b->len += n ;
} /* buf_put */

static void
buf_zero (BUF *b, size_t n)
{	static const uint8_t zeros [8] = { 0 } ;
	while (n > 0)
	{	size_t k = n < sizeof (zeros) ? n : sizeof (zeros) ;
		buf_put (b, zeros, k) ;
		n -= k ;
		} ;
} /* buf_zero */

static void
buf_u16le (BUF *b, uint32_t v)
{	uint8_t t [2] = { (uint8_t) v, (uint8_t) (v >> 8) } ;
	buf_put (b, t, sizeof (t)) ;
} /* buf_u16le */

static void
buf_u32le (BUF *b, uint32_t v)
{	uint8_t t [4] = { (uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24) } ;
	buf_put (b, t, sizeof (t)) ;
} /* buf_u32le */

static void
buf_marker (BUF *b, const char *id)
{	buf_put (b, id, 4) ;
} /* buf_marker */

static void
buf_str (BUF *b, const char *s)
{	buf_put (b, s, strlen (s)) ;
} /* buf_str */

/* Wrap a payload in a RIFF chunk : header, payload, pad byte if odd. */
static void
buf_chunk (BUF *b, const char *id, const BUF *payload)
{	buf_marker (b, id) ;
	buf_u32le (b, (uint32_t) payload->len) ;
	buf_put (b, payload->data, payload->len) ;
	if (payload->len & 1)
		buf_zero (b, 1) ;
} /* buf_chunk */

static BUF
make_fmt_chunk (void)
{	BUF b = { NULL, 0, 0 } ;
	buf_u16le (&b, 1) ;					/* PCM */
	buf_u16le (&b, TEST_CHANNELS) ;
	buf_u32le (&b, 48000) ;
	buf_u32le (&b, 48000 * TEST_CHANNELS * 2) ;
	buf_u16le (&b, TEST_CHANNELS * 2) ;
	buf_u16le (&b, 16) ;
	return b ;
} /* make_fmt_chunk */

static BUF
make_data_chunk (void)
{	BUF b = { NULL, 0, 0 } ;
	int k ;
	for (k = 0 ; k < TEST_FRAMES * TEST_CHANNELS ; k++)
		buf_u16le (&b, (uint16_t) test_pcm [k]) ;
	return b ;
} /* make_data_chunk */

static BUF
make_cue_chunk (const int32_t *ids, const uint32_t *offsets, int count)
{	BUF b = { NULL, 0, 0 } ;
	int k ;
	buf_u32le (&b, (uint32_t) count) ;
	for (k = 0 ; k < count ; k++)
	{	buf_u32le (&b, (uint32_t) ids [k]) ;
		buf_u32le (&b, offsets [k]) ;	/* position */
		buf_marker (&b, "data") ;
		buf_u32le (&b, 0) ;				/* chunk start */
		buf_u32le (&b, 0) ;				/* block start */
		buf_u32le (&b, offsets [k]) ;	/* sample offset */
		} ;
	return b ;
} /* make_cue_chunk */

/* A 'labl' subchunk : cue id + NUL terminated label (+ pad). */
static void
buf_labl (BUF *b, int32_t id, const char *name, size_t name_len)
{	buf_marker (b, "labl") ;
	buf_u32le (b, (uint32_t) (4 + name_len + 1)) ;
	buf_u32le (b, (uint32_t) id) ;
	buf_put (b, name, name_len) ;
	buf_zero (b, 1) ;					/* NUL terminator */
	if ((name_len + 1) & 1)
		buf_zero (b, 1) ;				/* pad byte */
} /* buf_labl */

/* A 'note' subchunk, which the parser must skip over. */
static void
buf_note (BUF *b, int32_t id, const char *text)
{	size_t n = strlen (text) + 1 ;
	buf_marker (b, "note") ;
	buf_u32le (b, (uint32_t) (4 + n)) ;
	buf_u32le (b, (uint32_t) id) ;
	buf_put (b, text, n) ;
	if (n & 1)
		buf_zero (b, 1) ;
} /* buf_note */

/* An unknown subchunk with an odd sized payload, must also be skipped. */
static void
buf_unknown (BUF *b, const char *id, size_t payload_len)
{	buf_marker (b, id) ;
	buf_u32le (b, (uint32_t) payload_len) ;
	buf_zero (b, payload_len) ;
	if (payload_len & 1)
		buf_zero (b, 1) ;
} /* buf_unknown */

static BUF
make_wav (const BUF *body)
{	BUF b = { NULL, 0, 0 } ;
	buf_marker (&b, "RIFF") ;
	buf_u32le (&b, (uint32_t) (4 + body->len)) ;
	buf_marker (&b, "WAVE") ;
	buf_put (&b, body->data, body->len) ;
	return b ;
} /* make_wav */

static void
write_file (const char *path, const uint8_t *data, size_t len)
{	FILE *f = fopen (path, "wb") ;
	if (f == NULL)
	{	fprintf (stderr, "Cannot open %s for writing.\n", path) ;
		exit (1) ;
		} ;
	if (fwrite (data, 1, len, f) != len)
	{	fprintf (stderr, "Short write on %s.\n", path) ;
		exit (1) ;
		} ;
	fclose (f) ;
} /* write_file */

static uint8_t *
read_file (const char *path, size_t *len)
{	FILE *f = fopen (path, "rb") ;
	uint8_t *data ;
	long n ;
	if (f == NULL)
		return NULL ;
	fseek (f, 0, SEEK_END) ;
	n = ftell (f) ;
	fseek (f, 0, SEEK_SET) ;
	if ((data = malloc (n > 0 ? n : 1)) == NULL)
	{	fprintf (stderr, "Out of memory.\n") ;
		exit (1) ;
		} ;
	if (fread (data, 1, (size_t) n, f) != (size_t) n)
	{	fprintf (stderr, "Short read on %s.\n", path) ;
		exit (1) ;
		} ;
	fclose (f) ;
	*len = (size_t) n ;
	return data ;
} /* read_file */

/*------------------------------------------------------------------------------
** In-memory virtual IO for reading the same bytes through sf_open_virtual.
*/

typedef struct
{	const uint8_t *data ;
	sf_count_t	len ;
	sf_count_t	pos ;
} MEMIO ;

static sf_count_t
mem_get_filelen (void *user)
{	return ((MEMIO *) user)->len ;
} /* mem_get_filelen */

static sf_count_t
mem_seek (sf_count_t offset, int whence, void *user)
{	MEMIO *m = user ;
	sf_count_t pos ;
	switch (whence)
	{	case SEEK_SET : pos = offset ; break ;
		case SEEK_CUR : pos = m->pos + offset ; break ;
		case SEEK_END : pos = m->len + offset ; break ;
		default : return -1 ;
		} ;
	if (pos < 0 || pos > m->len)
		return -1 ;
	m->pos = pos ;
	return pos ;
} /* mem_seek */

static sf_count_t
mem_read (void *ptr, sf_count_t count, void *user)
{	MEMIO *m = user ;
	if (m->pos + count > m->len)
		count = m->len - m->pos ;
	memcpy (ptr, m->data + m->pos, (size_t) count) ;
	m->pos += count ;
	return count ;
} /* mem_read */

static sf_count_t
mem_write (const void *ptr, sf_count_t count, void *user)
{	(void) ptr ;
	(void) count ;
	(void) user ;
	return 0 ;
} /* mem_write */

static sf_count_t
mem_tell (void *user)
{	return ((MEMIO *) user)->pos ;
} /* mem_tell */

/*------------------------------------------------------------------------------
** Read back checks, shared by the sf_open and sf_open_virtual paths.
*/

typedef struct
{	int32_t		id ;
	uint32_t	offset ;
	const char	*name ;
} EXPECT_CUE ;

static void
check_open_file (SNDFILE *file, const SF_INFO *info, const EXPECT_CUE *expect, int expect_count, const char *what)
{	short pcm [TEST_FRAMES * TEST_CHANNELS] ;
	SF_CUES_VAR (8) cues ;
	uint32_t cue_count = 0 ;
	int k ;

	CHECK (info->frames == TEST_FRAMES, "frame count mismatch") ;
	CHECK (info->channels == TEST_CHANNELS, "channel count mismatch") ;

	if (expect_count == 0)
	{	CHECK (sf_command (file, SFC_GET_CUE_COUNT, &cue_count, sizeof (cue_count)) == SF_FALSE,
				"SFC_GET_CUE_COUNT should fail when there are no cues") ;
		}
	else
	{	CHECK (sf_command (file, SFC_GET_CUE_COUNT, &cue_count, sizeof (cue_count)) == SF_TRUE,
				"SFC_GET_CUE_COUNT failed") ;
		CHECK ((int) cue_count == expect_count, "cue count mismatch") ;

		memset (&cues, 0, sizeof (cues)) ;
		CHECK (sf_command (file, SFC_GET_CUE, &cues, sizeof (cues)) == SF_TRUE, "SFC_GET_CUE failed") ;
		CHECK ((int) cues.cue_count == expect_count, "cue count mismatch in SFC_GET_CUE") ;

		for (k = 0 ; k < expect_count && k < (int) cues.cue_count ; k++)
		{	CHECK (cues.cue_points [k].indx == expect [k].id, "cue id mismatch") ;
			CHECK (cues.cue_points [k].sample_offset == expect [k].offset, "cue sample offset mismatch") ;
			if (strcmp (cues.cue_points [k].name, expect [k].name) != 0)
			{	printf ("FAIL : %s : cue %d name '%s', expected '%s'\n",
						what, expect [k].id, cues.cue_points [k].name, expect [k].name) ;
				failures ++ ;
				} ;
			} ;
		} ;

	/* Actually read the audio and compare it with the written signal. */
	memset (pcm, 0, sizeof (pcm)) ;
	CHECK (sf_readf_short (file, pcm, TEST_FRAMES) == TEST_FRAMES, "short read of audio frames") ;
	CHECK (memcmp (pcm, test_pcm, sizeof (test_pcm)) == 0, "audio data mismatch") ;
} /* check_open_file */

static void
check_via_paths (const BUF *wav, const EXPECT_CUE *expect, int expect_count, const char *what)
{	SF_VIRTUAL_IO vio ;
	MEMIO mem ;
	SF_INFO info ;
	SNDFILE *file ;

	/* Path 1 : a regular file read with sf_open. */
	write_file (TEST_FILE, wav->data, wav->len) ;
	memset (&info, 0, sizeof (info)) ;
	if ((file = sf_open (TEST_FILE, SFM_READ, &info)) == NULL)
	{	printf ("FAIL : %s : sf_open failed : %s\n", what, sf_strerror (NULL)) ;
		failures ++ ;
		}
	else
	{	check_open_file (file, &info, expect, expect_count, what) ;
		sf_close (file) ;
		} ;
	remove (TEST_FILE) ;

	/* Path 2 : the same bytes through sf_open_virtual. */
	memset (&vio, 0, sizeof (vio)) ;
	vio.get_filelen	= mem_get_filelen ;
	vio.seek		= mem_seek ;
	vio.read		= mem_read ;
	vio.write		= mem_write ;
	vio.tell		= mem_tell ;
	mem.data	= wav->data ;
	mem.len		= (sf_count_t) wav->len ;
	mem.pos		= 0 ;
	memset (&info, 0, sizeof (info)) ;
	if ((file = sf_open_virtual (&vio, SFM_READ, &info, &mem)) == NULL)
	{	printf ("FAIL : %s : sf_open_virtual failed : %s\n", what, sf_strerror (NULL)) ;
		failures ++ ;
		}
	else
	{	check_open_file (file, &info, expect, expect_count, what) ;
		sf_close (file) ;
		} ;
} /* check_via_paths */

/*------------------------------------------------------------------------------
** Read direction scenarios, all from hand crafted RIFF bytes.
*/

/* LIST/adtl before the cue chunk, labels in reverse id order, unknown and
** 'note' subchunks in the way, odd and even sized labels.
*/
static void
test_labels_before_cue (void)
{	static const int32_t ids [] = { 1, 2 } ;
	static const uint32_t offsets [] = { 1, 3 } ;
	static const EXPECT_CUE expect [] =
	{	{ 1, 1, "Intro" },		/* 5 chars + NUL : even payload */
		{ 2, 3, "Chorus" }		/* 6 chars + NUL : odd payload, pad byte */
	} ;
	BUF adtl = { NULL, 0, 0 }, list = { NULL, 0, 0 }, body = { NULL, 0, 0 }, wav ;
	BUF fmt, cue, data ;

	buf_marker (&adtl, "adtl") ;
	buf_unknown (&adtl, "zzzz", 5) ;		/* unknown, odd length */
	buf_note (&adtl, 1, "a note") ;			/* note, odd length */
	buf_labl (&adtl, 2, "Chorus", 6) ;		/* reverse id order */
	buf_labl (&adtl, 1, "Intro", 5) ;
	buf_chunk (&list, "LIST", &adtl) ;

	fmt = make_fmt_chunk () ;
	data = make_data_chunk () ;
	cue = make_cue_chunk (ids, offsets, 2) ;

	buf_chunk (&body, "fmt ", &fmt) ;
	buf_put (&body, list.data, list.len) ;	/* LIST before cue and data */
	buf_chunk (&body, "data", &data) ;
	buf_chunk (&body, "cue ", &cue) ;		/* cue after data */
	wav = make_wav (&body) ;

	check_via_paths (&wav, expect, 2, "labels_before_cue") ;

	free (adtl.data) ; free (list.data) ; free (body.data) ; free (wav.data) ;
	free (fmt.data) ; free (cue.data) ; free (data.data) ;
} /* test_labels_before_cue */

/* LIST/adtl after the cue chunk, between cue and data. */
static void
test_labels_after_cue (void)
{	static const int32_t ids [] = { 7 } ;
	static const uint32_t offsets [] = { 2 } ;
	static const EXPECT_CUE expect [] = { { 7, 2, "Solo" } } ;
	BUF adtl = { NULL, 0, 0 }, list = { NULL, 0, 0 }, body = { NULL, 0, 0 }, wav ;
	BUF fmt, cue, data ;

	buf_marker (&adtl, "adtl") ;
	buf_labl (&adtl, 7, "Solo", 4) ;
	buf_chunk (&list, "LIST", &adtl) ;

	fmt = make_fmt_chunk () ;
	cue = make_cue_chunk (ids, offsets, 1) ;
	data = make_data_chunk () ;

	buf_chunk (&body, "fmt ", &fmt) ;
	buf_chunk (&body, "cue ", &cue) ;
	buf_put (&body, list.data, list.len) ;
	buf_chunk (&body, "data", &data) ;
	wav = make_wav (&body) ;

	check_via_paths (&wav, expect, 1, "labels_after_cue") ;

	free (adtl.data) ; free (list.data) ; free (body.data) ; free (wav.data) ;
	free (fmt.data) ; free (cue.data) ; free (data.data) ;
} /* test_labels_after_cue */

/* Two complete labels for the same id : the last one wins. A label whose id
** is not in the cue table is ignored.
*/
static void
test_duplicate_and_unknown_labels (void)
{	static const int32_t ids [] = { 1 } ;
	static const uint32_t offsets [] = { 0 } ;
	static const EXPECT_CUE expect [] = { { 1, 0, "Last" } } ;
	BUF adtl = { NULL, 0, 0 }, list = { NULL, 0, 0 }, body = { NULL, 0, 0 }, wav ;
	BUF fmt, cue, data ;

	buf_marker (&adtl, "adtl") ;
	buf_labl (&adtl, 1, "First", 5) ;
	buf_labl (&adtl, 9, "Ghost", 5) ;		/* id not in the cue table */
	/* A 'labl' with no text at all must not erase the previous label. */
	buf_marker (&adtl, "labl") ;
	buf_u32le (&adtl, 4) ;
	buf_u32le (&adtl, 1) ;
	buf_labl (&adtl, 1, "Last", 4) ;
	buf_chunk (&list, "LIST", &adtl) ;

	fmt = make_fmt_chunk () ;
	cue = make_cue_chunk (ids, offsets, 1) ;
	data = make_data_chunk () ;

	buf_chunk (&body, "fmt ", &fmt) ;
	buf_chunk (&body, "cue ", &cue) ;
	buf_put (&body, list.data, list.len) ;
	buf_chunk (&body, "data", &data) ;
	wav = make_wav (&body) ;

	check_via_paths (&wav, expect, 1, "duplicate_and_unknown_labels") ;

	free (adtl.data) ; free (list.data) ; free (body.data) ; free (wav.data) ;
	free (fmt.data) ; free (cue.data) ; free (data.data) ;
} /* test_duplicate_and_unknown_labels */

/* A label longer than SF_CUE_POINT.name can hold : truncated to 255 bytes,
** and the label after it must still be found.
*/
static void
test_long_label (void)
{	static const int32_t ids [] = { 1, 2 } ;
	static const uint32_t offsets [] = { 0, 2 } ;
	char long_name [301] ;
	char expect_name [256] ;
	EXPECT_CUE expect [2] ;
	BUF adtl = { NULL, 0, 0 }, list = { NULL, 0, 0 }, body = { NULL, 0, 0 }, wav ;
	BUF fmt, cue, data ;

	memset (long_name, 'a', 300) ;
	long_name [300] = 0 ;
	memset (expect_name, 'a', 255) ;
	expect_name [255] = 0 ;

	expect [0].id = 1 ; expect [0].offset = 0 ; expect [0].name = expect_name ;
	expect [1].id = 2 ; expect [1].offset = 2 ; expect [1].name = "After" ;

	buf_marker (&adtl, "adtl") ;
	buf_labl (&adtl, 1, long_name, 300) ;
	buf_labl (&adtl, 2, "After", 5) ;
	buf_chunk (&list, "LIST", &adtl) ;

	fmt = make_fmt_chunk () ;
	cue = make_cue_chunk (ids, offsets, 2) ;
	data = make_data_chunk () ;

	buf_chunk (&body, "fmt ", &fmt) ;
	buf_chunk (&body, "cue ", &cue) ;
	buf_put (&body, list.data, list.len) ;
	buf_chunk (&body, "data", &data) ;
	wav = make_wav (&body) ;

	check_via_paths (&wav, expect, 2, "long_label") ;

	free (adtl.data) ; free (list.data) ; free (body.data) ; free (wav.data) ;
	free (fmt.data) ; free (cue.data) ; free (data.data) ;
} /* test_long_label */

/* A 'labl' whose declared length runs past the end of its LIST : the rest of
** the LIST is abandoned, but the complete label before it and the top level
** chunks after the LIST must survive.
*/
static void
test_truncated_labl (void)
{	static const int32_t ids [] = { 1, 2 } ;
	static const uint32_t offsets [] = { 0, 2 } ;
	static const EXPECT_CUE expect [] =
	{	{ 1, 0, "Good" },
		{ 2, 2, "" }			/* its label is broken : stays empty */
	} ;
	BUF adtl = { NULL, 0, 0 }, list = { NULL, 0, 0 }, body = { NULL, 0, 0 }, wav ;
	BUF fmt, cue, data ;

	buf_marker (&adtl, "adtl") ;
	buf_labl (&adtl, 1, "Good", 4) ;
	buf_marker (&adtl, "labl") ;
	buf_u32le (&adtl, 1000) ;				/* claims far more than the LIST holds */
	buf_u32le (&adtl, 2) ;
	buf_str (&adtl, "Bad") ;
	buf_chunk (&list, "LIST", &adtl) ;

	fmt = make_fmt_chunk () ;
	cue = make_cue_chunk (ids, offsets, 2) ;
	data = make_data_chunk () ;

	buf_chunk (&body, "fmt ", &fmt) ;
	buf_chunk (&body, "cue ", &cue) ;
	buf_put (&body, list.data, list.len) ;
	buf_chunk (&body, "data", &data) ;		/* must still be parsed */
	wav = make_wav (&body) ;

	check_via_paths (&wav, expect, 2, "truncated_labl") ;

	free (adtl.data) ; free (list.data) ; free (body.data) ; free (wav.data) ;
	free (fmt.data) ; free (cue.data) ; free (data.data) ;
} /* test_truncated_labl */

/* The LIST itself declares a length running past the end of the file. The
** complete label inside it must survive, the broken rest is dropped.
*/
static void
test_truncated_list (void)
{	static const int32_t ids [] = { 1 } ;
	static const uint32_t offsets [] = { 1 } ;
	static const EXPECT_CUE expect [] = { { 1, 1, "Kept" } } ;
	BUF body = { NULL, 0, 0 }, wav ;
	BUF fmt, cue, data ;

	fmt = make_fmt_chunk () ;
	cue = make_cue_chunk (ids, offsets, 1) ;
	data = make_data_chunk () ;

	buf_chunk (&body, "fmt ", &fmt) ;
	buf_chunk (&body, "data", &data) ;
	buf_chunk (&body, "cue ", &cue) ;

	/* LIST with a wildly exaggerated length, closed by end of file. */
	buf_marker (&body, "LIST") ;
	buf_u32le (&body, 0x00FFFF00) ;
	buf_marker (&body, "adtl") ;
	buf_labl (&body, 1, "Kept", 4) ;
	buf_marker (&body, "labl") ;
	buf_u32le (&body, 500) ;				/* truncated : file ends shortly after */
	buf_u32le (&body, 1) ;
	buf_str (&body, "Gone") ;

	wav = make_wav (&body) ;

	check_via_paths (&wav, expect, 1, "truncated_list") ;

	free (body.data) ; free (wav.data) ;
	free (fmt.data) ; free (cue.data) ; free (data.data) ;
} /* test_truncated_list */

/* An old style WAV without any cue or label chunks. */
static void
test_no_cues (void)
{	BUF body = { NULL, 0, 0 }, wav ;
	BUF fmt, data ;

	fmt = make_fmt_chunk () ;
	data = make_data_chunk () ;

	buf_chunk (&body, "fmt ", &fmt) ;
	buf_chunk (&body, "data", &data) ;
	wav = make_wav (&body) ;

	check_via_paths (&wav, NULL, 0, "no_cues") ;

	free (body.data) ; free (wav.data) ;
	free (fmt.data) ; free (data.data) ;
} /* test_no_cues */

/*------------------------------------------------------------------------------
** Write direction : independent walk of the RIFF bytes produced by the
** library, checking chunk lengths, ids, NUL terminators, padding and the
** audio position.
*/

static uint32_t
rd_u32le (const uint8_t *p)
{	return (uint32_t) p [0] | ((uint32_t) p [1] << 8) | ((uint32_t) p [2] << 16) | ((uint32_t) p [3] << 24) ;
} /* rd_u32le */

static int
marker_eq (const uint8_t *p, const char *id)
{	return memcmp (p, id, 4) == 0 ;
} /* marker_eq */

typedef struct
{	int32_t		id ;
	const char	*name ;
} EXPECT_LABEL ;

static void
check_list_chunk (const uint8_t *p, uint32_t size, const EXPECT_LABEL *labels, int label_count)
{	uint32_t pos, found = 0 ;

	CHECK (marker_eq (p, "adtl"), "LIST chunk is not of type 'adtl'") ;
	if (! marker_eq (p, "adtl"))
		return ;

	pos = 4 ;
	while (pos + 8 <= size)
	{	uint32_t sub_size = rd_u32le (p + pos + 4) ;
		uint32_t sub_total = 8 + sub_size + (sub_size & 1) ;
		const char *want_name = NULL ;
		uint32_t want_len = 0, name_len ;
		int32_t id ;
		int k ;

		CHECK (marker_eq (p + pos, "labl"), "LIST/adtl contains a non-'labl' subchunk") ;
		CHECK (pos + sub_total <= size, "'labl' subchunk overruns its LIST") ;
		if (! marker_eq (p + pos, "labl") || pos + sub_total > size)
			return ;

		id = (int32_t) rd_u32le (p + pos + 8) ;

		for (k = 0 ; k < label_count ; k++)
			if (labels [k].id == id)
			{	want_name = labels [k].name ;
				want_len = (uint32_t) strlen (want_name) ;
				break ;
				} ;

		CHECK (want_name != NULL, "unexpected 'labl' cue id") ;
		if (want_name == NULL)
			return ;

		/* Subchunk length excludes the pad byte. */
		CHECK (sub_size == 4 + want_len + 1, "'labl' length field wrong") ;
		/* Name bytes are stored verbatim and NUL terminated. */
		CHECK (memcmp (p + pos + 12, want_name, want_len) == 0, "'labl' name bytes wrong") ;
		CHECK (p [pos + 12 + want_len] == 0, "'labl' name not NUL terminated") ;
		/* An odd sized payload is followed by a pad byte. */
		if (sub_size & 1)
			CHECK (p [pos + 8 + sub_size] == 0, "'labl' pad byte missing") ;

		name_len = 0 ;
		while (name_len < want_len && p [pos + 12 + name_len] != 0)
			name_len ++ ;
		CHECK (name_len == want_len, "'labl' name shorter than expected") ;

		found ++ ;
		pos += sub_total ;
		} ;

	CHECK (pos == size, "LIST/adtl size does not match its subchunks") ;
	CHECK ((int) found == label_count, "wrong number of 'labl' subchunks") ;
} /* check_list_chunk */

static void
check_written_file (const char *path, const EXPECT_CUE *cues, int cue_count,
					const EXPECT_LABEL *labels, int label_count, int expect_list)
{	uint8_t *file ;
	size_t len, pos ;
	int seen_cue = 0, seen_list = 0, seen_data = 0 ;

	if ((file = read_file (path, &len)) == NULL)
	{	printf ("FAIL : cannot read back %s\n", path) ;
		failures ++ ;
		return ;
		} ;

	CHECK (len >= 12, "file too short for RIFF") ;
	CHECK (marker_eq (file, "RIFF"), "missing RIFF marker") ;
	CHECK (rd_u32le (file + 4) == len - 8, "RIFF size does not cover the whole file") ;
	CHECK (marker_eq (file + 8, "WAVE"), "missing WAVE marker") ;
	if (len < 12)
	{	free (file) ;
		return ;
		} ;

	pos = 12 ;
	while (pos + 8 <= len)
	{	const uint8_t *chunk = file + pos ;
		uint32_t size = rd_u32le (chunk + 4) ;
		size_t total = 8 + (size_t) size + (size & 1) ;

		CHECK (pos + total <= len, "chunk overruns the file") ;
		if (pos + total > len)
			break ;

		if (marker_eq (chunk, "cue "))
		{	int k ;

			seen_cue ++ ;
			CHECK (size == 4 + (uint32_t) cue_count * 24, "'cue ' chunk length wrong") ;
			CHECK ((int) rd_u32le (chunk + 8) == cue_count, "'cue ' count wrong") ;
			for (k = 0 ; k < cue_count ; k++)
			{	const uint8_t *e = chunk + 12 + k * 24 ;
				CHECK ((int32_t) rd_u32le (e) == cues [k].id, "'cue ' id wrong") ;
				CHECK (rd_u32le (e + 4) == cues [k].offset, "'cue ' position wrong") ;
				CHECK (marker_eq (e + 8, "data"), "'cue ' chunk id is not 'data'") ;
				CHECK (rd_u32le (e + 20) == cues [k].offset, "'cue ' sample offset wrong") ;
				} ;
			}
		else if (marker_eq (chunk, "LIST"))
		{	seen_list ++ ;
			check_list_chunk (chunk + 8, size, labels, label_count) ;
			}
		else if (marker_eq (chunk, "data"))
		{	seen_data ++ ;
			/* The audio must sit here, unmoved and unharmed. */
			CHECK (size == TEST_FRAMES * TEST_CHANNELS * 2, "'data' chunk length wrong") ;
			if (size == TEST_FRAMES * TEST_CHANNELS * 2)
			{	short pcm [TEST_FRAMES * TEST_CHANNELS] ;
				int k ;
				for (k = 0 ; k < TEST_FRAMES * TEST_CHANNELS ; k++)
					pcm [k] = (short) (chunk [8 + 2 * k] | (chunk [8 + 2 * k + 1] << 8)) ;
				CHECK (memcmp (pcm, test_pcm, sizeof (test_pcm)) == 0, "PCM data moved or damaged") ;
				} ;
			} ;

		pos += total ;
		} ;

	CHECK (pos == len, "trailing garbage after the last chunk") ;
	CHECK (seen_cue == 1, "expected exactly one 'cue ' chunk") ;
	CHECK (seen_data == 1, "expected exactly one 'data' chunk") ;
	CHECK (seen_list == expect_list, "wrong number of LIST chunks (duplicate label lists?)") ;

	free (file) ;
} /* check_written_file */

/* Set named cues, write audio in two goes with a header update and a sync in
** between, then verify both the raw bytes and the library read back.
*/
static void
test_write_labels (void)
{	static const EXPECT_CUE expect [] =
	{	{ 10, 0, "Intro" },		/* even sized payload */
		{ 20, 2, "" },			/* empty name : no 'labl' */
		{ 30, 2, "Hook" },		/* odd sized payload, same position as above */
		{ 40, 3, "Intro" }		/* same name as id 10 : no merging */
	} ;
	static const EXPECT_LABEL labels [] =
	{	{ 10, "Intro" },
		{ 30, "Hook" },
		{ 40, "Intro" }
	} ;
	SF_CUES_VAR (8) cues ;
	SF_INFO info ;
	SNDFILE *file ;
	int k ;

	memset (&info, 0, sizeof (info)) ;
	info.samplerate	= 48000 ;
	info.channels	= TEST_CHANNELS ;
	info.format		= SF_FORMAT_WAV | SF_FORMAT_PCM_16 ;

	if ((file = sf_open (TEST_FILE, SFM_WRITE, &info)) == NULL)
	{	printf ("FAIL : test_write_labels : sf_open failed : %s\n", sf_strerror (NULL)) ;
		failures ++ ;
		return ;
		} ;

	memset (&cues, 0, sizeof (cues)) ;
	cues.cue_count = 4 ;
	for (k = 0 ; k < 4 ; k++)
	{	cues.cue_points [k].indx = expect [k].id ;
		cues.cue_points [k].position = expect [k].offset ;
		cues.cue_points [k].fcc_chunk = 0x61746164 ;	/* 'data' */
		cues.cue_points [k].sample_offset = expect [k].offset ;
		snprintf (cues.cue_points [k].name, sizeof (cues.cue_points [k].name), "%s", expect [k].name) ;
		} ;

	CHECK (sf_command (file, SFC_SET_CUE, &cues, sizeof (cues)) == SF_TRUE, "SFC_SET_CUE failed") ;

	/* First half of the audio, then update the header and sync. */
	CHECK (sf_writef_short (file, test_pcm, 2) == 2, "first audio write failed") ;
	sf_command (file, SFC_UPDATE_HEADER_NOW, NULL, 0) ;
	sf_write_sync (file) ;

	/* Second half of the audio. */
	CHECK (sf_writef_short (file, test_pcm + 2 * TEST_CHANNELS, 2) == 2, "second audio write failed") ;
	CHECK (sf_close (file) == 0, "sf_close failed") ;

	/* Independent check of the RIFF bytes. */
	check_written_file (TEST_FILE, expect, 4, labels, 3, 1) ;

	/* Read back through both open paths, audio included. */
	{	size_t len ;
		uint8_t *bytes = read_file (TEST_FILE, &len) ;
		BUF wav ;
		if (bytes == NULL)
		{	printf ("FAIL : test_write_labels : cannot read back written file\n") ;
			failures ++ ;
			}
		else
		{	wav.data = bytes ;
			wav.len = len ;
			wav.cap = len ;
			check_via_paths (&wav, expect, 4, "write_labels_readback") ;
			free (bytes) ;
			} ;
		} ;

	remove (TEST_FILE) ;
} /* test_write_labels */

/* Cues without names must not produce a LIST/adtl chunk. */
static void
test_write_unnamed_cues (void)
{	static const EXPECT_CUE expect [] =
	{	{ 1, 0, "" },
		{ 2, 2, "" }
	} ;
	SF_CUES_VAR (8) cues ;
	SF_INFO info ;
	SNDFILE *file ;
	int k ;

	memset (&info, 0, sizeof (info)) ;
	info.samplerate	= 48000 ;
	info.channels	= TEST_CHANNELS ;
	info.format		= SF_FORMAT_WAV | SF_FORMAT_PCM_16 ;

	if ((file = sf_open (TEST_FILE, SFM_WRITE, &info)) == NULL)
	{	printf ("FAIL : test_write_unnamed_cues : sf_open failed : %s\n", sf_strerror (NULL)) ;
		failures ++ ;
		return ;
		} ;

	memset (&cues, 0, sizeof (cues)) ;
	cues.cue_count = 2 ;
	for (k = 0 ; k < 2 ; k++)
	{	cues.cue_points [k].indx = expect [k].id ;
		cues.cue_points [k].position = expect [k].offset ;
		cues.cue_points [k].fcc_chunk = 0x61746164 ;	/* 'data' */
		cues.cue_points [k].sample_offset = expect [k].offset ;
		} ;

	CHECK (sf_command (file, SFC_SET_CUE, &cues, sizeof (cues)) == SF_TRUE, "SFC_SET_CUE failed") ;
	CHECK (sf_writef_short (file, test_pcm, TEST_FRAMES) == TEST_FRAMES, "audio write failed") ;
	CHECK (sf_close (file) == 0, "sf_close failed") ;

	check_written_file (TEST_FILE, expect, 2, NULL, 0, 0) ;

	{	size_t len ;
		uint8_t *bytes = read_file (TEST_FILE, &len) ;
		BUF wav ;
		if (bytes == NULL)
		{	printf ("FAIL : test_write_unnamed_cues : cannot read back written file\n") ;
			failures ++ ;
			}
		else
		{	wav.data = bytes ;
			wav.len = len ;
			wav.cap = len ;
			check_via_paths (&wav, expect, 2, "write_unnamed_readback") ;
			free (bytes) ;
			} ;
		} ;

	remove (TEST_FILE) ;
} /* test_write_unnamed_cues */

/* Setting cues after audio has been written must still be rejected. */
static void
test_set_cue_after_write_rejected (void)
{	SF_CUES_VAR (8) cues ;
	SF_INFO info ;
	SNDFILE *file ;

	memset (&info, 0, sizeof (info)) ;
	info.samplerate	= 48000 ;
	info.channels	= TEST_CHANNELS ;
	info.format		= SF_FORMAT_WAV | SF_FORMAT_PCM_16 ;

	if ((file = sf_open (TEST_FILE, SFM_WRITE, &info)) == NULL)
	{	printf ("FAIL : test_set_cue_after_write_rejected : sf_open failed : %s\n", sf_strerror (NULL)) ;
		failures ++ ;
		return ;
		} ;

	CHECK (sf_writef_short (file, test_pcm, 1) == 1, "audio write failed") ;

	memset (&cues, 0, sizeof (cues)) ;
	cues.cue_count = 1 ;
	cues.cue_points [0].indx = 1 ;
	snprintf (cues.cue_points [0].name, sizeof (cues.cue_points [0].name), "Late") ;

	CHECK (sf_command (file, SFC_SET_CUE, &cues, sizeof (cues)) == SF_FALSE,
			"SFC_SET_CUE after writing audio should fail") ;

	CHECK (sf_close (file) == 0, "sf_close failed") ;
	remove (TEST_FILE) ;
} /* test_set_cue_after_write_rejected */

/* AIFF cue behaviour, including marker names, must be unchanged. */
static void
test_aiff_cues_unchanged (void)
{	SF_CUES_VAR (8) cues ;
	SF_INFO info ;
	SNDFILE *file ;

	memset (&info, 0, sizeof (info)) ;
	info.samplerate	= 48000 ;
	info.channels	= TEST_CHANNELS ;
	info.format		= SF_FORMAT_AIFF | SF_FORMAT_PCM_16 ;

	if ((file = sf_open (TEST_FILE, SFM_WRITE, &info)) == NULL)
	{	printf ("FAIL : test_aiff_cues_unchanged : sf_open failed : %s\n", sf_strerror (NULL)) ;
		failures ++ ;
		return ;
		} ;

	memset (&cues, 0, sizeof (cues)) ;
	cues.cue_count = 1 ;
	cues.cue_points [0].indx = 3 ;
	cues.cue_points [0].sample_offset = 2 ;
	snprintf (cues.cue_points [0].name, sizeof (cues.cue_points [0].name), "Mark") ;

	CHECK (sf_command (file, SFC_SET_CUE, &cues, sizeof (cues)) == SF_TRUE, "SFC_SET_CUE (AIFF) failed") ;
	CHECK (sf_writef_short (file, test_pcm, TEST_FRAMES) == TEST_FRAMES, "audio write failed") ;
	CHECK (sf_close (file) == 0, "sf_close failed") ;

	memset (&info, 0, sizeof (info)) ;
	if ((file = sf_open (TEST_FILE, SFM_READ, &info)) == NULL)
	{	printf ("FAIL : test_aiff_cues_unchanged : sf_open read failed : %s\n", sf_strerror (NULL)) ;
		failures ++ ;
		}
	else
	{	uint32_t cue_count = 0 ;
		CHECK (sf_command (file, SFC_GET_CUE_COUNT, &cue_count, sizeof (cue_count)) == SF_TRUE,
				"SFC_GET_CUE_COUNT (AIFF) failed") ;
		CHECK (cue_count == 1, "AIFF cue count mismatch") ;
		memset (&cues, 0, sizeof (cues)) ;
		CHECK (sf_command (file, SFC_GET_CUE, &cues, sizeof (cues)) == SF_TRUE, "SFC_GET_CUE (AIFF) failed") ;
		CHECK (cues.cue_points [0].indx == 3, "AIFF cue id mismatch") ;
		CHECK (strcmp (cues.cue_points [0].name, "Mark") == 0, "AIFF cue name mismatch") ;
		sf_close (file) ;
		} ;

	remove (TEST_FILE) ;
} /* test_aiff_cues_unchanged */

int
main (void)
{	test_labels_before_cue () ;
	test_labels_after_cue () ;
	test_duplicate_and_unknown_labels () ;
	test_long_label () ;
	test_truncated_labl () ;
	test_truncated_list () ;
	test_no_cues () ;

	test_write_labels () ;
	test_write_unnamed_cues () ;
	test_set_cue_after_write_rejected () ;
	test_aiff_cues_unchanged () ;

	if (failures > 0)
	{	printf ("%d check(s) failed.\n", failures) ;
		return 1 ;
		} ;

	puts ("WAV_CUE_LABELS_OK") ;
	return 0 ;
} /* main */
