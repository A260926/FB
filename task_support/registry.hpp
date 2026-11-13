#pragma once
#include <vector>
#include <string>
#include <functional>
#include <stdexcept>
struct Case { int checks=0; void check(bool v,const char* m){++checks;if(!v)throw std::runtime_error(m);} };
struct Entry {std::string group,name;std::function<int(Case&)> fn;};
struct Registry {std::vector<Entry> entries;void add(const char*g,const char*n,std::function<int(Case&)> f){entries.push_back({g,n,f});}};
void register_task_tests(Registry&);
