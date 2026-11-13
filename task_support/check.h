#ifndef TASK_CHECK_H
#define TASK_CHECK_H
#include <stdio.h>
#include <stdlib.h>
void task_case(const char *name);
void task_check(int ok,const char *expr,const char *file,int line);
#define CASE(name) task_case(name)
#define CHECK(expr) task_check(!!(expr),#expr,__FILE__,__LINE__)
#define REQUIRE(expr) do { if(!(expr)) { fprintf(stderr,"REQUIRE %s:%d %s\n",__FILE__,__LINE__,#expr); exit(77); } } while(0)
#endif
