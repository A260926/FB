#ifndef MMIO_CASE_H
#define MMIO_CASE_H
#include <stdio.h>
#define MMIO_CHECK(expr) do { if (!(expr)) { fprintf(stderr,"check failed %s:%d: %s\n",__FILE__,__LINE__,#expr);return -1; } ++*checks; } while (0)
int mmio_cases(const char *group, unsigned *checks);
#endif
