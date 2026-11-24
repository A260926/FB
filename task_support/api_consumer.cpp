#include "model/causal_trace.hpp"
#include "model/checker.hpp"
#include <stdexcept>
int main(){
 model::Action a;a.kind=model::ActionKind::Yield;
 model::ModelChecker c(model::Program{{{a,a}}});model::Schedule s{{0,0,{}},{0,1,{}}};
 auto t=c.causal_trace(s);
 if(t.cancelled||t.events.size()!=2||t.events[0].id!=0||t.events[1].id!=1)return 11;
 auto path=t.explain(0,1);if(path.empty())return 12;
 const auto& e=t.edges.at(path.front());if(e.from!=0||e.to!=1||e.reason.empty())return 13;
 auto prefix=c.causal_trace(s,1);if(!prefix.cancelled||prefix.events.size()!=1)return 14;
 return 0;
}
