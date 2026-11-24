#include "fuzz/ownership_replay.h"
#include <assert.h>
#include <stdio.h>
extern int LLVMFuzzerTestOneInput(const uint8_t*,size_t);
int main(void){
 const uint8_t plan[]={0,0,0,1,0,1,3,0,0,4,2,0,2,1,0,2,2,0,5,0,0};
 struct qjs_ownership_report r;
 assert(qjs_ownership_run(plan,sizeof plan,&r)==0);
 assert(r.steps==7&&r.created==1&&r.finalized==1&&r.comparisons>0&&r.exceptions==1);
 assert(LLVMFuzzerTestOneInput(plan,sizeof plan)==0);
 puts("QJS_API_CONSUMER_OK");return 0;
}
