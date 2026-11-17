/* ///////////////////////////////////////////////////////////////////////////
//                                                                          //
// modes/mode_decode_loop.c                                                 //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// Copyright (C) 2023-2025, Shane Seelig                                    //
// SPDX-License-Identifier: GPL-3.0-or-later                                //
//                                                                          //
/////////////////////////////////////////////////////////////////////////// */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../libttaR.h"

#include "../alloc.h"
#include "../byteswap.h"
#include "../cli.h"
#include "../common.h"
#include "../debug.h"
#include "../formats.h"
#include "../main.h"

#include "./bufs.h"
#include "./mt-struct.h"
#include "./pqueue.h"
#include "./threads.h"

/* //////////////////////////////////////////////////////////////////////// */

#undef decbuf
#undef priv
#undef user_out
#undef nsamples_flat_2pad
static NOINLINE enum LibTTAr_DecRetVal dec_frame_decode(
	struct DecBuf *RESTRICT decbuf,
	/*@reldef@*/ struct LibTTAr_CodecState_Priv *RESTRICT priv,
	/*@out@*/ struct LibTTAr_CodecState_User *RESTRICT user_out,
	enum LibTTAr_SampleBytes, unsigned int, size_t, size_t,
	/*@out@*/ size_t *RESTRICT nsamples_flat_2pad
)
/*@modifies	*decbuf->i32buf,
		*decbuf->pcmbuf,
		*priv,
		*user_out,
		*nsamples_flat_2pad
@*/
;

#undef decbuf
#undef dstat_out
#undef outfile
static void NOINLINE dec_frame_write(
	struct DecBuf *RESTRICT decbuf,
	/*@in@*/ struct DecStats *RESTRICT dstat_out,
	struct LibTTAr_CodecState_User *RESTRICT,
	const char *RESTRICT, FILE *RESTRICT outfile,
	const char *RESTRICT, enum LibTTAr_SampleBytes, unsigned int,
	uint32_t, int8_t, size_t, size_t,
	const struct SampleRangeWrite *RESTRICT, size_t
)
/*@globals	fileSystem@*/
/*@modifies	fileSystem,
		*decbuf->pcmbuf,
		*dstat_out,
		outfile
@*/
;

CONST
static size_t dec_ni32_perframe(size_t, size_t, size_t, unsigned int) /*@*/;

#undef pcmbuf
COLD
static NOINLINE void dec_frame_zeropad(
	uint8_t *RESTRICT pcmbuf, size_t, size_t, enum LibTTAr_SampleBytes
)
/*@modifies	*pcmbuf@*/
;

/* ------------------------------------------------------------------------ */

#undef arg
HOT
START_ROUTINE_ABI
static start_routine_ret decmt_io(struct MTArg_DecIO *RESTRICT arg)
/*@globals	fileSystem,
		internalState
@*/
/*@modifies	fileSystem,
		internalState,
		*arg->frames.navailable,
		*arg->frames.post_decoder,
		*arg->frames.ni32_perframe,
		*arg->frames.decbuf,
		*arg->frames.crc_read,
		arg->outfile.handle,
		arg->infile.handle,
		*arg->seektable,
		*arg->dstat_out
@*/
;

#undef arg
START_ROUTINE_ABI
static start_routine_ret decmt_decoder_wrapper(void *const RESTRICT arg)
/*@globals	fileSystem,
		internalState
@*/
/*@modifies	fileSystem,
		internalState,
		*arg
@*/
;

#undef arg
HOT
START_ROUTINE_ABI
static start_routine_ret decmt_decoder(struct MTArg_Decoder *RESTRICT arg)
/*@globals	fileSystem,
		internalState
@*/
/*@modifies	fileSystem,
		internalState,
		*arg->frames.navailable,
		arg->frames.queue,
		*arg->frames.post_decoder,
		*arg->frames.decbuf,
		*arg->frames.dec_retval
@*/
;

/* //////////////////////////////////////////////////////////////////////// */

