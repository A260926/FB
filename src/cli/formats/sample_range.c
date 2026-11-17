/* ///////////////////////////////////////////////////////////////////////////
//                                                                          //
// formats/sample_range.c                                                   //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// Copyright (C) 2023-2025, Shane Seelig                                    //
// SPDX-License-Identifier: GPL-3.0-or-later                                //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
//  Parsing and resolution for the decode option '--samples=START:END'.     //
//                                                                          //
/////////////////////////////////////////////////////////////////////////// */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../byteswap.h"
#include "../common.h"
#include "../formats.h"

#include "./sample_range.h"

/* //////////////////////////////////////////////////////////////////////// */

CONST
static uintmax_t samplerange_max_off(void) /*@*/;

static enum SampleRangeParseStatus samplerange_parse_uint(
	/*@out@*/ size_t *RESTRICT, const char *RESTRICT, size_t
)
/*@modifies	*@*/
;

/* //////////////////////////////////////////////////////////////////////// */

/**@fn samplerange_parse_uint
 * @brief parses a non-negative decimal integer
 *
 * @param out - parsed value
 * @param str - string to parse
 * @param len - length of 'str'
 *
 * @retval SAMPLERANGE_PARSE_OK
 * @retval SAMPLERANGE_PARSE_SYNTAX    - empty or non-digit character
 * @retval SAMPLERANGE_PARSE_OVERFLOW  - value does not fit in size_t
**/
static enum SampleRangeParseStatus
samplerange_parse_uint(
	/*@out@*/ size_t *const RESTRICT out, const char *const str,
	const size_t len
)
/*@modifies	*out@*/
{
	size_t value = 0;
	size_t i;

	if ( len == 0 ){
		return SAMPLERANGE_PARSE_SYNTAX;
	}
	for ( i = 0; i < len; ++i ){
		const char c = str[i];
		size_t digit;

		if ( (c < '0') || (c > '9') ){
			return SAMPLERANGE_PARSE_SYNTAX;
		}
		digit = (size_t) (c - '0');
		if ( value > (SIZE_MAX - digit) / SIZE_C(10) ){
			return SAMPLERANGE_PARSE_OVERFLOW;
		}
		value = (value * SIZE_C(10)) + digit;
	}
	*out = value;
	return SAMPLERANGE_PARSE_OK;
}

/**@fn samplerange_parse_string
 * @brief parses a "START:END" sample-frame interval
 *
 * @param str - string to parse
 * @param opt - parsed interval
 *
 * @retval SAMPLERANGE_PARSE_OK
 * @retval SAMPLERANGE_PARSE_SYNTAX   - empty field, extra ':' or non-digit
 * @retval SAMPLERANGE_PARSE_OVERFLOW - a field does not fit in size_t
 * @retval SAMPLERANGE_PARSE_ORDER    - START > END
**/
BUILD enum SampleRangeParseStatus
samplerange_parse_string(
	const char *const RESTRICT str,
	/*@out@*/ struct SampleRangeOpt *const RESTRICT opt
)
/*@modifies	*opt@*/
{
	const char *sep;
	size_t len0, len1;
	enum SampleRangeParseStatus status;
	size_t start, end;

	sep = strchr(str, ':');
	if ( sep == NULL ){
		return SAMPLERANGE_PARSE_SYNTAX;
	}
	/* exactly one ':' */
	if ( strchr(&sep[1], ':') != NULL ){
		return SAMPLERANGE_PARSE_SYNTAX;
	}
	len0 = (size_t) (sep - str);
	len1 = strlen(&sep[1u]);

	status = samplerange_parse_uint(&start, str, len0);
	if ( status != SAMPLERANGE_PARSE_OK ){
		return status;
	}
	status = samplerange_parse_uint(&end, &sep[1u], len1);
	if ( status != SAMPLERANGE_PARSE_OK ){
		return status;
	}
	if ( start > end ){
		return SAMPLERANGE_PARSE_ORDER;
	}

	opt->enabled = 1;
	opt->start   = start;
	opt->end     = end;
	return SAMPLERANGE_PARSE_OK;
}

/* ======================================================================== */

