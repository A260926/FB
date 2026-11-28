/* ///////////////////////////////////////////////////////////////////////////
//                                                                          //
// stream_dec.c - streaming single-frame TTA decoder                        //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// Copyright (C) 2026, Shane Seelig                                         //
// SPDX-License-Identifier: GPL-3.0-or-later                                //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// A reentrant, byte-at-a-time streaming decoder for one compressed TTA     //
// audio frame.  Unlike libttaR_tta_decode() it never asks the caller for a //
// safety margin: every input slice is consumed in place and the partial    //
// progress of a code word split across a slice is kept inside the caller   //
// supplied workspace.  No heap memory is used and the workspace size is    //
// independent of frame_bytes, sample_frames, and the code word length.     //
//                                                                          //
/////////////////////////////////////////////////////////////////////////// */

#include <stddef.h>
#include <stdint.h>

#include "./common.h"
#include "./crc32.h"
#include "./filter.h"
#include "./overflow.h"
#include "./rice24.h"
#include "./tta.h"
#include "./types.h"

/* //////////////////////////////////////////////////////////////////////// */

#define STREAMDEC_MAGIC		UINT32_C(0x53544431)	/* "STD1" */
#define STREAMDEC_MAX_NCHAN	((unsigned int) 8u)
#define STREAMDEC_SIZE_LIMIT	((size_t) 4096u)
#define STREAMDEC_CRC_NBYTES	((size_t) 4u)

enum StreamDec_Phase {
	STREAMDEC_PHASE_UNARY	= 0u,
	STREAMDEC_PHASE_BINARY	= 1u,
	STREAMDEC_PHASE_CRC	= 2u
};

/* ------------------------------------------------------------------------ */

struct libttaR_streamdec {
	uint32_t			magic;

	/* frame declaration */
	unsigned int			nchan;
	unsigned int			samplebytes;
	size_t				sample_frames;	/* per channel */
	size_t				frame_bytes;	/* incl. CRC */
	size_t				payload_bytes;	/* excl. CRC */

	/* codec parameters */
	bitcnt				predict_k;
	int32_t				filter_round;
	bitcnt				filter_k;
	rice24_dec			unary_lax_limit;

	/* progress */
	size_t				src_total;	/* payload bytes fed */
	size_t				frame_out;	/* groups decoded */
	uint32_t			crc;		/* running payload CRC */

	/* bitcache: 0..7 valid bits between code words */
	cache32				cache;
	bitcnt_dec			count;

	/* symbol state machine */
	unsigned int			phase;
	unsigned int			chan;
	rice24_dec			unary_acc;	/* running 1-bit count */
	rice24_dec			unary_value;	/* completed unary */
	bitcnt_dec			bin_k;
	int				unary_long;	/* mid-unary across a cut */

	/* per-channel codec history */
	struct Codec			codec[STREAMDEC_MAX_NCHAN];

	/* the one channel group assembled ahead of the output writer */
	int32_t				frame[STREAMDEC_MAX_NCHAN];
	int				pending;	/* group awaits output */

	/* trailing frame CRC */
	size_t				crc_got;
	uint8_t				crc_buf[STREAMDEC_CRC_NBYTES];

	/* end-of-stream / sticky terminal status */
	int				final_locked;
	int				status;		/* 0, 1, or negative */
};

/* //////////////////////////////////////////////////////////////////////// */

/*@observer@*/
static struct libttaR_streamdec *streamdec_check(void *) /*@*/;

static int streamdec_take_payload(
	struct libttaR_streamdec *RESTRICT, const uint8_t *RESTRICT,
	size_t, size_t
)
/*@modifies *s@*/
;

static void streamdec_write_group(
	const struct libttaR_streamdec *RESTRICT,
	int32_t *RESTRICT, size_t
)
/*@modifies *dst@*/
;

/* ======================================================================== */

