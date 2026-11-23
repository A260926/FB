/* examples/pool_undo.c
 * Demonstrates per-track pooled-sample undo: a few trailing video samples
 * are handed to the muxer, revoked before they are written out, and replaced
 * by a corrected batch from the same decode position. The resulting file is
 * identical to one where the revoked samples were never appended. */

#include "lsmash.h"
#include <stdio.h>
#include <string.h>

static lsmash_sample_t *make_sample( uint64_t dts, uint64_t cts, uint8_t mark )
{
    lsmash_sample_t *s = lsmash_create_sample( 8 );
    if( !s )
        return NULL;
    /* a recognizable fake AVC access unit: 4-byte length + NAL + marker */
    s->data[0] = 0;
    s->data[1] = 0;
    s->data[2] = 0;
    s->data[3] = 4;
    s->data[4] = 0x65;
    s->data[5] = 0x88;
    s->data[6] = 0x84;
    s->data[7] = mark;
    s->dts   = dts;
    s->cts   = cts;
    s->index = 1;
    s->prop.ra_flags = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
    return s;
}

static int append_marked( lsmash_root_t *root, uint32_t track, uint64_t dts, uint64_t cts, uint8_t mark )
{
    lsmash_sample_t *s = make_sample( dts, cts, mark );
    if( !s )
        return -1;
    int err = lsmash_append_sample( root, track, s );
    if( err < 0 )
        lsmash_delete_sample( s );
    return err;
}

int main( void )
{
    const char *name = ".task-build/pool-undo-example.mp4";
    lsmash_root_t *root = lsmash_create_root();
    if( !root )
        return 1;
    lsmash_file_parameters_t file = { 0 };
    if( lsmash_open_file( name, 0, &file ) < 0 )
        return 1;
    static lsmash_brand_type brands[] = { ISOM_BRAND_TYPE_ISOM, ISOM_BRAND_TYPE_ISO6, ISOM_BRAND_TYPE_AVC1 };
    file.major_brand = ISOM_BRAND_TYPE_ISOM;
    file.brands      = brands;
    file.brand_count = 3;
    if( !lsmash_set_file( root, &file ) )
        return 1;
    lsmash_movie_parameters_t mp;
    lsmash_initialize_movie_parameters( &mp );
    mp.timescale = 1000;
    if( lsmash_set_movie_parameters( root, &mp ) < 0 )
        return 1;
    uint32_t track = lsmash_create_track( root, ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK );
    lsmash_track_parameters_t tp;
    lsmash_initialize_track_parameters( &tp );
    tp.display_width  = 16 << 16;
    tp.display_height = 16 << 16;
    if( !track || lsmash_set_track_parameters( root, track, &tp ) < 0 )
        return 1;
    lsmash_media_parameters_t media;
    lsmash_initialize_media_parameters( &media );
    media.timescale = 1000;
    if( lsmash_set_media_parameters( root, track, &media ) < 0 )
        return 1;
    lsmash_video_summary_t *v = (lsmash_video_summary_t *)lsmash_create_summary( LSMASH_SUMMARY_TYPE_VIDEO );
    if( !v )
        return 1;
    v->sample_type = ISOM_CODEC_TYPE_AVC1_VIDEO;
    v->width       = 16;
    v->height      = 16;
    lsmash_codec_specific_t *c = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_VIDEO_H264,
                                                                    LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
    if( !c )
        return 1;
    lsmash_h264_specific_parameters_t *avc = c->data.structured;
    avc->lengthSizeMinusOne = 3;
    const uint8_t sps[] = { 0x67, 0x42, 0xc0, 0x0a, 0xda, 0x7b, 0x01, 0x10, 0, 0, 3, 0, 0x10, 0, 0, 3, 3, 0xc0, 0xf1, 0x22, 0x6a };
    const uint8_t pps[] = { 0x68, 0xce, 0x0f, 0xc8 };
    if( lsmash_append_h264_parameter_set( avc, H264_PARAMETER_SET_TYPE_SPS, (void *)sps, sizeof(sps) ) < 0
     || lsmash_append_h264_parameter_set( avc, H264_PARAMETER_SET_TYPE_PPS, (void *)pps, sizeof(pps) ) < 0 )
        return 1;
    if( lsmash_add_codec_specific_data( (lsmash_summary_t *)v, c ) < 0 )
        return 1;
    lsmash_destroy_codec_specific_data( c );
    if( lsmash_add_sample_entry( root, track, (lsmash_summary_t *)v ) != 1 )
        return 1;
    lsmash_cleanup_summary( (lsmash_summary_t *)v );
    /* enable undo before the first sample */
    if( lsmash_set_pool_undo( root, track, 1 ) < 0 )
        return 1;
    /* two good samples; commit them so they can no longer be revoked */
    if( append_marked( root, track,   0,   0, 0x11 ) < 0
     || append_marked( root, track,  40,  40, 0x12 ) < 0
     || lsmash_flush_pooled_samples( root, track, 40 ) < 0 )
        return 1;
    /* a tail batch we decide to take back */
    if( append_marked( root, track,  80,  80, 0x51 ) < 0
     || append_marked( root, track, 120, 120, 0x52 ) < 0
     || append_marked( root, track, 160, 160, 0x53 ) < 0 )
        return 1;
    uint32_t discarded = 0;
    if( lsmash_discard_pooled_samples( root, track, &discarded ) < 0 )
        return 1;
    printf( "POOL_UNDO_EXAMPLE discarded=%u\n", discarded );
    if( discarded != 3 )
        return 1;
    /* replacement tail from the same decode position */
    if( append_marked( root, track,  80,  80, 0x61 ) < 0
     || append_marked( root, track, 120, 120, 0x62 ) < 0 )
        return 1;
    if( lsmash_flush_pooled_samples( root, track, 40 ) < 0
     || lsmash_finish_movie( root, NULL ) < 0 )
        return 1;
    if( lsmash_close_file( &file ) < 0 )
        return 1;
    lsmash_destroy_root( root );
    printf( "POOL_UNDO_EXAMPLE wrote %s samples=4 marks=11,12,61,62\n", name );
    return 0;
}
