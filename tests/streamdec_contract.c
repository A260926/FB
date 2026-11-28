/* ///////////////////////////////////////////////////////////////////////////
//                                                                          //
// streamdec_contract.c - contract tests for the streaming frame decoder    //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// Copyright (C) 2026, Shane Seelig                                         //
// SPDX-License-Identifier: GPL-3.0-or-later                                //
//                                                                          //
/////////////////////////////////////////////////////////////////////////// */

#include "task_support/check.h"

#include "src/libttaR.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* //////////////////////////////////////////////////////////////////////// */

#define SENTINEL	INT32_C(0x5A5A5A5A)
#define MAX_NCHAN	8u

struct test_frame {
	unsigned int	samplebytes;
	unsigned int	nchan;
	size_t		nframes;	/* per channel */
	size_t		nsamples;	/* total interleaved samples */
	size_t		payload;	/* compressed payload bytes */
	size_t		frame_bytes;	/* payload + 4 byte CRC */
	uint32_t	crc;
	int32_t		*pcm;
	uint8_t		*data;		/* frame_bytes + slack */
	size_t		data_cap;
};

/* ------------------------------------------------------------------------ */

static void *aligned16(void *p)
{
	return (void *) ((((uintptr_t) p) + 15u) & ~((uintptr_t) 15u));
}

/* deterministic, in-range PCM for the chosen bit depth */
static void gen_pcm(
	int32_t *pcm, size_t nsamples, unsigned nchan,
	unsigned samplebytes, unsigned seed
)
{
	size_t i;

	for ( i = 0; i < nsamples; ++i ){
		int32_t v = (int32_t) (
			((i * 7919u) + ((i % nchan) * 997u) + (seed * 131u))
			% 32749u
		) - 16374;
		switch ( samplebytes ){
		case 1u:
			/* keep inside the signed 8-bit range */
			pcm[i] = v % 121;
			break;
		case 2u:
			pcm[i] = v;
			break;
		default:
			pcm[i] = v * 257;
			break;
		}
	}
	return;
}

/* encode one frame with the real production encoder and append the LE CRC */
static int build_frame(
	struct test_frame *f, unsigned samplebytes, unsigned nchan,
	size_t nframes, unsigned seed
)
{
	size_t margin, cap, psz;
	void *raw_priv, *priv;
	struct LibTTAr_CodecState_User eu = LIBTTAr_CODECSTATE_USER_INIT;
	struct LibTTAr_EncMisc em;
	int er;

	f->samplebytes	= samplebytes;
	f->nchan	= nchan;
	f->nframes	= nframes;
	f->nsamples	= nframes * (size_t) nchan;
	f->pcm		= malloc(f->nsamples * sizeof *f->pcm);
	TASK_CHECK(f->pcm != NULL);
	gen_pcm(f->pcm, f->nsamples, nchan, samplebytes, seed);

	margin = libttaR_ttabuf_safety_margin(
		(enum LibTTAr_SampleBytes) samplebytes, nchan
	);
	TASK_CHECK(margin != 0);
	cap = (2u * margin) + (f->nsamples * 16u) + 32u;
	f->data_cap = cap + 16u;
	f->data = calloc(f->data_cap, 1u);
	TASK_CHECK(f->data != NULL);

	psz = libttaR_codecstate_priv_size(nchan);
	TASK_CHECK(psz != 0);
	raw_priv = calloc(psz + 16u, 1u);
	TASK_CHECK(raw_priv != NULL);
	priv = aligned16(raw_priv);

	em.dest_len	= cap;
	em.src_len	= f->nsamples;
	em.ni32_target	= f->nsamples;
	em.ni32_perframe = f->nsamples;
	em.samplebytes	= (enum LibTTAr_SampleBytes) samplebytes;
	em.nchan	= nchan;

	er = libttaR_tta_encode(f->data, f->pcm, priv, &eu, &em);
	TASK_CHECK(er == LIBTTAr_ERV_OK_DONE);
	TASK_CHECK(eu.ni32_total == f->nsamples);
	TASK_CHECK(eu.nbytes_tta_total != 0);

	f->payload	= eu.nbytes_tta_total;
	f->crc		= eu.crc;
	f->data[f->payload + 0u] = (uint8_t) (f->crc >>  0);
	f->data[f->payload + 1u] = (uint8_t) (f->crc >>  8);
	f->data[f->payload + 2u] = (uint8_t) (f->crc >> 16);
	f->data[f->payload + 3u] = (uint8_t) (f->crc >> 24);
	f->frame_bytes	= f->payload + 4u;

	free(raw_priv);
	return 0;
}

