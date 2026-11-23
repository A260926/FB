#include "gsm.h"
#include "private.h"
#include "../src/plc_frame.c"
#include <stdio.h>
#include <string.h>
int main(void){
 gsm en=gsm_create(),ew=gsm_create(); int one=1; gsm_option(ew,GSM_OPT_WAV49,&one);
 int n=16,f,i,mism=0; gsm_signal pcm[320]; gsm_byte wb[65];
 word L[8],N[4],bc[4],M[4],x[4],xm[52]; gsm_byte chain;
 for(f=0;f<n;f+=2){
   for(i=0;i<320;i++){unsigned t=f*160+i;pcm[i%160]=(gsm_signal)((int)((t*73+t*t*3)%24001)-12000);}
   memset(wb,0,65);
   gsm_encode(ew,pcm,wb);
   gsm_encode(ew,pcm+160,wb+32);
   unpack_wav_even(wb,L,N,bc,M,x,xm,&chain);
   unsigned hi=wb[32]>>4, lo=wb[32]&0xf;
   printf("block%d chain=%u hi=%u lo=%u %s\n",f/2,(unsigned)chain,hi,lo,
     (chain==hi)?"hi-ok":"HI-BAD");
   if(chain!=hi)mism++;
 }
 printf("mismatches=%d\n",mism);
 gsm_destroy(en);gsm_destroy(ew);return 0;
}
