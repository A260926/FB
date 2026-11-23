#include "gsm.h"
#include "private.h"
#include "../src/plc_frame.c"
#include <stdio.h>
#include <string.h>
int main(void){
 gsm en=gsm_create(),ew=gsm_create(); int one=1; gsm_option(ew,GSM_OPT_WAV49,&one);
 gsm_signal pcm[320]; int i; gsm_byte wb[65];
 for(i=0;i<320;i++){unsigned t=i;pcm[i]=(gsm_signal)((int)((t*73+t*t*3)%24001)-12000);}
 memset(wb,0,65); gsm_encode(ew,pcm,wb); gsm_encode(ew,pcm+160,wb+32);
 word L1[8],N1[4],b1[4],M1[4],x1[4],xm1[52]; word L2[8],N2[4],b2[4],M2[4],x2[4],xm2[52];
 gsm_byte even_chain;
 unpack_wav_even(wb,L1,N1,b1,M1,x1,xm1,&even_chain);
 gsm_byte shared_low = wb[32] & 0x0f;
 printf("even_chain=%u shared_low=%u sharedbyte=0x%02x\n",
	(unsigned)even_chain,(unsigned)shared_low,wb[32]);
 /* odd via shared low nibble vs via even chain */
 word La[8],Na[4],ba[4],Ma[4],xa[4],xma[52];
 unpack_wav_odd(shared_low, wb+33, La,Na,ba,Ma,xa,xma);
 unpack_wav_odd(even_chain, wb+33, L2,N2,b2,M2,x2,xm2);
 int diff=0,k;
 for(k=0;k<8;k++) if(La[k]!=L2[k]){printf("L[%d] shared=%d even=%d\n",k,La[k],L2[k]);diff=1;}
 printf(diff?"DIFFER\n":"SAME\n");
 gsm_destroy(en);gsm_destroy(ew);return 0;
}
