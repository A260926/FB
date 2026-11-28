/* ///////////////////////////////////////////////////////////////////////////
//                                                                          //
// tests/sample_range.c                                                     //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// Regression tests for `ttaR decode --samples=START:END'.                  //
//                                                                          //
// Materials are produced by driving the real encoder/decoder through the   //
// built ttaR binary. Each case verifies raw output sample-for-sample, and  //
// parses the WAV/W64 containers for both payload bytes and declared sizes. //
//                                                                          //
/////////////////////////////////////////////////////////////////////////// */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/libttaR.h"
#include "task_support/check.h"

/* //////////////////////////////////////////////////////////////////////// */

#define CLI		".task-build/ttaR"
#define WORKDIR		".task-build/sample_range"

#define NCHAN		2u
#define SAMPLEBYTES	2u
#define SAMPLERATE	8000u
#define FRAMELEN	((size_t) ((SAMPLERATE * 256u) / 245u))

/* number of per-channel sample frames in the main test material */
#define TOTAL_FRAMES	40000u

/* ------------------------------------------------------------------------ */

/* expected raw PCM for the asymmetric test signal, per channel */
static int32_t expected_sample(size_t frame, unsigned int chan)
{
	const int32_t base = (int32_t) (frame % 65536u);
	int32_t v;

	if ( chan == 0u ){
		v = base * 37 + (int32_t) ((frame / 13u) % 65536u) * 101;
	}
	else {	v = base * 911 + (int32_t) ((frame / 7u) % 65536u) * 313
			+ (int32_t) ((frame % 251u) * 17u);
	}
	return (int32_t) (int16_t) (uint16_t) v;
}

/* 8-bit unsigned mono sample (stored raw, no interleave) */
static unsigned int expected_u8(size_t frame)
{
	return (unsigned int) ((frame * 37u + frame / 13u) % 256u);
}

/* 24-bit signed stereo sample, asymmetrical per channel */
static int32_t expected_i24(size_t frame, unsigned int chan)
{
	const int32_t span = (int32_t) 1 << 23;

	return (int32_t) (
		((frame * 37u + chan * 13u) % 16777216u)
	) - span;
}

/* ------------------------------------------------------------------------ */

static int run(const char *cmd)
{
	int rc = system(cmd);
	if ( rc != 0 ){
		fprintf(stderr, "command failed (%d): %s\n", rc, cmd);
	}
	return rc;
}

static int file_exists(const char *path)
{
	FILE *f = fopen(path, "rb");
	if ( f == NULL ){ return 0; }
	fclose(f);
	return 1;
}

/* read an entire file into a malloc'd buffer */
static unsigned char *read_file(const char *path, size_t *len_out)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
	long len;

	if ( f == NULL ){ return NULL; }
	if ( fseek(f, 0, SEEK_END) != 0 ){ fclose(f); return NULL; }
	len = ftell(f);
	if ( len < 0 ){ fclose(f); return NULL; }
	rewind(f);
	buf = malloc((size_t) len + 1u);
	if ( buf == NULL ){ fclose(f); return NULL; }
	if ( fread(buf, 1u, (size_t) len, f) != (size_t) len ){
		free(buf); fclose(f); return NULL;
	}
	fclose(f);
	*len_out = (size_t) len;
	return buf;
}

/* compare a raw PCM file with the reference range */
static int raw_matches_range(const char *path, size_t start, size_t end)
{
	size_t len = 0, want, i;
	unsigned char *got = read_file(path, &len);

	if ( got == NULL ){ return 0; }
	want = (end - start) * NCHAN * SAMPLEBYTES;
	if ( len != want ){ free(got); return 0; }
	for ( i = 0; i < len; i += 2u ){
		const size_t frame = start + (i / (NCHAN * SAMPLEBYTES));
		const unsigned int chan = (unsigned int)
			((i / SAMPLEBYTES) % NCHAN);
		const int32_t s = expected_sample(frame, chan);
		const uint16_t u = (uint16_t) s;
		if ( got[i] != (unsigned char) (u & 0xFFu)
		  || got[i + 1u] != (unsigned char) (u >> 8u) ){
			free(got); return 0;
		}
	}
	free(got);
	return 1;
}

