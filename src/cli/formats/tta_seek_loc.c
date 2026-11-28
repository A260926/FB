/* ///////////////////////////////////////////////////////////////////////////
//                                                                          //
// formats/tta_seek_loc.c                                                   //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// Copyright (C) 2023-2025, Shane Seelig                                    //
// SPDX-License-Identifier: GPL-3.0-or-later                                //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
//  http://tausoft.org/wiki/True_Audio_Codec_Format                         //
//                                                                          //
/////////////////////////////////////////////////////////////////////////// */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "../byteswap.h"
#include "../common.h"
#include "../formats.h"

/* //////////////////////////////////////////////////////////////////////// */

/* largest value representable by off_t (off_t is signed) */
#define TTA_OFF_MAX	((off_t) (sizeof(off_t) >= sizeof(int64_t) \
	? (uint64_t) INT64_MAX : (uint64_t) INT32_MAX))

/* //////////////////////////////////////////////////////////////////////// */

/**@fn seektable_span_locate
 * @brief locate the first frame of a span and validate the whole span
 *
 * @param start_off    - absolute offset of the first frame's payload
 * @param first_nbytes - size of the first frame's payload (no footer)
 * @param st           - seektable struct
 * @param first        - index of the first frame
 * @param nmemb        - number of frames in the span (may be 0)
 * @param data_off     - absolute offset of the first compressed frame
 *
 * @return FILECHECK_OK on success
 * @retval FILECHECK_MALFORMED a seektable entry is too small
 * @retval FILECHECK_SEEK_ERROR a file offset cannot be represented
 *
 * @note every entry is the cumulative size of all preceding frames
 *   (including each frame's 4-byte footer). Accumulation uses an explicit
 *   overflow check so it never wraps, and every selected entry is validated
 *   before any audio is written.
**/
BUILD enum FileCheck
seektable_span_locate(
	/*@out@*/ off_t *const RESTRICT start_off,
	/*@out@*/ size_t *const RESTRICT first_nbytes,
	const struct SeekTable *const RESTRICT st, const size_t first,
	const size_t nmemb, const off_t data_off
)
/*@globals	internalState@*/
/*@modifies	internalState,
		*start_off,
		*first_nbytes
@*/
{
	uint64_t cumulative = 0;
	uint64_t nbytes;
	size_t i;

	assert(st->nmemb >= first);
	assert(st->nmemb - first >= nmemb);

	if ( data_off < 0 ){
		return FILECHECK_SEEK_ERROR;
	}
	if ( nmemb == 0 ){
		*start_off    = data_off;
		*first_nbytes = 0;
		return FILECHECK_OK;
	}

	/* sum the frames that precede the span */
	for ( i = 0; i < first; ++i ){
		nbytes = (uint64_t) byteswap_letoh_u32(st->table[i]);
		if UNLIKELY ( nbytes < (uint64_t) sizeof(uint32_t) ){
			return FILECHECK_MALFORMED;
		}
		if UNLIKELY ( cumulative > (UINT64_MAX - nbytes) ){
			return FILECHECK_SEEK_ERROR;
		}
		cumulative += nbytes;
	}
	if UNLIKELY (
	     ((uint64_t) data_off > (uint64_t) TTA_OFF_MAX)
	    ||
	     (cumulative > (uint64_t) (TTA_OFF_MAX - data_off))
	){
		return FILECHECK_SEEK_ERROR;
	}
	*start_off = data_off + (off_t) cumulative;

	/* validate every selected entry and its (representable) offset */
	for ( i = 0; i < nmemb; ++i ){
		const size_t idx = first + i;

		nbytes = (uint64_t) byteswap_letoh_u32(st->table[idx]);
		if UNLIKELY ( nbytes <= (uint64_t) sizeof(uint32_t) ){
			return FILECHECK_MALFORMED;
		}
		if UNLIKELY (
		     (cumulative > (UINT64_MAX - nbytes))
		    ||
		     (cumulative + nbytes
		      > (uint64_t) (TTA_OFF_MAX - data_off))
		){
			return FILECHECK_SEEK_ERROR;
		}
		if ( i == 0 ){
			*first_nbytes = (size_t) (nbytes - sizeof(uint32_t));
		}
		cumulative += nbytes;
	}

	return FILECHECK_OK;
}

/* EOF //////////////////////////////////////////////////////////////////// */
