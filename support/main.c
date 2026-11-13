#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t checks;
void unit_check(int ok,const char *text,const char *file,int line) {
 ++checks; if(!ok){fprintf(stderr,"FAIL %s:%d %s\n",file,line,text);exit(1);}
}
int main(void) {
 const char *groups[]={"normal","boundary","integration"}; size_t total=0;
 if(unit_case_count<6 || unit_case_count>10000)return 2;
 for(size_t i=0;i<unit_case_count;i++) {
  if(!unit_cases[i].name || !unit_cases[i].run || !unit_cases[i].group)return 2;
  for(size_t j=0;j<i;j++)if(!strcmp(unit_cases[i].name,unit_cases[j].name))return 2;
  int known=0;for(size_t g=0;g<3;g++)if(!strcmp(groups[g],unit_cases[i].group))known=1;
  if(!known)return 2;
 }
 for(size_t g=0;g<3;g++) {size_t cases=0,before=checks;
  for(size_t i=0;i<unit_case_count;i++)if(!strcmp(groups[g],unit_cases[i].group)) {
   size_t c=checks; unit_cases[i].run(); if(checks==c)return 3;
   printf("CASE %s %s %zu\n",groups[g],unit_cases[i].name,checks-c);++cases;++total;
  }
  if(cases<2)return 4;
  printf("GROUP %s %zu %zu\n",groups[g],cases,checks-before);
 }
 printf("TARGET_COMPLETE %zu %zu\n",total,checks);return 0;
}
