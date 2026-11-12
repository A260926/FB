#include "section-qa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct SectionQARegistry {
  struct { const char *group, *name; SectionQACase run; } cases[256];
  unsigned count;
};
static const char *groups[]={"topology","paired-curves","analytic-coverage"};
static int group_index(const char *s) {
  if(s) for(int i=0;i<3;i++) if(!strcmp(s,groups[i])) return i;
  return -1;
}
void section_qa_add(SectionQARegistry *r,const char *group,
                    const char *name,SectionQACase run) {
  if(!r||r->count>=256||group_index(group)<0||!name||!*name||!run) {
    fputs("QA_REGISTRY_INVALID\n",stderr);exit(65);
  }
  for(unsigned i=0;i<r->count;i++)
    if(!strcmp(r->cases[i].group,group)&&!strcmp(r->cases[i].name,name)) {
      fputs("QA_REGISTRY_DUPLICATE\n",stderr);exit(65);
    }
  r->cases[r->count].group=group;r->cases[r->count].name=name;
  r->cases[r->count++].run=run;
}
int main(void) {
  SectionQARegistry r={0};unsigned registered[3]={0},passed[3]={0};
  section_qa_register(&r);
  for(unsigned i=0;i<r.count;i++) registered[group_index(r.cases[i].group)]++;
  for(int g=0;g<3;g++) if(!registered[g]) {
    fprintf(stderr,"QA_NO_CASES group=%s registered=%u\n",groups[g],registered[g]);return 66;
  }
  for(unsigned i=0;i<r.count;i++) {
    printf("RUN %s %s\n",r.cases[i].group,r.cases[i].name);fflush(stdout);
    int rc=r.cases[i].run();
    if(rc) {
      fprintf(stderr,"QA_CASE_FAILED group=%s name=%s result=%d%s\n",
              r.cases[i].group,r.cases[i].name,rc,rc==77?" SKIP_REJECTED":"");return 67;
    }
    passed[group_index(r.cases[i].group)]++;
    printf("OK %s %s\n",r.cases[i].group,r.cases[i].name);
  }
  for(int g=0;g<3;g++) printf("PASS %s cases=%u\n",groups[g],passed[g]);
  printf("SECTION_QA_EXECUTED cases=%u\n",r.count);return 0;
}
