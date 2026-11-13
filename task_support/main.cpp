#include "registry.hpp"
#include <exception>
int main(int argc,char**argv) {
 try {
  Registry r; register_task_tests(r); if(r.entries.empty()) throw std::runtime_error("zero registrations");
  for(const auto& e:r.entries) { bool known=false; for(int i=1;i<argc;++i) known |= e.group==argv[i]; if(!known) throw std::runtime_error("unknown group"); }
  for(int i=1;i<argc;++i) {
   unsigned cases=0,checks=0;
   for(auto& e:r.entries) if(e.group==argv[i]) { Case c; e.fn(c); if(!c.checks) throw std::runtime_error("zero checks / skipped case"); ++cases; checks+=c.checks; }
   if(!cases||!checks) throw std::runtime_error("missing group");
   std::printf("GROUP %s cases=%u checks=%u DONE\n",argv[i],cases,checks); std::fflush(stdout);
  }
  std::puts("TASK_ALL_DONE");return 0;
 } catch(const std::exception& e) { std::fprintf(stderr,"FAIL %s\n",e.what());return 1; }
}
