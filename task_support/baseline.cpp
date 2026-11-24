#include "model/checker.hpp"
#include <iostream>
using namespace model;
Action make(ActionKind k){Action a;a.kind=k;return a;}
int main(){
 Action lock=make(ActionKind::Lock);lock.mutex="m";
 Action unlock=make(ActionKind::Unlock);unlock.mutex="m";
 Action write=make(ActionKind::Write);write.address="x";
 Action bar=make(ActionKind::BarrierWait);bar.barrier="phase";bar.parties=2;
 Action yield=make(ActionKind::Yield);
 Program p{{{lock,write,unlock,bar,yield,bar},{lock,write,unlock,bar,yield,bar}}};
 Schedule s;
 for(unsigned i=0;i<4;++i)s.push_back({0,i,{}});
 for(unsigned i=0;i<4;++i)s.push_back({1,i,{}});
 s.push_back({0,4,{}});s.push_back({0,5,{}});s.push_back({1,4,{}});s.push_back({1,5,{}});
 ModelChecker c(p);auto result=c.replay(s);auto trace=c.inspect_schedule(s);
 if(result.first_race||result.first_deadlock||result.first_error||result.first_assertion||trace.size()!=12)return 1;
 for(const auto& e:trace)if(!e.executed||e.clock_after.size()!=2)return 2;
 std::cout<<"DPOR_BASELINE committed=12 barrier_generations=2 mutex_safe=1\n";
}
