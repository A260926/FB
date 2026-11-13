#ifndef UNIT_CHECK_H
#define UNIT_CHECK_H
#include <stddef.h>
void unit_check(int value,const char *expr,const char *file,int line);
#define UNIT_CHECK(expr) unit_check(!!(expr),#expr,__FILE__,__LINE__)
typedef struct { const char *group; const char *name; void (*run)(void); } unit_case;
extern const unit_case unit_cases[];
extern const size_t unit_case_count;
#endif
