#ifndef WAVE_FIXTURE_H
#define WAVE_FIXTURE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
static void wf16(unsigned char *p,unsigned x){p[0]=x;p[1]=x>>8;}
static void wf32(unsigned char *p,unsigned x){wf16(p,x);wf16(p+2,x>>16);}
static size_t wave_pcm(unsigned char *p,unsigned frames,unsigned rate){size_t z=44+(size_t)frames*4;memset(p,0,z);memcpy(p,"RIFF",4);wf32(p+4,z-8);memcpy(p+8,"WAVEfmt ",8);wf32(p+16,16);wf16(p+20,1);wf16(p+22,2);wf32(p+24,rate);wf32(p+28,rate*4);wf16(p+32,4);wf16(p+34,16);memcpy(p+36,"data",4);wf32(p+40,frames*4);for(unsigned i=0;i<frames;i++){wf16(p+44+i*4,(unsigned)(int16_t)((i*367)%24000-12000));wf16(p+46+i*4,(unsigned)(int16_t)((i*719)%20000-10000));}return z;}
static size_t wave_adpcm(unsigned char *p,unsigned blocks,unsigned rate){static const int coef[7][2]={{256,0},{512,-256},{0,0},{192,64},{240,0},{460,-208},{392,-232}};size_t z=90+blocks*32;memset(p,0,z);memcpy(p,"RIFF",4);wf32(p+4,z-8);memcpy(p+8,"WAVEfmt ",8);wf32(p+16,50);wf16(p+20,2);wf16(p+22,2);wf32(p+24,rate);wf32(p+28,rate*32/20);wf16(p+32,32);wf16(p+34,4);wf16(p+36,32);wf16(p+38,20);wf16(p+40,7);for(int i=0;i<7;i++){wf16(p+42+i*4,coef[i][0]);wf16(p+44+i*4,coef[i][1]);}memcpy(p+70,"fact",4);wf32(p+74,4);wf32(p+78,blocks*20);memcpy(p+82,"data",4);wf32(p+86,blocks*32);for(unsigned k=0;k<blocks;k++){unsigned char *v=p+90+k*32;v[0]=0;v[1]=0;wf16(v+2,64+k);wf16(v+4,80+k);wf16(v+6,1000+k*31);wf16(v+8,-900-k*27);wf16(v+10,500+k*13);wf16(v+12,-400-k*11);for(unsigned i=14;i<32;i++)v[i]=(unsigned char)((i+k)%16*16+(15-i+k)%16);}return z;}
#endif
