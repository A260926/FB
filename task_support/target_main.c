#include "case_harness.h"
unsigned task_cases, task_checks;
int main(void) {
task_cases=task_checks=0; test_chunks(); if (!task_cases || !task_checks) return 90; printf("chunks cases=%u checks=%u\n",task_cases,task_checks);
task_cases=task_checks=0; test_access_units(); if (!task_cases || !task_checks) return 90; printf("access_units cases=%u checks=%u\n",task_cases,task_checks);
task_cases=task_checks=0; test_clock(); if (!task_cases || !task_checks) return 90; printf("clock cases=%u checks=%u\n",task_cases,task_checks);
task_cases=task_checks=0; test_invalid(); if (!task_cases || !task_checks) return 90; printf("invalid cases=%u checks=%u\n",task_cases,task_checks);
return 0; }
