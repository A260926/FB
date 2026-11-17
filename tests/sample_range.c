/* ///////////////////////////////////////////////////////////////////////////
//                                                                          //
// tests/sample_range.c                                                     //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
//  Regressions for 'ttaR decode --samples=START:END'.                      //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
//  Fixtures are generated with the real libttaR encoder (in-process, via   //
//  libttaR_tta_encode), so every TTA file here has a genuine TTA1 header,  //
//  seektable, frame footer CRCs, and per-channel asymmetric PCM.           //
//                                                                          //
/////////////////////////////////////////////////////////////////////////// */

#include "task_support/check.h"

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/libttaR.h"

/* //////////////////////////////////////////////////////////////////////// */

#define PROG	".task-build/ttaR"
#define PATH_MAX_TEST	600

/* //////////////////////////////////////////////////////////////////////// */

struct fixture {
	char	tta[PATH_MAX_TEST];
	char	pcm[PATH_MAX_TEST];
	size_t	nsamples;	/* per channel */
	unsigned int	nchan;
	unsigned int	bits;
	unsigned int	rate;
};

static struct fixture fx_small;	/* i16le stereo, short last frame */
static struct fixture fx_big;	/* i16le stereo, many frames     */
static struct fixture fx_mono8;	/* u8 mono                       */
static struct fixture fx_s24;	/* i24le stereo                  */
static struct fixture fx_quad;	/* i16le 4-channel               */
static struct fixture fx_alt;	/* i16le stereo, other framelen  */

static char scratch[PATH_MAX_TEST];
static int  fixtures_ready;

/* //////////////////////////////////////////////////////////////////////// */

/* little-endian scalar writers */
static void put_u16le(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
}

static void put_u32le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
	p[2] = (uint8_t) (v >> 16);
	p[3] = (uint8_t) (v >> 24);
}

static uint32_t get_u32le(const uint8_t *p)
{
	return (uint32_t) p[0]
		| ((uint32_t) p[1] << 8)
		| ((uint32_t) p[2] << 16)
		| ((uint32_t) p[3] << 24);
}

/* deterministic, and intentionally different per channel */
static int32_t sample_value(size_t i, unsigned int chan, uint32_t seed)
{
	uint32_t x = seed + (uint32_t) i * 2654435761u
		+ (uint32_t) chan * 40503u;
	int32_t v;

	x ^= x >> 13;
	x *= 2246822519u;
	x ^= x >> 15;
	v = (int32_t) (x & 0x7FFFFFu);	/* 0 .. 2^23-1 */
	if (chan & 1u) {
		v = -v;		/* mirror odd channels */
	}
	if (chan >= 2u) {
		v = (v / 3) + (int32_t) chan * 1000;
	}
	return v;
}

static void write_pcm_sample(uint8_t *p, int32_t v, unsigned int bits)
{
	switch (bits) {
	case 8:
		p[0] = (uint8_t) ((v & 0xFF) ^ 0x80u);
		break;
	case 16:
		put_u16le(p, (uint16_t) (v & 0xFFFF));
		break;
	case 24:
		p[0] = (uint8_t) v;
		p[1] = (uint8_t) (v >> 8);
		p[2] = (uint8_t) (v >> 16);
		break;
	}
}

static size_t samplebytes_of(unsigned int bits)
{
	return (bits + 7u) / 8u;
}

/* ------------------------------------------------------------------------ */

#define TTABUF_GUESS(ni32, samplebytes, nchan)	((size_t) ( \
	(ni32) * 4u + 65536u \
	+ libttaR_ttabuf_safety_margin((samplebytes), (nchan)) \
))

/* encode one frame; grows 'buf' as needed, as the CLI's encoder does */
static int encode_frame(
	struct LibTTAr_CodecState_Priv *priv,
	uint8_t **buf, size_t *cap,
	const int32_t *i32, size_t ni32,
	enum LibTTAr_SampleBytes samplebytes, unsigned int nchan,
	size_t *nbytes_out, uint32_t *crc_out
)
{
	struct LibTTAr_CodecState_User user = LIBTTAr_CODECSTATE_USER_INIT;
	struct LibTTAr_EncMisc misc;
	enum LibTTAr_EncRetVal status;

	misc.ni32_perframe = ni32;
	misc.samplebytes   = samplebytes;
	misc.nchan         = nchan;

	do {
		if (*cap < TTABUF_GUESS(ni32, samplebytes, nchan)) {
			*cap = TTABUF_GUESS(ni32, samplebytes, nchan);
			*buf = realloc(*buf, *cap);
			if (*buf == NULL) {
				return -1;
			}
		}
		misc.dest_len    = *cap - user.nbytes_tta_total;
		misc.src_len     = ni32;
		misc.ni32_target = ni32 - user.ni32_total;
		status = libttaR_tta_encode(
			&(*buf)[user.nbytes_tta_total],
			&i32[user.ni32_total], priv, &user, &misc
		);
		if (status == LIBTTAr_ERV_OK_AGAIN) {
			*cap += TTABUF_GUESS(ni32, samplebytes, nchan);
			*buf = realloc(*buf, *cap);
			if (*buf == NULL) {
				return -1;
			}
		}
	} while (status == LIBTTAr_ERV_OK_AGAIN);

	if (status != LIBTTAr_ERV_OK_DONE) {
		return -1;
	}
	*nbytes_out = user.nbytes_tta_total;
	*crc_out    = user.crc;
	return 0;
}

