/* Target cases for the lossless cut/reorder remuxer.
 * No main() here: task_support/target_main.c drives these functions. */

#include "../task_support/case_harness.h"

#include "lsmash.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/wait.h>

#define REMUXER    ".task-build/remuxer_target"
#define SOURCE_MP4 "task_support/source.mp4"

typedef struct
{
    uint64_t dts;
    uint64_t cts;
    uint32_t delta;
    uint32_t length;
    uint64_t pos;
    unsigned ra;
    uint8_t *data;
} sample_snapshot_t;

typedef struct
{
    uint32_t            count;
    sample_snapshot_t  *samples;
    uint32_t            timescale;
    uint32_t            num_edits;
    lsmash_edit_t      *edits;
    uint32_t            width;
    uint32_t            height;
    uint16_t            language;
} track_snapshot_t;

typedef struct
{
    lsmash_root_t          *root;
    lsmash_file_parameters_t file_param;
    uint32_t                num_tracks;
    uint32_t                movie_timescale;
    uint64_t                movie_duration;
    track_snapshot_t       *tracks;
} movie_snapshot_t;

static const char *tmp_dir( void )
{
    const char *d = getenv( "TMPDIR" );
    return (d && *d) ? d : "/tmp";
}

static void write_text_file( const char *path, const char *content )
{
    FILE *fp = fopen( path, "w" );
    CHECK( fp != NULL );
    fputs( content, fp );
    fclose( fp );
}

static int run_remuxer( const char *args )
{
    char cmd[1024];
    /* Expected-failure cases print diagnostics by design; keep test output clean. */
    snprintf( cmd, sizeof(cmd), "%s %s 2>/dev/null", REMUXER, args );
    int status = system( cmd );
    if( status == -1 )
        return -1;
    return WEXITSTATUS( status );
}

static int open_snapshot( const char *path, movie_snapshot_t *m )
{
    memset( m, 0, sizeof(*m) );
    m->root = lsmash_create_root();
    if( !m->root )
        return -1;
    if( lsmash_open_file( path, 1, &m->file_param ) < 0 )
        return -1;
    lsmash_file_t *fh = lsmash_set_file( m->root, &m->file_param );
    if( !fh || lsmash_read_file( fh, &m->file_param ) < 0 )
        return -1;
    lsmash_movie_parameters_t mp;
    lsmash_initialize_movie_parameters( &mp );
    if( lsmash_get_movie_parameters( m->root, &mp ) )
        return -1;
    m->movie_timescale = mp.timescale;
    m->movie_duration  = mp.duration;
    m->num_tracks      = mp.number_of_tracks;
    m->tracks = lsmash_malloc_zero( m->num_tracks * sizeof(track_snapshot_t) );
    if( !m->tracks )
        return -1;
    for( uint32_t i = 1; i <= m->num_tracks; i++ )
    {
        track_snapshot_t *t = &m->tracks[i-1];
        uint32_t track_ID = lsmash_get_track_ID( m->root, i );
        if( !track_ID || lsmash_construct_timeline( m->root, track_ID ) )
            return -1;
        lsmash_media_parameters_t media;
        lsmash_initialize_media_parameters( &media );
        if( lsmash_get_media_parameters( m->root, track_ID, &media ) )
            return -1;
        t->timescale = media.timescale;
        t->language  = media.ISO_language;
        lsmash_track_parameters_t tp;
        lsmash_initialize_track_parameters( &tp );
        if( lsmash_get_track_parameters( m->root, track_ID, &tp ) == 0 )
        {
            t->width  = tp.display_width >> 16;
            t->height = tp.display_height >> 16;
        }
        t->count = lsmash_get_sample_count_in_media_timeline( m->root, track_ID );
        t->samples = lsmash_malloc_zero( t->count * sizeof(sample_snapshot_t) );
        if( !t->samples )
            return -1;
        for( uint32_t s = 1; s <= t->count; s++ )
        {
            lsmash_sample_t *sm = lsmash_get_sample_from_media_timeline( m->root, track_ID, s );
            if( !sm )
                return -1;
            sample_snapshot_t *d = &t->samples[s-1];
            d->dts    = sm->dts;
            d->cts    = sm->cts;
            d->length = sm->length;
            d->pos    = sm->pos;
            d->ra     = sm->prop.ra_flags;
            d->data   = lsmash_malloc( sm->length );
            if( !d->data )
                return -1;
            memcpy( d->data, sm->data, sm->length );
            if( lsmash_get_sample_delta_from_media_timeline( m->root, track_ID, s, &d->delta ) )
                return -1;
            lsmash_delete_sample( sm );
        }
        t->num_edits = lsmash_count_explicit_timeline_map( m->root, track_ID );
        t->edits = lsmash_malloc_zero( (t->num_edits ? t->num_edits : 1) * sizeof(lsmash_edit_t) );
        if( !t->edits )
            return -1;
        for( uint32_t e = 1; e <= t->num_edits; e++ )
            if( lsmash_get_explicit_timeline_map( m->root, track_ID, e, &t->edits[e-1] ) )
                return -1;
    }
    return 0;
}

