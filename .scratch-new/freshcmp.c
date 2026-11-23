#include "gsm.h"
#include <stdio.h>
#include <string.h>
int main(void){
 gsm ew=gsm_create(); int one=1; gsm_option(ew,GSM_OPT_WAV49,&one);
 int n=4,f,i; gsm_signal pcm[640]; gsm_byte wb[2][65];
 for(f=0;n>f;f++){for(i=0;i<160;i++){unsigned t=f*160+i;pcm[f*160+i]=(gsm_signal)((int)((t*73+t*t*3)%24001)-12000);}}
 gsm en=gsm_create(); gsm_byte nf[4][33];
 for(f=0;f<n;f++){memset(nf[f],0,33);gsm_encode(en,pcm+f*160,nf[f]);}
 for(f=0;f<n;f+=2){memset(wb[f/2],0,65);gsm_encode(ew,pcm+f*160,wb[f/2]);gsm_encode(ew,pcm+(f+1)*160,wb[f/2]+32);}
 gsm legacy=gsm_create(); gsm_option(legacy,GSM_OPT_WAV49,&one);
 gsm block=gsm_create();  gsm_option(block,GSM_OPT_WAV49,&one);
 gsm_signal ol[640],ob[640];
 gsm_decode(legacy,wb[0],ol); gsm_decode(legacy,wb[0]+32,ol+160);
 gsm_decode(legacy,wb[1],ol+320); gsm_decode(legacy,wb[1]+32,ol+480);
 gsm_decode_plc(block,wb[0],65,3,ob,320);
 gsm_decode_plc(block,wb[1],65,3,ob+320,320);
 int diff=0,first=-1; for(i=0;i<n*160;i++){if(ol[i]!=ob[i]){if(first<0)first=i;diff++;}}
 printf("fresh legacy-wav vs plc-wav diffs=%d first=%d\n",diff,first);
 gsm_destroy(ew);gsm_destroy(en);gsm_destroy(legacy);gsm_destroy(block);return 0;
}