/* encode a raw PCM file into a TTA1 file (header + seektable + frames) */
static int encode_tta_file(
	const char *tta_path, const uint8_t *pcm, size_t nsamples_perchan,
	unsigned int nchan, unsigned int bits, unsigned int samplerate
)
{
	const size_t samplebytes = samplebytes_of(bits);
	const size_t framelen    = libttaR_nsamples_perframe_tta1(samplerate);
	const size_t nframes     = nsamples_perchan / framelen
		+ (nsamples_perchan % framelen != 0);
	const size_t buflen      = framelen * nchan;
	/* * */
	struct LibTTAr_CodecState_Priv *priv;
	void *priv_raw;
	int32_t *i32buf = NULL;
	uint8_t *frames = NULL, *seektable = NULL, *hdr = NULL;
	size_t frames_len = 0;
	size_t frame_offsets_size = nframes * sizeof(uint32_t);
	FILE *f;
	size_t i;

	if ((framelen == 0) || (nframes == 0)) {
		return -1;
	}
	priv_raw = malloc(libttaR_codecstate_priv_size(nchan)
		+ LIBTTAr_CODECSTATE_PRIV_ALIGN);
	i32buf = calloc(buflen, sizeof *i32buf);
	seektable = calloc(nframes, sizeof(uint32_t));
	hdr = calloc(22u, 1u);
	if ((priv_raw == NULL) || (i32buf == NULL)
	    || (seektable == NULL) || (hdr == NULL)) {
		free(priv_raw);
		goto fail;
	}
	/* align priv the same way the CLI does */
	{
		uintptr_t base = (uintptr_t) priv_raw;
		uintptr_t align = base % LIBTTAr_CODECSTATE_PRIV_ALIGN;
		priv = (struct LibTTAr_CodecState_Priv *)(void *)
			(base + (LIBTTAr_CODECSTATE_PRIV_ALIGN - align)
			 % LIBTTAr_CODECSTATE_PRIV_ALIGN);
	}

	for (i = 0; i < nframes; ++i) {
		const size_t first = i * framelen;
		const size_t count = (first + framelen > nsamples_perchan
			? nsamples_perchan - first : framelen);
		const size_t ni32  = count * nchan;
		size_t nbytes;
		uint32_t crc;
		uint8_t *framebuf = NULL;
		size_t framecap = 0;

		(void) libttaR_pcm_read(
			i32buf, &pcm[first * nchan * samplebytes], ni32,
			(enum LibTTAr_SampleBytes) samplebytes
		);
		if (encode_frame(
			priv, &framebuf, &framecap, i32buf, ni32,
			(enum LibTTAr_SampleBytes) samplebytes, nchan,
			&nbytes, &crc) != 0) {
			free(framebuf);
			goto fail;
		}
		frames = realloc(frames, frames_len + nbytes + 4u);
		if (frames == NULL) {
			free(framebuf);
			goto fail;
		}
		memcpy(&frames[frames_len], framebuf, nbytes);
		frames_len += nbytes;
		put_u32le(&frames[frames_len], crc);
		frames_len += 4u;
		/* seektable entries hold this frame's size, CRC included */
		put_u32le(
			&seektable[i * sizeof(uint32_t)],
			(uint32_t) (nbytes + 4u)
		);
		free(framebuf);
	}

	/* TTA1 header */
	memcpy(hdr, "TTA1", 4u);
	put_u16le(&hdr[4], 1u);			/* WAVE_FMT_PCM */
	put_u16le(&hdr[6], (uint16_t) nchan);
	put_u16le(&hdr[8], (uint16_t) bits);
	put_u32le(&hdr[10], samplerate);
	put_u32le(&hdr[14], (uint32_t) nsamples_perchan);
	put_u32le(&hdr[18], libttaR_crc32(hdr, 18u));

	f = fopen(tta_path, "wb");
	if (f == NULL) {
		goto fail;
	}
	if (fwrite(hdr, 22u, 1u, f) != 1u
	    || fwrite(seektable, frame_offsets_size, 1u, f) != 1u) {
		fclose(f);
		goto fail;
	}
	{
		uint32_t crc = libttaR_crc32(seektable, frame_offsets_size);
		uint8_t c[4];
		put_u32le(c, crc);
		if (fwrite(c, 4u, 1u, f) != 1u
		    || fwrite(frames, frames_len, 1u, f) != 1u) {
			fclose(f);
			goto fail;
		}
	}
	if (fclose(f) != 0) {
		goto fail;
	}
	free(priv_raw);
	free(i32buf);
	free(frames);
	free(seektable);
	free(hdr);
	return 0;

fail:
	free(priv_raw);
	free(i32buf);
	free(frames);
	free(seektable);
	free(hdr);
	return -1;
}

/* ------------------------------------------------------------------------ */

static int make_fixture(
	struct fixture *fx, const char *name, size_t nsamples,
	unsigned int nchan, unsigned int bits, unsigned int rate,
	uint32_t seed
)
{
	const size_t samplebytes = samplebytes_of(bits);
	const size_t pcm_size = nsamples * nchan * samplebytes;
	uint8_t *pcm;
	size_t i;
	FILE *f;

	fx->nsamples = nsamples;
	fx->nchan    = nchan;
	fx->bits     = bits;
	fx->rate     = rate;
	(void) snprintf(
		fx->pcm, sizeof fx->pcm, "%s/%s.pcm", scratch, name
	);
	(void) snprintf(
		fx->tta, sizeof fx->tta, "%s/%s.tta", scratch, name
	);

	pcm = malloc(pcm_size);
	if (pcm == NULL) {
		return -1;
	}
	for (i = 0; i < nsamples; ++i) {
		unsigned int c;
		for (c = 0; c < nchan; ++c) {
			const size_t off = (
				(i * nchan + c) * samplebytes
			);
			write_pcm_sample(
				&pcm[off], sample_value(i, c, seed), bits
			);
		}
	}
	f = fopen(fx->pcm, "wb");
	if (f == NULL) {
		free(pcm);
		return -1;
	}
	if (fwrite(pcm, pcm_size, 1u, f) != 1u || fclose(f) != 0) {
		free(pcm);
		return -1;
	}
	if (encode_tta_file(
		fx->tta, pcm, nsamples, nchan, bits, rate) != 0) {
		free(pcm);
		return -1;
	}
	free(pcm);
	return 0;
}

