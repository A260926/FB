#include <nghttp3/nghttp3.h>
#include "nghttp3_qpack.h"
#include "nghttp3_buf.h"
#include <stdio.h>
#include <string.h>
struct section { nghttp3_buf p,r; };
static int feedback(nghttp3_qpack_decoder*d,nghttp3_qpack_encoder*e){
 uint8_t data[4096];nghttp3_buf b;size_t n,i;nghttp3_buf_wrap_init(&b,data,sizeof(data));
 n=nghttp3_qpack_decoder_get_decoder_streamlen2(d);if(n>sizeof(data))return 1;
 nghttp3_qpack_decoder_write_decoder(d,&b);
 for(i=0;i<nghttp3_buf_len(&b);i++)if(nghttp3_qpack_encoder_read_decoder(e,data+i,1)!=1)return 2;
 return 0;
}
int main(void){
 const nghttp3_mem*m=nghttp3_mem_default();nghttp3_qpack_encoder *e=NULL;nghttp3_qpack_decoder*d=NULL;
 nghttp3_qpack_stream_context *s0=NULL,*s4=NULL;nghttp3_buf eb;struct section s[3];int i,emits=0,final=0;size_t off;uint8_t flags;nghttp3_qpack_nv nv;nghttp3_ssize n;
 const char *vals[]={"foo","bar","foo"};
 if(nghttp3_qpack_encoder_new2(&e,4096,7,m)||nghttp3_qpack_decoder_new(&d,4096,4,m))return 1;
 nghttp3_qpack_encoder_set_max_dtable_capacity(e,4096);nghttp3_qpack_encoder_set_max_blocked_streams(e,4);nghttp3_buf_init(&eb);
 for(i=0;i<3;i++){
  nghttp3_nv field={(uint8_t*)"link",(uint8_t*)vals[i],4,3,NGHTTP3_NV_FLAG_NONE};nghttp3_buf_init(&s[i].p);nghttp3_buf_init(&s[i].r);
  if(nghttp3_qpack_encoder_encode(e,&s[i].p,&s[i].r,&eb,i==2?4:0,&field,1))return 2;
 }
 if(!nghttp3_qpack_encoder_find_stream(e,0)||nghttp3_ringbuf_len(&nghttp3_qpack_encoder_find_stream(e,0)->refs)!=2)return 3;
 if(nghttp3_qpack_stream_context_new(&s0,0,m)||nghttp3_qpack_stream_context_new(&s4,4,m))return 4;
 n=nghttp3_qpack_decoder_read_request(d,s0,&nv,&flags,s[0].p.pos,nghttp3_buf_len(&s[0].p),0);
 if(n<0||!(flags&NGHTTP3_QPACK_DECODE_FLAG_BLOCKED))return 5;
 if(nghttp3_qpack_decoder_cancel_stream(d,0)||feedback(d,e))return 6;
 if(nghttp3_qpack_encoder_find_stream(e,0)||e->krcnt!=0)return 7;
 puts("BASELINE canceled_stream=0 refs=0 known_received=0");
 for(off=0;off<nghttp3_buf_len(&eb);off++)if(nghttp3_qpack_decoder_read_encoder(d,eb.pos+off,1)!=1)return 8;
 n=nghttp3_qpack_decoder_read_request(d,s4,&nv,&flags,s[2].p.pos,nghttp3_buf_len(&s[2].p),0);if(n<0)return 9;
 off=0;for(i=0;i<100 && !final;i++){
  size_t avail=nghttp3_buf_len(&s[2].r)-off;
  n=nghttp3_qpack_decoder_read_request(d,s4,&nv,&flags,s[2].r.pos+off,avail,1);if(n<0)return 10;off+=(size_t)n;
  if(flags&NGHTTP3_QPACK_DECODE_FLAG_EMIT){
   nghttp3_vec name=nghttp3_rcbuf_get_buf(nv.name),v=nghttp3_rcbuf_get_buf(nv.value);
   if(name.len!=4||memcmp(name.base,"link",4)||v.len!=3||memcmp(v.base,"foo",3))return 11;
   emits++;nghttp3_rcbuf_decref(nv.name);nghttp3_rcbuf_decref(nv.value);
  }
  final=(flags&NGHTTP3_QPACK_DECODE_FLAG_FINAL)!=0;
 }
 if(!final||emits!=1||feedback(d,e)||nghttp3_qpack_encoder_find_stream(e,4)||e->krcnt==0)return 12;
 printf("BASELINE survivor_stream=4 fields=%d final=%d feedback=one_byte\n",emits,final);
 nghttp3_qpack_stream_context_del(s0);nghttp3_qpack_stream_context_del(s4);
 for(i=0;i<3;i++){nghttp3_buf_free(&s[i].p,m);nghttp3_buf_free(&s[i].r,m);}nghttp3_buf_free(&eb,m);
 nghttp3_qpack_decoder_del(d);nghttp3_qpack_encoder_del(e);return 0;
}
