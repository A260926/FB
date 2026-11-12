#include <stdio.h>
#include "cases.h"
static int checks,failed;
int unit_check(int ok,const char *expr,const char *file,int line){checks++;if(!ok){failed=1;fprintf(stderr,"%s:%d failed: %s\n",file,line,expr);}return ok;}
extern int range_case(const char *);
int main(void){const char *gs[]={"bounds","pages","cursors"};for(int i=0;i<3;i++){checks=failed=0;int n=range_case(gs[i]);if(n<=0||checks<=0||failed){fprintf(stderr,"empty/failed group %s\n",gs[i]);return 2;}printf("RANGE_GROUP %s PASS cases=%d checks=%d\n",gs[i],n,checks);}return 0;}
