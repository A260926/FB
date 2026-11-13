#include "qa.hpp"
#include <cstdio>
#include <set>
int main() {
    Registry r; register_intersections(r);
    const char *groups[]={"isolated","overlap","integration"};
    std::set<std::string> names;
    for(const auto &c:r.cases){
        if(c.group!="isolated" && c.group!="overlap" && c.group!="integration")return 2;
        if(c.name.empty() || !c.run || !names.insert(c.group+":"+c.name).second)return 2;
    }
    std::size_t total=0;
    for(const char *g:groups){
        std::size_t cases=0,checks=0;
        for(const auto &c:r.cases)if(c.group==g){
            Check ck; int rc;
            try {rc=c.run(ck);} catch(const std::exception &e){ std::fprintf(stderr,"FAIL %s/%s: %s\n",g,c.name.c_str(),e.what());return 1; }
            if(rc || !ck.count){std::fprintf(stderr,"FAIL skipped/empty %s/%s rc=%d\n",g,c.name.c_str(),rc);return 1;}
            ++cases; checks+=ck.count;
            std::printf("CASE %s/%s checks=%zu\n",g,c.name.c_str(),ck.count);
        }
        if(cases<2)return 2;
        std::printf("PASS %s cases=%zu checks=%zu\n",g,cases,checks);total+=cases;
    }
    std::printf("INTERSECTIONS_ALL_DONE cases=%zu\n",total);
    return 0;
}