static void close_snapshot( movie_snapshot_t *m )
{
    if( !m->root )
        return;
    for( uint32_t i = 0; i < m->num_tracks; i++ )
    {
        track_snapshot_t *t = &m->tracks[i];
        for( uint32_t s = 0; s < t->count; s++ )
            lsmash_freep( &t->samples[s].data );
        lsmash_freep( &t->samples );
        lsmash_freep( &t->edits );
    }
    lsmash_freep( &m->tracks );
    lsmash_destroy_root( m->root );
    lsmash_close_file( &m->file_param );
}

static int same_bytes( const sample_snapshot_t *a, const sample_snapshot_t *b )
{
    return a->length == b->length && memcmp( a->data, b->data, a->length ) == 0;
}

/* ---- synthetic fixture construction ---- */

typedef struct
{
    uint32_t dts;
    uint32_t cts;
    uint32_t delta;
    uint8_t  nalu_type;     /* AVC NAL unit type of the sole NAL unit */
    int      sync;
    uint8_t  fill;
} synth_video_sample_t;

static void put_be32( uint8_t *p, uint32_t v )
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Build a 3-second MP4 with one avc1 track (6 IDR pictures) and, optionally,
 * one mp4a track whose 10 packets (duration 300 in a 1000 timescale) straddle
 * the 500/1500 movie-time boundaries.  Sample descriptions are reused from
 * task_support/source.mp4. */
