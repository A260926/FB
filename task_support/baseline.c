#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"
#include <stdio.h>
#include <stdlib.h>
static int wr(int64_t off,const void*p,size_t n,void*t){FILE*f=t;return fseek(f,(long)off,SEEK_SET)||fwrite(p,1,n,f)!=n;}
static int rd(int64_t off,void*p,size_t n,void*t){FILE*f=t;return fseek(f,(long)off,SEEK_SET)||fread(p,1,n,f)!=n;}
int main(void){
 FILE*f=tmpfile();if(!f)return 2;
 MP4E_mux_t*m=MP4E_open(0,0,f,wr);if(!m)return 3;
 mp4_h26x_writer_t h;if(mp4_h26x_write_init(&h,m,64,64,1))return 4;
 /* Packaging-only NAL fixture: parameter sets, two IDR slices, suffix SEI. */
 const unsigned char n[]={0,0,1,64,1,128,0,0,1,66,1,128,0,0,1,68,1,128,0,0,1,38,1,128,0,0,1,38,1,0,0,0,1,80,1,128};
 int rc=mp4_h26x_write_nal(&h,n,sizeof(n),3003);mp4_h26x_write_close(&h);if(rc||MP4E_close(m))return 5;
 long size=ftell(f);MP4D_demux_t d;if(!MP4D_open(&d,rd,f,size))return 6;
 printf("HEVC legacy samples=%u (one picture supplied), duration=%u\n",d.track[0].sample_count,d.track[0].duration_lo);
 if(d.track[0].sample_count!=3)return 7;
 MP4D_close(&d);fclose(f);return 0;
}
