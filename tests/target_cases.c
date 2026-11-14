/*****************************************************************************
 * target_cases.c
 *****************************************************************************
 * Copyright (C) 2017 L-SMASH project
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.
 *****************************************************************************/

/* This file is available under an ISC license. */

#include "../task_support/case_harness.h"
#include "lsmash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include <inttypes.h>

/*--------------------------------------------------------------------------------*
 * Small shared helpers (L-SMASH and system libraries only)
 *--------------------------------------------------------------------------------*/

static const char *remuxer_bin( void )
{
    const char *p = getenv( "REMUXER_TARGET" );
    return p ? p : ".task-build/remuxer_target";
}

static unsigned tmp_seq;

/* Each call returns a distinct heap path (the short-lived test process leaks
 * these on purpose so the returned strings never alias one another). */
static char *make_tmp( const char *suffix )
{
    const char *dir = getenv( "TMPDIR" );
    if( !dir || !*dir )
        dir = "/tmp";
    char *path = lsmash_malloc( 256 );
    if( !path )
        exit( 1 );
    snprintf( path, 256, "%s/lsmash_cut_%d_%u%s",
              dir, (int)getpid(), tmp_seq++, suffix );
    return path;
}

static int run_remuxer( char *const argv[] )
{
    fflush( stdout );
    fflush( stderr );   /* avoid children flushing inherited stdio buffers */
    pid_t pid = fork();
    if( pid < 0 )
        return -1;
    if( pid == 0 )
    {
        /* Keep the child quiet unless debugging; failures are judged by exit status. */
        if( !getenv( "REMUXER_DEBUG" ) )
        {
            freopen( "/dev/null", "w", stdout );
            freopen( "/dev/null", "w", stderr );
        }
        execv( remuxer_bin(), argv );
        _exit( 127 );
    }
    int status = 0;
    if( waitpid( pid, &status, 0 ) < 0 )
        return -1;
    return WIFEXITED( status ) ? WEXITSTATUS( status ) : -1;
}

static void write_cuts( const char *path, const char *content )
{
    FILE *fp = fopen( path, "w" );
    if( !fp ) { perror( "cuts" ); exit( 1 ); }
    fputs( content, fp );
    fclose( fp );
}

typedef struct
{
    lsmash_root_t             *root;
    lsmash_file_parameters_t   fp;
    uint32_t                   video_tid;
    uint32_t                   audio_tid;
} movie_t;