/* locate a byte substring in buf; returns index or (size_t)-1 */
static size_t find_bytes(
	const unsigned char *buf, size_t len,
	const unsigned char *needle, size_t nlen
)
{
	size_t i;
	if ( len < nlen ){ return (size_t) -1; }
	for ( i = 0; i + nlen <= len; ++i ){
		if ( memcmp(&buf[i], needle, nlen) == 0 ){ return i; }
	}
	return (size_t) -1;
}

/* find a byte substring in a buffer (portable memmem) */
static int buffer_contains(
	const unsigned char *buf, size_t len,
	const char *needle, size_t nlen
)
{
	return find_bytes(buf, len, (const unsigned char *) needle, nlen)
		!= (size_t) -1;
}

static uint32_t le_u32(const unsigned char *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8u)
	     | ((uint32_t) p[2] << 16u) | ((uint32_t) p[3] << 24u);
}

static uint64_t le_u64(const unsigned char *p)
{
	uint64_t v = 0;
	unsigned int i;
	for ( i = 0; i < 8u; ++i ){ v |= (uint64_t) p[i] << (8u * i); }
	return v;
}

/* verify a WAV file: declared data size and payload both match the range */
static int wav_matches_range(const char *path, size_t start, size_t end)
{
	size_t len = 0, idx, payload_len, i;
	unsigned char *buf = read_file(path, &len);
	const unsigned char data_id[4] = { 'd', 'a', 't', 'a' };
	uint32_t declared;
	int ok = 0;

	if ( buf == NULL ){ return 0; }
	idx = find_bytes(buf, len, data_id, sizeof data_id);
	if ( idx == (size_t) -1 || idx + 8u > len ){ goto done; }
	declared = le_u32(&buf[idx + 4u]);
	payload_len = (end - start) * NCHAN * SAMPLEBYTES;
	if ( (size_t) declared != payload_len ){ goto done; }
	if ( idx + 8u + payload_len != len ){ goto done; }
	for ( i = 0; i < payload_len; i += 2u ){
		const size_t frame = start + (i / (NCHAN * SAMPLEBYTES));
		const unsigned int chan = (unsigned int)
			((i / SAMPLEBYTES) % NCHAN);
		const uint16_t u = (uint16_t) expected_sample(frame, chan);
		if ( buf[idx + 8u + i] != (unsigned char) (u & 0xFFu) ||
		     buf[idx + 8u + i + 1u] != (unsigned char) (u >> 8u) ){
			goto done;
		}
	}
	ok = 1;
done:
	free(buf);
	return ok;
}

/* verify a W64 file: declared data size and payload both match the range */
static int w64_matches_range(const char *path, size_t start, size_t end)
{
	size_t len = 0, idx, payload_len, i;
	unsigned char *buf = read_file(path, &len);
	/* the "data" GUID: 'd','a','t','a',F3,AC,D3,11,... */
	const unsigned char data_guid[8] = {
		'd', 'a', 't', 'a', 0xF3, 0xAC, 0xD3, 0x11
	};
	uint64_t declared;
	int ok = 0;

	if ( buf == NULL ){ return 0; }
	idx = find_bytes(buf, len, data_guid, sizeof data_guid);
	if ( idx == (size_t) -1 || idx + 24u > len ){ goto done; }
	/* chunk size includes the 24-byte chunk header */
	declared = le_u64(&buf[idx + 16u]);
	payload_len = (end - start) * NCHAN * SAMPLEBYTES;
	if ( declared != (uint64_t) (payload_len + 24u) ){ goto done; }
	if ( idx + 24u + payload_len != len ){ goto done; }
	for ( i = 0; i < payload_len; i += 2u ){
		const size_t frame = start + (i / (NCHAN * SAMPLEBYTES));
		const unsigned int chan = (unsigned int)
			((i / SAMPLEBYTES) % NCHAN);
		const uint16_t u = (uint16_t) expected_sample(frame, chan);
		if ( buf[idx + 24u + i] != (unsigned char) (u & 0xFFu) ||
		     buf[idx + 24u + i + 1u] != (unsigned char) (u >> 8u) ){
			goto done;
		}
	}
	ok = 1;
done:
	free(buf);
	return ok;
}