static int ensure_fixtures(void)
{
	const char *tmp = getenv("TMPDIR");
	char templ[PATH_MAX_TEST];

	if (fixtures_ready) {
		return 0;
	}
	if ((tmp == NULL) || (tmp[0] == '\0')) {
		tmp = "/tmp";
	}
	(void) snprintf(templ, sizeof templ, "%s/ttar-range-XXXXXX", tmp);
	if (mkdtemp(templ) == NULL) {
		return -1;
	}
	(void) snprintf(scratch, sizeof scratch, "%s", templ);

	/* 3 full 8000 Hz frames (8359) + a short last frame */
	if (make_fixture(
		&fx_small, "small", 8359u * 3u + 1234u, 2u, 16u, 8000u,
		0x12345678u) != 0) {
		return -1;
	}
	/* 11 full frames + a short last frame: several queue circles */
	if (make_fixture(
		&fx_big, "big", 8359u * 11u + 400u, 2u, 16u, 8000u,
		0xA5A5A5A5u) != 0) {
		return -1;
	}
	if (make_fixture(
		&fx_mono8, "mono8", 8359u * 2u + 100u, 1u, 8u, 8000u,
		0x0BADF00Du) != 0) {
		return -1;
	}
	if (make_fixture(
		&fx_s24, "s24", 8359u * 2u + 17u, 2u, 24u, 8000u,
		0xC0FFEE11u) != 0) {
		return -1;
	}
	if (make_fixture(
		&fx_quad, "quad", 8359u + 500u, 4u, 16u, 8000u,
		0x11223344u) != 0) {
		return -1;
	}
	/* different samplerate => different frame length, for per-file
		ranges in one command
	*/
	if (make_fixture(
		&fx_alt, "alt", 30000u, 2u, 16u, 11025u,
		0x55667788u) != 0) {
		return -1;
	}
	fixtures_ready = 1;
	return 0;
}

/* //////////////////////////////////////////////////////////////////////// */

static int run_sh(const char *cmd)
{
	int rc = system(cmd);

	if (rc == -1) {
		return -1;
	}
	if (WIFEXITED(rc)) {
		return WEXITSTATUS(rc);
	}
	return -1;
}

static int file_read(const char *path, uint8_t **buf_out, size_t *size_out)
{
	FILE *f = fopen(path, "rb");
	long size;
	uint8_t *buf;

	if (f == NULL) {
		return -1;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return -1;
	}
	size = ftell(f);
	if ((size < 0) || (fseek(f, 0, SEEK_SET) != 0)) {
		fclose(f);
		return -1;
	}
	buf = malloc((size_t) size + 1u);
	if (buf == NULL) {
		fclose(f);
		return -1;
	}
	if (fread(buf, 1u, (size_t) size, f) != (size_t) size) {
		free(buf);
		fclose(f);
		return -1;
	}
	fclose(f);
	*buf_out  = buf;
	*size_out = (size_t) size;
	return 0;
}

static int buf_contains(
	const uint8_t *hay, size_t haylen, const char *needle
)
{
	const size_t nlen = strlen(needle);
	size_t i;

	if (nlen == 0) {
		return 1;
	}
	if (haylen < nlen) {
		return 0;
	}
	for (i = 0; i + nlen <= haylen; ++i) {
		if (memcmp(&hay[i], needle, nlen) == 0) {
			return 1;
		}
	}
	return 0;
}

static int file_exists(const char *path)
{
	struct stat st;

	return (stat(path, &st) == 0);
}

static int file_remove(const char *path)
{
	return remove(path);
}

/* compare raw PCM [start,end) of a fixture to a file */
static int check_raw_slice(
	const struct fixture *fx, const char *outfile,
	size_t start, size_t end
)
{
	const size_t samplebytes = samplebytes_of(fx->bits);
	const size_t offset = start * fx->nchan * samplebytes;
	const size_t len    = (end - start) * fx->nchan * samplebytes;
	uint8_t *want = NULL, *got = NULL;
	size_t want_size = 0, got_size = 0;
	int rc;

	rc = file_read(fx->pcm, &want, &want_size);
	TASK_CHECK(rc == 0);
	TASK_CHECK(offset + len <= want_size);
	rc = file_read(outfile, &got, &got_size);
	TASK_CHECK(rc == 0);
	TASK_CHECK(got_size == len);
	TASK_CHECK(memcmp(got, &want[offset], len) == 0);

	free(want);
	free(got);
	return 0;
}

/* compare the data chunk of a wav/w64 file to raw PCM [start,end) */
static int check_container_slice(
	const struct fixture *fx, const char *outfile,
	size_t start, size_t end, int is_w64
)
{
	const size_t samplebytes = samplebytes_of(fx->bits);
	const size_t offset = start * fx->nchan * samplebytes;
	const size_t len    = (end - start) * fx->nchan * samplebytes;
	uint8_t *want = NULL, *got = NULL;
	size_t want_size = 0, got_size = 0;
	size_t data_at = 0, data_size = 0;
	int found = 0;
	size_t i;
	int rc;

	rc = file_read(fx->pcm, &want, &want_size);
	TASK_CHECK(rc == 0);
	rc = file_read(outfile, &got, &got_size);
	TASK_CHECK(rc == 0);

	if (is_w64) {
		for (i = 0; i + 24u <= got_size; ++i) {
			if (memcmp(&got[i], "data", 4u) == 0) {
				data_at   = i + 24u;
				data_size = (size_t) (
					(uint64_t) got[i + 16]
					| ((uint64_t) got[i + 17] << 8)
					| ((uint64_t) got[i + 18] << 16)
					| ((uint64_t) got[i + 19] << 24)
					| ((uint64_t) got[i + 20] << 32)
					| ((uint64_t) got[i + 21] << 40)
					| ((uint64_t) got[i + 22] << 48)
					| ((uint64_t) got[i + 23] << 56)
				);
				found = 1;
				break;
			}
		}
	}
	else {
		for (i = 0; i + 8u <= got_size; ++i) {
			if (memcmp(&got[i], "data", 4u) == 0) {
				data_at   = i + 8u;
				data_size = (size_t) get_u32le(&got[i + 4]);
				found = 1;
				break;
			}
		}
	}
	TASK_CHECK(found == 1);
	/* Wave64 chunk sizes include their 24-byte header */
	if (is_w64) {
		TASK_CHECK(data_size == len + 24u);
	}
	else {
		TASK_CHECK(data_size == len);
	}
	TASK_CHECK(got_size == data_at + len);
	if (found && (data_at + len <= got_size)) {
		TASK_CHECK(offset + len <= want_size);
		TASK_CHECK(memcmp(
			&got[data_at], &want[offset], len
		) == 0);
	}

	free(want);
	free(got);
	return 0;
}

