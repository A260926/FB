#include <sbc.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
  sbc_t enc,dec; sbc_reset(&enc);sbc_reset(&dec);
  struct sbc_frame f={.msbc=false,.freq=SBC_FREQ_48K,.mode=SBC_MODE_JOINT_STEREO,
    .bam=SBC_BAM_LOUDNESS,.nblocks=16,.nsubbands=8,.bitpool=32},parsed;
  int16_t in[256],out[256]; unsigned char frame[512];
  double energy=0; int i,k;
  assert(sbc_get_frame_size(&f)==77);
  for(k=0;k<3;k++) {
    for(i=0;i<128;i++) {
      in[2*i]=(int16_t)(12000*sin((k*128+i)*0.1));
      in[2*i+1]=(int16_t)(9000*sin((k*128+i)*0.11+0.3));
    }
    assert(!sbc_encode(&enc,in,2,in+1,2,&f,frame,sizeof frame));
    assert(!sbc_probe(frame,&parsed));
    assert(parsed.bitpool==32 && parsed.mode==SBC_MODE_JOINT_STEREO);
    assert(!sbc_decode(&dec,frame,77,&parsed,out,2,out+1,2));
    for(i=0;i<256;i++) energy+=(double)out[i]*out[i];
  }
  assert(energy>100000);
  printf("SBC_BASELINE_OK frames=3 bytes-per-frame=77\n");
  return 0;
}
