/* ///////////////////////////////////////////////////////////////////////////
//                                                                          //
// codec/streamdec.c                                                        //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// Copyright (C) 2026, Shane Seelig                                         //
// SPDX-License-Identifier: GPL-3.0-or-later                                //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
//      Incremental (byte-fragmented) decoder for a single TTA1 audio       //
//      frame.  The caller supplies the frame's compressed bytes in         //
//      arbitrarily sized pieces and receives interleaved int32_t PCM.      //
//                                                                          //
//      All progress - including a partially read unary code - lives in     //
//      a caller supplied, 16-byte aligned workspace.  Nothing is           //
//      allocated, and the workspace size is independent of the frame       //
//      length, the sample count, and the length of any unary code.         //
//                                                                          //
//      The compressed frame layout consumed here is the seektable's:       //
//                                                                          //
//          [ payload bytes ][ 4-byte little-endian CRC32 ]                 //
//                                                                          //
//      with 'frame_bytes' covering both parts.  The CRC covers exactly     //
//      the payload, one update per payload byte as it enters the bit       //
//      cache; the trailing 4 bytes never reach the bit cache.              //
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
#include "./tta_state.h"
#include "./types.h"

/* //////////////////////////////////////////////////////////////////////// */

#define STREAMDEC_MAGIC		UINT32_C(0x53544431)	/* 'STD1' */
#define STREAMDEC_NCHAN_MAX	8u
#define STREAMDEC_CRC_BYTES	((size_t) 4u)

/* a frame is at least the CRC plus one payload byte */
#define STREAMDEC_FRAME_BYTES_MIN \
	(STREAMDEC_CRC_BYTES + (size_t) 1u)

/* ------------------------------------------------------------------------ */

struct ALIGNED(LIBTTAr_CODECSTATE_PRIV_ALIGN) StreamDec_State {
	/* the codec array is first so that it stays 16-byte aligned */
	struct Codec	codec[STREAMDEC_NCHAN_MAX];
	int32_t		group[STREAMDEC_NCHAN_MAX];	/* one pending frame */
	/* * */
	uint32_t	magic;
	uint32_t	crc;		/* running CRC over payload bytes */
	uint32_t	crc_stored;	/* CRC assembled from the tail bytes */
	uint32_t	bitcache;	/* valid bits are the low 'count' bits */
	uint32_t	binary;		/* bits of the current binary code */
	rice24_dec	unary;		/* ones seen in the current unary code */
	rice24_dec	unary_lax;	/* limit for one unary code */
	/* * */
	size_t		pos;		/* frame bytes consumed so far */
	size_t		payload;	/* 'frame_bytes' - 4 */
	size_t		frame_bytes;
	size_t		sample_frames;
	size_t		frames_done;	/* sample frames handed to the caller */
	size_t		samples_done;	/* symbols decoded */
	size_t		total_samples;	/* 'sample_frames' * 'nchan' */
	/* * */
	bitcnt_dec	predict_k;
	bitcnt_dec	filter_k;
	int32_t		filter_round;
	/* * */
	unsigned int	nchan;
	unsigned int	chan;		/* channel of the current group */
	unsigned int	bin_got;	/* binary bits read for the symbol */
	unsigned int	bin_k;		/* binary bits needed for the symbol */
	unsigned int	count;		/* valid bits in 'bitcache' */
	unsigned int	crc_read;	/* CRC tail bytes gathered */
	unsigned int	phase;		/* 0 unary, 1 binary */
	/* * */
	int		pending;	/* 'group' holds a complete frame */
	int		final_locked;	/* no more input will ever arrive */
	int		done;		/* frame delivered and CRC verified */
	int		error;		/* sticky bitstream error */
};

/* //////////////////////////////////////////////////////////////////////// */

CONST
static size_t streamdec_size_of(unsigned int) /*@*/;

static void streamdec_init_state(
	struct StreamDec_State *RESTRICT, unsigned int,
	enum LibTTAr_SampleBytes, size_t, size_t
)
/*@modifies	*@*/
;

static void streamdec_emit(
	const struct StreamDec_State *RESTRICT, /*@out@*/ int32_t *RESTRICT
)
/*@modifies	*@*/
;

static void streamdec_symbol_done(struct StreamDec_State *RESTRICT)
/*@modifies	*@*/
;

static int streamdec_feed(
	struct StreamDec_State *RESTRICT, const uint8_t *RESTRICT,
	size_t, size_t *
)
/*@modifies	*@*/
;

/* //////////////////////////////////////////////////////////////////////// */

