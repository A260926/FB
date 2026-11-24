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
  // Count hooks per function WITHOUT calling them
  c.ir.for_each_typed_id<SPIRFunction>([&](uint32_t, SPIRFunction& fn){
    fprintf(stderr,"func %u hooks_in=%zu\n",fn.self,fn.fixup_hooks_in.size());
  });
  // Now find which variables have initializers among ALL variables
  for(auto vid : c.ir.ids_for_type[TypeVariable]){
    auto &var=c.get<SPIRVariable>(vid);
    if(var.initializer)
      fprintf(stderr,"var %u storage=%d init=%u\n",vid,(int)var.storage,var.initializer);
  }
  return 0;
}
