#include "qoa.h"
#include "search_contract.h"
#include <stdio.h>
struct entry {const char *name;int (*run)(void);};
int main(void) {
    const struct entry cases[]={
        {"fixture_improves", search_fixture_improves},
        {"two_slice_oracle", search_two_slice_oracle},
        {"frame_snapshots", search_frame_snapshots},
        {"channels_tail", search_channels_tail},
        {"legacy_repeat", search_legacy_repeat},
        {"failure_atomicity", search_failure_atomicity}
    };
    unsigned count=sizeof(cases)/sizeof(cases[0]),passed=0;
    for(unsigned i=0;i<count;i++) {
        int code=cases[i].run();
        if(code) {fprintf(stderr,"QOA_SEARCH_CASE_FAIL name=%s exit=%d\n",cases[i].name,code);return 1;}
        passed++;
        printf("QOA_SEARCH_CASE_PASS name=%s\n",cases[i].name);
    }
    if(count!=6||passed!=count)return 1;
    printf("QOA_SEARCH_CONTRACT_OK cases=%u\n",passed);
    return 0;
}