/**@fn samplerange_plan
 * @brief resolves a sample-frame interval against one TTA file's seektable
 *
 * @param plan  - resolved work plan
 * @param opt   - sample-frame interval
 * @param fstat - bloated file stats struct
 * @param st    - seektable struct
 *
 * @retval SAMPLERANGE_PLAN_OK
 * @retval SAMPLERANGE_PLAN_RANGE     - interval outside the declared samples
 * @retval SAMPLERANGE_PLAN_SEEKTABLE - malformed/unusable seektable
 * @retval SAMPLERANGE_PLAN_OFFSET    - frame offset not representable
 *
 * @note only the seektable entries preceding (and belonging to) the selected
 *   frame interval are inspected; no compressed frame payload is read here
**/
BUILD enum SampleRangePlanStatus
samplerange_plan(
	/*@out@*/ struct SampleRangePlan *const RESTRICT plan,
	const struct SampleRangeOpt *const RESTRICT opt,
	const struct FileStats *const RESTRICT fstat,
	const struct SeekTable *const RESTRICT st
)
/*@globals	internalState@*/
/*@modifies	internalState,
		*plan
@*/
{
	const size_t framelen   = fstat->framelen;
	const size_t nsamples   = fstat->nsamples_enc;
	const uintmax_t max_off = samplerange_max_off();
	/* * */
	size_t total_frames, first_frame, last_frame, nframes;
	uintmax_t data_start, offset;
	size_t i;

	if ( framelen == 0 ){
		return SAMPLERANGE_PLAN_SEEKTABLE;
	}
	if ( opt->end > nsamples ){
		return SAMPLERANGE_PLAN_RANGE;
	}
	if ( opt->start > opt->end ){
		return SAMPLERANGE_PLAN_RANGE;
	}

	total_frames = (nsamples / framelen)
		+ (size_t) (nsamples % framelen != 0);
	if ( st->nmemb < total_frames ){
		return SAMPLERANGE_PLAN_SEEKTABLE;
	}

	first_frame = opt->start / framelen;
	if ( opt->start == opt->end ){
		/* empty interval: no frame intersects it */
		last_frame = first_frame;
	}
	else {	last_frame = ((opt->end - 1u) / framelen) + 1u; }
	if ( (last_frame < first_frame) || (first_frame > st->nmemb) ){
		return SAMPLERANGE_PLAN_SEEKTABLE;
	}
	nframes = last_frame - first_frame;

	/* accumulate the encoded lengths that precede the first selected frame;
		every entry used here is validated first
	*/
	offset = 0;
	for ( i = 0; i < first_frame; ++i ){
		const size_t frame_size = (size_t) byteswap_letoh_u32(
			st->table[i]
		);
		if ( frame_size <= sizeof(uint32_t) ){
			return SAMPLERANGE_PLAN_SEEKTABLE;
		}
		if ( offset > (UINTMAX_MAX - (uintmax_t) frame_size) ){
			return SAMPLERANGE_PLAN_OFFSET;
		}
		offset += (uintmax_t) frame_size;
	}
	/* the selected frames must be usable, too */
	for ( i = first_frame; i < last_frame; ++i ){
		const size_t frame_size = (size_t) byteswap_letoh_u32(
			st->table[i]
		);
		if ( frame_size <= sizeof(uint32_t) ){
			return SAMPLERANGE_PLAN_SEEKTABLE;
		}
	}

	/* data starts after the header, the seektable, and the table CRC */
	if ( fstat->enctta_off < 0 ){
		return SAMPLERANGE_PLAN_OFFSET;
	}
	if ( (uintmax_t) st->nmemb > (
		(UINTMAX_MAX - sizeof(uint32_t)) / sizeof(uint32_t)
	     )
	){
		return SAMPLERANGE_PLAN_OFFSET;
	}
	data_start = (uintmax_t) fstat->enctta_off;
	data_start += (uintmax_t) st->nmemb * sizeof(uint32_t);
	data_start += sizeof(uint32_t);
	if ( data_start > max_off ){
		return SAMPLERANGE_PLAN_OFFSET;
	}
	if ( offset > (max_off - data_start) ){
		return SAMPLERANGE_PLAN_OFFSET;
	}

	plan->first_frame = first_frame;
	plan->nframes     = nframes;
	plan->start       = opt->start;
	plan->end         = opt->end;
	plan->data_off    = (off_t) (data_start + offset);
	return SAMPLERANGE_PLAN_OK;
}

/**@fn samplerange_max_off
 * @brief largest positive value representable by off_t
**/
CONST
static uintmax_t
samplerange_max_off(void)
/*@*/
{
	const unsigned int bits = (unsigned int) (sizeof(off_t) * CHAR_BIT);

	if ( bits >= (unsigned int) (sizeof(uintmax_t) * CHAR_BIT) ){
		return UINTMAX_MAX;
	}
	return ((uintmax_t) (SIZE_C(1) << (bits - 1u))) - UINTMAX_C(1);
}

/* EOF //////////////////////////////////////////////////////////////////// */
