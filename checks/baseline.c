#include <FLAC/stream_encoder.h>
#include <FLAC/stream_decoder.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct { FILE *f[2]; int part, meta, frames; } Ctx;
static FLAC__StreamDecoderReadStatus rd(const FLAC__StreamDecoder*d, FLAC__byte*b,size_t*n,void*v) {
 Ctx*c=v; (void)d; *n=fread(b,1,*n,c->f[c->part]);
 if(*n) return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
 if(c->part==0){c->part=1;return FLAC__STREAM_DECODER_READ_STATUS_END_OF_LINK;}
 return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
}
static FLAC__StreamDecoderWriteStatus wr(const FLAC__StreamDecoder*d,const FLAC__Frame*f,const FLAC__int32*const b[],void*v){
 Ctx*c=v; (void)d; assert(f->header.channels==(unsigned)c->meta); assert(f->header.blocksize==32);
 for(unsigned i=0;i<32;i++)for(unsigned ch=0;ch<f->header.channels;ch++)assert(b[ch][i]==(int)i*3+(int)ch);
 c->frames++;return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void md(const FLAC__StreamDecoder*d,const FLAC__StreamMetadata*m,void*v){(void)d;if(m->type==FLAC__METADATA_TYPE_STREAMINFO)((Ctx*)v)->meta++;}
static void er(const FLAC__StreamDecoder*d,FLAC__StreamDecoderErrorStatus e,void*v){(void)d;(void)e;(void)v;abort();}
int main(void){
 Ctx c={0}; FLAC__int32 pcm[64];
 for(int part=0;part<2;part++){
  char name[32]; snprintf(name,sizeof(name),"part%d.flac",part);
  FLAC__StreamEncoder*e=FLAC__stream_encoder_new();assert(e);
  assert(FLAC__stream_encoder_set_channels(e,part+1));assert(FLAC__stream_encoder_set_bits_per_sample(e,16));
  assert(FLAC__stream_encoder_set_sample_rate(e,part?48000:44100));
  assert(FLAC__stream_encoder_init_file(e,name,NULL,NULL)==FLAC__STREAM_ENCODER_INIT_STATUS_OK);
  for(int i=0;i<32;i++)for(int ch=0;ch<=part;ch++)pcm[i*(part+1)+ch]=i*3+ch;
  assert(FLAC__stream_encoder_process_interleaved(e,pcm,32));assert(FLAC__stream_encoder_finish(e));FLAC__stream_encoder_delete(e);
  c.f[part]=fopen(name,"rb");assert(c.f[part]);
 }
 FLAC__StreamDecoder*d=FLAC__stream_decoder_new();assert(d);assert(FLAC__stream_decoder_set_md5_checking(d,1));
 assert(!FLAC__stream_decoder_get_decode_chained_stream(d));
 int accepted=FLAC__stream_decoder_set_decode_chained_stream(d,1);assert(accepted);
 assert(FLAC__stream_decoder_get_decode_chained_stream(d));
 assert(FLAC__stream_decoder_init_stream(d,rd,NULL,NULL,NULL,NULL,wr,md,er,&c)==FLAC__STREAM_DECODER_INIT_STATUS_OK);
 assert(!FLAC__stream_decoder_set_decode_chained_stream(d,0));
 assert(FLAC__stream_decoder_process_until_end_of_link(d));assert(FLAC__stream_decoder_get_state(d)==FLAC__STREAM_DECODER_END_OF_LINK);
 assert(c.meta==1&&c.frames==1);assert(FLAC__stream_decoder_finish_link(d));
 assert(FLAC__stream_decoder_process_until_end_of_stream(d));assert(c.meta==2&&c.frames==2);assert(FLAC__stream_decoder_finish(d));
 FLAC__stream_decoder_delete(d);fclose(c.f[0]);fclose(c.f[1]);puts("BASELINE native_links=2 pcm_exact=1 md5_ok=1 chained_flag_accepted=1");return 0;
}
