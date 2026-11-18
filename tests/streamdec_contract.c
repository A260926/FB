/* ///////////////////////////////////////////////////////////////////////////
//                                                                          //
// tests/streamdec_contract.c                                               //
//                                                                          //
// Contract and interoperability tests for the libttaR streaming frame      //
// decoder (libttaR_streamdec_*).                                           //
//                                                                          //
// Every compressed frame used here is produced by the real libttaR         //
// encoder; decoded PCM is compared point by point against both the         //
// original PCM and a full-buffer libttaR_tta_decode() reference.           //
//                                                                          //
/////////////////////////////////////////////////////////////////////////// */

#include "task_support/check.h"

#include "src/libttaR.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* //////////////////////////////////////////////////////////////////////// */

#define SENTINEL	((int32_t) 0x5A5A5A5A)
#define GUARD_BYTES	((size_t) 8u)
#define MAX_CALLS	((size_t) 4000000u)

static void *
ts_align(void *const p)
{
	return (void *) ((((uintptr_t) p) + 15u) & ~(uintptr_t) 15u);
}

/* ------------------------------------------------------------------------ */

struct enc_frame {
	uint8_t	*buf;		/* payload followed by the 4-byte CRC */
	size_t	 len;		/* payload + 4: what the seektable stores */
	size_t	 payload;	/* compressed payload length */
	uint32_t crc;
};

/* encode with an explicit destination capacity; some pathological but legal
   PCM needs far more than the usual 2 * margin + 16 * nsamples bytes */
static int
encode_frame_cap(
	const unsigned int sb, const unsigned int ch,
	const int32_t *const pcm, const size_t frames, struct enc_frame *const out,
	const size_t cap0
)
{
	const size_t nsamples = frames * (size_t) ch;
	const size_t margin = libttaR_ttabuf_safety_margin(
		(enum LibTTAr_SampleBytes) sb, ch
	);
	const size_t cap = (cap0 != 0)
		? cap0 : ((2u * margin) + (nsamples * 16u) + 64u);
	const size_t psz = libttaR_codecstate_priv_size(ch);
	struct LibTTAr_CodecState_User user = LIBTTAr_CODECSTATE_USER_INIT;
	struct LibTTAr_EncMisc misc;
	void *codec;
	uint8_t *buf;
	size_t len, i;
	int retval;

	if ( (margin == 0) || (psz == 0) || (nsamples == 0) ){
		return -1;
	}
	buf   = (uint8_t *) calloc(cap + 4u, 1u);
	codec = calloc(psz + 16u, 1u);
	if ( (buf == NULL) || (codec == NULL) ){
		free(buf);
		free(codec);
		return -1;
	}

	misc.dest_len      = cap;
	misc.src_len       = nsamples;
	misc.ni32_target   = nsamples;
	misc.ni32_perframe = nsamples;
	misc.samplebytes   = (enum LibTTAr_SampleBytes) sb;
	misc.nchan         = ch;

	retval = (int) libttaR_tta_encode(
		buf, pcm, ts_align(codec), &user, &misc
	);
	free(codec);
	if ( (retval != LIBTTAr_ERV_OK_DONE) || (user.ni32_total != nsamples) ){
		free(buf);
		return -1;
	}
	len = user.nbytes_tta_total;
	if ( (len == 0) || (len + 4u > cap + 4u) ){
		free(buf);
		return -1;
	}
	for ( i = 0; i < 4u; ++i ){
		buf[len + i] = (uint8_t) (user.crc >> (8u * i));
	}
	out->buf     = buf;
	out->payload = len;
	out->len     = len + 4u;
	out->crc     = user.crc;
	return 0;
}

static int
encode_frame(
	const unsigned int sb, const unsigned int ch,
	const int32_t *const pcm, const size_t frames, struct enc_frame *const out
)
{
	return encode_frame_cap(sb, ch, pcm, frames, out, 0u);
}

/* full-buffer reference decode */
static int
decode_full(
	const unsigned int sb, const unsigned int ch, const struct enc_frame *fr,
	const int32_t *const expect, const size_t frames
)
{
	const size_t nsamples = frames * (size_t) ch;
	const size_t margin = libttaR_ttabuf_safety_margin(
		(enum LibTTAr_SampleBytes) sb, ch
	);
	const size_t psz = libttaR_codecstate_priv_size(ch);
	struct LibTTAr_CodecState_User user = LIBTTAr_CODECSTATE_USER_INIT;
	struct LibTTAr_DecMisc misc;
	void *codec;
	int32_t *pcm;
	int retval;

	codec = calloc(psz + 16u, 1u);
	pcm   = (int32_t *) calloc(nsamples, sizeof *pcm);
	if ( (codec == NULL) || (pcm == NULL) ){
		free(codec);
		free(pcm);
		return -1;
	}
	misc.dest_len            = nsamples;
	misc.src_len             = fr->payload + margin;
	misc.ni32_target         = nsamples;
	misc.nbytes_tta_target   = fr->payload;
	misc.ni32_perframe       = nsamples;
	misc.nbytes_tta_perframe = fr->payload;
	misc.samplebytes         = (enum LibTTAr_SampleBytes) sb;
	misc.nchan               = ch;

	retval = (int) libttaR_tta_decode(
		pcm, fr->buf, ts_align(codec), &user, &misc
	);
	if ( (retval == LIBTTAr_DRV_OK_DONE)
	    &&
	     (user.ni32_total == nsamples)
	    &&
	     (user.nbytes_tta_total == fr->payload)
	    &&
	     (memcmp(pcm, expect, nsamples * sizeof *pcm) == 0)
	){
		retval = 0;
	}
	else {	retval = -1; }
	free(codec);
	free(pcm);
	return retval;
}

/* ------------------------------------------------------------------------ */

/* deterministic PCM generator; values always fit 'sb' signed samples */
static void
gen_pcm(
	const unsigned int sb, const unsigned int ch, const size_t frames,
	const int kind, int32_t *const pcm
)
{
	const int32_t maxv = (sb == 1u) ? INT32_C(127)
		: (sb == 2u) ? INT32_C(32767) : INT32_C(8388607);
	const size_t jump_at = (frames * 3u) / 4u;
	uint32_t seed = UINT32_C(0x1234567)
		^ ((uint32_t) sb * UINT32_C(0x9E37))
		^ ((uint32_t) ch * UINT32_C(0x85EB))
		^ ((uint32_t) kind * UINT32_C(0xC2B2));
	size_t i;

	for ( i = 0; i < (frames * (size_t) ch); ++i ){
		uint32_t r;
		int32_t value;

		seed = (seed * UINT32_C(1664525)) + UINT32_C(1013904223);
		r = seed >> 8u;

		if ( kind == 0 ){
			/* smooth, moderate, and occasionally full scale */
			const int32_t span = maxv / 2;

			value = (int32_t) (r % (uint32_t) (2 * span + 1))
				- span;
			if ( (i % 61u) == 0u ){
				value = (i & 1u) ? maxv : -maxv;
			}
			else if ( (i % 17u) == 0u ){
				value /= 64;
			}
		}
		else {	const size_t frame = i / (size_t) ch;

			if ( frame < jump_at ){
				/* a long, quiet prefix drives rice.k to 0 */
				value = (int32_t) (r % 3u) - 1;
			}
			else if ( frame == jump_at ){
				value = maxv / 16;	/* a large jump */
			}
			else {	value = (int32_t) (r % 2001u) - 1000; }
		}
		pcm[i] = value;
	}
	return;
}

/* //////////////////////////////////////////////////////////////////////// */

