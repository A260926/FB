// Independent, deliberately small inspector for the normal four-sample support fixture.
#ifndef SIMPLE_TS_CHECK_H
#define SIMPLE_TS_CHECK_H
#include <cstddef>
#include <cstdint>
static uint64_t read_pts(const unsigned char* p){return (uint64_t(p[0]&14)<<29)|(uint64_t(p[1])<<22)|(uint64_t(p[2]&254)<<14)|(uint64_t(p[3])<<7)|(p[4]>>1);}
static uint64_t read_pcr(const unsigned char* p){return ((uint64_t(p[0])<<25)|(uint64_t(p[1])<<17)|(uint64_t(p[2])<<9)|(uint64_t(p[3])<<1)|(p[4]>>7))*300+((p[4]&1)<<8)+p[5];}
static int inspect_simple(const unsigned char* d,size_t n,bool edited){
 if(!n||n%188)return 1;
 unsigned pes=0,pcrs=0,pat=0,pmt=0;int lastcc=-1;
 for(size_t k=0;k<n;k+=188){
  const unsigned char* p=d+k;if(p[0]!=0x47)return 2;
  unsigned pid=((p[1]&31)<<8)|p[2],afc=(p[3]>>4)&3,cc=p[3]&15;
  if(pid==0){++pat;continue;}if(pid==0x100){++pmt;continue;}if(pid!=0x102)return 3;
  if(afc==0)return 4;
  size_t off=4;if(afc&2){if(p[4]>183)return 5;off+=1+p[4];}
  if(edited && afc==2){
   if((p[1]&64)||p[4]!=183||p[5]!=16||cc!=unsigned(lastcc<0?0:lastcc))return 6;
   if(pcrs>=5||read_pcr(p+6)!=(100000ULL+pcrs*3600)*300)return 7;
   ++pcrs;continue;
  }
  if(!(afc&1)||off>=188)return 8;
  if(edited && cc!=unsigned(lastcc<0?0:(lastcc+1)%16))return 9;
  lastcc=cc;
  if(p[1]&64){
   const unsigned char* q=p+off;if(off+14>188||q[0]||q[1]||q[2]!=1||q[3]!=0xe0)return 10;
   uint64_t want=(edited?109000ULL:9000ULL)+pes*3600;
   if(read_pts(q+9)!=want)return 11;
   ++pes;
  }
 }
 return pat==1&&pmt==1&&pes==4&&(!edited||pcrs==5)?0:12;
}
#endif