static void free_frame(struct test_frame *f)
{
	free(f->pcm);
	free(f->data);
	f->pcm = NULL;
	f->data = NULL;
	return;
}

/* full-buffer reference decode through the existing padded decoder API */
static int reference_decode(const struct test_frame *f, int32_t *out)
{
	size_t margin, psz;
	void *raw, *priv;
	struct LibTTAr_CodecState_User du = LIBTTAr_CODECSTATE_USER_INIT;
	struct LibTTAr_DecMisc dm;
	int dr;

	margin = libttaR_ttabuf_safety_margin(
		(enum LibTTAr_SampleBytes) f->samplebytes, f->nchan
	);
	TASK_CHECK(margin != 0);
	psz = libttaR_codecstate_priv_size(f->nchan);
	raw = calloc(psz + 16u, 1u);
	TASK_CHECK(raw != NULL);
	priv = aligned16(raw);

	dm.dest_len		= f->nsamples;
	dm.src_len		= f->payload + margin;
	dm.ni32_target		= f->nsamples;
	dm.nbytes_tta_target	= f->payload;
	dm.ni32_perframe	= f->nsamples;
	dm.nbytes_tta_perframe	= f->payload;
	dm.samplebytes		= (enum LibTTAr_SampleBytes) f->samplebytes;
	dm.nchan		= f->nchan;

	dr = libttaR_tta_decode(out, f->data, priv, &du, &dm);
	TASK_CHECK(dr == LIBTTAr_DRV_OK_DONE);
	TASK_CHECK(du.ni32_total == f->nsamples);
	TASK_CHECK(du.nbytes_tta_total == f->payload);
	TASK_CHECK(du.crc == f->crc);
	free(raw);
	return 0;
}

/* one-shot streaming decode of the whole frame */
static int full_stream_decode(const struct test_frame *f, int32_t *out)
{
	size_t wsz;
	void *raw, *ws;
	size_t b, fr;
	int rv;

	wsz = libttaR_streamdec_size(f->nchan);
	TASK_CHECK(wsz != 0);
	raw = calloc(wsz + 16u, 1u);
	TASK_CHECK(raw != NULL);
	ws = aligned16(raw);

	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, f->samplebytes, f->nchan, f->nframes, f->frame_bytes
	) == 0);
	b = f->frame_bytes;
	fr = f->nframes;
	rv = libttaR_streamdec_process(ws, f->data, &b, out, &fr, 1);
	TASK_CHECK(rv == 1);
	TASK_CHECK(b == f->frame_bytes);
	TASK_CHECK(fr == f->nframes);
	free(raw);
	return 0;
}

/* ------------------------------------------------------------------------ */

/* fragment-driven pump; records real consumption and delivered frames */
static int pump_fragments(
	const struct test_frame *f, size_t frag, size_t ocap,
	int32_t *out, size_t *consumed_out, size_t *frames_out
)
{
	size_t wsz;
	void *raw, *ws;
	size_t in_pos = 0, out_pos = 0;
	int rv = 0;

	wsz = libttaR_streamdec_size(f->nchan);
	TASK_CHECK(wsz != 0);
	raw = calloc(wsz + 16u, 1u);
	TASK_CHECK(raw != NULL);
	ws = aligned16(raw);

	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, f->samplebytes, f->nchan, f->nframes, f->frame_bytes
	) == 0);

	for ( ; ; ){
		size_t remaining = f->frame_bytes - in_pos;
		size_t offer = (frag < remaining) ? frag : remaining;
		size_t b = offer;
		size_t fr = ocap;
		int32_t *dp = (ocap != 0u) ? (out + (out_pos * f->nchan)) : NULL;
		int fin = (in_pos + offer >= f->frame_bytes);
		int r2;

		rv = libttaR_streamdec_process(
			ws, f->data + in_pos, &b, dp, &fr, fin
		);
		in_pos += b;
		out_pos += fr;
		if ( rv < 0 ){
			break;
		}

		/* zero output capacity: drain whatever was retained using
		     zero-input calls (backpressure must not be truncation) */
		if ( ocap == 0u ){
			for ( ; ; ){
				size_t zb = 0, zf = 1;
				r2 = libttaR_streamdec_process(
					ws, NULL, &zb,
					out + (out_pos * f->nchan), &zf, 0
				);
				out_pos += zf;
				if ( r2 < 0 ){
					rv = r2;
					break;
				}
				if ( zf == 0u ){
					break;
				}
				rv = r2;
			}
			if ( rv < 0 ){
				break;
			}
		}

		if ( rv == 1 ){
			break;
		}
		TASK_CHECK( (b != 0u) || (fr != 0u) || (ocap == 0u) );
		if ( (in_pos >= f->frame_bytes) && (out_pos >= f->nframes) ){
			break;
		}
	}

	*consumed_out = in_pos;
	*frames_out   = out_pos;
	free(raw);
	return rv < 0 ? (int) rv : 0;
}

