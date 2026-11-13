#include <stdio.h>
#include <stdlib.h>
#include "check.h"
static unsigned checks,total;
static const char *group;
void task_check(int ok,const char*expr,const char*file,int line){checks++;if(!ok){printf("CONTENT_DIFFERENCE %s\n%s:%d: %s\n",group,file,line,expr);fflush(stdout);exit(41);}}
int contract_model(void);
int contract_resume(void);
int contract_lifetime(void);
int main(void){int n;
group="model";checks=0;n=contract_model();if(n<=0||checks==0)return 4;printf("GROUP model %u DONE\n",checks);fflush(stdout);total+=checks;
group="resume";checks=0;n=contract_resume();if(n<=0||checks==0)return 4;printf("GROUP resume %u DONE\n",checks);fflush(stdout);total+=checks;
group="lifetime";checks=0;n=contract_lifetime();if(n<=0||checks==0)return 4;printf("GROUP lifetime %u DONE\n",checks);fflush(stdout);total+=checks;
printf("ALL_GROUPS_DONE %u\n",total);return 0;}