static int open_movie( const char *path, movie_t *m )
{
    memset( m, 0, sizeof(*m) );
    m->root = lsmash_create_root();
    if( !m->root )
        return -1;
    if( lsmash_open_file( path, 1, &m->fp ) < 0 )
        return -1;
    lsmash_file_t *h = lsmash_set_file( m->root, &m->fp );
    if( !h || lsmash_read_file( h, &m->fp ) < 0 )
        return -1;
    lsmash_movie_parameters_t mp;
    lsmash_initialize_movie_parameters( &mp );
    if( lsmash_get_movie_parameters( m->root, &mp ) )
        return -1;
    m->video_tid = m->audio_tid = 0;
    for( uint32_t i = 1; i <= mp.number_of_tracks; i++ )
    {
        uint32_t tid = lsmash_get_track_ID( m->root, i );
        if( lsmash_construct_timeline( m->root, tid ) )
            return -1;
        lsmash_media_parameters_t media;
        lsmash_initialize_media_parameters( &media );
        if( lsmash_get_media_parameters( m->root, tid, &media ) )
            return -1;
        if( media.handler_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
            m->video_tid = tid;
        else if( media.handler_type == ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK )
            m->audio_tid = tid;
    }
    return 0;
}

static void close_movie( movie_t *m )
{
    lsmash_destroy_root( m->root );
    lsmash_close_file( &m->fp );
}

static int sample_bytes( movie_t *m, uint32_t tid, uint32_t n,
                        uint8_t *buf, uint32_t cap, uint32_t *len, uint64_t *pos )
{
    lsmash_sample_t *s = lsmash_get_sample_from_media_timeline( m->root, tid, n );
    if( !s )
        return -1;
    if( s->length > cap )
    {
        lsmash_delete_sample( s );
        return -1;
    }
    memcpy( buf, s->data, s->length );
    *len = s->length;
    if( pos )
        *pos = s->pos;
    lsmash_delete_sample( s );
    return 0;
}

static int sample_info( movie_t *m, uint32_t tid, uint32_t n, lsmash_sample_t *info )
{
    return lsmash_get_sample_info_from_media_timeline( m->root, tid, n, info );
}

static uint32_t sample_count( movie_t *m, uint32_t tid )
{
    return lsmash_get_sample_count_in_media_timeline( m->root, tid );
}

static int sample_delta( movie_t *m, uint32_t tid, uint32_t n, uint32_t *delta )
{
    return lsmash_get_sample_delta_from_media_timeline( m->root, tid, n, delta );
}

/*--------------------------------------------------------------------------------*
 * Synthetic MP4 fixture builders
 *--------------------------------------------------------------------------------*/

/* Fetch the source's valid avcC bytes so synthetic AVC files always re-open. */
static int get_source_avcc( const char *source, uint8_t **out, uint32_t *out_len )
{
    movie_t m;
    if( open_movie( source, &m ) )
        return -1;
    lsmash_summary_t *sum = lsmash_get_summary( m.root, m.video_tid, 1 );
    int rc = -1;
    if( sum )
    {
        uint32_t ncs = lsmash_count_codec_specific_data( sum );
        for( uint32_t i = 1; i <= ncs; i++ )
        {
            lsmash_codec_specific_t *cs = lsmash_get_codec_specific_data( sum, i );
            if( !cs || cs->type != LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_VIDEO_H264 )
                continue;
            uint8_t *dup = lsmash_malloc( cs->size );
            if( !dup )
                break;
            memcpy( dup, cs->data.unstructured, cs->size );
            *out     = dup;
            *out_len = cs->size;
            rc       = 0;
            break;
        }
        lsmash_cleanup_summary( sum );
    }
    close_movie( &m );
    return rc;
}

/* Append one length-prefixed NAL unit. */
static void put_nalu( uint8_t **p, uint8_t nal_header, const uint8_t *payload, uint32_t payload_len )
{
    uint32_t total = payload_len + 1;
    *(*p)++ = (uint8_t)(total >> 24);
    *(*p)++ = (uint8_t)(total >> 16);
    *(*p)++ = (uint8_t)(total >> 8);
    *(*p)++ = (uint8_t)(total);
    *(*p)++ = nal_header;
    memcpy( *p, payload, payload_len );
    *p += payload_len;
}

static lsmash_sample_t *make_video_sample( uint32_t n, uint64_t dts, uint64_t cts,
                                           int idr, const uint8_t *au, uint32_t au_len )
{
    lsmash_sample_t *s = lsmash_create_sample( au_len );
    if( !s )
        return NULL;
    memcpy( s->data, au, au_len );
    s->dts   = dts;
    s->cts   = cts;
    s->index = 1;
    memset( &s->prop, 0, sizeof(s->prop) );
    if( idr )
        s->prop.ra_flags = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
    (void)n;
    return s;
}

static lsmash_sample_t *make_audio_sample( uint32_t size, uint64_t ts, uint8_t marker )
{
    lsmash_sample_t *s = lsmash_create_sample( size );
    if( !s )
        return NULL;
    memset( s->data, marker, size );
    s->dts   = ts;
    s->cts   = ts;
    s->index = 1;
    memset( &s->prop, 0, sizeof(s->prop) );
    s->prop.ra_flags = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
    return s;
}

/* Build a video-only MP4.  Frames in idr_frame are real IDR access units;
 * frames in fake_sync carry a sync flag but only a non-IDR (intra) NAL unit,
 * so they must never be accepted as cut boundaries.  If extra_edit is non-NULL
 * it is installed as the track's only edit. */
static int build_video_only( const char *path, uint32_t timescale, uint32_t frame_delta,
                             uint32_t num_frames, const uint32_t *idr_frame,
                             uint32_t num_idr, const uint32_t *fake_sync, uint32_t num_fake,
                             const lsmash_edit_t *extra_edit,
                             const uint8_t *avcc, uint32_t avcc_len )
{
    lsmash_root_t *root = lsmash_create_root();
    if( !root )
        return -1;
    lsmash_file_parameters_t fp;
    if( lsmash_open_file( path, 0, &fp ) < 0 )
        return -1;
    fp.major_brand = ISOM_BRAND_TYPE_MP42;
    fp.brand_count = 1;
    fp.brands      = lsmash_malloc( sizeof(lsmash_brand_type) );
    fp.brands[0]   = ISOM_BRAND_TYPE_MP42;
    lsmash_file_t *h = lsmash_set_file( root, &fp );
    if( !h )
        return -1;
    lsmash_movie_parameters_t movie;
    lsmash_initialize_movie_parameters( &movie );
    movie.timescale = timescale;
    lsmash_set_movie_parameters( root, &movie );

    uint32_t tid = lsmash_create_track( root, ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK );
    lsmash_track_parameters_t tp;
    lsmash_initialize_track_parameters( &tp );
    tp.display_width  = 16 << 16;
    tp.display_height = 16 << 16;
    lsmash_set_track_parameters( root, tid, &tp );
    lsmash_media_parameters_t mp;
    lsmash_initialize_media_parameters( &mp );
    mp.timescale          = timescale;
    mp.ISO_language       = ISOM_LANGUAGE_CODE_UNDEFINED;
    mp.media_handler_name = "L-SMASH Video Handler";
    lsmash_set_media_parameters( root, tid, &mp );

    lsmash_video_summary_t *sum = (lsmash_video_summary_t *)lsmash_create_summary( LSMASH_SUMMARY_TYPE_VIDEO );
    sum->sample_type = ISOM_CODEC_TYPE_AVC1_VIDEO;
    sum->width       = 16;
    sum->height      = 16;
    sum->depth       = AVC_DEPTH_COLOR_WITH_NO_ALPHA;
    sum->max_au_length = 64;
    lsmash_codec_specific_t *cs = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_VIDEO_H264,
                                                                     LSMASH_CODEC_SPECIFIC_FORMAT_UNSTRUCTURED );
    cs->data.unstructured = lsmash_malloc( avcc_len );
    memcpy( cs->data.unstructured, avcc, avcc_len );
    cs->size = avcc_len;
    lsmash_add_codec_specific_data( (lsmash_summary_t *)sum, cs );
    lsmash_destroy_codec_specific_data( cs );
    sum->data_ref_index = 1;
    lsmash_data_reference_t dref = { .index = 1, .location = NULL };
    lsmash_create_data_reference( root, tid, &dref, h );
    lsmash_add_sample_entry( root, tid, (lsmash_summary_t *)sum );
    lsmash_cleanup_summary( (lsmash_summary_t *)sum );

    uint8_t au[64];
    for( uint32_t f = 0; f < num_frames; f++ )
    {
        int idr = 0, fake = 0;
        for( uint32_t k = 0; k < num_idr; k++ )
            if( idr_frame[k] == f )
                idr = 1;
        for( uint32_t k = 0; k < num_fake; k++ )
            if( fake_sync[k] == f )
                fake = 1;
        uint8_t *p = au;
        uint8_t sps[4] = { 0xAA, 0x01, 0x02, (uint8_t)f };
        uint8_t pps[3] = { 0xBB, 0x03, 0x04 };
        uint8_t slc[4] = { 0xCC, (uint8_t)(f + 1), 0x10, 0x20 };
        if( idr )
        {
            put_nalu( &p, 0x67, sps, sizeof(sps) );   /* SPS */
            put_nalu( &p, 0x68, pps, sizeof(pps) );   /* PPS */
            put_nalu( &p, 0x65, slc, sizeof(slc) );   /* IDR slice */
        }
        else
        {
            /* Non-IDR VCL NAL unit even for the flagged "intra" frame. */
            put_nalu( &p, 0x41, slc, sizeof(slc) );
        }
        lsmash_sample_t *s = make_video_sample( f, (uint64_t)f * frame_delta,
                                                (uint64_t)f * frame_delta, idr || fake, au, (uint32_t)(p - au) );
        if( fake && !idr )
            s->prop.ra_flags = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;   /* flagged, but no IDR NAL */
        lsmash_append_sample( root, tid, s );
    }
    lsmash_flush_pooled_samples( root, tid, frame_delta );
    if( extra_edit )
        lsmash_create_explicit_timeline_map( root, tid, *extra_edit );
    lsmash_adhoc_remux_t remux = { .func = NULL, .buffer_size = 4 * 1024 * 1024, .param = NULL };
    lsmash_finish_movie( root, &remux );
    lsmash_destroy_root( root );
    lsmash_close_file( &fp );
    lsmash_free( fp.brands );
    return 0;
}