/**@fn decst_loop
 * @brief the single-threaded decoder
 *
 * @param seektable    - seektable struct
 * @param dstat_out    - decode stats return struct
 * @param fstat        - bloated file stats struct
 * @param outfile      - destination file
 * @param outfile_name - name of the destination file (warnings/errors)
 * @param infile       - source file
 * @param infile_name  - name of the source file (warnings/errors)
 * @param range        - sample-frame interval plan, or NULL for the whole file
**/
HOT
BUILD NOINLINE void
decst_loop(
	const struct SeekTable *const RESTRICT seektable,
	/*@out@*/ struct DecStats *const RESTRICT dstat_out,
	const struct FileStats *const RESTRICT fstat,
	FILE *const RESTRICT outfile, const char *const outfile_name,
	FILE *const RESTRICT infile, const char *const infile_name,
	const struct SampleRangePlan *const RESTRICT range
)
/*@globals	fileSystem,
		internalState
@*/
/*@modifies	fileSystem,
		internalState,
		*dstat_out,
		outfile,
		infile
@*/
{
	const size_t framelen     = fstat->framelen;
	const size_t buflen       = fstat->buflen;
	const size_t nsamples_enc = fstat->nsamples_enc;
	const unsigned int nchan  = (unsigned int)   fstat->nchan;
	const enum LibTTAr_SampleBytes samplebytes = fstat->samplebytes;
	/* * */
	struct LibTTAr_CodecState_Priv *priv = NULL;
	struct LibTTAr_CodecState_User user;
	struct DecBuf decbuf;
	struct DecStats dstat;
	struct SampleRangeWrite range_write;
	/* * */
	size_t ni32_perframe, nbytes_tta_perframe, framesize_tta, nbytes_read;
	size_t nsamples_perchan_dec_total = 0;
	size_t frame_index, frame_index_end;
	int8_t dec_retval;
	size_t nsamples_flat_2pad;
	uint32_t crc_read;
	union {	size_t	z;
		int	d;
	} result;

	/* setup */
	memset(&dstat, 0x00, sizeof dstat);
	memset(&range_write, 0x00, sizeof range_write);
	decbuf_init(
		&decbuf, buflen, TTABUF_LEN_DEFAULT, nchan, samplebytes,
		CBM_SINGLE_THREADED
	);
	priv = priv_alloc(nchan);

	/* seektable entries before the selected interval still determine
		the sample-frame count of the selected frames, but they are not
		read or decoded
	*/
	if ( range != NULL ){
		frame_index      = range->first_frame;
		frame_index_end  = range->first_frame + range->nframes;
		nsamples_perchan_dec_total = (
			range->first_frame * framelen
		);
		if ( nsamples_perchan_dec_total > nsamples_enc ){
			nsamples_perchan_dec_total = nsamples_enc;
		}
		range_write.enabled = 1;
		range_write.start   = range->start;
		range_write.end     = range->end;
	}
	else {	frame_index     = 0;
		frame_index_end = seektable->nmemb;
	}

	while ( frame_index < frame_index_end ){
		/* xENCFMT_TTA1:
			- get size of tta-frame from seektable
		*/
		framesize_tta = (size_t) byteswap_letoh_u32(
			seektable->table[frame_index]
		);
		if ( framesize_tta <= (sizeof crc_read) ){
			warning_tta(
				"%s: frame %zu: malformed seektable entry",
				infile_name, frame_index
			);
			break;
		}
		else {	framesize_tta -= (sizeof crc_read); }
		ni32_perframe = dec_ni32_perframe(
			nsamples_perchan_dec_total, nsamples_enc, framelen,
			nchan
		);
		if ( ni32_perframe == 0 ){
			/* malformed seektable */
			break;
		}
		nsamples_perchan_dec_total += ni32_perframe / nchan;

		/* read TTA from infile */
		decbuf_check_adjust(
			&decbuf, framesize_tta, nchan, samplebytes
		);
		nbytes_read = fread(
			decbuf.ttabuf, SIZE_C(1), framesize_tta, infile
		);
		nbytes_tta_perframe = nbytes_read;
		if UNLIKELY ( nbytes_read != framesize_tta ){
			if UNLIKELY ( ferror(infile) != 0 ){
				error_sys(errno, "fread", infile_name);
			}
			else {	warning_tta("%s: frame %zu: truncated file",
					infile_name, frame_index
				);
			}
			goto loop_truncated;
		}

		/* read frame footer (CRC); kept as little-endian */
		result.z = fread(
			&crc_read, sizeof crc_read, SIZE_C(1), infile
		);
		if UNLIKELY ( result.z != SIZE_C(1) ){
			if UNLIKELY ( ferror(infile) != 0 ){
				error_sys(errno, "fread", infile_name);
			}
			else {	warning_tta("%s: frame %zu: truncated file",
					infile_name, frame_index
				);
			}
loop_truncated:
			crc_read        = 0;
			frame_index_end = frame_index;
		}

		/* decode frame */
		dec_retval = (int8_t) dec_frame_decode(
			&decbuf, priv, &user, samplebytes, nchan,
			ni32_perframe, nbytes_tta_perframe,
			&nsamples_flat_2pad
		);

		/* write PCM to outfile */
		range_write.frame_sample = (
			nsamples_perchan_dec_total - (ni32_perframe / nchan)
		);
		range_write.frame_nsamples = (size_t) (
			ni32_perframe / nchan
		);
		dec_frame_write(
			&decbuf, &dstat, &user, infile_name, outfile,
			outfile_name, samplebytes, nchan, crc_read,
			dec_retval, nsamples_flat_2pad, nbytes_tta_perframe,
			&range_write, frame_index
		);

		frame_index += 1u;
		if ( (! g_flag.quiet) && (frame_index % SPINNER_FREQ == 0) ){
			errprint_spinner();
		}
	}

	/* cleanup */
	priv_free(priv);
	codecbuf_free(&decbuf, CBM_SINGLE_THREADED);

	*dstat_out = dstat;
	return;
}