struct run_stats {
	size_t	consumed;
	size_t	delivered;
	size_t	calls;
	int	ret;
};

struct run_opts {
	const uint8_t	*input;
	size_t		 total;		/* includes guard bytes */
	size_t		 frame_bytes;
	unsigned int	 sb;
	unsigned int	 ch;
	size_t		 sample_frames;
	const int32_t	*expect;
	const size_t	*chunks;
	size_t		 nchunks;
	const size_t	*caps;
	size_t		 ncaps;
	int		 expect_ret;
	int		 use_final;
	int		 check_pcm;	/* 0 for deliberately corrupted frames */
};

/* fragment-driven decode with per-call output/sentinel checks.
   When 'expect_ret' is negative the PCM produced before detection may be a
   prefix of the true audio only; it is then checked against the first
   'st.delivered' samples of a zero-filled expectation instead. */
static int
run_decode(const struct run_opts *const opt, struct run_stats *const st)
{
	const size_t ws_size = libttaR_streamdec_size(opt->ch);
	size_t cap_max = 1;
	size_t i, j, k;
	uint8_t *ws_base, *ws;
	int32_t *out;
	size_t pos, fidx, ci, cpi;
	int retval, finished;

	memset(st, 0, sizeof *st);
	if ( ws_size == 0 ){
		return 1;
	}
	for ( i = 0; i < opt->ncaps; ++i ){
		if ( opt->caps[i] > cap_max ){
			cap_max = opt->caps[i];
		}
	}
	ws_base = (uint8_t *) malloc(ws_size + 16u);
	out     = (int32_t *) malloc(cap_max * (size_t) opt->ch * sizeof *out);
	if ( (ws_base == NULL) || (out == NULL) ){
		free(ws_base);
		free(out);
		return 2;
	}
	ws = (uint8_t *) ts_align(ws_base);
	if ( libttaR_streamdec_init(
		ws, ws_size, opt->sb, opt->ch,
		opt->sample_frames, opt->frame_bytes
	) != 0 ){
		free(ws_base);
		free(out);
		return 3;
	}

	pos = 0;
	fidx = 0;
	ci = 0;
	cpi = 0;
	retval = -1000;
	finished = 0;

	for ( i = 0; i < MAX_CALLS; ++i ){
		size_t avail, n, cap, b, f, real_end;
		int fin;

		real_end = (opt->total < opt->frame_bytes)
			? opt->total : opt->frame_bytes;
		if ( pos < opt->total ){
			avail = opt->total - pos;
			n = (opt->chunks != NULL)
				? opt->chunks[ci % opt->nchunks] : avail;
			ci += 1;
			if ( n > avail ){ n = avail; }
			if ( n == 0u ){ n = 1u; }
		}
		else {	avail = 0;
			n = 0;
		}
		fin = (opt->use_final && ((pos + n) >= real_end));
		cap = opt->caps[cpi % opt->ncaps];
		cpi += 1;

		for ( j = 0; j < (cap_max * (size_t) opt->ch); ++j ){
			out[j] = SENTINEL;
		}
		b = n;
		f = cap;
		retval = libttaR_streamdec_process(
			ws, (n != 0u) ? (opt->input + pos) : NULL,
			&b, out, &f, fin
		);
		st->calls += 1;

		/* counts must be within the supplied limits */
		if ( (b > n) || (f > cap) ){
			retval = -9001;
			break;
		}
		/* delivered frames must match the expected PCM ... */
		for ( j = 0; (opt->check_pcm != 0) && (j < f); ++j ){
			if ( fidx + j >= opt->sample_frames ){
				retval = -9002;
				break;
			}
			for ( k = 0; k < (size_t) opt->ch; ++k ){
				if ( out[(j * (size_t) opt->ch) + k]
				     != opt->expect[((fidx + j) * (size_t) opt->ch) + k]
				){
					retval = -9003;
					break;
				}
			}
			if ( retval == -9003 ){ break; }
		}
		/* ... and everything past the delivered frames keeps the
		   sentinel value */
		if ( retval >= 0 ){
			for ( j = (f * (size_t) opt->ch);
			      j < (cap_max * (size_t) opt->ch); ++j
			){
				if ( out[j] != SENTINEL ){
					retval = -9004;
					break;
				}
			}
		}
		if ( retval < -90 ){
			break;
		}

		pos  += b;
		fidx += f;
		st->consumed  += b;
		st->delivered += f;

		if ( (retval == 1) || (retval < 0) ){
			finished = 1;
			break;
		}
		if ( retval != 0 ){
			retval = -9005;
			break;
		}
		/* a call with input and output room must make progress */
		if ( (b == 0u) && (f == 0u) ){
			if ( (n != 0u) && (cap != 0u) ){
				retval = -9006;
				break;
			}
			if ( (n == 0u) && (cap == 0u) ){
				retval = -9007;
				break;
			}
		}
	}

	st->ret = retval;
	if ( (finished == 0) || (retval == -9001) || (retval == -9002)
	    ||
	     (retval == -9003) || (retval == -9004)
	    ||
	     (retval == -9005) || (retval == -9006) || (retval == -9007)
	){
		free(ws_base);
		free(out);
		return 4;
	}
	if ( retval != opt->expect_ret ){
		/* -100 means "any negative value is acceptable" */
		if ( !((opt->expect_ret == -100) && (retval < 0)) ){
			free(ws_base);
			free(out);
			return 5;
		}
	}
	if ( st->consumed > opt->frame_bytes ){
		free(ws_base);
		free(out);
		return 7;
	}
	if ( retval == 1 ){
		if ( (pos != opt->frame_bytes)
		    ||
		     (fidx != opt->sample_frames)
		    ||
		     (st->consumed != opt->frame_bytes)
		    ||
		     (st->delivered != opt->sample_frames)
		){
			free(ws_base);
			free(out);
			return 6;
		}
	}
	free(ws_base);
	free(out);
	return 0;
}

/* one encoded frame plus guard bytes of a following frame */
static uint8_t *
make_input(const struct enc_frame *const fr, const uint8_t *const next,
	const size_t next_len, size_t *const total_out)
{
	uint8_t *buf;
	size_t fill;

	buf = (uint8_t *) malloc(fr->len + GUARD_BYTES);
	if ( buf == NULL ){
		return NULL;
	}
	memcpy(buf, fr->buf, fr->len);
	if ( (next != NULL) && (next_len != 0u) ){
		fill = (next_len < GUARD_BYTES) ? next_len : GUARD_BYTES;
		memcpy(buf + fr->len, next, fill);
	}
	else {	memset(buf + fr->len, 0xAB, GUARD_BYTES);
	}
	*total_out = fr->len + GUARD_BYTES;
	return buf;
}

/* //////////////////////////////////////////////////////////////////////// */
/* Fragments                                                                */
/* //////////////////////////////////////////////////////////////////////// */

