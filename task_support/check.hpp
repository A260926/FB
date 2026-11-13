#pragma once
#include <cstdio>
#include <cstdlib>
#include <vector>

struct TaskCase { const char* name; void (*run)(); };
inline std::vector<TaskCase>& taskCases() { static std::vector<TaskCase> v; return v; }
inline unsigned long& taskChecks() { static unsigned long n = 0; return n; }
struct TaskRegister {
    TaskRegister(const char* name, void (*run)()) { taskCases().push_back({name, run}); }
};
#define CASE(name) static void name(); static TaskRegister reg_##name(#name, name); static void name()
#define CHECK(expr) do { ++taskChecks(); if (!(expr)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr); std::exit(2); } } while (0)
