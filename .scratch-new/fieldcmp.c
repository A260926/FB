#include "gsm.h"
#include "private.h"
#include <stdio.h>
#include <string.h>
int main(void){
 gsm ew=gsm_create(); int one=1; gsm_option(ew,GSM_OPT_WAV49,&one);
 int i,k; gsm_signal pcm[320]; gsm_byte wb[65];
 for(i=0;i<320;i++){unsigned t=i;pcm[i]=(gsm_signal)((int)((t*73+t*t*3)%24001)-12000);}
 memset(wb,0,65); gsm_encode(ew,pcm,wb); gsm_encode(ew,pcm+160,wb+32);
 gsm L=gsm_create(); gsm_option(L,GSM_OPT_WAV49,&one);
 gsm B=gsm_create(); gsm_option(B,GSM_OPT_WAV49,&one);
 gsm_signal ol[320],ob[320];
 struct gsm_state *pl=(struct gsm_state*)L,*pb=(struct gsm_state*)B;
 gsm_decode(L,wb,ol);
 gsm_decode_plc(B,wb,65,1,ob,320);
 printf("dp0 memcmp=%d\n",memcmp(pl->dp0,pb->dp0,sizeof pl->dp0));
 printf("e memcmp=%d\n",memcmp(pl->e,pb->e,sizeof pl->e));
 printf("z1=%d/%d L_z2=%ld/%ld mp=%d/%d\n",pl->z1,pb->z1,(long)pl->L_z2,(long)pb->L_z2,pl->mp,pb->mp);
 printf("u memcmp=%d LARpp memcmp=%d j=%d/%d nrp=%d/%d v memcmp=%d msr=%d/%d\n",
   memcmp(pl->u,pb->u,sizeof pl->u),memcmp(pl->LARpp,pb->LARpp,sizeof pl->LARpp),pl->j,pb->j,
   pl->nrp,pb->nrp,memcmp(pl->v,pb->v,sizeof pl->v),pl->msr,pb->msr);
 printf("wav_fmt=%d/%d frame_index=%d/%d frame_chain=%d/%d\n",pl->wav_fmt,pb->wav_fmt,
   pl->frame_index,pb->frame_index,pl->frame_chain,pb->frame_chain);
 printf("plc_have=%d/%d plc_lost=%d,%d/%d,%d\n",pl->plc_have,pb->plc_have,
   pl->plc_lost[0],pl->plc_lost[1],pb->plc_lost[0],pb->plc_lost[1]);
 printf("plc_lar memcmp=%d plc_nc memcmp=%d plc_bc memcmp=%d plc_e memcmp=%d\n",
   memcmp(pl->plc_lar,pb->plc_lar,sizeof pl->plc_lar),
   memcmp(pl->plc_nc,pb->plc_nc,sizeof pl->plc_nc),
   memcmp(pl->plc_bc,pb->plc_bc,sizeof pl->plc_bc),
   memcmp(pl->plc_e,pb->plc_e,sizeof pl->plc_e));
 (void)k;
 gsm_destroy(ew);gsm_destroy(L);gsm_destroy(B);return 0;
}
