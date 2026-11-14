#ifndef TASK_CHECK_H
#define TASK_CHECK_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int task_check(int value, const char *expr, const char *file, int line);
#define TASK_CHECK(x) do { if (!task_check(!!(x), #x, __FILE__, __LINE__)) return 1; } while (0)
struct task_case { const char *group; const char *name; int (*run)(void); };
extern const struct task_case task_cases[];
extern const size_t task_case_count;
#ifdef __cplusplus
}
#endif
#endif