/* Build an A/V MP4 whose AAC packets (1024 ticks each) straddle cut points,
 * with a source edit carrying an encoder-delay style media offset. */
static int build_av( const char *path, uint32_t movie_ts, uint32_t video_ts,
                     uint32_t num_video, uint32_t video_delta,
                     uint32_t audio_ts, uint32_t num_audio, uint32_t audio_delta,
                     uint64_t audio_edit_offset,
                     const uint8_t *avcc, uint32_t avcc_len )
{
    lsmash_root_t *root = lsmash_create_root();
    if( !root )
        return -1;
    lsmash_file_parameters_t fp;
    if( lsmash_open_file( path, 0, &fp ) < 0 )
        return -1;
    fp.major_brand = ISOM_BRAND_TYPE_MP42;
    fp.brand_count = 1;
    fp.brands      = lsmash_malloc( sizeof(lsmash_brand_type) );
    fp.brands[0]   = ISOM_BRAND_TYPE_MP42;
    lsmash_file_t *h = lsmash_set_file( root, &fp );
    if( !h )
        return -1;
    lsmash_movie_parameters_t movie;
    lsmash_initialize_movie_parameters( &movie );
    movie.timescale = movie_ts;
    lsmash_set_movie_parameters( root, &movie );

    /* Video track: every frame is an IDR. */
    uint32_t vtid = lsmash_create_track( root, ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK );
    lsmash_track_parameters_t vtp;
    lsmash_initialize_track_parameters( &vtp );
    vtp.display_width  = 16 << 16;
    vtp.display_height = 16 << 16;
    lsmash_set_track_parameters( root, vtid, &vtp );
    lsmash_media_parameters_t vmp;
    lsmash_initialize_media_parameters( &vmp );
    vmp.timescale          = video_ts;
    vmp.ISO_language       = ISOM_LANGUAGE_CODE_UNDEFINED;
    vmp.media_handler_name = "L-SMASH Video Handler";
    lsmash_set_media_parameters( root, vtid, &vmp );
    lsmash_video_summary_t *vsum = (lsmash_video_summary_t *)lsmash_create_summary( LSMASH_SUMMARY_TYPE_VIDEO );
    vsum->sample_type  = ISOM_CODEC_TYPE_AVC1_VIDEO;
    vsum->width        = 16;
    vsum->height       = 16;
    vsum->depth        = AVC_DEPTH_COLOR_WITH_NO_ALPHA;
    vsum->max_au_length = 64;
    lsmash_codec_specific_t *vcs = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_VIDEO_H264,
                                                                      LSMASH_CODEC_SPECIFIC_FORMAT_UNSTRUCTURED );
    vcs->data.unstructured = lsmash_malloc( avcc_len );
    memcpy( vcs->data.unstructured, avcc, avcc_len );
    vcs->size = avcc_len;
    lsmash_add_codec_specific_data( (lsmash_summary_t *)vsum, vcs );
    lsmash_destroy_codec_specific_data( vcs );
    vsum->data_ref_index = 1;
    lsmash_data_reference_t vdref = { .index = 1, .location = NULL };
    lsmash_create_data_reference( root, vtid, &vdref, h );
    lsmash_add_sample_entry( root, vtid, (lsmash_summary_t *)vsum );
    lsmash_cleanup_summary( (lsmash_summary_t *)vsum );
    uint8_t au[64];
    for( uint32_t f = 0; f < num_video; f++ )
    {
        uint8_t *p = au;
        uint8_t sps[4] = { 0xAA, 0x01, 0x02, (uint8_t)f };
        uint8_t pps[3] = { 0xBB, 0x03, 0x04 };
        uint8_t slc[4] = { 0xCC, (uint8_t)(f + 1), 0x10, 0x20 };
        put_nalu( &p, 0x67, sps, sizeof(sps) );
        put_nalu( &p, 0x68, pps, sizeof(pps) );
        put_nalu( &p, 0x65, slc, sizeof(slc) );
        lsmash_sample_t *s = make_video_sample( f, (uint64_t)f * video_delta,
                                                (uint64_t)f * video_delta, 1, au, (uint32_t)(p - au) );
        lsmash_append_sample( root, vtid, s );
    }
    lsmash_flush_pooled_samples( root, vtid, video_delta );

    /* Audio track: AAC-like packets with a real AudioSpecificConfig. */
    uint32_t atid = lsmash_create_track( root, ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK );
    lsmash_track_parameters_t atp;
    lsmash_initialize_track_parameters( &atp );
    lsmash_set_track_parameters( root, atid, &atp );
    lsmash_media_parameters_t amp;
    lsmash_initialize_media_parameters( &amp );
    amp.timescale          = audio_ts;
    amp.ISO_language       = ISOM_LANGUAGE_CODE_UNDEFINED;
    amp.media_handler_name = "L-SMASH Audio Handler";
    lsmash_set_media_parameters( root, atid, &amp );
    lsmash_audio_summary_t *asum = (lsmash_audio_summary_t *)lsmash_create_summary( LSMASH_SUMMARY_TYPE_AUDIO );
    asum->sample_type      = ISOM_CODEC_TYPE_MP4A_AUDIO;
    asum->aot              = MP4A_AUDIO_OBJECT_TYPE_AAC_LC;
    asum->frequency        = audio_ts;
    asum->channels         = 2;
    asum->sample_size      = 16;
    asum->samples_in_frame = 1024;
    asum->sbr_mode         = MP4A_AAC_SBR_NOT_SPECIFIED;
    asum->max_au_length    = 32;
    asum->data_ref_index   = 1;
    lsmash_setup_AudioSpecificConfig( asum );
    lsmash_data_reference_t adref = { .index = 1, .location = NULL };
    lsmash_create_data_reference( root, atid, &adref, h );
    lsmash_add_sample_entry( root, atid, (lsmash_summary_t *)asum );
    lsmash_cleanup_summary( (lsmash_summary_t *)asum );
    for( uint32_t f = 0; f < num_audio; f++ )
    {
        lsmash_sample_t *s = make_audio_sample( 8, (uint64_t)f * audio_delta, (uint8_t)(0x40 + (f & 0x3f)) );
        lsmash_append_sample( root, atid, s );
    }
    lsmash_flush_pooled_samples( root, atid, audio_delta );

    /* One normal edit selecting the media after an encoder-delay offset. */
    if( audio_edit_offset )
    {
        lsmash_edit_t edit;
        edit.duration   = (uint64_t)num_audio * audio_delta * movie_ts / audio_ts;
        edit.start_time = (int64_t)audio_edit_offset;
        edit.rate       = ISOM_EDIT_MODE_NORMAL;
        lsmash_create_explicit_timeline_map( root, atid, edit );
    }
    lsmash_adhoc_remux_t remux = { .func = NULL, .buffer_size = 4 * 1024 * 1024, .param = NULL };
    lsmash_finish_movie( root, &remux );
    lsmash_destroy_root( root );
    lsmash_close_file( &fp );
    lsmash_free( fp.brands );
    return 0;
}

