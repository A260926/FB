#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <math.h>
#include <ogg/ogg.h>
#include <opus.h>
static void put(ogg_stream_state *s,FILE *f,unsigned char *p,long n,int bos,int eos,int64_t gp,int64_t no){
 ogg_packet op={p,n,bos,eos,gp,no};ogg_page pg;
 assert(ogg_stream_packetin(s,&op)==0);
 while(ogg_stream_flush(s,&pg)){assert(fwrite(pg.header,1,pg.header_len,f)==(size_t)pg.header_len);assert(fwrite(pg.body,1,pg.body_len,f)==(size_t)pg.body_len);}
}
int main(int argc,char **argv){
 assert(argc==3);int gain=atoi(argv[2]);FILE *f=fopen(argv[1],"wb");assert(f);
 ogg_stream_state os;assert(!ogg_stream_init(&os,123));
 unsigned char head[19]={ 'O','p','u','s','H','e','a','d',1,1,0x38,1,0x80,0xbb,0,0,0,0,0};
 head[16]=gain&255;head[17]=(gain>>8)&255;
 unsigned char tags[16]={'O','p','u','s','T','a','g','s',0,0,0,0,0,0,0,0};
 put(&os,f,head,sizeof head,1,0,0,0);put(&os,f,tags,sizeof tags,0,0,0,1);
 int err;OpusEncoder *en=opus_encoder_create(48000,1,OPUS_APPLICATION_AUDIO,&err);assert(en&&err==0);
 assert(!opus_encoder_ctl(en,OPUS_SET_BITRATE(64000)));
 for(int j=0;j<6;j++){
  float pcm[960];unsigned char packet[4000];
  for(int i=0;i<960;i++)pcm[i]=0.03f*sinf((j*960+i)*2.0*3.141592653589793*997/48000);
  int n=opus_encode_float(en,pcm,960,packet,sizeof packet);assert(n>0);
  put(&os,f,packet,n,0,j==5,(j+1)*960,j+2);
 }
 opus_encoder_destroy(en);ogg_stream_clear(&os);assert(!fclose(f));return 0;
}
