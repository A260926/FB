#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "minih264e.h"
#include "h264bsd_decoder.h"
static union { long double align; unsigned char data[64*96*3/2]; } input_store;
#define image input_store.data
static unsigned char correct[64*96*3/2], damaged[64*96*3/2];
static void decode(storage_t *d, unsigned char *bytes, int n, int frame, unsigned char *dest) {
 unsigned offset=0,used=0,got=0,stalls=0;
 while(offset<(unsigned)n) {
  unsigned status=h264bsdDecode(d,bytes+offset,n-offset,frame,&used);
  assert(status<=H264BSD_HDRS_RDY);offset+=used;
  assert(used||++stalls<8);
  if(status==H264BSD_PIC_RDY) {
   unsigned id,key,errors;unsigned char *p=h264bsdNextOutputPicture(d,&id,&key,&errors);
   assert(p&&id==(unsigned)frame&&errors==0);memcpy(dest,p,sizeof(image));got++;
  }
 }
 assert(got==1);
}
int main(void) {
 H264E_create_param_t p={0};H264E_run_param_t r={0};H264E_io_yuv_t y;int a,b;
 p.width=64;p.height=96;p.num_layers=1;p.enableNEON=1;
 assert(H264E_sizeof(&p,&a,&b)==0);
 void *enc=malloc(a),*scratch=malloc(b);assert(enc&&scratch&&H264E_init(enc,&p)==0);
 y.yuv[0]=image;y.yuv[1]=image+64*96;y.yuv[2]=image+64*96*5/4;
 y.stride[0]=64;y.stride[1]=y.stride[2]=32;
 r.qp_min=r.qp_max=30;r.encode_speed=5;
 storage_t d0,d1;assert(h264bsdInit(&d0,1)==0&&h264bsdInit(&d1,1)==0);
 int drift=0;
 for(int f=0;f<5;f++) {
  /* Identical source frames deliberately exercise ordinary P/skip prediction. */
  for(int i=0;i<(int)sizeof(image);i++)image[i]=(unsigned char)((i*17+i/64*29)&255);
  unsigned char *out;int n;int status=H264E_encode(enc,scratch,&r,&y,&out,&n); if(status||n<=0)fprintf(stderr,"encode frame=%d status=%d n=%d\n",f,status,n);assert(status==0&&n>0);
  unsigned char *copy=malloc(n);assert(copy);memcpy(copy,out,n);decode(&d0,copy,n,f,correct);
  memcpy(copy,out,n);decode(&d1,copy,n,f,damaged);free(copy);
  assert(!memcmp(image,correct,sizeof(image)));
  if(f)drift+=memcmp(correct,damaged,sizeof(image))!=0;
  else {
   int refs=0;
   for(unsigned k=0;k<=d1.dpb->dpbSize;k++) {
    dpbPicture_t *pic=d1.dpb->buffer+k;
    if(pic->status==SHORT_TERM||pic->status==LONG_TERM) {
     for(size_t i=0;i<sizeof(image);i++)pic->data[i]^=127;
     refs++;
    }
   }
   assert(refs==1);
  }
 }
 assert(drift==4);h264bsdShutdown(&d0);h264bsdShutdown(&d1);free(enc);free(scratch);
 puts("REFRESH_BASELINE frames=5 clean_decode=exact concealment=0 drift_after_damage=4");
}
