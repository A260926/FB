#ifndef CHECK_TEST_SUPPORT_H
#define CHECK_TEST_SUPPORT_H
#include <stdio.h>
#include <stdlib.h>
extern unsigned checks;
#define CHECK_CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#endif
