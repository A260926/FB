#include "gsm.h"
#include <stdio.h>
#include <string.h>
static int diffs(const short*a,const short*b,int n){int d=0,i;for(i=0;i<n;i++)if(a[i]!=b[i])d++;return d;}
int main(void){
 gsm en=gsm_create(),ew=gsm_create(); int one=1; gsm_option(ew,GSM_OPT_WAV49,&one);
 int n=4,f,i; gsm_signal pcm[640]; gsm_byte nf[4][33]; gsm_byte wb[2][65];
 for(f=0;f<n;f++)for(i=0;i<160;i++){unsigned t=f*160+i;pcm[f*160+i]=(gsm_signal)((int)((t*73+t*t*3)%24001)-12000);}
 for(f=0;f<n;f++){memset(nf[f],0,33);gsm_encode(en,pcm+f*160,nf[f]);}
 for(f=0;f<n;f+=2){memset(wb[f/2],0,65);gsm_encode(ew,pcm+f*160,wb[f/2]);gsm_encode(ew,pcm+(f+1)*160,wb[f/2]+32);}
 gsm ln=gsm_create(), lw=gsm_create(), bn=gsm_create(), bw=gsm_create();
 gsm_option(lw,GSM_OPT_WAV49,&one); gsm_option(bw,GSM_OPT_WAV49,&one);
 gsm_signal oln[640],olw[640],obn[640],obw[640];
 for(f=0;f<n;f++) gsm_decode(ln,nf[f],oln+f*160);
 gsm_decode(lw,wb[0],olw); gsm_decode(lw,wb[0]+33,olw+160);
 gsm_decode(lw,wb[1],olw+320); gsm_decode(lw,wb[1]+33,olw+480);
 for(f=0;f<n;f++) gsm_decode_plc(bn,nf[f],33,1,obn+f*160,160);
 gsm_decode_plc(bw,wb[0],65,3,obw,320);
 gsm_decode_plc(bw,wb[1],65,3,obw+320,320);
 printf("legacy-normal vs legacy-wav   : %d\n",diffs(oln,olw,640));
 printf("block-normal  vs block-wav    : %d\n",diffs(obn,obw,640));
 printf("legacy-normal vs block-normal : %d\n",diffs(oln,obn,640));
 printf("legacy-wav    vs block-wav    : %d\n",diffs(olw,obw,640));
 return 0;
}
