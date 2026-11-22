#include "kvazaar.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"baseline failed line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void) {
 const kvz_api *a=kvz_api_get(8); REQUIRE(a);
 kvz_config *c=a->config_alloc(); REQUIRE(c && a->config_init(c));
 REQUIRE(a->config_parse(c,"preset","ultrafast"));
 c->width=64;c->height=64;c->gop_len=0;c->intra_period=0;c->ref_frames=1;
 c->bipred=0;c->tmvp_enable=0;c->owf=2;c->threads=2;c->wpp=1;c->cpuid=0;c->qp=28;
 c->enable_logging_output=0;
 int option=a->config_parse(c,"ltr-slots","0");
 kvz_encoder *e=a->encoder_open(c); REQUIRE(e);
 int done=0, last_old=0; uint64_t bytes=0,checksum=0;
 for(int n=0;n<52;n++) {
  kvz_picture *p=NULL;
  if(n<48) {
   p=a->picture_alloc(64,64); REQUIRE(p);p->pts=n;
   for(int y=0;y<64;y++)for(int x=0;x<64;x++)p->y[y*p->stride+x]=(uint8_t)((x*7+y*5+(n%5)*19)^((x+n)%17));
   memset(p->u,96,1024);memset(p->v,160,1024);
  }
  kvz_data_chunk *data=NULL;uint32_t len=0;kvz_picture *rec=NULL,*src=NULL;kvz_frame_info info;
  REQUIRE(a->encoder_encode(e,p,&data,&len,&rec,&src,&info));a->picture_free(p);
  if(rec) {
   REQUIRE(src && len>0 && data && info.poc==done && src->pts==done && rec->pts==done);
   if(done) {REQUIRE(info.ref_list_len[0]==1 && info.ref_list[0][0]==done-1); if(done>16 && info.ref_list[0][0]!=0)last_old++;}
   uint32_t counted=0;for(kvz_data_chunk *q=data;q;q=q->next) counted+=q->len;REQUIRE(counted==len);
   for(int y=0;y<64;y++)for(int x=0;x<64;x++)checksum=checksum*33+rec->y[y*rec->stride+x];
   bytes+=len;done++;
  } else REQUIRE(!data && !len && !src);
  a->picture_free(src);a->picture_free(rec);a->chunk_free(data);
 }
 REQUIRE(done==48 && bytes>100 && last_old>20 && checksum);
 printf("BASELINE frames=%d bytes=%llu checksum=%016llx latest_only=%d LTR_OPTION=%d\n",done,(unsigned long long)bytes,(unsigned long long)checksum,last_old,option);
 a->encoder_close(e);a->config_destroy(c);return 0;
}