/**@fn libttaR_streamdec_size
 * @brief calculates the workspace size of the streaming frame decoder
 *
 * @param nchan - number of audio channels
 *
 * @return number of bytes the workspace must provide for 'nchan'
 * @retval 0 - bad 'nchan' (0 or greater than 8)
 *
 * @pre the workspace passed to libttaR_streamdec_init() must be aligned to
 *   LIBTTAr_CODECSTATE_PRIV_ALIGN (16) bytes
 * @note the returned size never exceeds 4096 * (nchan + 1) bytes
**/
BUILD_EXPORT
CONST
size_t
libttaR_streamdec_size(const unsigned int nchan)
/*@*/
{
	return streamdec_size_of(nchan);
}

/**@fn libttaR_streamdec_init
 * @brief initializes (or resets) a streaming frame decoder workspace
 *
 * @param workspace       - caller supplied, 16-byte aligned workspace
 * @param workspace_bytes - size of 'workspace'
 * @param samplebytes     - number of bytes per PCM sample (1, 2, or 3)
 * @param nchan           - number of audio channels (1 through 8)
 * @param sample_frames   - samples per channel in the frame (must be > 0)
 * @param frame_bytes     - total compressed frame size, including the
 *   trailing 4-byte little-endian CRC
 *
 * @retval 0  - success (an old state, if any, was reset)
 * @retval -1 - bad parameter, bad size, or arithmetic overflow
 *
 * @note one frame is decoded per initialization; after
 *   libttaR_streamdec_process() reports 1 (or a sticky error), call this
 *   again to start another frame
**/
BUILD_EXPORT
int
libttaR_streamdec_init(
	void *const workspace, const size_t workspace_bytes,
	const unsigned int samplebytes, const unsigned int nchan,
	const size_t sample_frames, const size_t frame_bytes
)
/*@modifies	*workspace@*/
{
	struct StreamDec_State *state;
	size_t size, total_samples;
	int overflow;

	if ( workspace == NULL ){
		return -1;
	}
	if ( ((uintptr_t) workspace) % LIBTTAr_CODECSTATE_PRIV_ALIGN != 0 ){
		return -1;
	}
	if ( (samplebytes < (unsigned int) LIBTTAr_SAMPLEBYTES_1)
	    ||
	     (samplebytes > (unsigned int) LIBTTAr_SAMPLEBYTES_MAX)
	){
		return -1;
	}
	if ( (nchan == 0u) || (nchan > STREAMDEC_NCHAN_MAX) ){
		return -1;
	}
	if ( sample_frames == 0 ){
		return -1;
	}
	if ( frame_bytes < STREAMDEC_FRAME_BYTES_MIN ){
		return -1;
	}

	size = streamdec_size_of(nchan);
	if ( size == 0 ){
		return -1;
	}
	if ( workspace_bytes < size ){
		return -1;
	}

	/* 'nchan' is nonzero, so this is a genuine overflow check */
	overflow = mul_usize_overflow(&total_samples, sample_frames, nchan);
	if ( overflow != 0 ){
		return -1;
	}
	if ( total_samples == 0 ){
		return -1;
	}

	state = (struct StreamDec_State *) workspace;
	streamdec_init_state(
		state, nchan, (enum LibTTAr_SampleBytes) samplebytes,
		sample_frames, frame_bytes
	);
	return 0;
}

