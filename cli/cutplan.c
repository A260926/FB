/*****************************************************************************
 * cutplan.c
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

#include "cutplan.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>

#define CP_ERR( ... ) do { if( err_stream ) fprintf( err_stream, "[Error] " __VA_ARGS__ ); } while( 0 )

typedef struct
{
    uint64_t dts;
    uint64_t cts;
    uint32_t delta;
    lsmash_random_access_flag ra_flags;
} sample_timeline_t;

typedef struct
{
    uint32_t                    track_ID;
    uint32_t                    timescale;
    lsmash_media_type           handler_type;
    lsmash_codec_type_t         sample_type;
    /* the sole rate=1 non-empty edit, in movie / media timescales */
    uint64_t                    edit_movie_start;   /* movie timescale */
    uint64_t                    edit_movie_duration;
    int64_t                     edit_media_start;   /* media timescale */
    sample_timeline_t          *samples;
    uint32_t                    num_samples;
    cut_track_plan_t           *plan;
} cut_track_ctx_t;

void cutplan_cleanup( cut_plan_t *plan )
{
    if( !plan )
        return;
    lsmash_freep( &plan->cuts );
    if( plan->tracks )
    {
        for( uint32_t i = 0; i < plan->num_tracks; i++ )
            lsmash_freep( &plan->tracks[i].segment );
        lsmash_freep( &plan->tracks );
    }
    memset( plan, 0, sizeof(cut_plan_t) );
}

