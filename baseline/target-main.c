#include <stdio.h>
int contract_call_graph(void);
int contract_frames(void);
int contract_stack_bounds(void);
int contract_helpers_jit(void);
int main(void)
{
    struct group { const char *name; int (*run)(void); } groups[]={
        {"call_graph",contract_call_graph},{"frames",contract_frames},
        {"stack_bounds",contract_stack_bounds},{"helpers_jit",contract_helpers_jit}};
    unsigned long total=0;
    for(unsigned i=0;i<sizeof(groups)/sizeof(groups[0]);i++) {
        int n=groups[i].run();
        if(n<=0) { fprintf(stderr,"FAIL %s nonpositive assertion count\n",groups[i].name); return 2; }
        total+=(unsigned)n;
        printf("PASS %s assertions=%d\n",groups[i].name,n);
    }
    printf("UBPF_LOCAL_CALLS_EXECUTED groups=4 assertions=%lu\n",total);
    return 0;
}
