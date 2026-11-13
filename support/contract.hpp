#pragma once
#include <cstddef>
#include <cstdio>
#include <cstdlib>
struct unit_case {const char *group;const char *name;void (*run)();};
extern unit_case unit_cases[];
extern std::size_t unit_case_count;
extern std::size_t unit_checks;
#define UNIT_CHECK(expr) do {++unit_checks; if(!(expr)){std::fprintf(stderr,"UNIT_ASSERT %s:%d %s\n",__FILE__,__LINE__,#expr);std::exit(42);}}while(0)