static int
frag_chunks(void)
{
	static const size_t chunks_1[] = { 1u };
	static const size_t chunks_7[] = { 7u };
	static const size_t chunks_31[] = { 31u };
	static const size_t caps_1[] = { 1u };
	static const size_t caps_3[] = { 3u };
	static const unsigned int chans[] = { 1u, 2u, 3u, 6u, 8u };
	unsigned int sb, ci;
	size_t frames = 129u;

	for ( sb = 1; sb <= 3u; ++sb ){
		for ( ci = 0; ci < (sizeof chans / sizeof chans[0]); ++ci ){
			const unsigned int ch = chans[ci];
			const size_t nsamples = frames * (size_t) ch;
			int32_t *pcm = (int32_t *) calloc(nsamples, sizeof *pcm);
			struct enc_frame fr;
			const size_t *chunks[3] = { chunks_1, chunks_7, chunks_31 };
			const size_t *caps[2]   = { caps_1, caps_3 };
			size_t x, y;

			TASK_CHECK(pcm != NULL);
			gen_pcm(sb, ch, frames, 0, pcm);
			TASK_CHECK(encode_frame(sb, ch, pcm, frames, &fr) == 0);
			TASK_CHECK(decode_full(sb, ch, &fr, pcm, frames) == 0);

			for ( x = 0; x < 3u; ++x ){
				for ( y = 0; y < 2u; ++y ){
					struct run_opts opt;
					struct run_stats st;
					uint8_t *input;
					size_t total;

					input = make_input(
						&fr, NULL, 0, &total
					);
					TASK_CHECK(input != NULL);
					opt.input         = input;
					opt.total         = total;
					opt.frame_bytes   = fr.len;
					opt.sb            = sb;
					opt.ch            = ch;
					opt.sample_frames = frames;
					opt.expect        = pcm;
					opt.chunks        = chunks[x];
					opt.nchunks       = 1u;
					opt.caps          = caps[y];
					opt.ncaps         = 1u;
					opt.expect_ret    = 1;
					opt.use_final     = 1;
					opt.check_pcm     = 1;
					TASK_CHECK(run_decode(&opt, &st) == 0);
					free(input);
				}
			}
			free(fr.buf);
			free(pcm);
		}
	}
	return 0;
}

static int
frag_zero_capacity(void)
{
	static const size_t chunks[] = { 5u };
	static const size_t caps[] = { 0u, 0u, 3u };
	static const unsigned int chans[] = { 1u, 3u, 8u };
	unsigned int sb, ci;
	size_t frames = 65u;

	for ( sb = 1; sb <= 3u; ++sb ){
		for ( ci = 0; ci < (sizeof chans / sizeof chans[0]); ++ci ){
			const unsigned int ch = chans[ci];
			const size_t nsamples = frames * (size_t) ch;
			int32_t *pcm = (int32_t *) calloc(nsamples, sizeof *pcm);
			struct enc_frame fr;
			struct run_opts opt;
			struct run_stats st;
			uint8_t *input;
			size_t total;

			TASK_CHECK(pcm != NULL);
			gen_pcm(sb, ch, frames, 0, pcm);
			TASK_CHECK(encode_frame(sb, ch, pcm, frames, &fr) == 0);
			input = make_input(&fr, NULL, 0, &total);
			TASK_CHECK(input != NULL);

			opt.input         = input;
			opt.total         = total;
			opt.frame_bytes   = fr.len;
			opt.sb            = sb;
			opt.ch            = ch;
			opt.sample_frames = frames;
			opt.expect        = pcm;
			opt.chunks        = chunks;
			opt.nchunks       = 1u;
			opt.caps          = caps;
			opt.ncaps         = 3u;
			opt.expect_ret    = 1;
			opt.use_final     = 1;
			opt.check_pcm     = 1;
			TASK_CHECK(run_decode(&opt, &st) == 0);
			/* frames must never be delivered whole unless all of them
			   arrived; every delivered frame matched above */
			free(input);
			free(fr.buf);
			free(pcm);
		}
	}
	return 0;
}

/* find the longest run of 0xFF bytes in the payload */
static void
longest_ff_run(
	const uint8_t *const buf, const size_t len, size_t *const start,
	size_t *const run
)
{
	size_t i, best = 0, best_at = 0, cur = 0, cur_at = 0;

	for ( i = 0; i < len; ++i ){
		if ( buf[i] == 0xFFu ){
			if ( cur == 0u ){ cur_at = i; }
			cur += 1;
			if ( cur > best ){
				best    = cur;
				best_at = cur_at;
			}
		}
		else {	cur = 0; }
	}
	*start = best_at;
	*run   = best;
	return;
}

static int
frag_long_unary(void)
{
	const unsigned int sb = 3u, ch = 1u;
	const size_t frames = 4200u;
	const size_t nsamples = frames;
	int32_t *pcm = (int32_t *) calloc(nsamples, sizeof *pcm);
	struct enc_frame fr;
	size_t ws_size, run, run_start, i;
	uint8_t *ws_base, *ws;
	int32_t *out;
	size_t pos, fidx;
	int paused_calls = 0;

	TASK_CHECK(pcm != NULL);
	/* a long, very quiet prefix (this drives rice.k to 0), then one
	   sample whose Rice code is a legal unary longer than the workspace */
	for ( i = 0; i < nsamples; ++i ){
		pcm[i] = ((int32_t) (i & 1u) << 1) - 1;
	}
	pcm[4160] = INT32_C(8388607);
	pcm[4161] = INT32_C(-8388607);
	pcm[4162] = INT32_C(8388607);
	TASK_CHECK(encode_frame_cap(sb, ch, pcm, frames, &fr, 12u * 1024u * 1024u) == 0);
	TASK_CHECK(decode_full(sb, ch, &fr, pcm, frames) == 0);
	ws_size = libttaR_streamdec_size(ch);
	TASK_CHECK(ws_size > 0);
	TASK_CHECK(ws_size <= 4096u * (ch + 1u));

	longest_ff_run(fr.buf, fr.payload, &run_start, &run);
	/* the unary really is longer than the whole workspace */
	TASK_CHECK(run > ws_size);
	TASK_CHECK(run_start > 0u);

	ws_base = (uint8_t *) malloc(ws_size + 16u);
	out = (int32_t *) malloc(8u * sizeof *out);
	TASK_CHECK(ws_base != NULL && out != NULL);
	ws = (uint8_t *) ts_align(ws_base);
	TASK_CHECK(libttaR_streamdec_init(
		ws, ws_size, sb, ch, frames, fr.len
	) == 0);

	pos  = 0;
	fidx = 0;
	while ( pos < fr.len ){
		size_t b = 1u, f = 8u;
		int r;
		int inside_unary;

		inside_unary = (pos >= run_start + 1u)
			&& (pos < run_start + run);
		for ( i = 0; i < 8u; ++i ){ out[i] = SENTINEL; }
		r = libttaR_streamdec_process(
			ws, fr.buf + pos, &b, out, &f, (pos + 1u == fr.len)
		);
		TASK_CHECK(r >= 0);
		TASK_CHECK(b == 1u);
		if ( inside_unary ){
			/* pausing in the middle of the long unary must not
			   complete a symbol */
			TASK_CHECK(f == 0u);
			TASK_CHECK(r == 0);
			paused_calls += 1;
		}
		for ( i = 0; i < f; ++i ){
			TASK_CHECK(fidx + i < frames);
			TASK_CHECK(out[i] == pcm[fidx + i]);
		}
		for ( i = f; i < 8u; ++i ){
			TASK_CHECK(out[i] == SENTINEL);
		}
		pos  += b;
		fidx += f;
	}
	/* the frame must have been verified by now */
	{
		size_t b = 0, f = 8u;
		int r = libttaR_streamdec_process(
			ws, NULL, &b, out, &f, 1
		);

		TASK_CHECK(r == 1);
		TASK_CHECK(b == 0u);
		TASK_CHECK(f == 0u);
	}
	TASK_CHECK(fidx == frames);
	/* it really paused many times inside that one unary code (the run is
	   over two million bytes); check a sample of the loop, not each byte */
	TASK_CHECK(paused_calls > 1000000);

	free(ws_base);
	free(out);
	free(fr.buf);
	free(pcm);
	return 0;
}

