/* ///////////////////////////////////////////////////////////////////////////
//                                                                          //
// sample_range.c                                                           //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// Copyright (C) 2023-2025, Shane Seelig                                    //
// SPDX-License-Identifier: GPL-3.0-or-later                                //
//                                                                          //
/////////////////////////////////////////////////////////////////////////// */

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "./common.h"
#include "./debug.h"
#include "./main.h"
#include "./sample_range.h"

/* //////////////////////////////////////////////////////////////////////// */

#undef range
static void sample_range_parse_field(
	/*@out@*/ size_t *RESTRICT range, const char *RESTRICT,
	const char *RESTRICT, size_t
)
/*@modifies	*range@*/
;

/* //////////////////////////////////////////////////////////////////////// */

/**@fn sample_range_parse
 * @brief parse a "START:END" argument into a sample range
 *
 * @param range   - destination range struct
 * @param subopt  - the option argument string
 * @param optname - option name (errors)
**/
BUILD void
sample_range_parse(
	/*@out@*/ struct SampleRange *const RESTRICT range,
	const char *const RESTRICT subopt, const char *const RESTRICT optname
)
/*@globals	fileSystem,
		internalState,
		g_flag
@*/
/*@modifies	fileSystem,
		internalState,
		g_flag,
		*range
@*/
{
	const char *colon;
	size_t len;

	assert(subopt != NULL);
	assert(optname != NULL);

	colon = strchr(subopt, ':');
	if UNLIKELY ( colon == NULL ){
		error_tta("%s: bad argument: %s (expected START:END)",
			optname, subopt
		);
	}
	len = strlen(subopt);

	(void) sample_range_parse_field(
		&range->start, subopt, optname, (size_t) (colon - subopt)
	);
	(void) sample_range_parse_field(
		&range->end, &colon[1u], optname,
		(size_t) (len - (size_t) (colon - subopt) - 1u)
	);

	if UNLIKELY ( range->start > range->end ){
		error_tta("%s: bad argument: %s (START > END)",
			optname, subopt
		);
	}
	range->isset = true;

	return;
}

/**@fn sample_range_validate
 * @brief check a parsed range against a file's declared sample count
 *
 * @param range       - sample range
 * @param nsamples    - total number of sample frames in the file
 * @param infile_name - name of the source file (errors)
**/
BUILD void
sample_range_validate(
	const struct SampleRange *const RESTRICT range,
	const size_t nsamples, const char *const RESTRICT infile_name
)
/*@globals	fileSystem,
		g_nwarnings
@*/
/*@modifies	fileSystem,
		g_nwarnings
@*/
{
	if UNLIKELY ( range->end > nsamples ){
		error_tta(
			"%s: sample range end (%zu) exceeds total samples (%zu)",
			infile_name, range->end, nsamples
		);
	}
	return;
}

/**@fn sample_range_span
 * @brief calculate which compressed frames intersect a range
 *
 * @param span     - destination span struct
 * @param range    - sample range
 * @param framelen - number of sample frames per TTA frame
 * @param nframes  - number of compressed frames in the seektable
**/
BUILD void
sample_range_span(
	/*@out@*/ struct FrameSpan *const RESTRICT span,
	const struct SampleRange *const RESTRICT range,
	const size_t framelen, const size_t nframes
)
/*@modifies	*span@*/
{
	size_t start_frame, end_frame;

	if ( range->start == range->end ){
		span->first = 0;
		span->nmemb = 0;
		span->start = range->start;
		span->end   = range->end;
		return;
	}

	assert(framelen != 0);
	start_frame = range->start / framelen;
	end_frame   = (range->end - 1u) / framelen;
	assert(start_frame <= end_frame);

	/* clamp to the frames that actually exist */
	if UNLIKELY ( start_frame >= nframes ){
		span->first = 0;
		span->nmemb = 0;
		span->start = range->start;
		span->end   = range->end;
		return;
	}
	if ( end_frame >= nframes ){
		end_frame = (nframes != 0 ? nframes - 1u : 0);
	}

	span->first = start_frame;
	span->nmemb = (end_frame - start_frame) + 1u;
	span->start = range->start;
	span->end   = range->end;

	return;
}

/**@fn sample_range_parse_field
 * @brief parse one decimal non-negative field
 *
 * @param out     - parsed value
 * @param str     - start of the field
 * @param optname - option name (errors)
 * @param len     - number of characters in the field
**/
static void
sample_range_parse_field(
	/*@out@*/ size_t *const RESTRICT out,
	const char *const RESTRICT str, const char *const RESTRICT optname,
	const size_t len
)
/*@modifies	*out@*/
{
	size_t value = 0;
	size_t i;

	if UNLIKELY ( len == 0u ){
		error_tta("%s: bad argument (empty field)", optname);
	}
	for ( i = 0; i < len; ++i ){
		const unsigned char c = (unsigned char) str[i];

		if UNLIKELY ( (c < (unsigned char) '0')
			   || (c > (unsigned char) '9') ){
			error_tta("%s: bad argument: %s", optname, str);
		}
		/* overflow: value * 10 + digit must fit in size_t */
		if UNLIKELY (
		     (value > (SIZE_MAX / SIZE_C(10)))
		    ||
		     ((value == (SIZE_MAX / SIZE_C(10)))
		      && ((size_t) (c - (unsigned char) '0') >
			  (SIZE_MAX % SIZE_C(10))))
		){
			error_tta("%s: argument out of range: %s",
				optname, str
			);
		}
		value = (value * SIZE_C(10)) + (size_t) (c - (unsigned char) '0');
	}
	*out = value;

	return;
}

/* EOF //////////////////////////////////////////////////////////////////// */
