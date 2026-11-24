#include "honggfuzz.h"
#include "libhfuzz/instrument.h"
#include <assert.h>
#include <stdio.h>
extern feedback_t *globalCovFeedback;
extern void __sanitizer_cov_8bit_counters_init(char*,char*);
int main(void){
 char a[2]={0},b[1]={0};
 __sanitizer_cov_8bit_counters_init(a,a+2);
 __sanitizer_cov_8bit_counters_init(b,b+1);
 a[0]=1;a[1]=3;b[0]=65;
 instrument8BitCountersCount();
 assert(a[0]==0 && a[1]==0 && b[0]==0);
 assert(globalCovFeedback->pcGuardMap[1]==1);
 assert(globalCovFeedback->pcGuardMap[2]==4);
 assert(globalCovFeedback->pcGuardMap[3]==128);
 assert(globalCovFeedback->pidTotalEdge[0].val==3);
 instrument8BitCountersCount();
 assert(globalCovFeedback->pidTotalEdge[0].val==3);
 puts("HF_LEGACY_MODULES buckets=1,4,128 cleared=3");
 puts("HF_LEGACY_RECOUNT total_edges=3 unchanged");
}