/**@fn decmt_loop
 * @brief the multi-threaded decoder handler
 *
 * @param seektable    - seektable struct
 * @param dstat_out    - decode stats return struct
 * @param fstat        - bloated file stats struct
 * @param outfile      - destination file
 * @param outfile_name - name of the destination file (warnings/errors)
 * @param infile       - source file
 * @param infile_name  - name of the source file (warnings/errors)
 * @param nthreads     - number of decoder threads to use
 * @param range        - sample-frame interval plan, or NULL for the whole file
 *
 * @see encmt_loop note
**/
BUILD NOINLINE void
decmt_loop(
	const struct SeekTable *const RESTRICT seektable,
	/*@out@*/ struct DecStats *const RESTRICT dstat_out,
	const struct FileStats *const RESTRICT fstat,
	FILE *const RESTRICT outfile, const char *const outfile_name,
	FILE *const RESTRICT infile, const char *const infile_name,
	const unsigned int nthreads,
	const struct SampleRangePlan *const RESTRICT range
)
/*@globals	fileSystem,
		internalState
@*/
/*@modifies	fileSystem,
		internalState,
		*dstat_out,
		outfile,
		infile
@*/
{
	struct MTArg_DecIO io_state;
	struct MTArg_Decoder decoder_state;
	thread_p io_thread, decoder_thread;
	struct FileStats_DecMT fstat_c;
	struct DecStats dstat;
	const size_t        samplebuf_len = fstat->buflen;
	const unsigned int framequeue_len = FRAMEQUEUE_LEN(nthreads);
	unsigned int i;

	assert(nthreads > 0);

	/* setup/init */
	memset(&dstat, 0x00, sizeof dstat);
	if ( (range != NULL) && (range->nframes == 0) ){
		/* empty interval: no frame is read or decoded, and the
			ring-buffer protocol cannot be entered with zero frames
		*/
		*dstat_out = dstat;
		return;
	}
	decmt_fstat_init(&fstat_c, fstat);
	decmt_state_init(
		&io_state, &decoder_state, framequeue_len, samplebuf_len,
		outfile, outfile_name, infile, infile_name, seektable, &dstat,
		&fstat_c, range
	);

	/* create coders */
	thread_create(
		&io_thread,
		(START_ROUTINE_ABI start_routine_ret (*)(void *)) decmt_io,
		&io_state
	);
	for ( i = 0; i < nthreads - 1u; ++i ){
		thread_create(
			&decoder_thread, decmt_decoder_wrapper, &decoder_state
		);
	}
	(void) decmt_decoder(&decoder_state);

	/* wait for i/o */
	thread_join(&io_thread);

	/* cleanup */
	decmt_state_free(&io_state, &decoder_state, framequeue_len);

	*dstat_out = dstat;
	return;
}

/* ======================================================================== */

