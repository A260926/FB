#ifndef H_TTA_SAMPLE_RANGE_H
#define H_TTA_SAMPLE_RANGE_H
/* ///////////////////////////////////////////////////////////////////////////
//                                                                          //
// sample_range.h                                                           //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// Copyright (C) 2023-2025, Shane Seelig                                    //
// SPDX-License-Identifier: GPL-3.0-or-later                                //
//                                                                          //
/////////////////////////////////////////////////////////////////////////// */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "./common.h"

/* //////////////////////////////////////////////////////////////////////// */

/* zero-based half-open range of per-channel sample frames */
struct SampleRange {
	size_t	start;
	size_t	end;
	bool	isset;
};

/* a run of consecutive (source-order) compressed frames to decode */
struct FrameSpan {
	size_t	first;		/* first frame that intersects the range */
	size_t	nmemb;		/* number of intersecting frames          */
	size_t	start;		/* absolute per-channel range start       */
	size_t	end;		/* absolute per-channel range end         */
};

/* //////////////////////////////////////////////////////////////////////// */

/**@fn sample_range_parse
 * @brief parse a "START:END" argument into a sample range
 *
 * @param range   - destination range struct
 * @param subopt  - the option argument string
 * @param optname - option name (for errors)
 *
 * @note exits on any malformed argument (overflow, negative, extra chars,
 *   or START > END)
**/
BUILD_EXTERN void sample_range_parse(
	/*@out@*/ struct SampleRange *RESTRICT,
	const char *RESTRICT, const char *RESTRICT
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
;

/**@fn sample_range_validate
 * @brief check a parsed range against a file's declared sample count
 *
 * @param range       - sample range
 * @param nsamples    - total number of sample frames in the file
 * @param infile_name - name of the source file (for errors)
 *
 * @note exits if END > nsamples
**/
BUILD_EXTERN void sample_range_validate(
	const struct SampleRange *RESTRICT, size_t, const char *RESTRICT
)
/*@globals	fileSystem,
		g_nwarnings
@*/
/*@modifies	fileSystem,
		g_nwarnings
@*/
;

/**@fn sample_range_span
 * @brief calculate which compressed frames intersect a range
 *
 * @param span     - destination span struct
 * @param range    - sample range
 * @param framelen - number of sample frames per TTA frame
 * @param nframes  - number of compressed frames in the seektable
**/
BUILD_EXTERN void sample_range_span(
	/*@out@*/ struct FrameSpan *RESTRICT,
	const struct SampleRange *RESTRICT, size_t, size_t
)
/*@modifies	*span@*/
;

/* EOF //////////////////////////////////////////////////////////////////// */
#endif	/* H_TTA_SAMPLE_RANGE_H */