/*--------------------------------------------------------------------------------*
 * Reference expectation logic, independently re-derived from the source
 *--------------------------------------------------------------------------------*/

/* Reference video GOP selection: for each cut, expand from the IDR shown at the
 * start up to the IDR shown at the end (exclusive), in decode order. */
static uint32_t ref_video_sequence( movie_t *src, uint32_t tid,
                                    const uint64_t *cut_start_media, const uint64_t *cut_end_media,
                                    uint32_t num_cuts, uint32_t *out, uint32_t cap )
{
    uint32_t n = sample_count( src, tid );
    uint32_t w = 0;
    for( uint32_t c = 0; c < num_cuts; c++ )
    {
        uint32_t first = 0, last = n;
        for( uint32_t k = 1; k <= n; k++ )
        {
            lsmash_sample_t info;
            sample_info( src, tid, k, &info );
            if( info.cts == cut_start_media[c] )
                first = k;
            if( info.cts == cut_end_media[c] )
            {
                last = k - 1;
                break;
            }
        }
        for( uint32_t k = first; k <= last && w < cap; k++ )
            out[w++] = k;
    }
    return w;
}

/* Reference audio selection: every packet intersecting the media interval. */
static uint32_t ref_audio_sequence( movie_t *src, uint32_t tid,
                                    uint64_t m_start, uint64_t m_end,
                                    uint32_t *out, uint32_t cap )
{
    uint32_t n = sample_count( src, tid );
    uint32_t w = 0;
    for( uint32_t k = 1; k <= n && w < cap; k++ )
    {
        lsmash_sample_t info;
        sample_info( src, tid, k, &info );
        uint32_t delta;
        sample_delta( src, tid, k, &delta );
        if( info.cts < m_end && info.cts + delta > m_start )
            out[w++] = k;
    }
    return w;
}

