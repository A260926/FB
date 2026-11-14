/*****************************************************************************
 * cutplan.c
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

#include "cutplan.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>

/*--------------------------------------------------------------------------------*
 * Parsing of the cuts file
 *--------------------------------------------------------------------------------*/

void cutplan_free_plan( cut_plan_t *plan )
{
    if( !plan )
        return;
    lsmash_free( plan->interval );
    plan->interval = NULL;
    plan->count    = 0;
}

/* Parse one unsigned decimal integer with strict syntax.
 * Return 0 and advance *pp on success, -1 otherwise. */
static int parse_uint( const char **pp, uint64_t *value )
{
    const char *p = *pp;
    while( *p && isspace( (unsigned char)*p ) )
        ++p;
    if( !isdigit( (unsigned char)*p ) )
        return -1;
    uint64_t v = 0;
    while( isdigit( (unsigned char)*p ) )
    {
        unsigned d = (unsigned)(*p - '0');
        if( v > (UINT64_MAX - d) / 10 )
            return -1;
        v = v * 10 + d;
        ++p;
    }
    *pp    = p;
    *value = v;
    return 0;
}

int cutplan_parse_file( const char *path, cut_plan_t *plan )
{
    if( !path || !plan )
        return -1;
    FILE *fp = fopen( path, "r" );
    if( !fp )
        return -1;
    plan->interval = NULL;
    plan->count    = 0;
    uint32_t cap   = 0;
    char *line     = NULL;
    size_t linesz  = 0;
    int err        = 0;
    for( ;; )
    {
        /* Read one physical line into a growable buffer. */
        size_t len = 0;
        int ch;
        while( (ch = fgetc( fp )) != EOF && ch != '\n' )
        {
            if( len + 1 >= linesz )
            {
                size_t newsz = linesz ? linesz * 2 : 128;
                char *nl = lsmash_realloc( line, newsz );
                if( !nl )
                {
                    err = 1;
                    break;
                }
                line   = nl;
                linesz = newsz;
            }
            line[ len++ ] = (char)ch;
        }
        if( err )
            break;
        if( ch == EOF && len == 0 )
            break;   /* end of file */
        line[ len ] = '\0';
        const char *p = line;
        while( *p && isspace( (unsigned char)*p ) )
            ++p;
        if( *p != '\0' )   /* non-empty line */
        {
            uint64_t start, end;
            const char *after = p;
            if( parse_uint( &after, &start ) || parse_uint( &after, &end ) )
            {
                err = 1;
                break;
            }
            while( *after && isspace( (unsigned char)*after ) )
                ++after;
            if( *after != '\0' || start >= end )
            {
                err = 1;   /* trailing garbage or empty/negative interval */
                break;
            }
            if( plan->count == cap )
            {
                uint32_t newcap = cap ? cap * 2 : 8;
                cut_interval_t *ni = lsmash_realloc( plan->interval, newcap * sizeof(cut_interval_t) );
                if( !ni )
                {
                    err = 1;
                    break;
                }
                plan->interval = ni;
                cap            = newcap;
            }
            plan->interval[ plan->count ].start = start;
            plan->interval[ plan->count ].end   = end;
            ++plan->count;
        }
        if( ch == EOF )
            break;
    }
    lsmash_free( line );
    fclose( fp );
    if( err || plan->count == 0 )
    {
        cutplan_free_plan( plan );
        return -1;
    }
    return 0;
}

/*--------------------------------------------------------------------------------*
 * Exact integer time conversion
 *--------------------------------------------------------------------------------*/

/* Convert t ticks from timescale 'from' to timescale 'to'.
 * Require an exact integer result; return -1 if rounding would be needed. */
static int scale_time( uint64_t t, uint32_t from, uint32_t to, uint64_t *out )
{
    if( from == 0 || to == 0 )
        return -1;
    if( from == to )
    {
        *out = t;
        return 0;
    }
    if( t != 0 && (uint64_t)to > UINT64_MAX / t )
        return -1;
    uint64_t product = t * (uint64_t)to;
    if( product % from != 0 )
        return -1;
    *out = product / from;
    return 0;
}

/*--------------------------------------------------------------------------------*
 * Internal per-track resolved state while building the plan
 *--------------------------------------------------------------------------------*/

