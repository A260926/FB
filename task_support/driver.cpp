#include "check.h"
#include <string.h>
unsigned unit_checks;
static int fail(int code,const char *reason){fprintf(stderr,"TARGET_DRIVER_ERROR code=%d reason=%s\n",code,reason);return code;}
int main(void) {
 const char *groups[]={"curves","hierarchy","deformation"};
 unsigned i,j,k,used=0;
 if(unit_case_count<6) return fail(3,"less-than-six-registered-cases");
 for(i=0;i<unit_case_count;i++) { if(!unit_cases[i].name||!unit_cases[i].group||!unit_cases[i].run) return fail(4,"invalid-case");
  for(j=0;j<i;j++) if(!strcmp(unit_cases[i].name,unit_cases[j].name)) return fail(5,"duplicate-case-name"); }
 for(k=0;k<3;k++) { unsigned count=0,start=unit_checks;
  for(i=0;i<unit_case_count;i++) if(!strcmp(unit_cases[i].group,groups[k])) {
   unsigned before=unit_checks;printf("CASE_BEGIN %s\n",unit_cases[i].name);unit_cases[i].run();
   if(unit_checks==before)return fail(6,"case-without-assertions");
   printf("CASE_DONE %s checks=%u\n",unit_cases[i].name,unit_checks-before);count++;used++; }
  if(count<2||unit_checks==start)return fail(7,"missing-group-cases");
  printf("GROUP %s %u %u DONE\n",groups[k],count,unit_checks-start);
 }
 if(used!=unit_case_count)return fail(8,"unknown-case-group");
 printf("TARGET_DONE %u\n",unit_checks); return 0;
}