static int bytes_equal( movie_t *a, uint32_t ta, uint32_t na,
                        movie_t *b, uint32_t tb, uint32_t nb )
{
    static uint8_t ba[1 << 16], bb[1 << 16];
    uint32_t la, lb;
    if( sample_bytes( a, ta, na, ba, sizeof(ba), &la, NULL ) )
        return 0;
    if( sample_bytes( b, tb, nb, bb, sizeof(bb), &lb, NULL ) )
        return 0;
    return la == lb && memcmp( ba, bb, la ) == 0;
}

/* Map a movie-time point into a track's media time using its normal edit and
 * exact integer scaling, independently of the production cut code. */
static int movie_to_media_ref( movie_t *m, uint32_t tid, uint32_t movie_ts,
                               uint64_t movie_t, uint64_t *media_t )
{
    lsmash_edit_t ed;
    if( lsmash_get_explicit_timeline_map( m->root, tid, 1, &ed ) < 0 || ed.start_time < 0 )
        return -1;
    lsmash_media_parameters_t mp;
    lsmash_initialize_media_parameters( &mp );
    if( lsmash_get_media_parameters( m->root, tid, &mp ) )
        return -1;
    uint64_t scaled = movie_t * (uint64_t)mp.timescale;
    if( scaled % movie_ts )
        return -1;
    *media_t = (uint64_t)ed.start_time + scaled / movie_ts;
    return 0;
}

/*--------------------------------------------------------------------------------*
 * test_reorder: source.mp4 + cuts.txt, full byte and timing verification
 *--------------------------------------------------------------------------------*/

void test_reorder( void )
{
    const char *source = "task_support/source.mp4";
    const char *cuts   = "task_support/cuts.txt";
    char *out = make_tmp( "_reorder.mp4" );
    unlink( out );

    CASE();
    {
        char *argv[] = { (char *)remuxer_bin(), (char *)"-i", (char *)source,
                         (char *)"--cuts", (char *)cuts, (char *)"-o", out, NULL };
        CHECK( run_remuxer( argv ) == 0 );
    }

    movie_t src, dst;
    CASE();
    CHECK( open_movie( source, &src ) == 0 );
    CHECK( open_movie( out, &dst ) == 0 );

    lsmash_movie_parameters_t dmp;
    lsmash_initialize_movie_parameters( &dmp );
    CASE();
    CHECK( lsmash_get_movie_parameters( dst.root, &dmp ) == 0 );
    CHECK( dmp.timescale == 1000 );
    CHECK( dmp.duration == 3000 );                 /* sum of (end-start) */
    CHECK( dst.video_tid && dst.audio_tid );

    /* Movie-time cuts mapped into each media timescale through the source edit. */
    uint64_t cut_start_movie[3] = { 1000, 0, 1000 };
    uint64_t cut_end_movie[3]   = { 2000, 1000, 2000 };
    uint64_t vstart[3], vend[3];
    CASE();
    for( int c = 0; c < 3; c++ )
    {
        CHECK( movie_to_media_ref( &src, src.video_tid, 1000, cut_start_movie[c], &vstart[c] ) == 0 );
        CHECK( movie_to_media_ref( &src, src.video_tid, 1000, cut_end_movie[c], &vend[c] ) == 0 );
    }
    {
        uint32_t expect[64];
        uint32_t nexp = ref_video_sequence( &src, src.video_tid, vstart, vend, 3, expect, 64 );
        CHECK( nexp == 18 );
        CHECK( sample_count( &dst, dst.video_tid ) == nexp );
        for( uint32_t i = 0; i < nexp; i++ )
            CHECK( bytes_equal( &dst, dst.video_tid, i + 1, &src, src.video_tid, expect[i] ) );
        /* Repeated shots are real copies: identical bytes, distinct file offsets. */
        uint64_t p1, p2;
        static uint8_t scratch[1 << 16];
        uint32_t l1;
        CHECK( sample_bytes( &dst, dst.video_tid, 1, scratch, sizeof(scratch), &l1, &p1 ) == 0 );
        CHECK( sample_bytes( &dst, dst.video_tid, 13, scratch, sizeof(scratch), &l1, &p2 ) == 0 );
        CHECK( p1 != p2 );
    }

    /* Video timing: DTS from 0, contiguous deltas, preserved cts-dts offsets. */
    CASE();
    {
        uint32_t n = sample_count( &dst, dst.video_tid );
        uint64_t prev_dts = 0;
        for( uint32_t i = 1; i <= n; i++ )
        {
            lsmash_sample_t di, si;
            sample_info( &dst, dst.video_tid, i, &di );
            if( i == 1 )
                CHECK( di.dts == 0 );
            else
            {
                lsmash_sample_t prev;
                sample_info( &dst, dst.video_tid, i - 1, &prev );
                uint32_t pd;
                sample_delta( &dst, dst.video_tid, i - 1, &pd );
                CHECK( di.dts == prev.dts + pd );
                (void)prev_dts;
            }
            (void)si;
        }
        /* cts-dts offsets match the corresponding source samples. */
        uint32_t expect[64];
        uint32_t nexp = ref_video_sequence( &src, src.video_tid, vstart, vend, 3, expect, 64 );
        for( uint32_t i = 0; i < nexp; i++ )
        {
            lsmash_sample_t di, si;
            sample_info( &dst, dst.video_tid, i + 1, &di );
            sample_info( &src, src.video_tid, expect[i], &si );
            CHECK( (di.cts - di.dts) == (si.cts - si.dts) );
        }
    }

    /* Audio: intersection-based packets in the right order, real copies. */
    CASE();
    {
        uint32_t expect[256];
        uint32_t nexp = 0;
        for( int c = 0; c < 3; c++ )
        {
            uint64_t ms, me;
            CHECK( movie_to_media_ref( &src, src.audio_tid, 1000, cut_start_movie[c], &ms ) == 0 );
            CHECK( movie_to_media_ref( &src, src.audio_tid, 1000, cut_end_movie[c], &me ) == 0 );
            nexp += ref_audio_sequence( &src, src.audio_tid, ms, me,
                                        expect + nexp, (uint32_t)(256 - nexp) );
        }
        CHECK( sample_count( &dst, dst.audio_tid ) == nexp );
        for( uint32_t i = 0; i < nexp; i++ )
            CHECK( bytes_equal( &dst, dst.audio_tid, i + 1, &src, src.audio_tid, expect[i] ) );
        /* A shared boundary packet is copied twice with different offsets. */
        uint64_t p1, p2;
        static uint8_t scratch[256];
        uint32_t l1;
        CHECK( sample_bytes( &dst, dst.audio_tid, 48, scratch, sizeof(scratch), &l1, &p1 ) == 0 );
        CHECK( sample_bytes( &dst, dst.audio_tid, 96, scratch, sizeof(scratch), &l1, &p2 ) == 0 );
        CHECK( p1 != p2 );
    }

    /* Three independent edits per track with exact durations. */
    CASE();
    {
        uint32_t tids[2] = { dst.video_tid, dst.audio_tid };
        for( int t = 0; t < 2; t++ )
        {
            CHECK( lsmash_count_explicit_timeline_map( dst.root, tids[t] ) == 3 );
            uint64_t total = 0;
            for( uint32_t e = 1; e <= 3; e++ )
            {
                lsmash_edit_t ed;
                CHECK( lsmash_get_explicit_timeline_map( dst.root, tids[t], e, &ed ) == 0 );
                CHECK( ed.rate == ISOM_EDIT_MODE_NORMAL );
                CHECK( ed.duration == 1000 );
                CHECK( ed.start_time >= 0 );
                total += ed.duration;
            }
            CHECK( total == 3000 );
        }
    }

    close_movie( &src );
    close_movie( &dst );
    unlink( out );
}