/* //////////////////////////////////////////////////////////////////////// */
/* Fragments group                                                           */
/* //////////////////////////////////////////////////////////////////////// */

static int case_fragment_matrix(void)
{
	static const unsigned depths[] = {1u, 2u, 3u};
	static const unsigned chans[]  = {1u, 2u, 3u, 6u, 8u};
	static const size_t frags[]    = {1u, 7u, 31u};
	static const size_t ocaps[]    = {0u, 1u, 3u};
	size_t di, ci, fi, oi;

	for ( di = 0; di < 3u; ++di ){
		for ( ci = 0; ci < 5u; ++ci ){
			struct test_frame f;
			int32_t *streamed, *ref, *one_shot;
			size_t frag, ocap, consumed, got;
			int rv;

			TASK_CHECK(build_frame(&f, depths[di], chans[ci], 64u, 1u) == 0);
			streamed = malloc(f.nsamples * sizeof *streamed);
			ref      = malloc(f.nsamples * sizeof *ref);
			one_shot = malloc(f.nsamples * sizeof *one_shot);
			TASK_CHECK(streamed != NULL && ref != NULL && one_shot != NULL);

			TASK_CHECK(reference_decode(&f, ref) == 0);
			TASK_CHECK(full_stream_decode(&f, one_shot) == 0);
			TASK_CHECK(memcmp(ref, f.pcm, f.nsamples * 4u) == 0);
			TASK_CHECK(memcmp(one_shot, f.pcm, f.nsamples * 4u) == 0);

			for ( fi = 0; fi < 3u; ++fi ){
				for ( oi = 0; oi < 3u; ++oi ){
					frag = frags[fi];
					ocap = ocaps[oi];
					memset(streamed, 0x5A, f.nsamples * 4u);
					rv = pump_fragments(
						&f, frag, ocap, streamed,
						&consumed, &got
					);
					TASK_CHECK(rv == 0 || rv == 1);
					TASK_CHECK(got == f.nframes);
					TASK_CHECK(consumed == f.frame_bytes);
					TASK_CHECK(
						memcmp(streamed, f.pcm, f.nsamples * 4u) == 0
					);
				}
			}
			free(streamed);
			free(ref);
			free(one_shot);
			free_frame(&f);
		}
	}
	return 0;
}

/* low-amplitude long prefix followed by large impulses yields a unary run
   longer than the workspace; the decoder must pause inside it and resume */
