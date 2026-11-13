#include <stdio.h>
#include <stdlib.h>
#include "check.h"
static unsigned checks,total;
static const char *group;
void task_check(int ok,const char*expr,const char*file,int line){checks++;if(!ok){printf("CONTENT_DIFFERENCE %s\n%s:%d: %s\n",group,file,line,expr);fflush(stdout);exit(41);}}
int contract_finally(void);
int contract_catch(void);
int contract_abrupt(void);
int main(void){int n;
group="finally";checks=0;n=contract_finally();if(n<=0||checks==0)return 4;printf("GROUP finally %u DONE\n",checks);fflush(stdout);total+=checks;
group="catch";checks=0;n=contract_catch();if(n<=0||checks==0)return 4;printf("GROUP catch %u DONE\n",checks);fflush(stdout);total+=checks;
group="abrupt";checks=0;n=contract_abrupt();if(n<=0||checks==0)return 4;printf("GROUP abrupt %u DONE\n",checks);fflush(stdout);total+=checks;
printf("ALL_GROUPS_DONE %u\n",total);return 0;}
