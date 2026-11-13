/* Original task support, CC0-1.0. */
#ifndef TASK_CHECK_H
#define TASK_CHECK_H
void task_check_impl(int ok,const char *expression,const char *file,int line);
#define TASK_CHECK(x) task_check_impl(!!(x),#x,__FILE__,__LINE__)
/* Implement in task_support/target_cases.c; return 1 only after completing group. */
int task_cases(const char *group);
#endif
