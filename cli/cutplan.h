/*****************************************************************************
 * cutplan.h
 *****************************************************************************
 * Copyright (C) 2017 L-SMASH project
 *
 * Authors: L-SMASH project
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *****************************************************************************/

/* This file is available under an ISC license. */

/*
 * Cut plan: reorder a set of half-open [start,end) intervals of one input
 * movie into a new self-contained MP4 without re-encoding.
 *
 * All interval endpoints in the cuts file are expressed in the input movie
 * timescale.  Every conversion towards a media timescale is required to be
 * exact (integer); no floating point arithmetic is used for timestamps.
 */

#ifndef CUTPLAN_H
#define CUTPLAN_H

#include "cli.h"

#include <stdio.h>
#include <stdint.h>

/* One interval as read from the cuts file: movie timescale, [start,end). */
typedef struct
{
    uint64_t start;
    uint64_t end;
} cut_interval_t;

/* Parsed, order-preserving cut list. */
typedef struct
{
    cut_interval_t *interval;
    uint32_t        count;
} cut_plan_t;

/*
 * Parse a cuts file.
 * Each non-empty line shall contain exactly two decimal unsigned integers,
 * "start end", with start < end.  Blank lines are ignored.  Lines keep their
 * file order: they are never sorted nor merged.
 *
 * Return 0 on success, a negative value on any malformed line or empty plan.
 * On success *plan holds newly allocated memory owned by the caller and
 * released with cutplan_free_plan().
 */
int cutplan_parse_file( const char *path, cut_plan_t *plan );

void cutplan_free_plan( cut_plan_t *plan );

/* One sample to copy into an output track. */
typedef struct
{
    uint32_t src_sample;   /* 1-based sample number in the input track (decode order) */
    uint64_t dts;          /* DTS to assign in the output media timescale */
    uint64_t cts;          /* CTS to assign in the output media timescale */
} cut_job_t;

/* One edit list entry to create for one cut on one track. */
typedef struct
{
    uint64_t media_time;   /* elst media_time in the output media timescale */
} cut_edit_t;

/* Resolved per-track copy instructions. */
typedef struct
{
    uint32_t    track_ID;          /* input track_ID */
    int         is_video;          /* 1 for the avc1 video track, 0 for audio */
    uint32_t    media_timescale;
    uint32_t    last_sample_delta;
    cut_job_t  *job;               /* flat sample list, decode order, repeats expanded */
    uint32_t    num_jobs;
    cut_edit_t *edit;              /* one edit per cut */
    uint32_t    num_edits;
} cut_track_t;

/* Fully resolved plan against one open input movie. */
typedef struct
{
    uint32_t        movie_timescale;
    uint32_t        num_cuts;
    cut_interval_t *cut;            /* movie-time intervals, output order */
    uint32_t        num_tracks;
    cut_track_t    *track;          /* one entry per output track, track order */
} cutplan_result_t;

/* Everything cutplan_build() needs to know about one input track.
 * The summary pointers are the ones cached while the input boxes were open;
 * they stay valid after the box tree is torn down. */
typedef struct
{
    uint32_t          track_ID;
    lsmash_media_type handler_type;
    uint32_t          media_timescale;
    uint32_t          num_summaries;
    lsmash_summary_t **summary;
} cutplan_src_track_t;

/* Open input movie description.  Timelines must already be constructed. */
typedef struct
{
    lsmash_root_t        *root;
    uint32_t              movie_timescale;
    cutplan_src_track_t  *track;
    uint32_t              num_tracks;
} cutplan_input_t;

/*
 * Resolve a parsed plan against one open input movie.
 *
 * The input is restricted to one self-contained MP4 with one avc1 video
 * track and at most one mp4a audio track, each having a single sample
 * description, closed-GOP video, and at most one rate == 1.0 non-empty edit
 * (optionally preceded by empty edits).  All validation - interval bounds,
 * exact time conversion, IDR display-time boundaries, edit shapes, common
 * media region - happens here, before any output file is written.
 *
 * Return 0 on success and store a newly allocated result in *result;
 * return a negative value and leave *result untouched on any rejection.
 */
int cutplan_build
(
    const cutplan_input_t *input,
    const cut_plan_t      *plan,
    cutplan_result_t     **result
);

void cutplan_free_result( cutplan_result_t *result );

#endif
