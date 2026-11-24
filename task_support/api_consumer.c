#include "tests/fuzz/scope_replay.h"
#include <assert.h>
#include <stdio.h>
extern int LLVMFuzzerTestOneInput(const uint8_t*,size_t);
int main(void){
 const uint8_t plan[]={0,0,0,0,1,0,0,0,0,0,0,0,3,0,1,1,5,1,0,0,6,0,0,0,6,0,0,0,7,0,0,0};
 struct jerry_scope_report r;assert(jerry_scope_run(plan,sizeof plan,&r)==0);
 assert(r.steps==8&&r.created==1&&r.finalized==1&&r.comparisons>0&&r.opened==2&&r.escapes==1&&r.takes==1&&r.copies==0);
 assert(LLVMFuzzerTestOneInput(plan,sizeof plan)==0);puts("JERRY_API_CONSUMER_OK");return 0;
}
