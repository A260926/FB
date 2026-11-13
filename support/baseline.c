#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define PL_MPEG_IMPLEMENTATION
#include "../pl_mpeg.h"
static unsigned long long hash(plm_frame_t *f) {
 unsigned long long h=1469598103934665603ULL;
 plm_plane_t p[3]={f->y,f->cb,f->cr};
 for(int c=0;c<3;c++)for(unsigned y=0;y<p[c].height;y++)for(unsigned x=0;x<p[c].width;x++) {h^=p[c].data[y*p[c].width+x];h*=1099511628211ULL;}
 return h;
}
int main(int argc,char **argv) {
 assert(argc==3);size_t expected=(size_t)atoi(argv[2]);assert(expected>0&&expected<=38);FILE *fp=fopen(argv[1],"rb");assert(fp);assert(!fseek(fp,0,SEEK_END));long n=ftell(fp);assert(n>0);rewind(fp);
 unsigned char *b=malloc((size_t)n);assert(b);assert(fread(b,1,(size_t)n,fp)==(size_t)n);fclose(fp);
 size_t pics=0,bs=0,open=0;
 for(long i=0;i+8<n;i++)if(!b[i]&&!b[i+1]&&b[i+2]==1) {
  if(b[i+3]==0){pics++;bs+=((b[i+5]>>3)&7)==3;}
  if(b[i+3]==0xb8)open+=!(b[i+7]&64);
 }
 assert(pics==expected);
 plm_buffer_t *buf=plm_buffer_create_with_filename(argv[1]);plm_video_t *v=plm_video_create_with_buffer(buf,1);assert(v);
 unsigned long long hashes[38];
 for(int pass=0;pass<2;pass++){
  size_t k=0;plm_frame_t *f;
  while((f=plm_video_decode(v))){assert(k<expected);assert(f->width==48&&f->height==32);assert(fabs(f->time-k/25.0)<1e-10);unsigned long long h=hash(f);if(pass)assert(h==hashes[k]);else hashes[k]=h;k++;}
  fprintf(stderr,"baseline decoded=%zu headers=%zu pass=%d\n",k,pics,pass);assert(k==expected);for(int t=0;t<3;t++)assert(!plm_video_decode(v));
  if(!pass)plm_video_rewind(v);
 }
 size_t changed=0;for(int k=1;k<expected;k++)changed+=hashes[k]!=hashes[k-1];assert(changed>=30);
 printf("FORWARD_OK file=%s frames=%zu b_pictures=%zu open_gops=%zu changed=%zu rewind=%zu\n",argv[1],expected,bs,open,changed,expected);
 plm_video_destroy(v);free(b);return 0;
}