static int build_synth_mp4( const char *path, int with_audio, int non_idr_intra )
{
    movie_snapshot_t src;
    if( open_snapshot( SOURCE_MP4, &src ) )
        return -1;
    lsmash_root_t *root = lsmash_create_root();
    lsmash_file_parameters_t fp = { 0 };
    int rc = -1;
    if( !root || lsmash_open_file( path, 0, &fp ) < 0 )
        goto fail_src;
    fp.major_brand   = src.file_param.major_brand;
    fp.minor_version = src.file_param.minor_version;
    fp.brands        = src.file_param.brands;
    fp.brand_count   = src.file_param.brand_count;
    lsmash_file_t *fh = lsmash_set_file( root, &fp );
    if( !fh )
        goto fail_out;
    lsmash_movie_parameters_t mp;
    lsmash_initialize_movie_parameters( &mp );
    mp.timescale = 1000;
    if( lsmash_set_movie_parameters( root, &mp ) )
        goto fail_out;
    uint32_t src_video_ID = lsmash_get_track_ID( src.root, 1 );
    uint32_t video_ID = lsmash_create_track( root, ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK );
    lsmash_track_parameters_t tp;
    lsmash_initialize_track_parameters( &tp );
    lsmash_get_track_parameters( src.root, src_video_ID, &tp );
    tp.track_ID = video_ID;
    lsmash_set_track_parameters( root, video_ID, &tp );
    lsmash_media_parameters_t vmp;
    lsmash_initialize_media_parameters( &vmp );
    lsmash_get_media_parameters( src.root, src_video_ID, &vmp );
    vmp.timescale = 1000;
    lsmash_set_media_parameters( root, video_ID, &vmp );
    lsmash_data_reference_t dref = { .index = 1, .location = NULL };
    if( lsmash_create_data_reference( root, video_ID, &dref, fh ) < 0 )
        goto fail_out;
    lsmash_summary_t *vsum = lsmash_get_summary( src.root, src_video_ID, 1 );
    if( !vsum )
        goto fail_out;
    vsum->data_ref_index = 1;
    if( lsmash_add_sample_entry( root, video_ID, vsum ) == 0 )
    {
        lsmash_cleanup_summary( vsum );
        goto fail_out;
    }
    lsmash_cleanup_summary( vsum );
    static const synth_video_sample_t vid[6] =
    {
        {    0,    0, 500, 5, 1, 0x10 },
        {  500,  500, 500, 1, 1, 0x20 },   /* non-IDR intra when requested */
        { 1000, 1000, 500, 5, 1, 0x30 },
        { 1500, 1500, 500, 5, 1, 0x40 },
        { 2000, 2000, 500, 5, 1, 0x50 },
        { 2500, 2500, 500, 5, 1, 0x60 }
    };
    for( uint32_t i = 0; i < 6; i++ )
    {
        uint8_t nal_type = non_idr_intra ? vid[i].nalu_type : 5;
        uint8_t buf[9];
        put_be32( buf, 5 );
        buf[4] = (uint8_t)(0x60 | nal_type);   /* nal_ref_idc = 3 */
        memset( buf + 5, vid[i].fill, 4 );
        lsmash_sample_t *s = lsmash_create_sample( sizeof(buf) );
        if( !s )
            goto fail_out;
        memcpy( s->data, buf, sizeof(buf) );
        s->dts    = vid[i].dts;
        s->cts    = vid[i].cts;
        s->index  = 1;
        if( !vid[i].sync )
            s->prop.ra_flags = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE;
        else if( non_idr_intra && i == 1 )
            /* A random-accessible intra that is not an IDR: must not pass. */
            s->prop.ra_flags = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_RAP;
        else
            s->prop.ra_flags = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
        if( lsmash_append_sample( root, video_ID, s ) < 0 )
        {
            lsmash_delete_sample( s );
            goto fail_out;
        }
    }
    if( lsmash_flush_pooled_samples( root, video_ID, 500 ) )
        goto fail_out;
    lsmash_edit_t edit = { .duration = 3000, .start_time = 0, .rate = ISOM_EDIT_MODE_NORMAL };
    if( lsmash_create_explicit_timeline_map( root, video_ID, edit ) )
        goto fail_out;
    if( with_audio )
    {
        uint32_t src_audio_ID = lsmash_get_track_ID( src.root, 2 );
        uint32_t audio_ID = lsmash_create_track( root, ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK );
        lsmash_track_parameters_t atp;
        lsmash_initialize_track_parameters( &atp );
        lsmash_get_track_parameters( src.root, src_audio_ID, &atp );
        atp.track_ID = audio_ID;
        lsmash_set_track_parameters( root, audio_ID, &atp );
        lsmash_media_parameters_t amp;
        lsmash_initialize_media_parameters( &amp );
        lsmash_get_media_parameters( src.root, src_audio_ID, &amp );
        amp.timescale = 1000;
        lsmash_set_media_parameters( root, audio_ID, &amp );
        if( lsmash_create_data_reference( root, audio_ID, &dref, fh ) < 0 )
            goto fail_out;
        lsmash_summary_t *asum = lsmash_get_summary( src.root, src_audio_ID, 1 );
        if( !asum )
            goto fail_out;
        asum->data_ref_index = 1;
        if( lsmash_add_sample_entry( root, audio_ID, asum ) == 0 )
        {
            lsmash_cleanup_summary( asum );
            goto fail_out;
        }
        lsmash_cleanup_summary( asum );
        for( uint32_t i = 0; i < 10; i++ )
        {
            uint8_t payload[16];
            memset( payload, (int)(0xA0 + i), sizeof(payload) );
            lsmash_sample_t *s = lsmash_create_sample( sizeof(payload) );
            if( !s )
                goto fail_out;
            memcpy( s->data, payload, sizeof(payload) );
            s->dts = i * 300;
            s->cts = s->dts;
            s->index = 1;
            s->prop.ra_flags = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
            if( lsmash_append_sample( root, audio_ID, s ) < 0 )
            {
                lsmash_delete_sample( s );
                goto fail_out;
            }
        }
        if( lsmash_flush_pooled_samples( root, audio_ID, 300 ) )
            goto fail_out;
        if( lsmash_create_explicit_timeline_map( root, audio_ID, edit ) )
            goto fail_out;
    }
    if( lsmash_finish_movie( root, NULL ) )
        goto fail_out;
    rc = 0;
fail_out:
    lsmash_destroy_root( root );
    lsmash_close_file( &fp );
fail_src:
    close_snapshot( &src );
    return rc;
}

/* ---- test cases ---- */

