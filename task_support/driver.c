#include "check.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned checks;
void task_check(int ok,const char *m){checks++;if(!ok){fprintf(stderr,"ASSERT %s\n",m);exit(42);}}
int main(void){
checks=0;if(task_cases("quadrants")!=1 || !checks)return 43;printf("GROUP quadrants %u COMPLETE\n",checks);
checks=0;if(task_cases("chain")!=1 || !checks)return 43;printf("GROUP chain %u COMPLETE\n",checks);
checks=0;if(task_cases("packets")!=1 || !checks)return 43;printf("GROUP packets %u COMPLETE\n",checks);
puts("VORBIS_COUPLING_OK");return 0;}
