#pragma once
#include <cstddef>
#include <stdexcept>
#include <vector>
#include <string>
namespace task {
struct Case { const char* group; const char* name; void (*run)(); };
std::vector<Case>& cases();
struct Register { Register(const char* group,const char* name,void (*f)()) { cases().push_back({group,name,f}); } };
void check(bool value,const char* expression,int line);
}
#define TASK_CAT_(a,b) a##b
#define TASK_CAT(a,b) TASK_CAT_(a,b)
#define TASK_CASE(group,name) static void name(); static task::Register TASK_CAT(reg_,name)(group,#name,name); static void name()
#define TASK_CHECK(expr) task::check(bool(expr),#expr,__LINE__)