static int case_long_unary_pause(void)
{
	struct test_frame f;
	size_t wsz, rawsz, run_len = 0, run_start = 0, cur = 0, cur_start = 0, i;
	void *raw, *ws;
	int32_t *out;
	size_t cut, b, fr, got;
	int rv;

	TASK_CHECK(build_frame(&f, 3u, 1u, 4000u, 7u) == 0);
	/* replace with silence then large, in-range impulses */
	for ( i = 0; i < f.nsamples; ++i ){
		f.pcm[i] = 0;
	}
	f.pcm[f.nframes - 40u] = 8000000;
	f.pcm[f.nframes - 30u] = -8000000;
	f.pcm[f.nframes - 20u] = 8000000;

	/* re-encode the crafted PCM */
	{
		size_t margin = libttaR_ttabuf_safety_margin(
			LIBTTAr_SAMPLEBYTES_3, 1u
		);
		size_t cap = (2u * margin) + (f.nsamples * 16u) + 32u;
		size_t psz = libttaR_codecstate_priv_size(1u);
		void *ep_raw = calloc(psz + 16u, 1u);
		struct LibTTAr_CodecState_User eu = LIBTTAr_CODECSTATE_USER_INIT;
		struct LibTTAr_EncMisc em = {
			cap, f.nsamples, f.nsamples, f.nsamples,
			LIBTTAr_SAMPLEBYTES_3, 1u
		};
		uint8_t *nb = calloc(cap + 16u, 1u);
		TASK_CHECK(ep_raw != NULL && nb != NULL);
		TASK_CHECK(libttaR_tta_encode(
			nb, f.pcm, aligned16(ep_raw), &eu, &em
		) == LIBTTAr_ERV_OK_DONE);
		free(f.data);
		f.payload = eu.nbytes_tta_total;
		f.crc = eu.crc;
		nb[f.payload + 0u] = (uint8_t) (f.crc >>  0);
		nb[f.payload + 1u] = (uint8_t) (f.crc >>  8);
		nb[f.payload + 2u] = (uint8_t) (f.crc >> 16);
		nb[f.payload + 3u] = (uint8_t) (f.crc >> 24);
		f.frame_bytes = f.payload + 4u;
		f.data = nb;
		f.data_cap = cap + 16u;
		free(ep_raw);
	}

	/* locate the longest 0xFF run in the payload */
	for ( i = 0; i < f.payload; ++i ){
		if ( f.data[i] == 0xFFu ){
			if ( cur == 0u ){
				cur_start = i;
			}
			cur++;
			if ( cur > run_len ){
				run_len = cur;
				run_start = cur_start;
			}
		}
		else {
			cur = 0;
		}
	}
	wsz = libttaR_streamdec_size(1u);
	TASK_CHECK(run_len > wsz);	/* a unary longer than the workspace */

	rawsz = wsz + 16u;
	raw = calloc(rawsz, 1u);
	ws = aligned16(raw);
	out = malloc(f.nsamples * sizeof *out);
	TASK_CHECK(raw != NULL && out != NULL);
	memset(out, 0x5A, f.nsamples * 4u);
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 3u, 1u, f.nframes, f.frame_bytes
	) == 0);

	/* cut strictly inside the long run: must pause mid-unary */
	cut = run_start + (run_len / 2u);
	b = cut;
	fr = f.nframes;
	rv = libttaR_streamdec_process(ws, f.data, &b, out, &fr, 0);
	got = fr;
	TASK_CHECK(rv == 0);		/* paused, not finished or errored */
	TASK_CHECK(b == cut);		/* whole slice consumed */
	TASK_CHECK(got < f.nframes);	/* frame not complete mid-unary */

	/* resume with the remainder; the half unary resumes correctly */
	b = f.frame_bytes - cut;
	fr = f.nframes - got;
	rv = libttaR_streamdec_process(
		ws, f.data + cut, &b, out + got, &fr, 1
	);
	TASK_CHECK(rv == 1);
	TASK_CHECK(b == f.frame_bytes - cut);
	TASK_CHECK(got + fr == f.nframes);
	TASK_CHECK(memcmp(out, f.pcm, f.nsamples * 4u) == 0);

	free(raw);
	free(out);
	free_frame(&f);
	return 0;
}

/* the trailing CRC arriving one byte at a time must still verify */
static int case_crc_split(void)
{
	struct test_frame f;
	int32_t *out;
	size_t wsz, i, got = 0, consumed = 0;
	void *raw, *ws;
	int rv = 0;

	TASK_CHECK(build_frame(&f, 2u, 2u, 48u, 3u) == 0);
	wsz = libttaR_streamdec_size(2u);
	raw = calloc(wsz + 16u, 1u);
	ws = aligned16(raw);
	out = malloc(f.nsamples * sizeof *out);
	TASK_CHECK(raw != NULL && out != NULL);
	memset(out, 0x5A, f.nsamples * 4u);
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 2u, 2u, f.nframes, f.frame_bytes
	) == 0);

	/* payload in 7-byte slices, then each CRC byte separately */
	for ( i = 0; i < f.payload; ){
		size_t step = (f.payload - i < 7u) ? (f.payload - i) : 7u;
		size_t b = step, fr = 2u;
		rv = libttaR_streamdec_process(
			ws, f.data + i, &b, out + (got * 2u), &fr, 0
		);
		i += b;
		got += fr;
		TASK_CHECK(rv >= 0);
	}
	for ( i = f.payload; i < f.frame_bytes; ++i ){
		size_t b = 1u, fr = 2u;
		rv = libttaR_streamdec_process(
			ws, f.data + i, &b, out + (got * 2u), &fr,
			(i + 1u >= f.frame_bytes)
		);
		got += fr;
		TASK_CHECK(b == 1u);
	}
	TASK_CHECK(rv == 1);
	TASK_CHECK(got == f.nframes);
	TASK_CHECK(memcmp(out, f.pcm, f.nsamples * 4u) == 0);
	(void) consumed;

	free(raw);
	free(out);
	free_frame(&f);
	return 0;
}

