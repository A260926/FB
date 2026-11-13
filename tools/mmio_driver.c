#include <stdio.h>
#include "mmio_case.h"
int main(void){const char *groups[]={"nested","split","pci","identity","gaps","ownership"};unsigned i;for(i=0;i<sizeof(groups)/sizeof(groups[0]);++i){unsigned checks=0;if(mmio_cases(groups[i],&checks)<0||!checks){fprintf(stderr,"failed or empty: %s\n",groups[i]);return 1;}printf("PASS %s checks=%u\n",groups[i],checks);}return 0;}
