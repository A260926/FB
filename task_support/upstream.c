#include "CuTest.h"
#include <stdio.h>
CuSuite *get_interpolation_suite(void);
int main(void){CuSuite *s=get_interpolation_suite();CuString *o=CuStringNew();CuSuiteRun(s);CuSuiteSummary(s,o);CuSuiteDetails(s,o);printf("%s",o->buffer);int fail=s->failCount;printf("UPSTREAM_INTERPOLATION cases=%d failed=%d\n",s->count,fail);CuStringDelete(o);CuSuiteDelete(s);return fail?1:0;}
