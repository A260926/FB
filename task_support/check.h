#pragma once
#include <cstdio>
#include <cstdlib>
#include <cmath>
inline unsigned task_assertions=0;
inline void task_check(bool value,const char *text,int line) {
    ++task_assertions;
    if (!value) { std::fprintf(stderr,"LINK_MODE_ASSERT line=%d %s\n",line,text); std::exit(1); }
}
#define TASK_CHECK(expr) task_check(bool(expr),#expr,__LINE__)
struct task_case { const char *group; const char *name; void (*run)(); };
extern const task_case task_cases[];
extern const unsigned task_case_count;
