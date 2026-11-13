#pragma once
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <set>
#include <string>
namespace uf { inline unsigned checks=0; struct Case {const char *name,*group; void (*run)();}; }
#define UNIT_CHECK(x) do { if(!(x)){std::fprintf(stderr,"CHECK FAILED %s:%d: %s\n",__FILE__,__LINE__,#x);std::exit(3);}++uf::checks;}while(0)
std::vector<uf::Case> unit_cases();