/**@fn dec_frame_decode
 * @brief decode a TTA frame
 *
 * @param decbuf              - decode buffers struct
 * @param priv                - private state struct
 * @param user_out            - user state return struct
 * @param samplebytes         - number of bytes per PCM sample
 * @param nchan               - number of audio channels
 * @param ni32_perframe total - number of i32 in a TTA frame
 * @param nbytes_tta_perframe - number of TTA bytes in the current frame
 * @param nsamples_flat_2pad  - number of i32 samples to zero-pad
 *
 * @return what libttaR_tta_decode(3) returned
**/
static NOINLINE enum LibTTAr_DecRetVal
dec_frame_decode(
	struct DecBuf *const RESTRICT decbuf,
	/*@reldef@*/ struct LibTTAr_CodecState_Priv *const RESTRICT priv,
	/*@out@*/ struct LibTTAr_CodecState_User *const RESTRICT user_out,
	const enum LibTTAr_SampleBytes samplebytes, const unsigned int nchan,
	size_t ni32_perframe, const size_t nbytes_tta_perframe,
	/*@out@*/ size_t *const RESTRICT nsamples_flat_2pad
)
/*@modifies	*decbuf->i32buf,
		*decbuf->pcmbuf,
		*priv,
		*user_out,
		*nsamples_flat_2pad
@*/
{
	enum LibTTAr_DecRetVal status;
	struct LibTTAr_CodecState_User user = LIBTTAr_CODECSTATE_USER_INIT;
	struct LibTTAr_DecMisc misc;
	size_t pad_target;
	UNUSED union {	size_t z; } result;

	assert(decbuf->i32buf != NULL);

	/* check for truncated sample */
	pad_target     = (ni32_perframe % nchan == 0
		? 0 : (size_t) (nchan - (ni32_perframe % nchan))
	);
	ni32_perframe += pad_target;

	/* decode TTA to I32 */
	misc.dest_len            = decbuf->i32buf_len;
	misc.src_len             = decbuf->ttabuf_len;
	misc.ni32_target         = ni32_perframe;
	misc.nbytes_tta_target   = nbytes_tta_perframe;
	misc.ni32_perframe       = ni32_perframe;
	misc.nbytes_tta_perframe = nbytes_tta_perframe;
	misc.samplebytes         = samplebytes;
	misc.nchan               = nchan;
	/* * */
	status = libttaR_tta_decode(
		decbuf->i32buf, decbuf->ttabuf, priv, &user, &misc
	);
	assert((status == LIBTTAr_DRV_OK_DONE)
	      ||
	       (status == LIBTTAr_DRV_FAIL_DECODE)
	);
	/* with the way the decoding is setup, OK_AGAIN should not happen */

	if UNLIKELY ( status == LIBTTAr_DRV_FAIL_DECODE ){
		pad_target     += ni32_perframe - user.ni32_total;
		user.ni32_total = ni32_perframe;
	}

	/* convert I32 to PCM */
	result.z = libttaR_pcm_write(
		decbuf->pcmbuf, decbuf->i32buf, user.ni32_total, samplebytes
	);
	assert(result.z != 0);

	*user_out           = user;
	*nsamples_flat_2pad = pad_target;
	return status;
}

