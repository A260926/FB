/*****************************************************************************
 * cutplan.h
 *****************************************************************************
 * Copyright (C) 2017 L-SMASH project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
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

/* Cut plan for lossless reordering of closed-GOP AVC video and optional AAC
 * audio within a single self-contained MP4.
 *
 * A cut file contains non-empty lines of two decimal unsigned integers
 * "start end" that specify half-open intervals [start, end) in the movie
 * timescale of the input.  Line order is the output order; intervals may be
 * reversed or repeated and are never sorted nor merged. */

#ifndef CUTPLAN_H
#define CUTPLAN_H

#include "cli.h"

#include <stdio.h>
#include <stdint.h>

typedef struct
{
    uint64_t start;     /* in the input movie timescale */
    uint64_t end;
} cut_interval_t;

/* Per-track, per-cut media sample range [first_sample, last_sample], in
 * decoding order, plus the media-time window the edit list must present. */
typedef struct
{
    uint32_t first_sample;
    uint32_t last_sample;
    uint64_t src_first_dts;      /* DTS of first_sample in the source media, media timescale */
    uint64_t media_start;        /* cut start in source media composition time, media timescale */
    uint64_t edit_duration;       /* presented window length, media timescale */
    uint64_t out_dts;             /* output DTS of first_sample, media timescale */
    uint64_t out_cts_first;       /* output CTS of the first copied sample */
    uint64_t edit_start;          /* edit start_time in output media composition time */
} cut_track_segment_t;

typedef struct
{
    uint32_t              track_ID;
    uint32_t              timescale;
    cut_track_segment_t  *segment;       /* one entry per cut */
    uint32_t              num_samples;   /* number of samples copied in total */
} cut_track_plan_t;

typedef struct
{
    cut_interval_t    *cuts;
    uint32_t           num_cuts;
    uint64_t           total_duration;   /* sum of (end - start), movie timescale */
    cut_track_plan_t  *tracks;
    uint32_t           num_tracks;
    uint32_t           movie_timescale;
} cut_plan_t;

/* Parse a cut file.
 * Return 0 if successful, -1 otherwise with a message printed to err_stream. */
int cutplan_parse_file( const char *filename, cut_plan_t *plan, FILE *err_stream );

/* Validate the plan against an opened input movie and compute every per-track
 * sample range, output timestamp mapping and edit list entry.
 * The input must be a single self-contained MP4 with one avc1 video track and
 * at most one optional mp4a audio track, each having one sample description.
 * Return 0 if successful, -1 otherwise with a message printed to err_stream. */
int cutplan_build
(
    lsmash_root_t *root,
    cut_plan_t    *plan,
    FILE          *err_stream
);

/* Release everything owned by the plan. */
void cutplan_cleanup( cut_plan_t *plan );

/* Find the plan entry of a track by its source track_ID.
 * Return 0 if found, -1 otherwise. */
int cutplan_find_track( const cut_plan_t *plan, uint32_t track_ID, uint32_t *index );

#endif /* CUTPLAN_H */