int cutplan_parse_file( const char *filename, cut_plan_t *plan, FILE *err_stream )
{
    FILE *fp = fopen( filename, "r" );
    if( !fp )
    {
        CP_ERR( "failed to open cut file %s: %s\n", filename, strerror( errno ) );
        return -1;
    }
    char     line[256];
    uint32_t line_number = 0;
    uint32_t capacity    = 0;
    memset( plan, 0, sizeof(cut_plan_t) );
    while( fgets( line, sizeof(line), fp ) )
    {
        ++line_number;
        /* A complete line must fit; reject overlong lines instead of silently
         * parsing the remainder as another record. */
        size_t len = strlen( line );
        if( len && line[len - 1] != '\n' && !feof( fp ) )
        {
            CP_ERR( "line %u in %s is too long.\n", line_number, filename );
            goto fail;
        }
        char *p = line;
        while( *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' )
            ++p;
        if( *p == '\0' )
            continue;   /* blank line */
        if( *p == '-' || *p == '+' || *p < '0' || *p > '9' )
        {
            CP_ERR( "invalid cut at line %u in %s.\n", line_number, filename );
            goto fail;
        }
        errno = 0;
        char *after_start;
        unsigned long long start_val = strtoull( p, &after_start, 10 );
        if( errno || after_start == p
         || (*after_start != ' ' && *after_start != '\t') )
        {
            CP_ERR( "invalid cut at line %u in %s.\n", line_number, filename );
            goto fail;
        }
        p = after_start;
        while( *p == ' ' || *p == '\t' )
            ++p;
        if( *p == '-' || *p == '+' || *p < '0' || *p > '9' )
        {
            CP_ERR( "invalid cut at line %u in %s.\n", line_number, filename );
            goto fail;
        }
        char *after_end;
        unsigned long long end_val = strtoull( p, &after_end, 10 );
        if( errno || after_end == p )
        {
            CP_ERR( "invalid cut at line %u in %s.\n", line_number, filename );
            goto fail;
        }
        p = after_end;
        while( *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' )
            ++p;
        if( *p != '\0' )
        {
            CP_ERR( "invalid cut at line %u in %s.\n", line_number, filename );
            goto fail;
        }
        if( plan->num_cuts >= capacity )
        {
            capacity = capacity ? capacity * 2 : 8;
            cut_interval_t *tmp = lsmash_realloc( plan->cuts, capacity * sizeof(cut_interval_t) );
            if( !tmp )
            {
                CP_ERR( "failed to allocate cut plan.\n" );
                goto fail;
            }
            plan->cuts = tmp;
        }
        plan->cuts[ plan->num_cuts ].start = start_val;
        plan->cuts[ plan->num_cuts ].end   = end_val;
        ++ plan->num_cuts;
    }
    int read_error = ferror( fp );
    fclose( fp );
    if( read_error )
    {
        CP_ERR( "failed to read cut file %s.\n", filename );
        return -1;
    }
    if( plan->num_cuts == 0 )
    {
        CP_ERR( "cut file %s contains no valid cut.\n", filename );
        return -1;
    }
    for( uint32_t i = 0; i < plan->num_cuts; i++ )
    {
        if( plan->cuts[i].start >= plan->cuts[i].end )
        {
            CP_ERR( "cut %u has an empty or reversed interval.\n", i + 1 );
            return -1;
        }
        plan->total_duration += plan->cuts[i].end - plan->cuts[i].start;
    }
    return 0;
fail:
    fclose( fp );
    cutplan_cleanup( plan );
    return -1;
}

/* Exact movie-time -> media-time conversion. No floating point. */
static int movie_to_media_time
(
    uint64_t  movie_time_offset,
    uint32_t  media_timescale,
    uint32_t  movie_timescale,
    uint64_t *media_time
)
{
    if( movie_time_offset > UINT64_MAX / media_timescale )
        return -1;
    uint64_t scaled = movie_time_offset * media_timescale;
    if( scaled % movie_timescale )
        return -1;
    *media_time = scaled / movie_timescale;
    return 0;
}

/* Return non-zero if the AVC sample contains an IDR slice NAL unit (type 5).
 * Samples of avc1 tracks are length-prefixed, never start-code prefixed. */
static int sample_contains_idr_nalu( const uint8_t *data, uint32_t length, uint8_t nalu_length_size )
{
    uint32_t offset = 0;
    while( offset + nalu_length_size <= length )
    {
        uint32_t nalu_length = 0;
        for( uint8_t i = 0; i < nalu_length_size; i++ )
            nalu_length = (nalu_length << 8) | data[ offset + i ];
        offset += nalu_length_size;
        if( nalu_length == 0 || nalu_length > length - offset )
            return 0;
        if( (data[offset] & 0x1F) == 5 )
            return 1;
        offset += nalu_length;
    }
    return 0;
}

static uint8_t get_avc_nalu_length_size( lsmash_root_t *root, uint32_t track_ID )
{
    lsmash_summary_t *summary = lsmash_get_summary( root, track_ID, 1 );
    if( !summary )
        return 0;
    uint32_t count = lsmash_count_codec_specific_data( summary );
    for( uint32_t i = 1; i <= count; i++ )
    {
        lsmash_codec_specific_t *cs = lsmash_get_codec_specific_data( summary, i );
        if( cs && cs->type == LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_VIDEO_H264
                 && cs->format == LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED )
        {
            lsmash_h264_specific_parameters_t *avcc =
                (lsmash_h264_specific_parameters_t *)cs->data.structured;
            return (avcc->lengthSizeMinusOne & 0x03) + 1;
        }
    }
    return 4;   /* mandated default length size */
}

static int find_sample_by_cts( const sample_timeline_t *samples, uint32_t num_samples, uint64_t cts, uint32_t *sample_number )
{
    /* CTS is unique and monotonically increasing within a track. */
    for( uint32_t i = 0; i < num_samples; i++ )
        if( samples[i].cts == cts )
        {
            *sample_number = i + 1;
            return 0;
        }
    return -1;
}

/* First sample whose [cts, cts+delta) interval extends past start, i.e. the
 * first sample intersecting a region beginning at start. */
static int find_first_sample_intersecting( const sample_timeline_t *samples, uint32_t num_samples,
                                           uint64_t start, uint32_t *sample_number )
{
    for( uint32_t i = 0; i < num_samples; i++ )
        if( samples[i].cts + samples[i].delta > start )
        {
            *sample_number = i + 1;
            return 0;
        }
    return -1;
}

static int find_last_sample_cts_lt( const sample_timeline_t *samples, uint32_t num_samples, uint64_t cts, uint32_t *sample_number )
{
    for( int32_t i = (int32_t)num_samples - 1; i >= 0; i-- )
        if( samples[i].cts < cts )
        {
            *sample_number = (uint32_t)i + 1;
            return 0;
        }
    return -1;
}

int cutplan_build
(
    lsmash_root_t    *root,
    cut_plan_t       *plan,
    FILE             *err_stream
)
{
    if( !plan || plan->num_cuts == 0 )
    {
        CP_ERR( "empty cut plan.\n" );
        return -1;
    }
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    if( lsmash_get_movie_parameters( root, &movie_param ) )
    {
        CP_ERR( "failed to get movie parameters.\n" );
        return -1;
    }
    plan->movie_timescale = movie_param.timescale;
    uint32_t num_tracks = movie_param.number_of_tracks;
    if( num_tracks == 0 )
    {
        CP_ERR( "input movie has no track.\n" );
        return -1;
    }
    cut_track_ctx_t *ctx = lsmash_malloc_zero( num_tracks * sizeof(cut_track_ctx_t) );
    if( !ctx )
    {
        CP_ERR( "failed to allocate cut track contexts.\n" );
        return -1;
    }
    int ret = -1;
    uint32_t num_video = 0;
    uint32_t num_audio = 0;
    for( uint32_t i = 0; i < num_tracks; i++ )
    {
        uint32_t track_ID = lsmash_get_track_ID( root, i + 1 );
        if( !track_ID )
        {
            CP_ERR( "failed to get track_ID.\n" );
            goto fail;
        }
        cut_track_ctx_t *t = &ctx[i];
        t->track_ID = track_ID;
        lsmash_media_parameters_t media_param;
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( root, track_ID, &media_param ) )
        {
            CP_ERR( "failed to get media parameters for track %u.\n", track_ID );
            goto fail;
        }
        t->timescale    = media_param.timescale;
        t->handler_type = media_param.handler_type;
        if( lsmash_count_data_reference( root, track_ID ) != 1 )
        {
            CP_ERR( "track %u must have exactly one self-contained data reference.\n", track_ID );
            goto fail;
        }
        lsmash_data_reference_t data_ref = { .index = 1 };
        if( lsmash_get_data_reference( root, track_ID, &data_ref ) < 0 || data_ref.location )
        {
            lsmash_cleanup_data_reference( &data_ref );
            CP_ERR( "track %u references external media; only self-contained MP4 is supported.\n", track_ID );
            goto fail;
        }
        lsmash_cleanup_data_reference( &data_ref );
        if( lsmash_count_summary( root, track_ID ) != 1 )
        {
            CP_ERR( "track %u must have exactly one sample description.\n", track_ID );
            goto fail;
        }
        lsmash_summary_t *summary = lsmash_get_summary( root, track_ID, 1 );
        if( !summary )
        {
            CP_ERR( "failed to get sample description of track %u.\n", track_ID );
            goto fail;
        }
        t->sample_type = summary->sample_type;
        int codec_ok = 1;
        if( t->handler_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
        {
            if( !lsmash_check_codec_type_identical( t->sample_type, ISOM_CODEC_TYPE_AVC1_VIDEO ) )
                codec_ok = 0;
            else
                ++ num_video;
        }
        else if( t->handler_type == ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK )
        {
            if( !lsmash_check_codec_type_identical( t->sample_type, ISOM_CODEC_TYPE_MP4A_AUDIO ) )
                codec_ok = 0;
            else
                ++ num_audio;
        }
        else
            codec_ok = 0;
        lsmash_cleanup_summary( summary );
        if( !codec_ok )
        {
            CP_ERR( "track %u has an unsupported codec for its handler type.\n", track_ID );
            goto fail;
        }
    }
    if( num_video != 1 || num_audio > 1 )
    {
        CP_ERR( "cutting requires exactly one avc1 video track and at most one mp4a audio track.\n" );
        goto fail;
    }
    /* Validate edit lists: optional leading empty edits followed by exactly
     * one rate=1 non-empty edit.  Also derive the common media-covered region
     * in movie time. */
    uint64_t common_start = 0;
    uint64_t common_end   = UINT64_MAX;
    for( uint32_t i = 0; i < num_tracks; i++ )
    {
        cut_track_ctx_t *t = &ctx[i];
        uint32_t num_edits = lsmash_count_explicit_timeline_map( root, t->track_ID );
        if( num_edits == 0 )
        {
            CP_ERR( "track %u has no edit list; unsupported timeline.\n", t->track_ID );
            goto fail;
        }
        uint64_t movie_cursor    = 0;
        int      normal_edit_seen = 0;
        for( uint32_t e = 1; e <= num_edits; e++ )
        {
            lsmash_edit_t edit;
            if( lsmash_get_explicit_timeline_map( root, t->track_ID, e, &edit ) )
            {
                CP_ERR( "failed to get edit %u of track %u.\n", e, t->track_ID );
                goto fail;
            }
            if( edit.start_time == ISOM_EDIT_MODE_EMPTY )
            {
                if( normal_edit_seen )
                {
                    CP_ERR( "empty edit after media edit in track %u is not supported.\n", t->track_ID );
                    goto fail;
                }
                movie_cursor += edit.duration;
                continue;
            }
            if( normal_edit_seen || edit.rate != ISOM_EDIT_MODE_NORMAL || edit.start_time < 0 )
            {
                CP_ERR( "unsupported edit form in track %u.\n", t->track_ID );
                goto fail;
            }
            normal_edit_seen       = 1;
            t->edit_movie_start    = movie_cursor;
            t->edit_movie_duration = edit.duration;
            t->edit_media_start    = edit.start_time;
            movie_cursor          += edit.duration;
        }
        if( !normal_edit_seen || t->edit_movie_duration == 0 )
        {
            CP_ERR( "track %u lacks a non-empty rate=1 edit.\n", t->track_ID );
            goto fail;
        }
        if( movie_cursor < t->edit_movie_start )
        {
            CP_ERR( "edit timeline overflow in track %u.\n", t->track_ID );
            goto fail;
        }
        uint64_t track_end = t->edit_movie_start + t->edit_movie_duration;
        if( t->edit_movie_start > common_start )
            common_start = t->edit_movie_start;
        if( track_end < common_end )
            common_end = track_end;
    }
    if( common_start >= common_end )
    {
        CP_ERR( "the tracks have no common media-covered region.\n" );
        goto fail;
    }
    /* Build per-track sample tables and validate boundaries.  The caller has
     * already constructed the media timeline for every track. */
    uint8_t nalu_length_size = 0;
    for( uint32_t i = 0; i < num_tracks; i++ )
    {
        cut_track_ctx_t *t = &ctx[i];
        t->num_samples = lsmash_get_sample_count_in_media_timeline( root, t->track_ID );
        if( t->num_samples == 0 )
        {
            CP_ERR( "track %u has no sample.\n", t->track_ID );
            goto fail;
        }
        t->samples = lsmash_malloc_zero( t->num_samples * sizeof(sample_timeline_t) );
        if( !t->samples )
        {
            CP_ERR( "failed to allocate sample table for track %u.\n", t->track_ID );
            goto fail;
        }
        for( uint32_t s = 1; s <= t->num_samples; s++ )
        {
            lsmash_sample_t info;
            if( lsmash_get_sample_info_from_media_timeline( root, t->track_ID, s, &info )
             || lsmash_get_sample_delta_from_media_timeline( root, t->track_ID, s, &t->samples[s-1].delta ) )
            {
                CP_ERR( "failed to get sample %u information for track %u.\n", s, t->track_ID );
                goto fail;
            }
            t->samples[s-1].dts      = info.dts;
            t->samples[s-1].cts      = info.cts;
            t->samples[s-1].ra_flags = info.prop.ra_flags;
        }
        if( t->handler_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
        {
            nalu_length_size = get_avc_nalu_length_size( root, t->track_ID );
            if( nalu_length_size == 0 )
            {
                CP_ERR( "broken AVC decoder configuration in track %u.\n", t->track_ID );
                goto fail;
            }
            /* Every random access point must be a real IDR; a non-IDR intra
             * sample cannot anchor a closed-GOP cut. */
            for( uint32_t s = 1; s <= t->num_samples; s++ )
            {
                if( t->samples[s-1].ra_flags == ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE )
                    continue;
                lsmash_sample_t *sample = lsmash_get_sample_from_media_timeline( root, t->track_ID, s );
                if( !sample
                 || !sample_contains_idr_nalu( sample->data, sample->length, nalu_length_size ) )
                {
                    if( sample )
                        lsmash_delete_sample( sample );
                    CP_ERR( "video track %u is not made of closed IDR GOPs (sample %u).\n",
                            t->track_ID, s );
                    goto fail;
                }
                lsmash_delete_sample( sample );
            }
        }
    }
    /* Allocate the output plans. */
    plan->tracks = lsmash_malloc_zero( num_tracks * sizeof(cut_track_plan_t) );
    if( !plan->tracks )
    {
        CP_ERR( "failed to allocate output track plans.\n" );
        goto fail;
    }
    plan->num_tracks = num_tracks;
    for( uint32_t i = 0; i < num_tracks; i++ )
    {
        plan->tracks[i].track_ID  = ctx[i].track_ID;
        plan->tracks[i].timescale = ctx[i].timescale;
        plan->tracks[i].segment   = lsmash_malloc_zero( plan->num_cuts * sizeof(cut_track_segment_t) );
        if( !plan->tracks[i].segment )
        {
            CP_ERR( "failed to allocate segment plan for track %u.\n", ctx[i].track_ID );
            goto fail;
        }
        ctx[i].plan = &plan->tracks[i];
    }
    /* Validate every cut against every track and compute sample ranges. */
    for( uint32_t c = 0; c < plan->num_cuts; c++ )
    {
        const cut_interval_t *cut = &plan->cuts[c];
        if( cut->start < common_start || cut->end > common_end || cut->end <= cut->start )
        {
            CP_ERR( "cut %u [%"PRIu64", %"PRIu64") is outside the common media region [%"PRIu64", %"PRIu64").\n",
                    c + 1, cut->start, cut->end, common_start, common_end );
            goto fail;
        }
        for( uint32_t i = 0; i < num_tracks; i++ )
        {
            cut_track_ctx_t *t = &ctx[i];
            uint64_t off_start = cut->start - t->edit_movie_start;
            uint64_t off_end   = cut->end   - t->edit_movie_start;
            uint64_t media_start, media_end;
            if( movie_to_media_time( off_start, t->timescale, plan->movie_timescale, &media_start )
             || movie_to_media_time( off_end,   t->timescale, plan->movie_timescale, &media_end ) )
            {
                CP_ERR( "cut %u boundary does not map to an integer media time for track %u.\n",
                        c + 1, t->track_ID );
                goto fail;
            }
            media_start += (uint64_t)t->edit_media_start;
            media_end   += (uint64_t)t->edit_media_start;
            cut_track_segment_t *seg = &t->plan->segment[c];
            if( t->handler_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
            {
                /* Boundaries must be IDR display times, not DTS times. */
                uint32_t start_sample;
                if( find_sample_by_cts( t->samples, t->num_samples, media_start, &start_sample ) )
                {
                    CP_ERR( "cut %u start does not correspond to a video sample display time.\n", c + 1 );
                    goto fail;
                }
                lsmash_sample_t *sample = lsmash_get_sample_from_media_timeline( root, t->track_ID, start_sample );
                if( !sample
                 || !(t->samples[start_sample-1].ra_flags & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC)
                 || !sample_contains_idr_nalu( sample->data, sample->length, nalu_length_size ) )
                {
                    if( sample )
                        lsmash_delete_sample( sample );
                    CP_ERR( "cut %u start is not an IDR display time.\n", c + 1 );
                    goto fail;
                }
                lsmash_delete_sample( sample );
                uint32_t end_sample;   /* first sample excluded from the segment */
                if( cut->end >= common_end )
                    end_sample = t->num_samples + 1;
                else
                {
                    if( find_sample_by_cts( t->samples, t->num_samples, media_end, &end_sample ) )
                    {
                        CP_ERR( "cut %u end does not correspond to a video sample display time.\n", c + 1 );
                        goto fail;
                    }
                    sample = lsmash_get_sample_from_media_timeline( root, t->track_ID, end_sample );
                    if( !sample
                     || !(t->samples[end_sample-1].ra_flags & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC)
                     || !sample_contains_idr_nalu( sample->data, sample->length, nalu_length_size ) )
                    {
                        if( sample )
                            lsmash_delete_sample( sample );
                        CP_ERR( "cut %u end is not an IDR display time.\n", c + 1 );
                        goto fail;
                    }
                    lsmash_delete_sample( sample );
                }
                seg->first_sample = start_sample;
                seg->last_sample  = end_sample - 1;
            }
            else
            {
                /* Audio packets may straddle the boundary: take every complete
                 * sample intersecting the media interval, never split one.
                 * Intersection of [cts, cts+delta) with [media_start, media_end):
                 * cts+delta > media_start and cts < media_end. */
                if( find_first_sample_intersecting( t->samples, t->num_samples, media_start, &seg->first_sample )
                 || find_last_sample_cts_lt( t->samples, t->num_samples, media_end, &seg->last_sample )
                 || seg->first_sample > seg->last_sample )
                {
                    CP_ERR( "cut %u selects no audio sample in track %u.\n", c + 1, t->track_ID );
                    goto fail;
                }
            }
            seg->media_start   = media_start;
            seg->edit_duration = media_end - media_start;
            seg->src_first_dts = t->samples[ seg->first_sample - 1 ].dts;
        }
    }
    /* Compute output DTS / CTS mapping: each track starts at DTS 0 and each
     * segment is appended by actual copied sample durations. */
    for( uint32_t i = 0; i < num_tracks; i++ )
    {
        cut_track_ctx_t *t = &ctx[i];
        uint64_t dts_cursor = 0;
        uint32_t total      = 0;
        for( uint32_t c = 0; c < plan->num_cuts; c++ )
        {
            cut_track_segment_t *seg = &t->plan->segment[c];
            const sample_timeline_t *first = &t->samples[ seg->first_sample - 1 ];
            seg->out_dts       = dts_cursor;
            if( first->cts < first->dts || first->cts > seg->media_start )
            {
                CP_ERR( "cut %u produces an invalid presentation start for track %u.\n",
                        c + 1, t->track_ID );
                goto fail;
            }
            seg->out_cts_first = dts_cursor + (first->cts - first->dts);
            /* The edit starts at the cut's presentation start inside the first
             * copied sample.  For video the boundary is a sample display time;
             * for audio it may lie inside a packet, hiding that packet head. */
            seg->edit_start = seg->out_cts_first + (seg->media_start - first->cts);
            uint64_t copied_duration = 0;
            for( uint32_t s = seg->first_sample; s <= seg->last_sample; s++ )
                copied_duration += t->samples[s-1].delta;
            dts_cursor += copied_duration;
            total      += seg->last_sample - seg->first_sample + 1;
        }
        t->plan->num_samples = total;
    }
    ret = 0;
fail:
    for( uint32_t i = 0; i < num_tracks; i++ )
        lsmash_freep( &ctx[i].samples );
    lsmash_free( ctx );
    if( ret < 0 )
    {
        if( plan->tracks )
        {
            for( uint32_t i = 0; i < plan->num_tracks; i++ )
                lsmash_freep( &plan->tracks[i].segment );
            lsmash_freep( &plan->tracks );
        }
    }
    return ret;
}

int cutplan_find_track( const cut_plan_t *plan, uint32_t track_ID, uint32_t *index )
{
    for( uint32_t i = 0; i < plan->num_tracks; i++ )
        if( plan->tracks[i].track_ID == track_ID )
        {
            if( index )
                *index = i;
            return 0;
        }
    return -1;
}
