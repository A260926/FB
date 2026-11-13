#pragma once
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
struct TaskCase {const char* name; void (*fn)();};
inline std::vector<TaskCase>& task_cases(){static std::vector<TaskCase> v;return v;}
inline unsigned& task_checks(){static unsigned n=0;return n;}
struct TaskRegister {TaskRegister(const char* n,void(*f)()){task_cases().push_back({n,f});}};
#define CASE(n) static void n(); static TaskRegister reg_##n(#n,n); static void n()
#define CHECK(x) do {++task_checks();if(!(x)){std::fprintf(stderr,"CHECK FAILED %s:%d: %s\n",__FILE__,__LINE__,#x);std::exit(2);}}while(0)
