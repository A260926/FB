#ifndef TASK_CASE_H
#define TASK_CASE_H
#include <stdio.h>
#include <stdlib.h>
extern unsigned task_cases, task_checks;
#define CASE() (++task_cases)
#define CHECK(x) do { ++task_checks; if (!(x)) { fprintf(stderr,"CHECK %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
void test_chunks(void);
void test_access_units(void);
void test_clock(void);
void test_invalid(void);
#endif
