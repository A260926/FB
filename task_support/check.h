#ifndef UNIT_CHECK_H
#define UNIT_CHECK_H
#include <stdio.h>
#include <stdlib.h>
extern unsigned unit_checks;
#define UNIT_CHECK(x) do { ++unit_checks; if(!(x)) { fprintf(stderr,"check failed %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
typedef struct { const char *name; const char *group; void (*run)(void); } unit_case;
extern const unit_case unit_cases[];
extern const unsigned unit_case_count;
#endif
