#include "neaacdec.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static unsigned char bytes[4096];static unsigned pos;
static void bits(unsigned v,unsigned n){while(n--){unsigned b=(v>>n)&1;bytes[pos>>3]|=b<<(7-(pos&7));pos++;}}
static unsigned frame(void){
 memset(bytes,0,sizeof(bytes));pos=0;
 bits(1,3);bits(0,4);bits(1,1); /* CPE, tag, common window */
 bits(0,1);bits(0,2);bits(0,1);bits(1,6);bits(0,1); /* long ICS */
 bits(2,2); /* all bands correlated */
 for(int c=0;c<2;c++){
  bits(100,8);bits(13,4);bits(1,5);bits(256+4*c,9);
  bits(0,1);bits(0,1);bits(0,1);
 }
 bits(7,3);return (pos+7)/8;
}
int main(void){
 unsigned char asc[]={0x12,0x10};unsigned long rate;unsigned char channels;NeAACDecFrameInfo fi;
 NeAACDecHandle d=NeAACDecOpen();if(!d)return 1;
 NeAACDecConfigurationPtr cfg=NeAACDecGetCurrentConfiguration(d);cfg->outputFormat=FAAD_FMT_FLOAT;
 if(!NeAACDecSetConfiguration(d,cfg))return 2;
 if(NeAACDecInit2(d,asc,2,&rate,&channels))return 3;
 unsigned len=frame(),total=0;double energy=0;
 for(int p=0;p<8;p++){
  float *pcm=NeAACDecDecode(d,&fi,bytes,len);
  if(fi.error){fprintf(stderr,"AAC_ERROR %d %s\n",fi.error,NeAACDecGetErrorMessage(fi.error));return 4;}
  if(fi.bytesconsumed!=len)return 5;
  if(fi.samples && !pcm)return 6;
  for(unsigned j=0;j<fi.samples;j++){if(!isfinite(pcm[j]))return 7;energy+=(double)pcm[j]*pcm[j];}
  total+=fi.samples;
 }
 if(total==0||energy<=0)return 8;
 printf("FAAD_PNS_BASELINE packets=8 samples=%u rate=%lu channels=%u energy=%.9g\n",total,rate,channels,energy);
 NeAACDecClose(d);return 0;
}