void test_reorder( void )
{
    CASE();
    char out[256];
    snprintf( out, sizeof(out), "%s/reorder.mp4", tmp_dir() );
    char args[512];
    snprintf( args, sizeof(args), "-i %s --cuts task_support/cuts.txt -o %s", SOURCE_MP4, out );
    CHECK( run_remuxer( args ) == 0 );

    movie_snapshot_t src, dst;
    CHECK( open_snapshot( SOURCE_MP4, &src ) == 0 );
    CHECK( open_snapshot( out, &dst ) == 0 );
    CHECK( dst.movie_timescale == 1000 );
    CHECK( dst.movie_duration == 3000 );
    CHECK( dst.num_tracks == 2 );
    /* Source order for cuts [1000,2000) [0,1000) [1000,2000): video 7..12,1..6,7..12. */
    static const uint32_t video_map[18] = { 7,8,9,10,11,12, 1,2,3,4,5,6, 7,8,9,10,11,12 };
    track_snapshot_t *sv = &src.tracks[0];
    track_snapshot_t *dv = &dst.tracks[0];
    CHECK( dv->count == 18 );
    CHECK( dv->timescale == sv->timescale );
    CHECK( dv->width == 64 && dv->height == 64 );
    CHECK( dv->language == sv->language );
    for( uint32_t i = 0; i < 18; i++ )
    {
        sample_snapshot_t *a = &dv->samples[i];
        sample_snapshot_t *b = &sv->samples[ video_map[i] - 1 ];
        CHECK( same_bytes( a, b ) );
        /* cts-dts difference must survive the rebase. */
        CHECK( a->cts - a->dts == b->cts - b->dts );
        CHECK( a->delta == b->delta );
    }
    /* Output DTS runs from 0 by actual sample durations. */
    CHECK( dv->samples[0].dts == 0 );
    for( uint32_t i = 1; i < 18; i++ )
        CHECK( dv->samples[i].dts == dv->samples[i-1].dts + dv->samples[i-1].delta );
    /* Segment starts are sync points in the output as well. */
    CHECK( (dv->samples[0].ra  & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC) != 0 );
    CHECK( (dv->samples[6].ra  & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC) != 0 );
    CHECK( (dv->samples[12].ra & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC) != 0 );
    /* Repeated shot: really duplicated bytes, distinct media locations. */
    for( uint32_t i = 0; i < 6; i++ )
    {
        CHECK( same_bytes( &dv->samples[i], &dv->samples[12 + i] ) );
        CHECK( dv->samples[i].pos != dv->samples[12 + i].pos );
    }
    /* Three edits per track, one per cut, each 1000 movie ticks. */
    for( uint32_t t = 0; t < 2; t++ )
    {
        track_snapshot_t *tr = &dst.tracks[t];
        CHECK( tr->num_edits == 3 );
        for( uint32_t e = 0; e < 3; e++ )
        {
            CHECK( tr->edits[e].duration == 1000 );
            CHECK( tr->edits[e].rate == ISOM_EDIT_MODE_NORMAL );
            CHECK( tr->edits[e].start_time >= 0 );
        }
    }
    CHECK( dst.tracks[0].edits[0].start_time == 4096 );
    CHECK( dst.tracks[0].edits[1].start_time == 16384 );
    CHECK( dst.tracks[0].edits[2].start_time == 28672 );
    /* Audio edits hide AAC packet overhang: cut 1/3 start inside a packet. */
    CHECK( dst.tracks[1].edits[0].start_time == 896 );
    CHECK( dst.tracks[1].edits[1].start_time == 49152 );
    CHECK( dst.tracks[1].edits[2].start_time == 98176 );
    /* No leading empty edit: the first edit starts presentation at 0. */
    CHECK( dst.tracks[0].edits[0].start_time != ISOM_EDIT_MODE_EMPTY );

    /* Audio: independent intersection rule against packet boundaries.
     * Source edit starts media at 1024, so media windows include that offset. */
    track_snapshot_t *sa = &src.tracks[1];
    track_snapshot_t *da = &dst.tracks[1];
    CHECK( da->count == 143 );
    CHECK( da->timescale == sa->timescale );
    uint32_t out_index = 0;
    static const uint64_t cut_media[3][2] = { {49024,97024}, {1024,49024}, {49024,97024} };
    for( uint32_t c = 0; c < 3; c++ )
    {
        uint32_t first = 0, last = 0;
        for( uint32_t s = 1; s <= sa->count; s++ )
        {
            uint64_t st = sa->samples[s-1].cts;
            uint64_t en = st + sa->samples[s-1].delta;
            if( st < cut_media[c][1] && en > cut_media[c][0] )
            {
                if( !first )
                    first = s;
                last = s;
            }
        }
        CHECK( first != 0 );
        for( uint32_t s = first; s <= last; s++ )
        {
            CHECK( same_bytes( &da->samples[out_index], &sa->samples[s-1] ) );
            CHECK( da->samples[out_index].delta == sa->samples[s-1].delta );
            ++out_index;
        }
    }
    CHECK( out_index == 143 );
    /* Cuts 1 and 3 select the identical source range (48 packets each), so
     * their copied byte sequences match, while every copy has its own location. */
    for( uint32_t i = 0; i < 48; i++ )
    {
        CHECK( same_bytes( &da->samples[i], &da->samples[95 + i] ) );
        CHECK( da->samples[i].pos != da->samples[95 + i].pos );
    }
    close_snapshot( &src );
    close_snapshot( &dst );
    unlink( out );
}