/* //////////////////////////////////////////////////////////////////////// */
/* Channels group                                                            */
/* //////////////////////////////////////////////////////////////////////// */

static int case_channel_counts(void)
{
	static const unsigned chans[] = {1u, 2u, 3u, 6u, 8u};
	size_t ci;

	for ( ci = 0; ci < 5u; ++ci ){
		struct test_frame f;
		int32_t *out;
		size_t consumed, got;
		int rv;

		TASK_CHECK(build_frame(&f, 2u, chans[ci], 72u, ci + 4u) == 0);
		out = malloc(f.nsamples * sizeof *out);
		TASK_CHECK(out != NULL);
		memset(out, 0x5A, f.nsamples * 4u);
		rv = pump_fragments(&f, 7u, 1u, out, &consumed, &got);
		TASK_CHECK((rv == 0) || (rv == 1));
		TASK_CHECK(got == f.nframes);
		TASK_CHECK(consumed == f.frame_bytes);
		TASK_CHECK(memcmp(out, f.pcm, f.nsamples * 4u) == 0);
		free(out);
		free_frame(&f);
	}
	return 0;
}

/* an incomplete channel group must not produce any sample frame */
static int case_group_incomplete_no_output(void)
{
	struct test_frame f;
	size_t wsz;
	void *raw, *ws;
	int32_t *out;
	size_t b, fr;
	int rv;

	TASK_CHECK(build_frame(&f, 1u, 6u, 40u, 9u) == 0);
	wsz = libttaR_streamdec_size(6u);
	raw = calloc(wsz + 16u, 1u);
	ws = aligned16(raw);
	out = malloc(f.nsamples * sizeof *out);
	TASK_CHECK(raw != NULL && out != NULL);
	memset(out, 0x5A, f.nsamples * 4u);
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 1u, 6u, f.nframes, f.frame_bytes
	) == 0);

	/* a single byte cannot complete a six-channel group */
	b = 1u;
	fr = f.nframes;
	rv = libttaR_streamdec_process(ws, f.data, &b, out, &fr, 0);
	TASK_CHECK(rv == 0);
	TASK_CHECK(fr == 0u);
	TASK_CHECK(out[0] == SENTINEL);	/* nothing written */

	free(raw);
	free(out);
	free_frame(&f);
	return 0;
}

/* two independent decoders advanced interleaved must not cross-contaminate */
static int case_two_instances_interleaved(void)
{
	struct test_frame a, bframe;
	int32_t *oa, *ob;
	size_t wsza, wszb, pa = 0, pb = 0, ga = 0, gb = 0;
	void *ra, *rb, *wsa, *wsb;
	int rva = 0, rvb = 0;

	TASK_CHECK(build_frame(&a, 2u, 2u, 50u, 11u) == 0);
	TASK_CHECK(build_frame(&bframe, 3u, 3u, 50u, 22u) == 0);
	wsza = libttaR_streamdec_size(2u);
	wszb = libttaR_streamdec_size(3u);
	ra = calloc(wsza + 16u, 1u);
	rb = calloc(wszb + 16u, 1u);
	wsa = aligned16(ra);
	wsb = aligned16(rb);
	oa = malloc(a.nsamples * sizeof *oa);
	ob = malloc(bframe.nsamples * sizeof *ob);
	TASK_CHECK(ra != NULL && rb != NULL && oa != NULL && ob != NULL);
	memset(oa, 0x5A, a.nsamples * 4u);
	memset(ob, 0x5A, bframe.nsamples * 4u);
	TASK_CHECK(libttaR_streamdec_init(
		wsa, wsza, 2u, 2u, a.nframes, a.frame_bytes
	) == 0);
	TASK_CHECK(libttaR_streamdec_init(
		wsb, wszb, 3u, 3u, bframe.nframes, bframe.frame_bytes
	) == 0);

	while ( (pa < a.frame_bytes) || (pb < bframe.frame_bytes) ){
		if ( pa < a.frame_bytes ){
			size_t n = 7u, fr = 2u;
			if ( n > a.frame_bytes - pa ){ n = a.frame_bytes - pa; }
			rva = libttaR_streamdec_process(
				wsa, a.data + pa, &n, oa + (ga * 2u), &fr,
				(pa + n >= a.frame_bytes)
			);
			pa += n;
			ga += fr;
		}
		if ( pb < bframe.frame_bytes ){
			size_t n = 3u, fr = 2u;
			if ( n > bframe.frame_bytes - pb ){
				n = bframe.frame_bytes - pb;
			}
			rvb = libttaR_streamdec_process(
				wsb, bframe.data + pb, &n, ob + (gb * 3u), &fr,
				(pb + n >= bframe.frame_bytes)
			);
			pb += n;
			gb += fr;
		}
	}
	TASK_CHECK(rva == 1);
	TASK_CHECK(rvb == 1);
	TASK_CHECK(ga == a.nframes && pa == a.frame_bytes);
	TASK_CHECK(gb == bframe.nframes && pb == bframe.frame_bytes);
	TASK_CHECK(memcmp(oa, a.pcm, a.nsamples * 4u) == 0);
	TASK_CHECK(memcmp(ob, bframe.pcm, bframe.nsamples * 4u) == 0);

	free(ra);
	free(rb);
	free(oa);
	free(ob);
	free_frame(&a);
	free_frame(&bframe);
	return 0;
}

