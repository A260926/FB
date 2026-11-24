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
  for(size_t k=0;k<func.fixup_hooks_in.size();++k){
    auto &fn=func.fixup_hooks_in[k];
    printf("hook %zu target null? %d\n", k, (int)(fn.template target<void()>()==nullptr));
  }
  return 0;
}
