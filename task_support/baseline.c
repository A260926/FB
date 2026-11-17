#include "gsm.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t hash_bytes(uint64_t h,const void *p,unsigned n){const unsigned char *b=p;while(n--)h=(h^*b++)*UINT64_C(1099511628211);return h;}
int main(void){
 gsm en=gsm_create(), ew=gsm_create(),dn=gsm_create(),dw=gsm_create();
 gsm_signal in[320],n[320],w[320];gsm_byte a[66],b[66];
 unsigned frame,i;int one=1;uint64_t hc=1469598103934665603ULL,hp=1469598103934665603ULL;unsigned nonzero=0;
 if(!en||!ew||!dn||!dw)return 2;
 gsm_option(ew,GSM_OPT_WAV49,&one);gsm_option(dw,GSM_OPT_WAV49,&one);
 for(frame=0;frame<8;frame+=2){
  for(i=0;i<320;i++){unsigned t=frame*160+i;in[i]=(gsm_signal)((int)((t*73+t*t*3)%24001)-12000);}
  memset(a,0,sizeof a);memset(b,0,sizeof b);
  gsm_encode(en,in,a);gsm_encode(en,in+160,a+33);
  gsm_encode(ew,in,b);gsm_encode(ew,in+160,b+32);
  if(gsm_decode(dn,a,n)||gsm_decode(dn,a+33,n+160)||gsm_decode(dw,b,w)||gsm_decode(dw,b+33,w+160))return 3;
  if(memcmp(n,w,sizeof n))return 4;
  for(i=0;i<320;i++)if(n[i])nonzero++;
  hc=hash_bytes(hc,a,66);hp=hash_bytes(hp,n,sizeof n);
 }
 if(!nonzero)return 5;
 printf("GSM_BASELINE frames=8 samples=1280 packing_equal=1 coded=%016llx pcm=%016llx nonzero=%u\n",(unsigned long long)hc,(unsigned long long)hp,nonzero);
 gsm_destroy(en);gsm_destroy(ew);gsm_destroy(dn);gsm_destroy(dw);return 0;
}
