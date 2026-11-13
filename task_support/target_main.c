#include "case_harness.h"
unsigned task_cases, task_checks;
int main(void) {
task_cases=task_checks=0; test_reorder(); if (!task_cases || !task_checks) return 90; printf("reorder cases=%u checks=%u\n",task_cases,task_checks);
task_cases=task_checks=0; test_audio_edges(); if (!task_cases || !task_checks) return 90; printf("audio_edges cases=%u checks=%u\n",task_cases,task_checks);
task_cases=task_checks=0; test_mapping(); if (!task_cases || !task_checks) return 90; printf("mapping cases=%u checks=%u\n",task_cases,task_checks);
task_cases=task_checks=0; test_reject(); if (!task_cases || !task_checks) return 90; printf("reject cases=%u checks=%u\n",task_cases,task_checks);
return 0; }
