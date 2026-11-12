#include <cstdio>
#include "cases.h"
static int checks,failed;
int unit_check(int ok,const char *expr,const char *file,int line){
 ++checks;
 if(!ok){failed=1;fprintf(stderr,"%s:%d failed: %s\n",file,line,expr);}
 return ok;
}
extern int multiget_case(const char *);
int main(){
 const char *gs[]={"versions","routing","blocks","errors"};
 int total_cases=0,total_checks=0;
 for(auto g:gs){
  checks=failed=0;
  int n=multiget_case(g);
  if(n<=0||checks<=0||failed){fprintf(stderr,"empty/failed group %s\n",g);return 2;}
  printf("MULTIGET_GROUP %s PASS cases=%d checks=%d\n",g,n,checks);fflush(stdout);
  total_cases+=n;total_checks+=checks;
 }
 printf("MULTIGET_COMPLETE groups=4 cases=%d checks=%d\n",total_cases,total_checks);
 return 0;
}
