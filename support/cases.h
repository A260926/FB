#ifndef CHECK_CASES_H
#define CHECK_CASES_H
void task_register(const char *, const char *, void (*)(void));
void task_check(int, const char *, int);
#define TASK_CHECK(x) task_check(!!(x), #x, __LINE__)
#define TASK_CASE(group,name) static void name(void); static void __attribute__((constructor)) reg_##name(void){task_register(group,#name,name);} static void name(void)
#endif
