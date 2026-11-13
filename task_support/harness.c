#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t checks=0;
void unit_check(int value,const char *expr,const char *file,int line){checks++;if(!value){fprintf(stderr,"FAIL %s:%d %s\n",file,line,expr);exit(1);}}
int main(void){
 const char *groups[]={"prefix","tuple","syntax"};
 size_t counts[sizeof(groups)/sizeof(groups[0])];memset(counts,0,sizeof counts);
 if(!unit_case_count)return 2;
 for(size_t i=0;i<unit_case_count;i++){
  size_t g;for(g=0;g<sizeof(groups)/sizeof(groups[0]);g++)if(!strcmp(groups[g],unit_cases[i].group))break;
  if(g==sizeof(groups)/sizeof(groups[0])||!unit_cases[i].run)return 3;
  size_t before=checks;unit_cases[i].run();if(checks==before)return 4;counts[g]++;
  printf("CASE_PASS %s %s checks=%zu\n",groups[g],unit_cases[i].name,checks-before);
 }
 for(size_t g=0;g<sizeof(groups)/sizeof(groups[0]);g++){if(!counts[g])return 5;printf("GROUP_COMPLETE %s cases=%zu\n",groups[g],counts[g]);}
 printf("UNIT_ALL_COMPLETE cases=%zu checks=%zu\n",unit_case_count,checks);return 0;
}
