/* tests/pool_undo.c
 * Task tests for per-track pooled-sample undo (lsmash_set_pool_undo /
 * lsmash_discard_pooled_samples). Registered through task_cases; the driver
 * provides main(). */

#include "task_support/check.h"

#include "common/internal.h"
#include "core/box.h"
#include "core/pool_undo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* counted I/O                                                               */
/* ------------------------------------------------------------------------ */

typedef struct
{
    FILE          *fp;
    unsigned long  write_calls;
    unsigned long  seek_calls;
    unsigned long  read_calls;
} io_count_t;

static int io_count_read( void *opaque, uint8_t *buf, int size )
{
    io_count_t *io = (io_count_t *)opaque;
    ++io->read_calls;
    int ret = fread( buf, 1, size, io->fp );
    return ferror( io->fp ) ? LSMASH_ERR_NAMELESS : ret;
}

static int io_count_write( void *opaque, uint8_t *buf, int size )
{
    io_count_t *io = (io_count_t *)opaque;
    ++io->write_calls;
    return fwrite( buf, 1, size, io->fp );
}

static int64_t io_count_seek( void *opaque, int64_t offset, int whence )
{
    io_count_t *io = (io_count_t *)opaque;
    ++io->seek_calls;
    if( lsmash_fseek( io->fp, offset, whence ) != 0 )
        return LSMASH_ERR_NAMELESS;
    return lsmash_ftell( io->fp );
}

/* ------------------------------------------------------------------------ */
/* movie builder                                                             */
/* ------------------------------------------------------------------------ */

typedef struct
{
    lsmash_root_t            *root;
    lsmash_file_parameters_t  fp;
    io_count_t                io;
    uint32_t                  vtrack;
    uint32_t                  atrack;
} movie_t;

static int movie_open( movie_t *m, const char *name,
                       double max_chunk_duration, double max_async_tolerance, uint64_t max_chunk_size,
                       int with_audio, int rap_grouping, int roll_grouping )
{
    memset( m, 0, sizeof(*m) );
    m->io.fp = fopen( name, "w+b" );
    if( !m->io.fp )
        return -1;
    m->root = lsmash_create_root();
    if( !m->root )
        return -1;
    static lsmash_brand_type brands[] = { ISOM_BRAND_TYPE_ISOM, ISOM_BRAND_TYPE_ISO6, ISOM_BRAND_TYPE_AVC1 };
    m->fp.mode                 = LSMASH_FILE_MODE_WRITE | LSMASH_FILE_MODE_BOX
                               | LSMASH_FILE_MODE_INITIALIZATION | LSMASH_FILE_MODE_MEDIA;
    m->fp.opaque               = &m->io;
    m->fp.read                 = io_count_read;
    m->fp.write                = io_count_write;
    m->fp.seek                 = io_count_seek;
    m->fp.major_brand          = ISOM_BRAND_TYPE_ISOM;
    m->fp.brands               = brands;
    m->fp.brand_count          = 3;
    m->fp.max_chunk_duration   = max_chunk_duration;
    m->fp.max_async_tolerance  = max_async_tolerance;
    m->fp.max_chunk_size       = max_chunk_size;
    m->fp.max_read_size        = 4 * 1024 * 1024;
    if( !lsmash_set_file( m->root, &m->fp ) )
        return -1;
    lsmash_movie_parameters_t mp;
    lsmash_initialize_movie_parameters( &mp );
    mp.timescale = 1000;
    if( lsmash_set_movie_parameters( m->root, &mp ) < 0 )
        return -1;
    /* video track with two compatible avc1 descriptions */
    m->vtrack = lsmash_create_track( m->root, ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK );
    if( !m->vtrack )
        return -1;
    lsmash_track_parameters_t tp;
    lsmash_initialize_track_parameters( &tp );
    tp.display_width  = 16 << 16;
    tp.display_height = 16 << 16;
    if( lsmash_set_track_parameters( m->root, m->vtrack, &tp ) < 0 )
        return -1;
    lsmash_media_parameters_t media;
    lsmash_initialize_media_parameters( &media );
    media.timescale     = 1000;
    media.rap_grouping  = rap_grouping;
    media.roll_grouping = roll_grouping;
    if( lsmash_set_media_parameters( m->root, m->vtrack, &media ) < 0 )
        return -1;
    for( int i = 0; i < 2; i++ )
    {
        lsmash_video_summary_t *v = (lsmash_video_summary_t *)lsmash_create_summary( LSMASH_SUMMARY_TYPE_VIDEO );
        if( !v )
            return -1;
        v->sample_type = ISOM_CODEC_TYPE_AVC1_VIDEO;
        v->width       = 16 + 16 * i;
        v->height      = 16 + 16 * i;
        lsmash_codec_specific_t *c = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_VIDEO_H264,
                                                                        LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
        if( !c )
            return -1;
        lsmash_h264_specific_parameters_t *avc = c->data.structured;
        avc->lengthSizeMinusOne = 3;
        uint8_t sps[] = { 0x67, 0x42, 0xc0, 0x0a, 0xda, 0x7b, 0x01, 0x10, 0, 0, 3, 0, 0x10, 0, 0, 3, 3, 0xc0, 0xf1, 0x22, 0x6a };
        uint8_t pps[] = { 0x68, 0xce, 0x0f, 0xc8 };
        sps[20] += i;   /* make the two descriptions distinguishable */
        pps[3]  += i;
        if( lsmash_append_h264_parameter_set( avc, H264_PARAMETER_SET_TYPE_SPS, sps, sizeof(sps) ) < 0
         || lsmash_append_h264_parameter_set( avc, H264_PARAMETER_SET_TYPE_PPS, pps, sizeof(pps) ) < 0 )
            return -1;
        if( lsmash_add_codec_specific_data( (lsmash_summary_t *)v, c ) < 0 )
            return -1;
        lsmash_destroy_codec_specific_data( c );
        int index = lsmash_add_sample_entry( m->root, m->vtrack, (lsmash_summary_t *)v );
        lsmash_cleanup_summary( (lsmash_summary_t *)v );
        if( index != i + 1 )
            return -1;
    }
    if( with_audio )
    {
        m->atrack = lsmash_create_track( m->root, ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK );
        if( !m->atrack )
            return -1;
        lsmash_initialize_track_parameters( &tp );
        if( lsmash_set_track_parameters( m->root, m->atrack, &tp ) < 0 )
            return -1;
        lsmash_initialize_media_parameters( &media );
        media.timescale = 48000;
        if( lsmash_set_media_parameters( m->root, m->atrack, &media ) < 0 )
            return -1;
        lsmash_audio_summary_t *a = (lsmash_audio_summary_t *)lsmash_create_summary( LSMASH_SUMMARY_TYPE_AUDIO );
        if( !a )
            return -1;
        a->sample_type      = ISOM_CODEC_TYPE_MP4A_AUDIO;
        a->aot              = MP4A_AUDIO_OBJECT_TYPE_AAC_LC;
        a->frequency        = 48000;
        a->channels         = 2;
        a->sample_size      = 16;
        a->samples_in_frame = 1024;
        if( lsmash_setup_AudioSpecificConfig( (lsmash_audio_summary_t *)a ) < 0 )
            return -1;
        int index = lsmash_add_sample_entry( m->root, m->atrack, (lsmash_summary_t *)a );
        lsmash_cleanup_summary( (lsmash_summary_t *)a );
        if( index != 1 )
            return -1;
    }
    return 0;
}

static int movie_close( movie_t *m, uint32_t vdelta, uint32_t adelta )
{
    int err = 0;
    if( m->vtrack && lsmash_flush_pooled_samples( m->root, m->vtrack, vdelta ) < 0 )
        err = -1;
    if( !err && m->atrack && lsmash_flush_pooled_samples( m->root, m->atrack, adelta ) < 0 )
        err = -1;
    if( !err && lsmash_finish_movie( m->root, NULL ) < 0 )
        err = -1;
    lsmash_destroy_root( m->root );
    fclose( m->io.fp );
    m->root = NULL;
    return err;
}

/* ------------------------------------------------------------------------ */
/* sample factory: recognizable compressed payloads                          */
/* ------------------------------------------------------------------------ */

static lsmash_sample_t *make_sample( uint32_t size, uint8_t tag, uint64_t dts, uint64_t cts, uint32_t index )
{
    lsmash_sample_t *s = lsmash_create_sample( size );
    if( !s )
        return NULL;
    uint32_t n = size - 4;
    s->data[0] = (uint8_t)(n >> 24);
    s->data[1] = (uint8_t)(n >> 16);
    s->data[2] = (uint8_t)(n >>  8);
    s->data[3] = (uint8_t)(n      );
    s->data[4] = tag;   /* fake NAL header carrying the sample tag */
    for( uint32_t i = 5; i < size; i++ )
        s->data[i] = (uint8_t)(tag * 7 + (uint8_t)dts + 3 * i + 1);
    s->dts   = dts;
    s->cts   = cts;
    s->index = index;
    return s;
}

static int append_tag( movie_t *m, uint32_t track, uint32_t size, uint8_t tag,
                       uint64_t dts, uint64_t cts, uint32_t index, lsmash_random_access_flag ra )
{
    lsmash_sample_t *s = make_sample( size, tag, dts, cts, index );
    if( !s )
        return -1;
    s->prop.ra_flags = ra;
    int err = lsmash_append_sample( m->root, track, s );
    if( err < 0 )
        lsmash_delete_sample( s );
    return err;
}

