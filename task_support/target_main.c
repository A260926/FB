#include "case_harness.h"
unsigned task_cases, task_checks;
int main(void) {
task_cases=task_checks=0; test_sps(); if (!task_cases || !task_checks) return 90; printf("sps cases=%u checks=%u\n",task_cases,task_checks);
task_cases=task_checks=0; test_schedule(); if (!task_cases || !task_checks) return 90; printf("schedule cases=%u checks=%u\n",task_cases,task_checks);
task_cases=task_checks=0; test_callbacks(); if (!task_cases || !task_checks) return 90; printf("callbacks cases=%u checks=%u\n",task_cases,task_checks);
task_cases=task_checks=0; test_compatibility(); if (!task_cases || !task_checks) return 90; printf("compatibility cases=%u checks=%u\n",task_cases,task_checks);
return 0; }
