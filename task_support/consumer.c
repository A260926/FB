/* Fixed contract consumer. Production answers do not belong in this file. */
#include "granule.h"
#include "task_support/check.h"
#include "task_support/fixture.h"
#include "task_support/work.h"
#include <string.h>
#include <stdint.h>
static const uint64_t expected[2][2]={
 {UINT64_C(0x93b20feb62343de2),UINT64_C(0x0a30c68a1b860611)},
 {UINT64_C(0x2f72906cf02bfab5),UINT64_C(0x6600ed334f03ec52)}};
static int run_stream(int lsf,int half,int fragmented) {
  unsigned char bytes[TASK_MAX_BYTES],scratch[TASK_MAX_BYTES];
  static mad_fixed_t ref[2][TASK_MAX_SAMPLES];
  struct mad_granule_decoder *d;struct mad_pcm pcm;struct mad_granule_progress p;
  unsigned n,delivered=0,round=0,i,ch,granules=0;size_t len,pos=0,used,take;
  uint64_t hash,s,t;int finished=0,status,options=half?MAD_OPTION_HALFSAMPLERATE:0;
  len=task_fixture(bytes,lsf,0);TASK_CHECK(!task_reference(bytes,len,options,ref,&n,&hash));
  TASK_CHECK(hash==expected[lsf][half]);d=mad_granule_new(options);TASK_CHECK(d);
  for(;;) {
    TASK_CHECK(++round<20000);s=task_spectral_passes;t=task_synth_passes;
    if(pos<len) {
      take=fragmented?(round%17)+1:len-pos;if(take>len-pos)take=len-pos;
      memcpy(scratch,bytes+pos,take);used=(size_t)-1;
      TASK_CHECK(!mad_granule_feed(d,scratch,take,&used) && used<=take);
      pos+=used;memset(scratch,0xcc,sizeof scratch);
    } else if(!finished) {TASK_CHECK(!mad_granule_finish(d));finished=1;}
    TASK_CHECK(task_spectral_passes==s && task_synth_passes==t);
    TASK_CHECK(!mad_granule_progress(d,&p) && p.sample_frames==delivered && p.granules==granules && p.buffered_bytes<=16384);
    used=99;TASK_CHECK(!mad_granule_feed(d,NULL,0,&used) || finished);
    if(!finished)TASK_CHECK(used==0);
    TASK_CHECK(task_spectral_passes==s && task_synth_passes==t);
    status=mad_granule_next(d,&pcm);
    if(status==MAD_GRANULE_PCM) {
      TASK_CHECK(task_spectral_passes==s+1 && task_synth_passes==t+1);
      TASK_CHECK(pcm.length==(half?288:576) && pcm.channels==2);
      TASK_CHECK(pcm.samplerate==(unsigned)(lsf?22050:44100)/(half?2:1));
      TASK_CHECK(delivered+pcm.length<=n);
      for(i=0;i<pcm.length;++i)for(ch=0;ch<2;++ch)TASK_CHECK(pcm.samples[ch][i]==ref[ch][delivered+i]);
      delivered+=pcm.length;++granules;
      TASK_CHECK(!mad_granule_progress(d,&p));
      TASK_CHECK(p.sample_frames==delivered && p.granules==granules && p.physical_frames==granules/(lsf?1:2));
    } else if(status==MAD_GRANULE_NEED_INPUT) {
      TASK_CHECK(!finished && pcm.length==0);
      TASK_CHECK(task_spectral_passes==s && task_synth_passes==t);
    } else {
      TASK_CHECK(status==MAD_GRANULE_END && finished && delivered==n && pcm.length==0);
      TASK_CHECK(task_spectral_passes==s && task_synth_passes==t);break;
    }
  }
  TASK_CHECK(mad_granule_next(d,&pcm)==MAD_GRANULE_END);
  TASK_CHECK(!mad_granule_finish(d));mad_granule_free(d);return 0;
}
static int streams(void) {
  int lsf,half,split;for(lsf=0;lsf<2;++lsf)for(half=0;half<2;++half)for(split=0;split<2;++split)TASK_CHECK(!run_stream(lsf,half,split));return 0;
}
static int late_failure(void) {
  unsigned char bytes[TASK_MAX_BYTES],bad[TASK_MAX_BYTES];static mad_fixed_t ref[2][TASK_MAX_SAMPLES];
  struct mad_granule_decoder *d;struct mad_pcm pcm;struct mad_granule_progress p;
  unsigned n,i,ch;size_t len,used;uint64_t hash,s,t;
  len=task_fixture(bytes,0,0);TASK_CHECK(!task_reference(bytes,len,0,ref,&n,&hash));task_fixture(bad,0,1);
  d=mad_granule_new(0);TASK_CHECK(d);TASK_CHECK(!mad_granule_feed(d,bad,len,&used) && used==len);
  TASK_CHECK(!mad_granule_finish(d));s=task_spectral_passes;t=task_synth_passes;
  TASK_CHECK(mad_granule_next(d,&pcm)==MAD_GRANULE_PCM && pcm.length==576);
  TASK_CHECK(task_spectral_passes==s+1 && task_synth_passes==t+1);
  for(i=0;i<576;++i)for(ch=0;ch<2;++ch)TASK_CHECK(pcm.samples[ch][i]==ref[ch][i]);
  TASK_CHECK(mad_granule_next(d,&pcm)==MAD_GRANULE_ERROR && pcm.length==0);
  TASK_CHECK(task_spectral_passes==s+2 && task_synth_passes==t+1);
  TASK_CHECK(!mad_granule_progress(d,&p) && p.error==MAD_ERROR_BADHUFFDATA && p.granules==1 && p.physical_frames==0 && p.sample_frames==576);
  TASK_CHECK(mad_granule_next(d,&pcm)==MAD_GRANULE_ERROR);
  TASK_CHECK(task_spectral_passes==s+2 && task_synth_passes==t+1);
  mad_granule_reset(d);TASK_CHECK(!mad_granule_progress(d,&p) && !p.granules && !p.sample_frames && !p.physical_frames && !p.buffered_bytes && !p.error);
  TASK_CHECK(mad_granule_next(d,&pcm)==MAD_GRANULE_NEED_INPUT);mad_granule_free(d);return 0;
}
static int lifecycle(void) {
  unsigned char bytes[TASK_MAX_BYTES];size_t len,used;unsigned i,ch;
  struct mad_granule_decoder *a,*b;struct mad_pcm pa,pb;struct mad_granule_progress p;
  uint64_t s,t;
  TASK_CHECK(mad_granule_new(0x40000000)==NULL);a=mad_granule_new(0);b=mad_granule_new(0);TASK_CHECK(a && b);
  TASK_CHECK(mad_granule_feed(a,NULL,1,&used)==-1);
  TASK_CHECK(mad_granule_progress(a,NULL)==-1 && mad_granule_next(a,NULL)==-1);
  TASK_CHECK(!mad_granule_progress(a,&p) && !p.granules && !p.error);
  s=task_spectral_passes;t=task_synth_passes;
  TASK_CHECK(mad_granule_next(a,&pa)==MAD_GRANULE_NEED_INPUT);
  TASK_CHECK(task_spectral_passes==s && task_synth_passes==t);
  TASK_CHECK(!mad_granule_finish(a) && !mad_granule_finish(a));TASK_CHECK(mad_granule_next(a,&pa)==MAD_GRANULE_END);
  TASK_CHECK(mad_granule_feed(a,NULL,0,&used)==-1);mad_granule_reset(a);
  len=task_fixture(bytes,0,0);TASK_CHECK(!mad_granule_feed(a,bytes,len,&used) && used==len);
  TASK_CHECK(!mad_granule_feed(b,bytes,len,&used) && used==len);TASK_CHECK(!mad_granule_finish(a) && !mad_granule_finish(b));
  for(i=0;i<5;++i) {
    unsigned j;TASK_CHECK(mad_granule_next(a,&pa)==MAD_GRANULE_PCM);
    TASK_CHECK(mad_granule_next(b,&pb)==MAD_GRANULE_PCM);
    TASK_CHECK(pa.length==576 && pb.length==576);
    for(ch=0;ch<2;++ch)for(j=0;j<576;++j)TASK_CHECK(pa.samples[ch][j]==pb.samples[ch][j]);
  }
  mad_granule_reset(a);TASK_CHECK(!mad_granule_feed(a,bytes,10,&used) && used==10);
  TASK_CHECK(!mad_granule_finish(a));TASK_CHECK(mad_granule_next(a,&pa)==MAD_GRANULE_ERROR);
  TASK_CHECK(!mad_granule_progress(a,&p) && p.error==MAD_ERROR_BUFLEN && !p.granules);
  mad_granule_free(a);mad_granule_free(b);mad_granule_reset(NULL);mad_granule_free(NULL);return 0;
}
const struct task_case task_cases[]={{"stream","pcm-and-fragments",streams},{"suspend","late-main-data-error",late_failure},{"lifecycle","reset-and-interleave",lifecycle}};
const size_t task_case_count=sizeof task_cases/sizeof task_cases[0];