/*--------------------------------------------------------------------------------*
 * test_audio_edges: packets straddle boundaries; shared packet copied per cut
 *--------------------------------------------------------------------------------*/

void test_audio_edges( void )
{
    uint8_t *avcc = NULL;
    uint32_t avcc_len = 0;
    CASE();
    CHECK( get_source_avcc( "task_support/source.mp4", &avcc, &avcc_len ) == 0 );

    char *inp = make_tmp( "_av.mp4" );
    char *out = make_tmp( "_av_out.mp4" );
    char *cf  = make_tmp( "_av.cuts" );
    /* movie 1000; video 1000, 4 frames @250; audio 48000, 48 packets @1024,
     * edit offset 1024. Cuts at 500ms -> media 24000, not 1024-aligned. */
    CASE();
    CHECK( build_av( inp, 1000, 1000, 4, 250, 48000, 48, 1024, 1024, avcc, avcc_len ) == 0 );
    write_cuts( cf, "0 500\n500 1000\n" );
    unlink( out );
    {
        char *argv[] = { (char *)remuxer_bin(), (char *)"-i", inp,
                         (char *)"--cuts", cf, (char *)"-o", out, NULL };
        CHECK( run_remuxer( argv ) == 0 );
    }

    movie_t src, dst;
    CASE();
    CHECK( open_movie( inp, &src ) == 0 );
    CHECK( open_movie( out, &dst ) == 0 );

    /* Media interval per cut: offset 1024 + movie*48. */
    CASE();
    {
        uint64_t s0 = 1024,            e0 = 1024 + 24000;
        uint64_t s1 = 1024 + 24000,    e1 = 1024 + 48000;
        uint32_t a[128], b[128];
        uint32_t na = ref_audio_sequence( &src, src.audio_tid, s0, e0, a, 128 );
        uint32_t nb = ref_audio_sequence( &src, src.audio_tid, s1, e1, b, 128 );
        CHECK( na > 0 && nb > 0 );
        CHECK( sample_count( &dst, dst.audio_tid ) == na + nb );
        for( uint32_t i = 0; i < na; i++ )
            CHECK( bytes_equal( &dst, dst.audio_tid, i + 1, &src, src.audio_tid, a[i] ) );
        for( uint32_t i = 0; i < nb; i++ )
            CHECK( bytes_equal( &dst, dst.audio_tid, na + i + 1, &src, src.audio_tid, b[i] ) );
        /* The packet straddling the boundary is selected by both cuts. */
        int shared = 0;
        for( uint32_t i = 0; i < na && !shared; i++ )
            for( uint32_t j = 0; j < nb; j++ )
                if( a[i] == b[j] )
                    shared = 1;
        CHECK( shared );
        /* Two edits, one per cut, hiding the head overhang. */
        CHECK( lsmash_count_explicit_timeline_map( dst.root, dst.audio_tid ) == 2 );
        lsmash_edit_t ed0, ed1;
        CHECK( lsmash_get_explicit_timeline_map( dst.root, dst.audio_tid, 1, &ed0 ) == 0 );
        CHECK( lsmash_get_explicit_timeline_map( dst.root, dst.audio_tid, 2, &ed1 ) == 0 );
        CHECK( ed0.duration == 500 && ed1.duration == 500 );
        CHECK( ed0.start_time >= 0 && ed1.start_time >= 0 );
    }

    lsmash_free( avcc );
    close_movie( &src );
    close_movie( &dst );
    unlink( inp );
    unlink( out );
    unlink( cf );
}