/* ======================================================================== */

/* one-time setup: build the test material with the real encoder */
static int ensure_material(void)
{
	static int done = 0;
	FILE *f;
	size_t frame;
	unsigned int chan;
	char cmd[512];

	if ( done ){ return 0; }

	(void) run("mkdir -p " WORKDIR);

	f = fopen(WORKDIR "/signal.raw", "wb");
	if ( f == NULL ){ return -1; }
	for ( frame = 0; frame < TOTAL_FRAMES; ++frame ){
		for ( chan = 0; chan < NCHAN; ++chan ){
			const uint16_t u = (uint16_t) expected_sample(frame, chan);
			if ( fputc((int) (u & 0xFFu), f) == EOF ||
			     fputc((int) (u >> 8u), f) == EOF ){
				fclose(f); return -1;
			}
		}
	}
	if ( fclose(f) != 0 ){ return -1; }

	(void) snprintf(cmd, sizeof cmd,
		CLI " encode -S -q --rawpcm=i16le,%u,%u "
		WORKDIR "/signal.raw -o " WORKDIR "/material.tta",
		SAMPLERATE, NCHAN
	);
	if ( run(cmd) != 0 ){ return -1; }

	/* 8-bit unsigned, mono material (conversion preservation) */
	f = fopen(WORKDIR "/u8.raw", "wb");
	if ( f == NULL ){ return -1; }
	for ( frame = 0; frame < TOTAL_FRAMES; ++frame ){
		if ( fputc((int) expected_u8(frame), f) == EOF ){
			fclose(f); return -1;
		}
	}
	if ( fclose(f) != 0 ){ return -1; }
	(void) snprintf(cmd, sizeof cmd,
		CLI " encode -S -q --rawpcm=u8,%u,1 "
		WORKDIR "/u8.raw -o " WORKDIR "/u8.tta", SAMPLERATE
	);
	if ( run(cmd) != 0 ){ return -1; }

	/* 24-bit signed, stereo material (conversion preservation) */
	f = fopen(WORKDIR "/i24.raw", "wb");
	if ( f == NULL ){ return -1; }
	for ( frame = 0; frame < TOTAL_FRAMES; ++frame ){
		for ( chan = 0; chan < NCHAN; ++chan ){
			const int32_t s = expected_i24(frame, chan);
			const uint32_t u = (uint32_t) s;
			if ( fputc((int) (u & 0xFFu), f) == EOF ||
			     fputc((int) ((u >> 8u) & 0xFFu), f) == EOF ||
			     fputc((int) ((u >> 16u) & 0xFFu), f) == EOF ){
				fclose(f); return -1;
			}
		}
	}
	if ( fclose(f) != 0 ){ return -1; }
	(void) snprintf(cmd, sizeof cmd,
		CLI " encode -S -q --rawpcm=i24le,%u,%u "
		WORKDIR "/i24.raw -o " WORKDIR "/i24.tta",
		SAMPLERATE, NCHAN
	);
	if ( run(cmd) != 0 ){ return -1; }

	done = 1;
	return 0;
}

/* decode one range with the given thread flags into a file */
static int decode_range(
	const char *infile, const char *outfile, const char *fmt,
	size_t start, size_t end, int multithreaded
)
{
	char cmd[1024];
	(void) snprintf(cmd, sizeof cmd,
		CLI " decode %s -q -f %s --samples=%zu:%zu %s -o %s",
		multithreaded ? "-M -t 2" : "-S", fmt, start, end,
		infile, outfile
	);
	return run(cmd);
}