/**@fn dec_frame_write
 * @brief write a PCM frame
 *
 * @param decbuf              - decode buffers struct
 * @param dstat_out           - decode stats return struct
 * @param user_in             - user state struct
 * @param infile_name         - name of the source file (warnings/errors)
 * @param outfile             - destination file
 * @param outfile_name        - name of the destination file (warnings/errors)
 * @param samplebytes         - number of bytes per PCM sample
 * @param nchan               - number of audio channels
 * @param crc_read            - CRC from source file (little-endian)
 * @param dec_retval          - return value from dec_frame_decode()
 * @param nsamples_flat_2pad  - number of i32 samples to zero-pad
 * @param nbytes_tta_perframe - number of TTA bytes in the current frame
 * @param range_write         - part of the frame to write
 * @param source_frame        - source frame number (diagnostics)
**/
static NOINLINE void
dec_frame_write(
	struct DecBuf *const RESTRICT decbuf,
	/*@in@*/ struct DecStats *const RESTRICT dstat_out,
	struct LibTTAr_CodecState_User *const RESTRICT user_in,
	const char *const RESTRICT infile_name,
	FILE *const RESTRICT outfile, const char *const RESTRICT outfile_name,
	const enum LibTTAr_SampleBytes samplebytes, const unsigned int nchan,
	const uint32_t crc_read /*little-endian*/, const int8_t dec_retval,
	const size_t nsamples_flat_2pad, const size_t nbytes_tta_perframe,
	const struct SampleRangeWrite *const RESTRICT range_write,
	const size_t source_frame
)
/*@globals	fileSystem@*/
/*@modifies	fileSystem,
		*decbuf->pcmbuf,
		*dstat_out,
		outfile
@*/
{
	struct LibTTAr_CodecState_User user = *user_in;
	struct DecStats dstat = *dstat_out;
	struct SampleRangeWrite w = *range_write;
	uint32_t   crc_read_h = byteswap_letoh_u32(crc_read);
	size_t write_offset, write_nsamples;	/* in sample-frames */
	size_t write_flat;
	union { unsigned int	u;
		size_t		z;
	} result;

	/* failure check */
	if UNLIKELY ( dec_retval != (int8_t) LIBTTAr_DRV_OK_DONE ){
		if ( dec_retval == (int8_t) LIBTTAr_DRV_FAIL_DECODE ){
			warning_tta("%s: frame %zu: decoding failed",
				infile_name, source_frame
			);
		}
		else if ( dec_retval == (int8_t) LIBTTAr_DRV_INVAL_TRUNC ){
			warning_tta("%s: frame %zu: last sample truncated, "
				"zero-padding", infile_name, source_frame
			);
		}
		else {	error_tta("%s: frame %zu: unexpected decode status "
				"(%" PRId8 ")",
				infile_name, source_frame, dec_retval
			);
		}
		/* zero-pad */
		dec_frame_zeropad(
			decbuf->pcmbuf, user.ni32_total, nsamples_flat_2pad,
			samplebytes
		);
		/* re-calc CRC */
		user.crc = libttaR_crc32(decbuf->ttabuf, nbytes_tta_perframe);
	}

	/* check frame CRC */
	if UNLIKELY ( user.crc != crc_read_h ){
		warning_tta("%s: frame %zu is corrupted; bad CRC",
			infile_name, source_frame
		);
	}
	user.nbytes_tta_total += sizeof crc_read_h;

	/* select the part of the frame that belongs in the output */
	if ( w.enabled ){
		size_t decoded = (size_t) (user.ni32_total / nchan);
		samplerange_slice(&w, &write_offset, &write_nsamples);
		/* a truncated/short decode can leave fewer samples than
			the frame was expected to hold
		*/
		if ( write_offset > decoded ){
			write_offset = decoded;
		}
		if ( write_nsamples > (decoded - write_offset) ){
			write_nsamples = decoded - write_offset;
		}
		write_flat = (size_t) (write_nsamples * nchan);
	}
	else {	/* untouched whole-file behavior */
		write_offset = 0;
		write_flat   = user.ni32_total;
		write_nsamples = (size_t) (user.ni32_total / nchan);
	}
	if ( write_flat != 0 ){
		/* write frame */
		result.z = fwrite(
			&decbuf->pcmbuf[
				write_offset * nchan * (size_t) samplebytes
			],
			samplebytes, write_flat, outfile
		);
		if UNLIKELY ( result.z != write_flat ){
			error_sys(errno, "fwrite", outfile_name);
		}
	}

	/* update dstat */
	dstat.nframes          += 1u;
	dstat.nsamples_flat    += write_flat;
	dstat.nsamples_perchan += write_nsamples;
	dstat.nbytes_decoded   += user.nbytes_tta_total;

	*dstat_out = dstat;
	return;
}

/**@fn dec_ni32_perframe
 * @brief calculates the total number of i32 in the current TTA frame
 *
 * @param nsamples_dec       - total number of samples of 'nchan' channels
 *   decoded so far
 * @param nsamples_enc total - number of samples of 'nchan' channels encoded
 * @param nsamples_perframe  - number of samples of 'nchan' channels per frame
 * @param nchan              - number of audio channels
 *
 * @return the total number of i32 per TTA frame
**/
CONST
static size_t
dec_ni32_perframe(
	const size_t nsamples_dec, const size_t nsamples_enc,
	const size_t nsamples_perframe, const unsigned int nchan
)
/*@*/
{
	if ( nsamples_dec + nsamples_perframe > nsamples_enc ){
		return (size_t) ((nsamples_enc - nsamples_dec) * nchan);
	}
	else {	return (size_t) (nsamples_perframe * nchan); }
}

