#include <stdio.h>
#include <string.h>
#include "marker_test.h"
void marker_check_(MarkerTest* ctx,int ok,const char* expr,const char* file,int line) {
 ctx->checks++;if(!ok){ctx->failures++;fprintf(stderr,"%s:%d: %s\n",file,line,expr);}
}
int main(void){
 const char* groups[]={"topology","coordinates","paint","reuse"};
 size_t g,i,j,all=0;int bad=0;
 if(marker_case_count==0 || marker_case_count>10000) return 3;
 for(i=0;i<marker_case_count;i++){
  int known=0; if(!marker_cases[i].group || !marker_cases[i].name || !marker_cases[i].run)return 3;
  for(g=0;g<4;g++) known|=strcmp(marker_cases[i].group,groups[g])==0;
  if(!known)return 3;
  for(j=0;j<i;j++)if(!strcmp(marker_cases[i].group,marker_cases[j].group) && !strcmp(marker_cases[i].name,marker_cases[j].name))return 3;
 }
 for(g=0;g<4;g++){
  size_t cases=0,checks=0,failures=0;
  for(i=0;i<marker_case_count;i++)if(!strcmp(marker_cases[i].group,groups[g])){
   MarkerTest ctx={0,0};marker_cases[i].run(&ctx);cases++;checks+=ctx.checks;failures+=ctx.failures;
   if(!ctx.checks){fprintf(stderr,"empty case: %s/%s\n",groups[g],marker_cases[i].name);bad=1;}
  }
  if(!cases || !checks || failures)bad=1;
  printf("MARKER_GROUP %s cases=%zu checks=%zu failures=%zu DONE\n",groups[g],cases,checks,failures);fflush(stdout);all+=checks;
 }
 if(bad)return 1;
 printf("MARKER_ALL_DONE checks=%zu\n",all);return 0;
}