/**@fn libttaR_streamdec_process
 * @brief incrementally decodes one TTA1 frame
 *
 * @param workspace - workspace initialized by libttaR_streamdec_init()
 * @param src       - compressed input bytes (may be NULL if *bytes == 0)
 * @param bytes     - in: readable bytes in 'src'; out: bytes consumed
 * @param dst       - interleaved int32_t output (may be NULL if *frames == 0)
 * @param frames    - in: writable sample frames in 'dst'; out: frames written
 * @param final     - nonzero if no bytes will be supplied after this call
 *
 * @retval  0 - progress was made, or the decoder is waiting for more input
 *              or more output capacity
 * @retval  1 - every sample was delivered and the CRC matched
 * @retval -1 - bad parameter (the decoder state is not advanced)
 * @retval -2 - 'final' input arrived before the frame was complete
 * @retval -3 - the frame CRC does not match
 * @retval -4 - malformed bitstream or inconsistent declared frame length
 *
 * @note PCM may be delivered before the CRC has been checked.  Only a return
 *   of 1 certifies the frame; after a negative return the decoder must be
 *   re-initialized.  Output back pressure never reports -2: the caller may
 *   drain the remaining frames with a zero-length input.  The terminal state
 *   is sticky: a finished decoder returns 1 (or its error) with both counts
 *   set to 0.
**/
BUILD_EXPORT
int
libttaR_streamdec_process(
	void *const workspace, const uint8_t *const src,
	size_t *const bytes, int32_t *const dst, size_t *const frames,
	const int final
)
/*@modifies	*bytes,
		*dst,
		*frames,
		*workspace
@*/
{
	struct StreamDec_State *state;
	size_t avail, cap, consumed, delivered;
	int final_arg, retval;

	/* illegal parameters never advance the decoder; report zero progress
	   whenever the count pointers are usable */
	if ( (workspace == NULL) || (bytes == NULL) || (frames == NULL) ){
		if ( bytes != NULL ){
			*bytes = 0;
		}
		if ( frames != NULL ){
			*frames = 0;
		}
		return -1;
	}
	if ( ((uintptr_t) workspace) % LIBTTAr_CODECSTATE_PRIV_ALIGN != 0 ){
		*bytes  = 0;
		*frames = 0;
		return -1;
	}

	state = (struct StreamDec_State *) workspace;
	if ( state->magic != STREAMDEC_MAGIC ){
		*bytes  = 0;
		*frames = 0;
		return -1;
	}

	avail = *bytes;
	cap   = *frames;
	if ( ((avail != 0) && (src == NULL))
	    ||
	     ((cap != 0) && (dst == NULL))
	){
		*bytes  = 0;
		*frames = 0;
		return -1;
	}

	if ( state->done != 0 ){
		*bytes  = 0;
		*frames = 0;
		return 1;
	}
	if ( state->error != 0 ){
		*bytes  = 0;
		*frames = 0;
		return state->error;
	}

	final_arg = (final != 0);
	consumed  = 0;
	delivered = 0;
	retval    = 0;

	for (;;){
		/* a complete sample frame is waiting for output space */
		if ( state->pending != 0 ){
			if ( delivered < cap ){
				streamdec_emit(
					state, dst + (delivered * state->nchan)
				);
				state->pending = 0;
				state->chan    = 0;
				state->frames_done += 1;
				delivered += 1;
				continue;
			}
			/* no output space; only the CRC tail can still move */
			if ( state->samples_done < state->total_samples ){
				retval = 0;
				goto out;
			}
		}

		/* every symbol is decoded: validate the tail and the CRC */
		if ( state->samples_done == state->total_samples ){
			if ( state->crc_read == 0u ){
				/* before the CRC bytes are read, every declared
				   payload byte must have been consumed and the
				   unused bits of the last byte must be zero */
				if ( state->pos != state->payload ){
					retval = -4;
					goto bad;
				}
				if ( state->bitcache != 0u ){
					retval = -4;
					goto bad;
				}
				/* an idle bit cache means the CRC starts here;
				   give the caller its audio before the verdict */
				if ( state->pending != 0 ){
					retval = 0;
					goto out;
				}
			}

			while ( (state->crc_read < STREAMDEC_CRC_BYTES)
			        &&
				(consumed < avail)
			){
				const uint32_t byte = (uint32_t) src[consumed];

				consumed += 1;
				state->pos += 1;
				state->crc_stored |= (uint32_t) (
					byte << (8u * state->crc_read)
				);
				state->crc_read += 1;
			}

			if ( state->crc_read < STREAMDEC_CRC_BYTES ){
				retval = 0;
				goto out;
			}
			if ( state->crc_stored != CRC32_FINI(state->crc) ){
				retval = -3;
				goto bad;
			}
			if ( state->pending != 0 ){
				/* valid audio still has to be handed over */
				retval = 0;
				goto out;
			}
			state->done = 1;
			retval = 1;
			goto out;
		}

		/* decoding one group ahead of the output capacity lets a caller
		   drain the buffered group with a zero-length input.
		   streamdec_feed() refuses to read past the declared payload */
		{
			const int feed = streamdec_feed(
				state, src, avail, &consumed
			);

			if ( feed < 0 ){
				retval = -4;
				goto bad;
			}
			if ( feed == 0 ){
				retval = 0;
				goto out;
			}
		}
	}

bad:
	state->error = retval;

out:
	if ( (final_arg != 0) && (consumed == avail) ){
		state->final_locked = 1;
	}
	/* a locked final state with no input left is a truncation, unless the
	   only thing missing is output space for an already decoded frame */
	if ( (retval == 0) && (state->final_locked != 0) && (consumed == avail) ){
		if ( (state->pending == 0)
		    &&
		     (state->samples_done < state->total_samples)
		){
			state->error = -2;
			retval = -2;
		}
		else if ( (state->samples_done == state->total_samples)
		          &&
			  (state->crc_read < STREAMDEC_CRC_BYTES)
		          &&
			  (state->pending == 0)
		){
			state->error = -2;
			retval = -2;
		}
	}

	*bytes  = consumed;
	*frames = delivered;
	return retval;
}

