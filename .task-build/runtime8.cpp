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
  fprintf(stderr,"local_variables=%zu\n",func.local_variables.size());
  for(auto vid : func.local_variables){
    auto &var=c.get<SPIRVariable>(vid);
    fprintf(stderr,"var %u storage=%d initializer=%u static_expr=%u\n",
            vid,(int)var.storage,var.initializer,var.static_expression);
  }
  return 0;
}
