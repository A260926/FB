#pragma once
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
namespace anchor_test {
struct Case{std::string group,name;std::function<void()>body;};
struct Registry{std::vector<Case>cases;void add(const std::string&g,const std::string&n,std::function<void()>f){cases.push_back({g,n,f});}};
inline unsigned long long assertions=0;
inline void check(bool value,const char*text,int line){++assertions;if(!value)throw std::runtime_error(std::string(text)+":"+std::to_string(line));}
}
#define ANCHOR_CHECK(expr) ::anchor_test::check(bool(expr),#expr,__LINE__)
void register_time_anchor_tests(anchor_test::Registry&);