/* video sample with full property control */
static int append_prop( movie_t *m, uint32_t track, uint32_t size, uint8_t tag,
                        uint64_t dts, uint64_t cts, uint32_t index, lsmash_sample_property_t *prop )
{
    lsmash_sample_t *s = make_sample( size, tag, dts, cts, index );
    if( !s )
        return -1;
    s->prop = *prop;
    int err = lsmash_append_sample( m->root, track, s );
    if( err < 0 )
        lsmash_delete_sample( s );
    return err;
}

static isom_trak_t *movie_trak( movie_t *m, uint32_t track )
{
    return isom_get_trak( m->root->file, track );
}

static uint32_t pooled_count( movie_t *m, uint32_t track )
{
    isom_trak_t *trak = movie_trak( m, track );
    return trak->cache->chunk.pool ? trak->cache->chunk.pool->sample_count : 0;
}

static uint32_t table_sample_count( movie_t *m, uint32_t track )
{
    isom_trak_t *trak = movie_trak( m, track );
    isom_stbl_t *stbl = trak->mdia->minf->stbl;
    return stbl->stsz ? stbl->stsz->sample_count : 0;
}

/* ------------------------------------------------------------------------ */
/* independent box parser                                                    */
/* ------------------------------------------------------------------------ */

#define MAX_ENTRIES 256
#define MAX_SAMPLES 256
#define MAX_TRACKS  4

typedef struct { uint32_t count, value; } run_pair_t;
typedef struct { uint32_t first, spc, sdi; } stsc_row_t;
typedef struct { uint32_t count, gdi; }     sbgp_row_t;

typedef struct
{
    uint32_t   track_id;
    uint32_t   handler;
    uint32_t   stsd_count;
    uint32_t   stsd_type[8];
    uint32_t   stts_n;   run_pair_t stts[MAX_ENTRIES];
    uint32_t   ctts_present, ctts_version;
    uint32_t   ctts_n;   run_pair_t ctts[MAX_ENTRIES];
    uint32_t   stss_present;
    uint32_t   stss_n;   uint32_t  stss[MAX_ENTRIES];
    uint32_t   sdtp_present;
    uint32_t   sdtp_n;   uint8_t   sdtp[MAX_SAMPLES];
    uint32_t   stsc_n;   stsc_row_t stsc[MAX_ENTRIES];
    uint32_t   stco_n;   uint64_t  stco[MAX_ENTRIES];
    uint32_t   stsz_const;
    uint32_t   stsz_n;   uint32_t  stsz[MAX_SAMPLES];
    uint32_t   sbgp_rap_n;  sbgp_row_t sbgp_rap[MAX_ENTRIES];
    uint32_t   sgpd_rap_n;  uint8_t    sgpd_rap[MAX_ENTRIES];
    uint32_t   sbgp_roll_n; sbgp_row_t sbgp_roll[MAX_ENTRIES];
    uint32_t   sgpd_roll_n; int16_t    sgpd_roll[MAX_ENTRIES];
    uint32_t   nsamples;
    uint64_t   dts[MAX_SAMPLES];
    int64_t    cts[MAX_SAMPLES];
    uint32_t   sdi[MAX_SAMPLES];
    uint64_t   offset[MAX_SAMPLES];
} parsed_track_t;

typedef struct
{
    uint8_t        *data;
    uint64_t        size;
    uint32_t        ntracks;
    parsed_track_t  tracks[MAX_TRACKS];
} parsed_file_t;

#define FCC(a,b,c,d) ((uint32_t)(uint8_t)(a)<<24 | (uint32_t)(uint8_t)(b)<<16 | (uint32_t)(uint8_t)(c)<<8 | (uint32_t)(uint8_t)(d))

static uint32_t be32( const uint8_t *p )
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t be64( const uint8_t *p )
{
    return ((uint64_t)be32( p ) << 32) | be32( p + 4 );
}

typedef struct
{
    uint64_t start;
    uint32_t type;
    uint64_t head;
    uint64_t size;
} box_t;

static int next_box( const uint8_t *buf, uint64_t *pos, uint64_t end, box_t *box )
{
    if( *pos + 8 > end )
        return 0;
    uint64_t size = be32( buf + *pos );
    uint64_t head = 8;
    if( size == 1 )
    {
        if( *pos + 16 > end )
            return -1;
        size = be64( buf + *pos + 8 );
        head = 16;
    }
    else if( size == 0 )
        size = end - *pos;
    if( size < head || *pos + size > end )
        return -1;
    box->start = *pos;
    box->type  = be32( buf + *pos + 4 );
    box->head  = head;
    box->size  = size;
    *pos += size;
    return 1;
}

static int find_box( const uint8_t *buf, uint64_t start, uint64_t end, uint32_t type, box_t *out )
{
    uint64_t pos = start;
    box_t    box;
    int      ret;
    while( (ret = next_box( buf, &pos, end, &box )) > 0 )
        if( box.type == type )
        {
            *out = box;
            return 1;
        }
    return ret < 0 ? -1 : 0;
}

static int parse_stbl( parsed_track_t *t, const uint8_t *buf, uint64_t start, uint64_t end )
{
    uint64_t pos = start;
    box_t    box;
    int      ret;
    while( (ret = next_box( buf, &pos, end, &box )) > 0 )
    {
        const uint8_t *p = buf + box.start + box.head;
        switch( box.type )
        {
            case FCC('s','t','s','d') :
                t->stsd_count = be32( p + 4 );
                if( t->stsd_count > 8 )
                    return -1;
                for( uint32_t i = 0; i < t->stsd_count; i++ )
                {
                    uint64_t epos = box.start + box.head + 8;
                    for( uint32_t j = 0; j < i; j++ )
                        epos += be32( buf + epos );
                    t->stsd_type[i] = be32( buf + epos + 4 );
                }
                break;
            case FCC('s','t','t','s') :
                t->stts_n = be32( p + 4 );
                if( t->stts_n > MAX_ENTRIES )
                    return -1;
                for( uint32_t i = 0; i < t->stts_n; i++ )
                {
                    t->stts[i].count = be32( p + 8 + 8 * i );
                    t->stts[i].value = be32( p + 12 + 8 * i );
                }
                break;
            case FCC('c','t','t','s') :
                t->ctts_present = 1;
                t->ctts_version = p[0];
                t->ctts_n       = be32( p + 4 );
                if( t->ctts_n > MAX_ENTRIES )
                    return -1;
                for( uint32_t i = 0; i < t->ctts_n; i++ )
                {
                    t->ctts[i].count = be32( p + 8 + 8 * i );
                    t->ctts[i].value = be32( p + 12 + 8 * i );
                }
                break;
            case FCC('s','t','s','s') :
                t->stss_present = 1;
                t->stss_n       = be32( p + 4 );
                if( t->stss_n > MAX_ENTRIES )
                    return -1;
                for( uint32_t i = 0; i < t->stss_n; i++ )
                    t->stss[i] = be32( p + 8 + 4 * i );
                break;
            case FCC('s','d','t','p') :
                t->sdtp_present = 1;
                t->sdtp_n       = (uint32_t)(box.size - box.head - 4);
                if( t->sdtp_n > MAX_SAMPLES )
                    return -1;
                for( uint32_t i = 0; i < t->sdtp_n; i++ )
                    t->sdtp[i] = p[4 + i];
                break;
            case FCC('s','t','s','c') :
                t->stsc_n = be32( p + 4 );
                if( t->stsc_n > MAX_ENTRIES )
                    return -1;
                for( uint32_t i = 0; i < t->stsc_n; i++ )
                {
                    t->stsc[i].first = be32( p + 8 + 12 * i );
                    t->stsc[i].spc   = be32( p + 12 + 12 * i );
                    t->stsc[i].sdi   = be32( p + 16 + 12 * i );
                }
                break;
            case FCC('s','t','c','o') :
                t->stco_n = be32( p + 4 );
                if( t->stco_n > MAX_ENTRIES )
                    return -1;
                for( uint32_t i = 0; i < t->stco_n; i++ )
                    t->stco[i] = be32( p + 8 + 4 * i );
                break;
            case FCC('c','o','6','4') :
                t->stco_n = be32( p + 4 );
                if( t->stco_n > MAX_ENTRIES )
                    return -1;
                for( uint32_t i = 0; i < t->stco_n; i++ )
                    t->stco[i] = be64( p + 8 + 8 * i );
                break;
            case FCC('s','t','s','z') :
                t->stsz_const = be32( p + 4 );
                t->stsz_n     = be32( p + 8 );
                if( t->stsz_n > MAX_SAMPLES )
                    return -1;
                if( t->stsz_const == 0 )
                    for( uint32_t i = 0; i < t->stsz_n; i++ )
                        t->stsz[i] = be32( p + 12 + 4 * i );
                else
                    for( uint32_t i = 0; i < t->stsz_n; i++ )
                        t->stsz[i] = t->stsz_const;
                break;
            case FCC('s','b','g','p') :
            {
                uint32_t grouping = be32( p + 4 );
                uint64_t base     = 8;
                if( p[0] == 1 )
                    base += 4;  /* grouping_type_parameter */
                uint32_t count = be32( p + base );
                if( count > MAX_ENTRIES )
                    return -1;
                sbgp_row_t *rows = NULL;
                if( grouping == FCC('r','a','p',' ') )
                {
                    t->sbgp_rap_n = count;
                    rows          = t->sbgp_rap;
                }
                else if( grouping == FCC('r','o','l','l') || grouping == FCC('p','r','o','l') )
                {
                    t->sbgp_roll_n = count;
                    rows           = t->sbgp_roll;
                }
                else
                    return -1;
                for( uint32_t i = 0; i < count; i++ )
                {
                    rows[i].count = be32( p + base + 4 + 8 * i );
                    rows[i].gdi   = be32( p + base + 8 + 8 * i );
                }
                break;
            }
            case FCC('s','g','p','d') :
            {
                uint32_t grouping       = be32( p + 4 );
                uint32_t default_length = p[0] == 1 ? be32( p + 8 ) : 0;
                uint64_t base           = p[0] == 1 ? 12 : 8;
                uint32_t count          = be32( p + base );
                if( grouping == FCC('r','a','p',' ') )
                {
                    t->sgpd_rap_n = count;
                    if( count > MAX_ENTRIES || default_length != 1 )
                        return -1;
                    for( uint32_t i = 0; i < count; i++ )
                        t->sgpd_rap[i] = p[base + 4 + i];
                }
                else if( grouping == FCC('r','o','l','l') || grouping == FCC('p','r','o','l') )
                {
                    t->sgpd_roll_n = count;
                    if( count > MAX_ENTRIES || default_length != 2 )
                        return -1;
                    for( uint32_t i = 0; i < count; i++ )
                        t->sgpd_roll[i] = (int16_t)((p[base + 4 + 2 * i] << 8) | p[base + 5 + 2 * i]);
                }
                break;
            }
            default :
                break;
        }
    }
    return ret < 0 ? -1 : 0;
}

