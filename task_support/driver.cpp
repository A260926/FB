#include "check.h"
#include <cstring>
int main() {
    const char *groups[]={"residual","ordered","scene"};
    unsigned counts[3]={};
    if (task_case_count<6 || task_case_count>256) return 2;
    for (unsigned i=0;i<task_case_count;i++) {
        const task_case &c=task_cases[i];
        if (!c.name||!c.name[0]||!c.group||!c.run) return 2;
        for(unsigned j=0;j<i;j++) if(!std::strcmp(c.name,task_cases[j].name)) return 2;
        int g=-1; for(int j=0;j<3;j++) if(!std::strcmp(c.group,groups[j])) g=j;
        if(g<0) return 2;
        unsigned before=task_assertions; c.run();
        if(task_assertions==before) { std::fprintf(stderr,"EMPTY_CASE %s\n",c.name);return 2; }
        counts[g]++; std::printf("CASE_OK %s/%s\n",c.group,c.name);
    }
    for(int j=0;j<3;j++) if(counts[j]<2) return 2;
    std::printf("LINK_MODES_TARGET_OK cases=%u assertions=%u\n",task_case_count,task_assertions);
}
