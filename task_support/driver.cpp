#include "registry.hpp"
#include <iostream>
#include <map>
#include <set>
int main() {
    using namespace ordered_test;
    try {
        Registry reg; register_ordered_tests(reg);
        const std::vector<std::string> groups={"order","instances","frontier","render"};
        std::set<std::string> names;
        std::map<std::string,std::pair<unsigned,std::uint64_t>> totals;
        for (const auto& e:reg.entries) {
            bool known=false; for (const auto& g:groups) if(e.group==g) known=true;
            if(!known || e.name.empty() || e.name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string::npos || !e.run || !names.insert(e.name).second)
                throw std::runtime_error("invalid/duplicate test registration");
        }
        for(const auto& g:groups) {
            for(const auto& e:reg.entries) if(e.group==g) {
                Case c; e.run(c);
                if(!c.checks) throw std::runtime_error("test made no checks: "+e.name);
                auto& t=totals[g]; ++t.first; t.second+=c.checks;
                std::cout<<"ORDERED_CASE "<<g<<" "<<e.name<<" "<<c.checks<<std::endl;
            }
            const auto& t=totals[g];
            if(t.first<2) throw std::runtime_error("need two named cases: "+g);
            std::cout<<"ORDERED_GROUP "<<g<<" "<<t.first<<" "<<t.second<<std::endl;
        }
        std::cout<<"ORDERED_COMPLETE "<<reg.entries.size()<<std::endl;
    } catch(const std::exception& e) { std::cerr<<"ORDERED_FAIL "<<e.what()<<std::endl; return 1; }
}
