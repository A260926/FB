#ifndef TASK_CHECK_H
#define TASK_CHECK_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {const char *name; const char *group; void (*run)(void);} unit_case;
extern const unit_case unit_cases[];
extern const size_t unit_case_count;
void unit_check(int ok,const char *text,const char *file,int line);
#define UNIT_CHECK(x) unit_check(!!(x),#x,__FILE__,__LINE__)
#ifdef __cplusplus
}
#endif
#endif