/* //////////////////////////////////////////////////////////////////////// */
/* FrameEnd group                                                            */
/* //////////////////////////////////////////////////////////////////////// */

/* bytes belonging to the next frame must never be consumed */
static int case_adjacent_frame_boundary(void)
{
	struct test_frame f;
	static const uint8_t slack[8u] = {
		0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u, 0x88u
	};
	size_t wsz;
	void *raw, *ws;
	int32_t *out;
	size_t b, fr, i;
	int rv;

	TASK_CHECK(build_frame(&f, 2u, 2u, 40u, 5u) == 0);
	TASK_CHECK(f.data_cap >= f.frame_bytes + sizeof slack);
	memcpy(f.data + f.frame_bytes, slack, sizeof slack);
	wsz = libttaR_streamdec_size(2u);
	raw = calloc(wsz + 16u, 1u);
	ws = aligned16(raw);
	out = malloc(f.nsamples * sizeof *out);
	TASK_CHECK(raw != NULL && out != NULL);
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 2u, 2u, f.nframes, f.frame_bytes
	) == 0);

	b = f.frame_bytes + sizeof slack;	/* offer more than the frame */
	fr = f.nframes;
	rv = libttaR_streamdec_process(ws, f.data, &b, out, &fr, 1);
	TASK_CHECK(rv == 1);
	TASK_CHECK(b == f.frame_bytes);		/* exact frame consumption */
	TASK_CHECK(fr == f.nframes);
	for ( i = 0; i < sizeof slack; ++i ){
		TASK_CHECK(f.data[f.frame_bytes + i] == slack[i]);
	}
	TASK_CHECK(memcmp(out, f.pcm, f.nsamples * 4u) == 0);

	free(raw);
	free(out);
	free_frame(&f);
	return 0;
}

/* a corrupt CRC must report -3 while a corrupt payload also errors */
static int case_crc_mismatch(void)
{
	struct test_frame f;
	size_t wsz;
	void *raw, *ws;
	int32_t *out;
	size_t b, fr;
	int rv;

	TASK_CHECK(build_frame(&f, 2u, 1u, 40u, 6u) == 0);
	wsz = libttaR_streamdec_size(1u);
	raw = calloc(wsz + 16u, 1u);
	ws = aligned16(raw);
	out = malloc(f.nsamples * sizeof *out);
	TASK_CHECK(raw != NULL && out != NULL);

	/* flip the last CRC byte */
	f.data[f.frame_bytes - 1u] ^= 0x01u;
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 2u, 1u, f.nframes, f.frame_bytes
	) == 0);
	b = f.frame_bytes;
	fr = f.nframes;
	rv = libttaR_streamdec_process(ws, f.data, &b, out, &fr, 1);
	TASK_CHECK(rv == -3);
	/* sticky: repeat keeps the error with zero counters */
	b = 0;
	fr = 0;
	rv = libttaR_streamdec_process(ws, f.data, &b, out, &fr, 1);
	TASK_CHECK(rv == -3 && b == 0u && fr == 0u);

	/* flip a payload byte instead: must error (-3 CRC or -4 stream) */
	f.data[f.frame_bytes - 1u] ^= 0x01u;	/* restore CRC */
	f.data[f.payload / 2u] ^= 0x80u;
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 2u, 1u, f.nframes, f.frame_bytes
	) == 0);
	b = f.frame_bytes;
	fr = f.nframes;
	rv = libttaR_streamdec_process(ws, f.data, &b, out, &fr, 1);
	TASK_CHECK(rv == -3 || rv == -4);

	free(raw);
	free(out);
	free_frame(&f);
	return 0;
}

