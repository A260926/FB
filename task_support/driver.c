#include "check.h"
unsigned task_assertions=0;
int run_gain_layers_tests(const char* group);
int main(void) {
const char* groups[]={
    "scope",
    "curves",
    "lifetime",
};
for (unsigned i=0;i<sizeof(groups)/sizeof(groups[0]);++i) {
unsigned before=task_assertions;
if (run_gain_layers_tests(groups[i])!=0 || task_assertions-before<3) return 1;
printf("GROUP %s assertions=%u complete\n",groups[i],task_assertions-before);
}
puts("TARGET_COMPLETE");
return 0;
}
