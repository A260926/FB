#include "suite.h"
#include "work_trace.h"
#include <cstdio>
#include <cstring>
#include <exception>
namespace {
size_t checks=0, eventCount=0;
task::WorkEvent eventBuffer[1048576];
}
namespace task {
std::vector<Case>& cases() { static std::vector<Case> c; return c; }
void check(bool value,const char* expression,int line) {
  ++checks;
  if(!value) throw std::runtime_error(std::string("line=")+std::to_string(line)+" "+expression);
}
void clear_work() { eventCount=0; }
size_t work_count() { return eventCount; }
WorkEvent work_event(size_t i) { if(i>=eventCount) throw std::out_of_range("work event");return eventBuffer[i]; }
}
void task_tail_work(std::uint64_t q,const char* kind,size_t units) {
  if(eventCount>=1048576) throw std::runtime_error("trace capacity");
  eventBuffer[eventCount++]={q,kind,units};
}
int main() {
  const char* groups[]={"pcm","schedule","lifecycle"};
  size_t total=0;
  try {
    for(auto group:groups) {
      size_t count=0, groupChecks=0;
      for(const auto& c:task::cases()) if(!std::strcmp(c.group,group)) {
        checks=0;c.run();
        if(checks==0) { std::fprintf(stderr,"zero checks: %s/%s\n",group,c.name);return 2; }
        std::printf("CASE %s/%s checks=%zu PASS\n",group,c.name,checks);std::fflush(stdout);
        groupChecks+=checks;++count;++total;
      }
      if(count<2) { std::fprintf(stderr,"need two cases: %s\n",group);return 3; }
      std::printf("GROUP %s cases=%zu checks=%zu COMPLETE\n",group,count,groupChecks);std::fflush(stdout);
    }
    if(total!=task::cases().size()) {std::fprintf(stderr,"unknown group\n");return 4;}
    std::printf("TARGET_COMPLETE cases=%zu\n",total);
  } catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 5;}
}
