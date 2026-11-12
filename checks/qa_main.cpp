#include "qa.hpp"
#include <iostream>
#include <set>
int main(){
 Registry r; register_contracts(r);
 const std::vector<std::string> groups={"analytic-paths","knot-events","termination"};
 std::set<std::string> names;
 try {
 for(const auto& c:r.cases) {
   if(c.name.empty()) throw std::runtime_error("empty case name");
   bool valid=false; for(const auto& g:groups) valid=valid||g==c.group;
   if(!valid) throw std::runtime_error("unknown group: "+c.group);
 }
 for(const auto& group:groups){
 int count=0,total=0;
 for(const auto& c:r.cases) if(c.group==group){
 if(!names.insert(c.group+"/"+c.name).second) throw std::runtime_error("duplicate case");
 Check t; int rc=c.fn(t);
 if(rc!=0 || t.assertions==0) throw std::runtime_error("skipped, failed, or empty case: "+c.name);
 ++count; total+=t.assertions;
 }
 if(count==0) throw std::runtime_error("missing group: "+group);
 std::cout<<"PASS "<<group<<" cases="<<count<<" assertions="<<total<<"\n";
 }
 std::cout<<"LNLIB_GEODESIC_GROUPS_COMPLETE\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
 return 0;
}
