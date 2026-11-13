#include "check.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned checks;
void task_check(int ok,const char *m){checks++;if(!ok){fprintf(stderr,"ASSERT %s\n",m);exit(42);}}
int main(void){
checks=0;if(task_cases("samples")!=1 || !checks)return 43;printf("GROUP samples %u COMPLETE\n",checks);
checks=0;if(task_cases("stereo")!=1 || !checks)return 43;printf("GROUP stereo %u COMPLETE\n",checks);
checks=0;if(task_cases("stream")!=1 || !checks)return 43;printf("GROUP stream %u COMPLETE\n",checks);
puts("FOXEN_32_OK");return 0;}
