/* Fixed production binding. No implementation of the requested API. */
#include "granule.h"
#include "task_support/fixture.h"
#include "task_support/work.h"
#include <stdio.h>
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"API_CONSUMER line=%d: %s\n",__LINE__,#x);return 21;}}while(0)
int main(void) {
  unsigned char bytes[TASK_MAX_BYTES];static mad_fixed_t ref[2][TASK_MAX_SAMPLES];
  struct mad_granule_decoder *d;struct mad_pcm pcm;struct mad_granule_progress p;
  unsigned count,i,ch;size_t len,accepted;uint64_t hash,s,t;
  len=task_fixture(bytes,0,0);REQUIRE(!task_reference(bytes,len,0,ref,&count,&hash));
  REQUIRE(hash==UINT64_C(0x93b20feb62343de2));d=mad_granule_new(0);REQUIRE(d);
  REQUIRE(!mad_granule_feed(d,bytes,len,&accepted) && accepted==len);
  REQUIRE(!mad_granule_finish(d));s=task_spectral_passes;t=task_synth_passes;
  REQUIRE(mad_granule_next(d,&pcm)==MAD_GRANULE_PCM);
  REQUIRE(pcm.length==576 && pcm.channels==2 && pcm.samplerate==44100);
  for(i=0;i<576;++i) for(ch=0;ch<2;++ch) REQUIRE(pcm.samples[ch][i]==ref[ch][i]);
  REQUIRE(task_spectral_passes==s+1 && task_synth_passes==t+1);
  REQUIRE(!mad_granule_progress(d,&p) && p.granules==1 && p.physical_frames==0 && p.sample_frames==576 && p.error==MAD_ERROR_NONE);
  mad_granule_free(d);puts("MAD_GRANULE_API_OK first=576 physical=0 spectral=1 synth=1");return 0;
}
