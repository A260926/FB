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
  auto &v=func.fixup_hooks_in;
  for(size_t k=0;k<v.size();++k){
    fprintf(stderr,"call %zu\n",k); fflush(stderr);
    v[k]();
  }
  fprintf(stderr,"all hooks returned\n");
  return 0;
}
