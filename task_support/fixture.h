#ifndef TASK_FLAC_FIXTURE_H
#define TASK_FLAC_FIXTURE_H
/* Ordinary production encoding and full-input decoding; no pump solution. */
#include <FLAC/stream_encoder.h>
#include <FLAC/stream_decoder.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define FIXTURE_FRAMES 1536
struct fixture {unsigned char *data;size_t len,cap,pos,frames;int errors,metadata,md5_nonzero;};
static FLAC__int32 fixture_sample(size_t i,unsigned c){return c?(FLAC__int32)((i*73)%16000)-8000:(FLAC__int32)((i*37)%12000)-6000;}
static FLAC__StreamEncoderWriteStatus fixture_write(const FLAC__StreamEncoder *e,const FLAC__byte b[],size_t n,unsigned s,unsigned f,void *v){
 (void)e;(void)s;(void)f;struct fixture *x=v;assert(n<1000000&&x->pos<10000000);
 if(x->pos+n>x->cap){x->cap=(x->pos+n)*2;x->data=realloc(x->data,x->cap);assert(x->data);}
 memcpy(x->data+x->pos,b,n);x->pos+=n;if(x->pos>x->len)x->len=x->pos;return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}
static FLAC__StreamEncoderSeekStatus fixture_seek(const FLAC__StreamEncoder *e,FLAC__uint64 p,void *v){
 (void)e;struct fixture *x=v;if(p>x->len)return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;x->pos=(size_t)p;return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}
static FLAC__StreamEncoderTellStatus fixture_tell(const FLAC__StreamEncoder *e,FLAC__uint64 *p,void *v){(void)e;*p=((struct fixture *)v)->pos;return FLAC__STREAM_ENCODER_TELL_STATUS_OK;}
static void fixture_encode(struct fixture *x){
 FLAC__StreamEncoder *e=FLAC__stream_encoder_new();assert(e);FLAC__int32 pcm[FIXTURE_FRAMES*2];
 for(size_t i=0;i<FIXTURE_FRAMES;i++)for(unsigned c=0;c<2;c++)pcm[i*2+c]=fixture_sample(i,c);
 assert(FLAC__stream_encoder_set_channels(e,2));assert(FLAC__stream_encoder_set_bits_per_sample(e,16));
 assert(FLAC__stream_encoder_set_sample_rate(e,44100));assert(FLAC__stream_encoder_set_blocksize(e,256));
 assert(FLAC__stream_encoder_set_total_samples_estimate(e,FIXTURE_FRAMES));assert(FLAC__stream_encoder_set_verify(e,1));
 assert(FLAC__stream_encoder_init_stream(e,fixture_write,fixture_seek,fixture_tell,NULL,x)==FLAC__STREAM_ENCODER_INIT_STATUS_OK);
 assert(FLAC__stream_encoder_process_interleaved(e,pcm,FIXTURE_FRAMES));assert(FLAC__stream_encoder_finish(e));FLAC__stream_encoder_delete(e);x->pos=0;
}
static FLAC__StreamDecoderReadStatus fixture_read(const FLAC__StreamDecoder *d,FLAC__byte b[],size_t *n,void *v){
 (void)d;struct fixture *x=v;if(x->pos==x->len){*n=0;return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;}
 if(*n>x->len-x->pos)*n=x->len-x->pos;memcpy(b,x->data+x->pos,*n);x->pos+=*n;return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}
static FLAC__StreamDecoderWriteStatus fixture_pcm(const FLAC__StreamDecoder *d,const FLAC__Frame *f,const FLAC__int32 *const pcm[],void *v){
 (void)d;struct fixture *x=v;assert(f->header.channels==2&&f->header.bits_per_sample==16);assert(x->frames+f->header.blocksize<=FIXTURE_FRAMES);
 for(unsigned i=0;i<f->header.blocksize;i++)for(unsigned c=0;c<2;c++)assert(pcm[c][i]==fixture_sample(x->frames+i,c));x->frames+=f->header.blocksize;return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void fixture_meta(const FLAC__StreamDecoder *d,const FLAC__StreamMetadata *m,void *v){
 (void)d;struct fixture *x=v;x->metadata++;if(m->type==FLAC__METADATA_TYPE_STREAMINFO)for(unsigned i=0;i<16;i++)x->md5_nonzero|=m->data.stream_info.md5sum[i]!=0;
}
static void fixture_error(const FLAC__StreamDecoder *d,FLAC__StreamDecoderErrorStatus s,void *v){(void)d;(void)s;((struct fixture *)v)->errors++;}
static FLAC__StreamDecoder *fixture_decoder(struct fixture *x){
 FLAC__StreamDecoder *d=FLAC__stream_decoder_new();assert(d);assert(FLAC__stream_decoder_set_md5_checking(d,1));
 assert(FLAC__stream_decoder_init_stream(d,fixture_read,NULL,NULL,NULL,NULL,fixture_pcm,fixture_meta,fixture_error,x)==FLAC__STREAM_DECODER_INIT_STATUS_OK);return d;
}
#endif
