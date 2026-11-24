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
  fprintf(stderr,"size=%zu ptr=%p\n",v.size(),(void*)v.ptr);
  std::function<void()> &first = v[0];
  fprintf(stderr,"got ref sizeof=%zu\n",sizeof(first));
  bool empty = first.operator bool();
  fprintf(stderr,"bool=%d\n",(int)empty);
  return 0;
}