/* //////////////////////////////////////////////////////////////////////// */

/**@fn streamdec_size_of
 * @brief internal workspace size query
**/
CONST
static size_t
streamdec_size_of(const unsigned int nchan)
/*@*/
{
	size_t size;
	size_t mod;

	if ( (nchan == 0u) || (nchan > STREAMDEC_NCHAN_MAX) ){
		return 0;
	}
	size = sizeof(struct StreamDec_State);
	mod  = size % LIBTTAr_CODECSTATE_PRIV_ALIGN;
	if ( mod != 0 ){
		size += LIBTTAr_CODECSTATE_PRIV_ALIGN - mod;
	}
	/* guaranteed by the fixed layout; keep the promise explicit */
	if ( size > (SIZE_C(4096) * ((size_t) nchan + 1u)) ){
		return 0;
	}
	return size;
}

/**@fn streamdec_init_state
 * @brief resets a streaming decoder workspace
**/
static void
streamdec_init_state(
	struct StreamDec_State *const RESTRICT state, const unsigned int nchan,
	const enum LibTTAr_SampleBytes samplebytes, const size_t sample_frames,
	const size_t frame_bytes
)
/*@modifies	*state@*/
{
	MEMSET(state, 0x00, sizeof *state);

	codec_init_dec(state->codec, nchan);

	state->magic         = STREAMDEC_MAGIC;
	state->crc           = CRC32_INIT;
	state->crc_stored    = 0;
	state->bitcache      = 0;
	state->binary        = 0;
	state->unary         = 0;
	state->unary_lax     = get_unary_lax_limit(samplebytes);
	state->pos           = 0;
	state->payload       = frame_bytes - STREAMDEC_CRC_BYTES;
	state->frame_bytes   = frame_bytes;
	state->sample_frames = sample_frames;
	state->frames_done   = 0;
	state->samples_done  = 0;
	/* 'sample_frames' and 'nchan' are nonzero and cannot overflow */
	state->total_samples = sample_frames * (size_t) nchan;
	state->predict_k     = (bitcnt_dec) get_predict_k(samplebytes);
	state->filter_k      = (bitcnt_dec) get_filter_k(samplebytes);
	state->filter_round  = get_filter_round(samplebytes);
	state->nchan         = nchan;
	state->chan          = 0;
	state->bin_got       = 0;
	state->bin_k         = 0;
	state->count         = 0;
	state->crc_read      = 0;
	state->phase         = 0;
	state->pending       = 0;
	state->final_locked  = 0;
	state->done          = 0;
	state->error         = 0;

	return;
}

/**@fn streamdec_emit
 * @brief inverse channel decorrelation of one complete sample frame
**/
static void
streamdec_emit(
	const struct StreamDec_State *const RESTRICT state,
	/*@out@*/ int32_t *const RESTRICT dst
)
/*@modifies	*dst@*/
{
	const unsigned int nchan = state->nchan;
	unsigned int j;
	int32_t curr;

	if ( nchan == 1u ){
		dst[0] = state->group[0];
		return;
	}

	for ( j = 0; (j + 1u) < nchan; ++j ){
		dst[j] = state->group[j];
	}
	curr = state->group[nchan - 1u] + (state->group[nchan - 2u] / 2);
	dst[nchan - 1u] = curr;
	j = nchan - 1u;
	while ( j-- != 0u ){
		curr   -= dst[j];
		dst[j]  = curr;
	}

	return;
}

