#ifndef TASK_SUPPORT_QA_HPP
#define TASK_SUPPORT_QA_HPP
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>
struct Check {
    std::size_t count = 0;
    void require(bool b, const char *why) { ++count; if(!b)throw std::runtime_error(why); }
};
struct Case { std::string group,name; int (*run)(Check &); };
struct Registry {
    std::vector<Case> cases;
    void add(const char *group,const char *name,int (*run)(Check &)) { cases.push_back({group,name,run}); }
};
void register_intersections(Registry &);
#endif