/* ======================================================================== */
/* slices: frame-locality of the selection                                  */

static int check_range_both(
	size_t start, size_t end, const char *tag
)
{
	char out_s[256], out_m[256];
	(void) tag;

	(void) snprintf(out_s, sizeof out_s, WORKDIR "/slice_s.raw");
	(void) snprintf(out_m, sizeof out_m, WORKDIR "/slice_m.raw");

	if ( decode_range(WORKDIR "/material.tta", out_s, "raw",
			  start, end, 0) != 0 ){ return 1; }
	if ( decode_range(WORKDIR "/material.tta", out_m, "raw",
			  start, end, 1) != 0 ){ return 1; }

	TASK_CHECK(raw_matches_range(out_s, start, end));
	TASK_CHECK(raw_matches_range(out_m, start, end));
	return 0;
}

static int slice_same_frame(void)
{
	if ( ensure_material() != 0 ){ return 1; }
	return check_range_both(100u, 900u, "same-frame");
}

static int slice_cross_frame(void)
{
	if ( ensure_material() != 0 ){ return 1; }
	/* spans frame 1 into frame 3 */
	return check_range_both(FRAMELEN + 10u, 3u * FRAMELEN - 10u,
		"cross-frame");
}

static int slice_frame_boundary(void)
{
	if ( ensure_material() != 0 ){ return 1; }
	/* exact start on a frame boundary, exact end on one */
	return check_range_both(2u * FRAMELEN, 4u * FRAMELEN,
		"frame-boundary");
}

static int slice_last_short_frame(void)
{
	if ( ensure_material() != 0 ){ return 1; }
	/* the tail beyond the last whole frame (TOTAL_FRAMES % FRAMELEN) */
	return check_range_both(
		(TOTAL_FRAMES / FRAMELEN) * FRAMELEN, TOTAL_FRAMES,
		"last-short-frame"
	);
}

static int slice_empty(void)
{
	if ( ensure_material() != 0 ){ return 1; }
	return check_range_both(12345u, 12345u, "empty");
}

/* ======================================================================== */
/* queues: ring-buffer rotation, ordering, tiny spans across many rotations */

static int queue_many_rotations(void)
{
	if ( ensure_material() != 0 ){ return 1; }
	/* far more frames than FRAMEQUEUE_LEN(2) == 4 */
	return check_range_both(1000u, TOTAL_FRAMES - 1000u,
		"many-rotations");
}

static int queue_full_file(void)
{
	if ( ensure_material() != 0 ){ return 1; }
	return check_range_both(0u, TOTAL_FRAMES, "full-file");
}

static int queue_first_sample(void)
{
	if ( ensure_material() != 0 ){ return 1; }
	return check_range_both(0u, 1u, "first-sample");
}

static int queue_last_sample(void)
{
	if ( ensure_material() != 0 ){ return 1; }
	return check_range_both(TOTAL_FRAMES - 1u, TOTAL_FRAMES,
		"last-sample");
}

/* ======================================================================== */
/* formats: WAV/W64 payload and declared lengths                            */

static int format_wav(void)
{
	char out_s[256], out_m[256];
	size_t a = 3000u, b = 3000u + FRAMELEN + 500u;

	if ( ensure_material() != 0 ){ return 1; }
	(void) snprintf(out_s, sizeof out_s, WORKDIR "/fmt_s.wav");
	(void) snprintf(out_m, sizeof out_m, WORKDIR "/fmt_m.wav");

	if ( decode_range(WORKDIR "/material.tta", out_s, "wav",
			  a, b, 0) != 0 ){ return 1; }
	if ( decode_range(WORKDIR "/material.tta", out_m, "wav",
			  a, b, 1) != 0 ){ return 1; }
	TASK_CHECK(wav_matches_range(out_s, a, b));
	TASK_CHECK(wav_matches_range(out_m, a, b));
	return 0;
}

