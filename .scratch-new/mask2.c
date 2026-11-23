#include "gsm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void){
 gsm en=gsm_create(),ew=gsm_create(); int one=1; gsm_option(ew,GSM_OPT_WAV49,&one);
 int n=8,f,i; gsm_signal pcm[1280]; gsm_byte nf[8][33]; gsm_byte wb[4][65];
 for(f=0;f<n;f++){for(i=0;i<160;i++){unsigned t=f*160+i;pcm[f*160+i]=(gsm_signal)((int)((t*73+t*t*3)%24001)-12000);}memset(nf[f],0,33);gsm_encode(en,pcm+f*160,nf[f]);}
 for(f=0;f<n;f+=2){memset(wb[f/2],0,65);gsm_encode(ew,pcm+f*160,wb[f/2]);gsm_encode(ew,pcm+(f+1)*160,wb[f/2]+32);}
 gsm a=gsm_create(), b=gsm_create(); gsm_option(b,GSM_OPT_WAV49,&one);
 gsm_signal oa[1280],ob[1280];
 for(f=0;f<n;f++){ int good=(f&1); /* even lost, odd good */
   if(good){ gsm_decode_plc(a,nf[f],33,1,oa+f*160,160);} else { gsm_decode_plc(a,0,0,0,oa+f*160,160);} }
 for(f=0;f<4;f++){ gsm_decode_plc(b,wb[f],65,2,ob+2*f*160,320);} /* mask 2 */
 int diff=0,first=-1; for(i=0;i<n*160;i++){if(oa[i]!=ob[i]){if(first<0)first=i;diff++;}}
 printf("normal-vs-wav mask2 diffs=%d first=%d (n=%d w=%d)\n",diff,first,first>=0?oa[first]:0,first>=0?ob[first]:0);
 gsm_destroy(en);gsm_destroy(ew);gsm_destroy(a);gsm_destroy(b);return 0;
}
