#include <stdio.h>
#include <stdlib.h>
#include "check.h"
static unsigned checks,total;
static const char *group;
void task_check(int ok,const char*expr,const char*file,int line){checks++;if(!ok){printf("CONTENT_DIFFERENCE %s\n%s:%d: %s\n",group,file,line,expr);fflush(stdout);exit(41);}}
int contract_relocations(void);
int contract_dwarf(void);
int contract_ownership(void);
int main(void){int n;
group="relocations";checks=0;n=contract_relocations();if(n<=0||checks==0)return 4;printf("GROUP relocations %u DONE\n",checks);fflush(stdout);total+=checks;
group="dwarf";checks=0;n=contract_dwarf();if(n<=0||checks==0)return 4;printf("GROUP dwarf %u DONE\n",checks);fflush(stdout);total+=checks;
group="ownership";checks=0;n=contract_ownership();if(n<=0||checks==0)return 4;printf("GROUP ownership %u DONE\n",checks);fflush(stdout);total+=checks;
printf("ALL_GROUPS_DONE %u\n",total);return 0;}
