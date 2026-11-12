#pragma once
#include <vector>
#include <string>
#include <stdexcept>
#include <iostream>
namespace uf {
struct Test {const char* group;const char* name;void (*fn)();};
inline std::vector<Test>& tests(){static std::vector<Test> t;return t;}
inline unsigned& count(){static unsigned n=0;return n;}
struct Register {Register(const char*g,const char*n,void(*f)()){tests().push_back({g,n,f});}};
inline void check(bool b,const char* e,int line){++count();if(!b)throw std::runtime_error(std::string(e)+" line="+std::to_string(line));}
}
#define UNIT_CHECK(e) uf::check(bool(e),#e,__LINE__)
#define UNIT_TEST(g,n) static void n(); static uf::Register reg_##n(g,#n,n); static void n()
