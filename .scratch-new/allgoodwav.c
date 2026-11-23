#include "gsm.h"
#include <stdio.h>
#include <string.h>
int main(void){
 gsm en=gsm_create(),ew=gsm_create(); int one=1; gsm_option(ew,GSM_OPT_WAV49,&one);
 int n=4,f,i; gsm_signal pcm[640]; gsm_byte nf[4][33]; gsm_byte wb[2][65];
 for(f=0;f<n;f++){for(i=0;i<160;i++){unsigned t=f*160+i;pcm[f*160+i]=(gsm_signal)((int)((t*73+t*t*3)%24001)-12000);}memset(nf[f],0,33);gsm_encode(en,pcm+f*160,nf[f]);}
 for(f=0;f<n;f+=2){memset(wb[f/2],0,65);gsm_encode(ew,pcm+f*160,wb[f/2]);gsm_encode(ew,pcm+(f+1)*160,wb[f/2]+32);}
 gsm a=gsm_create(), b=gsm_create(); gsm_option(b,GSM_OPT_WAV49,&one);
 gsm_signal oa[640],ob[640];
 for(f=0;f<n;f++) gsm_decode_plc(a,nf[f],33,1,oa+f*160,160);
 for(f=0;f<2;f++) gsm_decode_plc(b,wb[f],65,3,ob+2*f*160,320);
 int diff=0,first=-1; for(i=0;i<n*160;i++){if(oa[i]!=ob[i]){if(first<0)first=i;diff++;}}
 printf("all-good normal-vs-wav-plc diffs=%d first=%d\n",diff,first);
 /* also legacy wav vs plc wav */
 gsm c=gsm_create(); gsm_option(c,GSM_OPT_WAV49,&one); gsm_signal oc[640];
 gsm_decode(c,wb[0],oc); gsm_decode(c,wb[0]+32,oc+160);
 gsm_decode(c,wb[1],oc+320); gsm_decode(c,wb[1]+32,oc+480);
 int d2=0,fi2=-1; for(i=0;i<n*160;i++){if(oc[i]!=ob[i]){if(fi2<0)fi2=i;d2++;}}
 printf("legacy-wav-vs-plc-wav diffs=%d first=%d\n",d2,fi2);
 gsm_destroy(en);gsm_destroy(ew);gsm_destroy(a);gsm_destroy(b);gsm_destroy(c);return 0;
}
