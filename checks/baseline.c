#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
enum {F=64};
int main(void) {
  short zero[F]={0}, ref[F]={0}, mic[F], a[F], b[F];
  SpeexEchoState *async=speex_echo_state_init(F,256), *direct=speex_echo_state_init(F,256);
  unsigned rng=7; short history[2][F]={{0}};
  for(int frame=0;frame<80;frame++) {
    for(int i=0;i<F;i++) {rng=rng*1664525u+1013904223u; mic[i]=(short)((int)(rng%2000)-1000);}
    speex_echo_capture(async,mic,a);
    speex_echo_cancellation(direct,mic,frame<2?zero:history[frame%2],b);
    assert(memcmp(a,b,sizeof(a))==0);
    for(int i=0;i<F;i++) ref[i]=(short)(mic[i]/2);
    speex_echo_playback(async,ref); memcpy(history[frame%2],ref,sizeof(ref));
  }
  puts("AEC_MONO_DELAY_REFERENCE_OK frames=80");
  speex_echo_state_destroy(async); speex_echo_state_destroy(direct);
  SpeexEchoState *mc=speex_echo_state_init_mc(F,256,2,2);
  short in[2*F], out[2*F]; for(int i=0;i<2*F;i++) in[i]=(short)(20+i);
  speex_echo_capture(mc,in,out); speex_echo_capture(mc,in,out);
  for(int i=0;i<2*F;i++) out[i]=-12345;
  speex_echo_capture(mc,in,out);
  int copied=0, stale=0;
  for(int i=0;i<2*F;i++) { copied+=(out[i]==in[i]); stale+=(out[i]==-12345); }
  /* Underrun: every microphone channel is copied through untouched and no
     sentinel sample may survive inside the frame. */
  assert(copied==2*F);
  assert(stale==0);
  printf("AEC_MULTIMIC_UNDERRUN_COPY samples=%d\n",copied);
  speex_echo_state_destroy(mc);
}
