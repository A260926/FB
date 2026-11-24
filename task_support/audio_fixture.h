#ifndef TASK_AUDIO_FIXTURE_H
#define TASK_AUDIO_FIXTURE_H
#include <FLAC/stream_encoder.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/metadata.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
enum { AUDIO_FRAMES=4099, AUDIO_CHANNELS=2, AUDIO_CAP=1048576 };
struct audio_sink { unsigned char *data; size_t pos, size; int disabled; };
static int32_t audio_value(unsigned frame, unsigned channel) {
    return (int32_t)(((frame*37u + (frame%19u)*193u + channel*7001u) % 60001u))-30000;
}
static FLAC__StreamEncoderWriteStatus audio_write(const FLAC__StreamEncoder *e,const FLAC__byte *p,size_t n,uint32_t s,uint32_t f,void *u) {
    struct audio_sink *a=u; (void)e;(void)s;(void)f;
    if(a->disabled || n>AUDIO_CAP-a->pos) return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    memcpy(a->data+a->pos,p,n);a->pos+=n;if(a->pos>a->size)a->size=a->pos;
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}
static FLAC__StreamEncoderSeekStatus audio_seek(const FLAC__StreamEncoder *e,FLAC__uint64 n,void *u) {
    struct audio_sink *a=u;(void)e;if(n>a->size)return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
    a->pos=(size_t)n;return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}
static FLAC__StreamEncoderTellStatus audio_tell(const FLAC__StreamEncoder *e,FLAC__uint64 *n,void *u) {
    (void)e;*n=((struct audio_sink*)u)->pos;return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}
static FLAC__StreamEncoder *audio_encoder(struct audio_sink *sink) {
    FLAC__StreamEncoder *e=FLAC__stream_encoder_new();if(!e)return NULL;
    if(!FLAC__stream_encoder_set_compression_level(e,5) ||
       !FLAC__stream_encoder_set_channels(e,2) || !FLAC__stream_encoder_set_bits_per_sample(e,16) ||
       !FLAC__stream_encoder_set_sample_rate(e,48000) || !FLAC__stream_encoder_set_blocksize(e,256) ||
       !FLAC__stream_encoder_set_verify(e,0) ||
       FLAC__stream_encoder_init_stream(e,audio_write,audio_seek,audio_tell,NULL,sink)!=FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        FLAC__stream_encoder_delete(e);return NULL;
    }return e;
}
struct audio_decode { struct audio_sink *sink; size_t pos; unsigned frames; int bad,errors,md5_nonzero; };
static FLAC__StreamDecoderReadStatus audio_read(const FLAC__StreamDecoder *d,FLAC__byte *out,size_t *n,void *u) {
    struct audio_decode *a=u;size_t left=a->sink->size-a->pos;(void)d;
    if(*n>left)*n=left;if(!*n)return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    memcpy(out,a->sink->data+a->pos,*n);a->pos+=*n;return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}
static FLAC__StreamDecoderWriteStatus audio_decoded(const FLAC__StreamDecoder *d,const FLAC__Frame *f,const FLAC__int32 *const in[],void *u) {
    struct audio_decode *a=u;unsigned i,c;(void)d;
    if(f->header.channels!=2 || a->frames+f->header.blocksize>AUDIO_FRAMES)return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    for(i=0;i<f->header.blocksize;i++)for(c=0;c<2;c++)if(in[c][i]!=audio_value(a->frames+i,c))a->bad=1;
    a->frames+=f->header.blocksize;return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void audio_metadata(const FLAC__StreamDecoder *d,const FLAC__StreamMetadata *m,void *u) {
    struct audio_decode *a=u;unsigned i;(void)d;
    if(m->type==FLAC__METADATA_TYPE_STREAMINFO) {
        if(m->data.stream_info.total_samples!=AUDIO_FRAMES)a->bad=1;
        for(i=0;i<16;i++)a->md5_nonzero|=m->data.stream_info.md5sum[i];
    }
}
static void audio_error(const FLAC__StreamDecoder *d,FLAC__StreamDecoderErrorStatus s,void *u) {
    (void)d;(void)s;((struct audio_decode*)u)->errors++;
}
#endif