void test_audio_edges( void )
{
    CASE();
    char src[256], cuts[256], out[256];
    snprintf( src,  sizeof(src),  "%s/edges.mp4",   tmp_dir() );
    snprintf( cuts, sizeof(cuts), "%s/edges.txt",   tmp_dir() );
    snprintf( out,  sizeof(out),  "%s/edges_out.mp4", tmp_dir() );
    CHECK( build_synth_mp4( src, 1, 0 ) == 0 );
    /* Boundaries 500 and 2500 are video IDR times yet land inside AAC packets
     * (period 300): 500 in [300,600), 2500 in [2400,2700). */
    write_text_file( cuts, "0 500\n500 2500\n2500 3000\n" );
    char args[768];
    snprintf( args, sizeof(args), "-i %s --cuts %s -o %s", src, cuts, out );
    CHECK( run_remuxer( args ) == 0 );

    movie_snapshot_t m;
    CHECK( open_snapshot( out, &m ) == 0 );
    CHECK( m.movie_duration == 3000 );
    CHECK( m.num_tracks == 2 );
    track_snapshot_t *v = &m.tracks[0];
    track_snapshot_t *a = &m.tracks[1];
    CHECK( v->count == 6 );
    /* cut1 [0,500): packets 0,1; cut2 [500,2500): packets 1..8; cut3 [2500,3000): packets 8,9. */
    CHECK( a->count == 2 + 8 + 2 );
    static const uint8_t expect_fill[12] =
        { 0xA0,0xA1, 0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8, 0xA8,0xA9 };
    for( uint32_t i = 0; i < 12; i++ )
    {
        CHECK( a->samples[i].length == 16 );
        CHECK( a->samples[i].data[0] == expect_fill[i] );
    }
    /* The packet straddling 500 is physically copied twice, adjacent. */
    CHECK( a->samples[1].data[0] == 0xA1 );
    CHECK( a->samples[2].data[0] == 0xA1 );
    CHECK( a->samples[1].pos != a->samples[2].pos );
    /* Same for the packet straddling 2500. */
    CHECK( a->samples[9].data[0] == 0xA8 );
    CHECK( a->samples[10].data[0] == 0xA8 );
    CHECK( a->samples[9].pos != a->samples[10].pos );
    /* Edits show exact 500/2000/500 movie-tick windows and no empty edit. */
    CHECK( a->num_edits == 3 );
    CHECK( a->edits[0].duration == 500 );
    CHECK( a->edits[1].duration == 2000 );
    CHECK( a->edits[2].duration == 500 );
    /* Edit start hides the overhanging packet head:
     * seg1 first packet cts 0, start 0 -> 0;
     * seg2 first packet cts 300, cut start 500, output cts 600 -> 600+200 = 800;
     * seg3 first packet cts 2400, cut start 2500, output cts 3000 -> 3100. */
    CHECK( a->edits[0].start_time == 0 );
    CHECK( a->edits[1].start_time == 800 );
    CHECK( a->edits[2].start_time == 3100 );
    CHECK( a->edits[0].start_time != ISOM_EDIT_MODE_EMPTY );
    /* Output DTS contiguous from 0 over real packet durations. */
    CHECK( a->samples[0].dts == 0 );
    for( uint32_t i = 1; i < 12; i++ )
        CHECK( a->samples[i].dts == a->samples[i-1].dts + 300 );
    close_snapshot( &m );
    unlink( src );
    unlink( cuts );
    unlink( out );
}

