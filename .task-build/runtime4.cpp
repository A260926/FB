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
  fprintf(stderr,"about to count\n");
  size_t n=func.fixup_hooks_in.size();
  fprintf(stderr,"n=%zu\n",n);
  for(size_t k=0;k<n;++k){
    fprintf(stderr,"k=%zu\n",k);
    auto *p=func.fixup_hooks_in[k].target<void()>();
    fprintf(stderr,"  target=%p\n",(void*)p);
  }
  return 0;
}
