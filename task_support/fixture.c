#include "minimp4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { unsigned char *p; size_t n; } buf;
static int wr(int64_t off,const void *p,size_t n,void *v){buf*b=v;if(off<0||(uint64_t)off+n>10000000)return 1;if((size_t)off+n>b->n){void*q=realloc(b->p,(size_t)off+n);if(!q)return 1;b->p=q;b->n=(size_t)off+n;}memcpy(b->p+off,p,n);return 0;}
static int rd(int64_t off,void*p,size_t n,void*v){buf*b=v;if(off<0||(uint64_t)off+n>b->n)return 1;memcpy(p,b->p+off,n);return 0;}
static const unsigned char video[]={0,0,0,2,0x65,0x80},audio[]={0x21,0x10};
static int make(buf *b,int fragmented){
 MP4E_track_t t; MP4E_mux_t*m=MP4E_open(0,fragmented,b,wr);int v,a,i;
 const unsigned char sps[]={0x67,0x42,0,0x1e,0xe9,1,0x40,0x7b,0x20},pps[]={0x68,0xce,6,0xe2},asc[]={0x11,0x90};
 if(!m)return 1;memset(&t,0,sizeof t);memcpy(t.language,"und",4);t.track_media_kind=e_video;t.time_scale=90000;t.default_duration=3000;t.object_type_indication=MP4_OBJECT_TYPE_AVC;t.u.v.width=16;t.u.v.height=16;
 v=MP4E_add_track(m,&t);if(v<0||MP4E_set_sps(m,v,sps,sizeof sps)||MP4E_set_pps(m,v,pps,sizeof pps))return 2;
 memset(&t,0,sizeof t);memcpy(t.language,"und",4);t.track_media_kind=e_audio;t.time_scale=48000;t.default_duration=1024;t.object_type_indication=MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3;t.u.a.channelcount=2;
 a=MP4E_add_track(m,&t);if(a<0||MP4E_set_dsi(m,a,asc,sizeof asc))return 3;
 for(i=0;i<2;i++)if(MP4E_put_sample(m,v,video,sizeof video,3000,MP4E_SAMPLE_RANDOM_ACCESS)||MP4E_put_sample(m,a,audio,sizeof audio,1024,MP4E_SAMPLE_DEFAULT))return 4;
 return MP4E_close(m)!=0;
}
int main(int argc,char**argv){
 buf n={0},f={0};MP4D_demux_t d;unsigned t,s,bytes,dts,pts,dur,count=0;int sync;FILE*out;
 if(make(&n,0)||make(&f,1))return 10;
 if(argc==2&&strcmp(argv[1],"emit")==0){int ok=fwrite(f.p,1,f.n,stdout)==f.n;free(n.p);free(f.p);return !ok;}
 if(argc!=3)return 11;
 if(!MP4D_open(&d,rd,&n,n.n)||d.track_count!=2)return 12;
 for(t=0;t<2;t++)for(s=0;s<d.track[t].sample_count;s++){
  MP4D_file_offset_t off=MP4D_frame_offset(&d,t,s,&bytes,&dts,&pts,&dur,&sync);
  const unsigned char*p=t?audio:video;size_t len=t?sizeof audio:sizeof video;
  if(bytes!=len||(uint64_t)off+bytes>n.n||memcmp(n.p+off,p,len)||dur!=(t?1024:3000))return 13;count++;
 }
 MP4D_close(&d);if(count!=4)return 14;
 printf("NORMAL tracks=2 samples=%u payloads=verified\n",count);
 if(!MP4D_open(&d,rd,&f,f.n))return 15;
 count=0;for(t=0;t<d.track_count;t++)count+=d.track[t].sample_count;
 printf("FRAGMENT tracks=%u indexed_samples=%u\n",d.track_count,count);MP4D_close(&d);
 out=fopen(argv[1],"wb");if(!out||fwrite(f.p,1,f.n,out)!=f.n||fclose(out))return 16;
 out=fopen(argv[2],"wb");if(!out||fwrite(n.p,1,n.n,out)!=n.n||fclose(out))return 17;
 printf("BASELINE production_mux_demux=OK\n");free(n.p);free(f.p);return 0;
}