/* decode a range through the chosen threading path */
static int decode(
	const char *thread_opt, const char *fmt, const char *samples,
	const char *infile, const char *outfile
)
{
	char cmd[4096];

	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode %s -q -f %s --samples=%s '%s' -o '%s' "
		">/dev/null 2>&1",
		PROG, thread_opt, fmt, samples, infile, outfile
	);
	return run_sh(cmd);
}

static int decode_separate(
	const char *thread_opt, const char *fmt, const char *samples,
	const char *infile, const char *outfile
)
{
	char cmd[4096];

	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode %s -q -f %s --samples %s '%s' -o '%s' "
		">/dev/null 2>&1",
		PROG, thread_opt, fmt, samples, infile, outfile
	);
	return run_sh(cmd);
}

#define THREAD_ST	"-S"
#define THREAD_MT	"-M -t 2"

static void scratch_path(char *dst, size_t size, const char *name)
{
	(void) snprintf(dst, size, "%s/%s", scratch, name);
}

/* both decode paths over raw output */
static int check_both_paths_raw(
	const struct fixture *fx, const char *tag, const char *samples,
	size_t start, size_t end
)
{
	char out[PATH_MAX_TEST];
	char name[128];

	(void) snprintf(name, sizeof name, "%s_st.raw", tag);
	scratch_path(out, sizeof out, name);
	if (decode(THREAD_ST, "raw", samples, fx->tta, out) != 0) {
		TASK_CHECK(0);
	}
	if (check_raw_slice(fx, out, start, end) != 0) {
		return 1;
	}

	(void) snprintf(name, sizeof name, "%s_mt.raw", tag);
	scratch_path(out, sizeof out, name);
	if (decode(THREAD_MT, "raw", samples, fx->tta, out) != 0) {
		TASK_CHECK(0);
	}
	if (check_raw_slice(fx, out, start, end) != 0) {
		return 1;
	}
	return 0;
}

/* //////////////////////////////////////////////////////////////////////// */

/* corruption helpers: operate on a real encoded TTA file */

struct tta_layout {
	uint8_t	*buf;
	size_t	size;
	size_t	nsamples;
	unsigned int	nchan;
	unsigned int	bits;
	unsigned int	rate;
	size_t	framelen;
	size_t	nframes;
	size_t	table_off;
	size_t	payload_off;
	uint32_t	*entries;
};

static int tta_layout_load(const char *path, struct tta_layout *l)
{
	size_t i;
	uint8_t *buf = NULL;

	if (file_read(path, &buf, &l->size) != 0) {
		return -1;
	}
	if ((l->size < 26u) || (memcmp(buf, "TTA1", 4u) != 0)) {
		free(buf);
		return -1;
	}
	l->buf      = buf;
	l->nchan    = (unsigned int) (buf[6] | (buf[7] << 8));
	l->bits     = (unsigned int) (buf[8] | (buf[9] << 8));
	l->rate     = (unsigned int) get_u32le(&buf[10]);
	l->nsamples = (size_t) get_u32le(&buf[14]);
	l->framelen = libttaR_nsamples_perframe_tta1(l->rate);
	l->nframes  = l->nsamples / l->framelen
		+ (l->nsamples % l->framelen != 0);
	l->table_off  = 22u;
	l->payload_off = l->table_off + l->nframes * 4u + 4u;
	if (l->payload_off > l->size) {
		free(buf);
		return -1;
	}
	l->entries = calloc(l->nframes, sizeof *l->entries);
	if (l->entries == NULL) {
		free(buf);
		return -1;
	}
	for (i = 0; i < l->nframes; ++i) {
		l->entries[i] = get_u32le(&buf[l->table_off + i * 4u]);
	}
	return 0;
}

static void tta_layout_free(struct tta_layout *l)
{
	free(l->buf);
	free(l->entries);
	l->buf = NULL;
	l->entries = NULL;
}

static size_t tta_frame_offset(const struct tta_layout *l, size_t frame)
{
	size_t off = l->payload_off;
	size_t i;

	for (i = 0; i < frame; ++i) {
		off += l->entries[i];
	}
	return off;
}

static int tta_write(const struct tta_layout *l, const char *dst)
{
	FILE *f = fopen(dst, "wb");
	int rc;

	if (f == NULL) {
		return -1;
	}
	rc = (fwrite(l->buf, l->size, 1u, f) == 1u ? 0 : -1);
	if (fclose(f) != 0) {
		rc = -1;
	}
	return rc;
}

/* flip a byte in a frame's CRC footer */
static int corrupt_frame_crc(
	const char *src, const char *dst, size_t frame
)
{
	struct tta_layout l;
	size_t off;
	int rc;

	if (tta_layout_load(src, &l) != 0) {
		return -1;
	}
	if (frame >= l.nframes) {
		tta_layout_free(&l);
		return -1;
	}
	off = tta_frame_offset(&l, frame) + l.entries[frame] - 1u;
	l.buf[off] ^= 0xFFu;
	rc = tta_write(&l, dst);
	tta_layout_free(&l);
	return rc;
}

/* flip a byte in a frame's compressed payload (not its footer) */
static int corrupt_frame_payload(
	const char *src, const char *dst, size_t frame, size_t nth
)
{
	struct tta_layout l;
	size_t base, payload_len, off;
	int rc;

	if (tta_layout_load(src, &l) != 0) {
		return -1;
	}
	if (frame >= l.nframes) {
		tta_layout_free(&l);
		return -1;
	}
	base = tta_frame_offset(&l, frame);
	payload_len = l.entries[frame] - 4u;
	off = base + (nth % payload_len);
	l.buf[off] ^= 0x5Au;
	rc = tta_write(&l, dst);
	tta_layout_free(&l);
	return rc;
}

/* flip a byte in the seektable CRC */
static int corrupt_seektable_crc(const char *src, const char *dst)
{
	struct tta_layout l;
	size_t crc_off;
	int rc;

	if (tta_layout_load(src, &l) != 0) {
		return -1;
	}
	crc_off = l.table_off + l.nframes * 4u;
	l.buf[crc_off] ^= 0xFFu;
	rc = tta_write(&l, dst);
	tta_layout_free(&l);
	return rc;
}