/* genuine end-of-input before the frame completes is -2, not backpressure */
static int case_real_truncation(void)
{
	struct test_frame f;
	size_t wsz, cut;
	void *raw, *ws;
	int32_t *out;
	size_t b, fr;
	int rv;

	TASK_CHECK(build_frame(&f, 2u, 2u, 50u, 8u) == 0);
	cut = f.frame_bytes / 2u;
	wsz = libttaR_streamdec_size(2u);
	raw = calloc(wsz + 16u, 1u);
	ws = aligned16(raw);
	out = malloc(f.nsamples * sizeof *out);
	TASK_CHECK(raw != NULL && out != NULL);
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 2u, 2u, f.nframes, f.frame_bytes
	) == 0);

	b = cut;
	fr = f.nframes;	/* ample room: the stall is lack of input */
	rv = libttaR_streamdec_process(ws, f.data, &b, out, &fr, 1);
	TASK_CHECK(rv == -2);
	TASK_CHECK(b == cut);

	free(raw);
	free(out);
	free_frame(&f);
	return 0;
}

/* after a failed short frame, reinitializing the same workspace recovers */
static int case_short_frame_reset(void)
{
	struct test_frame f;
	size_t wsz;
	void *raw, *ws;
	int32_t *out;
	size_t b, fr;
	int rv;

	TASK_CHECK(build_frame(&f, 1u, 2u, 36u, 10u) == 0);
	wsz = libttaR_streamdec_size(2u);
	raw = calloc(wsz + 16u, 1u);
	ws = aligned16(raw);
	out = malloc(f.nsamples * sizeof *out);
	TASK_CHECK(raw != NULL && out != NULL);

	/* truncated attempt */
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 1u, 2u, f.nframes, f.frame_bytes
	) == 0);
	b = f.frame_bytes / 3u;
	fr = f.nframes;
	rv = libttaR_streamdec_process(ws, f.data, &b, out, &fr, 1);
	TASK_CHECK(rv == -2);

	/* reset and decode the full frame successfully */
	memset(out, 0x5A, f.nsamples * 4u);
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 1u, 2u, f.nframes, f.frame_bytes
	) == 0);
	b = f.frame_bytes;
	fr = f.nframes;
	rv = libttaR_streamdec_process(ws, f.data, &b, out, &fr, 1);
	TASK_CHECK(rv == 1);
	TASK_CHECK(b == f.frame_bytes && fr == f.nframes);
	TASK_CHECK(memcmp(out, f.pcm, f.nsamples * 4u) == 0);

	free(raw);
	free(out);
	free_frame(&f);
	return 0;
}

/* feed under zero output capacity, then drain entirely with zero input */
static int case_final_drain(void)
{
	struct test_frame f;
	size_t wsz, pos = 0, got = 0;
	void *raw, *ws;
	int32_t *out;
	int rv = 0;

	TASK_CHECK(build_frame(&f, 3u, 2u, 48u, 12u) == 0);
	wsz = libttaR_streamdec_size(2u);
	raw = calloc(wsz + 16u, 1u);
	ws = aligned16(raw);
	out = malloc(f.nsamples * sizeof *out);
	TASK_CHECK(raw != NULL && out != NULL);
	memset(out, 0x5A, f.nsamples * 4u);
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 3u, 2u, f.nframes, f.frame_bytes
	) == 0);

	/* feed small input slices with no writable frames */
	while ( pos < f.frame_bytes ){
		size_t offer = 7u, b, fr = 0;
		if ( offer > f.frame_bytes - pos ){ offer = f.frame_bytes - pos; }
		b = offer;
		rv = libttaR_streamdec_process(
			ws, f.data + pos, &b, NULL, &fr,
			(pos + b >= f.frame_bytes)
		);
		pos += b;
		TASK_CHECK(fr == 0u);
		TASK_CHECK(rv >= 0);
		/* drain whatever is retained, with zero input */
		for ( ; ; ){
			size_t zb = 0, zf = 1;
			int r2 = libttaR_streamdec_process(
				ws, NULL, &zb, out + (got * 2u), &zf, 0
			);
			got += zf;
			TASK_CHECK(zb == 0u);
			if ( r2 < 0 ){
				free(raw);
				free(out);
				free_frame(&f);
				TASK_CHECK(0);
			}
			if ( zf == 0u ){
				break;
			}
			rv = r2;
		}
	}
	TASK_CHECK(rv == 1);
	TASK_CHECK(got == f.nframes);
	TASK_CHECK(memcmp(out, f.pcm, f.nsamples * 4u) == 0);

	/* post-completion repeat: 1 with both counters zero */
	{
		size_t zb = 0, zf = 0;
		int r2 = libttaR_streamdec_process(
			ws, NULL, &zb, out, &zf, 1
		);
		TASK_CHECK(r2 == 1 && zb == 0u && zf == 0u);
	}

	free(raw);
	free(out);
	free_frame(&f);
	return 0;
}

