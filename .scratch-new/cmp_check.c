#include "gsm.h"
#include <stdio.h>
#include <string.h>
int main(void){
 gsm en=gsm_create(), ew=gsm_create(),dn=gsm_create(),dw=gsm_create();
 gsm_signal in[320],n[320],w[320];gsm_byte a[66],b[66];
 unsigned frame,i;int one=1;
 if(!en||!ew||!dn||!dw)return 2;
 gsm_option(ew,GSM_OPT_WAV49,&one);gsm_option(dw,GSM_OPT_WAV49,&one);
 for(frame=0;frame<8;frame+=2){
  for(i=0;i<320;i++){unsigned t=frame*160+i;in[i]=(gsm_signal)((int)((t*73+t*t*3)%24001)-12000);}
  memset(a,0,sizeof a);memset(b,0,sizeof b);
  gsm_encode(en,in,a);gsm_encode(en,in+160,a+33);
  gsm_encode(ew,in,b);gsm_encode(ew,in+160,b+32);
  if(gsm_decode(dn,a,n)||gsm_decode(dn,a+33,n+160)||gsm_decode(dw,b,w)||gsm_decode(dw,b+33,w+160))return 3;
  for(i=0;i<320;i++) if(n[i]!=w[i]){
    printf("MISMATCH framepair=%u sample=%u n=%d w=%d\n",frame,i,(int)n[i],(int)w[i]);
    return 0;
  }
 }
 printf("ALL_MATCH\n");
 gsm_destroy(en);gsm_destroy(ew);gsm_destroy(dn);gsm_destroy(dw);return 0;
}