/**@fn libttaR_streamdec_size
 * @brief size of the workspace needed by the streaming single-frame decoder
 *
 * @param nchan - number of audio channels (1..8)
 *
 * @return workspace size in bytes
 * @retval 0 - bad 'nchan'
**/
BUILD_EXPORT
size_t
libttaR_streamdec_size(const unsigned int nchan)
/*@*/
{
	size_t retval;
	size_t mod;

	if ( (nchan == 0u) || (nchan > STREAMDEC_MAX_NCHAN) ){
		return 0;
	}

	retval = sizeof(struct libttaR_streamdec);
	mod    = retval % LIBTTAr_CODECSTATE_PRIV_ALIGN;
	if ( mod != 0 ){
		retval += LIBTTAr_CODECSTATE_PRIV_ALIGN - mod;
	}

	/* contract: a legal size must stay within the per-channel bound */
	if ( retval > (STREAMDEC_SIZE_LIMIT * ((size_t) nchan + 1u)) ){
		return 0;
	}
	return retval;
}

/**@fn libttaR_streamdec_init
 * @brief validate and (re)initialize a streaming decoder workspace
 *
 * @param workspace       - 16-byte aligned caller supplied workspace
 * @param workspace_bytes - size returned by libttaR_streamdec_size()
 * @param samplebytes     - 1, 2, or 3
 * @param nchan           - 1..8 audio channels
 * @param sample_frames   - positive number of per-channel sample frames
 * @param frame_bytes     - seektable frame size, including the 4-byte CRC
 *
 * @return 0 on success, -1 on any bad parameter/length/overflow
**/
BUILD_EXPORT
int
libttaR_streamdec_init(
	void *const workspace, const size_t workspace_bytes,
	const unsigned int samplebytes, const unsigned int nchan,
	const size_t sample_frames, const size_t frame_bytes
)
/*@modifies *workspace@*/
{
	struct libttaR_streamdec *s;
	size_t need, payload, total_samples, payload_bits, temp;
	int overflow_0, overflow_1;

	if ( workspace == NULL ){
		return -1;
	}
	if ( ((uintptr_t) workspace) % LIBTTAr_CODECSTATE_PRIV_ALIGN != 0u ){
		return -1;
	}
	if ( (nchan < 1u) || (nchan > STREAMDEC_MAX_NCHAN) ){
		return -1;
	}
	if ( (samplebytes < 1u)
	    || (samplebytes > (unsigned int) LIBTTAr_SAMPLEBYTES_MAX)
	){
		return -1;
	}
	if ( sample_frames == 0u ){
		return -1;
	}
	/* one payload byte plus the four CRC bytes */
	if ( frame_bytes < (STREAMDEC_CRC_NBYTES + 1u) ){
		return -1;
	}

	need = libttaR_streamdec_size(nchan);
	if ( (need == 0u) || (workspace_bytes < need) ){
		return -1;
	}

	payload = frame_bytes - STREAMDEC_CRC_NBYTES;

	/* every symbol needs at least one bit, so the payload cannot hold more
	     samples than it has bits */
	overflow_0 = mul_usize_overflow(
		&total_samples, sample_frames, (size_t) nchan
	);
	overflow_1 = mul_usize_overflow(&payload_bits, payload, (size_t) 8u);
	if ( (overflow_0 != 0) || (overflow_1 != 0)
	    || (total_samples > payload_bits)
	){
		return -1;
	}
	/* defensive overflow check on the output frame indexing math */
	overflow_0 = mul_usize_overflow(&temp, sample_frames, (size_t) nchan);
	if ( overflow_0 != 0 ){
		return -1;
	}

	MEMSET(workspace, 0x00, need);
	s = (struct libttaR_streamdec *) workspace;

	s->magic		= STREAMDEC_MAGIC;
	s->nchan		= nchan;
	s->samplebytes		= samplebytes;
	s->sample_frames	= sample_frames;
	s->frame_bytes		= frame_bytes;
	s->payload_bytes	= payload;

	s->predict_k		= get_predict_k(
		(enum LibTTAr_SampleBytes) samplebytes
	);
	s->filter_round		= get_filter_round(
		(enum LibTTAr_SampleBytes) samplebytes
	);
	s->filter_k		= get_filter_k(
		(enum LibTTAr_SampleBytes) samplebytes
	);
	s->unary_lax_limit	= get_unary_lax_limit(
		(enum LibTTAr_SampleBytes) samplebytes
	);

	s->crc			= CRC32_INIT;
	s->phase		= STREAMDEC_PHASE_UNARY;
	s->status		= 0;

	codec_init_dec(s->codec, nchan);
	return 0;
}

