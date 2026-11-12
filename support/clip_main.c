/* Local fixed runner: no target geometry or reference answers. */
#include <stdio.h>
#include <string.h>
#include "clip_test.h"
struct ClipTest { size_t checks, failures; const char* name; };
void clip_check(ClipTest* t,int value,const char* expression,const char* file,int line) {
    t->checks++;
    if(!value) { t->failures++; fprintf(stderr,"FAIL %s %s:%d %s\n",t->name,file,line,expression); }
}
static int known(const char* s) {
    return s && (!strcmp(s,"geometry") || !strcmp(s,"scopes") || !strcmp(s,"coverage") || !strcmp(s,"reuse"));
}
int main(int argc,char** argv) {
    if(argc!=2 || !known(argv[1])) { fputs("expected one registered group\n",stderr); return 2; }
    if(!clip_case_count || clip_case_count>4096) { fputs("invalid case count\n",stderr); return 2; }
    for(size_t i=0;i<clip_case_count;i++) {
        if(!known(clip_cases[i].group) || !clip_cases[i].name || !*clip_cases[i].name || !clip_cases[i].run) return 2;
        for(size_t j=0;j<i;j++) if(!strcmp(clip_cases[i].name,clip_cases[j].name)) return 2;
    }
    size_t cases=0,checks=0,failures=0;
    for(size_t i=0;i<clip_case_count;i++) {
        if(strcmp(clip_cases[i].group,argv[1])) continue;
        ClipTest t={0,0,clip_cases[i].name};
        clip_cases[i].run(&t);
        if(!t.checks) { fprintf(stderr,"EMPTY_CASE %s\n",t.name); t.failures++; }
        printf("CASE %s/%s checks=%zu failures=%zu\n",argv[1],t.name,t.checks,t.failures);
        cases++; checks+=t.checks; failures+=t.failures;
    }
    if(!cases || !checks || failures) { fprintf(stderr,"GROUP_FAILED %s cases=%zu checks=%zu failures=%zu\n",argv[1],cases,checks,failures); return 1; }
    printf("CLIP_GROUP_OK %s cases=%zu checks=%zu\n",argv[1],cases,checks);
    return 0;
}
