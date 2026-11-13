/* Original task support, CC0-1.0. This file contains no decoder assertions. */
#include "check.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned checks;
void task_check_impl(int ok,const char *expression,const char *file,int line) {
    checks++;
    if(!ok) {fprintf(stderr,"CHECK_FAILED %s:%d %s\n",file,line,expression);exit(41);}
}
int main(void) {
    const char *groups[]={"lists","prediction","filter"};
    for(unsigned i=0;i<3;i++) {
        checks=0;
        int completed=task_cases(groups[i]);
        if(completed!=1 || checks==0) {fprintf(stderr,"INCOMPLETE %s checks=%u return=%d\n",groups[i],checks,completed);return 43;}
        printf("GROUP %s checks=%u COMPLETE\n",groups[i],checks);
        fflush(stdout);
    }
    puts("H264BSD_B_EXPLICIT_OK");
    return 0;
}
