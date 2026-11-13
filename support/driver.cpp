#include "contract.hpp"
#include <cstring>
#include <set>
#include <string>
std::size_t unit_checks=0;
int main(){
 const char *groups[]={"index","pixels","budget"};
 if(unit_case_count<6)return 2;
 std::set<std::string> names;
 for(std::size_t i=0;i<unit_case_count;i++){
  const unit_case &c=unit_cases[i];
  if(!c.name||!c.group||!c.run||!c.name[0]||std::strpbrk(c.name," \t\r\n")||!names.insert(c.name).second)return 3;
  if(std::strcmp(c.group,groups[0])&&std::strcmp(c.group,groups[1])&&std::strcmp(c.group,groups[2]))return 4;
 }
 std::size_t total=0;
 for(const char *g:groups){
  std::size_t n=0,start=unit_checks;
  std::printf("GROUP_BEGIN %s\n",g);std::fflush(stdout);
  for(std::size_t i=0;i<unit_case_count;i++)if(!std::strcmp(unit_cases[i].group,g)){
   std::size_t before=unit_checks;unit_cases[i].run();
   if(unit_checks==before)return 5;
   std::printf("CASE %s %s %zu\n",g,unit_cases[i].name,unit_checks-before);std::fflush(stdout);++n;++total;
  }
  if(n<2)return 6;
  std::printf("GROUP_END %s %zu %zu\n",g,n,unit_checks-start);std::fflush(stdout);
 }
 if(total!=unit_case_count)return 7;
 std::printf("TARGET_COMPLETE cases=%zu checks=%zu\n",total,unit_checks);
 return 0;
}
