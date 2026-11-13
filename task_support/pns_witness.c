#define main baseline_main
#include "baseline.c"
#undef main
#include <stdlib.h>
static unsigned unilateral(unsigned mask){
 memset(bytes,0,sizeof(bytes));pos=0;
 bits(1,3);bits(0,4);bits(1,1);
 bits(0,1);bits(0,2);bits(0,1);bits(1,6);bits(0,1);bits(mask,2);
 bits(100,8);bits(0,4);bits(1,5);bits(0,1);bits(0,1);bits(0,1);
 bits(100,8);bits(13,4);bits(1,5);bits(280,9);bits(0,1);bits(0,1);bits(0,1);
 bits(7,3);return (pos+7)/8;
}
int main(void){
 double e[2]={0,0};
 for(int mode=0;mode<2;mode++){
  unsigned char asc[]={0x12,0x10};unsigned long sr;unsigned char ch;NeAACDecFrameInfo fi;
  NeAACDecHandle d=NeAACDecOpen();NeAACDecConfigurationPtr c=NeAACDecGetCurrentConfiguration(d);
  c->outputFormat=FAAD_FMT_FLOAT;NeAACDecSetConfiguration(d,c);if(NeAACDecInit2(d,asc,2,&sr,&ch))return 1;
  unsigned len=unilateral(mode?2:0);
  for(int k=0;k<8;k++){
   float *v=NeAACDecDecode(d,&fi,bytes,len);if(fi.error)return 2;
   for(unsigned j=0;j<fi.samples;j+=2)e[mode]+=(double)v[j]*v[j];
  }
  NeAACDecClose(d);
 }
 printf("PNS_RIGHT_ONLY left_energy mask0=%.12g mask2=%.12g\n",e[0],e[1]);
 if(getenv("EXPECT_PNS_FIX") && e[1]!=e[0])return 42;
 return 0;
}