/* size query, parameter validation, and the no-advance illegal-call rule */
static int case_limits_and_params(void)
{
	struct test_frame f;
	size_t wsz, ch;
	void *raw, *ws, *mis_raw, *mis;
	int32_t *out;
	size_t b, fr;

	TASK_CHECK(libttaR_streamdec_size(0u) == 0u);
	TASK_CHECK(libttaR_streamdec_size(9u) == 0u);
	for ( ch = 1u; ch <= MAX_NCHAN; ++ch ){
		wsz = libttaR_streamdec_size((unsigned) ch);
		TASK_CHECK(wsz != 0u);
		TASK_CHECK(wsz <= (4096u * (ch + 1u)));
		TASK_CHECK((wsz % 16u) == 0u);
	}

	TASK_CHECK(build_frame(&f, 2u, 2u, 32u, 13u) == 0);
	wsz = libttaR_streamdec_size(2u);
	raw = calloc(wsz + 16u, 1u);
	ws = aligned16(raw);
	out = malloc(f.nsamples * sizeof *out);
	TASK_CHECK(raw != NULL && out != NULL);

	TASK_CHECK(libttaR_streamdec_init(
		NULL, wsz, 2u, 2u, f.nframes, f.frame_bytes
	) == -1);
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 0u, 2u, f.nframes, f.frame_bytes
	) == -1);
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 4u, 2u, f.nframes, f.frame_bytes
	) == -1);
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 2u, 0u, f.nframes, f.frame_bytes
	) == -1);
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 2u, 9u, f.nframes, f.frame_bytes
	) == -1);
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 2u, 2u, 0u, f.frame_bytes
	) == -1);
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 2u, 2u, f.nframes, 4u
	) == -1);
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz - 1u, 2u, 2u, f.nframes, f.frame_bytes
	) == -1);

	mis_raw = calloc(wsz + 16u, 1u);
	mis = (void *) ((char *) mis_raw + 1u);
	TASK_CHECK(libttaR_streamdec_init(
		mis, wsz, 2u, 2u, f.nframes, f.frame_bytes
	) == -1);
	free(mis_raw);

	/* a valid init followed by an illegal process call */
	TASK_CHECK(libttaR_streamdec_init(
		ws, wsz, 2u, 2u, f.nframes, f.frame_bytes
	) == 0);
	b = f.frame_bytes;
	fr = f.nframes;
	TASK_CHECK(libttaR_streamdec_process(
		ws, f.data, &b, out, &fr, 7
	) == -1);
	TASK_CHECK(b == 0u && fr == 0u);	/* no advance, counters zeroed */
	TASK_CHECK(libttaR_streamdec_process(
		NULL, f.data, &b, out, &fr, 1
	) == -1);
	TASK_CHECK(b == 0u && fr == 0u);

	free(raw);
	free(out);
	free_frame(&f);
	return 0;
}

/* //////////////////////////////////////////////////////////////////////// */

const struct task_case task_cases[] = {
	{ "Fragments", "fragment_matrix",		case_fragment_matrix },
	{ "Fragments", "long_unary_pause",		case_long_unary_pause },
	{ "Fragments", "crc_split",			case_crc_split },

	{ "Channels", "channel_counts",		case_channel_counts },
	{ "Channels", "group_incomplete_no_output",	case_group_incomplete_no_output },
	{ "Channels", "two_instances_interleaved",	case_two_instances_interleaved },

	{ "FrameEnd", "adjacent_frame_boundary",	case_adjacent_frame_boundary },
	{ "FrameEnd", "crc_mismatch",			case_crc_mismatch },
	{ "FrameEnd", "real_truncation",		case_real_truncation },
	{ "FrameEnd", "short_frame_reset",		case_short_frame_reset },
	{ "FrameEnd", "final_drain",			case_final_drain },
	{ "FrameEnd", "limits_and_params",		case_limits_and_params }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

/* EOF //////////////////////////////////////////////////////////////////// */