static int parse_trak( parsed_track_t *t, const uint8_t *buf, uint64_t start, uint64_t end )
{
    box_t mdia, minf, stbl, tkhd, hdlr;
    if( find_box( buf, start, end, FCC('t','k','h','d'), &tkhd ) != 1 )
        return -1;
    const uint8_t *p = buf + tkhd.start + tkhd.head;
    t->track_id = p[0] == 1 ? be32( p + 20 ) : be32( p + 12 );
    if( find_box( buf, start, end, FCC('m','d','i','a'), &mdia ) != 1 )
        return -1;
    if( find_box( buf, mdia.start + mdia.head, mdia.start + mdia.size, FCC('h','d','l','r'), &hdlr ) != 1 )
        return -1;
    t->handler = be32( buf + hdlr.start + hdlr.head + 8 );
    if( find_box( buf, mdia.start + mdia.head, mdia.start + mdia.size, FCC('m','i','n','f'), &minf ) != 1 )
        return -1;
    if( find_box( buf, minf.start + minf.head, minf.start + minf.size, FCC('s','t','b','l'), &stbl ) != 1 )
        return -1;
    if( parse_stbl( t, buf, stbl.start + stbl.head, stbl.start + stbl.size ) < 0 )
        return -1;
    /* derive per-sample information */
    uint32_t n = t->stsz_n;
    if( n > MAX_SAMPLES )
        return -1;
    t->nsamples = n;
    uint64_t dts = 0;
    uint32_t si  = 0;
    for( uint32_t i = 0; i < t->stts_n && si < n; i++ )
        for( uint32_t j = 0; j < t->stts[i].count && si < n; j++, si++ )
        {
            t->dts[si] = dts;
            dts       += t->stts[i].value;
        }
    if( si != n )
        return -1;
    si = 0;
    for( uint32_t i = 0; i < t->ctts_n && si < n; i++ )
        for( uint32_t j = 0; j < t->ctts[i].count && si < n; j++, si++ )
        {
            int64_t offset = t->ctts_version ? (int64_t)(int32_t)t->ctts[i].value : (int64_t)t->ctts[i].value;
            t->cts[si] = (int64_t)t->dts[si] + offset;
        }
    if( t->ctts_n == 0 )
        for( si = 0; si < n; si++ )
            t->cts[si] = (int64_t)t->dts[si];
    else if( si != n )
        return -1;
    /* chunk walk for offsets and description indexes */
    uint32_t sample = 0;
    uint32_t sc     = 0;
    for( uint32_t c = 0; c < t->stco_n; c++ )
    {
        while( sc + 1 < t->stsc_n && t->stsc[sc + 1].first <= c + 1 )
            ++sc;
        uint64_t offset = t->stco[c];
        for( uint32_t j = 0; j < t->stsc[sc].spc && sample < n; j++, sample++ )
        {
            t->offset[sample] = offset;
            t->sdi[sample]    = t->stsc[sc].sdi;
            offset           += t->stsz[sample];
        }
    }
    if( sample != n )
        return -1;
    return 0;
}

static int parse_movie( const char *name, parsed_file_t *pf )
{
    memset( pf, 0, sizeof(*pf) );
    FILE *fp = fopen( name, "rb" );
    if( !fp )
        return -1;
    if( lsmash_fseek( fp, 0, SEEK_END ) != 0 )
        return -1;
    int64_t size = lsmash_ftell( fp );
    if( size <= 0 || lsmash_fseek( fp, 0, SEEK_SET ) != 0 )
        return -1;
    pf->data = malloc( size );
    if( !pf->data )
        return -1;
    if( fread( pf->data, 1, size, fp ) != (size_t)size )
        return -1;
    fclose( fp );
    pf->size = size;
    box_t moov;
    if( find_box( pf->data, 0, pf->size, FCC('m','o','o','v'), &moov ) != 1 )
        return -1;
    uint64_t pos = moov.start + moov.head;
    while( pos + 8 <= moov.start + moov.size && pf->ntracks < MAX_TRACKS )
    {
        box_t trak;
        int ret = next_box( pf->data, &pos, moov.start + moov.size, &trak );
        if( ret <= 0 )
            return -1;
        if( trak.type != FCC('t','r','a','k') )
            continue;
        if( parse_trak( &pf->tracks[pf->ntracks], pf->data, trak.start + trak.head, trak.start + trak.size ) < 0 )
            return -1;
        ++pf->ntracks;
    }
    return 0;
}

static void free_movie( parsed_file_t *pf )
{
    free( pf->data );
    pf->data = NULL;
}

/* compare two parsed tracks field by field, including sample payloads */
static int compare_tracks( parsed_file_t *pa, parsed_track_t *a, parsed_file_t *pb, parsed_track_t *b )
{
    TASK_CHECK( a->handler   == b->handler );
    TASK_CHECK( a->stsd_count == b->stsd_count );
    for( uint32_t i = 0; i < a->stsd_count && i < 8; i++ )
        TASK_CHECK( a->stsd_type[i] == b->stsd_type[i] );
    TASK_CHECK( a->stts_n == b->stts_n );
    for( uint32_t i = 0; i < a->stts_n && i < MAX_ENTRIES; i++ )
    {
        TASK_CHECK( a->stts[i].count == b->stts[i].count );
        TASK_CHECK( a->stts[i].value == b->stts[i].value );
    }
    TASK_CHECK( a->ctts_present == b->ctts_present );
    TASK_CHECK( a->ctts_version == b->ctts_version );
    TASK_CHECK( a->ctts_n == b->ctts_n );
    for( uint32_t i = 0; i < a->ctts_n && i < MAX_ENTRIES; i++ )
    {
        TASK_CHECK( a->ctts[i].count == b->ctts[i].count );
        TASK_CHECK( a->ctts[i].value == b->ctts[i].value );
    }
    TASK_CHECK( a->stss_present == b->stss_present );
    TASK_CHECK( a->stss_n == b->stss_n );
    for( uint32_t i = 0; i < a->stss_n && i < MAX_ENTRIES; i++ )
        TASK_CHECK( a->stss[i] == b->stss[i] );
    TASK_CHECK( a->sdtp_present == b->sdtp_present );
    TASK_CHECK( a->sdtp_n == b->sdtp_n );
    for( uint32_t i = 0; i < a->sdtp_n && i < MAX_SAMPLES; i++ )
        TASK_CHECK( a->sdtp[i] == b->sdtp[i] );
    TASK_CHECK( a->stsc_n == b->stsc_n );
    for( uint32_t i = 0; i < a->stsc_n && i < MAX_ENTRIES; i++ )
    {
        TASK_CHECK( a->stsc[i].first == b->stsc[i].first );
        TASK_CHECK( a->stsc[i].spc   == b->stsc[i].spc );
        TASK_CHECK( a->stsc[i].sdi   == b->stsc[i].sdi );
    }
    TASK_CHECK( a->stco_n == b->stco_n );
    for( uint32_t i = 0; i < a->stco_n && i < MAX_ENTRIES; i++ )
        TASK_CHECK( a->stco[i] == b->stco[i] );
    TASK_CHECK( a->stsz_const == b->stsz_const );
    TASK_CHECK( a->stsz_n == b->stsz_n );
    for( uint32_t i = 0; i < a->stsz_n && i < MAX_SAMPLES; i++ )
        TASK_CHECK( a->stsz[i] == b->stsz[i] );
    TASK_CHECK( a->sbgp_rap_n == b->sbgp_rap_n );
    for( uint32_t i = 0; i < a->sbgp_rap_n && i < MAX_ENTRIES; i++ )
    {
        TASK_CHECK( a->sbgp_rap[i].count == b->sbgp_rap[i].count );
        TASK_CHECK( a->sbgp_rap[i].gdi   == b->sbgp_rap[i].gdi );
    }
    TASK_CHECK( a->sgpd_rap_n == b->sgpd_rap_n );
    for( uint32_t i = 0; i < a->sgpd_rap_n && i < MAX_ENTRIES; i++ )
        TASK_CHECK( a->sgpd_rap[i] == b->sgpd_rap[i] );
    TASK_CHECK( a->sbgp_roll_n == b->sbgp_roll_n );
    for( uint32_t i = 0; i < a->sbgp_roll_n && i < MAX_ENTRIES; i++ )
    {
        TASK_CHECK( a->sbgp_roll[i].count == b->sbgp_roll[i].count );
        TASK_CHECK( a->sbgp_roll[i].gdi   == b->sbgp_roll[i].gdi );
    }
    TASK_CHECK( a->sgpd_roll_n == b->sgpd_roll_n );
    for( uint32_t i = 0; i < a->sgpd_roll_n && i < MAX_ENTRIES; i++ )
        TASK_CHECK( a->sgpd_roll[i] == b->sgpd_roll[i] );
    /* per-sample clocks, description indexes and payload bytes */
    TASK_CHECK( a->nsamples == b->nsamples );
    for( uint32_t i = 0; i < a->nsamples && i < MAX_SAMPLES; i++ )
    {
        TASK_CHECK( a->dts[i] == b->dts[i] );
        TASK_CHECK( a->cts[i] == b->cts[i] );
        TASK_CHECK( a->sdi[i] == b->sdi[i] );
        TASK_CHECK( a->offset[i] + a->stsz[i] <= pa->size );
        TASK_CHECK( b->offset[i] + b->stsz[i] <= pb->size );
        TASK_CHECK( memcmp( pa->data + a->offset[i], pb->data + b->offset[i], a->stsz[i] ) == 0 );
    }
    return 0;
}