/* rewrite one seektable length entry and fix the table CRC */
static int corrupt_seektable_len(
	const char *src, const char *dst, size_t frame, uint32_t value
)
{
	struct tta_layout l;
	size_t table_bytes, crc_off;
	uint32_t crc;
	int rc;

	if (tta_layout_load(src, &l) != 0) {
		return -1;
	}
	if (frame >= l.nframes) {
		tta_layout_free(&l);
		return -1;
	}
	put_u32le(&l.buf[l.table_off + frame * 4u], value);
	table_bytes = l.nframes * 4u;
	crc = libttaR_crc32(&l.buf[l.table_off], table_bytes);
	crc_off = l.table_off + table_bytes;
	put_u32le(&l.buf[crc_off], crc);
	rc = tta_write(&l, dst);
	tta_layout_free(&l);
	return rc;
}

/* //////////////////////////////////////////////////////////////////////// */

static int case_same_frame(void)
{
	char out[PATH_MAX_TEST];

	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	TASK_CHECK(check_both_paths_raw(
		&fx_small, "same", "100:200", 100, 200) == 0);

	/* separate-argument option form is accepted too */
	scratch_path(out, sizeof out, "sep.raw");
	TASK_CHECK(decode_separate(
		THREAD_ST, "raw", "100:200", fx_small.tta, out) == 0);
	TASK_CHECK(check_raw_slice(&fx_small, out, 100, 200) == 0);
	TASK_CHECK(decode_separate(
		THREAD_MT, "raw", "100:200", fx_small.tta, out) == 0);
	TASK_CHECK(check_raw_slice(&fx_small, out, 100, 200) == 0);
	return 0;
}

static int case_cross_frame(void)
{
	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	/* 8359 is the frame boundary at 8000 Hz */
	TASK_CHECK(check_both_paths_raw(
		&fx_small, "cross", "8300:8500", 8300, 8500) == 0);
	TASK_CHECK(check_both_paths_raw(
		&fx_small, "cross2", "7000:18000", 7000, 18000) == 0);
	return 0;
}

static int case_frame_boundary(void)
{
	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	TASK_CHECK(check_both_paths_raw(
		&fx_small, "bound", "8359:16718", 8359, 16718) == 0);
	TASK_CHECK(check_both_paths_raw(
		&fx_small, "bound0", "0:8359", 0, 8359) == 0);
	TASK_CHECK(check_both_paths_raw(
		&fx_small, "bound1", "8359:8359", 8359, 8359) == 0);
	return 0;
}

static int case_last_short_frame(void)
{
	const size_t total = fx_small.nsamples;

	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	/* the last frame only holds total - 3*8359 samples */
	TASK_CHECK(check_both_paths_raw(
		&fx_small, "tail", "25500:26311", 25500, total) == 0);
	TASK_CHECK(check_both_paths_raw(
		&fx_small, "tailcross", "24000:26311", 24000, total) == 0);
	TASK_CHECK(check_both_paths_raw(
		&fx_small, "tailstart", "25077:26311", 25077, total) == 0);
	return 0;
}

static int case_empty_interval(void)
{
	char out[PATH_MAX_TEST];
	char samples[64];
	char cmd[4096];
	uint8_t *buf = NULL;
	size_t size = 0;
	size_t positions[4];
	size_t i;
	int rc;

	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	positions[0] = 0;
	positions[1] = 100;
	positions[2] = 8359;
	positions[3] = fx_small.nsamples;
	for (i = 0; i < 4u; ++i) {
		(void) snprintf(
			samples, sizeof samples, "%zu:%zu",
			positions[i], positions[i]
		);
		scratch_path(out, sizeof out, "empty.raw");
		(void) file_remove(out);
		TASK_CHECK(decode(
			THREAD_ST, "raw", samples, fx_small.tta, out) == 0);
		rc = file_read(out, &buf, &size);
		TASK_CHECK(rc == 0);
		TASK_CHECK(size == 0);
		free(buf);
		buf = NULL;

		(void) file_remove(out);
		TASK_CHECK(decode(
			THREAD_MT, "raw", samples, fx_small.tta, out) == 0);
		rc = file_read(out, &buf, &size);
		TASK_CHECK(rc == 0);
		TASK_CHECK(size == 0);
		free(buf);
		buf = NULL;
	}

	/* empty interval still gets a valid container header, length 0 */
	scratch_path(out, sizeof out, "empty.wav");
	TASK_CHECK(decode(
		THREAD_ST, "wav", "5000:5000", fx_small.tta, out) == 0);
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -S -q -f wav --samples=5000:5000 '%s' -o '%s' "
		">/dev/null 2>&1",
		PROG, fx_small.tta, out
	);
	TASK_CHECK(run_sh(cmd) == 0);
	{
		uint8_t *w = NULL;
		size_t wsize = 0, k, at = 0;
		int found = 0;
		TASK_CHECK(file_read(out, &w, &wsize) == 0);
		for (k = 0; k + 8u <= wsize; ++k) {
			if (memcmp(&w[k], "data", 4u) == 0) {
				at = k;
				found = 1;
				break;
			}
		}
		TASK_CHECK(found);
		TASK_CHECK(get_u32le(&w[at + 4]) == 0u);
		TASK_CHECK(wsize == at + 8u);
		free(w);
	}
	return 0;
}

static int case_multiple_inputs(void)
{
	char outdir[PATH_MAX_TEST];
	char samples[64];
	char cmd[4096];
	char out1[PATH_MAX_TEST], out2[PATH_MAX_TEST];
	size_t start = 9000, end = 9600;

	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	(void) snprintf(outdir, sizeof outdir, "%s/multi", scratch);
	(void) mkdir(outdir, 0777);

	/* the same [START,END) is interpreted per input; the fixtures
		have different samplerates, hence different frame lengths
	*/
	(void) snprintf(
		samples, sizeof samples, "%zu:%zu", start, end
	);
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -S -q -f raw --samples=%s '%s' '%s' -o '%s/' "
		">/dev/null 2>&1",
		PROG, samples, fx_small.tta, fx_alt.tta, outdir
	);
	TASK_CHECK(run_sh(cmd) == 0);

	(void) snprintf(out1, sizeof out1, "%s/small.raw", outdir);
	(void) snprintf(out2, sizeof out2, "%s/alt.raw", outdir);
	TASK_CHECK(check_raw_slice(&fx_small, out1, start, end) == 0);
	TASK_CHECK(check_raw_slice(&fx_alt, out2, start, end) == 0);
	return 0;
}

