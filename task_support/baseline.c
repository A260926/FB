#include "xmp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"baseline line %d failed\n",__LINE__);return 1;} } while(0)
static void fill_samples(xmp_context ctx) {
 struct xmp_module_info mi;xmp_get_module_info(ctx,&mi);
 for(int k=0;k<mi.mod->smp;k++) {
  struct xmp_sample *sample=&mi.mod->xxs[k];if(!sample->data || sample->len<=0)continue;
  size_t n=(size_t)sample->len*((sample->flg & XMP_SAMPLE_STEREO)?2:1);
  for(size_t i=0;i<n;i++){int wave=(int)(i%64)-32;if(sample->flg & XMP_SAMPLE_16BIT)((int16_t*)sample->data)[i]=(int16_t)(wave*256);else ((int8_t*)sample->data)[i]=(int8_t)wave;}
 }
}
static int start_tone(xmp_context ctx) {
 struct xmp_module_info mi;struct xmp_event event;memset(&event,0,sizeof(event));xmp_get_module_info(ctx,&mi);
 for(int i=0;i<mi.mod->ins;i++)if(mi.mod->xxi[i].nsm>0){int sid=mi.mod->xxi[i].sub[0].sid;if(sid>=0 && sid<mi.mod->smp && mi.mod->xxs[sid].len>0){event.note=60;event.ins=i+1;event.vol=65;xmp_inject_event(ctx,0,&event);return 0;}}
 return 1;
}
static int check(const char *path, int interp) {
 xmp_context a=xmp_create_context(),b=xmp_create_context();struct xmp_frame_info fi;
 const size_t bytes=32768;unsigned char *expected=malloc(bytes),*actual=malloc(bytes);size_t filled=0;
 REQUIRE(a && b && expected && actual); REQUIRE(xmp_load_module(a,path)==0); REQUIRE(xmp_load_module(b,path)==0);
 fill_samples(a);fill_samples(b);
 REQUIRE(xmp_start_player(a,44100,0)==0); REQUIRE(xmp_start_player(b,44100,0)==0);
 REQUIRE(xmp_set_player(a,XMP_PLAYER_INTERP,interp)==0); REQUIRE(xmp_set_player(b,XMP_PLAYER_INTERP,interp)==0);
 REQUIRE(start_tone(a)==0); REQUIRE(start_tone(b)==0);
 while(filled<bytes){REQUIRE(xmp_play_frame(a)==0);xmp_get_frame_info(a,&fi);REQUIRE(fi.buffer_size>0);size_t n=(size_t)fi.buffer_size;if(n>bytes-filled)n=bytes-filled;memcpy(expected+filled,fi.buffer,n);filled+=n;}
 filled=0;size_t turn=0;const size_t frames[]={1,7,113};
 while(filled<bytes){size_t n=4*frames[turn++%3];if(n>bytes-filled)n=bytes-filled;REQUIRE(xmp_play_buffer(b,actual+filled,(int)n,0)==0);filled+=n;}
 REQUIRE(memcmp(expected,actual,bytes)==0);size_t nonzero=0;uint64_t hash=UINT64_C(1469598103934665603);
 for(size_t i=0;i<bytes;i++){nonzero+=expected[i]!=0;hash^=expected[i];hash*=UINT64_C(1099511628211);} REQUIRE(nonzero>100);
 printf("LEGACY_PCM_OK file=%s interpolation=%d bytes=%zu nonzero=%zu hash=%016llx split=equal\n",path,interp,bytes,nonzero,(unsigned long long)hash);
 xmp_end_player(a);xmp_end_player(b);xmp_release_module(a);xmp_release_module(b);xmp_free_context(a);xmp_free_context(b);free(expected);free(actual);return 0;
}
int main(void){const char *files[]={"test-dev/data/test.xm","test-dev/data/it_sample_porta.it"};for(size_t f=0;f<2;f++)for(int mode=0;mode<3;mode++)if(check(files[f],mode))return 1;puts("LIBXMP_NATIVE_BASELINE_OK");return 0;}
