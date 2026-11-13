#include "check.h"
unsigned task_assertions=0;
int task_cases(const char*);
int main(void){const char* groups[]={"model","metamorphic","replay"};for(int i=0;i<3;i++){unsigned before=task_assertions;if(task_cases(groups[i])||task_assertions-before<5)return 1;printf("GROUP %s assertions=%u complete\n",groups[i],task_assertions-before);}puts("TARGET_COMPLETE");return 0;}