static int case_stats(void)
{
	char out[PATH_MAX_TEST];
	char errfile[PATH_MAX_TEST];
	char cmd[4096];
	uint8_t *err = NULL;
	size_t errsize = 0;
	size_t nbytes = (200u - 100u) * 2u * 2u;
	char needle[64];

	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	scratch_path(out, sizeof out, "stats.raw");
	scratch_path(errfile, sizeof errfile, "stats.err");
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -S -f raw --samples=100:200 '%s' -o '%s' "
		"2>'%s' >/dev/null",
		PROG, fx_small.tta, out, errfile
	);
	TASK_CHECK(run_sh(cmd) == 0);
	TASK_CHECK(file_read(errfile, &err, &errsize) == 0);
	(void) snprintf(
		needle, sizeof needle, "pcm\t: %zu B", nbytes
	);
	TASK_CHECK(buf_contains(err, errsize, needle));
	free(err);
	return 0;
}

static int case_corrupt_outside(void)
{
	char bad[PATH_MAX_TEST], out[PATH_MAX_TEST];
	char cmd[4096];

	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	scratch_path(bad, sizeof bad, "bad_outside.tta");
	scratch_path(out, sizeof out, "bad_outside.raw");
	TASK_CHECK(corrupt_frame_payload(
		fx_big.tta, bad, 0u, 17u) == 0);
	/* frame 0 is outside [20000,20200): it must not even be read */
	(void) file_remove(out);
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -S -q -f raw --samples=20000:20200 '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, bad, out
	);
	TASK_CHECK(run_sh(cmd) == 0);
	TASK_CHECK(check_raw_slice(&fx_big, out, 20000, 20200) == 0);

	/* same through the multi-threaded path */
	(void) file_remove(out);
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -M -t 2 -q -f raw --samples=20000:20200 '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, bad, out
	);
	TASK_CHECK(run_sh(cmd) == 0);
	TASK_CHECK(check_raw_slice(&fx_big, out, 20000, 20200) == 0);

	/* corrupt CRC footer outside the interval, too */
	scratch_path(bad, sizeof bad, "bad_outside_crc.tta");
	TASK_CHECK(corrupt_frame_crc(fx_big.tta, bad, 0u) == 0);
	(void) file_remove(out);
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -S -q -f raw --samples=20000:20200 '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, bad, out
	);
	TASK_CHECK(run_sh(cmd) == 0);
	TASK_CHECK(check_raw_slice(&fx_big, out, 20000, 20200) == 0);
	return 0;
}

static int case_corrupt_intersect(void)
{
	char bad[PATH_MAX_TEST], out[PATH_MAX_TEST];
	char cmd[4096];

	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	/* frame 1 spans samples [8359,16718). The interval keeps only
		its first samples, but the whole frame must still be checked.
	*/
	scratch_path(bad, sizeof bad, "bad_in.tta");
	scratch_path(out, sizeof out, "bad_in.raw");
	TASK_CHECK(corrupt_frame_payload(
		fx_big.tta, bad, 1u, 2000u) == 0);
	(void) file_remove(out);
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -S -q -f raw --samples=8359:8362 '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, bad, out
	);
	TASK_CHECK(run_sh(cmd) != 0);

	/* the same corrupt frame is flagged by the mt path */
	(void) file_remove(out);
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -M -t 2 -q -f raw --samples=8359:8362 '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, bad, out
	);
	TASK_CHECK(run_sh(cmd) != 0);

	/* corrupt the CRC footer of the intersecting frame */
	scratch_path(bad, sizeof bad, "bad_in_crc.tta");
	TASK_CHECK(corrupt_frame_crc(fx_big.tta, bad, 1u) == 0);
	(void) file_remove(out);
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -S -q -f raw --samples=16000:16010 '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, bad, out
	);
	TASK_CHECK(run_sh(cmd) != 0);
	return 0;
}

static int case_invalid_range(void)
{
	static const char *bad_args[] = {
		"20:10",		/* start after end          */
		"-1:10",		/* negative                 */
		"10x:20",		/* non-digit                */
		"10:20x",
		"10",			/* missing ':'              */
		"1:2:3",		/* extra ':'                */
		":10",			/* empty field              */
		"10:",
		"99999999999999999999999999:10",	/* overflow */
		"10:99999999999999999999999999"
	};
	char out[PATH_MAX_TEST];
	char cmd[4096];
	char past[64], past2[64];
	size_t i;

	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	scratch_path(out, sizeof out, "invalid.raw");
	/* end past the declared total */
	(void) snprintf(
		past, sizeof past, "%zu:%zu",
		fx_small.nsamples + 1u, fx_small.nsamples + 1u
	);
	(void) snprintf(
		past2, sizeof past2, "0:%zu", fx_small.nsamples + 1u
	);
	for (i = 0; i < sizeof bad_args / sizeof bad_args[0]; ++i) {
		(void) file_remove(out);
		(void) snprintf(
			cmd, sizeof cmd,
			"%s decode -S -q -f raw --samples=%s '%s' "
			"-o '%s' >/dev/null 2>&1",
			PROG, bad_args[i], fx_small.tta, out
		);
		TASK_CHECK(run_sh(cmd) != 0);
		TASK_CHECK(! file_exists(out));
	}
	for (i = 0; i < 2u; ++i) {
		const char *arg = (i == 0 ? past : past2);
		(void) file_remove(out);
		(void) snprintf(
			cmd, sizeof cmd,
			"%s decode -S -q -f raw --samples=%s '%s' "
			"-o '%s' >/dev/null 2>&1",
			PROG, arg, fx_small.tta, out
		);
		TASK_CHECK(run_sh(cmd) != 0);
		TASK_CHECK(! file_exists(out));
	}
	return 0;
}