static int format_w64(void)
{
	char out_s[256], out_m[256];
	size_t a = 500u, b = 2u * FRAMELEN + 1u;

	if ( ensure_material() != 0 ){ return 1; }
	(void) snprintf(out_s, sizeof out_s, WORKDIR "/fmt_s.w64");
	(void) snprintf(out_m, sizeof out_m, WORKDIR "/fmt_m.w64");

	if ( decode_range(WORKDIR "/material.tta", out_s, "w64",
			  a, b, 0) != 0 ){ return 1; }
	if ( decode_range(WORKDIR "/material.tta", out_m, "w64",
			  a, b, 1) != 0 ){ return 1; }
	TASK_CHECK(w64_matches_range(out_s, a, b));
	TASK_CHECK(w64_matches_range(out_m, a, b));
	return 0;
}

static int format_raw_stereo_conversion(void)
{
	/* raw output must be the interleaved i16le range, channel-correct */
	if ( ensure_material() != 0 ){ return 1; }
	return check_range_both(FRAMELEN - 2u, FRAMELEN + 2u,
		"raw-stereo");
}

/* compare a raw file against a byte-per-sample reference */
static int raw_matches_u8(const char *path, size_t start, size_t end)
{
	size_t len = 0, want, i;
	unsigned char *got = read_file(path, &len);

	if ( got == NULL ){ return 0; }
	want = end - start;
	if ( len != want ){ free(got); return 0; }
	for ( i = 0; i < len; ++i ){
		if ( got[i] != (unsigned char) expected_u8(start + i) ){
			free(got); return 0;
		}
	}
	free(got);
	return 1;
}

static int format_u8_mono(void)
{
	char out_s[256], out_m[256];
	size_t a = 10u, b = 3u * FRAMELEN + 7u;

	if ( ensure_material() != 0 ){ return 1; }
	(void) snprintf(out_s, sizeof out_s, WORKDIR "/u8_s.raw");
	(void) snprintf(out_m, sizeof out_m, WORKDIR "/u8_m.raw");

	if ( decode_range(WORKDIR "/u8.tta", out_s, "raw", a, b, 0) != 0 ){
		return 1;
	}
	if ( decode_range(WORKDIR "/u8.tta", out_m, "raw", a, b, 1) != 0 ){
		return 1;
	}
	TASK_CHECK(raw_matches_u8(out_s, a, b));
	TASK_CHECK(raw_matches_u8(out_m, a, b));
	return 0;
}

static int format_i24_stereo(void)
{
	char out_s[256], out_m[256];
	size_t len = 0, a = 100u, b = 2u * FRAMELEN + 3u;
	size_t i, want;
	unsigned char *got;

	if ( ensure_material() != 0 ){ return 1; }
	(void) snprintf(out_s, sizeof out_s, WORKDIR "/i24_s.raw");
	(void) snprintf(out_m, sizeof out_m, WORKDIR "/i24_m.raw");

	if ( decode_range(WORKDIR "/i24.tta", out_s, "raw", a, b, 0) != 0 ){
		return 1;
	}
	if ( decode_range(WORKDIR "/i24.tta", out_m, "raw", a, b, 1) != 0 ){
		return 1;
	}

	want = (b - a) * NCHAN * 3u;
	got = read_file(out_s, &len);
	TASK_CHECK(got != NULL);
	if ( got != NULL ){
		TASK_CHECK(len == want);
		if ( len == want ){
			int ok = 1;
			for ( i = 0; i < want; i += 3u ){
				const size_t frame = a + (i / (NCHAN * 3u));
				const unsigned int chan = (unsigned int)
					((i / 3u) % NCHAN);
				const uint32_t u = (uint32_t)
					expected_i24(frame, chan);
				if ( got[i] != (unsigned char) (u & 0xFFu)
				  || got[i + 1u]
				     != (unsigned char) ((u >> 8u) & 0xFFu)
				  || got[i + 2u]
				     != (unsigned char) ((u >> 16u) & 0xFFu) ){
					ok = 0;
				}
			}
			TASK_CHECK(ok);
		}
		free(got);
	}
	got = read_file(out_m, &len);
	TASK_CHECK(got != NULL);
	if ( got != NULL ){
		TASK_CHECK(len == want);
		free(got);
	}
	return 0;
}

