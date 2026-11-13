#include <stdio.h>
#include <stdlib.h>
#include "check.h"
void contract_dma(void);
void contract_funk(void);
void contract_instances(void);
void contract_compatibility(void);
static unsigned checks;
void contract_check(int ok,const char *expr,const char *file,int line) {
    ++checks;
    if(!ok) { fprintf(stderr,"FAIL %s:%d %s\n",file,line,expr); exit(1); }
}
static void run(const char *name,void (*fn)(void)) {
    checks=0; fn();
    if(!checks) { fprintf(stderr,"NO_CHECKS %s\n",name);exit(2); }
    printf("GROUP %s CHECKS %u COMPLETE\n",name,checks); fflush(stdout);
}
int main(void) {
    run("dma",contract_dma); run("funk",contract_funk);
    run("instances",contract_instances); run("compatibility",contract_compatibility);
    puts("MICROMOD_TARGET_COMPLETE groups=4"); return 0;
}