void test_mapping( void )
{
    CASE();
    /* Reversed ordering plus a single-video output. */
    char src[256], cuts[256], out[256], out_v[256];
    snprintf( src,   sizeof(src),   "%s/map.mp4",     tmp_dir() );
    snprintf( cuts,  sizeof(cuts),  "%s/map.txt",     tmp_dir() );
    snprintf( out,   sizeof(out),   "%s/map_out.mp4", tmp_dir() );
    snprintf( out_v, sizeof(out_v), "%s/map_v.mp4",   tmp_dir() );
    CHECK( build_synth_mp4( src, 1, 0 ) == 0 );
    write_text_file( cuts, "2000 3000\n0 1000\n1000 2000\n" );
    char args[768];
    snprintf( args, sizeof(args), "-i %s --cuts %s -o %s", src, cuts, out );
    CHECK( run_remuxer( args ) == 0 );
    movie_snapshot_t m;
    CHECK( open_snapshot( out, &m ) == 0 );
    track_snapshot_t *v = &m.tracks[0];
    CHECK( v->count == 6 );
    /* Display order is cut order: source samples 5,6,1,2,3,4. */
    static const uint8_t expect_fill[6] = { 0x50,0x60,0x10,0x20,0x30,0x40 };
    for( uint32_t i = 0; i < 6; i++ )
        CHECK( v->samples[i].data[8] == expect_fill[i] );
    CHECK( v->samples[0].dts == 0 );
    for( uint32_t i = 1; i < 6; i++ )
        CHECK( v->samples[i].dts == i * 500 );
    CHECK( v->num_edits == 3 );
    /* Edit start times are output media times: rebased DTS 0/1000/2000. */
    CHECK( v->edits[0].duration == 1000 && v->edits[0].start_time == 0 );
    CHECK( v->edits[1].duration == 1000 && v->edits[1].start_time == 1000 );
    CHECK( v->edits[2].duration == 1000 && v->edits[2].start_time == 2000 );
    CHECK( m.movie_duration == 3000 );
    close_snapshot( &m );

    /* Single video track: audio omitted by a dedicated fixture. */
    CHECK( build_synth_mp4( src, 0, 0 ) == 0 );
    snprintf( args, sizeof(args), "-i %s --cuts %s -o %s", src, cuts, out_v );
    CHECK( run_remuxer( args ) == 0 );
    CHECK( open_snapshot( out_v, &m ) == 0 );
    CHECK( m.num_tracks == 1 );
    CHECK( m.tracks[0].count == 6 );
    CHECK( m.tracks[0].num_edits == 3 );
    CHECK( m.movie_duration == 3000 );
    close_snapshot( &m );

    /* B-frame cts-dts preservation on the real B-frame source, reversed cuts. */
    char cuts2[256], out2[256];
    snprintf( cuts2, sizeof(cuts2), "%s/map2.txt", tmp_dir() );
    snprintf( out2,  sizeof(out2),  "%s/map2.mp4",  tmp_dir() );
    write_text_file( cuts2, "2000 3000\n0 1000\n" );
    snprintf( args, sizeof(args), "-i %s --cuts %s -o %s", SOURCE_MP4, cuts2, out2 );
    CHECK( run_remuxer( args ) == 0 );
    movie_snapshot_t s, d;
    CHECK( open_snapshot( SOURCE_MP4, &s ) == 0 );
    CHECK( open_snapshot( out2, &d ) == 0 );
    static const uint32_t rev_map[12] = { 13,14,15,16,17,18, 1,2,3,4,5,6 };
    track_snapshot_t *sv = &s.tracks[0], *dv = &d.tracks[0];
    CHECK( dv->count == 12 );
    for( uint32_t i = 0; i < 12; i++ )
    {
        CHECK( same_bytes( &dv->samples[i], &sv->samples[ rev_map[i] - 1 ] ) );
        CHECK( dv->samples[i].cts - dv->samples[i].dts
            == sv->samples[ rev_map[i] - 1 ].cts - sv->samples[ rev_map[i] - 1 ].dts );
    }
    CHECK( dv->samples[0].dts == 0 );
    close_snapshot( &s );
    close_snapshot( &d );
    unlink( src );
    unlink( cuts );
    unlink( out );
    unlink( out_v );
    unlink( cuts2 );
    unlink( out2 );
}

