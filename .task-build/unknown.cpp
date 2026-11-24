#include "fixture.hpp"
#include <iostream>
int main(){
  auto f=pair_fixture::make(2,3,true,true);
  spirv_cross::CompilerGLSL c(f.words);c.set_common_options(pair_fixture::options());
  c.build_combined_image_samplers();
  bool threw=false;
  try { (void)c.get_combined_image_sampler_bindings(9999); }
  catch(const spirv_cross::CompilerError&){ threw=true; }
  std::cout<<"unknown id threw="<<threw<<"\n";
  // query constness / no mutation: call twice
  auto id=c.get_combined_image_samplers()[0].combined_id;
  auto a=c.get_combined_image_sampler_bindings(id);
  auto b=c.get_combined_image_sampler_bindings(id);
  std::cout<<"repeat equal="<<(a.size()==b.size())<<" size="<<a.size()<<"\n";
  return 0;
}
