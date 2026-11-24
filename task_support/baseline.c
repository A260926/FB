#include "task_support/fixture.h"
#include "stream.h"
#include "frame.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"baseline line=%d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
  unsigned char bytes[TASK_MAX_BYTES],bad[TASK_MAX_BYTES];
  static mad_fixed_t ref[2][TASK_MAX_SAMPLES];
  unsigned lsf,half,n,i,nonzero,asym;uint64_t hash;size_t len;
  for(lsf=0;lsf<2;++lsf) for(half=0;half<2;++half) {
    len=task_fixture(bytes,lsf,0);
    CHECK(task_reference(bytes,len,half?MAD_OPTION_HALFSAMPLERATE:0,ref,&n,&hash)==0);
    CHECK(n==TASK_FRAMES*(lsf?576:1152)/(half?2:1));
    nonzero=asym=0;
    for(i=0;i<n;++i){nonzero+=!!ref[0][i]+!!ref[1][i];asym+=ref[0][i]!=ref[1][i];}
    CHECK(nonzero>n && asym>0);
    printf("MAD_NORMAL lsf=%u half=%u frames=8 samples=%u nonzero=%u asymmetric=%u hash=%016llx\n",lsf,half,n,nonzero,asym,(unsigned long long)hash);
  }
  {
    struct mad_stream a,b;struct mad_frame good,broken;
    len=task_fixture(bytes,0,0);CHECK(task_fixture(bad,0,1)==len);
    mad_stream_init(&a);mad_stream_init(&b);mad_frame_init(&good);mad_frame_init(&broken);
    mad_stream_buffer(&a,bytes,len+MAD_BUFFER_GUARD);mad_stream_buffer(&b,bad,len+MAD_BUFFER_GUARD);
    CHECK(mad_frame_decode(&good,&a)==0);
    CHECK(mad_frame_decode(&broken,&b)==-1 && b.error==MAD_ERROR_BADHUFFDATA);
    CHECK(memcmp(good.sbsample[0],broken.sbsample[0],18*32*sizeof(mad_fixed_t))==0);
    CHECK(memcmp(good.sbsample[1],broken.sbsample[1],18*32*sizeof(mad_fixed_t))==0);
    printf("MAD_LATE_ERROR code=%04x first_granule_subbands_equal=1 whole_frame_rejected=1\n",b.error);
    mad_frame_finish(&good);mad_frame_finish(&broken);mad_stream_finish(&a);mad_stream_finish(&b);
  }
  puts("MAD_GRANULE_BASELINE_OK ordinary_configs=4 late_error=1");return 0;
}
