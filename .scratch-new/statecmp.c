#include "gsm.h"
#include "private.h"
#include <stdio.h>
#include <string.h>
int main(void){
 gsm ew=gsm_create(); int one=1; gsm_option(ew,GSM_OPT_WAV49,&one);
 int i; gsm_signal pcm[320]; gsm_byte wb[65];
 for(i=0;i<320;i++){unsigned t=i;pcm[i]=(gsm_signal)((int)((t*73+t*t*3)%24001)-12000);}
 memset(wb,0,65); gsm_encode(ew,pcm,wb); gsm_encode(ew,pcm+160,wb+32);
 gsm L=gsm_create(); gsm_option(L,GSM_OPT_WAV49,&one);
 gsm B=gsm_create(); gsm_option(B,GSM_OPT_WAV49,&one);
 gsm_signal ol[320],ob[320];
 struct gsm_state *pl=(struct gsm_state*)L,*pb=(struct gsm_state*)B;
 gsm_decode(L,wb,ol);                                  /* legacy even */
 gsm_decode_plc(B,wb,65,1,ob,320);                     /* block even-only mask1 */
 printf("after even state memcmp=%d (0=same)\n",memcmp(pl,pb,sizeof(*pl)));
 printf("after even pcm memcmp=%d\n",memcmp(ol,ob,160*sizeof(short)));
 printf("L frame_index=%u chain=%u | B frame_index=%u chain=%u\n",
   pl->frame_index,pl->frame_chain,pb->frame_index,pb->frame_chain);
 gsm_destroy(ew);gsm_destroy(L);gsm_destroy(B);return 0;
}
