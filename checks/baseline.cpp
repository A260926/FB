#include <DSPatch.h>
#include <array>
#include <cassert>
#include <iostream>
using Block=std::array<float,8>;
struct Source:DSPatch::Component {int t=0;Source(){SetOutputCount_(1);}void Process_(DSPatch::SignalBus const&,DSPatch::SignalBus&o)override{Block b{};for(int i=0;i<8;i++)b[i]=float(t*8+i);o.SetValue(0,b);t++;}};
struct Sink:DSPatch::Component{int t=0;Sink(){SetInputCount_(1);}void Process_(DSPatch::SignalBus const&i,DSPatch::SignalBus&)override{auto p=i.GetValue<Block>(0);assert(p);for(int j=0;j<8;j++)assert((*p)[j]==float(t*8+j));t++;}};
int main(){for(auto m:{DSPatch::Component::TickMode::Series,DSPatch::Component::TickMode::Parallel}){DSPatch::Circuit c;auto s=std::make_shared<Source>();auto a=std::make_shared<Sink>();auto b=std::make_shared<Sink>();c.AddComponent(b);c.AddComponent(s);c.AddComponent(a);assert(c.ConnectOutToIn(s,0,a,0));assert(c.ConnectOutToIn(s,0,b,0));for(int i=0;i<64;i++)c.Tick(m);assert(a->t==64&&b->t==64);}std::cout<<"DSPATCH_BASELINE_OK modes=2 frames=1024\n";}