typedef struct
{
    const cutplan_src_track_t *src;
    cut_track_t                out;
    int      has_edit;
    uint64_t edit_movie_start;   /* movie position where the normal edit begins */
    uint64_t edit_movie_length;  /* normal edit length in movie timescale */
    uint64_t media_lo;           /* first presented media time (media timescale) */
    uint64_t media_hi;           /* one past last presented media time */
} build_track_t;

typedef struct
{
    uint32_t sample;
    uint64_t dts;
    uint64_t cts;
    uint32_t delta;
    int      is_idr;
    unsigned ra_flags;
} video_sample_t;

typedef struct
{
    lsmash_root_t      *root;
    uint32_t            movie_timescale;
    const cut_plan_t   *plan;
    build_track_t      *bt;
    uint32_t            num_bt;
    build_track_t      *video;
    build_track_t      *audio;
    video_sample_t     *vs;
    uint32_t            num_vs;
    uint8_t             nalu_length_size;
} build_ctx_t;

/*--------------------------------------------------------------------------------*
 * AVC bitstream IDR verification
 *
 * A boundary is accepted only when the access unit actually carries an IDR slice
 * NAL unit (nal_unit_type == 5).  A plain intra (non-IDR) picture or the mere
 * presence of a sync / random-access flag is not sufficient.
 *--------------------------------------------------------------------------------*/

