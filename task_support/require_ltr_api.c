#include "kvazaar.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"required LTR consumer failed: %s\n",#x); return 1; } } while (0)
/* A small public contract consumer, not the required full LTR test suite. */
int main(void) {
 const kvz_api *a=kvz_api_get(8); REQUIRE(a);
 kvz_config *c=a->config_alloc(); REQUIRE(c && a->config_init(c));
 REQUIRE(c->ltr_slots==0);
 REQUIRE(a->config_parse(c,"preset","ultrafast"));
 REQUIRE(a->config_parse(c,"ltr-slots","2") && c->ltr_slots==2);
 c->width=64;c->height=64;c->gop_len=0;c->intra_period=0;c->ref_frames=1;
 c->bipred=0;c->tmvp_enable=0;c->owf=0;c->threads=2;c->wpp=1;c->cpuid=0;
 c->enable_logging_output=0;
 kvz_encoder *e=a->encoder_open(c); REQUIRE(e);
 int done=0;
 for(int n=0;n<6;n++) {
  kvz_picture *p=NULL;
  if(n<2) {
   p=a->picture_alloc(64,64); REQUIRE(p);
   REQUIRE(p->ltr_mark==-1 && p->ltr_use==-1);
   p->pts=n;p->ltr_mark=n==0?0:-1;p->ltr_use=n==1?0:-1;
   for(int y=0;y<64;y++)for(int x=0;x<64;x++)p->y[y*p->stride+x]=(uint8_t)(x*3+y*7+n*11);
   memset(p->u,96,1024);memset(p->v,160,1024);
  }
  kvz_data_chunk *data=NULL;uint32_t len=0;kvz_picture *rec=NULL,*src=NULL;kvz_frame_info info;
  REQUIRE(a->encoder_encode(e,p,&data,&len,&rec,&src,&info));a->picture_free(p);
  if(rec) {
   REQUIRE(src && data && len>0 && info.poc==done && src->pts==done);
   if(done==1) REQUIRE(info.ref_list_len[0]==1 && info.ref_list[0][0]==0);
   done++;
  } else REQUIRE(!src && !data && !len);
  a->picture_free(src);a->picture_free(rec);a->chunk_free(data);
 }
 REQUIRE(done==2);a->encoder_close(e);a->config_destroy(c);
 puts("REQUIRED_LTR_CONSUMER_OK frames=2 mark=0 use=0");return 0;
}
