#define protected public
#define private public
#include "spirv_glsl.hpp"
#include "fixture.hpp"
#include <cstdio>
#include <cstddef>
using namespace spirv_cross;
int main(){
  auto f=pair_fixture::make(1,1,false,false);
  CompilerGLSL c(f.words);c.set_common_options(pair_fixture::options());
  c.build_combined_image_samplers();
  auto &func = c.get<SPIRFunction>(c.get_entry_point().self);
  fprintf(stderr,"func addr=%p size=%zu\n",(void*)&func,sizeof(func));
  fprintf(stderr,"fixup_hooks_in ptr=%p size=%zu\n",(void*)func.fixup_hooks_in.ptr,func.fixup_hooks_in.buffer_size);
  fprintf(stderr,"  stack addr=%p inline=%d\n",(void*)func.fixup_hooks_in.stack_storage.data(),
          int(func.fixup_hooks_in.ptr==func.fixup_hooks_in.stack_storage.data()));
  return 0;
}