/* ======================================================================== */
/* errors: illegal ranges and a corrupt seektable                           */

static int expect_failure(const char *cmd)
{
	int rc = system(cmd);
	return rc != 0;
}

static int error_bad_ranges(void)
{
	char cmd[1024];
	const char *bad[] = {
		"5:3", "-1:5", "1:2:3", "1:2x", "abc", "1:", ":5",
		"0:40001", "40001:40001", "99999999999999999999:1"
	};
	size_t i;

	if ( ensure_material() != 0 ){ return 1; }
	remove(WORKDIR "/bad.raw");
	for ( i = 0; i < sizeof bad / sizeof bad[0]; ++i ){
		(void) snprintf(cmd, sizeof cmd,
			CLI " decode -S -q -f raw --samples=%s "
			WORKDIR "/material.tta -o " WORKDIR "/bad.raw "
			"2>/dev/null", bad[i]
		);
		TASK_CHECK(expect_failure(cmd));
	}
	/* a rejected range must not have produced an output file */
	TASK_CHECK(! file_exists(WORKDIR "/bad.raw"));
	return 0;
}

static int error_corrupt_seektable(void)
{
	unsigned char *buf;
	size_t len = 0;
	char cmd[1024];
	size_t off;

	if ( ensure_material() != 0 ){ return 1; }
	buf = read_file(WORKDIR "/material.tta", &len);
	if ( buf == NULL ){ return 1; }
	/* corrupt the seektable CRC (stored just after the entries) */
	off = 22u + 4u * ((TOTAL_FRAMES + FRAMELEN - 1u) / FRAMELEN);
	if ( off + 4u > len ){ free(buf); return 1; }
	buf[off] ^= 0xFFu;
	{
		FILE *f = fopen(WORKDIR "/bad_table.tta", "wb");
		if ( f == NULL ){ free(buf); return 1; }
		if ( fwrite(buf, 1u, len, f) != len ){ fclose(f); free(buf); return 1; }
		fclose(f);
	}
	free(buf);

	remove(WORKDIR "/badtab.raw");
	(void) snprintf(cmd, sizeof cmd,
		CLI " decode -S -q -f raw --samples=100:200 "
		WORKDIR "/bad_table.tta -o " WORKDIR "/badtab.raw 2>/dev/null"
	);
	TASK_CHECK(expect_failure(cmd));
	TASK_CHECK(! file_exists(WORKDIR "/badtab.raw"));
	return 0;
}

/* ======================================================================== */
/* damage: skipped payload vs. fully-checked intersecting frames            */

