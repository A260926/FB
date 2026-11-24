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
  c.ir.for_each_typed_id<SPIRVariable>([&](uint32_t, SPIRVariable& var){
    if(var.initializer)
      fprintf(stderr,"var %u storage=%d init=%u\n",var.self,(int)var.storage,var.initializer);
  });
  return 0;
}
