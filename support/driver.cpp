#include "check.h"
#include <string.h>
unsigned unit_checks;
int main(void) {
 const char *groups[]={"normal","boundary","integration"};
 unsigned i,j,k,used=0;
 if(unit_case_count<6) return 3;
 for(i=0;i<unit_case_count;i++) { if(!unit_cases[i].name||!unit_cases[i].run) return 4;
  for(j=0;j<i;j++) if(!strcmp(unit_cases[i].name,unit_cases[j].name)) return 5; }
 for(k=0;k<3;k++) { unsigned count=0,start=unit_checks;
  for(i=0;i<unit_case_count;i++) if(!strcmp(unit_cases[i].group,groups[k])) {
   unsigned before=unit_checks;unit_cases[i].run(); if(unit_checks==before)return 6;count++;used++; }
  if(count<2||unit_checks==start)return 7;
  printf("GROUP %s %u %u DONE\n",groups[k],count,unit_checks-start);
 }
 if(used!=unit_case_count)return 8;
 printf("TARGET_DONE %u\n",unit_checks); return 0;
}
