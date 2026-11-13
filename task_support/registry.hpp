#pragma once
#include <functional>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstddef>
namespace task {
struct Case {
 size_t checks=0;
 void check(bool ok,const std::string& message){++checks;if(!ok)throw std::runtime_error(message);}
};
struct Entry {std::string group,name;std::function<void(Case&)> run;};
struct Registry {
 std::vector<Entry> entries;
 void add(std::string group,std::string name,std::function<void(Case&)> fn){
  if(name.empty()||!fn)throw std::runtime_error("empty registration");
  for(auto& e:entries)if(e.group==group&&e.name==name)throw std::runtime_error("duplicate case");
  entries.push_back({group,name,fn});
 }
};
}
void register_spatial_tests(task::Registry&);
