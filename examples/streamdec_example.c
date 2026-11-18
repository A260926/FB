/* ///////////////////////////////////////////////////////////////////////////
//                                                                          //
// examples/streamdec_example.c                                             //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// A self-contained round trip for libttaR_streamdec_*():                   //
//                                                                          //
//   1. build some PCM, encode one frame with libttaR_tta_encode(),         //
//   2. append the little-endian frame CRC the seektable stores,            //
//   3. decode that frame in deliberately awkward fragments,                //
//   4. compare the result with the original PCM and with a one-shot        //
//      libttaR_tta_decode() of the same frame.                             //
//                                                                          //
// Build (from the repository root):                                        //
//                                                                          //
//   cc -std=c99 -O2 -I. src/build_lib.c examples/streamdec_example.c \     //
//      -o streamdec_example && ./streamdec_example                         //
//                                                                          //
// Only the standard library's malloc/free are used, and no external codec  //
// tool is involved.                                                        //
//                                                                          //
/////////////////////////////////////////////////////////////////////////// */

#include "src/libttaR.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* //////////////////////////////////////////////////////////////////////// */

static void *
align16(void *const p)
{
	return (void *) ((((uintptr_t) p) + 15u) & ~(uintptr_t) 15u);
}

/* encode one frame and append its trailing 4-byte little-endian CRC */
static int
encode_frame(
	const unsigned int samplebytes, const unsigned int nchan,
	const int32_t *const pcm, const size_t frames,
	uint8_t **const out, size_t *const out_len
)
{
	const size_t nsamples = frames * (size_t) nchan;
	const size_t margin = libttaR_ttabuf_safety_margin(
		(enum LibTTAr_SampleBytes) samplebytes, nchan
	);
	const size_t priv_size = libttaR_codecstate_priv_size(nchan);
	const size_t cap = (2u * margin) + (nsamples * 16u) + 64u;
	struct LibTTAr_CodecState_User user = LIBTTAr_CODECSTATE_USER_INIT;
	struct LibTTAr_EncMisc misc;
	uint8_t *buf;
	void *priv;
	size_t i;

	if ( (margin == 0u) || (priv_size == 0u) ){
		return -1;
	}
	buf  = (uint8_t *) calloc(cap + 4u, 1u);
	priv = calloc(priv_size + 16u, 1u);
	if ( (buf == NULL) || (priv == NULL) ){
		free(buf);
		free(priv);
		return -1;
	}

	misc.dest_len      = cap;
	misc.src_len       = nsamples;
	misc.ni32_target   = nsamples;
	misc.ni32_perframe = nsamples;
	misc.samplebytes   = (enum LibTTAr_SampleBytes) samplebytes;
	misc.nchan         = nchan;

	if ( libttaR_tta_encode(
		buf, pcm, align16(priv), &user, &misc
	) != LIBTTAr_ERV_OK_DONE ){
		free(buf);
		free(priv);
		return -1;
	}
	free(priv);

	for ( i = 0; i < 4u; ++i ){
		buf[user.nbytes_tta_total + i] =
			(uint8_t) (user.crc >> (8u * i));
	}
	*out     = buf;
	*out_len = user.nbytes_tta_total + 4u;
	return 0;
}

/* one-shot reference decode of the payload */
static int
decode_reference(
	const unsigned int samplebytes, const unsigned int nchan,
	const uint8_t *const frame, const size_t frame_len,
	const size_t frames, int32_t *const pcm
)
{
	const size_t nsamples = frames * (size_t) nchan;
	const size_t margin = libttaR_ttabuf_safety_margin(
		(enum LibTTAr_SampleBytes) samplebytes, nchan
	);
	const size_t priv_size = libttaR_codecstate_priv_size(nchan);
	const size_t payload = frame_len - 4u;
	struct LibTTAr_CodecState_User user = LIBTTAr_CODECSTATE_USER_INIT;
	struct LibTTAr_DecMisc misc;
	void *priv;
	int retval;

	priv = calloc(priv_size + 16u, 1u);
	if ( priv == NULL ){
		return -1;
	}
	misc.dest_len            = nsamples;
	misc.src_len             = payload + margin;
	misc.ni32_target         = nsamples;
	misc.nbytes_tta_target   = payload;
	misc.ni32_perframe       = nsamples;
	misc.nbytes_tta_perframe = payload;
	misc.samplebytes         = (enum LibTTAr_SampleBytes) samplebytes;
	misc.nchan               = nchan;

	retval = (int) libttaR_tta_decode(
		pcm, frame, align16(priv), &user, &misc
	);
	free(priv);
	return (retval == LIBTTAr_DRV_OK_DONE) ? 0 : -1;
}

