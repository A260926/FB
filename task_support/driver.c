#include "check.h"
unsigned task_assertions=0;
int run_cut_plan_tests(const char* group);
int main(void){const char* g[]={"planner","pcm","lifecycle"};
for(unsigned i=0;i<3;i++){unsigned n=task_assertions;if(run_cut_plan_tests(g[i])||task_assertions-n<3)return 1;printf("GROUP %s assertions=%u complete\n",g[i],task_assertions-n);}
puts("SOX_CUT_PLAN_COMPLETE");return 0;}