/**@fn streamdec_symbol_done
 * @brief finish the current symbol and update only its channel's history
**/
static void
streamdec_symbol_done(struct StreamDec_State *const RESTRICT state)
/*@modifies	*state@*/
{
	const unsigned int chan = state->chan;
	struct Codec *const RESTRICT codec = &state->codec[chan];
	uint32_t value;
	int32_t x;

	/* the Rice model is updated only now that unary and binary bits of
	   this symbol are complete */
	if ( state->unary != 0u ){
		value = (uint32_t) (
			(((rice24_dec) state->unary - 1u)
			 << state->bin_k) + state->binary
		);
		rice24_update_dec(
			&codec->rice.dec.sum[1u], &codec->rice.dec.k[1u],
			value, &binexp32p4_table[codec->rice.dec.k[1u]]
		);
		value += (uint32_t) BINEXP32(codec->rice.dec.k[0u]);
		rice24_update_dec(
			&codec->rice.dec.sum[0u], &codec->rice.dec.k[0u],
			value, &binexp32p4_table[codec->rice.dec.k[0u]]
		);
	}
	else {	value = state->binary;
		rice24_update_dec(
			&codec->rice.dec.sum[0u], &codec->rice.dec.k[0u],
			value, &binexp32p4_table[codec->rice.dec.k[0u]]
		);
	}

	x  = tta_prefilter_dec(value);
	x  = tta_filter_dec(
		&codec->filter, x, state->filter_round, state->filter_k
	);
	x += tta_predict1(codec->prev, state->predict_k);
	codec->prev = x;

	state->group[chan] = x;
	state->chan        = chan + 1u;
	state->phase       = 0;
	state->unary       = 0;
	state->binary      = 0;
	state->bin_got     = 0;
	state->samples_done += 1;
	if ( state->chan == state->nchan ){
		state->pending = 1;
	}

	return;
}

/**@fn streamdec_feed
 * @brief consume as many bits as possible for the current symbol
 *
 * @return 1 if a symbol completed, 0 if more input is required, -1 if the
 *   bitstream is malformed
**/
static int
streamdec_feed(
	struct StreamDec_State *const RESTRICT state,
	const uint8_t *const RESTRICT src, const size_t avail, size_t *const idx
)
/*@modifies	*state,
		*idx
@*/
{
	for (;;){
		if ( state->phase == 0u ){
			/* unary code: ones up to a zero terminator */
			if ( state->count == 0u ){
				uint8_t byte;
				unsigned int nbits;

				if ( state->pos >= state->payload ){
					return -1;
				}
				if ( *idx >= avail ){
					return 0;
				}
				byte = src[(*idx)++];
				state->pos += 1;
				state->crc = (uint32_t) crc32_cont_dec(
					byte, (crc32_dec) state->crc
				);

				nbits = (unsigned int) TBCNT8(byte);
				if ( nbits == 8u ){
					state->unary += (rice24_dec) 8u;
					if ( state->unary > state->unary_lax ){
						return -1;
					}
					continue;
				}
				state->unary += (rice24_dec) nbits;
				if ( state->unary > state->unary_lax ){
					return -1;
				}
				state->bitcache = (uint32_t) byte
					>> (nbits + 1u);
				state->count = 7u - nbits;
			}
			else {	unsigned int nbits;

				nbits = (unsigned int) TBCNT8(state->bitcache);
				if ( nbits >= state->count ){
					/* every cached bit is a one */
					state->unary += (rice24_dec) (
						state->count
					);
					if ( state->unary > state->unary_lax ){
						return -1;
					}
					state->bitcache >>= state->count;
					state->count = 0;
					continue;
				}
				state->unary += (rice24_dec) nbits;
				if ( state->unary > state->unary_lax ){
					return -1;
				}
				state->bitcache >>= (nbits + 1u);
				state->count -= (nbits + 1u);
			}

			/* the unary code is complete; set up the binary code */
			state->phase   = 1u;
			state->binary  = 0;
			state->bin_got = 0;
			state->bin_k   = (state->unary != 0u)
				? (unsigned int) state->codec[state->chan]
					.rice.dec.k[1u]
				: (unsigned int) state->codec[state->chan]
					.rice.dec.k[0u];
			if ( state->bin_k == 0u ){
				streamdec_symbol_done(state);
				return 1;
			}
			continue;
		}
		else {	/* binary code: 'bin_k' low bits, least significant first */
			unsigned int need, take;
			uint32_t mask;

			need = state->bin_k - state->bin_got;
			if ( state->count == 0u ){
				uint8_t byte;

				if ( state->pos >= state->payload ){
					return -1;
				}
				if ( *idx >= avail ){
					return 0;
				}
				byte = src[(*idx)++];
				state->pos += 1;
				state->crc = (uint32_t) crc32_cont_dec(
					byte, (crc32_dec) state->crc
				);
				state->bitcache = (uint32_t) byte;
				state->count    = 8u;
				continue;
			}

			take = (need < state->count) ? need : state->count;
			mask = ((uint32_t) 1u << take) - 1u;
			state->binary |= (uint32_t) (
				(state->bitcache & mask) << state->bin_got
			);
			state->bitcache >>= take;
			state->count    -= take;
			state->bin_got  += take;
			if ( state->bin_got == state->bin_k ){
				streamdec_symbol_done(state);
				return 1;
			}
		}
	}
}

/* EOF //////////////////////////////////////////////////////////////////// */