/* ======================================================================== */

/* validate the workspace without touching it */
static struct libttaR_streamdec *
streamdec_check(void *const workspace)
/*@*/
{
	struct libttaR_streamdec *s = (struct libttaR_streamdec *) workspace;

	if ( workspace == NULL ){
		return NULL;
	}
	if ( ((uintptr_t) workspace) % LIBTTAr_CODECSTATE_PRIV_ALIGN != 0u ){
		return NULL;
	}
	if ( s->magic != STREAMDEC_MAGIC ){
		return NULL;
	}
	if ( (s->nchan < 1u) || (s->nchan > STREAMDEC_MAX_NCHAN) ){
		return NULL;
	}
	return s;
}

/**@fn streamdec_take_payload
 * @brief consume one payload byte into the bitcache and CRC
 *
 * @return  1 - a byte was consumed
 * @return  0 - no input byte in this slice (fragment boundary; wait)
 * @return -1 - the declared payload is already exhausted (stream error)
**/
static int
streamdec_take_payload(
	struct libttaR_streamdec *const RESTRICT s,
	const uint8_t *const RESTRICT src, const size_t in_used,
	const size_t in_avail
)
/*@modifies *s@*/
{
	uint8_t inbyte;

	if ( s->src_total >= s->payload_bytes ){
		return -1;
	}
	if ( in_used >= in_avail ){
		return 0;
	}
	inbyte = src[in_used];
	s->crc    = crc32_cont_dec(inbyte, s->crc);
	s->cache |= ((cache32) inbyte) << s->count;
	s->count += (bitcnt_dec) 8u;
	s->src_total++;
	return 1;
}

/**@fn streamdec_write_group
 * @brief inverse channel decorrelation for one retained sample frame
 *
 * Writes nchan interleaved int32 values starting at dst[out_idx * nchan].
 * Mirrors the backwards decorrelation pass of tta_decode_mch_loop().
**/
static void
streamdec_write_group(
	const struct libttaR_streamdec *const RESTRICT s,
	int32_t *const RESTRICT dst, const size_t out_idx
)
/*@modifies *dst@*/
{
	const unsigned int nchan = s->nchan;
	int32_t prev = 0;
	int32_t curr;
	unsigned int j;

	for ( j = 0u; j + 1u < nchan; ++j ){
		prev = s->frame[j];
	}
	curr = s->frame[nchan - 1u] + (prev / 2);
	dst[(out_idx * nchan) + (nchan - 1u)] = curr;
	for ( j = nchan; j-- > 1u; ){
		curr -= s->frame[j - 1u];
		dst[(out_idx * nchan) + (j - 1u)] = curr;
	}
	return;
}

/* ======================================================================== */