/*--------------------------------------------------------------------------------*
 * test_mapping: edit display start/end, total duration, sync points preserved
 *--------------------------------------------------------------------------------*/

void test_mapping( void )
{
    const char *source = "task_support/source.mp4";
    const char *cuts   = "task_support/cuts.txt";
    char *out = make_tmp( "_mapping.mp4" );
    unlink( out );
    CASE();
    {
        char *argv[] = { (char *)remuxer_bin(), (char *)"-i", (char *)source,
                         (char *)"--cuts", (char *)cuts, (char *)"-o", out, NULL };
        CHECK( run_remuxer( argv ) == 0 );
    }
    movie_t dst;
    CASE();
    CHECK( open_movie( out, &dst ) == 0 );

    /* Each edit's cumulative display window is 1s; total 3s. */
    CASE();
    {
        uint32_t tid = dst.video_tid;
        uint64_t cursor = 0;
        for( uint32_t e = 1; e <= 3; e++ )
        {
            lsmash_edit_t ed;
            CHECK( lsmash_get_explicit_timeline_map( dst.root, tid, e, &ed ) == 0 );
            CHECK( ed.duration == 1000 );
            cursor += ed.duration;
        }
        CHECK( cursor == 3000 );
        lsmash_movie_parameters_t mp;
        lsmash_initialize_movie_parameters( &mp );
        lsmash_get_movie_parameters( dst.root, &mp );
        CHECK( mp.duration == 3000 );
        /* Track duration in movie timescale equals the edit sum. */
        CHECK( lsmash_get_track_duration( dst.root, tid ) == 3000 );
    }

    /* Sync samples survive: the first sample of each copied GOP is a sync point. */
    CASE();
    {
        uint32_t n = sample_count( &dst, dst.video_tid );
        uint32_t syncs = 0;
        for( uint32_t i = 1; i <= n; i++ )
        {
            lsmash_sample_t info;
            sample_info( &dst, dst.video_tid, i, &info );
            if( info.prop.ra_flags & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC )
                ++syncs;
        }
        CHECK( syncs == 3 );   /* one IDR per cut */
    }

    /* Audio edit media times strictly increase and stay within the media. */
    CASE();
    {
        uint64_t media_dur = lsmash_get_media_duration_from_media_timeline( dst.root, dst.audio_tid );
        int64_t prev = -1;
        for( uint32_t e = 1; e <= 3; e++ )
        {
            lsmash_edit_t ed;
            CHECK( lsmash_get_explicit_timeline_map( dst.root, dst.audio_tid, e, &ed ) == 0 );
            CHECK( ed.start_time > prev );
            CHECK( (uint64_t)ed.start_time < media_dur );
            prev = ed.start_time;
        }
    }

    close_movie( &dst );
    unlink( out );
}

/*--------------------------------------------------------------------------------*
 * test_reject: malformed plans, bounds, non-IDR boundaries, option conflicts
 *--------------------------------------------------------------------------------*/

static int reject_cuts( const char *content, const char *input, const char *extra_opt )
{
    char *cf  = make_tmp( "_rej.cuts" );
    char *out = make_tmp( "_rej.mp4" );
    write_cuts( cf, content );
    unlink( out );
    int rc;
    if( extra_opt )
    {
        char *argv[] = { (char *)remuxer_bin(), (char *)"-i", (char *)input,
                         (char *)extra_opt, (char *)"--cuts", cf, (char *)"-o", out, NULL };
        rc = run_remuxer( argv );
    }
    else
    {
        char *argv[] = { (char *)remuxer_bin(), (char *)"-i", (char *)input,
                         (char *)"--cuts", cf, (char *)"-o", out, NULL };
        rc = run_remuxer( argv );
    }
    unlink( cf );
    int no_output = access( out, F_OK ) != 0;
    unlink( out );
    return rc != 0 && no_output;
}