static int
frag_crc_split(void)
{
	const unsigned int sb = 2u, ch = 2u;
	const size_t frames = 33u;
	const size_t nsamples = frames * ch;
	int32_t *pcm = (int32_t *) calloc(nsamples, sizeof *pcm);
	struct enc_frame fr;
	size_t chunks_raw[3];
	const size_t *chunks;
	struct run_opts opt;
	struct run_stats st;
	uint8_t *input;
	size_t total;

	TASK_CHECK(pcm != NULL);
	gen_pcm(sb, ch, frames, 0, pcm);
	TASK_CHECK(encode_frame(sb, ch, pcm, frames, &fr) == 0);
	TASK_CHECK(fr.len > 8u);
	/* split the trailing CRC into three pieces */
	chunks_raw[0] = fr.len - 6u;
	chunks_raw[1] = 3u;
	chunks_raw[2] = 3u;
	chunks = chunks_raw;

	input = make_input(&fr, NULL, 0, &total);
	TASK_CHECK(input != NULL);
	opt.input         = input;
	opt.total         = total;
	opt.frame_bytes   = fr.len;
	opt.sb            = sb;
	opt.ch            = ch;
	opt.sample_frames = frames;
	opt.expect        = pcm;
	opt.chunks        = chunks;
	opt.nchunks       = 3u;
	opt.caps          = NULL;
	opt.ncaps         = 0u;
	opt.expect_ret    = 1;
	opt.use_final     = 1;
	opt.check_pcm     = 1;
	/* run_decode needs a cap pattern; use one big enough for everything */
	{
		static const size_t caps[] = { 7u };

		opt.caps  = caps;
		opt.ncaps = 1u;
		TASK_CHECK(run_decode(&opt, &st) == 0);
	}
	free(input);
	free(fr.buf);
	free(pcm);
	return 0;
}

static int
frag_crc_mismatch(void)
{
	const unsigned int sb = 2u, ch = 2u;
	const size_t frames = 40u;
	const size_t nsamples = frames * ch;
	int32_t *pcm = (int32_t *) calloc(nsamples, sizeof *pcm);
	struct enc_frame fr;
	static const size_t chunks[] = { 7u };
	static const size_t caps[] = { 2u };
	struct run_opts opt;
	struct run_stats st;
	uint8_t *input;
	size_t total;

	TASK_CHECK(pcm != NULL);
	gen_pcm(sb, ch, frames, 0, pcm);
	TASK_CHECK(encode_frame(sb, ch, pcm, frames, &fr) == 0);
	input = make_input(&fr, NULL, 0, &total);
	TASK_CHECK(input != NULL);
	/* corrupt one byte of the trailing little-endian CRC */
	input[fr.len - 2u] ^= 0x40u;

	opt.input         = input;
	opt.total         = fr.len;	/* only the damaged frame */
	opt.frame_bytes   = fr.len;
	opt.sb            = sb;
	opt.ch            = ch;
	opt.sample_frames = frames;
	opt.expect        = pcm;
	opt.chunks        = chunks;
	opt.nchunks       = 1u;
	opt.caps          = caps;
	opt.ncaps         = 1u;
	opt.expect_ret    = -3;
	opt.use_final     = 1;
	opt.check_pcm     = 0;
	TASK_CHECK(run_decode(&opt, &st) == 0);
	free(input);
	free(fr.buf);
	free(pcm);
	return 0;
}

static int
frag_payload_corrupt(void)
{
	const unsigned int sb = 3u, ch = 1u;
	const size_t frames = 50u;
	int32_t *pcm = (int32_t *) calloc(frames, sizeof *pcm);
	struct enc_frame fr;
	static const size_t chunks[] = { 3u };
	static const size_t caps[] = { 4u };
	struct run_opts opt;
	struct run_stats st;
	uint8_t *input;
	size_t total;

	TASK_CHECK(pcm != NULL);
	gen_pcm(sb, ch, frames, 0, pcm);
	TASK_CHECK(encode_frame(sb, ch, pcm, frames, &fr) == 0);
	TASK_CHECK(fr.len > 16u);
	input = make_input(&fr, NULL, 0, &total);
	TASK_CHECK(input != NULL);
	input[fr.payload / 2u] ^= 0x11u;

	opt.input         = input;
	opt.total         = fr.len;	/* only the damaged frame */
	opt.frame_bytes   = fr.len;
	opt.sb            = sb;
	opt.ch            = ch;
	opt.sample_frames = frames;
	opt.expect        = pcm;
	opt.chunks        = chunks;
	opt.nchunks       = 1u;
	opt.caps          = caps;
	opt.ncaps         = 1u;
	opt.expect_ret    = -100;	/* any detected error, never verified */
	opt.use_final     = 1;
	opt.check_pcm     = 0;
	/* corrupting a payload byte may manifest as -2, -3, or -4; what
	   matters is that it is detected, never reported as verified */
	TASK_CHECK(run_decode(&opt, &st) == 0);
	TASK_CHECK(st.ret < 0);
	free(input);
	free(fr.buf);
	free(pcm);
	return 0;
}

static int
frag_truncated(void)
{
	const unsigned int sb = 2u, ch = 3u;
	const size_t frames = 48u;
	const size_t nsamples = frames * ch;
	int32_t *pcm = (int32_t *) calloc(nsamples, sizeof *pcm);
	struct enc_frame fr;
	static const size_t chunks[] = { 5u };
	static const size_t caps[] = { 2u };
	struct run_opts opt;
	struct run_stats st;
	size_t cut, total;

	TASK_CHECK(pcm != NULL);
	gen_pcm(sb, ch, frames, 0, pcm);
	TASK_CHECK(encode_frame(sb, ch, pcm, frames, &fr) == 0);
	TASK_CHECK(fr.len > 8u);

	/* payload complete, but the last CRC byte missing */
	cut = fr.len - 1u;
	opt.input         = fr.buf;
	opt.total         = cut;
	opt.frame_bytes   = fr.len;
	opt.sb            = sb;
	opt.ch            = ch;
	opt.sample_frames = frames;
	opt.expect        = pcm;
	opt.chunks        = chunks;
	opt.nchunks       = 1u;
	opt.caps          = caps;
	opt.ncaps         = 1u;
	opt.expect_ret    = -2;
	opt.use_final     = 1;
	opt.check_pcm     = 1;
	TASK_CHECK(run_decode(&opt, &st) == 0);

	/* and again truncating in the middle of the payload */
	{
		uint8_t *input = make_input(&fr, NULL, 0, &total);

		TASK_CHECK(input != NULL);
		cut = fr.len / 2u;
		opt.input = input;
		opt.total = cut;
		TASK_CHECK(run_decode(&opt, &st) == 0);
		free(input);
	}
	free(fr.buf);
	free(pcm);
	return 0;
}

