#include "check.h"
#include <string.h>
unsigned task_assertions;
int main(void) {
 const char *required[] = {"anchors","wrap","overlap","recalibrate"};
 if (task_case_count < sizeof(required)/sizeof(required[0])) return 2;
 for (unsigned j=0;j<sizeof(required)/sizeof(required[0]);++j) {
  unsigned found=0;
  for (unsigned i=0;i<task_case_count;++i) if (!strcmp(task_cases[i].name,required[j])) {
   unsigned before=task_assertions; task_cases[i].run(); if (before==task_assertions) return 3; ++found;
  }
  if (!found) return 4;
  printf("PASS %s\n",required[j]);
 }
 printf("TARGET_OK assertions=%u\n",task_assertions);return 0;
}
