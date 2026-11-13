#include "cases.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t checks;
void task_check(int ok,const char *expr,const char *file,int line) {
 checks++;if(!ok){fprintf(stderr,"CHECK_FAILED %s:%d %s\n",file,line,expr);exit(1);}
}
int main(void) {
 const char *groups[]={"recovery","prediction","compat"};size_t total=0;
 setvbuf(stdout,NULL,_IONBF,0);
 if(task_case_count==0||task_case_count>10000)return 2;
 for(size_t i=0;i<task_case_count;i++) {
  if(!task_cases[i].group||!task_cases[i].name||!task_cases[i].run)return 2;
  if(strcmp(task_cases[i].group,groups[0])&&strcmp(task_cases[i].group,groups[1])&&strcmp(task_cases[i].group,groups[2]))return 2;
 }
 for(size_t g=0;g<3;g++) {
  size_t n=0,before=checks;
  for(size_t i=0;i<task_case_count;i++)if(!strcmp(task_cases[i].group,groups[g])) {
   size_t start=checks;task_cases[i].run();if(checks==start)return 3;n++;
  }
  if(!n)return 4;total+=n;
  printf("GROUP %s cases=%zu checks=%zu\n",groups[g],n,checks-before);
 }
 printf("COMPLETE groups=3 cases=%zu checks=%zu\n",total,checks);
 return 0;
}
