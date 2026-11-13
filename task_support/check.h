#ifndef TASK_CHECK_H
#define TASK_CHECK_H
void task_check(int,const char*,const char*,int);
#define TASK_CHECK(expr) task_check(!!(expr),#expr,__FILE__,__LINE__)
#endif
