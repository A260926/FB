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
  std::function<void()> &first = v[0];
  fprintf(stderr,"calling first\n"); fflush(stderr);
  first();
  fprintf(stderr,"returned\n");
  return 0;
}
