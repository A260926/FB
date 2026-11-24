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
  auto &func = c.get<SPIRFunction>(c.get_entry_point().self);
  fprintf(stderr,"main fixup_hooks_in=%zu fixup_hooks_out=%zu\n",
          func.fixup_hooks_in.size(), func.fixup_hooks_out.size());
  // iterate all functions
  c.ir.for_each_typed_id<SPIRFunction>([&](uint32_t, SPIRFunction& fn){
    fprintf(stderr,"func %u hooks_in=%zu hooks_out=%zu local_vars=%zu\n",
            fn.self, fn.fixup_hooks_in.size(), fn.fixup_hooks_out.size(), fn.local_variables.size());
  });
  return 0;
}
