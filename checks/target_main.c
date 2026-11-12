#include <stdio.h>
#include <stdlib.h>
#include "suite.h"
static int assertions;
void unit_expect(int ok,const char *expr,const char *file,int line){if(!ok){fprintf(stderr,"ASSERTION_FAILED %s:%d %s\n",file,line,expr);exit(1);}assertions++;}
int unit_run_group(const char *);
int main(void){const char *g[]={"lowering","control","diagnostics"};int total=0;for(unsigned i=0;i<sizeof(g)/sizeof(*g);i++){assertions=0;int n=unit_run_group(g[i]);if(n<=0 || assertions<=0 || n!=assertions){fprintf(stderr,"EMPTY_OR_FAILED %s claimed=%d actual=%d\n",g[i],n,assertions);return 1;}total+=assertions;printf("GROUP_OK %s %d\n",g[i],assertions);}printf("TARGET_OK %d\n",total);return 0;}