/**@fn dec_frame_zeropad
 * @brief zero-pads the PCM buffer (truncated last sample)
 *
 * @param pcmbuf               - PCM buffer
 * @param pcmbuf_nsamples_flat - to number of samples in the PCM buffer
 * @param nsamples_flat_2pad   - total number of samples to pad
 * @param samplebytes          - number of bytes per PCM sample
**/
COLD
static NOINLINE void
dec_frame_zeropad(
	uint8_t *const RESTRICT pcmbuf, const size_t pcmbuf_nsamples_flat,
	const size_t nsamples_flat_2pad,
	const enum LibTTAr_SampleBytes samplebytes
)
/*@modifies	*pcmbuf@*/
{
	const size_t idx     = (size_t) (
		(pcmbuf_nsamples_flat - nsamples_flat_2pad) * samplebytes
	);
	const size_t padsize = (size_t) (nsamples_flat_2pad * samplebytes);

	memset(&pcmbuf[idx], 0x00, padsize);

	return;
}

/* ======================================================================== */

/**@fn decmt_io
 * @brief the I/O thread function for the multi-threaded decoder
 *
 * @param arg - state for the thread
 *
 * @pre arg->frames->nmemb > the number of decoder threads
 *
 * @retval (start_routine_ret) 0
**/
HOT
START_ROUTINE_ABI
static start_routine_ret
decmt_io(struct MTArg_DecIO *const RESTRICT arg)
/*@globals	fileSystem,
		internalState
@*/
/*@modifies	fileSystem,
		internalState,
		*arg->frames.navailable,
		*arg->frames.post_decoder,
		*arg->frames.ni32_perframe,
		*arg->frames.decbuf,
		*arg->frames.crc_read,
		arg->outfile.handle,
		arg->infile.handle,
		*arg->seektable,
		*arg->dstat_out
@*/
{
	struct MTArg_DecIO_Frames *const RESTRICT  frames  = &arg->frames;
	struct MTArg_IO_File      *const RESTRICT  outfile = &arg->outfile;
	struct MTArg_IO_File      *const RESTRICT  infile  = &arg->infile;
	const struct FileStats_DecMT *const RESTRICT fstat =  arg->fstat;
	const struct SeekTable *const RESTRICT seektable   =  arg->seektable;
	const struct SampleRangePlan *const RESTRICT range = arg->range;
	/* * */
	semaphore_p   *const RESTRICT nframes_avail = frames->navailable;
	semaphore_p   *const RESTRICT post_decoder  = frames->post_decoder;
	size_t        *const RESTRICT ni32_perframe = frames->ni32_perframe;
	size_t        *const RESTRICT nbytes_tta_perframe  = (
		frames->nbytes_tta_perframe
	);
	struct DecBuf *const RESTRICT decbuf             = frames->decbuf;
	struct LibTTAr_CodecState_User *const RESTRICT user = frames->user;
	size_t        *const RESTRICT nsamples_flat_2pad = (
		frames->nsamples_flat_2pad
	);
	int8_t     *const RESTRICT dec_retval     = frames->dec_retval;
	uint32_t   *const RESTRICT crc_read       = frames->crc_read;
	struct SampleRangeWrite *const RESTRICT range_write = frames->range_write;
	size_t     *const RESTRICT frame_index    = frames->frame_index;
	/* * */
	FILE       *const RESTRICT outfile_handle = outfile->handle;
	const char *const RESTRICT outfile_name   = outfile->name;
	FILE       *const RESTRICT infile_handle  = infile->handle;
	const char *const RESTRICT infile_name    = infile->name;
	/* * */
	const unsigned int framequeue_len          = frames->nmemb;
	const unsigned int nchan                   = fstat->nchan;
	const enum LibTTAr_SampleBytes samplebytes = fstat->samplebytes;
	const size_t nsamples_perframe             = fstat->nsamples_perframe;
	const size_t nsamples_enc                  = fstat->nsamples_enc;
	/* * */
	struct DecStats dstat = *arg->dstat_out;
	size_t framesize_tta, nbytes_read;
	size_t nsamples_perchan_dec_total = 0;
	size_t frame_curr, frame_curr_end;
	bool   start_writing  = false;
	unsigned int idx = 0, last;
	union {	unsigned int	u;
		size_t		z;
	} result;

	/* the interval selects which frames are read; the counters derive
		from the source frame numbering
	*/
	if ( range != NULL ){
		frame_curr     = range->first_frame;
		frame_curr_end = range->first_frame + range->nframes;
		nsamples_perchan_dec_total = (
			range->first_frame * nsamples_perframe
		);
		if ( nsamples_perchan_dec_total > nsamples_enc ){
			nsamples_perchan_dec_total = nsamples_enc;
		}
	}
	else {	frame_curr     = 0;
		frame_curr_end = seektable->nmemb;
	}

	goto loop0_entr;
	do {
		/* xENCFMT_TTA1
			- get size of tta-frame from seektable
		*/
		framesize_tta = (size_t) byteswap_letoh_u32(
			seektable->table[frame_curr]
		);
		if ( framesize_tta <= (sizeof *crc_read) ){
			warning_tta(
				"%s: frame %zu: malformed seektable entry",
				infile_name, frame_curr
			);
			nbytes_tta_perframe[idx] = 0;
			break;
		}
		else {	framesize_tta -= (sizeof *crc_read); }
		ni32_perframe[idx] = dec_ni32_perframe(
			nsamples_perchan_dec_total, nsamples_enc,
			nsamples_perframe, nchan
		);
		if ( ni32_perframe[idx] == 0 ){
			/* malformed seektable check */
			break;
		}
		nsamples_perchan_dec_total += ni32_perframe[idx] / nchan;

		/* remember the source frame number and what to write */
		frame_index[idx] = frame_curr;
		memset(&range_write[idx], 0x00, sizeof range_write[idx]);
		if ( range != NULL ){
			range_write[idx].enabled = 1;
			range_write[idx].start   = range->start;
			range_write[idx].end     = range->end;
		}
		range_write[idx].frame_sample = (
			nsamples_perchan_dec_total - (ni32_perframe[idx] / nchan)
		);
		range_write[idx].frame_nsamples = (size_t) (
			ni32_perframe[idx] / nchan
		);

		/* read TTA from infile */
		decbuf_check_adjust(
			&decbuf[idx], framesize_tta, nchan, samplebytes
		);
		nbytes_read = fread(
			decbuf[idx].ttabuf, SIZE_C(1), framesize_tta,
			infile_handle
		);
		nbytes_tta_perframe[idx] = nbytes_read;
		if UNLIKELY ( nbytes_read != framesize_tta ){
			if UNLIKELY ( ferror(infile_handle) != 0 ){
				error_sys(errno, "fread", infile_name);
			}
			else {	warning_tta("%s: frame %zu: truncated file",
					infile_name, frame_curr
				);
			}
			goto loop0_truncated;
		}

		/* read frame footer (crc); kept as little-endian */
		result.z = fread(
			&crc_read[idx], sizeof *crc_read, SIZE_C(1),
			infile_handle
		);
		if UNLIKELY ( result.z != SIZE_C(1) ){
			if UNLIKELY ( ferror(infile_handle) != 0 ){
				error_sys(errno, "fread", infile_name);
			}
			else {	warning_tta("%s: frame %zu: truncated file",
					infile_name, frame_curr
				);
			}
loop0_truncated:
			crc_read[idx]  = 0;
			frame_curr_end = frame_curr;
		}

		/* make frame available */
		semaphore_post(nframes_avail);

		frame_curr += 1u;
		idx = pqueue_next(idx, framequeue_len);
		/* all frame ttabuf's are filled before writing out any PCM */
		if ( ! start_writing ){
			if ( idx != 0 ){
				goto loop0_read;
			}
			start_writing = true;
		}

		/* wait for frame to finish decoding */
		semaphore_wait(&post_decoder[idx]);

		/* write pcm to outfile */
		dec_frame_write(
			&decbuf[idx], &dstat, &user[idx], infile_name,
			outfile_handle, outfile_name, samplebytes, nchan,
			crc_read[idx], dec_retval[idx],
			nsamples_flat_2pad[idx], nbytes_tta_perframe[idx],
			&range_write[idx], frame_index[idx]
		);
loop0_entr:
		if ( (! g_flag.quiet) && (frame_curr % SPINNER_FREQ == 0) ){
			errprint_spinner();
		}
loop0_read:
		;
	}
	while ( frame_curr < frame_curr_end );
	nbytes_tta_perframe[idx] = 0;
	last = idx;

	/* write the remaining frames */
	if ( start_writing ){
		goto loop1_not_tiny;
	}
	else {	/* unlock any uninitialized frames (tiny infile) */
		do {	semaphore_post(nframes_avail);
			idx = pqueue_next(idx, framequeue_len);
		}
		while ( idx != 0 );
	}
	do {	/* wait for frame to finish encoding */
		semaphore_wait(&post_decoder[idx]);

		/* write pcm to outfile */
		dec_frame_write(
			&decbuf[idx], &dstat, &user[idx], infile_name,
			outfile_handle, outfile_name, samplebytes, nchan,
			crc_read[idx], dec_retval[idx],
			nsamples_flat_2pad[idx], nbytes_tta_perframe[idx],
			&range_write[idx], frame_index[idx]
		);

		/* mark frame as done and make available */
		nbytes_tta_perframe[idx] = 0;
		semaphore_post(nframes_avail);
loop1_not_tiny:
		idx = pqueue_next(idx, framequeue_len);
	}
	while ( idx != last );

	*arg->dstat_out = dstat;
	return (start_routine_ret) 0;
}