static int damage_outside_vs_inside(void)
{
	unsigned char *buf, *buf2;
	size_t len = 0;
	size_t tab_off, data_off, nframes, off_inside, off_outside;
	FILE *f;
	char cmd[1024];
	size_t a, b;

	if ( ensure_material() != 0 ){ return 1; }
	buf = read_file(WORKDIR "/material.tta", &len);
	if ( buf == NULL ){ return 1; }
	nframes = (TOTAL_FRAMES + FRAMELEN - 1u) / FRAMELEN;
	tab_off = 22u;
	data_off = tab_off + 4u * nframes + 4u;
	/* pick a range fully inside frame 0 and damage frame 0's payload */
	off_outside = data_off + 64u;
	/* and damage a byte in frame 1's payload */
	{
		const uint32_t frame0_size = le_u32(&buf[tab_off]);
		off_inside = data_off + frame0_size + 64u;
	}

	/* damage frame 0 (outside the frame-1 range): must be skipped */
	buf2 = malloc(len);
	if ( buf2 == NULL ){ free(buf); return 1; }
	memcpy(buf2, buf, len);
	buf2[off_outside] ^= 0xFFu;
	f = fopen(WORKDIR "/bad_outside.tta", "wb");
	if ( f == NULL ){ free(buf); free(buf2); return 1; }
	if ( fwrite(buf2, 1u, len, f) != len ){ fclose(f); free(buf); free(buf2); return 1; }
	fclose(f);

	a = FRAMELEN + 10u;
	b = FRAMELEN + 200u;
	/* corruption outside the selected frame -> no warning, exit 0 */
	(void) snprintf(cmd, sizeof cmd,
		CLI " decode -S -q -f raw --samples=%zu:%zu "
		WORKDIR "/bad_outside.tta -o " WORKDIR "/dmg.raw",
		a, b
	);
	TASK_CHECK(system(cmd) == 0);
	TASK_CHECK(raw_matches_range(WORKDIR "/dmg.raw", a, b));

	/* damage frame 1 (inside the frame-1 range): must be reported */
	memcpy(buf2, buf, len);
	buf2[off_inside] ^= 0xFFu;
	f = fopen(WORKDIR "/bad_inside.tta", "wb");
	if ( f == NULL ){ free(buf); free(buf2); return 1; }
	if ( fwrite(buf2, 1u, len, f) != len ){ fclose(f); free(buf); free(buf2); return 1; }
	fclose(f);

	/* a warning makes the exit status non-zero; the frame was still
	 * fully decoded and checked */
	(void) snprintf(cmd, sizeof cmd,
		CLI " decode -S -q -f raw --samples=%zu:%zu "
		WORKDIR "/bad_inside.tta -o " WORKDIR "/dmg2.raw "
		"2>" WORKDIR "/dmg2.log",
		a, b
	);
	TASK_CHECK(system(cmd) != 0);
	{
		size_t llen = 0;
		unsigned char *log = read_file(WORKDIR "/dmg2.log", &llen);
		TASK_CHECK(log != NULL);
		if ( log != NULL ){
			/* `dmg2.log' is text; the frame must be reported as
			 * corrupt even though only part of it is written */
			TASK_CHECK(buffer_contains(
				log, llen, "bad CRC", 7u
			));
			free(log);
		}
	}
	free(buf);
	free(buf2);
	return 0;
}

/* ======================================================================== */

const struct task_case task_cases[] = {
	{ "slices",  "same_frame",        slice_same_frame       },
	{ "slices",  "cross_frame",       slice_cross_frame      },
	{ "slices",  "frame_boundary",    slice_frame_boundary   },
	{ "slices",  "last_short_frame",  slice_last_short_frame },
	{ "slices",  "empty",             slice_empty            },

	{ "queues",  "many_rotations",    queue_many_rotations   },
	{ "queues",  "full_file",         queue_full_file        },
	{ "queues",  "first_sample",      queue_first_sample     },
	{ "queues",  "last_sample",       queue_last_sample      },

	{ "formats", "wav",               format_wav             },
	{ "formats", "w64",               format_w64             },
	{ "formats", "raw_stereo",        format_raw_stereo_conversion },
	{ "formats", "u8_mono",           format_u8_mono         },
	{ "formats", "i24_stereo",        format_i24_stereo      },

	{ "formats", "error_bad_ranges",  error_bad_ranges       },
	{ "formats", "error_corrupt_seektable", error_corrupt_seektable },
	{ "formats", "damage_inside_outside",   damage_outside_vs_inside },
};
const size_t task_case_count =
	sizeof task_cases / sizeof task_cases[0];

/* EOF //////////////////////////////////////////////////////////////////// */
