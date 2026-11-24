#include "fixture.hpp"
#include <fstream>
#include <iostream>
int main(){
  auto f=pair_fixture::make(2,3,true,true,false);
  spirv_cross::CompilerGLSL c(f.words);c.set_common_options(pair_fixture::options());
  c.build_combined_image_samplers();
  auto id=c.get_combined_image_samplers()[0].combined_id;
  c.set_name(id,"paired");c.set_decoration(id,spv::DecorationBinding,7);
  std::ofstream(".task-build/direct.glsl")<<c.compile();
  std::ofstream s(".task-build/direct.slots");
  for(auto& b:c.get_combined_image_sampler_bindings(id))
    s<<"7 "<<b.combined_index<<" "<<b.image_index<<" "<<b.sampler_index<<"\n";
  return 0;
}
