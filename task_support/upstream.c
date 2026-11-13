#include "testutils.h"
CuSuite *get_eval_suite(void), *get_to_beziers_suite(void), *get_insert_knot_suite(void), *get_derive_suite(void);
int main(void) {
    CuSuite *suites[4]; const char *names[4]={"eval","to_beziers","insert_knot","derive"};
    int i, n=0, fails=0;
    suites[0]=get_eval_suite(); suites[1]=get_to_beziers_suite();
    suites[2]=get_insert_knot_suite(); suites[3]=get_derive_suite();
    for(i=0;i<4;++i){
        CuString *out=CuStringNew();
        if(suites[i]->count<=0)return 2;
        CuSuiteRun(suites[i]); CuSuiteDetails(suites[i],out);
        printf("UPSTREAM %s cases=%d failed=%d\n%s",names[i],suites[i]->count,suites[i]->failCount,out->buffer);
        n+=suites[i]->count; fails+=suites[i]->failCount;
        CuStringDelete(out); CuSuiteDelete(suites[i]);
    }
    if(fails)return 1;
    printf("UPSTREAM_SELECTED_OK cases=%d\n",n); return 0;
}