static int
frag_short_frame_reset(void)
{
	const unsigned int sb = 1u, ch = 1u;
	const size_t frames_a = 3u, frames_b = 7u;
	int32_t pcm_a[3] = { 3, -7, 11 };
	int32_t pcm_b[7] = { 1, 2, 3, -4, 5, -6, 7 };
	struct enc_frame fa, fb;
	size_t ws_size = libttaR_streamdec_size(ch);
	uint8_t *ws_base = (uint8_t *) malloc(ws_size + 16u);
	uint8_t *ws;
	int32_t out[7];
	size_t b, f, i;
	int r;

	TASK_CHECK(ws_base != NULL);
	ws = (uint8_t *) ts_align(ws_base);
	TASK_CHECK(encode_frame(sb, ch, pcm_a, frames_a, &fa) == 0);
	TASK_CHECK(encode_frame(sb, ch, pcm_b, frames_b, &fb) == 0);

	/* first a valid short frame */
	TASK_CHECK(libttaR_streamdec_init(
		ws, ws_size, sb, ch, frames_a, fa.len
	) == 0);
	b = fa.len;
	f = frames_a;
	r = libttaR_streamdec_process(ws, fa.buf, &b, out, &f, 1);
	TASK_CHECK(r == 1);
	TASK_CHECK(b == fa.len);
	TASK_CHECK(f == frames_a);
	for ( i = 0; i < frames_a; ++i ){
		TASK_CHECK(out[i] == pcm_a[i]);
	}
	/* re-initialize the very same workspace for the next frame */
	TASK_CHECK(libttaR_streamdec_init(
		ws, ws_size, sb, ch, frames_b, fb.len
	) == 0);
	b = fb.len;
	f = frames_b;
	r = libttaR_streamdec_process(ws, fb.buf, &b, out, &f, 1);
	TASK_CHECK(r == 1);
	TASK_CHECK(b == fb.len);
	TASK_CHECK(f == frames_b);
	for ( i = 0; i < frames_b; ++i ){
		TASK_CHECK(out[i] == pcm_b[i]);
	}

	free(fa.buf);
	free(fb.buf);
	free(ws_base);
	return 0;
}

static int
frag_two_instances(void)
{
	const unsigned int ch = 2u;
	const size_t frames = 41u;
	const size_t nsamples = frames * ch;
	int32_t *pcm_a = (int32_t *) calloc(nsamples, sizeof *pcm_a);
	int32_t *pcm_b = (int32_t *) calloc(nsamples, sizeof *pcm_b);
	struct enc_frame fa, fb;
	const size_t ws_size = libttaR_streamdec_size(ch);
	uint8_t *wsa_base, *wsb_base, *wsa, *wsb;
	int32_t out_a[8], out_b[8];
	size_t pa = 0, pb = 0, fa_done = 0, fb_done = 0, i;
	int ra = 0, rb = 0;

	TASK_CHECK(pcm_a != NULL && pcm_b != NULL);
	gen_pcm(2u, ch, frames, 0, pcm_a);
	gen_pcm(3u, ch, frames, 0, pcm_b);
	TASK_CHECK(encode_frame(2u, ch, pcm_a, frames, &fa) == 0);
	TASK_CHECK(encode_frame(3u, ch, pcm_b, frames, &fb) == 0);

	wsa_base = (uint8_t *) malloc(ws_size + 16u);
	wsb_base = (uint8_t *) malloc(ws_size + 16u);
	TASK_CHECK(wsa_base != NULL && wsb_base != NULL);
	wsa = (uint8_t *) ts_align(wsa_base);
	wsb = (uint8_t *) ts_align(wsb_base);
	TASK_CHECK(libttaR_streamdec_init(wsa, ws_size, 2u, ch, frames, fa.len) == 0);
	TASK_CHECK(libttaR_streamdec_init(wsb, ws_size, 3u, ch, frames, fb.len) == 0);

	/* alternate one input byte at a time */
	for ( i = 0; i < MAX_CALLS; ++i ){
		if ( (ra == 0) && (pa < fa.len) ){
			size_t b = 1u, f = 4u;
			int r = libttaR_streamdec_process(
				wsa, fa.buf + pa, &b, out_a, &f,
				(pa + 1u == fa.len)
			);

			TASK_CHECK(r >= 0);
			pa += b;
			ra = r;
			TASK_CHECK(fa_done + f <= frames);
			{
				size_t j;

				for ( j = 0; j < f; ++j ){
					size_t k;

					for ( k = 0; k < ch; ++k ){
						TASK_CHECK(
							out_a[j * ch + k]
							== pcm_a[(fa_done + j) * ch + k]
						);
					}
				}
			}
			fa_done += f;
			if ( ra < 0 ){ return 1; }
		}
		if ( (rb == 0) && (pb < fb.len) ){
			size_t b = 1u, f = 4u;
			int r = libttaR_streamdec_process(
				wsb, fb.buf + pb, &b, out_b, &f,
				(pb + 1u == fb.len)
			);

			TASK_CHECK(r >= 0);
			pb += b;
			rb = r;
			TASK_CHECK(fb_done + f <= frames);
			{
				size_t j;

				for ( j = 0; j < f; ++j ){
					size_t k;

					for ( k = 0; k < ch; ++k ){
						TASK_CHECK(
							out_b[j * ch + k]
							== pcm_b[(fb_done + j) * ch + k]
						);
					}
				}
			}
			fb_done += f;
			if ( rb < 0 ){ return 1; }
		}
		if ( (ra == 1) && (rb == 1) ){
			break;
		}
		if ( (pa == fa.len) && (pb == fb.len) ){
			TASK_CHECK(0);	/* neither instance finished */
			break;
		}
	}
	TASK_CHECK(ra == 1);
	TASK_CHECK(rb == 1);
	TASK_CHECK(fa_done == frames);
	TASK_CHECK(fb_done == frames);
	TASK_CHECK(pa == fa.len);
	TASK_CHECK(pb == fb.len);

	free(fa.buf);
	free(fb.buf);
	free(wsa_base);
	free(wsb_base);
	free(pcm_a);
	free(pcm_b);
	return 0;
}

static int
frag_adjacent_frames(void)
{
	const unsigned int sb = 2u, ch = 2u;
	const size_t frames = 24u;
	const size_t nsamples = frames * ch;
	int32_t *pcm_a = (int32_t *) calloc(nsamples, sizeof *pcm_a);
	int32_t *pcm_b = (int32_t *) calloc(nsamples, sizeof *pcm_b);
	struct enc_frame fa, fb;
	static const size_t chunks[] = { 3u };
	static const size_t caps[] = { 2u };
	struct run_opts opt;
	struct run_stats st;
	uint8_t *input;
	size_t total;

	TASK_CHECK(pcm_a != NULL && pcm_b != NULL);
	gen_pcm(sb, ch, frames, 0, pcm_a);
	gen_pcm(sb, ch, frames, 1, pcm_b);
	TASK_CHECK(encode_frame(sb, ch, pcm_a, frames, &fa) == 0);
	TASK_CHECK(encode_frame(sb, ch, pcm_b, frames, &fb) == 0);

	/* both frames in one buffer; the decoder must stop exactly at the
	   end of the first one */
	input = (uint8_t *) malloc(fa.len + fb.len);
	TASK_CHECK(input != NULL);
	memcpy(input, fa.buf, fa.len);
	memcpy(input + fa.len, fb.buf, fb.len);
	total = fa.len + fb.len;

	opt.input         = input;
	opt.total         = total;
	opt.frame_bytes   = fa.len;
	opt.sb            = sb;
	opt.ch            = ch;
	opt.sample_frames = frames;
	opt.expect        = pcm_a;
	opt.chunks        = chunks;
	opt.nchunks       = 1u;
	opt.caps          = caps;
	opt.ncaps         = 1u;
	opt.expect_ret    = 1;
	opt.use_final     = 0;	/* the second frame follows! */
	opt.check_pcm     = 1;
	TASK_CHECK(run_decode(&opt, &st) == 0);
	TASK_CHECK(st.consumed == fa.len);

	free(input);
	free(fa.buf);
	free(fb.buf);
	free(pcm_a);
	free(pcm_b);
	return 0;
}

/* //////////////////////////////////////////////////////////////////////// */
/* Channels                                                                 */
/* //////////////////////////////////////////////////////////////////////// */

