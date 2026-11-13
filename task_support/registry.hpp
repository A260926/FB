#pragma once
#include <vector>
#include <string>
#include <functional>
#include <stdexcept>
#include <cstdio>
struct Case { unsigned checks=0; void check(bool ok,const char* msg="content assertion") { ++checks; if(!ok) throw std::runtime_error(msg); } };
struct Entry { std::string group,name; std::function<void(Case&)> fn; };
struct Registry { std::vector<Entry> entries; void add(const char* group,const char* name,std::function<void(Case&)> fn) { entries.push_back({group,name,fn}); } };
void register_task_tests(Registry&);
