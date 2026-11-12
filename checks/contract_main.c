#include "contract.h"
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv){size_t n=0,i; int j;const struct contract_case *t=contract_cases(&n);
 if(!t||!n||n>100000){fputs("EMPTY_SUITE\n",stderr);return 2;}
 for(j=1;j<argc;j++){int found=0;for(i=0;i<n;i++)if(t[i].group&&!strcmp(argv[j],t[i].group))found++;if(!found){fprintf(stderr,"MISSING_GROUP %s\n",argv[j]);return 3;}}
 for(i=0;i<n;i++){if(!t[i].name||!t[i].run){return 4;}printf("RUN %s/%s\n",t[i].group,t[i].name);fflush(stdout);int r=t[i].run();if(r){fprintf(stderr,"FAIL %s/%s result=%d\n",t[i].group,t[i].name,r);return 5;}printf("PASS %s/%s\n",t[i].group,t[i].name);}
 printf("CONTRACT_OK cases=%zu\n",n);return 0;}