void test_reject( void )
{
    CASE();
    char cuts[256], out[256];
    snprintf( cuts, sizeof(cuts), "%s/reject.txt", tmp_dir() );
    snprintf( out,  sizeof(out),  "%s/reject.mp4", tmp_dir() );
    char args[768];

    struct { const char *content; } bad_lines[] =
    {
        { "1000\n" },
        { "1000 x\n" },
        { "1000 2000 extra\n" },
        { "-1 2\n" },
        { "2000 1000\n" },
        { "\n  \n" },
        { "1000.5 2000\n" }
    };
    for( uint32_t i = 0; i < sizeof(bad_lines)/sizeof(bad_lines[0]); i++ )
    {
        write_text_file( cuts, bad_lines[i].content );
        unlink( out );
        snprintf( args, sizeof(args), "-i %s --cuts %s -o %s", SOURCE_MP4, cuts, out );
        CHECK( run_remuxer( args ) != 0 );
        CHECK( access( out, F_OK ) != 0 );
    }

    /* Out of range and non-IDR / non-integer boundaries. */
    const char *bad_cuts[] =
    {
        "0 4000\n",
        "500 1000\n",
        "1 1000\n",
        "0 500\n"
    };
    for( uint32_t i = 0; i < sizeof(bad_cuts)/sizeof(bad_cuts[0]); i++ )
    {
        write_text_file( cuts, bad_cuts[i] );
        unlink( out );
        snprintf( args, sizeof(args), "-i %s --cuts %s -o %s", SOURCE_MP4, cuts, out );
        CHECK( run_remuxer( args ) != 0 );
        CHECK( access( out, F_OK ) != 0 );
    }

    /* Option conflicts. */
    unlink( out );
    snprintf( args, sizeof(args), "-i %s --cuts task_support/cuts.txt --fragment 1 -o %s", SOURCE_MP4, out );
    CHECK( run_remuxer( args ) != 0 );
    CHECK( access( out, F_OK ) != 0 );

    unlink( out );
    snprintf( args, sizeof(args), "-i %s -i %s --cuts task_support/cuts.txt -o %s", SOURCE_MP4, SOURCE_MP4, out );
    CHECK( run_remuxer( args ) != 0 );
    CHECK( access( out, F_OK ) != 0 );

    unlink( out );
    snprintf( args, sizeof(args), "-i %s?1:remove --cuts task_support/cuts.txt -o %s", SOURCE_MP4, out );
    CHECK( run_remuxer( args ) != 0 );
    CHECK( access( out, F_OK ) != 0 );

    unlink( out );
    snprintf( args, sizeof(args), "-i %s?1:seek=1 --cuts task_support/cuts.txt -o %s", SOURCE_MP4, out );
    CHECK( run_remuxer( args ) != 0 );
    CHECK( access( out, F_OK ) != 0 );

    /* A non-IDR intra sample must not be accepted as an IDR boundary. */
    char src[256];
    snprintf( src, sizeof(src), "%s/nonidr.mp4", tmp_dir() );
    CHECK( build_synth_mp4( src, 0, 1 ) == 0 );
    const char *non_idr_cuts[] = { "500 1000\n", "0 500\n", "500 2500\n" };
    for( uint32_t i = 0; i < sizeof(non_idr_cuts)/sizeof(non_idr_cuts[0]); i++ )
    {
        write_text_file( cuts, non_idr_cuts[i] );
        unlink( out );
        snprintf( args, sizeof(args), "-i %s --cuts %s -o %s", src, cuts, out );
        CHECK( run_remuxer( args ) != 0 );
        CHECK( access( out, F_OK ) != 0 );
    }

    /* Baseline behavior without --cuts is untouched: whole movie remuxed. */
    unlink( out );
    snprintf( args, sizeof(args), "-i %s -o %s", SOURCE_MP4, out );
    CHECK( run_remuxer( args ) == 0 );
    movie_snapshot_t m;
    CHECK( open_snapshot( out, &m ) == 0 );
    CHECK( m.num_tracks == 2 );
    CHECK( m.tracks[0].count == 18 );
    CHECK( m.tracks[1].count == 142 );
    CHECK( m.tracks[0].num_edits == 1 );
    CHECK( m.tracks[1].num_edits == 1 );
    close_snapshot( &m );
    unlink( src );
    unlink( cuts );
    unlink( out );
}
