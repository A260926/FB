#pragma once
#include <vector>
#include <string>
#include <stdexcept>
struct Test { const char* name; void(*fn)(); };
inline std::vector<Test>& cases(){static std::vector<Test> v;return v;}
inline int& assertions(){static int n=0;return n;}
struct Register {Register(const char*n,void(*f)()){cases().push_back({n,f});}};
#define CASE(n) static void n(); static Register reg_##n(#n,n); static void n()
#define CHECK(x) do{++assertions();if(!(x))throw std::runtime_error(#x);}while(0)
