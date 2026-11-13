#include "case_harness.h"
unsigned task_cases, task_checks;
int main(void) {
task_cases=task_checks=0; test_packet_binding(); if (!task_cases || !task_checks) return 90; printf("packet_binding cases=%u checks=%u\n",task_cases,task_checks);
task_cases=task_checks=0; test_reordered_frames(); if (!task_cases || !task_checks) return 90; printf("reordered_frames cases=%u checks=%u\n",task_cases,task_checks);
task_cases=task_checks=0; test_lifecycle(); if (!task_cases || !task_checks) return 90; printf("lifecycle cases=%u checks=%u\n",task_cases,task_checks);
task_cases=task_checks=0; test_compatibility(); if (!task_cases || !task_checks) return 90; printf("compatibility cases=%u checks=%u\n",task_cases,task_checks);
return 0; }