static int
chan_all_counts(void)
{
	static const size_t chunks[] = { 31u };
	static const size_t caps[] = { 1u };
	unsigned int sb, ch;

	for ( sb = 1; sb <= 3u; ++sb ){
		for ( ch = 1; ch <= 8u; ++ch ){
			const size_t frames = 97u;
			const size_t nsamples = frames * (size_t) ch;
			int32_t *pcm = (int32_t *) calloc(nsamples, sizeof *pcm);
			struct enc_frame fr;
			struct run_opts opt;
			struct run_stats st;
			uint8_t *input;
			size_t total;

			TASK_CHECK(pcm != NULL);
			gen_pcm(sb, ch, frames, 0, pcm);
			TASK_CHECK(encode_frame(sb, ch, pcm, frames, &fr) == 0);
			TASK_CHECK(decode_full(sb, ch, &fr, pcm, frames) == 0);
			input = make_input(&fr, NULL, 0, &total);
			TASK_CHECK(input != NULL);

			opt.input         = input;
			opt.total         = total;
			opt.frame_bytes   = fr.len;
			opt.sb            = sb;
			opt.ch            = ch;
			opt.sample_frames = frames;
			opt.expect        = pcm;
			opt.chunks        = chunks;
			opt.nchunks       = 1u;
			opt.caps          = caps;
			opt.ncaps         = 1u;
			opt.expect_ret    = 1;
			opt.use_final     = 1;
			opt.check_pcm     = 1;
			TASK_CHECK(run_decode(&opt, &st) == 0);

			free(input);
			free(fr.buf);
			free(pcm);
		}
	}
	return 0;
}

static int
chan_group_not_complete(void)
{
	const unsigned int ch = 3u;
	const size_t frames = 20u;
	const size_t nsamples = frames * ch;
	int32_t *pcm = (int32_t *) calloc(nsamples, sizeof *pcm);
	struct enc_frame fr;
	const size_t ws_size = libttaR_streamdec_size(ch);
	uint8_t *ws_base = (uint8_t *) malloc(ws_size + 16u);
	uint8_t *ws;
	int32_t out[8];
	size_t b, f, i;
	int r;

	TASK_CHECK(pcm != NULL);
	gen_pcm(2u, ch, frames, 0, pcm);
	TASK_CHECK(encode_frame(2u, ch, pcm, frames, &fr) == 0);
	TASK_CHECK(ws_base != NULL);
	ws = (uint8_t *) ts_align(ws_base);
	TASK_CHECK(libttaR_streamdec_init(
		ws, ws_size, 2u, ch, frames, fr.len
	) == 0);

	for ( i = 0; i < 8u; ++i ){ out[i] = SENTINEL; }
	/* one input byte cannot complete a channel group of three */
	b = 1u;
	f = 8u;
	r = libttaR_streamdec_process(ws, fr.buf, &b, out, &f, 0);
	TASK_CHECK(r == 0);
	TASK_CHECK(b == 1u);
	TASK_CHECK(f == 0u);
	for ( i = 0; i < 8u; ++i ){
		TASK_CHECK(out[i] == SENTINEL);
	}

	/* with capacity for exactly one frame it still needs all channels */
	for ( i = 0; i < 8u; ++i ){ out[i] = SENTINEL; }
	b = 2u;
	f = 1u;
	r = libttaR_streamdec_process(ws, fr.buf + 1u, &b, out, &f, 0);
	TASK_CHECK(r == 0);
	TASK_CHECK(f <= 1u);

	free(fr.buf);
	free(ws_base);
	free(pcm);
	return 0;
}

static int
chan_capacity_variants(void)
{
	static const size_t chunk_sets[3][1] = { {2u}, {13u}, {1u} };
	static const size_t caps_1[] = { 1u };
	static const size_t caps_3[] = { 3u };
	static const size_t caps_all[] = { 1000u };
	const size_t frames = 33u;
	unsigned int sb, ch, c;

	for ( sb = 1; sb <= 3u; ++sb ){
		for ( ch = 3u; ch <= 8u; ch += 5u ){
			const size_t nsamples = frames * (size_t) ch;
			int32_t *pcm = (int32_t *) calloc(nsamples, sizeof *pcm);
			struct enc_frame fr;

			TASK_CHECK(pcm != NULL);
			gen_pcm(sb, ch, frames, 0, pcm);
			TASK_CHECK(encode_frame(sb, ch, pcm, frames, &fr) == 0);

			for ( c = 0; c < 3u; ++c ){
				struct run_opts opt;
				struct run_stats st;
				uint8_t *input;
				size_t total;

				input = make_input(&fr, NULL, 0, &total);
				TASK_CHECK(input != NULL);
				opt.input         = input;
				opt.total         = total;
				opt.frame_bytes   = fr.len;
				opt.sb            = sb;
				opt.ch            = ch;
				opt.sample_frames = frames;
				opt.expect        = pcm;
				opt.chunks        = chunk_sets[c];
				opt.nchunks       = 1u;
				opt.caps          = (c == 0u) ? caps_1
					: (c == 1u) ? caps_3 : caps_all;
				opt.ncaps         = 1u;
				opt.expect_ret    = 1;
				opt.use_final     = 1;
				opt.check_pcm     = 1;
				TASK_CHECK(run_decode(&opt, &st) == 0);
				free(input);
			}
			free(fr.buf);
			free(pcm);
		}
	}
	return 0;
}

static int
chan_randomized_differential(void)
{
	/* many random (samplebytes, nchan, frame count, chunk, capacity)
	   combinations; each decode is checked against the encoder's input */
	uint32_t seed = UINT32_C(0xC0FFEE11);
	unsigned int trial;

	for ( trial = 0; trial < 60u; ++trial ){
		unsigned int sb, ch;
		size_t frames, frame_bytes;
		size_t nsamples;
		int32_t *pcm;
		struct enc_frame fr;
		static const size_t caps_pool[] = { 1u, 2u, 3u, 5u, 8u, 64u };
		static const size_t chunk_pool[] = { 1u, 2u, 7u, 13u, 31u, 97u };
		struct run_opts opt;
		struct run_stats st;
		uint8_t *input;
		size_t total;

		seed = (seed * UINT32_C(1664525)) + UINT32_C(1013904223);
		sb = (seed >> 1u) % 3u + 1u;
		seed = (seed * UINT32_C(1664525)) + UINT32_C(1013904223);
		ch = (seed >> 2u) % 8u + 1u;
		seed = (seed * UINT32_C(1664525)) + UINT32_C(1013904223);
		frames = (seed >> 3u) % 150u + 1u;

		nsamples = frames * (size_t) ch;
		pcm = (int32_t *) calloc(nsamples, sizeof *pcm);
		TASK_CHECK(pcm != NULL);
		gen_pcm(sb, ch, frames, (int) (trial % 2u), pcm);
		TASK_CHECK(encode_frame(sb, ch, pcm, frames, &fr) == 0);
		frame_bytes = fr.len;

		input = make_input(&fr, NULL, 0, &total);
		TASK_CHECK(input != NULL);

		seed = (seed * UINT32_C(1664525)) + UINT32_C(1013904223);
		opt.chunks = &chunk_pool[
			(seed >> 5u) % (sizeof chunk_pool / sizeof chunk_pool[0])
		];
		opt.nchunks = 1u;
		seed = (seed * UINT32_C(1664525)) + UINT32_C(1013904223);
		opt.caps = &caps_pool[
			(seed >> 7u) % (sizeof caps_pool / sizeof caps_pool[0])
		];
		opt.ncaps = 1u;
		opt.input         = input;
		opt.total         = total;
		opt.frame_bytes   = frame_bytes;
		opt.sb            = sb;
		opt.ch            = ch;
		opt.sample_frames = frames;
		opt.expect        = pcm;
		opt.expect_ret    = 1;
		opt.use_final     = 1;
		opt.check_pcm     = 1;
		TASK_CHECK(run_decode(&opt, &st) == 0);
		TASK_CHECK(st.consumed == frame_bytes);
		TASK_CHECK(st.delivered == frames);

		free(input);
		free(fr.buf);
		free(pcm);
	}
	return 0;
}

