#ifndef UNIT_SUITE_H
#define UNIT_SUITE_H
#include <stddef.h>
typedef void (*unit_fn)(void);
void unit_register(const char*, const char*, unit_fn);
void unit_check(int, const char*, int);
void unit_skip(const char*);
#define UNIT_JOIN_(a,b) a##b
#define UNIT_JOIN(a,b) UNIT_JOIN_(a,b)
#define UNIT_TEST(group,name) \
  static void UNIT_JOIN(unit_case_,name)(void); \
  static void __attribute__((constructor)) UNIT_JOIN(unit_register_,name)(void) { \
    unit_register(#group, #name, UNIT_JOIN(unit_case_,name)); \
  } \
  static void UNIT_JOIN(unit_case_,name)(void)
#define UNIT_CHECK(x) unit_check(!!(x),__FILE__,__LINE__)
#define UNIT_SKIP(reason) do { unit_skip(reason); return; } while (0)
#endif
