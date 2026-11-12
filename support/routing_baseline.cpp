#include "routing_fixture.h"
#include <cassert>
#include <iostream>
int main(int argc,char** argv){
 assert(argc==2);
 for(bool files:{true,false}){
  devcheck::RoutingFixture f(std::string(argv[1])+(files?"/files":"/blocks"),files);
  for(bool sparse:{false,true}){
   if(sparse){
    for(unsigned i=0;i<512;++i)f.keys[i]=devcheck::RoutingKey(i*(files?2:16));
    f.slices.clear();for(auto& k:f.keys)f.slices.push_back(k);
   }
   f.Reset();f.PointReference();auto c=f.Count();
   assert(c.reads==512);assert(c.unique_reads==512);assert(c.comparisons>512*10);
   std::cout<<"LEGACY_ROUTE layout="<<(files?"files1024":"blocks8192")
    <<" shape="<<(sparse?"sparse":"dense")<<" keys=512 reads="<<c.reads<<" unique_reads="<<c.unique_reads<<" bytes="<<c.bytes<<" comparisons="<<c.comparisons<<"\n";
  }
 }
 std::cout<<"ROUTING_REFERENCE_4_PASS\n";
}