/* match tracks of the two files by handler type and compare them */
static int compare_movies( parsed_file_t *pa, parsed_file_t *pb )
{
    TASK_CHECK( pa->ntracks == pb->ntracks );
    int used[MAX_TRACKS] = { 0 };
    for( uint32_t i = 0; i < pa->ntracks && i < MAX_TRACKS; i++ )
    {
        parsed_track_t *a = &pa->tracks[i];
        int found = -1;
        for( uint32_t j = 0; j < pb->ntracks && j < MAX_TRACKS; j++ )
            if( !used[j] && pb->tracks[j].handler == a->handler )
            {
                found = (int)j;
                break;
            }
        TASK_CHECK( found >= 0 );
        if( found < 0 )
            return 1;
        used[found] = 1;
        if( compare_tracks( pa, a, pb, &pb->tracks[found] ) )
            return 1;
    }
    return 0;
}

/* library read-back: reopen both files and compare every sample */
static int readback_compare( const char *name_a, const char *name_b )
{
    lsmash_root_t *ra = lsmash_create_root();
    lsmash_root_t *rb = lsmash_create_root();
    TASK_CHECK( ra && rb );
    lsmash_file_parameters_t fa = { 0 }, fb = { 0 };
    TASK_CHECK( lsmash_open_file( name_a, 1, &fa ) == 0 );
    TASK_CHECK( lsmash_open_file( name_b, 1, &fb ) == 0 );
    lsmash_file_t *file_a = lsmash_set_file( ra, &fa );
    lsmash_file_t *file_b = lsmash_set_file( rb, &fb );
    TASK_CHECK( file_a != NULL );
    TASK_CHECK( file_b != NULL );
    TASK_CHECK( lsmash_read_file( file_a, &fa ) >= 0 );
    TASK_CHECK( lsmash_read_file( file_b, &fb ) >= 0 );
    for( uint32_t tn = 1; ; tn++ )
    {
        uint32_t ta = lsmash_get_track_ID( ra, tn );
        uint32_t tb = lsmash_get_track_ID( rb, tn );
        TASK_CHECK( (ta == 0) == (tb == 0) );
        if( !ta )
            break;
        TASK_CHECK( lsmash_construct_timeline( ra, ta ) == 0 );
        TASK_CHECK( lsmash_construct_timeline( rb, tb ) == 0 );
        uint32_t na = lsmash_get_sample_count_in_media_timeline( ra, ta );
        uint32_t nb = lsmash_get_sample_count_in_media_timeline( rb, tb );
        TASK_CHECK( na == nb );
        for( uint32_t i = 1; i <= na; i++ )
        {
            lsmash_sample_t *sa = lsmash_get_sample_from_media_timeline( ra, ta, i );
            lsmash_sample_t *sb = lsmash_get_sample_from_media_timeline( rb, tb, i );
            TASK_CHECK( sa && sb );
            TASK_CHECK( sa->dts   == sb->dts );
            TASK_CHECK( sa->cts   == sb->cts );
            TASK_CHECK( sa->index == sb->index );
            TASK_CHECK( sa->length == sb->length );
            TASK_CHECK( memcmp( sa->data, sb->data, sa->length ) == 0 );
            lsmash_delete_sample( sa );
            lsmash_delete_sample( sb );
        }
    }
    TASK_CHECK( lsmash_close_file( &fa ) == 0 );
    TASK_CHECK( lsmash_close_file( &fb ) == 0 );
    lsmash_destroy_root( ra );
    lsmash_destroy_root( rb );
    return 0;
}

/* full comparison of the undo-produced file against the reference file */
static int check_movies_equal( const char *undo, const char *ref )
{
    parsed_file_t pa, pb;
    TASK_CHECK( parse_movie( undo, &pa ) == 0 );
    TASK_CHECK( parse_movie( ref,  &pb ) == 0 );
    int ret = compare_movies( &pa, &pb );
    free_movie( &pa );
    free_movie( &pb );
    TASK_CHECK( ret == 0 );
    TASK_CHECK( readback_compare( undo, ref ) == 0 );
    return 0;
}

/* ------------------------------------------------------------------------ */
/* tables group                                                              */
/* ------------------------------------------------------------------------ */

