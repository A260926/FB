#define protected public
#define private public
#include "spirv_glsl.hpp"
#include "fixture.hpp"
#include <cstdio>
using namespace spirv_cross;
int main(){
  auto f=pair_fixture::make(1,1,false,false);
  CompilerGLSL c(f.words);c.set_common_options(pair_fixture::options());
  c.build_combined_image_samplers();
  // Check resources emitted before hooks: simulate emit_resources partially
  auto sh=c.get_shader_resources();
  for(auto&r:sh.uniform_buffers) fprintf(stderr,"ub %u name=%s\n",r.id, c.get_name(r.id).c_str());
  for(auto&r:sh.sampled_images) fprintf(stderr,"si %u name=%s\n",r.id, c.get_name(r.id).c_str());
  for(auto&r:sh.storage_buffers) fprintf(stderr,"sb %u\n",r.id);
  return 0;
}
