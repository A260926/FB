#include "voice_fixture.h"
int main(void) {
    short old[128],incoming[128],actual[128],settle[512];int i;
    MikMod_RegisterDriver(&drv_nos);
    if(MikMod_SetRetriggerFade(0)!=1)return 20;
    if(vf_start()||vf_read(old,64))return 21;
    vf_stop();
    if(MikMod_SetRetriggerFade(0)!=1||vf_start())return 22;
    Voice_Play(0,vf_b,0);
    if(vf_read(settle,256)||vf_read(incoming,64))return 23;
    vf_stop();
    if(MikMod_SetRetriggerFade(64)!=1||vf_start())return 24;
    Voice_Play(0,vf_b,0);
    Sample_Free(vf_a);vf_a=NULL;
    if(vf_read(actual,64))return 25;
    vf_stop();
    for(i=0;i<128;i++) {
        int j=i/2;
        int expected=((64-j)*(int)old[i]+j*(int)incoming[i])/64;
        if(abs(actual[i]-expected)>1) {
            fprintf(stderr,"RETRIGGER_MISMATCH frame=%d expected=%d actual=%d\n",j,expected,actual[i]);
            return 30;
        }
    }
    puts("API_RETRIGGER_OK moving_old_pcm_after_sample_free");return 0;
}
