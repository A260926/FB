#include "kvazaar.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define W 192
#define H 136
#define N 18
#define CHECK(x) do {if(!(x)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static const kvz_api *api;
static void option(kvz_config*c,const char*n,const char*v){CHECK(api->config_parse(c,n,v));}
static void run(int mode){
 kvz_config*c=api->config_alloc();CHECK(c&&api->config_init(c));
 c->width=W;c->height=H;option(c,"preset","ultrafast");option(c,"gop","0");
 option(c,"threads","1");option(c,"owf","0");option(c,"wpp","0");
 option(c,"bitrate","180000");option(c,"rc-algorithm","lambda");
 kvz_encoder*e=api->encoder_open(c);CHECK(e);char path[64];snprintf(path,sizeof path,"devcheck/out/mode%d.hevc",mode);FILE*f=fopen(path,"wb");CHECK(f);
 unsigned frames=0;uint64_t bytes=0,sum=0;
 for(int n=0;n<N+8;n++){
  kvz_picture*in=NULL,*rec=NULL,*src=NULL;kvz_data_chunk*chunks=NULL;uint32_t len=0;
  if(n<N){in=api->picture_alloc(W,H);CHECK(in);in->pts=n;
   for(int y=0;y<H;y++)for(int x=0;x<W;x++)in->y[y*in->stride+x]=(uint8_t)(16+((x*13+y*7+n*19+(x*y%59))%220));
   for(int y=0;y<H/2;y++)for(int x=0;x<W/2;x++){in->u[y*in->stride/2+x]=96+(n%16);in->v[y*in->stride/2+x]=144;}
   if(mode){in->roi.width=3;in->roi.height=3;in->roi.roi_array=calloc(9,1);CHECK(in->roi.roi_array);if(mode==2)for(int i=0;i<9;i++)in->roi.roi_array[i]=(i%3==0?-6:3);}
  }
  CHECK(api->encoder_encode(e,in,&chunks,&len,&rec,&src,NULL));
  for(kvz_data_chunk*t=chunks;t;t=t->next)CHECK(fwrite(t->data,1,t->len,f)==t->len);
  bytes+=len;if(rec){frames++;for(int y=0;y<H;y++)for(int x=0;x<W;x++)sum+=rec->y[y*rec->stride+x];}
  api->chunk_free(chunks);api->picture_free(rec);api->picture_free(src);api->picture_free(in);
  if(n>=N&&!len)break;
 }
 CHECK(!fclose(f));CHECK(frames==N&&bytes>100&&sum>0);
 printf("BASELINE mode=%d frames=%u bytes=%llu reconstruction_sum=%llu PASS\n",mode,frames,(unsigned long long)bytes,(unsigned long long)sum);
 api->encoder_close(e);api->config_destroy(c);
}
int main(void){api=kvz_api_get(8);CHECK(api);for(int m=0;m<3;m++)run(m);return 0;}
