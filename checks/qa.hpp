#pragma once
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
struct Check {
 int assertions=0;
 void require(bool ok,const char* message) { ++assertions; if(!ok) throw std::runtime_error(message); }
};
struct Case {std::string group,name; std::function<int(Check&)> fn;};
struct Registry {
 std::vector<Case> cases;
 void add(std::string group,std::string name,std::function<int(Check&)> fn) { cases.push_back({group,name,fn}); }
};
void register_contracts(Registry&);
