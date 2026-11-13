#ifndef TASK_CASE_H
#define TASK_CASE_H
#include <stdio.h>
#include <stdlib.h>
extern unsigned task_cases, task_checks;
#define CASE() (++task_cases)
#define CHECK(x) do { ++task_checks; if (!(x)) { fprintf(stderr,"CHECK %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
void test_reorder(void);
void test_audio_edges(void);
void test_mapping(void);
void test_reject(void);
#endif
