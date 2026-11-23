#include "gsm.h"
#include "private.h"
#include <stdio.h>
#include <string.h>
int main(void){
 gsm w=gsm_create(); int one=1;
 gsm_option(w,GSM_OPT_WAV49,&one);
 struct gsm_state *p=(struct gsm_state*)w;
 gsm_signal out[320]; int i;
 for(i=0;i<320;i++) out[i]=0x1234;
 int rc=gsm_decode_plc(w,(const gsm_byte*)0,0,0,out,320);
 printf("rc=%d have=%u lost0=%u lost1=%u idx=%u\n",rc,p->plc_have,p->plc_lost[0],p->plc_lost[1],p->frame_index);
 int nz=0; for(i=0;i<320;i++) if(out[i]!=0){nz++; if(nz<6)printf("  out[%d]=%d\n",i,(int)out[i]);}
 printf("nonzero=%d msr=%d\n",nz,(int)p->msr);
 gsm_destroy(w); return 0;
}