static int case_bad_seektable(void)
{
	char bad[PATH_MAX_TEST], out[PATH_MAX_TEST];
	char cmd[4096];
	uint8_t *buf = NULL;
	size_t size = 0;

	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	scratch_path(out, sizeof out, "badtab.raw");

	/* corrupted seektable CRC: range mode must refuse before output */
	scratch_path(bad, sizeof bad, "badtab_crc.tta");
	TASK_CHECK(corrupt_seektable_crc(fx_big.tta, bad) == 0);
	(void) file_remove(out);
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -S -q -f raw --samples=20000:20100 '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, bad, out
	);
	TASK_CHECK(run_sh(cmd) != 0);
	TASK_CHECK(! file_exists(out));

	/* malformed entry before the selected frame (length <= CRC size) */
	scratch_path(bad, sizeof bad, "badtab_len.tta");
	TASK_CHECK(corrupt_seektable_len(fx_big.tta, bad, 0u, 4u) == 0);
	(void) file_remove(out);
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -S -q -f raw --samples=20000:20100 '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, bad, out
	);
	TASK_CHECK(run_sh(cmd) != 0);
	TASK_CHECK(! file_exists(out));

	/* malformed entry inside the selected interval */
	scratch_path(bad, sizeof bad, "badtab_len2.tta");
	TASK_CHECK(corrupt_seektable_len(fx_big.tta, bad, 2u, 3u) == 0);
	(void) file_remove(out);
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -M -t 2 -q -f raw --samples=17000:17100 '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, bad, out
	);
	TASK_CHECK(run_sh(cmd) != 0);
	TASK_CHECK(! file_exists(out));

	/* a huge (but representable) length must not wrap; the read is
		truncated, never silent audio
	*/
	scratch_path(bad, sizeof bad, "badtab_huge.tta");
	TASK_CHECK(corrupt_seektable_len(
		fx_big.tta, bad, 0u, 0xFFFFFF00u) == 0);
	(void) file_remove(out);
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -S -q -f raw --samples=17000:17100 '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, bad, out
	);
	TASK_CHECK(run_sh(cmd) != 0);
	/* no false audio was produced */
	if (file_read(out, &buf, &size) == 0) {
		TASK_CHECK(size == 0);
		free(buf);
	}

	/* the whole-file path keeps its old behavior: warning, not fatal */
	scratch_path(bad, sizeof bad, "badtab_crc2.tta");
	TASK_CHECK(corrupt_seektable_crc(fx_small.tta, bad) == 0);
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -S -q -f raw '%s' -o '%s' >/dev/null 2>&1",
		PROG, bad, out
	);
	TASK_CHECK(run_sh(cmd) != 0);	/* warnings -> nonzero status */
	return 0;
}

/* //////////////////////////////////////////////////////////////////////// */

static int case_mt_tiny_selection(void)
{
	char out[PATH_MAX_TEST];
	char ref[PATH_MAX_TEST];
	char cmd[4096];

	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	/* one frame is fewer than one queue circle (2*8 = 16) */
	scratch_path(out, sizeof out, "mt_tiny.raw");
	scratch_path(ref, sizeof ref, "mt_tiny_ref.raw");
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -M -t 8 -q -f raw --samples=20000:20010 '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, fx_big.tta, out
	);
	TASK_CHECK(run_sh(cmd) == 0);
	TASK_CHECK(check_raw_slice(&fx_big, out, 20000, 20010) == 0);
	(void) ref;
	return 0;
}

static int case_mt_queue_circles(void)
{
	char out_mt[PATH_MAX_TEST], out_st[PATH_MAX_TEST];
	char cmd[4096];
	size_t total = fx_big.nsamples;

	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	/* 12 frames with a queue of 4: several full circles */
	scratch_path(out_mt, sizeof out_mt, "mt_all.raw");
	scratch_path(out_st, sizeof out_st, "st_all.raw");
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -M -t 2 -q -f raw --samples=0:%zu '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, total, fx_big.tta, out_mt
	);
	TASK_CHECK(run_sh(cmd) == 0);
	TASK_CHECK(check_raw_slice(&fx_big, out_mt, 0, total) == 0);

	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -S -q -f raw --samples=0:%zu '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, total, fx_big.tta, out_st
	);
	TASK_CHECK(run_sh(cmd) == 0);
	{
		uint8_t *a = NULL, *b = NULL;
		size_t as = 0, bs = 0;
		TASK_CHECK(file_read(out_mt, &a, &as) == 0);
		TASK_CHECK(file_read(out_st, &b, &bs) == 0);
		TASK_CHECK(as == bs);
		TASK_CHECK(memcmp(a, b, as) == 0);
		free(a);
		free(b);
	}
	return 0;
}

static int case_mt_partial_order(void)
{
	char out[PATH_MAX_TEST];
	char cmd[4096];

	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	/* starts mid-frame, spans many queue circles, ends mid-frame */
	scratch_path(out, sizeof out, "mt_partial.raw");
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -M -t 2 -q -f raw --samples=8000:90000 '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, fx_big.tta, out
	);
	TASK_CHECK(run_sh(cmd) == 0);
	TASK_CHECK(check_raw_slice(&fx_big, out, 8000, 90000) == 0);

	/* first and last sample in the same (middle) frame */
	scratch_path(out, sizeof out, "mt_same.raw");
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -M -t 3 -q -f raw --samples=30000:30001 '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, fx_big.tta, out
	);
	TASK_CHECK(run_sh(cmd) == 0);
	TASK_CHECK(check_raw_slice(&fx_big, out, 30000, 30001) == 0);

	/* empty interval on the mt path */
	scratch_path(out, sizeof out, "mt_empty.raw");
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -M -t 2 -q -f raw --samples=40000:40000 '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, fx_big.tta, out
	);
	TASK_CHECK(run_sh(cmd) == 0);
	{
		uint8_t *b = NULL;
		size_t bs = 0;
		TASK_CHECK(file_read(out, &b, &bs) == 0);
		TASK_CHECK(bs == 0);
		free(b);
	}
	return 0;
}

