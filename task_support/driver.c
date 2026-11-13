#include <stdio.h>
#include <stdlib.h>
#include "check.h"
static unsigned checks,total;
static const char *group;
void task_check(int ok,const char*expr,const char*file,int line){checks++;if(!ok){printf("CONTENT_DIFFERENCE %s\n%s:%d: %s\n",group,file,line,expr);fflush(stdout);exit(41);}}
int contract_joint(void);
int contract_ownership(void);
int contract_compat(void);
int main(void){int n;
group="joint";checks=0;n=contract_joint();if(n<=0||checks==0)return 4;printf("GROUP joint %u DONE\n",checks);fflush(stdout);total+=checks;
group="ownership";checks=0;n=contract_ownership();if(n<=0||checks==0)return 4;printf("GROUP ownership %u DONE\n",checks);fflush(stdout);total+=checks;
group="compat";checks=0;n=contract_compat();if(n<=0||checks==0)return 4;printf("GROUP compat %u DONE\n",checks);fflush(stdout);total+=checks;
printf("ALL_GROUPS_DONE %u\n",total);return 0;}