void test_reject( void )
{
    const char *source = "task_support/source.mp4";
    char *out = make_tmp( "_single.mp4" );

    /* Malformed lines and empty plans. */
    CASE();
    CHECK( reject_cuts( "1000\n", source, NULL ) );                 /* missing end */
    CHECK( reject_cuts( "abc 2000\n", source, NULL ) );             /* non-numeric */
    CHECK( reject_cuts( "2000 1000\n", source, NULL ) );           /* start >= end */
    CHECK( reject_cuts( "1000 2000 extra\n", source, NULL ) );     /* trailing junk */
    CHECK( reject_cuts( "\n  \n\t\n", source, NULL ) );            /* empty plan */

    /* Out of bounds (movie is 3000 ticks long). */
    CASE();
    CHECK( reject_cuts( "0 9000\n", source, NULL ) );
    CHECK( reject_cuts( "2500 3500\n", source, NULL ) );

    /* Non-IDR boundary: IDR frames only at 0, 3 in a six-frame video. */
    uint8_t *avcc = NULL;
    uint32_t avcc_len = 0;
    CASE();
    CHECK( get_source_avcc( source, &avcc, &avcc_len ) == 0 );
    char *vid = make_tmp( "_nonidr.mp4" );
    CASE();
    {
        uint32_t idr[2] = { 0, 3 };
        CHECK( build_video_only( vid, 600, 100, 6, idr, 2, NULL, 0, NULL, avcc, avcc_len ) == 0 );
    }
    CASE();
    CHECK( reject_cuts( "100 200\n", vid, NULL ) );    /* starts on a P frame */
    CHECK( reject_cuts( "0 100\n", vid, NULL ) );      /* ends on a P frame */
    CHECK( reject_cuts( "0 700\n", vid, NULL ) );      /* beyond media */

    /* A flagged sync sample that is only a non-IDR intra must not count. */
    char *fake = make_tmp( "_fakeidr.mp4" );
    CASE();
    {
        uint32_t idr[1] = { 0 };
        uint32_t fakesync[1] = { 2 };
        CHECK( build_video_only( fake, 600, 100, 4, idr, 1, fakesync, 1, NULL, avcc, avcc_len ) == 0 );
    }
    CASE();
    CHECK( reject_cuts( "200 300\n", fake, NULL ) );   /* boundary on the fake-IDR intra */

    /* Non-integer movie->media conversion must be rejected: movie 600, media 1000. */
    char *odd = make_tmp( "_oddts.mp4" );
    CASE();
    {
        uint32_t idr_all[2] = { 0, 1 };   /* both frames IDR so only conversion can fail */
        CHECK( build_video_only( odd, 600, 1000, 2, idr_all, 2, NULL, 0, NULL, avcc, avcc_len ) == 0 );
    }
    CASE();
    CHECK( reject_cuts( "1 600\n", odd, NULL ) );     /* 1 movie tick is not an integer media tick */

    /* A dwell (rate 0) edit is an unsupported edit shape. */
    char *dwell = make_tmp( "_dwell.mp4" );
    CASE();
    {
        uint32_t idr[2] = { 0, 3 };
        lsmash_edit_t dwell_edit = { .duration = 300, .start_time = 0, .rate = ISOM_EDIT_MODE_DWELL };
        CHECK( build_video_only( dwell, 600, 100, 6, idr, 2, NULL, 0, &dwell_edit, avcc, avcc_len ) == 0 );
    }
    CASE();
    CHECK( reject_cuts( "0 300\n", dwell, NULL ) );    /* dwell edit must be rejected */

    /* A legal single-video cut must succeed (video-only input support). */
    CASE();
    {
        char *cf = make_tmp( "_ok.cuts" );
        write_cuts( cf, "0 300\n300 600\n" );
        unlink( out );
        char *argv[] = { (char *)remuxer_bin(), (char *)"-i", vid,
                         (char *)"--cuts", cf, (char *)"-o", out, NULL };
        CHECK( run_remuxer( argv ) == 0 );
        movie_t m;
        CHECK( open_movie( out, &m ) == 0 );
        CHECK( m.video_tid && !m.audio_tid );
        CHECK( sample_count( &m, m.video_tid ) == 6 );
        CHECK( lsmash_count_explicit_timeline_map( m.root, m.video_tid ) == 2 );
        close_movie( &m );
        unlink( cf );
        unlink( out );
    }

    /* Option conflicts. */
    CASE();
    {
        char *cf = make_tmp( "_conf.cuts" );
        write_cuts( cf, "0 1000\n" );
        char *o1 = make_tmp( "_c1.mp4" );
        char *o2 = make_tmp( "_c2.mp4" );
        char *o3 = make_tmp( "_c3.mp4" );
        unlink( o1 ); unlink( o2 ); unlink( o3 );

        /* fragmentation */
        char *frag[] = { (char *)remuxer_bin(), (char *)"-i", (char *)source,
                         (char *)"--fragment", (char *)"1", (char *)"--cuts", cf, (char *)"-o", o1, NULL };
        CHECK( run_remuxer( frag ) != 0 );
        CHECK( access( o1, F_OK ) != 0 );

        /* multiple inputs */
        char *multi[] = { (char *)remuxer_bin(), (char *)"-i", (char *)source, (char *)"-i", (char *)source,
                          (char *)"--cuts", cf, (char *)"-o", o2, NULL };
        CHECK( run_remuxer( multi ) != 0 );
        CHECK( access( o2, F_OK ) != 0 );

        /* track remove */
        char *rm_in[64];
        char spec[256];
        snprintf( spec, sizeof(spec), "%s?1:remove", source );
        char *rmv[] = { (char *)remuxer_bin(), (char *)"-i", spec,
                        (char *)"--cuts", cf, (char *)"-o", o3, NULL };
        (void)rm_in;
        CHECK( run_remuxer( rmv ) != 0 );
        CHECK( access( o3, F_OK ) != 0 );

        /* seek conflict */
        char *o4 = make_tmp( "_c4.mp4" );
        unlink( o4 );
        char seek_spec[256];
        snprintf( seek_spec, sizeof(seek_spec), "%s?1:seek=1", source );
        char *seekv[] = { (char *)remuxer_bin(), (char *)"-i", seek_spec,
                          (char *)"--cuts", cf, (char *)"-o", o4, NULL };
        CHECK( run_remuxer( seekv ) != 0 );
        CHECK( access( o4, F_OK ) != 0 );

        unlink( cf );
        unlink( o1 ); unlink( o2 ); unlink( o3 ); unlink( o4 );
    }

    lsmash_free( avcc );
    unlink( vid );
    unlink( fake );
    unlink( odd );
    unlink( dwell );
}