static int get_avc_nalu_length_size( lsmash_summary_t *summary, uint8_t *length_size )
{
    uint32_t ncs = lsmash_count_codec_specific_data( summary );
    for( uint32_t i = 1; i <= ncs; i++ )
    {
        lsmash_codec_specific_t *cs = lsmash_get_codec_specific_data( summary, i );
        if( !cs || cs->type != LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_VIDEO_H264 )
            continue;
        lsmash_codec_specific_t *st = lsmash_convert_codec_specific_format( cs, LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
        if( !st )
            return -1;
        lsmash_h264_specific_parameters_t *p = (lsmash_h264_specific_parameters_t *)st->data.structured;
        uint8_t sz = (uint8_t)(p->lengthSizeMinusOne + 1);
        lsmash_destroy_codec_specific_data( st );
        if( sz != 1 && sz != 2 && sz != 4 )
            return -1;
        *length_size = sz;
        return 0;
    }
    return -1;
}

static int sample_is_idr( const uint8_t *data, uint32_t length, uint8_t nalu_length_size )
{
    uint32_t pos = 0;
    while( pos + nalu_length_size <= length )
    {
        uint32_t nalu_len = 0;
        for( uint8_t b = 0; b < nalu_length_size; b++ )
            nalu_len = (nalu_len << 8) | data[pos + b];
        pos += nalu_length_size;
        if( nalu_len == 0 || nalu_len > length - pos )
            return 0;
        uint8_t nal_type = data[pos] & 0x1f;
        if( nal_type >= 1 && nal_type <= 5 )
            return nal_type == 5;   /* first VCL NAL decides the picture type */
        pos += nalu_len;
    }
    return 0;
}

/*--------------------------------------------------------------------------------*
 * Edit validation and movie<->media mapping
 *--------------------------------------------------------------------------------*/

static int validate_edits( build_ctx_t *ctx, build_track_t *bt )
{
    lsmash_root_t *root = ctx->root;
    uint32_t tid = bt->src->track_ID;
    uint32_t nedits = lsmash_count_explicit_timeline_map( root, tid );
    bt->has_edit = nedits != 0;
    if( nedits == 0 )
    {
        /* Implicit one-to-one mapping over the whole media. */
        bt->edit_movie_start = 0;
        bt->media_lo         = 0;
        bt->media_hi         = lsmash_get_media_duration_from_media_timeline( root, tid );
        return bt->media_hi != 0 ? 0 : -1;
    }
    int found_normal = 0;
    uint64_t movie_cursor = 0;
    for( uint32_t e = 1; e <= nedits; e++ )
    {
        lsmash_edit_t edit;
        if( lsmash_get_explicit_timeline_map( root, tid, e, &edit ) < 0 )
            return -1;
        if( edit.start_time == ISOM_EDIT_MODE_EMPTY )
        {
            if( found_normal )
                return -1;   /* empty edits may only precede the normal edit */
            movie_cursor += edit.duration;
            continue;
        }
        if( edit.start_time < 0 || edit.rate != ISOM_EDIT_MODE_NORMAL )
            return -1;       /* dwell or non-unit-rate edits are rejected */
        if( found_normal )
            return -1;       /* exactly one non-empty edit */
        found_normal = 1;
        uint64_t media_len;
        if( scale_time( edit.duration, ctx->movie_timescale, bt->src->media_timescale, &media_len ) )
            return -1;
        bt->edit_movie_start  = movie_cursor;
        bt->edit_movie_length = edit.duration;
        bt->media_lo          = (uint64_t)edit.start_time;
        bt->media_hi          = (uint64_t)edit.start_time + media_len;
        movie_cursor         += edit.duration;
    }
    return found_normal ? 0 : -1;
}

/* Map a movie-time point inside this track's normal edit into media time.
 * Exact integer conversion only; fails outside the presented media region. */
static int movie_to_media( const build_ctx_t *ctx, const build_track_t *bt,
                           uint64_t movie_t, uint64_t *media_t )
{
    if( movie_t < bt->edit_movie_start )
        return -1;
    uint64_t delta_movie = movie_t - bt->edit_movie_start;
    uint64_t delta_media;
    if( scale_time( delta_movie, ctx->movie_timescale, bt->src->media_timescale, &delta_media ) )
        return -1;
    uint64_t m = bt->media_lo + delta_media;
    if( m < bt->media_lo || m > bt->media_hi )
        return -1;
    *media_t = m;
    return 0;
}

/*--------------------------------------------------------------------------------*
 * Result allocation helpers
 *--------------------------------------------------------------------------------*/

static int append_job( cut_track_t *ct, uint32_t sample, uint64_t dts, uint64_t cts )
{
    cut_job_t *nj = lsmash_realloc( ct->job, (ct->num_jobs + 1) * sizeof(cut_job_t) );
    if( !nj )
        return -1;
    ct->job = nj;
    ct->job[ ct->num_jobs ].src_sample = sample;
    ct->job[ ct->num_jobs ].dts        = dts;
    ct->job[ ct->num_jobs ].cts        = cts;
    ++ct->num_jobs;
    return 0;
}

static int append_edit( cut_track_t *ct, uint64_t media_time )
{
    cut_edit_t *ne = lsmash_realloc( ct->edit, (ct->num_edits + 1) * sizeof(cut_edit_t) );
    if( !ne )
        return -1;
    ct->edit = ne;
    ct->edit[ ct->num_edits ].media_time = media_time;
    ++ct->num_edits;
    return 0;
}

/*--------------------------------------------------------------------------------*
 * Video planning
 *--------------------------------------------------------------------------------*/

/* Cache every video sample and prove each flagged RAP is a real IDR access unit. */
static int prepare_video_samples( build_ctx_t *ctx, build_track_t *vt )
{
    lsmash_root_t *root = ctx->root;
    uint32_t tid = vt->src->track_ID;
    uint32_t n = lsmash_get_sample_count_in_media_timeline( root, tid );
    if( n == 0 )
        return -1;
    ctx->vs = lsmash_malloc( n * sizeof(video_sample_t) );
    if( !ctx->vs )
        return -1;
    ctx->num_vs = n;
    for( uint32_t i = 0; i < n; i++ )
    {
        lsmash_sample_t info;
        if( lsmash_get_sample_info_from_media_timeline( root, tid, i + 1, &info ) < 0 )
            return -1;
        uint32_t delta;
        if( lsmash_get_sample_delta_from_media_timeline( root, tid, i + 1, &delta ) < 0 )
            return -1;
        ctx->vs[i].sample   = i + 1;
        ctx->vs[i].dts      = info.dts;
        ctx->vs[i].cts      = info.cts;
        ctx->vs[i].delta    = delta;
        ctx->vs[i].ra_flags = info.prop.ra_flags;
        ctx->vs[i].is_idr   = 0;
    }
    lsmash_summary_t *summary = vt->src->summary[0];
    if( get_avc_nalu_length_size( summary, &ctx->nalu_length_size ) )
        return -1;
    for( uint32_t i = 0; i < n; i++ )
    {
        /* Cut mode requires closed GOPs: any open-GOP random access point is
         * rejected outright. */
        if( LSMASH_IS_OPEN_RAP( ctx->vs[i].ra_flags ) )
            return -1;
        /* A sync/RAP flag is only a candidate: prove the IDR slice NAL unit in
         * the bitstream.  A flagged non-IDR intra simply stays is_idr == 0 and
         * therefore can never serve as a cut boundary. */
        if( !(ctx->vs[i].ra_flags & (ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC
                                   | ISOM_SAMPLE_RANDOM_ACCESS_FLAG_RAP)) )
            continue;
        lsmash_sample_t *smp = lsmash_get_sample_from_media_timeline( root, tid, i + 1 );
        if( !smp )
            return -1;
        ctx->vs[i].is_idr = sample_is_idr( smp->data, smp->length, ctx->nalu_length_size );
        lsmash_delete_sample( smp );
    }
    return 0;
}

static video_sample_t *find_video_by_cts( build_ctx_t *ctx, uint64_t cts )
{
    for( uint32_t i = 0; i < ctx->num_vs; i++ )
        if( ctx->vs[i].cts == cts )
            return &ctx->vs[i];
    return NULL;
}

/* A boundary is legal only when an IDR picture is displayed at that time.
 * is_idr is set only for a flagged access point proven to carry an IDR NAL. */
static int check_idr_boundary( build_ctx_t *ctx, build_track_t *vt, uint64_t movie_t )
{
    uint64_t media_cts;
    if( movie_to_media( ctx, vt, movie_t, &media_cts ) )
        return -1;
    video_sample_t *vs = find_video_by_cts( ctx, media_cts );
    if( !vs || !vs->is_idr )
        return -1;
    return 0;
}

static int plan_video( build_ctx_t *ctx, build_track_t *vt )
{
    lsmash_root_t *root = ctx->root;
    uint32_t tid = vt->src->track_ID;
    cut_track_t *ct = &vt->out;
    ct->track_ID        = tid;
    ct->is_video        = 1;
    ct->media_timescale = vt->src->media_timescale;
    if( lsmash_get_last_sample_delta_from_media_timeline( root, tid, &ct->last_sample_delta ) < 0 )
        return -1;
    uint64_t running_dts = 0;
    for( uint32_t c = 0; c < ctx->plan->count; c++ )
    {
        uint64_t m_start, m_end;
        if( movie_to_media( ctx, vt, ctx->plan->interval[c].start, &m_start ) )
            return -1;
        if( movie_to_media( ctx, vt, ctx->plan->interval[c].end, &m_end ) )
            return -1;
        video_sample_t *first = find_video_by_cts( ctx, m_start );
        if( !first || !first->is_idr )
            return -1;
        uint32_t first_idx = (uint32_t)(first - ctx->vs);
        /* Decode-order span of the complete GOPs inside [m_start, m_end). */
        uint32_t last_idx = ctx->num_vs - 1;   /* inclusive; track end by default */
        if( ctx->plan->interval[c].end < vt->edit_movie_start + vt->edit_movie_length )
        {
            video_sample_t *next = find_video_by_cts( ctx, m_end );
            if( !next || !next->is_idr )
                return -1;
            uint32_t next_idx = (uint32_t)(next - ctx->vs);
            last_idx = next_idx - 1;          /* stop before the next IDR GOP */
        }
        /* Segment DTS base is fixed for the whole GOP; samples keep their
         * relative decode spacing and cts-dts composition offsets. */
        uint64_t segment_base = running_dts;
        /* Edit maps this segment's presentation start onto the IDR output CTS. */
        if( append_edit( ct, segment_base + (first->cts - first->dts) ) )
            return -1;
        for( uint32_t k = first_idx; k <= last_idx; k++ )
        {
            uint64_t out_dts = segment_base + (ctx->vs[k].dts - first->dts);
            uint64_t out_cts = out_dts + (ctx->vs[k].cts - ctx->vs[k].dts);
            if( append_job( ct, ctx->vs[k].sample, out_dts, out_cts ) )
                return -1;
            running_dts = out_dts + ctx->vs[k].delta;
            ct->last_sample_delta = ctx->vs[k].delta;
        }
    }
    return 0;
}

/*--------------------------------------------------------------------------------*
 * Audio planning
 *
 * Endpoints usually fall inside an AAC packet, so selection is driven by the
 * media interval rather than video sample numbers.  Every packet intersecting
 * the interval is copied whole; an edit per cut hides the head overhang and the
 * re-arrangement delay.  Adjacent cuts sharing a source packet each get their own
 * real copy.
 *--------------------------------------------------------------------------------*/

static int plan_audio( build_ctx_t *ctx, build_track_t *at )
{
    lsmash_root_t *root = ctx->root;
    uint32_t tid = at->src->track_ID;
    cut_track_t *ct = &at->out;
    ct->track_ID        = tid;
    ct->is_video        = 0;
    ct->media_timescale = at->src->media_timescale;
    ct->last_sample_delta = 0;   /* updated from the final copied packet */
    uint32_t n = lsmash_get_sample_count_in_media_timeline( root, tid );
    if( n == 0 )
        return -1;
    uint64_t running_dts = 0;
    for( uint32_t c = 0; c < ctx->plan->count; c++ )
    {
        uint64_t m_start, m_end;
        if( movie_to_media( ctx, at, ctx->plan->interval[c].start, &m_start ) )
            return -1;
        if( movie_to_media( ctx, at, ctx->plan->interval[c].end, &m_end ) )
            return -1;
        int      found         = 0;
        uint64_t first_src_dts = 0;
        uint64_t segment_base  = running_dts;
        for( uint32_t k = 0; k < n; k++ )
        {
            lsmash_sample_t info;
            if( lsmash_get_sample_info_from_media_timeline( root, tid, k + 1, &info ) < 0 )
                return -1;
            uint32_t delta;
            if( lsmash_get_sample_delta_from_media_timeline( root, tid, k + 1, &delta ) < 0 )
                return -1;
            uint64_t s_cts = info.cts;
            uint64_t s_end = s_cts + delta;
            if( !(s_cts < m_end && s_end > m_start) )
                continue;   /* no intersection with this packet */
            if( !found )
            {
                found         = 1;
                first_src_dts = info.dts;
                /* Hide the head overhang inside this first intersecting packet. */
                if( append_edit( ct, segment_base + (m_start - s_cts) ) )
                    return -1;
            }
            uint64_t out_dts = segment_base + (info.dts - first_src_dts);
            if( append_job( ct, k + 1, out_dts, out_dts ) )
                return -1;
            running_dts           = out_dts + delta;
            ct->last_sample_delta = delta;   /* final value = last copied packet's duration */
        }
        if( !found )
            return -1;
    }
    return 0;
}

/*--------------------------------------------------------------------------------*
 * Public entry point
 *--------------------------------------------------------------------------------*/

void cutplan_free_result( cutplan_result_t *result )
{
    if( !result )
        return;
    for( uint32_t i = 0; i < result->num_tracks; i++ )
    {
        lsmash_free( result->track[i].job );
        lsmash_free( result->track[i].edit );
    }
    lsmash_free( result->track );
    lsmash_free( result->cut );
    lsmash_free( result );
}

int cutplan_build
(
    const cutplan_input_t *input,
    const cut_plan_t      *plan,
    cutplan_result_t     **result_out
)
{
    if( !input || !plan || !result_out || plan->count == 0 || input->num_tracks == 0 )
        return -1;
    if( input->movie_timescale == 0 )
        return -1;

    int err = -1;
    build_ctx_t ctx;
    memset( &ctx, 0, sizeof(ctx) );
    ctx.root            = input->root;
    ctx.movie_timescale = input->movie_timescale;
    ctx.plan            = plan;
    ctx.num_bt          = input->num_tracks;
    ctx.bt              = lsmash_malloc_zero( ctx.num_bt * sizeof(build_track_t) );
    if( !ctx.bt )
        return -1;

    /* Restrict the input shape: one avc1 video, at most one mp4a audio, one
     * sample description per track. */
    uint32_t num_video = 0, num_audio = 0;
    for( uint32_t i = 0; i < ctx.num_bt; i++ )
    {
        ctx.bt[i].src = &input->track[i];
        const cutplan_src_track_t *src = ctx.bt[i].src;
        if( src->media_timescale == 0 || src->num_summaries != 1 || !src->summary[0] )
            goto fail;
        if( src->handler_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
        {
            if( ++num_video != 1 )
                goto fail;
            ctx.video = &ctx.bt[i];
        }
        else if( src->handler_type == ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK )
        {
            if( ++num_audio > 1 )
                goto fail;
            ctx.audio = &ctx.bt[i];
        }
        else
            goto fail;
    }
    if( num_video != 1 || !ctx.video )
        goto fail;
    if( !lsmash_check_codec_type_identical( ctx.video->src->summary[0]->sample_type, ISOM_CODEC_TYPE_AVC1_VIDEO ) )
        goto fail;
    if( ctx.audio
     && !lsmash_check_codec_type_identical( ctx.audio->src->summary[0]->sample_type, ISOM_CODEC_TYPE_MP4A_AUDIO ) )
        goto fail;

    /* Validate edit lists and establish the presented media region per track. */
    for( uint32_t i = 0; i < ctx.num_bt; i++ )
        if( validate_edits( &ctx, &ctx.bt[i]) )
            goto fail;
    for( uint32_t i = 0; i < ctx.num_bt; i++ )
    {
        build_track_t *bt = &ctx.bt[i];
        if( bt->has_edit )
            continue;
        if( scale_time( bt->media_hi, bt->src->media_timescale, ctx.movie_timescale, &bt->edit_movie_length ) )
            goto fail;
    }

    /* Cache video samples and verify every RAP is a genuine IDR before relying
     * on any boundary check. */
    if( prepare_video_samples( &ctx, ctx.video ) )
        goto fail;

    /* Every interval must lie in the common presented region of every track and
     * respect IDR display-time boundaries. */
    for( uint32_t c = 0; c < plan->count; c++ )
    {
        const cut_interval_t *it = &plan->interval[c];
        if( it->start >= it->end )
            goto fail;
        for( uint32_t i = 0; i < ctx.num_bt; i++ )
        {
            uint64_t dummy;
            if( movie_to_media( &ctx, &ctx.bt[i], it->start, &dummy ) )
                goto fail;
            if( movie_to_media( &ctx, &ctx.bt[i], it->end, &dummy ) )
                goto fail;
        }
        if( check_idr_boundary( &ctx, ctx.video, it->start ) )
            goto fail;
        uint64_t video_movie_end = ctx.video->edit_movie_start + ctx.video->edit_movie_length;
        if( it->end < video_movie_end && check_idr_boundary( &ctx, ctx.video, it->end ) )
            goto fail;
    }

    if( plan_video( &ctx, ctx.video ) )
        goto fail;
    if( ctx.audio && plan_audio( &ctx, ctx.audio ) )
        goto fail;

    cutplan_result_t *result = lsmash_malloc_zero( sizeof(cutplan_result_t) );
    if( !result )
        goto fail;
    result->movie_timescale = ctx.movie_timescale;
    result->num_cuts        = plan->count;
    result->num_tracks      = ctx.num_bt;
    result->cut             = lsmash_malloc( plan->count * sizeof(cut_interval_t) );
    result->track           = lsmash_malloc_zero( ctx.num_bt * sizeof(cut_track_t) );
    if( !result->cut || !result->track )
    {
        cutplan_free_result( result );
        goto fail;
    }
    memcpy( result->cut, plan->interval, plan->count * sizeof(cut_interval_t) );
    for( uint32_t i = 0; i < ctx.num_bt; i++ )
        result->track[i] = ctx.bt[i].out;   /* transfer job/edit array ownership */

    *result_out = result;
    err = 0;
fail:
    lsmash_free( ctx.vs );
    if( err )
    {
        for( uint32_t i = 0; i < ctx.num_bt; i++ )
        {
            lsmash_free( ctx.bt[i].out.job );
            lsmash_free( ctx.bt[i].out.edit );
        }
    }
    lsmash_free( ctx.bt );
    return err ? -1 : 0;
}
