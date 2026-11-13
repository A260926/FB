#pragma once
#include <cstdlib>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace uf {
struct Case { std::string group, name; void (*run)(); };
inline std::vector<Case>& cases() { static std::vector<Case> c; return c; }
inline unsigned long long& checks() { static unsigned long long c=0; return c; }
struct Register {
  Register(const char* g,const char* n,void (*f)()) { cases().push_back({g,n,f}); }
};
inline void check(bool value,const char* expression,const char* file,int line) {
  ++checks();
  if (!value) { std::ostringstream o; o<<file<<":"<<line<<" assertion "<<expression; throw std::runtime_error(o.str()); }
}
}
#define UNIT_CASE(group,name) static void unit_case_##name(); \
 static uf::Register unit_reg_##name(#group,#name,unit_case_##name); \
 static void unit_case_##name()
#define UNIT_CHECK(expr) ::uf::check(bool(expr),#expr,__FILE__,__LINE__)
