#include "fixture.hpp"
#include <iostream>
using namespace spirv_cross;
int main(){
  // Two independent fixtures share no module; instead verify repeated sample of
  // same pair yields one mapping, and distinct N/M yields distinct axes.
  for(auto cfg : {std::pair<unsigned,unsigned>{2,3},{3,2}}){
    auto f=pair_fixture::make(cfg.first,cfg.second,true,true,false);
    CompilerGLSL c(f.words); c.set_common_options(pair_fixture::options());
    c.build_combined_image_samplers();
    auto m=c.get_combined_image_samplers();
    std::cout<<"N="<<cfg.first<<" M="<<cfg.second<<" maps="<<m.size()
             <<" slots="<<c.get_combined_image_sampler_bindings(m[0].combined_id).size()<<"\n";
  }
  return 0;
}
