#ifndef REFRESH_CASES_H
#define REFRESH_CASES_H
#include <stddef.h>
struct task_case { const char *group; const char *name; void (*run)(void); };
extern const struct task_case task_cases[];
extern const size_t task_case_count;
void task_check(int ok, const char *expr, const char *file, int line);
#define TASK_CHECK(expr) task_check(!!(expr), #expr, __FILE__, __LINE__)
#endif
