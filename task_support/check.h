#ifndef TASK_CHECK_H
#define TASK_CHECK_H
#include <stdio.h>
#include <stdlib.h>
extern unsigned task_assertions;
#define TASK_CHECK(x) do { ++task_assertions; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
struct task_case { const char *name; void (*run)(void); };
extern const struct task_case task_cases[];
extern const unsigned task_case_count;
#endif
