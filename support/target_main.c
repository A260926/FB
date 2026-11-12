#include <stdio.h>
#include <stdlib.h>
#include "check.h"
static unsigned checks;
void unit_check(int ok,const char *label){ if(!ok){fprintf(stderr,"ASSERT %s\n",label);exit(42);}checks++;}
int main(void){
 void (*groups[])(void)={test_optimality,test_composite,test_legacy};
 const char *names[]={"optimality","composite","legacy"};
 for(unsigned g=0;g<3;g++){unsigned before=checks;groups[g]();if(checks==before){fprintf(stderr,"EMPTY %s\n",names[g]);return 43;}printf("GROUP %s checks=%u\n",names[g],checks-before);}
 printf("GIF_PLAN_OK checks=%u\n",checks);return 0;
}
