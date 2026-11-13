#include "test_api.h"
#include <cstdio>
#include <set>
int main(){try{
 anchor_test::Registry r;register_time_anchor_tests(r);std::set<std::string>names;
 for(const auto&c:r.cases){if(c.group!="islands"&&c.group!="stretch"&&c.group!="stream")throw std::runtime_error("unknown group");if(c.name.empty()||!names.insert(c.group+"/"+c.name).second)throw std::runtime_error("empty/duplicate case");}
 for(const char*g:{"islands","stretch","stream"}){
  size_t tests=0;unsigned long long count=0;
  for(const auto&c:r.cases)if(c.group==g){auto start=anchor_test::assertions;c.body();auto n=anchor_test::assertions-start;if(!n)throw std::runtime_error("zero assertions "+c.name);++tests;count+=n;std::printf("ANCHOR_CASE %s/%s checks=%llu DONE\n",g,c.name.c_str(),n);}
  if(!tests)throw std::runtime_error(std::string("zero cases ")+g);
  std::printf("ANCHOR_GROUP %s tests=%zu checks=%llu DONE\n",g,tests,count);
 }
 std::puts("ANCHOR_SUITE_DONE groups=3");
}catch(const std::exception&e){std::fprintf(stderr,"anchor tests: %s\n",e.what());return 1;}}
