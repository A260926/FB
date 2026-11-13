#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cases.h"
struct entry {const char *g,*n; void (*f)(void);} static entries[1024];
static unsigned count,checks;
void task_register(const char*g,const char*n,void(*f)(void)){if(count==1024)exit(43);entries[count++]=(struct entry){g,n,f};}
void task_check(int ok,const char*x,int line){++checks;if(!ok){fprintf(stderr,"ASSERT line=%d %s\n",line,x);exit(42);}}
int main(void){
 const char *groups[]={"windows","bitstream","state"};
 for(unsigned g=0;g<sizeof(groups)/sizeof(*groups);g++){
 unsigned n=0,c=checks;
 for(unsigned i=0;i<count;i++)if(!strcmp(groups[g],entries[i].g)){
  for(unsigned j=0;j<i;j++)if(!strcmp(entries[j].n,entries[i].n))return 44;
  unsigned before=checks;entries[i].f();if(checks==before)return 45;
  ++n;printf("CASE_PASS %s %s checks=%u\n",groups[g],entries[i].n,checks-before);
 }
 if(!n||checks==c)return 46;
 printf("GROUP_PASS %s cases=%u checks=%u\n",groups[g],n,checks-c);
 }
 printf("TARGET_COMPLETE cases=%u checks=%u\n",count,checks);return 0;}
