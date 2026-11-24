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
  // enumerate ALL functions and their local variables + static expressions
  c.ir.for_each_typed_id<SPIRFunction>([&](uint32_t, SPIRFunction& fn){
    fprintf(stderr,"func %u local=%zu\n",fn.self,fn.local_variables.size());
    for(auto vid: fn.local_variables){
      auto &var=c.get<SPIRVariable>(vid);
      fprintf(stderr,"  lv %u storage=%d init=%u static=%u remapped=%u\n",
              vid,(int)var.storage,var.initializer,var.static_expression,(unsigned)var.remapped_variable);
    }
  });
  return 0;
}