/**@fn decmt_decoder_wrapper
 * @brief wraps the mt-decoder function. the thread detaches itself before
 *   returning
 *
 * @param arg - state for the thread
 *
 * @return whatever decmt_decoder() returns
**/
START_ROUTINE_ABI
static start_routine_ret
decmt_decoder_wrapper(void *const RESTRICT arg)
/*@globals	fileSystem,
		internalState
@*/
/*@modifies	fileSystem,
		internalState,
		*arg
@*/
{
	start_routine_ret retval;

	retval = decmt_decoder(arg);

	thread_detach_self();

	return retval;
}

/**@fn decmt_decoder
 * @brief the decoder thread function for the multi-threaded decoder
 *
 * @param arg - state for the thread
 *
 * @return (start_routine_ret) 0
**/
HOT
static start_routine_ret
decmt_decoder(struct MTArg_Decoder *const RESTRICT arg)
/*@globals	fileSystem,
		internalState
@*/
/*@modifies	fileSystem,
		internalState,
		*arg->frames.navailable,
		arg->frames.queue,
		*arg->frames.post_decoder,
		*arg->frames.decbuf,
		*arg->frames.dec_retval
@*/
{
	struct MTArg_Decoder_Frames  *const RESTRICT frames = &arg->frames;
	const struct FileStats_DecMT *const RESTRICT fstat  =  arg->fstat;
	/* * */
	struct MTPQueue *const RESTRICT queue      = &frames->queue;
	semaphore_p  *const RESTRICT nframes_avail =  frames->navailable;
	semaphore_p  *const RESTRICT post_decoder  =  frames->post_decoder;
	const size_t *const RESTRICT ni32_perframe =  frames->ni32_perframe;
	const size_t *const RESTRICT nbytes_tta_perframe    = (
		frames->nbytes_tta_perframe
	);
	struct DecBuf *const RESTRICT decbuf             = frames->decbuf;
	struct LibTTAr_CodecState_User *const RESTRICT user = frames->user;
	int8_t        *const RESTRICT dec_retval         = frames->dec_retval;
	size_t        *const RESTRICT nsamples_flat_2pad = (
		frames->nsamples_flat_2pad
	);
	/* * */
	const unsigned int nchan                   = fstat->nchan;
	const enum LibTTAr_SampleBytes samplebytes = fstat->samplebytes;
	/* * */
	int32_t *i32buf = NULL;
	struct LibTTAr_CodecState_Priv *priv = NULL;
	unsigned int idx;

	/* setup */
	i32buf = calloc_check(decbuf[0].i32buf_len, sizeof *i32buf);
	priv   = priv_alloc(nchan);

	goto loop_entr;
	do {
		/* decode frame */
		decbuf[idx].i32buf = i32buf;
		dec_retval[idx] = (int8_t) dec_frame_decode(
			&decbuf[idx], priv, &user[idx], samplebytes, nchan,
			ni32_perframe[idx], nbytes_tta_perframe[idx],
			&nsamples_flat_2pad[idx]
		);

		/* unlock frame */
		semaphore_post(&post_decoder[idx]);
loop_entr:
		/* wait for a frame to be available */
		semaphore_wait(nframes_avail);

		/* get frame id from decode queue */
		spinlock_lock(&queue->lock);
		{
			idx = pqueue_pop(&queue->q);
		}
		spinlock_unlock(&queue->lock);
	}
	while ( nbytes_tta_perframe[idx] != 0 );

	/* cleanup */
	priv_free(priv);
	free(i32buf);

	return (start_routine_ret) 0;
}

/* EOF //////////////////////////////////////////////////////////////////// */
