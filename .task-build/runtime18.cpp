#define protected public
#define private public
#include "spirv_glsl.hpp"
#include "fixture.hpp"
#include <cstdio>
using namespace spirv_cross;
int main(){
  auto f=pair_fixture::make(1,1,false,false);
  CompilerGLSL c(f.words);c.set_common_options(pair_fixture::options());
  size_t before=0;
  c.ir.for_each_typed_id<SPIRFunction>([&](uint32_t,SPIRFunction&fn){before+=fn.fixup_hooks_in.size();});
  fprintf(stderr,"hooks right after ctor=%zu\n",before);
  return 0;
}
