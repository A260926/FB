#define protected public
#define private public
#include "spirv_glsl.hpp"
#include "fixture.hpp"
#include <cstdio>
using namespace spirv_cross;
int main(){
  // Construct from raw words but do NOT parse via CompilerGLSL ctor path differently;
  // instead parse then immediately check.
  auto words = pair_fixture::make(1,1,false,false).words;
  CompilerGLSL c(words);
  size_t h=0;
  c.ir.for_each_typed_id<SPIRFunction>([&](uint32_t,SPIRFunction&fn){
    fprintf(stderr,"func hooks_in=%zu hooks_out=%zu\n",fn.fixup_hooks_in.size(),fn.fixup_hooks_out.size());
    h+=fn.fixup_hooks_in.size();
  });
  fprintf(stderr,"TOTAL hooks=%zu\n",h);
  return 0;
}