/* //////////////////////////////////////////////////////////////////////// */

int
main(void)
{
	const unsigned int samplebytes = 2u;
	const unsigned int nchan = 3u;
	const size_t frames = 96u;
	const size_t nsamples = frames * (size_t) nchan;
	int32_t *pcm = (int32_t *) calloc(nsamples, sizeof *pcm);
	int32_t *ref = (int32_t *) calloc(nsamples, sizeof *ref);
	int32_t *got = (int32_t *) calloc(nsamples, sizeof *got);
	uint8_t *frame = NULL;
	size_t frame_len = 0, ws_size, pos, done, i;
	uint8_t *ws_raw, *ws;
	int retval, status = 1;

	if ( (pcm == NULL) || (ref == NULL) || (got == NULL) ){
		goto cleanup;
	}
	for ( i = 0; i < nsamples; ++i ){
		pcm[i] = (int32_t) (((i * 7919u) + ((i % nchan) * 997u))
			% 20001u) - 10000;
	}

	if ( encode_frame(samplebytes, nchan, pcm, frames, &frame, &frame_len) ){
		fprintf(stderr, "encode failed\n");
		goto cleanup;
	}
	if ( decode_reference(
		samplebytes, nchan, frame, frame_len, frames, ref
	) || (memcmp(pcm, ref, nsamples * sizeof *pcm) != 0) ){
		fprintf(stderr, "reference decode failed\n");
		goto cleanup;
	}

	ws_size = libttaR_streamdec_size(nchan);
	if ( ws_size == 0u ){
		fprintf(stderr, "bad workspace size\n");
		goto cleanup;
	}
	ws_raw = (uint8_t *) malloc(ws_size + 16u);
	if ( ws_raw == NULL ){
		goto cleanup;
	}
	ws = (uint8_t *) align16(ws_raw);
	if ( libttaR_streamdec_init(
		ws, ws_size, samplebytes, nchan, frames, frame_len
	) != 0 ){
		fprintf(stderr, "init failed\n");
		free(ws_raw);
		goto cleanup;
	}

	/* feed one byte at a time, with room for only two frames at once */
	pos  = 0;
	done = 0;
	while ( pos < frame_len ){
		size_t bytes = 1u;
		size_t room = 2u;
		size_t n;

		retval = libttaR_streamdec_process(
			ws, frame + pos, &bytes, got + (done * nchan), &room,
			(pos + 1u == frame_len)
		);
		if ( retval < 0 ){
			fprintf(stderr, "stream decode failed: %d\n", retval);
			free(ws_raw);
			goto cleanup;
		}
		pos  += bytes;
		done += room;
		if ( pos == frame_len ){
			/* drain whatever is still buffered with no input */
			while ( retval == 0 ){
				size_t zero = 0u;
				size_t more = 2u;

				retval = libttaR_streamdec_process(
					ws, NULL, &zero,
					got + (done * nchan), &more, 1
				);
				if ( retval < 0 ){
					fprintf(stderr,
						"drain failed: %d\n", retval);
					free(ws_raw);
					goto cleanup;
				}
				done += more;
			}
		}
	}
	if ( (retval != 1) || (pos != frame_len) || (done != frames) ){
		fprintf(stderr, "frame did not complete cleanly\n");
		free(ws_raw);
		goto cleanup;
	}
	if ( memcmp(got, pcm, nsamples * sizeof *pcm) != 0 ){
		fprintf(stderr, "PCM mismatch\n");
		free(ws_raw);
		goto cleanup;
	}
	free(ws_raw);

	printf("streamdec example: %u-bit, %u channels, %zu frames, "
		"%zu compressed bytes, workspace %zu bytes, PCM verified\n",
		samplebytes * 8u, nchan, frames, frame_len, ws_size);
	status = 0;

cleanup:
	free(pcm);
	free(ref);
	free(got);
	free(frame);
	return status;
}

/* EOF //////////////////////////////////////////////////////////////////// */
