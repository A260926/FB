/* Fixed runner: rejects no registration, missing groups, skipped cases,
 * assertion-free cases and premature exit(0). CC0-1.0. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "contract.h"
static size_t assertions, failures;
static int finished;
static void at_end(void) {
    if (!finished) { fputs("incomplete contract run\n",stderr); _Exit(97); }
}
void nc_check(int truth,const char *message,const char *file,int line) {
    assertions++;
    if (!truth) { failures++; fprintf(stderr,"ASSERT %s:%d %s\n",file,line,message); }
}
int main(void) {
    const char *groups[]={"der","chain","workspace"};
    size_t counts[3]={0,0,0},n=0,i,j;
    const nc_case *v;
    if (atexit(at_end)) return 2;
    v=nc_contract_cases(&n);
    if (!v || !n || n>10000) { fputs("empty/invalid registration\n",stderr); return 2; }
    for(i=0;i<n;i++) {
        size_t before=assertions, prior=failures;
        int group=-1, r;
        if(!v[i].name || !*v[i].name || !v[i].group || !v[i].run) return 2;
        for(j=0;j<i;j++) if(!strcmp(v[i].name,v[j].name)) return 2;
        for(j=0;j<3;j++) if(!strcmp(v[i].group,groups[j])) group=(int)j;
        if(group<0) return 2;
        r=v[i].run();
        if(r || failures!=prior || assertions==before) {
            fprintf(stderr,"FAIL %s return=%d assertions=%lu\n",v[i].name,r,(unsigned long)(assertions-before));
            return 1;
        }
        counts[group]++;
        printf("PASS %s/%s assertions=%lu\n",v[i].group,v[i].name,(unsigned long)(assertions-before));
    }
    for(j=0;j<3;j++) if(!counts[j]) { fprintf(stderr,"missing group %s\n",groups[j]);return 2; }
    finished=1;
    printf("NC_CONTRACT_COMPLETE cases=%lu assertions=%lu der=%lu chain=%lu workspace=%lu\n",
        (unsigned long)n,(unsigned long)assertions,(unsigned long)counts[0],(unsigned long)counts[1],(unsigned long)counts[2]);
    return 0;
}
