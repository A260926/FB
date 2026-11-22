#include "fixture.h"
#include <vorbis/vorbisenc.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
static int append(fixture *f,const void *p,size_t n){
 if(f->size+n>f->capacity){size_t cap=(f->size+n)*2+4096;void *v=realloc(f->data,cap);if(!v)return -1;f->data=v;f->capacity=cap;}
 memcpy(f->data+f->size,p,n);f->size+=n;return 0;
}
static int page(fixture *f,ogg_page *p){return append(f,p->header,p->header_len)||append(f,p->body,p->body_len);}
int fixture_encode(fixture *f,int ch,int rate,int frames){
 vorbis_info vi;vorbis_comment vc;vorbis_dsp_state vd;vorbis_block vb;ogg_stream_state os;ogg_packet a,b,c,op;ogg_page pg;
 memset(f,0,sizeof(*f));f->short_read=61;
 vorbis_info_init(&vi);if(vorbis_encode_init_vbr(&vi,ch,rate,0.2f))return -1;
 vorbis_comment_init(&vc);if(vorbis_analysis_init(&vd,&vi))return -1;
 vorbis_block_init(&vd,&vb);ogg_stream_init(&os,917+ch+rate);
 vorbis_analysis_headerout(&vd,&vc,&a,&b,&c);ogg_stream_packetin(&os,&a);ogg_stream_packetin(&os,&b);ogg_stream_packetin(&os,&c);
 while(ogg_stream_flush(&os,&pg))if(page(f,&pg))return -1;
 for(int pos=0;pos<frames;){
  int n=frames-pos;if(n>257)n=257;float **pcm=vorbis_analysis_buffer(&vd,n);
  for(int i=0;i<n;i++)for(int j=0;j<ch;j++)pcm[j][i]=0.35f*sinf((pos+i)*(0.017f+j*0.013f))+(((pos+i)%997==0)?0.25f:0);
  vorbis_analysis_wrote(&vd,n);pos+=n;
  while(vorbis_analysis_blockout(&vd,&vb)==1){vorbis_analysis(&vb,NULL);vorbis_bitrate_addblock(&vb);
   while(vorbis_bitrate_flushpacket(&vd,&op)){ogg_stream_packetin(&os,&op);while(ogg_stream_pageout(&os,&pg))if(page(f,&pg))return -1;}}
 }
 vorbis_analysis_wrote(&vd,0);
 while(vorbis_analysis_blockout(&vd,&vb)==1){vorbis_analysis(&vb,NULL);vorbis_bitrate_addblock(&vb);
  while(vorbis_bitrate_flushpacket(&vd,&op)){ogg_stream_packetin(&os,&op);while(ogg_stream_pageout(&os,&pg))if(page(f,&pg))return -1;}}
 while(ogg_stream_flush(&os,&pg))if(page(f,&pg))return -1;
 ogg_stream_clear(&os);vorbis_block_clear(&vb);vorbis_dsp_clear(&vd);vorbis_comment_clear(&vc);vorbis_info_clear(&vi);return 0;
}
static size_t rd(void *p,size_t size,size_t n,void *u){fixture *f=u;size_t bytes=size*n;errno=0;f->reads++;
 if(!size)return 0;if(bytes>f->size-f->pos)bytes=f->size-f->pos;if(f->short_read&&bytes>f->short_read)bytes=f->short_read;
 bytes-=bytes%size;memcpy(p,f->data+f->pos,bytes);f->pos+=bytes;return bytes/size;}
static int sk(void *u,ogg_int64_t off,int whence){fixture *f=u;ogg_int64_t p=off+(whence==SEEK_CUR?(ogg_int64_t)f->pos:whence==SEEK_END?(ogg_int64_t)f->size:0);if(p<0||(size_t)p>f->size)return -1;f->pos=(size_t)p;return 0;}
static long tl(void *u){return (long)((fixture*)u)->pos;}
int fixture_open(fixture *f,OggVorbis_File *vf,int seekable){ov_callbacks cb={rd,seekable?sk:NULL,NULL,seekable?tl:NULL};f->pos=0;f->reads=0;return ov_open_callbacks(f,vf,NULL,0,cb);}
void fixture_free(fixture *f){free(f->data);memset(f,0,sizeof(*f));}