static int case_api_contract( void )
{
    uint32_t discarded = 777;
    movie_t m;
    /* invalid parameters and disabled-by-default behavior */
    TASK_CHECK( movie_open( &m, ".task-build/t-api-u.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( NULL, m.vtrack, 1 ) < 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, 0, 1 ) < 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, 99, 1 ) < 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 2 ) < 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, -1 ) < 0 );
    TASK_CHECK( lsmash_discard_pooled_samples( NULL, m.vtrack, &discarded ) < 0 );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, 0, &discarded ) < 0 );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, 99, &discarded ) < 0 );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, NULL ) < 0 );
    /* not enabled yet: negative and the output value is left alone */
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) < 0 );
    TASK_CHECK( discarded == 777 );
    /* enable/disable/enable are all fine before any sample */
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 0 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    /* discarding an empty pool succeeds with zero */
    discarded = 777;
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 0 );
    /* once a sample is accepted, switching is rejected and the mode is kept */
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x30, 0, 0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 0 ) < 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) < 0 );
    discarded = 0;
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 1 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x32, 0, 0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( movie_close( &m, 40, 0 ) == 0 );
    /* enabling requires sample descriptions */
    {
        lsmash_root_t *root = lsmash_create_root();
        lsmash_file_parameters_t fp = { 0 };
        io_count_t io = { 0 };
        io.fp = fopen( ".task-build/t-api-nodesc.mp4", "w+b" );
        TASK_CHECK( io.fp != NULL );
        fp.mode                = LSMASH_FILE_MODE_WRITE | LSMASH_FILE_MODE_BOX
                               | LSMASH_FILE_MODE_INITIALIZATION | LSMASH_FILE_MODE_MEDIA;
        fp.opaque              = &io;
        fp.read                = io_count_read;
        fp.write               = io_count_write;
        fp.seek                = io_count_seek;
        fp.major_brand         = ISOM_BRAND_TYPE_ISOM;
        static lsmash_brand_type brands[] = { ISOM_BRAND_TYPE_ISOM };
        fp.brands              = brands;
        fp.brand_count         = 1;
        fp.max_chunk_duration  = 1;
        fp.max_async_tolerance = 2;
        fp.max_chunk_size      = 1 << 20;
        fp.max_read_size       = 1 << 20;
        TASK_CHECK( lsmash_set_file( root, &fp ) != NULL );
        lsmash_movie_parameters_t mp;
        lsmash_initialize_movie_parameters( &mp );
        mp.timescale = 1000;
        TASK_CHECK( lsmash_set_movie_parameters( root, &mp ) == 0 );
        uint32_t track = lsmash_create_track( root, ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK );
        TASK_CHECK( track != 0 );
        lsmash_media_parameters_t media;
        lsmash_initialize_media_parameters( &media );
        media.timescale = 1000;
        TASK_CHECK( lsmash_set_media_parameters( root, track, &media ) == 0 );
        /* no sample entry at all */
        TASK_CHECK( lsmash_set_pool_undo( root, track, 1 ) < 0 );
        lsmash_destroy_root( root );
        fclose( io.fp );
    }
    /* unsupported codec: avc1/mp4a only */
    {
        movie_t c;
        TASK_CHECK( movie_open( &c, ".task-build/t-api-codec.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
        lsmash_audio_summary_t *a = (lsmash_audio_summary_t *)lsmash_create_summary( LSMASH_SUMMARY_TYPE_AUDIO );
        TASK_CHECK( a != NULL );
        a->sample_type = ISOM_CODEC_TYPE_SAMR_AUDIO;
        a->frequency   = 8000;
        a->channels    = 1;
        uint32_t atrack = lsmash_create_track( c.root, ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK );
        TASK_CHECK( atrack != 0 );
        c.atrack = atrack;
        lsmash_media_parameters_t media;
        lsmash_initialize_media_parameters( &media );
        media.timescale = 8000;
        TASK_CHECK( lsmash_set_media_parameters( c.root, atrack, &media ) == 0 );
        TASK_CHECK( lsmash_add_sample_entry( c.root, atrack, (lsmash_summary_t *)a ) == 1 );
        lsmash_cleanup_summary( (lsmash_summary_t *)a );
        TASK_CHECK( lsmash_set_pool_undo( c.root, atrack, 1 ) < 0 );
        /* the video track of the same file is still fine */
        TASK_CHECK( lsmash_set_pool_undo( c.root, c.vtrack, 1 ) == 0 );
        TASK_CHECK( append_tag( &c, c.vtrack, 16, 0x31, 0, 0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
        TASK_CHECK( append_tag( &c, atrack, 12, 0x33, 0, 0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
        TASK_CHECK( movie_close( &c, 40, 160 ) == 0 );
    }
    /* fragmented movie is rejected */
    {
        lsmash_root_t *root = lsmash_create_root();
        lsmash_file_parameters_t fp = { 0 };
        io_count_t io = { 0 };
        io.fp = fopen( ".task-build/t-api-frag.mp4", "w+b" );
        TASK_CHECK( io.fp != NULL );
        fp.mode                = LSMASH_FILE_MODE_WRITE | LSMASH_FILE_MODE_BOX
                               | LSMASH_FILE_MODE_INITIALIZATION | LSMASH_FILE_MODE_MEDIA
                               | LSMASH_FILE_MODE_FRAGMENTED;
        fp.opaque              = &io;
        fp.read                = io_count_read;
        fp.write               = io_count_write;
        fp.seek                = io_count_seek;
        fp.major_brand         = ISOM_BRAND_TYPE_ISOM;
        static lsmash_brand_type brands[] = { ISOM_BRAND_TYPE_ISOM, ISOM_BRAND_TYPE_ISO6 };
        fp.brands              = brands;
        fp.brand_count         = 2;
        fp.max_chunk_duration  = 1;
        fp.max_async_tolerance = 2;
        fp.max_chunk_size      = 1 << 20;
        fp.max_read_size       = 1 << 20;
        TASK_CHECK( lsmash_set_file( root, &fp ) != NULL );
        lsmash_movie_parameters_t mp;
        lsmash_initialize_movie_parameters( &mp );
        mp.timescale = 1000;
        TASK_CHECK( lsmash_set_movie_parameters( root, &mp ) == 0 );
        uint32_t track = lsmash_create_track( root, ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK );
        TASK_CHECK( track != 0 );
        lsmash_media_parameters_t media;
        lsmash_initialize_media_parameters( &media );
        media.timescale = 1000;
        TASK_CHECK( lsmash_set_media_parameters( root, track, &media ) == 0 );
        TASK_CHECK( lsmash_set_pool_undo( root, track, 1 ) < 0 );
        lsmash_destroy_root( root );
        fclose( io.fp );
    }
    return 0;
}

static int case_first_pool_full_discard( void )
{
    movie_t m;
    uint32_t discarded = 0;
    TASK_CHECK( movie_open( &m, ".task-build/t-full-u.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x10,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x11, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 20, 0x12, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( pooled_count( &m, m.vtrack ) == 3 );
    TASK_CHECK( table_sample_count( &m, m.vtrack ) == 3 );
    /* revoke the whole first pool */
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 3 );
    TASK_CHECK( pooled_count( &m, m.vtrack ) == 0 );
    TASK_CHECK( table_sample_count( &m, m.vtrack ) == 0 );
    /* a second discard does not subtract anything more */
    discarded = 55;
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 0 );
    /* replacements with different sizes and properties */
    TASK_CHECK( append_tag( &m, m.vtrack, 22, 0x20,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 14, 0x21, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 26, 0x22, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    /* reference: only the replacements were ever appended */
    TASK_CHECK( movie_open( &m, ".task-build/t-full-r.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 22, 0x20,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 14, 0x21, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 26, 0x22, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    TASK_CHECK( check_movies_equal( ".task-build/t-full-u.mp4", ".task-build/t-full-r.mp4" ) == 0 );
    return 0;
}

static int case_mid_run_withdraw( void )
{
    movie_t m;
    uint32_t discarded = 0;
    TASK_CHECK( movie_open( &m, ".task-build/t-run-u.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    /* committed prefix: constant dts step and constant cts offset (single runs) */
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x40,   0, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x41, 100, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x42, 200, 400, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( lsmash_flush_pooled_samples( m.root, m.vtrack, 100 ) == 0 );
    /* pooled batch extending the same stts/ctts runs */
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x43, 300, 500, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x44, 400, 600, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    {
        isom_trak_t *trak = movie_trak( &m, m.vtrack );
        isom_stbl_t *stbl = trak->mdia->minf->stbl;
        TASK_CHECK( stbl->stts->list->entry_count == 1 );
        TASK_CHECK( ((isom_stts_entry_t *)stbl->stts->list->tail->data)->sample_count == 5 );
        TASK_CHECK( stbl->ctts->list->entry_count == 1 );
        TASK_CHECK( ((isom_ctts_entry_t *)stbl->ctts->list->tail->data)->sample_count == 5 );
    }
    /* withdraw in the middle of the runs */
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 2 );
    {
        isom_trak_t *trak = movie_trak( &m, m.vtrack );
        isom_stbl_t *stbl = trak->mdia->minf->stbl;
        TASK_CHECK( stbl->stts->list->entry_count == 1 );
        TASK_CHECK( ((isom_stts_entry_t *)stbl->stts->list->tail->data)->sample_count == 3 );
        TASK_CHECK( stbl->ctts->list->entry_count == 1 );
        TASK_CHECK( ((isom_ctts_entry_t *)stbl->ctts->list->tail->data)->sample_count == 3 );
        TASK_CHECK( trak->cache->timestamp.dts == 200 );
    }
    /* replacements with a different CTS offset: the runs must split correctly */
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x45, 300, 550, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x46, 400, 650, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    /* reference */
    TASK_CHECK( movie_open( &m, ".task-build/t-run-r.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x40,   0, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x41, 100, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x42, 200, 400, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( lsmash_flush_pooled_samples( m.root, m.vtrack, 100 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x45, 300, 550, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x46, 400, 650, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    TASK_CHECK( check_movies_equal( ".task-build/t-run-u.mp4", ".task-build/t-run-r.mp4" ) == 0 );
    return 0;
}

static int case_description_switch_commit( void )
{
    movie_t m;
    uint32_t discarded = 0;
    TASK_CHECK( movie_open( &m, ".task-build/t-desc-u.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x50,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x51, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    /* the description switch commits the first pool; the switching sample stays revocable */
    TASK_CHECK( append_tag( &m, m.vtrack, 20, 0x52, 200, 200, 2, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( pooled_count( &m, m.vtrack ) == 1 );
    TASK_CHECK( append_tag( &m, m.vtrack, 20, 0x53, 300, 300, 2, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 2 );
    {
        isom_trak_t *trak = movie_trak( &m, m.vtrack );
        isom_stbl_t *stbl = trak->mdia->minf->stbl;
        /* the committed chunk survives */
        TASK_CHECK( stbl->stsc->list->entry_count == 1 );
        TASK_CHECK( ((isom_stsc_entry_t *)stbl->stsc->list->tail->data)->sample_description_index == 1 );
        TASK_CHECK( stbl->stco->list->entry_count == 1 );
        TASK_CHECK( stbl->stsz->sample_count == 2 );
    }
    /* replacements on the second description */
    TASK_CHECK( append_tag( &m, m.vtrack, 24, 0x54, 200, 200, 2, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 24, 0x55, 300, 300, 2, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    /* reference */
    TASK_CHECK( movie_open( &m, ".task-build/t-desc-r.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x50,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x51, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 24, 0x54, 200, 200, 2, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 24, 0x55, 300, 300, 2, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    TASK_CHECK( check_movies_equal( ".task-build/t-desc-u.mp4", ".task-build/t-desc-r.mp4" ) == 0 );
    return 0;
}

static int case_sdtp_withdraw( void )
{
    movie_t m;
    uint32_t discarded = 0;
    lsmash_sample_property_t prop = { 0 };
    /* part 1: the sdtp box introduced by a revoked batch must disappear */
    TASK_CHECK( movie_open( &m, ".task-build/t-sdtp1-u.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x60,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x61, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( lsmash_flush_pooled_samples( m.root, m.vtrack, 100 ) == 0 );
    prop.ra_flags    = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE;
    prop.independent = ISOM_SAMPLE_IS_INDEPENDENT;
    prop.disposable  = ISOM_SAMPLE_IS_DISPOSABLE;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x62, 200, 200, 1, &prop ) == 0 );
    prop.independent = ISOM_SAMPLE_IS_NOT_INDEPENDENT;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x63, 300, 300, 1, &prop ) == 0 );
    TASK_CHECK( LSMASH_IS_EXISTING_BOX( movie_trak( &m, m.vtrack )->mdia->minf->stbl->sdtp ) );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 2 );
    TASK_CHECK( LSMASH_IS_NON_EXISTING_BOX( movie_trak( &m, m.vtrack )->mdia->minf->stbl->sdtp ) );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x64, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x65, 300, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    TASK_CHECK( movie_open( &m, ".task-build/t-sdtp1-r.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x60,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x61, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( lsmash_flush_pooled_samples( m.root, m.vtrack, 100 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x64, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x65, 300, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    TASK_CHECK( check_movies_equal( ".task-build/t-sdtp1-u.mp4", ".task-build/t-sdtp1-r.mp4" ) == 0 );
    /* part 2: entries appended by a revoked batch are truncated from an existing sdtp */
    TASK_CHECK( movie_open( &m, ".task-build/t-sdtp2-u.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    prop.ra_flags    = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
    prop.independent = ISOM_SAMPLE_IS_INDEPENDENT;
    prop.disposable  = ISOM_SAMPLE_IS_NOT_DISPOSABLE;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x66,   0,   0, 1, &prop ) == 0 );
    prop.ra_flags    = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE;
    prop.disposable  = ISOM_SAMPLE_IS_DISPOSABLE;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x67, 100, 100, 1, &prop ) == 0 );
    TASK_CHECK( lsmash_flush_pooled_samples( m.root, m.vtrack, 100 ) == 0 );
    prop.disposable = ISOM_SAMPLE_IS_NOT_DISPOSABLE;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x68, 200, 200, 1, &prop ) == 0 );
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x69, 300, 300, 1, &prop ) == 0 );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 2 );
    TASK_CHECK( movie_trak( &m, m.vtrack )->mdia->minf->stbl->sdtp->list->entry_count == 2 );
    prop.independent = ISOM_SAMPLE_IS_NOT_INDEPENDENT;
    prop.disposable  = ISOM_SAMPLE_IS_DISPOSABLE;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x6a, 200, 200, 1, &prop ) == 0 );
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x6b, 300, 300, 1, &prop ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    TASK_CHECK( movie_open( &m, ".task-build/t-sdtp2-r.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    prop.ra_flags    = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
    prop.independent = ISOM_SAMPLE_IS_INDEPENDENT;
    prop.disposable  = ISOM_SAMPLE_IS_NOT_DISPOSABLE;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x66,   0,   0, 1, &prop ) == 0 );
    prop.ra_flags    = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE;
    prop.disposable  = ISOM_SAMPLE_IS_DISPOSABLE;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x67, 100, 100, 1, &prop ) == 0 );
    TASK_CHECK( lsmash_flush_pooled_samples( m.root, m.vtrack, 100 ) == 0 );
    prop.independent = ISOM_SAMPLE_IS_NOT_INDEPENDENT;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x6a, 200, 200, 1, &prop ) == 0 );
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x6b, 300, 300, 1, &prop ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    TASK_CHECK( check_movies_equal( ".task-build/t-sdtp2-u.mp4", ".task-build/t-sdtp2-r.mp4" ) == 0 );
    return 0;
}

/* ------------------------------------------------------------------------ */
/* boundaries group                                                          */
/* ------------------------------------------------------------------------ */

static int case_duration_threshold_flush( void )
{
    movie_t m;
    uint32_t discarded = 0;
    /* two samples per chunk: the third sample in decode order closes the chunk */
    TASK_CHECK( movie_open( &m, ".task-build/t-dur-u.mp4", 0.15, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x70,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x71, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    /* this append commits [0x70,0x71]; the triggering sample itself stays revocable */
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x72, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( pooled_count( &m, m.vtrack ) == 1 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x73, 300, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x74, 400, 400, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( pooled_count( &m, m.vtrack ) == 1 );
    /* only the last sample is still pooled */
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 1 );
    {
        isom_trak_t *trak = movie_trak( &m, m.vtrack );
        isom_stbl_t *stbl = trak->mdia->minf->stbl;
        TASK_CHECK( stbl->stsz->sample_count == 4 );
        TASK_CHECK( stbl->stco->list->entry_count == 2 );
        /* the delta derivation for the last committed sample was rolled back */
        TASK_CHECK( stbl->stts->list->entry_count == 1 );
        TASK_CHECK( ((isom_stts_entry_t *)stbl->stts->list->tail->data)->sample_count == 3 );
        TASK_CHECK( trak->cache->timestamp.dts == 300 );
    }
    /* the replacement re-derives the previous committed sample's duration */
    TASK_CHECK( append_tag( &m, m.vtrack, 20, 0x75, 450, 450, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( movie_close( &m, 150, 0 ) == 0 );
    /* reference */
    TASK_CHECK( movie_open( &m, ".task-build/t-dur-r.mp4", 0.15, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x70,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x71, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x72, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x73, 300, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 20, 0x75, 450, 450, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( movie_close( &m, 150, 0 ) == 0 );
    TASK_CHECK( check_movies_equal( ".task-build/t-dur-u.mp4", ".task-build/t-dur-r.mp4" ) == 0 );
    return 0;
}

static int case_size_threshold_flush( void )
{
    movie_t m;
    uint32_t discarded = 0;
    TASK_CHECK( movie_open( &m, ".task-build/t-size-u.mp4", 10, 20, 40, 0, 0, 0 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x80,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x81, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    /* 16+18+16 exceeds the 40-byte pool: the first two samples are committed */
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x82, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( pooled_count( &m, m.vtrack ) == 1 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x83, 300, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x84, 400, 400, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( pooled_count( &m, m.vtrack ) == 1 );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 1 );
    TASK_CHECK( table_sample_count( &m, m.vtrack ) == 4 );
    TASK_CHECK( append_tag( &m, m.vtrack, 24, 0x85, 400, 400, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    /* reference */
    TASK_CHECK( movie_open( &m, ".task-build/t-size-r.mp4", 10, 20, 40, 0, 0, 0 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x80,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x81, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x82, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x83, 300, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 24, 0x85, 400, 400, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    TASK_CHECK( check_movies_equal( ".task-build/t-size-u.mp4", ".task-build/t-size-r.mp4" ) == 0 );
    return 0;
}

static int case_async_arbitration_flush( void )
{
    movie_t m;
    uint32_t discarded = 0;
    /* small async tolerance: a far-ahead audio track commits the video pool */
    TASK_CHECK( movie_open( &m, ".task-build/t-async-u.mp4", 0.1, 0.25, 4 * 1024 * 1024, 1, 0, 0 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x90,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x91, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    for( uint32_t i = 0; i < 15; i++ )
        TASK_CHECK( append_tag( &m, m.atrack, 12, (uint8_t)(0xa0 + i), 1024 * i, 1024 * i, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    /* the audio burst committed the first video pool */
    TASK_CHECK( pooled_count( &m, m.vtrack ) == 0 );
    TASK_CHECK( table_sample_count( &m, m.vtrack ) == 2 );
    /* new video samples are pooled again and revocable */
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x92, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x93, 300, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 2 );
    TASK_CHECK( table_sample_count( &m, m.vtrack ) == 2 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x94, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x95, 300, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 1024 ) == 0 );
    /* reference */
    TASK_CHECK( movie_open( &m, ".task-build/t-async-r.mp4", 0.1, 0.25, 4 * 1024 * 1024, 1, 0, 0 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x90,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x91, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    for( uint32_t i = 0; i < 15; i++ )
        TASK_CHECK( append_tag( &m, m.atrack, 12, (uint8_t)(0xa0 + i), 1024 * i, 1024 * i, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x94, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x95, 300, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 1024 ) == 0 );
    TASK_CHECK( check_movies_equal( ".task-build/t-async-u.mp4", ".task-build/t-async-r.mp4" ) == 0 );
    return 0;
}

static int case_interleaved_tracks( void )
{
    movie_t m;
    uint32_t discarded = 0;
    TASK_CHECK( movie_open( &m, ".task-build/t-inter-u.mp4", 10, 20, 4 * 1024 * 1024, 1, 0, 0 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.atrack, 1 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0xb0,   0,    0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.atrack, 12, 0xc0,   0,    0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0xb1, 100,  100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.atrack, 12, 0xc1, 1024, 1024, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0xb2, 200,  200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.atrack, 12, 0xc2, 2048, 2048, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    /* commit the video track only; the audio pool stays pooled */
    TASK_CHECK( lsmash_flush_pooled_samples( m.root, m.vtrack, 100 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0xb3, 300,  300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.atrack, 12, 0xc3, 3072, 3072, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0xb4, 400,  400, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.atrack, 12, 0xc4, 4096, 4096, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    /* each track revokes independently of the other */
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 2 );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.atrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 5 );
    TASK_CHECK( table_sample_count( &m, m.vtrack ) == 3 );
    TASK_CHECK( table_sample_count( &m, m.atrack ) == 0 );
    /* replacements resume from the revoked positions */
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0xb5, 300,  300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.atrack, 14, 0xc5,    0,    0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0xb6, 400,  400, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.atrack, 14, 0xc6, 1024, 1024, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 1024 ) == 0 );
    /* reference */
    TASK_CHECK( movie_open( &m, ".task-build/t-inter-r.mp4", 10, 20, 4 * 1024 * 1024, 1, 0, 0 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0xb0,   0,    0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0xb1, 100,  100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0xb2, 200,  200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( lsmash_flush_pooled_samples( m.root, m.vtrack, 100 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0xb5, 300,  300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.atrack, 14, 0xc5,    0,    0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0xb6, 400,  400, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.atrack, 14, 0xc6, 1024, 1024, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 1024 ) == 0 );
    TASK_CHECK( check_movies_equal( ".task-build/t-inter-u.mp4", ".task-build/t-inter-r.mp4" ) == 0 );
    return 0;
}

static int case_no_io_and_bounded_state( void )
{
    movie_t m;
    uint32_t discarded = 0;
    /* automatic (duration threshold) commits keep the roll grouping alive */
    TASK_CHECK( movie_open( &m, ".task-build/t-io-u.mp4", 0.15, 20, 4 * 1024 * 1024, 0, 0, 1 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    /* the undo record must not grow with the number of committed chunks */
    uint64_t footprint = 0;
    for( uint32_t i = 0; i < 7; i++ )
    {
        TASK_CHECK( append_tag( &m, m.vtrack, 16, (uint8_t)(0xd0 + i), 100 * i, 100 * i, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
        if( pooled_count( &m, m.vtrack ) == 1 && table_sample_count( &m, m.vtrack ) > 1 )
        {
            /* this append committed the previous pool */
            uint64_t now = isom_pool_undo_footprint( movie_trak( &m, m.vtrack ) );
            if( !footprint )
                footprint = now;
            else
                TASK_CHECK( now == footprint );
        }
    }
    TASK_CHECK( footprint != 0 );
    TASK_CHECK( table_sample_count( &m, m.vtrack ) == 7 );
    /* discard performs no I/O at all */
    unsigned long writes = m.io.write_calls;
    unsigned long seeks  = m.io.seek_calls;
    unsigned long reads  = m.io.read_calls;
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 1 );
    TASK_CHECK( m.io.write_calls == writes );
    TASK_CHECK( m.io.seek_calls  == seeks );
    TASK_CHECK( m.io.read_calls  == reads );
    /* a repeated discard also stays I/O free and reports zero */
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 0 );
    TASK_CHECK( m.io.write_calls == writes );
    TASK_CHECK( m.io.seek_calls  == seeks );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0xd7, 600, 600, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    /* reference: the revoked sample was never appended */
    TASK_CHECK( movie_open( &m, ".task-build/t-io-r.mp4", 0.15, 20, 4 * 1024 * 1024, 0, 0, 1 ) == 0 );
    for( uint32_t i = 0; i < 6; i++ )
        TASK_CHECK( append_tag( &m, m.vtrack, 16, (uint8_t)(0xd0 + i), 100 * i, 100 * i, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0xd7, 600, 600, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    TASK_CHECK( check_movies_equal( ".task-build/t-io-u.mp4", ".task-build/t-io-r.mp4" ) == 0 );
    return 0;
}

/* ------------------------------------------------------------------------ */
/* recovery group                                                            */
/* ------------------------------------------------------------------------ */

static int case_sync_point_withdraw( void )
{
    movie_t m;
    uint32_t discarded = 0;
    TASK_CHECK( movie_open( &m, ".task-build/t-sync-u.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    /* committed prefix with a sync table */
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x10,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x11, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x12, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( lsmash_flush_pooled_samples( m.root, m.vtrack, 100 ) == 0 );
    {
        isom_trak_t *trak = movie_trak( &m, m.vtrack );
        TASK_CHECK( LSMASH_IS_EXISTING_BOX( trak->mdia->minf->stbl->stss ) );
        TASK_CHECK( trak->mdia->minf->stbl->stss->list->entry_count == 2 );
        TASK_CHECK( trak->cache->all_sync == 0 );
    }
    /* pooled batch: a sync sample extends stss */
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x13, 300, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x14, 400, 400, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( movie_trak( &m, m.vtrack )->mdia->minf->stbl->stss->list->entry_count == 3 );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 2 );
    TASK_CHECK( movie_trak( &m, m.vtrack )->mdia->minf->stbl->stss->list->entry_count == 2 );
    /* replacements without any sync sample: stss must stay as committed */
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x15, 300, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x16, 400, 400, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    /* reference */
    TASK_CHECK( movie_open( &m, ".task-build/t-sync-r.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x10,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x11, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x12, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( lsmash_flush_pooled_samples( m.root, m.vtrack, 100 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x15, 300, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x16, 400, 400, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    TASK_CHECK( check_movies_equal( ".task-build/t-sync-u.mp4", ".task-build/t-sync-r.mp4" ) == 0 );
    /* second part: revoking the first non-sync sample removes the whole stss box */
    TASK_CHECK( movie_open( &m, ".task-build/t-sync2-u.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x18,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x19, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( movie_trak( &m, m.vtrack )->cache->all_sync == 1 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x1a, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( LSMASH_IS_EXISTING_BOX( movie_trak( &m, m.vtrack )->mdia->minf->stbl->stss ) );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 3 );
    TASK_CHECK( LSMASH_IS_NON_EXISTING_BOX( movie_trak( &m, m.vtrack )->mdia->minf->stbl->stss ) );
    TASK_CHECK( movie_trak( &m, m.vtrack )->cache->all_sync == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x1b,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x1c, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    TASK_CHECK( movie_open( &m, ".task-build/t-sync2-r.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x1b,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x1c, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    TASK_CHECK( check_movies_equal( ".task-build/t-sync2-u.mp4", ".task-build/t-sync2-r.mp4" ) == 0 );
    return 0;
}

static int case_rap_roll_recovery_withdraw( void )
{
    movie_t m;
    uint32_t discarded = 0;
    lsmash_sample_property_t prop = { 0 };
    /* automatic commits only: the roll grouping cache must survive them */
    TASK_CHECK( movie_open( &m, ".task-build/t-roll-u.mp4", 0.35, 20, 4 * 1024 * 1024, 0, 1, 1 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    /* a sync sample and the start of a post-roll recovery sequence */
    prop.ra_flags = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x20,   0,   0, 1, &prop ) == 0 );
    prop.ra_flags              = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_POST_ROLL_START;
    prop.post_roll.identifier  = 10;
    prop.post_roll.complete    = 13;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x21, 100, 100, 1, &prop ) == 0 );
    prop.ra_flags              = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE;
    prop.post_roll.identifier  = 11;
    prop.post_roll.complete    = 0;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x22, 200, 200, 1, &prop ) == 0 );
    prop.post_roll.identifier  = 12;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x23, 300, 300, 1, &prop ) == 0 );
    /* the next append commits the four samples above and stays revocable */
    prop.post_roll.identifier  = 13;    /* recovery point of the first group */
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x24, 400, 400, 1, &prop ) == 0 );
    TASK_CHECK( pooled_count( &m, m.vtrack ) == 1 );
    uint32_t committed_roll_sbgp = 0, committed_roll_sgpd = 0, committed_rap_sbgp = 0, committed_rap_sgpd = 0;
    {
        isom_trak_t *trak = movie_trak( &m, m.vtrack );
        isom_stbl_t *stbl = trak->mdia->minf->stbl;
        isom_sbgp_t *sbgp = isom_get_roll_recovery_sample_to_group( &stbl->sbgp_list );
        isom_sgpd_t *sgpd = isom_get_roll_recovery_sample_group_description( &stbl->sgpd_list );
        TASK_CHECK( LSMASH_IS_EXISTING_BOX( sbgp ) && LSMASH_IS_EXISTING_BOX( sgpd ) );
        committed_roll_sbgp = sbgp->list->entry_count;
        committed_roll_sgpd = sgpd->list->entry_count;
        TASK_CHECK( committed_roll_sbgp != 0 );
        TASK_CHECK( sgpd->list->entry_count == 0 );     /* recovery not established yet */
        isom_sbgp_t *rap_sbgp = isom_get_sample_to_group         ( stbl, ISOM_GROUP_TYPE_RAP );
        isom_sgpd_t *rap_sgpd = isom_get_sample_group_description( stbl, ISOM_GROUP_TYPE_RAP );
        TASK_CHECK( LSMASH_IS_EXISTING_BOX( rap_sbgp ) && LSMASH_IS_EXISTING_BOX( rap_sgpd ) );
        committed_rap_sbgp = rap_sbgp->list->entry_count;
        committed_rap_sgpd = rap_sgpd->list->entry_count;
        TASK_CHECK( committed_rap_sbgp != 0 && committed_rap_sgpd != 0 );
        /* the recovery group started by the committed samples is still open */
        TASK_CHECK( trak->cache->roll.pool && trak->cache->roll.pool->entry_count == 2 );
    }
    /* revoked batch: completes the recovery and starts another group */
    prop.ra_flags              = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_POST_ROLL_START;
    prop.post_roll.identifier  = 20;
    prop.post_roll.complete    = 22;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x25, 500, 500, 1, &prop ) == 0 );
    prop.ra_flags              = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE;
    prop.post_roll.identifier  = 21;
    prop.post_roll.complete    = 0;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x26, 600, 600, 1, &prop ) == 0 );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 3 );
    {
        isom_trak_t *trak = movie_trak( &m, m.vtrack );
        isom_stbl_t *stbl = trak->mdia->minf->stbl;
        isom_sbgp_t *sbgp = isom_get_roll_recovery_sample_to_group( &stbl->sbgp_list );
        isom_sgpd_t *sgpd = isom_get_roll_recovery_sample_group_description( &stbl->sgpd_list );
        /* the completed recovery and the second group are gone */
        TASK_CHECK( sbgp->list->entry_count == committed_roll_sbgp );
        TASK_CHECK( sgpd->list->entry_count == committed_roll_sgpd );
        isom_sbgp_t *rap_sbgp = isom_get_sample_to_group         ( stbl, ISOM_GROUP_TYPE_RAP );
        isom_sgpd_t *rap_sgpd = isom_get_sample_group_description( stbl, ISOM_GROUP_TYPE_RAP );
        TASK_CHECK( rap_sbgp->list->entry_count == committed_rap_sbgp );
        TASK_CHECK( rap_sgpd->list->entry_count == committed_rap_sgpd );
        TASK_CHECK( trak->cache->roll.pool && trak->cache->roll.pool->entry_count == 2 );
        TASK_CHECK( trak->cache->timestamp.dts == 300 );
    }
    /* replacements: complete the recovery differently and resync */
    prop.ra_flags              = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE;
    prop.post_roll.identifier  = 13;
    prop.post_roll.complete    = 0;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x27, 400, 400, 1, &prop ) == 0 );
    prop.ra_flags              = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
    prop.post_roll.identifier  = 0;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x28, 500, 500, 1, &prop ) == 0 );
    prop.ra_flags              = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE;
    prop.post_roll.identifier  = 14;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x29, 600, 600, 1, &prop ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    /* reference */
    TASK_CHECK( movie_open( &m, ".task-build/t-roll-r.mp4", 0.35, 20, 4 * 1024 * 1024, 0, 1, 1 ) == 0 );
    prop.ra_flags = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x20,   0,   0, 1, &prop ) == 0 );
    prop.ra_flags              = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_POST_ROLL_START;
    prop.post_roll.identifier  = 10;
    prop.post_roll.complete    = 13;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x21, 100, 100, 1, &prop ) == 0 );
    prop.ra_flags              = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE;
    prop.post_roll.identifier  = 11;
    prop.post_roll.complete    = 0;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x22, 200, 200, 1, &prop ) == 0 );
    prop.post_roll.identifier  = 12;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x23, 300, 300, 1, &prop ) == 0 );
    prop.post_roll.identifier  = 13;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x27, 400, 400, 1, &prop ) == 0 );
    prop.ra_flags              = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
    prop.post_roll.identifier  = 0;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x28, 500, 500, 1, &prop ) == 0 );
    prop.ra_flags              = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE;
    prop.post_roll.identifier  = 14;
    TASK_CHECK( append_prop( &m, m.vtrack, 16, 0x29, 600, 600, 1, &prop ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    TASK_CHECK( check_movies_equal( ".task-build/t-roll-u.mp4", ".task-build/t-roll-r.mp4" ) == 0 );
    /* the recovery grouping must actually be present in the compared files */
    {
        parsed_file_t pf;
        TASK_CHECK( parse_movie( ".task-build/t-roll-u.mp4", &pf ) == 0 );
        TASK_CHECK( pf.ntracks == 1 );
        TASK_CHECK( pf.tracks[0].sbgp_roll_n != 0 );
        TASK_CHECK( pf.tracks[0].sgpd_roll_n != 0 );
        TASK_CHECK( pf.tracks[0].sbgp_rap_n  != 0 );
        TASK_CHECK( pf.tracks[0].sgpd_rap_n  != 0 );
        free_movie( &pf );
    }
    return 0;
}

static int case_bframe_reorder_withdraw( void )
{
    movie_t m;
    uint32_t discarded = 0;
    /* B-frame display offsets, including negative composition offsets */
    static const struct { int dts; int cts; } frame[6] =
    {
        { 0, 2 }, { 1, 0 }, { 2, 1 }, { 3, 5 }, { 4, 3 }, { 5, 4 }
    };
    TASK_CHECK( movie_open( &m, ".task-build/t-bfrm-u.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    for( int i = 0; i < 4; i++ )
        TASK_CHECK( append_tag( &m, m.vtrack, 16, (uint8_t)(0x30 + i), frame[i].dts * 100, frame[i].cts * 100, 1,
                                i == 0 ? ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC : ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( lsmash_flush_pooled_samples( m.root, m.vtrack, 100 ) == 0 );
    uint32_t committed_ctts_runs = 0;
    {
        isom_trak_t *trak = movie_trak( &m, m.vtrack );
        isom_stbl_t *stbl = trak->mdia->minf->stbl;
        TASK_CHECK( LSMASH_IS_EXISTING_BOX( stbl->ctts ) );
        TASK_CHECK( stbl->ctts->version == 1 );     /* negative offsets required it */
        committed_ctts_runs = stbl->ctts->list->entry_count;
        TASK_CHECK( committed_ctts_runs > 1 );      /* reordered: several runs */
    }
    /* revoked batch continues the reordering */
    for( int i = 4; i < 6; i++ )
        TASK_CHECK( append_tag( &m, m.vtrack, 16, (uint8_t)(0x38 + i), frame[i].dts * 100, frame[i].cts * 100, 1,
                                ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 2 );
    {
        isom_trak_t *trak = movie_trak( &m, m.vtrack );
        isom_stbl_t *stbl = trak->mdia->minf->stbl;
        TASK_CHECK( stbl->ctts->list->entry_count == committed_ctts_runs );
        TASK_CHECK( trak->cache->timestamp.cts == 500 );
        TASK_CHECK( trak->cache->timestamp.dts == 300 );
    }
    /* replacements with a different display offset pattern */
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x3a, 400, 700, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x3b, 500, 400, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    /* reference */
    TASK_CHECK( movie_open( &m, ".task-build/t-bfrm-r.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    for( int i = 0; i < 4; i++ )
        TASK_CHECK( append_tag( &m, m.vtrack, 16, (uint8_t)(0x30 + i), frame[i].dts * 100, frame[i].cts * 100, 1,
                                i == 0 ? ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC : ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( lsmash_flush_pooled_samples( m.root, m.vtrack, 100 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x3a, 400, 700, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 18, 0x3b, 500, 400, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    TASK_CHECK( check_movies_equal( ".task-build/t-bfrm-u.mp4", ".task-build/t-bfrm-r.mp4" ) == 0 );
    return 0;
}

static int case_repeated_discard_cycles( void )
{
    movie_t m;
    uint32_t discarded = 0;
    TASK_CHECK( movie_open( &m, ".task-build/t-rep-u.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( lsmash_set_pool_undo( m.root, m.vtrack, 1 ) == 0 );
    /* cycle 1 */
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x40,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x41, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 2 );
    /* cycle 2 */
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x42, 0, 0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 1 );
    /* cycle 3 with a redundant discard in between */
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x43,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x44, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 16, 0x45, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 3 );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 0 );
    TASK_CHECK( table_sample_count( &m, m.vtrack ) == 0 );
    /* the surviving batch */
    TASK_CHECK( append_tag( &m, m.vtrack, 20, 0x46,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 20, 0x47, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 20, 0x48, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( lsmash_flush_pooled_samples( m.root, m.vtrack, 100 ) == 0 );
    /* one more revoke after the explicit flush, then finish */
    TASK_CHECK( append_tag( &m, m.vtrack, 20, 0x49, 300, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( lsmash_discard_pooled_samples( m.root, m.vtrack, &discarded ) == 0 );
    TASK_CHECK( discarded == 1 );
    TASK_CHECK( append_tag( &m, m.vtrack, 22, 0x4a, 300, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    /* reference */
    TASK_CHECK( movie_open( &m, ".task-build/t-rep-r.mp4", 10, 20, 4 * 1024 * 1024, 0, 0, 0 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 20, 0x46,   0,   0, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 20, 0x47, 100, 100, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 20, 0x48, 200, 200, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE ) == 0 );
    TASK_CHECK( lsmash_flush_pooled_samples( m.root, m.vtrack, 100 ) == 0 );
    TASK_CHECK( append_tag( &m, m.vtrack, 22, 0x4a, 300, 300, 1, ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC ) == 0 );
    TASK_CHECK( movie_close( &m, 100, 0 ) == 0 );
    TASK_CHECK( check_movies_equal( ".task-build/t-rep-u.mp4", ".task-build/t-rep-r.mp4" ) == 0 );
    return 0;
}

/* ------------------------------------------------------------------------ */
/* registration                                                              */
/* ------------------------------------------------------------------------ */

const struct task_case task_cases[] =
{
    { "tables",     "api_contract",               case_api_contract               },
    { "tables",     "first_pool_full_discard",    case_first_pool_full_discard    },
    { "tables",     "mid_run_withdraw",           case_mid_run_withdraw           },
    { "tables",     "description_switch_commit",  case_description_switch_commit  },
    { "tables",     "sdtp_withdraw",              case_sdtp_withdraw              },
    { "boundaries", "duration_threshold_flush",   case_duration_threshold_flush   },
    { "boundaries", "size_threshold_flush",       case_size_threshold_flush       },
    { "boundaries", "async_arbitration_flush",    case_async_arbitration_flush    },
    { "boundaries", "interleaved_tracks",         case_interleaved_tracks         },
    { "boundaries", "no_io_and_bounded_state",    case_no_io_and_bounded_state    },
    { "recovery",   "sync_point_withdraw",        case_sync_point_withdraw        },
    { "recovery",   "rap_roll_recovery_withdraw", case_rap_roll_recovery_withdraw },
    { "recovery",   "bframe_reorder_withdraw",    case_bframe_reorder_withdraw    },
    { "recovery",   "repeated_discard_cycles",    case_repeated_discard_cycles    },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
