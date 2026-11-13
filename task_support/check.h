#ifndef TASK_SUPPORT_CHECK_H
#define TASK_SUPPORT_CHECK_H
#include <stdio.h>
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned task_assertions;
#ifdef __cplusplus
}
#endif
#define TASK_CHECK(expr) do { ++task_assertions; if (!(expr)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#expr); exit(1); } } while(0)
#endif