static int case_mt_tail(void)
{
	char out[PATH_MAX_TEST];
	char cmd[4096];
	size_t total = fx_big.nsamples;
	size_t start = total - 100u;

	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	/* last short frame, reached past several queue circles */
	scratch_path(out, sizeof out, "mt_tail.raw");
	(void) snprintf(
		cmd, sizeof cmd,
		"%s decode -M -t 2 -q -f raw --samples=%zu:%zu '%s' "
		"-o '%s' >/dev/null 2>&1",
		PROG, start, total, fx_big.tta, out
	);
	TASK_CHECK(run_sh(cmd) == 0);
	TASK_CHECK(check_raw_slice(&fx_big, out, start, total) == 0);
	return 0;
}

/* //////////////////////////////////////////////////////////////////////// */

static int formats_one(
	const struct fixture *fx, const char *tag, size_t start, size_t end
)
{
	static const char *fmts[] = { "raw", "wav", "w64" };
	static const char *threads[] = { THREAD_ST, THREAD_MT };
	size_t fi, ti;

	for (fi = 0; fi < 3u; ++fi) {
		for (ti = 0; ti < 2u; ++ti) {
			char out[PATH_MAX_TEST];
			char name[128];
			char samples[64];
			int rc;

			(void) snprintf(
				name, sizeof name, "%s_%s_%s", tag,
				fmts[fi], ti == 0 ? "st" : "mt"
			);
			scratch_path(out, sizeof out, name);
			(void) file_remove(out);
			(void) snprintf(
				samples, sizeof samples, "%zu:%zu",
				start, end
			);
			rc = decode(
				threads[ti], fmts[fi], samples,
				fx->tta, out
			);
			TASK_CHECK(rc == 0);
			if (fi == 0) {
				TASK_CHECK(check_raw_slice(
					fx, out, start, end) == 0);
			}
			else {
				TASK_CHECK(check_container_slice(
					fx, out, start, end,
					fi == 2) == 0);
			}
		}
	}
	return 0;
}

/* every slice shape, in raw/wav/w64, through both decode paths */
static int formats_scenarios(const struct fixture *fx, const char *tag)
{
	const size_t fl = libttaR_nsamples_perframe_tta1(fx->rate);
	const size_t total = fx->nsamples;
	char sub[64];

	/* both ends inside one frame */
	(void) snprintf(sub, sizeof sub, "%s_same", tag);
	TASK_CHECK(formats_one(fx, sub, 100, 200) == 0);
	/* straddles a frame boundary */
	(void) snprintf(sub, sizeof sub, "%s_cross", tag);
	TASK_CHECK(formats_one(fx, sub, fl - 50u, fl + 50u) == 0);
	/* exactly one full frame, aligned to the boundary */
	(void) snprintf(sub, sizeof sub, "%s_bound", tag);
	TASK_CHECK(formats_one(
		fx, sub, fl, (total < 2u * fl ? total : 2u * fl)) == 0);
	/* the final, possibly short, frame */
	(void) snprintf(sub, sizeof sub, "%s_tail", tag);
	TASK_CHECK(formats_one(fx, sub, total - 37u, total) == 0);
	/* empty interval */
	(void) snprintf(sub, sizeof sub, "%s_empty", tag);
	TASK_CHECK(formats_one(fx, sub, 100, 100) == 0);
	return 0;
}

static int case_formats_i16_stereo(void)
{
	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	TASK_CHECK(formats_scenarios(&fx_small, "fmt16") == 0);
	return 0;
}

static int case_formats_u8_mono(void)
{
	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	TASK_CHECK(formats_scenarios(&fx_mono8, "fmt8") == 0);
	return 0;
}

static int case_formats_i24(void)
{
	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	TASK_CHECK(formats_scenarios(&fx_s24, "fmt24") == 0);
	return 0;
}

static int case_formats_multichannel(void)
{
	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	/* 4 channels, asymmetric per channel; raw and containers */
	TASK_CHECK(formats_scenarios(&fx_quad, "fmtquad") == 0);
	return 0;
}

static int case_formats_other_rate(void)
{
	const size_t fl = libttaR_nsamples_perframe_tta1(fx_alt.rate);

	if (ensure_fixtures() != 0) {
		TASK_CHECK(0);
	}
	/* a different samplerate means a different frame length */
	TASK_CHECK(formats_one(&fx_alt, "fmt_alt", fl - 10u, fl + 10u) == 0);
	TASK_CHECK(formats_one(
		&fx_alt, "fmt_alt2", 0u, fl) == 0);
	return 0;
}

/* //////////////////////////////////////////////////////////////////////// */

/*@unchecked@*/
const struct task_case task_cases[] = {
	/* slices */
	{ "slices", "same_frame",         case_same_frame },
	{ "slices", "cross_frame",        case_cross_frame },
	{ "slices", "frame_boundary",     case_frame_boundary },
	{ "slices", "last_short_frame",   case_last_short_frame },
	{ "slices", "empty_interval",     case_empty_interval },
	{ "slices", "multiple_inputs",    case_multiple_inputs },
	{ "slices", "stats_samples",      case_stats },
	{ "slices", "corrupt_outside",    case_corrupt_outside },
	{ "slices", "corrupt_intersect",  case_corrupt_intersect },
	{ "slices", "invalid_range",      case_invalid_range },
	{ "slices", "bad_seektable",      case_bad_seektable },

	/* queues */
	{ "queues", "mt_tiny_selection",  case_mt_tiny_selection },
	{ "queues", "mt_queue_circles",   case_mt_queue_circles },
	{ "queues", "mt_partial_order",   case_mt_partial_order },
	{ "queues", "mt_tail",            case_mt_tail },

	/* formats */
	{ "formats", "i16_stereo",        case_formats_i16_stereo },
	{ "formats", "u8_mono",           case_formats_u8_mono },
	{ "formats", "i24",               case_formats_i24 },
	{ "formats", "multichannel",      case_formats_multichannel },
	{ "formats", "other_rate",        case_formats_other_rate }
};

/*@unchecked@*/
const size_t task_case_count = sizeof task_cases / sizeof task_cases[0];

/* EOF //////////////////////////////////////////////////////////////////// */
