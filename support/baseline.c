#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "neaacdec.h"
#include "common.h"
#include "structs.h"
#include "tns.h"
static unsigned char bits[64]; static unsigned pos;
static void put(unsigned v,unsigned n){while(n--){bits[pos/8]|=((v>>n)&1)<<(7-pos%8);++pos;}}
int main(void){
 ic_stream s={0}; real_t x[1024]={1};
 s.tns_data_present=1;s.num_windows=1;s.num_swb=s.max_sfb=1;s.swb_offset[1]=s.swb_offset_max=4;
 s.tns.n_filt[0]=1;s.tns.length[0][0]=1;s.tns.order[0][0]=1;s.tns.coef[0][0][0]=1;
 tns_decode_frame(&s,&s.tns,4,LC,x,1024);
 if(fabs(x[1]+0.4338837391)>1e-6)return 2;
 printf("TNS_BASELINE impulse=%.9g\n",(double)x[1]);
 /* Raw AAC-LC SCE: one codebook-1 band containing (1,0,0,0), first-order TNS. */
 put(0,3);put(0,4);put(140,8);put(0,1);put(0,2);put(0,1);put(1,6);put(0,1);
 put(1,4);put(1,5);put(0,1);put(0,1);put(1,1);
 put(1,2);put(0,1);put(49,6);put(1,5);put(0,1);put(0,1);put(1,3);
 put(0,1);put(16,5);put(7,3);
 unsigned char asc[]={0x12,0x08};unsigned long rate;unsigned char channels;
 NeAACDecHandle d=NeAACDecOpen();NeAACDecConfigurationPtr c=NeAACDecGetCurrentConfiguration(d);
 c->outputFormat=FAAD_FMT_FLOAT;NeAACDecSetConfiguration(d,c);
 if(NeAACDecInit2(d,asc,sizeof asc,&rate,&channels))return 3;
 unsigned long samples=0; double energy=0;
 for(int f=0;f<4;f++){NeAACDecFrameInfo info;float *pcm=NeAACDecDecode(d,&info,bits,(pos+7)/8);
 if(info.error||!pcm){fprintf(stderr,"decode error %d\n",info.error);return 4;}
 samples+=info.samples;for(unsigned long i=0;i<info.samples;i++)energy+=pcm[i]*pcm[i];}
 NeAACDecClose(d);if(!samples||!(energy>0))return 5;
 printf("AAC_BASELINE rate=%lu channels=%u samples=%lu nonzero=1\n",rate,channels,samples);return 0;
}
