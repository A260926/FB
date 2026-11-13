#include <stdio.h>
#include <stdlib.h>
#include "checks.h"
static unsigned long assertions;
static const char *active;
void task_check(int condition,const char *message) {
    if(!condition) {fprintf(stderr,"WV_PUSH_FAIL %s %s\n",active,message);exit(1);}
    assertions++;
}
int main(void) {
    const char *groups[]={"fragment","correction","flow","terminal"};
    setvbuf(stdout,NULL,_IONBF,0);
    for(unsigned i=0;i<sizeof(groups)/sizeof(groups[0]);++i) {
        active=groups[i]; assertions=0;
        printf("WV_PUSH_BEGIN %s\n",active);
        push_cases(active);
        if(!assertions) {fprintf(stderr,"WV_PUSH_EMPTY %s\n",active);return 2;}
        printf("WV_PUSH_PASS %s checks=%lu\n",active,assertions);
    }
    puts("WV_PUSH_COMPLETE groups=4");return 0;
}
