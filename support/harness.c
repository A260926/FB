#include "contract.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct {const char *name;void (*fn)(void);} cases[3];
static unsigned count,checks;
void contract_add(const char *n,void (*fn)(void)){if(count>=3||!fn){fputs("bad registration\n",stderr);exit(2);}cases[count].name=n;cases[count++].fn=fn;}
void contract_check(int ok,const char *f,int l){checks++;if(!ok){fprintf(stderr,"%s:%d check failed\n",f,l);exit(3);}}
int main(void){const char *groups[]={"vectors","binding","contexts"};unsigned i,j;register_contract();if(count!=3)return 4;
for(i=0;i<3;i++){for(j=0;j<count;j++)if(!strcmp(groups[i],cases[j].name))break;if(j==count)return 5;checks=0;cases[j].fn();if(!checks)return 6;printf("GROUP %s checks=%u\n",groups[i],checks);}
puts("CONTRACT_COMPLETE 3");return 0;}
