#include "voice_fixture.h"
int main(void) {
    short a[128],b[128];int i;long sum_a=0,sum_b=0;
    MikMod_RegisterDriver(&drv_nos);
    if(vf_start())return 20;
    if(vf_read(a,64))return 21;
    Voice_Play(0,vf_b,0);
    if(vf_read(b,64))return 22;
    for(i=0;i<128;i++) {sum_a+=abs(a[i]);sum_b+=abs(b[i]);}
    vf_stop();
    printf("BASELINE voice_frames=128 old_abs=%ld new_abs=%ld\n",sum_a,sum_b);
    if(!sum_a||!sum_b)return 23;
    puts("BASELINE_OK real_voice_retrigger");return 0;
}
