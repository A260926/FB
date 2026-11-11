#include "CuTest.h"
#include <stdio.h>
CuSuite *get_rmf_suite(void);
int main(void) {
    CuSuite *s=get_rmf_suite(); CuString *o=CuStringNew(); int fail;
    CuSuiteRun(s); CuSuiteSummary(s,o); CuSuiteDetails(s,o); puts(o->buffer);
    fail=s->failCount; CuSuiteDelete(s); CuStringDelete(o);
    if(!fail) puts("RMF upstream selected suite PASS");
    return fail!=0;
}