static int
chan_output_sentinel(void)
{
	const unsigned int sb = 2u, ch = 4u;
	const size_t frames = 5u;
	const size_t nsamples = frames * ch;
	int32_t *pcm = (int32_t *) calloc(nsamples, sizeof *pcm);
	struct enc_frame fr;
	static const size_t caps[] = { 8u };
	struct run_opts opt;
	struct run_stats st;
	uint8_t *input;
	size_t total;

	TASK_CHECK(pcm != NULL);
	gen_pcm(sb, ch, frames, 0, pcm);
	TASK_CHECK(encode_frame(sb, ch, pcm, frames, &fr) == 0);
	input = make_input(&fr, NULL, 0, &total);
	TASK_CHECK(input != NULL);

	opt.input         = input;
	opt.total         = total;
	opt.frame_bytes   = fr.len;
	opt.sb            = sb;
	opt.ch            = ch;
	opt.sample_frames = frames;
	opt.expect        = pcm;
	opt.chunks        = NULL;
	opt.nchunks       = 0u;
	opt.caps          = caps;
	opt.ncaps         = 1u;
	opt.expect_ret    = 1;
	opt.use_final     = 1;
	opt.check_pcm     = 1;
	/* run_decode checks the sentinel region past every delivered frame */
	TASK_CHECK(run_decode(&opt, &st) == 0);
	free(input);
	free(fr.buf);
	free(pcm);
	return 0;
}

/* //////////////////////////////////////////////////////////////////////// */
/* FrameEnd                                                                 */
/* //////////////////////////////////////////////////////////////////////// */

static int
end_final_drain(void)
{
	const unsigned int sb = 2u, ch = 2u;
	const size_t frames = 64u;
	const size_t nsamples = frames * ch;
	int32_t *pcm = (int32_t *) calloc(nsamples, sizeof *pcm);
	struct enc_frame fr;
	static const size_t chunks[] = { 9u };
	static const size_t caps[] = { 1u };
	struct run_opts opt;
	struct run_stats st;
	uint8_t *input;
	size_t total;

	TASK_CHECK(pcm != NULL);
	gen_pcm(sb, ch, frames, 0, pcm);
	TASK_CHECK(encode_frame(sb, ch, pcm, frames, &fr) == 0);
	input = make_input(&fr, NULL, 0, &total);
	TASK_CHECK(input != NULL);

	opt.input         = input;
	opt.total         = total;
	opt.frame_bytes   = fr.len;
	opt.sb            = sb;
	opt.ch            = ch;
	opt.sample_frames = frames;
	opt.expect        = pcm;
	opt.chunks        = chunks;
	opt.nchunks       = 1u;
	opt.caps          = caps;
	opt.ncaps         = 1u;
	opt.expect_ret    = 1;
	opt.use_final     = 1;
	opt.check_pcm     = 1;
	/* the decoder repeatedly hits its one-frame output bound, which must
	   never be mistaken for a truncated frame */
	TASK_CHECK(run_decode(&opt, &st) == 0);
	TASK_CHECK(st.calls > frames / 2u);
	free(input);
	free(fr.buf);
	free(pcm);
	return 0;
}

static int
end_terminal_state(void)
{
	const unsigned int sb = 2u, ch = 1u;
	const size_t frames = 20u;
	int32_t *pcm = (int32_t *) calloc(frames, sizeof *pcm);
	struct enc_frame fr;
	const size_t ws_size = libttaR_streamdec_size(ch);
	uint8_t *ws_base = (uint8_t *) malloc(ws_size + 16u);
	uint8_t *ws;
	int32_t out[20];
	size_t b, f, i;
	int r;

	TASK_CHECK(pcm != NULL);
	gen_pcm(sb, ch, frames, 0, pcm);
	TASK_CHECK(encode_frame(sb, ch, pcm, frames, &fr) == 0);
	TASK_CHECK(ws_base != NULL);
	ws = (uint8_t *) ts_align(ws_base);
	TASK_CHECK(libttaR_streamdec_init(
		ws, ws_size, sb, ch, frames, fr.len
	) == 0);
	b = fr.len;
	f = frames;
	r = libttaR_streamdec_process(ws, fr.buf, &b, out, &f, 1);
	TASK_CHECK(r == 1);
	for ( i = 0; i < frames; ++i ){
		TASK_CHECK(out[i] == pcm[i]);
	}
	/* repeated calls after completion: 1 with both counts zero */
	for ( i = 0; i < 3u; ++i ){
		b = 7u;
		f = 5u;
		r = libttaR_streamdec_process(
			ws, fr.buf, &b, out, &f, 1
		);
		TASK_CHECK(r == 1);
		TASK_CHECK(b == 0u);
		TASK_CHECK(f == 0u);
	}

	/* a CRC mismatch is sticky: same error, both counts zero */
	{
		uint8_t *bad = (uint8_t *) malloc(fr.len);

		TASK_CHECK(bad != NULL);
		memcpy(bad, fr.buf, fr.len);
		bad[fr.len - 1u] ^= 0x01u;
		TASK_CHECK(libttaR_streamdec_init(
			ws, ws_size, sb, ch, frames, fr.len
		) == 0);
		b = fr.len;
		f = frames;
		r = libttaR_streamdec_process(ws, bad, &b, out, &f, 1);
		TASK_CHECK(r == -3);
		for ( i = 0; i < 3u; ++i ){
			b = 4u;
			f = 2u;
			r = libttaR_streamdec_process(ws, bad, &b, out, &f, 1);
			TASK_CHECK(r == -3);
			TASK_CHECK(b == 0u);
			TASK_CHECK(f == 0u);
		}
		free(bad);
	}
	free(fr.buf);
	free(ws_base);
	free(pcm);
	return 0;
}

