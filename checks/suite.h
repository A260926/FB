#pragma once
#include <cstdio>
#include <cstdlib>
extern int unit_checks;
#define UNIT_CHECK(x) do { if (!(x)) { std::fprintf(stderr,"CHECK_FAIL %s:%d\n",__FILE__,__LINE__); return -1; } ++unit_checks; } while(0)
int unit_run_group(const char*);
