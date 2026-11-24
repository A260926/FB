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
  fprintf(stderr,"global_variables=%zu\n",c.global_variables.size());
  for(auto vid : c.global_variables){
    auto &var=c.get<SPIRVariable>(vid);
    fprintf(stderr,"gvar %u storage=%d init=%u\n",vid,(int)var.storage,var.initializer);
  }
  return 0;
}