static int
end_illegal_params(void)
{
	const size_t ws_size = libttaR_streamdec_size(2u);
	uint8_t *base = (uint8_t *) malloc(ws_size + 32u);
	uint8_t *ws;
	int32_t out[4];
	size_t b, f;

	TASK_CHECK(ws_size > 0);
	TASK_CHECK(ws_size <= 4096u * (2u + 1u));
	TASK_CHECK(base != NULL);
	ws = (uint8_t *) ts_align(base);

	/* invalid size queries */
	TASK_CHECK(libttaR_streamdec_size(0u) == 0u);
	TASK_CHECK(libttaR_streamdec_size(9u) == 0u);
	{
		unsigned int ch;

		for ( ch = 1; ch <= 8u; ++ch ){
			const size_t sz = libttaR_streamdec_size(ch);

			TASK_CHECK(sz > 0u);
			TASK_CHECK(sz <= 4096u * ((size_t) ch + 1u));
		}
	}

	/* invalid init parameters */
	TASK_CHECK(libttaR_streamdec_init(NULL, ws_size, 2u, 2u, 8u, 32u) == -1);
	TASK_CHECK(libttaR_streamdec_init(ws + 1u, ws_size, 2u, 2u, 8u, 32u) == -1);
	TASK_CHECK(libttaR_streamdec_init(ws, ws_size - 1u, 2u, 2u, 8u, 32u) == -1);
	TASK_CHECK(libttaR_streamdec_init(ws, ws_size, 0u, 2u, 8u, 32u) == -1);
	TASK_CHECK(libttaR_streamdec_init(ws, ws_size, 4u, 2u, 8u, 32u) == -1);
	TASK_CHECK(libttaR_streamdec_init(ws, ws_size, 2u, 0u, 8u, 32u) == -1);
	TASK_CHECK(libttaR_streamdec_init(ws, ws_size, 2u, 9u, 8u, 32u) == -1);
	TASK_CHECK(libttaR_streamdec_init(ws, ws_size, 2u, 2u, 0u, 32u) == -1);
	TASK_CHECK(libttaR_streamdec_init(ws, ws_size, 2u, 2u, 8u, 0u) == -1);
	TASK_CHECK(libttaR_streamdec_init(ws, ws_size, 2u, 2u, 8u, 3u) == -1);
	TASK_CHECK(libttaR_streamdec_init(
		ws, ws_size, 2u, 8u, SIZE_MAX / 2u, 32u
	) == -1);

	/* a good init, then illegal process parameters must not advance the
	   decoder and must zero both counts */
	TASK_CHECK(libttaR_streamdec_init(ws, ws_size, 2u, 2u, 8u, 32u) == 0);
	b = 4u;
	f = 4u;
	TASK_CHECK(libttaR_streamdec_process(NULL, NULL, &b, out, &f, 0) == -1);
	{
		uint8_t dummy[8] = { 0 };

		b = 4u;
		f = 4u;
		TASK_CHECK(libttaR_streamdec_process(
			ws, dummy, NULL, out, &f, 0
		) == -1);
		b = 4u;
		f = 4u;
		TASK_CHECK(libttaR_streamdec_process(
			ws, dummy, &b, out, NULL, 0
		) == -1);
		b = 4u;
		f = 4u;
		TASK_CHECK(libttaR_streamdec_process(
			ws, NULL, &b, out, &f, 0
		) == -1);
		TASK_CHECK(b == 0u);
		TASK_CHECK(f == 0u);
		b = 4u;
		f = 4u;
		TASK_CHECK(libttaR_streamdec_process(
			ws, dummy, &b, NULL, &f, 0
		) == -1);
		TASK_CHECK(b == 0u);
		TASK_CHECK(f == 0u);

		/* NULL is allowed when the corresponding count is zero */
		b = 0u;
		f = 0u;
		TASK_CHECK(libttaR_streamdec_process(
			ws, NULL, &b, NULL, &f, 0
		) == 0);
		TASK_CHECK(b == 0u);
		TASK_CHECK(f == 0u);

		/* and the decoder still works normally afterwards: the dummy
		   bytes are consumed (they are not enough to finish a frame) */
		b = 4u;
		f = 0u;
		TASK_CHECK(libttaR_streamdec_process(
			ws, dummy, &b, NULL, &f, 0
		) == 0);
		TASK_CHECK(b <= 4u);
	}
	free(base);
	return 0;
}

static int
end_uninitialized(void)
{
	uint8_t *raw = (uint8_t *) calloc(4096u + 16u, 1u);
	uint8_t *ws;
	size_t b = 0, f = 0;
	int32_t out[1];

	TASK_CHECK(raw != NULL);
	ws = (uint8_t *) ts_align(raw);
	TASK_CHECK(libttaR_streamdec_process(
		ws, NULL, &b, out, &f, 1
	) == -1);
	TASK_CHECK(b == 0u);
	TASK_CHECK(f == 0u);
	free(raw);
	return 0;
}

static int
end_final_truncation_reports_error(void)
{
	const unsigned int sb = 1u, ch = 2u;
	const size_t frames = 12u;
	const size_t nsamples = frames * ch;
	int32_t *pcm = (int32_t *) calloc(nsamples, sizeof *pcm);
	struct enc_frame fr;
	const size_t ws_size = libttaR_streamdec_size(ch);
	uint8_t *ws_base = (uint8_t *) malloc(ws_size + 16u);
	uint8_t *ws;
	int32_t out[nsamples];
	size_t b, f, i, half;
	int r;

	TASK_CHECK(pcm != NULL);
	gen_pcm(sb, ch, frames, 0, pcm);
	TASK_CHECK(encode_frame(sb, ch, pcm, frames, &fr) == 0);
	TASK_CHECK(ws_base != NULL);
	ws = (uint8_t *) ts_align(ws_base);
	TASK_CHECK(libttaR_streamdec_init(
		ws, ws_size, sb, ch, frames, fr.len
	) == 0);

	/* half the payload, then final */
	half = fr.len / 2u;
	b = half;
	f = nsamples;
	r = libttaR_streamdec_process(ws, fr.buf, &b, out, &f, 1);
	TASK_CHECK(r == -2);
	TASK_CHECK(b == half);
	/* frames actually delivered before the truncation matched */
	for ( i = 0; i < f; ++i ){
		TASK_CHECK(out[i] == pcm[i]);
	}

	/* re-initialization is required and is sufficient for a new frame */
	TASK_CHECK(libttaR_streamdec_init(
		ws, ws_size, sb, ch, frames, fr.len
	) == 0);
	b = fr.len;
	f = nsamples;
	r = libttaR_streamdec_process(ws, fr.buf, &b, out, &f, 1);
	TASK_CHECK(r == 1);
	for ( i = 0; i < nsamples; ++i ){
		TASK_CHECK(out[i] == pcm[i]);
	}

	free(fr.buf);
	free(ws_base);
	free(pcm);
	return 0;
}

/* //////////////////////////////////////////////////////////////////////// */
/* registration                                                             */
/* //////////////////////////////////////////////////////////////////////// */

const struct task_case task_cases[] = {
	{ "Fragments", "chunks_1_7_31_caps_1_3",	frag_chunks },
	{ "Fragments", "zero_output_capacity",		frag_zero_capacity },
	{ "Fragments", "long_unary_beyond_workspace",	frag_long_unary },
	{ "Fragments", "crc_split_across_calls",	frag_crc_split },
	{ "Fragments", "crc_mismatch",			frag_crc_mismatch },
	{ "Fragments", "payload_corruption",		frag_payload_corrupt },
	{ "Fragments", "truncated_frames",		frag_truncated },
	{ "Fragments", "short_frame_reset",		frag_short_frame_reset },
	{ "Fragments", "two_instances_interleaved",	frag_two_instances },
	{ "Fragments", "adjacent_frames_no_overconsume",	frag_adjacent_frames },

	{ "Channels",  "all_counts_1_to_8",		chan_all_counts },
	{ "Channels",  "group_incomplete_no_output",	chan_group_not_complete },
	{ "Channels",  "output_capacity_variants",	chan_capacity_variants },
	{ "Channels",  "randomized_differential",	chan_randomized_differential },
	{ "Channels",  "undelivered_sentinel",		chan_output_sentinel },

	{ "FrameEnd",  "final_drain_backpressure",	end_final_drain },
	{ "FrameEnd",  "terminal_state_repeat",		end_terminal_state },
	{ "FrameEnd",  "illegal_parameters",		end_illegal_params },
	{ "FrameEnd",  "uninitialized_workspace",	end_uninitialized },
	{ "FrameEnd",  "final_truncation_and_reset",	end_final_truncation_reports_error }
};

const size_t task_case_count = sizeof task_cases / sizeof task_cases[0];

/* EOF //////////////////////////////////////////////////////////////////// */
