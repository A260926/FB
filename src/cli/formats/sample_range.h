#ifndef H_TTA_FORMATS_SAMPLE_RANGE_H
#define H_TTA_FORMATS_SAMPLE_RANGE_H
/* ///////////////////////////////////////////////////////////////////////////
//                                                                          //
// formats/sample_range.h                                                   //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// Copyright (C) 2023-2025, Shane Seelig                                    //
// SPDX-License-Identifier: GPL-3.0-or-later                                //
//                                                                          //
/////////////////////////////////////////////////////////////////////////// */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "../common.h"

/* //////////////////////////////////////////////////////////////////////// */

struct FileStats;
struct SeekTable;

/* zero-based, half-open sample-frame interval: [start, end) */
struct SampleRangeOpt {
	int	enabled;
	size_t	start;
	size_t	end;
};

/* the work required to decode an interval, resolved for one TTA file */
struct SampleRangePlan {
	size_t	first_frame;	/* index of the first intersecting frame */
	size_t	nframes;	/* number of intersecting frames           */
	size_t	start;		/* copy of SampleRangeOpt.start            */
	size_t	end;		/* copy of SampleRangeOpt.end              */
	off_t	data_off;	/* file offset of 'first_frame'            */
};

enum SampleRangeParseStatus {
	SAMPLERANGE_PARSE_OK = 0,
	SAMPLERANGE_PARSE_SYNTAX,
	SAMPLERANGE_PARSE_OVERFLOW,
	SAMPLERANGE_PARSE_ORDER
};

enum SampleRangePlanStatus {
	SAMPLERANGE_PLAN_OK = 0,
	SAMPLERANGE_PLAN_RANGE,		/* outside the file's declared samples */
	SAMPLERANGE_PLAN_SEEKTABLE,	/* malformed/unusable seektable        */
	SAMPLERANGE_PLAN_OFFSET		/* frame offset not representable      */
};

/* the part of one decoded frame that belongs in the output */
struct SampleRangeWrite {
	int	enabled;
	size_t	start;		/* interval start (per-channel samples) */
	size_t	end;		/* interval end   (per-channel samples) */
	size_t	frame_sample;	/* source sample-frame of this frame    */
	size_t	frame_nsamples;	/* source sample-frames in this frame   */
};

/* //////////////////////////////////////////////////////////////////////// */

/**@fn samplerange_slice
 * @brief computes the part of a decoded frame to write
 *
 * @param w        - per-frame write description
 * @param offset   - [out] first sample-frame of the frame to write
 * @param nsamples - [out] number of sample-frames to write
**/
INLINE void
samplerange_slice(
	const struct SampleRangeWrite *const RESTRICT w,
	/*@out@*/ size_t *const RESTRICT offset,
	/*@out@*/ size_t *const RESTRICT nsamples
)
/*@modifies	*offset,
		*nsamples
@*/
{
	size_t lo, hi, frame_end;

	if ( ! w->enabled ){
		*offset   = 0;
		*nsamples = w->frame_nsamples;
		return;
	}
	frame_end = (size_t) (w->frame_sample + w->frame_nsamples);
	lo = (w->start > w->frame_sample ? w->start : w->frame_sample);
	hi = (w->end < frame_end ? w->end : frame_end);
	if ( hi < lo ){
		hi = lo;
	}
	*offset   = (size_t) (lo - w->frame_sample);
	*nsamples = (size_t) (hi - lo);

	return;
}

/* ------------------------------------------------------------------------ */

#undef opt
BUILD_EXTERN enum SampleRangeParseStatus samplerange_parse_string(
	const char *RESTRICT, /*@out@*/ struct SampleRangeOpt *RESTRICT opt
)
/*@modifies	*opt@*/
;

#undef plan
#undef opt
#undef fstat
#undef st
BUILD_EXTERN enum SampleRangePlanStatus samplerange_plan(
	/*@out@*/ struct SampleRangePlan *RESTRICT plan,
	const struct SampleRangeOpt *RESTRICT,
	const struct FileStats *RESTRICT, const struct SeekTable *RESTRICT
)
/*@globals	internalState@*/
/*@modifies	internalState,
		*plan
@*/
;

/* EOF //////////////////////////////////////////////////////////////////// */
#endif	/* H_TTA_FORMATS_SAMPLE_RANGE_H */
