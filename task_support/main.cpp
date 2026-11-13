#include "registry.hpp"
#include <iostream>
#include <set>
int main(int argc,char**argv){try {Registry r;register_task_tests(r);std::set<std::string> expected;for(int i=1;i<argc;++i)expected.insert(argv[i]);for(auto&e:r.entries)if(!expected.count(e.group))throw std::runtime_error("unknown group");for(int i=1;i<argc;++i){int n=0,c=0;for(auto&e:r.entries)if(e.group==argv[i]){Case q;int rc=e.fn(q);if(rc||!q.checks)throw std::runtime_error("skipped/failed/empty callback");++n;c+=q.checks;std::cout<<"case "<<e.group<<"/"<<e.name<<" checks="<<q.checks<<"\n";}if(!n)throw std::runtime_error("zero cases");std::cout<<"PASS "<<argv[i]<<" cases="<<n<<" checks="<<c<<"\n";}std::cout<<"TARGET_COMPLETE\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