/**@fn libttaR_streamdec_process
 * @brief stream-decode one TTA frame from arbitrary byte fragments
 *
 * @param workspace - initialized streaming decoder workspace
 * @param src       - compressed bytes (may start mid-code-word)
 * @param bytes     - in: readable bytes; out: bytes consumed
 * @param dst       - interleaved int32 PCM destination (NULL if *frames == 0)
 * @param frames    - in: writable frame capacity; out: frames delivered
 * @param final     - 1 iff no bytes will ever follow this slice
 *
 * @return  0 - progress made, or waiting on input/output capacity
 * @return  1 - frame complete and the CRC matched
 * @return -1 - bad parameter
 * @return -2 - input ended before the frame was complete
 * @return -3 - CRC mismatch
 * @return -4 - another bitstream or declared-length error
**/
BUILD_EXPORT
int
libttaR_streamdec_process(
	void *const workspace,
	const uint8_t *const RESTRICT src, size_t *const bytes,
	int32_t *const dst, size_t *const frames, const int final
)
/*@modifies *workspace, *bytes, *frames@*/
{
	struct libttaR_streamdec *s;
	size_t in_avail, out_cap;
	size_t in_used = 0;
	size_t out_n = 0;
	int took;
	/* 0 = running, 1 = blocked needing input, 2 = blocked on output space */
	int stall = 0;

	if ( (bytes == NULL) || (frames == NULL) ){
		return -1;
	}
	in_avail = *bytes;
	out_cap  = *frames;
	*bytes  = 0;
	*frames = 0;

	if ( (final != 0) && (final != 1) ){
		return -1;
	}
	if ( (in_avail != 0u) && (src == NULL) ){
		return -1;
	}
	if ( (out_cap != 0u) && (dst == NULL) ){
		return -1;
	}
	s = streamdec_check(workspace);
	if ( s == NULL ){
		return -1;
	}

	/* sticky terminal states repeat with both counters zero */
	if ( s->status != 0 ){
		return s->status;
	}

	for ( ; ; ){

		/* drain a group decoded one frame ahead (zero-input safe) --- */
		if ( s->pending ){
			if ( out_n >= out_cap ){
				stall = 2;
				goto wait;
			}
			streamdec_write_group(s, dst, out_n);
			out_n++;
			s->pending = 0;
			continue;
		}

		/* ---- trailing frame CRC ------------------------------- */
		if ( s->phase == STREAMDEC_PHASE_CRC ){
			while ( s->crc_got < STREAMDEC_CRC_NBYTES ){
				if ( in_used >= in_avail ){
					stall = 1;
					goto wait;
				}
				s->crc_buf[s->crc_got] = src[in_used];
				s->crc_got++;
				in_used++;
			}
			{
				uint32_t expect = (uint32_t) s->crc_buf[0u]
					| ((uint32_t) s->crc_buf[1u] << 8u)
					| ((uint32_t) s->crc_buf[2u] << 16u)
					| ((uint32_t) s->crc_buf[3u] << 24u);
				uint32_t actual = CRC32_FINI(s->crc);

				s->status = (expect == actual) ? 1 : -3;
			}
			break;
		}

		/* ---- all audio decoded: validate zero tail + drain ----- */
		if ( s->frame_out == s->sample_frames ){
			uint32_t padmask;

			if ( (s->phase != STREAMDEC_PHASE_UNARY) || s->unary_long ){
				s->status = -4;
				break;
			}
			/* the encoder byte-aligns the frame by zero-padding the
			     last partial byte; the bits left in the cache must
			     therefore all be zero */
			padmask = (s->count == 0u)
				? 0u : (((uint32_t) 1u << s->count) - 1u);
			if ( (s->cache & padmask) != 0u ){
				s->status = -4;	/* garbage where padding belongs */
				break;
			}
			while ( s->src_total < s->payload_bytes ){
				uint8_t padbyte;

				if ( in_used >= in_avail ){
					stall = 1;
					goto wait;
				}
				padbyte = src[in_used];
				s->crc = crc32_cont_dec(padbyte, s->crc);
				s->src_total++;
				in_used++;
				if ( padbyte != 0u ){
					s->status = -4;
					break;
				}
			}
			if ( s->status != 0 ){
				break;
			}
			s->phase = STREAMDEC_PHASE_CRC;
			continue;
		}

		/* ---- unary --------------------------------------------- */
		if ( s->phase == STREAMDEC_PHASE_UNARY ){
			bitcnt_dec nbits;

			if ( s->count == 0u ){
				took = streamdec_take_payload(
					s, src, in_used, in_avail
				);
				if ( took <= 0 ){
					if ( took < 0 ){
						s->status = -4;	/* payload ended mid-frame */
						break;
					}
					stall = 1;
					goto wait;
				}
				in_used++;
			}

			nbits = (bitcnt_dec) TBCNT8((uint8_t) s->cache);
			if ( nbits >= s->count ){
				/* every available bit is a 1: no terminator yet */
				nbits = s->count;
				s->unary_acc += nbits;
				if ( s->unary_acc > s->unary_lax_limit ){
					s->status = -4;
					break;
				}
				s->cache     >>= nbits;
				s->count      -= nbits;	/* becomes 0 */
				s->unary_long = 1;
				continue;		/* fetch another byte */
			}

			/* terminator zero found at position 'nbits' */
			s->unary_acc += nbits;
			if ( s->unary_acc > s->unary_lax_limit ){
				s->status = -4;
				break;
			}
			s->cache     >>= (nbits + 1u);
			s->count      -= (nbits + 1u);
			s->unary_long = 0;
			s->unary_value = s->unary_acc;
			{
				struct Rice_Dec *const r =
					&s->codec[s->chan].rice.dec;

				s->bin_k = (s->unary_value != 0u)
					? r->k[1u] : r->k[0u];
			}
			s->phase = STREAMDEC_PHASE_BINARY;
			continue;
		}

		/* ---- binary remainder ---------------------------------- */
		{
			rice24_dec binary;
			uint32_t value;
			struct Codec *const c = &s->codec[s->chan];

			while ( s->count < s->bin_k ){
				took = streamdec_take_payload(
					s, src, in_used, in_avail
				);
				if ( took <= 0 ){
					if ( took < 0 ){
						s->status = -4;
						goto report;
					}
					stall = 1;
					goto wait;
				}
				in_used++;
			}

			binary = (rice24_dec) (
				s->cache & ((((uint32_t) 1u) << s->bin_k) - 1u)
			);
			s->cache >>= s->bin_k;
			s->count -= s->bin_k;

			/* the symbol is complete: only now touch channel state */
			if ( s->unary_value != 0u ){
				value = (uint32_t) (
					((s->unary_value - 1u) << s->bin_k)
					+ binary
				);
				rice24_update_dec(
					&c->rice.dec.sum[1u], &c->rice.dec.k[1u],
					value,
					&binexp32p4_table[c->rice.dec.k[1u]]
				);
				value += BINEXP32(c->rice.dec.k[0u]);
			}
			else {
				value = (uint32_t) binary;
			}
			rice24_update_dec(
				&c->rice.dec.sum[0u], &c->rice.dec.k[0u],
				value, &binexp32p4_table[c->rice.dec.k[0u]]
			);

			{
				int32_t curr;

				curr = tta_prefilter_dec(value);
				curr = tta_filter_dec(
					&c->filter, curr, s->filter_round,
					(bitcnt) s->filter_k
				);
				curr += tta_predict1(
					c->prev, (bitcnt) s->predict_k
				);
				c->prev = curr;
				s->frame[s->chan] = curr;
			}

			/* every symbol ends here: the next one always starts a
			     fresh unary, so reset before any channel/group branch
			     (otherwise channel 0's unary leaks into channel 1) */
			s->phase	= STREAMDEC_PHASE_UNARY;
			s->unary_acc	= 0;

			/* advance inside the channel group */
			s->chan++;
			if ( s->chan == s->nchan ){
				s->chan = 0;
				s->frame_out++;

				/* write now, or retain this single group so a
				     later zero-input call can drain it */
				if ( out_n < out_cap ){
					streamdec_write_group(s, dst, out_n);
					out_n++;
				}
				else {
					s->pending = 1;
					stall = 2;
					goto wait;
				}
			}
			continue;
		}
	}

wait:
	/* 'final' locks only when this whole slice was consumed; an output
	     stall with bytes still pending must not lock end-of-input */
	if ( (final != 0) && (in_used == in_avail) ){
		s->final_locked = 1;
	}

	/* once no more input can arrive, a stall that still needs a payload or
	     CRC byte is a real truncation.  an output stall is never one: the
	     caller may drain with a zero-input call. */
	if ( (s->status == 0) && s->final_locked && (stall == 1) ){
		s->status = -2;
	}

report:
	*bytes  = in_used;
	*frames = out_n;
	return s->status;
}

/* EOF //////////////////////////////////////////////////////////////////// */
